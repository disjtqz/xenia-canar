/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_THREAD_STATE_H_
#define XENIA_CPU_THREAD_STATE_H_

#include <string>

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

class Processor;

class ThreadState {
 public:

  ~ThreadState();

  void operator delete(void* vp);

  ppc::PPCContext* context() const {
    return reinterpret_cast<ppc::PPCContext*>(const_cast<ThreadState*>(this));
  }

  static void Bind(ThreadState* thread_state);
  static ThreadState* Get();
  static uint32_t GetThreadID();
  XE_NOALIAS
  static ppc::PPCContext* GetContext();

  static ThreadState* Create(Processor* processor, uint32_t thread_id,
                             uint32_t stack_base = 0, uint32_t pcr_address = 0);
  
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_THREAD_STATE_H_
