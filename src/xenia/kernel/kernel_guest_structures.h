/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Xenia Canary. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_KERNEL_GUEST_STRUCTURES_H_
#define XENIA_KERNEL_KERNEL_GUEST_STRUCTURES_H_
#include "xenia/base/memory.h"
#include "xenia/kernel/util/native_list.h"
#include "xenia/xbox.h"
namespace xe {
namespace kernel {
static constexpr uint32_t kKernelAuxstackSize = 65536;
enum Irql : uint8_t {
  IRQL_PASSIVE = 0,
  IRQL_APC = 1,
  IRQL_DISPATCH = 2,
  IRQL_DPC = 3,
  IRQL_AUDIO = 68,  // used a few times in the audio driver
  IRQL_CLOCK = 116, //irql used by the clock interrupt
  IRQL_HIGHEST = 124
};

enum {
  DISPATCHER_MANUAL_RESET_EVENT = 0,
  DISPATCHER_AUTO_RESET_EVENT = 1,
  DISPATCHER_MUTANT = 2,
  DISPATCHER_QUEUE = 4,
  DISPATCHER_SEMAPHORE = 5,
  DISPATCHER_THREAD = 6,
  DISPATCHER_MANUAL_RESET_TIMER = 8,
  DISPATCHER_AUTO_RESET_TIMER = 9,
};

// https://www.geoffchappell.com/studies/windows/km/ntoskrnl/inc/ntos/ke/kthread_state.htm
enum : uint8_t {
  KTHREAD_STATE_INITIALIZED = 0,
  KTHREAD_STATE_READY = 1,
  KTHREAD_STATE_RUNNING = 2,
  KTHREAD_STATE_STANDBY = 3,
  KTHREAD_STATE_TERMINATED = 4,
  KTHREAD_STATE_WAITING = 5,
  KTHREAD_STATE_UNKNOWN = 6,  //"Transition" except that makes no sense here, so
                              //6 likely has a different meaning on xboxkrnl
};

static constexpr uint32_t XE_FLAG_THREAD_INITIALLY_SUSPENDED = 1,
                          XE_FLAG_SYSTEM_THREAD = 2,
                          XE_FLAG_PRIORITY_CLASS1 = 0x20,
                          XE_FLAG_PRIORITY_CLASS2 = 0x40,
                          XE_FLAG_RETURN_KTHREAD_PTR = 0x80,
                          XE_FLAG_AFFINITY_CPU0 = 1U << 24,
                          XE_FLAG_AFFINITY_CPU1 = 1U << 25,
                          XE_FLAG_AFFINITY_CPU2 = 1U << 26,
                          XE_FLAG_AFFINITY_CPU3 = 1U << 27,
                          XE_FLAG_AFFINITY_CPU4 = 1U << 28,
                          XE_FLAG_AFFINITY_CPU5 = 1U << 29;

struct X_KTHREAD;
struct X_KPROCESS;
struct X_KPCR;
struct X_KPRCB;
#pragma pack(push, 1)

enum X_OBJECT_HEADER_FLAGS : uint16_t {
  OBJECT_HEADER_FLAG_NAMED_OBJECT =
      1,  // if set, has X_OBJECT_HEADER_NAME_INFO prior to X_OBJECT_HEADER
  OBJECT_HEADER_FLAG_IS_PERMANENT = 2,
  OBJECT_HEADER_FLAG_CONTAINED_IN_DIRECTORY =
      4,  // this object resides in an X_OBJECT_DIRECTORY
  OBJECT_HEADER_IS_TITLE_OBJECT = 0x10,  // used in obcreateobject

};

// https://www.nirsoft.net/kernel_struct/vista/OBJECT_HEADER.html
struct X_OBJECT_HEADER {
  xe::be<uint32_t> pointer_count;
  xe::be<uint32_t> handle_count;
  xe::be<uint32_t> object_type_ptr;  // -0x8 POBJECT_TYPE
  xe::be<uint16_t> flags;
  uint8_t unknownE;
  uint8_t unknownF;
  // Object lives after this header.
  // (There's actually a body field here which is the object itself)
};
static_assert_size(X_OBJECT_HEADER, 0x10);

struct X_OBJECT_DIRECTORY {
  // each is a pointer to X_OBJECT_HEADER_NAME_INFO
  // i believe offset 0 = pointer to next in bucket
  xe::be<uint32_t> name_buckets[13];
};
static_assert_size(X_OBJECT_DIRECTORY, 0x34);

// https://www.geoffchappell.com/studies/windows/km/ntoskrnl/inc/ntos/ob/object_header_name_info.htm
// quite different, though
struct X_OBJECT_HEADER_NAME_INFO {
  // i think that this is the next link in an X_OBJECT_DIRECTORY's buckets
  xe::be<uint32_t> next_in_directory;
  xe::be<uint32_t> object_directory;  // pointer to X_OBJECT_DIRECTORY
  X_ANSI_STRING name;
};
struct X_OBJECT_ATTRIBUTES {
  xe::be<uint32_t> root_directory;  // 0x0
  xe::be<uint32_t> name_ptr;        // 0x4 PANSI_STRING
  xe::be<uint32_t> attributes;      // 0xC
};
struct X_OBJECT_TYPE {
  xe::be<uint32_t> allocate_proc;  // 0x0
  xe::be<uint32_t> free_proc;      // 0x4
  xe::be<uint32_t> close_proc;     // 0x8
  xe::be<uint32_t> delete_proc;    // 0xC
  xe::be<uint32_t> unknown_proc;   // 0x10
  xe::be<uint32_t>
      unknown_size_or_object_;  // this seems to be a union, it can be a pointer
                                // or it can be the size of the object
  xe::be<uint32_t> pool_tag;    // 0x18
};
static_assert_size(X_OBJECT_TYPE, 0x1C);

struct X_KSYMLINK {
  xe::be<uint32_t> refed_object_maybe;
  X_ANSI_STRING refed_object_name_maybe;
};
static_assert_size(X_KSYMLINK, 0xC);
// https://msdn.microsoft.com/en-us/library/windows/desktop/aa363082.aspx
typedef struct {
  // Renamed due to a collision with exception_code from Windows excpt.h.
  xe::be<uint32_t> code;
  xe::be<uint32_t> exception_flags;
  xe::be<uint32_t> exception_record;
  xe::be<uint32_t> exception_address;
  xe::be<uint32_t> number_parameters;
  xe::be<uint32_t> exception_information[15];
} X_EXCEPTION_RECORD;
static_assert_size(X_EXCEPTION_RECORD, 0x50);

struct X_KSPINLOCK {
  xe::be<uint32_t> pcr_of_owner;
};
static_assert_size(X_KSPINLOCK, 4);

struct XDPC {
  xe::be<uint16_t> type;
  uint8_t selected_cpu_number;
  uint8_t desired_cpu_number;
  X_LIST_ENTRY list_entry;
  xe::be<uint32_t> routine;
  xe::be<uint32_t> context;
  xe::be<uint32_t> arg1;
  xe::be<uint32_t> arg2;

