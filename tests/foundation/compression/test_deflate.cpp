// tests/foundation/compression/test_deflate.cpp — crd-compression's DEFLATE, zlib and Adler-32: round trips, a foreign
// encoder's dynamic-Huffman stream, the exact-size contract, the claim bound and every refusal of the decoder, each
// on a stream built bit by bit.

#include <crd/compression/deflate.hpp>
#include <crd/containers/array.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <new>

using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using namespace crd::compression;

namespace
{
constexpr u32 kA = 'a';

alignas(crd::memory::GrowableTlsfAllocator) unsigned char s_df_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
crd::memory::GrowableTlsfAllocator& s_df = *::new (s_df_buf) crd::memory::GrowableTlsfAllocator();

ConstSpan<u8> view(const Array<u8>& bytes)
{
    return {bytes.data(), bytes.size()};
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

// Decodes `comp` into `n` bytes with `inflate` (inflate_raw or zlib_inflate); false when refused.
template <typename Inflate> bool decode(Inflate inflate, ConstSpan<u8> comp, usize n, Array<u8>& out)
{
    out.resize(n);
    return inflate(comp, {out.data(), out.size()});
}

// Round trip at the exact size, and the exact-size refusals one byte either side.
template <typename Deflate, typename Inflate>
void roundtrip(Deflate deflate, Inflate inflate, const Array<u8>& src, const char* label)
{
    INFO(label);
    CAPTURE(src.size());
    const Array<u8> comp = deflate(view(src), &s_df);
    Array<u8> back(&s_df);
    REQUIRE(decode(inflate, view(comp), src.size(), back));
    CHECK(same(back, src));
    CHECK_FALSE(decode(inflate, view(comp), src.size() + 1U, back));
    if (src.size() > 0U)
    {
        CHECK_FALSE(decode(inflate, view(comp), src.size() - 1U, back));
    }
}
void roundtrip_both(const Array<u8>& src, const char* label)
{
    roundtrip(deflate_raw, inflate_raw, src, label);
    roundtrip(zlib_deflate, zlib_inflate, src, label);
}

// A DEFLATE stream written bit by bit: header fields least significant bit first, Huffman codes most significant first.
class Bits
{
public:
    void put(u32 value, u32 n)
    {
        for (u32 i = 0; i < n; ++i)
        {
            bit((value >> i) & 1U);
        }
    }
    void code(u32 code, u32 length)
    {
        for (u32 i = length; i-- > 0U;)
        {
            bit((code >> i) & 1U);
        }
    }
    [[nodiscard]] ConstSpan<u8> bytes() const { return view(m_bytes); }

private:
    void bit(u32 b)
    {
        if (m_used == 0U)
        {
            m_bytes.push_back(0U);
        }
        m_bytes[m_bytes.size() - 1U] = static_cast<u8>(m_bytes[m_bytes.size() - 1U] | (b << m_used));
        m_used = (m_used + 1U) & 7U;
    }
    Array<u8> m_bytes{&s_df};
    u32 m_used = 0;
};

// Canonical codes from code lengths (RFC 1951 §3.2.2).
void canonical(const u8* lengths, usize n, u16* codes)
{
    u16 count[16] = {};
    for (usize s = 0; s < n; ++s)
    {
        if (lengths[s] != 0U)
        {
            ++count[lengths[s]];
        }
    }
    u16 next[16] = {};
    u16 c = 0;
    for (usize len = 1; len < 16U; ++len)
    {
        c = static_cast<u16>((c + count[len - 1U]) << 1U);
        next[len] = c;
    }
    for (usize s = 0; s < n; ++s)
    {
        codes[s] = lengths[s] != 0U ? next[lengths[s]]++ : u16{0};
    }
}

// The fixed code (§3.2.6): literal/length symbol `sym`, and distance code `dist` in five bits.
void fixed_symbol(Bits& b, u32 sym)
{
    if (sym < 144U)
    {
        b.code(0x30U + sym, 8U);
    }
    else if (sym < 256U)
    {
        b.code(0x190U + sym - 144U, 9U);
    }
    else if (sym < 280U)
    {
        b.code(sym - 256U, 7U);
    }
    else
    {
        b.code(0xC0U + sym - 280U, 8U);
    }
}

// The code-length code {0, 1, 2, 18} at two bits each, by symbol: canonically a length is its own code and 18 is 11.
constexpr u8 kClDefault[19] = {2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};

// A dynamic block's trees (§3.2.7): `lengths` holds the hlit literal/length lengths then the hdist distance lengths,
// sent with the code-length code `cl` (lengths by symbol, through symbol 1 of the transmission order), a zero run of 11
// or more as one 18 when `cl` has 18. `overrun` lengthens a zero run that reaches the last length past the end.
void put_dynamic_trees(Bits& b, const u8* lengths, u32 hlit, u32 hdist, const u8* cl = kClDefault, u32 overrun = 0)
{
    b.put(hlit - 257U, 5U);
    b.put(hdist - 1U, 5U);
    b.put(18U - 4U, 4U);
    static constexpr u8 kOrder[18] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1};
    for (const u8 symbol : kOrder)
    {
        b.put(cl[symbol], 3U);
    }
    u16 codes[19] = {};
    canonical(cl, 19U, codes);
    const u32 n = hlit + hdist;
    for (u32 i = 0; i < n;)
    {
        u32 run = 0;
        while (i + run < n && lengths[i + run] == 0U && run < 138U)
        {
            ++run;
        }
        const u32 sent = i + run == n ? run + overrun : run;
        if (cl[18] != 0U && sent >= 11U)
        {
            b.code(codes[18], cl[18]);
            b.put(sent - 11U, 7U);
            i += run;
            continue;
        }
        b.code(codes[lengths[i]], cl[lengths[i]]);
        ++i;
    }
}

// A final dynamic block with these trees that decodes 'a' (when it has a code) and ends: valid but for whatever rule
// a test breaks in its trees.
Bits a_then_end(const u8* lengths, u32 hlit, u32 hdist, const u8* cl = kClDefault, u32 overrun = 0)
{
    Bits b;
    b.put(1U, 1U);
    b.put(2U, 2U);
    put_dynamic_trees(b, lengths, hlit, hdist, cl, overrun);
    u16 codes[288] = {};
    canonical(lengths, hlit, codes);
    b.code(codes[kA], lengths[kA]);
    b.code(codes[256], lengths[256]);
    return b;
}

// The trees for a stream of literal 'a' (97), end of block (256), length 3 (257) or 258 (285), and distance 1 (code
// 0, a single one-bit code): 'a' and 256 two bits, the length one bit; the codes it assigns.
struct Trees
{
    u8 lengths[286 + 1] = {};
    u16 lit[286] = {};
    u16 dist[1] = {};
};
Trees match_trees(u32 length_symbol)
{
    Trees t;
    t.lengths[kA] = 2U;
    t.lengths[256] = 2U;
    t.lengths[length_symbol] = 1U;
    t.lengths[286] = 1U; // distance code 0
    canonical(t.lengths, 286U, t.lit);
    canonical(t.lengths + 286, 1U, t.dist);
    return t;
}

bool inflate_bits(const Bits& b, usize n)
{
    Array<u8> out(&s_df);
    return decode(inflate_raw, b.bytes(), n, out);
}
} // namespace

