// crd-geometry-spatial v5a — kd_radius brute-force cross-validation tests.

#include <crd/containers/array.hpp>
#include <crd/geometry/spatial/kd_nearest_n.hpp>
#include <crd/geometry/spatial/kd_radius.hpp>
#include <crd/geometry/spatial/kd_tree.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>

using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::spatial::kd_build;
using crd::geometry::spatial::kd_radius;
using crd::geometry::spatial::KdRadiusHit;
using crd::math::Vec3f;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 20}; };

crd::containers::Array<Vec3f> make_cloud(u32 n, u32 seed, crd::memory::IAllocator* a)
{
    crd::containers::Array<Vec3f> pts(a);
    pts.reserve(n);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> u(-1.0F, 1.0F);
    for (u32 i = 0; i < n; ++i) { pts.push_back(Vec3f{u(rng), u(rng), u(rng)}); }
    return pts;
}

// Brute-force: every point within `r` of `q`.
crd::containers::Array<u32> brute_radius(crd::containers::ConstSpan<Vec3f> pts,
                                           const Vec3f& q, f32 r,
                                           crd::memory::IAllocator* a)
{
    const f32 r2 = r * r;
    crd::containers::Array<u32> out(a);
    for (u32 i = 0; i < pts.size(); ++i)
    {
        const Vec3f d = pts[i] - q;
        const f32 d2 = d.x * d.x + d.y * d.y + d.z * d.z;
        if (d2 <= r2) { out.push_back(i); }
    }
    std::sort(out.data(), out.data() + out.size());
    return out;
}
} // namespace

TEST_CASE("kd_radius matches brute force on random cloud", "[geometry-spatial][kd][radius]")
{
    AllocFixture f{};
    auto pts = make_cloud(500U, 17U, &f.alloc);
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                                &f.alloc);

    for (f32 r : {0.05F, 0.1F, 0.25F, 0.5F, 1.5F})
    {
        std::mt19937 rng(31U + static_cast<u32>(r * 100.0F));
        std::uniform_real_distribution<f32> u(-1.0F, 1.0F);
        for (u32 trial = 0; trial < 8U; ++trial)
        {
            const Vec3f q{u(rng), u(rng), u(rng)};

            crd::containers::Array<KdRadiusHit<f32>> hits(&f.alloc);
            kd_radius<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                            q, r, hits);

            crd::containers::Array<u32> got(&f.alloc);
            got.reserve(hits.size());
            for (usize i = 0; i < hits.size(); ++i) { got.push_back(hits[i].payload); }
            std::sort(got.data(), got.data() + got.size());

            auto expected = brute_radius(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                                           q, r, &f.alloc);
            REQUIRE(got.size() == expected.size());
            for (usize i = 0; i < got.size(); ++i) { REQUIRE(got[i] == expected[i]); }
        }
    }
}

TEST_CASE("kd_radius zero-radius returns only coincident points", "[geometry-spatial][kd][radius]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3f> pts(&f.alloc);
    pts.push_back(Vec3f{0.0F, 0.0F, 0.0F});
    pts.push_back(Vec3f{1.0F, 0.0F, 0.0F});
    pts.push_back(Vec3f{0.0F, 0.0F, 0.0F}); // duplicate of pts[0]
    pts.push_back(Vec3f{0.0F, 1.0F, 0.0F});

    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                                &f.alloc);
    crd::containers::Array<KdRadiusHit<f32>> hits(&f.alloc);
    kd_radius<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                    Vec3f{0.0F, 0.0F, 0.0F}, 0.0F, hits);
    REQUIRE(hits.size() == 2U);
    // Both hits must reference original index 0 OR 2 (the two coincident pts).
    bool saw_0 = false;
    bool saw_2 = false;
    for (const auto& h : hits) { if (h.payload == 0U) saw_0 = true; if (h.payload == 2U) saw_2 = true; }
    REQUIRE(saw_0);
    REQUIRE(saw_2);
}

TEST_CASE("kd_radius empty tree returns no hits", "[geometry-spatial][kd][radius]")
{
    AllocFixture f{};
    crd::containers::ConstSpan<Vec3f> empty{};
    auto tree = kd_build<f32>(empty, &f.alloc);
    crd::containers::Array<KdRadiusHit<f32>> hits(&f.alloc);
    kd_radius<f32>(tree, empty, Vec3f{}, 1.0F, hits);
    REQUIRE(hits.size() == 0U);
}

TEST_CASE("kd_radius negative radius returns no hits", "[geometry-spatial][kd][radius]")
{
    AllocFixture f{};
    auto pts = make_cloud(50U, 5U, &f.alloc);
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, &f.alloc);
    crd::containers::Array<KdRadiusHit<f32>> hits(&f.alloc);
    kd_radius<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                    Vec3f{}, -0.1F, hits);
    REQUIRE(hits.size() == 0U);
}

TEST_CASE("kd_radius tolerates non-finite query (no hits, no crash)", "[geometry-spatial][kd][radius][nan]")
{
    AllocFixture f{};
    auto pts = make_cloud(50U, 4U, &f.alloc);
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, &f.alloc);
    crd::containers::Array<KdRadiusHit<f32>> hits(&f.alloc);

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    // NaN query: every finite-vs-NaN comparison is false → AABB-prune kills root → 0 hits.
    kd_radius<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                    Vec3f{nan, 0.0F, 0.0F}, 0.5F, hits);
    REQUIRE(hits.size() == 0U);
    // +Inf radius with finite query → all 50 points hit.
    kd_radius<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                    Vec3f{0.0F, 0.0F, 0.0F}, inf, hits);
    REQUIRE(hits.size() == 50U);
}

TEST_CASE("kd_nearest_n tolerates non-finite query", "[geometry-spatial][kd][knn][nan]")
{
    AllocFixture f{};
    auto pts = make_cloud(30U, 6U, &f.alloc);
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, &f.alloc);

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    crd::containers::Array<crd::geometry::spatial::KdNeighbor<f32>> got(&f.alloc);
    crd::geometry::spatial::kd_nearest_n<f32>(
        tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
        Vec3f{nan, nan, nan}, 5U, got);
    // Heap-fill phase always pushes (out.size() < k), so candidates whose
    // distance² is NaN do enter the heap but remain unordered. The tree's
    // pruning (`f.lower_dsq > worst`) is false for NaN-vs-NaN; the search
    // proceeds. Documented contract: no crash, output is well-formed (size
    // ≤ k), specific contents are unspecified for NaN inputs.
    REQUIRE(got.size() <= 5U);
}

TEST_CASE("kd_radius squared distances match Euclidean truth", "[geometry-spatial][kd][radius]")
{
    AllocFixture f{};
    auto pts = make_cloud(200U, 23U, &f.alloc);
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, &f.alloc);
    const Vec3f q{0.1F, 0.2F, 0.3F};
    crd::containers::Array<KdRadiusHit<f32>> hits(&f.alloc);
    kd_radius<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                    q, 0.4F, hits);
    for (const auto& h : hits)
    {
        const Vec3f d = pts[h.payload] - q;
        const f32 truth = d.x * d.x + d.y * d.y + d.z * d.z;
        REQUIRE(h.distance_squared == truth);
    }
}
