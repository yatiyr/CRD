// crd-net -- a QUIC server behind its I/O loop and ingress (4.2a; docs/design/network-foundation.md#quic-server):
// clients as plain connections over seeded paths that lose, duplicate and reorder, reaching the server through its
// virtual socket. Many clients at once in both versions, each echoed four streams and given a NEW_TOKEN token, with
// no allocation while serving and every route gone after they close; 0-RTT that arrives ahead of its Initial held and
// read by the connection that Initial creates, before and after a Retry, with a NEW_TOKEN token proving the next
// address; a full pool answered with CONNECTION_REFUSED, slots reused and stale handles refused; datagrams from
// another address dropped, a late copy of a first Initial opening nothing, and a client's stateless reset ending its
// connection unanswered; the held ring's bounds, a host that is busy, the configurations refused, and a client on
// the host's loopback.

#include "ingress_fixture.hpp"
#include "loop_fixture.hpp"
#include "quic_connection_fixture.hpp"
#include "quic_stream_fixture.hpp"

#include <crd/net/quic_server.hpp>

#include <atomic>
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
using namespace crd::crypto;
using namespace crd::net;
using namespace crd::net::testing;

constexpr usize kMaxClients = 24U;
constexpr usize kStreams = 4U;
constexpr u8 kPrf[kPrfKeyBytes] = {0x71, 0x72, 0x73};
constexpr u8 kResetSecret[kQuicResetKeyBytes] = {0x5E, 0x5F};
constexpr u8 kAppContext[] = {0x5C, 0x11, 0xE7, 0x02, 0x9A, 0x43, 0x0D, 0xB8};
constexpr usize kEarlyBytes = 3000U;

QuicStreamLimits limits()
{
    QuicStreamLimits out;
    out.peer_bidi = kStreams;
    out.local_bidi = kStreams;
    out.receive_bytes = 16384U;
    out.send_bytes = 16384U;
    out.stream_window = 8192U;
    out.connection_window = 24576U;
    out.connection_max_window = 131072U;
    return out;
}

Endpoint client_peer(usize index)
{
    return Endpoint{IpAddress::v4(198, 51, 100, static_cast<u8>(1U + (index % 200U))),
                    static_cast<crd::u16>(40000U + index)};
}

// The server's application: echoes every stream a client opens with another pattern as it reads, and counts what
// 0-RTT carried. State per slot, cleared when a connection is accepted there.
class EchoApp final : public QuicServerEvents
{
public:
    void on_accept(QuicServerHandle handle, QuicConnection&, const Endpoint&) noexcept override
    {
        ++accepted;
        m_state[handle.slot] = State{};
        last = handle;
    }

    void on_service(QuicServerHandle handle, QuicConnection& connection, u64) noexcept override
    {
        ++services;
        State& state = m_state[handle.slot];
        QuicStreamSet& set = connection.streams();
        QuicStreamEvent event;
        while (set.poll(event))
        {
        }
        for (usize index = 0U; index < kStreams; ++index)
        {
            const u64 stream = 4U * index;
            if (set.recv_state(stream) == QuicRecvState::None)
            {
                continue;
            }
            const u64 before = state.read[index];
            const bool ended = set.recv_state(stream) == QuicRecvState::DataRead ||
                               drain(set, stream, state.read[index], stream, &early_bytes);
            bytes_read += state.read[index] - before;
            read_handshaking = read_handshaking ||
                               (connection.state() == QuicConnectionState::Handshaking && state.read[index] > before);
            const QuicSendState echo = set.send_state(stream);
            if (echo == QuicSendState::Ready || echo == QuicSendState::Send)
            {
                static_cast<void>(
                    write_pattern(set, stream, state.echoed[index], state.read[index], stream + 1U, ended));
            }
        }
    }

    void on_closed(QuicServerHandle, const QuicConnection&) noexcept override { ++closed; }

    usize accepted = 0U;
    usize services = 0U;
    usize closed = 0U;
    u64 early_bytes = 0U;
    u64 bytes_read = 0U;
    bool read_handshaking = false;
    QuicServerHandle last;

private:
    struct State
    {
        u64 read[kStreams] = {};
        u64 echoed[kStreams] = {};
    };
    State m_state[kMaxClients];
};

struct Setup
{
    u32 connections = kMaxClients;
    u32 early_datagrams = 16U;
    u32 early_per_client = 4U;
    u32 early_bytes = 1500U;
    bool always_retry = false;
    const u8* early_context = nullptr;
    usize early_context_bytes = 0U;
};

