/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include "xenia/base/chrono.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XTimer::XTimer(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() {
    //delete_proc for X_KTIMER object type
  xboxkrnl::xeKeCancelTimer(cpu::ThreadState::GetContext(),
                            guest_object<X_KTIMER>());
}

void XTimer::Initialize(uint32_t timer_type) {
  auto context = cpu::ThreadState::Get()->context();
  uint32_t guest_objptr = 0;
  auto guest_globals = context->TranslateVirtual<KernelGuestGlobals*>(
      kernel_state()->GetKernelGuestGlobals());

  X_STATUS create_status =
      xboxkrnl::xeObCreateObject(&guest_globals->ExTimerObjectType, nullptr,
                                 sizeof(X_EXTIMER), &guest_objptr, context);
  xenia_assert(create_status == X_STATUS_SUCCESS);
  xenia_assert(guest_objptr != 0);

  auto guest_object = context->TranslateVirtual<X_EXTIMER*>(guest_objptr);
  xboxkrnl::xeKeInitializeExTimer(context, guest_object, timer_type);

  SetNativePointer(guest_objptr);
}

}  // namespace kernel
}  // namespace xe
