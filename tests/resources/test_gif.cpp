// tests/resources/test_gif.cpp — the GIF decoder gates. Every fixture is built IN-MEMORY through OUR OWN GIF-LZW
// encoder (the codec proves itself end-to-end through a real compressed stream — dictionary matches, the KwKwK case,
// and variable code-width GROWTH are all exercised, not just literal runs): solid + patterned images, GIF89a
// transparency, 4-pass interlace, local-vs-global color tables, auto-dispatch, and the named failure classes.
//
// NOTE (honest gate boundary): encoder and decoder are our own, so a real-world GIF corpus (an external oracle) is the
// recommended follow-up to fully break encoder/decoder symmetry at the code-width boundary — see the fork report.

#include <catch2/catch_test_macros.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/resources/gif_image.hpp>
#include <crd/resources/ldr_image.hpp>

using namespace crd::resources;
using crd::containers::Array;

namespace
{

void put16le(Array<crd::u8>& b, crd::u16 v)
{
    b.push_back(static_cast<crd::u8>(v & 0xFFU));
    b.push_back(static_cast<crd::u8>((v >> 8U) & 0xFFU));
}

// LSB-first bit writer for LZW codes.
struct BitW
{
    Array<crd::u8>* out;
    crd::u32        buf = 0;
    crd::u32        cnt = 0;
    void            put(crd::u32 code, crd::u32 w)
    {
        buf |= (code & ((1U << w) - 1U)) << cnt;
        cnt += w;
        while (cnt >= 8U)
        {
            out->push_back(static_cast<crd::u8>(buf & 0xFFU));
            buf >>= 8U;
            cnt -= 8U;
        }
    }
    void flush()
    {
        if (cnt > 0U)
        {
            out->push_back(static_cast<crd::u8>(buf & 0xFFU));
            buf = 0;
            cnt = 0;
        }
    }
};

// Our own GIF-LZW encoder — builds a real compressed stream (dictionary growth, code-width increase) that the decoder
// must invert. Growth rule mirrors the decoder (`next == 2^width` after assignment); resets on a full 12-bit table.
Array<crd::u8> lzw_encode(const crd::u8* idx, crd::usize n, crd::u8 min_cs, crd::memory::IAllocator* a)
{
    Array<crd::u8> out(a);
    BitW           bw{&out};
    const crd::u32 clear = 1U << min_cs;
    const crd::u32 end   = clear + 1U;
    crd::u32       width = min_cs + 1U;
    crd::u32       next  = clear + 2U;
    Array<crd::u32> dp(a); // dict entry prefix code
    Array<crd::u32> dc(a); // dict entry char
    Array<crd::u32> dk(a); // dict entry assigned code

    bw.put(clear, width);
    if (n == 0)
    {
        bw.put(end, width);
        bw.flush();
        return out;
    }
    crd::u32 cur = idx[0];
    for (crd::usize i = 1; i < n; ++i)
    {
        const crd::u32 k = idx[i];
        crd::i64       f = -1;
        for (crd::usize j = 0; j < dk.size(); ++j)
        {
            if (dp[j] == cur && dc[j] == k)
            {
                f = static_cast<crd::i64>(dk[j]);
                break;
            }
        }
        if (f >= 0)
        {
            cur = static_cast<crd::u32>(f);
            continue;
        }
        bw.put(cur, width);
        if (next < 4096U)
        {
            dp.push_back(cur);
            dc.push_back(k);
            dk.push_back(next);
            ++next;
            // Encoder grows one code LATER than the decoder: the decoder assigns entries one step behind (its first
            // code after a clear adds nothing), so it hits 2^width one code later. Pair with it via `next == 2^width+1`.
            if (next == (1U << width) + 1U && width < 12U) { ++width; }
        }
        else
        {
            bw.put(clear, width);
            dp.clear();
            dc.clear();
            dk.clear();
            next  = clear + 2U;
            width = min_cs + 1U;
        }
        cur = k;
    }
    bw.put(cur, width);
    bw.put(end, width);
    bw.flush();
    return out;
}

// Assemble a full GIF87a/89a file around a row-major index buffer.
Array<crd::u8> build_gif(crd::memory::IAllocator* a, crd::u32 w, crd::u32 h, const crd::u8* pal, crd::u32 pn,
                         const crd::u8* idx, crd::i32 transparent, bool interlace)
{
    Array<crd::u8> g(a);
    const bool     use89 = transparent >= 0;
    const char*    sig   = use89 ? "GIF89a" : "GIF87a";
    for (int i = 0; i < 6; ++i) { g.push_back(static_cast<crd::u8>(sig[i])); }

    crd::u8 nbits = 0;
    while ((2U << nbits) < pn) { ++nbits; } // 2^(nbits+1) >= pn
    const crd::u32 gsz     = 2U << nbits;   // padded table entries
    crd::u8        min_cs  = static_cast<crd::u8>(nbits + 1U);
    if (min_cs < 2U) { min_cs = 2U; }

    put16le(g, static_cast<crd::u16>(w));
    put16le(g, static_cast<crd::u16>(h));
    g.push_back(static_cast<crd::u8>(0x80U | nbits)); // GCT present + size
    g.push_back(0);                                   // background index
    g.push_back(0);                                   // aspect ratio
    for (crd::u32 i = 0; i < gsz; ++i)
    {
        if (i < pn)
        {
            g.push_back(pal[i * 3U + 0U]);
            g.push_back(pal[i * 3U + 1U]);
            g.push_back(pal[i * 3U + 2U]);
        }
        else
        {
            g.push_back(0);
            g.push_back(0);
            g.push_back(0);
        }
    }
    if (use89)
    {
        g.push_back(0x21);
        g.push_back(0xF9);
        g.push_back(4);
        g.push_back(0x01); // transparency flag set
        put16le(g, 0);     // delay
        g.push_back(static_cast<crd::u8>(transparent));
        g.push_back(0x00); // block terminator
    }
    g.push_back(0x2C); // image descriptor
    put16le(g, 0);
    put16le(g, 0);
    put16le(g, static_cast<crd::u16>(w));
    put16le(g, static_cast<crd::u16>(h));
    g.push_back(interlace ? 0x40U : 0x00U);
    g.push_back(min_cs);

    // reorder into interlaced storage order if needed
    Array<crd::u8> order(a);
    if (interlace)
    {
        const crd::u32 starts[4] = {0U, 4U, 2U, 1U};
        const crd::u32 steps[4]  = {8U, 8U, 4U, 2U};
        for (int pass = 0; pass < 4; ++pass)
        {
            for (crd::u32 y = starts[pass]; y < h; y += steps[pass])
            {
                for (crd::u32 x = 0; x < w; ++x) { order.push_back(idx[y * w + x]); }
            }
        }
    }
    else
    {
        for (crd::u32 i = 0; i < w * h; ++i) { order.push_back(idx[i]); }
    }

    const Array<crd::u8> lzw = lzw_encode(order.data(), order.size(), min_cs, a);
    crd::usize           off = 0;
    while (off < lzw.size())
    {
        crd::usize chunk = lzw.size() - off;
        if (chunk > 255U) { chunk = 255U; }
        g.push_back(static_cast<crd::u8>(chunk));
        for (crd::usize i = 0; i < chunk; ++i) { g.push_back(lzw[off + i]); }
        off += chunk;
    }
    g.push_back(0x00); // block terminator
    g.push_back(0x3B); // trailer
    return g;
}

} // namespace

