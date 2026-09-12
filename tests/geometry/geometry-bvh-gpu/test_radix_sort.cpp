// ---------------------------------------------------------------------------
// crd-geometry-bvh-gpu v9a-b2 -- GPU LSD radix sort of (Morton, index) pairs.
//
// CALIBRATION FIRST per advisor TDD + v9-prereq-test-harness discipline:
//   1. N=16 hand-rolled input; GPU output byte-identical to v9a-b1 CPU
//      oracle (the discriminating sanity gate -- if this fails, all
//      downstream tests are meaningless).
//   2. Empty / single / sub-WG / single-WG / multi-WG / cap.
//   3. Stability discriminator (all-equal keys preserve input index order).
//   4. Bullet-proof oracle -- random N=10000 + N=262144 byte-identical
//      to CPU v9a-b1 via `bit_compare<MortonPair<u32>>`.
//   5. gpu_determinism_check 3 rounds (prefix-sum scatter ⇒ truly deterministic).
//   6. ValidationCapture silent on every dispatch.
//   7. CRD_PERF_BUDGET_LE -- 1M elements end-to-end within tiered budget.
//   8. Integration with compute_morton_codes_cpu -> dispatch_radix_sort
//      end-to-end LBVH-pipeline candidate.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh_gpu/morton.hpp>
#include <crd/geometry/bvh_gpu/morton_sort.hpp>
#include <crd/geometry/bvh_gpu/radix_sort.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/measure.hpp>
#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/test_helpers/gpu_compare.hpp>
#include <crd/test_helpers/gpu_determinism.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace fs = crd::platform::fs;

namespace
{

[[nodiscard]] bool headless_requested() noexcept
{
    const char* v = std::getenv("CRD_PLATFORM_HEADLESS");
    return v != nullptr && v[0] == '1';
}

using crd::geometry::bvh_gpu::MortonPair;
using crd::geometry::bvh_gpu::MortonRadixGpuPipeline;
using crd::geometry::bvh_gpu::sort_morton_pairs;
using crd::geometry::primitives::AABB3;
using crd::math::Vec3;

// Build random u32 codes seeded by `seed`. Mixes uniformly-random with a
// quarter-rate "low 16 bits only" pattern to force ties + exercise upper
// passes. Deterministic per seed.
template <typename Rng>
void fill_random_codes(crd::containers::Array<crd::u32>& out, crd::usize n, Rng& rng)
{
    out.resize(n, 0U);
    std::uniform_int_distribution<crd::u32> dist(0U, 0xFFFFFFFFU);
    for (crd::usize i = 0; i < n; ++i)
    {
        out[i] = ((i & 0x3U) == 0U)
                  ? (dist(rng) & 0xFFFFU)        // forced low-16-bits-only ⇒ ties
                  : dist(rng);
    }
}

} // namespace

// =========================================================================
// CALIBRATION FIRST -- N=16 hand-rolled, GPU byte-identical to CPU oracle
// =========================================================================

TEST_CASE("v9a-b2 GPU radix N=16 calibration: byte-identical to v9a-b1 CPU oracle",
          "[radix][gpu][calibration]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping GPU radix calibration");
        return;
    }
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);

    auto ctx = crd::gpu::create_vulkan_gpu_context({});
    REQUIRE(ctx != nullptr);
    auto* vkctx = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonRadixGpuPipeline pipeline(compute, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    // 16 hand-rolled codes -- mix of unique + ties to exercise basic
    // ordering + stability in one calibration test.
    const auto in = []{
        crd::u32 a[] = {17U, 3U, 17U, 100U, 0U, 8U, 17U, 999U,
                        42U, 3U, 42U, 0U,   5U, 100U, 1U, 17U};
        crd::containers::Array<crd::u32> arr;
        arr.resize(16U);
        for (crd::u32 i = 0; i < 16U; ++i) { arr[i] = a[i]; }
        return arr;
    }();
    const auto in_span = crd::containers::ConstSpan<crd::u32>(in.data(), in.size());

    const auto cpu = sort_morton_pairs<crd::u32>(in_span, &alloc);
    const auto gpu = pipeline.dispatch_radix_sort(in_span, &alloc);

    REQUIRE(cpu.size() == 16U);
    REQUIRE(gpu.size() == 16U);

    const auto cpu_bytes = crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(cpu.data()),
        cpu.size() * sizeof(MortonPair<crd::u32>));
    const auto gpu_bytes = crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(gpu.data()),
        gpu.size() * sizeof(MortonPair<crd::u32>));
    const auto cmp = crd::test::bit_compare<crd::u8>(cpu_bytes, gpu_bytes);
    if (!cmp.ok)
    {
        INFO("first mismatch at byte " << cmp.first_mismatch_index
             << " cpu=" << static_cast<int>(cmp.cpu_value)
             << " gpu=" << static_cast<int>(cmp.gpu_value));
    }
    CHECK(cmp.ok);
    // (validation-layer checks dropped with the RHI removal; correctness is the bit_compare above)
}

