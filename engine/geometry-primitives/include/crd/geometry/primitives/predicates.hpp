#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — Shewchuk 1997 adaptive-precision geometric
// predicates (Phase 3.1.7 v3a; ADR-0076 §18 amendment).
//
// Implementation of "Adaptive Precision Floating-Point Arithmetic and Fast
// Robust Geometric Predicates" (Shewchuk, Discrete & Computational Geometry,
// 18(3):305-363, 1997). Four predicates:
//
//   - **`orient2d(a, b, c)`** — sign of the 2x2 determinant
//        | b.x-a.x   c.x-a.x |
//        | b.y-a.y   c.y-a.y |
//     Positive: a, b, c are counter-clockwise. Negative: clockwise. Zero:
//     exactly collinear.
//
//   - **`orient3d(a, b, c, d)`** — sign of the 3x3 determinant of `[a-d, b-d,
//     c-d]` (Shewchuk 1997 convention; this is the *negative* of the more
//     common `det([b-a, c-a, d-a])` form, but matches the published Shewchuk
//     reference + every computational-geometry textbook that follows it).
//     **Positive:** d lies BELOW the plane through (a, b, c), where "below"
//     means on the negative side of the outward normal when (a, b, c) is viewed
//     CCW from above. **Negative:** d lies ABOVE. **Zero:** coplanar.
//     Equivalently: positive iff the tetrahedron (a, b, c, d) is positively
//     oriented in the Shewchuk sense.
//
//   - **`incircle(a, b, c, d)`** — given a, b, c on a circle in CCW order,
//     is d inside? Positive: inside. Negative: outside. Zero: cocircular.
//
//   - **`insphere(a, b, c, d, e)`** — given a, b, c, d a positively-
//     oriented tetrahedron on a sphere, is e inside? Positive: inside.
//     Negative: outside. Zero: cospherical.
//
// **The "adaptive precision" contract.** A naive floating-point evaluation
// of these determinants can return the wrong SIGN on inputs near degeneracy
// (collinear / coplanar / cocircular / cospherical). Shewchuk's scheme:
//
//   1. **Stage A** — compute the determinant in `f64`. Check whether
//      `|estimate| > permanent × ε_static`, where `permanent` is the sum of
//      absolute values of the intermediate products and `ε_static` is a
//      precomputed bound on the relative error. If yes: sign is correct;
//      return the estimate.
//   2. **Stage B** — recompute using Two-Sum / Two-Product to capture the
//      roundoff in each operation. Sum the corrections back into a more
//      accurate estimate. Check the dynamic error bound (now smaller
//      because most of the cancellation error has been corrected). If
//      reliable: return.
//   3. **Stage C** — fall through to fully exact computation using
//      "expansion arithmetic" (a sum of nonoverlapping doubles representing
//      the exact value). The expansion's leading nonzero term has the
//      correct sign of the true determinant.
//
// **The result.** The SIGN returned is always exactly correct. The MAGNITUDE
// is an approximation (the actual value has float error), but the sign-of-
// degenerate-input is 0.0 exactly when the true mathematical value is 0.
//
// **f32 inputs.** Promoted to `f64` internally and run through the `f64`
// adaptive path. Shewchuk's error bounds are derived for IEEE 754 binary64
// only; re-deriving for binary32 is its own paper. Promotion to f64 gives
// ~9 decimal digits of headroom over f32 input, which is more than enough
// for any non-mathematically-degenerate input.
//
// **NaN / Inf inputs.** Returns 0.0 (degenerate). Per ADR-0076 §15: queries
// tolerate (never UB), builders reject in debug. Predicates are queries.
// Callers that ARE builders (Quickhull, Bowyer-Watson) `CRD_ASSERT` on
// finite inputs at their entry points; predicates themselves stay tolerant.
//
// **Determinism (ADR-0063).** Bit-exact across compilers / SIMD widths /
// OSes. No transcendentals, no STL sort. IEEE 754 binary64 only. The
// adaptive scheme is purely arithmetic — same inputs always produce same
// expansion intermediates, same sign result.
//
// **Multi-domain consumers (locked by ADR-0076 §18):**
//   - v3b 2D convex hull (Andrew's monotone chain) — `orient2d` for "left
//     turn?" decisions.
//   - v3c 3D Quickhull (Barber-Dobkin-Huhdanpaa) — `orient3d` for face-side
//     tests.
//   - v6 polygon ops (Vatti / Bentley-Ottmann) — `orient2d` for segment
//     intersection sign tests.
//   - v8 Bowyer-Watson Delaunay (2D + 3D) — `incircle` / `insphere` for
//     Delaunay-flip tests.
//   - v9c V-HACD convex decomposition — `orient3d` for cluster membership.
//   - Phase 3.1.8 `crd-brep` exact boolean — `orient3d` + `insphere` for
//     B-rep face-side classification.
//   - Phase 3.1.10 `crd-cfd` AMR — `orient2d` / `orient3d` for cell
//     refinement decisions.
//   - Phase 3.1.12 `crd-fea` contact — `orient3d` for triangle/tetrahedron
//     containment.
//
// **Implementation split.** The fast (Stage A) path is inlined in this
// header (the common case). The Stage B / Stage C adaptive paths are in
// `engine/geometry-primitives/src/predicates.cpp` — large stack arrays for
// expansions; keep them out of every translation-unit's include footprint.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/math/vec.hpp>

