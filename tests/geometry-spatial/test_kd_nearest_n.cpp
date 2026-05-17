// crd-geometry-spatial v5a — kd_nearest_n brute-force cross-validation tests.
//
// Verifies the branch-and-bound k-NN search returns the exact same result as
// a brute-force per-point distance scan, with the lowest-payload-index
// tiebreak on equal distances per ADR-0076 §4 pin #11.

#include <crd/containers/array.hpp>
#include <crd/geometry/spatial/kd_nearest_n.hpp>
#include <crd/geometry/spatial/kd_tree.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <random>

using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::spatial::kd_build;
using crd::geometry::spatial::kd_nearest_n;
using crd::geometry::spatial::KdNeighbor;
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

// Brute-force k-NN with lowest-payload-tie-breaker.
crd::containers::Array<KdNeighbor<f32>> brute_knn(crd::containers::ConstSpan<Vec3f> pts,
                                                    const Vec3f& q, u32 k,
                                                    crd::memory::IAllocator* a)
{
    crd::containers::Array<KdNeighbor<f32>> all(a);
    all.reserve(pts.size());
    for (u32 i = 0; i < pts.size(); ++i)
    {
        const Vec3f d = pts[i] - q;
        all.push_back(KdNeighbor<f32>{i, d.x * d.x + d.y * d.y + d.z * d.z});
    }
    auto cmp = [](const KdNeighbor<f32>& lhs, const KdNeighbor<f32>& rhs) {
        if (lhs.distance_squared < rhs.distance_squared) return true;
        if (lhs.distance_squared > rhs.distance_squared) return false;
        return lhs.payload < rhs.payload;
    };
    std::sort(all.data(), all.data() + all.size(), cmp);

    crd::containers::Array<KdNeighbor<f32>> top(a);
    const u32 take = std::min<u32>(k, static_cast<u32>(all.size()));
    top.reserve(take);
    for (u32 i = 0; i < take; ++i) { top.push_back(all[i]); }
    return top;
}
} // namespace

TEST_CASE("kd_nearest_n matches brute force", "[geometry-spatial][kd][knn]")
{
    AllocFixture f{};
    auto pts = make_cloud(1000U, 42U, &f.alloc);
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                                &f.alloc);

    for (u32 k : {1U, 5U, 20U, 100U})
    {
        std::mt19937 rng(101U + k);
        std::uniform_real_distribution<f32> u(-1.0F, 1.0F);
        for (u32 trial = 0; trial < 5U; ++trial)
        {
            const Vec3f q{u(rng), u(rng), u(rng)};

            crd::containers::Array<KdNeighbor<f32>> got(&f.alloc);
            kd_nearest_n<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                                q, k, got);

            auto expected = brute_knn(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                                        q, k, &f.alloc);
            REQUIRE(got.size() == expected.size());
            for (usize i = 0; i < got.size(); ++i)
            {
                REQUIRE(got[i].payload          == expected[i].payload);
                REQUIRE(got[i].distance_squared == expected[i].distance_squared);
            }
        }
    }
}

TEST_CASE("kd_nearest_n k=1 is the closest point", "[geometry-spatial][kd][knn]")
{
    AllocFixture f{};
    auto pts = make_cloud(500U, 7U, &f.alloc);
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                                &f.alloc);
    const Vec3f q{0.3F, -0.2F, 0.1F};

    crd::containers::Array<KdNeighbor<f32>> got(&f.alloc);
    kd_nearest_n<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, q, 1U, got);
    REQUIRE(got.size() == 1U);

    // Verify: no other point is strictly closer.
    for (u32 i = 0; i < pts.size(); ++i)
    {
        const Vec3f d = pts[i] - q;
        const f32 d2 = d.x * d.x + d.y * d.y + d.z * d.z;
        REQUIRE(d2 >= got[0].distance_squared);
    }
}

