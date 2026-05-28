// Tests for crd-geometry-delaunay v8c 3D Bowyer-Watson Delaunay.
//
// Coverage:
//   - Diagnostic statuses (TooFewPoints / NonFiniteInput / DuplicatePoint /
//     Coplanar).
//   - Single tetrahedron (4 pts → 1 tet).
//   - Cube (8 pts → 5 or 6 tets depending on lex order; verified by
//     Delaunay invariants not by exact count).
//   - 24-pt deterministic random cloud satisfies Delaunay invariants
//     (orient3d > 0 + empty circumsphere on every output tet).
//   - **Cospherical-pathology mixture (the v8c-pre paydown validator)**:
//     5 cospherical points on r²=5e9 sphere + 4 non-cospherical points
//     interspersed. This is the test case that would PRODUCE INVERTED TETS
//     without v8c-pre's Stage D `insphere`. The mesh is well-formed because
//     Stage D returns exact zero on the cospherical 5-tuple and the lex-
//     tiebreak picks deterministic results.
//   - Insertion-order determinism (shuffled input → equivalent tet set).
//   - Large-coord f32 stability.
//   - f64 precision tier.

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/delaunay_3d.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <array>
#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec3;
using crd::geometry::delaunay::DelaunayStatus3;
using crd::geometry::delaunay::delaunay_3d;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{16U * 1024U * 1024U, nullptr, "delaunay3d-test-arena"};
};

// Verify Delaunay invariants on every output tet:
//   (1) orient3d(v0, v1, v2, v3) > 0 — positively oriented
//   (2) for every other input point p, insphere(v0, v1, v2, v3, p) <= 0
//       (empty circumsphere — strict > 0 means p is inside, which would
//       violate Delaunay)
template <typename T>
bool verify_delaunay_3d(const crd::containers::Array<Vec3<T>>& pts,
                         const crd::containers::Array<u32>&     tets)
{
    const u32 tet_count = static_cast<u32>(tets.size() / 4U);
    for (u32 t = 0; t < tet_count; ++t)
    {
        const u32 a = tets[4U * t + 0U];
        const u32 b = tets[4U * t + 1U];
        const u32 c = tets[4U * t + 2U];
        const u32 d = tets[4U * t + 3U];
        const T o = crd::geometry::primitives::orient3d(pts[a], pts[b], pts[c], pts[d]);
        if (o <= static_cast<T>(0)) { return false; }
        for (u32 p = 0; p < pts.size(); ++p)
        {
            if (p == a || p == b || p == c || p == d) { continue; }
            const T s = crd::geometry::primitives::insphere(pts[a], pts[b], pts[c], pts[d], pts[p]);
            if (s > static_cast<T>(0)) { return false; }
        }
    }
    return true;
}

} // anonymous namespace

TEST_CASE("delaunay_3d: < 4 points -> TooFewPoints",
          "[geometry-delaunay][v8c]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    auto r = delaunay_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == DelaunayStatus3::TooFewPoints);
    CHECK(r.tet_count == 0U);
}

TEST_CASE("delaunay_3d: non-finite input -> NonFiniteInput",
          "[geometry-delaunay][v8c]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{std::numeric_limits<f32>::infinity(), 0, 0});
    auto r = delaunay_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == DelaunayStatus3::NonFiniteInput);
}

TEST_CASE("delaunay_3d: duplicate input -> DuplicatePoint",
          "[geometry-delaunay][v8c]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0}); // duplicate
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, 1});
    auto r = delaunay_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == DelaunayStatus3::DuplicatePoint);
}

TEST_CASE("delaunay_3d: all-coplanar input -> Coplanar",
          "[geometry-delaunay][v8c]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    // All points have z = 0 -> coplanar in the xy plane.
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{1, 1, 0});
    pts.push_back(Vec3<f32>{0.5F, 0.5F, 0});
    auto r = delaunay_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == DelaunayStatus3::Coplanar);
}

