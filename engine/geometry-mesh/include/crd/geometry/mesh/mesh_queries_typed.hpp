#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh — typed Quantity-overload mesh queries (Phase 3.1.7 v4a;
// ADR-0078 §5 D27/D32-D36 — two-layer typed architecture).
//
// The raw `mesh_closest_point(TriangleMeshViewf, ...)` algorithm operates on
// raw `f32` per the §5 D34 lower-layer rule. Consumers that author typed
// `Vec3<Length32>` meshes (eylem TriangleMeshCollider, future cad surfaces)
// reach for these wrappers: strip the Dim tag at the boundary, call the raw
// algorithm, re-tag the result with the original `D`.
//
// Zero runtime overhead — `to_raw_vec` / `from_raw_vec` are constexpr;
// `.value` accessors compile away.

#include <crd/geometry/mesh/mesh_bvh.hpp>
#include <crd/geometry/mesh/mesh_closest_point.hpp>
#include <crd/geometry/mesh/mesh_raycast.hpp>
#include <crd/geometry/mesh/mesh_winding_number.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
#include <crd/math/vec.hpp>
#include <crd/units/quantity.hpp>
#include <crd/units/quantity_aliases.hpp>

#include <optional>

namespace crd::geometry::mesh
{

// Typed view: vertices carry a Length dim, indices stay raw.
template <typename D, typename T> struct TriangleMeshViewT
{
    crd::containers::ConstSpan<crd::math::Vec3<crd::units::Quantity<D, T>>> vertices{};
    crd::containers::ConstSpan<crd::u32>                                     indices{};
};

// Typed closest-point result: payload (triangle index) stays raw u32;
// point carries Length; distance carries Length²  (= DimMul<D, D>).
template <typename D, typename T> struct MeshClosestPointT
{
    crd::math::Vec3<crd::units::Quantity<D, T>>                        point{};
    crd::units::Quantity<crd::units::DimMul<D, D>, T>                  distance_squared{};
    crd::u32                                                            payload{};
};

// Build the BVH from a typed view by stripping vertices at the boundary.
// Caller owns the raw `Vec3<T>` storage backing `raw_vertices`; the typed
// view's `vertices` span MUST be `from_raw_vec`-bridged from the same
// backing buffer so element layout is bit-identical (the typed and raw
// Vec3 have the same `sizeof` per ADR-0078 §2 D2 layout pin).
template <typename D, typename T>
[[nodiscard]] inline TriangleMeshBvh
build_triangle_mesh_bvh(const TriangleMeshViewT<D, T>& view,
                          crd::containers::ConstSpan<crd::math::Vec3<T>> raw_vertices,
                          crd::memory::IAllocator* alloc)
{
    TriangleMeshView<T> raw_view{raw_vertices, view.indices};
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        return crd::geometry::mesh::build_triangle_mesh_bvh(raw_view, alloc);
    }
    else
    {
        static_assert(std::is_same_v<T, crd::f32>,
                      "TriangleMeshBvh currently builds against f32 vertices only — "
                      "f64 path lands when a consumer surfaces.");
        return TriangleMeshBvh{alloc};
    }
}

// Typed closest-point query. Bridges through the raw algorithm.
template <typename D, typename T>
[[nodiscard]] inline std::optional<MeshClosestPointT<D, T>>
mesh_closest_point(const TriangleMeshViewT<D, T>& typed_view,
                   crd::containers::ConstSpan<crd::math::Vec3<T>> raw_vertices,
                   const TriangleMeshBvh& bvh,
                   const crd::math::Vec3<crd::units::Quantity<D, T>>& query) noexcept
{
    static_assert(std::is_same_v<T, crd::f32>,
                  "mesh_closest_point typed wrapper is f32 only at v4a.");
    TriangleMeshView<T> raw_view{raw_vertices, typed_view.indices};
    const auto raw_query = crd::math::to_raw_vec(query);
    const auto raw_result = crd::geometry::mesh::mesh_closest_point(raw_view, bvh, raw_query);
    if (!raw_result.has_value())
    {
        return std::nullopt;
    }
    MeshClosestPointT<D, T> out{};
    out.point = crd::math::from_raw_vec<D>(raw_result->point);
    out.distance_squared = crd::units::Quantity<crd::units::DimMul<D, D>, T>{raw_result->distance_squared};
    out.payload = raw_result->payload;
    return out;
}

