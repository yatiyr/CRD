#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/resources/deflate.hpp>

#include <new>

using namespace crd::resources;
using crd::containers::Array;
using crd::containers::ConstSpan;

namespace
{
alignas(crd::memory::GrowableTlsfAllocator) unsigned char s_df_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
crd::memory::GrowableTlsfAllocator& s_df = *::new (s_df_buf) crd::memory::GrowableTlsfAllocator();

void roundtrip_raw(const Array<crd::u8>& src, const char* label)
{
    INFO(label);
    CAPTURE(src.size());
    Array<crd::u8> comp = deflate_raw({src.data(), src.size()}, &s_df);
    Array<crd::u8> back(&s_df);
    REQUIRE(inflate_raw({comp.data(), comp.size()}, back));
    REQUIRE(back.size() == src.size());
    int bad = 0; for (crd::usize i = 0; i < src.size(); ++i) { if (back[i] != src[i]) { ++bad; } }
    CHECK(bad == 0);
}
void roundtrip_zlib(const Array<crd::u8>& src, const char* label)
{
    INFO(label);
    CAPTURE(src.size());
    Array<crd::u8> comp = zlib_deflate({src.data(), src.size()}, &s_df);
    Array<crd::u8> back(&s_df);
    REQUIRE(zlib_inflate({comp.data(), comp.size()}, back)); // also verifies the Adler-32 trailer
    REQUIRE(back.size() == src.size());
    int bad = 0; for (crd::usize i = 0; i < src.size(); ++i) { if (back[i] != src[i]) { ++bad; } }
    CHECK(bad == 0);
}
} // namespace

TEST_CASE("deflate: Adler-32 matches the known 'Wikipedia' vector", "[resources][deflate]")
{
    const char* s = "Wikipedia";
    CHECK(adler32({reinterpret_cast<const crd::u8*>(s), 9}) == 0x11E60398U);
    const crd::u8 abc[3] = {'A', 'B', 'C'};
    CHECK(adler32({abc, 3}) == 0x018D00C7U);
}

TEST_CASE("deflate: inflate a HAND-BUILT stored zlib stream (wrapper + stored + Adler)", "[resources][deflate]")
{
    // 78 01 | [stored block: bfinal=1 btype=00] 03 00 FC FF 'A' 'B' 'C' | adler 01 8D 00 C7
    const crd::u8 stream[] = {0x78, 0x01, 0x01, 0x03, 0x00, 0xFC, 0xFF, 0x41, 0x42, 0x43, 0x01, 0x8D, 0x00, 0xC7};
    Array<crd::u8> out(&s_df);
    REQUIRE(zlib_inflate({stream, sizeof(stream)}, out));
    REQUIRE(out.size() == 3U);
    CHECK(out[0] == 0x41U); CHECK(out[1] == 0x42U); CHECK(out[2] == 0x43U);
}

