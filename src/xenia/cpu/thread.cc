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

DEFINE_bool(threads_aint_cheap, false, "For people with < 8 hardware threads",
            "CPU");

DEFINE_bool(enable_cpu_timing_fences, false,
            "If true, introduce artificial delays to try to better match "
            "original cpu/kernel timing",
            "CPU");

DEFINE_bool(
    no_idle_sleeping_for_hw_threads, false,
    "If true, do not make the os thread sleep when a hw thread has no work. "
    "Reduces latency for interrupts at the cost of much higher cpu usage.",
    "CPU");
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

// thread_local HWThread* this_hw_thread = nullptr;

HWThread* this_hw_thread(ppc::PPCContext* context) {
  return context->processor->GetCPUThread((context->r[13] >> 12) & 0x7);
}
void HWThread::ThreadFunc() {
  if (cpu_number_) {
    // synchronize to cpu 0 timebase

    uint64_t tbtime = mftb();
    uint64_t systemtime = Clock::QueryHostSystemTime();

    // estimate from difference in systemtime what cpu0's timestamp counter
    // currently looks like
    uint64_t systemtime_delta = systemtime - mftb_cycle_sync_systemtime_;
    constexpr double HUNDREDNANOSECOND_TO_SECOND = 1e-7;

    constexpr double RESCALE_SYSTIME =
        static_cast<double>(TIMEBASE_FREQUENCY) * HUNDREDNANOSECOND_TO_SECOND;

    uint64_t current_cpu0_timebase =
        mftb_cycle_sync_ +
        static_cast<uint64_t>(
            round(RESCALE_SYSTIME * static_cast<double>(systemtime_delta)));

    if (current_cpu0_timebase > tbtime) {
      mftb_delta_sign_ = false;
      mftb_delta_ = current_cpu0_timebase - tbtime;
    } else {
      mftb_delta_sign_ = true;
      mftb_delta_ = tbtime - current_cpu0_timebase;
    }
  }
  threading::set_current_thread_id(this->cpu_number_);
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
  bool internal_;
};
// todo: handle interrupt controller/irql shit, that matters too
// theres a special mmio region 0x7FFF (or 0xFFFF, cant tell)
static bool may_run_interrupt_proc(ppc::PPCContext_s* context) {
  return context->ExternalInterruptsEnabled() &&
         this_hw_thread(context)->interrupt_controller()->GetEOI() != 0;
}
static bool internal_may_run_interrupt_proc(ppc::PPCContext_s* context) {
  // despite not using the external interrupt controller, EI still controls
  // whether the decrementer interrupt happens
  return context->ExternalInterruptsEnabled();
}

uintptr_t HWThread::IPIWrapperFunction(ppc::PPCContext_s* context,
                                       ppc::PPCInterruptRequest* request,
                                       void* ud) {
  auto interrupt_wrapper = reinterpret_cast<GuestInterruptWrapper*>(ud);

  ppc::PPCGprSnapshot snap;
  context->TakeGPRSnapshot(&snap);
  if (!interrupt_wrapper->internal_) {  // is it an external interrupt? most are
    auto kpcr = context->TranslateVirtualGPR<kernel::X_KPCR*>(context->r[13]);

    bool cr2 = kpcr->use_alternative_stack == 0;

    auto old_irql = kpcr->current_irql;
    bool cr3;
    context->DisableEI();
    if (cr2) {
      cr3 = 1 < old_irql;
      if (!cr3) {
        kpcr->current_irql = kernel::IRQL_DISPATCH;
      }
      kpcr->use_alternative_stack = kpcr->alt_stack_base_ptr;
      context->r[1] = kpcr->alt_stack_base_ptr;
    }
    this_hw_thread(context)->interrupt_controller()->SetEOI(0);

    interrupt_wrapper->ipi_func(interrupt_wrapper->ud);
    this_hw_thread(context)->interrupt_controller()->SetEOI(1);
    // xenia_assert(interrupt_wrapper->thiz->interrupt_controller()->GetEOI());
    kpcr = context->TranslateVirtualGPR<kernel::X_KPCR*>(context->r[13]);

    context->RestoreGPRSnapshot(&snap);

    if (cr2) {
      kpcr->use_alternative_stack = 0;
      if (!cr3) {
        context->kernel_state->GenericExternalInterruptEpilog(context,
                                                              old_irql);
      }
    }
  } else {
    // internal interrupt, does not get dispatched the same way
    interrupt_wrapper->ipi_func(interrupt_wrapper->ud);
    context->RestoreGPRSnapshot(&snap);
  }
  context->AssertInterruptsOn();
  return 2;
}

