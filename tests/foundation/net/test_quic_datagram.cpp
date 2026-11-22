// crd-net -- a QUIC connection's unreliable datagrams (4.2a; docs/design/network-foundation.md#quic-datagrams): the
// payload limit at every Length size, the limits and their refusals, max_datagram_frame_size both ways, a received
// frame at and past the size advertised in both encodings, the receive ring full and freed, both rings in order and
// byte-exact across wraps and empty payloads, a datagram that waits for room, expiry, and every outcome reported once
// with send_count holding a datagram until its report is polled. Every frame crosses as bytes.

#include "ingress_fixture.hpp"

#include <crd/containers/wire_bytes.hpp>
#include <crd/net/quic_datagram.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireReader;
using namespace crd::net;
using namespace crd::net::testing;

constexpr u64 kMs = 1000000U;
constexpr usize kUdp = 1200U;
constexpr usize kLimitTop = 70000U; // the payload limit is checked up to past the four-byte Length
constexpr usize kModelIds = 4096U;  // the seeded runs: datagrams followed
constexpr usize kModelHeld = 8U;    // and the receiver's model ring

QuicDatagramLimits limits(u32 send_bytes = 4096U, u32 send_count = 8U, bool outcomes = false)
{
    QuicDatagramLimits l;
    l.max_frame_bytes = 1200U;
    l.receive_bytes = 4096U;
    l.receive_count = 8U;
    l.send_bytes = send_bytes;
    l.send_count = send_count;
    l.outcomes = outcomes;
    return l;
}

u8 pattern(u64 id, usize offset)
{
    return static_cast<u8>((id * 131U) + (offset * 7U) + 1U);
}

void fill(u8* out, usize bytes, u64 id)
{
    for (usize offset = 0U; offset < bytes; ++offset)
    {
        out[offset] = pattern(id, offset);
    }
}

bool holds(ConstSpan<u8> bytes, usize size, u64 id)
{
    if (bytes.size() != size)
    {
        return false;
    }
    for (usize offset = 0U; offset < size; ++offset)
    {
        if (bytes[offset] != pattern(id, offset))
        {
            return false;
        }
    }
    return true;
}

// Reads back the one DATAGRAM frame write_next() wrote.
QuicFrame read_back(const u8* bytes, usize size)
{
    WireReader in(ConstSpan<u8>(bytes, size));
    QuicFrame frame;
    REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
    REQUIRE(frame.kind == QuicFrameKind::Datagram);
    REQUIRE_FALSE(frame.to_end);
    REQUIRE(in.done());
    return frame;
}

// splitmix64.
struct Rng
{
    u64 state = 1U;

    u64 next()
    {
        u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
};
} // namespace

TEST_CASE("QUIC datagrams: the payload limit is the largest payload whose frame fits, at every Length size",
          "[net][4.2a][quic-datagram]")
{
    usize payload = 0U;
    CHECK_FALSE(quic_datagram_payload_limit(0U, payload));
    CHECK_FALSE(quic_datagram_payload_limit(1U, payload));
    // Against the sizes of every payload up to past the four-byte Length: the frame size is monotone in the payload.
    usize best = 0U;
    for (usize frame = 2U; frame <= kLimitTop; ++frame)
    {
        while (best + 1U <= kLimitTop && 1U + quic_varint_bytes(best + 1U) + best + 1U <= frame)
        {
            ++best;
        }
        REQUIRE(quic_datagram_payload_limit(frame, payload));
        REQUIRE(payload == best);
    }
    CHECK(quic_datagram_payload_limit(66U, payload));
    CHECK(payload == 63U); // 64 would need a two-byte Length
    CHECK(quic_datagram_payload_limit(16387U, payload));
    CHECK(payload == 16383U);
    CHECK(quic_datagram_payload_limit(kQuicMaxVarint, payload));
}

TEST_CASE("QUIC datagrams: limits out of range are refused and init takes everything or nothing",
          "[net][4.2a][quic-datagram]")
{
    CHECK(QuicDatagrams::limits_valid(QuicDatagramLimits{}));
    CHECK(QuicDatagrams::limits_valid(limits()));
    CHECK(QuicDatagrams::limits_valid(limits(0U, 0U)));
    for (usize which = 0U; which < 12U; ++which)
    {
        QuicDatagramLimits l = limits();
        switch (which)
        {
            case 0U:
                l.max_frame_bytes = kQuicMaxDatagramFrame + 1U;
                break;
            case 1U:
                l.max_frame_bytes = 1U; // an empty payload without a Length, into a ring of nothing
                l.receive_bytes = 0U;
                break;
            case 2U:
                l.receive_bytes = 1198U; // a frame of 1200 bytes without a Length carries 1199
                break;
            case 3U:
                l.receive_count = 0U;
                break;
            case 4U:
                l.max_frame_bytes = 0U; // a ring without a size advertised
                break;
            case 5U:
                l.receive_bytes = kQuicMaxDatagramRing + 1U;
                break;
            case 6U:
                l.receive_count = kQuicMaxDatagramCount + 1U;
                break;
            case 7U:
                l.send_bytes = kQuicMaxDatagramRing + 1U;
                break;
            case 8U:
                l.send_count = 0U;
                break;
            case 9U:
                l.send_bytes = 0U; // a count without a ring
                break;
            case 10U:
                l.send_count = kQuicMaxDatagramCount + 1U;
                break;
            default:
                l.send_bytes = 0U;
                l.send_count = 0U;
                l.outcomes = true; // outcomes of nothing sent
                break;
        }
        INFO("variant " << which);
        CHECK_FALSE(QuicDatagrams::limits_valid(l));
        CountingAllocator allocator;
        QuicDatagrams datagrams;
        CHECK_FALSE(datagrams.init(allocator, l));
        CHECK(allocator.calls() == 0U);
    }
    QuicDatagramLimits edge = limits();
    edge.receive_bytes = 1199U;
    CHECK(QuicDatagrams::limits_valid(edge));
    edge.max_frame_bytes = 1U;
    edge.receive_bytes = 1U;
    CHECK(QuicDatagrams::limits_valid(edge));
    for (usize refuse = 0U; refuse < 3U; ++refuse)
    {
        CountingAllocator allocator;
        allocator.refuse_after(refuse);
        QuicDatagrams datagrams;
        CHECK_FALSE(datagrams.init(allocator, limits(4096U, 8U, true)));
    }
    CountingAllocator allocator;
    QuicDatagrams datagrams;
    REQUIRE(datagrams.init(allocator, limits(4096U, 8U, true)));
    CHECK(allocator.calls() == 3U);
    CHECK_FALSE(datagrams.init(allocator, limits())); // once
    QuicDatagrams none;
    REQUIRE(none.init(allocator, QuicDatagramLimits{}));
    CHECK(allocator.calls() == 3U); // nothing to hold, nothing taken
}

