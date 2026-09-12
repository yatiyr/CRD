// ---------------------------------------------------------------------------
// MortonGpu60BitPipeline — u64 60-bit Morton GPU dispatch. Phase 3.1.7
// v9a-60bit-gpu (2026-05-18); v17-i-c: migrated onto crd::gpu::VulkanComputeContext (ADR-0099, off the rendering RHI).
//
// Sibling of MortonGpuPipeline (30-bit u32 path). Requires the
// `shaderInt64` Vulkan feature; ctor checks `VulkanComputeContext::supports_shader_int64()`
// and returns invalid pipeline if unavailable (caller falls back to 30-bit).
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh_gpu/dispatch.hpp>

#include <crd/containers/string.hpp>
#include <crd/core/assert.hpp>
#include <crd/gpu/compute.hpp>

#include <cstdint>
#include <cstring>

namespace crd::geometry::bvh_gpu
{

namespace
{

struct alignas(16) Morton60PushConstants
{
    float         scene_min_x  = 0.0F;
    float         scene_min_y  = 0.0F;
    float         scene_min_z  = 0.0F;
    float         pad_a        = 0.0F;
    float         inv_extent_x = 0.0F;
    float         inv_extent_y = 0.0F;
    float         inv_extent_z = 0.0F;
    float         pad_b        = 0.0F;
    std::uint32_t count        = 0U;
    std::uint32_t pad_c[3]     = {0U, 0U, 0U};
};
static_assert(sizeof(Morton60PushConstants) == 48U,
              "Morton60PushConstants must match the GLSL 60-bit push_constant block size");

constexpr crd::u32 kWorkgroupSize60 = 64U;

} // namespace

struct MortonGpu60BitPipeline::Impl
{
    crd::gpu::IComputeContext*                 ctx = nullptr;
    std::unique_ptr<crd::gpu::ComputePipeline> pipeline{};
    bool                                       valid = false;
};

MortonGpu60BitPipeline::MortonGpu60BitPipeline(crd::gpu::IComputeContext&   ctx,
                                                  crd::containers::StringView shader_dir) noexcept
    : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.ctx   = &ctx;

    // Gracefully degrade if the device doesn't support 64-bit shader ints.
    if (!ctx.supports_shader_int64())
    {
        return; // valid stays false
    }

    // Kernel by name — the backend loads its own cooked kernel; no API, no file format named here.
    impl.pipeline = ctx.create_pipeline(shader_dir, crd::containers::StringView{"compute_morton_codes_60bit"}, 2, sizeof(Morton60PushConstants));
    if (impl.pipeline == nullptr) { return; }

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

    auto&          impl      = *m_impl;
    auto&          ctx       = *impl.ctx;
    const crd::u32 count     = static_cast<crd::u32>(aabbs.size());
    const crd::u64 in_bytes  = static_cast<crd::u64>(count) * 6U * sizeof(float);
    const crd::u64 out_bytes = static_cast<crd::u64>(count) * sizeof(std::uint64_t);

    crd::containers::Array<float> packed_in(alloc);
    packed_in.resize(static_cast<crd::usize>(count) * 6U, 0.0F);
    for (crd::u32 i = 0U; i < count; ++i)
    {
        const auto& b          = aabbs[i];
        packed_in[6U * i + 0U] = b.min.x;
        packed_in[6U * i + 1U] = b.min.y;
        packed_in[6U * i + 2U] = b.min.z;
        packed_in[6U * i + 3U] = b.max.x;
        packed_in[6U * i + 4U] = b.max.y;
        packed_in[6U * i + 5U] = b.max.z;
    }

    auto in_staging = ctx.create_buffer(in_bytes, crd::gpu::compute_usage::transfer_src, crd::gpu::ComputeMemory::CpuToGpu);
    auto in_gpu     = ctx.create_buffer(in_bytes, crd::gpu::compute_usage::storage | crd::gpu::compute_usage::transfer_dst,
                                        crd::gpu::ComputeMemory::GpuOnly);
    auto out_buf    = ctx.create_buffer(out_bytes, crd::gpu::compute_usage::storage, crd::gpu::ComputeMemory::GpuToCpu);
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

    const crd::u32           group_count_x = (count + kWorkgroupSize60 - 1U) / kWorkgroupSize60;
    crd::gpu::ComputeBuffer* binds[]       = {in_gpu.get(), out_buf.get()};

    auto& rec = ctx.begin();
    rec.copy(*in_staging, *in_gpu, 0U, 0U, in_bytes);
    rec.barrier(*in_gpu, crd::gpu::ComputeAccess::TransferDst, crd::gpu::ComputeAccess::ShaderRead);
    rec.dispatch(*impl.pipeline, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(binds, 2), &pc, sizeof(pc),
                 group_count_x, 1U, 1U);
    ctx.submit_and_wait();

    if (auto* src = static_cast<std::uint64_t*>(out_buf->map()))
    {
        out.resize(count, std::uint64_t{0});
        std::memcpy(out.data(), src, out_bytes);
        out_buf->unmap();
    }
    return out;
}

} // namespace crd::geometry::bvh_gpu
