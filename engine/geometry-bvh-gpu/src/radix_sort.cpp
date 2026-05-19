// ---------------------------------------------------------------------------
// MortonRadixGpuPipeline — cached Vulkan compute pipeline + per-call dispatch
// for the v9a-b2 GPU LSD radix sort. Phase 3.1.7 v9a-b2.
//
// The dispatch path (25 dispatches per sort, sync-fence-wait at the end):
//
//   1. upload `codes` into a host-coherent staging buffer.
//   2. copy_buffer into a GpuOnly storage buffer (in_codes).
//   3. init kernel: pack (code, gl_GlobalInvocationID.x) pairs into pairs_a.
//   4. for pass in 0..7:
//        a. histogram(pairs_in)  → block_hist
//        b. scan(block_hist)     → scatter_off
//        c. scatter(pairs_in, scatter_off) → pairs_out
//        d. swap pairs_in / pairs_out roles
//   5. After 8 passes (even), final result is back in pairs_a.
//   6. copy_buffer pairs_a → host-readback buffer.
//   7. fence wait + map + memcpy into caller-owned Array<MortonPair<u32>>.
//
// Pipeline objects (one per kernel × 4 kernels) are built once in the
// ctor; reused across many dispatches.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh_gpu/radix_sort.hpp>

#include <crd/containers/string.hpp>
#include <crd/core/assert.hpp>
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

#include <cstdint>
#include <cstring>

namespace crd::geometry::bvh_gpu
{

namespace
{

// Push-constant blocks per kernel. Each must match the GLSL `layout(push_constant)`
// declaration EXACTLY (std430 / scalar-compatible packing).

struct alignas(16) InitPushConstants
{
    std::uint32_t count = 0U;
    std::uint32_t pad[3] = {0U, 0U, 0U};
};
static_assert(sizeof(InitPushConstants) == 16U,
              "InitPushConstants must match radix_sort_init.comp's push_constant size");

struct alignas(16) HistogramPushConstants
{
    std::uint32_t count = 0U;
    std::uint32_t shift = 0U;
    std::uint32_t pad[2] = {0U, 0U};
};
static_assert(sizeof(HistogramPushConstants) == 16U,
              "HistogramPushConstants must match radix_sort_histogram.comp");

struct alignas(16) ScanPushConstants
{
    std::uint32_t num_blocks = 0U;
    std::uint32_t pad[3] = {0U, 0U, 0U};
};
static_assert(sizeof(ScanPushConstants) == 16U,
              "ScanPushConstants must match radix_sort_scan.comp");

struct alignas(16) ScatterPushConstants
{
    std::uint32_t count = 0U;
    std::uint32_t shift = 0U;
    std::uint32_t pad[2] = {0U, 0U};
};
static_assert(sizeof(ScatterPushConstants) == 16U,
              "ScatterPushConstants must match radix_sort_scatter.comp");

// Helper that loads a SPIR-V file + creates a single-stage compute pipeline.
// Returns false on any failure (file missing, validation, OOM); caller
// drops out of ctor leaving impl.valid = false.
struct PipelineQuad
{
    std::unique_ptr<crd::rhi::ShaderModule>        shader{};
    std::unique_ptr<crd::rhi::DescriptorSetLayout> set_layout{};
    std::unique_ptr<crd::rhi::PipelineLayout>      pipeline_layout{};
    std::unique_ptr<crd::rhi::ComputePipeline>     pipeline{};
};

[[nodiscard]] bool
build_pipeline(crd::rhi::Device& device,
                const crd::platform::fs::Path& spv_path,
                crd::containers::ConstSpan<crd::rhi::DescriptorBinding> bindings,
                std::uint32_t push_constants_size,
                PipelineQuad& out) noexcept
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

struct MortonRadixGpuPipeline::Impl
{
    crd::rhi::Device* device = nullptr;

    PipelineQuad init{};
    PipelineQuad histogram{};
    PipelineQuad scan{};
    PipelineQuad scatter{};

    std::unique_ptr<crd::rhi::DescriptorAllocator> desc_alloc{};

