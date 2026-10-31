// Mat2/Mat3/Mat4 value types and their scalar (template) operations. Include <crd/math/mat.hpp>, which adds the
// Mat4<f32> SIMD operator overloads (mat_simd_f32.hpp); this split exists so that header can depend on these
// types without the mat.hpp <-> mat_simd_f32.hpp include cycle the strict analysis rejects.
#pragma once

#include <crd/math/vec.hpp>

#include <cmath>

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

// The closed-form small-matrix routines, crd-math's alone (cmake/capability-owners.json,
// math.fixed-size-linear-algebra). determinant is the cofactor expansion along the first column, in the order written,
// and the inverses and the inverse-transpose divide by that value, so a caller that tests the determinant before
// inverting tests the value the inverse uses. The 3x3 solve takes each of Cramer's determinants as determinant() of the
// transpose (the expansion along the first row, in which the four share their minors), so its determinant can differ
// from determinant(m)'s in the last bit; try_solve gates on the value it divides by. The inverses and solves need a
// regular matrix (a zero determinant gives infinities and NaNs); try_inverse and try_solve refuse a small one.

template <MathScalar T> [[nodiscard]] constexpr T determinant(const Mat2<T>& m) noexcept
{
    return m.c0.x * m.c1.y - m.c0.y * m.c1.x;
}

template <MathScalar T> [[nodiscard]] constexpr T determinant(const Mat3<T>& m) noexcept
{
    return m.c0.x * (m.c1.y * m.c2.z - m.c2.y * m.c1.z) - m.c0.y * (m.c1.x * m.c2.z - m.c2.x * m.c1.z) +
           m.c0.z * (m.c1.x * m.c2.y - m.c2.x * m.c1.y);
}

namespace detail
{
// The cofactor matrix of m, column-major like m: column j holds the signed minors of column j's entries.
template <MathScalar T> [[nodiscard]] constexpr Mat3<T> cofactors(const Mat3<T>& m) noexcept
{
    return Mat3<T>(Vec3<T>(m.c1.y * m.c2.z - m.c2.y * m.c1.z, -(m.c1.x * m.c2.z - m.c2.x * m.c1.z),
                           m.c1.x * m.c2.y - m.c2.x * m.c1.y),
                   Vec3<T>(-(m.c0.y * m.c2.z - m.c2.y * m.c0.z), m.c0.x * m.c2.z - m.c2.x * m.c0.z,
                           -(m.c0.x * m.c2.y - m.c2.x * m.c0.y)),
                   Vec3<T>(m.c0.y * m.c1.z - m.c1.y * m.c0.z, -(m.c0.x * m.c1.z - m.c1.x * m.c0.z),
                           m.c0.x * m.c1.y - m.c1.x * m.c0.y));
}

// determinant(m) from m's cofactor matrix: the first column's entries times their cofactors, the same bits.
template <MathScalar T> [[nodiscard]] constexpr T determinant_of(const Mat3<T>& m, const Mat3<T>& cof) noexcept
{
    return m.c0.x * cof.c0.x + m.c0.y * cof.c0.y + m.c0.z * cof.c0.z;
}

// The inverse from the cofactor matrix and the determinant: the adjugate (the transposed cofactors) over det.
template <MathScalar T> [[nodiscard]] constexpr Mat3<T> adjugate_over(const Mat3<T>& cof, T det) noexcept
{
    const Mat3<T> adjugate = transpose(cof);
    const T inv_det = static_cast<T>(1) / det;
    return Mat3<T>(adjugate.c0 * inv_det, adjugate.c1 * inv_det, adjugate.c2 * inv_det);
}

// Cramer's rule for m x = b: the numerators (entry i the determinant of m with column i replaced by b) and m's.
template <typename V, MathScalar T> struct Cramer
{
    V numerators;
    T det;
};

template <MathScalar T> [[nodiscard]] constexpr Cramer<Vec2<T>, T> cramer(const Mat2<T>& m, const Vec2<T>& b) noexcept
{
    return {Vec2<T>(b.x * m.c1.y - b.y * m.c1.x, m.c0.x * b.y - m.c0.y * b.x), determinant(m)};
}

// Each of the four determinants is determinant() of its transpose, the expansion along the first row, so all four share
// the six 2x2 minors of rows 1 and 2 of [m | b]: 27 multiplications, where the adjugate's numerators take 33.
template <MathScalar T> [[nodiscard]] constexpr Cramer<Vec3<T>, T> cramer(const Mat3<T>& m, const Vec3<T>& b) noexcept
{
    const T m12 = m.c1.y * m.c2.z - m.c2.y * m.c1.z; // rows 1 and 2 of columns 1 and 2
    const T m02 = m.c0.y * m.c2.z - m.c2.y * m.c0.z;
    const T m01 = m.c0.y * m.c1.z - m.c1.y * m.c0.z;
    const T b2 = b.y * m.c2.z - m.c2.y * b.z; // of b and column 2
    const T b1 = b.y * m.c1.z - m.c1.y * b.z;
    const T b0 = m.c0.y * b.z - b.y * m.c0.z; // of column 0 and b
    return {Vec3<T>(b.x * m12 - m.c1.x * b2 + m.c2.x * b1, m.c0.x * b2 - b.x * m02 + m.c2.x * b0,
                    -(m.c0.x * b1) - m.c1.x * b0 + b.x * m01),
            m.c0.x * m12 - m.c1.x * m02 + m.c2.x * m01};
}
} // namespace detail

