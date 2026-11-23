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
constexpr u8 kClientPathCid[8] = {0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF}; // a raw client's path ID 1
constexpr u8 kClientPathTwoCid[8] = {0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF}; // and its path ID 2
constexpr u8 kClientPathThreeCid[8] = {0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF}; // and its path ID 3
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
        client.receive(Span<u8>(junk, sizeof(junk)), Ecn::NotEct, net->now);
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
        client.receive(Span<u8>(junk, sizeof(junk)), Ecn::NotEct, net->now);
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
        server.receive(Span<u8>(first, size), Ecn::NotEct, kStartNs);
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
        client.receive(Span<u8>(foreign, size), Ecn::NotEct, kStartNs);
        CHECK(client.state() == QuicConnectionState::Handshaking);
        client.receive(Span<u8>(packet, size), Ecn::NotEct, kStartNs);
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
    connection.receive(Span<u8>(copy.bytes, copy.size), Ecn::NotEct, now);
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
            server.receive(Span<u8>(again, size), Ecn::NotEct, at);
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
        server.receive(Span<u8>(again, size), Ecn::NotEct, kStartNs);
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
    for (usize which = 0U; which < 13U; ++which)
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
            case 11U:
                // RFC 10069: a limit is at most the path slots beyond path ID 0's (design S78).
                config.parameters.has_initial_max_path_id = true;
                config.parameters.initial_max_path_id = kQuicMaxPaths;
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
    // A client offering multipath issues a connection ID of non-zero length (RFC 10069 section 2.1); one that does not
    // offer it may issue a zero-length one.
    QuicConnectionConfig offering = good;
    offering.parameters.has_initial_max_path_id = true;
    QuicConnection multipath(allocator, offering);
    REQUIRE(multipath.ready());
    CHECK_FALSE(multipath.connect({}, ConstSpan<u8>(kInitialDcid, 8U), kStartNs));
    CHECK(multipath.connect(ConstSpan<u8>(kClientCid, 8U), ConstSpan<u8>(kInitialDcid, 8U), kStartNs));
    QuicConnection plain(allocator, good);
    CHECK(plain.connect({}, ConstSpan<u8>(kInitialDcid, 8U), kStartNs));
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

// The raw peers' server: room for the thousand packets a skipped number takes to appear. Its paths take turns, so each
// path's acknowledgements go on that path (QuicPathChoice::Turns), which RawClient's reader holds it to.
QuicConnectionConfig server_config(const Identity& identity, crd::u32 sent_packets, const QuicDatagramLimits& datagrams,
                                   QuicConnectionIds* ids, ConstSpan<u32> versions, u64 receive_timestamps,
                                   QuicAddressDiscovery discovery, bool multipath, u64 max_path_id)
{
    QuicConnectionConfig config = config_for(QuicSide::Server, identity, QuicVersion::V1, 2U);
    config.parameters.max_receive_timestamps_per_ack = receive_timestamps;
    config.parameters.address_discovery = discovery;
    config.parameters.has_initial_max_path_id = multipath;
    config.parameters.initial_max_path_id = max_path_id;
    config.path_choice = QuicPathChoice::Turns;
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
    u8 close_dcid = 0U; // the first byte of the Destination Connection ID the close came to
    bool retire_seen = false;
    u64 retire_sequence = 0U;
    usize retirements = 0U; // RETIRE_CONNECTION_ID frames read
    // The server's NEW_CONNECTION_ID and RETIRE_CONNECTION_ID frames, and their PATH_ forms (RFC 10069), read.
    usize plain_id_frames = 0U;
    usize path_id_frames = 0U;
    bool path_response_seen = false;
    bool seen[4096] = {};      // the server's 1-RTT packet numbers received
    bool further_seen[1024] = {}; // and its path ID 1 packet numbers
    usize eliciting_seen = 0U; // the server's ack-eliciting 1-RTT packets received
    u8 path_response[kQuicPathDataBytes] = {};
    usize path_response_datagram = 0U;
    usize datagrams = 0U;
    // The server's PATH_CHALLENGE frames (the first 32): each one's data, the path and size of its datagram, and the
    // first byte of the Destination Connection ID it came to.
    usize challenges = 0U;
    u8 challenge[32][kQuicPathDataBytes] = {};
    QuicPath challenge_path[32] = {};
    usize challenge_datagram[32] = {};
    u8 challenge_dcid[32] = {};
    // PATH_RESPONSE frames the server sent in a packet with a non-probing frame (RFC 9000 section 9.3.3).
    usize responses_non_probing = 0U;
    // Every server datagram's path, size and the first byte of its last packet's Destination Connection ID, in order
    // (the first 256).
    QuicPath sent_to[256] = {};
    usize sent_size[256] = {};
    u8 sent_dcid[256] = {};
    // The server's NEW_CONNECTION_ID frames, by sequence.
    u8 issued[8][20] = {};
    u8 issued_tokens[8][kQuicResetTokenBytes] = {};
    usize issued_bytes[8] = {};
    u64 retire_prior_to = 0U;
    // Multipath (RFC 10069): the server's IDs for further path IDs (each one's count and its sequence 0), its
    // PATH_ACK frames (the latest one's path ID and largest), and the raw client's path slot opened for path ID 1
    // (kClientPathCid), 0 for none, with its next packet number.
    usize further_ids[kQuicMaxPaths] = {};
    u64 largest_issued = 0U; // the largest path ID the server issued an ID for
    u8 further_issued[kQuicMaxPaths][20] = {};
    usize further_bytes[kQuicMaxPaths] = {};
    u8 further_second[kQuicMaxPaths][20] = {}; // ... and each one's sequence 1
    usize further_second_bytes[kQuicMaxPaths] = {};
    usize path_acks = 0U;
    u64 path_ack_id = 0U;
    u64 path_ack_largest = 0U;
    usize path_slot = 0U;
    u8 path_next_cid[sizeof(kClientPathCid)] = {}; // a second ID of the raw client's for path ID 1, once given
    u64 path_next = 0U;
    // The server's PATH_ABANDON frames: how many, and the latest one's path ID, error code and path.
    usize abandons = 0U;
    u64 abandon_id = 0U;
    u64 abandon_error = 0U;
    bool abandon_further = false;
    // The server's MAX_PATH_ID frames: how many, and the latest one's Maximum Path Identifier and packet number.
    usize max_path_ids = 0U;
    u64 max_path_id = 0U;
    u64 max_path_id_packet = 0U;
    // The server's PATH_STATUS frames (the first 16), and the latest one's packet number.
    usize statuses = 0U;
    QuicFrame status[16] = {};
    u64 status_packet = 0U;
    // The server's ACK frames (path ID 0's, RFC 10069 section 2.3) in path ID 1's packets, and the latest one's
    // largest.
    usize acks_further = 0U;
    u64 ack_further_largest = 0U;
    // The server's OBSERVED_ADDRESS frames (the first 32): each one's sequence, address, path and packet number.
    usize observations = 0U;
    u64 observed_sequence[32] = {};
    Endpoint observed[32] = {};
    QuicPath observed_path[32] = {};
    u64 observed_packet[32] = {};
};

// (MSVC C4324 as for QuicPacketProtection, which the connection holds inline.)
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
class RawRun
{
public:
    explicit RawRun(const Identity& identity, crd::u32 sent_packets = 2048U, const QuicDatagramLimits& datagrams = {},
                    QuicConnectionIds* ids = nullptr, ConstSpan<u32> versions = {}, u64 receive_timestamps = 0U,
                    QuicAddressDiscovery server_discovery = QuicAddressDiscovery::Off, bool server_multipath = false,
                    u64 server_max_path_id = 0U)
        : server(allocator, server_config(identity, sent_packets, datagrams, ids, versions, receive_timestamps,
                                          server_discovery, server_multipath, server_max_path_id)),
          m_identity(identity)
    {
    }

    CountingAllocator allocator;
    QuicConnection server;
    RawClient raw;
    u64 now = kStartNs;
    ConstSpan<u8> scid;      // a Source Connection ID other than the client's, when set
    bool pad_initial = true; // false sends an Initial in a datagram under 1200 bytes
    QuicPath from;           // the path the raw client's datagrams arrive on (S75)
    usize pad_to = 0U;       // a 1-RTT packet's datagram is padded to this many bytes
    usize path_pad_to = kQuicMinInitialDatagram; // ... and one of send_on_path()
    QuicAddressDiscovery discovery = QuicAddressDiscovery::Off; // what the raw client's parameters offer
    bool multipath = false;  // the raw client's parameters offer initial_max_path_id (RFC 10069)
    u64 max_path_id = 3U;    // ... as this
    bool empty_scid = false; // the raw client's own connection ID is zero-length
    u8 last[kDatagram] = {}; // the raw client's latest datagram, as sent
    usize last_bytes = 0U;

    // Runs the handshake to the server's completion; `before_finished` runs with the client holding Handshake keys.
    template <typename Before> void handshake(Before before_finished)
    {
        QuicTransportParameters parameters;
        parameters.max_idle_timeout = 30000U;
        parameters.address_discovery = discovery;
        parameters.has_initial_max_path_id = multipath;
        parameters.initial_max_path_id = multipath ? max_path_id : 0U;
        parameters.has_initial_source_connection_id = true;
        REQUIRE(parameters.initial_source_connection_id.assign(own_cid()));
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
        accept.client_scid = own_cid();
        accept.original_dcid = accept.client_dcid;
        REQUIRE(server.accept(accept, now));
        send_crypto(QuicLevel::Initial);
        if (empty_scid && server.state() == QuicConnectionState::Closing)
        {
            return; // a zero-length ID refused with the client's parameters (RFC 10069 section 2.1)
        }
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
        spec.scid = scid.empty() ? own_cid() : scid;
        spec.number = raw.next[static_cast<usize>(quic_space_of(type))]++;
        REQUIRE(writer.begin(spec, Span<u8>(datagram, kQuicMinInitialDatagram)));
        const usize used = write(writer.frames());
        usize floor = (type == QuicPacketType::OneRtt) ? pad_to : 0U;
        floor = (type == QuicPacketType::Initial && pad_initial) ? kQuicMinInitialDatagram : floor;
        usize size = 0U;
        REQUIRE(raw.protection.seal(writer, used, {floor, 0U}, size) == QuicPacketError::None);
        std::memcpy(last, datagram, size);
        last_bytes = size;
        server.receive(from, Span<u8>(datagram, size), Ecn::NotEct, now);
        drain();
    }

    // One 1-RTT packet in the raw client's path slot (raw.path_slot, path ID 1) to `dcid` from `path`, padded to
    // path_pad_to bytes, with its authentication tag broken when `forged`, then the server's answers read.
    template <typename Write>
    void send_on_path(ConstSpan<u8> dcid, const QuicPath& path, Write write, bool forged = false)
    {
        u8 datagram[kDatagram] = {};
        QuicPacketWriter writer;
        QuicPacketSpec spec;
        spec.type = QuicPacketType::OneRtt;
        spec.dcid = dcid;
        spec.number = raw.path_next++;
        REQUIRE(writer.begin(spec, Span<u8>(datagram, kQuicMinInitialDatagram)));
        const usize used = write(writer.frames());
        usize size = 0U;
        REQUIRE(raw.protection.seal(writer, used, {path_pad_to, 0U}, size, raw.path_slot) == QuicPacketError::None);
        datagram[size - 1U] ^= forged ? 0x01U : 0x00U;
        server.receive(path, Span<u8>(datagram, size), Ecn::NotEct, now);
        drain();
    }

    // The raw client's Source Connection ID: kClientCid, or none with empty_scid.
    ConstSpan<u8> own_cid() const
    {
        return empty_scid ? ConstSpan<u8>() : ConstSpan<u8>(kClientCid, sizeof(kClientCid));
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
            if (raw.datagrams < 256U)
            {
                raw.sent_to[raw.datagrams] = server.sent_path();
                raw.sent_size[raw.datagrams] = size;
            }
            m_reading = server.sent_path();
            read(Span<u8>(datagram, size));
            if (raw.datagrams < 256U)
            {
                raw.sent_dcid[raw.datagrams] = m_dcid;
            }
            ++raw.datagrams;
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
            // A packet to kClientPathCid, or the raw client's next ID for path ID 1, is path ID 1's, opened in its
            // slot (RFC 10069 section 2).
            const bool further = raw.path_slot != 0U && header.type == QuicPacketType::OneRtt &&
                                 (same(header.dcid, ConstSpan<u8>(kClientPathCid, sizeof(kClientPathCid))) ||
                                  same(header.dcid, ConstSpan<u8>(raw.path_next_cid, sizeof(raw.path_next_cid))));
            QuicOpened opened;
            if (raw.protection.open(header, packet, opened, further ? raw.path_slot : 0U) != QuicPacketError::None)
            {
                continue;
            }
            m_further = further;
            m_dcid = header.dcid.empty() ? u8{0} : header.dcid[0];
            m_number = opened.number;
            const usize space = static_cast<usize>(quic_space_of(opened.type));
            raw.largest[space] =
                (raw.any[space] && raw.largest[space] > opened.number) ? raw.largest[space] : opened.number;
            raw.any[space] = true;
            bool eliciting = false;
            bool probing = true;
            bool response = false;
            WireReader in(opened.payload);
            while (!in.done())
            {
                QuicFrame frame;
                REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
                REQUIRE(quic_frame_allowed(frame.kind, frame.application, opened.type, true));
                eliciting = eliciting || quic_frame_ack_eliciting(frame.kind);
                probing = probing && quic_frame_probing(frame.kind);
                response = response || frame.kind == QuicFrameKind::PathResponse;
                take(frame, opened.type, datagram.size());
            }
            // A packet to another path than the active one of its path ID carries probing frames alone (section 9.1).
            const QuicPaths& paths = server.paths(further ? 1U : 0U);
            CHECK((probing || m_reading == paths.path(paths.active())));
            raw.responses_non_probing += (response && !probing) ? 1U : 0U;
            if (opened.type == QuicPacketType::OneRtt && !further && opened.number < sizeof(raw.seen))
            {
                raw.seen[opened.number] = true;
                raw.eliciting_seen += eliciting ? 1U : 0U;
            }
            if (further && opened.number < sizeof(raw.further_seen))
            {
                raw.further_seen[opened.number] = true;
            }
            REQUIRE(raw.protection.install(raw.handshake) == CryptoError::None);
        }
    }

    // An ACK of exactly the server's 1-RTT packets received, as a peer that acknowledges only ack-eliciting
    // packets sends it once one arrived.
    usize ack_received(Span<u8> out) const { return ack_seen(out, ConstSpan<bool>(raw.seen, sizeof(raw.seen)), false); }
    // A PATH_ACK of exactly the server's path ID 1 packets received.
    usize ack_further(Span<u8> out) const
    {
        return ack_seen(out, ConstSpan<bool>(raw.further_seen, sizeof(raw.further_seen)), true);
    }
    static usize ack_seen(Span<u8> out, ConstSpan<bool> seen, bool further)
    {
        QuicAckRange ranges[kQuicAckRanges];
        usize count = 0U;
        for (usize number = seen.size(); number-- > 0U && count < kQuicAckRanges;)
        {
            if (!seen[number])
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
        const ConstSpan<QuicAckRange> acked(ranges, count);
        if (count == 0U)
        {
            return 0U;
        }
        return further ? write_quic_path_ack(1U, acked, 0U, nullptr, out) : write_quic_ack(acked, 0U, nullptr, out);
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
                raw.close_dcid = m_dcid;
                break;
            case QuicFrameKind::RetireConnectionId:
            case QuicFrameKind::PathRetireConnectionId:
                REQUIRE(frame.path_id == 0U);
                raw.retire_seen = true;
                raw.retire_sequence = frame.sequence;
                ++raw.retirements;
                count_id_frame(frame.kind);
                break;
            case QuicFrameKind::NewConnectionId:
            case QuicFrameKind::PathNewConnectionId:
                REQUIRE(frame.sequence < 8U);
                raw.largest_issued = (frame.path_id > raw.largest_issued) ? frame.path_id : raw.largest_issued;
                if (frame.path_id != 0U && frame.path_id < kQuicMaxPaths)
                {
                    ++raw.further_ids[frame.path_id];
                    if (frame.sequence == 0U)
                    {
                        std::memcpy(raw.further_issued[frame.path_id], frame.data.data(), frame.data.size());
                        raw.further_bytes[frame.path_id] = frame.data.size();
                    }
                    if (frame.sequence == 1U)
                    {
                        std::memcpy(raw.further_second[frame.path_id], frame.data.data(), frame.data.size());
                        raw.further_second_bytes[frame.path_id] = frame.data.size();
                    }
                    count_id_frame(frame.kind);
                    break;
                }
                count_id_frame(frame.kind);
                std::memcpy(raw.issued[frame.sequence], frame.data.data(), frame.data.size());
                std::memcpy(raw.issued_tokens[frame.sequence], frame.reset_token.data(), kQuicResetTokenBytes);
                raw.issued_bytes[frame.sequence] = frame.data.size();
                raw.retire_prior_to = frame.retire_prior_to;
                break;
            case QuicFrameKind::PathChallenge:
                if (raw.challenges < 32U)
                {
                    std::memcpy(raw.challenge[raw.challenges], frame.data.data(), kQuicPathDataBytes);
                    raw.challenge_path[raw.challenges] = m_reading;
                    raw.challenge_datagram[raw.challenges] = datagram_size;
                    raw.challenge_dcid[raw.challenges] = m_dcid;
                }
                ++raw.challenges;
                break;
            case QuicFrameKind::PathAbandon:
                ++raw.abandons;
                raw.abandon_id = frame.path_id;
                raw.abandon_error = frame.error_code;
                raw.abandon_further = m_further;
                break;
            case QuicFrameKind::PathStatus:
                if (raw.statuses < 16U)
                {
                    raw.status[raw.statuses] = frame;
                }
                ++raw.statuses;
                raw.status_packet = m_number;
                break;
            case QuicFrameKind::MaxPathId:
                ++raw.max_path_ids;
                raw.max_path_id = frame.value;
                raw.max_path_id_packet = m_number;
                break;
            case QuicFrameKind::Ack:
                if (m_further)
                {
                    ++raw.acks_further;
                    raw.ack_further_largest = frame.ack.largest;
                }
                break;
            case QuicFrameKind::PathAck:
                // Each path ID's PATH_ACK goes on that path ID's own packets (the server's Turns, section 5.5), an
                // abandoned one's on path ID 0's (section 3.4.3).
                CHECK((m_further == (frame.path_id != 0U) || (!m_further && raw.abandons != 0U)));
                ++raw.path_acks;
                raw.path_ack_id = frame.path_id;
                raw.path_ack_largest = frame.ack.largest;
                break;
            case QuicFrameKind::PathResponse:
                raw.path_response_seen = true;
                std::memcpy(raw.path_response, frame.data.data(), kQuicPathDataBytes);
                raw.path_response_datagram = datagram_size;
                break;
            case QuicFrameKind::ObservedAddress:
                if (raw.observations < 32U)
                {
                    Endpoint& seen = raw.observed[raw.observations];
                    seen.address.family = (frame.data.size() == 4U) ? AddressFamily::V4 : AddressFamily::V6;
                    std::memcpy(seen.address.bytes, frame.data.data(), frame.data.size());
                    seen.port = static_cast<crd::u16>(frame.value);
                    raw.observed_sequence[raw.observations] = frame.sequence;
                    raw.observed_path[raw.observations] = m_reading;
                    raw.observed_packet[raw.observations] = m_number;
                }
                ++raw.observations;
                break;
            default:
                break;
        }
    }

    void count_id_frame(QuicFrameKind kind)
    {
        const bool path_form =
            kind == QuicFrameKind::PathNewConnectionId || kind == QuicFrameKind::PathRetireConnectionId;
        ++(path_form ? raw.path_id_frames : raw.plain_id_frames);
    }

    const Identity& m_identity;
    QuicPath m_reading;     // the path of the server datagram being read
    bool m_further = false; // the packet being read is path ID 1's
    u8 m_dcid = 0U;         // the first byte of its Destination Connection ID
    u64 m_number = 0U;  // the number of the server packet being read
};
#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif

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

// Sends an ACK_RECEIVE_TIMESTAMPS frame in a packet of `type` acknowledging what the server sent in its space (the
// 1-RTT packets the raw client opened, every Handshake number up to the largest), timing the `nth` largest of them
// (repeats allowed), the first `first` microseconds past the basis and each next `step` earlier.
void send_timed_ack(RawRun& run, QuicPacketType type, std::initializer_list<usize> nth, u64 first, u64 step)
{
    u64 numbers[64];
    usize known = 0U;
    if (type == QuicPacketType::OneRtt)
    {
        for (usize number = sizeof(run.raw.seen); number-- > 0U && known < 64U;)
        {
            if (run.raw.seen[number])
            {
                numbers[known++] = number;
            }
        }
    }
    else
    {
        for (u64 number = run.raw.largest[static_cast<usize>(quic_space_of(type))] + 1U; number-- > 0U && known < 64U;)
        {
            numbers[known++] = number;
        }
    }
    REQUIRE(known != 0U);
    QuicAckRange ranges[64];
    usize range_count = 0U;
    for (usize index = 0U; index < known; ++index)
    {
        if (range_count > 0U && ranges[range_count - 1U].smallest == numbers[index] + 1U)
        {
            ranges[range_count - 1U].smallest = numbers[index];
        }
        else
        {
            ranges[range_count++] = QuicAckRange{numbers[index], numbers[index]};
        }
    }
    QuicReceiveTimestamp stamps[8];
    usize count = 0U;
    for (const usize index : nth)
    {
        REQUIRE((count < 8U && index < known));
        stamps[count] = QuicReceiveTimestamp{numbers[index], first - (count * step)};
        ++count;
    }
    run.send(type,
             [&](Span<u8> out)
             {
                 const usize bytes = write_quic_ack(ConstSpan<QuicAckRange>(ranges, range_count), 0U, nullptr, out,
                                                    ConstSpan<QuicReceiveTimestamp>(stamps, count));
                 REQUIRE(bytes != 0U);
                 return bytes;
             });
}
} // namespace

TEST_CASE("QUIC connection: ACK_RECEIVE_TIMESTAMPS only where asked for, in 1-RTT packets and within the maximum",
          "[net][net-path][quic-connection][receive-timestamps]")
{
    Identity identity;
    // Never asked for: a frame of no known type.
    {
        RawRun run(identity);
        run.handshake();
        send_timed_ack(run, QuicPacketType::OneRtt, {0U}, 5000U, 0U);
        REQUIRE(run.server.state() == QuicConnectionState::Closing);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::FrameEncodingError));
    }
    // Asked for four: one timestamp is taken; five are past the maximum.
    {
        RawRun run(identity, 2048U, {}, nullptr, {}, 4U);
        run.handshake();
        CHECK(run.server.peer_parameters().max_receive_timestamps_per_ack == 0U);
        run.server.ping();
        run.drain();
        send_timed_ack(run, QuicPacketType::OneRtt, {0U}, 5000U, 0U);
        CHECK(run.server.state() == QuicConnectionState::Established);
        CHECK(run.server.counters().receive_timestamps == 1U);
        run.server.ping();
        run.drain();
        send_timed_ack(run, QuicPacketType::OneRtt, {0U, 0U, 0U, 0U, 0U}, 9000U, 1U);
        REQUIRE(run.server.state() == QuicConnectionState::Closing);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
    // One-way delays a second apart for packets sent at one time: implausible, none taken, nothing closed.
    {
        RawRun run(identity, 2048U, {}, nullptr, {}, 4U);
        run.handshake();
        run.server.ping();
        run.drain();
        run.server.ping();
        run.drain();
        send_timed_ack(run, QuicPacketType::OneRtt, {0U, 1U}, 2000000U, 1000000U);
        CHECK(run.server.state() == QuicConnectionState::Established);
        CHECK(run.server.counters().receive_timestamps == 0U);
    }
    // In a Handshake packet: the draft's frame belongs to 1-RTT packets.
    {
        RawRun run(identity, 2048U, {}, nullptr, {}, 4U);
        run.handshake([&] { send_timed_ack(run, QuicPacketType::Handshake, {0U}, 5000U, 0U); });
        REQUIRE(run.server.state() == QuicConnectionState::Closing);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
}

TEST_CASE("QUIC connection: multipath offered by the client alone is not negotiated, and its frames are an "
          "unnegotiated extension's",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069's frames, each with valid fields (PATH_ACK from write_quic_path_ack).
    const u8 id[8] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7};
    const u8 token[16] = {};
    const auto frames = [&](usize index, Span<u8> out) -> usize
    {
        QuicFrame frame;
        frame.path_id = 1U;
        switch (index)
        {
        case 0U:
        {
            const QuicAckRange range{0U, 0U};
            return write_quic_path_ack(0U, ConstSpan<QuicAckRange>(&range, 1U), 0U, nullptr, out);
        }
        case 1U:
            frame.kind = QuicFrameKind::PathAbandon;
            break;
        case 2U:
            frame.kind = QuicFrameKind::PathStatus;
            frame.backup = true;
            break;
        case 3U:
            frame.kind = QuicFrameKind::PathNewConnectionId;
            frame.data = ConstSpan<u8>(id, sizeof(id));
            frame.reset_token = ConstSpan<u8>(token, sizeof(token));
            break;
        case 4U:
            frame.kind = QuicFrameKind::PathRetireConnectionId;
            break;
        case 5U:
            frame.kind = QuicFrameKind::MaxPathId;
            frame.value = 4U;
            break;
        case 6U:
            frame.kind = QuicFrameKind::PathsBlocked;
            frame.value = 3U;
            break;
        default:
            frame.kind = QuicFrameKind::PathCidsBlocked;
            break;
        }
        return write_quic_frame(frame, out);
    };
    Identity identity;
    for (usize index = 0U; index < 8U; ++index)
    {
        INFO("frame " << index);
        // A client that offers initial_max_path_id still completes its handshake; the server offers none, so
        // multipath is never negotiated and each frame is of no known type (RFC 9000 section 12.4).
        RawRun run(identity);
        run.multipath = true;
        run.handshake();
        CHECK(run.server.peer_parameters().has_initial_max_path_id);
        QuicTransportParameters offered;
        REQUIRE(read_quic_transport_parameters(run.raw.handshake.peer_transport_parameters(), QuicSide::Server,
                                               offered) == QuicTransportError::NoError);
        CHECK_FALSE(offered.has_initial_max_path_id);
        CHECK_FALSE(quic_multipath_negotiated(offered, run.server.peer_parameters()));
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return frames(index, out); });
        REQUIRE(run.server.state() == QuicConnectionState::Closing);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::FrameEncodingError));
        CHECK(run.raw.close_code == run.server.close_info().error_code);
    }
    // In a Handshake packet each is PROTOCOL_VIOLATION (RFC 10069 section 4: 1-RTT packets alone).
    for (usize index = 0U; index < 8U; ++index)
    {
        INFO("frame " << index);
        RawRun run(identity);
        run.handshake([&] { run.send(QuicPacketType::Handshake, [&](Span<u8> out) { return frames(index, out); }); });
        REQUIRE(run.server.state() == QuicConnectionState::Closing);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
}

namespace
{
// A raw client offering initial_max_path_id 3 to a server offering 0: multipath negotiated (RFC 10069 section 2).
void negotiate(RawRun& run)
{
    run.multipath = true;
    run.handshake();
    run.drain();
    REQUIRE(run.server.multipath());
}

void send_frame(RawRun& run, const QuicFrame& frame)
{
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(frame, out); });
}

void send_path_ack(RawRun& run, u64 path_id, ConstSpan<QuicAckRange> ranges)
{
    run.send(QuicPacketType::OneRtt,
             [&](Span<u8> out) { return write_quic_path_ack(path_id, ranges, 0U, nullptr, out); });
}

QuicFrame path_frame(QuicFrameKind kind, u64 path_id = 0U, u64 sequence = 0U, u64 value = 0U)
{
    QuicFrame frame;
    frame.kind = kind;
    frame.path_id = path_id;
    frame.sequence = sequence;
    frame.value = value;
    return frame;
}

constexpr u8 kPathCid[8] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7}; // not kClientCid
constexpr u8 kPathToken[kQuicResetTokenBytes] = {1U};
} // namespace

TEST_CASE("QUIC connection: multipath negotiated for path ID 0 reads RFC 10069's frames under this side's limit",
          "[net][net-path][quic-connection][multipath]")
{
    Identity identity;
    {
        RawRun run(identity, 2048U, {}, nullptr, {}, 0U, QuicAddressDiscovery::Off, true);
        negotiate(run);
        QuicTransportParameters offered;
        REQUIRE(read_quic_transport_parameters(run.raw.handshake.peer_transport_parameters(), QuicSide::Server,
                                               offered) == QuicTransportError::NoError);
        CHECK(offered.has_initial_max_path_id);
        CHECK(offered.initial_max_path_id == 0U);
        CHECK(run.server.peer_max_path_id() == 3U);
        CHECK(run.server.peer_path_status() == QuicPathStatus::Unknown);
        // MAX_PATH_ID raises the peer's limit and is ignored when it does not (section 4.6).
        send_frame(run, path_frame(QuicFrameKind::MaxPathId, 0U, 0U, 3U));
        CHECK(run.server.peer_max_path_id() == 3U);
        send_frame(run, path_frame(QuicFrameKind::MaxPathId, 0U, 0U, 9U));
        CHECK(run.server.peer_max_path_id() == 9U);
        send_frame(run, path_frame(QuicFrameKind::MaxPathId, 0U, 0U, 5U));
        CHECK(run.server.peer_max_path_id() == 9U);
        // PATH_STATUS: the newest sequence wins, an older or repeated one is ignored (section 4.3).
        QuicFrame status = path_frame(QuicFrameKind::PathStatus, 0U, 5U);
        status.backup = true;
        send_frame(run, status);
        CHECK(run.server.peer_path_status() == QuicPathStatus::Backup);
        status.backup = false;
        status.sequence = 4U;
        send_frame(run, status);
        status.sequence = 5U;
        send_frame(run, status);
        CHECK(run.server.peer_path_status() == QuicPathStatus::Backup);
        status.sequence = 6U;
        send_frame(run, status);
        CHECK(run.server.peer_path_status() == QuicPathStatus::Available);
        CHECK(run.server.peer_path_status(1U) == QuicPathStatus::Unknown);
        // The BLOCKED frames at this side's limit and next sequence inform only (section 4.7).
        send_frame(run, path_frame(QuicFrameKind::PathsBlocked));
        send_frame(run, path_frame(QuicFrameKind::PathCidsBlocked, 0U, 1U));
        // PATH_NEW_CONNECTION_ID of path ID 0 is held as NEW_CONNECTION_ID is.
        QuicFrame issued = path_frame(QuicFrameKind::PathNewConnectionId, 0U, 1U);
        issued.data = ConstSpan<u8>(kPathCid, sizeof(kPathCid));
        issued.reset_token = ConstSpan<u8>(kPathToken, sizeof(kPathToken));
        send_frame(run, issued);
        CHECK(run.server.peer_cid_count() == 2U);
        // PATH_ACK of path ID 0 acknowledges the 1-RTT space as an ACK frame does (section 2.3).
        const u64 in_flight = run.server.recovery().bytes_in_flight();
        REQUIRE(in_flight > 0U);
        QuicAckRange ranges[kQuicAckRanges];
        usize count = 0U;
        for (usize number = sizeof(run.raw.seen); number-- > 0U && count < kQuicAckRanges;)
        {
            if (!run.raw.seen[number])
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
        REQUIRE(count > 0U);
        send_path_ack(run, 0U, ConstSpan<QuicAckRange>(ranges, count));
        CHECK(run.server.recovery().bytes_in_flight() < in_flight);
        CHECK(run.server.state() == QuicConnectionState::Established);
    }
    // Refusals: a path ID above this side's limit of 0 in any frame, a PATHS_BLOCKED above it, a MAX_PATH_ID below the
    // peer's initial_max_path_id, a PATH_CIDS_BLOCKED past the next sequence, a retirement of a sequence never issued
    // and a PATH_ACK of a packet never sent: PROTOCOL_VIOLATION (sections 4, 4.5 to 4.7; RFC 9000 section 13.1).
    for (usize index = 0U; index < 12U; ++index)
    {
        INFO("refusal " << index);
        RawRun run(identity, 2048U, {}, nullptr, {}, 0U, QuicAddressDiscovery::Off, true);
        negotiate(run);
        const QuicAckRange first{0U, 0U};
        const QuicAckRange unsent{1000U, 1000U};
        QuicFrame frame;
        switch (index)
        {
            case 0U:
                send_path_ack(run, 1U, ConstSpan<QuicAckRange>(&first, 1U));
                break;
            case 1U:
                frame = path_frame(QuicFrameKind::PathAbandon, 1U);
                break;
            case 2U:
                frame = path_frame(QuicFrameKind::PathStatus, 1U);
                break;
            case 3U:
                frame = path_frame(QuicFrameKind::PathNewConnectionId, 1U);
                frame.data = ConstSpan<u8>(kPathCid, sizeof(kPathCid));
                frame.reset_token = ConstSpan<u8>(kPathToken, sizeof(kPathToken));
                break;
            case 4U:
                frame = path_frame(QuicFrameKind::PathRetireConnectionId, 1U);
                break;
            case 5U:
                frame = path_frame(QuicFrameKind::PathCidsBlocked, 1U);
                break;
            case 6U:
                frame = path_frame(QuicFrameKind::PathsBlocked, 0U, 0U, 1U);
                break;
            case 7U:
                frame = path_frame(QuicFrameKind::MaxPathId, 0U, 0U, 2U);
                break;
            case 8U:
                frame = path_frame(QuicFrameKind::PathCidsBlocked, 0U, 2U);
                break;
            case 9U:
                frame = path_frame(QuicFrameKind::PathRetireConnectionId, 0U, 5U);
                break;
            case 10U:
                send_path_ack(run, 0U, ConstSpan<QuicAckRange>(&unsent, 1U));
                break;
            default:
                frame = path_frame(QuicFrameKind::PathAbandon, kQuicMaxPathId);
                break;
        }
        if (index != 0U && index != 10U)
        {
            send_frame(run, frame);
        }
        REQUIRE(run.server.state() == QuicConnectionState::Closing);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
        CHECK(run.raw.close_code == run.server.close_info().error_code);
    }
    {
        // PATH_ABANDON of path ID 0, the only path, closes the connection without an error (section 3.4).
        RawRun run(identity, 2048U, {}, nullptr, {}, 0U, QuicAddressDiscovery::Off, true);
        negotiate(run);
        send_frame(run, path_frame(QuicFrameKind::PathAbandon));
        REQUIRE(run.server.state() == QuicConnectionState::Closing);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::NoError));
        CHECK(run.raw.close_seen);
    }
}

