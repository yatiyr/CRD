// ---------------------------------------------------------------------------
// LbvhGpuPipeline — GPU-side fat-node LBVH (Karras 2012 + KittenGpuLBVH-style
// 64 B node layout). Phase 3.1.7 v9a-c-followon "Track A elite rewrite"
// (2026-05-18).
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
// fat-node layout. For 1 M: ~64 MB nodes readback (down from ~76 MB in the
// compact-node version).
//
// `prim_indices` is derived on CPU from `sorted_pairs` (already in caller's
// memory — zero readback cost). The upsweep kernel reads leaf AABBs by
// ORIGINAL primitive index (NOT sorted order); the staging buffer mirrors
// the caller's `leaf_aabbs` argument verbatim.
//
// **All working-set buffers cached in the pipeline ctor** at kRadixMaxItems
// capacity. Per-call buffer-create cost: 0 ms.
//
// Expected pure-GPU compute on RTX 4070 Ti SUPER for 1 M:
//   - Build kernel:  ~1-2 ms (N-1 threads, Karras logic)
//   - Upsweep kernel: ~1-3 ms (carry-register walk; coherent buffer)
//   - Total compute: ~2-5 ms (matches/beats KittenGpuLBVH's 1.5 ms on RTX
//     3090, scaled to our hardware).
//
// Synchronous dispatch (matches v9a-b2 pattern). Async-compute follow-on
// when a consumer needs overlap.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh_gpu/lbvh.hpp>

#include <crd/containers/string.hpp>
#include <crd/core/assert.hpp>
#include <crd/geometry/bvh_gpu/radix_sort.hpp>   // kRadixMaxItems for pre-allocation
#include <crd/platform/filesystem.hpp>
#include <crd/rhi/buffer.hpp>
#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/compute_pipeline.hpp>
#include <crd/rhi/descriptor.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/fence.hpp>
#include <crd/rhi/pipeline.hpp>
#include <crd/rhi/queue.hpp>
#include <crd/rhi/shader_module.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace crd::geometry::bvh_gpu
{

namespace
{

using crd::geometry::primitives::AABB3;

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

struct PipelineQuad
{
    std::unique_ptr<crd::rhi::ShaderModule>        shader{};
    std::unique_ptr<crd::rhi::DescriptorSetLayout> set_layout{};
    std::unique_ptr<crd::rhi::PipelineLayout>      pipeline_layout{};
    std::unique_ptr<crd::rhi::ComputePipeline>     pipeline{};
};

[[nodiscard]] bool
build_pipeline(crd::rhi::Device& device, const crd::platform::fs::Path& spv_path,
               crd::containers::ConstSpan<crd::rhi::DescriptorBinding> bindings,
               std::uint32_t push_constants_size, PipelineQuad& out) noexcept
{
    crd::containers::Array<crd::u8> spv;
    if (!crd::platform::fs::read_file_binary(spv_path, spv)) { return false; }

    out.shader = device.create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main",
         crd::containers::ConstSpan<crd::u8>(spv.data(), spv.size())});
    if (out.shader == nullptr) { return false; }

    crd::rhi::DescriptorSetLayoutDesc set_desc{};
    set_desc.bindings = bindings;
    out.set_layout = device.create_descriptor_set_layout(set_desc);
    if (out.set_layout == nullptr) { return false; }

    const crd::rhi::DescriptorSetLayout* layouts[] = {out.set_layout.get()};
    crd::rhi::PushConstantRange pc_range{};
    pc_range.stages = crd::rhi::ShaderStage::Compute;
    pc_range.offset = 0U;
    pc_range.size   = push_constants_size;
    crd::rhi::PipelineLayoutDesc layout_desc{};
    layout_desc.set_layouts =
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(layouts, 1);
    layout_desc.push_constant_ranges =
        crd::containers::ConstSpan<crd::rhi::PushConstantRange>(&pc_range, 1);
    out.pipeline_layout = device.create_pipeline_layout(layout_desc);
    if (out.pipeline_layout == nullptr) { return false; }

    crd::rhi::ComputePipelineDesc pipe_desc{};
    pipe_desc.compute_shader  = out.shader.get();
    pipe_desc.pipeline_layout = out.pipeline_layout.get();
    out.pipeline = device.create_compute_pipeline(pipe_desc);
    return out.pipeline != nullptr;
}

} // namespace

struct LbvhGpuPipeline::Impl
{
    crd::rhi::Device* device = nullptr;
    PipelineQuad      build{};
    PipelineQuad      upsweep{};        // carry-register (fallback for small N + degenerate all-equal codes)
    PipelineQuad      init_leaves{};    // level-by-level pass 1 (leaf scatter)
    PipelineQuad      merge_level{};    // level-by-level pass 2..K (internal merge)
    PipelineQuad      extract_prim_indices{};  // v9a-c-gpu-inputs: GPU-side prim_indices extraction
    PipelineQuad      upsweep_persistent{};    // v9a-c-persistent-threads: atomic-queue upsweep
    std::unique_ptr<crd::rhi::DescriptorAllocator> desc_alloc{};

