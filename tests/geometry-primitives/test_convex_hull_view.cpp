// crd-geometry-primitives v1h -- ConvexHullView: the non-owning query-side hull.
// v1h ships the type plus the two trivially-correct queries: support() (extreme
// vertex along a direction, lowest-index tiebreak) and contains() (inside iff on
// the inner side of every outward-facing face plane). Ray-vs-hull /
// closest-point-on-hull arrive with GJK/EPA in -convex (Phase 3.1.7 v2).

#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/geometry/primitives/primitives.hpp>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace crd;
using namespace crd::geometry::primitives;
using crd::math::Vec3;

namespace
{
// A unit cube [-1,1]^3 as a ConvexHullView. 8 verts, 6 outward-facing faces.
struct CubeHull
{
    std::array<Vec3<f32>, 8> verts{
        Vec3<f32>(-1, -1, -1), Vec3<f32>(1, -1, -1), Vec3<f32>(1, 1, -1), Vec3<f32>(-1, 1, -1),
        Vec3<f32>(-1, -1, 1),  Vec3<f32>(1, -1, 1),  Vec3<f32>(1, 1, 1),  Vec3<f32>(-1, 1, 1),
    };
    std::array<Plane<f32>, 6> faces{
        Plane<f32>(Vec3<f32>(1, 0, 0), -1),  Plane<f32>(Vec3<f32>(-1, 0, 0), -1), Plane<f32>(Vec3<f32>(0, 1, 0), -1),
        Plane<f32>(Vec3<f32>(0, -1, 0), -1), Plane<f32>(Vec3<f32>(0, 0, 1), -1),  Plane<f32>(Vec3<f32>(0, 0, -1), -1),
    };
    // All 6 faces' vertex lists (4 each, CCW) — the fixture satisfies its own
    // type's invariant: offsets has size faces+1, face f owns indices[f][..].
    std::array<u32, 24> face_verts{
        1, 2, 6, 5, // +x
        0, 3, 7, 4, // -x
        3, 2, 6, 7, // +y
        0, 4, 5, 1, // -y
        4, 5, 6, 7, // +z
        0, 3, 2, 1, // -z
    };
    std::array<u32, 7> offsets{0, 4, 8, 12, 16, 20, 24};

    [[nodiscard]] ConvexHullView<f32> view() const noexcept
    {
        return ConvexHullView<f32>(crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size()),
                                   crd::containers::ConstSpan<Plane<f32>>(faces.data(), faces.size()),
                                   crd::containers::ConstSpan<u32>(face_verts.data(), face_verts.size()),
                                   crd::containers::ConstSpan<u32>(offsets.data(), offsets.size()));
    }
};
} // namespace

TEST_CASE("ConvexHullView: support picks the extreme vertex along the direction", "[geometry][hull]")
{
    const CubeHull cube;
    const ConvexHullView<f32> h = cube.view();
    // v2a expanded `support()` to return `SupportPoint{point, vertex_idx}`
    // (ADR-0076 §4 pin #14). Read `.point` for the v1h-equivalent value.
    REQUIRE(support(h, Vec3<f32>(1, 1, 1)).point == Vec3<f32>(1, 1, 1));
    REQUIRE(support(h, Vec3<f32>(-1, -1, -1)).point == Vec3<f32>(-1, -1, -1));
    REQUIRE(support(h, Vec3<f32>(1, -1, 1)).point == Vec3<f32>(1, -1, 1));
    // A face-normal direction: ties on .y/.z resolve to the lowest index among
    // the four +x vertices -> index 1 = (1,-1,-1).
    REQUIRE(support(h, Vec3<f32>(1, 0, 0)).point == Vec3<f32>(1, -1, -1));
}

TEST_CASE("ConvexHullView: contains == inside every face half-space", "[geometry][hull]")
{
    const CubeHull cube;
    const ConvexHullView<f32> h = cube.view();
    REQUIRE(contains(h, Vec3<f32>(0, 0, 0)));
    REQUIRE(contains(h, Vec3<f32>(0.5F, -0.9F, 0.2F)));
    REQUIRE(contains(h, Vec3<f32>(1.0F, 0.0F, 0.0F))); // on the +x face (within epsilon)
    REQUIRE_FALSE(contains(h, Vec3<f32>(1.5F, 0, 0)));
    REQUIRE_FALSE(contains(h, Vec3<f32>(0, 0, -2.0F)));
    REQUIRE_FALSE(contains(h, Vec3<f32>(2, 2, 2)));
}

TEST_CASE("ConvexHullView: is_finite + the offsets layout addresses each face's verts", "[geometry][hull]")
{
    CubeHull cube;
    const ConvexHullView<f32> h = cube.view();
    REQUIRE(is_finite(h));

    // Walk face 0's vertex list via offsets.
    REQUIRE(h.face_vertex_offsets[1] - h.face_vertex_offsets[0] == 4U);
    for (u32 k = h.face_vertex_offsets[0]; k < h.face_vertex_offsets[1]; ++k)
    {
        const Vec3<f32>& v = h.vertices[h.face_vertex_indices[k]];
        REQUIRE(v.x == 1.0F); // every vertex of the +x face has x == 1
    }

    cube.verts[3] = Vec3<f32>(-1, std::numeric_limits<f32>::infinity(), -1);
    REQUIRE_FALSE(is_finite(cube.view()));
}
