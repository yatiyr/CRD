#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <type_traits>

namespace crd::containers
{
// -----------------------------------------------------------------------
// StaticArray<T, N> — fixed-SIZE, always-full, stack-only array. The Cerid
// analogue of std::array: exactly N elements, no dynamic size, no allocator,
// aggregate brace-init, literal type when T is. Use this (not std::array, not
// FixedArray) when the element count is a compile-time constant and the array
// is always full — e.g. a frustum's 6 planes, a 3x3 axis triple, a fixed LUT.
//
// FixedArray<T, N> is the other one — fixed *capacity*, dynamic *size* (push
// up to N). Pick StaticArray when "size == N always", FixedArray when "size in
// [0, N]".
//
// Aggregate on purpose: `StaticArray<Plane, 6>{p0, p1, p2, p3, p4, p5}` works
// via brace elision (one member, an array of N), and a default-init instance
// value-initializes all N elements (zero for scalars / value-init for class T).
// -----------------------------------------------------------------------
template <typename T, usize N> struct StaticArray
{
    static_assert(N > 0, "StaticArray<T, 0> is not allowed");

    using value_type = T;
    using size_type = usize;
    using iterator = T*;
    using const_iterator = const T*;
    using reference = T&;
    using const_reference = const T&;

    T elems[N];

    [[nodiscard]] constexpr reference operator[](usize i) noexcept
    {
        CRD_ASSERT(i < N);
        return elems[i];
    }
    [[nodiscard]] constexpr const_reference operator[](usize i) const noexcept
    {
        CRD_ASSERT(i < N);
        return elems[i];
    }

    [[nodiscard]] constexpr reference front() noexcept { return elems[0]; }
    [[nodiscard]] constexpr const_reference front() const noexcept { return elems[0]; }
    [[nodiscard]] constexpr reference back() noexcept { return elems[N - 1]; }
    [[nodiscard]] constexpr const_reference back() const noexcept { return elems[N - 1]; }

    [[nodiscard]] constexpr T* data() noexcept { return elems; }
    [[nodiscard]] constexpr const T* data() const noexcept { return elems; }

    [[nodiscard]] constexpr iterator begin() noexcept { return elems; }
    [[nodiscard]] constexpr iterator end() noexcept { return elems + N; }
    [[nodiscard]] constexpr const_iterator begin() const noexcept { return elems; }
    [[nodiscard]] constexpr const_iterator end() const noexcept { return elems + N; }
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return elems; }
    [[nodiscard]] constexpr const_iterator cend() const noexcept { return elems + N; }

    [[nodiscard]] constexpr usize size() const noexcept { return N; }
    [[nodiscard]] static constexpr usize capacity() noexcept { return N; }
    [[nodiscard]] constexpr bool empty() const noexcept { return false; }

    constexpr void fill(const T& value)
    {
        for (usize i = 0; i < N; ++i)
        {
            elems[i] = value;
        }
    }
};

template <typename T, usize N>
[[nodiscard]] constexpr bool operator==(const StaticArray<T, N>& lhs, const StaticArray<T, N>& rhs)
{
    for (usize i = 0; i < N; ++i)
    {
        if (!(lhs.elems[i] == rhs.elems[i]))
        {
            return false;
        }
    }
    return true;
}

template <typename T, usize N>
[[nodiscard]] constexpr bool operator!=(const StaticArray<T, N>& lhs, const StaticArray<T, N>& rhs)
{
    return !(lhs == rhs);
}

} // namespace crd::containers
