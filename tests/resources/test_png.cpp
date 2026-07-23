// tests/resources/test_png.cpp — the PNG decoder gates. Every fixture is built IN-MEMORY with OUR zlib_deflate + OUR
// png_crc32 (the codec proves itself end-to-end through its own compression stack): all five filters reconstructed
// against reference pixels, every color type × representative bit depths, palette+tRNS, color keys, 16-bit
// downconversion, Adam7 interlace scatter, and the failure classes (CRC corruption is REJECTED, size contracts hold).

#include <catch2/catch_test_macros.hpp>

#include <crd/resources/deflate.hpp>
#include <crd/resources/png_image.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstring>

using namespace crd::resources;

namespace
{

void push_be32(crd::containers::Array<crd::u8>& b, crd::u32 v)
{
    b.push_back(static_cast<crd::u8>(v >> 24U));
    b.push_back(static_cast<crd::u8>(v >> 16U));
    b.push_back(static_cast<crd::u8>(v >> 8U));
    b.push_back(static_cast<crd::u8>(v));
}

void add_chunk(crd::containers::Array<crd::u8>& png, const char* type, const crd::containers::Array<crd::u8>& data)
{
    push_be32(png, static_cast<crd::u32>(data.size()));
    crd::containers::Array<crd::u8> crc_input(data.allocator());
    for (int i = 0; i < 4; ++i)
    {
        png.push_back(static_cast<crd::u8>(type[i]));
        crc_input.push_back(static_cast<crd::u8>(type[i]));
    }
    for (crd::usize i = 0; i < data.size(); ++i)
    {
        png.push_back(data[i]);
        crc_input.push_back(data[i]);
    }
    push_be32(png, png_crc32(crd::containers::as_const_span(crc_input)));
}

void begin_png(crd::containers::Array<crd::u8>& png, crd::memory::IAllocator* a, crd::u32 w, crd::u32 h, crd::u8 depth,
               crd::u8 color_type, crd::u8 interlace)
{
    static constexpr crd::u8 kSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    for (crd::u8 s : kSig) { png.push_back(s); }
    crd::containers::Array<crd::u8> ihdr(a);
    push_be32(ihdr, w);
    push_be32(ihdr, h);
    ihdr.push_back(depth);
    ihdr.push_back(color_type);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(interlace);
    add_chunk(png, "IHDR", ihdr);
}

void add_idat_end(crd::containers::Array<crd::u8>& png, crd::memory::IAllocator* a,
                  const crd::containers::Array<crd::u8>& raw)
{
    const auto compressed = zlib_deflate(crd::containers::as_const_span(raw), a);
    add_chunk(png, "IDAT", compressed);
    crd::containers::Array<crd::u8> empty(a);
    add_chunk(png, "IEND", empty);
}

[[nodiscard]] crd::u8 paeth_ref(int a, int b, int c)
{
    const int p  = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) { return static_cast<crd::u8>(a); }
    if (pb <= pc) { return static_cast<crd::u8>(b); }
    return static_cast<crd::u8>(c);
}

// FORWARD-filter one row (the encoder side, test-only) so the decoder's reconstruction is exercised for real
void filter_row(crd::containers::Array<crd::u8>& out, const crd::u8* row, const crd::u8* prior, crd::usize rb,
                crd::u32 bpp, crd::u8 filter)
{
    out.push_back(filter);
    for (crd::usize x = 0; x < rb; ++x)
    {
        const int left = x >= bpp ? row[x - bpp] : 0;
        const int up   = prior != nullptr ? prior[x] : 0;
        const int ul   = (prior != nullptr && x >= bpp) ? prior[x - bpp] : 0;
        int       v    = row[x];
        switch (filter)
        {
        case 1: v -= left; break;
        case 2: v -= up; break;
        case 3: v -= (left + up) / 2; break;
        case 4: v -= paeth_ref(left, up, ul); break;
        default: break;
        }
        out.push_back(static_cast<crd::u8>(v));
    }
}

} // namespace