TEST_CASE("kd_nearest_n with k > N returns all points sorted", "[geometry-spatial][kd][knn]")
{
    AllocFixture f{};
    auto pts = make_cloud(15U, 9U, &f.alloc);
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                                &f.alloc);

    crd::containers::Array<KdNeighbor<f32>> got(&f.alloc);
    // Request 50, only 15 exist.
    kd_nearest_n<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                        Vec3f{}, 50U, got);
    REQUIRE(got.size() == 15U);
    // Check ascending sort.
    for (usize i = 1; i < got.size(); ++i)
    {
        REQUIRE(got[i].distance_squared >= got[i - 1U].distance_squared);
    }
}

TEST_CASE("kd_nearest_n empty tree", "[geometry-spatial][kd][knn]")
{
    AllocFixture f{};
    crd::containers::ConstSpan<Vec3f> empty{};
    auto tree = kd_build<f32>(empty, &f.alloc);
    crd::containers::Array<KdNeighbor<f32>> got(&f.alloc);
    kd_nearest_n<f32>(tree, empty, Vec3f{}, 5U, got);
    REQUIRE(got.size() == 0U);
}

TEST_CASE("kd_nearest_n k=0 returns empty", "[geometry-spatial][kd][knn]")
{
    AllocFixture f{};
    auto pts = make_cloud(20U, 3U, &f.alloc);
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, &f.alloc);
    crd::containers::Array<KdNeighbor<f32>> got(&f.alloc);
    // k = 0
    kd_nearest_n<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                        Vec3f{}, 0U, got);
    REQUIRE(got.size() == 0U);
}

// =============================================================================
// Concurrent k-NN queries via crd-jobs fiber pool — proves naturally-const-safe
// =============================================================================
//
// KdTree's queries are PURELY READ-ONLY (each point lives in exactly one
// leaf — no per-object dedup state, no `mutable` member). This test
// validates the const-safe-by-construction claim empirically: many fibers
// running concurrent k-NN against the SHARED tree, each with its own
// output Array, must produce the same result as the single-thread reference.
// ASan instrumentation catches any silent race the impl might have despite
// the construction-level safety claim.
//
// No `KdScratch` parameter — and no need for one. The other backends with
// multi-cell membership (SpatialHash, UniformGrid) need a scratch because
// their dedup state is mutable; KdTree doesn't have dedup state at all.
// (Same property holds for LooseOctree + R*-tree — see their concurrent
// tests in test_loose_octree.cpp + test_rtree.cpp.)

namespace
{
struct KdConcurrencyCorpus
{
    static constexpr u32 kQueries = 16U;
    static constexpr u32 kItersPerQuery = 25U;
    static constexpr usize kNeighbors = 10U;

    crd::memory::TlsfAllocator alloc{1U << 22};
    crd::containers::Array<crd::math::Vec3f> pts{&alloc};
    crd::geometry::spatial::KdTree<f32> tree{&alloc};
    crd::math::Vec3f queries[kQueries]{};
    crd::containers::Array<KdNeighbor<f32>> ref[kQueries] = {
        crd::containers::Array<KdNeighbor<f32>>{&alloc}, crd::containers::Array<KdNeighbor<f32>>{&alloc},
        crd::containers::Array<KdNeighbor<f32>>{&alloc}, crd::containers::Array<KdNeighbor<f32>>{&alloc},
        crd::containers::Array<KdNeighbor<f32>>{&alloc}, crd::containers::Array<KdNeighbor<f32>>{&alloc},
        crd::containers::Array<KdNeighbor<f32>>{&alloc}, crd::containers::Array<KdNeighbor<f32>>{&alloc},
        crd::containers::Array<KdNeighbor<f32>>{&alloc}, crd::containers::Array<KdNeighbor<f32>>{&alloc},
        crd::containers::Array<KdNeighbor<f32>>{&alloc}, crd::containers::Array<KdNeighbor<f32>>{&alloc},
        crd::containers::Array<KdNeighbor<f32>>{&alloc}, crd::containers::Array<KdNeighbor<f32>>{&alloc},
        crd::containers::Array<KdNeighbor<f32>>{&alloc}, crd::containers::Array<KdNeighbor<f32>>{&alloc}};
    std::atomic<crd::u32> mismatches{0};
};
} // namespace

