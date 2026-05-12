#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — primitive shape types (2D + 3D) + closest-point /
// intersection / containment helpers.
//
// Phase 3.1.7 v0a (ADR-0076 §13): this header parked the v0 type catalogue plus
// the helpers migrated wholesale from the (now-deleted) `crd/math/geometry.hpp`.
// v0b (ADR-0076 §13 amendment) adds the full 2D peer set and the closest-point
// catalogue (split into `closest_point.hpp`). v0c–v0f add intersection,
// barycentric, the iq formulary, and the branchless/SIMD corpus. `crd-math`
// stays the lean leaf substrate (Vec/Mat/Quat/Transform/SIMD/`deterministic`).
//
// Naming rule (PIN — read before adding a type; ADR-0076 §13 amendment):
//   * All shape types are templated on the scalar `T` (`crd::math::MathScalar`).
//   * Where a concept has both a 2D and a 3D form *under the same name*, BOTH
//     carry a dimension suffix — `Line2`/`Line3`, `Segment2`/`Segment3`,
//     `Ray2`/`Ray3`, `AABB2`/`AABB3`, `OBB2`/`OBB3`, `Triangle2`/`Triangle3`,
//     `Capsule2`/`Capsule3` — mirroring `crd::math::Vec2`/`Vec3`/`Mat2`/`Mat3`.
//   * Where the 2D and 3D forms have distinct natural names, neither is suffixed
//     — `Circle` (2D) / `Sphere` (3D).
//   * Where only one dimension exists, no suffix — `Plane` and `Frustum` are
//     3D-only (a half-space boundary in 2D is a `Line2` with a normal+offset;
//     there is no 2D frustum). `Point2`/`Point3` are aliases of `Vec2`/`Vec3`.
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
using crd::math::Vec2;
using crd::math::Vec3;

// A point in space — just `Vec2<T>` / `Vec3<T>`. Aliased for call-site
// readability where a parameter is conceptually a position, not a free vector.
template <MathScalar T> using Point2 = Vec2<T>;
template <MathScalar T> using Point3 = Vec3<T>;

// ---- Linear primitives: Line3 / Segment3 / Ray3 ------------------------------

// Infinite line through `point` along `direction` (direction need not be unit).
template <MathScalar T> struct Line3
{
    Vec3<T> point{};
    Vec3<T> direction{};

    constexpr Line3() noexcept = default;
    constexpr Line3(const Vec3<T>& point_in, const Vec3<T>& direction_in) noexcept
        : point(point_in), direction(direction_in)
    {
    }
};

// Finite segment from `a` to `b`.
template <MathScalar T> struct Segment3
{
    Vec3<T> a{};
    Vec3<T> b{};

    constexpr Segment3() noexcept = default;
    constexpr Segment3(const Vec3<T>& a_in, const Vec3<T>& b_in) noexcept : a(a_in), b(b_in) {}
};

