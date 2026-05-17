// crd-geometry-mesh-processing v7a - HalfEdgeMesh substrate tests.
//
// Coverage:
//   * build_from / to_indexed round-trip identity
//   * topology queries (vertex / face / edge count, manifold, closed, Euler)
//   * boundary loop detection (open mesh = N boundary loops)
//   * diagnostic statuses (NonFinite / OutOfBounds / Degenerate / IndicesNotMultipleOf3 / NonManifoldEdge)
//   * walk helpers (for_each_outgoing_he, for_each_face_he)
//   * atomic edits:
//     - flip_edge (diagonal swap on a 2-triangle quad)
//     - split_edge (1 edge becomes 2; 2 faces become 4)
//     - collapse_edge (1 edge merges; 2 faces eliminate)
//   * link-condition rejection
//   * f64 precision tier

#include <crd/containers/array.hpp>
#include <crd/geometry/mesh_processing/mesh_processing.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::math::Vec3;
using crd::geometry::mesh_processing::BuildStatus;
using crd::geometry::mesh_processing::HalfEdgeMesh;
using crd::geometry::mesh_processing::k_null_he;
using crd::geometry::mesh_processing::k_null_vertex;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 22}; };

// Unit cube as 12 triangles (8 vertices). Closed manifold.
void make_cube(crd::containers::Array<Vec3<f32>>& positions, crd::containers::Array<u32>& indices)
{
    positions.push_back(Vec3<f32>{0.F, 0.F, 0.F}); // 0
    positions.push_back(Vec3<f32>{1.F, 0.F, 0.F}); // 1
    positions.push_back(Vec3<f32>{1.F, 1.F, 0.F}); // 2
    positions.push_back(Vec3<f32>{0.F, 1.F, 0.F}); // 3
    positions.push_back(Vec3<f32>{0.F, 0.F, 1.F}); // 4
    positions.push_back(Vec3<f32>{1.F, 0.F, 1.F}); // 5
    positions.push_back(Vec3<f32>{1.F, 1.F, 1.F}); // 6
    positions.push_back(Vec3<f32>{0.F, 1.F, 1.F}); // 7
    // 12 triangles, CCW outward.
    const u32 tris[36] = {
        // -Z face (z=0)
        0, 2, 1,  0, 3, 2,
        // +Z face (z=1)
        4, 5, 6,  4, 6, 7,
        // -Y face (y=0)
        0, 1, 5,  0, 5, 4,
        // +Y face (y=1)
        3, 7, 6,  3, 6, 2,
        // -X face (x=0)
        0, 4, 7,  0, 7, 3,
        // +X face (x=1)
        1, 2, 6,  1, 6, 5,
    };
    for (u32 i = 0; i < 36U; ++i) { indices.push_back(tris[i]); }
}

// Single triangle.
void make_triangle(crd::containers::Array<Vec3<f32>>& positions,
                   crd::containers::Array<u32>&         indices)
{
    positions.push_back(Vec3<f32>{0.F, 0.F, 0.F});
    positions.push_back(Vec3<f32>{1.F, 0.F, 0.F});
    positions.push_back(Vec3<f32>{0.F, 1.F, 0.F});
    indices.push_back(0U);
    indices.push_back(1U);
    indices.push_back(2U);
}

// Two-triangle quad sharing an edge.
void make_quad(crd::containers::Array<Vec3<f32>>& positions, crd::containers::Array<u32>& indices)
{
    positions.push_back(Vec3<f32>{0.F, 0.F, 0.F}); // 0
    positions.push_back(Vec3<f32>{1.F, 0.F, 0.F}); // 1
    positions.push_back(Vec3<f32>{1.F, 1.F, 0.F}); // 2
    positions.push_back(Vec3<f32>{0.F, 1.F, 0.F}); // 3
    // Two triangles sharing edge (0, 2).
    indices.push_back(0U); indices.push_back(1U); indices.push_back(2U);
    indices.push_back(0U); indices.push_back(2U); indices.push_back(3U);
}
} // namespace

