#pragma once

// zstd.hpp — the engine's binding of the pinned zstd (RFC 8878 frames): the bound of a compressed frame, one-shot
// compression at a level and exact one-shot decompression into a buffer the caller sized. zstd's own header stays
// inside this module; every engine use of zstd goes through here (capability compression.codecs, OWN.1).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::compression
{

// The level the engine's cookers compress at (zstd's own default).
inline constexpr int kZstdDefaultLevel = 3;

// The most bytes one zstd block decodes to (RFC 8878, Block_Maximum_Size).
inline constexpr crd::u64 kZstdBlockMaxBytes = 128U * 1024U;

// The most bytes `compressed_size` bytes of zstd frames can decode to: every block carries a 3-byte header and decodes
// to at most kZstdBlockMaxBytes, so they hold at most compressed_size / 3 blocks. Saturates instead of wrapping, so a
// reader can bound a claimed size before it reserves anything for it.
[[nodiscard]] constexpr crd::u64 zstd_max_decompressed_size(crd::u64 compressed_size) noexcept
{
    const crd::u64 blocks = compressed_size / 3U;
    if (blocks > ~crd::u64{0} / kZstdBlockMaxBytes)
    {
        return ~crd::u64{0};
    }
    return blocks * kZstdBlockMaxBytes;
}

// The most bytes compressing `size` bytes can produce, or 0 when `size` is too large for one frame.
[[nodiscard]] crd::usize zstd_compress_bound(crd::usize size) noexcept;

// Compress `in` as one frame at `level` (zstd clamps it to its range), replacing the contents of `out` with the frame.
// False, with `out` empty, when zstd fails (an input too large for one frame).
[[nodiscard]] bool zstd_compress(crd::containers::ConstSpan<crd::u8> in, int level,
                                 crd::containers::Array<crd::u8>& out);

// Decode the frames of `in` into `out`: true only when they decode to exactly out.size() bytes. A corrupt or truncated
// frame, one that decodes to more bytes than `out` holds and one that decodes to fewer are all refused, so a caller
// that sized `out` by a claim never keeps a tail the frames did not write.
[[nodiscard]] bool zstd_decompress_exact(crd::containers::ConstSpan<crd::u8> in,
                                         crd::containers::Span<crd::u8> out) noexcept;

} // namespace crd::compression
