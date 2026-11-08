// crd-net -- a QUIC connection (4.2a; docs/design/network-foundation.md#quic-connection): the handshake carried to
// confirmation in both versions over a clean path and over seeded paths that lose, duplicate and reorder, the
// anti-amplification limit, probes recovering lost flights, HANDSHAKE_DONE resent, key discards, the idle timeout and
// keep-alives, key updates across the connection, closing and draining, an alert's CRYPTO_ERROR at its levels, the
// transport parameter and version checks, a client's Version Negotiation, and, from a raw peer that writes any frame,
// every frame this slice refuses with the error RFC 9000 and RFC 9221 name, the peer's connection IDs and their
// retirement, PATH_CHALLENGE answered and an acknowledgement of a skipped packet number; and streams carried both ways
// over hostile paths with a frame ledger far smaller than their load.

#include "ingress_fixture.hpp"
#include "quic_stream_fixture.hpp"

#include <crd/crypto/tls.hpp>
#include <crd/net/quic_connection.hpp>

#include <catch2/catch_test_macros.hpp>
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
using crd::containers::StringView;
using crd::containers::WireReader;
using namespace crd::crypto;
using namespace crd::net;
using namespace crd::net::testing;

constexpr u8 kClientCid[8] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7};
constexpr u8 kServerCid[16] = {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
                               0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F};
constexpr u8 kInitialDcid[8] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7};
constexpr AlgorithmId kGroups[] = {AlgorithmId::X25519Mlkem768, AlgorithmId::X25519};
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519};
constexpr AlgorithmId kAeads[] = {AlgorithmId::Aes128Gcm, AlgorithmId::Chacha20Poly1305};
constexpr StringView kProtocols[] = {"cerid-game/1"};
constexpr const char* kServerName = "game.cerid.test";
constexpr QuicVersion kVersions[] = {QuicVersion::V1, QuicVersion::V2};
constexpr u64 kStartNs = 1000000000U;
constexpr u64 kMs = 1000000U;
constexpr usize kDatagram = 1500U;

// A server key and the client and server contexts that trust it; `wrong_pin` makes the client pin another key.
struct Identity
{
    SigningKey key;
    SigningKey other;
    TlsCredential credential;
    TlsContext client;
    TlsContext server;

    explicit Identity(bool wrong_pin = false)
    {
        u8 seed[32];
        for (usize i = 0U; i < sizeof(seed); ++i)
        {
            seed[i] = static_cast<u8>(i + 1U);
        }
        REQUIRE(key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed, sizeof(seed))) == CryptoError::None);
        seed[0] = 0xEEU;
        REQUIRE(other.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed, sizeof(seed))) == CryptoError::None);
        REQUIRE(credential.init(key, kServerName) == TlsError::None);
        TlsCredential stranger;
        REQUIRE(stranger.init(other, kServerName) == TlsError::None);
        const KeyId pin = wrong_pin ? stranger.key_id() : credential.key_id();

        TlsConfig client_config;
        client_config.role = TlsRole::Client;
        client_config.groups = kGroups;
        client_config.signatures = kSignatures;
        client_config.aeads = kAeads;
        client_config.pinned = ConstSpan<KeyId>(&pin, 1U);
        client_config.server_name = kServerName;
        client_config.application_protocols = kProtocols;
        REQUIRE(client.init(client_config) == TlsError::None);

        TlsConfig server_config;
        server_config.role = TlsRole::Server;
        server_config.groups = kGroups;
        server_config.aeads = kAeads;
        server_config.credential = &credential;
        server_config.server_name = kServerName;
        server_config.application_protocols = kProtocols;
        REQUIRE(server.init(server_config) == TlsError::None);
    }
};

constexpr u8 kSeeds[4][kPrfKeyBytes] = {{0x01}, {0x02}, {0x03}, {0x04}};

