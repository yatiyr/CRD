// crd-geometry-convex v2a — `support()` overload tests.
//
// Verifies:
//   * Sphere: `center + radius·dir̂`; canonical fallback on zero `dir`;
//     vertex_idx is always `k_invalid_vertex`.
//   * OBB3: corner-picking by `dot(axis_i, dir)` sign; `+h` on a tie;
//     packed corner index in [0,8); vertex_idx is the corner.
//   * Capsule3: endpoint picked by `dot(endpoint, dir)`, tie → `a`; radial
//     adds `radius·dir̂`; vertex_idx ∈ {0, 1}.
//   * ConvexHullView: linear scan, strict-greater-wins ⇒ lowest-index
//     argmax on coincident extrema; vertex_idx is that index.

#include <crd/containers/array.hpp>
#include <crd/geometry/convex/support.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::f32;
using crd::u32;
using crd::geometry::convex::k_invalid_vertex;
using crd::geometry::convex::SupportPoint;
using crd::geometry::primitives::support;
using crd::geometry::primitives::Capsule3;
using crd::geometry::primitives::ConvexHullView;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::Sphere;
using crd::math::Mat3;
using crd::math::Vec3;

constexpr f32 kTol = 1e-5F;

bool approx(const Vec3<f32>& lhs, const Vec3<f32>& rhs, f32 tol = kTol)
{
    return std::fabs(lhs.x - rhs.x) <= tol && std::fabs(lhs.y - rhs.y) <= tol && std::fabs(lhs.z - rhs.z) <= tol;
}
} // namespace

TEST_CASE("Sphere support: center + radius dir-hat, canonical zero-dir fallback", "[geometry-convex][support][sphere]")
{
    const Sphere<f32> s(Vec3<f32>(1.0F, 2.0F, 3.0F), 2.0F);
    SECTION("axis-aligned direction")
    {
        const SupportPoint<f32> sp = support(s, Vec3<f32>(1.0F, 0.0F, 0.0F));
        REQUIRE(approx(sp.point, Vec3<f32>(3.0F, 2.0F, 3.0F)));
        REQUIRE(sp.vertex_idx == k_invalid_vertex);
    }
    SECTION("non-unit direction normalises")
    {
        const SupportPoint<f32> sp = support(s, Vec3<f32>(0.0F, 5.0F, 0.0F));
        REQUIRE(approx(sp.point, Vec3<f32>(1.0F, 4.0F, 3.0F)));
    }
    SECTION("zero direction → canonical +X fallback (deterministic)")
    {
        const SupportPoint<f32> sp1 = support(s, Vec3<f32>(0.0F, 0.0F, 0.0F));
        const SupportPoint<f32> sp2 = support(s, Vec3<f32>(0.0F, 0.0F, 0.0F));
        REQUIRE(approx(sp1.point, Vec3<f32>(3.0F, 2.0F, 3.0F)));
        REQUIRE(sp1.point.x == sp2.point.x); // bit-exact identical, not "approx"
        REQUIRE(sp1.point.y == sp2.point.y);
        REQUIRE(sp1.point.z == sp2.point.z);
    }
}

TEST_CASE("OBB3 support: corner picking, +h on tie, packed index", "[geometry-convex][support][obb]")
{
    // Axis-aligned unit cube centered at origin.
    const OBB3<f32> box(Vec3<f32>(0.0F), Vec3<f32>(1.0F, 1.0F, 1.0F), Mat3<f32>::identity());
    SECTION("all eight corners reachable by axis-aligned directions")
    {
        const Vec3<f32> dirs[8] = {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, -1}, {-1, 1, 1},
                                   {1, -1, -1},  {1, -1, 1},  {1, 1, -1},  {1, 1, 1}};
        for (u32 i = 0; i < 8; ++i)
        {
            const SupportPoint<f32> sp = support(box, dirs[i]);
            REQUIRE(approx(sp.point, dirs[i]));
            // Packed index: bit 2 ⇐ x≥0, bit 1 ⇐ y≥0, bit 0 ⇐ z≥0
            const u32 expected = (dirs[i].x < 0 ? 0U : 4U) | (dirs[i].y < 0 ? 0U : 2U) | (dirs[i].z < 0 ? 0U : 1U);
            REQUIRE(sp.vertex_idx == expected);
        }
    }
    SECTION("zero-projection on an axis picks +h (deterministic tie)")
    {
        // dir along +X with y=0, z=0: ties on y, z → both +h → corner (+1,+1,+1)
        const SupportPoint<f32> sp = support(box, Vec3<f32>(1.0F, 0.0F, 0.0F));
        REQUIRE(approx(sp.point, Vec3<f32>(1.0F, 1.0F, 1.0F)));
        REQUIRE(sp.vertex_idx == (4U | 2U | 1U));
    }
    SECTION("rotated OBB: support transports through orientation")
    {
        // 90° rotation about Z: local X axis = world Y, local Y axis = world -X.
        Mat3<f32> rot(Vec3<f32>(0, 1, 0), Vec3<f32>(-1, 0, 0), Vec3<f32>(0, 0, 1));
        const OBB3<f32> rbox(Vec3<f32>(0.0F), Vec3<f32>(2.0F, 1.0F, 1.0F), rot);
        // World +Y → strongest projection on local +X axis → +h.x on local X.
        const SupportPoint<f32> sp = support(rbox, Vec3<f32>(0.0F, 1.0F, 0.0F));
        // local (+2, ±h.y, ±h.z) → world (∓h.y, +2, ±h.z), ties pick +h ⇒
        // local (+2, +1, +1) → world (-1, +2, +1)
        REQUIRE(approx(sp.point, Vec3<f32>(-1.0F, 2.0F, 1.0F)));
    }
}

