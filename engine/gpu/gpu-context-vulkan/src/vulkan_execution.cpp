#include "vulkan_execution.hpp"

#include <cstring>
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
    std::mutex                  mutex;
    VkDevice                    devices[kMaxDevices]   = {};
    VkDeviceFailure             records[kMaxDevices]   = {};
    VkDeviceFaultReport         faults[kMaxDevices]    = {}; // DIAG.7c(d)
    PFN_vkGetDeviceFaultInfoEXT fault_fns[kMaxDevices] = {}; // set when the device enabled VK_EXT_device_fault
    crd::usize                  next                   = 0;
    crd::u64                    sequence               = 0;
    VkResult                    injected               = VK_SUCCESS;
    VkDeviceFailure             last{};       // the most recent first failure, kept after its device is forgotten
    VkDeviceFaultReport         last_fault{}; // the fault report that belongs to `last`
};

FailureStore& store() noexcept
{
    static FailureStore s;
    return s;
}

// The slot index for `device`; kMaxDevices when absent and `create` is false. Caller holds the mutex.
crd::usize slot_index(FailureStore& s, VkDevice device, bool create) noexcept
{
    for (crd::usize i = 0; i < kMaxDevices; ++i)
    {
        if (s.devices[i] == device)
        {
            return i;
        }
    }
    if (!create)
    {
        return kMaxDevices;
    }
    const crd::usize i = s.next;
    s.next             = (s.next + 1U) % kMaxDevices;
    s.devices[i]       = device;
    s.records[i]       = VkDeviceFailure{};
    s.faults[i]        = VkDeviceFaultReport{};
    s.fault_fns[i]     = nullptr;
    return i;
}

// The failure record for `device`; nullptr when absent and `create` is false. Caller holds the mutex.
VkDeviceFailure* slot(FailureStore& s, VkDevice device, bool create) noexcept
{
    const crd::usize i = slot_index(s, device, create);
    return i < kMaxDevices ? &s.records[i] : nullptr;
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
    const crd::usize i = slot_index(s, device, true);
    VkDeviceFailure* r = &s.records[i];
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
        s.last       = *r; // the newest first failure, with its running count
        s.last_fault = s.faults[i];
    }
}

// DIAG.7c(d): read VK_EXT_device_fault for a lost device. The vendor binary is not collected (its size is recorded).
VkDeviceFaultReport query_device_fault(VkDevice device, PFN_vkGetDeviceFaultInfoEXT fn) noexcept
{
    VkDeviceFaultReport report{};
    report.available = true;
    report.queried   = true;
    VkDeviceFaultCountsEXT counts{};
    counts.sType    = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
    VkResult result = fn(device, &counts, nullptr);
    if (result != VK_SUCCESS)
    {
        report.query_result = static_cast<crd::i32>(result);
        return report;
    }
    report.address_count      = counts.addressInfoCount;
    report.vendor_count       = counts.vendorInfoCount;
    report.vendor_binary_size = counts.vendorBinarySize;
    const auto kept           = [](crd::u32 n) { return n < kVkFaultInfosKept ? n : kVkFaultInfosKept; };
    counts.addressInfoCount   = kept(counts.addressInfoCount);
    counts.vendorInfoCount    = kept(counts.vendorInfoCount);
    counts.vendorBinarySize   = 0U;
    VkDeviceFaultInfoEXT info{};
    info.sType          = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    info.pAddressInfos  = report.addresses;
    info.pVendorInfos   = report.vendors;
    result              = fn(device, &counts, &info);
    report.query_result = static_cast<crd::i32>(result);
    if (result == VK_SUCCESS || result == VK_INCOMPLETE)
    {
        std::memcpy(report.description, info.description, sizeof(report.description));
        report.description[sizeof(report.description) - 1U] = '\0';
        report.addresses_kept = counts.addressInfoCount;
        report.vendors_kept   = counts.vendorInfoCount;
    }
    return report;
}

