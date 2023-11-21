/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/thread.h"
#include "xenia/base/atomic.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_guest_structures.h"
#include "xenia/kernel/kernel_state.h"
DEFINE_bool(emulate_guest_interrupts_in_software, true,
            "If true, emulate guest interrupts by repeatedly checking a "
            "location on the PCR. Otherwise uses host ipis.",
            "CPU");
DEFINE_bool(threads_aint_cheap, false, "For people with < 8 hardware threads",
            "CPU");
#define XE_NO_IPI_WORKERS 1
namespace xe {
namespace cpu {

thread_local Thread* Thread::current_thread_ = nullptr;

Thread::Thread() {}
Thread::~Thread() {}

bool Thread::IsInThread() { return current_thread_ != nullptr; }

Thread* Thread::GetCurrentThread() { return current_thread_; }
uint32_t Thread::GetCurrentThreadId() {
  return cpu::ThreadState::GetContext()->thread_id;
}

bool HWThread::HandleInterrupts() { return false; }

void HWThread::RunRunnable(RunnableThread* runnable) {
  runnable->fiber_->SwitchTo();
}

void HWThread::RunIdleProcess() {
  if (idle_process_function_) {
    idle_process_function_(cpu::ThreadState::Get()->context());
  }
}

thread_local HWThread* this_hw_thread = nullptr;
void HWThread::ThreadFunc() {
  this_hw_thread = this;
  interrupt_controller()->Initialize();
  idle_process_fiber_ = threading::Fiber::CreateFromThread();
  cpu::ThreadState::Bind(idle_process_threadstate_);

  if (boot_function_) {
    boot_function_(idle_process_threadstate_->context(), boot_ud_);
  }

  ready_ = true;
  idle_process_threadstate_->context()->processor->NotifyHWThreadBooted(
      cpu_number_);

  while (true) {
    RunIdleProcess();
  }
}

HWThread::HWThread(uint32_t cpu_number, cpu::ThreadState* thread_state)
    : cpu_number_(cpu_number),
      idle_process_threadstate_(thread_state),
      runnable_thread_list_() {
  threading::Thread::CreationParameters params;
  params.create_suspended = true;
  params.initial_priority = threading::ThreadPriority::kBelowNormal;
  params.stack_size = 16 * 1024 * 1024;

  os_thread_ =
      threading::Thread::Create(params, std::bind(&HWThread::ThreadFunc, this));

  if (!cvars::threads_aint_cheap) {
    os_thread_->set_affinity_mask(1ULL << cpu_number_);
  }

  os_thread_->set_name(std::string("PPC HW Thread ") +
                       std::to_string(cpu_number));

  os_thread_->is_ppc_thread_ = true;
  interrupt_controller_ = std::make_unique<XenonInterruptController>(
      this, thread_state->context()->processor);
  host_thread_id_ = os_thread_->system_id();
  wake_idle_event_ = threading::Event::CreateAutoResetEvent(false);
}
HWThread::~HWThread() {
  xenia_assert(false);  // dctor not implemented yet
}

void HWThread::EnqueueRunnableThread(RunnableThread* rth) {
  rth->list_entry_.next_ = nullptr;
  runnable_thread_list_.Push(&rth->list_entry_);
}

void HWThread::YieldToScheduler() {
  xenia_assert(cpu::ThreadState::Get() != idle_process_threadstate_);
  xenia_assert(threading::Fiber::GetCurrentFiber() !=
               this->idle_process_fiber_.get());
  // cpu::ThreadState::Bind(idle_process_threadstate_);
  this->idle_process_fiber_->SwitchTo();
}

struct GuestInterruptWrapper {
  void (*ipi_func)(void*);
  void* ud;
  HWThread* thiz;
};
// todo: handle interrupt controller/irql shit, that matters too
// theres a special mmio region 0x7FFF (or 0xFFFF, cant tell)
static bool may_run_interrupt_proc(ppc::PPCContext_s* context) {
  return context->ExternalInterruptsEnabled() &&
         this_hw_thread->interrupt_controller()->GetEOI() != 0;
}

uintptr_t HWThread::IPIWrapperFunction(ppc::PPCContext_s* context,
                                       ppc::PPCInterruptRequest* request,
                                       void* ud) {
  auto interrupt_wrapper = reinterpret_cast<GuestInterruptWrapper*>(ud);

  ppc::PPCGprSnapshot snap;
  context->TakeGPRSnapshot(&snap);

  auto kpcr = context->TranslateVirtualGPR<kernel::X_KPCR*>(context->r[13]);

  bool cr2 = kpcr->use_alternative_stack == 0;
  auto old_irql = kpcr->current_irql;
  bool cr3;
  context->DisableEI();
  if (cr2) {
    cr3 = 1 < old_irql;
    if (!cr3) {
      kpcr->current_irql = 2;
    }
    kpcr->use_alternative_stack = kpcr->alt_stack_base_ptr;
  }
  this_hw_thread->interrupt_controller()->SetEOI(0);

  interrupt_wrapper->ipi_func(interrupt_wrapper->ud);
  this_hw_thread->interrupt_controller()->SetEOI(1);
  // xenia_assert(interrupt_wrapper->thiz->interrupt_controller()->GetEOI());
  kpcr = context->TranslateVirtualGPR<kernel::X_KPCR*>(context->r[13]);

  context->RestoreGPRSnapshot(&snap);

  if (cr2) {
    kpcr->use_alternative_stack = 0;
    if (!cr3) {
      context->kernel_state->GenericExternalInterruptEpilog(context, old_irql);
    }
  }

  return 2;
}
#define NO_RESULT_MAGIC 0x69420777777ULL

void HWThread::ThreadDelay() {
  if (cvars::threads_aint_cheap) {
    threading::MaybeYield();
  } else {
    _mm_pause();
  }
}
bool HWThread::TrySendInterruptFromHost(void (*ipi_func)(void*), void* ud,
                                        bool wait_done) {
  ppc::PPCInterruptRequest* request =
      this->interrupt_controller()->AllocateInterruptRequest();
  GuestInterruptWrapper* wrapper =
      reinterpret_cast<GuestInterruptWrapper*>(&request->extra_data_[0]);

  wrapper->ipi_func = ipi_func;
  wrapper->ud = ud;
  wrapper->thiz = this;
  // ipi wrapper returns 0 if current context has interrupts disabled
  volatile uintptr_t result_from_call = 0;

  request->func_ = IPIWrapperFunction;
  request->ud_ = (void*)wrapper;
  request->may_run_ = may_run_interrupt_proc;
  request->result_out_ = (uintptr_t*)&result_from_call;
  request->wait = wait_done;

  this->interrupt_controller()->queued_interrupts_.Push(&request->list_entry_);

  if (this_hw_thread != this) {
    wake_idle_event_->Set();
  }
  if (!wait_done) {
    return true;
  } else {
    while (result_from_call != 2) {
      ThreadDelay();
    }
    return true;
  }

  return true;
}
bool HWThread::SendGuestIPI(void (*ipi_func)(void*), void* ud) {
  // todo: pool this structure!
  return TrySendInterruptFromHost(ipi_func, ud, false);
}

void HWThread::DecrementerInterruptEnqueueProc(
    XenonInterruptController* controller, uint32_t slot, void* ud) {
  auto thiz = reinterpret_cast<HWThread*>(ud);
  thiz->SendGuestIPI(thiz->decrementer_interrupt_callback_,
                     thiz->decrementer_ud_);

  controller->FreeTimedInterruptSlot(slot);
  thiz->decrementer_interrupt_slot_ = ~0u;
}
void HWThread::SetDecrementerTicks(int32_t ticks) {
  if (decrementer_interrupt_slot_ != ~0u) {
    interrupt_controller()->FreeTimedInterruptSlot(decrementer_interrupt_slot_);
    decrementer_interrupt_slot_ = ~0u;
  }
  // 0x7FFFFFFF just means cancel
  if (ticks != 0x7FFFFFFF) {
    double wait_time_in_microseconds =
        (static_cast<double>(ticks) / static_cast<double>(TIMEBASE_FREQUENCY)) *
        1000000.0;

    CpuTimedInterrupt cti;
    cti.destination_microseconds_ =
        interrupt_controller()->CreateRelativeUsTimestamp(
            static_cast<uint64_t>(wait_time_in_microseconds));

    cti.ud_ = this;
    cti.enqueue_ = DecrementerInterruptEnqueueProc;

    decrementer_interrupt_slot_ =
        interrupt_controller()->AllocateTimedInterruptSlot();

    interrupt_controller()->SetTimedInterruptArgs(decrementer_interrupt_slot_,
                                                  &cti);
  }
  interrupt_controller()->RecomputeNextEventCycles();
}
void HWThread::SetDecrementerInterruptCallback(void (*decr)(void* ud),
                                               void* ud) {
  decrementer_interrupt_callback_ = decr;
  decrementer_ud_ = ud;
}
void HWThread::IdleSleep(int64_t nanoseconds) {
  threading::NanoWait(wake_idle_event_.get(), false, nanoseconds);
}

uint64_t HWThread::mftb() const {
  // need to rescale to TIMEBASE_FREQUENCY
  
  long long freq = Clock::host_tick_frequency_platform();

  long long counter = Clock::host_tick_count_platform();
  unsigned long long rem = 0;

  unsigned long long ratio = (49875000ULL << 32) / static_cast<uint64_t>(freq);

  unsigned long long result_low = (ratio * counter) >> 32;

  unsigned long long result_high = __umulh(ratio, counter);

  unsigned long long result = result_low | (result_high << 32);
  return result;
}

}  // namespace cpu
}  // namespace xe