#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::preconditioners::detail
{
// -----------------------------------------------------------------------
// Factor-once / solve-many small dense LU with partial pivoting. Phase 3.1.6 v4i-3.
//
// Used by the Schwarz domain-decomposition preconditioner: each subdomain block A_ii is
// factored ONCE in setup (dense_lu_factor) and the forward/back solve runs every apply
// (dense_lu_solve_factored) -- unlike block_lu_solve which re-factors each call. Row-major
// m×m, in-place L\U (unit-lower L below the diagonal, U on/above), `piv[c]` = LAPACK-style
// row swapped to position c. Complex-capable (general LU, no conjugation). A pivot-floor
// (√ε·max|A|) keeps a collapsed block from blowing up (graceful, like ILUT/ILU(p)).
// -----------------------------------------------------------------------

template <typename T>
[[nodiscard]] inline crd::hesap::dense::RealType<T> dense_lu_mag(T v) noexcept
{
    using R = crd::hesap::dense::RealType<T>;
    if constexpr (crd::hesap::dense::is_complex_v<T>) { return std::sqrt(v.re * v.re + v.im * v.im); }
    else { return v < R(0) ? -v : v; }
}

// Factor m×m row-major `a` in place; `piv` (length m) receives the pivot swaps.
template <typename T>
inline void dense_lu_factor(T* a, crd::u32 m, crd::u32* piv) noexcept
{
    using R   = crd::hesap::dense::RealType<T>;
    R     dmax = R(0);
    for (crd::usize i = 0; i < static_cast<crd::usize>(m) * m; ++i) { const R g = dense_lu_mag(a[i]); dmax = g > dmax ? g : dmax; }
    const R eps = dmax * std::sqrt(std::numeric_limits<R>::epsilon()) + std::numeric_limits<R>::min();

    for (crd::u32 c = 0; c < m; ++c)
    {
        crd::u32 p    = c;
        R        best = dense_lu_mag(a[static_cast<crd::usize>(c) * m + c]);
        for (crd::u32 r = c + 1; r < m; ++r)
        {
            const R g = dense_lu_mag(a[static_cast<crd::usize>(r) * m + c]);
            if (g > best) { best = g; p = r; }
        }
        piv[c] = p;
        if (p != c)
        {
            for (crd::u32 j = 0; j < m; ++j)
            {
                const T t                              = a[static_cast<crd::usize>(p) * m + j];
                a[static_cast<crd::usize>(p) * m + j]   = a[static_cast<crd::usize>(c) * m + j];
                a[static_cast<crd::usize>(c) * m + j]   = t;
            }
        }
        T pivot = a[static_cast<crd::usize>(c) * m + c];
        if (dense_lu_mag(pivot) < eps) { pivot = T(eps); a[static_cast<crd::usize>(c) * m + c] = pivot; }
        const T inv = T(1) / pivot;
        for (crd::u32 r = c + 1; r < m; ++r)
        {
            const T f = a[static_cast<crd::usize>(r) * m + c] * inv;
            a[static_cast<crd::usize>(r) * m + c] = f; // store L multiplier
            if (dense_lu_mag(f) == R(0)) { continue; }
            for (crd::u32 j = c + 1; j < m; ++j)
            {
                a[static_cast<crd::usize>(r) * m + j] =
                    a[static_cast<crd::usize>(r) * m + j] - f * a[static_cast<crd::usize>(c) * m + j];
            }
        }
    }
}

// Solve A·x = b in place (b → x) using the factored `lu` + `piv` from dense_lu_factor.
template <typename T>
inline void dense_lu_solve_factored(const T* lu, const crd::u32* piv, crd::u32 m, T* b) noexcept
{
    for (crd::u32 c = 0; c < m; ++c) // apply row swaps (forward order)
    {
        if (piv[c] != c) { const T t = b[piv[c]]; b[piv[c]] = b[c]; b[c] = t; }
    }
    for (crd::u32 i = 0; i < m; ++i) // forward: unit-lower L
    {
        T s = b[i];
        for (crd::u32 j = 0; j < i; ++j) { s = s - lu[static_cast<crd::usize>(i) * m + j] * b[j]; }
        b[i] = s;
    }
    for (crd::u32 ii = 0; ii < m; ++ii) // back: upper U
    {
        const crd::u32 i = m - 1 - ii;
        T              s = b[i];
        for (crd::u32 j = i + 1; j < m; ++j) { s = s - lu[static_cast<crd::usize>(i) * m + j] * b[j]; }
        b[i] = s / lu[static_cast<crd::usize>(i) * m + i];
    }
}

} // namespace crd::hesap::preconditioners::detail
