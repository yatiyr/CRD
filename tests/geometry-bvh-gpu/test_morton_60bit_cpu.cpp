// ---------------------------------------------------------------------------
// crd-geometry-bvh-gpu v9a-60bit-cpu -- u64 60-bit Morton CPU oracle tests.
//
// CALIBRATION FIRST per advisor TDD + v8h precedent. Mirror of
// test_morton.cpp's 30-bit calibration but for the u64 / 20-bits-per-axis
// path. Lane-pinned bit patterns prove `spread_bits_60` is correct
// before any downstream test can yield meaningful signal.
//
// Discriminating test: km-scale scene where 30-bit Morton would collide
// (1m primitives in a 1km scene = 1024-bin resolution = 1m-per-bin
// collisions) but 60-bit resolves (1M-bin resolution = 1mm-per-bin).
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh_gpu/morton.hpp>
#include <crd/geometry/bvh_gpu/morton_60bit.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdint>

namespace
{

using crd::geometry::bvh_gpu::compute_morton_codes_cpu;
using crd::geometry::bvh_gpu::compute_morton_codes_cpu_60bit;
using crd::geometry::bvh_gpu::morton3_60bit_from_ints;
using crd::geometry::bvh_gpu::quantize_to_morton_grid_20bit;
using crd::geometry::bvh_gpu::spread_bits_60;
using crd::geometry::primitives::AABB3;
using crd::math::Vec3;

} // namespace

// =========================================================================
// CALIBRATION FIRST -- pure-int bit-interleave (20 bits per axis)
// =========================================================================

TEST_CASE("CALIBRATION: spread_bits_60 lane-pinned bit patterns",
          "[morton][morton60][calibration]")
{
    // Bit i of input lands at position 3*i of output. Output bits sit at
    // positions {0, 3, 6, ..., 57}.
    CHECK(spread_bits_60(std::uint64_t{0})        == std::uint64_t{0x0000000000000000ULL});
    CHECK(spread_bits_60(std::uint64_t{1})        == std::uint64_t{0x0000000000000001ULL}); // bit 0  -> 0
    CHECK(spread_bits_60(std::uint64_t{2})        == std::uint64_t{0x0000000000000008ULL}); // bit 1  -> 3
    CHECK(spread_bits_60(std::uint64_t{4})        == std::uint64_t{0x0000000000000040ULL}); // bit 2  -> 6
    CHECK(spread_bits_60(std::uint64_t{0x80000ULL}) == (std::uint64_t{1} << 57U));         // bit 19 -> 57
    // All 20 bits set -> every 3rd bit in [0, 57].
    CHECK(spread_bits_60(std::uint64_t{0xFFFFFULL}) == std::uint64_t{0x0249249249249249ULL});
    // Bits above the low 20 are masked off.
    CHECK(spread_bits_60(std::uint64_t{0xFFFFFFFFFFFFFFFFULL})
          == std::uint64_t{0x0249249249249249ULL});
}

TEST_CASE("CALIBRATION: morton3_60bit_from_ints canonical triples",
          "[morton][morton60][calibration]")
{
    CHECK(morton3_60bit_from_ints(0ULL, 0ULL, 0ULL) == std::uint64_t{0});
    CHECK(morton3_60bit_from_ints(1ULL, 0ULL, 0ULL) == std::uint64_t{0x0000000000000001ULL});
    CHECK(morton3_60bit_from_ints(0ULL, 1ULL, 0ULL) == std::uint64_t{0x0000000000000002ULL});
    CHECK(morton3_60bit_from_ints(0ULL, 0ULL, 1ULL) == std::uint64_t{0x0000000000000004ULL});
    CHECK(morton3_60bit_from_ints(1ULL, 1ULL, 1ULL) == std::uint64_t{0x0000000000000007ULL});
    // (1048575, 1048575, 1048575) -> every bit in [0, 59] set
    // = 0x0FFFFFFFFFFFFFFF.
    CHECK(morton3_60bit_from_ints(1048575ULL, 1048575ULL, 1048575ULL)
          == std::uint64_t{0x0FFFFFFFFFFFFFFFULL});
}

TEST_CASE("CALIBRATION: quantize_to_morton_grid_20bit endpoints + clamping",
          "[morton][morton60][calibration]")
{
    const crd::f32 lo         = 0.0F;
    const crd::f32 inv_extent = 1.0F;
    CHECK(quantize_to_morton_grid_20bit(0.0F,    lo, inv_extent) == std::uint64_t{0});
    CHECK(quantize_to_morton_grid_20bit(0.5F,    lo, inv_extent) == std::uint64_t{524288});
    CHECK(quantize_to_morton_grid_20bit(0.999F,  lo, inv_extent) == std::uint64_t{1047527});
    // At exact upper boundary -> clamp(1048576, 0, 1048575) = 1048575.
    CHECK(quantize_to_morton_grid_20bit(1.0F,    lo, inv_extent) == std::uint64_t{1048575});
    CHECK(quantize_to_morton_grid_20bit(-0.1F,   lo, inv_extent) == std::uint64_t{0});
    CHECK(quantize_to_morton_grid_20bit(99.0F,   lo, inv_extent) == std::uint64_t{1048575});
    CHECK(quantize_to_morton_grid_20bit(123.0F,  lo, 0.0F)        == std::uint64_t{0});
}

// =========================================================================
// CPU oracle on real centroids
// =========================================================================

