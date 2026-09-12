#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — typed Quantity-overload R*-tree wrappers
// (Phase 3.1.7 v5c; ADR-0078 §5 D27/D32-D36 — two-layer typed architecture).
//
// Strip-compute-retag wrappers per ADR-0078 §5 D34. Same pattern as
// `kd_queries_typed.hpp` (v5a) and `octree_queries_typed.hpp` (v5b).

#include <crd/containers/array.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/result_types.hpp>
#include <crd/geometry/spatial/rtree.hpp>
#include <crd/math/vec.hpp>
#include <crd/units/quantity.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <optional>

namespace crd::geometry::spatial
{

template <typename D, typename T>
struct RTreeRayHitT
{
    crd::units::Quantity<D, T> t{};
    crd::u32                    payload{};
};

template <typename D, typename T>
struct RTreeRay3T
{
    crd::math::Vec3<crd::units::Quantity<D, T>> origin{};
    crd::math::Vec3<T>                           direction{};
};

template <typename D, typename T>
struct RTreeNeighborT
{
    crd::u32                                              payload{};
    crd::units::Quantity<crd::units::DimMul<D, D>, T>     distance_squared{};
};

template <typename D, typename T>
[[nodiscard]] inline RTreeLeafId
rtree_insert(RTree<T>&                                                              tree,
              const crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>&  typed_aabb,
              crd::u32                                                              payload)
{
    const crd::geometry::primitives::AABB3<T> raw_aabb{
        crd::math::to_raw_vec(typed_aabb.min), crd::math::to_raw_vec(typed_aabb.max)};
    return tree.insert(raw_aabb, payload);
}

template <typename D, typename T, typename Fn>
inline void
rtree_overlap(const RTree<T>&                                                       tree,
               const crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>& typed_query,
               Fn&&                                                                  on_hit)
{
    const crd::geometry::primitives::AABB3<T> raw_query{
        crd::math::to_raw_vec(typed_query.min), crd::math::to_raw_vec(typed_query.max)};
    tree.overlap(raw_query, static_cast<Fn&&>(on_hit));
}

template <typename D, typename T>
inline void
rtree_overlap(const RTree<T>&                                                       tree,
               const crd::geometry::primitives::AABB3<crd::units::Quantity<D, T>>& typed_query,
               crd::containers::Array<crd::u32>&                                     out)
{
    const crd::geometry::primitives::AABB3<T> raw_query{
        crd::math::to_raw_vec(typed_query.min), crd::math::to_raw_vec(typed_query.max)};
    tree.overlap(raw_query, out);
}

template <typename D, typename T>
[[nodiscard]] inline std::optional<RTreeRayHitT<D, T>>
rtree_raycast(const RTree<T>&             tree,
               const RTreeRay3T<D, T>&    typed_ray,
               crd::units::Quantity<D, T> tmax = crd::units::Quantity<D, T>{
                   std::numeric_limits<T>::infinity()})
{
    const crd::geometry::primitives::Ray3<T> raw_ray{
        crd::math::to_raw_vec(typed_ray.origin), typed_ray.direction};
    const auto raw_hit = tree.raycast(raw_ray, tmax.value);
    if (!raw_hit.has_value()) { return std::nullopt; }
    return RTreeRayHitT<D, T>{crd::units::Quantity<D, T>{raw_hit->t}, raw_hit->payload};
}

template <typename D, typename T>
inline void
rtree_nearest_n(const RTree<T>&                                              tree,
                 const crd::math::Vec3<crd::units::Quantity<D, T>>&          typed_query,
                 crd::usize                                                   k,
                 crd::containers::Array<RTreeNeighborT<D, T>>&                out)
{
    crd::containers::Array<typename RTree<T>::Neighbor> raw_out{out.allocator()};
    tree.nearest_n(crd::math::to_raw_vec(typed_query), k, raw_out);
    out.clear();
    out.reserve(raw_out.size());
    for (const auto& n : raw_out)
    {
        out.push_back(RTreeNeighborT<D, T>{
            n.payload,
            crd::units::Quantity<crd::units::DimMul<D, D>, T>{n.distance_squared}});
    }
}

} // namespace crd::geometry::spatial
