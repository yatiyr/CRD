#include <crd/math/math.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <format>
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

template <typename T> void require_mat2_close(const Mat2<T>& actual, const Mat2<T>& expected, T epsilon)
{
    require_vec2_close(actual.c0, expected.c0, epsilon);
    require_vec2_close(actual.c1, expected.c1, epsilon);
}

template <typename T> void require_mat3_close(const Mat3<T>& actual, const Mat3<T>& expected, T epsilon)
{
    require_vec3_close(actual.c0, expected.c0, epsilon);
    require_vec3_close(actual.c1, expected.c1, epsilon);
    require_vec3_close(actual.c2, expected.c2, epsilon);
}

template <typename T> void require_mat4_close(const Mat4<T>& actual, const Mat4<T>& expected, T epsilon)
{
    require_vec4_close(actual.c0, expected.c0, epsilon);
    require_vec4_close(actual.c1, expected.c1, epsilon);
    require_vec4_close(actual.c2, expected.c2, epsilon);
    require_vec4_close(actual.c3, expected.c3, epsilon);
}

template <typename T> void require_quat_close(const Quat<T>& actual, const Quat<T>& expected, T epsilon)
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

TEST_CASE("Mat identity and zero constructors follow column-major contract", "[math][mat]")
{
    const Mat3f identity = Mat3f::identity();
    const Mat3f zero = Mat3f::zero();

    require_mat3_close(identity, Mat3f(Vec3f(1.0f, 0.0f, 0.0f), Vec3f(0.0f, 1.0f, 0.0f), Vec3f(0.0f, 0.0f, 1.0f)),
                       1.0e-6f);
    require_mat3_close(zero, Mat3f{}, 1.0e-6f);
}

TEST_CASE("Mat2 and Mat3 transpose swap rows and columns", "[math][mat]")
{
    const Mat2d m2(Vec2d(1.0, 2.0), Vec2d(3.0, 4.0));
    require_mat2_close(transpose(m2), Mat2d(Vec2d(1.0, 3.0), Vec2d(2.0, 4.0)), 1.0e-12);

    const Mat3d m3(Vec3d(1.0, 2.0, 3.0), Vec3d(4.0, 5.0, 6.0), Vec3d(7.0, 8.0, 9.0));
    require_mat3_close(transpose(m3), Mat3d(Vec3d(1.0, 4.0, 7.0), Vec3d(2.0, 5.0, 8.0), Vec3d(3.0, 6.0, 9.0)), 1.0e-12);
}

TEST_CASE("Mat3 multiplied by Vec3 uses column-vector semantics", "[math][mat]")
{
    const Mat3f basis(Vec3f(1.0f, 0.0f, 0.0f), Vec3f(0.0f, 2.0f, 0.0f), Vec3f(0.0f, 0.0f, 3.0f));
    const Vec3f v(5.0f, 7.0f, 11.0f);

    require_vec3_close(basis * v, Vec3f(5.0f, 14.0f, 33.0f), 1.0e-6f);
}

TEST_CASE("Mat4 composition matches lhs times rhs columns", "[math][mat]")
{
    const Mat4d lhs(Vec4d(1.0, 2.0, 3.0, 4.0), Vec4d(0.0, 1.0, 0.0, 0.0), Vec4d(2.0, 0.0, 1.0, 0.0),
                    Vec4d(0.0, 0.0, 0.0, 1.0));
    const Mat4d rhs(Vec4d(1.0, 0.0, 0.0, 0.0), Vec4d(0.0, 1.0, 0.0, 0.0), Vec4d(0.0, 0.0, 1.0, 0.0),
                    Vec4d(5.0, 6.0, 7.0, 1.0));

    const Mat4d product = lhs * rhs;
    require_mat4_close(product, Mat4d(lhs * rhs.c0, lhs * rhs.c1, lhs * rhs.c2, lhs * rhs.c3), 1.0e-12);
}

TEST_CASE("Mat4 identity is neutral for matrix and vector multiplication", "[math][mat]")
{
    const Mat4f identity = Mat4f::identity();
    const Mat4f m(Vec4f(1.0f, 2.0f, 3.0f, 4.0f), Vec4f(5.0f, 6.0f, 7.0f, 8.0f), Vec4f(9.0f, 10.0f, 11.0f, 12.0f),
                  Vec4f(13.0f, 14.0f, 15.0f, 16.0f));
    const Vec4f v(1.0f, 2.0f, 3.0f, 1.0f);

    require_mat4_close(identity * m, m, 1.0e-6f);
    require_mat4_close(m * identity, m, 1.0e-6f);
    require_vec4_close(identity * v, v, 1.0e-6f);
}

