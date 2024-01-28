/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Xenia Canary. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/xenon_interrupt_controller.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/mmio_handler.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread.h"
namespace xe {
namespace cpu {
// index is irql >> 2
static constexpr int interrupt_priorities[32] = {
    -1, -1, -1, -1, -1, 3,  13, -1, 2,  1,  -1, 4,   5,  6, 7,  -1,
    11, 12, -1, 10, -1, 69, 66, -1, -1, 64, 68, 131, -1, 0, -1, -1};

int XenonInterruptController::KernelIrqlToInterruptPriority(uint8_t irql) {
  return interrupt_priorities[irql >> 2];
}

XenonInterruptController::XenonInterruptController(HWThread* thread,
                                                   Processor* processor)
    : cpu_number_(thread->cpu_number()),
      owner_(thread),
      processor_(processor) {}

XenonInterruptController::~XenonInterruptController() {}

uint32_t XenonInterruptController::GuestMMIOAddress() const {
  xenia_assert(cpu_number_ < 6);
  return 0x7FFF0000 | (cpu_number_ << 12);
}
static void RaiseMMIOError() {
  xe::FatalError(
      "MMIO for interrupt controller unimplemented; 64-bit reads and "
      "writes unsupported by MMIO subsystem");
}
static uint32_t ReadRegisterStub(void* ppc_context, void* ud, uint32_t addr) {
  RaiseMMIOError();
  return 0;
}

static void WriteRegisterStub(void* ppc_context, void* ud, uint32_t addr,
                              uint32_t value) {
  RaiseMMIOError();
}

void XenonInterruptController::Initialize() {
  memset(data_, 0, sizeof(data_));
  processor_->memory()->AddVirtualMappedRange(GuestMMIOAddress(), 0xFFFF0000,
                                              0xFFFF, this, ReadRegisterStub,
                                              WriteRegisterStub);

  tick_microsecond_frequency =
      Clock::host_tick_frequency_platform() / (1000ULL * 1000ULL);
}

void XenonInterruptController::SetInterruptSource(uint64_t src) {
  WriteRegisterOffset(0x50, src);
}

void XenonInterruptController::InterruptFunction(void* ud) {
  auto extargs = reinterpret_cast<ExternalInterruptArgs*>(ud);
  auto controller = extargs->controller_;

  controller->SetInterruptSource(extargs->source_);

  controller->owner_->_CallExternalInterruptHandler(
      cpu::ThreadState::GetContext(), controller);
}

void XenonInterruptController::SendExternalInterrupt(
    ExternalInterruptArgs& args) {
  xenia_assert(false);
}

void XenonInterruptController::WriteRegisterOffset(uint32_t offset,
                                                   uint64_t value) {
  xenia_assert(offset + 8 <= sizeof(data_));

  *reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(&data_[0]) + offset) =
      value;
  if (offset == 8) {
    current_interrupt_priority_ =
        KernelIrqlToInterruptPriority(static_cast<uint8_t>(value));
  }
}
uint64_t XenonInterruptController::ReadRegisterOffset(uint32_t offset) {
  xenia_assert(offset + 8 <= sizeof(data_));
  return *reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(&data_[0]) +
                                      offset);
}

ppc::PPCInterruptRequest* XenonInterruptController::AllocateInterruptRequest() {
  auto head = free_interrupt_requests_.Pop();
  if (head) {
    return new (head) ppc::PPCInterruptRequest();
  } else {
    return new ppc::PPCInterruptRequest();
  }
}
void XenonInterruptController::FreeInterruptRequest(
    ppc::PPCInterruptRequest* request) {
  request->list_entry_.next_ = nullptr;
  // limit the number of available interrupts in the list to a sane value
  // if we hit this number, the guest has probably frozen and isn't processing
  // the interrupts we're sending
  if (free_interrupt_requests_.depth() < 256) {
    free_interrupt_requests_.Push(&request->list_entry_);
  } else {
    delete request;
  }
}

uint32_t XenonInterruptController::AllocateTimedInterruptSlot() {
  for (uint32_t i = 0; i < MAX_CPU_TIMED_INTERRUPTS; ++i) {
    if (!(timed_event_slots_bitmap_ & (1U << i))) {
      timed_event_slots_bitmap_ |= 1U << i;
      return i;
    }
  }
  xenia_assert(false);  // need to expand free slots!
  xe::FatalError("out of timed interrupt slots!");
  return ~0u;
}

