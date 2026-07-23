#pragma once

// vulkan_gpu_allocator.hpp — RET-4 pt 2 (ADR-0105 + ADR-0085 S6): the GPU device-memory SUBALLOCATOR, absorbed from
// crd-rhi-vulkan onto the ONE graphics layer (the rhi original dies at RET-8; its S6 contract survives here).
// One `VkDeviceMemory` per resource is a real ceiling (maxMemoryAllocationCount ≈ 4096) and a real cost — instead:
// pooled BLOCKS per (memory-type × linearity), suballocated by `crd::memory::OffsetAllocator` (O(1) free with
// coalescing), a dedicated path for ≥16 MiB requests, block-base mapping for host-visible types, and the
// non-coherent-atom alignment rule. Thread-safe (one mutex — the allocation path, not the per-frame hot path).
//
// LIFETIME CONTRACT (the allocator-outlives-borrowers rule): every resource whose ImageBundle/allocation came from
// this allocator must be destroyed BEFORE it (the standard device-before-resources GPU rule, one level down).
// `destroy_all()` tears down every block and flips a tombstone — a straggler `free()` after teardown is a safe no-op
// (defensive; the contract above is the law).

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocators/offset_allocator.hpp>

#include <vulkan/vulkan.h>

#include <mutex>

namespace crd::gpu
{

// One suballocation (or dedicated allocation). Opaque to consumers beyond memory/offset/mapped.
struct GpuAllocation
{
    VkDeviceMemory                     memory = VK_NULL_HANDLE;
    VkDeviceSize                       offset = 0;
    VkDeviceSize                       size   = 0;
    void*                              mapped = nullptr; // this allocation's bytes (host-visible types only)
    crd::u32                           block_index = 0xFFFFFFFFU;
    crd::u32                           memory_type_index = 0;
    crd::memory::OffsetAllocator::Allocation suballoc{};
    bool                               dedicated = false;

    [[nodiscard]] bool valid() const noexcept { return memory != VK_NULL_HANDLE; }
};

class VulkanGpuAllocator
{
public:
    VulkanGpuAllocator(VkPhysicalDevice physical, VkDevice device);
    ~VulkanGpuAllocator(); // destroy_all in the BODY — teardown order is explicit, never member-dtor implicit

    VulkanGpuAllocator(const VulkanGpuAllocator&)            = delete;
    VulkanGpuAllocator& operator=(const VulkanGpuAllocator&) = delete;
    VulkanGpuAllocator(VulkanGpuAllocator&&)                 = delete;
    VulkanGpuAllocator& operator=(VulkanGpuAllocator&&)      = delete;

    // Free every block's VkDeviceMemory. MUST run while the VkDevice is alive. Idempotent; tombstones the allocator.
    void destroy_all() noexcept;

    // Bind-ready memory for `reqs`: a suballocation from a pooled block (or a dedicated VkDeviceMemory for ≥16 MiB).
    // `linear` separates buffer/linear-image pools from optimal-tiling image pools (bufferImageGranularity safety).
    // `map` requests a CPU pointer (host-visible `required` flags only). Returns false on exhaustion — never fatal.
    [[nodiscard]] bool allocate(const VkMemoryRequirements& reqs, VkMemoryPropertyFlags required, bool linear, bool map,
                                GpuAllocation& out);

    // Return a suballocation to its block (or free a dedicated allocation). No-op on an invalid/torn-down allocation.
    void free(const GpuAllocation& allocation) noexcept;

    // Diagnostic (the S6 gate): pooled VkDeviceMemory blocks alive. Small across many small allocations = the proof.
    [[nodiscard]] crd::u32 block_count() const noexcept;

    // S7 (the compaction primitive): release EMPTY pooled blocks (live_count == 0) back to the driver — a drained
    // pool returns its VkDeviceMemory instead of squatting on it. Safe any time (empty blocks have no borrowers).
    // Returns the number of blocks released. (Live-suballocation RELOCATION defrag rides the buffer-bundle pass.)
    crd::u32 compact() noexcept;

private:
    struct Block;

    [[nodiscard]] Block* create_block(crd::u32 mti, bool linear, VkDeviceSize at_least);

    VkDevice                         m_device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties m_mem_props{};
    VkDeviceSize                     m_non_coherent_atom = 1;
    mutable std::mutex               m_mutex;
    crd::containers::Array<Block*>   m_blocks;
    bool                             m_torn_down = false;
};

} // namespace crd::gpu
