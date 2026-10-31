// Tests for crd-geometry-mesh-processing v7b Quadric<T>.
//
// Covers:
//   - Zero / from_plane construction
//   - Plane-distance invariant (evaluate at a point on the plane == 0)
//   - Addition + accumulation
//   - Scalar multiplication
//   - optimal_position (regular case + singular fallback)
//   - f64 precision tier

#include <catch2/catch_test_macros.hpp>

#include <crd/core/seed_generators.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/quadric.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <bit>
#include <cmath>
#include <optional>

using crd::f32;
using crd::f64;
using crd::math::Vec3;
using crd::geometry::mesh_processing::Quadric;
using crd::geometry::mesh_processing::evaluate;
using crd::geometry::mesh_processing::optimal_position;

TEST_CASE("Quadric: zero quadric evaluates to zero everywhere",
          "[geometry-mesh-processing][quadric]")
{
    const auto q = Quadric<f32>::zero();
    CHECK(evaluate(q, Vec3<f32>{0, 0, 0}) == 0.0F);
    CHECK(evaluate(q, Vec3<f32>{1, 2, 3}) == 0.0F);
    CHECK(evaluate(q, Vec3<f32>{-7, 11, 13}) == 0.0F);
}

TEST_CASE("Quadric: from_plane evaluates to zero at points on the plane",
          "[geometry-mesh-processing][quadric]")
{
    // Plane: x + 2y + 3z - 14 = 0 (passes through (1, 2, 3) since 1+4+9=14).
    // Normalise to unit normal: |n| = sqrt(1+4+9) = sqrt(14).
    const f32 inv = 1.0F / std::sqrt(14.0F);
    const f32 a = 1.0F * inv;
    const f32 b = 2.0F * inv;
    const f32 c = 3.0F * inv;
    const f32 d = -14.0F * inv;
    const auto q = Quadric<f32>::from_plane(a, b, c, d);

    // Points on the plane → evaluate ≈ 0.
    CHECK(crd::math::abs(evaluate(q, Vec3<f32>{1, 2, 3})) < 1e-5F);
    CHECK(crd::math::abs(evaluate(q, Vec3<f32>{0, 1, 4})) < 1e-5F);   // 0+2+12=14 ✓
    CHECK(crd::math::abs(evaluate(q, Vec3<f32>{4, 2, 2})) < 1e-5F);   // 4+4+6=14 ✓

    // Point off the plane → evaluate = signed_distance². For (0,0,0):
    // signed_dist = (0+0+0-14) / sqrt(14) = -14/sqrt(14) = -sqrt(14).
    // distance² = 14.
    const f32 dist_sq = evaluate(q, Vec3<f32>{0, 0, 0});
    CHECK(crd::math::abs(dist_sq - 14.0F) < 1e-4F);
}

TEST_CASE("Quadric: addition + accumulation",
          "[geometry-mesh-processing][quadric]")
{
    const auto q1 = Quadric<f32>::from_plane(1, 0, 0, -3); // plane x = 3
    const auto q2 = Quadric<f32>::from_plane(0, 1, 0, -4); // plane y = 4
    const auto q3 = Quadric<f32>::from_plane(0, 0, 1, -5); // plane z = 5

    const auto sum = q1 + q2 + q3;

    // At (3, 4, 5): all three planes contain the point → cost 0.
    CHECK(crd::math::abs(evaluate(sum, Vec3<f32>{3, 4, 5})) < 1e-5F);

    // At (0, 0, 0): dist² to x=3 = 9, to y=4 = 16, to z=5 = 25 → sum = 50.
    CHECK(crd::math::abs(evaluate(sum, Vec3<f32>{0, 0, 0}) - 50.0F) < 1e-4F);

    // += matches +.
    auto acc = Quadric<f32>::zero();
    acc += q1;
    acc += q2;
    acc += q3;
    for (int i = 0; i < 10; ++i)
    {
        CHECK(acc.data[i] == sum.data[i]);
    }
}

TEST_CASE("Quadric: scalar multiplication scales evaluate proportionally",
          "[geometry-mesh-processing][quadric]")
{
    const auto q = Quadric<f32>::from_plane(1, 0, 0, -3);
    const auto q2 = q * 2.5F;
    const f32 e_base   = evaluate(q,  Vec3<f32>{0, 0, 0});
    const f32 e_scaled = evaluate(q2, Vec3<f32>{0, 0, 0});
    CHECK(crd::math::abs(e_scaled - 2.5F * e_base) < 1e-4F);
}

TEST_CASE("Quadric: optimal_position recovers a planar intersection",
          "[geometry-mesh-processing][quadric]")
{
    // Three orthogonal planes meeting at (3, 4, 5).
    auto q = Quadric<f32>::zero();
    q += Quadric<f32>::from_plane(1, 0, 0, -3);
    q += Quadric<f32>::from_plane(0, 1, 0, -4);
    q += Quadric<f32>::from_plane(0, 0, 1, -5);

    const auto v = optimal_position(q);
    REQUIRE(v.has_value());
    const auto vec = *v;
    CHECK(crd::math::abs(vec.x - 3.0F) < 1e-4F);
    CHECK(crd::math::abs(vec.y - 4.0F) < 1e-4F);
    CHECK(crd::math::abs(vec.z - 5.0F) < 1e-4F);
}

TEST_CASE("Quadric: optimal_position returns nullopt for singular system",
          "[geometry-mesh-processing][quadric]")
{
    // Three parallel planes (all normal +x): system is rank-1; det = 0.
    auto q = Quadric<f32>::zero();
    q += Quadric<f32>::from_plane(1, 0, 0, -1);
    q += Quadric<f32>::from_plane(1, 0, 0, -2);
    q += Quadric<f32>::from_plane(1, 0, 0, -3);

    const auto v = optimal_position(q);
    CHECK_FALSE(v.has_value());
}

