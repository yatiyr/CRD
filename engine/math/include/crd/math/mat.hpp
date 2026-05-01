#pragma once

#include <crd/math/vec.hpp>

namespace crd::math
{
template <MathScalar T> struct Mat2
{
    Vec2<T> c0;
    Vec2<T> c1;

    constexpr Mat2() noexcept = default;
    constexpr Mat2(const Vec2<T>& col0, const Vec2<T>& col1) noexcept : c0(col0), c1(col1) {}
    explicit constexpr Mat2(T diagonal) noexcept : c0(diagonal, static_cast<T>(0)), c1(static_cast<T>(0), diagonal) {}

    [[nodiscard]] static constexpr Mat2 zero() noexcept { return Mat2{}; }
    [[nodiscard]] static constexpr Mat2 identity() noexcept { return Mat2(static_cast<T>(1)); }

    [[nodiscard]] constexpr Vec2<T>& operator[](crd::usize column) noexcept
    {
        CRD_ASSERT(column < 2);
        return column == 0 ? c0 : c1;
    }

    [[nodiscard]] constexpr const Vec2<T>& operator[](crd::usize column) const noexcept
    {
        CRD_ASSERT(column < 2);
        return column == 0 ? c0 : c1;
    }
};

template <MathScalar T> struct Mat3
{
    Vec3<T> c0;
    Vec3<T> c1;
    Vec3<T> c2;

    constexpr Mat3() noexcept = default;
    constexpr Mat3(const Vec3<T>& col0, const Vec3<T>& col1, const Vec3<T>& col2) noexcept
        : c0(col0), c1(col1), c2(col2)
    {
    }
    explicit constexpr Mat3(T diagonal) noexcept
        : c0(diagonal, static_cast<T>(0), static_cast<T>(0)), c1(static_cast<T>(0), diagonal, static_cast<T>(0)),
          c2(static_cast<T>(0), static_cast<T>(0), diagonal)
    {
    }

    [[nodiscard]] static constexpr Mat3 zero() noexcept { return Mat3{}; }
    [[nodiscard]] static constexpr Mat3 identity() noexcept { return Mat3(static_cast<T>(1)); }

    [[nodiscard]] constexpr Vec3<T>& operator[](crd::usize column) noexcept
    {
        CRD_ASSERT(column < 3);
        if (column == 0)
        {
            return c0;
        }
        if (column == 1)
        {
            return c1;
        }
        return c2;
    }

    [[nodiscard]] constexpr const Vec3<T>& operator[](crd::usize column) const noexcept
    {
        CRD_ASSERT(column < 3);
        if (column == 0)
        {
            return c0;
        }
        if (column == 1)
        {
            return c1;
        }
        return c2;
    }
};

template <MathScalar T> struct Mat4
{
    Vec4<T> c0;
    Vec4<T> c1;
    Vec4<T> c2;
    Vec4<T> c3;

    constexpr Mat4() noexcept = default;
    constexpr Mat4(const Vec4<T>& col0, const Vec4<T>& col1, const Vec4<T>& col2, const Vec4<T>& col3) noexcept
        : c0(col0), c1(col1), c2(col2), c3(col3)
    {
    }
    explicit constexpr Mat4(T diagonal) noexcept
        : c0(diagonal, static_cast<T>(0), static_cast<T>(0), static_cast<T>(0)),
          c1(static_cast<T>(0), diagonal, static_cast<T>(0), static_cast<T>(0)),
          c2(static_cast<T>(0), static_cast<T>(0), diagonal, static_cast<T>(0)),
          c3(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0), diagonal)
    {
    }

    [[nodiscard]] static constexpr Mat4 zero() noexcept { return Mat4{}; }
    [[nodiscard]] static constexpr Mat4 identity() noexcept { return Mat4(static_cast<T>(1)); }

    [[nodiscard]] constexpr Vec4<T>& operator[](crd::usize column) noexcept
    {
        CRD_ASSERT(column < 4);
        if (column == 0)
        {
            return c0;
        }
        if (column == 1)
        {
            return c1;
        }
        if (column == 2)
        {
            return c2;
        }
        return c3;
    }

