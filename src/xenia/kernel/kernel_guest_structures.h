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
#include "xenia/guest_pointers.h"
#include "xenia/kernel/util/native_list.h"
#include "xenia/xbox.h"
namespace xe {
namespace kernel {
enum Irql : uint8_t {
  IRQL_PASSIVE = 0,
  IRQL_APC = 1,
  IRQL_DISPATCH = 2,
  IRQL_DPC = 3,
};
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
  xe::be<uint32_t> prcb_of_owner;
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
  static const uint32_t kDummyKernelRoutine = 0xF00DFF00;
  static const uint32_t kDummyRundownRoutine = 0xF00DFF01;

  // KAPC is 0x28(40) bytes? (what's passed to ExAllocatePoolWithTag)
  // This is 4b shorter than NT - looks like the reserved dword at +4 is gone.
  // NOTE: stored in guest memory.
  uint16_t type;                     // +0
  uint8_t apc_mode;                  // +2
  uint8_t enqueued;                  // +3
  xe::be<uint32_t> thread_ptr;       // +4
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
      uint8_t npx_irql;
      uint8_t signalling;
    };
    uint8_t process_type;  // X_PROCTYPE_
    union {
      uint8_t inserted;
      uint8_t debug_active;
      uint8_t dpc_active;
    };
  };
  xe::be<int32_t> signal_state;
  X_LIST_ENTRY wait_list;
};
static_assert_size(X_DISPATCH_HEADER, 0x10);

enum : uint16_t {
  WAIT_ALL = 0,
  WAIT_ANY = 1,
};
// pretty much the vista KWAIT_BLOCK verbatim, except that sparebyte is gone and
// WaitType is 2 bytes instead of 1
struct X_KWAIT_BLOCK {
  X_LIST_ENTRY wait_list_entry;
  TypedGuestPointer<X_KTHREAD> thread;
  TypedGuestPointer<X_DISPATCH_HEADER> object;
  TypedGuestPointer<X_KWAIT_BLOCK> next_wait_block;
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
  xe::be<uint32_t> limit;
};
static_assert_size(X_KSEMAPHORE, 0x14);

struct X_KMUTANT {
  X_DISPATCH_HEADER header;            // 0x0
  X_LIST_ENTRY unk_list;               // 0x10
  TypedGuestPointer<X_KTHREAD> owner;  // 0x18
  bool abandoned;                      // 0x1C
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
struct X_KPRCB {
  TypedGuestPointer<X_KTHREAD> current_thread;  // 0x0
  TypedGuestPointer<X_KTHREAD> next_thread;     // 0x4
  TypedGuestPointer<X_KTHREAD> idle_thread;     // 0x8
  uint8_t current_cpu;                          // 0xC
  uint8_t unk_D[3];                             // 0xD
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
  xe::be<uint32_t> unk_58;                      // 0x58
  // definitely scheduler related
  X_SINGLE_LIST_ENTRY enqueued_threads_list;  // 0x5C
  xe::be<uint32_t> unk_60;                    // 0x60
  // i think the following mask has something to do with the array that comes
  // after
  xe::be<uint32_t> unk_mask_64;  // 0x64

  X_LIST_ENTRY unk_68[32];  // 0x68
  // ExTerminateThread tail calls a function that does KeInsertQueueDpc of this
  // dpc
  XDPC thread_exit_dpc;  // 0x168
  // thread_exit_dpc's routine drains this list and frees each threads threadid,
  // kernel stack and dereferences the thread
  X_LIST_ENTRY terminating_threads_list;  // 0x184
  XDPC unk_18C;                           // 0x18C
};
// Processor Control Region
struct X_KPCR {
  xe::be<uint32_t> tls_ptr;   // 0x0
  xe::be<uint32_t> msr_mask;  // 0x4
  union {
    xe::be<uint16_t> software_interrupt_state;  // 0x8
    struct {
      uint8_t unknown_8;                     // 0x8
      uint8_t apc_software_interrupt_state;  // 0x9
    };
  };
  uint8_t unk_0A[2];                 // 0xA
  uint8_t processtype_value_in_dpc;  // 0xC
  uint8_t unk_0D[3];                 // 0xD
  // used in KeSaveFloatingPointState / its vmx counterpart
  xe::be<uint32_t> thread_fpu_related;  // 0x10
  xe::be<uint32_t> thread_vmx_related;  // 0x14
  uint8_t current_irql;                 // 0x18
  uint8_t unk_19[0x17];                 // 0x19
  xe::be<uint64_t> pcr_ptr;             // 0x30

