// crd-net -- admission stages S1 to S3 (4.2a; docs/design/network-foundation.md#ingress): QUIC's variable-length
// integers against RFC 9000's examples, RFC 9001's and RFC 9369's sample headers and Retry packets byte for byte,
// every header refusal, Version Negotiation's layout and reserved version, the keyed connection ID table against a
// reference through heavy churn, the handshake buckets' rates, and every path through the ingress, Retry and NEW_TOKEN
// round trips included.

#include "ingress_fixture.hpp"

#include <crd/crypto/quic.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/ingress.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireReader;
using crd::containers::WireWriter;
using crd::crypto::CryptoError;
using crd::crypto::QuicTokenKeys;
using crd::crypto::QuicVersion;
using crd::crypto::ReplayFilter;
using namespace crd::net;
using namespace crd::net::testing;

constexpr u8 kRouteKey[crd::crypto::kPrfKeyBytes] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                                                      0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
constexpr u8 kTokenKey[crd::crypto::kQuicTokenKeyBytes] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
constexpr u64 kUnix = 1793577600U; // 2026-11-02T00:00:00Z
constexpr u64 kSecond = 1000000000U;

constexpr u8 kClientDcid[8] = {0x83, 0x94, 0xC8, 0xF0, 0x3E, 0x51, 0x57, 0x08}; // RFC 9001 appendix A
constexpr u8 kClientScid[4] = {0xC1, 0xC2, 0xC3, 0xC4};

const Endpoint kPeer{IpAddress::v4(192, 0, 2, 7), 50000U};

ConstSpan<u8> view_of(const u8* bytes, usize size)
{
    return ConstSpan<u8>(bytes, size);
}

// ---- Variable-length integers ----

TEST_CASE("QUIC variable-length integers hold RFC 9000's examples and every length boundary", "[net][ingress]")
{
    struct Example
    {
        const char* bytes;
        u64 value;
    };
    // RFC 9000 appendix A.1, the two-byte encoding of 37 included.
    const Example examples[] = {{"c2197c5eff14e88c", 151288809941952652ULL},
                                {"9d7f3e7d", 494878333U},
                                {"7bbd", 15293U},
                                {"25", 37U},
                                {"4025", 37U}};
    for (const Example& example : examples)
    {
        u8 bytes[8] = {};
        const usize size = hex(example.bytes, bytes);
        WireReader in(view_of(bytes, size));
        u64 value = 0U;
        REQUIRE(take_quic_varint(in, value));
        CHECK(value == example.value);
        CHECK(in.done());
    }

    const u64 boundaries[] = {0U, 63U, 64U, 16383U, 16384U, (u64{1} << 30U) - 1U, u64{1} << 30U, kQuicMaxVarint};
    const usize lengths[] = {1U, 1U, 2U, 2U, 4U, 4U, 8U, 8U};
    for (usize index = 0U; index < 8U; ++index)
    {
        CHECK(quic_varint_bytes(boundaries[index]) == lengths[index]);
        u8 bytes[8] = {};
        WireWriter out(Span<u8>(bytes, sizeof(bytes)));
        put_quic_varint(out, boundaries[index]);
        CHECK(out.written() == lengths[index]);
        WireReader in(view_of(bytes, out.written()));
        u64 value = 0U;
        REQUIRE(take_quic_varint(in, value));
        CHECK(value == boundaries[index]);
    }
    CHECK(quic_varint_bytes(kQuicMaxVarint + 1U) == 0U);

    // A short input consumes nothing.
    const u8 cut[3] = {0x80U, 0x01U, 0x02U};
    WireReader in(view_of(cut, sizeof(cut)));
    u64 value = 7U;
    CHECK_FALSE(take_quic_varint(in, value));
    CHECK(in.consumed() == 0U);
    CHECK(value == 7U);
    WireReader empty(ConstSpan<u8>{});
    CHECK_FALSE(take_quic_varint(empty, value));
}

// ---- S1: headers ----

TEST_CASE("S1 reads RFC 9001's and RFC 9369's sample Initial headers", "[net][ingress]")
{
    // The client Initial of RFC 9001 appendix A.2 as it is sent (first byte under header protection), and RFC 9369
    // appendix A.2's version 2 header: 1200-byte datagrams whose Length (0x449e) covers the rest.
    const char* const headers[] = {"c000000001088394c8f03e5157080000449e", "d36b3343cf088394c8f03e5157080000449e"};
    const u32 versions[] = {kV1, kV2};
    for (usize sample = 0U; sample < 2U; ++sample)
    {
        Datagram datagram;
        const usize header = hex(headers[sample], datagram.bytes);
        std::memset(datagram.bytes + header, 0x6B, kQuicMinInitialDatagram - header);
        datagram.size = kQuicMinInitialDatagram;
        QuicHeader read;
        REQUIRE(parse_quic_header(datagram.view(), 16U, read) == QuicHeaderError::None);
        CHECK(read.type == QuicPacketType::Initial);
        CHECK(read.version == versions[sample]);
        CHECK(same(read.dcid, view_of(kClientDcid, sizeof(kClientDcid))));
        CHECK(read.scid.empty());
        CHECK(read.token.empty());
        CHECK(read.number_offset == header);
        CHECK(read.packet_bytes == kQuicMinInitialDatagram);
    }

    // RFC 9001 appendix A.3's server Initial header: an empty Destination, an 8-byte Source and a Length of 0x75.
    Datagram server;
    const usize header = hex("c1000000010008f067a5502a4262b5004075", server.bytes);
    server.size = header + 0x75U + 40U; // a second coalesced packet follows
    QuicHeader read;
    REQUIRE(parse_quic_header(server.view(), 16U, read) == QuicHeaderError::None);
    CHECK(read.type == QuicPacketType::Initial);
    CHECK(read.dcid.empty());
    CHECK(read.scid.size() == 8U);
    CHECK(read.packet_bytes == header + 0x75U);
}

