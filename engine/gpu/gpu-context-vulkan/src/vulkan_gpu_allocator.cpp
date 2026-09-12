// vulkan_gpu_allocator.cpp — RET-4 pt 2: the S6 suballocation core, absorbed from crd-rhi-vulkan (ADR-0085/0105).

#include <crd/gpu/vulkan_gpu_allocator.hpp>

#include <crd/memory/allocator.hpp>

#include <new>

namespace crd::gpu
{

namespace
{
constexpr VkDeviceSize kDedicatedThreshold = VkDeviceSize{16} << 20;  // ≥ 16 MiB → its own VkDeviceMemory
constexpr VkDeviceSize kMaxBlockSize       = VkDeviceSize{256} << 20; // pooled-block ceiling
constexpr VkDeviceSize kMinBlockSize       = VkDeviceSize{16} << 20;  // pooled-block floor

[[nodiscard]] crd::u32 pick_memory_type(const VkPhysicalDeviceMemoryProperties& props, crd::u32 type_bits,
                                        VkMemoryPropertyFlags required) noexcept
{
    for (crd::u32 i = 0; i < props.memoryTypeCount; ++i)
    {
        const bool allowed = (type_bits & (1U << i)) != 0U;
        if (allowed && (props.memoryTypes[i].propertyFlags & required) == required) { return i; }
    }
    return 0xFFFFFFFFU;
}
} // namespace

struct VulkanGpuAllocator::Block
{
    VkDeviceMemory               memory = VK_NULL_HANDLE;
    VkDeviceSize                 size   = 0;
    crd::u32                     memory_type_index = 0;
    bool                         linear            = false;
    void*                        mapped            = nullptr;
    crd::u32                     live_count        = 0;
    crd::memory::OffsetAllocator oa; // by value: a Block is heap-allocated and never moved

    Block(VkDeviceMemory mem, VkDeviceSize sz, crd::u32 mti, bool lin, void* map_ptr, crd::u32 cap)
        : memory(mem), size(sz), memory_type_index(mti), linear(lin), mapped(map_ptr),
          oa(cap, 4096U, crd::memory::default_allocator(), "GpuBlock")
    {
    }
};

VulkanGpuAllocator::VulkanGpuAllocator(VkPhysicalDevice physical, VkDevice device)
    : m_device(device), m_blocks(crd::memory::default_allocator())
{
    vkGetPhysicalDeviceMemoryProperties(physical, &m_mem_props);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    m_non_coherent_atom = props.limits.nonCoherentAtomSize == 0 ? 1 : props.limits.nonCoherentAtomSize;
}

VulkanGpuAllocator::~VulkanGpuAllocator() { destroy_all(); }

void VulkanGpuAllocator::destroy_all() noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (crd::usize i = 0; i < m_blocks.size(); ++i)
    {
        Block* b = m_blocks[i];
        if (b == nullptr) { continue; } // a compacted tombstone
        if (b->mapped != nullptr) { vkUnmapMemory(m_device, b->memory); }
        vkFreeMemory(m_device, b->memory, nullptr);
        b->~Block();
        crd::memory::default_allocator()->deallocate(b);
    }
    m_blocks.clear();
    m_torn_down = true;
}

