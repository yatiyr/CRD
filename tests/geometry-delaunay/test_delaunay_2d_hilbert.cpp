// Tests for crd-geometry-delaunay v8b 2D Bowyer-Watson Delaunay with
// Hilbert-curve insertion order (delaunator-style spatial sort).
//
// Covers (same matrix as v8a + 2 extra):
//   - Diagnostic statuses (TooFewPoints, NonFiniteInput, DuplicatePoint).
//   - 3-point triangle (single triangle).
//   - 4-point square (2 triangles).
//   - Regular pentagon (3 triangles).
//   - N-point random cloud (every triangle CCW + empty circumcircle).
//   - Insertion-order determinism (shuffled input → same set).
//   - Large-coord f32 stability (1e6 scale).
//   - f64 precision tier.
//   - **Equivalence with v8a**: same input → same Delaunay STRUCTURE
//     (canonicalised triangle set identical). The triangle-id-ORDER in
//     the output array can differ because allocation order depends on
//     insertion order; the TRIANGLE SET is invariant.
//   - **Scale test**: 1024-point grid completes without error (verifies
//     Hilbert sort + jump-walk handle larger inputs cleanly).

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/delaunay_2d.hpp>
#include <crd/geometry/delaunay/delaunay_2d_hilbert.hpp>
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
using crd::math::Vec2;
using crd::geometry::delaunay::DelaunayStatus;
using crd::geometry::delaunay::delaunay_2d;
using crd::geometry::delaunay::delaunay_2d_hilbert;

namespace
{

struct AllocFixture
{
    crd::memory::TlsfAllocator alloc{8U * 1024U * 1024U, nullptr, "delaunay-hilbert-test-arena"};
};

template <typename T>
bool verify_delaunay(const crd::containers::Array<Vec2<T>>& pts,
                      const crd::containers::Array<u32>&     tris)
{
    const u32 tri_count = static_cast<u32>(tris.size() / 3U);
    for (u32 t = 0; t < tri_count; ++t)
    {
        const u32 a = tris[3U * t + 0U];
        const u32 b = tris[3U * t + 1U];
        const u32 c = tris[3U * t + 2U];
        const T o = crd::geometry::primitives::orient2d(pts[a], pts[b], pts[c]);
        if (o <= static_cast<T>(0)) { return false; }
        for (u32 p = 0; p < pts.size(); ++p)
        {
            if (p == a || p == b || p == c) { continue; }
            const T s = crd::geometry::primitives::incircle(pts[a], pts[b], pts[c], pts[p]);
            if (s > static_cast<T>(0)) { return false; }
        }
    }
    return true;
}

// Build a canonical (sorted) representation of a triangle list, keyed by
// vertex POSITIONS (not indices, since two different orderings may produce
// different index orders that still describe the same triangulation).
template <typename T>
crd::containers::Array<std::array<Vec2<T>, 3>>
canonicalize(const crd::containers::Array<Vec2<T>>& pts,
              const crd::containers::Array<u32>&     tris,
              crd::memory::IAllocator*               alloc)
{
    const u32 tri_count = static_cast<u32>(tris.size() / 3U);
    crd::containers::Array<std::array<Vec2<T>, 3>> out(alloc);
    out.reserve(tri_count);
    auto vec_less = [](const Vec2<T>& l, const Vec2<T>& r) {
        return l.x < r.x || (l.x == r.x && l.y < r.y);
    };
    for (u32 t = 0; t < tri_count; ++t)
    {
        std::array<Vec2<T>, 3> tp = {pts[tris[3U * t + 0]],
                                      pts[tris[3U * t + 1]],
                                      pts[tris[3U * t + 2]]};
        std::sort(tp.begin(), tp.end(), vec_less);
        out.push_back(tp);
    }
    std::sort(out.begin(), out.end(),
              [&](const std::array<Vec2<T>, 3>& a, const std::array<Vec2<T>, 3>& b) {
                  for (int i = 0; i < 3; ++i)
                  {
                      if (a[i].x != b[i].x) { return a[i].x < b[i].x; }
                      if (a[i].y != b[i].y) { return a[i].y < b[i].y; }
                  }
                  return false;
              });
    return out;
}

} // anonymous namespace

TEST_CASE("delaunay_2d_hilbert: < 3 points -> TooFewPoints",
          "[geometry-delaunay][v8b]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    auto r = delaunay_2d_hilbert<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == DelaunayStatus::TooFewPoints);
    CHECK(r.triangle_count == 0U);
}