#include <cmath> // std::fabs, std::fma — both IEEE-754 correctly-rounded, not banned by crd-no-std-math-check

namespace crd::geometry::primitives
{
using crd::math::Vec2;
using crd::math::Vec3;

namespace predicate_detail
{
// ----- IEEE 754 binary64 static error bounds (Shewchuk 1997 Section 3.2) ---
//
// `epsilon` = 2^-53 (machine epsilon for binary64).
// `splitter` = 2^27 + 1 (Veltkamp-Dekker split constant; only used when FMA
//   is unavailable — Cerid's predicates use std::fma which avoids the split).
// `resulterrbound` = 3 ε + 16 ε^2 — the relative error in the Stage-A f64
//   determinant evaluation. Used to validate whether Stage A's estimate is
//   reliable.
//
// Predicate-specific bounds are named per Shewchuk's paper:
//   `ccwerrbound{A,B,C}` for orient2d
//   `o3derrbound{A,B,C}` for orient3d
//   `iccerrbound{A,B,C}` for incircle
//   `isperrbound{A,B,C}` for insphere
//
// All constants computed at compile time from Cerid's fixed binary64 contract
// (ADR-0063 — no `exactinit()` runtime initialization needed; the original
// Shewchuk paper computed these at startup because some platforms had
// extended-precision FP registers, which Cerid explicitly disables via the
// /fp:precise + deterministic FP contract).

inline constexpr crd::f64 epsilon = 1.1102230246251565e-16; // 2^-53

// Stage-A relative-error bounds, per Shewchuk Table 1.
inline constexpr crd::f64 ccwerrbound_a = (3.0 + 16.0 * epsilon) * epsilon;
inline constexpr crd::f64 ccwerrbound_b = (2.0 + 12.0 * epsilon) * epsilon;
inline constexpr crd::f64 ccwerrbound_c = (9.0 + 64.0 * epsilon) * epsilon * epsilon;

inline constexpr crd::f64 o3derrbound_a = (7.0 + 56.0 * epsilon) * epsilon;
inline constexpr crd::f64 o3derrbound_b = (3.0 + 28.0 * epsilon) * epsilon;
inline constexpr crd::f64 o3derrbound_c = (26.0 + 288.0 * epsilon) * epsilon * epsilon;

inline constexpr crd::f64 iccerrbound_a = (10.0 + 96.0 * epsilon) * epsilon;
inline constexpr crd::f64 iccerrbound_b = (4.0 + 48.0 * epsilon) * epsilon;
inline constexpr crd::f64 iccerrbound_c = (44.0 + 576.0 * epsilon) * epsilon * epsilon;

inline constexpr crd::f64 isperrbound_a = (16.0 + 224.0 * epsilon) * epsilon;
inline constexpr crd::f64 isperrbound_b = (5.0 + 72.0 * epsilon) * epsilon;
inline constexpr crd::f64 isperrbound_c = (71.0 + 1408.0 * epsilon) * epsilon * epsilon;

// ----- Stage B / Stage C entry points (defined out-of-line) ---------------
//
// These compute the exact (or progressively-more-accurate) result and live in
// `predicates.cpp`. The header forwards to them only when the Stage-A path
// determines the estimate is unreliable.

[[nodiscard]] crd::f64 orient2d_adaptive(const Vec2<crd::f64>& a, const Vec2<crd::f64>& b,
                                          const Vec2<crd::f64>& c, crd::f64 detsum) noexcept;

[[nodiscard]] crd::f64 orient3d_adaptive(const Vec3<crd::f64>& a, const Vec3<crd::f64>& b,
                                          const Vec3<crd::f64>& c, const Vec3<crd::f64>& d,
                                          crd::f64 permanent) noexcept;

[[nodiscard]] crd::f64 incircle_adaptive(const Vec2<crd::f64>& a, const Vec2<crd::f64>& b,
                                          const Vec2<crd::f64>& c, const Vec2<crd::f64>& d,
                                          crd::f64 permanent) noexcept;

[[nodiscard]] crd::f64 insphere_adaptive(const Vec3<crd::f64>& a, const Vec3<crd::f64>& b,
                                          const Vec3<crd::f64>& c, const Vec3<crd::f64>& d,
                                          const Vec3<crd::f64>& e, crd::f64 permanent) noexcept;

// Internal NaN/Inf guard. Returns true if any component of any input is non-
// finite. Predicates return 0.0 in this case (queries-tolerate contract).
[[nodiscard]] inline bool any_nonfinite(const Vec2<crd::f64>& a, const Vec2<crd::f64>& b,
                                         const Vec2<crd::f64>& c) noexcept
{
    return !std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y) ||
           !std::isfinite(c.x) || !std::isfinite(c.y);
}

[[nodiscard]] inline bool any_nonfinite(const Vec3<crd::f64>& a, const Vec3<crd::f64>& b,
                                         const Vec3<crd::f64>& c, const Vec3<crd::f64>& d) noexcept
{
    return !std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z) || !std::isfinite(b.x) ||
           !std::isfinite(b.y) || !std::isfinite(b.z) || !std::isfinite(c.x) || !std::isfinite(c.y) ||
           !std::isfinite(c.z) || !std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z);
}

