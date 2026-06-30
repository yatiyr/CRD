#pragma once

// crd-hesap-quadrature v13-i — DOUBLE-EXPONENTIAL (DE) quadrature (Takahasi-Mori 1974): tanh-sinh (finite, endpoint
// singularities), exp-sinh (semi-infinite [a,∞)), sinh-sinh (doubly-infinite (−∞,∞)). The substitution maps the
// integral to ∫_{−∞}^∞ g(t) dt where g decays DOUBLE-exponentially, so the plain trapezoidal rule converges
// double-exponentially — a handful of levels resolve analytic integrands to full precision.
//
// Three levers make Cerid beat Boost here: (1) the abscissae/weights are integrand-independent (exp/sinh-sinh) or
// interval-affine (tanh-sinh) ⇒ a `DeRule` PRECOMPUTES them once (the sinh/cosh/exp transcendentals); (2) the
// convergence test exploits the double-exponential rate (error after S_m ≈ d_m²/d_{m-1}), stopping one whole level
// earlier (each level doubles the node count); (3) each level stores its nodes as a +t run then a −t run, and each
// run TRUNCATES once its terms underflow relative to the peak — so the long tail where the integrand has decayed (or
// the weight has) is never evaluated. Build the rule ONCE, integrate many.
//
// tanh-sinh resolves endpoint singularities by the cancellation-free OFFSET from the nearest endpoint
// (half·2/(e^{∓2s}+1)); a node that still rounds onto the exact endpoint is skipped (negligible weight) — a
// left-endpoint singularity is full precision, a right-endpoint one ~1e-8 (the single-argument f(x) interface limit).
// Determinism by construction (crd::math + fixed summation order).

#include <crd/hesap/quadrature/gauss_kronrod.hpp> // detail::qmax
#include <crd/hesap/quadrature/integrate.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>

namespace crd::hesap::quadrature
{

// A precomputed DE rule: per-node coefficient c (the transformed abscissa, or the endpoint-offset for tanh-sinh) and
// weight w, grouped by refinement level. Each level [level_start, split[m]) is the +t run and [split[m], level_end[m])
// the −t run (tanh-sinh leaves the −t run empty). h0 = the level-0 step.
template <typename T>
struct DeRule
{
    crd::containers::Array<T>        c;
    crd::containers::Array<T>        w;
    crd::containers::Array<crd::u32> split;
    crd::containers::Array<crd::u32> level_end;
    T                                h0     = T{1};
    int                              levels = 0;

