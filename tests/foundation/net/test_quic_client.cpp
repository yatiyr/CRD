// crd-net -- a QUIC client host behind its I/O loop and a client's ingress (4.2a;
// docs/design/network-foundation.md#quic-client): its connections share one virtual socket and reach servers behind
// their own loops over seeded paths that lose, duplicate and reorder. Many connections to two servers in both versions
// echoed and closed with no allocation while serving; Version Negotiation followed once into the version the client
// prefers, a second one and one listing nothing it runs ending the connection; a Retry taken and the NEW_TOKEN token
// carried by the next connection, which is not retried; a ticket's 0-RTT written in the first service; another
// address dropped and the server's stateless reset ending the connection only from the server's address; the pool's
// bounds, stale handles, a TLS context per connection and the configurations refused; and a client host served over
// the host's loopback.

#include "ingress_fixture.hpp"
#include "loop_fixture.hpp"
#include "quic_connection_fixture.hpp"
#include "quic_host_fixture.hpp"
#include "quic_stream_fixture.hpp"

#include <crd/net/quic_client.hpp>
#include <crd/net/quic_server.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;
using namespace crd::net;
using namespace crd::net::testing;

constexpr u8 kClientSecret[kQuicResetKeyBytes] = {0xC5, 0xC6};
constexpr usize kClientCid = 8U;
constexpr usize kMaxServers = 2U;
const Endpoint kClientAt{IpAddress::v4(198, 51, 100, 7), 50000U};
const Endpoint kStranger{IpAddress::v4(10, 0, 0, 9), 443U};

Endpoint server_at(usize index)
{
    return Endpoint{IpAddress::v4(10, 0, 0, static_cast<u8>(1U + index)), 443U};
}

// The client's application: per slot it opens four streams, writes a seeded size of pattern `stream` on each and
// reads the server's echo (pattern stream + 1); what a restart threw away is written again.
class ClientApp final : public QuicClientEvents
{
public:
    struct State
    {
        u64 size[kStreams] = {};
        u64 sent[kStreams] = {};
        u64 read[kStreams] = {};
        bool done[kStreams] = {};
        usize opened = 0U;
        usize finished = 0U;
        bool closed = false;
        bool wrote_handshaking = false; // wrote stream data before the handshake completed (0-RTT)
        QuicCloseInfo close;
        QuicVersion version = QuicVersion::V1;
    };

    void begin(QuicHandle handle, u64 seed)
    {
        REQUIRE(handle.valid());
        State& state = m_state[handle.slot];
        state = State{};
        Rng sizes;
        sizes.state = seed + 100U;
        for (u64& size : state.size)
        {
            size = 1U + (sizes.next() % 20000U);
        }
    }

    void on_service(QuicHandle handle, QuicConnection& connection, u64) noexcept override
    {
        ++services;
        State& state = m_state[handle.slot];
        QuicStreamSet& set = connection.streams();
        u64 id = 0U;
        while (state.opened < kStreams && set.open(true, id) == QuicStreamError::None)
        {
            ++state.opened;
        }
        QuicStreamEvent event;
        while (set.poll(event))
        {
        }
        for (usize index = 0U; index < state.opened; ++index)
        {
            const u64 stream = 4U * index;
            const QuicSendState sending = set.send_state(stream);
            if (sending == QuicSendState::Ready || sending == QuicSendState::Send)
            {
                const u64 before = state.sent[index];
                static_cast<void>(write_pattern(set, stream, state.sent[index], state.size[index], stream));
                state.wrote_handshaking =
                    state.wrote_handshaking ||
                    (connection.state() == QuicConnectionState::Handshaking && state.sent[index] > before);
            }
            if (!state.done[index] && set.recv_state(stream) != QuicRecvState::None &&
                drain(set, stream, state.read[index], stream + 1U))
            {
                CHECK(state.read[index] == state.size[index]);
                state.done[index] = true;
                ++state.finished;
            }
        }
    }

    void on_restart(QuicHandle handle, QuicConnection&) noexcept override
    {
        ++restarts;
        State& state = m_state[handle.slot];
        State fresh;
        std::memcpy(fresh.size, state.size, sizeof(fresh.size));
        state = fresh;
    }

    void on_closed(QuicHandle handle, const QuicConnection& connection) noexcept override
    {
        ++closed;
        State& state = m_state[handle.slot];
        state.closed = true;
        state.close = connection.close_info();
        state.version = connection.version();
    }

    [[nodiscard]] const State& state(QuicHandle handle) const { return m_state[handle.slot]; }
    [[nodiscard]] bool echoed(QuicHandle handle) const { return m_state[handle.slot].finished == kStreams; }

    usize services = 0U;
    usize restarts = 0U;
    usize closed = 0U;

private:
    State m_state[kMaxSlots];
};

QuicClientConfig client_config(const Identity& identity, QuicVersion version, ConstSpan<u32> versions,
                               Tickets* tickets = nullptr)
{
    QuicClientConfig config;
    config.connection = config_for(QuicSide::Client, identity, version, 0U);
    config.connection.streams = limits();
    config.connection.versions = versions;
    config.connection.tickets = tickets;
    config.connections = 16U;
    return config;
}