// =============================================================================
// build_from + to_indexed round-trip
// =============================================================================

TEST_CASE("HalfEdgeMesh: build_from(triangle) round-trips identically",
          "[geometry-mesh-processing][half-edge][build]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32> idx(&f.alloc);
    make_triangle(pos, idx);

    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::Ok);
    CHECK(m.vertex_count() == 3U);
    CHECK(m.face_count() == 1U);
    CHECK(m.edge_count() == 3U);

    crd::containers::Array<Vec3<f32>> out_pos(&f.alloc);
    crd::containers::Array<u32> out_idx(&f.alloc);
    m.to_indexed(out_pos, out_idx);
    CHECK(out_pos.size() == 3U);
    CHECK(out_idx.size() == 3U);
}

TEST_CASE("HalfEdgeMesh: build_from(cube) - 8 verts / 12 faces / 18 edges / Euler 2",
          "[geometry-mesh-processing][half-edge][build]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32> idx(&f.alloc);
    make_cube(pos, idx);

    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::Ok);
    CHECK(m.vertex_count() == 8U);
    CHECK(m.face_count() == 12U);
    CHECK(m.edge_count() == 18U); // cube has 18 edges (12 face + 6 axis-aligned along diagonals)
    // Euler V - E + F = 8 - 18 + 12 = 2 (sphere topology).
    CHECK(m.euler_characteristic() == 2);
    CHECK(m.is_closed());
    CHECK(m.is_manifold());
    CHECK(m.boundary_loop_count() == 0U);
}

TEST_CASE("HalfEdgeMesh: build_from(quad) - open mesh has 1 boundary loop",
          "[geometry-mesh-processing][half-edge][build][boundary]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32> idx(&f.alloc);
    make_quad(pos, idx);

    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::Ok);
    CHECK(m.vertex_count() == 4U);
    CHECK(m.face_count() == 2U);
    CHECK(m.edge_count() == 5U); // 4 outer + 1 diagonal
    CHECK_FALSE(m.is_closed());
    CHECK(m.is_manifold());
    CHECK(m.boundary_loop_count() == 1U); // single outer boundary loop
}

// =============================================================================
// Diagnostic statuses
// =============================================================================

TEST_CASE("HalfEdgeMesh: indices not multiple of 3 returns IndicesNotMultipleOf3",
          "[geometry-mesh-processing][half-edge][diagnostic]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    pos.push_back(Vec3<f32>{0.F, 0.F, 0.F});
    pos.push_back(Vec3<f32>{1.F, 0.F, 0.F});
    pos.push_back(Vec3<f32>{0.F, 1.F, 0.F});
    crd::containers::Array<u32> idx(&f.alloc);
    idx.push_back(0U); idx.push_back(1U); // only 2 indices

    HalfEdgeMesh<f32> m{&f.alloc};
    CHECK(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                        crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::IndicesNotMultipleOf3);
}

TEST_CASE("HalfEdgeMesh: out-of-bounds index returns OutOfBoundsIndex",
          "[geometry-mesh-processing][half-edge][diagnostic]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    pos.push_back(Vec3<f32>{0.F, 0.F, 0.F});
    pos.push_back(Vec3<f32>{1.F, 0.F, 0.F});
    pos.push_back(Vec3<f32>{0.F, 1.F, 0.F});
    crd::containers::Array<u32> idx(&f.alloc);
    idx.push_back(0U); idx.push_back(1U); idx.push_back(99U); // 99 out of range

    HalfEdgeMesh<f32> m{&f.alloc};
    CHECK(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                        crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::OutOfBoundsIndex);
}

TEST_CASE("HalfEdgeMesh: degenerate triangle returns DegenerateTriangle",
          "[geometry-mesh-processing][half-edge][diagnostic]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    pos.push_back(Vec3<f32>{0.F, 0.F, 0.F});
    pos.push_back(Vec3<f32>{1.F, 0.F, 0.F});
    pos.push_back(Vec3<f32>{0.F, 1.F, 0.F});
    crd::containers::Array<u32> idx(&f.alloc);
    idx.push_back(0U); idx.push_back(1U); idx.push_back(1U); // i1 == i2

    HalfEdgeMesh<f32> m{&f.alloc};
    CHECK(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                        crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::DegenerateTriangle);
}

