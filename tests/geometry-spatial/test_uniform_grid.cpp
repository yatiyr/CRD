// crd-geometry-spatial v5e — UniformGrid tests.
//
// Coverage:
//   * Empty / single insert / many insert; bounded grid + cell sizing
//   * Brute-force overlap + radius cross-validation on random AABB clouds
//   * Grid-bounds-clipped Amanatides-Woo voxel raycast — nearest, diagonal,
//     negative direction, equal-t lowest-payload tiebreak, miss-via-grid-exit,
//     miss-no-objects, ray starts outside grid, ray starts inside grid
//   * find_overlapping_pairs vs O(N^2) brute force
//   * Update fast-path (same cell range) + slow path (cell change)
//   * Insert/remove cycle handle stability
//   * Out-of-bounds AABB clamps to grid (no insertion outside)
//   * NaN tolerance on query inputs + zero-direction ray
//   * Scratch overload byte-identical parity
//   * Concurrent queries via crd::jobs::parallel_for (the whole point)
//   * Sizing pin diagnostics

#include <crd/containers/array.hpp>
#include <crd/geometry/spatial/uniform_grid.hpp>
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
using crd::geometry::spatial::UniformGrid;
using crd::geometry::spatial::UniformGridConfig;
using crd::geometry::spatial::UniformGridObjectId;
using crd::geometry::spatial::UniformGridPair;
using crd::geometry::spatial::UniformGridScratch;
using crd::math::Vec3f;

