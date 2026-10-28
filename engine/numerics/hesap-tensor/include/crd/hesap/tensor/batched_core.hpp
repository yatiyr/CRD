#pragma once

// The scalar tier of the batched factorizations (batched.hpp): one matrix at a time, in the exact operation order of
// the lane tier of the per-level tensor kernels (../../../../kernels/tensor_batched_kernels.cpp), so the two give the
// same bits lane for lane. The kernels run these above the lane tier's n and as their oracle; decomp.hpp runs them on
// its R x R systems. They live in the unit's level namespace (CRD_MATH_LEVEL_NS, ADR-0134 §5) and call only
// crd/math/libm.hpp, so general code and every kernel level compile their own copies.

#include <crd/core/types.hpp>
#include <crd/math/kernel_level.hpp>
#include <crd/math/libm.hpp>

#include <type_traits>

namespace crd::hesap::tensor::batcheddetail::inline CRD_MATH_LEVEL_NS
{

// Cholesky in place: the lower factor, the strict upper zeroed. info = 0, or the 1-based column whose pivot was not
// positive; that column continues on 1 (the lane tier's parity), so the flagged factor is unspecified.
template <typename T> inline void chol_scalar_one(T* a, crd::u64 n, crd::i32* info) noexcept
{
    namespace libm = crd::math::libm;
    *info = 0;
    for (crd::u64 j = 0; j < n; ++j)
    {
        T d = a[j * n + j];
        for (crd::u64 p = 0; p < j; ++p)
        {
            d = libm::fma(-a[j * n + p], a[j * n + p], d);
        }
        if (!(d > T(0)))
        {
            if (*info == 0)
            {
                *info = static_cast<crd::i32>(j) + 1;
            }
            d = T(1);
        }
        const T ljj = libm::sqrt(d);
        const T inv = T(1) / ljj;
        a[j * n + j] = ljj;
        for (crd::u64 i = j + 1U; i < n; ++i)
        {
            T s = a[i * n + j];
            for (crd::u64 p = 0; p < j; ++p)
            {
                s = libm::fma(-a[i * n + p], a[j * n + p], s);
            }
            a[i * n + j] = s * inv;
        }
        for (crd::u64 i = 0; i < j; ++i)
        {
            a[i * n + j] = T(0);
        }
    }
}

// LU with partial pivoting in place: L (unit lower, strict) and U. piv[j] is the 0-based row swapped into row j (the
// largest |a[i][j]|, strictly greater to win, so the lowest row takes a tie); info = 0 or the 1-based first singular
// column, which continues on a pivot of 1.
template <typename T> inline void lu_scalar_one(T* a, crd::u64 n, crd::i32* piv, crd::i32* info) noexcept
{
    namespace libm = crd::math::libm;
    *info = 0;
    for (crd::u64 j = 0; j < n; ++j)
    {
        crd::u64 pr = j;
        T best = a[j * n + j] < T(0) ? -a[j * n + j] : a[j * n + j];
        for (crd::u64 i = j + 1U; i < n; ++i)
        {
            const T v = a[i * n + j] < T(0) ? -a[i * n + j] : a[i * n + j];
            if (v > best)
            {
                best = v;
                pr = i;
            }
        }
        piv[j] = static_cast<crd::i32>(pr);
        if (pr != j)
        {
            for (crd::u64 c = 0; c < n; ++c)
            {
                const T t = a[j * n + c];
                a[j * n + c] = a[pr * n + c];
                a[pr * n + c] = t;
            }
        }
        T p = a[j * n + j];
        if (p == T(0))
        {
            if (*info == 0)
            {
                *info = static_cast<crd::i32>(j) + 1;
            }
            p = T(1);
        }
        const T inv = T(1) / p;
        for (crd::u64 i = j + 1U; i < n; ++i)
        {
            const T l = a[i * n + j] * inv;
            a[i * n + j] = l;
            for (crd::u64 c = j + 1U; c < n; ++c)
            {
                a[i * n + c] = libm::fma(-l, a[j * n + c], a[i * n + c]);
            }
        }
    }
}

// The one-sided Jacobi rotation threshold: a column pair rotates while |gamma| > tol * sqrt(alpha * beta).
template <typename T> [[nodiscard]] inline T svd_tol() noexcept
{
    return std::is_same_v<T, crd::f32> ? T(1e-6) : T(1e-14);
}

// The cyclic one-sided Jacobi sweeps on one matrix (in-place columns of a; v starts as the identity). info = 0 once a
// sweep applies no rotation, 1 when the last of max_sweeps still rotated (bounded iteration, ADR-0095).
template <typename T>
inline void svd_scalar_sweeps(T* a, T* v, crd::u64 n, crd::u32 max_sweeps, crd::i32* info) noexcept
{
    namespace libm = crd::math::libm;
    for (crd::u64 i = 0; i < n * n; ++i)
    {
        v[i] = T(0);
    }
    for (crd::u64 i = 0; i < n; ++i)
    {
        v[i * n + i] = T(1);
    }
    const T tol = svd_tol<T>();
    *info = 1;
    for (crd::u32 sweep = 0; sweep < max_sweeps; ++sweep)
    {
        bool rotated = false;
        for (crd::u64 p = 0; p + 1U < n; ++p)
        {
            for (crd::u64 q = p + 1U; q < n; ++q)
            {
                T alpha = T(0);
                T beta = T(0);
                T gamma = T(0);
                for (crd::u64 r = 0; r < n; ++r)
                {
                    alpha = libm::fma(a[r * n + p], a[r * n + p], alpha);
                    beta = libm::fma(a[r * n + q], a[r * n + q], beta);
                    gamma = libm::fma(a[r * n + p], a[r * n + q], gamma);
                }
                const T g = gamma < T(0) ? -gamma : gamma;
                if (!(g > tol * libm::sqrt(alpha * beta)))
                {
                    continue;
                }
                rotated = true;
                // the lane tier's operations: fma forms, and 0 - x for a negation (the signed-zero rule)
                const T zeta = (beta - alpha) / (T(2) * gamma);
                const T az = zeta < T(0) ? -zeta : zeta;
                const T t = (zeta >= T(0) ? T(1) : T(-1)) / (az + libm::sqrt(libm::fma(zeta, zeta, T(1))));
                const T c = T(1) / libm::sqrt(libm::fma(t, t, T(1)));
                const T s = c * t;
                for (crd::u64 r = 0; r < n; ++r)
                {
                    const T ap = a[r * n + p];
                    const T aq = a[r * n + q];
                    a[r * n + p] = libm::fma(c, ap, T(0) - s * aq);
                    a[r * n + q] = libm::fma(s, ap, c * aq);
                    const T vp = v[r * n + p];
                    const T vq = v[r * n + q];
                    v[r * n + p] = libm::fma(c, vp, T(0) - s * vq);
                    v[r * n + q] = libm::fma(s, vp, c * vq);
                }
            }
        }
        if (!rotated)
        {
            *info = 0;
            return;
        }
    }
}

// The post-pass of both tiers: sigma = the column norms, U = the normalized columns (a zero column becomes e_j), then a
// stable descending sort of sigma that permutes the columns of U and V with it.
template <typename T> inline void svd_finalize_one(T* a, T* v, T* sigma, crd::u64 n) noexcept
{
    namespace libm = crd::math::libm;
    for (crd::u64 j = 0; j < n; ++j)
    {
        T ss = T(0);
        for (crd::u64 r = 0; r < n; ++r)
        {
            ss = libm::fma(a[r * n + j], a[r * n + j], ss);
        }
        const T nrm = libm::sqrt(ss);
        sigma[j] = nrm;
        if (nrm > T(0))
        {
            const T inv = T(1) / nrm;
            for (crd::u64 r = 0; r < n; ++r)
            {
                a[r * n + j] *= inv;
            }
        }
        else
        {
            for (crd::u64 r = 0; r < n; ++r)
            {
                a[r * n + j] = r == j ? T(1) : T(0);
            }
        }
    }
    for (crd::u64 j = 1; j < n; ++j)
    {
        const T sj = sigma[j];
        crd::u64 pos = j;
        while (pos > 0U && sigma[pos - 1U] < sj)
        {
            --pos;
        }
        if (pos == j)
        {
            continue;
        }
        for (crd::u64 m = j; m > pos; --m)
        {
            sigma[m] = sigma[m - 1U];
            for (crd::u64 r = 0; r < n; ++r)
            {
                const T tu = a[r * n + m];
                a[r * n + m] = a[r * n + m - 1U];
                a[r * n + m - 1U] = tu;
                const T tv = v[r * n + m];
                v[r * n + m] = v[r * n + m - 1U];
                v[r * n + m - 1U] = tv;
            }
        }
        sigma[pos] = sj;
    }
}

} // namespace crd::hesap::tensor::batcheddetail::inline CRD_MATH_LEVEL_NS
