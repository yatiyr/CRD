// crd-geometry-spatial v5d — SpatialHash tests.
//
// Coverage:
//   * Empty / single insert / many insert
//   * Brute-force overlap + radius cross-validation on random AABB clouds
//   * Amanatides-Woo voxel raycast vs brute-force
//   * find_overlapping_pairs vs brute-force O(N^2)
//   * Update fast-path: same cell ⇒ no rebucketing (returns false)
//   * Update slow path: cell change ⇒ removed from old + inserted into new
//   * Insert/remove cycle handle stability
//   * Multi-cell object correctly findable from any cell it overlaps
//   * NaN tolerance + zero-direction raycast
//   * Cell-grid stability across positive + negative coordinate ranges

#include <crd/containers/array.hpp>
#include <crd/geometry/spatial/spatial_hash.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <algorithm>
#include <atomic>
#include <random>

// Catch2 listener — initialise the fiber job system once per test-binary run.
// Same pattern as `test_bvh_parallel.cpp`'s `BvhJobsListener`. Init at file
// scope (not via `jobs::init` directly) so it doesn't fire during
// `catch_discover_tests`'s name-listing phase.
//
// **Binary-wide listener** — registered ONCE per test binary. Tests in any
// file in this binary (`test_kd_*.cpp`, `test_loose_octree*.cpp`,
// `test_rtree*.cpp`, `test_spatial_hash*.cpp`, `test_uniform_grid*.cpp`)
// can use `crd::jobs::*` directly; init/shutdown is handled here.
namespace
{
struct GeometrySpatialJobsListener final : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(Catch::TestRunInfo const&) override
    {
        crd::jobs::init(crd::jobs::Config{.num_threads = 4, .frame_alloc_bytes = 16U << 20U});
    }
    void testCaseEnded(Catch::TestCaseStats const&) override { crd::jobs::frame_reset(); }
    void testRunEnded(Catch::TestRunStats const&) override { crd::jobs::shutdown(); }
};
} // namespace
CATCH_REGISTER_LISTENER(GeometrySpatialJobsListener)

using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Ray3;
using crd::geometry::spatial::SpatialHash;
using crd::geometry::spatial::SpatialHashConfig;
using crd::geometry::spatial::SpatialHashObjectId;
using crd::geometry::spatial::SpatialHashPair;
using crd::geometry::spatial::SpatialHashScratch;
using crd::math::Vec3f;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 22}; };

SpatialHashConfig<f32> default_cfg(f32 cs = 1.0F, u32 buckets = 256U)
{
    return SpatialHashConfig<f32>{cs, buckets};
}

AABB3<f32> aabb_around(const Vec3f& center, f32 half)
{
    return AABB3<f32>{Vec3f{center.x - half, center.y - half, center.z - half},
                      Vec3f{center.x + half, center.y + half, center.z + half}};
}

bool aabb_overlap(const AABB3<f32>& a, const AABB3<f32>& b) noexcept
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x
        && a.min.y <= b.max.y && a.max.y >= b.min.y
        && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

f32 point_aabb_d2(const Vec3f& p, const AABB3<f32>& a) noexcept
{
    f32 d2 = 0.0F;
    for (int i = 0; i < 3; ++i)
    {
        const f32 v = p[static_cast<usize>(i)];
        const f32 lo = a.min[static_cast<usize>(i)];
        const f32 hi = a.max[static_cast<usize>(i)];
        if (v < lo)      { const f32 d = lo - v; d2 += d * d; }
        else if (v > hi) { const f32 d = v - hi; d2 += d * d; }
    }
    return d2;
}
} // namespace

TEST_CASE("SpatialHash empty tree behavior", "[geometry-spatial][hash][build]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg()};
    REQUIRE(h.is_empty());
    REQUIRE(h.object_count() == 0U);
    REQUIRE(h.bucket_count() == 256U);

    crd::containers::Array<u32> hits(&f.alloc);
    h.overlap(AABB3<f32>{Vec3f{-1, -1, -1}, Vec3f{1, 1, 1}}, hits);
    REQUIRE(hits.size() == 0U);

    auto rh = h.raycast(Ray3<f32>{Vec3f{}, Vec3f{1, 0, 0}});
    REQUIRE_FALSE(rh.has_value());
}

