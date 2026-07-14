#pragma once

// deflate.hpp — OUR OWN DEFLATE (RFC 1951) + zlib (RFC 1950) codec, zero 3rd-party. Built for B-hdr-c step 2 (OpenEXR ZIP
// compression uses the zlib format), but a general-purpose lossless byte codec. `inflate` handles all three DEFLATE block
// types (stored, fixed-Huffman, dynamic-Huffman) so it reads real zlib/OpenEXR streams; `deflate` emits a single fixed-Huffman
// LZ77 block (correct + compresses; ratio is secondary).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::resources
{

// Adler-32 checksum (the zlib trailer).
[[nodiscard]] crd::u32 adler32(crd::containers::ConstSpan<crd::u8> data) noexcept;

// Raw DEFLATE (no wrapper). `out` is appended to; returns false on a corrupt/truncated stream.
[[nodiscard]] bool inflate_raw(crd::containers::ConstSpan<crd::u8> in, crd::containers::Array<crd::u8>& out);
[[nodiscard]] crd::containers::Array<crd::u8> deflate_raw(crd::containers::ConstSpan<crd::u8> in, crd::memory::IAllocator* a);

// zlib format (RFC 1950): 2-byte header + raw DEFLATE + 4-byte Adler-32. `zlib_inflate` verifies the checksum.
[[nodiscard]] bool zlib_inflate(crd::containers::ConstSpan<crd::u8> in, crd::containers::Array<crd::u8>& out);
[[nodiscard]] crd::containers::Array<crd::u8> zlib_deflate(crd::containers::ConstSpan<crd::u8> in, crd::memory::IAllocator* a);

} // namespace crd::resources
