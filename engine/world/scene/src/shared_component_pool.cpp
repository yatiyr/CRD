// Phase 3.0 v1m4b1 — SharedComponentPool implementation (ADR-0058 pillar 5).
//
// Deduplicated, refcounted byte pool. Backing store is a single contiguous
// byte buffer sized `m_capacity * m_entry_size`, grown × 2 on overflow.
// Refcount + freelist live in parallel Arrays. High-water mark tracks the
// smallest never-acquired slot — acquire either pops from freelist or hands
// out the high-water idx and bumps it.

#include <crd/core/assert.hpp>
#include <crd/scene/shared_component_pool.hpp>

#include <cstring>
#include <utility>

namespace crd::scene
{

SharedComponentPool::SharedComponentPool(crd::memory::IAllocator* alloc,
                                         crd::usize               entry_size,
                                         crd::usize               entry_alignment) noexcept
    : m_alloc(alloc), m_entry_size(entry_size), m_entry_alignment(entry_alignment),
      m_refcounts(alloc), m_entry_hashes(alloc), m_freelist(alloc), m_hash_to_idx(alloc)
{
    CRD_ASSERT(alloc != nullptr);
    CRD_ASSERT(entry_size > 0);
    CRD_ASSERT(entry_alignment > 0);
}

SharedComponentPool::~SharedComponentPool()
{
    if (m_bytes != nullptr && m_alloc != nullptr)
    {
        m_alloc->deallocate(m_bytes);
        m_bytes = nullptr;
    }
}

SharedComponentPool::SharedComponentPool(SharedComponentPool&& other) noexcept
    : m_alloc(other.m_alloc),
      m_entry_size(other.m_entry_size),
      m_entry_alignment(other.m_entry_alignment),
      m_bytes(other.m_bytes),
      m_refcounts(std::move(other.m_refcounts)),
      m_entry_hashes(std::move(other.m_entry_hashes)),
      m_freelist(std::move(other.m_freelist)),
      m_hash_to_idx(std::move(other.m_hash_to_idx)),
      m_capacity(other.m_capacity),
      m_live_count(other.m_live_count),
      m_high_water(other.m_high_water)
{
    other.m_bytes      = nullptr;
    other.m_capacity   = 0;
    other.m_live_count = 0;
    other.m_high_water = 0;
}

SharedComponentPool& SharedComponentPool::operator=(SharedComponentPool&& other) noexcept
{
    if (this != &other)
    {
        if (m_bytes != nullptr && m_alloc != nullptr)
        {
            m_alloc->deallocate(m_bytes);
        }
        m_alloc           = other.m_alloc;
        m_entry_size      = other.m_entry_size;
        m_entry_alignment = other.m_entry_alignment;
        m_bytes           = other.m_bytes;
        m_refcounts       = std::move(other.m_refcounts);
        m_entry_hashes    = std::move(other.m_entry_hashes);
        m_freelist        = std::move(other.m_freelist);
        m_hash_to_idx     = std::move(other.m_hash_to_idx);
        m_capacity        = other.m_capacity;
        m_live_count      = other.m_live_count;
        m_high_water      = other.m_high_water;

        other.m_bytes      = nullptr;
        other.m_capacity   = 0;
        other.m_live_count = 0;
        other.m_high_water = 0;
    }
    return *this;
}

void SharedComponentPool::grow(crd::u32 min_capacity)
{
    crd::u32 new_capacity = (m_capacity == 0) ? 8U : m_capacity * 2U;
    while (new_capacity < min_capacity)
    {
        new_capacity *= 2U;
    }
    crd::u8* new_bytes = static_cast<crd::u8*>(
        m_alloc->allocate(static_cast<crd::usize>(new_capacity) * m_entry_size, m_entry_alignment));
    CRD_ASSERT(new_bytes != nullptr);
    if (m_bytes != nullptr)
    {
        std::memcpy(new_bytes, m_bytes, static_cast<crd::usize>(m_capacity) * m_entry_size);
        m_alloc->deallocate(m_bytes);
    }
    m_bytes = new_bytes;
    m_refcounts.resize(new_capacity, 0U);
    m_entry_hashes.resize(new_capacity, 0U);
    m_capacity = new_capacity;
}

crd::u32 SharedComponentPool::acquire(const void* src)
{
    CRD_ASSERT(src != nullptr);
    crd::u32 idx;
    if (m_freelist.size() > 0)
    {
        idx = m_freelist[m_freelist.size() - 1U];
        m_freelist.pop_back();
    }
    else
    {
        if (m_high_water >= m_capacity)
        {
            grow(m_high_water + 1U);
        }
        idx = m_high_water;
        ++m_high_water;
    }
    m_refcounts[idx]    = 1U;
    m_entry_hashes[idx] = 0U;  // un-deduped path; v1m4b3 callers using acquire_or_retain set it
    std::memcpy(m_bytes + static_cast<crd::usize>(idx) * m_entry_size, src, m_entry_size);
    ++m_live_count;
    return idx;
}

crd::u32 SharedComponentPool::acquire_or_retain(const void* src, crd::u64 content_hash)
{
    CRD_ASSERT(src != nullptr);
    if (auto* existing = m_hash_to_idx.find(content_hash); existing != nullptr)
    {
        const crd::u32 idx = *existing;
        CRD_ASSERT(idx < m_capacity);
        CRD_ASSERT(m_refcounts[idx] > 0
                   && "SharedComponentPool::acquire_or_retain hit a stale hash_to_idx entry");
        ++m_refcounts[idx];
        return idx;
    }
    const crd::u32 idx = acquire(src);
    m_entry_hashes[idx] = content_hash;
    m_hash_to_idx.emplace(content_hash, idx);
    return idx;
}

void SharedComponentPool::retain(crd::u32 idx)
{
    CRD_ASSERT(idx < m_capacity);
    CRD_ASSERT(m_refcounts[idx] > 0 && "SharedComponentPool::retain on freed entry");
    ++m_refcounts[idx];
}

void SharedComponentPool::release(crd::u32 idx)
{
    CRD_ASSERT(idx < m_capacity);
    CRD_ASSERT(m_refcounts[idx] > 0 && "SharedComponentPool::release on already-freed entry");
    --m_refcounts[idx];
    if (m_refcounts[idx] == 0U)
    {
        // Clean up the dedup table if the entry was acquired via
        // `acquire_or_retain` (entry_hashes != 0). Plain `acquire` sets
        // hash to 0, so we won't accidentally erase a legitimate entry.
        if (m_entry_hashes[idx] != 0U)
        {
            m_hash_to_idx.erase(m_entry_hashes[idx]);
            m_entry_hashes[idx] = 0U;
        }
        m_freelist.push_back(idx);
        --m_live_count;
    }
}

const crd::u8* SharedComponentPool::entry_bytes(crd::u32 idx) const
{
    CRD_ASSERT(idx < m_capacity);
    CRD_ASSERT(m_refcounts[idx] > 0 && "SharedComponentPool::entry_bytes on freed entry");
    return m_bytes + static_cast<crd::usize>(idx) * m_entry_size;
}

crd::u8* SharedComponentPool::entry_bytes(crd::u32 idx)
{
    CRD_ASSERT(idx < m_capacity);
    CRD_ASSERT(m_refcounts[idx] > 0 && "SharedComponentPool::entry_bytes on freed entry");
    return m_bytes + static_cast<crd::usize>(idx) * m_entry_size;
}

crd::u32 SharedComponentPool::refcount(crd::u32 idx) const noexcept
{
    if (idx >= m_capacity)
    {
        return 0U;
    }
    return m_refcounts[idx];
}

} // namespace crd::scene
