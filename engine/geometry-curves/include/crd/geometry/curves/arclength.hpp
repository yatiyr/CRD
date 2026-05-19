#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-curves — Arc-length system. Phase 3.1.7 v10c (2026-05-19).
//
// `ArclengthTable<T>` = cumulative chord-length over a uniform-t sampling of
// the curve. Drives constant-speed traversal (cinematic camera dolly, robot
// trajectory at a fixed velocity, animation keyframe-timing remap).
//
// Algorithm: sample the curve at `n+1` uniform t values, accumulate the
// chord lengths into `samples[i].distance`. Each entry is
// `{t, distance_from_start}`. Closed curves: the wrap chord from t=last
// to t=0 IS part of the table (last entry at t=1 holds total_length).
//
// **D198 (planned for v10-close)** — Table is uniform-t chord-length, NOT
// analytic arc length. Some curves admit closed-form length (circular
// arcs = r·θ, Bezier via Gauss-Legendre 5-pt integration). Uniform-table
// infrastructure is the same for all kinds; consumers wanting tighter
// length accuracy raise `n_samples`. Analytic specialisations filed as
// `v10c-analytic-arc-length` follow-on if a consumer asks.
//
// **D199 (planned for v10-close)** — Default `n_samples = 64`. Good for
// cinematic / robotics resolution; lightweight (~2 KB for f32 table).
//
// **D200 (planned for v10-close)** — Open curves emit `n+1` entries
// covering t ∈ [0, 1] inclusive; closed curves use the SAME layout with
// the final entry at t=1 representing the wrap-back point. Uniform
// table structure → uniform binary search.
//
// **D201 (planned for v10-close)** — `t_at_distance` / `distance_at_t`
// use binary search + linear interpolation. Higher-order interpolation
// would not improve accuracy because the table is itself piecewise-
// linear chord approximation. Tighter accuracy ⇒ raise `n_samples`.
//
// **D202 (planned for v10-close)** — Closed-curve modular reduction:
// distance wraps modulo `total_length`; t wraps modulo `1.0`. Negative
// inputs are normalised via floor-based modular reduction.
//
// Determinism: all loops are forward; binary search uses integer indices;
// no `std::sort` / no transcendentals.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/curves/evaluator.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::curves
{

inline constexpr crd::u32 k_arclength_default_samples = 64U;

template <crd::math::MathScalar T> struct ArclengthSample
{
    T t;        // curve parameter, [0, 1]
    T distance; // cumulative arc length from t=0
};

template <crd::math::MathScalar T> struct ArclengthTable
{
    crd::containers::Array<ArclengthSample<T>> samples;
    T                                          total_length = T{};
    bool                                       closed       = false;

    explicit ArclengthTable(crd::memory::IAllocator* alloc) noexcept : samples(alloc) {}
};

// ---------------------------------------------------------------------------
// build_arclength_table — uniform-t cumulative chord-length sampling.
//
// Open + closed share the same layout (D200): `n_samples + 1` entries at
// t = 0, 1/n, 2/n, ..., 1. For closed curves the chord from t=(n-1)/n to
// t=1 IS the wrap chord; total_length includes it.
// ---------------------------------------------------------------------------

template <typename Curve>
[[nodiscard]] ArclengthTable<typename Curve::scalar_t> build_arclength_table(
    const Curve&             curve,
    crd::u32                 n_samples,
    crd::memory::IAllocator* alloc) noexcept
{
    using T = typename Curve::scalar_t;
    CRD_ASSERT(n_samples >= 1U);

    ArclengthTable<T> out(alloc);
    out.closed = curve.closed;
    out.samples.reserve(n_samples + 1U);

    auto    prev      = evaluate(curve, static_cast<T>(0));
    T       cumulative = static_cast<T>(0);
    out.samples.push_back(ArclengthSample<T>{static_cast<T>(0), static_cast<T>(0)});

    const T inv_n = static_cast<T>(1) / static_cast<T>(n_samples);
    for (crd::u32 i = 1U; i <= n_samples; ++i)
    {
        const T t = (i == n_samples) ? static_cast<T>(1) : (static_cast<T>(i) * inv_n);
        const auto cur     = evaluate(curve, t);
        const T    seg_len = crd::math::length(cur - prev);
        cumulative += seg_len;
        out.samples.push_back(ArclengthSample<T>{t, cumulative});
        prev = cur;
    }

    out.total_length = cumulative;
    return out;
}

// Convenience: default n_samples.
template <typename Curve>
[[nodiscard]] ArclengthTable<typename Curve::scalar_t> build_arclength_table(
    const Curve& curve, crd::memory::IAllocator* alloc) noexcept
{
    return build_arclength_table(curve, k_arclength_default_samples, alloc);
}

// ---------------------------------------------------------------------------
// length_of — total arc length from the table.
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
[[nodiscard]] T length_of(const ArclengthTable<T>& table) noexcept
{
    return table.total_length;
}

// Convenience: build + measure in one shot. Discards the table.
template <typename Curve>
[[nodiscard]] typename Curve::scalar_t length_of(
    const Curve& curve, crd::u32 n_samples, crd::memory::IAllocator* alloc) noexcept
{
    const auto table = build_arclength_table(curve, n_samples, alloc);
    return table.total_length;
}

// ---------------------------------------------------------------------------
// Modular reduction helpers for closed-curve wrap.
// ---------------------------------------------------------------------------

namespace detail
{

template <crd::math::MathScalar T>
[[nodiscard]] constexpr T floor_mod(T value, T modulus) noexcept
{
    // floor-based modular reduction. Result is in [0, modulus).
    // Handles negative `value` correctly:
    //   floor_mod(-0.5, 1.0) = 0.5
    //   floor_mod( 1.5, 1.0) = 0.5
    const T n = static_cast<T>(static_cast<crd::i64>(
        value < static_cast<T>(0) ? value - modulus + static_cast<T>(1) : value));
    (void)n; // unused; explicit floor below avoids edge-case off-by-one.
    // Use long double arithmetic in the floor() avoidance: compute
    // `value - floor(value / modulus) * modulus` without std::floor (we
    // stay in deterministic-FP territory by hand).
    const T quotient = value / modulus;
    const crd::i64 i_quot = (quotient < static_cast<T>(0))
                                ? static_cast<crd::i64>(quotient) - 1
                                : static_cast<crd::i64>(quotient);
    const T q_floor = static_cast<T>(i_quot);
    T       result  = value - q_floor * modulus;
    // Defensive: clamp to [0, modulus) in case of edge-case rounding.
    if (result < static_cast<T>(0)) { result += modulus; }
    if (result >= modulus) { result -= modulus; }
    return result;
}

} // namespace detail

// ---------------------------------------------------------------------------
// t_at_distance — distance -> t via binary search + linear interpolation.
//
// Open: distance is clamped to [0, total_length].
// Closed: distance wraps modulo total_length (D202).
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
[[nodiscard]] T t_at_distance(const ArclengthTable<T>& table, T distance) noexcept
{
    CRD_ASSERT(table.samples.size() >= 2U);
    if (table.total_length <= static_cast<T>(0)) { return static_cast<T>(0); }

    T d = distance;
    if (table.closed)
    {
        d = detail::floor_mod(d, table.total_length);
    }
    else
    {
        if (d <= static_cast<T>(0)) { return table.samples[0].t; }
        if (d >= table.total_length) { return table.samples[table.samples.size() - 1U].t; }
    }

    // Binary search: find segment i where samples[i].distance <= d < samples[i+1].distance.
    crd::usize lo = 0U;
    crd::usize hi = table.samples.size() - 1U;
    while (lo + 1U < hi)
    {
        const crd::usize mid = lo + (hi - lo) / 2U;
        if (d < table.samples[mid].distance) { hi = mid; }
        else { lo = mid; }
    }

    const auto& a = table.samples[lo];
    const auto& b = table.samples[lo + 1U];
    const T     span = b.distance - a.distance;
    if (span <= static_cast<T>(0)) { return a.t; }
    const T u = (d - a.distance) / span;
    return a.t + (b.t - a.t) * u;
}

// ---------------------------------------------------------------------------
// distance_at_t — t -> distance via binary search + linear interpolation.
//
// Open: t is clamped to [0, 1].
// Closed: t wraps modulo 1.0 (D202).
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
[[nodiscard]] T distance_at_t(const ArclengthTable<T>& table, T t) noexcept
{
    CRD_ASSERT(table.samples.size() >= 2U);

    T t_eff = t;
    if (table.closed)
    {
        t_eff = detail::floor_mod(t_eff, static_cast<T>(1));
    }
    else
    {
        if (t_eff <= static_cast<T>(0)) { return table.samples[0].distance; }
        if (t_eff >= static_cast<T>(1)) { return table.samples[table.samples.size() - 1U].distance; }
    }

    // Binary search: find segment i where samples[i].t <= t_eff < samples[i+1].t.
    crd::usize lo = 0U;
    crd::usize hi = table.samples.size() - 1U;
    while (lo + 1U < hi)
    {
        const crd::usize mid = lo + (hi - lo) / 2U;
        if (t_eff < table.samples[mid].t) { hi = mid; }
        else { lo = mid; }
    }

    const auto& a = table.samples[lo];
    const auto& b = table.samples[lo + 1U];
    const T     span = b.t - a.t;
    if (span <= static_cast<T>(0)) { return a.distance; }
    const T u = (t_eff - a.t) / span;
    return a.distance + (b.distance - a.distance) * u;
}

} // namespace crd::geometry::curves
