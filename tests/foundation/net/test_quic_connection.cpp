// crd-net -- a QUIC connection (4.2a; docs/design/network-foundation.md#quic-connection): the handshake carried to
// confirmation in both versions over a clean path and over seeded paths that lose, duplicate and reorder, the
// anti-amplification limit, probes recovering lost flights, HANDSHAKE_DONE resent, key discards, the idle timeout and
// keep-alives, key updates across the connection, closing and draining, an alert's CRYPTO_ERROR at its levels, the
// transport parameter and version checks, a client's Version Negotiation, and, from a raw peer that writes any frame,
// every frame this slice refuses with the error RFC 9000 and RFC 9221 name, the peer's connection IDs and their
// retirement, PATH_CHALLENGE answered and an acknowledgement of a skipped packet number; streams carried both ways
// over hostile paths with a frame ledger far smaller than their load; and datagrams (RFC 9221) beside them, each
// reported once, the largest ones while packets carry ACKs, a peer without them and frames past the size advertised;
// and this side's connection IDs (RFC 9000 sections 5.1 and 19.15 to 19.16): issued to the peer's limit through loss,
// rotated by Retire Prior To, replaced when retired and refused when a retirement names the packet's own ID or an ID
// never issued, with stateless resets (section 10.3) detected only by a used, unretired ID's token, also from the
// ingress of a server that lost the connection; and Retry (section 17.2.5): one taken, every other refused, then,
// behind a Retry-everything ingress over hostile paths, the server's NEW_TOKEN token proving the next connection once,
// and an INVALID_TOKEN close ending a client whose token no longer validates; and compatible version negotiation (RFC
// 9368 section 2.3, RFC 9369 sections 4.1 and 5): a server switching both ways over hostile paths and after a Retry,
// long headers never going back to the original version, no switch where either side does not offer one, a client
// settled by CRYPTO data in the original version or closing on CRYPTO data in it after the switch, parameters that do
// not parse or name another version closing with their transport error, and tokens of the version a connection ran;
// and 0-RTT (RFC 9000 section 7.4.1, RFC 9001 sections 4.1.4, 4.6, 4.9.3 and 5.7, RFC 9002 section 6.4): a resumed
// client's streams and datagrams read by the server before its handshake completes and marked replayable, over
// hostile paths and after a Retry, four rejections resetting the client, and packets held for their keys.

#include "ingress_fixture.hpp"
#include "quic_connection_fixture.hpp"
#include "quic_stream_fixture.hpp"

#include <crd/crypto/tls.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/quic_connection.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <ctime>
#include <memory>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::StringView;
using crd::containers::WireReader;
using namespace crd::crypto;
using namespace crd::net;
using namespace crd::net::testing;

constexpr u8 kClientCid[8] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7};
constexpr u8 kServerCid[16] = {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
                               0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F};
constexpr u8 kInitialDcid[8] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7};
constexpr u8 kLongCid[20] = {0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
                             0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xF3};

bool both_confirmed(QuicConnection& client, QuicConnection& server)
{
    return client.confirmed() && server.confirmed();
}

void connect(QuicConnection& client, u64 now = kStartNs)
{
    REQUIRE(client.ready());
    REQUIRE(client.connect(ConstSpan<u8>(kClientCid, sizeof(kClientCid)),
                           ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), now));
}
} // namespace

TEST_CASE("QUIC connection: the handshake reaches confirmation in both versions over a clean path",
          "[net][4.2a][quic-connection]")
{
    for (const QuicVersion version : kVersions)
    {
        Identity identity;
        CountingAllocator allocator;
        QuicConnection client(allocator, config_for(QuicSide::Client, identity, version, 0U));
        QuicConnection server(allocator, config_for(QuicSide::Server, identity, version, 1U));
        REQUIRE(server.ready());
        const usize allocations = allocator.calls();
        connect(client);
        CHECK(client.state() == QuicConnectionState::Handshaking);
        auto net = std::make_unique<Net>(client, server);
        REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));

        for (QuicConnection* end : {&client, &server})
        {
            CHECK(end->state() == QuicConnectionState::Established);
            CHECK(end->peer_parameters_known());
            CHECK(end->handshake().application_protocol() == StringView("cerid-game/1"));
            CHECK_FALSE(end->protection().can_send(QuicPacketType::Initial));
            CHECK_FALSE(end->protection().can_send(QuicPacketType::Handshake));
            CHECK(end->protection().can_send(QuicPacketType::OneRtt));
            CHECK(end->peer_parameters().grease_quic_bit);
            CHECK(end->peer_parameters().version_information.chosen == static_cast<u32>(version));
        }
        CHECK(server.address_validated());
        CHECK(same(client.peer_cid(), ConstSpan<u8>(kServerCid, sizeof(kServerCid))));
        CHECK(same(server.peer_cid(), ConstSpan<u8>(kClientCid, sizeof(kClientCid))));
        CHECK(net->client_initials_padded);
        CHECK(net->amplification_kept);
        CHECK(allocator.calls() == allocations); // nothing allocated after construction
        CHECK(client.recovery().rtt().sampled());
        // Quiet once settled (the delayed ACKs sent): the next timer is the idle timeout.
        net->advance(net->now + 500U * kMs);
        CHECK(net->in_flight() == 0U);
        CHECK(client.next_timer_ns() >= net->now + 1000U * kMs);
        CHECK(server.next_timer_ns() >= net->now + 1000U * kMs);
    }
}

TEST_CASE("QUIC connection: packets pad to the configured block", "[net][4.2a][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnectionConfig client_config = config_for(QuicSide::Client, identity, QuicVersion::V1, 0U);
    client_config.padding_block = 128U;
    QuicConnection client(allocator, client_config);
    QuicConnection server(allocator, config_for(QuicSide::Server, identity, QuicVersion::V1, 1U));
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
    net->advance(net->now + 500U * kMs);
    client.ping();
    u8 datagram[kDatagram];
    const usize sent = client.send(Span<u8>(datagram, sizeof(datagram)), net->now);
    REQUIRE(sent != 0U);
    CHECK(sent % 128U == 0U);
}

TEST_CASE("QUIC connection: paths that lose, duplicate and reorder still confirm, within the amplification limit",
          "[net][4.2a][quic-connection]")
{
    for (const QuicVersion version : kVersions)
    {
        for (u64 seed = 1U; seed <= 20U; ++seed)
        {
            Identity identity;
            CountingAllocator allocator;
            QuicConnection client(allocator, config_for(QuicSide::Client, identity, version, seed));
            QuicConnection server(allocator, config_for(QuicSide::Server, identity, version, seed + 1U));
            connect(client);
            auto net = std::make_unique<Net>(client, server);
            net->rng.state = seed;
            for (Path& path : net->path)
            {
                path.loss = 200U;
                path.duplicate = 100U;
                path.delay_ns = 20U * kMs;
                path.jitter_ns = 40U * kMs;
            }
            INFO("version " << static_cast<u32>(version) << " seed " << seed);
            REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
            CHECK(client.state() == QuicConnectionState::Established);
            CHECK(server.state() == QuicConnectionState::Established);
            CHECK(net->client_initials_padded);
            CHECK(net->amplification_kept);
        }
    }
}

namespace
{
// Drops every client datagram after the first `pass`.
class DropClientAfter final : public Filter
{
public:
    explicit DropClientAfter(usize pass) : m_pass(pass) {}
    bool drop(usize from, ConstSpan<u8>) override { return from == 0U && m_seen++ >= m_pass; }
    void stop() { m_pass = ~usize{0}; }

private:
    usize m_pass;
    usize m_seen = 0U;
};

// Drops the first `count` datagrams of one end, from the moment `armed` is set.
class DropFirst final : public Filter
{
public:
    DropFirst(usize from, usize count) : m_from(from), m_count(count) {}
    bool armed = true;
    bool drop(usize from, ConstSpan<u8>) override
    {
        if (from != m_from || !armed || m_count == 0U)
        {
            return false;
        }
        --m_count;
        return true;
    }

private:
    usize m_from;
    usize m_count;
};
} // namespace

TEST_CASE("QUIC connection: a server sends at most three times what it received before the address is proven",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnectionConfig client_config = config_for(QuicSide::Client, identity, QuicVersion::V1);
    QuicConnectionConfig server_config = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
    client_config.parameters.max_idle_timeout = 300000U; // the client's probes back off for a long time
    server_config.parameters.max_idle_timeout = 300000U;
    QuicConnection client(allocator, client_config);
    QuicConnection server(allocator, server_config);
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    DropClientAfter filter(2U); // the two Initial datagrams of the hybrid ClientHello arrive, nothing after
    net->filter = &filter;
    // The server probes until its credit is spent, then waits for a datagram with no timer of its own.
    net->advance(kStartNs + 20000U * kMs);
    CHECK(net->amplification_kept);
    CHECK_FALSE(server.address_validated());
    CHECK(server.counters().bytes_received == 2U * kQuicMinInitialDatagram);
    CHECK(server.counters().bytes_sent > 2U * kQuicMinInitialDatagram); // its first flight and probes went out
    CHECK(server.counters().bytes_sent <= 3U * server.counters().bytes_received);
    CHECK(server.recovery().loss_timer_ns() == kNeverNs); // blocked: nothing armed until a datagram arrives
    const usize server_sent = net->sent[1];
    net->advance(kStartNs + 25000U * kMs);
    CHECK(net->sent[1] == server_sent);
    // The client's next datagrams arrive: the handshake completes.
    filter.stop();
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }, 300000U * kMs));
    CHECK(net->amplification_kept);
}

TEST_CASE("QUIC connection: probes recover a lost first flight of either end, and HANDSHAKE_DONE is resent",
          "[net][4.2a][quic-connection]")
{
    for (usize from = 0U; from < 2U; ++from)
    {
        Identity identity;
        CountingAllocator allocator;
        QuicConnection client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1));
        QuicConnection server(allocator, config_for(QuicSide::Server, identity, QuicVersion::V1, 1U));
        connect(client);
        auto net = std::make_unique<Net>(client, server);
        DropFirst filter(from, 2U);
        net->filter = &filter;
        if (from == 0U)
        {
            // The probe carries the ClientHello again (RFC 9002 section 6.2.4): the server reads it from the two
            // probe datagrams, before any acknowledgement could have asked for it.
            REQUIRE(net->run_until([&] { return server.peer_parameters_known(); }));
            CHECK(net->sent[0] == 4U);
        }
        REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
        CHECK(net->dropped[from] == 2U);
        CHECK(net->client_initials_padded);
    }
    // The server's datagrams after it completes (HANDSHAKE_DONE among them) are lost until the PTO resends it.
    Identity identity;
    CountingAllocator allocator;
    QuicConnection client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V2));
    QuicConnection server(allocator, config_for(QuicSide::Server, identity, QuicVersion::V2, 1U));
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    DropFirst filter(1U, 3U);
    filter.armed = false;
    net->filter = &filter;
    REQUIRE(net->run_until([&] { return server.confirmed(); }));
    filter.armed = true;
    REQUIRE(net->run_until([&] { return client.confirmed(); }));
    CHECK(net->dropped[1] == 3U);
    net->advance(net->now + 500U * kMs);
    CHECK(server.recovery().pto_count() == 0U); // the acknowledgement reset the backoff
    CHECK(server.recovery().bytes_in_flight() == 0U);
}

TEST_CASE("QUIC connection: the idle timeout closes silently and a keep-alive holds it open",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnectionConfig client_config = config_for(QuicSide::Client, identity, QuicVersion::V1);
    QuicConnectionConfig server_config = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
    client_config.parameters.max_idle_timeout = 5000U;
    server_config.parameters.max_idle_timeout = 2000U; // the smaller one rules (section 10.1)
    QuicConnection client(allocator, client_config);
    QuicConnection server(allocator, server_config);
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));

    // Keep-alives every second keep both open for ten seconds.
    const u64 start = net->now;
    while (net->now < start + 10000U * kMs)
    {
        client.ping();
        REQUIRE(net->run_until([&] { return net->in_flight() == 0U; }));
        net->advance(net->now + 1000U * kMs);
    }
    CHECK(client.state() == QuicConnectionState::Established);
    CHECK(server.state() == QuicConnectionState::Established);

    // Then silence after a last exchange: both close at the timeout, without a frame.
    client.ping();
    REQUIRE(net->run_until([&] { return net->in_flight() == 0U; }));
    const u64 quiet = net->now;
    REQUIRE(net->run_until(
        [&]
        { return client.state() == QuicConnectionState::Closed && server.state() == QuicConnectionState::Closed; }));
    CHECK(client.close_info().idle);
    CHECK(server.close_info().idle);
    CHECK(net->now >= quiet + 1900U * kMs);
    CHECK(net->now < quiet + 5000U * kMs);
}

namespace
{
class DropAll final : public Filter
{
public:
    bool armed = false;
    bool drop(usize, ConstSpan<u8>) override { return armed; }
};
} // namespace

TEST_CASE("QUIC connection: the first ack-eliciting packet sent after a receipt restarts the idle timer",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnectionConfig client_config = config_for(QuicSide::Client, identity, QuicVersion::V1);
    QuicConnectionConfig server_config = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
    client_config.parameters.max_idle_timeout = 2000U;
    server_config.parameters.max_idle_timeout = 2000U;
    QuicConnection client(allocator, client_config);
    QuicConnection server(allocator, server_config);
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    DropAll filter;
    net->filter = &filter;
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
    net->advance(net->now + 200U * kMs);
    filter.armed = true; // nothing arrives from here on
    const u64 quiet = net->now;
    net->advance(quiet + 1500U * kMs);
    client.ping(); // sent 1.5 s into the silence: the client's timer restarts, the server's does not
    const u64 pinged = net->now;
    REQUIRE(net->run_until([&] { return server.state() == QuicConnectionState::Closed; }));
    CHECK(net->now < pinged + 1000U * kMs);
    REQUIRE(net->run_until([&] { return client.state() == QuicConnectionState::Closed; }));
    CHECK(client.close_info().idle);
    CHECK(net->now >= pinged + 1900U * kMs);
}

TEST_CASE("QUIC connection: keys update across the connection and the previous key goes three PTOs later",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnectionConfig client_config = config_for(QuicSide::Client, identity, QuicVersion::V1);
    QuicConnectionConfig server_config = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
    client_config.key_update_interval = 64U;
    server_config.key_update_interval = 64U;
    QuicConnection client(allocator, client_config);
    QuicConnection server(allocator, server_config);
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
    for (usize round = 0U; round < 300U; ++round)
    {
        client.ping();
        server.ping();
        REQUIRE(net->run_until([&] { return net->in_flight() == 0U; }));
    }
    CHECK(client.protection().send_phase() >= 2U);
    CHECK(server.protection().send_phase() >= 2U);
    net->advance(net->now + 10000U * kMs);
    CHECK_FALSE(client.protection().previous_held());
    CHECK_FALSE(server.protection().previous_held());
    CHECK(client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: closing resends at a falling rate, the peer drains, both end after three PTOs",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnection client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1));
    QuicConnection server(allocator, config_for(QuicSide::Server, identity, QuicVersion::V1, 1U));
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
    REQUIRE(net->run_until([&] { return net->in_flight() == 0U; }));

    constexpr u8 bye[] = {'b', 'y', 'e'};
    client.close(0x42U, ConstSpan<u8>(bye, sizeof(bye)), net->now);
    CHECK(client.state() == QuicConnectionState::Closing);
    client.close(0x43U, {}, net->now); // only the first close counts
    REQUIRE(net->run_until([&] { return server.state() == QuicConnectionState::Draining; }));
    CHECK(server.close_info().application);
    CHECK_FALSE(server.close_info().local);
    CHECK(server.close_info().error_code == 0x42U);
    CHECK(same(server.close_reason(), ConstSpan<u8>(bye, sizeof(bye))));
    CHECK(client.close_info().local);

    // Seven datagrams in Closing draw the close again after the 1st, 3rd and 7th.
    usize answers = 0U;
    for (usize i = 0U; i < 7U; ++i)
    {
        u8 junk[kQuicMinInitialDatagram] = {0x40U};
        client.receive(Span<u8>(junk, sizeof(junk)), QuicEcn::NotEct, net->now);
        u8 out[kDatagram];
        while (client.send(Span<u8>(out, sizeof(out)), net->now) != 0U)
        {
            ++answers;
        }
    }
    CHECK(answers == 3U);
    u8 out[kDatagram];
    CHECK(server.send(Span<u8>(out, sizeof(out)), net->now) == 0U); // draining sends nothing
    REQUIRE(net->run_until(
        [&]
        { return client.state() == QuicConnectionState::Closed && server.state() == QuicConnectionState::Closed; }));
    CHECK(client.next_timer_ns() == kNeverNs);
}

TEST_CASE("QUIC connection: an application close before the handshake travels as APPLICATION_ERROR",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnection client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1));
    QuicConnection server(allocator, config_for(QuicSide::Server, identity, QuicVersion::V1, 1U));
    connect(client);
    constexpr u8 secret[] = {'s', 'e', 'c', 'r', 'e', 't'};
    client.close(0x99U, ConstSpan<u8>(secret, sizeof(secret)), kStartNs);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return server.state() == QuicConnectionState::Draining; }));
    CHECK_FALSE(server.close_info().application);
    CHECK(server.close_info().error_code == static_cast<u64>(QuicTransportError::ApplicationError));
    CHECK(server.close_reason().empty());
    CHECK(net->client_initials_padded);
}

TEST_CASE("QUIC connection: an alert closes with CRYPTO_ERROR at its level and the lower ones",
          "[net][4.2a][quic-connection]")
{
    Identity identity(true); // the client pins another key
    CountingAllocator allocator;
    QuicConnection client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1));
    QuicConnection server(allocator, config_for(QuicSide::Server, identity, QuicVersion::V1, 1U));
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return client.state() == QuicConnectionState::Closing; }));
    const TlsAlert alert = client.handshake().alert_sent();
    REQUIRE(alert.present);
    CHECK(client.close_info().error_code == quic_crypto_error(alert.description));

    REQUIRE(net->run_until([&] { return server.state() == QuicConnectionState::Draining; }));
    CHECK(server.close_info().error_code == quic_crypto_error(alert.description));
    CHECK(server.close_info().frame_type == 0x06U); // the CRYPTO frame
    // Before confirmation the close goes at the alert's level (Handshake) and the lower levels this side still holds
    // keys for (an Initial, then padded).
    u8 out[kDatagram] = {};
    usize size = 0U;
    for (usize i = 0U; i < 8U && size == 0U; ++i)
    {
        u8 junk[kQuicMinInitialDatagram] = {0x40U};
        client.receive(Span<u8>(junk, sizeof(junk)), QuicEcn::NotEct, net->now);
        size = client.send(Span<u8>(out, sizeof(out)), net->now);
    }
    REQUIRE(size != 0U);
    const bool initial = client.protection().can_send(QuicPacketType::Initial);
    QuicDatagramPackets packets(Span<u8>(out, size), sizeof(kClientCid));
    QuicHeader header;
    Span<u8> packet;
    REQUIRE(packets.next(header, packet));
    if (initial)
    {
        CHECK(header.type == QuicPacketType::Initial);
        CHECK(size >= kQuicMinInitialDatagram);
        REQUIRE(packets.next(header, packet));
    }
    CHECK(header.type == QuicPacketType::Handshake);
    CHECK_FALSE(packets.next(header, packet));
}