TEST_CASE("QUIC datagrams: max_datagram_frame_size both ways bounds what is queued and written",
          "[net][4.2a][quic-datagram]")
{
    CountingAllocator allocator;
    QuicDatagrams datagrams;
    REQUIRE(datagrams.init(allocator, limits()));
    QuicTransportParameters local;
    datagrams.fill_parameters(local);
    CHECK(local.max_datagram_frame_size == 1200U);

    u8 payload[kUdp] = {};
    u64 id = 0U;
    CHECK(datagrams.queue(ConstSpan<u8>(payload, 1U), 0U, id) == QuicDatagramError::NotReady);
    datagrams.set_peer(0U, kUdp);
    CHECK_FALSE(datagrams.sendable());
    CHECK(datagrams.queue(ConstSpan<u8>(payload, 1U), 0U, id) == QuicDatagramError::Unsupported);
    // The peer's size rules when it is the smaller.
    datagrams.set_peer(100U, kUdp);
    CHECK(datagrams.max_payload() == 97U); // 98 would need a two-byte Length: 101 bytes
    // One 1-RTT packet of 1200 bytes leaves 1159 for frames with a 20-byte connection ID: 1156 of payload.
    datagrams.set_peer(kQuicMaxDatagramFrame, kUdp);
    REQUIRE(datagrams.sendable());
    CHECK(datagrams.max_payload() == kUdp - kQuicShortPacketOverhead - 3U);
    const usize largest = datagrams.max_payload();
    CHECK(datagrams.queue(ConstSpan<u8>(payload, largest + 1U), 0U, id) == QuicDatagramError::TooLarge);
    fill(payload, largest, 0U);
    REQUIRE(datagrams.queue(ConstSpan<u8>(payload, largest), 0U, id) == QuicDatagramError::None);
    CHECK(id == 0U);
    // It never splits: a room one byte short leaves it first, the packet's room takes it whole.
    u8 frame[kUdp] = {};
    const usize room = kUdp - kQuicShortPacketOverhead;
    CHECK(datagrams.write_next(Span<u8>(frame, room - 1U), 0U, id) == 0U);
    CHECK(datagrams.queued() == 1U);
    CHECK(datagrams.has_frame(0U));
    REQUIRE(datagrams.write_next(Span<u8>(frame, room), 0U, id) == room);
    CHECK(holds(read_back(frame, room).data, largest, 0U));
    CHECK(datagrams.queued() == 0U);
    CHECK_FALSE(datagrams.has_frame(0U));
    CHECK(datagrams.counters().sent == 1U);

    // A ring smaller than the packet bounds the payload, and this side without a ring sends nothing.
    QuicDatagrams small;
    REQUIRE(small.init(allocator, limits(64U, 2U)));
    small.set_peer(kQuicMaxDatagramFrame, kUdp);
    CHECK(small.max_payload() == 64U);
    QuicDatagrams mute;
    REQUIRE(mute.init(allocator, limits(0U, 0U)));
    mute.set_peer(kQuicMaxDatagramFrame, kUdp);
    CHECK(mute.queue(ConstSpan<u8>(payload, 1U), 0U, id) == QuicDatagramError::Unsupported);
    // A path too small for any frame.
    datagrams.set_peer(kQuicMaxDatagramFrame, kQuicShortPacketOverhead + 1U);
    CHECK_FALSE(datagrams.sendable());
}

TEST_CASE("QUIC datagrams: a received frame is held to the size advertised in both encodings",
          "[net][4.2a][quic-datagram]")
{
    CountingAllocator allocator;
    QuicDatagrams none;
    REQUIRE(none.init(allocator, QuicDatagramLimits{}));
    u8 payload[1200] = {};
    QuicFrame frame;
    frame.kind = QuicFrameKind::Datagram;
    frame.data = ConstSpan<u8>(payload, 1U);
    CHECK(none.on_frame(frame) == QuicTransportError::ProtocolViolation); // none advertised

    QuicDatagrams datagrams;
    REQUIRE(datagrams.init(allocator, limits()));
    for (const bool to_end : {false, true})
    {
        INFO("to_end " << to_end);
        const usize at_limit = to_end ? 1199U : 1197U;
        fill(payload, at_limit, at_limit);
        frame.to_end = to_end;
        frame.data = ConstSpan<u8>(payload, at_limit);
        REQUIRE(quic_frame_bytes(frame) == 1200U);
        REQUIRE(datagrams.on_frame(frame) == QuicTransportError::NoError);
        ConstSpan<u8> held;
        REQUIRE(datagrams.peek(held));
        CHECK(holds(held, at_limit, at_limit));
        datagrams.pop();
        CHECK_FALSE(datagrams.peek(held));
        frame.data = ConstSpan<u8>(payload, at_limit + 1U);
        CHECK(datagrams.on_frame(frame) == QuicTransportError::ProtocolViolation);
    }
    CHECK(datagrams.counters().received == 2U);

    // A full ring drops what arrives and counts it; a popped one makes room again.
    frame.to_end = false;
    frame.data = ConstSpan<u8>(payload, 600U);
    for (usize index = 0U; index < 8U; ++index)
    {
        REQUIRE(datagrams.on_frame(frame) == QuicTransportError::NoError);
    }
    CHECK(datagrams.held() == 6U); // 4096 bytes hold six of 600
    CHECK(datagrams.counters().dropped == 2U);
    datagrams.pop();
    REQUIRE(datagrams.on_frame(frame) == QuicTransportError::NoError);
    CHECK(datagrams.held() == 6U);
    CHECK(datagrams.counters().dropped == 2U);
    datagrams.pop();
    datagrams.pop();
    frame.data = {};
    REQUIRE(datagrams.on_frame(frame) == QuicTransportError::NoError); // an empty payload is a datagram
    for (usize index = 0U; index < 4U; ++index)
    {
        datagrams.pop();
    }
    ConstSpan<u8> held;
    REQUIRE(datagrams.peek(held));
    CHECK(held.empty());
    datagrams.pop();
    CHECK_FALSE(datagrams.peek(held));
    datagrams.pop(); // nothing held: nothing happens
    CHECK(datagrams.held() == 0U);
}