  void Initialize(uint32_t guest_func, uint32_t guest_context) {
    type = 19;
    selected_cpu_number = 0;
    desired_cpu_number = 0;
    routine = guest_func;
    context = guest_context;
  }
};

struct XAPC {
  static const uint32_t kSize = 40;

  // KAPC is 0x28(40) bytes? (what's passed to ExAllocatePoolWithTag)
  // This is 4b shorter than NT - looks like the reserved dword at +4 is gone.
  // NOTE: stored in guest memory.
  uint16_t type;                     // +0
  uint8_t apc_mode;                  // +2
  uint8_t enqueued;                  // +3
  EZPointer<X_KTHREAD> thread_ptr;   // +4
  X_LIST_ENTRY list_entry;           // +8
  xe::be<uint32_t> kernel_routine;   // +16
  xe::be<uint32_t> rundown_routine;  // +20
  xe::be<uint32_t> normal_routine;   // +24
  xe::be<uint32_t> normal_context;   // +28
  xe::be<uint32_t> arg1;             // +32
  xe::be<uint32_t> arg2;             // +36
};
// https://www.nirsoft.net/kernel_struct/vista/DISPATCHER_HEADER.html
struct X_DISPATCH_HEADER {
  struct {
    uint8_t type;

    union {
      uint8_t abandoned;
      uint8_t absolute;
    };
    uint8_t process_type;  // X_PROCTYPE_
    uint8_t inserted;
  };
  xe::be<int32_t> signal_state;
  X_LIST_ENTRY wait_list;
};
static_assert_size(X_DISPATCH_HEADER, 0x10);

enum : uint16_t {
  WAIT_ALL = 0,
  WAIT_ANY = 1,
};

// https://www.geoffchappell.com/studies/windows/km/ntoskrnl/inc/ntos/ke_x/kwait_block.htm
//  pretty much the vista KWAIT_BLOCK verbatim, except that sparebyte is gone
//  and WaitType is 2 bytes instead of 1
struct X_KWAIT_BLOCK {
  X_LIST_ENTRY wait_list_entry;  // 0x0
  EZPointer<X_KTHREAD> thread;
  EZPointer<X_DISPATCH_HEADER> object;
  EZPointer<X_KWAIT_BLOCK> next_wait_block;
  // this isnt the official vista name, but i think its better.
  // this value is what will be returned to the waiter if this particular wait
  // is satisfied
  xe::be<uint16_t> wait_result_xstatus;
  // WAIT_ALL or WAIT_ANY
  xe::be<uint16_t> wait_type;
};

static_assert_size(X_KWAIT_BLOCK, 0x18);

struct X_KSEMAPHORE {
  X_DISPATCH_HEADER header;
  xe::be<int32_t> limit;
};
static_assert_size(X_KSEMAPHORE, 0x14);

struct X_KMUTANT {
  X_DISPATCH_HEADER header;    // 0x0
  X_LIST_ENTRY unk_list;       // 0x10
  EZPointer<X_KTHREAD> owner;  // 0x18
  bool abandoned;              // 0x1C
  // these might just be padding
  uint8_t unk_1D;  // 0x1D
  uint8_t unk_1E;  // 0x1E
  uint8_t unk_1F;  // 0x1F
};

static_assert_size(X_KMUTANT, 0x20);

// https://www.nirsoft.net/kernel_struct/vista/KEVENT.html
struct X_KEVENT {
  X_DISPATCH_HEADER header;
};
static_assert_size(X_KEVENT, 0x10);

struct X_KTHREAD;
struct X_KPROCESS;
// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_object
struct X_KFILE_OBJECT {
  uint8_t unk_0[0x68];
};
static_assert_size(X_KFILE_OBJECT, 0x68);
struct X_KPRCB {
  EZPointer<X_KTHREAD> current_thread;  // 0x0
  EZPointer<X_KTHREAD> next_thread;     // 0x4
  EZPointer<X_KTHREAD> idle_thread;     // 0x8
  uint8_t current_cpu;                  // 0xC
  uint8_t unk_D[3];                     // 0xD
  // should only have 1 bit set, used for ipis
  xe::be<uint32_t> processor_mask;  // 0x10
  // incremented in clock interrupt
  xe::be<uint32_t> dpc_clock;        // 0x14
  xe::be<uint32_t> interrupt_clock;  // 0x18
  xe::be<uint32_t> unk_1C;           // 0x1C
  xe::be<uint32_t> unk_20;           // 0x20
  // various fields used by KeIpiGenericCall
  xe::be<uint32_t> ipi_args[3];  // 0x24
  // looks like the target cpus clear their corresponding bit
  // in this mask to signal completion to the initiator
  xe::be<uint32_t> targeted_ipi_cpus_mask;  // 0x30
  xe::be<uint32_t> ipi_function;            // 0x34
  // used to synchronize?
  TypedGuestPointer<X_KPRCB> ipi_initiator_prcb;  // 0x38
  xe::be<uint32_t> unk_3C;                        // 0x3C
  xe::be<uint32_t> dpc_related_40;                // 0x40
  // must be held to modify any dpc-related fields in the kprcb
  X_KSPINLOCK dpc_lock;  // 0x44
  util::X_TYPED_LIST<XDPC, offsetof(XDPC, list_entry)> queued_dpcs_list_head;
  // // 0x48
  xe::be<uint32_t> dpc_active;                  // 0x50
  X_KSPINLOCK enqueued_processor_threads_lock;  // 0x54
  // if the idle thread is running, this is set to point to it, else 0
  EZPointer<X_KTHREAD> running_idle_thread;  // 0x58
  // definitely scheduler related
  X_SINGLE_LIST_ENTRY enqueued_threads_list;  // 0x5C
  // if bit 0 set, have a thread at priority 0, etc
  xe::be<uint32_t> has_ready_thread_by_priority;  // 0x60
  // i think the following mask has something to do with the array that comes
  // after
  xe::be<uint32_t> unk_mask_64;  // 0x64
  // have to hardcode this offset, KTHREAD not defined yet
  util::X_TYPED_LIST<X_KTHREAD, 0x94> ready_threads_by_priority[32];  // 0x68
  // ExTerminateThread tail calls a function that does KeInsertQueueDpc of this
  // dpc
  XDPC thread_exit_dpc;  // 0x168
  // thread_exit_dpc's routine drains this list and frees each threads threadid,
  // kernel stack and dereferences the thread
  X_LIST_ENTRY terminating_threads_list;  // 0x184
  XDPC switch_thread_processor_dpc;       // 0x18C
};
// Processor Control Region
struct X_KPCR {
  xe::be<uint32_t> tls_ptr;   // 0x0
  xe::be<uint32_t> msr_mask;  // 0x4
  union {
    xe::be<uint16_t> software_interrupt_state;  // 0x8
    struct {
      // covers timers, dpcs, thread switches
      uint8_t generic_software_interrupt;    // 0x 8                   // 0x8
      uint8_t apc_software_interrupt_state;  // 0x9
    };
  };
  xe::be<uint16_t> unk_0A;           // 0xA
  uint8_t processtype_value_in_dpc;  // 0xC
  uint8_t timeslice_ended;           // 0xD
  uint8_t timer_pending;             // 0xE
  uint8_t unk_0F;                    // 0xF
  // used in KeSaveFloatingPointState / its vmx counterpart
  xe::be<uint32_t> thread_fpu_related;   // 0x10
  xe::be<uint32_t> thread_vmx_related;   // 0x14
  uint8_t current_irql;                  // 0x18
  uint8_t background_scheduling_active;  // 0x19
  uint8_t background_scheduling_1A;      // 0x1A
  uint8_t background_scheduling_1B;      // 0x1B
  xe::be<uint32_t> timer_related;        // 0x1C
  uint8_t unk_20[0x10];                  // 0x20
  xe::be<uint64_t> pcr_ptr;              // 0x30

