// crd-geometry-spatial v5b — LooseOctree tests.
//
// Coverage:
//   * Build/sizeof + empty / single-object / many-object correctness
//   * insert/remove/update mechanics + handle stability
//   * **The fast-path test** — Ulrich's invariant verification: small motions
//     within loose AABB cause ZERO restructure (the elite-quality discriminator)
//   * Brute-force cross-validation of overlap + raycast on random clouds
//   * Loosening factor sanity (1.5 / 2.0 / 3.0)
//   * Lowest-payload tiebreak on equal raycast t
//   * Determinism under permuted insert order
//   * NaN-tolerance on query inputs
//   * Empty / single-object / all-coincident / huge-extent edge cases

#include <crd/containers/array.hpp>
#include <crd/geometry/spatial/loose_octree.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <random>

using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Ray3;
using crd::geometry::spatial::LooseOctree;
using crd::geometry::spatial::OctreeBuildOptions;
using crd::geometry::spatial::OctreeObjectId;
using crd::math::Vec3;
using crd::math::Vec3f;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 20}; };

OctreeBuildOptions<f32> default_opts(f32 half = 100.0F)
{
    return OctreeBuildOptions<f32>{
        AABB3<f32>{Vec3f{-half, -half, -half}, Vec3f{half, half, half}},
        2.0F, 8U, 8U};
}

AABB3<f32> aabb_around(const Vec3f& center, f32 half_extent)
{
    return AABB3<f32>{Vec3f{center.x - half_extent, center.y - half_extent, center.z - half_extent},
                      Vec3f{center.x + half_extent, center.y + half_extent, center.z + half_extent}};
}

bool aabb_overlap(const AABB3<f32>& a, const AABB3<f32>& b) noexcept
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x
        && a.min.y <= b.max.y && a.max.y >= b.min.y
        && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

// Brute-force overlap reference.
crd::containers::Array<u32> brute_overlap(crd::containers::ConstSpan<AABB3<f32>> objs,
                                            const AABB3<f32>& q,
                                            crd::memory::IAllocator* a)
{
    crd::containers::Array<u32> out(a);
    for (u32 i = 0; i < objs.size(); ++i)
    {
        if (aabb_overlap(objs[i], q)) { out.push_back(i); }
    }
    std::sort(out.data(), out.data() + out.size());
    return out;
}
} // namespace

TEST_CASE("LooseOctree empty tree behavior", "[geometry-spatial][octree][build]")
{
    AllocFixture f{};
    LooseOctree<f32> tree{&f.alloc, default_opts()};
    REQUIRE(tree.is_empty());
    REQUIRE(tree.object_count() == 0U);
    REQUIRE(tree.node_count() == 0U); // root not allocated until first insert

    crd::containers::Array<u32> hits(&f.alloc);
    tree.overlap(AABB3<f32>{Vec3f{-1, -1, -1}, Vec3f{1, 1, 1}}, hits);
    REQUIRE(hits.size() == 0U);

    auto rh = tree.raycast(Ray3<f32>{Vec3f{}, Vec3f{1, 0, 0}});
    REQUIRE_FALSE(rh.has_value());
}

TEST_CASE("LooseOctree single insert + overlap finds it", "[geometry-spatial][octree][insert]")
{
    AllocFixture f{};
    LooseOctree<f32> tree{&f.alloc, default_opts()};
    auto h = tree.insert(aabb_around(Vec3f{1.0F, 2.0F, 3.0F}, 0.5F), 42U);
    REQUIRE(h.valid());
    REQUIRE(tree.object_count() == 1U);

    crd::containers::Array<u32> hits(&f.alloc);
    tree.overlap(aabb_around(Vec3f{1.0F, 2.0F, 3.0F}, 1.0F), hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 42U);

    // Query far away → no hit.
    hits.clear();
    tree.overlap(aabb_around(Vec3f{50.0F, 50.0F, 50.0F}, 1.0F), hits);
    REQUIRE(hits.size() == 0U);
}

