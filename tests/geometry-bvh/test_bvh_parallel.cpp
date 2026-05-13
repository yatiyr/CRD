// crd-geometry-bvh v1f — bvh_build_parallel: produces a tree byte-for-byte
// equal to the serial bvh_build (any num_jobs), incl. the default-threshold
// large-corpus path and the degenerate cases. Hosts the binary's jobs listener.

#include <crd/geometry/bvh/bvh.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstring>
#include <vector>

// The whole crd-geometry-bvh-tests binary runs with the job system up — a
// listener (not jobs::init() at file scope) so it doesn't fire during
// catch_discover_tests' listing phase. frame_reset() after each test case
// keeps the per-thread frame arenas (which parallel_reduce allocates from)
// from filling across the parallel-build cases.
namespace
{
struct BvhJobsListener final : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override
    {
        // Big frame arena: the parallel-build tests do many builds with a low
        // threshold, each `parallel_for` call burning a little frame-arena
        // (JobDecl array) — `frame_reset` runs per case, but give generous slack.
        crd::jobs::init(crd::jobs::Config{.num_threads = 4, .frame_alloc_bytes = 64U << 20U});
    }
    void testCaseEnded(Catch::TestCaseStats const&) override { crd::jobs::frame_reset(); }
    void testRunEnded(Catch::TestRunStats const&) override { crd::jobs::shutdown(); }
};
} // namespace
CATCH_REGISTER_LISTENER(BvhJobsListener)

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::bvh::bvh_build;
using crd::geometry::bvh::bvh_build_parallel;
using crd::geometry::bvh::BvhBuildOptions;
using crd::geometry::bvh::BvhNode;
using crd::geometry::bvh::BvhTree;
using crd::geometry::primitives::AABB3;
using crd::math::Vec3;

struct Rng
{
    crd::u64 state;
    explicit Rng(crd::u64 seed) : state(seed) {}
    crd::u64 next()
    {
        crd::u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    f32 unit() { return static_cast<f32>(next() >> 40) / static_cast<f32>(1U << 24); }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
};

AABB3<f32> random_box(Rng& rng, f32 world, f32 max_size)
{
    const Vec3<f32> c(rng.range(-world, world), rng.range(-world, world), rng.range(-world, world));
    const Vec3<f32> h(rng.range(0.05F, max_size), rng.range(0.05F, max_size), rng.range(0.05F, max_size));
    return AABB3<f32>(Vec3<f32>(c.x - h.x, c.y - h.y, c.z - h.z), Vec3<f32>(c.x + h.x, c.y + h.y, c.z + h.z));
}

void require_identical(const BvhTree& a, const BvhTree& b)
{
    REQUIRE(a.node_count() == b.node_count());
    REQUIRE(a.prim_count() == b.prim_count());
    REQUIRE(a.root() == b.root());
    if (a.node_count() > 0)
    {
        REQUIRE(std::memcmp(a.nodes().data(), b.nodes().data(), a.nodes().size() * sizeof(BvhNode)) == 0);
    }
    if (a.prim_count() > 0)
    {
        REQUIRE(std::memcmp(a.prim_indices().data(), b.prim_indices().data(), a.prim_indices().size() * sizeof(u32)) ==
                0);
    }
}

} // namespace

TEST_CASE("bvh_build_parallel: bit-identical to the serial build (random corpora, any num_jobs)",
          "[geometry][bvh][parallel]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "bvh-test");
    Rng rng(0xA11E1);
    for (usize trial = 0; trial < 5; ++trial)
    {
        const usize n = 200U + (rng.next() % 800U);
        std::vector<AABB3<f32>> prims;
        for (usize i = 0; i < n; ++i)
        {
            prims.push_back(random_box(rng, 100.0F, 3.0F));
        }
        BvhBuildOptions opts;
        opts.max_leaf_prims = static_cast<crd::u16>(1U + (rng.next() % 8U));
        opts.sah_bins = static_cast<u32>(4U + (rng.next() % 24U));
        const auto span = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
        const BvhTree serial = bvh_build(span, &alloc, opts);
        // Threshold pulled low so the parallel path is genuinely exercised.
        for (u32 nj : {1U, 2U, 4U, 8U})
        {
            const BvhTree par = bvh_build_parallel(span, &alloc, opts, nj, /*parallel_threshold*/ 64U);
            require_identical(serial, par);
        }
    }
}

TEST_CASE("bvh_build_parallel: large corpus above the default threshold matches the serial build",
          "[geometry][bvh][parallel]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 24, nullptr, "bvh-test");
    Rng rng(0xB16C04);
    std::vector<AABB3<f32>> prims;
    constexpr usize n = 20000; // > the 8192 default parallel_threshold
    for (usize i = 0; i < n; ++i)
    {
        prims.push_back(random_box(rng, 200.0F, 2.0F));
    }
    const auto span = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree serial = bvh_build(span, &alloc);
    const BvhTree par = bvh_build_parallel(span, &alloc); // default num_jobs (auto) + default threshold
    require_identical(serial, par);
}

TEST_CASE("bvh_build_parallel: degenerate inputs match the serial build", "[geometry][bvh][parallel]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "bvh-test");

    // Empty.
    require_identical(bvh_build(crd::containers::ConstSpan<AABB3<f32>>(), &alloc),
                      bvh_build_parallel(crd::containers::ConstSpan<AABB3<f32>>(), &alloc, {}, 4U, 1U));

    // Single primitive.
    const AABB3<f32> one(Vec3<f32>(-1, -2, -3), Vec3<f32>(4, 5, 6));
    require_identical(bvh_build(crd::containers::ConstSpan<AABB3<f32>>(&one, 1), &alloc),
                      bvh_build_parallel(crd::containers::ConstSpan<AABB3<f32>>(&one, 1), &alloc, {}, 4U, 1U));

    // Coincident centroids (forces the median-by-index fallback at every split).
    std::vector<AABB3<f32>> coincident(800, AABB3<f32>(Vec3<f32>(-1, -1, -1), Vec3<f32>(1, 1, 1)));
    BvhBuildOptions opts;
    opts.max_leaf_prims = 4;
    const auto cspan = crd::containers::ConstSpan<AABB3<f32>>(coincident.data(), coincident.size());
    require_identical(bvh_build(cspan, &alloc, opts), bvh_build_parallel(cspan, &alloc, opts, 4U, /*threshold*/ 64U));
}

TEST_CASE("bvh_build_parallel: num_jobs=1 and small-input both take the serial fast path", "[geometry][bvh][parallel]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 20, nullptr, "bvh-test");
    Rng rng(0x5A4EFA57);
    std::vector<AABB3<f32>> prims;
    for (usize i = 0; i < 300; ++i)
    {
        prims.push_back(random_box(rng, 50.0F, 2.0F));
    }
    const auto span = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree serial = bvh_build(span, &alloc);
    require_identical(serial, bvh_build_parallel(span, &alloc, {}, /*num_jobs*/ 1U, /*threshold*/ 1U));
    require_identical(serial, bvh_build_parallel(span, &alloc, {}, /*num_jobs*/ 8U, /*threshold*/ 1000000U));
}
