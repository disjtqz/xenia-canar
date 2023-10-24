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
static void insert_8009CFE0(PPCContext* context, X_KTHREAD* thread, int unk);
static void insert_8009D048(PPCContext* context, X_KTHREAD* thread);
static X_KTHREAD* xeScanForReadyThread(PPCContext* context, X_KPRCB* prcb,
                                       int priority);
static void xeProcessQueuedThreads(PPCContext* context,
                                   bool under_dispatcher_lock);
X_KTHREAD* xeSelectThreadDueToTimesliceExpiration(PPCContext* context);

static void set_msr_interrupt_bits(PPCContext* context, uint32_t value) {
  // todo: implement!
  uint64_t old_msr = context->msr;
  context->msr = (old_msr & ~0x8000ULL) | (value & 0x8000);

}

using ready_thread_pointer_t =
    ShiftedPointer<X_LIST_ENTRY, X_KTHREAD,
                   offsetof(X_KTHREAD, ready_prcb_entry)>;

/*
    a special spinlock-releasing function thats used in a lot of scheduler
   related functions im not very confident in the correctness of this one. the
   original jumps around a lot, directly into the bodies of other functions and
   appears to have been written in asm
*/
void xeDispatcherSpinlockUnlock(PPCContext* context, X_KSPINLOCK* lock,
                                uint32_t irql) {
  xenia_assert(lock->pcr_of_owner == static_cast<uint32_t>(context->r[13]));
  lock->pcr_of_owner = 0;
reenter:
  auto kpcr = GetKPCR(context);
  if (kpcr->prcb_data.enqueued_threads_list.next) {
    xeProcessQueuedThreads(context, false);
    // todo: theres a jump here!!
    // this doesnt feel right
    goto reenter;
  } else if (kpcr->prcb_data.next_thread.m_ptr != 0) {
    if (irql > IRQL_APC) {
      if (!kpcr->prcb_data.dpc_active) {
        kpcr->generic_software_interrupt = irql;
      }
    } else {
      xboxkrnl::xeKeKfAcquireSpinLock(context, &kpcr->prcb_data.enqueued_processor_threads_lock, false);
      auto next_thread = kpcr->prcb_data.next_thread;
      auto v3 = kpcr->prcb_data.current_thread.xlat();
      v3->unk_A4 = irql;
      kpcr->prcb_data.next_thread = 0U;
      kpcr->prcb_data.current_thread = next_thread;
      insert_8009D048(context, v3);
      // jump here!
      // r31 = next_thread
      // r30 = current thread
      // definitely switching threads

      context->kernel_state->ContextSwitch(context, next_thread.xlat());

      // at this point we're supposed to load a bunch of fields from r31 and do
      // shit

      // im just assuming r31 is supposed to be this
      X_KTHREAD* r31 = next_thread.xlat();

      auto r3 = r31->unk_A4;
      auto r29 = r31->wait_result;
      GetKPCR(context)->current_irql = r3;
      auto r4 = GetKPCR(context)->software_interrupt_state;
      if (r3 < r4) {
        xeDispatchProcedureCallInterrupt(r3, r4, context);
      }
    }
  } else if (irql <= IRQL_APC) {
    kpcr->current_irql = irql;
    auto v4 = kpcr->software_interrupt_state;
    if (irql < v4) {
      xeDispatchProcedureCallInterrupt(irql, v4, context);
    }
  }
}

