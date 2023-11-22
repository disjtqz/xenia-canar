/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_THREAD_H_
#define XENIA_CPU_THREAD_H_
#include "xenia/base/cvar.h"
#include "xenia/base/threading.h"

#include <cstdint>
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/xenon_interrupt_controller.h"
DECLARE_bool(emulate_guest_interrupts_in_software);
DECLARE_bool(threads_aint_cheap);
namespace xe {
namespace cpu {
class ThreadState;

// Represents a thread that runs guest code.
class Thread {
 public:
  Thread();
  ~Thread();

  static bool IsInThread();
  static Thread* GetCurrentThread();
  static uint32_t GetCurrentThreadId();
  ThreadState* thread_state() const { return thread_state_; }

  // True if the thread should be paused by the debugger.
  // All threads that can run guest code must be stopped for the debugger to
  // work properly.
  bool can_debugger_suspend() const { return can_debugger_suspend_; }
  void set_can_debugger_suspend(bool value) { can_debugger_suspend_ = value; }

  xe::threading::Thread* thread() { return thread_.get(); }
  const std::string& thread_name() const { return thread_name_; }

 protected:
  thread_local static Thread* current_thread_;

  ThreadState* thread_state_ = nullptr;
  std::unique_ptr<xe::threading::Thread> thread_ = nullptr;

  bool can_debugger_suspend_ = true;
  std::string thread_name_;
};

struct RunnableThread {
  threading::AtomicListEntry list_entry_;

  threading::Fiber* fiber_;
  cpu::ThreadState* thread_state_;
  uint32_t kthread_;
};
class HWThread;

/*
    decrementer interrupt handler
    runs at the same rate as the timebase


    according to KeQueryPerformanceFrequency, there are 50000000 timebase ticks
   per second

    the normal decrementer interrupt does 0x7FFFFFFF ticks
    that means that the decrementer has an interval of 42 seconds? that cant be
   right

    xeSelectThreadDueToTimesliceExpiration sets the decrementer to 50000, which
   is 1 millisecond

    this makes more sense: the decrementer signals the end of the timeslice. in
   the interrupt, it gets set to an impossibly large value so it wont trigger
   again. the kernel isnt setting it because it wants it to take 42 seconds to
   trigger. so lets just treat 0x7FFFFFFF as a special value that disables the
   decrementer


*/
// this figure comes courtesy of libxenon. turns out 50mhz was not the real
// frequency, so i wonder where we got that figure from
static constexpr uint64_t TIMEBASE_FREQUENCY = 49875000ULL;

static constexpr int32_t DECREMENTER_DISABLE = 0x7FFFFFFF;

class HWThread {
  void ThreadFunc();

  bool HandleInterrupts();

  void RunRunnable(RunnableThread* runnable);
  // dpcs?
  void RunIdleProcess();

  static uintptr_t IPIWrapperFunction(ppc::PPCContext_s* context,
                                      ppc::PPCInterruptRequest* request,
                                      void* ud);
  volatile bool ready_ = false;
  std::unique_ptr<threading::Thread> os_thread_;

  std::unique_ptr<threading::Fiber> idle_process_fiber_;

  cpu::ThreadState* idle_process_threadstate_;
  uint32_t cpu_number_;

  threading::AtomicListHeader runnable_thread_list_;

  RunnableThread* last_run_thread_ = nullptr;

  // set by kernel
  void (*idle_process_function_)(ppc::PPCContext* context) = nullptr;

  void (*boot_function_)(ppc::PPCContext* context, void* ud) = nullptr;
  void* boot_ud_ = nullptr;
  std::unique_ptr<XenonInterruptController> interrupt_controller_;

  void (*external_interrupt_handler_)(cpu::ppc::PPCContext* context,
                                      XenonInterruptController* controller);

  uint32_t decrementer_interrupt_slot_ = ~0u;

  void (*decrementer_interrupt_callback_)(void* ud);
  void* decrementer_ud_;

  uint32_t host_thread_id_;
  static void DecrementerInterruptEnqueueProc(
      XenonInterruptController* controller, uint32_t slot, void* ud); 

  std::unique_ptr<threading::Event> wake_idle_event_;
 public:
  HWThread(uint32_t cpu_number, cpu::ThreadState* thread_state);
  ~HWThread();

  uint32_t cpu_number() const { return cpu_number_; }

  void SetBootFunction(void (*f)(ppc::PPCContext*, void*), void* ud) {
    boot_function_ = f;
    boot_ud_ = ud;
  }
  bool HasBooted() { return ready_; }

  void SetDecrementerTicks(int32_t ticks);
  void SetDecrementerInterruptCallback(void (*decr)(void* ud), void* ud);

  static void ThreadDelay();
  void SetExternalInterruptHandler(void (*handler)(
      cpu::ppc::PPCContext* context, XenonInterruptController* controller)) {
    external_interrupt_handler_ = handler;
  }

  void _CallExternalInterruptHandler(cpu::ppc::PPCContext* context,
                                     XenonInterruptController* controller) {
    if (external_interrupt_handler_) {
      external_interrupt_handler_(context, controller);
    }
  }

  void SetIdleProcessFunction(
      void (*idle_process_function)(ppc::PPCContext* context)) {
    idle_process_function_ = idle_process_function;
  }

  void Boot() { os_thread_->Resume(); }

  void EnqueueRunnableThread(RunnableThread* rth);

  void YieldToScheduler();

  bool TrySendInterruptFromHost(void (*ipi_func)(void*), void* ud,
                                bool wait_done = false);

  void IdleSleep(int64_t nanoseconds);

  void Suspend();
  void Resume();

  uint64_t mftb() const;
  // SendGuestIPI is designed to run on a guest thread
  // it ought to be nonblocking, unlike TrySendHostIPI
  bool SendGuestIPI(void (*ipi_func)(void*), void* ud);
  XenonInterruptController* interrupt_controller() {
    return interrupt_controller_.get();
  }
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_THREAD_H_