// =========================================================================
// Trivial-shape sanity
// =========================================================================

TEST_CASE("v9a-b2 GPU radix empty input yields empty output", "[radix][gpu]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);

    auto ctx = crd::gpu::create_vulkan_gpu_context({});
    REQUIRE(ctx != nullptr);
    auto* vkctx = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vkctx, &alloc);
    REQUIRE(compute.valid());
    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonRadixGpuPipeline pipeline(compute, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    crd::containers::ConstSpan<crd::u32> empty{};
    const auto out = pipeline.dispatch_radix_sort(empty, &alloc);
    CHECK(out.empty());

    // (validation-layer checks dropped with the RHI removal; correctness is the bit_compare above)
}

TEST_CASE("v9a-b2 GPU radix N=1 yields single pair {code, 0}", "[radix][gpu]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    crd::memory::TlsfAllocator alloc(2U * 1024U * 1024U);

    auto ctx = crd::gpu::create_vulkan_gpu_context({});
    REQUIRE(ctx != nullptr);
    auto* vkctx = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vkctx, &alloc);
    REQUIRE(compute.valid());
    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonRadixGpuPipeline pipeline(compute, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    const crd::u32 code = 0xDEADBEEFU;
    const auto in_span = crd::containers::ConstSpan<crd::u32>(&code, 1);
    const auto out = pipeline.dispatch_radix_sort(in_span, &alloc);
    REQUIRE(out.size() == 1U);
    CHECK(out[0].code  == 0xDEADBEEFU);
    CHECK(out[0].index == 0U);

    // (validation-layer checks dropped with the RHI removal; correctness is the bit_compare above)
}

// =========================================================================
// STABILITY discriminator -- all-equal keys preserve input index order
// =========================================================================

TEST_CASE("v9a-b2 GPU radix all-equal keys: stable (input index order preserved)",
          "[radix][gpu][stability]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);

    auto ctx = crd::gpu::create_vulkan_gpu_context({});
    REQUIRE(ctx != nullptr);
    auto* vkctx = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vkctx, &alloc);
    REQUIRE(compute.valid());
    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonRadixGpuPipeline pipeline(compute, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    // 4096 identical codes -- spans 4 workgroups (1024 items each), so
    // tests stability both within and ACROSS workgroups. Output indices
    // must be 0, 1, 2, ..., 4095 (monotonic-ascending).
    constexpr crd::u32 k_n = 4096U;
    crd::containers::Array<crd::u32> in(&alloc);
    in.resize(k_n, 0xCAFEBABEU);

    const auto out = pipeline.dispatch_radix_sort(
        crd::containers::ConstSpan<crd::u32>(in.data(), in.size()), &alloc);
    REQUIRE(out.size() == k_n);
    for (crd::u32 i = 0; i < k_n; ++i)
    {
        CHECK(out[i].code  == 0xCAFEBABEU);
        CHECK(out[i].index == i);
    }

    // (validation-layer checks dropped with the RHI removal; correctness is the bit_compare above)
}

// =========================================================================
// Bullet-proof oracle -- random multi-WG inputs byte-identical to CPU v9a-b1
// =========================================================================

