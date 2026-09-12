#pragma once

// crd-hesap-interp v13-a — 1-D piecewise interpolation + the interface/safety contract.
//
// The certification-grade substrate of the v13 cluster (ADR-0095). Three pillars on every entry point:
//   1. DETERMINISM BY CONSTRUCTION — these kernels are pure FMUL/FADD (no transcendentals, no data-dependent
//      branches in the arithmetic) ⇒ bit-identical across compilers/opt-levels/threads. The {1,4,16} moat holds.
//   2. ALLOCATION-FREE STREAMING — the eval kernels are `noexcept`, reentrant, and allocate NOTHING (the real-time
//      hot path: satellite ephemeris eval, robot LUT lookup at kHz). The build phase (object ctor) allocates ONCE.
//   3. ERROR-EXPOSING / STATUS-NOT-EXCEPTION — `build` returns `InterpStatus`; defensive validation rejects
//      NaN/Inf/non-increasing knots with a status, never a trap or garbage. Tier-2 certified error bounds where they
//      exist (`linear_worst_case_error`).
//
// Kernels: linear · nearest · cubic Hermite (scalar; the vector cousin is `crd::hesap::ode::hermite_eval`) · PCHIP
// (Fritsch-Carlson monotone — the no-overshoot, certifiable control-LUT default). Gated ≤1e-12 vs
// scipy.interpolate.{interp1d, CubicHermiteSpline, PchipInterpolator} + the no-overshoot invariant on monotone data.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <type_traits>

