#pragma once

// crd-containers -- generation-checked handle map.
//
// A dense store of T addressed by a small `Handle{index, generation}`. Every access validates the
// handle's generation against the slot's; erasing a slot bumps its generation, so every handle to
// that slot -- and any borrow derived from one -- goes stale at once and get()/contains() reject
// it instead of returning a reused element (the classic dangling-handle / use-after-free-of-a-slot
// bug). Handles are trivially copyable values, safe to store in containers and pass to
// callbacks: the check happens at dereference, not at copy.
//
// T must be move-constructible and move-assignable (erase moves the element out to run its
// destructor; reuse move-assigns the new element in). No T default-construction is required.
//
// Contract: docs/design/runtime-diagnostics.md; ADR-0133.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>

namespace crd::containers
{

template <typename T>
class SlotMap
{
public:
    struct Handle
    {
        crd::u32 index      = 0xFFFFFFFFU;
        crd::u32 generation = 0U; // 0 is reserved: a default Handle is always invalid

        [[nodiscard]] bool valid() const noexcept { return index != 0xFFFFFFFFU && generation != 0U; }
        [[nodiscard]] bool operator==(const Handle& o) const noexcept
        {
            return index == o.index && generation == o.generation;
        }
    };

    explicit SlotMap(memory::IAllocator* alloc = memory::default_allocator())
        : m_values(alloc), m_generation(alloc), m_occupied(alloc), m_free(alloc)
    {
    }

    // Insert `value`, returning a fresh handle. Reuses a retired slot when one is free.
    Handle insert(T value)
    {
        if (m_free.size() > 0U)
        {
            const crd::u32 idx = m_free[m_free.size() - 1U];
            m_free.pop_back();
            m_values[idx]    = static_cast<T&&>(value);
            m_occupied[idx]  = 1U;
            ++m_size;
            return Handle{idx, m_generation[idx]};
        }

        const crd::u32 idx = static_cast<crd::u32>(m_values.size());
        m_values.push_back(static_cast<T&&>(value));
        m_generation.push_back(1U); // generations start at 1 (0 = null handle)
        m_occupied.push_back(1U);
        ++m_size;
        return Handle{idx, 1U};
    }

    // True iff the handle still refers to a live slot at its generation.
    [[nodiscard]] bool contains(Handle h) const noexcept
    {
        return h.valid() && h.index < m_generation.size() && m_occupied[h.index] != 0U &&
               m_generation[h.index] == h.generation;
    }

    // The element, or nullptr if the handle is stale/invalid. The returned pointer is itself a
    // borrow: it is valid only until the next erase()/insert() that could relocate storage.
    [[nodiscard]] T* get(Handle h) noexcept { return contains(h) ? &m_values[h.index] : nullptr; }
    [[nodiscard]] const T* get(Handle h) const noexcept { return contains(h) ? &m_values[h.index] : nullptr; }

    // Retire the slot. Returns false for a stale/invalid handle (so a double-erase is rejected,
    // not a corruption). Bumps the slot generation so every outstanding handle to it goes stale.
    bool erase(Handle h) noexcept
    {
        if (!contains(h))
            return false;

        const crd::u32 idx = h.index;
        {
            T moved_out = static_cast<T&&>(m_values[idx]); // run T's destructor via the temporary
            (void)moved_out;
        }
        m_occupied[idx] = 0U;
        ++m_generation[idx];
        if (m_generation[idx] == 0U) // explicit wrap guard: never reuse the null generation
            m_generation[idx] = 1U;
        m_free.push_back(idx);
        --m_size;
        return true;
    }

    [[nodiscard]] crd::usize size() const noexcept { return m_size; }
    [[nodiscard]] bool       empty() const noexcept { return m_size == 0U; }
    [[nodiscard]] crd::usize slot_capacity() const noexcept { return m_values.size(); }

private:
    Array<T>        m_values;     // one entry per slot; retired slots hold a moved-from T
    Array<crd::u32> m_generation; // per-slot generation, parallel to m_values
    Array<crd::u8>  m_occupied;   // per-slot live flag, parallel to m_values
    Array<crd::u32> m_free;       // stack of retired slot indices available for reuse
    crd::usize      m_size = 0U;  // live element count
};

} // namespace crd::containers