void xeHandleReadyThreadOnDifferentProcessor(PPCContext* context,
                                             X_KTHREAD* kthread) {
  auto kpcr = GetKPCR(context);
  auto v3 = &kpcr->prcb_data;
  xboxkrnl::xeKeKfAcquireSpinLock(
      context, &kpcr->prcb_data.enqueued_processor_threads_lock, false);

  if (kthread->thread_state != 2) {
    // xe::FatalError("Doing some fpu/vmx shit here?");
    // it looks like its saving the fpu and vmx state
    // we don't have to do this i think, because we already have different
    // PPCContext per guest thread
  }
  // https://www.geoffchappell.com/studies/windows/km/ntoskrnl/inc/ntos/ke/kthread_state.htm
  switch (kthread->thread_state) {
    case 1: {  // ready
      auto v23 = kthread->ready_prcb_entry.flink_ptr;
      auto v24 = kthread->ready_prcb_entry.blink_ptr;
      v24->flink_ptr = v23;
      v23->blink_ptr = v24;
      if (v24 == v23) {
        v3->has_ready_thread_by_priority =
            v3->has_ready_thread_by_priority & (~(1 << kthread->priority));
      }
      break;
    }
    case 2: {  // running
      if (!v3->next_thread) {
        auto v22 = xeScanForReadyThread(context, v3, 0);
        if (!v22) {
          v22 = v3->idle_thread.xlat();
          v3->running_idle_thread = v22;
        }
        v22->thread_state = 3;
        v3->next_thread = v22;
        GetKPCR(context)->generic_software_interrupt = 2;
      }
      xboxkrnl::xeKeKfReleaseSpinLock(
          context, &kpcr->prcb_data.enqueued_processor_threads_lock, 0, false);
      return;
    }
    case 3: {  // standby
      auto v21 = xeScanForReadyThread(context, v3, 0);
      if (!v21) {
        v21 = v3->idle_thread.xlat();
        v3->running_idle_thread = v21;
      }
      v21->thread_state = 3;
      v3->next_thread = v21;
      break;
    }
    default: {
      auto v19 = kthread->another_prcb_ptr;
      auto v20 = v19->current_cpu;
      kthread->a_prcb_ptr = v19;
      kthread->current_cpu = v20;
      xboxkrnl::xeKeKfReleaseSpinLock(
          context, &kpcr->prcb_data.enqueued_processor_threads_lock, 0, false);
      return;
    }
  }

  auto v25 = kthread->another_prcb_ptr;
  auto v26 = v25->current_cpu;
  kthread->a_prcb_ptr = v25;
  kthread->current_cpu = v26;

  xboxkrnl::xeKeKfReleaseSpinLock(
      context, &kpcr->prcb_data.enqueued_processor_threads_lock, 0, false);
  xeReallyQueueThread(context, kthread);
}

static void insert_8009CFE0(PPCContext* context, X_KTHREAD* thread, int unk) {
  auto priority = thread->priority;
  auto thread_prcb = context->TranslateVirtual(thread->a_prcb_ptr);
  auto thread_ready_list_entry = &thread->ready_prcb_entry;
  thread->thread_state = 1;
  auto& list_for_priority = thread_prcb->ready_threads_by_priority[priority];
  if (unk) {
    auto v6 = list_for_priority.flink_ptr;
    thread->ready_prcb_entry.blink_ptr = &list_for_priority;
    thread_ready_list_entry->flink_ptr = v6;
    v6->blink_ptr = thread_ready_list_entry;
    list_for_priority.flink_ptr = thread_ready_list_entry;
  } else {
    auto v7 = list_for_priority.blink_ptr;
    thread_ready_list_entry->flink_ptr = &list_for_priority;
    thread->ready_prcb_entry.blink_ptr = v7;
    v7->flink_ptr = thread_ready_list_entry;
    list_for_priority.blink_ptr = thread_ready_list_entry;
  }

  thread_prcb->has_ready_thread_by_priority =
      thread_prcb->has_ready_thread_by_priority | (1U << priority);
}

