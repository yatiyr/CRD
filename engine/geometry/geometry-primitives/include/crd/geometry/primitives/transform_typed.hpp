#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives -- Typed transform boundary. Phase 3.1.7 v11
// (2026-05-19).
//
// Quantity-aware `transform_*_typed` wrappers covering every transform helper
// in `transform.hpp`. Strip-compute-retag pattern mirroring
// `queries_typed.hpp` (ADR-0078 §5 D34). Mat4/Mat3 stay raw (dimensionless
// world-to-world transform); shape input + output are typed.
//
// **D233 (planned for ADR-0076 §28)** -- Typed boundary mirrors
// `queries_typed.hpp` pattern. `detail_typed::strip(typed_shape)` already
// exists for most shapes; this header adds the missing 2D + Line3 + Frustum
// + Tetrahedron strips alongside the typed transform wrappers.
// ---------------------------------------------------------------------------

#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/queries_typed.hpp> // re-uses detail_typed::strip + retag
#include <crd/geometry/primitives/transform.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>
#include <crd/units/quantity_aliases.hpp>

namespace crd::geometry::primitives
{

namespace detail_typed
{

// Additional strips not in queries_typed.hpp (the 2D peers + Line3 +
// Tetrahedron + Frustum). Each is a one-liner; constexpr.

template <typename D, typename T>
[[nodiscard]] constexpr Line3<T> strip(const Line3<crd::units::Quantity<D, T>>& l) noexcept
{
    return Line3<T>{to_raw_vec(l.point), to_raw_vec(l.direction)};
}

template <typename D, typename T>
[[nodiscard]] constexpr Tetrahedron<T> strip(const Tetrahedron<crd::units::Quantity<D, T>>& t) noexcept
{
    return Tetrahedron<T>{to_raw_vec(t.a), to_raw_vec(t.b), to_raw_vec(t.c), to_raw_vec(t.d)};
}

template <typename D, typename T>
[[nodiscard]] inline Frustum<T> strip(const Frustum<crd::units::Quantity<D, T>>& f) noexcept
{
    Frustum<T> out;
    for (crd::usize i = 0U; i < 6U; ++i)
    {
        out.planes[i] = Plane<T>{to_raw_vec(f.planes[i].normal), f.planes[i].d.value};
    }
    return out;
}

// 2D strips.
template <typename D, typename T>
[[nodiscard]] constexpr AABB2<T> strip(const AABB2<crd::units::Quantity<D, T>>& b) noexcept
{
    return AABB2<T>{crd::math::Vec2<T>(b.min.x.value, b.min.y.value),
                     crd::math::Vec2<T>(b.max.x.value, b.max.y.value)};
}

template <typename D, typename T>
[[nodiscard]] constexpr OBB2<T> strip(const OBB2<crd::units::Quantity<D, T>>& b) noexcept
{
    return OBB2<T>{crd::math::Vec2<T>(b.center.x.value, b.center.y.value),
                    crd::math::Vec2<T>(b.half_extents.x.value, b.half_extents.y.value),
                    b.orientation};
}

template <typename D, typename T>
[[nodiscard]] constexpr Circle<T> strip(const Circle<crd::units::Quantity<D, T>>& c) noexcept
{
    return Circle<T>{crd::math::Vec2<T>(c.center.x.value, c.center.y.value), c.radius.value};
}

template <typename D, typename T>
[[nodiscard]] constexpr Capsule2<T> strip(const Capsule2<crd::units::Quantity<D, T>>& c) noexcept
{
    return Capsule2<T>{crd::math::Vec2<T>(c.a.x.value, c.a.y.value),
                        crd::math::Vec2<T>(c.b.x.value, c.b.y.value), c.radius.value};
}

template <typename D, typename T>
[[nodiscard]] constexpr Segment2<T> strip(const Segment2<crd::units::Quantity<D, T>>& s) noexcept
{
    return Segment2<T>{crd::math::Vec2<T>(s.a.x.value, s.a.y.value),
                        crd::math::Vec2<T>(s.b.x.value, s.b.y.value)};
}

template <typename D, typename T>
[[nodiscard]] constexpr Ray2<T> strip(const Ray2<crd::units::Quantity<D, T>>& r) noexcept
{
    return Ray2<T>{crd::math::Vec2<T>(r.origin.x.value, r.origin.y.value),
                    crd::math::Vec2<T>(r.direction.x.value, r.direction.y.value)};
}

template <typename D, typename T>
[[nodiscard]] constexpr Triangle2<T> strip(const Triangle2<crd::units::Quantity<D, T>>& t) noexcept
{
    return Triangle2<T>{crd::math::Vec2<T>(t.a.x.value, t.a.y.value),
                         crd::math::Vec2<T>(t.b.x.value, t.b.y.value),
                         crd::math::Vec2<T>(t.c.x.value, t.c.y.value)};
}

// Retag helpers (raw shape -> typed shape).

template <typename D, typename T>
[[nodiscard]] constexpr AABB3<crd::units::Quantity<D, T>> retag(const AABB3<T>& b) noexcept
{
    return AABB3<crd::units::Quantity<D, T>>{from_raw_vec<D>(b.min), from_raw_vec<D>(b.max)};
}

template <typename D, typename T>
[[nodiscard]] constexpr OBB3<crd::units::Quantity<D, T>> retag(const OBB3<T>& b) noexcept
{
    return OBB3<crd::units::Quantity<D, T>>{from_raw_vec<D>(b.center),
                                              from_raw_vec<D>(b.half_extents), b.orientation};
}

template <typename D, typename T>
[[nodiscard]] constexpr Sphere<crd::units::Quantity<D, T>> retag(const Sphere<T>& s) noexcept
{
    return Sphere<crd::units::Quantity<D, T>>{from_raw_vec<D>(s.center),
                                                crd::units::Quantity<D, T>{s.radius}};
}

template <typename D, typename T>
[[nodiscard]] constexpr Capsule3<crd::units::Quantity<D, T>> retag(const Capsule3<T>& c) noexcept
{
    return Capsule3<crd::units::Quantity<D, T>>{from_raw_vec<D>(c.a), from_raw_vec<D>(c.b),
                                                  crd::units::Quantity<D, T>{c.radius}};
}

template <typename D, typename T>
[[nodiscard]] constexpr Cylinder3<crd::units::Quantity<D, T>> retag(const Cylinder3<T>& c) noexcept
{
    return Cylinder3<crd::units::Quantity<D, T>>{from_raw_vec<D>(c.a), from_raw_vec<D>(c.b),
                                                   crd::units::Quantity<D, T>{c.radius}};
}

template <typename D, typename T>
[[nodiscard]] constexpr Triangle3<crd::units::Quantity<D, T>> retag(const Triangle3<T>& t) noexcept
{
    return Triangle3<crd::units::Quantity<D, T>>{from_raw_vec<D>(t.a), from_raw_vec<D>(t.b),
                                                   from_raw_vec<D>(t.c)};
}

template <typename D, typename T>
[[nodiscard]] constexpr Tetrahedron<crd::units::Quantity<D, T>> retag(const Tetrahedron<T>& t) noexcept
{
    return Tetrahedron<crd::units::Quantity<D, T>>{from_raw_vec<D>(t.a), from_raw_vec<D>(t.b),
                                                     from_raw_vec<D>(t.c), from_raw_vec<D>(t.d)};
}

template <typename D, typename T>
[[nodiscard]] constexpr Plane<crd::units::Quantity<D, T>> retag(const Plane<T>& p) noexcept
{
    return Plane<crd::units::Quantity<D, T>>{from_raw_vec<D>(p.normal),
                                               crd::units::Quantity<D, T>{p.d}};
}

template <typename D, typename T>
[[nodiscard]] constexpr Ray3<crd::units::Quantity<D, T>> retag(const Ray3<T>& r) noexcept
{
    return Ray3<crd::units::Quantity<D, T>>{from_raw_vec<D>(r.origin), from_raw_vec<D>(r.direction)};
}

template <typename D, typename T>
[[nodiscard]] constexpr Segment3<crd::units::Quantity<D, T>> retag(const Segment3<T>& s) noexcept
{
    return Segment3<crd::units::Quantity<D, T>>{from_raw_vec<D>(s.a), from_raw_vec<D>(s.b)};
}

template <typename D, typename T>
[[nodiscard]] constexpr Line3<crd::units::Quantity<D, T>> retag(const Line3<T>& l) noexcept
{
    return Line3<crd::units::Quantity<D, T>>{from_raw_vec<D>(l.point), from_raw_vec<D>(l.direction)};
}

template <typename D, typename T>
[[nodiscard]] inline Frustum<crd::units::Quantity<D, T>> retag(const Frustum<T>& f) noexcept
{
    Frustum<crd::units::Quantity<D, T>> out;
    for (crd::usize i = 0U; i < 6U; ++i)
    {
        out.planes[i] = Plane<crd::units::Quantity<D, T>>{from_raw_vec<D>(f.planes[i].normal),
                                                            crd::units::Quantity<D, T>{f.planes[i].d}};
    }
    return out;
}

// 2D retag helpers.
template <typename D, typename T>
[[nodiscard]] constexpr AABB2<crd::units::Quantity<D, T>> retag(const AABB2<T>& b) noexcept
{
    return AABB2<crd::units::Quantity<D, T>>{
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{b.min.x},
                                                      crd::units::Quantity<D, T>{b.min.y}),
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{b.max.x},
                                                      crd::units::Quantity<D, T>{b.max.y})};
}

template <typename D, typename T>
[[nodiscard]] constexpr OBB2<crd::units::Quantity<D, T>> retag(const OBB2<T>& b) noexcept
{
    return OBB2<crd::units::Quantity<D, T>>{
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{b.center.x},
                                                      crd::units::Quantity<D, T>{b.center.y}),
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{b.half_extents.x},
                                                      crd::units::Quantity<D, T>{b.half_extents.y}),
        b.orientation};
}

template <typename D, typename T>
[[nodiscard]] constexpr Circle<crd::units::Quantity<D, T>> retag(const Circle<T>& c) noexcept
{
    return Circle<crd::units::Quantity<D, T>>{
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{c.center.x},
                                                      crd::units::Quantity<D, T>{c.center.y}),
        crd::units::Quantity<D, T>{c.radius}};
}

template <typename D, typename T>
[[nodiscard]] constexpr Capsule2<crd::units::Quantity<D, T>> retag(const Capsule2<T>& c) noexcept
{
    return Capsule2<crd::units::Quantity<D, T>>{
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{c.a.x},
                                                      crd::units::Quantity<D, T>{c.a.y}),
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{c.b.x},
                                                      crd::units::Quantity<D, T>{c.b.y}),
        crd::units::Quantity<D, T>{c.radius}};
}

