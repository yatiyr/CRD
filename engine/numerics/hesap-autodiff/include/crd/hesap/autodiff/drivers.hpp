#pragma once

// drivers.hpp — Phase 3.1.6 v15-d: the forward-mode Jacobian-vector DRIVERS over Dual<f64>: one pass per tangent,
// the agent-facing entry points that turn a scalar-generic functor into a JVP or a directional derivative. ADR-0097.
// The gradient and Jacobian drivers over the SIMD carrier live with it in crd/hesap/autodiff/jet_simd.hpp (per CPU
// level, raw pointers, so a kernel unit can run them).
//
// ALLOCATION-FREE: the caller owns the Dual workspace (`scratch`, `yscratch`).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/autodiff/dual.hpp>

namespace crd::hesap::autodiff::forward
{

using crd::containers::ConstSpan;
using crd::containers::Span;

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
