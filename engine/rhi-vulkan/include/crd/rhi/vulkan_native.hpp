#pragma once

#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/instance.hpp>
#include <crd/rhi/swapchain.hpp>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace crd::rhi
{
[[nodiscard]] VkInstance vulkan_instance(Instance& instance) noexcept;
[[nodiscard]] VkPhysicalDevice vulkan_physical_device(Device& device) noexcept;
[[nodiscard]] VkDevice vulkan_device(Device& device) noexcept;
[[nodiscard]] VkQueue vulkan_graphics_queue(Device& device) noexcept;
[[nodiscard]] crd::u32 vulkan_graphics_queue_family_index(Device& device) noexcept;
[[nodiscard]] VkCommandBuffer vulkan_command_buffer(CommandBuffer& command_buffer) noexcept;
[[nodiscard]] VkFormat vulkan_swapchain_color_format(Swapchain& swapchain) noexcept;
[[nodiscard]] crd::u32 vulkan_swapchain_image_count(Swapchain& swapchain) noexcept;
} // namespace crd::rhi
