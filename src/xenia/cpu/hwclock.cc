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

void HWClock::SynchronizeToHostClockInterrupt() {}
void HWClock::WorkerThreadMain() {
  SynchronizeToHostClockInterrupt();

  uint64_t millisecond_frequency =
      Clock::host_tick_frequency_platform() / 1000LL;

  uint64_t last_tick_count = Clock::host_tick_count_platform();

  uint64_t rdtsc_endpoint = Clock::HostTickTimestampToQuickTimestamp(
      last_tick_count + millisecond_frequency);

  while (true) {
    uint64_t new_value;
    while (true) {
      new_value = Clock::QueryQuickCounter();
      if (new_value >= rdtsc_endpoint) {
        break;
      } else {
        _mm_pause();
        _mm_pause();
        _mm_pause();
        _mm_pause();
      }
    }
    last_tick_count = Clock::host_tick_count_platform();

    rdtsc_endpoint = Clock::HostTickTimestampToQuickTimestamp(
        last_tick_count + millisecond_frequency);

    // uint64_t num_interrupts_to_trigger = new_value - last_value;
    // last_value = new_value;

    // for (uint64_t i = 0; i < num_interrupts_to_trigger; ++i) {
    if (interrupt_callback_) {
      interrupt_callback_(processor_);
    }

    threading::NanoSleep(1000000 / 2);
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