#pragma once

// crd-gpu-context-vulkan — the Vulkan `IGpuContext` (ADR-0099, v17-i-a). Creates a headless, compute-capable Vulkan
// device (instance / physical device / logical device with a compute queue + the cooperative-matrix / coopmat2 / fp16
// feature chain, all guarded by adapter support) with **no** surface/swapchain. `ComputeDevice` (v17-i-b) downcasts an
// `IGpuContext*` to `VulkanGpuContext*` to reach the handles. This is the compute foundation `kir-vulkan` will use
// instead of `crd::rhi::create_vulkan_instance` — the decoupling from the rendering RHI.

#include <crd/gpu/context.hpp>
#include <crd/gpu/program.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <vulkan/vulkan.h>

#include <memory>

namespace crd::gpu
{

// Concrete-Vulkan view of an IGpuProgram. A program retains its cooked SPIR-V so a backend consumer can build EITHER a
// compute pipeline (VkShaderModule) OR a shader object (`VkShaderEXT`, from the SPIR-V — D-008 C1-b raster). `IGpuProgram`
// stays opaque to portable code; the Vulkan raster/compute contexts downcast to reach `vk_spirv()`.
class VulkanGpuProgram : public IGpuProgram
{
public:
    [[nodiscard]] virtual crd::containers::ConstSpan<crd::u8> vk_spirv() const noexcept = 0;
    // D-008 C2-d: the compiled VkShaderModule, so rhi-vulkan's create_graphics_pipeline / create_compute_pipeline can take
    // an opaque IGpuProgram instead of a raw-SPIR-V ShaderModule (closes I2). Entry point is always "main" for our SPIR-V.
    [[nodiscard]] virtual VkShaderModule vk_module() const noexcept = 0;
};

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

    // D-008 C1: the converged device is graphics-capable too (ADR-0099 "one device, both concerns"). A GRAPHICS queue +
    // family (distinct from the async-compute queue on adapters that have a dedicated compute family) so `IRasterContext`
    // can draw. `VK_NULL_HANDLE` / UINT32_MAX + `graphics_capable()==false` on a compute-only adapter (CUDA-class). And
    // the frontier pipeline model: `shader_object()` reports `VK_EXT_shader_object` (dynamic rendering is always enabled).
    [[nodiscard]] virtual bool     graphics_capable() const noexcept = 0;
    [[nodiscard]] virtual VkQueue  graphics_queue() const noexcept   = 0;
    [[nodiscard]] virtual crd::u32 graphics_family() const noexcept  = 0;
    [[nodiscard]] virtual bool     shader_object() const noexcept    = 0; // VK_EXT_shader_object enabled?

    // D-008 C2-a: render-capable = the surface (instance) + `VK_KHR_swapchain` (device) extensions are enabled and a
    // graphics queue exists, so this ONE device can present (ADR-0099). false on a headless/compute context
    // (`GpuContextConfig::headless`). The device unification (C2) makes `rhi-vulkan` adopt a render-capable context.
    [[nodiscard]] virtual bool render_capable() const noexcept = 0;
};

// Create a headless Vulkan compute context per `config` (config.backend must be Vulkan). Returns nullptr on failure
// (no device, no compute queue). The returned object is a `VulkanGpuContext` behind the `IGpuContext` handle.
[[nodiscard]] std::unique_ptr<IGpuContext> create_vulkan_gpu_context(const GpuContextConfig& config);

// D-008 C2-d4: mint a `VulkanGpuProgram` from cooked SPIR-V given only a `VkDevice`. This is the ONE program constructor
// (ADR-0103: gpu-context-vulkan owns program authoring); `IGpuContext::create_program` and the standalone `rhi-vulkan`
// device (which holds a bare `VkDevice`, not a full context) both route here so `ShaderModule` can retire. Cooked SPIR-V
// crosses only as a parameter on this sanctioned mint path — never as a stored public-header field (I2). Returns nullptr
// on an invalid device or mis-sized bytecode. The returned program owns its `VkShaderModule` and must not outlive `device`.
[[nodiscard]] std::unique_ptr<IGpuProgram>
make_vulkan_program(VkDevice device, ShaderStage stage, crd::containers::ConstSpan<crd::u8> cooked);

} // namespace crd::gpu
