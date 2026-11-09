// crd-net -- the raw UDP path (4.2a; docs/design/network-foundation.md#raw-path): a QUIC connection opened from a
// secret its session holds, with no TLS handshake on the path. Both ends Established at once in both versions, three
// AEADs and every ID length, carrying streams and datagrams after one round trip over clean and hostile paths, keyed
// from a real TLS session's exporter; every refusal of open_keyed() and the secret zeroed each time; another secret, a
// flipped byte, a replay and packets of the handshake's levels opening nothing; the 3x limit until the client
// acknowledges a server's packet; every path through the hello (split and reordered, retransmitted, too long, followed
// by more, unreadable, naming the wrong IDs); key updates, padding, close and the idle timeout.

#include "policy_test_signer.hpp"
#include "quic_connection_fixture.hpp"
#include "quic_host_fixture.hpp"
#include "quic_stream_fixture.hpp"

#include <crd/net/quic_connection.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstring>
#include <memory>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;
using namespace crd::net;
using namespace crd::net::testing;

constexpr u8 kFill = 0x4DU;
constexpr usize kIdBytes = 8U;
constexpr u32 kPayload = 1000U;

QuicConnectionConfig keyed_config(QuicSide side, QuicVersion version = QuicVersion::V1, usize seed = 0U)
{
    QuicConnectionConfig config;
    config.side = side;
    config.version = version;
    config.parameters.max_idle_timeout = 30000U;
    config.entropy_seed = ConstSpan<u8>(kSeeds[seed % 4U], kPrfKeyBytes);
    config.streams = limits();
    config.datagrams.max_frame_bytes = 1100U;
    config.datagrams.receive_bytes = 8192U;
    config.datagrams.receive_count = 16U;
    config.datagrams.send_bytes = 8192U;
    config.datagrams.send_count = 16U;
    config.datagrams.outcomes = true;
    return config;
}

// A session secret and the start it opens; each end needs its own copy, since opening zeroes it.
struct Key
{
    u8 secret[kQuicKeyedSecretBytes] = {};
    QuicKeyedStart start;

    explicit Key(u8 fill = kFill, AlgorithmId aead = AlgorithmId::Aes128Gcm, usize ids = kIdBytes)
    {
        std::memset(secret, fill, sizeof(secret));
        start.secret = Span<u8>(secret, sizeof(secret));
        start.aead = aead;
        start.local_id_bytes = ids;
        start.peer_id_bytes = ids;
    }

    [[nodiscard]] bool zeroed() const
    {
        constexpr u8 none[kQuicKeyedSecretBytes] = {};
        return std::memcmp(secret, none, sizeof(secret)) == 0;
    }
};

void open_pair(QuicConnection& client, QuicConnection& server, AlgorithmId aead = AlgorithmId::Aes128Gcm,
               usize ids = kIdBytes, u64 now = kStartNs)
{
    REQUIRE(client.ready());
    REQUIRE(server.ready());
    Key a(kFill, aead, ids);
    Key b(kFill, aead, ids);
    REQUIRE(client.open_keyed(a.start, now));
    REQUIRE(server.open_keyed(b.start, now));
    CHECK(a.zeroed());
    CHECK(b.zeroed());
}

bool parameters_known(const QuicConnection& client, const QuicConnection& server)
{
    return client.peer_parameters_known() && server.peer_parameters_known();
}

// `size` bytes of `pattern_id` from `out` to `in` on `id`, whole and unchanged.
void carry(Net& net, QuicStreamSet& out, QuicStreamSet& in, u64 id, u64 size, u64 pattern_id)
{
    u64 written = 0U;
    u64 read = 0U;
    bool fin = false;
    REQUIRE(net.run_until(
        [&]
        {
            static_cast<void>(write_pattern(out, id, written, size, pattern_id));
            fin = fin || (in.recv_state(id) != QuicRecvState::None && drain(in, id, read, pattern_id));
            return fin;
        }));
    CHECK(read == size);
}

// Each end's first stream, in both directions, larger than every window; and datagrams each way, each once.
void carry_both_ways(Net& net, QuicConnection& client, QuicConnection& server)
{
    u64 from_client = 0U;
    u64 from_server = 0U;
    REQUIRE(client.streams().open(true, from_client) == QuicStreamError::None);
    REQUIRE(server.streams().open(true, from_server) == QuicStreamError::None);
    carry(net, client.streams(), server.streams(), from_client, 100000U, 3U);
    carry(net, server.streams(), client.streams(), from_server, 100000U, 4U);
}

