#pragma once

// Public scalar Vec types (Vec2/Vec3/Vec4 over MathScalar T).
//
// **Vec3f / Vec4f are intentionally NOT SIMD-ified at the per-instance
// API level.** Empirical data (v0e bench, win-release AVX2):
//   - per-instance Quatf compose under SIMD: 0.65× scalar speed
//     (load/store dominates the single op)
//   - Vec3f is 12 bytes — SIMD-ifying bloats sizeof to 16 (33% memory
//     waste in arrays + 1 wasted lane per op)
//   - compiler auto-vectorisation already SIMDifies tight loops over
//     scalar Vec3f/Vec4f at /O2; per-instance SIMD gives nothing extra
//
// For batched workloads (physics, animation, particles), use the
// `crd::math::simd::Soa<TChunk, Lane>` AoSoA substrate from v0b — the
// `BodyChunk8`-style pattern processes 8 entities × 3 axes per AVX op
// with zero waste. That's the SIMD path for Vec3-shaped data; do NOT
// reach for Vec3-as-__m128 (that's the Bullet btVector3 mistake).
//
// Mat4f IS SIMD-ified internally — it fills the SIMD register
// completely (4×4 = 64 bytes) and Mat*Mat does enough work to amortise
// the load cost (12.7× measured speedup). See mat_simd_f32.hpp.

#include <crd/core/assert.hpp>
#include <crd/math/scalar.hpp>

#include <cmath>