TEST_CASE("S1 reads every long packet type of both versions", "[net][ingress]")
{
    const QuicPacketType types[] = {QuicPacketType::Initial, QuicPacketType::ZeroRtt, QuicPacketType::Handshake};
    for (const u32 version : {kV1, kV2})
    {
        for (const QuicPacketType type : types)
        {
            const Datagram datagram =
                long_packet(version, type, view_of(kClientDcid, 8U), view_of(kClientScid, 4U), {}, 100U);
            QuicHeader read;
            REQUIRE(parse_quic_header(datagram.view(), 16U, read) == QuicHeaderError::None);
            CHECK(read.type == type);
            CHECK(read.version == version);
            CHECK(same(read.scid, view_of(kClientScid, 4U)));
            CHECK(read.packet_bytes == 100U);
        }
        Datagram retry;
        retry.bytes[0] = static_cast<u8>(0xC0U | (long_type_bits(version, QuicPacketType::Retry) << 4U));
        crd::containers::wire_store<u32>(Span<u8>(retry.bytes), 1U, version);
        retry.size = 7U + 30U;
        QuicHeader read;
        REQUIRE(parse_quic_header(retry.view(), 16U, read) == QuicHeaderError::None);
        CHECK(read.type == QuicPacketType::Retry);
    }
}

TEST_CASE("S1 refuses every malformed header", "[net][ingress]")
{
    QuicHeader read;
    CHECK(parse_quic_header(ConstSpan<u8>{}, 16U, read) == QuicHeaderError::Empty);

    // Every prefix of an Initial's header is truncated.
    const u8 token[3] = {1U, 2U, 3U};
    const Datagram whole = initial(kV1, view_of(kClientDcid, 8U), view_of(kClientScid, 4U), view_of(token, 3U));
    const usize header_bytes = 1U + 4U + 1U + 8U + 1U + 4U + 1U + 3U + 2U;
    for (usize size = 1U; size < header_bytes; ++size)
    {
        CHECK(parse_quic_header(whole.view().first(size), 16U, read) == QuicHeaderError::Truncated);
    }
    CHECK(parse_quic_header(whole.view(), 16U, read) == QuicHeaderError::None);
    CHECK(same(read.token, view_of(token, 3U)));

    // A Length past the datagram, and one too short to hold the header protection sample.
    CHECK(parse_quic_header(whole.view().first(whole.size - 1U), 16U, read) == QuicHeaderError::LengthPastDatagram);
    Datagram short_length = whole;
    crd::containers::wire_store<u16>(Span<u8>(short_length.bytes), header_bytes - 2U,
                                     static_cast<u16>(0x4000U | (kQuicMinProtectedBytes - 1U)));
    CHECK(parse_quic_header(short_length.view(), 16U, read) == QuicHeaderError::TooShortToProtect);

    // A token length past the datagram.
    Datagram long_token = whole;
    long_token.bytes[1U + 4U + 1U + 8U + 1U + 4U] = 0x7FU;
    long_token.size = 40U;
    CHECK(parse_quic_header(long_token.view(), 16U, read) == QuicHeaderError::Truncated);

    // Version 1 and 2 cap connection IDs at 20 bytes; the invariants allow 255 for other versions.
    u8 long_id[255] = {};
    const Datagram v1_long = long_packet(kV1, QuicPacketType::Handshake, view_of(long_id, 21U), {}, {}, 100U);
    CHECK(parse_quic_header(v1_long.view(), 16U, read) == QuicHeaderError::ConnectionIdTooLong);
    const Datagram v2_long_source = long_packet(kV2, QuicPacketType::Handshake, {}, view_of(long_id, 21U), {}, 100U);
    CHECK(parse_quic_header(v2_long_source.view(), 16U, read) == QuicHeaderError::ConnectionIdTooLong);
    Datagram other;
    WireWriter writer(Span<u8>(other.bytes, sizeof(other.bytes)));
    writer.put_u8(0xC0U);
    writer.put_u32(0x1A2A3A4AU);
    writer.put_u8(255U);
    writer.put_bytes(view_of(long_id, 255U));
    writer.put_u8(255U);
    writer.put_bytes(view_of(long_id, 255U));
    other.size = writer.written();
    REQUIRE(parse_quic_header(other.view(), 16U, read) == QuicHeaderError::None);
    CHECK(read.type == QuicPacketType::OtherVersion);
    CHECK(read.dcid.size() == 255U);
    CHECK(read.scid.size() == 255U);
    CHECK(parse_quic_header(other.view().first(other.size - 1U), 16U, read) == QuicHeaderError::Truncated);

    // A version field of zero is Version Negotiation.
    other.bytes[1] = other.bytes[2] = other.bytes[3] = other.bytes[4] = 0U;
    REQUIRE(parse_quic_header(other.view(), 16U, read) == QuicHeaderError::None);
    CHECK(read.type == QuicPacketType::VersionNegotiation);

    // A short header holds the endpoint's own ID length and then a protection sample; its fixed bit is not checked.
    u8 cid[16] = {};
    Datagram one = short_packet(view_of(cid, 16U), 1U + 16U + kQuicMinProtectedBytes);
    REQUIRE(parse_quic_header(one.view(), 16U, read) == QuicHeaderError::None);
    CHECK(read.type == QuicPacketType::OneRtt);
    CHECK(read.number_offset == 17U);
    one.bytes[0] = 0x01U;
    CHECK(parse_quic_header(one.view(), 16U, read) == QuicHeaderError::None);
    CHECK(parse_quic_header(one.view().first(one.size - 1U), 16U, read) == QuicHeaderError::TooShortToProtect);
    CHECK(parse_quic_header(one.view().first(10U), 16U, read) == QuicHeaderError::Truncated);
}

// ---- Stateless answers ----

