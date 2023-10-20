/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Xenia Canary. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/util/guest_timer_list.h"
#include "xenia/base/atomic.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_memory.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
namespace xe {
namespace kernel {
namespace util {

void InitializeTimerTable(uint32_t table_ptr, cpu::ppc::PPCContext* context) {
  X_TIMER_TABLE* table = context->TranslateVirtual<X_TIMER_TABLE*>(table_ptr);
  for (uint32_t i = 0; i < countof(table->buckets); ++i) {
    table->buckets[i].Initialize(context);
  }
}

void TimersExpire(uint32_t table_ptr, uint32_t scratch_list,
                  cpu::ppc::PPCContext* context) {
  X_TIMER_TABLE* table = context->TranslateVirtual<X_TIMER_TABLE*>(table_ptr);
  X_LIST_ENTRY* expired_head =
      context->TranslateVirtual<X_LIST_ENTRY*>(scratch_list);
  util::XeInitializeListHead(expired_head, scratch_list);
}

void TimersComputeHighestExpiredIndex(uint32_t table_ptr,
                                      uint32_t ktimestamp_bundle,
                                      uint32_t& highest_bucket,
                                      uint8_t& lowest_bucket,
                                      cpu::ppc::PPCContext* context) {}

}  // namespace util
}  // namespace kernel
}  // namespace xe
