#pragma once

// hdr_piz.hpp — private interface for the B-hdr-c PIZ codec (OpenEXR wavelet + Huffman). Our own implementation of the
// algorithms (the resulting code is ours; the algorithms are the published OpenEXR format, like DEFLATE is RFC 1951).
// Operates on the SAME uncompressed EXR block layout as NONE/RLE/ZIP: per scanline, per channel (chlist order), a planar
// row of `width · word_count[c]` little-endian 16-bit words.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::resources::piz_detail
{

// Decompress a PIZ block into `out_raw` (size = width·nlines·Σword_counts·2 bytes). `word_counts[c]` = 16-bit words per
// sample of channel c (1 = HALF, 2 = FLOAT/UINT). Returns false on a corrupt stream.
[[nodiscard]] bool piz_uncompress(
    const crd::u8* data, crd::usize dsz, crd::u32 width, crd::u32 nlines,
    const crd::u8* word_counts, crd::u32 nchan,
    crd::u8* out_raw, crd::usize out_raw_size, crd::memory::IAllocator* a);

// Compress `raw` (the uncompressed block, out_raw layout above) into a PIZ block, returned as a byte array.
[[nodiscard]] crd::containers::Array<crd::u8> piz_compress(
    const crd::u8* raw, crd::usize raw_size, crd::u32 width, crd::u32 nlines,
    const crd::u8* word_counts, crd::u32 nchan, crd::memory::IAllocator* a);

} // namespace crd::resources::piz_detail
