// crd-net -- QUIC's path MTU discovery (NET-PATH; docs/design/network-foundation.md#pmtu). The search alone: no probes
// without discovery or above the floor, the ceiling probed first, a search to the exact byte for every link MTU, random
// probe losses that fail no size, an IPv6 path's base probed first and its failure, the raise timer that waits for
// data, a lowered ceiling and stale probes, a spurious loss that proves its size, and black hole detection against
// congestion's look-alikes, and Packet Too Big hints used, ignored and shrinking the path. Then connections over paths
// of a fixed MTU: both ends finding it exactly while a transfer runs, the peer's max_udp_payload_size as the ceiling,
// probe losses that reduce no window, a path whose MTU falls mid-transfer found again below it, loss and reordering on
// the way, DATAGRAM sizes following the path, an IPv6 path's base, no probes without discovery, a reporting hop found
// in one probe, and forged or stale reports.

#include "ingress_fixture.hpp"
#include "quic_connection_fixture.hpp"
#include "quic_stream_fixture.hpp"

#include <crd/net/quic_connection.hpp>
#include <crd/net/quic_pmtu.hpp>

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

constexpr u64 kSecond = 1000U * kMs;

// Runs the search against a path that carries `carries` bytes: each probe is acknowledged or lost by its size.
// Returns the probes sent.
u32 search(QuicPathMtu& pmtu, u32 carries, u64 now = 0U, bool busy = true)
{
    u32 probes = 0U;
    for (u32 guard = 0U; guard < 200U; ++guard)
    {
        const u32 probe = pmtu.probe(now, busy);
        if (probe == 0U)
        {
            break;
        }
        pmtu.on_probe_sent(probe);
        ++probes;
        if (probe <= carries)
        {
            pmtu.on_probe_acked(probe, now);
        }
        else
        {
            pmtu.on_probe_lost(probe, now);
        }
    }
    return probes;
}

QuicPathMtu started(u32 base, u32 ceiling, bool discovery = true)
{
    QuicPathMtu pmtu;
    pmtu.start(base, ceiling, discovery);
    return pmtu;
}

// A loss detection that lost `lost` (the oldest first) of `bytes`, and nothing smaller.
bool detect(QuicPathMtu& pmtu, u64 oldest, u32 lost, u32 bytes = 1500U)
{
    for (u32 index = 0U; index < lost; ++index)
    {
        pmtu.on_lost(oldest + index, bytes);
    }
    return pmtu.end_detection(0U);
}

} // namespace

TEST_CASE("QUIC PMTU: no discovery, or a ceiling at the floor, probes nothing", "[net][net-path][quic-pmtu]")
{
    QuicPathMtu off = started(1400U, 1500U, false);
    CHECK(off.state() == QuicPmtuState::Disabled);
    CHECK(off.size() == 1400U); // the base the configuration asserts
    CHECK(off.probe(0U, true) == 0U);
    QuicPathMtu floor = started(1200U, 1200U);
    CHECK(floor.state() == QuicPmtuState::Disabled);
    CHECK(floor.size() == kQuicPmtuFloor);
    CHECK(floor.probe(0U, true) == 0U);
    // Sizes under the floor count as the floor; a base above the ceiling falls to it.
    QuicPathMtu low = started(100U, 100U);
    CHECK(low.size() == kQuicPmtuFloor);
    QuicPathMtu high = started(1500U, 1300U, false);
    CHECK(high.size() == 1300U);
    // Without probes nothing finds a black hole.
    CHECK_FALSE(detect(off, 0U, 3U));
    CHECK_FALSE(detect(off, 3U, 3U));
    CHECK_FALSE(detect(off, 6U, 3U));
    CHECK(off.size() == 1400U);
}

TEST_CASE("QUIC PMTU: a path that carries the ceiling takes one probe", "[net][net-path][quic-pmtu]")
{
    QuicPathMtu pmtu = started(1200U, 1500U);
    CHECK(pmtu.state() == QuicPmtuState::Searching);
    CHECK(pmtu.size() == kQuicPmtuFloor);
    CHECK(pmtu.probe(0U, false) == 1500U); // the ceiling first, busy or not
    pmtu.on_probe_sent(1500U);
    CHECK(pmtu.probe(0U, true) == 0U); // one at a time
    pmtu.on_probe_acked(1500U, 0U);
    CHECK(pmtu.size() == 1500U);
    CHECK(pmtu.state() == QuicPmtuState::SearchComplete);
    CHECK(pmtu.probe(kQuicPmtuRaiseNs * 2U, true) == 0U); // the ceiling: nothing to raise
    CHECK(pmtu.counters().probes_sent == 1U);
}

TEST_CASE("QUIC PMTU: the search finds every link's size to the byte", "[net][net-path][quic-pmtu]")
{
    // Link MTUs less IPv4's 28 header bytes: the IPv6 minimum, PPPoE over Ethernet, WireGuard, Ethernet, and odd
    // sizes at both ends of the range.
    for (const u32 carries : {1201U, 1232U, 1252U, 1392U, 1420U, 1452U, 1464U, 1472U, 1499U})
    {
        INFO("carries " << carries);
        QuicPathMtu pmtu = started(1200U, 1500U);
        const u32 probes = search(pmtu, carries);
        CHECK(pmtu.size() == carries);
        CHECK(pmtu.state() == QuicPmtuState::SearchComplete);
        // The ceiling, then a halving per size: each failure costs kQuicPmtuMaxProbes probes.
        CHECK(probes <= kQuicPmtuMaxProbes * 10U);
        CHECK(pmtu.counters().probes_lost == probes - pmtu.counters().probes_acked);
    }
    // A wide ceiling and a jumbo path.
    QuicPathMtu jumbo = started(1200U, 9000U);
    CHECK(search(jumbo, 8972U) <= kQuicPmtuMaxProbes * 14U);
    CHECK(jumbo.size() == 8972U);
}

