#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8e 3D Lloyd's Centroidal Voronoi Tessellation
//                          relaxation (Lloyd 1982, 3D form).
//
// Iteratively moves each site to the volume-weighted centroid of its 3D
// Voronoi cell. Converges to a 3D Centroidal Voronoi Tessellation (CVT).
// Powers 3D blue-noise sampling, particle-cloud regularisation, isotropic
// tet-meshing seed-point generation, biological-tissue cell-packing models.
//
// **Algorithm**:
//   1. For iter = 0..max_iterations:
//      a. Compute 3D Voronoi diagram (`voronoi_3d`) of current sites.
//      b. For each cell:
//         - If cell.is_bounded: centroid via tet decomposition. Pick a
//           reference point (cell's first Voronoi vertex), build tets
//           `(ref, face_v[0], face_v[i], face_v[i+1])` for each face's
//           triangle fan. Cell centroid = sum(tet_centroid * tet_signed_vol)
//           / sum(tet_signed_vol).
//         - Else (hull site — cell unbounded):
//           - `HullPolicy3::Fix` (DEFAULT): keep site put.
//           - `HullPolicy3::ClipToBbox`: NOT YET IMPLEMENTED in v8e — 3D
//             polyhedron-vs-bbox halfspace clipping is scoped as a follow-on.
//             Returns `BboxClipNotSupported3D` status. Use `Fix` for now.
//      c. max_disp = max over all sites.
//      d. If max_disp < tolerance: converged, break.
//   2. Return relaxed sites + telemetry.
//
// Same `HullPolicy` + convergence + determinism contracts as the 2D form.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::delaunay
{

enum class HullPolicy3 : crd::u8
{
    Fix        = 0,
    ClipToBbox = 1, // RESERVED — returns BboxClipNotSupported3D in v8e; v8e-3d-clip follow-on adds the halfspace clipper
};

enum class LloydStatus3 : crd::u8
{
    Ok                       = 0,
    TooFewPoints             = 1, // < 4 input sites
    NonFiniteInput           = 2,
    DuplicatePoint           = 3,
    Coplanar                 = 4,
    BboxInvalid              = 5,
    BboxClipNotSupported3D   = 6, // ClipToBbox requested; not implemented yet
    NotConverged             = 7,
    InternalInvariant        = 8,
};

template <crd::math::MathScalar T>
struct LloydOptions3
{
    crd::u32           max_iterations = 50;
    T                  tolerance      = static_cast<T>(1e-4);
    HullPolicy3        hull_policy    = HullPolicy3::Fix;
    crd::math::Vec3<T> bbox_min{};
    crd::math::Vec3<T> bbox_max{};
    bool               bbox_set       = false;
};

template <crd::math::MathScalar T>
struct LloydResult3
{
    crd::containers::Array<crd::math::Vec3<T>> relaxed_sites;
    crd::u32                                     iterations_run         = 0;
    T                                            final_max_displacement = static_cast<T>(0);
    bool                                         converged              = false;
    LloydStatus3                                 status                 = LloydStatus3::Ok;

    explicit LloydResult3(crd::memory::IAllocator* alloc) : relaxed_sites(alloc) {}

    [[nodiscard]] bool ok() const noexcept
    {
        return status == LloydStatus3::Ok || status == LloydStatus3::NotConverged;
    }
};

template <crd::math::MathScalar T>
[[nodiscard]] LloydResult3<T>
lloyd_relax_3d(crd::containers::ConstSpan<crd::math::Vec3<T>> sites,
                const LloydOptions3<T>&                         opts,
                crd::memory::IAllocator*                        alloc);

} // namespace crd::geometry::delaunay
