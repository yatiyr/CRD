#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/resources/hdr_image.hpp>
#include <crd/resources/resource_id.hpp>

#include <new>

using namespace crd::resources;
using crd::containers::Array;
using crd::containers::ConstSpan;

namespace
{
alignas(crd::memory::GrowableTlsfAllocator) unsigned char s_hdr_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
crd::memory::GrowableTlsfAllocator& s_hdr = *::new (s_hdr_buf) crd::memory::GrowableTlsfAllocator(); // never destroyed

// A deterministic HDR test image with a variety of magnitudes (incl. into the highlights, and a firefly).
void fill_test_image(HdrImage& img, crd::u32 w, crd::u32 h, crd::u32 ch)
{
    img.resize(w, h, ch);
    for (crd::u32 y = 0; y < h; ++y)
    {
        for (crd::u32 x = 0; x < w; ++x)
        {
            for (crd::u32 c = 0; c < ch; ++c)
            {
                const crd::f32 base = 0.05F + 0.37F * static_cast<crd::f32>((x * 3U + y * 5U + c * 7U) % 17U);
                img.at(x, y, c) = base * (c == 0U ? 4.0F : 1.0F); // R pushed into HDR
            }
        }
    }
    if (w > 5U && h > 2U) { img.at(5U, 2U, 0) = 812.5F; } // a firefly
}
} // namespace

TEST_CASE("B-hdr: sniff detects Radiance / PFM / EXR magics", "[resources][hdr]")
{
    const crd::u8 rad[8] = {'#', '?', 'R', 'A', 'D', 'I', 'A', 'N'};
    const crd::u8 pfc[4] = {'P', 'F', '\n', '1'};
    const crd::u8 pfg[4] = {'P', 'f', '\n', '1'};
    const crd::u8 exr[4] = {0x76U, 0x2FU, 0x31U, 0x01U};
    const crd::u8 non[4] = {'x', 'y', 'z', 'w'};
    CHECK(hdr_sniff({rad, 8}) == HdrCodec::Radiance);
    CHECK(hdr_sniff({pfc, 4}) == HdrCodec::Pfm);
    CHECK(hdr_sniff({pfg, 4}) == HdrCodec::Pfm);
    CHECK(hdr_sniff({exr, 4}) == HdrCodec::Exr);
    CHECK(hdr_sniff({non, 4}) == HdrCodec::Unknown);
}

TEST_CASE("B-hdr-b: PFM is a LOSSLESS bit-exact round-trip (RGB + gray, LE + BE)", "[resources][hdr][pfm]")
{
    for (const bool le : {true, false})
    {
        for (const crd::u32 ch : {3U, 1U})
        {
            HdrImage src(&s_hdr);
            fill_test_image(src, 13U, 7U, ch);
            Array<crd::u8> enc = hdr_encode_pfm(src, le, &s_hdr);
            REQUIRE(enc.size() > 0U);
            REQUIRE(hdr_sniff({enc.data(), enc.size()}) == HdrCodec::Pfm);
            HdrImage dst(&s_hdr);
            REQUIRE(hdr_decode_pfm({enc.data(), enc.size()}, dst, &s_hdr) == HdrError::Ok);
            REQUIRE(dst.width == src.width);
            REQUIRE(dst.height == src.height);
            REQUIRE(dst.channels == src.channels);
            int bad = 0;
            for (crd::usize i = 0; i < src.pixels.size(); ++i) { if (dst.pixels[i] != src.pixels[i]) { ++bad; } }
            CHECK(bad == 0);
        }
    }
}