TEST_CASE("Version Negotiation swaps the IDs, lists the versions and one reserved version", "[net][ingress]")
{
    u8 dcid[255];
    for (usize index = 0U; index < sizeof(dcid); ++index)
    {
        dcid[index] = static_cast<u8>(index);
    }
    const u32 versions[] = {kV1, kV2};
    for (u64 position = 0U; position < 3U; ++position)
    {
        const u64 entropy = (position << 40U) | (u64{0x12345678U} << 8U) | 0x3FU;
        u8 out[kQuicMaxVersionNegotiationBytes] = {};
        const usize written = write_version_negotiation(view_of(dcid, 255U), view_of(kClientScid, 4U),
                                                        ConstSpan<u32>(versions), entropy, Span<u8>(out));
        REQUIRE(written == 1U + 4U + 1U + 4U + 1U + 255U + 12U);
        CHECK(written <= kQuicMaxVersionNegotiationBytes);
        CHECK(out[0] == 0xFFU);
        QuicHeader read;
        REQUIRE(parse_quic_header(view_of(out, written), 16U, read) == QuicHeaderError::None);
        CHECK(read.type == QuicPacketType::VersionNegotiation);
        CHECK(same(read.dcid, view_of(kClientScid, 4U)));
        CHECK(same(read.scid, view_of(dcid, 255U)));
        const usize list = 1U + 4U + 1U + 4U + 1U + 255U;
        usize listed = 0U;
        for (usize slot = 0U; slot < 3U; ++slot)
        {
            const u32 version = crd::containers::wire_load<u32>(view_of(out, written), list + (4U * slot));
            if (slot == position)
            {
                CHECK(quic_version_reserved(version));
                CHECK(version == quic_grease_version(0x12345678U));
            }
            else
            {
                CHECK(version == versions[listed++]);
            }
        }
    }
    CHECK(quic_grease_version(0U) == 0x0A0A0A0AU);
    CHECK(quic_version_reserved(quic_grease_version(0xFFFFFFFFU)));
    CHECK_FALSE(quic_version_reserved(kV1));
    CHECK_FALSE(quic_version_reserved(kV2));

    u8 out[kQuicMaxVersionNegotiationBytes] = {};
    const u32 reserved[] = {kV1, 0x1A2A3A4AU};
    const u32 zero[] = {0U};
    const u32 nine[9] = {kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1};
    const ConstSpan<u8> id = view_of(kClientDcid, 8U);
    CHECK(write_version_negotiation(id, id, ConstSpan<u32>{}, 0U, Span<u8>(out)) == 0U);
    CHECK(write_version_negotiation(id, id, ConstSpan<u32>(reserved), 0U, Span<u8>(out)) == 0U);
    CHECK(write_version_negotiation(id, id, ConstSpan<u32>(zero), 0U, Span<u8>(out)) == 0U);
    CHECK(write_version_negotiation(id, id, ConstSpan<u32>(nine), 0U, Span<u8>(out)) == 0U);
    CHECK(write_version_negotiation(id, id, ConstSpan<u32>(versions), 0U, Span<u8>(out, 34U)) == 0U);
    CHECK(write_version_negotiation(id, id, ConstSpan<u32>(versions), 0U, Span<u8>(out, 35U)) == 35U);
}

TEST_CASE("Retry matches RFC 9001's and RFC 9369's sample packets byte for byte", "[net][ingress]")
{
    u8 scid[8] = {};
    REQUIRE(hex("f067a5502a4262b5", scid) == 8U);
    const u8 token[] = {'t', 'o', 'k', 'e', 'n'};
    struct Sample
    {
        QuicVersion version;
        const char* packet;
    };
    const Sample samples[] = {
        {QuicVersion::V1, "ff000000010008f067a5502a4262b5746f6b656e04a265ba2eff4d829058fb3f0f2496ba"},
        {QuicVersion::V2, "cf6b3343cf0008f067a5502a4262b5746f6b656ec8646ce8bfe33952d955543665dcc7b6"},
    };
    for (const Sample& sample : samples)
    {
        u8 expected[64] = {};
        const usize expected_size = hex(sample.packet, expected);
        u8 out[64] = {};
        usize written = 0U;
        REQUIRE(write_retry(sample.version, ConstSpan<u8>{}, view_of(scid, 8U), view_of(kClientDcid, 8U),
                            view_of(token, 5U), 0x0FU, Span<u8>(out), written) == CryptoError::None);
        CHECK(written == quic_retry_bytes(0U, 8U, 5U));
        CHECK(same(view_of(out, written), view_of(expected, expected_size)));
        CHECK(crd::crypto::quic_retry_verify(sample.version, view_of(kClientDcid, 8U), view_of(out, written)) ==
              CryptoError::None);
        QuicHeader read;
        REQUIRE(parse_quic_header(view_of(out, written), 16U, read) == QuicHeaderError::None);
        CHECK(read.type == QuicPacketType::Retry);

        CHECK(write_retry(sample.version, {}, view_of(kClientDcid, 8U), view_of(kClientDcid, 8U), {}, 0U,
                          Span<u8>(out), written) == CryptoError::Overlap);
        CHECK(write_retry(sample.version, {}, {}, view_of(kClientDcid, 8U), {}, 0U, Span<u8>(out), written) ==
              CryptoError::BadLength);
        u8 long_id[21] = {};
        CHECK(write_retry(sample.version, view_of(long_id, 21U), view_of(scid, 8U), view_of(kClientDcid, 8U), {}, 0U,
                          Span<u8>(out), written) == CryptoError::BadLength);
        CHECK(write_retry(sample.version, {}, view_of(scid, 8U), view_of(kClientDcid, 8U), view_of(token, 5U), 0U,
                          Span<u8>(out, expected_size - 1U), written) == CryptoError::BadLength);
    }
    usize written = 0U;
    u8 out[64] = {};
    CHECK(write_retry(static_cast<QuicVersion>(0x1A2A3A4AU), {}, view_of(scid, 8U), view_of(kClientDcid, 8U), {}, 0U,
                      Span<u8>(out), written) == CryptoError::Unsupported);
}

// ---- S2: the connection ID table ----

