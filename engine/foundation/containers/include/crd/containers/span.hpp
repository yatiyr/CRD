#pragma once

#include <crd/core/types.hpp>

#include <span>
#include <type_traits>

namespace crd::containers
{
// Span is a non-owning view over a contiguous range of T. We just alias
// std::span -- it's exactly the right shape, well-tested, and any future
// toolchain bug fixes land for free. The wrapper here is for `crd::`
// namespace consistency, not because we need anything else.
template <typename T, std::size_t Extent = std::dynamic_extent> using Span = std::span<T, Extent>;

template <typename T> using ConstSpan = std::span<const T>;

// -----------------------------------------------------------------------
// Helpers: build a Span<T> from a raw pointer + size, a C-style array,
// or any container with .data() / .size() (e.g. our Array<T> below).
// -----------------------------------------------------------------------

template <typename T> constexpr Span<T> make_span(T* p, usize n) noexcept
{
    return Span<T>(p, n);
}

template <typename T, std::size_t N> constexpr Span<T, N> make_span(T (&arr)[N]) noexcept
{
    return Span<T, N>(arr, N);
}

// SFINAE-friendly factory for any "container-like" type (.data() + .size()).
template <typename Container>
constexpr auto as_span(Container& c) noexcept -> Span<std::remove_pointer_t<decltype(c.data())>>
{
    return Span<std::remove_pointer_t<decltype(c.data())>>(c.data(), c.size());
}

template <typename Container>
constexpr auto as_const_span(const Container& c) noexcept -> ConstSpan<std::remove_pointer_t<decltype(c.data())>>
{
    return ConstSpan<std::remove_pointer_t<decltype(c.data())>>(c.data(), c.size());
}
} // namespace crd::containers
