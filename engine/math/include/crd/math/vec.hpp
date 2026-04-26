#pragma once

#include <crd/core/assert.hpp>
#include <crd/math/scalar.hpp>

#include <cmath>

namespace crd::math
{
template <MathScalar T> struct Vec2
{
    T x = static_cast<T>(0);
    T y = static_cast<T>(0);

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

template <MathScalar T> struct Vec3
{
    T x = static_cast<T>(0);
    T y = static_cast<T>(0);
    T z = static_cast<T>(0);

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

template <MathScalar T> struct Vec4
{
    T x = static_cast<T>(0);
    T y = static_cast<T>(0);
    T z = static_cast<T>(0);
    T w = static_cast<T>(0);

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

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Vec2<T>& lhs, const Vec2<T>& rhs) noexcept
{
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Vec3<T>& lhs, const Vec3<T>& rhs) noexcept
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Vec4<T>& lhs, const Vec4<T>& rhs) noexcept
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
}

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> operator+(Vec2<T> lhs, const Vec2<T>& rhs) noexcept
{
    lhs += rhs;
    return lhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> operator-(Vec2<T> lhs, const Vec2<T>& rhs) noexcept
{
    lhs -= rhs;
    return lhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> operator*(Vec2<T> lhs, T scalar) noexcept
{
    lhs *= scalar;
    return lhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> operator*(T scalar, Vec2<T> rhs) noexcept
{
    rhs *= scalar;
    return rhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> operator/(Vec2<T> lhs, T scalar) noexcept
{
    lhs /= scalar;
    return lhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> operator+(Vec3<T> lhs, const Vec3<T>& rhs) noexcept
{
    lhs += rhs;
    return lhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> operator-(Vec3<T> lhs, const Vec3<T>& rhs) noexcept
{
    lhs -= rhs;
    return lhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> operator*(Vec3<T> lhs, T scalar) noexcept
{
    lhs *= scalar;
    return lhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> operator*(T scalar, Vec3<T> rhs) noexcept
{
    rhs *= scalar;
    return rhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> operator/(Vec3<T> lhs, T scalar) noexcept
{
    lhs /= scalar;
    return lhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec4<T> operator+(Vec4<T> lhs, const Vec4<T>& rhs) noexcept
{
    lhs += rhs;
    return lhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec4<T> operator-(Vec4<T> lhs, const Vec4<T>& rhs) noexcept
{
    lhs -= rhs;
    return lhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec4<T> operator*(Vec4<T> lhs, T scalar) noexcept
{
    lhs *= scalar;
    return lhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec4<T> operator*(T scalar, Vec4<T> rhs) noexcept
{
    rhs *= scalar;
    return rhs;
}

template <MathScalar T> [[nodiscard]] constexpr Vec4<T> operator/(Vec4<T> lhs, T scalar) noexcept
{
    lhs /= scalar;
    return lhs;
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

using Vec2f = Vec2<crd::f32>;
using Vec3f = Vec3<crd::f32>;
using Vec4f = Vec4<crd::f32>;
using Vec2d = Vec2<crd::f64>;
using Vec3d = Vec3<crd::f64>;
using Vec4d = Vec4<crd::f64>;
} // namespace crd::math