TEST_CASE("The connection ID table routes, refuses and fails closed", "[net][ingress]")
{
    CountingAllocator allocator;
    ConnectionIdTable table(allocator, 4U, ConstSpan<u8>(kRouteKey));
    REQUIRE(table.ready());
    CHECK(table.capacity() == 4U);
    const u8 ids[5][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12}, {13, 14, 15}};
    for (u32 index = 0U; index < 4U; ++index)
    {
        CHECK(table.insert(view_of(ids[index], 3U), index) == RouteError::None);
    }
    CHECK(table.insert(view_of(ids[4], 3U), 4U) == RouteError::Full);
    CHECK(table.insert(view_of(ids[0], 3U), 9U) == RouteError::Exists);
    CHECK(table.find(view_of(ids[2], 3U)) == 2U);
    CHECK(table.find(view_of(ids[2], 2U)) == kNoConnection); // a prefix is another ID
    CHECK(table.erase(view_of(ids[1], 3U)));
    CHECK_FALSE(table.erase(view_of(ids[1], 3U)));
    CHECK(table.find(view_of(ids[1], 3U)) == kNoConnection);
    CHECK(table.insert(view_of(ids[4], 3U), 4U) == RouteError::None);
    CHECK(table.size() == 4U);
    // The empty ID is an ID (a peer that uses none), and 21 bytes is not.
    CHECK(table.erase(view_of(ids[0], 3U)));
    CHECK(table.insert(ConstSpan<u8>{}, 7U) == RouteError::None);
    CHECK(table.find(ConstSpan<u8>{}) == 7U);
    u8 long_id[21] = {};
    CHECK(table.insert(view_of(long_id, 21U), 1U) == RouteError::BadLength);
    CHECK(table.find(view_of(long_id, 21U)) == kNoConnection);
    CHECK_FALSE(table.erase(view_of(long_id, 21U)));
    CHECK(table.insert(view_of(long_id, 20U), kNoConnection) == RouteError::BadValue);

    // No storage, no key or no capacity: nothing routes.
    CountingAllocator refusing;
    refusing.refuse_after(0U);
    ConnectionIdTable empty(refusing, 4U);
    CHECK_FALSE(empty.ready());
    CHECK(empty.insert(view_of(ids[0], 3U), 1U) == RouteError::NotReady);
    CHECK(empty.find(view_of(ids[0], 3U)) == kNoConnection);
    const u8 short_key[4] = {};
    CHECK_FALSE(ConnectionIdTable(allocator, 4U, ConstSpan<u8>(short_key)).ready());
    CHECK_FALSE(ConnectionIdTable(allocator, 0U).ready());
    CHECK_FALSE(ConnectionIdTable(allocator, kMaxRoutes + 1U).ready());
    CHECK(ConnectionIdTable(allocator, 4U).ready()); // a fresh key from the provider
}

TEST_CASE("The connection ID table agrees with a reference through heavy churn", "[net][ingress]")
{
    // Sixteen slots for eight IDs: runs wrap around the end constantly, which is where backward-shift deletion goes
    // wrong if it does.
    CountingAllocator allocator;
    ConnectionIdTable table(allocator, 8U, ConstSpan<u8>(kRouteKey));
    REQUIRE(table.ready());
    constexpr u32 id_count = 24U;
    u32 reference[id_count];
    for (u32& connection : reference)
    {
        connection = kNoConnection;
    }
    u32 held = 0U;
    u64 state = 0x9E3779B97F4A7C15ULL;
    const auto next = [&state]() -> u32
    {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return static_cast<u32>(state >> 32U);
    };
    for (u32 step = 0U; step < 200000U; ++step)
    {
        const u32 which = next() % id_count;
        const u8 id[2] = {static_cast<u8>(which), static_cast<u8>(which * 7U)};
        const ConstSpan<u8> key = view_of(id, (which % 3U == 0U) ? 1U : 2U);
        if ((next() & 1U) != 0U)
        {
            const RouteError error = table.insert(key, step);
            if (reference[which] != kNoConnection)
            {
                REQUIRE(error == RouteError::Exists);
            }
            else if (held == 8U)
            {
                REQUIRE(error == RouteError::Full);
            }
            else
            {
                REQUIRE(error == RouteError::None);
                reference[which] = step;
                ++held;
            }
        }
        else
        {
            REQUIRE(table.erase(key) == (reference[which] != kNoConnection));
            if (reference[which] != kNoConnection)
            {
                reference[which] = kNoConnection;
                --held;
            }
        }
        if (step % 64U == 0U)
        {
            for (u32 check = 0U; check < id_count; ++check)
            {
                const u8 probe[2] = {static_cast<u8>(check), static_cast<u8>(check * 7U)};
                REQUIRE(table.find(view_of(probe, (check % 3U == 0U) ? 1U : 2U)) == reference[check]);
            }
        }
    }
    CHECK(table.size() == held);
}

TEST_CASE("Sequential and colliding IDs keep short probe runs under the keyed hash", "[net][ingress]")
{
    // IDs an attacker would pick against a public hash: counters, and IDs that differ only in their last byte.
    CountingAllocator allocator;
    ConnectionIdTable table(allocator, 4096U, ConstSpan<u8>(kRouteKey));
    REQUIRE(table.ready());
    u8 id[8] = {};
    for (u32 index = 0U; index < 4096U; ++index)
    {
        crd::containers::wire_store<u32>(Span<u8>(id), 4U, index);
        REQUIRE(table.insert(view_of(id, 8U), index) == RouteError::None);
    }
    for (u32 index = 0U; index < 4096U; ++index)
    {
        crd::containers::wire_store<u32>(Span<u8>(id), 4U, index);
        REQUIRE(table.find(view_of(id, 8U)) == index);
    }
    // At load one half, linear probing's longest run grows with the logarithm of the table: 8192 slots stay far below
    // the hundreds a clustered public hash reaches.
    CHECK(table.longest_probe() <= 32U);
    const usize calls = allocator.calls();
    for (u32 index = 0U; index < 4096U; ++index)
    {
        crd::containers::wire_store<u32>(Span<u8>(id), 4U, index);
        REQUIRE(table.erase(view_of(id, 8U)));
    }
    CHECK(allocator.calls() == calls);
    CHECK(table.size() == 0U);
}

// ---- S3: handshake buckets ----

HandshakeRules tight_rules()
{
    HandshakeRules rules;
    rules.unvalidated = GateRule{1000U, kSecond};
    rules.per_prefix = GateRule{2U, kSecond}; // three at once from an idle bucket, then one per half second
    rules.validated = GateRule{4U, kSecond};
    rules.answers = GateRule{1000U, kSecond};
    rules.prefix_buckets = 1024U;
    return rules;
}