IngressConfig client_ingress_config()
{
    IngressConfig config;
    config.local_cid_bytes = kClientCid;
    return config;
}

// A client host and `servers` server stacks on one virtual clock, joined by seeded impaired paths: what the client's
// socket sends goes to the server whose address it names, what a server sends to the client's address goes back from
// the server's address.
class Network
{
public:
    static constexpr usize kFlights = 4096U;
    static constexpr usize kToClient = ~usize{0};

    Network(const Identity& identity, const Setup& setup, const QuicClientConfig& config, usize servers = 1U)
        : client_routes(client_allocator, 128U, ConstSpan<u8>(kHostPrf)),
          client_ingress(client_ingress_config(), client_routes),
          client(client_allocator, client_socket, client_ingress, client_routes, client_reset, app, config),
          client_loop(client_allocator, client_socket, client_clock, client_ingress, client, IoLoopConfig{}),
          m_servers(servers)
    {
        REQUIRE(client_reset.init(ConstSpan<u8>(kClientSecret)) == CryptoError::None);
        REQUIRE(client.ready());
        REQUIRE(client_loop.ready());
        client_socket.local = kClientAt;
        client_clock.now = now;
        for (usize index = 0U; index < servers; ++index)
        {
            m_server[index] = std::make_unique<ServerStack>(identity, setup, server_at(index));
        }
    }

    [[nodiscard]] ServerStack& server(usize index) { return *m_server[index]; }

    // Connects to server `index` with the stream sizes of `seed`.
    QuicHandle connect(usize index, u64 seed, const TlsContext* tls = nullptr)
    {
        QuicClientConnect request;
        request.server = server_at(index);
        request.tls = tls;
        const QuicHandle handle = client.connect(request, now);
        app.begin(handle, seed);
        m_client_next = now; // the client's loop has a connection to serve
        return handle;
    }

    // The live connection of `handle` (the test requires one).
    QuicConnection& live(QuicHandle handle)
    {
        QuicConnection* const connection = client.connection(handle);
        REQUIRE(connection != nullptr);
        return *connection;
    }

    // The stateless reset server `index` sends, having lost its state, for the next 1-RTT datagram of `handle`.
    Datagram server_reset(QuicHandle handle, usize index = 0U)
    {
        live(handle).ping();
        wake(handle);
        Datagram sent[8];
        REQUIRE(poll_client(sent, 8U) >= 1U);
        ServerStack& stack = server(index);
        ConnectionIdTable forgotten(stack.allocator, 16U, ConstSpan<u8>(kHostPrf));
        Ingress forgetful(IngressConfig{}, forgotten, stack.buckets, nullptr, nullptr, &stack.reset);
        const IngressDecision decision = forgetful.classify(sent[0].view(), false, kClientAt, now, kUnix);
        REQUIRE(decision.action == IngressAction::StatelessReset);
        Datagram reset;
        reset.size = forgetful.answer(decision, kClientAt, kUnix, Span<u8>(reset.bytes, kQuicMaxStatelessAnswerBytes));
        REQUIRE(reset.size != 0U);
        return reset;
    }

    // Wakes a connection the test wrote to or closed.
    void wake(QuicHandle handle)
    {
        client.wake(handle);
        m_client_next = now;
    }

    // A datagram from `from` reaching the client's socket at once.
    void deliver_to_client(ConstSpan<u8> bytes, const Endpoint& from)
    {
        Datagram datagram;
        std::memcpy(datagram.bytes, bytes.data(), bytes.size());
        datagram.size = bytes.size();
        client_socket.deliver(datagram, from);
    }

