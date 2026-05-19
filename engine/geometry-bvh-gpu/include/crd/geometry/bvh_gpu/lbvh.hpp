#pragma once

// ---------------------------------------------------------------------------
// LBVH (Karras 2012 + KittenGpuLBVH fat-node 64 B layout) tree build +
// AABB upsweep. Phase 3.1.7 v9a-c-followon "Track A elite rewrite"
// (2026-05-18).
//
// Pipeline composition (each stage's output is the next stage's input):
//
//   AABBs  →  compute_morton_codes  →  sort_morton_pairs  →  build_lbvh  →  LbvhTree (CPU) | GpuResidentTree (GPU)
//   (v9a-a Morton)                  (v9a-b1/b2 sort)        (this slice)
//
// THE CPU REFERENCE IS THE ALGORITHM DEFINITION (D134 discipline scales).
// `build_lbvh_cpu<KeyT>` produces the canonical byte-identical fat-node
// output. The GPU `LbvhGpuPipeline::dispatch_build_lbvh*` is a mechanical
// translation conformance-tested via byte-equality on the fat-node array
// + ULP-identical bounds (D162; commutative AABB union is bit-deterministic
// across run-to-run for finite-non-NaN inputs).
//
// =========================================================================
// PINNED DESIGN DECISIONS (carried via ADR-0076 §25 amendment + v9a-c-
// followon elite-rewrite log 2026-05-18):
//
//   D157 — CPU reference IS the algorithm definition. `build_lbvh_cpu`
//          serves as the byte-identical oracle for GPU conformance.
//
//   D158 — Internal-node AABBs are computed by the upsweep within this
//          same function (NOT a separate slice — fully queryable on return).
//
//   D159 — Upsweep uses Karras 2012 §2.4 atomic-counter parent-walk. AABB
//          union is commutative+associative, so output is bit-deterministic
//          across thread arrival orders. CPU reference mimics this
//          structure (serial bottom-up via leaf-parent lookup tables) and
//          produces byte-identical output. ONE GPU dispatch instead of
//          log₂N per-level barriers.
//
//   D162 — Conformance contract = byte-equality on LbvhFatNode fields
//          (parent_idx, left_idx, right_idx, fence) + ULP-identical
//          on inlined bounds[0..1] (1 ULP at our scale).
//
//   D164 — Equal-key Karras `delta` tiebreak uses original input indices
//          as secondary key (Karras 2012 convention: when two codes are
//          equal, treat the (code, index) pair as if it were a higher-
//          bit-width key with the index appended). Load-bears on the
//          stable-sort property of v9a-b1/b2.
//
//   D165 — **Fat-node 64 B layout + dual output paths** (elite-rewrite
//          2026-05-18). `LbvhTree` carries `Array<LbvhFatNode>` (64 B per
//          internal node — both children's bounds INLINE) NOT the canonical
//          32 B `BvhNode` of `crd-geometry-bvh`. Justification: KittenGpuLBVH
//          shows fat-node + carry-register upsweep hits ~1.5 ms / 1M on RTX
//          3090; compact-node + canonical reorder was 28 ms on RTX 4070 Ti
//          SUPER (v9a-c). Industry precedent: NVIDIA OptiX, AMD Radeon Rays,
//          KittenGpu — all use fat-node layouts with explicit child indices.
//          Two dispatch paths exposed:
//            (a) `dispatch_build_lbvh` — CPU-output path; reads back to
//                `LbvhTree`. Cost ~21 ms / 1M (12 ms readback).
//            (b) `dispatch_build_lbvh_gpu_resident` — GPU-resident output;
//                handle references pipeline-owned buffers. Cost ~7.6 ms / 1M.
//                Use this from eylem broadphase / GPU traversal / occlusion
//                culling. Drops the 64 MB readback.
//          `lbvh_to_bvh_tree()` conversion deferred; ship at consumer arrival.
//
// =========================================================================
//
// Out of scope:
//   - Parallel CPU LBVH (`v9a-c-parallel-cpu` follow-on; serial reference
//     is the algorithm definition).
//   - 60-bit u64 LBVH (`v9a-c-60bit` follow-on; settled scale-up).
//   - N > 1M (kRadixMaxItems cap; `v9a-c-large` follow-on).
//   - GPU-resident inputs (`v9a-c-gpu-inputs` follow-on; eylem broadphase
//     keeps pairs + leaf_aabbs on the GPU after morton + radix sort).
//   - `lbvh_to_bvh_tree()` conversion (`v9a-c-bvh-tree-bridge` follow-on
//     when first consumer needs compact-node BvhTree compatibility).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh_gpu/lbvh_tree.hpp>
#include <crd/geometry/bvh_gpu/morton_sort.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/memory/allocator.hpp>

#include <cstdint>
#include <memory>

namespace crd::rhi
{
class Buffer;
class Device;
}

