/**
 ******************************************************************************
 * Xenia Canary : Xbox 360 Emulator Research Project *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_COMPILER_PASSES_INTERRUPT_CHECK_INJECTION_PASS_H_
#define XENIA_CPU_COMPILER_PASSES_INTERRUPT_CHECK_INJECTION_PASS_H_

#include "xenia/cpu/compiler/compiler_pass.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

class InterruptInjectionPass : public CompilerPass {
 public:
  InterruptInjectionPass();
  ~InterruptInjectionPass() override;

  bool Run(hir::HIRBuilder* builder) override;
};

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_COMPILER_PASSES_INTERRUPT_CHECK_INJECTION_PASS_H_
