/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xmutant.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"

#include "xenia/cpu/processor.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
namespace xe {
namespace kernel {

XMutant::XMutant(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XMutant::XMutant() : XObject(kObjectType) {}

XMutant::~XMutant() {
  auto guest_mutant = guest_object<X_KMUTANT>();
  if (guest_mutant) {
    if (guest_mutant->header.wait_list_flink) {
      auto event_for = kernel_state()->FreeInternalHandle<threading::Mutant>(
          guest_mutant->header.wait_list_flink);
      guest_mutant->header.wait_list_flink = 0;
      if (event_for) {
        delete event_for;
      }
    }
  }
}

void XMutant::Initialize(bool initial_owner, X_OBJECT_ATTRIBUTES* attributes) {
  auto context = cpu::ThreadState::Get()->context();

  auto guest_globals = context->TranslateVirtual<KernelGuestGlobals*>(
      kernel_state()->GetKernelGuestGlobals());
  uint32_t guest_objptr = 0;
  X_STATUS create_status =
      xboxkrnl::xeObCreateObject(&guest_globals->ExMutantObjectType, attributes,
                                 sizeof(X_KMUTANT), &guest_objptr, context);

  xenia_assert(create_status == X_STATUS_SUCCESS);
  xenia_assert(guest_objptr != 0);

  auto guest_object = context->TranslateVirtual<X_KMUTANT*>(guest_objptr);
  xboxkrnl::xeKeInitializeMutant(guest_object, initial_owner, context);
  SetNativePointer(guest_objptr, false);

  auto event_for = threading::Mutant::Create(initial_owner);

  auto event_ptr = event_for.release();

  uint32_t intrnl_handle =
      kernel_state()->AllocateInternalHandle((void*)event_ptr);
  guest_object->header.wait_list_flink = intrnl_handle;
}

void XMutant::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  xe::FatalError("Unimplemented XMutant::InitializeNative");
}

X_STATUS XMutant::ReleaseMutant(uint32_t priority_increment, bool abandon,
                                bool wait) {
  auto context = cpu::ThreadState::Get()->context();

  uint32_t irql = kernel_state()->LockDispatcher(context);

  auto mutant = guest_object<X_KMUTANT>();
  auto event_for = kernel_state()->LookupInternalHandle<threading::Mutant>(
      mutant->header.wait_list_flink);
  event_for->Release();
  uint32_t v10 = mutant->header.signal_state;
  bool v12 = abandon == 0;
  auto kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  uint32_t v14 = kpcr->prcb_data.current_thread.m_ptr;
  int new_signalstate;
  if (v12) {
    if (mutant->owner != v14) {
      kernel_state()->UnlockDispatcher(context, irql);
      xenia_assert(false);

      return X_STATUS_UNSUCCESSFUL;
    }
    new_signalstate = mutant->header.signal_state + 1;
  } else {
    new_signalstate = 1;
    mutant->abandoned = 1;
  }
  mutant->header.signal_state = new_signalstate;

  if (new_signalstate == 1) {
    if (v10 <= 0) {
      util::XeRemoveEntryList(&mutant->unk_list, context);
    }
    mutant->owner = 0;
  }

  kernel_state()->UnlockDispatcher(context, irql);

  return v10;
}

X_STATUS XMutant::GetSignaledStatus(X_STATUS success_in) {
  if (success_in <= 63U) {
    auto km = guest_object<X_KMUTANT>();
    if (km->abandoned) {
      return X_STATUS_ABANDONED_WAIT_0 + success_in;
    }
  }
  return success_in;
}
bool XMutant::Save(ByteStream* stream) { return true; }

object_ref<XMutant> XMutant::Restore(KernelState* kernel_state,
                                     ByteStream* stream) {
  auto mutant = new XMutant();

  return object_ref<XMutant>(mutant);
}
xe::threading::WaitHandle* XMutant::GetWaitHandle() {
  X_KMUTANT* km = guest_object<X_KMUTANT>();

  return kernel_state()->LookupInternalHandle<threading::Mutant>(
      km->header.wait_list_flink);
}
void XMutant::WaitCallback() {
  auto context = cpu::ThreadState::Get()->context();

  auto v4 = context->TranslateVirtual(
      context->TranslateVirtualGPR<X_KPCR*>(context->r[13])
          ->prcb_data.current_thread);

  auto mutant = guest_object<X_KMUTANT>();

  uint32_t v20 = --mutant->header.signal_state;
  if (!v20) {
    mutant->owner = context->HostToGuestVirtual(v4);
    util::XeInsertHeadList(v4->mutants_list.blink_ptr, &mutant->unk_list,
                           context);
  }
}

}  // namespace kernel
}  // namespace xe
