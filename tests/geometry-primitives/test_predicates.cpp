// crd-geometry-primitives v3a — Shewchuk 1997 adaptive-precision predicates.
//
// Coverage:
//
//   (1) Basic CCW/CW/collinear orient2d on simple inputs.
//   (2) Above/below/coplanar orient3d on simple inputs.
//   (3) Inside/outside/cocircular incircle.
//   (4) Inside/outside/cospherical insphere.
//   (5) Symmetry contract:
//        orient2d(a, b, c) = -orient2d(b, a, c)
//        orient3d(a, b, c, d) = -orient3d(b, a, c, d)
//        incircle(a, b, c, d) = -incircle(a, c, b, d) (swapping circle order
//                                                       reverses sign)
//   (6) NaN/Inf tolerance: non-finite input → returns 0.0.
//   (7) Determinism replay: two identical calls produce bit-exact results.
//   (8) Adversarial near-degenerate cases — the "Shewchuk torture corpus"
//        where naïve float predicates return the wrong sign.
//   (9) f32 wrapper agrees with f64 result for non-pathological inputs.
//   (10) Exact zero on truly degenerate input (geometrically collinear /
//        coplanar / cocircular / cospherical).

#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/vec.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
using crd::f32;
using crd::f64;
using crd::geometry::primitives::incircle;
using crd::geometry::primitives::insphere;
using crd::geometry::primitives::orient2d;
using crd::geometry::primitives::orient3d;
using crd::math::Vec2;
using crd::math::Vec3;
} // namespace

// ---------------------------------------------------------------------------
// (1) orient2d basics
// ---------------------------------------------------------------------------

TEST_CASE("orient2d: CCW triangle returns positive", "[v3a][predicates][orient2d]")
{
    const Vec2<f64> a(0, 0);
    const Vec2<f64> b(1, 0);
    const Vec2<f64> c(0, 1);
    CHECK(orient2d(a, b, c) > 0.0);
}

TEST_CASE("orient2d: CW triangle returns negative", "[v3a][predicates][orient2d]")
{
    const Vec2<f64> a(0, 0);
    const Vec2<f64> b(0, 1);
    const Vec2<f64> c(1, 0);
    CHECK(orient2d(a, b, c) < 0.0);
}

TEST_CASE("orient2d: collinear points return exactly zero", "[v3a][predicates][orient2d]")
{
    const Vec2<f64> a(0, 0);
    const Vec2<f64> b(1, 1);
    const Vec2<f64> c(2, 2);
    CHECK(orient2d(a, b, c) == 0.0);
}

TEST_CASE("orient2d: coincident points return exactly zero", "[v3a][predicates][orient2d]")
{
    const Vec2<f64> p(3.14, 2.71);
    CHECK(orient2d(p, p, p) == 0.0);
}

// ---------------------------------------------------------------------------
// (2) orient3d basics
// ---------------------------------------------------------------------------

TEST_CASE("orient3d: below-plane point returns positive (Shewchuk convention)",
          "[v3a][predicates][orient3d]")
{
    // CCW triangle on z=0: a=(0,0,0), b=(1,0,0), c=(0,1,0). Per Shewchuk's
    // convention: orient3d > 0 iff d is BELOW the plane (negative-z side here).
    const Vec3<f64> a(0, 0, 0);
    const Vec3<f64> b(1, 0, 0);
    const Vec3<f64> c(0, 1, 0);
    const Vec3<f64> d_below(0, 0, -1);
    CHECK(orient3d(a, b, c, d_below) > 0.0);
}

TEST_CASE("orient3d: above-plane point returns negative (Shewchuk convention)",
          "[v3a][predicates][orient3d]")
{
    const Vec3<f64> a(0, 0, 0);
    const Vec3<f64> b(1, 0, 0);
    const Vec3<f64> c(0, 1, 0);
    const Vec3<f64> d_above(0, 0, 1);
    CHECK(orient3d(a, b, c, d_above) < 0.0);
}