TEST_CASE("QUIC PMTU: a size fails only after three consecutive probe losses", "[net][net-path][quic-pmtu]")
{
    QuicPathMtu pmtu = started(1200U, 1500U);
    for (u32 attempt = 0U; attempt + 1U < kQuicPmtuMaxProbes; ++attempt)
    {
        REQUIRE(pmtu.probe(0U, true) == 1500U); // the same size again
        pmtu.on_probe_sent(1500U);
        pmtu.on_probe_lost(1500U, 0U); // random loss
    }
    REQUIRE(pmtu.probe(0U, true) == 1500U);
    pmtu.on_probe_sent(1500U);
    pmtu.on_probe_acked(1500U, 0U);
    CHECK(pmtu.size() == 1500U);
    CHECK(pmtu.state() == QuicPmtuState::SearchComplete);
    // A third loss fails the size: the search halves the gap.
    QuicPathMtu failing = started(1200U, 1500U);
    for (u32 attempt = 0U; attempt < kQuicPmtuMaxProbes; ++attempt)
    {
        REQUIRE(failing.probe(0U, true) == 1500U);
        failing.on_probe_sent(1500U);
        failing.on_probe_lost(1500U, 0U);
    }
    CHECK(failing.probe(0U, true) == 1200U + (1499U - 1200U + 1U) / 2U);
    CHECK(failing.size() == kQuicPmtuFloor);
}

TEST_CASE("QUIC PMTU: an IPv6 path probes its base first; a base that fails waits for the raise timer",
          "[net][net-path][quic-pmtu]")
{
    QuicPathMtu pmtu = started(kQuicIpv6BaseDatagram, 1500U);
    CHECK(pmtu.state() == QuicPmtuState::Base);
    CHECK(pmtu.size() == kQuicPmtuFloor); // until the base is acknowledged
    CHECK(pmtu.probe(0U, true) == kQuicIpv6BaseDatagram);
    pmtu.on_probe_sent(kQuicIpv6BaseDatagram);
    pmtu.on_probe_acked(kQuicIpv6BaseDatagram, 0U);
    CHECK(pmtu.size() == kQuicIpv6BaseDatagram);
    CHECK(pmtu.state() == QuicPmtuState::Searching);
    CHECK(pmtu.probe(0U, true) == 1500U);
    search(pmtu, 1452U);
    CHECK(pmtu.size() == 1452U);

    // A broken tunnel that carries less than IPv6's floor: Error at QUIC's floor.
    QuicPathMtu broken = started(kQuicIpv6BaseDatagram, 1500U);
    search(broken, 1210U, 5U * kSecond);
    CHECK(broken.state() == QuicPmtuState::Error);
    CHECK(broken.size() == kQuicPmtuFloor);
    CHECK(broken.counters().probes_lost == kQuicPmtuMaxProbes);
    const u64 raise = 5U * kSecond + kQuicPmtuRaiseNs;
    CHECK(broken.probe(raise - 1U, true) == 0U);
    CHECK(broken.probe(raise, false) == 0U); // an idle connection is not kept alive by probes
    CHECK(broken.state() == QuicPmtuState::Error);
    CHECK(broken.probe(raise, true) == kQuicIpv6BaseDatagram);
    CHECK(broken.state() == QuicPmtuState::Base);
    // The tunnel was repaired: the base, then the search.
    search(broken, 1400U, raise);
    CHECK(broken.size() == 1400U);
    CHECK(broken.state() == QuicPmtuState::SearchComplete);
}

TEST_CASE("QUIC PMTU: a finished search below the ceiling probes again at its raise timer, while busy",
          "[net][net-path][quic-pmtu]")
{
    QuicPathMtu pmtu = started(1200U, 1500U);
    search(pmtu, 1400U, kSecond);
    REQUIRE(pmtu.size() == 1400U);
    const u64 raise = kSecond + kQuicPmtuRaiseNs;
    CHECK(pmtu.probe(raise - 1U, true) == 0U);
    CHECK(pmtu.probe(raise, false) == 0U);
    CHECK(pmtu.probe(raise, true) == 1500U); // the ceiling again: the path may have grown
    CHECK(pmtu.state() == QuicPmtuState::Searching);
    CHECK(pmtu.size() == 1400U); // the size found holds while the search runs
    search(pmtu, 1480U, raise);
    CHECK(pmtu.size() == 1480U);
    // Unchanged: the search confirms the size and ends again.
    QuicPathMtu same = started(1200U, 1500U);
    search(same, 1300U);
    search(same, 1300U, kQuicPmtuRaiseNs);
    CHECK(same.size() == 1300U);
    CHECK(same.state() == QuicPmtuState::SearchComplete);
}