TEST_CASE("deflate: Adler-32 matches the known 'Wikipedia' vector", "[compression][deflate]")
{
    const char* s = "Wikipedia";
    CHECK(adler32({reinterpret_cast<const crd::u8*>(s), 9}) == 0x11E60398U);
    const crd::u8 abc[3] = {'A', 'B', 'C'};
    CHECK(adler32({abc, 3}) == 0x018D00C7U);
}

TEST_CASE("deflate: inflate a HAND-BUILT stored zlib stream (wrapper + stored + Adler)", "[compression][deflate]")
{
    // 78 01 | [stored block: bfinal=1 btype=00] 03 00 FC FF 'A' 'B' 'C' | adler 01 8D 00 C7
    const crd::u8 stream[] = {0x78, 0x01, 0x01, 0x03, 0x00, 0xFC, 0xFF, 0x41, 0x42, 0x43, 0x01, 0x8D, 0x00, 0xC7};
    Array<crd::u8> out(&s_df);
    REQUIRE(decode(zlib_inflate, {stream, sizeof(stream)}, 3U, out));
    CHECK(out[0] == 0x41U);
    CHECK(out[1] == 0x42U);
    CHECK(out[2] == 0x43U);
    CHECK_FALSE(decode(zlib_inflate, {stream, sizeof(stream)}, 2U, out));
    CHECK_FALSE(decode(zlib_inflate, {stream, sizeof(stream)}, 4U, out));
}

