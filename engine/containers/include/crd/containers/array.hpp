#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>

#include <cstring> // memcpy for trivially-copyable relocate
#include <initializer_list>
#include <iterator>
#include <new> // placement new
#include <type_traits>
#include <utility>

namespace crd::containers
{
// -----------------------------------------------------------------------
// Array<T> — a dynamic, contiguous, allocator-aware sequence container.
//
// Think of this as our std::vector. Differences:
//   - Allocator is an IAllocator* CONSTRUCTOR ARGUMENT, not a template
//     parameter. The type does not change when the allocator changes.
//   - No exceptions. push_back / reserve trigger CRD_FATAL on OOM via
//     the allocator. try_push_back / try_reserve return false instead.
//   - Growth strategy is 1.5x (Folly/EA), starting at 8 elements.
//   - Iterators are raw pointers (std-compatible, range-for + <algorithm>).
//   - swap_remove(i) is O(1) — fast for unordered collections.
//
// Move semantics: move ctor/assign transfer the buffer AND the allocator.
// Copy ctor takes an optional allocator argument; if null, copies use the
// RHS's allocator.
// -----------------------------------------------------------------------
template <typename T> class Array
{
public:
    using value_type = T;
    using size_type = usize;
    using iterator = T*;
    using const_iterator = const T*;
    using reference = T&;
    using const_reference = const T&;

    // ---- Ctors ----------------------------------------------------

    explicit Array(memory::IAllocator* alloc = memory::default_allocator()) noexcept : m_alloc(alloc) {}

    Array(usize initial_capacity, memory::IAllocator* alloc = memory::default_allocator()) : m_alloc(alloc)
    {
        if (initial_capacity > 0)
        {
            grow_to_at_least(initial_capacity);
        }
    }

    Array(std::initializer_list<T> list, memory::IAllocator* alloc = memory::default_allocator()) : m_alloc(alloc)
    {
        grow_to_at_least(list.size());
        for (const T& v : list)
        {
            push_back(v);
        }
    }

    // Copy: RHS's allocator unless an explicit one is provided.
    Array(const Array& other, memory::IAllocator* alloc = nullptr) : m_alloc(alloc ? alloc : other.m_alloc)
    {
        if (other.m_size > 0)
        {
            grow_to_at_least(other.m_size);
            for (usize i = 0; i < other.m_size; ++i)
            {
                ::new (static_cast<void*>(m_data + i)) T(other.m_data[i]);
            }
            m_size = other.m_size;
        }
    }

    // Move: take buffer + allocator. RHS becomes empty (allocator preserved).
    Array(Array&& other) noexcept
        : m_alloc(other.m_alloc), m_data(other.m_data), m_size(other.m_size), m_capacity(other.m_capacity)
    {
        other.check_mutable(); // moving a frozen array is a misuse
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
    }

    ~Array()
    {
        clear();
        free_buffer();
    }

    // ---- Assignment -----------------------------------------------

    Array& operator=(const Array& other)
    {
        if (this == &other)
        {
            return *this;
        }
        clear();
        if (other.m_size > m_capacity)
        {
            free_buffer();
            m_alloc = other.m_alloc;
            grow_to_at_least(other.m_size);
        }
        for (usize i = 0; i < other.m_size; ++i)
        {
            ::new (static_cast<void*>(m_data + i)) T(other.m_data[i]);
        }
        m_size = other.m_size;
        return *this;
    }

    Array& operator=(Array&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        other.check_mutable(); // moving a frozen array is a misuse (this side is covered by clear())
        clear();
        free_buffer();
        m_alloc = other.m_alloc;
        m_data = other.m_data;
        m_size = other.m_size;
        m_capacity = other.m_capacity;
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
        return *this;
    }

    // ---- Element access ------------------------------------------

    reference operator[](usize i) noexcept
    {
        CRD_ASSERT(i < m_size);
        return m_data[i];
    }
    const_reference operator[](usize i) const noexcept
    {
        CRD_ASSERT(i < m_size);
        return m_data[i];
    }

    reference front() noexcept
    {
        CRD_ASSERT(m_size > 0);
        return m_data[0];
    }
    const_reference front() const noexcept
    {
        CRD_ASSERT(m_size > 0);
        return m_data[0];
    }
    reference back() noexcept
    {
        CRD_ASSERT(m_size > 0);
        return m_data[m_size - 1];
    }
    const_reference back() const noexcept
    {
        CRD_ASSERT(m_size > 0);
        return m_data[m_size - 1];
    }

    T* data() noexcept { return m_data; }
    const T* data() const noexcept { return m_data; }

    // ---- Capacity --------------------------------------------------

    usize size() const noexcept { return m_size; }
    usize capacity() const noexcept { return m_capacity; }
    bool empty() const noexcept { return m_size == 0; }

    // Reserve at least `n` slots. OOM/exhaustion is fatal.
    void reserve(usize n)
    {
        check_mutable();
        if (n > m_capacity)
        {
            grow_to_at_least(n);
        }
    }

    // Best-effort reserve: returns false if the allocator refused.
    // Useful when backing this with a sub-budget allocator (LinearAllocator
    // out of room, PoolAllocator wrong slot size, etc).
    [[nodiscard]] bool try_reserve(usize n) noexcept
    {
        check_mutable();
        if (n <= m_capacity)
        {
            return true;
        }
        return try_grow_to_at_least(n);
    }

    void shrink_to_fit()
    {
        check_mutable();
        if (m_size == m_capacity)
        {
            return;
        }
        if (m_size == 0)
        {
            free_buffer();
            return;
        }
        T* new_data = static_cast<T*>(m_alloc->allocate(sizeof(T) * m_size, alignof(T)));
        relocate(new_data, m_data, m_size);
        free_buffer();
        m_data = new_data;
        m_capacity = m_size;
    }

    // ---- Modifiers -------------------------------------------------

    // Default-construct elements up to size n. Shrinks (calling dtors) if smaller.
    void resize(usize n)
    {
        check_mutable();
        if (n < m_size)
        {
            destroy_range(m_data + n, m_data + m_size);
        }
        else if (n > m_size)
        {
            reserve(n);
            for (usize i = m_size; i < n; ++i)
            {
                ::new (static_cast<void*>(m_data + i)) T();
            }
        }
        m_size = n;
    }

    // Resize with a fill value.
    void resize(usize n, const T& fill)
    {
        check_mutable();
        if (n < m_size)
        {
            destroy_range(m_data + n, m_data + m_size);
        }
        else if (n > m_size)
        {
            reserve(n);
            for (usize i = m_size; i < n; ++i)
            {
                ::new (static_cast<void*>(m_data + i)) T(fill);
            }
        }
        m_size = n;
    }

    // Resize WITHOUT initialising newly exposed elements when T is trivially
    // constructible (the new slots hold indeterminate values). For non-trivial
    // T this falls back to value-initialising resize() so it is always safe.
    // Use only when every new slot will be written before it is read -- e.g. a
    // scatter that fills all positions. Skips the zero-init pass that dominates
    // bulk-build hot paths (sparse assembly).
    void resize_uninitialized(usize n)
    {
        if constexpr (!std::is_trivially_constructible_v<T>)
        {
            resize(n);
            return;
        }
        else
        {
            check_mutable();
            if (n < m_size)
            {
                destroy_range(m_data + n, m_data + m_size);
            }
            else if (n > m_size)
            {
                reserve(n);
                // No construction: trivially-constructible slots are left as-is.
            }
            m_size = n;
        }
    }

    // Destroy all elements; capacity is unchanged.
    void clear() noexcept
    {
        check_mutable();
        destroy_range(m_data, m_data + m_size);
        m_size = 0;
    }

    // OOM/exhaustion is fatal (via underlying allocator).
    void push_back(const T& v)
    {
        check_mutable();
        if (m_size == m_capacity)
        {
            grow_to_at_least(m_size + 1);
        }
        ::new (static_cast<void*>(m_data + m_size)) T(v);
        ++m_size;
    }

    void push_back(T&& v)
    {
        check_mutable();
        if (m_size == m_capacity)
        {
            grow_to_at_least(m_size + 1);
        }
        ::new (static_cast<void*>(m_data + m_size)) T(std::move(v));
        ++m_size;
    }

    template <typename... Args> T& emplace_back(Args&&... args)
    {
        check_mutable();
        if (m_size == m_capacity)
        {
            grow_to_at_least(m_size + 1);
        }
        T* p = ::new (static_cast<void*>(m_data + m_size)) T(std::forward<Args>(args)...);
        ++m_size;
        return *p;
    }

    // Returns false if the allocator refused (sub-budget exhaustion).
    [[nodiscard]] bool try_push_back(const T& v) noexcept
    {
        check_mutable();
        if (m_size == m_capacity && !try_grow_to_at_least(m_size + 1))
        {
            return false;
        }
        ::new (static_cast<void*>(m_data + m_size)) T(v);
        ++m_size;
        return true;
    }

    [[nodiscard]] bool try_push_back(T&& v) noexcept
    {
        check_mutable();
        if (m_size == m_capacity && !try_grow_to_at_least(m_size + 1))
        {
            return false;
        }
        ::new (static_cast<void*>(m_data + m_size)) T(std::move(v));
        ++m_size;
        return true;
    }

    void pop_back() noexcept
    {
        check_mutable();
        CRD_ASSERT(m_size > 0);
        --m_size;
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            m_data[m_size].~T();
        }
    }

