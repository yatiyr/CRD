// crd-geometry-convex v2j - Sutherland-Hodgman polygon clipping + face/edge
// feature enumeration.
//
// Coverage:
//
//   (1) clip_convex_polygon: closed-form clip of a unit-square against a
//       plane crossing through the middle; degenerate cases (empty,
//       all-inside, all-outside, vertex-on-plane); coincident-edge case
//       where two consecutive vertices sit exactly on the clipping plane.
//
//   (2) clip_against_convex_volume: triangle clipped to an OBB-as-6-planes
//       volume; empty-result short-circuit when first plane culls everything;
//       no-planes pass-through.
//
//   (3) enumerate_faces(OBB3): 6 features, correct outward normals + CCW-
//       from-outside vertex ordering; cross-check the index pattern against
//       test_hill_climb.cpp's CubeHullWithAdjacency convention.
//
//   (4) enumerate_edges_obb: 12 edges with v0 < v1; every (v0, v1) pair
//       corresponds to corners differing in exactly 1 bit; the two
//       face_a/face_b indices match the two SHARED-sign axes.
//
//   (5) enumerate_spine(Capsule3): returns Segment3{a, b}.
//
//   (6) enumerate_faces(ConvexHullView): copies the cube hull's face data
//       through verbatim; vertex_indices spans line up with face_vertex_
//       offsets prefix-sum.
//
//   (7) enumerate_edges(ConvexHullView): symmetry contract — every (v0, v1)
//       appears as v0→v1 in face_a and v1→v0 in face_b.
//
//   (8) closest_face_index: ties-go-to-lowest-index; basic +X / -X / +Y...
//       discrimination on aligned directions.
//
//   (9) is_smooth: Sphere/Capsule = true; OBB/ConvexHullView = false.
//
//   (10) f64 instantiation: clip + enumerate work at f64 precision (the
//        substrate is templated; this pin guards against accidental f32-
//        only assumptions per v2i policy).

#include <crd/containers/array.hpp>
#include <crd/geometry/convex/convex.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

namespace
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::u8;
using crd::usize;

using crd::geometry::convex::clip_against_convex_volume;
using crd::geometry::convex::clip_convex_polygon;
using crd::geometry::convex::closest_face_index;
using crd::geometry::convex::EdgeFeature;
using crd::geometry::convex::enumerate_edges;
using crd::geometry::convex::enumerate_edges_obb;
using crd::geometry::convex::enumerate_faces;
using crd::geometry::convex::enumerate_spine;
using crd::geometry::convex::HullFaceFeature;
using crd::geometry::convex::is_smooth;

using crd::geometry::primitives::Capsule3;
using crd::geometry::primitives::ConvexHullView;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::Segment3;
using crd::geometry::primitives::Sphere;

using crd::math::Mat3;
using crd::math::Vec3;

template <typename T> bool approx(T l, T r, T tol = static_cast<T>(1e-5))
{
    return std::fabs(l - r) <= tol;
}

template <typename T> bool approx_vec(const Vec3<T>& a, const Vec3<T>& b, T tol = static_cast<T>(1e-5))
{
    return approx(a.x, b.x, tol) && approx(a.y, b.y, tol) && approx(a.z, b.z, tol);
}

} // namespace

// ---------------------------------------------------------------------------
// (1) clip_convex_polygon: closed-form + degenerate cases
// ---------------------------------------------------------------------------