static void insert_8009D048(PPCContext* context, X_KTHREAD* thread) {
  if (context->TranslateVirtual(thread->another_prcb_ptr) ==
      &GetKPCR(context)->prcb_data) {
    unsigned char unk = thread->unk_BD;
    thread->unk_BD = 0;
    insert_8009CFE0(context, thread, unk);
  } else {
    thread->thread_state = 6;
    auto kpcr = GetKPCR(context);
    thread->ready_prcb_entry.flink_ptr =
        kpcr->prcb_data.enqueued_threads_list.next;
    kpcr->prcb_data.enqueued_threads_list.next =
        context->HostToGuestVirtual(&thread->ready_prcb_entry);
    kpcr->generic_software_interrupt = 2;
  }
}
/*
    performs bitscanning on the bitmask of available thread priorities to
    select the first runnable one that is greater than or equal to the prio arg
*/
static X_KTHREAD* xeScanForReadyThread(PPCContext* context, X_KPRCB* prcb,
                                       int priority) {
  auto v3 = prcb->has_ready_thread_by_priority;
  if ((prcb->has_ready_thread_by_priority & ~((1 << priority) - 1) & v3) == 0) {
    return nullptr;
  }
  auto v4 = xe::lzcnt(prcb->has_ready_thread_by_priority &
                      ~((1 << priority) - 1) & v3);
  auto v5 = 31 - v4;

  auto result = prcb->ready_threads_by_priority[31 - v4].HeadObject(context);

  auto v7 = result->ready_prcb_entry.flink_ptr;
  auto v8 = result->ready_prcb_entry.blink_ptr;
  context->TranslateVirtual<X_LIST_ENTRY*>(v8)->flink_ptr = v7;
  context->TranslateVirtual<X_LIST_ENTRY*>(v7)->blink_ptr = v8;
  if (v8 == v7) {
    prcb->has_ready_thread_by_priority =
        prcb->has_ready_thread_by_priority & (~(1 << v5));
  }
  return result;
}

void HandleCpuThreadDisownedIPI(void* ud) { xenia_assert(false); }

