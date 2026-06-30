#pragma once

// crd-hesap-quadrature v13-g — the integrate() API + the error-tier-exposing result contract + fixed-order rule
// integrators (Gauss / Lobatto / Radau on [a,b]) + composite rules on samples (trapezoid / Simpson, scipy-exact) +
// Newton-Cotes weights. ADR-0095 pillar 3 (error-tier-exposing): every integral returns a `QuadResult` carrying
// {value, error_estimate, eval_count, subdiv_count, status, tolerance_met}. For the FIXED-order rules here the
// error_estimate is 0 (a fixed rule offers no self-estimate — honestly Tier-0, not a foolable Tier-1 bound); the
// adaptive Gauss-Kronrod engine that fills error_estimate/subdiv_count/tolerance_met lands in v13-h.
//
// Determinism by construction (pillar 1): fixed FP summation order, crd::math, no data-dependent branches in the
// arithmetic ⇒ bit-identical across compilers/opt-levels. Allocation (pillar 2): the n nodes/weights once per call;
// the composite-on-samples rules allocate nothing.

#include <crd/hesap/quadrature/gauss.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::quadrature
{

enum class QuadStatus : crd::u8
{
    Ok              = 0,
    BadInput        = 1, // n < 1 / non-finite bounds / size mismatch
    MaxSubdivisions = 2, // adaptive: the subdivision budget was exhausted before the tolerance was met (WCET bound hit)
    RoundoffError   = 3, // adaptive: roundoff prevents the requested tolerance (the estimate stopped improving)
};

// The error-tier result contract (ADR-0095 §pillar-3). For the fixed-order rules below: error_estimate = 0 (no
// estimate), subdiv_count = 0, tolerance_met = false. The adaptive engine (v13-h) populates all fields.
template <typename T>
struct QuadResult
{
    T          value          = T{0};
    T          error_estimate = T{0}; // Tier-1 estimate for adaptive rules; 0 here (a fixed rule self-estimates nothing)
    crd::u32   eval_count     = 0;
    crd::u32   subdiv_count   = 0;
    QuadStatus status         = QuadStatus::Ok;
    bool       tolerance_met  = false;

    [[nodiscard]] bool ok() const noexcept { return status == QuadStatus::Ok; }
};

namespace detail
{
template <typename T>
[[nodiscard]] constexpr bool quad_finite(T v) noexcept
{
    return (v == v) && (v - v == T{0});
}

// Apply a fixed [−1,1] rule (nodes/weights) to f on [a,b] via x ↦ ((b−a)x + (a+b))/2, scaling by (b−a)/2.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> apply_rule(F&& f, T a, T b, const T* nodes, const T* w, int n)
{
    const T h = (b - a) / T{2};
    const T m = (a + b) / T{2};
    T       s = T{0};
    for (int i = 0; i < n; ++i)
    {
        s += w[i] * f(h * nodes[i] + m);
    }
    QuadResult<T> r;
    r.value      = h * s;
    r.eval_count = static_cast<crd::u32>(n);
    return r;
}
} // namespace detail

// Apply a PRECOMPUTED [−1,1] rule (nodes/weights from gauss_legendre / gauss_lobatto / gauss_radau) to f over [a,b].
// The real-time / hot-loop path: compute the rule ONCE, integrate many — allocation-free, NO eigensolve per call
// (the integrate_gauss/lobatto/radau convenience wrappers below regenerate the rule each call).
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_with_nodes(F&& f, T a, T b, crd::containers::ConstSpan<T> nodes,
                                                 crd::containers::ConstSpan<T> weights)
{
    if (nodes.empty() || nodes.size() != weights.size() || !detail::quad_finite(a) || !detail::quad_finite(b))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    return detail::apply_rule<T>(std::forward<F>(f), a, b, nodes.data(), weights.data(),
                                 static_cast<int>(nodes.size()));
}

// Apply a PRECOMPUTED SYMMETRIC rule (nodes ±xᵢ ascending, symmetric weights — Gauss-Legendre and Gauss-Lobatto are
// symmetric) to f over [a,b], exploiting the symmetry: pair node i with node n−1−i so each weight multiplies the SUM
// of the ± evaluations (⌈n/2⌉ weight-mults instead of n). Matches a hand-specialized constexpr gauss<N> while staying
// a general runtime rule. ⚠ For a NON-symmetric rule (Gauss-Radau, arbitrary nodes) use integrate_with_nodes — this
// reuses weights[i] for the (i, n−1−i) pair, which is exact only when w is symmetric.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_symmetric(F&& f, T a, T b, crd::containers::ConstSpan<T> nodes,
                                                crd::containers::ConstSpan<T> weights)
{
    if (nodes.empty() || nodes.size() != weights.size() || !detail::quad_finite(a) || !detail::quad_finite(b))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    const T         h  = (b - a) / T{2};
    const T         mid = (a + b) / T{2};
    const crd::usize nn = nodes.size();
    T               s  = T{0};
    crd::usize      i  = 0;
    crd::usize      j  = nn - 1;
    while (i < j)
    {
        s += weights[i] * (f(mid + h * nodes[i]) + f(mid + h * nodes[j]));
        ++i;
        --j;
    }
    if (i == j) // odd node count: the unpaired centre node
    {
        s += weights[i] * f(mid + h * nodes[i]);
    }
    QuadResult<T> r;
    r.value      = h * s;
    r.eval_count = static_cast<crd::u32>(nn);
    return r;
}