TEST_CASE("Capsule3 support: endpoint tiebreak to a, vertex_idx invalid (continuous radial)",
          "[geometry-convex][support][capsule]")
{
    // Capsule reports `k_invalid_vertex` — the radial component is a
    // continuous function of `dir`, so the support point is NOT a bijection
    // from a discrete vertex set. (The endpoint choice IS discrete, but
    // the full SupportPoint is not.) This makes GJK fall back to the
    // geometric termination, which fully converges the radial direction.
    const Capsule3<f32> cap(Vec3<f32>(0, 0, 0), Vec3<f32>(0, 0, 2), 0.5F);
    SECTION("dir along +Z picks b (point only — vertex_idx invalid)")
    {
        const SupportPoint<f32> sp = support(cap, Vec3<f32>(0, 0, 1));
        REQUIRE(approx(sp.point, Vec3<f32>(0, 0, 2.5F)));
        REQUIRE(sp.vertex_idx == k_invalid_vertex);
    }
    SECTION("dir along -Z picks a")
    {
        const SupportPoint<f32> sp = support(cap, Vec3<f32>(0, 0, -1));
        REQUIRE(approx(sp.point, Vec3<f32>(0, 0, -0.5F)));
        REQUIRE(sp.vertex_idx == k_invalid_vertex);
    }
    SECTION("dir orthogonal to axis: tiebreak to a (deterministic on point)")
    {
        // dot(a,X)=0, dot(b,X)=0 — tied → picks a; radial is +X·0.5.
        const SupportPoint<f32> sp = support(cap, Vec3<f32>(1, 0, 0));
        REQUIRE(approx(sp.point, Vec3<f32>(0.5F, 0, 0)));
        REQUIRE(sp.vertex_idx == k_invalid_vertex);
    }
    SECTION("zero direction → canonical fallback radial + endpoint a")
    {
        const SupportPoint<f32> sp = support(cap, Vec3<f32>(0, 0, 0));
        REQUIRE(approx(sp.point, Vec3<f32>(0.5F, 0, 0)));
        REQUIRE(sp.vertex_idx == k_invalid_vertex);
    }
}

TEST_CASE("ConvexHullView support: lowest-index argmax on coincident extrema", "[geometry-convex][support][hull]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 16, nullptr, "hull-test");
    crd::containers::Array<Vec3<f32>> verts(&alloc);
    crd::containers::Array<Plane<f32>> faces(&alloc);
    crd::containers::Array<u32> face_idx(&alloc);
    crd::containers::Array<u32> face_off(&alloc);

    // Two coincident extrema along +X — vertices 0 and 2 both have x=+1.
    verts.push_back(Vec3<f32>(1.0F, 0.0F, 0.0F));   // idx 0 — extremum +X
    verts.push_back(Vec3<f32>(-1.0F, 0.0F, 0.0F));  // idx 1
    verts.push_back(Vec3<f32>(1.0F, 0.5F, 0.0F));   // idx 2 — same x=+1 as idx 0
    verts.push_back(Vec3<f32>(0.0F, 0.0F, 1.0F));   // idx 3

    const ConvexHullView<f32> hull(crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size()),
                                   crd::containers::ConstSpan<Plane<f32>>(faces.data(), faces.size()),
                                   crd::containers::ConstSpan<u32>(face_idx.data(), face_idx.size()),
                                   crd::containers::ConstSpan<u32>(face_off.data(), face_off.size()));
    SECTION("strict-greater-wins ⇒ lowest-index argmax wins ties")
    {
        const SupportPoint<f32> sp = support(hull, Vec3<f32>(1.0F, 0.0F, 0.0F));
        REQUIRE(sp.vertex_idx == 0U);
        REQUIRE(approx(sp.point, Vec3<f32>(1.0F, 0.0F, 0.0F)));
    }
    SECTION("clear winner — no tiebreak needed")
    {
        const SupportPoint<f32> sp = support(hull, Vec3<f32>(0.0F, 0.0F, 1.0F));
        REQUIRE(sp.vertex_idx == 3U);
    }
    SECTION("strict-greater-wins on +Y picks vertex 2 (only +y extremum)")
    {
        const SupportPoint<f32> sp = support(hull, Vec3<f32>(0.0F, 1.0F, 0.0F));
        REQUIRE(sp.vertex_idx == 2U);
    }
}