TEST_CASE("deflate: inflate a REAL zlib DYNAMIC-Huffman stream (foreign encoder -> reads real .exr)", "[resources][deflate]")
{
    // Produced by CPython zlib.compress(payload, 6); the first block is dynamic-Huffman (btype=10). Validates the dynamic
    // path against a foreign encoder — the exact case a real OpenEXR ZIP file exercises.
    static const crd::u8 kComp[] = {
        120,156,237,213,183,17,96,53,20,64,81,185,47,243,164,34,212,202,122,239,96,171,128,136,140,161,122,102,78,11,100,204,38,55,61,225,77,233,197,139,27,163,190,124,121,35,231,87,175,110,204,246,250,245,141,82,222,188,185,177,210,219,183,55,106,125,247,238,70,228,247,239,111,180,246,225,195,141,93,62,126,188,241,164,79,159,110,156,250,249,243,141,158,191,124,185,145,218,215,175,55,70,249,246,237,70,78,223,191,223,152,245,199,143,27,37,255,246,219,141,213,126,255,253,70,45,63,127,222,8,110,227,110,238,195,61,220,206,77,220,193,205,220,201,45,220,197,173,220,224,54,238,230,62,220,195,237,220,196,29,220,204,157,220,194,93,220,202,13,110,227,110,238,195,61,220,206,77,220,193,205,220,201,45,220,197,173,220,224,54,238,230,62,220,195,237,220,196,29,220,204,157,220,194,93,220,202,13,110,227,110,238,195,61,220,206,77,220,193,205,220,201,45,220,197,173,220,224,54,238,230,62,220,195,237,220,196,29,220,204,157,220,194,93,220,202,13,110,227,110,238,195,61,220,206,77,220,193,205,220,201,45,220,197,173,220,224,54,238,230,62,220,195,237,220,196,29,220,204,157,220,194,93,220,202,13,110,227,110,238,195,61,220,206,77,220,193,205,220,201,45,220,197,173,220,224,54,238,230,62,220,195,237,220,196,29,220,204,157,220,194,93,220,202,13,110,227,110,238,195,61,220,206,77,220,193,205,220,201,45,220,197,173,220,224,54,238,230,62,220,195,237,220,196,29,220,204,157,220,194,93,220,202,13,110,227,110,238,195,61,220,206,77,220,193,205,220,201,45,220,197,173,220,224,54,238,230,62,220,195,237,220,196,29,220,204,157,220,194,93,220,202,13,110,227,110,238,195,61,220,206,77,220,193,205,220,201,45,220,197,173,220,224,54,238,230,62,220,195,237,220,196,29,220,204,157,220,194,93,220,202,13,110,227,110,238,195,61,220,206,77,220,193,205,220,201,45,220,197,173,220,224,54,238,230,62,220,195,237,220,196,29,220,204,157,220,194,93,220,202,13,110,227,110,238,195,61,220,206,77,191,190,240,235,11,191,190,240,159,191,240,207,95,127,255,241,231,255,32,255,2,68,49,143,187};
    Array<crd::u8> out(&s_df);
    REQUIRE(zlib_inflate({kComp, sizeof(kComp)}, out));
    REQUIRE(out.size() == 2600U);
    crd::u64 sum = 0; for (crd::usize i = 0; i < out.size(); ++i) { sum += out[i]; }
    CHECK(sum == 102315U);
    const crd::u8 first16[16] = {0, 0, 65, 65, 32, 10, 7, 3, 66, 66, 32, 10, 1, 1, 67, 67};
    const crd::u8 last16[16]  = {107, 122, 113, 120, 106, 107, 122, 113, 120, 106, 107, 122, 113, 120, 106, 107};
    int bad = 0;
    for (int i = 0; i < 16; ++i) { if (out[static_cast<crd::usize>(i)] != first16[i]) { ++bad; } if (out[out.size() - 16U + static_cast<crd::usize>(i)] != last16[i]) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("deflate: deflate->inflate round-trip (raw + zlib) on varied inputs", "[resources][deflate]")
{
    // empty
    { Array<crd::u8> e(&s_df); roundtrip_raw(e, "empty"); roundtrip_zlib(e, "empty"); }
    // short literal
    { Array<crd::u8> s(&s_df); const char* t = "hello, deflate"; for (const char* p = t; *p; ++p) { s.push_back(static_cast<crd::u8>(*p)); } roundtrip_raw(s, "hello"); roundtrip_zlib(s, "hello"); }
    // highly repetitive (LZ77 long matches + overlap)
    { Array<crd::u8> s(&s_df); for (int i = 0; i < 5000; ++i) { s.push_back(static_cast<crd::u8>('a' + (i % 3))); } roundtrip_raw(s, "abc-rep"); roundtrip_zlib(s, "abc-rep"); }
    // all-constant (distance-1 overlap runs)
    { Array<crd::u8> s(&s_df); for (int i = 0; i < 3000; ++i) { s.push_back(0x5AU); } roundtrip_raw(s, "const"); roundtrip_zlib(s, "const"); }
    // structured binary (like predicted EXR bytes)
    { Array<crd::u8> s(&s_df); crd::u32 st = 12345U; for (int i = 0; i < 4096; ++i) { st = st * 1103515245U + 12345U; s.push_back(static_cast<crd::u8>((st >> 16U) & (i % 7 == 0 ? 0xFFU : 0x0FU))); } roundtrip_raw(s, "structured"); roundtrip_zlib(s, "structured"); }
    // full byte range
    { Array<crd::u8> s(&s_df); for (int i = 0; i < 256; ++i) { s.push_back(static_cast<crd::u8>(i)); } roundtrip_raw(s, "range256"); roundtrip_zlib(s, "range256"); }
}

TEST_CASE("deflate: zlib output actually compresses a repetitive input", "[resources][deflate]")
{
    Array<crd::u8> s(&s_df); for (int i = 0; i < 4000; ++i) { s.push_back(static_cast<crd::u8>('x')); }
    Array<crd::u8> comp = zlib_deflate({s.data(), s.size()}, &s_df);
    CHECK(comp.size() < s.size() / 4U);
}
