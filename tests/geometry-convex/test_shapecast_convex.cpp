// crd-geometry-convex v2f - GJK-based convex shapecast tests.
//
// Six claim categories:
//
//   (1) CLOSED-FORM SPHERE-VS-SPHERE: known TOI = (|offset| - sum_radii) /
//       (dot(sweep_dir, unit(offset))). The canonical "spheres approaching
//       head-on" case.
//
//   (2) CLOSED-FORM BOX-VS-BOX (axis-aligned): TOI when A's +X face
//       reaches B's -X face. Known geometry.
//
//   (3) OVERLAP-AT-START: A and B already overlapping at t=0. Returns
//       toi=0 with EPA's normal + witnesses.
//
//   (4) MOVING-AWAY / TANGENT: sweep_dir not approaching B → nullopt.
//       Initially-touching + tangent sweep → toi=0 (degenerate-converged).
//
//   (5) TMAX CUTOFF: sweep within [0, tmax] doesn't reach contact →
//       nullopt.
//
//   (6) BISECTION CROSS-CHECK: random separated pairs with random sweep
//       directions. Use `gjk_distance(...).distance_squared` to bisect
//       the TOI manually; verify shapecast_convex agrees within eps.
//
// Plus FACADE: `crd::geometry::cast_convex(...)` returns just the TOI and
// matches the rich `shapecast_convex` result.

#include <crd/containers/array.hpp>
#include <crd/geometry/convex/convex.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/transform.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <optional>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::cast_convex;
using crd::geometry::convex::gjk_distance;
using crd::geometry::convex::shapecast_convex;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Sphere;
using crd::math::Mat3;
using crd::math::Quat;
using crd::math::Transform;
using crd::math::Vec3;

Transform<f32> xform(const Vec3<f32>& t, const Quat<f32>& r = Quat<f32>::identity())
{
    return Transform<f32>(t, r);
}

bool approx(f32 l, f32 r, f32 tol = 1e-3F)
{
    return std::fabs(l - r) <= tol;
}
bool approx(const Vec3<f32>& l, const Vec3<f32>& r, f32 tol = 1e-3F)
{
    return std::fabs(l.x - r.x) <= tol && std::fabs(l.y - r.y) <= tol && std::fabs(l.z - r.z) <= tol;
}

struct Rng
{
    crd::u64 state;
    explicit Rng(crd::u64 seed) : state(seed) {}
    crd::u64 next()
    {
        crd::u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    f32 unit() { return static_cast<f32>(next() >> 40) / static_cast<f32>(1U << 24); }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
    Vec3<f32> rand_vec(f32 lo, f32 hi) { return Vec3<f32>(range(lo, hi), range(lo, hi), range(lo, hi)); }
};

// Bisection reference: find the FIRST t ∈ [0, tmax] where overlap begins.
// Uses linear sampling to detect the first separated-→-overlap transition
// (handles "A passes through B" where the pair is overlap-only-in-middle),
// then bisects the bracket interval to eps precision.
template <typename A, typename B>
[[nodiscard]] f32 bisect_toi(const A& a, const Transform<f32>& xform_a_start, const Vec3<f32>& sweep_dir, f32 tmax,
                              const B& b, const Transform<f32>& xform_b, f32 eps = 1e-4F)
{
    auto eval_overlap_at = [&](f32 t) {
        Transform<f32> xform_a(Vec3<f32>(xform_a_start.translation.x + sweep_dir.x * t,
                                          xform_a_start.translation.y + sweep_dir.y * t,
                                          xform_a_start.translation.z + sweep_dir.z * t),
                               xform_a_start.rotation);
        const auto gjk = gjk_distance<f32>(a, xform_a, b, xform_b);
        return gjk.overlapping || gjk.distance_squared < eps * eps;
    };
    // If already overlapping at t=0, TOI is 0.
    if (eval_overlap_at(0.0F))
    {
        return 0.0F;
    }
    // Linear sample to find the FIRST separated→overlap transition.
    // (Pure binary search of [0, tmax] would miss pass-through cases where
    // at tmax we're separated again.)
    constexpr int kSamples = 200;
    f32 prev_t = 0.0F;
    bool prev_overlap = false;
    for (int i = 1; i <= kSamples; ++i)
    {
        const f32 t = (tmax * static_cast<f32>(i)) / static_cast<f32>(kSamples);
        const bool now = eval_overlap_at(t);
        if (now && !prev_overlap)
        {
            // Found the transition. Bisect [prev_t, t].
            f32 lo = prev_t;
            f32 hi = t;
            for (int j = 0; j < 32; ++j)
            {
                const f32 mid = (lo + hi) * 0.5F;
                if (eval_overlap_at(mid))
                {
                    hi = mid;
                }
                else
                {
                    lo = mid;
                }
                if (hi - lo < eps)
                {
                    break;
                }
            }
            return lo;
        }
        prev_t = t;
        prev_overlap = now;
    }
    return -1.0F; // sentinel: no impact within [0, tmax]
}
} // namespace