QuicConnectionConfig config_for(QuicSide side, const Identity& identity, QuicVersion version, usize seed = 0U)
{
    QuicConnectionConfig config;
    config.side = side;
    config.version = version;
    config.tls = (side == QuicSide::Client) ? &identity.client : &identity.server;
    config.parameters.max_idle_timeout = 30000U;
    config.entropy_seed = ConstSpan<u8>(kSeeds[seed % 4U], kPrfKeyBytes);
    return config;
}

// splitmix64: the seeded paths' impairments.
struct Rng
{
    u64 state = 0U;

    u64 next()
    {
        u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    bool chance(u32 permille) { return (next() % 1000U) < permille; }
};

struct Path
{
    u32 loss = 0U;      // permille
    u32 duplicate = 0U; // permille
    u64 delay_ns = 10U * kMs;
    u64 jitter_ns = 0U; // a uniform extra delay, which reorders
};

// Decides per datagram whether the path drops it beyond the random loss: `from` 0 is the client.
class Filter
{
public:
    virtual ~Filter() = default;
    virtual bool drop(usize from, ConstSpan<u8> datagram) = 0;

protected:
    Filter() = default;
    Filter(const Filter&) = default;
    Filter(Filter&&) noexcept = default;
    Filter& operator=(const Filter&) = default;
    Filter& operator=(Filter&&) noexcept = default;
};

// A client and a server joined by two impaired paths on a virtual clock. The server accepts the first client datagram.
class Net
{
public:
    static constexpr usize kFlights = 512U;

    Net(QuicConnection& client, QuicConnection& server)
    {
        m_ends[0] = &client;
        m_ends[1] = &server;
    }

    Path path[2];
    Rng rng;
    Filter* filter = nullptr;
    u64 now = kStartNs;
    usize sent[2] = {};
    usize dropped[2] = {};
    bool client_initials_padded = true; // every client datagram whose first packet is an Initial had 1200 bytes
    bool amplification_kept = true;     // the server never sent more than three times what it received unvalidated

    QuicConnection& end(usize index) { return *m_ends[index]; }

    // Every datagram both ends have to send now goes on its path.
    void flush()
    {
        for (usize from = 0U; from < 2U; ++from)
        {
            for (usize guard = 0U; guard < 256U; ++guard)
            {
                u8 out[kDatagram];
                const usize size = m_ends[from]->send(Span<u8>(out, sizeof(out)), now);
                if (size == 0U)
                {
                    break;
                }
                ++sent[from];
                check(from, ConstSpan<u8>(out, size));
                const bool lost =
                    rng.chance(path[from].loss) || (filter != nullptr && filter->drop(from, ConstSpan<u8>(out, size)));
                if (lost)
                {
                    ++dropped[from];
                    continue;
                }
                enqueue(from, ConstSpan<u8>(out, size));
                if (rng.chance(path[from].duplicate))
                {
                    enqueue(from, ConstSpan<u8>(out, size));
                }
            }
        }
    }

    // Sends, then names the next arrival or timer.
    u64 next_event()
    {
        flush();
        u64 next = kNeverNs;
        for (usize index = 0U; index < m_count; ++index)
        {
            next = (m_flights[index].at < next) ? m_flights[index].at : next;
        }
        for (QuicConnection* const end : m_ends)
        {
            const u64 timer = end->next_timer_ns();
            next = (timer < next) ? timer : next;
        }
        return next;
    }

    // Moves time to `at` and runs every arrival and timer due. A timer that stays due while nothing arrives is a
    // connection that would spin its loop: the run fails.
    void run_at(u64 at)
    {
        m_stalled = (at <= now && m_count == 0U) ? m_stalled + 1U : 0U;
        REQUIRE(m_stalled < 64U);
        now = (at > now) ? at : now;
        for (usize index = 0U; index < m_count;)
        {
            if (m_flights[index].at <= now)
            {
                Flight flight = m_flights[index];
                m_flights[index] = m_flights[--m_count];
                deliver(flight);
                continue;
            }
            ++index;
        }
        for (QuicConnection* const end : m_ends)
        {
            if (end->next_timer_ns() <= now)
            {
                end->on_timeout(now);
            }
        }
    }