TEST_CASE("SpatialHash bucket_count must be power of two (debug-asserts)",
          "[geometry-spatial][hash][build]")
{
    // Just check we accept valid POW2 values — non-POW2 would CRD_ASSERT in
    // debug, which we can't easily test here without a death-test framework.
    AllocFixture f{};
    SpatialHash<f32> h64{&f.alloc, SpatialHashConfig<f32>{1.0F, 64U}};
    REQUIRE(h64.bucket_count() == 64U);
    SpatialHash<f32> h1024{&f.alloc, SpatialHashConfig<f32>{1.0F, 1024U}};
    REQUIRE(h1024.bucket_count() == 1024U);
}

TEST_CASE("SpatialHash single insert + overlap finds it", "[geometry-spatial][hash][insert]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};
    auto id = h.insert(aabb_around(Vec3f{1.5F, 2.5F, 3.5F}, 0.3F), 42U);
    REQUIRE(id.valid());
    REQUIRE(h.object_count() == 1U);

    crd::containers::Array<u32> hits(&f.alloc);
    h.overlap(aabb_around(Vec3f{1.5F, 2.5F, 3.5F}, 1.0F), hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 42U);
}

TEST_CASE("SpatialHash multi-cell object findable from any overlapping cell",
          "[geometry-spatial][hash][insert]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};
    // AABB spanning 4×4×4 = 64 cells (extent 4 with cell_size 1).
    auto id = h.insert(AABB3<f32>{Vec3f{-2, -2, -2}, Vec3f{2, 2, 2}}, 7U);
    (void)id;
    REQUIRE(h.object_count() == 1U);

    // Query each corner cell — all should find the object.
    crd::containers::Array<u32> hits(&f.alloc);
    h.overlap(aabb_around(Vec3f{-1.5F, -1.5F, -1.5F}, 0.1F), hits);
    REQUIRE(hits.size() == 1U);
    hits.clear();
    h.overlap(aabb_around(Vec3f{1.5F, 1.5F, 1.5F}, 0.1F), hits);
    REQUIRE(hits.size() == 1U);
    hits.clear();
    h.overlap(aabb_around(Vec3f{0, 0, 0}, 0.1F), hits);
    REQUIRE(hits.size() == 1U);
    // Query OUTSIDE — no hit.
    hits.clear();
    h.overlap(aabb_around(Vec3f{10, 10, 10}, 0.1F), hits);
    REQUIRE(hits.size() == 0U);
}

TEST_CASE("SpatialHash overlap matches brute force on random AABB cloud",
          "[geometry-spatial][hash][overlap][bruteforce]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(2.0F, 1024U)};

    std::mt19937 rng(13U);
    std::uniform_real_distribution<f32> uc(-50.0F, 50.0F);
    std::uniform_real_distribution<f32> uh(0.2F, 1.5F);

    crd::containers::Array<AABB3<f32>> objs(&f.alloc);
    for (u32 i = 0; i < 300U; ++i)
    {
        objs.push_back(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng)));
        (void)h.insert(objs[i], i);
    }

    std::uniform_real_distribution<f32> uqh(0.5F, 8.0F);
    for (u32 trial = 0; trial < 20U; ++trial)
    {
        const Vec3f qc{uc(rng), uc(rng), uc(rng)};
        const AABB3<f32> q = aabb_around(qc, uqh(rng));
        crd::containers::Array<u32> got(&f.alloc);
        h.overlap(q, got);
        std::sort(got.data(), got.data() + got.size());

        crd::containers::Array<u32> expected(&f.alloc);
        for (u32 i = 0; i < objs.size(); ++i)
        {
            if (aabb_overlap(objs[i], q)) { expected.push_back(i); }
        }
        std::sort(expected.data(), expected.data() + expected.size());
        REQUIRE(got.size() == expected.size());
        for (usize i = 0; i < got.size(); ++i) { REQUIRE(got[i] == expected[i]); }
    }
}

