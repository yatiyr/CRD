#include <crd/math/math.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace crd;
using namespace crd::math;

namespace
{
template <typename T> void require_vec2_close(const Vec2<T>& actual, const Vec2<T>& expected, T epsilon)
{
    REQUIRE(approx_equal_abs(actual.x, expected.x, epsilon));
    REQUIRE(approx_equal_abs(actual.y, expected.y, epsilon));
}

template <typename T> void require_vec3_close(const Vec3<T>& actual, const Vec3<T>& expected, T epsilon)
{
    REQUIRE(approx_equal_abs(actual.x, expected.x, epsilon));
    REQUIRE(approx_equal_abs(actual.y, expected.y, epsilon));
    REQUIRE(approx_equal_abs(actual.z, expected.z, epsilon));
}

template <typename T> void require_vec4_close(const Vec4<T>& actual, const Vec4<T>& expected, T epsilon)
{
    REQUIRE(approx_equal_abs(actual.x, expected.x, epsilon));
    REQUIRE(approx_equal_abs(actual.y, expected.y, epsilon));
    REQUIRE(approx_equal_abs(actual.z, expected.z, epsilon));
    REQUIRE(approx_equal_abs(actual.w, expected.w, epsilon));
}
} // namespace

TEST_CASE("math scalar constants and angle helpers", "[math][scalar]")
{
    REQUIRE(approx_equal_rel(k_pi<f32>, 3.1415927f));
    REQUIRE(approx_equal_rel(k_tau<f64>, 6.283185307179586));
    REQUIRE(deg_to_rad(180.0f) == Catch::Approx(k_pi<f32>));
    REQUIRE(rad_to_deg(k_half_pi<f64>) == Catch::Approx(90.0));
}

TEST_CASE("math scalar comparison helpers distinguish abs and rel", "[math][scalar]")
{
    REQUIRE(approx_equal_abs(1.0f, 1.0f + 1.0e-6f));
    REQUIRE_FALSE(approx_equal_abs(1.0f, 1.1f));
    REQUIRE(approx_equal_rel(1000000.0, 1000000.000001, 2.0e-12));
    REQUIRE(is_finite(42.0));
    REQUIRE(is_nan(std::numeric_limits<f64>::quiet_NaN()));
}

TEST_CASE("Vec2 basic arithmetic works for float", "[math][vec2]")
{
    const Vec2f a(1.0f, 2.0f);
    const Vec2f b(3.0f, -4.0f);

    require_vec2_close(a + b, Vec2f(4.0f, -2.0f), 1.0e-6f);
    require_vec2_close(a - b, Vec2f(-2.0f, 6.0f), 1.0e-6f);
    require_vec2_close(2.0f * a, Vec2f(2.0f, 4.0f), 1.0e-6f);
    REQUIRE(dot(a, b) == Catch::Approx(-5.0f));
    REQUIRE(cross(a, b) == Catch::Approx(-10.0f));
}

TEST_CASE("Vec3 cross product is right-handed", "[math][vec3]")
{
    const Vec3d x_axis(1.0, 0.0, 0.0);
    const Vec3d y_axis(0.0, 1.0, 0.0);
    const Vec3d z_axis = cross(x_axis, y_axis);

    require_vec3_close(z_axis, Vec3d(0.0, 0.0, 1.0), 1.0e-12);
    REQUIRE(dot(z_axis, x_axis) == Catch::Approx(0.0));
    REQUIRE(dot(z_axis, y_axis) == Catch::Approx(0.0));
}

TEST_CASE("Vec4 indexing and scalar division work", "[math][vec4]")
{
    Vec4f v(2.0f, 4.0f, 6.0f, 8.0f);
    v[2] = 10.0f;
    require_vec4_close(v / 2.0f, Vec4f(1.0f, 2.0f, 5.0f, 4.0f), 1.0e-6f);
}

TEST_CASE("Vec length and normalization work for float and double", "[math][normalize]")
{
    Vec3f vf(3.0f, 4.0f, 0.0f);
    REQUIRE(length(vf) == Catch::Approx(5.0f));
    require_vec3_close(normalized(vf), Vec3f(0.6f, 0.8f, 0.0f), 1.0e-4f);

    Vec3d vd(0.0, 0.0, 2.0);
    REQUIRE(length(vd) == Catch::Approx(2.0));
    require_vec3_close(normalized(vd), Vec3d(0.0, 0.0, 1.0), 1.0e-12);
}

TEST_CASE("try_normalize rejects near-zero vectors", "[math][normalize]")
{
    Vec2d v(0.0, 0.0);
    REQUIRE_FALSE(try_normalize(v));
    require_vec2_close(v, Vec2d(0.0, 0.0), 1.0e-12);
}

TEST_CASE("distance and lerp helpers behave as expected", "[math][vec]")
{
    const Vec3f a(0.0f, 0.0f, 0.0f);
    const Vec3f b(10.0f, 20.0f, 30.0f);

    REQUIRE(distance(a, b) == Catch::Approx(std::sqrt(1400.0f)));
    require_vec3_close(lerp(a, b, 0.25f), Vec3f(2.5f, 5.0f, 7.5f), 1.0e-6f);
}

TEST_CASE("hadamard and constructors preserve component ordering", "[math][vec]")
{
    const Vec2d xy(2.0, 3.0);
    const Vec3d xyz(xy, 4.0);
    const Vec4d xyzw(xyz, 5.0);

    require_vec3_close(hadamard(xyz, Vec3d(10.0, 20.0, 30.0)), Vec3d(20.0, 60.0, 120.0), 1.0e-12);
    require_vec4_close(xyzw, Vec4d(2.0, 3.0, 4.0, 5.0), 1.0e-12);
}