TEST_CASE("QUIC datagrams: both rings keep order and every byte across wraps, by bytes and by count",
          "[net][4.2a][quic-datagram]")
{
    SECTION("a wrap skips the ring's tail, and the ring refuses what its free run cannot hold")
    {
        CountingAllocator allocator;
        QuicDatagrams datagrams;
        REQUIRE(datagrams.init(allocator, limits(100U, 8U)));
        datagrams.set_peer(kQuicMaxDatagramFrame, kUdp);
        u8 payload[100];
        u64 id = 0U;
        const auto put = [&](usize bytes)
        {
            fill(payload, bytes, datagrams.queued() + datagrams.counters().sent);
            return datagrams.queue(ConstSpan<u8>(payload, bytes), 0U, id);
        };
        u8 frame[200];
        const auto take = [&](usize bytes, u64 expected)
        {
            const usize written = datagrams.write_next(Span<u8>(frame, sizeof(frame)), 0U, id);
            REQUIRE(written != 0U);
            CHECK(id == expected);
            CHECK(holds(read_back(frame, written).data, bytes, expected));
        };
        REQUIRE(put(60U) == QuicDatagramError::None);
        CHECK(put(50U) == QuicDatagramError::Full); // 40 left
        REQUIRE(put(30U) == QuicDatagramError::None);
        take(60U, 0U);
        REQUIRE(put(50U) == QuicDatagramError::None); // past the end: it starts the next lap at 0
        CHECK(put(20U) == QuicDatagramError::Full);   // 10 free between it and the oldest
        REQUIRE(put(10U) == QuicDatagramError::None);
        take(30U, 1U);
        take(50U, 2U);
        take(10U, 3U);
        REQUIRE(put(100U) == QuicDatagramError::None); // an empty ring holds its whole size
        take(100U, 4U);
        REQUIRE(put(0U) == QuicDatagramError::None);
        const usize written = datagrams.write_next(Span<u8>(frame, sizeof(frame)), 0U, id);
        CHECK(written == 2U); // the type and a zero Length
        CHECK(read_back(frame, written).data.empty());
    }
    SECTION("seeded runs against a model")
    {
        for (u64 seed = 1U; seed <= 16U; ++seed)
        {
            CountingAllocator allocator;
            QuicDatagrams sender;
            QuicDatagrams receiver;
            QuicDatagramLimits l = limits(1500U + (static_cast<u32>(seed) * 37U), 6U);
            l.receive_bytes = 1300U + (static_cast<u32>(seed) * 53U);
            l.receive_count = 5U;
            REQUIRE(sender.init(allocator, l));
            REQUIRE(receiver.init(allocator, l));
            sender.set_peer(1200U, kUdp);
            Rng rng;
            rng.state = seed;
            usize sizes[kModelIds] = {};
            u64 held_ids[kModelHeld] = {}; // the model of what the receiver holds, oldest first
            usize held_first = 0U;
            usize held_count = 0U;
            u64 queued = 0U;
            INFO("seed " << seed);
            for (usize round = 0U; round < 4000U; ++round)
            {
                const u64 choice = rng.next() % 4U;
                if (choice < 2U && queued < kModelIds)
                {
                    const usize bytes = static_cast<usize>(rng.next() % (sender.max_payload() + 1U));
                    u8 payload[1200];
                    fill(payload, bytes, queued);
                    u64 id = 0U;
                    const QuicDatagramError error = sender.queue(ConstSpan<u8>(payload, bytes), 0U, id);
                    REQUIRE((error == QuicDatagramError::None || error == QuicDatagramError::Full));
                    if (error == QuicDatagramError::None)
                    {
                        REQUIRE(id == queued);
                        sizes[queued++] = bytes;
                    }
                }
                else if (choice == 2U)
                {
                    u8 frame[1200];
                    u64 id = 0U;
                    const usize written = sender.write_next(Span<u8>(frame, sizeof(frame)), 0U, id);
                    if (written != 0U)
                    {
                        const QuicFrame read = read_back(frame, written);
                        REQUIRE(holds(read.data, sizes[id], id));
                        const u64 received = receiver.counters().received;
                        REQUIRE(receiver.on_frame(read) == QuicTransportError::NoError);
                        if (receiver.counters().received != received)
                        {
                            REQUIRE(held_count < kModelHeld);
                            held_ids[(held_first + held_count++) % kModelHeld] = id;
                        }
                    }
                }
                else
                {
                    ConstSpan<u8> held;
                    REQUIRE(receiver.peek(held) == (held_count != 0U));
                    if (held_count != 0U)
                    {
                        const u64 id = held_ids[held_first];
                        REQUIRE(holds(held, sizes[id], id));
                        receiver.pop();
                        held_first = (held_first + 1U) % kModelHeld;
                        --held_count;
                    }
                }
                REQUIRE(receiver.held() == held_count);
            }
            CHECK(sender.counters().sent + sender.queued() == queued);
            CHECK(receiver.counters().received + receiver.counters().dropped == sender.counters().sent);
            CHECK(receiver.counters().dropped != 0U); // the small ring was full at times
            CHECK(allocator.calls() == 4U);
        }
    }
}

