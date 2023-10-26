/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xsemaphore.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"

namespace xe {
namespace kernel {

XSemaphore::XSemaphore(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XSemaphore::~XSemaphore() = default;

bool XSemaphore::Initialize(int32_t initial_count, int32_t maximum_count) {
  auto context = cpu::ThreadState::Get()->context();
  uint32_t guest_objptr = 0;
  auto guest_globals = context->TranslateVirtual<KernelGuestGlobals*>(
      kernel_state()->GetKernelGuestGlobals());
  X_STATUS create_status =
      xboxkrnl::xeObCreateObject(&guest_globals->ExSemaphoreObjectType, nullptr,
                                 sizeof(X_KSEMAPHORE), &guest_objptr, context);

  xenia_assert(create_status == X_STATUS_SUCCESS);
  xenia_assert(guest_objptr != 0);

  auto ksem = context->TranslateVirtual<X_KSEMAPHORE*>(guest_objptr);
  xboxkrnl::xeKeInitializeSemaphore(ksem, initial_count, maximum_count);
  SetNativePointer(guest_objptr);
  return true;
}

bool XSemaphore::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  return true;
}

int32_t XSemaphore::ReleaseSemaphore(int32_t release_count) {
  
    return xboxkrnl::xeKeReleaseSemaphore(cpu::ThreadState::GetContext(),
                                 guest_object<X_KSEMAPHORE>(), 1, release_count,
                                 0);
}

bool XSemaphore::Save(ByteStream* stream) {

  return true;
}

object_ref<XSemaphore> XSemaphore::Restore(KernelState* kernel_state,
                                           ByteStream* stream) {

  return object_ref<XSemaphore>(nullptr);
}

}  // namespace kernel
}  // namespace xe