TEST_CASE("B-hdr-a: Radiance decode of a hand-built flat bitstream (exact power-of-two values)", "[resources][hdr][rgbe]")
{
    // header + two flat RGBE pixels: (128,64,32,129) and (200,100,50,128).
    Array<crd::u8> buf(&s_hdr);
    const char* hdr = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
    for (const char* p = hdr; *p != '\0'; ++p) { buf.push_back(static_cast<crd::u8>(*p)); }
    const crd::u8 px[8] = {128U, 64U, 32U, 129U, 200U, 100U, 50U, 128U};
    for (crd::u8 b : px) { buf.push_back(b); }

    HdrImage img(&s_hdr);
    REQUIRE(hdr_decode_radiance({buf.data(), buf.size()}, img, &s_hdr) == HdrError::Ok);
    REQUIRE(img.width == 2U);
    REQUIRE(img.height == 1U);
    // pixel0: f = 2^(129-136) = 2^-7 → (128,64,32)·2^-7 = (1.0, 0.5, 0.25)
    CHECK(img.at(0, 0, 0) == 1.0F);
    CHECK(img.at(0, 0, 1) == 0.5F);
    CHECK(img.at(0, 0, 2) == 0.25F);
    // pixel1: f = 2^(128-136) = 2^-8 → (200,100,50)/256
    CHECK(img.at(1, 0, 0) == 200.0F / 256.0F);
    CHECK(img.at(1, 0, 1) == 100.0F / 256.0F);
    CHECK(img.at(1, 0, 2) == 50.0F / 256.0F);
}

TEST_CASE("B-hdr-a: Radiance RLE encode->decode round-trip is STABLE (bytes + pixels idempotent)", "[resources][hdr][rgbe]")
{
    HdrImage src(&s_hdr);
    fill_test_image(src, 24U, 6U, 3U); // width 24 ⇒ new-format RLE path

    Array<crd::u8> b1 = hdr_encode_radiance(src, &s_hdr);
    REQUIRE(b1.size() > 0U);
    REQUIRE(hdr_sniff({b1.data(), b1.size()}) == HdrCodec::Radiance);

    HdrImage f1(&s_hdr);
    REQUIRE(hdr_decode_radiance({b1.data(), b1.size()}, f1, &s_hdr) == HdrError::Ok);
    REQUIRE(f1.width == 24U);
    REQUIRE(f1.height == 6U);

    // Re-encode the decoded image: byte-for-byte identical (B→float→B is Ward-idempotent through the RLE).
    Array<crd::u8> b2 = hdr_encode_radiance(f1, &s_hdr);
    REQUIRE(b2.size() == b1.size());
    int byte_bad = 0;
    for (crd::usize i = 0; i < b1.size(); ++i) { if (b1[i] != b2[i]) { ++byte_bad; } }
    CHECK(byte_bad == 0);

    // Decode b2: pixels bit-identical to f1.
    HdrImage f2(&s_hdr);
    REQUIRE(hdr_decode_radiance({b2.data(), b2.size()}, f2, &s_hdr) == HdrError::Ok);
    int px_bad = 0;
    for (crd::usize i = 0; i < f1.pixels.size(); ++i) { if (f1.pixels[i] != f2.pixels[i]) { ++px_bad; } }
    CHECK(px_bad == 0);
}

TEST_CASE("B-hdr-a: Radiance RLE compresses a constant-run scanline", "[resources][hdr][rgbe]")
{
    HdrImage src(&s_hdr);
    src.resize(64U, 4U, 3U);
    for (crd::u32 y = 0; y < 4U; ++y) { for (crd::u32 x = 0; x < 64U; ++x) { src.at(x, y, 0) = 2.0F; src.at(x, y, 1) = 1.0F; src.at(x, y, 2) = 0.5F; } }
    Array<crd::u8> enc = hdr_encode_radiance(src, &s_hdr);
    // 64-wide constant rows must RLE far below the flat 64·4·4 = 1024 pixel bytes.
    CHECK(enc.size() < 400U);
    HdrImage dst(&s_hdr);
    REQUIRE(hdr_decode_radiance({enc.data(), enc.size()}, dst, &s_hdr) == HdrError::Ok);
    CHECK(dst.at(0, 0, 0) == 2.0F);
    CHECK(dst.at(63, 3, 2) == 0.5F);
}

