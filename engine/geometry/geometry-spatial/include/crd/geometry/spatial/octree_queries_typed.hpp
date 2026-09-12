#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — typed Quantity-overload LooseOctree queries
// (Phase 3.1.7 v5b; ADR-0078 §5 D27/D32-D36 — two-layer typed architecture).
//
// Raw `LooseOctree<f32>::overlap` / `raycast` operate on raw `f32` per the
// §5 D34 lower-layer rule. Consumers that author typed AABBs / rays
// (`AABB3<Length32>`, scene world-AABBs from `crd-scene` v5-index-bringup)
// reach for these wrappers: strip the Dim tag at the boundary, call the raw
// algorithm, re-tag the typed result fields.
//
// Zero overhead — `to_raw_vec` / `from_raw_vec` are constexpr; `.value`
// accessors compile away. Element layout of `Vec3<Quantity<D, T>>` is
// bit-identical to `Vec3<T>` (ADR-0078 §2 D2 layout pin).
//
// Same pattern as `kd_queries_typed.hpp` (v5a) and `mesh_queries_typed.hpp` (v4).

#include <crd/containers/array.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/result_types.hpp>
#include <crd/geometry/spatial/loose_octree.hpp>
#include <crd/math/vec.hpp>
#include <crd/units/quantity.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <optional>

namespace crd::geometry::spatial
{

// Typed raycast hit — `t` carries the typed length (parametric distance along
// a dimensionless unit-direction ray equals length).
template <typename D, typename T>
struct OctreeRayHitT
{
    crd::units::Quantity<D, T> t{};
    crd::u32                    payload{};
};

// Typed Ray3 — origin = `Vec3<Quantity<D,T>>`, direction = dimensionless `Vec3<T>`.
template <typename D, typename T>
struct OctreeRay3T
{
    crd::math::Vec3<crd::units::Quantity<D, T>> origin{};
    crd::math::Vec3<T>                           direction{};
};

// Typed insert — accepts an `AABB3<Quantity<D,T>>` at the API surface, strips
// to raw on the way through. Returns the same `OctreeObjectId` as the raw form.
template <typename D, typename T>
[[nodiscard]] inline OctreeObjectId
octree_insert(LooseOctree<T>&                                                       tree,
               const crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>&  typed_aabb,
               crd::u32                                                              payload)
{
    const crd::geometry::primitives::AABB3<T> raw_aabb{
        crd::math::to_raw_vec(typed_aabb.min), crd::math::to_raw_vec(typed_aabb.max)};
    return tree.insert(raw_aabb, payload);
}

// Typed update.
template <typename D, typename T>
inline bool
octree_update(LooseOctree<T>&                                                       tree,
               OctreeObjectId                                                        id,
               const crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>&  typed_new_aabb)
{
    const crd::geometry::primitives::AABB3<T> raw_aabb{
        crd::math::to_raw_vec(typed_new_aabb.min), crd::math::to_raw_vec(typed_new_aabb.max)};
    return tree.update(id, raw_aabb);
}

// Typed overlap (callback form).
template <typename D, typename T, typename Fn>
inline void
octree_overlap(const LooseOctree<T>&                                                tree,
                const crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>& typed_query,
                Fn&&                                                                  on_hit)
{
    const crd::geometry::primitives::AABB3<T> raw_query{
        crd::math::to_raw_vec(typed_query.min), crd::math::to_raw_vec(typed_query.max)};
    tree.overlap(raw_query, static_cast<Fn&&>(on_hit));
}

// Typed overlap (Array sink form).
template <typename D, typename T>
inline void
octree_overlap(const LooseOctree<T>&                                                tree,
                const crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>& typed_query,
                crd::containers::Array<crd::u32>&                                     out)
{
    const crd::geometry::primitives::AABB3<T> raw_query{
        crd::math::to_raw_vec(typed_query.min), crd::math::to_raw_vec(typed_query.max)};
    tree.overlap(raw_query, out);
}

// Typed raycast — typed ray in, typed RayHit out (or std::nullopt).
template <typename D, typename T>
[[nodiscard]] inline std::optional<OctreeRayHitT<D, T>>
octree_raycast(const LooseOctree<T>&         tree,
                const OctreeRay3T<D, T>&     typed_ray,
                crd::units::Quantity<D, T>   tmax = crd::units::Quantity<D, T>{
                    std::numeric_limits<T>::infinity()})
{
    const crd::geometry::primitives::Ray3<T> raw_ray{
        crd::math::to_raw_vec(typed_ray.origin), typed_ray.direction};
    const auto raw_hit = tree.raycast(raw_ray, tmax.value);
    if (!raw_hit.has_value()) { return std::nullopt; }
    return OctreeRayHitT<D, T>{crd::units::Quantity<D, T>{raw_hit->t}, raw_hit->payload};
}

} // namespace crd::geometry::spatial