[[nodiscard]] inline bool any_nonfinite(const Vec2<crd::f64>& a, const Vec2<crd::f64>& b,
                                         const Vec2<crd::f64>& c, const Vec2<crd::f64>& d) noexcept
{
    return any_nonfinite(a, b, c) || !std::isfinite(d.x) || !std::isfinite(d.y);
}

[[nodiscard]] inline bool any_nonfinite(const Vec3<crd::f64>& a, const Vec3<crd::f64>& b,
                                         const Vec3<crd::f64>& c, const Vec3<crd::f64>& d,
                                         const Vec3<crd::f64>& e) noexcept
{
    return any_nonfinite(a, b, c, d) || !std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z);
}

} // namespace predicate_detail

// ===========================================================================
// PUBLIC API — orient2d
// ===========================================================================

// Sign of the 2x2 determinant det([b-a, c-a]). Adaptive precision; exact sign
// guaranteed. Returns 0.0 exactly when a, b, c are collinear; returns 0.0 on
// non-finite input (per ADR-0076 §15 queries-tolerate contract).
[[nodiscard]] inline crd::f64 orient2d(const Vec2<crd::f64>& a, const Vec2<crd::f64>& b,
                                        const Vec2<crd::f64>& c) noexcept
{
    if (predicate_detail::any_nonfinite(a, b, c))
    {
        return 0.0;
    }

    const crd::f64 detleft = (a.x - c.x) * (b.y - c.y);
    const crd::f64 detright = (a.y - c.y) * (b.x - c.x);
    const crd::f64 det = detleft - detright;

    crd::f64 detsum;
    if (detleft > 0.0)
    {
        if (detright <= 0.0)
        {
            return det; // signs differ: cancellation is impossible
        }
        else
        {
            detsum = detleft + detright;
        }
    }
    else if (detleft < 0.0)
    {
        if (detright >= 0.0)
        {
            return det;
        }
        else
        {
            detsum = -detleft - detright;
        }
    }
    else
    {
        return det; // detleft == 0: no cancellation
    }

    const crd::f64 errbound = predicate_detail::ccwerrbound_a * detsum;
    if (det >= errbound || -det >= errbound)
    {
        return det; // Stage A reliable
    }

    return predicate_detail::orient2d_adaptive(a, b, c, detsum);
}

