/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_KERNEL_STATE_H_
#define XENIA_KERNEL_KERNEL_STATE_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <vector>

#include "achievement_manager.h"
#include "xenia/base/bit_map.h"
#include "xenia/base/cvar.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/kernel_guest_structures.h"
#include "xenia/kernel/util/guest_object_table.h"
#include "xenia/kernel/util/guest_timer_list.h"
#include "xenia/kernel/util/kernel_fwd.h"
#include "xenia/kernel/util/native_list.h"
#include "xenia/kernel/util/object_table.h"
#include "xenia/kernel/util/xdbf_utils.h"
#include "xenia/kernel/xam/app_manager.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xevent.h"
#include "xenia/memory.h"
#include "xenia/vfs/virtual_file_system.h"
namespace xe {
class ByteStream;
class Emulator;
namespace cpu {
class Processor;
}  // namespace cpu
}  // namespace xe

namespace xe {
namespace kernel {

constexpr fourcc_t kKernelSaveSignature = make_fourcc("KRNL");

struct TerminateNotification {
  uint32_t guest_routine;
  uint32_t priority;
};

// structure for KeTimeStampBuindle
// a bit like the timers on KUSER_SHARED on normal win32
// https://www.geoffchappell.com/studies/windows/km/ntoskrnl/inc/api/ntexapi_x/kuser_shared_data/index.htm
struct X_TIME_STAMP_BUNDLE {
  xe::be<uint64_t> interrupt_time;
  // i assume system_time is in 100 ns intervals like on win32
  xe::be<uint64_t> system_time;
  xe::be<uint32_t> tick_count;
  xe::be<uint32_t> padding;
};

struct KernelGuestGlobals {
  X_OBJECT_TYPE ExThreadObjectType;
  X_OBJECT_TYPE ExEventObjectType;
  X_OBJECT_TYPE ExMutantObjectType;
  X_OBJECT_TYPE ExSemaphoreObjectType;
  X_OBJECT_TYPE ExTimerObjectType;
  X_OBJECT_TYPE IoCompletionObjectType;
  X_OBJECT_TYPE IoDeviceObjectType;
  X_OBJECT_TYPE IoFileObjectType;
  X_OBJECT_TYPE ObDirectoryObjectType;
  X_OBJECT_TYPE ObSymbolicLinkObjectType;

  // these are Xam object types, and not exported
  X_OBJECT_TYPE XamNotifyListenerObjectType;
  X_OBJECT_TYPE XamEnumeratorObjectType;

  X_DISPATCH_HEADER XamDefaultObject;

  // a constant buffer that some object types' "unknown_size_or_object" field
  // points to
  X_DISPATCH_HEADER XboxKernelDefaultObject;
  X_KPROCESS idle_process;    // X_PROCTYPE_IDLE. runs in interrupt contexts. is
                              // also the context the kernel starts in?
  X_KPROCESS title_process;   // X_PROCTYPE_TITLE
  X_KPROCESS system_process;  // X_PROCTYPE_SYSTEM. no idea when this runs. can
                              // create threads in this process with
                              // ExCreateThread and the thread flag 0x2

  // locks.
  alignas(128)
      X_KSPINLOCK dispatcher_lock;  // called the "dispatcher lock" in nt 3.5
                                    // ppc .dbg file. Used basically everywhere
                                    // that DISPATCHER_HEADER'd objects appear

  X_KSPINLOCK timer_table_spinlock;//does not exist on real hw

  // this lock is only used in some Ob functions. It's odd that it is used at
  // all, as each table already has its own spinlock.
  alignas(128) X_KSPINLOCK ob_lock;

  // if LLE emulating Xam, this is needed or you get an immediate freeze
  X_KEVENT UsbdBootEnumerationDoneEvent;
  util::X_HANDLE_TABLE TitleObjectTable;
  util::X_HANDLE_TABLE SystemObjectTable;
  // threadids use a different table
  util::X_HANDLE_TABLE TitleThreadIdTable;
  util::X_HANDLE_TABLE SystemThreadIdTable;

  // util::X_TIMER_TABLE timer_table;

  // for very bad timer impl
  util::X_TYPED_LIST<X_KTIMER, offsetof(X_KTIMER, table_bucket_entry)>
      running_timers;

  X_TIME_STAMP_BUNDLE KeTimestampBundle;

  uint32_t guest_nullsub;
  uint32_t suspendthread_apc_routine;
  uint32_t extimer_dpc_routine;
  uint32_t extimer_apc_kernel_routine;
  XDPC graphics_interrupt_dpc;

  X_KSPINLOCK tls_lock;