TEST_CASE("QUIC connection: the peer's parameters are checked (connection IDs, version downgrade)",
          "[net][4.2a][quic-connection]")
{
    SECTION("a retry_source_connection_id without a Retry")
    {
        Identity identity;
        CountingAllocator allocator;
        QuicConnection client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1));
        QuicConnection server(allocator, config_for(QuicSide::Server, identity, QuicVersion::V1, 1U));
        connect(client);
        u8 first[kDatagram];
        const usize size = client.send(Span<u8>(first, sizeof(first)), kStartNs);
        REQUIRE(size == kQuicMinInitialDatagram);
        QuicAccept accept;
        accept.local_cid = ConstSpan<u8>(kServerCid, sizeof(kServerCid));
        accept.client_dcid = ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid));
        accept.client_scid = ConstSpan<u8>(kClientCid, sizeof(kClientCid));
        constexpr u8 original[8] = {0xAA};
        accept.original_dcid = ConstSpan<u8>(original, sizeof(original));
        CHECK_FALSE(server.accept(accept, kStartNs)); // without retried, the original must be the Initial's
        accept.retried = true;
        REQUIRE(server.accept(accept, kStartNs));
        server.receive(Span<u8>(first, size), QuicEcn::NotEct, kStartNs);
        auto net = std::make_unique<Net>(client, server);
        REQUIRE(net->run_until([&] { return client.state() == QuicConnectionState::Closing; }));
        CHECK(client.close_info().error_code == static_cast<u64>(QuicTransportError::ProtocolViolation));
    }
    SECTION("a downgrade after Version Negotiation")
    {
        Identity identity;
        CountingAllocator allocator;
        constexpr u32 preference[] = {static_cast<u32>(QuicVersion::V2), static_cast<u32>(QuicVersion::V1)};
        QuicConnectionConfig client_config = config_for(QuicSide::Client, identity, QuicVersion::V1);
        client_config.versions = preference;
        client_config.after_version_negotiation = true;
        QuicConnectionConfig server_config = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
        server_config.versions = preference; // it would have run version 2: the client was misled
        QuicConnection client(allocator, client_config);
        QuicConnection server(allocator, server_config);
        connect(client);
        auto net = std::make_unique<Net>(client, server);
        REQUIRE(net->run_until([&] { return client.state() == QuicConnectionState::Closing; }));
        CHECK(client.close_info().error_code == static_cast<u64>(QuicTransportError::VersionNegotiationError));
        REQUIRE(net->run_until([&] { return server.state() == QuicConnectionState::Draining; }));
    }
    SECTION("the same versions after Version Negotiation complete")
    {
        Identity identity;
        CountingAllocator allocator;
        constexpr u32 only[] = {static_cast<u32>(QuicVersion::V1)};
        QuicConnectionConfig client_config = config_for(QuicSide::Client, identity, QuicVersion::V1);
        client_config.after_version_negotiation = true;
        QuicConnectionConfig server_config = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
        server_config.versions = only;
        QuicConnection client(allocator, client_config);
        QuicConnection server(allocator, server_config);
        connect(client);
        auto net = std::make_unique<Net>(client, server);
        REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
    }
}

TEST_CASE("QUIC connection: a client's Version Negotiation packet is read by RFC 9000 section 6.2",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    constexpr u32 preference[] = {static_cast<u32>(QuicVersion::V1), static_cast<u32>(QuicVersion::V2)};
    for (const bool lists_own : {true, false})
    {
        CountingAllocator allocator;
        QuicConnectionConfig config = config_for(QuicSide::Client, identity, QuicVersion::V1);
        config.versions = preference;
        QuicConnection client(allocator, config);
        connect(client);
        const u32 offered[] = {lists_own ? static_cast<u32>(QuicVersion::V1) : static_cast<u32>(QuicVersion::V2)};
        u8 packet[kQuicMaxVersionNegotiationBytes];
        const usize size = write_version_negotiation(ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)),
                                                     ConstSpan<u8>(kClientCid, sizeof(kClientCid)), offered, 0x1234U,
                                                     Span<u8>(packet, sizeof(packet)));
        REQUIRE(size != 0U);
        u8 foreign[kQuicMaxVersionNegotiationBytes];
        std::memcpy(foreign, packet, size);
        foreign[6] ^= 0xFFU; // another client's ID: ignored
        client.receive(Span<u8>(foreign, size), QuicEcn::NotEct, kStartNs);
        CHECK(client.state() == QuicConnectionState::Handshaking);
        client.receive(Span<u8>(packet, size), QuicEcn::NotEct, kStartNs);
        if (lists_own)
        {
            CHECK(client.state() == QuicConnectionState::Handshaking); // it lists the version run: not an answer
        }
        else
        {
            CHECK(client.state() == QuicConnectionState::Closed);
            CHECK(client.close_info().version_negotiation);
            CHECK(client.close_info().retry_version == static_cast<u32>(QuicVersion::V2));
        }
    }
}

namespace
{
// A Retry built by hand and tagged for `original` (RFC 9001 section 5.8), to reach every check a client makes.
Datagram retry_of(QuicVersion version, ConstSpan<u8> dcid, ConstSpan<u8> scid, ConstSpan<u8> token,
                  ConstSpan<u8> original)
{
    Datagram out;
    crd::containers::WireWriter writer(Span<u8>(out.bytes, sizeof(out.bytes)));
    writer.put_u8(static_cast<u8>(0xC0U | (quic_long_type_bits(version, QuicPacketType::Retry) << 4U)));
    writer.put_u32(static_cast<u32>(version));
    writer.put_u8(static_cast<u8>(dcid.size()));
    writer.put_bytes(dcid);
    writer.put_u8(static_cast<u8>(scid.size()));
    writer.put_bytes(scid);
    writer.put_bytes(token);
    const usize body = writer.written();
    REQUIRE(quic_retry_tag(version, original, ConstSpan<u8>(out.bytes, body),
                           Span<u8>(out.bytes + body, kQuicTagBytes)) == CryptoError::None);
    out.size = body + kQuicTagBytes;
    return out;
}

// An ingress that proves every address with a Retry before a handshake, and the keys of its tokens.
struct RetryFront
{
    RetryFront()
        : routes(allocator, 8U), buckets(allocator, rules()),
          ingress(IngressConfig{}, routes, buckets, &tokens, &replay)
    {
        REQUIRE(tokens.init(ConstSpan<u8>(kTokenKey), {}) == CryptoError::None);
        REQUIRE(replay.init(1024U, kQuicNewTokenLifetime, kUnix) == CryptoError::None);
        REQUIRE(ingress.ready());
    }
    static HandshakeRules rules()
    {
        HandshakeRules out;
        out.always_retry = true;
        return out;
    }

    CountingAllocator allocator;
    ConnectionIdTable routes;
    HandshakeBuckets buckets;
    QuicTokenKeys tokens;
    ReplayFilter replay;
    Ingress ingress;
};

void receive_copy(QuicConnection& connection, const Datagram& datagram, u64 now)
{
    Datagram copy = datagram; // receiving opens in place
    connection.receive(Span<u8>(copy.bytes, copy.size), QuicEcn::NotEct, now);
}
} // namespace

TEST_CASE("QUIC connection: a client takes one Retry, for its IDs, with a token and a tag that verifies",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    const ConstSpan<u8> original(kInitialDcid, sizeof(kInitialDcid));
    const ConstSpan<u8> own(kClientCid, sizeof(kClientCid));
    static constexpr u8 kRetryScid[12] = {0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85};
    const ConstSpan<u8> retry_scid(kRetryScid, sizeof(kRetryScid));
    u8 token[kQuicMaxCarriedTokenBytes + 1U];
    std::memset(token, 0x3C, sizeof(token));
    const ConstSpan<u8> short_token(token, 16U);
    const ConstSpan<u8> longest_token(token, kQuicMaxCarriedTokenBytes); // another server's tokens may be this long

    SECTION("Retries it must discard change nothing")
    {
        QuicConnection client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1));
        connect(client);
        u8 out[kDatagram];
        REQUIRE(client.send(Span<u8>(out, sizeof(out)), kStartNs) == kQuicMinInitialDatagram);
        Datagram flipped = retry_of(QuicVersion::V1, own, retry_scid, short_token, original);
        flipped.bytes[flipped.size - 1U] ^= 0x01U;
        const ConstSpan<u8> other(kLongCid, 8U);
        const Datagram refused[] = {
            flipped,
            retry_of(QuicVersion::V1, own, retry_scid, short_token, other),      // a tag for another original ID
            retry_of(QuicVersion::V1, other, retry_scid, short_token, original), // to an ID it never chose
            retry_of(QuicVersion::V1, own, retry_scid, {}, original),            // no token (section 17.2.5.2)
            retry_of(QuicVersion::V2, own, retry_scid, short_token, original),   // another version
            retry_of(QuicVersion::V1, own, original, short_token, original),     // the ID it already sends to
            retry_of(QuicVersion::V1, own, retry_scid, ConstSpan<u8>(token, sizeof(token)), original), // too long
        };
        for (const Datagram& retry : refused)
        {
            const u64 dropped = client.counters().packets_dropped;
            receive_copy(client, retry, kStartNs + kMs);
            CHECK(client.counters().packets_dropped == dropped + 1U);
            CHECK_FALSE(client.retried());
            CHECK(same(client.peer_cid(), original));
        }
        CHECK(client.state() == QuicConnectionState::Handshaking);
    }
    SECTION("an accepted Retry re-keys the Initial, carries the token, keeps the numbers and is taken once")
    {
        for (const QuicVersion version : kVersions)
        {
            QuicConnection client(allocator, config_for(QuicSide::Client, identity, version));
            QuicConnection server(allocator, config_for(QuicSide::Server, identity, version, 1U));
            connect(client);
            u8 first[kDatagram];
            REQUIRE(client.send(Span<u8>(first, sizeof(first)), kStartNs) == kQuicMinInitialDatagram);
            const u64 next = client.next_packet_number(QuicSpace::Initial);
            const u64 at = kStartNs + 20U * kMs;
            receive_copy(client, retry_of(version, own, retry_scid, longest_token, original), at);
            REQUIRE(client.retried());
            CHECK(same(client.peer_cid(), retry_scid));
            CHECK(client.next_packet_number(QuicSpace::Initial) == next); // numbers continue (section 17.2.5.3)
            CHECK(client.recovery().rtt().smoothed_ns == 20U * kMs);      // the first flight's time to the Retry
            CHECK(client.recovery().bytes_in_flight() == 0U);

            u8 again[kDatagram];
            const usize size = client.send(Span<u8>(again, sizeof(again)), at);
            REQUIRE(size >= kQuicMinInitialDatagram);
            QuicHeader header;
            REQUIRE(parse_quic_header(ConstSpan<u8>(again, size), sizeof(kServerCid), header) == QuicHeaderError::None);
            CHECK(header.type == QuicPacketType::Initial);
            CHECK(same(header.dcid, retry_scid));
            CHECK(same(header.token, longest_token));
            CHECK(client.next_packet_number(QuicSpace::Initial) > next);

            const u64 dropped = client.counters().packets_dropped;
            receive_copy(client, retry_of(version, own, ConstSpan<u8>(kLongCid, 12U), short_token, original), at);
            CHECK(client.counters().packets_dropped == dropped + 1U); // one Retry per connection attempt
            CHECK(same(client.peer_cid(), retry_scid));

            // The server opens what the new keys sealed, and its retry_source_connection_id matches.
            QuicAccept accept;
            accept.local_cid = ConstSpan<u8>(kServerCid, sizeof(kServerCid));
            accept.client_dcid = retry_scid;
            accept.client_scid = own;
            accept.original_dcid = original;
            accept.retried = true;
            accept.validated = true;
            REQUIRE(server.accept(accept, at));
            server.receive(Span<u8>(again, size), QuicEcn::NotEct, at);
            auto net = std::make_unique<Net>(client, server);
            net->now = at;
            REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
            CHECK(server.retried());
            CHECK(net->client_initials_padded);
        }
    }
    SECTION("a server whose parameters name no Retry is refused")
    {
        QuicConnection client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1));
        QuicConnection server(allocator, config_for(QuicSide::Server, identity, QuicVersion::V1, 1U));
        connect(client);
        u8 first[kDatagram];
        REQUIRE(client.send(Span<u8>(first, sizeof(first)), kStartNs) != 0U);
        receive_copy(client, retry_of(QuicVersion::V1, own, retry_scid, short_token, original), kStartNs);
        REQUIRE(client.retried());
        u8 again[kDatagram];
        const usize size = client.send(Span<u8>(again, sizeof(again)), kStartNs);
        QuicAccept accept; // as if the Initial had come first: original_destination names the Retry's ID
        accept.local_cid = ConstSpan<u8>(kServerCid, sizeof(kServerCid));
        accept.client_dcid = retry_scid;
        accept.client_scid = own;
        accept.original_dcid = retry_scid;
        REQUIRE(server.accept(accept, kStartNs));
        server.receive(Span<u8>(again, size), QuicEcn::NotEct, kStartNs);
        auto net = std::make_unique<Net>(client, server);
        REQUIRE(net->run_until([&] { return client.state() == QuicConnectionState::Closing; }));
        CHECK(client.close_info().error_code == static_cast<u64>(QuicTransportError::ProtocolViolation));
    }
    SECTION("a server Initial that carries a token is discarded (section 17.2.2)")
    {
        QuicConnection client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1));
        connect(client);
        u8 first[kDatagram];
        REQUIRE(client.send(Span<u8>(first, sizeof(first)), kStartNs) != 0U);
        QuicPacketProtection raw_server;
        REQUIRE(raw_server.start(QuicSide::Server, QuicVersion::V1, original) == CryptoError::None);
        const auto server_initial = [&](ConstSpan<u8> with_token, u64 number)
        {
            Datagram out;
            QuicPacketSpec spec;
            spec.type = QuicPacketType::Initial;
            spec.dcid = own;
            spec.scid = ConstSpan<u8>(kServerCid, sizeof(kServerCid));
            spec.token = with_token;
            spec.number = number;
            QuicPacketWriter writer;
            REQUIRE(writer.begin(spec, Span<u8>(out.bytes, sizeof(out.bytes))));
            writer.frames()[0] = 0x01U; // PING
            REQUIRE(raw_server.seal(writer, 1U, QuicPadding{}, out.size) == QuicPacketError::None);
            return out;
        };
        const u64 dropped = client.counters().packets_dropped;
        receive_copy(client, server_initial(short_token, 0U), kStartNs + kMs);
        CHECK(client.counters().packets_dropped == dropped + 1U);
        CHECK(same(client.peer_cid(), original));
        receive_copy(client, server_initial({}, 1U), kStartNs + kMs); // the same packet without one opens
        CHECK(client.counters().packets_dropped == dropped + 1U);
        CHECK(same(client.peer_cid(), ConstSpan<u8>(kServerCid, sizeof(kServerCid))));
    }
    SECTION("a Retry after the server's first packet is discarded")
    {
        QuicConnection client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1));
        QuicConnection server(allocator, config_for(QuicSide::Server, identity, QuicVersion::V1, 1U));
        connect(client);
        auto net = std::make_unique<Net>(client, server);
        REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
        const u64 dropped = client.counters().packets_dropped;
        receive_copy(client, retry_of(QuicVersion::V1, own, retry_scid, short_token, original), net->now);
        CHECK(client.counters().packets_dropped == dropped + 1U);
        CHECK_FALSE(client.retried());
        CHECK(client.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC connection: behind an ingress that proves every address, Retry and NEW_TOKEN carry clients through "
          "loss",
          "[net][4.2a][quic-connection]")
{
    for (const QuicVersion version : kVersions)
    {
        for (u64 round = 0U; round < 4U; ++round)
        {
            Identity identity;
            CountingAllocator allocator;
            RetryFront front;
            QuicConnection client(allocator, config_for(QuicSide::Client, identity, version));
            QuicConnection server(allocator, config_for(QuicSide::Server, identity, version, 1U));
            connect(client);
            auto net = std::make_unique<Net>(client, server);
            net->ingress = &front.ingress;
            net->tokens = &front.tokens;
            net->rng.state = round;
            if (round != 0U)
            {
                for (Path& path : net->path)
                {
                    path.loss = 150U;
                    path.duplicate = 50U;
                    path.jitter_ns = 8U * kMs;
                }
            }
            REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
            CHECK(client.retried());
            CHECK(server.retried());
            CHECK(net->answers >= 1U);
            CHECK(front.ingress.counters().retries == net->answers);
            CHECK(net->client_initials_padded);
            CHECK(net->amplification_kept);
            // The server's NEW_TOKEN token reaches the client, resent until acknowledged.
            const ConstSpan<u8> minted(net->minted, net->minted_bytes);
            REQUIRE(net->run_until([&] { return same(client.new_token(), minted); }));
            CHECK(same(server.new_token(), minted));

            // The token proves the next connection's address from its first Initial: no Retry.
            u8 kept[kQuicMaxCarriedTokenBytes];
            std::memcpy(kept, minted.data(), minted.size());
            const ConstSpan<u8> token(kept, minted.size());
            const auto reconnect = [&](usize seed)
            {
                QuicConnectionConfig config = config_for(QuicSide::Client, identity, version, seed);
                config.token = token;
                QuicConnection next(allocator, config);
                QuicConnection next_server(allocator, config_for(QuicSide::Server, identity, version, seed + 1U));
                connect(next, net->now);
                auto second = std::make_unique<Net>(next, next_server);
                second->now = net->now;
                second->ingress = &front.ingress;
                REQUIRE(second->run_until([&] { return both_confirmed(next, next_server); }));
                return next.retried();
            };
            const u64 validated = front.ingress.counters().admitted_validated;
            const u64 retries = front.ingress.counters().retries;
            CHECK_FALSE(reconnect(2U));
            CHECK(front.ingress.counters().admitted_validated == validated + 1U);
            CHECK(front.ingress.counters().retries == retries);
            // Its second use is a replay: the address is proven by a Retry again (RFC 9000 section 8.1.4).
            CHECK(reconnect(2U));
        }
    }
}

namespace
{
// Records the version of each datagram's first long header per direction (0 is the client), dropping nothing.
class VersionLog final : public Filter
{
public:
    explicit VersionLog(u32 version) : m_negotiated(version) {}

    bool drop(usize from, ConstSpan<u8> datagram) override
    {
        QuicHeader header;
        const usize short_bytes = (from == 0U) ? sizeof(kServerCid) : sizeof(kClientCid);
        if (parse_quic_header(datagram, short_bytes, header) == QuicHeaderError::None &&
            header.type != QuicPacketType::OneRtt)
        {
            if (header.version == m_negotiated)
            {
                negotiated[from] = true;
            }
            else
            {
                went_back[from] = went_back[from] || negotiated[from];
                ++original[from];
            }
        }
        return false;
    }