TEST_CASE("QUIC PMTU: the peer's ceiling lowers every bound, and stale probes settle nothing",
          "[net][net-path][quic-pmtu]")
{
    QuicPathMtu pmtu = started(1200U, 1500U);
    REQUIRE(pmtu.probe(0U, true) == 1500U);
    pmtu.on_probe_sent(1500U);
    pmtu.limit(1350U); // the peer's max_udp_payload_size arrives with the probe in flight
    CHECK(pmtu.ceiling() == 1350U);
    CHECK(pmtu.probe(0U, true) == 0U); // the stale probe is still in flight
    pmtu.on_probe_acked(1500U, 0U);
    CHECK(pmtu.size() == kQuicPmtuFloor); // over the ceiling: proves nothing usable
    CHECK(pmtu.probe(0U, true) == 1350U);
    pmtu.on_probe_sent(1350U);
    pmtu.on_probe_acked(1350U, 0U);
    CHECK(pmtu.size() == 1350U);
    CHECK(pmtu.state() == QuicPmtuState::SearchComplete);
    pmtu.limit(1400U); // raising is no limit
    CHECK(pmtu.ceiling() == 1350U);
    pmtu.limit(1300U);
    CHECK(pmtu.size() == 1300U);
    pmtu.limit(1200U);
    CHECK(pmtu.state() == QuicPmtuState::Disabled);
    CHECK(pmtu.size() == kQuicPmtuFloor);

    // A lost probe over a lowered ceiling counts no failure toward the next size.
    QuicPathMtu lost = started(1200U, 1500U);
    REQUIRE(lost.probe(0U, true) == 1500U);
    lost.on_probe_sent(1500U);
    lost.limit(1400U);
    lost.on_probe_lost(1500U, 0U);
    CHECK(lost.probe(0U, true) == 1400U);
    lost.on_probe_sent(1400U);
    lost.on_probe_acked(1400U, 0U);
    CHECK(lost.size() == 1400U);

    // The base above the peer's ceiling falls to it; at the floor the search starts there.
    QuicPathMtu v6 = started(kQuicIpv6BaseDatagram, 1500U);
    v6.limit(1220U);
    CHECK(v6.state() == QuicPmtuState::Base);
    CHECK(v6.probe(0U, true) == 1220U);
    QuicPathMtu v6_floor = started(kQuicIpv6BaseDatagram, 1500U);
    v6_floor.limit(1200U);
    CHECK(v6_floor.state() == QuicPmtuState::Disabled);

    // A failed base the ceiling lowers to a size a late probe proved fails no more: the raise probes nothing below it.
    QuicPathMtu failed = started(1400U, 1500U);
    for (u32 attempt = 0U; attempt < kQuicPmtuMaxProbes; ++attempt)
    {
        REQUIRE(failed.probe(0U, true) == 1400U);
        failed.on_probe_sent(1400U);
        failed.on_probe_lost(1400U, 0U);
    }
    REQUIRE(failed.state() == QuicPmtuState::Error);
    failed.on_probe_acked(1300U, 0U); // a probe of an earlier search, late
    CHECK(failed.size() == 1300U);
    failed.limit(1300U);
    CHECK(failed.state() == QuicPmtuState::SearchComplete);
    CHECK(failed.probe(kQuicPmtuRaiseNs, true) == 0U); // fuzz_quic_pmtu.cpp found it probing the size it had
}

TEST_CASE("QUIC PMTU: a probe declared lost and then acknowledged proves its size", "[net][net-path][quic-pmtu]")
{
    QuicPathMtu pmtu = started(1200U, 1500U);
    for (u32 attempt = 0U; attempt < kQuicPmtuMaxProbes; ++attempt)
    {
        REQUIRE(pmtu.probe(0U, true) == 1500U);
        pmtu.on_probe_sent(1500U);
        pmtu.on_probe_lost(1500U, 0U); // a reordering path declares them lost
    }
    REQUIRE(pmtu.probe(0U, true) == 1350U);
    pmtu.on_probe_sent(1350U);
    pmtu.on_probe_acked(1500U, 0U); // the third 1500-byte probe arrives after all (a spurious loss)
    CHECK(pmtu.size() == 1500U);
    pmtu.on_probe_acked(1350U, 0U);
    CHECK(pmtu.size() == 1500U); // a smaller proof lowers nothing
    CHECK(pmtu.state() == QuicPmtuState::SearchComplete);
}