TEST_CASE("QUIC connection: multipath needs connection IDs of non-zero length both ways",
          "[net][net-path][quic-connection][multipath]")
{
    Identity identity;
    // A client offering multipath with a zero-length Source Connection ID to a server offering it: PROTOCOL_VIOLATION
    // once the server reads its parameters (RFC 10069 section 2.1).
    RawRun run(identity, 2048U, {}, nullptr, {}, 0U, QuicAddressDiscovery::Off, true);
    run.multipath = true;
    run.empty_scid = true;
    run.handshake();
    REQUIRE(run.server.state() == QuicConnectionState::Closing);
    CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    CHECK_FALSE(run.server.multipath());
    // Without the server's offer the zero-length ID is RFC 9000's to allow.
    RawRun plain(identity);
    plain.multipath = true;
    plain.empty_scid = true;
    plain.handshake();
    CHECK(plain.server.state() == QuicConnectionState::Established);
    CHECK_FALSE(plain.server.multipath());
}

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
        const usize retirements = run.raw.retirements;
        issue(run, 1U, 0U, id3, token1); // below Retire Prior To, its retirement in flight: not retired again
        CHECK(run.server.state() == QuicConnectionState::Established);
        CHECK(run.raw.retirements == retirements);
        // Retirements owed or unacknowledged stay within twice the limit, counted once per ID (section 5.1.2): with 0
        // and 1 in flight, the third and the fourth are allowed, the fifth refused.
        constexpr u8 id4[8] = {0x44U, 4U};
        constexpr u8 id5[8] = {0x55U, 5U};
        constexpr u8 token3[kQuicResetTokenBytes] = {0xA3U};
        constexpr u8 token4[kQuicResetTokenBytes] = {0xA4U};
        constexpr u8 token5[kQuicResetTokenBytes] = {0xA5U};
        issue(run, 3U, 3U, id3, token3);
        issue(run, 4U, 4U, id4, token4);
        CHECK(run.server.state() == QuicConnectionState::Established);
        CHECK(run.raw.retire_sequence == 3U);
        issue(run, 5U, 5U, id5, token5);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ConnectionIdLimitError));
        // The refused frame changed nothing: the close goes to sequence 4, which the peer routes.
        run.drain();
        CHECK(run.raw.close_seen);
        CHECK(run.raw.close_code == code(QuicTransportError::ConnectionIdLimitError));
        CHECK(run.raw.close_dcid == id4[0]);
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

// Each end fills its sent packet storage with stream data before any acknowledgement can reach it.
struct FullStorageRun
{
    CountingAllocator allocator;
    Identity identity;
    QuicConnection client;
    QuicConnection server;
    std::unique_ptr<Net> net;

    FullStorageRun()
        : client(allocator, limited_config(QuicSide::Client, identity, 0U)),
          server(allocator, limited_config(QuicSide::Server, identity, 1U))
    {
        connect(client);
        net = std::make_unique<Net>(client, server);
        net->path[0].delay_ns = 40U * kMs;
        net->path[1].delay_ns = 40U * kMs;
        REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
        client.ping();
        server.ping();
        net->advance(net->now + (2000U * kMs)); // every earlier packet settled and its entry free
        for (QuicConnection* const end : {&client, &server})
        {
            for (usize count = 0U; count < 2U; ++count)
            {
                u64 stream = 0U;
                REQUIRE(end->streams().open(true, stream) == QuicStreamError::None);
                u64 written = 0U;
                static_cast<void>(write_pattern(end->streams(), stream, written, 8000U, stream, false));
            }
        }
    }

    static QuicConnectionConfig limited_config(QuicSide side, const Identity& identity, usize seed)
    {
        QuicConnectionConfig config = config_for(side, identity, QuicVersion::V1, seed);
        config.streams = stream_limits();
        config.sent_packets = 8U;
        return config;
    }

    [[nodiscard]] bool drained() const
    {
        return client.recovery().bytes_in_flight() == 0U && server.recovery().bytes_in_flight() == 0U;
    }
};

TEST_CASE("QUIC connection: two ends full of each other's unacknowledged data still acknowledge it",
          "[net][4.2a][quic-connection]")
{
    FullStorageRun run;
    run.net->flush();
    CHECK_FALSE(run.client.recovery().can_send_data(QuicSpace::Application));
    CHECK_FALSE(run.server.recovery().can_send_data(QuicSpace::Application));
    const usize client_sent = run.net->sent[0];
    const usize server_sent = run.net->sent[1];
    run.net->advance(run.net->now + (100U * kMs)); // every packet arrived, no probe is due yet
    CHECK(run.net->sent[0] > client_sent);
    CHECK(run.net->sent[1] > server_sent);
    REQUIRE(run.net->run_until([&] { return run.drained(); }, 20000U * kMs));
    CHECK(run.client.state() == QuicConnectionState::Established);
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a space full of unacknowledged packets still acknowledges what arrives",
          "[net][4.2a][quic-connection]")
{
    Identity identity;
    RawRun run(identity, 8U); // the raw peer acknowledges nothing
    run.handshake();
    u8 out[kDatagram];
    for (usize round = 0U; round < 32U && run.server.recovery().can_send_data(QuicSpace::Application); ++round)
    {
        run.server.ping();
        while (run.server.send(Span<u8>(out, sizeof(out)), run.now) != 0U)
        {
        }
        run.now += kMs;
    }
    REQUIRE_FALSE(run.server.recovery().can_send_data(QuicSpace::Application));
    QuicFrame ping;
    ping.kind = QuicFrameKind::Ping;
    run.send(QuicPacketType::OneRtt, [&](Span<u8> frames) { return write_quic_frame(ping, frames); });
    run.now += 100U * kMs; // past the acknowledgement delay
    while (run.server.send(Span<u8>(out, sizeof(out)), run.now) != 0U)
    {
    }
    CHECK(run.server.recovery().ack_deadline_ns(QuicSpace::Application) > run.now); // it went: none is owed
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a space full of data that was lost still sends its probes",
          "[net][4.2a][quic-connection]")
{
    FullStorageRun run;
    run.net->path[0].loss = 1000U;
    run.net->path[1].loss = 1000U;
    run.net->flush();
    CHECK_FALSE(run.client.recovery().can_send_data(QuicSpace::Application));
    const usize sent_before = run.net->sent[0];
    run.net->advance(run.net->now + (2000U * kMs));
    CHECK(run.net->sent[0] > sent_before); // the probes went, into the entries data may not take
    run.net->path[0].loss = 0U;
    run.net->path[1].loss = 0U;
    REQUIRE(run.net->run_until([&] { return run.drained(); }, 20000U * kMs));
    CHECK(run.client.state() == QuicConnectionState::Established);
    CHECK(run.server.state() == QuicConnectionState::Established);
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

TEST_CASE("QUIC connection: a reliable reset delivers its bytes and then the reset over hostile paths, both versions",
          "[net][4.2a][quic-connection]")
{
    for (const QuicVersion version : kVersions)
    {
        for (u64 seed = 1U; seed <= 8U; ++seed)
        {
            Identity identity;
            CountingAllocator allocator;
            QuicConnectionConfig client_config = stream_config(QuicSide::Client, identity, version, seed);
            client_config.streams.reset_stream_at = true;
            QuicConnectionConfig server_config = stream_config(QuicSide::Server, identity, version, seed + 1U);
            server_config.streams.reset_stream_at = true;
            QuicConnection client(allocator, client_config);
            QuicConnection server(allocator, server_config);
            REQUIRE(server.ready());
            connect(client);
            auto net = std::make_unique<Net>(client, server);
            net->rng.state = seed;
            for (Path& path : net->path)
            {
                path.loss = 150U;
                path.duplicate = 50U;
                path.delay_ns = 10U * kMs;
                path.jitter_ns = 15U * kMs;
            }
            REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
            CHECK(client.peer_parameters().reset_stream_at);
            CHECK(server.peer_parameters().reset_stream_at);

            u64 stream = 0U;
            REQUIRE(client.streams().open(true, stream) == QuicStreamError::None);
            u64 written = 0U;
            static_cast<void>(write_pattern(client.streams(), stream, written, 4000U, stream, false));
            const u64 reliable = 1500U + (seed * 100U);
            REQUIRE(client.streams().reset(stream, 0x2AU, reliable) == QuicStreamError::None);

            u64 read = 0U;
            u64 code = 0U;
            const auto drained = [&]
            {
                QuicStreamRead part = server.streams().peek(stream);
                for (; !part.data.empty(); part = server.streams().peek(stream))
                {
                    for (usize at = 0U; at < part.data.size(); ++at)
                    {
                        REQUIRE(part.data[at] == pattern(stream, read + at));
                    }
                    read += part.data.size();
                    REQUIRE(server.streams().consume(stream, part.data.size()) == QuicStreamError::None);
                }
                if (part.reset)
                {
                    code = part.reset_error;
                }
                return part.reset && client.streams().send_state(stream) == QuicSendState::ResetRecvd;
            };
            REQUIRE(net->run_until(drained, 600000U * kMs));
            CHECK(read == reliable);
            CHECK(code == 0x2AU);
            CHECK(client.state() == QuicConnectionState::Established);
            CHECK(server.state() == QuicConnectionState::Established);
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
    server_config.max_datagram_bytes = kQuicMinInitialDatagram; // the server's own ceiling the other way
    server_config.datagrams.max_frame_bytes = 1400U;
    QuicConnection client(allocator, client_config);
    QuicConnection server(allocator, server_config);
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
    // The client's path MTU search reaches the server's 1250 bytes (one probe on this clean path).
    REQUIRE(net->run_until([&] { return client.path_mtu().state() == QuicPmtuState::SearchComplete; }));
    CHECK(client.path_mtu().size() == 1250U);
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
        // At the floor, where the room a packet has, not the peer's frame size, bounds the largest datagram.
        QuicConnectionConfig client_config = datagram_config(QuicSide::Client, identity, QuicVersion::V1, 0U, false);
        QuicConnectionConfig server_config = datagram_config(QuicSide::Server, identity, QuicVersion::V1, 1U, false);
        client_config.max_datagram_bytes = kQuicMinInitialDatagram;
        server_config.max_datagram_bytes = kQuicMinInitialDatagram;
        QuicConnection client(allocator, client_config);
        QuicConnection server(allocator, server_config);
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
            server.receive(Span<u8>(out, size), Ecn::NotEct, net->now);
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
        client.receive(Span<u8>(out, size), Ecn::NotEct, net->now);
        // A carried ACK waits behind the datagram; an owed one goes first and the datagram takes the next packet.
        CHECK(client.datagrams().held() == (owed ? 0U : 1U));
        const bool eliciting = server.recovery().bytes_in_flight() > in_flight; // a DATAGRAM frame elicits an ACK
        CHECK(eliciting == !owed);
        if (owed)
        {
            size = server.send(Span<u8>(out, sizeof(out)), net->now);
            REQUIRE(size != 0U);
            client.receive(Span<u8>(out, size), Ecn::NotEct, net->now);
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

TEST_CASE("QUIC connection: with multipath, path ID 0's connection IDs go in the PATH_ frames, and the plain ones "
          "still count for path ID 0",
          "[net][net-path][quic-connection][multipath]")
{
    Identity identity;
    // Each offer combination: negotiated only when both ends offered (RFC 10069 section 2).
    for (usize offers = 0U; offers < 4U; ++offers)
    {
        const bool client_offers = (offers & 1U) != 0U;
        const bool server_offers = (offers & 2U) != 0U;
        const bool multipath = client_offers && server_offers;
        INFO("client offers " << client_offers << ", server offers " << server_offers);
        CountingAllocator allocator;
        Host host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
        RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, server_offers);
        run.multipath = client_offers;
        run.handshake();
        run.drain();
        CHECK(run.server.multipath() == multipath);
        // The raw client's limit is the default 2: sequence 1 is issued, in the form negotiation names.
        REQUIRE(run.raw.issued_bytes[1] == sizeof(kServerCid));
        CHECK(run.raw.plain_id_frames == (multipath ? 0U : 1U));
        CHECK(run.raw.path_id_frames == (multipath ? 1U : 0U));
        if (!multipath)
        {
            // One end's offer alone: a multipath frame is of no known type (RFC 9000 section 12.4).
            send_frame(run, path_frame(QuicFrameKind::PathRetireConnectionId, 0U, 1U));
            REQUIRE(run.server.state() == QuicConnectionState::Closing);
            CHECK(run.server.close_info().error_code == code(QuicTransportError::FrameEncodingError));
            continue;
        }
        // Path ID 0's IDs retire by either form; each replacement goes in the negotiated one.
        send_frame(run, path_frame(QuicFrameKind::PathRetireConnectionId, 0U, 1U));
        CHECK(host.retired == 1U);
        REQUIRE(run.raw.issued_bytes[2] == sizeof(kServerCid));
        send_frame(run, path_frame(QuicFrameKind::RetireConnectionId, 0U, 2U));
        CHECK(host.retired == 2U);
        REQUIRE(run.raw.issued_bytes[3] == sizeof(kServerCid));
        CHECK(run.raw.plain_id_frames == 0U);
        CHECK(run.raw.path_id_frames == 3U);
        CHECK(run.server.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC connection: two ends offering multipath negotiate it, keep their IDs topped up and rotate them",
          "[net][net-path][quic-connection][multipath]")
{
    Identity identity;
    for (const bool server_offers : {false, true})
    {
        INFO("server offers " << server_offers);
        CountingAllocator allocator;
        Host client_host(allocator, 0U, ConstSpan<u8>(kClientCid, sizeof(kClientCid)));
        Host server_host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
        QuicConnectionConfig client_config =
            hosted(config_for(QuicSide::Client, identity, QuicVersion::V1), client_host, 4U);
        QuicConnectionConfig server_config =
            hosted(config_for(QuicSide::Server, identity, QuicVersion::V1, 1U), server_host, 2U);
        client_config.parameters.has_initial_max_path_id = true;
        server_config.parameters.has_initial_max_path_id = server_offers;
        QuicConnection client(allocator, client_config);
        QuicConnection server(allocator, server_config);
        connect(client);
        auto net = std::make_unique<Net>(client, server);
        settle(*net, client, server, 2U, 4U);
        CHECK(client.multipath() == server_offers);
        CHECK(server.multipath() == server_offers);
        CHECK(client.peer_max_path_id() == 0U);
        CHECK(server.peer_max_path_id() == 0U);
        REQUIRE(server.rotate_connection_ids());
        REQUIRE(net->run_until([&] { return server.local_cid_count() == 4U && net->in_flight() == 0U; }));
        net->advance(net->now + 200U * kMs);
        CHECK(server_host.retired == 4U);
        CHECK(client.peer_cid_count() == 4U);
        const u64 received = server.counters().datagrams_received;
        client.ping();
        REQUIRE(
            net->run_until([&] { return net->in_flight() == 0U && server.counters().datagrams_received > received; }));
        CHECK(client.state() == QuicConnectionState::Established);
        CHECK(server.state() == QuicConnectionState::Established);
    }
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
        client.receive(tiny.view(), Ecn::NotEct, pair.net->now);
        ResetDatagram unused(pair.server_host.tokens[0], 60U);
        const u64 dropped = client.counters().packets_dropped;
        client.receive(unused.view(), Ecn::NotEct, pair.net->now);
        CHECK(client.counters().packets_dropped == dropped + 1U);
        ResetDatagram forged(wrong, 60U);
        client.receive(forged.view(), Ecn::NotEct, pair.net->now);
        CHECK_FALSE(client.stateless_reset(ConstSpan<u8>(forged.bytes, forged.size), pair.net->now));
        CHECK(client.state() == QuicConnectionState::Established);

        // The token of the ID in use, at the least size: the client drains, sends nothing and ends.
        ResetDatagram reset(used, kQuicMinResetBytes);
        client.receive(reset.view(), Ecn::NotEct, pair.net->now);
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
        client.receive(reset.view(), Ecn::NotEct, pair.net->now);
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
        Discovery,    // address discovery turned on: a server never changes it under accepted 0-RTT (the draft's 3)
    };
    const u32 client_versions[] = {static_cast<u32>(QuicVersion::V1), static_cast<u32>(QuicVersion::V2)};
    const u32 server_versions[] = {static_cast<u32>(QuicVersion::V2), static_cast<u32>(QuicVersion::V1)};
    for (const Refusal refusal :
         {Refusal::NoEarlyData, Refusal::OtherContext, Refusal::LowerLimit, Refusal::Switch, Refusal::Discovery})
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
            case Refusal::Discovery:
                server_config.parameters.address_discovery = QuicAddressDiscovery::Both;
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
            to.receive(Span<u8>(flight[index].bytes, flight[index].size), Ecn::NotEct, kStartNs);
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
            client.receive(Span<u8>(answer[index].bytes, answer[index].size), Ecn::NotEct, kStartNs);
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

// ---- Paths and migration (S75) ----

namespace
{
QuicPath client_at(u8 last, crd::u16 port)
{
    return QuicPath{0U, Endpoint{IpAddress::v4(198, 51, 100, last), port}};
}

void send_frame(RawRun& run, QuicFrameKind kind, ConstSpan<u8> data = {})
{
    QuicFrame frame;
    frame.kind = kind;
    frame.data = data;
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(frame, out); });
}

// The latest challenge the server sent on `path`: its index in the raw client's log, or 32 for none.
usize latest_challenge(const RawRun& run, const QuicPath& path)
{
    const usize logged = (run.raw.challenges < 32U) ? run.raw.challenges : 32U;
    for (usize index = logged; index-- > 0U;)
    {
        if (run.raw.challenge_path[index] == path)
        {
            return index;
        }
    }
    return 32U;
}

// Answers the server's latest challenge on `on` with a PATH_RESPONSE (a probing packet) from the raw client's path.
void answer(RawRun& run, const QuicPath& on)
{
    const usize index = latest_challenge(run, on);
    REQUIRE(index < 32U);
    send_frame(run, QuicFrameKind::PathResponse, ConstSpan<u8>(run.raw.challenge[index], kQuicPathDataBytes));
}

// The server's challenges sent on `path`.
usize challenges_on(const RawRun& run, const QuicPath& path)
{
    usize count = 0U;
    for (usize index = 0U; index < run.raw.challenges && index < 32U; ++index)
    {
        count += (run.raw.challenge_path[index] == path) ? 1U : 0U;
    }
    return count;
}

// The server's bytes sent on `path` since datagram `from` of the raw client's log.
usize bytes_to(const RawRun& run, const QuicPath& path, usize from)
{
    usize total = 0U;
    for (usize index = from; index < run.raw.datagrams && index < 256U; ++index)
    {
        total += (run.raw.sent_to[index] == path) ? run.raw.sent_size[index] : 0U;
    }
    return total;
}

// Runs the server's timers, reading what it sends, until `until`.
void run_timers(RawRun& run, u64 until)
{
    for (u64 next = run.server.next_timer_ns(); next <= until; next = run.server.next_timer_ns())
    {
        run.now = (next > run.now) ? next : run.now;
        run.server.on_timeout(run.now);
        run.drain();
        if (run.server.state() == QuicConnectionState::Closed)
        {
            return;
        }
    }
    run.now = until;
}
} // namespace

TEST_CASE("QUIC paths: a probing packet from a new path is answered there with a challenge of the server's, padded, "
          "and moves nothing",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    RawRun run(identity);
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath b = client_at(8U, 50000U);
    run.from = a;
    run.handshake();
    REQUIRE(run.server.path() == a);
    CHECK(run.server.address_validated());
    run.from = b;
    run.pad_to = kQuicMinInitialDatagram;
    const u8 data[kQuicPathDataBytes] = {0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U};
    send_frame(run, QuicFrameKind::PathChallenge, ConstSpan<u8>(data));
    CHECK(run.server.path() == a);
    CHECK(run.server.counters().migrations == 0U);
    REQUIRE(run.raw.path_response_seen);
    CHECK(std::memcmp(run.raw.path_response, data, sizeof(data)) == 0);
    CHECK(run.raw.path_response_datagram >= kQuicMinInitialDatagram);
    const usize challenge = latest_challenge(run, b);
    REQUIRE(challenge < 32U); // section 9.6.3: the server validates the client's new address
    CHECK(run.raw.challenge_datagram[challenge] >= kQuicMinInitialDatagram);
    const usize b_slot = run.server.paths().find(b);
    REQUIRE(b_slot != kQuicNoPath);
    CHECK_FALSE(run.server.paths().validated(b_slot));
    CHECK(bytes_to(run, b, 0U) <= 3U * kQuicMinInitialDatagram);
    answer(run, b);
    CHECK(run.server.paths().validated(b_slot));
    CHECK(run.server.counters().path_validations == 1U);
    CHECK(run.server.path() == a);
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC paths: a PATH_RESPONSE that answers no challenge is ignored, and a challenge on the active path is "
          "answered with a non-probing packet",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    RawRun run(identity);
    run.from = client_at(7U, 50000U);
    run.handshake();
    const u8 data[kQuicPathDataBytes] = {9U};
    send_frame(run, QuicFrameKind::PathResponse, ConstSpan<u8>(data));
    CHECK(run.server.state() == QuicConnectionState::Established);
    CHECK(run.server.counters().path_validations == 0U);
    send_frame(run, QuicFrameKind::PathChallenge, ConstSpan<u8>(data));
    REQUIRE(run.raw.path_response_seen);
    CHECK(run.raw.responses_non_probing == 1U);
    CHECK(run.raw.path_response_datagram >= kQuicMinInitialDatagram);
}

TEST_CASE("QUIC paths: a NAT rebinding moves the server's datagrams at once, inside the amplification limit, keeps "
          "congestion state, challenges both paths and expands an unexpanded challenge",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    RawRun run(identity);
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath rebound = client_at(7U, 50001U);
    run.from = a;
    run.handshake();
    run.drain();
    const u64 smoothed = run.server.recovery().rtt().smoothed_ns;
    const usize mark = run.raw.datagrams;
    run.from = rebound;
    run.pad_to = 200U; // three times this is under 1200 bytes: the first challenge cannot be expanded
    send_frame(run, QuicFrameKind::Ping);
    CHECK(run.server.path() == rebound);
    CHECK(run.server.counters().migrations == 1U);
    CHECK_FALSE(run.server.address_validated());
    CHECK(run.server.recovery().path_floor() == 0U); // only the port changed: the state is kept (section 9.4)
    CHECK(run.server.recovery().rtt().smoothed_ns == smoothed);
    CHECK(bytes_to(run, rebound, mark) <= 3U * 200U);
    const usize first = latest_challenge(run, rebound);
    REQUIRE(first < 32U);
    CHECK(run.raw.challenge_datagram[first] < kQuicMinInitialDatagram);
    REQUIRE(latest_challenge(run, a) < 32U); // section 9.3.3: the path before is challenged too
    CHECK(run.raw.challenge_datagram[latest_challenge(run, a)] >= kQuicMinInitialDatagram);
    run.pad_to = 0U;
    answer(run, rebound);
    CHECK(run.server.address_validated());
    const usize expanded = latest_challenge(run, rebound);
    REQUIRE(expanded > first);
    CHECK(run.raw.challenge_datagram[expanded] >= kQuicMinInitialDatagram); // section 8.2.3
    const usize slot = run.server.paths().find(rebound);
    CHECK(run.server.paths().validating(slot));
    answer(run, rebound);
    CHECK_FALSE(run.server.paths().validating(slot));
    answer(run, a);
    CHECK(run.server.path() == rebound); // a probing packet from the old path moves nothing back
    CHECK(run.server.counters().path_validations == 3U);
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC paths: an address change keeps datagrams to 1200 bytes until validated, then restarts the RTT "
          "estimate, congestion control and the path MTU search from its family's base",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    RawRun run(identity);
    run.from = client_at(7U, 50000U);
    run.handshake();
    run.drain();
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    REQUIRE(run.server.recovery().rtt().sampled());
    const u64 next = run.server.next_packet_number(QuicSpace::Application);
    const usize mark = run.raw.datagrams;
    run.from = QuicPath{0U, Endpoint{IpAddress::v4(203, 0, 113, 5), 50000U}};
    run.pad_to = kQuicMinInitialDatagram;
    send_frame(run, QuicFrameKind::Ping);
    CHECK(run.server.path() == run.from);
    // Not yet confirmed: the credit and the floor hold the path, and the estimate stands (section 9.4).
    CHECK(run.server.recovery().path_floor() == 0U);
    CHECK(run.server.recovery().rtt().sampled());
    for (usize index = mark; index < run.raw.datagrams && index < 256U; ++index)
    {
        CHECK(run.raw.sent_size[index] <= kQuicMinInitialDatagram);
    }
    answer(run, run.from);
    REQUIRE(run.server.address_validated());
    CHECK(run.server.recovery().path_floor() >= next);
    CHECK_FALSE(run.server.recovery().rtt().sampled());
    CHECK(run.server.path_mtu().size() == kQuicPmtuFloor);
    CHECK(run.server.path_mtu().state() == QuicPmtuState::Searching); // an IPv4 path's base is the floor
    // What was in flight before counts no more on the new path.
    CHECK(run.server.recovery().bytes_in_flight() <= bytes_to(run, run.from, mark));
    // An IPv6 address probes its 1232 bytes first (RFC 8899 section 5.2).
    constexpr u8 v6[16] = {0x20U, 0x01U, 0x0DU, 0xB8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U};
    run.from = QuicPath{0U, Endpoint{IpAddress::v6(v6), 443U}};
    run.pad_to = kQuicMinInitialDatagram;
    send_frame(run, QuicFrameKind::Ping);
    CHECK(run.server.path() == run.from);
    answer(run, run.from);
    CHECK(run.server.path_mtu().size() == kQuicPmtuFloor);
    CHECK(run.server.path_mtu().state() == QuicPmtuState::Base);
    CHECK(run.server.counters().migrations == 2U);
}

TEST_CASE("QUIC paths: only the highest-numbered non-probing packet moves the path, and a return to a validated path "
          "needs no validation",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    RawRun run(identity);
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath b = client_at(8U, 50000U);
    run.from = a;
    run.handshake();
    run.pad_to = kQuicMinInitialDatagram;
    u64& next = run.raw.next[static_cast<usize>(QuicSpace::Application)];
    const u64 skipped = next;
    next += 4U; // a gap the late packet falls into
    run.from = b;
    send_frame(run, QuicFrameKind::Ping);
    REQUIRE(run.server.path() == b);
    const u64 after = next;
    next = skipped; // a packet sent earlier from the old path, arriving late
    run.from = a;
    send_frame(run, QuicFrameKind::Ping);
    CHECK(run.server.path() == b);
    CHECK(run.server.counters().migrations == 1U);
    next = after;
    send_frame(run, QuicFrameKind::Ping);
    CHECK(run.server.path() == a);
    CHECK(run.server.counters().migrations == 2U);
    CHECK(run.server.address_validated()); // validated in the handshake: no validation again (section 9.3)
}

TEST_CASE("QUIC paths: a validation that fails reverts to the last validated path, and with none left the "
          "connection ends silently",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath b = client_at(8U, 50000U);
    {
        RawRun run(identity);
        run.from = a;
        run.handshake();
        run.pad_to = kQuicMinInitialDatagram;
        run.from = b;
        const usize mark = run.raw.datagrams;
        send_frame(run, QuicFrameKind::Ping);
        REQUIRE(run.server.path() == b);
        // The client still holds the old path (an attacker's copy moved the server): it answers there, but never
        // sends on the new path again.
        run.from = a;
        run.pad_to = 0U;
        answer(run, a);
        CHECK(run.server.path() == b);
        run_timers(run, run.now + 30000U * kMs);
        CHECK(run.server.path() == a); // section 9.3.2
        CHECK(run.server.recovery().path_floor() == 0U); // b was never confirmed: a's state stands
        // b stays known, no longer validating, with the ID sent there (shared with a here).
        const usize left = run.server.paths().find(b);
        REQUIRE(left != kQuicNoPath);
        CHECK_FALSE(run.server.paths().validating(left));
        CHECK_FALSE(run.server.paths().validated(left));
        CHECK(run.server.counters().path_failures == 1U);
        CHECK(run.server.counters().migrations == 2U);
        // Nothing more arrived on b: what went there stayed inside three times its one datagram.
        CHECK(bytes_to(run, b, mark) <= 3U * kQuicMinInitialDatagram);
        CHECK(run.server.state() == QuicConnectionState::Established);
    }
    {
        RawRun run(identity);
        run.from = a;
        run.handshake();
        run.pad_to = kQuicMinInitialDatagram;
        run.from = b;
        send_frame(run, QuicFrameKind::Ping);
        REQUIRE(run.server.path() == b);
        run_timers(run, run.now + 30000U * kMs);
        CHECK(challenges_on(run, a) >= 2U); // the validated old path's challenge goes again until given up
        CHECK(run.server.state() == QuicConnectionState::Closed);
        CHECK(run.server.close_info().path_lost);
        CHECK_FALSE(run.raw.close_seen); // silently (section 9.3.2)
        CHECK(run.server.counters().path_failures == 2U);
    }
}

TEST_CASE("QUIC paths: an off-path attacker's forwarded copy moves the server for one packet and costs its "
          "congestion state nothing; the spurious path is abandoned",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    RawRun run(identity);
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath attacker = QuicPath{0U, Endpoint{IpAddress::v4(203, 0, 113, 66), 9999U}};
    run.from = a;
    run.handshake();
    run.drain();
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    REQUIRE(run.server.recovery().rtt().sampled());
    const u64 smoothed = run.server.recovery().rtt().smoothed_ns;
    const u64 window = run.server.congestion().window();
    // The attacker's copy arrives first from its address, and moves the server there.
    run.pad_to = kQuicMinInitialDatagram;
    run.from = attacker;
    send_frame(run, QuicFrameKind::Ping);
    REQUIRE(run.server.path() == attacker);
    CHECK_FALSE(run.server.address_validated());
    // The client's next packet from its own address, higher-numbered: back at once, the copy's path abandoned
    // (section 9.3.2), nothing restarted: a is validated and the state was never rebuilt.
    run.from = a;
    run.pad_to = 0U;
    send_frame(run, QuicFrameKind::Ping);
    CHECK(run.server.path() == a);
    CHECK(run.server.address_validated());
    CHECK(run.server.paths().find(attacker) == kQuicNoPath);
    CHECK(run.server.counters().migrations == 2U);
    CHECK(run.server.recovery().path_floor() == 0U);
    CHECK(run.server.recovery().rtt().smoothed_ns == smoothed);
    CHECK(run.server.congestion().window() == window);
    // What went toward the attacker stayed inside three times its one datagram.
    CHECK(bytes_to(run, attacker, 0U) <= 3U * kQuicMinInitialDatagram);
    run_timers(run, run.now + 10000U * kMs);
    CHECK(run.server.state() == QuicConnectionState::Established);
    CHECK(run.server.path() == a);
}

TEST_CASE("QUIC paths: a client that changed its connection ID is answered on an unused one of its own, and an ID "
          "left on a forgotten path is retired",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    Host host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host);
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath b = client_at(8U, 50000U);
    run.from = a;
    run.handshake();
    run.drain();
    REQUIRE(run.raw.issued_bytes[1] == sizeof(kServerCid));
    // The client gives the server a second ID of its own first (section 9.5's SHOULD).
    constexpr u8 second[sizeof(kClientCid)] = {0xC8U, 0xC9U, 0xCAU, 0xCBU, 0xCCU, 0xCDU, 0xCEU, 0xCFU};
    constexpr u8 token[kQuicResetTokenBytes] = {0x7AU};
    QuicFrame issue;
    issue.kind = QuicFrameKind::NewConnectionId;
    issue.sequence = 1U;
    issue.data = ConstSpan<u8>(second);
    issue.reset_token = ConstSpan<u8>(token);
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(issue, out); });
    REQUIRE(run.server.peer_cid_count() == 2U);
    // It moves to b with the server's sequence 1.
    std::memcpy(run.raw.dcid, run.raw.issued[1], run.raw.issued_bytes[1]);
    run.from = b;
    run.pad_to = kQuicMinInitialDatagram;
    send_frame(run, QuicFrameKind::Ping);
    REQUIRE(run.server.path() == b);
    CHECK(run.server.peer_cid()[0] == 0xC8U);
    const usize on_b = latest_challenge(run, b);
    const usize on_a = latest_challenge(run, a);
    REQUIRE(on_b < 32U);
    REQUIRE(on_a < 32U);
    CHECK(run.raw.challenge_dcid[on_b] == 0xC8U); // an ID never used on another path (section 9.5)
    CHECK(run.raw.challenge_dcid[on_a] == kClientCid[0]);
    answer(run, b);
    // The old path never answers: it is forgotten, and its ID retired so that nothing links the two.
    run_timers(run, run.now + 30000U * kMs);
    CHECK(run.server.paths().find(a) == kQuicNoPath);
    CHECK(run.server.state() == QuicConnectionState::Established);
    CHECK(run.raw.retire_seen);
    CHECK(run.raw.retire_sequence == 0U);
    CHECK(run.server.peer_cid_count() == 1U);
    CHECK(run.server.peer_cid()[0] == 0xC8U);
}

// A raw client of `run` gives the server its second ID (sequence 1, first byte 0xC8): section 9.5's SHOULD.
void issue_second_id(RawRun& run)
{
    static constexpr u8 kSecond[sizeof(kClientCid)] = {0xC8U, 0xC9U, 0xCAU, 0xCBU, 0xCCU, 0xCDU, 0xCEU, 0xCFU};
    static constexpr u8 kToken[kQuicResetTokenBytes] = {0x7AU};
    QuicFrame issue;
    issue.kind = QuicFrameKind::NewConnectionId;
    issue.sequence = 1U;
    issue.data = ConstSpan<u8>(kSecond);
    issue.reset_token = ConstSpan<u8>(kToken);
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(issue, out); });
}

