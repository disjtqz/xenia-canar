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

template <size_t fmt_len, typename... Ts>
static void SCHEDLOG(PPCContext* context, const char (&fmt)[fmt_len],
                     Ts... args) {
#if 0
#define prefixfmt "(Context {}, Fiber {}, HW Thread {}, Guest Thread {}) "

  char tmpbuf[fmt_len + sizeof(prefixfmt)];

  memcpy(tmpbuf, prefixfmt, sizeof(prefixfmt) - 1);

  memcpy(&tmpbuf[sizeof(prefixfmt) - 1], &fmt[0], fmt_len);

  XELOGE(&tmpbuf[0], (void*)context, (void*)threading::Fiber::GetCurrentFiber(),
         context->kernel_state->GetPCRCpuNum(GetKPCR(context)),
         (void*)GetKThread(context), args...);
#else

#endif
}

static void insert_8009CFE0(PPCContext* context, X_KTHREAD* thread, int unk);
static void insert_8009D048(PPCContext* context, X_KTHREAD* thread);
XE_COMPARISON_NOINLINE
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
  // SCHEDLOG(context, "xeDispatcherSpinlockUnlock irql = {}", irql);
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
      } else {
        __nop();
      }
    } else {
      xboxkrnl::xeKeKfAcquireSpinLock(
          context, &kpcr->prcb_data.enqueued_processor_threads_lock, false);
      auto next_thread = kpcr->prcb_data.next_thread;
      auto v3 = kpcr->prcb_data.current_thread.xlat();
      v3->wait_irql = irql;
      kpcr->prcb_data.next_thread = 0U;
      kpcr->prcb_data.current_thread = next_thread;
      insert_8009D048(context, v3);
      // jump here!
      // r31 = next_thread
      // r30 = current thread
      // definitely switching threads

      context->kernel_state->ContextSwitch(context, next_thread.xlat());

// this is all already done in ContextSwitch!
#if 0
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
#endif
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
  SCHEDLOG(context,
           "xeHandleReadyThreadOnDifferentProcessor kthread = {}, "
           "thread_state = {}",
           (void*)kthread, kthread->thread_state);
  if (kthread->thread_state != 2) {
    // xe::FatalError("Doing some fpu/vmx shit here?");
    // it looks like its saving the fpu and vmx state
    // we don't have to do this i think, because we already have different
    // PPCContext per guest thread
  }
  // https://www.geoffchappell.com/studies/windows/km/ntoskrnl/inc/ntos/ke/kthread_state.htm

  xenia_assert(kthread->a_prcb_ptr.xlat() == v3);
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
// if was_preempted, insert to front of ready list
static void insert_8009CFE0(PPCContext* context, X_KTHREAD* thread,
                            int was_preempted) {
  SCHEDLOG(context, "insert_8009D048 - thread = {}, unk = {}", (void*)thread,
           was_preempted);
  auto priority = thread->priority;
  auto thread_prcb = context->TranslateVirtual(thread->a_prcb_ptr);
  auto thread_ready_list_entry = &thread->ready_prcb_entry;
  thread->thread_state = 1;
  auto& list_for_priority = thread_prcb->ready_threads_by_priority[priority];
  if (was_preempted) {
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
  SCHEDLOG(context, "insert_8009D048 - thread = {}", (void*)thread);
  if (context->TranslateVirtual(thread->another_prcb_ptr) ==
      &GetKPCR(context)->prcb_data) {
    unsigned char unk = thread->was_preempted;
    thread->was_preempted = 0;
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
XE_COMPARISON_NOINLINE
static X_KTHREAD* xeScanForReadyThread(PPCContext* context, X_KPRCB* prcb,
                                       int priority) {
  SCHEDLOG(context, "xeScanForReadyThread - prcb = {}, priority = {}",
           (void*)prcb, priority);
  unsigned v3 = prcb->has_ready_thread_by_priority;
  if ((prcb->unk_mask_64 & ~((1 << priority) - 1) & v3) == 0) {
    return nullptr;
  }
  unsigned int v4 = xe::lzcnt(prcb->unk_mask_64 & ~((1 << priority) - 1) & v3);
  char v5 = 31 - v4;

  auto result = prcb->ready_threads_by_priority[31 - v4].HeadObject(context);

  uint32_t v7 = result->ready_prcb_entry.flink_ptr;
  uint32_t v8 = result->ready_prcb_entry.blink_ptr;
  context->TranslateVirtual<X_LIST_ENTRY*>(v8)->flink_ptr = v7;
  context->TranslateVirtual<X_LIST_ENTRY*>(v7)->blink_ptr = v8;
  if (v8 == v7) {
    prcb->has_ready_thread_by_priority &= ~(1 << v5);
  }
  return result;
}

void HandleCpuThreadDisownedIPI(void* ud) {
  // xenia_assert(false);
  // this is incorrect
  // xeHandleDPCsAndThreadSwapping(cpu::ThreadState::GetContext(), false);
  auto context = cpu::ThreadState::GetContext();
  // hack!!! don't know what the ipi that the kernel sends actually does

  auto kpcr = GetKPCR(context);
  KernelState::HWThreadFor(context)->interrupt_controller()->SetEOI(1);
  GetKPCR(context)->generic_software_interrupt = 2;
}

void xeReallyQueueThread(PPCContext* context, X_KTHREAD* kthread) {
  SCHEDLOG(context, "xeReallyQueueThread - kthread = {}", (void*)kthread);
  auto prcb_for_thread = context->TranslateVirtual(kthread->a_prcb_ptr);
  xboxkrnl::xeKeKfAcquireSpinLock(
      context, &prcb_for_thread->enqueued_processor_threads_lock, false);

  auto thread_priority = kthread->priority;
  auto was_preempted = kthread->was_preempted;
  kthread->was_preempted = 0;
  if ((prcb_for_thread->unk_mask_64 & (1 << thread_priority)) == 0) {
    insert_8009CFE0(context, kthread, was_preempted);
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
          ->SendGuestIPI(HandleCpuThreadDisownedIPI, (void*)kthread);
    }
    return;
  }

  X_KTHREAD* next_thread =
      context->TranslateVirtual(prcb_for_thread->next_thread);

  if (!prcb_for_thread->next_thread) {
    if (thread_priority >
        context->TranslateVirtual(prcb_for_thread->current_thread)->priority) {
      context->TranslateVirtual(prcb_for_thread->current_thread)
          ->was_preempted = 1;
      goto label_6;
    }
    insert_8009CFE0(context, kthread, was_preempted);
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

  v11->has_ready_thread_by_priority |= (1 << v10);

  xboxkrnl::xeKeKfReleaseSpinLock(
      context, &prcb_for_thread->enqueued_processor_threads_lock, 0, false);
}

static void xeProcessQueuedThreads(PPCContext* context,
                                   bool under_dispatcher_lock) {
  SCHEDLOG(context, "xeProcessQueuedThreads - under_dispatcher_lock {}",
           under_dispatcher_lock);
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
  SCHEDLOG(context, "xeSelectThreadDueToTimesliceExpiration");
  auto pcr = GetKPCR(context);
  auto prcb = &pcr->prcb_data;

  auto list_lock = &prcb->enqueued_processor_threads_lock;

  xboxkrnl::xeKeKfAcquireSpinLock(context, list_lock, false);

  auto current_thread = context->TranslateVirtual(prcb->current_thread);

  if (current_thread->quantum <= 0) {
    auto current_process = current_thread->process;
    if (current_process->unk_1B && current_thread->priority >= 0x12u) {
      current_thread->quantum = 0x7FFFFFFF;
    } else {
      auto current_prio = current_thread->priority;
      current_thread->quantum = current_process->quantum;
      if ((unsigned int)current_prio < 0x12) {
        current_prio = current_prio - current_thread->unk_BA - 1;
        if (current_prio < current_thread->unk_B9) {
          current_prio = current_thread->unk_B9;
        }
        current_thread->unk_BA = 0;
      }
      current_thread->priority = current_prio;
      if (prcb->next_thread) {
        current_thread->was_preempted = 0;
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

  auto v13 = prcb->next_thread;
  if (v13) {
    return v13.xlat();
  }
  xboxkrnl::xeKeKfReleaseSpinLock(context, list_lock, 0, false);

  return nullptr;
}

// handles DPCS, also switches threads?
// timer related?
void xeHandleDPCsAndThreadSwapping(PPCContext* context, bool from_idle_loop) {
  SCHEDLOG(context, "xeHandleDPCsAndThreadSwapping");

  X_KTHREAD* next_thread = nullptr;
  while (true) {
    set_msr_interrupt_bits(context, 0);

    GetKPCR(context)->generic_software_interrupt = 0;
    if (!GetKPCR(context)->prcb_data.queued_dpcs_list_head.empty(context) ||
        GetKPCR(context)->timer_pending) {
      // todo: incomplete!
      if (from_idle_loop) {
        xeExecuteDPCList2(context, GetKPCR(context)->timer_pending,
                          GetKPCR(context)->prcb_data.queued_dpcs_list_head, 0);
      } else {
        uint32_t altstack = GetKPCR(context)->use_alternative_stack;

        xenia_assert(altstack == 0);

        uint32_t r4 = GetKPCR(context)->alt_stack_base_ptr;
        GetKPCR(context)->use_alternative_stack =
            static_cast<uint32_t>(context->r[1]);
        /*
          addi      r4, r4, -320
     subf      r4, r1, r4
       addi      r5, r1, 0xF0
                stwux     r5, r1, r4
        */

        SCHEDLOG(context,
                 "xeHandleDPCsAndThreadSwapping - entering xeExecuteDPCList2");
        xeExecuteDPCList2(context, GetKPCR(context)->timer_pending,
                          GetKPCR(context)->prcb_data.queued_dpcs_list_head, 0);
        GetKPCR(context)->use_alternative_stack = 0;
      }
    }
    set_msr_interrupt_bits(context, 0xFFFF8000);

    if (GetKPCR(context)->prcb_data.enqueued_threads_list.next) {
      SCHEDLOG(context,
               "xeHandleDPCsAndThreadSwapping - entering "
               "xeProcessQueuedThreads");
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
    xboxkrnl::xeKeKfAcquireSpinLock(
        context, &GetKPCR(context)->prcb_data.enqueued_processor_threads_lock,
        false);
    // some kind of lock acquire function here??

    uint32_t thrd_u = GetKPCR(context)->prcb_data.next_thread.m_ptr;

    if (from_idle_loop && thrd_u == GetKPCR(context)->prcb_data.idle_thread) {
      GetKPCR(context)->prcb_data.next_thread = 0U;
      xboxkrnl::xeKeKfReleaseSpinLock(
          context, &GetKPCR(context)->prcb_data.enqueued_processor_threads_lock,
          0, false);
      return;
    }

    if (!thrd_u) {
      next_thread = nullptr;
    } else {
      next_thread = context->TranslateVirtual<X_KTHREAD*>(thrd_u);
      break;
    }
  }
  SCHEDLOG(context, "xeHandleDPCsAndThreadSwapping - Got a new next thread");
  // requeue ourselves
  // GetKPCR(context)->prcb_data.current_thread
  auto& prcb = GetKPCR(context)->prcb_data;
  auto ble = context->TranslateVirtual(prcb.current_thread);
  prcb.next_thread = 0U;
  prcb.current_thread = context->HostToGuestVirtual(next_thread);
  // idle loop does not reinsert itself!
  if (!from_idle_loop) {
    insert_8009D048(context, ble);
  }
  context->kernel_state->ContextSwitch(context, next_thread, from_idle_loop);
}

void xeEnqueueThreadPostWait(PPCContext* context, X_KTHREAD* thread,
                             X_STATUS wait_result, int priority_increment) {
  SCHEDLOG(context,
           "xeEnqueueThreadPostWait - thread {}, wait_result {:04X}, "
           "priority_increment {}",
           (void*)thread, wait_result, priority_increment);
  xenia_assert(thread->thread_state == 5);
  thread->wait_result = thread->wait_result | wait_result;
  auto kpcr = GetKPCR(context);
  xenia_assert(kpcr->current_irql == IRQL_DISPATCH);

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
  auto unk_ptr = thread->queue;
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
    thread->quantum = thread_process->quantum;

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
      thread->quantum = thread_process->quantum;
    } else {
      auto v18 = thread->quantum - 10;
      thread->quantum = v18;
      if (v18 <= 0) {
        auto v19 = (unsigned char)(thread->priority - v15 - 1);
        thread->quantum = thread_process->quantum;

        thread->priority = v19;
        if (v19 < v17) {
          thread->priority = v17;
        }
        thread->unk_BA = 0;
      }
    }
  }
  thread->thread_state = 6;

#if 0
  thread->ready_prcb_entry.flink_ptr = prcb->enqueued_threads_list.next;

  prcb->enqueued_threads_list.next =
      context->HostToGuestVirtual(&thread->ready_prcb_entry);
#else
  thread->ready_prcb_entry.flink_ptr =
      GetKPCR(context)->prcb_data.enqueued_threads_list.next;
  GetKPCR(context)->prcb_data.enqueued_threads_list.next =
      context->HostToGuestVirtual(&thread->ready_prcb_entry);
#endif
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
                                 int increment) {
  SCHEDLOG(context, "xeDispatchSignalStateChange - header {}, increment = {}",
           (void*)header, increment);
  auto waitlist_head = &header->wait_list;

  // hack!
  // happens in marathon durandal. todo: figure out why
  if (waitlist_head->blink_ptr == 0 && waitlist_head->flink_ptr == 0) {
    return;
  }

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
    xeEnqueueThreadPostWait(context, v7, v6->wait_result_xstatus, increment);
  LABEL_23:;
  }
}

X_STATUS xeNtYieldExecution(PPCContext* context) {
  SCHEDLOG(context, "xeNtYieldExecution");
  X_STATUS result;
  auto kpcr = GetKPCR(context);
  auto v1 = context->TranslateVirtual(kpcr->prcb_data.current_thread);
  auto old_irql = kpcr->current_irql;
  kpcr->current_irql = IRQL_DISPATCH;

  v1->wait_irql = old_irql;
  auto v2 = &kpcr->prcb_data;
  xboxkrnl::xeKeKfAcquireSpinLock(context, &v2->enqueued_processor_threads_lock,
                                  false);
  X_KTHREAD* next_thread = context->TranslateVirtual(v2->next_thread);
  if (!next_thread) {
    next_thread = xeScanForReadyThread(context, v2, 1);
    v2->next_thread = context->HostToGuestVirtual(next_thread);
  }
  if (next_thread) {
    v1->quantum = v1->process->quantum;
    int v4 = v1->priority;
    if ((unsigned int)v4 < 0x12) {
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
    xboxkrnl::xeKfLowerIrql(context, v1->wait_irql);
    result = X_STATUS_NO_YIELD_PERFORMED;
  }
  return result;
}
XE_COMPARISON_NOINLINE
void scheduler_80097F90(PPCContext* context, X_KTHREAD* thread) {
  SCHEDLOG(context, "scheduler_80097F90 - thread {}", (void*)thread);
  auto pcrb = &GetKPCR(context)->prcb_data;

  xboxkrnl::xeKeKfAcquireSpinLock(
      context, &pcrb->enqueued_processor_threads_lock, false);

  unsigned int priority = thread->priority;
  if (priority < 0x12) {
    unsigned int v6 = thread->unk_B9;
    if (v6 < thread->unk_CA) {
      int v7 = thread->quantum - 10;
      thread->quantum = v7;
      if (v7 <= 0) {
        thread->quantum = thread->process->quantum;
        int v8 = priority - thread->unk_BA - 1;
        if (v8 < (int)v6) {
          v8 = v6;
        }
        thread->priority = v8;
        thread->unk_BA = 0;
        if (pcrb->next_thread) {
          thread->was_preempted = 0;
        } else {
          X_KTHREAD* v9 = xeScanForReadyThread(context, pcrb, v8);
          if (v9) {
            v9->thread_state = 3;
            pcrb->next_thread = v9;
          }
        }
      }
    }
  }
  xeDispatcherSpinlockUnlock(context, &pcrb->enqueued_processor_threads_lock,
                             thread->wait_irql);
}
XE_COMPARISON_NOINLINE
X_STATUS xeSchedulerSwitchThread(PPCContext* context) {
  SCHEDLOG(context, "xeSchedulerSwitchThread");
  auto pcr = GetKPCR(context);
  auto prcb = &pcr->prcb_data;

  auto current_thread = prcb->current_thread;
  auto next_thread = prcb->next_thread.xlat();

  if (next_thread) {
  } else {
    unsigned int ready_by_prio = prcb->has_ready_thread_by_priority;
    int has_ready = ready_by_prio & prcb->unk_mask_64;
    if (has_ready) {
      unsigned int v5 = 31 - xe::lzcnt(has_ready);
      auto v6 = &prcb->ready_threads_by_priority[v5];

      // if the list has a bit set in the mask, it definitely should have an
      // entry
      xenia_assert(!v6->empty(context));

      int v8 = ready_by_prio ^ (1 << v5);
      next_thread = v6->UnlinkHeadObject(context);

      if (v6->empty(context)) {
        // list is empty now, update mask
        prcb->has_ready_thread_by_priority = v8;
      }
    } else {
      unsigned i = 0;
      for (auto&& thrdlist : prcb->ready_threads_by_priority) {
        if (prcb->unk_mask_64 & (1U << i)) {
          xenia_assert(thrdlist.empty(context));
        }
        ++i;
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
  auto result = context->kernel_state->ContextSwitch(context, next_thread);
  pcr = GetKPCR(context);
#if 0
  auto v9 = next_thread->unk_A4;
  auto result = next_thread->wait_result;
  pcr->current_irql = v9;
  auto v11 = pcr->software_interrupt_state;

  if (v9 < v11) {
    xeDispatchProcedureCallInterrupt(v9, v11, context);
  }
#endif
  return result;
}

/*
    this function is quite confusing and likely wrong, probably was written in
   asm

*/
XE_COMPARISON_NOINLINE
X_STATUS xeSchedulerSwitchThread2(PPCContext* context) {
  SCHEDLOG(context, "xeSchedulerSwitchThread2");
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
  auto disp = context->kernel_state->GetDispatcherLock(context);
  xenia_assert(disp->pcr_of_owner == static_cast<uint32_t>(context->r[13]));

  disp->pcr_of_owner = 0;
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
          XELOGE("Just using suspend signal state decrement");
          context->kernel_state->LockDispatcherAtIrql(context);
          thread->suspend_sema.header.signal_state--;
          context->kernel_state->UnlockDispatcherAtIrql(context);

        } else {
          XELOGE("Enqueuing suspendthread apc");
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
    XELOGE("New suspendcount {}", (int)suspendcount - 1);
    if (suspendcount == 1) {
      XELOGE("Awaking for suspendcount");
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
  XELOGE("xeSuspendThreadApcRoutine called");
  auto thrd = GetKThread(context);
  xeKeWaitForSingleObject(context, &thrd->suspend_sema.header, 2, 0, 0, 0);
  XELOGE("xeSuspendThreadApcRoutine awoken");
}

X_STATUS xeKeWaitForSingleObject(PPCContext* context, X_DISPATCH_HEADER* object,
                                 unsigned reason, unsigned processor_mode,
                                 bool alertable, int64_t* timeout) {
  int64_t tmp_timeout;
  auto this_thread = GetKThread(context);

  uint32_t guest_stash =
      context->HostToGuestVirtual(&this_thread->scratch_waitblock_memory);

  X_KWAIT_BLOCK* stash = &this_thread->scratch_waitblock_memory[0];

  auto reason2 = reason;
  if (this_thread->wait_next)
    this_thread->wait_next = 0;
  else {
    this_thread->wait_irql = context->kernel_state->LockDispatcher(context);
  }

  X_STATUS v14;
  uint64_t v11 = 0;
  auto v12 = timeout;
  while (1) {
    if (this_thread->deferred_apc_software_interrupt_state &&
        !this_thread->wait_irql) {
      xeDispatcherSpinlockUnlock(
          context, context->kernel_state->GetDispatcherLock(context), 0);
      goto LABEL_41;
    }
    this_thread->wait_result = 0;

    auto obj_signalstate = object->signal_state;
    if (object->type == 2) {
      X_KMUTANT* mutant = reinterpret_cast<X_KMUTANT*>(object);
      if (obj_signalstate > 0 || this_thread == mutant->owner.xlat()) {
        if (obj_signalstate != 0x80000000) {
          auto v20 = mutant->header.signal_state - 1;
          mutant->header.signal_state = v20;
          if (!v20) {
            auto v21 = mutant->abandoned;
            mutant->owner = this_thread;
            if (v21 == 1) {
              mutant->abandoned = 0;
              this_thread->wait_result = 128;
            }
            auto v22 = this_thread->mutants_list.blink_ptr;
            auto v23 = v22->flink_ptr;
            mutant->unk_list.blink_ptr = v22;
            mutant->unk_list.flink_ptr = v23;
            v23->blink_ptr = &mutant->unk_list;
            v22->flink_ptr = &mutant->unk_list;
          }
          v14 = this_thread->wait_result;
          goto LABEL_57;
        }
        xeDispatcherSpinlockUnlock(
            context, context->kernel_state->GetDispatcherLock(context),
            this_thread->wait_irql);

        // X_STATUS_MUTANT_LIMIT_EXCEEDED
        // should raise status
        xenia_assert(false);
      }
      goto LABEL_16;
    }
    if (obj_signalstate > 0) {
      break;
    }

  LABEL_16:
    this_thread->wait_blocks = guest_stash;
    stash->object = object;
    stash->wait_result_xstatus = 0;
    stash->wait_type = WAIT_ANY;
    stash->thread = this_thread;
    if (alertable) {
      if (this_thread->alerted[processor_mode]) {
        v14 = X_STATUS_ALERTED;
        this_thread->alerted[processor_mode] = 0;
        goto LABEL_55;
      }
      if (processor_mode &&
          !util::XeIsListEmpty(&this_thread->apc_lists[1], context)) {
        this_thread->user_apc_pending = 1;
      LABEL_54:
        v14 = X_STATUS_USER_APC;
      LABEL_55:
        xeDispatcherSpinlockUnlock(
            context, context->kernel_state->GetDispatcherLock(context),
            this_thread->wait_irql);
        goto LABEL_58;
      }
      if (this_thread->alerted[0]) {
        v14 = X_STATUS_ALERTED;
        this_thread->alerted[0] = 0;
        goto LABEL_55;
      }
    } else if (processor_mode && this_thread->user_apc_pending) {
      goto LABEL_54;
    }
    if (timeout) {
      if (!*timeout ||
          (stash->next_wait_block = &this_thread->wait_timeout_block,
           this_thread->wait_timeout_timer.header.wait_list.flink_ptr =
               &this_thread->wait_timeout_block.wait_list_entry,
           this_thread->wait_timeout_timer.header.wait_list.blink_ptr =
               &this_thread->wait_timeout_block.wait_list_entry,
           this_thread->wait_timeout_block.next_wait_block = guest_stash,
           !xboxkrnl::XeInsertGlobalTimer(
               context, &this_thread->wait_timeout_timer, *timeout))) {
        v14 = X_STATUS_TIMEOUT;
        goto LABEL_57;
      }
      v11 = this_thread->wait_timeout_timer.due_time;
    } else {
      stash->next_wait_block = guest_stash;
    }
    auto v15 = object->wait_list.blink_ptr;
    stash->wait_list_entry.flink_ptr = &object->wait_list;
    stash->wait_list_entry.blink_ptr = v15;
    v15->flink_ptr = guest_stash;
    object->wait_list.blink_ptr = guest_stash;

    uint32_t v16 = this_thread->queue;
    if (v16) {
      xeKeSignalQueue(context, context->TranslateVirtual<X_KQUEUE*>(v16));
    }

    auto v17 = (unsigned __int8)this_thread->wait_irql;
    this_thread->alertable = alertable;
    this_thread->processor_mode = processor_mode;
    this_thread->wait_reason = reason2;
    this_thread->thread_state = 5;
    v14 = xeSchedulerSwitchThread2(context);

    if (v14 == X_STATUS_USER_APC) {
      xeProcessUserApcs(context);
    }
    if (v14 != X_STATUS_KERNEL_APC) {
      return v14;
    }
    if (timeout) {
      if (*timeout < 0) {
        tmp_timeout =
            context->kernel_state->GetKernelInterruptTime() - *timeout;
        timeout = &tmp_timeout;
      } else {
        timeout = v12;
      }
    }
  LABEL_41:
    this_thread->wait_irql = context->kernel_state->LockDispatcher(context);
  }
  auto obj_type = object->type;
  if ((obj_type & 7) == 1) {
    object->signal_state = 0;
  } else if (obj_type == 5) {
    --object->signal_state;
  }
  v14 = X_STATUS_SUCCESS;
LABEL_57:
  context->kernel_state->UnlockDispatcherAtIrql(context);
  scheduler_80097F90(context, this_thread);
LABEL_58:
  if (v14 == X_STATUS_USER_APC) {
    xeProcessUserApcs(context);
  }
  return v14;
}

void xeKeSetAffinityThread(PPCContext* context, X_KTHREAD* thread,
                           uint32_t affinity, uint32_t* prev_affinity) {
  uint32_t irql = context->kernel_state->LockDispatcher(context);
  auto old_cpu = thread->current_cpu;
  uint32_t affinity_to_cpu = 31 - xe::lzcnt(affinity);
  if (old_cpu != affinity_to_cpu) {
    thread->another_prcb_ptr =
        &context->kernel_state->KPCRPageForCpuNumber(affinity_to_cpu)
             ->pcr.prcb_data;

    if (old_cpu == GetKPCR(context)->prcb_data.current_cpu) {
      if (thread->thread_state != 6) {
        xeHandleReadyThreadOnDifferentProcessor(context, thread);
      }
    } else {
      // todo: args are undefined in ida! find out why
      xeKeInsertQueueDpc(&thread->a_prcb_ptr->switch_thread_processor_dpc, 0, 0,
                         context);
    }
  }

  xeDispatcherSpinlockUnlock(
      context, context->kernel_state->GetDispatcherLock(context), irql);
  *prev_affinity = 1U << old_cpu;
}
void xeKeSetPriorityClassThread(PPCContext* context, X_KTHREAD* thread,
                                bool a2) {
  SCHEDLOG(context, "xeKeSetPriorityClassThread - thread {}, a2 {}",
           (void*)thread, a2);
  uint32_t old_irql = context->kernel_state->LockDispatcher(context);
  xboxkrnl::xeKeKfAcquireSpinLock(
      context, &thread->a_prcb_ptr->enqueued_processor_threads_lock, false);

  auto v8 = thread->unk_C9;
  auto v9 = thread->unk_C9;
  auto v10 = a2 == 0 ? 5 : 13;
  char v11 = v10 - v9;
  if (v10 != v9) {
    auto v12 = thread->priority;
    thread->unk_C9 = v10;
    auto v13 = thread->unk_C8 + v11;
    auto v14 = thread->unk_B9 + v11;
    auto v15 = thread->unk_CA + v11;
    thread->unk_C8 = v13;
    thread->unk_B9 = v14;
    thread->unk_CA = v15;
    if (v12 < 0x12) {
      auto v16 = thread->process;
      thread->unk_BA = 0;
      thread->quantum = v16->quantum;
      xeKeChangeThreadPriority(context, thread, v14);
    }
  }

  xboxkrnl::xeKeKfReleaseSpinLock(
      context, &thread->a_prcb_ptr->enqueued_processor_threads_lock, 0, false);
  xeDispatcherSpinlockUnlock(
      context, context->kernel_state->GetDispatcherLock(context), old_irql);
}

void xeKeChangeThreadPriority(PPCContext* context, X_KTHREAD* thread,
                              int priority) {
  SCHEDLOG(context, "xeKeChangeThreadPriority - thread {}, a2 {}",
           (void*)thread, priority);
  int prio = thread->priority;
  auto thread_prcb = thread->a_prcb_ptr;

  if (prio == priority) {
    SCHEDLOG(context, "Skipping, priority is the same");
    return;
  }
  auto thread_state = thread->thread_state;
  thread->priority = priority;

  // todo: lzcnt & 0x20 is just a zero test
  bool v7 = (xe::lzcnt(thread_prcb->unk_mask_64 & (1 << priority)) & 0x20) == 0;
  X_KTHREAD* new_next_thread;
  switch (thread_state) {
    case 1: {
      auto v17 = &thread->ready_prcb_entry;
      auto v18 = thread->ready_prcb_entry.flink_ptr;
      auto v19 = thread->ready_prcb_entry.blink_ptr;
      v19->flink_ptr = v18;
      v18->blink_ptr = v19;
      if (v19 == v18) {
        thread_prcb->has_ready_thread_by_priority =
            thread_prcb->has_ready_thread_by_priority & (~(1 << prio));
      }
      thread->thread_state = 6;
      auto kpcr = GetKPCR(context);
      v17->flink_ptr = kpcr->prcb_data.enqueued_threads_list.next;
      kpcr->prcb_data.enqueued_threads_list.next =
          context->HostToGuestVirtual(v17);
      break;
    }
    case 2: {
      if (thread_prcb->next_thread) {
        return;
      }
      if (!v7) {
        goto LABEL_9;
      }
      if (priority < prio) {
        new_next_thread =
            xeScanForReadyThread(context, thread_prcb.xlat(), priority);
        if (new_next_thread) {
          new_next_thread->thread_state = 3;
          thread_prcb->next_thread = new_next_thread;
          return;
        }
      }
      break;
    }
    case 3: {
      if (!v7) {
        thread->thread_state = 1;
        auto v8 = &thread_prcb->ready_threads_by_priority[priority];
        auto v9 = v8->flink_ptr;
        thread->ready_prcb_entry.blink_ptr = v8;
        thread->ready_prcb_entry.flink_ptr = v9;
        v9->blink_ptr = &thread->ready_prcb_entry;
        v8->flink_ptr = &thread->ready_prcb_entry;
        thread_prcb->has_ready_thread_by_priority =
            (1 << priority) | thread_prcb->has_ready_thread_by_priority;
      LABEL_9:
        new_next_thread = xeScanForReadyThread(context, thread_prcb.xlat(), 0);
        if (!new_next_thread) {
          new_next_thread = thread_prcb->idle_thread.xlat();
          thread_prcb->running_idle_thread = new_next_thread;
        }
        new_next_thread->thread_state = 3;
        thread_prcb->next_thread = new_next_thread;
        return;
      }
      if (priority < prio) {
        auto v11 = xeScanForReadyThread(context, thread_prcb.xlat(), priority);
        if (v11) {
          v11->thread_state = 3;
          thread_prcb->next_thread = v11;
          int v12 = thread->priority;
          auto v13 = thread->a_prcb_ptr;
          thread->thread_state = 1;
          int v14 = 1 << v12;
          auto v15 = &v13->ready_threads_by_priority[v12];
          auto v16 = v15->flink_ptr;
          thread->ready_prcb_entry.blink_ptr = v15;
          thread->ready_prcb_entry.flink_ptr = v16;
          v16->blink_ptr = &thread->ready_prcb_entry;
          v15->flink_ptr = &thread->ready_prcb_entry;
          v13->has_ready_thread_by_priority =
              v13->has_ready_thread_by_priority | v14;
        }
      }
      break;
    }
    default:
      return;
  }
}

X_STATUS xeKeDelayExecutionThread(PPCContext* context, char mode,
                                  bool alertable, int64_t* interval) {
  auto thread = GetKThread(context);

  int64_t v6 = *interval;
  if (thread->wait_next)
    thread->wait_next = 0;
  else
    thread->wait_irql = context->kernel_state->LockDispatcher(context);
  auto v7 = v6;
  X_STATUS result;
  while (1) {
    if (thread->deferred_apc_software_interrupt_state && !thread->wait_irql) {
      xeDispatcherSpinlockUnlock(
          context, context->kernel_state->GetDispatcherLock(context), 0);
      goto LABEL_28;
    }
    if (alertable) {
      if (thread->alerted[mode]) {
        result = X_STATUS_ALERTED;
        thread->alerted[mode] = 0;
        goto LABEL_32;
      }
      if (mode && !thread->apc_lists[1].empty(context)) {
        thread->user_apc_pending = 1;
      LABEL_31:
        result = X_STATUS_USER_APC;
      LABEL_32:
        xeDispatcherSpinlockUnlock(
            context, context->kernel_state->GetDispatcherLock(context),
            thread->wait_irql);
        if (result == X_STATUS_USER_APC) {
          xboxkrnl::xeProcessUserApcs(context);
        }
        return result;
      }
      if (thread->alerted[0]) {
        result = X_STATUS_ALERTED;
        thread->alerted[0] = 0;
        goto LABEL_32;
      }
    } else if (mode && thread->user_apc_pending) {
      goto LABEL_31;
    }
    thread->wait_result = 0;
    thread->wait_blocks = &thread->wait_timeout_block;
    thread->wait_timeout_block.next_wait_block = &thread->wait_timeout_block;

    thread->wait_timeout_timer.header.wait_list.flink_ptr =
        &thread->wait_timeout_block.wait_list_entry;

    thread->wait_timeout_timer.header.wait_list.blink_ptr =
        &thread->wait_timeout_block.wait_list_entry;

    if (!XeInsertGlobalTimer(context, &thread->wait_timeout_timer, v6)) {
      break;
    }
    uint32_t v9 = thread->queue;
    v6 = thread->wait_timeout_timer.due_time;
    if (v9) {
      xeKeSignalQueue(context, context->TranslateVirtual<X_KQUEUE*>(v9));
    }
    thread->alertable = alertable;
    thread->processor_mode = mode;
    thread->wait_reason = 1;
    thread->thread_state = 5;

    result = xeSchedulerSwitchThread2(context);
    if (result == X_STATUS_USER_APC) {
      xeProcessUserApcs(context);
    }
    if (result != X_STATUS_KERNEL_APC) {
      if (result == X_STATUS_TIMEOUT) {
        result = X_STATUS_SUCCESS;
      }
      return result;
    }
    // this part is a bit fucked up, not sure this is right
    if (v7 < 0) {
      v6 = static_cast<int64_t>(
               context->kernel_state->GetKernelInterruptTime()) -
           v6;

    } else {
      v6 = v7;
    }

  LABEL_28:
    thread->wait_irql = context->kernel_state->LockDispatcher(context);
  }
  if (v6) {
    context->kernel_state->UnlockDispatcherAtIrql(context);
    scheduler_80097F90(context, thread);
    result = X_STATUS_SUCCESS;
  } else {
    xeDispatcherSpinlockUnlock(
        context, context->kernel_state->GetDispatcherLock(context),
        thread->wait_irql);
    result = xeNtYieldExecution(context);
  }
  return result;
}

int32_t xeKeSetBasePriorityThread(PPCContext* context, X_KTHREAD* thread,
                                  int increment) {
  uint32_t old_irql = context->kernel_state->LockDispatcher(context);
  xboxkrnl::xeKeKfAcquireSpinLock(
      context, &thread->a_prcb_ptr->enqueued_processor_threads_lock, false);

  int v8 = thread->unk_C9;
  int v9 = thread->unk_B9;
  int result = v9 - v8;
  if (thread->unk_B8) {
    result = 16 * thread->unk_B8;
  }
  thread->unk_B8 = 0;

  if (std::abs(increment) >= 16) {
    char v11 = 1;
    if (increment <= 0) {
      v11 = -1;
    }
    thread->unk_B8 = v11;
  }

  int v12 = thread->unk_CA;

  int v13 = v8 + increment;
  if (v8 + increment <= v12) {
    if (v13 < thread->unk_C8) {
      v13 = thread->unk_C8;
    }
  } else {
    v13 = thread->unk_CA;
  }
  int v14;
  if (thread->unk_B8) {
    v14 = v13;
  } else {
    v14 = thread->priority - thread->unk_BA - v9 + v13;
    if (v14 > v12) {
      v14 = thread->unk_CA;
    }
  }
  int v15 = thread->priority;
  thread->unk_B9 = v13;
  thread->unk_BA = 0;
  if (v14 != v15) {
    thread->quantum = thread->process->quantum;
    xeKeChangeThreadPriority(context, thread, v14);
  }

  xboxkrnl::xeKeKfReleaseSpinLock(
      context, &thread->a_prcb_ptr->enqueued_processor_threads_lock, 0, false);
  xeDispatcherSpinlockUnlock(
      context, context->kernel_state->GetDispatcherLock(context), old_irql);
  return result;
}
uint32_t xeKeWaitForSingleObjectEx(
    PPCContext* context,
    ShiftedPointer<X_DISPATCH_HEADER, X_OBJECT_HEADER, 16> wait,
    unsigned char waitmode, bool alertable, int64_t* timeout) {
  return xeKeWaitForSingleObject(context,
                                 xeObGetWaitableObject(context, wait.m_base), 3,
                                 waitmode, alertable, timeout);
}

X_STATUS xeKeSignalAndWaitForSingleObjectEx(
    PPCContext* context,
    ShiftedPointer<X_DISPATCH_HEADER, X_OBJECT_HEADER, 16> signal,
    ShiftedPointer<X_DISPATCH_HEADER, X_OBJECT_HEADER, 16> wait,
    unsigned char mode, bool alertable, int64_t* timeout) {
  X_DISPATCH_HEADER* waiter = xeObGetWaitableObject(context, wait.m_base);

  X_STATUS result = X_STATUS_SUCCESS;
  auto signal_type_ptr = signal.GetAdjacent()->object_type_ptr;
  auto globals = context->kernel_state->GetKernelGuestGlobals();

  if (signal_type_ptr ==
      static_cast<uint32_t>(globals +
                            offsetof(KernelGuestGlobals, ExEventObjectType))) {
    xeKeSetEvent(context, reinterpret_cast<X_KEVENT*>(signal.m_base), 1, 1);

  } else if (signal_type_ptr ==
             static_cast<uint32_t>(
                 globals + offsetof(KernelGuestGlobals, ExMutantObjectType))) {
    xeKeReleaseMutant(context, reinterpret_cast<X_KMUTANT*>(signal.m_base), 1,
                      0, 1);
    uint32_t cstatus = context->CatchStatus();
    if (cstatus) {
      return cstatus;
    }

  } else if (signal_type_ptr ==
             static_cast<uint32_t>(globals + offsetof(KernelGuestGlobals,
                                                      ExSemaphoreObjectType))) {
    xeKeReleaseSemaphore(
        context, reinterpret_cast<X_KSEMAPHORE*>(signal.m_base), 1, 1, 1);
    uint32_t cstatus = context->CatchStatus();
    if (cstatus) {
      return cstatus;
    }
  } else {
    result = X_STATUS_OBJECT_TYPE_MISMATCH;
  }
  if (result >= X_STATUS_SUCCESS) {
    result =
        xeKeWaitForSingleObject(context, waiter, 3, mode, alertable, timeout);
  }
  return result;
}

int32_t xeKeQueryBasePriorityThread(PPCContext* context, X_KTHREAD* thread) {
  uint32_t old_irql = context->kernel_state->LockDispatcher(context);
  char v4 = thread->unk_B8;
  int v5 = thread->unk_B9 - thread->unk_C9;
  if (v4) {
    v5 = 16 * v4;
  }
  xeDispatcherSpinlockUnlock(
      context, context->kernel_state->GetDispatcherLock(context), old_irql);
  return v5;
}

X_STATUS xeKeWaitForMultipleObjects(
    PPCContext* context, unsigned int num_objects, X_DISPATCH_HEADER** objects,
    unsigned wait_type, unsigned reason, unsigned char mode, int alertable,
    int64_t* timeout, X_KWAIT_BLOCK* wait_blocks) {
  X_STATUS result;
  auto thread = GetKThread(context);
  if (thread->wait_next)
    thread->wait_next = 0;
  else {
    thread->wait_irql = context->kernel_state->LockDispatcher(context);
  }
  unsigned int v21;
  int64_t v43 = 0;
  ShiftedPointer<xe::be<uint16_t>, X_KWAIT_BLOCK, 0x14> wait_blocks_shifted =
      nullptr;

  int64_t v16 = 0;  // v43;
  auto v17 = context->TranslateVirtual<X_KWAIT_BLOCK*>(v43 >> 32);
  auto timeout2 = timeout;
  int v20;
  int64_t other_timer_storage;
  while (true) {
    auto v19 = thread->deferred_apc_software_interrupt_state;
    thread->wait_blocks = wait_blocks;
    if (v19 && !thread->wait_irql) {
      xeDispatcherSpinlockUnlock(
          context, context->kernel_state->GetDispatcherLock(context), 0);
      goto LABEL_60;
    }
    v20 = 1;
    thread->wait_result = 0;
    v21 = 0;  // HIDWORD(v16);
    if (num_objects) {
      break;
    }
  LABEL_32:
    if (wait_type == 0 && v20) {
      v17->next_wait_block = wait_blocks;
      xeHandleWaitTypeAll(context, v17);
      result = thread->wait_result;
      goto LABEL_75;
    }
    if (alertable) {
      if (thread->alerted[mode]) {
        result = X_STATUS_ALERTED;
        thread->alerted[mode] = 0;
        goto LABEL_73;
      }
      if (mode && !util::XeIsListEmpty(&thread->apc_lists[1], context)) {
        thread->user_apc_pending = 1;
      LABEL_72:
        result = X_STATUS_USER_APC;
      LABEL_73:
        xeDispatcherSpinlockUnlock(
            context, context->kernel_state->GetDispatcherLock(context),
            thread->wait_irql);
        goto deliver_apc_and_return;
      }
      if (thread->alerted[0]) {
        result = X_STATUS_ALERTED;
        thread->alerted[0] = 0;
        goto LABEL_73;
      }
    } else if (mode && thread->user_apc_pending) {
      goto LABEL_72;
    }
    if (timeout) {
      if (!*timeout || (v17->next_wait_block = &thread->wait_timeout_block,
                        v17 = &thread->wait_timeout_block,
                        thread->wait_timeout_timer.header.wait_list.flink_ptr =
                            &thread->wait_timeout_timer.header.wait_list,
                        thread->wait_timeout_timer.header.wait_list.blink_ptr =
                            &thread->wait_timeout_timer.header.wait_list,

                        !XeInsertGlobalTimer(
                            context, &thread->wait_timeout_timer, *timeout))) {
        result = X_STATUS_TIMEOUT;
        goto LABEL_75;
      }
      v16 = thread->wait_timeout_timer.due_time;
    }
    v17->next_wait_block = wait_blocks;
    v17 = wait_blocks;
    do {
      auto v32 = &v17->object->wait_list;
      auto v33 = v17->object->wait_list.blink_ptr;
      v17->wait_list_entry.flink_ptr = v32;
      v17->wait_list_entry.blink_ptr = v33;
      v33->flink_ptr = &v17->wait_list_entry;
      v32->blink_ptr = &v17->wait_list_entry;
      v17 = v17->next_wait_block.xlat();
    } while (v17 != wait_blocks);

    uint32_t v34 = thread->queue;
    if (v34) {
      xeKeSignalQueue(context, context->TranslateVirtual<X_KQUEUE*>(v34));
    }
    thread->alertable = alertable;
    thread->processor_mode = mode;
    thread->wait_reason = reason;
    auto v35 = (unsigned char)thread->wait_irql;
    thread->thread_state = 5;
    result = xeSchedulerSwitchThread2(context);
    if (result == X_STATUS_USER_APC) {
      xeProcessUserApcs(context);
    }
    if (result != X_STATUS_KERNEL_APC) {
      return result;
    }
    if (timeout) {
      if (timeout2 < 0) {
        other_timer_storage =
            context->kernel_state->GetKernelInterruptTime() - *timeout;
        timeout2 = &other_timer_storage;
        timeout = &other_timer_storage;
      } else {
        timeout = timeout2;
      }
    }
  LABEL_60:
    thread->wait_irql = context->kernel_state->LockDispatcher(context);
  }
  auto v22 = objects;
  wait_blocks_shifted = &wait_blocks->wait_result_xstatus;
  int obj_type;
  // not actually X_KMUTANT, but it covers all the fields we need here
  X_KMUTANT* v24;
  while (1) {
    v24 = (X_KMUTANT*)*v22;

    obj_type = v24->header.type;
    if (wait_type == 1) {
      break;
    }
    if (obj_type == 2) {
      auto v29 = v24->owner;
      if (thread != v29.xlat() || v24->header.signal_state != 0x80000000) {
        if (v24->header.signal_state > 0 || thread == v29.xlat()) {
          goto LABEL_31;
        }
      LABEL_30:
        // todo: fix!
        v20 = 0;  // HIDWORD(v16);
        goto LABEL_31;
      }
      goto LABEL_19;
    }
    if (v24->header.signal_state <= 0) {
      goto LABEL_30;
    }
  LABEL_31:
    auto v30 = v21;
    ADJ(wait_blocks_shifted)->object = &v24->header;
    ADJ(wait_blocks_shifted)->wait_type = wait_type;
    ++v21;
    ADJ(wait_blocks_shifted)->thread = thread;
    v17 = ADJ(wait_blocks_shifted);
    ++v22;
    ADJ(wait_blocks_shifted)->wait_result_xstatus = v30;
    ADJ(wait_blocks_shifted)->next_wait_block = ADJ(wait_blocks_shifted) + 1;
    wait_blocks_shifted.m_base += 12;
    if (v21 >= num_objects) {
      goto LABEL_32;
    }
  }
  bool is_mutant = obj_type == DISPATCHER_MUTANT;
  int saved_signalstate = v24->header.signal_state;
  if (is_mutant) {
    if (saved_signalstate <= 0 && thread != v24->owner.xlat()) {
      goto LABEL_31;
    }

    if (saved_signalstate != 0x80000000) {
      auto v38 = v24->header.signal_state - 1;
      v24->header.signal_state = v38;
      if (!v38) {
        auto v39 = v24->abandoned;
        v24->owner = thread;
        if (v39 == 1) {
          v24->abandoned = 0;  /// BYTE3(v16);
          thread->wait_result = 128;
        }
        auto v40 = thread->mutants_list.blink_ptr;
        auto v41 = v40->flink_ptr;
        v24->unk_list.blink_ptr = v40;
        v24->unk_list.flink_ptr = v41;
        v41->blink_ptr = &v24->unk_list;
        v40->flink_ptr = &v24->unk_list;
      }
      result = thread->wait_result | v21;
      goto LABEL_75;
    }
  LABEL_19:
    xeDispatcherSpinlockUnlock(
        context, context->kernel_state->GetDispatcherLock(context),
        thread->wait_irql);
    // RtlRaiseStatus(X_STATUS_MUTANT_LIMIT_EXCEEDED);
    xenia_assert(false);
    goto LABEL_31;
  }
  if (saved_signalstate <= 0) {
    goto LABEL_31;
  }
  auto object_type = v24->header.type;
  if ((object_type & 7) == 1) {
    v24->header.signal_state = 0;
    // HIDWORD(v16);
  } else if (object_type == 5) {
    --v24->header.signal_state;
  }
  result = v21;
LABEL_75:
  context->kernel_state->UnlockDispatcherAtIrql(context);
  scheduler_80097F90(context, thread);
deliver_apc_and_return:
  if (result == X_STATUS_USER_APC) {
    xeProcessUserApcs(context);
  }
  return result;
}

int32_t xeKeSetDisableBoostThread(PPCContext* context, X_KTHREAD* thread,
                                  char a2) {
  uint32_t old_irql = context->kernel_state->LockDispatcher(context);
  xboxkrnl::xeKeKfAcquireSpinLock(
      context, &thread->a_prcb_ptr->enqueued_processor_threads_lock, false);

  auto old_disable_boost = thread->boost_disabled;

  thread->boost_disabled = a2;

  xboxkrnl::xeKeKfReleaseSpinLock(
      context, &thread->a_prcb_ptr->enqueued_processor_threads_lock, 0, false);

  xeDispatcherSpinlockUnlock(
      context, context->kernel_state->GetDispatcherLock(context), old_irql);
  return old_disable_boost;
}

int32_t xeKeSetPriorityThread(PPCContext* context, X_KTHREAD* thread,
                              int priority) {
  uint32_t old_irql = context->kernel_state->LockDispatcher(context);
  xboxkrnl::xeKeKfAcquireSpinLock(
      context, &thread->a_prcb_ptr->enqueued_processor_threads_lock, false);

  auto old_priority = thread->priority;
  auto v8 = thread->process->quantum;
  thread->unk_BA = 0;
  thread->quantum = v8;
  xeKeChangeThreadPriority(context, thread, priority);
  xboxkrnl::xeKeKfReleaseSpinLock(
      context, &thread->a_prcb_ptr->enqueued_processor_threads_lock, 0, false);

  xeDispatcherSpinlockUnlock(
      context, context->kernel_state->GetDispatcherLock(context), old_irql);
  return old_priority;
}
static void BackgroundModeIPI(void* ud) {
  auto context = cpu::ThreadState::GetContext();
  auto KPCR = GetKPCR(context);
  KPCR->generic_software_interrupt = 2;
  KPCR->unk_1A = 0x20;
  KPCR->timeslice_ended = 0x20;
  KernelState::HWThreadFor(context)->interrupt_controller()->SetEOI(1);
}
void xeKeEnterBackgroundMode(PPCContext* context) {
  uint32_t BackgroundProcessors =
      xboxkrnl::xeKeQueryBackgroundProcessors(context);
  auto KPCR = GetKPCR(context);
  uint32_t processor_mask = KPCR->prcb_data.processor_mask;
  if ((BackgroundProcessors & processor_mask) != 0) {
    BackgroundProcessors &= ~processor_mask;
    KPCR->unk_1A = 1;
    KPCR->timeslice_ended = 1;
    KPCR->generic_software_interrupt = 2;
  }
  if (BackgroundProcessors) {
    for (uint32_t i = 0; i < 6; ++i) {
      if (((1 << i) & BackgroundProcessors) != 0) {
        auto CPUThread = context->processor->GetCPUThread(i);

        CPUThread->SendGuestIPI(BackgroundModeIPI, nullptr);
      }
    }
  }
}

uint32_t xeKeQueryBackgroundProcessors(PPCContext* context) {
  return context->kernel_state->GetKernelGuestGlobals(context)
      ->background_processors;
}

void xeKeSetBackgroundProcessors(PPCContext* context, unsigned int new_bgproc) {
  context->kernel_state->GetKernelGuestGlobals(context)->background_processors =
      new_bgproc;
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