TEST_CASE("deflate: inflate a REAL zlib DYNAMIC-Huffman stream (foreign encoder -> reads real .exr)",
          "[compression][deflate]")
{
    // Produced by CPython zlib.compress(payload, 6); the first block is dynamic-Huffman (btype=10). Validates the
    // dynamic path against a foreign encoder — the exact case a real OpenEXR ZIP file exercises.
    static const crd::u8 kComp[] = {
        120, 156, 237, 213, 183, 17,  96,  53,  20,  64,  81,  185, 47,  243, 164, 34,  212, 202, 122, 239, 96,  171,
        128, 136, 140, 161, 122, 102, 78,  11,  100, 204, 38,  55,  61,  225, 77,  233, 197, 139, 27,  163, 190, 124,
        121, 35,  231, 87,  175, 110, 204, 246, 250, 245, 141, 82,  222, 188, 185, 177, 210, 219, 183, 55,  106, 125,
        247, 238, 70,  228, 247, 239, 111, 180, 246, 225, 195, 141, 93,  62,  126, 188, 241, 164, 79,  159, 110, 156,
        250, 249, 243, 141, 158, 191, 124, 185, 145, 218, 215, 175, 55,  70,  249, 246, 237, 70,  78,  223, 191, 223,
        152, 245, 199, 143, 27,  37,  255, 246, 219, 141, 213, 126, 255, 253, 70,  45,  63,  127, 222, 8,   110, 227,
        110, 238, 195, 61,  220, 206, 77,  220, 193, 205, 220, 201, 45,  220, 197, 173, 220, 224, 54,  238, 230, 62,
        220, 195, 237, 220, 196, 29,  220, 204, 157, 220, 194, 93,  220, 202, 13,  110, 227, 110, 238, 195, 61,  220,
        206, 77,  220, 193, 205, 220, 201, 45,  220, 197, 173, 220, 224, 54,  238, 230, 62,  220, 195, 237, 220, 196,
        29,  220, 204, 157, 220, 194, 93,  220, 202, 13,  110, 227, 110, 238, 195, 61,  220, 206, 77,  220, 193, 205,
        220, 201, 45,  220, 197, 173, 220, 224, 54,  238, 230, 62,  220, 195, 237, 220, 196, 29,  220, 204, 157, 220,
        194, 93,  220, 202, 13,  110, 227, 110, 238, 195, 61,  220, 206, 77,  220, 193, 205, 220, 201, 45,  220, 197,
        173, 220, 224, 54,  238, 230, 62,  220, 195, 237, 220, 196, 29,  220, 204, 157, 220, 194, 93,  220, 202, 13,
        110, 227, 110, 238, 195, 61,  220, 206, 77,  220, 193, 205, 220, 201, 45,  220, 197, 173, 220, 224, 54,  238,
        230, 62,  220, 195, 237, 220, 196, 29,  220, 204, 157, 220, 194, 93,  220, 202, 13,  110, 227, 110, 238, 195,
        61,  220, 206, 77,  220, 193, 205, 220, 201, 45,  220, 197, 173, 220, 224, 54,  238, 230, 62,  220, 195, 237,
        220, 196, 29,  220, 204, 157, 220, 194, 93,  220, 202, 13,  110, 227, 110, 238, 195, 61,  220, 206, 77,  220,
        193, 205, 220, 201, 45,  220, 197, 173, 220, 224, 54,  238, 230, 62,  220, 195, 237, 220, 196, 29,  220, 204,
        157, 220, 194, 93,  220, 202, 13,  110, 227, 110, 238, 195, 61,  220, 206, 77,  220, 193, 205, 220, 201, 45,
        220, 197, 173, 220, 224, 54,  238, 230, 62,  220, 195, 237, 220, 196, 29,  220, 204, 157, 220, 194, 93,  220,
        202, 13,  110, 227, 110, 238, 195, 61,  220, 206, 77,  220, 193, 205, 220, 201, 45,  220, 197, 173, 220, 224,
        54,  238, 230, 62,  220, 195, 237, 220, 196, 29,  220, 204, 157, 220, 194, 93,  220, 202, 13,  110, 227, 110,
        238, 195, 61,  220, 206, 77,  191, 190, 240, 235, 11,  191, 190, 240, 159, 191, 240, 207, 95,  127, 255, 241,
        231, 255, 32,  255, 2,   68,  49,  143, 187};
    Array<crd::u8> out(&s_df);
    REQUIRE(decode(zlib_inflate, {kComp, sizeof(kComp)}, 2600U, out));
    crd::u64 sum = 0;
    for (crd::usize i = 0; i < out.size(); ++i)
    {
        sum += out[i];
    }
    CHECK(sum == 102315U);
    const crd::u8 first16[16] = {0, 0, 65, 65, 32, 10, 7, 3, 66, 66, 32, 10, 1, 1, 67, 67};
    const crd::u8 last16[16] = {107, 122, 113, 120, 106, 107, 122, 113, 120, 106, 107, 122, 113, 120, 106, 107};
    int bad = 0;
    for (int i = 0; i < 16; ++i)
    {
        if (out[static_cast<crd::usize>(i)] != first16[i])
        {
            ++bad;
        }
        if (out[out.size() - 16U + static_cast<crd::usize>(i)] != last16[i])
        {
            ++bad;
        }
    }
    CHECK(bad == 0);

    // The raw stream between the header and the trailer decodes to the same bytes; one byte more or less is refused.
    const ConstSpan<u8> raw{kComp + 2, sizeof(kComp) - 6U};
    Array<u8> again(&s_df);
    REQUIRE(decode(inflate_raw, raw, 2600U, again));
    CHECK(same(again, out));
    CHECK_FALSE(decode(inflate_raw, raw, 2599U, again));
    CHECK_FALSE(decode(inflate_raw, raw, 2601U, again));
    CHECK_FALSE(decode(zlib_inflate, {kComp, sizeof(kComp)}, 2599U, again));
    CHECK_FALSE(decode(zlib_inflate, {kComp, sizeof(kComp)}, 2601U, again));
    // Every truncation of the raw stream is refused.
    int accepted = 0;
    for (usize n = 0; n < raw.size(); ++n)
    {
        if (decode(inflate_raw, raw.first(n), 2600U, again))
        {
            ++accepted;
        }
    }
    CHECK(accepted == 0);
}

