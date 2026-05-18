// ---------------------------------------------------------------------------
// crd-geometry-bvh-gpu v9a-a-typed -- Length<T> AABB wrapper tests.
//
// Strip-compute-retag pattern per ADR-0078 §5 D34: typed AABB at the
// API surface is stripped to raw f32 at the boundary, dispatched
// through the existing raw kernel, returns dimensionless u32 morton
// codes (Morton codes are bit indices, not lengths -- no Dim).
//
// Test contract: the typed entry must produce BYTE-IDENTICAL output to
// the raw entry when given equivalent input. Round-trip exactness is
// the discriminating property of a correct strip-compute-retag wrapper.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/bvh_gpu/dispatch.hpp>
#include <crd/geometry/bvh_gpu/morton.hpp>
#include <crd/geometry/bvh_gpu/morton_typed.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/rhi/vulkan_backend.hpp>
#include <crd/rhi/vulkan_validation_capture.hpp>
#include <crd/test_helpers/gpu_compare.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <cstdlib>

namespace fs = crd::platform::fs;

namespace
{

[[nodiscard]] bool headless_requested() noexcept
{
    const char* v = std::getenv("CRD_PLATFORM_HEADLESS");
    return v != nullptr && v[0] == '1';
}

using crd::geometry::bvh_gpu::AABB3T;
using crd::geometry::bvh_gpu::compute_morton_codes_cpu;
using crd::geometry::bvh_gpu::compute_morton_codes_cpu_typed;
using crd::geometry::bvh_gpu::dispatch_morton_codes_typed;
using crd::geometry::bvh_gpu::MortonGpuPipeline;
using crd::geometry::primitives::AABB3;
using crd::math::Vec3;
using crd::units::dim::Length;
using crd::units::Length32;

} // namespace

// =========================================================================
// CPU strip-compute-retag round-trip exactness
// =========================================================================

TEST_CASE("v9a-a-typed CPU: typed entry matches raw entry byte-for-byte",
          "[morton][typed][round-trip]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);

    // Build identical input as both raw and typed.
    crd::containers::Array<AABB3<crd::f32>> raw_aabbs(&alloc);
    crd::containers::Array<AABB3T<Length, crd::f32>> typed_aabbs(&alloc);
    constexpr crd::u32 kCount = 256U;
    for (crd::u32 i = 0; i < kCount; ++i)
    {
        const crd::f32 fx = static_cast<crd::f32>((i * 2654435761U) & 0xFFFFU) / 65536.0F;
        const crd::f32 fy = static_cast<crd::f32>((i * 40503U)       & 0xFFFFU) / 65536.0F;
        const crd::f32 fz = static_cast<crd::f32>((i * 1664525U)     & 0xFFFFU) / 65536.0F;
        const crd::f32 r  = 0.01F;
        raw_aabbs.push_back({{fx - r, fy - r, fz - r}, {fx + r, fy + r, fz + r}});

        AABB3T<Length, crd::f32> t{};
        t.min.x = Length32{fx - r}; t.min.y = Length32{fy - r}; t.min.z = Length32{fz - r};
        t.max.x = Length32{fx + r}; t.max.y = Length32{fy + r}; t.max.z = Length32{fz + r};
        typed_aabbs.push_back(t);
    }
    const AABB3<crd::f32> raw_scene{{-0.5F, -0.5F, -0.5F}, {1.5F, 1.5F, 1.5F}};
    AABB3T<Length, crd::f32> typed_scene{};
    typed_scene.min = Vec3<Length32>{Length32{-0.5F}, Length32{-0.5F}, Length32{-0.5F}};
    typed_scene.max = Vec3<Length32>{Length32{ 1.5F}, Length32{ 1.5F}, Length32{ 1.5F}};

    const auto raw_codes = compute_morton_codes_cpu(
        crd::containers::ConstSpan<AABB3<crd::f32>>(raw_aabbs.data(), raw_aabbs.size()),
        raw_scene, &alloc);
    const auto typed_codes = compute_morton_codes_cpu_typed<Length, crd::f32>(
        crd::containers::ConstSpan<AABB3T<Length, crd::f32>>(typed_aabbs.data(), typed_aabbs.size()),
        typed_scene, &alloc);

    REQUIRE(raw_codes.size()   == kCount);
    REQUIRE(typed_codes.size() == kCount);
    const auto cmp = crd::test::bit_compare<crd::u32>(
        crd::containers::ConstSpan<crd::u32>(raw_codes.data(),   raw_codes.size()),
        crd::containers::ConstSpan<crd::u32>(typed_codes.data(), typed_codes.size()));
    if (!cmp.ok)
    {
        INFO("first mismatch at " << cmp.first_mismatch_index
              << " raw=" << cmp.cpu_value
              << " typed=" << cmp.gpu_value);
    }
    CHECK(cmp.ok);
    CHECK(cmp.compared_count == kCount);
}

