// ---------------------------------------------------------------------------
// LbvhGpuPipeline — GPU-side fat-node LBVH (Karras 2012 + KittenGpuLBVH-style
// 64 B node layout). Phase 3.1.7 v9a-c-followon "Track A elite rewrite"
// (2026-05-18); v17-i-c: migrated off the rendering RHI onto the crd-gpu-context
// COMPUTE layer (VulkanComputeContext) — dedicated compute queue, no rendering.
//
// **TWO** GPU dispatches per call (down from THREE in the compact-node
// version — no separate finalize, because the fat-node array IS the final
// output format):
//
//   1. lbvh_fat_build.comp   — N-1 threads, one internal node each. Writes
//                                left_idx / right_idx (MSB = isLeaf) / fence
//                                + sentinel bounds into self; fixes up the
//                                children's parent_idx (MSB = isRight) — or
//                                writes leaf_parents[k] for leaf-children.
//                                D162 byte-identity with `build_lbvh_cpu<u32>`.
//
//   2. lbvh_fat_upsweep.comp — N threads (leaves). Each carries its bounds
//                                in a REGISTER variable, walks up via
//                                parent_idx, writes into parent.bounds[isRight].
//                                atomicAdd-coordinated; second arriver unions
//                                with the sibling slot (SAME cache line as
//                                its store — zero random tree-walk reads).
//                                `coherent` on the Nodes+ChildrenDone buffers
//                                is load-bearing (Lesson 09).
//
// **Readback** is just the final 64 B nodes array (N-1 × 64 B). No separate
// child_left / child_right / bounds buffers — they don't exist in the
// fat-node layout. For 1 M: ~64 MB nodes readback.
//
// **All working-set buffers cached in the pipeline ctor** at kRadixMaxItems
// capacity. Per-call buffer-create cost: 0 ms.
//
// Synchronous dispatch (VulkanComputeContext::submit_and_wait). Async-compute
// follow-on when a consumer needs overlap.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh_gpu/lbvh.hpp>

