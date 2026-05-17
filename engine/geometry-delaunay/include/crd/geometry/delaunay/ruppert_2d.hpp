#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8g Ruppert 1995 2D Delaunay refinement.
//
// Quality-bounded 2D Delaunay mesh generation. Input is a Planar Straight-
// Line Graph (PSLG) = points + boundary segments. Output is a Constrained
// Delaunay Triangulation where every triangle's MINIMUM angle exceeds `α`
// (typically 25-30°). Powers FEA / FVM 2D meshing, planar finite-element
// surface preprocessing, computational-geometry quality preprocessing for
// downstream meshing pipelines (CGAL / Triangle equivalent functionality).
//
// **Algorithm** (Ruppert 1995 / Shewchuk 1996 Triangle):
//   1. Build initial CDT of the PSLG (delegates to v6c
//      `crd::geometry::polygon::constrained_delaunay`).
//   2. **Loop** until no encroached segment AND no bad triangle remains:
//      a. If any SEGMENT is **encroached** (a vertex V lies inside its
//         diametral disk -- equivalent to angle AVB > 90° at V or
//         dot(A-V, B-V) < 0), split the segment at its midpoint. The two
//         new sub-segments replace the original. Re-CDT with the new
//         point set and segment list.
//      b. Else if any triangle has **min angle < α** (a "bad" triangle):
//         - Compute its circumcentre.
//         - Encroachment check: does the circumcentre encroach any
//           segment? If YES, prioritise: split the encroached segment
//           first (skip the circumcentre insertion this iteration).
//         - Else: insert the circumcentre as a Steiner vertex. Re-CDT.
//      c. Else: done -> Ok + converged = true.
//   3. If `max_iterations` exhausted: status = `NotConverged` (caller can
//      inspect `triangle_indices` for the partial mesh; some triangles may
//      still fail the quality bound).
//
// **Termination theorem** (Ruppert 1995): for `α ≤ arcsin(1/(2√2)) ≈
// 20.7°`, the algorithm provably terminates. For larger α (e.g., 30°)
// termination is empirically common but not proven; pathological inputs
// can loop. Set `min_angle_degrees ≤ 20.7` for guaranteed termination;
// 25° typically works on production inputs and produces visibly nicer
// meshes (`triangle.org` Triangle's default).
//
// **Determinism** (ADR-0063 + ADR-0076 §4): each iteration scans segments
// then triangles in lex order (input-segment-id order, then input-triangle
// order); the first encroached segment found is split; if none, the first
// bad triangle (lowest min-angle, tie-break by input-id) is handled.
// Byte-identical output for byte-identical input + options.
//
// **Robustness contract** (ADR-0076 §15):
//   - Diagnostics propagated from CDT: `TooFewPoints` / `NonFiniteInput` /
//     `DuplicatePoint` / `ConstraintOutOfBounds` / `ConstraintsCrossing`.
//   - `InvalidAngle` if `min_angle_degrees` is non-positive or > 60°
//     (60° is the equilateral-triangle limit; > 60° is geometrically
//     impossible to satisfy for all triangles).
//   - `NotConverged` if `max_iterations` or `max_steiner` cap hit.
//   - `InternalInvariant` on any algorithmic inconsistency (defense-in-
//     depth — should not trip).
//
// **Two-layer typing** (ADR-0078 §5 D34): raw `<MathScalar T>` body;
// typed wrappers in `ruppert_2d_typed.hpp` ship at slice close on first
// typed consumer.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::delaunay
{

// Boundary segment of the input PSLG -- two indices into the input points
// array. Endpoints are unordered (the algorithm canonicalises). Mirror of
// `crd::geometry::polygon::CdtEdge` but kept separate to keep the Ruppert
// public header independent of `crd-geometry-polygon` includes.
struct RuppertSegment
{
    crd::u32 a;
    crd::u32 b;
};

enum class RuppertStatus : crd::u8
{
    Ok                    = 0,
    TooFewPoints          = 1, // < 3 input points
    NonFiniteInput        = 2,
    DuplicatePoint        = 3,
    ConstraintOutOfBounds = 4, // segment endpoint index out of range
    ConstraintsCrossing   = 5, // two input segments cross interior-to-interior
    InvalidAngle          = 6, // min_angle_degrees outside (0, 60]
    NotConverged          = 7, // max_iterations or max_steiner reached
    InternalInvariant     = 8,
};

template <crd::math::MathScalar T>
struct RuppertOptions
{
    T        min_angle_degrees = static_cast<T>(25);
    crd::u32 max_iterations    = 10000U;
    crd::u32 max_steiner       = 100000U; // safety cap on output growth
};

template <crd::math::MathScalar T>
struct RuppertResult2
{
    // Output vertices = input points ++ Steiner points appended in
    // insertion order.
    crd::containers::Array<crd::math::Vec2<T>>    vertices;
    // Output triangles -- 3 indices per triangle (CCW), referencing the
    // OUTPUT vertices array.
    crd::containers::Array<crd::u32>                triangle_indices;
    // Final segment list -- includes splits of input segments (each split
    // becomes two sub-segments in the result).
    crd::containers::Array<RuppertSegment>          refined_segments;
    crd::u32                                        triangle_count = 0;
    crd::u32                                        steiner_count  = 0;
    crd::u32                                        iterations_run = 0;
    bool                                            converged      = false;
    RuppertStatus                                   status         = RuppertStatus::Ok;

    explicit RuppertResult2(crd::memory::IAllocator* alloc)
      : vertices(alloc), triangle_indices(alloc), refined_segments(alloc) {}

    [[nodiscard]] bool ok() const noexcept
    {
        return status == RuppertStatus::Ok || status == RuppertStatus::NotConverged;
    }
};

// Entry point. Refines a PSLG into a Delaunay mesh with all triangles
// satisfying `opts.min_angle_degrees`.
template <crd::math::MathScalar T>
[[nodiscard]] RuppertResult2<T>
ruppert_refine_2d(crd::containers::ConstSpan<crd::math::Vec2<T>> points,
                   crd::containers::ConstSpan<RuppertSegment>      segments,
                   const RuppertOptions<T>&                        opts,
                   crd::memory::IAllocator*                        alloc);

} // namespace crd::geometry::delaunay
