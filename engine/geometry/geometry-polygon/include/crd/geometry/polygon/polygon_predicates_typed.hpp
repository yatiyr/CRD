#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-polygon — typed Quantity-overload wrappers (Phase 3.1.7 v6a;
// ADR-0078 §5 D27/D32-D36 — two-layer typed architecture).
//
// The raw `polygon_predicates.hpp` algorithms operate on `MathScalar T`
// (`f32`/`f64`) per the §5 D34 lower-layer rule. Consumers that author typed
// polygons (`Vec2<Length32>` for SI metres, `Vec2<Length64>` for cartography,
// future `Vec2<MicroLength32>` for PCB) reach for these wrappers: strip the
// Dim tag at the boundary, call the raw algorithm, re-tag the result with
// the original `D`.
//
// Zero runtime overhead — element layout of `Vec2<Quantity<D, T>>` is
// bit-identical to `Vec2<T>` (ADR-0078 §2 D2 layout pin), so the typed point
// span is reinterpreted as a raw span at the boundary at zero cost.
//
// Area type: `signed_area` returns `Quantity<DimMul<D, D>, T>` — typed Area
// when D = Length. Same convention as `mesh_closest_point` (v4a).
// ---------------------------------------------------------------------------

#include <crd/geometry/polygon/polygon_predicates.hpp>
#include <crd/geometry/polygon/polygon_types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/units/quantity.hpp>
#include <crd/units/quantity_aliases.hpp>

namespace crd::geometry::polygon
{

// ---- Typed signed_area --------------------------------------------------
//
// Layout pin (ADR-0078 §2 D2): `Vec2<Quantity<D, T>>` is bit-identical to
// `Vec2<T>` — same element layout, same alignment. We `reinterpret_cast` the
// typed span to a raw span at the boundary, run the raw algorithm, retag
// the result. Zero codegen overhead — verified by `MeshClosestPointTyped`
// pattern in v4a.

namespace polygon_typed_detail
{
template <typename D, typename T>
[[nodiscard]] inline crd::containers::ConstSpan<crd::math::Vec2<T>>
strip_typed_span(crd::containers::ConstSpan<crd::math::Vec2<crd::units::Quantity<D, T>>> typed) noexcept
{
    static_assert(sizeof(crd::math::Vec2<crd::units::Quantity<D, T>>) == sizeof(crd::math::Vec2<T>),
                  "Vec2<Quantity<D,T>> must be layout-identical to Vec2<T> (ADR-0078 §2 D2)");
    return crd::containers::ConstSpan<crd::math::Vec2<T>>{
        reinterpret_cast<const crd::math::Vec2<T>*>(typed.data()), typed.size()};
}
} // namespace polygon_typed_detail

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<crd::units::DimMul<D, D>, T>
signed_area(crd::containers::ConstSpan<crd::math::Vec2<crd::units::Quantity<D, T>>> typed_ring) noexcept
{
    const Ring2<T> raw{polygon_typed_detail::strip_typed_span(typed_ring)};
    return crd::units::Quantity<crd::units::DimMul<D, D>, T>{signed_area(raw)};
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<crd::units::DimMul<D, D>, T>
signed_area_typed(PolygonView2<T>                                     raw_view,
                  crd::units::Quantity<D, T> /*dim_marker_for_overload*/) noexcept
{
    // For owning Polygon2<Q>/PolygonView2<Q> we'd template on a typed view;
    // for v6a the explicit-ring overload above covers the common path.
    return crd::units::Quantity<crd::units::DimMul<D, D>, T>{signed_area(raw_view)};
}

// ---- Typed centroid ------------------------------------------------------

template <typename D, typename T>
[[nodiscard]] inline crd::math::Vec2<crd::units::Quantity<D, T>>
centroid(crd::containers::ConstSpan<crd::math::Vec2<crd::units::Quantity<D, T>>> typed_ring) noexcept
{
    const Ring2<T> raw{polygon_typed_detail::strip_typed_span(typed_ring)};
    const auto     c = centroid(raw);
    return crd::math::Vec2<crd::units::Quantity<D, T>>{
        crd::units::Quantity<D, T>{c.x}, crd::units::Quantity<D, T>{c.y}};
}

// ---- Typed aabb ---------------------------------------------------------

template <typename D, typename T>
[[nodiscard]] inline crd::geometry::primitives::AABB2<crd::units::Quantity<D, T>>
aabb(crd::containers::ConstSpan<crd::math::Vec2<crd::units::Quantity<D, T>>> typed_ring) noexcept
{
    using Q  = crd::units::Quantity<D, T>;
    using BB = crd::geometry::primitives::AABB2<Q>;
    const Ring2<T> raw{polygon_typed_detail::strip_typed_span(typed_ring)};
    const auto     b = aabb(raw);
    return BB{crd::math::Vec2<Q>{Q{b.min.x}, Q{b.min.y}},
              crd::math::Vec2<Q>{Q{b.max.x}, Q{b.max.y}}};
}

// ---- Typed point_in_ring / point_in_polygon ------------------------------

template <typename D, typename T>
[[nodiscard]] inline PointInPolygon
point_in_ring(crd::containers::ConstSpan<crd::math::Vec2<crd::units::Quantity<D, T>>> typed_ring,
              const crd::math::Vec2<crd::units::Quantity<D, T>>&                      query) noexcept
{
    const Ring2<T> raw{polygon_typed_detail::strip_typed_span(typed_ring)};
    const crd::math::Vec2<T> q{query.x.value, query.y.value};
    return point_in_ring(raw, q);
}

// ---- Typed is_ccw / is_simple --------------------------------------------

template <typename D, typename T>
[[nodiscard]] inline bool
is_ccw(crd::containers::ConstSpan<crd::math::Vec2<crd::units::Quantity<D, T>>> typed_ring) noexcept
{
    return is_ccw(Ring2<T>{polygon_typed_detail::strip_typed_span(typed_ring)});
}

template <typename D, typename T>
[[nodiscard]] inline bool
is_simple(crd::containers::ConstSpan<crd::math::Vec2<crd::units::Quantity<D, T>>> typed_ring) noexcept
{
    return is_simple(Ring2<T>{polygon_typed_detail::strip_typed_span(typed_ring)});
}

} // namespace crd::geometry::polygon
