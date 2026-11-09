// crd-net -- QUIC transport parameters and compatible version negotiation (4.2a;
// docs/design/network-foundation.md#quic-transport-parameters): RFC 9001's sample client parameters, round trips of
// every parameter with the reserved parameter and version written each time, every refusal of RFC 9000 section 18,
// RFC 9287 and RFC 9368 section 4, the connection ID authentication of section 7.3 and RFC 9368's downgrade checks.

#include "ingress_fixture.hpp"

#include <crd/containers/array.hpp>
#include <crd/net/quic_transport_parameters.hpp>
#include <crd/net/register_ids.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <random>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireReader;
using crd::containers::WireWriter;
using crd::crypto::QuicSide;
using namespace crd::net;
using namespace crd::net::testing;

constexpr u32 kV1 = iana::kQuicVersion1;
constexpr u32 kV2 = iana::kQuicVersion2;

// RFC 9001 appendix A.2: the quic_transport_parameters extension's value in the client's ClientHello.
constexpr const char* kClientParameters =
    "0408ffffffffffffffff05048000ffff07048000ffff0801100104800075300901100f088394c8f03e51570806048000ffff";
constexpr u8 kClientDcid[8] = {0x83, 0x94, 0xC8, 0xF0, 0x3E, 0x51, 0x57, 0x08};

// Transport parameters written field by field.
struct Encoded
{
    u8    bytes[600] = {};
    usize size       = 0U;

    void add(u64 id, ConstSpan<u8> value)
    {
        WireWriter out(Span<u8>(bytes + size, sizeof(bytes) - size));
        put_quic_varint(out, id);
        put_quic_varint(out, value.size());
        out.put_bytes(value);
        size += out.written();
    }

    void add_varint(u64 id, u64 value)
    {
        u8 encoded[8] = {};
        WireWriter out(Span<u8>(encoded, sizeof(encoded)));
        put_quic_varint(out, value);
        add(id, ConstSpan<u8>(encoded, out.written()));
    }

    void add_versions(std::initializer_list<u32> versions)
    {
        u8 encoded[80] = {};
        WireWriter out(Span<u8>(encoded, sizeof(encoded)));
        for (const u32 version : versions)
        {
            out.put_u32(version);
        }
        add(iana::kQuicTpVersionInformation, ConstSpan<u8>(encoded, out.written()));
    }

    [[nodiscard]] ConstSpan<u8> view() const { return ConstSpan<u8>(bytes, size); }
};

constexpr u64 id(QuicTransportParameterId value)
{
    return static_cast<u64>(value);
}

QuicConnectionId connection_id(std::initializer_list<u8> bytes)
{
    u8 storage[crd::crypto::kQuicMaxConnectionIdBytes] = {};
    usize size = 0U;
    for (const u8 byte : bytes)
    {
        storage[size++] = byte;
    }
    QuicConnectionId out;
    REQUIRE(out.assign(ConstSpan<u8>(storage, size)));
    return out;
}

// A server's parameters with every field away from its default.
QuicTransportParameters full_server()
{
    QuicTransportParameters p;
    p.has_original_destination_connection_id = true;
    p.original_destination_connection_id = connection_id({0x83, 0x94, 0xC8, 0xF0, 0x3E, 0x51, 0x57, 0x08});
    p.max_idle_timeout = 30000U;
    p.has_stateless_reset_token = true;
    for (usize index = 0U; index < kQuicResetTokenBytes; ++index)
    {
        p.stateless_reset_token[index] = static_cast<u8>(0xA0U + index);
    }
    p.max_udp_payload_size = 1472U;
    p.initial_max_data = 1U << 20U;
    p.initial_max_stream_data_bidi_local = 1U << 18U;
    p.initial_max_stream_data_bidi_remote = 1U << 17U;
    p.initial_max_stream_data_uni = 1U << 16U;
    p.initial_max_streams_bidi = kQuicMaxStreams;
    p.initial_max_streams_uni = 3U;
    p.ack_delay_exponent = kQuicMaxAckDelayExponent;
    p.max_ack_delay = kQuicMaxAckDelayLimit - 1U;
    p.disable_active_migration = true;
    p.has_preferred_address = true;
    p.preferred_address.ipv4[0] = 198U;
    p.preferred_address.ipv4[3] = 9U;
    p.preferred_address.ipv4_port = 4433U;
    p.preferred_address.ipv6[0] = 0x20U;
    p.preferred_address.ipv6[15] = 0x01U;
    p.preferred_address.ipv6_port = 4434U;
    p.preferred_address.connection_id = connection_id({0x51, 0x52, 0x53, 0x54});
    p.preferred_address.reset_token[0] = 0x77U;
    p.active_connection_id_limit = 8U;
    p.has_initial_source_connection_id = true;
    p.initial_source_connection_id = connection_id({0xF0, 0x67, 0xA5, 0x50, 0x2A, 0x42, 0x62, 0xB5});
    p.has_retry_source_connection_id = true;
    p.retry_source_connection_id = connection_id({0x01});
    p.max_datagram_frame_size = 65535U;
    p.grease_quic_bit = true;
    p.reset_stream_at = true;
    p.has_version_information = true;
    p.version_information.chosen = kV2;
    p.version_information.available[0] = kV2;
    p.version_information.available[1] = kV1;
    p.version_information.available_count = 2U;
    return p;
}

