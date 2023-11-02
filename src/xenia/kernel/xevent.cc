/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xevent.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"

namespace xe {
namespace kernel {

XEvent::XEvent(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XEvent::~XEvent() {}

void XEvent::Initialize(bool manual_reset, bool initial_state) {
  auto context = cpu::ThreadState::Get()->context();

  auto guest_globals = context->TranslateVirtual<KernelGuestGlobals*>(
      kernel_state()->GetKernelGuestGlobals());
  uint32_t guest_objptr = 0;
  // todo: attributes
  X_STATUS create_status =
      xboxkrnl::xeObCreateObject(&guest_globals->ExEventObjectType, nullptr,
                                 sizeof(X_KEVENT), &guest_objptr, context);
  xenia_assert(create_status == X_STATUS_SUCCESS);
  xenia_assert(guest_objptr != 0);

  auto guest_object = context->TranslateVirtual<X_KEVENT*>(guest_objptr);

  guest_object->header.type = !manual_reset;
  guest_object->header.signal_state = initial_state;
  util::XeInitializeListHead(&guest_object->header.wait_list, context);
  SetNativePointer(guest_objptr);
}

void XEvent::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {


  SetNativePointer(cpu::ThreadState::GetContext()->HostToGuestVirtual(header));
}

int32_t XEvent::Set(uint32_t priority_increment, bool wait) {
  xboxkrnl::xeKeSetEvent(cpu::ThreadState::GetContext(),
                         guest_object<X_KEVENT>(), 0, wait);
  return 1;
}

int32_t XEvent::Reset() {
  xboxkrnl::xeKeResetEvent(cpu::ThreadState::GetContext(),
                           guest_object<X_KEVENT>());
  return 1;
}
void XEvent::Query(uint32_t* out_type, uint32_t* out_state) {
  xenia_assert(false);
}
void XEvent::Clear() { Reset(); }

bool XEvent::Save(ByteStream* stream) {
  xenia_assert(false);

  return true;
}

object_ref<XEvent> XEvent::Restore(KernelState* kernel_state,
                                   ByteStream* stream) {
  xenia_assert(false);

  return object_ref<XEvent>(nullptr);
}

}  // namespace kernel
}  // namespace xe
