#include <crd/renderer/gpu_uploader.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/queue.hpp>
#include <crd/rhi/types.hpp>
#include <cstring>

namespace crd::renderer
{

namespace
{
[[nodiscard]] crd::rhi::Format texture_format_to_rhi(TextureFormat fmt) noexcept
{
    switch (fmt)
    {
        case TextureFormat::RGBA8Unorm:
            return crd::rhi::Format::R8G8B8A8Unorm;
        default:
            CRD_ASSERT_UNREACHABLE("unsupported TextureFormat in GpuUploader (BC7 deferred to Phase 2.8)");
            return crd::rhi::Format::Undefined;
    }
}
} // namespace

GpuTexture GpuUploader::upload_texture(const TextureResource& cpu, crd::rhi::Device& device)
{
    CRD_ASSERT(!cpu.mips.empty());

    // Calculate total staging buffer size.
    crd::u64 total_bytes = 0;
    for (const auto& mip : cpu.mips)
    {
        total_bytes += static_cast<crd::u64>(mip.pixels.size());
    }
    CRD_ASSERT(total_bytes > 0);

    // Create host-visible staging buffer.
    auto staging = device.create_buffer({
        .size_bytes   = total_bytes,
        .usage        = crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
        .memory_usage = crd::rhi::MemoryUsage::CpuToGpu,
    });

    // Upload all mips into the staging buffer.
    auto* ptr       = static_cast<crd::u8*>(staging->map());
    crd::u64 offset = 0;
    for (const auto& mip : cpu.mips)
    {
        std::memcpy(ptr + offset, mip.pixels.data(), mip.pixels.size());
        offset += static_cast<crd::u64>(mip.pixels.size());
    }
    staging->unmap();

    // Create the device-local image.
    const auto& top = cpu.mips[0];
    auto image      = device.create_image({
             .extent     = {top.width, top.height},
             .format     = texture_format_to_rhi(cpu.format),
             .usage      = crd::rhi::enum_bits(crd::rhi::ImageUsage::TransferDst) |
                      crd::rhi::enum_bits(crd::rhi::ImageUsage::Sampled),
             .mip_levels  = cpu.mip_count,
             .array_layers = 1,
    });

    // Build the per-mip copy regions.
    crd::containers::Array<crd::rhi::BufferImageCopy> regions;
    offset = 0;
    for (crd::u32 m = 0; m < cpu.mip_count; ++m)
    {
        const auto& mip = cpu.mips[m];
        regions.push_back({
            .buffer_offset = offset,
            .mip_level     = m,
            .extent        = {mip.width, mip.height},
        });
        offset += static_cast<crd::u64>(mip.pixels.size());
    }

    // Record and submit transfer commands.
    auto cmd = device.create_command_buffer();
    cmd->begin();
    cmd->transition_image(*image, crd::rhi::ImageAccess::Undefined, crd::rhi::ImageAccess::TransferDst);
    cmd->copy_buffer_to_image(*staging, *image, crd::containers::as_const_span(regions));
    cmd->transition_image(*image, crd::rhi::ImageAccess::TransferDst, crd::rhi::ImageAccess::ShaderRead);
    cmd->end();
    device.graphics_queue().submit_and_wait(*cmd);

    return GpuTexture{std::move(image)};
}

GpuMesh GpuUploader::upload_mesh(const MeshResource& cpu, crd::rhi::Device& device)
{
    CRD_ASSERT(!cpu.vertices.empty());
    CRD_ASSERT(!cpu.indices.empty());

    const crd::u64 vb_bytes = static_cast<crd::u64>(cpu.vertices.size());
    const crd::u64 ib_bytes = static_cast<crd::u64>(cpu.indices.size());

    // Staging buffers.
    auto stage_vb = device.create_buffer({
        .size_bytes   = vb_bytes,
        .usage        = crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
        .memory_usage = crd::rhi::MemoryUsage::CpuToGpu,
    });
    auto stage_ib = device.create_buffer({
        .size_bytes   = ib_bytes,
        .usage        = crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
        .memory_usage = crd::rhi::MemoryUsage::CpuToGpu,
    });

    std::memcpy(stage_vb->map(), cpu.vertices.data(), vb_bytes);
    stage_vb->unmap();
    std::memcpy(stage_ib->map(), cpu.indices.data(), ib_bytes);
    stage_ib->unmap();

    // Device-local buffers.
    auto vertex_buf = device.create_buffer({
        .size_bytes   = vb_bytes,
        .usage        = crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst) |
                        crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex),
        .memory_usage = crd::rhi::MemoryUsage::GpuOnly,
    });
    auto index_buf = device.create_buffer({
        .size_bytes   = ib_bytes,
        .usage        = crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst) |
                        crd::rhi::enum_bits(crd::rhi::BufferUsage::Index),
        .memory_usage = crd::rhi::MemoryUsage::GpuOnly,
    });

    // Record and submit.
    auto cmd = device.create_command_buffer();
    cmd->begin();
    cmd->copy_buffer(*stage_vb, *vertex_buf, 0, 0, vb_bytes);
    cmd->copy_buffer(*stage_ib, *index_buf, 0, 0, ib_bytes);
    cmd->end();
    device.graphics_queue().submit_and_wait(*cmd);

    return GpuMesh{std::move(vertex_buf), std::move(index_buf)};
}

// ─── Phase 3.0 v1o2 — async GPU upload (ADR-0061 Layer 2) ─────────────────────
//
// Mirrors the synchronous variants above but submits with a Fence and returns
// an UploadHandle that the caller polls per frame. The staging buffers, the
// command buffer, and the fence are kept alive inside the handle until
// take_mesh() / take_texture() consumes the produced resource and the handle
// drops.

