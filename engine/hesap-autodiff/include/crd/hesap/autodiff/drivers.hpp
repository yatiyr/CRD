#pragma once

// drivers.hpp — Phase 3.1.6 v15-d: forward-mode DRIVERS over the SIMD vector-forward carrier (JetPackD<W>). These
// are the agent-facing entry points that turn a scalar-generic functor into a gradient / Jacobian / JVP. ADR-0097.
//
// RUNTIME-n TILING: an arbitrary-length gradient is strip-mined into ceil(n/W) SIMD passes — each pass seeds W input
// directions into the pack, evaluates f once, and reads W partials; the ragged final tile seeds only its valid
// directions (the rest carry a zero tangent and are ignored). W is a COMPILE-TIME tile width (default 8 = two Vec4d
// registers); the DIMENSION n is a RUNTIME argument. This separates the two things Ceres conflates (compile-time N
// vs a driver over arbitrary n), and needs no Eigen.
//
// ALLOCATION-FREE: the caller owns the jet workspace (a `Span<JetPackD<W>>` of length >= n). No hidden malloc — the
// Cerid allocator discipline. DETERMINISM MOAT: each output partial ∂f/∂x_k is computed in its OWN SIMD lane by a
// per-direction chain that never mixes with other lanes (forward mode is per-direction independent), so the result
// is BIT-IDENTICAL across the tile width W, across {1..16} parallel workers, and across platforms (single-rounded
// `fma`, fixed order). It matches the scalar Jet<T,N> driver to <= 1 ulp (fma vs mul+add — the correctness oracle).

#include <crd/hesap/autodiff/dual.hpp>
#include <crd/hesap/autodiff/jet_simd.hpp>

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::autodiff::forward
{

using crd::containers::ConstSpan;
using crd::containers::Span;

// ---- gradient: ∇f of a SCALAR functor f: R^n → R at x, into g[0..n). ---------------------------------------
// Functor: `T f(const T* x, int n)`. `scratch` (length >= n) is the caller-owned jet workspace.
template <int W = 8, class F>
inline void gradient(const F& f, ConstSpan<crd::f64> x, Span<crd::f64> g, Span<JetPackD<W>> scratch) noexcept
{
    const int n = static_cast<int>(x.size());
    for (int i = 0; i < n; ++i)
    {
        scratch[i] = JetPackD<W>(x[i]); // every input a constant (zero tangent)
    }
    crd::f64 part[W];
    for (int base = 0; base < n; base += W)
    {
        const int cnt = (n - base < W) ? (n - base) : W;
        for (int k = 0; k < cnt; ++k)
        {
            scratch[base + k] = JetPackD<W>(x[base + k], k); // seed direction k of this tile
        }
        const JetPackD<W> y = f(scratch.data(), n);
        y.store_partials(part);
        for (int k = 0; k < cnt; ++k)
        {
            g[base + k] = part[k]; // ∂f/∂x_{base+k}
        }
        for (int k = 0; k < cnt; ++k)
        {
            scratch[base + k] = JetPackD<W>(x[base + k]); // reset to constant for the next tile
        }
    }
}

// ---- jacobian: J of a VECTOR functor f: R^n → R^m into row-major jac[m*n]. ----------------------------------
// Functor: `void f(const T* x, int n, T* y, int m)` (writes y[0..m)). `scratch` (>= n) + `yscratch` (>= m) caller-owned.
template <int W = 8, class F>
inline void jacobian(const F& f, ConstSpan<crd::f64> x, int m, Span<crd::f64> jac, Span<JetPackD<W>> scratch,
                     Span<JetPackD<W>> yscratch) noexcept
{
    const int n = static_cast<int>(x.size());
    for (int i = 0; i < n; ++i)
    {
        scratch[i] = JetPackD<W>(x[i]);
    }
    crd::f64 part[W];
    for (int base = 0; base < n; base += W)
    {
        const int cnt = (n - base < W) ? (n - base) : W;
        for (int k = 0; k < cnt; ++k)
        {
            scratch[base + k] = JetPackD<W>(x[base + k], k);
        }
        f(scratch.data(), n, yscratch.data(), m);
        for (int j = 0; j < m; ++j)
        {
            yscratch[j].store_partials(part);
            for (int k = 0; k < cnt; ++k)
            {
                jac[j * n + base + k] = part[k]; // ∂f_j/∂x_{base+k}
            }
        }
        for (int k = 0; k < cnt; ++k)
        {
            scratch[base + k] = JetPackD<W>(x[base + k]);
        }
    }
}

// ---- jvp: forward Jacobian-vector product  (J·v) of f: R^n → R^m into out[0..m), ONE Dual pass. --------------
// Functor: `void f(const Dual<f64>* x, int n, Dual<f64>* y, int m)`. `scratch` (>= n) + `yscratch` (>= m) caller-owned.
template <class F>
inline void jvp(const F& f, ConstSpan<crd::f64> x, ConstSpan<crd::f64> v, int m, Span<crd::f64> out,
                Span<Dual<crd::f64>> scratch, Span<Dual<crd::f64>> yscratch) noexcept
{
    const int n = static_cast<int>(x.size());
    for (int i = 0; i < n; ++i)
    {
        scratch[i] = Dual<crd::f64>{x[i], v[i]}; // seed the whole tangent v at once
    }
    f(scratch.data(), n, yscratch.data(), m);
    for (int j = 0; j < m; ++j)
    {
        out[j] = yscratch[j].d; // (J·v)_j
    }
}

// Scalar-output JVP convenience: directional derivative  ∇f·v  of f: R^n → R (one Dual pass).
template <class F>
[[nodiscard]] inline crd::f64 directional(const F& f, ConstSpan<crd::f64> x, ConstSpan<crd::f64> v,
                                          Span<Dual<crd::f64>> scratch) noexcept
{
    const int n = static_cast<int>(x.size());
    for (int i = 0; i < n; ++i)
    {
        scratch[i] = Dual<crd::f64>{x[i], v[i]};
    }
    return f(scratch.data(), n).d;
}

} // namespace crd::hesap::autodiff::forward
