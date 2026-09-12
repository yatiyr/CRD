#pragma once

// streaming_category_allocator.hpp — RET-3 (ADR-0085 S5 + ADR-0105): the IAllocator VIEW over ONE StreamingAllocator
// category's RESIDENT store. This is the seam the StreamingAllocator was built toward ("the ResourceManager
// integration lands with the first real streaming consumer") — any allocator-aware consumer (the resource LOADERS
// foremost) plugs a category view in as its payload heap and lives under the streaming budgets + residency policy
// with zero code changes on its side.
//
// Contract mapping (deliberate):
//   allocate()      → try_allocate_resident; a nullptr (over-budget AND the policy could shed nothing) is treated as
//                     the IAllocator OOM contract demands — CRD_FATAL. Budgeted-but-graceful callers use…
//   try_allocate()  → the honest streaming path: nullptr on budget exhaustion, no fatal (IAllocator's non-throwing
//                     overload exists for exactly this).
//   deallocate()    → release_resident (O(1), un-charges the category).
// THREAD-SAFETY: the StreamingAllocator's resident path is internally serialized — this view is safe to share across
// async-load fibers WITHOUT a ThreadSafeAllocator wrapper.

#include <crd/core/assert.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/streaming_allocator.hpp>

namespace crd::memory
{

class StreamingCategoryAllocator final : public IAllocator
{
public:
    StreamingCategoryAllocator(StreamingAllocator* sa, CategoryId category) noexcept : m_sa(sa), m_category(category)
    {
        CRD_ASSERT(sa != nullptr && category < sa->num_categories());
    }

    void* allocate(usize size, usize alignment = kDefaultAlignment) override
    {
        void* p = m_sa->try_allocate_resident(m_category, size, alignment);
        CRD_ASSERT_MSG(p != nullptr || size == 0,
                       "StreamingCategoryAllocator: resident store exhausted (over budget, nothing to shed)");
        return p;
    }

    [[nodiscard]] void* try_allocate(usize size, usize alignment = kDefaultAlignment) override
    {
        return m_sa->try_allocate_resident(m_category, size, alignment);
    }

    void deallocate(void* p) noexcept override
    {
        if (p != nullptr) { m_sa->release_resident(m_category, p); }
    }

    [[nodiscard]] bool owns(const void* /*p*/) const noexcept override
    {
        return false; // ownership queries route to the StreamingAllocator's own diagnostics, not per-category views
    }

    [[nodiscard]] StreamingAllocator& streaming() const noexcept { return *m_sa; }
    [[nodiscard]] CategoryId          category() const noexcept { return m_category; }

private:
    StreamingAllocator* m_sa       = nullptr;
    CategoryId          m_category = 0;
};

} // namespace crd::memory