TEST_CASE("deflate: deflate->inflate round-trip (raw + zlib) on varied inputs", "[compression][deflate]")
{
    // empty
    {
        Array<crd::u8> e(&s_df);
        roundtrip_both(e, "empty");
    }
    // short literal
    {
        Array<crd::u8> s(&s_df);
        const char* t = "hello, deflate";
        for (const char* p = t; *p; ++p)
        {
            s.push_back(static_cast<crd::u8>(*p));
        }
        roundtrip_both(s, "hello");
    }
    // highly repetitive (LZ77 long matches + overlap)
    {
        Array<crd::u8> s(&s_df);
        for (int i = 0; i < 5000; ++i)
        {
            s.push_back(static_cast<crd::u8>('a' + (i % 3)));
        }
        roundtrip_both(s, "abc-rep");
    }
    // all-constant (distance-1 overlap runs)
    {
        Array<crd::u8> s(&s_df);
        for (int i = 0; i < 3000; ++i)
        {
            s.push_back(0x5AU);
        }
        roundtrip_both(s, "const");
    }
    // structured binary (like predicted EXR bytes)
    {
        Array<crd::u8> s(&s_df);
        crd::u32 st = 12345U;
        for (int i = 0; i < 4096; ++i)
        {
            st = st * 1103515245U + 12345U;
            s.push_back(static_cast<crd::u8>((st >> 16U) & (i % 7 == 0 ? 0xFFU : 0x0FU)));
        }
        roundtrip_both(s, "structured");
    }
    // full byte range
    {
        Array<crd::u8> s(&s_df);
        for (int i = 0; i < 256; ++i)
        {
            s.push_back(static_cast<crd::u8>(i));
        }
        roundtrip_both(s, "range256");
    }
    // past the 32 KiB window, matches reaching back across it
    {
        Array<crd::u8> s(&s_df);
        crd::u32 st = 777U;
        for (int i = 0; i < 70000; ++i)
        {
            st = st * 1103515245U + 12345U;
            s.push_back(static_cast<crd::u8>(i % 40000 < 20000 ? st >> 24U : static_cast<crd::u32>('z' + i % 5)));
        }
        roundtrip_both(s, "window");
    }
}

