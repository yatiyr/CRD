#include <crd/memory/allocators/virtual_memory_allocator.hpp>

#include <crd/core/assert.hpp>
#include <crd/memory/alignment.hpp>

#include <cstddef>
#include <cstring>

// ---- AddressSanitizer manual poisoning --------------------------------------
// A bump arena commits whole pages but hands out only [0, alloc_pos). The tail
// [alloc_pos, commit_pos) is legitimately committed, so without help ASan can't
// catch a read past the live top. We poison that tail and unpoison on bump, so
// win-asan (a DoD-required config) catches arena overruns. Forward-declare the
// stable ASan ABI rather than chase a per-compiler header.
// NOLINTBEGIN(cppcoreguidelines-macro-usage) — feature-detect must be preprocessor
// (set from __SANITIZE_ADDRESS__ / __has_feature, which are not constexpr-visible).
#if defined(__SANITIZE_ADDRESS__)
#define CRD_VMA_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CRD_VMA_ASAN 1
#endif
#endif
#ifndef CRD_VMA_ASAN
#define CRD_VMA_ASAN 0
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

#if CRD_VMA_ASAN
extern "C" void __asan_poison_memory_region(void const volatile* addr, std::size_t size);
extern "C" void __asan_unpoison_memory_region(void const volatile* addr, std::size_t size);
#endif

