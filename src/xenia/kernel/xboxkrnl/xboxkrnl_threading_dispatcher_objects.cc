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
    current_thread->wait_next = wait;
    current_thread->wait_irql = old_irql;
  } else {
    xeDispatcherSpinlockUnlock(
        context, context->kernel_state->GetDispatcherLock(context), old_irql);
  }
  return old_signalstate;
}

int32_t xeKePulseEvent(PPCContext* context, X_KEVENT* event, int increment,
                       unsigned char wait) {
  xenia_assert(event && event->header.type < 2);
  uint32_t old_irql = context->kernel_state->LockDispatcher(context);

  auto old_signalstate = event->header.signal_state;
  auto wait_list = context->TranslateVirtual<X_KWAIT_BLOCK*>(
      event->header.wait_list.flink_ptr);

  if (!old_signalstate &&
      &wait_list->wait_list_entry != &event->header.wait_list) {
    event->header.signal_state = 1;
    xeDispatchSignalStateChange(context, &event->header, increment);
  }
  event->header.signal_state = 0;
  if (wait) {
    auto current_thread =
        context->TranslateVirtual(GetKPCR(context)->prcb_data.current_thread);
    current_thread->wait_next = wait;
    current_thread->wait_irql = old_irql;
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

int32_t xeKeReleaseMutant(PPCContext* context, X_KMUTANT* mutant, int increment,
                          bool abandoned, unsigned char wait) {
  auto old_irql = context->kernel_state->LockDispatcher(context);
  int32_t old_signal_state = mutant->header.signal_state;
  int32_t new_signal_state;
  auto current_thread =
      context->TranslateVirtual(GetKPCR(context)->prcb_data.current_thread);

  if (!abandoned) {
    if (context->TranslateVirtual(mutant->owner) != current_thread) {
      xeDispatcherSpinlockUnlock(
          context, context->kernel_state->GetDispatcherLock(context), old_irql);
      // xe::FatalError("We don't own the mutant, but we're releasing it!");
      // return -1;
      // xenia_assert(false);
      X_STATUS stat = mutant->abandoned ? X_STATUS_ABANDONED_WAIT_0
                                        : X_STATUS_MUTANT_NOT_OWNED;
      // should RtlRaiseStatus! NtReleaseMutant catches the status i think, ida
      // indicates a try handler

      context->RaiseStatus(stat);
      return 0;
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
      xeDispatchSignalStateChange(context, &mutant->header, increment);
    }
  }

  if (wait) {
    current_thread->wait_next = wait;
    current_thread->wait_irql = old_irql;

  } else {
    xeDispatcherSpinlockUnlock(
        context, context->kernel_state->GetDispatcherLock(context), old_irql);
  }
  return old_signal_state;
}

int32_t xeKeReleaseSemaphore(PPCContext* context, X_KSEMAPHORE* semaphore,
                             int increment, int adjustment,
                             unsigned char wait) {
  auto old_irql = context->kernel_state->LockDispatcher(context);
  int32_t old_signal_state = semaphore->header.signal_state;

  int32_t new_signal_state = old_signal_state + adjustment;

  if (new_signal_state > semaphore->limit ||
      new_signal_state < old_signal_state) {
    xeDispatcherSpinlockUnlock(
        context, context->kernel_state->GetDispatcherLock(context), old_irql);
    // should RtlRaiseStatus
    // xenia_assert(false);
    context->RaiseStatus(X_STATUS_SEMAPHORE_LIMIT_EXCEEDED);
    return 0;
  }

  semaphore->header.signal_state = new_signal_state;

  if (!old_signal_state &&
      !util::XeIsListEmpty(&semaphore->header.wait_list, context)) {
    xeDispatchSignalStateChange(context, &semaphore->header, increment);
  }

  if (wait) {
    GetKThread(context)->wait_next = wait;
    GetKThread(context)->wait_irql = old_irql;

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

int xeKeCancelTimer(PPCContext* context, X_KTIMER* timer) {
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

void xeEXTimerDPCRoutine(PPCContext* context) {
  X_EXTIMER* timer = context->TranslateVirtualGPR<X_EXTIMER*>(context->r[4]);
  uint32_t apcarg1 = static_cast<uint32_t>(context->r[5]);
  uint32_t apcarg2 = static_cast<uint32_t>(context->r[6]);

  auto old_irql = xeKeKfAcquireSpinLock(context, &timer->timer_lock);

  if (timer->has_apc) {
    xeKeInsertQueueApc(&timer->apc, apcarg1, apcarg2, 0, context);
  }

  xeKeKfReleaseSpinLock(context, &timer->timer_lock, old_irql);
}

void xeKeInitializeExTimer(PPCContext* context, X_EXTIMER* timer,
                           uint32_t type) {
  memset(timer, 0, sizeof(X_EXTIMER));
  timer->dpc.Initialize(context->kernel_state->GetKernelGuestGlobals(context)
                            ->extimer_dpc_routine,
                        context->HostToGuestVirtual(timer));
  xeKeInitializeTimerEx(&timer->ktimer, type,
                        xeKeGetCurrentProcessType(context), context);
}
void xeEXTimerAPCKernelRoutine(PPCContext* context) {
  X_EXTIMER* timer = context->TranslateVirtualGPR<X_EXTIMER*>(
      context->r[3] - offsetof(X_EXTIMER, apc));

  uint32_t old_irql =
      xboxkrnl::xeKeKfAcquireSpinLock(context, &timer->timer_lock);

  auto current_thread = GetKThread(context);
  xboxkrnl::xeKeKfAcquireSpinLock(context, &current_thread->timer_list_lock,
                                  false);
  bool v10 = false;
  if (timer->has_apc && current_thread == timer->apc.thread_ptr.xlat()) {
    if (!timer->period) {
      v10 = true;
      util::XeRemoveEntryList(&timer->thread_timer_list_entry, context);
      timer->has_apc = false;
    }

  } else {
    *context->TranslateVirtualGPR<uint32_t*>(context->r[4]) = 0;
  }

  xboxkrnl::xeKeKfReleaseSpinLock(context, &current_thread->timer_list_lock, 0,
                                  false);

  xboxkrnl::xeKeKfReleaseSpinLock(context, &timer->timer_lock, old_irql);

  // if v10 is set, supposed to dereference here, but that must wait until we
  // implement objects correctly
}
static bool HelperCancelTimer(PPCContext* context, X_EXTIMER* timer) {
  if (timer->has_apc) {
    xeKeKfAcquireSpinLock(context, &timer->apc.thread_ptr->timer_list_lock,
                          false);

    util::XeRemoveEntryList(&timer->thread_timer_list_entry, context);
    timer->has_apc = false;
    xeKeKfReleaseSpinLock(context, &timer->apc.thread_ptr->timer_list_lock, 0,
                          false);
    xeKeCancelTimer(context, &timer->ktimer);
    xeKeRemoveQueueDpc(&timer->dpc, context);
    xeKeRemoveQueueApc(&timer->apc, context);
    return true;
  } else {
    xeKeCancelTimer(context, &timer->ktimer);
    return false;
  }
}

// todo: this is incomplete, theres a bunch of dereferenceobject calls missing
int xeKeSetExTimer(PPCContext* context, X_EXTIMER* timer, int64_t due_timer,
                   uint32_t apc_routine, uint32_t apc_arg, int period,
                   int apc_mode) {
  uint32_t old_irql = xeKeKfAcquireSpinLock(context, &timer->timer_lock);

  bool v21 = HelperCancelTimer(context, timer);

  auto old_signalstate = timer->ktimer.header.signal_state;

  timer->period = period;

  if (apc_routine) {
    auto current_thread = GetKThread(context);
    xeKeInitializeApc(&timer->apc,
                      GetKPCR(context)->prcb_data.current_thread.m_ptr,
                      context->kernel_state->GetKernelGuestGlobals(context)
                          ->extimer_apc_kernel_routine,
                      0, apc_routine, apc_mode, apc_arg);
    xeKeKfAcquireSpinLock(context, &current_thread->timer_list_lock, false);
    util::XeInsertTailList(&current_thread->timer_list,
                           &timer->thread_timer_list_entry, context);
    timer->has_apc = true;
    xeKeKfReleaseSpinLock(context, &current_thread->timer_list_lock, 0, false);
    xeKeSetTimerEx(context, &timer->ktimer, due_timer, period, &timer->dpc);
  } else {
    xeKeSetTimerEx(context, &timer->ktimer, due_timer, period, 0);
  }
  xeKeKfReleaseSpinLock(context, &timer->timer_lock, old_irql);
  return old_signalstate;
}

int xeKeCancelExTimer(PPCContext* context, X_EXTIMER* timer) {
  uint32_t old_irql = xeKeKfAcquireSpinLock(context, &timer->timer_lock);

  bool v8 = HelperCancelTimer(context, timer);
  xeKeKfReleaseSpinLock(context, &timer->timer_lock, old_irql);
  int old_signalstate = timer->ktimer.header.signal_state;

  return old_signalstate;
}

X_DISPATCH_HEADER* xeObGetWaitableObject(PPCContext* context, void* object) {
  auto wait_object_type = context->TranslateVirtual<X_OBJECT_TYPE*>(
      reinterpret_cast<X_OBJECT_HEADER*>(object)[-1].object_type_ptr);

  // either encodes an offset from the object base to the object to wait on,
  // or a default object to wait on?
  uint32_t unk = wait_object_type->unknown_size_or_object_;
  auto kernel_guest_globals =
      context->kernel_state->GetKernelGuestGlobals(context);
  if (wait_object_type == &kernel_guest_globals
                               ->IoFileObjectType) {
    xenia_assert(false);
  }

  if (wait_object_type == &kernel_guest_globals->IoCompletionObjectType) {
    xenia_assert(false);
  }
  X_DISPATCH_HEADER* waiter =
      context->TranslateVirtual<X_DISPATCH_HEADER*>(unk);
  // if (unk) {
  //   __debugbreak();
  // }
  if (!((unsigned int)unk >> 16)) {
    waiter = reinterpret_cast<X_DISPATCH_HEADER*>(
        reinterpret_cast<char*>(object) + unk);
  } else {
    __debugbreak();
  }
  return waiter;
}

void xeKeInitializeQueue(X_KQUEUE* queue, uint32_t count, PPCContext* context) {
  queue->header.signal_state = 0;
  queue->header.type = DISPATCHER_QUEUE;
  util::XeInitializeListHead(&queue->header.wait_list, context);
  util::XeInitializeListHead(&queue->entry_list_head, context);
  util::XeInitializeListHead(&queue->thread_list_head, context);
  queue->current_count = 0;
  if (count) {
    queue->maximum_count = count;
  } else {
    queue->maximum_count = 1;
  }
}

template <bool to_head>
static int32_t InsertQueueUnderLock(PPCContext* context, X_KQUEUE* queue,
                                    X_LIST_ENTRY* entry) {
  auto old_irql = context->kernel_state->LockDispatcher(context);
  auto first_waitblock = context->TranslateVirtual<X_KWAIT_BLOCK*>(
      queue->header.wait_list.blink_ptr);
  auto current_thread = GetKThread(context);
  int32_t old_signalstate = queue->header.signal_state;
  if (first_waitblock == (X_KWAIT_BLOCK*)&queue->header.wait_list ||
      queue->current_count >= queue->maximum_count ||
      current_thread->queue.xlat() == queue &&
          current_thread->wait_reason == 4) {
    queue->header.signal_state = old_signalstate + 1;
    if (to_head) {
      util::XeInsertHeadList(&queue->entry_list_head, entry, context);

    } else {
      util::XeInsertTailList(&queue->entry_list_head, entry, context);
    }
  } else {
    util::XeRemoveEntryList(&first_waitblock->wait_list_entry, context);

    auto thread_for_waitblock =
        context->TranslateVirtual(first_waitblock->thread);
    thread_for_waitblock->wait_result = (int)context->HostToGuestVirtual(entry);
    ++queue->current_count;
    thread_for_waitblock->wait_reason = 0;
    if (thread_for_waitblock->wait_timeout_timer.header.inserted) {
      thread_for_waitblock->wait_timeout_timer.header.inserted = 0;
      util::XeRemoveEntryList(
          &thread_for_waitblock->wait_timeout_timer.table_bucket_entry,
          context);
    }
    xeReallyQueueThread(context, thread_for_waitblock);
  }
  xeDispatcherSpinlockUnlock(
      context, context->kernel_state->GetDispatcherLock(context), old_irql);
  return old_signalstate;
}

int32_t xeKeInsertQueue(X_KQUEUE* queue, X_LIST_ENTRY* entry,
                        PPCContext* context) {
  return InsertQueueUnderLock<false>(context, queue, entry);
}
int32_t xeKeInsertHeadQueue(X_KQUEUE* queue, X_LIST_ENTRY* entry,
                            PPCContext* context) {
  return InsertQueueUnderLock<true>(context, queue, entry);
}

void xeKeSignalQueue(PPCContext* context, X_KQUEUE* queue) {
  uint32_t new_currentcount = queue->current_count - 1U;
  queue->current_count = new_currentcount;
  if (new_currentcount >= queue->maximum_count) {
    return;
  }

  if (util::XeIsListEmpty(&queue->header.wait_list, context) ||
      util::XeIsListEmpty(&queue->entry_list_head, context)) {
    return;
  }

  X_KWAIT_BLOCK* block = context->TranslateVirtual<X_KWAIT_BLOCK*>(
      queue->header.wait_list.blink_ptr);
  uint32_t entry_guest = queue->entry_list_head.flink_ptr;
  X_LIST_ENTRY* entry = context->TranslateVirtual<X_LIST_ENTRY*>(entry_guest);

  util::XeRemoveEntryList(entry, context);
  entry->flink_ptr = 0u;

  queue->header.signal_state--;
  // send the list entry to the waiter
  xeEnqueueThreadPostWait(context, context->TranslateVirtual(block->thread),
                          static_cast<X_STATUS>(entry_guest), 0);
}

X_LIST_ENTRY* xeKeRundownQueue(PPCContext* context, X_KQUEUE* queue) {
  uint32_t old_irql = context->kernel_state->LockDispatcher(context);
  auto v4 = context->TranslateVirtual(queue->entry_list_head.flink_ptr);
  if (v4 == &queue->entry_list_head) {
    v4 = 0;
  } else {
    util::XeRemoveEntryList(&queue->entry_list_head, context);
  }
  auto v5 = &queue->thread_list_head;
  while (!v5->empty(context)) {
    auto kthread = v5->HeadObject(context);
    kthread->queue = 0U;

    util::XeRemoveEntryList(&kthread->queue_related, context);
  }
  xeDispatcherSpinlockUnlock(
      context, context->kernel_state->GetDispatcherLock(context), old_irql);
  return v4;
}

uint32_t xeKeRemoveQueue(PPCContext* context, X_KQUEUE* queue,
                         unsigned char wait_mode, int64_t* timeout) {
  auto this_thread = GetKThread(context);
  if (this_thread->wait_next)
    this_thread->wait_next = 0;
  else
    this_thread->wait_irql = context->kernel_state->LockDispatcher(context);

  auto v8 = context->TranslateVirtual(this_thread->queue);
  this_thread->queue = context->HostToGuestVirtual(queue);
  if (queue == v8) {
    --queue->current_count;
  } else {
    auto v9 = &this_thread->queue_related;
    if (v8) {
      auto v10 = v9->flink_ptr;
      auto v11 = this_thread->queue_related.blink_ptr;
      v11->flink_ptr = v9->flink_ptr;
      v10->blink_ptr = v11;
      xeKeSignalQueue(context, v8);
    }
    auto v12 = queue->thread_list_head.blink_ptr;
    v9->flink_ptr = &queue->thread_list_head;
    this_thread->queue_related.blink_ptr = v12;
    v12->flink_ptr = v9;
    queue->thread_list_head.blink_ptr = v9;
  }
  auto v13 = timeout;
  auto v14 = &queue->entry_list_head;
  int64_t v20;
  int64_t tmp_timeout;
  uint32_t v15;
  auto scratch_waitblock = &this_thread->scratch_waitblock_memory[0];
  while (1) {
    v15 = v14->flink_ptr;
    if (v15 != context->HostToGuestVirtual(v14) &&
        queue->current_count < queue->maximum_count) {
      auto queue_newcount = queue->current_count + 1;
      --queue->header.signal_state;
      queue->current_count = queue_newcount;
      util::XeRemoveEntryList(v15, context);
      context->TranslateVirtual<X_LIST_ENTRY*>(v15)->flink_ptr = 0u;
      goto LABEL_36;
    }
    if (this_thread->deferred_apc_software_interrupt_state &&
        !this_thread->wait_irql) {
      ++queue->current_count;
      xeDispatcherSpinlockUnlock(
          context, context->kernel_state->GetDispatcherLock(context),
          this_thread->wait_irql);
      goto LABEL_31;
    }
    if (wait_mode && this_thread->user_apc_pending) {
      v15 = X_STATUS_USER_APC;
      goto LABEL_35;
    }
    this_thread->wait_result = 0;
    this_thread->wait_blocks = scratch_waitblock;
    scratch_waitblock->object = &queue->header;
    scratch_waitblock->wait_result_xstatus = 0;
    scratch_waitblock->thread = this_thread;
    scratch_waitblock->wait_type = 1;
    if (!timeout) {
      scratch_waitblock->next_wait_block = scratch_waitblock;
      goto LABEL_26;
    }
    if (!*timeout) {
      break;
    }
    scratch_waitblock->next_wait_block = &this_thread->wait_timeout_block;
    this_thread->wait_timeout_timer.header.wait_list.flink_ptr =
        &this_thread->wait_timeout_block.wait_list_entry;
    this_thread->wait_timeout_timer.header.wait_list.blink_ptr =
        &this_thread->wait_timeout_block.wait_list_entry;
    this_thread->wait_timeout_block.next_wait_block = scratch_waitblock;

    if (!XeInsertGlobalTimer(context, &this_thread->wait_timeout_timer,
                             *timeout))
      break;
    v20 = this_thread->wait_timeout_timer.due_time;
  LABEL_26:
    auto v16 = queue->header.wait_list.blink_ptr;
    scratch_waitblock->wait_list_entry.flink_ptr = &queue->header.wait_list;
    scratch_waitblock->wait_list_entry.blink_ptr = v16;
    v16->flink_ptr = &scratch_waitblock->wait_list_entry;
    queue->header.wait_list.blink_ptr = &scratch_waitblock->wait_list_entry;
    this_thread->alertable = 0;
    this_thread->processor_mode = wait_mode;
    this_thread->wait_reason = 4;
    this_thread->thread_state = 5;

    auto result = xeSchedulerSwitchThread2(context);
    this_thread->wait_reason = 0;
    if (result != X_STATUS_KERNEL_APC) {
      return result;
    }
    if (timeout) {
      if (*timeout < 0) {
        tmp_timeout =
            context->kernel_state->GetKernelInterruptTime() - *timeout;
        timeout = &tmp_timeout;
      } else {
        timeout = v13;
      }
    }

  LABEL_31:
    this_thread->wait_irql = context->kernel_state->LockDispatcher(context);
    --queue->current_count;
  }
  v15 = X_STATUS_TIMEOUT;
LABEL_35:
  ++queue->current_count;
LABEL_36:
  xeDispatcherSpinlockUnlock(context,
                             context->kernel_state->GetDispatcherLock(context),
                             this_thread->wait_irql);
  return v15;
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
