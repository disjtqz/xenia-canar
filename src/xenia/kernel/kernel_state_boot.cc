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

XE_NOINLINE
XE_COLD
uint32_t KernelState::CreateKeTimestampBundle() {
  auto crit = global_critical_region::Acquire();

  uint32_t pKeTimeStampBundle =
      memory_->SystemHeapAlloc(sizeof(X_TIME_STAMP_BUNDLE));
  X_TIME_STAMP_BUNDLE* lpKeTimeStampBundle =
      memory_->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(pKeTimeStampBundle);

  uint32_t ticks = Clock::QueryGuestUptimeMillis();
  uint64_t time_imprecise = static_cast<uint64_t>(ticks) * 1000000ULL;
  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->interrupt_time,
                               time_imprecise);

  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->system_time,
                               time_imprecise);

  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->tick_count, ticks);

  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->padding, 0);

  timestamp_timer_ = xe::threading::HighResolutionTimer::CreateRepeating(
      std::chrono::milliseconds(1), [this]() { this->SystemClockInterrupt(); });
  ke_timestamp_bundle_ptr_ = pKeTimeStampBundle;
  return pKeTimeStampBundle;
}

void KernelState::InitializeProcess(X_KPROCESS* process, uint32_t type,
                                    char unk_18, char unk_19, char unk_1A) {
  uint32_t guest_kprocess = memory()->HostToGuestVirtual(process);

  uint32_t thread_list_guest_ptr =
      guest_kprocess + offsetof(X_KPROCESS, thread_list);

  process->unk_18 = unk_18;
  process->unk_19 = unk_19;
  process->unk_1A = unk_1A;
  util::XeInitializeListHead(&process->thread_list, thread_list_guest_ptr);
  process->unk_0C = 60;
  // doubt any guest code uses this ptr, which i think probably has something to
  // do with the page table
  process->clrdataa_masked_ptr = 0;
  // clrdataa_ & ~(1U << 31);
  process->thread_count = 0;
  process->unk_1B = 0x06;
  process->kernel_stack_size = 16 * 1024;
  process->tls_slot_size = 0x80;

  process->process_type = type;
  uint32_t unk_list_guest_ptr = guest_kprocess + offsetof(X_KPROCESS, unk_54);
  // TODO(benvanik): figure out what this list is.
  util::XeInitializeListHead(&process->unk_54, unk_list_guest_ptr);
}

void KernelState::SetProcessTLSVars(X_KPROCESS* process, int num_slots,
                                    int tls_data_size,
                                    int tls_static_data_address) {
  uint32_t slots_padded = (num_slots + 3) & 0xFFFFFFFC;
  process->tls_data_size = tls_data_size;
  process->tls_raw_data_size = tls_data_size;
  process->tls_static_data_address = tls_static_data_address;
  process->tls_slot_size = 4 * slots_padded;
  uint32_t count_div32 = slots_padded / 32;
  for (unsigned word_index = 0; word_index < count_div32; ++word_index) {
    process->bitmap[word_index] = -1;
  }

  // set remainder of bitset
  if (((num_slots + 3) & 0x1C) != 0)
    process->bitmap[count_div32] = -1 << (32 - ((num_slots + 3) & 0x1C));
}
void AllocateThread(PPCContext* context) {
  uint32_t thread_mem_size = static_cast<uint32_t>(context->r[3]);
  uint32_t a2 = static_cast<uint32_t>(context->r[4]);
  uint32_t a3 = static_cast<uint32_t>(context->r[5]);
  if (thread_mem_size <= 0xFD8) thread_mem_size += 8;
  uint32_t result =
      xboxkrnl::xeAllocatePoolTypeWithTag(context, thread_mem_size, a2, a3);
  if (((unsigned short)result & 0xFFF) != 0) {
    result += 2;
  }

  context->r[3] = static_cast<uint64_t>(result);
}
void FreeThread(PPCContext* context) {
  uint32_t thread_memory = static_cast<uint32_t>(context->r[3]);
  if ((thread_memory & 0xFFF) != 0) {
    thread_memory -= 8;
  }
  xboxkrnl::xeFreePool(context, thread_memory);
}