bool same_id(const QuicConnectionId& a, const QuicConnectionId& b)
{
    return same(a.view(), b.view());
}

// Every field of `b` equals `a`'s, `b`'s available versions being `a`'s and then one reserved version.
void check_equal(const QuicTransportParameters& a, const QuicTransportParameters& b)
{
    CHECK(a.has_original_destination_connection_id == b.has_original_destination_connection_id);
    CHECK(same_id(a.original_destination_connection_id, b.original_destination_connection_id));
    CHECK(a.max_idle_timeout == b.max_idle_timeout);
    CHECK(a.has_stateless_reset_token == b.has_stateless_reset_token);
    CHECK(std::memcmp(a.stateless_reset_token, b.stateless_reset_token, kQuicResetTokenBytes) == 0);
    CHECK(a.max_udp_payload_size == b.max_udp_payload_size);
    CHECK(a.initial_max_data == b.initial_max_data);
    CHECK(a.initial_max_stream_data_bidi_local == b.initial_max_stream_data_bidi_local);
    CHECK(a.initial_max_stream_data_bidi_remote == b.initial_max_stream_data_bidi_remote);
    CHECK(a.initial_max_stream_data_uni == b.initial_max_stream_data_uni);
    CHECK(a.initial_max_streams_bidi == b.initial_max_streams_bidi);
    CHECK(a.initial_max_streams_uni == b.initial_max_streams_uni);
    CHECK(a.ack_delay_exponent == b.ack_delay_exponent);
    CHECK(a.max_ack_delay == b.max_ack_delay);
    CHECK(a.disable_active_migration == b.disable_active_migration);
    CHECK(a.has_preferred_address == b.has_preferred_address);
    if (a.has_preferred_address)
    {
        const QuicPreferredAddress& x = a.preferred_address;
        const QuicPreferredAddress& y = b.preferred_address;
        CHECK(std::memcmp(x.ipv4, y.ipv4, 4U) == 0);
        CHECK(std::memcmp(x.ipv6, y.ipv6, 16U) == 0);
        CHECK((x.ipv4_port == y.ipv4_port && x.ipv6_port == y.ipv6_port));
        CHECK(same_id(x.connection_id, y.connection_id));
        CHECK(std::memcmp(x.reset_token, y.reset_token, kQuicResetTokenBytes) == 0);
    }
    CHECK(a.active_connection_id_limit == b.active_connection_id_limit);
    CHECK(a.has_initial_source_connection_id == b.has_initial_source_connection_id);
    CHECK(same_id(a.initial_source_connection_id, b.initial_source_connection_id));
    CHECK(a.has_retry_source_connection_id == b.has_retry_source_connection_id);
    CHECK(same_id(a.retry_source_connection_id, b.retry_source_connection_id));
    CHECK(a.max_datagram_frame_size == b.max_datagram_frame_size);
    CHECK(a.grease_quic_bit == b.grease_quic_bit);
    CHECK(a.reset_stream_at == b.reset_stream_at);
    CHECK(a.has_version_information == b.has_version_information);
    if (a.has_version_information)
    {
        const QuicVersionInformation& x = a.version_information;
        const QuicVersionInformation& y = b.version_information;
        CHECK(x.chosen == y.chosen);
        REQUIRE(y.available_count == x.available_count + 1U);
        for (usize index = 0U; index < x.available_count; ++index)
        {
            CHECK(x.available[index] == y.available[index]);
        }
        CHECK(quic_version_reserved(y.available[x.available_count]));
    }
}

// The reserved parameters (31 * N + 27) in `bytes`.
usize reserved_parameters(ConstSpan<u8> bytes)
{
    WireReader in(bytes);
    usize count = 0U;
    while (!in.done())
    {
        u64 parameter = 0U;
        u64 length = 0U;
        ConstSpan<u8> value;
        REQUIRE(take_quic_varint(in, parameter));
        REQUIRE(take_quic_varint(in, length));
        REQUIRE(in.take_bytes(static_cast<usize>(length), value));
        count += (parameter % 31U == 27U) ? 1U : 0U;
    }
    return count;
}