    bool negotiated[2] = {};
    bool went_back[2] = {}; // a long header in the original version after one in the negotiated version
    usize original[2] = {};

private:
    u32 m_negotiated = 0U;
};

struct VersionCase
{
    QuicVersion original;
    QuicVersion negotiated;
};
constexpr VersionCase kSwitches[] = {{QuicVersion::V1, QuicVersion::V2}, {QuicVersion::V2, QuicVersion::V1}};

usize collect(QuicConnection& from, Datagram* out, usize room, u64 now)
{
    usize count = 0U;
    for (; count < room; ++count)
    {
        out[count].size = from.send(Span<u8>(out[count].bytes, sizeof(out[count].bytes)), now);
        if (out[count].size == 0U)
        {
            break;
        }
    }
    return count;
}

u32 first_version(const Datagram& datagram)
{
    QuicHeader header;
    REQUIRE(parse_quic_header(datagram.view(), sizeof(kClientCid), header) == QuicHeaderError::None);
    return header.version;
}
} // namespace

TEST_CASE("QUIC connection: a server switches compatibly to the first of its versions its client offers, over "
          "hostile paths (RFC 9368 section 2.3, RFC 9369 section 4.1)",
          "[net][4.2a][quic-connection]")
{
    for (const VersionCase& versions : kSwitches)
    {
        const u32 original = static_cast<u32>(versions.original);
        const u32 negotiated = static_cast<u32>(versions.negotiated);
        const u32 client_versions[] = {original, negotiated};
        const u32 server_versions[] = {negotiated, original};
        for (u64 seed = 0U; seed <= 12U; ++seed)
        {
            Identity identity;
            CountingAllocator allocator;
            QuicConnectionConfig client_config = config_for(QuicSide::Client, identity, versions.original, seed);
            client_config.versions = client_versions;
            QuicConnectionConfig server_config = config_for(QuicSide::Server, identity, versions.original, seed + 1U);
            server_config.versions = server_versions;
            QuicConnection client(allocator, client_config);
            QuicConnection server(allocator, server_config);
            connect(client);
            CHECK(client.version() == versions.original);
            auto net = std::make_unique<Net>(client, server);
            VersionLog log(negotiated);
            net->filter = &log;
            net->rng.state = seed;
            if (seed != 0U)
            {
                for (Path& path : net->path)
                {
                    path.loss = 150U;
                    path.duplicate = 50U;
                    path.jitter_ns = 8U * kMs;
                }
            }
            REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
            for (QuicConnection* end : {&client, &server})
            {
                CHECK(end->version() == versions.negotiated);
                CHECK(end->handshake().version() == versions.negotiated); // its tickets belong to it
                CHECK(end->protection().original_version() == versions.original);
            }
            // The server names the version it switched to; the client the one its first flight chose.
            CHECK(client.peer_parameters().version_information.chosen == negotiated);
            CHECK(server.peer_parameters().version_information.chosen == original);
            CHECK(log.original[0] >= 1U); // the client's first flight
            CHECK(log.negotiated[0]);
            CHECK(log.negotiated[1]);
            CHECK_FALSE(log.went_back[0]);
            CHECK_FALSE(log.went_back[1]);
            CHECK(net->client_initials_padded);
            CHECK(net->amplification_kept);
        }
    }
}

TEST_CASE("QUIC connection: no switch when the client offers one version, the server prefers the original or runs "
          "only it",
          "[net][4.2a][quic-connection]")
{
    for (const VersionCase& versions : kSwitches)
    {
        const u32 original = static_cast<u32>(versions.original);
        const u32 other = static_cast<u32>(versions.negotiated);
        const u32 both[] = {original, other};
        const u32 other_first[] = {other, original};
        const u32 original_first[] = {original, other};
        struct Offer
        {
            ConstSpan<u32> client;
            ConstSpan<u32> server;
        };
        // The client lists its version alone; the server prefers the original; the server runs it alone.
        const Offer offers[] = {
            {{}, ConstSpan<u32>(other_first, 2U)},
            {ConstSpan<u32>(both, 2U), ConstSpan<u32>(original_first, 2U)},
            {ConstSpan<u32>(both, 2U), {}},
        };
        for (const Offer& offer : offers)
        {
            Identity identity;
            CountingAllocator allocator;
            QuicConnectionConfig client_config = config_for(QuicSide::Client, identity, versions.original);
            client_config.versions = offer.client;
            QuicConnectionConfig server_config = config_for(QuicSide::Server, identity, versions.original, 1U);
            server_config.versions = offer.server;
            QuicConnection client(allocator, client_config);
            QuicConnection server(allocator, server_config);
            connect(client);
            auto net = std::make_unique<Net>(client, server);
            VersionLog log(other);
            net->filter = &log;
            REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
            CHECK(client.version() == versions.original);
            CHECK(server.version() == versions.original);
            CHECK(client.peer_parameters().version_information.chosen == original);
            CHECK_FALSE(log.negotiated[0]);
            CHECK_FALSE(log.negotiated[1]);
        }
    }
}

TEST_CASE("QUIC connection: a client settles on CRYPTO data in the original version and closes on it after a switch",
          "[net][4.2a][quic-connection]")
{
    // Two servers answer the same first flight: one stays in version 1, the other switches to version 2. The one
    // that stays reads the client's datagrams one at a time, so its ServerHello has another packet number.
    for (const bool switched_first : {true, false})
    {
        Identity identity;
        CountingAllocator allocator;
        const u32 both[] = {static_cast<u32>(QuicVersion::V1), static_cast<u32>(QuicVersion::V2)};
        const u32 prefer_v2[] = {static_cast<u32>(QuicVersion::V2), static_cast<u32>(QuicVersion::V1)};
        QuicConnectionConfig client_config = config_for(QuicSide::Client, identity, QuicVersion::V1);
        client_config.versions = both;
        QuicConnectionConfig switching_config = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
        switching_config.versions = prefer_v2;
        QuicConnection client(allocator, client_config);
        QuicConnection staying(allocator, config_for(QuicSide::Server, identity, QuicVersion::V1, 2U));
        QuicConnection switching(allocator, switching_config);
        connect(client);
        Datagram flight[4];
        const usize count = collect(client, flight, 4U, kStartNs);
        REQUIRE(count >= 2U); // the hybrid key share spans two datagrams
        QuicAccept accept;
        accept.local_cid = ConstSpan<u8>(kServerCid, sizeof(kServerCid));
        accept.client_dcid = ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid));
        accept.client_scid = ConstSpan<u8>(kClientCid, sizeof(kClientCid));
        accept.original_dcid = accept.client_dcid;
        REQUIRE(staying.accept(accept, kStartNs));
        REQUIRE(switching.accept(accept, kStartNs));
        Datagram stay_out[8];
        usize stay_count = 0U;
        usize hello = 0U; // the staying server's datagram with its ServerHello: the first after the last flight
        for (usize index = 0U; index < count; ++index)
        {
            receive_copy(staying, flight[index], kStartNs);
            hello = stay_count;
            stay_count += collect(staying, stay_out + stay_count, 8U - stay_count, kStartNs);
            receive_copy(switching, flight[index], kStartNs);
        }
        Datagram switch_out[8];
        const usize switch_count = collect(switching, switch_out, 8U, kStartNs);
        REQUIRE(hello >= 1U); // an acknowledgement went before it
        REQUIRE(stay_count > hello);
        REQUIRE(switch_count >= 1U);
        CHECK(switching.version() == QuicVersion::V2);
        CHECK(first_version(switch_out[0]) == static_cast<u32>(QuicVersion::V2));
        CHECK(first_version(stay_out[hello]) == static_cast<u32>(QuicVersion::V1));
        if (switched_first)
        {
            receive_copy(client, switch_out[0], kStartNs + kMs);
            CHECK(client.version() == QuicVersion::V2);
            CHECK(client.state() == QuicConnectionState::Handshaking);
            // The version 1 server's ServerHello opens under the original key, but its CRYPTO frame is a violation.
            receive_copy(client, stay_out[hello], kStartNs + kMs);
            CHECK(client.state() == QuicConnectionState::Closing);
            CHECK(client.close_info().error_code == static_cast<u64>(QuicTransportError::ProtocolViolation));
        }
        else
        {
            receive_copy(client, stay_out[hello], kStartNs + kMs);
            const u64 dropped = client.counters().packets_dropped;
            receive_copy(client, switch_out[0], kStartNs + kMs);
            CHECK(client.version() == QuicVersion::V1);
            CHECK(client.counters().packets_dropped > dropped);
            auto net = std::make_unique<Net>(client, staying);
            net->now = kStartNs + kMs;
            REQUIRE(net->run_until([&] { return both_confirmed(client, staying); }));
            CHECK(client.version() == QuicVersion::V1);
        }
    }
}

TEST_CASE("QUIC connection: after a Retry in the original version the server switches, and its NEW_TOKEN token "
          "proves only a connection in the negotiated version (RFC 9369 sections 4.1 and 5)",
          "[net][4.2a][quic-connection]")
{
    for (const VersionCase& versions : kSwitches)
    {
        const u32 original = static_cast<u32>(versions.original);
        const u32 negotiated = static_cast<u32>(versions.negotiated);
        const u32 client_versions[] = {original, negotiated};
        const u32 server_versions[] = {negotiated, original};
        for (u64 round = 0U; round < 3U; ++round)
        {
            Identity identity;
            CountingAllocator allocator;
            RetryFront front;
            QuicConnectionConfig client_config = config_for(QuicSide::Client, identity, versions.original);
            client_config.versions = client_versions;
            QuicConnectionConfig server_config = config_for(QuicSide::Server, identity, versions.original, 1U);
            server_config.versions = server_versions;
            QuicConnection client(allocator, client_config);
            QuicConnection server(allocator, server_config);
            connect(client);
            auto net = std::make_unique<Net>(client, server);
            net->ingress = &front.ingress;
            net->tokens = &front.tokens;
            net->rng.state = round;
            if (round != 0U)
            {
                for (Path& path : net->path)
                {
                    path.loss = 150U;
                    path.jitter_ns = 8U * kMs;
                }
            }
            REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
            CHECK(client.retried());
            CHECK(server.retried()); // its retry_source_connection_id checked by the client in the new version
            CHECK(client.version() == versions.negotiated);
            CHECK(server.version() == versions.negotiated);
            const ConstSpan<u8> minted(net->minted, net->minted_bytes);
            REQUIRE(net->run_until([&] { return same(client.new_token(), minted); }));
            u8 kept[kQuicMaxCarriedTokenBytes];
            std::memcpy(kept, minted.data(), minted.size());
            const ConstSpan<u8> token(kept, minted.size());
            // The token is of the negotiated version: a connection starting there is proven without a Retry, one
            // starting in the original version is not.
            const auto reconnect = [&](QuicVersion start, usize seed)
            {
                QuicConnectionConfig config = config_for(QuicSide::Client, identity, start, seed);
                config.token = token;
                QuicConnection next(allocator, config);
                QuicConnection next_server(allocator, config_for(QuicSide::Server, identity, start, seed + 1U));
                connect(next, net->now);
                auto second = std::make_unique<Net>(next, next_server);
                second->now = net->now;
                second->ingress = &front.ingress;
                REQUIRE(second->run_until([&] { return both_confirmed(next, next_server); }));
                return next.retried();
            };
            CHECK(reconnect(versions.original, 2U));
            CHECK_FALSE(reconnect(versions.negotiated, 2U));
        }
    }
}

TEST_CASE("QUIC connection: a Retry token the server no longer accepts ends the client at once with INVALID_TOKEN",
          "[net][4.2a][quic-connection]")
{
    for (const QuicVersion version : kVersions)
    {
        Identity identity;
        CountingAllocator allocator;
        RetryFront front;
        QuicConnection client(allocator, config_for(QuicSide::Client, identity, version));
        QuicConnection server(allocator, config_for(QuicSide::Server, identity, version, 1U));
        connect(client);
        auto net = std::make_unique<Net>(client, server);
        net->ingress = &front.ingress;
        REQUIRE(net->run_until([&] { return client.retried(); }));
        // The fleet's token key rotated twice since the Retry: its token no longer opens (RFC 9000 section 8.1.2).
        static constexpr u8 kRotated[kQuicTokenKeyBytes] = {0x99, 0x98, 0x97};
        front.tokens.clear();
        REQUIRE(front.tokens.init(ConstSpan<u8>(kRotated), {}) == CryptoError::None);
        const u64 rotated_at = net->now;
        REQUIRE(net->run_until([&] { return client.state() == QuicConnectionState::Draining; }));
        CHECK(client.close_info().error_code == static_cast<u64>(QuicTransportError::InvalidToken));
        CHECK_FALSE(client.close_info().application);
        CHECK(net->now - rotated_at <= 3U * Path{}.delay_ns); // one round trip, not a handshake timeout
        CHECK(server.state() == QuicConnectionState::Idle);
        CHECK(front.ingress.counters().token_closes >= 1U);
    }
}

TEST_CASE("QUIC connection: configurations and starts out of range are refused", "[net][4.2a][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    const QuicConnectionConfig good = config_for(QuicSide::Client, identity, QuicVersion::V1);
    constexpr u32 other_versions[] = {static_cast<u32>(QuicVersion::V2)};
    constexpr u8 short_seed[4] = {};
    for (usize which = 0U; which < 12U; ++which)
    {
        QuicConnectionConfig config = good;
        switch (which)
        {
            case 0U:
                config.tls = nullptr; // a connection opened keyed: nothing of a handshake is configured for it
                config.token = ConstSpan<u8>(kClientCid, 8U);
                break;
            case 1U:
                config.parameters.initial_max_streams_bidi = 1U;
                break;
            case 2U:
                config.parameters.initial_max_streams_uni = 1U;
                break;
            case 3U:
                config.parameters.max_datagram_frame_size = 1200U;
                break;
            case 4U:
                config.versions = other_versions;
                break;
            case 5U:
                config.crypto_bytes = 4096U + 1U;
                break;
            case 6U:
                config.crypto_bytes = 2048U;
                break;
            case 7U:
                config.max_datagram_bytes = 1199U;
                break;
            case 8U:
                config.sent_packets = 0U;
                break;
            case 9U:
                config.entropy_seed = short_seed;
                break;
            case 10U:
                config.parameters.active_connection_id_limit = kQuicMaxPeerConnectionIds + 1U;
                break;
            default:
                config.version = static_cast<QuicVersion>(0x0A0A0A0AU);
                break;
        }
        INFO("case " << which);
        QuicConnection refused(allocator, config);
        CHECK_FALSE(refused.ready());
        CHECK_FALSE(refused.connect(ConstSpan<u8>(kClientCid, 8U), ConstSpan<u8>(kInitialDcid, 8U), kStartNs));
    }
    allocator.refuse_after(allocator.calls() + 2U);
    QuicConnection starved(allocator, good);
    CHECK_FALSE(starved.ready());
    allocator.refuse_after(~usize{0});

    QuicConnection client(allocator, good);
    QuicAccept accept;
    CHECK_FALSE(client.accept(accept, kStartNs)); // a client accepts nothing
    CHECK_FALSE(client.connect(ConstSpan<u8>(kClientCid, 8U), ConstSpan<u8>(kInitialDcid, 7U), kStartNs));
    connect(client);
    CHECK_FALSE(client.connect(ConstSpan<u8>(kClientCid, 8U), ConstSpan<u8>(kInitialDcid, 8U), kStartNs));
    QuicConnection server(allocator, config_for(QuicSide::Server, identity, QuicVersion::V1));
    CHECK_FALSE(server.connect(ConstSpan<u8>(kClientCid, 8U), ConstSpan<u8>(kInitialDcid, 8U), kStartNs));
    u8 small[kQuicMinInitialDatagram - 1U];
    CHECK(client.send(Span<u8>(small, sizeof(small)), kStartNs) == 0U);
}

// ---- A raw peer writing any frame ----

namespace
{
QuicLevel level_of(QuicPacketType type)
{
    if (type == QuicPacketType::Initial)
    {
        return QuicLevel::Initial;
    }
    return (type == QuicPacketType::Handshake) ? QuicLevel::Handshake : QuicLevel::Application;
}

QuicPacketType type_of(QuicLevel level)
{
    if (level == QuicLevel::Initial)
    {
        return QuicPacketType::Initial;
    }
    return (level == QuicLevel::Handshake) ? QuicPacketType::Handshake : QuicPacketType::OneRtt;
}

// The raw peers' server: room for the thousand packets a skipped number takes to appear.
QuicConnectionConfig server_config(const Identity& identity, crd::u32 sent_packets, const QuicDatagramLimits& datagrams,
                                   QuicConnectionIds* ids, ConstSpan<u32> versions)
{
    QuicConnectionConfig config = config_for(QuicSide::Server, identity, QuicVersion::V1, 2U);
    config.versions = versions;
    config.ids = ids;
    config.sent_packets = sent_packets;
    config.datagrams = datagrams;
    config.sent_frames = 2048U;
    return config;
}

// A client built from crypto::QuicHandshake and QuicPacketProtection directly, which writes whatever frames a test
// names, against a real server connection.
struct RawClient
{
    QuicHandshake handshake;
    QuicPacketProtection protection;
    u64 next[3] = {};
    u64 crypto_sent[3] = {};
    u64 crypto_received[3] = {};
    u64 largest[3] = {};
    bool any[3] = {};
    u8 dcid[20] = {};
    usize dcid_bytes = 0U;
    // What the server sent.
    bool close_seen = false;
    u64 close_code = 0U;
    bool retire_seen = false;
    u64 retire_sequence = 0U;
    bool path_response_seen = false;
    bool seen[4096] = {};      // the server's 1-RTT packet numbers received
    usize eliciting_seen = 0U; // the server's ack-eliciting 1-RTT packets received
    u8 path_response[kQuicPathDataBytes] = {};
    usize path_response_datagram = 0U;
    usize datagrams = 0U;
    // The server's NEW_CONNECTION_ID frames, by sequence.
    u8 issued[8][20] = {};
    u8 issued_tokens[8][kQuicResetTokenBytes] = {};
    usize issued_bytes[8] = {};
    u64 retire_prior_to = 0U;
};

class RawRun
{
public:
    explicit RawRun(const Identity& identity, crd::u32 sent_packets = 2048U, const QuicDatagramLimits& datagrams = {},
                    QuicConnectionIds* ids = nullptr, ConstSpan<u32> versions = {})
        : server(allocator, server_config(identity, sent_packets, datagrams, ids, versions)), m_identity(identity)
    {
    }

    CountingAllocator allocator;
    QuicConnection server;
    RawClient raw;
    u64 now = kStartNs;
    ConstSpan<u8> scid;      // a Source Connection ID other than the client's, when set
    bool pad_initial = true; // false sends an Initial in a datagram under 1200 bytes