TEST_CASE("B-hdr: hdr_decode auto-detects the format", "[resources][hdr]")
{
    HdrImage src(&s_hdr);
    fill_test_image(src, 20U, 5U, 3U);
    Array<crd::u8> pfm = hdr_encode_pfm(src, true, &s_hdr);
    Array<crd::u8> rad = hdr_encode_radiance(src, &s_hdr);
    HdrImage a(&s_hdr); HdrImage b(&s_hdr);
    CHECK(hdr_decode({pfm.data(), pfm.size()}, a, &s_hdr) == HdrError::Ok);
    CHECK(hdr_decode({rad.data(), rad.size()}, b, &s_hdr) == HdrError::Ok);
    CHECK(a.width == 20U);
    CHECK(b.width == 20U);
}

TEST_CASE("B-hdr-c: EXR FLOAT is a LOSSLESS round-trip (NONE + RLE, RGB + gray)", "[resources][hdr][exr]")
{
    for (const ExrCompression comp : {ExrCompression::None, ExrCompression::Rle})
    {
        for (const crd::u32 ch : {3U, 1U})
        {
            HdrImage src(&s_hdr);
            fill_test_image(src, 19U, 11U, ch);
            Array<crd::u8> enc = hdr_encode_exr(src, ExrPixelType::Float, comp, &s_hdr);
            REQUIRE(enc.size() > 0U);
            REQUIRE(hdr_sniff({enc.data(), enc.size()}) == HdrCodec::Exr);
            HdrImage dst(&s_hdr);
            REQUIRE(hdr_decode_exr({enc.data(), enc.size()}, dst, &s_hdr) == HdrError::Ok);
            REQUIRE(dst.width == src.width);
            REQUIRE(dst.height == src.height);
            REQUIRE(dst.channels == src.channels);
            int bad = 0;
            for (crd::usize i = 0; i < src.pixels.size(); ++i) { if (dst.pixels[i] != src.pixels[i]) { ++bad; } }
            CHECK(bad == 0);
        }
    }
}

TEST_CASE("B-hdr-c: EXR ZIP (our own DEFLATE) FLOAT lossless round-trip across multiple 16-line blocks", "[resources][hdr][exr][zip]")
{
    for (const crd::u32 ch : {3U, 1U})
    {
        HdrImage src(&s_hdr);
        fill_test_image(src, 23U, 40U, ch); // 40 rows ⇒ 3 ZIP blocks (16 + 16 + 8)
        Array<crd::u8> enc = hdr_encode_exr(src, ExrPixelType::Float, ExrCompression::Zip, &s_hdr);
        REQUIRE(enc.size() > 0U);
        HdrImage dst(&s_hdr);
        REQUIRE(hdr_decode_exr({enc.data(), enc.size()}, dst, &s_hdr) == HdrError::Ok);
        REQUIRE(dst.width == src.width);
        REQUIRE(dst.height == src.height);
        REQUIRE(dst.channels == src.channels);
        int bad = 0;
        for (crd::usize i = 0; i < src.pixels.size(); ++i) { if (dst.pixels[i] != src.pixels[i]) { ++bad; } }
        CHECK(bad == 0);
    }
}

