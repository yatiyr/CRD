// tests/resources/test_tga_bmp.cpp — TGA + BMP decoder gates (hermetic in-memory fixtures): raw + RLE paths, palettes,
// bit-packing, channel masks, both row origins, and the failure classes.

#include <catch2/catch_test_macros.hpp>

#include <crd/resources/bmp_image.hpp>
#include <crd/resources/ldr_image.hpp>
#include <crd/resources/tga_image.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstring>

using namespace crd::resources;

namespace
{
void push16(crd::containers::Array<crd::u8>& b, crd::u16 v)
{
    b.push_back(static_cast<crd::u8>(v & 0xFFU));
    b.push_back(static_cast<crd::u8>(v >> 8U));
}
void push32(crd::containers::Array<crd::u8>& b, crd::u32 v)
{
    for (int i = 0; i < 4; ++i) { b.push_back(static_cast<crd::u8>(v >> (8 * i))); }
}
void tga_header(crd::containers::Array<crd::u8>& b, crd::u8 type, crd::u16 w, crd::u16 h, crd::u8 bpp, crd::u8 desc,
                crd::u8 cmap_type = 0, crd::u16 cmap_len = 0, crd::u8 cmap_bits = 0)
{
    b.push_back(0);         // id_len
    b.push_back(cmap_type); // cmap
    b.push_back(type);
    push16(b, 0);
    push16(b, cmap_len);
    b.push_back(cmap_bits);
    push16(b, 0);
    push16(b, 0);
    push16(b, w);
    push16(b, h);
    b.push_back(bpp);
    b.push_back(desc);
}
} // namespace

TEST_CASE("resources: TGA raw 24-bit bottom-up + RLE 32-bit + palette + 16-bit", "[resources][tga]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    SECTION("raw 24-bit BGR, BOTTOM-up origin flips rows")
    {
        crd::containers::Array<crd::u8> t(&alloc);
        tga_header(t, 2, 1, 2, 24, 0); // bottom-origin
        const crd::u8 bottom[3] = {10, 20, 30}; // stored first = image BOTTOM row (B,G,R)
        const crd::u8 top[3]    = {40, 50, 60};
        for (crd::u8 v : bottom) { t.push_back(v); }
        for (crd::u8 v : top) { t.push_back(v); }
        LdrImage img(&alloc);
        REQUIRE(tga_decode(crd::containers::as_const_span(t), img, &alloc) == LdrError::Ok);
        CHECK(img.pixels[0] == 60); // row 0 (TOP) = the second stored record, BGR→RGB
        CHECK(img.pixels[2] == 40);
        CHECK(img.pixels[4 + 0] == 30);
        CHECK(ldr_sniff(crd::containers::as_const_span(t)) == LdrCodec::Tga); // heuristic sniff claims it
    }
    SECTION("RLE 32-bit: a run + literals")
    {
        crd::containers::Array<crd::u8> t(&alloc);
        tga_header(t, 10, 3, 1, 32, 0x20U); // top-origin
        t.push_back(0x81U);                 // run of 2
        const crd::u8 bgra[4] = {1, 2, 3, 200};
        for (crd::u8 v : bgra) { t.push_back(v); }
        t.push_back(0x00U); // 1 literal
        const crd::u8 lit[4] = {9, 8, 7, 100};
        for (crd::u8 v : lit) { t.push_back(v); }
        LdrImage img(&alloc);
        REQUIRE(tga_decode(crd::containers::as_const_span(t), img, &alloc) == LdrError::Ok);
        CHECK(img.pixels[0] == 3);  // BGRA→RGBA
        CHECK(img.pixels[3] == 200);
        CHECK(img.pixels[4] == 3);  // the run repeated
        CHECK(img.pixels[8] == 7);  // the literal
        CHECK(img.pixels[11] == 100);
    }
    SECTION("8-bit palette + 16-bit A1R5G5B5")
    {
        crd::containers::Array<crd::u8> t(&alloc);
        tga_header(t, 1, 2, 1, 8, 0x20U, 1, 2, 24); // palette: 2 × 24-bit BGR entries
        const crd::u8 pal[6] = {0, 0, 255, 0, 255, 0}; // red, green
        for (crd::u8 v : pal) { t.push_back(v); }
        t.push_back(1); // indices: green, red
        t.push_back(0);
        LdrImage img(&alloc);
        REQUIRE(tga_decode(crd::containers::as_const_span(t), img, &alloc) == LdrError::Ok);
        CHECK(img.pixels[1] == 255); // green first
        CHECK(img.pixels[4] == 255); // then red

        crd::containers::Array<crd::u8> t16(&alloc);
        tga_header(t16, 2, 1, 1, 16, 0x20U);
        push16(t16, static_cast<crd::u16>((31U << 10U) | (0U << 5U) | 15U)); // R=31 G=0 B=15
        LdrImage img16(&alloc);
        REQUIRE(tga_decode(crd::containers::as_const_span(t16), img16, &alloc) == LdrError::Ok);
        CHECK(img16.pixels[0] == 255);
        CHECK(img16.pixels[1] == 0);
        CHECK(img16.pixels[2] == 123); // 15/31 → 123
    }
    SECTION("truncated pixel data")
    {
        crd::containers::Array<crd::u8> t(&alloc);
        tga_header(t, 2, 4, 4, 24, 0);
        t.push_back(1); // far too few bytes
        LdrImage img(&alloc);
        CHECK(tga_decode(crd::containers::as_const_span(t), img, &alloc) == LdrError::Truncated);
    }
}