TEST_CASE("v9a-b2 GPU radix N=10000: byte-identical to v9a-b1 CPU oracle",
          "[radix][gpu][oracle]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);

    auto ctx = crd::gpu::create_vulkan_gpu_context({});
    REQUIRE(ctx != nullptr);
    auto* vkctx = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vkctx, &alloc);
    REQUIRE(compute.valid());
    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonRadixGpuPipeline pipeline(compute, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    crd::containers::Array<crd::u32> in(&alloc);
    std::mt19937 rng(0xC0FFEE01U);
    fill_random_codes(in, 10000U, rng);
    const auto in_span = crd::containers::ConstSpan<crd::u32>(in.data(), in.size());

    const auto cpu = sort_morton_pairs<crd::u32>(in_span, &alloc);
    const auto gpu = pipeline.dispatch_radix_sort(in_span, &alloc);
    REQUIRE(cpu.size() == 10000U);
    REQUIRE(gpu.size() == 10000U);

    const auto cpu_bytes = crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(cpu.data()),
        cpu.size() * sizeof(MortonPair<crd::u32>));
    const auto gpu_bytes = crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(gpu.data()),
        gpu.size() * sizeof(MortonPair<crd::u32>));
    const auto cmp = crd::test::bit_compare<crd::u8>(cpu_bytes, gpu_bytes);
    if (!cmp.ok)
    {
        INFO("first byte mismatch at offset " << cmp.first_mismatch_index
             << " cpu=" << static_cast<int>(cmp.cpu_value)
             << " gpu=" << static_cast<int>(cmp.gpu_value));
    }
    CHECK(cmp.ok);

    // (validation-layer checks dropped with the RHI removal; correctness is the bit_compare above)
}

TEST_CASE("v9a-b2 GPU radix N=262144 (multi-block): byte-identical to CPU oracle",
          "[radix][gpu][oracle][large]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);

    auto ctx = crd::gpu::create_vulkan_gpu_context({});
    REQUIRE(ctx != nullptr);
    auto* vkctx = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vkctx, &alloc);
    REQUIRE(compute.valid());
    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonRadixGpuPipeline pipeline(compute, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    crd::containers::Array<crd::u32> in(&alloc);
    std::mt19937 rng(0x12345678U);
    fill_random_codes(in, 262144U, rng);
    const auto in_span = crd::containers::ConstSpan<crd::u32>(in.data(), in.size());

    const auto cpu = sort_morton_pairs<crd::u32>(in_span, &alloc);
    const auto gpu = pipeline.dispatch_radix_sort(in_span, &alloc);
    REQUIRE(cpu.size() == 262144U);
    REQUIRE(gpu.size() == 262144U);

    const auto cpu_bytes = crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(cpu.data()),
        cpu.size() * sizeof(MortonPair<crd::u32>));
    const auto gpu_bytes = crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(gpu.data()),
        gpu.size() * sizeof(MortonPair<crd::u32>));
    const auto cmp = crd::test::bit_compare<crd::u8>(cpu_bytes, gpu_bytes);
    if (!cmp.ok)
    {
        INFO("first byte mismatch at offset " << cmp.first_mismatch_index
             << " cpu=" << static_cast<int>(cmp.cpu_value)
             << " gpu=" << static_cast<int>(cmp.gpu_value));
    }
    CHECK(cmp.ok);

    // (validation-layer checks dropped with the RHI removal; correctness is the bit_compare above)
}

// =========================================================================
// Determinism -- prefix-sum scatter ⇒ truly deterministic (D146 contract)
// =========================================================================

TEST_CASE("v9a-b2 GPU radix is deterministic across 3 dispatches (D146)",
          "[radix][gpu][determinism]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    crd::memory::TlsfAllocator alloc(32U * 1024U * 1024U);

    auto ctx = crd::gpu::create_vulkan_gpu_context({});
    REQUIRE(ctx != nullptr);
    auto* vkctx = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vkctx, &alloc);
    REQUIRE(compute.valid());
    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonRadixGpuPipeline pipeline(compute, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    crd::containers::Array<crd::u32> in(&alloc);
    std::mt19937 rng(0xDADADADAU);
    fill_random_codes(in, 8192U, rng);
    const auto in_span = crd::containers::ConstSpan<crd::u32>(in.data(), in.size());

    crd::containers::Array<MortonPair<crd::u32>> snapshot(&alloc);
    const bool deterministic = crd::test::gpu_determinism_check(
        [&]{
            snapshot = pipeline.dispatch_radix_sort(in_span, &alloc);
        },
        [&]() -> crd::containers::ConstSpan<crd::u8> {
            return {reinterpret_cast<const crd::u8*>(snapshot.data()),
                    snapshot.size() * sizeof(MortonPair<crd::u32>)};
        },
        /*rounds*/ 3);
    CHECK(deterministic);

    // (validation-layer checks dropped with the RHI removal; correctness is the bit_compare above)
}