TEST_CASE("v9a-a-typed CPU: union-scene overload matches raw union-scene overload",
          "[morton][typed][round-trip]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);

    crd::containers::Array<AABB3<crd::f32>> raw_aabbs(&alloc);
    crd::containers::Array<AABB3T<Length, crd::f32>> typed_aabbs(&alloc);
    constexpr crd::u32 kCount = 64U;
    for (crd::u32 i = 0; i < kCount; ++i)
    {
        const crd::f32 t = static_cast<crd::f32>(i) / static_cast<crd::f32>(kCount);
        raw_aabbs.push_back({{t, t, t}, {t + 0.05F, t + 0.05F, t + 0.05F}});

        AABB3T<Length, crd::f32> tt{};
        tt.min = Vec3<Length32>{Length32{t}, Length32{t}, Length32{t}};
        tt.max = Vec3<Length32>{Length32{t + 0.05F}, Length32{t + 0.05F}, Length32{t + 0.05F}};
        typed_aabbs.push_back(tt);
    }

    const auto raw_codes = compute_morton_codes_cpu(
        crd::containers::ConstSpan<AABB3<crd::f32>>(raw_aabbs.data(), raw_aabbs.size()),
        &alloc);
    const auto typed_codes = compute_morton_codes_cpu_typed<Length, crd::f32>(
        crd::containers::ConstSpan<AABB3T<Length, crd::f32>>(typed_aabbs.data(), typed_aabbs.size()),
        &alloc);

    REQUIRE(raw_codes.size()   == kCount);
    REQUIRE(typed_codes.size() == kCount);
    const auto cmp = crd::test::bit_compare<crd::u32>(
        crd::containers::ConstSpan<crd::u32>(raw_codes.data(),   raw_codes.size()),
        crd::containers::ConstSpan<crd::u32>(typed_codes.data(), typed_codes.size()));
    CHECK(cmp.ok);
}

// =========================================================================
// GPU strip-compute-retag round-trip
// =========================================================================

TEST_CASE("v9a-a-typed GPU: typed dispatch matches raw dispatch byte-for-byte",
          "[morton][typed][gpu][round-trip]")
{
    if (headless_requested())
    {
        SUCCEED("CRD_PLATFORM_HEADLESS=1, skipping GPU typed-dispatch test");
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

    crd::containers::Array<AABB3<crd::f32>> raw_aabbs(&alloc);
    crd::containers::Array<AABB3T<Length, crd::f32>> typed_aabbs(&alloc);
    constexpr crd::u32 kCount = 512U;
    for (crd::u32 i = 0; i < kCount; ++i)
    {
        const crd::f32 fx = static_cast<crd::f32>((i * 2654435761U) & 0xFFFFU) / 65536.0F;
        const crd::f32 fy = static_cast<crd::f32>((i * 40503U)       & 0xFFFFU) / 65536.0F;
        const crd::f32 fz = static_cast<crd::f32>((i * 1664525U)     & 0xFFFFU) / 65536.0F;
        const crd::f32 r  = 0.01F;
        raw_aabbs.push_back({{fx - r, fy - r, fz - r}, {fx + r, fy + r, fz + r}});

        AABB3T<Length, crd::f32> tt{};
        tt.min = Vec3<Length32>{Length32{fx - r}, Length32{fy - r}, Length32{fz - r}};
        tt.max = Vec3<Length32>{Length32{fx + r}, Length32{fy + r}, Length32{fz + r}};
        typed_aabbs.push_back(tt);
    }
    const AABB3<crd::f32> raw_scene{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
    AABB3T<Length, crd::f32> typed_scene{};
    typed_scene.min = Vec3<Length32>{Length32{0.0F}, Length32{0.0F}, Length32{0.0F}};
    typed_scene.max = Vec3<Length32>{Length32{1.0F}, Length32{1.0F}, Length32{1.0F}};

    const auto raw_codes = pipeline.dispatch_morton_codes(
        crd::containers::ConstSpan<AABB3<crd::f32>>(raw_aabbs.data(), raw_aabbs.size()),
        raw_scene, &alloc);
    const auto typed_codes = dispatch_morton_codes_typed<Length, crd::f32>(
        pipeline,
        crd::containers::ConstSpan<AABB3T<Length, crd::f32>>(typed_aabbs.data(), typed_aabbs.size()),
        typed_scene, &alloc);

    REQUIRE(raw_codes.size()   == kCount);
    REQUIRE(typed_codes.size() == kCount);
    const auto cmp = crd::test::bit_compare<crd::u32>(
        crd::containers::ConstSpan<crd::u32>(raw_codes.data(),   raw_codes.size()),
        crd::containers::ConstSpan<crd::u32>(typed_codes.data(), typed_codes.size()));
    CHECK(cmp.ok);
    CHECK(cmp.compared_count == kCount);

    CHECK(capture.error_count()   == 0U);
    CHECK(capture.warning_count() == 0U);

    device->wait_idle();
}
