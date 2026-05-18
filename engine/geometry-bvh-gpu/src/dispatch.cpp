// ---------------------------------------------------------------------------
// MortonGpuPipeline — cached Vulkan compute pipeline + per-call dispatch
// for the v9a-a Morton-code kernel. Phase 3.1.7 v9a-a.
//
// The dispatch path:
//   1. upload `aabbs` into a host-coherent staging buffer.
//   2. copy_buffer into a GpuOnly storage buffer.
//   3. buffer_barrier (TransferDst → ComputeShaderRead).
//   4. bind compute pipeline + descriptor set + push constants.
//   5. dispatch (ceil(count / 64), 1, 1).
//   6. buffer_barrier on output (ComputeShaderWrite → TransferSrc).
//   7. copy_buffer output → host-readback buffer.
//   8. fence wait + map + memcpy into caller-owned Array<u32>.
//
// Per-call cost is the staging + dispatch + readback. The pipeline
// itself (shader compile + VkPipeline) is built once in the ctor.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh_gpu/dispatch.hpp>

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

// Push-constant block — must match shader `layout(push_constant)`
// declaration EXACTLY (std140 / scalar-block-layout interplay; we use
// scalar / std430 compatible packing here = 3 floats + pad to vec4 +
// 3 floats + pad + 1 uint + pad).
struct alignas(16) MortonPushConstants
{
    float    scene_min_x  = 0.0F;
    float    scene_min_y  = 0.0F;
    float    scene_min_z  = 0.0F;
    float    pad_a        = 0.0F;
    float    inv_extent_x = 0.0F;
    float    inv_extent_y = 0.0F;
    float    inv_extent_z = 0.0F;
    float    pad_b        = 0.0F;
    std::uint32_t count        = 0U;
    std::uint32_t pad_c[3]     = {0U, 0U, 0U};
};
static_assert(sizeof(MortonPushConstants) == 48U,
              "MortonPushConstants must match the GLSL push_constant block size");

constexpr crd::u32 kWorkgroupSize = 64U;

} // namespace

struct MortonGpuPipeline::Impl
{
    crd::rhi::Device*                              device      = nullptr;
    std::unique_ptr<crd::rhi::ShaderModule>        shader{};
    std::unique_ptr<crd::rhi::DescriptorSetLayout> set_layout{};
    std::unique_ptr<crd::rhi::PipelineLayout>      pipeline_layout{};
    std::unique_ptr<crd::rhi::ComputePipeline>     pipeline{};
    std::unique_ptr<crd::rhi::DescriptorAllocator> desc_alloc{};
    bool                                           valid = false;
};

MortonGpuPipeline::MortonGpuPipeline(crd::rhi::Device& device,
                                      crd::containers::StringView shader_dir) noexcept
    : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.device = &device;

    // --- Load SPIR-V from the shader directory.
    crd::platform::fs::Path spv_path{shader_dir};
    spv_path = spv_path / crd::containers::StringView{"compute_morton_codes.comp.spv"};
    crd::containers::Array<crd::u8> spv;
    if (!crd::platform::fs::read_file_binary(spv_path, spv))
    {
        return; // impl.valid stays false
    }

    impl.shader = device.create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main",
         crd::containers::ConstSpan<crd::u8>(spv.data(), spv.size())});
    if (impl.shader == nullptr) { return; }

    // --- Descriptor set layout: set 0, two storage buffers (in + out).
    crd::rhi::DescriptorBinding bindings[] = {
        {.binding = 0, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
        {.binding = 1, .type = crd::rhi::DescriptorType::StorageBuffer,
         .count = 1, .stages = crd::rhi::ShaderStage::Compute},
    };
    crd::rhi::DescriptorSetLayoutDesc set_desc{};
    set_desc.bindings = crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(bindings, 2);
    impl.set_layout = device.create_descriptor_set_layout(set_desc);
    if (impl.set_layout == nullptr) { return; }

    // --- Pipeline layout: 1 descriptor set + push-constant range.
    const crd::rhi::DescriptorSetLayout* layouts[] = {impl.set_layout.get()};
    crd::rhi::PushConstantRange pc_range{};
    pc_range.stages = crd::rhi::ShaderStage::Compute;
    pc_range.offset = 0U;
    pc_range.size   = sizeof(MortonPushConstants);
    crd::rhi::PipelineLayoutDesc layout_desc{};
    layout_desc.set_layouts =
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(layouts, 1);
    layout_desc.push_constant_ranges =
        crd::containers::ConstSpan<crd::rhi::PushConstantRange>(&pc_range, 1);
    impl.pipeline_layout = device.create_pipeline_layout(layout_desc);
    if (impl.pipeline_layout == nullptr) { return; }

    // --- Compute pipeline.
    crd::rhi::ComputePipelineDesc pipe_desc{};
    pipe_desc.compute_shader  = impl.shader.get();
    pipe_desc.pipeline_layout = impl.pipeline_layout.get();
    impl.pipeline = device.create_compute_pipeline(pipe_desc);
    if (impl.pipeline == nullptr) { return; }

    // --- Descriptor allocator (small ring; one set per dispatch).
    crd::rhi::DescriptorAllocatorDesc alloc_desc{};
    alloc_desc.frames_in_flight              = 2; // ping-pong
    alloc_desc.max_sets_per_frame            = 4;
    alloc_desc.max_storage_buffers_per_frame = 8;
    impl.desc_alloc = device.create_descriptor_allocator(alloc_desc);
    if (impl.desc_alloc == nullptr) { return; }

    impl.valid = true;
}