#include <crd/containers/string.hpp>
#include <crd/core/assert.hpp>
#include <crd/geometry/bvh_gpu/radix_sort.hpp>   // kRadixMaxItems for pre-allocation
#include <crd/gpu/compute.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace crd::geometry::bvh_gpu
{

namespace
{

using crd::geometry::primitives::AABB3;
namespace gpu = crd::gpu;

struct alignas(16) BuildPushConstants
{
    std::uint32_t n = 0U;
    std::uint32_t pad[3] = {0U, 0U, 0U};
};
static_assert(sizeof(BuildPushConstants) == 16U);

struct alignas(16) UpsweepPushConstants
{
    std::uint32_t n = 0U;
    std::uint32_t pad[3] = {0U, 0U, 0U};
};
static_assert(sizeof(UpsweepPushConstants) == 16U);

struct alignas(16) MergeLevelPushConstants
{
    std::uint32_t n_int = 0U;
    std::uint32_t pad[3] = {0U, 0U, 0U};
};
static_assert(sizeof(MergeLevelPushConstants) == 16U);

} // namespace

struct LbvhGpuPipeline::Impl
{
    gpu::IComputeContext*             ctx = nullptr;
    std::unique_ptr<gpu::ComputePipeline> build{};
    std::unique_ptr<gpu::ComputePipeline> upsweep{};             // carry-register (fallback for small N + degenerate all-equal codes)
    std::unique_ptr<gpu::ComputePipeline> init_leaves{};         // level-by-level pass 1 (leaf scatter)
    std::unique_ptr<gpu::ComputePipeline> merge_level{};         // level-by-level pass 2..K (internal merge)
    std::unique_ptr<gpu::ComputePipeline> extract_prim_indices{}; // v9a-c-gpu-inputs: GPU-side prim_indices extraction
    std::unique_ptr<gpu::ComputePipeline> upsweep_persistent{};  // v9a-c-persistent-threads: atomic-queue upsweep

    // All working-set buffers pre-allocated at kRadixMaxItems capacity in the ctor. Reused across dispatch calls.
    std::unique_ptr<gpu::ComputeBuffer> pairs_staging{};
    std::unique_ptr<gpu::ComputeBuffer> pairs_gpu{};
    std::unique_ptr<gpu::ComputeBuffer> leaf_aabbs_staging{};
    std::unique_ptr<gpu::ComputeBuffer> leaf_aabbs_gpu{};
    std::unique_ptr<gpu::ComputeBuffer> nodes_gpu{};               // 64 B per LbvhFatNode
    std::unique_ptr<gpu::ComputeBuffer> prim_indices_gpu{};        // 4 B per leaf (sorted-order original prim index)
    std::unique_ptr<gpu::ComputeBuffer> prim_indices_staging{};    // CPU-staged source of the GPU prim_indices
    std::unique_ptr<gpu::ComputeBuffer> leaf_parents_gpu{};
    std::unique_ptr<gpu::ComputeBuffer> done_staging{};
    std::unique_ptr<gpu::ComputeBuffer> done_gpu{};
    std::unique_ptr<gpu::ComputeBuffer> nodes_readback{};
    // 4-byte staging holding 0xFFFFFFFF — copied to nodes_gpu offset 0 each call (= nodes[0].parent_idx, root sentinel).
    std::unique_ptr<gpu::ComputeBuffer> root_parent_init_staging{};
    // v9a-c-persistent-threads: 4-byte atomic counter + pre-zeroed staging seed.
    std::unique_ptr<gpu::ComputeBuffer> work_queue_gpu{};
    std::unique_ptr<gpu::ComputeBuffer> work_queue_init_staging{};

    bool valid = false;
};

LbvhGpuPipeline::LbvhGpuPipeline(gpu::IComputeContext&        ctx,
                                 crd::containers::StringView shader_dir) noexcept
    : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.ctx   = &ctx;
    // Kernels requested BY NAME — the backend loads its own cooked kernel; this code names no API + no file format.
    const auto sv = [](const char* s) { return crd::containers::StringView{s}; };

    // Build kernel: in_pairs(0) + out_nodes(1) + out_leaf_parents(2).
    impl.build = ctx.create_pipeline(shader_dir, sv("lbvh_fat_build"), 3, sizeof(BuildPushConstants));
    if (impl.build == nullptr) { return; }
    // Upsweep kernel: nodes(0) + leaf_parents(1) + done(2) + pairs(3) + leaf_aabbs(4).
    impl.upsweep = ctx.create_pipeline(shader_dir, sv("lbvh_fat_upsweep"), 5, sizeof(UpsweepPushConstants));
    if (impl.upsweep == nullptr) { return; }
    // init_leaves uses the same 5-binding layout as upsweep.
    impl.init_leaves = ctx.create_pipeline(shader_dir, sv("lbvh_fat_init_leaves"), 5, sizeof(UpsweepPushConstants));
    if (impl.init_leaves == nullptr) { return; }
    // merge_level: just nodes(0) + done(1).
    impl.merge_level = ctx.create_pipeline(shader_dir, sv("lbvh_fat_merge_level"), 2, sizeof(MergeLevelPushConstants));
    if (impl.merge_level == nullptr) { return; }
    // extract_prim_indices: in_pairs(0) + out_prim_indices(1).
    impl.extract_prim_indices = ctx.create_pipeline(shader_dir, sv("lbvh_fat_extract_prim_indices"), 2, sizeof(UpsweepPushConstants));
    if (impl.extract_prim_indices == nullptr) { return; }
    // upsweep_persistent: 6 storage-buffer bindings (same 5 as regular upsweep + work_queue at binding 5).
    impl.upsweep_persistent = ctx.create_pipeline(shader_dir, sv("lbvh_fat_upsweep_persistent"), 6, sizeof(UpsweepPushConstants));
    if (impl.upsweep_persistent == nullptr) { return; }

    // ---- Pre-allocate all working-set buffers at kRadixMaxItems capacity --
    constexpr crd::u64 max_n           = static_cast<crd::u64>(kRadixMaxItems);
    constexpr crd::u64 max_n_int        = max_n - 1U;
    constexpr crd::u64 max_pairs_bytes  = max_n    * sizeof(MortonPair<crd::u32>);
    constexpr crd::u64 max_aabb_bytes   = max_n    * (6U * sizeof(crd::f32));
    constexpr crd::u64 max_nodes_bytes  = max_n_int * 64U;          // 64 B per LbvhFatNode
    constexpr crd::u64 max_parent_bytes = max_n    * sizeof(crd::u32);
    constexpr crd::u64 max_done_bytes   = max_n_int * sizeof(crd::u32);
    using gpu::compute_usage::storage;
    using gpu::compute_usage::transfer_dst;
    using gpu::compute_usage::transfer_src;

    impl.pairs_staging = ctx.create_buffer(max_pairs_bytes, transfer_src, gpu::ComputeMemory::CpuToGpu);
    impl.pairs_gpu     = ctx.create_buffer(max_pairs_bytes, storage | transfer_dst, gpu::ComputeMemory::GpuOnly);
    impl.leaf_aabbs_staging = ctx.create_buffer(max_aabb_bytes, transfer_src, gpu::ComputeMemory::CpuToGpu);
    impl.leaf_aabbs_gpu     = ctx.create_buffer(max_aabb_bytes, storage | transfer_dst, gpu::ComputeMemory::GpuOnly);
    impl.nodes_gpu = ctx.create_buffer(max_nodes_bytes, storage | transfer_src | transfer_dst, gpu::ComputeMemory::GpuOnly);
    impl.prim_indices_staging = ctx.create_buffer(max_n * sizeof(crd::u32), transfer_src, gpu::ComputeMemory::CpuToGpu);
    impl.prim_indices_gpu     = ctx.create_buffer(max_n * sizeof(crd::u32), storage | transfer_dst | transfer_src, gpu::ComputeMemory::GpuOnly);
    impl.leaf_parents_gpu     = ctx.create_buffer(max_parent_bytes, storage, gpu::ComputeMemory::GpuOnly);
    impl.done_staging = ctx.create_buffer(max_done_bytes, transfer_src, gpu::ComputeMemory::CpuToGpu);
    impl.done_gpu     = ctx.create_buffer(max_done_bytes, storage | transfer_dst, gpu::ComputeMemory::GpuOnly);
    impl.nodes_readback = ctx.create_buffer(max_nodes_bytes, transfer_dst, gpu::ComputeMemory::GpuToCpu);

    impl.root_parent_init_staging = ctx.create_buffer(4U, transfer_src, gpu::ComputeMemory::CpuToGpu);
    if (impl.root_parent_init_staging != nullptr)
    {
        if (auto* dst = static_cast<crd::u32*>(impl.root_parent_init_staging->map()))
        {
            *dst = 0xFFFFFFFFU;
            impl.root_parent_init_staging->unmap();
        }
    }

    // Pre-zero done_staging ONCE in the ctor. Staging buffers are host-coherent CpuToGpu memory the CPU never reads;
    // they stay zeroed across calls, so per-call memsets were a ~0.5-1 ms waste (v9a-c-perf-tune sub-win 2026-05-18).
    if (impl.done_staging != nullptr)
    {
        if (auto* dst = static_cast<crd::u32*>(impl.done_staging->map()))
        {
            constexpr crd::u64 max_done_bytes_ctor = max_done_bytes;
            std::memset(dst, 0, max_done_bytes_ctor);
            impl.done_staging->unmap();
        }
    }

    // v9a-c-persistent-threads: 4-byte work-queue counter + pre-zeroed staging.
    impl.work_queue_gpu          = ctx.create_buffer(4U, storage | transfer_dst, gpu::ComputeMemory::GpuOnly);
    impl.work_queue_init_staging = ctx.create_buffer(4U, transfer_src, gpu::ComputeMemory::CpuToGpu);
    if (impl.work_queue_init_staging != nullptr)
    {
        if (auto* dst = static_cast<crd::u32*>(impl.work_queue_init_staging->map()))
        {
            *dst = 0U;
            impl.work_queue_init_staging->unmap();
        }
    }

    if (impl.pairs_staging == nullptr || impl.pairs_gpu == nullptr
        || impl.leaf_aabbs_staging == nullptr || impl.leaf_aabbs_gpu == nullptr
        || impl.nodes_gpu == nullptr || impl.prim_indices_staging == nullptr
        || impl.prim_indices_gpu == nullptr
        || impl.leaf_parents_gpu == nullptr
        || impl.done_staging == nullptr || impl.done_gpu == nullptr
        || impl.nodes_readback == nullptr || impl.root_parent_init_staging == nullptr
        || impl.work_queue_gpu == nullptr || impl.work_queue_init_staging == nullptr)
    {
        return;
    }

    impl.valid = true;
}

