#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — typed Quantity-overload UniformGrid wrappers
// (Phase 3.1.7 v5e; ADR-0078 §5 D27/D32-D36 — two-layer typed architecture).

#include <crd/containers/array.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/result_types.hpp>
#include <crd/geometry/spatial/uniform_grid.hpp>
#include <crd/math/vec.hpp>
#include <crd/units/quantity.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <optional>

namespace crd::geometry::spatial
{

template <typename D, typename T>
struct UniformGridRayHitT
{
    crd::units::Quantity<D, T> t{};
    crd::u32                    payload{};
};

template <typename D, typename T>
struct UniformGridRay3T
{
    crd::math::Vec3<crd::units::Quantity<D, T>> origin{};
    crd::math::Vec3<T>                           direction{};
};

template <typename D, typename T>
[[nodiscard]] inline UniformGridObjectId
uniform_grid_insert(UniformGrid<T>&                                                       tree,
                     const crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>& typed_aabb,
                     crd::u32                                                              payload)
{
    const crd::geometry::primitives::AABB3<T> raw_aabb{
        crd::math::to_raw_vec(typed_aabb.min), crd::math::to_raw_vec(typed_aabb.max)};
    return tree.insert(raw_aabb, payload);
}

template <typename D, typename T>
inline bool
uniform_grid_update(UniformGrid<T>&                                                       tree,
                     UniformGridObjectId                                                   id,
                     const crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>& typed_new_aabb)
{
    const crd::geometry::primitives::AABB3<T> raw_aabb{
        crd::math::to_raw_vec(typed_new_aabb.min), crd::math::to_raw_vec(typed_new_aabb.max)};
    return tree.update(id, raw_aabb);
}

template <typename D, typename T, typename Fn>
inline void
uniform_grid_overlap(const UniformGrid<T>&                                                tree,
                      const crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>& typed_query,
                      Fn&&                                                                  on_hit)
{
    const crd::geometry::primitives::AABB3<T> raw_query{
        crd::math::to_raw_vec(typed_query.min), crd::math::to_raw_vec(typed_query.max)};
    tree.overlap(raw_query, static_cast<Fn&&>(on_hit));
}

template <typename D, typename T>
inline void
uniform_grid_overlap(const UniformGrid<T>&                                                tree,
                      const crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>& typed_query,
                      crd::containers::Array<crd::u32>&                                     out)
{
    const crd::geometry::primitives::AABB3<T> raw_query{
        crd::math::to_raw_vec(typed_query.min), crd::math::to_raw_vec(typed_query.max)};
    tree.overlap(raw_query, out);
}

template <typename D, typename T, typename Fn>
inline void
uniform_grid_radius(const UniformGrid<T>&                                       tree,
                     const crd::math::Vec3<crd::units::Quantity<D, T>>&         typed_point,
                     crd::units::Quantity<D, T>                                  r,
                     Fn&&                                                         on_hit)
{
    tree.radius(crd::math::to_raw_vec(typed_point), r.value, static_cast<Fn&&>(on_hit));
}

template <typename D, typename T>
inline void
uniform_grid_radius(const UniformGrid<T>&                              tree,
                     const crd::math::Vec3<crd::units::Quantity<D, T>>& typed_point,
                     crd::units::Quantity<D, T>                         r,
                     crd::containers::Array<crd::u32>&                  out)
{
    tree.radius(crd::math::to_raw_vec(typed_point), r.value, out);
}

template <typename D, typename T>
[[nodiscard]] inline std::optional<UniformGridRayHitT<D, T>>
uniform_grid_raycast(const UniformGrid<T>&            tree,
                      const UniformGridRay3T<D, T>&   typed_ray,
                      crd::units::Quantity<D, T>      tmax = crd::units::Quantity<D, T>{
                          std::numeric_limits<T>::infinity()})
{
    const crd::geometry::primitives::Ray3<T> raw_ray{
        crd::math::to_raw_vec(typed_ray.origin), typed_ray.direction};
    const auto raw_hit = tree.raycast(raw_ray, tmax.value);
    if (!raw_hit.has_value()) { return std::nullopt; }
    return UniformGridRayHitT<D, T>{crd::units::Quantity<D, T>{raw_hit->t}, raw_hit->payload};
}

} // namespace crd::geometry::spatial