TEST_CASE("QUIC PMTU: black holes are found, and congestion's losses are not taken for one",
          "[net][net-path][quic-pmtu]")
{
    QuicPathMtu pmtu = started(1200U, 1500U);
    search(pmtu, 1500U);
    REQUIRE(pmtu.size() == 1500U);
    pmtu.on_acked(9U, 1500U);
    // Three detections losing only large packets sent after the newest large one acknowledged: a black hole.
    CHECK_FALSE(detect(pmtu, 10U, 4U));
    CHECK_FALSE(detect(pmtu, 14U, 2U));
    CHECK(detect(pmtu, 16U, 1U));
    CHECK(pmtu.size() == kQuicPmtuFloor);
    CHECK(pmtu.counters().black_holes == 1U);
    CHECK(pmtu.state() == QuicPmtuState::Searching);
    CHECK(pmtu.probe(0U, true) == 1200U + (1499U - 1200U + 1U) / 2U); // below the size that failed
    search(pmtu, 1300U);
    CHECK(pmtu.size() == 1300U);

    // Congestion: each detection's acknowledgement of a later large packet clears the suspicion.
    QuicPathMtu congested = started(1200U, 1500U);
    search(congested, 1500U);
    u64 next = 0U;
    for (u32 round = 0U; round < 10U; ++round)
    {
        congested.on_lost(next, 1500U);
        congested.on_lost(next + 1U, 1500U);
        congested.on_acked(next + 2U, 1500U); // the same ACK frame acknowledges what followed
        CHECK_FALSE(congested.end_detection(0U));
        next += 3U;
    }
    // Losses that a later large acknowledgement explains after the detection.
    CHECK_FALSE(detect(congested, next, 2U));
    CHECK_FALSE(detect(congested, next + 2U, 2U));
    congested.on_acked(next + 4U, 1500U);
    CHECK_FALSE(detect(congested, next + 5U, 2U));
    CHECK_FALSE(detect(congested, next + 7U, 2U));
    // A detection that lost a small packet too, or packets older than the newest large one acknowledged, is none.
    congested.on_lost(next + 9U, 1500U);
    congested.on_lost(next + 10U, 1100U);
    CHECK_FALSE(congested.end_detection(0U));
    CHECK_FALSE(detect(congested, next, 1U));
    CHECK_FALSE(detect(congested, next + 1U, 1U));
    CHECK(congested.size() == 1500U);
    CHECK(congested.counters().black_holes == 0U);
    // Small packets acknowledged clear nothing; detections that lost nothing count nothing.
    congested.on_acked(next + 40U, 1200U);
    CHECK_FALSE(congested.end_detection(0U));
    CHECK(detect(congested, next + 20U, 1U));
    CHECK(congested.counters().black_holes == 1U);

    // At the floor there is nothing to fall back from.
    QuicPathMtu floor = started(1200U, 1500U);
    CHECK_FALSE(detect(floor, 0U, 1U));
    CHECK_FALSE(detect(floor, 1U, 1U));
    CHECK_FALSE(detect(floor, 2U, 1U));

    // An IPv6 path falls to QUIC's floor and probes its base again; a probe in flight then settles as a stale one.
    QuicPathMtu v6 = started(kQuicIpv6BaseDatagram, 1500U);
    search(v6, 1452U);
    REQUIRE(v6.size() == 1452U);
    v6.limit(1500U);
    REQUIRE(v6.probe(kQuicPmtuRaiseNs, true) == 1500U);
    v6.on_probe_sent(1500U);
    CHECK_FALSE(detect(v6, 0U, 1U, 1452U));
    CHECK_FALSE(detect(v6, 1U, 1U, 1452U));
    CHECK(detect(v6, 2U, 1U, 1452U));
    CHECK(v6.state() == QuicPmtuState::Base);
    CHECK(v6.size() == kQuicPmtuFloor);
    v6.on_probe_lost(1500U, 0U); // the stale probe
    CHECK(v6.probe(0U, true) == kQuicIpv6BaseDatagram);
    search(v6, 1280U);
    CHECK(v6.size() == 1280U);
}

TEST_CASE("QUIC PMTU: a Packet Too Big hint fails a probe at once and the next probe tries its limit",
          "[net][net-path][quic-pmtu]")
{
    QuicPathMtu pmtu = started(1200U, 1500U);
    REQUIRE(pmtu.probe(0U, true) == 1500U);
    pmtu.on_probe_sent(1500U);
    CHECK(pmtu.on_too_big(1500U, 1400U, 0U));
    CHECK(pmtu.size() == kQuicPmtuFloor); // a hint raises nothing
    CHECK(pmtu.probe(0U, true) == 1400U); // without waiting for the probe's loss
    pmtu.on_probe_sent(1400U);
    pmtu.on_probe_lost(1500U, 0U); // the probe the hint answered settles as a stale one
    CHECK(pmtu.probe(0U, true) == 0U);
    pmtu.on_probe_acked(1400U, 0U);
    CHECK(pmtu.size() == 1400U);
    CHECK(pmtu.state() == QuicPmtuState::SearchComplete);
    CHECK(pmtu.counters().hints == 1U);
    CHECK(pmtu.counters().probes_sent == 2U);

    // A limit the next probe finds wrong (a hop that lies low, or a route that changed): the search goes on below it.
    QuicPathMtu low = started(1200U, 1500U);
    REQUIRE(low.probe(0U, true) == 1500U);
    low.on_probe_sent(1500U);
    REQUIRE(low.on_too_big(1500U, 1400U, 0U));
    CHECK(search(low, 1300U) > 1U);
    CHECK(low.size() == 1300U);
}

TEST_CASE("QUIC PMTU: Packet Too Big hints inconsistent with the search are ignored", "[net][net-path][quic-pmtu]")
{
    QuicPathMtu off = started(1200U, 1500U, false);
    CHECK_FALSE(off.on_too_big(1500U, 1300U, 0U)); // no discovery
    QuicPathMtu v6 = started(kQuicIpv6BaseDatagram, 1500U);
    CHECK_FALSE(v6.on_too_big(1500U, kQuicIpv6BaseDatagram - 1U, 0U)); // under the base
    QuicPathMtu pmtu = started(1200U, 1500U);
    CHECK_FALSE(pmtu.on_too_big(1500U, kQuicPmtuFloor - 1U, 0U)); // under QUIC's floor (RFC 9000 section 14.2.1)
    CHECK_FALSE(pmtu.on_too_big(1400U, 1400U, 0U));               // the datagram fit
    CHECK_FALSE(pmtu.on_too_big(1300U, 1400U, 0U));               // larger than the datagram
    CHECK(pmtu.counters().hints == 0U);
    CHECK(pmtu.counters().hints_ignored == 3U);
    CHECK(pmtu.probe(0U, true) == 1500U); // the search as before
    CHECK(off.counters().hints_ignored == 1U);
    CHECK(v6.counters().hints_ignored == 1U);

    // A stale datagram above the size found, with a limit at or above that size, changes nothing.
    QuicPathMtu found = started(1200U, 1500U);
    REQUIRE(search(found, 1400U) != 0U);
    CHECK(found.on_too_big(1500U, 1450U, 0U));
    CHECK(found.size() == 1400U);
    CHECK(found.state() == QuicPmtuState::SearchComplete);
    CHECK(found.probe(0U, true) == 0U);
}