TEST_CASE("QUIC transport parameters: RFC 9001's sample client parameters", "[net][quic-tp]")
{
    u8 bytes[64] = {};
    const usize size = hex(kClientParameters, bytes);
    QuicTransportParameters p;
    REQUIRE(read_quic_transport_parameters(ConstSpan<u8>(bytes, size), QuicSide::Client, p) ==
            QuicTransportError::NoError);
    CHECK(p.initial_max_data == crd::net::kQuicMaxVarint);
    CHECK(p.initial_max_stream_data_bidi_local == 0xFFFFU);
    CHECK(p.initial_max_stream_data_bidi_remote == 0xFFFFU);
    CHECK(p.initial_max_stream_data_uni == 0xFFFFU);
    CHECK(p.initial_max_streams_bidi == 16U);
    CHECK(p.initial_max_streams_uni == 16U);
    CHECK(p.max_idle_timeout == 30000U);
    REQUIRE(p.has_initial_source_connection_id);
    CHECK(same(p.initial_source_connection_id.view(), ConstSpan<u8>(kClientDcid, sizeof(kClientDcid))));
    CHECK(p.max_udp_payload_size == kQuicDefaultMaxUdpPayload);
    CHECK(p.ack_delay_exponent == kQuicDefaultAckDelayExponent);
    CHECK(p.max_ack_delay == kQuicDefaultMaxAckDelay);
    CHECK(p.active_connection_id_limit == kQuicDefaultActiveIdLimit);
    CHECK_FALSE(p.has_version_information);

    // The server's section 7.3 check of this client: its Initial's Source Connection ID.
    QuicConnectionIdExpectation expected;
    expected.peer_source = ConstSpan<u8>(kClientDcid, sizeof(kClientDcid));
    CHECK(quic_check_connection_ids(p, QuicSide::Client, expected) == QuicTransportError::NoError);

    // Written again it reads back the same, with one reserved parameter more.
    u8 out[128] = {};
    const usize written = write_quic_transport_parameters(p, QuicSide::Client, {0x1234U}, Span<u8>(out, sizeof(out)));
    REQUIRE(written != 0U);
    CHECK(reserved_parameters(ConstSpan<u8>(out, written)) == 1U);
    QuicTransportParameters again;
    REQUIRE(read_quic_transport_parameters(ConstSpan<u8>(out, written), QuicSide::Client, again) ==
            QuicTransportError::NoError);
    check_equal(p, again);
}

TEST_CASE("QUIC transport parameters: every field round trips with GREASE from any entropy", "[net][quic-tp]")
{
    std::mt19937_64 random(0x7EA5E5U);
    const QuicTransportParameters server = full_server();
    for (int round = 0; round < 300; ++round)
    {
        // Entropy's extremes first, then random draws.
        u64 entropy = random();
        if (round < 2)
        {
            entropy = (round == 0) ? 0U : ~u64{0};
        }
        const QuicTransportParameterGrease grease{entropy};
        const usize bytes = quic_transport_parameters_bytes(server, QuicSide::Server, grease);
        REQUIRE(bytes != 0U);
        u8 out[400] = {};
        REQUIRE(write_quic_transport_parameters(server, QuicSide::Server, grease, Span<u8>(out, sizeof(out))) ==
                bytes);
        CHECK(write_quic_transport_parameters(server, QuicSide::Server, grease, Span<u8>(out, bytes - 1U)) == 0U);
        CHECK(reserved_parameters(ConstSpan<u8>(out, bytes)) == 1U);
        QuicTransportParameters read;
        REQUIRE(read_quic_transport_parameters(ConstSpan<u8>(out, bytes), QuicSide::Server, read) ==
                QuicTransportError::NoError);
        check_equal(server, read);
    }

    // Defaults write nothing but the reserved parameter.
    const QuicTransportParameters defaults;
    u8 out[32] = {};
    const usize bytes = write_quic_transport_parameters(defaults, QuicSide::Client, {0U}, Span<u8>(out, sizeof(out)));
    CHECK(bytes == 2U); // identifier 27, length 0
    CHECK(reserved_parameters(ConstSpan<u8>(out, bytes)) == 1U);
}