TEST_CASE("orient3d: coplanar input returns exactly zero", "[v3a][predicates][orient3d]")
{
    const Vec3<f64> a(0, 0, 0);
    const Vec3<f64> b(1, 0, 0);
    const Vec3<f64> c(0, 1, 0);
    const Vec3<f64> d_on_plane(0.5, 0.5, 0);
    CHECK(orient3d(a, b, c, d_on_plane) == 0.0);
}

TEST_CASE("orient3d: 4 coincident points return zero", "[v3a][predicates][orient3d]")
{
    const Vec3<f64> p(1, 2, 3);
    CHECK(orient3d(p, p, p, p) == 0.0);
}

// ---------------------------------------------------------------------------
// (3) incircle basics
// ---------------------------------------------------------------------------

TEST_CASE("incircle: point inside the unit circle returns positive", "[v3a][predicates][incircle]")
{
    // a, b, c on the unit circle in CCW order.
    const Vec2<f64> a(1, 0);
    const Vec2<f64> b(0, 1);
    const Vec2<f64> c(-1, 0);
    // d inside the circle (origin).
    const Vec2<f64> d(0, 0);
    CHECK(incircle(a, b, c, d) > 0.0);
}

TEST_CASE("incircle: point outside the unit circle returns negative", "[v3a][predicates][incircle]")
{
    const Vec2<f64> a(1, 0);
    const Vec2<f64> b(0, 1);
    const Vec2<f64> c(-1, 0);
    const Vec2<f64> d(2, 2); // far outside
    CHECK(incircle(a, b, c, d) < 0.0);
}

TEST_CASE("incircle: cocircular point returns exactly zero", "[v3a][predicates][incircle]")
{
    // a, b, c, d all on the unit circle.
    const Vec2<f64> a(1, 0);
    const Vec2<f64> b(0, 1);
    const Vec2<f64> c(-1, 0);
    const Vec2<f64> d(0, -1);
    CHECK(incircle(a, b, c, d) == 0.0);
}

// ---------------------------------------------------------------------------
// (4) insphere basics
// ---------------------------------------------------------------------------

TEST_CASE("insphere: point inside the unit sphere returns positive", "[v3a][predicates][insphere]")
{
    // a, b, c, d on the unit sphere, positively oriented tetrahedron.
    const Vec3<f64> a(1, 0, 0);
    const Vec3<f64> b(0, 1, 0);
    const Vec3<f64> c(0, 0, 1);
    const Vec3<f64> d(-1, 0, 0);
    const Vec3<f64> e(0, 0, 0); // origin is inside
    CHECK(insphere(a, b, c, d, e) > 0.0);
}

TEST_CASE("insphere: point outside the unit sphere returns negative", "[v3a][predicates][insphere]")
{
    const Vec3<f64> a(1, 0, 0);
    const Vec3<f64> b(0, 1, 0);
    const Vec3<f64> c(0, 0, 1);
    const Vec3<f64> d(-1, 0, 0);
    const Vec3<f64> e(2, 2, 2); // far outside
    CHECK(insphere(a, b, c, d, e) < 0.0);
}

TEST_CASE("insphere: cospherical point returns exactly zero", "[v3a][predicates][insphere]")
{
    // 5 points on the unit sphere — cospherical → exact zero.
    const Vec3<f64> a(1, 0, 0);
    const Vec3<f64> b(0, 1, 0);
    const Vec3<f64> c(0, 0, 1);
    const Vec3<f64> d(-1, 0, 0);
    const Vec3<f64> e(0, -1, 0);
    CHECK(insphere(a, b, c, d, e) == 0.0);
}

// ---------------------------------------------------------------------------
// (5) Symmetry / anti-symmetry contracts
// ---------------------------------------------------------------------------