namespace
{
// 32 MB TLSF — UniformGrid stores `Array<Array<u32>>` sized cell_count, where
// each empty inner `Array<u32>` carries ~32 B overhead. A 50³ = 125k-cell
// grid needs ~4 MB just for the cells array; default tests use smaller
// grids but plumbing per-cell payloads adds up. 32 MB carries every
// configuration in this file with comfortable margin.
struct AllocFixture { crd::memory::TlsfAllocator alloc{64U << 20}; };

// Default test grid: 20×20×20 = 8000 cells (~256 KB cells overhead).
UniformGridConfig<f32> default_cfg(f32 half = 10.0F, f32 cs = 1.0F)
{
    return UniformGridConfig<f32>{
        AABB3<f32>{Vec3f{-half, -half, -half}, Vec3f{half, half, half}}, cs};
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

TEST_CASE("UniformGrid empty grid behavior", "[geometry-spatial][grid][build]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg()};
    REQUIRE(g.is_empty());
    REQUIRE(g.object_count() == 0U);
    // default_cfg(half=10, cs=1) ⇒ 20×20×20.
    REQUIRE(g.nx() == 20U);
    REQUIRE(g.ny() == 20U);
    REQUIRE(g.nz() == 20U);
    REQUIRE(g.cell_count() == 20U * 20U * 20U);

    crd::containers::Array<u32> hits(&f.alloc);
    g.overlap(AABB3<f32>{Vec3f{-1, -1, -1}, Vec3f{1, 1, 1}}, hits);
    REQUIRE(hits.size() == 0U);

    auto rh = g.raycast(Ray3<f32>{Vec3f{}, Vec3f{1, 0, 0}});
    REQUIRE_FALSE(rh.has_value());
}

TEST_CASE("UniformGrid grid-cell counts via ceil", "[geometry-spatial][grid][build]")
{
    AllocFixture f{};
    // bounds: 10x10x10 with cell_size=3 ⇒ ceil(10/3) = 4 cells per axis
    UniformGridConfig<f32> cfg{
        AABB3<f32>{Vec3f{0, 0, 0}, Vec3f{10, 10, 10}}, 3.0F};
    UniformGrid<f32> g{&f.alloc, cfg};
    REQUIRE(g.nx() == 4U);
    REQUIRE(g.ny() == 4U);
    REQUIRE(g.nz() == 4U);
    REQUIRE(g.cell_count() == 64U);
}

TEST_CASE("UniformGrid single insert + overlap finds it", "[geometry-spatial][grid][insert]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg()};
    auto id = g.insert(aabb_around(Vec3f{1.5F, 2.5F, 3.5F}, 0.3F), 42U);
    REQUIRE(id.valid());
    REQUIRE(g.object_count() == 1U);

    crd::containers::Array<u32> hits(&f.alloc);
    g.overlap(aabb_around(Vec3f{1.5F, 2.5F, 3.5F}, 1.0F), hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 42U);
}

TEST_CASE("UniformGrid out-of-bounds AABB clamps to grid (no insertion outside)",
          "[geometry-spatial][grid][insert]")
{
    AllocFixture f{};
    // Tight grid: bounds [0,10]^3, cell_size 1.
    UniformGrid<f32> g{&f.alloc, UniformGridConfig<f32>{
        AABB3<f32>{Vec3f{0, 0, 0}, Vec3f{10, 10, 10}}, 1.0F}};

    // AABB that straddles the grid edge — should land in cells inside the grid only.
    auto id = g.insert(AABB3<f32>{Vec3f{8, 8, 8}, Vec3f{12, 12, 12}}, 100U);
    (void)id;
    REQUIRE(g.object_count() == 1U);

    crd::containers::Array<u32> hits(&f.alloc);
    g.overlap(AABB3<f32>{Vec3f{8.5F, 8.5F, 8.5F}, Vec3f{9.5F, 9.5F, 9.5F}}, hits);
    REQUIRE(hits.size() == 1U);

    // AABB wholly outside the grid: no spatial occupancy, but handle remains valid.
    auto id2 = g.insert(AABB3<f32>{Vec3f{20, 20, 20}, Vec3f{25, 25, 25}}, 200U);
    REQUIRE(g.object_count() == 2U);
    REQUIRE(g.object_payload(id2) == 200U);
    // It is unfindable spatially because no grid query can reach its cells:
    hits.clear();
    g.overlap(AABB3<f32>{Vec3f{0, 0, 0}, Vec3f{30, 30, 30}}, hits);
    // We see only the in-bounds object (object 100).
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 100U);
}

TEST_CASE("UniformGrid overlap matches brute force on random AABB cloud",
          "[geometry-spatial][grid][overlap][bruteforce]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg(40.0F, 2.0F)};
    std::mt19937 rng(13U);
    std::uniform_real_distribution<f32> uc(-35.0F, 35.0F);
    std::uniform_real_distribution<f32> uh(0.2F, 1.5F);
    crd::containers::Array<AABB3<f32>> objs(&f.alloc);
    for (u32 i = 0; i < 250U; ++i)
    {
        objs.push_back(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng)));
        (void)g.insert(objs[i], i);
    }
    std::uniform_real_distribution<f32> uqh(0.5F, 6.0F);
    for (u32 trial = 0; trial < 20U; ++trial)
    {
        const Vec3f qc{uc(rng), uc(rng), uc(rng)};
        const AABB3<f32> q = aabb_around(qc, uqh(rng));
        crd::containers::Array<u32> got(&f.alloc);
        g.overlap(q, got);
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

TEST_CASE("UniformGrid radius matches brute force", "[geometry-spatial][grid][radius][bruteforce]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg(30.0F, 1.0F)};
    std::mt19937 rng(77U);
    std::uniform_real_distribution<f32> uc(-25.0F, 25.0F);
    crd::containers::Array<AABB3<f32>> objs(&f.alloc);
    for (u32 i = 0; i < 200U; ++i)
    {
        objs.push_back(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, 0.3F));
        (void)g.insert(objs[i], i);
    }
    for (f32 r : {0.5F, 1.5F, 5.0F})
    {
        for (u32 trial = 0; trial < 5U; ++trial)
        {
            const Vec3f q{uc(rng), uc(rng), uc(rng)};
            crd::containers::Array<u32> got(&f.alloc);
            g.radius(q, r, got);
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

TEST_CASE("UniformGrid raycast picks nearest hit (Amanatides-Woo with grid-bounds clip)",
          "[geometry-spatial][grid][raycast]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg(50.0F, 2.0F)};
    (void)g.insert(aabb_around(Vec3f{15.0F, 0, 0}, 0.5F), 15U);
    (void)g.insert(aabb_around(Vec3f{ 5.0F, 0, 0}, 0.5F),  5U);
    (void)g.insert(aabb_around(Vec3f{10.0F, 0, 0}, 0.5F), 10U);
    auto hit = g.raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{1, 0, 0}});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 5U);
    REQUIRE(hit->t >= 4.4F);
    REQUIRE(hit->t <= 4.6F);
}