    // Runs the handshake to the server's completion; `before_finished` runs with the client holding Handshake keys.
    template <typename Before> void handshake(Before before_finished)
    {
        QuicTransportParameters parameters;
        parameters.max_idle_timeout = 30000U;
        parameters.has_initial_source_connection_id = true;
        REQUIRE(parameters.initial_source_connection_id.assign(ConstSpan<u8>(kClientCid, sizeof(kClientCid))));
        u8 encoded[512];
        const usize bytes =
            write_quic_transport_parameters(parameters, QuicSide::Client, {7U}, Span<u8>(encoded, sizeof(encoded)));
        REQUIRE(bytes != 0U);
        REQUIRE(raw.handshake.init(m_identity.client, ConstSpan<u8>(encoded, bytes)) == TlsError::None);
        REQUIRE(raw.protection.start(QuicSide::Client, QuicVersion::V1,
                                     ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid))) == CryptoError::None);
        std::memcpy(raw.dcid, kInitialDcid, sizeof(kInitialDcid));
        raw.dcid_bytes = sizeof(kInitialDcid);
        QuicAccept accept;
        accept.local_cid = ConstSpan<u8>(kServerCid, sizeof(kServerCid));
        accept.client_dcid = ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid));
        accept.client_scid = ConstSpan<u8>(kClientCid, sizeof(kClientCid));
        accept.original_dcid = accept.client_dcid;
        REQUIRE(server.accept(accept, now));
        send_crypto(QuicLevel::Initial);
        REQUIRE(raw.protection.can_open(QuicPacketType::Handshake));
        before_finished();
        if (server.state() != QuicConnectionState::Handshaking)
        {
            return;
        }
        send_crypto(QuicLevel::Handshake);
        REQUIRE(server.state() == QuicConnectionState::Established);
        REQUIRE(raw.protection.can_open(QuicPacketType::OneRtt));
        raw.protection.confirm();
    }

    void handshake()
    {
        handshake([] {});
    }

    // Sends the handshake's CRYPTO data at `level`, one packet per datagram, and reads what the server answers.
    void send_crypto(QuicLevel level)
    {
        REQUIRE(raw.protection.install(raw.handshake) == CryptoError::None);
        const usize index = static_cast<usize>(level);
        while (raw.handshake.pending(level) > 0U)
        {
            u8 chunk[1000];
            usize taken = 0U;
            REQUIRE(raw.handshake.take(level, Span<u8>(chunk, sizeof(chunk)), taken) == TlsError::None);
            QuicFrame frame;
            frame.kind = QuicFrameKind::Crypto;
            frame.offset = raw.crypto_sent[index];
            frame.data = ConstSpan<u8>(chunk, taken);
            raw.crypto_sent[index] += taken;
            send(type_of(level), [&](Span<u8> out) { return write_quic_frame(frame, out); });
        }
    }

    // One packet of `type` whose frames `write` puts into the span it is given, then the server's answers read.
    template <typename Write> void send(QuicPacketType type, Write write)
    {
        u8 datagram[kDatagram] = {};
        QuicPacketWriter writer;
        QuicPacketSpec spec;
        spec.type = type;
        spec.dcid = ConstSpan<u8>(raw.dcid, raw.dcid_bytes);
        spec.scid = scid.empty() ? ConstSpan<u8>(kClientCid, sizeof(kClientCid)) : scid;
        spec.number = raw.next[static_cast<usize>(quic_space_of(type))]++;
        REQUIRE(writer.begin(spec, Span<u8>(datagram, kQuicMinInitialDatagram)));
        const usize used = write(writer.frames());
        const usize floor = (type == QuicPacketType::Initial && pad_initial) ? kQuicMinInitialDatagram : 0U;
        usize size = 0U;
        REQUIRE(raw.protection.seal(writer, used, {floor, 0U}, size) == QuicPacketError::None);
        server.receive(Span<u8>(datagram, size), QuicEcn::NotEct, now);
        drain();
    }

    // Reads every datagram the server has to send now.
    void drain()
    {
        for (usize guard = 0U; guard < 64U; ++guard)
        {
            u8 datagram[kDatagram];
            const usize size = server.send(Span<u8>(datagram, sizeof(datagram)), now);
            if (size == 0U)
            {
                return;
            }
            ++raw.datagrams;
            read(Span<u8>(datagram, size));
        }
    }

    void read(Span<u8> datagram)
    {
        QuicDatagramPackets packets(datagram, sizeof(kClientCid));
        QuicHeader header;
        Span<u8> packet;
        while (packets.next(header, packet))
        {
            if (header.type != QuicPacketType::OneRtt && raw.dcid_bytes == sizeof(kInitialDcid) &&
                same(ConstSpan<u8>(raw.dcid, raw.dcid_bytes), ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid))))
            {
                std::memcpy(raw.dcid, header.scid.data(), header.scid.size());
                raw.dcid_bytes = header.scid.size();
            }
            QuicOpened opened;
            if (raw.protection.open(header, packet, opened) != QuicPacketError::None)
            {
                continue;
            }
            const usize space = static_cast<usize>(quic_space_of(opened.type));
            raw.largest[space] =
                (raw.any[space] && raw.largest[space] > opened.number) ? raw.largest[space] : opened.number;
            raw.any[space] = true;
            bool eliciting = false;
            WireReader in(opened.payload);
            while (!in.done())
            {
                QuicFrame frame;
                REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
                REQUIRE(quic_frame_allowed(frame.kind, frame.application, opened.type, true));
                eliciting = eliciting || quic_frame_ack_eliciting(frame.kind);
                take(frame, opened.type, datagram.size());
            }
            if (opened.type == QuicPacketType::OneRtt && opened.number < sizeof(raw.seen))
            {
                raw.seen[opened.number] = true;
                raw.eliciting_seen += eliciting ? 1U : 0U;
            }
            REQUIRE(raw.protection.install(raw.handshake) == CryptoError::None);
        }
    }

    // An ACK of exactly the server's 1-RTT packets received, as a peer that acknowledges only ack-eliciting
    // packets sends it once one arrived.
    usize ack_received(Span<u8> out) const
    {
        QuicAckRange ranges[kQuicAckRanges];
        usize count = 0U;
        for (usize number = sizeof(raw.seen); number-- > 0U && count < kQuicAckRanges;)
        {
            if (!raw.seen[number])
            {
                continue;
            }
            if (count > 0U && ranges[count - 1U].smallest == number + 1U)
            {
                ranges[count - 1U].smallest = number;
                continue;
            }
            ranges[count++] = QuicAckRange{number, number};
        }
        return (count == 0U) ? 0U : write_quic_ack(ConstSpan<QuicAckRange>(ranges, count), 0U, nullptr, out);
    }

    // An ACK of every packet number up to `largest`.
    static usize ack(Span<u8> out, u64 largest)
    {
        const QuicAckRange range{0U, largest};
        return write_quic_ack(ConstSpan<QuicAckRange>(&range, 1U), 0U, nullptr, out);
    }

private:
    void take(const QuicFrame& frame, QuicPacketType type, usize datagram_size)
    {
        switch (frame.kind)
        {
            case QuicFrameKind::Crypto:
            {
                const QuicLevel level = level_of(type);
                const usize index = static_cast<usize>(level);
                if (frame.offset == raw.crypto_received[index])
                {
                    raw.crypto_received[index] += frame.data.size();
                    REQUIRE(raw.handshake.provide(level, frame.data) == TlsError::None);
                }
                break;
            }
            case QuicFrameKind::ConnectionClose:
                raw.close_seen = true;
                raw.close_code = frame.error_code;
                break;
            case QuicFrameKind::RetireConnectionId:
                raw.retire_seen = true;
                raw.retire_sequence = frame.sequence;
                break;
            case QuicFrameKind::NewConnectionId:
                REQUIRE(frame.sequence < 8U);
                std::memcpy(raw.issued[frame.sequence], frame.data.data(), frame.data.size());
                std::memcpy(raw.issued_tokens[frame.sequence], frame.reset_token.data(), kQuicResetTokenBytes);
                raw.issued_bytes[frame.sequence] = frame.data.size();
                raw.retire_prior_to = frame.retire_prior_to;
                break;
            case QuicFrameKind::PathResponse:
                raw.path_response_seen = true;
                std::memcpy(raw.path_response, frame.data.data(), kQuicPathDataBytes);
                raw.path_response_datagram = datagram_size;
                break;
            default:
                break;
        }
    }

    const Identity& m_identity;
};

// A frame of `kind` written by the caller's fields, sent in a 1-RTT packet after the handshake: the error the server
// closes with.
template <typename Fill> u64 refused(Fill fill, QuicPacketType type = QuicPacketType::OneRtt)
{
    Identity identity;
    RawRun run(identity);
    run.handshake();
    QuicFrame frame;
    fill(frame);
    run.send(type, [&](Span<u8> out) { return write_quic_frame(frame, out); });
    REQUIRE(run.server.state() == QuicConnectionState::Closing);
    CHECK(run.raw.close_seen); // the CONNECTION_CLOSE reached the raw peer
    CHECK(run.raw.close_code == run.server.close_info().error_code);
    return run.server.close_info().error_code;
}

u64 code(QuicTransportError error)
{
    return static_cast<u64>(error);
}
} // namespace

TEST_CASE("QUIC connection: frames this slice does not carry close with the error the RFCs name",
          "[net][4.2a][quic-connection]")
{
    // A client-initiated stream from the server's view: no stream credit was granted (section 4.6).
    CHECK(refused(
              [](QuicFrame& f)
              {
                  f.kind = QuicFrameKind::Stream;
                  f.stream_id = 0U;
              }) == code(QuicTransportError::StreamLimitError));
    CHECK(refused(
              [](QuicFrame& f)
              {
                  f.kind = QuicFrameKind::ResetStream;
                  f.stream_id = 2U;
              }) == code(QuicTransportError::StreamLimitError));
    // A server-initiated stream the server never opened.
    CHECK(refused(
              [](QuicFrame& f)
              {
                  f.kind = QuicFrameKind::Stream;
                  f.stream_id = 1U;
              }) == code(QuicTransportError::StreamStateError));
    CHECK(refused(
              [](QuicFrame& f)
              {
                  f.kind = QuicFrameKind::MaxStreamData;
                  f.stream_id = 3U;
                  f.value = 9U;
              }) == code(QuicTransportError::StreamStateError));
    // STOP_SENDING and MAX_STREAM_DATA for the client's unidirectional streams, which the server only receives.
    CHECK(refused(
              [](QuicFrame& f)
              {
                  f.kind = QuicFrameKind::StopSending;
                  f.stream_id = 2U;
              }) == code(QuicTransportError::StreamStateError));
    CHECK(refused(
              [](QuicFrame& f)
              {
                  f.kind = QuicFrameKind::MaxStreamData;
                  f.stream_id = 6U;
              }) == code(QuicTransportError::StreamStateError));
    CHECK(refused(
              [](QuicFrame& f)
              {
                  f.kind = QuicFrameKind::StreamDataBlocked;
                  f.stream_id = 4U;
              }) == code(QuicTransportError::StreamLimitError));
    // A retirement of the ID the packet was sent to (the server issued sequence 0 alone), and no PATH_CHALLENGE.
    CHECK(refused(
              [](QuicFrame& f)
              {
                  f.kind = QuicFrameKind::RetireConnectionId;
                  f.sequence = 0U;
              }) == code(QuicTransportError::ProtocolViolation));
    constexpr u8 path_bytes[kQuicPathDataBytes] = {9U};
    CHECK(refused(
              [&](QuicFrame& f)
              {
                  f.kind = QuicFrameKind::PathResponse;
                  f.data = path_bytes;
              }) == code(QuicTransportError::ProtocolViolation));
    // Only a server sends HANDSHAKE_DONE (section 19.20).
    CHECK(refused([](QuicFrame& f) { f.kind = QuicFrameKind::HandshakeDone; }) ==
          code(QuicTransportError::ProtocolViolation));
    // Frames the server accepts change nothing.
    Identity identity;
    RawRun run(identity);
    run.handshake();
    for (const QuicFrameKind kind : {QuicFrameKind::MaxData, QuicFrameKind::DataBlocked, QuicFrameKind::MaxStreams,
                                     QuicFrameKind::StreamsBlocked, QuicFrameKind::Ping})
    {
        QuicFrame frame;
        frame.kind = kind;
        frame.value = 4U;
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(frame, out); });
        CHECK(run.server.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC connection: CRYPTO data past the window and frames out of place close the handshake",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    {
        RawRun run(identity);
        run.handshake(
            [&]
            {
                constexpr u8 byte_bytes[1] = {0x16U};
                QuicFrame frame;
                frame.kind = QuicFrameKind::Crypto;
                frame.offset = u64{1} << 20U;
                frame.data = byte_bytes;
                run.send(QuicPacketType::Handshake, [&](Span<u8> out) { return write_quic_frame(frame, out); });
            });
        CHECK(run.server.close_info().error_code == code(QuicTransportError::CryptoBufferExceeded));
        CHECK(run.raw.close_seen);
    }
    {
        RawRun run(identity);
        run.handshake(
            [&]
            {
                QuicFrame frame;
                frame.kind = QuicFrameKind::MaxData;
                frame.value = 100U;
                run.send(QuicPacketType::Handshake, [&](Span<u8> out) { return write_quic_frame(frame, out); });
            });
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
    {
        // A long header from another Source Connection ID is not this connection's: dropped unread (section 7.2).
        RawRun run(identity);
        constexpr u8 stranger[8] = {0x99U};
        run.handshake(
            [&]
            {
                QuicFrame frame;
                frame.kind = QuicFrameKind::MaxData;
                frame.value = 100U;
                const u64 dropped = run.server.counters().packets_dropped;
                run.scid = ConstSpan<u8>(stranger, sizeof(stranger));
                run.send(QuicPacketType::Handshake, [&](Span<u8> out) { return write_quic_frame(frame, out); });
                run.scid = {};
                CHECK(run.server.state() == QuicConnectionState::Handshaking);
                CHECK(run.server.counters().packets_dropped == dropped + 1U);
            });
        CHECK(run.server.state() == QuicConnectionState::Established);
    }
    {
        // Garbage at the Handshake level fails the TLS handshake with an alert: CRYPTO_ERROR.
        RawRun run(identity);
        run.handshake(
            [&]
            {
                constexpr u8 garbage[8] = {0x14U, 0x00U, 0x00U, 0x04U, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
                QuicFrame frame;
                frame.kind = QuicFrameKind::Crypto;
                frame.offset = 0U;
                frame.data = garbage;
                run.send(QuicPacketType::Handshake, [&](Span<u8> out) { return write_quic_frame(frame, out); });
            });
        CHECK(run.server.close_info().error_code >= static_cast<u64>(QuicTransportError::CryptoErrorFirst));
        CHECK(run.server.close_info().error_code < static_cast<u64>(QuicTransportError::CryptoErrorFirst) + 256U);
    }
}

TEST_CASE("QUIC connection: the peer's connection IDs are held to the limit and retired as asked",
          "[net][4.2a][quic-connection]")
{
    constexpr u8 id1[8] = {0x11U, 1U};
    constexpr u8 id2[8] = {0x22U, 2U};
    constexpr u8 id3[8] = {0x33U, 3U};
    constexpr u8 token1[kQuicResetTokenBytes] = {0xA1U};
    constexpr u8 token2[kQuicResetTokenBytes] = {0xA2U};
    const auto issue = [](RawRun& run, u64 sequence, u64 retire, ConstSpan<u8> id, ConstSpan<u8> token)
    {
        QuicFrame frame;
        frame.kind = QuicFrameKind::NewConnectionId;
        frame.sequence = sequence;
        frame.retire_prior_to = retire;
        frame.data = id;
        frame.reset_token = token;
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(frame, out); });
    };
    Identity identity;
    {
        RawRun run(identity);
        run.handshake();
        issue(run, 1U, 0U, id1, token1);
        CHECK(run.server.peer_cid_count() == 2U);
        issue(run, 1U, 0U, id1, token1); // a repeat
        CHECK(run.server.state() == QuicConnectionState::Established);
        issue(run, 2U, 0U, id2, token2); // a third ID past active_connection_id_limit 2
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ConnectionIdLimitError));
    }
    {
        RawRun run(identity);
        run.handshake();
        issue(run, 1U, 0U, id1, token1);
        issue(run, 1U, 0U, id2, token1); // another ID under a sequence held
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
    {
        RawRun run(identity);
        run.handshake();
        issue(run, 1U, 0U, id1, token1);
        issue(run, 2U, 0U, id1, token2); // the same ID under another sequence
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
    {
        RawRun run(identity);
        run.handshake();
        issue(run, 1U, 0U, id1, token1);
        issue(run, 2U, 0U, id2, token1); // one token under two IDs (section 10.3.2)
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
    {
        RawRun run(identity);
        run.handshake();
        issue(run, 1U, 0U, id1, token1);
        issue(run, 2U, 2U, id2, token2); // retires 0 and 1: the server moves to 2 and says so
        CHECK(run.server.state() == QuicConnectionState::Established);
        CHECK(run.server.peer_cid_count() == 1U);
        CHECK(same(run.server.peer_cid(), ConstSpan<u8>(id2, sizeof(id2))));
        CHECK(run.raw.retire_seen);
        issue(run, 1U, 0U, id3, token1); // below Retire Prior To: retired at once
        CHECK(run.raw.retire_sequence == 1U);
        CHECK(run.server.state() == QuicConnectionState::Established);
        // Retirements owed or unacknowledged stay within twice the limit: the fourth is allowed, the fifth refused.
        issue(run, 1U, 0U, id3, token1);
        CHECK(run.server.state() == QuicConnectionState::Established);
        issue(run, 1U, 0U, id3, token1);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ConnectionIdLimitError));
    }
    {
        // Retiring the ID in use while another stays held: the server moves to the newly issued one.
        RawRun run(identity);
        run.handshake();
        issue(run, 1U, 0U, id1, token1);
        issue(run, 2U, 1U, id2, token2);
        CHECK(run.server.peer_cid_count() == 2U);
        CHECK(same(run.server.peer_cid(), ConstSpan<u8>(id2, sizeof(id2))));
        CHECK(run.raw.retire_sequence == 0U);
    }
}

TEST_CASE("QUIC connection: a PATH_CHALLENGE is answered in a datagram of at least 1200 bytes",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    RawRun run(identity);
    run.handshake();
    constexpr u8 challenge[kQuicPathDataBytes] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    QuicFrame frame;
    frame.kind = QuicFrameKind::PathChallenge;
    frame.data = challenge;
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(frame, out); });
    REQUIRE(run.raw.path_response_seen);
    CHECK(std::memcmp(run.raw.path_response, challenge, sizeof(challenge)) == 0);
    CHECK(run.raw.path_response_datagram >= kQuicMinInitialDatagram);
}

