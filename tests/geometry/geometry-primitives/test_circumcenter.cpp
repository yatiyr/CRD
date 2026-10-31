// crd-geometry-primitives: circumcentres of triangles and tetrahedra (circumcenter.hpp).
//
// OWN.1 moved their Cramer's-rule solves onto crd-math (try_solve), translated to the first vertex in 2D as in 3D. The
// removed copies are written out here. The 3D centre keeps the copy's bits: crd-math's 3x3 solve expands along the
// first row, as the copy did. The 2D copy squared absolute coordinates; the translated centre agrees with it within a
// declared tolerance, 2^-40 of the input's extent plus the circumradius, on well-shaped random triangles near the
// origin, and away from it keeps digits the copy lost, so there the oracle is the centre's equidistance. The exact
// cases and the degenerate fallback are checked directly.

#include <catch2/catch_test_macros.hpp>

#include <crd/core/seed_generators.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/circumcenter.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <bit>
#include <cmath>

using crd::f32;
using crd::f64;
using crd::math::Vec2;
using crd::math::Vec3;
using crd::geometry::primitives::circumcenter_2d;
using crd::geometry::primitives::circumcenter_3d;

namespace
{

constexpr f64 kTolerance = 0x1.0p-40;

Vec2<f64> removed_circumcenter_2d(const Vec2<f64>& a, const Vec2<f64>& b, const Vec2<f64>& c) noexcept
{
    const f64 d = 2.0 * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
    const f64 a_sq = a.x * a.x + a.y * a.y;
    const f64 b_sq = b.x * b.x + b.y * b.y;
    const f64 c_sq = c.x * c.x + c.y * c.y;
    return Vec2<f64>{(a_sq * (b.y - c.y) + b_sq * (c.y - a.y) + c_sq * (a.y - b.y)) / d,
                     (a_sq * (c.x - b.x) + b_sq * (a.x - c.x) + c_sq * (b.x - a.x)) / d};
}

Vec3<f64> removed_circumcenter_3d(const Vec3<f64>& a, const Vec3<f64>& b, const Vec3<f64>& c,
                                  const Vec3<f64>& d) noexcept
{
    const f64 bx = b.x - a.x;
    const f64 by = b.y - a.y;
    const f64 bz = b.z - a.z;
    const f64 cx = c.x - a.x;
    const f64 cy = c.y - a.y;
    const f64 cz = c.z - a.z;
    const f64 dx = d.x - a.x;
    const f64 dy = d.y - a.y;
    const f64 dz = d.z - a.z;
    const f64 det6 = bx * (cy * dz - cz * dy) - by * (cx * dz - cz * dx) + bz * (cx * dy - cy * dx);
    const f64 rhs_b = (bx * bx + by * by + bz * bz) * 0.5;
    const f64 rhs_c = (cx * cx + cy * cy + cz * cz) * 0.5;
    const f64 rhs_d = (dx * dx + dy * dy + dz * dz) * 0.5;
    const f64 inv_det = 1.0 / det6;
    const f64 ox = (rhs_b * (cy * dz - cz * dy) - by * (rhs_c * dz - cz * rhs_d) + bz * (rhs_c * dy - cy * rhs_d)) *
                   inv_det;
    const f64 oy = (bx * (rhs_c * dz - cz * rhs_d) - rhs_b * (cx * dz - cz * dx) + bz * (cx * rhs_d - rhs_c * dx)) *
                   inv_det;
    const f64 oz = (bx * (cy * rhs_d - rhs_c * dy) - by * (cx * rhs_d - rhs_c * dx) + rhs_b * (cx * dy - cy * dx)) *
                   inv_det;
    return Vec3<f64>{a.x + ox, a.y + oy, a.z + oz};
}

f64 unit_draw(crd::u64& state) noexcept
{
    return static_cast<f64>(crd::splitmix64_next(state) >> 11U) * 0x1.0p-52 - 1.0;
}

Vec2<f64> vec2_draw(crd::u64& state, f64 offset) noexcept
{
    Vec2<f64> v;
    v.x = offset + unit_draw(state);
    v.y = offset + unit_draw(state);
    return v;
}

Vec3<f64> vec3_draw(crd::u64& state, f64 offset) noexcept
{
    Vec3<f64> v;
    v.x = offset + unit_draw(state);
    v.y = offset + unit_draw(state);
    v.z = offset + unit_draw(state);
    return v;
}

f64 max_abs(const Vec2<f64>& v) noexcept
{
    return crd::math::max(std::fabs(v.x), std::fabs(v.y));
}

} // namespace