TEST_CASE("deflate: zlib output actually compresses a repetitive input", "[compression][deflate]")
{
    Array<crd::u8> s(&s_df);
    for (int i = 0; i < 4000; ++i)
    {
        s.push_back(static_cast<crd::u8>('x'));
    }
    Array<crd::u8> comp = zlib_deflate({s.data(), s.size()}, &s_df);
    CHECK(comp.size() < s.size() / 4U);
}

TEST_CASE("deflate: the claim bound is four 258-byte matches per byte, saturating", "[compression][deflate]")
{
    static_assert(deflate_max_inflated_size(0U) == 0U);
    static_assert(deflate_max_inflated_size(1U) == 1032U);
    static_assert(deflate_max_inflated_size(~u64{0} / 1032U) == ~u64{0} / 1032U * 1032U);
    static_assert(deflate_max_inflated_size(~u64{0} / 1032U + 1U) == ~u64{0});
    static_assert(deflate_max_inflated_size(~u64{0}) == ~u64{0});

    // The densest stream there is: one literal, then 258-byte matches at distance 1, each a one-bit length code and a
    // one-bit distance code. It decodes within the bound, and close to it.
    const Trees t = match_trees(285U);
    Bits b;
    b.put(1U, 1U);
    b.put(2U, 2U);
    put_dynamic_trees(b, t.lengths, 286U, 1U);
    b.code(t.lit[kA], 2U);
    constexpr usize matches = 4000U;
    for (usize i = 0; i < matches; ++i)
    {
        b.code(t.lit[285], 1U);
        b.code(t.dist[0], 1U);
    }
    b.code(t.lit[256], 2U);
    const usize decoded = 1U + matches * 258U;
    Array<u8> out(&s_df);
    REQUIRE(decode(inflate_raw, b.bytes(), decoded, out));
    bool all_a = true;
    for (usize i = 0; i < out.size(); ++i)
    {
        all_a = all_a && out[i] == kA;
    }
    CHECK(all_a);
    CHECK(decoded <= deflate_max_inflated_size(b.bytes().size()));
    CHECK(decoded * 10U > deflate_max_inflated_size(b.bytes().size()) * 9U);
}