namespace crd::math
{
template <MathValue T> struct Vec2
{
    T x{};
    T y{};

    constexpr Vec2() noexcept = default;
    constexpr Vec2(T x_in, T y_in) noexcept : x(x_in), y(y_in) {}
    explicit constexpr Vec2(T scalar) noexcept : x(scalar), y(scalar) {}

    [[nodiscard]] constexpr T& operator[](crd::usize index) noexcept
    {
        CRD_ASSERT(index < 2);
        return index == 0 ? x : y;
    }

    [[nodiscard]] constexpr const T& operator[](crd::usize index) const noexcept
    {
        CRD_ASSERT(index < 2);
        return index == 0 ? x : y;
    }

    [[nodiscard]] constexpr Vec2 operator-() const noexcept { return Vec2(-x, -y); }

    constexpr Vec2& operator+=(const Vec2& rhs) noexcept
    {
        x += rhs.x;
        y += rhs.y;
        return *this;
    }

    constexpr Vec2& operator-=(const Vec2& rhs) noexcept
    {
        x -= rhs.x;
        y -= rhs.y;
        return *this;
    }

    constexpr Vec2& operator*=(T scalar) noexcept
    {
        x *= scalar;
        y *= scalar;
        return *this;
    }

    constexpr Vec2& operator/=(T scalar) noexcept
    {
        CRD_ASSERT(!approx_zero(scalar));
        x /= scalar;
        y /= scalar;
        return *this;
    }
};

template <MathValue T> struct Vec3
{
    T x{};
    T y{};
    T z{};

    constexpr Vec3() noexcept = default;
    constexpr Vec3(T x_in, T y_in, T z_in) noexcept : x(x_in), y(y_in), z(z_in) {}
    explicit constexpr Vec3(T scalar) noexcept : x(scalar), y(scalar), z(scalar) {}
    constexpr Vec3(const Vec2<T>& xy, T z_in) noexcept : x(xy.x), y(xy.y), z(z_in) {}

    [[nodiscard]] constexpr T& operator[](crd::usize index) noexcept
    {
        CRD_ASSERT(index < 3);
        if (index == 0)
        {
            return x;
        }
        if (index == 1)
        {
            return y;
        }
        return z;
    }

    [[nodiscard]] constexpr const T& operator[](crd::usize index) const noexcept
    {
        CRD_ASSERT(index < 3);
        if (index == 0)
        {
            return x;
        }
        if (index == 1)
        {
            return y;
        }
        return z;
    }

    [[nodiscard]] constexpr Vec3 operator-() const noexcept { return Vec3(-x, -y, -z); }

    constexpr Vec3& operator+=(const Vec3& rhs) noexcept
    {
        x += rhs.x;
        y += rhs.y;
        z += rhs.z;
        return *this;
    }

    constexpr Vec3& operator-=(const Vec3& rhs) noexcept
    {
        x -= rhs.x;
        y -= rhs.y;
        z -= rhs.z;
        return *this;
    }

    constexpr Vec3& operator*=(T scalar) noexcept
    {
        x *= scalar;
        y *= scalar;
        z *= scalar;
        return *this;
    }

    constexpr Vec3& operator/=(T scalar) noexcept
    {
        CRD_ASSERT(!approx_zero(scalar));
        x /= scalar;
        y /= scalar;
        z /= scalar;
        return *this;
    }
};

template <MathValue T> struct Vec4
{
    T x{};
    T y{};
    T z{};
    T w{};

    constexpr Vec4() noexcept = default;
    constexpr Vec4(T x_in, T y_in, T z_in, T w_in) noexcept : x(x_in), y(y_in), z(z_in), w(w_in) {}
    explicit constexpr Vec4(T scalar) noexcept : x(scalar), y(scalar), z(scalar), w(scalar) {}
    constexpr Vec4(const Vec3<T>& xyz, T w_in) noexcept : x(xyz.x), y(xyz.y), z(xyz.z), w(w_in) {}

    [[nodiscard]] constexpr T& operator[](crd::usize index) noexcept
    {
        CRD_ASSERT(index < 4);
        if (index == 0)
        {
            return x;
        }
        if (index == 1)
        {
            return y;
        }
        if (index == 2)
        {
            return z;
        }
        return w;
    }

    [[nodiscard]] constexpr const T& operator[](crd::usize index) const noexcept
    {
        CRD_ASSERT(index < 4);
        if (index == 0)
        {
            return x;
        }
        if (index == 1)
        {
            return y;
        }
        if (index == 2)
        {
            return z;
        }
        return w;
    }

    [[nodiscard]] constexpr Vec4 operator-() const noexcept { return Vec4(-x, -y, -z, -w); }

    constexpr Vec4& operator+=(const Vec4& rhs) noexcept
    {
        x += rhs.x;
        y += rhs.y;
        z += rhs.z;
        w += rhs.w;
        return *this;
    }

    constexpr Vec4& operator-=(const Vec4& rhs) noexcept
    {
        x -= rhs.x;
        y -= rhs.y;
        z -= rhs.z;
        w -= rhs.w;
        return *this;
    }

    constexpr Vec4& operator*=(T scalar) noexcept
    {
        x *= scalar;
        y *= scalar;
        z *= scalar;
        w *= scalar;
        return *this;
    }

    constexpr Vec4& operator/=(T scalar) noexcept
    {
        CRD_ASSERT(!approx_zero(scalar));
        x /= scalar;
        y /= scalar;
        z /= scalar;
        w /= scalar;
        return *this;
    }
};

// Operators that work for both raw scalars AND Quantity types (per ADR-0078
// §2 D3). Reductions (dot/cross/length/...) below stay on MathScalar and
// refuse Quantity at the type level.

template <MathValue T> [[nodiscard]] constexpr bool operator==(const Vec2<T>& lhs, const Vec2<T>& rhs) noexcept
{
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

template <MathValue T> [[nodiscard]] constexpr bool operator==(const Vec3<T>& lhs, const Vec3<T>& rhs) noexcept
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

template <MathValue T> [[nodiscard]] constexpr bool operator==(const Vec4<T>& lhs, const Vec4<T>& rhs) noexcept
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
}

template <MathValue T> [[nodiscard]] constexpr Vec2<T> operator+(Vec2<T> lhs, const Vec2<T>& rhs) noexcept
{
    lhs += rhs;
    return lhs;
}

template <MathValue T> [[nodiscard]] constexpr Vec2<T> operator-(Vec2<T> lhs, const Vec2<T>& rhs) noexcept
{
    lhs -= rhs;
    return lhs;
}

template <MathValue T, typename S> [[nodiscard]] constexpr Vec2<T> operator*(Vec2<T> lhs, S scalar) noexcept
    requires requires(T t, S s) { { t * s } -> std::same_as<T>; }
{
    return Vec2<T>{lhs.x * scalar, lhs.y * scalar};
}

template <MathValue T, typename S> [[nodiscard]] constexpr Vec2<T> operator*(S scalar, Vec2<T> rhs) noexcept
    requires requires(S s, T t) { { s * t } -> std::same_as<T>; }
{
    return Vec2<T>{scalar * rhs.x, scalar * rhs.y};
}

template <MathValue T, typename S> [[nodiscard]] constexpr Vec2<T> operator/(Vec2<T> lhs, S scalar) noexcept
    requires requires(T t, S s) { { t / s } -> std::same_as<T>; }
{
    return Vec2<T>{lhs.x / scalar, lhs.y / scalar};
}

template <MathValue T> [[nodiscard]] constexpr Vec3<T> operator+(Vec3<T> lhs, const Vec3<T>& rhs) noexcept
{
    lhs += rhs;
    return lhs;
}

template <MathValue T> [[nodiscard]] constexpr Vec3<T> operator-(Vec3<T> lhs, const Vec3<T>& rhs) noexcept
{
    lhs -= rhs;
    return lhs;
}

template <MathValue T, typename S> [[nodiscard]] constexpr Vec3<T> operator*(Vec3<T> lhs, S scalar) noexcept
    requires requires(T t, S s) { { t * s } -> std::same_as<T>; }
{
    return Vec3<T>{lhs.x * scalar, lhs.y * scalar, lhs.z * scalar};
}

template <MathValue T, typename S> [[nodiscard]] constexpr Vec3<T> operator*(S scalar, Vec3<T> rhs) noexcept
    requires requires(S s, T t) { { s * t } -> std::same_as<T>; }
{
    return Vec3<T>{scalar * rhs.x, scalar * rhs.y, scalar * rhs.z};
}

template <MathValue T, typename S> [[nodiscard]] constexpr Vec3<T> operator/(Vec3<T> lhs, S scalar) noexcept
    requires requires(T t, S s) { { t / s } -> std::same_as<T>; }
{
    return Vec3<T>{lhs.x / scalar, lhs.y / scalar, lhs.z / scalar};
}

template <MathValue T> [[nodiscard]] constexpr Vec4<T> operator+(Vec4<T> lhs, const Vec4<T>& rhs) noexcept
{
    lhs += rhs;
    return lhs;
}

template <MathValue T> [[nodiscard]] constexpr Vec4<T> operator-(Vec4<T> lhs, const Vec4<T>& rhs) noexcept
{
    lhs -= rhs;
    return lhs;
}

template <MathValue T, typename S> [[nodiscard]] constexpr Vec4<T> operator*(Vec4<T> lhs, S scalar) noexcept
    requires requires(T t, S s) { { t * s } -> std::same_as<T>; }
{
    return Vec4<T>{lhs.x * scalar, lhs.y * scalar, lhs.z * scalar, lhs.w * scalar};
}

template <MathValue T, typename S> [[nodiscard]] constexpr Vec4<T> operator*(S scalar, Vec4<T> rhs) noexcept
    requires requires(S s, T t) { { s * t } -> std::same_as<T>; }
{
    return Vec4<T>{scalar * rhs.x, scalar * rhs.y, scalar * rhs.z, scalar * rhs.w};
}

template <MathValue T, typename S> [[nodiscard]] constexpr Vec4<T> operator/(Vec4<T> lhs, S scalar) noexcept
    requires requires(T t, S s) { { t / s } -> std::same_as<T>; }
{
    return Vec4<T>{lhs.x / scalar, lhs.y / scalar, lhs.z / scalar, lhs.w / scalar};
}

template <MathScalar T> [[nodiscard]] constexpr T dot(const Vec2<T>& lhs, const Vec2<T>& rhs) noexcept
{
    return lhs.x * rhs.x + lhs.y * rhs.y;
}

template <MathScalar T> [[nodiscard]] constexpr T dot(const Vec3<T>& lhs, const Vec3<T>& rhs) noexcept
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

template <MathScalar T> [[nodiscard]] constexpr T dot(const Vec4<T>& lhs, const Vec4<T>& rhs) noexcept
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z + lhs.w * rhs.w;
}