// f32 overload — promotes to f64 + calls the robust f64 path.
[[nodiscard]] inline crd::f32 orient2d(const Vec2<crd::f32>& a, const Vec2<crd::f32>& b,
                                        const Vec2<crd::f32>& c) noexcept
{
    const Vec2<crd::f64> a64(static_cast<crd::f64>(a.x), static_cast<crd::f64>(a.y));
    const Vec2<crd::f64> b64(static_cast<crd::f64>(b.x), static_cast<crd::f64>(b.y));
    const Vec2<crd::f64> c64(static_cast<crd::f64>(c.x), static_cast<crd::f64>(c.y));
    return static_cast<crd::f32>(orient2d(a64, b64, c64));
}

// ===========================================================================
// PUBLIC API — orient3d
// ===========================================================================

// Sign of the 3x3 determinant det([b-a, c-a, d-a]). Adaptive precision; exact
// sign guaranteed. Returns 0.0 when a, b, c, d are coplanar.
[[nodiscard]] inline crd::f64 orient3d(const Vec3<crd::f64>& a, const Vec3<crd::f64>& b,
                                        const Vec3<crd::f64>& c, const Vec3<crd::f64>& d) noexcept
{
    if (predicate_detail::any_nonfinite(a, b, c, d))
    {
        return 0.0;
    }

    const crd::f64 adx = a.x - d.x;
    const crd::f64 bdx = b.x - d.x;
    const crd::f64 cdx = c.x - d.x;
    const crd::f64 ady = a.y - d.y;
    const crd::f64 bdy = b.y - d.y;
    const crd::f64 cdy = c.y - d.y;
    const crd::f64 adz = a.z - d.z;
    const crd::f64 bdz = b.z - d.z;
    const crd::f64 cdz = c.z - d.z;

    const crd::f64 bdxcdy = bdx * cdy;
    const crd::f64 cdxbdy = cdx * bdy;

    const crd::f64 cdxady = cdx * ady;
    const crd::f64 adxcdy = adx * cdy;

    const crd::f64 adxbdy = adx * bdy;
    const crd::f64 bdxady = bdx * ady;

    const crd::f64 det = adz * (bdxcdy - cdxbdy) + bdz * (cdxady - adxcdy) + cdz * (adxbdy - bdxady);

    const crd::f64 permanent =
        (std::fabs(bdxcdy) + std::fabs(cdxbdy)) * std::fabs(adz) +
        (std::fabs(cdxady) + std::fabs(adxcdy)) * std::fabs(bdz) +
        (std::fabs(adxbdy) + std::fabs(bdxady)) * std::fabs(cdz);

    const crd::f64 errbound = predicate_detail::o3derrbound_a * permanent;
    if (det > errbound || -det > errbound)
    {
        return det; // Stage A reliable
    }

    return predicate_detail::orient3d_adaptive(a, b, c, d, permanent);
}

// f32 overload.
[[nodiscard]] inline crd::f32 orient3d(const Vec3<crd::f32>& a, const Vec3<crd::f32>& b,
                                        const Vec3<crd::f32>& c, const Vec3<crd::f32>& d) noexcept
{
    const Vec3<crd::f64> a64(static_cast<crd::f64>(a.x), static_cast<crd::f64>(a.y), static_cast<crd::f64>(a.z));
    const Vec3<crd::f64> b64(static_cast<crd::f64>(b.x), static_cast<crd::f64>(b.y), static_cast<crd::f64>(b.z));
    const Vec3<crd::f64> c64(static_cast<crd::f64>(c.x), static_cast<crd::f64>(c.y), static_cast<crd::f64>(c.z));
    const Vec3<crd::f64> d64(static_cast<crd::f64>(d.x), static_cast<crd::f64>(d.y), static_cast<crd::f64>(d.z));
    return static_cast<crd::f32>(orient3d(a64, b64, c64, d64));
}

// ===========================================================================
// PUBLIC API — incircle
// ===========================================================================

