// crd-net -- a QUIC connection under congestion control and pacing (NET-PATH; docs/design/network-foundation.md#
// congestion): a bulk transfer whose bytes in flight never pass the window as it grows, a live swap of the controller
// half way that keeps the window, the pacer holding a window with room and its release time a timer that frees it,
// a window that holds data back while ACK-only packets still go, transfers over paths that lose, duplicate and
// reorder with reductions made and the bytes intact, and policies out of range refused.

#include "ingress_fixture.hpp"
#include "quic_connection_fixture.hpp"
#include "quic_stream_fixture.hpp"

#include <crd/net/quic_connection.hpp>

#include <catch2/catch_test_macros.hpp>
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
    std::unique_ptr<Net> net;

    explicit Pair(const CongestionPolicy& policy, usize seed = 0U)
        : client(allocator, with(bulk_config(QuicSide::Client, identity, seed), policy)),
          server(allocator, with(bulk_config(QuicSide::Server, identity, seed + 1U), policy))
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

    static QuicConnectionConfig with(QuicConnectionConfig config, const CongestionPolicy& policy)
    {
        config.congestion = policy;
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

    const usize allocations = pair.allocator.calls();
    REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
    CHECK(pair.allocator.calls() == allocations); // nothing allocated on the way
    CHECK_FALSE(watch.over);
    CHECK(watch.peak > 12000U);
    CHECK(pair.client.congestion().state().reductions == 0U);
    CHECK(pair.client.congestion().window() >= half.window);
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
    const u64 burst = pair.client.congestion().burst();
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

TEST_CASE("QUIC congestion: a full window holds data while ACK-only packets still go", "[net][net-path][quic-connection]")
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
    pair.client.receive(Span<u8>(ping, ping_bytes), QuicEcn::NotEct, now);
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
    for (const CongestionAlgorithm algorithm : {CongestionAlgorithm::Cubic, CongestionAlgorithm::NewReno})
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
            CHECK(state.reductions > 0U);
            CHECK(state.window >= 2U * 1200U);
            CHECK(pair.client.state() == QuicConnectionState::Established);
        }
    }
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