TEST_CASE("clip_convex_polygon unit-square clipped by x=0 plane", "[v2j][clip]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f32>> input(&alloc);
    crd::containers::Array<Vec3<f32>> output(&alloc);

    // Unit square in the XY plane, CCW from +Z, centered at origin.
    input.push_back(Vec3<f32>(-1, -1, 0));
    input.push_back(Vec3<f32>(1, -1, 0));
    input.push_back(Vec3<f32>(1, 1, 0));
    input.push_back(Vec3<f32>(-1, 1, 0));

    // Clip by plane x=0 with normal=(+1,0,0), d=0. Half-space x <= 0 = inside.
    const Plane<f32> clip(Vec3<f32>(1, 0, 0), 0);
    clip_convex_polygon<f32>(
        crd::containers::ConstSpan<Vec3<f32>>(input.data(), input.size()), clip, output);

    REQUIRE(output.size() == 4U);
    // Expected: a rectangle with x in [-1, 0]. The walked output starts at
    // the intersection of edge (1,1,0)->(-1,1,0) with the plane (which sits
    // at (0, 1, 0)) — but actually with the algorithm walking from prev=
    // last_vertex (-1,1,0), curr=(-1,-1,0):
    //   prev=(-1,1,0)  sd=-1 inside;  curr=(-1,-1,0) sd=-1 inside  -> emit curr
    //   prev=(-1,-1,0) sd=-1 inside;  curr=(1,-1,0)  sd=+1 outside -> emit (0,-1,0)
    //   prev=(1,-1,0)  sd=+1 outside; curr=(1,1,0)   sd=+1 outside -> nothing
    //   prev=(1,1,0)   sd=+1 outside; curr=(-1,1,0)  sd=-1 inside  -> emit (0,1,0) + curr
    // So output = [(-1,-1,0), (0,-1,0), (0,1,0), (-1,1,0)] — the clipped rectangle.
    CHECK(approx_vec(output[0], Vec3<f32>(-1, -1, 0)));
    CHECK(approx_vec(output[1], Vec3<f32>(0, -1, 0)));
    CHECK(approx_vec(output[2], Vec3<f32>(0, 1, 0)));
    CHECK(approx_vec(output[3], Vec3<f32>(-1, 1, 0)));
}

TEST_CASE("clip_convex_polygon all-inside emits input verbatim", "[v2j][clip]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U);
    crd::containers::Array<Vec3<f32>> input(&alloc);
    crd::containers::Array<Vec3<f32>> output(&alloc);

    input.push_back(Vec3<f32>(-1, -1, 0));
    input.push_back(Vec3<f32>(1, -1, 0));
    input.push_back(Vec3<f32>(0, 1, 0));

    // Plane far away — everything is inside.
    const Plane<f32> clip(Vec3<f32>(1, 0, 0), -100);
    clip_convex_polygon<f32>(
        crd::containers::ConstSpan<Vec3<f32>>(input.data(), input.size()), clip, output);

    REQUIRE(output.size() == 3U);
    CHECK(approx_vec(output[0], input[0]));
    CHECK(approx_vec(output[1], input[1]));
    CHECK(approx_vec(output[2], input[2]));
}

TEST_CASE("clip_convex_polygon all-outside emits empty", "[v2j][clip]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U);
    crd::containers::Array<Vec3<f32>> input(&alloc);
    crd::containers::Array<Vec3<f32>> output(&alloc);

    input.push_back(Vec3<f32>(-1, -1, 0));
    input.push_back(Vec3<f32>(1, -1, 0));
    input.push_back(Vec3<f32>(0, 1, 0));

    // Plane with all vertices on the OUT side. inside = x + 100 <= 0 = x <= -100.
    const Plane<f32> clip(Vec3<f32>(1, 0, 0), 100);
    clip_convex_polygon<f32>(
        crd::containers::ConstSpan<Vec3<f32>>(input.data(), input.size()), clip, output);

    CHECK(output.empty());
}

TEST_CASE("clip_convex_polygon empty input emits empty", "[v2j][clip]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U);
    crd::containers::Array<Vec3<f32>> output(&alloc);
    const Plane<f32> clip(Vec3<f32>(1, 0, 0), 0);
    clip_convex_polygon<f32>(crd::containers::ConstSpan<Vec3<f32>>{}, clip, output);
    CHECK(output.empty());
}

TEST_CASE("clip_convex_polygon vertex exactly on plane stays inside", "[v2j][clip]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U);
    crd::containers::Array<Vec3<f32>> input(&alloc);
    crd::containers::Array<Vec3<f32>> output(&alloc);

    input.push_back(Vec3<f32>(0, -1, 0));   // on plane x=0
    input.push_back(Vec3<f32>(-1, 0, 0));   // inside
    input.push_back(Vec3<f32>(0, 1, 0));    // on plane x=0
    input.push_back(Vec3<f32>(-1, -1, 0));  // inside

    const Plane<f32> clip(Vec3<f32>(1, 0, 0), 0);
    clip_convex_polygon<f32>(
        crd::containers::ConstSpan<Vec3<f32>>(input.data(), input.size()), clip, output);

    // All vertices have sd <= 0 (on-plane treated as inside), so output ==
    // input verbatim.
    REQUIRE(output.size() == 4U);
    for (usize i = 0; i < input.size(); ++i)
    {
        CHECK(approx_vec(output[i], input[i]));
    }
}