TEST_CASE("QUIC transport parameters: section 18, RFC 9287 and RFC 9368 refusals", "[net][quic-tp]")
{
    const u8 one[1] = {0x01};
    const u8 id21[21] = {};
    const u8 token15[15] = {};
    struct Case
    {
        const char* name;
        Encoded     encoded;
        QuicSide    side;
    };
    crd::containers::Array<Case> cases;
    const auto add = [&](const char* name, QuicSide side) -> Encoded&
    {
        cases.push_back(Case{name, Encoded{}, side});
        return cases.back().encoded;
    };
    {
        Encoded& e = add("truncated length", QuicSide::Client);
        e.add_varint(id(QuicTransportParameterId::MaxIdleTimeout), 5U);
        e.size -= 1U;
    }
    {
        Encoded& e = add("varint short of its length", QuicSide::Client);
        const u8 value[2] = {0x05, 0x00};
        e.add(id(QuicTransportParameterId::MaxIdleTimeout), ConstSpan<u8>(value, 2U));
    }
    {
        Encoded& e = add("repeated", QuicSide::Client);
        e.add_varint(id(QuicTransportParameterId::InitialMaxData), 1U);
        e.add_varint(id(QuicTransportParameterId::InitialMaxData), 1U);
    }
    {
        Encoded& e = add("repeated version_information", QuicSide::Server);
        e.add_versions({kV1, kV1});
        e.add_versions({kV1, kV1});
    }
    add("original_destination_connection_id from a client", QuicSide::Client)
        .add(id(QuicTransportParameterId::OriginalDestinationConnectionId), ConstSpan<u8>(one, 1U));
    add("stateless_reset_token from a client", QuicSide::Client)
        .add(id(QuicTransportParameterId::StatelessResetToken), ConstSpan<u8>(id21, 16U));
    add("retry_source_connection_id from a client", QuicSide::Client)
        .add(id(QuicTransportParameterId::RetrySourceConnectionId), ConstSpan<u8>(one, 1U));
    add("preferred_address from a client", QuicSide::Client)
        .add(id(QuicTransportParameterId::PreferredAddress), ConstSpan<u8>(id21, 21U));
    add("max_udp_payload_size 1199", QuicSide::Client)
        .add_varint(id(QuicTransportParameterId::MaxUdpPayloadSize), 1199U);
    add("ack_delay_exponent 21", QuicSide::Client).add_varint(id(QuicTransportParameterId::AckDelayExponent), 21U);
    add("max_ack_delay 2^14", QuicSide::Client).add_varint(id(QuicTransportParameterId::MaxAckDelay), 1U << 14U);
    add("active_connection_id_limit 1", QuicSide::Client)
        .add_varint(id(QuicTransportParameterId::ActiveConnectionIdLimit), 1U);
    add("initial_max_streams_bidi 2^60 + 1", QuicSide::Client)
        .add_varint(id(QuicTransportParameterId::InitialMaxStreamsBidi), kQuicMaxStreams + 1U);
    add("initial_max_streams_uni 2^60 + 1", QuicSide::Client)
        .add_varint(id(QuicTransportParameterId::InitialMaxStreamsUni), kQuicMaxStreams + 1U);
    add("a 21-byte connection ID", QuicSide::Client)
        .add(id(QuicTransportParameterId::InitialSourceConnectionId), ConstSpan<u8>(id21, 21U));
    add("a 15-byte reset token", QuicSide::Server)
        .add(id(QuicTransportParameterId::StatelessResetToken), ConstSpan<u8>(token15, 15U));
    add("disable_active_migration with a value", QuicSide::Client)
        .add(id(QuicTransportParameterId::DisableActiveMigration), ConstSpan<u8>(one, 1U));
    add("grease_quic_bit with a value", QuicSide::Client).add(iana::kQuicTpGreaseQuicBit, ConstSpan<u8>(one, 1U));
    add("reset_stream_at with a value", QuicSide::Client).add(iana::kQuicTpResetStreamAt, ConstSpan<u8>(one, 1U));
    add("max_datagram_frame_size empty", QuicSide::Client).add(iana::kQuicTpMaxDatagramFrameSize, ConstSpan<u8>{});
    {
        u8 address[41] = {};
        address[24] = 0U; // a zero-length connection ID
        add("preferred_address without a connection ID", QuicSide::Server)
            .add(id(QuicTransportParameterId::PreferredAddress), ConstSpan<u8>(address, 41U));
        address[24] = 2U;
        add("preferred_address one byte short", QuicSide::Server)
            .add(id(QuicTransportParameterId::PreferredAddress), ConstSpan<u8>(address, 41U));
    }
    add("version_information empty", QuicSide::Server).add(iana::kQuicTpVersionInformation, ConstSpan<u8>{});
    add("version_information of 6 bytes", QuicSide::Server)
        .add(iana::kQuicTpVersionInformation, ConstSpan<u8>(id21, 6U));
    add("chosen version 0", QuicSide::Server).add_versions({0U, kV1});
    add("an available version 0", QuicSide::Server).add_versions({kV1, kV1, 0U});
    add("a client's chosen version unlisted", QuicSide::Client).add_versions({kV2, kV1});
    add("17 available versions", QuicSide::Server)
        .add_versions({kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1, kV1});

    for (const Case& test : cases)
    {
        INFO(test.name);
        QuicTransportParameters out;
        out.max_idle_timeout = 4242U;
        CHECK(read_quic_transport_parameters(test.encoded.view(), test.side, out) ==
              QuicTransportError::TransportParameterError);
        CHECK(out.max_idle_timeout == 4242U); // unchanged
    }

    // What the same bytes may carry from the other side, and unknown parameters repeated or not.
    Encoded accepted;
    accepted.add(id(QuicTransportParameterId::StatelessResetToken), ConstSpan<u8>(id21, 16U));
    accepted.add(0x1BU, ConstSpan<u8>(one, 1U));      // reserved, N = 0
    accepted.add(0x1BU, ConstSpan<u8>{});              // repeated: not read, so not checked
    accepted.add(0xFF00U, ConstSpan<u8>(id21, 21U));   // unknown
    accepted.add_versions({kV2});                      // a server's chosen version need not be listed
    accepted.add(iana::kQuicTpGreaseQuicBit, ConstSpan<u8>{});
    QuicTransportParameters out;
    REQUIRE(read_quic_transport_parameters(accepted.view(), QuicSide::Server, out) == QuicTransportError::NoError);
    CHECK(out.has_stateless_reset_token);
    CHECK(out.grease_quic_bit);
    CHECK(out.version_information.chosen == kV2);
    CHECK(out.version_information.available_count == 0U);
    CHECK(read_quic_transport_parameters(ConstSpan<u8>{}, QuicSide::Client, out) == QuicTransportError::NoError);
}