    // One client turn, then what it sent, taken off its socket (no path carries it).
    usize poll_client(Datagram* out, usize room)
    {
        static_cast<void>(client_loop.poll());
        const usize count = (client_socket.sent_count < room) ? client_socket.sent_count : room;
        for (usize index = 0U; index < count; ++index)
        {
            out[index] = client_socket.sent[index].datagram;
        }
        client_socket.sent_count = 0U;
        return count;
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

    Path path;
    Rng rng;
    u64 now = kStartNs;
    bool busy = false; // the client's host takes one datagram per send call at most, and none every other turn
    usize stray = 0U;  // datagrams sent to no one

    CountingAllocator client_allocator;
    ConnectionIdTable client_routes;
    QuicResetKey client_reset;
    Ingress client_ingress;
    VirtualSocket client_socket;
    TestClock client_clock;
    ClientApp app;
    QuicClient client;
    IoLoop client_loop;

private:
    struct Flight
    {
        u8 bytes[kDatagram] = {};
        usize size = 0U;
        u64 at = 0U;
        usize to = 0U;
        Endpoint from;
    };

    void enqueue(usize to, ConstSpan<u8> datagram, const Endpoint& from)
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
            flight.at = now + path.delay_ns + jitter;
            flight.to = to;
            flight.from = from;
        }
    }

    void send_all()
    {
        for (usize index = 0U; index < client_socket.sent_count; ++index)
        {
            const VirtualSocket::Outbound& out = client_socket.sent[index];
            usize to = m_servers;
            for (usize target = 0U; target < m_servers; ++target)
            {
                to = (server_at(target) == out.peer) ? target : to;
            }
            if (to == m_servers)
            {
                ++stray;
                continue;
            }
            enqueue(to, out.datagram.view(), kClientAt);
        }
        client_socket.sent_count = 0U;
        for (usize index = 0U; index < m_servers; ++index)
        {
            VirtualSocket& socket = m_server[index]->socket;
            for (usize sent = 0U; sent < socket.sent_count; ++sent)
            {
                const VirtualSocket::Outbound& out = socket.sent[sent];
                if (out.peer == kClientAt)
                {
                    enqueue(kToClient, out.datagram.view(), server_at(index));
                }
                else
                {
                    ++stray;
                }
            }
            socket.sent_count = 0U;
        }
    }

    u64 next_event()
    {
        send_all();
        u64 next = (client_socket.wait_readable(0U) || m_client_next <= now) ? now : m_client_next;
        for (usize index = 0U; index < m_servers; ++index)
        {
            if (m_server[index]->socket.wait_readable(0U) || m_server_next[index] <= now)
            {
                next = now;
            }
            next = (m_server_next[index] < next) ? m_server_next[index] : next;
        }
        for (usize index = 0U; index < m_count; ++index)
        {
            next = (m_flights[index].at < next) ? m_flights[index].at : next;
        }
        return next;
    }

    void run_at(u64 at)
    {
        m_stalled = (at <= now && m_count == 0U) ? m_stalled + 1U : 0U;
        REQUIRE(m_stalled < 256U);
        now = (at > now) ? at : now;
        client_clock.now = now;
        for (usize index = 0U; index < m_servers; ++index)
        {
            m_server[index]->clock.now = now;
        }
        // Due flights arrive in the order they were sent (a path without jitter keeps it); the rest keep theirs.
        usize kept = 0U;
        for (usize index = 0U; index < m_count; ++index)
        {
            const Flight& flight = m_flights[index];
            if (flight.at > now)
            {
                if (kept != index)
                {
                    m_flights[kept] = flight;
                }
                ++kept;
                continue;
            }
            VirtualSocket& socket = (flight.to == kToClient) ? client_socket : m_server[flight.to]->socket;
            if (socket.pending() == VirtualSocket::kQueue)
            {
                continue; // the host's buffer is full: dropped, as a kernel drops it
            }
            Datagram datagram;
            std::memcpy(datagram.bytes, flight.bytes, flight.size);
            datagram.size = flight.size;
            socket.deliver(datagram, flight.from);
        }
        m_count = kept;
        m_turn = !m_turn;
        client_socket.throttle(busy, m_turn);
        m_client_next = client_loop.poll();
        for (usize index = 0U; index < m_servers; ++index)
        {
            m_server_next[index] = m_server[index]->loop.poll();
        }
    }

    std::unique_ptr<ServerStack> m_server[kMaxServers];
    usize m_servers = 0U;
    u64 m_server_next[kMaxServers] = {kNeverNs, kNeverNs};
    u64 m_client_next = kNeverNs;
    Flight m_flights[kFlights];
    usize m_count = 0U;
    usize m_stalled = 0U;
    bool m_turn = false;
};

void hostile(Network& network, u64 seed)
{
    network.rng.state = seed;
    network.path.loss = 40U;
    network.path.duplicate = 30U;
    network.path.jitter_ns = 8U * kMs;
}
} // namespace