TEST_CASE("compute_morton_codes_cpu_60bit empty input -> empty output",
          "[morton][morton60][cpu]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    crd::containers::ConstSpan<AABB3<crd::f32>> empty{};
    const auto out = compute_morton_codes_cpu_60bit(empty, &alloc);
    CHECK(out.empty());
}

TEST_CASE("compute_morton_codes_cpu_60bit corner AABBs map to corner Morton codes",
          "[morton][morton60][cpu]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    crd::containers::Array<AABB3<crd::f32>> aabbs(&alloc);
    for (int iz = 0; iz < 2; ++iz)
    {
        for (int iy = 0; iy < 2; ++iy)
        {
            for (int ix = 0; ix < 2; ++ix)
            {
                Vec3<crd::f32> c{
                    ix == 0 ? 0.0F : 0.999F,
                    iy == 0 ? 0.0F : 0.999F,
                    iz == 0 ? 0.0F : 0.999F,
                };
                aabbs.push_back({c, c});
            }
        }
    }
    const AABB3<crd::f32> scene_aabb{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
    const auto codes = compute_morton_codes_cpu_60bit(
        crd::containers::ConstSpan<AABB3<crd::f32>>(aabbs.data(), aabbs.size()),
        scene_aabb, &alloc);
    REQUIRE(codes.size() == 8U);
    for (crd::usize i = 0; i < 8; ++i)
    {
        for (crd::usize j = i + 1; j < 8; ++j)
        {
            CHECK(codes[i] != codes[j]);
        }
    }
    CHECK(codes[0] == std::uint64_t{0});
    // The (0.999, 0.999, 0.999) corner -> bin (1047527, 1047527, 1047527).
    CHECK(codes[7] == morton3_60bit_from_ints(1047527ULL, 1047527ULL, 1047527ULL));
}

// =========================================================================
// DISCRIMINATING: km-scale scene where 30-bit collides + 60-bit resolves
// =========================================================================

TEST_CASE("km-scale scene: 30-bit Morton collides, 60-bit resolves",
          "[morton][morton60][discriminating]")
{
    // Scene = 1 km cube. Two AABBs at (100.0, 100.0, 100.0) and
    // (100.5, 100.0, 100.0) -- 0.5 m apart along x. With 30-bit Morton
    // at 1024 bins per axis, bin size = 1000m / 1024 ~ 0.977 m, so both
    // centroids fall in the same bin. With 60-bit Morton at 1048576
    // bins per axis, bin size = 1000m / 1048576 ~ 0.95 mm, so the two
    // centroids fall in DIFFERENT bins (~500 bins apart along x).
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    crd::containers::Array<AABB3<crd::f32>> aabbs(&alloc);
    aabbs.push_back({{100.0F, 100.0F, 100.0F}, {100.0F, 100.0F, 100.0F}});
    aabbs.push_back({{100.5F, 100.0F, 100.0F}, {100.5F, 100.0F, 100.0F}});

    const AABB3<crd::f32> scene_aabb{{0.0F, 0.0F, 0.0F}, {1000.0F, 1000.0F, 1000.0F}};

    const auto codes_30 = compute_morton_codes_cpu(
        crd::containers::ConstSpan<AABB3<crd::f32>>(aabbs.data(), aabbs.size()),
        scene_aabb, &alloc);
    const auto codes_60 = compute_morton_codes_cpu_60bit(
        crd::containers::ConstSpan<AABB3<crd::f32>>(aabbs.data(), aabbs.size()),
        scene_aabb, &alloc);
    REQUIRE(codes_30.size() == 2U);
    REQUIRE(codes_60.size() == 2U);

    // 30-bit collides at this scale (the bug 60-bit fixes).
    INFO("30-bit codes: [" << codes_30[0] << ", " << codes_30[1] << "]");
    INFO("60-bit codes: [" << codes_60[0] << ", " << codes_60[1] << "]");
    CHECK(codes_30[0] == codes_30[1]);
    // 60-bit resolves.
    CHECK(codes_60[0] != codes_60[1]);
}

TEST_CASE("60-bit Morton determinism: identical input yields identical output",
          "[morton][morton60][determinism]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);
    crd::containers::Array<AABB3<crd::f32>> aabbs(&alloc);
    constexpr crd::u32 kCount = 256U;
    for (crd::u32 i = 0; i < kCount; ++i)
    {
        const crd::f32 fx = static_cast<crd::f32>((i * 2654435761U) & 0xFFFFU) / 65536.0F;
        const crd::f32 fy = static_cast<crd::f32>((i * 40503U)       & 0xFFFFU) / 65536.0F;
        const crd::f32 fz = static_cast<crd::f32>((i * 1664525U)     & 0xFFFFU) / 65536.0F;
        aabbs.push_back({{fx, fy, fz}, {fx + 0.01F, fy + 0.01F, fz + 0.01F}});
    }
    const AABB3<crd::f32> scene{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
    const auto span = crd::containers::ConstSpan<AABB3<crd::f32>>(aabbs.data(), aabbs.size());

    const auto a = compute_morton_codes_cpu_60bit(span, scene, &alloc);
    const auto b = compute_morton_codes_cpu_60bit(span, scene, &alloc);
    REQUIRE(a.size() == kCount);
    REQUIRE(b.size() == kCount);
    bool all_match = true;
    for (crd::usize i = 0; i < kCount; ++i)
    {
        if (a[i] != b[i]) { all_match = false; break; }
    }
    CHECK(all_match);
}
