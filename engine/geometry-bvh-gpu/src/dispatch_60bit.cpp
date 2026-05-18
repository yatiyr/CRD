// ---------------------------------------------------------------------------
// MortonGpu60BitPipeline — u64 60-bit Morton GPU dispatch. Phase 3.1.7
// v9a-60bit-gpu (2026-05-18).
//
// Sibling of MortonGpuPipeline (30-bit u32 path). Requires the
// `shaderInt64` Vulkan feature; ctor checks `Device::supports_shader_int64()`
// and returns invalid pipeline if unavailable (caller falls back to 30-bit).
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

struct alignas(16) Morton60PushConstants
{
    float        scene_min_x  = 0.0F;
    float        scene_min_y  = 0.0F;
    float        scene_min_z  = 0.0F;
    float        pad_a        = 0.0F;
    float        inv_extent_x = 0.0F;
    float        inv_extent_y = 0.0F;
    float        inv_extent_z = 0.0F;
    float        pad_b        = 0.0F;
    std::uint32_t count       = 0U;
    std::uint32_t pad_c[3]    = {0U, 0U, 0U};
};
static_assert(sizeof(Morton60PushConstants) == 48U,
              "Morton60PushConstants must match the GLSL 60-bit push_constant block size");

constexpr crd::u32 kWorkgroupSize60 = 64U;

} // namespace

struct MortonGpu60BitPipeline::Impl
{
    crd::rhi::Device*                              device      = nullptr;
    std::unique_ptr<crd::rhi::ShaderModule>        shader{};
    std::unique_ptr<crd::rhi::DescriptorSetLayout> set_layout{};
    std::unique_ptr<crd::rhi::PipelineLayout>      pipeline_layout{};
    std::unique_ptr<crd::rhi::ComputePipeline>     pipeline{};
    std::unique_ptr<crd::rhi::DescriptorAllocator> desc_alloc{};
    bool                                           valid = false;
};

MortonGpu60BitPipeline::MortonGpu60BitPipeline(crd::rhi::Device& device,
                                                  crd::containers::StringView shader_dir) noexcept
    : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.device = &device;

    // Gracefully degrade if the device doesn't support 64-bit shader ints.
    if (!device.supports_shader_int64())
    {
        return; // valid stays false
    }

    crd::platform::fs::Path spv_path{shader_dir};
    spv_path = spv_path / crd::containers::StringView{"compute_morton_codes_60bit.comp.spv"};
    crd::containers::Array<crd::u8> spv;
    if (!crd::platform::fs::read_file_binary(spv_path, spv))
    {
        return;
    }

    impl.shader = device.create_shader_module(
        {crd::rhi::ShaderStage::Compute, "main",
         crd::containers::ConstSpan<crd::u8>(spv.data(), spv.size())});
    if (impl.shader == nullptr) { return; }

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

    const crd::rhi::DescriptorSetLayout* layouts[] = {impl.set_layout.get()};
    crd::rhi::PushConstantRange pc_range{};
    pc_range.stages = crd::rhi::ShaderStage::Compute;
    pc_range.offset = 0U;
    pc_range.size   = sizeof(Morton60PushConstants);
    crd::rhi::PipelineLayoutDesc layout_desc{};
    layout_desc.set_layouts =
        crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(layouts, 1);
    layout_desc.push_constant_ranges =
        crd::containers::ConstSpan<crd::rhi::PushConstantRange>(&pc_range, 1);
    impl.pipeline_layout = device.create_pipeline_layout(layout_desc);
    if (impl.pipeline_layout == nullptr) { return; }

    crd::rhi::ComputePipelineDesc pipe_desc{};
    pipe_desc.compute_shader  = impl.shader.get();
    pipe_desc.pipeline_layout = impl.pipeline_layout.get();
    impl.pipeline = device.create_compute_pipeline(pipe_desc);
    if (impl.pipeline == nullptr) { return; }

    crd::rhi::DescriptorAllocatorDesc alloc_desc{};
    alloc_desc.frames_in_flight              = 2;
    alloc_desc.max_sets_per_frame            = 4;
    alloc_desc.max_storage_buffers_per_frame = 8;
    impl.desc_alloc = device.create_descriptor_allocator(alloc_desc);
    if (impl.desc_alloc == nullptr) { return; }

    impl.valid = true;
}

