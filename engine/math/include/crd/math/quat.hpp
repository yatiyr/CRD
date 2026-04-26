#pragma once

#include <crd/math/mat.hpp>

#include <cmath>

namespace crd::math
{
template <MathScalar T> struct Quat
{
    T x = static_cast<T>(0);
    T y = static_cast<T>(0);
    T z = static_cast<T>(0);
    T w = static_cast<T>(1);

    constexpr Quat() noexcept = default;
    constexpr Quat(T x_in, T y_in, T z_in, T w_in) noexcept : x(x_in), y(y_in), z(z_in), w(w_in) {}

    [[nodiscard]] static constexpr Quat identity() noexcept
    {
        return Quat(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0), static_cast<T>(1));
    }
};

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Quat<T>& lhs, const Quat<T>& rhs) noexcept
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
}

template <MathScalar T> [[nodiscard]] constexpr T dot(const Quat<T>& lhs, const Quat<T>& rhs) noexcept
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z + lhs.w * rhs.w;
}

template <MathScalar T> [[nodiscard]] constexpr T length_squared(const Quat<T>& q) noexcept
{
    return dot(q, q);
}

template <MathScalar T> [[nodiscard]] inline T length(const Quat<T>& q) noexcept
{
    return static_cast<T>(std::sqrt(length_squared(q)));
}

template <MathScalar T> [[nodiscard]] constexpr Quat<T> conjugate(const Quat<T>& q) noexcept
{
    return Quat<T>(-q.x, -q.y, -q.z, q.w);
}

template <MathScalar T> [[nodiscard]] inline bool try_normalize(Quat<T>& q, T epsilon = default_epsilon<T>()) noexcept
{
    const T len = length(q);
    if (len <= epsilon)
    {
        return false;
    }
    q.x /= len;
    q.y /= len;
    q.z /= len;
    q.w /= len;
    return true;
}

template <MathScalar T> [[nodiscard]] inline Quat<T> normalized(Quat<T> q) noexcept
{
    const bool ok = try_normalize(q);
    CRD_ASSERT(ok);
    (void)ok;
    return q;
}

template <MathScalar T>
[[nodiscard]] inline bool try_inverse(const Quat<T>& q, Quat<T>& out, T epsilon = default_epsilon<T>()) noexcept
{
    const T len_sq = length_squared(q);
    if (len_sq <= epsilon)
    {
        return false;
    }

    const Quat<T> conj = conjugate(q);
    out = Quat<T>(conj.x / len_sq, conj.y / len_sq, conj.z / len_sq, conj.w / len_sq);
    return true;
}

template <MathScalar T> [[nodiscard]] inline Quat<T> inversed(const Quat<T>& q) noexcept
{
    Quat<T> out{};
    const bool ok = try_inverse(q, out);
    CRD_ASSERT(ok);
    (void)ok;
    return out;
}

template <MathScalar T> [[nodiscard]] constexpr Quat<T> operator*(const Quat<T>& lhs, const Quat<T>& rhs) noexcept
{
    return Quat<T>(lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
                   lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
                   lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
                   lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z);
}

template <MathScalar T> [[nodiscard]] inline Quat<T> from_axis_angle(const Vec3<T>& axis, T radians) noexcept
{
    Vec3<T> unit_axis = normalized(axis);
    const T half = radians * static_cast<T>(0.5);
    const T s = static_cast<T>(std::sin(half));
    const T c = static_cast<T>(std::cos(half));
    return Quat<T>(unit_axis.x * s, unit_axis.y * s, unit_axis.z * s, c);
}

template <MathScalar T> [[nodiscard]] inline Vec3<T> rotate_vector(const Quat<T>& q_in, const Vec3<T>& v) noexcept
{
    const Quat<T> q = normalized(q_in);
    const Vec3<T> qv(q.x, q.y, q.z);
    const Vec3<T> t = static_cast<T>(2) * cross(qv, v);
    return v + q.w * t + cross(qv, t);
}

