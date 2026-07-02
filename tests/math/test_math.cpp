#include <crd/math/math.hpp>

#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <format>
#include <limits>
#include <type_traits>

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
    REQUIRE(approx_equal_rel(k_pi<f32>, 3.1415927F));
    REQUIRE(approx_equal_rel(k_tau<f64>, 6.283185307179586));
    REQUIRE(deg_to_rad(180.0F) == Catch::Approx(k_pi<f32>));
    REQUIRE(rad_to_deg(k_half_pi<f64>) == Catch::Approx(90.0));
}

TEST_CASE("math scalar comparison helpers distinguish abs and rel", "[math][scalar]")
{
    REQUIRE(approx_equal_abs(1.0F, 1.0F + 1.0e-6F));
    REQUIRE_FALSE(approx_equal_abs(1.0F, 1.1F));
    REQUIRE(approx_equal_rel(1000000.0, 1000000.000001, 2.0e-12));
    REQUIRE(is_finite(42.0));
    REQUIRE(is_nan(std::numeric_limits<f64>::quiet_NaN()));
}

TEST_CASE("math scalar utility helpers cover clamp epsilon and comparisons", "[math][scalar]")
{
    REQUIRE(default_epsilon<f32>() == Catch::Approx(1.0e-5F));
    REQUIRE(default_epsilon<f64>() == Catch::Approx(1.0e-12));
    REQUIRE(abs(-3.5F) == Catch::Approx(3.5F));
    REQUIRE(min(3.0, 4.0) == Catch::Approx(3.0));
    REQUIRE(max(3.0, 4.0) == Catch::Approx(4.0));
    REQUIRE(clamp(5.0, 1.0, 4.0) == Catch::Approx(4.0));
    REQUIRE(clamp(-1.0F, 0.0F, 10.0F) == Catch::Approx(0.0F));
    REQUIRE(approx_zero(1.0e-6F));
    REQUIRE_FALSE(approx_zero(1.0e-2F));
}

TEST_CASE("math types keep standard-layout trivially-copyable storage", "[math][layout]")
{
    STATIC_REQUIRE(std::is_standard_layout_v<Vec3f>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Vec3f>);
    STATIC_REQUIRE(std::is_standard_layout_v<Mat4f>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Mat4f>);
    STATIC_REQUIRE(std::is_standard_layout_v<Quatf>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Quatf>);
    STATIC_REQUIRE(std::is_standard_layout_v<Transformf>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Transformf>);
    STATIC_REQUIRE(sizeof(Vec4f) == sizeof(f32) * 4);
    STATIC_REQUIRE(sizeof(Mat4f) == sizeof(Vec4f) * 4);
}

TEST_CASE("Vec2 basic arithmetic works for float", "[math][vec2]")
{
    const Vec2f a(1.0F, 2.0F);
    const Vec2f b(3.0F, -4.0F);

    require_vec2_close(a + b, Vec2f(4.0F, -2.0F), 1.0e-6F);
    require_vec2_close(a - b, Vec2f(-2.0F, 6.0F), 1.0e-6F);
    require_vec2_close(2.0F * a, Vec2f(2.0F, 4.0F), 1.0e-6F);
    REQUIRE(dot(a, b) == Catch::Approx(-5.0F));
    REQUIRE(cross(a, b) == Catch::Approx(-10.0F));
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
    Vec4f v(2.0F, 4.0F, 6.0F, 8.0F);
    v[2] = 10.0F;
    require_vec4_close(v / 2.0F, Vec4f(1.0F, 2.0F, 5.0F, 4.0F), 1.0e-6F);
}

TEST_CASE("Vec length and normalization work for float and double", "[math][normalize]")
{
    Vec3f vf(3.0F, 4.0F, 0.0F);
    REQUIRE(length(vf) == Catch::Approx(5.0F));
    require_vec3_close(normalized(vf), Vec3f(0.6F, 0.8F, 0.0F), 1.0e-4F);

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
    const Vec3f a(0.0F, 0.0F, 0.0F);
    const Vec3f b(10.0F, 20.0F, 30.0F);

    REQUIRE(distance(a, b) == Catch::Approx(std::sqrt(1400.0F)));
    REQUIRE(distance_squared(a, b) == Catch::Approx(1400.0F));
    require_vec3_close(lerp(a, b, 0.25F), Vec3f(2.5F, 5.0F, 7.5F), 1.0e-6F);
}