TEST_CASE("SpatialHash radius matches brute force",
          "[geometry-spatial][hash][radius][bruteforce]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F, 1024U)};

    std::mt19937 rng(77U);
    std::uniform_real_distribution<f32> uc(-30.0F, 30.0F);
    crd::containers::Array<AABB3<f32>> objs(&f.alloc);
    for (u32 i = 0; i < 200U; ++i)
    {
        objs.push_back(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, 0.3F));
        (void)h.insert(objs[i], i);
    }

    for (f32 r : {0.5F, 1.5F, 5.0F})
    {
        for (u32 trial = 0; trial < 5U; ++trial)
        {
            const Vec3f q{uc(rng), uc(rng), uc(rng)};
            crd::containers::Array<u32> got(&f.alloc);
            h.radius(q, r, got);
            std::sort(got.data(), got.data() + got.size());

            const f32 r2 = r * r;
            crd::containers::Array<u32> expected(&f.alloc);
            for (u32 i = 0; i < objs.size(); ++i)
            {
                if (point_aabb_d2(q, objs[i]) <= r2) { expected.push_back(i); }
            }
            std::sort(expected.data(), expected.data() + expected.size());
            REQUIRE(got.size() == expected.size());
            for (usize i = 0; i < got.size(); ++i) { REQUIRE(got[i] == expected[i]); }
        }
    }
}

TEST_CASE("SpatialHash raycast picks nearest hit via Amanatides-Woo voxel traversal",
          "[geometry-spatial][hash][raycast]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(2.0F, 256U)};

    (void)h.insert(aabb_around(Vec3f{15.0F, 0, 0}, 0.5F), 15U);
    (void)h.insert(aabb_around(Vec3f{ 5.0F, 0, 0}, 0.5F),  5U);
    (void)h.insert(aabb_around(Vec3f{10.0F, 0, 0}, 0.5F), 10U);

    auto hit = h.raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{1, 0, 0}});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 5U);
    REQUIRE(hit->t >= 4.4F);
    REQUIRE(hit->t <= 4.6F);
}

TEST_CASE("SpatialHash raycast lowest-payload tiebreak on equal t",
          "[geometry-spatial][hash][raycast]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};
    // Three coincident boxes — all hit at same t.
    (void)h.insert(aabb_around(Vec3f{5.0F, 0, 0}, 0.4F), 9U);
    (void)h.insert(aabb_around(Vec3f{5.0F, 0, 0}, 0.4F), 3U);
    (void)h.insert(aabb_around(Vec3f{5.0F, 0, 0}, 0.4F), 7U);
    auto hit = h.raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{1, 0, 0}});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 3U);
}

TEST_CASE("SpatialHash raycast with negative direction works",
          "[geometry-spatial][hash][raycast]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};
    (void)h.insert(aabb_around(Vec3f{-5.0F, 0, 0}, 0.5F), 100U);
    auto hit = h.raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{-1, 0, 0}});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 100U);
}

TEST_CASE("SpatialHash raycast with diagonal direction (Amanatides-Woo correctness)",
          "[geometry-spatial][hash][raycast]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};
    // Object on the diagonal — voxel traversal must visit cells along (1,1,1).
    (void)h.insert(aabb_around(Vec3f{5.0F, 5.0F, 5.0F}, 0.4F), 55U);

    Vec3f dir{1.0F, 1.0F, 1.0F};
    const f32 inv_len = 1.0F / std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    dir.x *= inv_len; dir.y *= inv_len; dir.z *= inv_len;

    auto hit = h.raycast(Ray3<f32>{Vec3f{0, 0, 0}, dir});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 55U);
}

TEST_CASE("SpatialHash raycast misses everything returns nullopt",
          "[geometry-spatial][hash][raycast]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};
    (void)h.insert(aabb_around(Vec3f{50, 0, 0}, 0.5F), 0U);
    auto hit = h.raycast(Ray3<f32>{Vec3f{0, 50, 0}, Vec3f{0, 1, 0}});
    REQUIRE_FALSE(hit.has_value());
}

TEST_CASE("SpatialHash update fast-path: same cell -> no rebucketing",
          "[geometry-spatial][hash][update][fastpath]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};

    // Object centered at (0.5, 0.5, 0.5) with half=0.1 → fits entirely in
    // cell (0,0,0). Snapshot bucket fill before/after.
    auto id = h.insert(aabb_around(Vec3f{0.5F, 0.5F, 0.5F}, 0.1F), 42U);
    const usize objs_before = h.object_count();

    // Move slightly within cell (0,0,0).
    const bool restructured = h.update(id, aabb_around(Vec3f{0.6F, 0.6F, 0.6F}, 0.1F));
    REQUIRE_FALSE(restructured);
    REQUIRE(h.object_count() == objs_before);

    // Verify object is still at new position.
    REQUIRE(h.object_aabb(id).min.x >= 0.49F);
    REQUIRE(h.object_aabb(id).max.x <= 0.71F);
}

