/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XMUTANT_H_
#define XENIA_KERNEL_XMUTANT_H_

#include "xenia/base/threading.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
struct X_KTHREAD;
struct X_KMUTANT {
  X_DISPATCH_HEADER header;             //0x0
  X_LIST_ENTRY unk_list;                //0x10
  TypedGuestPointer<X_KTHREAD> owner;   //0x18
  bool abandoned;                       //0x1C
  //these might just be padding 
  uint8_t unk_1D;                       //0x1D
  uint8_t unk_1E;                       //0x1E
  uint8_t unk_1F;                       //0x1F
};
static_assert_size(X_KMUTANT, 0x20);

class XThread;

class XMutant : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Mutant;

  explicit XMutant(KernelState* kernel_state);
  ~XMutant() override;

  void Initialize(bool initial_owner, X_OBJECT_ATTRIBUTES* attributes);
  void InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header);

  X_STATUS ReleaseMutant(uint32_t priority_increment, bool abandon, bool wait);

  bool Save(ByteStream* stream) override;
  static object_ref<XMutant> Restore(KernelState* kernel_state,
                                     ByteStream* stream);

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override;
  void WaitCallback() override;
  virtual X_STATUS GetSignaledStatus(X_STATUS success_in) override;
 private:
  XMutant();
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XMUTANT_H_
