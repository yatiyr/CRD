#pragma once

// DIAG.7c(c): the Vulkan completion seam, the counterpart of dx12_submit/dx12_wait. Every submission and fence wait that
// moves onto it gets a bounded wait, observes VK_ERROR_DEVICE_LOST, and keeps the FIRST failure per device: once a
// device has failed, later calls (which typically fail too, often with less information) only count, so the original
// result and operation survive. A device known to be lost is not called again: its submissions and waits return
// VK_ERROR_DEVICE_LOST at once. Provider-private; the record outlives every context that used the device.

#include <crd/core/types.hpp>

#include <vulkan/vulkan.h>

namespace crd::gpu::detail
{
// The DX12 default, so both providers bound a wait identically.
inline constexpr crd::u64 kVkDefaultWaitNs = 30'000'000'000ULL;

enum class VkFailureOrigin : crd::u8
{
    None,      // nothing has failed on this device
    Observed,  // the runtime/driver returned VK_ERROR_DEVICE_LOST: an actual device loss
    TimedOut,  // a bounded fence wait expired; the device is not known to be lost
    Failed,    // another error result from a submission or wait
    Simulated, // a result injected at the seam by a test (error-path coverage, never a real loss)
};

struct VkDeviceFailure
{
    VkFailureOrigin origin       = VkFailureOrigin::None;
    crd::i32        first_result = 0;       // the VkResult of the first failure, kept through later failures
    const char*     operation    = nullptr; // the static label of the call that failed first
    crd::u64        sequence     = 0;       // 1-based order of first failures across the process
    crd::u64        failures     = 0;       // every failed call on this device, the first included
    crd::u64        slow_waits   = 0;       // completions that outlasted their report threshold (not failures)
    // DIAG.7c(g): the first VK_ERROR_DEVICE_LOST on this device, which need not be its first failure (a bounded wait
    // can time out before the device is reported lost). Observed or Simulated; None while the device is not lost.
    VkFailureOrigin loss_origin    = VkFailureOrigin::None;
    const char*     loss_operation = nullptr; // the static label of the call that first returned the loss
    [[nodiscard]] bool lost() const noexcept
    {
        return loss_origin != VkFailureOrigin::None;
    }
};

// DIAG.7c(d): what VK_EXT_device_fault reported for a lost device, read once when the loss is first recorded. Fixed
// size (no allocation on the failure path); counts are what the driver reported, `*_kept` what fits here.
inline constexpr crd::u32 kVkFaultInfosKept = 8U;

struct VkDeviceFaultReport
{
    bool                        available          = false; // the device enabled VK_EXT_device_fault
    bool                        queried            = false; // vkGetDeviceFaultInfoEXT ran for this loss
    crd::i32                    query_result       = 0;     // its VkResult (VK_INCOMPLETE: infos were truncated)
    crd::u32                    address_count      = 0;
    crd::u32                    vendor_count       = 0;
    crd::u32                    addresses_kept     = 0;
    crd::u32                    vendors_kept       = 0;
    crd::u64                    vendor_binary_size = 0;
    char                        description[VK_MAX_DESCRIPTION_SIZE] = {};
    VkDeviceFaultAddressInfoEXT addresses[kVkFaultInfosKept]          = {};
    VkDeviceFaultVendorInfoEXT  vendors[kVkFaultInfosKept]            = {};
};

// DIAG.7c(g): the adapter a device runs on, registered when the device is created, so a loss bundle names the
// adapter and driver without the process that wrote it.
struct VkLossAdapter
{
    crd::u32 vendor_id      = 0;
    crd::u32 device_id      = 0;
    crd::u32 driver_version = 0;
    crd::u32 api_version    = 0;
    char     name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
};

// Called once a device is created, with the properties of its physical device.
void vk_register_device_adapter(VkDevice device, const VkPhysicalDeviceProperties& properties) noexcept;

// Called once a device is created with VK_EXT_device_fault enabled: the seam queries `fn` when that device is lost.
void vk_register_device_fault(VkDevice device, PFN_vkGetDeviceFaultInfoEXT fn) noexcept;
// The fault report of the live `device` (available false when it never registered the extension).
[[nodiscard]] VkDeviceFaultReport vk_device_fault_report(VkDevice device) noexcept;
// The report that belongs to vk_last_device_failure(), kept after that device is destroyed.
[[nodiscard]] VkDeviceFaultReport vk_last_device_fault_report() noexcept;

// Submit `submit` to `queue`, signalling `fence` (may be VK_NULL_HANDLE). `operation` must be a string literal.
[[nodiscard]] VkResult vk_submit(VkDevice device, VkQueue queue, const VkSubmitInfo& submit, VkFence fence,
                                 const char* operation) noexcept;
// Wait for `fence` at most `timeout_ns`. VK_TIMEOUT is returned and recorded as TimedOut. Only for a caller that may
// safely ABANDON the wait: nothing it will free can still be referenced by queued work.
[[nodiscard]] VkResult vk_wait(VkDevice device, VkFence fence, crd::u64 timeout_ns, const char* operation) noexcept;

// Wait for `fence` until the work COMPLETES or the device is lost; past `report_after_ns` the wait is counted as slow
// (VkDeviceFailure::slow_waits) and continues. Vulkan has no way to stop queued work (DX12 forces RemoveDevice), so a
// caller that frees memory after the wait must never abandon it: an abandoned wait let a software device keep writing
// into freed buffers. Returns VK_SUCCESS or the failure that ended the wait.
[[nodiscard]] VkResult vk_wait_complete(VkDevice device, VkFence fence, crd::u64 report_after_ns,
                                        const char* operation) noexcept;

// vkQueueWaitIdle / vkDeviceWaitIdle through the seam. Vulkan cannot bound an idle wait, so these complete or fail;
// a failure is classified and kept like any other, and a device known to be lost is not called again.
[[nodiscard]] VkResult vk_queue_wait_idle(VkDevice device, VkQueue queue, const char* operation) noexcept;
[[nodiscard]] VkResult vk_device_wait_idle(VkDevice device, const char* operation) noexcept;

// The first failure recorded for the live `device` (origin None when it never failed).
[[nodiscard]] VkDeviceFailure vk_device_failure(VkDevice device) noexcept;
// The most recent first failure on any device, kept after that device is destroyed (the last-known state).
[[nodiscard]] VkDeviceFailure vk_last_device_failure() noexcept;
// Called before vkDestroyDevice: the handle value may be reused by a later device, which must not inherit this record.
void vk_forget_device(VkDevice device) noexcept;

// DIAG.7c(g): the loss bundle. The first time a device is recorded lost (after its VK_EXT_device_fault report is read),
// the seam writes the loss, when crash capture is installed, as a live dump of kind DeviceRemoved (gpu_*.dmp) whose
// evidence stream holds a VkLossBundleHeader followed by this record: once per device, and never for a failure that is
// not a loss (a timed-out wait, another error). A simulated loss is labelled Simulated, never presented as a real one.
// Live dumps are a Windows facility: elsewhere the write reports Unsupported and the record is kept in process only.
// The record holds no pointers: operation labels are copied, truncated and terminated.
inline constexpr crd::u32 kVkOperationBytes = 64U;

struct VkLossRecord
{
    VkFailureOrigin     origin         = VkFailureOrigin::None; // the device's first failure (VkDeviceFailure)
    VkFailureOrigin     loss_origin    = VkFailureOrigin::None; // Observed or Simulated
    crd::i32            first_result   = 0;
    crd::u64            sequence       = 0;
    crd::u64            failures       = 0; // failed calls on the device when the loss was written
    crd::u64            slow_waits     = 0;
    char                operation[kVkOperationBytes]      = {}; // the first failure's operation
    char                loss_operation[kVkOperationBytes] = {}; // the operation that returned the loss
    VkLossAdapter       adapter{};
    VkDeviceFaultReport fault{};
    // The crash::WriteResult of the bundle written for this loss (NotInstalled when crash capture is off). Set on the
    // stored record after the write; the copy inside the bundle carries NotInstalled.
    crd::u32 bundle_result = 0;
};

// The reader refuses a stream whose magic, version or size does not match this build rather than misreading it; the
// magic differs from the DX12 removal bundle's, so neither provider's reader accepts the other's evidence.
inline constexpr crd::u32 kVkLossBundleMagic   = 0x4C565243U; // bytes 'C','R','V','L'
inline constexpr crd::u32 kVkLossBundleVersion = 1U;

struct VkLossBundleHeader
{
    crd::u32 magic        = kVkLossBundleMagic;
    crd::u32 version      = kVkLossBundleVersion;
    crd::u32 record_bytes = 0;
    crd::u32 reserved     = 0;
};

enum class VkBundleRead : crd::u8
{
    Ok,
    NoStream,   // the file has no Cerid evidence stream (or is not a readable dump on this platform)
    BadMagic,   // the evidence stream is not a Vulkan loss bundle
    BadVersion, // a loss bundle of another format version
    BadSize,    // the size does not match this build's record
};

// Read a loss bundle back from a dump file. Never throws; `out` is written only on Ok.
[[nodiscard]] VkBundleRead vk_read_loss_bundle(const wchar_t* dump_path, VkLossRecord& out) noexcept;
// The most recently written loss, with its bundle result, kept after its device is destroyed (origin None before any).
[[nodiscard]] VkLossRecord vk_last_loss() noexcept;

// Test seam: the next seam call (submit, wait or idle wait) on any device returns `result` without calling the driver,
// and records it as Simulated. VK_SUCCESS clears a pending injection.
void vk_inject_next_result(VkResult result) noexcept;
} // namespace crd::gpu::detail