TEST_CASE("delaunay_2d_hilbert: non-finite input -> NonFiniteInput",
          "[geometry-delaunay][v8b]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{std::numeric_limits<f32>::infinity(), 0});
    auto r = delaunay_2d_hilbert<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == DelaunayStatus::NonFiniteInput);
}

TEST_CASE("delaunay_2d_hilbert: duplicate input -> DuplicatePoint",
          "[geometry-delaunay][v8b]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{1, 0}); // duplicate
    pts.push_back(Vec2<f32>{0, 1});
    auto r = delaunay_2d_hilbert<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.status == DelaunayStatus::DuplicatePoint);
}

TEST_CASE("delaunay_2d_hilbert: single triangle (3 pts)",
          "[geometry-delaunay][v8b]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{0, 1});
    auto r = delaunay_2d_hilbert<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count == 1U);
    CHECK(verify_delaunay(pts, r.triangle_indices));
}

TEST_CASE("delaunay_2d_hilbert: square (4 pts, 2 triangles)",
          "[geometry-delaunay][v8b]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{1, 0});
    pts.push_back(Vec2<f32>{1, 1});
    pts.push_back(Vec2<f32>{0, 1});
    auto r = delaunay_2d_hilbert<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count == 2U);
    CHECK(verify_delaunay(pts, r.triangle_indices));
}

TEST_CASE("delaunay_2d_hilbert: regular pentagon (5 pts, 3 triangles)",
          "[geometry-delaunay][v8b]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    pts.push_back(Vec2<f32>{ 1.0F,        0.0F});
    pts.push_back(Vec2<f32>{ 0.309017F,   0.951057F});
    pts.push_back(Vec2<f32>{-0.809017F,   0.587785F});
    pts.push_back(Vec2<f32>{-0.809017F,  -0.587785F});
    pts.push_back(Vec2<f32>{ 0.309017F,  -0.951057F});
    auto r = delaunay_2d_hilbert<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count == 3U);
    CHECK(verify_delaunay(pts, r.triangle_indices));
}

TEST_CASE("delaunay_2d_hilbert: 64-pt random cloud satisfies Delaunay invariants",
          "[geometry-delaunay][v8b]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    u32 state = 0xCAFEBABEU;
    auto next_rand = [&]() -> f32 {
        state = state * 1103515245U + 12345U;
        return static_cast<f32>((state >> 8) & 0xFFFFFU) / static_cast<f32>(0x100000U);
    };
    for (u32 i = 0; i < 64U; ++i)
    {
        pts.push_back(Vec2<f32>{next_rand() * 10.0F, next_rand() * 10.0F});
    }
    auto r = delaunay_2d_hilbert<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count > 0U);
    CHECK(verify_delaunay(pts, r.triangle_indices));
}

TEST_CASE("delaunay_2d_hilbert: insertion-order determinism (shuffled input)",
          "[geometry-delaunay][v8b][determinism]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts_a(&f.alloc);
    crd::containers::Array<Vec2<f32>> pts_b(&f.alloc);
    pts_a.push_back(Vec2<f32>{0, 0});
    pts_a.push_back(Vec2<f32>{1, 0});
    pts_a.push_back(Vec2<f32>{1, 1});
    pts_a.push_back(Vec2<f32>{0, 1});
    pts_a.push_back(Vec2<f32>{0.5F, 0.5F});

    pts_b.push_back(Vec2<f32>{0.5F, 0.5F});
    pts_b.push_back(Vec2<f32>{0, 0});
    pts_b.push_back(Vec2<f32>{1, 1});
    pts_b.push_back(Vec2<f32>{0, 1});
    pts_b.push_back(Vec2<f32>{1, 0});

    auto ra = delaunay_2d_hilbert<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts_a.data(), pts_a.size()}, &f.alloc);
    auto rb = delaunay_2d_hilbert<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts_b.data(), pts_b.size()}, &f.alloc);
    REQUIRE(ra.ok());
    REQUIRE(rb.ok());
    CHECK(ra.triangle_count == rb.triangle_count);

    const auto ca = canonicalize(pts_a, ra.triangle_indices, &f.alloc);
    const auto cb = canonicalize(pts_b, rb.triangle_indices, &f.alloc);
    REQUIRE(ca.size() == cb.size());
    for (u32 t = 0; t < ca.size(); ++t)
    {
        for (int i = 0; i < 3; ++i)
        {
            CHECK(ca[t][i].x == cb[t][i].x);
            CHECK(ca[t][i].y == cb[t][i].y);
        }
    }
}

