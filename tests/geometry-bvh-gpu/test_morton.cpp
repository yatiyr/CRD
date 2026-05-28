// ---------------------------------------------------------------------------
// crd-geometry-bvh-gpu v9a-a -- Morton-code generation tests.
//
// CALIBRATION FIRST per advisor TDD + v8h precedent:
//   1. spread_bits_30 lane-pinned: 10 specific bit patterns + their
//      expected output. If this fails, the bit-interleave is broken
//      and every downstream test is meaningless.
//   2. morton3_30bit_from_ints: a handful of canonical (ix,iy,iz)
//      triples with manually-computed expected u32 codes.
//   3. quantize_to_morton_grid endpoints.
//
// Then CPU-vs-CPU (covers the FP path), then GPU-vs-CPU via
// ValidationCapture + bit_compare + gpu_determinism_check +
// CRD_PERF_BUDGET_LE per the v9-prereq-test-harness discipline.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh_gpu/dispatch.hpp>
#include <crd/geometry/bvh_gpu/morton.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/measure.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/rhi/vulkan_backend.hpp>
#include <crd/rhi/vulkan_validation_capture.hpp>
#include <crd/test_helpers/gpu_compare.hpp>
#include <crd/test_helpers/gpu_determinism.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fs = crd::platform::fs;

namespace
{

[[nodiscard]] bool headless_requested() noexcept
{
    const char* v = std::getenv("CRD_PLATFORM_HEADLESS");
    return v != nullptr && v[0] == '1';
}

using crd::geometry::bvh_gpu::compute_morton_codes_cpu;
using crd::geometry::bvh_gpu::MortonGpuPipeline;
using crd::geometry::bvh_gpu::morton3_30bit_from_ints;
using crd::geometry::bvh_gpu::quantize_to_morton_grid;
using crd::geometry::bvh_gpu::spread_bits_30;
using crd::geometry::bvh_gpu::union_aabb_of;
using crd::geometry::primitives::AABB3;
using crd::math::Vec3;

} // namespace

// =========================================================================
// CALIBRATION FIRST -- pure-int bit-interleave
// =========================================================================

TEST_CASE("CALIBRATION: spread_bits_30 lane-pinned bit patterns",
          "[morton][calibration]")
{
    // Bit i of input lands at position 3*i of output.
    CHECK(spread_bits_30(0U)        == 0x00000000U);
    CHECK(spread_bits_30(1U)        == 0x00000001U);       // bit 0 -> 0
    CHECK(spread_bits_30(2U)        == 0x00000008U);       // bit 1 -> 3
    CHECK(spread_bits_30(4U)        == 0x00000040U);       // bit 2 -> 6
    CHECK(spread_bits_30(0x200U)    == (1U << 27U));       // bit 9 -> 27
    // All 10 bits set => every 3rd bit in [0, 27].
    CHECK(spread_bits_30(0x3FFU)    == 0x09249249U);
    // Bits above the low 10 are masked off.
    CHECK(spread_bits_30(0xFFFFFFFFU) == 0x09249249U);
}

TEST_CASE("CALIBRATION: morton3_30bit_from_ints canonical triples",
          "[morton][calibration]")
{
    // Origin -> 0.
    CHECK(morton3_30bit_from_ints(0U, 0U, 0U) == 0U);
    // (1,0,0) -> bit 0 = 1.
    CHECK(morton3_30bit_from_ints(1U, 0U, 0U) == 0x00000001U);
    // (0,1,0) -> bit 1 = 1.
    CHECK(morton3_30bit_from_ints(0U, 1U, 0U) == 0x00000002U);
    // (0,0,1) -> bit 2 = 1.
    CHECK(morton3_30bit_from_ints(0U, 0U, 1U) == 0x00000004U);
    // (1,1,1) -> bits 0,1,2 = 0b111 = 7.
    CHECK(morton3_30bit_from_ints(1U, 1U, 1U) == 0x00000007U);
    // (1023, 1023, 1023) -> every bit in [0,29] set = 0x3FFFFFFF.
    CHECK(morton3_30bit_from_ints(1023U, 1023U, 1023U) == 0x3FFFFFFFU);
}