void HWThread::ThreadDelay() {
  if (cvars::threads_aint_cheap) {
    threading::MaybeYield();
  } else {
    _mm_pause();
  }
}
bool HWThread::TrySendInterruptFromHost(SendInterruptArguments& arguments) {
  auto ipi_func = arguments.ipi_func;
  auto ud = arguments.ud;
  auto wait_done = arguments.wait_done;
  ppc::PPCInterruptRequest* request =
      this->interrupt_controller()->AllocateInterruptRequest();
  GuestInterruptWrapper* wrapper =
      reinterpret_cast<GuestInterruptWrapper*>(&request->extra_data_[0]);

  wrapper->ipi_func = ipi_func;
  wrapper->ud = ud;
  wrapper->thiz = this;
  wrapper->internal_ = arguments.internal_interrupt_;

  // ipi wrapper returns 0 if current context has interrupts disabled
  volatile uintptr_t result_from_call = 0;

  request->func_ = IPIWrapperFunction;
  request->ud_ = (void*)wrapper;
  request->may_run_ = arguments.internal_interrupt_
                          ? internal_may_run_interrupt_proc
                          : may_run_interrupt_proc;
  request->result_out_ = (uintptr_t*)&result_from_call;
  request->wait = wait_done;
  request->interrupt_serial_number_ =
      this->interrupt_controller()->interrupt_serial_number_++;
  request->internal_interrupt_ = arguments.internal_interrupt_;
  request->irql_ = arguments.irql_;
  this->interrupt_controller()->queued_interrupts_.Push(&request->list_entry_);
  if (!cvars::no_idle_sleeping_for_hw_threads) {
    auto context = cpu::ThreadState::GetContext();
    if (!context || this_hw_thread(context) != this) {
      this->wake_idle_event_->Set();
    }
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
bool HWThread::SendGuestIPI(SendInterruptArguments& arguments) {
  // todo: pool this structure!
  return TrySendInterruptFromHost(arguments);
}

void HWThread::DecrementerInterruptEnqueueProc(
    XenonInterruptController* controller, uint32_t slot, void* ud) {
  auto thiz = reinterpret_cast<HWThread*>(ud);

  cpu::SendInterruptArguments interrupt_arguments{};
  interrupt_arguments.ipi_func = thiz->decrementer_interrupt_callback_;
  interrupt_arguments.ud = thiz->decrementer_ud_;
  interrupt_arguments.wait_done = false;
  interrupt_arguments.irql_ = 0;
  interrupt_arguments.internal_interrupt_ = true;
  thiz->SendGuestIPI(interrupt_arguments);

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
  if (!cvars::no_idle_sleeping_for_hw_threads) {
    threading::NanoWait(wake_idle_event_.get(), false, nanoseconds);
  }
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
  if (mftb_delta_sign_) {
    return result - mftb_delta_;
  } else {
    return result + mftb_delta_;
  }
}

void HWThread::Suspend() { os_thread_->Suspend(); }
void HWThread::Resume() { os_thread_->Resume(); }

MFTBFence::MFTBFence(uint64_t timebase_cycles)
    : desired_timebase_value_(
          timebase_cycles + this_hw_thread(ThreadState::GetContext())->mftb()) {
}
MFTBFence::~MFTBFence() {
  auto context = ThreadState::GetContext();
  auto hwthread = this_hw_thread(context);
  if (cvars::enable_cpu_timing_fences) {
    while (hwthread->mftb() < desired_timebase_value_) {
      context->CheckInterrupt();
    }
  }
}

}  // namespace cpu
}  // namespace xe