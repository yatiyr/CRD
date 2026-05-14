// crd-geometry-convex v2-close - degenerate corpus.
//
// The substrate must NOT UB on pathological inputs (zero-radius spheres,
// zero-extent OBBs, degenerate capsules, tiny/empty hulls, tangent contacts,
// far-origin inputs). The promise is "valid-or-conservatively-degenerate result,
// never crash, never NaN-propagate" — same shape as ADR-0076 §15's "queries
// tolerate, builders reject" contract.
//
// Coverage:
//
//   (A) Zero-radius spheres + zero-extent OBBs + zero-length capsules.
//   (B) Hulls with 1/2/3/4 vertices (below the polyhedral threshold).
//   (C) All-coplanar hull (zero volume — every face_vertex sequence
//       degenerates to a 2D outline).
//   (D) Identical-pose pairs (A == B at same xform — Minkowski diff = the
//       point/origin; GJK overlap; EPA must converge or report
//       !converged without UB).
//   (E) Tangent contact (distance exactly zero) — face-face, face-vertex,
//       edge-edge. Acceptable: `gjk_overlap` may return true OR false
//       depending on pair-kind per v2b's touching convention; the
//       guarantee is no crash + replay-deterministic.
//   (F) Far-origin (1e6, 1e7) f32 inputs — sanity that GJK still produces
//       a result (correctness past 1 Mm is v2i's f64 territory).
//   (G) Near-zero separation (1e-7) — GJK should report distance² ~ 1e-14
//       (or overlap; either is acceptable per v2b touching convention).

#include <crd/containers/array.hpp>
#include <crd/geometry/convex/convex.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/transform.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;

using crd::geometry::convex::compute_contact;
using crd::geometry::convex::gjk_distance;
using crd::geometry::convex::gjk_overlap;
using crd::geometry::convex::PointShape;
using crd::geometry::convex::sat_obb_obb;

using crd::geometry::primitives::Capsule3;
using crd::geometry::primitives::ConvexHullView;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::Sphere;
using crd::geometry::primitives::support;

using crd::math::Mat3;
using crd::math::Quat;
using crd::math::Transform;
using crd::math::Vec3;
} // namespace

// ---------------------------------------------------------------------------
// (A) Zero-radius / zero-extent / zero-length degenerate primitives.
// ---------------------------------------------------------------------------

TEST_CASE("degen: zero-radius sphere vs unit sphere", "[v2-close][degen][sphere]")
{
    const Sphere<f32> zero(Vec3<f32>(0), 0.0F); // point-like sphere
    const Sphere<f32> unit(Vec3<f32>(0), 1.0F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());

    // Separated: zero sphere 5 units away.
    {
        const Transform<f32> xb(Vec3<f32>(5, 0, 0), Quat<f32>::identity());
        const auto r = gjk_distance<f32>(zero, xa, unit, xb);
        CHECK(!r.overlapping);
        CHECK(r.distance_squared > 0.0F);
        // Replay determinism.
        const auto r2 = gjk_distance<f32>(zero, xa, unit, xb);
        CHECK(std::fabs(r.distance_squared - r2.distance_squared) < 1e-6F);
    }
    // Inside: zero sphere at origin → overlapping.
    {
        const Transform<f32> xb(Vec3<f32>(0, 0, 0), Quat<f32>::identity());
        const auto r = gjk_overlap<f32>(zero, xa, unit, xb);
        CHECK(r);
    }
}

TEST_CASE("degen: zero-extent OBB (one axis = 0) vs unit OBB", "[v2-close][degen][obb]")
{
    // Half-extent (0, 1, 1) — the OBB is a 0x2x2 quad in the YZ plane.
    const OBB3<f32> flat(Vec3<f32>(0), Vec3<f32>(0, 1, 1), Mat3<f32>::identity());
    const OBB3<f32> unit(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());

    // Separated.
    {
        const Transform<f32> xb(Vec3<f32>(5, 0, 0), Quat<f32>::identity());
        const auto r = gjk_distance<f32>(flat, xa, unit, xb);
        CHECK(!r.overlapping);
        CHECK(r.distance_squared > 0.0F);
    }
    // Overlapping.
    {
        const Transform<f32> xb(Vec3<f32>(0.5F, 0, 0), Quat<f32>::identity());
        const auto r = gjk_overlap<f32>(flat, xa, unit, xb);
        CHECK(r);
    }
}