template <typename D, typename T>
[[nodiscard]] constexpr Segment2<crd::units::Quantity<D, T>> retag(const Segment2<T>& s) noexcept
{
    return Segment2<crd::units::Quantity<D, T>>{
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{s.a.x},
                                                      crd::units::Quantity<D, T>{s.a.y}),
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{s.b.x},
                                                      crd::units::Quantity<D, T>{s.b.y})};
}

template <typename D, typename T>
[[nodiscard]] constexpr Ray2<crd::units::Quantity<D, T>> retag(const Ray2<T>& r) noexcept
{
    return Ray2<crd::units::Quantity<D, T>>{
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{r.origin.x},
                                                      crd::units::Quantity<D, T>{r.origin.y}),
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{r.direction.x},
                                                      crd::units::Quantity<D, T>{r.direction.y})};
}

template <typename D, typename T>
[[nodiscard]] constexpr Triangle2<crd::units::Quantity<D, T>> retag(const Triangle2<T>& t) noexcept
{
    return Triangle2<crd::units::Quantity<D, T>>{
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{t.a.x},
                                                      crd::units::Quantity<D, T>{t.a.y}),
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{t.b.x},
                                                      crd::units::Quantity<D, T>{t.b.y}),
        crd::math::Vec2<crd::units::Quantity<D, T>>(crd::units::Quantity<D, T>{t.c.x},
                                                      crd::units::Quantity<D, T>{t.c.y})};
}

} // namespace detail_typed

