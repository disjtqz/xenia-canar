/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Xenia Canary. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_HWCLOCK_H_
#define XENIA_CPU_HWCLOCK_H_

#include "xenia/base/threading.h"
namespace xe {
namespace cpu {

class Processor;
#define     XE_USE_TIMED_INTERRUPTS_FOR_CLOCK 1

// raises the clock interrupt on cpu 0 every 1 millisecond
class HWClock {
  void SynchronizeToHostClockInterrupt();
  void WorkerThreadMain();

 public:
  HWClock(Processor* processor);
  ~HWClock();
  void SetInterruptCallback(void (*callback)(Processor*)) {
    interrupt_callback_ = callback;
  }

  void Start();
 private:
  Processor* processor_;
  std::unique_ptr<threading::Thread> timer_thread_;

  void (*interrupt_callback_)(Processor*) = nullptr;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_HWCLOCK_H_