    // Remove element at index i, shifting everything after it left by one.
    // O(n) — preserves order.
    void erase(usize i)
    {
        check_mutable();
        CRD_ASSERT(i < m_size);
        for (usize k = i; k + 1 < m_size; ++k)
        {
            m_data[k] = std::move(m_data[k + 1]);
        }
        --m_size;
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            m_data[m_size].~T();
        }
    }

    // Remove element at index i by overwriting it with the back element.
    // O(1) — does NOT preserve order. Perfect for unordered collections
    // (entity lists, draw call buckets, free lists).
    void swap_remove(usize i) noexcept
    {
        check_mutable();
        CRD_ASSERT(i < m_size);
        const usize last = m_size - 1;
        if (i != last)
        {
            m_data[i] = std::move(m_data[last]);
        }
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            m_data[last].~T();
        }
        --m_size;
    }

    // Insert at position i, shifting everything from i..size right.
    // O(n) — call push_back if you can.
    void insert(usize i, const T& v)
    {
        check_mutable();
        CRD_ASSERT(i <= m_size);
        if (m_size == m_capacity)
        {
            grow_to_at_least(m_size + 1);
        }
        // Move-construct the new tail element from the old tail, then
        // shift everything else.
        if (i == m_size)
        {
            ::new (static_cast<void*>(m_data + m_size)) T(v);
        }
        else
        {
            ::new (static_cast<void*>(m_data + m_size)) T(std::move(m_data[m_size - 1]));
            for (usize k = m_size - 1; k > i; --k)
            {
                m_data[k] = std::move(m_data[k - 1]);
            }
            m_data[i] = v;
        }
        ++m_size;
    }

    // ---- Iterators -------------------------------------------------

    iterator begin() noexcept { return m_data; }
    iterator end() noexcept { return m_data + m_size; }
    const_iterator begin() const noexcept { return m_data; }
    const_iterator end() const noexcept { return m_data + m_size; }
    const_iterator cbegin() const noexcept { return m_data; }
    const_iterator cend() const noexcept { return m_data + m_size; }

    // ---- Misc ------------------------------------------------------

    memory::IAllocator* allocator() const noexcept { return m_alloc; }

    // ---- Debug freeze guard (detour D-002 v2) ----------------------
    //
    // freeze() / unfreeze() bracket a scope during which this Array must NOT
    // be structurally mutated (push/pop/erase/insert/clear/resize/reserve/
    // shrink/assign/move-from). Element access — operator[], data(), begin/end,
    // mutating individual elements in place — stays allowed: the canonical use
    // is "many fibers writing disjoint elements of a frozen Array during a
    // parallel for_each / par_each pass".
    //
    // A structural mutation while frozen trips a CRD_ASSERT at the point of
    // misuse. Like every CRD_ASSERT this is a *development net*, not a hard
    // barrier — in a debug/ASan run it fires loudly (and breaks under a
    // debugger); in a Release build the check (and the mutation) proceed. So
    // freeze() catches "a structural change leaked into a parallel pass" during
    // testing; it does not make the operation safe at runtime.
    //
    // Debug-only: in non-assert builds (Release/Shipping) these are no-ops and
    // the depth counter does not exist — zero size, zero ABI cost. freeze() is
    // const so a `const Array&` handed to read-only workers can be frozen too.
    // Re-entrant: nested freeze() calls are counted; the array is mutable again
    // only after the matching unfreeze(). Prefer FrozenView (below) for RAII.
    void freeze() const noexcept
    {
#if CRD_ENABLE_ASSERTS
        ++m_freeze_depth;
#endif
    }
    void unfreeze() const noexcept
    {
#if CRD_ENABLE_ASSERTS
        CRD_ASSERT_MSG(m_freeze_depth > 0, "Array::unfreeze() with no matching freeze()");
        --m_freeze_depth;
#endif
    }
    [[nodiscard]] bool is_frozen() const noexcept
    {
#if CRD_ENABLE_ASSERTS
        return m_freeze_depth != 0;
#else
        return false;
#endif
    }