TEST_CASE("QUIC client: many connections over one socket reach two servers in both versions over hostile paths, "
          "serve without allocation and leave nothing routed when they close",
          "[net][4.2a][quic-client]")
{
    for (u64 round = 0U; round < 3U; ++round)
    {
        for (const QuicVersion version : kVersions)
        {
            INFO("round " << round << ", version " << static_cast<u32>(version));
            Identity identity;
            auto network = std::make_unique<Network>(identity, Setup{}, client_config(identity, version, {}), 2U);
            if (round != 0U)
            {
                hostile(*network, (round * 10U) + static_cast<u64>(version));
            }
            network->busy = round == 2U;
            static constexpr usize kConnections = 12U;
            QuicHandle handles[kConnections];
            for (usize index = 0U; index < kConnections; ++index)
            {
                handles[index] = network->connect(index % 2U, index);
                REQUIRE(handles[index].valid());
            }
            CHECK(network->client.live() == kConnections);
            // Serving allocates nothing: every buffer was taken by connect().
            const usize allocations = network->client_allocator.calls();
            REQUIRE(network->run_until(
                [&]
                {
                    for (const QuicHandle handle : handles)
                    {
                        const QuicConnection* const connection = network->client.connection(handle);
                        if (connection == nullptr || !network->app.echoed(handle) || !connection->confirmed())
                        {
                            return false;
                        }
                    }
                    return !network->client.token_for(server_at(0U), version).empty() &&
                           !network->client.token_for(server_at(1U), version).empty();
                }));
            CHECK(network->client_allocator.calls() == allocations);
            CHECK(network->server(0U).server.live() == kConnections / 2U);
            CHECK(network->server(1U).server.live() == kConnections / 2U);
            for (const QuicHandle handle : handles)
            {
                CHECK(network->live(handle).version() == version);
            }
            const QuicClientCounters counters = network->client.counters();
            CHECK(counters.connected == kConnections);
            CHECK(counters.foreign_path == 0U);
            CHECK(counters.restarts == 0U);
            CHECK(counters.tokens_kept >= 2U);
            CHECK(network->stray == 0U);
            if (round == 2U)
            {
                CHECK(network->client_socket.send_calls > counters.datagrams_sent / 2U);
            }

            // Every connection closes: both ends reclaim their slots and the client routes nothing more.
            for (const QuicHandle handle : handles)
            {
                network->live(handle).close(0U, {}, network->now);
                network->wake(handle);
            }
            REQUIRE(network->run_until(
                [&]
                {
                    return network->client.live() == 0U && network->server(0U).server.live() == 0U &&
                           network->server(1U).server.live() == 0U;
                }));
            CHECK(network->app.closed == kConnections);
            CHECK(network->client.counters().closed == kConnections);
            CHECK(network->client_routes.size() == 0U);
            for (const QuicHandle handle : handles)
            {
                CHECK(network->client.connection(handle) == nullptr);
                CHECK(network->app.state(handle).close.local);
            }
        }
    }
}

TEST_CASE("QUIC client: Version Negotiation is followed once into the version the client prefers, and ends a "
          "connection when it lists none of the client's or comes again",
          "[net][4.2a][quic-client]")
{
    Identity identity;
    Setup setup;
    setup.only_v2 = true;
    {
        // A client of version 1 that also runs version 2 is answered by a server of version 2 alone.
        auto network = std::make_unique<Network>(
            identity, setup, client_config(identity, QuicVersion::V1, ConstSpan<u32>(kBothVersions)));
        hostile(*network, 31U);
        const QuicHandle handle = network->connect(0U, 3U);
        REQUIRE(network->run_until([&] { return network->app.echoed(handle); }));
        CHECK(network->app.restarts == 1U);
        CHECK(network->client.counters().restarts == 1U);
        CHECK(network->server(0U).ingress.counters().negotiations >= 1U);
        CHECK(network->live(handle).version() == QuicVersion::V2);
        CHECK(network->live(handle).confirmed());
        CHECK(network->app.closed == 0U);
        // The connection that followed answers to its server's address: its stateless reset ends it.
        const Datagram reset = network->server_reset(handle);
        network->deliver_to_client(reset.view(), server_at(0U));
        static_cast<void>(network->client_loop.poll());
        CHECK(network->client.counters().peer_resets == 1U);
    }
    {
        // A second Version Negotiation, to the connection that followed the first, ends it.
        auto network = std::make_unique<Network>(
            identity, setup, client_config(identity, QuicVersion::V1, ConstSpan<u32>(kBothVersions)));
        const QuicHandle handle = network->connect(0U, 4U);
        REQUIRE(network->run_until([&] { return network->app.restarts == 1U; }));
        Datagram sent[8];
        const usize count = network->poll_client(sent, 8U);
        REQUIRE(count >= 1U);
        QuicHeader header;
        REQUIRE(parse_quic_header(sent[0].view(), kClientCid, header) == QuicHeaderError::None);
        REQUIRE(header.type == QuicPacketType::Initial);
        REQUIRE(header.version == static_cast<u32>(QuicVersion::V2));
        const u32 offered[] = {static_cast<u32>(QuicVersion::V1)};
        u8 answer[kQuicMaxVersionNegotiationBytes];
        const usize bytes =
            write_version_negotiation(header.dcid, header.scid, ConstSpan<u32>(offered), 0x1234U, Span<u8>(answer));
        REQUIRE(bytes != 0U);
        network->deliver_to_client(ConstSpan<u8>(answer, bytes), server_at(0U));
        static_cast<void>(network->client_loop.poll());
        static_cast<void>(network->client_loop.poll());
        CHECK(network->app.closed == 1U);
        CHECK(network->app.state(handle).close.version_negotiation);
        CHECK(network->app.state(handle).close.retry_version == static_cast<u32>(QuicVersion::V1));
        CHECK(network->client.counters().restarts == 1U);
        CHECK(network->client.live() == 0U);
        CHECK(network->client.connection(handle) == nullptr);
    }
    {
        // A client of version 1 alone gets nothing it runs: the connection ends.
        auto network = std::make_unique<Network>(identity, setup, client_config(identity, QuicVersion::V1, {}));
        const QuicHandle handle = network->connect(0U, 5U);
        REQUIRE(network->run_until([&] { return network->app.closed == 1U; }));
        CHECK(network->app.state(handle).close.version_negotiation);
        CHECK(network->app.state(handle).close.retry_version == 0U);
        CHECK(network->client.counters().restarts == 0U);
        CHECK(network->client.live() == 0U);
    }
}

