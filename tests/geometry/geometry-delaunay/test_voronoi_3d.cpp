// Tests for crd-geometry-delaunay v8d-3d 3D Voronoi diagram extraction.
//
// Coverage:
//   - Diagnostic statuses propagated from Delaunay (TooFewPoints /
//     NonFiniteInput / DuplicatePoint / Coplanar).
//   - 4-site tetrahedron (1 Delaunay tet -> 1 Voronoi vertex; 4 unbounded
//     cells, each with 3 faces meeting at the vertex).
//   - 5-site (tet corners + interior) → 1 bounded interior cell + 4
//     unbounded.
//   - 8-site cube (Stage D incircle resolves cocircular degeneracies).
//   - N-site random cloud + cospherical-pathology validator.
//   - **Defining property**: for each cell, sites[cell.site_index] is the
//     nearest input site to itself (brute-force). The Voronoi DEFINITION.
//   - Face vertex CCW orientation: face normal (from site toward
//     neighbor) is consistent with vertex-order cross product.
//   - **ConvexHullView helper**: bounded cell converts to a valid
//     ConvexHullView; vertices are face-plane-consistent; outward normals
//     point AWAY from site.
//   - Insertion-order determinism.
//   - f64 precision.

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/voronoi_3d.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::math::Vec3;
using crd::geometry::delaunay::VoronoiResult3;
using crd::geometry::delaunay::VoronoiStatus3;
using crd::geometry::delaunay::convex_hull_for_cell;
using crd::geometry::delaunay::voronoi_3d;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{16U * 1024U * 1024U, nullptr, "voronoi3d-test-arena"};
};

// Defining property: for each cell, verify sites[cell.site_index] is the
// closest input site to itself (brute-force over all sites). The Voronoi
// DEFINITION. Trivially holds because input site is always inside its own
// cell, but the brute-force scan catches catastrophic Voronoi-extraction
// bugs (cells misassigned to wrong sites, etc).
template <typename T>
bool defining_property_holds(const crd::containers::Array<Vec3<T>>& sites,
                              const VoronoiResult3<T>&              r)
{
    for (const auto& cell : r.cells)
    {
        const Vec3<T> sample = sites[cell.site_index];
        T best_d2 = std::numeric_limits<T>::infinity();
        u32 best_s = std::numeric_limits<u32>::max();
        for (u32 s = 0; s < sites.size(); ++s)
        {
            const T dx = sample.x - sites[s].x;
            const T dy = sample.y - sites[s].y;
            const T dz = sample.z - sites[s].z;
            const T d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best_d2) { best_d2 = d2; best_s = s; }
        }
        if (best_s != cell.site_index) { return false; }
    }
    return true;
}

// Face normal sanity: compute the average face normal from triangulated
// fan (vertex[0], vertex[i], vertex[i+1]) cross products. Should align
// with (neighbor_site - site) direction (the cell's outward face normal).
template <typename T>
bool face_normals_outward(const crd::containers::Array<Vec3<T>>& sites,
                           const VoronoiResult3<T>&              r)
{
    for (const auto& cell : r.cells)
    {
        const Vec3<T> site = sites[cell.site_index];
        for (const auto& face : cell.faces)
        {
            if (face.vertex_indices.size() < 3U) { continue; } // degenerate/unbounded
            const Vec3<T> nbr = sites[face.neighbor_site_index];
            const Vec3<T> outward{nbr.x - site.x, nbr.y - site.y, nbr.z - site.z};

            // Average normal from triangle fan.
            const Vec3<T>& v0 = r.voronoi_vertices[face.vertex_indices[0]];
            T avg_nx = static_cast<T>(0);
            T avg_ny = static_cast<T>(0);
            T avg_nz = static_cast<T>(0);
            for (u32 i = 1; i + 1U < face.vertex_indices.size(); ++i)
            {
                const Vec3<T>& vi = r.voronoi_vertices[face.vertex_indices[i]];
                const Vec3<T>& vj = r.voronoi_vertices[face.vertex_indices[i + 1U]];
                const T ax = vi.x - v0.x;
                const T ay = vi.y - v0.y;
                const T az = vi.z - v0.z;
                const T bx = vj.x - v0.x;
                const T by = vj.y - v0.y;
                const T bz = vj.z - v0.z;
                avg_nx += ay * bz - az * by;
                avg_ny += az * bx - ax * bz;
                avg_nz += ax * by - ay * bx;
            }
            const T dotp = avg_nx * outward.x + avg_ny * outward.y + avg_nz * outward.z;
            if (dotp <= static_cast<T>(0)) { return false; }
        }
    }
    return true;
}

} // anonymous namespace

