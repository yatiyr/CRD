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
    [[nodiscard]] bool lost() const noexcept
    {
        return first_result == static_cast<crd::i32>(VK_ERROR_DEVICE_LOST);
    }
};

// Submit `submit` to `queue`, signalling `fence` (may be VK_NULL_HANDLE). `operation` must be a string literal.
[[nodiscard]] VkResult vk_submit(VkDevice device, VkQueue queue, const VkSubmitInfo& submit, VkFence fence,
                                 const char* operation) noexcept;
// Wait for `fence` at most `timeout_ns`. VK_TIMEOUT is returned and recorded as TimedOut.
[[nodiscard]] VkResult vk_wait(VkDevice device, VkFence fence, crd::u64 timeout_ns, const char* operation) noexcept;

// The first failure recorded for the live `device` (origin None when it never failed).
[[nodiscard]] VkDeviceFailure vk_device_failure(VkDevice device) noexcept;
// The most recent first failure on any device, kept after that device is destroyed (the last-known state).
[[nodiscard]] VkDeviceFailure vk_last_device_failure() noexcept;
// Called before vkDestroyDevice: the handle value may be reused by a later device, which must not inherit this record.
void vk_forget_device(VkDevice device) noexcept;

// Test seam: the next vk_submit or vk_wait on any device returns `result` without calling the driver, and records it
// as Simulated. VK_SUCCESS clears a pending injection.
void vk_inject_next_result(VkResult result) noexcept;
} // namespace crd::gpu::detail