    bool valid = false;
};

MortonRadixGpuPipeline::MortonRadixGpuPipeline(
    crd::rhi::Device& device,
    crd::containers::StringView shader_dir) noexcept
    : m_impl(std::make_unique<Impl>())
{
    auto& impl  = *m_impl;
    impl.device = &device;

    const crd::platform::fs::Path base{shader_dir};

    // --- Binding layouts per kernel.
    // Init: in_codes (binding 0, read), out_pairs (binding 1, write).
    crd::rhi::DescriptorBinding init_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    // Histogram: in_pairs (read), out_block_hist (write).
    crd::rhi::DescriptorBinding histogram_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    // Scan: in_block_hist (read), out_scatter (write).
    crd::rhi::DescriptorBinding scan_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    // Scatter: in_pairs (read), out_pairs (write), in_scatter (read).
    crd::rhi::DescriptorBinding scatter_bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 2, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };

    auto path_for = [&](const char* name) {
        return base / crd::containers::StringView{name};
    };

    if (!build_pipeline(device, path_for("radix_sort_init.comp.spv"),
                        crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(init_bindings, 2),
                        sizeof(InitPushConstants), impl.init))
    { return; }
    if (!build_pipeline(device, path_for("radix_sort_histogram.comp.spv"),
                        crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(histogram_bindings, 2),
                        sizeof(HistogramPushConstants), impl.histogram))
    { return; }
    if (!build_pipeline(device, path_for("radix_sort_scan.comp.spv"),
                        crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(scan_bindings, 2),
                        sizeof(ScanPushConstants), impl.scan))
    { return; }
    if (!build_pipeline(device, path_for("radix_sort_scatter.comp.spv"),
                        crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(scatter_bindings, 3),
                        sizeof(ScatterPushConstants), impl.scatter))
    { return; }

    // --- Descriptor allocator. We need 1 init set + 8 × 3 = 24 pass sets
    // per dispatch_radix_sort call. Size the ring liberally.
    crd::rhi::DescriptorAllocatorDesc alloc_desc{};
    alloc_desc.frames_in_flight              = 2;
    alloc_desc.max_sets_per_frame            = 64;     // 25 + headroom
    alloc_desc.max_storage_buffers_per_frame = 256;    // ~3 per set × 64
    impl.desc_alloc = device.create_descriptor_allocator(alloc_desc);
    if (impl.desc_alloc == nullptr) { return; }

