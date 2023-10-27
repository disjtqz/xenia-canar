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
DEFINE_bool(emulate_guest_interrupts_in_software, false,
            "If true, emulate guest interrupts by repeatedly checking a "
            "location on the PCR. Otherwise uses host ipis.",
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
  worker_->set_affinity_mask(1ULL << owner->cpu_number());
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
  cpu::ThreadState::Bind(runnable->thread_state_);
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
  idle_process_threadstate_->context()->processor->NotifyHWThreadBooted(cpu_number_);

  while (true) {
    cpu::ThreadState::Bind(idle_process_threadstate_);

    // if true, the thread yielded up to us to handle an interrupt.
    // after handling it, return execution to the thread that was interrupted
    if (HandleInterrupts()) {
      RunRunnable(last_run_thread_);
      continue;
    }

    last_run_thread_ =
        reinterpret_cast<RunnableThread*>(runnable_thread_list_.Pop());

    if (!last_run_thread_) {
      RunIdleProcess();
    } else {
      RunRunnable(last_run_thread_);
    }
  }
}

struct GuestIPI {
  threading::AtomicListEntry list_entry_;
  void (*function_)(void*);
  void* ud_;
};

void HWThread::GuestIPIWorkerThreadFunc() {
  while (true) {
    // todo: add another event that signals that its time to terminate
    threading::Wait(guest_ipi_dispatch_event_.get(), false);
    auto list_entry = reinterpret_cast<GuestIPI*>(guest_ipi_list_.Pop());
    if (!list_entry) {
      continue;
    }
    while (!TrySendInterruptFromHost(list_entry->function_, list_entry->ud_)) {
      threading::NanoSleep(10000);
    }
    delete list_entry;
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
  os_thread_->set_affinity_mask(1ULL << cpu_number_);
  os_thread_->set_name(std::string("PPC HW Thread ") +
                       std::to_string(cpu_number));

  guest_ipi_dispatch_event_ = threading::Event::CreateAutoResetEvent(false);
  params.stack_size = 512 * 1024;
  params.initial_priority = threading::ThreadPriority::kBelowNormal;
  params.create_suspended = false;
  guest_ipi_dispatch_worker_ = threading::Thread::Create(

      params, std::bind(&HWThread::GuestIPIWorkerThreadFunc, this));

  // is putting it on the same os thread a good idea? nah, probably not
  // but until we have real thread allocation logic thats where it goes
  guest_ipi_dispatch_worker_->set_affinity_mask(1ULL << cpu_number_);
  guest_ipi_dispatch_worker_->set_name(
      std::string("PPC HW Thread IPI Worker ") + std::to_string(cpu_number));
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
  cpu::ThreadState::Bind(idle_process_threadstate_);
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

  interrupt_wrapper->ipi_func(interrupt_wrapper->ud);

  current_context->RestoreGPRSnapshot(&snap);

  // new_ctx->msr = msr;
  // cpu::ThreadState::Bind(old_ts);

  return 1;
}
#define NO_RESULT_MAGIC 0x69420777777ULL
bool HWThread::TrySendInterruptFromHost(void (*ipi_func)(void*), void* ud) {
  GuestInterruptWrapper wrapper{};
  wrapper.ipi_func = ipi_func;
  wrapper.ud = ud;
  wrapper.thiz = this;
  // ipi wrapper returns 0 if current context has interrupts disabled
  volatile uintptr_t result_from_call = 0;
  if (cvars::emulate_guest_interrupts_in_software) {
    auto processor = idle_process_threadstate_->context()->processor;
    auto pcr = processor->GetPCRForCPU(this->cpu_number_);

    kernel::X_KPCR* p_pcr =
        processor->memory()->TranslateVirtual<kernel::X_KPCR*>(pcr);

    ppc::PPCInterruptRequest request;
    request.func_ = IPIWrapperFunction;
    request.ud_ = (void*)&wrapper;
    request.result_out_ = (uintptr_t*)&result_from_call;

    while (result_from_call == 0) {
      result_from_call = NO_RESULT_MAGIC;
      while (!xe::atomic_cas(reinterpret_cast<uint64_t>(p_pcr),
                             reinterpret_cast<uint64_t>(&request),
                             &p_pcr->emulated_interrupt)) {
        threading::MaybeYield();
      }

      while (p_pcr->emulated_interrupt ==
             reinterpret_cast<uint64_t>(&request)) {
        threading::MaybeYield();
      }
      // guest has read the interrupt, now wait for it to change our value

      while (result_from_call == NO_RESULT_MAGIC) {
        threading::MaybeYield();
      }
    }

    return true;

  } else {
    while (!os_thread_->IPI(IPIWrapperFunction, &wrapper,
                            (uintptr_t*)&result_from_call) ||
           result_from_call == 0) {
      threading::NanoSleep(10000);
    }
  }
  return true;
}
bool HWThread::SendGuestIPI(void (*ipi_func)(void*), void* ud) {
  // todo: pool this structure!

  auto msg = new GuestIPI();
  msg->list_entry_.next_ = nullptr;
  msg->function_ = ipi_func;
  msg->ud_ = ud;
  guest_ipi_list_.Push(&msg->list_entry_);
  guest_ipi_dispatch_event_->SetBoostPriority();
  return true;
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