TEST_CASE("Handshake buckets rate unproven prefixes, send them to Retry and rate proven addresses", "[net][ingress]")
{
    CountingAllocator allocator;
    HandshakeBuckets buckets(allocator, tight_rules(), ConstSpan<u8>(kRouteKey));
    REQUIRE(buckets.ready());
    const u64 t0 = 10U * kSecond;
    const IpAddress first = IpAddress::v4(198, 51, 100, 1);
    u32 admitted = 0U;
    while (buckets.admit(first, false, t0) == HandshakeVerdict::Admit)
    {
        ++admitted;
        REQUIRE(admitted < 10U);
    }
    CHECK(admitted == 3U);
    CHECK(buckets.admit(first, false, t0) == HandshakeVerdict::Retry);
    // Another /32 has its own bucket (unless the keyed hash put it in the same one).
    const IpAddress second = IpAddress::v4(198, 51, 100, 2);
    if (buckets.bucket_of(second) != buckets.bucket_of(first))
    {
        CHECK(buckets.admit(second, false, t0) == HandshakeVerdict::Admit);
    }
    // Half a second refills one.
    CHECK(buckets.admit(first, false, t0 + (kSecond / 2U)) == HandshakeVerdict::Admit);
    CHECK(buckets.admit(first, false, t0 + (kSecond / 2U)) == HandshakeVerdict::Retry);

    // A proven address passes a closed prefix and charges it; the proven rate is its own limit.
    u32 proven = 0U;
    while (buckets.admit(first, true, t0) == HandshakeVerdict::Admit)
    {
        ++proven;
        REQUIRE(proven < 10U);
    }
    CHECK(proven == 5U);
    CHECK(buckets.admit(first, true, t0) == HandshakeVerdict::Refuse);

    // Answers have their own rate.
    HandshakeRules one_answer = tight_rules();
    one_answer.answers = GateRule{1U, 0U};
    HandshakeBuckets answers(allocator, one_answer, ConstSpan<u8>(kRouteKey));
    REQUIRE(answers.ready());
    CHECK(answers.answer(t0));
    CHECK_FALSE(answers.answer(t0));
    CHECK(answers.answer(t0 + kSecond));
}

