#include <catch2/catch_test_macros.hpp>
#include <crd/resources/resource_id.hpp>

#include <crd/containers/hash_set.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <new>

using crd::resources::ResourceId;
using crd::resources::kNullResourceId;

// ── Helpers ───────────────────────────────────────────────────────────────

alignas(crd::memory::GrowableTlsfAllocator) static unsigned char s_alloc_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
static crd::memory::GrowableTlsfAllocator& s_alloc = *::new (s_alloc_buf) crd::memory::GrowableTlsfAllocator(); // never destroyed: static-destruction-order safe

// ── mint_random ───────────────────────────────────────────────────────────

TEST_CASE("ResourceId mint_random produces non-null ids", "[resources][resource_id]")
{
    const ResourceId id = ResourceId::mint_random();
    CHECK_FALSE(id.is_null());
}

TEST_CASE("ResourceId mint_random produces unique ids (10 000 mints)", "[resources][resource_id]")
{
    crd::containers::HashSet<ResourceId> seen(&s_alloc);
    for (int i = 0; i < 10'000; ++i)
    {
        const ResourceId id = ResourceId::mint_random();
        REQUIRE_FALSE(seen.contains(id));
        seen.insert(id);
    }
}

TEST_CASE("ResourceId mint_random sets UUID v4 version and variant bits", "[resources][resource_id]")
{
    for (int i = 0; i < 100; ++i)
    {
        const ResourceId id = ResourceId::mint_random();

        // byte[6] high nibble must be 4 (version 4)
        // byte[6] lives at bits 15..8 of hi; high nibble = bits 15..12
        const crd::u8 byte6_high_nibble =
            static_cast<crd::u8>((id.hi >> 12U) & 0x0FU);
        CHECK(byte6_high_nibble == 4U);

        // byte[8] high 2 bits must be 0b10 (RFC 4122 variant)
        // byte[8] is the MSByte of lo → lo bits 63..62
        const crd::u8 byte8_high2 =
            static_cast<crd::u8>((id.lo >> 62U) & 0x03U);
        CHECK(byte8_high2 == 2U);
    }
}

// ── from_content ──────────────────────────────────────────────────────────

TEST_CASE("ResourceId from_content is deterministic (same bytes -> same id)", "[resources][resource_id]")
{
    const crd::u8 data[] = {0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB};
    const auto span = crd::containers::ConstSpan<crd::u8>(data, sizeof(data));

    const ResourceId a = ResourceId::from_content(span);
    const ResourceId b = ResourceId::from_content(span);
    CHECK(a == b);
    CHECK_FALSE(a.is_null());
}

TEST_CASE("ResourceId from_content differs for different bytes", "[resources][resource_id]")
{
    const crd::u8 data1[] = {0x01, 0x02, 0x03};
    const crd::u8 data2[] = {0x01, 0x02, 0x04};

    const ResourceId a = ResourceId::from_content({data1, sizeof(data1)});
    const ResourceId b = ResourceId::from_content({data2, sizeof(data2)});
    CHECK_FALSE(a == b);
}

TEST_CASE("ResourceId from_content sets UUID v5 version and variant bits", "[resources][resource_id]")
{
    const crd::u8 data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    const ResourceId id = ResourceId::from_content({data, sizeof(data)});

    // byte[6] high nibble must be 5 (version 5)
    const crd::u8 byte6_high_nibble =
        static_cast<crd::u8>((id.hi >> 12U) & 0x0FU);
    CHECK(byte6_high_nibble == 5U);

    // byte[8] high 2 bits must be 0b10
    const crd::u8 byte8_high2 =
        static_cast<crd::u8>((id.lo >> 62U) & 0x03U);
    CHECK(byte8_high2 == 2U);
}

TEST_CASE("ResourceId from_content empty span is stable", "[resources][resource_id]")
{
    const ResourceId a = ResourceId::from_content({});
    const ResourceId b = ResourceId::from_content({});
    CHECK(a == b);
    CHECK_FALSE(a.is_null());
}

// ── parse / to_string ─────────────────────────────────────────────────────

TEST_CASE("ResourceId parse + to_string round-trip (v4 ids)", "[resources][resource_id]")
{
    for (int i = 0; i < 1000; ++i)
    {
        const ResourceId original = ResourceId::mint_random();
        const auto str = original.to_string(&s_alloc);
        const ResourceId parsed = ResourceId::parse(str);
        CHECK(parsed == original);
    }
}

TEST_CASE("ResourceId to_string produces 36-char hyphenated format", "[resources][resource_id]")
{
    const ResourceId id = ResourceId::mint_random();
    const auto str = id.to_string(&s_alloc);
    REQUIRE(str.size() == 36U);
    CHECK(str.data()[8]  == '-');
    CHECK(str.data()[13] == '-');
    CHECK(str.data()[18] == '-');
    CHECK(str.data()[23] == '-');
}

TEST_CASE("ResourceId parse rejects wrong length", "[resources][resource_id]")
{
    CHECK(ResourceId::parse("too-short").is_null());
    CHECK(ResourceId::parse("").is_null());
    CHECK(ResourceId::parse("00000000-0000-0000-0000-0000000000000").is_null()); // 37 chars
}

TEST_CASE("ResourceId parse rejects wrong dash positions", "[resources][resource_id]")
{
    CHECK(ResourceId::parse("0000000x-0000-0000-0000-000000000000").is_null());
}

TEST_CASE("ResourceId parse rejects non-hex characters", "[resources][resource_id]")
{
    CHECK(ResourceId::parse("zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz").is_null());
}

TEST_CASE("ResourceId parse known value is stable", "[resources][resource_id]")
{
    const ResourceId id = ResourceId::parse("550e8400-e29b-41d4-a716-446655440000");
    CHECK_FALSE(id.is_null());
    const auto back = id.to_string(&s_alloc);
    CHECK(back == "550e8400-e29b-41d4-a716-446655440000");
}

// ── null / equality ───────────────────────────────────────────────────────

TEST_CASE("kNullResourceId is_null", "[resources][resource_id]")
{
    CHECK(kNullResourceId.is_null());
    CHECK(kNullResourceId == kNullResourceId);
}

TEST_CASE("ResourceId equality is reflexive", "[resources][resource_id]")
{
    const ResourceId id = ResourceId::mint_random();
    CHECK(id == id);
}

TEST_CASE("ResourceId copy equality", "[resources][resource_id]")
{
    const ResourceId a = ResourceId::mint_random();
    const ResourceId b = a;
    CHECK(a == b);
}