// Whether every server datagram to `path` since datagram `from` of the raw client's log came to an ID starting `id`.
bool only_id_to(const RawRun& run, const QuicPath& path, usize from, u8 id)
{
    bool only = true;
    for (usize index = from; index < run.raw.datagrams && index < 256U; ++index)
    {
        only = only && (!(run.raw.sent_to[index] == path) || run.raw.sent_dcid[index] == id);
    }
    return only;
}

TEST_CASE("QUIC paths: a server keeps the address it reverted from with the ID it sent there, so the client's next "
          "packet from there moves it back on that packet, to that ID alone",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    Host host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host);
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath b = client_at(8U, 50000U);
    run.from = a;
    run.handshake();
    run.drain();
    REQUIRE(run.raw.issued_bytes[1] == sizeof(kServerCid));
    issue_second_id(run);
    REQUIRE(run.server.peer_cid_count() == 2U);
    // The client moves to b with the server's sequence 1; b's challenges are lost, and it answers a's check.
    std::memcpy(run.raw.dcid, run.raw.issued[1], run.raw.issued_bytes[1]);
    run.from = b;
    run.pad_to = kQuicMinInitialDatagram;
    const usize start = run.raw.datagrams;
    send_frame(run, QuicFrameKind::Ping);
    REQUIRE(run.server.path() == b);
    run.from = a;
    run.pad_to = 0U;
    answer(run, a);
    run_timers(run, run.now + 30000U * kMs);
    REQUIRE(run.server.path() == a); // section 9.3.2
    REQUIRE(run.server.counters().path_failures == 1U);
    // b is kept, its validation over, and the ID sent there is neither retired nor sent to anywhere else.
    const usize left = run.server.paths().find(b);
    REQUIRE(left != kQuicNoPath);
    CHECK_FALSE(run.server.paths().validating(left));
    CHECK_FALSE(run.raw.retire_seen);
    CHECK(run.server.peer_cid_count() == 2U);
    CHECK(only_id_to(run, a, start, kClientCid[0]));
    // The client, at b all along, sends again from there with the ID it used: the server moves on that packet.
    const usize back = run.raw.datagrams;
    const usize challenged = challenges_on(run, b);
    run.from = b;
    run.pad_to = kQuicMinInitialDatagram;
    send_frame(run, QuicFrameKind::Ping);
    CHECK(run.server.path() == b);
    CHECK(run.server.counters().migrations == 3U);
    CHECK(run.server.peer_cid()[0] == 0xC8U);
    CHECK(run.server.paths().validating(left));
    REQUIRE(challenges_on(run, b) > challenged);
    CHECK(run.raw.challenge_dcid[latest_challenge(run, b)] == 0xC8U);
    CHECK(bytes_to(run, b, back) != 0U);
    CHECK(only_id_to(run, b, start, 0xC8U)); // never a's ID (section 9.5)
    run.pad_to = 0U;
    answer(run, b);
    CHECK(run.server.address_validated());
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC paths: a client's move with no ID of its own unused waits, validating, for the next one and happens "
          "when it arrives; a later packet from elsewhere cancels it, as the address's failed validation does",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath b = client_at(8U, 50000U);
    const QuicPath c = client_at(9U, 50000U);
    enum class Then : u8
    {
        Moves,
        Cancelled, // the client sends from a again
        Expired,   // b's validation fails; a probe from c takes its slot
    };
    for (const Then then : {Then::Moves, Then::Cancelled, Then::Expired})
    {
        CAPTURE(static_cast<u32>(then));
        CountingAllocator allocator;
        Host host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
        RawRun run(identity, 2048U, {}, &host);
        run.from = a;
        run.handshake();
        run.drain();
        REQUIRE(run.raw.issued_bytes[1] == sizeof(kServerCid));
        u8 first[sizeof(run.raw.dcid)] = {};
        std::memcpy(first, run.raw.dcid, sizeof(first));
        // The client moves to b with the server's sequence 1 before it gave a second ID of its own.
        std::memcpy(run.raw.dcid, run.raw.issued[1], run.raw.issued_bytes[1]);
        run.from = b;
        run.pad_to = kQuicMinInitialDatagram;
        const usize start = run.raw.datagrams;
        send_frame(run, QuicFrameKind::Ping);
        CHECK(run.server.path() == a);
        const usize waiting = run.server.paths().find(b);
        REQUIRE(waiting != kQuicNoPath);
        CHECK(run.server.paths().validating(waiting)); // its challenge waits for the ID
        CHECK(bytes_to(run, b, start) == 0U);          // a's ID never goes there (section 9.5)
        CHECK(run.server.counters().migrations == 0U);
        QuicPath probed = b;
        if (then == Then::Cancelled)
        {
            // Back at a with the ID it used there: the client's latest choice, so the ID arriving moves nothing.
            std::memcpy(run.raw.dcid, first, sizeof(first));
            run.from = a;
            run.pad_to = 0U;
            send_frame(run, QuicFrameKind::Ping);
        }
        if (then == Then::Expired)
        {
            run_timers(run, run.now + 10000U * kMs);
            REQUIRE(run.server.counters().path_failures == 1U);
            REQUIRE(run.server.paths().find(b) == kQuicNoPath);
            constexpr u8 data[kQuicPathDataBytes] = {0x5AU};
            run.from = c;
            send_frame(run, QuicFrameKind::PathChallenge, ConstSpan<u8>(data));
            REQUIRE(run.server.paths().find(c) == waiting); // the slot b held: never the move b waited for
            probed = c;
            run.from = a;
        }
        // The ID comes in a packet of probing frames alone (section 9.1).
        issue_second_id(run);
        REQUIRE(run.server.peer_cid_count() == 2U);
        const bool moved = then == Then::Moves;
        CHECK(run.server.path() == (moved ? b : a));
        CHECK(run.server.counters().migrations == (moved ? 1U : 0U));
        REQUIRE(latest_challenge(run, probed) < 32U); // the validation goes on either way
        CHECK(run.raw.challenge_dcid[latest_challenge(run, probed)] == 0xC8U);
        CHECK(only_id_to(run, probed, start, 0xC8U));
        CHECK(only_id_to(run, a, start, kClientCid[0]));
        run.pad_to = 0U;
        run.from = probed;
        answer(run, probed);
        CHECK(run.server.paths().validated(run.server.paths().find(probed)));
        CHECK(run.server.path() == (moved ? b : a)); // a response is probing: it moves nothing
        CHECK(run.server.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC paths: a move waiting for an ID keeps its path while probes from new addresses overflow the table, "
          "and happens when the ID arrives",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    Host host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host);
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath b = client_at(8U, 50000U);
    const QuicPath last = client_at(11U, 50000U);
    run.from = a;
    run.handshake();
    run.drain();
    // The client moves to b with the server's sequence 1 before it gave a second ID of its own: the move waits.
    std::memcpy(run.raw.dcid, run.raw.issued[1], run.raw.issued_bytes[1]);
    run.from = b;
    run.pad_to = kQuicMinInitialDatagram;
    send_frame(run, QuicFrameKind::Ping);
    const usize waiting = run.server.paths().find(b);
    REQUIRE(waiting != kQuicNoPath);
    // Probes from three more addresses at once: two take the free slots and the last gives one up. b is the least
    // recently used (the lowest slot of a tie), yet stays.
    constexpr u8 data[kQuicPathDataBytes] = {0x5AU};
    for (const u8 address : {u8{9U}, u8{10U}, u8{11U}})
    {
        run.from = client_at(address, 50000U);
        send_frame(run, QuicFrameKind::PathChallenge, ConstSpan<u8>(data));
    }
    CHECK(run.server.paths().find(last) != kQuicNoPath);
    CHECK(run.server.paths().find(b) == waiting);
    CHECK(run.server.paths().validating(waiting));
    CHECK(run.server.counters().path_failures == 0U);
    CHECK(run.server.path() == a);
    run.from = a;
    issue_second_id(run);
    CHECK(run.server.path() == b);
    CHECK(run.server.counters().migrations == 1U);
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC paths: a path whose check ends in its own packet gives its ID to the path that waits only once that "
          "packet chose its path: a probe gives it up, a non-probing packet moves there and keeps it",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath r = client_at(8U, 50000U);
    const QuicPath w = client_at(9U, 50000U);
    const QuicPath z = client_at(10U, 50000U);
    static constexpr u8 kThird[sizeof(kClientCid)] = {0xE0U, 0xE1U, 0xE2U, 0xE3U, 0xE4U, 0xE5U, 0xE6U, 0xE7U};
    static constexpr u8 kToken[kQuicResetTokenBytes] = {0x7BU};
    constexpr u8 data[kQuicPathDataBytes] = {0x5AU};
    for (const bool moves : {false, true})
    {
        CAPTURE(moves);
        CountingAllocator allocator;
        Host host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
        RawRun run(identity, 2048U, {}, &host);
        run.from = a;
        run.handshake();
        run.drain();
        REQUIRE(run.raw.issued_bytes[1] == sizeof(kServerCid));
        issue_second_id(run);
        REQUIRE(run.server.peer_cid_count() == 2U);
        u8 first[sizeof(run.raw.dcid)] = {};
        std::memcpy(first, run.raw.dcid, sizeof(first));
        // r probes with the server's sequence 1 and takes the client's; w probes with it too and finds none unused.
        std::memcpy(run.raw.dcid, run.raw.issued[1], run.raw.issued_bytes[1]);
        run.pad_to = kQuicMinInitialDatagram;
        run.from = r;
        send_frame(run, QuicFrameKind::PathChallenge, ConstSpan<u8>(data));
        const usize held = run.server.paths().find(r);
        REQUIRE(held != kQuicNoPath);
        REQUIRE(run.server.paths().validating(held));
        run.from = w;
        send_frame(run, QuicFrameKind::PathChallenge, ConstSpan<u8>(data));
        REQUIRE(run.server.paths().find(w) != kQuicNoPath);
        REQUIRE(challenges_on(run, w) == 0U); // its challenge waits for an ID (section 9.5)
        // From r, the answer to r's check; moving, a challenge and a PING with it.
        const usize challenge = latest_challenge(run, r);
        REQUIRE(challenge < 32U);
        QuicFrame response;
        response.kind = QuicFrameKind::PathResponse;
        response.data = ConstSpan<u8>(run.raw.challenge[challenge], kQuicPathDataBytes);
        QuicFrame probe;
        probe.kind = QuicFrameKind::PathChallenge;
        probe.data = ConstSpan<u8>(data);
        QuicFrame ping;
        ping.kind = QuicFrameKind::Ping;
        run.from = r;
        run.raw.path_response_seen = false;
        const usize sent = run.raw.datagrams;
        run.send(QuicPacketType::OneRtt,
                 [&](Span<u8> out)
                 {
                     usize at = write_quic_frame(response, out);
                     if (moves)
                     {
                         at += write_quic_frame(probe, out.subspan(at));
                         at += write_quic_frame(ping, out.subspan(at));
                     }
                     return at;
                 });
        CHECK(run.server.path() == (moves ? r : a));
        CHECK(run.server.counters().migrations == (moves ? 1U : 0U));
        CHECK((run.server.paths().find(r) == kQuicNoPath) == !moves);
        CHECK(run.raw.retire_seen == !moves); // the ID r gave up, for the peer's replacement
        CHECK(run.raw.path_response_seen == moves);
        CHECK((bytes_to(run, r, sent) != 0U) == moves); // answered on r, where it came (section 8.2.2)
        CHECK(challenges_on(run, w) == 0U);
        // z probes with the ID a uses: a probe, which moves nothing (section 9.1), in r's place when r is forgotten.
        std::memcpy(run.raw.dcid, first, sizeof(first));
        run.from = z;
        send_frame(run, QuicFrameKind::PathChallenge, ConstSpan<u8>(data));
        const usize probed = run.server.paths().find(z);
        REQUIRE(probed != kQuicNoPath);
        CHECK((probed == held) == !moves);
        CHECK(run.server.path() == (moves ? r : a));
        CHECK(run.server.counters().migrations == (moves ? 1U : 0U));
        CHECK(run.server.paths().validating(probed));
        CHECK(challenges_on(run, z) == (moves ? 0U : 1U)); // on r, z's local ID is not r's: it waits for one too
        CHECK(run.server.state() == QuicConnectionState::Established);
        if (moves)
        {
            continue; // the server holds the client's two IDs, both in use
        }
        // The client's next ID goes to w, whose challenge leaves on it; the server stays on a.
        QuicFrame issue;
        issue.kind = QuicFrameKind::NewConnectionId;
        issue.sequence = 2U;
        issue.data = ConstSpan<u8>(kThird);
        issue.reset_token = ConstSpan<u8>(kToken);
        std::memcpy(run.raw.dcid, first, sizeof(first));
        run.from = a;
        run.pad_to = 0U;
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(issue, out); });
        REQUIRE(latest_challenge(run, w) < 32U);
        CHECK(run.raw.challenge_dcid[latest_challenge(run, w)] == 0xE0U);
        CHECK(run.server.path() == a);
        CHECK(run.server.counters().migrations == 0U);
        CHECK(run.server.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC paths: before confirmation a server takes nothing from another path",
          "[net][net-path][quic-connection]")
{
    Identity identity;
    RawRun run(identity);
    const QuicPath a = client_at(7U, 50000U);
    run.from = a;
    run.handshake(
        [&]
        {
            // Mid-handshake: a datagram from another path is counted and dropped before it is opened.
            run.from = client_at(9U, 50000U);
            run.send(QuicPacketType::Handshake,
                     [](Span<u8> out)
                     {
                         QuicFrame ping;
                         ping.kind = QuicFrameKind::Ping;
                         return write_quic_frame(ping, out);
                     });
            CHECK(run.server.counters().foreign_datagrams == 1U);
            run.from = a;
        });
    CHECK(run.server.path() == a);
    CHECK(run.server.counters().migrations == 0U);
}

// ---- Address discovery (draft-ietf-quic-address-discovery-01, design S77) ----

namespace
{
const Endpoint kServerAt{IpAddress::v4(203, 0, 113, 80), 443U};

void send_observed(RawRun& run, u64 sequence, const Endpoint& observed)
{
    QuicFrame frame;
    frame.kind = QuicFrameKind::ObservedAddress;
    frame.sequence = sequence;
    frame.data = observed.address.view();
    frame.value = observed.port;
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(frame, out); });
}

// Has the server send ack-eliciting packets, acknowledging what the raw client opened, until it sent `count`
// OBSERVED_ADDRESS frames in all (its first may come before the raw client holds 1-RTT keys, and then goes again).
void collect_observations(RawRun& run, usize count)
{
    for (usize round = 0U; round < 16U && run.raw.observations < count; ++round)
    {
        run.server.ping();
        run.drain();
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    }
    REQUIRE(run.raw.observations == count);
}

usize observations_on(const RawRun& run, const QuicPath& path)
{
    usize count = 0U;
    for (usize index = 0U; index < run.raw.observations && index < 32U; ++index)
    {
        count += (run.raw.observed_path[index] == path) ? 1U : 0U;
    }
    return count;
}

QuicConnectionConfig discovery_config(QuicSide side, const Identity& identity, QuicAddressDiscovery mode, usize seed)
{
    QuicConnectionConfig config = config_for(side, identity, QuicVersion::V1, seed);
    config.parameters.address_discovery = mode;
    if (side == QuicSide::Client)
    {
        config.path = QuicPath{0U, kServerAt};
    }
    return config;
}

constexpr QuicAddressDiscovery kModes[] = {QuicAddressDiscovery::Off, QuicAddressDiscovery::Provide,
                                           QuicAddressDiscovery::Receive, QuicAddressDiscovery::Both};
} // namespace

TEST_CASE("QUIC address discovery: OBSERVED_ADDRESS is taken only where asked for, in the application space, the "
          "newest sequence on each path winning",
          "[net][net-path][quic-connection][address-discovery]")
{
    const Endpoint first{IpAddress::v4(192, 0, 2, 40), 6000U};
    const Endpoint second{IpAddress::v4(192, 0, 2, 41), 6001U};
    constexpr u8 v6[16] = {0x20U, 0x01U, 0x0DU, 0xB8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 9U};
    const Endpoint third{IpAddress::v6(v6), 7000U};
    // Not asked for, by sending no parameter or by offering reports alone: PROTOCOL_VIOLATION (section 4.1).
    CHECK(refused(
              [&](QuicFrame& f)
              {
                  f.kind = QuicFrameKind::ObservedAddress;
                  f.data = first.address.view();
                  f.value = first.port;
              }) == code(QuicTransportError::ProtocolViolation));
    Identity identity;
    {
        RawRun run(identity, 2048U, {}, nullptr, {}, 0U, QuicAddressDiscovery::Provide);
        run.from = client_at(7U, 50000U);
        run.handshake();
        send_observed(run, 0U, first);
        REQUIRE(run.server.state() == QuicConnectionState::Closing);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
    // Outside the application space: a frame out of place.
    {
        RawRun run(identity, 2048U, {}, nullptr, {}, 0U, QuicAddressDiscovery::Both);
        run.handshake(
            [&]
            {
                QuicFrame frame;
                frame.kind = QuicFrameKind::ObservedAddress;
                frame.data = first.address.view();
                frame.value = first.port;
                run.send(QuicPacketType::Handshake, [&](Span<u8> out) { return write_quic_frame(frame, out); });
            });
        REQUIRE(run.server.state() == QuicConnectionState::Closing);
        CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
    }
    // Asked for: each path keeps its newest report; an older or equal sequence there changes nothing.
    RawRun run(identity, 2048U, {}, nullptr, {}, 0U, QuicAddressDiscovery::Receive);
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath b = client_at(7U, 50001U);
    run.from = a;
    run.handshake();
    run.drain();
    CHECK(run.server.observed_address() == Endpoint{});
    send_observed(run, 5U, first);
    CHECK(run.server.observed_address() == first);
    send_observed(run, 3U, second);
    send_observed(run, 5U, second);
    CHECK(run.server.observed_address() == first);
    CHECK(run.server.counters().address_observations == 1U);
    send_observed(run, 9U, third);
    CHECK(run.server.observed_address() == third);
    // Another path's first report counts there, whatever its sequence; a packet of it alone probes, moving nothing.
    run.from = b;
    send_observed(run, 4U, second);
    CHECK(run.server.path() == a);
    CHECK(run.server.counters().migrations == 0U);
    CHECK(run.server.paths().observed(run.server.paths().find(b)) == second);
    CHECK(run.server.observed_address() == third);
    CHECK(run.server.counters().address_observations == 3U);
    CHECK(run.raw.observations == 0U); // a server that does not offer reports sends none
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC address discovery: a server sends each path its address once, again on that path alone when lost, "
          "never for a replayed packet from a spoofed address",
          "[net][net-path][quic-connection][address-discovery]")
{
    Identity identity;
    RawRun run(identity, 2048U, {}, nullptr, {}, 0U, QuicAddressDiscovery::Provide);
    run.discovery = QuicAddressDiscovery::Receive;
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath rebound = client_at(7U, 50001U);
    run.from = a;
    run.handshake();
    collect_observations(run, 1U);
    CHECK(run.raw.observed[0] == a.peer);
    CHECK(run.raw.observed_path[0] == a);
    for (usize round = 0U; round < 4U; ++round)
    {
        run.server.ping();
        run.drain();
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    }
    CHECK(run.raw.observations == 1U); // acknowledged: never again on that path

    // A NAT rebinding: the new path's report rides with its challenge, at a higher sequence.
    run.from = rebound;
    send_frame(run, QuicFrameKind::Ping);
    REQUIRE(run.server.path() == rebound);
    REQUIRE(run.raw.observations == 2U);
    CHECK(run.raw.observed[1] == rebound.peer);
    CHECK(run.raw.observed_path[1] == rebound);
    CHECK(run.raw.observed_sequence[1] > run.raw.observed_sequence[0]);
    // Lost: the raw client never acknowledges its packet, so later ones acknowledged declare it lost.
    run.raw.seen[run.raw.observed_packet[1]] = false;
    answer(run, rebound);
    REQUIRE(run.server.address_validated());
    for (usize round = 0U; round < 6U && run.raw.observations < 3U; ++round)
    {
        run.server.ping();
        run.drain();
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    }
    REQUIRE(run.raw.observations == 3U);
    CHECK(run.raw.observed[2] == rebound.peer);
    CHECK(run.raw.observed_path[2] == rebound);
    CHECK(run.raw.observed_sequence[2] > run.raw.observed_sequence[1]);
    CHECK(observations_on(run, a) == 1U); // the path left behind was challenged, but owed no report

    // A replay of the raw client's last datagram from a spoofed address is a duplicate: no path, nothing sent.
    const QuicPath spoofed = client_at(66U, 1234U);
    u8 replay[kDatagram];
    std::memcpy(replay, run.last, run.last_bytes);
    const usize mark = run.raw.datagrams;
    run.server.receive(spoofed, Span<u8>(replay, run.last_bytes), Ecn::NotEct, run.now);
    run.drain();
    CHECK(run.server.paths().find(spoofed) == kQuicNoPath);
    CHECK(bytes_to(run, spoofed, mark) == 0U);
    CHECK(run.raw.observations == 3U);
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC address discovery: off the active path a report only rides a path frame, and a lost one waits there "
          "for the next one",
          "[net][net-path][quic-connection][address-discovery]")
{
    Identity identity;
    RawRun run(identity, 2048U, {}, nullptr, {}, 0U, QuicAddressDiscovery::Provide);
    run.discovery = QuicAddressDiscovery::Receive;
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath b = client_at(8U, 50000U);
    run.from = a;
    run.handshake();
    collect_observations(run, 1U);
    // A probe from another path: the answer there carries that path's report, and nothing moves.
    // Padded, so that the amplification limit leaves b room for more than the answer.
    constexpr u8 probe[kQuicPathDataBytes] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    run.from = b;
    run.pad_to = kQuicMinInitialDatagram;
    send_frame(run, QuicFrameKind::PathChallenge, ConstSpan<u8>(probe));
    run.pad_to = 0U;
    REQUIRE(run.server.paths().credit(run.server.paths().find(b)) >= kQuicMinInitialDatagram);
    REQUIRE(run.raw.path_response_seen);
    REQUIRE(run.raw.observations == 2U);
    CHECK(run.raw.observed[1] == b.peer);
    CHECK(run.raw.observed_path[1] == b);
    CHECK(run.server.path() == a);
    CHECK(run.server.counters().migrations == 0U);
    // Lost there (three later packets acknowledged): owed again on that path, it draws no datagram of its own.
    run.raw.seen[run.raw.observed_packet[1]] = false;
    const usize mark = run.raw.datagrams;
    const usize challenged = challenges_on(run, b);
    run.from = a;
    for (usize round = 0U; round < 6U; ++round)
    {
        run.server.ping();
        run.drain();
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    }
    CHECK(bytes_to(run, b, mark) == 0U);
    CHECK(run.raw.observations == 2U);
    // The server's next challenge there carries it, at a higher sequence; every datagram there is a challenge's.
    run_timers(run, run.now + 2000U * kMs);
    REQUIRE(run.raw.observations == 3U);
    CHECK(run.raw.observed_path[2] == b);
    CHECK(run.raw.observed[2] == b.peer);
    CHECK(run.raw.observed_sequence[2] > run.raw.observed_sequence[1]);
    usize to_b = 0U;
    for (usize index = mark; index < run.raw.datagrams && index < 256U; ++index)
    {
        to_b += (run.raw.sent_to[index] == b) ? 1U : 0U;
    }
    CHECK(to_b != 0U);
    CHECK(to_b == challenges_on(run, b) - challenged);
    CHECK(run.server.path() == a);
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC address discovery: an IPv4-mapped peer is reported as IPv4, an IPv6 one without its scope",
          "[net][net-path][quic-connection][address-discovery]")
{
    Identity identity;
    constexpr u8 link_local[16] = {0xFEU, 0x80U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0x21U};
    const QuicPath mapped{0U, Endpoint{IpAddress::v4(198, 51, 100, 7).mapped(), 50000U}};
    const QuicPath scoped{0U, Endpoint{IpAddress::v6(link_local, 3U), 50002U}};
    const Endpoint plain_v4{IpAddress::v4(198, 51, 100, 7), 50000U};
    const Endpoint plain_v6{IpAddress::v6(link_local), 50002U};
    for (const bool v6 : {false, true})
    {
        RawRun run(identity, 2048U, {}, nullptr, {}, 0U, QuicAddressDiscovery::Both);
        run.discovery = QuicAddressDiscovery::Both;
        run.from = v6 ? scoped : mapped;
        run.handshake();
        collect_observations(run, 1U);
        CHECK(run.raw.observed[0] == (v6 ? plain_v6 : plain_v4));
    }
}

TEST_CASE("QUIC address discovery: every pair of modes reports exactly where one end offers and the other asks, over "
          "paths that lose, duplicate and reorder",
          "[net][net-path][quic-connection][address-discovery]")
{
    Identity identity;
    for (const QuicAddressDiscovery client_mode : kModes)
    {
        for (const QuicAddressDiscovery server_mode : kModes)
        {
            const u64 seed = (static_cast<u64>(client_mode) * 4U) + static_cast<u64>(server_mode) + 1U;
            INFO("client " << static_cast<u32>(client_mode) << " server " << static_cast<u32>(server_mode));
            CountingAllocator allocator;
            QuicConnection client(allocator, discovery_config(QuicSide::Client, identity, client_mode, 0U));
            QuicConnection server(allocator, discovery_config(QuicSide::Server, identity, server_mode, 1U));
            connect(client);
            auto net = std::make_unique<Net>(client, server);
            net->paths = true;
            net->rng.state = seed;
            for (Path& path : net->path)
            {
                path.loss = 150U;
                path.duplicate = 50U;
                path.jitter_ns = 8U * kMs;
            }
            const bool down = quic_provides_addresses(server_mode) && quic_wants_addresses(client_mode);
            const bool up = quic_provides_addresses(client_mode) && quic_wants_addresses(server_mode);
            REQUIRE(net->run_until(
                [&]
                {
                    return both_confirmed(client, server) && (!down || client.observed_address().port != 0U) &&
                           (!up || server.observed_address().port != 0U);
                },
                300000U * kMs));
            net->advance(net->now + 3000U * kMs);
            CHECK(client.observed_address() == (down ? net->client_at : Endpoint{}));
            CHECK(server.observed_address() == (up ? kServerAt : Endpoint{}));
            CHECK((client.counters().address_observations != 0U) == down);
            CHECK((server.counters().address_observations != 0U) == up);
            CHECK(client.state() == QuicConnectionState::Established);
            CHECK(server.state() == QuicConnectionState::Established);
        }
    }
}

TEST_CASE("QUIC address discovery: a client learns of its NAT rebinding and of an address change from its server's "
          "reports over a hostile path",
          "[net][net-path][quic-connection][address-discovery]")
{
    for (u64 seed = 1U; seed <= 3U; ++seed)
    {
        INFO("seed " << seed);
        Identity identity;
        CountingAllocator allocator;
        QuicConnection client(allocator,
                              discovery_config(QuicSide::Client, identity, QuicAddressDiscovery::Receive, 0U));
        QuicConnection server(allocator,
                              discovery_config(QuicSide::Server, identity, QuicAddressDiscovery::Provide, 1U));
        connect(client);
        auto net = std::make_unique<Net>(client, server);
        net->paths = true;
        net->rng.state = seed;
        for (Path& path : net->path)
        {
            path.loss = 100U;
            path.duplicate = 50U;
            path.jitter_ns = 10U * kMs;
        }
        REQUIRE(net->run_until(
            [&] { return both_confirmed(client, server) && client.observed_address() == net->client_at; },
            300000U * kMs));
        const Endpoint moves[] = {Endpoint{net->client_at.address, 50001U},
                                  Endpoint{IpAddress::v4(203, 0, 113, 5), 40000U}};
        for (const Endpoint& move : moves)
        {
            net->client_at = move;
            client.ping(); // the server sees the new address in its next datagram, resent until acknowledged
            REQUIRE(net->run_until([&] { return client.observed_address() == move; }, 300000U * kMs));
        }
        CHECK(client.path().peer == kServerAt); // the client's own path never changed
        CHECK(client.counters().address_observations >= 3U);
        CHECK(client.state() == QuicConnectionState::Established);
        CHECK(server.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC connection: multipath opens an ID space per path ID up to its limit and issues up to the peer's",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 3.2.1: IDs for each unused path ID up to the smaller limit; none past the peer's.
    Identity identity;
    CountingAllocator allocator;
    Host client_host(allocator, 0U, ConstSpan<u8>(kClientCid, sizeof(kClientCid)));
    Host server_host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    QuicConnectionConfig client_config =
        hosted(config_for(QuicSide::Client, identity, QuicVersion::V1), client_host, 4U);
    QuicConnectionConfig server_config =
        hosted(config_for(QuicSide::Server, identity, QuicVersion::V1, 1U), server_host, 2U);
    client_config.parameters.has_initial_max_path_id = true;
    client_config.parameters.initial_max_path_id = 3U;
    server_config.parameters.has_initial_max_path_id = true;
    server_config.parameters.initial_max_path_id = 2U;
    QuicConnection client(allocator, client_config);
    QuicConnection server(allocator, server_config);
    REQUIRE(client.ready());
    REQUIRE(server.ready());
    connect(client);
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until(
        [&]
        {
            return both_confirmed(client, server) && client.peer_cid_count(2U) == 4U &&
                   server.peer_cid_count(2U) == 2U && net->in_flight() == 0U;
        }));
    net->advance(net->now + 200U * kMs);
    CHECK(client.peer_max_path_id() == 2U);
    CHECK(server.peer_max_path_id() == 3U);
    for (u64 path_id = 0U; path_id <= 2U; ++path_id)
    {
        INFO("path ID " << path_id);
        CHECK(client.peer_cid_count(path_id) == 4U);
        CHECK(server.local_cid_count(path_id) == 4U);
        CHECK(server.peer_cid_count(path_id) == 2U);
        CHECK(client.local_cid_count(path_id) == 2U);
    }
    // Path ID 3 is under the client's limit alone: its space is open there and holds none of either side's IDs.
    CHECK(client.peer_cid_count(3U) == 0U);
    CHECK(client.local_cid_count(3U) == 0U);
    CHECK(server.peer_cid_count(3U) == 0U);
    CHECK(server_host.issued == 11U); // path ID 0's sequences 1 to 3, then four for each of path IDs 1 and 2
    CHECK(client_host.issued == 5U);
    const u64 received = server.counters().datagrams_received;
    client.ping();
    REQUIRE(net->run_until([&] { return net->in_flight() == 0U && server.counters().datagrams_received > received; }));
    CHECK(client.state() == QuicConnectionState::Established);
    CHECK(server.state() == QuicConnectionState::Established);
    CHECK(server.counters().paths_opened == 0U);
}

TEST_CASE("QUIC connection: a server opens a path ID at its first authenticated packet, answers there and validates it",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 3.1: the client's PATH_CHALLENGE is packet 1 of path ID 1's own space (a forged packet 0
    // opened nothing); the server answers in that space, on that 4-tuple, to that path ID's ID, and validates the
    // client's address before data goes there.
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    run.handshake();
    REQUIRE(run.server.multipath());
    for (u64 path_id = 1U; path_id <= 3U; ++path_id)
    {
        CHECK(run.raw.further_ids[path_id] == 2U); // the raw client's default active_connection_id_limit
        CHECK(run.server.local_cid_count(path_id) == 2U);
    }
    QuicFrame issued;
    issued.kind = QuicFrameKind::PathNewConnectionId;
    issued.path_id = 1U;
    issued.data = ConstSpan<u8>(kClientPathCid, sizeof(kClientPathCid));
    const u8 token[kQuicResetTokenBytes] = {0x7A, 0x7B};
    issued.reset_token = ConstSpan<u8>(token, sizeof(token));
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(issued, out); });
    REQUIRE(run.server.peer_cid_count(1U) == 1U);
    REQUIRE(run.raw.protection.open_path(1U, 1U));
    run.raw.path_slot = 1U;
    const QuicPath second = client_at(9U, 40000U);
    const ConstSpan<u8> server_id(run.raw.further_issued[1], run.raw.further_bytes[1]);
    const u8 data[kQuicPathDataBytes] = {1, 2, 3, 4, 5, 6, 7, 8};
    const auto challenge = [&](Span<u8> out)
    {
        QuicFrame frame;
        frame.kind = QuicFrameKind::PathChallenge;
        frame.data = ConstSpan<u8>(data, sizeof(data));
        return write_quic_frame(frame, out);
    };
    const usize before = run.raw.datagrams;
    run.send_on_path(server_id, second, challenge, true);
    CHECK(run.server.counters().paths_opened == 0U);
    CHECK(run.raw.datagrams == before); // nothing answers a packet that did not authenticate
    CHECK_FALSE(run.raw.path_response_seen);
    run.send_on_path(server_id, second, challenge);
    CHECK(run.server.counters().paths_opened == 1U);
    REQUIRE(run.raw.path_response_seen);
    CHECK(same(ConstSpan<u8>(run.raw.path_response, kQuicPathDataBytes), ConstSpan<u8>(data, sizeof(data))));
    CHECK(run.raw.path_acks != 0U);
    CHECK(run.raw.path_ack_id == 1U);
    CHECK(run.raw.path_ack_largest == 1U);
    REQUIRE(run.raw.challenges != 0U);
    const usize last = (run.raw.challenges - 1U) % 32U;
    CHECK(run.raw.challenge_path[last] == second);
    CHECK(run.raw.challenge_dcid[last] == kClientPathCid[0]);
    // What went to the new 4-tuple is held to three times what came from it, the forged datagram not counted.
    usize to_second = 0U;
    for (usize index = before; index < run.raw.datagrams && index < 256U; ++index)
    {
        CHECK(run.raw.sent_to[index] == second);
        to_second += run.raw.sent_size[index];
    }
    CHECK(to_second <= 3U * kQuicMinInitialDatagram);
    const QuicPaths& paths = run.server.paths(1U);
    CHECK_FALSE(paths.validated(paths.active()));
    // The client's answer, in path ID 1's space, validates it; path ID 0 is untouched.
    u8 answer[kQuicPathDataBytes] = {};
    std::memcpy(answer, run.raw.challenge[last], kQuicPathDataBytes);
    run.send_on_path(server_id, second,
                     [&](Span<u8> out)
                     {
                         QuicFrame frame;
                         frame.kind = QuicFrameKind::PathResponse;
                         frame.data = ConstSpan<u8>(answer, sizeof(answer));
                         return write_quic_frame(frame, out);
                     });
    CHECK(paths.validated(paths.active()));
    CHECK(run.server.counters().path_validations == 1U);
    CHECK(run.server.path() == run.from);
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a PATH_ACK of a path ID whose number space never opened is a protocol violation",
          "[net][net-path][quic-connection][multipath]")
{
    // Nothing was sent in path ID 2's space to acknowledge (RFC 9000 section 13.1), though its ID space is open.
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    run.handshake();
    REQUIRE(run.server.multipath());
    const QuicAckRange range{0U, 0U};
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out)
             { return write_quic_path_ack(2U, ConstSpan<QuicAckRange>(&range, 1U), 0U, nullptr, out); });
    REQUIRE(run.server.state() == QuicConnectionState::Closing);
    CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
}