TEST_CASE("delaunay_3d: single tetrahedron (4 pts)",
          "[geometry-delaunay][v8c]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, 1});
    auto r = delaunay_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.tet_count == 1U);
    CHECK(verify_delaunay_3d(pts, r.tet_indices));
}

TEST_CASE("delaunay_3d: 5 points (4 tet corners + center)",
          "[geometry-delaunay][v8c]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, 1});
    pts.push_back(Vec3<f32>{0.25F, 0.25F, 0.25F}); // inside the tet
    auto r = delaunay_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.tet_count == 4U); // tet split into 4 by inserted center point
    CHECK(verify_delaunay_3d(pts, r.tet_indices));
}

TEST_CASE("delaunay_3d: cube (8 pts) produces valid Delaunay mesh",
          "[geometry-delaunay][v8c]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{1, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, 1});
    pts.push_back(Vec3<f32>{1, 0, 1});
    pts.push_back(Vec3<f32>{0, 1, 1});
    pts.push_back(Vec3<f32>{1, 1, 1});
    auto r = delaunay_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.tet_count >= 5U); // cube tessellates into 5-6 tets
    CHECK(r.tet_count <= 12U);
    CHECK(verify_delaunay_3d(pts, r.tet_indices));
}

TEST_CASE("delaunay_3d: 24-pt random cloud satisfies Delaunay invariants",
          "[geometry-delaunay][v8c]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    u32 state = 0xC0FFEEU;
    auto next_rand = [&]() -> f32 {
        state = state * 1103515245U + 12345U;
        return static_cast<f32>((state >> 8) & 0xFFFFFU) / static_cast<f32>(0x100000U);
    };
    for (u32 i = 0; i < 24U; ++i)
    {
        pts.push_back(Vec3<f32>{next_rand() * 10.0F, next_rand() * 10.0F, next_rand() * 10.0F});
    }
    auto r = delaunay_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.tet_count > 0U);
    CHECK(verify_delaunay_3d(pts, r.tet_indices));
}

TEST_CASE("delaunay_3d: cospherical-pathology 9-pt mixture (v8c-pre validator)",
          "[geometry-delaunay][v8c][cospherical]")
{
    // 5 cospherical points on r^2=5e9 sphere (the v8c-pre Stage D
    // discriminator) + 4 non-cospherical points. Without v8c-pre's full
    // Stage D insphere, cavity BFS on the cospherical 5-tuple would produce
    // non-star-shaped cavities -> inverted tets. With it, lex-tiebreak picks
    // a deterministic valid Delaunay mesh.
    AllocFixture f{};
    crd::containers::Array<Vec3<f64>> pts(&f.alloc);
    pts.push_back(Vec3<f64>{50000, 50000, 0});      // cospherical r^2=5e9
    pts.push_back(Vec3<f64>{50000, 40000, 30000});  // cospherical
    pts.push_back(Vec3<f64>{40000, 50000, 30000});  // cospherical
    pts.push_back(Vec3<f64>{30000, 40000, 50000});  // cospherical
    pts.push_back(Vec3<f64>{50000, 30000, 40000});  // cospherical
    pts.push_back(Vec3<f64>{20000, 20000, 20000});  // strictly inside
    pts.push_back(Vec3<f64>{100000, 0, 0});         // outside
    pts.push_back(Vec3<f64>{0, 100000, 0});         // outside
    pts.push_back(Vec3<f64>{0, 0, 100000});         // outside
    auto r = delaunay_3d<f64>(
        crd::containers::ConstSpan<Vec3<f64>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.tet_count > 0U);
    // All output tets must satisfy Delaunay invariants — orient3d > 0
    // and empty circumsphere. Stage D insphere is what makes this pass.
    CHECK(verify_delaunay_3d(pts, r.tet_indices));
}

