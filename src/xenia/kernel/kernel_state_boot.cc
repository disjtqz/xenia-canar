/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_state.h"

#include <string>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_memory.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"
namespace xe {
namespace kernel {

void KernelState::InitializeProcess(X_KPROCESS* process, uint32_t type,
                                    char unk_18, char unk_19, char unk_1A) {
  uint32_t guest_kprocess = memory()->HostToGuestVirtual(process);

  uint32_t thread_list_guest_ptr =
      guest_kprocess + offsetof(X_KPROCESS, thread_list);

  process->unk_18 = unk_18;
  process->unk_19 = unk_19;
  process->unk_1A = unk_1A;
  util::XeInitializeListHead(&process->thread_list, thread_list_guest_ptr);
  process->unk_0C = 60;
  // doubt any guest code uses this ptr, which i think probably has something to
  // do with the page table
  process->clrdataa_masked_ptr = 0;
  // clrdataa_ & ~(1U << 31);
  process->thread_count = 0;
  // process->unk_1B = 0x06;
  process->kernel_stack_size = 16 * 1024;
  process->tls_slot_size = 0x80;

  process->process_type = type;
  uint32_t unk_list_guest_ptr = guest_kprocess + offsetof(X_KPROCESS, unk_54);
  // TODO(benvanik): figure out what this list is.
  util::XeInitializeListHead(&process->unk_54, unk_list_guest_ptr);
}

void KernelState::SetProcessTLSVars(X_KPROCESS* process, int num_slots,
                                    int tls_data_size,
                                    int tls_static_data_address) {
  uint32_t slots_padded = (num_slots + 3) & 0xFFFFFFFC;
  process->tls_data_size = tls_data_size;
  process->tls_raw_data_size = tls_data_size;
  process->tls_static_data_address = tls_static_data_address;
  process->tls_slot_size = 4 * slots_padded;
  uint32_t count_div32 = slots_padded / 32;
  for (unsigned word_index = 0; word_index < count_div32; ++word_index) {
    process->tls_slot_bitmap[word_index] = -1;
  }

  // set remainder of bitset
  if (((num_slots + 3) & 0x1C) != 0)
    process->tls_slot_bitmap[count_div32] = -1
                                            << (32 - ((num_slots + 3) & 0x1C));
}
void AllocateThread(PPCContext* context) {
  uint32_t thread_mem_size = static_cast<uint32_t>(context->r[3]);
  uint32_t a2 = static_cast<uint32_t>(context->r[4]);
  uint32_t a3 = static_cast<uint32_t>(context->r[5]);
  if (thread_mem_size <= 0xFD8) thread_mem_size += 8;
  uint32_t result =
      xboxkrnl::xeAllocatePoolTypeWithTag(context, thread_mem_size, a2, a3);
  if (((unsigned short)result & 0xFFF) != 0) {
    result += 2;
  }

  context->r[3] = static_cast<uint64_t>(result);
}
void FreeThread(PPCContext* context) {
  uint32_t thread_memory = static_cast<uint32_t>(context->r[3]);
  if ((thread_memory & 0xFFF) != 0) {
    thread_memory -= 8;
  }
  xboxkrnl::xeFreePool(context, thread_memory);
}

void SimpleForwardAllocatePoolTypeWithTag(PPCContext* context) {
  uint32_t a1 = static_cast<uint32_t>(context->r[3]);
  uint32_t a2 = static_cast<uint32_t>(context->r[4]);
  uint32_t a3 = static_cast<uint32_t>(context->r[5]);
  context->r[3] = static_cast<uint64_t>(
      xboxkrnl::xeAllocatePoolTypeWithTag(context, a1, a2, a3));
}
void SimpleForwardFreePool(PPCContext* context) {
  xboxkrnl::xeFreePool(context, static_cast<uint32_t>(context->r[3]));
}

void DeleteMutant(PPCContext* context) {
  // todo: this should call kereleasemutant with some specific args

  xe::FatalError("DeleteMutant - need KeReleaseMutant(mutant, 1, 1, 0) ");
}
void DeleteTimer(PPCContext* context) {
  // todo: this should call KeCancelTimer
  xe::FatalError("DeleteTimer - need KeCancelTimer(mutant, 1, 1, 0) ");
}

void DeleteIoCompletion(PPCContext* context) {}

void UnknownProcIoDevice(PPCContext* context) {}

void CloseFileProc(PPCContext* context) {}

void DeleteFileProc(PPCContext* context) {}

void UnknownFileProc(PPCContext* context) {}

void DeleteSymlink(PPCContext* context) {
  X_KSYMLINK* lnk = context->TranslateVirtualGPR<X_KSYMLINK*>(context->r[3]);

  context->r[3] = lnk->refed_object_maybe;
  xboxkrnl::xeObDereferenceObject(context, lnk->refed_object_maybe);
}

static void InitializeHandleTable(util::X_HANDLE_TABLE* result,
                                  unsigned char poolarg,
                                  unsigned char handle_high_byte,
                                  unsigned char unk_36) {
  result->unk_pool_arg_34 = poolarg;
  result->handle_high_byte = handle_high_byte;
  result->unk_36 = unk_36;
  result->num_handles = 0;
  result->free_offset = 0;
  result->highest_allocated_offset = 0;
  result->table_dynamic_buckets = 0;
}

static void GuestClockInterruptForwarder(void* ud) {
  reinterpret_cast<KernelState*>(ud)->SystemClockInterrupt();
}
// called by HWClock on hw clock thread. sends an interrupt to guest cpu 0 to
// run the kernels clock interrupt function
static void HWClockCallback(cpu::Processor* processor) {
  for (unsigned thrd = 0; thrd < 6; ++thrd) {
    auto thrd0 = processor->GetCPUThread(thrd);

    while (!thrd0->SendGuestIPI(GuestClockInterruptForwarder, kernel_state())) {
    }
  }
}
static void DefaultInterruptProc(PPCContext* context) {}

static void IPIInterruptProc(PPCContext* context) {}

// ues _KTHREAD list_entry field at 0x94
// this dpc uses none of the routine args
static void DestroyThreadDpc(PPCContext* context) {
  XELOGD("DestroyThreadDpc");
  //  context->kernel_state->LockDispatcherAtIrql(context);

  // context->kernel_state->UnlockDispatcherAtIrql(context);
}

static void ThreadSwitchHelper(PPCContext* context, X_KPROCESS* process) {
  xboxkrnl::xeKeKfAcquireSpinLock(context, &process->thread_list_spinlock,
                                  false);

  context->kernel_state->LockDispatcherAtIrql(context);

  auto v3 = &GetKPCR(context)->prcb_data;
  for (auto&& i : process->thread_list.IterateForward(context)) {
    if (i.a_prcb_ptr.xlat() == v3 && i.another_prcb_ptr.xlat() != v3 &&
        i.thread_state != 6) {
      xboxkrnl::xeHandleReadyThreadOnDifferentProcessor(context, &i);
    }
  }
  context->kernel_state->UnlockDispatcherAtIrql(context);

  xboxkrnl::xeKeKfReleaseSpinLock(context, &process->thread_list_spinlock, 0,
                                  false);
}

static void ThreadSwitchRelatedDpc(PPCContext* context) {
  // iterates over threads in the game process + threads in the system process
  auto kgg = context->kernel_state->GetKernelGuestGlobals(context);

  ThreadSwitchHelper(context, &kgg->title_process);
  ThreadSwitchHelper(context, &kgg->system_process);
}

void KernelState::InitProcessorStack(X_KPCR* pcr) {
  pcr->unk_stack_5c = xboxkrnl::xeMmCreateKernelStack(0x4000, 2);
  uint32_t other_stack = xboxkrnl::xeMmCreateKernelStack(0x4000, 2);
  pcr->stack_base_ptr = other_stack;
  pcr->alt_stack_base_ptr = other_stack;
  pcr->use_alternative_stack = other_stack;
  pcr->stack_end_ptr = other_stack - 0x4000;
  pcr->alt_stack_end_ptr = other_stack - 0x4000;
}

void KernelState::SetupProcessorPCR(uint32_t which_processor_index) {
  XELOGD("Setting up processor {} pcr", which_processor_index);
  X_KPCR_PAGE* page_for = this->KPCRPageForCpuNumber(which_processor_index);
  memset(page_for, 0, 4096);

  auto pcr = &page_for->pcr;
  pcr->prcb_data.current_cpu = static_cast<uint8_t>(which_processor_index);
  pcr->prcb_data.processor_mask = 1U << which_processor_index;
  pcr->prcb = memory()->HostToGuestVirtual(&pcr->prcb_data);

  XeInitializeListHead(&pcr->prcb_data.queued_dpcs_list_head, memory());
  for (uint32_t i = 0; i < 32; ++i) {
    util::XeInitializeListHead(&pcr->prcb_data.ready_threads_by_priority[i],
                               memory());
  }
  pcr->prcb_data.unk_mask_64 = 0xF6DBFC03;
  pcr->prcb_data.thread_exit_dpc.Initialize(
      kernel_trampoline_group_.NewLongtermTrampoline(DestroyThreadDpc), 0);
  // remember, DPC cpu indices start at 1
  pcr->prcb_data.thread_exit_dpc.desired_cpu_number = which_processor_index + 1;
  util::XeInitializeListHead(&pcr->prcb_data.terminating_threads_list,
                             memory());

  pcr->prcb_data.switch_thread_processor_dpc.Initialize(
      kernel_trampoline_group_.NewLongtermTrampoline(ThreadSwitchRelatedDpc),
      0);

  pcr->prcb_data.switch_thread_processor_dpc.desired_cpu_number =
      which_processor_index + 1;

  // this cpu needs special handling, its initializing the kernel
  // InitProcessorStack gets called for it later, after all kernel init
  if (which_processor_index == 0) {
    uint32_t protdata = processor()->GetPCRForCPU(0);
    uint32_t protdata_stackbase = processor()->GetPCRForCPU(0) + 0x7000;

    pcr->stack_base_ptr = protdata_stackbase;
    pcr->alt_stack_base_ptr = protdata_stackbase;
    pcr->use_alternative_stack = protdata_stackbase;
    // it looks like it actually sets it to pcr3?? that seems wrong
    // probably a hexrays/ida bug or even a kernel bug

    // we are only giving it a page of stack though
    pcr->alt_stack_end_ptr = protdata + 0x6000;
    pcr->stack_end_ptr = protdata + 0x6000;
  } else {
    this->InitProcessorStack(pcr);
  }
  uint32_t default_interrupt =
      kernel_trampoline_group_.NewLongtermTrampoline(DefaultInterruptProc);
  for (uint32_t i = 0; i < 32; ++i) {
    pcr->interrupt_handlers[i] = default_interrupt;
  }

  // todo: missing some interrupts here

  pcr->interrupt_handlers[0x1E] =
      kernel_trampoline_group_.NewLongtermTrampoline(IPIInterruptProc);

  pcr->current_irql = 0;
  pcr->thread_fpu_related = -1;
  pcr->msr_mask = -1;
  pcr->thread_vmx_related = -1;
}
// need to implement "initialize thread" function!
// this gets called after initial pcr
void KernelState::SetupProcessorIdleThread(uint32_t which_processor_index) {
  XELOGD("Setting up processor {} idle thread", which_processor_index);
  X_KPCR_PAGE* page_for = this->KPCRPageForCpuNumber(which_processor_index);
  X_KTHREAD* thread = &page_for->idle_process_thread;
  thread->thread_state = 2;

  thread->priority = 31;
  thread->unk_A4 = 2;
  thread->may_queue_apcs = 1;

  auto prcb_guest = memory()->HostToGuestVirtual(&page_for->pcr.prcb_data);
  thread->a_prcb_ptr = prcb_guest;
  thread->another_prcb_ptr = prcb_guest;
  thread->current_cpu = page_for->pcr.prcb_data.current_cpu;
  auto idle_process_ptr = GetIdleProcess();
  thread->process = idle_process_ptr;
  auto guest_thread = memory()->HostToGuestVirtual(thread);
  page_for->pcr.prcb_data.current_thread = guest_thread;
  page_for->pcr.prcb_data.idle_thread = guest_thread;

  auto process = memory()->TranslateVirtual<X_KPROCESS*>(idle_process_ptr);
  // priority related values
  thread->unk_C8 = process->unk_18;
  auto v19 = process->unk_19;
  thread->unk_C9 = v19;
  auto v20 = process->unk_1A;
  thread->unk_B9 = v19;
  thread->unk_CA = v20;
  // timeslice related
  thread->unk_B4 = process->unk_0C;
  thread->msr_mask = 0xFDFFD7FF;
}

void KernelState::SetupKPCRPageForCPU(uint32_t cpunum) {
  XELOGD("SetupKPCRPageForCpu - cpu {}", cpunum);
  SetupProcessorPCR(cpunum);
  SetupProcessorIdleThread(cpunum);
}

static void KernelNullsub(PPCContext* context) {}
void KernelState::BootInitializeStatics() {
  XELOGD("Initializing kernel statics");
  kernel_guest_globals_ = memory_->SystemHeapAlloc(sizeof(KernelGuestGlobals));

  KernelGuestGlobals* block =
      memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  memset(block, 0, sizeof(block));

  block->background_processors = 0x3C;

  auto idle_process = memory()->TranslateVirtual<X_KPROCESS*>(GetIdleProcess());
  InitializeProcess(idle_process, X_PROCTYPE_IDLE, 0, 0, 0);
  idle_process->unk_0C = 0x7F;
  auto system_process =
      memory()->TranslateVirtual<X_KPROCESS*>(GetSystemProcess());
  InitializeProcess(system_process, X_PROCTYPE_SYSTEM, 2, 5, 9);
  SetProcessTLSVars(system_process, 32, 0, 0);

  InitializeHandleTable(&block->TitleObjectTable, X_PROCTYPE_TITLE, 0xF8, 0);
  InitializeHandleTable(&block->TitleThreadIdTable, X_PROCTYPE_TITLE, 0xF9, 0);

  // i cant find where these get initialized on 17559, but we already know what
  // to fill in here
  InitializeHandleTable(&block->SystemObjectTable, X_PROCTYPE_SYSTEM, 0xFA, 0);
  InitializeHandleTable(&block->SystemThreadIdTable, X_PROCTYPE_SYSTEM, 0xFB,
                        0);

  block->running_timers.Initialize(memory());
  uint32_t oddobject_offset =
      kernel_guest_globals_ +
      offsetof(KernelGuestGlobals, XboxKernelDefaultObject);

  // init unknown object

  block->XboxKernelDefaultObject.type = 1;

  block->XboxKernelDefaultObject.signal_state = 1;
  util::XeInitializeListHead(
      &block->XboxKernelDefaultObject.wait_list,
      oddobject_offset + offsetof32(X_DISPATCH_HEADER, wait_list));

  // init thread object
  block->ExThreadObjectType.pool_tag = 0x65726854;
  block->ExThreadObjectType.allocate_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(AllocateThread);

  block->ExThreadObjectType.free_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(FreeThread);

  // several object types just call freepool/allocatepool
  uint32_t trampoline_allocatepool =
      kernel_trampoline_group_.NewLongtermTrampoline(
          SimpleForwardAllocatePoolTypeWithTag);
  uint32_t trampoline_freepool =
      kernel_trampoline_group_.NewLongtermTrampoline(SimpleForwardFreePool);

  // init event object
  block->ExEventObjectType.pool_tag = 0x76657645;
  block->ExEventObjectType.allocate_proc = trampoline_allocatepool;
  block->ExEventObjectType.free_proc = trampoline_freepool;

  // init mutant object
  block->ExMutantObjectType.pool_tag = 0x6174754D;
  block->ExMutantObjectType.allocate_proc = trampoline_allocatepool;
  block->ExMutantObjectType.free_proc = trampoline_freepool;

  block->ExMutantObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteMutant);
  // init semaphore obj
  block->ExSemaphoreObjectType.pool_tag = 0x616D6553;
  block->ExSemaphoreObjectType.allocate_proc = trampoline_allocatepool;
  block->ExSemaphoreObjectType.free_proc = trampoline_freepool;
  // init timer obj
  block->ExTimerObjectType.pool_tag = 0x656D6954;
  block->ExTimerObjectType.allocate_proc = trampoline_allocatepool;
  block->ExTimerObjectType.free_proc = trampoline_freepool;
  block->ExTimerObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteTimer);
  // iocompletion object
  block->IoCompletionObjectType.pool_tag = 0x706D6F43;
  block->IoCompletionObjectType.allocate_proc = trampoline_allocatepool;
  block->IoCompletionObjectType.free_proc = trampoline_freepool;
  block->IoCompletionObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteIoCompletion);
  block->IoCompletionObjectType.unknown_size_or_object_ = oddobject_offset;

  // iodevice object
  block->IoDeviceObjectType.pool_tag = 0x69766544;
  block->IoDeviceObjectType.allocate_proc = trampoline_allocatepool;
  block->IoDeviceObjectType.free_proc = trampoline_freepool;
  block->IoDeviceObjectType.unknown_size_or_object_ = oddobject_offset;
  block->IoDeviceObjectType.unknown_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(UnknownProcIoDevice);

  // file object
  block->IoFileObjectType.pool_tag = 0x656C6946;
  block->IoFileObjectType.allocate_proc = trampoline_allocatepool;
  block->IoFileObjectType.free_proc = trampoline_freepool;
  block->IoFileObjectType.unknown_size_or_object_ =
      0x38;  // sizeof fileobject, i believe
  block->IoFileObjectType.close_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(CloseFileProc);
  block->IoFileObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteFileProc);
  block->IoFileObjectType.unknown_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(UnknownFileProc);

  // directory object
  block->ObDirectoryObjectType.pool_tag = 0x65726944;
  block->ObDirectoryObjectType.allocate_proc = trampoline_allocatepool;
  block->ObDirectoryObjectType.free_proc = trampoline_freepool;
  block->ObDirectoryObjectType.unknown_size_or_object_ = oddobject_offset;

  // symlink object
  block->ObSymbolicLinkObjectType.pool_tag = 0x626D7953;
  block->ObSymbolicLinkObjectType.allocate_proc = trampoline_allocatepool;
  block->ObSymbolicLinkObjectType.free_proc = trampoline_freepool;
  block->ObSymbolicLinkObjectType.unknown_size_or_object_ = oddobject_offset;
  block->ObSymbolicLinkObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteSymlink);

  host_object_type_enum_to_guest_object_type_ptr_ = {
      {XObject::Type::Event,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExEventObjectType)},
      {XObject::Type::Semaphore,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExSemaphoreObjectType)},
      {XObject::Type::Thread,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExThreadObjectType)},
      {XObject::Type::File,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, IoFileObjectType)},
      {XObject::Type::Mutant,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExMutantObjectType)},
      {XObject::Type::Device,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, IoDeviceObjectType)}};

  block->guest_nullsub =
      kernel_trampoline_group_.NewLongtermTrampoline(KernelNullsub);
  block->suspendthread_apc_routine =
      kernel_trampoline_group_.NewLongtermTrampoline(
          xboxkrnl::xeSuspendThreadApcRoutine);
  block->extimer_dpc_routine = kernel_trampoline_group_.NewLongtermTrampoline(
      xboxkrnl::xeEXTimerDPCRoutine);

  block->extimer_apc_kernel_routine =
      kernel_trampoline_group_.NewLongtermTrampoline(
          xboxkrnl::xeEXTimerAPCKernelRoutine);

  block->graphics_interrupt_dpc.Initialize(
      kernel_trampoline_group_.NewLongtermTrampoline(
          &KernelState::GraphicsInterruptDPC),
      0);
  // cpu2,remember all dpc cpu numbers are +1, because 0 means "any cpu"
  block->graphics_interrupt_dpc.desired_cpu_number = 3;
}