TEST_CASE("Handshake buckets key IPv6 by /64 and IPv4 by its configured prefix", "[net][ingress]")
{
    CountingAllocator allocator;
    HandshakeRules rules = tight_rules();
    rules.ipv4_prefix_bits = 24U;
    HandshakeBuckets buckets(allocator, rules, ConstSpan<u8>(kRouteKey));
    REQUIRE(buckets.ready());
    const u8 a[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 2, 0xAA, 0xBB, 0, 0, 0, 0, 0, 1};
    const u8 b[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 2, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    CHECK(buckets.bucket_of(IpAddress::v6(a)) == buckets.bucket_of(IpAddress::v6(b)));
    CHECK(buckets.bucket_of(IpAddress::v6(a, 3U)) == buckets.bucket_of(IpAddress::v6(a))); // a zone is not a host
    CHECK(buckets.bucket_of(IpAddress::v4(203, 0, 113, 1)) == buckets.bucket_of(IpAddress::v4(203, 0, 113, 254)));

    // One /64 shares its rate across all its addresses.
    const u64 t0 = 5U * kSecond;
    u32 admitted = 0U;
    for (u8 host = 0U; host < 8U; ++host)
    {
        u8 address[16];
        std::memcpy(address, a, sizeof(address));
        address[15] = host;
        admitted += (buckets.admit(IpAddress::v6(address), false, t0) == HandshakeVerdict::Admit) ? 1U : 0U;
    }
    CHECK(admitted == 3U);

    rules.always_retry = true;
    HandshakeBuckets strict(allocator, rules, ConstSpan<u8>(kRouteKey));
    CHECK(strict.admit(IpAddress::v4(203, 0, 113, 1), false, t0) == HandshakeVerdict::Retry);
    CHECK(strict.admit(IpAddress::v4(203, 0, 113, 1), true, t0) == HandshakeVerdict::Admit);

    // Rules out of range, or no storage: not ready, and nothing is admitted.
    HandshakeRules bad = tight_rules();
    bad.prefix_buckets = 3U;
    CHECK_FALSE(HandshakeBuckets(allocator, bad).ready());
    bad = tight_rules();
    bad.prefix_buckets = 0U;
    CHECK_FALSE(HandshakeBuckets(allocator, bad).ready());
    bad = tight_rules();
    bad.ipv4_prefix_bits = 33U;
    CHECK_FALSE(HandshakeBuckets(allocator, bad).ready());
    bad = tight_rules();
    bad.ipv6_prefix_bits = 0U;
    CHECK_FALSE(HandshakeBuckets(allocator, bad).ready());
    CountingAllocator refusing;
    refusing.refuse_after(0U);
    HandshakeBuckets none(refusing, tight_rules());
    CHECK_FALSE(none.ready());
    CHECK(none.admit(IpAddress::v4(203, 0, 113, 1), true, t0) == HandshakeVerdict::Refuse);
    CHECK_FALSE(none.answer(t0));
}

// ---- The ingress ----

struct Server
{
    explicit Server(const HandshakeRules& rules = tight_rules(), const IngressConfig& config = IngressConfig{},
                    bool with_tokens = true, bool with_new_tokens = true)
        : routes(allocator, 64U, ConstSpan<u8>(kRouteKey)), buckets(allocator, rules, ConstSpan<u8>(kRouteKey)),
          ingress(config, routes, buckets, with_tokens ? &tokens : nullptr, with_new_tokens ? &replay : nullptr)
    {
        REQUIRE(tokens.init(ConstSpan<u8>(kTokenKey), {}) == CryptoError::None);
        REQUIRE(replay.init(1024U, crd::crypto::kQuicNewTokenLifetime, kUnix) == CryptoError::None);
        REQUIRE(routes.ready());
        REQUIRE(buckets.ready());
    }

    IngressDecision classify(const Datagram& datagram, const Endpoint& peer = kPeer, u64 now = kSecond,
                             u64 unix_seconds = kUnix)
    {
        return ingress.classify(datagram.view(), false, peer, now, unix_seconds);
    }

    CountingAllocator allocator;
    ConnectionIdTable routes;
    HandshakeBuckets buckets;
    QuicTokenKeys tokens;
    ReplayFilter replay;
    Ingress ingress;
};

u64 dropped(const Ingress& ingress, DropReason reason)
{
    return ingress.counters().drops[static_cast<usize>(reason)];
}

TEST_CASE("The ingress refuses configurations it cannot run", "[net][ingress]")
{
    CountingAllocator allocator;
    ConnectionIdTable routes(allocator, 4U);
    HandshakeBuckets buckets(allocator, HandshakeRules{});
    IngressConfig config;
    config.local_cid_bytes = 7U;
    CHECK_FALSE(Ingress(config, routes, buckets, nullptr, nullptr).ready());
    config.local_cid_bytes = 21U;
    CHECK_FALSE(Ingress(config, routes, buckets, nullptr, nullptr).ready());
    config = IngressConfig{};
    config.version_count = 0U;
    CHECK_FALSE(Ingress(config, routes, buckets, nullptr, nullptr).ready());
    config = IngressConfig{};
    config.versions[1] = 0x1A2A3A4AU;
    CHECK_FALSE(Ingress(config, routes, buckets, nullptr, nullptr).ready());
    config = IngressConfig{};
    config.version_count = kQuicMaxNegotiationVersions + 1U;
    CHECK_FALSE(Ingress(config, routes, buckets, nullptr, nullptr).ready());

    config = IngressConfig{};
    config.version_count = 0U;
    Ingress refused(config, routes, buckets, nullptr, nullptr);
    const Datagram datagram = initial(kV1, view_of(kClientDcid, 8U), view_of(kClientScid, 4U));
    CHECK(refused.classify(datagram.view(), false, kPeer, kSecond, kUnix).action == IngressAction::Drop);
    CHECK(Ingress(IngressConfig{}, routes, buckets, nullptr, nullptr).ready());
}

TEST_CASE("S1 drops what no server takes and answers unsupported versions only from 1200 bytes", "[net][ingress]")
{
    Server server;
    const Datagram datagram = initial(kV1, view_of(kClientDcid, 8U), view_of(kClientScid, 4U));
    CHECK(server.ingress.classify(datagram.view(), true, kPeer, kSecond, kUnix).reason == DropReason::Truncated);
    CHECK(server.ingress.classify(datagram.view().first(10U), false, kPeer, kSecond, kUnix).reason ==
          DropReason::Malformed);
    CHECK(server.ingress.classify(ConstSpan<u8>{}, false, kPeer, kSecond, kUnix).reason == DropReason::Malformed);
    u8 long_id[21] = {};
    CHECK(server.classify(long_packet(kV1, QuicPacketType::Handshake, view_of(long_id, 21U), {}, {}, 100U)).reason ==
          DropReason::ConnectionIdTooLong);
    CHECK(server.classify(short_packet(view_of(long_id, 16U), 20U)).reason == DropReason::TooShortToProtect);

    // An unsupported version: answered from 1200 bytes, dropped below.
    Datagram other = initial(kV1, view_of(kClientDcid, 8U), view_of(kClientScid, 4U));
    crd::containers::wire_store<u32>(Span<u8>(other.bytes), 1U, 0x1A2A3A4AU);
    const IngressDecision negotiate = server.classify(other);
    REQUIRE(negotiate.action == IngressAction::NegotiateVersion);
    u8 answer[kQuicMaxStatelessAnswerBytes] = {};
    const usize written = server.ingress.answer(negotiate, kPeer, kUnix, Span<u8>(answer));
    REQUIRE(written != 0U);
    CHECK(written <= 3U * other.size);
    QuicHeader read;
    REQUIRE(parse_quic_header(view_of(answer, written), 16U, read) == QuicHeaderError::None);
    CHECK(read.type == QuicPacketType::VersionNegotiation);
    CHECK(same(read.dcid, view_of(kClientScid, 4U)));
    CHECK(same(read.scid, view_of(kClientDcid, 8U)));
    CHECK(server.ingress.counters().negotiations == 1U);
    other.size = kQuicMinInitialDatagram - 1U;
    crd::containers::wire_store<u16>(Span<u8>(other.bytes), 1U + 4U + 1U + 8U + 1U + 4U + 1U,
                                     static_cast<u16>(0x4000U | (other.size - 22U)));
    CHECK(server.classify(other).reason == DropReason::UnsupportedSmall);

    // A Version Negotiation packet or a Retry is never a client's to send.
    Datagram vn;
    vn.size = write_version_negotiation(view_of(kClientDcid, 8U), view_of(kClientScid, 4U),
                                        ConstSpan<u32>(IngressConfig{}.versions, 2U), 0U, Span<u8>(vn.bytes));
    CHECK(server.classify(vn).reason == DropReason::VersionNegotiation);
    Datagram retry;
    retry.bytes[0] = 0xF0U;
    crd::containers::wire_store<u32>(Span<u8>(retry.bytes), 1U, kV1);
    retry.size = 1200U;
    CHECK(server.classify(retry).reason == DropReason::ClientRetry);

    // A version this build reads but the server does not run is negotiated too.
    IngressConfig v1_only;
    v1_only.version_count = 1U;
    Server narrow(tight_rules(), v1_only);
    const IngressDecision v2 = narrow.classify(initial(kV2, view_of(kClientDcid, 8U), view_of(kClientScid, 4U)));
    REQUIRE(v2.action == IngressAction::NegotiateVersion);
    const usize v2_written = narrow.ingress.answer(v2, kPeer, kUnix, Span<u8>(answer));
    REQUIRE(v2_written == 1U + 4U + 1U + 4U + 1U + 8U + 8U);
}

TEST_CASE("S2 routes known IDs and S3 admits new Initials", "[net][ingress]")
{
    Server server;
    const Datagram first = initial(kV1, view_of(kClientDcid, 8U), view_of(kClientScid, 4U));
    const IngressDecision admit = server.classify(first);
    REQUIRE(admit.action == IngressAction::Admit);
    CHECK_FALSE(admit.validated);
    CHECK(same(admit.header.dcid, view_of(kClientDcid, 8U)));
    CHECK(server.ingress.counters().admitted == 1U);

    // The connection routes the client's original ID and its own; later packets on either reach it.
    REQUIRE(server.routes.insert(view_of(kClientDcid, 8U), 5U) == RouteError::None);
    const IngressDecision again = server.classify(first);
    CHECK(again.action == IngressAction::Route);
    CHECK(again.connection == 5U);
    const u8 own[16] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    REQUIRE(server.routes.insert(view_of(own, 16U), 6U) == RouteError::None);
    CHECK(server.classify(short_packet(view_of(own, 16U))).connection == 6U);
    CHECK(server.classify(long_packet(kV1, QuicPacketType::Handshake, view_of(own, 16U), {}, {}, 80U)).connection ==
          6U);
    CHECK(server.ingress.counters().routed == 3U);

    // For no connection: a short header, 0-RTT and Handshake are dropped.
    const u8 unknown[16] = {1};
    CHECK(server.classify(short_packet(view_of(unknown, 16U))).reason == DropReason::UnknownConnection);
    CHECK(server.classify(long_packet(kV1, QuicPacketType::ZeroRtt, view_of(unknown, 16U), {}, {}, 80U)).reason ==
          DropReason::EarlyPacket);
    CHECK(server.classify(long_packet(kV2, QuicPacketType::Handshake, view_of(unknown, 16U), {}, {}, 80U)).reason ==
          DropReason::EarlyPacket);

    // A client Initial needs 1200 bytes and an 8-byte Destination Connection ID.
    CHECK(server.classify(initial(kV1, view_of(unknown, 8U), {}, {}, 1199U)).reason == DropReason::SmallInitial);
    CHECK(server.classify(initial(kV1, view_of(unknown, 7U), {})).reason == DropReason::ShortClientDcid);
    CHECK(dropped(server.ingress, DropReason::SmallInitial) == 1U);
}

TEST_CASE("S3 sends unproven addresses to Retry, and the Retry token proves the address", "[net][ingress]")
{
    HandshakeRules rules = tight_rules();
    rules.always_retry = true;
    for (const u32 version : {kV1, kV2})
    {
        Server server(rules);
        const Datagram first = initial(version, view_of(kClientDcid, 8U), view_of(kClientScid, 4U));
        const IngressDecision retry = server.classify(first);
        REQUIRE(retry.action == IngressAction::Retry);
        u8 answer[kQuicMaxStatelessAnswerBytes] = {};
        const usize written = server.ingress.answer(retry, kPeer, kUnix, Span<u8>(answer));
        REQUIRE(written != 0U);
        CHECK(written * 3U <= first.size);
        const auto quic_version = static_cast<QuicVersion>(version);
        CHECK(crd::crypto::quic_retry_verify(quic_version, view_of(kClientDcid, 8U), view_of(answer, written)) ==
              CryptoError::None);
        QuicHeader read;
        REQUIRE(parse_quic_header(view_of(answer, written), 16U, read) == QuicHeaderError::None);
        REQUIRE(read.type == QuicPacketType::Retry);
        CHECK(same(read.dcid, view_of(kClientScid, 4U)));
        CHECK(read.scid.size() == 16U);
        const usize token_at = 1U + 4U + 1U + read.dcid.size() + 1U + read.scid.size();
        const ConstSpan<u8> token = view_of(answer + token_at, written - token_at - crd::crypto::kQuicTagBytes);
        CHECK(server.ingress.counters().retries == 1U);

        // The client's next Initial goes to the Retry's Source Connection ID with the token: admitted as proven,
        // carrying the original Destination Connection ID for the transport parameters.
        u8 retry_scid[20] = {};
        std::memcpy(retry_scid, read.scid.data(), read.scid.size());
        const Datagram second =
            initial(version, view_of(retry_scid, 16U), view_of(kClientScid, 4U), token, kQuicMinInitialDatagram);
        const IngressDecision admit = server.classify(second, kPeer, kSecond, kUnix + 5U);
        REQUIRE(admit.action == IngressAction::Admit);
        CHECK(admit.validated);
        CHECK(same(admit.claims.original_dcid_view(), view_of(kClientDcid, 8U)));
        CHECK(server.ingress.counters().admitted_validated == 1U);

        // Another port, a later time, or a damaged token is dropped (INVALID_TOKEN).
        const Endpoint moved{kPeer.address, static_cast<u16>(kPeer.port + 1U)};
        CHECK(server.classify(second, moved).reason == DropReason::InvalidRetryToken);
        CHECK(server.classify(second, kPeer, kSecond, kUnix + 31U).reason == DropReason::InvalidRetryToken);
        Datagram damaged = second;
        damaged.bytes[1U + 4U + 1U + 16U + 1U + 4U + 1U + 10U] ^= 0x01U;
        CHECK(server.classify(damaged).reason == DropReason::InvalidRetryToken);
    }
}

TEST_CASE("S3 takes a NEW_TOKEN token once, and treats a bad one as none", "[net][ingress]")
{
    HandshakeRules rules = tight_rules();
    rules.always_retry = true;
    Server server(rules);
    u8 token[crd::crypto::kQuicNewTokenBytes] = {};
    usize token_bytes = 0U;
    REQUIRE(server.tokens.mint_new_token(QuicVersion::V1, crd::crypto::QuicTokenPeer{kPeer.address.view(), 0U},
                                         kUnix - 100U, Span<u8>(token), token_bytes) == CryptoError::None);
    const Datagram datagram =
        initial(kV1, view_of(kClientDcid, 8U), view_of(kClientScid, 4U), view_of(token, token_bytes));
    const IngressDecision first = server.classify(datagram);
    REQUIRE(first.action == IngressAction::Admit);
    CHECK(first.validated);
    CHECK(server.classify(datagram).action == IngressAction::Retry); // replayed: no proof, so a Retry

    token[token_bytes - 1U] ^= 0x80U;
    const Datagram forged =
        initial(kV1, view_of(kClientDcid, 8U), view_of(kClientScid, 4U), view_of(token, token_bytes));
    CHECK(server.classify(forged).action == IngressAction::Retry);

    // Without a replay filter, NEW_TOKEN tokens are not read at all.
    Server no_filter(rules, IngressConfig{}, true, false);
    token[token_bytes - 1U] ^= 0x80U;
    const Datagram unread =
        initial(kV1, view_of(kClientDcid, 8U), view_of(kClientScid, 4U), view_of(token, token_bytes));
    CHECK(no_filter.classify(unread).action == IngressAction::Retry);
}

TEST_CASE("S3 drops what it can neither admit nor answer", "[net][ingress]")
{
    // Without token keys there is no Retry: a closed bucket drops.
    Server tokenless(tight_rules(), IngressConfig{}, false, false);
    u32 admitted = 0U;
    for (u32 attempt = 0U; attempt < 6U; ++attempt)
    {
        const IngressDecision decision =
            tokenless.classify(initial(kV1, view_of(kClientDcid, 8U), view_of(kClientScid, 4U)));
        admitted += (decision.action == IngressAction::Admit) ? 1U : 0U;
        if (decision.action != IngressAction::Admit)
        {
            CHECK(decision.reason == DropReason::Overloaded);
        }
    }
    CHECK(admitted == 3U);
    CHECK(dropped(tokenless.ingress, DropReason::Overloaded) == 3U);
    IngressDecision unanswerable;
    unanswerable.action = IngressAction::Retry;
    u8 answer[kQuicMaxStatelessAnswerBytes] = {};
    CHECK(tokenless.ingress.answer(unanswerable, kPeer, kUnix, Span<u8>(answer)) == 0U);

    // A spent answer rate drops what would have been answered.
    HandshakeRules rules = tight_rules();
    rules.always_retry = true;
    rules.answers = GateRule{1U, 0U};
    Server quiet(rules);
    const Datagram datagram = initial(kV1, view_of(kClientDcid, 8U), view_of(kClientScid, 4U));
    CHECK(quiet.classify(datagram).action == IngressAction::Retry);
    CHECK(quiet.classify(datagram).reason == DropReason::Overloaded);
    Datagram other = datagram;
    crd::containers::wire_store<u32>(Span<u8>(other.bytes), 1U, 0x5A6A7A8AU);
    CHECK(quiet.classify(other).reason == DropReason::Overloaded);
    CHECK(quiet.classify(other, kPeer, 3U * kSecond).action == IngressAction::NegotiateVersion);

    // A validated address over the proven rate is refused outright.
    rules = tight_rules();
    rules.validated = GateRule{1U, 0U};
    Server proven(rules);
    u8 token[crd::crypto::kQuicNewTokenBytes] = {};
    usize token_bytes = 0U;
    for (u32 attempt = 0U; attempt < 2U; ++attempt)
    {
        REQUIRE(proven.tokens.mint_new_token(QuicVersion::V1, crd::crypto::QuicTokenPeer{kPeer.address.view(), 0U},
                                             kUnix, Span<u8>(token), token_bytes) == CryptoError::None);
        const IngressDecision decision = proven.classify(
            initial(kV1, view_of(kClientDcid, 8U), view_of(kClientScid, 4U), view_of(token, token_bytes)));
        CHECK(decision.action == ((attempt == 0U) ? IngressAction::Admit : IngressAction::Drop));
    }
    CHECK(dropped(proven.ingress, DropReason::Overloaded) == 1U);
}

TEST_CASE("A short header for no connection draws a stateless reset shorter than itself", "[net][ingress]")
{
    constexpr u8 reset_key[crd::crypto::kQuicResetKeyBytes] = {0x31, 0x32, 0x33};
    crd::crypto::QuicResetKey key;
    REQUIRE(key.init(ConstSpan<u8>(reset_key)) == CryptoError::None);
    CountingAllocator allocator;
    ConnectionIdTable routes(allocator, 8U, ConstSpan<u8>(kRouteKey));
    HandshakeRules rules = tight_rules();
    HandshakeBuckets buckets(allocator, rules, ConstSpan<u8>(kRouteKey));
    Ingress ingress(IngressConfig{}, routes, buckets, nullptr, nullptr, &key);
    REQUIRE(ingress.ready());
    const u8 unknown[16] = {0x77, 0x01};
    u8 token[crd::crypto::kQuicResetTokenBytes];
    REQUIRE(key.token(view_of(unknown, 16U), Span<u8>(token)) == CryptoError::None);

    usize lengths[kQuicMaxResetAnswerBytes + 1U] = {};
    const usize triggers[] = {1U + 16U + 20U, 43U, 44U, 45U, 64U, 129U, 130U, 1200U, 1500U};
    for (const usize trigger : triggers)
    {
        for (u32 round = 0U; round < 64U; ++round)
        {
            const IngressDecision decision = ingress.classify(short_packet(view_of(unknown, 16U), trigger).view(),
                                                              false, kPeer, kSecond + round, kUnix);
            REQUIRE(decision.action == IngressAction::StatelessReset);
            CHECK(decision.datagram_bytes == trigger);
            u8 answer[kQuicMaxStatelessAnswerBytes] = {};
            const usize written = ingress.answer(decision, kPeer, kUnix, Span<u8>(answer));
            REQUIRE(written >= kQuicMinResetBytes);
            CHECK(written < trigger); // a loop of resets shrinks (RFC 9000 section 10.3.3)
            CHECK(written <= kQuicMaxResetAnswerBytes);
            if (trigger <= 43U)
            {
                CHECK(written == trigger - 1U);
            }
            else
            {
                CHECK(written >= 43U);
            }
            CHECK((answer[0] & 0xC0U) == 0x40U); // a short header's fixed bits
            CHECK(std::memcmp(answer + written - sizeof(token), token, sizeof(token)) == 0);
            lengths[written] += (trigger == 1500U) ? 1U : 0U;
        }
    }
    // Long triggers draw lengths spread over 43 to kQuicMaxResetAnswerBytes, not one fixed size.
    usize distinct = 0U;
    for (const usize count : lengths)
    {
        distinct += (count != 0U) ? 1U : 0U;
    }
    CHECK(distinct >= 16U);
    CHECK(ingress.counters().resets == 64U * (sizeof(triggers) / sizeof(triggers[0])));

    // An answer too small for the reset, or a decision built without a key, writes nothing.
    const IngressDecision decision =
        ingress.classify(short_packet(view_of(unknown, 16U), 1200U).view(), false, kPeer, 2U * kSecond, kUnix);
    u8 tiny[kQuicMinResetBytes] = {};
    CHECK(ingress.answer(decision, kPeer, kUnix, Span<u8>(tiny)) == 0U);
    Ingress keyless(IngressConfig{}, routes, buckets, nullptr, nullptr);
    u8 answer[kQuicMaxStatelessAnswerBytes] = {};
    CHECK(keyless.answer(decision, kPeer, kUnix, Span<u8>(answer)) == 0U);
    CHECK(keyless.classify(short_packet(view_of(unknown, 16U)).view(), false, kPeer, 2U * kSecond, kUnix).reason ==
          DropReason::UnknownConnection);

    // A routed ID is never reset; the answer rate bounds the resets.
    REQUIRE(routes.insert(view_of(unknown, 16U), 3U) == RouteError::None);
    CHECK(ingress.classify(short_packet(view_of(unknown, 16U)).view(), false, kPeer, 2U * kSecond, kUnix).action ==
          IngressAction::Route);
    rules.answers = GateRule{1U, 0U};
    HandshakeBuckets one_answer(allocator, rules, ConstSpan<u8>(kRouteKey));
    Ingress quiet(IngressConfig{}, routes, one_answer, nullptr, nullptr, &key);
    const u8 other[16] = {0x78};
    CHECK(quiet.classify(short_packet(view_of(other, 16U)).view(), false, kPeer, kSecond, kUnix).action ==
          IngressAction::StatelessReset);
    CHECK(quiet.classify(short_packet(view_of(other, 16U)).view(), false, kPeer, kSecond, kUnix).reason ==
          DropReason::UnknownConnection);
}

} // namespace