TEST_CASE("QUIC connection: a multipath limit allocates the path slots up front, and any refusal leaves it not ready",
          "[net][net-path][quic-connection][multipath]")
{
    // Opening a path allocates nothing on the peer's behalf (RFC 10069 section 3.1): the slots come at construction.
    Identity identity;
    CountingAllocator allocator;
    QuicConnectionConfig config = config_for(QuicSide::Client, identity, QuicVersion::V1);
    config.parameters.has_initial_max_path_id = true;
    config.parameters.initial_max_path_id = 3U;
    QuicConnectionConfig single = config;
    single.parameters.initial_max_path_id = 0U;
    usize start = allocator.calls();
    {
        QuicConnection one(allocator, single);
        REQUIRE(one.ready());
    }
    const usize without = allocator.calls() - start;
    start = allocator.calls();
    {
        QuicConnection whole(allocator, config);
        REQUIRE(whole.ready());
    }
    const usize total = allocator.calls() - start;
    CHECK(total == without + 2U); // the slots and their sent-packet storage
    for (usize allowed = 0U; allowed < total; ++allowed)
    {
        INFO("allocations allowed " << allowed);
        allocator.refuse_after(allocator.calls() + allowed);
        QuicConnection refused(allocator, config);
        CHECK_FALSE(refused.ready());
        allocator.refuse_after(~usize{0});
    }
}

namespace
{
// A raw client's ID `id` for path ID `path_id`, sent to the server on path ID 0.
void announce_path(RawRun& run, u64 path_id, ConstSpan<u8> id)
{
    QuicFrame issued;
    issued.kind = QuicFrameKind::PathNewConnectionId;
    issued.path_id = path_id;
    issued.data = id;
    const u8 token[kQuicResetTokenBytes] = {0x7A, 0x7B, static_cast<u8>(path_id)};
    issued.reset_token = ConstSpan<u8>(token, sizeof(token));
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(issued, out); });
}

// Its ID for path ID 1, kClientPathCid.
void announce_path_one(RawRun& run)
{
    announce_path(run, 1U, ConstSpan<u8>(kClientPathCid, sizeof(kClientPathCid)));
}

// A raw client's path ID 1: its ID to the server (unless `announce` is false), its protection slot, and its first
// packet there, a PATH_CHALLENGE from `second`.
void open_path_one(RawRun& run, const QuicPath& second, bool announce = true)
{
    if (announce)
    {
        announce_path_one(run);
    }
    REQUIRE(run.raw.protection.open_path(1U, 1U));
    run.raw.path_slot = 1U;
    const u8 data[kQuicPathDataBytes] = {9, 8, 7, 6, 5, 4, 3, 2};
    run.send_on_path(ConstSpan<u8>(run.raw.further_issued[1], run.raw.further_bytes[1]), second,
                     [&](Span<u8> out)
                     {
                         QuicFrame frame;
                         frame.kind = QuicFrameKind::PathChallenge;
                         frame.data = ConstSpan<u8>(data, sizeof(data));
                         return write_quic_frame(frame, out);
                     });
    REQUIRE(run.server.counters().paths_opened == 1U);
}

// The raw client answers the server's latest PATH_CHALLENGE on path ID 1 from `second`: path ID 1 validates.
void validate_path_one(RawRun& run, const QuicPath& second)
{
    REQUIRE(run.raw.challenges != 0U);
    u8 answer[kQuicPathDataBytes] = {};
    std::memcpy(answer, run.raw.challenge[(run.raw.challenges - 1U) % 32U], kQuicPathDataBytes);
    run.send_on_path(ConstSpan<u8>(run.raw.further_issued[1], run.raw.further_bytes[1]), second,
                     [&](Span<u8> out)
                     {
                         QuicFrame frame;
                         frame.kind = QuicFrameKind::PathResponse;
                         frame.data = ConstSpan<u8>(answer, sizeof(answer));
                         return write_quic_frame(frame, out);
                     });
    REQUIRE(run.server.paths(1U).validated(run.server.paths(1U).active()));
}

// What the server sent to `path`: its datagrams' bytes and its PATH_CHALLENGE frames there.
usize bytes_to(const RawRun& run, const QuicPath& path)
{
    usize bytes = 0U;
    for (usize index = 0U; index < run.raw.datagrams && index < 256U; ++index)
    {
        bytes += (run.raw.sent_to[index] == path) ? run.raw.sent_size[index] : 0U;
    }
    return bytes;
}

usize challenges_to(const RawRun& run, const QuicPath& path)
{
    usize count = 0U;
    for (usize index = 0U; index < run.raw.challenges && index < 32U; ++index)
    {
        count += (run.raw.challenge_path[index] == path) ? 1U : 0U;
    }
    return count;
}
} // namespace

TEST_CASE("QUIC connection: a path ID opened before the client's ID for it answers once the ID arrives",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 3.1: an endpoint without an ID for the path may delay its PATH_RESPONSE; nothing goes to
    // another path ID's ID meanwhile.
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    run.handshake();
    const QuicPath second = client_at(9U, 40000U);
    open_path_one(run, second, false);
    CHECK(bytes_to(run, second) == 0U);
    CHECK_FALSE(run.raw.path_response_seen);
    CHECK(run.server.next_timer_ns() > run.now); // the owed answer is no timer send() cannot serve
    announce_path_one(run);
    REQUIRE(run.raw.path_response_seen);
    CHECK(bytes_to(run, second) != 0U);
    CHECK(challenges_to(run, second) == 1U);
    CHECK(run.raw.challenge_dcid[(run.raw.challenges - 1U) % 32U] == kClientPathCid[0]);
}

TEST_CASE("QUIC connection: a client's move on path ID 1 with no ID of its own unused completes when the ID comes "
          "in path ID 0's packet",
          "[net][net-path][quic-connection][multipath]")
{
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    run.handshake();
    const QuicPath second = client_at(9U, 40000U);
    const QuicPath third = client_at(10U, 40000U);
    open_path_one(run, second);
    validate_path_one(run, second);
    REQUIRE(run.raw.further_second_bytes[1] != 0U);
    // Path ID 1 moves to third with the server's sequence 1 there, before the client gave a second ID for it.
    const usize start = run.raw.datagrams;
    run.send_on_path(ConstSpan<u8>(run.raw.further_second[1], run.raw.further_second_bytes[1]), third,
                     [&](Span<u8> out) { return write_quic_frame(path_frame(QuicFrameKind::Ping), out); });
    const QuicPaths& paths = run.server.paths(1U);
    CHECK(paths.path(paths.active()) == second);
    const usize waiting = paths.find(third);
    REQUIRE(waiting != kQuicNoPath);
    CHECK(paths.validating(waiting));
    CHECK(bytes_to(run, third, start) == 0U);
    // Its ID for path ID 1 comes on path ID 0: path ID 1's move happens on that packet.
    constexpr u8 next[sizeof(kClientPathCid)] = {0xD8U, 0xD9U, 0xDAU, 0xDBU, 0xDCU, 0xDDU, 0xDEU, 0xDFU};
    std::memcpy(run.raw.path_next_cid, next, sizeof(next));
    QuicFrame issued = path_frame(QuicFrameKind::PathNewConnectionId, 1U, 1U);
    issued.data = ConstSpan<u8>(next);
    const u8 token[kQuicResetTokenBytes] = {0x7C};
    issued.reset_token = ConstSpan<u8>(token, sizeof(token));
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return write_quic_frame(issued, out); });
    CHECK(paths.path(paths.active()) == third);
    CHECK(paths.validating(waiting));
    CHECK(challenges_to(run, third) != 0U);
    CHECK(only_id_to(run, third, start, 0xD8U)); // never the ID sent to second (section 9.5)
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a further path's challenge timers and expiry run, within its amplification limit",
          "[net][net-path][quic-connection][multipath]")
{
    // The client goes silent after its first packet on path ID 1: the server's challenge goes again on its timer,
    // never past three times the 1200 bytes that arrived, and the validation fails at its deadline with no data sent
    // there (RFC 9000 sections 8.2 and 8.2.4, RFC 10069 section 3.1). Every timer reported due is one send() serves.
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    run.handshake();
    const QuicPath second = client_at(9U, 40000U);
    open_path_one(run, second);
    REQUIRE(challenges_to(run, second) == 1U);
    const QuicPaths& paths = run.server.paths(1U);
    for (usize guard = 0U; guard < 256U && run.server.counters().path_failures == 0U; ++guard)
    {
        const u64 next = run.server.next_timer_ns();
        REQUIRE(next != kNeverNs);
        run.now = (next > run.now) ? next : run.now;
        run.server.on_timeout(run.now);
        run.drain();
        CHECK(run.server.next_timer_ns() > run.now);
        CHECK(bytes_to(run, second) <= 3U * kQuicMinInitialDatagram);
    }
    CHECK(run.server.counters().path_failures == 1U);
    CHECK(challenges_to(run, second) >= 2U);
    CHECK_FALSE(paths.validated(paths.active()));
    CHECK(run.server.state() == QuicConnectionState::Established);
    CHECK(run.server.path() == run.from);
    // The failure abandons path ID 1 (RFC 10069 section 3.1), on path ID 0: the slot sends nothing more.
    run.drain();
    CHECK(run.raw.abandons == 1U);
    CHECK(run.raw.abandon_id == 1U);
    CHECK(run.raw.abandon_error == code(QuicTransportError::PathUnstableOrPoor));
    CHECK_FALSE(run.raw.abandon_further);
    REQUIRE(run.server.abandon_of(1U) != nullptr);
    CHECK_FALSE(run.server.recovery().path_sending(1U));
    CHECK(run.server.counters().paths_abandoned == 1U);
}

TEST_CASE("QUIC connection: a peer's PATH_ABANDON is answered once however often it repeats, the path's stragglers "
          "are acknowledged on path ID 0, and its frames are ignored once it closes",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 sections 3.4 to 4: the receiver answers, keeps the path's state three PTOs from the first frame (no
    // copy extends it), acknowledges what still arrives there on another path, and ignores frames of the path ID.
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    run.handshake();
    const QuicPath second = client_at(9U, 40000U);
    open_path_one(run, second);
    run.drain();
    const u64 unstable = code(QuicTransportError::PathUnstableOrPoor);
    QuicFrame abandon = path_frame(QuicFrameKind::PathAbandon, 1U);
    abandon.error_code = unstable;
    send_frame(run, abandon);
    const QuicPathAbandon* const entry = run.server.abandon_of(1U);
    REQUIRE(entry != nullptr);
    CHECK(entry->answered);
    CHECK(entry->peer_error_code == unstable);
    CHECK_FALSE(run.server.recovery().path_sending(1U));
    run.drain();
    CHECK(run.raw.abandons == 1U);
    CHECK(run.raw.abandon_id == 1U);
    CHECK(run.raw.abandon_error == code(QuicTransportError::NoError));
    CHECK_FALSE(run.raw.abandon_further);
    for (u64 copy = 0U; copy < 64U; ++copy)
    {
        abandon.error_code = copy;
        send_frame(run, abandon);
    }
    run.drain();
    CHECK(entry->peer_error_code == unstable);
    CHECK(entry->deadline_ns == kNeverNs); // its own frame is not yet acknowledged
    CHECK(run.raw.abandons == 1U);
    CHECK(run.server.counters().paths_abandoned == 1U);
    // PATH_STATUS, PATH_CIDS_BLOCKED (past any sequence) and PATH_ACK frames of it are ignored.
    send_frame(run, path_frame(QuicFrameKind::PathStatus, 1U, 1U));
    CHECK(run.server.peer_path_status(1U) == QuicPathStatus::Unknown);
    send_frame(run, path_frame(QuicFrameKind::PathCidsBlocked, 1U, 50U));
    const QuicAckRange first{0U, 0U};
    send_path_ack(run, 1U, ConstSpan<QuicAckRange>(&first, 1U));
    REQUIRE(run.server.state() == QuicConnectionState::Established);
    // A straggler on path ID 1 is read and acknowledged on path ID 0 (section 3.4.3), nothing sent on path ID 1.
    const auto ping_on_one = [&]
    {
        run.send_on_path(ConstSpan<u8>(run.raw.further_issued[1], run.raw.further_bytes[1]), second,
                         [&](Span<u8> out)
                         {
                             QuicFrame ping;
                             ping.kind = QuicFrameKind::Ping;
                             return write_quic_frame(ping, out);
                         });
    };
    const u64 opened = run.server.counters().packets_opened;
    const usize acks = run.raw.path_acks;
    const u64 sent_on_one = run.server.next_packet_number(QuicSpace::Application, 1U);
    ping_on_one();
    CHECK(run.server.counters().packets_opened == opened + 1U);
    run_timers(run, run.now + 100U * kMs);
    CHECK(run.raw.path_acks == acks + 1U);
    CHECK(run.raw.path_ack_id == 1U);
    CHECK(run.server.next_packet_number(QuicSpace::Application, 1U) == sent_on_one);
    // Its PATH_ABANDON acknowledged, the path ID closes three PTOs later.
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    REQUIRE(run.server.abandon_of(1U) != nullptr);
    const u64 deadline = run.server.abandon_of(1U)->deadline_ns;
    const QuicRecovery& recovery = run.server.recovery();
    const u64 own = recovery.probe_period_ns(QuicSpace::Application, 1U);
    const u64 period = (recovery.largest_probe_period_ns() > own) ? recovery.largest_probe_period_ns() : own;
    CHECK(deadline == run.now + 3U * period);
    run_timers(run, deadline);
    CHECK(run.server.counters().paths_closed == 1U);
    CHECK(run.server.abandon_of(1U) == nullptr);
    CHECK(run.server.local_cid_count(1U) == 0U);
    CHECK_FALSE(run.server.recovery().path_open(1U));
    // Gone: its frames and packets are ignored.
    const u64 dropped = run.server.counters().packets_dropped;
    ping_on_one();
    CHECK(run.server.counters().packets_dropped == dropped + 1U);
    send_frame(run, path_frame(QuicFrameKind::PathAbandon, 1U));
    send_frame(run, path_frame(QuicFrameKind::PathCidsBlocked, 1U, 50U));
    send_path_ack(run, 1U, ConstSpan<QuicAckRange>(&first, 1U));
    CHECK(run.server.counters().paths_abandoned == 1U);
    // A path ID never used is answered all the same (section 3.4).
    send_frame(run, path_frame(QuicFrameKind::PathAbandon, 2U));
    run.drain();
    CHECK(run.raw.abandon_id == 2U);
    CHECK(run.server.counters().paths_abandoned == 2U);
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a peer that acknowledges PATH_ABANDON and never answers holds the path ID three PTOs, and "
          "one naming a path ID above the peer's own limit is not answered",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 3.4 leaves an unanswered PATH_ABANDON to the implementation: its state closes three PTOs after
    // the frame's acknowledgement. Section 4: no frame of the server's may name a path ID above the client's limit.
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    run.max_path_id = 2U;
    run.handshake();
    const QuicPath second = client_at(9U, 40000U);
    open_path_one(run, second);
    run.drain();
    const u64 resource = code(QuicTransportError::PathResourceLimitReached);
    CHECK(run.server.abandon_path(3U, resource, run.now) == QuicAbandon::NotOpen); // above the client's limit
    REQUIRE(run.server.abandon_path(1U, resource, run.now) == QuicAbandon::Abandoned);
    run.drain();
    CHECK(run.raw.abandons == 1U);
    CHECK(run.raw.abandon_id == 1U);
    CHECK(run.raw.abandon_error == resource);
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    const QuicPathAbandon* const entry = run.server.abandon_of(1U);
    REQUIRE(entry != nullptr);
    CHECK(entry->acked);
    CHECK_FALSE(entry->answered);
    REQUIRE(entry->deadline_ns != kNeverNs);
    const u64 deadline = entry->deadline_ns;
    run_timers(run, deadline - 1U);
    CHECK(run.server.counters().paths_closed == 0U);
    run_timers(run, deadline);
    CHECK(run.server.counters().paths_closed == 1U);
    CHECK(run.server.local_cid_count(1U) == 0U);
    CHECK_FALSE(run.server.recovery().path_open(1U));
    // The client names path ID 3, under the server's limit and above its own: abandoned with no answer.
    send_frame(run, path_frame(QuicFrameKind::PathAbandon, 3U));
    run.drain();
    CHECK(run.raw.abandons == 1U);
    const QuicPathAbandon* const above = run.server.abandon_of(3U);
    REQUIRE(above != nullptr);
    CHECK_FALSE(above->owed);
    REQUIRE(above->deadline_ns != kNeverNs);
    run_timers(run, above->deadline_ns);
    CHECK(run.server.counters().paths_closed == 2U);
    CHECK(run.raw.abandons == 1U);
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: each path ID that closes raises the server's limit by one; its MAX_PATH_ID goes again "
          "when lost, never once a larger one went, and a path ID under the raised limit is read",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 sections 3.2.1 and 4.6: MAX_PATH_ID raises the limit initial_max_path_id set, is ack-eliciting and
    // is sent again when lost unless a more recent one went; a path ID above the limit is PROTOCOL_VIOLATION.
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 2U);
    run.multipath = true;
    run.max_path_id = 8U;
    run.handshake();
    REQUIRE(run.server.multipath());
    CHECK(run.server.local_max_path_id() == 2U);
    // The raw client abandons a path ID it never used and acknowledges the answer: when the path ID closes.
    const auto abandon = [&](u64 path_id)
    {
        send_frame(run, path_frame(QuicFrameKind::PathAbandon, path_id));
        run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
        const QuicPathAbandon* const entry = run.server.abandon_of(path_id);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->deadline_ns != kNeverNs);
        return entry->deadline_ns;
    };
    run_timers(run, abandon(2U));
    CHECK(run.server.counters().paths_closed == 1U);
    CHECK(run.server.local_max_path_id() == 3U);
    REQUIRE(run.raw.max_path_ids == 1U);
    CHECK(run.raw.max_path_id == 3U);
    // Lost: the raw client acknowledges only the probe after it, and the frame goes again.
    run.raw.seen[run.raw.max_path_id_packet] = false;
    run_timers(run, run.now + 200U * kMs);
    CHECK(run.raw.max_path_ids == 1U); // a probe carries no copy
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    REQUIRE(run.raw.max_path_ids == 2U);
    CHECK(run.raw.max_path_id == 3U);
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    // Two closes apart: the earlier one's frame is lost after the later one's went, and goes no more.
    const u64 three = abandon(3U);
    run.now += kMs;
    const u64 one = abandon(1U);
    REQUIRE(three != one);
    const u64 earlier = (three < one) ? three : one;
    const u64 later = (three < one) ? one : three;
    run_timers(run, earlier);
    CHECK(run.server.local_max_path_id() == 4U);
    REQUIRE(run.raw.max_path_ids == 3U);
    CHECK(run.raw.max_path_id == 4U);
    run.raw.seen[run.raw.max_path_id_packet] = false;
    run_timers(run, later);
    CHECK(run.server.counters().paths_closed == 3U);
    CHECK(run.server.local_max_path_id() == 5U);
    REQUIRE(run.raw.max_path_ids == 4U);
    CHECK(run.raw.max_path_id == 5U);
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    run_timers(run, run.now + 200U * kMs);
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    CHECK(run.raw.max_path_ids == 4U);
    // IDs go out for the raised path IDs within the raw client's limit, and frames of them are read.
    CHECK(run.server.local_cid_count(5U) != 0U);
    CHECK(run.raw.largest_issued == 5U);
    send_frame(run, path_frame(QuicFrameKind::PathStatus, 5U, 1U));
    CHECK(run.server.peer_path_status(5U) == QuicPathStatus::Available);
    REQUIRE(run.server.state() == QuicConnectionState::Established);
    send_frame(run, path_frame(QuicFrameKind::PathStatus, 6U, 1U));
    CHECK(run.server.state() == QuicConnectionState::Closing);
    CHECK(run.server.close_info().error_code == code(QuicTransportError::ProtocolViolation));
}

namespace
{
const Endpoint kSecondAt{IpAddress::v4(192, 0, 2, 66), 41000U}; // a client's socket 1
const Endpoint kThirdAt{IpAddress::v4(198, 18, 0, 9), 42000U};  // and its socket 2

// Two ends offering multipath with further path IDs up to `client_limit` and `server_limit` (the server not at all
// unless `server_offers`), issuing IDs through hosts, on Net's paths: the client's sockets 1 and 2 at kSecondAt and
// kThirdAt, the server at kServerAt.
struct MultipathPair
{
    Identity identity;
    CountingAllocator allocator;
    Host client_host;
    Host server_host;
    QuicConnection client;
    QuicConnection server;
    std::unique_ptr<Net> net;

    // `sent_frames` sizes each end's frame ledger; `datagrams` offers DATAGRAM frames, with their outcomes unless
    // `outcomes` is false, and `redundant` that many redundant ones followed at once; `streams` sizes both ends'.
    explicit MultipathPair(u64 client_limit = 2U, u64 server_limit = 2U, bool disable_migration = false,
                           bool server_offers = true, u32 sent_frames = 1024U, bool datagrams = false,
                           u32 redundant = 0U, bool outcomes = true, const QuicStreamLimits& streams = stream_limits())
        : client_host(allocator, 0U, ConstSpan<u8>(kClientCid, sizeof(kClientCid))),
          server_host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid))),
          client(allocator, made(QuicSide::Client, client_limit, true, false, client_host, sent_frames, datagrams,
                                 redundant, outcomes, streams)),
          server(allocator, made(QuicSide::Server, server_limit, server_offers, disable_migration, server_host,
                                 sent_frames, datagrams, redundant, outcomes, streams))
    {
        REQUIRE(client.ready());
        REQUIRE(server.ready());
        connect(client);
        net = std::make_unique<Net>(client, server);
        net->paths = true;
        net->socket_at[0] = kSecondAt;
        net->socket_at[1] = kThirdAt;
        for (Path& path : net->path)
        {
            path.delay_ns = 20U * kMs;
        }
    }

    QuicConnectionConfig made(QuicSide side, u64 limit, bool offers, bool disable_migration, Host& host,
                              u32 sent_frames, bool datagrams, u32 redundant, bool outcomes,
                              const QuicStreamLimits& streams) const
    {
        const usize seed = (side == QuicSide::Server) ? 1U : 0U;
        QuicConnectionConfig config = hosted(config_for(side, identity, QuicVersion::V1, seed), host, 4U);
        config.streams = streams;
        config.sent_frames = sent_frames;
        if (datagrams)
        {
            config.datagrams = datagram_limits(outcomes);
            config.datagrams.redundant_count = redundant;
        }
        config.parameters.has_initial_max_path_id = offers;
        config.parameters.initial_max_path_id = limit;
        config.parameters.disable_active_migration = disable_migration;
        if (side == QuicSide::Client)
        {
            config.path = QuicPath{0U, kServerAt};
        }
        return config;
    }

    // Confirmed, every ID issued at negotiation acknowledged.
    void confirm()
    {
        REQUIRE(net->run_until([&] { return both_confirmed(client, server); }));
        net->advance(net->now + 300U * kMs);
    }

    // Confirmed, with path ID 1 opened from the client's socket 1 and validated by both ends.
    void open_second()
    {
        confirm();
        REQUIRE(net->run_until([&] { return client.peer_cid_count(1U) != 0U && server.peer_cid_count(1U) != 0U; }));
        u64 path_id = 0U;
        REQUIRE(client.add_path(QuicPath{1U, kServerAt}, net->now, path_id) == QuicMigration::Started);
        REQUIRE(net->run_until([&] { return open_both(1U); }));
    }

    // The client's further path in `slot` is validated, and so is the server's for it.
    [[nodiscard]] bool open_both(usize slot) const
    {
        const QuicPaths& mine = client.paths(slot);
        const QuicPaths& theirs = server.paths(slot);
        return mine.used(mine.active()) && mine.proven(mine.active()) && theirs.used(theirs.active()) &&
               theirs.validated(theirs.active());
    }

    // Writes `bytes` of a new stream of the client's (its FIN after them) until the server read them all.
    void stream_to_server(u64 bytes)
    {
        u64 stream = 0U;
        REQUIRE(client.streams().open(true, stream) == QuicStreamError::None);
        u64 written = 0U;
        u64 read = 0U;
        REQUIRE(net->run_until(
            [&]
            {
                static_cast<void>(write_pattern(client.streams(), stream, written, bytes, stream));
                return server.streams().recv_state(stream) != QuicRecvState::None &&
                       drain(server.streams(), stream, read, stream);
            },
            600000U * kMs));
        CHECK(read == bytes);
    }
};
} // namespace

TEST_CASE("QUIC connection: a client opens path ID 1 from a socket of its own, each end validates it and data crosses "
          "both paths over links that lose, duplicate and jitter",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 3.1: the client's PATH_CHALLENGE opens path ID 1 at the server; each end validates the other's
    // address before data goes there, and path ID 0 stays where it was.
    for (u64 seed = 1U; seed <= 3U; ++seed)
    {
        INFO("seed " << seed);
        MultipathPair pair;
        pair.net->rng.state = seed;
        for (Path& path : pair.net->path)
        {
            path.loss = 40U;
            path.duplicate = 30U;
            path.jitter_ns = 4U * kMs;
        }
        pair.confirm();
        REQUIRE(pair.net->run_until(
            [&] { return pair.client.peer_cid_count(1U) != 0U && pair.server.peer_cid_count(1U) != 0U; }));
        const QuicPath second{1U, kServerAt};
        u64 path_id = 9U;
        REQUIRE(pair.client.add_path(second, pair.net->now, path_id) == QuicMigration::Started);
        CHECK(path_id == 1U);
        CHECK(pair.client.counters().paths_opened == 1U);
        const QuicPaths& mine = pair.client.paths(1U);
        CHECK(mine.path(mine.active()) == second);
        CHECK_FALSE(mine.proven(mine.active()));
        REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
        CHECK(pair.server.counters().paths_opened == 1U);
        const QuicPaths& theirs = pair.server.paths(1U);
        CHECK(theirs.path(theirs.active()).peer == kSecondAt);
        CHECK(pair.client.path() == QuicPath{0U, kServerAt});
        CHECK(pair.server.path().peer == pair.net->client_at);

        // A stream to the server rides both paths in turn, each numbering its own packets.
        REQUIRE(pair.client.set_path_choice(QuicPathChoice::Turns));
        const u64 first_before = pair.client.next_packet_number(QuicSpace::Application, 0U);
        const u64 second_before = pair.client.next_packet_number(QuicSpace::Application, 1U);
        pair.stream_to_server(96U * 1024U);
        CHECK(pair.client.next_packet_number(QuicSpace::Application, 0U) > first_before + 16U);
        CHECK(pair.client.next_packet_number(QuicSpace::Application, 1U) > second_before + 16U);
        CHECK(pair.server.next_packet_number(QuicSpace::Application, 1U) != 0U);
        REQUIRE(pair.net->run_until([&] { return pair.client.recovery().bytes_in_flight(1U) == 0U; }));
        CHECK(pair.client.counters().path_failures == 0U);
        CHECK(pair.client.state() == QuicConnectionState::Established);
        CHECK(pair.server.state() == QuicConnectionState::Established);
    }
}

namespace
{
// At each datagram of the sender's `slot` that carries more than acknowledgements, that slot's bytes in flight are
// within its window unless it owes its own probe: another path's owed probe opens neither its window nor its pacer.
class WindowWatch final : public Filter
{
public:
    // From the slot's next packet: what it put in flight before is not this watch's.
    WindowWatch(const QuicConnection& sender, usize from, usize slot)
        : m_sender(sender), m_from(from), m_slot(slot), m_next(sender.next_packet_number(QuicSpace::Application, slot))
    {
    }

    bool drop(usize from, ConstSpan<u8> datagram) override
    {
        const u64 next = m_sender.next_packet_number(QuicSpace::Application, m_slot);
        const QuicPathControl* const control = m_sender.path_control(m_slot);
        if (from == m_from && next != m_next && datagram.size() >= kQuicMinInitialDatagram / 2U && control != nullptr &&
            m_sender.recovery().pto_count(m_slot) == 0U)
        {
            m_kept = m_kept && m_sender.recovery().bytes_in_flight(m_slot) <= control->congestion().window();
            ++m_checked;
        }
        m_next = (from == m_from) ? next : m_next;
        return false;
    }

    [[nodiscard]] bool kept() const { return m_kept; }
    [[nodiscard]] usize checked() const { return m_checked; }

private:
    const QuicConnection& m_sender;
    usize m_from;
    usize m_slot;
    u64 m_next = 0U;
    bool m_kept = true;
    usize m_checked = 0U;
};
} // namespace

