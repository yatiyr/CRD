#include <catch2/catch_test_macros.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <new>

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
    crd::u8 buf[32] = {};
    buf[0] = 'X'; buf[1] = 'X'; buf[2] = 'X'; buf[3] = 'X'; // bad magic
    buf[4] = 0x01; buf[5] = 0x00; // version 1

    CrdrFile file(&s_alloc);
    const CrdrError err = crdr_read({buf, 32}, file, &s_alloc);
    CHECK(err == CrdrError::BadMagic);
}

TEST_CASE("crdr_read rejects wrong version", "[resources][crdr]")
{
    crd::u8 buf[32] = {};
    // Correct magic: 'CRDR' in LE = 0x52445243
    buf[0] = 'C'; buf[1] = 'R'; buf[2] = 'D'; buf[3] = 'R';
    buf[4] = 0x02; buf[5] = 0x00; // version 2 (not supported)

    CrdrFile file(&s_alloc);
    const CrdrError err = crdr_read({buf, 32}, file, &s_alloc);
    CHECK(err == CrdrError::BadVersion);
}

TEST_CASE("crdr_read rejects truncated header", "[resources][crdr]")
{
    crd::u8 buf[16] = {}; // only 16 bytes, header is 32
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
    CHECK(file.version == 1U);
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