// ---------------------------------------------------------------------------
// Typed 3D transforms.
// ---------------------------------------------------------------------------

template <typename D, typename T>
[[nodiscard]] inline AABB3<crd::units::Quantity<D, T>>
transform_aabb_typed(const crd::math::Mat4<T>& m,
                      const AABB3<crd::units::Quantity<D, T>>& box) noexcept
{
    return detail_typed::retag<D, T>(transform_aabb(m, detail_typed::strip(box)));
}

template <typename D, typename T>
[[nodiscard]] inline OBB3<crd::units::Quantity<D, T>>
transform_obb_typed(const crd::math::Mat4<T>& m,
                     const OBB3<crd::units::Quantity<D, T>>& obb) noexcept
{
    return detail_typed::retag<D, T>(transform_obb(m, detail_typed::strip(obb)));
}

template <typename D, typename T>
[[nodiscard]] inline Sphere<crd::units::Quantity<D, T>>
transform_sphere_typed(const crd::math::Mat4<T>& m,
                        const Sphere<crd::units::Quantity<D, T>>& s) noexcept
{
    return detail_typed::retag<D, T>(transform_sphere(m, detail_typed::strip(s)));
}

template <typename D, typename T>
[[nodiscard]] inline Capsule3<crd::units::Quantity<D, T>>
transform_capsule3_typed(const crd::math::Mat4<T>& m,
                          const Capsule3<crd::units::Quantity<D, T>>& c) noexcept
{
    return detail_typed::retag<D, T>(transform_capsule3(m, detail_typed::strip(c)));
}

template <typename D, typename T>
[[nodiscard]] inline Cylinder3<crd::units::Quantity<D, T>>
transform_cylinder3_typed(const crd::math::Mat4<T>& m,
                           const Cylinder3<crd::units::Quantity<D, T>>& c) noexcept
{
    return detail_typed::retag<D, T>(transform_cylinder3(m, detail_typed::strip(c)));
}

template <typename D, typename T>
[[nodiscard]] inline Triangle3<crd::units::Quantity<D, T>>
transform_triangle3_typed(const crd::math::Mat4<T>& m,
                           const Triangle3<crd::units::Quantity<D, T>>& t) noexcept
{
    return detail_typed::retag<D, T>(transform_triangle3(m, detail_typed::strip(t)));
}

template <typename D, typename T>
[[nodiscard]] inline Tetrahedron<crd::units::Quantity<D, T>>
transform_tetrahedron_typed(const crd::math::Mat4<T>& m,
                             const Tetrahedron<crd::units::Quantity<D, T>>& t) noexcept
{
    return detail_typed::retag<D, T>(transform_tetrahedron(m, detail_typed::strip(t)));
}

template <typename D, typename T>
[[nodiscard]] inline Plane<crd::units::Quantity<D, T>>
transform_plane_typed(const crd::math::Mat4<T>& m,
                       const Plane<crd::units::Quantity<D, T>>& p) noexcept
{
    return detail_typed::retag<D, T>(transform_plane(m, detail_typed::strip(p)));
}

template <typename D, typename T>
[[nodiscard]] inline Ray3<crd::units::Quantity<D, T>>
transform_ray3_typed(const crd::math::Mat4<T>& m,
                      const Ray3<crd::units::Quantity<D, T>>& r) noexcept
{
    return detail_typed::retag<D, T>(transform_ray3(m, detail_typed::strip(r)));
}