    // All working-set buffers pre-allocated at kRadixMaxItems capacity in
    // the ctor. Reused across dispatch_build_lbvh calls — avoids the per-
    // call cost of `device.create_buffer()` allocations. Per-call we just
    // memcpy into the staging slots and dispatch.
    std::unique_ptr<crd::rhi::Buffer> pairs_staging{};
    std::unique_ptr<crd::rhi::Buffer> pairs_gpu{};
    std::unique_ptr<crd::rhi::Buffer> leaf_aabbs_staging{};
    std::unique_ptr<crd::rhi::Buffer> leaf_aabbs_gpu{};
    std::unique_ptr<crd::rhi::Buffer> nodes_gpu{};               // 64 B per LbvhFatNode
    std::unique_ptr<crd::rhi::Buffer> prim_indices_gpu{};        // 4 B per leaf (sorted-order original prim index)
    std::unique_ptr<crd::rhi::Buffer> prim_indices_staging{};    // CPU-staged source of the GPU prim_indices
    std::unique_ptr<crd::rhi::Buffer> leaf_parents_gpu{};
    std::unique_ptr<crd::rhi::Buffer> done_staging{};
    std::unique_ptr<crd::rhi::Buffer> done_gpu{};
    std::unique_ptr<crd::rhi::Buffer> nodes_readback{};
    // 4-byte staging holding 0xFFFFFFFF — copied to nodes_gpu offset 0 each
    // call (= nodes[0].parent_idx, the root sentinel). The build kernel
    // writes every NON-root parent_idx; the root has no parent thread.
    std::unique_ptr<crd::rhi::Buffer> root_parent_init_staging{};

    // v9a-c-persistent-threads: 4-byte atomic counter for the persistent-
    // threads work queue. work_queue_init is a pre-zeroed staging buffer
    // (same pattern as root_init); the dispatch copies it to work_queue at
    // call start.
    std::unique_ptr<crd::rhi::Buffer> work_queue_gpu{};
    std::unique_ptr<crd::rhi::Buffer> work_queue_init_staging{};

    // v9a-c-cmd-cache: pre-allocated cmd buffer + fence, reused across calls.
    // Each call resets and re-records the cmd buffer; saves the per-call
    // create_command_buffer + create_fence cost (~0.1-0.2 ms / 1 M).
    std::unique_ptr<crd::rhi::CommandBuffer> cached_cmd{};
    std::unique_ptr<crd::rhi::Fence>         cached_fence{};

    bool valid = false;
};

LbvhGpuPipeline::LbvhGpuPipeline(crd::rhi::Device&            device,
                                 crd::containers::StringView shader_dir) noexcept
    : m_impl(std::make_unique<Impl>())
{
    auto& impl  = *m_impl;
    impl.device = &device;

    const crd::platform::fs::Path base{shader_dir};
    auto path_for = [&](const char* name) {
        return base / crd::containers::StringView{name};
    };

    // Build kernel: in_pairs(0) + out_nodes(1) + out_leaf_parents(2).
    crd::rhi::DescriptorBinding build_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 2, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };

    // Upsweep kernel: nodes(0) + leaf_parents(1) + done(2) + pairs(3) + leaf_aabbs(4).
    crd::rhi::DescriptorBinding upsweep_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 2, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 3, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 4, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };

    if (!build_pipeline(device, path_for("lbvh_fat_build.comp.spv"),
                        crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(build_bindings, 3),
                        sizeof(BuildPushConstants), impl.build))
    { return; }

    if (!build_pipeline(device, path_for("lbvh_fat_upsweep.comp.spv"),
                        crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(upsweep_bindings, 5),
                        sizeof(UpsweepPushConstants), impl.upsweep))
    { return; }

    // init_leaves uses the same 5-binding layout as upsweep.
    if (!build_pipeline(device, path_for("lbvh_fat_init_leaves.comp.spv"),
                        crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(upsweep_bindings, 5),
                        sizeof(UpsweepPushConstants), impl.init_leaves))
    { return; }

    // merge_level: just nodes(0) + done(1).
    crd::rhi::DescriptorBinding merge_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    if (!build_pipeline(device, path_for("lbvh_fat_merge_level.comp.spv"),
                        crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(merge_bindings, 2),
                        sizeof(MergeLevelPushConstants), impl.merge_level))
    { return; }

    // extract_prim_indices: in_pairs(0) + out_prim_indices(1).
    crd::rhi::DescriptorBinding extract_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    if (!build_pipeline(device, path_for("lbvh_fat_extract_prim_indices.comp.spv"),
                        crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(extract_bindings, 2),
                        sizeof(UpsweepPushConstants), impl.extract_prim_indices))
    { return; }

    // upsweep_persistent: 6 storage-buffer bindings (same 5 as regular
    // upsweep + work_queue at binding 5).
    crd::rhi::DescriptorBinding upsweep_persistent_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 2, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 3, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 4, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 5, .type = crd::rhi::DescriptorType::StorageBuffer, .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    if (!build_pipeline(device, path_for("lbvh_fat_upsweep_persistent.comp.spv"),
                        crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(upsweep_persistent_bindings, 6),
                        sizeof(UpsweepPushConstants), impl.upsweep_persistent))
    { return; }

    crd::rhi::DescriptorAllocatorDesc alloc_desc{};
    alloc_desc.frames_in_flight              = 2;
    alloc_desc.max_sets_per_frame            = 8;
    alloc_desc.max_storage_buffers_per_frame = 32;
    impl.desc_alloc = device.create_descriptor_allocator(alloc_desc);
    if (impl.desc_alloc == nullptr) { return; }

    // ---- Pre-allocate all working-set buffers at kRadixMaxItems capacity --
    constexpr crd::u64 kMaxN           = static_cast<crd::u64>(kRadixMaxItems);
    constexpr crd::u64 kMaxNInt        = kMaxN - 1U;
    constexpr crd::u64 kMaxPairsBytes  = kMaxN    * sizeof(MortonPair<crd::u32>);
    constexpr crd::u64 kMaxAabbBytes   = kMaxN    * (6U * sizeof(crd::f32));
    constexpr crd::u64 kMaxNodesBytes  = kMaxNInt * 64U;          // 64 B per LbvhFatNode
    constexpr crd::u64 kMaxParentBytes = kMaxN    * sizeof(crd::u32);
    constexpr crd::u64 kMaxDoneBytes   = kMaxNInt * sizeof(crd::u32);

    impl.pairs_staging = device.create_buffer(
        {kMaxPairsBytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
         crd::rhi::MemoryUsage::CpuToGpu});
    impl.pairs_gpu = device.create_buffer(
        {kMaxPairsBytes,
         crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::GpuOnly});

    impl.leaf_aabbs_staging = device.create_buffer(
        {kMaxAabbBytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
         crd::rhi::MemoryUsage::CpuToGpu});
    impl.leaf_aabbs_gpu = device.create_buffer(
        {kMaxAabbBytes,
         crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::GpuOnly});

    impl.nodes_gpu = device.create_buffer(
        {kMaxNodesBytes,
         crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::GpuOnly});
    impl.prim_indices_staging = device.create_buffer(
        {kMaxN * sizeof(crd::u32), crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
         crd::rhi::MemoryUsage::CpuToGpu});
    impl.prim_indices_gpu = device.create_buffer(
        {kMaxN * sizeof(crd::u32),
         crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
         crd::rhi::MemoryUsage::GpuOnly});
    impl.leaf_parents_gpu = device.create_buffer(
        {kMaxParentBytes,
         crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuOnly});

    impl.done_staging = device.create_buffer(
        {kMaxDoneBytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
         crd::rhi::MemoryUsage::CpuToGpu});
    impl.done_gpu = device.create_buffer(
        {kMaxDoneBytes,
         crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::GpuOnly});

    impl.nodes_readback = device.create_buffer(
        {kMaxNodesBytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::GpuToCpu});

    impl.root_parent_init_staging = device.create_buffer(
        {4U, crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
         crd::rhi::MemoryUsage::CpuToGpu});
    if (impl.root_parent_init_staging != nullptr)
    {
        if (auto* dst = static_cast<crd::u32*>(impl.root_parent_init_staging->map()))
        {
            *dst = 0xFFFFFFFFU;
            impl.root_parent_init_staging->unmap();
        }
    }

    // Pre-zero done_staging ONCE in the ctor. Subsequent per-call memsets
    // were a ~0.5-1 ms waste — staging buffers are host-coherent CpuToGpu
    // memory; the CPU never reads from them. They stay zeroed across calls.
    // Saves CPU memcpy on every dispatch (v9a-c-perf-tune sub-win 2026-05-18).
    if (impl.done_staging != nullptr)
    {
        if (auto* dst = static_cast<crd::u32*>(impl.done_staging->map()))
        {
            constexpr crd::u64 kMaxDoneBytesCtor = kMaxDoneBytes;
            std::memset(dst, 0, kMaxDoneBytesCtor);
            impl.done_staging->unmap();
        }
    }

    // v9a-c-persistent-threads: 4-byte work-queue counter + pre-zeroed staging.
    impl.work_queue_gpu = device.create_buffer(
        {4U,
         crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::GpuOnly});
    impl.work_queue_init_staging = device.create_buffer(
        {4U, crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
         crd::rhi::MemoryUsage::CpuToGpu});
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

    // v9a-c-cmd-cache: pre-allocate cmd buffer + fence. Reused across all
    // dispatch_* calls; reset + re-record per call.
    impl.cached_cmd   = device.create_command_buffer();
    impl.cached_fence = device.create_fence();
    if (impl.cached_cmd == nullptr || impl.cached_fence == nullptr) { return; }

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