    [[nodiscard]] constexpr const Vec4<T>& operator[](crd::usize column) const noexcept
    {
        CRD_ASSERT(column < 4);
        if (column == 0)
        {
            return c0;
        }
        if (column == 1)
        {
            return c1;
        }
        if (column == 2)
        {
            return c2;
        }
        return c3;
    }
};

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Mat2<T>& lhs, const Mat2<T>& rhs) noexcept
{
    return lhs.c0 == rhs.c0 && lhs.c1 == rhs.c1;
}

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Mat3<T>& lhs, const Mat3<T>& rhs) noexcept
{
    return lhs.c0 == rhs.c0 && lhs.c1 == rhs.c1 && lhs.c2 == rhs.c2;
}

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Mat4<T>& lhs, const Mat4<T>& rhs) noexcept
{
    return lhs.c0 == rhs.c0 && lhs.c1 == rhs.c1 && lhs.c2 == rhs.c2 && lhs.c3 == rhs.c3;
}

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> operator*(const Mat2<T>& lhs, const Vec2<T>& rhs) noexcept
{
    return lhs.c0 * rhs.x + lhs.c1 * rhs.y;
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> operator*(const Mat3<T>& lhs, const Vec3<T>& rhs) noexcept
{
    return lhs.c0 * rhs.x + lhs.c1 * rhs.y + lhs.c2 * rhs.z;
}

template <MathScalar T> [[nodiscard]] constexpr Vec4<T> operator*(const Mat4<T>& lhs, const Vec4<T>& rhs) noexcept
{
    return lhs.c0 * rhs.x + lhs.c1 * rhs.y + lhs.c2 * rhs.z + lhs.c3 * rhs.w;
}

template <MathScalar T> [[nodiscard]] constexpr Mat2<T> operator*(const Mat2<T>& lhs, const Mat2<T>& rhs) noexcept
{
    return Mat2<T>(lhs * rhs.c0, lhs * rhs.c1);
}

template <MathScalar T> [[nodiscard]] constexpr Mat3<T> operator*(const Mat3<T>& lhs, const Mat3<T>& rhs) noexcept
{
    return Mat3<T>(lhs * rhs.c0, lhs * rhs.c1, lhs * rhs.c2);
}

template <MathScalar T> [[nodiscard]] constexpr Mat4<T> operator*(const Mat4<T>& lhs, const Mat4<T>& rhs) noexcept
{
    return Mat4<T>(lhs * rhs.c0, lhs * rhs.c1, lhs * rhs.c2, lhs * rhs.c3);
}

template <MathScalar T> [[nodiscard]] constexpr Mat2<T> transpose(const Mat2<T>& m) noexcept
{
    return Mat2<T>(Vec2<T>(m.c0.x, m.c1.x), Vec2<T>(m.c0.y, m.c1.y));
}

template <MathScalar T> [[nodiscard]] constexpr Mat3<T> transpose(const Mat3<T>& m) noexcept
{
    return Mat3<T>(Vec3<T>(m.c0.x, m.c1.x, m.c2.x), Vec3<T>(m.c0.y, m.c1.y, m.c2.y), Vec3<T>(m.c0.z, m.c1.z, m.c2.z));
}

template <MathScalar T> [[nodiscard]] constexpr Mat4<T> transpose(const Mat4<T>& m) noexcept
{
    return Mat4<T>(Vec4<T>(m.c0.x, m.c1.x, m.c2.x, m.c3.x), Vec4<T>(m.c0.y, m.c1.y, m.c2.y, m.c3.y),
                   Vec4<T>(m.c0.z, m.c1.z, m.c2.z, m.c3.z), Vec4<T>(m.c0.w, m.c1.w, m.c2.w, m.c3.w));
}

// Laplace cofactor expansion for 4x4 inverse (column-major: c0..c3, each a Vec4 column).
// Undefined behaviour if the matrix is singular (det == 0).
template <MathScalar T> [[nodiscard]] constexpr Mat4<T> inverse(const Mat4<T>& m) noexcept
{
    const T c00 = m.c2.z * m.c3.w - m.c3.z * m.c2.w;
    const T c02 = m.c1.z * m.c3.w - m.c3.z * m.c1.w;
    const T c03 = m.c1.z * m.c2.w - m.c2.z * m.c1.w;
    const T c04 = m.c2.y * m.c3.w - m.c3.y * m.c2.w;
    const T c06 = m.c1.y * m.c3.w - m.c3.y * m.c1.w;
    const T c07 = m.c1.y * m.c2.w - m.c2.y * m.c1.w;
    const T c08 = m.c2.y * m.c3.z - m.c3.y * m.c2.z;
    const T c10 = m.c1.y * m.c3.z - m.c3.y * m.c1.z;
    const T c11 = m.c1.y * m.c2.z - m.c2.y * m.c1.z;
    const T c12 = m.c2.x * m.c3.w - m.c3.x * m.c2.w;
    const T c14 = m.c1.x * m.c3.w - m.c3.x * m.c1.w;
    const T c15 = m.c1.x * m.c2.w - m.c2.x * m.c1.w;
    const T c16 = m.c2.x * m.c3.z - m.c3.x * m.c2.z;
    const T c18 = m.c1.x * m.c3.z - m.c3.x * m.c1.z;
    const T c19 = m.c1.x * m.c2.z - m.c2.x * m.c1.z;
    const T c20 = m.c2.x * m.c3.y - m.c3.x * m.c2.y;
    const T c22 = m.c1.x * m.c3.y - m.c3.x * m.c1.y;
    const T c23 = m.c1.x * m.c2.y - m.c2.x * m.c1.y;

    const Vec4<T> i0( m.c1.y * c00 - m.c1.z * c04 + m.c1.w * c08,
                     -(m.c0.y * c00 - m.c0.z * c04 + m.c0.w * c08),
                      m.c0.y * c02 - m.c0.z * c06 + m.c0.w * c10,
                     -(m.c0.y * c03 - m.c0.z * c07 + m.c0.w * c11));
    const Vec4<T> i1(-(m.c1.x * c00 - m.c1.z * c12 + m.c1.w * c16),
                      m.c0.x * c00 - m.c0.z * c12 + m.c0.w * c16,
                     -(m.c0.x * c02 - m.c0.z * c14 + m.c0.w * c18),
                      m.c0.x * c03 - m.c0.z * c15 + m.c0.w * c19);
    const Vec4<T> i2( m.c1.x * c04 - m.c1.y * c12 + m.c1.w * c20,
                     -(m.c0.x * c04 - m.c0.y * c12 + m.c0.w * c20),
                      m.c0.x * c06 - m.c0.y * c14 + m.c0.w * c22,
                     -(m.c0.x * c07 - m.c0.y * c15 + m.c0.w * c23));
    const Vec4<T> i3(-(m.c1.x * c08 - m.c1.y * c16 + m.c1.z * c20),
                      m.c0.x * c08 - m.c0.y * c16 + m.c0.z * c20,
                     -(m.c0.x * c10 - m.c0.y * c18 + m.c0.z * c22),
                      m.c0.x * c11 - m.c0.y * c19 + m.c0.z * c23);

    const T det = m.c0.x * i0.x + m.c0.y * i1.x + m.c0.z * i2.x + m.c0.w * i3.x;
    const T inv_det = static_cast<T>(1) / det;
    return Mat4<T>(i0 * inv_det, i1 * inv_det, i2 * inv_det, i3 * inv_det);
}

using Mat2f = Mat2<crd::f32>;
using Mat3f = Mat3<crd::f32>;
using Mat4f = Mat4<crd::f32>;
using Mat2d = Mat2<crd::f64>;
using Mat3d = Mat3<crd::f64>;
using Mat4d = Mat4<crd::f64>;
} // namespace crd::math
