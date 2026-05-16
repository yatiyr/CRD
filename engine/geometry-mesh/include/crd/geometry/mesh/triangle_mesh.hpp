#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh — TriangleMeshView (Phase 3.1.7 v4a / ADR-0076 §17).
//
// Non-owning view of an indexed triangle mesh. The substrate's "what is a
// triangle mesh" type — every mesh-query API in `crd-geometry-mesh` accepts
// one of these. Three contiguous, ADR-pinned arrays:
//
//   * `vertices`         : `ConstSpan<Vec3<T>>` — vertex positions (SI metres
//                          by convention; the type stays raw `T` per
//                          ADR-0078 §5 D34 — typed surfaces live one layer
//                          above via `triangle_mesh_typed.hpp`).
//   * `indices`          : `ConstSpan<u32>` — face index buffer; every 3
//                          consecutive entries form a triangle. `size() % 3
//                          == 0` is a builder-reject precondition.
//
// The mesh is consumed by these v4 queries:
//   * `closest_point(mesh, p)`  — v4a (this slice). Ericson Voronoi-region
//                                  cascade over BVH-indexed triangles.
//   * `raycast(mesh, ray)`      — v4b. Möller-Trumbore over BVH.
//   * `winding_number(mesh, p)` — v4c. Jacobson 2013 generalised winding
//                                  number; robust inside/outside on
//                                  non-watertight meshes.
//
// Each query is a free function. `TriangleMeshView` itself carries no BVH —
// the BVH is built once externally (via `build_triangle_mesh_bvh`) and passed
// alongside. This separation keeps the view trivially-copyable and lets a
// caller build the BVH once and run thousands of queries against it.
//
// ── Two-layer typing (ADR-0078 §5) ────────────────────────────────────────
// `TriangleMeshView<T>` template parameter is `MathScalar T` (raw f32/f64).
// The typed surface (`TriangleMeshView<Length32>`, returning typed
// `Vec3<Length32>` / `Length32`) ships at the API surface via
// `triangle_mesh_typed.hpp` strip-compute-retag wrappers (same pattern as
// `crd-geometry-primitives/queries_typed.hpp`).
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::mesh
{
using crd::math::MathScalar;

template <MathScalar T> struct TriangleMeshView
{
    crd::containers::ConstSpan<crd::math::Vec3<T>> vertices{};
    crd::containers::ConstSpan<crd::u32>            indices{};

    [[nodiscard]] constexpr crd::u32 triangle_count() const noexcept
    {
        return static_cast<crd::u32>(indices.size() / 3U);
    }

    [[nodiscard]] constexpr bool is_empty() const noexcept
    {
        return triangle_count() == 0U;
    }
};

using TriangleMeshViewf = TriangleMeshView<crd::f32>;
using TriangleMeshViewd = TriangleMeshView<crd::f64>;

} // namespace crd::geometry::mesh
