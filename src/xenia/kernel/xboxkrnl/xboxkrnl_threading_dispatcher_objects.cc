/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Xenia Canary. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <vector>
#include "xenia/base/atomic.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xmutant.h"
#include "xenia/kernel/xsemaphore.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xtimer.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

int32_t xeKeSetEvent(PPCContext* context, X_KEVENT* event, int increment,
                     unsigned char wait) {
  xenia_assert(event && event->header.type < 2);
  uint32_t old_irql = context->kernel_state->LockDispatcher(context);

  auto old_signalstate = event->header.signal_state;
  auto wait_list = context->TranslateVirtual<X_KWAIT_BLOCK*>(
      event->header.wait_list.flink_ptr);

  if (&wait_list->wait_list_entry == &event->header.wait_list) {
    // no waiters, just set signalstate
    event->header.signal_state = 1;
  } else if (event->header.type != 0 && wait_list->wait_type == WAIT_ANY) {
    xeEnqueueThreadPostWait(context,
                            context->TranslateVirtual(wait_list->thread),
                            wait_list->wait_result_xstatus, increment);
  } else if (!old_signalstate) {
    event->header.signal_state = 1;
    xeDispatchSignalStateChange(context, &event->header, increment);
  }
  if (wait) {
    auto current_thread =
        context->TranslateVirtual(GetKPCR(context)->prcb_data.current_thread);
    current_thread->unk_A6 = wait;
    current_thread->unk_A4 = old_irql;
  } else {
    xeDispatcherSpinlockUnlock(
        context, context->kernel_state->GetDispatcherLock(context), old_irql);
  }
  return old_signalstate;
}

int32_t xeKeResetEvent(PPCContext* context, X_KEVENT* event) {
  xenia_assert(event && event->header.type < 2);
  auto old_irql = context->kernel_state->LockDispatcher(context);
  int32_t old_signal_state = event->header.signal_state;

  event->header.signal_state = 0;

  // is this really necessary? i thought this function was only used when a
  // thread may be unwaited, but is uses it in resetevent
  xeDispatcherSpinlockUnlock(
      context, context->kernel_state->GetDispatcherLock(context), old_irql);
  return old_signal_state;
}

int32_t xeKeReleaseMutant(PPCContext* context, X_KMUTANT* mutant, int unk,
                          bool abandoned, unsigned char unk2) {
  auto old_irql = context->kernel_state->LockDispatcher(context);
  int32_t old_signal_state = mutant->header.signal_state;
  int32_t new_signal_state;
  auto current_thread =
      context->TranslateVirtual(GetKPCR(context)->prcb_data.current_thread);

  if (!abandoned) {
    if (context->TranslateVirtual(mutant->owner) != current_thread) {
      xeDispatcherSpinlockUnlock(
          context, context->kernel_state->GetDispatcherLock(context), old_irql);
      xe::FatalError("We don't own the mutant, but we're releasing it!");
      return -1;
    }
    new_signal_state = old_signal_state + 1;
  } else {
    new_signal_state = 1;
    mutant->abandoned = 1;
  }

  mutant->header.signal_state = new_signal_state;
  if (new_signal_state == 1) {
    if (old_signal_state <= 0) {
      util::XeRemoveEntryList(&mutant->unk_list, context);
    }
    mutant->owner = 0U;
    if (!util::XeIsListEmpty(&mutant->header.wait_list, context)) {
      xeDispatchSignalStateChange(context, &mutant->header, unk);
    }
  }

  if (unk2) {
    current_thread->unk_A6 = unk2;
    current_thread->unk_A4 = old_irql;

  } else {
    xeDispatcherSpinlockUnlock(
        context, context->kernel_state->GetDispatcherLock(context), old_irql);
  }
  return old_signal_state;
}

int32_t xeKeReleaseSemaphore(PPCContext* context, X_KSEMAPHORE* semaphore,
                             int unk, int unk2, unsigned char unk3) {
  auto old_irql = context->kernel_state->LockDispatcher(context);
  int32_t old_signal_state = semaphore->header.signal_state;

  auto current_thread =
      context->TranslateVirtual(GetKPCR(context)->prcb_data.current_thread);

  int32_t new_signal_state = old_signal_state + unk2;

  if (new_signal_state > semaphore->limit ||
      new_signal_state < old_signal_state) {
    xeDispatcherSpinlockUnlock(
        context, context->kernel_state->GetDispatcherLock(context), old_irql);
    // should RtlRaiseStatus
    xenia_assert(false);
    return -1;
  }

  semaphore->header.signal_state = new_signal_state;

  if (!old_signal_state &&
      !util::XeIsListEmpty(&semaphore->header.wait_list, context)) {
    xeDispatchSignalStateChange(context, &semaphore->header, unk);
  }

  if (unk3) {
    current_thread->unk_A6 = unk3;
    current_thread->unk_A4 = old_irql;

  } else {
    xeDispatcherSpinlockUnlock(
        context, context->kernel_state->GetDispatcherLock(context), old_irql);
  }
  return old_signal_state;
}

int xeKeSetTimerEx(PPCContext* context, X_KTIMER* timer, int64_t duetime,
                   int period, XDPC* dpc) {
  auto old_irql = context->kernel_state->LockDispatcher(context);
  auto was_inserted = timer->header.inserted;

  if (was_inserted) {
    timer->header.inserted = 0;
    util::XeRemoveEntryList(&timer->table_bucket_entry, context);
  }

  timer->header.signal_state = 0;
  timer->dpc = context->HostToGuestVirtual(dpc);
  timer->period = period;
  if (!XeInsertGlobalTimer(context, timer, duetime)) {
    if (!util::XeIsListEmpty(&timer->header.wait_list, context)) {
      xeDispatchSignalStateChange(context, &timer->header, 0);
    }
    if (dpc) {
      auto systime = context->kernel_state->GetKernelSystemTime();
      xeKeInsertQueueDpc(dpc, static_cast<uint32_t>(systime),
                         static_cast<uint32_t>(systime >> 32), context);
    }
    if (period) {
      while (!XeInsertGlobalTimer(context, timer, -10000LL * period)) {
        //??
        xenia_assert(false);
      }
    }
  }

  xeDispatcherSpinlockUnlock(
      context, context->kernel_state->GetDispatcherLock(context), old_irql);
  return was_inserted;
}

int xeKeSetTimer(PPCContext* context, X_KTIMER* timer, int64_t duetime,
                 XDPC* dpc) {
  return xeKeSetTimerEx(context, timer, duetime, 0, dpc);
}

int xeKeCancelTimerEx(PPCContext* context, X_KTIMER* timer) {
  auto old_irql = context->kernel_state->LockDispatcher(context);
  auto was_inserted = timer->header.inserted;
  if (was_inserted) {
    timer->header.inserted = 0;
    util::XeRemoveEntryList(&timer->table_bucket_entry, context);
  }
  xeDispatcherSpinlockUnlock(
      context, context->kernel_state->GetDispatcherLock(context), old_irql);
  return was_inserted;
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe