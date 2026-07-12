#pragma once

#include <crd/containers/array.hpp>
#include <crd/rhi/types.hpp>

namespace crd::rhi
{
// D-008 C2-f (ADR-0099/0103): the Instance enumerates adapters only. DEVICE creation moved to the gpu-context — a
// `crd::rhi::Device` is minted by `create_vulkan_device_adopting(crd::gpu::IGpuContext&)`, so there is ONE VkDevice
// owner (the `VulkanGpuContext`) and rhi-vulkan never creates its own. `Instance::create_device` was retired here.
class Instance
{
public:
    virtual ~Instance() = default;

    [[nodiscard]] virtual BackendApi api() const noexcept = 0;
    virtual void enumerate_adapters(crd::containers::Array<AdapterInfo>& out) const = 0;
};
} // namespace crd::rhi
