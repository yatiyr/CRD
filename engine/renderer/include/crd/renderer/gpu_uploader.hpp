#pragma once

#include <crd/renderer/mesh_resource.hpp>
#include <crd/renderer/texture_resource.hpp>
#include <crd/rhi/buffer.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/image.hpp>
#include <memory>

namespace crd::renderer
{

// GPU-resident texture (v1d: image only; sampler deferred to Phase 2.8).
struct GpuTexture
{
    std::unique_ptr<crd::rhi::Image> image;
};

// GPU-resident mesh: separate vertex and index device buffers.
struct GpuMesh
{
    std::unique_ptr<crd::rhi::Buffer> vertex_buffer;
    std::unique_ptr<crd::rhi::Buffer> index_buffer;
};

// Synchronous staging-buffer GPU upload helpers.
// Each call records a command buffer, submits it, and blocks until idle.
class GpuUploader
{
public:
    [[nodiscard]] static GpuTexture upload_texture(const TextureResource& cpu, crd::rhi::Device& device);
    [[nodiscard]] static GpuMesh    upload_mesh(const MeshResource& cpu, crd::rhi::Device& device);
};

} // namespace crd::renderer