TEST_CASE("resources: BMP 24-bit padding + palette + bitfields + RLE8 + top-down", "[resources][bmp]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    const auto bmp_headers = [&](crd::containers::Array<crd::u8>& b, crd::i32 w, crd::i32 h, crd::u16 bpp, crd::u32 comp,
                                 crd::u32 pal_n, crd::u32 pixel_off) {
        b.push_back('B');
        b.push_back('M');
        push32(b, 0); // file size (unchecked)
        push32(b, 0);
        push32(b, pixel_off);
        push32(b, 40);
        push32(b, static_cast<crd::u32>(w));
        push32(b, static_cast<crd::u32>(h));
        push16(b, 1);
        push16(b, bpp);
        push32(b, comp);
        push32(b, 0);
        push32(b, 0);
        push32(b, 0);
        push32(b, pal_n);
        push32(b, 0);
    };

    SECTION("24-bit bottom-up with 4-byte row padding (3x2)")
    {
        crd::containers::Array<crd::u8> b(&alloc);
        bmp_headers(b, 3, 2, 24, 0, 0, 54);
        // bottom row first; 3·3=9 bytes padded to 12
        const crd::u8 row_bot[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9}; // BGR triplets
        const crd::u8 row_top[9] = {11, 12, 13, 14, 15, 16, 17, 18, 19};
        for (crd::u8 v : row_bot) { b.push_back(v); }
        for (int i = 0; i < 3; ++i) { b.push_back(0); }
        for (crd::u8 v : row_top) { b.push_back(v); }
        for (int i = 0; i < 3; ++i) { b.push_back(0); }
        LdrImage img(&alloc);
        REQUIRE(bmp_decode(crd::containers::as_const_span(b), img, &alloc) == LdrError::Ok);
        CHECK(img.pixels[0] == 13); // top row (stored second), BGR→RGB
        CHECK(img.pixels[2] == 11);
        CHECK(img.pixels[(3 + 0) * 4] == 3); // bottom row
    }
    SECTION("8-bit palette + RLE8 (run, EOL, absolute, EOB)")
    {
        crd::containers::Array<crd::u8> b(&alloc);
        bmp_headers(b, 4, 2, 8, 1, 2, 54 + 8);
        const crd::u8 pal[8] = {0, 0, 255, 0, /*red*/ 0, 255, 0, 0 /*green*/};
        for (crd::u8 v : pal) { b.push_back(v); }
        // bottom row: run 3×idx0, then 1 literal idx1 (absolute needs ≥3 — use a 1-run) | EOL | top row: absolute 4 | EOB
        b.push_back(3);
        b.push_back(0); // run: 3 × red
        b.push_back(1);
        b.push_back(1); // run: 1 × green
        b.push_back(0);
        b.push_back(0); // EOL
        b.push_back(0);
        b.push_back(4); // absolute: 4 literals
        b.push_back(1);
        b.push_back(0);
        b.push_back(1);
        b.push_back(0); // (even count → already 16-bit aligned)
        b.push_back(0);
        b.push_back(1); // EOB
        LdrImage img(&alloc);
        REQUIRE(bmp_decode(crd::containers::as_const_span(b), img, &alloc) == LdrError::Ok);
        CHECK(img.pixels[(4 + 0) * 4 + 0] == 255); // bottom row starts red
        CHECK(img.pixels[(4 + 3) * 4 + 1] == 255); // ...ends green
        CHECK(img.pixels[0 * 4 + 1] == 255);       // top row absolute: green,red,green,red
        CHECK(img.pixels[1 * 4 + 0] == 255);
    }
    SECTION("32-bit BITFIELDS masks + TOP-DOWN height")
    {
        crd::containers::Array<crd::u8> b(&alloc);
        bmp_headers(b, 1, -1, 32, 3, 0, 54 + 12); // negative height = top-down; 3 masks follow the header
        push32(b, 0x00FF0000U);                   // R
        push32(b, 0x0000FF00U);                   // G
        push32(b, 0x000000FFU);                   // B
        push32(b, 0x11223344U);                   // one pixel: R=0x22 G=0x33 B=0x44 (alpha mask absent → opaque)
        LdrImage img(&alloc);
        REQUIRE(bmp_decode(crd::containers::as_const_span(b), img, &alloc) == LdrError::Ok);
        CHECK(img.pixels[0] == 0x22);
        CHECK(img.pixels[1] == 0x33);
        CHECK(img.pixels[2] == 0x44);
        CHECK(img.pixels[3] == 255);
    }
    SECTION("OS/2 core header is Unsupported BY NAME")
    {
        crd::containers::Array<crd::u8> b(&alloc);
        b.push_back('B');
        b.push_back('M');
        for (int i = 0; i < 12; ++i) { b.push_back(0); }
        push32(b, 12U); // core header size
        for (int i = 0; i < 40; ++i) { b.push_back(0); }
        LdrImage img(&alloc);
        CHECK(bmp_decode(crd::containers::as_const_span(b), img, &alloc) == LdrError::Unsupported);
    }
}