LbvhGpuPipeline::LbvhGpuPipeline(LbvhGpuPipeline&&) noexcept            = default;
LbvhGpuPipeline& LbvhGpuPipeline::operator=(LbvhGpuPipeline&&) noexcept = default;
LbvhGpuPipeline::~LbvhGpuPipeline()                                      = default;

bool LbvhGpuPipeline::is_valid() const noexcept
{
    return m_impl != nullptr && m_impl->valid;
}

namespace
{

// Shared helper: stage inputs, dispatch build + upsweep, populate `impl.nodes_gpu` (n_int × 64 B) +
// `impl.prim_indices_gpu` (n × 4 B), optionally also issue a readback copy. Returns true on success.
[[nodiscard]] bool
run_build_upsweep(LbvhGpuPipeline::Impl& impl,
                  crd::containers::ConstSpan<MortonPair<crd::u32>> sorted_pairs,
                  crd::containers::ConstSpan<AABB3<crd::f32>>      leaf_aabbs,
                  crd::u32  n,
                  bool      request_nodes_readback,
                  bool      upload_prim_indices_gpu) noexcept
{
    auto& ctx = *impl.ctx;
    const crd::u32 n_int = n - 1U;

    const crd::u64 pairs_bytes        = static_cast<crd::u64>(n)     * sizeof(MortonPair<crd::u32>);
    const crd::u64 aabbs_bytes        = static_cast<crd::u64>(n)     * (6U * sizeof(crd::f32));
    const crd::u64 nodes_bytes        = static_cast<crd::u64>(n_int) * 64U;
    const crd::u64 done_bytes         = static_cast<crd::u64>(n_int) * sizeof(crd::u32);
    const crd::u64 prim_indices_bytes = static_cast<crd::u64>(n)     * sizeof(crd::u32);

    auto& pairs_staging         = *impl.pairs_staging;
    auto& pairs_gpu             = *impl.pairs_gpu;
    auto& leaf_aabbs_staging    = *impl.leaf_aabbs_staging;
    auto& leaf_aabbs_gpu        = *impl.leaf_aabbs_gpu;
    auto& nodes_gpu             = *impl.nodes_gpu;
    auto& prim_indices_staging  = *impl.prim_indices_staging;
    auto& prim_indices_gpu      = *impl.prim_indices_gpu;
    auto& leaf_parents_gpu      = *impl.leaf_parents_gpu;
    auto& done_staging          = *impl.done_staging;
    auto& done_gpu              = *impl.done_gpu;
    auto& nodes_readback        = *impl.nodes_readback;
    auto& root_init_staging     = *impl.root_parent_init_staging;

    // ---- Stage inputs ------------------------------------------------------
    if (auto* dst = static_cast<MortonPair<crd::u32>*>(pairs_staging.map()))
    {
        std::memcpy(dst, sorted_pairs.data(), pairs_bytes);
        pairs_staging.unmap();
    }
    else { return false; }

    // Upload leaf AABBs in ORIGINAL prim-index order (NOT sorted order). The upsweep kernel reads
    // `in_leaf_aabbs.aabbs[prim_idx * 6]` where prim_idx comes from `in_sorted_pairs.pairs[2*k + 1]`.
    if (auto* dst = static_cast<crd::f32*>(leaf_aabbs_staging.map()))
    {
        for (crd::u32 k = 0U; k < n; ++k)
        {
            const AABB3<crd::f32>& aabb = leaf_aabbs[k];
            crd::f32* slot = dst + static_cast<crd::usize>(k) * 6U;
            slot[0] = aabb.min.x; slot[1] = aabb.min.y; slot[2] = aabb.min.z;
            slot[3] = aabb.max.x; slot[4] = aabb.max.y; slot[5] = aabb.max.z;
        }
        leaf_aabbs_staging.unmap();
    }
    else { return false; }

    // done_staging is pre-zeroed in the ctor (kept zero across calls); no per-call CPU memcpy needed.

    // Stage prim_indices to GPU only if the caller needs them on GPU (GPU-resident path).
    if (upload_prim_indices_gpu)
    {
        if (auto* dst = static_cast<crd::u32*>(prim_indices_staging.map()))
        {
            for (crd::u32 k = 0U; k < n; ++k) { dst[k] = sorted_pairs[k].index; }
            prim_indices_staging.unmap();
        }
        else { return false; }
    }

    // Upsweep strategy: carry-register single-dispatch always on this hardware. Tested level-by-level (init_leaves +
    // ~32 merge_level dispatches) at 1 M / RTX 4070 Ti SUPER win-shipping (v9a-c-perf-tune 2026-05-18): 7.7-8.5 ms vs
    // carry-register's 7.4 ms — barrier overhead (~70 μs × 32 barriers ≈ 2.2 ms) eats the algorithmic gain. The level-
    // wise pipelines remain compiled but unused; threshold = u32 max so the condition always selects carry-register.
    constexpr crd::u32 level_wise_threshold = ~0U;
    const bool use_level_wise = (n >= level_wise_threshold);

    // ---- Record + submit ---------------------------------------------------
    auto& rec = ctx.begin();
    rec.copy(pairs_staging,        pairs_gpu,        0U, 0U, pairs_bytes);
    rec.copy(leaf_aabbs_staging,   leaf_aabbs_gpu,   0U, 0U, aabbs_bytes);
    rec.copy(done_staging,         done_gpu,         0U, 0U, done_bytes);
    if (upload_prim_indices_gpu)
    {
        rec.copy(prim_indices_staging, prim_indices_gpu, 0U, 0U, prim_indices_bytes);
    }
    // Seed nodes[0].parent_idx (dword 0 = byte offset 0) with 0xFFFFFFFF — the root sentinel. The build kernel writes
    // every NON-root parent_idx; the root has no parent thread.
    rec.copy(root_init_staging, nodes_gpu, 0U, 0U, 4U);

    rec.barrier(pairs_gpu,        gpu::ComputeAccess::TransferDst, gpu::ComputeAccess::ShaderRead);
    rec.barrier(leaf_aabbs_gpu,   gpu::ComputeAccess::TransferDst, gpu::ComputeAccess::ShaderRead);
    rec.barrier(done_gpu,         gpu::ComputeAccess::TransferDst, gpu::ComputeAccess::ShaderWrite);
    rec.barrier(nodes_gpu,        gpu::ComputeAccess::TransferDst, gpu::ComputeAccess::ShaderWrite);
    // prim_indices_gpu doesn't need a barrier — no shader reads/writes it.

    // Build kernel — N-1 internal-node threads.
    {
        BuildPushConstants pc{};
        pc.n = n;
        gpu::ComputeBuffer* binds[] = {&pairs_gpu, &nodes_gpu, &leaf_parents_gpu};
        const crd::u32 groups = (n_int + 255U) / 256U;
        rec.dispatch(*impl.build, crd::containers::ConstSpan<gpu::ComputeBuffer*>(binds, 3), &pc, sizeof(pc),
                     std::max<crd::u32>(groups, 1U), 1U, 1U);
    }
    rec.barrier(nodes_gpu,        gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::ShaderRead);
    rec.barrier(leaf_parents_gpu, gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::ShaderRead);

    if (use_level_wise)
    {
        // Level-by-level upsweep (v9a-c-perf-tune 2026-05-18). Step A: init_leaves (N threads, scatter + atomicAdd done).
        // Step B: merge_level dispatched K times, each advancing one level toward the root, with a barrier between.
        crd::u32 log2_n = 0U;
        for (crd::u32 v = n; v > 1U; v >>= 1U) { ++log2_n; }
        const crd::u32 max_iters = std::min<crd::u32>(32U, log2_n + 8U);

        // Step A — init_leaves.
        {
            UpsweepPushConstants pc{};
            pc.n = n;
            gpu::ComputeBuffer* binds[] = {&nodes_gpu, &leaf_parents_gpu, &done_gpu, &pairs_gpu, &leaf_aabbs_gpu};
            const crd::u32 groups = (n + 63U) / 64U;
            rec.dispatch(*impl.init_leaves, crd::containers::ConstSpan<gpu::ComputeBuffer*>(binds, 5), &pc, sizeof(pc), groups, 1U, 1U);
        }
        rec.barrier(nodes_gpu, gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::ShaderRead);
        rec.barrier(done_gpu,  gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::ShaderRead);

        // Step B — merge_level loop.
        MergeLevelPushConstants pc{};
        pc.n_int = n_int;
        gpu::ComputeBuffer* binds[] = {&nodes_gpu, &done_gpu};
        const crd::u32 merge_groups = (n_int + 63U) / 64U;
        for (crd::u32 iter = 0U; iter < max_iters; ++iter)
        {
            rec.dispatch(*impl.merge_level, crd::containers::ConstSpan<gpu::ComputeBuffer*>(binds, 2), &pc, sizeof(pc), merge_groups, 1U, 1U);
            // Single COMPUTE→COMPUTE SHADER_WRITE→SHADER_READ barrier per iteration (device-wide visibility).
            rec.barrier(nodes_gpu, gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::ShaderRead);
        }
    }
    else
    {
        // Carry-register upsweep — N leaf-walker threads, single dispatch. Handles arbitrary depth.
        UpsweepPushConstants pc{};
        pc.n = n;
        gpu::ComputeBuffer* binds[] = {&nodes_gpu, &leaf_parents_gpu, &done_gpu, &pairs_gpu, &leaf_aabbs_gpu};
        const crd::u32 groups = (n + 63U) / 64U;
        rec.dispatch(*impl.upsweep, crd::containers::ConstSpan<gpu::ComputeBuffer*>(binds, 5), &pc, sizeof(pc), groups, 1U, 1U);
    }

    if (request_nodes_readback)
    {
        rec.barrier(nodes_gpu, gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::TransferSrc);
        rec.copy(nodes_gpu, nodes_readback, 0U, 0U, nodes_bytes);
    }
    else
    {
        // GPU-resident output: leave nodes_gpu readable for subsequent GPU ops.
        rec.barrier(nodes_gpu, gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::ShaderRead);
    }

    ctx.submit_and_wait();
    return true;
}

} // namespace

