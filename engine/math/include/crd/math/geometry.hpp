#pragma once

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4723) // Guarded reciprocal code in intersection helpers is safe; MSVC is conservative here.
#endif

#include <crd/math/mat.hpp>

#include <array>

namespace crd::math
{
template <MathScalar T> struct Ray
{
    Vec3<T> origin{};
    Vec3<T> direction{};

    constexpr Ray() noexcept = default;
    constexpr Ray(const Vec3<T>& origin_in, const Vec3<T>& direction_in) noexcept
        : origin(origin_in), direction(direction_in)
    {
    }
};

template <MathScalar T> struct Plane
{
    Vec3<T> normal{};
    T d = static_cast<T>(0);

    constexpr Plane() noexcept = default;
    constexpr Plane(const Vec3<T>& normal_in, T d_in) noexcept : normal(normal_in), d(d_in) {}
};

template <MathScalar T> struct Sphere
{
    Vec3<T> center{};
    T radius = static_cast<T>(0);

    constexpr Sphere() noexcept = default;
    constexpr Sphere(const Vec3<T>& center_in, T radius_in) noexcept : center(center_in), radius(radius_in) {}
};

template <MathScalar T> struct AABB
{
    Vec3<T> min{};
    Vec3<T> max{};

    constexpr AABB() noexcept = default;
    constexpr AABB(const Vec3<T>& min_in, const Vec3<T>& max_in) noexcept : min(min_in), max(max_in) {}
};

template <MathScalar T> struct Triangle
{
    Vec3<T> a{};
    Vec3<T> b{};
    Vec3<T> c{};

    constexpr Triangle() noexcept = default;
    constexpr Triangle(const Vec3<T>& a_in, const Vec3<T>& b_in, const Vec3<T>& c_in) noexcept
        : a(a_in), b(b_in), c(c_in)
    {
    }
};

template <MathScalar T> struct Frustum
{
    std::array<Plane<T>, 6> planes{};

    constexpr Frustum() noexcept = default;
    constexpr explicit Frustum(const std::array<Plane<T>, 6>& planes_in) noexcept : planes(planes_in) {}
};

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Ray<T>& lhs, const Ray<T>& rhs) noexcept
{
    return lhs.origin == rhs.origin && lhs.direction == rhs.direction;
}

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Plane<T>& lhs, const Plane<T>& rhs) noexcept
{
    return lhs.normal == rhs.normal && lhs.d == rhs.d;
}

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Sphere<T>& lhs, const Sphere<T>& rhs) noexcept
{
    return lhs.center == rhs.center && lhs.radius == rhs.radius;
}

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const AABB<T>& lhs, const AABB<T>& rhs) noexcept
{
    return lhs.min == rhs.min && lhs.max == rhs.max;
}

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Triangle<T>& lhs, const Triangle<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c;
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> point_at(const Ray<T>& ray, T t) noexcept
{
    return ray.origin + ray.direction * t;
}

template <MathScalar T>
[[nodiscard]] inline bool try_normalize(Plane<T>& plane, T epsilon = default_epsilon<T>()) noexcept
{
    const T len = length(plane.normal);
    if (len <= epsilon)
    {
        return false;
    }

    plane.normal /= len;
    plane.d /= len;
    return true;
}

template <MathScalar T> [[nodiscard]] inline Plane<T> normalized(Plane<T> plane) noexcept
{
    const bool ok = try_normalize(plane);
    CRD_ASSERT(ok);
    (void)ok;
    return plane;
}

template <MathScalar T>
[[nodiscard]] inline Plane<T> plane_from_point_normal(const Vec3<T>& point, const Vec3<T>& normal) noexcept
{
    const Vec3<T> unit_normal = normalized(normal);
    return Plane<T>(unit_normal, -dot(unit_normal, point));
}

template <MathScalar T> [[nodiscard]] constexpr T signed_distance(const Plane<T>& plane, const Vec3<T>& point) noexcept
{
    return dot(plane.normal, point) + plane.d;
}