  // this seems to be just garbage data? we can stash a pointer to context here
  // as a hack for now
  union {
    uint8_t unk_38[8];  // 0x38
    // points to XenonInterruptController
    uint64_t emulated_interrupt;  // 0x38
  };
  uint8_t unk_40[28];                      // 0x40
  xe::be<uint32_t> unk_stack_5c;           // 0x5C
  uint8_t unk_60[12];                      // 0x60
  xe::be<uint32_t> use_alternative_stack;  // 0x6C
  xe::be<uint32_t> stack_base_ptr;  // 0x70 Stack base address (high addr)
  xe::be<uint32_t> stack_end_ptr;   // 0x74 Stack end (low addr)

  // maybe these are the stacks used in apcs?
  // i know they're stacks, RtlGetStackLimits returns them if another var here
  // is set

  xe::be<uint32_t> alt_stack_base_ptr;  // 0x78
  xe::be<uint32_t> alt_stack_end_ptr;   // 0x7C
  // if bit 1 is set in a handler pointer, it actually points to a KINTERRUPT
  // otherwise, it points to a function to execute
  xe::be<uint32_t> interrupt_handlers[32];  // 0x80
  X_KPRCB prcb_data;                        // 0x100
  // pointer to KPCRB?
  TypedGuestPointer<X_KPRCB> prcb;  // 0x2A8
  uint8_t unk_2AC[0x2C];            // 0x2AC
};

/*
    there must be two timer structures, because the size passed to
   ObCreateObject does not make sense if we apply that structure size to the
   timer embedded in KTHREAD
*/
struct X_KTIMER {
  X_DISPATCH_HEADER header;         // 0x0
  xe::be<uint64_t> due_time;        // 0x10
  X_LIST_ENTRY table_bucket_entry;  // 0x18
  TypedGuestPointer<XDPC> dpc;      // 0x20
  xe::be<uint32_t> period;          // 0x24
};
static_assert_size(X_KTIMER, 0x28);

struct X_EXTIMER {
  X_KTIMER ktimer;                       // 0x0
  XDPC dpc;                              // 0x28
  XAPC apc;                              // 0x44
  X_LIST_ENTRY thread_timer_list_entry;  // 0x6C
  X_KSPINLOCK timer_lock;                // 0x74
  // not confident in this name
  xe::be<uint32_t> period;  // 0x78
  bool has_apc;             // 0x7C
  uint8_t unk_7D[3];        // 0x7D
};

static_assert_size(X_EXTIMER, 0x80);

// iocompletions appear to just be a KQUEUE under another name
// seems to exactly match normal nt structure!
// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-keinitializequeue
// todo: figure out where thread_list_head links into 360 KTHREAD
struct X_KQUEUE {
  X_DISPATCH_HEADER header;                               // 0x0
  X_LIST_ENTRY entry_list_head;                           // 0x10
  xe::be<uint32_t> current_count;                         // 0x18
  xe::be<uint32_t> maximum_count;                         // 0x1C
  util::X_TYPED_LIST<X_KTHREAD, 0x11C> thread_list_head;  // 0x20
};
static_assert_size(X_KQUEUE, 0x28);

// just an alias, they are identical structures
using X_KIO_COMPLETION = X_KQUEUE;

struct X_KINTERRUPT {
  xe::be<uint32_t> service_routine;  // 0x0
  xe::be<uint32_t> service_context;  // 0x4
  X_KSPINLOCK spinlock;              // 0x8
  xe::be<uint32_t> service_count;    // 0xC
  uint8_t unk_10;                    // 0x10
  uint8_t irql;                      // 0x11
  uint8_t unk_12;                    // 0x12
  uint8_t unk_13;                    // 0x13
};

static_assert_size(X_KINTERRUPT, 0x14);

struct X_KTHREAD {
  X_DISPATCH_HEADER header;  // 0x0
  util::X_TYPED_LIST<X_KMUTANT, offsetof(X_KMUTANT, unk_list)>
      mutants_list;                  // 0x10
  X_KTIMER wait_timeout_timer;       // 0x18
  X_KWAIT_BLOCK wait_timeout_block;  // 0x40
  uint8_t unk_58[0x4];               // 0x58
  xe::be<uint32_t> stack_base;       // 0x5C
  xe::be<uint32_t> stack_limit;      // 0x60
  xe::be<uint32_t> stack_kernel;     // 0x64
  xe::be<uint32_t> tls_address;      // 0x68
  // state = is thread running, suspended, etc
  uint8_t thread_state;  // 0x6C
  // 0x70 = priority?