static void SetupIdleThreadPriority(cpu::ppc::PPCContext* context,
                                    X_KPCR* kpcr) {
  xboxkrnl::xeKeSetPriorityThread(context, kpcr->prcb_data.idle_thread.xlat(),
                                  0);
  kpcr->prcb_data.idle_thread->priority = 18;
  if (!kpcr->prcb_data.next_thread) {
    kpcr->prcb_data.running_idle_thread.m_ptr = 1;
  }
}

void KernelState::BootCPU0(cpu::ppc::PPCContext* context, X_KPCR* kpcr) {
  KernelGuestGlobals* block =
      memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);

  util::XeInitializeListHead(
      &block->UsbdBootEnumerationDoneEvent.header.wait_list, context);
  xboxkrnl::xeKeSetEvent(context, &block->UsbdBootEnumerationDoneEvent, 1, 0);

  for (unsigned i = 1; i < 6; ++i) {
    SetupKPCRPageForCPU(i);
  }

  xboxkrnl::xeKfLowerIrql(context, 1);
  for (unsigned i = 1; i < 6; ++i) {
    auto cpu_thread = processor()->GetCPUThread(i);
    cpu_thread->Boot();
  }

  BootInitializeXam(context);

  // this is deliberate, does not change the interrupt priority!
  kpcr->current_irql = 2;
  SetupIdleThreadPriority(context, kpcr);
}