TEST_CASE("UniformGrid raycast lowest-payload tiebreak on equal t",
          "[geometry-spatial][grid][raycast]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg()};
    (void)g.insert(aabb_around(Vec3f{5.0F, 0, 0}, 0.4F), 9U);
    (void)g.insert(aabb_around(Vec3f{5.0F, 0, 0}, 0.4F), 3U);
    (void)g.insert(aabb_around(Vec3f{5.0F, 0, 0}, 0.4F), 7U);
    auto hit = g.raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{1, 0, 0}});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 3U);
}

TEST_CASE("UniformGrid raycast with negative direction works",
          "[geometry-spatial][grid][raycast]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg()};
    (void)g.insert(aabb_around(Vec3f{-5.0F, 0, 0}, 0.5F), 100U);
    auto hit = g.raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{-1, 0, 0}});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 100U);
}

TEST_CASE("UniformGrid raycast diagonal direction (corner-grazing safe)",
          "[geometry-spatial][grid][raycast]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg()};
    (void)g.insert(aabb_around(Vec3f{5.0F, 5.0F, 5.0F}, 0.4F), 55U);
    // Un-normalised (1,1,1) — exact corner-grazing pattern (advance-tied-axes
    // logic is what makes this find the cell).
    auto hit = g.raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{1, 1, 1}});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 55U);
}

TEST_CASE("UniformGrid raycast misses (no objects on ray path)",
          "[geometry-spatial][grid][raycast]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg()};
    (void)g.insert(aabb_around(Vec3f{40.0F, 0, 0}, 0.5F), 0U);
    auto hit = g.raycast(Ray3<f32>{Vec3f{0, 40.0F, 0}, Vec3f{0, 1, 0}});
    REQUIRE_FALSE(hit.has_value());
}

TEST_CASE("UniformGrid raycast starts outside grid (slab clip kicks in)",
          "[geometry-spatial][grid][raycast]")
{
    AllocFixture f{};
    // Grid [0, 10]^3 with cell_size 1.
    UniformGrid<f32> g{&f.alloc, UniformGridConfig<f32>{
        AABB3<f32>{Vec3f{0, 0, 0}, Vec3f{10, 10, 10}}, 1.0F}};
    (void)g.insert(aabb_around(Vec3f{5.0F, 5.0F, 5.0F}, 0.4F), 7U);

    // Ray starts at (-5, 5, 5), going +X. Must clip into the grid at x=0,
    // walk through cells, find the object at x=5.
    auto hit = g.raycast(Ray3<f32>{Vec3f{-5.0F, 5.0F, 5.0F}, Vec3f{1, 0, 0}});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == 7U);
    REQUIRE(hit->t >= 9.5F);  // ~10 from origin to front of object
    REQUIRE(hit->t <= 10.5F);
}

TEST_CASE("UniformGrid raycast misses grid entirely (slab test rejects)",
          "[geometry-spatial][grid][raycast]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, UniformGridConfig<f32>{
        AABB3<f32>{Vec3f{0, 0, 0}, Vec3f{10, 10, 10}}, 1.0F}};
    (void)g.insert(aabb_around(Vec3f{5.0F, 5.0F, 5.0F}, 0.4F), 7U);
    // Ray parallel to grid but offset out of grid — never enters.
    auto hit = g.raycast(Ray3<f32>{Vec3f{-5.0F, 50.0F, 5.0F}, Vec3f{1, 0, 0}});
    REQUIRE_FALSE(hit.has_value());
}

TEST_CASE("UniformGrid update fast-path: same cell range -> no rebucketing",
          "[geometry-spatial][grid][update][fastpath]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg()};
    auto id = g.insert(aabb_around(Vec3f{0.5F, 0.5F, 0.5F}, 0.1F), 42U);
    const usize objs_before = g.object_count();
    const bool restructured = g.update(id, aabb_around(Vec3f{0.6F, 0.6F, 0.6F}, 0.1F));
    REQUIRE_FALSE(restructured);
    REQUIRE(g.object_count() == objs_before);
}