TEST_CASE("Vec2 and Vec4 helper families behave consistently", "[math][vec]")
{
    Vec2f a(1.0F, -2.0F);
    Vec2f b(3.0F, 5.0F);
    REQUIRE(a == Vec2f(1.0F, -2.0F));
    require_vec2_close(-a, Vec2f(-1.0F, 2.0F), 1.0e-6F);
    require_vec2_close(hadamard(a, b), Vec2f(3.0F, -10.0F), 1.0e-6F);
    REQUIRE(distance_squared(a, b) == Catch::Approx(53.0F));
    require_vec2_close(lerp(a, b, 0.5F), Vec2f(2.0F, 1.5F), 1.0e-6F);
    REQUIRE(try_normalize(a));

    Vec4d c(1.0, 2.0, 3.0, 4.0);
    Vec4d d(0.5, -1.0, 2.0, 8.0);
    require_vec4_close(hadamard(c, d), Vec4d(0.5, -2.0, 6.0, 32.0), 1.0e-12);
    REQUIRE(distance_squared(c, d) == Catch::Approx(26.25));
    require_vec4_close(lerp(c, d, 0.25), Vec4d(0.875, 1.25, 2.75, 5.0), 1.0e-12);
    REQUIRE(try_normalize(c));
    REQUIRE(length_squared(c) == Catch::Approx(1.0));
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
    const Mat2f identity2 = Mat2f::identity();
    const Mat4f identity4 = Mat4f::identity();

    require_mat3_close(identity, Mat3f(Vec3f(1.0F, 0.0F, 0.0F), Vec3f(0.0F, 1.0F, 0.0F), Vec3f(0.0F, 0.0F, 1.0F)),
                       1.0e-6F);
    require_mat3_close(zero, Mat3f{}, 1.0e-6F);
    require_mat2_close(identity2, Mat2f(Vec2f(1.0F, 0.0F), Vec2f(0.0F, 1.0F)), 1.0e-6F);
    require_mat4_close(identity4,
                       Mat4f(Vec4f(1.0F, 0.0F, 0.0F, 0.0F), Vec4f(0.0F, 1.0F, 0.0F, 0.0F),
                             Vec4f(0.0F, 0.0F, 1.0F, 0.0F), Vec4f(0.0F, 0.0F, 0.0F, 1.0F)),
                       1.0e-6F);
}

TEST_CASE("Mat2 and Mat3 transpose swap rows and columns", "[math][mat]")
{
    const Mat2d m2(Vec2d(1.0, 2.0), Vec2d(3.0, 4.0));
    require_mat2_close(transpose(m2), Mat2d(Vec2d(1.0, 3.0), Vec2d(2.0, 4.0)), 1.0e-12);

    const Mat3d m3(Vec3d(1.0, 2.0, 3.0), Vec3d(4.0, 5.0, 6.0), Vec3d(7.0, 8.0, 9.0));
    require_mat3_close(transpose(m3), Mat3d(Vec3d(1.0, 4.0, 7.0), Vec3d(2.0, 5.0, 8.0), Vec3d(3.0, 6.0, 9.0)), 1.0e-12);

    const Mat4d m4(Vec4d(1.0, 2.0, 3.0, 4.0), Vec4d(5.0, 6.0, 7.0, 8.0), Vec4d(9.0, 10.0, 11.0, 12.0),
                   Vec4d(13.0, 14.0, 15.0, 16.0));
    require_mat4_close(transpose(m4),
                       Mat4d(Vec4d(1.0, 5.0, 9.0, 13.0), Vec4d(2.0, 6.0, 10.0, 14.0), Vec4d(3.0, 7.0, 11.0, 15.0),
                             Vec4d(4.0, 8.0, 12.0, 16.0)),
                       1.0e-12);
}

TEST_CASE("Mat2 and Mat3 matrix multiplication and indexing work", "[math][mat]")
{
    const Mat2f a(Vec2f(1.0F, 2.0F), Vec2f(3.0F, 4.0F));
    const Mat2f b(Vec2f(2.0F, 0.0F), Vec2f(1.0F, 2.0F));
    require_mat2_close(a * b, Mat2f(a * b.c0, a * b.c1), 1.0e-6F);
    REQUIRE(a[0].x == Catch::Approx(1.0F));
    REQUIRE(a[1].y == Catch::Approx(4.0F));

    const Mat3d c(Vec3d(1.0, 0.0, 2.0), Vec3d(0.0, 1.0, 0.0), Vec3d(3.0, 0.0, 1.0));
    const Mat3d d(Vec3d(2.0, 1.0, 0.0), Vec3d(0.0, 2.0, 1.0), Vec3d(1.0, 0.0, 2.0));
    require_mat3_close(c * d, Mat3d(c * d.c0, c * d.c1, c * d.c2), 1.0e-12);
}

