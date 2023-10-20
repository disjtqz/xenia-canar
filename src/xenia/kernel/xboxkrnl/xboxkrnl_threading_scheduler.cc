/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
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

static void set_msr_interrupt_bits(PPCContext* context, uint32_t value) {
  // todo: implement!
}


using ready_thread_pointer_t =
    ShiftedPointer<X_LIST_ENTRY, X_KTHREAD,
                   offsetof(X_KTHREAD, ready_prcb_entry)>;

static void xeHandleReadyThreadOnDifferentProcessor(PPCContext* context,
                                                    X_KTHREAD* kthread) {
  xe::FatalError("Cant handle thread processor switching atm");
}

static void xeReallyQueueThread(PPCContext* context, X_KTHREAD* kthread) {
  auto prcb_for_thread = context->TranslateVirtual(kthread->a_prcb_ptr);
  xboxkrnl::xeKeKfAcquireSpinLock(
      context, &prcb_for_thread->enqueued_processor_threads_lock, false);

  xboxkrnl::xeKeKfReleaseSpinLock(
      context, &prcb_for_thread->enqueued_processor_threads_lock, 0, false);
}

static void xeProcessQueuedThreads(PPCContext* context,
                                   bool under_dispatcher_lock) {
  auto kernel = context->kernel_state;

  if (under_dispatcher_lock) {
    kernel->AssertDispatcherLocked(context);
  } else {
    kernel->LockDispatcherAtIrql(context);
  }

  uint32_t first_ready_thread =
      GetKPCR(context)->prcb_data.enqueued_threads_list.next;

  GetKPCR(context)->prcb_data.enqueued_threads_list.next = 0;

  while (first_ready_thread) {
    ready_thread_pointer_t ready_thread =
        context->TranslateVirtual<X_LIST_ENTRY*>(first_ready_thread);
    first_ready_thread = ready_thread->flink_ptr;
    // xeEnqueueThreadPostWait sets it to 6
    xenia_assert(ready_thread.GetAdjacent()->thread_state == 6);

    uint32_t prcb =
        static_cast<uint32_t>(context->r[13]) + offsetof(X_KPCR, prcb_data);

    auto adj = ready_thread.GetAdjacent();
    if (adj->a_prcb_ptr == prcb && adj->another_prcb_ptr != prcb) {
      xeHandleReadyThreadOnDifferentProcessor(context, adj);
    }
    xeReallyQueueThread(context, adj);
  }

  kernel->AssertDispatcherLocked(context);
  if (!under_dispatcher_lock) {
    kernel->UnlockDispatcherAtIrql(context);
  }
}

X_KTHREAD* xeSelectThreadDueToTimesliceExpiration(PPCContext* context) {
  xe::FatalError("xeSelectThreadDueToTimesliceExpiration unimplemented");
  return nullptr;
}

// handles DPCS, also switches threads?
// timer related?
void xeHandleDPCsAndThreadSwapping(PPCContext* context) {
  X_KTHREAD* next_thread = nullptr;
  while (true) {
    set_msr_interrupt_bits(context, 0);
    if (!GetKPCR(context)->prcb_data.queued_dpcs_list_head.empty(context) ||
        GetKPCR(context)->timer_pending) {
      // todo: incomplete!

      xeExecuteDPCList2(context, GetKPCR(context)->timer_pending,
                        GetKPCR(context)->prcb_data.queued_dpcs_list_head, 0);
    }
    set_msr_interrupt_bits(context, 0xFFFF8000);

    if (GetKPCR(context)->prcb_data.enqueued_threads_list.next) {
      xeProcessQueuedThreads(context, false);
    }

    if (GetKPCR(context)->timeslice_ended) {
      GetKPCR(context)->timeslice_ended = 0;
      next_thread = xeSelectThreadDueToTimesliceExpiration(context);
      break;
    }
    // failed to select a thread to switch to
    if (!GetKPCR(context)->prcb_data.next_thread) {
      return;
    }

    // some kind of lock acquire function here??

    uint32_t thrd_u = GetKPCR(context)->prcb_data.next_thread.m_ptr;

    if (!thrd_u) {
      next_thread = nullptr;
    } else {
      next_thread = context->TranslateVirtual<X_KTHREAD*>(thrd_u);
    }
  }
  // requeue ourselves
  // GetKPCR(context)->prcb_data.current_thread
}