TEST_CASE("QUIC transport parameters: writers refuse what readers refuse", "[net][quic-tp]")
{
    const auto refused = [](const QuicTransportParameters& p, QuicSide side)
    {
        u8 out[400] = {0xEE};
        return quic_transport_parameters_bytes(p, side, {}) == 0U &&
               write_quic_transport_parameters(p, side, {}, Span<u8>(out, sizeof(out))) == 0U && out[0] == 0xEEU;
    };
    QuicTransportParameters p = full_server();
    CHECK(refused(p, QuicSide::Client)); // server-only parameters
    CHECK_FALSE(refused(p, QuicSide::Server));
    const auto changed = [&](auto&& change)
    {
        QuicTransportParameters copy = full_server();
        change(copy);
        return refused(copy, QuicSide::Server);
    };
    CHECK(changed([](QuicTransportParameters& c) { c.max_udp_payload_size = 1199U; }));
    CHECK(changed([](QuicTransportParameters& c) { c.ack_delay_exponent = 21U; }));
    CHECK(changed([](QuicTransportParameters& c) { c.max_ack_delay = kQuicMaxAckDelayLimit; }));
    CHECK(changed([](QuicTransportParameters& c) { c.active_connection_id_limit = 1U; }));
    CHECK(changed([](QuicTransportParameters& c) { c.initial_max_streams_uni = kQuicMaxStreams + 1U; }));
    CHECK(changed([](QuicTransportParameters& c) { c.initial_max_data = crd::net::kQuicMaxVarint + 1U; }));
    CHECK(changed([](QuicTransportParameters& c) { c.preferred_address.connection_id.bytes = 0U; }));
    CHECK(changed([](QuicTransportParameters& c) { c.initial_source_connection_id.bytes = 21U; }));
    CHECK(changed([](QuicTransportParameters& c) { c.version_information.available_count = 0U; }));
    CHECK(changed([](QuicTransportParameters& c) { c.version_information.chosen = 0U; }));
    CHECK(changed([](QuicTransportParameters& c) { c.version_information.available[1] = 0x1A2A3A4AU; }));
    CHECK(changed([](QuicTransportParameters& c) { c.version_information.available_count = 16U; }));

    QuicTransportParameters client;
    client.has_version_information = true;
    client.version_information.chosen = kV1;
    client.version_information.available[0] = kV2;
    client.version_information.available_count = 1U;
    CHECK(refused(client, QuicSide::Client)); // a client lists its chosen version
    client.version_information.available[1] = kV1;
    client.version_information.available_count = 2U;
    CHECK_FALSE(refused(client, QuicSide::Client));

    const u8 id21[21] = {};
    QuicConnectionId oversized;
    CHECK_FALSE(oversized.assign(ConstSpan<u8>(id21, sizeof(id21))));
    CHECK(oversized.bytes == 0U);
}

