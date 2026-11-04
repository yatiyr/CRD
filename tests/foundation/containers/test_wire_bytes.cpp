// crd-containers -- big-endian integers over byte spans (wire_bytes.hpp): network byte order on every host, exact
// round trips at the bounds, refused short reads that consume nothing, and input-viewing byte ranges.

#include <crd/containers/wire_bytes.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::wire_load;
using crd::containers::wire_store;
using crd::containers::WireReader;
using crd::containers::WireWriter;

TEST_CASE("Wire bytes: integers are written most significant byte first", "[containers][wire-bytes]")
{
    u8         out[16] = {};
    WireWriter writer(Span<u8>(out, sizeof(out)));
    writer.put_u8(0x01U);
    writer.put_u16(0x0203U);
    writer.put_u32(0x04050607U);
    writer.put_u64(0x08090A0B0C0D0E0FULL);
    const u8 tail[] = {0x10U};
    writer.put_bytes(ConstSpan<u8>(tail));
    REQUIRE(writer.written() == 16U);
    for (usize i = 0U; i < sizeof(out); ++i)
    {
        CHECK(out[i] == i + 1U);
    }
}

TEST_CASE("Wire bytes: every width round-trips at its bounds", "[containers][wire-bytes]")
{
    u8         bytes[2U * (1U + 2U + 4U + 8U)] = {};
    WireWriter writer(Span<u8>(bytes, sizeof(bytes)));
    writer.put_u8(0U);
    writer.put_u16(0U);
    writer.put_u32(0U);
    writer.put_u64(0U);
    writer.put_u8(0xFFU);
    writer.put_u16(0xFFFFU);
    writer.put_u32(0xFFFFFFFFU);
    writer.put_u64(~u64{0U});
    REQUIRE(writer.written() == sizeof(bytes));

    WireReader reader(ConstSpan<u8>(bytes, sizeof(bytes)));
    u8         a = 1U;
    u16        b = 1U;
    u32        c = 1U;
    u64        d = 1U;
    REQUIRE((reader.take_u8(a) && reader.take_u16(b) && reader.take_u32(c) && reader.take_u64(d)));
    CHECK((a == 0U && b == 0U && c == 0U && d == 0U));
    REQUIRE((reader.take_u8(a) && reader.take_u16(b) && reader.take_u32(c) && reader.take_u64(d)));
    CHECK((a == 0xFFU && b == 0xFFFFU && c == 0xFFFFFFFFU && d == ~u64{0U}));
    CHECK(reader.done());
    CHECK(reader.consumed() == sizeof(bytes));
}

TEST_CASE("Wire bytes: a field at a fixed offset is read and patched in place", "[containers][wire-bytes]")
{
    u8 bytes[15] = {};
    wire_store<u8>(Span<u8>(bytes), 0U, u8{0x01U});
    wire_store<u16>(Span<u8>(bytes), 1U, u16{0x0203U});
    wire_store<u32>(Span<u8>(bytes), 3U, 0x04050607U);
    wire_store<u64>(Span<u8>(bytes), 7U, 0x08090A0B0C0D0E0FULL);
    for (usize i = 0U; i < sizeof(bytes); ++i)
    {
        CHECK(bytes[i] == i + 1U);
    }
    const ConstSpan<u8> view(bytes);
    CHECK(wire_load<u8>(view, 0U) == 0x01U);
    CHECK(wire_load<u16>(view, 1U) == 0x0203U);
    CHECK(wire_load<u32>(view, 3U) == 0x04050607U);
    CHECK(wire_load<u64>(view, 7U) == 0x08090A0B0C0D0E0FULL);
    CHECK(wire_load<u16>(view, 13U) == 0x0E0FU); // the last field the span holds

    wire_store<u32>(Span<u8>(bytes), 11U, 0xFFFFFFFFU); // a patch touches only its own bytes
    CHECK((bytes[10] == 11U && bytes[11] == 0xFFU && bytes[14] == 0xFFU));
    CHECK(wire_load<u32>(view, 11U) == 0xFFFFFFFFU);
}

TEST_CASE("Wire bytes: a short read is refused and a byte range views the input", "[containers][wire-bytes]")
{
    const u8   bytes[] = {0xAAU, 0xBBU, 0xCCU};
    WireReader reader(ConstSpan<u8>(bytes, sizeof(bytes)));
    u32        wide = 7U;
    CHECK_FALSE(reader.take_u32(wide));
    CHECK(wide == 7U);
    CHECK(reader.consumed() == 0U);
    u16 narrow = 0U;
    REQUIRE(reader.take_u16(narrow));
    CHECK(narrow == 0xAABBU);
    CHECK_FALSE(reader.take_u16(narrow));
    CHECK(reader.consumed() == 2U); // the refused take left the last byte

    WireReader          fresh(ConstSpan<u8>(bytes, sizeof(bytes)));
    ConstSpan<u8>       range;
    CHECK_FALSE(fresh.take_bytes(4U, range));
    CHECK(fresh.consumed() == 0U);
    CHECK((fresh.rest().data() == bytes && fresh.rest().size() == 3U));
    REQUIRE(fresh.take_bytes(2U, range));
    CHECK(range.data() == bytes);
    CHECK(range.size() == 2U);
    CHECK_FALSE(fresh.done());
    CHECK((fresh.rest().data() == bytes + 2U && fresh.rest().size() == 1U));
    u16 half = 0U;
    CHECK_FALSE(fresh.take_u16(half));
    REQUIRE(fresh.take_bytes(1U, range));
    CHECK(range.data() == bytes + 2U);
    CHECK(fresh.done());
    CHECK(fresh.rest().empty());
    u8 none = 0U;
    CHECK_FALSE(fresh.take_u8(none));
    CHECK(fresh.take_bytes(0U, range));
    CHECK(range.empty());
}