LbvhTree
LbvhGpuPipeline::dispatch_build_lbvh(
    crd::containers::ConstSpan<MortonPair<crd::u32>>   sorted_pairs,
    crd::containers::ConstSpan<AABB3<crd::f32>>        leaf_aabbs,
    crd::memory::IAllocator*                           alloc) noexcept
{
    LbvhTree tree(alloc);
    if (!is_valid()) { return tree; }

    const crd::usize n_usize = sorted_pairs.size();
    if (n_usize == 0U) { return tree; }

    const crd::u32 n = static_cast<crd::u32>(n_usize);

    // prim_indices: derived on CPU from sorted_pairs (zero GPU readback cost).
    {
        auto& prim_out = tree.prim_indices_mut();
        prim_out.resize(n);
        for (crd::u32 k = 0U; k < n; ++k)
        {
            prim_out[k] = sorted_pairs[k].index;
        }
    }

    // ---- N=1 singleton: no internal nodes; consumer uses prim_indices[0] --
    if (n == 1U)
    {
        tree.set_root(0U);
        return tree;
    }

    auto& impl = *m_impl;
    if (!run_build_upsweep(impl, sorted_pairs, leaf_aabbs, n,
                           /*request_nodes_readback*/      true,
                           /*upload_prim_indices_gpu*/     false))
    {
        return tree;
    }

    // ---- Read back final fat-node array ------------------------------------
    const crd::u32 n_int = n - 1U;
    const crd::u64 nodes_bytes = static_cast<crd::u64>(n_int) * 64U;
    auto& nodes_out = tree.nodes_mut();
    nodes_out.resize(n_int);
    if (auto* src = impl.nodes_readback->map())
    {
        std::memcpy(nodes_out.data(), src, nodes_bytes);
        impl.nodes_readback->unmap();
    }

    tree.set_root(0U);
    return tree;
}