// One client: its connection, its address and its half of the echo.
struct Client
{
    std::unique_ptr<QuicConnection> connection;
    Endpoint peer;
    u64 size[kStreams] = {};
    u64 sent[kStreams] = {};
    u64 read[kStreams] = {};
    bool done[kStreams] = {};
    usize opened = 0U;
    usize finished = 0U;
    bool initial_late = false; // its datagrams whose first packet is an Initial take 20 ms longer

    // Opens and writes its streams and reads their echoes; true once every echo was read whole.
    bool echo()
    {
        QuicStreamSet& set = connection->streams();
        u64 id = 0U;
        while (opened < kStreams && set.open(true, id) == QuicStreamError::None)
        {
            ++opened;
        }
        QuicStreamEvent event;
        while (set.poll(event))
        {
        }
        for (usize index = 0U; index < opened; ++index)
        {
            const u64 stream = 4U * index;
            const QuicSendState sending = set.send_state(stream);
            if (sending == QuicSendState::Ready || sending == QuicSendState::Send)
            {
                static_cast<void>(write_pattern(set, stream, sent[index], size[index], stream));
            }
            if (!done[index] && set.recv_state(stream) != QuicRecvState::None &&
                drain(set, stream, read[index], stream + 1U))
            {
                CHECK(read[index] == size[index]);
                done[index] = true;
                ++finished;
            }
        }
        return finished == kStreams;
    }
};

// A server behind its loop over a virtual socket, and clients joined to it by seeded impaired paths on a virtual
// clock (the socket's peer addresses tell the clients apart).
class Fleet
{
public:
    static constexpr usize kFlights = 4096U;
    static constexpr usize kToServer = ~usize{0};

    Fleet(const Identity& identity, const Setup& setup)
        : routes(allocator, 256U, ConstSpan<u8>(kPrf)), buckets(allocator, rules(setup), ConstSpan<u8>(kPrf)),
          ingress(IngressConfig{}, routes, buckets, &tokens, &new_tokens, &reset),
          server(allocator, socket, clock, ingress, routes, reset, &tokens, app, server_config(identity, setup)),
          loop(allocator, socket, clock, ingress, server, IoLoopConfig{}), m_identity(identity)
    {
        REQUIRE(tokens.init(ConstSpan<u8>(kTokenKey), {}) == CryptoError::None);
        REQUIRE(new_tokens.init(1024U, kQuicNewTokenLifetime, kUnix) == CryptoError::None);
        REQUIRE(reset.init(ConstSpan<u8>(kResetSecret)) == CryptoError::None);
        REQUIRE(server.ready());
        REQUIRE(loop.ready());
        clock.now = kStartNs;
    }

    // A client of `version` at `peer` with `seed`'s entropy, connected now.
    Client& add(QuicVersion version, usize seed, const Endpoint& peer, Tickets* tickets = nullptr,
                ConstSpan<u8> token = {}, QuicConnectionIds* ids = nullptr)
    {
        REQUIRE(m_clients < kMaxClients);
        Client& client = m_client[m_clients];
        QuicConnectionConfig config = config_for(QuicSide::Client, m_identity, version, seed);
        config.streams = limits();
        config.tickets = tickets;
        config.token = token;
        config.ids = ids;
        client.connection = std::make_unique<QuicConnection>(client_allocator, config);
        client.peer = peer;
        Rng sizes;
        sizes.state = seed + 100U;
        for (u64& size : client.size)
        {
            size = sizes.next() % 20000U;
        }
        const u8 local[8] = {0xC0, static_cast<u8>(m_clients), 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, static_cast<u8>(seed)};
        const u8 dcid[8] = {0xD0, static_cast<u8>(m_clients), 0xD2, 0xD3, 0xD4, 0xD5, static_cast<u8>(seed), 0xD7};
        REQUIRE(client.connection->connect(ConstSpan<u8>(local), ConstSpan<u8>(dcid), now));
        ++m_clients;
        return client;
    }

    [[nodiscard]] Client& client(usize index) { return m_client[index]; }
    [[nodiscard]] usize clients() const { return m_clients; }

    // A datagram from `peer` arriving at the server at `at`.
    void inject(ConstSpan<u8> datagram, const Endpoint& peer, u64 at)
    {
        REQUIRE(m_count < kFlights);
        Flight& flight = m_flights[m_count++];
        std::memcpy(flight.bytes, datagram.data(), datagram.size());
        flight.size = datagram.size();
        flight.at = at;
        flight.to = kToServer;
        flight.peer = peer;
    }

    template <typename Done> bool run_until(Done done, u64 limit_ns = 120000U * kMs)
    {
        const u64 until = now + limit_ns;
        while (!done())
        {
            const u64 next = next_event();
            if (now > until || next == kNeverNs)
            {
                return done();
            }
            run_at(next);
        }
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
        clock.now = now;
    }