TEST_CASE("QUIC connection: a client Initial in a datagram under 1200 bytes is discarded",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    RawRun run(identity);
    run.handshake(
        [&]
        {
            const u64 dropped = run.server.counters().packets_dropped;
            const usize answers = run.raw.datagrams;
            QuicFrame ping;
            ping.kind = QuicFrameKind::Ping;
            run.pad_initial = false;
            run.send(QuicPacketType::Initial, [&](Span<u8> out) { return write_quic_frame(ping, out); });
            run.pad_initial = true;
            CHECK(run.server.counters().packets_dropped == dropped + 1U);
            CHECK(run.raw.datagrams == answers); // nothing owed, nothing sent
        });
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a run of ACK-only packets carries a PING, so a peer's ACKs free their storage",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnectionConfig server_config = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
    server_config.sent_packets = 24U; // the server only acknowledges: without PINGs its ACK-only packets fill this
    QuicConnection client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1));
    QuicConnection server(allocator, server_config);
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
    for (usize round = 0U; round < 200U; ++round)
    {
        client.ping();
        net->advance(net->now + 30U * kMs);
    }
    net->advance(net->now + 500U * kMs);
    CHECK(server.state() == QuicConnectionState::Established);
    CHECK(server.recovery().can_send(QuicSpace::Application));
    CHECK(client.recovery().bytes_in_flight() == 0U); // every PING acknowledged
    CHECK(client.recovery().pto_count() == 0U);
}

TEST_CASE("QUIC connection: a peer that acknowledges only ack-eliciting packets still frees the ACK-only ones",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    RawRun run(identity, 24U);
    run.handshake();
    usize acknowledged = 0U;
    for (usize round = 0U; round < 200U; ++round)
    {
        const bool owed = run.raw.eliciting_seen > acknowledged;
        acknowledged = run.raw.eliciting_seen;
        run.send(QuicPacketType::OneRtt,
                 [&](Span<u8> out)
                 {
                     const usize ack = owed ? run.ack_received(out) : 0U;
                     QuicFrame ping;
                     ping.kind = QuicFrameKind::Ping;
                     return ack + write_quic_frame(ping, out.subspan(ack));
                 });
        run.now += kMs;
        REQUIRE(run.server.state() == QuicConnectionState::Established);
    }
    CHECK(run.raw.eliciting_seen > 2U); // the HANDSHAKE_DONE packet and the PINGs riding on ACK-only runs
    CHECK(run.server.recovery().can_send(QuicSpace::Application));
}

TEST_CASE("QUIC connection: a space whose storage is full reports no ACK timer it cannot serve",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    RawRun run(identity, 16U); // the raw peer acknowledges nothing
    run.handshake();
    for (usize round = 0U; round < 60U; ++round)
    {
        QuicFrame ping;
        ping.kind = QuicFrameKind::Ping;
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(ping, out); });
        run.now += kMs;
    }
    CHECK_FALSE(run.server.recovery().can_send(QuicSpace::Application));
    CHECK(run.server.recovery().ack_deadline_ns(QuicSpace::Application) <= run.now); // owed, unsendable
    CHECK(run.server.next_timer_ns() > run.now);
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: an acknowledgement of a skipped or unsent packet number is PROTOCOL_VIOLATION",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    {
        // Pings until the server has skipped a 1-RTT number among the ACK-bearing packets it sends.
        RawRun run(identity);
        run.handshake();
        u64 skipped = kQuicNoPacket;
        for (usize round = 0U; round < 2000U && skipped == kQuicNoPacket; ++round)
        {
            QuicFrame ping;
            ping.kind = QuicFrameKind::Ping;
            run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(ping, out); });
            const u64 next = run.server.next_packet_number(QuicSpace::Application);
            for (u64 number = 0U; number < next; ++number)
            {
                skipped = run.server.skipped(QuicSpace::Application, number) ? number : skipped;
            }
        }
        REQUIRE(skipped != kQuicNoPacket);
        const u64 largest = run.raw.largest[2];
        REQUIRE(largest > skipped);
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return RawRun::ack(out, largest); });
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
    {
        RawRun run(identity);
        run.handshake();
        const u64 beyond = run.server.next_packet_number(QuicSpace::Application) + 5U;
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return RawRun::ack(out, beyond); });
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
}

namespace
{
QuicStreamLimits stream_limits()
{
    QuicStreamLimits limits;
    limits.peer_bidi = 4U;
    limits.peer_uni = 2U;
    limits.local_bidi = 4U;
    limits.local_uni = 2U;
    limits.receive_bytes = 16384U;
    limits.send_bytes = 16384U;
    limits.stream_window = 8192U;
    limits.connection_window = 24576U;
    limits.connection_max_window = 131072U;
    return limits;
}

// The client opens `streams` bidirectional streams of seeded sizes as its grant allows; the server echoes each with
// another pattern as it reads, and both ends free what they finish. Called between events; true once every echo was
// read whole.
class Echo
{
public:
    static constexpr usize kStreams = 12U;

    Echo(QuicConnection& client, QuicConnection& server, u64 seed) : m_client(client), m_server(server)
    {
        Rng rng;
        rng.state = seed;
        for (u64& size : m_size)
        {
            size = rng.next() % 30000U;
        }
    }

    bool step()
    {
        QuicStreamSet& c = m_client.streams();
        QuicStreamSet& s = m_server.streams();
        u64 id = 0U;
        while (m_opened < kStreams && c.open(true, id) == QuicStreamError::None)
        {
            REQUIRE(id == 4U * m_opened);
            ++m_opened;
        }
        QuicStreamEvent event;
        while (c.poll(event))
        {
        }
        while (s.poll(event))
        {
        }
        for (usize index = 0U; index < m_opened; ++index)
        {
            const u64 stream = 4U * index;
            const QuicSendState sending = c.send_state(stream);
            if (sending == QuicSendState::Ready || sending == QuicSendState::Send)
            {
                static_cast<void>(write_pattern(c, stream, m_sent[index], m_size[index], stream));
            }
            if (!m_done[index] && c.recv_state(stream) != QuicRecvState::None &&
                drain(c, stream, m_client_read[index], stream + 1U))
            {
                CHECK(m_client_read[index] == m_size[index]);
                m_done[index] = true;
                ++m_finished;
            }
            if (s.recv_state(stream) == QuicRecvState::None)
            {
                continue;
            }
            const bool ended =
                s.recv_state(stream) == QuicRecvState::DataRead || drain(s, stream, m_server_read[index], stream);
            const QuicSendState echo = s.send_state(stream);
            if (echo == QuicSendState::Ready || echo == QuicSendState::Send)
            {
                static_cast<void>(write_pattern(s, stream, m_echoed[index], m_server_read[index], stream + 1U, ended));
            }
        }
        return m_finished == kStreams;
    }

    [[nodiscard]] u64 total() const
    {
        u64 sum = 0U;
        for (const u64 size : m_size)
        {
            sum += size;
        }
        return sum;
    }

private:
    QuicConnection& m_client;
    QuicConnection& m_server;
    u64 m_size[kStreams] = {};
    u64 m_sent[kStreams] = {};
    u64 m_echoed[kStreams] = {};
    u64 m_server_read[kStreams] = {};
    u64 m_client_read[kStreams] = {};
    bool m_done[kStreams] = {};
    usize m_opened = 0U;
    usize m_finished = 0U;
};

QuicConnectionConfig stream_config(QuicSide side, const Identity& identity, QuicVersion version, usize seed,
                                   u32 sent_frames = 1024U)
{
    QuicConnectionConfig config = config_for(side, identity, version, seed);
    config.streams = stream_limits();
    config.sent_frames = sent_frames;
    return config;
}
} // namespace

TEST_CASE("QUIC connection: streams carry data both ways over paths that lose, duplicate and reorder",
          "[net][4.2a][quic-connection]")
{
    for (const QuicVersion version : kVersions)
    {
        for (u64 seed = 1U; seed <= 8U; ++seed)
        {
            Identity identity;
            CountingAllocator allocator;
            QuicConnection client(allocator, stream_config(QuicSide::Client, identity, version, seed));
            QuicConnection server(allocator, stream_config(QuicSide::Server, identity, version, seed + 1U));
            REQUIRE(server.ready());
            const usize allocations = allocator.calls();
            connect(client);
            auto net = std::make_unique<Net>(client, server);
            net->rng.state = seed;
            for (Path& path : net->path)
            {
                path.loss = 100U;
                path.duplicate = 50U;
                path.delay_ns = 15U * kMs;
                path.jitter_ns = 20U * kMs;
            }
            INFO("version " << static_cast<u32>(version) << " seed " << seed);
            REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
            // Each side's grants are the other's limits.
            CHECK(client.peer_parameters().initial_max_streams_bidi == 4U);
            CHECK(client.peer_parameters().initial_max_stream_data_bidi_remote == 8192U);
            CHECK(server.peer_parameters().initial_max_data == 24576U);
            Echo echo(client, server, seed);
            REQUIRE(net->run_until([&] { return echo.step(); }, 600000U * kMs));
            CHECK(client.state() == QuicConnectionState::Established);
            CHECK(server.state() == QuicConnectionState::Established);
            CHECK(client.streams().connection_sent() == echo.total());
            CHECK(server.streams().connection_sent() == echo.total());
            CHECK(client.streams().peer_limit(true) >= Echo::kStreams); // MAX_STREAMS followed the freed slots
            CHECK(allocator.calls() == allocations);
        }
    }
}

TEST_CASE("QUIC connection: a frame ledger far smaller than the stream load still drains",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnection client(allocator, stream_config(QuicSide::Client, identity, QuicVersion::V1, 0U, 12U));
    QuicConnection server(allocator, stream_config(QuicSide::Server, identity, QuicVersion::V1, 1U, 12U));
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    net->path[0].loss = 50U;
    net->path[1].loss = 50U;
    net->rng.state = 7U;
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
    Echo echo(client, server, 3U);
    REQUIRE(net->run_until([&] { return echo.step(); }, 600000U * kMs));
    CHECK(client.state() == QuicConnectionState::Established);
}

namespace
{
QuicDatagramLimits datagram_limits(bool outcomes)
{
    QuicDatagramLimits limits;
    limits.max_frame_bytes = 1200U;
    limits.receive_bytes = 1U << 17U; // a whole flight of the largest arrives between two reads
    limits.receive_count = 256U;
    limits.send_bytes = 16384U;
    limits.send_count = 32U;
    limits.outcomes = outcomes;
    return limits;
}

QuicConnectionConfig datagram_config(QuicSide side, const Identity& identity, QuicVersion version, usize seed,
                                     bool outcomes, u32 sent_frames = 1024U)
{
    QuicConnectionConfig config = stream_config(side, identity, version, seed, sent_frames);
    config.datagrams = datagram_limits(outcomes);
    return config;
}

// One direction of datagrams: `from` queues kCount payloads of seeded sizes (or all of the largest) as its queue
// allows, `to` reads each in place. A payload starts with its id, so the reader checks that it was queued, arrives
// at most once and holds its bytes; with outcomes every id is reported once, and an acknowledged one arrived.
class DatagramFlow
{
public:
    static constexpr usize kCount = 300U;

    DatagramFlow(QuicConnection& from, QuicConnection& to, u64 seed, bool largest)
        : m_from(from), m_to(to), m_largest(largest)
    {
        m_rng.state = seed;
    }

    bool step(u64 now_ns)
    {
        QuicDatagrams& out = m_from.datagrams();
        QuicDatagrams& in = m_to.datagrams();
        u8 payload[kDatagram];
        while (m_queued < kCount && out.sendable())
        {
            const usize largest = out.max_payload();
            const usize bytes = m_largest ? largest : 8U + static_cast<usize>(m_rng.next() % (largest - 7U));
            write(m_queued, bytes, payload);
            u64 id = 0U;
            const QuicDatagramError error = out.queue(ConstSpan<u8>(payload, bytes), now_ns, id);
            if (error == QuicDatagramError::Full)
            {
                break;
            }
            REQUIRE(error == QuicDatagramError::None);
            REQUIRE(id == m_queued);
            m_size[m_queued++] = bytes;
        }
        ConstSpan<u8> held;
        while (in.peek(held))
        {
            REQUIRE(held.size() >= 8U);
            u64 id = 0U;
            for (usize index = 0U; index < 8U; ++index)
            {
                id |= u64{held[index]} << (8U * index);
            }
            REQUIRE(id < m_queued);
            REQUIRE_FALSE(m_arrived[id]); // a packet is opened once: no datagram arrives twice
            write(id, m_size[id], payload);
            REQUIRE(same(held, ConstSpan<u8>(payload, m_size[id])));
            m_arrived[id] = true;
            ++m_arrivals;
            in.pop();
        }
        QuicDatagramReport report;
        while (out.poll(report))
        {
            REQUIRE(report.id < m_queued);
            REQUIRE(m_outcome[report.id] == 0U);
            REQUIRE(report.outcome != QuicDatagramOutcome::Expired); // no age limit
            m_outcome[report.id] = (report.outcome == QuicDatagramOutcome::Acked) ? 1U : 2U;
            m_lost += (report.outcome == QuicDatagramOutcome::Lost) ? 1U : 0U;
            m_acked += (report.outcome == QuicDatagramOutcome::Acked) ? 1U : 0U;
            ++m_reported;
        }
        return out.outcomes() ? m_reported == kCount : m_arrivals == kCount;
    }

    // After the run: every acknowledged datagram arrived, and the reader's ring never dropped one.
    void check() const
    {
        for (usize id = 0U; id < kCount; ++id)
        {
            if (m_outcome[id] == 1U)
            {
                REQUIRE(m_arrived[id]);
            }
        }
        CHECK(m_to.datagrams().counters().dropped == 0U);
        CHECK(m_from.datagrams().counters().sent == kCount);
    }

    [[nodiscard]] usize lost() const { return m_lost; }
    [[nodiscard]] usize acked() const { return m_acked; }
    [[nodiscard]] usize arrivals() const { return m_arrivals; }

private:
    static void write(u64 id, usize bytes, u8* out)
    {
        for (usize index = 0U; index < 8U; ++index)
        {
            out[index] = static_cast<u8>(id >> (8U * index));
        }
        for (usize offset = 8U; offset < bytes; ++offset)
        {
            out[offset] = static_cast<u8>((id * 29U) + (offset * 13U) + 5U);
        }
    }

    QuicConnection& m_from;
    QuicConnection& m_to;
    Rng m_rng;
    bool m_largest = false;
    usize m_size[kCount] = {};
    bool m_arrived[kCount] = {};
    u8 m_outcome[kCount] = {}; // 1 acknowledged, 2 lost
    usize m_queued = 0U;
    usize m_arrivals = 0U;
    usize m_reported = 0U;
    usize m_lost = 0U;
    usize m_acked = 0U;
};
} // namespace

TEST_CASE("QUIC connection: datagrams cross both ways beside streams over hostile paths, each reported once",
          "[net][4.2a][quic-connection][quic-datagram]")
{
    usize lost = 0U;
    usize acked = 0U;
    for (const QuicVersion version : kVersions)
    {
        for (u64 seed = 1U; seed <= 6U; ++seed)
        {
            Identity identity;
            CountingAllocator allocator;
            QuicConnection client(allocator, datagram_config(QuicSide::Client, identity, version, seed, true));
            QuicConnection server(allocator, datagram_config(QuicSide::Server, identity, version, seed + 1U, true));
            REQUIRE(server.ready());
            const usize allocations = allocator.calls();
            u64 id = 0U;
            CHECK(client.datagrams().queue(ConstSpan<u8>(), kStartNs, id) == QuicDatagramError::NotReady);
            connect(client);
            auto net = std::make_unique<Net>(client, server);
            net->rng.state = seed;
            for (Path& path : net->path)
            {
                path.loss = 100U;
                path.duplicate = 50U;
                path.delay_ns = 15U * kMs;
                path.jitter_ns = 20U * kMs;
            }
            INFO("version " << static_cast<u32>(version) << " seed " << seed);
            REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
            CHECK(client.peer_parameters().max_datagram_frame_size == 1200U);
            CHECK(server.peer_parameters().max_datagram_frame_size == 1200U);
            Echo echo(client, server, seed);
            DatagramFlow up(client, server, seed, false);
            DatagramFlow down(server, client, seed + 100U, false);
            REQUIRE(net->run_until(
                [&]
                {
                    const bool streams = echo.step();
                    const bool sent_up = up.step(net->now);
                    const bool sent_down = down.step(net->now);
                    return streams && sent_up && sent_down;
                },
                600000U * kMs));
            up.check();
            down.check();
            lost += up.lost() + down.lost();
            acked += up.acked() + down.acked();
            CHECK(client.state() == QuicConnectionState::Established);
            CHECK(server.state() == QuicConnectionState::Established);
            CHECK(client.streams().connection_sent() == echo.total());
            CHECK(allocator.calls() == allocations);
        }
    }
    CHECK(lost != 0U); // the paths' losses were reported, and reordering before the thresholds rose
    CHECK(acked != 0U);
}

TEST_CASE("QUIC connection: datagrams of the largest size go while the packets carry ACKs",
          "[net][4.2a][quic-connection][quic-datagram]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnection client(allocator, datagram_config(QuicSide::Client, identity, QuicVersion::V1, 0U, false));
    QuicConnection server(allocator, datagram_config(QuicSide::Server, identity, QuicVersion::V1, 1U, false));
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
    // A 1200-byte packet to a 16-byte connection ID still bounds the payload by the 20-byte worst case.
    CHECK(client.datagrams().max_payload() == kQuicMinInitialDatagram - kQuicShortPacketOverhead - 3U);
    Echo echo(client, server, 5U);
    DatagramFlow up(client, server, 1U, true);
    DatagramFlow down(server, client, 2U, true);
    REQUIRE(net->run_until(
        [&]
        {
            const bool streams = echo.step();
            const bool sent_up = up.step(net->now);
            const bool sent_down = down.step(net->now);
            return streams && sent_up && sent_down;
        },
        600000U * kMs));
    up.check();
    down.check();
    CHECK(up.arrivals() == DatagramFlow::kCount);
}

TEST_CASE("QUIC connection: a frame ledger far smaller than the datagram load holds outcomes back and drains",
          "[net][4.2a][quic-connection][quic-datagram]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnection client(allocator, datagram_config(QuicSide::Client, identity, QuicVersion::V1, 0U, true, 12U));
    QuicConnection server(allocator, datagram_config(QuicSide::Server, identity, QuicVersion::V1, 1U, true, 12U));
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    net->path[0].loss = 50U;
    net->path[1].loss = 50U;
    net->rng.state = 3U;
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
    DatagramFlow up(client, server, 3U, false);
    DatagramFlow down(server, client, 4U, false);
    REQUIRE(net->run_until(
        [&]
        {
            const bool sent_up = up.step(net->now);
            const bool sent_down = down.step(net->now);
            return sent_up && sent_down;
        },
        600000U * kMs));
    up.check();
    down.check();
    CHECK(up.acked() > 4U * up.lost());
    CHECK(down.acked() > 4U * down.lost());
}