private:
    // Asserts that no freeze() scope is active. Called at the top of every
    // structural mutator. No-op (and elided) in non-assert builds.
    void check_mutable() const noexcept
    {
#if CRD_ENABLE_ASSERTS
        CRD_ASSERT_MSG(m_freeze_depth == 0,
                       "Array structurally mutated while frozen — a push/pop/erase/insert/clear/resize/"
                       "reserve/shrink/assign/move-from happened inside a freeze() scope (e.g. during a "
                       "parallel for_each / par_each pass)");
#endif
    }

    // 1.5x growth strategy with a minimum initial capacity.
    // Returns the new capacity that would result.
    static constexpr usize kInitialCapacity = 8;

    usize next_capacity(usize required) const noexcept
    {
        if (m_capacity == 0)
        {
            return required > kInitialCapacity ? required : kInitialCapacity;
        }
        const usize grown = m_capacity + (m_capacity >> 1); // 1.5x
        return required > grown ? required : grown;
    }

    // Grow the buffer; OOM is fatal (allocator decides).
    void grow_to_at_least(usize required)
    {
        const usize new_cap = next_capacity(required);
        T* new_data = static_cast<T*>(m_alloc->allocate(sizeof(T) * new_cap, alignof(T)));
        relocate(new_data, m_data, m_size);
        free_buffer();
        m_data = new_data;
        m_capacity = new_cap;
    }

    // Best-effort grow. Returns false if the allocator returned nullptr.
    // (MallocAllocator never returns nullptr — it either succeeds or
    // CRD_FATAL's. Sub-budget allocators DO return nullptr on exhaustion.)
    [[nodiscard]] bool try_grow_to_at_least(usize required) noexcept
    {
        const usize new_cap = next_capacity(required);
        // We need a non-throwing alloc. Our IAllocator::allocate is noexcept-safe
        // for sub-budgets (returns nullptr) but heap allocators FATAL on OOM.
        // For try_*, the caller has explicitly opted into "no fatal", so this
        // is the right shape.
        T* new_data = static_cast<T*>(m_alloc->allocate(sizeof(T) * new_cap, alignof(T)));
        if (!new_data)
        {
            return false;
        }
        relocate(new_data, m_data, m_size);
        free_buffer();
        m_data = new_data;
        m_capacity = new_cap;
        return true;
    }

    // Move-construct or memcpy `count` elements from `src` into the
    // freshly-allocated `dst`, then destroy the originals.
    void relocate(T* dst, T* src, usize count) noexcept
    {
        if constexpr (std::is_trivially_copyable_v<T>)
        {
            if (count > 0)
            {
                std::memcpy(dst, src, sizeof(T) * count);
            }
        }
        else
        {
            for (usize i = 0; i < count; ++i)
            {
                ::new (static_cast<void*>(dst + i)) T(std::move(src[i]));
                src[i].~T();
            }
        }
    }

    void destroy_range(T* first, T* last) noexcept
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            for (T* p = first; p != last; ++p)
            {
                p->~T();
            }
        }
        else
        {
            (void)first;
            (void)last;
        }
    }

    void free_buffer() noexcept
    {
        if (m_data)
        {
            m_alloc->deallocate(m_data);
            m_data = nullptr;
            m_capacity = 0;
        }
    }

    memory::IAllocator* m_alloc = nullptr;
    T* m_data = nullptr;
    usize m_size = 0;
    usize m_capacity = 0;
