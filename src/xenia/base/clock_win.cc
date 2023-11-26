/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/clock.h"

#include "xenia/base/platform_win.h"
XE_NTDLL_IMPORT(NtQuerySystemInformation, NtQuerySystemInformation_cls,
                NtQuerySystemInformationPtr);
static __int64 RtlpHypervisorSharedUserVa = 0;

namespace xe {
constexpr uint32_t SystemHypervisorSharedPageInformation = 0xC5;

#if XE_USE_KUSER_SHARED == 1
uint64_t Clock::host_tick_frequency_platform() { return 10000000ULL; }

uint64_t Clock::host_tick_count_platform() {
  return *reinterpret_cast<volatile uint64_t*>(GetKUserSharedSystemTime());
}
uint64_t Clock::QueryHostSystemTime() {
  return *reinterpret_cast<volatile uint64_t*>(GetKUserSharedSystemTime());
}

#else

Clock::QpcParams Clock::GetQpcParams() {
  QpcParams result;
  result.performance_frequency = *reinterpret_cast<uint64_t*>(0x7FFE0300LL);
  result.shared_user_va_bias = *(uint64_t*)(RtlpHypervisorSharedUserVa + 16);
  result.shared_user_va_multiplier =
      *(unsigned __int64*)(RtlpHypervisorSharedUserVa + 8);
  result.qpc_bias = *reinterpret_cast<uint64_t*>(0x7FFE03B8LL);
  result.qpc_shift = *reinterpret_cast<char*>(0x7FFE03C7LL);
  return result;
}
// pretty much always 10000000
static uint64_t XeQueryPerformanceFrequency() {
  return *reinterpret_cast<uint64_t*>(0x7FFE0300LL);
}

static uint64_t XeQueryPerformanceFrequencyMs() {
  return *reinterpret_cast<uint64_t*>(0x7FFE0300LL) / 1000LL;
}
static uint64_t XeQueryPerformanceCounter() {
  auto v1 = *(uint64_t*)(RtlpHypervisorSharedUserVa + 16);
  uint64_t v2 = *(unsigned __int64*)(RtlpHypervisorSharedUserVa + 8);

  uint64_t v4 = v1 + __umulh(__rdtsc(), v2);
  return (*reinterpret_cast<uint64_t*>(0x7FFE03B8LL) + v4) >>
         *reinterpret_cast<char*>(0x7FFE03C7LL);
}
static uint64_t XeDestinationPerformanceCounterToRdtscStamp(
    uint64_t dest_time) {
  uint64_t rescaled = dest_time;

  rescaled <<= *reinterpret_cast<char*>(0x7FFE03C7LL);

  rescaled -= *reinterpret_cast<uint64_t*>(0x7FFE03B8LL);

  rescaled -= *(uint64_t*)(RtlpHypervisorSharedUserVa + 16);

  // undo __umulh(__rdtsc(), v2);

  uint64_t undo_mul = *(unsigned __int64*)(RtlpHypervisorSharedUserVa + 8);
  unsigned long long rem;
  uint64_t cycles = _udiv128(rescaled, 0, undo_mul, &rem);

  return cycles;
}

uint64_t Clock::host_tick_frequency_platform() {
  return XeQueryPerformanceFrequency();
}

uint64_t Clock::host_tick_count_platform() {
  return XeQueryPerformanceCounter();
}

uint64_t Clock::HostTickTimestampToQuickTimestamp(uint64_t host_ticks) {
  return XeDestinationPerformanceCounterToRdtscStamp(host_ticks);
}

uint64_t Clock::QueryHostSystemTime() {
  FILETIME t;
  GetSystemTimeAsFileTime(&t);
  return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

#endif
uint64_t Clock::QueryHostUptimeMillis() {
  return host_tick_count_platform() * 1000 / host_tick_frequency_platform();
}
// todo: we only take the low part of interrupttime! this is actually a 96-bit
// int!
uint64_t Clock::QueryHostInterruptTime() {
  return *reinterpret_cast<uint64_t*>(KUserShared() +
                                      KUSER_SHARED_INTERRUPTTIME_OFFSET);
}
extern uint64_t guest_tick_frequency_;
extern uint64_t guest_system_time_base_;

extern uint64_t last_host_tick_count_;

void Clock::Initialize() {
  NtQuerySystemInformationPtr.invoke(SystemHypervisorSharedPageInformation,
                                     &RtlpHypervisorSharedUserVa, 8, nullptr);
   guest_tick_frequency_ = Clock::host_tick_frequency_platform();
  // Base FILETIME of the guest system from app start.
   guest_system_time_base_ = Clock::QueryHostSystemTime();


  // Last sampled host tick count.
   last_host_tick_count_ = Clock::QueryHostTickCount();
}
}  // namespace xe
