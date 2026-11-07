// crd-fuzz-compression-messages: crd-compression's entropy coder and dictionary frames for small messages against any
// bytes.
//
// Input. The first byte picks a mode and the second and third the size asked for, then the rest is the message or
// coding.
//
// Oracles:
//   * what the entropy coder decodes re-encodes to exactly its input (one encoding), and any bytes code and decode
//   back;
//   * a coding asked for one byte more or fewer than it holds, or with a byte more of input, is refused;
//   * what a dictionary frame decodes compresses and decodes back, and any bytes compress and decode back;
//   * no room is a clean zero, never a write past `out`.
#include <crd/compression/entropy.hpp>
#include <crd/compression/zstd.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstring>

namespace
{
using crd::u32;
using crd::u8;
using crd::usize;
using crd::compression::EntropyModel;
using crd::compression::EntropyTrainer;
using crd::compression::kEntropyModelBytes;
using crd::compression::ZstdDictionary;
using crd::compression::ZstdMessageCoder;
using crd::containers::ConstSpan;
using crd::containers::Span;

constexpr usize kMaxMessage = 4096U;
constexpr int kLevel = crd::compression::kZstdDefaultLevel;

constexpr u8 kDictionary[] = "the quick brown fox jumps over the lazy dog; the five boxing wizards jump quickly; "
                             "pack my box with five dozen liquor jugs; sphinx of black quartz, judge my vow.";

struct Fixture
{
    Fixture()
        : heap(32U * 1024U * 1024U),
          dictionary(heap, ConstSpan<u8>(kDictionary, sizeof(kDictionary) - 1U), kMaxMessage, kLevel),
          coder(heap, kMaxMessage, kLevel)
    {
        EntropyTrainer trainer;
        u8 sample[1024] = {};
        for (usize i = 0U; i < sizeof(sample); i += 7U)
        {
            sample[i] = static_cast<u8>(i * 13U);
        }
        trainer.add(sample);
        u8 source[kEntropyModelBytes];
        trainer.write_model(source);
        CRD_FUZZ_REQUIRE(entropy.load(source));
        CRD_FUZZ_REQUIRE(dictionary.ready() && coder.ready());
    }