  uint8_t alerted[2];         // 0x6D
  uint8_t alertable;          // 0x6F
  uint8_t priority;           // 0x70
  uint8_t fpu_exceptions_on;  // 0x71
  // these two process types both get set to the same thing, process_type is
  // referenced most frequently, however process_type_dup gets referenced a few
  // times while the process is being created
  uint8_t process_type_dup;
  uint8_t process_type;
  // apc_mode determines which list an apc goes into
  util::X_TYPED_LIST<XAPC, offsetof(XAPC, list_entry)> apc_lists[2];
  EZPointer<X_KPROCESS> process;  // 0x84
  uint8_t executing_kernel_apc;   // 0x88
  // when context switch happens, this is copied into
  // apc_software_interrupt_state for kpcr
  uint8_t deferred_apc_software_interrupt_state;  // 0x89
  uint8_t user_apc_pending;                       // 0x8A
  uint8_t may_queue_apcs;                         // 0x8B
  X_KSPINLOCK apc_lock;                           // 0x8C
  xe::be<uint32_t> num_context_switches_to;       // 0x90
  X_LIST_ENTRY ready_prcb_entry;                  // 0x94
  xe::be<uint32_t> msr_mask;                      // 0x9C
  xe::be<X_STATUS> wait_result;                   // 0xA0
  uint8_t wait_irql;                              // 0xA4
  uint8_t processor_mode;                         // 0xA5
  uint8_t wait_next;                              // 0xA6
  uint8_t wait_reason;                            // 0xA7
  EZPointer<X_KWAIT_BLOCK> wait_blocks;           // 0xA8
  uint8_t unk_AC[4];                              // 0xAC
  xe::be<int32_t> apc_disable_count;              // 0xB0
  xe::be<int32_t> quantum;                        // 0xB4
  uint8_t unk_B8;                                 // 0xB8
  uint8_t unk_B9;                                 // 0xB9
  uint8_t unk_BA;                                 // 0xBA
  uint8_t boost_disabled;                         // 0xBB
  uint8_t suspend_count;                          // 0xBC
  uint8_t was_preempted;                          // 0xBD
  uint8_t terminated;                             // 0xBE
  uint8_t current_cpu;                            // 0xBF
  EZPointer<X_KPRCB> a_prcb_ptr;                  // 0xC0
  EZPointer<X_KPRCB> another_prcb_ptr;            // 0xC4
  uint8_t unk_C8;                                 // 0xC8
  uint8_t unk_C9;                                 // 0xC9
  uint8_t unk_CA;                                 // 0xCA
  uint8_t unk_CB;                                 // 0xCB
  X_KSPINLOCK timer_list_lock;                    // 0xCC
  xe::be<uint32_t> stack_alloc_base;              // 0xD0
  XAPC on_suspend;                                // 0xD4
  X_KSEMAPHORE suspend_sema;                      // 0xFC
  X_LIST_ENTRY process_threads;                   // 0x110
  EZPointer<X_KQUEUE> queue;                      // 0x118
  X_LIST_ENTRY queue_related;                     // 0x11c
  xe::be<uint32_t> unk_124;                       // 0x124
  xe::be<uint32_t> unk_128;                       // 0x128
  xe::be<uint32_t> unk_12C;                       // 0x12C
  xe::be<uint64_t> create_time;                   // 0x130
  xe::be<uint64_t> exit_time;                     // 0x138
  xe::be<uint32_t> exit_status;                   // 0x140
  // tracks all pending timers that have apcs which target this thread
  X_LIST_ENTRY timer_list;          // 0x144
  xe::be<uint32_t> thread_id;       // 0x14C
  xe::be<uint32_t> start_address;   // 0x150
  X_LIST_ENTRY unk_154;             // 0x154
  uint8_t unk_15C[0x4];             // 0x15C
  xe::be<uint32_t> last_error;      // 0x160
  xe::be<uint32_t> fiber_ptr;       // 0x164
  uint8_t unk_168[0x4];             // 0x168
  xe::be<uint32_t> creation_flags;  // 0x16C

