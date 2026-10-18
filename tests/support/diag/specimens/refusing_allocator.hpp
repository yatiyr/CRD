#pragma once

// The refusing allocator of the contract-fatal specimens (expected_fatal.hpp). It grants every request from the
// default allocator until armed, then returns nullptr for the requests its rule names, as an exhausted sub-budget
// allocator does (allocator.hpp: a linear, stack, pool or growable linear allocator refuses that way). A mode arms the
// rule that makes the allocation it is about the run's first refused one; a refusal that lands elsewhere first reaches
// another fatal (CRD_DIAG_FATAL_OTHER) or a crash, and the mode fails. The rules:
//   refuse_all()                    every later request
//   refuse_shape(size, alignment)   every later request of exactly that size and alignment
//   refuse_aligned(min_alignment)   every later request asking for at least that alignment
//   refuse_after(grants)            every request after the next `grants` ones
// refusals() counts the requests refused so far.

#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd_diag_fatal
{
class RefusingAllocator final : public crd::memory::IAllocator
{
public:
    explicit RefusingAllocator(const char* name = "fatal-specimen-refusing") noexcept { m_name = name; }

    void* allocate(crd::usize size, crd::usize alignment) override { return try_allocate(size, alignment); }
    [[nodiscard]] void* try_allocate(crd::usize size, crd::usize alignment) override
    {
        if (refuses(size, alignment))
        {
            ++m_refusals;
            return nullptr;
        }
        return crd::memory::default_allocator()->try_allocate(size, alignment);
    }
    void deallocate(void* p) noexcept override { crd::memory::default_allocator()->deallocate(p); }
    [[nodiscard]] bool owns(const void* p) const noexcept override { return p != nullptr; }

    void refuse_all() noexcept { m_rule = Rule::All; }
    void refuse_shape(crd::usize size, crd::usize alignment) noexcept
    {
        m_rule = Rule::Shape;
        m_size = size;
        m_alignment = alignment;
    }
    void refuse_aligned(crd::usize min_alignment) noexcept
    {
        m_rule = Rule::Aligned;
        m_alignment = min_alignment;
    }
    void refuse_after(crd::usize grants) noexcept
    {
        m_rule = Rule::After;
        m_grants = grants;
    }
    [[nodiscard]] crd::usize refusals() const noexcept { return m_refusals; }

private:
    enum class Rule : crd::u8
    {
        None,
        All,
        Shape,
        Aligned,
        After,
    };

    [[nodiscard]] bool refuses(crd::usize size, crd::usize alignment) noexcept
    {
        switch (m_rule)
        {
            case Rule::None:
                return false;
            case Rule::All:
                return true;
            case Rule::Shape:
                return size == m_size && alignment == m_alignment;
            case Rule::Aligned:
                return alignment >= m_alignment;
            case Rule::After:
                if (m_grants > 0U)
                {
                    --m_grants;
                    return false;
                }
                return true;
        }
        return false;
    }

    Rule m_rule = Rule::None;
    crd::usize m_size = 0U;
    crd::usize m_alignment = 0U;
    crd::usize m_grants = 0U;
    crd::usize m_refusals = 0U;
};
} // namespace crd_diag_fatal
