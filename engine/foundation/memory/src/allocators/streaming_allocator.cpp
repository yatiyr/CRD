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
    if (size == 0)
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
            if (cur + size <= lim)
            {
                void* p = m_resident_heap.try_allocate(size, alignment);
                if (p != nullptr)
                {
                    // Charge the ACTUAL block size so release un-charges the same amount.
                    b.used.fetch_add(static_cast<u64>(m_resident_heap.allocation_size(p)), std::memory_order_relaxed);
                    return p;
                }
                // Within budget but the resident heap is physically full -> fall through
                // to eviction (freeing this category's blocks frees heap space).
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