namespace crd::memory
{
namespace
{
inline void asan_poison([[maybe_unused]] void* p, [[maybe_unused]] usize n) noexcept
{
#if CRD_VMA_ASAN
    if (n != 0)
    {
        __asan_poison_memory_region(p, static_cast<std::size_t>(n));
    }
#endif
}
inline void asan_unpoison([[maybe_unused]] void* p, [[maybe_unused]] usize n) noexcept
{
#if CRD_VMA_ASAN
    if (n != 0)
    {
        __asan_unpoison_memory_region(p, static_cast<std::size_t>(n));
    }
#endif
}
} // namespace

VirtualMemoryAllocator::VirtualMemoryAllocator() : VirtualMemoryAllocator(Config{}) {}

VirtualMemoryAllocator::VirtualMemoryAllocator(const Config& cfg, const char* name)
{
    m_name         = name;
    m_page         = vm::page_size();
    m_commit_block = align_up(cfg.commit_block == 0 ? m_page : cfg.commit_block, m_page);
    if (m_commit_block < m_page)
    {
        m_commit_block = m_page;
    }

    m_region = vm::reserve(cfg.reserve_bytes);
    if (!m_region.valid())
    {
        CRD_FATAL("VirtualMemoryAllocator: failed to reserve address space (raise/lower Config::reserve_bytes)");
    }
    m_base     = static_cast<u8*>(m_region.base);
    m_reserved = m_region.size;

    if (cfg.initial_commit_bytes != 0)
    {
        const usize target = cfg.initial_commit_bytes >= m_reserved ? m_reserved : cfg.initial_commit_bytes;
        if (!ensure_committed(target))
        {
            CRD_FATAL("VirtualMemoryAllocator: failed to pre-commit Config::initial_commit_bytes");
        }
    }
}

VirtualMemoryAllocator::~VirtualMemoryAllocator()
{
    // release() decommits everything + frees the address space; under ASan the
    // shadow for an unmapped range is reclaimed by the runtime, so no unpoison.
    vm::release(m_region);
    m_base       = nullptr;
    m_reserved   = 0;
    m_commit_pos = 0;
    m_alloc_pos  = 0;
}

bool VirtualMemoryAllocator::ensure_committed(usize target) noexcept
{
    if (target <= m_commit_pos)
    {
        return true;
    }
    // Round the new high-water mark up to a whole commit block, capped at the
    // (page-aligned) reservation size.
    usize new_commit = align_up(target, m_commit_block);
    if (new_commit > m_reserved)
    {
        new_commit = m_reserved;
    }
    if (!vm::commit(m_base + m_commit_pos, new_commit - m_commit_pos))
    {
        return false;
    }
    // Freshly committed pages sit above the live top -> poison until handed out.
    asan_poison(m_base + m_commit_pos, new_commit - m_commit_pos);
    m_commit_pos = new_commit;
    return true;
}

void* VirtualMemoryAllocator::bump(usize size, usize alignment) noexcept
{
    if (size == 0)
    {
        return nullptr;
    }
    const usize base_addr    = reinterpret_cast<usize>(m_base);
    const usize aligned_addr = align_up(base_addr + m_alloc_pos, alignment);
    const usize offset       = aligned_addr - base_addr;

    // Reservation-exhaustion / overflow guard.
    if (offset < m_alloc_pos || size > m_reserved || offset > m_reserved - size)
    {
        return nullptr;
    }
    const usize new_top = offset + size;

    if (new_top > m_commit_pos && !ensure_committed(new_top))
    {
        return nullptr;
    }

    // Hand out [alloc_pos, new_top): unpoison padding + payload (keeps the
    // invariant that [0, alloc_pos) is unpoisoned, [alloc_pos, commit_pos) is not).
    asan_unpoison(m_base + m_alloc_pos, new_top - m_alloc_pos);
    m_stats.on_allocate(new_top - m_alloc_pos);

    m_last_offset = offset;
    m_has_last    = true;
    m_alloc_pos   = new_top;
    return m_base + offset;
}

void* VirtualMemoryAllocator::allocate(usize size, usize alignment)
{
    CRD_ASSERT(size != 0);
    CRD_ASSERT(is_pow2(alignment));
    void* p = bump(size, alignment);
    if (p == nullptr)
    {
        CRD_FATAL("VirtualMemoryAllocator: out of reserved address space or commit failed");
    }
    return p;
}

void* VirtualMemoryAllocator::try_allocate(usize size, usize alignment)
{
    CRD_ASSERT(is_pow2(alignment));
    return bump(size, alignment);
}

void VirtualMemoryAllocator::deallocate(void* p) noexcept
{
    if (p == nullptr)
    {
        return;
    }
    // Bump arena: individual free is a no-op. Ownership check catches a pointer
    // from a different allocator being freed here (a real bug).
    CRD_ASSERT(owns(p));
}

bool VirtualMemoryAllocator::owns(const void* p) const noexcept
{
    return m_base != nullptr && p >= m_base && p < m_base + m_alloc_pos;
}

void* VirtualMemoryAllocator::reallocate(void* p, usize old_size, usize new_size, usize alignment)
{
    if (p == nullptr)
    {
        return allocate(new_size, alignment);
    }
    if (new_size == 0)
    {
        deallocate(p);
        return nullptr;
    }
    CRD_ASSERT(owns(p));

    // In-place grow/shrink iff p is the most-recent allocation (the arena top).
    if (m_has_last && p == m_base + m_last_offset)
    {
        const usize new_top = m_last_offset + new_size;
        if (new_size > m_reserved || m_last_offset > m_reserved - new_size)
        {
            CRD_FATAL("VirtualMemoryAllocator: reallocate exceeds reserved address space");
        }
        if (new_top > m_alloc_pos) // grow
        {
            if (new_top > m_commit_pos && !ensure_committed(new_top))
            {
                CRD_FATAL("VirtualMemoryAllocator: reallocate commit failed");
            }
            asan_unpoison(m_base + m_alloc_pos, new_top - m_alloc_pos);
            m_stats.on_allocate(new_top - m_alloc_pos);
        }
        else if (new_top < m_alloc_pos) // shrink
        {
            asan_poison(m_base + new_top, m_alloc_pos - new_top);
            m_stats.on_deallocate(m_alloc_pos - new_top);
        }
        m_alloc_pos = new_top;
        return p;
    }

    // Not the top allocation: bump a fresh block and copy.
    void* np = allocate(new_size, alignment);
    std::memcpy(np, p, old_size < new_size ? old_size : new_size);
    return np;
}

usize VirtualMemoryAllocator::allocation_size(const void* p) const noexcept
{
    (void)p;
    return 0; // bump arenas don't track per-allocation size
}

void VirtualMemoryAllocator::reset_to(Marker m) noexcept
{
    CRD_ASSERT(m <= m_alloc_pos);
    if (m < m_alloc_pos)
    {
        asan_poison(m_base + m, m_alloc_pos - m);
        m_stats.on_deallocate(m_alloc_pos - m);
        m_alloc_pos = m;
    }
    m_has_last = false; // can't in-place-grow across a pop
}

void VirtualMemoryAllocator::reset() noexcept
{
    reset_to(0);
}

void VirtualMemoryAllocator::purge() noexcept
{
    // Decommit whole pages above the live top; keep the page that the live top
    // sits in. Hands physical RAM back to the OS (RSS drops); addresses stay
    // reserved, so a later allocate() re-commits + zero-fills.
    const usize keep = align_up(m_alloc_pos, m_page);
    if (keep < m_commit_pos)
    {
        (void)vm::decommit(m_base + keep, m_commit_pos - keep);
        m_commit_pos = keep;
    }
}

void VirtualMemoryAllocator::reset_and_purge() noexcept
{
    reset();
    purge();
}
} // namespace crd::memory