void xeEnqueueThreadPostWait(PPCContext* context, X_KTHREAD* thread,
                             X_STATUS wait_result, int unknown) {
  xenia_assert(thread->thread_state == 5);
  thread->wait_result = thread->wait_result | wait_result;

  xenia_assert(GetKPCR(context)->current_irql < IRQL_DISPATCH);

  X_KWAIT_BLOCK* wait_blocks = context->TranslateVirtual(thread->wait_blocks);
  do {
    uint32_t v7 = wait_blocks->wait_list_entry.flink_ptr;
    uint32_t v8 = wait_blocks->wait_list_entry.blink_ptr;

    context->TranslateVirtual<X_LIST_ENTRY*>(v8)->flink_ptr =
        wait_blocks->wait_list_entry.flink_ptr;
    context->TranslateVirtual<X_LIST_ENTRY*>(v7)->blink_ptr = v8;
    wait_blocks = context->TranslateVirtual(wait_blocks->next_wait_block);
  } while (wait_blocks != context->TranslateVirtual(thread->wait_blocks));

  // wait is over, so cancel the timeout timer
  if (thread->wait_timeout_timer.header.inserted) {
    thread->wait_timeout_timer.header.inserted = 0;
    util::XeRemoveEntryList(&thread->wait_timeout_timer.table_bucket_entry,
                            context);
    thread->wait_timeout_timer.table_bucket_entry.flink_ptr = 0;
    thread->wait_timeout_timer.table_bucket_entry.blink_ptr = 0;
  }
  auto unk_ptr = thread->unkptr_118;
  if (unk_ptr) {
    auto unk_counter = context->TranslateVirtualBE<uint32_t>(unk_ptr + 0x18);
    *unk_counter++;
  }

  auto prcb = context->TranslateVirtual(thread->a_prcb_ptr);

  xboxkrnl::xeKeKfAcquireSpinLock(
      context, &prcb->enqueued_processor_threads_lock, false);

  /*
    todo: a lot of priority related shit here that im skipping!!!




  */

  thread->thread_state = 6;
  thread->ready_prcb_entry.flink_ptr = prcb->enqueued_threads_list.next;

  prcb->enqueued_threads_list.next =
      context->HostToGuestVirtual(&thread->ready_prcb_entry);

  xboxkrnl::xeKeKfReleaseSpinLock(
      context, &prcb->enqueued_processor_threads_lock, 0, false);
}

static void xeSignalDispatcher(PPCContext* context, X_DISPATCH_HEADER* entry,
                               X_KTHREAD* thread_for_wait) {
  auto current_wait_object = entry;
  int current_object_type = current_wait_object->type;
  if ((current_object_type & 7) == 1) {
    current_wait_object->signal_state = 0;
  } else if (current_object_type == 5) {  // semaphore
    --current_wait_object->signal_state;
  } else if (current_object_type == 2) {  // mutant
    int new_signalstate = current_wait_object->signal_state - 1;
    current_wait_object->signal_state = new_signalstate;
    if (!new_signalstate) {
      X_KMUTANT* mutant = reinterpret_cast<X_KMUTANT*>(current_wait_object);
      auto v6 = mutant->abandoned;
      mutant->owner = context->HostToGuestVirtual(thread_for_wait);
      if (v6 == 1) {
        mutant->abandoned = 0;
        thread_for_wait->wait_result = X_STATUS_ABANDONED_WAIT_0;
      }

      // clearly inserthead or tail, determine which and clean this up
      uint32_t v7 = thread_for_wait->mutants_list.blink_ptr;
      auto v7ptr = context->TranslateVirtual<X_LIST_ENTRY*>(v7);
      uint32_t v8 = v7ptr->flink_ptr;

      auto v8ptr = context->TranslateVirtual<X_LIST_ENTRY*>(v8);
      mutant->unk_list.blink_ptr = v7;
      mutant->unk_list.flink_ptr = v8;
      auto unk_list = context->HostToGuestVirtual(&mutant->unk_list);
      v8ptr->blink_ptr = unk_list;
      v7ptr->flink_ptr = unk_list;
    }
  }
}

void xeHandleWaitTypeAll(PPCContext* context, X_KWAIT_BLOCK* block) {
  auto thread_for_wait = context->TranslateVirtual(block->thread);
  auto current_waitblock = block;
  do {
    if (current_waitblock->wait_result_xstatus != X_STATUS_TIMEOUT) {
      xeSignalDispatcher(context,
                         context->TranslateVirtual(current_waitblock->object),
                         thread_for_wait);
    }
    current_waitblock =
        context->TranslateVirtual(current_waitblock->next_wait_block);
  } while (current_waitblock != block);
}
void xeDispatchSignalStateChange(PPCContext* context, X_DISPATCH_HEADER* header,
                                 int unk) {
  auto waitlist_head = &header->wait_list;

  for (X_KWAIT_BLOCK* i = context->TranslateVirtual<X_KWAIT_BLOCK*>(
           header->wait_list.flink_ptr);
       static_cast<int>(header->signal_state) > 0;
       i = context->TranslateVirtual<X_KWAIT_BLOCK*>(
           i->wait_list_entry.flink_ptr)) {
    if ((X_LIST_ENTRY*)i == waitlist_head) {
      break;
    }

    auto v6 = i;
    auto v7 = context->TranslateVirtual(i->thread);
    if (i->wait_type == WAIT_ANY) {
      xeSignalDispatcher(context, header, v7);
    } else {
      for (X_KWAIT_BLOCK* j = context->TranslateVirtual(i->next_wait_block);
           j != i; j = context->TranslateVirtual(j->next_wait_block)) {
        if (j->wait_result_xstatus != X_STATUS_TIMEOUT) {
          auto v9 = context->TranslateVirtual(j->object);

          if ((v9->type != 2 || v9->signal_state > 0 ||
               v7 != context->TranslateVirtual(
                         reinterpret_cast<X_KMUTANT*>(v9)->owner)) &&
              v9->signal_state <= 0) {
            goto LABEL_23;
          }
        }
      }
      i = context->TranslateVirtual<X_KWAIT_BLOCK*>(
          i->wait_list_entry.blink_ptr);
      xeHandleWaitTypeAll(context, v6);
    }
    xeEnqueueThreadPostWait(context, v7, v6->wait_result_xstatus, unk);
  LABEL_23:;
  }
}
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
