#pragma once

// deflate.hpp — OUR OWN DEFLATE (RFC 1951) + zlib (RFC 1950) codec, zero 3rd-party. Built for B-hdr-c step 2 (OpenEXR
// ZIP compression uses the zlib format), but a general-purpose lossless byte codec. `inflate_raw` handles all three
// DEFLATE block types (stored, fixed-Huffman, dynamic-Huffman) so it reads real zlib, PNG, OpenEXR and zip streams;
// `deflate_raw` emits a single fixed-Huffman LZ77 block (correct + compresses; ratio is secondary).
//
// Inflate decodes into a buffer of exactly the size the caller knows (a zip directory, an image or chunk header): the
// buffer is the LZ77 window, nothing is allocated, and a stream that decodes to any other size is refused, so a claim
// never grows memory past what the caller sized. `deflate_max_inflated_size` bounds a claim before it is allocated.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::compression
{

// The most bytes DEFLATE can expand one compressed byte to: a 258-byte match coded in two bits (a one-bit length code
// and a one-bit distance code), so four matches per byte.
inline constexpr crd::u64 kDeflateMaxRatio = 1032U;

// The largest decoded size `compressed_bytes` of raw DEFLATE can claim (saturating); a claim above it is corrupt.
[[nodiscard]] constexpr crd::u64 deflate_max_inflated_size(crd::u64 compressed_bytes) noexcept
{
    return compressed_bytes > ~crd::u64{0} / kDeflateMaxRatio ? ~crd::u64{0} : compressed_bytes * kDeflateMaxRatio;
}

// Adler-32 checksum (the zlib trailer).
[[nodiscard]] crd::u32 adler32(crd::containers::ConstSpan<crd::u8> data) noexcept;

// Raw DEFLATE (no wrapper) into `out`, whose size is the exact decoded size. Returns false on a corrupt or truncated
// stream and on one that decodes to any other size (`out` then holds unspecified bytes). Allocation-free.
[[nodiscard]] bool inflate_raw(crd::containers::ConstSpan<crd::u8> in, crd::containers::Span<crd::u8> out) noexcept;
[[nodiscard]] crd::containers::Array<crd::u8> deflate_raw(crd::containers::ConstSpan<crd::u8> in,
                                                          crd::memory::IAllocator* a);

// zlib format (RFC 1950): 2-byte header + raw DEFLATE + 4-byte Adler-32. `zlib_inflate` decodes into `out` as
// `inflate_raw` does and also verifies the header and the checksum.
[[nodiscard]] bool zlib_inflate(crd::containers::ConstSpan<crd::u8> in, crd::containers::Span<crd::u8> out) noexcept;
[[nodiscard]] crd::containers::Array<crd::u8> zlib_deflate(crd::containers::ConstSpan<crd::u8> in,
                                                           crd::memory::IAllocator* a);

} // namespace crd::compression