  // this seems to be just garbage data? we can stash a pointer to context here
  // as a hack for now
  union {
    uint8_t unk_38[8];    // 0x38
    uint64_t host_stash;  // 0x38
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
  X_DISPATCH_HEADER header;   // 0x0
  xe::be<uint64_t> due_time;  // 0x10
  X_LIST_ENTRY unk_18;        // 0x18
  xe::be<uint32_t> unk_20;    // 0x20
  xe::be<uint32_t> period;    // 0x24
};
static_assert_size(X_KTIMER, 0x28);

struct X_EXTIMER {
  X_KTIMER ktimer;               // 0x0
  uint8_t unk_28[0x80u - 0x28];  // 0x28
};

static_assert_size(X_EXTIMER, 0x80);

struct X_KTHREAD {
  X_DISPATCH_HEADER header;          // 0x0
  X_LIST_ENTRY mutants_list;         // 0x10
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
  uint8_t unk_6D[0x3];        // 0x6D
  uint8_t priority;           // 0x70
  uint8_t fpu_exceptions_on;  // 0x71
  // these two process types both get set to the same thing, process_type is
  // referenced most frequently, however process_type_dup gets referenced a few
  // times while the process is being created
  uint8_t process_type_dup;
  uint8_t process_type;
  // apc_mode determines which list an apc goes into
  util::X_TYPED_LIST<XAPC, offsetof(XAPC, list_entry)> apc_lists[2];
  TypedGuestPointer<X_KPROCESS> process;         // 0x84
  uint8_t unk_88;                                // 0x88
  uint8_t running_kernel_apcs;                   // 0x89
  uint8_t unk_8A;                                // 0x8A
  uint8_t may_queue_apcs;                        // 0x8B
  X_KSPINLOCK apc_lock;                          // 0x8C
  uint8_t unk_90[4];                             // 0x90
  X_LIST_ENTRY ready_prcb_entry;                 // 0x94
  xe::be<uint32_t> msr_mask;                     // 0x9C
  xe::be<X_STATUS> wait_result;                  // 0xA0
  uint8_t unk_A4;                                // 0xA4
  uint8_t unk_A5[3];                             // 0xA5
  TypedGuestPointer<X_KWAIT_BLOCK> wait_blocks;  // 0xA8
  uint8_t unk_AC[4];                             // 0xAC
  int32_t apc_disable_count;                     // 0xB0
  uint8_t unk_B4[4];                             // 0xB4
  uint8_t unk_B8;                                // 0xB8
  uint8_t unk_B9;                                // 0xB9
  uint8_t unk_BA;                                // 0xBA
  uint8_t boost_disabled;                        // 0xBB
  uint8_t suspend_count;                         // 0xBC
  uint8_t unk_BD;                                // 0xBD
  uint8_t terminated;                            // 0xBE
  uint8_t current_cpu;                           // 0xBF
  // these two pointers point to KPRCBs, but seem to be rarely referenced, if at
  // all
  TypedGuestPointer<X_KPRCB> a_prcb_ptr;        // 0xC0
  TypedGuestPointer<X_KPRCB> another_prcb_ptr;  // 0xC4
  uint8_t unk_C8[8];                            // 0xC8
  xe::be<uint32_t> stack_alloc_base;            // 0xD0
  // uint8_t unk_D4[0x5C];               // 0xD4
  XAPC on_suspend;      // 0xD4
  X_KSEMAPHORE unk_FC;  // 0xFC
  // this is an entry in
  X_LIST_ENTRY process_threads;     // 0x110
  xe::be<uint32_t> unkptr_118;      // 0x118
  xe::be<uint32_t> unk_11C;         // 0x11C
  xe::be<uint32_t> unk_120;         // 0x120
  xe::be<uint32_t> unk_124;         // 0x124
  xe::be<uint32_t> unk_128;         // 0x128
  xe::be<uint32_t> unk_12C;         // 0x12C
  xe::be<uint64_t> create_time;     // 0x130
  xe::be<uint64_t> exit_time;       // 0x138
  xe::be<uint32_t> exit_status;     // 0x140
  xe::be<uint32_t> unk_144;         // 0x144
  xe::be<uint32_t> unk_148;         // 0x148
  xe::be<uint32_t> thread_id;       // 0x14C
  xe::be<uint32_t> start_address;   // 0x150
  xe::be<uint32_t> unk_154;         // 0x154
  xe::be<uint32_t> unk_158;         // 0x158
  uint8_t unk_15C[0x4];             // 0x15C
  xe::be<uint32_t> last_error;      // 0x160
  xe::be<uint32_t> fiber_ptr;       // 0x164
  uint8_t unk_168[0x4];             // 0x168
  xe::be<uint32_t> creation_flags;  // 0x16C
  uint8_t unk_170[0xC];             // 0x170
  xe::be<uint32_t> unk_17C;         // 0x17C
  uint8_t unk_180[0x930];           // 0x180

  // This struct is actually quite long... so uh, not filling this out!
};
static_assert_size(X_KTHREAD, 0xAB0);
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
  X_LIST_ENTRY thread_list;

  xe::be<uint32_t> unk_0C;
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
  xe::be<uint32_t> bitmap[8];
  xe::be<uint32_t> unk_50;
  X_LIST_ENTRY unk_54;
  xe::be<uint32_t> unk_5C;
};
static_assert_size(X_KPROCESS, 0x60);

#pragma pack(pop)

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_KERNEL_GUEST_STRUCTURES_H_