// Ray3 from `origin` along `direction` (parameter t >= 0; direction need not be unit).
template <MathScalar T> struct Ray3
{
    Vec3<T> origin{};
    Vec3<T> direction{};

    constexpr Ray3() noexcept = default;
    constexpr Ray3(const Vec3<T>& origin_in, const Vec3<T>& direction_in) noexcept
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

template <MathScalar T> struct AABB3
{
    Vec3<T> min{};
    Vec3<T> max{};

    constexpr AABB3() noexcept = default;
    constexpr AABB3(const Vec3<T>& min_in, const Vec3<T>& max_in) noexcept : min(min_in), max(max_in) {}
};

// ---- Oriented bounding box ------------------------------------------------
//
// Center + half-extents in a local frame whose axes are the columns of
// `orientation` (orthonormal). World point = center + orientation * (local
// coordinate in [-half_extents, +half_extents]). Algorithms (SAT, closest-point)
// land in v0b/v0c — v0a defines the type.
template <MathScalar T> struct OBB3
{
    Vec3<T> center{};
    Vec3<T> half_extents{};
    crd::math::Mat3<T> orientation = crd::math::Mat3<T>::identity();

    OBB3() noexcept = default;
    OBB3(const Vec3<T>& center_in, const Vec3<T>& half_extents_in, const crd::math::Mat3<T>& orientation_in) noexcept
        : center(center_in), half_extents(half_extents_in), orientation(orientation_in)
    {
    }
};

// ---- Capsule3: segment a→b with a swept radius -----------------------------

template <MathScalar T> struct Capsule3
{
    Vec3<T> a{};
    Vec3<T> b{};
    T radius = static_cast<T>(0);

    constexpr Capsule3() noexcept = default;
    constexpr Capsule3(const Vec3<T>& a_in, const Vec3<T>& b_in, T radius_in) noexcept
        : a(a_in), b(b_in), radius(radius_in)
    {
    }
};

// ---- Cylinder3: segment a→b with radius, FLAT caps -------------------------
//
// Like Capsule3 but the ends are flat disks, not hemispheres. The axis need not
// be unit-length. (Useful for picking / robotics joints / vehicle wheels — eylem
// colliders use Capsule, not Cylinder, so this lives only in -primitives.)
template <MathScalar T> struct Cylinder3
{
    Vec3<T> a{};
    Vec3<T> b{};
    T radius = static_cast<T>(0);

    constexpr Cylinder3() noexcept = default;
    constexpr Cylinder3(const Vec3<T>& a_in, const Vec3<T>& b_in, T radius_in) noexcept
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

// ---- Tetrahedron: the 3-simplex (3D-only; no 2D peer, hence no suffix) -----
//
// Vertex order matters for orientation: `signed_volume` is positive when
// (a, b, c, d) is "positively oriented" (d on the positive side of plane abc).
template <MathScalar T> struct Tetrahedron
{
    Vec3<T> a{};
    Vec3<T> b{};
    Vec3<T> c{};
    Vec3<T> d{};

    constexpr Tetrahedron() noexcept = default;
    constexpr Tetrahedron(const Vec3<T>& a_in, const Vec3<T>& b_in, const Vec3<T>& c_in, const Vec3<T>& d_in) noexcept
        : a(a_in), b(b_in), c(c_in), d(d_in)
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

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Line3<T>& lhs, const Line3<T>& rhs) noexcept
{
    return lhs.point == rhs.point && lhs.direction == rhs.direction;
}
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Segment3<T>& lhs, const Segment3<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b;
}
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Ray3<T>& lhs, const Ray3<T>& rhs) noexcept
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
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const AABB3<T>& lhs, const AABB3<T>& rhs) noexcept
{
    return lhs.min == rhs.min && lhs.max == rhs.max;
}
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const OBB3<T>& lhs, const OBB3<T>& rhs) noexcept
{
    return lhs.center == rhs.center && lhs.half_extents == rhs.half_extents && lhs.orientation == rhs.orientation;
}
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Capsule3<T>& lhs, const Capsule3<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.radius == rhs.radius;
}
template <MathScalar T>
[[nodiscard]] constexpr bool operator==(const Cylinder3<T>& lhs, const Cylinder3<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.radius == rhs.radius;
}
template <MathScalar T>
[[nodiscard]] constexpr bool operator==(const Triangle3<T>& lhs, const Triangle3<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c;
}
template <MathScalar T>
[[nodiscard]] constexpr bool operator==(const Tetrahedron<T>& lhs, const Tetrahedron<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c && lhs.d == rhs.d;
}

// ---- Ray3 helpers -----------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> point_at(const Ray3<T>& ray, T t) noexcept
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

// ---- AABB3 helpers ----------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> center(const AABB3<T>& bounds) noexcept
{
    return (bounds.min + bounds.max) * static_cast<T>(0.5);
}

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> extents(const AABB3<T>& bounds) noexcept
{
    return (bounds.max - bounds.min) * static_cast<T>(0.5);
}

