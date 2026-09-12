// ---------------------------------------------------------------------------
// MortonGpuPipeline — cached Vulkan compute pipeline + per-call dispatch
// for the v9a-a Morton-code kernel. Phase 3.1.7 v9a-a; v17-i-c: migrated onto
// crd::gpu::VulkanComputeContext (ADR-0099) — off the rendering RHI, on the dedicated compute queue.
//
// The dispatch path: stage `aabbs` → copy to GpuOnly storage → barrier →
// dispatch (ceil(count/64)) → read back into a caller-owned Array<u32>.
// The pipeline (shader compile + VkPipeline) is built once in the ctor and
// cached inside the compute context. Both `dispatch_morton_codes` and
// `dispatch_morton_codes_async` now run on the context's compute queue (the
// async/sync distinction collapsed with the RHI-graphics-queue removal).
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

struct alignas(16) MortonPushConstants
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
static_assert(sizeof(MortonPushConstants) == 48U,
              "MortonPushConstants must match the GLSL push_constant block size");

constexpr crd::u32 kWorkgroupSize = 64U;

} // namespace

struct MortonGpuPipeline::Impl
{
    crd::gpu::IComputeContext*                 ctx = nullptr;
    std::unique_ptr<crd::gpu::ComputePipeline> pipeline{};
    bool                                       valid = false;
};

MortonGpuPipeline::MortonGpuPipeline(crd::gpu::IComputeContext&   ctx,
                                      crd::containers::StringView shader_dir) noexcept
    : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.ctx   = &ctx;

    // Kernel by name — the backend loads its own cooked kernel; no API, no file format named here.
    impl.pipeline = ctx.create_pipeline(shader_dir, crd::containers::StringView{"compute_morton_codes"}, 2, sizeof(MortonPushConstants));
    if (impl.pipeline == nullptr) { return; }

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

[[nodiscard]] crd::containers::Array<crd::u32>
dispatch_inner(MortonGpuPipeline::Impl& impl,
               crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
               const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
               crd::memory::IAllocator* alloc) noexcept
{
    crd::containers::Array<crd::u32> out(alloc);
    if (aabbs.empty())
    {
        return out;
    }

    auto&          ctx       = *impl.ctx;
    const crd::u32 count     = static_cast<crd::u32>(aabbs.size());
    const crd::u64 in_bytes  = static_cast<crd::u64>(count) * 6U * sizeof(float);
    const crd::u64 out_bytes = static_cast<crd::u64>(count) * sizeof(crd::u32);

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
    // Out: a GpuToCpu storage buffer doubles as the readback target — the shader writes it host-coherent directly.
    auto out_buf = ctx.create_buffer(out_bytes, crd::gpu::compute_usage::storage, crd::gpu::ComputeMemory::GpuToCpu);
    if (in_staging == nullptr || in_gpu == nullptr || out_buf == nullptr)
    {
        return out; // OOM; return empty
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

    const crd::u32           group_count_x = (count + kWorkgroupSize - 1U) / kWorkgroupSize;
    crd::gpu::ComputeBuffer* binds[]       = {in_gpu.get(), out_buf.get()};

    auto& rec = ctx.begin();
    rec.copy(*in_staging, *in_gpu, 0U, 0U, in_bytes);
    rec.barrier(*in_gpu, crd::gpu::ComputeAccess::TransferDst, crd::gpu::ComputeAccess::ShaderRead);
    rec.dispatch(*impl.pipeline, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(binds, 2), &pc, sizeof(pc),
                 group_count_x, 1U, 1U);
    ctx.submit_and_wait();

    if (auto* src = static_cast<crd::u32*>(out_buf->map()))
    {
        out.resize(count, 0U);
        std::memcpy(out.data(), src, out_bytes);
        out_buf->unmap();
    }

    return out;
}

} // namespace

crd::containers::Array<crd::u32>
MortonGpuPipeline::dispatch_morton_codes(
    crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
    const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
    crd::memory::IAllocator* alloc) noexcept
{
    if (!is_valid()) { return crd::containers::Array<crd::u32>(alloc); }
    return dispatch_inner(*m_impl, aabbs, scene_aabb, alloc);
}

crd::containers::Array<crd::u32>
MortonGpuPipeline::dispatch_morton_codes_async(
    crd::containers::ConstSpan<crd::geometry::primitives::AABB3<crd::f32>> aabbs,
    const crd::geometry::primitives::AABB3<crd::f32>& scene_aabb,
    crd::memory::IAllocator* alloc) noexcept
{
    if (!is_valid()) { return crd::containers::Array<crd::u32>(alloc); }
    return dispatch_inner(*m_impl, aabbs, scene_aabb, alloc);
}

} // namespace crd::geometry::bvh_gpu
