#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh — formal mesh validation (Phase 3.1.7 v4-validate).
//
// Walks a `TriangleMeshView` and reports structural defects that would break
// downstream queries / cooker pipelines / FEA prep / collision detection:
//
//   * **Out-of-bounds index**: triangle references a vertex index ≥ vertex_count.
//   * **Degenerate triangle**: i0==i1 || i1==i2 || i2==i0 (same vertex repeated
//     within a triangle).
//   * **Zero-area triangle**: |edge1 × edge2| / 2 < area_epsilon. Triangles
//     collapsed to a line or point.
//   * **Non-manifold edge**: a single undirected edge shared by ≥3 triangles
//     ("fin" / fan-overshoot — the volumetric solid-or-not decision is
//     undefined at such an edge).
//   * **Boundary edge**: a single undirected edge shared by exactly 1
//     triangle. Informational on its own; together with manifold edges → an
//     "open mesh" (not watertight). The Jacobson 2013 winding-number query
//     (v4c) handles those cases robustly; raycast / closest-point do not
//     need watertightness.
//   * **Inconsistent orientation**: a single edge appearing in the SAME
//     direction in two adjacent triangles' winding. In a CCW-outward
//     manifold, every shared edge is traversed in opposite directions by
//     its two faces. Detecting this catches mesh-loading bugs and bad
//     authoring.
//
// API surface:
//
//   * `MeshDefectKind` enum + `MeshDefect{kind, a, b}` POD.
//   * `MeshValidationReport` — defects vector + summary booleans.
//   * `validate_triangle_mesh(view, alloc, opts)` — the validator.
//
// **Determinism (ADR-0076 §4 pin #11)**:
//   * Defects are emitted in deterministic order: pass 1 = triangle-index
//     ascending, pass 2 = edge-canonical-key ascending.
//   * Edge canonical key = (min(v_lo, v_hi), max(v_lo, v_hi)) — orientation-
//     free, deterministic across input winding.
//   * Area-zero test uses unsigned squared length of the edge1 × edge2
//     cross product — branchless, bit-exact.
//
// **Two-layer typing (ADR-0078 §5)**: `area_epsilon` is typed `Area32`
// (Length²) at the options surface; the algorithm body strips to raw
// at the boundary. Same pattern as `queries_typed.hpp`.
//
// **Use cases**:
//   * **Cooker pipeline gate**: reject meshes with `defect_count > 0` and
//     `well_formed = false` from the cooked-asset path. Authoring tools
//     fix-or-warn; the engine refuses to load known-broken meshes.
//   * **Editor mesh-import diagnostics**: surface every defect with file:line
//     pointers to the artist.
//   * **Runtime safety check**: optional debug-only gate before handing a
//     scanned / network-received mesh to the eylem TriangleMeshCollider.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/units/quantity_aliases.hpp>

namespace crd::geometry::mesh
{

enum class MeshDefectKind : crd::u8
{
    OutOfBoundsIndex,        // triangle a refers to vertex b which is ≥ vertex_count
    DegenerateTriangle,      // triangle a has a repeated vertex (i0==i1 || i1==i2 || i2==i0)
    ZeroAreaTriangle,        // triangle a's area < threshold (vertices collinear / coincident)
    NonManifoldEdge,         // edge between vertex a and vertex b is shared by ≥3 triangles
    BoundaryEdge,            // edge between vertex a and vertex b is shared by 1 triangle (open mesh)
    InconsistentOrientation, // triangles a and b share an edge in the same direction
};

struct MeshDefect
{
    MeshDefectKind kind;
    crd::u32       a; // triangle index (OutOfBounds / Degenerate / ZeroArea / InconsistentOrientation),
                       // OR low vertex index (NonManifold / Boundary)
    crd::u32       b; // out-of-bounds vertex index (OutOfBounds), OR
                       // high vertex index (NonManifold / Boundary), OR
                       // second triangle (InconsistentOrientation), OR
                       // 0xFFFFFFFFU sentinel (others)
};

// Tunables for the validation pass.
struct MeshValidationOptions
{
    // Triangles with area below this threshold are flagged as zero-area.
    // Default 1e-12 m² ≈ a triangle smaller than a 1-micrometre square.
    crd::units::Area32 area_epsilon{1.0e-12F};

    // Detection toggles. Disable expensive checks when only a quick sanity
    // pass is wanted (e.g., debug-only runtime gate).
    bool check_edges                = true;  // builds the edge map (~O(E log E))
    bool check_orientation          = true;  // requires edge map; flags consistent-direction shared edges
    bool report_boundary_edges      = true;  // emit BoundaryEdge defects; toggle off when open meshes are normal
};

// Summary report. Defects are emitted in deterministic order.
struct MeshValidationReport
{
    crd::containers::Array<MeshDefect> defects;
    crd::u32 triangle_count        = 0;
    crd::u32 vertex_count          = 0;
    crd::u32 non_manifold_edge_count = 0;
    crd::u32 boundary_edge_count   = 0;
    crd::u32 manifold_edge_count   = 0;

    // **well_formed**: no critical defects (OutOfBounds, Degenerate, NonManifold,
    // InconsistentOrientation). Boundary edges + zero-area triangles do NOT
    // disqualify well-formed-ness — they're authoring smells, not loaders bugs.
    bool well_formed = false;

    // **watertight**: well_formed AND zero boundary edges. Implies closed
    // manifold surface — every edge shared by exactly 2 consistently-oriented
    // triangles. Required for some downstream consumers (CSG, exact volume,
    // SDF baking via flood-fill).
    bool watertight  = false;

    explicit MeshValidationReport(crd::memory::IAllocator* a) noexcept : defects(a) {}
};

// Run all enabled checks; populate the report. Caller owns `alloc`; defect
// array binds to it. `view` is read-only; the report borrows nothing from it.
[[nodiscard]] MeshValidationReport
validate_triangle_mesh(const TriangleMeshViewf& view,
                       crd::memory::IAllocator* alloc,
                       const MeshValidationOptions& opts = {});

} // namespace crd::geometry::mesh