TEST_CASE("orient2d: swapping arguments reverses sign", "[v3a][predicates][orient2d][symmetry]")
{
    const Vec2<f64> a(1.0, 2.0);
    const Vec2<f64> b(4.0, 5.0);
    const Vec2<f64> c(7.0, 11.0);
    const f64 forward = orient2d(a, b, c);
    const f64 swapped = orient2d(b, a, c);
    CHECK(forward == -swapped);
}

TEST_CASE("orient3d: swapping first two arguments reverses sign",
          "[v3a][predicates][orient3d][symmetry]")
{
    const Vec3<f64> a(1, 2, 3);
    const Vec3<f64> b(4, 5, 6);
    const Vec3<f64> c(7, 8, 10);
    const Vec3<f64> d(11, 13, 14);
    const f64 forward = orient3d(a, b, c, d);
    const f64 swapped = orient3d(b, a, c, d);
    CHECK(forward == -swapped);
}

// ---------------------------------------------------------------------------
// (6) NaN / Inf tolerance contract (ADR-0076 §15 queries-tolerate)
// ---------------------------------------------------------------------------

TEST_CASE("orient2d: NaN input returns zero", "[v3a][predicates][nan]")
{
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();
    const Vec2<f64> a(nan, 0.0);
    const Vec2<f64> b(1, 0);
    const Vec2<f64> c(0, 1);
    CHECK(orient2d(a, b, c) == 0.0);
}

TEST_CASE("orient3d: Inf input returns zero", "[v3a][predicates][nan]")
{
    const f64 inf = std::numeric_limits<f64>::infinity();
    const Vec3<f64> a(0, 0, 0);
    const Vec3<f64> b(1, 0, 0);
    const Vec3<f64> c(0, inf, 0);
    const Vec3<f64> d(0, 0, 1);
    CHECK(orient3d(a, b, c, d) == 0.0);
}

TEST_CASE("incircle: NaN input returns zero", "[v3a][predicates][nan]")
{
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();
    const Vec2<f64> a(1, 0);
    const Vec2<f64> b(0, 1);
    const Vec2<f64> c(-1, 0);
    const Vec2<f64> d(nan, 0);
    CHECK(incircle(a, b, c, d) == 0.0);
}

TEST_CASE("insphere: Inf input returns zero", "[v3a][predicates][nan]")
{
    const f64 inf = std::numeric_limits<f64>::infinity();
    const Vec3<f64> a(1, 0, 0);
    const Vec3<f64> b(0, 1, 0);
    const Vec3<f64> c(0, 0, 1);
    const Vec3<f64> d(-1, 0, 0);
    const Vec3<f64> e(inf, 0, 0);
    CHECK(insphere(a, b, c, d, e) == 0.0);
}

// ---------------------------------------------------------------------------
// (7) Determinism / replay equality
// ---------------------------------------------------------------------------

TEST_CASE("orient2d: replay produces bit-exact result", "[v3a][predicates][determinism]")
{
    const Vec2<f64> a(0.1, 0.2);
    const Vec2<f64> b(0.3, 0.4);
    const Vec2<f64> c(0.5, 0.6); // colinear-ish — triggers the adaptive path
    const f64 r1 = orient2d(a, b, c);
    const f64 r2 = orient2d(a, b, c);
    CHECK(std::memcmp(&r1, &r2, sizeof(f64)) == 0);
}

TEST_CASE("orient3d: replay produces bit-exact result", "[v3a][predicates][determinism]")
{
    const Vec3<f64> a(0.1, 0.2, 0.3);
    const Vec3<f64> b(0.4, 0.5, 0.6);
    const Vec3<f64> c(0.7, 0.8, 0.9);
    const Vec3<f64> d(1.0, 1.1, 1.2); // nearly coplanar
    const f64 r1 = orient3d(a, b, c, d);
    const f64 r2 = orient3d(a, b, c, d);
    CHECK(std::memcmp(&r1, &r2, sizeof(f64)) == 0);
}

