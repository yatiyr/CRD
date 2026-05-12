#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — primitive shape types + closest-point /
// intersection / containment helpers.
//
// Phase 3.1.7 v0a (ADR-0076 §13): this header parks the v0 type catalogue plus
// the helpers migrated wholesale from the (now-deleted) `crd/math/geometry.hpp`.
// As the catalogue grows, v0b–v0f split it into per-algorithm headers
// (closest-point, intersection, barycentric, the iq formulary, the cutting-edge
// branchless/SIMD corpus). `crd-math` thereafter ships only Vec/Mat/Quat/
// Transform/SIMD/`deterministic` — the lean leaf substrate it was meant to be.
//
// Naming rule (pin — read before adding a type): primitives here are 3D and
// templated on the scalar `T` (`crd::math::MathScalar`). They carry no
// dimension suffix UNLESS a 2D peer is planned — `Triangle3` carries the `3`
// because `crd-geometry-polygon` (v6) will add `Triangle2`; `Ray`/`Line`/
// `Segment`/`Plane`/`AABB`/`OBB`/`Sphere`/`Capsule`/`Frustum` have no 2D peer
// on the roadmap, so no suffix. If a 2D `Ray`/`AABB`/etc. ever lands, revisit
// the whole set holistically rather than spot-renaming.
//
// API layers (ADR-0076 §5): this is the typed C++ "Eigen-class" layer — zero-
// overhead inlined templates, data-oriented (`ConstSpan` of vertex/index data,
// never `Mesh*`). The opt-in cooker/editor handle-based façade is reserved for
// later sub-slices; nothing here forbids it.
// ---------------------------------------------------------------------------

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable                                                                                                \
                : 4723) // Guarded reciprocal code in the intersection helpers stays non-zero; MSVC is conservative.
#endif

#include <crd/containers/static_array.hpp>
#include <crd/math/mat.hpp>

#include <cmath>
#include <limits>