    Path path;
    Rng rng;
    u64 now = kStartNs;
    usize to_server = 0U;  // datagrams the clients sent
    usize to_clients = 0U; // datagrams the server sent
    bool busy = false;     // the host takes one datagram per send call at most, and is busy on every other turn

    CountingAllocator allocator;
    CountingAllocator client_allocator;
    ReplayFilter new_tokens; // NEW_TOKEN tokens are single use
    ConnectionIdTable routes;
    HandshakeBuckets buckets;
    QuicTokenKeys tokens;
    QuicResetKey reset;
    Ingress ingress;
    VirtualSocket socket;
    TestClock clock;
    EchoApp app;
    QuicServer server;
    IoLoop loop;

private:
    struct Flight
    {
        u8 bytes[kDatagram] = {};
        usize size = 0U;
        u64 at = 0U;
        usize to = 0U;
        Endpoint peer; // a datagram to the server: its source
    };

    static HandshakeRules rules(const Setup& setup)
    {
        HandshakeRules out;
        out.always_retry = setup.always_retry;
        return out;
    }

    static QuicServerConfig server_config(const Identity& identity, const Setup& setup)
    {
        QuicServerConfig config;
        config.connection = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
        config.connection.streams = limits();
        config.connection.versions = ConstSpan<u32>(kServerVersions);
        config.connection.early_context = ConstSpan<u8>(setup.early_context, setup.early_context_bytes);
        config.connections = setup.connections;
        config.early_datagrams = setup.early_datagrams;
        config.early_per_client = setup.early_per_client;
        config.early_bytes = setup.early_bytes;
        return config;
    }

    static constexpr u32 kServerVersions[] = {static_cast<u32>(QuicVersion::V1), static_cast<u32>(QuicVersion::V2)};

    void enqueue(usize to, ConstSpan<u8> datagram, const Endpoint& peer, u64 extra)
    {
        if (rng.chance(path.loss))
        {
            return;
        }
        for (u32 copies = rng.chance(path.duplicate) ? 2U : 1U; copies != 0U; --copies)
        {
            REQUIRE(m_count < kFlights);
            Flight& flight = m_flights[m_count++];
            std::memcpy(flight.bytes, datagram.data(), datagram.size());
            flight.size = datagram.size();
            const u64 jitter = (path.jitter_ns == 0U) ? 0U : rng.next() % path.jitter_ns;
            flight.at = now + path.delay_ns + jitter + extra;
            flight.to = to;
            flight.peer = peer;
        }
    }

    // What every end has to send now goes on its path.
    void send_all()
    {
        for (usize index = 0U; index < m_clients; ++index)
        {
            Client& client = m_client[index];
            for (usize guard = 0U; guard < 256U; ++guard)
            {
                u8 out[kDatagram];
                const usize size = client.connection->send(Span<u8>(out, sizeof(out)), now);
                if (size == 0U)
                {
                    break;
                }
                ++to_server;
                QuicHeader header;
                const bool initial = parse_quic_header(ConstSpan<u8>(out, size), 8U, header) == QuicHeaderError::None &&
                                     header.type == QuicPacketType::Initial;
                enqueue(kToServer, ConstSpan<u8>(out, size), client.peer,
                        (client.initial_late && initial) ? 20U * kMs : 0U);
            }
        }
        for (usize index = 0U; index < socket.sent_count; ++index)
        {
            const VirtualSocket::Outbound& out = socket.sent[index];
            ++to_clients;
            for (usize target = 0U; target < m_clients; ++target)
            {
                if (m_client[target].peer == out.peer)
                {
                    enqueue(target, out.datagram.view(), out.peer, 0U);
                }
            }
        }
        socket.sent_count = 0U;
    }

    u64 next_event()
    {
        send_all();
        u64 next = (socket.wait_readable(0U) || server_next <= now) ? now : server_next;
        for (usize index = 0U; index < m_count; ++index)
        {
            next = (m_flights[index].at < next) ? m_flights[index].at : next;
        }
        for (usize index = 0U; index < m_clients; ++index)
        {
            const u64 timer = m_client[index].connection->next_timer_ns();
            next = (timer < next) ? timer : next;
        }
        return next;
    }

