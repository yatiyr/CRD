// crd::containers — deterministic sort + heap algorithms.
// Phase 3.1 v0d. ADR-0063 §3.
//
// Ships replacements for std::sort / std::stable_sort / std::nth_element /
// std::push_heap / std::pop_heap / std::make_heap that produce bit-exact
// identical output across MSVC / GCC / clang × x64 / ARM64. Banned-std lint
// (`crd-no-std-sort-check`) keeps `engine/eylem/**` + `engine/hesap/**`
// from regressing to the libc++ / libstdc++ versions whose tie-breaking
// orders differ across implementations.
//
// Algorithm choices:
//   sort         → merge sort (naturally stable; no pivot-dependent ordering;
//                  same algorithm as stable_sort — bit-exact equivalence)
//   stable_sort  → merge sort
//   nth_element  → quickselect with median-of-three pivot (deterministic
//                  pivot selection; fall-through to insertion sort for tiny
//                  partitions)
//   make_heap    → standard floyd's bottom-up sift-down build
//   push_heap    → sift-up from end-1
//   pop_heap     → swap *begin with *(end-1), then sift-down [begin, end-1)
//   sort_heap    → repeated pop_heap
//
// All algorithms take a comparator (defaults to std::less<>). The comparator
// MUST itself be deterministic — that's a contract the caller upholds.
//
// Memory: stable_sort allocates an auxiliary buffer of size N*sizeof(T)
// (default-constructed). Requires T to be default-constructible. Other ops
// are in-place.

#pragma once

#include <crd/core/types.hpp>
#include <crd/containers/array.hpp>

#include <functional>     // std::less default comparator
#include <iterator>       // std::iterator_traits
#include <utility>        // std::move, std::swap