namespace crd::geometry::bvh_gpu
{

// CPU reference. THE algorithm definition.
//
//   sorted_pairs : output of `sort_morton_pairs<KeyT>(codes, alloc)`; pairs
//                  must be ascending by `code` (the sort's contract). Equal
//                  codes must preserve input-index order (the sort's
//                  stability contract).
//
//   leaf_aabbs   : the ORIGINAL AABB array indexed by `sorted_pairs[k].index`
//                  (NOT in sorted order). The upsweep reads
//                  `leaf_aabbs[sorted_pairs[k].index]` for each leaf k.
//
//   alloc        : caller-owned allocator; the returned BvhTree binds to it.
//
// Returns a BvhTree with 2N-1 nodes (N-1 internal + N leaves) in canonical
// `BvhTree` layout. The returned tree is FULLY QUERYABLE — internal-node
// AABBs are populated by the upsweep step.
//
//   N=0 → empty tree
//   N=1 → 1 leaf node, no upsweep
//   N≥2 → 2N-1 nodes with full topology + AABBs
template <typename KeyT>
[[nodiscard]] LbvhTree
build_lbvh_cpu(crd::containers::ConstSpan<MortonPair<KeyT>>                sorted_pairs,
               crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> leaf_aabbs,
               crd::memory::IAllocator*                                    alloc) noexcept;

extern template LbvhTree
build_lbvh_cpu<crd::u32>(crd::containers::ConstSpan<MortonPair<crd::u32>>,
                          crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>>,
                          crd::memory::IAllocator*) noexcept;

extern template LbvhTree
build_lbvh_cpu<std::uint64_t>(crd::containers::ConstSpan<MortonPair<std::uint64_t>>,
                                crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>>,
                                crd::memory::IAllocator*) noexcept;

// GPU pipeline. Holds the cached compute pipelines + descriptor allocators.
// One instance can serve many `dispatch_build_lbvh` calls; rebuild only when
// the device changes.
class LbvhGpuPipeline
{
public:
    LbvhGpuPipeline(crd::rhi::Device& device, crd::containers::StringView shader_dir) noexcept;

    LbvhGpuPipeline(const LbvhGpuPipeline&)            = delete;
    LbvhGpuPipeline& operator=(const LbvhGpuPipeline&) = delete;
    LbvhGpuPipeline(LbvhGpuPipeline&&) noexcept;
    LbvhGpuPipeline& operator=(LbvhGpuPipeline&&) noexcept;
    ~LbvhGpuPipeline();

    [[nodiscard]] bool is_valid() const noexcept;

    // Build a queryable LBVH end-to-end on the GPU. Synchronous: uploads
    // inputs, dispatches build + upsweep, reads back to CPU memory, returns
    // the LbvhTree. Use this from tests, debug, or CPU-side consumers.
    //
    // Output BYTE-IDENTICAL to `build_lbvh_cpu<KeyT>(sorted_pairs, leaf_aabbs, alloc)`
    // on the topology fields, ULP-identical on internal-node AABBs (D162).
    //
    // Cost at 1 M / RTX 4070 Ti SUPER (win-shipping, 2026-05-18 elite rewrite):
    //   - GPU compute: ~10 ms
    //   - CPU readback (64 MB): ~12 ms
    //   - CPU staging + cmd-buffer record: ~3 ms
    //   - TOTAL: ~25-26 ms
    //
    // For elite-tier perf consumers (eylem broadphase, on-GPU traversal),
    // prefer `dispatch_build_lbvh_gpu_resident` which drops the readback.
    [[nodiscard]] LbvhTree
    dispatch_build_lbvh(crd::containers::ConstSpan<MortonPair<crd::u32>>                  sorted_pairs,
                        crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> leaf_aabbs,
                        crd::memory::IAllocator*                                          alloc) noexcept;

    // GPU-resident output handle: holds non-owning pointers to GPU buffers
    // populated by `dispatch_build_lbvh_gpu_resident`. The buffers are owned
    // by the pipeline (cached at kRadixMaxItems size); the handle is valid
    // until the next `dispatch_build_lbvh*` call on this pipeline overwrites
    // the internal buffers.
    //
    // **Layout** matches the on-CPU `LbvhTree`:
    //   - `nodes` is the fat-node array (LbvhFatNode, 64 bytes each).
    //   - `prim_indices` is the per-leaf primitive index in sorted order
    //     (u32 each).
    //   - `internal_count` = N - 1 (or 0 if `prim_count == 1`).
    //   - `prim_count` = N.
    //   - `nodes_byte_size` = (N - 1) * 64.
    //   - `prim_indices_byte_size` = N * 4.
    struct GpuResidentTree
    {
        crd::rhi::Buffer* nodes        = nullptr;
        crd::rhi::Buffer* prim_indices = nullptr;
        crd::u32          internal_count        = 0U;
        crd::u32          prim_count            = 0U;
        crd::u64          nodes_byte_size       = 0U;
        crd::u64          prim_indices_byte_size = 0U;
    };