TEST_CASE("QUIC connection: a path that goes dark holds no data back: its probe timeout sends its frames again on the "
          "live path, its probes carry none, and once it returns it carries data again",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 5.7: no packet of a dark path is declared lost before an acknowledgement on it, so without the
    // resend its stream data would hold the stream, its datagrams their outcomes and its records the 64-record ledger
    // until the idle timeout. Either end sends; path ID 1 or path ID 0 goes dark, both ways.
    for (usize run = 0U; run < 4U; ++run)
    {
        const bool by_client = (run % 2U) == 0U;
        const usize dark = (run < 2U) ? 1U : 0U; // its slot, and the client's socket it leaves from
        INFO((by_client ? "the client sends" : "the server sends") << ", path ID " << dark << " goes dark");
        MultipathPair pair(2U, 2U, false, true, 64U, true);
        QuicConnection& sender = by_client ? pair.client : pair.server;
        QuicConnection& receiver = by_client ? pair.server : pair.client;
        // Each path acknowledged on itself, so the dark path's loss is the sender's alone (shortest-path PATH_ACKs
        // that share its fate: the PATH_ACK placement cases).
        REQUIRE(receiver.set_path_choice(QuicPathChoice::Turns));
        // Path ID 1 opens with a window of two datagrams, so the live path waits on its window as the dark one probes.
        CongestionPolicy policy;
        policy.algorithm = CongestionAlgorithm::NewReno;
        policy.initial_packets = 2U;
        policy.hystart = false;
        REQUIRE(sender.set_congestion(policy));
        for (Path& path : pair.net->path)
        {
            path.rate = 150000U; // a drop-tail bottleneck both paths share keeps each window below what waits
            path.buffer = 6000U;
        }
        pair.open_second();
        const usize live = 1U - dark;
        const QuicRecovery& recovery = sender.recovery();
        u64 stream = 0U;
        REQUIRE(sender.streams().open(true, stream) == QuicStreamError::None);
        constexpr u64 total = 256U * 1024U;
        DatagramFlow datagrams(sender, receiver, run + 1U, false);
        u64 written = 0U;
        u64 read = 0U;
        u64 dark_ns = kNeverNs;
        u64 dark_number = 0U; // the dark slot's next packet number and bytes in flight at its first probe timeout
        u64 dark_flight = 0U;
        WindowWatch watch(sender, by_client ? 0U : 1U, live);
        pair.net->filter = &watch;
        const auto flow = [&]
        {
            static_cast<void>(write_pattern(sender.streams(), stream, written, total, stream));
            const bool all_read = receiver.streams().recv_state(stream) != QuicRecvState::None &&
                                  drain(receiver.streams(), stream, read, stream);
            const bool reported = datagrams.step(pair.net->now);
            if (dark_ns == kNeverNs && written > total / 4U && recovery.bytes_in_flight(dark) > 4096U)
            {
                pair.net->down = 1U << dark;
                dark_ns = pair.net->now;
            }
            if (dark_ns != kNeverNs && dark_number == 0U && recovery.pto_count(dark) != 0U)
            {
                dark_number = sender.next_packet_number(QuicSpace::Application, dark);
                dark_flight = recovery.bytes_in_flight(dark);
            }
            return all_read && reported;
        };
        REQUIRE(pair.net->run_until(flow, 20000U * kMs));
        REQUIRE(dark_ns != kNeverNs);
        CHECK(read == total);
        CHECK(pair.net->now - dark_ns < 8000U * kMs);
        datagrams.check();
        CHECK(datagrams.lost() != 0U); // the dark path's and the bottleneck's, each reported once
        CHECK(sender.counters().pto_resends != 0U);
        CHECK(watch.checked() > 64U);
        CHECK(watch.kept());
        pair.net->filter = nullptr;
        for (Path& path : pair.net->path)
        {
            path.rate = 0U; // and no loss from here: no probe timeout but the dark path's
            path.buffer = 0U;
        }
        // Its later probes carry an ack-eliciting frame, no data: a few dozen bytes each, never a datagram's worth.
        REQUIRE(pair.net->run_until([&] { return recovery.pto_count(dark) >= 3U; }));
        const u64 probes = sender.next_packet_number(QuicSpace::Application, dark) - dark_number;
        CHECK(probes >= 2U);
        CHECK(recovery.bytes_in_flight(dark) - dark_flight <= probes * 128U);
        const u64 resent = sender.counters().pto_resends;
        CHECK(sender.counters().paths_abandoned == 0U);
        CHECK(sender.state() == QuicConnectionState::Established);
        CHECK(receiver.state() == QuicConnectionState::Established);

        // It returns: its probes are answered, the timeout clears and data rides it again; nothing more was resent.
        pair.net->down = 0U;
        REQUIRE(pair.net->run_until([&] { return recovery.pto_count(dark) == 0U; }));
        CHECK(sender.counters().pto_resends == resent);
        const u64 before = sender.next_packet_number(QuicSpace::Application, dark);
        u64 again = 0U;
        REQUIRE(sender.streams().open(true, again) == QuicStreamError::None);
        written = 0U;
        read = 0U;
        REQUIRE(pair.net->run_until(
            [&]
            {
                static_cast<void>(write_pattern(sender.streams(), again, written, total, again));
                return receiver.streams().recv_state(again) != QuicRecvState::None &&
                       drain(receiver.streams(), again, read, again);
            }));
        CHECK(sender.next_packet_number(QuicSpace::Application, dark) > before + 16U);
        CHECK(sender.counters().pto_resends == resent);
    }
}

