#pragma once

// sparse_hessian.hpp — Phase 3.1.6 v15-e: sparse HESSIAN recovery. Given the Hessian sparsity pattern (from
// sparsity_hessian.hpp) as upper-triangle CSR, compute ONLY the nonzero entries H_ij = ∂²f/∂xᵢ∂xⱼ. Forward mode
// gets O(1) mixed partial per (ε1,ε2) seed; we vectorize ε2 into a WIDTH-W tile so ONE pass returns W entries of a
// row (the ε2-tiled analog of the v15-d gradient tiling). Carrier `HessRow<W>` = value + ε1 (row) + ε2[W] (column
// tile) + ε1ε2[W] (the W Hessian entries) — the flat hyper-dual generalized to W second directions. Passes =
// ceil(nnz/W) instead of the dense n(n+1)/2 — the sparsity crush (bit-identical to the dense hyper-dual Hessian).
//
// The theoretically-fewer-passes route (star coloring + B=H·S) needs the VECTOR HVP, which is forward-over-reverse
// ⇒ v16-e (research §B); star coloring itself ships here (sparse_jacobian.hpp `star_color`) for that consumer.
// Allocation-free: the caller owns the HessRow scratch. ADR-0097.

#include <crd/core/types.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::forward
{

template <int W>
struct HessRow
{
    crd::f64 f0;      // value
    crd::f64 f1;      // ∂/∂ε1 (the row direction)
    crd::f64 f2[W];   // ∂/∂ε2_k (W column directions)
    crd::f64 f12[W];  // ∂²/∂ε1∂ε2_k = H_{row, col_k}

    constexpr HessRow() noexcept : f0(0), f1(0), f2{}, f12{} {}
    constexpr HessRow(crd::f64 v) noexcept : f0(v), f1(0), f2{}, f12{} {} // NOLINT — constant
};

// chain rule for a unary φ: g=φ(f0), g1=φ'(f0), g2=φ''(f0).
template <int W>
[[nodiscard]] inline HessRow<W> hr_chain(const HessRow<W>& x, crd::f64 g, crd::f64 g1, crd::f64 g2) noexcept
{
    HessRow<W> r;
    r.f0 = g;
    r.f1 = g1 * x.f1;
    for (int k = 0; k < W; ++k)
    {
        r.f2[k]  = g1 * x.f2[k];
        r.f12[k] = g2 * x.f1 * x.f2[k] + g1 * x.f12[k];
    }
    return r;
}

template <int W>
[[nodiscard]] inline HessRow<W> operator+(const HessRow<W>& a, const HessRow<W>& b) noexcept
{
    HessRow<W> r;
    r.f0 = a.f0 + b.f0;
    r.f1 = a.f1 + b.f1;
    for (int k = 0; k < W; ++k)
    {
        r.f2[k]  = a.f2[k] + b.f2[k];
        r.f12[k] = a.f12[k] + b.f12[k];
    }
    return r;
}
template <int W>
[[nodiscard]] inline HessRow<W> operator-(const HessRow<W>& a, const HessRow<W>& b) noexcept
{
    HessRow<W> r;
    r.f0 = a.f0 - b.f0;
    r.f1 = a.f1 - b.f1;
    for (int k = 0; k < W; ++k)
    {
        r.f2[k]  = a.f2[k] - b.f2[k];
        r.f12[k] = a.f12[k] - b.f12[k];
    }
    return r;
}
template <int W>
[[nodiscard]] inline HessRow<W> operator*(const HessRow<W>& a, const HessRow<W>& b) noexcept
{
    HessRow<W> r;
    r.f0 = a.f0 * b.f0;
    r.f1 = a.f0 * b.f1 + a.f1 * b.f0;
    for (int k = 0; k < W; ++k)
    {
        r.f2[k]  = a.f0 * b.f2[k] + a.f2[k] * b.f0;
        r.f12[k] = a.f0 * b.f12[k] + a.f1 * b.f2[k] + a.f2[k] * b.f1 + a.f12[k] * b.f0;
    }
    return r;
}
template <int W>
[[nodiscard]] inline HessRow<W> recip(const HessRow<W>& x) noexcept
{
    const crd::f64 iv = 1.0 / x.f0;
    return hr_chain(x, iv, -iv * iv, 2.0 * iv * iv * iv);
}
template <int W>
[[nodiscard]] inline HessRow<W> operator/(const HessRow<W>& a, const HessRow<W>& b) noexcept { return a * recip(b); }

// scalar mixed
template <int W>
[[nodiscard]] inline HessRow<W> operator+(crd::f64 s, const HessRow<W>& x) noexcept { HessRow<W> r = x; r.f0 = s + x.f0; return r; }
template <int W>
[[nodiscard]] inline HessRow<W> operator+(const HessRow<W>& x, crd::f64 s) noexcept { return s + x; }
template <int W>
[[nodiscard]] inline HessRow<W> operator-(const HessRow<W>& x, crd::f64 s) noexcept { HessRow<W> r = x; r.f0 = x.f0 - s; return r; }
template <int W>
[[nodiscard]] inline HessRow<W> operator*(crd::f64 s, const HessRow<W>& x) noexcept
{
    HessRow<W> r;
    r.f0 = s * x.f0;
    r.f1 = s * x.f1;
    for (int k = 0; k < W; ++k) { r.f2[k] = s * x.f2[k]; r.f12[k] = s * x.f12[k]; }
    return r;
}
template <int W>
[[nodiscard]] inline HessRow<W> operator*(const HessRow<W>& x, crd::f64 s) noexcept { return s * x; }

// transcendentals (g, g', g'')
template <int W>
[[nodiscard]] inline HessRow<W> exp(const HessRow<W>& x) noexcept { const crd::f64 e = crd::math::exp(x.f0); return hr_chain(x, e, e, e); }
template <int W>
[[nodiscard]] inline HessRow<W> log(const HessRow<W>& x) noexcept { const crd::f64 iv = 1.0 / x.f0; return hr_chain(x, crd::math::log(x.f0), iv, -iv * iv); }
template <int W>
[[nodiscard]] inline HessRow<W> sin(const HessRow<W>& x) noexcept { crd::f64 s = 0, c = 0; crd::math::sincos(x.f0, s, c); return hr_chain(x, s, c, -s); }
template <int W>
[[nodiscard]] inline HessRow<W> cos(const HessRow<W>& x) noexcept { crd::f64 s = 0, c = 0; crd::math::sincos(x.f0, s, c); return hr_chain(x, c, -s, -c); }
template <int W>
[[nodiscard]] inline HessRow<W> sqrt(const HessRow<W>& x) noexcept { const crd::f64 r = crd::math::sqrt(x.f0); const crd::f64 g1 = 1.0 / (2.0 * r); return hr_chain(x, r, g1, -g1 / (2.0 * x.f0)); }
template <int W>
[[nodiscard]] inline HessRow<W> tanh(const HessRow<W>& x) noexcept { const crd::f64 t = crd::math::tanh(x.f0); const crd::f64 g1 = 1.0 - t * t; return hr_chain(x, t, g1, -2.0 * t * g1); }
template <int W>
[[nodiscard]] inline HessRow<W> pow(const HessRow<W>& x, crd::f64 p) noexcept
{
    const crd::f64 g  = crd::math::pow(x.f0, p);
    const crd::f64 g1 = p * crd::math::pow(x.f0, p - 1.0);
    const crd::f64 g2 = p * (p - 1.0) * crd::math::pow(x.f0, p - 2.0);
    return hr_chain(x, g, g1, g2);
}

// ---- Sparse Hessian recovery: fill values[0..nnz) from the UPPER-TRIANGLE CSR (row_ptr[n+1], col_idx[nnz], j>=i).
// Functor: `T f(const T* x, int n)` (scalar output). scratch (>= n) caller-owned. ceil(nnz/W) passes.
template <int W, class F>
inline void sparse_hessian(const F& f, const crd::f64* x, int n, const int* row_ptr, const int* col_idx,
                           crd::f64* values, HessRow<W>* scratch) noexcept
{
    for (int j = 0; j < n; ++j)
    {
        scratch[j] = HessRow<W>(x[j]);
    }
    for (int i = 0; i < n; ++i)
    {
        scratch[i].f1 = 1.0; // ε1 = e_i for the whole row
        for (int base = row_ptr[i]; base < row_ptr[i + 1]; base += W)
        {
            const int cnt = (row_ptr[i + 1] - base < W) ? (row_ptr[i + 1] - base) : W;
            for (int k = 0; k < cnt; ++k)
            {
                scratch[col_idx[base + k]].f2[k] = 1.0; // ε2_k = e_{col} (diagonal: col==i gets f1 AND f2)
            }
            const HessRow<W> y = f(scratch, n);
            for (int k = 0; k < cnt; ++k)
            {
                values[base + k] = y.f12[k];
            }
            for (int k = 0; k < cnt; ++k)
            {
                scratch[col_idx[base + k]].f2[k] = 0.0; // un-seed
            }
        }
        scratch[i].f1 = 0.0;
    }
}

} // namespace crd::hesap::autodiff::forward
