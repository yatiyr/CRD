#include <catch2/catch_test_macros.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <cstring>
#include <iterator>
#include <new>
#include <string_view>

using namespace crd::resources;

alignas(crd::memory::GrowableTlsfAllocator) static unsigned char s_alloc_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
static crd::memory::GrowableTlsfAllocator& s_alloc = *::new (s_alloc_buf) crd::memory::GrowableTlsfAllocator(); // never destroyed: static-destruction-order safe

// ── Reader validation ─────────────────────────────────────────────────────

TEST_CASE("crdr_read rejects empty span", "[resources][crdr]")
{
    CrdrFile file(&s_alloc);
    const CrdrError err = crdr_read({}, file, &s_alloc);
    CHECK(err == CrdrError::Truncated);
}

TEST_CASE("crdr_read rejects bad magic", "[resources][crdr]")
{
    crd::u8 buf[40] = {};
    buf[0] = 'X'; buf[1] = 'X'; buf[2] = 'X'; buf[3] = 'X'; // bad magic
    buf[4] = 0x03; buf[5] = 0x00; // version 3

    CrdrFile file(&s_alloc);
    const CrdrError err = crdr_read({buf, 40}, file, &s_alloc);
    CHECK(err == CrdrError::BadMagic);
}

TEST_CASE("crdr_read rejects wrong version", "[resources][crdr]")
{
    crd::u8 buf[40] = {};
    // Correct magic: 'CRDR' in LE = 0x52445243
    buf[0] = 'C'; buf[1] = 'R'; buf[2] = 'D'; buf[3] = 'R';
    buf[4] = 0x02; buf[5] = 0x00; // version 2 (the DX12 device-removal bundle's, never a container's)

    CrdrFile file(&s_alloc);
    const CrdrError err = crdr_read({buf, 40}, file, &s_alloc);
    CHECK(err == CrdrError::BadVersion);
}

TEST_CASE("crdr_read rejects truncated header", "[resources][crdr]")
{
    crd::u8 buf[16] = {}; // only 16 bytes, header is 40
    CrdrFile file(&s_alloc);
    const CrdrError err = crdr_read({buf, 16}, file, &s_alloc);
    CHECK(err == CrdrError::Truncated);
}

// ── Round-trip ─────────────────────────────────────────────────────────────

TEST_CASE("crdr round-trip: empty container", "[resources][crdr]")
{
    const ResourceId id = ResourceId::mint_random();
    CrdrWriter writer(&s_alloc, id, kFourCC_BLOB);
    const auto blob = writer.finish();

    CrdrFile file(&s_alloc);
    const CrdrError err = crdr_read(crd::containers::as_const_span(blob), file, &s_alloc);
    REQUIRE(err == CrdrError::Ok);
    CHECK(file.id == id);
    CHECK(file.type_fourcc == kFourCC_BLOB);
    CHECK(file.version == kCrdrVersion);
    CHECK(file.chunks.size() == 0U);
}

TEST_CASE("crdr round-trip: single BLOB chunk", "[resources][crdr]")
{
    const ResourceId id = ResourceId::mint_random();
    CrdrWriter writer(&s_alloc, id, kFourCC_BLOB);

    const crd::u8 payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    writer.add_chunk(kFourCC_BLOB, {payload, sizeof(payload)});
    const auto blob = writer.finish();

    CrdrFile file(&s_alloc);
    REQUIRE(crdr_read(crd::containers::as_const_span(blob), file, &s_alloc) == CrdrError::Ok);
    REQUIRE(file.chunks.size() == 1U);
    CHECK(file.chunks[0].fourcc == kFourCC_BLOB);
    CHECK(file.chunks[0].payload.size() == sizeof(payload));
    for (crd::usize i = 0; i < sizeof(payload); ++i)
    {
        CHECK(file.chunks[0].payload[i] == payload[i]);
    }
}

