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
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"
namespace xe {
namespace cpu {

thread_local Thread* Thread::current_thread_ = nullptr;

Thread::Thread() {}
Thread::~Thread() {}

bool Thread::IsInThread() { return current_thread_ != nullptr; }

Thread* Thread::GetCurrentThread() { return current_thread_; }
uint32_t Thread::GetCurrentThreadId() {
  return Thread::GetCurrentThread()->thread_state()->thread_id();
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
      threading::MaybeYield();
    }
    delete list_entry;
  }
}

HWThread::HWThread(uint32_t cpu_number, cpu::ThreadState* thread_state)
    : cpu_number_(cpu_number),
      idle_process_threadstate_(thread_state),
      runnable_thread_list_() {
  threading::Thread::CreationParameters params;
  params.create_suspended = true;
  params.initial_priority = threading::ThreadPriority::kAboveNormal;
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
}
HWThread::~HWThread() {
  xenia_assert(false);  // dctor not implemented yet
}

void HWThread::EnqueueRunnableThread(RunnableThread* rth) {
  rth->list_entry_.next_ = nullptr;
  runnable_thread_list_.Push(&rth->list_entry_);
}

void HWThread::YieldToScheduler() { this->idle_process_fiber_->SwitchTo(); }

// todo: handle interrupt controller/irql shit, that matters too
// theres a special mmio region 0x7FFF (or 0xFFFF, cant tell)
bool HWThread::AreInterruptsDisabled() {
  auto context = cpu::ThreadState::Get()->context();
  if (context->msr & 0x8000) {
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

  interrupt_wrapper->ipi_func(interrupt_wrapper->ud);
  return 1;
}

bool HWThread::TrySendInterruptFromHost(void (*ipi_func)(void*), void* ud) {
  GuestInterruptWrapper wrapper{};
  wrapper.ipi_func = ipi_func;
  wrapper.ud = ud;
  wrapper.thiz = this;
  // ipi wrapper returns 0 if current context has interrupts disabled
  uintptr_t result_from_call=0;

  while (!os_thread_->IPI(IPIWrapperFunction, &wrapper, &result_from_call) ||
         result_from_call == 0) {
    threading::MaybeYield();
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

}  // namespace cpu
}  // namespace xe