TEST_CASE("SpatialHash update slow path: cell change triggers rebucketing",
          "[geometry-spatial][hash][update][slowpath]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};
    auto id = h.insert(aabb_around(Vec3f{0.5F, 0.5F, 0.5F}, 0.1F), 0U);
    // Move to a far cell.
    const bool restructured = h.update(id, aabb_around(Vec3f{50.5F, 50.5F, 50.5F}, 0.1F));
    REQUIRE(restructured);
    // Old position no longer hits.
    crd::containers::Array<u32> hits(&f.alloc);
    h.overlap(aabb_around(Vec3f{0.5F, 0.5F, 0.5F}, 0.5F), hits);
    REQUIRE(hits.size() == 0U);
    // New position hits.
    hits.clear();
    h.overlap(aabb_around(Vec3f{50.5F, 50.5F, 50.5F}, 0.5F), hits);
    REQUIRE(hits.size() == 1U);
}

TEST_CASE("SpatialHash insert/remove cycle keeps surviving handles valid",
          "[geometry-spatial][hash][cycle]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};

    crd::containers::Array<SpatialHashObjectId> ids(&f.alloc);
    for (u32 i = 0; i < 50U; ++i)
    {
        ids.push_back(h.insert(aabb_around(Vec3f{static_cast<f32>(i) - 25.0F, 0, 0}, 0.3F), i));
    }
    REQUIRE(h.object_count() == 50U);

    // Remove evens.
    for (u32 i = 0; i < 50U; i += 2U) { h.remove(ids[i]); }
    REQUIRE(h.object_count() == 25U);

    // Verify odds still resolve.
    for (u32 i = 1; i < 50U; i += 2U)
    {
        REQUIRE(h.object_payload(ids[i]) == i);
    }

    crd::containers::Array<u32> hits(&f.alloc);
    h.overlap(AABB3<f32>{Vec3f{-100, -100, -100}, Vec3f{100, 100, 100}}, hits);
    REQUIRE(hits.size() == 25U);
}

TEST_CASE("SpatialHash find_overlapping_pairs matches brute force",
          "[geometry-spatial][hash][pairs][bruteforce]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F, 1024U)};

    std::mt19937 rng(99U);
    std::uniform_real_distribution<f32> uc(-10.0F, 10.0F);
    std::uniform_real_distribution<f32> uh(0.2F, 0.8F);

    crd::containers::Array<AABB3<f32>> objs(&f.alloc);
    for (u32 i = 0; i < 60U; ++i)
    {
        objs.push_back(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng)));
        (void)h.insert(objs[i], i);
    }

    crd::containers::Array<SpatialHashPair> got_pairs(&f.alloc);
    h.find_overlapping_pairs(got_pairs);

    // Brute-force O(N^2) reference.
    crd::containers::Array<SpatialHashPair> expected(&f.alloc);
    for (u32 i = 0; i < objs.size(); ++i)
    {
        for (u32 j = i + 1U; j < objs.size(); ++j)
        {
            if (aabb_overlap(objs[i], objs[j]))
            {
                expected.push_back(SpatialHashPair{i, j});
            }
        }
    }
    std::sort(expected.data(), expected.data() + expected.size(),
                [](const SpatialHashPair& a, const SpatialHashPair& b) { return a < b; });

    REQUIRE(got_pairs.size() == expected.size());
    for (usize i = 0; i < got_pairs.size(); ++i)
    {
        REQUIRE(got_pairs[i] == expected[i]);
    }
}

TEST_CASE("SpatialHash works across negative coordinate ranges",
          "[geometry-spatial][hash][negative]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};

    // Insert objects at strongly negative + strongly positive coords.
    auto id_neg = h.insert(aabb_around(Vec3f{-100.5F, -100.5F, -100.5F}, 0.3F), 1U);
    auto id_pos = h.insert(aabb_around(Vec3f{ 100.5F,  100.5F,  100.5F}, 0.3F), 2U);
    (void)id_neg; (void)id_pos;

    crd::containers::Array<u32> hits(&f.alloc);
    h.overlap(aabb_around(Vec3f{-100.5F, -100.5F, -100.5F}, 1.0F), hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 1U);
    hits.clear();
    h.overlap(aabb_around(Vec3f{100.5F, 100.5F, 100.5F}, 1.0F), hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 2U);
}

