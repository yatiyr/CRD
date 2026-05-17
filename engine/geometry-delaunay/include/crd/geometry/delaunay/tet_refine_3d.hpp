#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8h 3D Delaunay quality refinement (dihedral-
// bounded). Tet-mesh post-processing for FEA / FVM 3D simulation prep.
//
// **Scope honest** (D119, pinned at v8-close):
//
// This is **dihedral-bounded refinement** — a 3D-Ruppert analog that
// inserts Steiner points at bad-tet circumcentres until every tet's
// **minimum dihedral angle** exceeds `α`. It is NOT the **sliver exudation**
// algorithm (Cheng-Dey-Edelsbrunner-Facello-Teng 2000), which uses
// *weighted* Delaunay with per-vertex weight perturbations. Sliver
// exudation is a separate algorithm scoped as v8h-exude follow-on.
//
// **Known limitation**: Steiner-insertion-only refinement (this algorithm)
// **does not provably remove all 3D slivers**. Slivers have GOOD radius-
// edge ratio and BAD dihedral; their circumcentres often land back inside
// another sliver or outside the input domain. Termination is NOT
// guaranteed for arbitrary α (no 3D analog to Ruppert's 2D ≤ 20.7° bound).
// Adversarial inputs CAN trip `NotConverged` — that's a valid outcome,
// not a bug. For inputs where `Fix` produces unrefinable slivers, the
// upcoming sliver-exudation follow-on is the right tool.
//
// **Algorithm** (per iteration):
//   1. Build / rebuild Delaunay via v8c `delaunay_3d`.
//   2. Scan tets in order; find first tet with min-dihedral < α (a "bad" tet).
//   3. Compute its circumcentre via `crd::geometry::primitives::circumcenter_3d`.
//   4. **Out-of-domain skip** (D121): if circumcentre is outside the input
//      bbox (with 10% pad), skip this tet; continue scanning for next bad
//      tet (don't try to insert outside the input domain).
//   5. **Near-duplicate skip** (D122): if circumcentre is within a
//      bbox-scaled eps (`eps_sq = (bbox_diag * 1e-6)²`) of any existing
//      vertex, skip.
//   6. Insert the actionable circumcentre as a Steiner vertex; re-Delaunay.
//   7. If no actionable bad tet found in a full scan: halt with
//      `NotConverged` (we can't make progress without violating
//      domain/duplicate invariants).
//   8. If `max_iterations` or `max_steiner` reached: also `NotConverged`.
//
// **Six dihedrals per tet** (D120): for tet (v0, v1, v2, v3), enumerate
// edges `(0,1), (0,2), (0,3), (1,2), (1,3), (2,3)`. For edge (vi, vj) with
// off-edge vertices (vk, vl), dihedral = arccos(dot(n1, n2) / (|n1| |n2|))
// where `n1 = (vj - vi) × (vk - vi)`, `n2 = (vj - vi) × (vl - vi)`.
// Calibration: regular tetrahedron has all 6 dihedrals = arccos(1/3) ≈
// 70.5288°.
//
// **Determinism contract**: tet scan in CDT-output order; first bad
// actionable tet found is processed; bbox derived from input order.
// Byte-identical for byte-identical input + options.
//
// **Robustness contract**:
//   - Diagnostics from Delaunay: `TooFewPoints` / `NonFiniteInput` /
//     `DuplicatePoint` / `Coplanar`.
//   - `InvalidAngle` if `min_dihedral_degrees` is outside (0, 70.5°]
//     (70.53° is regular-tet dihedral — geometric upper bound for
//     uniform meshes).
//   - `NotConverged` for unrefinable inputs.
//   - `InternalInvariant` for unexpected algorithmic failure.
//
// **Two-layer typing** (ADR-0078 §5 D34): raw `<MathScalar T>` body;
// typed wrappers in `tet_refine_3d_typed.hpp` ship at slice close on
// first typed consumer.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::delaunay
{

enum class TetRefineStatus : crd::u8
{
    Ok                = 0,
    TooFewPoints      = 1, // < 4 input points
    NonFiniteInput    = 2,
    DuplicatePoint    = 3,
    Coplanar          = 4, // all input points coplanar
    InvalidAngle      = 5, // min_dihedral_degrees outside (0, 70.5]
    NotConverged      = 6, // max_iterations / max_steiner reached, or no actionable bad tet
    InternalInvariant = 7,
};

template <crd::math::MathScalar T>
struct TetRefineOptions
{
    T        min_dihedral_degrees = static_cast<T>(10);
    crd::u32 max_iterations       = 5000U;
    crd::u32 max_steiner          = 50000U;
};

template <crd::math::MathScalar T>
struct TetRefineResult
{
    crd::containers::Array<crd::math::Vec3<T>> vertices;       // input + Steiner appended
    crd::containers::Array<crd::u32>             tet_indices;    // 4 per tet
    crd::u32                                     tet_count       = 0;
    crd::u32                                     steiner_count   = 0;
    crd::u32                                     iterations_run  = 0;
    bool                                         converged       = false;
    TetRefineStatus                              status          = TetRefineStatus::Ok;

    explicit TetRefineResult(crd::memory::IAllocator* alloc)
      : vertices(alloc), tet_indices(alloc) {}

    [[nodiscard]] bool ok() const noexcept
    {
        return status == TetRefineStatus::Ok || status == TetRefineStatus::NotConverged;
    }
};

// Entry point. Refines a 3D point set into a Delaunay tet mesh where
// every tet's minimum dihedral angle exceeds `opts.min_dihedral_degrees`.
// PSLG constraints (boundary triangles) are deferred to v8h-pslg follow-on.
template <crd::math::MathScalar T>
[[nodiscard]] TetRefineResult<T>
tet_refine_3d(crd::containers::ConstSpan<crd::math::Vec3<T>> points,
               const TetRefineOptions<T>&                      opts,
               crd::memory::IAllocator*                        alloc);

// Public helper: minimum dihedral angle of a tetrahedron, in radians.
// Enumerates all 6 edges; returns the smallest. Returns 0 if any face has
// zero area (degenerate). Calibration: regular tet returns arccos(1/3).
template <crd::math::MathScalar T>
[[nodiscard]] T
min_dihedral_of_tet_rad(const crd::math::Vec3<T>& v0,
                         const crd::math::Vec3<T>& v1,
                         const crd::math::Vec3<T>& v2,
                         const crd::math::Vec3<T>& v3) noexcept;

} // namespace crd::geometry::delaunay
