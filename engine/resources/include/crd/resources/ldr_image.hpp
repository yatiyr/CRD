#pragma once

// ldr_image.hpp — the SHARED LDR image type + auto-dispatch front door for the owned codec family (PNG · TGA · BMP ·
// JPEG — the HDR family lives in hdr_image.hpp with float pixels). Every codec decodes to the same RGBA8 layout (the
// GPU-upload format) and reports what the file actually carried; `ldr_decode` sniffs the magic and dispatches.
//
// The remaining codecs are SCHEDULED, not excluded: GIF/TIFF/progressive-JPEG (MED-1), WebP (MED-3), AVIF (MED-4) —
// the D-007 MED band (docs/research/2026-07-23-media-codec-platform.md) grows this family + encoders.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::resources
{

struct LdrImage
{
    crd::u32                        width  = 0;
    crd::u32                        height = 0;
    crd::containers::Array<crd::u8> pixels; // RGBA8, row-major top-to-bottom: (x,y) at [(y·width+x)·4]
    crd::u8                         source_channels  = 0; // 1/2/3/4 as authored (before the RGBA8 expansion)
    crd::u8                         source_bit_depth = 0; // per-channel bits as authored (1/2/4/5/8/16)

    explicit LdrImage(crd::memory::IAllocator* a = crd::memory::default_allocator()) : pixels(a) {}

    [[nodiscard]] bool valid() const noexcept
    {
        return width != 0U && height != 0U && pixels.size() == static_cast<crd::usize>(width) * height * 4U;
    }
};

enum class LdrError : crd::u8
{
    Ok,
    BadMagic,    // no recognized signature
    Truncated,   // bytes end inside the structure / the pixel data is short
    BadHeader,   // header malformed (illegal combination, nonzero reserved fields, …)
    BadChunkCrc, // an integrity check failed (PNG chunk CRC) — corrupt, never silently decoded
    BadData,     // the pixel stream violates the format (bad filter/RLE run, index out of range, size mismatch)
    Unsupported, // valid but out of the covered surface (progressive JPEG, CMYK, OS/2 BMP cores) — named, not silent
    TooLarge,    // dimensions above the sane cap (16384²)
};

enum class LdrCodec : crd::u8
{
    Unknown,
    Png,
    Tga, // note: TGA has NO magic — sniffed heuristically, so it dispatches LAST
    Bmp,
    Jpeg,
};

// Sniff the leading bytes. TGA (magic-less) is claimed only when its header fields are self-consistent.
[[nodiscard]] LdrCodec ldr_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept;

// Auto-detect + decode. `out` is (re)filled on success.
[[nodiscard]] LdrError ldr_decode(crd::containers::ConstSpan<crd::u8> bytes, LdrImage& out, crd::memory::IAllocator* a);

} // namespace crd::resources