TEST_CASE("QUIC connection: a peer without datagrams is sent none, and either side without a ring sends none",
          "[net][4.2a][quic-connection][quic-datagram]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnection client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1));
    QuicConnection server(allocator, datagram_config(QuicSide::Server, identity, QuicVersion::V1, 1U, false));
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
    CHECK(client.peer_parameters().max_datagram_frame_size == 1200U);
    CHECK(server.peer_parameters().max_datagram_frame_size == 0U);
    const u8 payload[4] = {1U, 2U, 3U, 4U};
    u64 id = 0U;
    CHECK(client.datagrams().queue(payload, net->now, id) == QuicDatagramError::Unsupported); // no ring
    CHECK(server.datagrams().queue(payload, net->now, id) == QuicDatagramError::Unsupported); // the peer takes none
    CHECK_FALSE(server.datagrams().sendable());
}

TEST_CASE("QUIC connection: a peer's DATAGRAM frame is held to the size advertised, and refused without one",
          "[net][4.2a][quic-connection][quic-datagram]")
{
    // max_datagram_frame_size is 0 (RFC 9221 section 3).
    constexpr u8 data_bytes[3] = {1U, 2U, 3U};
    CHECK(refused(
              [&](QuicFrame& f)
              {
                  f.kind = QuicFrameKind::Datagram;
                  f.data = data_bytes;
              }) == code(QuicTransportError::ProtocolViolation));

    QuicDatagramLimits limits;
    limits.max_frame_bytes = 100U;
    limits.receive_bytes = 99U;
    limits.receive_count = 2U;
    u8 payload[100];
    for (usize index = 0U; index < sizeof(payload); ++index)
    {
        payload[index] = static_cast<u8>(index + 1U);
    }
    Identity identity;
    RawRun run(identity, 2048U, limits);
    run.handshake();
    // Both encodings at exactly 100 bytes arrive whole.
    for (const bool to_end : {false, true})
    {
        QuicFrame frame;
        frame.kind = QuicFrameKind::Datagram;
        frame.to_end = to_end;
        frame.data = ConstSpan<u8>(payload, to_end ? 99U : 97U);
        REQUIRE(quic_frame_bytes(frame) == 100U);
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(frame, out); });
        REQUIRE(run.server.state() == QuicConnectionState::Established);
        ConstSpan<u8> held;
        REQUIRE(run.server.datagrams().peek(held));
        CHECK(same(held, frame.data));
        run.server.datagrams().pop();
    }
    // One byte more is PROTOCOL_VIOLATION.
    QuicFrame frame;
    frame.kind = QuicFrameKind::Datagram;
    frame.data = ConstSpan<u8>(payload, 98U);
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(frame, out); });
    REQUIRE(run.server.state() == QuicConnectionState::Closing);
    CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    CHECK(run.raw.close_seen);
}

TEST_CASE("QUIC connection: the largest datagram follows the smaller of the two sides' UDP payload sizes",
          "[net][4.2a][quic-connection][quic-datagram]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnectionConfig client_config = datagram_config(QuicSide::Client, identity, QuicVersion::V1, 0U, true);
    client_config.max_datagram_bytes = 1400U;
    QuicConnectionConfig server_config = datagram_config(QuicSide::Server, identity, QuicVersion::V1, 1U, false);
    server_config.parameters.max_udp_payload_size = 1250U;
    server_config.datagrams.max_frame_bytes = 1400U;
    QuicConnection client(allocator, client_config);
    QuicConnection server(allocator, server_config);
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
    // The client sends this server packets of at most 1250 bytes; the server's own 1200 bytes rule the other way.
    CHECK(client.datagrams().max_payload() == 1250U - kQuicShortPacketOverhead - 3U);
    CHECK(server.datagrams().max_payload() == kQuicMinInitialDatagram - kQuicShortPacketOverhead - 3U);
    DatagramFlow up(client, server, 1U, true);
    REQUIRE(net->run_until([&] { return up.step(net->now); }, 600000U * kMs));
    up.check();
    CHECK(up.acked() == DatagramFlow::kCount); // a clean path loses none
}

TEST_CASE("QUIC connection: a datagram of the largest size takes the first packet unless an ACK is owed",
          "[net][4.2a][quic-connection][quic-datagram]")
{
    for (const bool owed : {false, true})
    {
        INFO("ACK owed " << owed);
        Identity identity;
        CountingAllocator allocator;
        QuicConnection client(allocator, datagram_config(QuicSide::Client, identity, QuicVersion::V1, 0U, false));
        QuicConnection server(allocator, datagram_config(QuicSide::Server, identity, QuicVersion::V1, 1U, false));
        // A 20-byte connection ID leaves the server's packets no room for an ACK beside the largest datagram.
        REQUIRE(client.connect(ConstSpan<u8>(kLongCid, sizeof(kLongCid)),
                               ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), kStartNs));
        auto net = std::make_unique<Net>(client, server);
        REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
        net->advance(net->now + 1000U * kMs); // every packet acknowledged, nothing owed
        // One ack-eliciting packet makes the server's ACK delayed, two make it owed at once (section 13.2.2).
        u8 out[kDatagram];
        for (usize packet = 0U; packet < (owed ? 2U : 1U); ++packet)
        {
            client.ping();
            const usize size = client.send(Span<u8>(out, sizeof(out)), net->now);
            REQUIRE(size != 0U);
            server.receive(Span<u8>(out, size), QuicEcn::NotEct, net->now);
        }
        REQUIRE((server.recovery().ack_deadline_ns(QuicSpace::Application) <= net->now) == owed);
        REQUIRE_FALSE(server.recovery().received(QuicSpace::Application).ranges().empty());
        u8 payload[kDatagram] = {};
        const usize largest = server.datagrams().max_payload();
        u64 id = 0U;
        REQUIRE(server.datagrams().queue(ConstSpan<u8>(payload, largest), net->now, id) == QuicDatagramError::None);
        const u64 in_flight = server.recovery().bytes_in_flight();
        usize size = server.send(Span<u8>(out, sizeof(out)), net->now);
        REQUIRE(size != 0U);
        client.receive(Span<u8>(out, size), QuicEcn::NotEct, net->now);
        // A carried ACK waits behind the datagram; an owed one goes first and the datagram takes the next packet.
        CHECK(client.datagrams().held() == (owed ? 0U : 1U));
        const bool eliciting = server.recovery().bytes_in_flight() > in_flight; // a DATAGRAM frame elicits an ACK
        CHECK(eliciting == !owed);
        if (owed)
        {
            size = server.send(Span<u8>(out, sizeof(out)), net->now);
            REQUIRE(size != 0U);
            client.receive(Span<u8>(out, size), QuicEcn::NotEct, net->now);
            CHECK(client.datagrams().held() == 1U);
            CHECK(server.recovery().bytes_in_flight() > in_flight);
        }
        ConstSpan<u8> held;
        REQUIRE(client.datagrams().peek(held));
        CHECK(held.size() == largest);
    }
}

// ---- This side's connection IDs and stateless reset ----

namespace
{
constexpr u8 kResetKeys[2][kQuicResetKeyBytes] = {{0x61U, 0x01U}, {0x62U, 0x02U}};

// An end's host: its IDs routed through a real table by QuicRoutedIds under its reset key, sequence 0 routed first.
class Host final : public QuicConnectionIds
{
public:
    Host(CountingAllocator& allocator, usize index, ConstSpan<u8> sequence0)
        : routes(allocator, 64U), m_ids(routes, key, static_cast<u32>(index))
    {
        REQUIRE(key.init(ConstSpan<u8>(kResetKeys[index])) == CryptoError::None);
        REQUIRE(routes.insert(sequence0, static_cast<u32>(index)) == RouteError::None);
    }

    bool issue(Span<u8> id, Span<u8> token) noexcept override
    {
        if (refuse || !m_ids.issue(id, token))
        {
            return false;
        }
        if (issued < 16U)
        {
            std::memcpy(ids[issued], id.data(), id.size());
            std::memcpy(tokens[issued], token.data(), token.size());
        }
        ++issued;
        return true;
    }
    bool token(ConstSpan<u8> id, Span<u8> token) noexcept override { return m_ids.token(id, token); }
    void retire(ConstSpan<u8> id) noexcept override
    {
        ++retired;
        m_ids.retire(id);
    }
    void watch_reset(ConstSpan<u8> token) noexcept override
    {
        ++watches;
        std::memcpy(watched, token.data(), sizeof(watched));
    }
    void unwatch_reset(ConstSpan<u8> token) noexcept override
    {
        ++unwatches;
        std::memcpy(unwatched, token.data(), sizeof(unwatched));
    }

    // The token `key` derives for `id`.
    void token_of(ConstSpan<u8> id, u8 (&out)[kQuicResetTokenBytes]) const
    {
        REQUIRE(key.token(id, Span<u8>(out)) == CryptoError::None);
    }

    ConnectionIdTable routes;
    QuicResetKey key;
    usize issued = 0U;
    usize retired = 0U;
    bool refuse = false;
    u8 ids[16][20] = {};
    u8 tokens[16][kQuicResetTokenBytes] = {};
    usize watches = 0U;
    usize unwatches = 0U;
    u8 watched[kQuicResetTokenBytes] = {};
    u8 unwatched[kQuicResetTokenBytes] = {};

private:
    QuicRoutedIds m_ids;
};

QuicConnectionConfig hosted(QuicConnectionConfig config, Host& host, u64 limit)
{
    config.ids = &host;
    config.parameters.active_connection_id_limit = limit;
    return config;
}

// A datagram that looks like a short header of `size` bytes and ends in `token`.
struct ResetDatagram
{
    u8 bytes[kDatagram] = {};
    usize size = 0U;

    ResetDatagram(const u8 (&token)[kQuicResetTokenBytes], usize total) : size(total)
    {
        std::memset(bytes, 0x3C, total);
        bytes[0] = 0x4BU;
        std::memcpy(bytes + total - kQuicResetTokenBytes, token, kQuicResetTokenBytes);
    }
    Span<u8> view() { return Span<u8>(bytes, size); }
};

// Runs a hosted pair to confirmation and until every ID is issued and acknowledged.
void settle(Net& net, QuicConnection& client, QuicConnection& server, usize client_ids, usize server_ids)
{
    REQUIRE(net.run_until([&] { return both_confirmed(client, server); }));
    REQUIRE(net.run_until(
        [&]
        {
            return client.peer_cid_count() == server_ids && server.peer_cid_count() == client_ids &&
                   net.in_flight() == 0U;
        }));
    net.advance(net.now + 200U * kMs);
}
} // namespace

TEST_CASE("QUIC connection: both ends keep the peer at its limit of their IDs through loss, with the host's tokens",
          "[net][4.2a][quic-connection]")
{
    for (u64 seed = 1U; seed <= 12U; ++seed)
    {
        Identity identity;
        CountingAllocator allocator;
        Host client_host(allocator, 0U, ConstSpan<u8>(kClientCid, sizeof(kClientCid)));
        Host server_host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
        QuicConnection client(allocator,
                              hosted(config_for(QuicSide::Client, identity, QuicVersion::V1, seed), client_host, 4U));
        QuicConnection server(
            allocator, hosted(config_for(QuicSide::Server, identity, QuicVersion::V1, seed + 1U), server_host, 3U));
        connect(client);
        auto net = std::make_unique<Net>(client, server);
        net->rng.state = seed;
        for (Path& path : net->path)
        {
            path.loss = 200U;
            path.duplicate = 100U;
            path.jitter_ns = 30U * kMs;
        }
        INFO("seed " << seed);
        // The client holds four of the server's IDs (its limit), the server three of the client's.
        settle(*net, client, server, 3U, 4U);
        CHECK(server.local_cid_count() == 4U);
        CHECK(client.local_cid_count() == 3U);
        CHECK(server_host.issued == 3U);
        CHECK(client_host.issued == 2U);
        CHECK(server_host.routes.size() == 4U);
        CHECK(client_host.routes.size() == 3U);
        CHECK(client.state() == QuicConnectionState::Established);
        CHECK(server.state() == QuicConnectionState::Established);
        // A server sends sequence 0's token as a transport parameter; a client sends none (section 10.3).
        u8 expected[kQuicResetTokenBytes];
        server_host.token_of(ConstSpan<u8>(kServerCid, sizeof(kServerCid)), expected);
        REQUIRE(client.peer_parameters().has_stateless_reset_token);
        CHECK(std::memcmp(client.peer_parameters().stateless_reset_token, expected, sizeof(expected)) == 0);
        CHECK_FALSE(server.peer_parameters().has_stateless_reset_token);
        // Every datagram is kQuicResetPadding past the sender's ID length.
        CHECK(net->smallest[0] >= sizeof(kClientCid) + kQuicResetPadding);
        CHECK(net->smallest[1] >= sizeof(kServerCid) + kQuicResetPadding);
        // Closing hands every ID back to its host, sequence 0 included.
        client.close(0U, {}, net->now);
        REQUIRE(net->run_until(
            [&]
            {
                return client.state() == QuicConnectionState::Closed && server.state() == QuicConnectionState::Closed;
            }));
        CHECK(client_host.routes.size() == 0U);
        CHECK(server_host.routes.size() == 0U);
        CHECK(client_host.retired == 3U);
        CHECK(server_host.retired == 4U);
    }
}

TEST_CASE("QUIC connection: a rotation retires every earlier ID by Retire Prior To, and the new ones carry packets",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    Host client_host(allocator, 0U, ConstSpan<u8>(kClientCid, sizeof(kClientCid)));
    Host server_host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    QuicConnection client(allocator, hosted(config_for(QuicSide::Client, identity, QuicVersion::V1), client_host, 4U));
    QuicConnection server(allocator,
                          hosted(config_for(QuicSide::Server, identity, QuicVersion::V1, 1U), server_host, 2U));
    CHECK_FALSE(server.rotate_connection_ids()); // not before confirmation
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    settle(*net, client, server, 2U, 4U);
    u8 old_ids[3][20] = {};
    std::memcpy(old_ids, server_host.ids, sizeof(old_ids));

    REQUIRE(server.rotate_connection_ids());
    CHECK_FALSE(server.rotate_connection_ids()); // the earlier IDs are not yet retired
    REQUIRE(net->run_until([&] { return server.local_cid_count() == 4U && net->in_flight() == 0U; }));
    net->advance(net->now + 200U * kMs);
    CHECK(server_host.issued == 7U);
    CHECK(server_host.retired == 4U);
    CHECK(server_host.routes.size() == 4U);
    CHECK(server_host.routes.find(ConstSpan<u8>(kServerCid, sizeof(kServerCid))) == kNoConnection);
    for (const auto& id : old_ids)
    {
        CHECK(server_host.routes.find(ConstSpan<u8>(id, sizeof(kServerCid))) == kNoConnection);
    }
    CHECK(client.peer_cid_count() == 4U);
    CHECK_FALSE(same(client.peer_cid(), ConstSpan<u8>(kServerCid, sizeof(kServerCid))));
    CHECK(server_host.routes.find(client.peer_cid()) == 1U);

    // Packets to the new ID reach the server, and the retired IDs' tokens no longer reset the client.
    const u64 received = server.counters().datagrams_received;
    const u64 dropped = server.counters().packets_dropped;
    client.ping();
    REQUIRE(net->run_until([&] { return net->in_flight() == 0U && server.counters().datagrams_received > received; }));
    CHECK(server.counters().packets_dropped == dropped);
    u8 retired_token[kQuicResetTokenBytes];
    server_host.token_of(ConstSpan<u8>(kServerCid, sizeof(kServerCid)), retired_token);
    ResetDatagram stale(retired_token, 40U);
    CHECK_FALSE(client.stateless_reset(ConstSpan<u8>(stale.bytes, stale.size), net->now));
    CHECK(client.state() == QuicConnectionState::Established);
    REQUIRE(server.rotate_connection_ids()); // a second rotation, once the first is retired
    REQUIRE(net->run_until([&] { return server.local_cid_count() == 4U && net->in_flight() == 0U; }));
    net->advance(net->now + 200U * kMs);
    CHECK(server_host.retired == 8U);
    CHECK(client.state() == QuicConnectionState::Established);
    CHECK(server.state() == QuicConnectionState::Established);
    // A rotation the host cannot route changes nothing.
    server_host.refuse = true;
    CHECK_FALSE(server.rotate_connection_ids());
    // An end without a host never rotates.
    QuicConnection lone(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1));
    CHECK_FALSE(lone.rotate_connection_ids());
}

TEST_CASE("QUIC connection: a retired ID is replaced, and retiring the packet's own ID or one never issued is refused",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    const auto retire = [](RawRun& run, u64 sequence)
    {
        QuicFrame frame;
        frame.kind = QuicFrameKind::RetireConnectionId;
        frame.sequence = sequence;
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(frame, out); });
    };
    const auto use = [](RawRun& run, u64 sequence)
    {
        std::memcpy(run.raw.dcid, run.raw.issued[sequence], run.raw.issued_bytes[sequence]);
        run.raw.dcid_bytes = run.raw.issued_bytes[sequence];
    };
    {
        CountingAllocator allocator;
        Host host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
        RawRun run(identity, 2048U, {}, &host);
        run.handshake();
        run.drain();
        // The raw client's limit is the default 2: sequence 1 is issued with the host's token.
        REQUIRE(run.raw.issued_bytes[1] == sizeof(kServerCid));
        CHECK(run.raw.retire_prior_to == 0U);
        CHECK(std::memcmp(run.raw.issued[1], host.ids[0], sizeof(kServerCid)) == 0);
        CHECK(std::memcmp(run.raw.issued_tokens[1], host.tokens[0], kQuicResetTokenBytes) == 0);
        CHECK(run.server.local_cid_count() == 2U);
        // Retiring sequence 1 from a packet sent to sequence 0: handed back, and sequence 2 replaces it.
        retire(run, 1U);
        CHECK(run.server.state() == QuicConnectionState::Established);
        CHECK(host.retired == 1U);
        CHECK(host.routes.find(ConstSpan<u8>(host.ids[0], sizeof(kServerCid))) == kNoConnection);
        REQUIRE(run.raw.issued_bytes[2] == sizeof(kServerCid));
        CHECK(run.server.local_cid_count() == 2U);
        retire(run, 1U); // a repeat changes nothing
        CHECK(run.server.state() == QuicConnectionState::Established);
        CHECK(host.retired == 1U);
        // Sequence 0 retired from a packet sent to sequence 2: accepted.
        use(run, 2U);
        retire(run, 0U);
        CHECK(run.server.state() == QuicConnectionState::Established);
        CHECK(host.routes.find(ConstSpan<u8>(kServerCid, sizeof(kServerCid))) == kNoConnection);
        REQUIRE(run.raw.issued_bytes[3] == sizeof(kServerCid));
        // Sequence 2 retired from a packet sent to it: PROTOCOL_VIOLATION (section 19.16).
        retire(run, 2U);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
    {
        // One packet retiring an earlier ID and then its own: the second is still refused after the first moved it.
        CountingAllocator allocator;
        Host host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
        RawRun run(identity, 2048U, {}, &host);
        run.handshake();
        run.drain();
        retire(run, 1U);
        REQUIRE(run.raw.issued_bytes[2] == sizeof(kServerCid));
        use(run, 2U);
        QuicFrame first;
        first.kind = QuicFrameKind::RetireConnectionId;
        first.sequence = 0U;
        QuicFrame second = first;
        second.sequence = 2U;
        run.send(QuicPacketType::OneRtt,
                 [&](Span<u8> out)
                 {
                     const usize used = write_quic_frame(first, out);
                     return used + write_quic_frame(second, out.subspan(used));
                 });
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
    {
        // A sequence greater than any issued: PROTOCOL_VIOLATION.
        CountingAllocator allocator;
        Host host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
        RawRun run(identity, 2048U, {}, &host);
        run.handshake();
        run.drain();
        retire(run, 2U);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
    {
        // A host that routes nothing: sequence 0 alone, the connection unharmed, and issued once it routes again.
        CountingAllocator allocator;
        Host host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
        host.refuse = true;
        RawRun run(identity, 2048U, {}, &host);
        run.handshake();
        run.drain();
        CHECK(run.raw.issued_bytes[1] == 0U);
        CHECK(run.server.local_cid_count() == 1U);
        CHECK(run.server.state() == QuicConnectionState::Established);
        host.refuse = false;
        QuicFrame ping;
        ping.kind = QuicFrameKind::Ping;
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(ping, out); });
        CHECK(run.raw.issued_bytes[1] == sizeof(kServerCid));
    }
}

