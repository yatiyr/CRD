#pragma once

// ---------------------------------------------------------------------------
// crd-rhi-vulkan -- VulkanProfilerBackend (Detour D-003 v0d).
//
// Implements `crd::perf::IProfilerGpuBackend` over `VkQueryPool` +
// `vkCmdWriteTimestamp` + `vkGetQueryPoolResults`. One backend instance
// per `crd::rhi::Device`.
//
// Lifetime / threading:
//   - Construct once, after `crd::rhi::Device` is ready and the profiler
//     has been initialised.
//   - Call `crd::perf::set_gpu_backend(backend.get())` to register.
//   - Per-frame: the application calls
//        backend->begin_frame(frame_index);
//        // ... render passes with CRD_PERF_GPU_SCOPE(cmd, "...") ...
//        backend->end_frame();
//        // (after present)
//        backend->resolve_completed_frames();  // or crd::perf::resolve_gpu_frames()
//   - `resolve_completed_frames` is the only entry point that touches the
//     query pool results; safe to call from the main thread once per frame.
//
// Multi-frame-in-flight ring:
//   The query pool is sized to (max_spans_per_frame * frames_in_flight * 2)
//   slots. Each frame index maps to a slot offset; `begin_frame` resets
//   that slot range on a one-shot reset cmd buffer (or via cmd buffer the
//   caller provides -- see `begin_frame(VkCommandBuffer)`). The resolve
//   step skips frames whose GPU work hasn't retired.
//
// Cost when registered but with no spans recorded per frame: one cmd-buffer
// query reset per frame (typically a few hundred ns on the GPU side).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/perf/gpu_scope.hpp>
#include <crd/rhi/device.hpp>

#include <memory>

// VkCommandBuffer forward decl so consumers of this header don't pull
// the full Vulkan header in. Defined in vulkan_core.h.
typedef struct VkCommandBuffer_T* VkCommandBuffer; // NOLINT(modernize-use-using)

namespace crd::rhi
{

struct VulkanProfilerBackendDesc
{
    // Max number of GPU spans recorded per CPU frame. Default 256 -- plenty
    // for a typical engine frame (~20-50 passes, each with a few sub-regions).
    // Each span uses 2 query slots. Cost: ~16 KB at 1024 spans/frame.
    crd::u32 max_spans_per_frame = 256U;

    // CPU frames in flight. Must be >= the swapchain frame-in-flight count
    // (typically 2 or 3); default 4 keeps headroom. Resolve waits this many
    // frames before reading back, guaranteeing the GPU has retired the work.
    crd::u32 frames_in_flight = 4U;
};

// Create a Vulkan profiler backend bound to `device`. Returns an owning
// pointer; caller is responsible for calling `crd::perf::set_gpu_backend`
// (and clearing it before destroying the backend).
[[nodiscard]] std::unique_ptr<crd::perf::IProfilerGpuBackend>
create_vulkan_profiler_backend(crd::rhi::Device& device,
                               VulkanProfilerBackendDesc desc = {});

// ---- Convenience for begin_frame --------------------------------------
//
// `IProfilerGpuBackend::begin_frame` takes no cmd buffer because the
// crd-perf interface is backend-neutral. The Vulkan backend also needs a
// cmd buffer to record the query-pool reset on. Use this helper at the
// start of every render frame's command-buffer recording:
//
//   void begin_frame(crd::rhi::CommandBuffer& cb) {
//       crd::rhi::vulkan_profiler_begin_frame(*g_backend, cb,
//                                             crd::perf::current_frame_index());
//   }
//
// This forwards to the backend's internal "reset queries on this cmd
// buffer" path. Calling `IProfilerGpuBackend::begin_frame(frame_idx)` AND
// `vulkan_profiler_begin_frame(...)` is required (one books the slot;
// the other emits vkCmdResetQueryPool).
void vulkan_profiler_begin_frame(crd::perf::IProfilerGpuBackend& backend,
                                 crd::rhi::CommandBuffer&        cb,
                                 crd::u64                        frame_index) noexcept;

} // namespace crd::rhi