TEST_CASE("crdr round-trip: multiple chunks sorted by fourcc", "[resources][crdr]")
{
    const ResourceId id = ResourceId::mint_random();
    CrdrWriter writer(&s_alloc, id, kFourCC_PACK);

    // Add in reverse-alphabetical order; expect sorted output.
    const crd::u8 strp_data[] = {'h', 'i', '\0'};
    const crd::u8 mfst_data[] = {0x01, 0x02};
    writer.add_chunk(kFourCC_STRP, {strp_data, sizeof(strp_data)});
    writer.add_chunk(kFourCC_MFST, {mfst_data, sizeof(mfst_data)});

    const auto blob = writer.finish();

    CrdrFile file(&s_alloc);
    REQUIRE(crdr_read(crd::containers::as_const_span(blob), file, &s_alloc) == CrdrError::Ok);
    REQUIRE(file.chunks.size() == 2U);
    // MFST (0x5453464D) < STRP (0x50525453)? Let's check FourCC ordering.
    // make_fourcc('M','F','S','T') = 'M'|(F<<8)|(S<<16)|(T<<24) = 0x5453464D
    // make_fourcc('S','T','R','P') = 'S'|(T<<8)|(R<<16)|(P<<24) = 0x50525453
    // 0x50525453 < 0x5453464D → STRP < MFST after sorting ascending
    CHECK(file.chunks[0].fourcc == kFourCC_STRP);
    CHECK(file.chunks[1].fourcc == kFourCC_MFST);
}

TEST_CASE("crdr chunk payload 16-byte padding is transparent to reader", "[resources][crdr]")
{
    // A 3-byte payload should be stored with 13 bytes of padding; reader gives back 3.
    const ResourceId id = ResourceId::mint_random();
    CrdrWriter writer(&s_alloc, id, kFourCC_BLOB);

    const crd::u8 data[] = {0xAA, 0xBB, 0xCC};
    writer.add_chunk(kFourCC_BLOB, {data, sizeof(data)});
    const auto blob = writer.finish();

    CrdrFile file(&s_alloc);
    REQUIRE(crdr_read(crd::containers::as_const_span(blob), file, &s_alloc) == CrdrError::Ok);
    REQUIRE(file.chunks.size() == 1U);
    REQUIRE(file.chunks[0].payload.size() == 3U);
    CHECK(file.chunks[0].payload[0] == 0xAA);
    CHECK(file.chunks[0].payload[1] == 0xBB);
    CHECK(file.chunks[0].payload[2] == 0xCC);
}

// ── Manifest round-trip with 100 fake entries ───────────────────────────────

TEST_CASE("manifest write + read round-trip: 100 entries", "[resources][crdr][manifest]")
{
    const crd::usize count = 100U;

    // Build a string pool and entry array.
    crd::containers::Array<crd::u8> string_pool(&s_alloc);
    crd::containers::Array<ManifestEntry> entries_in(&s_alloc);

    for (crd::usize i = 0U; i < count; ++i)
    {
        // Record string pool offset.
        const crd::u32 name_offset = static_cast<crd::u32>(string_pool.size());

        // Append a fake name "asset_NNN\0".
        char name_buf[16];
        (void)std::snprintf(name_buf, sizeof(name_buf), "asset_%03zu", i);
        for (const char* p = name_buf; *p != '\0'; ++p)
        {
            string_pool.push_back(static_cast<crd::u8>(*p));
        }
        string_pool.push_back(0U); // null terminator

        ManifestEntry e;
        e.id            = ResourceId::mint_random();
        e.type_fourcc   = kFourCC_BLOB;
        e.flags         = 0U;
        e.blob_offset   = i * 4096U;
        e.blob_size     = 1024U;
        e.name_strp_idx = name_offset;
        entries_in.push_back(e);
    }

    // Write PACK file.
    const ResourceId pack_id = ResourceId::mint_random();
    CrdrWriter writer(&s_alloc, pack_id, kFourCC_PACK);
    manifest_write(
        writer,
        crd::containers::as_const_span(entries_in),
        crd::containers::as_const_span(string_pool));
    const auto pack_blob = writer.finish();

    // Read back.
    CrdrFile file(&s_alloc);
    REQUIRE(crdr_read(crd::containers::as_const_span(pack_blob), file, &s_alloc) == CrdrError::Ok);
    CHECK(file.type_fourcc == kFourCC_PACK);

    const CrdrChunk* mfst = crdr_find_chunk(file, kFourCC_MFST);
    REQUIRE(mfst != nullptr);

    crd::containers::Array<ManifestEntry> entries_out(&s_alloc);
    REQUIRE(manifest_read_entries(mfst->payload, entries_out, &s_alloc));
    REQUIRE(entries_out.size() == count);

    for (crd::usize i = 0U; i < count; ++i)
    {
        CHECK(entries_out[i].id          == entries_in[i].id);
        CHECK(entries_out[i].type_fourcc == entries_in[i].type_fourcc);
        CHECK(entries_out[i].blob_offset == entries_in[i].blob_offset);
        CHECK(entries_out[i].blob_size   == entries_in[i].blob_size);
        CHECK(entries_out[i].name_strp_idx == entries_in[i].name_strp_idx);
    }
}