namespace
{
// A hosted client and server run until both hold every ID.
struct HostedPair
{
    explicit HostedPair(const Identity& identity)
        : client_host(allocator, 0U, ConstSpan<u8>(kClientCid, sizeof(kClientCid))),
          server_host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid))),
          client(allocator, hosted(config_for(QuicSide::Client, identity, QuicVersion::V1), client_host, 4U)),
          server(allocator, hosted(config_for(QuicSide::Server, identity, QuicVersion::V1, 1U), server_host, 2U))
    {
        connect(client);
        net = std::make_unique<Net>(client, server);
        settle(*net, client, server, 2U, 4U);
    }

    CountingAllocator allocator;
    Host client_host;
    Host server_host;
    QuicConnection client;
    QuicConnection server;
    std::unique_ptr<Net> net;
};
} // namespace

TEST_CASE("QUIC connection: the host learns a peer's token once it is known and used, and forgets it when its ID "
          "retires or the connection closes",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    HostedPair pair(identity);
    u8 first[kQuicResetTokenBytes];
    pair.server_host.token_of(ConstSpan<u8>(kServerCid, sizeof(kServerCid)), first);
    // The client's token came with the server's parameters, after its first packet to that ID; the server's first
    // ID from the client carries no token, and the IDs the peers issued are not used yet.
    CHECK(pair.client_host.watches == 1U);
    CHECK(std::memcmp(pair.client_host.watched, first, sizeof(first)) == 0);
    CHECK(pair.client_host.unwatches == 0U);
    CHECK(pair.server_host.watches == 0U);

    REQUIRE(pair.server.rotate_connection_ids());
    REQUIRE(pair.net->run_until([&] { return pair.server.local_cid_count() == 4U && pair.net->in_flight() == 0U; }));
    pair.net->advance(pair.net->now + 200U * kMs);
    pair.client.ping();
    REQUIRE(pair.net->run_until([&] { return pair.net->in_flight() == 0U; }));
    u8 second[kQuicResetTokenBytes];
    pair.server_host.token_of(pair.client.peer_cid(), second);
    CHECK(pair.client_host.watches == 2U);
    CHECK(std::memcmp(pair.client_host.watched, second, sizeof(second)) == 0);
    CHECK(pair.client_host.unwatches == 1U);
    CHECK(std::memcmp(pair.client_host.unwatched, first, sizeof(first)) == 0);

    // More packets to the ID in use change nothing; the close forgets it, and the connection's end does not again.
    pair.client.ping();
    REQUIRE(pair.net->run_until([&] { return pair.net->in_flight() == 0U; }));
    CHECK(pair.client_host.watches == 2U);
    pair.client.close(7U, {}, pair.net->now);
    REQUIRE(pair.net->run_until([&] { return pair.client.state() == QuicConnectionState::Closed; }));
    CHECK(pair.client_host.unwatches == 2U);
    CHECK(std::memcmp(pair.client_host.unwatched, second, sizeof(second)) == 0);
    CHECK(pair.client_host.watches == 2U);

    // The connection's destruction after its close forgets nothing a second time.
    CountingAllocator allocator;
    Host client_host(allocator, 0U, ConstSpan<u8>(kClientCid, sizeof(kClientCid)));
    Host server_host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    {
        QuicConnection client(allocator,
                              hosted(config_for(QuicSide::Client, identity, QuicVersion::V1), client_host, 4U));
        QuicConnection server(allocator,
                              hosted(config_for(QuicSide::Server, identity, QuicVersion::V1, 1U), server_host, 2U));
        connect(client);
        auto net = std::make_unique<Net>(client, server);
        settle(*net, client, server, 2U, 4U);
        client.close(7U, {}, net->now);
        REQUIRE(net->run_until([&] { return client.state() == QuicConnectionState::Closed; }));
        CHECK(client_host.unwatches == 1U);
    }
    CHECK(client_host.unwatches == 1U);
}

TEST_CASE("QUIC connection: a host's token table keeps a token for its first holder", "[net][4.2a][quic-connection]")
{
    CountingAllocator allocator;
    ConnectionIdTable routes(allocator, 4U);
    ConnectionIdTable tokens(allocator, 4U);
    QuicResetKey key;
    constexpr u8 secret[kQuicResetKeyBytes] = {0x3CU};
    REQUIRE(key.init(ConstSpan<u8>(secret)) == CryptoError::None);
    QuicRoutedIds first(routes, key, 0U, &tokens);
    QuicRoutedIds second(routes, key, 1U, &tokens);
    QuicRoutedIds without(routes, key, 2U);
    constexpr u8 token[kQuicResetTokenBytes] = {0x11U};
    const ConstSpan<u8> view(token);

    first.watch_reset(view);
    CHECK(tokens.find(view) == 0U);
    second.watch_reset(view); // a peer repeating its own token across connections reaches the first alone
    CHECK(tokens.find(view) == 0U);
    second.unwatch_reset(view);
    CHECK(tokens.find(view) == 0U);
    first.unwatch_reset(view);
    CHECK(tokens.find(view) == kNoConnection);
    without.watch_reset(view); // a table is optional
    without.unwatch_reset(view);
    CHECK(tokens.size() == 0U);
}

TEST_CASE("QUIC connection: a stateless reset drains the connection, by the token of a used, unretired ID alone",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    {
        HostedPair pair(identity);
        QuicConnection& client = pair.client;
        u8 used[kQuicResetTokenBytes];
        pair.server_host.token_of(ConstSpan<u8>(kServerCid, sizeof(kServerCid)), used);
        u8 wrong[kQuicResetTokenBytes];
        std::memcpy(wrong, used, sizeof(wrong));
        wrong[15] ^= 0x01U;

        // Too short to be a reset, a token of an ID the client never sent to, or no token at all: nothing happens.
        ResetDatagram tiny(used, kQuicMinResetBytes - 1U);
        client.receive(tiny.view(), QuicEcn::NotEct, pair.net->now);
        ResetDatagram unused(pair.server_host.tokens[0], 60U);
        const u64 dropped = client.counters().packets_dropped;
        client.receive(unused.view(), QuicEcn::NotEct, pair.net->now);
        CHECK(client.counters().packets_dropped == dropped + 1U);
        ResetDatagram forged(wrong, 60U);
        client.receive(forged.view(), QuicEcn::NotEct, pair.net->now);
        CHECK_FALSE(client.stateless_reset(ConstSpan<u8>(forged.bytes, forged.size), pair.net->now));
        CHECK(client.state() == QuicConnectionState::Established);

        // The token of the ID in use, at the least size: the client drains, sends nothing and ends.
        ResetDatagram reset(used, kQuicMinResetBytes);
        client.receive(reset.view(), QuicEcn::NotEct, pair.net->now);
        CHECK(client.state() == QuicConnectionState::Draining);
        CHECK(client.close_info().stateless_reset);
        CHECK_FALSE(client.close_info().local);
        u8 out[kDatagram];
        CHECK(client.send(Span<u8>(out, sizeof(out)), pair.net->now) == 0U);
        REQUIRE(pair.net->run_until([&] { return client.state() == QuicConnectionState::Closed; }));
        CHECK(pair.client_host.routes.size() == 0U);
    }
    {
        // A reset reaching a closing connection drains it and keeps the close it sent.
        HostedPair pair(identity);
        QuicConnection& client = pair.client;
        client.close(7U, {}, pair.net->now);
        u8 used[kQuicResetTokenBytes];
        pair.server_host.token_of(ConstSpan<u8>(kServerCid, sizeof(kServerCid)), used);
        ResetDatagram reset(used, 50U);
        client.receive(reset.view(), QuicEcn::NotEct, pair.net->now);
        CHECK(client.state() == QuicConnectionState::Draining);
        CHECK(client.close_info().application);
        CHECK(client.close_info().error_code == 7U);
        CHECK(client.close_info().stateless_reset);
    }
    {
        // A server that lost the connection answers the client's next packet from its ingress with the same key.
        HostedPair pair(identity);
        QuicConnection& client = pair.client;
        ConnectionIdTable restarted(pair.allocator, 8U);
        HandshakeBuckets buckets(pair.allocator, HandshakeRules{});
        IngressConfig config;
        config.local_cid_bytes = sizeof(kServerCid);
        Ingress ingress(config, restarted, buckets, nullptr, nullptr, &pair.server_host.key);
        REQUIRE(ingress.ready());
        client.ping();
        u8 out[kDatagram];
        const usize size = client.send(Span<u8>(out, sizeof(out)), pair.net->now);
        REQUIRE(size != 0U);
        const Endpoint peer{IpAddress::v4(192, 0, 2, 9), 4433U};
        const IngressDecision decision = ingress.classify(ConstSpan<u8>(out, size), false, peer, pair.net->now, 0U);
        REQUIRE(decision.action == IngressAction::StatelessReset);
        u8 answer[kQuicMaxStatelessAnswerBytes];
        const usize written = ingress.answer(decision, peer, 0U, Span<u8>(answer));
        REQUIRE(written != 0U);
        CHECK(written < size);
        CHECK(client.stateless_reset(ConstSpan<u8>(answer, written), pair.net->now));
        CHECK(client.state() == QuicConnectionState::Draining);
        CHECK(client.close_info().stateless_reset);
    }
}

TEST_CASE("QUIC connection: a switching server closes on client parameters that do not parse or name another "
          "version, in the original version",
          "[net][4.2a][quic-connection]")
{
    const u32 prefer_v2[] = {static_cast<u32>(QuicVersion::V2), static_cast<u32>(QuicVersion::V1)};
    for (const bool parse : {false, true})
    {
        Identity identity;
        RawRun run(identity, 2048U, {}, nullptr, prefer_v2);
        u8 encoded[512];
        usize bytes = 0U;
        if (parse)
        {
            // A chosen version 2 in a version 1 Initial: RFC 9368 section 4's mismatch.
            QuicTransportParameters parameters;
            parameters.max_idle_timeout = 30000U;
            parameters.has_initial_source_connection_id = true;
            REQUIRE(parameters.initial_source_connection_id.assign(ConstSpan<u8>(kClientCid, sizeof(kClientCid))));
            parameters.has_version_information = true;
            parameters.version_information.chosen = static_cast<u32>(QuicVersion::V2);
            parameters.version_information.available[0] = static_cast<u32>(QuicVersion::V2);
            parameters.version_information.available[1] = static_cast<u32>(QuicVersion::V1);
            parameters.version_information.available_count = 2U;
            bytes =
                write_quic_transport_parameters(parameters, QuicSide::Client, {7U}, Span<u8>(encoded, sizeof(encoded)));
        }
        else
        {
            encoded[0] = 0x01U; // max_idle_timeout whose length runs past the end
            encoded[1] = 0x09U;
            bytes = 2U;
        }
        REQUIRE(bytes != 0U);
        REQUIRE(run.raw.handshake.init(identity.client, ConstSpan<u8>(encoded, bytes)) == TlsError::None);
        REQUIRE(run.raw.protection.start(QuicSide::Client, QuicVersion::V1,
                                         ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid))) == CryptoError::None);
        std::memcpy(run.raw.dcid, kInitialDcid, sizeof(kInitialDcid));
        run.raw.dcid_bytes = sizeof(kInitialDcid);
        QuicAccept accept;
        accept.local_cid = ConstSpan<u8>(kServerCid, sizeof(kServerCid));
        accept.client_dcid = ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid));
        accept.client_scid = ConstSpan<u8>(kClientCid, sizeof(kClientCid));
        accept.original_dcid = accept.client_dcid;
        REQUIRE(run.server.accept(accept, run.now));
        run.send_crypto(QuicLevel::Initial);
        const QuicTransportError expected =
            parse ? QuicTransportError::VersionNegotiationError : QuicTransportError::TransportParameterError;
        CHECK(run.server.state() == QuicConnectionState::Closing);
        CHECK(run.server.close_info().error_code == static_cast<u64>(expected));
        CHECK(run.server.version() == QuicVersion::V1);
        CHECK(run.raw.close_seen); // read under the version 1 keys
        CHECK(run.raw.close_code == static_cast<u64>(expected));
    }
}

// ---- 0-RTT (RFC 9000 section 7.4.1, RFC 9001 sections 4.1.4, 4.6, 4.9.3 and 5.7, RFC 9002 section 6.4) ----

namespace
{
constexpr u8 kAppContext[] = {0x5C, 0x11, 0xE7, 0x02, 0x9A, 0x43, 0x0D, 0xB8}; // a message schema digest's stand-in
constexpr u8 kOtherAppContext[] = {0x5C, 0x11, 0xE7, 0x02, 0x9A, 0x43, 0x0D, 0xB9};
constexpr usize kEarlyBytes = 3000U; // a request that takes three 0-RTT packets
constexpr u64 kEarlyPattern = 7U;
constexpr u8 kEarlyPayload[] = {0xDA, 0x7A, 0x01};

// A client's tickets: the newest one kept, each handed out once (T07).
// A fleet whose servers accept 0-RTT under one ticket key.
struct EarlyFleet
{
    ReadyFilter replay;
    Identity identity{false, &replay.filter};
};

QuicConnectionConfig early_config(QuicSide side, const Identity& identity, QuicVersion version, usize seed,
                                  Tickets* tickets, ConstSpan<u8> app = ConstSpan<u8>(kAppContext))
{
    QuicConnectionConfig config = datagram_config(side, identity, version, seed, true);
    if (side == QuicSide::Client)
    {
        config.tickets = tickets;
    }
    else
    {
        config.early_context = app;
    }
    return config;
}

// A first connection that leaves `tickets` a ticket of `version` from a server of `identity` with `app`'s context.
void take_ticket(const Identity& identity, QuicVersion version, Tickets& tickets,
                 ConstSpan<u8> app = ConstSpan<u8>(kAppContext))
{
    CountingAllocator allocator;
    QuicConnection client(allocator, early_config(QuicSide::Client, identity, version, 0U, &tickets));
    QuicConnection server(allocator, early_config(QuicSide::Server, identity, version, 1U, nullptr, app));
    REQUIRE(server.ready());
    connect(client);
    CHECK(client.handshake().early_data() == TlsEarlyData::None);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return both_confirmed(client, server) && tickets.holds(); }));
}

// What a resumed client writes before its server answered: kEarlyBytes of a stream with its FIN, and one datagram.
void send_early(QuicConnection& client, u64 now, u64& stream, u64& datagram)
{
    REQUIRE(client.streams().open(true, stream) == QuicStreamError::None);
    u64 offset = 0U;
    REQUIRE(write_pattern(client.streams(), stream, offset, kEarlyBytes, kEarlyPattern));
    REQUIRE(client.datagrams().queue(ConstSpan<u8>(kEarlyPayload), now, datagram) == QuicDatagramError::None);
}

// Counts the 0-RTT packets each end sends, and whether every one is in `original` (RFC 9369 section 4.1).
class EarlyWatch final : public Filter
{
public:
    explicit EarlyWatch(QuicVersion original) : m_original(static_cast<u32>(original)) {}

    bool drop(usize from, ConstSpan<u8> datagram) override
    {
        u8 copy[kDatagram];
        std::memcpy(copy, datagram.data(), datagram.size());
        QuicDatagramPackets packets(Span<u8>(copy, datagram.size()),
                                    static_cast<u8>((from == 0U) ? sizeof(kServerCid) : sizeof(kClientCid)));
        QuicHeader header;
        Span<u8> packet;
        while (packets.next(header, packet))
        {
            if (header.type == QuicPacketType::ZeroRtt)
            {
                ++early[from];
                in_original = in_original && header.version == m_original;
            }
        }
        return false;
    }

    usize early[2] = {};
    bool in_original = true;

private:
    u32 m_original = 0U;
};

// The server's reads of the early stream, each byte checked: true once all of it was read. `early` counts the bytes
// read that were marked replayable.
bool read_stream(QuicConnection& server, u64 stream, u64& read, u64& early)
{
    QuicStreamSet& set = server.streams();
    if (set.recv_state(stream) == QuicRecvState::None || set.recv_state(stream) == QuicRecvState::DataRead)
    {
        return read == kEarlyBytes;
    }
    for (QuicStreamRead peeked = set.peek(stream); !peeked.data.empty() || peeked.fin; peeked = set.peek(stream))
    {
        for (usize at = 0U; at < peeked.data.size(); ++at)
        {
            REQUIRE(peeked.data[at] == pattern(kEarlyPattern, read + at));
        }
        early += peeked.early_bytes;
        read += peeked.data.size();
        REQUIRE(set.consume(stream, peeked.data.size()) == QuicStreamError::None);
        if (peeked.fin)
        {
            break;
        }
    }
    return read == kEarlyBytes;
}
} // namespace