TEST_CASE("Quadric: f64 precision tier evaluates plane-distance correctly",
          "[geometry-mesh-processing][quadric][f64]")
{
    const f64 inv = 1.0 / std::sqrt(14.0);
    const auto q = Quadric<f64>::from_plane(1.0 * inv, 2.0 * inv, 3.0 * inv, -14.0 * inv);
    // Point on plane → 0.
    CHECK(crd::math::abs(evaluate(q, Vec3<f64>{1, 2, 3})) < 1e-12);
    // Origin → 14.
    CHECK(crd::math::abs(evaluate(q, Vec3<f64>{0, 0, 0}) - 14.0) < 1e-10);
}

// ---------------------------------------------------------------------------
// OWN.1: optimal_position's symmetric cofactor inverse moved onto crd-math (try_inverse and the matrix-vector
// product). The copy is written out here as it was; crd-math gives its bits, and its refusals, on every quadric of a
// sweep in both precisions: sums of planes (the decimator's quadrics, mostly regular) and raw symmetric blocks.
// ---------------------------------------------------------------------------

namespace
{

template <typename T>
[[nodiscard]] std::optional<Vec3<T>> removed_optimal_position(const Quadric<T>& q, T det_epsilon) noexcept
{
    const T a = q.data[0];
    const T b = q.data[1];
    const T c = q.data[2];
    const T d = q.data[4];
    const T e = q.data[5];
    const T f = q.data[7];
    const T det = a * (d * f - e * e) - b * (b * f - c * e) + c * (b * e - c * d);
    if (crd::math::abs(det) < det_epsilon)
    {
        return std::nullopt;
    }
    const T rx = -q.data[3];
    const T ry = -q.data[6];
    const T rz = -q.data[8];
    const T inv_det = T{1} / det;
    const T i00 = (d * f - e * e) * inv_det;
    const T i01 = -(b * f - c * e) * inv_det;
    const T i02 = (b * e - c * d) * inv_det;
    const T i11 = (a * f - c * c) * inv_det;
    const T i12 = -(a * e - b * c) * inv_det;
    const T i22 = (a * d - b * b) * inv_det;
    return Vec3<T>{i00 * rx + i01 * ry + i02 * rz, i01 * rx + i11 * ry + i12 * rz, i02 * rx + i12 * ry + i22 * rz};
}

template <typename T> [[nodiscard]] bool same_bits(T x, T y) noexcept
{
    if constexpr (sizeof(T) == sizeof(crd::u32))
    {
        return std::bit_cast<crd::u32>(x) == std::bit_cast<crd::u32>(y);
    }
    else
    {
        return std::bit_cast<crd::u64>(x) == std::bit_cast<crd::u64>(y);
    }
}

template <typename T> [[nodiscard]] T unit_draw(crd::u64& state) noexcept
{
    return static_cast<T>(static_cast<double>(crd::splitmix64_next(state) >> 11U) * 0x1.0p-52 - 1.0);
}

// Returns how many of the quadrics were regular, so the caller can see both paths ran.
template <typename T> crd::usize check_moved_optimal_position(T det_epsilon)
{
    crd::u64 state = 0x0E57'A7E0ULL;
    crd::usize regular = 0;
    for (crd::usize n = 0; n < 4096U; ++n)
    {
        Quadric<T> q = Quadric<T>::zero();
        if ((n & 1U) == 0U)
        {
            const crd::usize planes = 1U + (n / 2U) % 4U; // one to four planes: singular to regular
            for (crd::usize k = 0; k < planes; ++k)
            {
                const T a = unit_draw<T>(state);
                const T b = unit_draw<T>(state);
                const T c = unit_draw<T>(state);
                const T d = unit_draw<T>(state);
                q += Quadric<T>::from_plane(a, b, c, d);
            }
        }
        else
        {
            for (T& x : q.data)
            {
                x = unit_draw<T>(state);
            }
        }
        const auto expected = removed_optimal_position(q, det_epsilon);
        const auto got = optimal_position(q, det_epsilon);
        REQUIRE(got.has_value() == expected.has_value());
        if (expected.has_value())
        {
            ++regular;
            REQUIRE(same_bits(got->x, expected->x));
            REQUIRE(same_bits(got->y, expected->y));
            REQUIRE(same_bits(got->z, expected->z));
        }
    }
    return regular;
}

} // namespace

TEST_CASE("Quadric: optimal_position on crd-math keeps the removed copy's bits and refusals",
          "[geometry-mesh-processing][quadric][own]")
{
    const crd::usize regular32 = check_moved_optimal_position<f32>(static_cast<f32>(1e-10));
    const crd::usize regular64 = check_moved_optimal_position<f64>(1e-10);
    CHECK(regular32 > 1000U);
    CHECK(regular32 < 4096U);
    CHECK(regular64 > 1000U);
    CHECK(regular64 < 4096U);

    // A determinant equal to the bound passes on both sides (diagonal 1, 1, 2^-20: exact in f32 and f64).
    Quadric<f64> edge = Quadric<f64>::zero();
    edge.data[0] = 1.0;
    edge.data[4] = 1.0;
    edge.data[7] = 0x1.0p-20;
    edge.data[3] = -2.0;
    REQUIRE(optimal_position(edge, 0x1.0p-20).has_value());
    CHECK(removed_optimal_position(edge, 0x1.0p-20).has_value());
    CHECK_FALSE(optimal_position(edge, 0x1.0p-19).has_value());
    CHECK(optimal_position(edge, 0x1.0p-20)->x == 2.0);
}
