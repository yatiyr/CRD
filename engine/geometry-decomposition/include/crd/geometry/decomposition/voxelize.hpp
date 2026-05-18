#pragma once

// ---------------------------------------------------------------------------
// crd::geometry::decomposition::voxelize_mesh — triangle mesh → 3D voxel
// grid. Phase 3.1.7 v9c-a. First stage of the V-HACD pipeline (Mamou 2014
// §3.1); the v9c-b decompose stage consumes the resulting VoxelGrid.
//
// Algorithm — STRICTLY TWO NON-OVERLAPPING PASSES (pin D126):
//
//   Pass 1  Surface marking (Akenine-Möller 2001 SAT). Per triangle, walk
//           only voxels in the triangle's AABB and test exact 13-axis
//           triangle/box overlap. Parallelised via crd::jobs::parallel_for
//           over triangle batches; per-voxel write is the idempotent
//           `std::atomic_ref<u8>::fetch_or(Surface)`. All jobs join before
//           pass 2 starts.
//
//   Pass 2  Classification — only writes Outside / Inside into voxels
//           STILL marked Unknown (Surface cells are never overwritten):
//             * WindingNumber (default): Jacobson 2013 generalised winding
//               number per voxel centre; robust on non-watertight /
//               non-manifold input.
//             * FloodFill: 6-connected BFS from a corner Outside seed;
//               fast, REQUIRES watertight input.
//
// Determinism: identical input → byte-identical VoxelGrid contents
// regardless of thread interleaving. `fetch_or(Surface)` is commutative +
// idempotent, and pass 2 reads only finalised pass-1 state.
//
// Two-layer typing per ADR-0078 §5 D34: the public API surface is raw
// `<MathScalar T>` for this slice; typed `Length<T> voxel_size` mode is
// planned for v9c-b/close per the advisor scope-trim. v9c-a ships f32 only
// because `mesh_winding_number` (the WindingNumber-mode oracle) is f32-only
// today; f64 entry as a follow-on slice when a real consumer asks.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/decomposition/voxel.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::decomposition
{

// Voxelize the input triangle mesh into a 3D voxel grid per `opts`. The
// returned `VoxelizationResult<f32>` carries the grid + world-space AABB
// of the (padded) grid + per-voxel world size + telemetry. Caller checks
// `result.status == Ok` before consuming `result.grid`.
[[nodiscard]] VoxelizationResult<crd::f32>
voxelize_mesh(const crd::geometry::mesh::TriangleMeshViewf& view,
              const VoxelizationOptions&                    opts,
              crd::memory::IAllocator*                      alloc) noexcept;

} // namespace crd::geometry::decomposition
