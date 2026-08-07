#pragma once

// gif_image.hpp — OUR OWN GIF decoder (zero 3rd-party), the LDR peer of PNG/JPEG/TGA/BMP and the first MED-1 codec
// (D-007 Track D / the media band). GIF uses VARIABLE-WIDTH LZW (not deflate — PNG's zlib inflate does not apply), so
// this brings the first LZW decompressor into the engine. Covers GIF87a + GIF89a: the logical screen descriptor, global
// AND local color tables, the graphic-control-extension transparency index, and 4-pass INTERLACE — decoded to the
// family-wide RGBA8 `LdrImage`.
//
// SCOPE (MED-1 first increment): the FIRST image frame is decoded (the common single-frame case). ANIMATION (multi-frame
// disposal/compositing over the logical screen) is a NAMED follow-up, not silently mis-decoded. Same posture as every
// codec here: byte spans in, allocator-backed RGBA8 out, no filesystem; malformed input is REFUSED BY NAME.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/ldr_image.hpp>

namespace crd::resources
{

// GIF shares the family-wide LDR types (ldr_image.hpp) — the aliases keep the codec-local spelling readable.
using GifImage = LdrImage;
using GifError = LdrError;

// True iff a "GIF87a" or "GIF89a" signature leads `bytes`.
[[nodiscard]] bool gif_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept;

// Decode a GIF byte stream's FIRST frame into RGBA8 at the frame's own width/height. `out` is (re)filled on success;
// on failure it is left invalid. Palette indices map through the local table if present, else the global table;
// the graphic-control transparency index (GIF89a) becomes alpha 0. `source_channels` is reported as 4 (indexed→RGBA),
// `source_bit_depth` as 8.
[[nodiscard]] GifError gif_decode(crd::containers::ConstSpan<crd::u8> bytes, GifImage& out, crd::memory::IAllocator* a);

} // namespace crd::resources