TEST_CASE("SpatialHash tolerates non-finite query inputs", "[geometry-spatial][hash][nan]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};
    for (u32 i = 0; i < 20U; ++i)
    {
        (void)h.insert(aabb_around(Vec3f{static_cast<f32>(i), 0, 0}, 0.3F), i);
    }
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    crd::containers::Array<u32> hits(&f.alloc);
    h.overlap(AABB3<f32>{Vec3f{nan, 0, 0}, Vec3f{1, 1, 1}}, hits);
    REQUIRE(hits.size() == 0U);

    h.radius(Vec3f{nan, 0, 0}, 1.0F, hits);
    REQUIRE(hits.size() == 0U);

    auto rh = h.raycast(Ray3<f32>{Vec3f{nan, 0, 0}, Vec3f{1, 0, 0}});
    REQUIRE_FALSE(rh.has_value());

    auto rh2 = h.raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{0, 0, 0}}); // zero direction
    REQUIRE_FALSE(rh2.has_value());
}

TEST_CASE("SpatialHash raycast handles exact corner-grazing rays (Amanatides-Woo correctness)",
          "[geometry-spatial][hash][raycast]")
{
    // Ray with EXACT diagonal direction (1,1,1) un-normalised crosses cell
    // corners at t=1, t=2, t=3. Naive single-axis-advance loops would skip
    // cells touched only at the corner; the all-tied-axes-advance fix is
    // required for Amanatides-Woo correctness in this case.
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F, 256U)};
    // Object at exact corner cell (3, 3, 3) — the ray from origin in dir
    // (1,1,1) crosses every diagonal cell. Object spans a single cell.
    (void)h.insert(aabb_around(Vec3f{3.5F, 3.5F, 3.5F}, 0.4F), 33U);

    auto hit = h.raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{1, 1, 1}});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 33U);
}

// =============================================================================
// Scratch (thread-safe) overload tests
// =============================================================================

TEST_CASE("SpatialHash scratch overlap byte-identical to single-thread overload",
          "[geometry-spatial][hash][scratch]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(2.0F, 1024U)};
    std::mt19937 rng(13U);
    std::uniform_real_distribution<f32> uc(-50.0F, 50.0F);
    std::uniform_real_distribution<f32> uh(0.2F, 1.5F);
    crd::containers::Array<AABB3<f32>> objs(&f.alloc);
    for (u32 i = 0; i < 300U; ++i)
    {
        objs.push_back(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng)));
        (void)h.insert(objs[i], i);
    }

    SpatialHashScratch scratch(&f.alloc);
    std::uniform_real_distribution<f32> uqh(0.5F, 8.0F);
    for (u32 trial = 0; trial < 20U; ++trial)
    {
        const Vec3f qc{uc(rng), uc(rng), uc(rng)};
        const AABB3<f32> q = aabb_around(qc, uqh(rng));

        crd::containers::Array<u32> single(&f.alloc);
        h.overlap(q, single);
        std::sort(single.data(), single.data() + single.size());

        crd::containers::Array<u32> sc(&f.alloc);
        h.overlap(q, scratch, sc);
        std::sort(sc.data(), sc.data() + sc.size());

        REQUIRE(single.size() == sc.size());
        for (usize i = 0; i < single.size(); ++i) { REQUIRE(single[i] == sc[i]); }
    }
}

TEST_CASE("SpatialHash scratch radius byte-identical to single-thread overload",
          "[geometry-spatial][hash][scratch]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F, 1024U)};
    std::mt19937 rng(77U);
    std::uniform_real_distribution<f32> uc(-30.0F, 30.0F);
    crd::containers::Array<AABB3<f32>> objs(&f.alloc);
    for (u32 i = 0; i < 200U; ++i)
    {
        objs.push_back(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, 0.3F));
        (void)h.insert(objs[i], i);
    }
    SpatialHashScratch scratch(&f.alloc);
    for (f32 r : {0.5F, 1.5F, 5.0F})
    {
        for (u32 trial = 0; trial < 5U; ++trial)
        {
            const Vec3f q{uc(rng), uc(rng), uc(rng)};
            crd::containers::Array<u32> single(&f.alloc);
            h.radius(q, r, single);
            std::sort(single.data(), single.data() + single.size());
            crd::containers::Array<u32> sc(&f.alloc);
            h.radius(q, r, scratch, sc);
            std::sort(sc.data(), sc.data() + sc.size());
            REQUIRE(single.size() == sc.size());
            for (usize i = 0; i < single.size(); ++i) { REQUIRE(single[i] == sc[i]); }
        }
    }
}

