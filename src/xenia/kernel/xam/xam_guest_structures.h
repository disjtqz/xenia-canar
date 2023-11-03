/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Xenia Canary. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_GUEST_STRUCTURES_H_
#define XENIA_KERNEL_XAM_XAM_GUEST_STRUCTURES_H_
#include "xenia/kernel/kernel_guest_structures.h"
namespace xe {
namespace kernel {
struct X_XAMNOTIFY {
  char field_0[12];
  X_KEVENT event;
  X_KSPINLOCK spinlock;
  char an_irql;
  char field_21[3];
  int a_pointer;
  int field_28;
  int process_type_related;
};
static_assert_size(X_XAMNOTIFY, 48);

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_GUEST_STRUCTURES_H_
