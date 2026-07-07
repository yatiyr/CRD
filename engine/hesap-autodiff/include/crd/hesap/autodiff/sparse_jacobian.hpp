#pragma once

// sparse_jacobian.hpp — Phase 3.1.6 v15-e: the sparse-Jacobian PIPELINE = trace → color → compressed recovery.
// (1) trace_jacobian: one abstract pass on JacPattern gives each output row's nonzero-column set.
// (2) distance2_color: greedy DISTANCE-2 coloring of the columns (two columns share a color only if NO output row
//     contains both — structural orthogonality; Gebremedhin-Manne-Pothen 2005). ncol << n for sparse structure.
// (3) sparse_jacobian: ncol forward JVP sweeps (the v15-d driver) build the compressed B = J·S (m×ncol); direct
//     (CPR) recovery scatters J[i,k] = B[i, color(k)] over the nonzeros — valid because a color class is orthogonal.
// The crush: ncol sweeps instead of n dense columns — an n/ncol speedup for sparse Jacobians, exact (bit-identical
// to the dense driver on the nonzeros). Allocation-free: the caller owns every scratch span. ADR-0097.

#include <crd/hesap/autodiff/drivers.hpp> // jvp
#include <crd/hesap/autodiff/dual.hpp>
#include <crd/hesap/autodiff/sparsity.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::autodiff::forward
{

// (1) Trace the structural pattern: seed each input i with {i}, evaluate the vector functor, read the m row-sets.
// Functor: `void f(const T* x, int n, T* y, int m)`. `rows` (>= m) receives the patterns; `scratch` (>= n) the seeds.
template <int W, class F>
inline void trace_jacobian(const F& f, int n, int m, JacPattern<W>* rows, JacPattern<W>* scratch) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        scratch[i] = JacPattern<W>::seed(i);
    }
    f(scratch, n, rows, m);
}

// (2) Distance-2 greedy coloring of the n columns from the m row patterns. Writes color[0..n); returns ncol.
// Validity (hard invariant): two columns co-occurring in ANY row get different colors.
template <int W>
[[nodiscard]] inline int distance2_color(const JacPattern<W>* rows, int m, int n, int* color) noexcept
{
    for (int j = 0; j < n; ++j)
    {
        color[j] = -1;
    }
    int ncol = 0;
    for (int j = 0; j < n; ++j)
    {
        JacPattern<W> nb{}; // columns that co-occur with j (union of all rows containing j)
        for (int i = 0; i < m; ++i)
        {
            if (rows[i].has(j))
            {
                nb = nb | rows[i];
            }
        }
        JacPattern<W> forbidden{}; // colors already used by a conflicting column
        for (int k = 0; k < n; ++k)
        {
            if (color[k] >= 0 && nb.has(k))
            {
                forbidden.bits[color[k] >> 6] |= crd::u64{1} << (color[k] & 63);
            }
        }
        int c = 0;
        while ((forbidden.bits[c >> 6] >> (c & 63)) & crd::u64{1})
        {
            ++c;
        }
        color[j] = c;
        if (c + 1 > ncol)
        {
            ncol = c + 1;
        }
    }
    return ncol;
}

// (3) Compressed recovery: ncol JVP sweeps build B (m×ncol) then CPR-scatter into dense jac[m*n] (zeros preserved).
// Functor: `void f(const Dual<f64>* x, int n, Dual<f64>* y, int m)`. Scratch (caller-owned): v(>=n), bcol(>=m),
// bmat(>=m*ncol), ds(>=n), dy(>=m).
template <int W, class F>
inline void sparse_jacobian(const F& f, ConstSpan<crd::f64> x, int m, const JacPattern<W>* rows, const int* color,
                            int ncol, Span<crd::f64> jac, Span<crd::f64> v, Span<crd::f64> bcol, Span<crd::f64> bmat,
                            Span<Dual<crd::f64>> ds, Span<Dual<crd::f64>> dy) noexcept
{
    const int n = static_cast<int>(x.size());
    for (int c = 0; c < ncol; ++c)
    {
        for (int j = 0; j < n; ++j)
        {
            v[j] = (color[j] == c) ? 1.0 : 0.0; // seed all columns of this color at once
        }
        jvp(f, x, ConstSpan<crd::f64>(v.data(), n), m, bcol, ds, dy); // bcol[i] = (J·v)_i = the color-c compressed column
        for (int i = 0; i < m; ++i)
        {
            bmat[i * ncol + c] = bcol[i];
        }
    }
    // direct (CPR) recovery: each nonzero reads its color's compressed column; structural zeros stay 0.
    for (int i = 0; i < m; ++i)
    {
        for (int k = 0; k < n; ++k)
        {
            jac[i * n + k] = rows[i].has(k) ? bmat[i * ncol + color[k]] : 0.0;
        }
    }
}

// ---- CSR fast path (the crush): O(nnz) recovery into a compressed-sparse-row Jacobian, no O(n²) densification. --
// build_csr: one-time (amortized) — fill row_ptr[0..m] + col_idx[0..nnz) from the pattern; returns nnz.
template <int W>
[[nodiscard]] inline int build_csr(const JacPattern<W>* rows, int m, int n, int* row_ptr, int* col_idx) noexcept
{
    int nnz    = 0;
    row_ptr[0] = 0;
    for (int i = 0; i < m; ++i)
    {
        for (int k = 0; k < n; ++k)
        {
            if (rows[i].has(k))
            {
                col_idx[nnz++] = k;
            }
        }
        row_ptr[i + 1] = nnz;
    }
    return nnz;
}

// sparse_jacobian_csr: ncol JVP sweeps → compressed B (m×ncol), then O(nnz) CPR recovery into values[0..nnz).
template <int W, class F>
inline void sparse_jacobian_csr(const F& f, ConstSpan<crd::f64> x, int m, const int* row_ptr, const int* col_idx,
                                const int* color, int ncol, Span<crd::f64> values, Span<crd::f64> v,
                                Span<crd::f64> bcol, Span<crd::f64> bmat, Span<Dual<crd::f64>> ds,
                                Span<Dual<crd::f64>> dy) noexcept
{
    const int n = static_cast<int>(x.size());
    for (int c = 0; c < ncol; ++c)
    {
        for (int j = 0; j < n; ++j)
        {
            v[j] = (color[j] == c) ? 1.0 : 0.0;
        }
        jvp(f, x, ConstSpan<crd::f64>(v.data(), n), m, bcol, ds, dy);
        for (int i = 0; i < m; ++i)
        {
            bmat[i * ncol + c] = bcol[i];
        }
    }
    for (int i = 0; i < m; ++i) // O(nnz): each nonzero reads its color's compressed column
    {
        for (int e = row_ptr[i]; e < row_ptr[i + 1]; ++e)
        {
            values[e] = bmat[i * ncol + color[col_idx[e]]];
        }
    }
}

} // namespace crd::hesap::autodiff::forward
