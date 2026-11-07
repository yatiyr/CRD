// crd-compression's small messages against a dictionary (NET.3, network foundation S32).

#include <crd/compression/zstd.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string_view>

using crd::u32;
using crd::u8;
using crd::usize;
using crd::compression::ZstdDictionary;
using crd::compression::ZstdMessageCoder;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::memory::TlsfAllocator;

namespace
{
constexpr u32 kMessageBytes = 8192U;
constexpr int kLevel = crd::compression::kZstdDefaultLevel;
constexpr std::string_view kKnown = "the player joined the room and left the match; the server restarted the round. "
                                    "score updated for team blue and team red; connection lost; connection restored. ";

[[nodiscard]] ConstSpan<u8> bytes_of(std::string_view text)
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(text.data()), text.size());
}

struct Fixture
{
    TlsfAllocator heap{16U * 1024U * 1024U};
    ZstdDictionary dictionary{heap, bytes_of(kKnown), kMessageBytes, kLevel};
    ZstdMessageCoder coder{heap, kMessageBytes, kLevel};
};
} // namespace

TEST_CASE("zstd messages: a small message costs what the dictionary does not already hold", "[compression][zstd]")
{
    Fixture f;
    REQUIRE(f.dictionary.ready());
    REQUIRE(f.coder.ready());
    constexpr std::string_view message = "the player left the match; connection restored.";

    u8 frame[256];
    const usize size = f.coder.compress(f.dictionary, bytes_of(message), frame);
    REQUIRE(size != 0U);
    // No magic number, and a descriptor saying no content size, no dictionary identifier and no checksum.
    CHECK(frame[0] == 0U);
    CHECK(size * 2U < message.size());

    u8 back[message.size()];
    const ConstSpan<u8> coded(frame, size);
    REQUIRE(f.coder.decompress_exact(f.dictionary, coded, back));
    CHECK(std::memcmp(back, message.data(), sizeof(back)) == 0);

    // A plain zstd reader does not take it: nothing in it says it is zstd.
    CHECK_FALSE(crd::compression::zstd_decompress_exact(coded, back));

    // Each size but its own, and a frame cut short, are refused.
    u8 wider[message.size() + 1U];
    CHECK_FALSE(f.coder.decompress_exact(f.dictionary, coded, wider));
    CHECK_FALSE(f.coder.decompress_exact(f.dictionary, coded, Span<u8>(back, sizeof(back) - 1U)));
    CHECK_FALSE(f.coder.decompress_exact(f.dictionary, coded.first(size - 1U), back));
}

TEST_CASE("zstd messages: running out of room is no gain, and costs the next message nothing", "[compression][zstd]")
{
    Fixture f;
    constexpr std::string_view message = "score updated for team blue and team red; the server restarted the round.";
    u8 frame[256];
    const usize size = f.coder.compress(f.dictionary, bytes_of(message), frame);
    REQUIRE(size > 1U);
    CHECK(f.coder.compress(f.dictionary, bytes_of(message), Span<u8>(frame, size - 1U)) == 0U);
    CHECK(f.coder.compress(f.dictionary, bytes_of(message), Span<u8>(frame, 0U)) == 0U);
    CHECK(f.coder.compress(f.dictionary, bytes_of(message), frame) == size);
    u8 back[message.size()];
    REQUIRE(f.coder.decompress_exact(f.dictionary, ConstSpan<u8>(frame, size), back));
    CHECK(std::memcmp(back, message.data(), sizeof(back)) == 0);
}