template <MathScalar T> [[nodiscard]] inline Mat3<T> to_mat3(const Quat<T>& q_in) noexcept
{
    const Quat<T> q = normalized(q_in);
    const T xx = q.x * q.x;
    const T yy = q.y * q.y;
    const T zz = q.z * q.z;
    const T xy = q.x * q.y;
    const T xz = q.x * q.z;
    const T yz = q.y * q.z;
    const T wx = q.w * q.x;
    const T wy = q.w * q.y;
    const T wz = q.w * q.z;

    return Mat3<T>(Vec3<T>(static_cast<T>(1) - static_cast<T>(2) * (yy + zz), static_cast<T>(2) * (xy + wz),
                           static_cast<T>(2) * (xz - wy)),
                   Vec3<T>(static_cast<T>(2) * (xy - wz), static_cast<T>(1) - static_cast<T>(2) * (xx + zz),
                           static_cast<T>(2) * (yz + wx)),
                   Vec3<T>(static_cast<T>(2) * (xz + wy), static_cast<T>(2) * (yz - wx),
                           static_cast<T>(1) - static_cast<T>(2) * (xx + yy)));
}

template <MathScalar T> [[nodiscard]] inline Mat4<T> to_mat4(const Quat<T>& q) noexcept
{
    const Mat3<T> m3 = to_mat3(q);
    return Mat4<T>(Vec4<T>(m3.c0, static_cast<T>(0)), Vec4<T>(m3.c1, static_cast<T>(0)),
                   Vec4<T>(m3.c2, static_cast<T>(0)),
                   Vec4<T>(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0), static_cast<T>(1)));
}

template <MathScalar T> [[nodiscard]] inline Quat<T> from_mat3(const Mat3<T>& m) noexcept
{
    const T trace = m.c0.x + m.c1.y + m.c2.z;
    if (trace > static_cast<T>(0))
    {
        const T s = static_cast<T>(2) * static_cast<T>(std::sqrt(trace + static_cast<T>(1)));
        return normalized(
            Quat<T>((m.c1.z - m.c2.y) / s, (m.c2.x - m.c0.z) / s, (m.c0.y - m.c1.x) / s, static_cast<T>(0.25) * s));
    }
    if (m.c0.x > m.c1.y && m.c0.x > m.c2.z)
    {
        const T s = static_cast<T>(2) * static_cast<T>(std::sqrt(static_cast<T>(1) + m.c0.x - m.c1.y - m.c2.z));
        return normalized(
            Quat<T>(static_cast<T>(0.25) * s, (m.c1.x + m.c0.y) / s, (m.c2.x + m.c0.z) / s, (m.c1.z - m.c2.y) / s));
    }
    if (m.c1.y > m.c2.z)
    {
        const T s = static_cast<T>(2) * static_cast<T>(std::sqrt(static_cast<T>(1) + m.c1.y - m.c0.x - m.c2.z));
        return normalized(
            Quat<T>((m.c1.x + m.c0.y) / s, static_cast<T>(0.25) * s, (m.c2.y + m.c1.z) / s, (m.c2.x - m.c0.z) / s));
    }

    const T s = static_cast<T>(2) * static_cast<T>(std::sqrt(static_cast<T>(1) + m.c2.z - m.c0.x - m.c1.y));
    return normalized(
        Quat<T>((m.c2.x + m.c0.z) / s, (m.c2.y + m.c1.z) / s, static_cast<T>(0.25) * s, (m.c0.y - m.c1.x) / s));
}

template <MathScalar T> [[nodiscard]] inline Quat<T> nlerp(Quat<T> a, Quat<T> b, T t) noexcept
{
    if (dot(a, b) < static_cast<T>(0))
    {
        b = Quat<T>(-b.x, -b.y, -b.z, -b.w);
    }

    return normalized(
        Quat<T>(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
}

template <MathScalar T>
[[nodiscard]] inline Quat<T> slerp(Quat<T> a, Quat<T> b, T t, T epsilon = default_epsilon<T>()) noexcept
{
    T cos_theta = dot(a, b);
    if (cos_theta < static_cast<T>(0))
    {
        b = Quat<T>(-b.x, -b.y, -b.z, -b.w);
        cos_theta = -cos_theta;
    }

    if (cos_theta > static_cast<T>(1) - epsilon)
    {
        return nlerp(a, b, t);
    }

    cos_theta = clamp(cos_theta, static_cast<T>(-1), static_cast<T>(1));
    const T theta = static_cast<T>(std::acos(cos_theta));
    const T sin_theta = static_cast<T>(std::sin(theta));
    const T w0 = static_cast<T>(std::sin((static_cast<T>(1) - t) * theta)) / sin_theta;
    const T w1 = static_cast<T>(std::sin(t * theta)) / sin_theta;
    return Quat<T>(a.x * w0 + b.x * w1, a.y * w0 + b.y * w1, a.z * w0 + b.z * w1, a.w * w0 + b.w * w1);
}

using Quatf = Quat<crd::f32>;
using Quatd = Quat<crd::f64>;
} // namespace crd::math
