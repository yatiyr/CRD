#pragma once

#include <crd/renderer/mesh_resource.hpp>
#include <crd/renderer/texture_resource.hpp>
#include <crd/rhi/buffer.hpp>
#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/fence.hpp>
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

// Phase 3.0 v1o2 — async GPU upload (ADR-0061 Layer 2).
//
// UploadHandle owns the entire transient state of an in-flight upload:
//   - the Fence the queue submission was attached to (signals on completion);
//   - the CommandBuffer carrying the recorded copies;
//   - one or two staging Buffers (vertex/index for mesh; pixel staging for texture);
//   - the produced GpuMesh OR GpuTexture (move-out via take_*() once is_ready()).
//
// Move-only. Lifetime: caller holds an UploadHandle until is_ready() returns
// true, then take_mesh() / take_texture() to consume. Discarding an
// in-flight handle is safe — the destructor blocks via the device's idle
// path indirectly (the Fence destructor doesn't wait, but the Buffer / CmdBuf
// destructors will free their VkObjects safely once vkDeviceWaitIdle is
// called by the application's shutdown path; consumers should ensure the
// fence is signalled or wait_idle the device before letting handles drop).
//
// The struct is declared with public members so the RenderUploadSystem
// (and tests) can assemble synthetic handles without friend declarations.
// Production code uses GpuUploader::upload_*_async() which fills them.
class UploadHandle
{
public:
    UploadHandle() noexcept = default;
    UploadHandle(UploadHandle&&) noexcept = default;
    UploadHandle& operator=(UploadHandle&&) noexcept = default;
    UploadHandle(const UploadHandle&) = delete;
    UploadHandle& operator=(const UploadHandle&) = delete;
    ~UploadHandle() noexcept = default;

    [[nodiscard]] bool is_valid() const noexcept { return fence != nullptr; }

    // Non-blocking. Returns true once the GPU work has completed.
    [[nodiscard]] bool is_ready() const noexcept
    {
        return fence != nullptr && fence->is_signaled();
    }

    // Blocking wait on the underlying fence. No-op if the handle is empty.
    void wait()
    {
        if (fence != nullptr)
        {
            fence->wait();
        }
    }

    // Move-out the produced resource. Caller must check is_ready() first;
    // the called variant must match the upload kind (mesh vs texture).
    [[nodiscard]] GpuMesh    take_mesh()    noexcept { return std::move(pending_mesh); }
    [[nodiscard]] GpuTexture take_texture() noexcept { return std::move(pending_texture); }

    // Filled by GpuUploader during async upload; touched by tests + the
    // RenderUploadSystem. Documented as "engine-internal" in the header
    // comment above.
    std::unique_ptr<crd::rhi::Fence>          fence;
    std::unique_ptr<crd::rhi::CommandBuffer>  cmd;
    std::unique_ptr<crd::rhi::Buffer>         staging_a;  // mesh: vb staging | texture: pixel staging
    std::unique_ptr<crd::rhi::Buffer>         staging_b;  // mesh: ib staging | texture: unused
    GpuMesh                                   pending_mesh;
    GpuTexture                                pending_texture;
};

// Synchronous staging-buffer GPU upload helpers.
// Each call records a command buffer, submits it, and blocks until idle.
//
// The async siblings (Phase 3.0 v1o2) record commands, submit with a Fence,
// and return an UploadHandle without blocking. Caller polls is_ready() per
// frame and consumes via take_*() once the GPU finishes the copy.
class GpuUploader
{
public:
    [[nodiscard]] static GpuTexture upload_texture(const TextureResource& cpu, crd::rhi::Device& device);
    [[nodiscard]] static GpuMesh    upload_mesh(const MeshResource& cpu, crd::rhi::Device& device);

    [[nodiscard]] static UploadHandle upload_texture_async(const TextureResource& cpu, crd::rhi::Device& device);
    [[nodiscard]] static UploadHandle upload_mesh_async   (const MeshResource&    cpu, crd::rhi::Device& device);
};

} // namespace crd::renderer