void SimpleForwardAllocatePoolTypeWithTag(PPCContext* context) {
  uint32_t a1 = static_cast<uint32_t>(context->r[3]);
  uint32_t a2 = static_cast<uint32_t>(context->r[4]);
  uint32_t a3 = static_cast<uint32_t>(context->r[5]);
  context->r[3] = static_cast<uint64_t>(
      xboxkrnl::xeAllocatePoolTypeWithTag(context, a1, a2, a3));
}
void SimpleForwardFreePool(PPCContext* context) {
  xboxkrnl::xeFreePool(context, static_cast<uint32_t>(context->r[3]));
}

void DeleteMutant(PPCContext* context) {
  // todo: this should call kereleasemutant with some specific args

  xe::FatalError("DeleteMutant - need KeReleaseMutant(mutant, 1, 1, 0) ");
}
void DeleteTimer(PPCContext* context) {
  // todo: this should call KeCancelTimer
  xe::FatalError("DeleteTimer - need KeCancelTimer(mutant, 1, 1, 0) ");
}

void DeleteIoCompletion(PPCContext* context) {}

void UnknownProcIoDevice(PPCContext* context) {}

void CloseFileProc(PPCContext* context) {}

void DeleteFileProc(PPCContext* context) {}

void UnknownFileProc(PPCContext* context) {}

void DeleteSymlink(PPCContext* context) {
  X_KSYMLINK* lnk = context->TranslateVirtualGPR<X_KSYMLINK*>(context->r[3]);

  context->r[3] = lnk->refed_object_maybe;
  xboxkrnl::xeObDereferenceObject(context, lnk->refed_object_maybe);
}

static void InitializeHandleTable(util::X_HANDLE_TABLE* result,
                                  unsigned char poolarg,
                                  unsigned char handle_high_byte,
                                  unsigned char unk_36) {
  result->unk_pool_arg_34 = poolarg;
  result->handle_high_byte = handle_high_byte;
  result->unk_36 = unk_36;
  result->num_handles = 0;
  result->free_offset = 0;
  result->highest_allocated_offset = 0;
  result->table_dynamic_buckets = 0;
}