// ===========================================================================
// CLOSED-FORM SPHERE-VS-SPHERE
// ===========================================================================

TEST_CASE("shapecast_convex: sphere-vs-sphere head-on TOI=3 (canonical)",
          "[shapecast][closed-form][sphere]")
{
    // A: unit sphere at origin moving +X with sweep_dir=(1,0,0).
    // B: unit sphere at (5, 0, 0), stationary.
    // Centers reach distance 2 (sum_radii) when A's center is at (3, 0, 0).
    // So TOI = 3.
    const Sphere<f32> a(Vec3<f32>(0), 1.0F);
    const Sphere<f32> b(Vec3<f32>(0), 1.0F);
    const auto r = shapecast_convex<f32>(a, xform(Vec3<f32>(0)), Vec3<f32>(1, 0, 0), 10.0F, b, xform(Vec3<f32>(5, 0, 0)));
    REQUIRE(r.has_value());
    REQUIRE(r->converged);
    INFO("toi=" << r->toi << ", normal=(" << r->normal.x << "," << r->normal.y << "," << r->normal.z
                << "), iter=" << static_cast<int>(r->iteration_count));
    REQUIRE(approx(r->toi, 3.0F));
    REQUIRE(approx(r->normal, Vec3<f32>(1, 0, 0), 5e-3F));
    REQUIRE(r->iteration_count <= 10); // Newton converges fast on smooth pairs
}

TEST_CASE("shapecast_convex: sphere-vs-sphere off-axis", "[shapecast][closed-form][sphere]")
{
    // A: radius 1 at origin moving (2, 0, 0).
    // B: radius 0.5 at (5, 4, 0), stationary.
    // |offset_initial| = sqrt(25+16) = sqrt(41) ~= 6.403.
    // sum_radii = 1.5.
    // For sphere-vs-sphere with directed velocity, the geometry is:
    // Solve |c_a(t) - c_b|^2 = sum_radii^2:
    //   c_a(t) = (2t, 0, 0). c_b = (5, 4, 0).
    //   (2t - 5)^2 + 16 = 2.25
    //   (2t - 5)^2 = -13.75 -> no real solution.
    // So they never touch with this offset. Use a closer B.
    // Try B at (5, 1, 0): (2t - 5)^2 + 1 = 2.25 -> (2t - 5)^2 = 1.25 -> 2t - 5 = -sqrt(1.25) -> t = (5 - 1.118)/2 = 1.941.
    const Sphere<f32> a(Vec3<f32>(0), 1.0F);
    const Sphere<f32> b(Vec3<f32>(0), 0.5F);
    const auto r = shapecast_convex<f32>(a, xform(Vec3<f32>(0)), Vec3<f32>(2, 0, 0), 10.0F, b, xform(Vec3<f32>(5, 1, 0)));
    REQUIRE(r.has_value());
    REQUIRE(r->converged);
    INFO("toi=" << r->toi);
    const f32 expected = (5.0F - std::sqrt(1.25F)) * 0.5F;
    REQUIRE(approx(r->toi, expected, 5e-3F));
}

// ===========================================================================
// CLOSED-FORM BOX-VS-BOX (axis-aligned)
// ===========================================================================

TEST_CASE("shapecast_convex: axis-aligned cube-vs-cube TOI=3 head-on +X",
          "[shapecast][closed-form][box]")
{
    // A: unit cube at origin moving +X. B: unit cube at (5, 0, 0).
    // A's +X face reaches B's -X face when A's center is at (3, 0, 0).
    // TOI = 3 with sweep_dir = (1, 0, 0).
    const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const auto r = shapecast_convex<f32>(a, xform(Vec3<f32>(0)), Vec3<f32>(1, 0, 0), 10.0F, b, xform(Vec3<f32>(5, 0, 0)));
    REQUIRE(r.has_value());
    REQUIRE(r->converged);
    INFO("toi=" << r->toi << ", iter=" << static_cast<int>(r->iteration_count));
    REQUIRE(approx(r->toi, 3.0F));
    REQUIRE(approx(r->normal, Vec3<f32>(1, 0, 0), 5e-3F));
}

