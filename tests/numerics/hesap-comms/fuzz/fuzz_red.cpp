// crd-fuzz-hesap-red: hesap-comms' redundant audio data (RFC 2198; HESAP-NET; crd/hesap/comms/red.hpp) under the
// bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data.
//   0  a RED payload: the room for blocks, a timestamp, then the payload's bytes;
//   1  a packet to write: up to five redundant blocks (payload type, offset and length, any of them past its field),
//      the primary's type and length, the room the output is short of or past.
//
// Oracles, none of them the codec's own verdict:
//   * a reader restated bit by bit from §3's two header figures decides every parse's status, count, fields,
//     timestamps and the bytes each block views, and a refused parse leaves the blocks as they were;
//   * §3's field widths and the time-interval rule decide every write's status, and a refused write writes nothing;
//   * an accepted write equals the payload built here bit by bit, writes nothing past it, and reads back to its
//     blocks.
#include <crd/fuzz/harness.hpp>
#include <crd/hesap/comms/red.hpp>

#include <cstring>
#include <initializer_list>

namespace
{
using crd::u16;
using crd::u32;
using crd::u8;
using crd::usize;
using namespace crd::hesap::comms;

constexpr u8 kSentinel = 0xA5U;
constexpr u32 kMaxRoom = 8U;
constexpr u32 kMaxRedundant = 5U;
constexpr usize kMaxOut = (kMaxRedundant * (4U + 1100U)) + 1U + 1600U + 256U; // room runs to need + 223

struct Reader
{
    const u8* data;
    usize size;

    u8 next()
    {
        if (size == 0U)
        {
            return 0U;
        }
        --size;
        return *data++;
    }

