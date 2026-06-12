#pragma once

// symplectic.hpp — Phase 3.1.6 v9-g: geometric integrators for second-order systems x'' = a(t, x)
// (separable Hamiltonians — the eylem game-mode steppers and the ADR-0063 replay contract landing in
// hesap). KERNEL-layer like steppers.hpp: raw-span, allocation-free, inlined force callable
// `acc(T t, ConstSpan<const T> x, Span<T> a)`, fixed per-element FP order (bit-deterministic runs).
//
// FAMILY:
//   • step_symplectic_euler — kick-drift, order 1, 1 force eval. THE game-physics default (semi-implicit
//     Euler); long-run energy bounded (symplectic), unlike explicit Euler's blowup.
//   • step_velocity_verlet — order 2, 1 force eval/step via the FSAL acceleration (`a_io` carries a(x_n)
//     in, a(x_{n+1}) out; caller seeds it once with acc(t0, x0)). Time-reversible.
//   • step_composition — the Yoshida machinery: a palindromic sequence of velocity-Verlet substeps with
//     scaled steps w_k·h. With yoshida4_w (3 stages) ⇒ order 4 (3 force evals); with yoshida6_w
//     (7 stages, Yoshida 1990 solution A) ⇒ order 6 (7 force evals). 8th order = a later append (its
//     15-coefficient set is fetched, not recalled — the honesty rule for constants).
//
// Energy behavior (the v9-g gates): symplectic members show BOUNDED oscillating energy error with no
// secular drift over long Kepler integrations, where same-h RK4 drifts monotonically. ADR-0091.

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::ode
{

// Yoshida 4th-order composition weights (the classic w1, w0, w1 triple; w1 = 1/(2 − 2^{1/3})).
// 2^{1/3} spelled as an exact-precision literal so the array stays constexpr.
inline constexpr crd::f64 yoshida_cbrt2 = 1.2599210498948732;
inline constexpr crd::f64 yoshida4_w[3] = {1.0 / (2.0 - yoshida_cbrt2), -yoshida_cbrt2 / (2.0 - yoshida_cbrt2),
                                           1.0 / (2.0 - yoshida_cbrt2)};

// Yoshida 6th-order, solution A (Yoshida 1990, Table 1): palindromic w3 w2 w1 w0 w1 w2 w3.
inline constexpr crd::f64 yoshida6_w[7] = {0.78451361047755726, 0.23557321335935699, -1.1776799841788701,
                                           1.3151863206839063,  -1.1776799841788701, 0.23557321335935699,
                                           0.78451361047755726};

// Semi-implicit (symplectic) Euler, kick-drift: v += h·a(t, x); x += h·v. Order 1, 1 force eval.
template <typename T, typename FAcc>
void step_symplectic_euler(FAcc&& acc, T t, crd::containers::Span<T> x, crd::containers::Span<T> v, T h,
                           crd::containers::Span<T> scratch)
{
    const crd::usize n = x.size();
    CRD_ASSERT(v.size() == n && scratch.size() >= n);
    const crd::containers::Span<T> a = scratch.subspan(0, n);
    acc(t, crd::containers::ConstSpan<T>(x.data(), n), a);
    for (crd::usize i = 0; i < n; ++i)
    {
        v[i] += h * a[i];
        x[i] += h * v[i];
    }
}

// Velocity Verlet: x += h·v + (h²/2)·a; a_new = a(t+h, x); v += (h/2)·(a + a_new). Order 2, symplectic,
// time-reversible. `a_io` is the FSAL acceleration (in: a(t, x); out: a(t+h, x_new)) — seed it once with
// acc(t0, x0); 1 force eval per step thereafter.
template <typename T, typename FAcc>
void step_velocity_verlet(FAcc&& acc, T t, crd::containers::Span<T> x, crd::containers::Span<T> v,
                          crd::containers::Span<T> a_io, T h, crd::containers::Span<T> scratch)
{
    const crd::usize n = x.size();
    CRD_ASSERT(v.size() == n && a_io.size() == n && scratch.size() >= n);
    const crd::containers::Span<T> a_new = scratch.subspan(0, n);
    const T half_h = h * static_cast<T>(0.5);
    for (crd::usize i = 0; i < n; ++i)
    {
        x[i] += h * (v[i] + half_h * a_io[i]);
    }
    acc(t + h, crd::containers::ConstSpan<T>(x.data(), n), a_new);
    for (crd::usize i = 0; i < n; ++i)
    {
        v[i] += half_h * (a_io[i] + a_new[i]);
        a_io[i] = a_new[i];
    }
}

// Composition method: velocity-Verlet substeps scaled by w[k]·h (palindromic w ⇒ even order). With
// yoshida4_w ⇒ order 4; yoshida6_w ⇒ order 6. `a_io` seeded like step_velocity_verlet (substeps chain it).
template <typename T, typename FAcc>
void step_composition(FAcc&& acc, T t, crd::containers::Span<T> x, crd::containers::Span<T> v,
                      crd::containers::Span<T> a_io, T h, crd::containers::ConstSpan<crd::f64> w,
                      crd::containers::Span<T> scratch)
{
    T t_local = t;
    for (crd::usize k = 0; k < w.size(); ++k)
    {
        const T hk = h * static_cast<T>(w[k]);
        step_velocity_verlet(acc, t_local, x, v, a_io, hk, scratch);
        t_local += hk;
    }
}

} // namespace crd::hesap::ode
