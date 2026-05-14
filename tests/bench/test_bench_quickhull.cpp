// Benchmarks for v3 convex-hull substrate (Phase 3.1.7 v3-close).
//
// Coverage:
//   - 3D Quickhull build throughput vs N points (100 / 1k / 10k / 100k).
//   - 2D monotone-chain throughput vs N points.
//   - Hull simplification throughput vs source vertex count.
//
// Not in ctest. Run via: ./crd-bench.exe "[!benchmark]" --benchmark-warmup 100ms

#include <crd/geometry/convex/convex_hull_2d.hpp>
#include <crd/geometry/convex/hull_simplify.hpp>
#include <crd/geometry/convex/quickhull.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::f64;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::math::Vec2;
using crd::math::Vec3;

struct BenchRng
{
    u64 state;
    explicit BenchRng(u64 seed) : state(seed) {}
    u64 next()
    {
        u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    f64 unit() { return static_cast<f64>(next() >> 12) / static_cast<f64>(1ULL << 52); }
    f64 range(f64 lo, f64 hi) { return lo + (hi - lo) * unit(); }
};

template <typename Container, typename T>
void fill_random_3d(Container& out, usize n, u64 seed)
{
    BenchRng rng(seed);
    out.clear();
    out.reserve(n);
    for (usize i = 0; i < n; ++i)
    {
        out.push_back(Vec3<T>(static_cast<T>(rng.range(-1, 1)), static_cast<T>(rng.range(-1, 1)),
                               static_cast<T>(rng.range(-1, 1))));
    }
}

template <typename Container, typename T>
void fill_random_2d(Container& out, usize n, u64 seed)
{
    BenchRng rng(seed);
    out.clear();
    out.reserve(n);
    for (usize i = 0; i < n; ++i)
    {
        out.push_back(Vec2<T>(static_cast<T>(rng.range(-1, 1)), static_cast<T>(rng.range(-1, 1))));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 3D Quickhull build throughput
// ---------------------------------------------------------------------------

TEST_CASE("bench: Quickhull build 100 points", "[!benchmark][v3][quickhull]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    fill_random_3d<crd::containers::Array<Vec3<f64>>, f64>(points, 100, 0x1111111111111111ULL);

    BENCHMARK("Quickhull n=100")
    {
        return crd::geometry::convex::quickhull<f64>(
            crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    };
}

TEST_CASE("bench: Quickhull build 1k points", "[!benchmark][v3][quickhull]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    fill_random_3d<crd::containers::Array<Vec3<f64>>, f64>(points, 1000, 0x2222222222222222ULL);

    BENCHMARK("Quickhull n=1000")
    {
        return crd::geometry::convex::quickhull<f64>(
            crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    };
}

TEST_CASE("bench: Quickhull build 10k points", "[!benchmark][v3][quickhull]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    fill_random_3d<crd::containers::Array<Vec3<f64>>, f64>(points, 10000, 0x3333333333333333ULL);

    BENCHMARK("Quickhull n=10000")
    {
        return crd::geometry::convex::quickhull<f64>(
            crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    };
}

// ---------------------------------------------------------------------------
// 2D monotone-chain throughput
// ---------------------------------------------------------------------------

TEST_CASE("bench: 2D monotone chain 1k points", "[!benchmark][v3][hull2d]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    fill_random_2d<crd::containers::Array<Vec2<f64>>, f64>(points, 1000, 0x4444444444444444ULL);
    crd::containers::Array<u32> hull(&alloc);

    BENCHMARK("monotone-chain n=1000")
    {
        crd::geometry::convex::convex_hull_2d_indices<f64>(
            crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull);
        return hull.size();
    };
}

TEST_CASE("bench: 2D monotone chain 10k points", "[!benchmark][v3][hull2d]")
{
    crd::memory::TlsfAllocator alloc(32U * 1024U * 1024U);
    crd::containers::Array<Vec2<f64>> points(&alloc);
    fill_random_2d<crd::containers::Array<Vec2<f64>>, f64>(points, 10000, 0x5555555555555555ULL);
    crd::containers::Array<u32> hull(&alloc);

    BENCHMARK("monotone-chain n=10000")
    {
        crd::geometry::convex::convex_hull_2d_indices<f64>(
            crd::containers::ConstSpan<Vec2<f64>>(points.data(), points.size()), hull);
        return hull.size();
    };
}

// ---------------------------------------------------------------------------
// v3d hull simplification throughput
// ---------------------------------------------------------------------------

TEST_CASE("bench: simplify_hull 200-point source -> 8 vertices", "[!benchmark][v3d]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    fill_random_3d<crd::containers::Array<Vec3<f64>>, f64>(points, 200, 0x6666666666666666ULL);
    auto source = crd::geometry::convex::quickhull<f64>(
        crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);

    crd::geometry::convex::HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 8;

    BENCHMARK("simplify_hull 200 -> 8")
    {
        return crd::geometry::convex::simplify_hull<f64>(source, &alloc, opts);
    };
}

TEST_CASE("bench: simplify_hull 500-point source -> 16 vertices", "[!benchmark][v3d]")
{
    crd::memory::TlsfAllocator alloc(32U * 1024U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    fill_random_3d<crd::containers::Array<Vec3<f64>>, f64>(points, 500, 0x7777777777777777ULL);
    auto source = crd::geometry::convex::quickhull<f64>(
        crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);

    crd::geometry::convex::HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 16;

    BENCHMARK("simplify_hull 500 -> 16")
    {
        return crd::geometry::convex::simplify_hull<f64>(source, &alloc, opts);
    };
}