TEST_CASE("resources: PNG all five FILTERS reconstruct exactly (RGBA8)", "[resources][png]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         w = 4;
    constexpr crd::u32         h = 5; // one row per filter: None/Sub/Up/Average/Paeth
    crd::u8                    ref[h][w * 4];
    for (crd::u32 y = 0; y < h; ++y)
    {
        for (crd::u32 x = 0; x < w * 4U; ++x) { ref[y][x] = static_cast<crd::u8>(13U + 17U * y + 29U * x); }
    }
    crd::containers::Array<crd::u8> raw(&alloc);
    for (crd::u32 y = 0; y < h; ++y)
    {
        filter_row(raw, ref[y], y > 0U ? ref[y - 1] : nullptr, w * 4U, 4, static_cast<crd::u8>(y));
    }
    crd::containers::Array<crd::u8> png(&alloc);
    begin_png(png, &alloc, w, h, 8, 6, 0);
    add_idat_end(png, &alloc, raw);

    PngImage img(&alloc);
    REQUIRE(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::Ok);
    CHECK(img.width == w);
    CHECK(img.height == h);
    CHECK(img.source_bit_depth == 8);
    CHECK(img.source_channels == 4);
    for (crd::u32 y = 0; y < h; ++y)
    {
        CHECK(std::memcmp(img.pixels.data() + static_cast<crd::usize>(y) * w * 4U, ref[y], w * 4U) == 0);
    }
}

TEST_CASE("resources: PNG color-type coverage -- RGB, gray1, gray+alpha, palette4+tRNS, RGB key, 16-bit", "[resources][png]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    SECTION("RGB8 expands to opaque RGBA")
    {
        const crd::u8                   px[6] = {10, 20, 30, 200, 210, 220}; // 2x1
        crd::containers::Array<crd::u8> raw(&alloc);
        raw.push_back(0);
        for (crd::u8 v : px) { raw.push_back(v); }
        crd::containers::Array<crd::u8> png(&alloc);
        begin_png(png, &alloc, 2, 1, 8, 2, 0);
        add_idat_end(png, &alloc, raw);
        PngImage img(&alloc);
        REQUIRE(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::Ok);
        CHECK(img.pixels[0] == 10);
        CHECK(img.pixels[3] == 255);
        CHECK(img.pixels[4] == 200);
        CHECK(img.pixels[7] == 255);
    }
    SECTION("1-bit GRAY unpacks bits, scales to 0/255")
    {
        // 8x1: bit pattern 0b10110001
        crd::containers::Array<crd::u8> raw(&alloc);
        raw.push_back(0);
        raw.push_back(0xB1U);
        crd::containers::Array<crd::u8> png(&alloc);
        begin_png(png, &alloc, 8, 1, 1, 0, 0);
        add_idat_end(png, &alloc, raw);
        PngImage img(&alloc);
        REQUIRE(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::Ok);
        const crd::u8 expect[8] = {255, 0, 255, 255, 0, 0, 0, 255};
        for (int i = 0; i < 8; ++i)
        {
            CHECK(img.pixels[static_cast<crd::usize>(i) * 4U] == expect[i]);
            CHECK(img.pixels[static_cast<crd::usize>(i) * 4U + 3U] == 255);
        }
    }
    SECTION("GRAY+ALPHA 8-bit")
    {
        const crd::u8                   px[4] = {100, 50, 200, 250}; // 2x1: (g,a)(g,a)
        crd::containers::Array<crd::u8> raw(&alloc);
        raw.push_back(0);
        for (crd::u8 v : px) { raw.push_back(v); }
        crd::containers::Array<crd::u8> png(&alloc);
        begin_png(png, &alloc, 2, 1, 8, 4, 0);
        add_idat_end(png, &alloc, raw);
        PngImage img(&alloc);
        REQUIRE(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::Ok);
        CHECK(img.pixels[0] == 100);
        CHECK(img.pixels[1] == 100);
        CHECK(img.pixels[3] == 50);
        CHECK(img.pixels[4] == 200);
        CHECK(img.pixels[7] == 250);
    }
    SECTION("4-bit PALETTE + tRNS alpha")
    {
        crd::containers::Array<crd::u8> png(&alloc);
        begin_png(png, &alloc, 4, 1, 4, 3, 0);
        crd::containers::Array<crd::u8> plte(&alloc); // 3 entries: red, green, blue
        const crd::u8                   pal[9] = {255, 0, 0, 0, 255, 0, 0, 0, 255};
        for (crd::u8 v : pal) { plte.push_back(v); }
        add_chunk(png, "PLTE", plte);
        crd::containers::Array<crd::u8> trns(&alloc); // entry 1 (green) half-transparent
        trns.push_back(255);
        trns.push_back(128);
        add_chunk(png, "tRNS", trns);
        crd::containers::Array<crd::u8> raw(&alloc); // indices 0,1,2,0 → nibbles 0x01 0x20
        raw.push_back(0);
        raw.push_back(0x01U);
        raw.push_back(0x20U);
        add_idat_end(png, &alloc, raw);
        PngImage img(&alloc);
        REQUIRE(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::Ok);
        CHECK(img.pixels[0] == 255);  // red
        CHECK(img.pixels[3] == 255);
        CHECK(img.pixels[5] == 255);  // green...
        CHECK(img.pixels[7] == 128);  // ...half-transparent via tRNS
        CHECK(img.pixels[10] == 255); // blue
        CHECK(img.pixels[11] == 255); // beyond the tRNS array → opaque
    }
    SECTION("RGB8 with a tRNS COLOR KEY")
    {
        crd::containers::Array<crd::u8> png(&alloc);
        begin_png(png, &alloc, 2, 1, 8, 2, 0);
        crd::containers::Array<crd::u8> trns(&alloc); // key = (10,20,30) as 16-bit fields
        const crd::u8                   key[6] = {0, 10, 0, 20, 0, 30};
        for (crd::u8 v : key) { trns.push_back(v); }
        add_chunk(png, "tRNS", trns);
        crd::containers::Array<crd::u8> raw(&alloc);
        raw.push_back(0);
        const crd::u8 px[6] = {10, 20, 30, 99, 20, 30}; // first pixel IS the key
        for (crd::u8 v : px) { raw.push_back(v); }
        add_idat_end(png, &alloc, raw);
        PngImage img(&alloc);
        REQUIRE(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::Ok);
        CHECK(img.pixels[3] == 0);   // keyed out
        CHECK(img.pixels[7] == 255); // not the key
    }
    SECTION("16-bit RGB downconverts via the high byte")
    {
        crd::containers::Array<crd::u8> raw(&alloc);
        raw.push_back(0);
        const crd::u8 px[6] = {0xAB, 0xCD, 0x12, 0x34, 0xFF, 0x00}; // one pixel: R=0xABCD G=0x1234 B=0xFF00
        for (crd::u8 v : px) { raw.push_back(v); }
        crd::containers::Array<crd::u8> png(&alloc);
        begin_png(png, &alloc, 1, 1, 16, 2, 0);
        add_idat_end(png, &alloc, raw);
        PngImage img(&alloc);
        REQUIRE(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::Ok);
        CHECK(img.pixels[0] == 0xAB);
        CHECK(img.pixels[1] == 0x12);
        CHECK(img.pixels[2] == 0xFF);
        CHECK(img.source_bit_depth == 16);
    }
}

