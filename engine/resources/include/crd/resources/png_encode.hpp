#pragma once

// png_encode.hpp — GEO-4 pt 2 (D-007): the baseline PNG ENCODER — the write half of the owned PNG codec (png_image.hpp
// decodes). RGBA8 → 8-bit truecolor-alpha PNG: filter-None scanlines through OUR zlib_deflate, every chunk CRC'd with
// OUR png_crc32 — a fully valid PNG from the compression stack we already own. Deliberately the FLOOR: per-row filter
// heuristics + palette/gray paths are MED-2's optimizing encoder; this one is correct, deterministic, and small —
// exactly what the glTF exporter needs to embed textures TODAY. Round-trip contract: png_decode(png_encode(x)) == x
// byte-exact (gated).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::resources
{

// Encode width×height RGBA8 pixels (row-major, 4 bytes/pixel) as a PNG. Returns an empty array on invalid input
// (zero dimension, span size ≠ width·height·4, or dimensions beyond the decoder's own 16384² cap — what we encode,
// we must be able to decode).
[[nodiscard]] crd::containers::Array<crd::u8> png_encode_rgba(crd::containers::ConstSpan<crd::u8> rgba, crd::u32 width,
                                                              crd::u32 height, crd::memory::IAllocator* alloc);

} // namespace crd::resources