TEST_CASE("QUIC transport parameters: connection ID authentication of section 7.3", "[net][quic-tp]")
{
    const u8 dcid[8] = {0x83, 0x94, 0xC8, 0xF0, 0x3E, 0x51, 0x57, 0x08};
    const u8 server_scid[8] = {0xF0, 0x67, 0xA5, 0x50, 0x2A, 0x42, 0x62, 0xB5};
    const u8 retry_scid[1] = {0x01};
    QuicConnectionIdExpectation expected;
    expected.peer_source = ConstSpan<u8>(server_scid, 8U);
    expected.original_destination = ConstSpan<u8>(dcid, 8U);
    expected.retried = true;
    expected.retry_source = ConstSpan<u8>(retry_scid, 1U);
    const QuicTransportParameters server = full_server();
    CHECK(quic_check_connection_ids(server, QuicSide::Server, expected) == QuicTransportError::NoError);

    const auto check = [&](auto&& change, QuicTransportError error)
    {
        QuicTransportParameters copy = full_server();
        QuicConnectionIdExpectation saw = expected;
        change(copy, saw);
        CHECK(quic_check_connection_ids(copy, QuicSide::Server, saw) == error);
    };
    using P = QuicTransportParameters;
    using E = QuicConnectionIdExpectation;
    check([](P& p, E&) { p.has_initial_source_connection_id = false; }, QuicTransportError::TransportParameterError);
    check([](P& p, E&) { p.has_original_destination_connection_id = false; },
          QuicTransportError::TransportParameterError);
    check([](P&, E& e) { e.peer_source = ConstSpan<u8>{}; }, QuicTransportError::TransportParameterError);
    check([](P& p, E& e)
          {
              p.has_preferred_address = false;
              p.initial_source_connection_id.bytes = 0U;
              e.peer_source = ConstSpan<u8>{};
          },
          QuicTransportError::NoError); // zero-length IDs without a preferred address
    check([](P& p, E&) { p.initial_source_connection_id.id[7] ^= 1U; }, QuicTransportError::ProtocolViolation);
    check([](P& p, E&) { p.original_destination_connection_id.bytes = 7U; }, QuicTransportError::ProtocolViolation);
    check([](P& p, E&) { p.has_retry_source_connection_id = false; }, QuicTransportError::ProtocolViolation);
    check([](P&, E& e) { e.retried = false; }, QuicTransportError::ProtocolViolation);
    check([](P& p, E&) { p.retry_source_connection_id.id[0] = 2U; }, QuicTransportError::ProtocolViolation);
    check([](P& p, E& e)
          {
              p.has_retry_source_connection_id = false;
              e.retried = false;
          },
          QuicTransportError::NoError);

    // A server checks only its client's initial_source_connection_id.
    QuicTransportParameters client;
    QuicConnectionIdExpectation from_client;
    from_client.peer_source = ConstSpan<u8>(dcid, 8U);
    CHECK(quic_check_connection_ids(client, QuicSide::Client, from_client) ==
          QuicTransportError::TransportParameterError);
    client.has_initial_source_connection_id = true;
    REQUIRE(client.initial_source_connection_id.assign(ConstSpan<u8>(dcid, 8U)));
    CHECK(quic_check_connection_ids(client, QuicSide::Client, from_client) == QuicTransportError::NoError);
    client.initial_source_connection_id.id[0] ^= 1U;
    CHECK(quic_check_connection_ids(client, QuicSide::Client, from_client) == QuicTransportError::ProtocolViolation);
}