TEST_CASE("QUIC client: a Retry is taken, and the NEW_TOKEN token it is given proves the next connection's address "
          "once",
          "[net][4.2a][quic-client]")
{
    for (const QuicVersion version : kVersions)
    {
        INFO("version " << static_cast<u32>(version));
        Identity identity;
        Setup setup;
        setup.always_retry = true;
        auto network = std::make_unique<Network>(identity, setup, client_config(identity, version, {}));
        hostile(*network, 41U + static_cast<u64>(version));
        const QuicHandle first = network->connect(0U, 1U);
        REQUIRE(network->run_until(
            [&] { return network->app.echoed(first) && !network->client.token_for(server_at(0U), version).empty(); }));
        CHECK(network->live(first).retried());
        const IngressCounters& ingress = network->server(0U).ingress.counters();
        CHECK(ingress.retries >= 1U);
        const u64 retries = ingress.retries;
        network->live(first).close(0U, {}, network->now);
        network->wake(first);
        REQUIRE(network->run_until([&] { return network->client.live() == 0U; }));

        const QuicHandle second = network->connect(0U, 2U);
        CHECK(network->client.counters().tokens_used == 1U);
        CHECK(network->client.token_for(server_at(0U), version).empty()); // carried once
        REQUIRE(network->run_until([&] { return network->app.echoed(second); }));
        CHECK_FALSE(network->live(second).retried());
        CHECK(ingress.retries == retries);
        CHECK(ingress.admitted_validated == 2U);
    }
}

TEST_CASE("QUIC client: a ticket's 0-RTT is written in the connection's first service and read by the server before "
          "the handshake completes",
          "[net][4.2a][quic-client][quic-early]")
{
    for (const QuicVersion version : kVersions)
    {
        INFO("version " << static_cast<u32>(version));
        ReadyFilter early;
        Identity identity(false, &early.filter);
        Setup setup;
        setup.early_context = kAppContext;
        setup.early_context_bytes = sizeof(kAppContext);
        Tickets tickets;
        auto network = std::make_unique<Network>(identity, setup, client_config(identity, version, {}, &tickets));
        const QuicHandle first = network->connect(0U, 1U);
        REQUIRE(network->run_until([&] { return network->app.echoed(first) && tickets.holds(); }));
        CHECK_FALSE(network->app.state(first).wrote_handshaking);

        const QuicHandle second = network->connect(0U, 2U);
        REQUIRE(network->live(second).handshake().early_data() == TlsEarlyData::Offered);
        REQUIRE(network->run_until([&] { return network->app.echoed(second); }));
        CHECK(network->live(second).handshake().early_data() == TlsEarlyData::Accepted);
        CHECK(network->app.state(second).wrote_handshaking);
        CHECK(network->server(0U).app.read_handshaking);
        CHECK(network->server(0U).app.early_bytes != 0U);
    }
}

TEST_CASE("QUIC client: the server's stateless reset ends a connection only from the server's address",
          "[net][4.2a][quic-client]")
{
    Identity identity;
    auto network = std::make_unique<Network>(identity, Setup{},
                                             client_config(identity, QuicVersion::V1, ConstSpan<u32>(kBothVersions)));
    const QuicHandle handle = network->connect(0U, 1U);
    REQUIRE(network->run_until([&] { return network->app.echoed(handle); }));
    QuicConnection& connection = network->live(handle);
    const Datagram reset = network->server_reset(handle);

    // The reset from another address ends nothing, nor do its last 20 or 9 bytes from the server's (a reset is at
    // least 21 bytes); the whole reset from the server's address drains the connection.
    network->deliver_to_client(reset.view(), kStranger);
    static_cast<void>(network->client_loop.poll());
    for (const usize tail : {usize{20}, usize{9}})
    {
        Datagram cut;
        cut.bytes[0] = reset.bytes[0];
        std::memcpy(cut.bytes + 1U, reset.bytes + reset.size - (tail - 1U), tail - 1U);
        cut.size = tail;
        network->deliver_to_client(cut.view(), server_at(0U));
        static_cast<void>(network->client_loop.poll());
    }
    CHECK(network->client.counters().peer_resets == 0U);
    CHECK(connection.state() == QuicConnectionState::Established);
    network->deliver_to_client(reset.view(), server_at(0U));
    static_cast<void>(network->client_loop.poll());
    CHECK(network->client.counters().peer_resets == 1U);
    REQUIRE(network->run_until([&] { return network->app.closed == 1U; }));
    CHECK(network->app.state(handle).close.stateless_reset);
    CHECK(network->client.live() == 0U);
}