TEST_CASE("resources: PNG Adam7 INTERLACE scatters correctly (8x8 RGBA)", "[resources][png]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         n = 8;
    crd::u8                    ref[n * n * 4];
    for (crd::u32 i = 0; i < n * n; ++i)
    {
        ref[i * 4U + 0U] = static_cast<crd::u8>(i);        // a unique value per pixel: any mis-scatter is caught
        ref[i * 4U + 1U] = static_cast<crd::u8>(255U - i);
        ref[i * 4U + 2U] = static_cast<crd::u8>(i * 3U);
        ref[i * 4U + 3U] = 255;
    }
    // generate the 7 passes per the dispersal grid (filter 0 rows)
    const crd::u32 x0[7] = {0, 4, 0, 2, 0, 1, 0};
    const crd::u32 y0[7] = {0, 0, 4, 0, 2, 0, 1};
    const crd::u32 dx[7] = {8, 8, 4, 4, 2, 2, 1};
    const crd::u32 dy[7] = {8, 8, 8, 4, 4, 2, 2};
    crd::containers::Array<crd::u8> raw(&alloc);
    for (int pass = 0; pass < 7; ++pass)
    {
        for (crd::u32 y = y0[pass]; y < n; y += dy[pass])
        {
            raw.push_back(0); // filter: None
            for (crd::u32 x = x0[pass]; x < n; x += dx[pass])
            {
                for (int c = 0; c < 4; ++c) { raw.push_back(ref[(y * n + x) * 4U + static_cast<crd::u32>(c)]); }
            }
        }
    }
    crd::containers::Array<crd::u8> png(&alloc);
    begin_png(png, &alloc, n, n, 8, 6, 1); // interlace = Adam7
    add_idat_end(png, &alloc, raw);

    PngImage img(&alloc);
    REQUIRE(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::Ok);
    CHECK(std::memcmp(img.pixels.data(), ref, sizeof(ref)) == 0); // BYTE-exact scatter
}