TEST_CASE("resources: GIF solid + patterned round-trip (RGBA8, dict + KwKwK)", "[resources][gif]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    // 4-color palette; a repetitive-then-varied pattern forces dictionary matches, KwKwK, and code-width growth (min=2).
    const crd::u8 pal[12] = {10, 20, 30, /*0*/ 200, 40, 60, /*1*/ 70, 210, 90, /*2*/ 100, 110, 220 /*3*/};
    constexpr crd::u32 w = 8;
    constexpr crd::u32 h = 6;
    crd::u8            idx[w * h];
    for (crd::u32 y = 0; y < h; ++y)
    {
        for (crd::u32 x = 0; x < w; ++x) { idx[y * w + x] = static_cast<crd::u8>(((x / 2U) + y) % 4U); }
    }
    const Array<crd::u8> gif = build_gif(&alloc, w, h, pal, 4, idx, -1, false);

    GifImage img(&alloc);
    REQUIRE(gif_decode(crd::containers::as_const_span(gif), img, &alloc) == GifError::Ok);
    CHECK(img.width == w);
    CHECK(img.height == h);
    CHECK(img.source_channels == 4);
    CHECK(img.source_bit_depth == 8);
    REQUIRE(img.pixels.size() == static_cast<crd::usize>(w) * h * 4U);
    for (crd::u32 i = 0; i < w * h; ++i)
    {
        const crd::u8* c = pal + static_cast<crd::usize>(idx[i]) * 3U;
        CHECK(img.pixels[i * 4U + 0U] == c[0]);
        CHECK(img.pixels[i * 4U + 1U] == c[1]);
        CHECK(img.pixels[i * 4U + 2U] == c[2]);
        CHECK(img.pixels[i * 4U + 3U] == 255U); // opaque (no transparency)
    }
}

TEST_CASE("resources: GIF code-width GROWTH across many entries", "[resources][gif]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    // 16x16, min_code_size=2 → the table crosses 8/16/32/... repeatedly; a diagonal gradient builds long dict chains.
    const crd::u8      pal[12] = {0, 0, 0, 85, 85, 85, 170, 170, 170, 255, 255, 255};
    constexpr crd::u32 w = 16;
    constexpr crd::u32 h = 16;
    crd::u8            idx[w * h];
    for (crd::u32 y = 0; y < h; ++y)
    {
        for (crd::u32 x = 0; x < w; ++x) { idx[y * w + x] = static_cast<crd::u8>((x + y) % 4U); }
    }
    const Array<crd::u8> gif = build_gif(&alloc, w, h, pal, 4, idx, -1, false);
    GifImage             img(&alloc);
    REQUIRE(gif_decode(crd::containers::as_const_span(gif), img, &alloc) == GifError::Ok);
    for (crd::u32 i = 0; i < w * h; ++i)
    {
        const crd::u8* c = pal + static_cast<crd::usize>(idx[i]) * 3U;
        CHECK(img.pixels[i * 4U + 0U] == c[0]);
        CHECK(img.pixels[i * 4U + 3U] == 255U);
    }
}

