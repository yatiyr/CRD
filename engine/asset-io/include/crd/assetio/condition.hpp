#pragma once

// condition.hpp — GEO-2 (D-007 row 67): MESH CONDITIONING — the import-time processing that turns a parsed mesh into
// render-ready geometry: exact WELD (soup → indexed), CREASE-ANGLE-aware weighted NORMALS (the production "smooth by
// angle" semantics), and OUR OWN MikkTSpace-COMPATIBLE TANGENTS (with UV-mirror splitting). All three are DETERMINISTIC
// under face reordering: per-vertex contributions are canonically SORTED before summation, so a permuted import produces
// BIT-IDENTICAL output (the engine's determinism DNA applied to asset processing).
//
//   • weld_exact — dedup BIT-IDENTICAL (position, normal, uv) corner tuples into an indexed mesh. Exact by design: a
//     hard edge (different facet normals at one position) NATURALLY stays split; epsilon-welding is a destructive
//     editing op that belongs to explicit mesh-processing tools, never a silent import step.
//   • generate_normals — per-corner angle-weighted (Thürmer-Wüthrich) accumulation of adjacent face normals, gated by a
//     CREASE ANGLE (a face contributes to a corner only when it tilts ≤ the threshold from the corner's own face) — the
//     auto-smooth behavior every DCC ships. 0° = faceted (flat); π = smooth-everything.
//   • generate_tangents — the MikkTSpace-compatible tangent frame: per-face UV-gradient tangents, per-vertex
//     angle-weighted accumulation restricted to corners of the SAME UV orientation (a vertex touched by both a mirrored
//     and an unmirrored chart DUPLICATES per handedness — the mirror-seam split), Gram-Schmidt orthogonalized against
//     the shading normal, w = the bitangent sign. Compatibility is a CONTRACT (every DCC bakes normal maps against
//     MikkTSpace) — gated in tests against the reference mikktspace.c as ORACLE (direction + exact sign agreement).

#include <crd/assetio/imported_asset.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::assetio
{

// Weld bit-identical (position, normal, uv) corners into an indexed mesh (in place). Tangents, if present, are dropped
// (they are a DERIVED attribute — regenerate after welding). Returns the number of vertices removed.
crd::u32 weld_exact(ImportedMesh& mesh, crd::memory::IAllocator* alloc);

// Recompute per-vertex normals from the geometry with crease-angle clustering (in place; the mesh is re-split + re-welded
// so each output vertex carries exactly one smoothed normal). `smooth_angle_rad`: faces within this dihedral tilt of a
// corner's own face contribute to its smoothed normal (0 = faceted, π = smooth-everything; 30° is the production default).
// Degenerate (zero-area) faces contribute nothing. Tangents, if present, are dropped (regenerate after).
void generate_normals(ImportedMesh& mesh, crd::memory::IAllocator* alloc, crd::f32 smooth_angle_rad);

// Generate the MikkTSpace-compatible per-vertex tangent frame into `mesh.tangent` (xyz = tangent, w = bitangent sign
// ±1). Requires indexed geometry + per-vertex normals + uv0. Vertices spanning BOTH UV orientations are DUPLICATED per
// handedness (the mirror-seam split; indices are rewritten). Corners with degenerate UVs contribute nothing; a vertex
// with no valid contribution falls back to an arbitrary frame perpendicular to its normal (w = +1). Returns false when
// the preconditions are unmet (missing normals/uv0 — the caller decides whether that is an error).
bool generate_tangents(ImportedMesh& mesh, crd::memory::IAllocator* alloc);

} // namespace crd::assetio
