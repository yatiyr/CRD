#include "vulkan_execution.hpp"

#include <crd/core/crash.hpp> // DIAG.7c(g): a recorded loss is written as a DeviceRemoved live dump

#include <cstddef>
#include <cstring>
#include <mutex>
#include <type_traits>

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
    VkDeviceFaultReport         faults[kMaxDevices]     = {}; // DIAG.7c(d)
    PFN_vkGetDeviceFaultInfoEXT fault_fns[kMaxDevices]  = {}; // set when the device enabled VK_EXT_device_fault
    bool                        fault_done[kMaxDevices] = {}; // (g) the fault query of a lost device has finished
    VkLossAdapter               adapters[kMaxDevices]   = {}; // (g) registered at device creation
    bool                        bundled[kMaxDevices]    = {}; // (g) the loss bundle of this device has been written
    crd::usize                  next                   = 0;
    crd::u64                    sequence               = 0;
    VkResult                    injected               = VK_SUCCESS;
    VkDeviceFailure             last{};       // the most recent first failure, kept after its device is forgotten
    VkDeviceFaultReport         last_fault{}; // the fault report that belongs to `last`
    VkLossRecord                last_loss{};  // (g) the most recently written loss
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
    s.fault_done[i]    = false;
    s.adapters[i]      = VkLossAdapter{};
    s.bundled[i]       = false;
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
    if (result == VK_ERROR_DEVICE_LOST && r->loss_origin == VkFailureOrigin::None)
    {
        r->loss_origin    = origin; // (g) the first loss, even when an earlier failure was not one
        r->loss_operation = operation;
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
    s.faults[i]     = report;
    s.fault_done[i] = true;
    if (s.records[i].sequence == s.last.sequence)
    {
        s.last_fault = report;
    }
}

static_assert(std::is_trivially_copyable_v<VkLossRecord>, "the loss bundle stores the record's bytes");

// The bundle bytes: header then record, contiguous (the header is 16 bytes, so the 8-aligned record follows directly).
struct LossBundle
{
    VkLossBundleHeader header{};
    VkLossRecord       record{};
};
static_assert(offsetof(LossBundle, record) == sizeof(VkLossBundleHeader));

void copy_label(char (&out)[kVkOperationBytes], const char* label) noexcept
{
    if (label == nullptr)
    {
        return;
    }
    crd::usize n = 0;
    while (n + 1U < kVkOperationBytes && label[n] != '\0')
    {
        out[n] = label[n];
        ++n;
    }
    out[n] = '\0';
}

// The loss record of slot `i`. Caller holds the mutex.
VkLossRecord loss_record_locked(const FailureStore& s, crd::usize i) noexcept
{
    const VkDeviceFailure& r = s.records[i];
    VkLossRecord           out{};
    out.origin        = r.origin;
    out.loss_origin   = r.loss_origin;
    out.first_result  = r.first_result;
    out.sequence      = r.sequence;
    out.failures      = r.failures;
    out.slow_waits    = r.slow_waits;
    out.adapter       = s.adapters[i];
    out.fault         = s.faults[i];
    out.bundle_result = static_cast<crd::u32>(crd::crash::WriteResult::NotInstalled);
    copy_label(out.operation, r.operation);
    copy_label(out.loss_operation, r.loss_operation);
    return out;
}

// DIAG.7c(g): once per lost device, after its fault report is in, write the loss as a DeviceRemoved live dump. The dump
// is written outside the lock (it suspends the other threads and walks their stacks); its result is kept on the stored
// last-known loss. A device whose fault query is still running on another thread is written by that thread.
void write_loss_bundle(VkDevice device) noexcept
{
    FailureStore& s = store();
    LossBundle    bundle{};
    {
        const std::lock_guard lock(s.mutex);
        const crd::usize      i = slot_index(s, device, false);
        if (i == kMaxDevices || !s.records[i].lost() || s.bundled[i])
        {
            return;
        }
        if (s.fault_fns[i] != nullptr && !s.fault_done[i])
        {
            return;
        }
        s.bundled[i]  = true;
        bundle.record = loss_record_locked(s, i);
    }
    bundle.header.record_bytes = static_cast<crd::u32>(sizeof(VkLossRecord));
    crd::crash::DumpNote note{};
    note.kind                             = crd::crash::DumpKind::DeviceRemoved;
    note.evidence                         = &bundle;
    note.evidence_bytes                   = static_cast<std::uint32_t>(sizeof(bundle));
    const crd::crash::WriteResult written = crd::crash::capture_dump(note, nullptr);
    bundle.record.bundle_result           = static_cast<crd::u32>(written);
    const std::lock_guard lock(s.mutex);
    s.last_loss = bundle.record;
}

// After a failed seam call: read the device's fault report if it is now lost, then write its loss bundle.
void after_failure(VkDevice device) noexcept
{
    collect_device_fault(device);
    write_loss_bundle(device);
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
        after_failure(device);
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
        after_failure(device);
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
        after_failure(device);
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
        after_failure(device);
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
        after_failure(device);
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
            s.faults[i]     = VkDeviceFaultReport{};
            s.fault_fns[i]  = nullptr;
            s.fault_done[i] = false;
            s.adapters[i]   = VkLossAdapter{};
            s.bundled[i]    = false;
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

void vk_register_device_adapter(VkDevice device, const VkPhysicalDeviceProperties& properties) noexcept
{
    VkLossAdapter adapter{};
    adapter.vendor_id      = properties.vendorID;
    adapter.device_id      = properties.deviceID;
    adapter.driver_version = properties.driverVersion;
    adapter.api_version    = properties.apiVersion;
    std::memcpy(adapter.name, properties.deviceName, sizeof(adapter.name));
    adapter.name[sizeof(adapter.name) - 1U] = '\0';
    FailureStore&         s = store();
    const std::lock_guard lock(s.mutex);
    s.adapters[slot_index(s, device, true)] = adapter;
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

VkLossRecord vk_last_loss() noexcept
{
    FailureStore&         s = store();
    const std::lock_guard lock(s.mutex);
    return s.last_loss;
}

VkBundleRead vk_read_loss_bundle(const wchar_t* dump_path, VkLossRecord& out) noexcept
{
    // One bounded read: a stream of exactly this build's bundle size is read whole; any other size is measured (a
    // longer one is truncated into `bundle`, then refused below).
    LossBundle        bundle{};
    const std::size_t size =
        crd::crash::read_dump_stream(dump_path, crd::crash::kEvidenceStreamType, &bundle, sizeof(bundle));
    if (size == 0U)
    {
        return VkBundleRead::NoStream;
    }
    if (size < sizeof(VkLossBundleHeader) || bundle.header.magic != kVkLossBundleMagic)
    {
        return VkBundleRead::BadMagic;
    }
    if (bundle.header.version != kVkLossBundleVersion)
    {
        return VkBundleRead::BadVersion;
    }
    if (size != sizeof(bundle) || bundle.header.record_bytes != sizeof(VkLossRecord))
    {
        return VkBundleRead::BadSize;
    }
    out = bundle.record;
    return VkBundleRead::Ok;
}

void vk_inject_next_result(VkResult result) noexcept
{
    FailureStore&         s = store();
    const std::lock_guard lock(s.mutex);
    s.injected = result;
}
} // namespace crd::gpu::detail
