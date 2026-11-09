// crd-net -- the TCP and TLS path's host: over pipes under a test clock, and over the host's TCP on the loopback (4.2a;
// docs/design/network-foundation.md#stream-host).

#include "ingress_fixture.hpp"
#include "loop_fixture.hpp"
#include "quic_connection_fixture.hpp"
#include "quic_stream_fixture.hpp"
#include "stream_pipe_fixture.hpp"

#include <crd/net/ingress.hpp>
#include <crd/net/qmux_host.hpp>
#include <crd/net/ticket_cache.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace
{
using namespace crd;
using namespace crd::net;
using namespace crd::net::testing;
using crd::containers::ConstSpan;

constexpr u64 kMsNs = 1'000'000U;

QmuxConfig connection_config()
{
    QuicStreamLimits streams;
    streams.peer_bidi = 8U;
    streams.peer_uni = 4U;
    streams.local_bidi = 8U;
    streams.local_uni = 4U;
    streams.receive_bytes = 64U * 1024U;
    streams.send_bytes = 64U * 1024U;
    streams.stream_window = 32U * 1024U;
    streams.connection_window = 64U * 1024U;
    streams.connection_max_window = 256U * 1024U;
    QuicDatagramLimits datagrams;
    datagrams.max_frame_bytes = 1200U;
    datagrams.receive_bytes = 8192U;
    datagrams.receive_count = 32U;
    datagrams.send_bytes = 8192U;
    datagrams.send_count = 32U;
    QmuxConfig config;
    config.streams = streams;
    config.datagrams = datagrams;
    return config;
}

// The application's side: records what the host reports and, when `transfer` is set, sends that many bytes each way on
// the client's first stream.
struct Recorder final : QmuxHostEvents
{
    struct Ended
    {
        QuicHandle handle;
        QmuxCloseInfo info;
        SocketError transport = SocketError::None;
    };

    struct Flow
    {
        bool opened = false;
        bool read_done = false;
        u64 written = 0U;
        u64 read = 0U;
    };

    static constexpr usize kMost = 64U;

    void on_accept(QuicHandle handle, QmuxConnection&, const Endpoint& peer) noexcept override
    {
        last_peer = peer;
        accepted[accepted_count++] = handle;
    }

    void on_service(QuicHandle handle, QmuxConnection& connection, u64) noexcept override
    {
        ++serviced;
        if (transfer == 0U || connection.state() != QmuxState::Established || !connection.peer_parameters_known())
        {
            return;
        }
        Flow& flow = flows[handle.slot];
        if (client && !flow.opened)
        {
            u64 id = 0U;
            flow.opened = connection.streams().open(true, id) == QuicStreamError::None;
        }
        if (!client && connection.streams().recv_state(0U) != QuicRecvState::None)
        {
            flow.opened = true;
        }
        if (flow.opened && flow.written < transfer)
        {
            static_cast<void>(write_pattern(connection.streams(), 0U, flow.written, transfer, client ? 3U : 5U));
        }
        if (flow.opened && !flow.read_done)
        {
            flow.read_done = drain(connection.streams(), 0U, flow.read, client ? 5U : 3U);
        }
    }

    void on_closed(QuicHandle handle, const QmuxConnection& connection, SocketError transport) noexcept override
    {
        ended[ended_count++] = Ended{handle, connection.close_info(), transport};
    }

    [[nodiscard]] bool done(u32 slot) const noexcept { return flows[slot].read_done && flows[slot].read == transfer; }

    bool client = false;
    u64 transfer = 0U;
    QuicHandle accepted[kMost];
    usize accepted_count = 0U;
    Ended ended[kMost];
    usize ended_count = 0U;
    Flow flows[kMost];
    Endpoint last_peer;
    usize serviced = 0U;
};

HandshakeRules open_rules()
{
    return HandshakeRules{};
}

// A server host and a client host over one pipe network, under a test clock.
struct Hosts
{
    ReadyFilter ready;
    Identity identity;
    CountingAllocator allocator;
    TestClock clock;
    PipeNetwork network;
    Endpoint server_address{IpAddress::v4_loopback(), 4433U};
    PipePoller server_poller{network, server_address};
    PipePoller client_poller{network, Endpoint{IpAddress::v4_loopback(), 50000U}};
    HandshakeBuckets buckets;
    Recorder server_events;
    Recorder client_events;
    QmuxHostConfig server_config;
    QmuxHostConfig client_config;
    std::unique_ptr<QmuxHost> server;
    std::unique_ptr<QmuxHost> client;

    explicit Hosts(const HandshakeRules& rules = open_rules(), u32 server_connections = 8U, bool early = false)
        : identity(false, early ? &ready.filter : nullptr), buckets(allocator, rules)
    {
        network.listener = &server_poller;
        server_config.connection = connection_config();
        server_config.connection.tls = &identity.server;
        server_config.connections = server_connections;
        client_config.connection = connection_config();
        client_config.connections = 16U;
        client_events.client = true;
    }

    void start()
    {
        REQUIRE(buckets.ready());
        server = std::make_unique<QmuxHost>(allocator, server_poller, clock, &buckets, server_events, server_config);
        client = std::make_unique<QmuxHost>(allocator, client_poller, clock, nullptr, client_events, client_config);
        REQUIRE(server->ready());
        REQUIRE(client->ready());
    }

    QuicHandle connect()
    {
        SocketError error = SocketError::None;
        const QuicHandle handle = client->connect(QmuxConnect{server_address, &identity.client}, clock.now, error);
        REQUIRE(error == SocketError::None);
        REQUIRE(handle.valid());
        return handle;
    }

    void turns(usize count, u64 step_ns = kMsNs)
    {
        for (usize turn = 0U; turn < count; ++turn)
        {
            static_cast<void>(server->poll(0U));
            static_cast<void>(client->poll(0U));
            clock.now += step_ns;
        }
    }

    // Turns until the client's connection is established.
    void establish(QuicHandle handle)
    {
        for (usize turn = 0U; turn < 64U; ++turn)
        {
            turns(1U);
            QmuxConnection* const connection = client->connection(handle);
            if (connection != nullptr && connection->state() == QmuxState::Established &&
                connection->peer_parameters_known())
            {
                return;
            }
        }
        FAIL("not established");
    }
};

} // namespace