void XenonInterruptController::FreeTimedInterruptSlot(uint32_t slot) {
  xenia_assert(slot < MAX_CPU_TIMED_INTERRUPTS);
  xenia_assert(timed_event_slots_bitmap_ & (1U << slot));
  timed_event_slots_bitmap_ &= ~(1U << slot);
}
void XenonInterruptController::SetTimedInterruptArgs(uint32_t slot,
                                                     CpuTimedInterrupt* data) {
  timed_events_[slot] = *data;
}

void XenonInterruptController::RecomputeNextEventCycles() {
  last_qpc_params_ = Clock::GetQpcParams();
  uint64_t lowest_cycles = ~0ull;
  for (uint32_t i = 0; i < MAX_CPU_TIMED_INTERRUPTS; ++i) {
    if (!(timed_event_slots_bitmap_ & (1U << i))) {
      continue;
    }

    uint64_t rdtsc_cycles = Clock::HostTickTimestampToQuickTimestamp(
        timed_events_[i].destination_microseconds_ *
        tick_microsecond_frequency);

    if (rdtsc_cycles < lowest_cycles) {
      lowest_cycles = rdtsc_cycles;
    }
  }
  next_event_quick_timestamp_ = lowest_cycles;
}

void XenonInterruptController::EnqueueTimedInterrupts() {
  for (uint32_t timed_interrupt_slot = 0;
       timed_interrupt_slot < MAX_CPU_TIMED_INTERRUPTS;
       ++timed_interrupt_slot) {
    if (!(timed_event_slots_bitmap_ & (1U << timed_interrupt_slot))) {
      continue;
    }
    uint64_t current_time_us =
        Clock::host_tick_count_platform() / tick_microsecond_frequency;
    if (timed_events_[timed_interrupt_slot].destination_microseconds_ <
        current_time_us) {
      timed_events_[timed_interrupt_slot].enqueue_(
          this, timed_interrupt_slot, timed_events_[timed_interrupt_slot].ud_);
    }
  }
  RecomputeNextEventCycles();
}

uint64_t XenonInterruptController::CreateRelativeUsTimestamp(
    uint64_t microseconds) {
  return (Clock::host_tick_count_platform() / tick_microsecond_frequency) +
         microseconds;
}

uint64_t XenonInterruptController::ClampSleepMicrosecondsForTimedInterrupt(
    uint64_t sleep_microseconds) {
  uint64_t current_microseconds =
      Clock::host_tick_count_platform() / tick_microsecond_frequency;

  uint64_t sleep_expiration = current_microseconds + sleep_microseconds;
  uint64_t minimum_event_time = sleep_expiration;
  for (uint32_t timed_interrupt_slot = 0;
       timed_interrupt_slot < MAX_CPU_TIMED_INTERRUPTS;
       ++timed_interrupt_slot) {
    if (!(timed_event_slots_bitmap_ & (1U << timed_interrupt_slot))) {
      continue;
    }

    minimum_event_time = std::min<uint64_t>(
        minimum_event_time,
        timed_events_[timed_interrupt_slot].destination_microseconds_);

  }

  if (minimum_event_time == sleep_expiration) {
      //input delay is fine, no events would be missed
    return sleep_microseconds;
  } else {
    uint64_t delta_to_event = minimum_event_time - current_microseconds;

    
    //compute delta * 0.75
    //onehalf + onefourth
    //we do this because most of the time the kernel takes a good deal longer
    //than our provided interval
    return (delta_to_event >> 2) + (delta_to_event >> 1);
  }
}

void XenonInterruptController::SetEOI(uint64_t value) {
  auto context = cpu::ThreadState::GetContext();
  uint32_t cpunum = (context->r[13] >> 12) & 0x7;
  auto cpu = context->processor->GetCPUThread(cpunum);
  if (cpu->interrupt_controller() != this) {
    xenia_assert(false);
  }
  eoi_written_ = static_cast<uint32_t>(value);
  if (eoi_written_ && eoi_write_mirror_) {
    *eoi_write_mirror_ = 2;
    eoi_write_mirror_ = nullptr;
  }
}

bool XenonInterruptController::CanRunInterruptAtIrql(uint8_t irql) {
  int32_t prio = KernelIrqlToInterruptPriority(irql);

  if (current_interrupt_priority_ == -1 && prio == -1) {
    return true;
  } else {
    return prio > current_interrupt_priority_;
  }
}

uint64_t XenonInterruptController::GetEOI() { return eoi_written_; }

}  // namespace cpu
}  // namespace xe