TEST_CASE("QUIC client: a stateless reset is matched by its token among all the connections to one server",
          "[net][4.2a][quic-client]")
{
    Identity identity;
    auto network = std::make_unique<Network>(identity, Setup{},
                                             client_config(identity, QuicVersion::V1, ConstSpan<u32>(kBothVersions)));
    QuicHandle handles[3];
    for (usize index = 0U; index < 3U; ++index)
    {
        handles[index] = network->connect(0U, index + 1U);
    }
    REQUIRE(network->run_until(
        [&]
        {
            return network->app.echoed(handles[0]) && network->app.echoed(handles[1]) &&
                   network->app.echoed(handles[2]);
        }));
    CHECK(network->client.live_tokens() == 3U);

    // Each of the three is a connection of the same address: the reset names the last, and the others stay.
    const Datagram reset = network->server_reset(handles[2]);
    network->deliver_to_client(reset.view(), server_at(0U));
    static_cast<void>(network->client_loop.poll());
    CHECK(network->client.counters().peer_resets == 1U);
    CHECK(network->live(handles[0]).state() == QuicConnectionState::Established);
    CHECK(network->live(handles[1]).state() == QuicConnectionState::Established);
    REQUIRE(network->run_until([&] { return network->app.closed == 1U; }));
    CHECK(network->app.state(handles[2]).close.stateless_reset);
    CHECK(network->client.live() == 2U);
    CHECK(network->client.live_tokens() == 2U);
}

TEST_CASE("QUIC client: its own ticket cache resumes the next connection once, may be off, or replaced by a store",
          "[net][4.2a][quic-client][quic-early]")
{
    const auto early_network = [](Identity& identity, const QuicClientConfig& config)
    {
        Setup setup;
        setup.early_context = kAppContext;
        setup.early_context_bytes = sizeof(kAppContext);
        return std::make_unique<Network>(identity, setup, config);
    };
    for (const QuicVersion version : kVersions)
    {
        INFO("version " << static_cast<u32>(version));
        ReadyFilter early;
        Identity identity(false, &early.filter);
        auto network = early_network(identity, client_config(identity, version, {}));
        const QuicHandle first = network->connect(0U, 1U);
        REQUIRE(
            network->run_until([&] { return network->app.echoed(first) && network->client.tickets().size() != 0U; }));
        const u32 held = network->client.tickets().size();

        const QuicHandle second = network->connect(0U, 2U);
        REQUIRE(network->live(second).handshake().early_data() == TlsEarlyData::Offered);
        CHECK(network->client.tickets().size() == held - 1U);
        CHECK(network->client.tickets().counters().taken == 1U);
        REQUIRE(network->run_until([&] { return network->app.echoed(second); }));
        CHECK(network->live(second).handshake().early_data() == TlsEarlyData::Accepted);
    }
    {
        // Entries 0: nothing is kept, and the next connection offers none.
        ReadyFilter early;
        Identity identity(false, &early.filter);
        QuicClientConfig config = client_config(identity, QuicVersion::V1, {});
        config.tickets.entries = 0U;
        auto network = early_network(identity, config);
        const QuicHandle first = network->connect(0U, 1U);
        REQUIRE(network->run_until([&] { return network->app.echoed(first); }));
        CHECK(network->client.tickets().counters().kept == 0U);
        const QuicHandle second = network->connect(0U, 2U);
        CHECK(network->live(second).handshake().early_data() == TlsEarlyData::None);
    }
    {
        // A store of the caller's takes the tickets; the client's own stays empty.
        ReadyFilter early;
        Identity identity(false, &early.filter);
        Tickets tickets;
        auto network = early_network(identity, client_config(identity, QuicVersion::V1, {}, &tickets));
        const QuicHandle first = network->connect(0U, 1U);
        REQUIRE(network->run_until([&] { return network->app.echoed(first) && tickets.holds(); }));
        CHECK(network->client.tickets().counters().kept == 0U);
    }
    for (const bool turn : {true, false})
    {
        // A ticket past the age limit is gone within a turn, or before the next connection would offer it.
        INFO("turn " << turn);
        ReadyFilter early;
        Identity identity(false, &early.filter);
        QuicClientConfig config = client_config(identity, QuicVersion::V1, {});
        config.tickets.max_age_ns = 1000U * kMs;
        auto network = early_network(identity, config);
        const QuicHandle first = network->connect(0U, 1U);
        REQUIRE(
            network->run_until([&] { return network->app.echoed(first) && network->client.tickets().size() != 0U; }));
        if (turn)
        {
            REQUIRE(network->run_until([&] { return network->client.tickets().size() == 0U; }));
        }
        else
        {
            network->now += 2000U * kMs;
            const QuicHandle second = network->connect(0U, 2U);
            CHECK(network->live(second).handshake().early_data() == TlsEarlyData::None);
            CHECK(network->client.tickets().size() == 0U);
        }
        CHECK(network->client.tickets().counters().expired != 0U);
    }
}