TEST_CASE("CALIBRATION: quantize_to_morton_grid endpoints + clamping",
          "[morton][calibration]")
{
    const float lo          = 0.0F;
    const float inv_extent  = 1.0F;          // extent = 1
    CHECK(quantize_to_morton_grid(0.0F, lo, inv_extent)             == 0U);
    CHECK(quantize_to_morton_grid(0.5F, lo, inv_extent)             == 512U);
    CHECK(quantize_to_morton_grid(0.999F, lo, inv_extent)           == 1022U);
    // Exactly at the upper boundary maps to clamp(1024, 0, 1023) = 1023.
    CHECK(quantize_to_morton_grid(1.0F, lo, inv_extent)             == 1023U);
    // Out-of-AABB (below + above) clamps.
    CHECK(quantize_to_morton_grid(-0.1F, lo, inv_extent)            == 0U);
    CHECK(quantize_to_morton_grid(99.0F, lo, inv_extent)            == 1023U);
    // Zero inv_extent (degenerate axis) -> bin 0.
    CHECK(quantize_to_morton_grid(123.456F, lo, 0.0F)               == 0U);
}

// =========================================================================
// CPU oracle on real centroids
// =========================================================================

TEST_CASE("compute_morton_codes_cpu empty input -> empty output", "[morton][cpu]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    crd::containers::ConstSpan<AABB3<crd::f32>> empty{};
    const auto out = compute_morton_codes_cpu(empty, &alloc);
    CHECK(out.empty());
}

TEST_CASE("compute_morton_codes_cpu corner AABBs map to corner Morton codes",
          "[morton][cpu]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    crd::containers::Array<AABB3<crd::f32>> aabbs(&alloc);
    // Scene = unit cube. 8 corner AABBs at the 8 cube corners, with
    // centroids placed so they quantise to bin (0,0,0) or (1023,...)
    // deterministically. Using centroid coords 0.0 vs 0.999 lands us
    // squarely in bin 0 vs bin 1022 (slightly inside the scene). Pin
    // both Morton extremes.
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
    const auto codes = compute_morton_codes_cpu(
        crd::containers::ConstSpan<AABB3<crd::f32>>(aabbs.data(), aabbs.size()),
        scene_aabb, &alloc);
    REQUIRE(codes.size() == 8U);
    // 8 distinct centroids -> 8 distinct Morton codes.
    for (crd::usize i = 0; i < 8; ++i)
    {
        for (crd::usize j = i + 1; j < 8; ++j)
        {
            CHECK(codes[i] != codes[j]);
        }
    }
    // The (0,0,0) corner (linear index 0) -> Morton 0 exactly.
    CHECK(codes[0] == 0U);
    // The (1,1,1) corner (linear index 7) -> Morton with all 30 low bits
    // set at quantise level 1022, i.e. morton3_30bit_from_ints(1022,1022,1022).
    CHECK(codes[7] == crd::geometry::bvh_gpu::morton3_30bit_from_ints(1022U, 1022U, 1022U));
}

TEST_CASE("union_aabb_of: empty span -> empty AABB; single span -> single AABB",
          "[morton][cpu]")
{
    crd::memory::TlsfAllocator alloc(1U * 1024U * 1024U);
    {
        crd::containers::ConstSpan<AABB3<crd::f32>> empty{};
        const auto u = union_aabb_of(empty);
        CHECK(u.min.x > u.max.x); // empty convention
    }
    {
        const AABB3<crd::f32> single{{1.0F, 2.0F, 3.0F}, {4.0F, 5.0F, 6.0F}};
        const auto u = union_aabb_of(
            crd::containers::ConstSpan<AABB3<crd::f32>>(&single, 1));
        CHECK(u.min.x == 1.0F); CHECK(u.max.x == 4.0F);
        CHECK(u.min.y == 2.0F); CHECK(u.max.y == 5.0F);
        CHECK(u.min.z == 3.0F); CHECK(u.max.z == 6.0F);
    }
}

// =========================================================================
// GPU dispatch -- per v9-prereq-test-harness discipline
// =========================================================================