  uint32_t background_processors;

  XDPC command_processor_interrupt_dpcs[6]; //one per hw thread

  X_KEVENT title_terminated_event;
  xe::be<uint32_t> VdGlobalDevice;
  xe::be<uint32_t> VdGlobalXamDevice;
  xe::be<uint32_t> VdGpuClockInMHz;

  X_RTL_CRITICAL_SECTION VdHSIOCalibrationLock;

  X_KEVENT dispatch_queue_event_;

  X_KEVENT audio_interrupt_dpc_event_;
  XDPC audio_interrupt_dpc_;

};

struct X_KPCR_PAGE;
struct DPCImpersonationScope {
  uint8_t previous_irql_;
};
class KernelState {
 public:
  explicit KernelState(Emulator* emulator);
  ~KernelState();

  static KernelState* shared();

  Emulator* emulator() const { return emulator_; }
  Memory* memory() const { return memory_; }
  cpu::Processor* processor() const { return processor_; }
  vfs::VirtualFileSystem* file_system() const { return file_system_; }

  uint32_t title_id() const;
  util::XdbfGameData title_xdbf() const;
  util::XdbfGameData module_xdbf(object_ref<UserModule> exec_module) const;

  AchievementManager* achievement_manager() const {
    return achievement_manager_.get();
  }
  xam::AppManager* app_manager() const { return app_manager_.get(); }
  xam::ContentManager* content_manager() const {
    return content_manager_.get();
  }

  uint8_t GetConnectedUsers() const;
  void UpdateUsedUserProfiles();

  bool IsUserSignedIn(uint32_t index) const {
    return user_profiles_.find(index) != user_profiles_.cend();
  }

  bool IsUserSignedIn(uint64_t xuid) const {
    return user_profile(xuid) != nullptr;
  }

  xam::UserProfile* user_profile(uint32_t index) const {
    if (!IsUserSignedIn(index)) {
      return nullptr;
    }

    return user_profiles_.at(index).get();
  }

  xam::UserProfile* user_profile(uint64_t xuid) const {
    for (const auto& [key, value] : user_profiles_) {
      if (value->xuid() == xuid) {
        return user_profiles_.at(key).get();
      }
    }
    return nullptr;
  }

  // Access must be guarded by the global critical region.
  util::ObjectTable* object_table() { return &object_table_; }

  uint32_t GetSystemProcess() const {
    return kernel_guest_globals_ + offsetof(KernelGuestGlobals, system_process);
  }

  uint32_t GetTitleProcess() const {
    return kernel_guest_globals_ + offsetof(KernelGuestGlobals, title_process);
  }
  // also the "interrupt" process
  uint32_t GetIdleProcess() const {
    return kernel_guest_globals_ + offsetof(KernelGuestGlobals, idle_process);
  }

  uint32_t AllocateTLS(cpu::ppc::PPCContext* context);
  void FreeTLS(cpu::ppc::PPCContext* context, uint32_t slot);

  void RegisterTitleTerminateNotification(uint32_t routine, uint32_t priority);
  void RemoveTitleTerminateNotification(uint32_t routine);

  void RegisterModule(XModule* module);
  void UnregisterModule(XModule* module);
  bool RegisterUserModule(object_ref<UserModule> module);
  void UnregisterUserModule(UserModule* module);
  bool IsKernelModule(const std::string_view name);
  object_ref<XModule> GetModule(const std::string_view name,
                                bool user_only = false);

  object_ref<XThread> LaunchModule(object_ref<UserModule> module);
  object_ref<UserModule> GetExecutableModule();
  void SetExecutableModule(object_ref<UserModule> module);
  object_ref<UserModule> LoadUserModule(const std::string_view name,
                                        bool call_entry = true);
  X_RESULT FinishLoadingUserModule(const object_ref<UserModule> module,
                                   bool call_entry = true);
  void UnloadUserModule(const object_ref<UserModule>& module,
                        bool call_entry = true);

  void XamCall(cpu::ppc::PPCContext* context, uint16_t ordinal);

  object_ref<KernelModule> GetKernelModule(const std::string_view name);
  template <typename T>
  object_ref<KernelModule> LoadKernelModule() {
    auto kernel_module = object_ref<KernelModule>(new T(emulator_, this));
    LoadKernelModule(kernel_module);
    return kernel_module;
  }
  template <typename T>
  object_ref<T> GetKernelModule(const std::string_view name) {
    auto module = GetKernelModule(name);
    return object_ref<T>(reinterpret_cast<T*>(module.release()));
  }