// Fixed-order Gauss-Legendre integral of f over [a,b]. Exact for polynomials of degree ≤ 2n−1 (the optimal n-point
// degree). f: callable T→T. (Regenerates the rule each call — for repeated integration precompute once with
// gauss_legendre + integrate_with_nodes / integrate_symmetric.)
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_gauss(crd::memory::IAllocator* alloc, F&& f, T a, T b, int n)
{
    if (n < 1 || !detail::quad_finite(a) || !detail::quad_finite(b))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> w(alloc);
    x.resize(static_cast<crd::usize>(n));
    w.resize(static_cast<crd::usize>(n));
    gauss_legendre<T>(alloc, n, x.data(), w.data());
    return detail::apply_rule<T>(std::forward<F>(f), a, b, x.data(), w.data(), n);
}

// Fixed-order Gauss-Lobatto integral over [a,b] (n ≥ 2, includes both endpoints). Exact for degree ≤ 2n−3.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_lobatto(crd::memory::IAllocator* alloc, F&& f, T a, T b, int n)
{
    if (n < 2 || !detail::quad_finite(a) || !detail::quad_finite(b))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> w(alloc);
    x.resize(static_cast<crd::usize>(n));
    w.resize(static_cast<crd::usize>(n));
    gauss_lobatto<T>(alloc, n, x.data(), w.data());
    return detail::apply_rule<T>(std::forward<F>(f), a, b, x.data(), w.data(), n);
}

// Fixed-order Gauss-Radau integral over [a,b] (n ≥ 1, includes the LEFT endpoint a). Exact for degree ≤ 2n−2.
template <typename T, typename F>
[[nodiscard]] QuadResult<T> integrate_radau(crd::memory::IAllocator* alloc, F&& f, T a, T b, int n)
{
    if (n < 1 || !detail::quad_finite(a) || !detail::quad_finite(b))
    {
        return QuadResult<T>{T{0}, T{0}, 0, 0, QuadStatus::BadInput, false};
    }
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> w(alloc);
    x.resize(static_cast<crd::usize>(n));
    w.resize(static_cast<crd::usize>(n));
    gauss_radau<T>(alloc, n, x.data(), w.data());
    return detail::apply_rule<T>(std::forward<F>(f), a, b, x.data(), w.data(), n);
}