void exchange_datagrams(Net& net, QuicConnection& from, QuicConnection& to)
{
    u8 payload[kPayload];
    std::memset(payload, 0xD6, sizeof(payload));
    u64 first = 0U;
    REQUIRE(from.datagrams().queue(ConstSpan<u8>(payload), net.now, first) == QuicDatagramError::None);
    ConstSpan<u8> held;
    REQUIRE(net.run_until([&] { return to.datagrams().peek(held); }));
    REQUIRE(held.size() == sizeof(payload));
    CHECK(std::memcmp(held.data(), payload, sizeof(payload)) == 0);
    to.datagrams().pop();
    QuicDatagramReport report;
    REQUIRE(net.run_until([&] { return from.datagrams().poll(report); }));
    CHECK(report.id == first);
    CHECK(report.outcome == QuicDatagramOutcome::Acked);
}

// A peer holding the secret that writes any frame the test asks for: the client of the connection it talks to.
// (MSVC C4324 as for QuicPacketProtection, which it holds inline.)
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct KeyedPeer
{
    QuicPacketProtection protection;
    u8 client_id[kIdBytes] = {};
    u8 server_id[kIdBytes] = {};
    u64 number = 0U;
    u64 offset = 0U;
    u64 now = kStartNs;

    QuicSide own;

    explicit KeyedPeer(QuicSide side_of_peer = QuicSide::Client) : own(side_of_peer)
    {
        Key key;
        const ConstSpan<u8> secret(key.secret, sizeof(key.secret));
        protection.start_keyed(own, QuicVersion::V1);
        for (const QuicSide side : {QuicSide::Client, QuicSide::Server})
        {
            u8 traffic[kQuicMaxSecretBytes];
            usize bytes = 0U;
            REQUIRE(quic_keyed_traffic_secret(AlgorithmId::Aes128Gcm, secret, side, Span<u8>(traffic, sizeof(traffic)),
                                              bytes) == CryptoError::None);
            REQUIRE(protection.install(QuicLevel::Application, side == own, AlgorithmId::Aes128Gcm,
                                       ConstSpan<u8>(traffic, bytes)) == CryptoError::None);
        }
        protection.confirm();
        REQUIRE(quic_keyed_connection_id(secret, QuicSide::Client, Span<u8>(client_id, sizeof(client_id))) ==
                CryptoError::None);
        REQUIRE(quic_keyed_connection_id(secret, QuicSide::Server, Span<u8>(server_id, sizeof(server_id))) ==
                CryptoError::None);
    }

    // One 1-RTT packet holding `frames`, to `to`.
    void send(QuicConnection& to, ConstSpan<u8> frames)
    {
        u8 datagram[kDatagram] = {};
        QuicPacketWriter writer;
        QuicPacketSpec spec;
        spec.type = QuicPacketType::OneRtt;
        spec.version = static_cast<u32>(QuicVersion::V1);
        spec.dcid = (own == QuicSide::Client) ? ConstSpan<u8>(server_id, sizeof(server_id))
                                              : ConstSpan<u8>(client_id, sizeof(client_id));
        spec.number = number++;
        REQUIRE(writer.begin(spec, Span<u8>(datagram, kQuicMinInitialDatagram)));
        std::memcpy(writer.frames().data(), frames.data(), frames.size());
        usize size = 0U;
        REQUIRE(protection.seal(writer, frames.size(), {0U, 0U}, size) == QuicPacketError::None);
        to.receive(Span<u8>(datagram, size), QuicEcn::NotEct, now);
    }

    // CRYPTO bytes `data` at `at`, to `to`.
    void crypto_at(QuicConnection& to, u64 at, ConstSpan<u8> data)
    {
        QuicFrame frame;
        frame.kind = QuicFrameKind::Crypto;
        frame.offset = at;
        frame.data = data;
        u8 frames[1100];
        const usize bytes = write_quic_frame(frame, Span<u8>(frames, sizeof(frames)));
        REQUIRE(bytes != 0U);
        send(to, ConstSpan<u8>(frames, bytes));
    }

    // A client's hello: its parameters with its sequence 0 ID (or `id` instead), then `extra` bytes.
    [[nodiscard]] usize hello(Span<u8> out, ConstSpan<u8> id = {}, usize extra = 0U) const
    {
        QuicTransportParameters parameters;
        parameters.max_idle_timeout = 30000U;
        parameters.has_initial_source_connection_id = true;
        REQUIRE(parameters.initial_source_connection_id.assign(id.empty() ? ConstSpan<u8>(client_id, sizeof(client_id))
                                                                          : id));
        const usize bytes = write_quic_transport_parameters(parameters, QuicSide::Client,
                                                            QuicTransportParameterGrease{9U}, out.subspan(2U));
        REQUIRE(bytes != 0U);
        out[0] = static_cast<u8>(bytes >> 8U);
        out[1] = static_cast<u8>(bytes);
        return 2U + bytes + extra;
    }
};
#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif

void check_closed_with(const QuicConnection& connection, QuicTransportError error)
{
    CHECK(connection.state() == QuicConnectionState::Closing);
    CHECK(connection.close_info().local);
    CHECK(connection.close_info().error_code == static_cast<u64>(error));
}
} // namespace

TEST_CASE("Raw UDP path: both ends are up at once and carry streams and datagrams after one round trip",
          "[net][4.2a][raw-path]")
{
    const QuicVersion version = GENERATE(QuicVersion::V1, QuicVersion::V2);
    const AlgorithmId aead = GENERATE(AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305);
    const usize ids = GENERATE(usize{8U}, usize{20U});
    CountingAllocator allocator;
    QuicConnection client(allocator, keyed_config(QuicSide::Client, version, 0U));
    QuicConnection server(allocator, keyed_config(QuicSide::Server, version, 1U));
    open_pair(client, server, aead, ids);
    CHECK(client.state() == QuicConnectionState::Established);
    CHECK(server.state() == QuicConnectionState::Established);
    CHECK(client.confirmed());
    CHECK(server.confirmed());
    CHECK(client.protection().can_send(QuicPacketType::OneRtt));
    CHECK_FALSE(client.protection().can_send(QuicPacketType::Initial));
    CHECK_FALSE(client.protection().can_send(QuicPacketType::Handshake));

    const usize before = allocator.calls();
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return parameters_known(client, server); }));
    CHECK(net->now - kStartNs <= 2U * 10U * kMs); // the two 10 ms paths: one round trip
    CHECK(client.peer_parameters().initial_max_data != 0U);
    CHECK(server.peer_parameters().initial_max_data != 0U);

    carry_both_ways(*net, client, server);
    exchange_datagrams(*net, client, server);
    exchange_datagrams(*net, server, client);
    CHECK(net->amplification_kept);
    CHECK(allocator.calls() == before);
}

TEST_CASE("Raw UDP path: paths that lose, duplicate and reorder carry everything, within the amplification limit",
          "[net][4.2a][raw-path]")
{
    const u64 seed = GENERATE(1U, 2U, 3U, 4U, 5U, 6U);
    CountingAllocator allocator;
    QuicConnection client(allocator, keyed_config(QuicSide::Client, QuicVersion::V1, 0U));
    QuicConnection server(allocator, keyed_config(QuicSide::Server, QuicVersion::V1, 1U));
    open_pair(client, server);
    auto net = std::make_unique<Net>(client, server);
    net->rng.state = seed;
    net->path[0] = Path{150U, 80U, 15U * kMs, 12U * kMs};
    net->path[1] = Path{150U, 80U, 15U * kMs, 12U * kMs};
    REQUIRE(net->run_until([&] { return parameters_known(client, server); }));
    carry_both_ways(*net, client, server);
    CHECK(net->dropped[0] != 0U);
    CHECK(net->amplification_kept);
    CHECK(client.state() == QuicConnectionState::Established);
    CHECK(server.state() == QuicConnectionState::Established);
}

TEST_CASE("Raw UDP path: a real TLS session's exporter keys it", "[net][4.2a][raw-path]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnection tls_client(allocator, config_for(QuicSide::Client, identity, QuicVersion::V1, 0U));
    QuicConnection tls_server(allocator, config_for(QuicSide::Server, identity, QuicVersion::V1, 1U));
    constexpr u8 client_id[8] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7};
    constexpr u8 first_dcid[8] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7};
    REQUIRE(tls_client.connect(ConstSpan<u8>(client_id, 8U), ConstSpan<u8>(first_dcid, 8U), kStartNs));
    auto handshake = std::make_unique<Net>(tls_client, tls_server);
    REQUIRE(handshake->run_until([&] { return tls_client.confirmed() && tls_server.confirmed(); }));

    constexpr u8 context[] = {0x01U};
    u8 client_secret[kQuicKeyedSecretBytes];
    u8 server_secret[kQuicKeyedSecretBytes];
    u8 other_context[kQuicKeyedSecretBytes];
    REQUIRE(tls_client.handshake().export_keying_material("EXPERIMENTAL cerid raw path", ConstSpan<u8>(context),
                                                          Span<u8>(client_secret, sizeof(client_secret))) ==
            TlsError::None);
    REQUIRE(tls_server.handshake().export_keying_material("EXPERIMENTAL cerid raw path", ConstSpan<u8>(context),
                                                          Span<u8>(server_secret, sizeof(server_secret))) ==
            TlsError::None);
    constexpr u8 next_context[] = {0x02U};
    REQUIRE(tls_server.handshake().export_keying_material("EXPERIMENTAL cerid raw path", ConstSpan<u8>(next_context),
                                                          Span<u8>(other_context, sizeof(other_context))) ==
            TlsError::None);
    CHECK(std::memcmp(client_secret, other_context, sizeof(client_secret)) != 0);

    QuicConnection client(allocator, keyed_config(QuicSide::Client, QuicVersion::V1, 2U));
    QuicConnection server(allocator, keyed_config(QuicSide::Server, QuicVersion::V1, 3U));
    QuicKeyedStart start;
    start.aead = tls_client.handshake().aead();
    start.secret = Span<u8>(client_secret, sizeof(client_secret));
    REQUIRE(client.open_keyed(start, kStartNs));
    start.secret = Span<u8>(server_secret, sizeof(server_secret));
    REQUIRE(server.open_keyed(start, kStartNs));
    auto net = std::make_unique<Net>(client, server);
    REQUIRE(net->run_until([&] { return parameters_known(client, server); }));
    carry_both_ways(*net, client, server);

    // The next context's secret opens nothing of a connection keyed by the first.
    QuicConnection stranger(allocator, keyed_config(QuicSide::Server, QuicVersion::V1, 1U));
    QuicKeyedStart other;
    other.aead = start.aead;
    other.secret = Span<u8>(other_context, sizeof(other_context));
    REQUIRE(stranger.open_keyed(other, kStartNs));
    client.ping();
    u8 datagram[kDatagram];
    const usize sent = client.send(Span<u8>(datagram, sizeof(datagram)), net->now);
    REQUIRE(sent != 0U);
    stranger.receive(Span<u8>(datagram, sent), QuicEcn::NotEct, net->now);
    CHECK(stranger.counters().packets_opened == 0U);
}