  X_RESULT ApplyTitleUpdate(const object_ref<UserModule> module);
  // Terminates a title: Unloads all modules, and kills all guest threads.
  // This DOES NOT RETURN if called from a guest thread!
  void TerminateTitle();

  void RegisterThread(XThread* thread);
  void UnregisterThread(XThread* thread);
  void OnThreadExecute(XThread* thread);
  void OnThreadExit(XThread* thread);
  object_ref<XThread> GetThreadByID(uint32_t thread_id);

  void RegisterNotifyListener(XNotifyListener* listener);
  void UnregisterNotifyListener(XNotifyListener* listener);
  void BroadcastNotification(XNotificationID id, uint32_t data);

  void CompleteOverlapped(uint32_t overlapped_ptr, X_RESULT result);
  void CompleteOverlappedEx(uint32_t overlapped_ptr, X_RESULT result,
                            uint32_t extended_error, uint32_t length);

  void CompleteOverlappedImmediate(uint32_t overlapped_ptr, X_RESULT result);
  void CompleteOverlappedImmediateEx(uint32_t overlapped_ptr, X_RESULT result,
                                     uint32_t extended_error, uint32_t length);

  void CompleteOverlappedDeferred(
      std::function<void()> completion_callback, uint32_t overlapped_ptr,
      X_RESULT result, std::function<void()> pre_callback = nullptr,
      std::function<void()> post_callback = nullptr);
  void CompleteOverlappedDeferredEx(
      std::function<void()> completion_callback, uint32_t overlapped_ptr,
      X_RESULT result, uint32_t extended_error, uint32_t length,
      std::function<void()> pre_callback = nullptr,
      std::function<void()> post_callback = nullptr);

  void CompleteOverlappedDeferred(
      std::function<X_RESULT()> completion_callback, uint32_t overlapped_ptr,
      std::function<void()> pre_callback = nullptr,
      std::function<void()> post_callback = nullptr);
  void CompleteOverlappedDeferredEx(
      std::function<X_RESULT(uint32_t&, uint32_t&)> completion_callback,
      uint32_t overlapped_ptr, std::function<void()> pre_callback = nullptr,
      std::function<void()> post_callback = nullptr);

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  uint32_t notification_position_ = 2;

  uint32_t GetKeTimestampBundle();

  XE_NOINLINE
  XE_COLD
  uint32_t CreateKeTimestampBundle();
  void SystemClockInterrupt();

  void EmulateCPInterrupt(uint32_t interrupt_callback,
                             uint32_t interrupt_callback_data, uint32_t source,
                             uint32_t cpu);
  uint32_t LockDispatcher(cpu::ppc::PPCContext* context);
  void UnlockDispatcher(cpu::ppc::PPCContext* context, uint32_t irql);
  X_KSPINLOCK* GetDispatcherLock(cpu::ppc::PPCContext* context);

  void LockDispatcherAtIrql(cpu::ppc::PPCContext* context);
  void UnlockDispatcherAtIrql(cpu::ppc::PPCContext* context);
  uint32_t ReferenceObjectByHandle(cpu::ppc::PPCContext* context,
                                   uint32_t handle, uint32_t guest_object_type,
                                   uint32_t* object_out);
  void DereferenceObject(cpu::ppc::PPCContext* context, uint32_t object);

  void AssertDispatcherLocked(cpu::ppc::PPCContext* context);
  uint32_t AllocateInternalHandle(void* ud);
  void* _FreeInternalHandle(uint32_t id);
  template <typename T>
  T* LookupInternalHandle(uint32_t id) {
    std::unique_lock lock{internal_handle_table_mutex_};
    return reinterpret_cast<T*>(internal_handles_.find(id)->second);
  }
  template <typename T = void>
  T* FreeInternalHandle(uint32_t id) {
    return reinterpret_cast<T*>(_FreeInternalHandle(id));
  }

  uint32_t GetKernelTickCount();
  uint64_t GetKernelSystemTime();
  uint64_t GetKernelInterruptTime();
  X_KPCR_PAGE* KPCRPageForCpuNumber(uint32_t i);

  X_STATUS ContextSwitch(cpu::ppc::PPCContext* context, X_KTHREAD* guest,
                         bool from_idle_loop = false);
  // the cpu number is encoded in the pcr address
  uint32_t GetPCRCpuNum(X_KPCR* pcr) {
    return (memory_->HostToGuestVirtual(pcr) >> 12) & 0xF;
  }
  cpu::XenonInterruptController* InterruptControllerFromPCR(
      cpu::ppc::PPCContext* context, X_KPCR* pcr);
  void SetCurrentInterruptPriority(cpu::ppc::PPCContext* context, X_KPCR* pcr,
                                   uint32_t priority);
  static void GenericExternalInterruptEpilog(cpu::ppc::PPCContext* context, uint32_t r3);