// Newton-Cotes weights for the closed (n+1)-point rule on n equal subintervals, canonical [0,n] form (scipy
// convention: ∫₀ⁿ f ≈ Σ wᵢ f(i)). Exact for degree ≤ n (n+1 for even n by symmetry). Computed by the EXACT
// Lagrange-integral wᵢ = (1/∏_{j≠i}(i−j))·∫₀ⁿ ∏_{j≠i}(x−j) dx — bit-close to scipy through the practical range
// (n ≤ 8; higher orders develop NEGATIVE weights and are numerically unstable, a property of the rule itself).
template <typename T>
void newton_cotes(crd::memory::IAllocator* alloc, int n, T* weights)
{
    crd::containers::Array<T> coef(alloc);
    for (int i = 0; i <= n; ++i)
    {
        coef.clear();
        coef.push_back(T{1}); // ∏ starts at the constant polynomial 1 (ascending coeffs)
        T denom = T{1};
        for (int j = 0; j <= n; ++j)
        {
            if (j == i)
            {
                continue;
            }
            // multiply the polynomial by (x − j): new[k] = old[k−1] − j·old[k] (descending k, in place).
            const crd::usize d = coef.size();
            coef.push_back(T{0});
            for (crd::usize k = d; k >= 1; --k)
            {
                coef[k] = coef[k - 1] - static_cast<T>(j) * coef[k];
            }
            coef[0] = -static_cast<T>(j) * coef[0];
            denom *= static_cast<T>(i - j);
        }
        // ∫₀ⁿ Σ coef[k] xᵏ dx = Σ coef[k] nᵏ⁺¹/(k+1).
        T integ = T{0};
        T npk   = static_cast<T>(n); // n¹ for k=0
        for (crd::usize k = 0; k < coef.size(); ++k)
        {
            integ += coef[k] * npk / static_cast<T>(k + 1);
            npk *= static_cast<T>(n);
        }
        weights[i] = integ / denom;
    }
}

// Composite trapezoidal rule on UNIFORMLY-spaced samples y (step dx). scipy.integrate.trapezoid.
template <typename T>
[[nodiscard]] T trapezoid(crd::containers::ConstSpan<T> y, T dx) noexcept
{
    const crd::usize n = y.size();
    if (n < 2)
    {
        return T{0};
    }
    T s = (y[0] + y[n - 1]) / T{2};
    for (crd::usize i = 1; i + 1 < n; ++i)
    {
        s += y[i];
    }
    return dx * s;
}

// Composite trapezoidal rule on NON-uniform samples (x, y). scipy.integrate.trapezoid(y, x).
template <typename T>
[[nodiscard]] T trapezoid(crd::containers::ConstSpan<T> y, crd::containers::ConstSpan<T> x) noexcept
{
    const crd::usize n = y.size();
    if (n < 2 || x.size() != n)
    {
        return T{0};
    }
    T s = T{0};
    for (crd::usize i = 0; i + 1 < n; ++i)
    {
        s += (x[i + 1] - x[i]) * (y[i] + y[i + 1]);
    }
    return s / T{2};
}

// Composite Simpson's rule on UNIFORMLY-spaced samples y (step dx). Bit-exact to scipy.integrate.simpson: standard
// composite Simpson when the interval count is even; for an odd interval count (even sample count), Simpson on the
// first n−1 samples + a parabolic correction for the trailing interval (scipy's Cartwright handling). n = 2 falls
// back to the single trapezoid.
template <typename T>
[[nodiscard]] T simpson(crd::containers::ConstSpan<T> y, T dx) noexcept
{
    const crd::usize n = y.size();
    if (n < 2)
    {
        return T{0};
    }
    if (n == 2)
    {
        return dx * (y[0] + y[1]) / T{2};
    }
    auto composite = [&](crd::usize cnt) -> T // composite Simpson over the first `cnt` samples (cnt odd)
    {
        T s = y[0] + y[cnt - 1];
        for (crd::usize i = 1; i + 1 < cnt; ++i)
        {
            s += (i % 2 == 1 ? T{4} : T{2}) * y[i];
        }
        return dx / T{3} * s;
    };
    if ((n - 1) % 2 == 0) // even interval count (odd n): standard
    {
        return composite(n);
    }
    // odd interval count (even n): Simpson on the first n−1 samples + parabolic last-interval correction.
    return composite(n - 1) + dx / T{12} * (T{8} * y[n - 2] + T{5} * y[n - 1] - y[n - 3]);
}

} // namespace crd::hesap::quadrature
