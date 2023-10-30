/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_THREADING_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_THREADING_H_

#include "xenia/kernel/kernel_guest_structures.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xmutant.h"

namespace xe {
namespace kernel {
struct X_KEVENT;

namespace xboxkrnl {

uint32_t xeNtSetEvent(uint32_t handle, xe::be<uint32_t>* previous_state_ptr);

uint32_t xeNtClearEvent(uint32_t handle);

uint32_t xeNtWaitForMultipleObjectsEx(uint32_t count, xe::be<uint32_t>* handles,
                                      uint32_t wait_type, uint32_t wait_mode,
                                      uint32_t alertable, uint64_t* timeout_ptr,
                                      cpu::ppc::PPCContext* context);

uint32_t xeKeWaitForSingleObject(void* object_ptr, uint32_t wait_reason,
                                 uint32_t processor_mode, uint32_t alertable,
                                 uint64_t* timeout_ptr);

uint32_t NtWaitForSingleObjectEx(uint32_t object_handle, uint32_t wait_mode,
                                 uint32_t alertable, uint64_t* timeout_ptr);

int32_t xeKeSetEvent(PPCContext* context, X_KEVENT* event, int increment,
                     unsigned char wait);
int32_t xeKePulseEvent(PPCContext* context, X_KEVENT* event, int increment,
                       unsigned char wait);
int32_t
    xeKeResetEvent(PPCContext* context, X_KEVENT* event);

uint32_t KeDelayExecutionThread(uint32_t processor_mode, uint32_t alertable,
                                uint64_t* interval_ptr,
                                cpu::ppc::PPCContext* ctx);

uint32_t ExCreateThread(xe::be<uint32_t>* handle_ptr, uint32_t stack_size,
                        xe::be<uint32_t>* thread_id_ptr,
                        uint32_t xapi_thread_startup, uint32_t start_address,
                        uint32_t start_context, uint32_t creation_flags);

void xeKeInitializeSemaphore(X_KSEMAPHORE* semaphore, int count, int limit);

uint32_t ExTerminateThread(uint32_t exit_code);

uint32_t NtResumeThread(uint32_t handle, uint32_t* suspend_count_ptr);

uint32_t NtClose(uint32_t handle);
void xeKeInitializeApc(XAPC* apc, uint32_t thread_ptr, uint32_t kernel_routine,
                       uint32_t rundown_routine, uint32_t normal_routine,
                       uint32_t apc_mode, uint32_t normal_context);
uint32_t xeKeInsertQueueApc(XAPC* apc, uint32_t arg1, uint32_t arg2,
                            uint32_t priority_increment,
                            cpu::ppc::PPCContext* context);

void xeKeInsertQueueApcHelper(cpu::ppc::PPCContext* context,XAPC* apc,
                              int priority_increment);
uint32_t
    xeNtQueueApcThread(uint32_t thread_handle, uint32_t apc_routine,
                            uint32_t apc_routine_context, uint32_t arg1,
                            uint32_t arg2, cpu::ppc::PPCContext* context);
XE_NOINLINE
void xeKfLowerIrql(PPCContext* ctx, unsigned char new_irql);
XE_NOINLINE
unsigned char xeKfRaiseIrql(PPCContext* ctx, unsigned char new_irql);
XE_NOINLINE
void xeKeKfReleaseSpinLock(PPCContext* ctx, X_KSPINLOCK* lock,
                           uint32_t old_irql, bool change_irql = true);
XE_NOINLINE
uint32_t xeKeKfAcquireSpinLock(PPCContext* ctx, X_KSPINLOCK* lock,
                               bool change_irql = true);

X_STATUS xeProcessUserApcs(PPCContext* ctx);
X_STATUS xeProcessKernelApcs(PPCContext* ctx);
void xeExecuteDPCList2(
    PPCContext* context, uint32_t timer_unk,
    util::X_TYPED_LIST<XDPC, offsetof(XDPC, list_entry)>& dpc_list,
    uint32_t zero_register);
void xeHandleDPCsAndThreadSwapping(PPCContext* context, bool from_idle_loop=false);
void xeDispatchProcedureCallInterrupt(unsigned int new_irql,
                                      unsigned int software_interrupt_mask,
                                      cpu::ppc::PPCContext* context);
void xeRundownApcs(PPCContext* ctx);
uint32_t xeKeGetCurrentProcessType(PPCContext* context);
void xeKeSetCurrentProcessType(uint32_t type, PPCContext* context);
void xeKeInitializeMutant(X_KMUTANT* mutant, bool initially_owned,
                          xe::cpu::ppc::PPCContext* context);
void xeKeEnterCriticalRegion(PPCContext* context);
void xeKeLeaveCriticalRegion(PPCContext* context);

void xeKeInitializeTimerEx(X_KTIMER* timer, uint32_t type, uint32_t proctype,
                           PPCContext* context);
// dispatcher header helpers
void xeEnqueueThreadPostWait(PPCContext* context, X_KTHREAD* thread,
                             X_STATUS wait_result, int unknown);
void xeHandleWaitTypeAll(PPCContext* context, X_KWAIT_BLOCK* block);
void xeDispatchSignalStateChange(PPCContext* context, X_DISPATCH_HEADER* header,
                                 int unk);
uint32_t xeKeInsertQueueDpc(XDPC* dpc, uint32_t arg1, uint32_t arg2,
                            PPCContext* ctx);
uint32_t xeKeRemoveQueueDpc(XDPC* dpc, PPCContext* ctx);
void xeReallyQueueThread(PPCContext* context, X_KTHREAD* kthread);
void xeHandleReadyThreadOnDifferentProcessor(PPCContext* context,
                                             X_KTHREAD* kthread);
X_STATUS xeNtYieldExecution(PPCContext* context);
/*
    a special spinlock-releasing function thats used in a lot of scheduler
   related functions im not very confident in the correctness of this one. the
   original jumps around a lot, directly into the bodies of other functions and
   appears to have been written in asm
*/
void xeDispatcherSpinlockUnlock(PPCContext* context, X_KSPINLOCK* lock,
                                uint32_t irql);
XE_NOINLINE
void scheduler_80097F90(PPCContext* context, X_KTHREAD* thread);
XE_NOINLINE
X_STATUS xeSchedulerSwitchThread(PPCContext* context);
XE_NOINLINE
X_STATUS xeSchedulerSwitchThread2(PPCContext* context);

int xeKeSuspendThread(PPCContext* context, X_KTHREAD* thread);
int xeKeResumeThread(PPCContext* context, X_KTHREAD* thread);

void xeSuspendThreadApcRoutine(PPCContext* context);

X_STATUS xeKeWaitForSingleObject(PPCContext* context, X_DISPATCH_HEADER* object,
                                 unsigned reason, unsigned unk, bool alertable,
                                 int64_t* timeout);
int32_t xeKeReleaseMutant(PPCContext* context, X_KMUTANT* mutant, int unk,
                          bool abandoned, unsigned char unk2);
int XeInsertGlobalTimer(PPCContext* context, X_KTIMER* timer, int64_t time);
int xeKeSetTimerEx(PPCContext* context, X_KTIMER* timer, int64_t duetime,
                   int period, XDPC* dpc);
int xeKeSetTimer(PPCContext* context, X_KTIMER* timer, int64_t duetime,
                 XDPC* dpc);
int xeKeCancelTimer(PPCContext* context, X_KTIMER* timer);
void xeEXTimerDPCRoutine(PPCContext* context);
//NtCreateTimer
void xeKeInitializeExTimer(PPCContext* context, X_EXTIMER* timer,
                           uint32_t type);
//NtSetTimer
int xeKeSetExTimer(PPCContext* context, X_EXTIMER* timer, int64_t due_timer,
                   uint32_t apc_routine, uint32_t apc_arg, int period, int apc_mode);
//NtCancelTimer
int xeKeCancelExTimer(PPCContext* context, X_EXTIMER* timer);

uint32_t xeKeRemoveQueueApc(XAPC* apc, PPCContext* context);

void xeEXTimerAPCKernelRoutine(PPCContext* context);

void xeKeSetAffinityThread(PPCContext* context, X_KTHREAD* thread,
                           uint32_t affinity, uint32_t* prev_affinity);

void xeKeSetPriorityClassThread(PPCContext* context, X_KTHREAD* thread,
                                bool a2);

void xeKeChangeThreadPriority(PPCContext* context, X_KTHREAD* thread,
                              int priority);
int32_t xeKeReleaseSemaphore(PPCContext* context, X_KSEMAPHORE* semaphore,
                             int increment, int adjustment, unsigned char wait);
X_STATUS xeKeDelayExecutionThread(PPCContext* context, char mode,
                                  bool alertable, int64_t* interval);

int32_t xeKeSetBasePriorityThread(PPCContext* context, X_KTHREAD* thread,
                                  int increment);
X_STATUS xeKeSignalAndWaitForSingleObjectEx(
    PPCContext* context,
    ShiftedPointer<X_DISPATCH_HEADER, X_OBJECT_HEADER, 16> signal,
    ShiftedPointer<X_DISPATCH_HEADER, X_OBJECT_HEADER, 16> wait,
    unsigned char mode, bool alertable, int64_t* timeout);

int32_t xeKeQueryBasePriorityThread(PPCContext* context, X_KTHREAD* thread);
X_STATUS xeKeWaitForMultipleObjects(
    PPCContext* context, unsigned int num_objects, X_DISPATCH_HEADER** objects,
    unsigned wait_type, unsigned reason, unsigned char mode, int alertable,
    int64_t* timeout, X_KWAIT_BLOCK* wait_blocks);
int32_t xeKeSetDisableBoostThread(PPCContext* context, X_KTHREAD* thread,
                                  char a2);
int32_t xeKeSetPriorityThread(PPCContext* context, X_KTHREAD* thread,
                              int priority);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_THREADING_H_
