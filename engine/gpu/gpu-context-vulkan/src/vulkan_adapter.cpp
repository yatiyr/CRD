#include "vulkan_adapter.hpp"

#include <crd/containers/array.hpp>
#include <crd/core/platform.hpp>

#include <cstring>

namespace crd::gpu::detail
{
namespace
{
[[nodiscard]] bool has_driver_properties(VkPhysicalDevice physical, crd::u32 api) noexcept
{
    if (api >= VK_API_VERSION_1_2)
    {
        return true;
    }
    crd::u32 count = 0U;
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS)
    {
        return false;
    }
    crd::containers::Array<VkExtensionProperties> listed;
    listed.resize(count);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, listed.data()) < VK_SUCCESS)
    {
        return false;
    }
    for (crd::u32 i = 0U; i < count; ++i)
    {
        if (std::strcmp(listed[i].extensionName, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME) == 0)
        {
            return true;
        }
    }
    return false;
}
} // namespace

GpuAdapter vk_adapter(VkPhysicalDevice physical) noexcept
{
    GpuAdapter a{};
    if (physical == VK_NULL_HANDLE)
    {
        return a;
    }
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    vkGetPhysicalDeviceProperties(physical, &p2.properties);
    const bool with_driver = has_driver_properties(physical, p2.properties.apiVersion);
    if (with_driver)
    {
        p2.pNext = &driver;
        vkGetPhysicalDeviceProperties2(physical, &p2);
    }
    const VkPhysicalDeviceProperties& p = p2.properties;
    std::memcpy(a.backend, "vulkan", sizeof("vulkan"));
    static_assert(sizeof(a.name) >= VK_MAX_PHYSICAL_DEVICE_NAME_SIZE, "a Vulkan device name fits");
    std::memcpy(a.name, p.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);
    a.name[sizeof(a.name) - 1U] = '\0';
    a.vendor = p.vendorID;
    a.device = p.deviceID;
    a.driver = p.driverVersion;
    a.api = p.apiVersion;
    driver.driverInfo[VK_MAX_DRIVER_INFO_SIZE - 1U] = '\0';
    if (with_driver && driver.driverInfo[0] != '\0')
    {
        static_assert(sizeof(a.driver_version) <= VK_MAX_DRIVER_INFO_SIZE, "the copy reads inside driverInfo");
        std::memcpy(a.driver_version, driver.driverInfo, sizeof(a.driver_version));
        a.driver_version[sizeof(a.driver_version) - 1U] = '\0';
    }
    else
    {
        format_vulkan_driver_version(p.vendorID, p.driverVersion, CRD_OS_WINDOWS != 0, a.driver_version);
    }
    a.known = true;
    return a;
}
} // namespace crd::gpu::detail
