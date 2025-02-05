/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_RTL_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_RTL_H_

#include "xenia/kernel/kernel_guest_structures.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

void xeRtlInitializeCriticalSection(X_RTL_CRITICAL_SECTION* cs,
                                    uint32_t cs_ptr);
X_STATUS xeRtlInitializeCriticalSectionAndSpinCount(X_RTL_CRITICAL_SECTION* cs,
                                                    uint32_t cs_ptr,
                                                    uint32_t spin_count);
void xeRtlEnterCriticalSection(cpu::ppc::PPCContext* context,
                               X_RTL_CRITICAL_SECTION* cs);
uint32_t xeRtlTryEnterCriticalSection(cpu::ppc::PPCContext* context,
                                      X_RTL_CRITICAL_SECTION* cs);
void xeRtlLeaveCriticalSection(cpu::ppc::PPCContext* context,
                               X_RTL_CRITICAL_SECTION* cs);
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_RTL_H_