void KernelState::InitializeKernelGuestGlobals() {
  kernel_guest_globals_ = memory_->SystemHeapAlloc(sizeof(KernelGuestGlobals));

  KernelGuestGlobals* block =
      memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  memset(block, 0, sizeof(block));

  auto idle_process = memory()->TranslateVirtual<X_KPROCESS*>(GetIdleProcess());
  InitializeProcess(idle_process, X_PROCTYPE_IDLE, 0, 0, 0);
  idle_process->unk_0C = 0x7F;
  auto system_process =
      memory()->TranslateVirtual<X_KPROCESS*>(GetSystemProcess());
  InitializeProcess(system_process, X_PROCTYPE_SYSTEM, 2, 5, 9);
  SetProcessTLSVars(system_process, 32, 0, 0);

  InitializeHandleTable(&block->TitleObjectTable, X_PROCTYPE_TITLE, 0xF8, 0);
  InitializeHandleTable(&block->TitleThreadIdTable, X_PROCTYPE_TITLE, 0xF9, 0);

  // i cant find where these get initialized on 17559, but we already know what
  // to fill in here
  InitializeHandleTable(&block->SystemObjectTable, X_PROCTYPE_SYSTEM, 0xFA, 0);
  InitializeHandleTable(&block->SystemThreadIdTable, X_PROCTYPE_SYSTEM, 0xFB,
                        0);

  uint32_t oddobject_offset =
      kernel_guest_globals_ + offsetof(KernelGuestGlobals, OddObj);

  // init unknown object

  block->OddObj.field0 = 0x1000000;
  block->OddObj.field4 = 1;
  block->OddObj.points_to_self =
      oddobject_offset + offsetof(X_UNKNOWN_TYPE_REFED, points_to_self);
  block->OddObj.points_to_prior = block->OddObj.points_to_self;

  // init thread object
  block->ExThreadObjectType.pool_tag = 0x65726854;
  block->ExThreadObjectType.allocate_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(AllocateThread);

  block->ExThreadObjectType.free_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(FreeThread);

  // several object types just call freepool/allocatepool
  uint32_t trampoline_allocatepool =
      kernel_trampoline_group_.NewLongtermTrampoline(
          SimpleForwardAllocatePoolTypeWithTag);
  uint32_t trampoline_freepool =
      kernel_trampoline_group_.NewLongtermTrampoline(SimpleForwardFreePool);

  // init event object
  block->ExEventObjectType.pool_tag = 0x76657645;
  block->ExEventObjectType.allocate_proc = trampoline_allocatepool;
  block->ExEventObjectType.free_proc = trampoline_freepool;

  // init mutant object
  block->ExMutantObjectType.pool_tag = 0x6174754D;
  block->ExMutantObjectType.allocate_proc = trampoline_allocatepool;
  block->ExMutantObjectType.free_proc = trampoline_freepool;

  block->ExMutantObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteMutant);
  // init semaphore obj
  block->ExSemaphoreObjectType.pool_tag = 0x616D6553;
  block->ExSemaphoreObjectType.allocate_proc = trampoline_allocatepool;
  block->ExSemaphoreObjectType.free_proc = trampoline_freepool;
  // init timer obj
  block->ExTimerObjectType.pool_tag = 0x656D6954;
  block->ExTimerObjectType.allocate_proc = trampoline_allocatepool;
  block->ExTimerObjectType.free_proc = trampoline_freepool;
  block->ExTimerObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteTimer);
  // iocompletion object
  block->IoCompletionObjectType.pool_tag = 0x706D6F43;
  block->IoCompletionObjectType.allocate_proc = trampoline_allocatepool;
  block->IoCompletionObjectType.free_proc = trampoline_freepool;
  block->IoCompletionObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteIoCompletion);
  block->IoCompletionObjectType.unknown_size_or_object_ = oddobject_offset;

  // iodevice object
  block->IoDeviceObjectType.pool_tag = 0x69766544;
  block->IoDeviceObjectType.allocate_proc = trampoline_allocatepool;
  block->IoDeviceObjectType.free_proc = trampoline_freepool;
  block->IoDeviceObjectType.unknown_size_or_object_ = oddobject_offset;
  block->IoDeviceObjectType.unknown_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(UnknownProcIoDevice);

  // file object
  block->IoFileObjectType.pool_tag = 0x656C6946;
  block->IoFileObjectType.allocate_proc = trampoline_allocatepool;
  block->IoFileObjectType.free_proc = trampoline_freepool;
  block->IoFileObjectType.unknown_size_or_object_ =
      0x38;  // sizeof fileobject, i believe
  block->IoFileObjectType.close_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(CloseFileProc);
  block->IoFileObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteFileProc);
  block->IoFileObjectType.unknown_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(UnknownFileProc);

  // directory object
  block->ObDirectoryObjectType.pool_tag = 0x65726944;
  block->ObDirectoryObjectType.allocate_proc = trampoline_allocatepool;
  block->ObDirectoryObjectType.free_proc = trampoline_freepool;
  block->ObDirectoryObjectType.unknown_size_or_object_ = oddobject_offset;

  // symlink object
  block->ObSymbolicLinkObjectType.pool_tag = 0x626D7953;
  block->ObSymbolicLinkObjectType.allocate_proc = trampoline_allocatepool;
  block->ObSymbolicLinkObjectType.free_proc = trampoline_freepool;
  block->ObSymbolicLinkObjectType.unknown_size_or_object_ = oddobject_offset;
  block->ObSymbolicLinkObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteSymlink);

#define offsetof32(s, m) static_cast<uint32_t>(offsetof(s, m))

  host_object_type_enum_to_guest_object_type_ptr_ = {
      {XObject::Type::Event,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExEventObjectType)},
      {XObject::Type::Semaphore,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExSemaphoreObjectType)},
      {XObject::Type::Thread,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExThreadObjectType)},
      {XObject::Type::File,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, IoFileObjectType)},
      {XObject::Type::Mutant,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExMutantObjectType)},
      {XObject::Type::Device,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, IoDeviceObjectType)}};
  xboxkrnl::xeKeSetEvent(&block->UsbdBootEnumerationDoneEvent, 1, 0);

  for (unsigned i = 0; i < 6; ++i) {
    auto cpu_thread = processor()->GetCPUThread(i);
    cpu_thread->idle_process_function_ =
        &KernelState::KernelIdleProcessFunction;
    cpu_thread->os_thread_->Resume();

  }
}

}  // namespace kernel
}  // namespace xe