namespace crd::geometry::primitives
{
using crd::math::MathScalar;
using crd::math::Vec3;

// A point in 3D space — just `Vec3<T>`. Aliased for call-site readability where
// a parameter is conceptually a position rather than a free vector.
template <MathScalar T> using Point3 = Vec3<T>;

// ---- Linear primitives: Line / Segment / Ray ------------------------------

// Infinite line through `point` along `direction` (direction need not be unit).
template <MathScalar T> struct Line
{
    Vec3<T> point{};
    Vec3<T> direction{};

    constexpr Line() noexcept = default;
    constexpr Line(const Vec3<T>& point_in, const Vec3<T>& direction_in) noexcept
        : point(point_in), direction(direction_in)
    {
    }
};

// Finite segment from `a` to `b`.
template <MathScalar T> struct Segment
{
    Vec3<T> a{};
    Vec3<T> b{};

    constexpr Segment() noexcept = default;
    constexpr Segment(const Vec3<T>& a_in, const Vec3<T>& b_in) noexcept : a(a_in), b(b_in) {}
};

// Ray from `origin` along `direction` (parameter t >= 0; direction need not be unit).
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

// ---- Plane: normal·x + d = 0 ----------------------------------------------

template <MathScalar T> struct Plane
{
    Vec3<T> normal{};
    T d = static_cast<T>(0);

    constexpr Plane() noexcept = default;
    constexpr Plane(const Vec3<T>& normal_in, T d_in) noexcept : normal(normal_in), d(d_in) {}
};

// ---- Sphere ----------------------------------------------------------------

template <MathScalar T> struct Sphere
{
    Vec3<T> center{};
    T radius = static_cast<T>(0);

    constexpr Sphere() noexcept = default;
    constexpr Sphere(const Vec3<T>& center_in, T radius_in) noexcept : center(center_in), radius(radius_in) {}
};

// ---- Axis-aligned bounding box --------------------------------------------

template <MathScalar T> struct AABB
{
    Vec3<T> min{};
    Vec3<T> max{};

    constexpr AABB() noexcept = default;
    constexpr AABB(const Vec3<T>& min_in, const Vec3<T>& max_in) noexcept : min(min_in), max(max_in) {}
};

// ---- Oriented bounding box ------------------------------------------------
//
// Center + half-extents in a local frame whose axes are the columns of
// `orientation` (orthonormal). World point = center + orientation * (local
// coordinate in [-half_extents, +half_extents]). Algorithms (SAT, closest-point)
// land in v0b/v0c — v0a defines the type.
template <MathScalar T> struct OBB
{
    Vec3<T> center{};
    Vec3<T> half_extents{};
    crd::math::Mat3<T> orientation = crd::math::Mat3<T>::identity();

    OBB() noexcept = default;
    OBB(const Vec3<T>& center_in, const Vec3<T>& half_extents_in, const crd::math::Mat3<T>& orientation_in) noexcept
        : center(center_in), half_extents(half_extents_in), orientation(orientation_in)
    {
    }
};

// ---- Capsule: segment a→b with a swept radius -----------------------------

template <MathScalar T> struct Capsule
{
    Vec3<T> a{};
    Vec3<T> b{};
    T radius = static_cast<T>(0);

    constexpr Capsule() noexcept = default;
    constexpr Capsule(const Vec3<T>& a_in, const Vec3<T>& b_in, T radius_in) noexcept
        : a(a_in), b(b_in), radius(radius_in)
    {
    }
};

// ---- Triangle (3D) --------------------------------------------------------

template <MathScalar T> struct Triangle3
{
    Vec3<T> a{};
    Vec3<T> b{};
    Vec3<T> c{};

    constexpr Triangle3() noexcept = default;
    constexpr Triangle3(const Vec3<T>& a_in, const Vec3<T>& b_in, const Vec3<T>& c_in) noexcept
        : a(a_in), b(b_in), c(c_in)
    {
    }
};

// ---- Frustum: 6 inward-facing planes (L, R, B, T, near, far) --------------

template <MathScalar T> struct Frustum
{
    crd::containers::StaticArray<Plane<T>, 6> planes{};

    constexpr Frustum() noexcept = default;
    constexpr explicit Frustum(const crd::containers::StaticArray<Plane<T>, 6>& planes_in) noexcept : planes(planes_in)
    {
    }
};

// ---- Equality --------------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Line<T>& lhs, const Line<T>& rhs) noexcept
{
    return lhs.point == rhs.point && lhs.direction == rhs.direction;
}
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Segment<T>& lhs, const Segment<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b;
}
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
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const OBB<T>& lhs, const OBB<T>& rhs) noexcept
{
    return lhs.center == rhs.center && lhs.half_extents == rhs.half_extents && lhs.orientation == rhs.orientation;
}
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Capsule<T>& lhs, const Capsule<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.radius == rhs.radius;
}
template <MathScalar T>
[[nodiscard]] constexpr bool operator==(const Triangle3<T>& lhs, const Triangle3<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c;
}

// ---- Ray helpers -----------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> point_at(const Ray<T>& ray, T t) noexcept
{
    return ray.origin + ray.direction * t;
}

// ---- Plane helpers ---------------------------------------------------------

template <MathScalar T>
[[nodiscard]] inline bool try_normalize(Plane<T>& plane, T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const T len = crd::math::length(plane.normal);
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
    const Vec3<T> unit_normal = crd::math::normalized(normal);
    return Plane<T>(unit_normal, -crd::math::dot(unit_normal, point));
}

template <MathScalar T> [[nodiscard]] constexpr T signed_distance(const Plane<T>& plane, const Vec3<T>& point) noexcept
{
    return crd::math::dot(plane.normal, point) + plane.d;
}

template <MathScalar T> [[nodiscard]] inline Vec3<T> closest_point(const Plane<T>& plane, const Vec3<T>& point) noexcept
{
    return point - plane.normal * signed_distance(plane, point);
}

// ---- AABB helpers ----------------------------------------------------------

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
    return Vec3<T>(crd::math::clamp(point.x, bounds.min.x, bounds.max.x),
                   crd::math::clamp(point.y, bounds.min.y, bounds.max.y),
                   crd::math::clamp(point.z, bounds.min.z, bounds.max.z));
}

template <MathScalar T> [[nodiscard]] constexpr bool intersects(const AABB<T>& lhs, const AABB<T>& rhs) noexcept
{
    return lhs.min.x <= rhs.max.x && lhs.max.x >= rhs.min.x && lhs.min.y <= rhs.max.y && lhs.max.y >= rhs.min.y &&
           lhs.min.z <= rhs.max.z && lhs.max.z >= rhs.min.z;
}