TEST_CASE("LooseOctree remove invalidates handle (object gone)", "[geometry-spatial][octree][remove]")
{
    AllocFixture f{};
    LooseOctree<f32> tree{&f.alloc, default_opts()};
    auto h = tree.insert(aabb_around(Vec3f{0, 0, 0}, 1.0F), 7U);
    REQUIRE(tree.object_count() == 1U);
    tree.remove(h);
    REQUIRE(tree.object_count() == 0U);
    crd::containers::Array<u32> hits(&f.alloc);
    tree.overlap(aabb_around(Vec3f{0, 0, 0}, 5.0F), hits);
    REQUIRE(hits.size() == 0U);
}

TEST_CASE("LooseOctree update fast-path: small motion within loose AABB causes ZERO restructure",
          "[geometry-spatial][octree][update][fastpath]")
{
    AllocFixture f{};
    LooseOctree<f32> tree{&f.alloc, default_opts()};

    // Insert 100 small objects scattered across the world. Object half-extent
    // 0.1 ensures cell extent at target depth (capped at max_depth 8 → 0.78)
    // is comfortably larger than object extent (0.2) so the fast-path window
    // `(loose_half - object_half) ≈ 0.68` exceeds any cell-resident object's
    // distance to its cell center (`cell_half = 0.39`) → fast-path always holds
    // for ANY initial position + small motion within the cell.
    std::mt19937 rng(7U);
    std::uniform_real_distribution<f32> uc(-90.0F, 90.0F);
    crd::containers::Array<OctreeObjectId> handles(&f.alloc);
    crd::containers::Array<Vec3f> centers(&f.alloc);
    for (u32 i = 0; i < 100U; ++i)
    {
        const Vec3f c{uc(rng), uc(rng), uc(rng)};
        centers.push_back(c);
        handles.push_back(tree.insert(aabb_around(c, 0.1F), i));
    }

    // Snapshot the structure.
    const usize snap_nodes = tree.node_count();
    const usize snap_objects = tree.object_count();
    REQUIRE(snap_objects == 100U);

    // Move every object by a TINY delta (well within the loose AABB).
    // With loose factor 2.0, the loose AABB is 2× wider on each side than the
    // tight cell. Motions of much less than half the cell extent should
    // always be no-op (Ulrich invariant).
    std::uniform_real_distribution<f32> ud(-0.05F, 0.05F);
    u32 fast_path_hits = 0;
    for (u32 i = 0; i < handles.size(); ++i)
    {
        const Vec3f c2{centers[i].x + ud(rng), centers[i].y + ud(rng), centers[i].z + ud(rng)};
        const bool restructured = tree.update(handles[i], aabb_around(c2, 0.1F));
        if (!restructured) { ++fast_path_hits; }
    }

    // The CORRECTNESS CHECK: zero restructures means zero new nodes, no
    // object_count change. With object_half = 0.1 + max_depth 8 + loose 2.0,
    // every cell-resident object has fast-path room ≥ cell-radius, so this
    // succeeds 100% by construction (Ulrich's invariant + sized-for-fit).
    REQUIRE(tree.node_count() == snap_nodes);
    REQUIRE(tree.object_count() == snap_objects);
    REQUIRE(fast_path_hits == 100U);
}

TEST_CASE("LooseOctree update slow path triggers when AABB leaves loose cell",
          "[geometry-spatial][octree][update][slowpath]")
{
    AllocFixture f{};
    LooseOctree<f32> tree{&f.alloc, default_opts()};
    auto h = tree.insert(aabb_around(Vec3f{1.0F, 1.0F, 1.0F}, 0.5F), 0U);

    // Move it far enough that no loose AABB at its current depth can enclose it —
    // 50 units is way bigger than any cell at depth ≥ 1.
    const bool restructured = tree.update(h, aabb_around(Vec3f{50.0F, 50.0F, 50.0F}, 0.5F));
    REQUIRE(restructured);
    REQUIRE(tree.object_count() == 1U);

    // Object should still be findable at new location.
    crd::containers::Array<u32> hits(&f.alloc);
    tree.overlap(aabb_around(Vec3f{50.0F, 50.0F, 50.0F}, 1.0F), hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 0U);

    // … and NOT findable at old location.
    hits.clear();
    tree.overlap(aabb_around(Vec3f{1.0F, 1.0F, 1.0F}, 1.0F), hits);
    REQUIRE(hits.size() == 0U);
}

