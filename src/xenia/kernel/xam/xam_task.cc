/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_modules.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace kernel {
namespace xam {

struct XTASK_MESSAGE {
  be<uint32_t> unknown_00;
  be<uint32_t> unknown_04;
  be<uint32_t> unknown_08;
  be<uint32_t> callback_arg_ptr;
  be<uint32_t> event_handle;
  be<uint32_t> unknown_14;
  be<uint32_t> task_handle;
};

struct XAM_TASK_ARGS {
  be<uint32_t> flags;
  be<uint32_t> value2;
  // i think there might be another value here, it might be padding
};
static_assert_size(XTASK_MESSAGE, 0x1C);

// handles defaults of various bitfields
static uint32_t unk_encode(uint32_t flags) {
  auto v1 = flags;
  if ((v1 & 0x1F) == 0) {
    flags = v1 | 4;
  }
  auto v2 = flags;
  if ((v2 & 0x700000) == 0) {
    flags = v2 | 0x200000;
  }
  auto v3 = flags;
  if ((v3 & 0x880000) == 0) {
    flags = v3 | 0x800000;
  }
  auto v4 = flags;
  if ((v4 & 0x6000000) == 0) {
    if ((v4 & 0x10) != 0) {
      flags = v4 | 0x4000000;
    } else {
      // todo: a lot more happens here!
      flags |= 0x4000000u;
    }
  }
  return flags;
}
static uint32_t g_thread_incrementer = 0;

static uint32_t get_cpunum_from_arg1(uint32_t dword8) {
  auto v1 = dword8 & 0xF1000000;
  switch (v1) {
    case 0x10000000u:
      return 5;
    case 0x20000000u:
      return (g_thread_incrementer++ & 1) + 3;
    case 0x40000000u:
    case 0x80000000:
      return 2;
  }
  return 5;
}
/*
    used for XamTaskSchedule, but on initialization Xam also calls this to create 8 different threads. 
    two seem to be for specific tasks, and the other 6 probably execute pooled tasks (task type 4, used by h4)
*/
static X_KTHREAD* XamThreadCreate(PPCContext* context, uint32_t arg1_from_flags,
                         uint32_t callback, uint32_t message, XAM_TASK_ARGS* optional_args=nullptr) {
  uint32_t dword8 = arg1_from_flags;
  xe::be<uint32_t> kthreaad_u;

  uint32_t create_result = xboxkrnl::ExCreateThread(
      &kthreaad_u, 65536, nullptr, 0, callback, message,
      XE_FLAG_THREAD_INITIALLY_SUSPENDED | XE_FLAG_RETURN_KTHREAD_PTR |
          XE_FLAG_SYSTEM_THREAD);

  auto resulting_kthread = context->TranslateVirtual<X_KTHREAD*>(kthreaad_u);

  if (XFAILED(create_result)) {
    // Failed!
    XELOGE("XAM task creation failed: {:08X}", create_result);
    xboxkrnl::xeKeLeaveCriticalRegion(context);
    return nullptr;
  }
  uint32_t cpunum;
  if (optional_args && optional_args->flags & 0x80000000) {
    cpunum = optional_args->value2;
  } else {
    cpunum = get_cpunum_from_arg1(arg1_from_flags);
  }

  if (arg1_from_flags & 0x80000) {
    xboxkrnl::xeKeSetPriorityClassThread(context, resulting_kthread, 1);
  }

  xboxkrnl::xeKeSetBasePriorityThread(context, resulting_kthread, 0);
  uint32_t old_aff;
  xboxkrnl::xeKeSetAffinityThread(context, resulting_kthread, 1U << cpunum,
                                  &old_aff);

  xboxkrnl::xeKeResumeThread(context, resulting_kthread);
  return resulting_kthread;
}

dword_result_t XamTaskSchedule_entry(lpvoid_t callback,
                                     pointer_t<XTASK_MESSAGE> message,
                                     dword_t optional_ptr, lpdword_t handle_ptr,
                                     const ppc_context_t& ctx) {
  xboxkrnl::xeKeEnterCriticalRegion(ctx);
  // TODO(gibbed): figure out what this is for
  *handle_ptr = 12345;

  XAM_TASK_ARGS args{};

  if (optional_ptr) {
    auto option = ctx->TranslateVirtual<XAM_TASK_ARGS*>(optional_ptr);

    args = *option;
    auto v1 = option->flags;
    auto v2 = option->value2;  // typically 0?

    XELOGI("Got xam task args: v1 = {:08X}, v2 = {:08X}", v1, v2);
  } else {
    args.flags = 0;
    args.value2 = 0;
  }

  args.flags = unk_encode(args.flags);

  if ((args.flags & 0x80000) != 0 && (args.flags & 0x20000000) == 0 &&
      !xboxkrnl::xeXexCheckExecutablePrivilege(1)) {
    args.flags &= 0xFFF7FFFF;
  }

  uint32_t what = args.flags & 0x1F;
  if (what == 2) {
    uint32_t arg1_from_flags =
        xe::rotate_right<uint32_t>(1, 4) & 0xFF07FFFF | args.flags & 0xF80000;

    X_KTHREAD* resulting_kthread =
        XamThreadCreate(ctx, arg1_from_flags, callback, message, &args);
    if (!resulting_kthread) {
      return X_STATUS_UNSUCCESSFUL;
    }

    // this is done in the destructor of the thread param in xam
    xboxkrnl::xeObDereferenceObject(ctx,
                                    ctx->HostToGuestVirtual(resulting_kthread));

    xboxkrnl::NtClose(XThread::FromGuest(resulting_kthread)->handle());
  } 
  else if (what == 4) {
    //pooled task? goes on the xam worker threads i think
    xenia_assert(false && "pooled tasks are unsupported");
  }
  else {
    xenia_assert(false);  // unhandled task type
  }

  XELOGD("XAM task ({:08X}) scheduled asynchronously",
         callback.guest_address());
  xboxkrnl::xeKeLeaveCriticalRegion(ctx);
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamTaskSchedule, kNone, kImplemented, kSketchy);

dword_result_t XamTaskShouldExit_entry(dword_t r3) { return 0; }
DECLARE_XAM_EXPORT2(XamTaskShouldExit, kNone, kStub, kSketchy);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Task);