TEST_CASE("B-hdr-c: EXR ZIP compresses a smooth FLOAT image (where RLE couldn't)", "[resources][hdr][exr][zip]")
{
    HdrImage src(&s_hdr);
    src.resize(64U, 32U, 3U);
    for (crd::u32 y = 0; y < 32U; ++y) { for (crd::u32 x = 0; x < 64U; ++x) { const crd::f32 g = 0.1F + 0.01F * static_cast<crd::f32>(x); src.at(x, y, 0) = g; src.at(x, y, 1) = g * 0.5F; src.at(x, y, 2) = g * 0.25F; } }
    Array<crd::u8> none = hdr_encode_exr(src, ExrPixelType::Float, ExrCompression::None, &s_hdr);
    Array<crd::u8> zip  = hdr_encode_exr(src, ExrPixelType::Float, ExrCompression::Zip, &s_hdr);
    CHECK(zip.size() < none.size());
    HdrImage dst(&s_hdr);
    REQUIRE(hdr_decode_exr({zip.data(), zip.size()}, dst, &s_hdr) == HdrError::Ok);
    int bad = 0;
    for (crd::usize i = 0; i < src.pixels.size(); ++i) { if (dst.pixels[i] != src.pixels[i]) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("B-hdr-c: EXR PIZ (our own wavelet+Huffman) round-trip -- FLOAT lossless + HALF idempotent, multi-block", "[resources][hdr][exr][piz]")
{
    // FLOAT lossless across 32-line PIZ blocks
    for (const crd::u32 ch : {3U, 1U})
    {
        HdrImage src(&s_hdr);
        fill_test_image(src, 23U, 40U, ch);
        Array<crd::u8> enc = hdr_encode_exr(src, ExrPixelType::Float, ExrCompression::Piz, &s_hdr);
        REQUIRE(enc.size() > 0U);
        HdrImage dst(&s_hdr);
        REQUIRE(hdr_decode_exr({enc.data(), enc.size()}, dst, &s_hdr) == HdrError::Ok);
        REQUIRE(dst.width == src.width); REQUIRE(dst.height == src.height); REQUIRE(dst.channels == src.channels);
        int bad = 0;
        for (crd::usize i = 0; i < src.pixels.size(); ++i) { if (dst.pixels[i] != src.pixels[i]) { ++bad; } }
        CHECK(bad == 0);
    }
    // HALF idempotent (encode f1 → decode f2 identical)
    {
        HdrImage src(&s_hdr); fill_test_image(src, 21U, 35U, 3U);
        Array<crd::u8> e1 = hdr_encode_exr(src, ExrPixelType::Half, ExrCompression::Piz, &s_hdr);
        HdrImage f1(&s_hdr); REQUIRE(hdr_decode_exr({e1.data(), e1.size()}, f1, &s_hdr) == HdrError::Ok);
        Array<crd::u8> e2 = hdr_encode_exr(f1, ExrPixelType::Half, ExrCompression::Piz, &s_hdr);
        HdrImage f2(&s_hdr); REQUIRE(hdr_decode_exr({e2.data(), e2.size()}, f2, &s_hdr) == HdrError::Ok);
        int bad = 0; for (crd::usize i = 0; i < f1.pixels.size(); ++i) { if (f1.pixels[i] != f2.pixels[i]) { ++bad; } }
        CHECK(bad == 0);
    }
}

TEST_CASE("B-hdr-c: decode a REAL OpenEXR PIZ file (foreign wavelet+Huffman -> our whole reader)", "[resources][hdr][exr][piz]")
{
    // A 7×5 HALF RGB .exr written by OpenCV's OpenEXR with PIZ compression. Validates our wavelet + Huffman decode against a
    // real OpenEXR encoder — the definitive PIZ test.
    static const crd::u8 kExr[] = {
        118,47,49,1,2,0,0,0,99,104,97,110,110,101,108,115,0,99,104,108,
        105,115,116,0,55,0,0,0,66,0,1,0,0,0,0,0,0,0,1,0,
        0,0,1,0,0,0,71,0,1,0,0,0,0,0,0,0,1,0,0,0,
        1,0,0,0,82,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,
        0,0,0,99,111,109,112,114,101,115,115,105,111,110,0,99,111,109,112,114,
        101,115,115,105,111,110,0,1,0,0,0,4,100,97,116,97,87,105,110,100,
        111,119,0,98,111,120,50,105,0,16,0,0,0,0,0,0,0,0,0,0,
        0,6,0,0,0,4,0,0,0,100,105,115,112,108,97,121,87,105,110,100,
        111,119,0,98,111,120,50,105,0,16,0,0,0,0,0,0,0,0,0,0,
        0,6,0,0,0,4,0,0,0,108,105,110,101,79,114,100,101,114,0,108,
        105,110,101,79,114,100,101,114,0,1,0,0,0,0,112,105,120,101,108,65,
        115,112,101,99,116,82,97,116,105,111,0,102,108,111,97,116,0,4,0,0,
        0,0,0,128,63,115,99,114,101,101,110,87,105,110,100,111,119,67,101,110,
        116,101,114,0,118,50,102,0,8,0,0,0,0,0,0,0,0,0,0,0,
        115,99,114,101,101,110,87,105,110,100,111,119,87,105,100,116,104,0,102,108,
        111,97,116,0,4,0,0,0,0,0,128,63,0,65,1,0,0,0,0,0,
        0,0,0,0,0,210,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,102,50,92,51,41,52,164,52,31,53,154,53,20,54,102,46,205,
        48,102,50,0,52,205,52,154,53,102,54,102,42,102,42,102,42,102,42,102,
        42,102,42,102,42,102,50,92,51,41,52,164,52,31,53,154,53,20,54,10,
        47,31,49,184,50,41,52,246,52,195,53,143,54,102,46,102,46,102,46,102,
        46,102,46,102,46,102,46,102,50,92,51,41,52,164,52,31,53,154,53,20,
        54,174,47,113,49,10,51,82,52,31,53,236,53,184,54,205,48,205,48,205,
        48,205,48,205,48,205,48,205,48,102,50,92,51,41,52,164,52,31,53,154,
        53,20,54,41,48,195,49,92,51,123,52,72,53,20,54,225,54,102,50,102,
        50,102,50,102,50,102,50,102,50,102,50,102,50,92,51,41,52,164,52,31,
        53,154,53,20,54,123,48,20,50,174,51,164,52,113,53,61,54,10,55};
    static const float kPix[] = {
        0.0999755859375F,0.199951171875F,0.0F,0.1500244140625F,0.22998046875F,0.0F,0.199951171875F,
        0.260009765625F,0.0F,0.25F,0.2900390625F,0.0F,0.300048828125F,0.320068359375F,
        0.0F,0.35009765625F,0.35009765625F,0.0F,0.39990234375F,0.3798828125F,0.0F,
        0.1099853515625F,0.199951171875F,0.04998779296875F,0.1600341796875F,0.22998046875F,0.04998779296875F,0.2099609375F,
        0.260009765625F,0.04998779296875F,0.260009765625F,0.2900390625F,0.04998779296875F,0.31005859375F,0.320068359375F,
        0.04998779296875F,0.360107421875F,0.35009765625F,0.04998779296875F,0.409912109375F,0.3798828125F,0.04998779296875F,
        0.1199951171875F,0.199951171875F,0.0999755859375F,0.1700439453125F,0.22998046875F,0.0999755859375F,0.219970703125F,
        0.260009765625F,0.0999755859375F,0.27001953125F,0.2900390625F,0.0999755859375F,0.320068359375F,0.320068359375F,
        0.0999755859375F,0.3701171875F,0.35009765625F,0.0999755859375F,0.419921875F,0.3798828125F,0.0999755859375F,
        0.1300048828125F,0.199951171875F,0.1500244140625F,0.1800537109375F,0.22998046875F,0.1500244140625F,0.22998046875F,
        0.260009765625F,0.1500244140625F,0.280029296875F,0.2900390625F,0.1500244140625F,0.330078125F,0.320068359375F,
        0.1500244140625F,0.3798828125F,0.35009765625F,0.1500244140625F,0.429931640625F,0.3798828125F,0.1500244140625F,
        0.1400146484375F,0.199951171875F,0.199951171875F,0.18994140625F,0.22998046875F,0.199951171875F,0.239990234375F,
        0.260009765625F,0.199951171875F,0.2900390625F,0.2900390625F,0.199951171875F,0.340087890625F,0.320068359375F,
        0.199951171875F,0.389892578125F,0.35009765625F,0.199951171875F,0.43994140625F,0.3798828125F,0.199951171875F};
    HdrImage img(&s_hdr);
    REQUIRE(hdr_decode({kExr, sizeof(kExr)}, img, &s_hdr) == HdrError::Ok);
    REQUIRE(img.width == 7U); REQUIRE(img.height == 5U); REQUIRE(img.channels == 3U); REQUIRE(img.pixels.size() == 105U);
    int bad = 0;
    for (crd::usize i = 0; i < 105U; ++i) { if (img.pixels[i] != kPix[i]) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("B-hdr-c: EXR HALF round-trip is idempotent after the first quantization (NONE + RLE)", "[resources][hdr][exr]")
{
    for (const ExrCompression comp : {ExrCompression::None, ExrCompression::Rle})
    {
        HdrImage src(&s_hdr);
        fill_test_image(src, 21U, 8U, 3U);
        Array<crd::u8> e1 = hdr_encode_exr(src, ExrPixelType::Half, comp, &s_hdr);
        HdrImage f1(&s_hdr);
        REQUIRE(hdr_decode_exr({e1.data(), e1.size()}, f1, &s_hdr) == HdrError::Ok);
        Array<crd::u8> e2 = hdr_encode_exr(f1, ExrPixelType::Half, comp, &s_hdr);
        HdrImage f2(&s_hdr);
        REQUIRE(hdr_decode_exr({e2.data(), e2.size()}, f2, &s_hdr) == HdrError::Ok);
        int px_bad = 0;
        for (crd::usize i = 0; i < f1.pixels.size(); ++i) { if (f1.pixels[i] != f2.pixels[i]) { ++px_bad; } }
        CHECK(px_bad == 0);
        int byte_bad = e1.size() == e2.size() ? 0 : 1; // half quantization is a fixed point ⇒ byte-identical re-encode
        for (crd::usize i = 0; i < e1.size() && i < e2.size(); ++i) { if (e1[i] != e2[i]) { ++byte_bad; } }
        CHECK(byte_bad == 0);
    }
}

TEST_CASE("B-hdr-c: EXR RLE compresses a constant HALF image below its NONE size", "[resources][hdr][exr]")
{
    // HALF constant planes → the predictor yields long byte-runs (float planes alternate period-2 and don't RLE — the
    // reason ZIP/PIZ exist). Constants are half-exact so the value checks are bit-exact.
    HdrImage src(&s_hdr);
    src.resize(96U, 8U, 3U);
    for (crd::u32 y = 0; y < 8U; ++y) { for (crd::u32 x = 0; x < 96U; ++x) { src.at(x, y, 0) = 1.5F; src.at(x, y, 1) = 0.5F; src.at(x, y, 2) = 0.25F; } }
    Array<crd::u8> none = hdr_encode_exr(src, ExrPixelType::Half, ExrCompression::None, &s_hdr);
    Array<crd::u8> rle  = hdr_encode_exr(src, ExrPixelType::Half, ExrCompression::Rle, &s_hdr);
    CHECK(rle.size() < none.size());
    HdrImage dst(&s_hdr);
    REQUIRE(hdr_decode_exr({rle.data(), rle.size()}, dst, &s_hdr) == HdrError::Ok);
    CHECK(dst.at(0, 0, 0) == 1.5F);
    CHECK(dst.at(95, 7, 2) == 0.25F);
}

TEST_CASE("B-hdr-c: decode a REAL OpenEXR ZIP file (foreign encoder -> our whole reader end-to-end)", "[resources][hdr][exr][zip]")
{
    // A 7×5 FLOAT RGB .exr written by OpenCV's OpenEXR (ZIP compression = zlib). Validates the header/chlist parse, offset
    // table, block split, reorder+predictor undo, our own inflate, and float decode — against a real OpenEXR encoder.
    static const crd::u8 kExr[] = {
        118,47,49,1,2,0,0,0,99,104,97,110,110,101,108,115,0,99,104,108,
        105,115,116,0,55,0,0,0,66,0,2,0,0,0,0,0,0,0,1,0,
        0,0,1,0,0,0,71,0,2,0,0,0,0,0,0,0,1,0,0,0,
        1,0,0,0,82,0,2,0,0,0,0,0,0,0,1,0,0,0,1,0,
        0,0,0,99,111,109,112,114,101,115,115,105,111,110,0,99,111,109,112,114,
        101,115,115,105,111,110,0,1,0,0,0,3,100,97,116,97,87,105,110,100,
        111,119,0,98,111,120,50,105,0,16,0,0,0,0,0,0,0,0,0,0,
        0,6,0,0,0,4,0,0,0,100,105,115,112,108,97,121,87,105,110,100,
        111,119,0,98,111,120,50,105,0,16,0,0,0,0,0,0,0,0,0,0,
        0,6,0,0,0,4,0,0,0,108,105,110,101,79,114,100,101,114,0,108,
        105,110,101,79,114,100,101,114,0,1,0,0,0,0,112,105,120,101,108,65,
        115,112,101,99,116,82,97,116,105,111,0,102,108,111,97,116,0,4,0,0,
        0,0,0,128,63,115,99,114,101,101,110,87,105,110,100,111,119,67,101,110,
        116,101,114,0,118,50,102,0,8,0,0,0,0,0,0,0,0,0,0,0,
        115,99,114,101,101,110,87,105,110,100,111,119,87,105,100,116,104,0,102,108,
        111,97,116,0,4,0,0,0,0,0,128,63,0,65,1,0,0,0,0,0,
        0,0,0,0,0,231,0,0,0,120,156,99,104,64,6,190,255,131,207,156,
        245,189,99,252,77,82,128,65,243,89,119,189,223,127,147,255,38,12,179,234,
        165,128,184,241,63,35,10,68,86,153,179,121,229,204,89,179,30,250,250,158,
        57,231,227,123,166,166,190,17,25,162,168,244,125,198,102,204,96,60,97,38,
        131,100,227,76,6,41,103,20,19,77,80,84,150,157,77,63,147,126,198,238,
        217,150,84,147,103,91,210,10,241,216,254,60,237,44,16,190,49,78,223,252,
        12,136,231,163,248,200,231,211,113,203,132,5,123,92,36,159,151,118,95,212,
        231,251,120,71,149,239,147,211,190,219,170,165,221,124,159,248,62,242,35,67,
        20,149,157,101,82,207,125,182,36,44,248,197,54,57,87,247,50,62,149,44,
        191,195,87,117,150,215,55,74,62,223,228,231,179,229,182,42,50,228,67,81,
        153,176,96,114,238,113,203,121,73,230,39,47,234,103,77,3,218,143,2,145,
        85,206,75,2,154,251,103,143,75,216,170,15,2,157,229,0,68,235,208,191};
    static const float kPix[] = {
        0.10000000149011612F,0.20000000298023224F,0.0F,0.15000000596046448F,0.23000000417232513F,0.0F,0.20000000298023224F,
        0.25999999046325684F,0.0F,0.25F,0.28999999165534973F,0.0F,0.30000001192092896F,0.3199999928474426F,
        0.0F,0.3499999940395355F,0.3499999940395355F,0.0F,0.4000000059604645F,0.3799999952316284F,0.0F,
        0.10999999940395355F,0.20000000298023224F,0.05000000074505806F,0.1599999964237213F,0.23000000417232513F,0.05000000074505806F,0.20999999344348907F,
        0.25999999046325684F,0.05000000074505806F,0.25999999046325684F,0.28999999165534973F,0.05000000074505806F,0.3100000023841858F,0.3199999928474426F,
        0.05000000074505806F,0.36000001430511475F,0.3499999940395355F,0.05000000074505806F,0.4099999964237213F,0.3799999952316284F,0.05000000074505806F,
        0.11999999731779099F,0.20000000298023224F,0.10000000149011612F,0.17000000178813934F,0.23000000417232513F,0.10000000149011612F,0.2199999988079071F,
        0.25999999046325684F,0.10000000149011612F,0.27000001072883606F,0.28999999165534973F,0.10000000149011612F,0.3199999928474426F,0.3199999928474426F,
        0.10000000149011612F,0.3700000047683716F,0.3499999940395355F,0.10000000149011612F,0.41999998688697815F,0.3799999952316284F,0.10000000149011612F,
        0.12999999523162842F,0.20000000298023224F,0.15000000596046448F,0.18000000715255737F,0.23000000417232513F,0.15000000596046448F,0.23000000417232513F,
        0.25999999046325684F,0.15000000596046448F,0.2800000011920929F,0.28999999165534973F,0.15000000596046448F,0.33000001311302185F,0.3199999928474426F,
        0.15000000596046448F,0.3799999952316284F,0.3499999940395355F,0.15000000596046448F,0.4300000071525574F,0.3799999952316284F,0.15000000596046448F,
        0.14000000059604645F,0.20000000298023224F,0.20000000298023224F,0.1899999976158142F,0.23000000417232513F,0.20000000298023224F,0.23999999463558197F,
        0.25999999046325684F,0.20000000298023224F,0.28999999165534973F,0.28999999165534973F,0.20000000298023224F,0.3400000035762787F,0.3199999928474426F,
        0.20000000298023224F,0.38999998569488525F,0.3499999940395355F,0.20000000298023224F,0.4399999976158142F,0.3799999952316284F,0.20000000298023224F};
    HdrImage img(&s_hdr);
    REQUIRE(hdr_decode({kExr, sizeof(kExr)}, img, &s_hdr) == HdrError::Ok);
    REQUIRE(img.width == 7U);
    REQUIRE(img.height == 5U);
    REQUIRE(img.channels == 3U);
    REQUIRE(img.pixels.size() == 105U);
    int bad = 0;
    for (crd::usize i = 0; i < 105U; ++i) { if (img.pixels[i] != kPix[i]) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("B-hdr-c: hdr_decode auto-detects EXR", "[resources][hdr][exr]")
{
    HdrImage src(&s_hdr);
    fill_test_image(src, 18U, 6U, 3U);
    Array<crd::u8> exr = hdr_encode_exr(src, ExrPixelType::Float, ExrCompression::None, &s_hdr);
    HdrImage dst(&s_hdr);
    REQUIRE(hdr_decode({exr.data(), exr.size()}, dst, &s_hdr) == HdrError::Ok);
    int bad = 0;
    for (crd::usize i = 0; i < src.pixels.size(); ++i) { if (dst.pixels[i] != src.pixels[i]) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("B-hdr: HdrImage <-> CRDR is a LOSSLESS round-trip", "[resources][hdr][crdr]")
{
    HdrImage src(&s_hdr);
    fill_test_image(src, 17U, 9U, 3U);
    const ResourceId id{0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};
    Array<crd::u8> blob = hdr_to_crdr(src, id, &s_hdr);
    REQUIRE(blob.size() > 0U);
    HdrImage dst(&s_hdr);
    REQUIRE(hdr_from_crdr({blob.data(), blob.size()}, dst, &s_hdr) == HdrError::Ok);
    REQUIRE(dst.width == src.width);
    REQUIRE(dst.height == src.height);
    REQUIRE(dst.channels == src.channels);
    int bad = 0;
    for (crd::usize i = 0; i < src.pixels.size(); ++i) { if (dst.pixels[i] != src.pixels[i]) { ++bad; } }
    CHECK(bad == 0);
}