TEST_CASE("clip_convex_polygon coincident edge on plane", "[v2j][clip]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U);
    crd::containers::Array<Vec3<f32>> input(&alloc);
    crd::containers::Array<Vec3<f32>> output(&alloc);

    // A pentagon with one edge lying exactly along x=0.
    input.push_back(Vec3<f32>(-2, -1, 0));
    input.push_back(Vec3<f32>(0, -1, 0));   // on plane
    input.push_back(Vec3<f32>(0, 1, 0));    // on plane
    input.push_back(Vec3<f32>(-1, 2, 0));
    input.push_back(Vec3<f32>(-3, 0, 0));

    const Plane<f32> clip(Vec3<f32>(1, 0, 0), 0);
    clip_convex_polygon<f32>(
        crd::containers::ConstSpan<Vec3<f32>>(input.data(), input.size()), clip, output);

    // All 5 input vertices have sd <= 0 (left of x=0). Output should be the
    // full input polygon, no spurious intersection vertices.
    REQUIRE(output.size() == 5U);
    for (usize i = 0; i < input.size(); ++i)
    {
        CHECK(approx_vec(output[i], input[i]));
    }
}

// ---------------------------------------------------------------------------
// (2) clip_against_convex_volume
// ---------------------------------------------------------------------------

TEST_CASE("clip_against_convex_volume triangle into unit cube", "[v2j][clip]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f32>> input(&alloc);
    crd::containers::Array<Vec3<f32>> output(&alloc);
    crd::containers::Array<Vec3<f32>> scratch(&alloc);

    // Big triangle in the z=0 plane, surrounds the cube.
    input.push_back(Vec3<f32>(-5, -5, 0));
    input.push_back(Vec3<f32>(5, -5, 0));
    input.push_back(Vec3<f32>(0, 5, 0));

    // Unit cube at origin: 6 outward planes (dot(n,x)+d <= 0 is inside).
    crd::containers::Array<Plane<f32>> planes(&alloc);
    planes.push_back(Plane<f32>(Vec3<f32>(1, 0, 0), -1));    // x <= 1
    planes.push_back(Plane<f32>(Vec3<f32>(-1, 0, 0), -1));   // x >= -1
    planes.push_back(Plane<f32>(Vec3<f32>(0, 1, 0), -1));    // y <= 1
    planes.push_back(Plane<f32>(Vec3<f32>(0, -1, 0), -1));   // y >= -1
    // z planes don't constrain (triangle is in z=0 plane, both sides).
    planes.push_back(Plane<f32>(Vec3<f32>(0, 0, 1), -1));
    planes.push_back(Plane<f32>(Vec3<f32>(0, 0, -1), -1));

    clip_against_convex_volume<f32>(
        crd::containers::ConstSpan<Vec3<f32>>(input.data(), input.size()),
        crd::containers::ConstSpan<Plane<f32>>(planes.data(), planes.size()), output, scratch);

    // Expected: the triangle clipped to the unit-square (z=0 cross section
    // of the cube) — a polygon strictly inside [-1,1]x[-1,1]x{0}.
    REQUIRE(output.size() >= 3U);
    for (usize i = 0; i < output.size(); ++i)
    {
        CHECK(output[i].x >= -1.0F - 1e-5F);
        CHECK(output[i].x <= 1.0F + 1e-5F);
        CHECK(output[i].y >= -1.0F - 1e-5F);
        CHECK(output[i].y <= 1.0F + 1e-5F);
        CHECK(std::fabs(output[i].z) <= 1e-5F);
    }
}

TEST_CASE("clip_against_convex_volume empty short-circuits", "[v2j][clip]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U);
    crd::containers::Array<Vec3<f32>> input(&alloc);
    crd::containers::Array<Vec3<f32>> output(&alloc);
    crd::containers::Array<Vec3<f32>> scratch(&alloc);

    input.push_back(Vec3<f32>(100, 100, 100));
    input.push_back(Vec3<f32>(101, 100, 100));
    input.push_back(Vec3<f32>(100, 101, 100));

    // First plane culls everything.
    crd::containers::Array<Plane<f32>> planes(&alloc);
    planes.push_back(Plane<f32>(Vec3<f32>(1, 0, 0), -1));    // x <= 1 — all input outside

    clip_against_convex_volume<f32>(
        crd::containers::ConstSpan<Vec3<f32>>(input.data(), input.size()),
        crd::containers::ConstSpan<Plane<f32>>(planes.data(), planes.size()), output, scratch);

    CHECK(output.empty());
}