TEST_CASE("Mat3 multiplied by Vec3 uses column-vector semantics", "[math][mat]")
{
    const Mat3f basis(Vec3f(1.0F, 0.0F, 0.0F), Vec3f(0.0F, 2.0F, 0.0F), Vec3f(0.0F, 0.0F, 3.0F));
    const Vec3f v(5.0F, 7.0F, 11.0F);

    require_vec3_close(basis * v, Vec3f(5.0F, 14.0F, 33.0F), 1.0e-6F);
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
    const Mat4f m(Vec4f(1.0F, 2.0F, 3.0F, 4.0F), Vec4f(5.0F, 6.0F, 7.0F, 8.0F), Vec4f(9.0F, 10.0F, 11.0F, 12.0F),
                  Vec4f(13.0F, 14.0F, 15.0F, 16.0F));
    const Vec4f v(1.0F, 2.0F, 3.0F, 1.0F);

    require_mat4_close(identity * m, m, 1.0e-6F);
    require_mat4_close(m * identity, m, 1.0e-6F);
    require_vec4_close(identity * v, v, 1.0e-6F);
}

// ---------------------------------------------------------------------------
// v0f � Mat4<f32> SIMD specialization parity vs scalar reference.
//
// The non-template Mat4<f32> operator* overloads in mat_simd_f32.hpp must
// produce **bit-exact** identical results to the scalar template path. Same
// accumulation order (c0*x + c1*y + c2*z + c3*w, left-to-right), same
// mul_add two-rounding semantics. ADR-0063 �1: bit-exact across SIMD and
// scalar backends.
// ---------------------------------------------------------------------------

namespace
{
// Manual scalar reference Mat4f * Vec4f (replicates what the scalar
// template path would compute, lane by lane). Bypasses the SIMD overload.
Vec4f mat4f_vec4f_scalar_reference(const Mat4f& m, const Vec4f& v) noexcept
{
    Vec4f r;
    r.x = m.c0.x * v.x + m.c1.x * v.y + m.c2.x * v.z + m.c3.x * v.w;
    r.y = m.c0.y * v.x + m.c1.y * v.y + m.c2.y * v.z + m.c3.y * v.w;
    r.z = m.c0.z * v.x + m.c1.z * v.y + m.c2.z * v.z + m.c3.z * v.w;
    r.w = m.c0.w * v.x + m.c1.w * v.y + m.c2.w * v.z + m.c3.w * v.w;
    return r;
}

Mat4f mat4f_mat4f_scalar_reference(const Mat4f& a, const Mat4f& b) noexcept
{
    return Mat4f(mat4f_vec4f_scalar_reference(a, b.c0), mat4f_vec4f_scalar_reference(a, b.c1),
                 mat4f_vec4f_scalar_reference(a, b.c2), mat4f_vec4f_scalar_reference(a, b.c3));
}

// Bit-exact equality (memcmp-style; no epsilon).
bool vec4f_bit_eq(const Vec4f& a, const Vec4f& b) noexcept
{
    return std::bit_cast<crd::u32>(a.x) == std::bit_cast<crd::u32>(b.x) &&
           std::bit_cast<crd::u32>(a.y) == std::bit_cast<crd::u32>(b.y) &&
           std::bit_cast<crd::u32>(a.z) == std::bit_cast<crd::u32>(b.z) &&
           std::bit_cast<crd::u32>(a.w) == std::bit_cast<crd::u32>(b.w);
}

bool mat4f_bit_eq(const Mat4f& a, const Mat4f& b) noexcept
{
    return vec4f_bit_eq(a.c0, b.c0) && vec4f_bit_eq(a.c1, b.c1) && vec4f_bit_eq(a.c2, b.c2) && vec4f_bit_eq(a.c3, b.c3);
}
} // namespace