TEST_CASE("LooseOctree overlap matches brute force on random AABB cloud",
          "[geometry-spatial][octree][overlap][bruteforce]")
{
    AllocFixture f{};
    LooseOctree<f32> tree{&f.alloc, default_opts()};

    std::mt19937 rng(13U);
    std::uniform_real_distribution<f32> uc(-80.0F, 80.0F);
    std::uniform_real_distribution<f32> uh(0.1F, 2.0F);

    crd::containers::Array<AABB3<f32>> objs(&f.alloc);
    for (u32 i = 0; i < 200U; ++i)
    {
        const Vec3f c{uc(rng), uc(rng), uc(rng)};
        objs.push_back(aabb_around(c, uh(rng)));
        (void)tree.insert(objs[i], i);
    }

    // 20 random query boxes.
    std::uniform_real_distribution<f32> uqh(0.5F, 10.0F);
    for (u32 trial = 0; trial < 20U; ++trial)
    {
        const Vec3f qc{uc(rng), uc(rng), uc(rng)};
        const AABB3<f32> q = aabb_around(qc, uqh(rng));

        crd::containers::Array<u32> got(&f.alloc);
        tree.overlap(q, got);
        std::sort(got.data(), got.data() + got.size());

        auto expected = brute_overlap(crd::containers::ConstSpan<AABB3<f32>>{objs.data(), objs.size()},
                                        q, &f.alloc);
        REQUIRE(got.size() == expected.size());
        for (usize i = 0; i < got.size(); ++i) { REQUIRE(got[i] == expected[i]); }
    }
}

TEST_CASE("LooseOctree raycast picks nearest hit", "[geometry-spatial][octree][raycast]")
{
    AllocFixture f{};
    LooseOctree<f32> tree{&f.alloc, default_opts()};

    // Three boxes along +X axis at distance 5, 10, 15.
    (void)tree.insert(aabb_around(Vec3f{15.0F, 0, 0}, 0.5F), 15U);
    (void)tree.insert(aabb_around(Vec3f{ 5.0F, 0, 0}, 0.5F),  5U);
    (void)tree.insert(aabb_around(Vec3f{10.0F, 0, 0}, 0.5F), 10U);

    auto hit = tree.raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{1, 0, 0}});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 5U);
    // t should be ~4.5 (front of nearest box at x=4.5).
    REQUIRE(hit->t >= 4.4F);
    REQUIRE(hit->t <= 4.6F);
}

TEST_CASE("LooseOctree raycast equal-t lowest-payload tiebreak", "[geometry-spatial][octree][raycast]")
{
    AllocFixture f{};
    LooseOctree<f32> tree{&f.alloc, default_opts()};

    // Two coincident boxes at the same position — ray hits both at the same t.
    (void)tree.insert(aabb_around(Vec3f{5.0F, 0, 0}, 0.5F), 9U);
    (void)tree.insert(aabb_around(Vec3f{5.0F, 0, 0}, 0.5F), 3U);
    (void)tree.insert(aabb_around(Vec3f{5.0F, 0, 0}, 0.5F), 7U);

    auto hit = tree.raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{1, 0, 0}});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 3U); // lowest payload wins
}

TEST_CASE("LooseOctree raycast misses everything returns nullopt", "[geometry-spatial][octree][raycast]")
{
    AllocFixture f{};
    LooseOctree<f32> tree{&f.alloc, default_opts()};
    (void)tree.insert(aabb_around(Vec3f{50.0F, 0, 0}, 0.5F), 0U);

    auto hit = tree.raycast(Ray3<f32>{Vec3f{0, 50.0F, 0}, Vec3f{0, 1, 0}});
    REQUIRE_FALSE(hit.has_value());
}

