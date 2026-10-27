// The vector steps of the hesap kernel families (SIMD-DISPATCH, ADR-0134 §7): the level's vector of each type, its
// multiply-add (two roundings below avx2, one fused rounding from avx2) and the walks over whole and partial vectors
// that give every element the same step. The kernel units of hesap-dense and hesap-direct include it; its inline code
// lives in the level namespace and calls no standard-library inline function.

#pragma once

#include <crd/core/types.hpp>
#include <crd/math/kernel_level.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

namespace crd::hesap::dense::detail::inline CRD_MATH_LEVEL_NS
{

namespace simd = crd::math::simd;

template <typename T> struct VectorOf
{
    using Type = simd::Vec4d;
    static constexpr crd::usize kWidth = 4;
};
template <> struct VectorOf<crd::f32>
{
    using Type = simd::Vec8f;
    static constexpr crd::usize kWidth = 8;
};

template <typename V> CRD_FORCEINLINE V madd(V a, V b, V c) noexcept
{
#if CRD_SIMD_HAS_AVX2
    return simd::fma(a, b, c);
#else
    return simd::mul_add(a, b, c);
#endif
}

template <crd::usize Q> struct Index
{
    static constexpr crd::usize kValue = Q;
};

// f(Index<Q>{}) for Q = 0 .. N - 1: every register array index is a constant, so the arrays stay in registers.
template <crd::usize N, crd::usize Q = 0, typename F> CRD_FORCEINLINE void unroll(F& f) noexcept
{
    if constexpr (Q < N)
    {
        f(Index<Q>{});
        unroll<N, Q + 1>(f);
    }
}

// The first `count` values (1 to w) of p as a vector, and the store of its first `count` lanes.
template <typename V, typename T> CRD_FORCEINLINE V load_n(const T* p, crd::usize count) noexcept
{
    return count == VectorOf<T>::kWidth ? V::load(p) : V::load_partial(p, count);
}

template <typename V, typename T> CRD_FORCEINLINE void store_n(V v, T* p, crd::usize count) noexcept
{
    if (count == VectorOf<T>::kWidth)
    {
        v.store(p);
    }
    else
    {
        v.store_partial(p, count);
    }
}

// f(k, count) for each whole vector of n elements (count w) and the partial one that ends them.
template <typename T, typename F> CRD_FORCEINLINE void for_each_vector(crd::usize n, F f) noexcept
{
    constexpr crd::usize w = VectorOf<T>::kWidth;
    crd::usize k = 0;
    for (; k + w <= n; k += w)
    {
        f(k, w);
    }
    if (k < n)
    {
        f(k, n - k);
    }
}

// y + alpha x as the level's step.
template <typename T> CRD_FORCEINLINE void axpy(T* y, const T* x, T alpha, crd::usize n) noexcept
{
    using V = typename VectorOf<T>::Type;
    // The lambdas capture scalars: a captured vector's alignment pads the closure (C4324).
    for_each_vector<T>(n, [y, x, alpha](crd::usize k, crd::usize count)
                       { store_n(madd(V(alpha), load_n<V>(x + k, count), load_n<V>(y + k, count)), y + k, count); });
}

} // namespace crd::hesap::dense::detail::inline CRD_MATH_LEVEL_NS