// ── crdr_find_chunk ───────────────────────────────────────────────────────

TEST_CASE("crdr_find_chunk returns nullptr for missing chunk", "[resources][crdr]")
{
    const ResourceId id = ResourceId::mint_random();
    CrdrWriter writer(&s_alloc, id, kFourCC_BLOB);
    const auto blob = writer.finish();

    CrdrFile file(&s_alloc);
    REQUIRE(crdr_read(crd::containers::as_const_span(blob), file, &s_alloc) == CrdrError::Ok);
    CHECK(crdr_find_chunk(file, kFourCC_MFST) == nullptr);
}

TEST_CASE("crdr_find_chunk finds the right chunk", "[resources][crdr]")
{
    const ResourceId id = ResourceId::mint_random();
    CrdrWriter writer(&s_alloc, id, kFourCC_BLOB);
    const crd::u8 payload[] = {0x42};
    writer.add_chunk(kFourCC_META, {payload, 1});
    const auto blob = writer.finish();

    CrdrFile file(&s_alloc);
    REQUIRE(crdr_read(crd::containers::as_const_span(blob), file, &s_alloc) == CrdrError::Ok);
    const CrdrChunk* meta = crdr_find_chunk(file, kFourCC_META);
    REQUIRE(meta != nullptr);
    REQUIRE(meta->payload.size() == 1U);
    CHECK(meta->payload[0] == 0x42);
}

// ── Integrity (HW-RESILIENCE.1) ─────────────────────────────────────────────