TEST_CASE("v9a-a GPU Morton matches CPU oracle byte-for-byte",
          "[morton][gpu][bit-compare]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping GPU Morton dispatch test");
        return;
    }
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    crd::rhi::ValidationCapture capture(*instance);

    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonGpuPipeline pipeline(*device, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    // 1024 random-ish AABBs inside a unit cube.
    crd::containers::Array<AABB3<crd::f32>> aabbs(&alloc);
    constexpr crd::u32 count = 1024U;
    for (crd::u32 i = 0; i < count; ++i)
    {
        const float fx = static_cast<float>((i * 2654435761U) & 0xFFFFU) / 65536.0F;
        const float fy = static_cast<float>((i * 40503U)       & 0xFFFFU) / 65536.0F;
        const float fz = static_cast<float>((i * 1664525U)     & 0xFFFFU) / 65536.0F;
        const Vec3<crd::f32> c{fx, fy, fz};
        const float r = 0.01F;
        aabbs.push_back({{c.x - r, c.y - r, c.z - r}, {c.x + r, c.y + r, c.z + r}});
    }
    const AABB3<crd::f32> scene{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
    const auto aabb_span =
        crd::containers::ConstSpan<AABB3<crd::f32>>(aabbs.data(), aabbs.size());

    const auto cpu_codes = compute_morton_codes_cpu(aabb_span, scene, &alloc);
    const auto gpu_codes = pipeline.dispatch_morton_codes(aabb_span, scene, &alloc);
    REQUIRE(cpu_codes.size() == count);
    REQUIRE(gpu_codes.size() == count);

    const auto cmp = crd::test::bit_compare<crd::u32>(
        crd::containers::ConstSpan<crd::u32>(cpu_codes.data(), cpu_codes.size()),
        crd::containers::ConstSpan<crd::u32>(gpu_codes.data(), gpu_codes.size()));
    if (!cmp.ok)
    {
        INFO("first mismatch at " << cmp.first_mismatch_index
             << ": cpu=" << cmp.cpu_value
             << " gpu=" << cmp.gpu_value);
    }
    CHECK(cmp.ok);
    CHECK(cmp.compared_count == count);

    // Dump captured validation messages straight to stderr so they
    // print regardless of Catch scoping (UNSCOPED_INFO is unreliable
    // across Catch2 minor versions; stderr always works).
    if (capture.error_or_warning_count() != 0U)
    {
        for (const auto& msg : capture.messages())
        {
            std::fprintf(stderr,
                          "[vk-validation] severity=%d VUID=%d text=%s\n",
                          static_cast<int>(msg.severity),
                          msg.message_id_number,
                          msg.message_text.c_str());
        }
    }
    CHECK(capture.error_count()   == 0U);
    CHECK(capture.warning_count() == 0U);

    device->wait_idle();
}

TEST_CASE("v9a-a GPU Morton is deterministic across 3 dispatches",
          "[morton][gpu][determinism]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping GPU determinism test");
        return;
    }
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    crd::rhi::ValidationCapture capture(*instance);

    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonGpuPipeline pipeline(*device, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    crd::containers::Array<AABB3<crd::f32>> aabbs(&alloc);
    constexpr crd::u32 count = 256U;
    for (crd::u32 i = 0; i < count; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        aabbs.push_back({{t, t, t}, {t + 0.01F, t + 0.01F, t + 0.01F}});
    }
    const AABB3<crd::f32> scene{{0.0F, 0.0F, 0.0F}, {1.1F, 1.1F, 1.1F}};
    const auto aabb_span =
        crd::containers::ConstSpan<AABB3<crd::f32>>(aabbs.data(), aabbs.size());

    crd::containers::Array<crd::u32> snapshot(&alloc);
    const bool deterministic = crd::test::gpu_determinism_check(
        [&]{
            snapshot = pipeline.dispatch_morton_codes(aabb_span, scene, &alloc);
        },
        [&]() -> crd::containers::ConstSpan<crd::u8> {
            return {reinterpret_cast<const crd::u8*>(snapshot.data()),
                    snapshot.size() * sizeof(crd::u32)};
        },
        /*rounds*/ 3);
    CHECK(deterministic);

    CHECK(capture.error_count()   == 0U);
    CHECK(capture.warning_count() == 0U);

    device->wait_idle();
}

// =========================================================================
// v9a-a-async-compute: cross-queue-family dispatch path
// =========================================================================

