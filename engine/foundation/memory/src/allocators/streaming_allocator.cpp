#include <crd/memory/allocators/streaming_allocator.hpp>

#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/log_channel.hpp>

namespace crd::memory
{
namespace
{
// Bounds the pressure loop: a pathological policy (or a concurrent allocation in
// another category re-claiming freed space) must not livelock the load path. After
// this many evict-then-retry rounds we give up gracefully with nullptr.
constexpr int kMaxEvictRetries = 16;
} // namespace

StreamingAllocator::StreamingAllocator() : StreamingAllocator(Config{}) {}

StreamingAllocator::StreamingAllocator(const Config& cfg, IResidencyPolicy* policy, const char* name)
    : m_name(name), m_num_categories(cfg.num_categories <= kMaxCategories ? cfg.num_categories : kMaxCategories),
      m_policy(policy != nullptr ? policy : &m_null_policy),
      m_resident_vm(VirtualMemoryAllocator::Config{cfg.reserve_bytes}, "StreamingAllocator.resident"),
      m_resident_heap(cfg.resident_chunk_bytes, &m_resident_vm, "StreamingAllocator.heap"),
      m_staging(cfg.staging_bytes, &m_resident_vm, cfg.staging_epochs, "StreamingAllocator.staging")
{
    CRD_ASSERT_MSG(cfg.num_categories >= 1 && cfg.num_categories <= kMaxCategories,
                   "StreamingAllocator: num_categories out of range [1, kMaxCategories]");
}

StreamingAllocator::~StreamingAllocator() = default;

void* StreamingAllocator::try_allocate_resident(CategoryId category, usize size, usize alignment)
{
    CRD_ASSERT(category < m_num_categories);
    // Requests no resident heap state can serve are refused before the policy is asked: evicting cannot help, so
    // shedding resident data for them only destroys it, and the heap would grow a chunk for each attempt (a
    // non-power-of-two alignment, a size past the whole reservation, an alignment past any chunk; DIAG.10a
    // allocator fuzzing).
    if (size == 0 || !is_pow2(alignment) || size > m_resident_vm.reserved_bytes() ||
        alignment > GrowableTlsfAllocator::max_chunk_bytes())
    {
        return nullptr;
    }

    for (int attempt = 0; attempt <= kMaxEvictRetries; ++attempt)
    {
        {
            const std::lock_guard<std::mutex> g(m_resident_mtx);
            Budget&   b   = m_budgets[category];
            const u64 cur = b.used.load(std::memory_order_relaxed);
            const u64 lim = b.limit.load(std::memory_order_relaxed);
            if (static_cast<u64>(size) > lim)
            {
                return nullptr; // larger than the whole budget: evicting cannot help (DIAG.3a)
            }
            if (cur <= lim - static_cast<u64>(size)) // never `cur + size`, which wraps for a huge size
            {
                void* p = m_resident_heap.try_allocate(size, alignment);
                if (p != nullptr)
                {
                    // Charge the ACTUAL block size so release un-charges the same amount, and hold the budget to
                    // that charge: checking only the request let the heap's rounding carry the category past its
                    // budget (DIAG.10a allocator fuzzing). A block rounded past the remaining budget goes back.
                    const u64 charge = static_cast<u64>(m_resident_heap.allocation_size(p));
                    if (charge <= lim - cur)
                    {
                        b.used.fetch_add(charge, std::memory_order_relaxed);
                        return p;
                    }
                    m_resident_heap.deallocate(p);
                    if (charge > lim)
                    {
                        return nullptr; // the rounded block alone exceeds the budget: evicting cannot help
                    }
                }
                // Within budget but the resident heap is physically full, or the rounded block does not fit the
                // remaining budget -> fall through to eviction (freeing this category's blocks frees both).
            }
        } // release the lock BEFORE evicting: the policy calls release_resident, which
          // re-locks; holding it here would deadlock (std::mutex is non-recursive).

        const u64 freed = m_policy->evict(*this, category, static_cast<u64>(size));
        if (freed == 0)
        {
            return nullptr; // policy cannot shed more -> graceful failure (no fatal)
        }
        // else: space was freed -> retry the allocation (bounded by kMaxEvictRetries).
    }

    // Pressure never resolved within the retry budget (pathological policy, or other
    // categories repeatedly re-claiming the freed space). Fail gracefully.
    CRD_LOG_WARN(g_log_memory, "StreamingAllocator: category {} pressure unresolved after {} evict retries", category,
                 kMaxEvictRetries);
    return nullptr;
}

void StreamingAllocator::release_resident(CategoryId category, void* p) noexcept
{
    if (p == nullptr)
    {
        return;
    }
    CRD_ASSERT(category < m_num_categories);

    const std::lock_guard<std::mutex> g(m_resident_mtx);
    // Query the charged size BEFORE freeing (the block is gone after deallocate).
    const u64 sz = static_cast<u64>(m_resident_heap.allocation_size(p));
    m_resident_heap.deallocate(p);

    Budget&   b   = m_budgets[category];
    const u64 cur = b.used.load(std::memory_order_relaxed);
    const u64 dec = sz <= cur ? sz : cur; // saturating (defensive against double-release)
    b.used.store(cur - dec, std::memory_order_relaxed);
}

void* StreamingAllocator::try_stage(usize size, usize alignment) noexcept
{
    return m_staging.try_claim(size, alignment);
}

void StreamingAllocator::begin_staging_epoch(u64 fence) noexcept
{
    m_staging.begin_epoch(fence);
}

void StreamingAllocator::retire_staging(u64 completed_fence) noexcept
{
    m_staging.retire(completed_fence);
}

void StreamingAllocator::set_budget(CategoryId category, u64 bytes) noexcept
{
    CRD_ASSERT(category < m_num_categories);
    m_budgets[category].limit.store(bytes, std::memory_order_relaxed);
}

u64 StreamingAllocator::budget(CategoryId category) const noexcept
{
    CRD_ASSERT(category < m_num_categories);
    return m_budgets[category].limit.load(std::memory_order_relaxed);
}

u64 StreamingAllocator::used(CategoryId category) const noexcept
{
    CRD_ASSERT(category < m_num_categories);
    return m_budgets[category].used.load(std::memory_order_relaxed);
}
} // namespace crd::memory