TEST_CASE("QUIC datagrams: a datagram queued past max_age_ns is dropped unsent", "[net][4.2a][quic-datagram]")
{
    for (const bool outcomes : {false, true})
    {
        INFO("outcomes " << outcomes);
        CountingAllocator allocator;
        QuicDatagrams datagrams;
        QuicDatagramLimits l = limits(4096U, 2U, outcomes);
        l.max_age_ns = 10U * kMs;
        REQUIRE(datagrams.init(allocator, l));
        datagrams.set_peer(1200U, kUdp);
        u8 payload[8] = {};
        u64 first = 0U;
        u64 second = 0U;
        REQUIRE(datagrams.queue(ConstSpan<u8>(payload, 8U), 0U, first) == QuicDatagramError::None);
        REQUIRE(datagrams.queue(ConstSpan<u8>(payload, 8U), 5U * kMs, second) == QuicDatagramError::None);
        CHECK(datagrams.has_frame(10U * kMs));
        CHECK(datagrams.has_frame(15U * kMs)); // the newest is exactly 10 ms old
        CHECK_FALSE(datagrams.has_frame(15U * kMs + 1U));
        u8 frame[64];
        u64 id = 99U;
        REQUIRE(datagrams.write_next(Span<u8>(frame, sizeof(frame)), 12U * kMs, id) != 0U);
        CHECK(id == second); // the first expired
        CHECK(datagrams.counters().expired == 1U);
        QuicDatagramReport report;
        if (outcomes)
        {
            CHECK(datagrams.unreported() == 2U); // one expired, one in flight
            REQUIRE(datagrams.poll(report));
            CHECK(report.id == first);
            CHECK(report.outcome == QuicDatagramOutcome::Expired);
            datagrams.on_acked(second);
            REQUIRE(datagrams.poll(report));
            CHECK(report.outcome == QuicDatagramOutcome::Acked);
        }
        CHECK_FALSE(datagrams.poll(report));
        CHECK(datagrams.unreported() == 0U);
        // Expiry makes room for a new one at queue().
        REQUIRE(datagrams.queue(ConstSpan<u8>(payload, 8U), 20U * kMs, id) == QuicDatagramError::None);
        REQUIRE(datagrams.queue(ConstSpan<u8>(payload, 8U), 20U * kMs, id) == QuicDatagramError::None);
        CHECK(datagrams.queue(ConstSpan<u8>(payload, 8U), 30U * kMs, id) == QuicDatagramError::Full);
        if (outcomes)
        {
            REQUIRE(datagrams.queue(ConstSpan<u8>(payload, 8U), 31U * kMs, id) == QuicDatagramError::Full);
            CHECK(datagrams.unreported() == 2U); // both expired, their reports waiting
            REQUIRE(datagrams.poll(report));
            CHECK(report.outcome == QuicDatagramOutcome::Expired);
        }
        CHECK(datagrams.queue(ConstSpan<u8>(payload, 8U), 31U * kMs, id) == QuicDatagramError::None);
        // A clock earlier than a datagram's queueing never expires it.
        CHECK(datagrams.has_frame(0U));
    }
}

TEST_CASE("QUIC datagrams: every outcome is reported once and send_count holds a datagram until it is polled",
          "[net][4.2a][quic-datagram]")
{
    CountingAllocator allocator;
    QuicDatagrams datagrams;
    REQUIRE(datagrams.init(allocator, limits(4096U, 3U, true)));
    datagrams.set_peer(1200U, kUdp);
    u8 payload[16] = {};
    u64 ids[3] = {};
    for (u64& id : ids)
    {
        REQUIRE(datagrams.queue(ConstSpan<u8>(payload, 16U), 0U, id) == QuicDatagramError::None);
    }
    u64 id = 0U;
    CHECK(datagrams.queue(ConstSpan<u8>(payload, 16U), 0U, id) == QuicDatagramError::Full);
    u8 frame[64];
    REQUIRE(datagrams.write_next(Span<u8>(frame, sizeof(frame)), 0U, id) != 0U);
    REQUIRE(datagrams.write_next(Span<u8>(frame, sizeof(frame)), 0U, id) != 0U);
    CHECK(datagrams.queued() == 1U);
    CHECK(datagrams.queue(ConstSpan<u8>(payload, 16U), 0U, id) == QuicDatagramError::Full); // two in flight
    datagrams.on_acked(ids[0]);
    datagrams.on_lost(ids[1]);
    CHECK(datagrams.queue(ConstSpan<u8>(payload, 16U), 0U, id) == QuicDatagramError::Full); // reports unpolled
    QuicDatagramReport report;
    REQUIRE(datagrams.poll(report));
    CHECK(report.id == ids[0]);
    CHECK(report.outcome == QuicDatagramOutcome::Acked);
    REQUIRE(datagrams.queue(ConstSpan<u8>(payload, 16U), 0U, id) == QuicDatagramError::None);
    CHECK(id == 3U);
    REQUIRE(datagrams.poll(report));
    CHECK(report.id == ids[1]);
    CHECK(report.outcome == QuicDatagramOutcome::Lost);
    CHECK_FALSE(datagrams.poll(report));
    // The report ring wraps with the queue.
    for (u64 round = 0U; round < 20U; ++round)
    {
        while (datagrams.write_next(Span<u8>(frame, sizeof(frame)), 0U, id) != 0U)
        {
            datagrams.on_lost(id);
        }
        while (datagrams.poll(report))
        {
            CHECK(report.outcome == QuicDatagramOutcome::Lost);
        }
        REQUIRE(datagrams.queue(ConstSpan<u8>(payload, 16U), 0U, id) == QuicDatagramError::None);
    }
    CHECK(datagrams.unreported() == 1U);

    // Without outcomes nothing is reported and a datagram is done once written.
    QuicDatagrams plain;
    REQUIRE(plain.init(allocator, limits(4096U, 1U)));
    plain.set_peer(1200U, kUdp);
    REQUIRE(plain.queue(ConstSpan<u8>(payload, 16U), 0U, id) == QuicDatagramError::None);
    REQUIRE(plain.write_next(Span<u8>(frame, sizeof(frame)), 0U, id) != 0U);
    plain.on_acked(id);
    plain.on_lost(id);
    CHECK_FALSE(plain.poll(report));
    CHECK(plain.unreported() == 0U);
    CHECK(plain.queue(ConstSpan<u8>(payload, 16U), 0U, id) == QuicDatagramError::None);
}

