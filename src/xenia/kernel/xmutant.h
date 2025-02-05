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
#include "xenia/kernel/kernel_guest_structures.h"

namespace xe {
namespace kernel {

class XThread;

class XMutant : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Mutant;

  explicit XMutant(KernelState* kernel_state);
  ~XMutant() override;

  void Initialize(bool initial_owner, X_OBJECT_ATTRIBUTES* attributes);
  void InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header);

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
