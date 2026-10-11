#pragma once

// DIAG.10c: the Vulkan side of device and host pressure evidence (crd/gpu/device_pressure.hpp). Provider-private.

#include <crd/gpu/device_pressure.hpp>

#include <vulkan/vulkan.h>

namespace crd::gpu::detail
{
// `physical`'s memory as VK_EXT_memory_budget reports it for this process: the device-local heaps summed into `local`,
// the other heaps into `non_local` (a device with no such heap leaves it unread). `budget_enabled` says the device was
// created with the extension; without it the reading is unavailable, never zero use. Valid after the device is lost
// (the query is a physical-device query).
[[nodiscard]] DeviceMemoryReading vk_device_memory(VkPhysicalDevice physical, bool budget_enabled) noexcept;

// Called once a device is created: the seam reads `physical`'s memory when that device is first recorded lost.
void vk_register_device_memory(VkDevice device, VkPhysicalDevice physical, bool budget_enabled) noexcept;

// The pressure taken when the most recent device was first recorded lost; its loss_sequence is that failure's
// sequence (VkDeviceFailure::sequence). `taken` is false before any loss.
[[nodiscard]] GpuLossPressure vk_last_loss_pressure() noexcept;
} // namespace crd::gpu::detail
