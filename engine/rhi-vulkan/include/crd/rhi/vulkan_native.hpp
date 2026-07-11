#pragma once

#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/instance.hpp>
#include <crd/rhi/swapchain.hpp>

// In GLFW 3.4, GLFW_INCLUDE_VULKAN does NOT suppress GL/gl.h; only GLFW_INCLUDE_NONE does.
// Include Vulkan first so GLFW sees VK_VERSION_1_0 and declares glfwCreateWindowSurface.
#include <vulkan/vulkan.h>
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

namespace crd::rhi
{
[[nodiscard]] VkInstance vulkan_instance(Instance& instance) noexcept;
[[nodiscard]] VkInstance vulkan_instance(Device& device) noexcept; // D-008 C2-c2: instance from a device (adopted path)
[[nodiscard]] VkPhysicalDevice vulkan_physical_device(Device& device) noexcept;
[[nodiscard]] VkDevice vulkan_device(Device& device) noexcept;
[[nodiscard]] VkQueue vulkan_graphics_queue(Device& device) noexcept;
[[nodiscard]] crd::u32 vulkan_graphics_queue_family_index(Device& device) noexcept;
[[nodiscard]] VkCommandBuffer vulkan_command_buffer(CommandBuffer& command_buffer) noexcept;
[[nodiscard]] VkFormat vulkan_swapchain_color_format(Swapchain& swapchain) noexcept;
[[nodiscard]] crd::u32 vulkan_swapchain_image_count(Swapchain& swapchain) noexcept;
} // namespace crd::rhi