#if CRD_ENABLE_ASSERTS
    // Active freeze() scope count. Debug-only — absent in Release/Shipping.
    // Not transferred on move: the moved-FROM array is being emptied (and
    // moving a frozen array is itself a misuse caught by check_mutable); the
    // moved-TO array starts unfrozen.
    mutable int m_freeze_depth = 0;
#endif
};

// -----------------------------------------------------------------------
// FrozenView<T> — RAII handle that freeze()s an Array<T> on construction and
// unfreeze()s it on destruction (detour D-002 v2). Move-only. Hand it (by
// reference) to a parallel for_each / par_each so the structural-mutation
// guard is scoped to exactly the parallel region:
//
//     {
//         crd::containers::FrozenView fv(my_array);          // frozen here
//         crd::jobs::run_and_wait(parallel_for(fv.size(), k,
//             [&fv](u32 b, u32 e){ for (u32 i = b; i < e; ++i) fv[i] = f(i); }));
//     }                                                       // unfrozen here
//
// Element access through the view reads AND writes individual elements (the
// disjoint-write use case); you cannot resize through it. In non-assert builds
// freeze()/unfreeze() are no-ops, so FrozenView compiles to a thin pointer.
// -----------------------------------------------------------------------
template <typename T> class FrozenView
{
public:
    explicit FrozenView(Array<T>& a) noexcept : m_arr(&a) { m_arr->freeze(); }
    ~FrozenView()
    {
        if (m_arr != nullptr)
        {
            m_arr->unfreeze();
        }
    }

    FrozenView(const FrozenView&) = delete;
    FrozenView& operator=(const FrozenView&) = delete;
    FrozenView(FrozenView&& o) noexcept : m_arr(o.m_arr) { o.m_arr = nullptr; }
    FrozenView& operator=(FrozenView&&) = delete;

    [[nodiscard]] usize size() const noexcept { return m_arr->size(); }
    [[nodiscard]] bool empty() const noexcept { return m_arr->empty(); }
    [[nodiscard]] T* data() const noexcept { return m_arr->data(); }
    [[nodiscard]] T& operator[](usize i) const noexcept { return (*m_arr)[i]; }
    [[nodiscard]] T* begin() const noexcept { return m_arr->begin(); }
    [[nodiscard]] T* end() const noexcept { return m_arr->end(); }
    [[nodiscard]] Array<T>& array() const noexcept { return *m_arr; }

private:
    Array<T>* m_arr;
};

// ---- Free-function helpers ------------------------------------------

template <typename T> bool operator==(const Array<T>& a, const Array<T>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i)
    {
        if (!(a[i] == b[i]))
        {
            return false;
        }
    }
    return true;
}

template <typename T> bool operator!=(const Array<T>& a, const Array<T>& b)
{
    return !(a == b);
}
} // namespace crd::containers
