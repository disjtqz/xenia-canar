/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Xenia Canary. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_GUEST_TIMER_LIST_H_
#define XENIA_KERNEL_UTIL_GUEST_TIMER_LIST_H_

#include "xenia/kernel/kernel_guest_structures.h"
#include "xenia/cpu/ppc/ppc_context.h"
namespace xe {
namespace kernel {
namespace util {

/*
    xboxkrnl uses a 32 entry table. each entry is an X_LIST_ENTRY which
    is X_KTIMER.table_bucket_entry
*/

struct X_TIMER_TABLE {
  util::X_TYPED_LIST<X_KTIMER, offsetof(X_KTIMER, table_bucket_entry)>
      buckets[32];

};

void InitializeTimerTable(uint32_t table_ptr, cpu::ppc::PPCContext* context);

//must be under dispatcher lock!
void TimersExpire(uint32_t table_ptr, uint32_t scratch_list, cpu::ppc::PPCContext* context);

void TimersComputeHighestExpiredIndex(uint32_t table_ptr, uint32_t ktimestamp_bundle,
                                      uint32_t& highest_bucket,
                                      uint8_t& lowest_bucket,
                                      cpu::ppc::PPCContext* context);

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_GUEST_TIMER_LIST_H_
