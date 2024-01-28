/**
 ******************************************************************************
 * Xenia Canary : Xbox 360 Emulator Research Project *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/compiler/passes/interrupt_check_injection_pass.h"

#include "xenia/base/assert.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/compiler/compiler.h"
#include "xenia/cpu/processor.h"

namespace xe {
namespace cpu {
namespace compiler {
namespace passes {

using namespace xe::cpu::hir;

InterruptInjectionPass::InterruptInjectionPass() : CompilerPass() {}

InterruptInjectionPass::~InterruptInjectionPass() {}

bool InterruptInjectionPass::Run(HIRBuilder* builder) {
  bool added_interrupt_checks = false;
  // add interrupt checks to the front of each block
  for (auto block = builder->first_block(); block != nullptr;
       block = block->next) {
    auto first_nonfake = block->instr_head;
    for (; first_nonfake && first_nonfake->IsFake();
         first_nonfake = first_nonfake->next) {
    }

    if (first_nonfake &&
        first_nonfake->GetOpcodeNum() != OPCODE_CHECK_INTERRUPT) {
      auto interrupt_instruction = builder->CheckInterrupt();

      interrupt_instruction->MoveBefore(first_nonfake);
      added_interrupt_checks = true;
    }
  }
  return added_interrupt_checks;
}

}  // namespace passes
}  // namespace compiler
}  // namespace cpu
}  // namespace xe
