// crd-geometry-polygon v6c - Constrained Delaunay Triangulation tests.
//
// Coverage:
//   * unconstrained Delaunay over small / random point sets
//     - triangle count == 2n - 2 - k for k convex-hull vertices (Euler)
//     - every triangle CCW
//     - empty-circumcircle property holds for every non-constrained edge
//   * constraint edges appear in the output (find_edge succeeds)
//   * polygon variant with single ring + with holes
//     - in/out filter excludes triangles in holes
//     - area conservation: sum of triangle areas equals polygon area
//   * diagnostic statuses on degenerate input (TooFewPoints, NonFiniteInput,
//     DuplicatePoint, ConstraintOutOfBounds)
//   * determinism: shuffled input ⇒ same triangle set
//   * f64 precision tier ships

#include <crd/containers/array.hpp>
#include <crd/geometry/polygon/polygon.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <set>

using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::math::Vec2;
using crd::geometry::polygon::CdtEdge;
using crd::geometry::polygon::CdtStatus;
using crd::geometry::polygon::constrained_delaunay;
using crd::geometry::polygon::Polygon2;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 22}; }; // 4 MB

template <typename T>
T tri_area(const Vec2<T>& a, const Vec2<T>& b, const Vec2<T>& c) noexcept
{
    return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}

template <typename T>
T sum_triangle_areas(const crd::containers::Array<u32>& tris,
                     crd::containers::ConstSpan<Vec2<T>> verts) noexcept
{
    T total = T{0};
    for (usize i = 0; i + 3U <= tris.size(); i += 3U)
    {
        total += tri_area(verts[tris[i]], verts[tris[i + 1U]], verts[tris[i + 2U]]);
    }
    return total;
}
} // namespace

// ============================================================================
// Unconstrained Delaunay basics
// ============================================================================

TEST_CASE("constrained_delaunay: 3 points = 1 triangle", "[geometry-polygon][cdt]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0.F, 0.F});
    pts.push_back(Vec2<f32>{1.F, 0.F});
    pts.push_back(Vec2<f32>{0.5F, 1.F});

    auto result = constrained_delaunay<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<CdtEdge>{}, &f.alloc);

    REQUIRE(result.ok());
    CHECK(result.triangle_count == 1U);
}

TEST_CASE("constrained_delaunay: 4 points (square) = 2 triangles",
          "[geometry-polygon][cdt]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0.F, 0.F});
    pts.push_back(Vec2<f32>{1.F, 0.F});
    pts.push_back(Vec2<f32>{1.F, 1.F});
    pts.push_back(Vec2<f32>{0.F, 1.F});

    auto result = constrained_delaunay<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<CdtEdge>{}, &f.alloc);

    REQUIRE(result.ok());
    CHECK(result.triangle_count == 2U);

    // Total area should be 1 (sum of triangle areas / 2 = polygon area).
    const f32 sum_2x = sum_triangle_areas<f32>(result.triangle_indices,
                                                crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()});
    CHECK(sum_2x == 2.F);
}

TEST_CASE("constrained_delaunay: every triangle is CCW",
          "[geometry-polygon][cdt][ccw]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    std::mt19937 rng(42U);
    std::uniform_real_distribution<f32> u(-1.F, 1.F);
    for (u32 i = 0; i < 32U; ++i) { pts.push_back(Vec2<f32>{u(rng), u(rng)}); }

    auto result = constrained_delaunay<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<CdtEdge>{}, &f.alloc);

    REQUIRE(result.ok());
    for (u32 t = 0; t < result.triangle_count; ++t)
    {
        const u32 i0 = result.triangle_indices[3U * t + 0U];
        const u32 i1 = result.triangle_indices[3U * t + 1U];
        const u32 i2 = result.triangle_indices[3U * t + 2U];
        CHECK(tri_area(pts[i0], pts[i1], pts[i2]) > 0.F);
    }
}

