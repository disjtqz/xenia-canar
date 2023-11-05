/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Xenia Canart. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/hwclock.h"
#include "xenia/base/clock.h"
#include "xenia/base/platform_win.h"
#include "xenia/cpu/processor.h"
namespace xe {
namespace cpu {

void HWClock::SynchronizeToHostClockInterrupt() {
  volatile uint64_t* timer_stash =
      reinterpret_cast<volatile uint64_t*>(0x7FFE0320ULL);
  uint64_t start_ticks = *timer_stash;

  while (start_ticks == *timer_stash) {
    _mm_pause();
  }
}
void HWClock::WorkerThreadMain() {
  SynchronizeToHostClockInterrupt();
  uint64_t last_value = Clock::QueryHostUptimeMillis();

  while (true) {
    uint64_t new_value;
    while (true) {
      new_value = Clock::QueryHostUptimeMillis();
      if (new_value != last_value) {
        if (cvars::threads_aint_cheap) {
          threading::MaybeYield();
        }
        break;
      }
    }

    uint64_t num_interrupts_to_trigger = new_value - last_value;
    last_value = new_value;

    // for (uint64_t i = 0; i < num_interrupts_to_trigger; ++i) {
    if (interrupt_callback_) {
      interrupt_callback_(processor_);
    }
    //}
  }
}
HWClock::HWClock(Processor* processor) : processor_(processor) {
  threading::Thread::CreationParameters crparams{};
  crparams.stack_size = 65536;
  crparams.initial_priority = threading::ThreadPriority::kBelowNormal;
  crparams.create_suspended = true;
  timer_thread_ = threading::Thread::Create(
      crparams, std::bind(&HWClock::WorkerThreadMain, this));
}
HWClock::~HWClock() {}

void HWClock::Start() { timer_thread_->Resume(); }
}  // namespace cpu
}  // namespace xe