template <MathScalar T> [[nodiscard]] constexpr bool contains(const AABB3<T>& bounds, const Vec3<T>& point) noexcept
{
    return point.x >= bounds.min.x && point.x <= bounds.max.x && point.y >= bounds.min.y && point.y <= bounds.max.y &&
           point.z >= bounds.min.z && point.z <= bounds.max.z;
}

template <MathScalar T>
[[nodiscard]] constexpr Vec3<T> closest_point(const AABB3<T>& bounds, const Vec3<T>& point) noexcept
{
    return Vec3<T>(crd::math::clamp(point.x, bounds.min.x, bounds.max.x),
                   crd::math::clamp(point.y, bounds.min.y, bounds.max.y),
                   crd::math::clamp(point.z, bounds.min.z, bounds.max.z));
}

template <MathScalar T> [[nodiscard]] constexpr bool intersects(const AABB3<T>& lhs, const AABB3<T>& rhs) noexcept
{
    return lhs.min.x <= rhs.max.x && lhs.max.x >= rhs.min.x && lhs.min.y <= rhs.max.y && lhs.max.y >= rhs.min.y &&
           lhs.min.z <= rhs.max.z && lhs.max.z >= rhs.min.z;
}

// The "positive vertex" of `bounds` w.r.t. a direction — the corner furthest
// along `normal`. Used by plane/frustum-vs-AABB3.
template <MathScalar T>
[[nodiscard]] constexpr Vec3<T> positive_vertex(const AABB3<T>& bounds, const Vec3<T>& normal) noexcept
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

template <MathScalar T>
[[nodiscard]] constexpr bool intersects(const AABB3<T>& bounds, const Sphere<T>& sphere) noexcept
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

// ---- Tetrahedron helpers ---------------------------------------------------
// (barycentric / contains for tetrahedra live in `barycentric.hpp` — v0d.)

template <MathScalar T> [[nodiscard]] constexpr Vec3<T> centroid(const Tetrahedron<T>& tet) noexcept
{
    return (tet.a + tet.b + tet.c + tet.d) * static_cast<T>(0.25);
}

// Signed volume: (1/6)·det[ b−a | c−a | d−a ]. Positive when (a,b,c,d) is
// positively oriented (d on the positive side of the plane abc).
template <MathScalar T> [[nodiscard]] constexpr T signed_volume(const Tetrahedron<T>& tet) noexcept
{
    return crd::math::dot(tet.b - tet.a, crd::math::cross(tet.c - tet.a, tet.d - tet.a)) / static_cast<T>(6);
}
template <MathScalar T> [[nodiscard]] constexpr T volume(const Tetrahedron<T>& tet) noexcept
{
    const T sv = signed_volume(tet);
    return sv < static_cast<T>(0) ? -sv : sv;
}

// ---- Ray3 vs primitive ------------------------------------------------------