    void run_at(u64 at)
    {
        m_stalled = (at <= now && m_count == 0U) ? m_stalled + 1U : 0U;
        REQUIRE(m_stalled < 256U);
        now = (at > now) ? at : now;
        clock.now = now;
        for (usize index = 0U; index < m_count;)
        {
            if (m_flights[index].at > now)
            {
                ++index;
                continue;
            }
            Flight flight = m_flights[index];
            m_flights[index] = m_flights[--m_count];
            if (flight.to == kToServer)
            {
                if (socket.pending() == VirtualSocket::kQueue)
                {
                    server_next = loop.poll(); // the host's buffer is full: the server reads before more arrive
                }
                Datagram datagram;
                std::memcpy(datagram.bytes, flight.bytes, flight.size);
                datagram.size = flight.size;
                socket.deliver(datagram, flight.peer);
                continue;
            }
            m_client[flight.to].connection->receive(Span<u8>(flight.bytes, flight.size), QuicEcn::NotEct, now);
        }
        for (usize index = 0U; index < m_clients; ++index)
        {
            QuicConnection& connection = *m_client[index].connection;
            if (connection.next_timer_ns() <= now)
            {
                connection.on_timeout(now);
            }
        }
        m_turn = !m_turn;
        socket.busy_after = (busy && m_turn) ? 0U : (busy ? 1U : ~usize{0});
        server_next = loop.poll();
    }

    const Identity& m_identity;
    Client m_client[kMaxClients];
    usize m_clients = 0U;
    Flight m_flights[kFlights];
    usize m_count = 0U;
    usize m_stalled = 0U;
    u64 server_next = kNeverNs;
    bool m_turn = false;
};

bool all_echoed(Fleet& fleet)
{
    bool done = true;
    for (usize index = 0U; index < fleet.clients(); ++index)
    {
        done = fleet.client(index).echo() && done;
    }
    return done;
}

void hostile(Fleet& fleet, u64 seed)
{
    fleet.rng.state = seed;
    fleet.path.loss = 40U;
    fleet.path.duplicate = 30U;
    fleet.path.jitter_ns = 8U * kMs;
}
} // namespace

TEST_CASE("QUIC server: many clients in both versions are echoed over hostile paths, given NEW_TOKEN tokens, served "
          "without allocation and unrouted when they close",
          "[net][4.2a][quic-server]")
{
    for (u64 round = 0U; round < 3U; ++round)
    {
        INFO("round " << round);
        Identity identity;
        auto fleet = std::make_unique<Fleet>(identity, Setup{});
        if (round != 0U)
        {
            hostile(*fleet, round);
        }
        fleet->busy = round == 2U; // the host takes one datagram a turn, and none every other turn
        constexpr usize clients = 16U;
        for (usize index = 0U; index < clients; ++index)
        {
            const QuicVersion version = kVersions[index % 2U];
            fleet->add(version, index, client_peer(index));
        }
        REQUIRE(fleet->run_until(
            [&]
            {
                for (usize index = 0U; index < clients; ++index)
                {
                    if (!fleet->client(index).connection->confirmed())
                    {
                        return false;
                    }
                }
                return true;
            }));
        CHECK(fleet->server.live() == clients);
        CHECK(fleet->server.counters().admitted == clients);
        CHECK(fleet->app.accepted == clients);
        CHECK(fleet->server.counters().refused == 0U);
        for (usize index = 0U; index < clients; ++index)
        {
            CHECK(fleet->client(index).connection->version() == kVersions[index % 2U]);
        }
        // Serving allocates nothing: the echo runs within what admission took.
        const usize allocations = fleet->allocator.calls();
        REQUIRE(fleet->run_until(
            [&]
            {
                bool tokens = true;
                for (usize index = 0U; index < clients; ++index)
                {
                    tokens = tokens && !fleet->client(index).connection->new_token().empty();
                }
                return all_echoed(*fleet) && tokens;
            }));
        CHECK(fleet->allocator.calls() == allocations);
        CHECK(fleet->server.counters().new_tokens == clients);
        CHECK(fleet->server.counters().foreign_path == 0U);
        u64 total = 0U;
        for (usize index = 0U; index < clients; ++index)
        {
            for (const u64 size : fleet->client(index).size)
            {
                total += size;
            }
        }
        CHECK(fleet->app.bytes_read == total);

        // Every client closes: the server drains them, reclaims every slot and routes nothing more.
        for (usize index = 0U; index < clients; ++index)
        {
            fleet->client(index).connection->close(0U, {}, fleet->now);
        }
        REQUIRE(fleet->run_until([&] { return fleet->server.live() == 0U; }));
        CHECK(fleet->app.closed == clients);
        CHECK(fleet->server.counters().closed == clients);
        CHECK(fleet->routes.size() == 0U);
        CHECK(fleet->server.connection(fleet->app.last) == nullptr);
    }
}

