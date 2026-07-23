#pragma once

// png_image.hpp — OUR OWN PNG decoder (zero 3rd-party), the LDR peer of the HDR/EXR codecs and the glTF/texture import
// leg (GEO-3). Built on OUR zlib inflate (deflate.hpp). FULL core-spec coverage — no gaps:
//
//   • all five color types (gray · RGB · palette · gray+alpha · RGBA) × all legal bit depths (1/2/4/8/16)
//   • PLTE + tRNS (palette alpha, gray/RGB color-key transparency)
//   • all five scanline filters (None/Sub/Up/Average/Paeth), correct sub-byte bpp handling
//   • Adam7 INTERLACE (the 7-pass origin/stride table, per-pass defilter + scatter)
//   • per-chunk CRC-32 VERIFICATION (a corrupt chunk is rejected, never silently decoded)
//
// Output is always RGBA8 (the GPU-upload layout): 16-bit samples take the high byte (the standard downconversion),
// palette expands, gray replicates, tRNS becomes real alpha. `source_channels`/`source_bit_depth` report what the file
// actually carried. Same posture as every codec here: byte spans in, allocator-backed pixels out, no filesystem.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/ldr_image.hpp>

namespace crd::resources
{

// PNG shares the family-wide LDR types (ldr_image.hpp) — the aliases keep the codec-local spelling readable.
using PngImage = LdrImage;
using PngError = LdrError;

// CRC-32 (ISO 3309 / ITU-T V.42 — the PNG chunk checksum). Exposed for chunk building (tests, the future encoder).
[[nodiscard]] crd::u32 png_crc32(crd::containers::ConstSpan<crd::u8> data) noexcept;

// True iff the 8-byte PNG signature leads `bytes`.
[[nodiscard]] bool png_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept;

// Decode a PNG byte stream into RGBA8. `out` is (re)filled on success; on failure it is left invalid.
[[nodiscard]] PngError png_decode(crd::containers::ConstSpan<crd::u8> bytes, PngImage& out, crd::memory::IAllocator* a);

} // namespace crd::resources