// After a seam call: when `device` is lost (really or by injection) and its fault was not yet read, read it once.
// The driver is called outside the lock; the report is then stored, and becomes the last-known one if this device
// holds the newest first failure.
void collect_device_fault(VkDevice device) noexcept
{
    FailureStore&               s  = store();
    PFN_vkGetDeviceFaultInfoEXT fn = nullptr;
    {
        const std::lock_guard lock(s.mutex);
        const crd::usize      i = slot_index(s, device, false);
        if (i == kMaxDevices || !s.records[i].lost() || s.fault_fns[i] == nullptr || s.faults[i].queried)
        {
            return;
        }
        fn = s.fault_fns[i];
        s.faults[i].queried = true; // claim it: a concurrent failure on the same device does not query twice
    }
    const VkDeviceFaultReport report = query_device_fault(device, fn);
    const std::lock_guard     lock(s.mutex);
    const crd::usize          i = slot_index(s, device, false);
    if (i == kMaxDevices)
    {
        return; // forgotten meanwhile
    }
    s.faults[i] = report;
    if (s.records[i].sequence == s.last.sequence)
    {
        s.last_fault = report;
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
    if (!answer_without_driver(device, operation, result))
    {
        result = vkQueueSubmit(queue, 1U, &submit, fence);
        if (result != VK_SUCCESS)
        {
            record(device, result, classify(result), operation);
        }
    }
    if (result != VK_SUCCESS)
    {
        collect_device_fault(device);
    }
    return result;
}

VkResult vk_wait(VkDevice device, VkFence fence, crd::u64 timeout_ns, const char* operation) noexcept
{
    VkResult result = VK_SUCCESS;
    if (!answer_without_driver(device, operation, result))
    {
        result = vkWaitForFences(device, 1U, &fence, VK_TRUE, timeout_ns);
        if (result != VK_SUCCESS)
        {
            record(device, result, classify(result), operation);
        }
    }
    if (result != VK_SUCCESS)
    {
        collect_device_fault(device);
    }
    return result;
}

VkResult vk_wait_complete(VkDevice device, VkFence fence, crd::u64 report_after_ns, const char* operation) noexcept
{
    VkResult result = VK_SUCCESS;
    if (!answer_without_driver(device, operation, result))
    {
        result = vkWaitForFences(device, 1U, &fence, VK_TRUE, report_after_ns);
        if (result == VK_TIMEOUT)
        {
            {
                FailureStore&         s = store();
                const std::lock_guard lock(s.mutex);
                ++slot(s, device, true)->slow_waits;
            }
            result = vkWaitForFences(device, 1U, &fence, VK_TRUE, UINT64_MAX); // completion or loss, never abandonment
        }
        if (result != VK_SUCCESS)
        {
            record(device, result, classify(result), operation);
        }
    }
    if (result != VK_SUCCESS)
    {
        collect_device_fault(device);
    }
    return result;
}

VkResult vk_queue_wait_idle(VkDevice device, VkQueue queue, const char* operation) noexcept
{
    VkResult result = VK_SUCCESS;
    if (!answer_without_driver(device, operation, result))
    {
        result = vkQueueWaitIdle(queue);
        if (result != VK_SUCCESS)
        {
            record(device, result, classify(result), operation);
        }
    }
    if (result != VK_SUCCESS)
    {
        collect_device_fault(device);
    }
    return result;
}

VkResult vk_device_wait_idle(VkDevice device, const char* operation) noexcept
{
    VkResult result = VK_SUCCESS;
    if (!answer_without_driver(device, operation, result))
    {
        result = vkDeviceWaitIdle(device);
        if (result != VK_SUCCESS)
        {
            record(device, result, classify(result), operation);
        }
    }
    if (result != VK_SUCCESS)
    {
        collect_device_fault(device);
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
            s.devices[i]   = VK_NULL_HANDLE;
            s.records[i]   = VkDeviceFailure{};
            s.faults[i]    = VkDeviceFaultReport{};
            s.fault_fns[i] = nullptr;
        }
    }
}

void vk_register_device_fault(VkDevice device, PFN_vkGetDeviceFaultInfoEXT fn) noexcept
{
    FailureStore&         s = store();
    const std::lock_guard lock(s.mutex);
    const crd::usize      i = slot_index(s, device, true);
    s.fault_fns[i]          = fn;
    s.faults[i]             = VkDeviceFaultReport{};
    s.faults[i].available   = fn != nullptr;
}

VkDeviceFaultReport vk_device_fault_report(VkDevice device) noexcept
{
    FailureStore&         s = store();
    const std::lock_guard lock(s.mutex);
    const crd::usize      i = slot_index(s, device, false);
    return i < kMaxDevices ? s.faults[i] : VkDeviceFaultReport{};
}

VkDeviceFaultReport vk_last_device_fault_report() noexcept
{
    FailureStore&         s = store();
    const std::lock_guard lock(s.mutex);
    return s.last_fault;
}

void vk_inject_next_result(VkResult result) noexcept
{
    FailureStore&         s = store();
    const std::lock_guard lock(s.mutex);
    s.injected = result;
}
} // namespace crd::gpu::detail