TEST_CASE("delaunay_3d: insertion-order determinism (shuffled input)",
          "[geometry-delaunay][v8c][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts_a(&f.alloc);
    crd::containers::Array<Vec3<f32>> pts_b(&f.alloc);
    pts_a.push_back(Vec3<f32>{0, 0, 0});
    pts_a.push_back(Vec3<f32>{1, 0, 0});
    pts_a.push_back(Vec3<f32>{0, 1, 0});
    pts_a.push_back(Vec3<f32>{0, 0, 1});
    pts_a.push_back(Vec3<f32>{0.5F, 0.5F, 0.5F});

    pts_b.push_back(Vec3<f32>{0.5F, 0.5F, 0.5F});
    pts_b.push_back(Vec3<f32>{0, 0, 1});
    pts_b.push_back(Vec3<f32>{0, 1, 0});
    pts_b.push_back(Vec3<f32>{1, 0, 0});
    pts_b.push_back(Vec3<f32>{0, 0, 0});

    auto ra = delaunay_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts_a.data(), pts_a.size()}, &f.alloc);
    auto rb = delaunay_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts_b.data(), pts_b.size()}, &f.alloc);
    REQUIRE(ra.ok());
    REQUIRE(rb.ok());
    CHECK(ra.tet_count == rb.tet_count);

    // Build a canonical (sorted) set of vertex-position tuples per tet,
    // then compare the two canonical lists. Same set of tets after vertex
    // canonicalisation = same triangulation.
    auto sorted_tet_positions = [](const crd::containers::Array<Vec3<f32>>& pts,
                                     const crd::containers::Array<u32>&     idx,
                                     u32                                     t) -> std::array<Vec3<f32>, 4> {
        std::array<Vec3<f32>, 4> tp = {pts[idx[4U * t + 0]], pts[idx[4U * t + 1]],
                                        pts[idx[4U * t + 2]], pts[idx[4U * t + 3]]};
        std::sort(tp.begin(), tp.end(), [](const Vec3<f32>& l, const Vec3<f32>& r) {
            if (l.x != r.x) return l.x < r.x;
            if (l.y != r.y) return l.y < r.y;
            return l.z < r.z;
        });
        return tp;
    };
    for (u32 t = 0; t < ra.tet_count; ++t)
    {
        const auto ta = sorted_tet_positions(pts_a, ra.tet_indices, t);
        bool found = false;
        for (u32 u = 0; u < rb.tet_count && !found; ++u)
        {
            const auto tb = sorted_tet_positions(pts_b, rb.tet_indices, u);
            bool match = true;
            for (int i = 0; i < 4 && match; ++i)
            {
                if (ta[i].x != tb[i].x || ta[i].y != tb[i].y || ta[i].z != tb[i].z)
                {
                    match = false;
                }
            }
            if (match) { found = true; }
        }
        CHECK(found);
    }
}

TEST_CASE("delaunay_3d: large-coord f32 stability (1e6 scale)",
          "[geometry-delaunay][v8c]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    constexpr f32 k_s = 1.0e6F;
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{k_s, 0, 0});
    pts.push_back(Vec3<f32>{0, k_s, 0});
    pts.push_back(Vec3<f32>{0, 0, k_s});
    pts.push_back(Vec3<f32>{0.5F * k_s, 0.5F * k_s, 0.5F * k_s});
    auto r = delaunay_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.tet_count >= 1U);
    CHECK(verify_delaunay_3d(pts, r.tet_indices));
}

TEST_CASE("delaunay_3d: f64 precision tier",
          "[geometry-delaunay][v8c][f64]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f64>> pts(&f.alloc);
    pts.push_back(Vec3<f64>{0, 0, 0});
    pts.push_back(Vec3<f64>{1, 0, 0});
    pts.push_back(Vec3<f64>{0, 1, 0});
    pts.push_back(Vec3<f64>{0, 0, 1});
    pts.push_back(Vec3<f64>{0.25, 0.25, 0.25});
    auto r = delaunay_3d<f64>(
        crd::containers::ConstSpan<Vec3<f64>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.tet_count == 4U);
    CHECK(verify_delaunay_3d(pts, r.tet_indices));
}
