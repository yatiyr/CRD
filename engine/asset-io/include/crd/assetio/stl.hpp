#pragma once

// stl.hpp — GEO-1: OUR OWN STL parser (binary + ASCII, auto-detected). STL is the mesh-only floor of the format ladder —
// trivially specified, universally emitted by CAD/printing tools, and DIRTY in practice. The dirt this parser handles
// explicitly (each is a real-world file class, not paranoia):
//
//   • BINARY files whose 80-byte header starts with "solid" — the classic mis-detection trap ("solid" is ASCII's magic).
//     Detection is therefore STRUCTURAL: size == 84 + 50·triangle_count wins over what the header claims.
//   • Zero/garbage facet normals — common from mesh exporters; recomputed from the winding (right-hand rule) so
//     downstream always sees usable normals.
//   • NaN/Inf coordinates — rejected (`NonFiniteData`), never admitted into the pipeline.
//   • STL has NO units and NO index buffer: values import as authored (SI scale applies at cook, ADR-0078), and the
//     output is triangle SOUP (indices 0,1,2,…) — vertex welding is GEO-2 conditioning, not a parser concern.
//
// The parser allocates ONLY through the caller's allocator (output arrays), reads only the given byte span (no file I/O,
// no hidden scratch) — the HDR/EXR-codec posture.

#include <crd/assetio/imported_asset.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::assetio
{

// Parse STL bytes (binary or ASCII, auto-detected) into `out` (one mesh appended). Returns Ok and appends exactly one
// ImportedMesh on success (0 triangles is a valid, empty solid); on failure returns the failure class and appends nothing.
[[nodiscard]] ImportStatus parse_stl(crd::containers::ConstSpan<crd::u8> bytes, crd::memory::IAllocator* alloc,
                                     ImportedAsset& out);

} // namespace crd::assetio
