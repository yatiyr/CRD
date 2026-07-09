#pragma once

// crd-gpu-context-vulkan — the Vulkan `IGpuContext` (ADR-0099, v17-i-a). Creates a headless, compute-capable Vulkan
// device (instance / physical device / logical device with a compute queue + the cooperative-matrix / coopmat2 / fp16
// feature chain, all guarded by adapter support) with **no** surface/swapchain. `ComputeDevice` (v17-i-b) downcasts an
// `IGpuContext*` to `VulkanGpuContext*` to reach the handles. This is the compute foundation `kir-vulkan` will use
// instead of `crd::rhi::create_vulkan_instance` — the decoupling from the rendering RHI.

#include <crd/gpu/context.hpp>

#include <vulkan/vulkan.h>

#include <memory>

namespace crd::gpu
{

// Concrete-Vulkan view of an IGpuContext. `backend() == GpuBackend::Vulkan` guarantees a safe downcast.
class VulkanGpuContext : public IGpuContext
{
public:
    [[nodiscard]] virtual VkInstance       vk_instance() const noexcept        = 0;
    [[nodiscard]] virtual VkPhysicalDevice vk_physical_device() const noexcept = 0;
    [[nodiscard]] virtual VkDevice         vk_device() const noexcept          = 0;
    [[nodiscard]] virtual VkQueue          compute_queue() const noexcept      = 0; // dedicated compute if present, else graphics
    [[nodiscard]] virtual crd::u32         compute_family() const noexcept     = 0;
    [[nodiscard]] virtual bool             cooperative_matrix2() const noexcept = 0; // VK_NV_cooperative_matrix2 enabled?
    [[nodiscard]] virtual bool             shader_int64() const noexcept        = 0; // shaderInt64 enabled (geometry 60-bit)?
};

// Create a headless Vulkan compute context per `config` (config.backend must be Vulkan). Returns nullptr on failure
// (no device, no compute queue). The returned object is a `VulkanGpuContext` behind the `IGpuContext` handle.
[[nodiscard]] std::unique_ptr<IGpuContext> create_vulkan_gpu_context(const GpuContextConfig& config);

} // namespace crd::gpu