void xeReallyQueueThread(PPCContext* context, X_KTHREAD* kthread) {
  auto prcb_for_thread = context->TranslateVirtual(kthread->a_prcb_ptr);
  xboxkrnl::xeKeKfAcquireSpinLock(
      context, &prcb_for_thread->enqueued_processor_threads_lock, false);

  auto thread_priority = kthread->priority;
  auto unk_BD = kthread->unk_BD;
  kthread->unk_BD = 0;
  if ((prcb_for_thread->has_ready_thread_by_priority &
       (1 << thread_priority)) == 0) {
    insert_8009CFE0(context, kthread, unk_BD);
    xboxkrnl::xeKeKfReleaseSpinLock(
        context, &prcb_for_thread->enqueued_processor_threads_lock, 0, false);
    return;
  }

  if (prcb_for_thread->running_idle_thread != 0) {
    xenia_assert(prcb_for_thread->running_idle_thread.m_ptr ==
                 prcb_for_thread->idle_thread.m_ptr);

    prcb_for_thread->running_idle_thread = 0U;
  label_6:
    kthread->thread_state = 3;
    prcb_for_thread->next_thread = context->HostToGuestVirtual(kthread);

    xboxkrnl::xeKeKfReleaseSpinLock(
        context, &prcb_for_thread->enqueued_processor_threads_lock, 0, false);

    uint32_t old_cpu_for_thread = kthread->current_cpu;
    if (old_cpu_for_thread != GetKPCR(context)->prcb_data.current_cpu) {
      /*
          do a non-blocking host IPI here. we need to be sure the original cpu
         this thread belonged to has given it up before we continue
      */
      context->processor->GetCPUThread(old_cpu_for_thread)
          ->TrySendInterruptFromHost(HandleCpuThreadDisownedIPI,
                                     (void*)kthread);
    }
    return;
  }

  X_KTHREAD* next_thread =
      context->TranslateVirtual(prcb_for_thread->next_thread);

  if (!prcb_for_thread->next_thread) {
    if (thread_priority >
        context->TranslateVirtual(prcb_for_thread->current_thread)->priority) {
      context->TranslateVirtual(prcb_for_thread->current_thread)->unk_BD = 1;
      goto label_6;
    }
    insert_8009CFE0(context, kthread, unk_BD);
    xboxkrnl::xeKeKfReleaseSpinLock(
        context, &prcb_for_thread->enqueued_processor_threads_lock, 0, false);
    return;
  }

  kthread->thread_state = 3;

  prcb_for_thread->next_thread = context->HostToGuestVirtual(kthread);
  uint32_t v10 = next_thread->priority;
  auto v11 = context->TranslateVirtual(next_thread->a_prcb_ptr);

  next_thread->thread_state = 1;
  v11->ready_threads_by_priority[v10].InsertHead(next_thread, context);

  v11->has_ready_thread_by_priority =
      v11->has_ready_thread_by_priority | (1 << v10);

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
  auto pcr = GetKPCR(context);
  auto prcb = &pcr->prcb_data;

  auto list_lock = &prcb->enqueued_processor_threads_lock;

  xboxkrnl::xeKeKfAcquireSpinLock(context, list_lock, false);

  auto current_thread = context->TranslateVirtual(prcb->current_thread);

  if (current_thread->unk_B4 <= 0) {
    auto current_process = current_thread->process;
    if (current_process->unk_1B && current_thread->priority >= 0x12u) {
      current_thread->unk_B4 = 0x7FFFFFFF;
    } else {
      auto current_prio = current_thread->priority;
      current_thread->unk_B4 = current_process->unk_0C;
      if ((unsigned int)current_prio < 0x12) {
        current_prio = current_prio - current_thread->unk_BA - 1;
        if (current_prio < current_thread->unk_B9) {
          current_prio = current_thread->unk_B9;
        }
        current_thread->unk_BA = 0;
      }
      current_thread->priority = current_prio;
      if (prcb->next_thread) {
        current_thread->unk_BD = 0;
      } else {
        auto v7 = xeScanForReadyThread(context, prcb, current_prio);
        if (v7) {
          v7->thread_state = 3;
          prcb->next_thread = v7;
        }
      }
    }
  }
  uint32_t unk_mask;

  if (pcr->unk_1A) {
    pcr->unk_1A = 0;
    pcr->unk_1B = 0;
    pcr->unk_19 = 1;

    uint32_t cpunum = context->kernel_state->GetPCRCpuNum(pcr);
    auto hw_thread = context->processor->GetCPUThread(cpunum);
    // todo: this is a variable that isnt in rdata, it might be modified by
    // other things, so this timeout might not be 100% accurate
    hw_thread->SetDecrementerTicks(50000);

    unk_mask = 0xEDB403FF;
  } else {
    if (!pcr->unk_1B) {
      auto result = prcb->next_thread.xlat();
      if (result) {
        // not releasing the spinlock! this appears to be intentional
        return result;
      }
      xboxkrnl::xeKeKfReleaseSpinLock(context, list_lock, 0, false);
      return nullptr;
    }
    pcr->unk_1B = 0;
    pcr->unk_19 = 0;
    unk_mask = 0xF6DBFC03;
  }
  X_KTHREAD* v12;
  if (prcb->unk_mask_64 != unk_mask) {
    auto next_thread = prcb->next_thread.xlat();
    prcb->unk_mask_64 = unk_mask;
    if (next_thread) {
      prcb->next_thread = 0U;
      insert_8009CFE0(context, next_thread, 1);
    }
    auto prcb_idle_thread = prcb->idle_thread.xlat();
    auto current_prio2 = current_thread->priority;
    prcb->running_idle_thread = 0U;
    if (current_thread == prcb_idle_thread ||
        ((1 << current_prio2) & unk_mask) == 0) {
      v12 = xeScanForReadyThread(context, prcb, 0);

      if (!v12) {
        v12 = prcb->idle_thread.xlat();
        prcb->running_idle_thread = v12;
      }
      if (v12 == current_thread) {
        xboxkrnl::xeKeKfReleaseSpinLock(context, list_lock, 0, false);
        return nullptr;
      }
    } else {
      v12 = xeScanForReadyThread(context, prcb, current_prio2);
      if (!v12) {
        xboxkrnl::xeKeKfReleaseSpinLock(context, list_lock, 0, false);

        return nullptr;
      }
    }
    v12->thread_state = 3;
    prcb->next_thread = v12;
  }
  xboxkrnl::xeKeKfReleaseSpinLock(context, list_lock, 0, false);

  return nullptr;
}