TEST_CASE("QUIC client: a datagram for a connection from another address is dropped and counted",
          "[net][4.2a][quic-client]")
{
    Identity identity;
    auto network = std::make_unique<Network>(identity, Setup{},
                                             client_config(identity, QuicVersion::V1, ConstSpan<u32>(kBothVersions)));
    const QuicHandle handle = network->connect(0U, 1U);
    REQUIRE(network->run_until([&] { return network->live(handle).confirmed(); }));
    // The server's next datagram, copied: the copy arrives from another address first.
    QuicConnection* const served = network->server(0U).server.connection(network->server(0U).app.last);
    REQUIRE(served != nullptr);
    served->ping();
    network->server(0U).server.wake(network->server(0U).app.last);
    static_cast<void>(network->server(0U).loop.poll());
    VirtualSocket& socket = network->server(0U).socket;
    REQUIRE(socket.sent_count >= 1U);
    const Datagram copy = socket.sent[0].datagram;
    const u64 received = network->live(handle).counters().datagrams_received;
    network->deliver_to_client(copy.view(), kStranger);
    static_cast<void>(network->client_loop.poll());
    CHECK(network->client.counters().foreign_path == 1U);
    CHECK(network->live(handle).counters().datagrams_received == received);
    network->deliver_to_client(copy.view(), server_at(0U));
    static_cast<void>(network->client_loop.poll());
    CHECK(network->live(handle).counters().datagrams_received == received + 1U);
}

TEST_CASE("QUIC client: the pool's bounds, stale handles, a TLS context per connection and the configurations "
          "refused",
          "[net][4.2a][quic-client]")
{
    Identity identity;
    Identity stranger(true); // a client context pinning another key
    {
        QuicClientConfig config = client_config(identity, QuicVersion::V1, ConstSpan<u32>(kBothVersions));
        config.connections = 2U;
        auto network = std::make_unique<Network>(identity, Setup{}, config);
        const QuicHandle a = network->connect(0U, 1U);
        const QuicHandle b = network->connect(0U, 2U, &stranger.client);
        REQUIRE(a.valid());
        REQUIRE(b.valid());
        QuicClientConnect request;
        request.server = server_at(0U);
        CHECK_FALSE(network->client.connect(request, network->now).valid()); // the pool is full
        CHECK(network->client.counters().refused == 1U);

        // The connection given the stranger's context refuses the server's key; the other completes.
        REQUIRE(network->run_until([&] { return network->app.echoed(a) && network->app.closed == 1U; }));
        CHECK(network->app.state(b).close.error_code >= 0x100U);
        CHECK(network->app.state(b).close.error_code < 0x200U);
        CHECK(network->client.connection(b) == nullptr);
        network->client.wake(b); // a stale handle wakes nothing

        const QuicHandle c = network->connect(0U, 3U);
        REQUIRE(c.valid());
        CHECK(c.slot == b.slot);
        CHECK(c.generation == b.generation + 1U);
        CHECK(network->client.connection(b) == nullptr);
        CHECK(network->client.connection(c) != nullptr);

        network->live(c).close(0U, {}, network->now);
        network->wake(c);
        REQUIRE(network->run_until([&] { return network->client.live() == 1U; }));
        request.server = Endpoint{IpAddress::v4(10, 0, 0, 1), 0U};
        CHECK_FALSE(network->client.connect(request, network->now).valid()); // port 0
        u8 long_token[kQuicMaxCarriedTokenBytes + 1U] = {};
        request.server = server_at(0U);
        request.token = ConstSpan<u8>(long_token);
        CHECK_FALSE(network->client.connect(request, network->now).valid());
        CHECK(network->client.counters().refused == 3U);

        // A client's ingress produces no admission and no early datagram; were one handed over, it is dropped.
        network->client.on_admit(IngressDecision{}, ReceivedDatagram{}, network->now);
        network->client.on_early(IngressDecision{}, ReceivedDatagram{}, network->now);
        CHECK(network->client.counters().unexpected == 2U);
    }

    CountingAllocator allocator;
    ConnectionIdTable routes(allocator, 64U, ConstSpan<u8>(kHostPrf));
    QuicResetKey reset;
    REQUIRE(reset.init(ConstSpan<u8>(kClientSecret)) == CryptoError::None);
    Ingress ingress(client_ingress_config(), routes);
    IngressConfig v1_only = client_ingress_config();
    v1_only.version_count = 1U;
    Ingress narrow(v1_only, routes);
    HandshakeBuckets buckets(allocator, HandshakeRules{});
    Ingress server_ingress(IngressConfig{}, routes, buckets, nullptr, nullptr);
    VirtualSocket socket;
    ClientApp app;
    const QuicClientConfig good = client_config(identity, QuicVersion::V1, ConstSpan<u32>(kBothVersions));
    const auto ready = [&](const QuicClientConfig& config, const Ingress& with)
    {
        const QuicClient client(allocator, socket, with, routes, reset, app, config);
        return client.ready();
    };
    CHECK(ready(good, ingress));
    CHECK_FALSE(ready(good, server_ingress));
    CHECK_FALSE(ready(good, narrow)); // version 2 is in its preference
    const auto refused = [&](auto change)
    {
        QuicClientConfig config = good;
        change(config);
        return !ready(config, ingress);
    };
    CHECK(refused([](QuicClientConfig& c) { c.connection.side = QuicSide::Server; }));
    CHECK(refused([](QuicClientConfig& c) { c.connection.tls = nullptr; }));
    CHECK(refused([](QuicClientConfig& c) { c.connection.max_datagram_bytes = 1199U; }));
    CHECK(refused([](QuicClientConfig& c) { c.connection.entropy_seed = ConstSpan<u8>(kHostPrf, 5U); }));
    CHECK(refused([](QuicClientConfig& c) { c.connections = 0U; }));
    CHECK(refused([](QuicClientConfig& c) { c.connections = kQuicMaxHostConnections + 1U; }));
    CHECK(refused([](QuicClientConfig& c) { c.tokens = kQuicMaxClientTokens + 1U; }));
    CHECK(refused([](QuicClientConfig& c) { c.tickets.per_binding = 0U; }));
    CHECK(refused([](QuicClientConfig& c) { c.send_slots = 0U; }));
    CHECK(refused([](QuicClientConfig& c) { c.send_retry_ns = 0U; }));
    CHECK_FALSE(refused([](QuicClientConfig& c) { c.tokens = 0U; }));

    // Every allocation it makes, refused in turn, leaves it not ready (and leaks nothing: the sanitizers' lanes).
    bool built = false;
    usize refusals = 0U;
    for (usize allowed = 0U; !built && allowed < 64U; ++allowed)
    {
        allocator.refuse_after(allocator.calls() + allowed);
        built = ready(good, ingress);
        allocator.refuse_after(~usize{0});
        refusals += built ? 0U : 1U;
    }
    CHECK(built);
    CHECK(refusals > 6U);
}