TEST_CASE("v9a-a-async-compute: async dispatch matches sync dispatch byte-for-byte",
          "[morton][gpu][async]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping GPU async-compute test");
        return;
    }
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    crd::rhi::ValidationCapture capture(*instance);

    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonGpuPipeline pipeline(*device, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    crd::containers::Array<AABB3<crd::f32>> aabbs(&alloc);
    constexpr crd::u32 count = 512U;
    for (crd::u32 i = 0; i < count; ++i)
    {
        const crd::f32 fx = static_cast<crd::f32>((i * 2654435761U) & 0xFFFFU) / 65536.0F;
        const crd::f32 fy = static_cast<crd::f32>((i * 40503U)       & 0xFFFFU) / 65536.0F;
        const crd::f32 fz = static_cast<crd::f32>((i * 1664525U)     & 0xFFFFU) / 65536.0F;
        aabbs.push_back({{fx, fy, fz}, {fx + 0.01F, fy + 0.01F, fz + 0.01F}});
    }
    const AABB3<crd::f32> scene{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
    const auto aabb_span =
        crd::containers::ConstSpan<AABB3<crd::f32>>(aabbs.data(), aabbs.size());

    const auto sync_codes  = pipeline.dispatch_morton_codes(aabb_span, scene, &alloc);
    const auto async_codes = pipeline.dispatch_morton_codes_async(aabb_span, scene, &alloc);

    REQUIRE(sync_codes.size()  == count);
    REQUIRE(async_codes.size() == count);
    const auto cmp = crd::test::bit_compare<crd::u32>(
        crd::containers::ConstSpan<crd::u32>(sync_codes.data(),  sync_codes.size()),
        crd::containers::ConstSpan<crd::u32>(async_codes.data(), async_codes.size()));
    if (!cmp.ok)
    {
        for (const auto& msg : capture.messages())
        {
            std::fprintf(stderr,
                          "[vk-validation async] severity=%d VUID=%d text=%s\n",
                          static_cast<int>(msg.severity),
                          msg.message_id_number,
                          msg.message_text.c_str());
        }
    }
    CHECK(cmp.ok);
    CHECK(cmp.compared_count == count);

    // The discriminating contract: cross-queue-family submit must
    // stay validation-silent on GPUs with a dedicated compute family
    // (the bug that bit us at v9a-a — see vulkan_backend.cpp surface
    // omission fix + the new create_command_buffer_for_queue virtual).
    if (capture.error_or_warning_count() != 0U)
    {
        for (const auto& msg : capture.messages())
        {
            std::fprintf(stderr,
                          "[vk-validation async] severity=%d VUID=%d text=%s\n",
                          static_cast<int>(msg.severity),
                          msg.message_id_number,
                          msg.message_text.c_str());
        }
    }
    CHECK(capture.error_count()   == 0U);
    CHECK(capture.warning_count() == 0U);

    if (device->has_dedicated_compute_queue())
    {
        INFO("Verified async-compute path on a GPU with a DEDICATED compute family "
              "(the discriminating case for v9a-a-async-compute).");
    }
    else
    {
        INFO("GPU reports no dedicated compute family; async path aliases sync path "
              "via the D9 pointer-identity contract. Test still validates the routing logic.");
    }

    device->wait_idle();
}

TEST_CASE("v9a-a GPU Morton perf budget: 256k AABBs end-to-end",
          "[morton][gpu][perf]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping perf test");
        return;
    }
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonGpuPipeline pipeline(*device, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    crd::containers::Array<AABB3<crd::f32>> aabbs(&alloc);
    constexpr crd::u32 count = 256U * 1024U; // 256k -- scaled-down from 1M
                                              // budget to keep CI green
                                              // on slower test hardware.
    for (crd::u32 i = 0; i < count; ++i)
    {
        const float t = static_cast<float>(i & 0xFFFFU) / 65536.0F;
        aabbs.push_back({{t, t, t}, {t + 0.001F, t + 0.001F, t + 0.001F}});
    }
    const AABB3<crd::f32> scene{{0.0F, 0.0F, 0.0F}, {1.1F, 1.1F, 1.1F}};
    const auto aabb_span =
        crd::containers::ConstSpan<AABB3<crd::f32>>(aabbs.data(), aabbs.size());

    // Wall-clock budget includes CPU-side staging upload, GPU dispatch,
    // fence wait, readback. The pure GPU kernel is sub-ms; the wall-
    // clock is dominated by validation-layer + debug-allocator overhead
    // on debug builds (~50x slower than release; first sweep 2026-05-18
    // saw 11s on win-debug for the same workload that runs in <200ms on
    // release). Tiered budget: 200ms baseline, 30000ms (30s) ceiling on
    // assert-enabled builds so the debug path doesn't false-positive
    // under the validation-layer cost. Tightens to the published
    // <0.5ms/1M GPU-only at v9a-close once timestamp queries isolate
    // kernel cost from host overhead.
    #if defined(NDEBUG)
        constexpr double budget_ms = 200.0;
    #else
        constexpr double budget_ms = 30000.0;
    #endif
    CRD_PERF_BUDGET_LE("morton_256k_aabbs_e2e", budget_ms, [&]{
        const auto codes = pipeline.dispatch_morton_codes(aabb_span, scene, &alloc);
        REQUIRE(codes.size() == count);
    });
    (void)budget_ms; // CRD_ASSERT_MSG compiles out under NDEBUG; silence C4189.

    device->wait_idle();
}