namespace
{

crd::u32 load_u32(const crd::u8* p)
{
    crd::u32 v = 0U;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

crd::u64 load_u64(const crd::u8* p)
{
    crd::u64 v = 0U;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void store_u32(crd::u8* p, crd::u32 v)
{
    std::memcpy(p, &v, sizeof(v));
}

// A container with an uncompressed 5-byte META chunk (11 bytes of padding) and a compressed VERT chunk (2 KiB that zstd
// shrinks), in that order: the writer sorts chunks by fourcc.
crd::containers::Array<crd::u8> two_chunk_container()
{
    crd::u8 big[2048];
    for (crd::u32 i = 0U; i < sizeof(big); ++i)
    {
        big[i] = static_cast<crd::u8>((i % 7U) * 13U);
    }
    const crd::u8 small[5] = {1U, 2U, 3U, 4U, 5U};
    CrdrWriter writer(&s_alloc, ResourceId::mint_random(), kFourCC_BLOB);
    writer.add_chunk_compressed(kFourCC_VERT, {big, sizeof(big)});
    writer.add_chunk(kFourCC_META, {small, sizeof(small)});
    return writer.finish();
}

// One chunk's place in a container: its header at `header`, its stored bytes after it, then padding to `end`.
struct ChunkSpan
{
    crd::usize header;
    crd::usize stored;
    crd::usize stored_end;
    crd::usize end;
    crd::u32 fourcc;
};

crd::containers::Array<ChunkSpan> chunk_spans(const crd::containers::Array<crd::u8>& blob)
{
    crd::containers::Array<ChunkSpan> spans(&s_alloc);
    const crd::u32 count = load_u32(blob.data() + 28U);
    crd::usize at = 40U;
    for (crd::u32 i = 0U; i < count; ++i)
    {
        const crd::usize stored_size = static_cast<crd::usize>(load_u64(blob.data() + at + 16U));
        const crd::usize padded = (stored_size + 15U) & ~static_cast<crd::usize>(15U);
        spans.push_back(ChunkSpan{at, at + 40U, at + 40U + stored_size, at + 40U + padded, load_u32(blob.data() + at)});
        at += 40U + padded;
    }
    return spans;
}

} // namespace

TEST_CASE("crdr integrity: every flipped bit outside the padding is a typed error naming its place",
          "[resources][crdr][hwres]")
{
    crd::containers::Array<crd::u8> blob = two_chunk_container();
    const crd::containers::Array<ChunkSpan> spans = chunk_spans(blob);
    REQUIRE(spans.size() == 2U);
    REQUIRE(spans[0].fourcc == kFourCC_META);
    REQUIRE(spans[1].fourcc == kFourCC_VERT);
    REQUIRE((load_u32(blob.data() + spans[1].header + 4U) & 1U) == 1U); // the VERT chunk is stored compressed
    REQUIRE(spans[1].end == blob.size());
    {
        CrdrFile clean(&s_alloc);
        REQUIRE(crdr_read(crd::containers::as_const_span(blob), clean, &s_alloc) == CrdrError::Ok);
    }

    crd::usize checked = 0U;
    for (crd::usize byte = 0U; byte < blob.size(); ++byte)
    {
        CrdrError expected = CrdrError::HeaderCorrupt;
        crd::u32 chunk = 0U;
        bool padding = false;
        if (byte < 4U)
        {
            expected = CrdrError::BadMagic;
        }
        else if (byte < 6U)
        {
            expected = CrdrError::BadVersion;
        }
        else if (byte >= 40U)
        {
            expected = CrdrError::ChunkCorrupt;
            for (crd::u32 i = 0U; i < spans.size(); ++i)
            {
                if (byte >= spans[i].header && byte < spans[i].end)
                {
                    chunk = i;
                    padding = byte >= spans[i].stored_end;
                }
            }
        }
        for (crd::u32 bit = 0U; bit < 8U; ++bit)
        {
            blob[byte] = static_cast<crd::u8>(blob[byte] ^ (1U << bit));
            CrdrFile file(&s_alloc);
            const CrdrError err = crdr_read(crd::containers::as_const_span(blob), file, &s_alloc);
            blob[byte] = static_cast<crd::u8>(blob[byte] ^ (1U << bit));
            INFO("byte " << byte << " bit " << bit);
            if (padding)
            {
                CHECK(err == CrdrError::Ok); // padding is not data; the reader never looks at it
                continue;
            }
            CHECK(err == expected);
            if (expected == CrdrError::ChunkCorrupt)
            {
                CHECK(file.fault_chunk == chunk);
                if (byte >= spans[chunk].stored)
                {
                    CHECK(file.fault_fourcc == spans[chunk].fourcc);
                }
            }
            ++checked;
        }
    }
    CHECK(checked == (blob.size() - (spans[0].end - spans[0].stored_end) - (spans[1].end - spans[1].stored_end)) * 8U);
}

TEST_CASE("crdr integrity: a compressed chunk that decodes to other bytes is DecodedCorrupt",
          "[resources][crdr][hwres]")
{
    // Bytes that pass every check of the read but decode to bytes other than the ones written: the decoded hash is
    // changed and the chunk header's checksum recomputed, as a fault in the decoder's output would look.
    crd::containers::Array<crd::u8> blob = two_chunk_container();
    const crd::containers::Array<ChunkSpan> spans = chunk_spans(blob);
    REQUIRE(spans[1].fourcc == kFourCC_VERT);
    crd::u8* const vert = blob.data() + spans[1].header;
    vert[24] = static_cast<crd::u8>(vert[24] ^ 0x01U);
    store_u32(vert + 36U, crd::containers::crc32c(crd::containers::ConstSpan<crd::u8>(vert, 36U)));

    CrdrFile file(&s_alloc);
    CHECK(crdr_read(crd::containers::as_const_span(blob), file, &s_alloc) == CrdrError::DecodedCorrupt);
    CHECK(file.fault_chunk == 1U);
    CHECK(file.fault_fourcc == kFourCC_VERT);
}

TEST_CASE("crdr integrity: an uncompressed chunk that claims a decoded hash or another size is invalid",
          "[resources][crdr][hwres]")
{
    for (const crd::usize field : {crd::usize{8U}, crd::usize{24U}})
    {
        crd::containers::Array<crd::u8> blob = two_chunk_container();
        const crd::containers::Array<ChunkSpan> spans = chunk_spans(blob);
        REQUIRE(spans[0].fourcc == kFourCC_META);
        crd::u8* const meta = blob.data() + spans[0].header;
        meta[field] = static_cast<crd::u8>(meta[field] ^ 0x01U);
        store_u32(meta + 36U, crd::containers::crc32c(crd::containers::ConstSpan<crd::u8>(meta, 36U)));
        CrdrFile file(&s_alloc);
        INFO("field +" << field);
        CHECK(crdr_read(crd::containers::as_const_span(blob), file, &s_alloc) == CrdrError::InvalidChunk);
        CHECK(file.fault_fourcc == kFourCC_META);
    }
}

TEST_CASE("crdr integrity: a container of another version with a valid checksum is BadVersion",
          "[resources][crdr][hwres]")
{
    crd::containers::Array<crd::u8> blob = two_chunk_container();
    blob[4] = 1U; // an old container's version
    store_u32(blob.data() + 36U, crd::containers::crc32c(crd::containers::ConstSpan<crd::u8>(blob.data(), 36U)));
    CrdrFile file(&s_alloc);
    CHECK(crdr_read(crd::containers::as_const_span(blob), file, &s_alloc) == CrdrError::BadVersion);
}

TEST_CASE("crdr integrity: a fault scope keeps the first failure on its thread, and an inner scope its own",
          "[resources][crdr][hwres]")
{
    crd::containers::Array<crd::u8> good = two_chunk_container();
    crd::containers::Array<crd::u8> bad = two_chunk_container();
    const crd::usize at = chunk_spans(bad)[0].stored; // META's stored bytes
    bad[at] = static_cast<crd::u8>(bad[at] ^ 0x80U);
    const auto read = [](const crd::containers::Array<crd::u8>& bytes)
    {
        CrdrFile file(&s_alloc);
        return crdr_read(crd::containers::as_const_span(bytes), file, &s_alloc);
    };

    REQUIRE(read(bad) == CrdrError::ChunkCorrupt); // no scope open: nothing is recorded anywhere
    CrdrFaultScope outer;
    CHECK(outer.error() == CrdrError::Ok);
    CHECK(read(good) == CrdrError::Ok);
    CHECK(outer.error() == CrdrError::Ok);
    {
        CrdrFaultScope inner;
        CHECK(read(bad) == CrdrError::ChunkCorrupt);
        CHECK(inner.error() == CrdrError::ChunkCorrupt);
        CHECK(inner.fault_fourcc() == kFourCC_META);
    }
    CHECK(outer.error() == CrdrError::Ok); // the inner scope took it
    const crd::u8 empty[8] = {};
    CrdrFile scratch(&s_alloc);
    CHECK(read(bad) == CrdrError::ChunkCorrupt);
    CHECK(crdr_read({empty, sizeof(empty)}, scratch, &s_alloc) == CrdrError::Truncated);
    CHECK(outer.error() == CrdrError::ChunkCorrupt); // the first failure stays
    CHECK(outer.fault_fourcc() == kFourCC_META);
}

TEST_CASE("crdr integrity: every error has its own text", "[resources][crdr][hwres]")
{
    const CrdrError all[] = {CrdrError::Ok,
                             CrdrError::BadMagic,
                             CrdrError::BadVersion,
                             CrdrError::Truncated,
                             CrdrError::InvalidChunk,
                             CrdrError::DecompressFailed,
                             CrdrError::HeaderCorrupt,
                             CrdrError::ChunkCorrupt,
                             CrdrError::DecodedCorrupt};
    for (crd::usize i = 0U; i < std::size(all); ++i)
    {
        const std::string_view text = crdr_error_text(all[i]);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown CRDR error");
        for (crd::usize j = 0U; j < i; ++j)
        {
            CHECK(text != std::string_view{crdr_error_text(all[j])});
        }
    }
}
