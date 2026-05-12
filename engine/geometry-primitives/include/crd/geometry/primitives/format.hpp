#pragma once

// std::format support for the crd-geometry-primitives shape types. Migrated
// from crd/math/format.hpp (Phase 3.1.7 v0a, ADR-0076 §13) — the geometry
// types left crd-math, so their formatters left with them. Reuses crd-math's
// `detail::ScalarFormatter<T>` base (precision/alternate-form parsing) and the
// `Vec3<T>` formatter (so `{}` on member vectors round-trips).

#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/format.hpp>

#include <format>

namespace std
{

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Ray3<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Ray3<T>& ray, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Ray3(o={}, d={})", ray.origin, ray.direction);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Line3<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Line3<T>& line, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Line3(p={}, d={})", line.point, line.direction);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Segment3<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Segment3<T>& seg, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Segment3(a={}, b={})", seg.a, seg.b);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Plane<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Plane<T>& plane, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Plane(n={}, d={})", plane.normal, plane.d);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Sphere<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Sphere<T>& sphere, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Sphere(c={}, r={})", sphere.center, sphere.radius);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::AABB3<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext>
    auto format(const crd::geometry::primitives::AABB3<T>& bounds, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "AABB3(min={}, max={})", bounds.min, bounds.max);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::OBB3<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::OBB3<T>& obb, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "OBB3(c={}, h={}, R={})", obb.center, obb.half_extents, obb.orientation);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Capsule3<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Capsule3<T>& cap, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Capsule3(a={}, b={}, r={})", cap.a, cap.b, cap.radius);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Cylinder3<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Cylinder3<T>& cyl, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Cylinder3(a={}, b={}, r={})", cyl.a, cyl.b, cyl.radius);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Triangle3<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Triangle3<T>& tri, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Triangle3(a={}, b={}, c={})", tri.a, tri.b, tri.c);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Tetrahedron<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Tetrahedron<T>& tet, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Tetrahedron(a={}, b={}, c={}, d={})", tet.a, tet.b, tet.c, tet.d);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Frustum<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Frustum<T>& frustum, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Frustum(l={}, r={}, b={}, t={}, n={}, f={})", frustum.planes[0],
                              frustum.planes[1], frustum.planes[2], frustum.planes[3], frustum.planes[4],
                              frustum.planes[5]);
    }
};

// ---- 2D peers (v0b) --------------------------------------------------------

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Line2<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Line2<T>& line, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Line2(p={}, d={})", line.point, line.direction);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Segment2<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Segment2<T>& seg, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Segment2(a={}, b={})", seg.a, seg.b);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Ray2<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Ray2<T>& ray, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Ray2(o={}, d={})", ray.origin, ray.direction);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::AABB2<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext>
    auto format(const crd::geometry::primitives::AABB2<T>& bounds, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "AABB2(min={}, max={})", bounds.min, bounds.max);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::OBB2<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::OBB2<T>& obb, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "OBB2(c={}, h={}, R={})", obb.center, obb.half_extents, obb.orientation);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Circle<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Circle<T>& circle, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Circle(c={}, r={})", circle.center, circle.radius);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Capsule2<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Capsule2<T>& cap, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Capsule2(a={}, b={}, r={})", cap.a, cap.b, cap.radius);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Cylinder2<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Cylinder2<T>& cyl, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Cylinder2(a={}, b={}, r={})", cyl.a, cyl.b, cyl.radius);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Triangle2<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Triangle2<T>& tri, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Triangle2(a={}, b={}, c={})", tri.a, tri.b, tri.c);
    }
};

} // namespace std