template <MathScalar T> [[nodiscard]] constexpr Mat2<T> inverse(const Mat2<T>& m) noexcept
{
    const T inv_det = static_cast<T>(1) / determinant(m);
    return Mat2<T>(Vec2<T>(m.c1.y * inv_det, -m.c0.y * inv_det), Vec2<T>(-m.c1.x * inv_det, m.c0.x * inv_det));
}

template <MathScalar T> [[nodiscard]] constexpr Mat3<T> inverse(const Mat3<T>& m) noexcept
{
    const Mat3<T> cof = detail::cofactors(m);
    return detail::adjugate_over(cof, detail::determinant_of(m, cof));
}

// The inverse when |determinant(m)| >= min_abs_det (a NaN determinant passes, and gives NaNs); else false, `out`
// untouched.
template <MathScalar T> [[nodiscard]] constexpr bool try_inverse(const Mat3<T>& m, Mat3<T>& out, T min_abs_det) noexcept
{
    const Mat3<T> cof = detail::cofactors(m);
    const T det = detail::determinant_of(m, cof);
    if (abs(det) < min_abs_det)
    {
        return false;
    }
    out = detail::adjugate_over(cof, det);
    return true;
}

// transpose(inverse(m)), the map of normals under m: the cofactor matrix over the determinant.
template <MathScalar T> [[nodiscard]] constexpr Mat3<T> inverse_transpose(const Mat3<T>& m) noexcept
{
    const Mat3<T> cof = detail::cofactors(m);
    const T inv_det = static_cast<T>(1) / detail::determinant_of(m, cof);
    return Mat3<T>(cof.c0 * inv_det, cof.c1 * inv_det, cof.c2 * inv_det);
}

// The x with m x = b, by Cramer's rule: x_i is the determinant of m with column i replaced by b, over m's.
template <MathScalar T> [[nodiscard]] constexpr Vec2<T> solve(const Mat2<T>& m, const Vec2<T>& b) noexcept
{
    const auto c = detail::cramer(m, b);
    return c.numerators * (static_cast<T>(1) / c.det);
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> solve(const Mat3<T>& m, const Vec3<T>& b) noexcept
{
    const auto c = detail::cramer(m, b);
    return c.numerators * (static_cast<T>(1) / c.det);
}

// solve(m, b) when the |determinant| it divides by is >= min_abs_det (a NaN passes, as in try_inverse); else false,
// `out` untouched.
template <MathScalar T>
[[nodiscard]] constexpr bool try_solve(const Mat2<T>& m, const Vec2<T>& b, Vec2<T>& out, T min_abs_det) noexcept
{
    const auto c = detail::cramer(m, b);
    if (abs(c.det) < min_abs_det)
    {
        return false;
    }
    out = c.numerators * (static_cast<T>(1) / c.det);
    return true;
}

template <MathScalar T>
[[nodiscard]] constexpr bool try_solve(const Mat3<T>& m, const Vec3<T>& b, Vec3<T>& out, T min_abs_det) noexcept
{
    const auto c = detail::cramer(m, b);
    if (abs(c.det) < min_abs_det)
    {
        return false;
    }
    out = c.numerators * (static_cast<T>(1) / c.det);
    return true;
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

// Right-handed view matrix from eye position, target, and up hint.
template <MathScalar T>
[[nodiscard]] inline Mat4<T> look_at(const Vec3<T>& eye, const Vec3<T>& target, const Vec3<T>& up_hint) noexcept
{
    const Vec3<T> f = normalized(target - eye);
    const Vec3<T> r = normalized(cross(f, up_hint));
    const Vec3<T> u = cross(r, f);
    Mat4<T> view    = Mat4<T>::identity();
    view.c0.x =  r.x; view.c1.x =  r.y; view.c2.x =  r.z;
    view.c0.y =  u.x; view.c1.y =  u.y; view.c2.y =  u.z;
    view.c0.z = -f.x; view.c1.z = -f.y; view.c2.z = -f.z;
    view.c3.x = -dot(r, eye);
    view.c3.y = -dot(u, eye);
    view.c3.z =  dot(f, eye);
    return view;
}

// Infinite-far reverse-Z perspective with Vulkan clip-space Y flip.
// z_near maps to NDC z=1; infinity maps to NDC z=0.
template <MathScalar T>
[[nodiscard]] inline Mat4<T> perspective_reverse_z(T fov_y_rad, T aspect, T z_near) noexcept
{
    const T tan_half = std::tan(fov_y_rad * static_cast<T>(0.5));
    Mat4<T> p{};
    p.c0.x = static_cast<T>(1) / (aspect * tan_half);
    p.c1.y = -static_cast<T>(1) / tan_half;
    p.c2.w = -static_cast<T>(1);
    p.c3.z = z_near;
    return p;
}

using Mat2f = Mat2<crd::f32>;
using Mat3f = Mat3<crd::f32>;
using Mat4f = Mat4<crd::f32>;
using Mat2d = Mat2<crd::f64>;
using Mat3d = Mat3<crd::f64>;
using Mat4d = Mat4<crd::f64>;
} // namespace crd::math
