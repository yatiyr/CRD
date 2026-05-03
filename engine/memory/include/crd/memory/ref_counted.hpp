#pragma once

#include <atomic>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

namespace crd::memory
{

// Intrusive reference-count base class (CRTP; ADR-0014).
//
// Usage:
//   class Foo : public RefCounted<Foo> { ... };
//   Foo* p = allocate_and_construct ...;   // refs == 1 (born with one ref)
//   p->add_ref();                           // refs == 2
//   if (p->release() == 0) destroy(...p);  // refs == 0 → caller frees
//
// The protected destructor prevents accidental deletion through a base pointer.
template <typename Derived>
class RefCounted
{
public:
    void add_ref() noexcept
    {
        m_refs.fetch_add(1, std::memory_order_relaxed);
    }

    // Decrements and returns the NEW refcount (0 means the caller should free).
    [[nodiscard]] crd::u32 release() noexcept
    {
        const crd::u32 prev = m_refs.fetch_sub(1, std::memory_order_acq_rel);
        CRD_ASSERT_MSG(prev > 0U, "RefCounted::release() on an object with zero refcount");
        return prev - 1U;
    }

    [[nodiscard]] crd::u32 use_count() const noexcept
    {
        return m_refs.load(std::memory_order_relaxed);
    }

protected:
    ~RefCounted() = default;

private:
    std::atomic<crd::u32> m_refs{1};
};

} // namespace crd::memory