// ---------------------------------------------------------------------------
// (8) Adversarial near-degenerate input — the Shewchuk torture corpus
// ---------------------------------------------------------------------------

TEST_CASE("orient2d: near-collinear input gives consistent sign (Shewchuk torture)",
          "[v3a][predicates][adversarial]")
{
    // Three points that are very nearly collinear but tilt slightly. The
    // naive 2x2 cross product can return the wrong sign due to catastrophic
    // cancellation; the adaptive predicate must return the correct sign.
    //
    // Pattern: take a baseline (0,0)-(1,0) line; place c at a tiny y-offset
    // that's near float epsilon at scale 1. Three configurations: c above,
    // c on the line, c below.

    const Vec2<f64> a(0.0, 0.0);
    const Vec2<f64> b(1.0, 0.0);

    // c above by a tiny epsilon → must report positive.
    const Vec2<f64> c_above(0.5, 1e-15);
    CHECK(orient2d(a, b, c_above) > 0.0);

    // c below by a tiny epsilon → must report negative.
    const Vec2<f64> c_below(0.5, -1e-15);
    CHECK(orient2d(a, b, c_below) < 0.0);

    // c exactly on the line → must report exact zero.
    const Vec2<f64> c_on(0.5, 0.0);
    CHECK(orient2d(a, b, c_on) == 0.0);
}

TEST_CASE("orient2d: large-coordinate near-collinear (Shewchuk torture, scale 1e6)",
          "[v3a][predicates][adversarial]")
{
    // At larger scale, ULP grows linearly. The adaptive path is the only way
    // to get the correct sign on a deliberately-tiny perpendicular offset.
    constexpr f64 kOrigin = 1.0e6;
    const Vec2<f64> a(kOrigin, kOrigin);
    const Vec2<f64> b(kOrigin + 1.0, kOrigin);
    const Vec2<f64> c_above(kOrigin + 0.5, kOrigin + 1e-9); // 1e-9 is well below 1e-6 ULP at scale
    const Vec2<f64> c_below(kOrigin + 0.5, kOrigin - 1e-9);
    CHECK(orient2d(a, b, c_above) > 0.0);
    CHECK(orient2d(a, b, c_below) < 0.0);
}

TEST_CASE("orient3d: near-coplanar input gives consistent sign (Shewchuk convention)",
          "[v3a][predicates][adversarial]")
{
    // Shewchuk convention: orient3d > 0 iff d is BELOW the plane.
    const Vec3<f64> a(0, 0, 0);
    const Vec3<f64> b(1, 0, 0);
    const Vec3<f64> c(0, 1, 0);
    const Vec3<f64> d_above(0.3, 0.3, 1e-15);
    const Vec3<f64> d_below(0.3, 0.3, -1e-15);
    const Vec3<f64> d_on(0.3, 0.3, 0.0);
    CHECK(orient3d(a, b, c, d_below) > 0.0); // below = positive (Shewchuk)
    CHECK(orient3d(a, b, c, d_above) < 0.0); // above = negative
    CHECK(orient3d(a, b, c, d_on) == 0.0);
}

// ---------------------------------------------------------------------------
// (9) f32 wrapper consistency with f64
// ---------------------------------------------------------------------------

TEST_CASE("orient2d: f32 overload matches f64 sign", "[v3a][predicates][f32]")
{
    const Vec2<f32> a32(0, 0);
    const Vec2<f32> b32(1, 0);
    const Vec2<f32> c32(0, 1);
    const f32 r32 = orient2d(a32, b32, c32);
    CHECK(r32 > 0.0F);

    // f32 on a clearly-CW triangle.
    const Vec2<f32> c32_cw(0, -1);
    CHECK(orient2d(a32, b32, c32_cw) < 0.0F);
}