// The "positive vertex" of `bounds` w.r.t. a direction — the corner furthest
// along `normal`. Used by plane/frustum-vs-AABB.
template <MathScalar T>
[[nodiscard]] constexpr Vec3<T> positive_vertex(const AABB<T>& bounds, const Vec3<T>& normal) noexcept
{
    return Vec3<T>(normal.x >= static_cast<T>(0) ? bounds.max.x : bounds.min.x,
                   normal.y >= static_cast<T>(0) ? bounds.max.y : bounds.min.y,
                   normal.z >= static_cast<T>(0) ? bounds.max.z : bounds.min.z);
}

// ---- Sphere helpers --------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr bool contains(const Sphere<T>& sphere, const Vec3<T>& point) noexcept
{
    return crd::math::distance_squared(sphere.center, point) <= sphere.radius * sphere.radius;
}

template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Sphere<T>& lhs, const Sphere<T>& rhs) noexcept
{
    const T r = lhs.radius + rhs.radius;
    return crd::math::distance_squared(lhs.center, rhs.center) <= r * r;
}

template <MathScalar T> [[nodiscard]] constexpr bool intersects(const AABB<T>& bounds, const Sphere<T>& sphere) noexcept
{
    return crd::math::distance_squared(closest_point(bounds, sphere.center), sphere.center) <=
           sphere.radius * sphere.radius;
}

// ---- Triangle helpers ------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> centroid(const Triangle3<T>& tri) noexcept
{
    return (tri.a + tri.b + tri.c) / static_cast<T>(3);
}

template <MathScalar T> [[nodiscard]] inline Vec3<T> normal(const Triangle3<T>& tri) noexcept
{
    return crd::math::normalized(crd::math::cross(tri.b - tri.a, tri.c - tri.a));
}

template <MathScalar T>
[[nodiscard]] constexpr Vec3<T> barycentric(const Triangle3<T>& tri, const Vec3<T>& point) noexcept
{
    const Vec3<T> v0 = tri.b - tri.a;
    const Vec3<T> v1 = tri.c - tri.a;
    const Vec3<T> v2 = point - tri.a;
    const T d00 = crd::math::dot(v0, v0);
    const T d01 = crd::math::dot(v0, v1);
    const T d11 = crd::math::dot(v1, v1);
    const T d20 = crd::math::dot(v2, v0);
    const T d21 = crd::math::dot(v2, v1);
    const T denom = d00 * d11 - d01 * d01;
    CRD_ASSERT(!crd::math::approx_zero(denom));
    const T v = (d11 * d20 - d01 * d21) / denom;
    const T w = (d00 * d21 - d01 * d20) / denom;
    const T u = static_cast<T>(1) - v - w;
    return Vec3<T>(u, v, w);
}

template <MathScalar T>
[[nodiscard]] constexpr bool contains(const Triangle3<T>& tri, const Vec3<T>& point,
                                      T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const Vec3<T> bc = barycentric(tri, point);
    return bc.x >= -epsilon && bc.y >= -epsilon && bc.z >= -epsilon;
}

// ---- Ray vs primitive ------------------------------------------------------

template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_plane(const Ray<T>& ray, const Plane<T>& plane, T& out_t,
                                              T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const T denom = crd::math::dot(plane.normal, ray.direction);
    if (crd::math::approx_zero(denom, epsilon))
    {
        return false;
    }
    const T t = -(crd::math::dot(plane.normal, ray.origin) + plane.d) / denom;
    if (t < static_cast<T>(0))
    {
        return false;
    }
    out_t = t;
    return true;
}