// Shared helper: stage inputs, dispatch build + upsweep, populate
// `impl.nodes_gpu` (n_int × 64 B) + `impl.prim_indices_gpu` (n × 4 B),
// optionally also issue a readback copy + wait the fence. Returns true on
// success. Caller is responsible for any post-fence CPU work.
//
// `request_nodes_readback` controls whether the cmd buffer issues the
// nodes_gpu → nodes_readback transfer. If false, the GPU-resident path
// can skip the 64 MB transfer (the big perf win at 1 M).
[[nodiscard]] bool
run_build_upsweep(LbvhGpuPipeline::Impl& impl,
                  crd::containers::ConstSpan<MortonPair<crd::u32>> sorted_pairs,
                  crd::containers::ConstSpan<AABB3<crd::f32>>      leaf_aabbs,
                  crd::u32  n,
                  bool      request_nodes_readback,
                  bool      upload_prim_indices_gpu) noexcept
{
    auto& device = *impl.device;
    const crd::u32 n_int = n - 1U;

    const crd::u64 pairs_bytes        = static_cast<crd::u64>(n)     * sizeof(MortonPair<crd::u32>);
    const crd::u64 aabbs_bytes        = static_cast<crd::u64>(n)     * (6U * sizeof(crd::f32));
    const crd::u64 nodes_bytes        = static_cast<crd::u64>(n_int) * 64U;
    const crd::u64 parent_bytes       = static_cast<crd::u64>(n)     * sizeof(crd::u32);
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

    // Upload leaf AABBs in ORIGINAL prim-index order (NOT sorted order). The
    // upsweep kernel reads `in_leaf_aabbs.aabbs[prim_idx * 6]` where prim_idx
    // comes from `in_sorted_pairs.pairs[2*k + 1]`.
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

    // done_staging is pre-zeroed in the ctor (kept zero across calls); no
    // per-call CPU memcpy needed.

    // Stage prim_indices to GPU only if the caller needs them on GPU (i.e.
    // GPU-resident path). The CPU-output path builds prim_indices directly
    // into LbvhTree.prim_indices_mut() and never reads from prim_indices_gpu.
    // Skipping this saves ~1 M × 4 B = 4 MB of CPU memcpy + GPU transfer per
    // call (~0.3-0.5 ms at 1 M on RTX 4070 Ti SUPER).
    if (upload_prim_indices_gpu)
    {
        if (auto* dst = static_cast<crd::u32*>(prim_indices_staging.map()))
        {
            for (crd::u32 k = 0U; k < n; ++k) { dst[k] = sorted_pairs[k].index; }
            prim_indices_staging.unmap();
        }
        else { return false; }
    }

    // ---- Descriptor sets ---------------------------------------------------
    impl.desc_alloc->begin_frame(0U);

    auto ds_build = impl.desc_alloc->allocate(*impl.build.set_layout);
    if (ds_build == nullptr) { return false; }
    ds_build->update_buffer(0U, pairs_gpu,        0U, pairs_bytes);
    ds_build->update_buffer(1U, nodes_gpu,        0U, nodes_bytes);
    ds_build->update_buffer(2U, leaf_parents_gpu, 0U, parent_bytes);

    // Upsweep strategy: carry-register single-dispatch always on this
    // hardware. Tested level-by-level (init_leaves + ~32 merge_level
    // dispatches) at 1 M / RTX 4070 Ti SUPER win-shipping (v9a-c-perf-tune
    // 2026-05-18): 7.7-8.5 ms vs carry-register's 7.4 ms — barrier overhead
    // (~70 μs × 32 barriers ≈ 2.2 ms) eats the algorithmic gain. The level-
    // wise pipelines remain compiled but unused; threshold = u32 max so the
    // condition always selects carry-register. Future: re-enable when (a)
    // RHI grows a batched buffer_barrier API, or (b) consumer hardware has
    // cheaper pipeline barriers.
    constexpr crd::u32 kLevelWiseThreshold = ~0U;
    const bool use_level_wise = (n >= kLevelWiseThreshold);

    std::unique_ptr<crd::rhi::DescriptorSet> ds_upsweep;
    std::unique_ptr<crd::rhi::DescriptorSet> ds_init_leaves;
    std::unique_ptr<crd::rhi::DescriptorSet> ds_merge_level;

    if (use_level_wise)
    {
        ds_init_leaves = impl.desc_alloc->allocate(*impl.init_leaves.set_layout);
        if (ds_init_leaves == nullptr) { return false; }
        ds_init_leaves->update_buffer(0U, nodes_gpu,        0U, nodes_bytes);
        ds_init_leaves->update_buffer(1U, leaf_parents_gpu, 0U, parent_bytes);
        ds_init_leaves->update_buffer(2U, done_gpu,         0U, done_bytes);
        ds_init_leaves->update_buffer(3U, pairs_gpu,        0U, pairs_bytes);
        ds_init_leaves->update_buffer(4U, leaf_aabbs_gpu,   0U, aabbs_bytes);

        ds_merge_level = impl.desc_alloc->allocate(*impl.merge_level.set_layout);
        if (ds_merge_level == nullptr) { return false; }
        ds_merge_level->update_buffer(0U, nodes_gpu, 0U, nodes_bytes);
        ds_merge_level->update_buffer(1U, done_gpu,  0U, done_bytes);
    }
    else
    {
        ds_upsweep = impl.desc_alloc->allocate(*impl.upsweep.set_layout);
        if (ds_upsweep == nullptr) { return false; }
        ds_upsweep->update_buffer(0U, nodes_gpu,        0U, nodes_bytes);
        ds_upsweep->update_buffer(1U, leaf_parents_gpu, 0U, parent_bytes);
        ds_upsweep->update_buffer(2U, done_gpu,         0U, done_bytes);
        ds_upsweep->update_buffer(3U, pairs_gpu,        0U, pairs_bytes);
        ds_upsweep->update_buffer(4U, leaf_aabbs_gpu,   0U, aabbs_bytes);
    }

    // ---- Record + submit ---------------------------------------------------
    // v9a-c-cmd-cache: reuse the pipeline-cached cmd buffer + fence.
    auto* cmd   = impl.cached_cmd.get();
    auto* fence = impl.cached_fence.get();
    cmd->reset();
    fence->reset();

    cmd->begin();
    cmd->copy_buffer(pairs_staging,        pairs_gpu,        0U, 0U, pairs_bytes);
    cmd->copy_buffer(leaf_aabbs_staging,   leaf_aabbs_gpu,   0U, 0U, aabbs_bytes);
    cmd->copy_buffer(done_staging,         done_gpu,         0U, 0U, done_bytes);
    if (upload_prim_indices_gpu)
    {
        cmd->copy_buffer(prim_indices_staging, prim_indices_gpu, 0U, 0U, prim_indices_bytes);
    }
    // Seed nodes[0].parent_idx (dword 0 = byte offset 0) with 0xFFFFFFFF —
    // the root sentinel. The build kernel writes every NON-root parent_idx;
    // the root has no parent thread.
    cmd->copy_buffer(root_init_staging, nodes_gpu, 0U, 0U, 4U);

    cmd->buffer_barrier(pairs_gpu,        crd::rhi::BufferAccess::TransferDst,
                                            crd::rhi::BufferAccess::ComputeShaderRead);
    cmd->buffer_barrier(leaf_aabbs_gpu,   crd::rhi::BufferAccess::TransferDst,
                                            crd::rhi::BufferAccess::ComputeShaderRead);
    cmd->buffer_barrier(done_gpu,         crd::rhi::BufferAccess::TransferDst,
                                            crd::rhi::BufferAccess::ComputeShaderWrite);
    cmd->buffer_barrier(nodes_gpu,        crd::rhi::BufferAccess::TransferDst,
                                            crd::rhi::BufferAccess::ComputeShaderWrite);
    // prim_indices_gpu doesn't need a barrier — no shader reads/writes it.

    // Build kernel — N-1 internal-node threads.
    {
        cmd->bind_compute_pipeline(*impl.build.pipeline);
        crd::rhi::DescriptorSet* sets[] = {ds_build.get()};
        cmd->bind_compute_descriptor_sets(*impl.build.pipeline_layout, 0U,
            crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
        BuildPushConstants pc{};
        pc.n = n;
        cmd->push_constants(*impl.build.pipeline_layout, crd::rhi::ShaderStage::Compute,
                            0U, sizeof(pc), &pc);
        const crd::u32 groups = (n_int + 255U) / 256U;
        cmd->dispatch(std::max<crd::u32>(groups, 1U), 1U, 1U);
    }
    cmd->buffer_barrier(nodes_gpu,        crd::rhi::BufferAccess::ComputeShaderWrite,
                                            crd::rhi::BufferAccess::ComputeShaderRead);
    cmd->buffer_barrier(leaf_parents_gpu, crd::rhi::BufferAccess::ComputeShaderWrite,
                                            crd::rhi::BufferAccess::ComputeShaderRead);

    if (use_level_wise)
    {
        // Level-by-level upsweep (v9a-c-perf-tune 2026-05-18):
        //
        //   Step A: init_leaves dispatch — N threads. Each leaf scatters its
        //           AABB into parent.bounds[isRight], atomicAdd done[parent].
        //           After this dispatch, internal nodes whose BOTH children
        //           are leaves have done == 2 (ready); rest are in {0, 1}.
        //
        //   Step B: merge_level dispatched K times. Each thread for internal
        //           node i checks done[i] == 2 (ready+not-yet-processed),
        //           atomicCompSwap (2 → 3), merges bounds[0]∪bounds[1],
        //           scatters to parent.bounds[isRight], atomicAdd done[parent].
        //           Each dispatch advances at most one level toward the root.
        //
        // Iteration bound: ceil(log₂(N)) + slack covers all non-pathological
        // scenes. Tree-build hard caps at 32-bit code = depth ≤ 32 levels per
        // bit-level, but random unit-cube inputs we benchmark on hit ~25-30.
        // Cap at 48 for safety; small overhead per wasted dispatch.
        crd::u32 log2_n = 0U;
        for (crd::u32 v = n; v > 1U; v >>= 1U) { ++log2_n; }
        // Cap iterations: random unit-cube tree depth ≈ 2·log₂(N) for random
        // binary trees; 32 covers N up to ~16 M. Each iteration adds buffer-
        // barrier driver cost (~50-100 μs on NVIDIA), so over-iterating burns
        // time without doing work. If a consumer hits a deeper pathological
        // tree, raise this cap or fall through to carry-register.
        const crd::u32 max_iters = std::min<crd::u32>(32U, log2_n + 8U);

        // Step A — init_leaves.
        {
            cmd->bind_compute_pipeline(*impl.init_leaves.pipeline);
            crd::rhi::DescriptorSet* sets[] = {ds_init_leaves.get()};
            cmd->bind_compute_descriptor_sets(*impl.init_leaves.pipeline_layout, 0U,
                crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
            UpsweepPushConstants pc{};
            pc.n = n;
            cmd->push_constants(*impl.init_leaves.pipeline_layout, crd::rhi::ShaderStage::Compute,
                                0U, sizeof(pc), &pc);
            const crd::u32 groups = (n + 63U) / 64U;
            cmd->dispatch(groups, 1U, 1U);
        }
        cmd->buffer_barrier(nodes_gpu, crd::rhi::BufferAccess::ComputeShaderWrite,
                                         crd::rhi::BufferAccess::ComputeShaderRead);
        cmd->buffer_barrier(done_gpu,  crd::rhi::BufferAccess::ComputeShaderWrite,
                                         crd::rhi::BufferAccess::ComputeShaderRead);

        // Step B — merge_level loop. Bind once (descriptors don't change
        // across iterations), push_constants once, then dispatch K times
        // with barriers between.
        cmd->bind_compute_pipeline(*impl.merge_level.pipeline);
        {
            crd::rhi::DescriptorSet* sets[] = {ds_merge_level.get()};
            cmd->bind_compute_descriptor_sets(*impl.merge_level.pipeline_layout, 0U,
                crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
            MergeLevelPushConstants pc{};
            pc.n_int = n_int;
            cmd->push_constants(*impl.merge_level.pipeline_layout, crd::rhi::ShaderStage::Compute,
                                0U, sizeof(pc), &pc);
        }
        const crd::u32 merge_groups = (n_int + 63U) / 64U;
        for (crd::u32 iter = 0U; iter < max_iters; ++iter)
        {
            cmd->dispatch(merge_groups, 1U, 1U);
            // Single barrier per iteration: Vulkan's COMPUTE→COMPUTE
            // SHADER_WRITE→SHADER_READ pipeline barrier provides device-wide
            // memory visibility independent of which buffer is named in the
            // VkBufferMemoryBarrier (the per-buffer reference is only used
            // for queue ownership transfers). Using one barrier instead of
            // two halves the per-iteration driver cost.
            cmd->buffer_barrier(nodes_gpu, crd::rhi::BufferAccess::ComputeShaderWrite,
                                             crd::rhi::BufferAccess::ComputeShaderRead);
        }
    }
    else
    {
        // Carry-register upsweep — N leaf-walker threads, single dispatch.
        // Handles arbitrary depth (used for small N + degenerate all-equal-
        // codes inputs where level-by-level would need too many iterations).
        cmd->bind_compute_pipeline(*impl.upsweep.pipeline);
        crd::rhi::DescriptorSet* sets[] = {ds_upsweep.get()};
        cmd->bind_compute_descriptor_sets(*impl.upsweep.pipeline_layout, 0U,
            crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
        UpsweepPushConstants pc{};
        pc.n = n;
        cmd->push_constants(*impl.upsweep.pipeline_layout, crd::rhi::ShaderStage::Compute,
                            0U, sizeof(pc), &pc);
        const crd::u32 groups = (n + 63U) / 64U;
        cmd->dispatch(groups, 1U, 1U);
    }

    if (request_nodes_readback)
    {
        cmd->buffer_barrier(nodes_gpu, crd::rhi::BufferAccess::ComputeShaderWrite,
                                         crd::rhi::BufferAccess::TransferSrc);
        cmd->copy_buffer(nodes_gpu, nodes_readback, 0U, 0U, nodes_bytes);
    }
    else
    {
        // GPU-resident output: leave nodes_gpu in compute-write state. The
        // caller will issue subsequent compute/transfer ops against it.
        cmd->buffer_barrier(nodes_gpu, crd::rhi::BufferAccess::ComputeShaderWrite,
                                         crd::rhi::BufferAccess::ComputeShaderRead);
    }

    cmd->end();
    device.graphics_queue().submit(*cmd, *fence);
    fence->wait();
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
        // Singleton: prim_indices buffer holds the one prim index; no nodes.
        // Stage the single prim index into prim_indices_gpu in this call.
        auto& device = *impl.device;
        if (auto* dst = static_cast<crd::u32*>(impl.prim_indices_staging->map()))
        {
            dst[0] = sorted_pairs[0].index;
            impl.prim_indices_staging->unmap();
        }
        auto* cmd   = impl.cached_cmd.get();
        auto* fence = impl.cached_fence.get();
        cmd->reset();
        fence->reset();
        cmd->begin();
        cmd->copy_buffer(*impl.prim_indices_staging, *impl.prim_indices_gpu, 0U, 0U, sizeof(crd::u32));
        cmd->end();
        device.graphics_queue().submit(*cmd, *fence);
        fence->wait();

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
    auto& impl   = *m_impl;
    auto& device = *impl.device;

    out.prim_count             = n;
    out.prim_indices           = impl.prim_indices_gpu.get();
    out.prim_indices_byte_size = static_cast<crd::u64>(n) * sizeof(crd::u32);

    // ---- N=1 singleton: no internal nodes; just copy prim_indices[0] ------
    if (n == 1U)
    {
        // Extract prim_indices[0] from inputs.sorted_pairs[1].
        impl.desc_alloc->begin_frame(0U);
        auto ds = impl.desc_alloc->allocate(*impl.extract_prim_indices.set_layout);
        if (ds == nullptr) { return GpuResidentTree{}; }
        ds->update_buffer(0U, *inputs.sorted_pairs, 0U, 2U * sizeof(crd::u32));
        ds->update_buffer(1U, *impl.prim_indices_gpu, 0U, sizeof(crd::u32));

        auto* cmd   = impl.cached_cmd.get();
        auto* fence = impl.cached_fence.get();
        cmd->reset();
        fence->reset();
        cmd->begin();
        cmd->bind_compute_pipeline(*impl.extract_prim_indices.pipeline);
        crd::rhi::DescriptorSet* sets[] = {ds.get()};
        cmd->bind_compute_descriptor_sets(*impl.extract_prim_indices.pipeline_layout, 0U,
            crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
        UpsweepPushConstants pc{};
        pc.n = 1U;
        cmd->push_constants(*impl.extract_prim_indices.pipeline_layout, crd::rhi::ShaderStage::Compute,
                            0U, sizeof(pc), &pc);
        cmd->dispatch(1U, 1U, 1U);
        cmd->end();
        device.graphics_queue().submit(*cmd, *fence);
        fence->wait();

        out.nodes           = nullptr;
        out.internal_count  = 0U;
        out.nodes_byte_size = 0U;
        return out;
    }

    const crd::u32 n_int = n - 1U;
    const crd::u64 pairs_bytes        = static_cast<crd::u64>(n)     * sizeof(MortonPair<crd::u32>);
    const crd::u64 aabbs_bytes        = static_cast<crd::u64>(n)     * (6U * sizeof(crd::f32));
    const crd::u64 nodes_bytes        = static_cast<crd::u64>(n_int) * 64U;
    const crd::u64 parent_bytes       = static_cast<crd::u64>(n)     * sizeof(crd::u32);
    const crd::u64 done_bytes         = static_cast<crd::u64>(n_int) * sizeof(crd::u32);
    const crd::u64 prim_indices_bytes = static_cast<crd::u64>(n)     * sizeof(crd::u32);

    auto& sorted_pairs_gpu = *inputs.sorted_pairs;
    auto& leaf_aabbs_gpu   = *inputs.leaf_aabbs;
    auto& nodes_gpu        = *impl.nodes_gpu;
    auto& prim_indices_gpu = *impl.prim_indices_gpu;
    auto& leaf_parents_gpu = *impl.leaf_parents_gpu;
    auto& done_staging     = *impl.done_staging;
    auto& done_gpu         = *impl.done_gpu;
    auto& root_init        = *impl.root_parent_init_staging;

    // ---- Descriptor sets --------------------------------------------------
    impl.desc_alloc->begin_frame(0U);

    auto ds_build = impl.desc_alloc->allocate(*impl.build.set_layout);
    if (ds_build == nullptr) { return GpuResidentTree{}; }
    ds_build->update_buffer(0U, sorted_pairs_gpu, 0U, pairs_bytes);
    ds_build->update_buffer(1U, nodes_gpu,        0U, nodes_bytes);
    ds_build->update_buffer(2U, leaf_parents_gpu, 0U, parent_bytes);

    // v9a-c-persistent-threads measurement 2026-05-18: persistent kernel
    // tested at 4K/16K/64K threads + warp-batched atomic pulls; all variants
    // within noise of regular flat-dispatch (1.39-1.64 ms range overall).
    // The upsweep is memory-bandwidth-bound at our 64 MB working set —
    // scheduling pattern is irrelevant. Reverting to regular upsweep (simpler,
    // same perf). Persistent kernel remains compiled for future-hardware /
    // mesh-coherent-input experiments.
    auto ds_upsweep = impl.desc_alloc->allocate(*impl.upsweep.set_layout);
    if (ds_upsweep == nullptr) { return GpuResidentTree{}; }
    ds_upsweep->update_buffer(0U, nodes_gpu,        0U, nodes_bytes);
    ds_upsweep->update_buffer(1U, leaf_parents_gpu, 0U, parent_bytes);
    ds_upsweep->update_buffer(2U, done_gpu,         0U, done_bytes);
    ds_upsweep->update_buffer(3U, sorted_pairs_gpu, 0U, pairs_bytes);
    ds_upsweep->update_buffer(4U, leaf_aabbs_gpu,   0U, aabbs_bytes);

    auto ds_extract = impl.desc_alloc->allocate(*impl.extract_prim_indices.set_layout);
    if (ds_extract == nullptr) { return GpuResidentTree{}; }
    ds_extract->update_buffer(0U, sorted_pairs_gpu, 0U, pairs_bytes);
    ds_extract->update_buffer(1U, prim_indices_gpu, 0U, prim_indices_bytes);

    // ---- Record + submit ---------------------------------------------------
    auto* cmd   = impl.cached_cmd.get();
    auto* fence = impl.cached_fence.get();
    cmd->reset();
    fence->reset();

    cmd->begin();

    // Init done_gpu (zeros, from pre-zeroed staging) + nodes[0].parent_idx
    // (sentinel). These are the only GPU transfers; inputs are already on GPU.
    cmd->copy_buffer(done_staging, done_gpu,  0U, 0U, done_bytes);
    cmd->copy_buffer(root_init,    nodes_gpu, 0U, 0U, 4U);

    cmd->buffer_barrier(done_gpu,         crd::rhi::BufferAccess::TransferDst,
                                            crd::rhi::BufferAccess::ComputeShaderWrite);
    cmd->buffer_barrier(nodes_gpu,        crd::rhi::BufferAccess::TransferDst,
                                            crd::rhi::BufferAccess::ComputeShaderWrite);
    // sorted_pairs_gpu + leaf_aabbs_gpu are assumed to be in ComputeShaderRead
    // state already (caller's contract; typically just-written by the prior
    // morton/sort kernels which signal-then-handover). No barrier needed.

    // Build kernel.
    {
        cmd->bind_compute_pipeline(*impl.build.pipeline);
        crd::rhi::DescriptorSet* sets[] = {ds_build.get()};
        cmd->bind_compute_descriptor_sets(*impl.build.pipeline_layout, 0U,
            crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
        BuildPushConstants pc{};
        pc.n = n;
        cmd->push_constants(*impl.build.pipeline_layout, crd::rhi::ShaderStage::Compute,
                            0U, sizeof(pc), &pc);
        const crd::u32 groups = (n_int + 255U) / 256U;
        cmd->dispatch(std::max<crd::u32>(groups, 1U), 1U, 1U);
    }
    cmd->buffer_barrier(nodes_gpu,        crd::rhi::BufferAccess::ComputeShaderWrite,
                                            crd::rhi::BufferAccess::ComputeShaderRead);
    cmd->buffer_barrier(leaf_parents_gpu, crd::rhi::BufferAccess::ComputeShaderWrite,
                                            crd::rhi::BufferAccess::ComputeShaderRead);

    // Regular flat-dispatch carry-register upsweep.
    {
        cmd->bind_compute_pipeline(*impl.upsweep.pipeline);
        crd::rhi::DescriptorSet* sets[] = {ds_upsweep.get()};
        cmd->bind_compute_descriptor_sets(*impl.upsweep.pipeline_layout, 0U,
            crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
        UpsweepPushConstants pc{};
        pc.n = n;
        cmd->push_constants(*impl.upsweep.pipeline_layout, crd::rhi::ShaderStage::Compute,
                            0U, sizeof(pc), &pc);
        const crd::u32 groups = (n + 63U) / 64U;
        cmd->dispatch(groups, 1U, 1U);
    }
    cmd->buffer_barrier(nodes_gpu, crd::rhi::BufferAccess::ComputeShaderWrite,
                                     crd::rhi::BufferAccess::ComputeShaderRead);

    // Extract prim_indices on GPU (avoids CPU memcpy + transfer of 4 MB).
    {
        cmd->bind_compute_pipeline(*impl.extract_prim_indices.pipeline);
        crd::rhi::DescriptorSet* sets[] = {ds_extract.get()};
        cmd->bind_compute_descriptor_sets(*impl.extract_prim_indices.pipeline_layout, 0U,
            crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
        UpsweepPushConstants pc{};
        pc.n = n;
        cmd->push_constants(*impl.extract_prim_indices.pipeline_layout, crd::rhi::ShaderStage::Compute,
                            0U, sizeof(pc), &pc);
        const crd::u32 groups = (n + 63U) / 64U;
        cmd->dispatch(groups, 1U, 1U);
    }
    cmd->buffer_barrier(prim_indices_gpu, crd::rhi::BufferAccess::ComputeShaderWrite,
                                            crd::rhi::BufferAccess::ComputeShaderRead);

    cmd->end();
    device.graphics_queue().submit(*cmd, *fence);
    fence->wait();

    out.nodes           = impl.nodes_gpu.get();
    out.internal_count  = n_int;
    out.nodes_byte_size = nodes_bytes;
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
    auto& impl   = *m_impl;
    auto& device = *impl.device;

    out.prim_count             = n;
    out.prim_indices           = impl.prim_indices_gpu.get();
    out.prim_indices_byte_size = static_cast<crd::u64>(n) * sizeof(crd::u32);

    // ---- N=1 singleton: nothing to refit (no internal bounds to recompute);
    // the tree had no internal nodes after build. Caller's leaf_aabbs is the
    // entire spatial information — they query it directly via prim_indices[0].
    if (n == 1U)
    {
        out.nodes           = nullptr;
        out.internal_count  = 0U;
        out.nodes_byte_size = 0U;
        return out;
    }

    const crd::u32 n_int = n - 1U;
    const crd::u64 pairs_bytes  = static_cast<crd::u64>(n)     * sizeof(MortonPair<crd::u32>);
    const crd::u64 aabbs_bytes  = static_cast<crd::u64>(n)     * (6U * sizeof(crd::f32));
    const crd::u64 nodes_bytes  = static_cast<crd::u64>(n_int) * 64U;
    const crd::u64 parent_bytes = static_cast<crd::u64>(n)     * sizeof(crd::u32);
    const crd::u64 done_bytes   = static_cast<crd::u64>(n_int) * sizeof(crd::u32);

    auto& sorted_pairs_gpu = *inputs.sorted_pairs;
    auto& leaf_aabbs_gpu   = *inputs.leaf_aabbs;
    auto& nodes_gpu        = *impl.nodes_gpu;
    auto& leaf_parents_gpu = *impl.leaf_parents_gpu;
    auto& done_staging     = *impl.done_staging;
    auto& done_gpu         = *impl.done_gpu;

    // ---- Descriptor set: upsweep only (5 bindings) -----------------------
    impl.desc_alloc->begin_frame(0U);
    auto ds_upsweep = impl.desc_alloc->allocate(*impl.upsweep.set_layout);
    if (ds_upsweep == nullptr) { return GpuResidentTree{}; }
    ds_upsweep->update_buffer(0U, nodes_gpu,        0U, nodes_bytes);
    ds_upsweep->update_buffer(1U, leaf_parents_gpu, 0U, parent_bytes);
    ds_upsweep->update_buffer(2U, done_gpu,         0U, done_bytes);
    ds_upsweep->update_buffer(3U, sorted_pairs_gpu, 0U, pairs_bytes);
    ds_upsweep->update_buffer(4U, leaf_aabbs_gpu,   0U, aabbs_bytes);

    // ---- Record + submit -------------------------------------------------
    auto* cmd   = impl.cached_cmd.get();
    auto* fence = impl.cached_fence.get();
    cmd->reset();
    fence->reset();

    cmd->begin();

    // Reset done_gpu to zero (from pre-zeroed staging). nodes_gpu topology
    // is preserved from the build; only its bounds slots will be overwritten
    // by the upsweep's `store_slot` calls.
    cmd->copy_buffer(done_staging, done_gpu, 0U, 0U, done_bytes);

    cmd->buffer_barrier(done_gpu,  crd::rhi::BufferAccess::TransferDst,
                                     crd::rhi::BufferAccess::ComputeShaderWrite);
    // nodes_gpu transition: prior call left it in ComputeShaderRead state
    // (or TransferSrc if the caller readback'd). Need ComputeShaderWrite
    // for the upsweep kernel to publish new bounds.
    cmd->buffer_barrier(nodes_gpu, crd::rhi::BufferAccess::ComputeShaderRead,
                                     crd::rhi::BufferAccess::ComputeShaderWrite);

    // Upsweep kernel — carry-register walks with NEW leaf AABBs. Topology
    // (parent_idx, left_idx, right_idx, leaf_parents) is untouched.
    {
        cmd->bind_compute_pipeline(*impl.upsweep.pipeline);
        crd::rhi::DescriptorSet* sets[] = {ds_upsweep.get()};
        cmd->bind_compute_descriptor_sets(*impl.upsweep.pipeline_layout, 0U,
            crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
        UpsweepPushConstants pc{};
        pc.n = n;
        cmd->push_constants(*impl.upsweep.pipeline_layout, crd::rhi::ShaderStage::Compute,
                            0U, sizeof(pc), &pc);
        const crd::u32 groups = (n + 63U) / 64U;
        cmd->dispatch(groups, 1U, 1U);
    }
    cmd->buffer_barrier(nodes_gpu, crd::rhi::BufferAccess::ComputeShaderWrite,
                                     crd::rhi::BufferAccess::ComputeShaderRead);

    cmd->end();
    device.graphics_queue().submit(*cmd, *fence);
    fence->wait();

    out.nodes           = impl.nodes_gpu.get();
    out.internal_count  = n_int;
    out.nodes_byte_size = nodes_bytes;
    return out;
}

} // namespace crd::geometry::bvh_gpu