TEST_CASE("clip_against_convex_volume no planes passes through", "[v2j][clip]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U);
    crd::containers::Array<Vec3<f32>> input(&alloc);
    crd::containers::Array<Vec3<f32>> output(&alloc);
    crd::containers::Array<Vec3<f32>> scratch(&alloc);

    input.push_back(Vec3<f32>(1, 2, 3));
    input.push_back(Vec3<f32>(4, 5, 6));
    input.push_back(Vec3<f32>(7, 8, 9));

    clip_against_convex_volume<f32>(
        crd::containers::ConstSpan<Vec3<f32>>(input.data(), input.size()),
        crd::containers::ConstSpan<Plane<f32>>{}, output, scratch);

    REQUIRE(output.size() == 3U);
    for (usize i = 0; i < input.size(); ++i)
    {
        CHECK(approx_vec(output[i], input[i]));
    }
}

// ---------------------------------------------------------------------------
// (3) enumerate_faces(OBB3)
// ---------------------------------------------------------------------------

TEST_CASE("enumerate_faces(OBB3) axis-aligned unit cube", "[v2j][feature][obb]")
{
    OBB3<f32> cube(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    auto faces = enumerate_faces(cube);

    REQUIRE(faces.size() == 6U);

    // Face 0 = +X: normal = +X axis, vertices = corners 4,5,7,6.
    CHECK(faces[0].face_index == 0U);
    CHECK(approx_vec(faces[0].plane.normal, Vec3<f32>(1, 0, 0)));
    CHECK(approx(faces[0].plane.d, -1.0F));
    // Corner 4 = (+1, -1, -1).
    CHECK(approx_vec(faces[0].vertices[0], Vec3<f32>(1, -1, -1)));
    CHECK(approx_vec(faces[0].vertices[1], Vec3<f32>(1, -1, 1)));
    CHECK(approx_vec(faces[0].vertices[2], Vec3<f32>(1, 1, 1)));
    CHECK(approx_vec(faces[0].vertices[3], Vec3<f32>(1, 1, -1)));

    // Face 1 = -X.
    CHECK(approx_vec(faces[1].plane.normal, Vec3<f32>(-1, 0, 0)));
    CHECK(approx(faces[1].plane.d, -1.0F));

    // Face 2 = +Y.
    CHECK(approx_vec(faces[2].plane.normal, Vec3<f32>(0, 1, 0)));
    CHECK(approx(faces[2].plane.d, -1.0F));

    // Every face vertex lies on its own plane (dot(n,v)+d == 0).
    for (u8 f = 0; f < 6U; ++f)
    {
        for (u8 k = 0; k < 4U; ++k)
        {
            const f32 sd =
                crd::math::dot(faces[f].plane.normal, faces[f].vertices[k]) + faces[f].plane.d;
            CHECK(approx(sd, 0.0F, 1e-5F));
        }
    }
}

TEST_CASE("enumerate_faces(OBB3) translated + rotated", "[v2j][feature][obb]")
{
    // 90° rotation about Y axis: +X local -> +Z world, +Z local -> -X world.
    Mat3<f32> rot;
    rot.c0 = Vec3<f32>(0, 0, -1); // local +X in world coords
    rot.c1 = Vec3<f32>(0, 1, 0);
    rot.c2 = Vec3<f32>(1, 0, 0);

    OBB3<f32> obb(Vec3<f32>(5, 0, 0), Vec3<f32>(2, 1, 1), rot);
    auto faces = enumerate_faces(obb);

    // Face 0 = +X local. Outward normal = rot.c0 = (0, 0, -1).
    CHECK(approx_vec(faces[0].plane.normal, Vec3<f32>(0, 0, -1)));
    // Face center is at obb.center + h_x * rot.c0 = (5,0,0) + 2*(0,0,-1) = (5,0,-2).
    // Every face vertex should satisfy dot(normal, v) + d == 0.
    for (u8 k = 0; k < 4U; ++k)
    {
        const f32 sd = crd::math::dot(faces[0].plane.normal, faces[0].vertices[k]) + faces[0].plane.d;
        CHECK(approx(sd, 0.0F, 1e-5F));
    }
}

// ---------------------------------------------------------------------------
// (4) enumerate_edges_obb
// ---------------------------------------------------------------------------

TEST_CASE("enumerate_edges_obb returns 12 deterministic edges", "[v2j][feature][obb]")
{
    auto edges = enumerate_edges_obb();
    REQUIRE(edges.size() == 12U);

    // Every edge has v0 < v1.
    for (u8 i = 0; i < 12U; ++i)
    {
        CHECK(edges[i].v0 < edges[i].v1);
        CHECK(edges[i].face_a < 6U);
        CHECK(edges[i].face_b < 6U);
        CHECK(edges[i].face_a != edges[i].face_b);
    }

    // Edge endpoints differ in exactly 1 bit (cube-corner index).
    for (u8 i = 0; i < 12U; ++i)
    {
        const u32 diff = edges[i].v0 ^ edges[i].v1;
        // popcount == 1 (one bit difference).
        CHECK((diff != 0U && (diff & (diff - 1U)) == 0U));
    }

    // Determinism: ordering is fixed (axis-X edges first, then Y, then Z).
    // First 4 edges should be axis-X (vary bit 2): diff == 4.
    for (u8 i = 0; i < 4U; ++i)
    {
        CHECK((edges[i].v0 ^ edges[i].v1) == 4U);
    }
    // Next 4: axis-Y, diff == 2.
    for (u8 i = 4; i < 8U; ++i)
    {
        CHECK((edges[i].v0 ^ edges[i].v1) == 2U);
    }
    // Last 4: axis-Z, diff == 1.
    for (u8 i = 8; i < 12U; ++i)
    {
        CHECK((edges[i].v0 ^ edges[i].v1) == 1U);
    }
}

// ---------------------------------------------------------------------------
// (5) enumerate_spine(Capsule3)
// ---------------------------------------------------------------------------

TEST_CASE("enumerate_spine returns segment a->b", "[v2j][feature][capsule]")
{
    Capsule3<f32> cap(Vec3<f32>(1, 2, 3), Vec3<f32>(4, 5, 6), 0.5F);
    Segment3<f32> spine = enumerate_spine(cap);
    CHECK(approx_vec(spine.a, Vec3<f32>(1, 2, 3)));
    CHECK(approx_vec(spine.b, Vec3<f32>(4, 5, 6)));
}

// ---------------------------------------------------------------------------
// (6, 7) ConvexHullView feature enumeration
// ---------------------------------------------------------------------------

namespace
{
// Hand-built unit cube hull (matching test_hill_climb.cpp's convention).
struct CubeHull
{
    crd::containers::Array<Vec3<f32>> verts;
    crd::containers::Array<Plane<f32>> faces;
    crd::containers::Array<u32> face_vertex_indices;
    crd::containers::Array<u32> face_vertex_offsets;

    explicit CubeHull(crd::memory::IAllocator* alloc)
        : verts(alloc), faces(alloc), face_vertex_indices(alloc), face_vertex_offsets(alloc)
    {
        for (int i = 0; i < 8; ++i)
        {
            verts.push_back(Vec3<f32>((i & 4) ? 1.0F : -1.0F, (i & 2) ? 1.0F : -1.0F, (i & 1) ? 1.0F : -1.0F));
        }
        faces.push_back(Plane<f32>(Vec3<f32>(1, 0, 0), -1));
        faces.push_back(Plane<f32>(Vec3<f32>(-1, 0, 0), -1));
        faces.push_back(Plane<f32>(Vec3<f32>(0, 1, 0), -1));
        faces.push_back(Plane<f32>(Vec3<f32>(0, -1, 0), -1));
        faces.push_back(Plane<f32>(Vec3<f32>(0, 0, 1), -1));
        faces.push_back(Plane<f32>(Vec3<f32>(0, 0, -1), -1));
        // +X (4,5,7,6); -X (0,2,3,1); +Y (2,6,7,3); -Y (0,1,5,4); +Z (1,3,7,5); -Z (0,4,6,2).
        const u32 fvi[24] = {4, 5, 7, 6, 0, 2, 3, 1, 2, 6, 7, 3, 0, 1, 5, 4, 1, 3, 7, 5, 0, 4, 6, 2};
        for (u32 v : fvi)
        {
            face_vertex_indices.push_back(v);
        }
        face_vertex_offsets.push_back(0);
        for (u32 f = 1U; f <= 6U; ++f)
        {
            face_vertex_offsets.push_back(f * 4U);
        }
    }

    ConvexHullView<f32> view() const
    {
        ConvexHullView<f32> v;
        v.vertices = crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size());
        v.faces = crd::containers::ConstSpan<Plane<f32>>(faces.data(), faces.size());
        v.face_vertex_indices =
            crd::containers::ConstSpan<u32>(face_vertex_indices.data(), face_vertex_indices.size());
        v.face_vertex_offsets =
            crd::containers::ConstSpan<u32>(face_vertex_offsets.data(), face_vertex_offsets.size());
        return v;
    }
};
} // namespace