static void XamNotifyListenerDeleteProc(PPCContext* context) {
  uint32_t a1 = static_cast<uint32_t>(context->r[3]);
  if (a1) {
    uint32_t deref1 = *context->TranslateVirtualBE<uint32_t>(a1);
    uint32_t deref2 = *context->TranslateVirtualBE<uint32_t>(deref1);
    context->processor->ExecuteRaw(context->thread_state(), deref2);
    return;
  }
}

void KernelState::BootInitializeXam(cpu::ppc::PPCContext* context) {
  XELOGD("BootInitializeXam");
  auto globals = context->kernel_state->GetKernelGuestGlobals(context);

  uint32_t trampoline_allocatepool =
      kernel_trampoline_group_.NewLongtermTrampoline(
          SimpleForwardAllocatePoolTypeWithTag);
  uint32_t trampoline_freepool =
      kernel_trampoline_group_.NewLongtermTrampoline(SimpleForwardFreePool);

  globals->XboxKernelDefaultObject.type = 1;
  globals->XboxKernelDefaultObject.signal_state = 1;

  util::XeInitializeListHead(&globals->XboxKernelDefaultObject.wait_list,
                             context);

  globals->XamNotifyListenerObjectType.allocate_proc = trampoline_allocatepool;
  globals->XamNotifyListenerObjectType.free_proc = trampoline_freepool;
  globals->XamNotifyListenerObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(
          XamNotifyListenerDeleteProc);
  globals->XamNotifyListenerObjectType.unknown_size_or_object_ = 0xC;
  globals->XamNotifyListenerObjectType.pool_tag = 0x66746F4E;
  // todo: Enumerator!
}