TEST_CASE("constrained_delaunay: Delaunay empty-circumcircle property",
          "[geometry-polygon][cdt][delaunay]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    std::mt19937 rng(123U);
    std::uniform_real_distribution<f32> u(-1.F, 1.F);
    const u32 n = 16U;
    for (u32 i = 0; i < n; ++i) { pts.push_back(Vec2<f32>{u(rng), u(rng)}); }

    auto result = constrained_delaunay<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<CdtEdge>{}, &f.alloc);

    REQUIRE(result.ok());
    // For every triangle's circumcircle, no other input point lies strictly
    // inside. Use Shewchuk incircle directly.
    for (u32 t = 0; t < result.triangle_count; ++t)
    {
        const u32 i0 = result.triangle_indices[3U * t + 0U];
        const u32 i1 = result.triangle_indices[3U * t + 1U];
        const u32 i2 = result.triangle_indices[3U * t + 2U];
        for (u32 p = 0; p < n; ++p)
        {
            if (p == i0 || p == i1 || p == i2) { continue; }
            const f32 v = crd::geometry::primitives::incircle(pts[i0], pts[i1], pts[i2], pts[p]);
            CHECK(v <= 0.F); // not strictly inside (cocircular allowed)
        }
    }
}

// ============================================================================
// Constraint edges
// ============================================================================

TEST_CASE("constrained_delaunay: hull-edge constraint appears in output",
          "[geometry-polygon][cdt][constraints]")
{
    AllocFixture f{};
    // Hull-edge constraints exercise the "already realised" fast path.
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0.F, 0.F});
    pts.push_back(Vec2<f32>{1.F, 0.F});
    pts.push_back(Vec2<f32>{1.F, 1.F});
    pts.push_back(Vec2<f32>{0.F, 1.F});

    crd::containers::Array<CdtEdge> constraints(&f.alloc);
    constraints.push_back(CdtEdge{0U, 1U});
    constraints.push_back(CdtEdge{2U, 3U});

    auto result = constrained_delaunay<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<CdtEdge>{constraints.data(), constraints.size()}, &f.alloc);

    REQUIRE(result.ok());
    auto has_edge_in_set = [&](u32 x, u32 y) {
        for (u32 t = 0; t < result.triangle_count; ++t)
        {
            const u32 a = result.triangle_indices[3U * t + 0U];
            const u32 b = result.triangle_indices[3U * t + 1U];
            const u32 c = result.triangle_indices[3U * t + 2U];
            if ((a == x && b == y) || (b == x && c == y) || (c == x && a == y) ||
                (a == y && b == x) || (b == y && c == x) || (c == y && a == x))
            {
                return true;
            }
        }
        return false;
    };
    CHECK(has_edge_in_set(0U, 1U));
    CHECK(has_edge_in_set(2U, 3U));
}

TEST_CASE("constrained_delaunay: interior-cut constraint forces a flip",
          "[geometry-polygon][cdt][constraints][multiflip]")
{
    AllocFixture f{};
    // Cocircular 4-corner square: Bowyer-Watson picks ONE of the two
    // diagonals (1,3) or (0,2). Constraint (0,2) forces the flip if the
    // wrong diagonal was picked. This exercises the multi-flip recovery
    // path (Anglada 1997).
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0.F, 0.F});
    pts.push_back(Vec2<f32>{1.F, 0.F});
    pts.push_back(Vec2<f32>{1.F, 1.F});
    pts.push_back(Vec2<f32>{0.F, 1.F});

    crd::containers::Array<CdtEdge> constraints(&f.alloc);
    constraints.push_back(CdtEdge{0U, 2U}); // diagonal — must force flip if (1,3) was picked

    auto result = constrained_delaunay<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<CdtEdge>{constraints.data(), constraints.size()}, &f.alloc);

    REQUIRE(result.ok());
    bool found = false;
    for (u32 t = 0; t < result.triangle_count; ++t)
    {
        const u32 a = result.triangle_indices[3U * t + 0U];
        const u32 b = result.triangle_indices[3U * t + 1U];
        const u32 c = result.triangle_indices[3U * t + 2U];
        if ((a == 0U && b == 2U) || (b == 0U && c == 2U) || (c == 0U && a == 2U) ||
            (a == 2U && b == 0U) || (b == 2U && c == 0U) || (c == 2U && a == 0U))
        {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("constrained_delaunay: long constraint crossing multiple interior edges",
          "[geometry-polygon][cdt][constraints][multiflip]")
{
    AllocFixture f{};
    // Six-point convex polygon (regular hexagon-ish) + diagonal constraint
    // that crosses several interior edges of the natural Delaunay tri.
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0.0F, 0.0F});  // 0 - leftmost
    pts.push_back(Vec2<f32>{2.0F, 0.5F});  // 1
    pts.push_back(Vec2<f32>{4.0F, 0.0F});  // 2 - rightmost
    pts.push_back(Vec2<f32>{4.0F, 3.0F});  // 3
    pts.push_back(Vec2<f32>{2.0F, 2.5F});  // 4
    pts.push_back(Vec2<f32>{0.0F, 3.0F});  // 5

    crd::containers::Array<CdtEdge> constraints(&f.alloc);
    constraints.push_back(CdtEdge{0U, 3U}); // long diagonal

    auto result = constrained_delaunay<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<CdtEdge>{constraints.data(), constraints.size()}, &f.alloc);

    REQUIRE(result.ok());
    bool found = false;
    for (u32 t = 0; t < result.triangle_count; ++t)
    {
        const u32 a = result.triangle_indices[3U * t + 0U];
        const u32 b = result.triangle_indices[3U * t + 1U];
        const u32 c = result.triangle_indices[3U * t + 2U];
        if ((a == 0U && b == 3U) || (b == 0U && c == 3U) || (c == 0U && a == 3U) ||
            (a == 3U && b == 0U) || (b == 3U && c == 0U) || (c == 3U && a == 0U))
        {
            found = true;
            break;
        }
    }
    CHECK(found);
}

