// tests/foundation/compression/test_zstd.cpp — crd-compression's zstd binding: the decode bound, the compress bound,
// one-shot compression and exact one-shot decompression, with every refusal.

#include <crd/compression/zstd.hpp>
#include <crd/containers/array.hpp>
#include <crd/core/seed_generators.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::compression;

namespace
{
// `n` bytes: a repeating phrase when `compressible`, SplitMix64 output otherwise.
Array<u8> make_bytes(usize n, bool compressible, u64 seed)
{
    Array<u8> bytes;
    bytes.resize(n);
    u64 state = seed;
    for (usize i = 0; i < n; ++i)
    {
        bytes[i] = compressible ? static_cast<u8>("cerid compression "[i % 18U])
                                : static_cast<u8>(crd::splitmix64_next(state));
    }
    return bytes;
}

ConstSpan<u8> view(const Array<u8>& bytes)
{
    return ConstSpan<u8>(bytes.data(), bytes.size());
}

bool same(const Array<u8>& a, const Array<u8>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}

Array<u8> compressed(const Array<u8>& bytes, int level = kZstdDefaultLevel)
{
    Array<u8> frame;
    REQUIRE(zstd_compress(view(bytes), level, frame));
    return frame;
}

// Decode `frame` into a buffer of `size` bytes; `decoded` keeps what was written.
bool decode(const Array<u8>& frame, usize size, Array<u8>& decoded)
{
    decoded.clear();
    decoded.resize(size);
    return zstd_decompress_exact(view(frame), Span<u8>(decoded.data(), decoded.size()));
}
} // namespace

TEST_CASE("zstd: the decode bound is 128 KiB per 3 stored bytes and saturates", "[compression][zstd]")
{
    STATIC_REQUIRE(zstd_max_decompressed_size(0U) == 0U);
    STATIC_REQUIRE(zstd_max_decompressed_size(2U) == 0U);
    STATIC_REQUIRE(zstd_max_decompressed_size(3U) == kZstdBlockMaxBytes);
    STATIC_REQUIRE(zstd_max_decompressed_size(8U) == 2U * kZstdBlockMaxBytes);
    // The last block count whose product fits, and the first that would wrap.
    constexpr u64 last_blocks = ~u64{0} / kZstdBlockMaxBytes;
    STATIC_REQUIRE(zstd_max_decompressed_size(3U * last_blocks) == last_blocks * kZstdBlockMaxBytes);
    STATIC_REQUIRE(zstd_max_decompressed_size(3U * (last_blocks + 1U)) == ~u64{0});
    STATIC_REQUIRE(zstd_max_decompressed_size(~u64{0}) == ~u64{0});
}

TEST_CASE("zstd: the compress bound covers its input and refuses a size past one frame", "[compression][zstd]")
{
    CHECK(zstd_compress_bound(0U) > 0U);
    CHECK(zstd_compress_bound(1U) > 1U);
    CHECK(zstd_compress_bound(1U << 20U) > (1U << 20U));
    CHECK(zstd_compress_bound(~usize{0}) == 0U);
}

TEST_CASE("zstd: a frame decodes to exactly its input at every level", "[compression][zstd]")
{
    for (const bool compressible : {true, false})
    {
        for (const usize n : {usize{0}, usize{1}, usize{17}, usize{4096}, usize{300000}})
        {
            for (const int level : {-5, 1, kZstdDefaultLevel, 19, 1000})
            {
                CAPTURE(compressible, n, level);
                const Array<u8> input = make_bytes(n, compressible, 0x5EEDU + n);
                const Array<u8> frame = compressed(input, level);
                CHECK(frame.size() <= zstd_compress_bound(n));
                Array<u8> decoded;
                CHECK(decode(frame, n, decoded));
                CHECK(same(decoded, input));
            }
        }
    }
}

TEST_CASE("zstd: compression is deterministic, shrinks a repetitive input and replaces the output",
          "[compression][zstd]")
{
    const Array<u8> input = make_bytes(65536U, true, 1U);
    const Array<u8> first = compressed(input);
    CHECK(first.size() < input.size() / 16U);
    Array<u8> second = make_bytes(999U, false, 2U); // stale contents the call must replace
    REQUIRE(zstd_compress(view(input), kZstdDefaultLevel, second));
    CHECK(same(first, second));
}

TEST_CASE("zstd: an input too large for one frame is refused with an empty output", "[compression][zstd]")
{
    const u8 byte = 0U;
    Array<u8> out = make_bytes(16U, false, 3U);
    // Only the size is read: the bound refuses it before any byte is touched.
    CHECK_FALSE(zstd_compress(ConstSpan<u8>(&byte, ~usize{0}), kZstdDefaultLevel, out));
    CHECK(out.empty());
}

TEST_CASE("zstd: decoding refuses a size claim it cannot meet exactly", "[compression][zstd]")
{
    const Array<u8> input = make_bytes(5000U, true, 4U);
    const Array<u8> frame = compressed(input);
    Array<u8> decoded;
    REQUIRE(decode(frame, input.size(), decoded));
    CHECK_FALSE(decode(frame, input.size() + 1U, decoded)); // decodes to fewer bytes than claimed
    CHECK_FALSE(decode(frame, input.size() - 1U, decoded)); // more bytes than the buffer holds
    CHECK_FALSE(decode(frame, 0U, decoded));
}

TEST_CASE("zstd: decoding refuses a truncated or corrupt frame", "[compression][zstd]")
{
    const Array<u8> input = make_bytes(5000U, false, 5U);
    const Array<u8> frame = compressed(input);
    Array<u8> decoded;

    Array<u8> truncated = frame;
    truncated.resize(frame.size() - 1U);
    CHECK_FALSE(decode(truncated, input.size(), decoded));

    Array<u8> bad_magic = frame;
    bad_magic[0] = static_cast<u8>(bad_magic[0] ^ 0xFFU);
    CHECK_FALSE(decode(bad_magic, input.size(), decoded));

    Array<u8> empty;
    CHECK_FALSE(decode(empty, input.size(), decoded));
}

TEST_CASE("zstd: concatenated frames decode to their inputs in order", "[compression][zstd]")
{
    const Array<u8> a = make_bytes(3000U, true, 6U);
    const Array<u8> b = make_bytes(700U, false, 7U);
    Array<u8> frames = compressed(a);
    const Array<u8> second = compressed(b);
    for (usize i = 0; i < second.size(); ++i)
    {
        frames.push_back(second[i]);
    }
    Array<u8> expected = a;
    for (usize i = 0; i < b.size(); ++i)
    {
        expected.push_back(b[i]);
    }
    Array<u8> decoded;
    CHECK(decode(frames, expected.size(), decoded));
    CHECK(same(decoded, expected));
}