TEST_CASE("degen: all-zero OBB (point-like) vs sphere", "[v2-close][degen][obb]")
{
    const OBB3<f32> point(Vec3<f32>(0), Vec3<f32>(0, 0, 0), Mat3<f32>::identity());
    const Sphere<f32> sph(Vec3<f32>(0), 1.0F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());

    // Outside sphere.
    {
        const Transform<f32> xb(Vec3<f32>(5, 0, 0), Quat<f32>::identity());
        const auto r = gjk_distance<f32>(point, xa, sph, xb);
        CHECK(!r.overlapping);
    }
    // Inside sphere.
    {
        const Transform<f32> xb(Vec3<f32>(0.5F, 0, 0), Quat<f32>::identity());
        const auto r = gjk_overlap<f32>(point, xa, sph, xb);
        CHECK(r);
    }
}

TEST_CASE("degen: capsule a==b is a sphere", "[v2-close][degen][capsule]")
{
    const Capsule3<f32> point_cap(Vec3<f32>(0), Vec3<f32>(0), 0.5F); // sphere of radius 0.5
    const Capsule3<f32> normal_cap(Vec3<f32>(0, -1, 0), Vec3<f32>(0, 1, 0), 0.5F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());

    // Separated.
    {
        const Transform<f32> xb(Vec3<f32>(5, 0, 0), Quat<f32>::identity());
        const auto r = gjk_distance<f32>(point_cap, xa, normal_cap, xb);
        CHECK(!r.overlapping);
    }
    // Overlapping (point-cap inside the spine of normal_cap).
    {
        const Transform<f32> xb(Vec3<f32>(0, 0, 0), Quat<f32>::identity());
        const auto r = gjk_overlap<f32>(point_cap, xa, normal_cap, xb);
        CHECK(r);
    }
}

TEST_CASE("degen: zero-radius zero-length capsule (point)", "[v2-close][degen][capsule]")
{
    const Capsule3<f32> point(Vec3<f32>(0), Vec3<f32>(0), 0.0F); // a pure point
    const Sphere<f32> sph(Vec3<f32>(0), 1.0F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    {
        const Transform<f32> xb(Vec3<f32>(5, 0, 0), Quat<f32>::identity());
        const auto r = gjk_distance<f32>(point, xa, sph, xb);
        CHECK(!r.overlapping);
    }
    {
        const Transform<f32> xb(Vec3<f32>(0.5F, 0, 0), Quat<f32>::identity());
        const auto r = gjk_overlap<f32>(point, xa, sph, xb);
        CHECK(r);
    }
}

// ---------------------------------------------------------------------------
// (B) Tiny hulls (1/2/3/4 vertices).
// ---------------------------------------------------------------------------

TEST_CASE("degen: hull with 1 vertex acts as a point", "[v2-close][degen][hull]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<Vec3<f32>> verts(&alloc);
    verts.push_back(Vec3<f32>(0, 0, 0));
    ConvexHullView<f32> hull;
    hull.vertices = crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size());

    const Sphere<f32> sph(Vec3<f32>(0), 1.0F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(0.5F, 0, 0), Quat<f32>::identity());

    const auto r = gjk_overlap<f32>(hull, xa, sph, xb);
    CHECK(r);
}

TEST_CASE("degen: hull with 2 vertices acts as a segment", "[v2-close][degen][hull]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<Vec3<f32>> verts(&alloc);
    verts.push_back(Vec3<f32>(-1, 0, 0));
    verts.push_back(Vec3<f32>(1, 0, 0));
    ConvexHullView<f32> hull;
    hull.vertices = crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size());

    const Sphere<f32> sph(Vec3<f32>(0), 0.5F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(0, 0.3F, 0), Quat<f32>::identity());

    const auto r = gjk_overlap<f32>(hull, xa, sph, xb);
    CHECK(r);
}