TEST_CASE("orient3d: f32 overload matches f64 sign (Shewchuk convention)",
          "[v3a][predicates][f32]")
{
    const Vec3<f32> a32(0, 0, 0);
    const Vec3<f32> b32(1, 0, 0);
    const Vec3<f32> c32(0, 1, 0);
    // Shewchuk: below = positive, above = negative.
    const Vec3<f32> d_below_32(0, 0, -1);
    CHECK(orient3d(a32, b32, c32, d_below_32) > 0.0F);

    const Vec3<f32> d_above_32(0, 0, 1);
    CHECK(orient3d(a32, b32, c32, d_above_32) < 0.0F);
}

TEST_CASE("incircle: f32 overload matches f64 sign", "[v3a][predicates][f32]")
{
    const Vec2<f32> a32(1, 0);
    const Vec2<f32> b32(0, 1);
    const Vec2<f32> c32(-1, 0);
    const Vec2<f32> d_inside_32(0, 0);
    CHECK(incircle(a32, b32, c32, d_inside_32) > 0.0F);

    const Vec2<f32> d_outside_32(2, 2);
    CHECK(incircle(a32, b32, c32, d_outside_32) < 0.0F);
}

TEST_CASE("insphere: f32 overload matches f64 sign", "[v3a][predicates][f32]")
{
    const Vec3<f32> a32(1, 0, 0);
    const Vec3<f32> b32(0, 1, 0);
    const Vec3<f32> c32(0, 0, 1);
    const Vec3<f32> d32(-1, 0, 0);
    const Vec3<f32> e_inside_32(0, 0, 0);
    CHECK(insphere(a32, b32, c32, d32, e_inside_32) > 0.0F);

    const Vec3<f32> e_outside_32(2, 2, 2);
    CHECK(insphere(a32, b32, c32, d32, e_outside_32) < 0.0F);
}

// ---------------------------------------------------------------------------
// (10) Exact zero on geometric degeneracy
// ---------------------------------------------------------------------------

