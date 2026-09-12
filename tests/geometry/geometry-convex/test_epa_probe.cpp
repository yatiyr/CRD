// THROWAWAY probe for v2c EPA: rotated hull-vs-hull self-consistency.
//
// Question: does EPA's pathology on rotated OBB-OBB extend to other
// polyhedral pairs (hull-vs-hull, hull-vs-OBB)? If yes, EPA has a
// substrate-wide bug that needs investigation. If no, OBB-OBB is the
// only problematic case and SAT covers it.
//
// Method: same self-consistency check used for SAT in test_sat.cpp -
// after EPA reports overlap with `normal` and `depth`, translating B by
// `+normal * (depth + eps)` should make the pair NOT overlap any more
// (verified via `gjk_overlap`, which we trust independently).
//
// If this test fails on rotated hull-hull or hull-OBB, the EPA bug
// is real and substrate-wide.
//
// DELETE this file once the question is answered.

#include <crd/containers/array.hpp>
#include <crd/geometry/convex/convex.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/transform.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::convex::compute_contact;
using crd::geometry::convex::EpaResult;
using crd::geometry::convex::gjk_overlap;
using crd::geometry::primitives::ConvexHullView;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Plane;
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

TEST_CASE("PROBE: EPA self-consistency on rotated hull-vs-hull", "[probe-epa]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "epa-probe");
    CubeHull a(&alloc, 1.0F);
    CubeHull b(&alloc, 1.0F);

    Rng rng(0xDEADBEEFU);
    int passed = 0;
    int failed = 0;
    int total_overlapping = 0;
    int worst_overshoot = 0; // how many ulps past the expected separation we get
    f32 worst_relative_error = 0.0F;

    for (int trial = 0; trial < 100; ++trial)
    {
        const Transform<f32> xa(rng.rand_vec(-1.5F, 1.5F), rng.rand_quat());
        const Transform<f32> xb(rng.rand_vec(-1.5F, 1.5F), rng.rand_quat());
        const auto contact = compute_contact<f32>(a.view(), xa, b.view(), xb);
        if (!contact.has_value())
        {
            continue;
        }
        ++total_overlapping;
        REQUIRE(contact->converged);

        // Self-consistency: translate B by +normal*(depth + eps) and verify
        // gjk_overlap reports they are NOT overlapping.
        const f32 eps = 1e-3F;
        const Vec3<f32> nudge(contact->normal.x * (contact->depth + eps), contact->normal.y * (contact->depth + eps),
                              contact->normal.z * (contact->depth + eps));
        const Transform<f32> xb_separated(
            Vec3<f32>(xb.translation.x + nudge.x, xb.translation.y + nudge.y, xb.translation.z + nudge.z), xb.rotation);
        const bool still_overlap = gjk_overlap<f32>(a.view(), xa, b.view(), xb_separated);

        if (still_overlap)
        {
            ++failed;
            // Measure how much overshoot would be needed.
            f32 step = 2.0F * contact->depth;
            while (step < 100.0F)
            {
                const Vec3<f32> big_nudge(contact->normal.x * step, contact->normal.y * step, contact->normal.z * step);
                const Transform<f32> xb_test(Vec3<f32>(xb.translation.x + big_nudge.x, xb.translation.y + big_nudge.y,
                                                       xb.translation.z + big_nudge.z),
                                             xb.rotation);
                if (!gjk_overlap<f32>(a.view(), xa, b.view(), xb_test))
                {
                    break;
                }
                step *= 2.0F;
            }
            const f32 true_depth_min = step / 2.0F;
            const f32 rel_err = (true_depth_min - contact->depth) / (contact->depth + 1e-6F);
            if (rel_err > worst_relative_error)
            {
                worst_relative_error = rel_err;
            }
            if (failed <= 3)
            {
                std::printf("[probe] trial %d FAILED: epa_depth=%.4f, normal=(%.3f,%.3f,%.3f), "
                            "true_depth_lower_bound=%.4f, rel_err=%.3f\n",
                            trial, (double)contact->depth, (double)contact->normal.x, (double)contact->normal.y,
                            (double)contact->normal.z, (double)true_depth_min, (double)rel_err);
            }
        }
        else
        {
            ++passed;
        }
    }
    std::printf("\n=== PROBE: EPA self-consistency on rotated hull-hull ===\n");
    std::printf("Total overlapping: %d, passed: %d, failed: %d, worst rel error: %.3f\n", total_overlapping, passed,
                failed, (double)worst_relative_error);
    std::printf("=== end probe ===\n");
    (void)worst_overshoot;

    REQUIRE(total_overlapping > 0);
    // The test PASSES iff failed == 0. If failed > 0, EPA is broken on
    // rotated hull-hull → substrate-wide pathology.
    // (Don't REQUIRE-pass yet — we want to SEE the failure count.)
}
