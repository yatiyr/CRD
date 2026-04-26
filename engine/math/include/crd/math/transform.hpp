#pragma once

#include <crd/math/quat.hpp>

namespace crd::math
{
template <MathScalar T> struct Transform
{
    Vec3<T> translation{};
    Quat<T> rotation = Quat<T>::identity();

    constexpr Transform() noexcept = default;
    constexpr Transform(const Vec3<T>& translation_in, const Quat<T>& rotation_in) noexcept
        : translation(translation_in), rotation(rotation_in)
    {
    }

    [[nodiscard]] static constexpr Transform identity() noexcept
    {
        return Transform(Vec3<T>(static_cast<T>(0)), Quat<T>::identity());
    }
};

template <MathScalar T> [[nodiscard]] inline Vec3<T> transform_vector(const Transform<T>& t, const Vec3<T>& v) noexcept
{
    return rotate_vector(t.rotation, v);
}

template <MathScalar T> [[nodiscard]] inline Vec3<T> transform_point(const Transform<T>& t, const Vec3<T>& p) noexcept
{
    return rotate_vector(t.rotation, p) + t.translation;
}

template <MathScalar T> [[nodiscard]] inline Transform<T> inversed(const Transform<T>& t) noexcept
{
    const Quat<T> inv_rotation = inversed(t.rotation);
    return Transform<T>(rotate_vector(inv_rotation, -t.translation), inv_rotation);
}

template <MathScalar T>
[[nodiscard]] inline Transform<T> operator*(const Transform<T>& lhs, const Transform<T>& rhs) noexcept
{
    return Transform<T>(transform_point(lhs, rhs.translation), lhs.rotation * rhs.rotation);
}

template <MathScalar T> [[nodiscard]] inline Mat4<T> to_mat4(const Transform<T>& t) noexcept
{
    Mat4<T> out = to_mat4(t.rotation);
    out.c3 = Vec4<T>(t.translation, static_cast<T>(1));
    return out;
}

using Transformf = Transform<crd::f32>;
using Transformd = Transform<crd::f64>;
} // namespace crd::math