template <MathScalar T> [[nodiscard]] constexpr T cross(const Vec2<T>& lhs, const Vec2<T>& rhs) noexcept
{
    return lhs.x * rhs.y - lhs.y * rhs.x;
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> cross(const Vec3<T>& lhs, const Vec3<T>& rhs) noexcept
{
    return Vec3<T>(lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z, lhs.x * rhs.y - lhs.y * rhs.x);
}

template <MathScalar T> [[nodiscard]] constexpr T length_squared(const Vec2<T>& v) noexcept
{
    return dot(v, v);
}

template <MathScalar T> [[nodiscard]] constexpr T length_squared(const Vec3<T>& v) noexcept
{
    return dot(v, v);
}

template <MathScalar T> [[nodiscard]] constexpr T length_squared(const Vec4<T>& v) noexcept
{
    return dot(v, v);
}

template <MathScalar T> [[nodiscard]] inline T length(const Vec2<T>& v) noexcept
{
    return static_cast<T>(std::sqrt(length_squared(v)));
}

template <MathScalar T> [[nodiscard]] inline T length(const Vec3<T>& v) noexcept
{
    return static_cast<T>(std::sqrt(length_squared(v)));
}

template <MathScalar T> [[nodiscard]] inline T length(const Vec4<T>& v) noexcept
{
    return static_cast<T>(std::sqrt(length_squared(v)));
}

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> hadamard(const Vec2<T>& lhs, const Vec2<T>& rhs) noexcept
{
    return Vec2<T>(lhs.x * rhs.x, lhs.y * rhs.y);
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> hadamard(const Vec3<T>& lhs, const Vec3<T>& rhs) noexcept
{
    return Vec3<T>(lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z);
}

template <MathScalar T> [[nodiscard]] constexpr Vec4<T> hadamard(const Vec4<T>& lhs, const Vec4<T>& rhs) noexcept
{
    return Vec4<T>(lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z, lhs.w * rhs.w);
}

template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const Vec2<T>& lhs, const Vec2<T>& rhs) noexcept
{
    return length_squared(lhs - rhs);
}

template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const Vec3<T>& lhs, const Vec3<T>& rhs) noexcept
{
    return length_squared(lhs - rhs);
}

