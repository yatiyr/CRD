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

class VulkanImage final : public Image
{
public:
    VulkanImage(ImageDesc desc, VkImage image) : m_desc(std::move(desc)), m_image(image) {}

    [[nodiscard]] const ImageDesc& desc() const noexcept override { return m_desc; }

private:
    ImageDesc m_desc{};
    VkImage m_image = VK_NULL_HANDLE;
};

class VulkanSwapchain final : public Swapchain
{
public:
    VulkanSwapchain(VkDevice device, VkSwapchainKHR swapchain, SwapchainDesc desc,
                    crd::containers::Array<std::unique_ptr<VulkanImage>> images)
        : m_device(device), m_swapchain(swapchain), m_desc(std::move(desc)), m_images(std::move(images))
    {
    }

    ~VulkanSwapchain() noexcept override
    {
        if (m_swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] const SwapchainDesc& desc() const noexcept override { return m_desc; }

    void acquire_next_image() override
    {
        [[maybe_unused]] const VkResult result = vkAcquireNextImageKHR(
            m_device, m_swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &m_current_image_index);
        CRD_ASSERT(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR);
    }

    [[nodiscard]] crd::u32 current_image_index() const noexcept override { return m_current_image_index; }
    [[nodiscard]] Image& current_image() noexcept override { return *m_images[m_current_image_index]; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    SwapchainDesc m_desc{};
    crd::containers::Array<std::unique_ptr<VulkanImage>> m_images{};
    crd::u32 m_current_image_index = 0;
};

class VulkanQueue final : public Queue
{
public:
    explicit VulkanQueue(VkQueue queue) : m_queue(queue) {}

    void submit(CommandBuffer& /*command_buffer*/) override
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "Queue::submit not implemented in bootstrap slice");
    }

    void present(Swapchain& /*swapchain*/) override
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "Queue::present not implemented in bootstrap slice");
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
                 DeviceDesc desc)
        : m_instance(instance), m_physical_device(physical_device), m_device(device),
          m_graphics_family_index(graphics_family_index), m_desc(std::move(desc))
    {
        vkGetDeviceQueue(m_device, m_graphics_family_index, 0, &m_graphics_queue_handle);
        m_graphics_queue = std::make_unique<VulkanQueue>(m_graphics_queue_handle);
    }

    ~VulkanDevice() noexcept override
    {
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
            wrapped_images.push_back(
                std::make_unique<VulkanImage>(ImageDesc{{chosen_extent.width, chosen_extent.height},
                                                        Format::B8G8R8A8Unorm,
                                                        enum_bits(ImageUsage::ColorAttachment),
                                                        1,
                                                        1},
                                              image));
        }

        SwapchainDesc resolved_desc = desc;
        resolved_desc.extent = {chosen_extent.width, chosen_extent.height};
        resolved_desc.color_format = Format::B8G8R8A8Unorm;
        return std::make_unique<VulkanSwapchain>(m_device, swapchain, std::move(resolved_desc),
                                                 std::move(wrapped_images));
    }

    [[nodiscard]] std::unique_ptr<Buffer> create_buffer(const BufferDesc& /*desc*/) override
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "create_buffer not implemented in bootstrap slice");
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<Image> create_image(const ImageDesc& /*desc*/) override
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "create_image not implemented in bootstrap slice");
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<ShaderModule> create_shader_module(const ShaderModuleDesc& /*desc*/) override
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "create_shader_module not implemented in bootstrap slice");
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<Pipeline> create_graphics_pipeline(const GraphicsPipelineDesc& /*desc*/) override
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "create_graphics_pipeline not implemented in bootstrap slice");
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<CommandBuffer> create_command_buffer() override
    {
        CRD_LOG_WARN(detail::g_log_rhi_vulkan, "create_command_buffer not implemented in bootstrap slice");
        return nullptr;
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
    crd::u32 m_graphics_family_index = 0;
    DeviceDesc m_desc{};
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

        const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create_info.queueCreateInfoCount = 1;
        create_info.pQueueCreateInfos = &queue_create_info;
        create_info.enabledExtensionCount = 1;
        create_info.ppEnabledExtensionNames = device_extensions;

        VkDevice device = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateDevice(physical_device, &create_info, nullptr, &device), "vkCreateDevice"))
        {
            return nullptr;
        }

        return std::make_unique<VulkanDevice>(m_instance, physical_device, device, graphics_family_index, desc);
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
    auto instance = std::make_unique<VulkanInstance>(desc);
    crd::containers::Array<AdapterInfo> probe;
    instance->enumerate_adapters(probe);
    return instance;
}
} // namespace crd::rhi
