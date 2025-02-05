/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>

#include "xenia/gpu/graphics_system.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
DEFINE_bool(
    store_shaders, true,
    "Store shaders persistently and load them when loading games to avoid "
    "runtime spikes and freezes when playing the game not for the first time.",
    "GPU");

namespace xe {
namespace gpu {

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if XE_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // XE_PLATFORM_WIN32

GraphicsSystem::GraphicsSystem() : vsync_worker_running_(false) {
  register_file_ = reinterpret_cast<RegisterFile*>(memory::AllocFixed(
      nullptr, sizeof(RegisterFile), memory::AllocationType::kReserveCommit,
      memory::PageAccess::kReadWrite));
}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::Setup(cpu::Processor* processor,
                               kernel::KernelState* kernel_state,
                               ui::WindowedAppContext* app_context,
                               [[maybe_unused]] bool is_surface_required) {
  memory_ = processor->memory();
  processor_ = processor;
  kernel_state_ = kernel_state;
  app_context_ = app_context;

  if (provider_) {
    // Safe if either the UI thread call or the presenter creation fails.
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() {
        presenter_ = provider_->CreatePresenter(
            [this](bool is_responsible, bool statically_from_ui_thread) {
              OnHostGpuLossFromAnyThread(is_responsible);
            });
      });
    } else {
      // May be needed for offscreen use, such as capturing the guest output
      // image.
      presenter_ = provider_->CreatePresenter(
          [this](bool is_responsible, bool statically_from_ui_thread) {
            OnHostGpuLossFromAnyThread(is_responsible);
          });
    }
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    XELOGE("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }

  // Let the processor know we want register access callbacks.
  memory_->AddVirtualMappedRange(
      0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
      reinterpret_cast<cpu::MMIOReadCallback>(ReadRegisterThunk),
      reinterpret_cast<cpu::MMIOWriteCallback>(WriteRegisterThunk));

  AddConstantRegisterValue(0x0F00, 0x08100748);  // RB_EDRAM_TIMING
  AddConstantRegisterValue(0x0F01, 0x0000200E);  // RB_BC_CONTROL

  AddConstantRegisterValue(0x194C, 0x000002D0);  // R500_D1MODE_V_COUNTER
  AddConstantRegisterValue(0x1951, 1);           // interrupt status, vblank
  AddConstantRegisterValue(
      0x1961, 0x050002D0);  // AVIVO_D1MODE_VIEWPORT_SIZE
                            // Screen res - 1280x720
                            // maximum [width(0x0FFF), height(0x0FFF)]
  if (cvars::trace_gpu_stream) {
    BeginTracing();
  }

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::AddConstantRegisterValue(uint32_t gpu_register,
                                              uint32_t value) {
  auto range = memory_->LookupVirtualMappedRange(0x7FC80000);

  range->constant_addresses[0x7FC80000 + (gpu_register * 4)] = value;
}
void GraphicsSystem::SetupVsync() {
#if XE_USE_TIMED_INTERRUPTS_FOR_VSYNC
  //1000 microseconds = one millisecond, 1000 milliseconds = 1 second
  vsync_relative_ts_ = cvars::vsync ? (1000ULL * 1000ULL) / cvars::vsync_fps
                                    : (1000ULL * 1000ULL);
  auto vsync_target_thread = processor()->GetCPUThread(2);

  auto interrupt_controller = vsync_target_thread->interrupt_controller();

  cpu::CpuTimedInterrupt vsync_cti;
  vsync_cti.destination_microseconds_ =
      interrupt_controller->CreateRelativeUsTimestamp(
          vsync_relative_ts_); // one second / vsync_fps

  vsync_cti.ud_ = reinterpret_cast<void*>(this);
  vsync_cti.enqueue_ = &GraphicsSystem::VsyncInterruptEnqueueProcedure;
  uint32_t clock_slot = interrupt_controller->AllocateTimedInterruptSlot();
  interrupt_controller->SetTimedInterruptArgs(clock_slot, &vsync_cti);
  interrupt_controller->RecomputeNextEventCycles();
#else
  // 60hz vsync timer.
  vsync_worker_running_ = true;
  threading::Thread::CreationParameters crparams{};
  crparams.create_suspended = false;
  crparams.stack_size = 16 * 1024 * 1024;
  vsync_worker_thread_ = threading::Thread::Create(crparams, [this]() {
    const double vsync_duration_d =
        cvars::vsync ? std::max<double>(
                           5.0, 1000.0 / static_cast<double>(cvars::vsync_fps))
                     : 1.0;
    uint64_t last_frame_time = Clock::QueryGuestTickCount();
    // Sleep for 90% of the vblank duration, spin for 10%
    const double duration_scalar = 0.90;

    while (vsync_worker_running_) {
      const uint64_t current_time = Clock::QueryGuestTickCount();
      const uint64_t tick_freq = Clock::guest_tick_frequency();
      const uint64_t time_delta = current_time - last_frame_time;
      const double elapsed_d = static_cast<double>(time_delta) /
                               (static_cast<double>(tick_freq) / 1000.0);
      if (elapsed_d >= vsync_duration_d) {
        last_frame_time = current_time;

        // TODO(disjtqz): should recalculate the remaining time to a vblank
        // after MarkVblank, no idea how long the guest code normally takes
        MarkVblank();
        if (cvars::vsync) {
          const uint64_t estimated_nanoseconds = static_cast<uint64_t>(
              (vsync_duration_d * 1000000.0) *
              duration_scalar);  // 1000 microseconds = 1 ms

          threading::NanoSleep(estimated_nanoseconds);
        }
      }
      if (!cvars::vsync) {
        xe::threading::Sleep(std::chrono::milliseconds(1));
      }
    }
    return 0;
  });
  Emulator::Get()->RegisterGuestHardwareBlockThread(vsync_worker_thread_.get());
  // As we run vblank interrupts the debugger must be able to suspend us.
  vsync_worker_thread_->set_name("GPU VSync");
#endif
}
#if XE_USE_TIMED_INTERRUPTS_FOR_VSYNC

void GraphicsSystem::VsyncInterruptEnqueueProcedure(
    cpu::XenonInterruptController* controller, uint32_t slot, void* ud) {
  GraphicsSystem* thiz = reinterpret_cast<GraphicsSystem*>(ud);

  cpu::CpuTimedInterrupt reschedule_args{};
  reschedule_args.destination_microseconds_ =
      controller->GetSlotUsTimestamp(slot) + thiz->vsync_relative_ts_;
  reschedule_args.ud_ = ud;
  reschedule_args.enqueue_ = &GraphicsSystem::VsyncInterruptEnqueueProcedure;
  controller->SetTimedInterruptArgs(slot, &reschedule_args);

  thiz->MarkVblank();
}
#endif
void GraphicsSystem::Shutdown() {
  if (command_processor_) {
    EndTracing();
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    threading::Wait(vsync_worker_thread_.get(), false);
    Emulator::Get()->UnregisterGuestHardwareBlockThread(
        vsync_worker_thread_.get());
    vsync_worker_thread_.reset();
  }

  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread(
    [[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  xe::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context,
                                           GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs,
                                        uint32_t addr, uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;
  // most of these are handled by AddConstantRegisterValue
  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C:  // R500_D1MODE_V_COUNTER
      return 0x000002D0;
    case 0x1951:  // interrupt status
      return 1;   // vblank
    case 0x1961:  // AVIVO_D1MODE_VIEWPORT_SIZE
                  // Screen res - 1280x720
                  // maximum [width(0x0FFF), height(0x0FFF)]
      return 0x050002D0;
    default:
      if (!register_file()->IsValidRegister(r)) {
        XELOGE("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file()->values[r].u32;
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      XELOGW("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  this->register_file()->values[r].u32 = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr,
                                                uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback,
                                          uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  XELOGGPU("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  kernel_state()->EmulateCPInterrupt(interrupt_callback_,
                                     interrupt_callback_data_, source, cpu);
}

void GraphicsSystem::MarkVblank() {
  SCOPE_profile_cpu_f("gpu");

  // Increment vblank counter (so the game sees us making progress).
  command_processor_->increment_counter();

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread(
      [&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  if (!cvars::store_shaders) {
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(cache_root, title_id, true);
    } else {
      xe::threading::Fence fence;
      command_processor_->CallInThread([this, cache_root, title_id, &fence]() {
        command_processor_->InitializeShaderStorage(cache_root, title_id, true);
        fence.Signal();
      });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread([this, cache_root, title_id]() {
      command_processor_->InitializeShaderStorage(cache_root, title_id, false);
    });
  }
}

void GraphicsSystem::RequestFrameTrace() {
  command_processor_->RequestFrameTrace(cvars::trace_gpu_prefix);
}

void GraphicsSystem::BeginTracing() {
  command_processor_->BeginTracing(cvars::trace_gpu_prefix);
}

void GraphicsSystem::EndTracing() { command_processor_->EndTracing(); }

void GraphicsSystem::Pause() {
  paused_ = true;

  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;

  command_processor_->Resume();
}

bool GraphicsSystem::Save(ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);

  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();

  return command_processor_->Restore(stream);
}

void GraphicsSystem::SetKernelState(xe::kernel::KernelState* ks) {
  kernel_state_ = ks;
  command_processor()->SetKernelState(ks);
}

}  // namespace gpu
}  // namespace xe