TEST_CASE("QmuxHost: a client and a server carry a transfer both ways and close cleanly", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.start();
    hosts.client_events.transfer = 2U * 1024U * 1024U;
    hosts.server_events.transfer = 2U * 1024U * 1024U;
    const QuicHandle handle = hosts.connect();
    CHECK(hosts.client->live() == 1U);
    for (usize turn = 0U; turn < 20000U && !(hosts.client_events.done(handle.slot) && hosts.server_events.done(0U));
         ++turn)
    {
        hosts.turns(1U);
    }
    CHECK(hosts.client_events.done(handle.slot));
    CHECK(hosts.server_events.done(0U));
    CHECK(hosts.server->counters().accepted == 1U);
    CHECK(hosts.client->counters().connected == 1U);
    CHECK(hosts.server_events.accepted_count == 1U);
    CHECK(hosts.server_events.last_peer.port == 50000U);
    CHECK(hosts.client->counters().bytes_written > hosts.client_events.transfer);
    CHECK(hosts.server->counters().bytes_read > hosts.client_events.transfer);

    QmuxConnection* const connection = hosts.client->connection(handle);
    REQUIRE(connection != nullptr);
    connection->close(7U, ConstSpan<u8>(), hosts.clock.now);
    hosts.client->wake(handle);
    hosts.turns(16U);

    REQUIRE(hosts.client_events.ended_count == 1U);
    REQUIRE(hosts.server_events.ended_count == 1U);
    CHECK(hosts.client_events.ended[0].info.reason == QmuxCloseReason::Local);
    CHECK(hosts.client_events.ended[0].transport == SocketError::None);
    CHECK(hosts.server_events.ended[0].info.reason == QmuxCloseReason::Peer);
    CHECK(hosts.server_events.ended[0].info.application);
    CHECK(hosts.server_events.ended[0].info.error_code == 7U);
    CHECK(hosts.client->live() == 0U);
    CHECK(hosts.server->live() == 0U);
    CHECK(hosts.client_poller.fins == 1U);
    CHECK(hosts.server_poller.fins == 1U);
    CHECK(hosts.client_poller.resets == 0U);
    CHECK(hosts.server_poller.resets == 0U);
    CHECK(hosts.client->counters().closed == 1U);
    CHECK(hosts.client->counters().resets_sent == 0U);
    CHECK(hosts.client->connection(handle) == nullptr);
}

TEST_CASE("QmuxHost: many connections finish at once, and their slots are reused under new handles", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.start();
    hosts.client_events.transfer = 128U * 1024U;
    hosts.server_events.transfer = 128U * 1024U;
    QuicHandle handles[8];
    for (QuicHandle& handle : handles)
    {
        handle = hosts.connect();
    }
    const auto finished = [&hosts]
    {
        for (u32 slot = 0U; slot < 8U; ++slot)
        {
            if (!hosts.client_events.done(slot) || !hosts.server_events.done(slot))
            {
                return false;
            }
        }
        return true;
    };
    for (usize turn = 0U; turn < 20000U && !finished(); ++turn)
    {
        hosts.turns(1U);
    }
    CHECK(finished());
    CHECK(hosts.server->live() == 8U);
    for (const QuicHandle& handle : handles)
    {
        hosts.client->connection(handle)->close(1U, ConstSpan<u8>(), hosts.clock.now);
        hosts.client->wake(handle);
    }
    hosts.turns(16U);
    CHECK(hosts.client->live() == 0U);
    CHECK(hosts.server->live() == 0U);

    hosts.client_events.transfer = 0U;
    const QuicHandle again = hosts.connect();
    hosts.establish(again);
    CHECK(again.generation == 1U);
    CHECK(hosts.client->connection(handles[again.slot]) == nullptr);
    CHECK(hosts.client->connection(again) != nullptr);
    CHECK(hosts.client->connection(QuicHandle{}) == nullptr);
}