TEST_CASE("v0f Mat4<f32> * Vec4<f32> is bit-exact with scalar reference", "[math][mat][simd][v0f]")
{
    // Pseudo-random but deterministic Mat4 + Vec4 (same input every run, so
    // any FP discrepancy across builds is a real divergence, not RNG noise).
    const Mat4f m(Vec4f(1.5F, -2.25F, 3.125F, -4.0625F), Vec4f(-5.5F, 6.75F, -7.875F, 8.9375F),
                  Vec4f(9.5F, -10.25F, 11.125F, -12.0625F), Vec4f(13.5F, -14.75F, 15.875F, -16.9375F));
    const Vec4f v(0.125F, -0.25F, 0.5F, -1.0F);

    const Vec4f simd_result = m * v;                                // routes through SIMD overload
    const Vec4f scalar_result = mat4f_vec4f_scalar_reference(m, v); // bypasses overload

    REQUIRE(vec4f_bit_eq(simd_result, scalar_result));
}

TEST_CASE("v0f Mat4<f32> * Mat4<f32> is bit-exact with scalar reference", "[math][mat][simd][v0f]")
{
    const Mat4f a(Vec4f(1.5F, -2.25F, 3.125F, -4.0625F), Vec4f(-5.5F, 6.75F, -7.875F, 8.9375F),
                  Vec4f(9.5F, -10.25F, 11.125F, -12.0625F), Vec4f(13.5F, -14.75F, 15.875F, -16.9375F));
    const Mat4f b(Vec4f(0.125F, -0.25F, 0.5F, -1.0F), Vec4f(0.625F, 1.25F, 2.5F, 5.0F),
                  Vec4f(-0.0625F, 0.125F, -0.25F, 0.5F), Vec4f(1.0F, -2.0F, 4.0F, -8.0F));

    const Mat4f simd_result = a * b;
    const Mat4f scalar_result = mat4f_mat4f_scalar_reference(a, b);

    REQUIRE(mat4f_bit_eq(simd_result, scalar_result));
}

TEST_CASE("v0f Mat4<f64> still goes through scalar template (no SIMD f64 overload)", "[math][mat][simd][v0f]")
{
    // Sanity check: the SIMD specialization is f32-only. f64 path is
    // unaffected. Doesn't bit-eq check (no SIMD f64 path to compare to);
    // just verifies the scalar template still handles f64.
    const Mat4d a(Vec4d(1.0, 2.0, 3.0, 4.0), Vec4d(5.0, 6.0, 7.0, 8.0), Vec4d(9.0, 10.0, 11.0, 12.0),
                  Vec4d(13.0, 14.0, 15.0, 16.0));
    const Mat4d identity = Mat4d::identity();
    require_mat4_close(a * identity, a, 1.0e-12);
}

TEST_CASE("Quat identity and normalization behave as expected", "[math][quat]")
{
    require_quat_close(Quatf::identity(), Quatf(0.0F, 0.0F, 0.0F, 1.0F), 1.0e-6F);

    Quatd q(0.0, 0.0, 0.0, 2.0);
    REQUIRE(try_normalize(q));
    require_quat_close(q, Quatd(0.0, 0.0, 0.0, 1.0), 1.0e-12);
    REQUIRE(length_squared(q) == Catch::Approx(1.0));
    REQUIRE(length(q) == Catch::Approx(1.0));
}

TEST_CASE("Quat conjugate inverse and failed inverse paths are pinned", "[math][quat]")
{
    const Quatd q = from_axis_angle(Vec3d(0.0, 0.0, 1.0), deg_to_rad(30.0));
    const Quatd qc = conjugate(q);
    require_quat_close(qc, Quatd(-q.x, -q.y, -q.z, q.w), 1.0e-12);

    Quatd inv{};
    REQUIRE(try_inverse(q, inv));
    require_quat_close(inv * q, Quatd::identity(), 1.0e-12);

    const Quatd zero(0.0, 0.0, 0.0, 0.0);
    REQUIRE_FALSE(try_inverse(zero, inv));
}

TEST_CASE("Quat axis-angle rotates vectors with Hamilton xyzw semantics", "[math][quat]")
{
    const Quatf quarter_turn = from_axis_angle(Vec3f(0.0F, 0.0F, 1.0F), k_half_pi_f);
    require_vec3_close(rotate_vector(quarter_turn, Vec3f(1.0F, 0.0F, 0.0F)), Vec3f(0.0F, 1.0F, 0.0F), 1.0e-4F);
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
    require_mat4_close(to_mat4(q),
                       Mat4d(Vec4d(to_mat3(q).c0, 0.0), Vec4d(to_mat3(q).c1, 0.0), Vec4d(to_mat3(q).c2, 0.0),
                             Vec4d(0.0, 0.0, 0.0, 1.0)),
                       1.0e-12);
}