// Sign of `|d|² × det(B - dM) - rows`-style 4x4 expansion. Positive when d is
// inside the circle through a, b, c (a, b, c in CCW order). Exact sign on
// cocircular input.
[[nodiscard]] inline crd::f64 incircle(const Vec2<crd::f64>& a, const Vec2<crd::f64>& b,
                                        const Vec2<crd::f64>& c, const Vec2<crd::f64>& d) noexcept
{
    if (predicate_detail::any_nonfinite(a, b, c, d))
    {
        return 0.0;
    }

    const crd::f64 adx = a.x - d.x;
    const crd::f64 bdx = b.x - d.x;
    const crd::f64 cdx = c.x - d.x;
    const crd::f64 ady = a.y - d.y;
    const crd::f64 bdy = b.y - d.y;
    const crd::f64 cdy = c.y - d.y;

    const crd::f64 bdxcdy = bdx * cdy;
    const crd::f64 cdxbdy = cdx * bdy;
    const crd::f64 alift = adx * adx + ady * ady;

    const crd::f64 cdxady = cdx * ady;
    const crd::f64 adxcdy = adx * cdy;
    const crd::f64 blift = bdx * bdx + bdy * bdy;

    const crd::f64 adxbdy = adx * bdy;
    const crd::f64 bdxady = bdx * ady;
    const crd::f64 clift = cdx * cdx + cdy * cdy;

    const crd::f64 det = alift * (bdxcdy - cdxbdy) + blift * (cdxady - adxcdy) + clift * (adxbdy - bdxady);

    const crd::f64 permanent = (std::fabs(bdxcdy) + std::fabs(cdxbdy)) * alift +
                                (std::fabs(cdxady) + std::fabs(adxcdy)) * blift +
                                (std::fabs(adxbdy) + std::fabs(bdxady)) * clift;

    const crd::f64 errbound = predicate_detail::iccerrbound_a * permanent;
    if (det > errbound || -det > errbound)
    {
        return det;
    }

    return predicate_detail::incircle_adaptive(a, b, c, d, permanent);
}

// f32 overload.
[[nodiscard]] inline crd::f32 incircle(const Vec2<crd::f32>& a, const Vec2<crd::f32>& b,
                                        const Vec2<crd::f32>& c, const Vec2<crd::f32>& d) noexcept
{
    const Vec2<crd::f64> a64(static_cast<crd::f64>(a.x), static_cast<crd::f64>(a.y));
    const Vec2<crd::f64> b64(static_cast<crd::f64>(b.x), static_cast<crd::f64>(b.y));
    const Vec2<crd::f64> c64(static_cast<crd::f64>(c.x), static_cast<crd::f64>(c.y));
    const Vec2<crd::f64> d64(static_cast<crd::f64>(d.x), static_cast<crd::f64>(d.y));
    return static_cast<crd::f32>(incircle(a64, b64, c64, d64));
}

// ===========================================================================
// PUBLIC API — insphere
// ===========================================================================