TEST_CASE("QUIC server: 0-RTT ahead of its Initial is held and read by the connection the Initial creates, before and "
          "after a Retry, and a NEW_TOKEN token proves the next address",
          "[net][4.2a][quic-server][quic-early]")
{
    for (const bool retry : {false, true})
    {
        for (const QuicVersion version : kVersions)
        {
            INFO("retry " << retry << ", version " << static_cast<u32>(version));
            ReadyFilter early;
            Identity identity(false, &early.filter);
            Setup setup;
            setup.always_retry = retry;
            setup.early_context = kAppContext;
            setup.early_context_bytes = sizeof(kAppContext);
            auto fleet = std::make_unique<Fleet>(identity, setup);
            hostile(*fleet, 7U + static_cast<u64>(version));
            fleet->path.loss = 0U; // the reordering is the subject here; loss would only resend the 0-RTT

            // A first connection leaves the client a ticket and a NEW_TOKEN token.
            Tickets tickets;
            Client& first = fleet->add(version, 0U, client_peer(0U), &tickets);
            REQUIRE(fleet->run_until(
                [&]
                {
                    return first.connection->confirmed() && tickets.holds() && !first.connection->new_token().empty();
                }));
            CHECK(first.connection->retried() == retry);
            u8 token[kQuicMaxCarriedTokenBytes];
            const usize token_bytes = first.connection->new_token().size();
            std::memcpy(token, first.connection->new_token().data(), token_bytes);

            // The resumed client's 0-RTT reaches the server 20 ms before its Initial, from a new port of the same
            // address (a NEW_TOKEN token proves an address, not a port).
            const u64 retries = fleet->ingress.counters().retries;
            const u64 proven = fleet->ingress.counters().admitted_validated;
            const Endpoint moved{first.peer.address, static_cast<crd::u16>(first.peer.port + 1U)};
            Client& second =
                fleet->add(version, 1U, moved, &tickets, retry ? ConstSpan<u8>(token, token_bytes) : ConstSpan<u8>{});
            second.initial_late = true;
            REQUIRE(second.connection->handshake().early_data() == TlsEarlyData::Offered);
            u64 stream = 0U;
            REQUIRE(second.connection->streams().open(true, stream) == QuicStreamError::None);
            ++second.opened;
            second.size[0] = kEarlyBytes;
            REQUIRE(write_pattern(second.connection->streams(), stream, second.sent[0], kEarlyBytes, stream));
            const u64 early_before = fleet->app.early_bytes;
            // Its other streams open once it is confirmed, so its 0-RTT is that one stream: three datagrams, all held.
            REQUIRE(fleet->run_until([&] { return second.connection->confirmed() && second.echo(); }));
            CHECK(fleet->server.counters().early_dropped == 0U);
            CHECK(second.connection->handshake().early_data() == TlsEarlyData::Accepted);
            CHECK(fleet->server.counters().early_held >= 1U);
            CHECK(fleet->server.counters().early_read == fleet->server.counters().early_held);
            CHECK(fleet->app.early_bytes - early_before >= kEarlyBytes);
            CHECK(fleet->app.read_handshaking);
            // The NEW_TOKEN token proved the address: no second Retry.
            CHECK(fleet->ingress.counters().retries == retries);
            CHECK(fleet->ingress.counters().admitted_validated == proven + (retry ? 1U : 0U));
            CHECK_FALSE(second.connection->retried());
            CHECK(fleet->server.early_held() == 0U);
        }
    }
}

TEST_CASE("QUIC server: a full pool answers CONNECTION_REFUSED, a closed connection's slot is reused and its handle "
          "goes stale",
          "[net][4.2a][quic-server]")
{
    Identity identity;
    Setup setup;
    setup.connections = 1U;
    auto fleet = std::make_unique<Fleet>(identity, setup);
    Client& first = fleet->add(QuicVersion::V1, 0U, client_peer(0U));
    REQUIRE(fleet->run_until([&] { return first.connection->confirmed(); }));
    const QuicServerHandle handle = fleet->app.last;
    REQUIRE(fleet->server.connection(handle) != nullptr);

    Client& second = fleet->add(QuicVersion::V2, 1U, client_peer(1U));
    REQUIRE(fleet->run_until([&] { return second.connection->state() == QuicConnectionState::Closed; }));
    CHECK(second.connection->close_info().error_code == static_cast<u64>(QuicTransportError::ConnectionRefused));
    CHECK_FALSE(second.connection->close_info().local);
    CHECK(fleet->server.counters().refused >= 1U);
    CHECK(fleet->ingress.counters().refusals == fleet->server.counters().refused);
    CHECK(fleet->server.live() == 1U);

    first.connection->close(7U, {}, fleet->now);
    REQUIRE(fleet->run_until([&] { return fleet->server.live() == 0U; }));
    CHECK(fleet->server.connection(handle) == nullptr);
    fleet->server.wake(handle); // a stale handle wakes nothing

    Client& third = fleet->add(QuicVersion::V1, 2U, client_peer(2U));
    REQUIRE(fleet->run_until([&] { return third.connection->confirmed() && third.echo(); }));
    CHECK(fleet->app.last.slot == handle.slot);
    CHECK(fleet->app.last.generation == handle.generation + 1U);
    CHECK(fleet->server.connection(fleet->app.last) != nullptr);
}