LbvhGpuPipeline::GpuResidentTree
LbvhGpuPipeline::dispatch_build_lbvh_gpu_resident(
    crd::containers::ConstSpan<MortonPair<crd::u32>>   sorted_pairs,
    crd::containers::ConstSpan<AABB3<crd::f32>>        leaf_aabbs) noexcept
{
    GpuResidentTree out{};
    if (!is_valid()) { return out; }

    const crd::usize n_usize = sorted_pairs.size();
    if (n_usize == 0U) { return out; }

    const crd::u32 n = static_cast<crd::u32>(n_usize);
    auto& impl = *m_impl;

    out.prim_count             = n;
    out.prim_indices           = impl.prim_indices_gpu.get();
    out.prim_indices_byte_size = static_cast<crd::u64>(n) * sizeof(crd::u32);

    if (n == 1U)
    {
        // Singleton: prim_indices buffer holds the one prim index; no nodes. Stage it in this call.
        auto& ctx = *impl.ctx;
        if (auto* dst = static_cast<crd::u32*>(impl.prim_indices_staging->map()))
        {
            dst[0] = sorted_pairs[0].index;
            impl.prim_indices_staging->unmap();
        }
        auto& rec = ctx.begin();
        rec.copy(*impl.prim_indices_staging, *impl.prim_indices_gpu, 0U, 0U, sizeof(crd::u32));
        ctx.submit_and_wait();

        out.nodes           = nullptr;
        out.internal_count  = 0U;
        out.nodes_byte_size = 0U;
        return out;
    }

    if (!run_build_upsweep(impl, sorted_pairs, leaf_aabbs, n,
                           /*request_nodes_readback*/  false,
                           /*upload_prim_indices_gpu*/ true))
    {
        return GpuResidentTree{};
    }

    const crd::u32 n_int = n - 1U;
    out.nodes           = impl.nodes_gpu.get();
    out.internal_count  = n_int;
    out.nodes_byte_size = static_cast<crd::u64>(n_int) * 64U;
    return out;
}