TEST_CASE("QUIC client: a client host is served over the host's loopback through real UDP sockets",
          "[net][4.2a][quic-client]")
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
    const HostClock clock;
    ConnectionIdTable routes(allocator, 64U);
    HandshakeBuckets buckets(allocator, HandshakeRules{});
    QuicResetKey reset;
    REQUIRE(reset.init(ConstSpan<u8>(kResetSecret)) == CryptoError::None);
    QuicTokenKeys tokens;
    REQUIRE(tokens.init(ConstSpan<u8>(kTokenKey), {}) == CryptoError::None);
    Ingress ingress(IngressConfig{}, routes, buckets, &tokens, nullptr, &reset);
    EchoApp echo;
    QuicServerConfig config;
    config.connection = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
    config.connection.streams = limits();
    config.connection.entropy_seed = {};
    QuicServer server(allocator, server_socket, clock, ingress, routes, reset, &tokens, echo, config);
    IoLoop server_loop(allocator, server_socket, clock, ingress, server, IoLoopConfig{});
    REQUIRE(server.ready());
    REQUIRE(server_loop.ready());

    ConnectionIdTable client_routes(allocator, 64U);
    QuicResetKey client_reset;
    REQUIRE(client_reset.init(ConstSpan<u8>(kClientSecret)) == CryptoError::None);
    Ingress client_ingress(client_ingress_config(), client_routes);
    ClientApp app;
    QuicClientConfig client_settings = client_config(identity, QuicVersion::V1, {});
    client_settings.connection.entropy_seed = {};
    QuicClient client(allocator, client_socket, client_ingress, client_routes, client_reset, app, client_settings);
    IoLoop client_loop(allocator, client_socket, clock, client_ingress, client, IoLoopConfig{});
    REQUIRE(client.ready());
    REQUIRE(client_loop.ready());

    QuicClientConnect request;
    request.server = server_socket.local_endpoint();
    const QuicHandle handle = client.connect(request, clock.monotonic_ns());
    app.begin(handle, 9U);
    const u64 deadline = clock.monotonic_ns() + (10U * kNanosPerSecond);
    while (!app.echoed(handle) && clock.monotonic_ns() < deadline)
    {
        static_cast<void>(client_loop.poll());
        static_cast<void>(server_socket.wait_readable(kMs));
        static_cast<void>(server_loop.poll());
        static_cast<void>(client_socket.wait_readable(kMs));
    }
    CHECK(app.echoed(handle));
    CHECK(server.live() == 1U);
    const QuicConnection* const connection = client.connection(handle);
    REQUIRE(connection != nullptr);
    CHECK(connection->confirmed());
    CHECK(client.counters().datagrams_sent != 0U);
    // The server's NEW_TOKEN token is remembered for its address.
    const u64 token_deadline = clock.monotonic_ns() + (5U * kNanosPerSecond);
    while (client.token_for(request.server, QuicVersion::V1).empty() && clock.monotonic_ns() < token_deadline)
    {
        static_cast<void>(client_loop.poll());
        static_cast<void>(server_socket.wait_readable(kMs));
        static_cast<void>(server_loop.poll());
        static_cast<void>(client_socket.wait_readable(kMs));
    }
    CHECK_FALSE(client.token_for(request.server, QuicVersion::V1).empty());
}