template <typename D, typename T>
[[nodiscard]] inline Ray3<crd::units::Quantity<D, T>>
transform_ray3_to_local_typed(const Ray3<crd::units::Quantity<D, T>>& world_ray,
                                const crd::math::Mat4<T>& world_to_local) noexcept
{
    return detail_typed::retag<D, T>(transform_ray3_to_local(detail_typed::strip(world_ray), world_to_local));
}

template <typename D, typename T>
[[nodiscard]] inline Segment3<crd::units::Quantity<D, T>>
transform_segment3_typed(const crd::math::Mat4<T>& m,
                          const Segment3<crd::units::Quantity<D, T>>& s) noexcept
{
    return detail_typed::retag<D, T>(transform_segment3(m, detail_typed::strip(s)));
}

template <typename D, typename T>
[[nodiscard]] inline Line3<crd::units::Quantity<D, T>>
transform_line3_typed(const crd::math::Mat4<T>& m,
                       const Line3<crd::units::Quantity<D, T>>& l) noexcept
{
    return detail_typed::retag<D, T>(transform_line3(m, detail_typed::strip(l)));
}

template <typename D, typename T>
[[nodiscard]] inline Frustum<crd::units::Quantity<D, T>>
transform_frustum_typed(const crd::math::Mat4<T>& m,
                         const Frustum<crd::units::Quantity<D, T>>& f) noexcept
{
    return detail_typed::retag<D, T>(transform_frustum(m, detail_typed::strip(f)));
}

// ---------------------------------------------------------------------------
// Typed 2D transforms.
// ---------------------------------------------------------------------------

template <typename D, typename T>
[[nodiscard]] inline AABB2<crd::units::Quantity<D, T>>
transform_aabb2_typed(const crd::math::Mat3<T>& m,
                       const AABB2<crd::units::Quantity<D, T>>& box) noexcept
{
    return detail_typed::retag<D, T>(transform_aabb2(m, detail_typed::strip(box)));
}

template <typename D, typename T>
[[nodiscard]] inline OBB2<crd::units::Quantity<D, T>>
transform_obb2_typed(const crd::math::Mat3<T>& m,
                      const OBB2<crd::units::Quantity<D, T>>& obb) noexcept
{
    return detail_typed::retag<D, T>(transform_obb2(m, detail_typed::strip(obb)));
}

template <typename D, typename T>
[[nodiscard]] inline Circle<crd::units::Quantity<D, T>>
transform_circle_typed(const crd::math::Mat3<T>& m,
                        const Circle<crd::units::Quantity<D, T>>& c) noexcept
{
    return detail_typed::retag<D, T>(transform_circle(m, detail_typed::strip(c)));
}

template <typename D, typename T>
[[nodiscard]] inline Capsule2<crd::units::Quantity<D, T>>
transform_capsule2_typed(const crd::math::Mat3<T>& m,
                          const Capsule2<crd::units::Quantity<D, T>>& c) noexcept
{
    return detail_typed::retag<D, T>(transform_capsule2(m, detail_typed::strip(c)));
}

template <typename D, typename T>
[[nodiscard]] inline Segment2<crd::units::Quantity<D, T>>
transform_segment2_typed(const crd::math::Mat3<T>& m,
                          const Segment2<crd::units::Quantity<D, T>>& s) noexcept
{
    return detail_typed::retag<D, T>(transform_segment2(m, detail_typed::strip(s)));
}

template <typename D, typename T>
[[nodiscard]] inline Ray2<crd::units::Quantity<D, T>>
transform_ray2_typed(const crd::math::Mat3<T>& m,
                      const Ray2<crd::units::Quantity<D, T>>& r) noexcept
{
    return detail_typed::retag<D, T>(transform_ray2(m, detail_typed::strip(r)));
}

template <typename D, typename T>
[[nodiscard]] inline Triangle2<crd::units::Quantity<D, T>>
transform_triangle2_typed(const crd::math::Mat3<T>& m,
                           const Triangle2<crd::units::Quantity<D, T>>& t) noexcept
{
    return detail_typed::retag<D, T>(transform_triangle2(m, detail_typed::strip(t)));
}

} // namespace crd::geometry::primitives