TEST_CASE("QmuxHost: a resumed client's first stream goes as 0-RTT and is read marked", "[net][qmux-host]")
{
    Hosts hosts(open_rules(), 8U, true);
    TicketCache tickets(hosts.allocator, TicketCacheConfig{});
    hosts.client_config.connection.tickets = &tickets;
    hosts.start();
    hosts.establish(hosts.connect());
    hosts.turns(8U);

    const QuicHandle resumed = hosts.connect();
    QmuxConnection* const client = hosts.client->connection(resumed);
    REQUIRE(client != nullptr);
    REQUIRE(client->handshake().early_data() == crypto::TlsEarlyData::Offered);
    u64 id = 0U;
    REQUIRE(client->streams().open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    REQUIRE(write_pattern(client->streams(), id, written, 5000U, 3U));
    hosts.client->wake(resumed);
    hosts.turns(8U);

    REQUIRE(hosts.server_events.accepted_count == 2U);
    QmuxConnection* const server = hosts.server->connection(hosts.server_events.accepted[1]);
    REQUIRE(server != nullptr);
    CHECK(client->handshake().early_data() == crypto::TlsEarlyData::Accepted);
    CHECK(server->handshake().early_data() == crypto::TlsEarlyData::Accepted);
    u64 read = 0U;
    u64 marked = 0U;
    CHECK(drain(server->streams(), id, read, 3U, &marked));
    CHECK(read == 5000U);
    CHECK(marked == 5000U);
}

TEST_CASE("QmuxHost: a connection with no slot is reset", "[net][qmux-host]")
{
    Hosts hosts(open_rules(), 1U);
    hosts.start();
    const QuicHandle first = hosts.connect();
    hosts.establish(first);
    const QuicHandle second = hosts.connect();
    hosts.turns(4U);
    CHECK(hosts.server->counters().refused_full == 1U);
    CHECK(hosts.server->counters().accepted == 1U);
    CHECK(hosts.server_poller.resets == 1U);
    CHECK(hosts.server_events.accepted_count == 1U);
    REQUIRE(hosts.client_events.ended_count == 1U);
    CHECK(hosts.client_events.ended[0].handle == second);
    CHECK(hosts.client_events.ended[0].info.reason == QmuxCloseReason::Reset);
    CHECK(hosts.client_events.ended[0].transport == SocketError::Reset);
    CHECK(hosts.client->connection(first) != nullptr);
}

TEST_CASE("QmuxHost: a connection past the handshake rate is reset, charged as a proven address", "[net][qmux-host]")
{
    HandshakeRules rules;
    rules.validated = GateRule{1U, 0U};
    Hosts hosts(rules);
    hosts.start();
    const QuicHandle first = hosts.connect();
    hosts.establish(first);
    static_cast<void>(hosts.connect());
    hosts.turns(4U);
    CHECK(hosts.server->counters().accepted == 1U);
    CHECK(hosts.server->counters().refused_rate == 1U);
    CHECK(hosts.server_poller.resets == 1U);
    CHECK(hosts.client->connection(first) != nullptr);
    CHECK(hosts.client_events.ended_count == 1U);
}

TEST_CASE("QmuxHost: a TCP peer has proved its address, so the unproven rates do not apply", "[net][qmux-host]")
{
    HandshakeRules rules;
    rules.unvalidated = GateRule{1U, 0U};
    rules.per_prefix = GateRule{1U, 0U};
    Hosts hosts(rules);
    hosts.start();
    static_cast<void>(hosts.connect());
    static_cast<void>(hosts.connect());
    hosts.turns(4U);
    CHECK(hosts.server->counters().accepted == 2U);
    CHECK(hosts.server->counters().refused_rate == 0U);
}

TEST_CASE("QmuxHost: an accepted connection that never speaks times out with a reset", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.start();
    PipePoller silent(hosts.network, Endpoint{IpAddress::v4_loopback(), 50001U});
    SocketError error = SocketError::None;
    REQUIRE(silent.connect(0U, hosts.server_address, error) != nullptr);
    hosts.turns(2U);
    CHECK(hosts.server->live() == 1U);
    CHECK(hosts.server_events.ended_count == 0U);
    hosts.clock.now += hosts.server_config.connection.handshake_timeout_ns;
    hosts.turns(1U);
    REQUIRE(hosts.server_events.ended_count == 1U);
    CHECK(hosts.server_events.ended[0].info.reason == QmuxCloseReason::HandshakeTimeout);
    CHECK(hosts.server_events.ended[0].transport == SocketError::None);
    CHECK(hosts.server_poller.resets == 1U);
    CHECK(hosts.server->counters().resets_sent == 1U);
}

TEST_CASE("QmuxHost: garbage on a new connection ends it", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.start();
    PipePoller raw(hosts.network, Endpoint{IpAddress::v4_loopback(), 50001U});
    SocketError error = SocketError::None;
    StreamSocket* const socket = raw.connect(0U, hosts.server_address, error);
    REQUIRE(socket != nullptr);
    hosts.turns(1U);
    const u8 garbage[64] = {0x16U, 0x03U, 0x01U, 0xFFU, 0xFFU};
    REQUIRE(socket->write(ConstSpan<u8>(garbage, sizeof(garbage))).bytes == sizeof(garbage));
    hosts.turns(2U);
    REQUIRE(hosts.server_events.ended_count == 1U);
    CHECK(hosts.server_events.ended[0].info.reason == QmuxCloseReason::TlsFailed);
    CHECK(hosts.server->live() == 0U);
}

TEST_CASE("QmuxHost: the idle timeout ends a connection with close_notify, not a reset", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.server_config.connection.idle_timeout_ns = 500U * kMsNs;
    hosts.client_config.connection.idle_timeout_ns = 500U * kMsNs;
    hosts.start();
    const QuicHandle handle = hosts.connect();
    hosts.establish(handle);
    hosts.turns(600U);
    REQUIRE(hosts.client_events.ended_count == 1U);
    REQUIRE(hosts.server_events.ended_count == 1U);
    const QmuxCloseReason client_reason = hosts.client_events.ended[0].info.reason;
    const QmuxCloseReason server_reason = hosts.server_events.ended[0].info.reason;
    CHECK((client_reason == QmuxCloseReason::IdleTimeout || client_reason == QmuxCloseReason::PeerShutdown));
    CHECK((server_reason == QmuxCloseReason::IdleTimeout || server_reason == QmuxCloseReason::PeerShutdown));
    CHECK(hosts.client_poller.resets == 0U);
    CHECK(hosts.server_poller.resets == 0U);
}

TEST_CASE("QmuxHost: a closed side stops waiting after close_wait", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.client_config.connection.close_wait_ns = 200U * kMsNs;
    hosts.start();
    const QuicHandle handle = hosts.connect();
    hosts.establish(handle);
    hosts.client->connection(handle)->close(9U, ConstSpan<u8>(), hosts.clock.now);
    hosts.client->wake(handle);
    static_cast<void>(hosts.client->poll(0U));
    PipeLink* const link = hosts.client_poller.link_of(handle.slot);
    CHECK(link->to_server.ended); // close_notify and the FIN went out
    CHECK(hosts.client_events.ended_count == 0U);
    hosts.clock.now += 199U * kMsNs;
    static_cast<void>(hosts.client->poll(0U));
    CHECK(hosts.client_events.ended_count == 0U);
    hosts.clock.now += kMsNs;
    static_cast<void>(hosts.client->poll(0U));
    REQUIRE(hosts.client_events.ended_count == 1U);
    CHECK(hosts.client_events.ended[0].info.reason == QmuxCloseReason::Local);
    CHECK(hosts.client_poller.fins == 1U);
}

TEST_CASE("QmuxHost: a peer's reset and a truncation are reported as such", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.start();
    const QuicHandle reset = hosts.connect();
    hosts.establish(reset);
    const QuicHandle truncated = hosts.connect();
    hosts.establish(truncated);

    hosts.client_poller.link_of(reset.slot)->to_client.reset = true;
    hosts.client_poller.link_of(truncated.slot)->to_client.ended = true;
    static_cast<void>(hosts.client->poll(0U));
    REQUIRE(hosts.client_events.ended_count == 2U);
    for (usize at = 0U; at < 2U; ++at)
    {
        const Recorder::Ended& ended = hosts.client_events.ended[at];
        if (ended.handle == reset)
        {
            CHECK(ended.info.reason == QmuxCloseReason::Reset);
            CHECK(ended.transport == SocketError::Reset);
        }
        else
        {
            CHECK(ended.handle == truncated);
            CHECK(ended.info.reason == QmuxCloseReason::Truncated);
            CHECK(ended.transport == SocketError::None);
        }
    }
    CHECK(hosts.client_poller.resets == 1U); // a failed transport is reset, a truncated one ended
    CHECK(hosts.client_poller.fins == 1U);
}

TEST_CASE("QmuxHost: a socket that fails a write ends its connection", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.start();
    const QuicHandle handle = hosts.connect();
    hosts.establish(handle);
    hosts.client_poller.link_of(handle.slot)->client.write_error = SocketError::Reset;
    QmuxConnection* const connection = hosts.client->connection(handle);
    connection->ping();
    hosts.client->wake(handle);
    static_cast<void>(hosts.client->poll(0U));
    REQUIRE(hosts.client_events.ended_count == 1U);
    CHECK(hosts.client_events.ended[0].transport == SocketError::Reset);
    CHECK(hosts.client_poller.resets == 1U);
}

TEST_CASE("QmuxHost: back-pressure waits for the socket, without a byte lost or a busy turn", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.network.limit = 512U;
    hosts.start();
    hosts.client_events.transfer = 512U * 1024U;
    hosts.server_events.transfer = 512U * 1024U;
    const QuicHandle handle = hosts.connect();
    for (usize turn = 0U; turn < 200000U && !(hosts.client_events.done(handle.slot) && hosts.server_events.done(0U));
         ++turn)
    {
        hosts.turns(1U, 100000U);
    }
    CHECK(hosts.client_events.done(handle.slot));
    CHECK(hosts.server_events.done(0U));
    CHECK(hosts.client->counters().write_blocks != 0U);
    CHECK(hosts.server->counters().write_blocks != 0U);
    CHECK(hosts.client_poller.interests_of(handle.slot) == kStreamReadable);

    // A blocked connection does not try again until the socket says so.
    PipeLink* const link = hosts.client_poller.link_of(handle.slot);
    link->to_server.limit = 0U;
    QmuxConnection* const connection = hosts.client->connection(handle);
    connection->ping();
    hosts.client->wake(handle);
    static_cast<void>(hosts.client->poll(0U));
    CHECK(hosts.client_poller.interests_of(handle.slot) == static_cast<u8>(kStreamReadable | kStreamWritable));
    const usize writes = link->client.writes;
    for (usize turn = 0U; turn < 8U; ++turn)
    {
        hosts.client->wake(handle);
        static_cast<void>(hosts.client->poll(0U));
    }
    CHECK(link->client.writes == writes);
    link->to_server.limit = kPipeBytes;
    hosts.turns(2U);
    CHECK(hosts.client_poller.interests_of(handle.slot) == kStreamReadable);

    // A write the socket takes only part of is the last one tried: the host does not ask it again to hear Busy.
    link->to_server.limit = link->to_server.size + 8U;
    connection->ping();
    hosts.client->wake(handle);
    const usize before_partial = link->client.writes;
    static_cast<void>(hosts.client->poll(0U));
    CHECK(link->client.writes == before_partial + 1U);
    CHECK(hosts.client->counters().write_blocks != 0U);
    link->to_server.limit = kPipeBytes;
}

TEST_CASE("QmuxHost: a side that cannot send its close ends with a reset", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.client_config.connection.close_wait_ns = 100U * kMsNs;
    hosts.start();
    const QuicHandle handle = hosts.connect();
    hosts.establish(handle);
    PipeLink* const link = hosts.client_poller.link_of(handle.slot);
    link->to_server.limit = 0U;
    hosts.client->connection(handle)->close(3U, ConstSpan<u8>(), hosts.clock.now);
    hosts.client->wake(handle);
    static_cast<void>(hosts.client->poll(0U));
    CHECK(hosts.client_events.ended_count == 0U);
    CHECK_FALSE(link->to_server.ended);
    hosts.clock.now += 100U * kMsNs;
    static_cast<void>(hosts.client->poll(0U));
    REQUIRE(hosts.client_events.ended_count == 1U);
    CHECK(hosts.client_poller.resets == 1U);
    CHECK(hosts.client->counters().resets_sent == 1U);
}

TEST_CASE("QmuxHost: connect() names what stopped it and keeps the slot", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.client_config.connections = 1U;
    hosts.start();
    SocketError error = SocketError::None;
    CHECK_FALSE(hosts.client->connect(QmuxConnect{hosts.server_address, nullptr}, hosts.clock.now, error).valid());
    CHECK(error == SocketError::Failed);
    CHECK_FALSE(hosts.client
                    ->connect(QmuxConnect{Endpoint{IpAddress::v4_loopback(), 0U}, &hosts.identity.client},
                              hosts.clock.now, error)
                    .valid());
    hosts.network.connect_error = SocketError::Unreachable;
    CHECK_FALSE(hosts.client->connect(QmuxConnect{hosts.server_address, &hosts.identity.client}, hosts.clock.now, error)
                    .valid());
    CHECK(error == SocketError::Unreachable);
    CHECK(hosts.client->counters().connect_refused == 3U);
    CHECK(hosts.client->live() == 0U);

    hosts.network.connect_error = SocketError::None;
    const QuicHandle handle = hosts.connect();
    CHECK(handle.generation == 0U);
    CHECK_FALSE(hosts.client->connect(QmuxConnect{hosts.server_address, &hosts.identity.client}, hosts.clock.now, error)
                    .valid()); // every slot in use
    CHECK(error == SocketError::Failed);
}

TEST_CASE("QmuxHost: a server's context cannot make a client's connection", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.start();
    SocketError error = SocketError::None;
    // A context of the wrong role is still a TLS session; what cannot be made is one whose ALPN is missing.
    crypto::TlsContext empty;
    CHECK_FALSE(hosts.client->connect(QmuxConnect{hosts.server_address, &empty}, hosts.clock.now, error).valid());
    CHECK(error == SocketError::Failed);
    CHECK(hosts.client_poller.resets == 1U);
    CHECK(hosts.client->live() == 0U);
}

TEST_CASE("QmuxHost: a connection that fails after starting is reported with the reason", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.start();
    hosts.network.connected_error = SocketError::Refused;
    const QuicHandle handle = hosts.connect();
    hosts.turns(1U);
    REQUIRE(hosts.client_events.ended_count == 1U);
    CHECK(hosts.client_events.ended[0].handle == handle);
    CHECK(hosts.client_events.ended[0].transport == SocketError::Refused);
    CHECK(hosts.client_events.ended[0].info.reason == QmuxCloseReason::Reset);
    CHECK(hosts.client_poller.resets == 1U);
}

TEST_CASE("QmuxHost: a report before the connection is made changes nothing", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.network.connects_busy = true;
    hosts.start();
    const QuicHandle handle = hosts.connect();
    hosts.turns(2U);
    REQUIRE(hosts.client->connection(handle) != nullptr);
    CHECK(hosts.client_poller.link_of(handle.slot)->client.writes == 0U);
    CHECK(hosts.client_events.ended_count == 0U);
    hosts.network.connects_busy = false;
    hosts.establish(handle);
}

TEST_CASE("QmuxHost: a connection that is never made times out", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.network.connects_hang = true;
    hosts.start();
    const QuicHandle handle = hosts.connect();
    hosts.turns(2U);
    CHECK(hosts.client->connection(handle) != nullptr);
    CHECK(hosts.client_poller.link_of(handle.slot)->client.writes == 0U); // nothing is written before it stands
    hosts.clock.now += hosts.client_config.connection.handshake_timeout_ns;
    hosts.turns(1U);
    REQUIRE(hosts.client_events.ended_count == 1U);
    CHECK(hosts.client_events.ended[0].info.reason == QmuxCloseReason::HandshakeTimeout);
    CHECK(hosts.client_poller.resets == 1U);
}

TEST_CASE("QmuxHost: wake serves a live connection in the next turn, and a stale handle nothing", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.start();
    const QuicHandle handle = hosts.connect();
    hosts.establish(handle);
    const usize before = hosts.client_events.serviced;
    static_cast<void>(hosts.client->poll(0U));
    CHECK(hosts.client_events.serviced == before);
    hosts.client->wake(handle);
    static_cast<void>(hosts.client->poll(0U));
    CHECK(hosts.client_events.serviced == before + 1U);
    hosts.client->wake(QuicHandle{handle.slot, handle.generation + 1U});
    hosts.client->wake(QuicHandle{});
    static_cast<void>(hosts.client->poll(0U));
    CHECK(hosts.client_events.serviced == before + 1U);
}

TEST_CASE("QmuxHost: running out of descriptors pauses accepting, then resumes", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.server_config.accept_retry_ns = 50U * kMsNs;
    hosts.start();
    hosts.server_poller.accept_error = SocketError::Resources;
    static_cast<void>(hosts.connect());
    const u64 start = hosts.clock.now;
    const u64 next = hosts.server->poll(0U);
    CHECK(hosts.server->counters().accept_pauses == 1U);
    CHECK(hosts.server_poller.listener_interest == 0U);
    CHECK(next == start + 50U * kMsNs);
    hosts.clock.now += 49U * kMsNs;
    static_cast<void>(hosts.server->poll(0U));
    CHECK(hosts.server_poller.listener_interest == 0U);
    hosts.server_poller.accept_error = SocketError::None;
    hosts.clock.now += kMsNs;
    static_cast<void>(hosts.server->poll(0U));
    CHECK(hosts.server_poller.listener_interest == kStreamReadable);
    hosts.turns(2U);
    CHECK(hosts.server->counters().accepted == 1U);
}

TEST_CASE("QmuxHost: a turn accepts its share and leaves the rest to the next", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.server_config.accepts_per_turn = 2U;
    hosts.start();
    for (usize count = 0U; count < 5U; ++count)
    {
        static_cast<void>(hosts.connect());
    }
    static_cast<void>(hosts.server->poll(0U));
    CHECK(hosts.server->counters().accepted == 2U);
    static_cast<void>(hosts.server->poll(0U));
    CHECK(hosts.server->counters().accepted == 4U);
    static_cast<void>(hosts.server->poll(0U));
    CHECK(hosts.server->counters().accepted == 5U);
    static_cast<void>(hosts.server->poll(0U));
    CHECK(hosts.server_poller.accepts == 5U);
}

TEST_CASE("QmuxHost: a turn writes one connection's share and runs again at once", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.client_config.write_bytes_per_turn = kQmuxHostChunkBytes;
    hosts.client_config.connection.streams.send_bytes = 1U << 20U;
    hosts.client_config.connection.streams.connection_window = 1U << 20U;
    hosts.client_config.connection.streams.connection_max_window = 1U << 20U;
    hosts.server_config.connection.streams.receive_bytes = 1U << 20U;
    hosts.server_config.connection.streams.connection_window = 1U << 20U;
    hosts.server_config.connection.streams.connection_max_window = 1U << 20U;
    hosts.server_config.connection.streams.stream_window = 1U << 20U;
    hosts.start();
    const QuicHandle handle = hosts.connect();
    hosts.establish(handle);
    QmuxConnection* const connection = hosts.client->connection(handle);
    u64 id = 0U;
    REQUIRE(connection->streams().open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    static_cast<void>(write_pattern(connection->streams(), id, written, 1U << 20U, 1U));
    REQUIRE(written > 2U * kQmuxHostChunkBytes);
    hosts.client->wake(handle);
    const u64 before = hosts.client->counters().bytes_written;
    const u64 next = hosts.client->poll(0U);
    CHECK(next <= hosts.clock.now);
    CHECK(hosts.client->counters().bytes_written - before <= kQmuxHostChunkBytes);
}

TEST_CASE("QmuxHost: a turn waits no longer than its deadlines allow", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.client_config.connection.handshake_timeout_ns = 40U * kMsNs;
    hosts.network.connects_hang = true;
    hosts.start();
    static_cast<void>(hosts.connect());
    static_cast<void>(hosts.client->poll(1'000U * kMsNs)); // due now: served at once
    CHECK(hosts.client_poller.last_timeout_ns == 0U);
    hosts.clock.now += 10U * kMsNs;
    static_cast<void>(hosts.client->poll(1'000U * kMsNs));
    CHECK(hosts.client_poller.last_timeout_ns == 30U * kMsNs);
    static_cast<void>(hosts.client->poll(5U * kMsNs));
    CHECK(hosts.client_poller.last_timeout_ns == 5U * kMsNs);
}

TEST_CASE("QmuxHost: a configuration out of range or an allocator's refusal makes no host", "[net][qmux-host]")
{
    Hosts hosts;
    const auto makes = [&hosts](const QmuxHostConfig& config)
    {
        const QmuxHost host(hosts.allocator, hosts.server_poller, hosts.clock, &hosts.buckets, hosts.server_events,
                            config);
        return host.ready();
    };
    CHECK(makes(hosts.server_config));
    QmuxHostConfig config = hosts.server_config;
    config.connections = 0U;
    CHECK_FALSE(makes(config));
    config = hosts.server_config;
    config.connections = kQuicMaxHostConnections + 1U;
    CHECK_FALSE(makes(config));
    config = hosts.server_config;
    config.ready_per_turn = 0U;
    CHECK_FALSE(makes(config));
    config = hosts.server_config;
    config.accepts_per_turn = 0U;
    CHECK_FALSE(makes(config));
    config = hosts.server_config;
    config.accept_retry_ns = 0U;
    CHECK_FALSE(makes(config));
    config = hosts.server_config;
    config.max_wait_ns = 0U;
    CHECK_FALSE(makes(config));
    config = hosts.server_config;
    config.write_bytes_per_turn = kQmuxHostChunkBytes - 1U;
    CHECK_FALSE(makes(config));
    config = hosts.server_config;
    config.connection.tls = &hosts.identity.client; // a server's context is needed to accept
    CHECK_FALSE(makes(config));
    {
        const QmuxHost host(hosts.allocator, hosts.server_poller, hosts.clock, nullptr, hosts.server_events,
                            hosts.server_config);
        CHECK_FALSE(host.ready()); // accepting needs the handshake rate
        CHECK(const_cast<QmuxHost&>(host).poll(0U) == kNeverNs);
    }

    bool made = false;
    for (usize refuse = 0U; refuse < 16U && !made; ++refuse)
    {
        CountingAllocator refusing;
        refusing.refuse_after(refuse);
        const QmuxHost host(refusing, hosts.server_poller, hosts.clock, &hosts.buckets, hosts.server_events,
                            hosts.server_config);
        made = host.ready();
    }
    CHECK(made);
}

TEST_CASE("QmuxHost: a connection that cannot be built is reset", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.start();
    hosts.allocator.refuse_after(hosts.allocator.calls());
    PipePoller raw(hosts.network, Endpoint{IpAddress::v4_loopback(), 50001U});
    SocketError error = SocketError::None;
    REQUIRE(raw.connect(0U, hosts.server_address, error) != nullptr);
    hosts.turns(1U);
    CHECK(hosts.server->counters().refused_start == 1U);
    CHECK(hosts.server->live() == 0U);
    CHECK(hosts.server_poller.resets == 1U);
}

TEST_CASE("QmuxHost: an established connection's turns allocate nothing", "[net][qmux-host]")
{
    Hosts hosts;
    hosts.start();
    hosts.client_events.transfer = 256U * 1024U;
    hosts.server_events.transfer = 256U * 1024U;
    const QuicHandle handle = hosts.connect();
    hosts.establish(handle);
    const usize calls = hosts.allocator.calls();
    for (usize turn = 0U; turn < 20000U && !(hosts.client_events.done(handle.slot) && hosts.server_events.done(0U));
         ++turn)
    {
        hosts.turns(1U);
    }
    CHECK(hosts.client_events.done(handle.slot));
    CHECK(hosts.allocator.calls() == calls);
}

// ---- The host's TCP ----

namespace
{
struct TcpHosts
{
    Identity identity;
    CountingAllocator allocator;
    HostClock clock;
    TcpListener listener;
    std::unique_ptr<TcpPoller> server_poller;
    std::unique_ptr<TcpPoller> client_poller;
    HandshakeBuckets buckets{allocator, HandshakeRules{}};
    Recorder server_events;
    Recorder client_events;
    std::unique_ptr<QmuxHost> server;
    std::unique_ptr<QmuxHost> client;

    explicit TcpHosts(const TcpOptions& socket_options = {})
    {
        TcpListenOptions options;
        options.bind = Endpoint{IpAddress::v4_loopback(), 0U};
        options.connection = socket_options;
        SocketError error = SocketError::None;
        listener = TcpListener::open(options, error);
        REQUIRE(error == SocketError::None);
        QmuxHostConfig server_config;
        server_config.connection = connection_config();
        server_config.connection.tls = &identity.server;
        server_config.connections = 16U;
        QmuxHostConfig client_config;
        client_config.connection = connection_config();
        client_config.connections = 16U;
        server_poller = std::make_unique<TcpPoller>(allocator, 17U, &listener);
        client_poller = std::make_unique<TcpPoller>(allocator, 17U, nullptr, socket_options);
        REQUIRE(server_poller->ready());
        REQUIRE(client_poller->ready());
        server = std::make_unique<QmuxHost>(allocator, *server_poller, clock, &buckets, server_events, server_config);
        client = std::make_unique<QmuxHost>(allocator, *client_poller, clock, nullptr, client_events, client_config);
        REQUIRE(server->ready());
        REQUIRE(client->ready());
        client_events.client = true;
    }

    void turns(usize count)
    {
        for (usize turn = 0U; turn < count; ++turn)
        {
            static_cast<void>(server->poll(100'000U));
            static_cast<void>(client->poll(100'000U));
        }
    }
};
} // namespace

TEST_CASE("QmuxHost: clients and a server carry transfers over real TCP, small buffers", "[net][qmux-host][tcp]")
{
    TcpOptions small;
    small.send_buffer_bytes = 16U * 1024U;
    small.receive_buffer_bytes = 16U * 1024U;
    TcpHosts hosts(small);
    hosts.client_events.transfer = 512U * 1024U;
    hosts.server_events.transfer = 512U * 1024U;
    constexpr u32 kConnections = 8U;
    SocketError error = SocketError::None;
    QuicHandle handles[kConnections];
    for (QuicHandle& handle : handles)
    {
        handle = hosts.client->connect(QmuxConnect{hosts.listener.local_endpoint(), &hosts.identity.client},
                                       hosts.clock.monotonic_ns(), error);
        REQUIRE(handle.valid());
    }
    const auto finished = [&hosts]
    {
        for (u32 slot = 0U; slot < kConnections; ++slot)
        {
            if (!hosts.client_events.done(slot) || !hosts.server_events.done(slot))
            {
                return false;
            }
        }
        return true;
    };
    for (usize turn = 0U; turn < 2'000'000U && !finished(); ++turn)
    {
        hosts.turns(1U);
    }
    REQUIRE(finished());
    CHECK(hosts.server->counters().accepted == kConnections);
    for (const QuicHandle& handle : handles)
    {
        hosts.client->connection(handle)->close(0U, ConstSpan<u8>(), hosts.clock.monotonic_ns());
        hosts.client->wake(handle);
    }
    for (usize turn = 0U; turn < 20000U && (hosts.client->live() != 0U || hosts.server->live() != 0U); ++turn)
    {
        hosts.turns(1U);
    }
    CHECK(hosts.client->live() == 0U);
    CHECK(hosts.server->live() == 0U);
    CHECK(hosts.client_events.ended_count == kConnections);
    CHECK(hosts.server_events.ended_count == kConnections);
    for (usize at = 0U; at < kConnections; ++at)
    {
        CHECK(hosts.client_events.ended[at].info.reason == QmuxCloseReason::Local);
        CHECK(hosts.client_events.ended[at].transport == SocketError::None);
        CHECK(hosts.server_events.ended[at].info.reason == QmuxCloseReason::Peer);
    }
}

TEST_CASE("QmuxHost: a port no one listens on is reported refused over real TCP", "[net][qmux-host][tcp]")
{
    TcpHosts hosts;
    Endpoint gone;
    {
        TcpListenOptions options;
        options.bind = Endpoint{IpAddress::v4_loopback(), 0U};
        SocketError listen_error = SocketError::None;
        const TcpListener listener = TcpListener::open(options, listen_error);
        REQUIRE(listen_error == SocketError::None);
        gone = listener.local_endpoint();
    }
    SocketError error = SocketError::None;
    const QuicHandle handle =
        hosts.client->connect(QmuxConnect{gone, &hosts.identity.client}, hosts.clock.monotonic_ns(), error);
    REQUIRE(handle.valid());
    for (usize turn = 0U; turn < 80000U && hosts.client_events.ended_count == 0U; ++turn)
    {
        hosts.turns(1U);
    }
    REQUIRE(hosts.client_events.ended_count == 1U);
    CHECK(hosts.client_events.ended[0].transport == SocketError::Refused);
    CHECK(hosts.client_events.ended[0].info.reason == QmuxCloseReason::Reset);
}

TEST_CASE("QmuxHost: run() turns until it is told to stop", "[net][qmux-host][tcp]")
{
    TcpHosts hosts;
    const std::atomic<bool> stopped{true};
    hosts.server->run(stopped); // returns at once
    CHECK(hosts.server->live() == 0U);
}