    // One event; false when nothing is left to happen.
    bool step()
    {
        const u64 next = next_event();
        if (next == kNeverNs)
        {
            return false;
        }
        run_at(next);
        return true;
    }

    // Runs every event up to `until` and stops the clock there.
    void advance(u64 until)
    {
        for (u64 next = next_event(); next <= until; next = next_event())
        {
            run_at(next);
        }
        now = (until > now) ? until : now;
    }

    template <typename Done> bool run_until(Done done, u64 limit_ns = 120000U * kMs)
    {
        const u64 until = now + limit_ns;
        flush();
        while (!done())
        {
            if (now > until || !step())
            {
                return done();
            }
        }
        return true;
    }

    [[nodiscard]] usize in_flight() const { return m_count; }

private:
    struct Flight
    {
        u8 bytes[kDatagram] = {};
        usize size = 0U;
        u64 at = 0U;
        usize to = 0U;
    };

    void check(usize from, ConstSpan<u8> datagram)
    {
        QuicHeader header;
        const usize short_bytes = (from == 0U) ? sizeof(kServerCid) : sizeof(kClientCid);
        if (from == 0U && parse_quic_header(datagram, short_bytes, header) == QuicHeaderError::None &&
            header.type == QuicPacketType::Initial && datagram.size() < kQuicMinInitialDatagram)
        {
            client_initials_padded = false;
        }
        const QuicConnection& server = *m_ends[1];
        if (from == 1U && !server.address_validated() &&
            server.counters().bytes_sent > 3U * server.counters().bytes_received)
        {
            amplification_kept = false;
        }
    }

    void enqueue(usize from, ConstSpan<u8> datagram)
    {
        REQUIRE(m_count < kFlights);
        Flight& flight = m_flights[m_count++];
        std::memcpy(flight.bytes, datagram.data(), datagram.size());
        flight.size = datagram.size();
        const u64 jitter = (path[from].jitter_ns == 0U) ? 0U : rng.next() % path[from].jitter_ns;
        flight.at = now + path[from].delay_ns + jitter;
        flight.to = 1U - from;
    }

    void deliver(Flight& flight)
    {
        QuicConnection& to = *m_ends[flight.to];
        if (flight.to == 1U && to.state() == QuicConnectionState::Idle)
        {
            QuicHeader header;
            REQUIRE(parse_quic_header(ConstSpan<u8>(flight.bytes, flight.size), sizeof(kServerCid), header) ==
                    QuicHeaderError::None);
            QuicAccept accept;
            accept.local_cid = ConstSpan<u8>(kServerCid, sizeof(kServerCid));
            accept.client_dcid = header.dcid;
            accept.client_scid = header.scid;
            accept.original_dcid = header.dcid;
            REQUIRE(to.accept(accept, now));
        }
        to.receive(Span<u8>(flight.bytes, flight.size), QuicEcn::NotEct, now);
    }

    QuicConnection* m_ends[2] = {};
    Flight m_flights[kFlights];
    usize m_count = 0U;
    usize m_stalled = 0U;
};

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
                config.tls = nullptr;
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
QuicConnectionConfig server_config(const Identity& identity, crd::u32 sent_packets)
{
    QuicConnectionConfig config = config_for(QuicSide::Server, identity, QuicVersion::V1, 2U);
    config.sent_packets = sent_packets;
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
};

class RawRun
{
public:
    explicit RawRun(const Identity& identity, crd::u32 sent_packets = 2048U)
        : server(allocator, server_config(identity, sent_packets)), m_identity(identity)
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
    // max_datagram_frame_size is 0 (RFC 9221 section 3).
    constexpr u8 data_bytes[3] = {1U, 2U, 3U};
    CHECK(refused(
              [&](QuicFrame& f)
              {
                  f.kind = QuicFrameKind::Datagram;
                  f.data = data_bytes;
              }) == code(QuicTransportError::ProtocolViolation));
    // The server issued sequence 0 alone, and sent no PATH_CHALLENGE.
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