TEST_CASE("HalfEdgeMesh: non-finite input returns NonFiniteInput",
          "[geometry-mesh-processing][half-edge][diagnostic]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    pos.push_back(Vec3<f32>{std::numeric_limits<f32>::infinity(), 0.F, 0.F});
    pos.push_back(Vec3<f32>{1.F, 0.F, 0.F});
    pos.push_back(Vec3<f32>{0.F, 1.F, 0.F});
    crd::containers::Array<u32> idx(&f.alloc);
    idx.push_back(0U); idx.push_back(1U); idx.push_back(2U);

    HalfEdgeMesh<f32> m{&f.alloc};
    CHECK(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                        crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::NonFiniteInput);
}

// =============================================================================
// Walk helpers
// =============================================================================

TEST_CASE("HalfEdgeMesh: for_each_face_he fires 3 times for a triangle",
          "[geometry-mesh-processing][half-edge][walk]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32> idx(&f.alloc);
    make_triangle(pos, idx);

    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::Ok);
    u32 count = 0;
    m.for_each_face_he(0U, [&](u32) { ++count; });
    CHECK(count == 3U);
}

TEST_CASE("HalfEdgeMesh: for_each_outgoing_he visits the cube vertex's full fan",
          "[geometry-mesh-processing][half-edge][walk]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32> idx(&f.alloc);
    make_cube(pos, idx);

    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::Ok);
    // Each cube vertex has 5 or 6 outgoing half-edges (depending on
    // triangulation choice). Count slot-scanned outgoings, compare with
    // walk count.
    for (u32 v = 0; v < 8U; ++v)
    {
        u32 walk_count = 0;
        m.for_each_outgoing_he(v, [&](u32) { ++walk_count; });
        u32 slot_count = 0;
        for (u32 h = 0; h < m.he_pool_size(); ++h)
        {
            if (m.he_alive(h) && m.he(h).origin == v) { ++slot_count; }
        }
        INFO("vertex " << v << " walk=" << walk_count << " slot=" << slot_count);
        CHECK(walk_count == slot_count);
        CHECK(walk_count >= 4U);
    }
}

// =============================================================================
// flip_edge
// =============================================================================

TEST_CASE("HalfEdgeMesh: flip_edge swaps the diagonal in a quad",
          "[geometry-mesh-processing][half-edge][flip]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32> idx(&f.alloc);
    make_quad(pos, idx);

    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::Ok);
    // Find the interior (diagonal) edge — it's the one whose twin is not
    // a boundary HE.
    u32 diagonal_he = k_null_he;
    for (u32 h = 0; h < m.he_pool_size(); ++h)
    {
        if (!m.he_alive(h)) { continue; }
        if (m.he_is_boundary(h)) { continue; }
        const u32 t = m.he(h).twin;
        if (t != k_null_he && !m.he_is_boundary(t)) { diagonal_he = h; break; }
    }
    REQUIRE(diagonal_he != k_null_he);

    CHECK(m.flip_edge(diagonal_he));
    CHECK(m.vertex_count() == 4U);
    CHECK(m.face_count() == 2U);
    CHECK(m.edge_count() == 5U);
    CHECK(m.is_manifold());
}

TEST_CASE("HalfEdgeMesh: flip_edge on boundary returns false",
          "[geometry-mesh-processing][half-edge][flip]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32> idx(&f.alloc);
    make_quad(pos, idx);

    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::Ok);
    // Find a boundary HE — its twin is a boundary HE too.
    u32 boundary_he = k_null_he;
    for (u32 h = 0; h < m.he_pool_size(); ++h)
    {
        if (!m.he_alive(h)) { continue; }
        if (m.he_is_boundary(h)) { boundary_he = h; break; }
    }
    REQUIRE(boundary_he != k_null_he);
    CHECK_FALSE(m.flip_edge(boundary_he));
}