  static void GraphicsInterruptDPC(cpu::ppc::PPCContext* context);
  static void CPInterruptIPI(void* ud);

  static cpu::HWThread* HWThreadFor(cpu::ppc::PPCContext* context);

  static void TriggerTrueExternalInterrupt(cpu::ppc::PPCContext* context);

  static void AudioInterruptDPC(cpu::ppc::PPCContext* context);
  static void AudioInterrupt(void* v);
  void InitKernelAuxstack(X_KTHREAD* thread);
 private:
  static void LaunchModuleInterrupt(void* ud);
  void LoadKernelModule(object_ref<KernelModule> kernel_module);
  void InitializeProcess(X_KPROCESS* process, uint32_t type, char unk_18,
                         char unk_19, char unk_1A);
  void SetProcessTLSVars(X_KPROCESS* process, int num_slots, int tls_data_size,
                         int tls_static_data_address);

  void CPU0WaitForLaunch(cpu::ppc::PPCContext* context);
  void BootKernel();
  void CreateDispatchThread();
  /*
    initializes objects/data that is normally pre-initialized in the rdata
    section of the kernel, or any other data that does not require execution on
    a PPCContext to init
  */
  void BootInitializeStatics();

  static void ForwardBootInitializeCPU0InSystemThread(
      cpu::ppc::PPCContext* context);

  //system thread gets created by cpu0 to perform additional init
  void BootInitializeCPU0InSystemThread(cpu::ppc::PPCContext* context);
  // runs on cpu0
  void BootInitializeXam(cpu::ppc::PPCContext* context);

  void BootCPU0(cpu::ppc::PPCContext* context, X_KPCR* kpcr);
  void BootCPU1Through5(cpu::ppc::PPCContext* context, X_KPCR* kpcr);

  static void HWThreadBootFunction(cpu::ppc::PPCContext* context, void* ud);

  void SetupProcessorPCR(uint32_t which_processor_index);
  void SetupProcessorIdleThread(uint32_t which_processor_index);
  void InitProcessorStack(X_KPCR* pcr);
  
  Emulator* emulator_;
  Memory* memory_;
  cpu::Processor* processor_;
  vfs::VirtualFileSystem* file_system_;

  std::unique_ptr<xam::AppManager> app_manager_;
  std::unique_ptr<xam::ContentManager> content_manager_;
  std::map<uint8_t, std::unique_ptr<xam::UserProfile>> user_profiles_;
  std::unique_ptr<AchievementManager> achievement_manager_;

  xe::global_critical_region global_critical_region_;

  // Must be guarded by the global critical region.
  util::ObjectTable object_table_;
  std::unordered_map<uint32_t, XThread*> threads_by_id_;
  std::vector<object_ref<XNotifyListener>> notify_listeners_;
  bool has_notified_startup_ = false;

  object_ref<UserModule> executable_module_;
  std::vector<object_ref<KernelModule>> kernel_modules_;
  std::vector<object_ref<UserModule>> user_modules_;
  std::vector<TerminateNotification> terminate_notifications_;
  uint32_t kernel_guest_globals_ = 0;

  std::atomic<bool> dispatch_thread_running_;
  object_ref<XHostThread> dispatch_thread_;
  threading::AtomicListHeader dispatch_queue_;
  cpu::backend::GuestTrampolineGroup kernel_trampoline_group_;
  // fixed address referenced by dashboards. Data is currently unknown
  uint32_t strange_hardcoded_page_ = 0x8E038634 & (~0xFFFF);
  uint32_t strange_hardcoded_location_ = 0x8E038634;

  // assign integer ids to arbitrary data, for stuffing threading::WaitHandle
  // into header flink_ptr
  std::unordered_map<uint32_t, void*> internal_handles_;
  uint32_t current_internal_handle_ = 0x66180000;
  xe_mutex internal_handle_table_mutex_;
  static void KernelIdleProcessFunction(cpu::ppc::PPCContext* context);
  static void KernelDecrementerInterrupt(void* ud);
  void SetupKPCRPageForCPU(uint32_t cpunum);
  friend class XObject;

 public:
  uint32_t dash_context_ = 0;
  std::unordered_map<XObject::Type, uint32_t>
      host_object_type_enum_to_guest_object_type_ptr_;
  uint32_t GetKernelGuestGlobals() const { return kernel_guest_globals_; }
  KernelGuestGlobals* GetKernelGuestGlobals(cpu::ppc::PPCContext* context);
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_KERNEL_STATE_H_