TEST_CASE("resources: GIF89a transparency index becomes alpha 0", "[resources][gif]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const crd::u8              pal[6] = {255, 0, 0, /*0*/ 0, 255, 0 /*1*/};
    constexpr crd::u32         w = 4;
    constexpr crd::u32         h = 2;
    crd::u8                    idx[w * h] = {0, 1, 0, 1, 1, 0, 1, 0};
    const Array<crd::u8>       gif        = build_gif(&alloc, w, h, pal, 2, idx, /*transparent=*/1, false);
    GifImage                   img(&alloc);
    REQUIRE(gif_decode(crd::containers::as_const_span(gif), img, &alloc) == GifError::Ok);
    for (crd::u32 i = 0; i < w * h; ++i)
    {
        const bool transp = (idx[i] == 1U);
        CHECK(img.pixels[i * 4U + 3U] == (transp ? 0U : 255U));
        if (!transp) { CHECK(img.pixels[i * 4U + 0U] == 255U); } // index 0 = red, opaque
    }
}

TEST_CASE("resources: GIF interlace de-scatters to row-major", "[resources][gif]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const crd::u8              pal[6] = {1, 2, 3, 250, 251, 252};
    constexpr crd::u32         w = 5;
    constexpr crd::u32         h = 9; // 9 rows exercises all four interlace passes
    crd::u8                    idx[w * h];
    for (crd::u32 y = 0; y < h; ++y)
    {
        for (crd::u32 x = 0; x < w; ++x) { idx[y * w + x] = static_cast<crd::u8>((y * 3U + x) % 2U); }
    }
    const Array<crd::u8> gif = build_gif(&alloc, w, h, pal, 2, idx, -1, /*interlace=*/true);
    GifImage             img(&alloc);
    REQUIRE(gif_decode(crd::containers::as_const_span(gif), img, &alloc) == GifError::Ok);
    for (crd::u32 y = 0; y < h; ++y)
    {
        for (crd::u32 x = 0; x < w; ++x)
        {
            const crd::u8* c = pal + static_cast<crd::usize>(idx[y * w + x]) * 3U;
            CHECK(img.pixels[(y * w + x) * 4U + 0U] == c[0]); // correct row => de-scatter correct
        }
    }
}

TEST_CASE("resources: GIF auto-dispatch through ldr_decode", "[resources][gif]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    const crd::u8              pal[6] = {9, 8, 7, 1, 2, 3};
    crd::u8                    idx[4] = {0, 1, 1, 0};
    const Array<crd::u8>       gif    = build_gif(&alloc, 2, 2, pal, 2, idx, -1, false);
    CHECK(ldr_sniff(crd::containers::as_const_span(gif)) == LdrCodec::Gif);
    LdrImage img(&alloc);
    REQUIRE(ldr_decode(crd::containers::as_const_span(gif), img, &alloc) == LdrError::Ok);
    CHECK(img.width == 2);
    CHECK(img.pixels[0] == 9); // index 0 -> (9,8,7)
}

TEST_CASE("resources: GIF failure classes are refused by name", "[resources][gif]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);

    SECTION("bad magic")
    {
        const crd::u8 junk[8] = {'N', 'O', 'T', 'A', 'G', 'I', 'F', '!'};
        LdrImage      img(&alloc);
        CHECK(gif_decode(crd::containers::ConstSpan<crd::u8>(junk, 8), img, &alloc) == GifError::BadMagic);
    }
    SECTION("truncated mid-stream")
    {
        const crd::u8        pal[6] = {1, 1, 1, 2, 2, 2};
        crd::u8              idx[4] = {0, 1, 0, 1};
        const Array<crd::u8> gif    = build_gif(&alloc, 2, 2, pal, 2, idx, -1, false);
        LdrImage             img(&alloc);
        // cut off inside the pixel/LZW data (keep header + palette + descriptor, drop the tail)
        const crd::usize cut = gif.size() - 4U;
        const GifError   e   = gif_decode(crd::containers::ConstSpan<crd::u8>(gif.data(), cut), img, &alloc);
        CHECK((e == GifError::Truncated || e == GifError::BadData));
    }
    SECTION("index out of palette range")
    {
        // Palette has 2 entries but the LZW stream references index 3 → BadData (validated before any pixel write).
        crd::memory::TlsfAllocator a2(4U << 20U);
        const crd::u8              pal[6] = {0, 0, 0, 1, 1, 1};
        crd::u8                    idx[4] = {0, 3, 0, 0}; // 3 is outside the 2-entry table
        const Array<crd::u8>       gif    = build_gif(&a2, 2, 2, pal, 2, idx, -1, false);
        LdrImage                   img(&a2);
        CHECK(gif_decode(crd::containers::as_const_span(gif), img, &a2) == GifError::BadData);
        CHECK(!img.valid());
    }
}