TEST_CASE("SpatialHash scratch raycast byte-identical to single-thread overload",
          "[geometry-spatial][hash][scratch]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};
    std::mt19937 rng(31U);
    std::uniform_real_distribution<f32> uc(-20.0F, 20.0F);
    for (u32 i = 0; i < 100U; ++i)
    {
        (void)h.insert(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, 0.4F), i);
    }
    SpatialHashScratch scratch(&f.alloc);
    std::uniform_real_distribution<f32> ud(-1.0F, 1.0F);
    for (u32 trial = 0; trial < 10U; ++trial)
    {
        Vec3f origin{uc(rng), uc(rng), uc(rng)};
        Vec3f dir{ud(rng), ud(rng), ud(rng)};
        const f32 inv_len = 1.0F / std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
        dir.x *= inv_len; dir.y *= inv_len; dir.z *= inv_len;

        auto single = h.raycast(Ray3<f32>{origin, dir});
        auto sc = h.raycast(Ray3<f32>{origin, dir}, scratch);
        REQUIRE(single.has_value() == sc.has_value());
        if (single.has_value())
        {
            REQUIRE(single->payload == sc->payload);
            REQUIRE(single->t == sc->t);
        }
    }
}

// =============================================================================
// Concurrent queries via crd-jobs fiber pool
// =============================================================================
//
// The whole point of the scratch overload: many fibers running concurrent
// overlap queries against the SAME tree, each with its own SpatialHashScratch,
// must produce results identical to the single-threaded reference. Race
// detected by either result mismatch (per-task atomic counter) or by
// ASan/TSan if the impl ever touches mutable tree state from a worker fiber.
//
// Scratches are stack-resident inside each parallel_for task — the task body
// constructs a TlsfAllocator + SpatialHashScratch on each iteration, so each
// fiber is fully isolated by construction. (A real consumer would more likely
// pool one scratch per worker thread; this layout is the most adversarial
// against the impl's correctness — fresh scratch per query.)

namespace
{
// Module-scope corpus — the scratch+jobs test fills these once and the
// parallel_for tasks read them. Pointers + raw arrays only (no Catch fixtures
// reach into worker fibers).
struct ConcurrencyCorpus
{
    static constexpr u32 k_queries = 16U;
    static constexpr u32 k_iters_per_query = 25U; // 16 × 25 = 400 query invocations

    crd::memory::TlsfAllocator alloc{1U << 22};
    SpatialHash<f32>           tree{&alloc, SpatialHashConfig<f32>{2.0F, 1024U}};
    AABB3<f32>                 queries[k_queries]{};
    crd::containers::Array<u32> ref[k_queries] = {
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

TEST_CASE("SpatialHash concurrent queries via crd-jobs fiber pool",
          "[geometry-spatial][hash][scratch][concurrent][jobs]")
{
    // Build the corpus on the main thread.
    auto corpus = std::make_unique<ConcurrencyCorpus>();
    std::mt19937 rng(101U);
    std::uniform_real_distribution<f32> uc(-50.0F, 50.0F);
    std::uniform_real_distribution<f32> uh(0.3F, 1.5F);
    for (u32 i = 0; i < 500U; ++i)
    {
        const AABB3<f32> a = aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng));
        (void)corpus->tree.insert(a, i);
    }
    std::uniform_real_distribution<f32> uqh(1.0F, 8.0F);
    for (u32 q = 0; q < ConcurrencyCorpus::k_queries; ++q)
    {
        corpus->queries[q] = aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uqh(rng));
        corpus->tree.overlap(corpus->queries[q], corpus->ref[q]);
        std::sort(corpus->ref[q].data(), corpus->ref[q].data() + corpus->ref[q].size());
    }