// ---------------------------------------------------------------------------
// v9a-c-gpu-inputs: build LBVH from GPU-resident inputs (no CPU staging).
// ---------------------------------------------------------------------------
LbvhGpuPipeline::GpuResidentTree
LbvhGpuPipeline::dispatch_build_lbvh_from_gpu(const GpuInputView& inputs) noexcept
{
    GpuResidentTree out{};
    if (!is_valid()) { return out; }
    if (inputs.sorted_pairs == nullptr || inputs.leaf_aabbs == nullptr) { return out; }
    if (inputs.n == 0U) { return out; }

    const crd::u32 n = inputs.n;
    auto& impl = *m_impl;
    auto& ctx  = *impl.ctx;

    out.prim_count             = n;
    out.prim_indices           = impl.prim_indices_gpu.get();
    out.prim_indices_byte_size = static_cast<crd::u64>(n) * sizeof(crd::u32);

    // ---- N=1 singleton: no internal nodes; just copy prim_indices[0] ------
    if (n == 1U)
    {
        // Extract prim_indices[0] from inputs.sorted_pairs[1].
        auto&               rec  = ctx.begin();
        gpu::ComputeBuffer* binds[] = {inputs.sorted_pairs, impl.prim_indices_gpu.get()};
        UpsweepPushConstants pc{};
        pc.n = 1U;
        rec.dispatch(*impl.extract_prim_indices, crd::containers::ConstSpan<gpu::ComputeBuffer*>(binds, 2), &pc, sizeof(pc), 1U, 1U, 1U);
        ctx.submit_and_wait();

        out.nodes           = nullptr;
        out.internal_count  = 0U;
        out.nodes_byte_size = 0U;
        return out;
    }

    const crd::u32 n_int = n - 1U;

    auto& sorted_pairs_gpu = *inputs.sorted_pairs;
    auto& leaf_aabbs_gpu   = *inputs.leaf_aabbs;
    auto& nodes_gpu        = *impl.nodes_gpu;
    auto& prim_indices_gpu = *impl.prim_indices_gpu;
    auto& leaf_parents_gpu = *impl.leaf_parents_gpu;
    auto& done_staging     = *impl.done_staging;
    auto& done_gpu         = *impl.done_gpu;
    auto& root_init        = *impl.root_parent_init_staging;

    const crd::u64 done_bytes = static_cast<crd::u64>(n_int) * sizeof(crd::u32);

    // ---- Record + submit ---------------------------------------------------
    auto& rec = ctx.begin();

    // Init done_gpu (zeros, from pre-zeroed staging) + nodes[0].parent_idx (sentinel). The only GPU transfers.
    rec.copy(done_staging, done_gpu,  0U, 0U, done_bytes);
    rec.copy(root_init,    nodes_gpu, 0U, 0U, 4U);

    rec.barrier(done_gpu,  gpu::ComputeAccess::TransferDst, gpu::ComputeAccess::ShaderWrite);
    rec.barrier(nodes_gpu, gpu::ComputeAccess::TransferDst, gpu::ComputeAccess::ShaderWrite);
    // sorted_pairs_gpu + leaf_aabbs_gpu are assumed in ShaderRead state already (caller's contract).

    // Build kernel.
    {
        BuildPushConstants pc{};
        pc.n = n;
        gpu::ComputeBuffer* binds[] = {&sorted_pairs_gpu, &nodes_gpu, &leaf_parents_gpu};
        const crd::u32 groups = (n_int + 255U) / 256U;
        rec.dispatch(*impl.build, crd::containers::ConstSpan<gpu::ComputeBuffer*>(binds, 3), &pc, sizeof(pc), std::max<crd::u32>(groups, 1U), 1U, 1U);
    }
    rec.barrier(nodes_gpu,        gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::ShaderRead);
    rec.barrier(leaf_parents_gpu, gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::ShaderRead);

    // Regular flat-dispatch carry-register upsweep.
    {
        UpsweepPushConstants pc{};
        pc.n = n;
        gpu::ComputeBuffer* binds[] = {&nodes_gpu, &leaf_parents_gpu, &done_gpu, &sorted_pairs_gpu, &leaf_aabbs_gpu};
        const crd::u32 groups = (n + 63U) / 64U;
        rec.dispatch(*impl.upsweep, crd::containers::ConstSpan<gpu::ComputeBuffer*>(binds, 5), &pc, sizeof(pc), groups, 1U, 1U);
    }
    rec.barrier(nodes_gpu, gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::ShaderRead);

    // Extract prim_indices on GPU (avoids CPU memcpy + transfer of 4 MB).
    {
        UpsweepPushConstants pc{};
        pc.n = n;
        gpu::ComputeBuffer* binds[] = {&sorted_pairs_gpu, &prim_indices_gpu};
        const crd::u32 groups = (n + 63U) / 64U;
        rec.dispatch(*impl.extract_prim_indices, crd::containers::ConstSpan<gpu::ComputeBuffer*>(binds, 2), &pc, sizeof(pc), groups, 1U, 1U);
    }
    rec.barrier(prim_indices_gpu, gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::ShaderRead);

    ctx.submit_and_wait();

    out.nodes           = impl.nodes_gpu.get();
    out.internal_count  = n_int;
    out.nodes_byte_size = static_cast<crd::u64>(n_int) * 64U;
    return out;
}

