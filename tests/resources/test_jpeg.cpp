// tests/resources/test_jpeg.cpp — baseline JPEG decoder gates. Fixtures are HAND-ENCODED in the test with the standard
// Annex-K luminance Huffman tables and a flat quant table, so every decoded pixel has an ANALYTIC expectation:
// a DC-only block IDCTs to the flat value DC·q/8 + 128 — grayscale, 4:2:0 color, restart markers, an AC structural
// gate, and the named-Unsupported classes (progressive) all verify against closed-form values.

#include <catch2/catch_test_macros.hpp>

#include <crd/resources/jpeg_image.hpp>
#include <crd/resources/ldr_image.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using namespace crd::resources;

namespace
{

// K.3.3.1 luminance DC: category t (0..11); K.3.3.2 luminance AC (only the codes the fixtures use)
struct BitWriter
{
    crd::containers::Array<crd::u8>* out;
    crd::u32                         acc  = 0;
    int                              nbit = 0;

    void put(crd::u32 code, int len)
    {
        for (int i = len - 1; i >= 0; --i)
        {
            acc = (acc << 1U) | ((code >> static_cast<crd::u32>(i)) & 1U);
            if (++nbit == 8)
            {
                out->push_back(static_cast<crd::u8>(acc));
                if ((acc & 0xFFU) == 0xFFU) { out->push_back(0x00U); } // byte stuffing
                acc  = 0;
                nbit = 0;
            }
        }
    }
    void flush()
    {
        while (nbit != 0) { put(1U, 1); } // pad with 1s per convention
    }
};

// standard luminance DC codes: cat0="00"(2) · cat1..5 = "010".."110"(3) · cat6="1110"(4) · cat7="11110"(5) …
void put_dc(BitWriter& bw, int diff)
{
    int cat = 0;
    int v   = diff < 0 ? -diff : diff;
    while (v != 0)
    {
        ++cat;
        v >>= 1;
    }
    static constexpr crd::u32 kCode[8] = {0b00U, 0b010U, 0b011U, 0b100U, 0b101U, 0b110U, 0b1110U, 0b11110U};
    static constexpr int      kLen[8]  = {2, 3, 3, 3, 3, 3, 4, 5};
    REQUIRE(cat <= 7);
    bw.put(kCode[cat], kLen[cat]);
    if (cat > 0)
    {
        const crd::u32 mag = diff >= 0 ? static_cast<crd::u32>(diff)
                                       : static_cast<crd::u32>(diff + (1 << cat) - 1); // one's-complement negatives
        bw.put(mag, cat);
    }
}
void put_eob(BitWriter& bw) { bw.put(0b1010U, 4); }        // AC luminance EOB
void put_ac01_plus1(BitWriter& bw)                          // AC symbol 0x01 = "00", then the 1-bit magnitude "1" (+1)
{
    bw.put(0b00U, 2);
    bw.put(0b1U, 1);
}

void push16be(crd::containers::Array<crd::u8>& b, crd::u16 v)
{
    b.push_back(static_cast<crd::u8>(v >> 8U));
    b.push_back(static_cast<crd::u8>(v & 0xFFU));
}

// segments: SOI + DQT(flat q) + SOF0 + the standard DHT (DC cats + the AC subset) + optional DRI + SOS header
void jpeg_prologue(crd::containers::Array<crd::u8>& b, crd::u16 w, crd::u16 h, int ncomp, crd::u8 y_hv, crd::u16 q,
                   crd::u16 dri)
{
    b.push_back(0xFFU);
    b.push_back(0xD8U); // SOI
    b.push_back(0xFFU);
    b.push_back(0xDBU); // DQT
    push16be(b, 2 + 1 + 64);
    b.push_back(0x00U); // 8-bit, table 0
    for (int i = 0; i < 64; ++i) { b.push_back(static_cast<crd::u8>(q)); }
    b.push_back(0xFFU);
    b.push_back(0xC0U); // SOF0
    push16be(b, static_cast<crd::u16>(8 + 3 * ncomp));
    b.push_back(8);
    push16be(b, h);
    push16be(b, w);
    b.push_back(static_cast<crd::u8>(ncomp));
    for (int c = 0; c < ncomp; ++c)
    {
        b.push_back(static_cast<crd::u8>(1 + c));
        b.push_back(c == 0 ? y_hv : crd::u8{0x11U});
        b.push_back(0); // quant table 0 for all
    }
    // DHT: DC table 0 — the standard luminance BITS/VALS
    b.push_back(0xFFU);
    b.push_back(0xC4U);
    push16be(b, 2 + (1 + 16 + 12) + (1 + 16 + 6));
    b.push_back(0x00U); // DC, id 0
    const crd::u8 dc_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
    for (crd::u8 v : dc_bits) { b.push_back(v); }
    for (crd::u8 v = 0; v < 12; ++v) { b.push_back(v); }
    // AC table 0 — a MINIMAL canonical table carrying exactly the symbols the fixtures use, with the STANDARD codes:
    // len2: 0x01 ("00") · len4: 0x00 EOB ("1010")... canonical len2 first code 00; len3 codes 010,011,100,101,110 unused
    // → declare bits: {1,0,1,...}: len1:0 len2:1(0x01) len3:0 len4:1(0x00)? canonical: len2 code=00; len4: code = ((00+1)<<2)=0100? That is NOT 1010.
    // To keep the STANDARD codes we declare the standard K.3.3.2 prefix: len2:{0x01,0x02} len3:{0x03} len4:{0x00,0x04,0x11} —
    // canonical then gives 0x01="00", 0x02="01", 0x03="100", 0x00="1010" — the standard assignments the writers above use.
    b.push_back(0x10U); // AC, id 0
    const crd::u8 ac_bits[16] = {0, 2, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (crd::u8 v : ac_bits) { b.push_back(v); }
    const crd::u8 ac_vals[6] = {0x01, 0x02, 0x03, 0x00, 0x04, 0x11};
    for (crd::u8 v : ac_vals) { b.push_back(v); }
    if (dri != 0U)
    {
        b.push_back(0xFFU);
        b.push_back(0xDDU);
        push16be(b, 4);
        push16be(b, dri);
    }
    b.push_back(0xFFU);
    b.push_back(0xDAU); // SOS
    push16be(b, static_cast<crd::u16>(6 + 2 * ncomp));
    b.push_back(static_cast<crd::u8>(ncomp));
    for (int c = 0; c < ncomp; ++c)
    {
        b.push_back(static_cast<crd::u8>(1 + c));
        b.push_back(0x00U); // DC 0 / AC 0
    }
    b.push_back(0);
    b.push_back(63);
    b.push_back(0);
}

} // namespace

TEST_CASE("resources: JPEG grayscale DC-only block decodes to the analytic flat value", "[resources][jpeg]")
{
    crd::memory::TlsfAllocator      alloc(8U << 20U);
    crd::containers::Array<crd::u8> j(&alloc);
    jpeg_prologue(j, 8, 8, 1, 0x11U, 16, 0);
    BitWriter bw{&j};
    put_dc(bw, 4); // DC=4 → dequant 64 → flat 64/8 = 8 → +128 = 136
    put_eob(bw);
    bw.flush();
    j.push_back(0xFFU);
    j.push_back(0xD9U); // EOI

    LdrImage img(&alloc);
    REQUIRE(jpeg_decode(crd::containers::as_const_span(j), img, &alloc) == LdrError::Ok);
    REQUIRE(img.width == 8U);
    CHECK(img.source_channels == 1);
    for (crd::u32 i = 0; i < 64U; ++i)
    {
        CHECK(img.pixels[i * 4U + 0U] == 136);
        CHECK(img.pixels[i * 4U + 3U] == 255);
    }
    CHECK(ldr_sniff(crd::containers::as_const_span(j)) == LdrCodec::Jpeg);
}

TEST_CASE("resources: JPEG 4:2:0 color MCU -- flat gray through the full YCbCr path", "[resources][jpeg]")
{
    crd::memory::TlsfAllocator      alloc(8U << 20U);
    crd::containers::Array<crd::u8> j(&alloc);
    jpeg_prologue(j, 16, 16, 3, 0x22U, 16, 0); // Y 2x2, Cb/Cr 1x1 — one MCU
    BitWriter bw{&j};
    put_dc(bw, 4); // Y block 0: DC 4 → 136
    put_eob(bw);
    for (int i = 0; i < 3; ++i) // Y blocks 1..3: diff 0 (pred carries) → 136
    {
        put_dc(bw, 0);
        put_eob(bw);
    }
    put_dc(bw, 0); // Cb → 128 → chroma 0
    put_eob(bw);
    put_dc(bw, 0); // Cr
    put_eob(bw);
    bw.flush();
    j.push_back(0xFFU);
    j.push_back(0xD9U);

    LdrImage img(&alloc);
    REQUIRE(jpeg_decode(crd::containers::as_const_span(j), img, &alloc) == LdrError::Ok);
    REQUIRE(img.width == 16U);
    CHECK(img.source_channels == 3);
    for (crd::u32 i = 0; i < 16U * 16U; ++i)
    {
        CHECK(img.pixels[i * 4U + 0U] == 136); // neutral chroma ⇒ R=G=B=Y
        CHECK(img.pixels[i * 4U + 1U] == 136);
        CHECK(img.pixels[i * 4U + 2U] == 136);
    }
}

TEST_CASE("resources: JPEG restart markers reset the DC predictor", "[resources][jpeg]")
{
    crd::memory::TlsfAllocator      alloc(8U << 20U);
    crd::containers::Array<crd::u8> j(&alloc);
    jpeg_prologue(j, 16, 8, 1, 0x11U, 16, 1); // two MCUs, DRI=1
    BitWriter bw{&j};
    put_dc(bw, 4); // MCU 0: 136
    put_eob(bw);
    bw.flush();
    j.push_back(0xFFU);
    j.push_back(0xD0U); // RST0
    BitWriter bw2{&j};
    put_dc(bw2, 2); // MCU 1: the predictor RESET ⇒ DC=2 → 2·16/8=4 → 132 (without the reset it would be 6 → 140)
    put_eob(bw2);
    bw2.flush();
    j.push_back(0xFFU);
    j.push_back(0xD9U);

    LdrImage img(&alloc);
    REQUIRE(jpeg_decode(crd::containers::as_const_span(j), img, &alloc) == LdrError::Ok);
    CHECK(img.pixels[0] == 136);       // block 0
    CHECK(img.pixels[8U * 4U] == 132); // block 1 — proves the reset
}

TEST_CASE("resources: JPEG AC coefficient -- structural gates on the u=1 cosine", "[resources][jpeg]")
{
    crd::memory::TlsfAllocator      alloc(8U << 20U);
    crd::containers::Array<crd::u8> j(&alloc);
    jpeg_prologue(j, 8, 8, 1, 0x11U, 16, 0);
    BitWriter bw{&j};
    put_dc(bw, 8);        // a bias so the ramp stays in range: flat 16 → 144
    put_ac01_plus1(bw);   // AC k=1 (u=1,v=0) = +1 → dequant 16: a horizontal half-cosine ramp
    put_eob(bw);
    bw.flush();
    j.push_back(0xFFU);
    j.push_back(0xD9U);

    LdrImage img(&alloc);
    REQUIRE(jpeg_decode(crd::containers::as_const_span(j), img, &alloc) == LdrError::Ok);
    // structure of a (u=1,v=0) basis: columns constant VERTICALLY; horizontally antisymmetric about the flat level
    for (crd::u32 x = 0; x < 8U; ++x)
    {
        const crd::u8 v0 = img.pixels[x * 4U];
        for (crd::u32 y = 1; y < 8U; ++y) { CHECK(img.pixels[(y * 8U + x) * 4U] == v0); }
        const int a = img.pixels[x * 4U];
        const int b = img.pixels[(7U - x) * 4U];
        CHECK(std::abs(a + b - 2 * 144) <= 2); // antisymmetry within rounding
    }
    CHECK(img.pixels[0] > 144);      // cos ramp: positive at x=0
    CHECK(img.pixels[7U * 4U] < 144); // negative at x=7
}

TEST_CASE("resources: JPEG named-Unsupported + failure classes", "[resources][jpeg]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    SECTION("progressive (SOF2) is Unsupported BY NAME")
    {
        crd::containers::Array<crd::u8> j(&alloc);
        j.push_back(0xFFU);
        j.push_back(0xD8U);
        j.push_back(0xFFU);
        j.push_back(0xC2U); // SOF2
        push16be(j, 11);
        j.push_back(8);
        push16be(j, 8);
        push16be(j, 8);
        j.push_back(1);
        j.push_back(1);
        j.push_back(0x11U);
        j.push_back(0);
        LdrImage img(&alloc);
        CHECK(jpeg_decode(crd::containers::as_const_span(j), img, &alloc) == LdrError::Unsupported);
    }
    SECTION("a truncated scan is Truncated")
    {
        crd::containers::Array<crd::u8> j(&alloc);
        jpeg_prologue(j, 8, 8, 1, 0x11U, 16, 0);
        // no entropy data, no EOI
        LdrImage img(&alloc);
        CHECK(jpeg_decode(crd::containers::as_const_span(j), img, &alloc) == LdrError::Truncated);
    }
    SECTION("bad magic")
    {
        const crd::u8 junk[4] = {1, 2, 3, 4};
        LdrImage      img(&alloc);
        CHECK(jpeg_decode(crd::containers::ConstSpan<crd::u8>(junk, 4), img, &alloc) == LdrError::BadMagic);
    }
}
