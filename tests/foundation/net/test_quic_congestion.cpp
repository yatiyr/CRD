// crd-net -- a QUIC connection under congestion control and pacing (NET-PATH; docs/design/network-foundation.md#
// congestion): a bulk transfer whose bytes in flight never pass the window as it grows, a live swap of the controller
// half way that keeps the window, the pacer holding a window with room and its release time a timer that frees it,
// a window that holds data back while ACK-only packets still go, transfers over paths that lose, duplicate and
// reorder with reductions made and the bytes intact, and policies out of range refused. And ECN (#ecn): a path that
// carries it validated in the handshake and marked from then on, CE marks reducing the window without a loss at most
// once a round trip, paths that bleach, re-mark or drop marked packets failing validation while the data arrives
// unmarked, validation holding over lossy, duplicating, reordering paths, and a policy that marks none. And Prague
// (#prague): ECT(1) marked and validated, its codepoint following a live swap both ways, ECT(1) re-marked as ECT(0)
// failing validation, and a transfer through a DualQ bottleneck's L4S step that keeps the queue near its 1 ms step
// at a full link, where CUBIC's ECT(0) fills the classic drop-tail queue. And SCReAM v2 (#scream): a transfer through a
// deep drop-tail queue held near its delay target at a full link, where CUBIC fills it, and ECT(1) in L4S mode. And
// GCC (#gcc): with no receive timestamps, each ACK frame's RTT sample less the ACK delay drives its delay trend, and a
// transfer through a deep drop-tail queue reaches the link from 288 kbit/s and backs off, where CUBIC fills it; with
// receive timestamps (#receive-timestamps) the peer's arrival times drive it the same way. Its probes (#gcc-probing)
// fill their clusters with padding where a media source has too little and find the path past the source's rate,
// from ACK frames alone or from receive timestamps. Receive timestamps are reported only when both ends ask for
// them, and hold over paths that lose, duplicate and reorder. And paths (#migration): transfers both ways follow a NAT
// rebinding and an address change over lossy, duplicating, reordering paths. And a client's own migration: its
// refusals, a probe that moves nothing, a move the server follows that restarts the path's state and leaves the old
// path, a move without probing whose old network goes at once over lossy paths, a socket that reaches nothing
// reverting to the path before it, and moves one after another on the IDs each leaves. And the server's preferred
// address (#migration, RFC 9000 section 9.6): followed once validated, the server moving only once it validated the
// client's path there and then dropping its first socket's datagrams; an unreachable one failing; a client moving
// only when told, disable_active_migration closing the handshake address alone; a client that moved first validating
// it from its new socket; transfers both ways over hostile paths; and the configurations, families and forms. And
// scheduled handovers (#handover) over a LEO link: CUBIC, BBR, SCReAM and GCC through them with the schedule, without
// it and with a wrong one; persistent congestion inside and past the guards; reordered covered packets undoing no
// genuine reduction; no path MTU probe while covered; refusals, and the schedule cleared by a move.

#include "ingress_fixture.hpp"
#include "quic_connection_fixture.hpp"
#include "quic_stream_fixture.hpp"

#include <crd/net/quic_connection.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>
#include <ostream>

namespace
{
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;
using namespace crd::net;
using namespace crd::net::testing;

constexpr u8 kClientCid[8] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7};
constexpr u8 kInitialDcid[8] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7};
constexpr u64 kStream = 0U; // the client's first bidirectional stream

QuicConnectionConfig bulk_config(QuicSide side, const Identity& identity, usize seed)
{
    QuicConnectionConfig config = config_for(side, identity, QuicVersion::V1, seed);
    QuicStreamLimits& limits = config.streams;
    limits.peer_bidi = 1U;
    limits.local_bidi = 1U;
    limits.receive_bytes = 1U << 20U;
    limits.send_bytes = 1U << 20U;
    limits.stream_window = 1U << 20U;
    limits.connection_window = 1U << 22U;
    limits.connection_max_window = 1U << 22U;
    config.sent_packets = 1024U;
    config.sent_frames = 4096U;
    return config;
}

// The client writes `total` bytes of the pattern on one stream without ending it; the server reads them.
struct Bulk
{
    QuicConnection& client;
    QuicConnection& server;
    u64 total = 0U;
    u64 written = 0U;
    u64 read = 0U;

    bool step()
    {
        if (written < total)
        {
            static_cast<void>(write_pattern(client.streams(), kStream, written, total, kStream, false));
        }
        QuicStreamEvent event;
        while (server.streams().poll(event))
        {
        }
        while (client.streams().poll(event))
        {
        }
        if (server.streams().recv_state(kStream) != QuicRecvState::None)
        {
            static_cast<void>(drain(server.streams(), kStream, read, kStream));
        }
        return read >= total;
    }
};

// Watches every client datagram: bytes in flight never pass the window once confirmed (no probe runs on these paths).
class WindowWatch final : public Filter
{
public:
    explicit WindowWatch(const QuicConnection& client) : m_client(client) {}

    bool drop(usize from, ConstSpan<u8> /*datagram*/) override
    {
        if (from == 0U && m_client.confirmed() && m_client.recovery().pto_count() == 0U)
        {
            const u64 in_flight = m_client.recovery().bytes_in_flight();
            over = over || in_flight > m_client.congestion().window();
            peak = (in_flight > peak) ? in_flight : peak;
        }
        return false;
    }

    bool over = false;
    u64 peak = 0U;

private:
    const QuicConnection& m_client;
};

struct Pair
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnection client;
    QuicConnection server;

    // The allocations of everything but the stream and CRYPTO buffers, which grow as their data needs them.
    [[nodiscard]] usize others() const
    {
        return allocator.calls() - client.buffer_allocations() - server.buffer_allocations();
    }
    std::unique_ptr<Net> net;

    // `client_times` and `server_times`: the receive timestamps each end asks for (0: none).
    explicit Pair(const CongestionPolicy& policy, usize seed = 0U, bool ecn = true, u64 client_times = 0U,
                  u64 server_times = 0U)
        : client(allocator, with(bulk_config(QuicSide::Client, identity, seed), policy, ecn, client_times)),
          server(allocator, with(bulk_config(QuicSide::Server, identity, seed + 1U), policy, ecn, server_times))
    {
        REQUIRE(client.ready());
        REQUIRE(server.ready());
        REQUIRE(client.connect(ConstSpan<u8>(kClientCid, sizeof(kClientCid)),
                               ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), kStartNs));
        net = std::make_unique<Net>(client, server);
        for (Path& path : net->path)
        {
            path.delay_ns = 25U * kMs;
        }
    }

    static QuicConnectionConfig with(QuicConnectionConfig config, const CongestionPolicy& policy, bool ecn, u64 times)
    {
        config.congestion = policy;
        config.ecn = ecn;
        config.parameters.max_receive_timestamps_per_ack = times;
        return config;
    }

    void confirm()
    {
        REQUIRE(net->run_until([&] { return client.confirmed() && server.confirmed(); }));
        u64 id = 0U;
        REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
        REQUIRE(id == kStream);
    }
};

constexpr u8 kResetKeys[2][kQuicResetKeyBytes] = {{0x71U, 0x01U}, {0x72U, 0x02U}};

// An end's connection IDs, routed by a table of its own: the IDs a move takes come from here.
struct Ids
{
    ConnectionIdTable routes;
    QuicResetKey key;
    QuicRoutedIds ids;

    Ids(CountingAllocator& allocator, u32 index, ConstSpan<u8> sequence0)
        : routes(allocator, 64U), ids(routes, key, index)
    {
        REQUIRE(key.init(ConstSpan<u8>(kResetKeys[index])) == CryptoError::None);
        REQUIRE(routes.insert(sequence0, index) == RouteError::None);
    }
};

constexpr Endpoint kServerAt{IpAddress::v4(192, 0, 2, 1), 443U};
constexpr Endpoint kPreferredAt{IpAddress::v4(192, 0, 2, 80), 4433U};

// What a Hosted server offers as its preferred address (RFC 9000 section 9.6), on its socket 1, and whether its client
// follows it by itself.
enum class Preferred : u8
{
    None,
    Follow,      // an IPv4 address, followed
    Manual,      // an IPv4 address the client moves to only when told
    V6Only,      // an IPv6 address, which an IPv4 client never follows by itself
    Mapped,      // an IPv4 address, followed by a client whose server address is IPv4-mapped
    OpenedThere, // offered, but the connection opened on that socket: none is sent
    Unbound,     // offered to a client whose path names no server address: it has none to move from
};

[[nodiscard]] Endpoint mapped(const Endpoint& at)
{
    return Endpoint{at.address.mapped(), at.port};
}

// A client and a server that issue connection IDs, on paths that follow the client's sockets.
struct Hosted
{
    Identity identity;
    CountingAllocator allocator;
    Ids client_ids{allocator, 0U, ConstSpan<u8>(kClientCid, sizeof(kClientCid))};
    Ids server_ids{allocator, 1U, ConstSpan<u8>(kNetServerCid, sizeof(kNetServerCid))};
    QuicConnection client;
    QuicConnection server;
    std::unique_ptr<Net> net;

    // `pmtu` false: no path MTU search, whose probes a restarted path would send.
    // `id_limit`: each end's active_connection_id_limit, the other end's IDs it holds.
    explicit Hosted(usize seed = 0U, bool disable_migration = false, bool pmtu = true,
                    Preferred preferred = Preferred::None, u64 id_limit = kQuicDefaultActiveIdLimit)
        : client(allocator,
                 with(bulk_config(QuicSide::Client, identity, seed), client_ids, false, pmtu, preferred, id_limit)),
          server(allocator, with(bulk_config(QuicSide::Server, identity, seed + 1U), server_ids, disable_migration,
                                 pmtu, preferred, id_limit))
    {
        REQUIRE(client.ready());
        REQUIRE(server.ready());
        REQUIRE(client.connect(ConstSpan<u8>(kClientCid, sizeof(kClientCid)),
                               ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), kStartNs));
        net = std::make_unique<Net>(client, server);
        net->paths = true;
        net->socket_at[0] = Endpoint{IpAddress::v4(203, 0, 113, 9), 41000U};
        net->socket_at[1] = Endpoint{IpAddress::v4(192, 0, 2, 33), 42000U};
        net->socket_at[2] = Endpoint{IpAddress::v4(198, 51, 100, 7), 43000U};
        if (preferred != Preferred::None && preferred != Preferred::Unbound)
        {
            const bool v4_mapped = preferred == Preferred::Mapped;
            net->server_at[0] = v4_mapped ? mapped(kServerAt) : kServerAt;
            net->server_at[1] = v4_mapped ? mapped(kPreferredAt) : kPreferredAt;
            if (preferred == Preferred::V6Only)
            {
                net->server_at[1] = preferred_v6();
            }
        }
        for (Path& path : net->path)
        {
            path.delay_ns = 25U * kMs;
        }
    }

    static Endpoint preferred_v6()
    {
        constexpr u8 address[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80};
        return Endpoint{IpAddress::v6(address), 4433U};
    }

    static QuicConnectionConfig with(QuicConnectionConfig config, Ids& ids, bool disable_migration, bool pmtu,
                                     Preferred preferred, u64 id_limit)
    {
        config.ids = &ids.ids;
        config.path_mtu_discovery = pmtu;
        config.parameters.active_connection_id_limit = id_limit;
        if (config.side == QuicSide::Client)
        {
            // The server's address.
            config.path = QuicPath{0U, (preferred == Preferred::Mapped) ? mapped(kServerAt) : kServerAt};
            config.path = (preferred == Preferred::Unbound) ? QuicPath{} : config.path;
            config.follow_preferred_address = preferred != Preferred::Manual;
        }
        else if (preferred != Preferred::None)
        {
            QuicPreferredAddress& address = config.parameters.preferred_address;
            config.parameters.has_preferred_address = true;
            config.preferred_socket = 1U;
            config.path.socket = (preferred == Preferred::OpenedThere) ? 1U : 0U;
            if (preferred == Preferred::V6Only)
            {
                std::memcpy(address.ipv6, preferred_v6().address.bytes, 16U);
                address.ipv6_port = preferred_v6().port;
            }
            else
            {
                std::memcpy(address.ipv4, kPreferredAt.address.bytes, 4U);
                address.ipv4_port = kPreferredAt.port;
            }
        }
        config.parameters.disable_active_migration = disable_migration;
        return config;
    }

    // Confirmed, the stream open, and each end's spare connection IDs given.
    void confirm()
    {
        REQUIRE(net->run_until([&] { return client.confirmed() && server.confirmed(); }));
        net->advance(net->now + 200U * kMs);
        u64 id = 0U;
        REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
        REQUIRE(id == kStream);
    }

    // The client's path from its socket `socket` to the server.
    [[nodiscard]] QuicPath socket(u32 socket) const { return QuicPath{socket, client.path().peer}; }
};

} // namespace

TEST_CASE("QUIC congestion: bytes in flight stay inside a growing window, and a live swap keeps it",
          "[net][net-path][quic-connection]")
{
    Pair pair(CongestionPolicy{});
    WindowWatch watch(pair.client);
    pair.net->filter = &watch;
    pair.confirm();
    // The handshake did not fill the window, so it did not grow (RFC 9002 section 7.8).
    CHECK(pair.client.congestion().window() == 12000U);
    CHECK(pair.client.congestion().algorithm() == CongestionAlgorithm::Cubic);

    Bulk bulk{pair.client, pair.server, 1U << 18U};
    REQUIRE(pair.net->run_until([&] { return bulk.read >= bulk.total / 2U || bulk.step(); }));
    const CongestionState half = pair.client.congestion().state();
    CHECK(half.window > 4U * 12000U); // slow start doubled it each round trip
    CongestionPolicy invalid;
    invalid.minimum_packets = 0U;
    CHECK_FALSE(pair.client.set_congestion(invalid));
    CHECK(pair.client.congestion().algorithm() == CongestionAlgorithm::Cubic);
    CongestionPolicy reno;
    reno.algorithm = CongestionAlgorithm::NewReno;
    REQUIRE(pair.client.set_congestion(reno));
    CHECK(pair.client.congestion().algorithm() == CongestionAlgorithm::NewReno);
    CHECK(pair.client.congestion().window() == half.window);
    CHECK(pair.client.congestion().state().slow_start_threshold == half.slow_start_threshold);

    const usize allocations = pair.others();
    REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
    CHECK(pair.others() == allocations); // nothing allocated on the way but the streams' buffers, as they grew
    CHECK_FALSE(watch.over);
    CHECK(watch.peak > 12000U);
    CHECK(pair.client.congestion().state().reductions == 0U);
    CHECK(pair.client.congestion().window() >= half.window);
}

TEST_CASE("QUIC congestion: a coupled NewReno's path joins the connection's coupling and a live swap keeps it exact",
          "[net][net-path][quic-connection][coupled]")
{
    CongestionPolicy coupled;
    coupled.algorithm = CongestionAlgorithm::NewReno;
    coupled.coupled = true;
    Pair pair(coupled);
    pair.confirm();
    CHECK(pair.client.coupling().members() == 1U);
    CHECK(pair.server.coupling().members() == 1U);
    Bulk bulk{pair.client, pair.server, 1U << 18U};
    REQUIRE(pair.net->run_until([&] { return bulk.read >= bulk.total / 2U || bulk.step(); }));
    REQUIRE(pair.client.set_congestion(CongestionPolicy{}));
    CHECK(pair.client.coupling().members() == 0U);
    const u64 window = pair.client.congestion().window();
    REQUIRE(pair.client.set_congestion(coupled));
    CHECK(pair.client.coupling().members() == 1U);
    CHECK(pair.client.congestion().window() == window);
    REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
    CHECK(bulk.read == bulk.total);
}