TEST_CASE("deflate: hand-built blocks the decoder accepts", "[compression][deflate]")
{
    SECTION("an empty fixed block decodes to nothing")
    {
        Bits b;
        b.put(1U, 1U);
        b.put(1U, 2U);
        fixed_symbol(b, 256U);
        CHECK(inflate_bits(b, 0U));
        CHECK_FALSE(inflate_bits(b, 1U));
    }
    SECTION("a fixed block's overlapping match repeats the run")
    {
        Bits b;
        b.put(1U, 1U);
        b.put(1U, 2U);
        fixed_symbol(b, kA);
        fixed_symbol(b, 257U); // length 3
        b.code(0U, 5U);        // distance 1
        fixed_symbol(b, 256U);
        Array<u8> out(&s_df);
        REQUIRE(decode(inflate_raw, b.bytes(), 4U, out));
        CHECK((out[0] == kA && out[1] == kA && out[2] == kA && out[3] == kA));
    }
    SECTION("a dynamic block with a single one-bit distance code")
    {
        const Trees t = match_trees(257U);
        Bits b;
        b.put(1U, 1U);
        b.put(2U, 2U);
        put_dynamic_trees(b, t.lengths, 286U, 1U);
        b.code(t.lit[kA], 2U);
        b.code(t.lit[257], 1U);
        b.code(t.dist[0], 1U);
        b.code(t.lit[256], 2U);
        CHECK(inflate_bits(b, 4U));
    }
    SECTION("a dynamic block of two one-bit literal/length codes, with one distance code and with none")
    {
        u8 lengths[257 + 1] = {}; // 257 literal/length lengths, then the one distance length
        lengths[kA] = 1U;
        lengths[256] = 1U;
        lengths[257] = 1U;
        CHECK(inflate_bits(a_then_end(lengths, 257U, 1U), 1U));
        const u8 zero_one[19] = {1, 1}; // the code-length code {0, 1} at one bit each, zeros sent one by one
        CHECK(inflate_bits(a_then_end(lengths, 257U, 1U, zero_one), 1U));
        lengths[257] = 0U;
        CHECK(inflate_bits(a_then_end(lengths, 257U, 1U), 1U));
    }
    SECTION("a dynamic block whose only code is end of block, and no distance code")
    {
        u8 lengths[257 + 1] = {};
        lengths[256] = 1U;
        Bits b;
        b.put(1U, 1U);
        b.put(2U, 2U);
        put_dynamic_trees(b, lengths, 257U, 1U);
        b.code(0U, 1U);
        CHECK(inflate_bits(b, 0U));
    }
    SECTION("a stored block, then a final fixed block")
    {
        Bits b;
        b.put(0U, 1U);
        b.put(0U, 2U);
        b.put(0U, 5U); // to the byte boundary
        b.put(2U, 16U);
        b.put(0xFFFDU, 16U);
        b.put('x', 8U);
        b.put('y', 8U);
        b.put(1U, 1U);
        b.put(1U, 2U);
        fixed_symbol(b, 'z');
        fixed_symbol(b, 256U);
        Array<u8> out(&s_df);
        REQUIRE(decode(inflate_raw, b.bytes(), 3U, out));
        CHECK((out[0] == 'x' && out[1] == 'y' && out[2] == 'z'));
    }
}