// ---------------------------------------------------------------------------
// v9b: GPU BVH refit — recompute internal-node bounds with new leaf AABBs,
// topology untouched. Pure upsweep over the existing tree.
// ---------------------------------------------------------------------------
LbvhGpuPipeline::GpuResidentTree
LbvhGpuPipeline::dispatch_refit_lbvh(const RefitInputs& inputs) noexcept
{
    GpuResidentTree out{};
    if (!is_valid()) { return out; }
    if (inputs.sorted_pairs == nullptr || inputs.leaf_aabbs == nullptr) { return out; }
    if (inputs.n == 0U) { return out; }

    const crd::u32 n = inputs.n;
    auto& impl = *m_impl;
    auto& ctx  = *impl.ctx;

    out.prim_count             = n;
    out.prim_indices           = impl.prim_indices_gpu.get();
    out.prim_indices_byte_size = static_cast<crd::u64>(n) * sizeof(crd::u32);

    // ---- N=1 singleton: nothing to refit -----------------------------------
    if (n == 1U)
    {
        out.nodes           = nullptr;
        out.internal_count  = 0U;
        out.nodes_byte_size = 0U;
        return out;
    }

    const crd::u32 n_int = n - 1U;
    const crd::u64 done_bytes = static_cast<crd::u64>(n_int) * sizeof(crd::u32);

    auto& sorted_pairs_gpu = *inputs.sorted_pairs;
    auto& leaf_aabbs_gpu   = *inputs.leaf_aabbs;
    auto& nodes_gpu        = *impl.nodes_gpu;
    auto& leaf_parents_gpu = *impl.leaf_parents_gpu;
    auto& done_staging     = *impl.done_staging;
    auto& done_gpu         = *impl.done_gpu;

    // ---- Record + submit ---------------------------------------------------
    auto& rec = ctx.begin();

    // Reset done_gpu to zero (from pre-zeroed staging). nodes_gpu topology is preserved from the build; only its bounds
    // slots will be overwritten by the upsweep's `store_slot` calls.
    rec.copy(done_staging, done_gpu, 0U, 0U, done_bytes);

    rec.barrier(done_gpu,  gpu::ComputeAccess::TransferDst, gpu::ComputeAccess::ShaderWrite);
    // nodes_gpu: prior call left it in ShaderRead (or TransferSrc if readback'd). Need ShaderWrite for the new bounds.
    rec.barrier(nodes_gpu, gpu::ComputeAccess::ShaderRead, gpu::ComputeAccess::ShaderWrite);

    // Upsweep kernel — carry-register walks with NEW leaf AABBs. Topology untouched.
    {
        UpsweepPushConstants pc{};
        pc.n = n;
        gpu::ComputeBuffer* binds[] = {&nodes_gpu, &leaf_parents_gpu, &done_gpu, &sorted_pairs_gpu, &leaf_aabbs_gpu};
        const crd::u32 groups = (n + 63U) / 64U;
        rec.dispatch(*impl.upsweep, crd::containers::ConstSpan<gpu::ComputeBuffer*>(binds, 5), &pc, sizeof(pc), groups, 1U, 1U);
    }
    rec.barrier(nodes_gpu, gpu::ComputeAccess::ShaderWrite, gpu::ComputeAccess::ShaderRead);

    ctx.submit_and_wait();

    out.nodes           = impl.nodes_gpu.get();
    out.internal_count  = n_int;
    out.nodes_byte_size = static_cast<crd::u64>(n_int) * 64U;
    return out;
}

} // namespace crd::geometry::bvh_gpu