TEST_CASE("QUIC connection: a path dark for long stays held while the path carrying its data takes a timeout of its "
          "own, so its probes still carry no data",
          "[net][net-path][quic-connection][multipath]")
{
    // Path ID 1 goes dark past four probe timeouts; path ID 0 then goes dark too until path ID 1 times out again, with
    // path ID 0's own timeouts pending; then path ID 0 returns alone. Path ID 1, with more timeouts pending than path
    // ID 0 throughout, carries none of the data waiting: its datagrams are probes of a few dozen bytes.
    MultipathPair pair;
    for (Path& path : pair.net->path)
    {
        path.rate = 150000U; // the window, not the stream's buffer, holds data back: data waits throughout
        path.buffer = 6000U;
    }
    pair.open_second();
    QuicConnection& client = pair.client;
    const QuicRecovery& recovery = client.recovery();
    u64 stream = 0U;
    REQUIRE(client.streams().open(true, stream) == QuicStreamError::None);
    constexpr u64 total = 3U * 1024U * 1024U;
    u64 written = 0U;
    u64 read = 0U;
    const auto move = [&]
    {
        static_cast<void>(write_pattern(client.streams(), stream, written, total, stream));
        return pair.server.streams().recv_state(stream) != QuicRecvState::None &&
               drain(pair.server.streams(), stream, read, stream);
    };
    REQUIRE(pair.net->run_until([&] { return !move() && recovery.bytes_in_flight(1U) > 4096U; }));
    pair.net->down = 1U << 1U;
    REQUIRE(pair.net->run_until([&] { return !move() && recovery.pto_count(1U) >= 4U; }));
    const u64 number = client.next_packet_number(QuicSpace::Application, 1U);
    const u64 flight = recovery.bytes_in_flight(1U);
    const u32 timeouts = recovery.pto_count(1U);
    pair.net->down = (1U << 0U) | (1U << 1U);
    REQUIRE(pair.net->run_until(
        [&]
        {
            static_cast<void>(move());
            return recovery.pto_count(0U) != 0U && recovery.pto_count(1U) > timeouts;
        }));
    CHECK(recovery.pto_count(0U) < recovery.pto_count(1U));
    pair.net->down = 1U << 1U;
    REQUIRE(pair.net->run_until(move));
    CHECK(read == total);
    const u64 probes = client.next_packet_number(QuicSpace::Application, 1U) - number;
    CHECK(probes >= 2U);
    CHECK(recovery.bytes_in_flight(1U) - flight <= probes * 128U);
    CHECK(client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a path whose acknowledgements go dark has its frames sent again on the live path, and "
          "their late acknowledgements count nothing twice",
          "[net][net-path][quic-connection][multipath]")
{
    // Path ID 1 carries the client's packets to the server but nothing back: the server holds the originals, the
    // client's probe timeout sends their frames again on path ID 0, and the PATH_ACKs that come once the path returns
    // acknowledge packets whose records were settled. Every datagram is reported once, the stream is acknowledged.
    MultipathPair pair(2U, 2U, false, true, 64U, true);
    REQUIRE(pair.server.set_path_choice(QuicPathChoice::Turns)); // each path acknowledged on itself
    pair.open_second();
    QuicConnection& client = pair.client;
    const QuicRecovery& recovery = client.recovery();
    u64 stream = 0U;
    REQUIRE(client.streams().open(true, stream) == QuicStreamError::None);
    constexpr u64 total = 128U * 1024U;
    DatagramFlow datagrams(client, pair.server, 7U, false);
    u64 written = 0U;
    u64 read = 0U;
    bool gone = false;
    REQUIRE(pair.net->run_until(
        [&]
        {
            static_cast<void>(write_pattern(client.streams(), stream, written, total, stream));
            const bool all_read = pair.server.streams().recv_state(stream) != QuicRecvState::None &&
                                  drain(pair.server.streams(), stream, read, stream);
            const bool reported = datagrams.step(pair.net->now);
            if (!gone && written > total / 4U && recovery.bytes_in_flight(1U) > 4096U)
            {
                pair.net->down_in = 1U << 1U;
                gone = true;
            }
            return all_read && reported && recovery.pto_count(1U) >= 2U;
        },
        20000U * kMs));
    CHECK(read == total);
    CHECK(client.counters().pto_resends != 0U);
    const u64 resent = client.counters().pto_resends;
    pair.net->down_in = 0U;
    REQUIRE(pair.net->run_until(
        [&]
        {
            static_cast<void>(datagrams.step(pair.net->now));
            return recovery.pto_count(1U) == 0U && recovery.bytes_in_flight(1U) == 0U &&
                   client.streams().send_state(stream) == QuicSendState::DataRecvd;
        }));
    datagrams.check();
    CHECK(client.counters().pto_resends == resent);
    CHECK(client.state() == QuicConnectionState::Established);
    CHECK(pair.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: with every path dark frames move once, to the path not yet known dark, never back, and "
          "the transfer ends once they return",
          "[net][net-path][quic-connection][multipath]")
{
    // Both paths go dark at once. The first to time out still sees the other live and hands it its frames; the other's
    // timeouts then find no live path and keep RFC 9002's probes, as on one path, so nothing moves again.
    MultipathPair pair(2U, 2U, false, true, 64U);
    pair.open_second();
    QuicConnection& client = pair.client;
    const QuicRecovery& recovery = client.recovery();
    u64 stream = 0U;
    REQUIRE(client.streams().open(true, stream) == QuicStreamError::None);
    constexpr u64 total = 128U * 1024U;
    u64 written = 0U;
    u64 read = 0U;
    bool gone = false;
    u64 first = kNeverNs; // pto_resends once a path first timed out
    bool moved_once = true;
    REQUIRE(pair.net->run_until(
        [&]
        {
            static_cast<void>(write_pattern(client.streams(), stream, written, total, stream));
            static_cast<void>(pair.server.streams().recv_state(stream) != QuicRecvState::None &&
                              drain(pair.server.streams(), stream, read, stream));
            if (!gone && written > total / 4U && recovery.bytes_in_flight(0U) != 0U &&
                recovery.bytes_in_flight(1U) != 0U)
            {
                pair.net->down = (1U << 0U) | (1U << 1U);
                gone = true;
            }
            if (gone && first == kNeverNs && (recovery.pto_count(0U) != 0U || recovery.pto_count(1U) != 0U))
            {
                first = client.counters().pto_resends;
            }
            moved_once = moved_once && (first == kNeverNs || client.counters().pto_resends == first);
            return gone && recovery.pto_count(0U) >= 3U && recovery.pto_count(1U) >= 3U;
        }));
    CHECK(first != 0U);
    CHECK(moved_once);
    pair.net->down = 0U;
    REQUIRE(pair.net->run_until(
        [&]
        {
            static_cast<void>(write_pattern(client.streams(), stream, written, total, stream));
            return pair.server.streams().recv_state(stream) != QuicRecvState::None &&
                   drain(pair.server.streams(), stream, read, stream);
        }));
    CHECK(read == total);
    CHECK(client.state() == QuicConnectionState::Established);
}

namespace
{
// Counts the datagrams `sender` (Net's end `from`) sends with data, by the path slot whose packet number moved for it
// (sent(), which also sees the datagrams a socket down loses): over
// 900 bytes and no larger than that slot's datagram size (a PMTU probe is larger), with no probe timeout pending on
// it. Under QuicPathChoice::LowestRtt none may leave a slot while a slot ranked before it (the lower smoothed RTT,
// then the lower slot), live and not a peer's backup path, had room in its window and ledger for a whole datagram
// and its pacer released (kept()).
class DataWatch final : public Filter
{
public:
    DataWatch(const QuicConnection& sender, usize from, const u64& now) : m_sender(sender), m_from(from), m_now(now)
    {
        for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
        {
            m_next[slot] = sender.next_packet_number(QuicSpace::Application, slot);
        }
    }

    void sent(usize from, ConstSpan<u8> /*datagram*/) override
    {
        if (from != m_from)
        {
            return;
        }
        m_sent = kQuicMaxPaths;
        for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
        {
            const u64 next = m_sender.next_packet_number(QuicSpace::Application, slot);
            m_sent = (next != m_next[slot]) ? slot : m_sent;
            m_next[slot] = next;
        }
    }

    bool drop(usize from, ConstSpan<u8> datagram) override
    {
        if (from != m_from)
        {
            return false;
        }
        const usize sent = m_sent;
        const QuicRecovery& recovery = m_sender.recovery();
        const QuicPathControl* const control = (sent < kQuicMaxPaths) ? m_sender.path_control(sent) : nullptr;
        if (control == nullptr || datagram.size() < 900U || datagram.size() > control->datagram_bytes() ||
            recovery.pto_count(sent) != 0U)
        {
            return false;
        }
        ++m_data[sent];
        for (usize other = 0U; other < kQuicMaxPaths && m_sender.path_choice() == QuicPathChoice::LowestRtt; ++other)
        {
            const QuicPathControl* const better = m_sender.path_control(other);
            if (other == sent || better == nullptr || recovery.pto_count(other) != 0U ||
                m_sender.peer_path_status(other) == QuicPathStatus::Backup || !before(other, sent))
            {
                continue;
            }
            const u64 in_flight = recovery.bytes_in_flight(other);
            const bool room = in_flight + better->datagram_bytes() <= better->congestion().window();
            const bool released = better->release_ns(m_now, recovery.rtt(other)) <= m_now;
            m_kept = m_kept && !(room && released && recovery.can_send_data(QuicSpace::Application, other));
        }
        return false;
    }

    void reset()
    {
        for (usize& count : m_data)
        {
            count = 0U;
        }
    }
    [[nodiscard]] usize data(usize slot) const { return m_data[slot]; }
    [[nodiscard]] bool kept() const { return m_kept; }

private:
    [[nodiscard]] bool before(usize a, usize b) const
    {
        const u64 first = m_sender.recovery().rtt(a).smoothed_ns;
        const u64 second = m_sender.recovery().rtt(b).smoothed_ns;
        return (first != second) ? first < second : a < b;
    }

    const QuicConnection& m_sender;
    usize m_from;
    const u64& m_now;
    u64 m_next[kQuicMaxPaths] = {};
    usize m_sent = kQuicMaxPaths;
    usize m_data[kQuicMaxPaths] = {};
    bool m_kept = true;
};

// Opens a unidirectional stream of the client's once a slot is free: both ends poll their streams' events, which
// frees the slots of streams done, and the server's MAX_STREAMS grants them again.
[[nodiscard]] bool open_uni(MultipathPair& pair, u64& stream)
{
    return pair.net->run_until(
        [&]
        {
            QuicStreamEvent event;
            for (QuicConnection* end : {&pair.client, &pair.server})
            {
                while (end->streams().poll(event))
                {
                }
            }
            return pair.client.streams().open(false, stream) == QuicStreamError::None;
        });
}

// A new unidirectional stream of the client's that never fills a window: `chunks` of 1000 bytes, one every 20 ms (the
// last with its FIN), until the server read them all.
void trickle(MultipathPair& pair, usize chunks)
{
    u64 stream = 0U;
    REQUIRE(open_uni(pair, stream));
    u64 written = 0U;
    u64 read = 0U;
    const auto delivered = [&]
    {
        return pair.server.streams().recv_state(stream) != QuicRecvState::None &&
               drain(pair.server.streams(), stream, read, stream);
    };
    for (usize chunk = 0U; chunk < chunks; ++chunk)
    {
        static_cast<void>(
            write_pattern(pair.client.streams(), stream, written, (chunk + 1U) * 1000U, stream, chunk + 1U == chunks));
        pair.net->advance(pair.net->now + 20U * kMs);
        static_cast<void>(delivered());
    }
    REQUIRE(pair.net->run_until(
        [&]
        {
            static_cast<void>(write_pattern(pair.client.streams(), stream, written, chunks * 1000U, stream));
            return delivered();
        }));
    CHECK(read == chunks * 1000U);
}

// A new unidirectional stream of the client's carrying `bytes` as fast as the windows allow, until the server read it.
void flood(MultipathPair& pair, u64 bytes)
{
    u64 stream = 0U;
    REQUIRE(open_uni(pair, stream));
    u64 written = 0U;
    u64 read = 0U;
    REQUIRE(pair.net->run_until(
        [&]
        {
            static_cast<void>(write_pattern(pair.client.streams(), stream, written, bytes, stream));
            return pair.server.streams().recv_state(stream) != QuicRecvState::None &&
                   drain(pair.server.streams(), stream, read, stream);
        },
        600000U * kMs));
    CHECK(read == bytes);
}
} // namespace

TEST_CASE("QUIC connection: by the lowest RTT data rides the faster path while its window holds a datagram, spills "
          "onto the slower one only while that window is full, and follows the paths' measured RTTs",
          "[net][net-path][quic-connection][multipath]")
{
    // S78's scheduling (RFC 10069 section 5.5 leaves it to the implementation): path ID 1 is 60 ms slower each round
    // trip, then path ID 0 is.
    MultipathPair pair;
    Host spare(pair.allocator, 0U, ConstSpan<u8>(kClientCid, sizeof(kClientCid)));
    QuicConnectionConfig refused =
        pair.made(QuicSide::Client, 2U, true, false, spare, 1024U, false, 0U, true, stream_limits());
    refused.path_choice = static_cast<QuicPathChoice>(2U);
    CHECK_FALSE(QuicConnection(pair.allocator, refused).ready());
    pair.net->socket_delay_ns[1] = 30U * kMs;
    pair.open_second();
    const QuicRecovery& recovery = pair.client.recovery();
    CHECK(pair.client.path_choice() == QuicPathChoice::LowestRtt);
    DataWatch watch(pair.client, 0U, pair.net->now);
    pair.net->filter = &watch;
    // A trickle never fills a window: all of it goes on path ID 0.
    trickle(pair, 100U);
    CHECK(watch.data(0U) >= 90U);
    CHECK(watch.data(1U) == 0U);
    // A bulk transfer: path ID 1 carries what path ID 0's window and pacer hold back.
    watch.reset();
    flood(pair, 512U * 1024U);
    CHECK(watch.data(1U) >= 16U);
    CHECK(watch.data(0U) > watch.data(1U));
    CHECK(watch.kept());
    CHECK(recovery.rtt(0U).smoothed_ns < recovery.rtt(1U).smoothed_ns);
    // The paths swap: a bulk transfer measures both again, and the trickle then rides path ID 1 alone.
    pair.net->socket_delay_ns[0] = 30U * kMs;
    pair.net->socket_delay_ns[1] = 0U;
    flood(pair, 512U * 1024U);
    REQUIRE(recovery.rtt(1U).smoothed_ns < recovery.rtt(0U).smoothed_ns);
    watch.reset();
    trickle(pair, 100U);
    CHECK(watch.data(1U) >= 90U);
    CHECK(watch.data(0U) == 0U);
    CHECK(watch.kept());
    // Turns, live: the trickle rides both paths; a value outside QuicPathChoice changes nothing.
    REQUIRE(pair.client.set_path_choice(QuicPathChoice::Turns));
    watch.reset();
    trickle(pair, 100U);
    CHECK(watch.data(0U) >= 20U);
    CHECK(watch.data(1U) >= 20U);
    CHECK_FALSE(pair.client.set_path_choice(static_cast<QuicPathChoice>(2U)));
    CHECK(pair.client.path_choice() == QuicPathChoice::Turns);
    pair.net->filter = nullptr;
    CHECK(pair.client.counters().path_failures == 0U);
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a path whose socket would block leaves its data to the next path by RTT, so nothing "
          "stalls, and takes it back once the socket is free",
          "[net][net-path][quic-connection][multipath]")
{
    // yields_data(): a slot yields only to one that sends in the same round, so a held socket never holds data back.
    MultipathPair pair;
    pair.net->socket_delay_ns[1] = 30U * kMs;
    pair.open_second();
    DataWatch watch(pair.client, 0U, pair.net->now);
    pair.net->filter = &watch;
    pair.net->held[0] = 1U; // the client's socket 0, path ID 0's
    trickle(pair, 40U);
    CHECK(watch.data(1U) >= 36U);
    CHECK(watch.data(0U) == 0U);
    pair.net->held[0] = 0U;
    watch.reset();
    trickle(pair, 40U);
    CHECK(watch.data(0U) >= 36U);
    CHECK(watch.data(1U) == 0U);
    pair.net->filter = nullptr;
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a peer's backup path carries no data while another of its paths is usable, under either "
          "choice; it takes the data while that path is dark or every path is backup, and gives it back",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 3.3: "no traffic should be sent on that path if another path is available and usable"; with
    // every established path backup no guidance is given. Path ID 1 is the faster, so the lowest RTT would take it.
    MultipathPair pair;
    pair.net->socket_delay_ns[0] = 30U * kMs;
    pair.open_second();
    DataWatch watch(pair.client, 0U, pair.net->now);
    pair.net->filter = &watch;
    trickle(pair, 40U);
    CHECK(watch.data(1U) >= 36U);
    REQUIRE(pair.server.set_path_status(1U, true) == QuicPathStatusChange::Set);
    CHECK(pair.server.path_status(1U) == QuicPathStatus::Backup);
    CHECK(pair.server.path_status(0U) == QuicPathStatus::Unknown);
    REQUIRE(pair.net->run_until([&] { return pair.client.peer_path_status(1U) == QuicPathStatus::Backup; }));
    CHECK(pair.client.path_status(1U) == QuicPathStatus::Unknown); // each end's preference is its own
    CHECK(pair.server.peer_path_status(1U) == QuicPathStatus::Unknown);
    for (const QuicPathChoice choice : {QuicPathChoice::LowestRtt, QuicPathChoice::Turns})
    {
        INFO("choice " << static_cast<u32>(choice));
        REQUIRE(pair.client.set_path_choice(choice));
        watch.reset();
        trickle(pair, 40U);
        flood(pair, 128U * 1024U); // a full window spills onto no backup path either
        CHECK(watch.data(0U) >= 40U);
        CHECK(watch.data(1U) == 0U);
    }
    REQUIRE(pair.client.set_path_choice(QuicPathChoice::LowestRtt));
    // Path ID 0 goes dark: once its probe timeout is pending, the backup path takes the data; back, it gives it back.
    pair.net->down = 1U;
    watch.reset();
    trickle(pair, 40U);
    CHECK(watch.data(1U) >= 20U);
    CHECK(pair.client.counters().pto_resends != 0U);
    pair.net->down = 0U;
    REQUIRE(pair.net->run_until([&] { return pair.client.recovery().pto_count(0U) == 0U; }));
    watch.reset();
    trickle(pair, 40U);
    CHECK(watch.data(0U) >= 36U);
    CHECK(watch.data(1U) == 0U);
    // Every path backup: no guidance, so the lowest RTT takes path ID 1; then path ID 1 alone available, then both.
    REQUIRE(pair.server.set_path_status(0U, true) == QuicPathStatusChange::Set);
    REQUIRE(pair.net->run_until([&] { return pair.client.peer_path_status(0U) == QuicPathStatus::Backup; }));
    watch.reset();
    trickle(pair, 40U);
    CHECK(watch.data(1U) >= 36U);
    CHECK(watch.data(0U) == 0U);
    REQUIRE(pair.server.set_path_status(1U, false) == QuicPathStatusChange::Set);
    REQUIRE(pair.net->run_until([&] { return pair.client.peer_path_status(1U) == QuicPathStatus::Available; }));
    REQUIRE(pair.server.set_path_status(0U, false) == QuicPathStatusChange::Set);
    REQUIRE(pair.net->run_until([&] { return pair.client.peer_path_status(0U) == QuicPathStatus::Available; }));
    watch.reset();
    flood(pair, 128U * 1024U);
    CHECK(watch.data(0U) != 0U); // path ID 0 takes what path ID 1's window cannot again
    CHECK(watch.data(1U) > watch.data(0U));
    CHECK(watch.kept());
    pair.net->filter = nullptr;
    CHECK(pair.client.counters().paths_abandoned == 0U);
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a path left without data is measured again each second while another carries data, so "
          "the lowest RTT follows it once it is faster; never a backup path, never without data",
          "[net][net-path][quic-connection][multipath]")
{
    // S78's scheduling (RFC 10069 section 5.9: an idle path probed before it is switched to). Path ID 1 is 60 ms
    // slower each round trip, then 20 ms faster than path ID 0, less than the peer's ACK delay; a trickle never fills
    // path ID 0's window, so path ID 1 learns it only from its refreshes, whose sample carries no ACK delay.
    MultipathPair pair;
    Host spare(pair.allocator, 0U, ConstSpan<u8>(kClientCid, sizeof(kClientCid)));
    QuicConnectionConfig config =
        pair.made(QuicSide::Client, 2U, true, false, spare, 1024U, false, 0U, true, stream_limits());
    config.path_refresh_ns = kQuicGranularityNs - 1U;
    CHECK_FALSE(QuicConnection(pair.allocator, config).ready());
    config.path_refresh_ns = 0U;
    CHECK(QuicConnection(pair.allocator, config).ready());
    CHECK(pair.client.path_refresh_ns() == kQuicPathRefreshNs);
    pair.net->socket_delay_ns[1] = 30U * kMs;
    pair.open_second();
    const QuicRecovery& recovery = pair.client.recovery();
    DataWatch watch(pair.client, 0U, pair.net->now);
    pair.net->filter = &watch;
    trickle(pair, 100U); // two seconds
    CHECK(watch.data(1U) == 0U);
    CHECK(pair.client.counters().path_refreshes >= 1U);
    CHECK(pair.client.counters().path_refreshes <= 2U);
    CHECK(recovery.rtt(0U).smoothed_ns < recovery.rtt(1U).smoothed_ns);
    pair.net->socket_delay_ns[0] = 10U * kMs;
    pair.net->socket_delay_ns[1] = 0U;
    watch.reset();
    trickle(pair, 150U);
    CHECK(recovery.rtt(1U).smoothed_ns < recovery.rtt(0U).smoothed_ns);
    CHECK(watch.data(1U) >= 90U);
    CHECK(watch.kept());
    // A bulk transfer measures both paths. Then, without refreshes, live: path ID 0 is the faster, by 10 ms each
    // round trip, but its last measure is the slower and the trickle never learns it; with them it does.
    flood(pair, 512U * 1024U);
    REQUIRE(recovery.rtt(1U).smoothed_ns < recovery.rtt(0U).smoothed_ns);
    CHECK_FALSE(pair.client.set_path_refresh(kQuicGranularityNs - 1U));
    CHECK(pair.client.path_refresh_ns() == kQuicPathRefreshNs);
    REQUIRE(pair.client.set_path_refresh(0U));
    pair.net->socket_delay_ns[0] = 0U;
    pair.net->socket_delay_ns[1] = 5U * kMs;
    u64 refreshes = pair.client.counters().path_refreshes;
    watch.reset();
    trickle(pair, 150U);
    CHECK(watch.data(0U) == 0U);
    CHECK(pair.client.counters().path_refreshes == refreshes);
    REQUIRE(pair.client.set_path_refresh(kQuicPathRefreshNs));
    watch.reset();
    trickle(pair, 150U);
    CHECK(watch.data(0U) >= 90U);
    CHECK(pair.client.counters().path_refreshes > refreshes);
    // A peer's backup path is not used while another is usable (section 3.3), not even to measure it.
    REQUIRE(pair.server.set_path_status(0U, true) == QuicPathStatusChange::Set);
    REQUIRE(pair.net->run_until([&] { return pair.client.peer_path_status(0U) == QuicPathStatus::Backup; }));
    refreshes = pair.client.counters().path_refreshes;
    trickle(pair, 150U);
    CHECK(pair.client.counters().path_refreshes == refreshes);
    // Without data no path is refreshed: a connection left idle sends nothing more, so it idles out as without them.
    REQUIRE(pair.server.set_path_status(0U, false) == QuicPathStatusChange::Set);
    REQUIRE(pair.net->run_until([&] { return pair.client.peer_path_status(0U) == QuicPathStatus::Available; }));
    pair.net->advance(pair.net->now + 500U * kMs);
    const u64 client_sent = pair.client.counters().datagrams_sent;
    const u64 server_sent = pair.server.counters().datagrams_sent;
    pair.net->advance(pair.net->now + 5000U * kMs);
    CHECK(pair.client.counters().datagrams_sent == client_sent);
    CHECK(pair.server.counters().datagrams_sent == server_sent);
    CHECK(pair.client.counters().path_refreshes == refreshes);
    CHECK(pair.server.counters().path_refreshes == 0U); // it acknowledged and granted credit, and sent no data
    pair.net->filter = nullptr;
    CHECK(pair.client.counters().path_failures == 0U);
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: an idle path gone dark is found by its refresh's probe timeout and carries no data, "
          "while without refreshes it is not",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 sections 5.7 and 5.9: the refresh lost on a dead path becomes its probe timeout, so the path is held
    // dark before any data goes there; the trickle stays whole on path ID 0.
    for (const bool refresh : {true, false})
    {
        INFO("refresh " << refresh);
        MultipathPair pair;
        pair.net->socket_delay_ns[1] = 30U * kMs;
        pair.open_second();
        REQUIRE(pair.client.set_path_refresh(refresh ? kQuicPathRefreshNs : 0U));
        pair.net->advance(pair.net->now + 1000U * kMs); // what the opening owed is acknowledged
        DataWatch watch(pair.client, 0U, pair.net->now);
        pair.net->filter = &watch;
        pair.net->down = 2U; // the client's socket 1, path ID 1's
        trickle(pair, 100U);
        CHECK(watch.data(1U) == 0U);
        CHECK((pair.client.recovery().pto_count(1U) != 0U) == refresh);
        CHECK(pair.client.counters().path_refreshes == (refresh ? 1U : 0U)); // then it times out, and owes none
        pair.net->filter = nullptr;
        CHECK(pair.client.counters().path_failures == 0U);
        CHECK(pair.client.state() == QuicConnectionState::Established);
    }
}

namespace
{
// Reads what arrived of `stream` at `to`.
void read_some(QuicConnection& to, u64 stream, u64& read)
{
    if (to.streams().recv_state(stream) != QuicRecvState::None)
    {
        static_cast<void>(drain(to.streams(), stream, read, stream));
    }
}

// Streams `chunks` of 500 bytes of `stream`, one every 20 ms, from `from` to `to`, each read as it arrives.
void chunks_over(MultipathPair& pair, QuicConnection& from, QuicConnection& to, u64 stream, usize chunks, u64& written,
                 u64& read)
{
    for (usize chunk = 0U; chunk < chunks; ++chunk)
    {
        static_cast<void>(write_pattern(from.streams(), stream, written, written + 500U, stream, false));
        pair.net->advance(pair.net->now + 20U * kMs);
        read_some(to, stream, read);
    }
}
} // namespace

TEST_CASE("QUIC connection: by the lowest RTT an end's PATH_ACKs for a slower path ride its faster one, so its peer "
          "measures that path's delay toward it plus the shorter return, and their ranges retire; taking turns, each "
          "path acknowledges itself",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 sections 5.4 and 5.5. Path ID 1 is 30 ms slower each way (50 ms against 20 ms) and carries the
    // client's stream pinned to it; the server, which only acknowledges, places the PATH_ACKs by its path choice.
    for (const QuicPathChoice choice : {QuicPathChoice::LowestRtt, QuicPathChoice::Turns})
    {
        const bool shortest = choice == QuicPathChoice::LowestRtt;
        INFO((shortest ? "LowestRtt" : "Turns"));
        MultipathPair pair;
        pair.net->socket_delay_ns[1] = 30U * kMs;
        pair.open_second();
        pair.net->advance(pair.net->now + 3000U * kMs); // the path MTU searches end
        REQUIRE(pair.server.set_path_choice(choice)); // live: Turns takes the PATH_ACKs back to their own paths
        u64 stream = 0U;
        REQUIRE(open_uni(pair, stream));
        REQUIRE(pair.client.streams().pin(stream, 1U) == QuicStreamError::None);
        const u64 server_sent = pair.server.next_packet_number(QuicSpace::Application, 1U);
        u64 written = 0U;
        u64 read = 0U;
        chunks_over(pair, pair.client, pair.server, stream, 100U, written, read); // two seconds
        CHECK(read + 1500U >= written); // 50 ms on the way
        const u64 smoothed = pair.client.recovery().rtt(1U).smoothed_ns;
        CHECK(smoothed >= (shortest ? 70U : 95U) * kMs); // switched live, Turns' estimate still converging
        CHECK(smoothed <= (shortest ? 75U : 105U) * kMs);
        CHECK((pair.server.next_packet_number(QuicSpace::Application, 1U) == server_sent) == shortest);
        // Acknowledged on path ID 0, the PATH_ACKs retire path ID 1's ranges (RFC 9000 section 13.2.4).
        const ConstSpan<QuicAckRange> ranges = pair.server.recovery().received(QuicSpace::Application, 1U).ranges();
        REQUIRE(ranges.size() == 1U);
        CHECK(ranges[0].smallest + 32U >= ranges[0].largest);
        CHECK(pair.server.counters().ack_path_moves == 0U);
        if (shortest)
        {
            // The client marks path ID 0 backup (section 3.3): the server's PATH_ACKs leave it, once.
            REQUIRE(pair.client.set_path_status(0U, true) == QuicPathStatusChange::Set);
            REQUIRE(pair.net->run_until([&] { return pair.server.peer_path_status(0U) == QuicPathStatus::Backup; }));
            chunks_over(pair, pair.client, pair.server, stream, 100U, written, read);
            CHECK(pair.client.recovery().rtt(1U).smoothed_ns >= 95U * kMs);
            CHECK(pair.server.next_packet_number(QuicSpace::Application, 1U) > server_sent);
            CHECK(pair.server.counters().ack_path_moves == 1U);
        }
        CHECK(pair.client.counters().path_failures == 0U);
        CHECK(pair.server.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC connection: an end whose shortest path stops carrying its packets, one way or both, under a flood, a "
          "trickle or datagrams pinned to the other path, finds it by the PING it sends there each round trip, and its "
          "PATH_ACKs return to their own paths until it carries again",
          "[net][net-path][quic-connection][multipath]")
{
    // ACK-only packets arm no probe timeout. The server, which only acknowledges, carries both paths' PATH_ACKs on path
    // ID 0, the shorter; then path ID 0 stops carrying anything to the client, or anything at all. Its PING once a
    // round trip (RFC 9000 section 13.2.4) times out, so path ID 1 acknowledges itself, and path ID 0's packets too
    // while they still arrive: the client's acknowledgements wait one probe timeout of the server's, not kAckOnlyRun
    // ACK-only packets. Datagrams pinned to path ID 1 draw no credit frame, so there the PING alone finds it.
    for (usize run = 0U; run < 5U; ++run)
    {
        const bool both_ways = (run & 1U) != 0U;
        const bool trickle = run >= 2U;
        const bool pinned = run == 4U;
        INFO((both_ways ? "both ways" : "toward the client")
             << (pinned ? ", pinned datagrams" : (trickle ? ", a trickle" : ", a flood")));
        MultipathPair pair(2U, 2U, false, true, 1024U, true);
        pair.net->socket_delay_ns[1] = 10U * kMs;
        pair.open_second();
        QuicConnection& client = pair.client;
        QuicConnection& server = pair.server;
        u64 stream = 0U;
        REQUIRE(open_uni(pair, stream));
        u64 written = 0U;
        u64 read = 0U;
        u64 dark_ns = kNeverNs;
        u64 moved_ns = kNeverNs;
        usize queued = 0U;
        usize arrived = 0U;
        // One unit 20 ms apart: 500 bytes of the stream, or a datagram pinned to path ID 1.
        const auto unit = [&]
        {
            if (!pinned)
            {
                chunks_over(pair, client, server, stream, 1U, written, read);
                return;
            }
            QuicDatagramSend how;
            how.pinned = true;
            how.path_id = 1U;
            const u8 payload[200] = {};
            u64 number = 0U;
            REQUIRE(client.datagrams().queue(ConstSpan<u8>(payload, sizeof(payload)), pair.net->now, number, how) ==
                    QuicDatagramError::None);
            ++queued;
            pair.net->advance(pair.net->now + 20U * kMs);
        };
        // What arrived: the stream read, the datagrams counted and their outcomes taken.
        const auto take = [&]
        {
            read_some(server, stream, read);
            ConstSpan<u8> held;
            for (; server.datagrams().peek(held); server.datagrams().pop())
            {
                ++arrived;
            }
            QuicDatagramReport report;
            while (client.datagrams().poll(report))
            {
            }
        };
        const auto watch = [&]
        {
            moved_ns = (moved_ns == kNeverNs && server.counters().ack_path_moves != 0U) ? pair.net->now : moved_ns;
        };
        if (trickle)
        {
            for (usize count = 0U; count < 50U; ++count) // a second
            {
                unit();
                take();
            }
            (both_ways ? pair.net->down : pair.net->down_in) = 1U << 0U; // the client's socket 0
            dark_ns = pair.net->now;
            for (usize count = 0U; count < 25U; ++count)
            {
                unit();
                take();
                watch();
            }
        }
        else
        {
            constexpr u64 total = 1024U * 1024U;
            REQUIRE(pair.net->run_until(
                [&]
                {
                    static_cast<void>(write_pattern(client.streams(), stream, written, total, stream, false));
                    if (server.streams().recv_state(stream) != QuicRecvState::None)
                    {
                        static_cast<void>(drain(server.streams(), stream, read, stream));
                    }
                    if (dark_ns == kNeverNs && written > total / 4U)
                    {
                        (both_ways ? pair.net->down : pair.net->down_in) = 1U << 0U;
                        dark_ns = pair.net->now;
                    }
                    watch();
                    return read == total;
                },
                20000U * kMs));
            CHECK(pair.net->now - dark_ns < 4000U * kMs);
        }
        // Everything sent arrives while path ID 0 still carries nothing back.
        REQUIRE(pair.net->run_until(
            [&]
            {
                take();
                return read == written && arrived == queued;
            },
            4000U * kMs));
        REQUIRE(moved_ns != kNeverNs);
        INFO("moved after " << (moved_ns - dark_ns) / kMs << " ms");
        CHECK(moved_ns - dark_ns < (trickle ? 250U : 400U) * kMs);
        CHECK(server.recovery().pto_count(0U) != 0U);
        // Path ID 0 carries again: the server's probe there is acknowledged and, with data to acknowledge, its
        // PATH_ACKs go back, path ID 0 being an eighth shorter.
        pair.net->down = 0U;
        pair.net->down_in = 0U;
        for (usize count = 0U; count < 250U; ++count) // five seconds
        {
            unit();
            take();
        }
        REQUIRE(pair.net->run_until(
            [&]
            {
                take();
                return read == written && arrived == queued;
            }));
        CHECK(server.recovery().pto_count(0U) == 0U);
        CHECK(server.counters().ack_path_moves == 2U);
        CHECK(client.counters().path_failures == 0U);
        CHECK(client.state() == QuicConnectionState::Established);
        CHECK(server.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC connection: PATH_ACKs stay on one of two paths of the same RTT under jitter while data floods both "
          "ways",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 5.5: "The estimates will converge faster if the scheduling strategy of PATH_ACK frames is
    // stable". Both ends send and place acknowledgements; their estimates of the two paths cross all the time.
    MultipathPair pair;
    for (Path& path : pair.net->path)
    {
        path.jitter_ns = 6U * kMs;
    }
    pair.open_second();
    QuicConnection& client = pair.client;
    QuicConnection& server = pair.server;
    u64 up = 0U;
    u64 down = 0U;
    REQUIRE(client.streams().open(false, up) == QuicStreamError::None);
    REQUIRE(server.streams().open(false, down) == QuicStreamError::None);
    constexpr u64 total = 4U * 1024U * 1024U;
    u64 written[2] = {};
    u64 read[2] = {};
    REQUIRE(pair.net->run_until(
        [&]
        {
            static_cast<void>(write_pattern(client.streams(), up, written[0], total, up));
            static_cast<void>(write_pattern(server.streams(), down, written[1], total, down));
            const bool up_read =
                server.streams().recv_state(up) != QuicRecvState::None && drain(server.streams(), up, read[0], up);
            const bool down_read = client.streams().recv_state(down) != QuicRecvState::None &&
                                   drain(client.streams(), down, read[1], down);
            return up_read && down_read;
        },
        60000U * kMs));
    INFO("moves " << client.counters().ack_path_moves << " and " << server.counters().ack_path_moves);
    CHECK(client.counters().ack_path_moves <= 1U);
    CHECK(server.counters().ack_path_moves <= 1U);
    CHECK(client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: this side's PATH_STATUS frames are numbered per path ID, go again when lost only while "
          "newest, and are refused, changing nothing, outside an open path ID",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 sections 3.3 and 4.3: "monotonically increasing values MUST be used" per path ID; a lost frame is
    // sent again only while it carries the newest status; any path ID under the peer's limit may be named.
    Identity identity;
    {
        RawRun plain(identity);
        CHECK(plain.server.set_path_status(0U, true) == QuicPathStatusChange::NotEstablished);
        plain.handshake();
        CHECK(plain.server.set_path_status(0U, true) == QuicPathStatusChange::NoMultipath);
        CHECK(plain.server.path_status(0U) == QuicPathStatus::Unknown);
    }
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    run.max_path_id = 2U;
    run.handshake();
    REQUIRE(run.server.multipath());
    const auto sent = [&](usize index, u64 path_id, u64 sequence, bool backup)
    {
        REQUIRE(index < run.raw.statuses);
        const QuicFrame& frame = run.raw.status[index];
        CHECK(frame.path_id == path_id);
        CHECK(frame.sequence == sequence);
        CHECK(frame.backup == backup);
    };
    // Path ID 3 is open at the server but above the raw client's limit, path ID 4 above both.
    CHECK(run.server.set_path_status(3U, true) == QuicPathStatusChange::NotOpen);
    CHECK(run.server.set_path_status(4U, true) == QuicPathStatusChange::NotOpen);
    CHECK(run.server.path_status(3U) == QuicPathStatus::Unknown);
    run.drain();
    CHECK(run.raw.statuses == 0U);
    // Path ID 1, not yet used: numbered from 0; the status in force again sends nothing; a change numbers on.
    REQUIRE(run.server.set_path_status(1U, true) == QuicPathStatusChange::Set);
    REQUIRE(run.server.set_path_status(1U, true) == QuicPathStatusChange::Set);
    run.drain();
    REQUIRE(run.raw.statuses == 1U);
    sent(0U, 1U, 0U, true);
    REQUIRE(run.server.set_path_status(1U, false) == QuicPathStatusChange::Set);
    REQUIRE(run.server.set_path_status(0U, true) == QuicPathStatusChange::Set);
    run.drain();
    REQUIRE(run.raw.statuses == 3U);
    sent(1U, 1U, 1U, false);
    sent(2U, 0U, 0U, true);
    // The peer's statuses are apart from this side's.
    send_frame(run, path_frame(QuicFrameKind::PathStatus, 1U, 7U));
    CHECK(run.server.peer_path_status(1U) == QuicPathStatus::Available);
    CHECK(run.server.path_status(1U) == QuicPathStatus::Available);
    CHECK(run.server.path_status(0U) == QuicPathStatus::Backup);
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    // Lost while newest: the raw client acknowledges only the probe after it, and the frame goes again.
    REQUIRE(run.server.set_path_status(1U, true) == QuicPathStatusChange::Set);
    run.drain();
    REQUIRE(run.raw.statuses == 4U);
    sent(3U, 1U, 2U, true);
    run.raw.seen[run.raw.status_packet] = false;
    run_timers(run, run.now + 200U * kMs);
    CHECK(run.raw.statuses == 4U); // a probe carries no copy
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    REQUIRE(run.raw.statuses == 5U);
    sent(4U, 1U, 2U, true);
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    // Lost after a newer one went: never again.
    REQUIRE(run.server.set_path_status(1U, false) == QuicPathStatusChange::Set);
    run.drain();
    REQUIRE(run.raw.statuses == 6U);
    sent(5U, 1U, 3U, false);
    run.raw.seen[run.raw.status_packet] = false;
    REQUIRE(run.server.set_path_status(1U, true) == QuicPathStatusChange::Set);
    run.drain();
    REQUIRE(run.raw.statuses == 7U);
    sent(6U, 1U, 4U, true);
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    run_timers(run, run.now + 200U * kMs);
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    run.drain();
    CHECK(run.raw.statuses == 7U);
    // An abandoned path ID: refused, and both ends' statuses for it forgotten once it closes.
    send_frame(run, path_frame(QuicFrameKind::PathAbandon, 1U));
    CHECK(run.server.set_path_status(1U, false) == QuicPathStatusChange::NotOpen);
    CHECK(run.server.path_status(1U) == QuicPathStatus::Backup);
    run.send(QuicPacketType::OneRtt, [&](Span<u8> out) { return run.ack_received(out); });
    const QuicPathAbandon* const entry = run.server.abandon_of(1U);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->deadline_ns != kNeverNs);
    run_timers(run, entry->deadline_ns);
    CHECK(run.server.counters().paths_closed == 1U);
    CHECK(run.server.path_status(1U) == QuicPathStatus::Unknown);
    CHECK(run.server.peer_path_status(1U) == QuicPathStatus::Unknown);
    CHECK(run.server.path_status(0U) == QuicPathStatus::Backup);
    CHECK(run.raw.statuses == 7U);
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a client takes path IDs in rising order up to both limits and reports the server's limit "
          "alone in a PATHS_BLOCKED frame, sent again when lost",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 sections 3.2.1 and 4.7: PATHS_BLOCKED carries the server's limit, the one that held the path back.
    {
        MultipathPair pair(3U, 1U);
        DropFirst lose(0U, 1U);
        lose.armed = false;
        pair.net->filter = &lose;
        pair.confirm();
        REQUIRE(pair.net->run_until([&] { return pair.client.peer_cid_count(1U) != 0U; }));
        u64 path_id = 0U;
        REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
        CHECK(path_id == 1U);
        REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
        pair.net->advance(pair.net->now + 500U * kMs);
        const usize dropped = pair.net->dropped[0];
        lose.armed = true; // the datagram carrying the first PATHS_BLOCKED is lost
        CHECK(pair.client.add_path(QuicPath{2U, kServerAt}, pair.net->now, path_id) == QuicMigration::NoPathId);
        CHECK(path_id == 0U);
        REQUIRE(pair.net->run_until([&] { return pair.server.counters().paths_blocked != 0U; }));
        CHECK(pair.net->dropped[0] == dropped + 1U);
        pair.net->advance(pair.net->now + 2000U * kMs);
        CHECK(pair.server.counters().paths_blocked == 1U); // acknowledged, so never repeated
        CHECK(pair.client.counters().paths_opened == 1U);
        CHECK(pair.server.state() == QuicConnectionState::Established);
    }
    {
        // Both limits 2: path IDs 1 and 2 open from two sockets, and the client's own limit is never reported.
        MultipathPair pair(2U, 2U);
        pair.confirm();
        REQUIRE(pair.net->run_until([&] { return pair.client.peer_cid_count(2U) != 0U; }));
        u64 path_id = 0U;
        REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
        CHECK(path_id == 1U);
        REQUIRE(pair.client.add_path(QuicPath{2U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
        CHECK(path_id == 2U);
        REQUIRE(pair.net->run_until([&] { return pair.open_both(1U) && pair.open_both(2U); }));
        const QuicPaths& third = pair.server.paths(2U);
        CHECK(third.path(third.active()).peer == kThirdAt);
        CHECK(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::NoPathId);
        pair.net->advance(pair.net->now + 2000U * kMs);
        CHECK(pair.server.counters().paths_blocked == 0U);
        CHECK(pair.server.counters().paths_opened == 2U);
        CHECK(pair.client.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC connection: a client without the server's IDs for its next path ID sends PATH_CIDS_BLOCKED and opens "
          "the path once they come",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 4.7: the Next Sequence Number is the next one the client expects, 0 before any. The first
    // frame is lost and sent again, as the server still issued none.
    MultipathPair pair;
    DropFirst lose(0U, 1U);
    lose.armed = false;
    pair.net->filter = &lose;
    pair.server_host.refuse = true; // the server issues no ID past path ID 0's first
    pair.confirm();
    CHECK(pair.client.peer_cid_count(1U) == 0U);
    u64 path_id = 0U;
    const usize dropped = pair.net->dropped[0];
    lose.armed = true;
    CHECK(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::NoConnectionId);
    CHECK(path_id == 0U);
    CHECK(pair.client.counters().paths_opened == 0U);
    REQUIRE(pair.net->run_until([&] { return pair.server.counters().paths_blocked == 1U; }));
    CHECK(pair.net->dropped[0] == dropped + 1U);
    pair.net->advance(pair.net->now + 2000U * kMs);
    CHECK(pair.server.counters().paths_blocked == 1U);
    CHECK(pair.server.state() == QuicConnectionState::Established);
    pair.server_host.refuse = false;
    pair.server.ping();
    REQUIRE(pair.net->run_until([&] { return pair.client.peer_cid_count(1U) != 0U; }));
    REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
    CHECK(path_id == 1U);
    REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
    CHECK(pair.server.counters().paths_blocked == 1U);
}

TEST_CASE("QUIC connection: a client that issued no ID of its own for a path ID does not open it and owes no frame",
          "[net][net-path][quic-connection][multipath]")
{
    // The server could not answer there (RFC 10069 section 3.1); its own IDs are not what holds the path back.
    MultipathPair pair;
    pair.client_host.refuse = true;
    pair.confirm();
    REQUIRE(pair.net->run_until([&] { return pair.client.peer_cid_count(2U) != 0U; }));
    CHECK(pair.client.local_cid_count(1U) == 0U);
    u64 path_id = 9U;
    CHECK(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::NoConnectionId);
    CHECK(path_id == 0U);
    pair.net->advance(pair.net->now + 1000U * kMs);
    CHECK(pair.server.counters().paths_blocked == 0U);
    CHECK(pair.client.counters().paths_opened == 0U);
    CHECK(pair.server.counters().paths_opened == 0U);
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a client's path open is refused, changing nothing, before confirmation, without "
          "multipath, toward another server or the handshake address the server closed to new paths",
          "[net][net-path][quic-connection][multipath]")
{
    const QuicPath second{1U, kServerAt};
    u64 path_id = 9U;
    {
        MultipathPair pair;
        CHECK(pair.client.add_path(second, pair.net->now, path_id) == QuicMigration::NotConfirmed);
        CHECK(path_id == 0U);
        pair.confirm();
        REQUIRE(pair.net->run_until([&] { return pair.client.peer_cid_count(1U) != 0U; }));
        CHECK(pair.server.add_path(QuicPath{0U, pair.net->client_at}, pair.net->now, path_id) ==
              QuicMigration::NotClient);
        CHECK(pair.client.add_path(QuicPath{1U, Endpoint{IpAddress::v4(203, 0, 113, 99), 443U}}, pair.net->now,
                                   path_id) == QuicMigration::OtherServer);
        CHECK(pair.client.counters().paths_opened == 0U);
        CHECK_FALSE(pair.client.paths(1U).used(0U));
        // Two path IDs may share a 4-tuple (section 5.2): the handshake path's own opens path ID 1.
        CHECK(pair.client.add_path(QuicPath{0U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
        CHECK(path_id == 1U);
        REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
        CHECK(pair.server.paths(1U).path(pair.server.paths(1U).active()) == pair.server.path());
    }
    {
        MultipathPair pair(2U, 2U, true); // disable_active_migration: no new path to the handshake address (2.2)
        pair.confirm();
        CHECK(pair.client.add_path(second, pair.net->now, path_id) == QuicMigration::Disabled);
        CHECK(pair.client.counters().paths_opened == 0U);
    }
    {
        MultipathPair pair(2U, 2U, false, false); // the server does not offer it
        pair.confirm();
        CHECK_FALSE(pair.client.multipath());
        CHECK(pair.client.add_path(second, pair.net->now, path_id) == QuicMigration::NoMultipath);
    }
    {
        MultipathPair pair(0U, 2U); // negotiated, but the client keeps path ID 0 alone
        pair.confirm();
        CHECK(pair.client.multipath());
        CHECK(pair.client.add_path(second, pair.net->now, path_id) == QuicMigration::NoMultipath);
        pair.net->advance(pair.net->now + 500U * kMs);
        CHECK(pair.server.counters().paths_blocked == 0U);
    }
}

TEST_CASE("QUIC connection: a client's further path carries no data before its server answers, nor after its "
          "validation failed",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 3.1: "If validation fails, the client MUST NOT use the path." Socket 1's network carries
    // nothing, so path ID 1's space numbers its PATH_CHALLENGE packets and, once they are outstanding, its probe
    // timeouts' PINGs alone (each twice as late as the one before); the streams finish on path ID 0.
    MultipathPair pair;
    pair.confirm();
    REQUIRE(pair.net->run_until([&] { return pair.client.peer_cid_count(1U) != 0U; }));
    pair.net->down = 1U << 1U;
    u64 path_id = 0U;
    REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
    pair.stream_to_server(48U * 1024U);
    REQUIRE(pair.net->run_until([&] { return pair.client.counters().path_failures == 1U; }, 600000U * kMs));
    const u64 challenges = pair.client.next_packet_number(QuicSpace::Application, 1U);
    CHECK(challenges != 0U);
    CHECK(challenges <= 6U);
    const QuicPaths& mine = pair.client.paths(1U);
    CHECK_FALSE(mine.proven(mine.active()));
    CHECK_FALSE(mine.validating(mine.active()));
    // The failure abandons path ID 1 ("the endpoint MUST explicitly close the path"): its slot sends nothing more,
    // the server, which never opened it, answers on path ID 0, and both close it.
    const QuicPathAbandon* const abandon = pair.client.abandon_of(1U);
    REQUIRE(abandon != nullptr);
    CHECK(abandon->error_code == code(QuicTransportError::PathUnstableOrPoor));
    CHECK_FALSE(pair.client.recovery().path_sending(1U));
    REQUIRE(pair.net->run_until([&]
                                {
                                    CHECK(pair.client.next_packet_number(QuicSpace::Application, 1U) ==
                                          (pair.client.recovery().path_open(1U) ? challenges : 0U));
                                    return pair.client.counters().paths_closed == 1U &&
                                           pair.server.counters().paths_closed == 1U;
                                }));
    pair.stream_to_server(48U * 1024U);
    CHECK_FALSE(pair.client.recovery().path_open(1U));
    CHECK(pair.server.local_cid_count(1U) == 0U);
    CHECK(pair.client.local_cid_count(1U) == 0U);
    CHECK(pair.server.counters().paths_opened == 0U);
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: either end abandons path ID 1 over links that lose, duplicate and jitter; both send "
          "PATH_ABANDON on path ID 0, its data in flight finishes there, and both close it three PTOs later",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 3.4: the abandoning end retires the peer's IDs for the path at once and sends no packet there
    // again; the receiver answers with its own PATH_ABANDON; each keeps its issued IDs and the path's received state
    // three PTOs, then frees both. Packets in flight there go back to the queues (section 3.4.3).
    for (u64 seed = 1U; seed <= 4U; ++seed)
    {
        const bool by_client = (seed % 2U) != 0U;
        INFO("seed " << seed << (by_client ? ", the client abandons" : ", the server abandons"));
        MultipathPair pair;
        pair.net->rng.state = seed;
        for (Path& path : pair.net->path)
        {
            path.loss = 40U;
            path.duplicate = 30U;
            path.jitter_ns = 4U * kMs;
        }
        pair.confirm();
        REQUIRE(pair.net->run_until(
            [&] { return pair.client.peer_cid_count(1U) != 0U && pair.server.peer_cid_count(1U) != 0U; }));
        u64 path_id = 0U;
        REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
        REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
        QuicConnection& abandoning = by_client ? pair.client : pair.server;
        QuicConnection& answering = by_client ? pair.server : pair.client;
        u64 stream = 0U;
        REQUIRE(pair.client.streams().open(true, stream) == QuicStreamError::None);
        constexpr u64 total = 160U * 1024U;
        u64 written = 0U;
        u64 read = 0U;
        const auto flow = [&]
        {
            static_cast<void>(write_pattern(pair.client.streams(), stream, written, total, stream));
            return pair.server.streams().recv_state(stream) != QuicRecvState::None &&
                   drain(pair.server.streams(), stream, read, stream);
        };
        REQUIRE(pair.net->run_until(
            [&]
            {
                static_cast<void>(flow());
                return pair.client.recovery().bytes_in_flight(1U) != 0U;
            }));
        const u64 error = code(QuicTransportError::ApplicationAbandonPath);
        CHECK(abandoning.abandon_path(1U, error, pair.net->now) == QuicAbandon::Abandoned);
        CHECK(abandoning.abandon_path(1U, error, pair.net->now) == QuicAbandon::NotOpen); // once
        CHECK(abandoning.peer_cid_count(1U) == 0U);
        CHECK_FALSE(abandoning.recovery().path_sending(1U));
        CHECK(abandoning.recovery().bytes_in_flight(1U) == 0U);
        const u64 last = abandoning.next_packet_number(QuicSpace::Application, 1U);
        u64 answered_error = ~u64{0};
        u64 peer_error = ~u64{0};
        REQUIRE(pair.net->run_until(
            [&]
            {
                static_cast<void>(flow());
                // Nothing goes on the abandoned path again, from the end that abandoned it or the one that answered.
                const QuicPathAbandon* const own = abandoning.abandon_of(1U);
                if (own != nullptr)
                {
                    CHECK(abandoning.next_packet_number(QuicSpace::Application, 1U) == last);
                    answered_error = own->peer_error_code;
                }
                const QuicPathAbandon* const theirs = answering.abandon_of(1U);
                if (theirs != nullptr)
                {
                    CHECK_FALSE(answering.recovery().path_sending(1U));
                    peer_error = theirs->peer_error_code;
                }
                return pair.client.counters().paths_closed == 1U && pair.server.counters().paths_closed == 1U;
            },
            600000U * kMs));
        CHECK(peer_error == error);
        CHECK(answered_error == code(QuicTransportError::NoError));
        REQUIRE(pair.net->run_until(flow, 600000U * kMs));
        CHECK(read == total);
        for (QuicConnection* end : {&pair.client, &pair.server})
        {
            CHECK(end->counters().paths_abandoned == 1U);
            CHECK(end->abandon_of(1U) == nullptr);
            CHECK_FALSE(end->recovery().path_open(1U));
            CHECK(end->local_cid_count(1U) == 0U);
            CHECK(end->peer_cid_count(1U) == 0U);
            CHECK(end->state() == QuicConnectionState::Established);
        }
        // Path ID 1 is consumed: the client cannot open it again (section 3.4); path ID 2 takes the freed slot, its
        // packets numbered from 0.
        REQUIRE(pair.net->run_until([&] { return pair.client.peer_cid_count(2U) != 0U; }));
        REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
        CHECK(path_id == 2U);
        CHECK(pair.client.next_packet_number(QuicSpace::Application, 1U) <= 2U);
        REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
        pair.stream_to_server(32U * 1024U);
        CHECK(pair.server.next_packet_number(QuicSpace::Application, 1U) != 0U);
    }
}

TEST_CASE("QUIC connection: a lost PATH_ABANDON goes again, and a path ID never used is abandoned and closed",
          "[net][net-path][quic-connection][multipath]")
{
    // Sections 3.4 and 4.2: PATH_ABANDON is ack-eliciting and repeated when lost; abandoning an unused path ID, to
    // remove a hole from the path IDs in use, is not an error, and the receiver answers it all the same.
    MultipathPair pair;
    DropFirst lose(0U, 1U);
    lose.armed = false;
    pair.net->filter = &lose;
    pair.confirm();
    REQUIRE(pair.net->run_until([&] { return pair.client.peer_cid_count(2U) != 0U; }));
    const usize dropped = pair.net->dropped[0];
    lose.armed = true; // the client's next datagram, the one carrying its PATH_ABANDON, is lost
    CHECK(pair.client.abandon_path(2U, code(QuicTransportError::NoError), pair.net->now) == QuicAbandon::Abandoned);
    CHECK(pair.client.peer_cid_count(2U) == 0U);
    REQUIRE(pair.net->run_until(
        [&] { return pair.client.counters().paths_closed == 1U && pair.server.counters().paths_closed == 1U; }));
    CHECK(pair.net->dropped[0] == dropped + 1U);
    CHECK(pair.server.counters().paths_abandoned == 1U);
    CHECK(pair.server.local_cid_count(2U) == 0U);
    CHECK(pair.client.local_cid_count(2U) == 0U);
    // The hole removed, path ID 1 still opens, and path ID 3, which the close let in, opens beside it; then the
    // limits stand at three path IDs open.
    u64 path_id = 0U;
    REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
    CHECK(path_id == 1U);
    REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
    for (const QuicConnection* end : {&pair.client, &pair.server})
    {
        CHECK(end->local_max_path_id() == 3U);
        CHECK(end->peer_max_path_id() == 3U);
    }
    REQUIRE(pair.net->run_until([&] { return pair.client.peer_cid_count(3U) != 0U; }));
    REQUIRE(pair.client.add_path(QuicPath{2U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
    CHECK(path_id == 3U);
    REQUIRE(pair.net->run_until([&] { return pair.open_both(2U); }));
    CHECK(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::NoPathId);
}

namespace
{
// `end`'s path IDs up to its limit after `closed` closed: none at or below it holds an ID but path ID 0, three hold
// some, and `host` routes exactly those IDs and watches at most three tokens.
void holds_three(const QuicConnection& end, const Host& host, u64 closed)
{
    usize routed = 0U;
    usize open = 0U;
    for (u64 path_id = 0U; path_id <= end.local_max_path_id(); ++path_id)
    {
        const usize count = end.local_cid_count(path_id);
        CHECK((path_id == 0U || path_id > closed || count == 0U));
        routed += count;
        open += (count != 0U) ? 1U : 0U;
    }
    CHECK(open == 3U);
    CHECK(host.routes.size() == routed);
    CHECK(host.watches - host.unwatches <= 3U);
}
} // namespace

TEST_CASE("QUIC connection: path IDs opened and abandoned twenty times over a hostile link raise both limits past "
          "kQuicMaxPaths, three path IDs open throughout",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 sections 3.2.1, 3.4 and 4.6: a closed path ID is never reused, so MAX_PATH_ID lets a new one in; the
    // ID spaces, routes and slots a connection holds stay those of 1 + initial_max_path_id path IDs.
    MultipathPair pair;
    pair.net->rng.state = 7U;
    for (Path& path : pair.net->path)
    {
        path.loss = 20U;
        path.duplicate = 20U;
        path.jitter_ns = 3U * kMs;
    }
    pair.confirm();
    for (u64 cycle = 0U; cycle < 20U; ++cycle)
    {
        INFO("cycle " << cycle);
        // Path IDs open in rising order: each cycle's is the smallest still open, 1 + its cycle.
        u64 path_id = 0U;
        REQUIRE(pair.net->run_until(
            [&]
            {
                return pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started;
            }));
        CHECK(path_id == cycle + 1U);
        REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
        QuicConnection& abandoning = ((cycle % 2U) == 0U) ? pair.client : pair.server;
        REQUIRE(abandoning.abandon_path(path_id, code(QuicTransportError::NoError), pair.net->now) ==
                QuicAbandon::Abandoned);
        REQUIRE(pair.net->run_until(
            [&]
            {
                return pair.client.counters().paths_closed == cycle + 1U &&
                       pair.server.counters().paths_closed == cycle + 1U &&
                       pair.client.peer_max_path_id() == cycle + 3U && pair.server.peer_max_path_id() == cycle + 3U;
            },
            600000U * kMs));
        for (const QuicConnection* end : {&pair.client, &pair.server})
        {
            CHECK(end->local_max_path_id() == cycle + 3U);
            CHECK(end->state() == QuicConnectionState::Established);
        }
        // The raised path ID's IDs go out; every path ID up to the limit is then open or closed for good, three are
        // open, and the host routes exactly the IDs of those and watches the tokens of the peer's IDs sent to.
        REQUIRE(pair.net->run_until(
            [&]
            {
                return pair.client.local_cid_count(cycle + 3U) != 0U && pair.server.local_cid_count(cycle + 3U) != 0U;
            }));
        holds_three(pair.client, pair.client_host, path_id);
        holds_three(pair.server, pair.server_host, path_id);
    }
    CHECK(pair.client.local_max_path_id() > kQuicMaxPaths);
    CHECK(pair.client.counters().paths_opened == 20U);
    CHECK(pair.server.counters().paths_opened == 20U);
    // The last open path ID still carries data on its own path, numbered from 0 in its slot.
    u64 path_id = 0U;
    REQUIRE(pair.net->run_until(
        [&]
        { return pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started; }));
    CHECK(path_id == 21U);
    REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
    pair.stream_to_server(32U * 1024U);
    CHECK(pair.server.next_packet_number(QuicSpace::Application, 1U) != 0U);
}

TEST_CASE("QUIC connection: a client's PATHS_BLOCKED owed at the server's old limit is not sent once MAX_PATH_ID "
          "raises it, and the path then opens",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 4.7: PATHS_BLOCKED reports the limit that prevented a path; one raised before the frame went
    // prevents nothing.
    MultipathPair pair(3U, 1U);
    pair.confirm();
    REQUIRE(pair.net->run_until([&] { return pair.client.peer_cid_count(1U) != 0U; }));
    u64 path_id = 0U;
    REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
    REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
    REQUIRE(pair.server.abandon_path(1U, code(QuicTransportError::NoError), pair.net->now) == QuicAbandon::Abandoned);
    // The server's close raises its limit; its MAX_PATH_ID is owed, not yet sent.
    REQUIRE(pair.net->run_until([&] { return pair.server.local_max_path_id() == 2U; }, 600000U * kMs));
    REQUIRE(pair.client.peer_max_path_id() == 1U);
    pair.net->flush();
    // The client, still at the old limit, owes PATHS_BLOCKED; the MAX_PATH_ID arrives before it sends.
    CHECK(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::NoPathId);
    pair.net->run_at(pair.net->now + 25U * kMs);
    REQUIRE(pair.client.peer_max_path_id() == 2U);
    const u64 blocked = pair.server.counters().paths_blocked;
    REQUIRE(pair.net->run_until(
        [&] { return pair.client.peer_cid_count(2U) != 0U && pair.client.counters().paths_closed == 1U; },
        600000U * kMs));
    pair.net->advance(pair.net->now + 300U * kMs);
    CHECK(pair.server.counters().paths_blocked == blocked);
    REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
    CHECK(path_id == 2U);
    REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
}

TEST_CASE("QUIC connection: abandon_path is refused, changing nothing, before the handshake, without multipath, for "
          "the last path still sending, for a path ID not open and for an error code past 2^62 - 1",
          "[net][net-path][quic-connection][multipath]")
{
    {
        MultipathPair pair;
        CHECK(pair.client.abandon_path(1U, 0U, pair.net->now) == QuicAbandon::NotEstablished);
        pair.confirm();
        CHECK(pair.client.abandon_path(0U, 0U, pair.net->now) == QuicAbandon::LastPath); // path ID 0 alone sends
        CHECK(pair.server.abandon_path(0U, 0U, pair.net->now) == QuicAbandon::LastPath);
        CHECK(pair.client.abandon_path(3U, 0U, pair.net->now) == QuicAbandon::NotOpen); // above the limit of 2
        CHECK(pair.client.abandon_path(1U, kQuicMaxVarint + 1U, pair.net->now) == QuicAbandon::BadError);
        CHECK(pair.client.abandon_of(1U) == nullptr);
        CHECK(pair.client.counters().paths_abandoned == 0U);
        pair.net->advance(pair.net->now + 500U * kMs);
        CHECK(pair.server.counters().paths_abandoned == 0U);
        CHECK(pair.client.state() == QuicConnectionState::Established);
    }
    {
        MultipathPair pair(2U, 2U, false, false); // the server does not offer it
        pair.confirm();
        CHECK(pair.client.abandon_path(1U, 0U, pair.net->now) == QuicAbandon::NoMultipath);
    }
    {
        // A path ID under this side's limit and above the peer's: no frame may name it there (section 4).
        MultipathPair pair(1U, 3U);
        pair.confirm();
        CHECK(pair.server.abandon_path(2U, 0U, pair.net->now) == QuicAbandon::NotOpen);
        CHECK(pair.server.abandon_path(1U, 0U, pair.net->now) == QuicAbandon::Abandoned);
        REQUIRE(pair.net->run_until(
            [&] { return pair.client.counters().paths_closed == 1U && pair.server.counters().paths_closed == 1U; }));
        CHECK(pair.client.state() == QuicConnectionState::Established);
    }
    {
        MultipathPair pair(3U, 1U);
        pair.confirm();
        CHECK(pair.client.abandon_path(2U, 0U, pair.net->now) == QuicAbandon::NotOpen);
        CHECK(pair.server.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC connection: either end abandons path ID 0 while path ID 1 lives, over links that lose, duplicate and "
          "jitter; path ID 1 carries the frames, the data and the close, and both close path ID 0 three PTOs later",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 3.4 and its Figure 5: path ID 0 is abandoned as any other, its PATH_ABANDON and the answer
    // going on path ID 1; its data in flight finishes there, and the lowest slot still sending leads.
    for (u64 seed = 1U; seed <= 4U; ++seed)
    {
        const bool by_client = (seed % 2U) != 0U;
        INFO("seed " << seed << (by_client ? ", the client abandons" : ", the server abandons"));
        MultipathPair pair;
        pair.net->rng.state = seed;
        for (Path& path : pair.net->path)
        {
            path.loss = 40U;
            path.duplicate = 30U;
            path.jitter_ns = 4U * kMs;
        }
        pair.confirm();
        REQUIRE(pair.net->run_until(
            [&] { return pair.client.peer_cid_count(1U) != 0U && pair.server.peer_cid_count(1U) != 0U; }));
        u64 path_id = 0U;
        REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
        REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
        QuicConnection& abandoning = by_client ? pair.client : pair.server;
        QuicConnection& answering = by_client ? pair.server : pair.client;
        u64 stream = 0U;
        REQUIRE(pair.client.streams().open(true, stream) == QuicStreamError::None);
        constexpr u64 total = 160U * 1024U;
        u64 written = 0U;
        u64 read = 0U;
        const auto flow = [&]
        {
            static_cast<void>(write_pattern(pair.client.streams(), stream, written, total, stream));
            return pair.server.streams().recv_state(stream) != QuicRecvState::None &&
                   drain(pair.server.streams(), stream, read, stream);
        };
        REQUIRE(pair.net->run_until(
            [&]
            {
                static_cast<void>(flow());
                return pair.client.recovery().bytes_in_flight(0U) != 0U;
            }));
        const u64 error = code(QuicTransportError::ApplicationAbandonPath);
        CHECK(abandoning.abandon_path(0U, error, pair.net->now) == QuicAbandon::Abandoned);
        CHECK(abandoning.abandon_path(0U, error, pair.net->now) == QuicAbandon::NotOpen); // once
        CHECK(abandoning.abandon_path(1U, error, pair.net->now) == QuicAbandon::LastPath);
        CHECK(abandoning.peer_cid_count(0U) == 0U);
        CHECK_FALSE(abandoning.recovery().path_sending(0U));
        CHECK(abandoning.recovery().bytes_in_flight(0U) == 0U);
        // Path ID 1 leads: its 4-tuple is path().
        CHECK(abandoning.path() == abandoning.paths(1U).path(abandoning.paths(1U).active()));
        const u64 last = abandoning.next_packet_number(QuicSpace::Application, 0U);
        u64 peer_error = ~u64{0};
        REQUIRE(pair.net->run_until(
            [&]
            {
                static_cast<void>(flow());
                if (abandoning.abandon_of(0U) != nullptr)
                {
                    CHECK(abandoning.next_packet_number(QuicSpace::Application, 0U) == last); // until it closes
                }
                const QuicPathAbandon* const theirs = answering.abandon_of(0U);
                if (theirs != nullptr)
                {
                    CHECK_FALSE(answering.recovery().path_sending(0U));
                    peer_error = theirs->peer_error_code;
                }
                return pair.client.counters().paths_closed == 1U && pair.server.counters().paths_closed == 1U;
            },
            600000U * kMs));
        CHECK(peer_error == error);
        REQUIRE(pair.net->run_until(flow, 600000U * kMs));
        CHECK(read == total);
        for (QuicConnection* end : {&pair.client, &pair.server})
        {
            CHECK(end->counters().paths_abandoned == 1U);
            CHECK(end->abandon_of(0U) == nullptr);
            CHECK_FALSE(end->recovery().path_open(0U));
            CHECK_FALSE(end->protection().path_open(0U));
            CHECK(end->local_cid_count(0U) == 0U);
            CHECK(end->peer_cid_count(0U) == 0U);
            CHECK(end->state() == QuicConnectionState::Established);
            CHECK(end->path() == end->paths(1U).path(end->paths(1U).active()));
            CHECK(end->abandon_path(1U, 0U, pair.net->now) == QuicAbandon::LastPath);
        }
        // Path ID 0's 4-tuples are what probe_path() and migrate() move: nothing once it is gone.
        CHECK(pair.client.migrate(QuicPath{2U, kServerAt}, pair.net->now) == QuicMigration::Abandoned);
        CHECK(pair.client.probe_path(QuicPath{2U, kServerAt}, pair.net->now) == QuicMigration::Abandoned);
        pair.stream_to_server(32U * 1024U);
        // The close goes on path ID 1, the leading path.
        const u8 reason[] = {0x62U, 0x79U, 0x65U};
        pair.client.close(0x2AU, ConstSpan<u8>(reason, sizeof(reason)), pair.net->now);
        REQUIRE(pair.net->run_until([&] { return pair.server.state() == QuicConnectionState::Draining; }));
        CHECK(pair.server.close_info().error_code == 0x2AU);
    }
}

TEST_CASE("QUIC connection: once path ID 0 closes its slot is reclaimed, so four path IDs stay open; path ID 4 opens "
          "in slot 0 on both ends, leads once validated and alone carries a stream over a hostile link",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 sections 3.2.1, 3.4 and 4.6: a closed path ID's slot holds the next one, path ID 0's as any other.
    const Endpoint fourth{IpAddress::v4(198, 18, 0, 10), 43000U}; // the client's socket 3
    for (u64 seed = 1U; seed <= 4U; ++seed)
    {
        const bool by_client = (seed % 2U) != 0U;
        INFO("seed " << seed << (by_client ? ", the client abandons" : ", the server abandons"));
        MultipathPair pair(3U, 3U);
        pair.net->rng.state = seed;
        pair.net->socket_at[2] = fourth;
        for (Path& path : pair.net->path)
        {
            path.loss = 20U;
            path.duplicate = 20U;
            path.jitter_ns = 3U * kMs;
        }
        pair.confirm();
        // Path IDs 1 to 3 take slots 1 to 3, one socket each: every slot is held.
        for (u64 next = 1U; next <= 3U; ++next)
        {
            REQUIRE(pair.net->run_until(
                [&] { return pair.client.peer_cid_count(next) != 0U && pair.server.peer_cid_count(next) != 0U; }));
            u64 path_id = 0U;
            REQUIRE(pair.client.add_path(QuicPath{static_cast<u32>(next), kServerAt}, pair.net->now, path_id) ==
                    QuicMigration::Started);
            CHECK(path_id == next);
            REQUIRE(pair.net->run_until([&] { return pair.open_both(static_cast<usize>(next)); }));
        }
        QuicConnection& abandoning = by_client ? pair.client : pair.server;
        const u64 error = code(QuicTransportError::ApplicationAbandonPath);
        REQUIRE(abandoning.abandon_path(0U, error, pair.net->now) == QuicAbandon::Abandoned);
        // Its close raises both limits to 4 although no further slot is spare: slot 0 is.
        REQUIRE(pair.net->run_until(
            [&]
            {
                return pair.client.counters().paths_closed == 1U && pair.server.counters().paths_closed == 1U &&
                       pair.client.peer_max_path_id() == 4U && pair.server.peer_max_path_id() == 4U &&
                       pair.client.peer_cid_count(4U) != 0U && pair.server.peer_cid_count(4U) != 0U;
            },
            600000U * kMs));
        for (const QuicConnection* end : {&pair.client, &pair.server})
        {
            CHECK(end->local_max_path_id() == 4U);
            CHECK_FALSE(end->protection().path_open(0U));
            CHECK_FALSE(end->recovery().path_open(0U));
        }
        // Path ID 4 goes to the server's address path ID 0 sent to, here from the 4-tuple path ID 0 left.
        u64 path_id = 0U;
        CHECK(pair.client.add_path(QuicPath{0U, kSecondAt}, pair.net->now, path_id) == QuicMigration::OtherServer);
        REQUIRE(pair.client.add_path(QuicPath{0U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
        CHECK(path_id == 4U);
        CHECK(pair.client.protection().path_id(0U) == 4U);
        // Still validating, it leads nothing: path ID 1 keeps path() and the close.
        CHECK(pair.client.path() == pair.client.paths(1U).path(pair.client.paths(1U).active()));
        REQUIRE(pair.net->run_until([&] { return pair.open_both(0U); }));
        CHECK(pair.server.protection().path_id(0U) == 4U);
        // Path ID 0's moves stay ended though slot 0 sends again.
        CHECK(pair.client.migrate(QuicPath{1U, kServerAt}, pair.net->now) == QuicMigration::Abandoned);
        CHECK(pair.client.probe_path(QuicPath{1U, kServerAt}, pair.net->now) == QuicMigration::Abandoned);
        // Path IDs 1 to 3 leave; path ID 4 alone carries the data, numbered from 0 in slot 0 and acknowledged in
        // PATH_ACK frames (an ACK frame's packets are path ID 0's, and nothing would acknowledge these).
        for (u64 leaving = 1U; leaving <= 3U; ++leaving)
        {
            REQUIRE(abandoning.abandon_path(leaving, error, pair.net->now) == QuicAbandon::Abandoned);
        }
        REQUIRE(pair.net->run_until(
            [&] { return pair.client.counters().paths_closed == 4U && pair.server.counters().paths_closed == 4U; },
            600000U * kMs));
        const u64 sent = pair.client.next_packet_number(QuicSpace::Application, 0U);
        pair.stream_to_server(64U * 1024U);
        REQUIRE(pair.net->run_until(
            [&]
            {
                return pair.client.recovery().bytes_in_flight(0U) == 0U &&
                       pair.server.recovery().bytes_in_flight(0U) == 0U;
            },
            600000U * kMs));
        CHECK(pair.client.next_packet_number(QuicSpace::Application, 0U) > sent + 40U);
        for (QuicConnection* end : {&pair.client, &pair.server})
        {
            CHECK(end->state() == QuicConnectionState::Established);
            CHECK(end->path() == end->paths(0U).path(end->paths(0U).active()));
            CHECK(end->local_max_path_id() == 7U);
            CHECK(end->abandon_path(4U, 0U, pair.net->now) == QuicAbandon::LastPath);
        }
        // Path ID 5 opens in slot 1, and the close goes on path ID 4, the leading path.
        REQUIRE(pair.net->run_until([&] { return pair.client.peer_cid_count(5U) != 0U; }));
        REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
        CHECK(path_id == 5U);
        CHECK(pair.client.protection().path_id(1U) == 5U);
        REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
        CHECK(pair.client.path() == pair.client.paths(0U).path(pair.client.paths(0U).active()));
        pair.client.close(0x2AU, ConstSpan<u8>(), pair.net->now);
        REQUIRE(pair.net->run_until([&] { return pair.server.state() == QuicConnectionState::Draining; }));
        CHECK(pair.server.close_info().error_code == 0x2AU);
    }
}

TEST_CASE("QUIC connection: a path ID in a reclaimed slot 0 that never validates is abandoned, and the server's "
          "stream and a tail only its probe timeout recovers still complete on path ID 1",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 sections 3.1 and 7.2: the new path ID spends only its own amplification credit, and its failed
    // validation abandons it while path ID 1 carries the data.
    for (u64 seed = 1U; seed <= 4U; ++seed)
    {
        INFO("seed " << seed);
        MultipathPair pair;
        pair.net->rng.state = seed;
        for (Path& path : pair.net->path)
        {
            path.loss = 20U;
            path.duplicate = 20U;
            path.jitter_ns = 3U * kMs;
        }
        pair.confirm();
        REQUIRE(pair.net->run_until(
            [&] { return pair.client.peer_cid_count(1U) != 0U && pair.server.peer_cid_count(1U) != 0U; }));
        u64 path_id = 0U;
        REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
        REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
        REQUIRE(pair.client.abandon_path(0U, code(QuicTransportError::NoError), pair.net->now) ==
                QuicAbandon::Abandoned);
        REQUIRE(pair.net->run_until(
            [&]
            {
                return pair.client.counters().paths_closed == 1U && pair.server.counters().paths_closed == 1U &&
                       pair.client.peer_cid_count(2U) != 0U && pair.server.peer_cid_count(2U) != 0U;
            },
            600000U * kMs));
        // Nothing reaches the client's socket 2: the server's slot 0 spends its credit on a path that never answers.
        pair.net->down_in = 1U << 2U;
        REQUIRE(pair.client.add_path(QuicPath{2U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
        CHECK(path_id == 2U);
        CHECK(pair.client.protection().path_id(0U) == 2U);
        REQUIRE(pair.net->run_until([&] { return pair.server.protection().path_open(0U); }));
        CHECK(pair.server.protection().path_id(0U) == 2U);
        u64 stream = 0U;
        REQUIRE(pair.server.streams().open(true, stream) == QuicStreamError::None);
        constexpr u64 total = 96U * 1024U;
        u64 written = 0U;
        u64 read = 0U;
        REQUIRE(pair.net->run_until(
            [&]
            {
                static_cast<void>(write_pattern(pair.server.streams(), stream, written, total, stream));
                return pair.client.streams().recv_state(stream) != QuicRecvState::None &&
                       drain(pair.client.streams(), stream, read, stream) &&
                       pair.client.counters().paths_closed == 2U && pair.server.counters().paths_closed == 2U;
            },
            600000U * kMs));
        CHECK(read == total);
        // A tail only the server's probe timeout recovers: everything it sends for 300 ms is lost.
        u64 tail = 0U;
        REQUIRE(pair.server.streams().open(true, tail) == QuicStreamError::None);
        u64 tail_written = 0U;
        u64 tail_read = 0U;
        static_cast<void>(write_pattern(pair.server.streams(), tail, tail_written, 8U * 1024U, tail));
        pair.net->path[1].loss = 1000U;
        pair.net->run_at(pair.net->now + 300U * kMs);
        pair.net->path[1].loss = 20U;
        REQUIRE(pair.net->run_until(
            [&]
            {
                return pair.client.streams().recv_state(tail) != QuicRecvState::None &&
                       drain(pair.client.streams(), tail, tail_read, tail);
            },
            600000U * kMs));
        CHECK(tail_read == 8U * 1024U);
        for (const QuicConnection* end : {&pair.client, &pair.server})
        {
            CHECK(end->state() == QuicConnectionState::Established);
            CHECK_FALSE(end->protection().path_open(0U));
            CHECK(end->path() == end->paths(1U).path(end->paths(1U).active()));
        }
        CHECK(pair.client.counters().path_failures != 0U);
    }
}

TEST_CASE("QUIC connection: ends that abandon each other's last path at once both close the connection with "
          "NO_ERROR",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 3.4: a PATH_ABANDON for the only open path SHOULD be answered with CONNECTION_CLOSE. The
    // client gives up path ID 0 and the server path ID 1 in the same instant, so each frame names the other end's
    // last path still sending.
    MultipathPair pair;
    pair.confirm();
    REQUIRE(pair.net->run_until(
        [&] { return pair.client.peer_cid_count(1U) != 0U && pair.server.peer_cid_count(1U) != 0U; }));
    u64 path_id = 0U;
    REQUIRE(pair.client.add_path(QuicPath{1U, kServerAt}, pair.net->now, path_id) == QuicMigration::Started);
    REQUIRE(pair.net->run_until([&] { return pair.open_both(1U); }));
    pair.net->advance(pair.net->now + 200U * kMs);
    REQUIRE(pair.client.abandon_path(0U, 0U, pair.net->now) == QuicAbandon::Abandoned);
    REQUIRE(pair.server.abandon_path(1U, 0U, pair.net->now) == QuicAbandon::Abandoned);
    REQUIRE(pair.net->run_until(
        [&]
        {
            return pair.client.state() == QuicConnectionState::Closed &&
                   pair.server.state() == QuicConnectionState::Closed;
        },
        600000U * kMs));
    for (const QuicConnection* end : {&pair.client, &pair.server})
    {
        CHECK(end->close_info().error_code == code(QuicTransportError::NoError));
        CHECK_FALSE(end->close_info().path_lost);
    }
}

TEST_CASE("QUIC connection: a PATH_ABANDON of path ID 0 sent on path ID 0 itself is answered on path ID 1, path 0's "
          "stragglers are acknowledged there, and the PATH_ABANDON of path ID 1 then closes the connection there",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 sections 3.4 and 3.4.3: the frame may go on the path it abandons; the answer and the last
    // acknowledgement go on another path (ACK frames are path ID 0's on any path, section 2.3). Path ID 1 is then the
    // only path: its PATH_ABANDON closes the connection, the close going there (section 3.4).
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    run.handshake();
    const QuicPath second = client_at(9U, 40000U);
    open_path_one(run, second);
    const ConstSpan<u8> further(run.raw.further_issued[1], run.raw.further_bytes[1]);
    const auto ping = [](Span<u8> out)
    {
        QuicFrame frame;
        frame.kind = QuicFrameKind::Ping;
        return write_quic_frame(frame, out);
    };
    // The server's challenge on path ID 1 answered there: the path validates.
    REQUIRE(challenges_to(run, second) == 1U);
    validate_path_one(run, second);
    const u64 unstable = code(QuicTransportError::PathUnstableOrPoor);
    QuicFrame abandon = path_frame(QuicFrameKind::PathAbandon, 0U);
    abandon.error_code = unstable;
    send_frame(run, abandon); // on path ID 0 itself
    REQUIRE(run.server.abandon_of(0U) != nullptr);
    CHECK(run.server.abandon_of(0U)->peer_error_code == unstable);
    CHECK_FALSE(run.server.recovery().path_sending(0U));
    CHECK(run.server.state() == QuicConnectionState::Established);
    CHECK(run.server.path() == second);
    run.drain();
    CHECK(run.raw.abandons == 1U);
    CHECK(run.raw.abandon_id == 0U);
    CHECK(run.raw.abandon_error == code(QuicTransportError::NoError));
    CHECK(run.raw.abandon_further);
    // A straggler on path ID 0 is read and acknowledged in an ACK frame on path ID 1, nothing sent on path ID 0.
    const usize acks = run.raw.acks_further;
    const u64 straggler = run.raw.next[static_cast<usize>(QuicSpace::Application)];
    const u64 sent_on_zero = run.server.next_packet_number(QuicSpace::Application, 0U);
    run.send(QuicPacketType::OneRtt, ping);
    run_timers(run, run.now + 100U * kMs);
    CHECK(run.raw.acks_further > acks);
    CHECK(run.raw.ack_further_largest == straggler);
    CHECK(run.server.next_packet_number(QuicSpace::Application, 0U) == sent_on_zero);
    // Its PATH_ABANDON acknowledged, path ID 0 closes three PTOs later; then its frames and packets are ignored.
    run.send_on_path(further, second, [&](Span<u8> out) { return run.ack_further(out); });
    REQUIRE(run.server.abandon_of(0U) != nullptr);
    REQUIRE(run.server.abandon_of(0U)->acked);
    run_timers(run, run.server.abandon_of(0U)->deadline_ns);
    REQUIRE(run.server.counters().paths_closed == 1U);
    CHECK_FALSE(run.server.protection().path_open(0U));
    QuicFrame issued = path_frame(QuicFrameKind::NewConnectionId, 0U, 5U);
    const u8 late_id[sizeof(kClientCid)] = {0x51U, 0x52U, 0x53U};
    const u8 token[kQuicResetTokenBytes] = {0x5AU, 0x5BU};
    issued.data = ConstSpan<u8>(late_id, sizeof(late_id));
    issued.reset_token = ConstSpan<u8>(token, sizeof(token));
    const QuicFrame retire = path_frame(QuicFrameKind::RetireConnectionId, 0U, 0U);
    run.send_on_path(further, second, [&](Span<u8> out) { return write_quic_frame(issued, out); });
    run.send_on_path(further, second, [&](Span<u8> out) { return write_quic_frame(retire, out); });
    run.send_on_path(further, second, [&](Span<u8> out) { return RawRun::ack(out, 0U); });
    CHECK(run.server.state() == QuicConnectionState::Established);
    CHECK(run.server.peer_cid_count(0U) == 0U);
    const u64 dropped = run.server.counters().packets_dropped;
    run.send(QuicPacketType::OneRtt, ping);
    CHECK(run.server.counters().packets_dropped == dropped + 1U);
    CHECK(run.server.state() == QuicConnectionState::Established);
    // Path ID 1 is the last path: its PATH_ABANDON closes the connection, answered there.
    const QuicFrame last = path_frame(QuicFrameKind::PathAbandon, 1U);
    run.send_on_path(further, second, [&](Span<u8> out) { return write_quic_frame(last, out); });
    REQUIRE(run.server.state() == QuicConnectionState::Closing);
    CHECK(run.server.close_info().error_code == code(QuicTransportError::NoError));
    CHECK(run.raw.close_seen);
    CHECK(run.raw.close_dcid == kClientPathCid[0]);
    CHECK(run.raw.sent_to[(run.raw.datagrams - 1U) % 256U] == second);
}

TEST_CASE("QUIC connection: a path ID that spends its amplification credit in a reclaimed slot 0 leaves the probe "
          "timeout armed, and what path ID 1 has in flight is probed while the client is silent",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 9002 section 6.2.2.1's deadlock guard is the handshake's: once path ID 0 closes, a later path ID validating
    // in slot 0 spends only its own credit (RFC 9000 section 8.2, RFC 10069 section 3.1) and never disarms the
    // connection's probe timeout, which still serves what is in flight on path ID 1.
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    run.handshake();
    announce_path(run, 2U, ConstSpan<u8>(kClientPathTwoCid, sizeof(kClientPathTwoCid)));
    const QuicPath second = client_at(9U, 40000U);
    open_path_one(run, second);
    validate_path_one(run, second);
    const ConstSpan<u8> further(run.raw.further_issued[1], run.raw.further_bytes[1]);
    send_frame(run, path_frame(QuicFrameKind::PathAbandon, 0U));
    run.send_on_path(further, second, [&](Span<u8> out) { return run.ack_further(out); });
    REQUIRE(run.server.abandon_of(0U) != nullptr);
    REQUIRE(run.server.abandon_of(0U)->acked);
    run_timers(run, run.server.abandon_of(0U)->deadline_ns);
    REQUIRE(run.server.counters().paths_closed == 1U);
    // An ack-eliciting PING in flight on path ID 1, never acknowledged.
    const usize before = bytes_to(run, second);
    run.server.ping();
    run.drain();
    const usize to_second = bytes_to(run, second);
    REQUIRE(to_second > before);
    // Path ID 2 opens in the reclaimed slot 0 from a third address with one unpadded datagram, then nothing more:
    // three times its size is all the server may send there.
    REQUIRE(run.raw.further_bytes[2] != 0U);
    REQUIRE(run.raw.protection.open_path(2U, 2U)); // the raw client's own slot 2
    run.raw.path_slot = 2U;
    const QuicPath third = client_at(10U, 40000U);
    run.path_pad_to = 0U;
    const u8 data[kQuicPathDataBytes] = {1, 2, 3, 4, 5, 6, 7, 8};
    run.send_on_path(ConstSpan<u8>(run.raw.further_issued[2], run.raw.further_bytes[2]), third,
                     [&](Span<u8> out)
                     {
                         QuicFrame frame;
                         frame.kind = QuicFrameKind::PathChallenge;
                         frame.data = ConstSpan<u8>(data, sizeof(data));
                         return write_quic_frame(frame, out);
                     });
    run.raw.path_slot = 1U;
    run.path_pad_to = kQuicMinInitialDatagram;
    REQUIRE(run.server.protection().path_open(0U));
    REQUIRE(run.server.protection().path_id(0U) == 2U);
    // Slot 0 spends its credit to under one useful packet (64 bytes); path ID 1's probe timeout still fires after
    // that, until path ID 2's validation fails and then on.
    const QuicRecovery& recovery = run.server.recovery();
    bool spent = false;
    u32 ptos_at_spent = 0U;
    for (usize guard = 0U; guard < 512U && (run.server.counters().path_failures == 0U || !spent ||
                                            recovery.pto_count(1U) < ptos_at_spent + 2U);
         ++guard)
    {
        if (!spent && run.server.protection().path_open(0U))
        {
            const QuicPaths& paths = run.server.paths(0U);
            spent = paths.credit(paths.active()) < 64U;
            ptos_at_spent = recovery.pto_count(1U);
        }
        REQUIRE(recovery.loss_timer_ns() != kNeverNs);
        const u64 next = run.server.next_timer_ns();
        REQUIRE(next != kNeverNs);
        run.now = (next > run.now) ? next : run.now;
        run.server.on_timeout(run.now);
        run.drain();
        REQUIRE(run.server.state() == QuicConnectionState::Established);
    }
    REQUIRE(spent);
    CHECK(bytes_to(run, third) != 0U);
    CHECK(recovery.pto_count(1U) >= ptos_at_spent + 2U);
    CHECK(bytes_to(run, second) > to_second);
    CHECK(run.server.counters().path_failures == 1U);
    CHECK_FALSE(run.server.recovery().path_sending(0U));
}

namespace
{
// Whether the server validated `path` on any path ID's 4-tuples.
bool proven_anywhere(const RawRun& run, const QuicPath& path)
{
    bool proven = false;
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        const usize index = run.server.protection().path_open(slot) ? run.server.paths(slot).find(path) : kQuicNoPath;
        proven = proven || (index != kQuicNoPath && run.server.paths(slot).validated(index));
    }
    return proven;
}

// The server's validations of addresses validated on no path ID that have started by now, each of them `timeout_ns`
// long: a paced one has not.
usize validations_underway(const RawRun& run, u64 timeout_ns)
{
    usize count = 0U;
    for (usize slot = 0U; slot < kQuicMaxPaths; ++slot)
    {
        if (!run.server.protection().path_open(slot))
        {
            continue;
        }
        const QuicPaths& paths = run.server.paths(slot);
        for (usize index = 0U; index < kQuicMaxPaths; ++index)
        {
            const bool started = paths.deadline_ns(index) - timeout_ns <= run.now;
            const bool paced = paths.used(index) && !proven_anywhere(run, paths.path(index));
            count += (paced && paths.validating(index) && started) ? 1U : 0U;
        }
    }
    return count;
}

// When the validation of `path` in the server's slot `slot` is abandoned.
u64 validation_deadline(const RawRun& run, usize slot, const QuicPath& path)
{
    const QuicPaths& paths = run.server.paths(slot);
    const usize index = paths.find(path);
    REQUIRE(index != kQuicNoPath);
    REQUIRE(paths.validating(index));
    return paths.deadline_ns(index);
}
} // namespace

TEST_CASE("QUIC connection: a peer's validations of addresses it chose are paced, kQuicMaxPaths at once over every "
          "path ID; a repeat takes no turn, a validated address waits for none, and a paced one validates late",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 7.2: a peer can make the server challenge an address of its choosing on each 4-tuple of each
    // path ID at once. Path IDs 1 and 2 open from two addresses, path ID 0 is probed from a third three times and
    // rebinds to a fourth: four turns, path ID 1's held though answered, while the old address's check (section
    // 9.3.3) and path ID 3 opening on that validated address take none. A second rebinding's validation waits for
    // the earliest turn to end, then runs its own full timeout and validates. Nothing is acknowledged, so every
    // timeout is the same.
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    const QuicPath home = client_at(7U, 50000U);
    run.from = home;
    run.handshake();
    announce_path(run, 2U, ConstSpan<u8>(kClientPathTwoCid, sizeof(kClientPathTwoCid)));
    const u64 start = run.now;
    const QuicPath first = client_at(9U, 40000U);
    open_path_one(run, first);
    CHECK(challenges_on(run, first) == 1U);
    const u64 turn_end = validation_deadline(run, 1U, first);
    const u64 timeout = turn_end - start;
    // Path ID 1's address is answered at once, yet its turn stays held to turn_end; the others start 1 ms later, so
    // the deferred start below is the only timer then.
    validate_path_one(run, first);
    run.now += kMs;
    const u64 later_end = turn_end + kMs;
    const QuicPath second = client_at(10U, 40000U);
    REQUIRE(run.raw.protection.open_path(2U, 2U));
    run.raw.path_slot = 2U;
    const u8 data[kQuicPathDataBytes] = {1, 2, 3, 4, 5, 6, 7, 8};
    run.send_on_path(ConstSpan<u8>(run.raw.further_issued[2], run.raw.further_bytes[2]), second,
                     [&](Span<u8> out)
                     {
                         QuicFrame frame;
                         frame.kind = QuicFrameKind::PathChallenge;
                         frame.data = ConstSpan<u8>(data, sizeof(data));
                         return write_quic_frame(frame, out);
                     });
    run.raw.path_slot = 1U;
    REQUIRE(run.server.counters().paths_opened == 2U);
    REQUIRE(run.server.protection().path_id(2U) == 2U);
    CHECK(validation_deadline(run, 2U, second) == later_end); // at once (the raw client reads path ID 2 not)
    CHECK(bytes_to(run, second) != 0U);
    run.pad_to = kQuicMinInitialDatagram;
    const QuicPath probe = client_at(11U, 40000U);
    run.from = probe;
    for (usize repeat = 0U; repeat < 3U; ++repeat)
    {
        send_frame(run, QuicFrameKind::PathChallenge, ConstSpan<u8>(data));
    }
    CHECK(run.server.path() == home);
    CHECK(challenges_on(run, probe) == 1U);
    CHECK(validation_deadline(run, 0U, probe) == later_end);
    CHECK(validations_underway(run, timeout) == 2U);
    const usize home_checks = challenges_on(run, home);
    const QuicPath rebound = client_at(12U, 40000U);
    run.from = rebound;
    send_frame(run, QuicFrameKind::Ping);
    REQUIRE(run.server.path() == rebound);
    CHECK(challenges_on(run, rebound) == 1U);
    CHECK(validation_deadline(run, 0U, rebound) == later_end);
    CHECK(challenges_on(run, home) == home_checks + 1U); // the validated address's check, at once
    CHECK(validations_underway(run, timeout) == kQuicMaxPaths - 1U); // and path ID 1's answered turn
    // Path ID 3 opens on path ID 0's validated 4-tuple (section 5.2): its own validation waits for no turn either.
    announce_path(run, 3U, ConstSpan<u8>(kClientPathThreeCid, sizeof(kClientPathThreeCid)));
    REQUIRE(run.raw.protection.open_path(3U, 3U));
    run.raw.path_slot = 3U;
    const usize home_bytes = bytes_to(run, home);
    run.send_on_path(ConstSpan<u8>(run.raw.further_issued[3], run.raw.further_bytes[3]), home,
                     [&](Span<u8> out)
                     {
                         QuicFrame frame;
                         frame.kind = QuicFrameKind::PathChallenge;
                         frame.data = ConstSpan<u8>(data, sizeof(data));
                         return write_quic_frame(frame, out);
                     });
    run.raw.path_slot = 1U;
    REQUIRE(run.server.counters().paths_opened == 3U);
    REQUIRE(run.server.protection().path_id(3U) == 3U);
    CHECK(validation_deadline(run, 3U, home) == later_end);
    CHECK(bytes_to(run, home) > home_bytes);
    CHECK(validations_underway(run, timeout) == kQuicMaxPaths - 1U);
    // Every turn is held: the next address waits for the earliest to end, its deadline a full timeout after that.
    const QuicPath late = client_at(13U, 40000U);
    run.from = late;
    send_frame(run, QuicFrameKind::Ping);
    REQUIRE(run.server.path() == late);
    CHECK(challenges_on(run, late) == 0U);
    const QuicPaths& paths = run.server.paths(0U);
    const usize waiting = paths.find(late);
    REQUIRE(waiting != kQuicNoPath);
    REQUIRE(paths.validating(waiting));
    CHECK(paths.due_ns(waiting) == turn_end);
    CHECK(paths.deadline_ns(waiting) == turn_end + timeout);
    CHECK(validations_underway(run, timeout) == kQuicMaxPaths - 2U); // the rebinding's turn is held, its path gone
    for (usize guard = 0U; guard < 512U && challenges_on(run, late) == 0U; ++guard)
    {
        const u64 next = run.server.next_timer_ns();
        REQUIRE(next != kNeverNs);
        run.now = (next > run.now) ? next : run.now;
        run.server.on_timeout(run.now);
        run.drain();
        REQUIRE(run.server.state() == QuicConnectionState::Established);
        CHECK(validations_underway(run, timeout) <= kQuicMaxPaths);
        CHECK((challenges_on(run, late) == 0U || run.now >= turn_end));
    }
    REQUIRE(challenges_on(run, late) != 0U);
    CHECK(run.now == turn_end);
    answer(run, late);
    const usize validated = run.server.paths(0U).find(late);
    REQUIRE(validated != kQuicNoPath);
    CHECK(run.server.paths(0U).validated(validated));
    CHECK(run.server.path() == late);
    CHECK(run.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: once path ID 0 is abandoned, the last path failing its validation ends the connection "
          "silently",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 10069 section 3.1 and RFC 9000 section 9.3.2: a path whose validation fails is not used; with no other path
    // still sending, no validated path is left and the state is discarded without a close.
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    run.handshake();
    const QuicPath second = client_at(9U, 40000U);
    open_path_one(run, second);
    send_frame(run, path_frame(QuicFrameKind::PathAbandon, 0U)); // path ID 1's challenge is never answered
    REQUIRE(run.server.abandon_of(0U) != nullptr);
    CHECK(run.server.state() == QuicConnectionState::Established);
    for (usize guard = 0U; guard < 256U && run.server.state() == QuicConnectionState::Established; ++guard)
    {
        const u64 next = run.server.next_timer_ns();
        REQUIRE(next != kNeverNs);
        run.now = (next > run.now) ? next : run.now;
        run.server.on_timeout(run.now);
        run.drain();
    }
    CHECK(run.server.state() == QuicConnectionState::Closed);
    CHECK(run.server.close_info().path_lost);
    CHECK(run.server.counters().path_failures == 1U);
    CHECK_FALSE(run.raw.close_seen);
    CHECK(bytes_to(run, second) <= 3U * kQuicMinInitialDatagram);
}

TEST_CASE("QUIC connection: the handshake slot's validation failing with no path to fall back to abandons path ID 0 "
          "while path ID 1 sends",
          "[net][net-path][quic-connection][multipath]")
{
    // RFC 9000 section 9.3.2 and RFC 10069 section 3.1: with no validated 4-tuple left on path ID 0 the path is not
    // used again; another path still sending keeps the connection, and the path ID is abandoned on it.
    Identity identity;
    CountingAllocator host_allocator;
    Host host(host_allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid)));
    RawRun run(identity, 2048U, {}, &host, {}, 0U, QuicAddressDiscovery::Off, true, 3U);
    run.multipath = true;
    const QuicPath a = client_at(7U, 50000U);
    const QuicPath b = client_at(8U, 50000U);
    run.from = a;
    run.handshake();
    const QuicPath second = client_at(9U, 40000U);
    open_path_one(run, second);
    const ConstSpan<u8> further(run.raw.further_issued[1], run.raw.further_bytes[1]);
    REQUIRE(challenges_to(run, second) == 1U);
    validate_path_one(run, second);
    // Path ID 0 moves to b, whose challenge (and a's again) the client never answers; path ID 1 keeps acknowledging.
    run.pad_to = kQuicMinInitialDatagram;
    run.from = b;
    send_frame(run, QuicFrameKind::Ping);
    REQUIRE(run.server.paths(0U).path(run.server.paths(0U).active()) == b);
    run.pad_to = 0U;
    for (usize guard = 0U; guard < 512U && run.server.abandon_of(0U) == nullptr; ++guard)
    {
        const u64 next = run.server.next_timer_ns();
        REQUIRE(next != kNeverNs);
        run.now = (next > run.now) ? next : run.now;
        run.server.on_timeout(run.now);
        run.drain();
        run.send_on_path(further, second, [&](Span<u8> out) { return run.ack_further(out); });
        REQUIRE(run.server.state() == QuicConnectionState::Established);
    }
    REQUIRE(run.server.abandon_of(0U) != nullptr);
    CHECK(run.server.counters().path_failures == 2U);
    CHECK_FALSE(run.server.recovery().path_sending(0U));
    CHECK(run.server.path() == second);
    run.drain();
    CHECK(run.raw.abandon_id == 0U);
    CHECK(run.raw.abandon_error == code(QuicTransportError::PathUnstableOrPoor));
    CHECK(run.raw.abandon_further);
    CHECK_FALSE(run.raw.close_seen);
}

namespace
{
constexpr usize kVoiceBytes = 64U;

// The server's reads of the client's redundant datagrams, each `kVoiceBytes` starting with its id: every arrival
// counted, and its first one's time.
struct Arrivals
{
    static constexpr usize kCount = 64U;
    usize copies[kCount] = {};
    u64 first_ns[kCount] = {};

    void read(QuicConnection& to, u64 now_ns)
    {
        ConstSpan<u8> held;
        while (to.datagrams().peek(held))
        {
            REQUIRE(held.size() == kVoiceBytes);
            const usize id = held[0];
            REQUIRE(id < kCount);
            for (usize offset = 1U; offset < kVoiceBytes; ++offset)
            {
                REQUIRE(held[offset] == static_cast<u8>(id + offset));
            }
            first_ns[id] = (copies[id] == 0U) ? now_ns : first_ns[id];
            ++copies[id];
            to.datagrams().pop();
        }
    }

    // Whether datagrams `first` to `last` (excluded) each arrived `times` times.
    [[nodiscard]] bool each(usize first, usize last, usize times) const
    {
        usize matched = 0U;
        for (usize id = first; id < last; ++id)
        {
            matched += (copies[id] == times) ? 1U : 0U;
        }
        return matched == last - first;
    }
};

// Queues voice datagram `id` as `how` says: its queue() answer, the id checked when it took it.
QuicDatagramError queue_voice(QuicConnection& from, usize id, u64 now_ns, const QuicDatagramSend& how)
{
    u8 payload[kVoiceBytes];
    for (usize offset = 0U; offset < kVoiceBytes; ++offset)
    {
        payload[offset] = static_cast<u8>(id + offset);
    }
    u64 queued = ~u64{0};
    const QuicDatagramError error = from.datagrams().queue(ConstSpan<u8>(payload, kVoiceBytes), now_ns, queued, how);
    if (error == QuicDatagramError::None)
    {
        CHECK(queued == id);
    }
    return error;
}

void queue_redundant(QuicConnection& from, usize id, u64 now_ns)
{
    QuicDatagramSend how;
    how.redundant = true;
    REQUIRE(queue_voice(from, id, now_ns, how) == QuicDatagramError::None);
}

QuicDatagramSend pinned_to(u64 path_id)
{
    QuicDatagramSend how;
    how.pinned = true;
    how.path_id = path_id;
    return how;
}
} // namespace

TEST_CASE("QUIC connection: a redundant datagram crosses every path carrying data, the slower one yielding its data "
          "still sending copies, none to a backup path, is reported once, and arrives at once while the faster path "
          "goes dark",
          "[net][net-path][quic-connection][multipath]")
{
    // S78's scheduling: input and voice want the first copy that arrives. Path ID 1 is 60 ms slower each round trip,
    // so under LowestRtt it carries no data of its own, yet its window and pacer send each datagram's copy.
    MultipathPair pair(2U, 2U, false, true, 256U, true, 32U);
    pair.net->socket_delay_ns[1] = 30U * kMs;
    pair.open_second();
    QuicConnection& client = pair.client;
    Arrivals arrivals;
    usize acked = 0U;
    const auto settle = [&]
    {
        arrivals.read(pair.server, pair.net->now);
        QuicDatagramReport report;
        while (client.datagrams().poll(report))
        {
            REQUIRE(report.outcome == QuicDatagramOutcome::Acked); // one copy at least always arrives
            ++acked;
        }
    };
    // A stream trickles beside them: its data rides path ID 0 alone, never in a packet of copies (DataWatch).
    DataWatch watch(client, 0U, pair.net->now);
    pair.net->filter = &watch;
    u64 stream = 0U;
    REQUIRE(open_uni(pair, stream));
    u64 written = 0U;
    // One datagram each 20 ms, as voice frames, from `first` to `last`, with 1000 stream bytes: each arrives within
    // `within_ns`. With `released` the queue holds nothing once it arrived, before any acknowledgement: no copy is
    // owed.
    const auto voice = [&](usize first, usize last, u64 within_ns, bool released)
    {
        for (usize id = first; id < last; ++id)
        {
            INFO("datagram " << id);
            const u64 queued_ns = pair.net->now;
            queue_redundant(client, id, queued_ns);
            static_cast<void>(write_pattern(client.streams(), stream, written, (id + 1U) * 1000U, stream, false));
            REQUIRE(pair.net->run_until(
                [&]
                {
                    settle();
                    return arrivals.copies[id] != 0U;
                }));
            CHECK(arrivals.first_ns[id] - queued_ns <= within_ns);
            if (released)
            {
                CHECK(client.datagrams().queued() == 0U);
            }
            pair.net->advance(queued_ns + 20U * kMs);
        }
        REQUIRE(pair.net->run_until(
            [&]
            {
                settle();
                return acked == last;
            }));
    };
    // Clean paths: the first copy rides path ID 0 (20 ms one way), the other path ID 1.
    voice(0U, 12U, 21U * kMs, false);
    REQUIRE(pair.net->run_until(
        [&]
        {
            settle();
            return arrivals.each(0U, 12U, 2U);
        }));
    CHECK(client.datagrams().counters().copies == 12U);
    CHECK(client.datagrams().queued() == 0U);
    CHECK(watch.data(0U) != 0U);
    CHECK(watch.data(1U) == 0U);
    CHECK(watch.kept());
    pair.net->filter = nullptr;
    // The server marks path ID 1 backup (RFC 10069 section 3.3): it carries no copy while path ID 0 is usable.
    REQUIRE(pair.server.set_path_status(1U, true) == QuicPathStatusChange::Set);
    REQUIRE(pair.net->run_until([&] { return client.peer_path_status(1U) == QuicPathStatus::Backup; }));
    voice(12U, 20U, 21U * kMs, true);
    CHECK(client.datagrams().counters().copies == 12U);
    REQUIRE(pair.server.set_path_status(1U, false) == QuicPathStatusChange::Set);
    REQUIRE(pair.net->run_until([&] { return client.peer_path_status(1U) == QuicPathStatus::Available; }));
    // Path ID 0 goes dark: each datagram still arrives within path ID 1's one way (50 ms), with no timeout waited for.
    // Once path ID 0 is known dark it is owed no copies.
    pair.net->down = 1U << 0U;
    voice(20U, 23U, 51U * kMs, false);
    REQUIRE(pair.net->run_until([&] { return client.recovery().pto_count(0U) != 0U; }));
    voice(23U, 32U, 51U * kMs, true);
    CHECK(arrivals.each(12U, 32U, 1U));
    CHECK(client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a pinned datagram crosses only its path ID, the slower one yielding its data or marked "
          "backup, from either end, and those waiting when their path ID is abandoned are Lost there",
          "[net][net-path][quic-connection][multipath]")
{
    // S78's scheduling: a flow kept on one path. Path ID 1 is 30 ms slower each way (50 ms against 20 ms), so under
    // LowestRtt it carries no data of its own, yet its window and pacer send what is pinned to it.
    MultipathPair pair(2U, 2U, false, true, 256U, true);
    pair.net->socket_delay_ns[1] = 30U * kMs;
    pair.open_second();
    QuicConnection& client = pair.client;
    QuicConnection& server = pair.server;
    Arrivals at_server;
    Arrivals at_client;
    usize acked[2] = {};
    usize lost = 0U;
    const auto settle = [&]
    {
        at_server.read(server, pair.net->now);
        at_client.read(client, pair.net->now);
        QuicDatagramReport report;
        for (usize end = 0U; end < 2U; ++end)
        {
            QuicConnection& from = (end == 0U) ? client : server;
            while (from.datagrams().poll(report))
            {
                acked[end] += (report.outcome == QuicDatagramOutcome::Acked) ? 1U : 0U;
                lost += (report.outcome == QuicDatagramOutcome::Lost) ? 1U : 0U;
            }
        }
    };
    // One datagram each 20 ms from `first` to `last`, pinned to `path_id` by `from`: each arrives once, between
    // `low_ns` and `high_ns` after it was queued, and is reported Acked.
    const auto flow =
        [&](QuicConnection& from, Arrivals& arrivals, usize first, usize last, u64 path_id, u64 low_ns, u64 high_ns)
    {
        const usize end = (&from == &client) ? 0U : 1U;
        const usize before = acked[end];
        for (usize id = first; id < last; ++id)
        {
            INFO("datagram " << id);
            const u64 queued_ns = pair.net->now;
            REQUIRE(queue_voice(from, id, queued_ns, pinned_to(path_id)) == QuicDatagramError::None);
            REQUIRE(pair.net->run_until(
                [&]
                {
                    settle();
                    return arrivals.copies[id] != 0U;
                }));
            CHECK(arrivals.first_ns[id] - queued_ns >= low_ns);
            CHECK(arrivals.first_ns[id] - queued_ns <= high_ns);
            pair.net->advance(queued_ns + 20U * kMs);
        }
        REQUIRE(pair.net->run_until(
            [&]
            {
                settle();
                return acked[end] - before == last - first;
            }));
        CHECK(arrivals.each(first, last, 1U));
    };
    // A stream trickles beside the client's flow on path ID 1: its data rides path ID 0 alone (DataWatch).
    DataWatch watch(client, 0U, pair.net->now);
    pair.net->filter = &watch;
    u64 stream = 0U;
    REQUIRE(open_uni(pair, stream));
    u64 written = 0U;
    for (usize id = 0U; id < 8U; ++id)
    {
        static_cast<void>(write_pattern(client.streams(), stream, written, (id + 1U) * 1000U, stream, false));
        flow(client, at_server, id, id + 1U, 1U, 50U * kMs, 51U * kMs);
    }
    CHECK(watch.data(0U) != 0U);
    CHECK(watch.data(1U) == 0U);
    CHECK(watch.kept());
    pair.net->filter = nullptr;
    // The server pins to either path ID of its own: path ID 0's arrive in its one way, path ID 1's in its own.
    flow(server, at_client, 0U, 4U, 1U, 50U * kMs, 51U * kMs);
    flow(server, at_client, 4U, 8U, 0U, 0U, 21U * kMs);
    // The server marks path ID 1 backup (RFC 10069 section 3.3): the client's pinned flow still takes it.
    REQUIRE(server.set_path_status(1U, true) == QuicPathStatusChange::Set);
    REQUIRE(pair.net->run_until([&] { return client.peer_path_status(1U) == QuicPathStatus::Backup; }));
    flow(client, at_server, 8U, 12U, 1U, 50U * kMs, 51U * kMs);
    // A path ID open in no slot is refused.
    CHECK(queue_voice(client, 12U, pair.net->now, pinned_to(2U)) == QuicDatagramError::NoPath);
    // Three pinned to path ID 1 wait unsent beside a plain one when the client abandons it: each Lost at once, counted
    // unrouted, never sent; the plain one crosses path ID 0, and path ID 1 is refused from then on.
    for (usize id = 12U; id < 15U; ++id)
    {
        REQUIRE(queue_voice(client, id, pair.net->now, pinned_to(1U)) == QuicDatagramError::None);
    }
    REQUIRE(queue_voice(client, 15U, pair.net->now, QuicDatagramSend{}) == QuicDatagramError::None);
    const u64 sent = client.datagrams().counters().sent;
    REQUIRE(client.abandon_path(1U, 0U, pair.net->now) == QuicAbandon::Abandoned);
    CHECK(client.datagrams().counters().unrouted == 3U);
    settle();
    CHECK(lost == 3U);
    CHECK(queue_voice(client, 16U, pair.net->now, pinned_to(1U)) == QuicDatagramError::NoPath);
    const u64 queued_ns = pair.net->now;
    REQUIRE(pair.net->run_until(
        [&]
        {
            settle();
            return at_server.copies[15U] != 0U && acked[0] == 13U;
        }));
    CHECK(at_server.first_ns[15U] - queued_ns <= 21U * kMs);
    CHECK(client.datagrams().counters().sent == sent + 1U);
    CHECK(at_server.each(12U, 15U, 0U));
    CHECK(lost == 3U);
    CHECK(client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a pinned stream crosses only its path ID, the slower one yielding its data or marked "
          "backup, from either end, and waits while its path is dark without holding other streams back",
          "[net][net-path][quic-connection][multipath]")
{
    // S78's scheduling: a stream kept on one path. Path ID 1 is 30 ms slower each way (50 ms against 20 ms), so under
    // LowestRtt it carries no data of its own, yet its window and pacer send the stream data pinned to it.
    MultipathPair pair;
    pair.net->socket_delay_ns[1] = 30U * kMs;
    pair.open_second();
    QuicConnection& client = pair.client;
    QuicConnection& server = pair.server;
    u64 plain = 0U;
    u64 pinned = 0U;
    REQUIRE(open_uni(pair, plain));
    REQUIRE(open_uni(pair, pinned));
    CHECK(client.streams().pin(pinned, 2U) == QuicStreamError::NoPath); // open in no slot
    REQUIRE(client.streams().pin(pinned, 1U) == QuicStreamError::None);
    u64 server_stream = 0U;
    REQUIRE(server.streams().open(false, server_stream) == QuicStreamError::None);
    REQUIRE(server.streams().pin(server_stream, 1U) == QuicStreamError::None);
    u64 written[3] = {};
    u64 read[3] = {};
    // 500 more bytes of stream `index` from `from`, 20 ms apart: the time until `to` read them all.
    const auto chunk = [&](QuicConnection& from, QuicConnection& to, u64 id, usize index)
    {
        const u64 start = pair.net->now;
        const u64 target = written[index] + 500U;
        static_cast<void>(write_pattern(from.streams(), id, written[index], target, id, false));
        REQUIRE(written[index] == target);
        REQUIRE(pair.net->run_until(
            [&]
            {
                if (to.streams().recv_state(id) != QuicRecvState::None)
                {
                    static_cast<void>(drain(to.streams(), id, read[index], id));
                }
                return read[index] == target;
            }));
        const u64 took = pair.net->now - start;
        pair.net->advance(start + 20U * kMs);
        return took;
    };
    for (usize round = 0U; round < 8U; ++round)
    {
        INFO("round " << round);
        const u64 fast = chunk(client, server, plain, 0U);
        CHECK(fast <= 21U * kMs);
        const u64 slow = chunk(client, server, pinned, 1U);
        CHECK(slow >= 50U * kMs);
        CHECK(slow <= 51U * kMs);
        const u64 back = chunk(server, client, server_stream, 2U);
        CHECK(back >= 50U * kMs);
        CHECK(back <= 51U * kMs);
    }
    // The server marks path ID 1 backup (RFC 10069 section 3.3): the client's pinned stream still takes it.
    REQUIRE(server.set_path_status(1U, true) == QuicPathStatusChange::Set);
    REQUIRE(pair.net->run_until([&] { return client.peer_path_status(1U) == QuicPathStatus::Backup; }));
    for (usize round = 0U; round < 4U; ++round)
    {
        INFO("backup round " << round);
        CHECK(chunk(client, server, plain, 0U) <= 21U * kMs);
        const u64 slow = chunk(client, server, pinned, 1U);
        CHECK(slow >= 50U * kMs);
        CHECK(slow <= 51U * kMs);
    }
    // Path ID 1 goes dark: the plain stream still arrives in path ID 0's one way, and the pinned one waits for its
    // path (the application chose it), never taking path ID 0, until the path carries again.
    pair.net->down = 1U << 1U;
    const u64 target = written[1] + 500U;
    static_cast<void>(write_pattern(client.streams(), pinned, written[1], target, pinned, false));
    for (usize round = 0U; round < 6U; ++round)
    {
        CHECK(chunk(client, server, plain, 0U) <= 21U * kMs);
    }
    REQUIRE(pair.net->run_until([&] { return client.recovery().pto_count(1U) != 0U; }));
    pair.net->advance(pair.net->now + 400U * kMs);
    static_cast<void>(drain(server.streams(), pinned, read[1], pinned));
    CHECK(read[1] == target - 500U);
    u64 pinned_path = 0U;
    REQUIRE(client.streams().pinned(pinned, pinned_path));
    CHECK(pinned_path == 1U);
    pair.net->down = 0U;
    REQUIRE(pair.net->run_until(
        [&]
        {
            static_cast<void>(drain(server.streams(), pinned, read[1], pinned));
            return read[1] == target;
        },
        5000U * kMs));
    // Dark again, the application unpins the waiting stream: its data, settled lost by the probe timeout, goes on path
    // ID 0 in that path's one way.
    pair.net->down = 1U << 1U;
    const u64 again = written[1] + 500U;
    static_cast<void>(write_pattern(client.streams(), pinned, written[1], again, pinned, false));
    REQUIRE(pair.net->run_until([&] { return client.recovery().pto_count(1U) != 0U; }));
    pair.net->advance(pair.net->now + 400U * kMs);
    static_cast<void>(drain(server.streams(), pinned, read[1], pinned));
    CHECK(read[1] == target);
    REQUIRE(client.streams().unpin(pinned) == QuicStreamError::None);
    CHECK_FALSE(client.streams().pinned(pinned, pinned_path));
    const u64 unpinned_ns = pair.net->now;
    REQUIRE(pair.net->run_until(
        [&]
        {
            static_cast<void>(drain(server.streams(), pinned, read[1], pinned));
            return read[1] == again;
        }));
    CHECK(pair.net->now - unpinned_ns <= 21U * kMs);
    pair.net->down = 0U;
    CHECK(client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a stream backlog pinned to a path that yields its data grows that path's window, held "
          "by the window and pacer as data, and the other path is application-limited once only it waits",
          "[net][net-path][quic-connection][multipath]")
{
    // Path ID 0 becomes 30 ms slower each way, so under LowestRtt it yields its data to path ID 1. First a datagram
    // backlog fills path ID 1's window. Then a stream kept full pinned to path ID 0, its credit wider than the window,
    // makes that path's window and pacer the limit, which keeps growing (RFC 9002 section 7.8), its delivery rate
    // unmarked application-limited while flow control is far from holding it; once the last datagram left, only data
    // pinned to path ID 0 waits, which leaves path ID 1's window the application's limit.
    QuicStreamLimits wide = stream_limits();
    wide.receive_bytes = 256U * 1024U;
    wide.send_bytes = 256U * 1024U;
    wide.stream_window = 128U * 1024U;
    wide.connection_window = 256U * 1024U;
    wide.connection_max_window = 1024U * 1024U;
    MultipathPair pair(2U, 2U, false, true, 1024U, true, 0U, false, wide);
    pair.open_second();
    pair.net->socket_delay_ns[0] = 30U * kMs;
    QuicConnection& client = pair.client;
    QuicConnection& server = pair.server;
    const QuicPathControl& fast = *client.path_control(1U);
    const QuicPathControl& slow = *client.path_control(0U);
    u64 id = 0U;
    REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
    REQUIRE(client.streams().pin(id, 0U) == QuicStreamError::None);
    u64 written = 0U;
    u64 read = 0U;
    usize received = 0U;
    const auto flow = [&](bool datagrams, bool streams)
    {
        u8 payload[1000] = {};
        u64 queued = 0U;
        while (datagrams && client.datagrams().queue(ConstSpan<u8>(payload, sizeof(payload)), pair.net->now, queued) ==
                                QuicDatagramError::None)
        {
        }
        ConstSpan<u8> held;
        while (server.datagrams().peek(held))
        {
            ++received;
            server.datagrams().pop();
        }
        if (streams)
        {
            static_cast<void>(write_pattern(client.streams(), id, written, written + 65536U, id, false));
        }
        if (server.streams().recv_state(id) != QuicRecvState::None)
        {
            static_cast<void>(drain(server.streams(), id, read, id));
        }
    };
    REQUIRE(pair.net->run_until(
        [&]
        {
            flow(true, false);
            return received >= 64U && !fast.delivery().app_limited() &&
                   client.recovery().rtt(0U).smoothed_ns > client.recovery().rtt(1U).smoothed_ns;
        }));
    const u64 initial = slow.congestion().window();
    bool slow_marked = false; // path ID 0's marked after its first round trip of data with under 64 KiB in flight
    bool fast_idle = false;   // path ID 1's marked once only the pinned stream waits, `idle_ns` after the start
    const u64 start_ns = pair.net->now;
    const u64 fast_packets = client.next_packet_number(QuicSpace::Application, 1U);
    u64 idle_ns = 0U;
    const bool grown = pair.net->run_until(
        [&]
        {
            slow_marked = slow_marked || (pair.net->now > start_ns + 150U * kMs && slow.delivery().app_limited() &&
                                          client.recovery().bytes_in_flight(0U) < 64U * 1024U);
            fast_idle = fast_idle || (client.datagrams().queued() == 0U && fast.delivery().app_limited());
            idle_ns = (idle_ns == 0U && fast_idle) ? pair.net->now - start_ns : idle_ns;
            flow(false, true);
            return pair.net->now >= start_ns + 1000U * kMs;
        },
        2000U * kMs);
    INFO("window " << slow.congestion().window() << " from " << initial);
    CHECK(grown);
    CHECK(slow.congestion().window() >= 2U * initial);
    CHECK_FALSE(slow_marked);
    CHECK(fast_idle);
    // At once, and with no packet of nothing sent for the data it cannot carry: 2 ms and 37 packets here, where a
    // slot counting another's pinned data as its own took 360 ms and sent 3222.
    CHECK(idle_ns <= 50U * kMs);
    CHECK(client.next_packet_number(QuicSpace::Application, 1U) - fast_packets <= 100U);
    CHECK(read != 0U);
    CHECK(client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a stream pinned to path ID 1 over links that lose, duplicate and jitter resends there "
          "while it is open, and once either end abandons it every byte, lost or unsent, completes on path ID 0",
          "[net][net-path][quic-connection][multipath]")
{
    // S78: a pinned stream's data is reliable, so once its path stops it goes on any path.
    for (u64 seed = 1U; seed <= 4U; ++seed)
    {
        const bool by_client = (seed % 2U) != 0U;
        INFO("seed " << seed << (by_client ? ", the client abandons" : ", the server abandons"));
        MultipathPair pair;
        pair.net->rng.state = seed;
        for (Path& path : pair.net->path)
        {
            path.loss = 40U;
            path.duplicate = 30U;
            path.jitter_ns = 4U * kMs;
        }
        pair.net->socket_delay_ns[1] = 30U * kMs;
        pair.open_second();
        QuicConnection& client = pair.client;
        u64 stream = 0U;
        REQUIRE(open_uni(pair, stream));
        REQUIRE(client.streams().pin(stream, 1U) == QuicStreamError::None);
        // Full-size datagrams of path ID 0 carry none of it while path ID 1 is open (DataWatch).
        DataWatch watch(client, 0U, pair.net->now);
        pair.net->filter = &watch;
        constexpr u64 total = 160U * 1024U;
        u64 written = 0U;
        u64 read = 0U;
        const auto flow = [&]
        {
            static_cast<void>(write_pattern(client.streams(), stream, written, total, stream));
            return pair.server.streams().recv_state(stream) != QuicRecvState::None &&
                   drain(pair.server.streams(), stream, read, stream);
        };
        const usize dropped = pair.net->dropped[0]; // the client's datagrams the links lost
        REQUIRE(pair.net->run_until(
            [&]
            {
                static_cast<void>(flow());
                return read >= total / 2U && pair.net->dropped[0] > dropped;
            }));
        CHECK(watch.data(0U) == 0U);
        CHECK(watch.data(1U) != 0U);
        QuicConnection& abandoning = by_client ? client : pair.server;
        REQUIRE(abandoning.abandon_path(1U, code(QuicTransportError::ApplicationAbandonPath), pair.net->now) ==
                QuicAbandon::Abandoned);
        REQUIRE(pair.net->run_until([&] { return flow(); }, 20000U * kMs));
        CHECK(read == total);
        u64 pinned_path = 0U;
        CHECK_FALSE(client.streams().pinned(stream, pinned_path));
        CHECK(watch.data(0U) != 0U);
        CHECK(client.state() == QuicConnectionState::Established);
        pair.net->filter = nullptr;
    }
}

TEST_CASE("QUIC connection: a backlog pinned to a path that yields its data grows that path's window, held by the "
          "window and pacer as data, not by the application",
          "[net][net-path][quic-connection][multipath]")
{
    // Path ID 1 is 30 ms slower each way, so under LowestRtt it yields its data; a queue kept full pinned to it (no
    // outcomes, so nothing but the ring holds the application back) keeps its window and pacer the limit, which slow
    // start then grows (RFC 9002 section 7.8), and never marks its delivery rate application-limited.
    MultipathPair pair(2U, 2U, false, true, 256U, true, 0U, false);
    pair.net->socket_delay_ns[1] = 30U * kMs;
    pair.open_second();
    QuicConnection& client = pair.client;
    const QuicPathControl& slow = *client.path_control(1U);
    const u64 initial = slow.congestion().window();
    constexpr usize bulk_bytes = 1000U;
    u8 payload[bulk_bytes] = {};
    usize received = 0U;
    bool marked = false; // the slow path's delivery rate marked application-limited once a round trip has passed
    const u64 start_ns = pair.net->now;
    const bool grown = pair.net->run_until(
        [&]
        {
            marked = marked || (pair.net->now > start_ns + 200U * kMs && slow.delivery().app_limited());
            u64 id = 0U;
            while (client.datagrams().queue(ConstSpan<u8>(payload, bulk_bytes), pair.net->now, id, pinned_to(1U)) ==
                   QuicDatagramError::None)
            {
            }
            ConstSpan<u8> held;
            while (pair.server.datagrams().peek(held))
            {
                REQUIRE(held.size() == bulk_bytes);
                ++received;
                pair.server.datagrams().pop();
            }
            return pair.net->now >= start_ns + 1000U * kMs;
        },
        2000U * kMs);
    INFO("window " << slow.congestion().window() << " from " << initial);
    CHECK(grown);
    CHECK(slow.congestion().window() >= 4U * initial);
    CHECK_FALSE(marked);
    CHECK(received != 0U);
    CHECK(client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC connection: a datagram pinned to a dark path gives way once what other paths sent waits on it, so "
          "the live path's datagrams never meet a full queue",
          "[net][net-path][quic-connection][multipath]")
{
    // Path ID 1 goes down after its first pinned datagram: once its probe timeout is pending it is held dark (S78's
    // Scheduling), so the next pinned to it waits unsent at the queue's head, and every plain datagram slot 0 sends
    // waits behind it in the ring until the room is needed.
    MultipathPair pair(2U, 2U, false, true, 256U, true);
    pair.open_second();
    QuicConnection& client = pair.client;
    Arrivals arrivals;
    usize acked = 0U;
    usize lost = 0U;
    const auto settle = [&]
    {
        arrivals.read(pair.server, pair.net->now);
        QuicDatagramReport report;
        while (client.datagrams().poll(report))
        {
            acked += (report.outcome == QuicDatagramOutcome::Acked) ? 1U : 0U;
            lost += (report.outcome == QuicDatagramOutcome::Lost) ? 1U : 0U;
        }
    };
    pair.net->down = 1U << 1U;
    REQUIRE(queue_voice(client, 0U, pair.net->now, pinned_to(1U)) == QuicDatagramError::None);
    REQUIRE(pair.net->run_until(
        [&]
        {
            settle();
            return client.recovery().pto_count(1U) != 0U;
        }));
    REQUIRE(queue_voice(client, 1U, pair.net->now, pinned_to(1U)) == QuicDatagramError::None);
    const usize send_count = datagram_limits(true).send_count;
    for (usize id = 2U; id < send_count + 8U; ++id)
    {
        INFO("datagram " << id);
        const u64 queued_ns = pair.net->now;
        REQUIRE(queue_voice(client, id, queued_ns, QuicDatagramSend{}) == QuicDatagramError::None);
        REQUIRE(pair.net->run_until(
            [&]
            {
                settle();
                return arrivals.copies[id] != 0U;
            }));
        CHECK(arrivals.first_ns[id] - queued_ns <= 21U * kMs);
        pair.net->advance(queued_ns + 20U * kMs);
    }
    // The waiting one gave way exactly once, Lost; the one sent into the outage is Lost by its packet; neither arrived.
    REQUIRE(pair.net->run_until(
        [&]
        {
            settle();
            return acked == send_count + 6U && lost == 2U;
        }));
    CHECK(client.datagrams().counters().unrouted == 1U);
    CHECK(client.abandon_of(1U) == nullptr); // it gave way: its path never stopped
    CHECK(arrivals.each(0U, 2U, 0U));
    CHECK(arrivals.each(2U, send_count + 8U, 1U));
    CHECK(client.state() == QuicConnectionState::Established);
}
