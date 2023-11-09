/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Xenia Canary. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_XENON_INTERRUPT_CONTROLLER_H_
#define XENIA_CPU_XENON_INTERRUPT_CONTROLLER_H_
#include "xenia/base/memory.h"
#include "xenia/base/threading.h"
namespace xe {
namespace cpu {
class HWThread;
class Processor;
class XenonInterruptController;
namespace ppc {
struct PPCInterruptRequest;
}
struct ExternalInterruptArgs {
  XenonInterruptController* controller_;
  uint32_t source_;
};
static constexpr uint32_t MAX_CPU_TIMED_INTERRUPTS = 4;
using CpuTimedInterruptProc = void (*)(XenonInterruptController* controller, uint32_t slot);
struct CpuTimedInterrupt {
    //time in nanoseconds that the event should be triggered at
  uint64_t destination_nanoseconds_;
  CpuTimedInterruptProc enqueue_;
};

/*
    todo: can't LLE this, because the MMIO handler does not support 8-byte loads
   and stores, and all accesses to this are 8 bytes
*/
class XenonInterruptController {
 public:
  threading::AtomicListHeader queued_interrupts_;
  uint64_t next_event_quick_timestamp_ = ~0ULL;

  // technically has a whole page, but I think only a little bit of it (0x100) is used. at least, from kernel space
  union {
    struct {
      uint64_t unk_0;  // 0x0
      // only interrupts with a higher irql than current_irql may be triggered
      uint64_t current_irql;  // 0x8

      // low 16 bits = value that gets passed to the cpu we're signalling
      // xbox kernel uses it to encode an absolute byte offset to the entry in
      // the KPCR's interrupts array high 16 bits = bitmask of cpus to send the
      // interrupt to
      uint64_t ipi_signal;  // 0x10
      uint64_t unk_18;      // 0x18
      uint64_t unk_20;      // 0x20
      uint64_t unk_28;      // 0x28
      uint64_t unk_30;      // 0x30
      uint64_t unk_38;      // 0x38
      uint64_t unk_40;      // 0x40
      uint64_t unk_48;      // 0x48
      uint64_t unk_50;      // 0x50
      uint64_t unk_58;      // 0x58
      uint64_t unk_60;      // 0x60
      // writing a value to this marks the end of the interrupt + sets
      // current_irql
      uint64_t eoi_irql;  // 0x68
      uint64_t unk_70;    // 0x70
    };
    uint64_t data_[32];  // 0x100 bytes
  };

 private:
  const uint32_t cpu_number_;
  uint32_t pad_;
  HWThread* const owner_;
  Processor* const processor_;
  uint64_t tick_nanosecond_frequency_;
  threading::AtomicListHeader free_interrupt_requests_;

  uint32_t timed_event_slots_bitmap_=0;
  CpuTimedInterrupt timed_events_[4];
  void Initialize();

  void SetInterruptSource(uint64_t src);
  static void InterruptFunction(void* ud);

 public:

  ppc::PPCInterruptRequest* AllocateInterruptRequest();
  void FreeInterruptRequest(ppc::PPCInterruptRequest* request);
  XenonInterruptController(HWThread* thread, Processor* processor);
  ~XenonInterruptController();
  // the address is normally calculated by setting the top bits
  // of KPCR to 0x7FFF
  // kpcr's low bits are always 0 except for the nibble starting at bit 12,
  // which contains the hw thread number
  uint32_t GuestMMIOAddress() const;

  void SendExternalInterrupt(ExternalInterruptArgs& args);

  void WriteRegisterOffset(uint32_t offset, uint64_t value);
  uint64_t ReadRegisterOffset(uint32_t offset);

  uint32_t AllocateTimedInterruptSlot();
  void FreeTimedInterruptSlot(uint32_t slot);
  void SetTimedInterruptArgs(uint32_t slot, CpuTimedInterrupt* data);

  void RecomputeNextEventCycles();
  void EnqueueTimedInterrupts();

};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_XENON_INTERRUPT_CONTROLLER_H_