    impl.valid = true;
}

MortonRadixGpuPipeline::MortonRadixGpuPipeline(MortonRadixGpuPipeline&&) noexcept            = default;
MortonRadixGpuPipeline& MortonRadixGpuPipeline::operator=(MortonRadixGpuPipeline&&) noexcept = default;
MortonRadixGpuPipeline::~MortonRadixGpuPipeline()                                              = default;

bool MortonRadixGpuPipeline::is_valid() const noexcept
{
    return m_impl != nullptr && m_impl->valid;
}

crd::containers::Array<MortonPair<crd::u32>>
MortonRadixGpuPipeline::dispatch_radix_sort(
    crd::containers::ConstSpan<crd::u32> codes,
    crd::memory::IAllocator* alloc) noexcept
{
    crd::containers::Array<MortonPair<crd::u32>> out(alloc);
    if (!is_valid()) { return out; }
    if (codes.empty()) { return out; }

    const crd::u32 count = static_cast<crd::u32>(codes.size());
    CRD_ASSERT_MSG(count <= kRadixMaxItems,
                   "dispatch_radix_sort: input exceeds the v9a-b2 cap (D147 — kRadixMaxItems). "
                   "Beyond this requires the v9a-b2-large recursive-scan follow-on.");

    auto& impl   = *m_impl;
    auto& device = *impl.device;

    // --- Buffer sizing.
    const crd::u64 codes_bytes  = static_cast<crd::u64>(count) * sizeof(crd::u32);
    const crd::u64 pair_bytes   = static_cast<crd::u64>(count) * sizeof(MortonPair<crd::u32>);
    const crd::u32 num_blocks   = (count + kRadixItemsPerBlock - 1U) / kRadixItemsPerBlock;
    const crd::u64 hist_bytes   = static_cast<crd::u64>(num_blocks) * kRadixBins * sizeof(crd::u32);
    const crd::u64 offset_bytes = hist_bytes;

    // --- Buffers.
    auto codes_staging = device.create_buffer(
        {codes_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
         crd::rhi::MemoryUsage::CpuToGpu});
    auto codes_gpu = device.create_buffer(
        {codes_bytes,
         crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::GpuOnly});
    auto pairs_a = device.create_buffer(
        {pair_bytes,
         crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
         crd::rhi::MemoryUsage::GpuOnly});
    auto pairs_b = device.create_buffer(
        {pair_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuOnly});
    auto block_hist_buf = device.create_buffer(
        {hist_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuOnly});
    auto scatter_off_buf = device.create_buffer(
        {offset_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuOnly});
    auto pairs_readback = device.create_buffer(
        {pair_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::GpuToCpu});

    if (codes_staging == nullptr || codes_gpu == nullptr
        || pairs_a == nullptr || pairs_b == nullptr
        || block_hist_buf == nullptr || scatter_off_buf == nullptr
        || pairs_readback == nullptr)
    {
        return out;
    }

    // --- Stage input codes.
    if (auto* dst = static_cast<crd::u32*>(codes_staging->map()))
    {
        std::memcpy(dst, codes.data(), codes_bytes);
        codes_staging->unmap();
    }
    else { return out; }

    // --- Allocate the 25 descriptor sets up front (one per dispatch).
    impl.desc_alloc->begin_frame(0U);

    auto ds_init = impl.desc_alloc->allocate(*impl.init.set_layout);
    if (ds_init == nullptr) { return out; }
    ds_init->update_buffer(0U, *codes_gpu, 0U, codes_bytes);
    ds_init->update_buffer(1U, *pairs_a,   0U, pair_bytes);

    struct PassSets {
        std::unique_ptr<crd::rhi::DescriptorSet> histogram_set;
        std::unique_ptr<crd::rhi::DescriptorSet> scan_set;
        std::unique_ptr<crd::rhi::DescriptorSet> scatter_set;
    };
    PassSets passes[kRadixNumPassesU32]{};
    for (crd::u32 p = 0U; p < kRadixNumPassesU32; ++p)
    {
        // Pair buffer roles ping-pong each pass:
        //   even pass  : in = pairs_a, out = pairs_b
        //   odd  pass  : in = pairs_b, out = pairs_a
        crd::rhi::Buffer* pair_in  = (p % 2U == 0U) ? pairs_a.get() : pairs_b.get();
        crd::rhi::Buffer* pair_out = (p % 2U == 0U) ? pairs_b.get() : pairs_a.get();

        passes[p].histogram_set = impl.desc_alloc->allocate(*impl.histogram.set_layout);
        if (passes[p].histogram_set == nullptr) { return out; }
        passes[p].histogram_set->update_buffer(0U, *pair_in,        0U, pair_bytes);
        passes[p].histogram_set->update_buffer(1U, *block_hist_buf, 0U, hist_bytes);

        passes[p].scan_set = impl.desc_alloc->allocate(*impl.scan.set_layout);
        if (passes[p].scan_set == nullptr) { return out; }
        passes[p].scan_set->update_buffer(0U, *block_hist_buf,   0U, hist_bytes);
        passes[p].scan_set->update_buffer(1U, *scatter_off_buf, 0U, offset_bytes);

        passes[p].scatter_set = impl.desc_alloc->allocate(*impl.scatter.set_layout);
        if (passes[p].scatter_set == nullptr) { return out; }
        passes[p].scatter_set->update_buffer(0U, *pair_in,         0U, pair_bytes);
        passes[p].scatter_set->update_buffer(1U, *pair_out,        0U, pair_bytes);
        passes[p].scatter_set->update_buffer(2U, *scatter_off_buf, 0U, offset_bytes);
    }

    // --- Record + submit.
    auto cmd   = device.create_command_buffer();
    auto fence = device.create_fence();
    if (cmd == nullptr || fence == nullptr) { return out; }

    cmd->begin();

    // Staging copy → codes_gpu.
    cmd->copy_buffer(*codes_staging, *codes_gpu, 0U, 0U, codes_bytes);
    cmd->buffer_barrier(*codes_gpu, crd::rhi::BufferAccess::TransferDst,
                         crd::rhi::BufferAccess::ComputeShaderRead);

    // --- Init dispatch. Packs (code, gl_GlobalInvocationID.x) pairs into
    // pairs_a. Workgroup = 256 threads × 4 items/thread = 1024 items.
    {
        cmd->bind_compute_pipeline(*impl.init.pipeline);
        crd::rhi::DescriptorSet* sets[] = {ds_init.get()};
        cmd->bind_compute_descriptor_sets(
            *impl.init.pipeline_layout, 0U,
            crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
        InitPushConstants pc{};
        pc.count = count;
        cmd->push_constants(*impl.init.pipeline_layout,
                            crd::rhi::ShaderStage::Compute,
                            0U, sizeof(pc), &pc);
        const crd::u32 init_groups = (count + kRadixItemsPerBlock - 1U) / kRadixItemsPerBlock;
        cmd->dispatch(init_groups, 1U, 1U);
    }
    cmd->buffer_barrier(*pairs_a, crd::rhi::BufferAccess::ComputeShaderWrite,
                         crd::rhi::BufferAccess::ComputeShaderRead);

    // --- 8 passes × 3 dispatches each. pair_in / pair_out buffer roles
    // ping-pong each pass; descriptor sets were already wired in the
    // pre-loop allocation phase, so only pair_out is needed here (for the
    // post-scatter buffer barrier that hands off to the next pass).
    for (crd::u32 p = 0U; p < kRadixNumPassesU32; ++p)
    {
        const crd::u32 shift     = p * kRadixDigitBits;
        crd::rhi::Buffer* pair_out = (p % 2U == 0U) ? pairs_b.get() : pairs_a.get();

        // -- Histogram.
        cmd->bind_compute_pipeline(*impl.histogram.pipeline);
        {
            crd::rhi::DescriptorSet* sets[] = {passes[p].histogram_set.get()};
            cmd->bind_compute_descriptor_sets(
                *impl.histogram.pipeline_layout, 0U,
                crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
            HistogramPushConstants pc{};
            pc.count = count;
            pc.shift = shift;
            cmd->push_constants(*impl.histogram.pipeline_layout,
                                crd::rhi::ShaderStage::Compute,
                                0U, sizeof(pc), &pc);
            cmd->dispatch(num_blocks, 1U, 1U);
        }
        cmd->buffer_barrier(*block_hist_buf,
                             crd::rhi::BufferAccess::ComputeShaderWrite,
                             crd::rhi::BufferAccess::ComputeShaderRead);

        // -- Scan.
        cmd->bind_compute_pipeline(*impl.scan.pipeline);
        {
            crd::rhi::DescriptorSet* sets[] = {passes[p].scan_set.get()};
            cmd->bind_compute_descriptor_sets(
                *impl.scan.pipeline_layout, 0U,
                crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
            ScanPushConstants pc{};
            pc.num_blocks = num_blocks;
            cmd->push_constants(*impl.scan.pipeline_layout,
                                crd::rhi::ShaderStage::Compute,
                                0U, sizeof(pc), &pc);
            cmd->dispatch(1U, 1U, 1U);   // single-workgroup scan
        }
        cmd->buffer_barrier(*scatter_off_buf,
                             crd::rhi::BufferAccess::ComputeShaderWrite,
                             crd::rhi::BufferAccess::ComputeShaderRead);

        // -- Scatter.
        cmd->bind_compute_pipeline(*impl.scatter.pipeline);
        {
            crd::rhi::DescriptorSet* sets[] = {passes[p].scatter_set.get()};
            cmd->bind_compute_descriptor_sets(
                *impl.scatter.pipeline_layout, 0U,
                crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
            ScatterPushConstants pc{};
            pc.count = count;
            pc.shift = shift;
            cmd->push_constants(*impl.scatter.pipeline_layout,
                                crd::rhi::ShaderStage::Compute,
                                0U, sizeof(pc), &pc);
            cmd->dispatch(num_blocks, 1U, 1U);
        }
        // pair_out now holds this pass's output; barrier it to be read by
        // the next pass (or by the final transfer-read after pass 7).
        cmd->buffer_barrier(*pair_out,
                             crd::rhi::BufferAccess::ComputeShaderWrite,
                             crd::rhi::BufferAccess::ComputeShaderRead);
    }

    // After 8 (even) passes, the final output is in pairs_a (pass 7 was
    // odd, scatter wrote pair_out = pairs_a). Barrier pairs_a from compute
    // read → transfer src, then copy to host-visible readback.
    cmd->buffer_barrier(*pairs_a,
                         crd::rhi::BufferAccess::ComputeShaderRead,
                         crd::rhi::BufferAccess::TransferSrc);
    cmd->copy_buffer(*pairs_a, *pairs_readback, 0U, 0U, pair_bytes);

    cmd->end();
    device.graphics_queue().submit(*cmd, *fence);
    fence->wait();

    // --- Readback.
    if (auto* src = static_cast<MortonPair<crd::u32>*>(pairs_readback->map()))
    {
        out.resize(count);
        std::memcpy(out.data(), src, pair_bytes);
        pairs_readback->unmap();
    }

    return out;
}

} // namespace crd::geometry::bvh_gpu
