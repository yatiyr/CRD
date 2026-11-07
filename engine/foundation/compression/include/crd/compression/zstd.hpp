#pragma once

// zstd.hpp — the engine's binding of the pinned zstd (RFC 8878 frames): the bound of a compressed frame, one-shot
// compression at a level and exact one-shot decompression into a buffer the caller sized. zstd's own header stays
// inside this module; every engine use of zstd goes through here (capability compression.codecs, OWN.1).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;
struct ZSTD_CDict_s;
struct ZSTD_DDict_s;

namespace crd::memory
{
class IAllocator;
}

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

// ---- Small messages against one dictionary ----
//
// Each message is one frame on its own, so a message a reader refused to open never costs it the next one, and nothing
// of an earlier message shares a window with a later one. Frames drop what the caller already knows: no magic number,
// content size, dictionary identifier or checksum. All memory comes from the injected allocator at construction, sized
// for one bound on message size, and zstd cannot allocate after that (network foundation S32).

// A trained dictionary (zstd_train_dictionary), or raw content both ends hold, compiled for both directions. Read-only
// after construction, so any number of coders on any threads may use it.
class ZstdDictionary
{
public:
    // `bytes` must outlive the dictionary.
    ZstdDictionary(crd::memory::IAllocator& allocator, crd::containers::ConstSpan<crd::u8> bytes,
                   crd::u32 max_message_bytes, int level) noexcept;
    ~ZstdDictionary();
    ZstdDictionary(const ZstdDictionary&) = delete;
    ZstdDictionary& operator=(const ZstdDictionary&) = delete;

    [[nodiscard]] bool ready() const noexcept { return m_compress != nullptr && m_decompress != nullptr; }

private:
    friend class ZstdMessageCoder;

    crd::memory::IAllocator& m_allocator;
    void* m_memory = nullptr;
    const ZSTD_CDict_s* m_compress = nullptr;
    const ZSTD_DDict_s* m_decompress = nullptr;
};

// One thread's contexts. `max_message_bytes` and `level` must be those of the dictionaries it uses.
class ZstdMessageCoder
{
public:
    ZstdMessageCoder(crd::memory::IAllocator& allocator, crd::u32 max_message_bytes, int level) noexcept;
    ~ZstdMessageCoder();
    ZstdMessageCoder(const ZstdMessageCoder&) = delete;
    ZstdMessageCoder& operator=(const ZstdMessageCoder&) = delete;

    [[nodiscard]] bool ready() const noexcept { return m_compress != nullptr && m_decompress != nullptr; }

    // Writes `in`'s frame to `out` and returns its size; zero when `out` is too small, which a caller uses to give up
    // once compressing has not paid.
    [[nodiscard]] crd::usize compress(const ZstdDictionary& dictionary, crd::containers::ConstSpan<crd::u8> in,
                                      crd::containers::Span<crd::u8> out) noexcept;

    // True only when `in` decodes to exactly out.size() bytes. Hostile input: `out` bounds what it produces, and the
    // work is linear in `in` and `out`.
    [[nodiscard]] bool decompress_exact(const ZstdDictionary& dictionary, crd::containers::ConstSpan<crd::u8> in,
                                        crd::containers::Span<crd::u8> out) noexcept;

private:
    crd::memory::IAllocator& m_allocator;
    void* m_memory = nullptr;
    ZSTD_CCtx_s* m_compress = nullptr;
    ZSTD_DCtx_s* m_decompress = nullptr;
};

// Trains a dictionary on the concatenated `samples`, whose sizes are `sizes`, and returns its size; zero when zstd
// cannot (too few samples, or too little content for the room). Not for a hot path: it allocates freely.
[[nodiscard]] crd::usize zstd_train_dictionary(crd::containers::ConstSpan<crd::u8> samples,
                                               crd::containers::ConstSpan<crd::usize> sizes,
                                               crd::containers::Span<crd::u8> out) noexcept;

} // namespace crd::compression
