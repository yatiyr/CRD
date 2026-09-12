#pragma once

// crd-hesap-diff v13-m — SPECTRAL differentiation matrices (Trefethen, Spectral Methods in MATLAB): build a dense
// matrix D such that (D·u) are the derivative values of u sampled on the grid, with EXPONENTIAL (spectral) accuracy
// for smooth functions — far beyond any finite-difference order. Two grids:
//   chebyshev_diff_matrix — Chebyshev-Lobatto nodes x_j = cos(jπ/(n−1)) on [−1,1] (non-periodic; the workhorse for
//                           boundary-value problems / PDE collocation). Diagonal via the accurate negative-sum trick.
//   fourier_diff_matrix   — equispaced nodes on [0,2π) (periodic; signals, periodic PDEs).
// Verified vs analytic derivatives of smooth test functions (spectral convergence) before this port. Moat:
// determinism (crd::math, fixed FP order) + allocation-free (caller-sized D / nodes).

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::diff
{

// Chebyshev differentiation matrix on n Chebyshev-Lobatto nodes x_j = cos(jπ/(n−1)), j=0..n−1, over [−1,1]. Fills the
// row-major n×n matrix D (D[i*n+j]) and the node vector `nodes` (length n). For u sampled at the nodes, (D·u)_i ≈ u'(x_i).
template <typename T>
void chebyshev_diff_matrix(int n, T* D, T* nodes)
{
    if (n < 2)
    {
        return;
    }
    const int nm1 = n - 1;
    const T   pi  = static_cast<T>(3.14159265358979323846264338327950288);
    for (int j = 0; j < n; ++j)
    {
        nodes[j] = crd::math::cos(pi * static_cast<T>(j) / static_cast<T>(nm1));
    }
    auto cfac = [&](int j) -> T {
        const T c = (j == 0 || j == nm1) ? T{2} : T{1};
        return (j % 2 == 0) ? c : -c; // c_j · (−1)^j
    };
    for (int i = 0; i < n; ++i)
    {
        T rowsum = T{0};
        for (int j = 0; j < n; ++j)
        {
            if (i != j)
            {
                const T dij = (cfac(i) / cfac(j)) / (nodes[i] - nodes[j]);
                D[i * n + j] = dij;
                rowsum += dij;
            }
        }
        D[i * n + i] = -rowsum; // negative-sum trick (more accurate than the closed-form diagonal)
    }
}

// Fourier (periodic) first-derivative differentiation matrix on n equispaced nodes x_j = 2πj/n, j=0..n−1, over [0,2π).
// Fills the row-major n×n matrix D and the node vector. For periodic u, (D·u)_i ≈ u'(x_i) with spectral accuracy.
template <typename T>
void fourier_diff_matrix(int n, T* D, T* nodes)
{
    if (n < 2)
    {
        return;
    }
    const T pi = static_cast<T>(3.14159265358979323846264338327950288);
    const T h  = T{2} * pi / static_cast<T>(n);
    for (int j = 0; j < n; ++j)
    {
        nodes[j] = h * static_cast<T>(j);
    }
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            if (i == j)
            {
                D[i * n + j] = T{0};
            }
            else
            {
                const int diff = i - j;
                // D_ij = 0.5·(−1)^{i−j}·cot((i−j)h/2)  (the classic periodic spectral first-derivative matrix)
                const T   ang  = static_cast<T>(diff) * h / T{2};
                const T   cot  = crd::math::cos(ang) / crd::math::sin(ang);
                const T   sign = (diff % 2 == 0) ? T{1} : T{-1};
                D[i * n + j]   = static_cast<T>(0.5) * sign * cot;
            }
        }
    }
}

} // namespace crd::hesap::diff