TEST_CASE("QUIC PMTU: a Packet Too Big hint under the size in use falls to the floor and searches up to it",
          "[net][net-path][quic-pmtu]")
{
    for (const u32 base : {kQuicPmtuFloor, kQuicIpv6BaseDatagram})
    {
        INFO("base " << base);
        QuicPathMtu pmtu = started(base, 1500U);
        REQUIRE(search(pmtu, 1500U) != 0U);
        REQUIRE(pmtu.size() == 1500U);
        CHECK(pmtu.on_too_big(1500U, 1380U, kSecond));
        CHECK(pmtu.size() == kQuicPmtuFloor);
        CHECK(pmtu.counters().black_holes == 0U); // no loss had to find it
        if (base > kQuicPmtuFloor)
        {
            CHECK(pmtu.state() == QuicPmtuState::Base);
            REQUIRE(pmtu.probe(kSecond, true) == base);
            pmtu.on_probe_sent(base);
            pmtu.on_probe_acked(base, kSecond);
        }
        CHECK(pmtu.probe(kSecond, true) == 1380U); // the limit first
        pmtu.on_probe_sent(1380U);
        pmtu.on_probe_acked(1380U, kSecond);
        CHECK(pmtu.size() == 1380U);
        CHECK(pmtu.state() == QuicPmtuState::SearchComplete);
        // The raise timer searches from the ceiling again: the hint bounds this search alone.
        CHECK(pmtu.probe(kSecond + kQuicPmtuRaiseNs, true) == 1500U);
    }
}

// ---- Connections over paths of a fixed MTU ----

namespace
{
constexpr u8 kClientCid[8] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7};
constexpr u8 kInitialDcid[8] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7};
constexpr u64 kStream = 0U;
constexpr u32 kCeiling = static_cast<u32>(kDatagram);

QuicConnectionConfig path_config(QuicSide side, const Identity& identity, usize seed)
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
    config.datagrams.max_frame_bytes = kCeiling;
    config.datagrams.receive_bytes = 1U << 16U;
    config.datagrams.receive_count = 64U;
    config.datagrams.send_bytes = 1U << 16U;
    config.datagrams.send_count = 64U;
    config.sent_packets = 1024U;
    config.sent_frames = 4096U;
    config.max_datagram_bytes = kCeiling;
    return config;
}

// The client writes `total` bytes of the pattern on `stream` (one it opens); the server reads them.
struct Bulk
{
    QuicConnection& client;
    QuicConnection& server;
    u64 total = 0U;
    u64 written = 0U;
    u64 read = 0U;
    u64 stream = kStream;

    bool step()
    {
        if (written < total)
        {
            static_cast<void>(write_pattern(client.streams(), stream, written, total, stream, false));
        }
        QuicStreamEvent event;
        while (server.streams().poll(event))
        {
        }
        while (client.streams().poll(event))
        {
        }
        if (server.streams().recv_state(stream) != QuicRecvState::None)
        {
            static_cast<void>(drain(server.streams(), stream, read, stream));
        }
        return read >= total;
    }
};

// A client and a server of path_config() that `edit(side, config)` changes first, over a path of `mtu` both ways.
struct Pair
{
    Identity identity;
    CountingAllocator allocator;
    std::unique_ptr<QuicConnection> ends[2];
    QuicConnection& client;
    QuicConnection& server;
    std::unique_ptr<Net> net;

    template <typename Edit>
    Pair(usize mtu, Edit edit, usize seed = 0U)
        : ends{build(QuicSide::Client, edit, seed), build(QuicSide::Server, edit, seed + 1U)}, client(*ends[0]),
          server(*ends[1])
    {
        REQUIRE(client.ready());
        REQUIRE(server.ready());
        REQUIRE(client.connect(ConstSpan<u8>(kClientCid, sizeof(kClientCid)),
                               ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), kStartNs));
        net = std::make_unique<Net>(client, server);
        for (Path& path : net->path)
        {
            path.delay_ns = 20U * kMs;
            path.mtu = mtu;
        }
    }

    explicit Pair(usize mtu, usize seed = 0U) : Pair(mtu, [](QuicSide, QuicConnectionConfig&) {}, seed) {}

    template <typename Edit> std::unique_ptr<QuicConnection> build(QuicSide side, Edit& edit, usize seed)
    {
        QuicConnectionConfig config = path_config(side, identity, seed);
        edit(side, config);
        return std::make_unique<QuicConnection>(allocator, config);
    }

    void confirm()
    {
        REQUIRE(net->run_until([&] { return client.confirmed() && server.confirmed(); }));
        u64 id = 0U;
        REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
        REQUIRE(id == kStream);
    }

    [[nodiscard]] bool searched() const
    {
        return client.path_mtu().state() == QuicPmtuState::SearchComplete &&
               server.path_mtu().state() == QuicPmtuState::SearchComplete;
    }
};

} // namespace