TEST_CASE("QUIC congestion: the pacer holds a window with room, and its release time is a timer that frees it",
          "[net][net-path][quic-connection]")
{
    Pair pair(CongestionPolicy{});
    pair.confirm();
    Bulk bulk{pair.client, pair.server, 1U << 18U};
    REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
    // After an idle second everything is acknowledged and the burst is whole. More data, and the client sends at one
    // instant until it stops: the pacer, not the window, held it.
    pair.net->advance(pair.net->now + 1000U * kMs);
    REQUIRE(pair.client.recovery().bytes_in_flight() == 0U);
    const u64 window = pair.client.congestion().window();
    const u64 burst = pair.client.congestion().burst(pair.client.recovery().rtt());
    REQUIRE(window > 4U * burst);
    bulk.total += 1U << 17U;
    static_cast<void>(write_pattern(pair.client.streams(), kStream, bulk.written, bulk.total, kStream, false));
    const u64 now = pair.net->now;
    u64 sent = 0U;
    for (usize guard = 0U; guard < 1024U; ++guard)
    {
        u8 out[kDatagram];
        const usize size = pair.client.send(Span<u8>(out, sizeof(out)), now);
        if (size == 0U)
        {
            break;
        }
        sent += size;
    }
    CHECK(sent >= burst);
    CHECK(sent <= burst + kDatagram);
    CHECK(pair.client.recovery().bytes_in_flight() + burst < window);
    CHECK(pair.client.congestion().state().limited);
    const u64 release = pair.client.next_timer_ns();
    CHECK(release > now);
    CHECK(release <= now + pair.client.recovery().rtt().smoothed_ns); // a datagram's share of the RTT at most
    u8 out[kDatagram];
    CHECK(pair.client.send(Span<u8>(out, sizeof(out)), release - 1U) == 0U);
    pair.client.on_timeout(release);
    CHECK(pair.client.send(Span<u8>(out, sizeof(out)), release) != 0U);
    // That one ran the credit into debt: the next waits for it.
    CHECK(pair.client.send(Span<u8>(out, sizeof(out)), release) == 0U);
    CHECK(pair.client.next_timer_ns() > release);
}

TEST_CASE("QUIC congestion: a full window holds data while ACK-only packets still go",
          "[net][net-path][quic-connection]")
{
    CongestionPolicy unpaced;
    unpaced.pacing = false;
    Pair pair(unpaced);
    pair.confirm();
    // Without acknowledgements the client sends its window and stops; no timer serves it.
    u64 written = 0U;
    static_cast<void>(write_pattern(pair.client.streams(), kStream, written, 1U << 16U, kStream, false));
    const u64 now = pair.net->now;
    u64 sent = 0U;
    for (usize guard = 0U; guard < 1024U; ++guard)
    {
        u8 out[kDatagram];
        const usize size = pair.client.send(Span<u8>(out, sizeof(out)), now);
        if (size == 0U)
        {
            break;
        }
        sent += size;
    }
    CHECK(sent != 0U);
    CHECK(sent <= 12000U);
    const u64 in_flight = pair.client.recovery().bytes_in_flight();
    REQUIRE(in_flight <= pair.client.congestion().window());
    CHECK(pair.client.congestion().window() - in_flight < 1200U); // less room than a datagram
    CHECK(pair.client.next_timer_ns() > now + kMs); // the loss timer, not a spin
    CHECK(pair.client.congestion().state().limited);
    // An ACK the client owes still goes: the server's PING arrives and the client answers it with the window full.
    pair.server.ping();
    u8 ping[kDatagram];
    const usize ping_bytes = pair.server.send(Span<u8>(ping, sizeof(ping)), now);
    REQUIRE(ping_bytes != 0U);
    pair.client.receive(Span<u8>(ping, ping_bytes), Ecn::NotEct, now);
    const u64 due = pair.client.next_timer_ns();
    REQUIRE(due < now + 30U * kMs); // the ACK's delay
    pair.client.on_timeout(due);
    u8 out[kDatagram];
    const usize ack = pair.client.send(Span<u8>(out, sizeof(out)), due);
    CHECK(ack != 0U);
    CHECK(ack < 200U); // an ACK, no data
}

TEST_CASE("QUIC congestion: transfers over paths that lose, duplicate and reorder reduce the window and arrive intact",
          "[net][net-path][quic-connection]")
{
    for (const CongestionAlgorithm algorithm :
         {CongestionAlgorithm::Cubic, CongestionAlgorithm::NewReno, CongestionAlgorithm::Bbr})
    {
        for (u64 seed = 1U; seed <= 4U; ++seed)
        {
            CongestionPolicy policy;
            policy.algorithm = algorithm;
            Pair pair(policy, seed);
            pair.net->rng.state = seed;
            for (Path& path : pair.net->path)
            {
                path.loss = 20U;
                path.duplicate = 10U;
                path.jitter_ns = 10U * kMs;
            }
            INFO("algorithm " << static_cast<u32>(algorithm) << " seed " << seed);
            pair.confirm();
            Bulk bulk{pair.client, pair.server, 1U << 18U};
            REQUIRE(pair.net->run_until([&] { return bulk.step(); }, 600000U * kMs));
            const CongestionState state = pair.client.congestion().state();
            // BBR's Startup probes through scattered losses: it leaves only past 2% lost in a round of six or more.
            CHECK((state.reductions > 0U || algorithm == CongestionAlgorithm::Bbr));
            CHECK(state.window >= ((algorithm == CongestionAlgorithm::Bbr) ? 4U : 2U) * 1200U);
            CHECK(pair.client.state() == QuicConnectionState::Established);
        }
    }
}

TEST_CASE("QUIC paths: transfers both ways follow a NAT rebinding and then an address change over paths that lose, "
          "duplicate and reorder, and arrive intact",
          "[net][net-path][quic-connection]")
{
    for (u64 seed = 1U; seed <= 3U; ++seed)
    {
        INFO("seed " << seed);
        Pair pair(CongestionPolicy{}, seed);
        Net& net = *pair.net;
        net.rng.state = seed;
        net.paths = true;
        for (Path& path : net.path)
        {
            path.loss = 20U;
            path.duplicate = 10U;
            path.jitter_ns = 10U * kMs;
        }
        pair.confirm();
        REQUIRE(pair.server.path().peer == net.client_at);
        // The client streams to the server and the server back on the same stream.
        constexpr u64 total = 1U << 19U;
        Bulk up{pair.client, pair.server, total};
        u64 down_written = 0U;
        u64 down_read = 0U;
        const auto step = [&]
        {
            const bool up_done = up.step();
            if (pair.server.streams().recv_state(kStream) != QuicRecvState::None && down_written < total)
            {
                static_cast<void>(write_pattern(pair.server.streams(), kStream, down_written, total, 7U, false));
            }
            if (pair.client.streams().recv_state(kStream) != QuicRecvState::None)
            {
                static_cast<void>(drain(pair.client.streams(), kStream, down_read, 7U));
            }
            return up_done && down_read >= total;
        };
        REQUIRE(net.run_until([&] { return step() || (up.read >= total / 3U && down_read >= total / 3U); },
                              600000U * kMs));
        // A NAT rebinding: the client's port changes and the old mapping is gone at once.
        net.client_at.port = 50001U;
        REQUIRE(net.run_until([&] { return step() || (up.read >= 2U * total / 3U && down_read >= 2U * total / 3U); },
                              600000U * kMs));
        CHECK(pair.server.path().peer == net.client_at);
        CHECK(pair.server.recovery().path_floor() == 0U); // only the port changed
        CHECK(pair.server.address_validated());
        const u64 rebound = pair.server.counters().migrations;
        CHECK(rebound >= 1U);
        // A move to another address: the server's congestion control and RTT estimate start again.
        net.client_at = Endpoint{IpAddress::v4(203, 0, 113, 5), 40000U};
        REQUIRE(net.run_until([&] { return step(); }, 600000U * kMs));
        CHECK(pair.server.path().peer == net.client_at);
        CHECK(pair.server.recovery().path_floor() != 0U);
        CHECK(pair.server.counters().migrations > rebound);
        CHECK(pair.server.counters().path_validations >= 2U);
        CHECK(pair.client.state() == QuicConnectionState::Established);
        CHECK(pair.server.state() == QuicConnectionState::Established);
        CHECK(net.misdirected != 0U); // the server's datagrams that still went the old way were lost, and recovered
    }
}