TEST_CASE("delaunay_2d_hilbert: large-coord f32 stability (1e6 scale)",
          "[geometry-delaunay][v8b]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    constexpr f32 kS = 1.0e6F;
    pts.push_back(Vec2<f32>{0, 0});
    pts.push_back(Vec2<f32>{kS, 0});
    pts.push_back(Vec2<f32>{kS, kS});
    pts.push_back(Vec2<f32>{0, kS});
    pts.push_back(Vec2<f32>{0.5F * kS, 0.5F * kS});
    auto r = delaunay_2d_hilbert<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count == 4U);
    CHECK(verify_delaunay(pts, r.triangle_indices));
}

TEST_CASE("delaunay_2d_hilbert: f64 precision tier",
          "[geometry-delaunay][v8b][f64]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f64>> pts(&f.alloc);
    pts.push_back(Vec2<f64>{0, 0});
    pts.push_back(Vec2<f64>{1, 0});
    pts.push_back(Vec2<f64>{1, 1});
    pts.push_back(Vec2<f64>{0, 1});
    pts.push_back(Vec2<f64>{0.5, 0.5});
    auto r = delaunay_2d_hilbert<f64>(
        crd::containers::ConstSpan<Vec2<f64>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count == 4U);
    CHECK(verify_delaunay(pts, r.triangle_indices));
}

TEST_CASE("delaunay_2d_hilbert: equivalence with v8a (same Delaunay structure)",
          "[geometry-delaunay][v8b][equivalence]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    u32 state = 0xDEADBEEFU;
    auto next_rand = [&]() -> f32 {
        state = state * 1103515245U + 12345U;
        return static_cast<f32>((state >> 8) & 0xFFFFFU) / static_cast<f32>(0x100000U);
    };
    for (u32 i = 0; i < 48U; ++i)
    {
        pts.push_back(Vec2<f32>{next_rand() * 100.0F, next_rand() * 100.0F});
    }

    auto ra = delaunay_2d<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    auto rb = delaunay_2d_hilbert<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    REQUIRE(ra.ok());
    REQUIRE(rb.ok());

    // Same point set → same Delaunay triangulation (unique modulo
    // cocircular ties; with random points cocircular ties have measure 0).
    CHECK(ra.triangle_count == rb.triangle_count);

    const auto ca = canonicalize(pts, ra.triangle_indices, &f.alloc);
    const auto cb = canonicalize(pts, rb.triangle_indices, &f.alloc);
    REQUIRE(ca.size() == cb.size());
    for (u32 t = 0; t < ca.size(); ++t)
    {
        for (int i = 0; i < 3; ++i)
        {
            CHECK(ca[t][i].x == cb[t][i].x);
            CHECK(ca[t][i].y == cb[t][i].y);
        }
    }

    // Both must satisfy the Delaunay invariants independently.
    CHECK(verify_delaunay(pts, ra.triangle_indices));
    CHECK(verify_delaunay(pts, rb.triangle_indices));
}

TEST_CASE("delaunay_2d_hilbert: 1024-point scale (jitter grid)",
          "[geometry-delaunay][v8b][scale]")
{
    AllocFixture f{};
    crd::containers::Array<Vec2<f32>> pts(&f.alloc);
    // 32×32 grid with deterministic jitter to avoid exact coincidences.
    u32 state = 0x12345678U;
    auto next_rand = [&]() -> f32 {
        state = state * 1103515245U + 12345U;
        return static_cast<f32>((state >> 8) & 0xFFFFFU) / static_cast<f32>(0x100000U);
    };
    for (u32 j = 0; j < 32U; ++j)
    {
        for (u32 i = 0; i < 32U; ++i)
        {
            const f32 jx = (next_rand() - 0.5F) * 0.3F;
            const f32 jy = (next_rand() - 0.5F) * 0.3F;
            pts.push_back(Vec2<f32>{static_cast<f32>(i) + jx, static_cast<f32>(j) + jy});
        }
    }
    auto r = delaunay_2d_hilbert<f32>(
        crd::containers::ConstSpan<Vec2<f32>>{pts.data(), pts.size()}, &f.alloc);
    CHECK(r.ok());
    CHECK(r.triangle_count > 0U);
    // 1024 points in general position have <= 2N triangles ≈ 2048.
    CHECK(r.triangle_count <= 2U * 1024U);
    CHECK(verify_delaunay(pts, r.triangle_indices));
}