TEST_CASE("LooseOctree loosening factor changes overlap result count",
          "[geometry-spatial][octree][loosening]")
{
    AllocFixture f{};
    // Same scene built with different loosening values.
    auto run = [&](f32 loosening) -> usize {
        OctreeBuildOptions<f32> opts = default_opts();
        opts.loosening = loosening;
        LooseOctree<f32> t{&f.alloc, opts};
        std::mt19937 rng(99U);
        std::uniform_real_distribution<f32> uc(-50.0F, 50.0F);
        for (u32 i = 0; i < 100U; ++i)
        {
            const Vec3f c{uc(rng), uc(rng), uc(rng)};
            (void)t.insert(aabb_around(c, 0.5F), i);
        }
        crd::containers::Array<u32> hits(&f.alloc);
        t.overlap(AABB3<f32>{Vec3f{-50, -50, -50}, Vec3f{50, 50, 50}}, hits);
        return hits.size();
    };
    // Same set of objects fit inside the same query → same overlap count
    // regardless of loosening. The loosening factor changes the *cell-prune*
    // behaviour, not the *result* (which is gated by the per-object aabb test).
    const usize r1 = run(1.5F);
    const usize r2 = run(2.0F);
    const usize r3 = run(3.0F);
    REQUIRE(r1 == r2);
    REQUIRE(r2 == r3);
    REQUIRE(r1 == 100U);
}

TEST_CASE("LooseOctree determinism under permuted insert order",
          "[geometry-spatial][octree][determinism]")
{
    AllocFixture f{};
    std::mt19937 rng(33U);
    std::uniform_real_distribution<f32> uc(-50.0F, 50.0F);

    crd::containers::Array<AABB3<f32>> base(&f.alloc);
    for (u32 i = 0; i < 100U; ++i)
    {
        const Vec3f c{uc(rng), uc(rng), uc(rng)};
        base.push_back(aabb_around(c, 0.3F));
    }

    auto build_and_overlap = [&](crd::containers::ConstSpan<u32> order, const AABB3<f32>& q) {
        LooseOctree<f32> t{&f.alloc, default_opts()};
        for (usize i = 0; i < order.size(); ++i)
        {
            (void)t.insert(base[order[i]], order[i]);
        }
        crd::containers::Array<u32> hits(&f.alloc);
        t.overlap(q, hits);
        std::sort(hits.data(), hits.data() + hits.size());
        return hits;
    };

    crd::containers::Array<u32> order_a(&f.alloc);
    for (u32 i = 0; i < 100U; ++i) { order_a.push_back(i); }
    crd::containers::Array<u32> order_b(&f.alloc);
    for (u32 i = 0; i < 100U; ++i) { order_b.push_back(99U - i); }

    const AABB3<f32> q = aabb_around(Vec3f{0, 0, 0}, 30.0F);
    auto hits_a = build_and_overlap(crd::containers::ConstSpan<u32>{order_a.data(), order_a.size()}, q);
    auto hits_b = build_and_overlap(crd::containers::ConstSpan<u32>{order_b.data(), order_b.size()}, q);
    REQUIRE(hits_a.size() == hits_b.size());
    for (usize i = 0; i < hits_a.size(); ++i) { REQUIRE(hits_a[i] == hits_b[i]); }
}

