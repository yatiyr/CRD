#pragma once

#include <crd/core/types.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/gpu_residency.hpp>
#include <crd/rhi/instance.hpp>

#include <memory>

namespace crd::gpu
{
class IGpuContext; // D-008 C2-b: rhi can ADOPT a gpu-context device (forward-declared to keep this header abstract)
} // namespace crd::gpu

namespace crd::rhi
{
[[nodiscard]] std::unique_ptr<Instance> create_vulkan_instance(const InstanceDesc& desc = InstanceDesc{});

// D-008 C2-b (ADR-0103): build an rhi Device that ADOPTS a `crd::gpu::VulkanGpuContext`'s VkInstance/VkPhysicalDevice/
// VkDevice/queues — ONE device shared by the renderer (this Device), compute (`IComputeContext`), and raster
// (`IRasterContext`). The returned Device uses the shared handles and NEVER destroys them; the context owns them and
// must OUTLIVE the Device. `context` must be Vulkan + graphics-capable, else nullptr. This is the step that unifies the
// two VkDevices the ADR-0099 audit found; `rhi-vulkan`'s own device creation retires at C2-f.
[[nodiscard]] std::unique_ptr<Device> create_vulkan_device_adopting(crd::gpu::IGpuContext& context);

// ADR-0085 S6 diagnostic: count of pooled VkDeviceMemory blocks the device's GPU
// suballocator holds (dedicated allocations excluded). For tests/tools — a small
// count across many small allocations proves suballocation is occurring. Returns 0
// for a non-Vulkan device. Snapshot: races with concurrent allocations.
[[nodiscard]] crd::u32 vulkan_resident_block_count(const Device& device) noexcept;

// ADR-0085 S7: run one idle-gated GPU defragmentation pass over the device's live
// resources, relocating the ones `policy` selects (recreate + transfer-copy + swap
// internal handle; on_relocated fires per moved resource). No-op for a non-Vulkan
// device. MUST be called single-threaded with no other allocation/GPU work in flight.
void vulkan_defragment(Device& device, IDefragPolicy& policy);

// ADR-0085 S7 residency. configure_* sets the injected policy + soft device-local
// byte budget (0 == unlimited); over-budget device-local allocations then auto-evict
// via the policy. make_resident re-promotes an evicted buffer back to device-local
// (the consumer's "on access" hook); evict_to_host forces a buffer to host memory.
// All return device-local bytes moved (0 on no-op / non-Vulkan device).
void vulkan_configure_residency(Device& device, IResidencyPolicy* policy, crd::u64 device_local_budget);
[[nodiscard]] crd::u64 vulkan_device_local_used(const Device& device) noexcept;
crd::u64               vulkan_make_resident(Device& device, Buffer& buffer);
crd::u64               vulkan_evict_to_host(Device& device, Buffer& buffer);
} // namespace crd::rhi