template <MathScalar T> [[nodiscard]] inline Vec3<T> closest_point(const Plane<T>& plane, const Vec3<T>& point) noexcept
{
    return point - plane.normal * signed_distance(plane, point);
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> center(const AABB<T>& bounds) noexcept
{
    return (bounds.min + bounds.max) * static_cast<T>(0.5);
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> extents(const AABB<T>& bounds) noexcept
{
    return (bounds.max - bounds.min) * static_cast<T>(0.5);
}

template <MathScalar T> [[nodiscard]] constexpr bool contains(const AABB<T>& bounds, const Vec3<T>& point) noexcept
{
    return point.x >= bounds.min.x && point.x <= bounds.max.x && point.y >= bounds.min.y && point.y <= bounds.max.y &&
           point.z >= bounds.min.z && point.z <= bounds.max.z;
}

template <MathScalar T>
[[nodiscard]] constexpr Vec3<T> closest_point(const AABB<T>& bounds, const Vec3<T>& point) noexcept
{
    return Vec3<T>(clamp(point.x, bounds.min.x, bounds.max.x), clamp(point.y, bounds.min.y, bounds.max.y),
                   clamp(point.z, bounds.min.z, bounds.max.z));
}

template <MathScalar T> [[nodiscard]] constexpr bool intersects(const AABB<T>& lhs, const AABB<T>& rhs) noexcept
{
    return lhs.min.x <= rhs.max.x && lhs.max.x >= rhs.min.x && lhs.min.y <= rhs.max.y && lhs.max.y >= rhs.min.y &&
           lhs.min.z <= rhs.max.z && lhs.max.z >= rhs.min.z;
}

template <MathScalar T> [[nodiscard]] constexpr bool contains(const Sphere<T>& sphere, const Vec3<T>& point) noexcept
{
    return distance_squared(sphere.center, point) <= sphere.radius * sphere.radius;
}

template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Sphere<T>& lhs, const Sphere<T>& rhs) noexcept
{
    const T r = lhs.radius + rhs.radius;
    return distance_squared(lhs.center, rhs.center) <= r * r;
}

template <MathScalar T> [[nodiscard]] constexpr bool intersects(const AABB<T>& bounds, const Sphere<T>& sphere) noexcept
{
    return distance_squared(closest_point(bounds, sphere.center), sphere.center) <= sphere.radius * sphere.radius;
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> centroid(const Triangle<T>& tri) noexcept
{
    return (tri.a + tri.b + tri.c) / static_cast<T>(3);
}

template <MathScalar T> [[nodiscard]] inline Vec3<T> normal(const Triangle<T>& tri) noexcept
{
    return normalized(cross(tri.b - tri.a, tri.c - tri.a));
}

template <MathScalar T>
[[nodiscard]] constexpr Vec3<T> barycentric(const Triangle<T>& tri, const Vec3<T>& point) noexcept
{
    const Vec3<T> v0 = tri.b - tri.a;
    const Vec3<T> v1 = tri.c - tri.a;
    const Vec3<T> v2 = point - tri.a;
    const T d00 = dot(v0, v0);
    const T d01 = dot(v0, v1);
    const T d11 = dot(v1, v1);
    const T d20 = dot(v2, v0);
    const T d21 = dot(v2, v1);
    const T denom = d00 * d11 - d01 * d01;
    CRD_ASSERT(!approx_zero(denom));
    const T v = (d11 * d20 - d01 * d21) / denom;
    const T w = (d00 * d21 - d01 * d20) / denom;
    const T u = static_cast<T>(1) - v - w;
    return Vec3<T>(u, v, w);
}

template <MathScalar T>
[[nodiscard]] constexpr bool contains(const Triangle<T>& tri, const Vec3<T>& point,
                                      T epsilon = default_epsilon<T>()) noexcept
{
    const Vec3<T> bc = barycentric(tri, point);
    return bc.x >= -epsilon && bc.y >= -epsilon && bc.z >= -epsilon;
}

template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_plane(const Ray<T>& ray, const Plane<T>& plane, T& out_t,
                                              T epsilon = default_epsilon<T>()) noexcept
{
    const T denom = dot(plane.normal, ray.direction);
    if (approx_zero(denom, epsilon))
    {
        return false;
    }

    const T t = -(dot(plane.normal, ray.origin) + plane.d) / denom;
    if (t < static_cast<T>(0))
    {
        return false;
    }

    out_t = t;
    return true;
}

template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_sphere(const Ray<T>& ray, const Sphere<T>& sphere, T& out_t,
                                               T epsilon = default_epsilon<T>()) noexcept
{
    const Vec3<T> oc = ray.origin - sphere.center;
    const T a = dot(ray.direction, ray.direction);
    if (approx_zero(a, epsilon) || a == static_cast<T>(0))
    {
        return false;
    }
    const T b = static_cast<T>(2) * dot(oc, ray.direction);
    const T c = dot(oc, oc) - sphere.radius * sphere.radius;
    const T discriminant = b * b - static_cast<T>(4) * a * c;
    if (discriminant < static_cast<T>(0))
    {
        return false;
    }

    const T sqrt_disc = static_cast<T>(std::sqrt(discriminant));
    const T inv_2a = static_cast<T>(0.5) / a;
    const T t0 = (-b - sqrt_disc) * inv_2a;
    const T t1 = (-b + sqrt_disc) * inv_2a;

    if (t0 >= epsilon)
    {
        out_t = t0;
        return true;
    }
    if (t1 >= epsilon)
    {
        out_t = t1;
        return true;
    }
    return false;
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4723) // MSVC cannot prove the guarded determinant path stays non-zero.
#endif
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_triangle(const Ray<T>& ray, const Triangle<T>& tri, T& out_t,
                                                 Vec3<T>& out_barycentric, T epsilon = default_epsilon<T>()) noexcept
{
    const Vec3<T> edge1 = tri.b - tri.a;
    const Vec3<T> edge2 = tri.c - tri.a;
    const Vec3<T> p = cross(ray.direction, edge2);
    const T det = dot(edge1, p);
    if (approx_zero(det, epsilon) || det == static_cast<T>(0))
    {
        return false;
    }

    const T det_floor = max(epsilon, std::numeric_limits<T>::epsilon());
    const T safe_det =
        det < static_cast<T>(0) ? (det < -det_floor ? det : -det_floor) : (det > det_floor ? det : det_floor);
    const T inv_det = static_cast<T>(1) / safe_det;
    const Vec3<T> s = ray.origin - tri.a;
    const T v = dot(s, p) * inv_det;
    if (v < static_cast<T>(0) || v > static_cast<T>(1))
    {
        return false;
    }

    const Vec3<T> q = cross(s, edge1);
    const T w = dot(ray.direction, q) * inv_det;
    if (w < static_cast<T>(0) || (v + w) > static_cast<T>(1))
    {
        return false;
    }

    const T t = dot(edge2, q) * inv_det;
    if (t < epsilon)
    {
        return false;
    }

    out_t = t;
    out_barycentric = Vec3<T>(static_cast<T>(1) - v - w, v, w);
    return true;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

template <MathScalar T>
[[nodiscard]] constexpr Vec3<T> positive_vertex(const AABB<T>& bounds, const Vec3<T>& normal) noexcept
{
    return Vec3<T>(normal.x >= static_cast<T>(0) ? bounds.max.x : bounds.min.x,
                   normal.y >= static_cast<T>(0) ? bounds.max.y : bounds.min.y,
                   normal.z >= static_cast<T>(0) ? bounds.max.z : bounds.min.z);
}

template <MathScalar T> [[nodiscard]] inline Frustum<T> frustum_from_view_projection(const Mat4<T>& m) noexcept
{
    const Plane<T> left(Vec3<T>(m.c3.x + m.c0.x, m.c3.y + m.c0.y, m.c3.z + m.c0.z), m.c3.w + m.c0.w);
    const Plane<T> right(Vec3<T>(m.c3.x - m.c0.x, m.c3.y - m.c0.y, m.c3.z - m.c0.z), m.c3.w - m.c0.w);
    const Plane<T> bottom(Vec3<T>(m.c3.x + m.c1.x, m.c3.y + m.c1.y, m.c3.z + m.c1.z), m.c3.w + m.c1.w);
    const Plane<T> top(Vec3<T>(m.c3.x - m.c1.x, m.c3.y - m.c1.y, m.c3.z - m.c1.z), m.c3.w - m.c1.w);
    const Plane<T> near_plane(Vec3<T>(m.c3.x + m.c2.x, m.c3.y + m.c2.y, m.c3.z + m.c2.z), m.c3.w + m.c2.w);
    const Plane<T> far_plane(Vec3<T>(m.c3.x - m.c2.x, m.c3.y - m.c2.y, m.c3.z - m.c2.z), m.c3.w - m.c2.w);

    return Frustum<T>{std::array<Plane<T>, 6>{normalized(left), normalized(right), normalized(bottom), normalized(top),
                                              normalized(near_plane), normalized(far_plane)}};
}

template <MathScalar T> [[nodiscard]] inline bool contains(const Frustum<T>& frustum, const Vec3<T>& point) noexcept
{
    for (const Plane<T>& plane : frustum.planes)
    {
        if (signed_distance(plane, point) < static_cast<T>(0))
        {
            return false;
        }
    }
    return true;
}

template <MathScalar T>
[[nodiscard]] inline bool intersects(const Frustum<T>& frustum, const Sphere<T>& sphere) noexcept
{
    for (const Plane<T>& plane : frustum.planes)
    {
        if (signed_distance(plane, sphere.center) < -sphere.radius)
        {
            return false;
        }
    }
    return true;
}

template <MathScalar T> [[nodiscard]] inline bool intersects(const Frustum<T>& frustum, const AABB<T>& bounds) noexcept
{
    for (const Plane<T>& plane : frustum.planes)
    {
        if (signed_distance(plane, positive_vertex(bounds, plane.normal)) < static_cast<T>(0))
        {
            return false;
        }
    }
    return true;
}

using Rayf = Ray<crd::f32>;
using Rayd = Ray<crd::f64>;
using Planef = Plane<crd::f32>;
using Planed = Plane<crd::f64>;
using Spheref = Sphere<crd::f32>;
using Sphered = Sphere<crd::f64>;
using AABBf = AABB<crd::f32>;
using AABBd = AABB<crd::f64>;
using Trianglef = Triangle<crd::f32>;
using Triangled = Triangle<crd::f64>;
using Frustumf = Frustum<crd::f32>;
using Frustumd = Frustum<crd::f64>;
} // namespace crd::math

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