TEST_CASE("QUIC server: another address is dropped, a late first Initial opens nothing, and a client's stateless reset "
          "ends its connection unanswered",
          "[net][4.2a][quic-server]")
{
    // The client's own IDs carry stateless reset tokens of its host's key (declared first: they outlive its
    // connection).
    CountingAllocator client_host;
    ConnectionIdTable client_routes(client_host, 16U);
    QuicResetKey client_key;
    constexpr u8 client_secret[kQuicResetKeyBytes] = {0xC5};
    REQUIRE(client_key.init(ConstSpan<u8>(client_secret)) == CryptoError::None);
    QuicRoutedIds client_ids(client_routes, client_key, 0U);
    Identity identity;
    auto fleet = std::make_unique<Fleet>(identity, Setup{});

    Client& client = fleet->add(QuicVersion::V1, 0U, client_peer(0U), nullptr, {}, &client_ids);
    // Captured: the client's first datagram, its Initial.
    u8 first[kDatagram];
    const usize first_bytes = client.connection->send(Span<u8>(first, sizeof(first)), fleet->now);
    REQUIRE(first_bytes >= kQuicMinInitialDatagram);
    fleet->inject(ConstSpan<u8>(first, first_bytes), client.peer, fleet->now + 10U * kMs);
    REQUIRE(fleet->run_until([&] { return client.connection->confirmed() && client.echo(); }));
    CHECK(fleet->server.live() == 1U);

    // A late copy of the first Initial reaches the connection, which no longer reads Initials: no second connection.
    fleet->inject(ConstSpan<u8>(first, first_bytes), client.peer, fleet->now + kMs);
    // The same client datagram from another address is dropped (NET-PATH validates migrations).
    fleet->inject(ConstSpan<u8>(first, first_bytes), client_peer(5U), fleet->now + kMs);
    fleet->advance(fleet->now + 50U * kMs);
    CHECK(fleet->server.live() == 1U);
    CHECK(fleet->server.counters().admitted == 1U);
    CHECK(fleet->server.counters().foreign_path == 1U);
    CHECK(client.connection->state() == QuicConnectionState::Established);
    // The client rotates its IDs, so the server sends to one that carries a reset token; then the client loses its
    // state and answers with a stateless reset (RFC 9000 section 10.3), which the server reads and does not answer.
    QuicConnection* const served = fleet->server.connection(fleet->app.last);
    REQUIRE(served != nullptr);
    REQUIRE(client.connection->rotate_connection_ids());
    REQUIRE(fleet->run_until(
        [&]
        {
            client.connection->ping();
            return client_routes.find(served->peer_cid()) != kNoConnection;
        }));
    client.connection->ping();
    fleet->advance(fleet->now + 100U * kMs); // the server has sent to that ID
    u8 reset_datagram[60];
    std::memset(reset_datagram, 0x77, sizeof(reset_datagram));
    reset_datagram[0] = 0x55U;
    REQUIRE(
        client_key.token(served->peer_cid(), Span<u8>(reset_datagram + sizeof(reset_datagram) - kQuicResetTokenBytes,
                                                      kQuicResetTokenBytes)) == CryptoError::None);
    const u64 answered = fleet->ingress.counters().resets;
    fleet->inject(ConstSpan<u8>(reset_datagram), client.peer, fleet->now + kMs);
    fleet->advance(fleet->now + 5U * kMs);
    CHECK(fleet->server.counters().peer_resets == 1U);
    CHECK(fleet->ingress.counters().resets == answered);
    CHECK(served->close_info().stateless_reset);
    // The same from another address is not the peer's: the ingress answers it with its own reset.
    fleet->inject(ConstSpan<u8>(reset_datagram), client_peer(6U), fleet->now + kMs);
    fleet->advance(fleet->now + 5U * kMs);
    CHECK(fleet->ingress.counters().resets == answered + 1U);
    REQUIRE(fleet->run_until([&] { return fleet->server.live() == 0U; }));
}