TEST_CASE("resources: PNG failure classes", "[resources][png]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);

    SECTION("bad magic")
    {
        const crd::u8 junk[16] = {1, 2, 3};
        PngImage      img(&alloc);
        CHECK(png_decode(crd::containers::ConstSpan<crd::u8>(junk, 16), img, &alloc) == PngError::BadMagic);
    }
    SECTION("a corrupted chunk CRC is REJECTED")
    {
        crd::containers::Array<crd::u8> raw(&alloc);
        raw.push_back(0);
        raw.push_back(7);
        crd::containers::Array<crd::u8> png(&alloc);
        begin_png(png, &alloc, 1, 1, 8, 0, 0);
        add_idat_end(png, &alloc, raw);
        png[20] ^= 0xFFU; // flip a byte inside IHDR's data → its CRC no longer matches
        PngImage img(&alloc);
        CHECK(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::BadChunkCrc);
    }
    SECTION("truncated mid-chunk")
    {
        crd::containers::Array<crd::u8> raw(&alloc);
        raw.push_back(0);
        raw.push_back(7);
        crd::containers::Array<crd::u8> png(&alloc);
        begin_png(png, &alloc, 1, 1, 8, 0, 0);
        add_idat_end(png, &alloc, raw);
        PngImage img(&alloc);
        CHECK(png_decode(crd::containers::ConstSpan<crd::u8>(png.data(), png.size() - 6U), img, &alloc)
              == PngError::Truncated);
    }
    SECTION("an illegal depth-color combination is BadHeader")
    {
        crd::containers::Array<crd::u8> raw(&alloc);
        raw.push_back(0);
        raw.push_back(7);
        crd::containers::Array<crd::u8> png(&alloc);
        begin_png(png, &alloc, 1, 1, 4, 2, 0); // 4-bit RGB is illegal
        add_idat_end(png, &alloc, raw);
        PngImage img(&alloc);
        CHECK(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::BadHeader);
    }
    SECTION("a bad filter id in the stream is BadData")
    {
        crd::containers::Array<crd::u8> raw(&alloc);
        raw.push_back(9); // filter 9 does not exist
        raw.push_back(7);
        crd::containers::Array<crd::u8> png(&alloc);
        begin_png(png, &alloc, 1, 1, 8, 0, 0);
        add_idat_end(png, &alloc, raw);
        PngImage img(&alloc);
        CHECK(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::BadData);
    }
    SECTION("a palette index past PLTE is BadData")
    {
        crd::containers::Array<crd::u8> png(&alloc);
        begin_png(png, &alloc, 1, 1, 8, 3, 0);
        crd::containers::Array<crd::u8> plte(&alloc); // ONE entry
        plte.push_back(1);
        plte.push_back(2);
        plte.push_back(3);
        add_chunk(png, "PLTE", plte);
        crd::containers::Array<crd::u8> raw(&alloc);
        raw.push_back(0);
        raw.push_back(5); // index 5 of a 1-entry palette
        add_idat_end(png, &alloc, raw);
        PngImage img(&alloc);
        CHECK(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::BadData);
    }
    SECTION("a size-contract violation (extra decompressed bytes) is BadData")
    {
        crd::containers::Array<crd::u8> raw(&alloc);
        raw.push_back(0);
        raw.push_back(7);
        raw.push_back(99); // one byte too many for a 1x1 gray8
        crd::containers::Array<crd::u8> png(&alloc);
        begin_png(png, &alloc, 1, 1, 8, 0, 0);
        add_idat_end(png, &alloc, raw);
        PngImage img(&alloc);
        CHECK(png_decode(crd::containers::as_const_span(png), img, &alloc) == PngError::BadData);
    }
}
