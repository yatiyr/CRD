#include "log_channel.hpp"

#include <crd/log/log.hpp>
#include <crd/rhi/vulkan_backend.hpp>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>
#include <memory>

namespace crd::rhi
{
namespace
{
[[nodiscard]] bool vk_ok(VkResult result, const char* what) noexcept
{
    if (result == VK_SUCCESS)
    {
        return true;
    }
    CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "{} failed with VkResult={}", what, static_cast<int>(result));
    return false;
}

[[nodiscard]] VkFormat to_vk_format(Format format) noexcept
{
    switch (format)
    {
        case Format::R8G8B8A8Unorm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::B8G8R8A8Unorm:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::D24UnormS8Uint:
            return VK_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32Sfloat:
            return VK_FORMAT_D32_SFLOAT;
        case Format::Undefined:
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

[[nodiscard]] VkAttachmentLoadOp to_vk_load_op(LoadOp op) noexcept
{
    switch (op)
    {
        case LoadOp::Load:
            return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadOp::Clear:
            return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::DontCare:
        default:
            return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
}

[[nodiscard]] VkAttachmentStoreOp to_vk_store_op(StoreOp op) noexcept
{
    switch (op)
    {
        case StoreOp::Store:
            return VK_ATTACHMENT_STORE_OP_STORE;
        case StoreOp::DontCare:
        default:
            return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
}

[[nodiscard]] bool has_extension(crd::containers::ConstSpan<VkExtensionProperties> exts, const char* name) noexcept
{
    for (const auto& ext : exts)
    {
        if (std::strcmp(ext.extensionName, name) == 0)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_layer(crd::containers::ConstSpan<VkLayerProperties> layers, const char* name) noexcept
{
    for (const auto& layer : layers)
    {
        if (std::strcmp(layer.layerName, name) == 0)
        {
            return true;
        }
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
                                              void* /*user_data*/)
{
    const char* message =
        callback_data != nullptr && callback_data->pMessage != nullptr ? callback_data->pMessage : "(null)";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
    {
        CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "{}", message);
    }
    else
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "{}", message);
    }
    return VK_FALSE;
}

struct FrameSync
{
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkSemaphore render_finished = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
};

class VulkanImage final : public Image
{
public:
    VulkanImage(VkDevice device, ImageDesc desc, VkImage image, VkImageView image_view)
        : m_device(device), m_desc(std::move(desc)), m_image(image), m_image_view(image_view)
    {
    }

    ~VulkanImage() noexcept override
    {
        if (m_image_view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, m_image_view, nullptr);
            m_image_view = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] const ImageDesc& desc() const noexcept override { return m_desc; }
    [[nodiscard]] VkImage handle() const noexcept { return m_image; }
    [[nodiscard]] VkImageView image_view() const noexcept { return m_image_view; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    ImageDesc m_desc{};
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_image_view = VK_NULL_HANDLE;
};

class VulkanSwapchain;

class VulkanCommandBuffer final : public CommandBuffer
{
public:
    VulkanCommandBuffer(VkDevice device, VkCommandPool command_pool, VkCommandBuffer command_buffer,
                        bool /*sync2_enabled*/)
        : m_device(device), m_command_pool(command_pool), m_command_buffer(command_buffer)
    {
    }

    ~VulkanCommandBuffer() noexcept override
    {
        if (m_command_buffer != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(m_device, m_command_pool, 1, &m_command_buffer);
            m_command_buffer = VK_NULL_HANDLE;
        }
    }

    void begin() override
    {
        CRD_ASSERT(vkResetCommandBuffer(m_command_buffer, 0) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CRD_ASSERT(vkBeginCommandBuffer(m_command_buffer, &begin_info) == VK_SUCCESS);
        m_render_target = nullptr;
    }

    void end() override { CRD_ASSERT(vkEndCommandBuffer(m_command_buffer) == VK_SUCCESS); }

    void reset() override
    {
        CRD_ASSERT(vkResetCommandBuffer(m_command_buffer, 0) == VK_SUCCESS);
        m_render_target = nullptr;
    }

    void begin_rendering(const RenderingInfo& info) override
    {
        auto* image = dynamic_cast<VulkanImage*>(info.color_attachment.image);
        CRD_ASSERT(image != nullptr);
        m_render_target = image;

        transition_to_color_attachment(*image);

        VkRenderingAttachmentInfo color_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color_attachment.imageView = image->image_view();
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = to_vk_load_op(info.color_attachment.load_op);
        color_attachment.storeOp = to_vk_store_op(info.color_attachment.store_op);
        color_attachment.clearValue.color = {{info.color_attachment.clear_color.r, info.color_attachment.clear_color.g,
                                              info.color_attachment.clear_color.b,
                                              info.color_attachment.clear_color.a}};

        VkRenderingInfo rendering_info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering_info.renderArea.offset = {0, 0};
        rendering_info.renderArea.extent = {info.extent.width, info.extent.height};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;
        vkCmdBeginRendering(m_command_buffer, &rendering_info);
    }

    void end_rendering() override
    {
        vkCmdEndRendering(m_command_buffer);
        CRD_ASSERT(m_render_target != nullptr);
        transition_to_present(*m_render_target);
        m_render_target = nullptr;
    }

    void bind_pipeline(Pipeline& /*pipeline*/) override {}

    void bind_vertex_buffer(Buffer& /*buffer*/, crd::u64 /*offset_bytes*/) override {}

    void draw(crd::u32 /*vertex_count*/, crd::u32 /*first_vertex*/) override {}

    [[nodiscard]] VkCommandBuffer handle() const noexcept { return m_command_buffer; }

private:
    void transition_to_color_attachment(VulkanImage& image)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.image = image.handle();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(m_command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void transition_to_present(VulkanImage& image)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.image = image.handle();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(m_command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkCommandPool m_command_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_command_buffer = VK_NULL_HANDLE;
    VulkanImage* m_render_target = nullptr;
};

class VulkanSwapchain final : public Swapchain
{
public:
    VulkanSwapchain(VkDevice device, VkSwapchainKHR swapchain, SwapchainDesc desc, crd::u32 frames_in_flight,
                    crd::containers::Array<std::unique_ptr<VulkanImage>> images)
        : m_device(device), m_swapchain(swapchain), m_desc(std::move(desc)), m_frames_in_flight(frames_in_flight),
          m_images(std::move(images))
    {
        CRD_ASSERT(create_frame_sync_objects());
    }

    ~VulkanSwapchain() noexcept override
    {
        for (auto& frame : m_frames)
        {
            if (frame.image_available != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_device, frame.image_available, nullptr);
            }
            if (frame.render_finished != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_device, frame.render_finished, nullptr);
            }
            if (frame.in_flight != VK_NULL_HANDLE)
            {
                vkDestroyFence(m_device, frame.in_flight, nullptr);
            }
        }
        if (m_swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] const SwapchainDesc& desc() const noexcept override { return m_desc; }

    [[nodiscard]] bool acquire_next_image() override
    {
        FrameSync& frame = current_frame_sync();
        if (!vk_ok(vkWaitForFences(m_device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX), "vkWaitForFences"))
        {
            return false;
        }
        if (!vk_ok(vkResetFences(m_device, 1, &frame.in_flight), "vkResetFences"))
        {
            return false;
        }

        const VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, frame.image_available,
                                                      VK_NULL_HANDLE, &m_current_image_index);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        {
            m_image_acquired = true;
            return true;
        }

        CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "vkAcquireNextImageKHR failed with VkResult={}",
                      static_cast<int>(result));
        return false;
    }

    [[nodiscard]] crd::u32 current_image_index() const noexcept override { return m_current_image_index; }
    [[nodiscard]] Image& current_image() noexcept override { return *m_images[m_current_image_index]; }
    [[nodiscard]] VkSwapchainKHR handle() const noexcept { return m_swapchain; }

    [[nodiscard]] FrameSync& current_frame_sync() noexcept { return m_frames[m_frame_index]; }
    [[nodiscard]] crd::u32 frame_index() const noexcept { return m_frame_index; }
    void advance_frame() noexcept { m_frame_index = (m_frame_index + 1u) % m_frames_in_flight; }
    [[nodiscard]] bool image_acquired() const noexcept { return m_image_acquired; }
    void clear_image_acquired() noexcept { m_image_acquired = false; }

private:
    [[nodiscard]] bool create_frame_sync_objects()
    {
        m_frames.resize(m_frames_in_flight);
        for (auto& frame : m_frames)
        {
            VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (!vk_ok(vkCreateSemaphore(m_device, &semaphore_info, nullptr, &frame.image_available),
                       "vkCreateSemaphore(image_available)"))
            {
                return false;
            }
            if (!vk_ok(vkCreateSemaphore(m_device, &semaphore_info, nullptr, &frame.render_finished),
                       "vkCreateSemaphore(render_finished)"))
            {
                return false;
            }
            if (!vk_ok(vkCreateFence(m_device, &fence_info, nullptr, &frame.in_flight), "vkCreateFence"))
            {
                return false;
            }
        }
        return true;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    SwapchainDesc m_desc{};
    crd::u32 m_frames_in_flight = 2;
    crd::containers::Array<FrameSync> m_frames{};
    crd::containers::Array<std::unique_ptr<VulkanImage>> m_images{};
    crd::u32 m_current_image_index = 0;
    crd::u32 m_frame_index = 0;
    bool m_image_acquired = false;
};

class VulkanQueue final : public Queue
{
public:
    VulkanQueue(VkQueue queue, bool /*sync2_enabled*/) : m_queue(queue) {}

    [[nodiscard]] bool submit(CommandBuffer& command_buffer, Swapchain& swapchain) override
    {
        auto* vk_command_buffer = dynamic_cast<VulkanCommandBuffer*>(&command_buffer);
        auto* vk_swapchain = dynamic_cast<VulkanSwapchain*>(&swapchain);
        if (vk_command_buffer == nullptr || vk_swapchain == nullptr)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Queue::submit received incompatible backend objects");
            return false;
        }
        if (!vk_swapchain->image_acquired())
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Queue::submit called before acquire_next_image");
            return false;
        }

        FrameSync& frame = vk_swapchain->current_frame_sync();
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &frame.image_available;
        submit_info.pWaitDstStageMask = &wait_stage;
        VkCommandBuffer handle = vk_command_buffer->handle();
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &handle;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &frame.render_finished;
        return vk_ok(vkQueueSubmit(m_queue, 1, &submit_info, frame.in_flight), "vkQueueSubmit");
    }

    void present(Swapchain& swapchain) override
    {
        auto* vk_swapchain = dynamic_cast<VulkanSwapchain*>(&swapchain);
        if (vk_swapchain == nullptr)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Queue::present received incompatible swapchain");
            return;
        }

        FrameSync& frame = vk_swapchain->current_frame_sync();
        VkSwapchainKHR swapchain_handle = vk_swapchain->handle();

        VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &frame.render_finished;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain_handle;
        crd::u32 image_index = vk_swapchain->current_image_index();
        present_info.pImageIndices = &image_index;

        const VkResult result = vkQueuePresentKHR(m_queue, &present_info);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "vkQueuePresentKHR failed with VkResult={}",
                          static_cast<int>(result));
        }
        vk_swapchain->clear_image_acquired();
        vk_swapchain->advance_frame();
    }

    void wait_idle() override
    {
        if (m_queue != VK_NULL_HANDLE)
        {
            vkQueueWaitIdle(m_queue);
        }
    }

private:
    VkQueue m_queue = VK_NULL_HANDLE;
};

class VulkanDevice final : public Device
{
public:
    VulkanDevice(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, crd::u32 graphics_family_index,
                 DeviceDesc desc, bool sync2_enabled, bool dynamic_rendering_enabled)
        : m_instance(instance), m_physical_device(physical_device), m_device(device),
          m_graphics_family_index(graphics_family_index), m_desc(std::move(desc)), m_sync2_enabled(sync2_enabled),
          m_dynamic_rendering_enabled(dynamic_rendering_enabled)
    {
        vkGetDeviceQueue(m_device, m_graphics_family_index, 0, &m_graphics_queue_handle);
        m_graphics_queue = std::make_unique<VulkanQueue>(m_graphics_queue_handle, m_sync2_enabled);

        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = m_graphics_family_index;
        CRD_ASSERT(vkCreateCommandPool(m_device, &pool_info, nullptr, &m_command_pool) == VK_SUCCESS);
    }

    ~VulkanDevice() noexcept override
    {
        if (m_command_pool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(m_device, m_command_pool, nullptr);
            m_command_pool = VK_NULL_HANDLE;
        }
        if (m_device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_device);
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
        if (m_surface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] BackendApi api() const noexcept override { return BackendApi::Vulkan; }

    [[nodiscard]] std::unique_ptr<Swapchain> create_swapchain(const SwapchainDesc& desc) override
    {
        if (m_device == VK_NULL_HANDLE)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "create_swapchain called on invalid VulkanDevice");
            return nullptr;
        }

        if (m_surface == VK_NULL_HANDLE)
        {
            if (desc.native_window_handle == nullptr)
            {
                CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "SwapchainDesc.native_window_handle was null");
                return nullptr;
            }

            const VkResult surface_result = glfwCreateWindowSurface(
                m_instance, static_cast<GLFWwindow*>(desc.native_window_handle), nullptr, &m_surface);
            if (!vk_ok(surface_result, "glfwCreateWindowSurface"))
            {
                return nullptr;
            }

            VkBool32 present_supported = VK_FALSE;
            if (!vk_ok(vkGetPhysicalDeviceSurfaceSupportKHR(m_physical_device, m_graphics_family_index, m_surface,
                                                            &present_supported),
                       "vkGetPhysicalDeviceSurfaceSupportKHR") ||
                present_supported != VK_TRUE)
            {
                CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Selected graphics queue family cannot present to the surface");
                return nullptr;
            }
        }

        VkSurfaceCapabilitiesKHR capabilities{};
        if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical_device, m_surface, &capabilities),
                   "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        {
            return nullptr;
        }

        crd::u32 format_count = 0;
        if (!vk_ok(vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &format_count, nullptr),
                   "vkGetPhysicalDeviceSurfaceFormatsKHR(count)"))
        {
            return nullptr;
        }
        if (format_count == 0)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Surface reported zero supported formats");
            return nullptr;
        }

        crd::containers::Array<VkSurfaceFormatKHR> formats;
        formats.resize(format_count);
        if (!vk_ok(vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &format_count, formats.data()),
                   "vkGetPhysicalDeviceSurfaceFormatsKHR(data)"))
        {
            return nullptr;
        }

        crd::u32 present_mode_count = 0;
        if (!vk_ok(
                vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical_device, m_surface, &present_mode_count, nullptr),
                "vkGetPhysicalDeviceSurfacePresentModesKHR(count)"))
        {
            return nullptr;
        }

        crd::containers::Array<VkPresentModeKHR> present_modes;
        present_modes.resize(present_mode_count);
        if (!vk_ok(vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical_device, m_surface, &present_mode_count,
                                                             present_modes.data()),
                   "vkGetPhysicalDeviceSurfacePresentModesKHR(data)"))
        {
            return nullptr;
        }

        VkSurfaceFormatKHR chosen_format = formats[0];
        for (const auto& candidate : formats)
        {
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                chosen_format = candidate;
                break;
            }
        }

        VkPresentModeKHR chosen_present_mode = VK_PRESENT_MODE_FIFO_KHR;
        for (const auto& candidate : present_modes)
        {
            if (desc.present_mode == PresentMode::Mailbox && candidate == VK_PRESENT_MODE_MAILBOX_KHR)
            {
                chosen_present_mode = candidate;
                break;
            }
            if (desc.present_mode == PresentMode::Immediate && candidate == VK_PRESENT_MODE_IMMEDIATE_KHR)
            {
                chosen_present_mode = candidate;
                break;
            }
        }

        VkExtent2D chosen_extent{};
        if (capabilities.currentExtent.width != UINT32_MAX)
        {
            chosen_extent = capabilities.currentExtent;
        }
        else
        {
            chosen_extent.width =
                std::clamp(desc.extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            chosen_extent.height =
                std::clamp(desc.extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }

        crd::u32 image_count = desc.image_count;
        image_count = std::max(image_count, capabilities.minImageCount);
        if (capabilities.maxImageCount > 0)
        {
            image_count = std::min(image_count, capabilities.maxImageCount);
        }

        VkSwapchainCreateInfoKHR create_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create_info.surface = m_surface;
        create_info.minImageCount = image_count;
        create_info.imageFormat = chosen_format.format;
        create_info.imageColorSpace = chosen_format.colorSpace;
        create_info.imageExtent = chosen_extent;
        create_info.imageArrayLayers = 1;
        create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info.preTransform = capabilities.currentTransform;
        create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create_info.presentMode = chosen_present_mode;
        create_info.clipped = VK_TRUE;

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateSwapchainKHR(m_device, &create_info, nullptr, &swapchain), "vkCreateSwapchainKHR"))
        {
            return nullptr;
        }

        crd::u32 swapchain_image_count = 0;
        if (!vk_ok(vkGetSwapchainImagesKHR(m_device, swapchain, &swapchain_image_count, nullptr),
                   "vkGetSwapchainImagesKHR(count)"))
        {
            vkDestroySwapchainKHR(m_device, swapchain, nullptr);
            return nullptr;
        }

        crd::containers::Array<VkImage> images;
        images.resize(swapchain_image_count);
        if (!vk_ok(vkGetSwapchainImagesKHR(m_device, swapchain, &swapchain_image_count, images.data()),
                   "vkGetSwapchainImagesKHR(data)"))
        {
            vkDestroySwapchainKHR(m_device, swapchain, nullptr);
            return nullptr;
        }

        crd::containers::Array<std::unique_ptr<VulkanImage>> wrapped_images;
        for (const VkImage image : images)
        {
            VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = chosen_format.format;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.baseMipLevel = 0;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.baseArrayLayer = 0;
            view_info.subresourceRange.layerCount = 1;

            VkImageView view = VK_NULL_HANDLE;
            if (!vk_ok(vkCreateImageView(m_device, &view_info, nullptr, &view), "vkCreateImageView"))
            {
                vkDestroySwapchainKHR(m_device, swapchain, nullptr);
                return nullptr;
            }

            wrapped_images.push_back(
                std::make_unique<VulkanImage>(m_device,
                                              ImageDesc{{chosen_extent.width, chosen_extent.height},
                                                        Format::B8G8R8A8Unorm,
                                                        enum_bits(ImageUsage::ColorAttachment),
                                                        1,
                                                        1},
                                              image, view));
        }

        SwapchainDesc resolved_desc = desc;
        resolved_desc.extent = {chosen_extent.width, chosen_extent.height};
        resolved_desc.color_format = Format::B8G8R8A8Unorm;
        return std::make_unique<VulkanSwapchain>(m_device, swapchain, std::move(resolved_desc), m_desc.frames_in_flight,
                                                 std::move(wrapped_images));
    }

    [[nodiscard]] std::unique_ptr<Buffer> create_buffer(const BufferDesc& /*desc*/) override
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "create_buffer not implemented in frame-sync slice");
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<Image> create_image(const ImageDesc& /*desc*/) override
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "create_image not implemented in frame-sync slice");
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<ShaderModule> create_shader_module(const ShaderModuleDesc& /*desc*/) override
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "create_shader_module not implemented in frame-sync slice");
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<Pipeline> create_graphics_pipeline(const GraphicsPipelineDesc& /*desc*/) override
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "create_graphics_pipeline not implemented in frame-sync slice");
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<CommandBuffer> create_command_buffer() override
    {
        if (!m_dynamic_rendering_enabled)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan,
                          "Dynamic rendering is unavailable; command buffer path is disabled");
            return nullptr;
        }

        VkCommandBufferAllocateInfo alloc_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc_info.commandPool = m_command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        if (!vk_ok(vkAllocateCommandBuffers(m_device, &alloc_info, &command_buffer), "vkAllocateCommandBuffers"))
        {
            return nullptr;
        }

        return std::make_unique<VulkanCommandBuffer>(m_device, m_command_pool, command_buffer, m_sync2_enabled);
    }

    [[nodiscard]] Queue& graphics_queue() noexcept override { return *m_graphics_queue; }

    void wait_idle() override
    {
        if (m_device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_device);
        }
    }

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkCommandPool m_command_pool = VK_NULL_HANDLE;
    crd::u32 m_graphics_family_index = 0;
    DeviceDesc m_desc{};
    bool m_sync2_enabled = false;
    bool m_dynamic_rendering_enabled = false;
    VkQueue m_graphics_queue_handle = VK_NULL_HANDLE;
    std::unique_ptr<VulkanQueue> m_graphics_queue{};
};