MortonGpu60BitPipeline::MortonGpu60BitPipeline(MortonGpu60BitPipeline&&) noexcept            = default;
MortonGpu60BitPipeline& MortonGpu60BitPipeline::operator=(MortonGpu60BitPipeline&&) noexcept = default;
MortonGpu60BitPipeline::~MortonGpu60BitPipeline()                                              = default;

bool MortonGpu60BitPipeline::is_valid() const noexcept
{
    return m_impl != nullptr && m_impl->valid;
}

crd::containers::Array<std::uint64_t>
MortonGpu60BitPipeline::dispatch_morton_codes_60bit(
    crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
    const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
    crd::memory::IAllocator* alloc) noexcept
{
    crd::containers::Array<std::uint64_t> out(alloc);
    if (!is_valid() || aabbs.empty())
    {
        return out;
    }

    auto& impl       = *m_impl;
    auto& device     = *impl.device;
    const crd::u32 count = static_cast<crd::u32>(aabbs.size());
    const crd::u64 in_bytes  = static_cast<crd::u64>(count) * 6U * sizeof(float);
    const crd::u64 out_bytes = static_cast<crd::u64>(count) * sizeof(std::uint64_t);

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

    auto in_staging = device.create_buffer(
        {in_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
         crd::rhi::MemoryUsage::CpuToGpu});
    auto in_gpu = device.create_buffer(
        {in_bytes,
         crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::GpuOnly});
    auto out_buf = device.create_buffer(
        {out_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage),
         crd::rhi::MemoryUsage::GpuToCpu});
    if (in_staging == nullptr || in_gpu == nullptr || out_buf == nullptr)
    {
        return out;
    }

    if (auto* dst = static_cast<float*>(in_staging->map()))
    {
        std::memcpy(dst, packed_in.data(), in_bytes);
        in_staging->unmap();
    }
    else
    {
        return out;
    }

    impl.desc_alloc->begin_frame(0U);
    auto desc_set = impl.desc_alloc->allocate(*impl.set_layout);
    if (desc_set == nullptr) { return out; }
    desc_set->update_buffer(0U, *in_gpu, 0U, in_bytes);
    desc_set->update_buffer(1U, *out_buf, 0U, out_bytes);

    Morton60PushConstants pc{};
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

    auto cmd   = device.create_command_buffer();
    auto fence = device.create_fence();
    if (cmd == nullptr || fence == nullptr) { return out; }

    cmd->begin();
    cmd->copy_buffer(*in_staging, *in_gpu, 0U, 0U, in_bytes);
    cmd->buffer_barrier(*in_gpu, crd::rhi::BufferAccess::TransferDst,
                         crd::rhi::BufferAccess::ComputeShaderRead);
    cmd->bind_compute_pipeline(*impl.pipeline);
    crd::rhi::DescriptorSet* sets[] = {desc_set.get()};
    cmd->bind_compute_descriptor_sets(
        *impl.pipeline_layout, 0U,
        crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
    cmd->push_constants(*impl.pipeline_layout, crd::rhi::ShaderStage::Compute,
                          0U, sizeof(pc), &pc);
    const crd::u32 group_count_x = (count + kWorkgroupSize60 - 1U) / kWorkgroupSize60;
    cmd->dispatch(group_count_x, 1U, 1U);
    cmd->end();
    device.graphics_queue().submit(*cmd, *fence);
    fence->wait();

    if (auto* src = static_cast<std::uint64_t*>(out_buf->map()))
    {
        out.resize(count, std::uint64_t{0});
        std::memcpy(out.data(), src, out_bytes);
        out_buf->unmap();
    }
    return out;
}

} // namespace crd::geometry::bvh_gpu