// ===========================================================================
// OVERLAP-AT-START
// ===========================================================================

TEST_CASE("shapecast_convex: overlap at start returns toi=0 with EPA witnesses",
          "[shapecast][edge][overlap-at-start]")
{
    // Cubes interpenetrating at t=0. Should immediately report toi=0.
    const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const auto r = shapecast_convex<f32>(a, xform(Vec3<f32>(0)), Vec3<f32>(1, 0, 0), 10.0F, b, xform(Vec3<f32>(0.5F, 0, 0)));
    REQUIRE(r.has_value());
    REQUIRE(r->converged);
    REQUIRE(approx(r->toi, 0.0F));
    // Normal from EPA: A→B direction along +X.
    REQUIRE(r->normal.x > 0.95F);
}

// ===========================================================================
// MOVING-AWAY / NO-APPROACH
// ===========================================================================

TEST_CASE("shapecast_convex: A moving AWAY from B returns nullopt",
          "[shapecast][edge][no-approach]")
{
    // A at origin, B at (5, 0, 0). Sweep -X (away from B).
    const Sphere<f32> a(Vec3<f32>(0), 1.0F);
    const Sphere<f32> b(Vec3<f32>(0), 1.0F);
    const auto r = shapecast_convex<f32>(a, xform(Vec3<f32>(0)), Vec3<f32>(-1, 0, 0), 100.0F, b, xform(Vec3<f32>(5, 0, 0)));
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("shapecast_convex: tangent sweep (perpendicular) returns nullopt",
          "[shapecast][edge][tangent]")
{
    // A at origin, B at (5, 0, 0). Sweep +Y (perpendicular to A→B direction).
    const Sphere<f32> a(Vec3<f32>(0), 1.0F);
    const Sphere<f32> b(Vec3<f32>(0), 1.0F);
    const auto r = shapecast_convex<f32>(a, xform(Vec3<f32>(0)), Vec3<f32>(0, 1, 0), 100.0F, b, xform(Vec3<f32>(5, 0, 0)));
    // Sweep along +Y; A never moves toward B. No impact.
    REQUIRE_FALSE(r.has_value());
}

// ===========================================================================
// TMAX CUTOFF
// ===========================================================================

TEST_CASE("shapecast_convex: tmax cutoff", "[shapecast][edge][tmax]")
{
    // True TOI = 3. With tmax = 2, should return nullopt.
    const Sphere<f32> a(Vec3<f32>(0), 1.0F);
    const Sphere<f32> b(Vec3<f32>(0), 1.0F);
    REQUIRE_FALSE(
        shapecast_convex<f32>(a, xform(Vec3<f32>(0)), Vec3<f32>(1, 0, 0), 2.0F, b, xform(Vec3<f32>(5, 0, 0))).has_value());
    // With tmax = 4 (past true TOI), should return hit.
    REQUIRE(shapecast_convex<f32>(a, xform(Vec3<f32>(0)), Vec3<f32>(1, 0, 0), 4.0F, b, xform(Vec3<f32>(5, 0, 0)))
                .has_value());
}

// ===========================================================================
// BISECTION CROSS-CHECK (the strong correctness pin)
// ===========================================================================

TEST_CASE("shapecast_convex: agrees with bisection of GJK distance on random pairs",
          "[shapecast][bisection][random]")
{
    Rng rng(0xABCDEF42U);
    int verified = 0;
    int skipped = 0;
    int significant_disagreements = 0;
    for (int trial = 0; trial < 50; ++trial)
    {
        // Random spheres at random initial separation. Sweep direction
        // roughly toward B.
        const Sphere<f32> a(Vec3<f32>(0), rng.range(0.3F, 1.0F));
        const Sphere<f32> b(Vec3<f32>(0), rng.range(0.3F, 1.0F));
        const Vec3<f32> b_pos(rng.range(3.0F, 7.0F), rng.range(-2, 2), rng.range(-2, 2));
        const Vec3<f32> sweep_dir(rng.range(0.5F, 1.5F), rng.range(-0.3F, 0.3F), rng.range(-0.3F, 0.3F));
        const f32 tmax = 20.0F;

        const auto sc = shapecast_convex<f32>(a, xform(Vec3<f32>(0)), sweep_dir, tmax, b, xform(b_pos));
        const f32 bisect = bisect_toi(a, xform(Vec3<f32>(0)), sweep_dir, tmax, b, xform(b_pos));

        if (!sc.has_value() && bisect < 0)
        {
            ++skipped; // Both say no impact - consistent.
            continue;
        }
        if (!sc.has_value() || bisect < 0)
        {
            // One reports hit, the other doesn't - mismatch.
            INFO("trial " << trial << ", shapecast=" << (sc.has_value() ? std::to_string(sc->toi) : std::string("nullopt"))
                          << ", bisect=" << bisect);
            ++significant_disagreements;
            // For corner cases (bisection precision close to no-impact),
            // accept small mismatches. Hard fail only if both disagree
            // significantly.
            if (sc.has_value() && bisect < 0)
            {
                REQUIRE(sc->toi >= tmax - 0.01F); // shapecast says hit near tmax
            }
            continue;
        }

        INFO("trial " << trial << ", shapecast.toi=" << sc->toi << ", bisect_toi=" << bisect
                      << ", iter=" << static_cast<int>(sc->iteration_count));
        REQUIRE(sc->converged);
        // Tolerance: bisection has ~1e-4 precision; shapecast (Catto secant)
        // should be MORE accurate; allow 5e-3 absolute tolerance.
        REQUIRE(std::fabs(sc->toi - bisect) <= 5e-3F);
        ++verified;
    }
    INFO("verified=" << verified << " skipped(no-impact)=" << skipped << " significant_disagreements="
                    << significant_disagreements);
    REQUIRE(verified > 10); // a decent fraction of the random corpus has a real contact
    REQUIRE(significant_disagreements == 0);
}

// ===========================================================================
// FACADE
// ===========================================================================

TEST_CASE("Facade: crd::geometry::cast_convex returns TOI matching shapecast_convex",
          "[shapecast][facade]")
{
    const Sphere<f32> a(Vec3<f32>(0), 1.0F);
    const Sphere<f32> b(Vec3<f32>(0), 1.0F);
    const auto facade_toi = cast_convex<f32>(a, xform(Vec3<f32>(0)), Vec3<f32>(1, 0, 0), 10.0F, b, xform(Vec3<f32>(5, 0, 0)));
    const auto direct = shapecast_convex<f32>(a, xform(Vec3<f32>(0)), Vec3<f32>(1, 0, 0), 10.0F, b, xform(Vec3<f32>(5, 0, 0)));
    REQUIRE(facade_toi.has_value());
    REQUIRE(direct.has_value());
    REQUIRE(*facade_toi == direct->toi);

    // No-impact case: both return nullopt.
    REQUIRE_FALSE(cast_convex<f32>(a, xform(Vec3<f32>(0)), Vec3<f32>(-1, 0, 0), 10.0F, b, xform(Vec3<f32>(5, 0, 0)))
                      .has_value());
}

// ===========================================================================
// MIXED ANALYTIC + POLYHEDRAL
// ===========================================================================

TEST_CASE("shapecast_convex: sphere casting at OBB", "[shapecast][mixed]")
{
    // Sphere radius 0.5 moving +X toward axis-aligned cube at (5, 0, 0)
    // half-extent (1, 1, 1).
    // Sphere's leading point (in +X) at A's center + 0.5 at offset.
    // Cube's leading point (in -X) at center.x - 1 = 4.
    // So sphere center reaches x=3.5 (== 4 - 0.5) at TOI when sweep_dir=(1,0,0).
    // TOI = 3.5.
    const Sphere<f32> a(Vec3<f32>(0), 0.5F);
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const auto r = shapecast_convex<f32>(a, xform(Vec3<f32>(0)), Vec3<f32>(1, 0, 0), 10.0F, b, xform(Vec3<f32>(5, 0, 0)));
    REQUIRE(r.has_value());
    REQUIRE(r->converged);
    INFO("toi=" << r->toi);
    REQUIRE(approx(r->toi, 3.5F, 5e-3F));
    // Normal: the contact is at the sphere's leading point touching the
    // cube's -X face. Normal should be +X (A→B direction); but the
    // smooth-sphere/polyhedral-cube witness reconstruction can produce
    // a slight tilt due to GJK's discrete corner picking on the cube
    // side combined with the sphere's continuous surface. Within 5e-2
    // tolerance is acceptable for physics use.
    REQUIRE(approx(r->normal, Vec3<f32>(1, 0, 0), 5e-2F));
}