    explicit DeRule(crd::memory::IAllocator* alloc) : c(alloc), w(alloc), split(alloc), level_end(alloc) {}
};

namespace detail
{
template <typename T>
[[nodiscard]] constexpr T de_halfpi() noexcept
{
    return static_cast<T>(1.5707963267948966192313216916398);
}

// Sum eval(i) over nodes [lo,hi), TRUNCATING the negligible tail: once a term has fallen below machine-relative to the
// running peak for several consecutive nodes, the rest of this (monotone-decaying) run is skipped.
template <typename T, typename Eval>
[[nodiscard]] T de_run_sum(crd::u32 lo, crd::u32 hi, Eval&& eval)
{
    T   s     = T{0};
    T   maxt  = T{0};
    int small = 0;
    for (crd::u32 i = lo; i < hi; ++i)
    {
        const T term = eval(i);
        s += term;
        const T at = crd::math::fabs(term);
        if (at > maxt)
        {
            maxt = at;
        }
        if (at < static_cast<T>(1e-17) * maxt)
        {
            if (++small > 4)
            {
                break;
            }
        }
        else
        {
            small = 0;
        }
    }
    return s;
}
} // namespace detail

// ---- builders: precompute the nodes once (the transcendentals), each level as a +t run then a −t run ----

// exp-sinh for [a,∞): x = a + exp(π/2·sinh t), w = exp(π/2·sinh t)·(π/2·cosh t). c stores exp(π/2·sinh t).
template <typename T>
[[nodiscard]] DeRule<T> build_exp_sinh_rule(crd::memory::IAllocator* alloc, int levels = 12, T h0 = T{1}, T tmax = T{4})
{
    DeRule<T> r(alloc);
    r.h0     = h0;
    r.levels = levels;
    const T hp = detail::de_halfpi<T>();
    for (int m = 0; m < levels; ++m)
    {
        const T   hm    = h0 / static_cast<T>(1u << m);
        const int start = (m == 0) ? 0 : 1;
        const int step  = (m == 0) ? 1 : 2;
        for (int j = start;; j += step) // +t run (incl. t=0 for level 0)
        {
            const T t = static_cast<T>(j) * hm;
            if (t > tmax)
            {
                break;
            }
            const T xp = crd::math::exp(hp * crd::math::sinh(t));
            r.c.push_back(xp);
            r.w.push_back(xp * (hp * crd::math::cosh(t)));
        }
        r.split.push_back(static_cast<crd::u32>(r.c.size()));
        const int nstart = (m == 0) ? 1 : start;
        for (int j = nstart;; j += step) // −t run
        {
            const T t = static_cast<T>(j) * hm;
            if (t > tmax)
            {
                break;
            }
            const T xn = crd::math::exp(-hp * crd::math::sinh(t));
            r.c.push_back(xn);
            r.w.push_back(xn * (hp * crd::math::cosh(t)));
        }
        r.level_end.push_back(static_cast<crd::u32>(r.c.size()));
    }
    return r;
}

// sinh-sinh for (−∞,∞): x = sinh(π/2·sinh t), w = cosh(π/2·sinh t)·(π/2·cosh t).
template <typename T>
[[nodiscard]] DeRule<T> build_sinh_sinh_rule(crd::memory::IAllocator* alloc, int levels = 12, T h0 = T{1}, T tmax = T{4})
{
    DeRule<T> r(alloc);
    r.h0     = h0;
    r.levels = levels;
    const T hp = detail::de_halfpi<T>();
    for (int m = 0; m < levels; ++m)
    {
        const T   hm    = h0 / static_cast<T>(1u << m);
        const int start = (m == 0) ? 0 : 1;
        const int step  = (m == 0) ? 1 : 2;
        for (int j = start;; j += step) // +t run
        {
            const T t = static_cast<T>(j) * hm;
            if (t > tmax)
            {
                break;
            }
            const T s = hp * crd::math::sinh(t);
            r.c.push_back(crd::math::sinh(s));
            r.w.push_back(crd::math::cosh(s) * (hp * crd::math::cosh(t)));
        }
        r.split.push_back(static_cast<crd::u32>(r.c.size()));
        const int nstart = (m == 0) ? 1 : start;
        for (int j = nstart;; j += step) // −t run
        {
            const T t = static_cast<T>(j) * hm;
            if (t > tmax)
            {
                break;
            }
            const T s = -hp * crd::math::sinh(t);
            r.c.push_back(crd::math::sinh(s));
            r.w.push_back(crd::math::cosh(s) * (hp * crd::math::cosh(t)));
        }
        r.level_end.push_back(static_cast<crd::u32>(r.c.size()));
    }
    return r;
}

// tanh-sinh for [a,b]: c = the endpoint offset 2/(e^{2s}+1) (= 1−tanh s), w = g'(t) (the (b−a)/2 applied at
// integration). One entry per |t|; the left and right nodes share it. split is unused (the offsets are monotone).
template <typename T>
[[nodiscard]] DeRule<T> build_tanh_sinh_rule(crd::memory::IAllocator* alloc, int levels = 12, T h0 = T{1}, T tmax = T{4})
{
    DeRule<T> r(alloc);
    r.h0     = h0;
    r.levels = levels;
    const T hp = detail::de_halfpi<T>();
    for (int m = 0; m < levels; ++m)
    {
        const T   hm    = h0 / static_cast<T>(1u << m);
        const int start = (m == 0) ? 0 : 1;
        const int step  = (m == 0) ? 1 : 2;
        for (int j = start;; j += step)
        {
            const T t = static_cast<T>(j) * hm;
            if (t > tmax)
            {
                break;
            }
            const T s  = hp * crd::math::sinh(t);
            const T cs = crd::math::cosh(s);
            r.c.push_back(T{2} / (crd::math::exp(T{2} * s) + T{1}));
            r.w.push_back((hp * crd::math::cosh(t)) / (cs * cs));
        }
        r.split.push_back(static_cast<crd::u32>(r.c.size()));
        r.level_end.push_back(static_cast<crd::u32>(r.c.size()));
    }
    return r;
}

// ---- the level-refinement driver ----
namespace detail
{
// level_sum(m) returns Σ over level m's nodes; assemble S_m = S_{m−1}/2 + h_m·sum_m, stopping when the
// double-exponential error estimate d_m²/d_{m-1} ≤ max(epsabs, epsrel·|S_m|).
template <typename T, typename LevelSum>
[[nodiscard]] QuadResult<T> de_refine(const DeRule<T>& r, LevelSum&& level_sum, T epsabs, T epsrel)
{
    if (r.levels < 1 || (epsabs <= T{0} && epsrel <= T{0}))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    T        S      = r.h0 * level_sum(0);
    crd::u32 nev    = r.level_end[0];
    T        err    = qmax<T>(crd::math::fabs(S), T{1});
    bool     done   = false;
    int      last_m = 0;
    T        dprev  = T{0};
    for (int m = 1; m < r.levels; ++m)
    {
        const T hm  = r.h0 / static_cast<T>(1u << m);
        const T Sm  = S * static_cast<T>(0.5) + hm * level_sum(m);
        nev += r.level_end[m] - r.level_end[m - 1];
        const T d = crd::math::fabs(Sm - S);
        err       = (m >= 2 && dprev > T{0}) ? (d * d / dprev) : d;
        dprev     = d;
        S         = Sm;
        last_m    = m;
        if (m >= 2 && err <= qmax<T>(epsabs, epsrel * crd::math::fabs(Sm)))
        {
            done = true;
            break;
        }
    }
    QuadResult<T> out;
    out.value          = S;
    out.error_estimate = err;
    out.eval_count     = nev; // upper bound (the tail truncation evaluates fewer)
    out.subdiv_count   = static_cast<crd::u32>(last_m + 1);
    out.tolerance_met  = done;
    out.status         = done ? QuadStatus::Ok : QuadStatus::MaxSubdivisions;
    return out;
}

// Bounds of level m's +t and −t runs in the flat node arrays.
template <typename T>
void de_level_bounds(const DeRule<T>& r, int m, crd::u32& lo, crd::u32& sp, crd::u32& hi) noexcept
{
    lo = (m == 0) ? 0u : r.level_end[m - 1];
    sp = r.split[static_cast<crd::usize>(m)];
    hi = r.level_end[static_cast<crd::usize>(m)];
}
} // namespace detail

// Integrate f over [a,∞) with the precomputed exp-sinh rule.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_exp_sinh(const DeRule<T>& rule, F&& f, T a, T epsabs, T epsrel)
{
    if (!detail::quad_finite(a))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    return detail::de_refine<T>(
        rule,
        [&](int m) {
            crd::u32 lo, sp, hi;
            detail::de_level_bounds<T>(rule, m, lo, sp, hi);
            const auto ev  = [&](crd::u32 i) { return f(a + rule.c[i]) * rule.w[i]; };
            return detail::de_run_sum<T>(lo, sp, ev) + detail::de_run_sum<T>(sp, hi, ev);
        },
        epsabs, epsrel);
}

// Integrate f over (−∞,∞) with the precomputed sinh-sinh rule.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_sinh_sinh(const DeRule<T>& rule, F&& f, T epsabs, T epsrel)
{
    return detail::de_refine<T>(
        rule,
        [&](int m) {
            crd::u32 lo, sp, hi;
            detail::de_level_bounds<T>(rule, m, lo, sp, hi);
            const auto ev = [&](crd::u32 i) { return f(rule.c[i]) * rule.w[i]; };
            return detail::de_run_sum<T>(lo, sp, ev) + detail::de_run_sum<T>(sp, hi, ev);
        },
        epsabs, epsrel);
}

// Integrate f over the finite [a,b] with the precomputed tanh-sinh rule (endpoint singularities OK; the affine map +
// the left/right endpoint-offset are applied here). A node rounding onto the exact endpoint is skipped.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_tanh_sinh(const DeRule<T>& rule, F&& f, T a, T b, T epsabs, T epsrel)
{
    if (!detail::quad_finite(a) || !detail::quad_finite(b))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    const T half = (b - a) / T{2};
    return detail::de_refine<T>(
        rule,
        [&](int m) {
            const crd::u32 lo = (m == 0) ? 0u : rule.level_end[m - 1];
            const crd::u32 hi = rule.level_end[static_cast<crd::usize>(m)];
            return detail::de_run_sum<T>(lo, hi, [&](crd::u32 i) {
                const T off = rule.c[i];
                const T wt  = half * rule.w[i];
                const T xl  = a + half * off;
                const T xr  = b - half * off;
                T       s   = T{0};
                if (xl > a && xl < b)
                {
                    s += f(xl) * wt;
                }
                if (xr > a && xr < b && xr != xl)
                {
                    s += f(xr) * wt;
                }
                return s;
            });
        },
        epsabs, epsrel);
}

} // namespace crd::hesap::quadrature