TEST_CASE("enumerate_faces(ConvexHullView) verbatim copy of cube hull", "[v2j][feature][hull]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    CubeHull cube(&alloc);
    auto v = cube.view();
    crd::containers::Array<HullFaceFeature<f32>> out(&alloc);
    enumerate_faces(v, out);

    REQUIRE(out.size() == 6U);
    for (u32 f = 0; f < 6U; ++f)
    {
        CHECK(out[f].face_index == f);
        CHECK(approx_vec(out[f].plane.normal, v.faces[f].normal));
        CHECK(approx(out[f].plane.d, v.faces[f].d));
        REQUIRE(out[f].vertex_indices.size() == 4U);
        // Span should match the face_vertex_indices region.
        for (u32 k = 0; k < 4U; ++k)
        {
            CHECK(out[f].vertex_indices[k] == cube.face_vertex_indices[f * 4U + k]);
        }
    }
}

TEST_CASE("enumerate_edges(ConvexHullView) 12 manifold-symmetric edges", "[v2j][feature][hull]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    CubeHull cube(&alloc);
    auto v = cube.view();
    crd::containers::Array<EdgeFeature> edges(&alloc);
    enumerate_edges(v, edges);

    REQUIRE(edges.size() == 12U);

    // Every edge satisfies v0 < v1 + face_a != face_b.
    for (usize i = 0; i < edges.size(); ++i)
    {
        CHECK(edges[i].v0 < edges[i].v1);
        CHECK(edges[i].face_a != edges[i].face_b);
        // 1-bit-difference cube-corner pair.
        const u32 diff = edges[i].v0 ^ edges[i].v1;
        CHECK((diff != 0U && (diff & (diff - 1U)) == 0U));
    }

    // Symmetry: for every edge (v0, v1, face_a, face_b), face_a contains
    // the directed pair v0->v1 and face_b contains v1->v0.
    auto face_has_directed = [&](u32 face, u32 from, u32 to) {
        const u32 begin = cube.face_vertex_offsets[face];
        const u32 end = cube.face_vertex_offsets[face + 1U];
        const u32 n = end - begin;
        for (u32 k = 0; k < n; ++k)
        {
            const u32 a = cube.face_vertex_indices[begin + k];
            const u32 b = cube.face_vertex_indices[begin + (k + 1U) % n];
            if (a == from && b == to)
            {
                return true;
            }
        }
        return false;
    };
    for (usize i = 0; i < edges.size(); ++i)
    {
        const bool fwd_in_a = face_has_directed(edges[i].face_a, edges[i].v0, edges[i].v1);
        const bool fwd_in_b = face_has_directed(edges[i].face_b, edges[i].v0, edges[i].v1);
        const bool rev_in_a = face_has_directed(edges[i].face_a, edges[i].v1, edges[i].v0);
        const bool rev_in_b = face_has_directed(edges[i].face_b, edges[i].v1, edges[i].v0);
        // Exactly one of (face_a, face_b) carries v0->v1, the other carries v1->v0.
        CHECK((fwd_in_a != fwd_in_b));
        CHECK((rev_in_a != rev_in_b));
        CHECK(fwd_in_a != rev_in_a);
    }
}