    // GPU-resident path: dispatches build + upsweep, copies prim_indices into
    // a GPU buffer (from sorted_pairs), but DOES NOT read the nodes back to
    // CPU. Synchronous (waits the fence); the returned handle's buffers are
    // immediately usable by subsequent GPU work (e.g. binding as a
    // descriptor in a traversal kernel).
    //
    // **Why it's faster:** no 64 MB readback at 1 M. Drops ~12 ms / 1 M from
    // the total wall time. Best for eylem broadphase and any consumer that
    // queries the LBVH on the GPU.
    //
    // **Lifetime:** the returned `GpuResidentTree::nodes` and `prim_indices`
    // pointers reference the pipeline's internal cached buffers (`nodes_gpu`
    // + `prim_indices_gpu`). They REMAIN VALID until the next call to any
    // `dispatch_build_lbvh*` on this pipeline (which overwrites them). Do
    // NOT free them — the pipeline owns them.
    [[nodiscard]] GpuResidentTree
    dispatch_build_lbvh_gpu_resident(
        crd::containers::ConstSpan<MortonPair<crd::u32>>                  sorted_pairs,
        crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> leaf_aabbs) noexcept;

    // ---- v9a-c-gpu-inputs ---------------------------------------------------
    // Fully GPU-resident I/O: inputs come from caller-supplied GPU buffers,
    // outputs are the same pipeline-owned GPU buffers as
    // `dispatch_build_lbvh_gpu_resident`. NO CPU staging memcpy, no CPU-to-GPU
    // transfer of inputs. Saves ~2-3 ms / 1 M at the upload axis.
    //
    // Caller is responsible for:
    //   - The buffers being populated with valid sorted Morton pairs +
    //     leaf AABBs in original-prim-index order.
    //   - The buffers being in `ComputeShaderRead` state when the pipeline's
    //     internal cmd buffer runs (the dispatch barrier-transitions FROM
    //     this state).
    //   - Buffer lifetime: must outlive the dispatch call (the dispatch is
    //     synchronous so this is trivial — caller can free immediately after).
    //
    // Expected usage:
    //   morton_codes_gpu → sort_morton_pairs_gpu → THIS DISPATCH → consumer
    //                       (output: sorted pairs)  (output: GpuResidentTree)
    //
    // No CPU intervention in the pipeline. Ideal for eylem broadphase tick.
    struct GpuInputView
    {
        crd::rhi::Buffer* sorted_pairs = nullptr;   // 2N × u32 (code, prim_idx interleaved)
        crd::rhi::Buffer* leaf_aabbs   = nullptr;   // 6N × f32 (min.xyz, max.xyz per prim)
        crd::u32          n            = 0U;        // primitive count
    };

    [[nodiscard]] GpuResidentTree
    dispatch_build_lbvh_from_gpu(const GpuInputView& inputs) noexcept;

    // ---- v9b: GPU BVH refit -------------------------------------------------
    // Recompute internal-node AABBs given NEW leaf AABBs, holding the existing
    // tree TOPOLOGY untouched. Use this every frame in a dynamic-body
    // broadphase where positions change but the set of bodies (and therefore
    // the spatial hierarchy structure) does not.
    //
    // **Why it's faster than a full build:** the build kernel is gone (~1 ms
    // saved at 1 M); the extract_prim_indices kernel is gone (already
    // populated by the prior build call); the upsweep is the only work left.
    //
    // **Contract on inputs:**
    //   - `sorted_pairs` MUST be the same buffer (with the same contents) that
    //     was passed to the most recent `dispatch_build_lbvh_from_gpu` /
    //     `dispatch_build_lbvh_gpu_resident` call. The consumer keeps this
    //     buffer alive across the build → refit cycle as part of their scene
    //     representation. Reusing a stale or different sort breaks topology
    //     correspondence.
    //   - `leaf_aabbs` are the NEW per-primitive AABBs, in ORIGINAL prim-index
    //     order (same indexing scheme as build: `leaf_aabbs[prim_idx]`).
    //   - `n` MUST match the n from the most recent build call.
    //   - Both buffers in `ComputeShaderRead` state when the dispatch runs.
    //
    // The returned handle is the SAME pipeline-owned nodes_gpu / prim_indices_gpu
    // as the previous build's output, with bounds updated in-place. Consumers
    // holding the previous handle's pointers are still valid (pointed buffers
    // unchanged — only the bytes inside `nodes_gpu` change).
    //
    // Cost at 1 M / RTX 4070 Ti SUPER target: ~0.5-1 ms (upsweep + done reset
    // + minimal cmd-buffer overhead). Memory-bandwidth-bound; same ~0.24 ms
    // theoretical floor as the upsweep portion of a full build.
    struct RefitInputs
    {
        crd::rhi::Buffer* sorted_pairs = nullptr;  // SAME buffer + contents as the prior build
        crd::rhi::Buffer* leaf_aabbs   = nullptr;  // NEW values, original prim-index order
        crd::u32          n            = 0U;       // MUST match prior build's n
    };

    [[nodiscard]] GpuResidentTree
    dispatch_refit_lbvh(const RefitInputs& inputs) noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::geometry::bvh_gpu
