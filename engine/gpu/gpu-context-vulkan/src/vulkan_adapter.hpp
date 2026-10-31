#pragma once

// DIAG.9a, HW-RESILIENCE.2: the Vulkan side of crd/gpu/adapter.hpp. Provider-private.

#include <crd/gpu/adapter.hpp>

#include <vulkan/vulkan.h>

namespace crd::gpu::detail
{
// `physical` as its properties report it. The driver version is VkPhysicalDeviceDriverProperties::driverInfo when the
// device reports it (Vulkan 1.2 or VK_KHR_driver_properties) and is not empty, otherwise driverVersion in its
// vendor's packing. Unknown for a null handle.
[[nodiscard]] GpuAdapter vk_adapter(VkPhysicalDevice physical) noexcept;
} // namespace crd::gpu::detail