void KernelState::BootCPU1Through5(cpu::ppc::PPCContext* context,
                                   X_KPCR* kpcr) {
  // todo: sets priority here! need to fill that in

  xboxkrnl::xeKfLowerIrql(context, 2);
  SetupIdleThreadPriority(context, kpcr);
}

void KernelState::HWThreadBootFunction(cpu::ppc::PPCContext* context,
                                       void* ud) {
  KernelState* ks = reinterpret_cast<KernelState*>(ud);
  context->kernel_state = ks;

  /*
    todo: the hypervisor or bootloader does some initialization before this
    point

  */

  auto kpcr = GetKPCR(context);

  kpcr->emulated_interrupt = reinterpret_cast<uint64_t>(kpcr);
  auto cpunum = ks->GetPCRCpuNum(kpcr);

  kpcr->prcb_data.current_cpu = cpunum;
  kpcr->prcb_data.processor_mask = 1U << cpunum;

  if (cpunum == 0) {
    ks->BootCPU0(context, kpcr);
  } else {
    ks->BootCPU1Through5(context, kpcr);
  }
}
void KernelState::BootKernel() {
  XELOGD("Booting kernel");
  BootInitializeStatics();

  // initialize the idle process' thread list prior to startup for convenience

  auto idle_process_ptr = GetIdleProcess();
  auto idle_process = memory()->TranslateVirtual<X_KPROCESS*>(idle_process_ptr);
  idle_process->thread_count = 6;

  for (unsigned i = 0; i < 6; ++i) {
    auto cpu_thread = processor()->GetCPUThread(i);
    cpu_thread->SetIdleProcessFunction(&KernelState::KernelIdleProcessFunction);
    cpu_thread->SetBootFunction(&KernelState::HWThreadBootFunction, this);
    cpu_thread->SetDecrementerInterruptCallback(
        &KernelState::KernelDecrementerInterrupt, nullptr);
    // dont need the thread list lock, because no guest code is running atm
    util::XeInsertTailList(
        &idle_process->thread_list,
        &this->KPCRPageForCpuNumber(i)->idle_process_thread.process_threads,
        memory());
  }
  SetupKPCRPageForCPU(0);
  // cpu 0 boots all other cpus
  processor()->GetCPUThread(0)->Boot();

  while (!processor()->AllHWThreadsBooted()) {
    threading::NanoSleep(10000);  // 10 microseconds
  }
  XELOGD("All processor HW threads have booted up");
  processor()->GetHWClock()->SetInterruptCallback(HWClockCallback);

  auto bundle =
      memory()->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(GetKeTimestampBundle());
  uint32_t initial_ms = static_cast<uint32_t>(Clock::QueryGuestTickCount());
  uint64_t initial_systemtime = Clock::QueryGuestSystemTime();

  bundle->interrupt_time = initial_systemtime;
  bundle->system_time = initial_systemtime;
  bundle->tick_count = initial_ms;
  processor()->GetHWClock()->Start();
}

}  // namespace kernel
}  // namespace xe