VulkanGpuAllocator::Block* VulkanGpuAllocator::create_block(crd::u32 mti, bool linear, VkDeviceSize at_least)
{
    const crd::u32     heap_index = m_mem_props.memoryTypes[mti].heapIndex;
    const VkDeviceSize heap_size  = m_mem_props.memoryHeaps[heap_index].size;
    VkDeviceSize       block_size = kMaxBlockSize < heap_size / 8 ? kMaxBlockSize : heap_size / 8;
    if (block_size < kMinBlockSize) { block_size = kMinBlockSize; }
    if (block_size < at_least) { block_size = at_least; }

    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = block_size;
    mai.memoryTypeIndex = mti;
    VkDeviceMemory mem  = VK_NULL_HANDLE;
    if (vkAllocateMemory(m_device, &mai, nullptr, &mem) != VK_SUCCESS) { return nullptr; }

    void* mapped = nullptr;
    if ((m_mem_props.memoryTypes[mti].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U)
    {
        if (vkMapMemory(m_device, mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        {
            vkFreeMemory(m_device, mem, nullptr);
            return nullptr;
        }
    }

    void* raw = crd::memory::default_allocator()->allocate(sizeof(Block), alignof(Block));
    auto* b   = new (raw)
        Block(mem, block_size, mti, linear, mapped, static_cast<crd::u32>(block_size < 0xFFFFFFFFULL ? block_size : 0xFFFFFFFFULL));
    for (crd::usize i = 0; i < m_blocks.size(); ++i) // reuse a compacted tombstone slot (index stability)
    {
        if (m_blocks[i] == nullptr)
        {
            m_blocks[i] = b;
            return b;
        }
    }
    m_blocks.push_back(b);
    return b;
}

bool VulkanGpuAllocator::allocate(const VkMemoryRequirements& reqs, VkMemoryPropertyFlags required, bool linear,
                                  bool map, GpuAllocation& out)
{
    out = {};
    const crd::u32 mti = pick_memory_type(m_mem_props, reqs.memoryTypeBits, required);
    if (mti == 0xFFFFFFFFU) { return false; }

    VkDeviceSize alignment = reqs.alignment == 0 ? 1 : reqs.alignment;
    const bool   host_vis  = (m_mem_props.memoryTypes[mti].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U;
    const bool   host_coh  = (m_mem_props.memoryTypes[mti].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U;
    if (host_vis && !host_coh && alignment < m_non_coherent_atom) { alignment = m_non_coherent_atom; }

    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_torn_down) { return false; }

    if (reqs.size >= kDedicatedThreshold) // its own VkDeviceMemory, never pooled
    {
        VkMemoryAllocateInfo mai{};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = reqs.size;
        mai.memoryTypeIndex = mti;
        VkDeviceMemory mem  = VK_NULL_HANDLE;
        if (vkAllocateMemory(m_device, &mai, nullptr, &mem) != VK_SUCCESS) { return false; }
        void* mapped = nullptr;
        if (map && host_vis && vkMapMemory(m_device, mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        {
            vkFreeMemory(m_device, mem, nullptr);
            return false;
        }
        out.memory            = mem;
        out.offset            = 0;
        out.size              = reqs.size;
        out.mapped            = mapped;
        out.memory_type_index = mti;
        out.dedicated         = true;
        return true;
    }

    const auto size32 = static_cast<crd::u32>(reqs.size);
    const auto algn32 = static_cast<crd::u32>(alignment);
    const auto fill   = [&](Block& b, crd::u32 index, const crd::memory::OffsetAllocator::Allocation& a) {
        out.memory            = b.memory;
        out.offset            = a.offset;
        out.size              = reqs.size;
        out.mapped            = b.mapped != nullptr ? static_cast<crd::u8*>(b.mapped) + a.offset : nullptr;
        out.block_index       = index;
        out.memory_type_index = mti;
        out.suballoc          = a;
        ++b.live_count;
    };

    for (crd::u32 i = 0; i < static_cast<crd::u32>(m_blocks.size()); ++i)
    {
        Block* b = m_blocks[i];
        if (b == nullptr || b->memory_type_index != mti || b->linear != linear) { continue; }
        const crd::memory::OffsetAllocator::Allocation a = b->oa.allocate(size32, algn32);
        if (a.valid())
        {
            fill(*b, i, a);
            return true;
        }
    }

    Block* nb = create_block(mti, linear, reqs.size);
    if (nb == nullptr) { return false; }
    const crd::memory::OffsetAllocator::Allocation a = nb->oa.allocate(size32, algn32);
    if (!a.valid()) { return false; } // a fresh block always fits a sub-threshold request
    crd::u32 nb_index = 0;
    for (crd::u32 i = 0; i < static_cast<crd::u32>(m_blocks.size()); ++i) // the slot create_block placed it in
    {
        if (m_blocks[i] == nb)
        {
            nb_index = i;
            break;
        }
    }
    fill(*nb, nb_index, a);
    return true;
}

void VulkanGpuAllocator::free(const GpuAllocation& allocation) noexcept
{
    if (!allocation.valid()) { return; }
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_torn_down) { return; } // the defensive tombstone — the lifetime contract is the law, this is the net
    if (allocation.dedicated)
    {
        vkFreeMemory(m_device, allocation.memory, nullptr); // implicitly unmaps
        return;
    }
    if (allocation.block_index >= m_blocks.size()) { return; }
    Block* b = m_blocks[allocation.block_index];
    if (b == nullptr) { return; } // freed into a compacted tombstone — the lifetime contract was violated upstream
    b->oa.free(allocation.suballoc);
    --b->live_count;
}

crd::u32 VulkanGpuAllocator::block_count() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    crd::u32 alive = 0;
    for (crd::usize i = 0; i < m_blocks.size(); ++i)
    {
        if (m_blocks[i] != nullptr) { ++alive; }
    }
    return alive;
}

crd::u32 VulkanGpuAllocator::compact() noexcept
{
    // Index stability is the law: live GpuAllocations store their block_index, so a live block NEVER moves.
    // Released slots become nullptr TOMBSTONES; create_block reuses them before appending.
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (m_torn_down) { return 0; }
    crd::u32 released = 0;
    for (crd::usize i = 0; i < m_blocks.size(); ++i)
    {
        Block* b = m_blocks[i];
        if (b == nullptr || b->live_count != 0U) { continue; }
        if (b->mapped != nullptr) { vkUnmapMemory(m_device, b->memory); }
        vkFreeMemory(m_device, b->memory, nullptr);
        b->~Block();
        crd::memory::default_allocator()->deallocate(b);
        m_blocks[i] = nullptr;
        ++released;
    }
    return released;
}

} // namespace crd::gpu
