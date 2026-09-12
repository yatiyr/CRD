#pragma once

// ---------------------------------------------------------------------------
// GPU dispatch entries for the LBVH build pipeline. Phase 3.1.7 v9a.
//
// Each entry takes a `Device&`, uploads input to a storage buffer,
// dispatches a compute pipeline, and reads back to a CPU `Array`. The
// caller owns the allocator that backs returned arrays.
//
// **`MortonGpuPipeline`** caches the heavyweight Vulkan objects
// (compute pipeline, descriptor allocator, descriptor set layout,
// pipeline layout, compiled SPIR-V module) across dispatches. Compiling
// SPIR-V + creating a `VkPipeline` is in the tens-of-milliseconds range;
// reusing the pipeline across many `dispatch_morton_codes` calls means
// the per-dispatch overhead is just the buffer upload + recording.
//
// Test pattern (per `feedback_v9_gpu_sanity_harness`):
//   1. wrap setup in `crd::gpu::ValidationCapture` → assert 0 errors.
//   2. compute CPU oracle via `compute_morton_codes_cpu`.
//   3. dispatch GPU via `MortonGpuPipeline::dispatch_morton_codes`.
//   4. `bit_compare` GPU output vs CPU oracle (must be byte-identical).
//   5. `gpu_determinism_check` 3 rounds (Morton is pure deterministic).
//   6. `CRD_PERF_BUDGET_LE("morton_1m_prims", 0.5, ...)` on perf path.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh_gpu/morton.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/memory/allocator.hpp>

#include <cstdint>
#include <memory>

namespace crd::gpu
{
class IComputeContext;
}

namespace crd::geometry::bvh_gpu
{

// Cached compute-pipeline objects for the Morton-code kernel. Build
// once, dispatch many times. Move-only; tied to the lifetime of the
// `Device` that constructed it.
class MortonGpuPipeline
{
public:
    // Construct (compile shader + create pipeline). On failure (shader
    // file missing, validation error during pipeline create) the
    // returned object satisfies `is_valid() == false`; caller checks
    // before dispatching.
    //
    // `shader_dir` is the directory containing
    // `compute_morton_codes.comp.spv` (the cooker-produced SPIR-V).
    // Defaults to looking next to the executable; pass an explicit path
    // for test-binary scenarios.
    MortonGpuPipeline(crd::gpu::IComputeContext& ctx,
                       crd::containers::StringView shader_dir) noexcept;

    MortonGpuPipeline(const MortonGpuPipeline&)            = delete;
    MortonGpuPipeline& operator=(const MortonGpuPipeline&) = delete;
    MortonGpuPipeline(MortonGpuPipeline&&) noexcept;
    MortonGpuPipeline& operator=(MortonGpuPipeline&&) noexcept;
    ~MortonGpuPipeline();

    [[nodiscard]] bool is_valid() const noexcept;

    // Dispatch the compute kernel. Returns an `Array<u32>` of Morton
    // codes (one per input AABB), byte-identical to
    // `compute_morton_codes_cpu(aabbs, scene_aabb, alloc)`.
    //
    // Empty `aabbs` ⇒ empty output array.
    // Degenerate `scene_aabb` (zero extent on any axis) is handled by
    // the same path as the CPU oracle: that axis maps every centroid
    // to bin 0.
    //
    // **Synchronous**: dispatches + fence.wait() before returning. The
    // perf-budget contract assumes the GPU is otherwise idle.
    [[nodiscard]] crd::containers::Array<crd::u32>
    dispatch_morton_codes(crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
                           const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
                           crd::memory::IAllocator* alloc) noexcept;

    // Phase 3.1.7 v9a-a-async-compute (2026-05-18) — same operation as
    // `dispatch_morton_codes` but submits via the compute queue using a
    // compute-family command pool (or graphics if no dedicated compute
    // family exists). Output is bit-identical to `dispatch_morton_codes`;
    // the only difference is the submission queue family.
    //
    // **Purpose:** validates the async-compute pipeline end-to-end, so
    // future consumers wanting compute-graphics overlap (eylem dynamic
    // broadphase, renderer cull pass) have a tested path. Currently
    // SYNCHRONOUS — the function blocks on the fence before returning.
    // True overlap (caller-owned fence + non-blocking return) is a
    // future enhancement when a real consumer wants it.
    [[nodiscard]] crd::containers::Array<crd::u32>
    dispatch_morton_codes_async(
        crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
        const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
        crd::memory::IAllocator* alloc) noexcept;

    // Forward-declared opaque impl. Public so the dispatch helper
    // (a free function in the .cpp shared between sync + async paths)
    // can reach the cached pipeline state; the definition lives in
    // the .cpp so this does NOT expose any internals. Same pattern as
    // `crd::gpu::ValidationCapture`.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
// MortonGpu60BitPipeline — sibling of MortonGpuPipeline for u64 60-bit
// codes. Phase 3.1.7 v9a-60bit-gpu (2026-05-18).
//
// Requires the `shaderInt64` feature on the underlying device. Ctor
// checks `Device::supports_shader_int64()` and returns invalid pipeline
// if unavailable; caller checks `is_valid()` and gracefully falls back
// to the 30-bit `MortonGpuPipeline`.
//
// Output is `Array<u64>` (`std::uint64_t`). Per slice discipline, the
// CPU oracle in `morton_60bit.hpp` IS the algorithm definition and
// `bit_compare<u64>` enforces byte-identical GPU output.
// ---------------------------------------------------------------------------

class MortonGpu60BitPipeline
{
public:
    MortonGpu60BitPipeline(crd::gpu::IComputeContext& ctx,
                            crd::containers::StringView shader_dir) noexcept;

    MortonGpu60BitPipeline(const MortonGpu60BitPipeline&)            = delete;
    MortonGpu60BitPipeline& operator=(const MortonGpu60BitPipeline&) = delete;
    MortonGpu60BitPipeline(MortonGpu60BitPipeline&&) noexcept;
    MortonGpu60BitPipeline& operator=(MortonGpu60BitPipeline&&) noexcept;
    ~MortonGpu60BitPipeline();

    [[nodiscard]] bool is_valid() const noexcept;

    // Dispatch the 60-bit kernel. Returns Array<u64> byte-identical to
    // `compute_morton_codes_cpu_60bit(aabbs, scene_aabb, alloc)`.
    [[nodiscard]] crd::containers::Array<std::uint64_t>
    dispatch_morton_codes_60bit(
        crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
        const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
        crd::memory::IAllocator* alloc) noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::geometry::bvh_gpu