TEST_CASE("QUIC transport parameters: RFC 9368's compatible negotiation and downgrade checks", "[net][quic-tp]")
{
    const u32 reserved = quic_grease_version(0x12345678U);
    CHECK(quic_versions_compatible(kV1, kV2));
    CHECK(quic_versions_compatible(kV2, kV1));
    CHECK(quic_versions_compatible(0x0BADBEEFU, 0x0BADBEEFU));
    CHECK_FALSE(quic_versions_compatible(kV1, 0x0BADBEEFU));

    const u32 prefer_v2[] = {kV2, kV1};
    const u32 prefer_v1[] = {kV1, kV2};
    const u32 offered[] = {reserved, kV1};
    CHECK(quic_select_version(ConstSpan<u32>(prefer_v2, 2U), ConstSpan<u32>(offered, 2U)) == kV1);
    const u32 greased_preference[] = {reserved, kV2};
    CHECK(quic_select_version(ConstSpan<u32>(greased_preference, 2U), ConstSpan<u32>(offered, 2U)) == 0U);

    // A server converts a version 1 first flight to version 2 when the client lists it.
    QuicTransportParameters client;
    client.has_version_information = true;
    client.version_information.chosen = kV1;
    client.version_information.available[0] = kV1;
    client.version_information.available[1] = kV2;
    client.version_information.available[2] = reserved;
    client.version_information.available_count = 3U;
    CHECK(quic_server_select_version(client, kV1, ConstSpan<u32>(prefer_v2, 2U)) == kV2);
    CHECK(quic_server_select_version(client, kV1, ConstSpan<u32>(prefer_v1, 2U)) == kV1);
    const u32 only_v1[] = {kV1};
    CHECK(quic_server_select_version(client, kV1, ConstSpan<u32>(only_v1, 1U)) == kV1);
    const u32 only_v2[] = {kV2};
    CHECK(quic_server_select_version(client, kV1, ConstSpan<u32>(only_v2, 1U)) == kV2);
    QuicTransportParameters silent;
    CHECK(quic_server_select_version(silent, kV1, ConstSpan<u32>(prefer_v2, 2U)) == kV1);
    CHECK(quic_server_select_version(silent, kV1, ConstSpan<u32>(only_v2, 1U)) == 0U);

    CHECK(quic_server_check_version_information(client, kV1) == QuicTransportError::NoError);
    CHECK(quic_server_check_version_information(client, kV2) == QuicTransportError::VersionNegotiationError);
    CHECK(quic_server_check_version_information(silent, kV2) == QuicTransportError::NoError);

    // The client's checks: the chosen version, and after Version Negotiation the server's list.
    QuicTransportParameters server;
    server.has_version_information = true;
    server.version_information.chosen = kV2;
    server.version_information.available[0] = kV2;
    server.version_information.available[1] = kV1;
    server.version_information.available_count = 2U;
    const ConstSpan<u32> v2_first(prefer_v2, 2U);
    const ConstSpan<u32> v1_first(prefer_v1, 2U);
    CHECK(quic_client_check_version_information(server, kV2, kV2, false, v2_first) == QuicTransportError::NoError);
    CHECK(quic_client_check_version_information(server, kV1, kV1, false, v2_first) ==
          QuicTransportError::VersionNegotiationError);
    CHECK(quic_client_check_version_information(server, kV2, kV2, true, v2_first) == QuicTransportError::NoError);
    // The client prefers version 1 and the server lists it: version 2 after Version Negotiation is a downgrade.
    CHECK(quic_client_check_version_information(server, kV2, kV2, true, v1_first) ==
          QuicTransportError::VersionNegotiationError);
    server.version_information.available_count = 0U;
    CHECK(quic_client_check_version_information(server, kV2, kV2, true, v2_first) ==
          QuicTransportError::VersionNegotiationError);
    CHECK(quic_client_check_version_information(server, kV2, kV2, false, v2_first) == QuicTransportError::NoError);
    const u32 not_listed[] = {kV1};
    server.version_information.available_count = 2U;
    CHECK(quic_client_check_version_information(server, kV2, kV2, true, ConstSpan<u32>(not_listed, 1U)) ==
          QuicTransportError::VersionNegotiationError);

    // RFC 9368's figure 1: a client that attempted version 1 after Version Negotiation and was switched to version
    // 2 passes when it would still attempt version 1 from the server's list, and fails when it would not.
    CHECK(quic_client_check_version_information(server, kV2, kV1, true, v1_first) == QuicTransportError::NoError);
    CHECK(quic_client_check_version_information(server, kV2, kV1, true, v2_first) ==
          QuicTransportError::VersionNegotiationError);
    CHECK(quic_client_check_version_information(server, kV2, kV1, false, v2_first) == QuicTransportError::NoError);

    // Section 8: a version 1 server without the parameter reads as choosing and listing version 1.
    CHECK(quic_client_check_version_information(silent, kV1, kV1, false, v2_first) == QuicTransportError::NoError);
    CHECK(quic_client_check_version_information(silent, kV1, kV1, true, v1_first) == QuicTransportError::NoError);
    CHECK(quic_client_check_version_information(silent, kV1, kV1, true, v2_first) == QuicTransportError::NoError);
    CHECK(quic_client_check_version_information(silent, kV2, kV2, true, v2_first) ==
          QuicTransportError::VersionNegotiationError);
}
TEST_CASE("QUIC transport parameters: a server's 0-RTT part of its early data context, and a client's check of the "
          "limits its 0-RTT kept to (RFC 9000 section 7.4.1, RFC 9221 section 3)",
          "[net][quic-tp][quic-early]")
{
    QuicTransportParameters server;
    server.initial_max_data = 1U;
    server.initial_max_stream_data_bidi_local = 2U;
    server.initial_max_stream_data_bidi_remote = 3U;
    server.initial_max_stream_data_uni = 4U;
    server.initial_max_streams_bidi = 5U;
    server.initial_max_streams_uni = 6U;
    server.active_connection_id_limit = 7U;
    server.max_datagram_frame_size = 8U;
    server.max_idle_timeout = 9U;
    server.max_udp_payload_size = 1500U;
    u8 encoded[kQuicEarlyParameterBytes];
    REQUIRE(write_quic_early_parameters(server, Span<u8>(encoded)) == kQuicEarlyParameterBytes);
    CHECK(encoded[0] == 2U); // the format
    WireReader in{ConstSpan<u8>(encoded)};
    u8 format = 0U;
    REQUIRE(in.take_u8(format));
    const u64 order[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 1500U, 0U, 0U};
    for (const u64 expected : order)
    {
        u64 value = 0U;
        REQUIRE(in.take_u64(value));
        CHECK(value == expected);
    }
    CHECK(in.done());
    CHECK(write_quic_early_parameters(server, Span<u8>(encoded, kQuicEarlyParameterBytes - 1U)) == 0U);

    // The same parameters write the same bytes (no grease); each one written changes them; the others do not.
    u8 again[kQuicEarlyParameterBytes];
    QuicTransportParameters same = server;
    same.ack_delay_exponent = 10U;
    same.max_ack_delay = 50U;
    same.grease_quic_bit = true;
    same.has_initial_source_connection_id = true;
    same.initial_source_connection_id.bytes = 4U;
    REQUIRE(write_quic_early_parameters(same, Span<u8>(again)) == kQuicEarlyParameterBytes);
    CHECK(std::memcmp(encoded, again, sizeof(encoded)) == 0);
    u64 QuicTransportParameters::* const written[] = {&QuicTransportParameters::initial_max_data,
                                                      &QuicTransportParameters::initial_max_stream_data_bidi_local,
                                                      &QuicTransportParameters::initial_max_stream_data_bidi_remote,
                                                      &QuicTransportParameters::initial_max_stream_data_uni,
                                                      &QuicTransportParameters::initial_max_streams_bidi,
                                                      &QuicTransportParameters::initial_max_streams_uni,
                                                      &QuicTransportParameters::active_connection_id_limit,
                                                      &QuicTransportParameters::max_datagram_frame_size,
                                                      &QuicTransportParameters::max_idle_timeout,
                                                      &QuicTransportParameters::max_udp_payload_size};
    for (u64 QuicTransportParameters::* const field : written)
    {
        QuicTransportParameters changed = server;
        changed.*field += 1U;
        REQUIRE(write_quic_early_parameters(changed, Span<u8>(again)) == kQuicEarlyParameterBytes);
        CHECK(std::memcmp(encoded, again, sizeof(encoded)) != 0);
    }
    QuicTransportParameters migration = server;
    migration.disable_active_migration = true;
    REQUIRE(write_quic_early_parameters(migration, Span<u8>(again)) == kQuicEarlyParameterBytes);
    CHECK(std::memcmp(encoded, again, sizeof(encoded)) != 0);
    // A server cannot turn reliable resets off under a ticket that remembers them (section 3 of the draft).
    QuicTransportParameters resets = server;
    resets.reset_stream_at = true;
    REQUIRE(write_quic_early_parameters(resets, Span<u8>(again)) == kQuicEarlyParameterBytes);
    CHECK(std::memcmp(encoded, again, sizeof(encoded)) != 0);
    CHECK(quic_check_remembered_parameters(resets, server) == QuicTransportError::ProtocolViolation);
    CHECK(quic_check_remembered_parameters(server, resets) == QuicTransportError::NoError);
    CHECK(quic_check_remembered_parameters(resets, resets) == QuicTransportError::NoError);

    // A client's check: any limit its 0-RTT kept to that the accepted parameters lower is PROTOCOL_VIOLATION.
    CHECK(quic_check_remembered_parameters(server, server) == QuicTransportError::NoError);
    for (usize index = 0U; index < 8U; ++index)
    {
        QuicTransportParameters lowered = server;
        lowered.*written[index] -= 1U;
        CHECK(quic_check_remembered_parameters(server, lowered) == QuicTransportError::ProtocolViolation);
        QuicTransportParameters raised = server;
        raised.*written[index] += 1U;
        CHECK(quic_check_remembered_parameters(server, raised) == QuicTransportError::NoError);
    }
    // What 0-RTT does not keep to may change (RFC 9000 section 7.4.1 lists the values a client never remembers).
    QuicTransportParameters other = server;
    other.max_idle_timeout = 1U;
    other.max_udp_payload_size = 1200U;
    other.ack_delay_exponent = 0U;
    other.max_ack_delay = 1U;
    CHECK(quic_check_remembered_parameters(server, other) == QuicTransportError::NoError);
}
} // namespace