MortonGpuPipeline::MortonGpuPipeline(MortonGpuPipeline&&) noexcept            = default;
MortonGpuPipeline& MortonGpuPipeline::operator=(MortonGpuPipeline&&) noexcept = default;
MortonGpuPipeline::~MortonGpuPipeline()                                        = default;

bool MortonGpuPipeline::is_valid() const noexcept
{
    return m_impl != nullptr && m_impl->valid;
}

namespace
{

// Inner dispatch body. Selects the queue + command-buffer factory based
// on `use_async` (false = graphics-queue/graphics-pool sync path; true =
// compute-queue/compute-pool async path, both still fence-waited).
[[nodiscard]] crd::containers::Array<crd::u32>
dispatch_inner(MortonGpuPipeline::Impl& impl,
               crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
               const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
               bool use_async,
               crd::memory::IAllocator* alloc) noexcept;

} // namespace

crd::containers::Array<crd::u32>
MortonGpuPipeline::dispatch_morton_codes(
    crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
    const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
    crd::memory::IAllocator* alloc) noexcept
{
    if (!is_valid()) { return crd::containers::Array<crd::u32>(alloc); }
    return dispatch_inner(*m_impl, aabbs, scene_aabb, /*use_async*/ false, alloc);
}

crd::containers::Array<crd::u32>
MortonGpuPipeline::dispatch_morton_codes_async(
    crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
    const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
    crd::memory::IAllocator* alloc) noexcept
{
    if (!is_valid()) { return crd::containers::Array<crd::u32>(alloc); }
    return dispatch_inner(*m_impl, aabbs, scene_aabb, /*use_async*/ true, alloc);
}

