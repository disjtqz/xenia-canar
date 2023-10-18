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
    }
  }
}

HWThread::HWThread(uint32_t cpu_number, cpu::ThreadState* thread_state)
    : cpu_number_(cpu_number), idle_process_threadstate_(thread_state), runnable_thread_list_() {
  threading::Thread::CreationParameters params;
  params.create_suspended = true;
  params.initial_priority = threading::ThreadPriority::kAboveNormal;
  params.stack_size = 16 * 1024 * 1024;

  os_thread_ =
      threading::Thread::Create(params, std::bind(&HWThread::ThreadFunc, this));
  os_thread_->set_affinity_mask(1ULL << cpu_number_);
}
HWThread::~HWThread() {}

void HWThread::EnqueueRunnableThread(RunnableThread* rth) {
  rth->list_entry_.next_ = nullptr;
  runnable_thread_list_.Push(&rth->list_entry_);
}

void HWThread::YieldToScheduler() { this->idle_process_fiber_->SwitchTo(); }
}  // namespace cpu
}  // namespace xe