TEST_CASE("QUIC server: the held 0-RTT ring keeps its per-client share, evicts the oldest, expires and drops what it "
          "cannot hold",
          "[net][4.2a][quic-server][quic-early]")
{
    Identity identity;
    Setup setup;
    setup.early_datagrams = 4U;
    setup.early_per_client = 2U;
    setup.early_bytes = 1200U;
    auto fleet = std::make_unique<Fleet>(identity, setup);
    const u8 a[8] = {0xA1, 1, 2, 3, 4, 5, 6, 7};
    const u8 b[8] = {0xB1, 1, 2, 3, 4, 5, 6, 7};
    const u8 c[9] = {0xC1, 1, 2, 3, 4, 5, 6, 7, 8};
    const auto early = [&](ConstSpan<u8> dcid, const Endpoint& from, usize bytes)
    {
        fleet->socket.deliver(long_packet(kV1, QuicPacketType::ZeroRtt, dcid, ConstSpan<u8>(a, 4U), {}, bytes), from);
        static_cast<void>(fleet->loop.poll());
    };
    early(ConstSpan<u8>(a), client_peer(0U), 90U);
    early(ConstSpan<u8>(a), client_peer(0U), 90U);
    early(ConstSpan<u8>(a), client_peer(0U), 90U); // its share is two
    CHECK(fleet->server.counters().early_dropped == 1U);
    early(ConstSpan<u8>(a), client_peer(1U), 90U); // another address has its own
    early(ConstSpan<u8>(b), client_peer(0U), 90U);
    CHECK(fleet->server.early_held() == 4U);
    early(ConstSpan<u8>(c), client_peer(0U), 90U); // full: the oldest goes
    CHECK(fleet->server.counters().early_evicted == 1U);
    CHECK(fleet->server.early_held() == 4U);
    early(ConstSpan<u8>(b), client_peer(2U), 1300U); // longer than a held slot
    CHECK(fleet->server.counters().early_dropped == 2U);
    CHECK(fleet->server.counters().early_held == 5U);
    CHECK(fleet->socket.sent_count == 0U); // none is answered

    fleet->clock.now += 99U * kMs;
    static_cast<void>(fleet->loop.poll());
    CHECK(fleet->server.early_held() == 4U);
    fleet->clock.now += kMs;
    static_cast<void>(fleet->loop.poll());
    CHECK(fleet->server.early_held() == 0U);
    CHECK(fleet->server.counters().early_expired == 4U);
    CHECK(fleet->server.counters().early_read == 0U);

    // A server that holds none drops them all.
    Setup none;
    none.early_datagrams = 0U;
    auto plain = std::make_unique<Fleet>(identity, none);
    plain->socket.deliver(long_packet(kV1, QuicPacketType::ZeroRtt, ConstSpan<u8>(a), {}, {}, 90U), client_peer(0U));
    static_cast<void>(plain->loop.poll());
    CHECK(plain->server.counters().early_dropped == 1U);
    CHECK(plain->server.early_held() == 0U);
}

TEST_CASE("QUIC server: configurations out of range and refused allocations leave it not ready",
          "[net][4.2a][quic-server]")
{
    Identity identity;
    CountingAllocator allocator;
    ConnectionIdTable routes(allocator, 64U, ConstSpan<u8>(kPrf));
    HandshakeBuckets buckets(allocator, HandshakeRules{}, ConstSpan<u8>(kPrf));
    QuicResetKey reset;
    REQUIRE(reset.init(ConstSpan<u8>(kResetSecret)) == CryptoError::None);
    Ingress ingress(IngressConfig{}, routes, buckets, nullptr, nullptr, &reset);
    VirtualSocket socket;
    TestClock clock;
    EchoApp app;
    QuicServerConfig good;
    good.connection = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
    const auto ready = [&](const QuicServerConfig& config)
    {
        const QuicServer server(allocator, socket, clock, ingress, routes, reset, nullptr, app, config);
        return server.ready();
    };
    CHECK(ready(good));
    const auto refused = [&](auto change)
    {
        QuicServerConfig config = good;
        change(config);
        return !ready(config);
    };
    CHECK(refused([](QuicServerConfig& c) { c.connection.side = QuicSide::Client; }));
    CHECK(refused([](QuicServerConfig& c) { c.connection.tls = nullptr; }));
    CHECK(refused([](QuicServerConfig& c) { c.connection.max_datagram_bytes = 1199U; }));
    CHECK(refused([](QuicServerConfig& c) { c.connection.entropy_seed = ConstSpan<u8>(kPrf, 5U); }));
    CHECK(refused([](QuicServerConfig& c) { c.connections = 0U; }));
    CHECK(refused([](QuicServerConfig& c) { c.connections = kQuicMaxServerConnections + 1U; }));
    CHECK(refused([](QuicServerConfig& c) { c.early_datagrams = kQuicMaxEarlyDatagrams + 1U; }));
    CHECK(refused([](QuicServerConfig& c) { c.early_per_client = 0U; }));
    CHECK(refused([](QuicServerConfig& c) { c.early_bytes = 1199U; }));
    CHECK(refused([](QuicServerConfig& c) { c.early_lifetime_ns = 0U; }));
    CHECK(refused([](QuicServerConfig& c) { c.send_slots = 0U; }));
    CHECK(refused([](QuicServerConfig& c) { c.send_retry_ns = 0U; }));
    CHECK_FALSE(refused(
        [](QuicServerConfig& c)
        {
            c.early_datagrams = 0U;
            c.early_per_client = 0U;
        }));

    // Every allocation it makes, refused in turn, leaves it not ready (and leaks nothing: the sanitizers' lanes).
    bool built = false;
    usize refusals = 0U;
    for (usize allowed = 0U; !built && allowed < 64U; ++allowed)
    {
        allocator.refuse_after(allocator.calls() + allowed);
        built = ready(good);
        allocator.refuse_after(~usize{0});
        refusals += built ? 0U : 1U;
    }
    CHECK(built);
    CHECK(refusals > 8U);
}

