/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_state.h"

#include <string>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_memory.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"
namespace xe {
namespace kernel {

struct KernelDebugStringBuffer : public StringBuffer {
  KernelState* const kernel_state_;
  static const char* ProcessTypeToString(uint8_t proctype);
  KernelDebugStringBuffer(KernelState* kernel_state)
      : kernel_state_(kernel_state) {
    this->Reserve(65536);
  }
};

static void DumpSpinlock(KernelState* ks, X_KSPINLOCK* lock,
                         KernelDebugStringBuffer* sbuffer) {
  uint32_t held_by_pcr = lock->pcr_of_owner;

  sbuffer->AppendFormat("(Owner = CPU {})", (held_by_pcr >> 12) & 0xF);
}

const char* KernelDebugStringBuffer::ProcessTypeToString(uint8_t proctype) {
  switch (proctype) {
    case X_PROCTYPE_IDLE:
      return "idle";
    case X_PROCTYPE_SYSTEM:
      return "system";
    case X_PROCTYPE_TITLE:
      return "title";
    default:
      xenia_assert(false);
      return "unknown";
  }
}

static void DumpProcess(KernelState* ks, X_KPROCESS* process,
                        KernelDebugStringBuffer* sbuffer) {
  sbuffer->Append("thread_list_spinlock = ");
  DumpSpinlock(ks, &process->thread_list_spinlock, sbuffer);
#define SIMPF(field_name)                           \
  sbuffer->AppendFormat("\n" #field_name " = {:X}", \
                        static_cast<uint32_t>(process->field_name))

  SIMPF(quantum);
  SIMPF(clrdataa_masked_ptr);
  SIMPF(thread_count);
  SIMPF(unk_18);
  SIMPF(unk_19);
  SIMPF(unk_1A);
  SIMPF(unk_1B);

  SIMPF(kernel_stack_size);
  SIMPF(tls_static_data_address);
  SIMPF(tls_data_size);

  SIMPF(tls_raw_data_size);
  SIMPF(tls_slot_size);
  SIMPF(is_terminating);
  SIMPF(process_type);
  sbuffer->AppendFormat("\nprocess_type = {}",
      KernelDebugStringBuffer::ProcessTypeToString(process->process_type));
}

static void DumpThread(KernelState* ks, X_KTHREAD* kthread,
                       KernelDebugStringBuffer* sbuffer) {}

}  // namespace kernel
}  // namespace xe