// handles DPCS, also switches threads?
// timer related?
void xeHandleDPCsAndThreadSwapping(PPCContext* context) {
  X_KTHREAD* next_thread = nullptr;
  while (true) {
    set_msr_interrupt_bits(context, 0);

    GetKPCR(context)->generic_software_interrupt = 0;
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
      if (!next_thread) {
        return;
      }
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
      break;
    }
  }
  // requeue ourselves
  // GetKPCR(context)->prcb_data.current_thread
  auto& prcb = GetKPCR(context)->prcb_data;
  auto ble = context->TranslateVirtual(prcb.current_thread);
  prcb.next_thread = 0U;
  prcb.current_thread = context->HostToGuestVirtual(next_thread);
  insert_8009D048(context, ble);
  context->kernel_state->ContextSwitch(context, next_thread);
}

void xeEnqueueThreadPostWait(PPCContext* context, X_KTHREAD* thread,
                             X_STATUS wait_result, int priority_increment) {
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
    thread->wait_timeout_timer.table_bucket_entry.flink_ptr = 0U;
    thread->wait_timeout_timer.table_bucket_entry.blink_ptr = 0U;
  }
  auto unk_ptr = thread->unkptr_118;
  if (unk_ptr) {
    auto unk_counter = context->TranslateVirtualBE<uint32_t>(unk_ptr + 0x18);
    *unk_counter++;
  }

  auto prcb = context->TranslateVirtual(thread->a_prcb_ptr);

  xboxkrnl::xeKeKfAcquireSpinLock(
      context, &prcb->enqueued_processor_threads_lock, false);

  auto thread_priority = thread->priority;
  auto thread_process = thread->process;
  if (thread_priority >= 0x12) {
    thread->unk_B4 = thread_process->unk_0C;

  } else {
    auto v15 = thread->unk_BA;
    if (!v15 && !thread->boost_disabled) {
      auto v16 = thread->unk_B9 + priority_increment;
      if (v16 > (int)thread_priority) {
        if (v16 < thread->unk_CA)
          thread->priority = v16;
        else
          thread->priority = thread->unk_CA;
      }
    }
    auto v17 = thread->unk_B9;
    if (v17 >= (unsigned int)thread->unk_CA) {
      thread->unk_B4 = thread_process->unk_0C;
    } else {
      auto v18 = thread->unk_B4 - 10;
      thread->unk_B4 = v18;
      if (v18 <= 0) {
        auto v19 = (unsigned char)(thread->priority - v15 - 1);
        thread->unk_B4 = thread_process->unk_0C;

        thread->priority = v19;
        if (v19 < v17) {
          thread->priority = v17;
        }
        thread->unk_BA = 0;
      }
    }
  }
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

X_STATUS xeNtYieldExecution(PPCContext* context) {
  X_STATUS result;
  auto kpcr = GetKPCR(context);
  auto v1 = context->TranslateVirtual(kpcr->prcb_data.current_thread);
  auto old_irql = kpcr->current_irql;
  kpcr->current_irql = IRQL_DISPATCH;

  v1->unk_A4 = old_irql;
  auto v2 = &kpcr->prcb_data;
  xboxkrnl::xeKeKfAcquireSpinLock(context, &v2->enqueued_processor_threads_lock,
                                  false);

  if (!v2->next_thread) {
    v2->next_thread = xeScanForReadyThread(context, v2, 1);
  }
  if (v2->next_thread) {
    v1->unk_B4 = v1->process->unk_0C;
    auto v4 = v1->priority;
    if (v4 < 0x12) {
      v4 = v4 - v1->unk_BA - 1;
      if (v4 < v1->unk_B9) {
        v4 = v1->unk_B9;
      }
      v1->unk_BA = 0;
    }
    v1->priority = v4;
    insert_8009D048(context, v1);
    xeSchedulerSwitchThread(context);

    result = X_STATUS_SUCCESS;
  } else {
    xboxkrnl::xeKeKfReleaseSpinLock(
        context, &v2->enqueued_processor_threads_lock, 0, false);
    xboxkrnl::xeKfLowerIrql(context, v1->unk_A4);
    result = X_STATUS_NO_YIELD_PERFORMED;
  }
  return result;
}
void scheduler_80097F90(PPCContext* context, X_KTHREAD* thread) {
  auto pcrb = &GetKPCR(context)->prcb_data;

  xboxkrnl::xeKeKfAcquireSpinLock(
      context, &pcrb->enqueued_processor_threads_lock, false);

  auto priority = thread->priority;
  if (priority < 0x12) {
    auto v6 = thread->unk_B9;
    if (v6 < thread->unk_CA) {
      auto v7 = thread->unk_B4 - 10;
      thread->unk_B4 = v7;
      if (v7 <= 0) {
        thread->unk_B4 = thread->process->unk_0C;
        auto v8 = priority - thread->unk_BA - 1;
        if (v8 < (int)v6) {
          v8 = v6;
        }
        thread->priority = v8;
        thread->unk_BA = 0;
        if (pcrb->next_thread) {
          thread->unk_BD = 0;
        } else {
          auto v9 = xeScanForReadyThread(context, pcrb, v8);
          if (v9) {
            v9->thread_state = 3;
            pcrb->next_thread = v9;
          }
        }
      }
    }
  }
  xeDispatcherSpinlockUnlock(context, &pcrb->enqueued_processor_threads_lock,
                             thread->unk_A4);
}