// ---------------------------------------------------------------------------
// Raycast — typed Quantity wrappers (v4b)
// ---------------------------------------------------------------------------

// Typed ray: origin carries Length; direction is dimensionless unit vector;
// max_t is a Length (parametric distance along the dimensionless direction
// equals Length).
template <typename D, typename T> struct Ray3T
{
    crd::math::Vec3<crd::units::Quantity<D, T>> origin{};
    crd::math::Vec3<T>                           direction{};
};

template <typename D, typename T> struct MeshRayHitT
{
    crd::units::Quantity<D, T>      t{};        // typed length along ray
    MeshHitPayload                   payload{};  // tri + barycentric (dimensionless)
};

template <typename D, typename T>
[[nodiscard]] inline std::optional<MeshRayHitT<D, T>>
mesh_raycast(const TriangleMeshViewT<D, T>& typed_view,
             crd::containers::ConstSpan<crd::math::Vec3<T>> raw_vertices,
             const TriangleMeshBvh& bvh,
             const Ray3T<D, T>& ray,
             crd::units::Quantity<D, T> tmax = crd::units::Quantity<D, T>{
                 std::numeric_limits<T>::infinity()},
             bool cull_back = false) noexcept
{
    static_assert(std::is_same_v<T, crd::f32>,
                  "mesh_raycast typed wrapper is f32 only at v4b.");
    TriangleMeshView<T> raw_view{raw_vertices, typed_view.indices};
    const crd::geometry::primitives::Ray3<T> raw_ray{
        crd::math::to_raw_vec(ray.origin), ray.direction};
    const auto raw_result = crd::geometry::mesh::mesh_raycast(
        raw_view, bvh, raw_ray, tmax.value, cull_back);
    if (!raw_result.has_value())
    {
        return std::nullopt;
    }
    MeshRayHitT<D, T> out{};
    out.t       = crd::units::Quantity<D, T>{raw_result->t};
    out.payload = raw_result->payload;
    return out;
}

// ---------------------------------------------------------------------------
// Winding number — typed Quantity wrapper (v4c)
// ---------------------------------------------------------------------------
//
// Winding number is dimensionless (rotations / 4π) — the typed wrapper
// only types the QUERY POINT, not the return.

template <typename D, typename T>
[[nodiscard]] inline crd::f32
mesh_winding_number(const TriangleMeshViewT<D, T>& typed_view,
                    crd::containers::ConstSpan<crd::math::Vec3<T>> raw_vertices,
                    const crd::math::Vec3<crd::units::Quantity<D, T>>& query) noexcept
{
    static_assert(std::is_same_v<T, crd::f32>,
                  "mesh_winding_number typed wrapper is f32 only at v4c.");
    TriangleMeshView<T> raw_view{raw_vertices, typed_view.indices};
    return crd::geometry::mesh::mesh_winding_number(raw_view, crd::math::to_raw_vec(query));
}

template <typename D, typename T>
[[nodiscard]] inline bool
mesh_is_inside(const TriangleMeshViewT<D, T>& typed_view,
               crd::containers::ConstSpan<crd::math::Vec3<T>> raw_vertices,
               const crd::math::Vec3<crd::units::Quantity<D, T>>& query,
               crd::f32 threshold = 0.5F) noexcept
{
    return mesh_winding_number(typed_view, raw_vertices, query) > threshold;
}

} // namespace crd::geometry::mesh
