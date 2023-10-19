/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xthread.h"

#include <cstring>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/ppc/ppc_decode_data.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xmutant.h"
DEFINE_bool(ignore_thread_priorities, true,
            "Ignores game-specified thread priorities.", "Kernel");
DEFINE_bool(ignore_thread_affinities, true,
            "Ignores game-specified thread affinities.", "Kernel");

namespace xe {
namespace kernel {

X_KPCR* GetKPCR() { return GetKPCR(cpu::ThreadState::Get()->context()); }
X_KPCR* GetKPCR(PPCContext* context) {
  return context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
}

X_KTHREAD* GetKThread() {
  return GetKThread(cpu::ThreadState::Get()->context());
}
X_KTHREAD* GetKThread(PPCContext* context) {
  return context->TranslateVirtual(GetKPCR(context)->prcb_data.current_thread);
}
const uint32_t XAPC::kSize;
const uint32_t XAPC::kDummyKernelRoutine;
const uint32_t XAPC::kDummyRundownRoutine;

using xe::cpu::ppc::PPCOpcode;

using namespace xe::literals;

uint32_t next_xthread_id_ = 0;

XThread::XThread(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType), guest_thread_(true) {}

XThread::XThread(KernelState* kernel_state, uint32_t stack_size,
                 uint32_t xapi_thread_startup, uint32_t start_address,
                 uint32_t start_context, uint32_t creation_flags,
                 bool guest_thread, bool main_thread, uint32_t guest_process)
    : XObject(kernel_state, kObjectType, !guest_thread),
      guest_thread_(guest_thread),
      main_thread_(main_thread) {
  creation_params_.stack_size = stack_size;
  creation_params_.xapi_thread_startup = xapi_thread_startup;
  creation_params_.start_address = start_address;
  creation_params_.start_context = start_context;

  // top 8 bits = processor ID (or 0 for default)
  // bit 0 = 1 to create suspended
  creation_params_.creation_flags = creation_flags;

  // Adjust stack size - min of 16k.
  if (creation_params_.stack_size < 16 * 1024) {
    creation_params_.stack_size = 16 * 1024;
  }
  creation_params_.guest_process = guest_process;
  // The kernel does not take a reference. We must unregister in the dtor.
  kernel_state_->RegisterThread(this);

  // Allocate thread state block from heap.
  // https://web.archive.org/web/20170704035330/https://www.microsoft.com/msj/archive/S2CE.aspx
  // This is set as r13 for user code and some special inlined Win32 calls
  // (like GetLastError/etc) will poke it directly.
  // We try to use it as our primary store of data just to keep things all
  // consistent.
  // 0x000: pointer to tls data
  // 0x100: pointer to TEB(?)
  // 0x10C: Current CPU(?)
  // 0x150: if >0 then error states don't get set (DPC active bool?)
  // TEB:
  // 0x14C: thread id
  // 0x160: last error
  // So, at offset 0x100 we have a 4b pointer to offset 200, then have the
  // structure.
  // pcr_address_ = memory()->SystemHeapAlloc(0x2D8);
  // if (!pcr_address_) {

  //}

  // Allocate processor thread state.
  // This is thread safe.
  thread_state_ = new cpu::ThreadState(
      kernel_state->processor(), this->handle(), stack_base_, pcr_address_);
  XELOGI("XThread{:08X} ({:X}) Stack: {:08X}-{:08X}", handle(), handle(),
         stack_limit_, stack_base_);

  // Exports use this to get the kernel.
  thread_state_->context()->kernel_state = kernel_state_;
}

XThread::~XThread() {
  // Unregister first to prevent lookups while deleting.
  kernel_state_->UnregisterThread(this);

  // Notify processor of our impending destruction.
  emulator()->processor()->OnThreadDestroyed(thread_id());

  fiber_.reset();

  if (thread_state_) {
    delete thread_state_;
  }
  kernel_state()->memory()->SystemHeapFree(tls_static_address_);
  // kernel_state()->memory()->SystemHeapFree(pcr_address_);
  FreeStack();
}

bool XThread::IsInThread() {
  //  return Thread::IsInThread();
  xe::FatalError("unimpl");
  return false;
}

static threading::TlsHandle g_current_xthread_fls =
    threading::kInvalidTlsHandle;

struct handle_initializer_t {
  handle_initializer_t() {
    g_current_xthread_fls = threading::AllocateFlsHandle();
  }
  ~handle_initializer_t() { threading::FreeFlsHandle(g_current_xthread_fls); }
} handle_initializer;

static XThread* GetFlsXThread() {
  return reinterpret_cast<XThread*>(
      threading::GetFlsValue(g_current_xthread_fls));
}

bool XThread::IsInThread(XThread* other) { return GetFlsXThread() == other; }

XThread* XThread::GetCurrentThread() {
  XThread* thread = GetFlsXThread();
  if (!thread) {
    assert_always("Attempting to use guest stuff from a non-guest thread.");
  }
  return thread;
}

uint32_t XThread::GetCurrentThreadHandle() {
  XThread* thread = XThread::GetCurrentThread();
  return thread->handle();
}

uint32_t XThread::GetCurrentThreadId() {
  XThread* thread = XThread::GetCurrentThread();
  return thread->guest_object<X_KTHREAD>()->thread_id;
}

uint32_t XThread::GetLastError() {
  XThread* thread = XThread::GetCurrentThread();
  return thread->last_error();
}

void XThread::SetLastError(uint32_t error_code) {
  XThread* thread = XThread::GetCurrentThread();
  thread->set_last_error(error_code);
}

uint32_t XThread::last_error() { return guest_object<X_KTHREAD>()->last_error; }

void XThread::set_last_error(uint32_t error_code) {
  guest_object<X_KTHREAD>()->last_error = error_code;
}

void XThread::set_name(const std::string_view name) {}

static uint8_t next_cpu = 0;
static uint8_t GetFakeCpuNumber(uint8_t proc_mask) {
  // NOTE: proc_mask is logical processors, not physical processors or cores.
  if (!proc_mask) {
    auto current_thread = cpu::ThreadState::Get();
    if (!current_thread) {
      return 0;
    } else {
      auto context = current_thread->context();
      X_KPCR* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
      return kpcr->prcb_data.current_cpu;
    }
  }
  assert_false(proc_mask & 0xC0);

  uint8_t cpu_number = 7 - xe::lzcnt(proc_mask);
  assert_true(1 << cpu_number == proc_mask);
  assert_true(cpu_number < 6);
  return cpu_number;
}

void XThread::InitializeGuestObject() {
  /*
   * not doing this right at all! we're not using our threads context, because
   * we may be on the host and have no underlying context. in reality we should
   * have a context and acquire any locks using that context!
   */
  auto context_here = thread_state_->context();
  auto guest_thread = guest_object<X_KTHREAD>();
  auto thread_guest_ptr = guest_object();
  guest_thread->header.type = 6;
  guest_thread->suspend_count =
      (creation_params_.creation_flags & X_CREATE_SUSPENDED) ? 1 : 0;
  util::XeInitializeListHead(&guest_thread->mutants_list, memory());
  uint32_t process_info_block_address =
      creation_params_.guest_process ? creation_params_.guest_process
                                     : this->kernel_state_->GetTitleProcess();

  X_KPROCESS* process =
      memory()->TranslateVirtual<X_KPROCESS*>(process_info_block_address);
  auto process_type = process->process_type;


  xboxkrnl::xeKeInitializeTimerEx(&guest_thread->wait_timeout_timer, 0,
                                  process_type, context_here);

  guest_thread->wait_timeout_block.object = memory()
                                                ->HostToGuestVirtual(&guest_thread->wait_timeout_timer);
  guest_thread->wait_timeout_block.wait_type = 1;
  guest_thread->wait_timeout_block.thread = thread_guest_ptr;

  auto timer_wait_header_list_entry = memory()->HostToGuestVirtual(
      &guest_thread->wait_timeout_timer.header.wait_list);
  
  guest_thread->wait_timeout_block.wait_list_entry.blink_ptr =
      timer_wait_header_list_entry;
  guest_thread->wait_timeout_block.wait_list_entry.flink_ptr =
      timer_wait_header_list_entry;
  guest_thread->wait_timeout_block.wait_key = 258;
  guest_thread->stack_base = (this->stack_base_);
  guest_thread->stack_limit = (this->stack_limit_);
  guest_thread->stack_kernel = (this->stack_base_ - 240);
  guest_thread->tls_address = (this->tls_static_address_);
  guest_thread->thread_state = 0;

  uint32_t kpcrb = pcr_address_ + offsetof(X_KPCR, prcb_data);

  
  guest_thread->process_type_dup = process_type;
  guest_thread->process_type = process_type;
  guest_thread->apc_lists[0].Initialize(memory());
  guest_thread->apc_lists[1].Initialize(memory());

  guest_thread->a_prcb_ptr = kpcrb;
  guest_thread->another_prcb_ptr = kpcrb;

  guest_thread->may_queue_apcs = 1;
  guest_thread->msr_mask = 0xFDFFD7FF;
  guest_thread->process = process_info_block_address;
  guest_thread->stack_alloc_base = this->stack_base_;
  guest_thread->create_time = Clock::QueryGuestSystemTime();
  guest_thread->unk_144 = thread_guest_ptr + 324;
  guest_thread->unk_148 = thread_guest_ptr + 324;
  guest_thread->thread_id = this->handle();
  guest_thread->start_address = this->creation_params_.start_address;
  guest_thread->unk_154 = thread_guest_ptr + 340;
  uint32_t v9 = thread_guest_ptr;
  guest_thread->last_error = 0;
  guest_thread->unk_158 = v9 + 340;
  guest_thread->creation_flags = this->creation_params_.creation_flags;
  guest_thread->unk_17C = 1;

  
  auto old_irql = xboxkrnl::xeKeKfAcquireSpinLock(
      context_here, &process->thread_list_spinlock);

  // todo: acquire dispatcher lock here?

  util::XeInsertTailList(&process->thread_list, &guest_thread->process_threads,
                         context_here);
  process->thread_count += 1;
  // todo: release dispatcher lock here?
  xboxkrnl::xeKeKfReleaseSpinLock(context_here, &process->thread_list_spinlock,
                                  old_irql);
}

bool XThread::AllocateStack(uint32_t size) {
  auto heap = memory()->LookupHeap(kStackAddressRangeBegin);

  auto alignment = heap->page_size();
  auto padding = heap->page_size() * 2;  // Guard page size * 2
  size = xe::round_up(size, alignment);
  auto actual_size = size + padding;

  uint32_t address = 0;
  if (!heap->AllocRange(
          kStackAddressRangeBegin, kStackAddressRangeEnd, actual_size,
          alignment, kMemoryAllocationReserve | kMemoryAllocationCommit,
          kMemoryProtectRead | kMemoryProtectWrite, false, &address)) {
    return false;
  }

  stack_alloc_base_ = address;
  stack_alloc_size_ = actual_size;
  stack_limit_ = address + (padding / 2);
  stack_base_ = stack_limit_ + size;

  // Initialize the stack with junk
  memory()->Fill(stack_alloc_base_, actual_size, 0xBE);

  // Setup the guard pages
  heap->Protect(stack_alloc_base_, padding / 2, kMemoryProtectNoAccess);
  heap->Protect(stack_base_, padding / 2, kMemoryProtectNoAccess);
  thread_state_->context()->r[1] = stack_base_;
  return true;
}

void XThread::FreeStack() {
  if (stack_alloc_base_) {
    auto heap = memory()->LookupHeap(kStackAddressRangeBegin);
    uint32_t region_size = 0;
    heap->Release(stack_alloc_base_, &region_size);
    xenia_assert(region_size);
    kernel_state()->object_table()->FlushGuestToHostMapping(stack_alloc_base_,
                                                            region_size);

    stack_alloc_base_ = 0;
    stack_alloc_size_ = 0;
    stack_base_ = 0;
    stack_limit_ = 0;
  }
}

X_STATUS XThread::Create() {
  // Thread kernel object.
  if (!CreateNative<X_KTHREAD>()) {
    XELOGW("Unable to allocate thread object");
    return X_STATUS_NO_MEMORY;
  }

  // Allocate a stack.
  if (!AllocateStack(creation_params_.stack_size)) {
    return X_STATUS_NO_MEMORY;
  }

  // Allocate TLS block.
  // Games will specify a certain number of 4b slots that each thread will get.
  xex2_opt_tls_info* tls_header = nullptr;
  auto module = kernel_state()->GetExecutableModule();
  if (module) {
    module->GetOptHeader(XEX_HEADER_TLS_INFO, &tls_header);
  }

  const uint32_t kDefaultTlsSlotCount = 1024;
  uint32_t tls_slots = kDefaultTlsSlotCount;
  uint32_t tls_extended_size = 0;
  if (tls_header && tls_header->slot_count) {
    tls_slots = tls_header->slot_count;
    tls_extended_size = tls_header->data_size;
  }

  // Allocate both the slots and the extended data.
  // Some TLS is compiled with the binary (declspec(thread)) vars. The game
  // will directly access those through 0(r13).
  uint32_t tls_slot_size = tls_slots * 4;
  tls_total_size_ = tls_slot_size + tls_extended_size;
  tls_static_address_ = memory()->SystemHeapAlloc(tls_total_size_);
  tls_dynamic_address_ = tls_static_address_ + tls_extended_size;
  if (!tls_static_address_) {
    XELOGW("Unable to allocate thread local storage block");
    return X_STATUS_NO_MEMORY;
  }

  // Zero all of TLS.
  memory()->Fill(tls_static_address_, tls_total_size_, 0);
  if (tls_extended_size) {
    // If game has extended data, copy in the default values.
    assert_not_zero(tls_header->raw_data_address);
    memory()->Copy(tls_static_address_, tls_header->raw_data_address,
                   tls_header->raw_data_size);
  }

  uint8_t cpu_index = GetFakeCpuNumber(
      static_cast<uint8_t>(creation_params_.creation_flags >> 24));

  // Assign the newly created thread to the logical processor, and also set up
  // the current CPU in KPCR and KTHREAD.
  SetActiveCpu(cpu_index, true);
  // Initialize the KTHREAD object.
  InitializeGuestObject();

  X_KPCR* pcr = memory()->TranslateVirtual<X_KPCR*>(pcr_address_);

  pcr->tls_ptr = tls_static_address_;
  pcr->pcr_ptr = pcr_address_;
  pcr->prcb_data.current_thread = guest_object();
  pcr->prcb = pcr_address_ + offsetof(X_KPCR, prcb_data);
  pcr->host_stash = reinterpret_cast<uint64_t>(thread_state_->context());
  pcr->stack_base_ptr = stack_base_;
  pcr->stack_end_ptr = stack_limit_;

  pcr->prcb_data.dpc_active = 0;  // DPC active bool?

  // Always retain when starting - the thread owns itself until exited.
  RetainHandle();

  xe::threading::Fiber::CreationParameters params;

  params.stack_size = 16_MiB;  // Allocate a big host stack.
  fiber_ = xe::threading::Fiber::Create(params, [this]() {
    // Execute user code.
    threading::SetFlsValue(g_current_xthread_fls, (uintptr_t)this);

    cpu::ThreadState::Bind(thread_state_);
    running_ = true;
    Execute();
    running_ = false;

    xe::Profiler::ThreadExit();

    // Release the self-reference to the thread.
    ReleaseHandle();
  });

  if (!fiber_) {
    // TODO(benvanik): translate error?
    XELOGE("CreateThread failed");
    return X_STATUS_NO_MEMORY;
  }

  // Notify processor of our creation.
  // emulator()->processor()->OnThreadCreated(handle(), thread_state_, this);
  runnable_entry_.fiber_ = fiber_.get();
  runnable_entry_.thread_state_ = thread_state_;

  if ((creation_params_.creation_flags & X_CREATE_SUSPENDED) == 0) {
    // Start the thread now that we're all setup.

    // thread_->Resume();

    Schedule();
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XThread::Exit(int exit_code) {
  // This may only be called on the thread itself.
  assert_true(XThread::GetCurrentThread() == this);
  // TODO(chrispy): not sure if this order is correct, should it come after
  // apcs?
  auto kthread = guest_object<X_KTHREAD>();
  auto cpu_context = thread_state_->context();
  kthread->terminated = 1;

  // TODO(benvanik): dispatch events? waiters? etc?
  RundownAPCs();

  // Set exit code.
  kthread->header.signal_state = 1;
  kthread->exit_status = exit_code;

  auto kprocess = cpu_context->TranslateVirtual(kthread->process);

  uint32_t old_irql = xboxkrnl::xeKeKfAcquireSpinLock(
      cpu_context, &kprocess->thread_list_spinlock);

  util::XeRemoveEntryList(&kthread->process_threads, cpu_context);

  kprocess->thread_count = kprocess->thread_count - 1;

  xboxkrnl::xeKeKfReleaseSpinLock(cpu_context, &kprocess->thread_list_spinlock,
                                  old_irql);

  kernel_state()->OnThreadExit(this);

  // Notify processor of our exit.
  emulator()->processor()->OnThreadExit(thread_id());

  // NOTE: unless PlatformExit fails, expect it to never return!

  running_ = false;
  ReleaseHandle();

  xe::FatalError("Brokey!");
  // NOTE: this does not return!
  xe::threading::Thread::Exit(exit_code);
  return X_STATUS_SUCCESS;
}

X_STATUS XThread::Terminate(int exit_code) {
  // TODO(benvanik): inform the profiler that this thread is exiting.

  // Set exit code.
  X_KTHREAD* thread = guest_object<X_KTHREAD>();
  thread->header.signal_state = 1;
  thread->exit_status = exit_code;

  // Notify processor of our exit.
  emulator()->processor()->OnThreadExit(thread_id());

  running_ = false;

  xe::FatalError("XThread::Terminate brokey");
  if (XThread::IsInThread(this)) {
    ReleaseHandle();
    xe::threading::Thread::Exit(exit_code);
  } else {
    // thread_->Terminate(exit_code);
    ReleaseHandle();
  }

  return X_STATUS_SUCCESS;
}

class reenter_exception {
 public:
  reenter_exception(uint32_t address) : address_(address){};
  virtual ~reenter_exception(){};
  uint32_t address() const { return address_; }

 private:
  uint32_t address_;
};

void XThread::Execute() {
  XELOGKERNEL("XThread::Execute thid {} (handle={:08X}, '{}', native={:08X})",
              thread_id(), handle(), "", 69420);
  // Let the kernel know we are starting.
  kernel_state()->OnThreadExecute(this);

  // All threads get a mandatory sleep. This is to deal with some buggy
  // games that are assuming the 360 is so slow to create threads that they
  // have time to initialize shared structures AFTER CreateThread (RR).
  xe::threading::Sleep(std::chrono::milliseconds(10));

  // Dispatch any APCs that were queued before the thread was created first.
  DeliverAPCs();

  uint32_t address;
  std::vector<uint64_t> args;
  bool want_exit_code;
  int exit_code = 0;

  // If a XapiThreadStartup value is present, we use that as a trampoline.
  // Otherwise, we are a raw thread.
  if (creation_params_.xapi_thread_startup) {
    address = creation_params_.xapi_thread_startup;
    args.push_back(creation_params_.start_address);
    args.push_back(creation_params_.start_context);
    want_exit_code = false;
  } else {
    // Run user code.
    address = creation_params_.start_address;
    args.push_back(creation_params_.start_context);
    want_exit_code = true;
  }

  uint32_t next_address;
  try {
    exit_code = static_cast<int>(kernel_state()->processor()->Execute(
        thread_state_, address, args.data(), args.size()));
    next_address = 0;
  } catch (const reenter_exception& ree) {
    next_address = ree.address();
  }

  // See XThread::Reenter comments.
  while (next_address != 0) {
    try {
      kernel_state()->processor()->ExecuteRaw(thread_state_, next_address);
      next_address = 0;
      if (want_exit_code) {
        exit_code = static_cast<int>(thread_state_->context()->r[3]);
      }
    } catch (const reenter_exception& ree) {
      next_address = ree.address();
    }
  }

  // If we got here it means the execute completed without an exit being called.
  // Treat the return code as an implicit exit code (if desired).
  Exit(!want_exit_code ? 0 : exit_code);
}

void XThread::Reenter(uint32_t address) {
  // TODO(gibbed): Maybe use setjmp/longjmp on Windows?
  // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/longjmp#remarks
  // On Windows with /EH, setjmp/longjmp do stack unwinding.
  // Is there a better solution than exceptions for stack unwinding?
  throw reenter_exception(address);
}

void XThread::EnqueueApc(uint32_t normal_routine, uint32_t normal_context,
                         uint32_t arg1, uint32_t arg2) {
  // don't use thread_state_ -> context() ! we're not running on the thread
  // we're enqueuing to
  uint32_t success = xboxkrnl::xeNtQueueApcThread(
      this->handle(), normal_routine, normal_context, arg1, arg2,
      cpu::ThreadState::Get()->context());

  xenia_assert(success == X_STATUS_SUCCESS);
}

void XThread::SetCurrentThread() {
  threading::SetFlsValue(g_current_xthread_fls, (uintptr_t)this);
}

void XThread::DeliverAPCs() {
  // https://www.drdobbs.com/inside-nts-asynchronous-procedure-call/184416590?pgno=1
  // https://www.drdobbs.com/inside-nts-asynchronous-procedure-call/184416590?pgno=7
  xboxkrnl::xeProcessUserApcs(thread_state_->context());
}

void XThread::RundownAPCs() {
  xboxkrnl::xeRundownApcs(thread_state_->context());
}

int32_t XThread::QueryPriority() { return priority_; }

void XThread::SetPriority(int32_t increment) {
  if (is_guest_thread()) {
    guest_object<X_KTHREAD>()->priority = static_cast<uint8_t>(increment);
  }
  priority_ = increment;
  int32_t target_priority = 0;
  if (increment > 0x22) {
    target_priority = xe::threading::ThreadPriority::kHighest;
  } else if (increment > 0x11) {
    target_priority = xe::threading::ThreadPriority::kAboveNormal;
  } else if (increment < -0x22) {
    target_priority = xe::threading::ThreadPriority::kLowest;
  } else if (increment < -0x11) {
    target_priority = xe::threading::ThreadPriority::kBelowNormal;
  } else {
    target_priority = xe::threading::ThreadPriority::kNormal;
  }
  if (!cvars::ignore_thread_priorities) {
    //  thread_->set_priority(target_priority);
  }
}

void XThread::SetAffinity(uint32_t affinity) {
  SetActiveCpu(GetFakeCpuNumber(affinity));
}

uint8_t XThread::active_cpu() const {
  const X_KPCR& pcr = *memory()->TranslateVirtual<const X_KPCR*>(pcr_address_);
  return pcr.prcb_data.current_cpu;
}

cpu::HWThread* XThread::HWThread() {
  uint32_t cpunum = active_cpu();

  return kernel_state()->processor()->GetCPUThread(cpunum);
}
void XThread::Schedule() {
  auto context = thread_state()->context();
  context->r[13] = pcr_address_;

  runnable_entry_.thread_state_ = thread_state();
  runnable_entry_.fiber_ = fiber();
  runnable_entry_.list_entry_.next_ = nullptr;
  runnable_entry_.kthread_ = guest_object_ptr_;
  HWThread()->EnqueueRunnableThread(&runnable_entry_);
}

void XThread::YieldCPU() { HWThread()->YieldToScheduler(); }
void XThread::SetActiveCpu(uint8_t cpu_index, bool initial) {
  // May be called during thread creation - don't skip if current == new.

  assert_true(cpu_index < 6);
  auto context = thread_state()->context();
  pcr_address_ = context->processor->GetPCRForCPU(cpu_index);
  context->r[13] = pcr_address_;
  X_KPCR& pcr = *memory()->TranslateVirtual<X_KPCR*>(pcr_address_);
  pcr.prcb_data.current_cpu = cpu_index;

  if (is_guest_thread()) {
    X_KTHREAD& thread_object =
        *memory()->TranslateVirtual<X_KTHREAD*>(guest_object());
    thread_object.current_cpu = cpu_index;
  }
  if (!initial) {
    xe::FatalError("Need to change cpu!");
  }
}

bool XThread::GetTLSValue(uint32_t slot, uint32_t* value_out) {
  if (slot * 4 > tls_total_size_) {
    return false;
  }

  auto mem = memory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  *value_out = xe::load_and_swap<uint32_t>(mem);
  return true;
}

bool XThread::SetTLSValue(uint32_t slot, uint32_t value) {
  if (slot * 4 >= tls_total_size_) {
    return false;
  }

  auto mem = memory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  xe::store_and_swap<uint32_t>(mem, value);
  return true;
}

uint32_t XThread::suspend_count() {
  return guest_object<X_KTHREAD>()->suspend_count;
}

X_STATUS XThread::Resume(uint32_t* out_suspend_count) {
  auto guest_thread = guest_object<X_KTHREAD>();

  uint8_t previous_suspend_count =
      reinterpret_cast<std::atomic_uint8_t*>(&guest_thread->suspend_count)
          ->fetch_sub(1);
  if (out_suspend_count) {
    *out_suspend_count = previous_suspend_count;
  }
  uint32_t unused_host_suspend_count = 0;

  if (previous_suspend_count == 1) {
    Schedule();
  }
  return 0;
}

X_STATUS XThread::Suspend(uint32_t* out_suspend_count) {
  // this normally holds the apc lock for the thread, because it queues a kernel
  // mode apc that does the actual suspension

  X_KTHREAD* guest_thread = guest_object<X_KTHREAD>();

  uint8_t previous_suspend_count =
      reinterpret_cast<std::atomic_uint8_t*>(&guest_thread->suspend_count)
          ->fetch_add(1);
  if (out_suspend_count) {
    *out_suspend_count = previous_suspend_count;
  }
  // If we are suspending ourselves, we can't hold the lock.
  uint32_t unused_host_suspend_count = 0;

  if (previous_suspend_count >= 0) {
    if (IsInThread(this)) {
      this->HWThread()->YieldToScheduler();
    }
  }
  // ops
  return 0;
}

X_STATUS XThread::Delay(uint32_t processor_mode, uint32_t alertable,
                        uint64_t interval) {
  int64_t timeout_ticks = interval;
  uint32_t timeout_ms;
  if (timeout_ticks > 0) {
    // Absolute time, based on January 1, 1601.
    // TODO(benvanik): convert time to relative time.
    assert_always();
    timeout_ms = 0;
  } else if (timeout_ticks < 0) {
    // Relative time.
    timeout_ms = uint32_t(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    timeout_ms = 0;
  }
  timeout_ms = Clock::ScaleGuestDurationMillis(timeout_ms);
  if (alertable) {
    auto result =
        xe::threading::AlertableSleep(std::chrono::milliseconds(timeout_ms));
    switch (result) {
      default:
      case xe::threading::SleepResult::kSuccess:
        return X_STATUS_SUCCESS;
      case xe::threading::SleepResult::kAlerted:
        return X_STATUS_USER_APC;
    }
  } else {
    xe::threading::Sleep(std::chrono::milliseconds(timeout_ms));
    return X_STATUS_SUCCESS;
  }
}

bool XThread::Save(ByteStream* stream) {
  xe::FatalError("XThread::Save unimplemented");
  return false;
}

object_ref<XThread> XThread::Restore(KernelState* kernel_state,
                                     ByteStream* stream) {
  xe::FatalError("XThread::Restore unimplemented");

  return object_ref<XThread>(nullptr);
}

XHostThread::XHostThread(KernelState* kernel_state, uint32_t stack_size,
                         uint32_t creation_flags, std::function<int()> host_fn,
                         uint32_t guest_process)
    : XThread(kernel_state, stack_size, 0, 0, 0, creation_flags, false, false,
              guest_process),
      host_fn_(host_fn) {}

void XHostThread::Execute() {
  XELOGKERNEL(
      "XThread::Execute thid {} (handle={:08X}, '{}', native={:08X}, <host>)",
      thread_id(), handle(), "", 69420);
  // Let the kernel know we are starting.
  kernel_state()->OnThreadExecute(this);
  int ret = host_fn_();

  // Exit.
  Exit(ret);
}

}  // namespace kernel
}  // namespace xe