TEST_CASE("deflate: the decoder refuses every malformed stream", "[compression][deflate]")
{
    SECTION("no input")
    {
        Bits b;
        CHECK_FALSE(inflate_bits(b, 0U));
    }
    SECTION("the reserved block type")
    {
        Bits b;
        b.put(1U, 1U);
        b.put(3U, 2U);
        b.put(0U, 13U);
        CHECK_FALSE(inflate_bits(b, 0U));
    }
    SECTION("a stored block whose length and its complement disagree")
    {
        const u8 stream[] = {0x01, 0x03, 0x00, 0xFD, 0xFF, 'A', 'B', 'C'};
        Array<u8> out(&s_df);
        CHECK_FALSE(decode(inflate_raw, {stream, sizeof(stream)}, 3U, out));
    }
    SECTION("a stored block shorter than its length, or longer than the output")
    {
        const u8 stream[] = {0x01, 0x03, 0x00, 0xFC, 0xFF, 'A', 'B', 'C'};
        Array<u8> out(&s_df);
        CHECK(decode(inflate_raw, {stream, sizeof(stream)}, 3U, out));
        CHECK_FALSE(decode(inflate_raw, {stream, sizeof(stream) - 1U}, 3U, out));
        CHECK_FALSE(decode(inflate_raw, {stream, 4U}, 3U, out));
        CHECK_FALSE(decode(inflate_raw, {stream, sizeof(stream)}, 2U, out));
    }
    SECTION("a reference before the first decoded byte")
    {
        Bits b;
        b.put(1U, 1U);
        b.put(1U, 2U);
        fixed_symbol(b, 257U);
        b.code(0U, 5U);
        fixed_symbol(b, 256U);
        CHECK_FALSE(inflate_bits(b, 3U));
        Bits far;
        far.put(1U, 1U);
        far.put(1U, 2U);
        fixed_symbol(far, kA);
        fixed_symbol(far, 257U);
        far.code(1U, 5U); // distance 2, one byte decoded
        fixed_symbol(far, 256U);
        CHECK_FALSE(inflate_bits(far, 4U));
    }
    SECTION("the unused literal/length symbols 286 and 287, and the distance codes 30 and 31")
    {
        for (const u32 sym : {286U, 287U})
        {
            Bits b;
            b.put(1U, 1U);
            b.put(1U, 2U);
            fixed_symbol(b, kA);
            fixed_symbol(b, sym);
            b.code(0U, 5U);
            fixed_symbol(b, 256U);
            CHECK_FALSE(inflate_bits(b, 4U));
        }
        for (const u32 dist : {30U, 31U})
        {
            Bits b;
            b.put(1U, 1U);
            b.put(1U, 2U);
            fixed_symbol(b, kA);
            fixed_symbol(b, 257U);
            b.code(dist, 5U);
            fixed_symbol(b, 256U);
            CHECK_FALSE(inflate_bits(b, 4U));
        }
    }
    SECTION("more than 286 literal/length codes, or more than 30 distance codes")
    {
        u8 lit[287 + 1] = {};
        lit[kA] = 1U;
        lit[256] = 1U;
        lit[287] = 1U;
        CHECK_FALSE(inflate_bits(a_then_end(lit, 287U, 1U), 1U));
        u8 dist[257 + 31] = {};
        dist[kA] = 1U;
        dist[256] = 1U;
        dist[257] = 1U;
        CHECK_FALSE(inflate_bits(a_then_end(dist, 257U, 31U), 1U));
    }
    SECTION("an over-subscribed, incomplete or single-code code-length code")
    {
        u8 lengths[257 + 1] = {};
        lengths[kA] = 1U;
        lengths[256] = 1U;
        lengths[257] = 1U;
        const u8 over[19] = {1, 1, 1}; // 0, 1 and 2 at one bit: 2 has no code, the stream sends only 0 and 1
        CHECK_FALSE(inflate_bits(a_then_end(lengths, 257U, 1U, over), 1U));
        const u8 incomplete[19] = {2, 1, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0, 0, 3}; // 1 is 0, 0 is 10, 18 is 110
        CHECK_FALSE(inflate_bits(a_then_end(lengths, 257U, 1U, incomplete), 1U));
        // One code alone can only send one length (all ones over-subscribe, all zeros have no end of block), so no
        // stream decodes with it; zlib refuses it with the tree, and so does the decoder.
        Bits single;
        single.put(1U, 1U);
        single.put(2U, 2U);
        single.put(0U, 5U);
        single.put(0U, 5U);
        single.put(0U, 4U); // hclen 4: the lengths of 16, 17, 18 and 0
        single.put(0U, 3U);
        single.put(0U, 3U);
        single.put(1U, 3U);
        single.put(0U, 3U);
        single.put(0U, 32U);
        CHECK_FALSE(inflate_bits(single, 0U));
    }
    SECTION("a repeat of the previous length with no previous length")
    {
        // The code-length code {0, 16}, one bit each: 0 is 0, 16 is 1.
        Bits b;
        b.put(1U, 1U);
        b.put(2U, 2U);
        b.put(0U, 5U);
        b.put(0U, 5U);
        b.put(0U, 4U);
        b.put(1U, 3U); // 16
        b.put(0U, 3U); // 17
        b.put(0U, 3U); // 18
        b.put(1U, 3U); // 0
        b.code(1U, 1U);
        b.put(0U, 2U);
        b.put(0U, 32U);
        CHECK_FALSE(inflate_bits(b, 0U));
    }
    SECTION("a zero run past the declared lengths")
    {
        u8 lengths[257 + 1] = {};
        lengths[kA] = 1U;
        lengths[256] = 1U; // no distance code: the last length is a zero, sent as a run of 11
        CHECK_FALSE(inflate_bits(a_then_end(lengths, 257U, 1U, kClDefault, 10U), 1U));
    }
    SECTION("a literal/length code without end of block")
    {
        // No block can end without it, so no stream decodes; zlib refuses it with the tree, and so does the decoder.
        u8 lengths[257 + 1] = {};
        lengths[kA] = 1U;
        lengths[kA + 1U] = 1U;
        lengths[257] = 1U;
        Bits b;
        b.put(1U, 1U);
        b.put(2U, 2U);
        put_dynamic_trees(b, lengths, 257U, 1U);
        b.put(0U, 16U);
        CHECK_FALSE(inflate_bits(b, 1U));
    }
    SECTION("an over-subscribed literal/length code")
    {
        u8 lengths[259 + 1] = {}; // 256, 257 and 258 at one bit: 258 has no code, end of block is 0
        lengths[256] = 1U;
        lengths[257] = 1U;
        lengths[258] = 1U;
        lengths[259] = 1U;
        CHECK_FALSE(inflate_bits(a_then_end(lengths, 259U, 1U), 0U));
    }
    SECTION("an incomplete literal/length code of more than one code")
    {
        u8 lengths[257 + 1] = {};
        lengths[kA] = 1U;
        lengths[256] = 2U;
        lengths[257] = 1U;
        CHECK_FALSE(inflate_bits(a_then_end(lengths, 257U, 1U), 1U));
    }
    SECTION("an incomplete distance code of more than one code")
    {
        u8 lengths[257 + 2] = {};
        lengths[kA] = 1U;
        lengths[256] = 1U;
        lengths[257] = 1U; // distance code 0 at one bit and code 1 at two
        lengths[258] = 2U;
        CHECK_FALSE(inflate_bits(a_then_end(lengths, 257U, 2U), 1U));
    }
    SECTION("the single code's unused bit pattern")
    {
        const Trees t = match_trees(257U);
        Bits b;
        b.put(1U, 1U);
        b.put(2U, 2U);
        put_dynamic_trees(b, t.lengths, 286U, 1U);
        b.code(t.lit[kA], 2U);
        b.code(t.lit[257], 1U);
        b.code(t.dist[0] ^ 1U, 1U);
        b.code(t.lit[256], 2U);
        CHECK_FALSE(inflate_bits(b, 4U));
    }
    SECTION("a stream that ends inside a code, or before its final block")
    {
        Bits b;
        b.put(0U, 1U);
        b.put(1U, 2U);
        fixed_symbol(b, kA);
        fixed_symbol(b, 256U); // not final: the next block header is missing
        CHECK_FALSE(inflate_bits(b, 1U));
        Bits cut;
        cut.put(1U, 1U);
        cut.put(1U, 2U);
        fixed_symbol(cut, kA);
        cut.code(0U, 3U); // three bits of a seven-bit end of block
        CHECK_FALSE(inflate_bits(cut, 1U));
    }
}