    u16 next16()
    {
        const u32 high = next();
        return static_cast<u16>((high << 8U) | next());
    }
};

u8 g_out[kMaxOut];
u8 g_expected[kMaxOut];
u8 g_data[kMaxOut];

// Bit `i` of `bytes`, bit 0 being the most significant bit of byte 0 (the RFC's figures).
u32 bit(const u8* bytes, usize i)
{
    return (bytes[i / 8U] >> (7U - (i % 8U))) & 1U;
}

u32 bits(const u8* bytes, usize first, usize count)
{
    u32 v = 0U;
    for (usize i = 0U; i < count; ++i)
    {
        v = (v << 1U) | bit(bytes, first + i);
    }
    return v;
}

void set_bits(u8* bytes, usize first, usize count, u32 value)
{
    for (usize i = 0U; i < count; ++i)
    {
        const u32 b = (value >> (count - 1U - i)) & 1U;
        const usize at = first + i;
        bytes[at / 8U] = static_cast<u8>((bytes[at / 8U] & ~(0x80U >> (at % 8U))) | (b << (7U - (at % 8U))));
    }
}

struct Restated
{
    u8 type;
    u32 offset;
    usize length;
};

// §3 read literally: headers until F = 0, each F = 1 header 32 bits, the last 8; then the data blocks in order.
RedStatus restated_parse(const u8* p, usize size, u32 room, Restated* out, u32& count)
{
    usize at = 0U;
    usize lengths = 0U;
    count = 0U;
    for (;;)
    {
        if (size - at < 1U)
        {
            return RedStatus::Truncated;
        }
        const u32 f = bit(p + at, 0U);
        if (f == 1U && size - at < 4U)
        {
            return RedStatus::Truncated;
        }
        if (count == room)
        {
            return RedStatus::NoSpace;
        }
        Restated& r = out[count++];
        r.type = static_cast<u8>(bits(p + at, 1U, 7U));
        if (f == 0U)
        {
            r.offset = 0U;
            ++at;
            break;
        }
        r.offset = bits(p + at, 8U, 14U);
        r.length = bits(p + at, 22U, 10U);
        lengths += r.length;
        at += 4U;
    }
    if (size - at < lengths)
    {
        return RedStatus::Truncated;
    }
    out[count - 1U].length = size - at - lengths;
    return RedStatus::Ok;
}

bool marked(const RedBlock& b)
{
    return b.payload_type == 0x55U && b.offset == 0x5555U && b.timestamp == 0x55555555U && b.data.empty();
}

void parse_request(Reader r)
{
    const u32 room = r.next() % (kMaxRoom + 1U);
    const u32 timestamp = (u32{r.next16()} << 16U) | r.next16();
    RedBlock blocks[kMaxRoom];
    for (RedBlock& b : blocks)
    {
        b.payload_type = 0x55U;
        b.offset = 0x5555U;
        b.timestamp = 0x55555555U;
    }
    Restated want[kMaxRoom];
    u32 want_count = 0U;
    const RedStatus want_status = restated_parse(r.data, r.size, room, want, want_count);
    u32 count = 99U;
    const RedStatus status = red_parse({r.data, r.size}, timestamp, {blocks, room}, count);
    CRD_FUZZ_REQUIRE(status == want_status);
    if (status != RedStatus::Ok)
    {
        CRD_FUZZ_REQUIRE(count == 99U);
        for (const RedBlock& b : blocks)
        {
            CRD_FUZZ_REQUIRE(marked(b));
        }
        return;
    }
    CRD_FUZZ_REQUIRE(count == want_count);
    const u8* data = r.data + (4U * (count - 1U)) + 1U;
    for (u32 i = 0U; i < count; ++i)
    {
        const RedBlock& b = blocks[i];
        CRD_FUZZ_REQUIRE(b.payload_type == want[i].type && b.offset == want[i].offset);
        CRD_FUZZ_REQUIRE(b.timestamp == static_cast<u32>(timestamp - want[i].offset));
        CRD_FUZZ_REQUIRE(b.data.data() == data && b.data.size() == want[i].length);
        data += want[i].length;
    }
    CRD_FUZZ_REQUIRE(data == r.data + r.size);
    for (u32 i = count; i < kMaxRoom; ++i)
    {
        CRD_FUZZ_REQUIRE(marked(blocks[i]));
    }
}

void encode_request(Reader r)
{
    const u32 levels = r.next() % (kMaxRedundant + 1U);
    RedBlock blocks[kMaxRedundant];
    u32 types[kMaxRedundant];
    usize used = 0U;
    for (u32 i = 0U; i < levels; ++i)
    {
        const u8 type = r.next();
        types[i] = type;
        const u16 offset = r.next16();
        const usize length = r.next16() % 1100U; // past 1023 now and then
        for (usize k = 0U; k < length; ++k)
        {
            g_data[used + k] = static_cast<u8>((used + k) * 131U);
        }
        blocks[i].payload_type = type;
        blocks[i].offset = offset;
        blocks[i].timestamp = r.next(); // ignored by the writer
        blocks[i].data = {g_data + used, length};
        used += length;
    }
    const u8 primary_type = r.next();
    const usize primary_length = r.next16() % 1600U;
    for (usize k = 0U; k < primary_length; ++k)
    {
        g_data[used + k] = static_cast<u8>((used + k) * 7U);
    }
    const u8* const primary = g_data + used;
    const u8 slack = r.next(); // 0..31: that much short; above: that much more than needed

    // §3's widths, then §3's "different time intervals" (no offset 0, no offset twice); block by block, then the
    // primary's type, then the room.
    RedStatus want = RedStatus::Ok;
    for (u32 i = 0U; i < levels && want == RedStatus::Ok; ++i)
    {
        if (types[i] >= 128U || blocks[i].offset >= 16384U || blocks[i].data.size() >= 1024U)
        {
            want = RedStatus::OutOfRange;
        }
        for (u32 j = 0U; j <= i && want == RedStatus::Ok; ++j)
        {
            if (blocks[i].offset == 0U || (j < i && blocks[j].offset == blocks[i].offset))
            {
                want = RedStatus::Repeated;
            }
        }
    }
    if (want == RedStatus::Ok && primary_type >= 128U)
    {
        want = RedStatus::OutOfRange;
    }
    usize need = 1U + primary_length;
    for (u32 i = 0U; i < levels; ++i)
    {
        need += 4U + blocks[i].data.size();
    }
    const usize short_by = slack < 32U ? slack % 4U + 1U : 0U;
    const usize room = short_by != 0U ? (need >= short_by ? need - short_by : 0U) : need + (slack - 32U);
    if (want == RedStatus::Ok && room < need)
    {
        want = RedStatus::NoSpace;
    }
    std::memset(g_out, kSentinel, sizeof(g_out));
    usize written = 12345U;
    const RedStatus status =
        red_encode({blocks, levels}, primary_type, {primary, primary_length}, {g_out, room}, written);
    CRD_FUZZ_REQUIRE(status == want);
    CRD_FUZZ_REQUIRE(red_bytes({blocks, levels}, primary_length) == need);
    if (status != RedStatus::Ok)
    {
        CRD_FUZZ_REQUIRE(written == 12345U);
        for (const u8 b : g_out)
        {
            CRD_FUZZ_REQUIRE(b == kSentinel);
        }
        return;
    }
    CRD_FUZZ_REQUIRE(written == need);
    std::memset(g_expected, 0, need);
    usize at = 0U;
    for (u32 i = 0U; i < levels; ++i)
    {
        set_bits(g_expected + at, 0U, 1U, 1U);
        set_bits(g_expected + at, 1U, 7U, types[i]);
        set_bits(g_expected + at, 8U, 14U, blocks[i].offset);
        set_bits(g_expected + at, 22U, 10U, static_cast<u32>(blocks[i].data.size()));
        at += 4U;
    }
    set_bits(g_expected + at, 0U, 8U, primary_type); // F = 0 and the type
    ++at;
    for (u32 i = 0U; i < levels; ++i)
    {
        std::memcpy(g_expected + at, blocks[i].data.data(), blocks[i].data.size());
        at += blocks[i].data.size();
    }
    if (primary_length != 0U)
    {
        std::memcpy(g_expected + at, primary, primary_length);
    }
    CRD_FUZZ_REQUIRE(std::memcmp(g_out, g_expected, need) == 0);
    for (usize i = need; i < sizeof(g_out); ++i)
    {
        CRD_FUZZ_REQUIRE(g_out[i] == kSentinel);
    }

    RedBlock read[kMaxRedundant + 1U];
    u32 count = 0U;
    CRD_FUZZ_REQUIRE(red_parse({g_out, written}, 0x01020304U, read, count) == RedStatus::Ok);
    CRD_FUZZ_REQUIRE(count == levels + 1U);
    for (u32 i = 0U; i < levels; ++i)
    {
        CRD_FUZZ_REQUIRE(read[i].payload_type == types[i] && read[i].offset == blocks[i].offset);
        CRD_FUZZ_REQUIRE(read[i].timestamp == 0x01020304U - blocks[i].offset);
        CRD_FUZZ_REQUIRE(crd::fuzz::bytes_equal(read[i].data.data(), read[i].data.size(), blocks[i].data.data(),
                                                blocks[i].data.size()));
    }
    CRD_FUZZ_REQUIRE(read[levels].payload_type == primary_type && read[levels].timestamp == 0x01020304U);
    CRD_FUZZ_REQUIRE(
        crd::fuzz::bytes_equal(read[levels].data.data(), read[levels].data.size(), primary, primary_length));
}

void add(crd::fuzz::SeedSink& sink, const char* name, std::initializer_list<u8> bytes)
{
    u8 buffer[64];
    usize size = 0U;
    for (const u8 b : bytes)
    {
        buffer[size++] = b;
    }
    sink.add(name, buffer, size);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const Reader rest{data + 1U, size - 1U};
    if (data[0] % 2U == 0U)
    {
        parse_request(rest);
    }
    else
    {
        encode_request(rest);
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Payloads (room, a timestamp, the bytes): §7's header with a short LPC block and primary, a primary alone,
    // three levels, lengths that leave an empty primary, and the refusals: an empty payload, a header cut at 1 and
    // 3 bytes, a chain that never ends, more blocks than room, lengths past the payload.
    add(sink, "parse-section-7", {0U, 4U, 0x00U, 0x00U, 0xBBU, 0x80U, 0x87U, 0x02U, 0x80U, 0x02U, 0x05U, 1U, 2U, 3U});
    add(sink, "parse-primary-alone", {0U, 1U, 0U, 0U, 0U, 0U, 0x6FU, 9U, 9U});
    add(sink, "parse-three-levels", {0U,    4U,    0U,    0U,    3U,    0xE8U, 0x89U, 0x0FU, 0x00U, 0x01U, 0x8AU, 0x0AU,
                                     0x00U, 0x01U, 0x8BU, 0x05U, 0x00U, 0x01U, 0x0CU, 1U,    2U,    3U,    4U,    5U});
    add(sink, "parse-empty", {0U, 4U, 0U, 0U, 0U, 0U});
    add(sink, "parse-cut-one", {0U, 4U, 0U, 0U, 0U, 0U, 0x87U});
    add(sink, "parse-cut-three", {0U, 4U, 0U, 0U, 0U, 0U, 0x87U, 0x00U, 0x40U});
    add(sink, "parse-endless", {0U, 4U, 0U, 0U, 0U, 0U, 0x87U, 0U, 0x40U, 0U, 0x88U, 0U, 0x40U, 0U});
    add(sink, "parse-no-room", {0U, 1U, 0U, 0U, 0U, 0U, 0x87U, 0U, 0x40U, 0U, 0x05U});
    add(sink, "parse-exact-lengths", {0U, 4U, 0U, 0U, 0U, 0U, 0x87U, 0U, 0x40U, 0x02U, 0x05U, 0xAAU, 0xBBU});
    add(sink, "parse-long-lengths", {0U, 4U, 0U, 0U, 0U, 0U, 0x87U, 0U, 0x40U, 0x02U, 0x05U, 0xAAU});
    // Packets to write (levels; per block type, offset, length, timestamp; primary type and length; slack): §7's
    // packet, a primary alone, the widest fields, and each refusal: a type, an offset and a length past their
    // fields, offset 0, an offset twice, a primary type past its field, one byte short.
    add(sink, "encode-section-7", {1U, 1U, 7U, 0U, 160U, 0U, 14U, 0U, 5U, 0U, 84U, 40U});
    add(sink, "encode-primary-alone", {1U, 0U, 111U, 0U, 3U, 32U});
    add(sink, "encode-widest",
        {1U, 2U, 127U, 0x3FU, 0xFFU, 0x03U, 0xFFU, 0U, 0U, 0U, 1U, 0U, 0U, 0U, 127U, 0U, 9U, 33U});
    add(sink, "encode-type-range", {1U, 1U, 128U, 0U, 160U, 0U, 4U, 0U, 5U, 0U, 6U, 40U});
    add(sink, "encode-offset-range", {1U, 1U, 7U, 0x40U, 0U, 0U, 4U, 0U, 5U, 0U, 6U, 40U});
    add(sink, "encode-length-range", {1U, 1U, 7U, 0U, 160U, 0x04U, 0x00U, 0U, 5U, 0U, 6U, 40U});
    add(sink, "encode-offset-zero", {1U, 1U, 7U, 0U, 0U, 0U, 4U, 0U, 5U, 0U, 6U, 40U});
    add(sink, "encode-offset-twice", {1U, 2U, 7U, 0U, 160U, 0U, 4U, 0U, 8U, 0U, 160U, 0U, 2U, 0U, 5U, 0U, 6U, 40U});
    add(sink, "encode-primary-range", {1U, 1U, 7U, 0U, 160U, 0U, 4U, 0U, 200U, 0U, 6U, 40U});
    add(sink, "encode-short", {1U, 1U, 7U, 0U, 160U, 0U, 4U, 0U, 5U, 0U, 6U, 0U});
}