TEST_CASE("degen: hull with 3 vertices (triangle)", "[v2-close][degen][hull]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<Vec3<f32>> verts(&alloc);
    verts.push_back(Vec3<f32>(-1, 0, 0));
    verts.push_back(Vec3<f32>(1, 0, 0));
    verts.push_back(Vec3<f32>(0, 1, 0));
    ConvexHullView<f32> hull;
    hull.vertices = crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size());

    const Sphere<f32> sph(Vec3<f32>(0), 0.3F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(0, 0.3F, 0), Quat<f32>::identity());

    const auto r = gjk_overlap<f32>(hull, xa, sph, xb);
    CHECK(r);
}

TEST_CASE("degen: hull with 4 vertices (tetra)", "[v2-close][degen][hull]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<Vec3<f32>> verts(&alloc);
    verts.push_back(Vec3<f32>(0, 0, 0));
    verts.push_back(Vec3<f32>(1, 0, 0));
    verts.push_back(Vec3<f32>(0, 1, 0));
    verts.push_back(Vec3<f32>(0, 0, 1));
    ConvexHullView<f32> hull;
    hull.vertices = crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size());

    const Sphere<f32> sph(Vec3<f32>(0), 0.2F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(0.3F, 0.3F, 0.3F), Quat<f32>::identity());

    const auto r = gjk_overlap<f32>(hull, xa, sph, xb);
    CHECK(r);
}

// ---------------------------------------------------------------------------
// (C) All-coplanar hull (zero volume).
// ---------------------------------------------------------------------------

TEST_CASE("degen: all-coplanar hull treated as a flat polygon", "[v2-close][degen][hull]")
{
    // 5 vertices in the z=0 plane.
    crd::memory::TlsfAllocator alloc(8U * 1024U);
    crd::containers::Array<Vec3<f32>> verts(&alloc);
    verts.push_back(Vec3<f32>(-1, -1, 0));
    verts.push_back(Vec3<f32>(1, -1, 0));
    verts.push_back(Vec3<f32>(1.5F, 0.5F, 0));
    verts.push_back(Vec3<f32>(0, 1.5F, 0));
    verts.push_back(Vec3<f32>(-1.2F, 0.8F, 0));
    ConvexHullView<f32> hull;
    hull.vertices = crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size());

    const Sphere<f32> sph(Vec3<f32>(0), 0.5F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());

    // Above the polygon.
    {
        const Transform<f32> xb(Vec3<f32>(0, 0, 5), Quat<f32>::identity());
        CHECK(!gjk_overlap<f32>(hull, xa, sph, xb));
    }
    // Touching the polygon plane from above.
    {
        const Transform<f32> xb(Vec3<f32>(0, 0, 0.3F), Quat<f32>::identity());
        CHECK(gjk_overlap<f32>(hull, xa, sph, xb));
    }
}

// ---------------------------------------------------------------------------
// (D) Identical-pose pairs.
// ---------------------------------------------------------------------------

TEST_CASE("degen: identical OBBs at identical pose are overlapping", "[v2-close][degen][identical]")
{
    const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const Transform<f32> x(Vec3<f32>(0), Quat<f32>::identity());

    CHECK(gjk_overlap<f32>(a, x, b, x));
    // SAT should report overlap with depth = 2 (full overlap along any axis
    // — A and B occupy the same volume).
    const auto r = sat_obb_obb<f32>(a, x, b, x);
    CHECK(r.overlapping);
    // EPA / compute_contact must converge or report !converged — must not
    // crash, must not return NaN.
    const auto c = compute_contact<f32>(a, x, b, x);
    if (c.has_value())
    {
        CHECK(!std::isnan(c->depth));
        CHECK(!std::isnan(c->normal.x));
    }
}

TEST_CASE("degen: identical spheres at identical pose", "[v2-close][degen][identical]")
{
    const Sphere<f32> s(Vec3<f32>(0), 1.0F);
    const Transform<f32> x(Vec3<f32>(0), Quat<f32>::identity());
    CHECK(gjk_overlap<f32>(s, x, s, x));
    const auto c = compute_contact<f32>(s, x, s, x);
    if (c.has_value())
    {
        CHECK(!std::isnan(c->depth));
    }
}

// ---------------------------------------------------------------------------
// (E) Tangent contact (distance exactly zero).
// ---------------------------------------------------------------------------

