#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — typed Quantity-overload kd queries (Phase 3.1.7 v5a;
// ADR-0078 §5 D27/D32-D36 — two-layer typed architecture).
//
// The raw `kd_radius<f32>` / `kd_nearest_n<f32>` / `kd_range_aabb<f32>` algo
// bodies operate on raw `f32` per the §5 D34 lower-layer rule. Consumers that
// author typed point clouds (`Vec3<Length32>` for SI metres, `Vec3<Length64>`
// for orbital-scale aerospace, future `Vec3<MicroLength32>` for PCB) reach
// for these wrappers: strip the Dim tag at the boundary, call the raw
// algorithm, re-tag the result with the original `D`.
//
// Zero runtime overhead — `to_raw_vec` / `from_raw_vec` are constexpr;
// `.value` accessors compile away. Element layout of `Vec3<Quantity<D, T>>` is
// bit-identical to `Vec3<T>` (ADR-0078 §2 D2 layout pin), so the typed point
// span is reinterpreted as a raw point span at the boundary at zero cost.
//
// Distance type: squared Length is `Quantity<DimMul<D, D>, T>` — the typed
// surface keeps everything in compile-time-known dimensions. Same convention
// as `mesh_closest_point` (v4a). No `DimRoot` needed: callers `sqrt_as<D>()`
// only when displaying.

#include <crd/containers/array.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/spatial/kd_nearest_n.hpp>
#include <crd/geometry/spatial/kd_radius.hpp>
#include <crd/geometry/spatial/kd_range_aabb.hpp>
#include <crd/geometry/spatial/kd_tree.hpp>
#include <crd/math/vec.hpp>
#include <crd/units/quantity.hpp>
#include <crd/units/quantity_aliases.hpp>

namespace crd::geometry::spatial
{

// ---------------------------------------------------------------------------
// Typed result hits — distance squared carries DimMul<D, D> (i.e. a typed Area
// when D = Length).
// ---------------------------------------------------------------------------

template <typename D, typename T> struct KdRadiusHitT
{
    crd::u32                                              payload{};
    crd::units::Quantity<crd::units::DimMul<D, D>, T>     distance_squared{};
};

template <typename D, typename T> struct KdNeighborT
{
    crd::u32                                              payload{};
    crd::units::Quantity<crd::units::DimMul<D, D>, T>     distance_squared{};
};

// ---------------------------------------------------------------------------
// Typed build — bridges through `to_raw_vec`-equivalent reinterpret.
// `Vec3<Quantity<D, T>>` is layout-identical to `Vec3<T>` (ADR-0078 §2 D2);
// we route via a raw view bridged from the same backing store. The caller
// owns BOTH buffers (typed view for type-checked code, raw span for the
// algorithm).
// ---------------------------------------------------------------------------

template <typename D, typename T>
[[nodiscard]] inline KdTree<T>
kd_build(crd::containers::ConstSpan<crd::math::Vec3<crd::units::Quantity<D, T>>> /*typed_points*/,
          crd::containers::ConstSpan<crd::math::Vec3<T>> raw_points,
          crd::memory::IAllocator* alloc,
          KdBuildOptions opts = {})
{
    return crd::geometry::spatial::kd_build<T>(raw_points, alloc, opts);
}

// ---------------------------------------------------------------------------
// Typed radius query.
// ---------------------------------------------------------------------------

template <typename D, typename T>
inline void
kd_radius(const KdTree<T>&                                                       tree,
           crd::containers::ConstSpan<crd::math::Vec3<crd::units::Quantity<D, T>>> /*typed_points*/,
           crd::containers::ConstSpan<crd::math::Vec3<T>>                         raw_points,
           const crd::math::Vec3<crd::units::Quantity<D, T>>&                     query,
           crd::units::Quantity<D, T>                                              radius,
           crd::containers::Array<KdRadiusHitT<D, T>>&                             out)
{
    // Scratch raw output. The query allocates onto this buffer; we re-tag
    // each hit at the boundary. Allocator: reuse `out`'s.
    crd::containers::Array<KdRadiusHit<T>> raw_out{out.allocator()};
    raw_out.reserve(out.capacity());
    crd::geometry::spatial::kd_radius<T>(
        tree, raw_points, crd::math::to_raw_vec(query), radius.value, raw_out);

    out.reserve(out.size() + raw_out.size());
    for (const auto& h : raw_out)
    {
        out.push_back(KdRadiusHitT<D, T>{
            h.payload,
            crd::units::Quantity<crd::units::DimMul<D, D>, T>{h.distance_squared}});
    }
}

// ---------------------------------------------------------------------------
// Typed k-NN query. `out`'s pre-allocated capacity is the `k`.
// ---------------------------------------------------------------------------

template <typename D, typename T>
inline void
kd_nearest_n(const KdTree<T>&                                                       tree,
              crd::containers::ConstSpan<crd::math::Vec3<crd::units::Quantity<D, T>>> /*typed_points*/,
              crd::containers::ConstSpan<crd::math::Vec3<T>>                         raw_points,
              const crd::math::Vec3<crd::units::Quantity<D, T>>&                     query,
              crd::usize                                                              k,
              crd::containers::Array<KdNeighborT<D, T>>&                              out)
{
    crd::containers::Array<KdNeighbor<T>> raw_out{out.allocator()};

    crd::geometry::spatial::kd_nearest_n<T>(
        tree, raw_points, crd::math::to_raw_vec(query), k, raw_out);

    out.clear();
    out.reserve(raw_out.size());
    for (const auto& h : raw_out)
    {
        out.push_back(KdNeighborT<D, T>{
            h.payload,
            crd::units::Quantity<crd::units::DimMul<D, D>, T>{h.distance_squared}});
    }
}

// ---------------------------------------------------------------------------
// Typed AABB-window range query. AABB carries Length-typed corners; payload
// indices come back raw u32 (the input ordering is the natural index).
// ---------------------------------------------------------------------------

template <typename D, typename T>
inline void
kd_range_aabb(const KdTree<T>&                                                       tree,
               crd::containers::ConstSpan<crd::math::Vec3<crd::units::Quantity<D, T>>> /*typed_points*/,
               crd::containers::ConstSpan<crd::math::Vec3<T>>                         raw_points,
               const crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>&    box,
               crd::containers::Array<crd::u32>&                                       out)
{
    const crd::geometry::primitives::AABB3<T> raw_box{
        crd::math::to_raw_vec(box.min), crd::math::to_raw_vec(box.max)};
    crd::geometry::spatial::kd_range_aabb<T>(tree, raw_points, raw_box, out);
}

} // namespace crd::geometry::spatial