TEST_CASE("UniformGrid update slow path: cell change triggers rebucketing",
          "[geometry-spatial][grid][update][slowpath]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg()};
    auto id = g.insert(aabb_around(Vec3f{0.5F, 0.5F, 0.5F}, 0.1F), 0U);
    // Move within the [-10, 10] grid bounds — but to a different cell range.
    const bool restructured = g.update(id, aabb_around(Vec3f{8.5F, 8.5F, 8.5F}, 0.1F));
    REQUIRE(restructured);
    crd::containers::Array<u32> hits(&f.alloc);
    g.overlap(aabb_around(Vec3f{0.5F, 0.5F, 0.5F}, 0.5F), hits);
    REQUIRE(hits.size() == 0U);
    hits.clear();
    g.overlap(aabb_around(Vec3f{8.5F, 8.5F, 8.5F}, 0.5F), hits);
    REQUIRE(hits.size() == 1U);
}

TEST_CASE("UniformGrid insert/remove cycle keeps surviving handles valid",
          "[geometry-spatial][grid][cycle]")
{
    AllocFixture f{};
    // Use a 50-half grid to fit -25..24 insertions.
    UniformGrid<f32> g{&f.alloc, default_cfg(50.0F, 2.0F)};
    crd::containers::Array<UniformGridObjectId> ids(&f.alloc);
    for (u32 i = 0; i < 50U; ++i)
    {
        ids.push_back(g.insert(aabb_around(Vec3f{static_cast<f32>(i) - 25.0F, 0, 0}, 0.3F), i));
    }
    REQUIRE(g.object_count() == 50U);
    for (u32 i = 0; i < 50U; i += 2U) { g.remove(ids[i]); }
    REQUIRE(g.object_count() == 25U);
    for (u32 i = 1; i < 50U; i += 2U) { REQUIRE(g.object_payload(ids[i]) == i); }
    crd::containers::Array<u32> hits(&f.alloc);
    g.overlap(AABB3<f32>{Vec3f{-100, -100, -100}, Vec3f{100, 100, 100}}, hits);
    REQUIRE(hits.size() == 25U);
}

TEST_CASE("UniformGrid find_overlapping_pairs matches brute force",
          "[geometry-spatial][grid][pairs][bruteforce]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg(20.0F, 1.0F)};
    std::mt19937 rng(99U);
    std::uniform_real_distribution<f32> uc(-15.0F, 15.0F);
    std::uniform_real_distribution<f32> uh(0.2F, 0.8F);
    crd::containers::Array<AABB3<f32>> objs(&f.alloc);
    for (u32 i = 0; i < 60U; ++i)
    {
        objs.push_back(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng)));
        (void)g.insert(objs[i], i);
    }
    crd::containers::Array<UniformGridPair> got_pairs(&f.alloc);
    g.find_overlapping_pairs(got_pairs);

    crd::containers::Array<UniformGridPair> expected(&f.alloc);
    for (u32 i = 0; i < objs.size(); ++i)
    {
        for (u32 j = i + 1U; j < objs.size(); ++j)
        {
            if (aabb_overlap(objs[i], objs[j])) { expected.push_back(UniformGridPair{i, j}); }
        }
    }
    std::sort(expected.data(), expected.data() + expected.size(),
                [](const UniformGridPair& a, const UniformGridPair& b) { return a < b; });

    REQUIRE(got_pairs.size() == expected.size());
    for (usize i = 0; i < got_pairs.size(); ++i)
    {
        REQUIRE(got_pairs[i] == expected[i]);
    }
}