TEST_CASE("QUIC datagrams: the payload limit follows the path's size, and a fall drops what no longer fits",
          "[net][net-path][quic-datagram]")
{
    CountingAllocator allocator;
    QuicDatagrams datagrams;
    REQUIRE(datagrams.init(allocator, limits(8192U, 4U, true)));
    datagrams.set_path(1500U); // nothing before the peer's parameters
    CHECK(datagrams.max_payload() == 0U);
    datagrams.set_peer(kQuicMaxDatagramFrame, kUdp);
    const usize floor = datagrams.max_payload();
    CHECK(floor == kUdp - kQuicShortPacketOverhead - 3U);
    datagrams.set_path(1500U);
    const usize grown = datagrams.max_payload();
    CHECK(grown == 1500U - kQuicShortPacketOverhead - 3U);
    u8 payload[1500] = {};
    u64 ids[3] = {};
    REQUIRE(datagrams.queue(ConstSpan<u8>(payload, grown), 0U, ids[0]) == QuicDatagramError::None);
    REQUIRE(datagrams.queue(ConstSpan<u8>(payload, floor), 0U, ids[1]) == QuicDatagramError::None);
    REQUIRE(datagrams.queue(ConstSpan<u8>(payload, grown), 0U, ids[2]) == QuicDatagramError::None);
    // A black hole: the path falls back to the floor. What no longer fits is dropped unsent when it is the oldest.
    datagrams.set_path(kUdp);
    CHECK(datagrams.max_payload() == floor);
    u64 id = 0U;
    CHECK(datagrams.queue(ConstSpan<u8>(payload, grown), 0U, id) == QuicDatagramError::TooLarge);
    CHECK(datagrams.has_frame(0U));
    u8 frame[1500];
    REQUIRE(datagrams.write_next(Span<u8>(frame, sizeof(frame)), 0U, id) != 0U);
    CHECK(id == ids[1]);
    CHECK(datagrams.write_next(Span<u8>(frame, sizeof(frame)), 0U, id) == 0U);
    CHECK(datagrams.queued() == 0U);
    CHECK(datagrams.counters().oversized == 2U);
    QuicDatagramReport report;
    REQUIRE(datagrams.poll(report));
    CHECK(report.id == ids[0]);
    CHECK(report.outcome == QuicDatagramOutcome::Lost);
    REQUIRE(datagrams.poll(report));
    CHECK(report.id == ids[2]);
    CHECK(report.outcome == QuicDatagramOutcome::Lost);
    CHECK_FALSE(datagrams.poll(report));
    // The peer's own frame size still bounds a grown path.
    QuicDatagrams bounded;
    REQUIRE(bounded.init(allocator, limits()));
    bounded.set_peer(1000U, kUdp);
    bounded.set_path(9000U);
    usize limit = 0U;
    REQUIRE(quic_datagram_payload_limit(1000U, limit));
    CHECK(bounded.max_payload() == limit);
}

TEST_CASE("QUIC datagrams: payloads of 0-RTT packets are marked, and a rejected 0-RTT drops the queue as Lost",
          "[net][4.2a][quic-datagram][quic-early]")
{
    for (const bool outcomes : {true, false})
    {
        CountingAllocator allocator;
        QuicDatagrams datagrams;
        REQUIRE(datagrams.init(allocator, limits(4096U, 8U, outcomes)));
        u8 bytes[16];
        fill(bytes, sizeof(bytes), 1U);
        QuicFrame frame;
        frame.kind = QuicFrameKind::Datagram;
        frame.data = ConstSpan<u8>(bytes, sizeof(bytes));
        REQUIRE(datagrams.on_frame(frame, true) == QuicTransportError::NoError);
        REQUIRE(datagrams.on_frame(frame) == QuicTransportError::NoError);
        CHECK(datagrams.oldest_early());
        datagrams.pop();
        CHECK_FALSE(datagrams.oldest_early());
        datagrams.pop();
        CHECK_FALSE(datagrams.oldest_early()); // none held

        datagrams.set_peer(1200U, kUdp);
        u64 ids[3] = {};
        for (u64& id : ids)
        {
            REQUIRE(datagrams.queue(ConstSpan<u8>(bytes, sizeof(bytes)), 0U, id) == QuicDatagramError::None);
        }
        u8 out[kUdp];
        u64 sent = 0U;
        REQUIRE(datagrams.write_next(Span<u8>(out, sizeof(out)), 0U, sent) != 0U); // one in flight
        datagrams.reject_early();
        CHECK(datagrams.queued() == 0U);
        CHECK_FALSE(datagrams.sendable());
        u64 id = 0U;
        CHECK(datagrams.queue(ConstSpan<u8>(bytes, sizeof(bytes)), 0U, id) == QuicDatagramError::NotReady);
        datagrams.on_lost(sent); // its packet's recovery state went
        usize lost = 0U;
        QuicDatagramReport report;
        while (datagrams.poll(report))
        {
            CHECK(report.outcome == QuicDatagramOutcome::Lost);
            ++lost;
        }
        CHECK(lost == (outcomes ? 3U : 0U));
        CHECK(datagrams.unreported() == 0U);
        datagrams.set_peer(1200U, kUdp);
        CHECK(datagrams.queue(ConstSpan<u8>(bytes, sizeof(bytes)), 0U, id) == QuicDatagramError::None);
    }
}