namespace crd::hesap::interp
{

template <typename T>
concept Real = std::is_floating_point_v<T>;

enum class InterpStatus : crd::u8
{
    Ok,
    BadInput,      // empty / size mismatch / NaN / Inf
    NotIncreasing, // knots x are not strictly increasing
};

namespace detail
{
// Finite without <cmath> (and without fast-math, which we never enable): NaN fails v==v; ±Inf fails v−v==0.
template <Real T> [[nodiscard]] constexpr bool is_finite(T v) noexcept
{
    return (v == v) && (v - v == static_cast<T>(0));
}

template <Real T> [[nodiscard]] constexpr int sgn(T v) noexcept
{
    return static_cast<int>(v > static_cast<T>(0)) - static_cast<int>(v < static_cast<T>(0));
}

template <Real T> [[nodiscard]] constexpr T abs_val(T v) noexcept
{
    return v < static_cast<T>(0) ? -v : v;
}
} // namespace detail

// Segment lookup: returns i ∈ [0, n−2] with x[i] ≤ xq < x[i+1] (clamped at the ends ⇒ out-of-range extrapolates on the
// boundary segment, matching scipy PchipInterpolator). O(1) amortized via the last-segment cache; O(log n) binary
// search on a jump. Assumes x strictly increasing, n ≥ 2 (validated at build time). noexcept, allocation-free.
template <Real T>
[[nodiscard]] crd::usize find_segment(crd::containers::ConstSpan<T> x, T xq, crd::usize& cache) noexcept
{
    const crd::usize n = x.size();
    const crd::usize last = n - 2;
    const crd::usize c = cache <= last ? cache : 0;
    if (xq >= x[c] && (c == last || xq < x[c + 1])) // O(1) cache hit (clustered queries)
    {
        return c;
    }
    crd::usize base = 0; // branchless binary search (Knuth) — no branch mispredicts on random queries
    crd::usize len = n;
    while (len > 1)
    {
        const crd::usize half = len / 2;
        base += static_cast<crd::usize>(x[base + half] <= xq) * half; // cmov, not a branch
        len -= half;
    }
    const crd::usize seg = base < last ? base : last; // largest i with x[i] ≤ xq, clamped to [0, last]
    cache = seg;
    return seg;
}

// Linear interpolation (scipy.interp1d(kind='linear') / numpy.interp). Convex combination ⇒ bounded, no overshoot.
template <Real T>
[[nodiscard]] T interp_linear(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, T xq,
                              crd::usize& cache) noexcept
{
    const crd::usize i = find_segment(x, xq, cache);
    const T t = (xq - x[i]) / (x[i + 1] - x[i]);
    return y[i] + t * (y[i + 1] - y[i]);
}

// Nearest-neighbour (scipy.interp1d(kind='nearest')). Ties (xq exactly at a segment midpoint) resolve to the right
// node.
template <Real T>
[[nodiscard]] T interp_nearest(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, T xq,
                               crd::usize& cache) noexcept
{
    const crd::usize i = find_segment(x, xq, cache);
    const T mid = (x[i] + x[i + 1]) / static_cast<T>(2);
    return xq < mid ? y[i] : y[i + 1];
}

// Cubic Hermite eval on the bracketing segment from per-knot derivatives `d` (scipy.CubicHermiteSpline). The scalar
// cousin of crd::hesap::ode::hermite_eval (vector dense output); reimplemented here to avoid an interp→ode edge for a
// 4-line basis. h00=2t³−3t²+1, h10=t³−2t²+t, h01=−2t³+3t², h11=t³−t².
template <Real T>
[[nodiscard]] T interp_hermite(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                               crd::containers::ConstSpan<T> d, T xq, crd::usize& cache) noexcept
{
    const crd::usize i = find_segment(x, xq, cache);
    const T h = x[i + 1] - x[i];
    const T t = (xq - x[i]) / h;
    const T t2 = t * t;
    const T t3 = t2 * t;
    const T h00 = static_cast<T>(2) * t3 - static_cast<T>(3) * t2 + static_cast<T>(1);
    const T h10 = t3 - static_cast<T>(2) * t2 + t;
    const T h01 = -static_cast<T>(2) * t3 + static_cast<T>(3) * t2;
    const T h11 = t3 - t2;
    return h00 * y[i] + h10 * h * d[i] + h01 * y[i + 1] + h11 * h * d[i + 1];
}

namespace detail
{
// PCHIP endpoint slope (scipy _edge_case): one-sided 3-point estimate, shape-limited so no overshoot is introduced.
template <Real T> [[nodiscard]] T pchip_edge(T h0, T h1, T m0, T m1) noexcept
{
    const T d = ((static_cast<T>(2) * h0 + h1) * m0 - h0 * m1) / (h0 + h1);
    if (sgn(d) != sgn(m0))
    {
        return static_cast<T>(0);
    }
    if (sgn(m0) != sgn(m1) && abs_val(d) > static_cast<T>(3) * abs_val(m0))
    {
        return static_cast<T>(3) * m0;
    }
    return d;
}
} // namespace detail

// Fritsch-Carlson monotone PCHIP tangents (scipy PchipInterpolator._find_derivatives), written into the caller-provided
// `d` (allocation-free). Interior: harmonic-mean weighting; a local extremum (opposite-sign or zero secants) ⇒ slope 0
// ⇒ PROVABLY no overshoot, monotonicity-preserving. The certifiable control-LUT interpolant.
template <Real T>
[[nodiscard]] InterpStatus pchip_slopes(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y,
                                        crd::containers::Span<T> d) noexcept
{
    const crd::usize n = x.size();
    if (n < 2 || y.size() != n || d.size() != n)
    {
        return InterpStatus::BadInput;
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        if (!detail::is_finite(x[i]) || !detail::is_finite(y[i]))
        {
            return InterpStatus::BadInput;
        }
    }
    for (crd::usize i = 0; i + 1 < n; ++i)
    {
        if (!(x[i] < x[i + 1]))
        {
            return InterpStatus::NotIncreasing;
        }
    }
    const auto sec = [&](crd::usize k)
    {
        return (y[k + 1] - y[k]) / (x[k + 1] - x[k]);
    }; // secant slope mk[k]
    if (n == 2)
    {
        const T m = sec(0);
        d[0] = m;
        d[1] = m;
        return InterpStatus::Ok;
    }
    for (crd::usize k = 1; k + 1 < n; ++k) // interior
    {
        const T mkm1 = sec(k - 1);
        const T mk = sec(k);
        if (mkm1 * mk <= static_cast<T>(0)) // local extremum / opposite signs ⇒ zero slope (no overshoot)
        {
            d[k] = static_cast<T>(0);
        }
        else
        {
            const T h_km1 = x[k] - x[k - 1];
            const T h_k = x[k + 1] - x[k];
            const T w1 = static_cast<T>(2) * h_k + h_km1;
            const T w2 = h_k + static_cast<T>(2) * h_km1;
            d[k] = (w1 + w2) / (w1 / mkm1 + w2 / mk); // weighted harmonic mean
        }
    }
    d[0] = detail::pchip_edge(x[1] - x[0], x[2] - x[1], sec(0), sec(1));
    d[n - 1] = detail::pchip_edge(x[n - 1] - x[n - 2], x[n - 2] - x[n - 3], sec(n - 2), sec(n - 3));
    return InterpStatus::Ok;
}

// Tier-2 a-priori certified error bound for linear interpolation: ‖f − L‖∞ ≤ (h²/8)·max|f″| (the constant is tight).
// A GUARANTEED upper bound (not an estimate) when the caller supplies a rigorous |f″| bound — see ADR-0095
// §error-tiers.
template <Real T> [[nodiscard]] constexpr T linear_worst_case_error(T max_h, T second_deriv_bound) noexcept
{
    return max_h * max_h / static_cast<T>(8) * second_deriv_bound;
}

// Build-once / evaluate-many PCHIP interpolant. ctor allocates the tangents ONCE (the init phase); eval is
// allocation-free, noexcept, O(1)-amortized (the real-time hot path). References the caller's x/y (kept alive).
template <Real T> class PchipInterpolant
{
public:
    explicit PchipInterpolant(crd::memory::IAllocator* alloc) noexcept : m_d(alloc) {}

    [[nodiscard]] InterpStatus build(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y)
    {
        m_x = x;
        m_y = y;
        m_d.resize(x.size());
        m_cache = 0;
        return pchip_slopes(x, y, crd::containers::Span<T>{m_d.data(), m_d.size()});
    }

    [[nodiscard]] T eval(T xq) const noexcept
    {
        return interp_hermite(m_x, m_y, crd::containers::ConstSpan<T>{m_d.data(), m_d.size()}, xq, m_cache);
    }

    [[nodiscard]] crd::containers::ConstSpan<T> slopes() const noexcept
    {
        return crd::containers::ConstSpan<T>{m_d.data(), m_d.size()};
    }

private:
    crd::containers::ConstSpan<T> m_x{};
    crd::containers::ConstSpan<T> m_y{};
    crd::containers::Array<T> m_d;
    mutable crd::usize m_cache = 0;
};

} // namespace crd::hesap::interp