X_STATUS xeSchedulerSwitchThread(PPCContext* context) {
  auto pcr = GetKPCR(context);
  auto prcb = &pcr->prcb_data;

  auto current_thread = prcb->current_thread;
  auto next_thread = prcb->next_thread.xlat();

  if (next_thread) {
  } else {
    auto ready_by_prio = prcb->has_ready_thread_by_priority;
    auto has_ready = ready_by_prio & prcb->unk_mask_64;
    if (has_ready) {
      auto v5 = 31 - xe::lzcnt(has_ready);
      auto v6 = &prcb->ready_threads_by_priority[v5];

      // if the list has a bit set in the mask, it definitely should have an
      // entry
      xenia_assert(!v6->empty(context));

      auto v8 = ready_by_prio ^ (1 << v5);
      next_thread = v6->UnlinkHeadObject(context);

      if (v6->empty(context)) {
        // list is empty now, update mask
        prcb->has_ready_thread_by_priority = v8;
      }
    }
  }

  if (next_thread) {
    prcb->next_thread = 0U;
  } else {
    // idle thread
    auto idle_thread =
        &reinterpret_cast<X_KPCR_PAGE*>(pcr)->idle_process_thread;
    next_thread = idle_thread;
    prcb->running_idle_thread = idle_thread;
  }

  prcb->current_thread = next_thread;
  context->kernel_state->ContextSwitch(context, next_thread);
  pcr = GetKPCR(context);
  auto v9 = next_thread->unk_A4;
  auto result = next_thread->wait_result;
  pcr->current_irql = v9;
  auto v11 = pcr->software_interrupt_state;

  if (v9 < v11) {
    xeDispatchProcedureCallInterrupt(v9, v11, context);
  }
  return result;
}

/*
    this function is quite confusing and likely wrong, probably was written in
   asm

*/
X_STATUS xeSchedulerSwitchThread2(PPCContext* context) {
reenter:
  auto pcr = GetKPCR(context);
  auto prcb = &pcr->prcb_data;
  if (prcb->enqueued_threads_list.next) {
    xeProcessQueuedThreads(context, true);
    goto reenter;
  }

  // this is wrong! its doing something else here,
  // some kind of "try lock" and then falling back to another function
  // xboxkrnl::xeKeKfAcquireSpinLock(
  //  context, &prcb->enqueued_processor_threads_lock, false);
  if (prcb->enqueued_processor_threads_lock.pcr_of_owner.value != 0) {
    while (prcb->enqueued_processor_threads_lock.pcr_of_owner.value) {
      // db16cyc
    }
    goto reenter;
  } else {
    xeKeKfAcquireSpinLock(context, &prcb->enqueued_processor_threads_lock,
                          false);
  }

  context->kernel_state->GetDispatcherLock(context)->pcr_of_owner = 0;
  return xeSchedulerSwitchThread(context);
}