namespace
{
// Path slot `slot` writing while the slots of `copies` carry data.
QuicDatagramPaths on(usize slot, u32 copies, bool copies_only = false)
{
    QuicDatagramPaths paths;
    paths.slot = slot;
    paths.copies = copies;
    paths.copies_only = copies_only;
    return paths;
}

QuicDatagramLimits redundant_limits(u32 send_bytes, u32 send_count, u32 redundant, bool outcomes)
{
    QuicDatagramLimits l = limits(send_bytes, send_count, outcomes);
    l.redundant_count = redundant;
    return l;
}

QuicDatagramSend redundantly()
{
    QuicDatagramSend how;
    how.redundant = true;
    return how;
}

// Queues datagram `id`'s `bytes` of pattern.
QuicDatagramError put(QuicDatagrams& datagrams, u64 id, usize bytes, u64 now_ns, const QuicDatagramSend& how = {})
{
    u8 payload[kUdp];
    fill(payload, bytes, id);
    u64 queued = ~u64{0};
    const QuicDatagramError error = datagrams.queue(ConstSpan<u8>(payload, bytes), now_ns, queued, how);
    if (error == QuicDatagramError::None)
    {
        CHECK(queued == id);
    }
    return error;
}

// Writes the next frame for `paths` into a whole packet, which holds datagram `id`'s `bytes`; returns its ticket.
u64 sends(QuicDatagrams& datagrams, const QuicDatagramPaths& paths, u64 now_ns, u64 id, usize bytes)
{
    REQUIRE(datagrams.has_frame(now_ns, paths));
    u8 frame[kUdp];
    u64 ticket = ~u64{0};
    const usize written = datagrams.write_next(Span<u8>(frame, sizeof(frame)), now_ns, ticket, paths);
    REQUIRE(written != 0U);
    CHECK(holds(read_back(frame, written).data, bytes, id));
    return ticket;
}

// Nothing is due for `paths`, and write_next() agrees.
void nothing(QuicDatagrams& datagrams, const QuicDatagramPaths& paths, u64 now_ns)
{
    CHECK_FALSE(datagrams.has_frame(now_ns, paths));
    u8 frame[kUdp];
    u64 ticket = 0U;
    CHECK(datagrams.write_next(Span<u8>(frame, sizeof(frame)), now_ns, ticket, paths) == 0U);
}

void reported(QuicDatagrams& datagrams, u64 id, QuicDatagramOutcome outcome)
{
    QuicDatagramReport report;
    REQUIRE(datagrams.poll(report));
    CHECK(report.id == id);
    CHECK(report.outcome == outcome);
}

constexpr u32 kBoth = 0x3U; // path slots 0 and 1 carry data
} // namespace

TEST_CASE("QUIC datagrams: a redundant datagram goes once on each path slot carrying data and is reported once",
          "[net][net-path][quic-datagram][multipath]")
{
    CountingAllocator allocator;
    QuicDatagrams datagrams;
    REQUIRE(datagrams.init(allocator, redundant_limits(4096U, 8U, 2U, true)));
    datagrams.set_peer(1200U, kUdp);
    CHECK(datagrams.settles());
    REQUIRE(put(datagrams, 0U, 32U, 0U, redundantly()) == QuicDatagramError::None);
    REQUIRE(put(datagrams, 1U, 32U, 0U) == QuicDatagramError::None);
    // Before its first frame no copy is owed: a slot yielding its data writes nothing.
    nothing(datagrams, on(1U, kBoth, true), 0U);
    const u64 first = sends(datagrams, on(0U, kBoth), 0U, 0U, 32U);
    CHECK(datagrams.queued() == 2U); // its copy is owed to slot 1
    const u64 copy = sends(datagrams, on(1U, kBoth, true), 0U, 0U, 32U);
    CHECK(datagrams.counters().copies == 1U);
    CHECK(datagrams.queued() == 1U);
    // The plain one goes once, on the first slot to write it; a slot yielding its data never sends it.
    nothing(datagrams, on(1U, kBoth, true), 0U);
    CHECK(sends(datagrams, on(1U, kBoth), 0U, 1U, 32U) == 1U); // a plain datagram's ticket is its id
    nothing(datagrams, on(0U, kBoth), 0U);
    CHECK(datagrams.queued() == 0U);
    CHECK(datagrams.counters().sent == 3U);
    // A copy lost while another is in flight reports nothing; the other's acknowledgement reports Acked.
    QuicDatagramReport report;
    datagrams.on_lost(first);
    CHECK_FALSE(datagrams.poll(report));
    datagrams.on_acked(copy);
    reported(datagrams, 0U, QuicDatagramOutcome::Acked);
    datagrams.on_acked(1U);
    reported(datagrams, 1U, QuicDatagramOutcome::Acked);

    // Every copy lost: Lost once, at the last.
    REQUIRE(put(datagrams, 2U, 40U, 0U, redundantly()) == QuicDatagramError::None);
    const u64 lost_first = sends(datagrams, on(1U, kBoth), 0U, 2U, 40U);
    const u64 lost_copy = sends(datagrams, on(0U, kBoth, true), 0U, 2U, 40U);
    datagrams.on_lost(lost_copy);
    CHECK_FALSE(datagrams.poll(report));
    datagrams.on_lost(lost_first);
    reported(datagrams, 2U, QuicDatagramOutcome::Lost);

    // Acknowledged before its copy went: the copy is no longer owed.
    REQUIRE(put(datagrams, 3U, 8U, 0U, redundantly()) == QuicDatagramError::None);
    const u64 only = sends(datagrams, on(0U, kBoth), 0U, 3U, 8U);
    CHECK(datagrams.queued() == 1U);
    datagrams.on_acked(only);
    CHECK(datagrams.queued() == 0U);
    nothing(datagrams, on(1U, kBoth, true), 0U);
    reported(datagrams, 3U, QuicDatagramOutcome::Acked);

    // Acknowledged, then another copy lost: one report.
    REQUIRE(put(datagrams, 4U, 8U, 0U, redundantly()) == QuicDatagramError::None);
    const u64 acked = sends(datagrams, on(0U, kBoth), 0U, 4U, 8U);
    const u64 late = sends(datagrams, on(1U, kBoth), 0U, 4U, 8U);
    datagrams.on_acked(acked);
    datagrams.on_lost(late);
    reported(datagrams, 4U, QuicDatagramOutcome::Acked);
    CHECK_FALSE(datagrams.poll(report));
    CHECK(datagrams.unreported() == 0U);

    // redundant_count records at once, each held from queue() until its every copy settled.
    REQUIRE(put(datagrams, 5U, 8U, 0U, redundantly()) == QuicDatagramError::None);
    const u64 fifth = sends(datagrams, on(0U, kBoth), 0U, 5U, 8U);
    REQUIRE(put(datagrams, 6U, 8U, 0U, redundantly()) == QuicDatagramError::None);
    CHECK(put(datagrams, 7U, 8U, 0U, redundantly()) == QuicDatagramError::Full);
    REQUIRE(put(datagrams, 7U, 8U, 0U) == QuicDatagramError::None); // a plain one takes no record
    const u64 fifth_copy = sends(datagrams, on(1U, kBoth), 0U, 5U, 8U);
    datagrams.on_acked(fifth);
    CHECK(put(datagrams, 8U, 8U, 0U, redundantly()) == QuicDatagramError::Full); // a copy is still in flight
    datagrams.on_lost(fifth_copy);
    REQUIRE(put(datagrams, 8U, 8U, 0U, redundantly()) == QuicDatagramError::None);

    // Without records nothing is queued redundant; without outcomes or records nothing is settled.
    QuicDatagrams plain;
    REQUIRE(plain.init(allocator, limits(4096U, 8U, true)));
    plain.set_peer(1200U, kUdp);
    CHECK(put(plain, 0U, 8U, 0U, redundantly()) == QuicDatagramError::Unsupported);
    CHECK(plain.settles());
    QuicDatagrams bare;
    REQUIRE(bare.init(allocator, limits(4096U, 8U)));
    CHECK_FALSE(bare.settles());
    CHECK_FALSE(QuicDatagrams::limits_valid(redundant_limits(4096U, 8U, 9U, false))); // more records than datagrams
    CHECK_FALSE(QuicDatagrams::limits_valid(redundant_limits(0U, 0U, 1U, false)));    // records without a send ring
    CHECK(QuicDatagrams::limits_valid(redundant_limits(4096U, 8U, 8U, false)));
}