TEST_CASE("QUIC connection: a resumed client's stream and datagram go in 0-RTT packets, read by the server before "
          "the handshake completes and marked replayable, in both versions",
          "[net][4.2a][quic-connection][quic-early]")
{
    for (const QuicVersion version : kVersions)
    {
        INFO("version " << static_cast<u32>(version));
        EarlyFleet fleet;
        Tickets tickets;
        take_ticket(fleet.identity, version, tickets);
        CountingAllocator allocator;
        QuicConnection client(allocator, early_config(QuicSide::Client, fleet.identity, version, 2U, &tickets));
        QuicConnection server(allocator, early_config(QuicSide::Server, fleet.identity, version, 3U, nullptr));
        REQUIRE(server.ready());
        const usize allocations = allocator.calls();
        connect(client);
        REQUIRE(client.handshake().early_data() == TlsEarlyData::Offered);
        CHECK(client.protection().can_send(QuicPacketType::ZeroRtt));
        CHECK_FALSE(tickets.holds()); // taken once
        u64 stream = 0U;
        u64 datagram = 0U;
        send_early(client, kStartNs, stream, datagram);

        auto net = std::make_unique<Net>(client, server);
        EarlyWatch watch(version);
        net->filter = &watch;
        u64 read = 0U;
        u64 early = 0U;
        bool read_handshaking = false;
        bool opened_one_rtt_early = false;
        bool datagram_early = false;
        REQUIRE(net->run_until(
            [&]
            {
                const bool handshaking = server.state() == QuicConnectionState::Handshaking;
                // RFC 9001 section 5.7: no 1-RTT packet opens before the server's handshake completes.
                opened_one_rtt_early =
                    opened_one_rtt_early || (handshaking && server.protection().can_open(QuicPacketType::OneRtt));
                const u64 before = read;
                const bool done = read_stream(server, stream, read, early);
                read_handshaking = read_handshaking || (handshaking && read > before);
                ConstSpan<u8> payload;
                if (server.datagrams().peek(payload))
                {
                    CHECK(same(payload, ConstSpan<u8>(kEarlyPayload)));
                    datagram_early = server.datagrams().oldest_early();
                    server.datagrams().pop();
                }
                return done && both_confirmed(client, server);
            }));
        CHECK(early == kEarlyBytes); // every byte came in 0-RTT
        CHECK(read_handshaking);     // half a round trip before the client's Finished
        CHECK_FALSE(opened_one_rtt_early);
        CHECK(datagram_early);
        CHECK(client.handshake().early_data() == TlsEarlyData::Accepted);
        CHECK(server.handshake().early_data() == TlsEarlyData::Accepted);
        CHECK(watch.early[0] != 0U);
        CHECK(watch.early[1] == 0U);
        CHECK(watch.in_original);
        CHECK(net->client_initials_padded);
        CHECK(net->amplification_kept);
        // Acknowledged in 1-RTT packets (RFC 9000 section 13.2.6), the datagram reports so.
        QuicDatagramReport report;
        REQUIRE(net->run_until([&] { return client.datagrams().poll(report); }));
        CHECK(report.id == datagram);
        CHECK(report.outcome == QuicDatagramOutcome::Acked);

        // Data after the handshake is not replayable.
        u64 second = 0U;
        REQUIRE(client.streams().open(true, second) == QuicStreamError::None);
        u64 written = 0U;
        REQUIRE(write_pattern(client.streams(), second, written, 1000U, kEarlyPattern));
        u64 marked = 0U;
        REQUIRE(net->run_until(
            [&]
            {
                const QuicStreamRead peeked = server.streams().peek(second);
                marked += peeked.early_bytes;
                return !peeked.data.empty() || server.streams().recv_state(second) == QuicRecvState::DataRead;
            }));
        CHECK(marked == 0U);
        // The server's 0-RTT keys go three PTOs after its first 1-RTT packet (RFC 9001 section 4.9.3).
        CHECK(server.protection().can_open(QuicPacketType::ZeroRtt));
        net->advance(net->now + 3000U * kMs);
        CHECK_FALSE(server.protection().can_open(QuicPacketType::ZeroRtt));
        CHECK(allocator.calls() == allocations);
    }
}

TEST_CASE("QUIC connection: 0-RTT over paths that lose, duplicate and reorder delivers every byte once, holding "
          "packets ahead of their keys",
          "[net][4.2a][quic-connection][quic-early]")
{
    u64 held = 0U;
    u64 early_total = 0U;
    for (const QuicVersion version : kVersions)
    {
        for (u64 seed = 1U; seed <= 6U; ++seed)
        {
            INFO("version " << static_cast<u32>(version) << " seed " << seed);
            EarlyFleet fleet;
            Tickets tickets;
            take_ticket(fleet.identity, version, tickets);
            CountingAllocator allocator;
            QuicConnection client(allocator, early_config(QuicSide::Client, fleet.identity, version, seed, &tickets));
            QuicConnection server(allocator,
                                  early_config(QuicSide::Server, fleet.identity, version, seed + 1U, nullptr));
            REQUIRE(server.ready());
            const usize allocations = allocator.calls();
            connect(client);
            u64 stream = 0U;
            u64 datagram = 0U;
            send_early(client, kStartNs, stream, datagram);
            auto net = std::make_unique<Net>(client, server);
            net->rng.state = seed;
            for (Path& path : net->path)
            {
                path.loss = 100U;
                path.duplicate = 50U;
                path.delay_ns = 15U * kMs;
                path.jitter_ns = 20U * kMs;
            }
            u64 read = 0U;
            u64 early = 0U;
            REQUIRE(net->run_until(
                [&] { return read_stream(server, stream, read, early) && both_confirmed(client, server); }));
            CHECK(client.handshake().early_data() == TlsEarlyData::Accepted);
            CHECK(early <= kEarlyBytes);
            CHECK(net->client_initials_padded);
            CHECK(net->amplification_kept);
            CHECK(allocator.calls() == allocations);
            held += client.counters().packets_held + server.counters().packets_held;
            early_total += early;
        }
    }
    CHECK(held != 0U);        // reordering put packets ahead of their keys, and they were read when the keys came
    CHECK(early_total != 0U); // and 0-RTT carried data on these paths
}

TEST_CASE("QUIC connection: a rejected 0-RTT resets every stream and drops the queued datagrams, the server reads "
          "none of it, and the data goes again in 1-RTT",
          "[net][4.2a][quic-connection][quic-early]")
{
    enum class Refusal : u8
    {
        NoEarlyData,  // a server of the fleet without early data
        OtherContext, // another application context
        LowerLimit,   // a lower stream window: the parameters' part of the context differs
        Switch,       // a server that switches versions: the ticket belongs to the original one (RFC 9369 section 5)
    };
    const u32 client_versions[] = {static_cast<u32>(QuicVersion::V1), static_cast<u32>(QuicVersion::V2)};
    const u32 server_versions[] = {static_cast<u32>(QuicVersion::V2), static_cast<u32>(QuicVersion::V1)};
    for (const Refusal refusal : {Refusal::NoEarlyData, Refusal::OtherContext, Refusal::LowerLimit, Refusal::Switch})
    {
        INFO("refusal " << static_cast<u32>(refusal));
        EarlyFleet fleet;
        Tickets tickets;
        take_ticket(fleet.identity, QuicVersion::V1, tickets);
        CountingAllocator allocator;
        QuicConnectionConfig client_config =
            early_config(QuicSide::Client, fleet.identity, QuicVersion::V1, 2U, &tickets);
        QuicConnectionConfig server_config =
            early_config(QuicSide::Server, fleet.identity, QuicVersion::V1, 3U, nullptr);
        switch (refusal)
        {
            case Refusal::NoEarlyData:
                server_config.tls = &fleet.identity.plain;
                server_config.early_context = {};
                break;
            case Refusal::OtherContext:
                server_config.early_context = ConstSpan<u8>(kOtherAppContext);
                break;
            case Refusal::LowerLimit:
                server_config.streams.stream_window = 4096U;
                break;
            case Refusal::Switch:
                client_config.versions = client_versions;
                server_config.versions = server_versions;
                break;
        }
        QuicConnection client(allocator, client_config);
        QuicConnection server(allocator, server_config);
        REQUIRE(server.ready());
        connect(client);
        REQUIRE(client.handshake().early_data() == TlsEarlyData::Offered);
        u64 stream = 0U;
        u64 datagram = 0U;
        send_early(client, kStartNs, stream, datagram);
        u64 queued = 0U;
        REQUIRE(client.datagrams().queue(ConstSpan<u8>(kEarlyPayload), kStartNs, queued) == QuicDatagramError::None);
        // One of the largest size waits for 1-RTT, the long header of a 0-RTT packet leaving it no room.
        static const u8 kLargest[kQuicMinInitialDatagram] = {};
        REQUIRE(client.datagrams().max_payload() <= sizeof(kLargest));
        REQUIRE(client.datagrams().queue(ConstSpan<u8>(kLargest, client.datagrams().max_payload()), kStartNs,
                                         queued) == QuicDatagramError::None);
        auto net = std::make_unique<Net>(client, server);
        EarlyWatch watch(QuicVersion::V1);
        net->filter = &watch;
        REQUIRE(net->run_until([&] { return client.handshake().early_data() == TlsEarlyData::Rejected; }));
        CHECK(watch.early[0] != 0U); // it was sent
        // RFC 9001 section 4.6.2: the client's streams start again, and its datagrams are reported lost.
        CHECK(client.streams().open_streams() == 0U);
        CHECK(client.streams().send_state(stream) == QuicSendState::None);
        CHECK(client.datagrams().queued() == 0U);
        usize lost = 0U;
        QuicDatagramReport report;
        while (client.datagrams().poll(report))
        {
            CHECK(report.outcome == QuicDatagramOutcome::Lost);
            ++lost;
        }
        CHECK(lost == 3U); // two sent in 0-RTT packets and the one still queued
        REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
        CHECK(server.handshake().early_data() != TlsEarlyData::Accepted);
        CHECK(server.streams().open_streams() == 0U); // a server rejecting 0-RTT processes none of it
        CHECK(server.datagrams().held() == 0U);
        CHECK_FALSE(server.protection().can_open(QuicPacketType::ZeroRtt));
        if (refusal == Refusal::Switch)
        {
            CHECK(client.version() == QuicVersion::V2);
        }
        // The application sends it again, in 1-RTT, under the server's own parameters.
        u64 again = 0U;
        send_early(client, net->now, again, datagram);
        CHECK(again == stream);
        u64 read = 0U;
        u64 early = 0U;
        REQUIRE(net->run_until([&] { return read_stream(server, again, read, early); }));
        CHECK(early == 0U);
        ConstSpan<u8> payload;
        REQUIRE(net->run_until([&] { return server.datagrams().peek(payload); }));
        CHECK(same(payload, ConstSpan<u8>(kEarlyPayload))); // the one sent again, never one dropped
        CHECK_FALSE(server.datagrams().oldest_early());
        CHECK(client.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC connection: 0-RTT sent before a Retry goes again after it, to the Retry's ID",
          "[net][4.2a][quic-connection][quic-early]")
{
    for (const QuicVersion version : kVersions)
    {
        for (u64 round = 0U; round < 3U; ++round)
        {
            INFO("version " << static_cast<u32>(version) << " round " << round);
            EarlyFleet fleet;
            Tickets tickets;
            take_ticket(fleet.identity, version, tickets);
            CountingAllocator allocator;
            RetryFront front;
            QuicConnection client(allocator, early_config(QuicSide::Client, fleet.identity, version, 2U, &tickets));
            QuicConnection server(allocator, early_config(QuicSide::Server, fleet.identity, version, 3U, nullptr));
            connect(client);
            u64 stream = 0U;
            u64 datagram = 0U;
            send_early(client, kStartNs, stream, datagram);
            auto net = std::make_unique<Net>(client, server);
            net->ingress = &front.ingress;
            net->rng.state = round;
            if (round != 0U)
            {
                for (Path& path : net->path)
                {
                    path.loss = 150U;
                    path.duplicate = 50U;
                    path.jitter_ns = 8U * kMs;
                }
            }
            u64 read = 0U;
            u64 early = 0U;
            REQUIRE(net->run_until(
                [&] { return read_stream(server, stream, read, early) && both_confirmed(client, server); }));
            CHECK(client.retried());
            CHECK(server.retried());
            CHECK(client.handshake().early_data() == TlsEarlyData::Accepted);
            if (round == 0U)
            {
                CHECK(early == kEarlyBytes); // the 0-RTT packets after the Retry carried all of it
            }
            CHECK(net->client_initials_padded);
        }
    }
}

namespace
{
// The client's first flight with a request of several packets: its later datagrams hold 0-RTT packets alone. The
// server accepts the flight's first Initial without reading it.
usize early_flight(QuicConnection& client, QuicConnection& server, Datagram* flight, usize room)
{
    connect(client);
    u64 id = 0U;
    REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
    u64 offset = 0U;
    REQUIRE(write_pattern(client.streams(), id, offset, 6000U, kEarlyPattern));
    const usize count = collect(client, flight, room, kStartNs);
    QuicHeader first;
    REQUIRE(parse_quic_header(flight[0].view(), sizeof(kServerCid), first) == QuicHeaderError::None);
    REQUIRE(first.type == QuicPacketType::Initial);
    QuicAccept accept;
    accept.local_cid = ConstSpan<u8>(kServerCid, sizeof(kServerCid));
    accept.client_dcid = first.dcid;
    accept.client_scid = first.scid;
    accept.original_dcid = first.dcid;
    REQUIRE(server.accept(accept, kStartNs));
    return count;
}

// Delivers the flight's datagrams whose first packet is (or is not) a 0-RTT packet, last first; their count.
usize deliver_flight(QuicConnection& to, Datagram* flight, usize count, bool early)
{
    usize delivered = 0U;
    for (usize index = count; index-- > 0U;)
    {
        QuicHeader header;
        REQUIRE(parse_quic_header(flight[index].view(), sizeof(kServerCid), header) == QuicHeaderError::None);
        if ((header.type == QuicPacketType::ZeroRtt) == early)
        {
            to.receive(Span<u8>(flight[index].bytes, flight[index].size), QuicEcn::NotEct, kStartNs);
            ++delivered;
        }
    }
    return delivered;
}
} // namespace

TEST_CASE("QUIC connection: packets ahead of their keys are held in a bounded ring and read once the keys come",
          "[net][4.2a][quic-connection][quic-early]")
{
    for (const u32 ring : {8U, 1U})
    {
        INFO("ring " << ring);
        EarlyFleet fleet;
        Tickets tickets;
        take_ticket(fleet.identity, QuicVersion::V1, tickets);
        CountingAllocator allocator;
        QuicConnectionConfig client_config =
            early_config(QuicSide::Client, fleet.identity, QuicVersion::V1, 2U, &tickets);
        QuicConnectionConfig server_config =
            early_config(QuicSide::Server, fleet.identity, QuicVersion::V1, 3U, nullptr);
        client_config.held_packets = ring;
        server_config.held_packets = ring;
        QuicConnection client(allocator, client_config);
        QuicConnection server(allocator, server_config);
        REQUIRE(server.ready());
        Datagram flight[16];
        const usize count = early_flight(client, server, flight, 16U);
        // The 0-RTT-only datagrams arrive first: held as the ring allows, then read once the Initials gave keys.
        const usize alone = deliver_flight(server, flight, count, true);
        REQUIRE(alone >= 2U);
        CHECK(server.counters().packets_held == ((alone < ring) ? alone : ring));
        CHECK(server.counters().packets_dropped == ((alone < ring) ? 0U : alone - ring));
        CHECK(server.streams().open_streams() == 0U);
        REQUIRE(deliver_flight(server, flight, count, false) >= 1U);
        REQUIRE(server.handshake().early_data() == TlsEarlyData::Accepted);
        CHECK(server.streams().open_streams() == 1U); // the held packets were read under the keys that came
        if (ring == 8U)
        {
            const QuicStreamRead peeked = server.streams().peek(0U);
            CHECK(peeked.data.size() == 6000U); // every byte of the flight, none sent again
            CHECK(peeked.early_bytes == peeked.data.size());
        }

        // The server's flight in reverse: its Handshake and 1-RTT packets wait on the client for the Initial's keys.
        Datagram answer[16];
        const usize answers = collect(server, answer, 16U, kStartNs);
        REQUIRE(answers >= 2U);
        for (usize index = answers; index-- > 0U;)
        {
            client.receive(Span<u8>(answer[index].bytes, answer[index].size), QuicEcn::NotEct, kStartNs);
        }
        CHECK(client.counters().packets_held != 0U);
        if (ring == 8U)
        {
            // No time passed and nothing was sent again: the held packets completed the handshake.
            CHECK(client.state() == QuicConnectionState::Established);
            CHECK(client.handshake().early_data() == TlsEarlyData::Accepted);
        }
        else
        {
            CHECK(client.counters().packets_dropped != 0U); // past the ring: the peer sends it again
        }
    }
}

TEST_CASE("QUIC connection: a server that rejects 0-RTT drops what it held, and early configurations out of range "
          "are refused",
          "[net][4.2a][quic-connection][quic-early]")
{
    {
        EarlyFleet fleet;
        Tickets tickets;
        take_ticket(fleet.identity, QuicVersion::V1, tickets);
        CountingAllocator allocator;
        QuicConnection client(allocator, early_config(QuicSide::Client, fleet.identity, QuicVersion::V1, 2U, &tickets));
        QuicConnection server(allocator, early_config(QuicSide::Server, fleet.identity, QuicVersion::V1, 3U, nullptr,
                                                      ConstSpan<u8>(kOtherAppContext)));
        Datagram flight[16];
        const usize count = early_flight(client, server, flight, 16U);
        REQUIRE(deliver_flight(server, flight, count, true) >= 1U);
        CHECK(server.counters().packets_held != 0U);
        const u64 dropped = server.counters().packets_dropped;
        REQUIRE(deliver_flight(server, flight, count, false) >= 1U);
        CHECK(server.handshake().early_data() == TlsEarlyData::Rejected);
        CHECK(server.streams().open_streams() == 0U); // RFC 9001 section 4.6.2: none processed, held ones included
        CHECK(server.counters().packets_dropped > dropped);
    }
    Identity plain;
    EarlyFleet fleet;
    static const u8 kBig[kQuicMaxEarlyContextBytes + 1U] = {};
    CountingAllocator allocator;
    const auto refused = [&](const QuicConnectionConfig& config)
    {
        const QuicConnection connection(allocator, config);
        return !connection.ready();
    };
    QuicConnectionConfig client = early_config(QuicSide::Client, fleet.identity, QuicVersion::V1, 0U, nullptr);
    client.early_context = ConstSpan<u8>(kAppContext);
    CHECK(refused(client)); // a client names no context
    QuicConnectionConfig without = config_for(QuicSide::Server, plain, QuicVersion::V1);
    without.early_context = ConstSpan<u8>(kAppContext);
    CHECK(refused(without)); // nor a server that takes no early data
    QuicConnectionConfig oversized = early_config(QuicSide::Server, fleet.identity, QuicVersion::V1, 0U, nullptr);
    oversized.early_context = ConstSpan<u8>(kBig, sizeof(kBig));
    CHECK(refused(oversized));
    oversized.early_context = ConstSpan<u8>(kBig, kQuicMaxEarlyContextBytes);
    CHECK_FALSE(refused(oversized));
    QuicConnectionConfig ring = config_for(QuicSide::Client, plain, QuicVersion::V1);
    ring.held_packets = kQuicMaxHeldPackets + 1U;
    CHECK(refused(ring));
    ring.held_packets = 0U;
    CHECK_FALSE(refused(ring));
    // A server with early data and an empty application part still names its parameters' part.
    QuicConnection server(allocator, early_config(QuicSide::Server, fleet.identity, QuicVersion::V1, 0U, nullptr, {}));
    REQUIRE(server.ready());
    QuicAccept accept;
    accept.local_cid = ConstSpan<u8>(kServerCid, sizeof(kServerCid));
    accept.client_dcid = ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid));
    accept.client_scid = ConstSpan<u8>(kClientCid, sizeof(kClientCid));
    accept.original_dcid = accept.client_dcid;
    CHECK(server.accept(accept, kStartNs));
}
