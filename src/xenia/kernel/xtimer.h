/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XTIMER_H_
#define XENIA_KERNEL_XTIMER_H_

#include "xenia/base/threading.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/kernel_guest_structures.h"

namespace xe {
namespace kernel {

class XThread;

class XTimer : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Timer;

  explicit XTimer(KernelState* kernel_state);
  ~XTimer() override;

  void Initialize(uint32_t timer_type);

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override { return nullptr; }

 private:

};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTIMER_H_
