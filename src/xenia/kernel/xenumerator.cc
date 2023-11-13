/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
namespace xe {
namespace kernel {

XEnumerator::XEnumerator(KernelState* kernel_state, size_t items_per_enumerate,
                         size_t item_size)
    : XObject(kernel_state, kObjectType),
      items_per_enumerate_(items_per_enumerate),
      item_size_(item_size) {}

XEnumerator::~XEnumerator() = default;

X_STATUS XEnumerator::Initialize(uint32_t user_index, uint32_t app_id,
                                 uint32_t open_message, uint32_t close_message,
                                 uint32_t flags, uint32_t extra_size,
                                 void** extra_buffer) {
  auto context = cpu::ThreadState::Get()->context();
  uint32_t guest_objptr = 0;
  auto guest_globals = context->TranslateVirtual<KernelGuestGlobals*>(
      kernel_state()->GetKernelGuestGlobals());
  X_STATUS create_status = xboxkrnl::xeObCreateObject(
      &guest_globals->XamEnumeratorObjectType, nullptr,
      sizeof(X_KENUMERATOR) + extra_size, &guest_objptr, context);

  xenia_assert(create_status == X_STATUS_SUCCESS);
  xenia_assert(guest_objptr != 0);
  SetNativePointer(guest_objptr);

  auto guest_object = context->TranslateVirtual<X_KENUMERATOR*>(guest_objptr);
  guest_object->app_id = app_id;
  guest_object->open_message = open_message;
  guest_object->close_message = close_message;
  guest_object->user_index = user_index;
  guest_object->items_per_enumerate =
      static_cast<uint32_t>(items_per_enumerate_);
  guest_object->flags = flags;
  if (extra_buffer) {
    *extra_buffer =
        !extra_buffer ? nullptr : &guest_object[sizeof(X_KENUMERATOR)];
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XEnumerator::Initialize(uint32_t user_index, uint32_t app_id,
                                 uint32_t open_message, uint32_t close_message,
                                 uint32_t flags) {
  return Initialize(user_index, app_id, open_message, close_message, flags, 0,
                    nullptr);
}

uint8_t* XStaticUntypedEnumerator::AppendItem() {
  size_t offset = buffer_.size();
  buffer_.resize(offset + item_size());
  item_count_++;
  return const_cast<uint8_t*>(&buffer_.data()[offset]);
}

uint32_t XStaticUntypedEnumerator::WriteItems(uint32_t buffer_ptr,
                                              uint8_t* buffer_data,
                                              uint32_t* written_count) {
  size_t count = std::min(item_count_ - current_item_, items_per_enumerate());
  if (!count) {
    return X_ERROR_NO_MORE_FILES;
  }

  size_t size = count * item_size();
  size_t offset = current_item_ * item_size();
  std::memcpy(buffer_data, buffer_.data() + offset, size);

  current_item_ += count;

  if (written_count) {
    *written_count = static_cast<uint32_t>(count);
  }

  return X_ERROR_SUCCESS;
}

}  // namespace kernel
}  // namespace xe