template <MathScalar T>
[[nodiscard]] inline bool intersect_ray_plane(const Ray3<T>& ray, const Plane<T>& plane, T& out_t,
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
[[nodiscard]] inline bool intersect_ray_sphere(const Ray3<T>& ray, const Sphere<T>& sphere, T& out_t,
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
[[nodiscard]] inline bool intersect_ray_triangle(const Ray3<T>& ray, const Triangle3<T>& tri, T& out_t,
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

template <MathScalar T> [[nodiscard]] inline bool intersects(const Frustum<T>& frustum, const AABB3<T>& bounds) noexcept
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

// ===========================================================================
// 2D PRIMITIVES (v0b — ADR-0076 §13 amendment)
//
// Planar peers of the 3D types above: same templating, same field names, with
// `Vec2<T>` / `Mat2<T>` where the 3D form uses `Vec3` / `Mat3`. No `Plane2`
// (the 2D analog of a plane is a `Line2` carrying a normal+offset — see the 2D
// signed-distance overload taking `Line2` in `closest_point.hpp`); no
// `Frustum2`. Closest-point / intersection on these land alongside the 3D ones.
// ===========================================================================

// Infinite line through `point` along `direction` (direction need not be unit).
template <MathScalar T> struct Line2
{
    Vec2<T> point{};
    Vec2<T> direction{};

    constexpr Line2() noexcept = default;
    constexpr Line2(const Vec2<T>& point_in, const Vec2<T>& direction_in) noexcept
        : point(point_in), direction(direction_in)
    {
    }
};

// Finite segment from `a` to `b`.
template <MathScalar T> struct Segment2
{
    Vec2<T> a{};
    Vec2<T> b{};

    constexpr Segment2() noexcept = default;
    constexpr Segment2(const Vec2<T>& a_in, const Vec2<T>& b_in) noexcept : a(a_in), b(b_in) {}
};

// Ray from `origin` along `direction` (parameter t >= 0; direction need not be unit).
template <MathScalar T> struct Ray2
{
    Vec2<T> origin{};
    Vec2<T> direction{};

    constexpr Ray2() noexcept = default;
    constexpr Ray2(const Vec2<T>& origin_in, const Vec2<T>& direction_in) noexcept
        : origin(origin_in), direction(direction_in)
    {
    }
};

// Axis-aligned bounding box (2D).
template <MathScalar T> struct AABB2
{
    Vec2<T> min{};
    Vec2<T> max{};

    constexpr AABB2() noexcept = default;
    constexpr AABB2(const Vec2<T>& min_in, const Vec2<T>& max_in) noexcept : min(min_in), max(max_in) {}
};

// Oriented bounding box (2D): center + half-extents in a local frame whose axes
// are the columns of `orientation` (orthonormal `Mat2`).
template <MathScalar T> struct OBB2
{
    Vec2<T> center{};
    Vec2<T> half_extents{};
    crd::math::Mat2<T> orientation = crd::math::Mat2<T>::identity();

    OBB2() noexcept = default;
    OBB2(const Vec2<T>& center_in, const Vec2<T>& half_extents_in, const crd::math::Mat2<T>& orientation_in) noexcept
        : center(center_in), half_extents(half_extents_in), orientation(orientation_in)
    {
    }
};

// Circle — the 2D bounding-volume peer of `Sphere` (distinct natural name, so no
// `2` suffix per the naming rule).
template <MathScalar T> struct Circle
{
    Vec2<T> center{};
    T radius = static_cast<T>(0);

    constexpr Circle() noexcept = default;
    constexpr Circle(const Vec2<T>& center_in, T radius_in) noexcept : center(center_in), radius(radius_in) {}
};

// Capsule (2D) — a "stadium" / discorectangle: segment a→b with a swept radius.
template <MathScalar T> struct Capsule2
{
    Vec2<T> a{};
    Vec2<T> b{};
    T radius = static_cast<T>(0);

    constexpr Capsule2() noexcept = default;
    constexpr Capsule2(const Vec2<T>& a_in, const Vec2<T>& b_in, T radius_in) noexcept
        : a(a_in), b(b_in), radius(radius_in)
    {
    }
};

// Cylinder (2D) — a "thick segment" with FLAT ends: the rectangle of half-width
// `radius` swept along a→b (the 2D peer of `Cylinder3`; an oriented rectangle
// parameterised as axis + half-width, vs `OBB2`'s center + half-extents form).
template <MathScalar T> struct Cylinder2
{
    Vec2<T> a{};
    Vec2<T> b{};
    T radius = static_cast<T>(0);

    constexpr Cylinder2() noexcept = default;
    constexpr Cylinder2(const Vec2<T>& a_in, const Vec2<T>& b_in, T radius_in) noexcept
        : a(a_in), b(b_in), radius(radius_in)
    {
    }
};

// Triangle (2D).
template <MathScalar T> struct Triangle2
{
    Vec2<T> a{};
    Vec2<T> b{};
    Vec2<T> c{};

    constexpr Triangle2() noexcept = default;
    constexpr Triangle2(const Vec2<T>& a_in, const Vec2<T>& b_in, const Vec2<T>& c_in) noexcept
        : a(a_in), b(b_in), c(c_in)
    {
    }
};

// ---- Equality (2D) ---------------------------------------------------------

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Line2<T>& lhs, const Line2<T>& rhs) noexcept
{
    return lhs.point == rhs.point && lhs.direction == rhs.direction;
}
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Segment2<T>& lhs, const Segment2<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b;
}
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Ray2<T>& lhs, const Ray2<T>& rhs) noexcept
{
    return lhs.origin == rhs.origin && lhs.direction == rhs.direction;
}
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const AABB2<T>& lhs, const AABB2<T>& rhs) noexcept
{
    return lhs.min == rhs.min && lhs.max == rhs.max;
}
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const OBB2<T>& lhs, const OBB2<T>& rhs) noexcept
{
    return lhs.center == rhs.center && lhs.half_extents == rhs.half_extents && lhs.orientation == rhs.orientation;
}
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Circle<T>& lhs, const Circle<T>& rhs) noexcept
{
    return lhs.center == rhs.center && lhs.radius == rhs.radius;
}
template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Capsule2<T>& lhs, const Capsule2<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.radius == rhs.radius;
}
template <MathScalar T>
[[nodiscard]] constexpr bool operator==(const Cylinder2<T>& lhs, const Cylinder2<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.radius == rhs.radius;
}
template <MathScalar T>
[[nodiscard]] constexpr bool operator==(const Triangle2<T>& lhs, const Triangle2<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c;
}