TEST_CASE("Quat identity and normalization behave as expected", "[math][quat]")
{
    require_quat_close(Quatf::identity(), Quatf(0.0f, 0.0f, 0.0f, 1.0f), 1.0e-6f);

    Quatd q(0.0, 0.0, 0.0, 2.0);
    REQUIRE(try_normalize(q));
    require_quat_close(q, Quatd(0.0, 0.0, 0.0, 1.0), 1.0e-12);
}

TEST_CASE("Quat axis-angle rotates vectors with Hamilton xyzw semantics", "[math][quat]")
{
    const Quatf quarter_turn = from_axis_angle(Vec3f(0.0f, 0.0f, 1.0f), k_half_pi_f);
    require_vec3_close(rotate_vector(quarter_turn, Vec3f(1.0f, 0.0f, 0.0f)), Vec3f(0.0f, 1.0f, 0.0f), 1.0e-4f);
}

TEST_CASE("Quat multiplication composes rotations in Mat * Vec order", "[math][quat]")
{
    const Quatd rz = from_axis_angle(Vec3d(0.0, 0.0, 1.0), k_half_pi_d);
    const Quatd ry = from_axis_angle(Vec3d(0.0, 1.0, 0.0), k_half_pi_d);
    const Vec3d v(1.0, 0.0, 0.0);

    const Vec3d sequential = rotate_vector(ry, rotate_vector(rz, v));
    const Vec3d combined = rotate_vector(ry * rz, v);
    require_vec3_close(combined, sequential, 1.0e-12);
}

TEST_CASE("Quat matrix conversion round-trips orientation", "[math][quat][mat]")
{
    const Quatd q = from_axis_angle(normalized(Vec3d(1.0, 2.0, 3.0)), deg_to_rad(47.0));
    const Quatd round_trip = from_mat3(to_mat3(q));

    const Vec3d probe(0.25, -0.5, 0.75);
    require_vec3_close(rotate_vector(q, probe), rotate_vector(round_trip, probe), 1.0e-12);
}

TEST_CASE("Transform point and vector semantics differ by translation", "[math][transform]")
{
    const Transformf t(Vec3f(10.0f, 0.0f, 0.0f), from_axis_angle(Vec3f(0.0f, 0.0f, 1.0f), k_half_pi_f));
    require_vec3_close(transform_vector(t, Vec3f(1.0f, 0.0f, 0.0f)), Vec3f(0.0f, 1.0f, 0.0f), 1.0e-4f);
    require_vec3_close(transform_point(t, Vec3f(1.0f, 0.0f, 0.0f)), Vec3f(10.0f, 1.0f, 0.0f), 1.0e-4f);
}

TEST_CASE("Transform inverse and composition round-trip", "[math][transform]")
{
    const Transformd a(Vec3d(1.0, 2.0, 3.0), from_axis_angle(Vec3d(0.0, 0.0, 1.0), k_half_pi_d));
    const Transformd b(Vec3d(-2.0, 1.0, 0.5), from_axis_angle(Vec3d(0.0, 1.0, 0.0), deg_to_rad(30.0)));
    const Transformd ab = a * b;
    const Vec3d p(0.25, -0.5, 0.75);

    require_vec3_close(transform_point(ab, p), transform_point(a, transform_point(b, p)), 1.0e-12);
    require_vec3_close(transform_point(inversed(ab), transform_point(ab, p)), p, 1.0e-12);
}

TEST_CASE("Math types format cleanly for logs and diagnostics", "[math][format]")
{
    const Vec3f v(1.0f, 2.0f, 3.0f);
    const Mat2f m(Vec2f(1.0f, 2.0f), Vec2f(3.0f, 4.0f));
    const Quatf q(0.0f, 0.0f, 0.70710677f, 0.70710677f);
    const Transformf t(Vec3f(10.0f, 20.0f, 30.0f), q);

    REQUIRE(std::format("{}", v) == "Vec3(1, 2, 3)");
    REQUIRE(std::format("{}", m) == "Mat2([[1, 3], [2, 4]])");
    REQUIRE(std::format("{}", q) == "Quat(0, 0, 0.70710677, 0.70710677)");
    REQUIRE(std::format("{}", t) == "Transform(t=Vec3(10, 20, 30), r=Quat(0, 0, 0.70710677, 0.70710677))");
}
