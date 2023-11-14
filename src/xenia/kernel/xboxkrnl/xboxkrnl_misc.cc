/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"
namespace xe {
namespace kernel {
namespace xboxkrnl {

void KeEnableFpuExceptions_entry(
    const ppc_context_t& ctx) {  // dword_t enabled) {
  // TODO(benvanik): can we do anything about exceptions?
  // theres a lot more thats supposed to happen here, the floating point state
  // has to be saved to kthread, the irql changes, the machine state register is
  // changed to enable exceptions

  auto old_irql = GetKPCR(ctx)->current_irql;
  GetKPCR(ctx)->current_irql = 2;

  GetKThread(ctx)->fpu_exceptions_on = static_cast<uint32_t>(ctx->r[3]) != 0;

  xboxkrnl::xeKfLowerIrql(ctx, old_irql);
}
DECLARE_XBOXKRNL_EXPORT1(KeEnableFpuExceptions, kNone, kStub);

//these are not properly implemented
void KeSaveFloatingPointState_entry(ppc_context_t& ctx) {
  auto kthread = GetKThread(ctx);

  for (unsigned i = 0; i < 32; ++i) {
    kthread->fpu_context[i] = ctx->f[i];
  }
}

DECLARE_XBOXKRNL_EXPORT1(KeSaveFloatingPointState, kNone, kImplemented);

void KeRestoreFloatingPointState_entry(ppc_context_t& ctx) {
  auto kthread = GetKThread(ctx);

  for (unsigned i = 0; i < 32; ++i) {
    ctx->f[i] = kthread->fpu_context[i];
  }
}

DECLARE_XBOXKRNL_EXPORT1(KeRestoreFloatingPointState, kNone, kImplemented);

static qword_result_t KeQueryInterruptTime_entry(const ppc_context_t& ctx) {
  auto kstate = ctx->kernel_state;
  uint32_t ts_bundle = kstate->GetKeTimestampBundle();
  X_TIME_STAMP_BUNDLE* bundle =
      ctx->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(ts_bundle);

  uint64_t int_time = bundle->interrupt_time;
  return int_time;
}
DECLARE_XBOXKRNL_EXPORT1(KeQueryInterruptTime, kNone, kImplemented);
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Misc);