TEST_CASE("voronoi_3d: < 4 points -> TooFewPoints",
          "[geometry-delaunay][voronoi][v8d-3d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    auto r = voronoi_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == VoronoiStatus3::TooFewPoints);
    CHECK(r.cells.size() == 0U);
}

TEST_CASE("voronoi_3d: non-finite input -> NonFiniteInput",
          "[geometry-delaunay][voronoi][v8d-3d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, std::numeric_limits<f32>::infinity()});
    auto r = voronoi_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == VoronoiStatus3::NonFiniteInput);
}

TEST_CASE("voronoi_3d: duplicate input -> DuplicatePoint",
          "[geometry-delaunay][voronoi][v8d-3d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0}); // duplicate
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, 1});
    auto r = voronoi_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == VoronoiStatus3::DuplicatePoint);
}

TEST_CASE("voronoi_3d: all-coplanar input -> Coplanar",
          "[geometry-delaunay][voronoi][v8d-3d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{1, 1, 0});
    pts.push_back(Vec3<f32>{0.5F, 0.5F, 0});
    auto r = voronoi_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == VoronoiStatus3::Coplanar);
}

TEST_CASE("voronoi_3d: 4-site tetrahedron (1 Voronoi vertex; all cells unbounded)",
          "[geometry-delaunay][voronoi][v8d-3d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, 1});
    auto r = voronoi_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.voronoi_vertices.size() == 1U); // 1 Delaunay tet -> 1 circumcentre
    CHECK(r.cells.size() == 4U);
    for (const auto& cell : r.cells)
    {
        CHECK(!cell.is_bounded);
        CHECK(cell.faces.size() == 3U); // each site has 3 Delaunay neighbors
    }
    CHECK(defining_property_holds(pts, r));
}

TEST_CASE("voronoi_3d: 5-site tet + center (1 bounded cell)",
          "[geometry-delaunay][voronoi][v8d-3d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, 1});
    pts.push_back(Vec3<f32>{0.2F, 0.2F, 0.2F}); // interior
    auto r = voronoi_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.cells.size() == 5U);
    // Interior site (index 4) should have a bounded cell.
    u32 bounded_count = 0;
    for (const auto& cell : r.cells)
    {
        if (cell.is_bounded) { ++bounded_count; }
    }
    CHECK(bounded_count == 1U);
    CHECK(r.cells[4].is_bounded);
    CHECK(defining_property_holds(pts, r));
    CHECK(face_normals_outward(pts, r));
}

TEST_CASE("voronoi_3d: 8-site cube produces valid Voronoi diagram",
          "[geometry-delaunay][voronoi][v8d-3d][cocircular]")
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
    auto r = voronoi_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.cells.size() == 8U);
    // All 8 cube corners are on the convex hull -> all 8 cells unbounded.
    for (const auto& cell : r.cells)
    {
        CHECK(!cell.is_bounded);
    }
    CHECK(defining_property_holds(pts, r));
}