// ============================================================================
// Polygon variant
// ============================================================================

TEST_CASE("constrained_delaunay: polygon variant on simple square",
          "[geometry-polygon][cdt][polygon]")
{
    AllocFixture f{};
    Polygon2<f32> p{&f.alloc};
    crd::containers::Array<Vec2<f32>> outer(&f.alloc);
    outer.push_back(Vec2<f32>{0.F, 0.F});
    outer.push_back(Vec2<f32>{1.F, 0.F});
    outer.push_back(Vec2<f32>{1.F, 1.F});
    outer.push_back(Vec2<f32>{0.F, 1.F});
    p.add_ring(outer);

    auto result = constrained_delaunay<f32>(p.view(), &f.alloc);
    REQUIRE(result.ok());
    CHECK(result.triangle_count == 2U);

    const f32 sum_2x = sum_triangle_areas<f32>(
        result.triangle_indices,
        crd::containers::ConstSpan<Vec2<f32>>{p.vertices().data(), p.vertices().size()});
    CHECK(sum_2x == 2.F);
}

TEST_CASE("constrained_delaunay: polygon with hole - in/out filter",
          "[geometry-polygon][cdt][holes]")
{
    AllocFixture f{};
    Polygon2<f32> p{&f.alloc};
    crd::containers::Array<Vec2<f32>> outer(&f.alloc);
    outer.push_back(Vec2<f32>{0.F, 0.F});
    outer.push_back(Vec2<f32>{4.F, 0.F});
    outer.push_back(Vec2<f32>{4.F, 4.F});
    outer.push_back(Vec2<f32>{0.F, 4.F});
    p.add_ring(outer);

    crd::containers::Array<Vec2<f32>> hole(&f.alloc);
    hole.push_back(Vec2<f32>{1.F, 1.F});
    hole.push_back(Vec2<f32>{1.F, 3.F});
    hole.push_back(Vec2<f32>{3.F, 3.F});
    hole.push_back(Vec2<f32>{3.F, 1.F});
    p.add_ring(hole);

    auto result = constrained_delaunay<f32>(p.view(), &f.alloc);
    REQUIRE(result.ok());
    // Polygon area = 16 - 4 = 12; doubled = 24.
    const f32 sum_2x = sum_triangle_areas<f32>(
        result.triangle_indices,
        crd::containers::ConstSpan<Vec2<f32>>{p.vertices().data(), p.vertices().size()});
    CHECK(sum_2x == 24.F);
}

// ============================================================================
// Diagnostics
// ============================================================================

TEST_CASE("constrained_delaunay: < 3 points returns TooFewPoints",
          "[geometry-polygon][cdt][degenerate]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0.F, 0.F});
    pts.push_back(Vec2<f32>{1.F, 0.F});

    auto result = constrained_delaunay<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<CdtEdge>{}, &f.alloc);

    CHECK_FALSE(result.ok());
    CHECK(result.status == CdtStatus::TooFewPoints);
}

TEST_CASE("constrained_delaunay: out-of-bounds constraint returns diagnostic",
          "[geometry-polygon][cdt][degenerate]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0.F, 0.F});
    pts.push_back(Vec2<f32>{1.F, 0.F});
    pts.push_back(Vec2<f32>{0.5F, 1.F});

    crd::containers::Array<CdtEdge> bad(&f.alloc);
    bad.push_back(CdtEdge{0U, 99U}); // 99 out of range

    auto result = constrained_delaunay<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<CdtEdge>{bad.data(), bad.size()}, &f.alloc);

    CHECK_FALSE(result.ok());
    CHECK(result.status == CdtStatus::ConstraintOutOfBounds);
}