TEST_CASE("QUIC datagrams: owed copies hold the queue's order until they go, and stop for a slot that stops carrying "
          "data, at max_age_ns, for a path grown too small, and before a new datagram is refused room",
          "[net][net-path][quic-datagram][multipath]")
{
    CountingAllocator allocator;
    SECTION("a slot that stops carrying data")
    {
        QuicDatagrams datagrams;
        REQUIRE(datagrams.init(allocator, redundant_limits(4096U, 8U, 4U, false)));
        datagrams.set_peer(1200U, kUdp);
        REQUIRE(put(datagrams, 0U, 16U, 0U, redundantly()) == QuicDatagramError::None);
        for (u64 id = 1U; id <= 2U; ++id)
        {
            REQUIRE(put(datagrams, id, 16U, 0U) == QuicDatagramError::None);
        }
        static_cast<void>(sends(datagrams, on(0U, kBoth), 0U, 0U, 16U));
        static_cast<void>(sends(datagrams, on(0U, kBoth), 0U, 1U, 16U));
        static_cast<void>(sends(datagrams, on(0U, kBoth), 0U, 2U, 16U));
        // Sent datagrams wait behind the copy slot 1 owes: without outcomes the redundant one counts until it goes.
        CHECK(datagrams.queued() == 3U);
        CHECK(datagrams.unreported() == 1U);
        nothing(datagrams, on(0U, kBoth), 0U);
        static_cast<void>(sends(datagrams, on(1U, kBoth, true), 0U, 0U, 16U));
        CHECK(datagrams.queued() == 0U);
        CHECK(datagrams.unreported() == 0U);
        // Slot 1 stops carrying data (dark, held for backup, closed): what it owed is dropped at the next write, one
        // that writes nothing too, and so after a write.
        REQUIRE(put(datagrams, 3U, 16U, 0U, redundantly()) == QuicDatagramError::None);
        static_cast<void>(sends(datagrams, on(0U, kBoth), 0U, 3U, 16U));
        nothing(datagrams, on(0U, 0x1U), 0U);
        CHECK(datagrams.queued() == 0U);
        REQUIRE(put(datagrams, 4U, 16U, 0U, redundantly()) == QuicDatagramError::None);
        static_cast<void>(sends(datagrams, on(0U, kBoth), 0U, 4U, 16U));
        REQUIRE(put(datagrams, 5U, 16U, 0U) == QuicDatagramError::None);
        static_cast<void>(sends(datagrams, on(0U, 0x1U), 0U, 5U, 16U));
        CHECK(datagrams.queued() == 0U);
        nothing(datagrams, on(1U, kBoth, true), 0U);
        CHECK(datagrams.counters().copies == 1U);
    }
    SECTION("max_age_ns")
    {
        QuicDatagrams datagrams;
        QuicDatagramLimits l = redundant_limits(4096U, 8U, 4U, true);
        l.max_age_ns = 10U * kMs;
        REQUIRE(datagrams.init(allocator, l));
        datagrams.set_peer(1200U, kUdp);
        REQUIRE(put(datagrams, 0U, 16U, 0U, redundantly()) == QuicDatagramError::None);
        const u64 ticket = sends(datagrams, on(0U, kBoth), 1U * kMs, 0U, 16U);
        CHECK(datagrams.has_frame(10U * kMs, on(1U, kBoth, true)));
        nothing(datagrams, on(1U, kBoth, true), 10U * kMs + 1U);
        CHECK(datagrams.queued() == 0U);
        CHECK(datagrams.counters().expired == 0U); // it was sent: only its copies stop
        datagrams.on_acked(ticket);
        reported(datagrams, 0U, QuicDatagramOutcome::Acked);
        // One never sent expires as any.
        REQUIRE(put(datagrams, 1U, 16U, 20U * kMs, redundantly()) == QuicDatagramError::None);
        nothing(datagrams, on(0U, kBoth), 31U * kMs);
        CHECK(datagrams.counters().expired == 1U);
        reported(datagrams, 1U, QuicDatagramOutcome::Expired);
        CHECK(datagrams.unreported() == 0U);
    }
    SECTION("a path grown too small")
    {
        QuicDatagrams datagrams;
        REQUIRE(datagrams.init(allocator, redundant_limits(4096U, 8U, 4U, true)));
        datagrams.set_peer(1200U, kUdp);
        REQUIRE(put(datagrams, 0U, 600U, 0U, redundantly()) == QuicDatagramError::None);
        const u64 first = sends(datagrams, on(0U, kBoth), 0U, 0U, 600U);
        datagrams.set_path(400U + kQuicShortPacketOverhead);
        // The copy fits no packet now: skipped on slot 1, and nothing is dropped unsent.
        u8 frame[kUdp];
        u64 ticket = 0U;
        CHECK(datagrams.write_next(Span<u8>(frame, sizeof(frame)), 0U, ticket, on(1U, kBoth, true)) == 0U);
        CHECK(datagrams.queued() == 0U);
        CHECK(datagrams.counters().oversized == 0U);
        datagrams.on_lost(first);
        reported(datagrams, 0U, QuicDatagramOutcome::Lost);
        // One never sent behind an owed copy: the copy gives way, the datagram is dropped as Lost.
        datagrams.set_path(kUdp);
        REQUIRE(put(datagrams, 1U, 600U, 0U, redundantly()) == QuicDatagramError::None);
        REQUIRE(put(datagrams, 2U, 600U, 0U) == QuicDatagramError::None);
        const u64 second = sends(datagrams, on(0U, kBoth), 0U, 1U, 600U);
        datagrams.set_path(400U + kQuicShortPacketOverhead);
        CHECK(datagrams.write_next(Span<u8>(frame, sizeof(frame)), 0U, ticket, on(0U, kBoth)) == 0U);
        CHECK(datagrams.queued() == 0U);
        CHECK(datagrams.counters().oversized == 1U);
        reported(datagrams, 2U, QuicDatagramOutcome::Lost);
        datagrams.on_acked(second);
        reported(datagrams, 1U, QuicDatagramOutcome::Acked);
    }
    SECTION("room for a new datagram")
    {
        // The byte ring: an owed copy gives way to a new datagram, never one waiting to be sent.
        QuicDatagrams datagrams;
        REQUIRE(datagrams.init(allocator, redundant_limits(128U, 4U, 1U, true)));
        datagrams.set_peer(1200U, kUdp);
        REQUIRE(put(datagrams, 0U, 100U, 0U, redundantly()) == QuicDatagramError::None);
        const u64 first = sends(datagrams, on(0U, kBoth), 0U, 0U, 100U);
        REQUIRE(put(datagrams, 1U, 100U, 0U) == QuicDatagramError::None);
        CHECK(datagrams.queued() == 1U);
        nothing(datagrams, on(1U, kBoth, true), 0U);
        CHECK(put(datagrams, 2U, 100U, 0U) == QuicDatagramError::Full);
        datagrams.on_lost(first);
        reported(datagrams, 0U, QuicDatagramOutcome::Lost);
        // send_count without outcomes: the redundant one counts until it leaves the ring, and gives way.
        QuicDatagrams counted;
        REQUIRE(counted.init(allocator, redundant_limits(4096U, 1U, 1U, false)));
        counted.set_peer(1200U, kUdp);
        REQUIRE(put(counted, 0U, 16U, 0U, redundantly()) == QuicDatagramError::None);
        static_cast<void>(sends(counted, on(0U, kBoth), 0U, 0U, 16U));
        CHECK(counted.unreported() == 1U);
        REQUIRE(put(counted, 1U, 16U, 0U) == QuicDatagramError::None);
        CHECK(counted.queued() == 1U);
        CHECK(put(counted, 2U, 16U, 0U) == QuicDatagramError::Full);
        // With outcomes: every copy sent was lost and one is still owed. Full gives the owed copy up, so its Lost
        // report comes and polling it makes room.
        QuicDatagrams reported_full;
        REQUIRE(reported_full.init(allocator, redundant_limits(4096U, 1U, 1U, true)));
        reported_full.set_peer(1200U, kUdp);
        REQUIRE(put(reported_full, 0U, 16U, 0U, redundantly()) == QuicDatagramError::None);
        reported_full.on_lost(sends(reported_full, on(0U, kBoth), 0U, 0U, 16U));
        CHECK(reported_full.queued() == 1U);
        CHECK(put(reported_full, 1U, 16U, 0U) == QuicDatagramError::Full);
        CHECK(reported_full.queued() == 0U);
        reported(reported_full, 0U, QuicDatagramOutcome::Lost);
        REQUIRE(put(reported_full, 1U, 16U, 0U) == QuicDatagramError::None);
    }
}

