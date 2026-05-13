// crd-geometry-primitives v1h -- the geometry epsilon/tolerance policy. The
// values must equal the magic numbers the substrate already used (v1h is a
// rename, not a retune), and the two retrofitted call sites (the BVH SAH-cost
// epsilon, the robust ray-AABB ULP pad) must still compute the old values.

#include <crd/geometry/primitives/constants.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace crd;
using namespace crd::geometry::primitives;

TEST_CASE("geometry constants: distance/area/parallel/extent epsilons hold the legacy magnitudes",
          "[geometry][constants]")
{
    CHECK(k_distance_epsilon<f32>() == 1.0e-6F);
    CHECK(k_distance_epsilon<f64>() == 1.0e-12);
    CHECK(k_area_epsilon<f32>() == 1.0e-6F);
    CHECK(k_area_epsilon<f64>() == 1.0e-12);
    CHECK(k_parallel_epsilon<f32>() == 1.0e-6F);
    CHECK(k_parallel_epsilon<f64>() == 1.0e-12);
    CHECK(k_degenerate_extent_epsilon<f32>() == 1.0e-6F);
    CHECK(k_degenerate_extent_epsilon<f64>() == 1.0e-12);
}

TEST_CASE("geometry constants: SAH-cost epsilon + fat margin + robust-pad ULP count", "[geometry][constants]")
{
    CHECK(k_sah_cost_epsilon<f32>() == 1.0e-6F);
    CHECK(k_sah_cost_epsilon<f64>() == 1.0e-12);
    CHECK(k_default_fat_margin<f32>() == 0.1F);
    CHECK(k_default_fat_margin<f64>() == 0.1);
    CHECK(k_robust_aabb_pad_ulps<f32>() == 3U);
    CHECK(k_robust_aabb_pad_ulps<f64>() == 3U);
}

TEST_CASE("geometry constants: robust ray-AABB pad equals the Ize-2013 1+2*gamma3 it always was",
          "[geometry][constants]")
{
    // The value the v0f form computed before v1h wired the ULP count in.
    const f32 u = std::numeric_limits<f32>::epsilon() / 2.0F;
    const f32 gamma3 = (3.0F * u) / (1.0F - 3.0F * u);
    const f32 expected = 1.0F + 2.0F * gamma3;
    CHECK(ray_aabb_robust_pad<f32>() == expected);
    CHECK(ray_aabb_robust_pad<f32>() > 1.0F);
}
