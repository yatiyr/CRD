#include "vulkan_device_memory.hpp"

#include <cstdint>

namespace crd::gpu::detail
{
namespace
{
// Add one heap to a summed reading; a sum that would wrap leaves the reading unread rather than wrong.
void add_heap(BudgetReading& r, VkDeviceSize budget, VkDeviceSize usage, bool& wrapped) noexcept
{
    if (r.capacity > UINT64_MAX - budget || r.in_use > UINT64_MAX - usage)
    {
        wrapped = true;
        return;
    }
    r.read     = true;
    r.bounded  = true;
    r.capacity += budget;
    r.in_use   += usage;
}
} // namespace

DeviceMemoryReading vk_device_memory(VkPhysicalDevice physical, bool budget_enabled) noexcept
{
    DeviceMemoryReading out;
    if (physical == VK_NULL_HANDLE)
    {
        out.reason = "no Vulkan physical device";
        return out;
    }
    if (!budget_enabled)
    {
        out.reason = "the device was created without VK_EXT_memory_budget";
        return out;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    properties.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(physical, &properties);

    bool           wrapped = false;
    const crd::u32 heaps   = properties.memoryProperties.memoryHeapCount;
    for (crd::u32 h = 0U; h < heaps && h < VK_MAX_MEMORY_HEAPS; ++h)
    {
        if (budget.heapBudget[h] == 0U && budget.heapUsage[h] == 0U)
        {
            continue; // a heap the OS grants nothing and nothing uses is absent, not full
        }
        const bool local = (properties.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0U;
        add_heap(local ? out.local : out.non_local, budget.heapBudget[h], budget.heapUsage[h], wrapped);
    }
    if (wrapped)
    {
        out.local     = BudgetReading{};
        out.non_local = BudgetReading{};
        out.reason    = "the heap budgets do not fit 64 bits";
        return out;
    }
    if (!out.local.read && !out.non_local.read)
    {
        out.reason = "the device reports no memory heap";
        return out;
    }
    out.source = DeviceMemorySource::VulkanMemoryBudget;
    out.reason = out.non_local.read ? "" : "the device has no heap outside device-local memory";
    if (!out.local.read)
    {
        out.reason = "the device has no device-local heap";
    }
    return out;
}
} // namespace crd::gpu::detail