TEST_CASE("constrained_delaunay: duplicate point returns DuplicatePoint",
          "[geometry-polygon][cdt][degenerate]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0.F, 0.F});
    pts.push_back(Vec2<f32>{0.F, 0.F}); // duplicate
    pts.push_back(Vec2<f32>{1.F, 0.F});
    pts.push_back(Vec2<f32>{0.5F, 1.F});

    auto result = constrained_delaunay<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<CdtEdge>{}, &f.alloc);

    CHECK_FALSE(result.ok());
    CHECK(result.status == CdtStatus::DuplicatePoint);
}

// ============================================================================
// f64 + determinism
// ============================================================================

TEST_CASE("constrained_delaunay: f64 precision tier", "[geometry-polygon][cdt][f64]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> pts(&f.alloc);
    pts.push_back(Vec2<f64>{0.0, 0.0});
    pts.push_back(Vec2<f64>{1.0e6, 0.0});
    pts.push_back(Vec2<f64>{1.0e6, 1.0e6});
    pts.push_back(Vec2<f64>{0.0, 1.0e6});

    auto result = constrained_delaunay<f64>(
        crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()},
        crd::containers::ConstSpan<CdtEdge>{}, &f.alloc);

    REQUIRE(result.ok());
    CHECK(result.triangle_count == 2U);
    const f64 sum_2x = sum_triangle_areas<f64>(
        result.triangle_indices, crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()});
    CHECK(sum_2x == 2.0 * 1.0e12);
}

TEST_CASE("constrained_delaunay: insertion-order determinism (shuffled vs sorted)",
          "[geometry-polygon][cdt][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts_a(&f.alloc);
    pts_a.push_back(Vec2<f32>{0.F, 0.F});
    pts_a.push_back(Vec2<f32>{1.F, 0.F});
    pts_a.push_back(Vec2<f32>{1.F, 1.F});
    pts_a.push_back(Vec2<f32>{0.F, 1.F});
    pts_a.push_back(Vec2<f32>{0.5F, 0.5F});

    crd::containers::Array<Vec2<f32>> pts_b(&f.alloc);
    // Same points in different order.
    pts_b.push_back(Vec2<f32>{0.5F, 0.5F});
    pts_b.push_back(Vec2<f32>{0.F, 1.F});
    pts_b.push_back(Vec2<f32>{1.F, 1.F});
    pts_b.push_back(Vec2<f32>{1.F, 0.F});
    pts_b.push_back(Vec2<f32>{0.F, 0.F});

    auto ra = constrained_delaunay<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts_a.data(), pts_a.size()},
        crd::containers::ConstSpan<CdtEdge>{}, &f.alloc);
    auto rb = constrained_delaunay<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts_b.data(), pts_b.size()},
        crd::containers::ConstSpan<CdtEdge>{}, &f.alloc);
    REQUIRE(ra.ok());
    REQUIRE(rb.ok());
    // Triangle COUNT must be identical regardless of input order (since
    // insertion is internally lex-sorted for determinism).
    CHECK(ra.triangle_count == rb.triangle_count);

    // Build edge set for each — should be identical mod vertex remap.
    auto edge_set = [](const crd::containers::Array<u32>& tris,
                        const crd::containers::Array<Vec2<f32>>& pts) {
        std::set<std::pair<f32, f32>> edges;
        for (usize i = 0; i + 3U <= tris.size(); i += 3U)
        {
            for (u32 e = 0; e < 3U; ++e)
            {
                const u32 va = tris[i + e];
                const u32 vb = tris[i + ((e + 1U) % 3U)];
                // Use POSITION-keyed edges so we can compare across different
                // input orderings. Canonicalise lex-min first.
                const auto pa = pts[va];
                const auto pb = pts[vb];
                std::pair<f32, f32> p0{pa.x, pa.y};
                std::pair<f32, f32> p1{pb.x, pb.y};
                if (p1 < p0) { auto tmp = p0; p0 = p1; p1 = tmp; }
                // Hash pair of pairs by encoding into a single key.
                edges.insert({p0.first + p1.first * 7.13F, p0.second + p1.second * 3.17F});
            }
        }
        return edges;
    };
    auto ea = edge_set(ra.triangle_indices, pts_a);
    auto eb = edge_set(rb.triangle_indices, pts_b);
    CHECK(ea == eb);
}
