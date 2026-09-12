#include <crd/memory/allocators/offset_allocator.hpp>

#include <crd/core/assert.hpp>
#include <crd/memory/alignment.hpp>
#include <crd/memory/allocator.hpp>

#include <bit>

// Algorithm: Sebastian Aaltonen's OffsetAllocator (2023, MIT — sebbbi/OffsetAllocator),
// reimplemented in Cerid idiom. The float-bin layout, two-level bitmap search, and
// neighbour-coalescing free list follow the reference; naming/types are Cerid's.
namespace crd::memory
{
namespace
{
// ---- 8-bit float size-bin distribution (3-bit mantissa + 5-bit exponent) -------
constexpr u32 kMantissaBits  = 3;
constexpr u32 kMantissaValue = 1U << kMantissaBits; // 8
constexpr u32 kMantissaMask  = kMantissaValue - 1U; // 7

// Round a size UP to its bin float (allocation: pick a bin that can hold >= size).
[[nodiscard]] u32 size_to_float_round_up(u32 size) noexcept
{
    u32 exp      = 0;
    u32 mantissa = 0;
    if (size < kMantissaValue)
    {
        mantissa = size;
    }
    else
    {
        const u32 highest_set_bit   = 31U - static_cast<u32>(std::countl_zero(size));
        const u32 mantissa_start    = highest_set_bit - kMantissaBits;
        exp                         = mantissa_start + 1U;
        mantissa                    = (size >> mantissa_start) & kMantissaMask;
        const u32 low_bits_mask     = (1U << mantissa_start) - 1U;
        if ((size & low_bits_mask) != 0U) // round up (a non-exact size needs the next bin)
        {
            ++mantissa;
        }
    }
    return (exp << kMantissaBits) + mantissa; // mantissa carry into exp is intentional
}

// Round a size DOWN to its bin float (insertion: a free region goes in the bin <= its size).
[[nodiscard]] u32 size_to_float_round_down(u32 size) noexcept
{
    u32 exp      = 0;
    u32 mantissa = 0;
    if (size < kMantissaValue)
    {
        mantissa = size;
    }
    else
    {
        const u32 highest_set_bit = 31U - static_cast<u32>(std::countl_zero(size));
        const u32 mantissa_start  = highest_set_bit - kMantissaBits;
        exp                       = mantissa_start + 1U;
        mantissa                  = (size >> mantissa_start) & kMantissaMask;
    }
    return (exp << kMantissaBits) + mantissa;
}

// Lowest set bit at index >= start, or kNotFound if none.
constexpr u32 kNotFound = 0xFFFFFFFFU;
[[nodiscard]] u32 lowest_set_bit_after(u32 mask, u32 start) noexcept
{
    if (start >= 32U)
    {
        return kNotFound;
    }
    const u32 masked = mask & (0xFFFFFFFFU << start);
    return masked == 0U ? kNotFound : static_cast<u32>(std::countr_zero(masked));
}
} // namespace

struct OffsetAllocator::Node
{
    u32  data_offset   = 0;
    u32  data_size     = 0;
    u32  bin_prev      = kUnused;
    u32  bin_next      = kUnused;
    u32  neighbor_prev = kUnused;
    u32  neighbor_next = kUnused;
    bool used          = false;
};

OffsetAllocator::OffsetAllocator(u32 capacity, u32 max_allocations, IAllocator* alloc, const char* name)
    : m_alloc(alloc != nullptr ? alloc : default_allocator()), m_name(name), m_capacity(capacity),
      m_max_allocs(max_allocations)
{
    CRD_ASSERT_MSG(capacity > 0, "OffsetAllocator: capacity must be > 0");
    CRD_ASSERT_MSG(max_allocations > 0, "OffsetAllocator: max_allocations must be > 0");

    m_nodes      = static_cast<Node*>(m_alloc->allocate(sizeof(Node) * m_max_allocs, alignof(Node)));
    m_free_nodes = static_cast<u32*>(m_alloc->allocate(sizeof(u32) * m_max_allocs, alignof(u32)));
    for (u32 i = 0; i < m_max_allocs; ++i)
    {
        m_nodes[i] = Node{};
    }
    reset();
}

OffsetAllocator::~OffsetAllocator()
{
    m_alloc->deallocate(m_free_nodes);
    m_alloc->deallocate(m_nodes);
}

void OffsetAllocator::reset() noexcept
{
    m_free_storage  = 0;
    m_used_bins_top = 0;
    for (u8& b : m_used_bins)
    {
        b = 0;
    }
    for (u32& bi : m_bin_indices)
    {
        bi = kUnused;
    }
    // Free-node stack: all indices available, top at m_max_allocs - 1.
    for (u32 i = 0; i < m_max_allocs; ++i)
    {
        m_free_nodes[i] = i;
    }
    m_free_offset = m_max_allocs - 1U;

    // One free region spanning the whole span.
    (void)insert_node_into_bin(m_capacity, 0);
}

u32 OffsetAllocator::insert_node_into_bin(u32 size, u32 data_offset) noexcept
{
    const u32 bin_index = size_to_float_round_down(size);
    const u32 top_bin   = bin_index >> kMantissaBits;
    const u32 leaf_bin  = bin_index & kMantissaMask;

    // First node in this bin? Light up the two-level bitmap.
    if (m_bin_indices[bin_index] == kUnused)
    {
        m_used_bins[top_bin] |= static_cast<u8>(1U << leaf_bin);
        m_used_bins_top |= (1U << top_bin);
    }

    CRD_ASSERT_MSG(m_free_offset != kUnused, "OffsetAllocator: node pool exhausted (raise max_allocations)");
    const u32 node_index = m_free_nodes[m_free_offset--];

    const u32 old_head            = m_bin_indices[bin_index];
    m_nodes[node_index].data_offset = data_offset;
    m_nodes[node_index].data_size   = size;
    m_nodes[node_index].bin_prev    = kUnused;
    m_nodes[node_index].bin_next    = old_head;
    m_nodes[node_index].used        = false;
    if (old_head != kUnused)
    {
        m_nodes[old_head].bin_prev = node_index;
    }
    m_bin_indices[bin_index] = node_index;

    m_free_storage += size;
    return node_index;
}

void OffsetAllocator::remove_node_from_bin(u32 node_index) noexcept
{
    Node& node = m_nodes[node_index];
    if (node.bin_prev != kUnused)
    {
        // Not the bin head — just unlink.
        m_nodes[node.bin_prev].bin_next = node.bin_next;
        if (node.bin_next != kUnused)
        {
            m_nodes[node.bin_next].bin_prev = node.bin_prev;
        }
    }
    else
    {
        // Bin head — repoint the bin, clear bitmap bits if it became empty.
        const u32 bin_index      = size_to_float_round_down(node.data_size);
        m_bin_indices[bin_index] = node.bin_next;
        if (node.bin_next != kUnused)
        {
            m_nodes[node.bin_next].bin_prev = kUnused;
        }
        else
        {
            const u32 top_bin  = bin_index >> kMantissaBits;
            const u32 leaf_bin = bin_index & kMantissaMask;
            m_used_bins[top_bin] &= static_cast<u8>(~(1U << leaf_bin));
            if (m_used_bins[top_bin] == 0)
            {
                m_used_bins_top &= ~(1U << top_bin);
            }
        }
    }

    m_free_storage -= node.data_size;
    m_free_nodes[++m_free_offset] = node_index; // recycle the node slot
}

OffsetAllocator::Allocation OffsetAllocator::allocate(u32 size, u32 alignment) noexcept
{
    CRD_ASSERT(is_pow2(alignment));
    if (size == 0)
    {
        return {};
    }
    // Over-allocate by (alignment - 1) so we can return an aligned offset inside the
    // region; free() works off the metadata node (the un-aligned region), so the
    // padding is reclaimed with it. alignment == 1 wastes nothing.
    const u64 padded64 = static_cast<u64>(size) + (static_cast<u64>(alignment) - 1U);
    if (padded64 > m_capacity)
    {
        return {}; // cannot possibly fit
    }
    const u32 padded = static_cast<u32>(padded64);

    // ---- find the lowest non-empty bin that can hold `padded` ------------------
    const u32 min_bin  = size_to_float_round_up(padded);
    const u32 min_top  = min_bin >> kMantissaBits;
    const u32 min_leaf = min_bin & kMantissaMask;

    u32 top_bin  = min_top;
    u32 leaf_bin = kNotFound;

    if ((m_used_bins_top & (1U << min_top)) != 0U)
    {
        // The min top-bin has leaves; is one of them >= min_leaf?
        leaf_bin = lowest_set_bit_after(m_used_bins[min_top], min_leaf);
    }
    if (leaf_bin == kNotFound)
    {
        // Fall to the next non-empty top-bin strictly above min_top.
        const u32 next_top = lowest_set_bit_after(m_used_bins_top, min_top + 1U);
        if (next_top == kNotFound)
        {
            return {}; // out of space
        }
        top_bin  = next_top;
        leaf_bin = static_cast<u32>(std::countr_zero(static_cast<u32>(m_used_bins[top_bin])));
    }

    const u32 bin_index  = (top_bin << kMantissaBits) | leaf_bin;
    const u32 node_index = m_bin_indices[bin_index];
    Node&     node       = m_nodes[node_index];

    const u32 total_size = node.data_size;
    node.data_size       = padded;
    node.used            = true;

    // Unlink this node from the bin head (it is reused, not recycled).
    m_bin_indices[bin_index] = node.bin_next;
    if (node.bin_next != kUnused)
    {
        m_nodes[node.bin_next].bin_prev = kUnused;
    }
    else
    {
        m_used_bins[top_bin] &= static_cast<u8>(~(1U << leaf_bin));
        if (m_used_bins[top_bin] == 0)
        {
            m_used_bins_top &= ~(1U << top_bin);
        }
    }
    m_free_storage -= total_size;

    // Split off the remainder as a new free region, linked as the next neighbour.
    const u32 remainder = total_size - padded;
    if (remainder > 0)
    {
        const u32 new_index = insert_node_into_bin(remainder, node.data_offset + padded);
        // node <-> new_index <-> node.neighbor_next
        const u32 old_next = node.neighbor_next;
        if (old_next != kUnused)
        {
            m_nodes[old_next].neighbor_prev = new_index;
        }
        m_nodes[new_index].neighbor_prev = node_index;
        m_nodes[new_index].neighbor_next = old_next;
        node.neighbor_next               = new_index;
    }

    const u32 aligned_offset = static_cast<u32>(align_up(node.data_offset, alignment));
    return Allocation{aligned_offset, node_index};
}

void OffsetAllocator::free(Allocation allocation) noexcept
{
    if (!allocation.valid())
    {
        return;
    }
    const u32 node_index = allocation.metadata;
    Node&     node       = m_nodes[node_index];
    CRD_ASSERT_MSG(node.used, "OffsetAllocator: free of an unused / double-freed allocation");

    u32 offset = node.data_offset;
    u32 size   = node.data_size;

    // Coalesce with the previous neighbour if it is free.
    if (node.neighbor_prev != kUnused && !m_nodes[node.neighbor_prev].used)
    {
        Node& prev = m_nodes[node.neighbor_prev];
        offset     = prev.data_offset;
        size += prev.data_size;
        const u32 prev_prev = prev.neighbor_prev;
        remove_node_from_bin(node.neighbor_prev);
        node.neighbor_prev = prev_prev;
    }
    // Coalesce with the next neighbour if it is free.
    if (node.neighbor_next != kUnused && !m_nodes[node.neighbor_next].used)
    {
        Node& next = m_nodes[node.neighbor_next];
        size += next.data_size;
        const u32 next_next = next.neighbor_next;
        remove_node_from_bin(node.neighbor_next);
        node.neighbor_next = next_next;
    }

    const u32 neighbor_prev = node.neighbor_prev;
    const u32 neighbor_next = node.neighbor_next;

    // Recycle this node, insert the (possibly merged) region as one free node.
    m_free_nodes[++m_free_offset] = node_index;
    const u32 combined = insert_node_into_bin(size, offset);

    if (neighbor_next != kUnused)
    {
        m_nodes[combined].neighbor_next     = neighbor_next;
        m_nodes[neighbor_next].neighbor_prev = combined;
    }
    if (neighbor_prev != kUnused)
    {
        m_nodes[combined].neighbor_prev      = neighbor_prev;
        m_nodes[neighbor_prev].neighbor_next = combined;
    }
}

OffsetAllocator::StorageReport OffsetAllocator::storage_report() const noexcept
{
    u32 largest = 0;
    // The largest free region lives in the highest non-empty bin; scan its members.
    for (u32 bin = 0; bin < 256U; ++bin)
    {
        for (u32 n = m_bin_indices[bin]; n != kUnused; n = m_nodes[n].bin_next)
        {
            if (m_nodes[n].data_size > largest)
            {
                largest = m_nodes[n].data_size;
            }
        }
    }
    return StorageReport{m_free_storage, largest};
}
} // namespace crd::memory