// Sign of the 5x5 determinant lifting (Shewchuk §4.4). Positive when e is
// inside the sphere through a, b, c, d (a, b, c, d positively oriented).
[[nodiscard]] inline crd::f64 insphere(const Vec3<crd::f64>& a, const Vec3<crd::f64>& b,
                                        const Vec3<crd::f64>& c, const Vec3<crd::f64>& d,
                                        const Vec3<crd::f64>& e) noexcept
{
    if (predicate_detail::any_nonfinite(a, b, c, d, e))
    {
        return 0.0;
    }

    const crd::f64 aex = a.x - e.x;
    const crd::f64 bex = b.x - e.x;
    const crd::f64 cex = c.x - e.x;
    const crd::f64 dex = d.x - e.x;
    const crd::f64 aey = a.y - e.y;
    const crd::f64 bey = b.y - e.y;
    const crd::f64 cey = c.y - e.y;
    const crd::f64 dey = d.y - e.y;
    const crd::f64 aez = a.z - e.z;
    const crd::f64 bez = b.z - e.z;
    const crd::f64 cez = c.z - e.z;
    const crd::f64 dez = d.z - e.z;

    const crd::f64 aexbey = aex * bey;
    const crd::f64 bexaey = bex * aey;
    const crd::f64 ab = aexbey - bexaey;
    const crd::f64 bexcey = bex * cey;
    const crd::f64 cexbey = cex * bey;
    const crd::f64 bc = bexcey - cexbey;
    const crd::f64 cexdey = cex * dey;
    const crd::f64 dexcey = dex * cey;
    const crd::f64 cd = cexdey - dexcey;
    const crd::f64 dexaey = dex * aey;
    const crd::f64 aexdey = aex * dey;
    const crd::f64 da = dexaey - aexdey;
    const crd::f64 aexcey = aex * cey;
    const crd::f64 cexaey = cex * aey;
    const crd::f64 ac = aexcey - cexaey;
    const crd::f64 bexdey = bex * dey;
    const crd::f64 dexbey = dex * bey;
    const crd::f64 bd = bexdey - dexbey;

    const crd::f64 abc = aez * bc - bez * ac + cez * ab;
    const crd::f64 bcd = bez * cd - cez * bd + dez * bc;
    const crd::f64 cda = cez * da + dez * ac + aez * cd;
    const crd::f64 dab = dez * ab + aez * bd + bez * da;

    const crd::f64 alift = aex * aex + aey * aey + aez * aez;
    const crd::f64 blift = bex * bex + bey * bey + bez * bez;
    const crd::f64 clift = cex * cex + cey * cey + cez * cez;
    const crd::f64 dlift = dex * dex + dey * dey + dez * dez;

    const crd::f64 det = (dlift * abc - clift * dab) + (blift * cda - alift * bcd);

    const crd::f64 aezplus = std::fabs(aez);
    const crd::f64 bezplus = std::fabs(bez);
    const crd::f64 cezplus = std::fabs(cez);
    const crd::f64 dezplus = std::fabs(dez);
    const crd::f64 aexbeyplus = std::fabs(aexbey);
    const crd::f64 bexaeyplus = std::fabs(bexaey);
    const crd::f64 bexceyplus = std::fabs(bexcey);
    const crd::f64 cexbeyplus = std::fabs(cexbey);
    const crd::f64 cexdeyplus = std::fabs(cexdey);
    const crd::f64 dexceyplus = std::fabs(dexcey);
    const crd::f64 dexaeyplus = std::fabs(dexaey);
    const crd::f64 aexdeyplus = std::fabs(aexdey);
    const crd::f64 aexceyplus = std::fabs(aexcey);
    const crd::f64 cexaeyplus = std::fabs(cexaey);
    const crd::f64 bexdeyplus = std::fabs(bexdey);
    const crd::f64 dexbeyplus = std::fabs(dexbey);

    const crd::f64 permanent =
        ((cexdeyplus + dexceyplus) * bezplus + (dexbeyplus + bexdeyplus) * cezplus +
         (bexceyplus + cexbeyplus) * dezplus) *
            alift +
        ((dexaeyplus + aexdeyplus) * cezplus + (aexceyplus + cexaeyplus) * dezplus +
         (cexdeyplus + dexceyplus) * aezplus) *
            blift +
        ((aexbeyplus + bexaeyplus) * dezplus + (bexdeyplus + dexbeyplus) * aezplus +
         (dexaeyplus + aexdeyplus) * bezplus) *
            clift +
        ((bexceyplus + cexbeyplus) * aezplus + (cexaeyplus + aexceyplus) * bezplus +
         (aexbeyplus + bexaeyplus) * cezplus) *
            dlift;

    const crd::f64 errbound = predicate_detail::isperrbound_a * permanent;
    if (det > errbound || -det > errbound)
    {
        return det;
    }

    return predicate_detail::insphere_adaptive(a, b, c, d, e, permanent);
}

// f32 overload.
[[nodiscard]] inline crd::f32 insphere(const Vec3<crd::f32>& a, const Vec3<crd::f32>& b,
                                        const Vec3<crd::f32>& c, const Vec3<crd::f32>& d,
                                        const Vec3<crd::f32>& e) noexcept
{
    const Vec3<crd::f64> a64(static_cast<crd::f64>(a.x), static_cast<crd::f64>(a.y), static_cast<crd::f64>(a.z));
    const Vec3<crd::f64> b64(static_cast<crd::f64>(b.x), static_cast<crd::f64>(b.y), static_cast<crd::f64>(b.z));
    const Vec3<crd::f64> c64(static_cast<crd::f64>(c.x), static_cast<crd::f64>(c.y), static_cast<crd::f64>(c.z));
    const Vec3<crd::f64> d64(static_cast<crd::f64>(d.x), static_cast<crd::f64>(d.y), static_cast<crd::f64>(d.z));
    const Vec3<crd::f64> e64(static_cast<crd::f64>(e.x), static_cast<crd::f64>(e.y), static_cast<crd::f64>(e.z));
    return static_cast<crd::f32>(insphere(a64, b64, c64, d64, e64));
}

} // namespace crd::geometry::primitives