TEST_CASE("circumcentre: exact cases in both precisions", "[geometry][circumcenter]")
{
    CHECK(circumcenter_2d(Vec2<f64>{0.0, 0.0}, Vec2<f64>{2.0, 0.0}, Vec2<f64>{0.0, 2.0}) == Vec2<f64>{1.0, 1.0});
    CHECK(circumcenter_2d(Vec2<f32>{1.0F, 1.0F}, Vec2<f32>{5.0F, 1.0F}, Vec2<f32>{1.0F, 3.0F}) ==
          Vec2<f32>{3.0F, 2.0F});
    CHECK(circumcenter_3d(Vec3<f64>{0.0, 0.0, 0.0}, Vec3<f64>{2.0, 0.0, 0.0}, Vec3<f64>{0.0, 2.0, 0.0},
                          Vec3<f64>{0.0, 0.0, 2.0}) == Vec3<f64>{1.0, 1.0, 1.0});
    CHECK(circumcenter_3d(Vec3<f32>{-1.0F, -1.0F, -1.0F}, Vec3<f32>{3.0F, -1.0F, -1.0F}, Vec3<f32>{-1.0F, 1.0F, -1.0F},
                          Vec3<f32>{-1.0F, -1.0F, 7.0F}) == Vec3<f32>{1.0F, 0.0F, 3.0F});
}

TEST_CASE("circumcentre: degenerate simplices fall back to the centroid", "[geometry][circumcenter]")
{
    CHECK(circumcenter_2d(Vec2<f64>{0.0, 0.0}, Vec2<f64>{1.0, 1.0}, Vec2<f64>{2.0, 2.0}) == Vec2<f64>{1.0, 1.0});
    CHECK(circumcenter_2d(Vec2<f32>{3.0F, 3.0F}, Vec2<f32>{3.0F, 3.0F}, Vec2<f32>{3.0F, 3.0F}) ==
          Vec2<f32>{3.0F, 3.0F});
    CHECK(circumcenter_3d(Vec3<f64>{0.0, 0.0, 0.0}, Vec3<f64>{4.0, 0.0, 0.0}, Vec3<f64>{0.0, 4.0, 0.0},
                          Vec3<f64>{4.0, 4.0, 0.0}) == Vec3<f64>{2.0, 2.0, 0.0});
}

TEST_CASE("circumcentre: equidistant, the removed 3D copy's bits and the 2D copy within the declared tolerance",
          "[geometry][circumcenter][own]")
{
    crd::u64 state = 0xC1C0'CE17ULL;
    crd::usize shaped = 0;
    for (crd::usize n = 0; n < 4096U; ++n)
    {
        // Near the origin and offset by 1000, where the removed 2D form squared absolute coordinates.
        const bool near_origin = (n & 1U) == 0U;
        const f64 offset = near_origin ? 0.0 : 1000.0;

        const Vec2<f64> a2 = vec2_draw(state, offset);
        const Vec2<f64> b2 = vec2_draw(state, offset);
        const Vec2<f64> c2 = vec2_draw(state, offset);
        const Vec3<f64> a3 = vec3_draw(state, offset);
        const Vec3<f64> b3 = vec3_draw(state, offset);
        const Vec3<f64> c3 = vec3_draw(state, offset);
        const Vec3<f64> d3 = vec3_draw(state, offset);
        // Well-shaped only: twice the area and six times the volume at least 0.1 of the unit extent's.
        const f64 area2 = (b2.x - a2.x) * (c2.y - a2.y) - (b2.y - a2.y) * (c2.x - a2.x);
        const f64 volume6 = crd::math::dot(b3 - a3, crd::math::cross(c3 - a3, d3 - a3));
        if (std::fabs(area2) < 0.1 || std::fabs(volume6) < 0.1)
        {
            continue;
        }
        ++shaped;

        const Vec2<f64> o2 = circumcenter_2d(a2, b2, c2);
        const f64 r2 = crd::math::length(o2 - a2);
        CHECK(std::fabs(crd::math::length(o2 - b2) - r2) <= kTolerance * (1.0 + r2));
        CHECK(std::fabs(crd::math::length(o2 - c2) - r2) <= kTolerance * (1.0 + r2));
        if (near_origin)
        {
            CHECK(max_abs(o2 - removed_circumcenter_2d(a2, b2, c2)) <= kTolerance * (max_abs(a2) + r2));
        }

        const Vec3<f64> o3 = circumcenter_3d(a3, b3, c3, d3);
        const f64 r3 = crd::math::length(o3 - a3);
        CHECK(std::fabs(crd::math::length(o3 - b3) - r3) <= kTolerance * (1.0 + r3));
        CHECK(std::fabs(crd::math::length(o3 - c3) - r3) <= kTolerance * (1.0 + r3));
        CHECK(std::fabs(crd::math::length(o3 - d3) - r3) <= kTolerance * (1.0 + r3));
        const Vec3<f64> removed3 = removed_circumcenter_3d(a3, b3, c3, d3);
        CHECK(std::bit_cast<crd::u64>(o3.x) == std::bit_cast<crd::u64>(removed3.x));
        CHECK(std::bit_cast<crd::u64>(o3.y) == std::bit_cast<crd::u64>(removed3.y));
        CHECK(std::bit_cast<crd::u64>(o3.z) == std::bit_cast<crd::u64>(removed3.z));
    }
    CHECK(shaped > 1000U);
}