TEST_CASE("QUIC server: a client on the host's loopback is served through a real UDP socket",
          "[net][4.2a][quic-server]")
{
    UdpOptions options;
    options.bind = Endpoint{IpAddress::v4(127, 0, 0, 1), 0U};
    SocketError error = SocketError::Failed;
    UdpSocket server_socket = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);
    UdpSocket client_socket = UdpSocket::open(options, error);
    REQUIRE(error == SocketError::None);

    Identity identity;
    CountingAllocator allocator;
    ConnectionIdTable routes(allocator, 64U);
    HandshakeBuckets buckets(allocator, HandshakeRules{});
    QuicResetKey reset;
    REQUIRE(reset.init(ConstSpan<u8>(kResetSecret)) == CryptoError::None);
    QuicTokenKeys tokens;
    REQUIRE(tokens.init(ConstSpan<u8>(kTokenKey), {}) == CryptoError::None);
    Ingress ingress(IngressConfig{}, routes, buckets, &tokens, nullptr, &reset);
    const HostClock clock;
    EchoApp app;
    QuicServerConfig config;
    config.connection = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
    config.connection.streams = limits();
    config.connection.entropy_seed = {};
    QuicServer server(allocator, server_socket, clock, ingress, routes, reset, &tokens, app, config);
    IoLoop loop(allocator, server_socket, clock, ingress, server, IoLoopConfig{});
    REQUIRE(server.ready());
    REQUIRE(loop.ready());

    Client client;
    QuicConnectionConfig client_config = config_for(QuicSide::Client, identity, QuicVersion::V1, 0U);
    client_config.streams = limits();
    client_config.entropy_seed = {};
    client.connection = std::make_unique<QuicConnection>(allocator, client_config);
    client.size[0] = 5000U;
    client.size[1] = 1U;
    const u8 local[8] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
    const u8 dcid[8] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27};
    REQUIRE(client.connection->connect(ConstSpan<u8>(local), ConstSpan<u8>(dcid), clock.monotonic_ns()));
    const Endpoint to = server_socket.local_endpoint();
    const u64 deadline = clock.monotonic_ns() + (10U * kNanosPerSecond);
    bool done = false;
    while (!done && clock.monotonic_ns() < deadline)
    {
        const u64 now = clock.monotonic_ns();
        u8 out[kDatagram];
        for (usize size = client.connection->send(Span<u8>(out, sizeof(out)), now); size != 0U;
             size = client.connection->send(Span<u8>(out, sizeof(out)), now))
        {
            const OutgoingDatagram outgoing{ConstSpan<u8>(out, size), to};
            REQUIRE(client_socket.send(ConstSpan<OutgoingDatagram>(&outgoing, 1U)).count == 1U);
        }
        static_cast<void>(server_socket.wait_readable(kMs));
        static_cast<void>(loop.poll());
        u8 storage[kDatagram];
        ReceivedDatagram slot;
        slot.storage = Span<u8>(storage);
        while (client_socket.receive(Span<ReceivedDatagram>(&slot, 1U)).count == 1U)
        {
            CHECK(slot.peer == to);
            client.connection->receive(slot.storage.first(slot.size), QuicEcn::NotEct, clock.monotonic_ns());
        }
        if (client.connection->next_timer_ns() <= clock.monotonic_ns())
        {
            client.connection->on_timeout(clock.monotonic_ns());
        }
        done = client.connection->confirmed() && client.echo();
    }
    CHECK(done);
    CHECK(server.live() == 1U);
    CHECK(server.counters().new_tokens == 1U);
    CHECK(server.counters().datagrams_sent != 0U);
}
