#pragma once

// crd-hesap-diff v13-l — FINITE DIFFERENCES done right: Fornberg arbitrary-stencil weights (any nodes, any derivative
// order, the stable generator) + central/forward derivatives with the PER-MAGNITUDE OPTIMAL step (never a hard-coded
// h) + Richardson/Ridders error-killing extrapolation (returns a derivative AND an error estimate) + FD gradient /
// Jacobian for vector functions. Verified vs the analytic stencils before this port.
//
// The documented accuracy floors (honest, ADR-0095 / SANITY #6): a central difference loses ~⅓ of the digits
// (best error ~ε^{2/3} at h~ε^{1/3}); a forward difference loses ~½ (~ε^{1/2} at h~ε^{1/2}). For MACHINE-EXACT
// gradients with zero cancellation, use complex_step.hpp (when f accepts complex args).
//
// Moat: determinism (crd::math, fixed FP order) + allocation-free (caller-sized weight buffer / stack stencils).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

#include <limits>
#include <utility>

namespace crd::hesap::diff
{

// A differentiation result that exposes its own error estimate (the error-tier contract).
template <typename T>
struct DiffResult
{
    T value          = T{0};
    T error_estimate = T{0}; // Tier-1 estimate (Ridders) — not a guaranteed bound
};

// Fornberg (1988) finite-difference weights: fill `c` (size (max_deriv+1)*nnodes) with c[k*nnodes + i] = the weight of
// node i for approximating the k-th derivative (k = 0..max_deriv) at the point z, using the arbitrary grid `nodes`.
// The numerically-stable recurrence — works for any node distribution (uniform or not), any derivative order.
template <typename T>
void fornberg_weights(T z, crd::containers::ConstSpan<T> nodes, int max_deriv, T* c)
{
    const int nn = static_cast<int>(nodes.size()); // nnodes
    const int n  = nn - 1;
    const int m  = max_deriv;
    for (int t = 0; t < (m + 1) * nn; ++t)
    {
        c[t] = T{0};
    }
    auto cf = [&](int k, int i) -> T& { return c[k * nn + i]; };
    T    c1 = T{1};
    T    c4 = nodes[0] - z;
    cf(0, 0) = T{1};
    for (int i = 1; i <= n; ++i)
    {
        const int mn = i < m ? i : m;
        T         c2 = T{1};
        const T   c5 = c4;
        c4           = nodes[static_cast<crd::usize>(i)] - z;
        for (int j = 0; j < i; ++j)
        {
            const T c3 = nodes[static_cast<crd::usize>(i)] - nodes[static_cast<crd::usize>(j)];
            c2 *= c3;
            if (j == i - 1)
            {
                for (int k = mn; k >= 1; --k)
                {
                    cf(k, i) = c1 * (static_cast<T>(k) * cf(k - 1, i - 1) - c5 * cf(k, i - 1)) / c2;
                }
                cf(0, i) = -c1 * c5 * cf(0, i - 1) / c2;
            }
            for (int k = mn; k >= 1; --k)
            {
                cf(k, j) = (c4 * cf(k, j) - static_cast<T>(k) * cf(k - 1, j)) / c3;
            }
            cf(0, j) = c4 * cf(0, j) / c3;
        }
        c1 = c2;
    }
}

namespace detail
{
// Per-magnitude optimal step: central ~ ε^{1/3}·scale, forward ~ ε^{1/2}·scale, scale = max(|x|, 1).
template <typename T>
[[nodiscard]] T optimal_h(T x, bool central) noexcept
{
    const T eps   = std::numeric_limits<T>::epsilon();
    const T scale = crd::math::fabs(x) > T{1} ? crd::math::fabs(x) : T{1};
    return (central ? crd::math::cbrt(eps) : crd::math::sqrt(eps)) * scale;
}
} // namespace detail

// First derivative by the 4th-order central 5-point stencil [1, −8, 0, 8, −1]/(12h), optimal h if h ≤ 0.
template <typename T, typename F>
[[nodiscard]] T derivative_central(F&& f, T x, T h = T{0})
{
    if (h <= T{0})
    {
        h = detail::optimal_h<T>(x, true);
    }
    return (f(x - T{2} * h) - static_cast<T>(8) * f(x - h) + static_cast<T>(8) * f(x + h) - f(x + T{2} * h))
           / (static_cast<T>(12) * h);
}

// Second derivative by the 4th-order central 5-point stencil [−1, 16, −30, 16, −1]/(12h²).
template <typename T, typename F>
[[nodiscard]] T second_derivative_central(F&& f, T x, T h = T{0})
{
    if (h <= T{0})
    {
        h = detail::optimal_h<T>(x, true);
    }
    return (-f(x - T{2} * h) + static_cast<T>(16) * f(x - h) - static_cast<T>(30) * f(x)
            + static_cast<T>(16) * f(x + h) - f(x + T{2} * h))
           / (static_cast<T>(12) * h * h);
}

namespace detail
{
// The tableau of derivative_ridders for x >= 0 (see there).
template <typename T, typename F>
[[nodiscard]] DiffResult<T> ridders_right(F&& f, T x, T h, int ntab)
{
    constexpr int maxLevels = 16;
    const int levels = ntab < 2 ? 2 : (ntab > maxLevels ? maxLevels : ntab);
    constexpr T shrink = static_cast<T>(1.4);
    const T shrinkSq = shrink * shrink;
    const T stopFactor = T{2};
    // The central difference at one nominal step; `used` receives the step that was sampled, which differs from the
    // nominal one by the rounding of x + nominal, and is the nominal one only when that sum is lost against x.
    auto central = [&](T nominal, T& used) -> T
    {
        const T edge = x + nominal;
        const T step = edge - x;
        used = step > T{0} ? step : nominal;
        const T ahead = f(edge);
        const T behind = f(x - step);
        return (ahead - behind) / (T{2} * used);
    };
    // Only the previous and the current tableau column are ever needed.
    T columns[2][maxLevels];
    T* prev = columns[0];
    T* cur = columns[1];
    // Weights of the finer and the coarser entry for each order of the level being formed.
    T keepFine[maxLevels];
    T dropCoarse[maxLevels];
    // The steps that were sampled, finest last.
    T sampled[maxLevels];
    T levelWeight = T{1};
    T nominal = crd::math::fabs(h) > T{0} ? crd::math::fabs(h) : static_cast<T>(0.1);
    const T start = nominal;
    bool fromSampled = false;
    prev[0] = central(nominal, sampled[0]);
    T bestErr = std::numeric_limits<T>::max();
    T best = prev[0];
    for (int level = 1; level < levels; ++level)
    {
        nominal /= shrink;
        cur[0] = central(nominal, sampled[level]);
        if (level == 1)
        {
            // The sum x + nominal is rounded by at most eps*(x + nominal)/2, so a sampled step is off from its
            // nominal one by a relative d <= eps*(x + start)/(2*nominal) at most, whatever f is. A weight off by d
            // moves a tableau entry by about 4*d times the difference of the two entries it combines, and two steps
            // can each be off: 8*d times the first difference, held to 64 eps of the value, is the condition below.
            // The bound falls with the step (the differences fall as its square), so the first level decides.
            fromSampled = (x + start) * crd::math::fabs(cur[0] - prev[0]) > T{16} * nominal * crd::math::fabs(cur[0]);
        }
        if (fromSampled)
        {
            const T finerInv = T{1} / sampled[level];
            for (int order = 1; order <= level; ++order)
            {
                const T ratio = sampled[level - order] * finerInv;
                const T w = ratio * ratio;
                // Equal sampled steps (quantised to ulp(x)) carry nothing to extrapolate with: keep the finer entry.
                dropCoarse[order] = w > T{1} ? T{1} / (w - T{1}) : T{0};
                keepFine[order] = w > T{1} ? w * dropCoarse[order] : T{1};
            }
        }
        else
        {
            levelWeight = shrinkSq * levelWeight;
            dropCoarse[level] = T{1} / (levelWeight - T{1});
            keepFine[level] = levelWeight * dropCoarse[level];
        }
        for (int order = 1; order <= level; ++order)
        {
            cur[order] = cur[order - 1] * keepFine[order] - prev[order - 1] * dropCoarse[order];
            const T dOrder = crd::math::fabs(cur[order] - cur[order - 1]);
            const T dLevel = crd::math::fabs(cur[order] - prev[order - 1]);
            const T spread = dOrder > dLevel ? dOrder : dLevel;
            if (spread <= bestErr)
            {
                bestErr = spread;
                best = cur[order];
            }
        }
        // The highest-order entry drifting away from its predecessor means round-off now dominates: stop.
        if (crd::math::fabs(cur[level] - prev[level - 1]) >= stopFactor * bestErr)
        {
            break;
        }
        T* const done = prev;
        prev = cur;
        cur = done;
    }
    return DiffResult<T>{best, bestErr};
}
} // namespace detail

// Ridders' method (Ridders 1982, "Accurate computation of F'(x) and F'(x)F''(x)", Adv. Eng. Softw. 4(2)): central
// differences at geometrically shrinking steps are combined by Neville-style Richardson extrapolation (each column
// of the tableau removes one more even power of the step), returning the first derivative AND a Tier-1 error
// estimate. Far more accurate than a fixed-h difference.
//
// Steps and weights at large |x|. Each nominal step is replaced by the difference (x + nominal) - x, which is exact
// once |x| is at least the step, and the point behind x is then exact as well: x +- step are exactly representable
// and 2*step is their true spacing. Dividing by the nominal 2*step instead costs a relative error of order
// ulp(x)/step, already 1e-12 at |x| = 100 with the default h. A negative x is differentiated on the mirrored function
// g(t) = f(-t) at -x, and the sign of the result is restored, so that the first point always lies on the side of x
// away from zero: there the point behind x moves toward zero and stays exact, where on the other side it would round
// whenever it crosses into the next binade. The points f is evaluated at are the same.
//
// The sampled steps are multiples of ulp(x), so once |x| is large they are no longer in the ratio 1.4, and the
// Richardson weights 1.4^(2k) leave a residue of order ulp(x)*step*|f'''| in the extrapolated value. The weights of the
// tableau (w/(w - 1) on the finer entry and 1/(w - 1) on the coarser one, w the squared ratio of the coarsest to the
// finest step of the entry) are therefore formed from the nominal ratio, once per level, while a bound on that residue
// stays at the rounding level of the answer, and from the sampled steps for the whole call otherwise. A zero or NaN h
// starts from 0.1, a negative h from its magnitude.
template <typename T, typename F>
[[nodiscard]] DiffResult<T> derivative_ridders(F&& f, T x, T h = static_cast<T>(0.1), int ntab = 10)
{
    if (x < T{0})
    {
        auto mirrored = [&f](T t) -> T
        {
            return f(-t);
        };
        const DiffResult<T> r = detail::ridders_right<T>(mirrored, -x, h, ntab);
        return DiffResult<T>{-r.value, r.error_estimate};
    }
    return detail::ridders_right<T>(f, x, h, ntab);
}

// First derivative by the 2nd-order forward difference (f(x+h)−f(x))/h with the optimal forward step (√ε·scale) if
// h ≤ 0. Use when f cannot be evaluated below x (one-sided data / a hard domain edge); loses ~½ the digits.
template <typename T, typename F>
[[nodiscard]] T derivative_forward(F&& f, T x, T h = T{0})
{
    if (h <= T{0})
    {
        h = detail::optimal_h<T>(x, false);
    }
    return (f(x + h) - f(x)) / h;
}

// FD Jacobian of a vector field f: R^n -> R^m via the 2nd-order central difference. `x` is the mutable point vector
// (restored on exit); `fp`/`fm` are scratch output vectors (length m); jac_out is row-major m×n. f: (ConstSpan in,
// Span out) -> void.
template <typename T, typename F>
void jacobian_central(F&& f, crd::containers::Span<T> x, crd::containers::Span<T> fp, crd::containers::Span<T> fm,
                      T* jac_out)
{
    const int n = static_cast<int>(x.size());
    const int m = static_cast<int>(fp.size());
    for (int c = 0; c < n; ++c)
    {
        const T xc = x[static_cast<crd::usize>(c)];
        const T h  = detail::optimal_h<T>(xc, true);
        x[static_cast<crd::usize>(c)] = xc + h;
        f(crd::containers::ConstSpan<T>{x.data(), x.size()}, fp);
        x[static_cast<crd::usize>(c)] = xc - h;
        f(crd::containers::ConstSpan<T>{x.data(), x.size()}, fm);
        x[static_cast<crd::usize>(c)] = xc;
        for (int r = 0; r < m; ++r)
        {
            jac_out[r * n + c] = (fp[static_cast<crd::usize>(r)] - fm[static_cast<crd::usize>(r)]) / (T{2} * h);
        }
    }
}

template <typename T, typename F>
void gradient_central(F&& f, crd::containers::Span<T> x, T* grad_out); // forward decl (defined below)

// Hessian-vector product H·v of a scalar field f: R^n -> R via the central FD of the gradient:
// (∇f(x+εv) − ∇f(x−εv)) / (2ε), with ∇f itself by the central stencil. `x` mutable (restored); `gp`/`gm` scratch
// gradients (length n); hv_out length n. ε auto-scaled to the step.
template <typename T, typename F>
void hessian_vector_central(F&& f, crd::containers::Span<T> x, crd::containers::ConstSpan<T> v, T* gp, T* gm, T* hv_out)
{
    const int n   = static_cast<int>(x.size());
    const T   eps = detail::optimal_h<T>(T{1}, true);
    for (int i = 0; i < n; ++i)
    {
        x[static_cast<crd::usize>(i)] += eps * v[static_cast<crd::usize>(i)];
    }
    gradient_central<T>(f, x, gp);
    for (int i = 0; i < n; ++i)
    {
        x[static_cast<crd::usize>(i)] -= T{2} * eps * v[static_cast<crd::usize>(i)];
    }
    gradient_central<T>(f, x, gm);
    for (int i = 0; i < n; ++i)
    {
        x[static_cast<crd::usize>(i)] += eps * v[static_cast<crd::usize>(i)]; // restore
        hv_out[i] = (gp[i] - gm[i]) / (T{2} * eps);
    }
}

// FD gradient of a scalar field f: R^n -> R via the 4th-order central stencil. grad_out has length n = x.size().
template <typename T, typename F>
void gradient_central(F&& f, crd::containers::Span<T> x, T* grad_out)
{
    const int n = static_cast<int>(x.size());
    for (int i = 0; i < n; ++i)
    {
        const T xi = x[static_cast<crd::usize>(i)];
        const T h  = detail::optimal_h<T>(xi, true);
        auto    g  = [&](T t) {
            x[static_cast<crd::usize>(i)] = t;
            const T v                     = f(crd::containers::ConstSpan<T>{x.data(), x.size()});
            return v;
        };
        grad_out[i] = derivative_central<T>(g, xi, h);
        x[static_cast<crd::usize>(i)] = xi;
    }
}

} // namespace crd::hesap::diff