TEST_CASE("degen: tangent spheres distance exactly zero", "[v2-close][degen][tangent]")
{
    // Two unit spheres touching at exactly one point (centers 2 apart).
    const Sphere<f32> a(Vec3<f32>(0), 1.0F);
    const Sphere<f32> b(Vec3<f32>(0), 1.0F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(2, 0, 0), Quat<f32>::identity());
    const auto r = gjk_distance<f32>(a, xa, b, xb);
    // Per v2b touching convention: analytic-vs-analytic at exact contact
    // reports overlap = true (the f32 closes the gap to exactly zero).
    // Either answer is acceptable — what we forbid is NaN / crash.
    CHECK(!std::isnan(r.distance_squared));
    CHECK(r.distance_squared >= 0.0F);
}

TEST_CASE("degen: tangent OBBs face-face contact", "[v2-close][degen][tangent]")
{
    const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    // Centers 2 apart → touching face-to-face.
    const Transform<f32> xb(Vec3<f32>(2, 0, 0), Quat<f32>::identity());

    // SAT should report overlap = true (polyhedral-vs-polyhedral at exact
    // contact). Depth ~ 0.
    const auto r = sat_obb_obb<f32>(a, xa, b, xb);
    CHECK(!std::isnan(r.depth));
    if (r.overlapping)
    {
        CHECK(r.depth >= 0.0F);
        CHECK(r.depth < 1e-3F); // tangent contact ⇒ near-zero depth
    }
}

// ---------------------------------------------------------------------------
// (F) Far-origin inputs.
// ---------------------------------------------------------------------------

TEST_CASE("degen: GJK at 1e6 origin still produces a valid result (f32)",
          "[v2-close][degen][far]")
{
    // 1 Mm — at the edge of f32 sub-mm precision. We don't claim accuracy,
    // only no UB.
    const f32 R = 1.0e6F;
    const Sphere<f32> a(Vec3<f32>(0), 1.0F);
    const Sphere<f32> b(Vec3<f32>(0), 1.0F);
    const Transform<f32> xa(Vec3<f32>(R, 0, 0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(R + 3.0F, 0, 0), Quat<f32>::identity());
    const auto r = gjk_distance<f32>(a, xa, b, xb);
    CHECK(!std::isnan(r.distance_squared));
    CHECK(r.distance_squared >= 0.0F);
}

TEST_CASE("degen: GJK at 1e7 origin still produces a valid result (f64)",
          "[v2-close][degen][far][f64]")
{
    // 10 Mm — well outside f32 sub-meter precision; f64 holds.
    const f64 R = 1.0e7;
    const Sphere<f64> a(Vec3<f64>(0), 1.0);
    const Sphere<f64> b(Vec3<f64>(0), 1.0);
    const Transform<f64> xa(Vec3<f64>(R, 0, 0), Quat<f64>::identity());
    const Transform<f64> xb(Vec3<f64>(R + 3.0, 0, 0), Quat<f64>::identity());
    const auto r = gjk_distance<f64>(a, xa, b, xb);
    CHECK(!std::isnan(r.distance_squared));
    CHECK(r.distance_squared >= 0.0);
    // f64 keeps nm precision at 10 Mm — expect ~ 1.0 (distance 1 between
    // surfaces, distance² ~ 1.0).
    CHECK(std::fabs(r.distance_squared - 1.0) < 1e-3);
}

// ---------------------------------------------------------------------------
// (G) Near-zero separation (1e-7).
// ---------------------------------------------------------------------------

TEST_CASE("degen: GJK near-zero separation (1e-7) no NaN", "[v2-close][degen][near-zero]")
{
    const Sphere<f32> a(Vec3<f32>(0), 1.0F);
    const Sphere<f32> b(Vec3<f32>(0), 1.0F);
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(2.0F + 1.0e-7F, 0, 0), Quat<f32>::identity());
    const auto r = gjk_distance<f32>(a, xa, b, xb);
    CHECK(!std::isnan(r.distance_squared));
    CHECK(r.distance_squared >= 0.0F);
    // Either overlapping (if f32 closes the gap) or distance² ~ 1e-14
    // (if f32 preserves it). Both acceptable; no NaN/crash is the contract.
}
