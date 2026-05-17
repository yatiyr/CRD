#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8e 2D Lloyd's Centroidal Voronoi Tessellation
//                          relaxation (Lloyd 1982).
//
// Iteratively moves each site to the centroid of its Voronoi cell. Converges
// to a Centroidal Voronoi Tessellation (CVT) — a point set where each site
// IS the centroid of its cell. Powers stippling, blue-noise sampling, point-
// cloud regularisation, isotropic remeshing alternative, particle initialisa-
// tion for SPH / FEM.
//
// **Algorithm**:
//   1. For iter = 0..max_iterations:
//      a. Compute Voronoi diagram of current `sites`.
//      b. For each cell:
//         - If cell.is_bounded: centroid via `crd::geometry::polygon::centroid`
//           of the cell's polygon (the Voronoi vertices in CCW order).
//         - Else (unbounded — site is on the convex hull):
//           - `HullPolicy::Fix` — keep site at current position (no move).
//           - `HullPolicy::ClipToBbox` — clip the cell against `bbox`
//             (extending the cell's rays to bbox boundary, walking bbox
//             corners between exit and entry, applying Sutherland-Hodgman
//             for safety), then compute clipped polygon centroid.
//      c. max_disp = max(||new_sites[i] - sites[i]||) over all sites.
//      d. If max_disp < `tolerance`: converged, break.
//   2. Return relaxed sites + telemetry.
//
// **HullPolicy** (D102):
//   - `Fix` (DEFAULT): hull sites don't move. Interior sites relax fully.
//     Use when you want to preserve a boundary point set + relax interior
//     points only. Simplest, always works.
//   - `ClipToBbox`: clip unbounded cells against `bbox` before computing
//     centroid. Use when relaxing ALL sites uniformly inside a closed
//     domain. Requires `bbox_set = true` OR auto-derived bbox (input bbox
//     + 10% pad).
//
// **Convergence** (D103):
//   - `tolerance` is the maximum per-iteration site displacement (in input
//     coordinate units, NOT relative). A tolerance of 1e-4 with input bbox
//     diagonal ~1 means relaxation halts when no site moves more than 1e-4.
//   - `max_iterations` is the safety cap. Typical CVT convergence: 20-30
//     iterations for well-distributed input, 100+ for adversarial.
//   - If max_iterations exhausted without reaching tolerance: status =
//     `NotConverged`, but `relaxed_sites` still returned with the best-so-
//     far positions.
//
// **Determinism** (ADR-0063): Voronoi extraction is deterministic; centroid
// is a pure function of cell vertices; iteration order is sequential.
// Byte-identical output for the same input + options across compilers.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::delaunay
{

enum class HullPolicy2 : crd::u8
{
    Fix         = 0, // hull sites stay put (no move)
    ClipToBbox  = 1, // clip unbounded cells to bbox, then centroid
};

enum class LloydStatus2 : crd::u8
{
    Ok                = 0,
    TooFewPoints      = 1, // < 3 input sites
    NonFiniteInput    = 2,
    DuplicatePoint    = 3,
    BboxInvalid       = 4, // ClipToBbox requested with degenerate bbox
    NotConverged      = 5, // ran max_iterations without reaching tolerance
    InternalInvariant = 6,
};

template <crd::math::MathScalar T>
struct LloydOptions2
{
    crd::u32           max_iterations = 50;
    T                  tolerance      = static_cast<T>(1e-4);
    HullPolicy2        hull_policy    = HullPolicy2::Fix;
    crd::math::Vec2<T> bbox_min{};
    crd::math::Vec2<T> bbox_max{};
    bool               bbox_set       = false; // false -> derive from input bbox + 10% pad
};

template <crd::math::MathScalar T>
struct LloydResult2
{
    crd::containers::Array<crd::math::Vec2<T>> relaxed_sites;
    crd::u32                                     iterations_run         = 0;
    T                                            final_max_displacement = static_cast<T>(0);
    bool                                         converged              = false;
    LloydStatus2                                 status                 = LloydStatus2::Ok;

    explicit LloydResult2(crd::memory::IAllocator* alloc) : relaxed_sites(alloc) {}

    [[nodiscard]] bool ok() const noexcept
    {
        return status == LloydStatus2::Ok || status == LloydStatus2::NotConverged;
    }
};

// Entry point. Relaxes `sites` to a Centroidal Voronoi Tessellation.
template <crd::math::MathScalar T>
[[nodiscard]] LloydResult2<T>
lloyd_relax_2d(crd::containers::ConstSpan<crd::math::Vec2<T>> sites,
                const LloydOptions2<T>&                         opts,
                crd::memory::IAllocator*                        alloc);

} // namespace crd::geometry::delaunay