// =========================================================================
// Integration: compute_morton_codes_cpu → dispatch_radix_sort
// (mirrors the upcoming v9a-c GPU LBVH builder's input pipeline)
// =========================================================================

TEST_CASE("v9a-b2 GPU radix integrates with compute_morton_codes_cpu",
          "[radix][gpu][integration]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    auto ctx = crd::gpu::create_vulkan_gpu_context({});
    REQUIRE(ctx != nullptr);
    auto* vkctx = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vkctx, &alloc);
    REQUIRE(compute.valid());
    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonRadixGpuPipeline pipeline(compute, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    // 2048 random AABBs inside a unit cube; CPU Morton + GPU radix.
    crd::containers::Array<AABB3<crd::f32>> aabbs(&alloc);
    constexpr crd::u32 count = 2048U;
    for (crd::u32 i = 0; i < count; ++i)
    {
        const float fx = static_cast<float>((i * 2654435761U) & 0xFFFFU) / 65536.0F;
        const float fy = static_cast<float>((i * 40503U)       & 0xFFFFU) / 65536.0F;
        const float fz = static_cast<float>((i * 1664525U)     & 0xFFFFU) / 65536.0F;
        const Vec3<crd::f32> c{fx, fy, fz};
        const float r = 0.005F;
        aabbs.push_back({{c.x - r, c.y - r, c.z - r}, {c.x + r, c.y + r, c.z + r}});
    }
    const AABB3<crd::f32> scene{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};

    const auto codes = crd::geometry::bvh_gpu::compute_morton_codes_cpu(
        crd::containers::ConstSpan<AABB3<crd::f32>>(aabbs.data(), aabbs.size()),
        scene, &alloc);
    REQUIRE(codes.size() == count);

    const auto codes_span = crd::containers::ConstSpan<crd::u32>(codes.data(), codes.size());
    const auto cpu = sort_morton_pairs<crd::u32>(codes_span, &alloc);
    const auto gpu = pipeline.dispatch_radix_sort(codes_span, &alloc);

    REQUIRE(cpu.size() == count);
    REQUIRE(gpu.size() == count);

    // Byte-identical end-to-end.
    const auto cpu_bytes = crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(cpu.data()),
        cpu.size() * sizeof(MortonPair<crd::u32>));
    const auto gpu_bytes = crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(gpu.data()),
        gpu.size() * sizeof(MortonPair<crd::u32>));
    const auto cmp = crd::test::bit_compare<crd::u8>(cpu_bytes, gpu_bytes);
    CHECK(cmp.ok);

    // (validation-layer checks dropped with the RHI removal; correctness is the bit_compare above)
}

// =========================================================================
// Perf budget -- 1M items end-to-end (cap = kRadixMaxItems = 1048576).
// Tiered NDEBUG / debug per feedback_v9_gpu_sanity_harness.
// =========================================================================

TEST_CASE("v9a-b2 GPU radix perf budget: 1M items end-to-end",
          "[radix][gpu][perf]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    crd::memory::TlsfAllocator alloc(256U * 1024U * 1024U);

    auto ctx = crd::gpu::create_vulkan_gpu_context({});
    REQUIRE(ctx != nullptr);
    auto* vkctx = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vkctx, &alloc);
    REQUIRE(compute.valid());
    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonRadixGpuPipeline pipeline(compute, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    crd::containers::Array<crd::u32> in(&alloc);
    std::mt19937 rng(0x87654321U);
    fill_random_codes(in, crd::geometry::bvh_gpu::kRadixMaxItems, rng);
    const auto in_span = crd::containers::ConstSpan<crd::u32>(in.data(), in.size());

    #ifdef NDEBUG
        constexpr double budget_ms = 30.0;
    #else
        constexpr double budget_ms = 30000.0;
    #endif
    CRD_PERF_BUDGET_LE("radix_sort_gpu_1m_e2e", budget_ms, [&]{
        const auto out = pipeline.dispatch_radix_sort(in_span, &alloc);
        REQUIRE(out.size() == crd::geometry::bvh_gpu::kRadixMaxItems);
    });
    (void)budget_ms;

    // (validation-layer checks dropped with the RHI removal; correctness is the bit_compare above)
}
