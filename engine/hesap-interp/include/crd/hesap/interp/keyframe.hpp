#pragma once

// crd-hesap-interp — KEYFRAME TRACKS (GEO-8, D-007 row 73): the ONE curve engine three consumers share — animation
// clips (GEO-8), timeline automation (GEO-9), and audio parameter curves (GEO-10). A track is (times[], values[])
// with an interpolation mode per glTF 2.0's animation sampler semantics — the industry interchange contract:
//
//   Step         — values[] holds one N-component value per key; v(t) = value of the key at/left of t.
//   Linear       — same layout; component-wise lerp (the QUATERNION linear case is SLERP and lives with the
//                  consumer that owns quaternion math — this scalar engine samples lanes).
//   CubicHermite — values[] holds THREE N-component elements per key, glTF CUBICSPLINE order:
//                  [in_tangent · value · out_tangent]; segment eval is the standard Hermite basis with the SPLIT
//                  tangents (out-tangent of key k, in-tangent of key k+1), tangents in value-units per second —
//                  delegated to `interp_hermite` on the bracketing pair (scipy-parity basis, gated ≤1e-12).
//
// Same pillars as the rest of the module: deterministic FMUL/FADD arithmetic, allocation-free `noexcept` eval,
// out-of-range CLAMPS to the boundary key (the animation contract — a clip holds its first/last pose, never
// extrapolates). Times must be strictly increasing (the glTF requirement); callers validate at build/cook time.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/interp/piecewise.hpp>

namespace crd::hesap::interp
{

enum class KeyInterp : crd::u8
{
    Step = 0,
    Linear,
    CubicHermite, // glTF CUBICSPLINE: per key [in_tangent, value, out_tangent], each `components` wide
};

// The value stride (in elements of width `components`) a mode stores per key.
[[nodiscard]] constexpr crd::u32 key_elements(KeyInterp interp) noexcept
{
    return interp == KeyInterp::CubicHermite ? 3U : 1U;
}

// Sample lane `lane` (0..components-1) of a keyframe track at time `t`. `values.size()` must equal
// `times.size() * key_elements(interp) * components`. Out-of-range t clamps to the boundary key's VALUE.
// `cache` is the moving segment hint (one per streaming consumer; may be shared across lanes of one track).
template <Real T>
[[nodiscard]] T sample_track(crd::containers::ConstSpan<T> times, crd::containers::ConstSpan<T> values,
                             crd::u32 components, crd::u32 lane, KeyInterp interp, T t,
                             crd::usize& cache) noexcept
{
    const crd::usize n = times.size();
    if (n == 0U) { return static_cast<T>(0); }
    const crd::usize span = static_cast<crd::usize>(key_elements(interp)) * components;

    // the value of key k (CubicHermite: the middle element of the [in, value, out] triple)
    const auto key_value = [&](crd::usize k) -> T {
        const crd::usize base = k * span;
        return interp == KeyInterp::CubicHermite ? values[base + components + lane] : values[base + lane];
    };

    if (n == 1U || t <= times[0]) { return key_value(t <= times[0] ? 0U : n - 1U); }
    if (t >= times[n - 1U]) { return key_value(n - 1U); }

    const crd::usize i = find_segment(times, t, cache);
    switch (interp)
    {
        case KeyInterp::Step: return key_value(i);
        case KeyInterp::Linear:
        {
            const T u = (t - times[i]) / (times[i + 1U] - times[i]);
            const T a = key_value(i);
            const T b = key_value(i + 1U);
            return a + u * (b - a);
        }
        case KeyInterp::CubicHermite:
        default:
        {
            // the bracketing pair through interp_hermite with the SPLIT tangents: d0 = out-tangent of key i,
            // d1 = in-tangent of key i+1 (glTF tangents ARE derivatives in value/second — the Hermite d's)
            const T x2[2] = {times[i], times[i + 1U]};
            const T y2[2] = {key_value(i), key_value(i + 1U)};
            const T d2[2] = {values[i * span + 2U * components + lane],          // out-tangent of key i
                             values[(i + 1U) * span + lane]};                    // in-tangent of key i+1
            crd::usize seg_cache = 0;
            return interp_hermite(crd::containers::ConstSpan<T>(x2, 2U), crd::containers::ConstSpan<T>(y2, 2U),
                                  crd::containers::ConstSpan<T>(d2, 2U), t, seg_cache);
        }
    }
}

// Track validation for build/cook time: strictly increasing finite times, the exact value count for the mode.
template <Real T>
[[nodiscard]] InterpStatus validate_track(crd::containers::ConstSpan<T> times,
                                          crd::containers::ConstSpan<T> values, crd::u32 components,
                                          KeyInterp interp) noexcept
{
    if (times.size() == 0U || components == 0U) { return InterpStatus::BadInput; }
    if (values.size()
        != times.size() * static_cast<crd::usize>(key_elements(interp)) * static_cast<crd::usize>(components))
    {
        return InterpStatus::BadInput;
    }
    for (crd::usize i = 0; i < times.size(); ++i)
    {
        if (!detail::is_finite(times[i])) { return InterpStatus::BadInput; }
        if (i > 0U && !(times[i] > times[i - 1U])) { return InterpStatus::NotIncreasing; }
    }
    for (crd::usize i = 0; i < values.size(); ++i)
    {
        if (!detail::is_finite(values[i])) { return InterpStatus::BadInput; }
    }
    return InterpStatus::Ok;
}

} // namespace crd::hesap::interp