TEST_CASE("UniformGrid tolerates non-finite query inputs", "[geometry-spatial][grid][nan]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg()};
    for (u32 i = 0; i < 20U; ++i)
    {
        (void)g.insert(aabb_around(Vec3f{static_cast<f32>(i) - 10.0F, 0, 0}, 0.3F), i);
    }
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    crd::containers::Array<u32> hits(&f.alloc);
    g.overlap(AABB3<f32>{Vec3f{nan, 0, 0}, Vec3f{1, 1, 1}}, hits);
    REQUIRE(hits.size() == 0U);
    g.radius(Vec3f{nan, 0, 0}, 1.0F, hits);
    REQUIRE(hits.size() == 0U);
    auto rh = g.raycast(Ray3<f32>{Vec3f{nan, 0, 0}, Vec3f{1, 0, 0}});
    REQUIRE_FALSE(rh.has_value());
    auto rh2 = g.raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{0, 0, 0}});
    REQUIRE_FALSE(rh2.has_value());
}

TEST_CASE("UniformGrid dedup: many-cell object visited once per query",
          "[geometry-spatial][grid][dedup]")
{
    AllocFixture f{};
    // 15 grid half-extent + 0.5 cell = 60×60×60 cells ≈ 8 MB cells overhead.
    UniformGrid<f32> g{&f.alloc, default_cfg(15.0F, 0.5F)};
    auto id = g.insert(AABB3<f32>{Vec3f{-5, -5, -5}, Vec3f{5, 5, 5}}, 999U);
    (void)id;
    crd::containers::Array<u32> hits(&f.alloc);
    g.overlap(AABB3<f32>{Vec3f{-10, -10, -10}, Vec3f{10, 10, 10}}, hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == 999U);
}

// =============================================================================
// Scratch overload tests (parity + concurrent via crd::jobs)
// =============================================================================

TEST_CASE("UniformGrid scratch overlap byte-identical to single-thread overload",
          "[geometry-spatial][grid][scratch]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg(40.0F, 2.0F)};
    std::mt19937 rng(13U);
    std::uniform_real_distribution<f32> uc(-35.0F, 35.0F);
    std::uniform_real_distribution<f32> uh(0.2F, 1.5F);
    crd::containers::Array<AABB3<f32>> objs(&f.alloc);
    for (u32 i = 0; i < 200U; ++i)
    {
        objs.push_back(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng)));
        (void)g.insert(objs[i], i);
    }
    UniformGridScratch scratch(&f.alloc);
    std::uniform_real_distribution<f32> uqh(0.5F, 6.0F);
    for (u32 trial = 0; trial < 20U; ++trial)
    {
        const Vec3f qc{uc(rng), uc(rng), uc(rng)};
        const AABB3<f32> q = aabb_around(qc, uqh(rng));
        crd::containers::Array<u32> single(&f.alloc);
        g.overlap(q, single);
        std::sort(single.data(), single.data() + single.size());
        crd::containers::Array<u32> sc(&f.alloc);
        g.overlap(q, scratch, sc);
        std::sort(sc.data(), sc.data() + sc.size());
        REQUIRE(single.size() == sc.size());
        for (usize i = 0; i < single.size(); ++i) { REQUIRE(single[i] == sc[i]); }
    }
}

TEST_CASE("UniformGrid scratch radius byte-identical to single-thread overload",
          "[geometry-spatial][grid][scratch]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg(30.0F, 1.0F)};
    std::mt19937 rng(77U);
    std::uniform_real_distribution<f32> uc(-25.0F, 25.0F);
    crd::containers::Array<AABB3<f32>> objs(&f.alloc);
    for (u32 i = 0; i < 150U; ++i)
    {
        objs.push_back(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, 0.3F));
        (void)g.insert(objs[i], i);
    }
    UniformGridScratch scratch(&f.alloc);
    for (f32 r : {0.5F, 1.5F, 5.0F})
    {
        for (u32 trial = 0; trial < 5U; ++trial)
        {
            const Vec3f q{uc(rng), uc(rng), uc(rng)};
            crd::containers::Array<u32> single(&f.alloc);
            g.radius(q, r, single);
            std::sort(single.data(), single.data() + single.size());
            crd::containers::Array<u32> sc(&f.alloc);
            g.radius(q, r, scratch, sc);
            std::sort(sc.data(), sc.data() + sc.size());
            REQUIRE(single.size() == sc.size());
            for (usize i = 0; i < single.size(); ++i) { REQUIRE(single[i] == sc[i]); }
        }
    }
}

