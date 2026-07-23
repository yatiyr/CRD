#pragma once

// ply.hpp — GEO-1: OUR OWN PLY parser (ASCII + binary_little_endian + binary_big_endian). PLY is the scanned/research
// mesh format (Stanford scans, photogrammetry, point clouds); its header DECLARES an arbitrary element/property schema,
// so the parser is schema-driven:
//
//   • The header is parsed into an element/property table; the body walker reads EVERY record by its declared layout —
//     properties we don't consume (colors, confidence, custom channels) are read-and-skipped EXACTLY (binary skipping
//     must honor per-record variable-length lists, or every later element misparses).
//   • Vertex semantics captured: x/y/z (required), nx/ny/nz, u/v (or s/t) — everything else skipped.
//   • Faces are `property list` records — fan-triangulated, indices validated; a PURE POINT CLOUD (no face element) is a
//     valid import (0 triangles): slicer/scan workflows depend on it.
//   • All 8 scalar types (i8..f64, incl. the intN/uintN/floatN synonyms); big-endian bodies byte-swapped.
//
// Span-based, allocator-only, no file I/O — the codec posture (see stl.hpp).

#include <crd/assetio/imported_asset.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::assetio
{

// Parse PLY bytes into `out` (one mesh appended on success; 0 triangles = a valid point cloud).
[[nodiscard]] ImportStatus parse_ply(crd::containers::ConstSpan<crd::u8> bytes, crd::memory::IAllocator* alloc,
                                     ImportedAsset& out);

} // namespace crd::assetio
