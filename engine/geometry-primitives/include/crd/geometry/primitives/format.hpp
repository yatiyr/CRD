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
struct formatter<crd::geometry::primitives::Ray<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Ray<T>& ray, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Ray(o={}, d={})", ray.origin, ray.direction);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Line<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Line<T>& line, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Line(p={}, d={})", line.point, line.direction);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Segment<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Segment<T>& seg, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Segment(a={}, b={})", seg.a, seg.b);
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
struct formatter<crd::geometry::primitives::AABB<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    template <typename FormatContext>
    auto format(const crd::geometry::primitives::AABB<T>& bounds, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "AABB(min={}, max={})", bounds.min, bounds.max);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::OBB<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::OBB<T>& obb, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "OBB(c={}, h={}, R={})", obb.center, obb.half_extents, obb.orientation);
    }
};

template <crd::math::MathScalar T>
struct formatter<crd::geometry::primitives::Capsule<T>, char> : crd::math::detail::ScalarFormatter<T>
{
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const crd::geometry::primitives::Capsule<T>& cap, FormatContext& ctx) const
    {
        return std::format_to(ctx.out(), "Capsule(a={}, b={}, r={})", cap.a, cap.b, cap.radius);
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

} // namespace std