  // we handle context differently from a native kernel, so we can stash extra
  // data here! the first 8 bytes of vscr are unused anyway
  union {
    vec128_t vscr;  // 0x170
    struct {
      void* host_xthread_stash;
      uintptr_t vscr_remainder;
    };
  };
  union {
    // 2048 bytes
    vec128_t vmx_context[128];  // 0x180
    struct {
      // 1536 bytes
      X_KWAIT_BLOCK scratch_waitblock_memory[65];
      // space for some more data!
      uint32_t kernel_aux_stack_base_;
      uint32_t kernel_aux_stack_current_;
      uint32_t kernel_aux_stack_limit_;
    };
  };
  xe::be<double> fpscr;            // 0x980
  xe::be<double> fpu_context[32];  // 0x988

  XAPC unk_A88;  // 0xA88

  // This struct is actually quite long... so uh, not filling this out!
};
static_assert_size(X_KTHREAD, 0xAB0);

static_assert(offsetof(X_KTHREAD, apc_lists[0]) == 0x74);
struct alignas(4096) X_KPCR_PAGE {
  X_KPCR pcr;        // 0x0
  char unk_2D8[40];  // 0x2D8
  X_KTHREAD idle_process_thread;
};

// (?), used by KeGetCurrentProcessType
constexpr uint32_t X_PROCTYPE_IDLE = 0;
constexpr uint32_t X_PROCTYPE_TITLE = 1;
constexpr uint32_t X_PROCTYPE_SYSTEM = 2;

struct X_KPROCESS {
  X_KSPINLOCK thread_list_spinlock;
  // list of threads in this process, guarded by the spinlock above
  util::X_TYPED_LIST<X_KTHREAD, offsetof(X_KTHREAD, process_threads)>
      thread_list;
  // quantum value assigned to each thread of the process
  xe::be<int32_t> quantum;
  // kernel sets this to point to a section of size 0x2F700 called CLRDATAA,
  // except it clears bit 31 of the pointer. in 17559 the address is 0x801C0000,
  // so it sets this ptr to 0x1C0000
  xe::be<uint32_t> clrdataa_masked_ptr;
  xe::be<uint32_t> thread_count;
  uint8_t unk_18;
  uint8_t unk_19;
  uint8_t unk_1A;
  uint8_t unk_1B;
  xe::be<uint32_t> kernel_stack_size;
  xe::be<uint32_t> tls_static_data_address;
  xe::be<uint32_t> tls_data_size;
  xe::be<uint32_t> tls_raw_data_size;
  xe::be<uint16_t> tls_slot_size;
  // ExCreateThread calls a subfunc references this field, returns
  // X_STATUS_PROCESS_IS_TERMINATING if true
  uint8_t is_terminating;
  // one of X_PROCTYPE_
  uint8_t process_type;
  xe::be<uint32_t> tls_slot_bitmap[8];
  xe::be<uint32_t> unk_50;
  X_LIST_ENTRY unk_54;
  xe::be<uint32_t> unk_5C;
};
static_assert_size(X_KPROCESS, 0x60);

struct X_EVENT_INFORMATION {
  xe::be<uint32_t> type;
  xe::be<uint32_t> signal_state;
};

struct X_RTL_CRITICAL_SECTION {
  X_DISPATCH_HEADER header;
  xe::be<int32_t> lock_count;       // 0x10 -1 -> 0 on first lock
  xe::be<int32_t> recursion_count;  // 0x14  0 -> 1 on first lock
  xe::be<uint32_t> owning_thread;   // 0x18 PKTHREAD 0 unless locked
};
static_assert_size(X_RTL_CRITICAL_SECTION, 28);
#pragma pack(pop)

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_KERNEL_GUEST_STRUCTURES_H_