TEST_CASE("KdTree concurrent k-NN queries via crd-jobs (proves naturally-const-safe)",
          "[geometry-spatial][kd][knn][concurrent][jobs]")
{
    auto corpus = std::make_unique<KdConcurrencyCorpus>();
    auto cloud = make_cloud(500U, 42U, &corpus->alloc);
    for (auto& p : cloud) { corpus->pts.push_back(p); }
    corpus->tree = kd_build<f32>(crd::containers::ConstSpan<crd::math::Vec3f>{corpus->pts.data(), corpus->pts.size()},
                                    &corpus->alloc);

    std::mt19937 rng(101U);
    std::uniform_real_distribution<f32> uc(-1.0F, 1.0F);
    for (u32 q = 0; q < KdConcurrencyCorpus::kQueries; ++q)
    {
        corpus->queries[q] = crd::math::Vec3f{uc(rng), uc(rng), uc(rng)};
        kd_nearest_n<f32>(corpus->tree,
                            crd::containers::ConstSpan<crd::math::Vec3f>{corpus->pts.data(), corpus->pts.size()},
                            corpus->queries[q], KdConcurrencyCorpus::kNeighbors, corpus->ref[q]);
    }

    constexpr u32 kTotalTasks = KdConcurrencyCorpus::kQueries * KdConcurrencyCorpus::kItersPerQuery;
    auto* corpus_ptr = corpus.get();
    crd::jobs::Counter* counter = crd::jobs::parallel_for(
        kTotalTasks, /*num_jobs=*/16U,
        [corpus_ptr](crd::u32 begin, crd::u32 end) noexcept {
            for (crd::u32 task_idx = begin; task_idx < end; ++task_idx)
            {
                const u32 q = task_idx % KdConcurrencyCorpus::kQueries;
                crd::memory::TlsfAllocator local_alloc{1U << 16};
                crd::containers::Array<KdNeighbor<f32>> got(&local_alloc);
                kd_nearest_n<f32>(corpus_ptr->tree,
                                    crd::containers::ConstSpan<crd::math::Vec3f>{corpus_ptr->pts.data(), corpus_ptr->pts.size()},
                                    corpus_ptr->queries[q], KdConcurrencyCorpus::kNeighbors, got);
                bool ok = (got.size() == corpus_ptr->ref[q].size());
                if (ok)
                {
                    for (usize i = 0; i < got.size(); ++i)
                    {
                        if (got[i].payload != corpus_ptr->ref[q][i].payload
                            || got[i].distance_squared != corpus_ptr->ref[q][i].distance_squared)
                        { ok = false; break; }
                    }
                }
                if (!ok) { corpus_ptr->mismatches.fetch_add(1U, std::memory_order_relaxed); }
            }
        });
    crd::jobs::wait(counter);
    REQUIRE(corpus->mismatches.load() == 0U);
}

TEST_CASE("kd_nearest_n equal-distance tie picks lowest payload", "[geometry-spatial][kd][knn]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3f> pts(&f.alloc);
    // Three points equidistant from origin, in input order 0, 1, 2.
    pts.push_back(Vec3f{ 1.0F, 0.0F, 0.0F});
    pts.push_back(Vec3f{-1.0F, 0.0F, 0.0F});
    pts.push_back(Vec3f{ 0.0F, 1.0F, 0.0F});
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, &f.alloc);

    crd::containers::Array<KdNeighbor<f32>> got(&f.alloc);
    kd_nearest_n<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                        Vec3f{}, 2U, got);
    REQUIRE(got.size() == 2U);
    // All three tie at distance² = 1. Pin #11 → lowest two payloads (0 and 1).
    REQUIRE(got[0].payload == 0U);
    REQUIRE(got[1].payload == 1U);
}