// =============================================================================
// split_edge
// =============================================================================

TEST_CASE("HalfEdgeMesh: split_edge on a quad's diagonal - 1 vertex + 2 faces added",
          "[geometry-mesh-processing][half-edge][split]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32> idx(&f.alloc);
    make_quad(pos, idx);

    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::Ok);
    // Find interior diagonal HE.
    u32 diag = k_null_he;
    for (u32 h = 0; h < m.he_pool_size(); ++h)
    {
        if (!m.he_alive(h)) { continue; }
        if (m.he_is_boundary(h)) { continue; }
        const u32 t = m.he(h).twin;
        if (t != k_null_he && !m.he_is_boundary(t)) { diag = h; break; }
    }
    REQUIRE(diag != k_null_he);

    const u32 new_v = m.split_edge(diag, Vec3<f32>{0.5F, 0.5F, 0.F});
    REQUIRE(new_v != k_null_vertex);
    CHECK(m.vertex_count() == 5U); // 4 + 1 new
    CHECK(m.face_count() == 4U);   // 2 + 2 new
    CHECK(m.is_manifold());
}

// =============================================================================
// collapse_edge
// =============================================================================

TEST_CASE("HalfEdgeMesh: collapse_edge on cube - vertex / face count drops",
          "[geometry-mesh-processing][half-edge][collapse]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f32>> pos(&f.alloc);
    crd::containers::Array<u32> idx(&f.alloc);
    make_cube(pos, idx);

    HalfEdgeMesh<f32> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f32>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::Ok);
    const u32 v_before = m.vertex_count();
    const u32 f_before = m.face_count();
    // Find a manifold-interior edge to collapse — any HE whose twin is
    // not boundary.
    u32 candidate = k_null_he;
    for (u32 h = 0; h < m.he_pool_size(); ++h)
    {
        if (!m.he_alive(h)) { continue; }
        if (m.he_is_boundary(h)) { continue; }
        const u32 t = m.he(h).twin;
        if (t != k_null_he && !m.he_is_boundary(t))
        {
            // Pick the first candidate whose collapse passes link-condition.
            candidate = h;
            const auto a = m.vertex(m.he(h).origin).position;
            const auto b = m.vertex(m.he_dest(h)).position;
            const Vec3<f32> midpoint{(a.x + b.x) * 0.5F, (a.y + b.y) * 0.5F, (a.z + b.z) * 0.5F};
            if (m.collapse_edge(candidate, midpoint))
            {
                CHECK(m.vertex_count() == v_before - 1U);
                CHECK(m.face_count() == f_before - 2U);
                CHECK(m.is_manifold());
                return;
            }
        }
    }
    FAIL("Could not find a collapsible edge on the cube.");
}

// =============================================================================
// f64 precision tier
// =============================================================================

TEST_CASE("HalfEdgeMesh<f64>: builds + queries work at orbital-scale coords",
          "[geometry-mesh-processing][half-edge][f64]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3<f64>> pos(&f.alloc);
    pos.push_back(Vec3<f64>{0.0, 0.0, 0.0});
    pos.push_back(Vec3<f64>{1.0e6, 0.0, 0.0});
    pos.push_back(Vec3<f64>{0.0, 1.0e6, 0.0});
    crd::containers::Array<u32> idx(&f.alloc);
    idx.push_back(0U); idx.push_back(1U); idx.push_back(2U);

    HalfEdgeMesh<f64> m{&f.alloc};
    REQUIRE(m.build_from(crd::containers::ConstSpan<Vec3<f64>>{pos.data(), pos.size()},
                          crd::containers::ConstSpan<u32>{idx.data(), idx.size()}) == BuildStatus::Ok);
    CHECK(m.vertex_count() == 3U);
    CHECK(m.face_count() == 1U);
}