// ---------------------------------------------------------------------------
// (8) closest_face_index
// ---------------------------------------------------------------------------

TEST_CASE("closest_face_index axis-aligned OBB", "[v2j][feature][obb]")
{
    OBB3<f32> cube(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    CHECK(closest_face_index(cube, Vec3<f32>(1, 0, 0)) == 0U);    // +X
    CHECK(closest_face_index(cube, Vec3<f32>(-1, 0, 0)) == 1U);   // -X
    CHECK(closest_face_index(cube, Vec3<f32>(0, 1, 0)) == 2U);    // +Y
    CHECK(closest_face_index(cube, Vec3<f32>(0, -1, 0)) == 3U);   // -Y
    CHECK(closest_face_index(cube, Vec3<f32>(0, 0, 1)) == 4U);    // +Z
    CHECK(closest_face_index(cube, Vec3<f32>(0, 0, -1)) == 5U);   // -Z
}

TEST_CASE("closest_face_index ties go to lowest face_index", "[v2j][feature][obb]")
{
    OBB3<f32> cube(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    // Direction equally aligned with +X (face 0) and +Y (face 2): face 0 wins.
    const Vec3<f32> diag(1, 1, 0);
    const u8 fi = closest_face_index(cube, diag);
    CHECK(fi == 0U);
}

TEST_CASE("closest_face_index hull", "[v2j][feature][hull]")
{
    crd::memory::TlsfAllocator alloc(32U * 1024U);
    CubeHull cube(&alloc);
    auto v = cube.view();
    CHECK(closest_face_index(v, Vec3<f32>(1, 0, 0)) == 0U);
    CHECK(closest_face_index(v, Vec3<f32>(-1, 0, 0)) == 1U);
    CHECK(closest_face_index(v, Vec3<f32>(0, 1, 0)) == 2U);
}

// ---------------------------------------------------------------------------
// (9) is_smooth
// ---------------------------------------------------------------------------

TEST_CASE("is_smooth predicate", "[v2j][feature][smooth]")
{
    Sphere<f32> sph(Vec3<f32>(0, 0, 0), 1.0F);
    CHECK(is_smooth(sph) == true);

    OBB3<f32> obb(Vec3<f32>(0, 0, 0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    CHECK(is_smooth(obb) == false);

    Capsule3<f32> cap(Vec3<f32>(0, 0, 0), Vec3<f32>(0, 1, 0), 0.5F);
    CHECK(is_smooth(cap) == true);

    crd::memory::TlsfAllocator alloc(32U * 1024U);
    CubeHull cube(&alloc);
    auto v = cube.view();
    CHECK(is_smooth(v) == false);
}

// ---------------------------------------------------------------------------
// (10) f64 instantiation
// ---------------------------------------------------------------------------

TEST_CASE("clip_convex_polygon f64", "[v2j][clip][f64]")
{
    crd::memory::TlsfAllocator alloc(32U * 1024U);
    crd::containers::Array<Vec3<f64>> input(&alloc);
    crd::containers::Array<Vec3<f64>> output(&alloc);

    input.push_back(Vec3<f64>(-1, -1, 0));
    input.push_back(Vec3<f64>(1, -1, 0));
    input.push_back(Vec3<f64>(1, 1, 0));
    input.push_back(Vec3<f64>(-1, 1, 0));

    const Plane<f64> clip(Vec3<f64>(1, 0, 0), 0);
    clip_convex_polygon<f64>(
        crd::containers::ConstSpan<Vec3<f64>>(input.data(), input.size()), clip, output);

    REQUIRE(output.size() == 4U);
    CHECK(approx_vec<f64>(output[0], Vec3<f64>(-1, -1, 0), 1e-12));
    CHECK(approx_vec<f64>(output[1], Vec3<f64>(0, -1, 0), 1e-12));
    CHECK(approx_vec<f64>(output[2], Vec3<f64>(0, 1, 0), 1e-12));
    CHECK(approx_vec<f64>(output[3], Vec3<f64>(-1, 1, 0), 1e-12));
}

TEST_CASE("enumerate_faces(OBB3) f64", "[v2j][feature][f64]")
{
    OBB3<f64> cube(Vec3<f64>(0, 0, 0), Vec3<f64>(1, 1, 1), Mat3<f64>::identity());
    auto faces = enumerate_faces(cube);
    REQUIRE(faces.size() == 6U);
    CHECK(approx_vec<f64>(faces[0].plane.normal, Vec3<f64>(1, 0, 0), 1e-12));
    CHECK(approx<f64>(faces[0].plane.d, -1.0, 1e-12));
}

// ---------------------------------------------------------------------------
// (11) Sutherland-Hodgman seam bit-equality (the ADR-0063 determinism pin).
// Clip a polygon by plane_A then plane_B, and by plane_B then plane_A. Where
// the two planes intersect on the polygon, the emitted seam vertex must be
// byte-identical across both orders — this is what the locked lerp form
// `v_i + t * (v_{i+1} - v_i)` guarantees (and what `(1-t)·a + t·b` breaks).
// ---------------------------------------------------------------------------

TEST_CASE("clip seam vertex bit-equal across plane orderings", "[v2j][clip][determinism]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    crd::containers::Array<Vec3<f32>> input(&alloc);
    crd::containers::Array<Vec3<f32>> ab(&alloc);  // A then B
    crd::containers::Array<Vec3<f32>> ba(&alloc);  // B then A
    crd::containers::Array<Vec3<f32>> scratch(&alloc);

    // A non-degenerate polygon (5 vertices, irregular coords to exercise
    // non-trivial intersection arithmetic).
    input.push_back(Vec3<f32>(-3.5F, -2.7F, 0.0F));
    input.push_back(Vec3<f32>(2.9F, -3.1F, 0.0F));
    input.push_back(Vec3<f32>(4.2F, 1.6F, 0.0F));
    input.push_back(Vec3<f32>(0.7F, 3.8F, 0.0F));
    input.push_back(Vec3<f32>(-3.1F, 2.2F, 0.0F));

    // Two planes — both cut the polygon; their intersection line is x=y.
    crd::containers::Array<Plane<f32>> ab_planes(&alloc);
    ab_planes.push_back(Plane<f32>(Vec3<f32>(1, 0, 0), -1));    // x <= 1
    ab_planes.push_back(Plane<f32>(Vec3<f32>(0, 1, 0), -1));    // y <= 1

    crd::containers::Array<Plane<f32>> ba_planes(&alloc);
    ba_planes.push_back(Plane<f32>(Vec3<f32>(0, 1, 0), -1));    // y <= 1
    ba_planes.push_back(Plane<f32>(Vec3<f32>(1, 0, 0), -1));    // x <= 1

    clip_against_convex_volume<f32>(
        crd::containers::ConstSpan<Vec3<f32>>(input.data(), input.size()),
        crd::containers::ConstSpan<Plane<f32>>(ab_planes.data(), ab_planes.size()), ab, scratch);
    clip_against_convex_volume<f32>(
        crd::containers::ConstSpan<Vec3<f32>>(input.data(), input.size()),
        crd::containers::ConstSpan<Plane<f32>>(ba_planes.data(), ba_planes.size()), ba, scratch);

    // The seam vertex is the intersection of the two planes with the polygon
    // boundary — must appear in BOTH outputs bit-identically.
    REQUIRE(ab.size() == ba.size());
    bool found_seam = false;
    for (usize i = 0; i < ab.size(); ++i)
    {
        // The seam vertex sits at (1, 1, 0) — the intersection point of the
        // two planes within the polygon.
        if (std::fabs(ab[i].x - 1.0F) < 1e-4F && std::fabs(ab[i].y - 1.0F) < 1e-4F)
        {
            // Find it in `ba` and require bit-equality (not just approx).
            bool matched = false;
            for (usize j = 0; j < ba.size(); ++j)
            {
                if (std::memcmp(&ab[i], &ba[j], sizeof(Vec3<f32>)) == 0)
                {
                    matched = true;
                    break;
                }
            }
            CHECK(matched);
            found_seam = true;
        }
    }
    CHECK(found_seam);
}

TEST_CASE("is_smooth f64", "[v2j][feature][smooth][f64]")
{
    Sphere<f64> sph(Vec3<f64>(0, 0, 0), 1.0);
    CHECK(is_smooth(sph) == true);
    OBB3<f64> obb(Vec3<f64>(0, 0, 0), Vec3<f64>(1, 1, 1), Mat3<f64>::identity());
    CHECK(is_smooth(obb) == false);
}