TEST_CASE("voronoi_3d: 9-site cube + interior (interior bounded)",
          "[geometry-delaunay][voronoi][v8d-3d]")
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
    pts.push_back(Vec3<f32>{0.5F, 0.5F, 0.5F}); // interior
    auto r = voronoi_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.cells.size() == 9U);
    CHECK(r.cells[8].is_bounded);
    // 8 hull cells should all be unbounded.
    for (u32 i = 0; i < 8U; ++i)
    {
        CHECK(!r.cells[i].is_bounded);
    }
    CHECK(defining_property_holds(pts, r));
    CHECK(face_normals_outward(pts, r));
}

TEST_CASE("voronoi_3d: 24-site random cloud satisfies defining property",
          "[geometry-delaunay][voronoi][v8d-3d]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    u32 state = 0xC0FFEEU;
    auto next_rand = [&]() -> f32 {
        state = state * 1103515245U + 12345U;
        return static_cast<f32>((state >> 8U) & 0xFFFFFU) / static_cast<f32>(0x100000U);
    };
    for (u32 i = 0; i < 24U; ++i)
    {
        pts.push_back(Vec3<f32>{next_rand() * 10.0F, next_rand() * 10.0F, next_rand() * 10.0F});
    }
    auto r = voronoi_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.cells.size() == 24U);
    CHECK(defining_property_holds(pts, r));
    CHECK(face_normals_outward(pts, r));
}

TEST_CASE("voronoi_3d: cospherical-pathology 9-pt mixture (v8c-pre validator)",
          "[geometry-delaunay][voronoi][v8d-3d][cospherical]")
{
    // Same 9-pt cospherical configuration as v8c's validator. Without
    // Stage D insphere, Voronoi extraction would cascade off inverted tets.
    AllocFixture f{};
    crd::containers::Array<Vec3<f64>> pts(&f.alloc);
    pts.push_back(Vec3<f64>{50000, 50000, 0});
    pts.push_back(Vec3<f64>{50000, 40000, 30000});
    pts.push_back(Vec3<f64>{40000, 50000, 30000});
    pts.push_back(Vec3<f64>{30000, 40000, 50000});
    pts.push_back(Vec3<f64>{50000, 30000, 40000});
    pts.push_back(Vec3<f64>{20000, 20000, 20000});
    pts.push_back(Vec3<f64>{100000, 0, 0});
    pts.push_back(Vec3<f64>{0, 100000, 0});
    pts.push_back(Vec3<f64>{0, 0, 100000});
    auto r = voronoi_3d<f64>(
        crd::containers::ConstSpan<Vec3<f64>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.cells.size() == 9U);
    CHECK(defining_property_holds(pts, r));
}