int xeKeSuspendThread(PPCContext* context, X_KTHREAD* thread) {
  int result;
  uint32_t old_irql =
      xboxkrnl::xeKeKfAcquireSpinLock(context, &thread->apc_lock);

  result = thread->suspend_count;
  if (result == 0x7F) {
    xenia_assert(false);
    // raise status here
  }

  if (thread->may_queue_apcs) {
    if (!thread->unk_CB) {
      thread->suspend_count = result + 1;
      if (!result) {
        if (thread->on_suspend.enqueued) {
          context->kernel_state->LockDispatcherAtIrql(context);
          thread->suspend_sema.header.signal_state--;
          context->kernel_state->UnlockDispatcherAtIrql(context);

        } else {
          thread->on_suspend.enqueued = 1;
          xeKeInsertQueueApcHelper(context, &thread->on_suspend, 0);
        }
      }
    }
  }

  xeDispatcherSpinlockUnlock(context, &thread->apc_lock, old_irql);
  return result;
}
int xeKeResumeThread(PPCContext* context, X_KTHREAD* thread) {
  int result;
  uint32_t old_irql =
      xboxkrnl::xeKeKfAcquireSpinLock(context, &thread->apc_lock);

  char suspendcount = thread->suspend_count;
  result = suspendcount;
  if (suspendcount) {
    thread->suspend_count = suspendcount - 1;
    if (suspendcount == 1) {
      context->kernel_state->LockDispatcherAtIrql(context);
      thread->suspend_sema.header.signal_state++;
      xeDispatchSignalStateChange(context, &thread->suspend_sema.header, 0);
      context->kernel_state->UnlockDispatcherAtIrql(context);
    }
  }

  xeDispatcherSpinlockUnlock(context, &thread->apc_lock, old_irql);
  return result;
}

void xeSuspendThreadApcRoutine(PPCContext* context) {
  auto thrd = GetKThread(context);
  xeKeWaitForSingleObject(&thrd->suspend_sema, 2, 0, 0, 0);
}

//very, very incorrect impl
X_STATUS xeKeWaitForSingleObject(PPCContext* context, X_DISPATCH_HEADER* object,
                                 unsigned reason, unsigned processor_mode, bool alertable,
                                 int64_t* timeout) {
  uint32_t old_irql = context->kernel_state->LockDispatcher(context);
  auto kthread = GetKThread(context);

  auto old_r1 = context->r[1];

  context->r[1] -= sizeof(X_KWAIT_BLOCK) * 4;
  uint32_t guest_stash = static_cast<uint32_t>(old_r1 - sizeof(X_KWAIT_BLOCK)* 4);
  X_KWAIT_BLOCK* stash = context->TranslateVirtual<X_KWAIT_BLOCK*>(guest_stash);

  kthread->wait_blocks = guest_stash;
  if (object->signal_state > 0) {
    context->kernel_state->UnlockDispatcher(context, old_irql);
    return X_STATUS_SUCCESS;
  }

  stash[0].object = object;
  stash[0].thread = kthread;
  stash[0].wait_result_xstatus = 0;
  stash[0].wait_type = WAIT_ANY;
  stash[0].next_wait_block = guest_stash;
  util::XeInitializeListHead(&stash[0].wait_list_entry, context);

  util::XeInsertHeadList(&object->wait_list, &stash[0].wait_list_entry,
                         context);

  kthread->alertable = alertable;
  kthread->processor_mode = processor_mode;
  kthread->wait_reason = reason;
  kthread->thread_state = 5;

  X_STATUS wait_status = xeSchedulerSwitchThread2(context);

  context->r[1] = old_r1;

  if (wait_status == X_STATUS_USER_APC) {
    xeProcessUserApcs(context);
  } else if (wait_status == X_STATUS_KERNEL_APC) {
    xeProcessKernelApcs(context);
  } else {
    auto v24 = object->type;

    if ((v24 & 7) == 1) {
      object->signal_state = 0;
    } else if (v24 == 5) {  // sem
      --object->signal_state;
    }
  }
  context->kernel_state->UnlockDispatcher(context, old_irql);
  scheduler_80097F90(context, kthread);
  return wait_status;
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