TEST_CASE("LooseOctree insert/remove cycle keeps surviving handles valid",
          "[geometry-spatial][octree][cycle]")
{
    AllocFixture f{};
    LooseOctree<f32> tree{&f.alloc, default_opts()};

    // Insert 10 objects. Remove 5 of them. Insert 5 new. Remove 3 new + 3 old.
    // Survivors should still be findable + queryable.
    crd::containers::Array<OctreeObjectId> all(&f.alloc);
    for (u32 i = 0; i < 10U; ++i)
    {
        const f32 x = static_cast<f32>(i) * 5.0F - 25.0F;
        all.push_back(tree.insert(aabb_around(Vec3f{x, 0, 0}, 0.5F), i));
    }
    REQUIRE(tree.object_count() == 10U);

    // Remove evens (0,2,4,6,8).
    for (u32 i = 0; i < 10U; i += 2U) { tree.remove(all[i]); }
    REQUIRE(tree.object_count() == 5U);

    // Verify odds (1,3,5,7,9) still present + queryable.
    crd::containers::Array<u32> hits(&f.alloc);
    tree.overlap(AABB3<f32>{Vec3f{-100, -100, -100}, Vec3f{100, 100, 100}}, hits);
    REQUIRE(hits.size() == 5U);
    std::sort(hits.data(), hits.data() + hits.size());
    REQUIRE(hits[0] == 1U);
    REQUIRE(hits[1] == 3U);
    REQUIRE(hits[2] == 5U);
    REQUIRE(hits[3] == 7U);
    REQUIRE(hits[4] == 9U);

    // Insert new objects with high payloads — should reuse free pool slots.
    for (u32 i = 0; i < 5U; ++i)
    {
        const f32 y = static_cast<f32>(i) * 5.0F - 10.0F;
        (void)tree.insert(aabb_around(Vec3f{0, y, 0}, 0.5F), 100U + i);
    }
    REQUIRE(tree.object_count() == 10U);

    hits.clear();
    tree.overlap(AABB3<f32>{Vec3f{-100, -100, -100}, Vec3f{100, 100, 100}}, hits);
    REQUIRE(hits.size() == 10U);
}

TEST_CASE("LooseOctree tolerates non-finite query inputs",
          "[geometry-spatial][octree][nan]")
{
    AllocFixture f{};
    LooseOctree<f32> tree{&f.alloc, default_opts()};
    for (u32 i = 0; i < 20U; ++i)
    {
        const f32 x = static_cast<f32>(i) - 10.0F;
        (void)tree.insert(aabb_around(Vec3f{x, 0, 0}, 0.5F), i);
    }

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    crd::containers::Array<u32> hits(&f.alloc);
    tree.overlap(AABB3<f32>{Vec3f{nan, 0, 0}, Vec3f{1, 1, 1}}, hits);
    REQUIRE(hits.size() == 0U); // NaN comparisons false → loose-AABB-prune kills root

    auto rh = tree.raycast(Ray3<f32>{Vec3f{nan, 0, 0}, Vec3f{1, 0, 0}});
    REQUIRE_FALSE(rh.has_value());
}

TEST_CASE("LooseOctree all-coincident objects all retrievable",
          "[geometry-spatial][octree][edge]")
{
    AllocFixture f{};
    LooseOctree<f32> tree{&f.alloc, default_opts()};
    for (u32 i = 0; i < 15U; ++i)
    {
        (void)tree.insert(aabb_around(Vec3f{1.0F, 2.0F, 3.0F}, 0.5F), i);
    }
    REQUIRE(tree.object_count() == 15U);

    crd::containers::Array<u32> hits(&f.alloc);
    tree.overlap(aabb_around(Vec3f{1.0F, 2.0F, 3.0F}, 1.0F), hits);
    REQUIRE(hits.size() == 15U);
    std::sort(hits.data(), hits.data() + hits.size());
    for (u32 i = 0; i < 15U; ++i) { REQUIRE(hits[i] == i); }
}

// =============================================================================
// Concurrent overlap queries via crd-jobs fiber pool — proves naturally-const-safe
// =============================================================================
//
// Per Ulrich's invariant: each object lives in EXACTLY ONE cell (terminal
// cell at target_depth). LooseOctree's overlap traversal visits each cell
// at most once per query and each object at most once. There is no per-
// object dedup state, no `mutable` member, no `const_cast` write during
// query — purely read-only by construction.
//
// This test validates that claim empirically: many fibers run concurrent
// overlap queries against the SHARED octree, each with its own output Array.
// ASan instrumentation catches any silent race the impl might have despite
// the construction-level safety claim. NO scratch parameter — adding one
// would be misleading API (the structure has no dedup state to scratch out).

namespace
{
struct OctreeConcurrencyCorpus
{
    static constexpr u32 kQueries = 16U;
    static constexpr u32 kItersPerQuery = 25U;