TEST_CASE("orient2d: exactly-collinear input across many configurations",
          "[v3a][predicates][exact-zero]")
{
    // A grid of collinear configurations: all on the line y = 2x + 1.
    for (int t1 = -5; t1 <= 5; ++t1)
    {
        for (int t2 = -5; t2 <= 5; ++t2)
        {
            for (int t3 = -5; t3 <= 5; ++t3)
            {
                if (t1 == t2 || t2 == t3 || t1 == t3)
                {
                    continue; // skip coincident
                }
                const Vec2<f64> a(static_cast<f64>(t1), 2.0 * t1 + 1.0);
                const Vec2<f64> b(static_cast<f64>(t2), 2.0 * t2 + 1.0);
                const Vec2<f64> c(static_cast<f64>(t3), 2.0 * t3 + 1.0);
                INFO("t1=" << t1 << " t2=" << t2 << " t3=" << t3);
                CHECK(orient2d(a, b, c) == 0.0);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Stage D coverage: adversarial inputs that drop through Stage A/B/C.
// (Added 2026-05-14 v3a-debt paydown.)
// ---------------------------------------------------------------------------

TEST_CASE("orient3d Stage D: 4 truly-coplanar points on slanted plane return exact zero",
          "[v3a][predicates][orient3d][stage-d]")
{
    // Plane: z = 2x + 3y + 1. All 4 points satisfy the equation exactly.
    // Stage A's f64 estimate may have ULP-level non-zero due to roundoff
    // chains in `adx * bdy - bdx * ady` etc. Stage B / C / D's job is to
    // produce exact zero on this geometrically-degenerate input.
    auto on_plane = [](f64 x, f64 y) {
        return Vec3<f64>(x, y, 2.0 * x + 3.0 * y + 1.0);
    };
    const Vec3<f64> a = on_plane(0, 0);
    const Vec3<f64> b = on_plane(1, 0);
    const Vec3<f64> c = on_plane(0, 1);
    const Vec3<f64> d = on_plane(2.5, 3.5);
    CHECK(orient3d(a, b, c, d) == 0.0);
    // Permutations should also be exact zero.
    CHECK(orient3d(b, c, d, a) == 0.0);
    CHECK(orient3d(d, a, b, c) == 0.0);
}

TEST_CASE("orient3d Stage D: tiny perpendicular perturbation at scale 100 resolves correctly",
          "[v3a][predicates][orient3d][stage-d]")
{
    // 4 points at scale 100, with d-z perturbed by ~1e-12 (above input-storage
    // ULP at scale 100 which is ~1e-14, but below the computation ULP for the
    // determinant of products which is ~1e-10 at this scale). Stage A's
    // noise floor masks the signal; Stage D's exact arithmetic resolves it.
    constexpr f64 kOrigin = 100.0;
    const Vec3<f64> a(kOrigin, kOrigin, kOrigin);
    const Vec3<f64> b(kOrigin + 1.0, kOrigin, kOrigin);
    const Vec3<f64> c(kOrigin, kOrigin + 1.0, kOrigin);
    const Vec3<f64> d_above(kOrigin + 0.5, kOrigin + 0.5, kOrigin + 1e-12);
    const Vec3<f64> d_below(kOrigin + 0.5, kOrigin + 0.5, kOrigin - 1e-12);
    CHECK(orient3d(a, b, c, d_below) > 0.0); // below = positive (Shewchuk)
    CHECK(orient3d(a, b, c, d_above) < 0.0);
}

TEST_CASE("incircle Stage D: 4 cocircular points on radius-1e3 circle return exact zero",
          "[v3a][predicates][incircle][stage-d]")
{
    // Generate 4 points at deterministic positions on a circle of radius 1e3.
    // Stage B may give a non-zero estimate due to roundoff in the lift terms
    // (x² + y² with large magnitudes); Stage D's exact expansion must return 0.
    constexpr f64 kRadius = 1.0e3;
    const Vec2<f64> a(kRadius, 0);
    const Vec2<f64> b(0, kRadius);
    const Vec2<f64> c(-kRadius, 0);
    const Vec2<f64> d(0, -kRadius);
    // All 4 satisfy x² + y² = kRadius² exactly (kRadius is exactly representable
    // in f64). Stage D must return exact zero.
    CHECK(incircle(a, b, c, d) == 0.0);
}

TEST_CASE("incircle Stage D: tiny perturbation outside circle returns negative",
          "[v3a][predicates][incircle][stage-d]")
{
    // Three points on unit circle (CCW), fourth point just barely outside.
    const Vec2<f64> a(1, 0);
    const Vec2<f64> b(0, 1);
    const Vec2<f64> c(-1, 0);
    // d at (0, -1 - 1e-13): well inside f64 ULP at scale 1 (~1e-16) but above
    // Stage A's noise floor at the typical permanent magnitude.
    const Vec2<f64> d(0, -(1.0 + 1e-13));
    CHECK(incircle(a, b, c, d) < 0.0);
}

TEST_CASE("orient3d: exactly-coplanar input on y = ax + bz + c plane",
          "[v3a][predicates][exact-zero]")
{
    // Plane y = 2x + 3z + 1.
    auto on_plane = [](f64 x, f64 z) {
        return Vec3<f64>(x, 2.0 * x + 3.0 * z + 1.0, z);
    };
    const Vec3<f64> a = on_plane(0, 0);
    const Vec3<f64> b = on_plane(1, 0);
    const Vec3<f64> c = on_plane(0, 1);
    const Vec3<f64> d = on_plane(2, 3);
    CHECK(orient3d(a, b, c, d) == 0.0);
    // Permutations should also yield exact zero (modulo sign, but here the
    // value is zero so sign doesn't matter).
    CHECK(orient3d(b, c, d, a) == 0.0);
    CHECK(orient3d(d, a, b, c) == 0.0);
}
