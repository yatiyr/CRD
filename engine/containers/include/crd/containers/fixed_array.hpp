#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <cstring>
#include <initializer_list>
#include <new>
#include <type_traits>
#include <utility>

namespace crd::containers
{
// -----------------------------------------------------------------------
// FixedArray<T, N> — stack-only, sabit kapasite, dinamik size.
//
// Use cases:
//  - "At most 8 vertex stream slots, at most 16 texture bindings, ..."
//    compile-time known upper bounds.
//  - Hot-path scratch where you want zero allocation.
//
// No allocator. The storage is an aligned byte array embedded in the
// FixedArray itself. push_back past N is asserted; try_push_back returns
// false instead.
// -----------------------------------------------------------------------
template <typename T, usize N> class FixedArray
{
    static_assert(N > 0, "FixedArray<T, 0> is not allowed");

public:
    using value_type = T;
    using size_type = usize;
    using iterator = T*;
    using const_iterator = const T*;
    using reference = T&;
    using const_reference = const T&;

    FixedArray() noexcept = default;

    FixedArray(std::initializer_list<T> list)
    {
        CRD_ASSERT(list.size() <= N);
        for (const T& v : list)
        {
            push_back(v);
        }
    }

    // Copy & move.
    FixedArray(const FixedArray& other)
    {
        for (usize i = 0; i < other.m_size; ++i)
        {
            ::new (slot(i)) T(other[i]);
        }
        m_size = other.m_size;
    }

    FixedArray(FixedArray&& other) noexcept
    {
        for (usize i = 0; i < other.m_size; ++i)
        {
            ::new (slot(i)) T(std::move(other[i]));
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                other[i].~T();
            }
        }
        m_size = other.m_size;
        other.m_size = 0;
    }

    FixedArray& operator=(const FixedArray& other)
    {
        if (this == &other)
        {
            return *this;
        }
        clear();
        for (usize i = 0; i < other.m_size; ++i)
        {
            ::new (slot(i)) T(other[i]);
        }
        m_size = other.m_size;
        return *this;
    }

    FixedArray& operator=(FixedArray&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        clear();
        for (usize i = 0; i < other.m_size; ++i)
        {
            ::new (slot(i)) T(std::move(other[i]));
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                other[i].~T();
            }
        }
        m_size = other.m_size;
        other.m_size = 0;
        return *this;
    }

    ~FixedArray() { clear(); }

    // ---- Element access ------------------------------------------

    reference operator[](usize i) noexcept
    {
        CRD_ASSERT(i < m_size);
        return data()[i];
    }
    const_reference operator[](usize i) const noexcept
    {
        CRD_ASSERT(i < m_size);
        return data()[i];
    }

    reference front() noexcept
    {
        CRD_ASSERT(m_size > 0);
        return data()[0];
    }
    const_reference front() const noexcept
    {
        CRD_ASSERT(m_size > 0);
        return data()[0];
    }
    reference back() noexcept
    {
        CRD_ASSERT(m_size > 0);
        return data()[m_size - 1];
    }
    const_reference back() const noexcept
    {
        CRD_ASSERT(m_size > 0);
        return data()[m_size - 1];
    }

    T* data() noexcept { return reinterpret_cast<T*>(&m_storage[0]); }
    const T* data() const noexcept { return reinterpret_cast<const T*>(&m_storage[0]); }

    // ---- Capacity ------------------------------------------------

    usize size() const noexcept { return m_size; }
    static constexpr usize capacity() noexcept { return N; }
    bool empty() const noexcept { return m_size == 0; }
    bool full() const noexcept { return m_size == N; }

    // ---- Modifiers -----------------------------------------------

    void clear() noexcept
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            for (usize i = 0; i < m_size; ++i)
            {
                data()[i].~T();
            }
        }
        m_size = 0;
    }

    // Asserts on full.
    void push_back(const T& v)
    {
        CRD_ASSERT(m_size < N);
        ::new (slot(m_size)) T(v);
        ++m_size;
    }

    void push_back(T&& v)
    {
        CRD_ASSERT(m_size < N);
        ::new (slot(m_size)) T(std::move(v));
        ++m_size;
    }

    template <typename... Args> T& emplace_back(Args&&... args)
    {
        CRD_ASSERT(m_size < N);
        T* p = ::new (slot(m_size)) T(std::forward<Args>(args)...);
        ++m_size;
        return *p;
    }

    // Returns false if full.
    [[nodiscard]] bool try_push_back(const T& v) noexcept
    {
        if (m_size == N)
        {
            return false;
        }
        ::new (slot(m_size)) T(v);
        ++m_size;
        return true;
    }

    [[nodiscard]] bool try_push_back(T&& v) noexcept
    {
        if (m_size == N)
        {
            return false;
        }
        ::new (slot(m_size)) T(std::move(v));
        ++m_size;
        return true;
    }

    void pop_back() noexcept
    {
        CRD_ASSERT(m_size > 0);
        --m_size;
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            data()[m_size].~T();
        }
    }

    void swap_remove(usize i) noexcept
    {
        CRD_ASSERT(i < m_size);
        const usize last = m_size - 1;
        if (i != last)
        {
            data()[i] = std::move(data()[last]);
        }
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            data()[last].~T();
        }
        --m_size;
    }

    // ---- Iterators -----------------------------------------------

    iterator begin() noexcept { return data(); }
    iterator end() noexcept { return data() + m_size; }
    const_iterator begin() const noexcept { return data(); }
    const_iterator end() const noexcept { return data() + m_size; }
    const_iterator cbegin() const noexcept { return data(); }
    const_iterator cend() const noexcept { return data() + m_size; }

private:
    void* slot(usize i) noexcept { return static_cast<void*>(&m_storage[i * sizeof(T)]); }

    // Aligned uninitialised storage for N elements.
    alignas(T) std::byte m_storage[sizeof(T) * N]{};
    usize m_size = 0;
};
} // namespace crd::containers