UploadHandle GpuUploader::upload_texture_async(const TextureResource& cpu, crd::rhi::Device& device)
{
    CRD_ASSERT(!cpu.mips.empty());

    crd::u64 total_bytes = 0;
    for (const auto& mip : cpu.mips)
    {
        total_bytes += static_cast<crd::u64>(mip.pixels.size());
    }
    CRD_ASSERT(total_bytes > 0);

    UploadHandle h;

    h.staging_a = device.create_buffer({
        .size_bytes   = total_bytes,
        .usage        = crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
        .memory_usage = crd::rhi::MemoryUsage::CpuToGpu,
    });
    if (h.staging_a == nullptr)
    {
        return UploadHandle{};
    }

    auto* ptr       = static_cast<crd::u8*>(h.staging_a->map());
    crd::u64 offset = 0;
    for (const auto& mip : cpu.mips)
    {
        std::memcpy(ptr + offset, mip.pixels.data(), mip.pixels.size());
        offset += static_cast<crd::u64>(mip.pixels.size());
    }
    h.staging_a->unmap();

    const auto& top = cpu.mips[0];
    h.pending_texture.image = device.create_image({
        .extent       = {top.width, top.height},
        .format       = texture_format_to_rhi(cpu.format),
        .usage        = crd::rhi::enum_bits(crd::rhi::ImageUsage::TransferDst) |
                        crd::rhi::enum_bits(crd::rhi::ImageUsage::Sampled),
        .mip_levels   = cpu.mip_count,
        .array_layers = 1,
    });
    if (h.pending_texture.image == nullptr)
    {
        return UploadHandle{};
    }

    crd::containers::Array<crd::rhi::BufferImageCopy> regions;
    offset = 0;
    for (crd::u32 m = 0; m < cpu.mip_count; ++m)
    {
        const auto& mip = cpu.mips[m];
        regions.push_back({
            .buffer_offset = offset,
            .mip_level     = m,
            .extent        = {mip.width, mip.height},
        });
        offset += static_cast<crd::u64>(mip.pixels.size());
    }

    h.cmd = device.create_command_buffer();
    if (h.cmd == nullptr)
    {
        return UploadHandle{};
    }
    h.cmd->begin();
    h.cmd->transition_image(*h.pending_texture.image,
                            crd::rhi::ImageAccess::Undefined, crd::rhi::ImageAccess::TransferDst);
    h.cmd->copy_buffer_to_image(*h.staging_a, *h.pending_texture.image,
                                crd::containers::as_const_span(regions));
    h.cmd->transition_image(*h.pending_texture.image,
                            crd::rhi::ImageAccess::TransferDst, crd::rhi::ImageAccess::ShaderRead);
    h.cmd->end();

    h.fence = device.create_fence();
    if (h.fence == nullptr)
    {
        return UploadHandle{};
    }
    device.graphics_queue().submit(*h.cmd, *h.fence);

    return h;
}

UploadHandle GpuUploader::upload_mesh_async(const MeshResource& cpu, crd::rhi::Device& device)
{
    CRD_ASSERT(!cpu.vertices.empty());
    CRD_ASSERT(!cpu.indices.empty());

    const crd::u64 vb_bytes = static_cast<crd::u64>(cpu.vertices.size());
    const crd::u64 ib_bytes = static_cast<crd::u64>(cpu.indices.size());

    UploadHandle h;

    h.staging_a = device.create_buffer({
        .size_bytes   = vb_bytes,
        .usage        = crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
        .memory_usage = crd::rhi::MemoryUsage::CpuToGpu,
    });
    h.staging_b = device.create_buffer({
        .size_bytes   = ib_bytes,
        .usage        = crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
        .memory_usage = crd::rhi::MemoryUsage::CpuToGpu,
    });
    if (h.staging_a == nullptr || h.staging_b == nullptr)
    {
        return UploadHandle{};
    }

    std::memcpy(h.staging_a->map(), cpu.vertices.data(), vb_bytes);
    h.staging_a->unmap();
    std::memcpy(h.staging_b->map(), cpu.indices.data(), ib_bytes);
    h.staging_b->unmap();

    h.pending_mesh.vertex_buffer = device.create_buffer({
        .size_bytes   = vb_bytes,
        .usage        = crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst) |
                        crd::rhi::enum_bits(crd::rhi::BufferUsage::Vertex),
        .memory_usage = crd::rhi::MemoryUsage::GpuOnly,
    });
    h.pending_mesh.index_buffer = device.create_buffer({
        .size_bytes   = ib_bytes,
        .usage        = crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst) |
                        crd::rhi::enum_bits(crd::rhi::BufferUsage::Index),
        .memory_usage = crd::rhi::MemoryUsage::GpuOnly,
    });
    if (h.pending_mesh.vertex_buffer == nullptr || h.pending_mesh.index_buffer == nullptr)
    {
        return UploadHandle{};
    }

    h.cmd = device.create_command_buffer();
    if (h.cmd == nullptr)
    {
        return UploadHandle{};
    }
    h.cmd->begin();
    h.cmd->copy_buffer(*h.staging_a, *h.pending_mesh.vertex_buffer, 0, 0, vb_bytes);
    h.cmd->copy_buffer(*h.staging_b, *h.pending_mesh.index_buffer,  0, 0, ib_bytes);
    h.cmd->end();

    h.fence = device.create_fence();
    if (h.fence == nullptr)
    {
        return UploadHandle{};
    }
    device.graphics_queue().submit(*h.cmd, *h.fence);

    return h;
}

} // namespace crd::renderer
