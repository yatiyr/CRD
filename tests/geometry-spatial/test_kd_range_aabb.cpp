// crd-geometry-spatial v5a — kd_range_aabb tests.

#include <crd/containers/array.hpp>
#include <crd/geometry/spatial/kd_range_aabb.hpp>
#include <crd/geometry/spatial/kd_tree.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>

using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::AABB3;
using crd::geometry::spatial::kd_build;
using crd::geometry::spatial::kd_range_aabb;
using crd::math::Vec3f;

namespace
{
struct AllocFixture { crd::memory::TlsfAllocator alloc{1U << 20}; };

crd::containers::Array<Vec3f> make_cloud(u32 n, u32 seed, crd::memory::IAllocator* a)
{
    crd::containers::Array<Vec3f> pts(a);
    pts.reserve(n);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> u(-1.0F, 1.0F);
    for (u32 i = 0; i < n; ++i) { pts.push_back(Vec3f{u(rng), u(rng), u(rng)}); }
    return pts;
}

bool inside(const Vec3f& p, const AABB3<f32>& b) noexcept
{
    return p.x >= b.min.x && p.x <= b.max.x
        && p.y >= b.min.y && p.y <= b.max.y
        && p.z >= b.min.z && p.z <= b.max.z;
}
} // namespace

TEST_CASE("kd_range_aabb matches brute force on random cloud", "[geometry-spatial][kd][range]")
{
    AllocFixture f{};
    auto pts = make_cloud(800U, 19U, &f.alloc);
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, &f.alloc);

    std::mt19937 rng(53U);
    std::uniform_real_distribution<f32> uc(-0.8F, 0.8F);
    std::uniform_real_distribution<f32> uh(0.05F, 0.5F);

    for (u32 trial = 0; trial < 16U; ++trial)
    {
        const Vec3f c{uc(rng), uc(rng), uc(rng)};
        const f32 hx = uh(rng);
        const f32 hy = uh(rng);
        const f32 hz = uh(rng);
        AABB3<f32> box{Vec3f{c.x - hx, c.y - hy, c.z - hz},
                       Vec3f{c.x + hx, c.y + hy, c.z + hz}};

        crd::containers::Array<u32> got(&f.alloc);
        kd_range_aabb<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                            box, got);

        crd::containers::Array<u32> expected(&f.alloc);
        for (u32 i = 0; i < pts.size(); ++i)
        {
            if (inside(pts[i], box)) { expected.push_back(i); }
        }
        std::sort(got.data(), got.data() + got.size());
        std::sort(expected.data(), expected.data() + expected.size());
        REQUIRE(got.size() == expected.size());
        for (usize i = 0; i < got.size(); ++i) { REQUIRE(got[i] == expected[i]); }
    }
}

TEST_CASE("kd_range_aabb empty query box returns no hits", "[geometry-spatial][kd][range]")
{
    AllocFixture f{};
    auto pts = make_cloud(50U, 1U, &f.alloc);
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, &f.alloc);
    crd::containers::Array<u32> got(&f.alloc);
    // Inverted box (min > max).
    AABB3<f32> bad{Vec3f{1.0F, 0.0F, 0.0F}, Vec3f{-1.0F, 0.0F, 0.0F}};
    kd_range_aabb<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()},
                        bad, got);
    REQUIRE(got.size() == 0U);
}

TEST_CASE("kd_range_aabb point exactly on box boundary is inclusive", "[geometry-spatial][kd][range]")
{
    AllocFixture f{};
    crd::containers::Array<Vec3f> pts(&f.alloc);
    pts.push_back(Vec3f{1.0F, 0.0F, 0.0F});  // exactly on +X face
    pts.push_back(Vec3f{0.0F, 0.0F, 0.0F});  // interior
    pts.push_back(Vec3f{1.5F, 0.0F, 0.0F});  // outside
    auto tree = kd_build<f32>(crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, &f.alloc);

    AABB3<f32> box{Vec3f{-1.0F, -1.0F, -1.0F}, Vec3f{1.0F, 1.0F, 1.0F}};
    crd::containers::Array<u32> got(&f.alloc);
    kd_range_aabb<f32>(tree, crd::containers::ConstSpan<Vec3f>{pts.data(), pts.size()}, box, got);
    REQUIRE(got.size() == 2U);
    std::sort(got.data(), got.data() + got.size());
    REQUIRE(got[0] == 0U);
    REQUIRE(got[1] == 1U);
}

TEST_CASE("kd_range_aabb empty tree", "[geometry-spatial][kd][range]")
{
    AllocFixture f{};
    crd::containers::ConstSpan<Vec3f> empty{};
    auto tree = kd_build<f32>(empty, &f.alloc);
    crd::containers::Array<u32> got(&f.alloc);
    AABB3<f32> box{Vec3f{-1, -1, -1}, Vec3f{1, 1, 1}};
    kd_range_aabb<f32>(tree, empty, box, got);
    REQUIRE(got.size() == 0U);
}
