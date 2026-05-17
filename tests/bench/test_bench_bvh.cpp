// crd-geometry-bvh v1f — off-by-default benchmarks (`[!benchmark]`, not in
// ctest). Measures Cerid's own numbers; the reference targets below are from
// the `docs/phases/phase-3.1.7-geometry.md` performance-budgets table (§4.1 of
// the supplement dossier) — Embree's published figures, the "within 2×" goal.
// No Embree dependency; if a direct head-to-head is wanted later, build against
// a local Embree behind an opt-in CMake flag.
//
// Reference targets (Zen-4-class CPU, win-shipping):
//   * bvh_build, 100k AABBs, single thread  ≤ ~50 ms   (Embree binned-SAH ~25 ms)
//   * bvh_build_parallel, 100k AABBs        materially faster than serial on ≥4 cores
//   * bvh_raycast, 1M-AABB tree             ≥ ~5M rays/s single-threaded
//   * BVH4 traversal node test              ~8 ns/node on AVX2 (Embree ~6 ns)
//   * bvh4_collapse, 100k-AABB binary tree  small constant-factor of the build
//   * bvh_closest_point, 100k-AABB tree     ≪ 1 µs/query average

#include <crd/geometry/bvh/bvh.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using namespace crd::geometry::bvh;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Ray3;
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

std::vector<AABB3<f32>> make_corpus(usize n, crd::u64 seed)
{
    Rng rng(seed);
    std::vector<AABB3<f32>> v;
    v.reserve(n);
    for (usize i = 0; i < n; ++i)
    {
        const Vec3<f32> c(rng.range(-200, 200), rng.range(-200, 200), rng.range(-200, 200));
        const Vec3<f32> h(rng.range(0.1F, 2.0F), rng.range(0.1F, 2.0F), rng.range(0.1F, 2.0F));
        v.emplace_back(Vec3<f32>(c.x - h.x, c.y - h.y, c.z - h.z), Vec3<f32>(c.x + h.x, c.y + h.y, c.z + h.z));
    }
    return v;
}

Vec3<f32> normalized(const Vec3<f32>& v)
{
    const f32 l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return Vec3<f32>(v.x / l, v.y / l, v.z / l);
}

std::vector<Ray3<f32>> make_rays(usize n, crd::u64 seed)
{
    Rng rng(seed);
    std::vector<Ray3<f32>> v;
    v.reserve(n);
    for (usize i = 0; i < n; ++i)
    {
        v.emplace_back(Vec3<f32>(rng.range(-260, 260), rng.range(-260, 260), rng.range(-260, 260)),
                       normalized(Vec3<f32>(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1))));
    }
    return v;
}

} // namespace

TEST_CASE("bench BVH build -- serial vs parallel", "[bench][bench-bvh][!benchmark]")
{
    crd::jobs::init();
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 27, nullptr, "bench-bvh"); // 128 MB
    const std::vector<AABB3<f32>> c100k = make_corpus(100000, 0xB14C00);
    const std::vector<AABB3<f32>> c1m = make_corpus(1000000, 0xB14C01);
    const auto s100k = crd::containers::ConstSpan<AABB3<f32>>(c100k.data(), c100k.size());
    const auto s1m = crd::containers::ConstSpan<AABB3<f32>>(c1m.data(), c1m.size());

    BENCHMARK("bvh_build  100k  (serial)")
    {
        return bvh_build(s100k, &alloc);
    };
    BENCHMARK("bvh_build_parallel  100k  (auto jobs)")
    {
        crd::jobs::frame_reset();
        return bvh_build_parallel(s100k, &alloc);
    };
    BENCHMARK("bvh_build  1M  (serial)")
    {
        return bvh_build(s1m, &alloc);
    };
    BENCHMARK("bvh_build_parallel  1M  (auto jobs)")
    {
        crd::jobs::frame_reset();
        return bvh_build_parallel(s1m, &alloc);
    };
    crd::jobs::shutdown();
}

TEST_CASE("bench BVH raycast -- binary vs BVH4, 100k rays vs a 100k-AABB tree", "[bench][bench-bvh][!benchmark]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 25, nullptr, "bench-bvh");
    const std::vector<AABB3<f32>> prims = make_corpus(100000, 0xB14C10);
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    const Bvh4Tree quad = bvh4_collapse(tree, &alloc);
    const std::vector<Ray3<f32>> rays = make_rays(100000, 0xB14C11);

    BENCHMARK("bvh_raycast  x100k  (binary)")
    {
        f32 acc = 0.0F;
        for (const Ray3<f32>& r : rays)
        {
            if (const auto h = bvh_raycast(tree, pspan, r))
            {
                acc += h->t;
            }
        }
        return acc;
    };
    BENCHMARK("bvh4_raycast  x100k  (4-wide)")
    {
        f32 acc = 0.0F;
        for (const Ray3<f32>& r : rays)
        {
            if (const auto h = bvh4_raycast(quad, pspan, r))
            {
                acc += h->t;
            }
        }
        return acc;
    };
    BENCHMARK("bvh4_collapse  100k")
    {
        return bvh4_collapse(tree, &alloc);
    };
}

TEST_CASE("bench BVH closest-point -- 100k queries vs a 100k-AABB tree", "[bench][bench-bvh][!benchmark]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 25, nullptr, "bench-bvh");
    const std::vector<AABB3<f32>> prims = make_corpus(100000, 0xB14C20);
    const auto pspan = crd::containers::ConstSpan<AABB3<f32>>(prims.data(), prims.size());
    const BvhTree tree = bvh_build(pspan, &alloc);
    Rng rng(0xB14C21);
    std::vector<Vec3<f32>> queries;
    queries.reserve(100000);
    for (usize i = 0; i < 100000; ++i)
    {
        queries.emplace_back(rng.range(-260, 260), rng.range(-260, 260), rng.range(-260, 260));
    }

    BENCHMARK("bvh_closest_point  x100k")
    {
        f32 acc = 0.0F;
        for (const Vec3<f32>& q : queries)
        {
            if (const auto cp = bvh_closest_point(tree, pspan, q))
            {
                acc += cp->distance_squared;
            }
        }
        return acc;
    };
}
