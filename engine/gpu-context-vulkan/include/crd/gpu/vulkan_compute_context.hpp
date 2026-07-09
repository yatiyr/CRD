#pragma once

// crd-gpu-context-vulkan — the VULKAN implementation of the one compute dispatch surface `crd::gpu::IComputeContext`
// (ADR-0100). Raw Vulkan over a VulkanGpuContext: command pool + descriptor pool + a SPIR-V-hashed pipeline cache; a
// multi-pass copy/barrier/dispatch recorder; dispatches on the compute queue, no rendering. Consumers depend on the
// ABSTRACT `IComputeContext` (crd/gpu/compute.hpp); only the composition layer names this concrete backend.

#include <crd/gpu/compute.hpp>
#include <crd/gpu/vulkan_context.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <memory>

namespace crd::memory
{
class IAllocator;
}

namespace crd::gpu
{

class VulkanComputeContext final : public IComputeContext
{
public:
    VulkanComputeContext(VulkanGpuContext& ctx, crd::memory::IAllocator* alloc);
    ~VulkanComputeContext() override;
    VulkanComputeContext(const VulkanComputeContext&)            = delete;
    VulkanComputeContext& operator=(const VulkanComputeContext&) = delete;
    VulkanComputeContext(VulkanComputeContext&&)                 = delete;
    VulkanComputeContext& operator=(VulkanComputeContext&&)      = delete;

    [[nodiscard]] bool valid() const noexcept override;
    [[nodiscard]] bool supports_shader_int64() const noexcept override;

    [[nodiscard]] std::unique_ptr<ComputeBuffer> create_buffer(crd::u64 bytes, crd::u32 usage, ComputeMemory memory) override;

    // Loads the cooked `<shader_dir>/<name>.comp.spv` and caches the pipeline (n storage-buffer bindings + push_size push).
    [[nodiscard]] std::unique_ptr<ComputePipeline> create_pipeline(crd::containers::StringView shader_dir,
                                                                   crd::containers::StringView name, int n_bindings,
                                                                   crd::u32 push_size) override;

    // Vulkan-specific: a pipeline directly from pre-compiled SPIR-V. For kir-vulkan's runtime-compiled kernels — NOT on
    // the backend-agnostic IComputeContext (SPIR-V is a Vulkan format; only the Vulkan backend consumes it).
    [[nodiscard]] std::unique_ptr<ComputePipeline> create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8> spirv,
                                                                              int n_bindings, crd::u32 push_size);

    [[nodiscard]] ComputeRecorder& begin() override;
    void                           submit_and_wait() override;

    // GPU-only execution time (ms) of the command buffer from the most recent submit_and_wait — measured by device
    // timestamps bracketing the recorded work, so it EXCLUDES CPU record/submit overhead. For fair kernel benchmarking.
    [[nodiscard]] double last_gpu_ms() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::gpu