TEST_CASE("QUIC datagrams: on one path a redundant datagram goes once, and a rejected 0-RTT settles its copies' "
          "records",
          "[net][net-path][quic-datagram][multipath][quic-early]")
{
    CountingAllocator allocator;
    QuicDatagrams datagrams;
    REQUIRE(datagrams.init(allocator, redundant_limits(4096U, 8U, 2U, true)));
    datagrams.set_peer(1200U, kUdp);
    REQUIRE(put(datagrams, 0U, 16U, 0U, redundantly()) == QuicDatagramError::None);
    const u64 only = sends(datagrams, QuicDatagramPaths{}, 0U, 0U, 16U);
    CHECK(datagrams.queued() == 0U);
    datagrams.on_acked(only);
    reported(datagrams, 0U, QuicDatagramOutcome::Acked);
    // One sent with its copy owed, one never sent: the rejection drops both from the ring, the unsent one Lost at
    // once, the sent one Lost when its packet's state goes; both records come free.
    REQUIRE(put(datagrams, 1U, 16U, 0U, redundantly()) == QuicDatagramError::None);
    const u64 sent = sends(datagrams, on(0U, kBoth), 0U, 1U, 16U);
    REQUIRE(put(datagrams, 2U, 16U, 0U, redundantly()) == QuicDatagramError::None);
    datagrams.reject_early();
    CHECK(datagrams.queued() == 0U);
    reported(datagrams, 2U, QuicDatagramOutcome::Lost);
    datagrams.on_lost(sent);
    reported(datagrams, 1U, QuicDatagramOutcome::Lost);
    CHECK(datagrams.unreported() == 0U);
    datagrams.set_peer(1200U, kUdp);
    REQUIRE(put(datagrams, 3U, 16U, 0U, redundantly()) == QuicDatagramError::None);
    REQUIRE(put(datagrams, 4U, 16U, 0U, redundantly()) == QuicDatagramError::None);
}
