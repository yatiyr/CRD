// THROWAWAY probe for v2c design — measures GJK terminating-simplex size
// distribution on overlapping inputs. Result informs whether EPA needs a full
// "puff to enclosing tetrahedron" routine for size < 4, or whether size == 4
// is the >99% case and we can ship the simple form with a fallback.
//
// DELETE this file once v2c lands.

#include <crd/containers/array.hpp>
#include <crd/geometry/convex/convex.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/transform.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::convex::gjk_distance;
using crd::geometry::primitives::Capsule3;
using crd::geometry::primitives::ConvexHullView;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::Sphere;
using crd::math::from_axis_angle;
using crd::math::Mat3;
using crd::math::Quat;
using crd::math::Transform;
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
    Vec3<f32> rand_vec(f32 lo, f32 hi) { return Vec3<f32>(range(lo, hi), range(lo, hi), range(lo, hi)); }
    Quat<f32> rand_quat()
    {
        Vec3<f32> ax = rand_vec(-1, 1);
        const f32 axlen = std::sqrt(crd::math::dot(ax, ax));
        if (axlen < 1e-3F)
        {
            ax = Vec3<f32>(1, 0, 0);
        }
        else
        {
            ax = Vec3<f32>(ax.x / axlen, ax.y / axlen, ax.z / axlen);
        }
        return from_axis_angle(ax, range(-3.14F, 3.14F));
    }
};

struct CubeHull
{
    crd::containers::Array<Vec3<f32>> verts;
    crd::containers::Array<Plane<f32>> faces;
    crd::containers::Array<u32> face_idx;
    crd::containers::Array<u32> face_off;

    explicit CubeHull(crd::memory::IAllocator* alloc, f32 half = 1.0F)
        : verts(alloc), faces(alloc), face_idx(alloc), face_off(alloc)
    {
        for (int i = 0; i < 8; ++i)
        {
            verts.push_back(Vec3<f32>((i & 4) ? half : -half, (i & 2) ? half : -half, (i & 1) ? half : -half));
        }
    }
    ConvexHullView<f32> view() const
    {
        return ConvexHullView<f32>(crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size()),
                                   crd::containers::ConstSpan<Plane<f32>>(faces.data(), faces.size()),
                                   crd::containers::ConstSpan<u32>(face_idx.data(), face_idx.size()),
                                   crd::containers::ConstSpan<u32>(face_off.data(), face_off.size()));
    }
};
} // namespace

TEST_CASE("PROBE: GJK simplex.size distribution on overlapping pairs", "[probe-v2c]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "probe");
    const CubeHull hull(&alloc, 1.0F);

    Rng rng(0xC0DEFACEU);
    int hist[5] = {0, 0, 0, 0, 0}; // size 0, 1, 2, 3, 4
    int total_overlapping = 0;
    int total_trials = 0;
    int by_kind_size4[5] = {0, 0, 0, 0, 0}; // sphere-sphere, sphere-box, sphere-capsule, box-box, hull-hull
    int by_kind_total[5] = {0, 0, 0, 0, 0};

    auto record = [&](int kind, const auto& a, const Transform<f32>& xa, const auto& b, const Transform<f32>& xb) {
        ++total_trials;
        const auto r = gjk_distance<f32>(a, xa, b, xb);
        if (r.overlapping)
        {
            ++total_overlapping;
            ++hist[r.simplex.size];
            ++by_kind_total[kind];
            if (r.simplex.size == 4)
            {
                ++by_kind_size4[kind];
            }
        }
    };

    // 500 random configurations across 5 shape-pair kinds (100 each), biased
    // toward small separations so a good fraction overlap.
    for (int trial = 0; trial < 100; ++trial)
    {
        // sphere-sphere
        {
            const f32 ra = rng.range(0.5F, 1.5F);
            const f32 rb = rng.range(0.5F, 1.5F);
            const Sphere<f32> a(Vec3<f32>(0), ra);
            const Sphere<f32> b(Vec3<f32>(0), rb);
            record(0, a, Transform<f32>(rng.rand_vec(-2, 2), Quat<f32>::identity()), b,
                   Transform<f32>(rng.rand_vec(-2, 2), Quat<f32>::identity()));
        }
        // sphere-box
        {
            const Sphere<f32> a(Vec3<f32>(0), rng.range(0.5F, 1.5F));
            const OBB3<f32> b(Vec3<f32>(0), rng.rand_vec(0.5F, 1.2F), Mat3<f32>::identity());
            record(1, a, Transform<f32>(rng.rand_vec(-2, 2), rng.rand_quat()), b,
                   Transform<f32>(rng.rand_vec(-2, 2), rng.rand_quat()));
        }
        // sphere-capsule
        {
            const Sphere<f32> a(Vec3<f32>(0), rng.range(0.5F, 1.5F));
            const Capsule3<f32> b(Vec3<f32>(0, 0, -0.5F), Vec3<f32>(0, 0, 0.5F), rng.range(0.2F, 0.7F));
            record(2, a, Transform<f32>(rng.rand_vec(-2, 2), rng.rand_quat()), b,
                   Transform<f32>(rng.rand_vec(-2, 2), rng.rand_quat()));
        }
        // box-box
        {
            const OBB3<f32> a(Vec3<f32>(0), rng.rand_vec(0.5F, 1.2F), Mat3<f32>::identity());
            const OBB3<f32> b(Vec3<f32>(0), rng.rand_vec(0.5F, 1.2F), Mat3<f32>::identity());
            record(3, a, Transform<f32>(rng.rand_vec(-2, 2), rng.rand_quat()), b,
                   Transform<f32>(rng.rand_vec(-2, 2), rng.rand_quat()));
        }
        // hull-hull
        {
            record(4, hull.view(), Transform<f32>(rng.rand_vec(-2, 2), rng.rand_quat()), hull.view(),
                   Transform<f32>(rng.rand_vec(-2, 2), rng.rand_quat()));
        }
    }

    std::printf("\n=== PROBE: GJK terminating simplex.size on overlapping pairs ===\n");
    std::printf("Total trials: %d\n", total_trials);
    std::printf("Total overlapping: %d (%.1f%%)\n", total_overlapping,
                100.0 * total_overlapping / total_trials);
    std::printf("Simplex size histogram (overlapping only):\n");
    for (int s = 0; s <= 4; ++s)
    {
        std::printf("  size %d: %4d (%5.1f%%)\n", s, hist[s],
                    total_overlapping == 0 ? 0.0 : 100.0 * hist[s] / total_overlapping);
    }
    const char* kind_names[5] = {"sphere-sphere", "sphere-box", "sphere-capsule", "box-box", "hull-hull"};
    std::printf("By-kind size==4 ratio:\n");
    for (int k = 0; k < 5; ++k)
    {
        std::printf("  %-16s: %3d / %3d (%5.1f%% size==4)\n", kind_names[k], by_kind_size4[k], by_kind_total[k],
                    by_kind_total[k] == 0 ? 0.0 : 100.0 * by_kind_size4[k] / by_kind_total[k]);
    }
    std::printf("=== end probe ===\n");

    // Sanity: should have *some* overlaps in the corpus.
    REQUIRE(total_overlapping > 0);
}