    crd::memory::TlsfAllocator heap;
    ZstdDictionary dictionary;
    ZstdMessageCoder coder;
    EntropyModel entropy;
};

Fixture& fixture()
{
    static Fixture instance;
    return instance;
}

u8 g_a[kMaxMessage * 3U];
u8 g_b[kMaxMessage * 3U];

void entropy_decode(ConstSpan<u8> coded, usize size)
{
    const EntropyModel& model = fixture().entropy;
    if (!model.decode(coded, Span<u8>(g_a, size)))
    {
        return;
    }
    const usize again = model.encode(ConstSpan<u8>(g_a, size), Span<u8>(g_b, sizeof(g_b)));
    CRD_FUZZ_REQUIRE(again == coded.size());
    CRD_FUZZ_REQUIRE(std::memcmp(g_b, coded.data(), again) == 0);
    CRD_FUZZ_REQUIRE(!model.decode(coded, Span<u8>(g_a, size + 1U)));
    if (size != 0U)
    {
        CRD_FUZZ_REQUIRE(!model.decode(coded, Span<u8>(g_a, size - 1U)));
    }
}

void entropy_round_trip(ConstSpan<u8> plain, usize damage)
{
    const EntropyModel& model = fixture().entropy;
    const usize coded = model.encode(plain, Span<u8>(g_a, sizeof(g_a)));
    CRD_FUZZ_REQUIRE(coded != 0U);
    CRD_FUZZ_REQUIRE(model.decode(ConstSpan<u8>(g_a, coded), Span<u8>(g_b, plain.size())));
    CRD_FUZZ_REQUIRE(plain.empty() || std::memcmp(g_b, plain.data(), plain.size()) == 0);
    CRD_FUZZ_REQUIRE(model.encode(plain, Span<u8>(g_b, coded - 1U)) == 0U);
    CRD_FUZZ_REQUIRE(!model.decode(ConstSpan<u8>(g_a, coded - 1U), Span<u8>(g_b, plain.size())));

    // Damaged, it is another message's one encoding or nothing at all.
    static u8 damaged[sizeof(g_a)];
    std::memcpy(damaged, g_a, coded);
    damaged[damage % coded] = static_cast<u8>(damaged[damage % coded] ^ (1U << (damage % 8U)));
    entropy_decode(ConstSpan<u8>(damaged, coded), plain.size());
}

void dictionary_decode(ConstSpan<u8> frame, usize size)
{
    Fixture& f = fixture();
    if (!f.coder.decompress_exact(f.dictionary, frame, Span<u8>(g_a, size)))
    {
        return;
    }
    const usize again = f.coder.compress(f.dictionary, ConstSpan<u8>(g_a, size), Span<u8>(g_b, sizeof(g_b)));
    CRD_FUZZ_REQUIRE(again != 0U);
    u8 back[kMaxMessage];
    CRD_FUZZ_REQUIRE(f.coder.decompress_exact(f.dictionary, ConstSpan<u8>(g_b, again), Span<u8>(back, size)));
    CRD_FUZZ_REQUIRE(size == 0U || std::memcmp(back, g_a, size) == 0);
}

void dictionary_round_trip(ConstSpan<u8> plain)
{
    Fixture& f = fixture();
    const usize coded = f.coder.compress(f.dictionary, plain, Span<u8>(g_a, sizeof(g_a)));
    CRD_FUZZ_REQUIRE(coded != 0U);
    CRD_FUZZ_REQUIRE(f.coder.decompress_exact(f.dictionary, ConstSpan<u8>(g_a, coded), Span<u8>(g_b, plain.size())));
    CRD_FUZZ_REQUIRE(plain.empty() || std::memcmp(g_b, plain.data(), plain.size()) == 0);
    CRD_FUZZ_REQUIRE(f.coder.compress(f.dictionary, plain, Span<u8>(g_b, 1U)) == 0U);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 3U)
    {
        return 0;
    }
    const u8 mode = data[0];
    const usize asked = ((usize{data[1]} << 8U) | data[2]) % kMaxMessage;
    usize left = size - 3U;
    left = (left < kMaxMessage) ? left : kMaxMessage;
    const ConstSpan<u8> rest(data + 3U, left);
    switch (mode % 4U)
    {
        case 0U:
            entropy_decode(rest, asked);
            break;
        case 1U:
            entropy_round_trip(rest, asked);
            break;
        case 2U:
            dictionary_decode(rest, asked);
            break;
        default:
            dictionary_round_trip(rest);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    u8 sparse[3U + 300U] = {1U, 0U, 16U};
    for (usize i = 3U; i < sizeof(sparse); i += 9U)
    {
        sparse[i] = static_cast<u8>(i);
    }
    sink.add("entropy-round-trip", sparse, sizeof(sparse));
    sparse[0] = 3U;
    sink.add("dictionary-round-trip", sparse, sizeof(sparse));
    constexpr char text[] = "\x03\x00\x00the quick brown fox and the five boxing wizards, again and again and again";
    sink.add("dictionary-text", reinterpret_cast<const u8*>(text), sizeof(text) - 1U);
    // The coding of nothing, and of nothing with a stray byte, and a state below the range.
    const u8 nothing[] = {0U, 0U, 0U, 1U, 0U, 0U};
    sink.add("entropy-nothing", nothing, sizeof(nothing));
    const u8 stray[] = {0U, 0U, 0U, 1U, 0U, 0U, 7U};
    sink.add("entropy-stray", stray, sizeof(stray));
    const u8 low[] = {0U, 0U, 4U, 0U, 9U, 9U, 9U};
    sink.add("entropy-low", low, sizeof(low));
    // A zstd block header that claims more than is here.
    const u8 frame[] = {2U, 0U, 200U, 0x20U, 0x40U, 0x01U, 0x00U, 0x00U, 0x61U};
    sink.add("dictionary-short", frame, sizeof(frame));
}