namespace crd::containers
{
namespace detail
{
// Insertion sort — used as the small-partition base case for both merge
// sort (bottom-up) and quickselect.
template <typename It, typename Cmp>
void insertion_sort(It first, It last, Cmp cmp)
{
    if (first == last) return;
    for (It i = first + 1; i != last; ++i)
    {
        auto val = std::move(*i);
        It j = i;
        while (j != first)
        {
            It prev = j - 1;
            // Stable: only move prev if STRICTLY greater than val.
            if (cmp(val, *prev))
            {
                *j = std::move(*prev);
                j = prev;
            }
            else break;
        }
        *j = std::move(val);
    }
}

// Stable merge of two sorted sub-ranges into out.
// Tie-breaker: when neither cmp(*a, *b) nor cmp(*b, *a) is true, take from
// the LEFT side first — preserves input order for equal-key elements.
template <typename It, typename T, typename Cmp>
void stable_merge(It a_first, It a_last, It b_first, It b_last, T* out, Cmp cmp)
{
    while (a_first != a_last && b_first != b_last)
    {
        if (cmp(*b_first, *a_first))
        {
            *out++ = std::move(*b_first++);
        }
        else
        {
            *out++ = std::move(*a_first++);
        }
    }
    while (a_first != a_last) *out++ = std::move(*a_first++);
    while (b_first != b_last) *out++ = std::move(*b_first++);
}

// Recursive merge sort. Caller pre-allocates `temp` of size (last - first).
template <typename It, typename T, typename Cmp>
void merge_sort_recursive(It first, It last, T* temp, Cmp cmp)
{
    using Diff = decltype(last - first);
    const Diff n = last - first;

    // Insertion sort base case for small partitions.
    if (n < 32)
    {
        insertion_sort(first, last, cmp);
        return;
    }

    const Diff half = n / 2;
    It mid = first + half;
    merge_sort_recursive(first, mid, temp,        cmp);
    merge_sort_recursive(mid,   last, temp + half, cmp);

    // Merge [first, mid) and [mid, last) into temp, then move back.
    stable_merge(first, mid, mid, last, temp, cmp);
    for (Diff i = 0; i < n; ++i)
    {
        first[i] = std::move(temp[i]);
    }
}

// Median of three. Returns iterator to the median value.
template <typename It, typename Cmp>
It median_of_three(It a, It b, It c, Cmp cmp) noexcept
{
    if (cmp(*a, *b))
    {
        if (cmp(*b, *c)) return b;
        if (cmp(*a, *c)) return c;
        return a;
    }
    if (cmp(*a, *c)) return a;
    if (cmp(*b, *c)) return c;
    return b;
}

// Lomuto partition. Returns iterator to the pivot's final position.
// Pivot selected via median-of-three — deterministic.
template <typename It, typename Cmp>
It partition_lomuto(It first, It last, Cmp cmp)
{
    const auto n = last - first;
    It mid = first + n / 2;
    It pivot_iter = median_of_three(first, mid, last - 1, cmp);

    // Move pivot to the end.
    auto pivot_val = std::move(*pivot_iter);
    *pivot_iter = std::move(*(last - 1));

    It store = first;
    for (It i = first; i != last - 1; ++i)
    {
        if (cmp(*i, pivot_val))
        {
            std::swap(*i, *store);
            ++store;
        }
    }
    *(last - 1) = std::move(*store);
    *store = std::move(pivot_val);
    return store;
}

// Heap helpers — operate on [first, last) viewed as a binary max-heap rooted
// at *first. cmp(a, b) is the underlying "less-than", so the heap puts the
// element for which `cmp(other, this)` is true above; this is the std-heap
// convention (top = max under cmp).

template <typename It, typename Cmp>
void heap_sift_up(It first, decltype(std::declval<It>() - std::declval<It>()) idx, Cmp cmp)
{
    auto val = std::move(first[idx]);
    while (idx > 0)
    {
        const auto parent = (idx - 1) / 2;
        if (cmp(first[parent], val))
        {
            first[idx] = std::move(first[parent]);
            idx = parent;
        }
        else break;
    }
    first[idx] = std::move(val);
}

template <typename It, typename Diff, typename Cmp>
void heap_sift_down(It first, Diff idx, Diff size, Cmp cmp)
{
    auto val = std::move(first[idx]);
    while (true)
    {
        const Diff left  = 2 * idx + 1;
        if (left >= size) break;
        const Diff right = left + 1;
        Diff largest = left;
        if (right < size && cmp(first[left], first[right])) largest = right;
        if (!cmp(val, first[largest])) break;
        first[idx] = std::move(first[largest]);
        idx = largest;
    }
    first[idx] = std::move(val);
}
}  // namespace detail

// ===========================================================================
// Public API
// ===========================================================================

// Stable, deterministic sort. Merge-sort-based; allocates O(N) auxiliary
// memory. Requires T (the value type) to be default-constructible.
template <typename It, typename Cmp = std::less<>>
void stable_sort(It first, It last, Cmp cmp = Cmp{})
{
    using Diff  = decltype(last - first);
    using T     = typename std::iterator_traits<It>::value_type;
    const Diff n = last - first;
    if (n < 2) return;

    Array<T> temp;
    temp.resize(static_cast<usize>(n));
    detail::merge_sort_recursive(first, last, temp.data(), cmp);
}

// Same algorithm as stable_sort — guarantees the same result. Kept as a
// separate overload so callers using `crd::containers::sort` get a
// recognisable name + future ABI flexibility (a faster non-stable variant
// could replace this without touching `stable_sort`).
template <typename It, typename Cmp = std::less<>>
void sort(It first, It last, Cmp cmp = Cmp{})
{
    crd::containers::stable_sort(first, last, cmp);
}

// Partial sort: places the element that would be at position `nth` after
// a full sort, into *nth. Elements before nth are ≤ *nth (under cmp);
// elements after nth are ≥ *nth. Their relative order is unspecified
// but deterministic across runs (median-of-three pivot selection).
template <typename It, typename Cmp = std::less<>>
void nth_element(It first, It nth, It last, Cmp cmp = Cmp{})
{
    if (nth == last) return;
    while (last - first > 16)
    {
        It p = detail::partition_lomuto(first, last, cmp);
        if (p == nth)        return;
        else if (p < nth)    first = p + 1;
        else                 last  = p;
    }
    detail::insertion_sort(first, last, cmp);
}

// Heap operations — array view, 0-indexed binary heap.

template <typename It, typename Cmp = std::less<>>
void push_heap(It first, It last, Cmp cmp = Cmp{})
{
    using Diff = decltype(last - first);
    const Diff n = last - first;
    if (n < 2) return;
    detail::heap_sift_up(first, n - 1, cmp);
}

template <typename It, typename Cmp = std::less<>>
void pop_heap(It first, It last, Cmp cmp = Cmp{})
{
    using Diff = decltype(last - first);
    const Diff n = last - first;
    if (n < 2) return;
    std::swap(*first, *(last - 1));
    detail::heap_sift_down(first, Diff{0}, n - 1, cmp);
}

template <typename It, typename Cmp = std::less<>>
void make_heap(It first, It last, Cmp cmp = Cmp{})
{
    using Diff = decltype(last - first);
    const Diff n = last - first;
    if (n < 2) return;
    // Floyd's bottom-up heapify: O(n).
    for (Diff i = (n / 2) - 1; i >= 0; --i)
    {
        detail::heap_sift_down(first, i, n, cmp);
    }
}

template <typename It, typename Cmp = std::less<>>
void sort_heap(It first, It last, Cmp cmp = Cmp{})
{
    while (last - first > 1)
    {
        crd::containers::pop_heap(first, last, cmp);
        --last;
    }
}

}  // namespace crd::containers
