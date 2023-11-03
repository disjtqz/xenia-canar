/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xam/xam_guest_structures.h"

#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
namespace xe {
namespace kernel {

XNotifyListener::XNotifyListener(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XNotifyListener::~XNotifyListener() {}
X_XAMNOTIFY* XNotifyListener::Get() { return guest_object<X_XAMNOTIFY>(); }
void XNotifyListener::Initialize(uint64_t mask, uint32_t max_version) {
  auto context = cpu::ThreadState::Get()->context();
  uint32_t guest_objptr = 0;
  auto guest_globals = context->TranslateVirtual<KernelGuestGlobals*>(
      kernel_state()->GetKernelGuestGlobals());
  X_STATUS create_status = xboxkrnl::xeObCreateObject(
      &guest_globals->XamNotifyListenerObjectType, nullptr, sizeof(X_XAMNOTIFY),
      &guest_objptr, context);

  xenia_assert(create_status == X_STATUS_SUCCESS);
  xenia_assert(guest_objptr != 0);

  auto ksem = context->TranslateVirtual<X_XAMNOTIFY*>(guest_objptr);

  ksem->event.header.type = 1;
  ksem->event.header.signal_state = 0;
  util::XeInitializeListHead(&ksem->event.header.wait_list, context);
  ksem->process_type_related =
      -2 - ((xboxkrnl::xeKeGetCurrentProcessType(context) == 2) - 3);
  ksem->spinlock.pcr_of_owner = 0;

  SetNativePointer(guest_objptr);
  mask_ = mask;
  max_version_ = max_version;

  kernel_state_->RegisterNotifyListener(this);
}

void XNotifyListener::EnqueueNotification(XNotificationID id, uint32_t data) {
  auto key = XNotificationKey(id);
  // Ignore if the notification doesn't match our mask.
  if ((mask_ & uint64_t(1ULL << key.mask_index)) == 0) {
    return;
  }
  // Ignore if the notification is too new.
  if (key.version > max_version_) {
    return;
  }
  auto thiz = Get();
  auto context = cpu::ThreadState::GetContext();

  thiz->an_irql = xboxkrnl::xeKeKfAcquireSpinLock(context, &thiz->spinlock);

  notifications_.push_back(std::pair<XNotificationID, uint32_t>(id, data));

  xboxkrnl::xeKeSetEvent(context, &thiz->event, 1, 0);
  xboxkrnl::xeKeKfReleaseSpinLock(context, &thiz->spinlock, thiz->an_irql);
}

bool XNotifyListener::DequeueNotification(XNotificationID* out_id,
                                          uint32_t* out_data) {
  auto thiz = Get();
  auto context = cpu::ThreadState::GetContext();
  thiz->an_irql = xboxkrnl::xeKeKfAcquireSpinLock(context, &thiz->spinlock);

  bool dequeued = false;
  if (notifications_.size()) {
    dequeued = true;
    auto it = notifications_.begin();
    *out_id = it->first;
    *out_data = it->second;
    notifications_.erase(it);
    if (!notifications_.size()) {
      // inlined clearevent? original XNotifyGetNext does this
      thiz->event.header.signal_state = 0;
    }
  }

  xboxkrnl::xeKeKfReleaseSpinLock(context, &thiz->spinlock, thiz->an_irql);
  return dequeued;
}

bool XNotifyListener::DequeueNotification(XNotificationID id,
                                          uint32_t* out_data) {
  auto thiz = Get();
  auto context = cpu::ThreadState::GetContext();
  thiz->an_irql = xboxkrnl::xeKeKfAcquireSpinLock(context, &thiz->spinlock);
  if (!notifications_.size()) {
    xboxkrnl::xeKeKfReleaseSpinLock(context, &thiz->spinlock, thiz->an_irql);
    return false;
  }
  bool dequeued = false;
  for (auto it = notifications_.begin(); it != notifications_.end(); ++it) {
    if (it->first != id) {
      continue;
    }
    dequeued = true;
    *out_data = it->second;
    notifications_.erase(it);
    if (!notifications_.size()) {
      thiz->event.header.signal_state = 0;
    }
    break;
  }
  xboxkrnl::xeKeKfReleaseSpinLock(context, &thiz->spinlock, thiz->an_irql);
  return dequeued;
}

bool XNotifyListener::Save(ByteStream* stream) {
  SaveObject(stream);

  stream->Write(mask_);
  stream->Write(max_version_);
  stream->Write(notifications_.size());
  for (auto pair : notifications_) {
    stream->Write<uint32_t>(pair.first);
    stream->Write<uint32_t>(pair.second);
  }

  return true;
}

object_ref<XNotifyListener> XNotifyListener::Restore(KernelState* kernel_state,
                                                     ByteStream* stream) {
  auto notify = new XNotifyListener(nullptr);
  notify->kernel_state_ = kernel_state;

  notify->RestoreObject(stream);

  auto mask = stream->Read<uint64_t>();
  auto max_version = stream->Read<uint32_t>();
  notify->Initialize(mask, max_version);

  auto notification_count_ = stream->Read<size_t>();
  for (size_t i = 0; i < notification_count_; i++) {
    std::pair<XNotificationID, uint32_t> pair;
    pair.first = stream->Read<uint32_t>();
    pair.second = stream->Read<uint32_t>();
    notify->notifications_.push_back(pair);
  }

  return object_ref<XNotifyListener>(notify);
}

}  // namespace kernel
}  // namespace xe
