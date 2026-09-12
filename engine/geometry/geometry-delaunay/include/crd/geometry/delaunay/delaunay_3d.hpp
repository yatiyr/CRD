#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8c 3D Bowyer-Watson Delaunay tetrahedralisation
//                          (Bowyer 1981 / Watson 1981).
//
// Pure 3D Delaunay tetrahedralisation of a point set. For every input point
// set (with at least 4 non-coplanar, finite, distinct points), the algorithm
// produces a Delaunay tetrahedralisation — every tetrahedron's circumsphere
// contains NO other input point ("empty circumsphere" property). The dual of
// this tet mesh is the 3D Voronoi diagram (v8d-3d).
//
// **Algorithm** (Bowyer 1981 / Watson 1981 incremental insertion, 3D form):
//   1. Build a "super-tetrahedron" containing all input points (1000× bbox
//      scale — same empirically-stable scaling as v8a). Verified positively
//      oriented at init via `orient3d` (D90 — super-tet must be CCW).
//   2. Lex-sort input points by `(x, y, z, original_index)` for determinism.
//   3. For each point in sorted order:
//      a. Jump-walk to the containing tet via apex-side `orient3d` for each
//         of 4 faces; deterministic tiebreak prefers lowest face index when
//         multiple are on the wrong side.
//      b. Cavity BFS via **Shewchuk adaptive Stage D `insphere`** (paid down
//         in v8c-pre) collecting all "bad" tets whose circumsphere contains
//         the query.
//      c. Identify the cavity polyhedron boundary = faces of bad-tet cluster
//         NOT shared with another bad tet. Each face is recorded with its
//         outward orientation (so the new tet with q apex is positively
//         oriented automatically).
//      d. Delete bad tets. Re-tetrahedralise the cavity by fanning new tets
//         from the inserted point to each cavity-boundary face.
//      e. Wire neighbour-tet pointers between new tets + the outer ring of
//         unchanged tets.
//   4. After all points inserted, strip every tet that references a
//      super-tetrahedron vertex.
//
// **Output tet convention.** Each output tet's four vertex indices reference
// the INPUT `points` array (super-tetrahedron vertices stripped). Orientation
// is positive: `orient3d(v0, v1, v2, v3) > 0` for every output tet. Expected
// tet count ≈ 6.5N to 7N for N points in general 3D position.
//
// **Determinism contract (ADR-0063 + ADR-0076 §4 pin #11):**
//   - Lex-sort `(x, y, z, original_index)` on input.
//   - Jump-walk picks the cross-face in fixed face-index order (0, 1, 2, 3)
//     when multiple are negative-side.
//   - Cavity BFS pops in monotonic tet-id order (low-id first).
//   - Output tets emitted in tet-id order at strip-time.
//   Byte-identical output across compilers / SIMD widths / OSes given
//   byte-identical input.
//
// **Robustness contract (ADR-0076 §15):**
//   - All input point coordinates must be finite (else `NonFiniteInput`).
//   - Input must have ≥ 4 points (else `TooFewPoints`).
//   - Two input points with EXACTLY the same coordinates ⇒ `DuplicatePoint`.
//   - All N points coplanar (no 3D tet possible) ⇒ `Coplanar`.
//   - Cospherical inputs: handled by Shewchuk Stage D `insphere` returning
//     exact 0 — the lex-tiebreak then picks the deterministic outcome.
//   - **Star-shape defensive check (D91)**: every cavity boundary face is
//     verified `orient3d(face_v0, face_v1, face_v2, q) > 0` before
//     constructing the new tet. A failure means either degenerate input the
//     predicates can't resolve OR a bug in the cavity-BFS logic; we return
//     `InternalInvariant` rather than ship a corrupt mesh.
//
// **Two-layer typing (ADR-0078 §5 D34):** raw `<MathScalar T>` body;
// typed `Vec3<Length32>` consumers ride wrappers in `delaunay_3d_typed.hpp`
// added at slice close on first typed consumer.
//
// **Prerequisite paid 2026-05-17**: v8c-pre upgraded `insphere_exact` from
// Stage-A-equivalent to full Shewchuk Stage D. Without this, cavity BFS on
// near-cospherical input produces non-star-shaped cavities → inverted tets.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::delaunay
{

enum class DelaunayStatus3 : crd::u8
{
    Ok                = 0,
    TooFewPoints      = 1, // < 4 input points
    NonFiniteInput    = 2, // a point coordinate is non-finite
    DuplicatePoint    = 3, // two points have identical coordinates
    Coplanar          = 4, // all N points are coplanar (3D tet impossible)
    InternalInvariant = 5, // unexpected algorithmic failure (should never trip)
};

template <crd::math::MathScalar T>
struct DelaunayResult3
{
    // Output tets — 4 indices per tet, positively oriented. Indices reference
    // the INPUT `points` array (super-tetrahedron vertices stripped).
    crd::containers::Array<crd::u32> tet_indices;
    crd::u32                          tet_count          = 0;
    crd::u32                          cavity_max_size    = 0; // largest cavity BFS encountered
    crd::u32                          super_tet_stripped = 0; // tets touching super-tet (filtered)
    DelaunayStatus3                   status             = DelaunayStatus3::Ok;

    explicit DelaunayResult3(crd::memory::IAllocator* alloc) : tet_indices(alloc) {}

    [[nodiscard]] bool ok() const noexcept { return status == DelaunayStatus3::Ok; }
};

// Entry point. Builds the 3D Delaunay tetrahedralisation of `points` on
// `alloc`.
template <crd::math::MathScalar T>
[[nodiscard]] DelaunayResult3<T>
delaunay_3d(crd::containers::ConstSpan<crd::math::Vec3<T>> points,
            crd::memory::IAllocator*                        alloc);

} // namespace crd::geometry::delaunay