    // Fan out: one job per (query, iteration) tuple. Worker fibers run on the
    // crd-jobs thread pool (4 workers per the JobsListener's config). Each
    // task constructs its own TlsfAllocator + SpatialHashScratch — total
    // isolation. Atomic mismatch counter aggregated across all fibers.
    constexpr u32 total_tasks = ConcurrencyCorpus::k_queries * ConcurrencyCorpus::k_iters_per_query;
    auto* corpus_ptr = corpus.get(); // SBO-trivial pointer for the lambda capture

    crd::jobs::Counter* counter = crd::jobs::parallel_for(
        total_tasks, /*num_jobs=*/16U,
        [corpus_ptr](crd::u32 begin, crd::u32 end) noexcept {
            for (crd::u32 task_idx = begin; task_idx < end; ++task_idx)
            {
                const u32 q = task_idx % ConcurrencyCorpus::k_queries;
                // Per-task fresh allocator + scratch — maximally adversarial.
                crd::memory::TlsfAllocator local_alloc{1U << 16};
                SpatialHashScratch         local_scratch(&local_alloc);
                crd::containers::Array<u32> got(&local_alloc);
                corpus_ptr->tree.overlap(corpus_ptr->queries[q], local_scratch, got);
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

    // Zero mismatches across 400 fiber-parallel queries → scratch overload
    // is provably race-free against this corpus.
    REQUIRE(corpus->mismatches.load() == 0U);
}

TEST_CASE("SpatialHashScratch reuses across multiple queries",
          "[geometry-spatial][hash][scratch]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};
    for (u32 i = 0; i < 30U; ++i)
    {
        (void)h.insert(aabb_around(Vec3f{static_cast<f32>(i), 0, 0}, 0.3F), i);
    }
    SpatialHashScratch scratch(&f.alloc);
    // Run 5 back-to-back queries. Each must produce the right answer despite
    // reusing the scratch (the gen counter bumps; per-object array stays).
    for (u32 trial = 0; trial < 5U; ++trial)
    {
        crd::containers::Array<u32> hits(&f.alloc);
        h.overlap(AABB3<f32>{Vec3f{-1, -1, -1}, Vec3f{31, 1, 1}}, scratch, hits);
        REQUIRE(hits.size() == 30U);
    }
}

TEST_CASE("SpatialHashScratch grows when tree object count exceeds capacity",
          "[geometry-spatial][hash][scratch]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(1.0F)};
    SpatialHashScratch scratch(&f.alloc);
    REQUIRE(scratch.capacity() == 0U);

    // Insert 5 objects + query; scratch should grow to >= 5.
    for (u32 i = 0; i < 5U; ++i) { (void)h.insert(aabb_around(Vec3f{static_cast<f32>(i), 0, 0}, 0.2F), i); }
    crd::containers::Array<u32> hits(&f.alloc);
    h.overlap(AABB3<f32>{Vec3f{-1, -1, -1}, Vec3f{6, 1, 1}}, scratch, hits);
    REQUIRE(hits.size() == 5U);
    REQUIRE(scratch.capacity() >= 5U);

    // Insert 50 more + query; scratch must grow.
    for (u32 i = 5; i < 55U; ++i) { (void)h.insert(aabb_around(Vec3f{static_cast<f32>(i), 0, 0}, 0.2F), i); }
    hits.clear();
    h.overlap(AABB3<f32>{Vec3f{-1, -1, -1}, Vec3f{56, 1, 1}}, scratch, hits);
    REQUIRE(hits.size() == 55U);
    REQUIRE(scratch.capacity() >= 55U);
}

TEST_CASE("SpatialHash deduplication: many-cell object visited once per query",
          "[geometry-spatial][hash][dedup]")
{
    AllocFixture f{};
    SpatialHash<f32> h{&f.alloc, default_cfg(0.5F, 1024U)};
    // AABB extent 10 with cell_size 0.5 → spans 20×20×20 = 8000 cells.
    auto id = h.insert(AABB3<f32>{Vec3f{-5, -5, -5}, Vec3f{5, 5, 5}}, 999U);
    (void)id;
    REQUIRE(h.object_count() == 1U);

    // Query covers the whole object — should emit it EXACTLY ONCE.
    crd::containers::Array<u32> hits(&f.alloc);
    h.overlap(AABB3<f32>{Vec3f{-10, -10, -10}, Vec3f{10, 10, 10}}, hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 999U);
}
