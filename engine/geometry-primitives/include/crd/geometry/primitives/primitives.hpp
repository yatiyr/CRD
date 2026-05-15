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

#include <crd/containers/span.hpp>
#include <crd/containers/static_array.hpp>
#include <crd/core/types.hpp>
#include <crd/math/mat.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

namespace crd::geometry::primitives
{
using crd::math::MathScalar;
using crd::math::MathValue;
using crd::math::Vec2;
using crd::math::Vec3;

// A point in space — just `Vec2<T>` / `Vec3<T>`. Aliased for call-site
// readability where a parameter is conceptually a position, not a free vector.
template <MathValue T> using Point2 = Vec2<T>;
template <MathValue T> using Point3 = Vec3<T>;

// ---- Linear primitives: Line3 / Segment3 / Ray3 ------------------------------

// Infinite line through `point` along `direction` (direction need not be unit).
template <MathValue T> struct Line3
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
template <MathValue T> struct Segment3
{
    Vec3<T> a{};
    Vec3<T> b{};

    constexpr Segment3() noexcept = default;
    constexpr Segment3(const Vec3<T>& a_in, const Vec3<T>& b_in) noexcept : a(a_in), b(b_in) {}
};

// Ray3 from `origin` along `direction` (parameter t >= 0; direction need not be unit).
template <MathValue T> struct Ray3
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

template <MathValue T> struct Plane
{
    Vec3<T> normal{};
    T d{}; // v0d-2: was `= static_cast<T>(0)` — broken for Quantity (explicit ctor). T{} works for both raw and Quantity (= 0).

    constexpr Plane() noexcept = default;
    constexpr Plane(const Vec3<T>& normal_in, T d_in) noexcept : normal(normal_in), d(d_in) {}
};

// ---- Sphere ----------------------------------------------------------------

template <MathValue T> struct Sphere
{
    Vec3<T> center{};
    T radius = static_cast<T>(0);

    constexpr Sphere() noexcept = default;
    constexpr Sphere(const Vec3<T>& center_in, T radius_in) noexcept : center(center_in), radius(radius_in) {}
};

// ---- Axis-aligned bounding box --------------------------------------------

template <MathValue T> struct AABB3
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
template <MathValue T> struct OBB3
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

template <MathValue T> struct Capsule3
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
template <MathValue T> struct Cylinder3
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

template <MathValue T> struct Triangle3
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
template <MathValue T> struct Tetrahedron
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

template <MathValue T> struct Frustum
{
    crd::containers::StaticArray<Plane<T>, 6> planes{};

    constexpr Frustum() noexcept = default;
    constexpr explicit Frustum(const crd::containers::StaticArray<Plane<T>, 6>& planes_in) noexcept : planes(planes_in)
    {
    }
};

// ---- ConvexHullView: a non-owning view of a convex polyhedron --------------
//
// The query-side hull (ADR-0076 §15): `vertices` (the hull's extreme points),
// `faces` (one outward-facing plane per face — `dot(n, x) + d <= 0` is the
// inside half-space), and `face_vertex_indices` packed CCW per face with a
// matching `face_vertex_offsets` (size `faces.size() + 1`, a prefix-sum so
// face `f` owns `face_vertex_indices[offsets[f] .. offsets[f+1])`). Owns
// nothing — the data lives in a cooked collider / a `crd-convex` v3 result;
// `crd-eylem`'s `Collider::ConvexHull` references one. Ray-vs-hull /
// closest-point-on-hull / contains-point-in-hull-via-GJK land in `-convex`
// (Phase 3.1.7 v2); v1h ships the type plus the two trivially-correct queries
// (`support`, plane-based `contains` — defined just below the 3D helpers,
// after `signed_distance`).
template <MathValue T> struct ConvexHullView
{
    crd::containers::ConstSpan<Vec3<T>> vertices{};
    crd::containers::ConstSpan<Plane<T>> faces{};
    crd::containers::ConstSpan<crd::u32> face_vertex_indices{};
    crd::containers::ConstSpan<crd::u32> face_vertex_offsets{};
    // v2g — OPTIONAL per-vertex adjacency for the hill-climb support path
    // (PhysX/Havok pattern). Empty ⇒ no adjacency, support() falls back to
    // O(N) linear scan. When populated:
    //   * `vertex_adjacency_indices` is a flat list of neighbor vertex
    //     indices for every vertex.
    //   * `vertex_adjacency_offsets` is the prefix-sum (`size = vertices.size()
    //     + 1`); vertex `i`'s neighbors are at
    //     `vertex_adjacency_indices[vertex_adjacency_offsets[i]
    //                              .. vertex_adjacency_offsets[i+1])`.
    //   * The adjacency must be EDGE-symmetric (every edge appears in both
    //     endpoints' neighbor lists) and DUPLICATE-FREE.
    // Cookers (V-HACD, Quickhull v3) populate this; hand-built hulls can use
    // the `compute_vertex_adjacency_from_faces` helper in eylem/test code.
    crd::containers::ConstSpan<crd::u32> vertex_adjacency_indices{};
    crd::containers::ConstSpan<crd::u32> vertex_adjacency_offsets{};

    // v2h — OPTIONAL Structure-of-Arrays vertex layout for the AVX2 `Vec8f`
    // SIMD-batched support path. Empty ⇒ no SoA, support() uses the AoS
    // `vertices` array (linear-scan or hill-climb).
    //
    // Layout: three flat `ConstSpan<f32>` arrays, one per coordinate.
    // `vx_soa[i]` / `vy_soa[i]` / `vz_soa[i]` are vertex `i`'s coordinates
    // (the same data as `vertices[i]` but transposed for SIMD-friendly
    // load/multiply).
    //
    // **Padding contract** (PIN — advisor 2026-05-14): all three SoA spans
    // MUST be padded to the next multiple of 8 (`padded_size = (n + 7) & ~7`)
    // by REPEATING vertex 0's coordinates in lanes `[n, padded_size)`. This
    // lets the SIMD reducer scan all 8 lanes per chunk without branching
    // on the chunk-end: padded lanes contribute `dot(vertex_0, dir)` —
    // tied with lane 0 on `vertex_idx` 0 winning by lowest-index tiebreak.
    //
    // **Alignment recommendation**: cookers should align SoA storage to 32
    // bytes so `Vec8f::load_aligned` can be used. The current implementation
    // uses unaligned loads (`Vec8f::load`) — alignment is a v2-close perf
    // followup.
    //
    // **f32-only** for v2h. f64 hulls (v2i territory) fall through to the
    // AoS linear-scan path (the SIMD dispatch is `if constexpr (T == f32)`).
    crd::containers::ConstSpan<crd::f32> vx_soa{};
    crd::containers::ConstSpan<crd::f32> vy_soa{};
    crd::containers::ConstSpan<crd::f32> vz_soa{};

    constexpr ConvexHullView() noexcept = default;
    constexpr ConvexHullView(crd::containers::ConstSpan<Vec3<T>> vertices_in,
                             crd::containers::ConstSpan<Plane<T>> faces_in,
                             crd::containers::ConstSpan<crd::u32> face_vertex_indices_in,
                             crd::containers::ConstSpan<crd::u32> face_vertex_offsets_in) noexcept
        : vertices(vertices_in), faces(faces_in), face_vertex_indices(face_vertex_indices_in),
          face_vertex_offsets(face_vertex_offsets_in)
    {
    }
    // v2g 6-arg constructor accepting adjacency. Backward-compatible: the
    // 4-arg form leaves adjacency empty.
    constexpr ConvexHullView(crd::containers::ConstSpan<Vec3<T>> vertices_in,
                             crd::containers::ConstSpan<Plane<T>> faces_in,
                             crd::containers::ConstSpan<crd::u32> face_vertex_indices_in,
                             crd::containers::ConstSpan<crd::u32> face_vertex_offsets_in,
                             crd::containers::ConstSpan<crd::u32> vertex_adjacency_indices_in,
                             crd::containers::ConstSpan<crd::u32> vertex_adjacency_offsets_in) noexcept
        : vertices(vertices_in), faces(faces_in), face_vertex_indices(face_vertex_indices_in),
          face_vertex_offsets(face_vertex_offsets_in), vertex_adjacency_indices(vertex_adjacency_indices_in),
          vertex_adjacency_offsets(vertex_adjacency_offsets_in)
    {
    }
    // v2h 9-arg constructor accepting adjacency + SoA. Backward-compatible:
    // the 4-arg and 6-arg forms leave SoA empty.
    constexpr ConvexHullView(crd::containers::ConstSpan<Vec3<T>> vertices_in,
                             crd::containers::ConstSpan<Plane<T>> faces_in,
                             crd::containers::ConstSpan<crd::u32> face_vertex_indices_in,
                             crd::containers::ConstSpan<crd::u32> face_vertex_offsets_in,
                             crd::containers::ConstSpan<crd::u32> vertex_adjacency_indices_in,
                             crd::containers::ConstSpan<crd::u32> vertex_adjacency_offsets_in,
                             crd::containers::ConstSpan<crd::f32> vx_soa_in,
                             crd::containers::ConstSpan<crd::f32> vy_soa_in,
                             crd::containers::ConstSpan<crd::f32> vz_soa_in) noexcept
        : vertices(vertices_in), faces(faces_in), face_vertex_indices(face_vertex_indices_in),
          face_vertex_offsets(face_vertex_offsets_in), vertex_adjacency_indices(vertex_adjacency_indices_in),
          vertex_adjacency_offsets(vertex_adjacency_offsets_in), vx_soa(vx_soa_in), vy_soa(vy_soa_in),
          vz_soa(vz_soa_in)
    {
    }
};

// ---- Equality --------------------------------------------------------------

template <MathValue T> [[nodiscard]] constexpr bool operator==(const Line3<T>& lhs, const Line3<T>& rhs) noexcept
{
    return lhs.point == rhs.point && lhs.direction == rhs.direction;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const Segment3<T>& lhs, const Segment3<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const Ray3<T>& lhs, const Ray3<T>& rhs) noexcept
{
    return lhs.origin == rhs.origin && lhs.direction == rhs.direction;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const Plane<T>& lhs, const Plane<T>& rhs) noexcept
{
    return lhs.normal == rhs.normal && lhs.d == rhs.d;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const Sphere<T>& lhs, const Sphere<T>& rhs) noexcept
{
    return lhs.center == rhs.center && lhs.radius == rhs.radius;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const AABB3<T>& lhs, const AABB3<T>& rhs) noexcept
{
    return lhs.min == rhs.min && lhs.max == rhs.max;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const OBB3<T>& lhs, const OBB3<T>& rhs) noexcept
{
    return lhs.center == rhs.center && lhs.half_extents == rhs.half_extents && lhs.orientation == rhs.orientation;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const Capsule3<T>& lhs, const Capsule3<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.radius == rhs.radius;
}
template <MathValue T>
[[nodiscard]] constexpr bool operator==(const Cylinder3<T>& lhs, const Cylinder3<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.radius == rhs.radius;
}
template <MathValue T>
[[nodiscard]] constexpr bool operator==(const Triangle3<T>& lhs, const Triangle3<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c;
}
template <MathValue T>
[[nodiscard]] constexpr bool operator==(const Tetrahedron<T>& lhs, const Tetrahedron<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c && lhs.d == rhs.d;
}

// ---- Ray3 helpers -----------------------------------------------------------

template <MathValue T> [[nodiscard]] constexpr Vec3<T> point_at(const Ray3<T>& ray, T t) noexcept
{
    return ray.origin + ray.direction * t;
}

// ---- Plane helpers ---------------------------------------------------------

template <MathValue T>
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

template <MathValue T> [[nodiscard]] inline Plane<T> normalized(Plane<T> plane) noexcept
{
    const bool ok = try_normalize(plane);
    CRD_ASSERT(ok);
    (void)ok;
    return plane;
}

template <MathValue T>
[[nodiscard]] inline Plane<T> plane_from_point_normal(const Vec3<T>& point, const Vec3<T>& normal) noexcept
{
    const Vec3<T> unit_normal = crd::math::normalized(normal);
    return Plane<T>(unit_normal, -crd::math::dot(unit_normal, point));
}

template <MathValue T> [[nodiscard]] constexpr T signed_distance(const Plane<T>& plane, const Vec3<T>& point) noexcept
{
    return crd::math::dot(plane.normal, point) + plane.d;
}

template <MathValue T> [[nodiscard]] inline Vec3<T> closest_point(const Plane<T>& plane, const Vec3<T>& point) noexcept
{
    return point - plane.normal * signed_distance(plane, point);
}

// ---- AABB3 helpers ----------------------------------------------------------

template <MathValue T> [[nodiscard]] constexpr Vec3<T> center(const AABB3<T>& bounds) noexcept
{
    return (bounds.min + bounds.max) * static_cast<T>(0.5);
}

template <MathValue T> [[nodiscard]] constexpr Vec3<T> extents(const AABB3<T>& bounds) noexcept
{
    return (bounds.max - bounds.min) * static_cast<T>(0.5);
}

template <MathValue T> [[nodiscard]] constexpr bool contains(const AABB3<T>& bounds, const Vec3<T>& point) noexcept
{
    return point.x >= bounds.min.x && point.x <= bounds.max.x && point.y >= bounds.min.y && point.y <= bounds.max.y &&
           point.z >= bounds.min.z && point.z <= bounds.max.z;
}

template <MathValue T>
[[nodiscard]] constexpr Vec3<T> closest_point(const AABB3<T>& bounds, const Vec3<T>& point) noexcept
{
    return Vec3<T>(crd::math::clamp(point.x, bounds.min.x, bounds.max.x),
                   crd::math::clamp(point.y, bounds.min.y, bounds.max.y),
                   crd::math::clamp(point.z, bounds.min.z, bounds.max.z));
}

template <MathValue T> [[nodiscard]] constexpr bool intersects(const AABB3<T>& lhs, const AABB3<T>& rhs) noexcept
{
    return lhs.min.x <= rhs.max.x && lhs.max.x >= rhs.min.x && lhs.min.y <= rhs.max.y && lhs.max.y >= rhs.min.y &&
           lhs.min.z <= rhs.max.z && lhs.max.z >= rhs.min.z;
}

// The "positive vertex" of `bounds` w.r.t. a direction — the corner furthest
// along `normal`. Used by plane/frustum-vs-AABB3.
template <MathValue T>
[[nodiscard]] constexpr Vec3<T> positive_vertex(const AABB3<T>& bounds, const Vec3<T>& normal) noexcept
{
    return Vec3<T>(normal.x >= static_cast<T>(0) ? bounds.max.x : bounds.min.x,
                   normal.y >= static_cast<T>(0) ? bounds.max.y : bounds.min.y,
                   normal.z >= static_cast<T>(0) ? bounds.max.z : bounds.min.z);
}

// ---- Sphere helpers --------------------------------------------------------

template <MathValue T> [[nodiscard]] constexpr bool contains(const Sphere<T>& sphere, const Vec3<T>& point) noexcept
{
    return crd::math::distance_squared(sphere.center, point) <= sphere.radius * sphere.radius;
}

template <MathValue T> [[nodiscard]] constexpr bool intersects(const Sphere<T>& lhs, const Sphere<T>& rhs) noexcept
{
    const T r = lhs.radius + rhs.radius;
    return crd::math::distance_squared(lhs.center, rhs.center) <= r * r;
}

template <MathValue T>
[[nodiscard]] constexpr bool intersects(const AABB3<T>& bounds, const Sphere<T>& sphere) noexcept
{
    return crd::math::distance_squared(closest_point(bounds, sphere.center), sphere.center) <=
           sphere.radius * sphere.radius;
}

// ---- Triangle helpers ------------------------------------------------------

template <MathValue T> [[nodiscard]] constexpr Vec3<T> centroid(const Triangle3<T>& tri) noexcept
{
    return (tri.a + tri.b + tri.c) / static_cast<T>(3);
}

template <MathValue T> [[nodiscard]] inline Vec3<T> normal(const Triangle3<T>& tri) noexcept
{
    return crd::math::normalized(crd::math::cross(tri.b - tri.a, tri.c - tri.a));
}

template <MathValue T>
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

template <MathValue T>
[[nodiscard]] constexpr bool contains(const Triangle3<T>& tri, const Vec3<T>& point,
                                      T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const Vec3<T> bc = barycentric(tri, point);
    return bc.x >= -epsilon && bc.y >= -epsilon && bc.z >= -epsilon;
}

// ---- Tetrahedron helpers ---------------------------------------------------
// (barycentric / contains for tetrahedra live in `barycentric.hpp` — v0d.)

template <MathValue T> [[nodiscard]] constexpr Vec3<T> centroid(const Tetrahedron<T>& tet) noexcept
{
    return (tet.a + tet.b + tet.c + tet.d) * static_cast<T>(0.25);
}

// Signed volume: (1/6)·det[ b−a | c−a | d−a ]. Positive when (a,b,c,d) is
// positively oriented (d on the positive side of the plane abc).
template <MathValue T> [[nodiscard]] constexpr T signed_volume(const Tetrahedron<T>& tet) noexcept
{
    return crd::math::dot(tet.b - tet.a, crd::math::cross(tet.c - tet.a, tet.d - tet.a)) / static_cast<T>(6);
}
template <MathValue T> [[nodiscard]] constexpr T volume(const Tetrahedron<T>& tet) noexcept
{
    const T sv = signed_volume(tet);
    return sv < static_cast<T>(0) ? -sv : sv;
}

// ---- Ray3 vs primitive ------------------------------------------------------

template <MathValue T>
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

template <MathValue T>
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
template <MathValue T>
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
template <MathValue T>
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

template <MathValue T> [[nodiscard]] inline bool contains(const Frustum<T>& frustum, const Vec3<T>& point) noexcept
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

template <MathValue T>
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

template <MathValue T> [[nodiscard]] inline bool intersects(const Frustum<T>& frustum, const AABB3<T>& bounds) noexcept
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

// ---- SupportPoint + support() overloads (the convex-shape substrate) -------
//
// `SupportPoint<T>` lives in the primitives layer so the four `support()`
// overloads (Sphere / OBB3 / Capsule3 / ConvexHullView) can live in the same
// namespace as the shape types — argument-dependent lookup then finds them
// from any caller (the `ConvexShape` concept in `crd-geometry-convex` is one
// such caller). The `vertex_idx` field is what makes GJK + EPA cross-platform
// bit-exact (ADR-0076 §4 pin #14): hulls report the argmax vertex index with
// lowest-index tiebreak on coincident extrema, analytic shapes report
// `k_invalid_vertex` (sentinel), and GJK's primary termination is the
// `(vidx_a, vidx_b)` index-match (Box2D pattern) — epsilon-free, deterministic.
//
// Phase 3.1.7 v2a (this slice) defines the four substrate overloads. v1h
// shipped only the `ConvexHullView` form with a bare-`Vec3` return; v2a
// expands the return shape to carry the vertex index and adds the analytic-
// shape overloads. The `ConvexShape` concept + `gjk_distance` driver live
// in the sibling `crd-geometry-convex` module.

// Sentinel for "no enumerable vertex" — analytic shapes (sphere / capsule)
// report this. Hulls report a real index in [0, hull.vertices.size()); OBB3
// reports a corner index in [0, 8) (packed `(sx<<2)|(sy<<1)|sz`).
inline constexpr crd::u32 k_invalid_vertex = ~crd::u32{0};

// The result of a support query: the extreme point on the shape in the given
// direction (shape-local frame), plus the vertex index that produced it (for
// hulls / OBBs) or `k_invalid_vertex` (for analytic shapes).
//
// Field order is FROZEN — GJK's index-match termination + EPA (v2c) read
// `point` and `vertex_idx` by name. Aggregate layout means `SupportPoint
// <f32>` is 16 bytes, `SupportPoint<f64>` 32 bytes.
template <MathValue T> struct SupportPoint
{
    Vec3<T> point{};
    crd::u32 vertex_idx = k_invalid_vertex;
};

namespace support_detail
{
// Unit vector in `dir`, or a canonical +X axis fallback on zero / sub-normal
// input. The deterministic zero-direction reply.
template <MathValue T> [[nodiscard]] inline Vec3<T> normalize_safe(const Vec3<T>& dir) noexcept
{
    const T dd = crd::math::dot(dir, dir);
    if (!(dd > std::numeric_limits<T>::min()))
    {
        return Vec3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
    }
    const T inv_len = static_cast<T>(1) / static_cast<T>(std::sqrt(dd));
    return Vec3<T>(dir.x * inv_len, dir.y * inv_len, dir.z * inv_len);
}
} // namespace support_detail

// ---- support(Sphere): `center + radius * dir̂`; zero `dir` → canonical reply
template <MathValue T>
[[nodiscard]] inline SupportPoint<T> support(const Sphere<T>& sphere, const Vec3<T>& dir) noexcept
{
    const Vec3<T> d = support_detail::normalize_safe(dir);
    return SupportPoint<T>{Vec3<T>(sphere.center.x + d.x * sphere.radius, sphere.center.y + d.y * sphere.radius,
                                   sphere.center.z + d.z * sphere.radius),
                           k_invalid_vertex};
}

// ---- support(OBB3): corner pick by `dot(axis_i, dir)` signs, `+h` on tie.
// Packed corner index: bit 2 ⇐ x≥0, bit 1 ⇐ y≥0, bit 0 ⇐ z≥0.
template <MathValue T>
[[nodiscard]] inline SupportPoint<T> support(const OBB3<T>& obb, const Vec3<T>& dir) noexcept
{
    const T dx = crd::math::dot(obb.orientation.c0, dir);
    const T dy = crd::math::dot(obb.orientation.c1, dir);
    const T dz = crd::math::dot(obb.orientation.c2, dir);
    const T sx = dx < static_cast<T>(0) ? -obb.half_extents.x : obb.half_extents.x;
    const T sy = dy < static_cast<T>(0) ? -obb.half_extents.y : obb.half_extents.y;
    const T sz = dz < static_cast<T>(0) ? -obb.half_extents.z : obb.half_extents.z;
    const Vec3<T> point(obb.center.x + obb.orientation.c0.x * sx + obb.orientation.c1.x * sy + obb.orientation.c2.x * sz,
                        obb.center.y + obb.orientation.c0.y * sx + obb.orientation.c1.y * sy + obb.orientation.c2.y * sz,
                        obb.center.z + obb.orientation.c0.z * sx + obb.orientation.c1.z * sy + obb.orientation.c2.z * sz);
    const crd::u32 idx = (dx < static_cast<T>(0) ? 0U : 4U) | (dy < static_cast<T>(0) ? 0U : 2U) |
                         (dz < static_cast<T>(0) ? 0U : 1U);
    return SupportPoint<T>{point, idx};
}

// ---- support(Capsule3): pick endpoint by `dot(endpoint, dir)`, tie → `a`.
// Radial adds `radius·dir̂`; zero `dir` → canonical +X reply.
//
// Reports `vertex_idx = k_invalid_vertex` — even though the endpoint choice
// (a vs b) is discrete, the **radial offset is a continuous function of
// `dir`**, so the support point as a whole is not a bijection from a small
// discrete vertex set. If we reported `vertex_idx ∈ {0, 1}`, GJK's index-
// match termination would fire as soon as the endpoint pair stabilises,
// leaving the radial direction not-yet-converged (~1% distance error on
// capsule-capsule pairs at typical scales). With `k_invalid_vertex`, GJK
// falls back to the geometric `|d|² + d·w ≤ ε²` test which polishes the
// radial direction to ε precision.
//
// Same reasoning applies to `Sphere` (already reports `k_invalid_vertex`).
// `OBB3` and `ConvexHullView` are polyhedral — their supports ARE a true
// bijection from a discrete vertex set, so they keep their real indices and
// benefit from the bit-exact index-match termination.
template <MathValue T>
[[nodiscard]] inline SupportPoint<T> support(const Capsule3<T>& cap, const Vec3<T>& dir) noexcept
{
    const T da = crd::math::dot(cap.a, dir);
    const T db = crd::math::dot(cap.b, dir);
    const Vec3<T>& end = (db > da) ? cap.b : cap.a;
    const Vec3<T> d = support_detail::normalize_safe(dir);
    return SupportPoint<T>{Vec3<T>(end.x + d.x * cap.radius, end.y + d.y * cap.radius, end.z + d.z * cap.radius),
                           k_invalid_vertex};
}

// ---- support(ConvexHullView): linear scan, strict-greater-wins, lowest-
// index argmax on ties (the deterministic substrate-wide tiebreak rule).
// O(n). The hill-climbing path (v2g) adds a warm-start `hint_vertex_idx`
// overload; the SIMD-batched path (v2h) parallelises the scan with `Vec8f`.
template <MathValue T>
[[nodiscard]] inline SupportPoint<T> support(const ConvexHullView<T>& hull, const Vec3<T>& dir) noexcept
{
    CRD_ASSERT(!hull.vertices.empty());
    Vec3<T> best = hull.vertices[0];
    T best_proj = crd::math::dot(best, dir);
    crd::u32 best_idx = 0U;
    for (crd::usize i = 1; i < hull.vertices.size(); ++i)
    {
        const T proj = crd::math::dot(hull.vertices[i], dir);
        if (proj > best_proj)
        {
            best_proj = proj;
            best = hull.vertices[i];
            best_idx = static_cast<crd::u32>(i);
        }
    }
    return SupportPoint<T>{best, best_idx};
}

// ---- v2g: hill-climbing hull support (PhysX/Havok pattern) -----------------
//
// `hill_climb_support` — best-neighbor walk on the hull's vertex adjacency
// graph. Starts at `start_idx` (the warm-start hint, typically the previous
// GJK iteration's argmax for this hull), then at each step moves to the
// neighbor with the largest `dot(vertex, dir)` projection. Converges to
// the global argmax in O(diameter) iterations on convex hulls — typically
// 1-3 iters when warm-started near the answer, vs O(N) for linear scan.
//
// **Determinism contract** (the load-bearing fix; ADR-0076 §4 pin #14):
// the returned `vertex_idx` must be the SAME as what `support(hull, dir)`
// (linear scan) would return for the same direction — the lowest-index
// vertex among all coincident extrema. Otherwise GJK's index-match
// termination breaks (the (vidx_a, vidx_b) pair would fail to repeat on
// converging directions because hill-climb might pick a different tied
// vertex than the linear scan did in a previous iter).
//
// The fix: after the walk converges to a local max, scan the converged
// vertex + ALL its neighbors with `proj` within `eps` of `best_proj`,
// pick the **lowest index** among them. Costs ~6 extra dot products (one
// per neighbor) in exchange for the bijection guarantee. Jolt does this;
// PhysX doesn't, and Jolt's contact stability is noticeably better.
template <MathValue T>
[[nodiscard]] inline SupportPoint<T> hill_climb_support(const ConvexHullView<T>& hull, const Vec3<T>& dir,
                                                         crd::u32 start_idx) noexcept
{
    CRD_ASSERT(!hull.vertices.empty());
    CRD_ASSERT(!hull.vertex_adjacency_offsets.empty());
    CRD_ASSERT(start_idx < hull.vertices.size());

    crd::u32 best = start_idx;
    T best_proj = crd::math::dot(hull.vertices[best], dir);

    // Best-neighbor walk: at each step, scan all neighbors of the current
    // best, find the one with the highest projection, advance to it if
    // strictly better. Converges when no neighbor improves.
    while (true)
    {
        const crd::u32 nb_begin = hull.vertex_adjacency_offsets[best];
        const crd::u32 nb_end = hull.vertex_adjacency_offsets[best + 1];
        crd::u32 best_neighbor = best;
        T best_neighbor_proj = best_proj;
        for (crd::u32 k = nb_begin; k < nb_end; ++k)
        {
            const crd::u32 j = hull.vertex_adjacency_indices[k];
            const T proj_j = crd::math::dot(hull.vertices[j], dir);
            // Strict-greater so a tie keeps the lower-index neighbor (the
            // post-walk tiebreak pass below covers global ties).
            if (proj_j > best_neighbor_proj)
            {
                best_neighbor = j;
                best_neighbor_proj = proj_j;
            }
        }
        if (best_neighbor == best)
        {
            break; // converged
        }
        best = best_neighbor;
        best_proj = best_neighbor_proj;
    }

    // Tiebreak pass: walk to the lowest-index vertex within the connected-
    // tied-vertex subgraph. For a convex polytope, tied vertices on the
    // same face are mutually connected (a face's polygon edges link
    // every pair of consecutive vertices on it). So walking neighbor-to-
    // neighbor among "same-projection" vertices converges to the lowest
    // index in that tied cluster — matching `support()`'s lowest-global-
    // index rule for the cases that actually occur on real hulls.
    //
    // The naive single-step tiebreak (only check direct neighbors of the
    // converged vertex) is NOT sufficient: on a cube queried along +X,
    // vertices {4, 5, 6, 7} are all tied; hill-climb from start=7 picks
    // vertex 5 (a neighbor with lower index, same proj), but vertex 4
    // (also tied, lower than 5) is NOT a direct neighbor of 7. The walk
    // form below catches this: 7→5→4, stops at 4.
    //
    // Worst-case cost: O(C) where C is the size of the tied cluster.
    // Typical: 1-4 steps (one face's vertices). Bounded by the polytope's
    // vertex count.
    //
    // Equality test is STRICT (`==`), not eps-based. Linear-scan `support`
    // uses strict-greater (`>`), so vertices with projections differing by
    // 1 ULP are treated as DISTINCT (the strictly-higher one wins). Eps-
    // based equality here would mis-classify those as tied and pick the
    // lower-index one — diverging from linear scan. Bit-exact equality
    // matches the contract: hill_climb_support returns the SAME vertex_idx
    // linear scan would.
    bool changed = true;
    while (changed)
    {
        changed = false;
        const crd::u32 nb_begin_t = hull.vertex_adjacency_offsets[best];
        const crd::u32 nb_end_t = hull.vertex_adjacency_offsets[best + 1];
        for (crd::u32 k = nb_begin_t; k < nb_end_t; ++k)
        {
            const crd::u32 j = hull.vertex_adjacency_indices[k];
            const T proj_j = crd::math::dot(hull.vertices[j], dir);
            if (proj_j == best_proj && j < best)
            {
                best = j;
                changed = true;
                break;
            }
        }
    }
    return SupportPoint<T>{hull.vertices[best], best};
}

// `support_with_hint(shape, dir, hint)` — dispatch surface that GJK calls
// in its hot loop. For shapes without adjacency-based fast paths (Sphere,
// OBB3, Capsule3, ConvexHullView without adjacency), it just delegates to
// the no-hint `support(shape, dir)`. For ConvexHullView WITH adjacency
// and a valid hint, it dispatches to `hill_climb_support`. The hint is
// the previous iteration's argmax for this shape (cached by the GJK
// driver across iterations).
//
// Generic template — matches any ConvexShape. The ConvexHullView overload
// below takes precedence when the shape is a ConvexHullView (more
// specific). Both must return the SAME vertex_idx as `support(s, dir)`
// for the same `dir` (the determinism contract; see hill_climb_support).
template <MathScalar T, typename S>
[[nodiscard]] inline SupportPoint<T> support_with_hint(const S& s, const Vec3<T>& dir,
                                                       crd::u32 /*hint_vertex_idx*/) noexcept
{
    return support(s, dir);
}

// ---- v2h: SIMD-batched hull support (AVX2 Vec8f) ---------------------------
//
// `support_simd_f32(hull, dir)` — declared here, defined out-of-line in
// `engine/geometry-primitives/src/hull_support_simd.cpp` so the AVX2 `Vec8f`
// instructions are emitted into a real .obj (the same pattern as v0f's
// `simd_batch.cpp`). Processes 8 vertices per `Vec8f` chunk; lane-wise dot
// product; per-chunk scalar reducer with strict-greater + lowest-index
// tiebreak — same determinism contract as `support(hull, dir)` linear scan.
//
// **SoA padding contract** (v2h): the input hull's `vx_soa`/`vy_soa`/
// `vz_soa` MUST be padded to multiple-of-8 by repeating vertex 0's
// coordinates in lanes `[n, padded)`. The padded lanes contribute
// `dot(vertex_0, dir)` projection — tied with lane 0 on `vertex_idx` 0
// winning by lowest-index tiebreak. Lets the scan be branch-free across
// all chunks; no `n_remaining` per-chunk handling.
//
// f32-only — the SIMD path is gated by `if constexpr (T == f32)` in the
// `support_with_hint` dispatch below. f64 hulls (v2i) fall through to the
// AoS linear scan / hill-climb.
[[nodiscard]] SupportPoint<crd::f32> support_simd_f32(const ConvexHullView<crd::f32>& hull,
                                                      const Vec3<crd::f32>& dir) noexcept;

// SIMD-vs-hill-climb dispatch threshold. Below this vertex count, SIMD-
// linear-scan beats hill-climb (no walk overhead, branch-free, straight-
// line Vec8f processing). Above, hill-climb wins (sub-linear in N with
// warm-start). Pinned at 32 per the v2h advisor's perf math (Zen 4
// reference); v2-close bench can retune.
inline constexpr crd::usize k_simd_support_threshold = 32;

// Specialised overload for ConvexHullView: routes to SIMD scan, hill-climb,
// or linear scan based on which optional facets the hull carries.
//
//   * SoA present + N ≤ 32 + T == f32 → SIMD-linear-scan (v2h).
//     SIMD wins on small hulls because there's no per-step branch and the
//     8-wide dot fits in one Vec8f mul-add chain.
//   * Adjacency present + valid hint → hill-climb (v2g).
//     Wins on larger hulls (≥ ~32 verts) because the walk converges in
//     O(diameter) ≈ O(log N) steps from a good warm-start.
//   * Else → AoS linear scan (v1h / v2a fallback).
//
// All three paths return the SAME `vertex_idx` for the SAME direction —
// the determinism contract. v2g tests pin this for hill-climb; v2h tests
// pin it for SIMD.
template <MathValue T>
[[nodiscard]] inline SupportPoint<T> support_with_hint(const ConvexHullView<T>& hull, const Vec3<T>& dir,
                                                       crd::u32 hint_vertex_idx) noexcept
{
    const bool have_soa = !hull.vx_soa.empty();
    const bool have_adjacency = !hull.vertex_adjacency_offsets.empty();
    const bool have_hint = (hint_vertex_idx != ~crd::u32{0}) && (hint_vertex_idx < hull.vertices.size());

    if constexpr (std::is_same_v<T, crd::f32>)
    {
        if (have_soa && hull.vertices.size() <= k_simd_support_threshold)
        {
            return support_simd_f32(hull, dir);
        }
    }
    if (have_adjacency && have_hint)
    {
        return hill_climb_support<T>(hull, dir, hint_vertex_idx);
    }
    return support(hull, dir);
}

// Point-in-hull: inside iff on the inner side of every face plane (faces are
// outward-facing, so `signed_distance <= epsilon` for all). Exact for a true
// convex hull; for an arbitrary face set this is the intersection of the
// half-spaces (still well-defined, just not "the mesh").
template <MathValue T>
[[nodiscard]] inline bool contains(const ConvexHullView<T>& hull, const Vec3<T>& point,
                                   T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    for (const Plane<T>& face : hull.faces)
    {
        if (signed_distance(face, point) > epsilon)
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
template <MathValue T> struct Line2
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
template <MathValue T> struct Segment2
{
    Vec2<T> a{};
    Vec2<T> b{};

    constexpr Segment2() noexcept = default;
    constexpr Segment2(const Vec2<T>& a_in, const Vec2<T>& b_in) noexcept : a(a_in), b(b_in) {}
};

// Ray from `origin` along `direction` (parameter t >= 0; direction need not be unit).
template <MathValue T> struct Ray2
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
template <MathValue T> struct AABB2
{
    Vec2<T> min{};
    Vec2<T> max{};

    constexpr AABB2() noexcept = default;
    constexpr AABB2(const Vec2<T>& min_in, const Vec2<T>& max_in) noexcept : min(min_in), max(max_in) {}
};

// Oriented bounding box (2D): center + half-extents in a local frame whose axes
// are the columns of `orientation` (orthonormal `Mat2`).
template <MathValue T> struct OBB2
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
template <MathValue T> struct Circle
{
    Vec2<T> center{};
    T radius = static_cast<T>(0);

    constexpr Circle() noexcept = default;
    constexpr Circle(const Vec2<T>& center_in, T radius_in) noexcept : center(center_in), radius(radius_in) {}
};

// Capsule (2D) — a "stadium" / discorectangle: segment a→b with a swept radius.
template <MathValue T> struct Capsule2
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
template <MathValue T> struct Cylinder2
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
template <MathValue T> struct Triangle2
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

template <MathValue T> [[nodiscard]] constexpr bool operator==(const Line2<T>& lhs, const Line2<T>& rhs) noexcept
{
    return lhs.point == rhs.point && lhs.direction == rhs.direction;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const Segment2<T>& lhs, const Segment2<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const Ray2<T>& lhs, const Ray2<T>& rhs) noexcept
{
    return lhs.origin == rhs.origin && lhs.direction == rhs.direction;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const AABB2<T>& lhs, const AABB2<T>& rhs) noexcept
{
    return lhs.min == rhs.min && lhs.max == rhs.max;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const OBB2<T>& lhs, const OBB2<T>& rhs) noexcept
{
    return lhs.center == rhs.center && lhs.half_extents == rhs.half_extents && lhs.orientation == rhs.orientation;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const Circle<T>& lhs, const Circle<T>& rhs) noexcept
{
    return lhs.center == rhs.center && lhs.radius == rhs.radius;
}
template <MathValue T> [[nodiscard]] constexpr bool operator==(const Capsule2<T>& lhs, const Capsule2<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.radius == rhs.radius;
}
template <MathValue T>
[[nodiscard]] constexpr bool operator==(const Cylinder2<T>& lhs, const Cylinder2<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.radius == rhs.radius;
}
template <MathValue T>
[[nodiscard]] constexpr bool operator==(const Triangle2<T>& lhs, const Triangle2<T>& rhs) noexcept
{
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c;
}

// ---- 2D helpers (peers of the 3D Ray/AABB/Sphere/Triangle helpers) ---------

template <MathValue T> [[nodiscard]] constexpr Vec2<T> point_at(const Ray2<T>& ray, T t) noexcept
{
    return ray.origin + ray.direction * t;
}

template <MathValue T> [[nodiscard]] constexpr Vec2<T> center(const AABB2<T>& bounds) noexcept
{
    return (bounds.min + bounds.max) * static_cast<T>(0.5);
}
template <MathValue T> [[nodiscard]] constexpr Vec2<T> extents(const AABB2<T>& bounds) noexcept
{
    return (bounds.max - bounds.min) * static_cast<T>(0.5);
}
template <MathValue T> [[nodiscard]] constexpr bool contains(const AABB2<T>& bounds, const Vec2<T>& point) noexcept
{
    return point.x >= bounds.min.x && point.x <= bounds.max.x && point.y >= bounds.min.y && point.y <= bounds.max.y;
}
template <MathValue T> [[nodiscard]] constexpr bool intersects(const AABB2<T>& lhs, const AABB2<T>& rhs) noexcept
{
    return lhs.min.x <= rhs.max.x && lhs.max.x >= rhs.min.x && lhs.min.y <= rhs.max.y && lhs.max.y >= rhs.min.y;
}
template <MathValue T>
[[nodiscard]] constexpr Vec2<T> positive_vertex(const AABB2<T>& bounds, const Vec2<T>& normal) noexcept
{
    return Vec2<T>(normal.x >= static_cast<T>(0) ? bounds.max.x : bounds.min.x,
                   normal.y >= static_cast<T>(0) ? bounds.max.y : bounds.min.y);
}

template <MathValue T> [[nodiscard]] constexpr bool contains(const Circle<T>& circle, const Vec2<T>& point) noexcept
{
    return crd::math::distance_squared(circle.center, point) <= circle.radius * circle.radius;
}
template <MathValue T> [[nodiscard]] constexpr bool intersects(const Circle<T>& lhs, const Circle<T>& rhs) noexcept
{
    const T r = lhs.radius + rhs.radius;
    return crd::math::distance_squared(lhs.center, rhs.center) <= r * r;
}
template <MathValue T>
[[nodiscard]] constexpr bool intersects(const AABB2<T>& bounds, const Circle<T>& circle) noexcept
{
    const Vec2<T> p(crd::math::clamp(circle.center.x, bounds.min.x, bounds.max.x),
                    crd::math::clamp(circle.center.y, bounds.min.y, bounds.max.y));
    return crd::math::distance_squared(p, circle.center) <= circle.radius * circle.radius;
}

template <MathValue T> [[nodiscard]] constexpr Vec2<T> centroid(const Triangle2<T>& tri) noexcept
{
    return (tri.a + tri.b + tri.c) / static_cast<T>(3);
}
// Signed area — positive when (a, b, c) wind counter-clockwise.
template <MathValue T> [[nodiscard]] constexpr T signed_area(const Triangle2<T>& tri) noexcept
{
    return static_cast<T>(0.5) * crd::math::cross(tri.b - tri.a, tri.c - tri.a);
}
// Barycentric coordinates (u, v, w) of `point` w.r.t. `tri` — same projection
// form as the 3D `barycentric`, valid for any non-degenerate triangle.
template <MathValue T>
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
template <MathValue T>
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
