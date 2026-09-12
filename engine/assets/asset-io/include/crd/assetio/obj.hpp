#pragma once

// obj.hpp — GEO-1: OUR OWN Wavefront OBJ (+MTL) parser. OBJ is the ubiquitous DCC text format and MESSY in practice; the
// dirt handled explicitly (each a real-world file class):
//
//   • SEPARATE index spaces (v/vt/vn) — collapsed to the single-indexed ImportedMesh via exact corner deduplication
//     (a HashMap over the (v,vt,vn) triple), preserving connectivity instead of exploding to soup.
//   • NEGATIVE (relative) indices — resolved against the running attribute counts, per spec.
//   • N-gon faces — fan-triangulated (downstream only ever sees triangles).
//   • MIXED corners (some with vn/vt, some without) inside one mesh — missing attributes fill with zero + a warning
//     (GEO-2 conditioning recomputes normals); rejecting would refuse half the OBJ files in the wild.
//   • `o`/`g` objects and `usemtl` changes SPLIT meshes (one ImportedMesh per object×material run — the render-submission
//     granularity), `l`/`p` (lines/points) and unknown keywords skip with a warning.
//
// FILE I/O STAYS OUTSIDE (span-based, the codec posture): `mtllib` is recorded as a NAME ONLY — the caller loads the .mtl
// bytes and runs `parse_mtl` FIRST, then `parse_obj` resolves each `usemtl` by name against `out.materials`. Unresolved
// `usemtl` = material -1 + warning (a missing .mtl must not kill the geometry).

#include <crd/assetio/imported_asset.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::assetio
{

// Parse MTL bytes, APPENDING materials to `out.materials`. Kd → base_color; Pr/Pm (PBR extension) → roughness/metallic;
// Ns (Blinn-Phong shininess) → roughness ≈ sqrt(2/(2+Ns)) when no Pr is given. Texture maps land at GEO-3.
[[nodiscard]] ImportStatus parse_mtl(crd::containers::ConstSpan<crd::u8> bytes, crd::memory::IAllocator* alloc,
                                     ImportedAsset& out);

// Parse OBJ bytes into `out` (one ImportedMesh per object×material run, only runs with ≥1 triangle are emitted).
// `usemtl` names resolve against materials ALREADY in `out.materials` (run parse_mtl first); unresolved → -1 + warning.
[[nodiscard]] ImportStatus parse_obj(crd::containers::ConstSpan<crd::u8> bytes, crd::memory::IAllocator* alloc,
                                     ImportedAsset& out);

} // namespace crd::assetio
