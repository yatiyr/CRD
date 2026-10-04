#include "vulkan_execution.hpp"

#include <mutex>

namespace crd::gpu::detail
{
namespace
{
// A small fixed table: a process holds a handful of devices, and the record must survive them (no allocation on the
// failure path). When full, the oldest entry is reused.
constexpr crd::usize kMaxDevices = 16;

struct FailureStore
{
    std::mutex      mutex;
    VkDevice        devices[kMaxDevices] = {};
    VkDeviceFailure records[kMaxDevices] = {};
    crd::usize      next                 = 0;
    crd::u64        sequence             = 0;
    VkResult        injected             = VK_SUCCESS;
    VkDeviceFailure last{}; // the most recent first failure, kept after its device is forgotten
};

FailureStore& store() noexcept
{
    static FailureStore s;
    return s;
}

// The slot for `device`; nullptr when absent and `create` is false. Caller holds the mutex.
VkDeviceFailure* slot(FailureStore& s, VkDevice device, bool create) noexcept
{
    for (crd::usize i = 0; i < kMaxDevices; ++i)
    {
        if (s.devices[i] == device)
        {
            return &s.records[i];
        }
    }
    if (!create)
    {
        return nullptr;
    }
    const crd::usize i = s.next;
    s.next             = (s.next + 1U) % kMaxDevices;
    s.devices[i]       = device;
    s.records[i]       = VkDeviceFailure{};
    return &s.records[i];
}

VkFailureOrigin classify(VkResult result) noexcept
{
    if (result == VK_ERROR_DEVICE_LOST)
    {
        return VkFailureOrigin::Observed;
    }
    if (result == VK_TIMEOUT)
    {
        return VkFailureOrigin::TimedOut;
    }
    return VkFailureOrigin::Failed;
}

// Count a failed call; only the first failure on a device sets its origin, result and operation. Caller holds the mutex.
void record_locked(FailureStore& s, VkDevice device, VkResult result, VkFailureOrigin origin,
                   const char* operation) noexcept
{
    VkDeviceFailure* r = slot(s, device, true);
    ++r->failures;
    if (r->origin == VkFailureOrigin::None)
    {
        r->origin       = origin;
        r->first_result = static_cast<crd::i32>(result);
        r->operation    = operation;
        r->sequence     = ++s.sequence;
    }
    if (r->sequence == s.sequence)
    {
        s.last = *r; // the newest first failure, with its running count
    }
}

void record(VkDevice device, VkResult result, VkFailureOrigin origin, const char* operation) noexcept
{
    FailureStore&         s = store();
    const std::lock_guard lock(s.mutex);
    record_locked(s, device, result, origin, operation);
}

// Before a call: a pending injection, or a device already known to be lost, answers without the driver.
bool answer_without_driver(VkDevice device, const char* operation, VkResult& out) noexcept
{
    FailureStore&         s = store();
    const std::lock_guard lock(s.mutex);
    if (s.injected != VK_SUCCESS)
    {
        out        = s.injected;
        s.injected = VK_SUCCESS;
        record_locked(s, device, out, VkFailureOrigin::Simulated, operation);
        return true;
    }
    const VkDeviceFailure* known = slot(s, device, false);
    if (known == nullptr || !known->lost())
    {
        return false;
    }
    out = VK_ERROR_DEVICE_LOST;
    record_locked(s, device, out, VkFailureOrigin::Observed, operation); // a further failure; the first one stays
    return true;
}
} // namespace

VkResult vk_submit(VkDevice device, VkQueue queue, const VkSubmitInfo& submit, VkFence fence,
                   const char* operation) noexcept
{
    VkResult result = VK_SUCCESS;
    if (answer_without_driver(device, operation, result))
    {
        return result;
    }
    result = vkQueueSubmit(queue, 1U, &submit, fence);
    if (result != VK_SUCCESS)
    {
        record(device, result, classify(result), operation);
    }
    return result;
}

VkResult vk_wait(VkDevice device, VkFence fence, crd::u64 timeout_ns, const char* operation) noexcept
{
    VkResult result = VK_SUCCESS;
    if (answer_without_driver(device, operation, result))
    {
        return result;
    }
    result = vkWaitForFences(device, 1U, &fence, VK_TRUE, timeout_ns);
    if (result != VK_SUCCESS)
    {
        record(device, result, classify(result), operation);
    }
    return result;
}

VkDeviceFailure vk_device_failure(VkDevice device) noexcept
{
    FailureStore&          s = store();
    const std::lock_guard  lock(s.mutex);
    const VkDeviceFailure* r = slot(s, device, false);
    return r != nullptr ? *r : VkDeviceFailure{};
}

VkDeviceFailure vk_last_device_failure() noexcept
{
    FailureStore&         s = store();
    const std::lock_guard lock(s.mutex);
    return s.last;
}

void vk_forget_device(VkDevice device) noexcept
{
    FailureStore&         s = store();
    const std::lock_guard lock(s.mutex);
    for (crd::usize i = 0; i < kMaxDevices; ++i)
    {
        if (s.devices[i] == device)
        {
            s.devices[i] = VK_NULL_HANDLE;
            s.records[i] = VkDeviceFailure{};
        }
    }
}

void vk_inject_next_result(VkResult result) noexcept
{
    FailureStore&         s = store();
    const std::lock_guard lock(s.mutex);
    s.injected = result;
}
} // namespace crd::gpu::detail