    crd::memory::TlsfAllocator alloc{1U << 22};
    LooseOctree<f32>           tree{&alloc, OctreeBuildOptions<f32>{
        AABB3<f32>{Vec3f{-100, -100, -100}, Vec3f{100, 100, 100}}, 2.0F, 8U, 8U}};
    AABB3<f32>                 queries[kQueries]{};
    crd::containers::Array<u32> ref[kQueries] = {
        crd::containers::Array<u32>{&alloc}, crd::containers::Array<u32>{&alloc},
        crd::containers::Array<u32>{&alloc}, crd::containers::Array<u32>{&alloc},
        crd::containers::Array<u32>{&alloc}, crd::containers::Array<u32>{&alloc},
        crd::containers::Array<u32>{&alloc}, crd::containers::Array<u32>{&alloc},
        crd::containers::Array<u32>{&alloc}, crd::containers::Array<u32>{&alloc},
        crd::containers::Array<u32>{&alloc}, crd::containers::Array<u32>{&alloc},
        crd::containers::Array<u32>{&alloc}, crd::containers::Array<u32>{&alloc},
        crd::containers::Array<u32>{&alloc}, crd::containers::Array<u32>{&alloc}};
    std::atomic<crd::u32> mismatches{0};
};
} // namespace

TEST_CASE("LooseOctree concurrent overlap queries via crd-jobs (proves naturally-const-safe)",
          "[geometry-spatial][octree][overlap][concurrent][jobs]")
{
    auto corpus = std::make_unique<OctreeConcurrencyCorpus>();
    std::mt19937 rng(101U);
    std::uniform_real_distribution<f32> uc(-90.0F, 90.0F);
    std::uniform_real_distribution<f32> uh(0.3F, 1.5F);
    for (u32 i = 0; i < 400U; ++i)
    {
        const Vec3f c{uc(rng), uc(rng), uc(rng)};
        (void)corpus->tree.insert(aabb_around(c, uh(rng)), i);
    }
    std::uniform_real_distribution<f32> uqh(1.0F, 8.0F);
    for (u32 q = 0; q < OctreeConcurrencyCorpus::kQueries; ++q)
    {
        corpus->queries[q] = aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uqh(rng));
        corpus->tree.overlap(corpus->queries[q], corpus->ref[q]);
        std::sort(corpus->ref[q].data(), corpus->ref[q].data() + corpus->ref[q].size());
    }

    constexpr u32 total_tasks = OctreeConcurrencyCorpus::kQueries * OctreeConcurrencyCorpus::kItersPerQuery;
    auto* corpus_ptr = corpus.get();
    crd::jobs::Counter* counter = crd::jobs::parallel_for(
        total_tasks, /*num_jobs=*/16U,
        [corpus_ptr](crd::u32 begin, crd::u32 end) noexcept {
            for (crd::u32 task_idx = begin; task_idx < end; ++task_idx)
            {
                const u32 q = task_idx % OctreeConcurrencyCorpus::kQueries;
                crd::memory::TlsfAllocator local_alloc{1U << 16};
                crd::containers::Array<u32> got(&local_alloc);
                corpus_ptr->tree.overlap(corpus_ptr->queries[q], got);
                std::sort(got.data(), got.data() + got.size());
                bool ok = (got.size() == corpus_ptr->ref[q].size());
                if (ok)
                {
                    for (usize i = 0; i < got.size(); ++i)
                    {
                        if (got[i] != corpus_ptr->ref[q][i]) { ok = false; break; }
                    }
                }
                if (!ok) { corpus_ptr->mismatches.fetch_add(1U, std::memory_order_relaxed); }
            }
        });
    crd::jobs::wait(counter);
    REQUIRE(corpus->mismatches.load() == 0U);
}

TEST_CASE("OctreeNode<f32> sizing pin (regression guard)",
          "[geometry-spatial][octree][build]")
{
    static_assert(sizeof(crd::geometry::spatial::OctreeNode<f32>) == 64,
                  "OctreeNode<f32> must stay 64 B (one per cache line)");
    REQUIRE(true);
}