// ---- 2D helpers (peers of the 3D Ray/AABB/Sphere/Triangle helpers) ---------

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> point_at(const Ray2<T>& ray, T t) noexcept
{
    return ray.origin + ray.direction * t;
}

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> center(const AABB2<T>& bounds) noexcept
{
    return (bounds.min + bounds.max) * static_cast<T>(0.5);
}
template <MathScalar T> [[nodiscard]] constexpr Vec2<T> extents(const AABB2<T>& bounds) noexcept
{
    return (bounds.max - bounds.min) * static_cast<T>(0.5);
}
template <MathScalar T> [[nodiscard]] constexpr bool contains(const AABB2<T>& bounds, const Vec2<T>& point) noexcept
{
    return point.x >= bounds.min.x && point.x <= bounds.max.x && point.y >= bounds.min.y && point.y <= bounds.max.y;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const AABB2<T>& lhs, const AABB2<T>& rhs) noexcept
{
    return lhs.min.x <= rhs.max.x && lhs.max.x >= rhs.min.x && lhs.min.y <= rhs.max.y && lhs.max.y >= rhs.min.y;
}
template <MathScalar T>
[[nodiscard]] constexpr Vec2<T> positive_vertex(const AABB2<T>& bounds, const Vec2<T>& normal) noexcept
{
    return Vec2<T>(normal.x >= static_cast<T>(0) ? bounds.max.x : bounds.min.x,
                   normal.y >= static_cast<T>(0) ? bounds.max.y : bounds.min.y);
}

template <MathScalar T> [[nodiscard]] constexpr bool contains(const Circle<T>& circle, const Vec2<T>& point) noexcept
{
    return crd::math::distance_squared(circle.center, point) <= circle.radius * circle.radius;
}
template <MathScalar T> [[nodiscard]] constexpr bool intersects(const Circle<T>& lhs, const Circle<T>& rhs) noexcept
{
    const T r = lhs.radius + rhs.radius;
    return crd::math::distance_squared(lhs.center, rhs.center) <= r * r;
}
template <MathScalar T>
[[nodiscard]] constexpr bool intersects(const AABB2<T>& bounds, const Circle<T>& circle) noexcept
{
    const Vec2<T> p(crd::math::clamp(circle.center.x, bounds.min.x, bounds.max.x),
                    crd::math::clamp(circle.center.y, bounds.min.y, bounds.max.y));
    return crd::math::distance_squared(p, circle.center) <= circle.radius * circle.radius;
}

