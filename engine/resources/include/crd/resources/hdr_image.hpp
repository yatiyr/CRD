#pragma once

// hdr_image.hpp — B-hdr: OUR OWN high-dynamic-range image codec (zero 3rd-party, user 2026-07-12 "we must do it ourselves").
// Decodes / encodes the HDR image formats to a flat float buffer (`HdrImage`), and round-trips that image through the CRDR
// container so an imported env map / HDR texture becomes a first-class Cerid resource. Formats:
//   • Radiance RGBE (.hdr / .pic) — B-hdr-a — read + write, new+old RLE.  The classic HDR env-map format.
//   • Portable FloatMap (.pfm)    — B-hdr-b — read + write, lossless raw float.
//   • OpenEXR (.exr)              — B-hdr-c — read + write, half/float, our own DEFLATE + PIZ (added in B-hdr-c).
//
// The codec operates on byte SPANS (decode) / produces byte ARRAYS (encode) — no filesystem here (the caller does file I/O),
// which keeps it pure + round-trip testable in memory. The GPU upload of the decoded float pixels as an env-map texture is
// the renderer/rhi consumer leaf (B8-e IBL / B15 sky) — it needs a float TextureFormat wired through the GPU texture path.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::resources
{

// ── HdrImage — a decoded HDR image: a flat, row-major (top-to-bottom), channel-interleaved float buffer ─────────────────────

struct HdrImage
{
    crd::u32                          width    = 0;
    crd::u32                          height   = 0;
    crd::u32                          channels = 0;   // 3 = RGB (RGBE/EXR-RGB), 1 = grayscale (PFM Pf)
    crd::containers::Array<crd::f32>  pixels;         // size = width·height·channels; pixel (x,y) channel c at [(y·width+x)·channels+c]

    explicit HdrImage(crd::memory::IAllocator* a = crd::memory::default_allocator()) : pixels(a) {}

    [[nodiscard]] crd::u64 texel_count() const noexcept { return static_cast<crd::u64>(width) * height; }
    [[nodiscard]] bool     valid() const noexcept
    {
        return width != 0 && height != 0 && (channels == 1 || channels == 3)
            && pixels.size() == static_cast<crd::usize>(texel_count() * channels);
    }
    // Allocate the pixel buffer for (w,h,ch) and zero it.
    void resize(crd::u32 w, crd::u32 h, crd::u32 ch);
    [[nodiscard]] crd::f32&       at(crd::u32 x, crd::u32 y, crd::u32 c)       noexcept { return pixels[(static_cast<crd::usize>(y) * width + x) * channels + c]; }
    [[nodiscard]] const crd::f32& at(crd::u32 x, crd::u32 y, crd::u32 c) const noexcept { return pixels[(static_cast<crd::usize>(y) * width + x) * channels + c]; }
};

enum class HdrCodec : crd::u8 { Unknown, Radiance, Pfm, Exr };

enum class HdrError : crd::u8
{
    Ok,
    Truncated,     // ran off the end of the input
    BadMagic,      // no recognized format signature
    BadHeader,     // header present but malformed (resolution, format line, …)
    Unsupported,   // a valid-but-unhandled variant (e.g. an EXR compression we don't do)
    TooLarge,      // dimensions exceed the sane cap
    BadData,       // a decode invariant failed (bad RLE run, corrupt scanline, …)
};

// Sniff the leading bytes for a known format signature.
[[nodiscard]] HdrCodec hdr_sniff(crd::containers::ConstSpan<crd::u8> bytes) noexcept;

// ── decode ──────────────────────────────────────────────────────────────────────────────────────────────────────────────

// Auto-detect the format from the magic + decode. `out` is (re)filled on success.
[[nodiscard]] HdrError hdr_decode(crd::containers::ConstSpan<crd::u8> bytes, HdrImage& out, crd::memory::IAllocator* a);

// Format-specific decoders (call directly when the format is known).
[[nodiscard]] HdrError hdr_decode_radiance(crd::containers::ConstSpan<crd::u8> bytes, HdrImage& out, crd::memory::IAllocator* a);
[[nodiscard]] HdrError hdr_decode_pfm(crd::containers::ConstSpan<crd::u8> bytes, HdrImage& out, crd::memory::IAllocator* a);
// OpenEXR (B-hdr-c) — scanline, half/float, NONE/RLE/ZIP compression (PIZ later). Our own DEFLATE + predictor.
[[nodiscard]] HdrError hdr_decode_exr(crd::containers::ConstSpan<crd::u8> bytes, HdrImage& out, crd::memory::IAllocator* a);

// ── encode ──────────────────────────────────────────────────────────────────────────────────────────────────────────────

// Encode `img` to a byte blob in the requested format. Returns an empty array on failure (invalid image).
[[nodiscard]] crd::containers::Array<crd::u8> hdr_encode_radiance(const HdrImage& img, crd::memory::IAllocator* a);
[[nodiscard]] crd::containers::Array<crd::u8> hdr_encode_pfm(const HdrImage& img, bool little_endian, crd::memory::IAllocator* a);

// OpenEXR encode. `pixel_type`: HALF (2 bytes) or FLOAT (4 bytes). `compression`: None / Rle / Zip (our own DEFLATE) / Piz
// (our own wavelet+Huffman). The enum values ARE the OpenEXR on-disk compression ids. ZIP = 16-line blocks; PIZ = 32.
enum class ExrCompression : crd::u8 { None = 0, Rle = 1, Zip = 3, Piz = 4 };
enum class ExrPixelType  : crd::u8 { Half = 1, Float = 2 };
[[nodiscard]] crd::containers::Array<crd::u8> hdr_encode_exr(const HdrImage& img, ExrPixelType pixel_type, ExrCompression compression, crd::memory::IAllocator* a);

// ── CRDR round-trip (an HDR image as a first-class Cerid resource) ───────────────────────────────────────────────────────

// Serialize `img` into a CRDR container (type 'HDRI'): a HEAD chunk (w,h,channels,float-format) + a PIXF chunk of raw f32
// pixels. Deterministic, lossless. `id` tags the resource.
[[nodiscard]] crd::containers::Array<crd::u8> hdr_to_crdr(const HdrImage& img, ResourceId id, crd::memory::IAllocator* a);

// Parse a CRDR 'HDRI' container back into `img` (the inverse of hdr_to_crdr).
[[nodiscard]] HdrError hdr_from_crdr(crd::containers::ConstSpan<crd::u8> bytes, HdrImage& out, crd::memory::IAllocator* a);

} // namespace crd::resources
