#pragma once

#include <crd/math/geometry.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/transform.hpp>
#include <crd/math/vec.hpp>

#include <format>

namespace crd::math::detail
{
template <typename T> struct ScalarFormatter
{
    std::formatter<T, char> value;

    constexpr auto parse(std::format_parse_context& ctx) { return value.parse(ctx); }

    template <typename FormatContext> auto format_scalar(const T& v, FormatContext& ctx) const
    {
        return value.format(v, ctx);
    }
};
} // namespace crd::math::detail

namespace std
{
template <crd::math::MathScalar T> struct formatter<crd::math::Vec2<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext> auto format(const crd::math::Vec2<T>& v, FormatContext& ctx) const
    {
        auto out = ctx.out();
        out = std::format_to(out, "Vec2(");
        out = this->format_scalar(v.x, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(v.y, ctx);
        return std::format_to(out, ")");
    }
};

template <crd::math::MathScalar T> struct formatter<crd::math::Vec3<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext> auto format(const crd::math::Vec3<T>& v, FormatContext& ctx) const
    {
        auto out = ctx.out();
        out = std::format_to(out, "Vec3(");
        out = this->format_scalar(v.x, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(v.y, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(v.z, ctx);
        return std::format_to(out, ")");
    }
};

template <crd::math::MathScalar T> struct formatter<crd::math::Vec4<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext> auto format(const crd::math::Vec4<T>& v, FormatContext& ctx) const
    {
        auto out = ctx.out();
        out = std::format_to(out, "Vec4(");
        out = this->format_scalar(v.x, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(v.y, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(v.z, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(v.w, ctx);
        return std::format_to(out, ")");
    }
};

template <crd::math::MathScalar T> struct formatter<crd::math::Quat<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext> auto format(const crd::math::Quat<T>& q, FormatContext& ctx) const
    {
        auto out = ctx.out();
        out = std::format_to(out, "Quat(");
        out = this->format_scalar(q.x, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(q.y, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(q.z, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(q.w, ctx);
        return std::format_to(out, ")");
    }
};

template <crd::math::MathScalar T> struct formatter<crd::math::Mat2<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext> auto format(const crd::math::Mat2<T>& m, FormatContext& ctx) const
    {
        auto out = ctx.out();
        out = std::format_to(out, "Mat2([[");
        out = this->format_scalar(m.c0.x, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c1.x, ctx);
        out = std::format_to(out, "], [");
        out = this->format_scalar(m.c0.y, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c1.y, ctx);
        return std::format_to(out, "]])");
    }
};

template <crd::math::MathScalar T> struct formatter<crd::math::Mat3<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext> auto format(const crd::math::Mat3<T>& m, FormatContext& ctx) const
    {
        auto out = ctx.out();
        out = std::format_to(out, "Mat3([[");
        out = this->format_scalar(m.c0.x, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c1.x, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c2.x, ctx);
        out = std::format_to(out, "], [");
        out = this->format_scalar(m.c0.y, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c1.y, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c2.y, ctx);
        out = std::format_to(out, "], [");
        out = this->format_scalar(m.c0.z, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c1.z, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c2.z, ctx);
        return std::format_to(out, "]])");
    }
};

template <crd::math::MathScalar T> struct formatter<crd::math::Mat4<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext> auto format(const crd::math::Mat4<T>& m, FormatContext& ctx) const
    {
        auto out = ctx.out();
        out = std::format_to(out, "Mat4([[");
        out = this->format_scalar(m.c0.x, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c1.x, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c2.x, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c3.x, ctx);
        out = std::format_to(out, "], [");
        out = this->format_scalar(m.c0.y, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c1.y, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c2.y, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c3.y, ctx);
        out = std::format_to(out, "], [");
        out = this->format_scalar(m.c0.z, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c1.z, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c2.z, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c3.z, ctx);
        out = std::format_to(out, "], [");
        out = this->format_scalar(m.c0.w, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c1.w, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c2.w, ctx);
        out = std::format_to(out, ", ");
        out = this->format_scalar(m.c3.w, ctx);
        return std::format_to(out, "]])");
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::math::Transform<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext> auto format(const crd::math::Transform<T>& t, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Transform(t={}, r={})", t.translation, t.rotation);
    }
};

template <crd::math::MathScalar T> struct formatter<crd::math::Ray<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext> auto format(const crd::math::Ray<T>& ray, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Ray(o={}, d={})", ray.origin, ray.direction);
    }
};

template <crd::math::MathScalar T> struct formatter<crd::math::Plane<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext> auto format(const crd::math::Plane<T>& plane, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Plane(n={}, d={})", plane.normal, plane.d);
    }
};

template <crd::math::MathScalar T> struct formatter<crd::math::Sphere<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext> auto format(const crd::math::Sphere<T>& sphere, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Sphere(c={}, r={})", sphere.center, sphere.radius);
    }
};

template <crd::math::MathScalar T> struct formatter<crd::math::AABB<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext> auto format(const crd::math::AABB<T>& bounds, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "AABB(min={}, max={})", bounds.min, bounds.max);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::math::Triangle<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext> auto format(const crd::math::Triangle<T>& tri, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Triangle(a={}, b={}, c={})", tri.a, tri.b, tri.c);
    }
};

template <crd::math::MathScalar T> struct formatter<crd::math::Frustum<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext> auto format(const crd::math::Frustum<T>& frustum, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Frustum(l={}, r={}, b={}, t={}, n={}, f={})", frustum.planes[0],
                              frustum.planes[1], frustum.planes[2], frustum.planes[3], frustum.planes[4],
                              frustum.planes[5]);
    }
};
} // namespace std