TEST_CASE("Raw UDP path: every refused start leaves the connection idle and the secret zeroed", "[net][4.2a][raw-path]")
{
    CountingAllocator allocator;

    SECTION("a connection built with a TLS context")
    {
        Identity identity;
        QuicConnection server(allocator, config_for(QuicSide::Server, identity, QuicVersion::V1, 1U));
        Key key;
        CHECK_FALSE(server.open_keyed(key.start, kStartNs));
        CHECK(key.zeroed());
        CHECK(server.state() == QuicConnectionState::Idle);
    }
    SECTION("a connection built without one neither connects nor accepts")
    {
        QuicConnection client(allocator, keyed_config(QuicSide::Client));
        QuicConnection server(allocator, keyed_config(QuicSide::Server));
        constexpr u8 id[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
        CHECK_FALSE(client.connect(ConstSpan<u8>(id, 8U), ConstSpan<u8>(id, 8U), kStartNs));
        QuicAccept accept;
        accept.local_cid = ConstSpan<u8>(id, 8U);
        accept.client_dcid = ConstSpan<u8>(id, 8U);
        accept.client_scid = ConstSpan<u8>(id, 8U);
        accept.original_dcid = ConstSpan<u8>(id, 8U);
        CHECK_FALSE(server.accept(accept, kStartNs));
    }
    SECTION("nothing of a handshake may be configured without one")
    {
        constexpr u8 bytes[4] = {1U, 2U, 3U, 4U};
        constexpr u32 versions[1] = {static_cast<u32>(QuicVersion::V1)};
        QuicConnectionConfig config = keyed_config(QuicSide::Client);
        config.token = ConstSpan<u8>(bytes, 4U);
        CHECK_FALSE(QuicConnection(allocator, config).ready());
        config = keyed_config(QuicSide::Server);
        config.early_context = ConstSpan<u8>(bytes, 4U);
        CHECK_FALSE(QuicConnection(allocator, config).ready());
        config = keyed_config(QuicSide::Client);
        config.versions = ConstSpan<u32>(versions, 1U);
        CHECK_FALSE(QuicConnection(allocator, config).ready());
        config = keyed_config(QuicSide::Client);
        config.after_version_negotiation = true;
        CHECK_FALSE(QuicConnection(allocator, config).ready());
        Tickets tickets;
        config = keyed_config(QuicSide::Client);
        config.tickets = &tickets;
        CHECK_FALSE(QuicConnection(allocator, config).ready());
    }
    SECTION("a start out of range")
    {
        QuicConnection client(allocator, keyed_config(QuicSide::Client));
        REQUIRE(client.ready());
        Key short_secret;
        short_secret.start.secret = short_secret.start.secret.first(31U);
        CHECK_FALSE(client.open_keyed(short_secret.start, kStartNs));
        CHECK(short_secret.secret[0] == 0U);
        Key no_aead(kFill, AlgorithmId::X25519);
        CHECK_FALSE(client.open_keyed(no_aead.start, kStartNs));
        CHECK(no_aead.zeroed());
        Key small_local;
        small_local.start.local_id_bytes = 7U;
        CHECK_FALSE(client.open_keyed(small_local.start, kStartNs));
        Key long_peer;
        long_peer.start.peer_id_bytes = 21U;
        CHECK_FALSE(client.open_keyed(long_peer.start, kStartNs));
        CHECK(long_peer.zeroed());
        CHECK(client.state() == QuicConnectionState::Idle);
        Key good;
        CHECK(client.open_keyed(good.start, kStartNs));
        Key twice;
        CHECK_FALSE(client.open_keyed(twice.start, kStartNs));
        CHECK(twice.zeroed());
        CHECK(client.state() == QuicConnectionState::Established);
    }
    SECTION("an AEAD the policy refuses, as a TLS handshake refuses it")
    {
        constexpr u64 now = 1800000000U;
        test::TestInstallation installation;
        PolicyContent content = test::accepting_all(1U, now - 60U);
        test::refuse_rule(content, AlgorithmId::Aes128Gcm);
        REQUIRE(installation.apply(content, now) == PolicyError::None);
        QuicConnection client(allocator, keyed_config(QuicSide::Client));
        Key refused;
        refused.start.policy = &installation.store;
        refused.start.unix_seconds = now;
        CHECK_FALSE(client.open_keyed(refused.start, kStartNs));
        CHECK(refused.zeroed());
        Key accepted(kFill, AlgorithmId::Chacha20Poly1305);
        accepted.start.policy = &installation.store;
        accepted.start.unix_seconds = now;
        CHECK(client.open_keyed(accepted.start, kStartNs));
    }
}

TEST_CASE("Raw UDP path: another secret, a flipped byte, a replay and the handshake's levels open nothing",
          "[net][4.2a][raw-path]")
{
    CountingAllocator allocator;
    QuicConnection client(allocator, keyed_config(QuicSide::Client, QuicVersion::V1, 0U));
    QuicConnection server(allocator, keyed_config(QuicSide::Server, QuicVersion::V1, 1U));
    open_pair(client, server);
    u8 first[kDatagram];
    const usize size = client.send(Span<u8>(first, sizeof(first)), kStartNs);
    REQUIRE(size != 0U);
    CHECK((first[0] & 0x80U) == 0U); // a short header: nothing of a handshake is ever on the wire
    u64 now = kStartNs + kMs;

    SECTION("another secret")
    {
        Key other(0x77U);
        QuicConnection stranger(allocator, keyed_config(QuicSide::Server, QuicVersion::V1, 2U));
        REQUIRE(stranger.open_keyed(other.start, now));
        stranger.receive(Span<u8>(first, size), QuicEcn::NotEct, now);
        CHECK(stranger.counters().packets_opened == 0U);
        CHECK_FALSE(stranger.peer_parameters_known());
    }
    SECTION("each byte flipped")
    {
        for (usize at = 0U; at < size; ++at)
        {
            u8 copy[kDatagram];
            std::memcpy(copy, first, size);
            copy[at] ^= 0x40U;
            server.receive(Span<u8>(copy, size), QuicEcn::NotEct, now);
        }
        CHECK(server.counters().packets_opened == 0U);
        CHECK_FALSE(server.peer_parameters_known());
        CHECK(server.state() == QuicConnectionState::Established);
    }
    SECTION("a replay")
    {
        u8 copy[kDatagram];
        std::memcpy(copy, first, size);
        server.receive(Span<u8>(first, size), QuicEcn::NotEct, now);
        CHECK(server.counters().packets_opened == 1U);
        const u64 dropped = server.counters().packets_dropped;
        server.receive(Span<u8>(copy, size), QuicEcn::NotEct, now);
        CHECK(server.counters().packets_opened == 1U);
        CHECK(server.counters().packets_dropped == dropped + 1U);
    }
    SECTION("an Initial anyone can seal")
    {
        KeyedPeer peer;
        QuicPacketProtection initial;
        REQUIRE(initial.start(QuicSide::Client, QuicVersion::V1, ConstSpan<u8>(peer.server_id, kIdBytes)) ==
                CryptoError::None);
        u8 datagram[kDatagram] = {};
        QuicPacketWriter writer;
        QuicPacketSpec spec;
        spec.type = QuicPacketType::Initial;
        spec.version = static_cast<u32>(QuicVersion::V1);
        spec.dcid = ConstSpan<u8>(peer.server_id, kIdBytes);
        spec.scid = ConstSpan<u8>(peer.client_id, kIdBytes);
        REQUIRE(writer.begin(spec, Span<u8>(datagram, kQuicMinInitialDatagram)));
        const u8 ping = 0x01U;
        std::memcpy(writer.frames().data(), &ping, 1U);
        usize written = 0U;
        REQUIRE(initial.seal(writer, 1U, {kQuicMinInitialDatagram, 0U}, written) == QuicPacketError::None);
        server.receive(Span<u8>(datagram, written), QuicEcn::NotEct, now);
        CHECK(server.counters().packets_opened == 0U);
        CHECK(server.counters().packets_dropped >= 1U);
        CHECK(server.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("Raw UDP path: a server holds no 0-RTT packet for keys that never come", "[net][4.2a][raw-path]")
{
    CountingAllocator allocator;
    QuicConnection server(allocator, keyed_config(QuicSide::Server, QuicVersion::V1, 1U));
    Key key;
    REQUIRE(server.open_keyed(key.start, kStartNs));
    KeyedPeer peer;
    u8 datagram[kDatagram] = {};
    usize at = 0U;
    datagram[at++] = 0xD0U; // a long header of type 0-RTT
    datagram[at++] = 0U;
    datagram[at++] = 0U;
    datagram[at++] = 0U;
    datagram[at++] = 1U; // version 1
    datagram[at++] = kIdBytes;
    std::memcpy(datagram + at, peer.server_id, kIdBytes);
    at += kIdBytes;
    datagram[at++] = kIdBytes;
    std::memcpy(datagram + at, peer.client_id, kIdBytes);
    at += kIdBytes;
    datagram[at++] = 32U; // its Length
    at += 32U;
    server.receive(Span<u8>(datagram, at), QuicEcn::NotEct, kStartNs);
    CHECK(server.counters().packets_held == 0U);
    CHECK(server.counters().packets_dropped == 1U);
    CHECK(server.state() == QuicConnectionState::Established);
}

TEST_CASE("Raw UDP path: a server sends at most three times what it received until a packet of its own is "
          "acknowledged",
          "[net][4.2a][raw-path]")
{
    CountingAllocator allocator;
    QuicConnectionConfig server_config = keyed_config(QuicSide::Server, QuicVersion::V1, 1U);
    server_config.padding_block = 1024U; // what it sends is larger than what it received
    QuicConnection client(allocator, keyed_config(QuicSide::Client, QuicVersion::V1, 0U));
    QuicConnection server(allocator, server_config);
    open_pair(client, server);
    CHECK(client.address_validated());
    CHECK_FALSE(server.address_validated());
    u8 datagram[kDatagram];
    CHECK(server.send(Span<u8>(datagram, sizeof(datagram)), kStartNs) == 0U); // it knows of no one yet

    const usize first = client.send(Span<u8>(datagram, sizeof(datagram)), kStartNs);
    REQUIRE(first != 0U);
    server.receive(Span<u8>(datagram, first), QuicEcn::NotEct, kStartNs);
    CHECK_FALSE(server.address_validated());
    usize sent = 0U;
    for (usize size = server.send(Span<u8>(datagram, sizeof(datagram)), kStartNs); size != 0U;
         size = server.send(Span<u8>(datagram, sizeof(datagram)), kStartNs))
    {
        sent += size;
        client.receive(Span<u8>(datagram, size), QuicEcn::NotEct, kStartNs);
    }
    CHECK(sent != 0U);
    CHECK(sent <= 3U * first);
    CHECK_FALSE(server.address_validated());

    client.ping();
    const usize answer = client.send(Span<u8>(datagram, sizeof(datagram)), kStartNs + kMs);
    REQUIRE(answer != 0U);
    server.receive(Span<u8>(datagram, answer), QuicEcn::NotEct, kStartNs + kMs);
    CHECK(server.address_validated());
}

TEST_CASE("Raw UDP path: the hello is read whole and only once", "[net][4.2a][raw-path]")
{
    CountingAllocator allocator;
    QuicConnection server(allocator, keyed_config(QuicSide::Server, QuicVersion::V1, 1U));
    Key key;
    REQUIRE(server.open_keyed(key.start, kStartNs));
    KeyedPeer peer;
    u8 hello[256] = {};

    SECTION("split, its second half first, and retransmitted")
    {
        const usize bytes = peer.hello(Span<u8>(hello, sizeof(hello)));
        peer.crypto_at(server, 5U, ConstSpan<u8>(hello + 5U, bytes - 5U));
        CHECK_FALSE(server.peer_parameters_known());
        peer.crypto_at(server, 0U, ConstSpan<u8>(hello, 1U));
        CHECK_FALSE(server.peer_parameters_known());
        peer.crypto_at(server, 1U, ConstSpan<u8>(hello + 1U, 4U));
        CHECK(server.peer_parameters_known());
        CHECK(server.state() == QuicConnectionState::Established);
        peer.crypto_at(server, 0U, ConstSpan<u8>(hello, bytes));
        CHECK(server.state() == QuicConnectionState::Established);
    }
    SECTION("longer than any parameters")
    {
        hello[0] = 0x10U;
        hello[1] = 0x01U;
        peer.crypto_at(server, 0U, ConstSpan<u8>(hello, 2U));
        check_closed_with(server, QuicTransportError::TransportParameterError);
    }
    SECTION("followed by more in the same data")
    {
        const usize bytes = peer.hello(Span<u8>(hello, sizeof(hello)), {}, 1U);
        peer.crypto_at(server, 0U, ConstSpan<u8>(hello, bytes));
        check_closed_with(server, QuicTransportError::ProtocolViolation);
    }
    SECTION("followed by more later")
    {
        const usize bytes = peer.hello(Span<u8>(hello, sizeof(hello)));
        peer.crypto_at(server, 0U, ConstSpan<u8>(hello, bytes));
        REQUIRE(server.peer_parameters_known());
        peer.crypto_at(server, bytes, ConstSpan<u8>(hello, 1U));
        check_closed_with(server, QuicTransportError::ProtocolViolation);
    }
    SECTION("unreadable parameters")
    {
        constexpr u8 broken[] = {0x00U, 0x03U, 0x01U, 0x09U, 0x00U};
        peer.crypto_at(server, 0U, ConstSpan<u8>(broken));
        check_closed_with(server, QuicTransportError::TransportParameterError);
    }
    SECTION("naming another connection ID")
    {
        constexpr u8 other[kIdBytes] = {0xEEU};
        const usize bytes = peer.hello(Span<u8>(hello, sizeof(hello)), ConstSpan<u8>(other, kIdBytes));
        peer.crypto_at(server, 0U, ConstSpan<u8>(hello, bytes));
        check_closed_with(server, QuicTransportError::ProtocolViolation);
    }
    SECTION("past the room for it")
    {
        u8 frames[1100];
        QuicFrame frame;
        frame.kind = QuicFrameKind::Crypto;
        frame.offset = 16384U;
        frame.data = ConstSpan<u8>(hello, 16U);
        const usize written = write_quic_frame(frame, Span<u8>(frames, sizeof(frames)));
        peer.send(server, ConstSpan<u8>(frames, written));
        check_closed_with(server, QuicTransportError::CryptoBufferExceeded);
    }
}

TEST_CASE("Raw UDP path: a NEW_CONNECTION_ID reordered before the hello keeps its own token", "[net][4.2a][raw-path]")
{
    CountingAllocator allocator;
    ConnectionIdTable routes(allocator, 16U, ConstSpan<u8>(kHostPrf));
    ConnectionIdTable tokens(allocator, kQuicMaxPeerConnectionIds, ConstSpan<u8>(kHostPrf));
    QuicResetKey reset;
    REQUIRE(reset.init(ConstSpan<u8>(kResetSecret)) == CryptoError::None);
    QuicRoutedIds ids(routes, reset, 0U, &tokens);
    QuicConnectionConfig config = keyed_config(QuicSide::Client);
    config.ids = &ids;
    QuicConnection client(allocator, config);
    Key key;
    REQUIRE(client.open_keyed(key.start, kStartNs));
    u8 datagram[kDatagram];
    REQUIRE(client.send(Span<u8>(datagram, sizeof(datagram)), kStartNs) != 0U); // sequence 0 is now in use

    KeyedPeer peer(QuicSide::Server);
    const u8 next_id[kIdBytes] = {0xA1U, 0xA2U, 0xA3U, 0xA4U, 0xA5U, 0xA6U, 0xA7U, 0xA8U};
    u8 next_token[kQuicResetTokenBytes];
    std::memset(next_token, 0x71, sizeof(next_token));
    QuicFrame issue;
    issue.kind = QuicFrameKind::NewConnectionId;
    issue.sequence = 1U;
    issue.retire_prior_to = 1U;
    issue.data = ConstSpan<u8>(next_id, kIdBytes);
    issue.reset_token = ConstSpan<u8>(next_token, sizeof(next_token));
    u8 frames[256];
    const usize bytes = write_quic_frame(issue, Span<u8>(frames, sizeof(frames)));
    REQUIRE(bytes != 0U);
    peer.send(client, ConstSpan<u8>(frames, bytes));

    QuicTransportParameters parameters;
    parameters.max_idle_timeout = 30000U;
    parameters.has_initial_source_connection_id = true;
    REQUIRE(parameters.initial_source_connection_id.assign(ConstSpan<u8>(peer.server_id, kIdBytes)));
    parameters.has_original_destination_connection_id = true;
    REQUIRE(parameters.original_destination_connection_id.assign(ConstSpan<u8>(peer.server_id, kIdBytes)));
    parameters.has_stateless_reset_token = true;
    std::memset(parameters.stateless_reset_token, 0x72, kQuicResetTokenBytes);
    u8 hello[256];
    const usize length = write_quic_transport_parameters(parameters, QuicSide::Server, QuicTransportParameterGrease{9U},
                                                         Span<u8>(hello + 2U, sizeof(hello) - 2U));
    REQUIRE(length != 0U);
    hello[0] = static_cast<u8>(length >> 8U);
    hello[1] = static_cast<u8>(length);
    peer.crypto_at(client, 0U, ConstSpan<u8>(hello, 2U + length));
    REQUIRE(client.peer_parameters_known());
    CHECK(client.state() == QuicConnectionState::Established);

    // Sequence 1 is in use from the next packet on; its token alone resets, sequence 0's is gone with it.
    client.ping();
    REQUIRE(client.send(Span<u8>(datagram, sizeof(datagram)), kStartNs + kMs) != 0U);
    CHECK(tokens.size() == 1U);
    u8 reset_datagram[48];
    std::memset(reset_datagram, 0x3C, sizeof(reset_datagram));
    reset_datagram[0] = 0x4BU;
    std::memset(reset_datagram + sizeof(reset_datagram) - kQuicResetTokenBytes, 0x72, kQuicResetTokenBytes);
    CHECK_FALSE(client.stateless_reset(ConstSpan<u8>(reset_datagram), kStartNs + kMs));
    std::memset(reset_datagram + sizeof(reset_datagram) - kQuicResetTokenBytes, 0x71, kQuicResetTokenBytes);
    CHECK(client.stateless_reset(ConstSpan<u8>(reset_datagram), kStartNs + kMs));
    CHECK(client.state() == QuicConnectionState::Draining);
}

TEST_CASE("Raw UDP path: keys update, packets pad, a close travels and silence ends it", "[net][4.2a][raw-path]")
{
    CountingAllocator allocator;

    SECTION("key updates at an interval, and the previous key goes")
    {
        QuicConnectionConfig client_config = keyed_config(QuicSide::Client, QuicVersion::V1, 0U);
        QuicConnectionConfig server_config = keyed_config(QuicSide::Server, QuicVersion::V1, 1U);
        client_config.key_update_interval = 64U;
        server_config.key_update_interval = 64U;
        QuicConnection client(allocator, client_config);
        QuicConnection server(allocator, server_config);
        open_pair(client, server);
        auto net = std::make_unique<Net>(client, server);
        REQUIRE(net->run_until([&] { return parameters_known(client, server); }));
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
        CHECK(client.state() == QuicConnectionState::Established);
    }
    SECTION("the padding block")
    {
        QuicConnectionConfig client_config = keyed_config(QuicSide::Client, QuicVersion::V1, 0U);
        client_config.padding_block = 128U;
        QuicConnection client(allocator, client_config);
        QuicConnection server(allocator, keyed_config(QuicSide::Server, QuicVersion::V1, 1U));
        open_pair(client, server);
        u8 datagram[kDatagram];
        const usize size = client.send(Span<u8>(datagram, sizeof(datagram)), kStartNs);
        REQUIRE(size != 0U);
        CHECK(size % 128U == 0U);
    }
    SECTION("an application close reaches the peer with its code and reason")
    {
        QuicConnection client(allocator, keyed_config(QuicSide::Client, QuicVersion::V1, 0U));
        QuicConnection server(allocator, keyed_config(QuicSide::Server, QuicVersion::V1, 1U));
        open_pair(client, server);
        auto net = std::make_unique<Net>(client, server);
        REQUIRE(net->run_until([&] { return parameters_known(client, server); }));
        constexpr u8 reason[] = {'b', 'y', 'e'};
        client.close(7U, ConstSpan<u8>(reason), net->now);
        REQUIRE(net->run_until([&] { return server.state() == QuicConnectionState::Draining; }));
        CHECK(server.close_info().application);
        CHECK(server.close_info().error_code == 7U);
        REQUIRE(server.close_reason().size() == sizeof(reason));
        CHECK(std::memcmp(server.close_reason().data(), reason, sizeof(reason)) == 0);
        REQUIRE(net->run_until(
            [&]
            {
                return client.state() == QuicConnectionState::Closed && server.state() == QuicConnectionState::Closed;
            }));
    }
    SECTION("a server no client ever reaches, and a client whose server never answers, end at the idle timeout")
    {
        QuicConnection client(allocator, keyed_config(QuicSide::Client, QuicVersion::V1, 0U));
        QuicConnection server(allocator, keyed_config(QuicSide::Server, QuicVersion::V1, 1U));
        open_pair(client, server);
        for (u64 now = kStartNs; server.state() != QuicConnectionState::Closed; now += 1000U * kMs)
        {
            REQUIRE(now < kStartNs + 60000U * kMs);
            u8 datagram[kDatagram];
            CHECK(server.send(Span<u8>(datagram, sizeof(datagram)), now) == 0U);
            static_cast<void>(client.send(Span<u8>(datagram, sizeof(datagram)), now));
            server.on_timeout(now);
            client.on_timeout(now);
        }
        CHECK(server.close_info().idle);
    }
}