template <MathScalar T> [[nodiscard]] constexpr Vec2<T> centroid(const Triangle2<T>& tri) noexcept
{
    return (tri.a + tri.b + tri.c) / static_cast<T>(3);
}
// Signed area — positive when (a, b, c) wind counter-clockwise.
template <MathScalar T> [[nodiscard]] constexpr T signed_area(const Triangle2<T>& tri) noexcept
{
    return static_cast<T>(0.5) * crd::math::cross(tri.b - tri.a, tri.c - tri.a);
}
// Barycentric coordinates (u, v, w) of `point` w.r.t. `tri` — same projection
// form as the 3D `barycentric`, valid for any non-degenerate triangle.
template <MathScalar T>
[[nodiscard]] constexpr Vec3<T> barycentric(const Triangle2<T>& tri, const Vec2<T>& point) noexcept
{
    const Vec2<T> v0 = tri.b - tri.a;
    const Vec2<T> v1 = tri.c - tri.a;
    const Vec2<T> v2 = point - tri.a;
    const T d00 = crd::math::dot(v0, v0);
    const T d01 = crd::math::dot(v0, v1);
    const T d11 = crd::math::dot(v1, v1);
    const T d20 = crd::math::dot(v2, v0);
    const T d21 = crd::math::dot(v2, v1);
    const T denom = d00 * d11 - d01 * d01;
    CRD_ASSERT(!crd::math::approx_zero(denom));
    const T v = (d11 * d20 - d01 * d21) / denom;
    const T w = (d00 * d21 - d01 * d20) / denom;
    return Vec3<T>(static_cast<T>(1) - v - w, v, w);
}
template <MathScalar T>
[[nodiscard]] constexpr bool contains(const Triangle2<T>& tri, const Vec2<T>& point,
                                      T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const Vec3<T> bc = barycentric(tri, point);
    return bc.x >= -epsilon && bc.y >= -epsilon && bc.z >= -epsilon;
}

// ---- Scalar aliases --------------------------------------------------------

using Line3f = Line3<crd::f32>;
using Line3d = Line3<crd::f64>;
using Line2f = Line2<crd::f32>;
using Line2d = Line2<crd::f64>;
using Segment3f = Segment3<crd::f32>;
using Segment3d = Segment3<crd::f64>;
using Segment2f = Segment2<crd::f32>;
using Segment2d = Segment2<crd::f64>;
using Ray3f = Ray3<crd::f32>;
using Ray3d = Ray3<crd::f64>;
using Ray2f = Ray2<crd::f32>;
using Ray2d = Ray2<crd::f64>;
using Planef = Plane<crd::f32>;
using Planed = Plane<crd::f64>;
using Spheref = Sphere<crd::f32>;
using Sphered = Sphere<crd::f64>;
using Circlef = Circle<crd::f32>;
using Circled = Circle<crd::f64>;
using AABB3f = AABB3<crd::f32>;
using AABB3d = AABB3<crd::f64>;
using AABB2f = AABB2<crd::f32>;
using AABB2d = AABB2<crd::f64>;
using OBB3f = OBB3<crd::f32>;
using OBB3d = OBB3<crd::f64>;
using OBB2f = OBB2<crd::f32>;
using OBB2d = OBB2<crd::f64>;
using Capsule3f = Capsule3<crd::f32>;
using Capsule3d = Capsule3<crd::f64>;
using Capsule2f = Capsule2<crd::f32>;
using Capsule2d = Capsule2<crd::f64>;
using Cylinder3f = Cylinder3<crd::f32>;
using Cylinder3d = Cylinder3<crd::f64>;
using Cylinder2f = Cylinder2<crd::f32>;
using Cylinder2d = Cylinder2<crd::f64>;
using Triangle3f = Triangle3<crd::f32>;
using Triangle3d = Triangle3<crd::f64>;
using Triangle2f = Triangle2<crd::f32>;
using Triangle2d = Triangle2<crd::f64>;
using Tetrahedronf = Tetrahedron<crd::f32>;
using Tetrahedrond = Tetrahedron<crd::f64>;
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