TEST_CASE("UniformGrid scratch raycast byte-identical to single-thread overload",
          "[geometry-spatial][grid][scratch]")
{
    AllocFixture f{};
    UniformGrid<f32> g{&f.alloc, default_cfg()};
    std::mt19937 rng(31U);
    std::uniform_real_distribution<f32> uc(-20.0F, 20.0F);
    for (u32 i = 0; i < 100U; ++i)
    {
        (void)g.insert(aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, 0.4F), i);
    }
    UniformGridScratch scratch(&f.alloc);
    std::uniform_real_distribution<f32> ud(-1.0F, 1.0F);
    for (u32 trial = 0; trial < 10U; ++trial)
    {
        Vec3f origin{uc(rng), uc(rng), uc(rng)};
        Vec3f dir{ud(rng), ud(rng), ud(rng)};
        const f32 inv_len = 1.0F / std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
        dir.x *= inv_len; dir.y *= inv_len; dir.z *= inv_len;
        auto single = g.raycast(Ray3<f32>{origin, dir});
        auto sc = g.raycast(Ray3<f32>{origin, dir}, scratch);
        REQUIRE(single.has_value() == sc.has_value());
        if (single.has_value())
        {
            REQUIRE(single->payload == sc->payload);
            REQUIRE(single->t == sc->t);
        }
    }
}

// =============================================================================
// Concurrent queries via crd-jobs fiber pool — same pattern as v5d
// =============================================================================

namespace
{
struct GridConcurrencyCorpus
{
    static constexpr u32 kQueries = 16U;
    static constexpr u32 kItersPerQuery = 25U;

    // 50³ cells × ~32 B Array<u32> overhead = 4 MB cells alone, plus per-cell
    // allocations + 400 objects + 16 ref Arrays. 32 MB carries it with
    // generous slack.
    crd::memory::TlsfAllocator alloc{32U << 20};
    UniformGrid<f32>           grid{&alloc, UniformGridConfig<f32>{
        AABB3<f32>{Vec3f{-50, -50, -50}, Vec3f{50, 50, 50}}, 2.0F}};
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

TEST_CASE("UniformGrid concurrent queries via crd-jobs fiber pool",
          "[geometry-spatial][grid][scratch][concurrent][jobs]")
{
    auto corpus = std::make_unique<GridConcurrencyCorpus>();
    std::mt19937 rng(101U);
    std::uniform_real_distribution<f32> uc(-40.0F, 40.0F);
    std::uniform_real_distribution<f32> uh(0.3F, 1.5F);
    for (u32 i = 0; i < 400U; ++i)
    {
        const AABB3<f32> a = aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng));
        (void)corpus->grid.insert(a, i);
    }
    std::uniform_real_distribution<f32> uqh(1.0F, 6.0F);
    for (u32 q = 0; q < GridConcurrencyCorpus::kQueries; ++q)
    {
        corpus->queries[q] = aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uqh(rng));
        corpus->grid.overlap(corpus->queries[q], corpus->ref[q]);
        std::sort(corpus->ref[q].data(), corpus->ref[q].data() + corpus->ref[q].size());
    }

    constexpr u32 kTotalTasks = GridConcurrencyCorpus::kQueries * GridConcurrencyCorpus::kItersPerQuery;
    auto* corpus_ptr = corpus.get();
    crd::jobs::Counter* counter = crd::jobs::parallel_for(
        kTotalTasks, /*num_jobs=*/16U,
        [corpus_ptr](crd::u32 begin, crd::u32 end) noexcept {
            for (crd::u32 task_idx = begin; task_idx < end; ++task_idx)
            {
                const u32 q = task_idx % GridConcurrencyCorpus::kQueries;
                crd::memory::TlsfAllocator local_alloc{1U << 16};
                UniformGridScratch         local_scratch(&local_alloc);
                crd::containers::Array<u32> got(&local_alloc);
                corpus_ptr->grid.overlap(corpus_ptr->queries[q], local_scratch, got);
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