TEST_CASE("Quat nlerp and slerp preserve unit length and endpoints", "[math][quat]")
{
    const Quatd a = Quatd::identity();
    const Quatd b = from_axis_angle(Vec3d(0.0, 0.0, 1.0), k_pi_d);

    require_quat_close(nlerp(a, b, 0.0), a, 1.0e-12);
    require_quat_close(slerp(a, b, 0.0), a, 1.0e-12);
    require_quat_close(nlerp(a, b, 1.0), b, 1.0e-12);
    require_quat_close(slerp(a, b, 1.0), b, 1.0e-12);
    REQUIRE(length(nlerp(a, b, 0.5)) == Catch::Approx(1.0));
    REQUIRE(length(slerp(a, b, 0.5)) == Catch::Approx(1.0));
}

TEST_CASE("Transform point and vector semantics differ by translation", "[math][transform]")
{
    const Transformf t(Vec3f(10.0F, 0.0F, 0.0F), from_axis_angle(Vec3f(0.0F, 0.0F, 1.0F), k_half_pi_f));
    require_vec3_close(transform_vector(t, Vec3f(1.0F, 0.0F, 0.0F)), Vec3f(0.0F, 1.0F, 0.0F), 1.0e-4F);
    require_vec3_close(transform_point(t, Vec3f(1.0F, 0.0F, 0.0F)), Vec3f(10.0F, 1.0F, 0.0F), 1.0e-4F);
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

TEST_CASE("Transform identity and matrix conversion are pinned", "[math][transform][mat]")
{
    const Transformf identity = Transformf::identity();
    require_vec3_close(transform_point(identity, Vec3f(1.0F, 2.0F, 3.0F)), Vec3f(1.0F, 2.0F, 3.0F), 1.0e-6F);
    require_mat4_close(to_mat4(identity), Mat4f::identity(), 1.0e-6F);
}

TEST_CASE("Math types format cleanly for logs and diagnostics", "[math][format]")
{
    const Vec3f v(1.0F, 2.0F, 3.0F);
    const Mat2f m(Vec2f(1.0F, 2.0F), Vec2f(3.0F, 4.0F));
    const Quatf q(0.0F, 0.0F, 0.70710677F, 0.70710677F);
    const Transformf t(Vec3f(10.0F, 20.0F, 30.0F), q);

    REQUIRE(std::format("{}", v) == "Vec3(1, 2, 3)");
    REQUIRE(std::format("{}", m) == "Mat2([[1, 3], [2, 4]])");
    REQUIRE(std::format("{}", q) == "Quat(0, 0, 0.70710677, 0.70710677)");
    REQUIRE(std::format("{}", t) == "Transform(t=Vec3(10, 20, 30), r=Quat(0, 0, 0.70710677, 0.70710677))");
    // Geometry-primitive formatters (Ray/Plane/Sphere/AABB/Triangle3/Frustum)
    // moved to crd-geometry-primitives � see tests/geometry-primitives/test_primitives.cpp.
}

TEST_CASE("look_at produces a correct right-handed view matrix", "[math][mat]")
{
    // Camera on the +Z axis looking at the origin — should produce a pure -Z translation.
    const Vec3f eye{0.0F, 0.0F, 5.0F};
    const Vec3f target{0.0F, 0.0F, 0.0F};
    const Vec3f up{0.0F, 1.0F, 0.0F};
    const Mat4f view = look_at(eye, target, up);

    require_mat4_close(view,
                       Mat4f(Vec4f(1.0F, 0.0F, 0.0F, 0.0F), Vec4f(0.0F, 1.0F, 0.0F, 0.0F),
                             Vec4f(0.0F, 0.0F, 1.0F, 0.0F), Vec4f(0.0F, 0.0F, -5.0F, 1.0F)),
                       1.0e-5F);

    // View matrix must map the eye to the origin.
    require_vec4_close(view * Vec4f(eye, 1.0F), Vec4f(0.0F, 0.0F, 0.0F, 1.0F), 1.0e-5F);
}

TEST_CASE("perspective_reverse_z maps z_near to NDC 1 and infinity to NDC 0", "[math][mat]")
{
    // 90-degree fov, square aspect: tan(45deg)=1, so scale_x=1, scale_y=-1.
    const Mat4f p = perspective_reverse_z(k_half_pi_f, 1.0F, 1.0F);
    REQUIRE(p.c0.x == Catch::Approx(1.0F));
    REQUIRE(p.c1.y == Catch::Approx(-1.0F));
    REQUIRE(p.c2.w == Catch::Approx(-1.0F));
    REQUIRE(p.c3.z == Catch::Approx(1.0F));

    // A point at view-space z=-z_near should produce clip w=z_near, clip z=z_near (NDC z=1).
    const Vec4f clip = p * Vec4f(0.0F, 0.0F, -1.0F, 1.0F);
    REQUIRE(clip.z == Catch::Approx(1.0F));
    REQUIRE(clip.w == Catch::Approx(1.0F));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Interpolation primitives
//  ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("scalar lerp/mix/saturate/step/inverse_lerp/remap", "[math][interp]")
{
    REQUIRE(lerp(0.0F, 10.0F, 0.0F) == Catch::Approx(0.0F));
    REQUIRE(lerp(0.0F, 10.0F, 1.0F) == Catch::Approx(10.0F));
    REQUIRE(lerp(0.0F, 10.0F, 0.5F) == Catch::Approx(5.0F));
    REQUIRE(lerp(0.0F, 10.0F, 1.5F) == Catch::Approx(15.0F)); // extrapolates
    REQUIRE(mix(2.0F, 6.0F, 0.25F) == Catch::Approx(3.0F));   // alias

    REQUIRE(saturate(-3.0F) == Catch::Approx(0.0F));
    REQUIRE(saturate(0.5F) == Catch::Approx(0.5F));
    REQUIRE(saturate(2.0F) == Catch::Approx(1.0F));

    REQUIRE(step(0.5F, 0.4F) == Catch::Approx(0.0F));
    REQUIRE(step(0.5F, 0.5F) == Catch::Approx(1.0F)); // x == edge → 1
    REQUIRE(step(0.5F, 0.6F) == Catch::Approx(1.0F));

    REQUIRE(inverse_lerp(2.0F, 6.0F, 4.0F) == Catch::Approx(0.5F));
    REQUIRE(remap(5.0F, 0.0F, 10.0F, 100.0F, 200.0F) == Catch::Approx(150.0F));
}

TEST_CASE("smoothstep / smootherstep boundary and midpoint", "[math][interp]")
{
    // At and past the edges, both saturate to 0 / 1.
    REQUIRE(smoothstep(0.0F, 1.0F, -0.5F) == Catch::Approx(0.0F));
    REQUIRE(smoothstep(0.0F, 1.0F, 0.0F) == Catch::Approx(0.0F));
    REQUIRE(smoothstep(0.0F, 1.0F, 1.0F) == Catch::Approx(1.0F));
    REQUIRE(smoothstep(0.0F, 1.0F, 1.5F) == Catch::Approx(1.0F));

    REQUIRE(smootherstep(0.0F, 1.0F, 0.0F) == Catch::Approx(0.0F));
    REQUIRE(smootherstep(0.0F, 1.0F, 1.0F) == Catch::Approx(1.0F));

    // Midpoint of both is exactly 0.5 by construction.
    REQUIRE(smoothstep(0.0F, 1.0F, 0.5F) == Catch::Approx(0.5F));
    REQUIRE(smootherstep(0.0F, 1.0F, 0.5F) == Catch::Approx(0.5F));

    // Both are monotone non-decreasing on [0, 1].
    float prev_smooth = -1.0F;
    float prev_smoother = -1.0F;
    for (int i = 0; i <= 32; ++i)
    {
        const float t = static_cast<float>(i) / 32.0F;
        const float s = smoothstep(0.0F, 1.0F, t);
        const float ss = smootherstep(0.0F, 1.0F, t);
        REQUIRE(s >= prev_smooth);
        REQUIRE(ss >= prev_smoother);
        prev_smooth = s;
        prev_smoother = ss;
    }
}

TEST_CASE("damp converges, frame-rate-stable, identity at dt=0", "[math][interp]")
{
    // dt = 0 → no progress, return current.
    REQUIRE(damp(3.0F, 7.0F, 5.0F, 0.0F) == Catch::Approx(3.0F));

    // Large dt → essentially target.
    REQUIRE(damp(3.0F, 7.0F, 5.0F, 100.0F) == Catch::Approx(7.0F).margin(1e-6F));

    // 60 fixed-step ticks at dt=1/60 must equal one tick at dt=1 for the same lambda
    // — this is the property that makes damp frame-rate independent. Tolerance is
    // generous because float rounding accumulates over 60 iterations.
    constexpr float k_lambda = 4.0F;
    float multi = 0.0F;
    for (int i = 0; i < 60; ++i)
    {
        multi = damp(multi, 1.0F, k_lambda, 1.0F / 60.0F);
    }
    const float single = damp(0.0F, 1.0F, k_lambda, 1.0F);
    REQUIRE(multi == Catch::Approx(single).margin(1e-3F));
}

TEST_CASE("Vec lerp / damp componentwise", "[math][interp][vec]")
{
    const Vec3f a{0.0F, 0.0F, 0.0F};
    const Vec3f b{4.0F, 8.0F, 12.0F};
    const Vec3f mid = lerp(a, b, 0.5F);
    REQUIRE(mid.x == Catch::Approx(2.0F));
    REQUIRE(mid.y == Catch::Approx(4.0F));
    REQUIRE(mid.z == Catch::Approx(6.0F));

    const Vec3f mixed = mix(a, b, 0.25F);
    REQUIRE(mixed.x == Catch::Approx(1.0F));

    const Vec3f same = damp(a, b, 5.0F, 0.0F);
    REQUIRE(same.x == Catch::Approx(0.0F));
    REQUIRE(same.y == Catch::Approx(0.0F));

    const Vec3f near_b = damp(a, b, 5.0F, 100.0F);
    REQUIRE(near_b.x == Catch::Approx(4.0F).margin(1e-6F));
    REQUIRE(near_b.z == Catch::Approx(12.0F).margin(1e-6F));
}

// ─────────────────────────────────────────────────────────────────────────────
// Penner easings
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("All easings: f(0) == 0 and f(1) == 1", "[math][easing]")
{
    constexpr float eps = 1e-5F;

    // All curves anchor at endpoints. Bounce/Elastic use exact early-outs at the
    // domain edges, so the equality is exact for those.
    REQUIRE(ease_linear(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_linear(1.0F) == Catch::Approx(1.0F).margin(eps));

    REQUIRE(ease_in_sine(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_sine(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_out_sine(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_out_sine(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_in_out_sine(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_out_sine(1.0F) == Catch::Approx(1.0F).margin(eps));

    REQUIRE(ease_in_quad(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_quad(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_out_quad(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_out_quad(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_in_out_quad(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_out_quad(1.0F) == Catch::Approx(1.0F).margin(eps));

    REQUIRE(ease_in_cubic(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_cubic(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_out_cubic(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_out_cubic(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_in_out_cubic(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_out_cubic(1.0F) == Catch::Approx(1.0F).margin(eps));

    REQUIRE(ease_in_quart(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_quart(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_out_quart(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_out_quart(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_in_out_quart(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_out_quart(1.0F) == Catch::Approx(1.0F).margin(eps));

    REQUIRE(ease_in_quint(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_quint(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_out_quint(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_out_quint(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_in_out_quint(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_out_quint(1.0F) == Catch::Approx(1.0F).margin(eps));

    REQUIRE(ease_in_expo(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_expo(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_out_expo(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_out_expo(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_in_out_expo(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_out_expo(1.0F) == Catch::Approx(1.0F).margin(eps));

    REQUIRE(ease_in_circ(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_circ(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_out_circ(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_out_circ(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_in_out_circ(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_out_circ(1.0F) == Catch::Approx(1.0F).margin(eps));

    REQUIRE(ease_in_back(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_back(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_out_back(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_out_back(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_in_out_back(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_out_back(1.0F) == Catch::Approx(1.0F).margin(eps));

    REQUIRE(ease_in_elastic(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_elastic(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_out_elastic(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_out_elastic(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_in_out_elastic(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_out_elastic(1.0F) == Catch::Approx(1.0F).margin(eps));

    REQUIRE(ease_in_bounce(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_bounce(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_out_bounce(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_out_bounce(1.0F) == Catch::Approx(1.0F).margin(eps));
    REQUIRE(ease_in_out_bounce(0.0F) == Catch::Approx(0.0F).margin(eps));
    REQUIRE(ease_in_out_bounce(1.0F) == Catch::Approx(1.0F).margin(eps));
}

TEST_CASE("Easings: In/Out reflection identity ease_in_X(t) == 1 - ease_out_X(1 - t)", "[math][easing]")
{
    // The reflection identity holds for the strictly-monotone families
    // (Sine/Quad/Cubic/Quart/Quint/Expo/Circ). Back/Elastic/Bounce use slightly
    // different In vs Out formulations and don't satisfy this exactly, so we
    // skip them here — their reflection is checked separately in the
    // monotonicity / overshoot tests.
    constexpr float eps = 1e-5F;
    for (int i = 1; i <= 31; ++i)
    {
        const float t = static_cast<float>(i) / 32.0F;
        const float u = 1.0F - t;

        REQUIRE(ease_in_sine(t) == Catch::Approx(1.0F - ease_out_sine(u)).margin(eps));
        REQUIRE(ease_in_quad(t) == Catch::Approx(1.0F - ease_out_quad(u)).margin(eps));
        REQUIRE(ease_in_cubic(t) == Catch::Approx(1.0F - ease_out_cubic(u)).margin(eps));
        REQUIRE(ease_in_quart(t) == Catch::Approx(1.0F - ease_out_quart(u)).margin(eps));
        REQUIRE(ease_in_quint(t) == Catch::Approx(1.0F - ease_out_quint(u)).margin(eps));
        REQUIRE(ease_in_circ(t) == Catch::Approx(1.0F - ease_out_circ(u)).margin(eps));
    }
}

TEST_CASE("Easings: Quad/Cubic/Quart/Quint/Sine/Circ are monotone non-decreasing", "[math][easing]")
{
    auto monotone = [](auto fn)
    {
        float prev = -1.0F;
        for (int i = 0; i <= 64; ++i)
        {
            const float t = static_cast<float>(i) / 64.0F;
            const float v = fn(t);
            REQUIRE(v >= prev - 1e-5F); // small slack for FP rounding
            prev = v;
        }
    };

    monotone([](float t) { return ease_in_sine(t); });
    monotone([](float t) { return ease_out_sine(t); });
    monotone([](float t) { return ease_in_out_sine(t); });
    monotone([](float t) { return ease_in_quad(t); });
    monotone([](float t) { return ease_out_quad(t); });
    monotone([](float t) { return ease_in_out_quad(t); });
    monotone([](float t) { return ease_in_cubic(t); });
    monotone([](float t) { return ease_out_cubic(t); });
    monotone([](float t) { return ease_in_out_cubic(t); });
    monotone([](float t) { return ease_in_quart(t); });
    monotone([](float t) { return ease_out_quart(t); });
    monotone([](float t) { return ease_in_out_quart(t); });
    monotone([](float t) { return ease_in_quint(t); });
    monotone([](float t) { return ease_out_quint(t); });
    monotone([](float t) { return ease_in_out_quint(t); });
    monotone([](float t) { return ease_in_circ(t); });
    monotone([](float t) { return ease_out_circ(t); });
    monotone([](float t) { return ease_in_out_circ(t); });
    monotone([](float t) { return ease_in_expo(t); });
    monotone([](float t) { return ease_out_expo(t); });
    monotone([](float t) { return ease_in_out_expo(t); });
}

TEST_CASE("Easings: Back/Elastic overshoot, Bounce stays in [0,1]", "[math][easing]")
{
    // Back overshoots below zero on the way out (in) and above one on the way out (out).
    bool back_in_undershot = false;
    bool back_out_overshot = false;
    for (int i = 1; i < 32; ++i)
    {
        const float t = static_cast<float>(i) / 32.0F;
        if (ease_in_back(t) < 0.0F)
            back_in_undershot = true;
        if (ease_out_back(t) > 1.0F)
            back_out_overshot = true;
    }
    REQUIRE(back_in_undershot);
    REQUIRE(back_out_overshot);

    // Elastic overshoots above 1 on out (and below 0 on in).
    bool elastic_out_overshot = false;
    bool elastic_in_undershot = false;
    for (int i = 1; i < 32; ++i)
    {
        const float t = static_cast<float>(i) / 32.0F;
        if (ease_out_elastic(t) > 1.0F)
            elastic_out_overshot = true;
        if (ease_in_elastic(t) < 0.0F)
            elastic_in_undershot = true;
    }
    REQUIRE(elastic_out_overshot);
    REQUIRE(elastic_in_undershot);

    // Bounce stays in [0, 1] (it touches endpoints repeatedly but doesn't escape).
    for (int i = 0; i <= 64; ++i)
    {
        const float t = static_cast<float>(i) / 64.0F;
        const float v = ease_out_bounce(t);
        REQUIRE(v >= 0.0F);
        REQUIRE(v <= 1.0F + 1e-5F);
    }
}