template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_sphere(const Ray<T>& ray, const Sphere<T>& sphere, T& out_t,
                                               T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const Vec3<T> oc = ray.origin - sphere.center;
    const T a = crd::math::dot(ray.direction, ray.direction);
    if (crd::math::approx_zero(a, epsilon) || a == static_cast<T>(0))
    {
        return false;
    }
    const T b = static_cast<T>(2) * crd::math::dot(oc, ray.direction);
    const T c = crd::math::dot(oc, oc) - sphere.radius * sphere.radius;
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

// Möller-Trumbore (1997). v0f adds the watertight (Woop 2013) and Baldwin-Weber
// (2016) variants + the SIMD batch kernels; this scalar form becomes the v0f
// cross-check reference on the non-degenerate corpus.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4723) // MSVC cannot prove the guarded determinant path stays non-zero.
#endif
template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_triangle(const Ray<T>& ray, const Triangle3<T>& tri, T& out_t,
                                                 Vec3<T>& out_barycentric,
                                                 T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const Vec3<T> edge1 = tri.b - tri.a;
    const Vec3<T> edge2 = tri.c - tri.a;
    const Vec3<T> p = crd::math::cross(ray.direction, edge2);
    const T det = crd::math::dot(edge1, p);
    if (crd::math::approx_zero(det, epsilon) || det == static_cast<T>(0))
    {
        return false;
    }
    const T det_floor = crd::math::max(epsilon, std::numeric_limits<T>::epsilon());
    const T safe_det =
        det < static_cast<T>(0) ? (det < -det_floor ? det : -det_floor) : (det > det_floor ? det : det_floor);
    const T inv_det = static_cast<T>(1) / safe_det;
    const Vec3<T> s = ray.origin - tri.a;
    const T v = crd::math::dot(s, p) * inv_det;
    if (v < static_cast<T>(0) || v > static_cast<T>(1))
    {
        return false;
    }
    const Vec3<T> q = crd::math::cross(s, edge1);
    const T w = crd::math::dot(ray.direction, q) * inv_det;
    if (w < static_cast<T>(0) || (v + w) > static_cast<T>(1))
    {
        return false;
    }
    const T t = crd::math::dot(edge2, q) * inv_det;
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

// ---- Frustum helpers -------------------------------------------------------

// Extract the 6 (normalised, inward-facing) clip planes from a view-projection
// matrix. Column-major `Mat4` per crd-math; planes ordered L, R, B, T, near, far.
template <MathScalar T>
[[nodiscard]] inline Frustum<T> frustum_from_view_projection(const crd::math::Mat4<T>& m) noexcept
{
    const Plane<T> left(Vec3<T>(m.c3.x + m.c0.x, m.c3.y + m.c0.y, m.c3.z + m.c0.z), m.c3.w + m.c0.w);
    const Plane<T> right(Vec3<T>(m.c3.x - m.c0.x, m.c3.y - m.c0.y, m.c3.z - m.c0.z), m.c3.w - m.c0.w);
    const Plane<T> bottom(Vec3<T>(m.c3.x + m.c1.x, m.c3.y + m.c1.y, m.c3.z + m.c1.z), m.c3.w + m.c1.w);
    const Plane<T> top(Vec3<T>(m.c3.x - m.c1.x, m.c3.y - m.c1.y, m.c3.z - m.c1.z), m.c3.w - m.c1.w);
    const Plane<T> near_plane(Vec3<T>(m.c3.x + m.c2.x, m.c3.y + m.c2.y, m.c3.z + m.c2.z), m.c3.w + m.c2.w);
    const Plane<T> far_plane(Vec3<T>(m.c3.x - m.c2.x, m.c3.y - m.c2.y, m.c3.z - m.c2.z), m.c3.w - m.c2.w);
    return Frustum<T>{crd::containers::StaticArray<Plane<T>, 6>{normalized(left), normalized(right), normalized(bottom),
                                                                normalized(top), normalized(near_plane),
                                                                normalized(far_plane)}};
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

// ---- Scalar aliases --------------------------------------------------------

using Linef = Line<crd::f32>;
using Lined = Line<crd::f64>;
using Segmentf = Segment<crd::f32>;
using Segmentd = Segment<crd::f64>;
using Rayf = Ray<crd::f32>;
using Rayd = Ray<crd::f64>;
using Planef = Plane<crd::f32>;
using Planed = Plane<crd::f64>;
using Spheref = Sphere<crd::f32>;
using Sphered = Sphere<crd::f64>;
using AABBf = AABB<crd::f32>;
using AABBd = AABB<crd::f64>;
using OBBf = OBB<crd::f32>;
using OBBd = OBB<crd::f64>;
using Capsulef = Capsule<crd::f32>;
using Capsuled = Capsule<crd::f64>;
using Triangle3f = Triangle3<crd::f32>;
using Triangle3d = Triangle3<crd::f64>;
using Frustumf = Frustum<crd::f32>;
using Frustumd = Frustum<crd::f64>;

// Force-link anchor — keeps the (otherwise header-only) static library a real
// link target so ASan / the SIMD-emission checks have an .obj to inspect once
// v0c+ adds out-of-line SIMD batch kernels. Defined in geometry_primitives.cpp.
int force_link_geometry_primitives() noexcept;

} // namespace crd::geometry::primitives

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