TEST_CASE("deflate: zlib refuses a bad header, a preset dictionary and a wrong checksum", "[compression][deflate]")
{
    Array<u8> src(&s_df);
    for (int i = 0; i < 100; ++i)
    {
        src.push_back(static_cast<u8>('a' + i % 7));
    }
    const Array<u8> good = zlib_deflate(view(src), &s_df);
    Array<u8> out(&s_df);
    REQUIRE(decode(zlib_inflate, view(good), src.size(), out));
    CHECK(same(out, src));

    const auto refused = [&](u8 cmf, u8 flg, bool flip_checksum)
    {
        Array<u8> bad(&s_df);
        for (usize i = 0; i < good.size(); ++i)
        {
            bad.push_back(good[i]);
        }
        bad[0] = cmf;
        bad[1] = flg;
        if (flip_checksum)
        {
            bad[bad.size() - 1U] = static_cast<u8>(bad[bad.size() - 1U] ^ 1U);
        }
        Array<u8> o(&s_df);
        return !decode(zlib_inflate, view(bad), src.size(), o);
    };
    const auto check_bits = [](u8 cmf, u8 flg)
    {
        return ((u32{cmf} << 8U) | flg) % 31U == 0U;
    };
    static_assert(((0x78U << 8U) | 0x9CU) % 31U == 0U);
    CHECK_FALSE(refused(0x78U, 0x9CU, false));
    CHECK(refused(0x78U, 0x9DU, false)); // header check bits
    CHECK(refused(0x77U, 0x0CU, false)); // compression method 7
    REQUIRE(check_bits(0x88U, 0x1CU));
    CHECK(refused(0x88U, 0x1CU, false)); // a 64 KiB window
    REQUIRE(check_bits(0x78U, 0xBBU));
    CHECK(refused(0x78U, 0xBBU, false)); // a preset dictionary
    CHECK(refused(0x78U, 0x9CU, true));  // the Adler-32
    const u8 tiny[5] = {0x78, 0x9C, 0x03, 0x00, 0x00};
    CHECK_FALSE(decode(zlib_inflate, {tiny, sizeof(tiny)}, 0U, out)); // shorter than a header and a trailer
}