TEST_CASE("zstd messages: any size up to the bound, on any of several dictionaries", "[compression][zstd]")
{
    Fixture f;
    constexpr std::string_view other_text =
        "pack my box with five dozen liquor jugs; sphinx of black quartz, judge my vow.";
    ZstdDictionary other(f.heap, bytes_of(other_text), kMessageBytes, kLevel);
    REQUIRE(other.ready());
    Array<u8> plain;
    Array<u8> frame;
    Array<u8> back;
    plain.resize(kMessageBytes);
    frame.resize(crd::compression::zstd_compress_bound(kMessageBytes));
    back.resize(kMessageBytes);
    for (usize i = 0U; i < kMessageBytes; ++i)
    {
        plain[i] = (i % 3U == 0U) ? static_cast<u8>((i * 2654435761U) >> 13U) : static_cast<u8>(kKnown[i % 40U]);
    }
    for (const usize size : {usize{0U}, usize{1U}, usize{2U}, usize{63U}, usize{1024U}, usize{kMessageBytes}})
    {
        for (const ZstdDictionary* dictionary : {&f.dictionary, &other})
        {
            const usize coded =
                f.coder.compress(*dictionary, ConstSpan<u8>(plain.data(), size), Span<u8>(frame.data(), frame.size()));
            REQUIRE(coded != 0U);
            REQUIRE(
                f.coder.decompress_exact(*dictionary, ConstSpan<u8>(frame.data(), coded), Span<u8>(back.data(), size)));
            REQUIRE(((size == 0U) || std::memcmp(back.data(), plain.data(), size) == 0));
        }
    }
}

TEST_CASE("zstd messages: an allocator's refusal is reported, not hit later", "[compression][zstd]")
{
    TlsfAllocator small(4096U);
    const ZstdDictionary dictionary(small, bytes_of(kKnown), kMessageBytes, kLevel);
    CHECK_FALSE(dictionary.ready());
    const ZstdMessageCoder coder(small, kMessageBytes, kLevel);
    CHECK_FALSE(coder.ready());
}

TEST_CASE("zstd messages: a trained dictionary beats none", "[compression][zstd]")
{
    Array<u8> samples;
    Array<usize> sizes;
    u32 state = 1U;
    const auto next = [&state]
    {
        state = (state * 1664525U) + 1013904223U;
        return state >> 8U;
    };
    static constexpr std::string_view kWords[] = {"join",   "leave",  "move",    "score", "chat", "team",
                                                  "blue",   "red",    "room",    "round", "hero", "tower",
                                                  "server", "update", "restart", "lost"};
    const auto sentence = [&](Array<u8>& out)
    {
        const usize before = out.size();
        for (u32 word = 0U; word < 12U; ++word)
        {
            for (const char c : kWords[next() % 16U])
            {
                out.push_back(static_cast<u8>(c));
            }
            out.push_back(static_cast<u8>(' '));
        }
        return out.size() - before;
    };
    for (u32 i = 0U; i < 1000U; ++i)
    {
        sizes.push_back(sentence(samples));
    }
    u8 trained[4096];
    const usize size = crd::compression::zstd_train_dictionary(ConstSpan<u8>(samples.data(), samples.size()),
                                                               ConstSpan<usize>(sizes.data(), sizes.size()), trained);
    REQUIRE(size != 0U);
    REQUIRE(size <= sizeof(trained));

    TlsfAllocator heap(16U * 1024U * 1024U);
    ZstdDictionary dictionary(heap, ConstSpan<u8>(trained, size), kMessageBytes, kLevel);
    ZstdMessageCoder coder(heap, kMessageBytes, kLevel);
    REQUIRE(dictionary.ready());
    REQUIRE(coder.ready());
    Array<u8> message;
    const usize plain = sentence(message);
    u8 frame[512];
    const usize coded = coder.compress(dictionary, ConstSpan<u8>(message.data(), plain), frame);
    REQUIRE(coded != 0U);
    CHECK(coded < plain / 2U);
    Array<u8> back;
    back.resize(plain);
    CHECK(coder.decompress_exact(dictionary, ConstSpan<u8>(frame, coded), Span<u8>(back.data(), plain)));
    CHECK(std::memcmp(back.data(), message.data(), plain) == 0);

    // Too little to learn from.
    u8 none[1024];
    const usize one[] = {4U};
    const u8 sample[] = {1U, 2U, 3U, 4U};
    CHECK(crd::compression::zstd_train_dictionary(sample, one, none) == 0U);
}