TEST_CASE("QUIC PMTU: both ends find the path's MTU exactly while a transfer runs", "[net][net-path][quic-pmtu]")
{
    for (const usize mtu : {1232U, 1280U, 1452U, 1472U, 1500U})
    {
        INFO("path " << mtu);
        Pair pair(mtu);
        pair.confirm();
        // The handshake keeps to the floor: nothing over 1200 bytes before confirmation.
        CHECK(pair.net->largest[0] == kQuicPmtuFloor);
        Bulk bulk{pair.client, pair.server, 1U << 19U};
        REQUIRE(pair.net->run_until([&] { return bulk.step() && pair.searched(); }));
        CHECK(pair.client.path_mtu().size() == mtu);
        CHECK(pair.server.path_mtu().size() == mtu);
        CHECK(bulk.read == bulk.total);
        // Probe losses reduced no window (RFC 9000 section 14.4), and nothing else was lost.
        CHECK(pair.client.congestion().state().reductions == 0U);
        CHECK(pair.server.congestion().state().reductions == 0U);
        CHECK(pair.client.counters().path_probes == pair.client.path_mtu().counters().probes_sent);
        const u64 failed_probes = pair.client.path_mtu().counters().probes_lost;
        CHECK(pair.net->dropped[0] == failed_probes);
        // Data took the size found: the window's unit grew with it.
        CHECK(pair.client.congestion().window() > 10U * mtu);
    }
}

TEST_CASE("QUIC PMTU: the peer's max_udp_payload_size is the ceiling", "[net][net-path][quic-pmtu]")
{
    Pair pair(1500U,
              [](QuicSide side, QuicConnectionConfig& config)
              {
                  if (side == QuicSide::Server)
                  {
                      config.parameters.max_udp_payload_size = 1350U;
                  }
                  else
                  {
                      config.max_datagram_bytes = 1400U; // and this side's own ceiling the other way
                  }
              });
    pair.confirm();
    Bulk bulk{pair.client, pair.server, 1U << 18U};
    REQUIRE(pair.net->run_until([&] { return bulk.step() && pair.searched(); }));
    CHECK(pair.client.path_mtu().size() == 1350U);
    CHECK(pair.server.path_mtu().size() == kCeiling);
    CHECK(pair.net->largest[0] == 1350U);
    CHECK(pair.client.datagrams().max_payload() == 1350U - kQuicShortPacketOverhead - 3U);
}

TEST_CASE("QUIC PMTU: a path whose MTU falls mid-transfer is a black hole found and searched below",
          "[net][net-path][quic-pmtu]")
{
    for (const CongestionAlgorithm algorithm : {CongestionAlgorithm::Cubic, CongestionAlgorithm::NewReno})
    {
        INFO("algorithm " << static_cast<int>(algorithm));
        // Flow control holds the window the path's queue holds (the Net's flights).
        Pair pair(1500U,
                  [&](QuicSide, QuicConnectionConfig& config)
                  {
                      config.congestion.algorithm = algorithm;
                      config.streams.stream_window = 1U << 18U;
                      config.streams.connection_window = 1U << 18U;
                      config.streams.connection_max_window = 1U << 18U;
                  });
        pair.confirm();
        Bulk bulk{pair.client, pair.server, 1U << 19U};
        REQUIRE(pair.net->run_until([&] { return bulk.step() && pair.searched(); }));
        REQUIRE(pair.client.path_mtu().size() == 1500U);
        bulk.total += 1U << 20U;
        // A route change onto a tunnel: the path now carries 1380 bytes, and drops larger datagrams unreported.
        const u64 midway = bulk.read + (1U << 18U);
        REQUIRE(pair.net->run_until([&] { return bulk.read >= midway || bulk.step(); }));
        for (Path& path : pair.net->path)
        {
            path.mtu = 1380U;
        }
        REQUIRE(pair.net->run_until([&] { return bulk.step() && pair.searched(); }, 600U * kSecond));
        CHECK(pair.client.path_mtu().counters().black_holes == 1U);
        CHECK(pair.client.path_mtu().size() == 1380U);
        // The server sends only acknowledgements, which the path still carries: nothing of its own was lost.
        CHECK(pair.server.path_mtu().counters().black_holes == 0U);
        CHECK(bulk.read == bulk.total);
        CHECK(pair.client.state() == QuicConnectionState::Established);
    }
}

