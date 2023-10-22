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

namespace xe {
namespace cpu {
class HWThread;
class Processor;

struct ExternalInterruptArgs {
  uint32_t source_;
};
/*
    todo: can't LLE this, because the MMIO handler does not support 8-byte loads
   and stores, and all accesses to this are 8 bytes
*/
class XenonInterruptController {
  const uint32_t cpu_number_;
  uint32_t pad_;
  HWThread* const owner_;
  Processor* const processor_;
  // technically has a whole page, but I think only a bit of it is used
  union {
    struct {
      uint64_t unk_0;   // 0x0
      uint64_t unk_8;   // 0x8
      uint64_t unk_10;  // 0x10
      uint64_t unk_18;  // 0x18
      uint64_t unk_20;  // 0x20
      uint64_t unk_28;  // 0x28
      uint64_t unk_30;  // 0x30
      uint64_t unk_38;  // 0x38
      uint64_t unk_40;  // 0x40
      uint64_t unk_48;  // 0x48
      uint64_t unk_50;  // 0x50
      uint64_t unk_58;  // 0x58
      uint64_t unk_60;  // 0x60
      uint64_t unk_68;  // 0x68
      uint64_t unk_70;  // 0x70
    };
    uint64_t data_[32];  // 0x100 bytes
  };

  void Initialize();

 public:
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
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_XENON_INTERRUPT_CONTROLLER_H_
