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

void HWDecrementer::WorkerMain() {
  threading::WaitHandle* cancellable[2];
  cancellable[0] = this->timer_.get();
  cancellable[1] = this->wrote_.get();

  while (true) {
    threading::Wait(this->wrote_.get(), false);

  cancelled:
    int32_t decrementer_ticks = this->value_;

    if (decrementer_ticks < 0) {
      // negative value means signal immediately
      goto do_ipi;
    }

    double wait_time_in_nanoseconds =
        (static_cast<double>(decrementer_ticks) /
         static_cast<double>(TIMEBASE_FREQUENCY)) *
        1000000000.0;
    timer_->SetOnceAfter(chrono::hundrednanoseconds(
        static_cast<long long>(wait_time_in_nanoseconds / 100.0)));
    {
      auto [result, which] = threading::WaitAny(cancellable, 2, false);
      // if the index is wrote_, our interrupt has been cancelled and a new one
      // needs setting up
      if (which == 1) {
        goto cancelled;
      }
    }

  do_ipi:
    // timer was signalled
    // send using sendguestipi, which is async. the interrupt may set the
    // decrementer again!
    hw_thread_->SendGuestIPI(interrupt_callback_, ud_);
  }
}

HWDecrementer::HWDecrementer(HWThread* owner) : hw_thread_(owner) {
  wrote_ = threading::Event::CreateAutoResetEvent(false);
  timer_ = threading::Timer::CreateManualResetTimer();
  value_ = 0x7FFFFFFF;
  threading::Thread::CreationParameters crparams;
  crparams.stack_size = 65536;
  // use a much higher priority than the hwthread itself so that we get more
  // accurate timing
  crparams.initial_priority = threading::ThreadPriority::kHighest;

  worker_ = threading::Thread::Create(
      crparams, std::bind(&HWDecrementer::WorkerMain, this));
  worker_->set_name("Decrementer Thread " +
                    std::to_string(owner->cpu_number()));
}
HWDecrementer ::~HWDecrementer() {}

void HWDecrementer::Set(int32_t value) {
  this->value_ = value;
  this->wrote_->Set();
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
void HWThread::ThreadFunc() {
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
      runnable_thread_list_(),
      decrementer_(std::make_unique<HWDecrementer>(this)) {
  threading::Thread::CreationParameters params;
  params.create_suspended = true;
  params.initial_priority = threading::ThreadPriority::kNormal;
  params.stack_size = 16 * 1024 * 1024;

  os_thread_ =
      threading::Thread::Create(params, std::bind(&HWThread::ThreadFunc, this));

  if (cvars::threads_aint_cheap) {
    os_thread_->set_affinity_mask(1ULL << cpu_number_);
  }

  os_thread_->set_name(std::string("PPC HW Thread ") +
                       std::to_string(cpu_number));

  os_thread_->is_ppc_thread_ = true;
  interrupt_controller_ = std::make_unique<XenonInterruptController>(
      this, thread_state->context()->processor);
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

// todo: handle interrupt controller/irql shit, that matters too
// theres a special mmio region 0x7FFF (or 0xFFFF, cant tell)
bool HWThread::AreInterruptsDisabled() {
  auto context = cpu::ThreadState::Get()->context();
  if (!context->ExternalInterruptsEnabled()) {
    return true;
  }
  return false;
}
struct GuestInterruptWrapper {
  void (*ipi_func)(void*);
  void* ud;
  HWThread* thiz;
};

static bool may_run_interrupt_proc(ppc::PPCContext_s* context) {
  return context->ExternalInterruptsEnabled();
}

uintptr_t HWThread::IPIWrapperFunction(void* ud) {
  auto interrupt_wrapper = reinterpret_cast<GuestInterruptWrapper*>(ud);

  if (interrupt_wrapper->thiz->AreInterruptsDisabled()) {
    // retry!
    return 0;
  }
  // todo: need to set current thread to idle thread!!
  // auto old_ts = cpu::ThreadState::Get();
  // auto new_ts = interrupt_wrapper->thiz->idle_process_threadstate_;
  // cpu::ThreadState::Bind(new_ts);
  // auto new_ctx = new_ts->context();

  // uint64_t msr = new_ctx->msr;
  // new_ctx->DisableEI();
  auto current_context = cpu::ThreadState::GetContext();
  ppc::PPCGprSnapshot snap;
  current_context->TakeGPRSnapshot(&snap);

  auto kpcr = current_context->TranslateVirtualGPR<kernel::X_KPCR*>(
      current_context->r[13]);

  auto old_irql = kpcr->current_irql;
  interrupt_wrapper->ipi_func(interrupt_wrapper->ud);
  kpcr = current_context->TranslateVirtualGPR<kernel::X_KPCR*>(
      current_context->r[13]);
  auto new_irql = kpcr->current_irql;
  xenia_assert(old_irql == new_irql);

  current_context->RestoreGPRSnapshot(&snap);

  // new_ctx->msr = msr;
  // cpu::ThreadState::Bind(old_ts);
  delete interrupt_wrapper;
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
  GuestInterruptWrapper* wrapper = new GuestInterruptWrapper();

  wrapper->ipi_func = ipi_func;
  wrapper->ud = ud;
  wrapper->thiz = this;
  // ipi wrapper returns 0 if current context has interrupts disabled
  volatile uintptr_t result_from_call = 0;

  ppc::PPCInterruptRequest* request =
      this->interrupt_controller()->AllocateInterruptRequest();

  request->func_ = IPIWrapperFunction;
  request->ud_ = (void*)wrapper;
  request->may_run_ = may_run_interrupt_proc;
  request->result_out_ = (uintptr_t*)&result_from_call;
  request->wait = wait_done;

  this->interrupt_controller()->queued_interrupts_.Push(&request->list_entry_);

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

uint64_t HWThread::mftb() const {
  // need to rescale to TIMEBASE_FREQUENCY
  long long freq = _Query_perf_frequency();

  long long counter = _Query_perf_counter();
  unsigned long long rem = 0;

  unsigned long long ratio = (49875000ULL << 32) / static_cast<uint64_t>(freq);

  unsigned long long result_low = (ratio * counter) >> 32;

  unsigned long long result_high = __umulh(ratio, counter);

  unsigned long long result = result_low | (result_high << 32);
  return result;
}

}  // namespace cpu
}  // namespace xe