TEST_CASE("QUIC PMTU: a path that falls toward one end of a two-way transfer is a black hole that end finds",
          "[net][net-path][quic-pmtu]")
{
    // The server's acknowledgements ride in its full data packets, which the fallen path drops with the data: the
    // client then hears nothing either, and only the server's own black hole detection ends the silence.
    Pair pair(1500U,
              [&](QuicSide, QuicConnectionConfig& config)
              {
                  config.streams.stream_window = 1U << 18U;
                  config.streams.connection_window = 1U << 18U;
                  config.streams.connection_max_window = 1U << 18U;
              });
    pair.confirm();
    u64 id = 0U;
    REQUIRE(pair.server.streams().open(true, id) == QuicStreamError::None);
    Bulk up{pair.client, pair.server, 1U << 19U};
    Bulk down{pair.server, pair.client, 1U << 19U, 0U, 0U, id};
    const auto both = [&]
    {
        const bool sent = up.step();
        return down.step() && sent;
    };
    REQUIRE(pair.net->run_until([&] { return both() && pair.searched(); }));
    up.total += 1U << 20U;
    down.total += 1U << 20U;
    const u64 midway = down.read + (1U << 18U);
    REQUIRE(pair.net->run_until(
        [&]
        {
            static_cast<void>(both());
            return down.read >= midway;
        }));
    pair.net->path[1].mtu = 1300U; // toward the client alone
    REQUIRE(pair.net->run_until([&] { return both() && pair.searched(); }, 600U * kSecond));
    CHECK(pair.server.path_mtu().counters().black_holes == 1U);
    CHECK(pair.server.path_mtu().size() == 1300U);
    CHECK(pair.client.path_mtu().counters().black_holes == 0U);
    CHECK(pair.client.path_mtu().size() == 1500U);
    CHECK(up.read == up.total);
    CHECK(down.read == down.total);
    CHECK(pair.server.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC PMTU: the search holds over loss, duplication and reordering", "[net][net-path][quic-pmtu]")
{
    for (usize seed = 0U; seed < 4U; ++seed)
    {
        INFO("seed " << seed);
        Pair pair(1420U, seed);
        pair.net->rng.state = seed + 11U;
        for (Path& path : pair.net->path)
        {
            path.loss = 20U;
            path.duplicate = 10U;
            path.jitter_ns = 10U * kMs;
        }
        pair.confirm();
        Bulk bulk{pair.client, pair.server, 1U << 19U};
        REQUIRE(pair.net->run_until([&] { return bulk.step() && pair.searched(); }, 600U * kSecond));
        CHECK(pair.client.path_mtu().size() == 1420U);
        CHECK(pair.server.path_mtu().size() == 1420U);
        CHECK(pair.client.path_mtu().counters().black_holes == 0U);
        CHECK(bulk.read == bulk.total);
    }
}

TEST_CASE("QUIC PMTU: DATAGRAM frames grow with the path and a fall drops the queued ones that no longer fit",
          "[net][net-path][quic-pmtu][quic-datagram]")
{
    Pair pair(1500U);
    pair.confirm();
    // Before the search the largest payload is the floor's.
    CHECK(pair.client.datagrams().max_payload() == kQuicPmtuFloor - kQuicShortPacketOverhead - 3U);
    REQUIRE(pair.net->run_until([&] { return pair.searched(); }));
    const usize grown = pair.client.datagrams().max_payload();
    CHECK(grown == 1500U - kQuicShortPacketOverhead - 3U);
    u8 payload[kDatagram] = {};
    u64 id = 0U;
    REQUIRE(pair.client.datagrams().queue(ConstSpan<u8>(payload, grown), pair.net->now, id) == QuicDatagramError::None);
    REQUIRE(pair.net->run_until([&] { return pair.server.datagrams().held() == 1U; }));
    ConstSpan<u8> held;
    REQUIRE(pair.server.datagrams().peek(held));
    CHECK(held.size() == grown);

    // The path falls to 1300: what is queued at the old size is lost on the path or, once the black hole is found,
    // dropped unsent; DATAGRAM frames then take the new size.
    for (Path& path : pair.net->path)
    {
        path.mtu = 1300U;
    }
    for (usize index = 0U; index < 2U; ++index)
    {
        REQUIRE(pair.client.datagrams().queue(ConstSpan<u8>(payload, grown), pair.net->now, id) ==
                QuicDatagramError::None);
    }
    Bulk bulk{pair.client, pair.server, 1U << 20U};
    REQUIRE(pair.net->run_until([&] { return bulk.step() && pair.searched(); }, 600U * kSecond));
    CHECK(pair.client.path_mtu().counters().black_holes == 1U);
    CHECK(pair.client.path_mtu().size() == 1300U);
    CHECK(pair.client.datagrams().max_payload() == 1300U - kQuicShortPacketOverhead - 3U);
    CHECK(pair.client.datagrams().queued() == 0U);
    const QuicDatagramCounters& counters = pair.client.datagrams().counters();
    CHECK(counters.sent + counters.oversized == 3U);
    CHECK(pair.server.datagrams().held() == 1U); // only the first arrived
}

TEST_CASE("QUIC PMTU: an IPv6 path's base, and no probes without discovery", "[net][net-path][quic-pmtu]")
{
    for (const usize mtu : {1232U, 1210U})
    {
        INFO("path " << mtu);
        // What QuicHost sets toward an IPv6 peer.
        Pair pair(mtu,
                  [](QuicSide, QuicConnectionConfig& config) { config.base_datagram_bytes = kQuicIpv6BaseDatagram; });
        pair.confirm();
        Bulk bulk{pair.client, pair.server, 1U << 18U};
        const auto settled = [&]
        {
            const QuicPmtuState state = pair.client.path_mtu().state();
            return state == QuicPmtuState::SearchComplete || state == QuicPmtuState::Error;
        };
        REQUIRE(pair.net->run_until([&] { return bulk.step() && settled(); }));
        pair.net->advance(pair.net->now + 10U * kSecond);
        if (mtu == kQuicIpv6BaseDatagram)
        {
            CHECK(pair.client.path_mtu().size() == kQuicIpv6BaseDatagram);
            CHECK(pair.client.path_mtu().state() == QuicPmtuState::SearchComplete);
        }
        else
        {
            CHECK(pair.client.path_mtu().size() == kQuicPmtuFloor); // under IPv6's floor: QUIC's
            CHECK(pair.client.path_mtu().state() == QuicPmtuState::Error);
        }
    }
    Pair pair(1500U,
              [](QuicSide side, QuicConnectionConfig& config)
              {
                  config.path_mtu_discovery = false;
                  config.base_datagram_bytes = (side == QuicSide::Server) ? 1300U : kQuicPmtuFloor; // a known path
              });
    pair.confirm();
    Bulk bulk{pair.client, pair.server, 1U << 18U};
    REQUIRE(pair.net->run_until([&] { return bulk.step(); }));
    CHECK(pair.client.counters().path_probes == 0U);
    CHECK(pair.server.counters().path_probes == 0U);
    CHECK(pair.net->largest[0] == kQuicPmtuFloor);
    CHECK(pair.net->largest[1] > kQuicPmtuFloor); // the base the server's configuration knows, from its handshake on
    CHECK(pair.net->largest[1] <= 1300U);
    CHECK(pair.client.path_mtu().state() == QuicPmtuState::Disabled);

    // A base outside 1200 to max_datagram_bytes is refused.
    QuicConnectionConfig over = path_config(QuicSide::Client, pair.identity, 0U);
    over.base_datagram_bytes = kCeiling + 1U;
    CHECK_FALSE(QuicConnection(pair.allocator, over).ready());
    over.base_datagram_bytes = 1199U;
    CHECK_FALSE(QuicConnection(pair.allocator, over).ready());
}

TEST_CASE("QUIC PMTU: a hop's Packet Too Big messages find its MTU in one probe and a fall without a black hole",
          "[net][net-path][quic-pmtu]")
{
    Pair pair(1300U);
    for (Path& path : pair.net->path)
    {
        path.too_big_quote = 64U;
    }
    pair.confirm();
    Bulk bulk{pair.client, pair.server, 1U << 19U};
    REQUIRE(pair.net->run_until([&] { return bulk.step() && pair.searched(); }));
    for (QuicConnection* const end : {&pair.client, &pair.server})
    {
        CHECK(end->path_mtu().size() == 1300U);
        CHECK(end->path_mtu().counters().hints == 1U);
        CHECK(end->path_mtu().counters().probes_sent == 2U); // the ceiling, then the hop's limit
    }
    CHECK(pair.net->too_big_matched[0] == pair.net->too_big[0]);
    bulk.total += 1U << 20U;
    const u64 midway = bulk.read + (1U << 18U);
    REQUIRE(pair.net->run_until([&] { return bulk.read >= midway || bulk.step(); }));
    for (Path& path : pair.net->path)
    {
        path.mtu = 1250U; // a route change the hop reports
    }
    REQUIRE(pair.net->run_until([&] { return bulk.step() && pair.searched(); }, 60U * kSecond));
    CHECK(pair.client.path_mtu().size() == 1250U);
    CHECK(pair.client.path_mtu().counters().black_holes == 0U);
    CHECK(pair.client.path_mtu().counters().hints >= 2U);
    CHECK(pair.net->too_big_matched[0] == pair.net->too_big[0]);
    CHECK(bulk.read == bulk.total);
    CHECK(pair.client.state() == QuicConnectionState::Established);
}

TEST_CASE("QUIC PMTU: Packet Too Big messages that quote no unacknowledged packet change nothing",
          "[net][net-path][quic-pmtu]")
{
    // The client's last large datagram as sent.
    struct Keep final : Filter
    {
        bool drop(usize from, ConstSpan<u8> datagram) override
        {
            if (from == 0U && datagram.size() > kQuicPmtuFloor)
            {
                std::memcpy(last, datagram.data(), datagram.size());
                bytes = datagram.size();
            }
            return false;
        }
        u8 last[kDatagram] = {};
        usize bytes = 0U;
    } keep;
    Pair pair(1500U);
    pair.net->filter = &keep;
    pair.confirm();
    Bulk bulk{pair.client, pair.server, 1U << 18U};
    REQUIRE(pair.net->run_until([&] { return bulk.step() && pair.searched(); }));
    pair.net->advance(pair.net->now + kSecond); // every acknowledgement arrives
    REQUIRE(keep.bytes > kQuicPmtuFloor);
    QuicConnection& client = pair.client;
    const usize id_bytes = client.peer_cid().size();
    REQUIRE(id_bytes != 0U);
    const QuicPmtuCounters before = client.path_mtu().counters();
    const u64 now = pair.net->now;
    const ConstSpan<u8> sent(keep.last, keep.bytes);
    CHECK_FALSE(client.path_too_big(sent.first(48U), 1300U, now)); // acknowledged
    u8 forged[48] = {};
    std::memcpy(forged, keep.last, sizeof(forged));
    forged[1U + id_bytes] ^= 0x01U; // the peer's ID and a guessed packet tag
    CHECK_FALSE(client.path_too_big(ConstSpan<u8>(forged, sizeof(forged)), 1300U, now));
    forged[1U + id_bytes] ^= 0x01U;
    forged[1] ^= 0x80U; // another connection ID
    CHECK_FALSE(client.path_too_big(ConstSpan<u8>(forged, sizeof(forged)), 1300U, now));
    forged[1] ^= 0x80U;
    forged[0] |= 0x80U; // a long header
    CHECK_FALSE(client.path_too_big(ConstSpan<u8>(forged, sizeof(forged)), 1300U, now));
    CHECK_FALSE(client.path_too_big(sent.first(id_bytes + 4U), 1300U, now)); // too short for its tag
    CHECK(client.path_mtu().size() == 1500U);
    CHECK(client.path_mtu().counters().hints == before.hints);
    CHECK(client.path_mtu().counters().hints_ignored == before.hints_ignored);

    // A datagram still in flight is matched: under a limit it fits, inconsistent; under a smaller one, used.
    bulk.total += 1U << 16U;
    static_cast<void>(bulk.step());
    u8 out[kDatagram];
    usize size = 0U;
    for (usize guard = 0U; guard < 8U && size <= kQuicPmtuFloor; ++guard)
    {
        size = client.send(Span<u8>(out, sizeof(out)), now);
    }
    REQUIRE(size > kQuicPmtuFloor);
    CHECK(client.path_too_big(ConstSpan<u8>(out, 64U), static_cast<u32>(size), now));
    CHECK(client.path_mtu().counters().hints_ignored == before.hints_ignored + 1U);
    CHECK(client.path_mtu().size() == 1500U);
    CHECK(client.path_too_big(ConstSpan<u8>(out, 64U), 1300U, now));
    CHECK(client.path_mtu().counters().hints == before.hints + 1U);
    CHECK(client.path_mtu().size() == kQuicPmtuFloor);
}