TEST_CASE("voronoi_3d: ConvexHullView helper on bounded cell",
          "[geometry-delaunay][voronoi][v8d-3d][hull]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f64>> pts(&f.alloc);
    pts.push_back(Vec3<f64>{0, 0, 0});
    pts.push_back(Vec3<f64>{1, 0, 0});
    pts.push_back(Vec3<f64>{0, 1, 0});
    pts.push_back(Vec3<f64>{0, 0, 1});
    pts.push_back(Vec3<f64>{0.2, 0.2, 0.2});
    auto r = voronoi_3d<f64>(
        crd::containers::ConstSpan<Vec3<f64>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    REQUIRE(r.cells[4].is_bounded);

    auto hull = convex_hull_for_cell<f64>(r,
        crd::containers::ConstSpan<Vec3<f64>>{pts.data(), pts.size()}, 4U, &f.alloc);
    CHECK(!hull.empty());
    CHECK(hull.faces.size() == r.cells[4].faces.size());
    CHECK(hull.face_vertex_offsets.size() == hull.faces.size() + 1U);

    // Every face plane should pass through its referenced vertices (within
    // numerical tolerance). For each face k, every vertex in the face's
    // vertex list satisfies normal · v + d ≈ 0.
    for (u32 k = 0; k < hull.faces.size(); ++k)
    {
        const auto& plane = hull.faces[k];
        const u32 v_begin = hull.face_vertex_offsets[k];
        const u32 v_end   = hull.face_vertex_offsets[k + 1U];
        for (u32 vi = v_begin; vi < v_end; ++vi)
        {
            const auto& v = hull.vertices[hull.face_vertex_indices[vi]];
            const f64 sd = plane.normal.x * v.x + plane.normal.y * v.y + plane.normal.z * v.z + plane.d;
            CHECK(std::abs(sd) < 1.0e-9);
        }
    }

    // The site (the inner point) should be on the NEGATIVE side of every
    // face plane (cell interior is where signed_distance < 0 since normals
    // point outward).
    const auto& site = pts[4];
    for (const auto& plane : hull.faces)
    {
        const f64 sd = plane.normal.x * site.x + plane.normal.y * site.y + plane.normal.z * site.z + plane.d;
        CHECK(sd < 0.0);
    }
}

TEST_CASE("voronoi_3d: ConvexHullView helper on unbounded cell returns empty",
          "[geometry-delaunay][voronoi][v8d-3d][hull]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts(&f.alloc);
    pts.push_back(Vec3<f32>{0, 0, 0});
    pts.push_back(Vec3<f32>{1, 0, 0});
    pts.push_back(Vec3<f32>{0, 1, 0});
    pts.push_back(Vec3<f32>{0, 0, 1});
    auto r = voronoi_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    // All 4 cells are unbounded.
    auto hull = convex_hull_for_cell<f32>(r,
        crd::containers::ConstSpan<Vec3<f32>>{pts.data(), pts.size()}, 0U, &f.alloc);
    CHECK(hull.empty());
}

TEST_CASE("voronoi_3d: insertion-order determinism",
          "[geometry-delaunay][voronoi][v8d-3d][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pts_a(&f.alloc);
    crd::containers::Array<Vec3<f32>> pts_b(&f.alloc);
    pts_a.push_back(Vec3<f32>{0, 0, 0});
    pts_a.push_back(Vec3<f32>{1, 0, 0});
    pts_a.push_back(Vec3<f32>{0, 1, 0});
    pts_a.push_back(Vec3<f32>{0, 0, 1});
    pts_a.push_back(Vec3<f32>{0.25F, 0.25F, 0.25F});

    pts_b.push_back(Vec3<f32>{0.25F, 0.25F, 0.25F});
    pts_b.push_back(Vec3<f32>{0, 0, 1});
    pts_b.push_back(Vec3<f32>{0, 1, 0});
    pts_b.push_back(Vec3<f32>{1, 0, 0});
    pts_b.push_back(Vec3<f32>{0, 0, 0});

    auto ra = voronoi_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts_a.data(), pts_a.size()}, &f.alloc);
    auto rb = voronoi_3d<f32>(
        crd::containers::ConstSpan<Vec3<f32>>{pts_b.data(), pts_b.size()}, &f.alloc);
    REQUIRE(ra.ok());
    REQUIRE(rb.ok());
    CHECK(ra.cells.size() == rb.cells.size());
    CHECK(defining_property_holds(pts_a, ra));
    CHECK(defining_property_holds(pts_b, rb));
}

TEST_CASE("voronoi_3d: f64 precision tier",
          "[geometry-delaunay][voronoi][v8d-3d][f64]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f64>> pts(&f.alloc);
    pts.push_back(Vec3<f64>{0, 0, 0});
    pts.push_back(Vec3<f64>{1, 0, 0});
    pts.push_back(Vec3<f64>{0, 1, 0});
    pts.push_back(Vec3<f64>{0, 0, 1});
    pts.push_back(Vec3<f64>{0.2, 0.2, 0.2});
    auto r = voronoi_3d<f64>(
        crd::containers::ConstSpan<Vec3<f64>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(r.ok());
    CHECK(r.cells.size() == 5U);
    CHECK(r.cells[4].is_bounded);
    CHECK(defining_property_holds(pts, r));
    CHECK(face_normals_outward(pts, r));
}