TEST_CASE("QUIC BBR: a transfer over a bottleneck finds its rate and round trip, queues little, stays in its window",
          "[net][net-path][quic-connection][bbr]")
{
    static constexpr u64 kRate = 2000000U;             // 16 Mbit/s
    static constexpr usize kBdp = kRate / 1000U * 50U; // the Pair's 50 ms round trip: 100 KB
    CongestionPolicy bbr;
    bbr.algorithm = CongestionAlgorithm::Bbr;
    Pair pair(bbr);
    pair.net->path[0].rate = kRate;
    pair.net->path[0].buffer = kBdp;
    WindowWatch watch(pair.client);
    pair.net->filter = &watch;
    pair.confirm();
    CHECK(pair.client.congestion().algorithm() == CongestionAlgorithm::Bbr);
    const u64 start = pair.net->now;
    Bulk bulk{pair.client, pair.server, 6U << 20U};
    REQUIRE(pair.net->run_until([&] { return bulk.read >= bulk.total / 2U || bulk.step(); }));
    const CongestionState half = pair.client.congestion().state();
    CHECK(half.bandwidth >= kRate * 90U / 100U);
    CHECK(half.bandwidth <= kRate * 105U / 100U);
    CHECK(half.min_rtt_ns >= 50U * kMs);
    CHECK(half.min_rtt_ns <= 52U * kMs);
    CHECK(half.phase != CongestionPhase::SlowStart);
    // A live swap to CUBIC and back keeps the window and the delivery the sampler holds.
    REQUIRE(pair.client.set_congestion(CongestionPolicy{}));
    CHECK(pair.client.congestion().window() == half.window);
    REQUIRE(pair.client.set_congestion(bbr));
    CHECK(pair.client.congestion().window() == half.window);
    CHECK(pair.client.delivery().delivered() > bulk.total / 2U);

    const usize allocations = pair.others();
    REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
    CHECK(pair.others() == allocations); // nothing allocated on the way but the streams' buffers, as they grew
    CHECK_FALSE(watch.over);
    const u64 elapsed = pair.net->now - start;
    CHECK(bulk.total * 1000U * kMs / elapsed >= kRate * 85U / 100U); // the whole transfer, Startup included
    CHECK(pair.net->overflowed[0] * 50U <= pair.net->sent[0]);       // under 2% of datagrams overflowed the queue
    CHECK(pair.client.congestion().state().bandwidth >= kRate * 85U / 100U);
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC congestion: a policy out of range is refused at construction", "[net][net-path][quic-connection]")
{
    Identity identity;
    CountingAllocator allocator;
    QuicConnectionConfig config = bulk_config(QuicSide::Client, identity, 0U);
    config.congestion.pacing_gain = kCongestionMaxPacingGain + 1U;
    const QuicConnection refused(allocator, config);
    CHECK_FALSE(refused.ready());
    config.congestion.pacing_gain = kCongestionMaxPacingGain;
    const QuicConnection accepted(allocator, config);
    CHECK(accepted.ready());
}

// ---- ECN (RFC 9000 section 13.4, RFC 9002 section 7.1) ----

namespace
{
constexpr u64 kRoundTripNs = 50U * kMs; // the Pair's paths: 25 ms each way

// Datagrams each end had sent, and of those marked, at one moment.
struct Marks
{
    usize sent[2] = {};
    usize marked[2] = {};
};

Marks marks_of(const Net& net)
{
    return Marks{{net.sent[0], net.sent[1]}, {net.marked[0], net.marked[1]}};
}
} // namespace

TEST_CASE("QUIC ECN: a path that carries ECN is validated in the handshake and every datagram is marked from then on",
          "[net][net-path][quic-connection][quic-ecn]")
{
    Pair pair(CongestionPolicy{});
    pair.confirm();
    for (const QuicConnection* const end : {&pair.client, &pair.server})
    {
        CHECK(end->ecn().state() == QuicEcnState::Capable);
    }
    const Marks marks = marks_of(*pair.net);
    Bulk bulk{pair.client, pair.server, 1U << 18U};
    REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
    for (usize end = 0U; end < 2U; ++end)
    {
        const usize sent = pair.net->sent[end] - marks.sent[end];
        CHECK(sent > 0U);
        CHECK(pair.net->marked[end] - marks.marked[end] == sent);
        CHECK(pair.net->end(end).ecn().state() == QuicEcnState::Capable);
        CHECK(pair.net->end(end).counters().ecn_signals == 0U);
    }
    // The server counted every marked packet that arrived; the client holds those counts as reported, all ECT(0).
    const EcnCounts& counted = pair.server.recovery().received(QuicSpace::Application).ecn();
    const EcnCounts& reported = pair.client.ecn().reported(QuicSpace::Application);
    CHECK(counted.ect0 > 100U);
    CHECK(counted.ect1 == 0U);
    CHECK(counted.ce == 0U);
    CHECK(reported.ect0 > 0U);
    CHECK(reported.ect0 <= counted.ect0);
    CHECK(pair.client.congestion().state().reductions == 0U);
}

TEST_CASE("QUIC ECN: CE marks reduce the window without a loss, are never undone, and at most once a round trip",
          "[net][net-path][quic-connection][quic-ecn]")
{
    for (const CongestionAlgorithm algorithm : {CongestionAlgorithm::Cubic, CongestionAlgorithm::NewReno})
    {
        // 3% of the client's datagrams marked CE, then every one of them: a congested queue, and a path (or a peer
        // inflating its CE count) that signals all the time.
        for (const u32 ce : {30U, 1000U})
        {
            CongestionPolicy policy;
            policy.algorithm = algorithm;
            Pair pair(policy, 7U);
            pair.net->path[0].ce = ce;
            INFO("algorithm " << static_cast<u32>(algorithm) << " CE permille " << ce);
            pair.confirm();
            const u64 start = pair.net->now;
            Bulk bulk{pair.client, pair.server, 1U << 18U};
            REQUIRE(pair.net->run_until([&] { return bulk.step(); }, 600000U * kMs));
            const CongestionState state = pair.client.congestion().state();
            CHECK(pair.client.ecn().state() == QuicEcnState::Capable);
            CHECK(pair.net->congested[0] > 0U);
            CHECK(pair.net->dropped[0] == 0U);
            CHECK(pair.client.ecn().reported(QuicSpace::Application).ce > 0U);
            CHECK(pair.client.counters().ecn_signals > 0U);
            CHECK(state.reductions > 0U);
            CHECK(state.reductions <= pair.client.counters().ecn_signals);
            CHECK(state.undone == 0U);
            CHECK(state.reductions <= (pair.net->now - start) / kRoundTripNs + 1U);
            CHECK(state.window >= 2U * 1200U);
            CHECK(pair.client.state() == QuicConnectionState::Established);
        }
    }
}

TEST_CASE("QUIC ECN: paths that bleach, re-mark or drop marked packets fail validation and the data arrives unmarked",
          "[net][net-path][quic-connection][quic-ecn]")
{
    struct Case
    {
        EcnPath path;
        QuicEcnFailure failure;
    };
    for (const Case& test :
         {Case{EcnPath::Bleach, QuicEcnFailure::Missing}, Case{EcnPath::Remark, QuicEcnFailure::Exceeded},
          Case{EcnPath::DropMarked, QuicEcnFailure::Lost}})
    {
        Pair pair(CongestionPolicy{});
        for (Path& path : pair.net->path)
        {
            path.ecn = test.path;
        }
        INFO("path " << static_cast<u32>(test.path));
        pair.confirm();
        const Marks marks = marks_of(*pair.net);
        Bulk bulk{pair.client, pair.server, 1U << 17U};
        REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
        for (usize end = 0U; end < 2U; ++end)
        {
            const QuicEcnValidator& ecn = pair.net->end(end).ecn();
            CHECK(ecn.state() == QuicEcnState::Failed);
            CHECK(ecn.failure() == test.failure);
            CHECK(marks.marked[end] <= kQuicEcnTestingPackets); // no more than the testing period's packets
            CHECK(pair.net->marked[end] == marks.marked[end]);
            CHECK(pair.net->end(end).counters().ecn_signals == 0U);
        }
        CHECK(pair.client.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC ECN: validation holds over paths that lose, duplicate and reorder, CE included",
          "[net][net-path][quic-connection][quic-ecn]")
{
    for (u64 seed = 1U; seed <= 4U; ++seed)
    {
        Pair pair(CongestionPolicy{}, seed);
        pair.net->rng.state = seed;
        for (Path& path : pair.net->path)
        {
            path.loss = 20U;
            path.duplicate = 10U;
            path.jitter_ns = 10U * kMs;
            path.ce = 30U;
        }
        INFO("seed " << seed);
        pair.confirm();
        Bulk bulk{pair.client, pair.server, 1U << 18U};
        REQUIRE(pair.net->run_until([&] { return bulk.step(); }, 600000U * kMs));
        for (usize end = 0U; end < 2U; ++end)
        {
            CHECK(pair.net->end(end).ecn().state() == QuicEcnState::Capable);
            CHECK(pair.net->end(end).ecn().failure() == QuicEcnFailure::None);
        }
        REQUIRE(pair.net->congested[0] > 0U);
        CHECK(pair.client.counters().ecn_signals > 0U);
        CHECK(pair.client.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC ECN: Prague marks and validates ECT(1), its codepoint follows a live swap, and no ECN marks nothing",
          "[net][net-path][quic-connection][quic-ecn][prague]")
{
    CongestionPolicy prague;
    prague.algorithm = CongestionAlgorithm::Prague;
    {
        Pair pair(prague);
        pair.confirm();
        Bulk bulk{pair.client, pair.server, 1U << 16U};
        REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
        CHECK(pair.client.ecn().state() == QuicEcnState::Capable);
        CHECK(pair.client.marking() == Ecn::Ect1);
        const EcnCounts& counted = pair.server.recovery().received(QuicSpace::Application).ecn();
        CHECK(counted.ect1 > 0U);
        CHECK(counted.ect0 == 0U);
        // A live swap to CUBIC marks ECT(0) from then on, and back to Prague ECT(1), the path staying validated.
        REQUIRE(pair.client.set_congestion(CongestionPolicy{}));
        Bulk more{pair.client, pair.server, 1U << 16U};
        more.written = bulk.total;
        more.read = bulk.total;
        more.total = 2U * bulk.total;
        REQUIRE(pair.net->run_until([&] { return more.step(); }));
        CHECK(pair.client.marking() == Ecn::Ect0);
        CHECK(counted.ect0 > 0U);
        const u64 ect1 = counted.ect1;
        REQUIRE(pair.client.set_congestion(prague));
        more.total = 3U * bulk.total;
        REQUIRE(pair.net->run_until([&] { return more.step(); }));
        CHECK(pair.client.marking() == Ecn::Ect1);
        CHECK(counted.ect1 > ect1);
        CHECK(pair.client.ecn().state() == QuicEcnState::Capable);
        CHECK(pair.client.counters().ecn_signals == 0U);
    }
    {
        // ECT(1) re-marked as ECT(0) on the way: counts above what was sent with that codepoint.
        Pair pair(prague);
        for (Path& path : pair.net->path)
        {
            path.ecn = EcnPath::Remark;
        }
        pair.confirm();
        Bulk bulk{pair.client, pair.server, 1U << 16U};
        REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
        CHECK(pair.client.ecn().state() == QuicEcnState::Failed);
        CHECK(pair.client.ecn().failure() == QuicEcnFailure::Exceeded);
        CHECK(pair.client.state() == QuicConnectionState::Established);
    }
    {
        Pair pair(prague, 0U, false);
        pair.confirm();
        Bulk bulk{pair.client, pair.server, 1U << 16U};
        REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
        for (usize end = 0U; end < 2U; ++end)
        {
            CHECK(pair.net->end(end).ecn().state() == QuicEcnState::Off);
            CHECK(pair.net->marked[end] == 0U);
        }
    }
}

TEST_CASE("QUIC Prague: through an L4S step the queue stays near the step at a full link, CUBIC's fills the buffer",
          "[net][net-path][quic-connection][quic-ecn][prague]")
{
    static constexpr u64 kRate = 2000000U;             // 16 Mbit/s
    static constexpr usize kBdp = kRate / 1000U * 50U; // the Pair's 50 ms round trip: 100 KB
    static constexpr usize kStep = kRate / 1000U;      // 1 ms of queue (RFC 9332 section 2.4's step)
    struct Run
    {
        u64 goodput = 0U;    // bytes per second over the second half
        u64 max_queued = 0U; // over the second half
        u64 marks = 0U;
        u64 overflowed = 0U;
        CongestionState state;
    };
    const auto run = [](CongestionAlgorithm algorithm)
    {
        CongestionPolicy policy;
        policy.algorithm = algorithm;
        Pair pair(policy, 3U);
        pair.net->path[0].rate = kRate;
        pair.net->path[0].buffer = 2U * kBdp;
        pair.net->path[0].ce_queue = kStep;
        pair.confirm();
        Bulk bulk{pair.client, pair.server, 8U << 20U};
        REQUIRE(pair.net->run_until([&] { return bulk.read >= bulk.total / 2U || bulk.step(); }));
        const u64 half_ns = pair.net->now;
        const u64 half_read = bulk.read;
        pair.net->max_queued[0] = 0U;
        REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
        Run out;
        out.goodput = (bulk.read - half_read) * 1000U * kMs / (pair.net->now - half_ns);
        out.max_queued = pair.net->max_queued[0];
        out.marks = pair.net->congested[0];
        out.overflowed = pair.net->overflowed[0];
        out.state = pair.client.congestion().state();
        CHECK(pair.client.ecn().state() == QuicEcnState::Capable);
        CHECK(pair.client.state() == QuicConnectionState::Established);
        return out;
    };
    const Run prague = run(CongestionAlgorithm::Prague);
    const Run cubic = run(CongestionAlgorithm::Cubic);
    INFO("Prague goodput " << prague.goodput << " max queue " << prague.max_queued << " alpha " << prague.state.alpha
                           << "; CUBIC goodput " << cubic.goodput << " max queue " << cubic.max_queued << " window "
                           << cubic.state.window << " reductions " << cubic.state.reductions << " marks " << cubic.marks
                           << " overflowed " << cubic.overflowed);
    CHECK(prague.marks > 0U);
    CHECK(prague.overflowed == 0U);
    CHECK(prague.goodput >= kRate * 93U / 100U);
    CHECK(prague.max_queued <= 2U * kStep);               // a datagram past the step, never a classic sawtooth
    CHECK(prague.state.alpha < kCongestionAlphaOne / 4U); // a small, steady fraction of marks
    CHECK(prague.state.classic == 0U);
    CHECK(cubic.marks == 0U); // ECT(0) is classic traffic: the drop-tail queue, unmarked
    CHECK(cubic.goodput >= kRate * 90U / 100U);
    CHECK(cubic.max_queued >= 20U * prague.max_queued);
}

TEST_CASE("QUIC SCReAM: a deep drop-tail queue held near the delay target at a full link, where CUBIC fills it",
          "[net][net-path][quic-connection][scream]")
{
    // 16 Mbit/s and the Pair's 50 ms round trip under a 150 ms drop-tail buffer, without ECN marks: the queue delay
    // SCReAM reads is the RTT sample less its base, with the peer's ACK delay taken off (QuicRtt::adjusted_ns).
    static constexpr u64 kRate = 2000000U;
    static constexpr usize kBuffer = kRate / 1000U * 150U;
    static constexpr u64 kTargetQueue = kRate * kScreamQueueDelayTargetNs / (1000U * kMs);
    struct Run
    {
        u64 goodput = 0U;    // bytes per second over the second half
        u64 max_queued = 0U; // over the second half
        u64 overflowed = 0U;
        CongestionState state;
    };
    const auto run = [](const CongestionPolicy& policy)
    {
        Pair pair(policy, 5U);
        pair.net->path[0].rate = kRate;
        pair.net->path[0].buffer = kBuffer;
        pair.confirm();
        Bulk bulk{pair.client, pair.server, 8U << 20U};
        REQUIRE(pair.net->run_until([&] { return bulk.read >= bulk.total / 2U || bulk.step(); }, 600000U * kMs));
        const u64 half_ns = pair.net->now;
        const u64 half_read = bulk.read;
        pair.net->max_queued[0] = 0U;
        const usize overflowed = pair.net->overflowed[0];
        REQUIRE(pair.net->run_until([&] { return bulk.step(); }, 600000U * kMs));
        Run out;
        out.goodput = (bulk.read - half_read) * 1000U * kMs / (pair.net->now - half_ns);
        out.max_queued = pair.net->max_queued[0];
        out.overflowed = pair.net->overflowed[0] - overflowed;
        out.state = pair.client.congestion().state();
        CHECK(pair.client.state() == QuicConnectionState::Established);
        return out;
    };
    CongestionPolicy scream;
    scream.algorithm = CongestionAlgorithm::Scream;
    const Run delay = run(scream);
    const Run cubic = run(CongestionPolicy{});
    INFO("SCReAM goodput " << delay.goodput << " max queue " << delay.max_queued << " queue delay "
                           << delay.state.queue_delay_ns << " target " << delay.state.target_rate << " reductions "
                           << delay.state.reductions << "; CUBIC goodput " << cubic.goodput << " max queue "
                           << cubic.max_queued << " overflowed " << cubic.overflowed);
    CHECK(delay.overflowed == 0U);
    CHECK(delay.goodput >= kRate * 90U / 100U);
    CHECK(delay.max_queued <= kTargetQueue * 5U / 4U);
    CHECK(delay.state.queue_delay_ns < kScreamQueueDelayTargetNs);
    CHECK(cubic.max_queued >= 2U * delay.max_queued);

    // In L4S mode SCReAM marks ECT(1), validated like Prague's.
    scream.scalable_ecn = true;
    Pair pair(scream);
    pair.confirm();
    Bulk bulk{pair.client, pair.server, 1U << 16U};
    REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
    CHECK(pair.client.ecn().state() == QuicEcnState::Capable);
    CHECK(pair.client.marking() == Ecn::Ect1);
    CHECK(pair.server.recovery().received(QuicSpace::Application).ecn().ect1 > 0U);
}

TEST_CASE("QUIC GCC: from ACK frames alone or from receive timestamps a transfer reaches the link and keeps a deep "
          "queue short of CUBIC's",
          "[net][net-path][quic-connection][gcc][receive-timestamps]")
{
    // 2 Mbit/s and the Pair's 50 ms round trip under a 300 ms drop-tail buffer. Without receive timestamps the trend
    // reads each ACK frame's largest packet: its RTT less the peer's ACK delay; with them, every packet's arrival at
    // the peer. From 288 kbit/s at 8% a second GCC needs about 25 s to reach the link; the transfer lasts past a
    // minute.
    static constexpr u64 kRate = 250000U;
    static constexpr usize kBuffer = kRate / 1000U * 300U;
    struct Run
    {
        u64 goodput = 0U;    // bytes per second over the second half
        u64 max_queued = 0U; // over the second half
        u64 overflowed = 0U;
        u64 timestamps = 0U; // packets the server's reports timed
        CongestionState state;
    };
    const auto run = [](const CongestionPolicy& policy, u64 times)
    {
        Pair pair(policy, 5U, true, times, times);
        pair.net->path[0].rate = kRate;
        pair.net->path[0].buffer = kBuffer;
        pair.confirm();
        Bulk bulk{pair.client, pair.server, 16U << 20U};
        REQUIRE(pair.net->run_until([&] { return bulk.read >= bulk.total / 2U || bulk.step(); }, 900000U * kMs));
        const u64 half_ns = pair.net->now;
        const u64 half_read = bulk.read;
        pair.net->max_queued[0] = 0U;
        const usize overflowed = pair.net->overflowed[0];
        REQUIRE(pair.net->run_until([&] { return bulk.step(); }, 900000U * kMs));
        Run out;
        out.goodput = (bulk.read - half_read) * 1000U * kMs / (pair.net->now - half_ns);
        out.max_queued = pair.net->max_queued[0];
        out.overflowed = pair.net->overflowed[0] - overflowed;
        out.timestamps = pair.client.counters().receive_timestamps;
        out.state = pair.client.congestion().state();
        CHECK(pair.client.state() == QuicConnectionState::Established);
        return out;
    };
    CongestionPolicy gcc;
    gcc.algorithm = CongestionAlgorithm::Gcc;
    const Run cubic = run(CongestionPolicy{}, 0U);
    for (const u64 times : {u64{0}, u64{kQuicMaxReceiveTimestamps}})
    {
        const Run delay = run(gcc, times);
        INFO("timestamps " << times << ": GCC goodput " << delay.goodput << " max queue " << delay.max_queued
                           << " target " << delay.state.target_rate << " estimate " << delay.state.bandwidth
                           << " reductions " << delay.state.reductions << " overflowed " << delay.overflowed
                           << " timed " << delay.timestamps << "; CUBIC goodput " << cubic.goodput << " max queue "
                           << cubic.max_queued << " overflowed " << cubic.overflowed);
        CHECK(delay.goodput >= kRate * 85U / 100U);
        CHECK(delay.state.reductions >= 1U);
        CHECK(delay.overflowed == 0U);
        CHECK(4U * delay.max_queued <= cubic.max_queued);
        // With them nearly every data packet is timed: 16 MiB is about 14000 packets.
        CHECK(((times == 0U) ? delay.timestamps == 0U : delay.timestamps >= 10000U));
    }
}

TEST_CASE("QUIC GCC: probes padded where a media source has too little find the path past its rate, from ACK frames "
          "alone or from receive timestamps",
          "[net][net-path][quic-connection][gcc][gcc-probe][receive-timestamps]")
{
    // 4 Mbit/s and the Pair's 50 ms round trip under a 300 ms buffer; the client writes 1250 bytes every 20 ms, 500
    // kbit/s. Without probes the estimate cannot pass 1.5 times what the source sends. The start's probes fill their
    // clusters with padding, and their acknowledgements (each ACK frame's arrival less its ACK delay, or the peer's
    // receive times) measure the path: 864 kbit/s, 1.7 and 3.5 Mbit/s, then the 5 Mbit/s ceiling, which the link
    // holds to 4.
    static constexpr u64 kRate = 500000U;
    static constexpr usize kBuffer = kRate / 1000U * 300U;
    static constexpr u64 kFrame = 1250U;
    static constexpr u64 kSource = kFrame * 50U;
    struct Run
    {
        u64 fills = 0U;
        u64 overflowed = 0U;
        u64 lag = 0U; // bytes written and not yet read at the end
        CongestionState state;
    };
    const auto run = [](bool probe, u64 times)
    {
        CongestionPolicy gcc;
        gcc.algorithm = CongestionAlgorithm::Gcc;
        gcc.probe = probe;
        Pair pair(gcc, 5U, true, times, times);
        pair.net->path[0].rate = kRate;
        pair.net->path[0].buffer = kBuffer;
        pair.confirm();
        Bulk bulk{pair.client, pair.server, 0U};
        const u64 end = pair.net->now + 10000U * kMs;
        while (pair.net->now < end)
        {
            bulk.total += kFrame;
            static_cast<void>(bulk.step());
            pair.net->advance(pair.net->now + 20U * kMs);
        }
        static_cast<void>(bulk.step());
        Run out;
        out.fills = pair.client.counters().probe_fills;
        out.overflowed = pair.net->overflowed[0];
        out.lag = bulk.total - bulk.read;
        out.state = pair.client.congestion().state();
        CHECK(pair.client.state() == QuicConnectionState::Established);
        return out;
    };
    for (const u64 times : {u64{0}, u64{kQuicMaxReceiveTimestamps}})
    {
        const Run probed = run(true, times);
        const Run plain = run(false, times);
        INFO("timestamps " << times << ": with probes " << probed.fills << " padded packets, estimate "
                           << probed.state.bandwidth << ", target " << probed.state.target_rate << ", overflowed "
                           << probed.overflowed << ", lag " << probed.lag << "; without, estimate "
                           << plain.state.bandwidth << ", lag " << plain.lag);
        CHECK(probed.fills > 0U);
        CHECK(plain.fills == 0U);
        CHECK(probed.state.bandwidth >= 3U * kSource);
        CHECK(probed.state.bandwidth <= kRate);
        CHECK(plain.state.bandwidth < 2U * kSource);
        CHECK(probed.overflowed == 0U);
        CHECK(probed.lag <= 4U * kFrame); // the source's frames arrive within a few frame times
        CHECK(plain.lag >= 40U * kFrame); // a rate that starts under the source's falls behind it for seconds
    }
}

TEST_CASE("QUIC GCC: probes pad and find the path inside the window while the prober also receives data",
          "[net][net-path][quic-connection][gcc][gcc-probe]")
{
    // The client's 500 kbit/s source probes the path while the server sends it 1 Mbit/s back, so the client owes ACK
    // frames all along: its probes still pad and find the path, the bytes in flight stay inside the window (a probe's
    // room included), and the data back is read as it comes.
    static constexpr u64 kRate = 500000U;
    static constexpr u64 kFrame = 1250U;
    static constexpr u64 kBack = 2500U;
    CongestionPolicy gcc;
    gcc.algorithm = CongestionAlgorithm::Gcc;
    Pair pair(gcc, 5U);
    pair.net->path[0].rate = kRate;
    pair.net->path[0].buffer = kRate / 1000U * 300U;
    WindowWatch watch(pair.client);
    pair.net->filter = &watch;
    pair.confirm();
    Bulk bulk{pair.client, pair.server, 0U};
    u64 back_total = 0U;
    u64 back_sent = 0U;
    u64 back_read = 0U;
    const u64 end = pair.net->now + 5000U * kMs;
    while (pair.net->now < end)
    {
        bulk.total += kFrame;
        static_cast<void>(bulk.step());
        back_total += kBack;
        if (pair.server.streams().recv_state(kStream) != QuicRecvState::None)
        {
            static_cast<void>(
                write_pattern(pair.server.streams(), kStream, back_sent, back_total, kStream + 1U, false));
        }
        static_cast<void>(drain(pair.client.streams(), kStream, back_read, kStream + 1U));
        pair.net->advance(pair.net->now + 20U * kMs);
    }
    INFO("padded " << pair.client.counters().probe_fills << ", peak in flight " << watch.peak << ", read back "
                   << back_read << " of " << back_total << ", estimate " << pair.client.congestion().state().bandwidth);
    CHECK(pair.client.state() == QuicConnectionState::Established);
    CHECK(pair.client.counters().probe_fills > 0U);
    CHECK_FALSE(watch.over);
    CHECK(back_read + 20U * kBack >= back_total);
    CHECK(pair.client.congestion().state().bandwidth >= 3U * kFrame * 50U);
}

TEST_CASE("QUIC receive timestamps: reported only when both ends ask, and held over lossy, duplicating, reordering "
          "paths",
          "[net][net-path][quic-connection][receive-timestamps]")
{
    for (const u64 client_times : {u64{0}, u64{8}})
    {
        for (const u64 server_times : {u64{0}, u64{8}})
        {
            CongestionPolicy gcc;
            gcc.algorithm = CongestionAlgorithm::Gcc;
            Pair pair(gcc, 7U, true, client_times, server_times);
            pair.confirm();
            CHECK(pair.client.peer_parameters().max_receive_timestamps_per_ack == server_times);
            CHECK(pair.server.peer_parameters().max_receive_timestamps_per_ack == client_times);
            Bulk bulk{pair.client, pair.server, 1U << 18U};
            REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
            INFO("client " << client_times << " server " << server_times);
            CHECK(pair.client.state() == QuicConnectionState::Established);
            CHECK(pair.server.state() == QuicConnectionState::Established);
            CHECK((pair.client.counters().receive_timestamps != 0U) == (client_times != 0U && server_times != 0U));
        }
    }
    for (usize seed = 1U; seed <= 4U; ++seed)
    {
        CongestionPolicy gcc;
        gcc.algorithm = CongestionAlgorithm::Gcc;
        Pair pair(gcc, seed, true, kQuicMaxReceiveTimestamps, kQuicMaxReceiveTimestamps);
        pair.net->rng.state = seed;
        for (Path& path : pair.net->path)
        {
            path.loss = 100U;
            path.duplicate = 50U;
            path.jitter_ns = 15U * kMs;
        }
        pair.confirm();
        Bulk bulk{pair.client, pair.server, 1U << 19U};
        REQUIRE(pair.net->run_until([&] { return bulk.step(); }, 600000U * kMs));
        INFO("seed " << seed);
        CHECK(pair.client.state() == QuicConnectionState::Established);
        CHECK(pair.client.counters().receive_timestamps > 100U);
    }
}

TEST_CASE("QUIC client migration: a server, an unconfirmed handshake, the active path, another server, no spare ID and "
          "disable_active_migration are refused, changing nothing",
          "[net][net-path][quic-connection][client-migration]")
{
    Hosted pair;
    CHECK(pair.client.migrate(pair.socket(1U), pair.net->now) == QuicMigration::NotConfirmed);
    CHECK(pair.client.probe_path(pair.socket(1U), pair.net->now) == QuicMigration::NotConfirmed);
    pair.confirm();
    CHECK(pair.server.migrate(QuicPath{1U, pair.net->client_at}, pair.net->now) == QuicMigration::NotClient);
    CHECK(pair.client.migrate(pair.client.path(), pair.net->now) == QuicMigration::ActivePath);
    const QuicPath other{1U, Endpoint{IpAddress::v4(192, 0, 2, 99), 443U}};
    CHECK(pair.client.probe_path(other, pair.net->now) == QuicMigration::OtherServer);
    CHECK(pair.client.paths().find(other) == kQuicNoPath);
    Pair unbound(CongestionPolicy{}); // a client whose path names no server address has none to keep
    unbound.confirm();
    CHECK(unbound.client.migrate(QuicPath{1U, unbound.client.path().peer}, unbound.net->now) ==
          QuicMigration::OtherServer);
    // The client's active_connection_id_limit of 2 holds one spare ID of the server's: one new socket takes it.
    REQUIRE(pair.client.probe_path(pair.socket(1U), pair.net->now) == QuicMigration::Started);
    CHECK(pair.client.probe_path(pair.socket(1U), pair.net->now) == QuicMigration::Started); // known: nothing new
    CHECK(pair.client.probe_path(pair.socket(2U), pair.net->now) == QuicMigration::NoConnectionId);
    CHECK(pair.client.paths().find(pair.socket(2U)) == kQuicNoPath);
    CHECK(pair.client.path().socket == 0U);
    CHECK(pair.client.counters().migrations == 0U);

    Hosted disabled(0U, true);
    disabled.confirm();
    CHECK(disabled.client.probe_path(disabled.socket(1U), disabled.net->now) == QuicMigration::Disabled);
    CHECK(disabled.client.migrate(disabled.socket(1U), disabled.net->now) == QuicMigration::Disabled);
    CHECK(disabled.client.paths().find(disabled.socket(1U)) == kQuicNoPath);
}

TEST_CASE("QUIC client migration: a probe validates a new socket and moves nothing; the move restarts the path's "
          "state, the server follows and the old path is left",
          "[net][net-path][quic-connection][client-migration]")
{
    Hosted pair;
    Net& net = *pair.net;
    pair.confirm();
    constexpr u64 total = 1U << 19U;
    Bulk up{pair.client, pair.server, total};
    REQUIRE(net.run_until([&] { return up.step() || up.read >= total / 4U; }, 600000U * kMs));
    const usize sent_before = net.sent[0];
    REQUIRE(pair.client.probe_path(pair.socket(1U), net.now) == QuicMigration::Started);
    const usize probed = pair.client.paths().find(pair.socket(1U));
    REQUIRE(probed != kQuicNoPath);
    REQUIRE(net.run_until([&] { return pair.client.paths().proven(probed) || up.step(); }, 600000U * kMs));
    CHECK(pair.client.paths().proven(probed));
    CHECK(pair.client.path().socket == 0U); // probing moved nothing (section 9.1)
    CHECK(pair.server.path().peer == net.client_at);
    CHECK(pair.client.counters().path_validations >= 1U);
    CHECK(pair.client.recovery().path_floor() == 0U);
    CHECK(net.sent[0] > sent_before);

    REQUIRE(up.read < total);
    REQUIRE(pair.client.migrate(pair.socket(1U), net.now) == QuicMigration::Started);
    CHECK(pair.client.path().socket == 1U);
    CHECK(pair.client.counters().migrations == 1U);
    CHECK(pair.client.recovery().path_floor() != 0U); // nothing sent from the old socket counts for the new path
    CHECK(pair.client.congestion().window() == 12000U);
    // The server's datagrams still reach the path it left, until the server follows.
    CHECK(pair.client.paths().find(pair.socket(0U)) != kQuicNoPath);
    REQUIRE(net.run_until([&] { return up.step(); }, 600000U * kMs));
    // Proven and followed: the path it left is forgotten and its peer ID retired.
    CHECK(pair.client.paths().find(pair.socket(0U)) == kQuicNoPath);
    CHECK(pair.server.path().peer == net.socket_at[0]);
    CHECK(pair.server.counters().migrations >= 1U);
    CHECK(pair.server.counters().path_validations >= 1U);
    CHECK(pair.server.address_validated());
    CHECK(pair.client.state() == QuicConnectionState::Established);
    CHECK(pair.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC client migration: a path on a held socket gets nothing and changes nothing, while a path on another "
          "still leaves",
          "[net][net-path][quic-connection][client-migration]")
{
    Hosted pair(0U, false, false);
    pair.confirm();
    const u64 now = pair.net->now + (100U * kMs); // past any pacing of what confirm() left
    u8 out[1500];
    const Span<u8> room(out, sizeof(out));
    while (pair.client.send(room, now) != 0U)
    {
    }
    CHECK_FALSE(pair.client.held_back()); // nothing owed: nothing waits
    const auto unchanged = [&](u32 held)
    {
        const QuicConnectionCounters before = pair.client.counters();
        u64 numbers[kQuicSpaces] = {};
        for (usize space = 0U; space < kQuicSpaces; ++space)
        {
            numbers[space] = pair.client.next_packet_number(static_cast<QuicSpace>(space));
        }
        const Ecn marking = pair.client.marking();
        CHECK(pair.client.send(room, now, held) == 0U);
        CHECK(pair.client.held_back());
        CHECK(std::memcmp(&before, &pair.client.counters(), sizeof(before)) == 0);
        for (usize space = 0U; space < kQuicSpaces; ++space)
        {
            CHECK(pair.client.next_packet_number(static_cast<QuicSpace>(space)) == numbers[space]);
        }
        CHECK(pair.client.marking() == marking);
    };

    // The active path's socket held: the ping waits, then leaves once the socket is free.
    pair.client.ping();
    unchanged(0b01U);
    unchanged(0b11U);
    CHECK(pair.client.send(room, now, 0b10U) != 0U); // another socket held: the active path's leaves
    CHECK(pair.client.sent_path().socket == 0U);
    CHECK_FALSE(pair.client.held_back());

    // A probe on socket 1 leaves while socket 0 is held; held itself, it waits behind the active path's datagram.
    REQUIRE(pair.client.probe_path(pair.socket(1U), now) == QuicMigration::Started);
    pair.client.ping();
    usize written = pair.client.send(room, now, 0b10U);
    CHECK(written != 0U);
    CHECK(pair.client.sent_path().socket == 0U);
    CHECK(pair.client.held_back()); // the challenge waits for socket 1
    CHECK(pair.client.send(room, now, 0b10U) == 0U);
    CHECK(pair.client.held_back());
    written = pair.client.send(room, now, 0b01U);
    CHECK(written == kQuicMinInitialDatagram); // the probing datagram, padded to the floor
    CHECK(pair.client.sent_path() == pair.socket(1U));
    CHECK_FALSE(pair.client.held_back());

    // A closing connection's close waits for its socket too.
    pair.client.close(3U, {}, now);
    unchanged(0b01U);
    CHECK(pair.client.send(room, now, 0b10U) != 0U);
    CHECK(pair.client.sent_path().socket == 0U);
}

TEST_CASE("QUIC client migration: a move without probing, the old network gone at once, carries transfers both ways "
          "over paths that lose, duplicate and reorder",
          "[net][net-path][quic-connection][client-migration]")
{
    for (u64 seed = 1U; seed <= 3U; ++seed)
    {
        INFO("seed " << seed);
        Hosted pair(seed);
        Net& net = *pair.net;
        net.rng.state = seed;
        for (Path& path : net.path)
        {
            path.loss = 20U;
            path.duplicate = 10U;
            path.jitter_ns = 10U * kMs;
        }
        pair.confirm();
        constexpr u64 total = 1U << 19U;
        Bulk up{pair.client, pair.server, total};
        u64 down_written = 0U;
        u64 down_read = 0U;
        const auto step = [&]
        {
            const bool up_done = up.step();
            if (pair.server.streams().recv_state(kStream) != QuicRecvState::None && down_written < total)
            {
                static_cast<void>(write_pattern(pair.server.streams(), kStream, down_written, total, 7U, false));
            }
            if (pair.client.streams().recv_state(kStream) != QuicRecvState::None)
            {
                static_cast<void>(drain(pair.client.streams(), kStream, down_read, 7U));
            }
            return up_done && down_read >= total;
        };
        REQUIRE(net.run_until([&] { return step() || (up.read >= total / 3U && down_read >= total / 3U); },
                              600000U * kMs));
        REQUIRE(pair.client.migrate(pair.socket(1U), net.now) == QuicMigration::Started);
        net.down = 1U; // socket 0's network is gone
        REQUIRE(net.run_until([&] { return step(); }, 600000U * kMs));
        CHECK(pair.client.path().socket == 1U);
        CHECK(pair.client.paths().proven(pair.client.paths().active()));
        CHECK(pair.client.paths().find(pair.socket(0U)) == kQuicNoPath);
        CHECK(pair.server.path().peer == net.socket_at[0]);
        CHECK(pair.server.recovery().path_floor() != 0U);
        CHECK(pair.client.state() == QuicConnectionState::Established);
        CHECK(pair.server.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC client migration: a socket that reaches nothing fails validation, and the client reverts to the path "
          "before it; a probe that fails moves nothing",
          "[net][net-path][quic-connection][client-migration]")
{
    Hosted pair;
    Net& net = *pair.net;
    pair.confirm();
    net.down = 1U << 1U; // socket 1 reaches nothing
    constexpr u64 total = 1U << 18U;
    Bulk up{pair.client, pair.server, total};
    REQUIRE(net.run_until([&] { return up.step() || up.read >= total / 4U; }, 600000U * kMs));
    REQUIRE(pair.client.migrate(pair.socket(1U), net.now) == QuicMigration::Started);
    CHECK(pair.client.path().socket == 1U);
    REQUIRE(net.run_until([&] { return pair.client.path().socket == 0U; }, 600000U * kMs));
    CHECK(pair.client.counters().path_failures == 1U);
    CHECK(pair.client.counters().migrations == 2U); // there and back
    CHECK(pair.client.paths().find(pair.socket(1U)) == kQuicNoPath);
    // The ID the dead path took is retired and replaced: a probe of another socket finds one.
    net.down |= 1U << 2U;
    REQUIRE(net.run_until(
        [&] { return pair.client.probe_path(pair.socket(2U), net.now) != QuicMigration::NoConnectionId; },
        600000U * kMs));
    REQUIRE(pair.client.paths().find(pair.socket(2U)) != kQuicNoPath);
    REQUIRE(net.run_until([&] { return pair.client.counters().path_failures == 2U; }, 600000U * kMs));
    CHECK(pair.client.path().socket == 0U);
    CHECK(pair.client.counters().migrations == 2U);
    CHECK(pair.client.paths().find(pair.socket(2U)) == kQuicNoPath);
    REQUIRE(net.run_until([&] { return up.step(); }, 600000U * kMs));
    CHECK(pair.server.path().peer == net.client_at);
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC client migration: moves one after another, back to the first socket too, each on the IDs the one "
          "before left",
          "[net][net-path][quic-connection][client-migration]")
{
    Hosted pair;
    Net& net = *pair.net;
    for (Path& path : net.path)
    {
        path.rate = 1000000U; // 8 Mbit/s and a 64 KB queue: the transfer outlasts four moves
        path.buffer = 64000U;
    }
    pair.confirm();
    constexpr u64 total = 1U << 24U;
    Bulk up{pair.client, pair.server, total};
    const auto settled = [&]
    {
        // The server follows the last move, and its check of the path before (section 9.3.3) ends: with
        // active_connection_id_limit 2 at both ends, the IDs the next move takes are the ones these leave.
        bool validating = false;
        for (usize index = 0U; index < kQuicMaxPaths; ++index)
        {
            validating = validating || (pair.server.paths().used(index) && pair.server.paths().validating(index));
        }
        const u32 socket = pair.client.path().socket;
        const Endpoint& at = (socket == 0U) ? net.client_at : net.socket_at[socket - 1U];
        return pair.server.path().peer == at && !validating && pair.client.paths().fallback() == kQuicNoPath;
    };
    const u32 order[] = {1U, 2U, 3U, 0U};
    for (const u32 socket : order)
    {
        INFO("socket " << socket);
        REQUIRE(net.run_until([&] { return up.step() || settled(); }, 600000U * kMs));
        // The server's replacement for the ID the client retired on leaving its last path may still be on its way.
        QuicMigration result = QuicMigration::NoConnectionId;
        REQUIRE(net.run_until(
            [&]
            {
                result = pair.client.migrate(pair.socket(socket), net.now);
                return result != QuicMigration::NoConnectionId;
            },
            600000U * kMs));
        REQUIRE(result == QuicMigration::Started);
        REQUIRE(up.read < total);
        REQUIRE(net.run_until([&] { return settled() || up.step(); }, 600000U * kMs));
        CHECK(pair.client.path().socket == socket);
        CHECK(pair.client.paths().proven(pair.client.paths().active()));
    }
    REQUIRE(net.run_until([&] { return up.step(); }, 600000U * kMs));
    CHECK(pair.client.counters().migrations == 4U);
    CHECK(pair.client.counters().path_failures == 0U);
    CHECK(pair.server.path().peer == net.client_at);
    CHECK(pair.server.counters().migrations == 4U);
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC client migration: a quiet client's move reaches its server in a packet of its own",
          "[net][net-path][quic-connection][client-migration]")
{
    Hosted pair(0U, false, false);
    Net& net = *pair.net;
    pair.confirm();
    net.advance(net.now + 1000U * kMs); // nothing left to send or acknowledge
    REQUIRE(pair.client.migrate(pair.socket(1U), net.now) == QuicMigration::Started);
    // The server follows on the first datagram from socket 1, one way's 25 ms later.
    REQUIRE(net.run_until([&] { return pair.server.path().peer == net.socket_at[0]; }, 30U * kMs));
    REQUIRE(net.run_until([&] { return pair.client.paths().fallback() == kQuicNoPath; }, 10000U * kMs));
    CHECK(pair.client.paths().proven(pair.client.paths().active()));
    REQUIRE(net.run_until([&] { return pair.server.address_validated(); }, 10000U * kMs));
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC client migration: a quiet client that reverts from a path carrying one way tells its server at once",
          "[net][net-path][quic-connection][client-migration]")
{
    Hosted pair(0U, false, false);
    Net& net = *pair.net;
    pair.confirm();
    net.advance(net.now + 1000U * kMs);
    net.down_in = 1U << 1U; // the server's datagrams to socket 1 are lost; the client's from it arrive
    REQUIRE(pair.client.migrate(pair.socket(1U), net.now) == QuicMigration::Started);
    REQUIRE(net.run_until([&] { return pair.server.path().peer == net.socket_at[0]; }, 10000U * kMs));
    REQUIRE(net.run_until([&] { return pair.client.path().socket == 0U; }, 10000U * kMs));
    CHECK(pair.client.counters().path_failures == 1U);
    // Its return reaches the server one way's 25 ms later, in its first datagram from there.
    REQUIRE(net.run_until([&] { return pair.server.path().peer == net.client_at; }, 30U * kMs));
    CHECK(pair.client.state() == QuicConnectionState::Established);
    CHECK(pair.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC client migration: after a rebinding whose old mapping stands, a server frees the ID both of its paths "
          "hold for a move that waits",
          "[net][net-path][quic-connection][client-migration]")
{
    Hosted pair;
    Net& net = *pair.net;
    for (Path& path : net.path)
    {
        path.rate = 1000000U;
        path.buffer = 64000U;
    }
    pair.confirm();
    constexpr u64 total = 1U << 24U;
    Bulk up{pair.client, pair.server, total};
    REQUIRE(net.run_until([&] { return up.step() || up.read >= total / 8U; }, 600000U * kMs));
    // A NAT rebinding: the server follows on the same ID and checks the old mapping, which still answers.
    net.also_at[0] = net.client_at;
    net.client_at.port = 50001U;
    const auto quiet = [&]
    {
        bool validating = false;
        for (usize index = 0U; index < kQuicMaxPaths; ++index)
        {
            validating = validating || (pair.server.paths().used(index) && pair.server.paths().validating(index));
        }
        return !validating;
    };
    REQUIRE(net.run_until([&] { return up.step() || (pair.server.path().peer == net.client_at && quiet()); },
                          600000U * kMs));
    REQUIRE(pair.server.paths().find(QuicPath{0U, net.also_at[0]}) != kQuicNoPath);
    for (const u32 socket : {1U, 2U})
    {
        INFO("socket " << socket);
        QuicMigration result = QuicMigration::NoConnectionId;
        REQUIRE(net.run_until(
            [&]
            {
                result = pair.client.migrate(pair.socket(socket), net.now);
                return result != QuicMigration::NoConnectionId;
            },
            600000U * kMs));
        REQUIRE(result == QuicMigration::Started);
        REQUIRE(net.run_until(
            [&] { return up.step() || (pair.server.path().peer == net.socket_at[socket - 1U] && quiet()); },
            600000U * kMs));
        CHECK(pair.server.path().peer == net.socket_at[socket - 1U]);
    }
    // The two paths of the rebinding held one ID: both are forgotten so that it retires.
    CHECK(pair.server.paths().find(QuicPath{0U, net.also_at[0]}) == kQuicNoPath);
    CHECK(pair.server.paths().find(QuicPath{0U, net.client_at}) == kQuicNoPath);
    REQUIRE(net.run_until([&] { return up.step(); }, 600000U * kMs));
    CHECK(pair.client.counters().path_failures == 0U);
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

namespace
{
// Keeps a copy of the last client datagram that left. With `moved` set it loses the first one `moved` sends from
// the preferred address once there, the PATH_RESPONSE it carries with it: the server sees the move before its
// validation ends.
class Capture final : public Filter
{
public:
    bool drop(usize from, ConstSpan<u8> datagram) override
    {
        if (from != 0U)
        {
            return false;
        }
        std::memcpy(bytes, datagram.data(), datagram.size());
        size = datagram.size();
        const bool lose = moved != nullptr && !lost && moved->on_preferred_address();
        lost = lost || lose;
        return lose;
    }

    const QuicConnection* moved = nullptr;
    bool lost = false;
    u8 bytes[kDatagram] = {};
    usize size = 0U;
};

// Loses every datagram `client` sends to the server's first address once it is confirmed, and keeps the client IDs
// `server` held when it first sent from its preferred socket.
class FirstAddressDark final : public Filter
{
public:
    void sent(usize from, ConstSpan<u8> /*datagram*/) override
    {
        if (from == 1U && answered == 0U && server->sent_path().socket == 1U)
        {
            answered = server->peer_cid_count();
        }
    }

    bool drop(usize from, ConstSpan<u8> /*datagram*/) override
    {
        return from == 0U && client->confirmed() && client->sent_path().peer == kServerAt;
    }

    const QuicConnection* client = nullptr;
    const QuicConnection* server = nullptr;
    usize answered = 0U;
};

// Every path a server knows is on `socket`.
[[nodiscard]] bool all_on(const QuicConnection& server, u32 socket)
{
    for (usize index = 0U; index < kQuicMaxPaths; ++index)
    {
        if (server.paths().used(index) && server.paths().path(index).socket != socket)
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("QUIC preferred address: once confirmed the client validates it and moves; the server answers there, moves "
          "only once it validated the client's path there, and then drops what reaches its first socket",
          "[net][net-path][quic-connection][preferred-address]")
{
    Hosted pair(0U, false, true, Preferred::Follow);
    Net& net = *pair.net;
    Capture capture;
    capture.moved = &pair.client;
    net.filter = &capture;
    REQUIRE(net.run_until([&] { return pair.client.confirmed() && pair.server.confirmed(); }));
    const QuicTransportParameters& offered = pair.client.peer_parameters();
    CHECK(offered.has_preferred_address);
    CHECK(offered.preferred_address.connection_id.bytes == sizeof(kNetServerCid));
    CHECK(pair.server.local_cid_count() == 2U); // sequence 1 is the preferred address's
    CHECK(pair.client.paths().find(QuicPath{0U, kPreferredAt}) != kQuicNoPath);
    CHECK_FALSE(pair.client.on_preferred_address()); // validated first (section 9.6.2)
    CHECK(pair.client.path().peer == kServerAt);

    // The server's datagrams stay on its first socket until it validated the client's path at its preferred one.
    bool early = false;
    bool client_first = false;
    REQUIRE(net.run_until(
        [&]
        {
            const QuicPaths& paths = pair.server.paths();
            early = early || (pair.server.path().socket == 1U && !paths.validated(paths.active()));
            client_first = client_first || (pair.client.on_preferred_address() && pair.server.path().socket == 0U);
            return pair.client.on_preferred_address() && pair.server.on_preferred_address();
        }));
    CHECK_FALSE(early);
    CHECK(client_first);
    CHECK(capture.lost);
    CHECK(pair.server.counters().path_validations >= 1U);
    CHECK(pair.client.path() == QuicPath{0U, kPreferredAt});
    CHECK(pair.server.path() == QuicPath{1U, net.client_at});
    CHECK(pair.client.counters().migrations == 1U);
    CHECK(pair.server.counters().migrations == 1U);
    CHECK(all_on(pair.server, 1U)); // its first socket's paths are left for good

    u64 id = 0U;
    REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None);
    Bulk up{pair.client, pair.server, 1U << 18U};
    REQUIRE(net.run_until([&] { return up.step(); }, 600000U * kMs));
    CHECK(pair.client.paths().find(QuicPath{0U, kServerAt}) == kQuicNoPath); // the server followed: forgotten
    CHECK(pair.client.counters().path_failures == 0U);
    CHECK(pair.server.counters().path_failures == 0U);

    // A datagram still reaching the first socket is dropped unread.
    REQUIRE(capture.size != 0U);
    const QuicConnectionCounters before = pair.server.counters();
    pair.server.receive(QuicPath{0U, net.client_at}, Span<u8>(capture.bytes, capture.size), Ecn::NotEct, net.now);
    CHECK(pair.server.counters().foreign_datagrams == before.foreign_datagrams + 1U);
    CHECK(pair.server.counters().packets_opened == before.packets_opened);
    CHECK(pair.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC preferred address: the client's challenge there carries its connection IDs, so a server that reads "
          "nothing else of the client's after confirmation has one unused to answer from its preferred socket",
          "[net][net-path][quic-connection][preferred-address]")
{
    // A server answers from a new address only with an ID it has not used elsewhere (RFC 9000 section 9.5); the
    // client issues its IDs once confirmed, in the same send as the challenge, and its active path's may arrive later.
    Hosted pair(0U, false, true, Preferred::Follow);
    Net& net = *pair.net;
    FirstAddressDark dark;
    dark.client = &pair.client;
    dark.server = &pair.server;
    net.filter = &dark;
    REQUIRE(net.run_until([&] { return pair.client.confirmed(); }));
    const usize issued = pair.server.peer_cid_count();
    REQUIRE(net.run_until([&] { return pair.client.on_preferred_address() && pair.server.on_preferred_address(); }));
    CHECK(issued == 1U);
    CHECK(dark.answered > issued);
    CHECK(pair.client.path() == QuicPath{0U, kPreferredAt});
    CHECK(pair.server.path() == QuicPath{1U, net.client_at});
    CHECK(pair.client.counters().path_failures == 0U);
    CHECK(pair.server.counters().path_failures == 0U);
}

TEST_CASE("QUIC preferred address: a preferred socket that reaches nothing fails validation, and the client stays at "
          "the first address",
          "[net][net-path][quic-connection][preferred-address]")
{
    Hosted pair(0U, false, true, Preferred::Follow);
    Net& net = *pair.net;
    net.server_down = 0b10U;
    REQUIRE(net.run_until([&] { return pair.client.confirmed(); }));
    const usize issued = pair.server.peer_cid_count();
    CHECK(issued == 1U);
    pair.confirm();
    Bulk up{pair.client, pair.server, 1U << 18U};
    REQUIRE(net.run_until([&]
                          { return up.step() && pair.client.paths().find(QuicPath{0U, kPreferredAt}) == kQuicNoPath; },
                          600000U * kMs));
    CHECK_FALSE(pair.client.on_preferred_address());
    CHECK_FALSE(pair.server.on_preferred_address());
    CHECK(pair.client.path() == QuicPath{0U, kServerAt});
    CHECK(pair.server.path().socket == 0U);
    CHECK(pair.client.counters().path_failures >= 1U);
    CHECK(pair.client.counters().migrations == 0U);
    CHECK(pair.client.state() == QuicConnectionState::Established);
    CHECK(pair.server.peer_cid_count() > issued); // the IDs its challenges carried came again on the first path
}

TEST_CASE("QUIC preferred address: a client that does not follow it moves only when told, migrate() waiting for the "
          "validation; disable_active_migration closes the handshake address alone",
          "[net][net-path][quic-connection][preferred-address]")
{
    const QuicPath preferred{0U, kPreferredAt};
    {
        Hosted pair(0U, true, true, Preferred::Manual);
        Net& net = *pair.net;
        pair.confirm();
        CHECK(pair.client.paths().find(preferred) == kQuicNoPath); // nothing by itself
        CHECK(pair.client.migrate(pair.socket(1U), net.now) == QuicMigration::Disabled);
        REQUIRE(pair.client.probe_path(preferred, net.now) == QuicMigration::Started);
        const usize probed = pair.client.paths().find(preferred);
        REQUIRE(probed != kQuicNoPath);
        REQUIRE(net.run_until([&] { return pair.client.paths().proven(probed); }));
        CHECK(pair.client.path().peer == kServerAt); // a probe moves nothing
        CHECK_FALSE(pair.client.on_preferred_address());

        // Proven: the move is at once, and the server follows once it validated the path.
        REQUIRE(pair.client.migrate(preferred, net.now) == QuicMigration::Started);
        CHECK(pair.client.path() == preferred);
        CHECK(pair.client.on_preferred_address());
        REQUIRE(net.run_until([&] { return pair.server.on_preferred_address(); }));

        // The handshake address is left for good; from the preferred one, the client's own moves are open.
        CHECK(pair.client.probe_path(QuicPath{1U, kServerAt}, net.now) == QuicMigration::OtherServer);
        CHECK(pair.client.migrate(QuicPath{1U, kServerAt}, net.now) == QuicMigration::OtherServer);
        pair.client.ping();
        REQUIRE(net.run_until([&] { return pair.client.paths().fallback() == kQuicNoPath; }));
        REQUIRE(net.run_until(
            [&] { return pair.client.migrate(QuicPath{1U, kPreferredAt}, net.now) == QuicMigration::Started; }));
        REQUIRE(net.run_until([&] { return pair.server.path().peer == net.socket_at[0]; }));
        CHECK(pair.server.path().socket == 1U);
        CHECK(pair.client.state() == QuicConnectionState::Established);
    }
    {
        // Not proven: migrate() starts the validation and moves once it ends.
        Hosted pair(0U, false, true, Preferred::Manual);
        Net& net = *pair.net;
        pair.confirm();
        REQUIRE(pair.client.migrate(preferred, net.now) == QuicMigration::Started);
        CHECK(pair.client.path().peer == kServerAt);
        CHECK_FALSE(pair.client.on_preferred_address());
        CHECK(pair.client.counters().migrations == 0U);
        REQUIRE(net.run_until([&] { return pair.client.on_preferred_address(); }));
        CHECK(pair.client.path() == preferred);
        REQUIRE(net.run_until([&] { return pair.server.on_preferred_address(); }));
    }
    {
        // A move asked for and still validating follows the client to the socket it moves to meanwhile: the socket
        // left behind never draws it back.
        Hosted pair(0U, false, true, Preferred::Manual, 4U);
        Net& net = *pair.net;
        pair.confirm();
        net.server_down = 0b10U; // the validation from socket 0 is held back
        REQUIRE(pair.client.migrate(preferred, net.now) == QuicMigration::Started);
        net.advance(net.now + 50U * kMs);
        REQUIRE(net.run_until([&] { return pair.client.migrate(pair.socket(1U), net.now) == QuicMigration::Started; }));
        CHECK(pair.client.paths().find(preferred) == kQuicNoPath);
        CHECK(pair.client.paths().find(QuicPath{1U, kPreferredAt}) != kQuicNoPath);
        net.server_down = 0U;
        REQUIRE(
            net.run_until([&] { return pair.client.on_preferred_address() && pair.server.on_preferred_address(); }));
        CHECK(pair.client.path() == QuicPath{1U, kPreferredAt});
        CHECK(pair.server.path() == QuicPath{1U, net.socket_at[0]});
    }
}

TEST_CASE("QUIC preferred address: a client that moves before reaching it validates it again from its new socket, "
          "and moves there as its first network goes",
          "[net][net-path][quic-connection][preferred-address]")
{
    // Each new address of either end takes an ID of the other's (section 9.5): the client's second socket to both
    // server addresses, and the server's preferred one to both client addresses, need four at each end.
    Hosted pair(0U, false, true, Preferred::Follow, 4U);
    Net& net = *pair.net;
    REQUIRE(net.run_until([&] { return pair.client.confirmed() && pair.server.confirmed(); }));
    REQUIRE_FALSE(pair.client.on_preferred_address());
    REQUIRE(pair.client.paths().find(QuicPath{0U, kPreferredAt}) != kQuicNoPath);
    net.down = 0b1U; // socket 0's network is gone
    REQUIRE(net.run_until([&] { return pair.client.migrate(pair.socket(1U), net.now) == QuicMigration::Started; }));
    CHECK(pair.client.paths().find(QuicPath{0U, kPreferredAt}) == kQuicNoPath); // left with its socket
    CHECK(pair.client.paths().find(QuicPath{1U, kPreferredAt}) != kQuicNoPath);
    // Within a few round trips: the server answers the probe once an ID of the client's reaches it.
    REQUIRE(net.run_until([&] { return pair.client.on_preferred_address() && pair.server.on_preferred_address(); },
                          500U * kMs));
    CHECK(pair.client.path() == QuicPath{1U, kPreferredAt});
    CHECK(pair.server.path() == QuicPath{1U, net.socket_at[0]});
    u64 id = 0U;
    REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None);
    Bulk up{pair.client, pair.server, 1U << 18U};
    REQUIRE(net.run_until([&] { return up.step(); }, 600000U * kMs));
    CHECK(all_on(pair.server, 1U));
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC preferred address: a client its NAT rebinds on its way there keeps its ID, which the server's path "
          "from the new address shares, and the server validates that path and moves there",
          "[net][net-path][quic-connection][preferred-address]")
{
    Hosted pair(0U, false, true, Preferred::Follow);
    Net& net = *pair.net;
    REQUIRE(net.run_until([&] { return pair.client.on_preferred_address(); }));
    REQUIRE_FALSE(pair.server.on_preferred_address());
    const QuicPaths& paths = pair.server.paths();
    const usize first = paths.find(QuicPath{1U, net.client_at});
    REQUIRE(first != kQuicNoPath);
    const u64 held = paths.peer_sequence(first);
    // The server holds two of the client's IDs (the default limit): its first socket's path's and this one's. The
    // NAT maps the client elsewhere and forgets the old mapping; the client kept its ID, so the new path shares the
    // one sent to the path it rebinds (RFC 9000 section 9.5), validates and takes the move at once.
    net.client_at.port = 50001U;
    const QuicPath rebound{1U, net.client_at};
    pair.client.ping();
    REQUIRE(net.run_until([&] { return paths.find(rebound) != kQuicNoPath; }));
    CHECK(paths.peer_sequence(paths.find(rebound)) == held);
    REQUIRE(net.run_until([&] { return pair.server.on_preferred_address(); }, 1000U * kMs));
    CHECK(pair.server.path() == rebound);
    CHECK(paths.validated(paths.active()));
    u64 id = 0U;
    REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None);
    Bulk up{pair.client, pair.server, 1U << 18U};
    REQUIRE(net.run_until([&] { return up.step(); }, 600000U * kMs));
    CHECK(all_on(pair.server, 1U));
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

namespace
{
// Loses what a server sends from its preferred socket while `armed`.
class PreferredLoss final : public Filter
{
public:
    explicit PreferredLoss(const QuicConnection& server) : m_server(server) {}

    bool drop(usize from, ConstSpan<u8> /*datagram*/) override
    {
        return armed && from == 1U && m_server.sent_path().socket == 1U;
    }

    bool armed = false;

private:
    const QuicConnection& m_server;
};
} // namespace

TEST_CASE("QUIC preferred address: until the server validated its client's path there, it sends from its first "
          "socket to the address the client moved from, when validated there",
          "[net][net-path][quic-connection][preferred-address]")
{
    Hosted pair(0U, false, true, Preferred::Manual);
    Net& net = *pair.net;
    PreferredLoss loss(pair.server);
    net.filter = &loss;
    pair.confirm();
    const QuicPaths& paths = pair.server.paths();
    const auto quiet = [&]
    {
        bool validating = false;
        for (usize index = 0U; index < kQuicMaxPaths; ++index)
        {
            validating = validating || (paths.used(index) && paths.validating(index));
        }
        return !validating;
    };
    // The NAT maps the client from a to b and back, each mapping standing: the server's first socket validates both
    // and is back at a.
    const Endpoint a = net.client_at;
    const Endpoint b{a.address, 50001U};
    for (const Endpoint& to : {b, a})
    {
        net.also_at[0] = net.client_at;
        net.client_at = to;
        pair.client.ping();
        REQUIRE(net.run_until([&] { return pair.server.path().peer == to && quiet(); }, 5000U * kMs));
    }
    const usize at_b = paths.find(QuicPath{0U, b});
    REQUIRE(at_b != kQuicNoPath);
    REQUIRE(paths.validated(at_b));
    // The client proves the preferred address from a.
    const QuicPath preferred{0U, kPreferredAt};
    REQUIRE(pair.client.probe_path(preferred, net.now) == QuicMigration::Started);
    REQUIRE(net.run_until([&] { return pair.client.paths().proven(pair.client.paths().find(preferred)); }));
    // Then to b for good while the client moves to the preferred address, whose path from b the server cannot
    // validate yet: its first socket follows the client to b meanwhile (section 9.6.2), and it moves once it can.
    net.client_at = b;
    net.also_at[0] = Endpoint{};
    loss.armed = true;
    REQUIRE(pair.client.migrate(preferred, net.now) == QuicMigration::Started);
    REQUIRE(pair.client.on_preferred_address());
    REQUIRE(paths.find(QuicPath{1U, b}) == kQuicNoPath);
    REQUIRE(net.run_until([&] { return paths.find(QuicPath{1U, b}) != kQuicNoPath; }, 1000U * kMs));
    CHECK(pair.server.path() == QuicPath{0U, b});
    CHECK_FALSE(pair.server.on_preferred_address());
    loss.armed = false;
    REQUIRE(net.run_until([&] { return pair.server.on_preferred_address(); }, 5000U * kMs));
    CHECK(pair.server.path() == QuicPath{1U, b});
    Bulk up{pair.client, pair.server, 1U << 18U};
    REQUIRE(net.run_until([&] { return up.step(); }, 600000U * kMs));
    CHECK(all_on(pair.server, 1U));
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC preferred address: the move carries transfers both ways over paths that lose, duplicate and reorder",
          "[net][net-path][quic-connection][preferred-address]")
{
    for (u64 seed = 1U; seed <= 3U; ++seed)
    {
        INFO("seed " << seed);
        Hosted pair(seed, false, true, Preferred::Follow);
        Net& net = *pair.net;
        net.rng.state = seed;
        for (Path& path : net.path)
        {
            path.loss = 20U;
            path.duplicate = 10U;
            path.jitter_ns = 10U * kMs;
        }
        pair.confirm();
        constexpr u64 total = 1U << 18U;
        Bulk up{pair.client, pair.server, total};
        u64 back_sent = 0U;
        u64 back_read = 0U;
        REQUIRE(net.run_until(
            [&]
            {
                const bool upward = up.step();
                if (pair.server.streams().recv_state(kStream) != QuicRecvState::None && back_sent < total)
                {
                    static_cast<void>(
                        write_pattern(pair.server.streams(), kStream, back_sent, total, kStream + 1U, false));
                }
                static_cast<void>(drain(pair.client.streams(), kStream, back_read, kStream + 1U));
                return upward && back_read >= total && pair.server.on_preferred_address();
            },
            600000U * kMs));
        CHECK(pair.client.on_preferred_address());
        CHECK(pair.client.path() == QuicPath{0U, kPreferredAt});
        CHECK(pair.server.path() == QuicPath{1U, net.client_at});
        CHECK(pair.client.state() == QuicConnectionState::Established);
        CHECK(pair.server.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC preferred address: configurations out of range, a server without IDs or opened on that socket, an "
          "address of the other family, an IPv4-mapped path, and a server without one",
          "[net][net-path][quic-connection][preferred-address]")
{
    Identity identity;
    CountingAllocator allocator;
    const auto ready = [&](const QuicConnectionConfig& config)
    {
        return QuicConnection(allocator, config).ready();
    };
    QuicConnectionConfig server = config_for(QuicSide::Server, identity, QuicVersion::V1, 1U);
    server.parameters.has_preferred_address = true;
    server.preferred_socket = 1U;
    CHECK_FALSE(ready(server)); // neither family given
    server.parameters.preferred_address.ipv4[0] = 192U;
    CHECK_FALSE(ready(server)); // an address without a port
    server.parameters.preferred_address.ipv4_port = 4433U;
    CHECK(ready(server));
    server.parameters.preferred_address.connection_id.bytes = 8U;
    CHECK_FALSE(ready(server)); // the connection issues its ID
    server.parameters.preferred_address.connection_id.bytes = 0U;
    QuicConnectionConfig client = config_for(QuicSide::Client, identity, QuicVersion::V1, 1U);
    client.parameters = server.parameters;
    CHECK_FALSE(ready(client)); // a server's parameter

    // Without IDs a server cannot issue sequence 1: it refuses to start.
    QuicConnection unrouted(allocator, server);
    REQUIRE(unrouted.ready());
    QuicAccept accept;
    accept.local_cid = ConstSpan<u8>(kNetServerCid, sizeof(kNetServerCid));
    accept.client_dcid = ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid));
    accept.client_scid = ConstSpan<u8>(kClientCid, sizeof(kClientCid));
    accept.original_dcid = accept.client_dcid;
    CHECK_FALSE(unrouted.accept(accept, kStartNs));

    {
        Hosted there(0U, false, true, Preferred::OpenedThere);
        there.confirm();
        CHECK_FALSE(there.client.peer_parameters().has_preferred_address);
        CHECK(there.server.local_cid_count() == 2U); // its next ID as a NEW_CONNECTION_ID frame
    }
    {
        // An IPv4 client never follows an IPv6 address by itself, and may still probe it.
        Hosted other(0U, false, true, Preferred::V6Only);
        other.confirm();
        CHECK(other.client.peer_parameters().has_preferred_address);
        const QuicPath v6{0U, Hosted::preferred_v6()};
        CHECK(other.client.paths().find(v6) == kQuicNoPath);
        CHECK(other.client.probe_path(v6, other.net->now) == QuicMigration::Started);
        CHECK_FALSE(other.client.on_preferred_address());
    }
    {
        // A client whose server address is IPv4-mapped follows the address in that form.
        Hosted v4_mapped(0U, false, true, Preferred::Mapped);
        Net& net = *v4_mapped.net;
        REQUIRE(net.run_until(
            [&] { return v4_mapped.client.on_preferred_address() && v4_mapped.server.on_preferred_address(); }));
        CHECK(v4_mapped.client.path() == QuicPath{0U, mapped(kPreferredAt)});
    }
    {
        // A client whose path names no server address never follows one, nor probes it.
        Hosted unbound(0U, false, true, Preferred::Unbound);
        unbound.confirm();
        REQUIRE(unbound.client.peer_parameters().has_preferred_address);
        CHECK(unbound.client.paths().find(QuicPath{0U, kPreferredAt}) == kQuicNoPath);
        CHECK(unbound.client.probe_path(QuicPath{0U, kPreferredAt}, unbound.net->now) == QuicMigration::OtherServer);
        unbound.net->advance(unbound.net->now + 2000U * kMs);
        CHECK_FALSE(unbound.client.on_preferred_address());
        CHECK(unbound.client.counters().migrations == 0U);
    }
    {
        // A server without one drops what reaches any socket but its active path's.
        Hosted plain;
        Capture capture;
        plain.net->filter = &capture;
        plain.confirm();
        REQUIRE(capture.size != 0U);
        const QuicConnectionCounters before = plain.server.counters();
        plain.server.receive(QuicPath{1U, plain.net->client_at}, Span<u8>(capture.bytes, capture.size), Ecn::NotEct,
                             plain.net->now);
        CHECK(plain.server.counters().foreign_datagrams == before.foreign_datagrams + 1U);
        CHECK(plain.server.counters().packets_opened == before.packets_opened);
    }
}

namespace
{
// A LEO access link at the client (#handover), as Starlink's are measured: every `period` from `first` its terminal
// hands over, losing each datagram that crosses its link in the first `outage` (the client's as they leave the
// bottleneck, the server's as they reach the terminal) and adding `spike` to each one-way delay for `spike_for` from
// the event.
class Handovers final : public Filter
{
public:
    Handovers(Net& net, u64 first, u64 period, u64 outage, u64 spike, u64 spike_for)
        : m_net(net), m_base_ns(net.path[0].delay_ns), m_first_ns(first), m_period_ns(period), m_spike_ns(spike),
          m_spike_for_ns(spike_for)
    {
        for (Path& path : net.path)
        {
            path.outage_first_ns = first;
            path.outage_period_ns = period;
            path.outage_ns = outage;
        }
        net.path[1].outage_after_ns = m_base_ns;
    }

    bool drop(usize from, ConstSpan<u8> /*datagram*/) override
    {
        if (from == 0U && m_net.now >= drop_at)
        {
            drop_at = kNeverNs; // the one other loss asked for
            return true;
        }
        return false;
    }

    // Both paths' one-way delay as of now, and no handover from stop_at; called between events.
    void follow()
    {
        if (m_net.now >= stop_at)
        {
            for (Path& path : m_net.path)
            {
                path.outage_ns = 0U;
            }
        }
        const bool spiking =
            m_net.now >= m_first_ns && m_net.now < stop_at && (m_net.now - m_first_ns) % m_period_ns < m_spike_for_ns;
        for (Path& path : m_net.path)
        {
            path.delay_ns = m_base_ns + (spiking ? m_spike_ns : 0U);
        }
    }

    // These handovers on the connection's clock, moved by `shift` (a wrong schedule).
    [[nodiscard]] HandoverSchedule schedule(u64 before, u64 after, u64 shift) const
    {
        HandoverSchedule out;
        out.period_ns = m_period_ns;
        out.phase_ns = (m_first_ns + shift) % m_period_ns;
        out.before_ns = before;
        out.after_ns = after;
        return out;
    }

    [[nodiscard]] u64 first_ns() const { return m_first_ns; }
    [[nodiscard]] u64 taken() const { return m_net.outaged[0] + m_net.outaged[1]; }

    u64 drop_at = kNeverNs; // the first client datagram sent from then is lost too
    u64 stop_at = kNeverNs; // the link hands over no more from then

private:
    Net& m_net;
    u64 m_base_ns;
    u64 m_first_ns;
    u64 m_period_ns;
    u64 m_spike_ns;
    u64 m_spike_for_ns;
};

constexpr u64 kLeoRate = 1250000U; // 10 Mbit/s
constexpr u64 kLeoOneWay = 20U * kMs;
constexpr u64 kLeoFirst = 3000U * kMs; // the first handover, from confirmation
constexpr u64 kLeoAfter = 2000U * kMs; // what follows a handover, measured

// What the client's schedule is: none, the handovers' own, one 7 s off them, or one it learns.
enum class Aware : u8
{
    None,
    True,
    Wrong,
    Learned,
};

// A LEO link's handovers: their outage, delay spike and its length; a genuine loss `genuine_at_ns` after
// confirmation (kNeverNs: none); the schedule's guard after each event.
struct LeoLink
{
    u64 outage_ns = 60U * kMs;
    u64 spike_ns = 30U * kMs;
    u64 spike_for_ns = 150U * kMs;
    u64 genuine_at_ns = kNeverNs;
    u64 guard_after_ns = kStarlinkHandoverGuardNs;
    u64 guard_before_ns = kStarlinkHandoverGuardNs;
    u64 period_ns = kStarlinkHandoverPeriodNs; // the handovers' on the connection's clock
    u64 learn_period_ns = 0U;                  // the period a learner is told (0: period_ns)
    u64 measure_from_ns = 0U;                  // handovers before then are not measured
    u64 after_ns = kLeoAfter;                  // what is measured after each
    u64 stop_ns = kNeverNs;                    // no handover from then
    u64 first_ns = kLeoFirst;                  // the first handover, from confirmation
    usize seed = 3U;                           // the fixture's
};

// What a transfer over a LEO link showed.
struct LeoRun
{
    u64 elapsed_ns = 0U;
    u64 handovers_lost = 0U;  // datagrams the handovers took
    u64 handover_losses = 0U; // the client's losses it kept from congestion control
    CongestionState state;
    u64 min_window = kNeverNs;      // the window's least from the first handover on
    u64 min_target_rate = kNeverNs; // a real-time controller's least target rate then
    u32 min_mtu = ~0U;              // the path MTU's least then
    u64 after_reductions = 0U;      // reductions in the 2 s from each handover
    u64 after_undone = 0U;          // reductions undone then
    u64 after_bytes = 0U;           // bytes the server read then
    u64 measured = 0U;              // handovers measured
    u64 locks = 0U;                 // learned schedules put in force
    bool learned = false;           // the schedule at the end was learned
    i64 phase_offset_ns = 0;        // its phase less the handovers' at the end, within half a period
    u64 lock_ns = kNeverNs;         // when a learned schedule first came in force, from confirmation
    u64 release_ns = kNeverNs;      // ... and last went out of force
};

std::ostream& operator<<(std::ostream& out, const LeoRun& run)
{
    return out << run.elapsed_ns / kMs << " ms, reductions " << run.state.reductions << ", undone " << run.state.undone
               << ", least window " << run.min_window << "; least target " << run.min_target_rate << ", least MTU "
               << run.min_mtu << ", taken " << run.handovers_lost << ", kept " << run.handover_losses
               << "; after: reductions " << run.after_reductions << ", undone " << run.after_undone << ", bytes "
               << run.after_bytes << " over " << run.measured << "; locks " << run.locks << ", learned " << run.learned
               << ", phase offset " << run.phase_offset_ns / 1000 << " us, locked at " << run.lock_ns / kMs
               << " ms, released at " << run.release_ns / kMs << " ms";
}

// The client sends `total` bytes over a 10 Mbit/s link with 40 ms round trips and a queue of one bandwidth-delay
// product, its handovers the link's (by default the first 3 s after confirmation and then every 15 s, Starlink's
// period); both ends ask for `times` receive timestamps an ACK frame.
LeoRun leo_transfer(const CongestionPolicy& policy, u64 total, const LeoLink& link, Aware aware, u64 times = 0U)
{
    Pair pair(policy, link.seed, false, times, times);
    for (Path& path : pair.net->path)
    {
        path.delay_ns = kLeoOneWay;
    }
    pair.net->path[0].rate = kLeoRate;
    pair.net->path[0].buffer = kLeoRate / 1000U * 40U;
    pair.confirm();
    const u64 start = pair.net->now;
    Handovers handovers(*pair.net, start + link.first_ns, link.period_ns, link.outage_ns, link.spike_ns, link.spike_for_ns);
    handovers.drop_at = (link.genuine_at_ns == kNeverNs) ? kNeverNs : start + link.genuine_at_ns;
    handovers.stop_at = (link.stop_ns == kNeverNs) ? kNeverNs : start + link.stop_ns;
    pair.net->filter = &handovers;
    const u64 learn_period = (link.learn_period_ns == 0U) ? link.period_ns : link.learn_period_ns;
    if (aware == Aware::Learned)
    {
        REQUIRE(pair.client.set_handover_learning(
            HandoverLearning{learn_period, link.guard_before_ns, link.guard_after_ns}));
    }
    else if (aware != Aware::None)
    {
        const u64 shift = (aware == Aware::Wrong) ? 7000U * kMs : 0U;
        REQUIRE(pair.client.set_handover(handovers.schedule(link.guard_before_ns, link.guard_after_ns, shift)));
    }
    LeoRun run;
    Bulk bulk{pair.client, pair.server, total};
    u64 next_event = handovers.first_ns();
    u64 window_end = kNeverNs;
    CongestionState at_event;
    u64 read_at = 0U;
    bool was_in_force = false;
    REQUIRE(pair.net->run_until(
        [&]
        {
            handovers.follow();
            const bool in_force = pair.client.handover_learned();
            if (run.lock_ns == kNeverNs && in_force)
            {
                run.lock_ns = pair.net->now - start;
            }
            if (was_in_force && !in_force)
            {
                run.release_ns = pair.net->now - start;
            }
            was_in_force = in_force;
            const CongestionState state = pair.client.congestion().state();
            if (pair.net->now >= next_event)
            {
                at_event = state;
                read_at = bulk.read;
                const bool measured = next_event >= start + link.measure_from_ns && next_event < handovers.stop_at;
                window_end = measured ? next_event + link.after_ns : kNeverNs;
                run.measured += measured ? 1U : 0U;
                next_event += link.period_ns;
            }
            if (pair.net->now >= window_end)
            {
                run.after_reductions += state.reductions - at_event.reductions;
                run.after_undone += state.undone - at_event.undone;
                run.after_bytes += bulk.read - read_at;
                window_end = kNeverNs;
            }
            if (pair.net->now >= handovers.first_ns())
            {
                run.min_window = (state.window < run.min_window) ? state.window : run.min_window;
                run.min_target_rate =
                    (state.target_rate < run.min_target_rate) ? state.target_rate : run.min_target_rate;
                const u32 mtu = pair.client.path_mtu().size();
                run.min_mtu = (mtu < run.min_mtu) ? mtu : run.min_mtu;
            }
            return bulk.step();
        },
        600000U * kMs));
    run.elapsed_ns = pair.net->now - start;
    run.handovers_lost = handovers.taken();
    run.handover_losses = pair.client.counters().handover_losses;
    run.locks = pair.client.counters().handover_locks;
    run.learned = pair.client.handover_learned();
    if (run.learned)
    {
        // The handovers' phase on the clock, read the way the learner's period folds it.
        const u64 first = start + link.first_ns;
        const u64 truth = (first + ((pair.net->now - first) / link.period_ns) * link.period_ns) % learn_period;
        const u64 d = (pair.client.handover().phase_ns + learn_period - truth) % learn_period;
        run.phase_offset_ns = (d <= learn_period / 2U) ? static_cast<i64>(d) : -static_cast<i64>(learn_period - d);
    }
    run.state = pair.client.congestion().state();
    CHECK(pair.client.state() == QuicConnectionState::Established);
    CHECK(pair.server.state() == QuicConnectionState::Established);
    return run;
}

} // namespace

TEST_CASE("QUIC handover: CUBIC and BBR keep a LEO link's handovers 15 s apart from congestion control with the "
          "schedule, and neither without it nor with a wrong one; no handover passes for a black hole",
          "[net][net-path][quic-connection][handover]")
{
    // Two handovers in a transfer of about 20 s: 60 ms lost each, then 30 ms more delay for 150 ms. What two
    // handovers do to the windows is chance (one per seed); the 2 s case below counts it over many.
    constexpr u64 total = 24U << 20U;
    for (const CongestionAlgorithm algorithm : {CongestionAlgorithm::Cubic, CongestionAlgorithm::Bbr})
    {
        INFO("algorithm " << static_cast<u32>(algorithm));
        CongestionPolicy policy;
        policy.algorithm = algorithm;
        const LeoRun blind = leo_transfer(policy, total, LeoLink{}, Aware::None);
        const LeoRun aware = leo_transfer(policy, total, LeoLink{}, Aware::True);
        const LeoRun off = leo_transfer(policy, total, LeoLink{}, Aware::Wrong);
        INFO("blind: " << blind);
        INFO("aware: " << aware);
        INFO("off: " << off);
        CHECK(blind.handovers_lost >= 80U);
        CHECK(blind.handover_losses == 0U);
        CHECK(off.handover_losses == 0U); // 7 s off a 15 s period covers nothing the link loses
        CHECK(aware.handover_losses >= 40U);
        // The handovers' losses never pass for a black hole: the path MTU search keeps its size.
        CHECK(blind.min_mtu == kQuicDefaultMaxDatagram);
        CHECK(aware.min_mtu == kQuicDefaultMaxDatagram);
        CHECK(off.min_mtu == kQuicDefaultMaxDatagram);
    }
}

namespace
{
// A LEO link handing over every 2 s, each handover's reductions counted net of those undone in the 500 ms after it.
LeoLink leo_every_2s(usize seed, u64 first_ms)
{
    LeoLink link;
    link.period_ns = 2000U * kMs;
    link.after_ns = 500U * kMs;
    link.first_ns = first_ms * kMs;
    link.seed = seed;
    return link;
}

i64 net_cuts(const LeoRun& run)
{
    return static_cast<i64>(run.after_reductions) - static_cast<i64>(run.after_undone);
}
} // namespace

TEST_CASE("QUIC handover: over a LEO link handing over every 2 s, CUBIC and BBR are cut at nearly every handover "
          "without the schedule or with a wrong one and seldom with it, and BBR ends sooner with it",
          "[net][net-path][quic-connection][handover]")
{
    // About ten handovers a transfer. Each per-run claim held in all 160 runs of each algorithm over seeds 0 to 3 and
    // first handovers 3.0 to 4.9 s in 100 ms steps, before and after the packet's single entropy draw; these two are
    // among them, and the sums over them (a single run of BBR can be cut nearly as often either way). On a queue of
    // one bandwidth-delay product CUBIC's 0.7 cut still fills the pipe, so its speed either way is chance (blind less
    // aware from -248 to +286 ms) and no claim; BBR's is not (aware slower once in 160, by 107 ms).
    struct Leo
    {
        usize seed;
        u64 first_ms;
    };
    constexpr Leo kRuns[] = {{1U, 3500U}, {3U, 4400U}};
    for (const CongestionAlgorithm algorithm : {CongestionAlgorithm::Cubic, CongestionAlgorithm::Bbr})
    {
        const bool cubic = algorithm == CongestionAlgorithm::Cubic;
        CongestionPolicy policy;
        policy.algorithm = algorithm;
        i64 blind_cuts = 0;
        i64 aware_cuts = 0;
        u64 blind_ns = 0U;
        u64 aware_ns = 0U;
        u64 off_ns = 0U;
        for (const Leo& leo : kRuns)
        {
            INFO("algorithm " << static_cast<u32>(algorithm) << ", seed " << leo.seed << ", first handover "
                              << leo.first_ms << " ms");
            const LeoLink link = leo_every_2s(leo.seed, leo.first_ms);
            const LeoRun blind = leo_transfer(policy, 24U << 20U, link, Aware::None);
            const LeoRun aware = leo_transfer(policy, 24U << 20U, link, Aware::True);
            const LeoRun off = leo_transfer(policy, 24U << 20U, link, Aware::Wrong);
            INFO("blind: " << blind);
            INFO("aware: " << aware);
            INFO("off: " << off);
            const i64 measured = static_cast<i64>(aware.measured);
            CHECK(measured >= 9);
            // Without the schedule, or with one 1 s off every handover (7 s folds into the 2 s period), the
            // handovers' losses reach congestion control: a cut after every handover (CUBIC misses at most one,
            // BBR's probing adds more).
            const i64 at_least = cubic ? measured - 1 : measured;
            CHECK(net_cuts(blind) >= at_least);
            CHECK(net_cuts(off) >= at_least);
            CHECK(blind.handover_losses == 0U);
            // With it they are kept from it, about twenty a handover, and what still cuts is the queue's own
            // overflow (CUBIC about every 2.7 s here) and BBR's probing.
            CHECK(aware.handover_losses >= 10U * aware.measured);
            CHECK(net_cuts(aware) < net_cuts(blind));
            if (cubic)
            {
                CHECK(net_cuts(aware) * 2 <= measured);
                CHECK(net_cuts(aware) + 4 <= net_cuts(blind));
            }
            CHECK(blind.min_mtu == kQuicDefaultMaxDatagram);
            CHECK(aware.min_mtu == kQuicDefaultMaxDatagram);
            CHECK(off.min_mtu == kQuicDefaultMaxDatagram);
            blind_cuts += net_cuts(blind);
            aware_cuts += net_cuts(aware);
            blind_ns += blind.elapsed_ns;
            aware_ns += aware.elapsed_ns;
            off_ns += off.elapsed_ns;
        }
        INFO("algorithm " << static_cast<u32>(algorithm) << ": cuts " << blind_cuts << " blind, " << aware_cuts
                          << " aware; " << blind_ns / kMs << " ms blind, " << aware_ns / kMs << " ms aware, "
                          << off_ns / kMs << " ms off");
        CHECK(aware_cuts * 2 <= blind_cuts);
        if (!cubic)
        {
            CHECK(aware_ns < blind_ns);
            CHECK(aware_ns < off_ns);
        }
    }
}

TEST_CASE("QUIC handover: a LEO link's delay spikes every 2 s reorder trains behind small packets, which pass for no "
          "black hole",
          "[net][net-path][quic-connection][quic-pmtu][handover]")
{
    // Each spike's end lets a small packet sent after it overtake the paced train sent before it, and RFC 9002's time
    // threshold declares the train lost a packet a detection: three suspect detections within 2 ms, which the path MTU
    // search took for a black hole in 13 of 48 such runs before a black hole's losses had to span a round trip.
    struct Leo
    {
        CongestionAlgorithm algorithm;
        usize seed;
        u64 first_ms;
        Aware aware;
    };
    const Leo runs[] = {{CongestionAlgorithm::Cubic, 1U, 5000U, Aware::None},
                        {CongestionAlgorithm::Cubic, 1U, 5000U, Aware::True},
                        {CongestionAlgorithm::Bbr, 2U, 3500U, Aware::None}};
    for (const Leo& leo : runs)
    {
        INFO("algorithm " << static_cast<u32>(leo.algorithm) << ", seed " << leo.seed << ", first handover "
                          << leo.first_ms << " ms, schedule " << static_cast<u32>(leo.aware));
        CongestionPolicy policy;
        policy.algorithm = leo.algorithm;
        const LeoRun run = leo_transfer(policy, 24U << 20U, leo_every_2s(leo.seed, leo.first_ms), leo.aware);
        INFO(run);
        CHECK(run.handovers_lost >= 80U);
        CHECK(run.min_mtu == kQuicDefaultMaxDatagram);
    }
}

TEST_CASE("QUIC handover: SCReAM and GCC hold their target rates through a handover's delay spike, which reads as a "
          "queue without the schedule",
          "[net][net-path][quic-connection][handover]")
{
    constexpr u64 total = 16U << 20U;
    LeoLink spike;
    spike.outage_ns = 40U * kMs;
    spike.spike_ns = 80U * kMs;
    spike.spike_for_ns = 100U * kMs;
    for (const CongestionAlgorithm algorithm : {CongestionAlgorithm::Scream, CongestionAlgorithm::Gcc})
    {
        INFO("algorithm " << static_cast<u32>(algorithm));
        CongestionPolicy policy;
        policy.algorithm = algorithm;
        policy.target_rate_max = 2U * kLeoRate;
        const LeoRun blind = leo_transfer(policy, total, spike, Aware::None);
        const LeoRun aware = leo_transfer(policy, total, spike, Aware::True);
        INFO("blind: " << blind);
        INFO("aware: " << aware);
        CHECK(aware.handover_losses != 0U);
        CHECK(aware.min_target_rate > blind.min_target_rate);
        CHECK(aware.after_bytes > blind.after_bytes);
        CHECK(aware.elapsed_ns <= blind.elapsed_ns);
    }
    // GCC on the peer's receive times, through a 25 ms step alone: those of covered packets are no queue either.
    CongestionPolicy gcc;
    gcc.algorithm = CongestionAlgorithm::Gcc;
    gcc.target_rate_max = 2U * kLeoRate;
    LeoLink step;
    step.outage_ns = 0U;
    step.spike_ns = 25U * kMs;
    step.spike_for_ns = 100U * kMs;
    const LeoRun blind = leo_transfer(gcc, total, step, Aware::None, kQuicMaxReceiveTimestamps);
    const LeoRun aware = leo_transfer(gcc, total, step, Aware::True, kQuicMaxReceiveTimestamps);
    INFO("timestamps, blind: " << blind);
    INFO("timestamps, aware: " << aware);
    CHECK(aware.min_target_rate > blind.min_target_rate);
    CHECK(blind.after_reductions != 0U);
    CHECK(aware.after_reductions == 0U);
    CHECK(aware.elapsed_ns < blind.elapsed_ns);
}

TEST_CASE("QUIC handover: a schedule learned without a clock reading answers the handovers as the authored one does, "
          "on a skewed clock too",
          "[net][net-path][quic-connection][handover]")
{
    // A handover every 2 s from 3 s under BBR's own losses: the learner locks within eight of them (by 21 s), and from
    // then each is measured for 1.5 s after it.
    constexpr u64 total = 40U << 20U;
    constexpr u64 locked_by = 21000U * kMs;
    LeoLink link;
    link.period_ns = 2000U * kMs;
    link.measure_from_ns = locked_by;
    link.after_ns = 1500U * kMs;
    CongestionPolicy policy;
    policy.algorithm = CongestionAlgorithm::Bbr;
    const LeoRun blind = leo_transfer(policy, total, link, Aware::None);
    const LeoRun aware = leo_transfer(policy, total, link, Aware::True);
    const LeoRun learned = leo_transfer(policy, total, link, Aware::Learned);
    // The link's period 1000 ppm longer than the learner is told: 2 ms a period, 36 ms over the run.
    LeoLink skewed = link;
    skewed.period_ns = 2002U * kMs;
    skewed.learn_period_ns = 2000U * kMs;
    const LeoRun drifting = leo_transfer(policy, total, skewed, Aware::Learned);
    INFO("blind: " << blind);
    INFO("aware: " << aware);
    INFO("learned: " << learned);
    INFO("skewed: " << drifting);
    REQUIRE(learned.measured >= 6U);
    CHECK(blind.locks == 0U);
    CHECK(aware.after_reductions < blind.after_reductions);
    for (const LeoRun* run : {&learned, &drifting})
    {
        CHECK(run->locks == 1U);
        CHECK(run->lock_ns <= locked_by);
        CHECK(run->learned);
        CHECK(run->phase_offset_ns >= -15 * static_cast<i64>(kMs));
        CHECK(run->phase_offset_ns <= 15 * static_cast<i64>(kMs));
        CHECK(run->handover_losses >= 40U);
        // Nearer the authored schedule's reductions than the blind ones (measured: 13 and 16 against 10 and 28).
        CHECK(2U * run->after_reductions <= aware.after_reductions + blind.after_reductions);
    }
}

TEST_CASE("QUIC handover: a congested path that never hands over teaches no schedule, and a learned one is released "
          "once the handovers stop",
          "[net][net-path][quic-connection][handover]")
{
    // CUBIC and BBR filling a one-BDP drop-tail queue: their losses fall at no fixed phase of a 1 s or 2 s period.
    LeoLink quiet;
    quiet.outage_ns = 0U;
    quiet.spike_ns = 0U;
    quiet.guard_before_ns = 60U * kMs; // within an eighth of 1 s
    quiet.guard_after_ns = 60U * kMs;
    for (const CongestionAlgorithm algorithm : {CongestionAlgorithm::Cubic, CongestionAlgorithm::Bbr})
    {
        for (const u64 period : {1000U * kMs, 2000U * kMs})
        {
            INFO("algorithm " << static_cast<u32>(algorithm) << ", period " << period / kMs);
            CongestionPolicy policy;
            policy.algorithm = algorithm;
            quiet.period_ns = period;
            quiet.after_ns = period / 2U;
            const LeoRun run = leo_transfer(policy, 24U << 20U, quiet, Aware::Learned);
            INFO(run);
            CHECK(run.state.reductions != 0U);
            CHECK(run.locks == 0U);
            CHECK_FALSE(run.learned);
        }
    }
    // A handover every second from 3 s until 20 s. In 80 runs (seeds 0 to 3, first handovers 3.0 to 3.9 s, before
    // and after the packet's single entropy draw) the lock came within eleven handovers, by 14 s, and went 12 to 26
    // quiet periods after the last: the 16-period history holds it eleven, the queue's own disturbances in its
    // window the rest.
    LeoLink stopping;
    stopping.period_ns = 1000U * kMs;
    stopping.guard_before_ns = 60U * kMs;
    stopping.guard_after_ns = 60U * kMs;
    stopping.after_ns = 500U * kMs;
    stopping.stop_ns = 20000U * kMs;
    CongestionPolicy cubic;
    const LeoRun run = leo_transfer(cubic, 64U << 20U, stopping, Aware::Learned);
    INFO(run);
    REQUIRE(run.elapsed_ns >= 50000U * kMs);
    CHECK(run.locks == 1U);
    CHECK(run.lock_ns <= 15000U * kMs);
    CHECK(run.handover_losses >= 100U);
    CHECK(run.release_ns >= stopping.stop_ns + 11U * stopping.period_ns);
    CHECK_FALSE(run.learned);
    // Handovers that only lose packets, to the end: the lock holds on the losses its own schedule covers, which the
    // learner still sees. CUBIC fills the 40 ms queue, so the packets the outage takes left up to that much before
    // the event, and the learned phase is theirs (coverage is by send time): at most 40 ms early, never late.
    LeoLink losing = stopping;
    losing.spike_ns = 0U;
    losing.stop_ns = kNeverNs;
    const LeoRun held = leo_transfer(cubic, 30U << 20U, losing, Aware::Learned);
    INFO(held);
    REQUIRE(held.elapsed_ns >= 22000U * kMs);
    CHECK(held.locks == 1U);
    CHECK(held.lock_ns <= 13000U * kMs);
    CHECK(held.learned);
    CHECK(held.phase_offset_ns >= -45 * static_cast<i64>(kMs));
    CHECK(held.phase_offset_ns <= 5 * static_cast<i64>(kMs));
}

TEST_CASE("QUIC handover: an outage inside the guards reaches no persistent congestion; one that outlasts them does",
          "[net][net-path][quic-connection][handover]")
{
    // 1.2 s down: the probe timeout's probes are lost over more than three times the persistent congestion duration.
    constexpr u64 total = 8U << 20U;
    LeoLink outage;
    outage.outage_ns = 1200U * kMs;
    outage.spike_ns = 0U;
    outage.guard_after_ns = 1300U * kMs;
    CongestionPolicy policy;
    const LeoRun blind = leo_transfer(policy, total, outage, Aware::None);
    const LeoRun aware = leo_transfer(policy, total, outage, Aware::True);
    LeoLink short_guard = outage;
    short_guard.guard_after_ns = kStarlinkHandoverGuardNs;
    const LeoRun outlasting = leo_transfer(policy, total, short_guard, Aware::True);
    INFO("blind: " << blind);
    INFO("aware: " << aware);
    INFO("outlasting: " << outlasting);
    const u64 minimum = policy.minimum_packets * kQuicDefaultMaxDatagram;
    CHECK(blind.min_window == minimum);
    CHECK(outlasting.min_window == minimum);
    CHECK(aware.min_window >= 20U * minimum);
    CHECK(aware.handover_losses != 0U);
    CHECK(aware.after_bytes > blind.after_bytes);
    CHECK(aware.elapsed_ns < blind.elapsed_ns);
}

TEST_CASE("QUIC handover: a handover's reordered packets acknowledged after their loss undo no genuine reduction",
          "[net][net-path][quic-connection][handover]")
{
    // A genuine loss 100 ms before the handover reduces the window; the handover only delays (30 ms for 150 ms), so the
    // packets sent at the end of the spike arrive after those sent once it ended, are declared lost and then arrive.
    constexpr u64 total = 8U << 20U;
    LeoLink reorder;
    reorder.outage_ns = 0U;
    reorder.genuine_at_ns = kLeoFirst - 100U * kMs;
    reorder.guard_after_ns = 200U * kMs; // the spike's packets, all covered
    CongestionPolicy policy;
    const LeoRun blind = leo_transfer(policy, total, reorder, Aware::None);
    const LeoRun aware = leo_transfer(policy, total, reorder, Aware::True);
    INFO("blind: " << blind);
    INFO("aware: " << aware);
    CHECK(aware.after_undone == 0U);
    CHECK(aware.handover_losses != 0U);
}

TEST_CASE("QUIC handover: no path MTU probe leaves while a handover may take it; the search follows once it passes",
          "[net][net-path][quic-connection][handover]")
{
    Pair pair(CongestionPolicy{}, 5U);
    pair.net->path[0].rate = kLeoRate;
    pair.net->path[0].buffer = kLeoRate / 1000U * 50U;
    HandoverSchedule covering;
    covering.period_ns = kStarlinkHandoverPeriodNs;
    covering.phase_ns = pair.net->now % kStarlinkHandoverPeriodNs;
    covering.after_ns = 1500U * kMs;
    REQUIRE(pair.client.set_handover(covering));
    pair.confirm();
    Bulk bulk{pair.client, pair.server, 8U << 20U};
    const u64 start = pair.net->now;
    REQUIRE(pair.net->run_until([&] { return bulk.step() || pair.net->now >= start + 1000U * kMs; }));
    CHECK(pair.client.counters().path_probes == 0U);
    CHECK(pair.client.path_mtu().size() == kQuicPmtuFloor);
    REQUIRE(pair.net->run_until([&] { return bulk.step() || pair.net->now >= start + 3000U * kMs; }));
    CHECK(pair.client.counters().path_probes != 0U);
    CHECK(pair.client.path_mtu().size() == pair.client.path_mtu().ceiling());
}

TEST_CASE("QUIC handover: schedules out of range are refused, and a move that restarts the congestion state clears "
          "one where a rebinding keeps it",
          "[net][net-path][quic-connection][handover]")
{
    HandoverSchedule bad;
    bad.phase_ns = 1U; // none names nothing else
    {
        Identity identity;
        CountingAllocator allocator;
        QuicConnectionConfig config = bulk_config(QuicSide::Client, identity, 0U);
        config.handover = bad;
        const QuicConnection refused(allocator, config);
        CHECK_FALSE(refused.ready());
        config.handover = starlink_handover_schedule(0U, 0U);
        const QuicConnection accepted(allocator, config);
        CHECK(accepted.ready());
        CHECK(accepted.handover() == config.handover);
        CHECK_FALSE(accepted.handover_learned());
        // An authored schedule and a learning together, or a learning past the share, are refused.
        config.handover_learning = starlink_handover_learning();
        const QuicConnection both(allocator, config);
        CHECK_FALSE(both.ready());
        config.handover = HandoverSchedule{};
        config.handover_learning.after_ns = kStarlinkHandoverPeriodNs / kHandoverMaxShare;
        const QuicConnection wide(allocator, config);
        CHECK_FALSE(wide.ready());
        config.handover_learning = starlink_handover_learning();
        const QuicConnection learning(allocator, config);
        CHECK(learning.ready());
        CHECK(learning.handover_learning() == starlink_handover_learning());
        CHECK(learning.handover() == HandoverSchedule{});
    }
    Pair pair(CongestionPolicy{}, 7U);
    Net& net = *pair.net;
    net.paths = true;
    net.path[0].rate = kLeoRate;
    net.path[0].buffer = kLeoRate / 1000U * 50U;
    pair.confirm();
    const HandoverSchedule schedule = starlink_handover_schedule(net.now, 0U, 20U * kMs);
    CHECK_FALSE(pair.server.set_handover(bad));
    HandoverSchedule wide = schedule;
    wide.after_ns = kStarlinkHandoverPeriodNs / kHandoverMaxShare;
    CHECK_FALSE(pair.server.set_handover(wide));
    CHECK(pair.server.handover() == HandoverSchedule{});
    REQUIRE(pair.server.set_handover(schedule));
    CHECK(pair.server.handover() == schedule);
    Bulk bulk{pair.client, pair.server, 4U << 20U};
    REQUIRE(net.run_until([&] { return bulk.step() || bulk.read >= bulk.total / 4U; }));
    net.client_at.port = 50001U; // a rebinding: the same link
    REQUIRE(net.run_until([&] { return bulk.step() || bulk.read >= bulk.total / 2U; }));
    REQUIRE(pair.server.path().peer == net.client_at);
    CHECK(pair.server.handover() == schedule);
    net.client_at = Endpoint{IpAddress::v4(203, 0, 113, 5), 40000U}; // another network
    REQUIRE(net.run_until(
        [&]
        {
            static_cast<void>(bulk.step());
            return pair.server.recovery().path_floor() != 0U;
        }));
    CHECK(pair.server.path().peer == net.client_at);
    CHECK(pair.server.handover() == HandoverSchedule{});
    REQUIRE(pair.server.set_handover(schedule)); // the application names the new link's, if it has one
    CHECK(pair.server.handover() == schedule);
    // Learning replaces the schedule and an authored one ends learning; a refused learning changes nothing.
    HandoverLearning wide_learning = starlink_handover_learning();
    wide_learning.before_ns = kStarlinkHandoverPeriodNs / kHandoverMaxShare;
    CHECK_FALSE(pair.server.set_handover_learning(wide_learning));
    CHECK(pair.server.handover() == schedule);
    REQUIRE(pair.server.set_handover_learning(starlink_handover_learning()));
    CHECK(pair.server.handover() == HandoverSchedule{});
    CHECK(pair.server.handover_learning() == starlink_handover_learning());
    REQUIRE(pair.server.set_handover(schedule));
    CHECK(pair.server.handover_learning() == HandoverLearning{});
    // A move that restarts the path's state starts the learning over on the new link: nothing learned, still learning.
    REQUIRE(pair.server.set_handover_learning(starlink_handover_learning()));
    const u64 floor = pair.server.recovery().path_floor();
    net.client_at = Endpoint{IpAddress::v4(198, 51, 100, 9), 40000U};
    REQUIRE(net.run_until(
        [&]
        {
            static_cast<void>(bulk.step());
            return pair.server.recovery().path_floor() != floor;
        }));
    CHECK(pair.server.handover_learning() == starlink_handover_learning());
    CHECK(pair.server.handover() == HandoverSchedule{});
    REQUIRE(pair.server.set_handover_learning(HandoverLearning{}));
    CHECK(pair.server.handover_learning() == HandoverLearning{});
    CHECK(pair.server.handover() == HandoverSchedule{});
    REQUIRE(net.run_until([&] { return bulk.step(); }));
}