template <MathScalar T> [[nodiscard]] constexpr T distance_squared(const Vec4<T>& lhs, const Vec4<T>& rhs) noexcept
{
    return length_squared(lhs - rhs);
}

template <MathScalar T> [[nodiscard]] inline T distance(const Vec2<T>& lhs, const Vec2<T>& rhs) noexcept
{
    return length(lhs - rhs);
}

template <MathScalar T> [[nodiscard]] inline T distance(const Vec3<T>& lhs, const Vec3<T>& rhs) noexcept
{
    return length(lhs - rhs);
}

template <MathScalar T> [[nodiscard]] inline T distance(const Vec4<T>& lhs, const Vec4<T>& rhs) noexcept
{
    return length(lhs - rhs);
}

template <MathScalar T> [[nodiscard]] inline bool try_normalize(Vec2<T>& v, T epsilon = default_epsilon<T>()) noexcept
{
    const T len = length(v);
    if (len <= epsilon)
    {
        return false;
    }
    v /= len;
    return true;
}

template <MathScalar T> [[nodiscard]] inline bool try_normalize(Vec3<T>& v, T epsilon = default_epsilon<T>()) noexcept
{
    const T len = length(v);
    if (len <= epsilon)
    {
        return false;
    }
    v /= len;
    return true;
}

template <MathScalar T> [[nodiscard]] inline bool try_normalize(Vec4<T>& v, T epsilon = default_epsilon<T>()) noexcept
{
    const T len = length(v);
    if (len <= epsilon)
    {
        return false;
    }
    v /= len;
    return true;
}