class VulkanInstance final : public Instance
{
public:
    explicit VulkanInstance(const InstanceDesc& desc)
    {
        crd::u32 glfw_extension_count = 0;
        const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);

        crd::containers::Array<const char*> enabled_extensions;
        if (glfw_extensions != nullptr)
        {
            for (crd::u32 i = 0; i < glfw_extension_count; ++i)
            {
                enabled_extensions.push_back(glfw_extensions[i]);
            }
        }
        else
        {
            CRD_LOG_WARN(detail::g_log_rhi_vulkan,
                         "GLFW is not initialised; creating Vulkan instance without window-system extensions");
        }

        crd::u32 instance_extension_count = 0;
        if (!vk_ok(vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, nullptr),
                   "vkEnumerateInstanceExtensionProperties(count)"))
        {
            return;
        }
        crd::containers::Array<VkExtensionProperties> instance_extensions;
        instance_extensions.resize(instance_extension_count);
        if (!vk_ok(
                vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, instance_extensions.data()),
                "vkEnumerateInstanceExtensionProperties(data)"))
        {
            return;
        }

        if (desc.enable_validation &&
            has_extension(crd::containers::as_const_span(instance_extensions), VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            enabled_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            m_validation_enabled = true;
        }

        crd::u32 layer_count = 0;
        if (!vk_ok(vkEnumerateInstanceLayerProperties(&layer_count, nullptr),
                   "vkEnumerateInstanceLayerProperties(count)"))
        {
            return;
        }
        crd::containers::Array<VkLayerProperties> layers;
        layers.resize(layer_count);
        if (!vk_ok(vkEnumerateInstanceLayerProperties(&layer_count, layers.data()),
                   "vkEnumerateInstanceLayerProperties(data)"))
        {
            return;
        }

        crd::containers::Array<const char*> enabled_layers;
        if (desc.enable_validation && has_layer(crd::containers::as_const_span(layers), "VK_LAYER_KHRONOS_validation"))
        {
            enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
        }
        else if (desc.enable_validation)
        {
            CRD_LOG_WARN(detail::g_log_rhi_vulkan,
                         "Validation requested but VK_LAYER_KHRONOS_validation is unavailable");
            m_validation_enabled = false;
        }

        VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app_info.pApplicationName = desc.application_name.c_str();
        app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.pEngineName = "Cerid";
        app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount = static_cast<crd::u32>(enabled_extensions.size());
        create_info.ppEnabledExtensionNames = enabled_extensions.data();
        create_info.enabledLayerCount = static_cast<crd::u32>(enabled_layers.size());
        create_info.ppEnabledLayerNames = enabled_layers.data();

        if (!vk_ok(vkCreateInstance(&create_info, nullptr, &m_instance), "vkCreateInstance"))
        {
            return;
        }

        if (m_validation_enabled)
        {
            VkDebugUtilsMessengerCreateInfoEXT debug_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            debug_info.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug_info.pfnUserCallback = &debug_callback;

            const auto create_debug_utils_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
            if (create_debug_utils_messenger != nullptr)
            {
                if (!vk_ok(create_debug_utils_messenger(m_instance, &debug_info, nullptr, &m_debug_messenger),
                           "vkCreateDebugUtilsMessengerEXT"))
                {
                    return;
                }
            }
        }

        m_valid = true;
    }

    ~VulkanInstance() noexcept override
    {
        if (m_debug_messenger != VK_NULL_HANDLE)
        {
            const auto destroy_debug_utils_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_debug_utils_messenger != nullptr)
            {
                destroy_debug_utils_messenger(m_instance, m_debug_messenger, nullptr);
            }
            m_debug_messenger = VK_NULL_HANDLE;
        }

        if (m_instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] BackendApi api() const noexcept override { return BackendApi::Vulkan; }

    void enumerate_adapters(crd::containers::Array<AdapterInfo>& out) const override
    {
        if (!m_valid || m_instance == VK_NULL_HANDLE)
        {
            return;
        }

        crd::u32 device_count = 0;
        if (!vk_ok(vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr), "vkEnumeratePhysicalDevices(count)"))
        {
            return;
        }

        crd::containers::Array<VkPhysicalDevice> devices;
        devices.resize(device_count);
        if (!vk_ok(vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data()),
                   "vkEnumeratePhysicalDevices(data)"))
        {
            return;
        }

        for (const VkPhysicalDevice device : devices)
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(device, &props);

            AdapterType type = AdapterType::Other;
            switch (props.deviceType)
            {
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    type = AdapterType::IntegratedGpu;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    type = AdapterType::DiscreteGpu;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                    type = AdapterType::VirtualGpu;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:
                    type = AdapterType::Cpu;
                    break;
                default:
                    break;
            }

            out.push_back(AdapterInfo{crd::containers::String(props.deviceName), type, 0, true, true});
        }
    }

    [[nodiscard]] std::unique_ptr<Device> create_device(const DeviceDesc& desc) override
    {
        if (!m_valid || m_instance == VK_NULL_HANDLE)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "create_device called on invalid VulkanInstance");
            return nullptr;
        }

        crd::u32 physical_device_count = 0;
        if (!vk_ok(vkEnumeratePhysicalDevices(m_instance, &physical_device_count, nullptr),
                   "vkEnumeratePhysicalDevices(count)"))
        {
            return nullptr;
        }

        crd::containers::Array<VkPhysicalDevice> physical_devices;
        physical_devices.resize(physical_device_count);
        if (!vk_ok(vkEnumeratePhysicalDevices(m_instance, &physical_device_count, physical_devices.data()),
                   "vkEnumeratePhysicalDevices(data)"))
        {
            return nullptr;
        }

        if (desc.preferred_adapter_index >= physical_devices.size())
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Preferred adapter index {} is out of range (adapter count={})",
                          desc.preferred_adapter_index, physical_devices.size());
            return nullptr;
        }

        const VkPhysicalDevice physical_device = physical_devices[desc.preferred_adapter_index];

        VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        VkPhysicalDeviceSynchronization2Features synchronization2_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        dynamic_rendering_features.pNext = &synchronization2_features;
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &dynamic_rendering_features;
        vkGetPhysicalDeviceFeatures2(physical_device, &features2);

        const bool dynamic_rendering_supported = dynamic_rendering_features.dynamicRendering == VK_TRUE;
        const bool sync2_supported = synchronization2_features.synchronization2 == VK_TRUE;
        if (!dynamic_rendering_supported)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "Selected adapter does not support dynamic rendering");
            return nullptr;
        }

        crd::u32 queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
        crd::containers::Array<VkQueueFamilyProperties> queue_families;
        queue_families.resize(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());

        crd::u32 graphics_family_index = UINT32_MAX;
        for (crd::u32 i = 0; i < queue_family_count; ++i)
        {
            if ((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
            {
                graphics_family_index = i;
                break;
            }
        }
        if (graphics_family_index == UINT32_MAX)
        {
            CRD_LOG_ERROR(detail::g_log_rhi_vulkan, "No graphics queue family found on selected adapter");
            return nullptr;
        }

        const float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_create_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_create_info.queueFamilyIndex = graphics_family_index;
        queue_create_info.queueCount = 1;
        queue_create_info.pQueuePriorities = &queue_priority;

        VkPhysicalDeviceDynamicRenderingFeatures enabled_dynamic_rendering{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        enabled_dynamic_rendering.dynamicRendering = VK_TRUE;
        VkPhysicalDeviceSynchronization2Features enabled_sync2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        enabled_sync2.synchronization2 = sync2_supported ? VK_TRUE : VK_FALSE;
        enabled_dynamic_rendering.pNext = &enabled_sync2;

        const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create_info.pNext = &enabled_dynamic_rendering;
        create_info.queueCreateInfoCount = 1;
        create_info.pQueueCreateInfos = &queue_create_info;
        create_info.enabledExtensionCount = 1;
        create_info.ppEnabledExtensionNames = device_extensions;

        VkDevice device = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateDevice(physical_device, &create_info, nullptr, &device), "vkCreateDevice"))
        {
            return nullptr;
        }

        return std::make_unique<VulkanDevice>(m_instance, physical_device, device, graphics_family_index, desc,
                                              sync2_supported, dynamic_rendering_supported);
    }

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
    bool m_validation_enabled = false;
    bool m_valid = false;
};
} // namespace

std::unique_ptr<Instance> create_vulkan_instance(const InstanceDesc& desc)
{
    return std::make_unique<VulkanInstance>(desc);
}
} // namespace crd::rhi
