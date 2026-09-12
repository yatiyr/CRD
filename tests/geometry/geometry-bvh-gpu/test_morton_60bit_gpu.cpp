// ---------------------------------------------------------------------------
// crd-geometry-bvh-gpu v9a-60bit-gpu -- u64 60-bit Morton GPU dispatch tests.
//
// Mirror of test_morton.cpp's GPU bit_compare for the 60-bit u64 path.
// Requires `shaderInt64`; gracefully skips when unavailable. Test
// contract: GPU output is byte-identical to the CPU 60-bit oracle.
// v17-i-c: migrated onto crd::gpu::VulkanComputeContext (ADR-0099).
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh_gpu/dispatch.hpp>
#include <crd/geometry/bvh_gpu/morton_60bit.hpp>
#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/test_helpers/gpu_compare.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace fs = crd::platform::fs;

namespace
{

[[nodiscard]] bool headless_requested() noexcept
{
    const char* v = std::getenv("CRD_PLATFORM_HEADLESS");
    return v != nullptr && v[0] == '1';
}

using crd::geometry::bvh_gpu::compute_morton_codes_cpu_60bit;
using crd::geometry::bvh_gpu::MortonGpu60BitPipeline;
using crd::geometry::primitives::AABB3;

} // namespace

TEST_CASE("v9a-60bit-gpu: graceful-skip when shaderInt64 unavailable",
          "[morton60][gpu][capability]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping GPU capability test");
        return;
    }
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    auto ctx = crd::gpu::create_vulkan_gpu_context({});
    REQUIRE(ctx != nullptr);
    auto* vkctx = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonGpu60BitPipeline pipeline(compute, shader_dir.generic());

    if (compute.supports_shader_int64())
    {
        INFO("Device reports shaderInt64 support; pipeline expected valid.");
        CHECK(pipeline.is_valid());
    }
    else
    {
        INFO("Device does NOT support shaderInt64; pipeline expected invalid (graceful skip).");
        CHECK_FALSE(pipeline.is_valid());
    }
}

TEST_CASE("v9a-60bit-gpu: GPU 60-bit Morton matches CPU oracle byte-for-byte",
          "[morton60][gpu][bit-compare]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping GPU 60-bit dispatch test");
        return;
    }
    crd::memory::TlsfAllocator alloc(16U * 1024U * 1024U);

    auto ctx = crd::gpu::create_vulkan_gpu_context({});
    REQUIRE(ctx != nullptr);
    auto* vkctx = static_cast<crd::gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vkctx, &alloc);
    REQUIRE(compute.valid());

    if (!compute.supports_shader_int64())
    {
        SUCCEED("shaderInt64 unavailable on this device; skipping bit-compare test.");
        return;
    }

    const auto shader_dir = fs::executable_dir() / crd::containers::StringView{"shaders"};
    MortonGpu60BitPipeline pipeline(compute, shader_dir.generic());
    REQUIRE(pipeline.is_valid());

    crd::containers::Array<AABB3<crd::f32>> aabbs(&alloc);
    constexpr crd::u32 count = 1024U;
    for (crd::u32 i = 0; i < count; ++i)
    {
        const crd::f32 fx = static_cast<crd::f32>((i * 2654435761U) & 0xFFFFU) / 65536.0F;
        const crd::f32 fy = static_cast<crd::f32>((i * 40503U)       & 0xFFFFU) / 65536.0F;
        const crd::f32 fz = static_cast<crd::f32>((i * 1664525U)     & 0xFFFFU) / 65536.0F;
        const crd::f32 r  = 0.01F;
        aabbs.push_back({{fx - r, fy - r, fz - r}, {fx + r, fy + r, fz + r}});
    }
    const AABB3<crd::f32> scene{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
    const auto span =
        crd::containers::ConstSpan<AABB3<crd::f32>>(aabbs.data(), aabbs.size());

    const auto cpu_codes = compute_morton_codes_cpu_60bit(span, scene, &alloc);
    const auto gpu_codes = pipeline.dispatch_morton_codes_60bit(span, scene, &alloc);
    REQUIRE(cpu_codes.size() == count);
    REQUIRE(gpu_codes.size() == count);

    // bit_compare requires an unsigned integral type — std::uint64_t fits.
    const auto cmp = crd::test::bit_compare<std::uint64_t>(
        crd::containers::ConstSpan<std::uint64_t>(cpu_codes.data(), cpu_codes.size()),
        crd::containers::ConstSpan<std::uint64_t>(gpu_codes.data(), gpu_codes.size()));
    if (!cmp.ok)
    {
        std::fprintf(stderr,
                      "[60bit] first mismatch at %zu cpu=%llu gpu=%llu\n",
                      static_cast<size_t>(cmp.first_mismatch_index),
                      static_cast<unsigned long long>(cmp.cpu_value),
                      static_cast<unsigned long long>(cmp.gpu_value));
    }
    CHECK(cmp.ok);
    CHECK(cmp.compared_count == count);
}