// ---------------------------------------------------------------------------
// Vec<Quantity<D, T>> reductions (Phase 3.1.7.5 v0d-1 / ADR-0078 §4 D26).
//
// Per ADR-0078 §1 D2 + §3 D24, reductions on `Vec<Quantity>` are scoped to
// the cases where the typed return type follows mechanically from DimMul /
// re-tag, WITHOUT needing fractional-exponent `DimRoot<>`:
//
//   dot(Vec<Q1>, Vec<Q2>)           -> Quantity<DimMul<D1, D2>, T>
//   length_squared(Vec<Q>)          -> Quantity<DimMul<D, D>, T>     (Q²)
//   length(Vec<Q>)                  -> Quantity<D, T>                (Q — sqrt of Q² re-tagged)
//   cross(Vec<Q>, Vec<Q>)           -> Vec3<Quantity<DimMul<D, D>, T>> (Vec<Q²>)
//   distance(Vec<Q>, Vec<Q>)        -> Quantity<D, T>                (Q — length of delta)
//   distance_squared(Vec<Q>,Vec<Q>) -> Quantity<DimMul<D, D>, T>     (Q²)
//   hadamard(Vec<Q1>, Vec<Q2>)      -> Vec<Quantity<DimMul<D1, D2>, T>>
//   normalized(Vec<Q>)              -> Vec<T>                        (dimensionless unit vector)
//
// `try_normalize(Vec<Q>&)` mutation-in-place is intentionally NOT widened —
// the result type (Vec<f32>) differs from the input (Vec<Quantity>), so the
// in-place signature doesn't compose. Callers use `normalized(...)` by value.
//
// Cases the v0d-1 surface deliberately does NOT cover (need fractional-
// exponent dims): `sqrt(area_quantity) -> length_quantity` where the user
// hands a raw Area Quantity and wants a Length Quantity back; this requires
// `DimRoot<>`. Deferred to v0d-5 if a consumer surfaces; the length(Vec<Q>)
// pattern above sidesteps the issue because the return type is known
// statically from the input Vec<Q>.

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr crd::units::Quantity<crd::units::DimMul<D1, D2>, T>
dot(const Vec2<crd::units::Quantity<D1, T>>& a, const Vec2<crd::units::Quantity<D2, T>>& b) noexcept
{
    return a.x * b.x + a.y * b.y;
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr crd::units::Quantity<crd::units::DimMul<D1, D2>, T>
dot(const Vec3<crd::units::Quantity<D1, T>>& a, const Vec3<crd::units::Quantity<D2, T>>& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr crd::units::Quantity<crd::units::DimMul<D1, D2>, T>
dot(const Vec4<crd::units::Quantity<D1, T>>& a, const Vec4<crd::units::Quantity<D2, T>>& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

template <typename D, typename T>
[[nodiscard]] constexpr crd::units::Quantity<crd::units::DimMul<D, D>, T>
length_squared(const Vec2<crd::units::Quantity<D, T>>& v) noexcept
{
    return dot(v, v);
}

template <typename D, typename T>
[[nodiscard]] constexpr crd::units::Quantity<crd::units::DimMul<D, D>, T>
length_squared(const Vec3<crd::units::Quantity<D, T>>& v) noexcept
{
    return dot(v, v);
}

template <typename D, typename T>
[[nodiscard]] constexpr crd::units::Quantity<crd::units::DimMul<D, D>, T>
length_squared(const Vec4<crd::units::Quantity<D, T>>& v) noexcept
{
    return dot(v, v);
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T>
length(const Vec2<crd::units::Quantity<D, T>>& v) noexcept
{
    return crd::units::Quantity<D, T>{static_cast<T>(std::sqrt(length_squared(v).value))};
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T>
length(const Vec3<crd::units::Quantity<D, T>>& v) noexcept
{
    return crd::units::Quantity<D, T>{static_cast<T>(std::sqrt(length_squared(v).value))};
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T>
length(const Vec4<crd::units::Quantity<D, T>>& v) noexcept
{
    return crd::units::Quantity<D, T>{static_cast<T>(std::sqrt(length_squared(v).value))};
}

template <typename D, typename T>
[[nodiscard]] constexpr Vec3<crd::units::Quantity<crd::units::DimMul<D, D>, T>>
cross(const Vec3<crd::units::Quantity<D, T>>& a, const Vec3<crd::units::Quantity<D, T>>& b) noexcept
{
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Vec2<crd::units::Quantity<crd::units::DimMul<D1, D2>, T>>
hadamard(const Vec2<crd::units::Quantity<D1, T>>& a, const Vec2<crd::units::Quantity<D2, T>>& b) noexcept
{
    return {a.x * b.x, a.y * b.y};
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Vec3<crd::units::Quantity<crd::units::DimMul<D1, D2>, T>>
hadamard(const Vec3<crd::units::Quantity<D1, T>>& a, const Vec3<crd::units::Quantity<D2, T>>& b) noexcept
{
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Vec4<crd::units::Quantity<crd::units::DimMul<D1, D2>, T>>
hadamard(const Vec4<crd::units::Quantity<D1, T>>& a, const Vec4<crd::units::Quantity<D2, T>>& b) noexcept
{
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

template <typename D, typename T>
[[nodiscard]] constexpr crd::units::Quantity<crd::units::DimMul<D, D>, T>
distance_squared(const Vec2<crd::units::Quantity<D, T>>& a, const Vec2<crd::units::Quantity<D, T>>& b) noexcept
{
    return length_squared(a - b);
}

template <typename D, typename T>
[[nodiscard]] constexpr crd::units::Quantity<crd::units::DimMul<D, D>, T>
distance_squared(const Vec3<crd::units::Quantity<D, T>>& a, const Vec3<crd::units::Quantity<D, T>>& b) noexcept
{
    return length_squared(a - b);
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T>
distance(const Vec2<crd::units::Quantity<D, T>>& a, const Vec2<crd::units::Quantity<D, T>>& b) noexcept
{
    return length(a - b);
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T>
distance(const Vec3<crd::units::Quantity<D, T>>& a, const Vec3<crd::units::Quantity<D, T>>& b) noexcept
{
    return length(a - b);
}

template <typename D, typename T>
[[nodiscard]] inline Vec2<T> normalized(const Vec2<crd::units::Quantity<D, T>>& v) noexcept
{
    const crd::units::Quantity<D, T> len_q = length(v);
    CRD_ASSERT(len_q.value > default_epsilon<T>());
    return Vec2<T>{v.x.value / len_q.value, v.y.value / len_q.value};
}

template <typename D, typename T>
[[nodiscard]] inline Vec3<T> normalized(const Vec3<crd::units::Quantity<D, T>>& v) noexcept
{
    const crd::units::Quantity<D, T> len_q = length(v);
    CRD_ASSERT(len_q.value > default_epsilon<T>());
    return Vec3<T>{v.x.value / len_q.value, v.y.value / len_q.value, v.z.value / len_q.value};
}

template <typename D, typename T>
[[nodiscard]] inline Vec4<T> normalized(const Vec4<crd::units::Quantity<D, T>>& v) noexcept
{
    const crd::units::Quantity<D, T> len_q = length(v);
    CRD_ASSERT(len_q.value > default_epsilon<T>());
    return Vec4<T>{v.x.value / len_q.value, v.y.value / len_q.value,
                   v.z.value / len_q.value, v.w.value / len_q.value};
}

template <MathScalar T> [[nodiscard]] inline Vec2<T> normalized(Vec2<T> v) noexcept
{
    const bool ok = try_normalize(v);
    CRD_ASSERT(ok);
    (void)ok;
    return v;
}

template <MathScalar T> [[nodiscard]] inline Vec3<T> normalized(Vec3<T> v) noexcept
{
    const bool ok = try_normalize(v);
    CRD_ASSERT(ok);
    (void)ok;
    return v;
}

template <MathScalar T> [[nodiscard]] inline Vec4<T> normalized(Vec4<T> v) noexcept
{
    const bool ok = try_normalize(v);
    CRD_ASSERT(ok);
    (void)ok;
    return v;
}

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> lerp(const Vec2<T>& a, const Vec2<T>& b, T t) noexcept
{
    return a + (b - a) * t;
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> lerp(const Vec3<T>& a, const Vec3<T>& b, T t) noexcept
{
    return a + (b - a) * t;
}

template <MathScalar T> [[nodiscard]] constexpr Vec4<T> lerp(const Vec4<T>& a, const Vec4<T>& b, T t) noexcept
{
    return a + (b - a) * t;
}

// GLSL alias for vector lerp.
template <MathScalar T> [[nodiscard]] constexpr Vec2<T> mix(const Vec2<T>& a, const Vec2<T>& b, T t) noexcept
{
    return lerp(a, b, t);
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> mix(const Vec3<T>& a, const Vec3<T>& b, T t) noexcept
{
    return lerp(a, b, t);
}

template <MathScalar T> [[nodiscard]] constexpr Vec4<T> mix(const Vec4<T>& a, const Vec4<T>& b, T t) noexcept
{
    return lerp(a, b, t);
}

// Componentwise frame-rate-independent exponential approach. See `damp` in scalar.hpp.
template <MathScalar T> [[nodiscard]] inline Vec2<T> damp(const Vec2<T>& a, const Vec2<T>& b, T lambda, T dt) noexcept
{
    return lerp(a, b, static_cast<T>(1) - std::exp(-lambda * dt));
}

template <MathScalar T> [[nodiscard]] inline Vec3<T> damp(const Vec3<T>& a, const Vec3<T>& b, T lambda, T dt) noexcept
{
    return lerp(a, b, static_cast<T>(1) - std::exp(-lambda * dt));
}

template <MathScalar T> [[nodiscard]] inline Vec4<T> damp(const Vec4<T>& a, const Vec4<T>& b, T lambda, T dt) noexcept
{
    return lerp(a, b, static_cast<T>(1) - std::exp(-lambda * dt));
}

using Vec2f = Vec2<crd::f32>;
using Vec3f = Vec3<crd::f32>;
using Vec4f = Vec4<crd::f32>;
using Vec2d = Vec2<crd::f64>;
using Vec3d = Vec3<crd::f64>;
using Vec4d = Vec4<crd::f64>;

// to_raw_vec — extract the underlying scalar Vec from a Vec<Quantity<>>
// (Phase 3.1.7.5 v0b-1 / ADR-0078 §2 D3). SIMD / GPU upload / Mat4 boundaries
// reach for this when they need the bare-scalar layout.
template <typename D, typename T>
[[nodiscard]] constexpr Vec2<T> to_raw_vec(const Vec2<crd::units::Quantity<D, T>>& v) noexcept
{
    return Vec2<T>{v.x.value, v.y.value};
}

template <typename D, typename T>
[[nodiscard]] constexpr Vec3<T> to_raw_vec(const Vec3<crd::units::Quantity<D, T>>& v) noexcept
{
    return Vec3<T>{v.x.value, v.y.value, v.z.value};
}

template <typename D, typename T>
[[nodiscard]] constexpr Vec4<T> to_raw_vec(const Vec4<crd::units::Quantity<D, T>>& v) noexcept
{
    return Vec4<T>{v.x.value, v.y.value, v.z.value, v.w.value};
}

// Inverse: tag a raw scalar Vec with a Dim. Useful at the boundary where
// data arrives untyped (glTF, asset cooker, GPU readback) and is re-tagged
// at the API surface.
template <typename D, typename T>
[[nodiscard]] constexpr Vec2<crd::units::Quantity<D, T>> from_raw_vec(const Vec2<T>& v) noexcept
{
    return Vec2<crd::units::Quantity<D, T>>{crd::units::Quantity<D, T>{v.x},
                                             crd::units::Quantity<D, T>{v.y}};
}

template <typename D, typename T>
[[nodiscard]] constexpr Vec3<crd::units::Quantity<D, T>> from_raw_vec(const Vec3<T>& v) noexcept
{
    return Vec3<crd::units::Quantity<D, T>>{crd::units::Quantity<D, T>{v.x},
                                             crd::units::Quantity<D, T>{v.y},
                                             crd::units::Quantity<D, T>{v.z}};
}

template <typename D, typename T>
[[nodiscard]] constexpr Vec4<crd::units::Quantity<D, T>> from_raw_vec(const Vec4<T>& v) noexcept
{
    return Vec4<crd::units::Quantity<D, T>>{crd::units::Quantity<D, T>{v.x},
                                             crd::units::Quantity<D, T>{v.y},
                                             crd::units::Quantity<D, T>{v.z},
                                             crd::units::Quantity<D, T>{v.w}};
}

// Cross-Dim Vec<Quantity> * Quantity overloads (Phase 3.1.7.5 v0c-1).
// Element-wise multiplication where the per-element result Dim differs
// from the source Dim. Lets integrator math compose end-to-end at the
// type level: Vec3<Velocity> * Time -> Vec3<Length>, Vec3<Force> *
// InverseMass -> Vec3<Acceleration>, etc.
//
// Same Vec semantics as the same-result-Dim operator: element-wise only,
// no reductions. Result Dim = DimMul<source Dim, scalar Dim>.

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Vec2<crd::units::Quantity<crd::units::DimMul<D1, D2>, T>>
operator*(const Vec2<crd::units::Quantity<D1, T>>& v, crd::units::Quantity<D2, T> q) noexcept
{
    return {v.x * q, v.y * q};
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Vec2<crd::units::Quantity<crd::units::DimMul<D1, D2>, T>>
operator*(crd::units::Quantity<D1, T> q, const Vec2<crd::units::Quantity<D2, T>>& v) noexcept
{
    return {q * v.x, q * v.y};
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Vec3<crd::units::Quantity<crd::units::DimMul<D1, D2>, T>>
operator*(const Vec3<crd::units::Quantity<D1, T>>& v, crd::units::Quantity<D2, T> q) noexcept
{
    return {v.x * q, v.y * q, v.z * q};
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Vec3<crd::units::Quantity<crd::units::DimMul<D1, D2>, T>>
operator*(crd::units::Quantity<D1, T> q, const Vec3<crd::units::Quantity<D2, T>>& v) noexcept
{
    return {q * v.x, q * v.y, q * v.z};
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Vec4<crd::units::Quantity<crd::units::DimMul<D1, D2>, T>>
operator*(const Vec4<crd::units::Quantity<D1, T>>& v, crd::units::Quantity<D2, T> q) noexcept
{
    return {v.x * q, v.y * q, v.z * q, v.w * q};
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Vec4<crd::units::Quantity<crd::units::DimMul<D1, D2>, T>>
operator*(crd::units::Quantity<D1, T> q, const Vec4<crd::units::Quantity<D2, T>>& v) noexcept
{
    return {q * v.x, q * v.y, q * v.z, q * v.w};
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Vec2<crd::units::Quantity<crd::units::DimDiv<D1, D2>, T>>
operator/(const Vec2<crd::units::Quantity<D1, T>>& v, crd::units::Quantity<D2, T> q) noexcept
{
    return {v.x / q, v.y / q};
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Vec3<crd::units::Quantity<crd::units::DimDiv<D1, D2>, T>>
operator/(const Vec3<crd::units::Quantity<D1, T>>& v, crd::units::Quantity<D2, T> q) noexcept
{
    return {v.x / q, v.y / q, v.z / q};
}

template <typename D1, typename D2, typename T>
[[nodiscard]] constexpr Vec4<crd::units::Quantity<crd::units::DimDiv<D1, D2>, T>>
operator/(const Vec4<crd::units::Quantity<D1, T>>& v, crd::units::Quantity<D2, T> q) noexcept
{
    return {v.x / q, v.y / q, v.z / q, v.w / q};
}

} // namespace crd::math