namespace
{

crd::containers::Array<crd::u32>
dispatch_inner(MortonGpuPipeline::Impl& impl,
               crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
               const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
               bool use_async,
               crd::memory::IAllocator* alloc) noexcept
{
    crd::containers::Array<crd::u32> out(alloc);
    if (aabbs.empty())
    {
        return out;
    }

    auto& device = *impl.device;
    const crd::u32 count           = static_cast<crd::u32>(aabbs.size());
    const crd::u64 in_bytes        = static_cast<crd::u64>(count) * 6U * sizeof(float);
    const crd::u64 out_bytes       = static_cast<crd::u64>(count) * sizeof(crd::u32);

    // --- Pack input AABBs into a flat float[6N] layout for the shader.
    crd::containers::Array<float> packed_in(alloc);
    packed_in.resize(static_cast<crd::usize>(count) * 6U, 0.0F);
    for (crd::u32 i = 0U; i < count; ++i)
    {
        const auto& b = aabbs[i];
        packed_in[6U * i + 0U] = b.min.x;
        packed_in[6U * i + 1U] = b.min.y;
        packed_in[6U * i + 2U] = b.min.z;
        packed_in[6U * i + 3U] = b.max.x;
        packed_in[6U * i + 4U] = b.max.y;
        packed_in[6U * i + 5U] = b.max.z;
    }

    // --- Buffers: in (staged), in_gpu (storage), out_readback (host-visible storage).
    auto in_staging = device.create_buffer(
        {in_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
         crd::rhi::MemoryUsage::CpuToGpu});
    auto in_gpu = device.create_buffer(
        {in_bytes,
         crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::GpuOnly});
    // Out: a GpuToCpu storage buffer doubles as the readback target — no
    // separate copy needed because the shader writes it directly host-
    // coherent.
    auto out_buf = device.create_buffer(
        {out_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuToCpu});
    if (in_staging == nullptr || in_gpu == nullptr || out_buf == nullptr)
    {
        return out; // OOM; return empty
    }

    // Stage in.
    if (auto* dst = static_cast<float*>(in_staging->map()))
    {
        std::memcpy(dst, packed_in.data(), in_bytes);
        in_staging->unmap();
    }
    else
    {
        return out;
    }

    // --- Descriptor set.
    impl.desc_alloc->begin_frame(0U);
    auto desc_set = impl.desc_alloc->allocate(*impl.set_layout);
    if (desc_set == nullptr) { return out; }
    desc_set->update_buffer(0U, *in_gpu, 0U, in_bytes);
    desc_set->update_buffer(1U, *out_buf, 0U, out_bytes);

    // --- Push constants.
    MortonPushConstants pc{};
    pc.scene_min_x = scene_aabb.min.x;
    pc.scene_min_y = scene_aabb.min.y;
    pc.scene_min_z = scene_aabb.min.z;
    const float ex = scene_aabb.max.x - scene_aabb.min.x;
    const float ey = scene_aabb.max.y - scene_aabb.min.y;
    const float ez = scene_aabb.max.z - scene_aabb.min.z;
    pc.inv_extent_x = ex > 0.0F ? (1.0F / ex) : 0.0F;
    pc.inv_extent_y = ey > 0.0F ? (1.0F / ey) : 0.0F;
    pc.inv_extent_z = ez > 0.0F ? (1.0F / ez) : 0.0F;
    pc.count        = count;

    // --- Record + submit. Select queue + command-buffer factory based
    // on the sync/async flag. The async path uses the dedicated compute
    // queue (when one exists) via the v9a-a-async-compute factory; the
    // sync path uses the graphics queue + graphics-family command pool
    // (the pre-async-compute path).
    crd::rhi::Queue* submit_queue = &device.graphics_queue();
    if (use_async)
    {
        submit_queue = &device.compute_queue();
    }
    auto cmd = use_async ? device.create_command_buffer_for_queue(*submit_queue)
                          : device.create_command_buffer();
    auto fence = device.create_fence();
    if (cmd == nullptr || fence == nullptr) { return out; }

    cmd->begin();
    // Staging copy → GPU input buffer.
    cmd->copy_buffer(*in_staging, *in_gpu, 0U, 0U, in_bytes);
    // Barrier: transfer destination → compute shader read.
    cmd->buffer_barrier(*in_gpu, crd::rhi::BufferAccess::TransferDst,
                         crd::rhi::BufferAccess::ComputeShaderRead);

    cmd->bind_compute_pipeline(*impl.pipeline);
    crd::rhi::DescriptorSet* sets[] = {desc_set.get()};
    cmd->bind_compute_descriptor_sets(
        *impl.pipeline_layout, 0U,
        crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
    cmd->push_constants(*impl.pipeline_layout, crd::rhi::ShaderStage::Compute,
                          0U, sizeof(pc), &pc);

    const crd::u32 group_count_x = (count + kWorkgroupSize - 1U) / kWorkgroupSize;
    cmd->dispatch(group_count_x, 1U, 1U);

    cmd->end();
    submit_queue->submit(*cmd, *fence);
    fence->wait();

    // --- Readback. out_buf is GpuToCpu host-coherent; map + memcpy.
    if (auto* src = static_cast<crd::u32*>(out_buf->map()))
    {
        out.resize(count, 0U);
        std::memcpy(out.data(), src, out_bytes);
        out_buf->unmap();
    }

    return out;
}

} // anonymous namespace (dispatch_inner)

} // namespace crd::geometry::bvh_gpu
