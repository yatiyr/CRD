// crd-net -- a QUIC connection's streams (4.2a; docs/design/network-foundation.md#quic-stream-set): the limits and
// parameters, opening within the peer's grant with STREAMS_BLOCKED once per limit, every error RFC 9000 names for a
// stream frame, a peer's frame opening every lower stream through each frame kind, both credits with their blocked
// frames and updates, MAX_STREAMS granted as slots free, resets and STOP_SENDING with section 13.3's retransmissions,
// urgencies with round robin, and many streams carried both ways over an exchange that loses, reorders and duplicates.
// Every frame crosses as bytes: written by write_quic_frame and read back by read_quic_frame.

#include "ingress_fixture.hpp"
#include "quic_stream_fixture.hpp"

#include <crd/containers/wire_bytes.hpp>
#include <crd/net/quic_stream_set.hpp>

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
using crd::containers::WireReader;
using crd::crypto::QuicSide;
using namespace crd::net;
using namespace crd::net::testing;

constexpr u64 kMs = 1000000U;
constexpr u64 kRtt = 20U * kMs;
constexpr usize kRoom = 1200U;
constexpr usize kMaxRecords = 64U;
constexpr usize kEchoStreams = 24U; // the many-streams case
constexpr usize kEchoFlight = 48U;  // packets its exchange holds

QuicStreamLimits limits(u32 bidi = 4U, u32 uni = 2U, u32 window = 4096U)
{
    QuicStreamLimits l;
    l.peer_bidi = bidi;
    l.peer_uni = uni;
    l.local_bidi = bidi;
    l.local_uni = uni;
    l.receive_bytes = 8192U;
    l.send_bytes = 8192U;
    l.stream_window = window;
    l.connection_window = 16384U;
    l.connection_max_window = 65536U;
    return l;
}

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

// The frames one end put into one packet, as bytes, with the records that settle them.
struct Packet
{
    u8 bytes[kRoom] = {};
    usize size = 0U;
    QuicStreamRecord records[kMaxRecords];
    usize count = 0U;
    usize from = 0U;
};

// A client's and a server's stream sets, their parameters exchanged.
class Pair
{
public:
    explicit Pair(const QuicStreamLimits& client, const QuicStreamLimits& server)
    {
        REQUIRE(end[0].init(allocator, client, QuicSide::Client));
        REQUIRE(end[1].init(allocator, server, QuicSide::Server));
        QuicTransportParameters parameters[2];
        end[0].fill_parameters(parameters[0]);
        end[1].fill_parameters(parameters[1]);
        end[0].set_peer_parameters(parameters[1]);
        end[1].set_peer_parameters(parameters[0]);
    }

    CountingAllocator allocator;
    QuicStreamSet end[2];
    u64 now = 1000U * kMs;

    // The frames `from` has now, up to `room` bytes, round tripped through the codec.
    Packet take(usize from, usize room = kRoom)
    {
        Packet packet;
        packet.from = from;
        QuicFrame frame;
        QuicStreamRecord record;
        while (packet.count < kMaxRecords && end[from].next_frame(room - packet.size, now, kRtt, frame, record))
        {
            const usize bytes = write_quic_frame(frame, Span<u8>(packet.bytes + packet.size, room - packet.size));
            REQUIRE(bytes != 0U);
            REQUIRE(bytes == quic_frame_bytes(frame));
            packet.size += bytes;
            packet.records[packet.count++] = record;
        }
        return packet;
    }

    // The packet's frames reach the other end: the first error, or NoError.
    QuicTransportError deliver(const Packet& packet)
    {
        WireReader in(ConstSpan<u8>(packet.bytes, packet.size));
        while (!in.done())
        {
            QuicFrame frame;
            REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
            const QuicTransportError error = end[1U - packet.from].on_frame(frame);
            if (error != QuicTransportError::NoError)
            {
                return error;
            }
        }
        return QuicTransportError::NoError;
    }

    void acked(const Packet& packet)
    {
        for (usize index = 0U; index < packet.count; ++index)
        {
            end[packet.from].on_acked(packet.records[index]);
        }
    }

    void lost(const Packet& packet)
    {
        for (usize index = 0U; index < packet.count; ++index)
        {
            end[packet.from].on_lost(packet.records[index]);
        }
    }

    // Sends, delivers and acknowledges until neither end has a frame.
    void settle()
    {
        for (usize round = 0U; round < 1000U && (end[0].has_frame() || end[1].has_frame()); ++round)
        {
            for (usize from = 0U; from < 2U; ++from)
            {
                if (end[from].has_frame())
                {
                    const Packet packet = take(from);
                    REQUIRE(packet.count != 0U);
                    REQUIRE(deliver(packet) == QuicTransportError::NoError);
                    acked(packet);
                }
            }
            now += kRtt;
        }
        CHECK_FALSE(end[0].has_frame());
        CHECK_FALSE(end[1].has_frame());
    }
};

// Both ends advertise reset_stream_at.
QuicStreamLimits reliable_limits()
{
    QuicStreamLimits l = limits(2U, 1U, 4096U);
    l.reset_stream_at = true;
    return l;
}

// RESET_STREAM_AT delivered to `set` as if from its peer.
QuicTransportError reset_at_to(QuicStreamSet& set, u64 id, u64 error, u64 final_size, u64 reliable)
{
    QuicFrame frame;
    frame.kind = QuicFrameKind::ResetStreamAt;
    frame.stream_id = id;
    frame.error_code = error;
    frame.value = final_size;
    frame.offset = reliable;
    return set.on_frame(frame);
}

// The frame kinds of a packet, in order.
bool has_kind(const Packet& packet, QuicFrameKind kind)
{
    for (usize index = 0U; index < packet.count; ++index)
    {
        if (packet.records[index].kind == kind)
        {
            return true;
        }
    }
    return false;
}

// A frame of `kind` on stream `id`, delivered to `set` as if from its peer.
QuicTransportError frame_to(QuicStreamSet& set, QuicFrameKind kind, u64 id, u64 value = 0U, ConstSpan<u8> data = {},
                            bool fin = false, u64 offset = 0U)
{
    QuicFrame frame;
    frame.kind = kind;
    frame.stream_id = id;
    frame.value = value;
    frame.data = data;
    frame.fin = fin;
    frame.offset = offset;
    frame.bidirectional = (id & 2U) == 0U;
    return set.on_frame(frame);
}

usize events_of(QuicStreamSet& set, u64 id, u8& mask)
{
    usize count = 0U;
    mask = 0U;
    QuicStreamEvent event;
    while (set.poll(event))
    {
        if (event.id == id)
        {
            mask = static_cast<u8>(mask | event.events);
        }
        ++count;
    }
    return count;
}

// Ends both parts of client stream `id` (a FIN each way, read and acknowledged) and polls both ends' events.
void finish_both(Pair& pair, u64 id)
{
    usize taken = 0U;
    REQUIRE(pair.end[0].write(id, {}, true, taken) == QuicStreamError::None);
    pair.settle();
    REQUIRE(pair.end[1].consume(id, 0U) == QuicStreamError::None);
    REQUIRE(pair.end[1].write(id, {}, true, taken) == QuicStreamError::None);
    pair.settle();
    REQUIRE(pair.end[0].consume(id, 0U) == QuicStreamError::None);
    QuicStreamEvent event;
    for (QuicStreamSet& end : pair.end)
    {
        while (end.poll(event))
        {
        }
    }
}
} // namespace

TEST_CASE("QUIC stream set: limits are checked and fill this side's parameters", "[net][4.2a][quic-stream-set]")
{
    CHECK(QuicStreamSet::limits_valid(QuicStreamLimits{}));
    CHECK(QuicStreamSet::limits_valid(limits()));
    QuicStreamLimits bad = limits();
    bad.receive_bytes = 100U; // not a multiple of the chunk
    CHECK_FALSE(QuicStreamSet::limits_valid(bad));
    bad = limits();
    bad.stream_window = 0U;
    CHECK_FALSE(QuicStreamSet::limits_valid(bad));
    bad = limits();
    bad.stream_window = bad.receive_bytes + 1U;
    CHECK_FALSE(QuicStreamSet::limits_valid(bad));
    bad = limits();
    bad.send_bytes = 0U;
    CHECK_FALSE(QuicStreamSet::limits_valid(bad));
    bad = limits();
    bad.peer_bidi = kQuicMaxStreamSlots + 1U;
    CHECK_FALSE(QuicStreamSet::limits_valid(bad));
    bad = limits();
    bad.connection_window = bad.connection_max_window + 1U;
    CHECK_FALSE(QuicStreamSet::limits_valid(bad));
    bad = limits();
    bad.receive_bytes = kQuicMaxStreamBuffer + kQuicStreamChunk;
    CHECK_FALSE(QuicStreamSet::limits_valid(bad));
    // A side that only sends unidirectional streams needs no receive buffer.
    QuicStreamLimits sender;
    sender.local_uni = 2U;
    sender.send_bytes = 1024U;
    CHECK(QuicStreamSet::limits_valid(sender));

    CountingAllocator allocator;
    QuicStreamSet set;
    REQUIRE(set.init(allocator, limits(4U, 2U, 2048U), QuicSide::Server));
    CHECK_FALSE(set.init(allocator, limits(), QuicSide::Server)); // once
    QuicTransportParameters local;
    set.fill_parameters(local);
    CHECK(local.initial_max_data == 16384U);
    CHECK(local.initial_max_stream_data_bidi_local == 2048U);
    CHECK(local.initial_max_stream_data_bidi_remote == 2048U);
    CHECK(local.initial_max_stream_data_uni == 2048U);
    CHECK(local.initial_max_streams_bidi == 4U);
    CHECK(local.initial_max_streams_uni == 2U);

    QuicStreamSet none;
    REQUIRE(none.init(allocator, QuicStreamLimits{}, QuicSide::Client));
    none.fill_parameters(local);
    CHECK(local.initial_max_streams_bidi == 0U);
    CHECK(local.initial_max_stream_data_uni == 0U);
    u64 id = 0U;
    CHECK(none.open(true, id) == QuicStreamError::NoSlot);
    CHECK_FALSE(none.has_frame()); // no slot: no STREAMS_BLOCKED

    CountingAllocator refusing;
    refusing.refuse_after(2U);
    QuicStreamSet refused;
    CHECK_FALSE(refused.init(refusing, limits(), QuicSide::Client));
}

TEST_CASE("QUIC stream set: streams open within the peer's grant, with one STREAMS_BLOCKED per limit",
          "[net][4.2a][quic-stream-set]")
{
    // An open() before the peer's parameters is Blocked at limit 0; the grant they bring makes its STREAMS_BLOCKED
    // stale, so it is never sent.
    {
        CountingAllocator early_allocator;
        QuicStreamSet early;
        REQUIRE(early.init(early_allocator, limits(4U, 2U), QuicSide::Client));
        u64 id = 0U;
        CHECK(early.open(true, id) == QuicStreamError::Blocked);
        CHECK(early.has_frame());
        QuicTransportParameters peer;
        peer.initial_max_streams_bidi = 2U;
        early.set_peer_parameters(peer);
        CHECK_FALSE(early.has_frame());
        CHECK(early.open(true, id) == QuicStreamError::None);
    }
    QuicStreamLimits server = limits(2U, 1U);
    server.local_bidi = 0U;
    QuicStreamLimits client = limits(4U, 2U);
    Pair pair(client, server);
    QuicStreamSet& c = pair.end[0];
    u64 ids[4] = {};
    CHECK(c.open(true, ids[0]) == QuicStreamError::None);
    CHECK(c.open(true, ids[1]) == QuicStreamError::None);
    CHECK(ids[0] == 0U);
    CHECK(ids[1] == 4U);
    CHECK(c.peer_limit(true) == 2U);
    CHECK(c.open(true, ids[2]) == QuicStreamError::Blocked);
    CHECK(c.open(true, ids[2]) == QuicStreamError::Blocked);
    u64 uni = 0U;
    CHECK(c.open(false, uni) == QuicStreamError::None);
    CHECK(uni == 2U);
    CHECK(c.send_state(uni) == QuicSendState::Ready);
    CHECK(c.recv_state(uni) == QuicRecvState::None);

    Packet packet = pair.take(0U);
    REQUIRE(packet.count == 1U);
    CHECK(packet.records[0].kind == QuicFrameKind::StreamsBlocked);
    CHECK(packet.records[0].value == 2U);
    CHECK(packet.records[0].bidirectional);
    CHECK_FALSE(c.has_frame()); // once per limit
    CHECK(c.open(true, ids[2]) == QuicStreamError::Blocked);
    CHECK_FALSE(c.has_frame());
    pair.lost(packet); // lost while still blocked at that limit: owed again
    packet = pair.take(0U);
    REQUIRE(packet.count == 1U);
    CHECK(packet.records[0].kind == QuicFrameKind::StreamsBlocked);
    REQUIRE(pair.deliver(packet) == QuicTransportError::NoError);
    pair.acked(packet);
    // The server has no free slot until a stream is freed, so its STREAMS_BLOCKED owes nothing yet.
    CHECK_FALSE(pair.end[1].has_frame());
    CHECK(frame_to(c, QuicFrameKind::MaxStreams, 0U, 5U) == QuicTransportError::NoError);
    CHECK(frame_to(c, QuicFrameKind::MaxStreams, 0U, 3U) == QuicTransportError::NoError); // never lowers
    CHECK(c.peer_limit(true) == 5U);
    CHECK(c.open(true, ids[2]) == QuicStreamError::None);
    CHECK(c.open(true, ids[3]) == QuicStreamError::None);
    u64 more = 0U;
    CHECK(c.open(true, more) == QuicStreamError::NoSlot); // four slots, all in use
    CHECK(c.open_streams() == 5U);
    CHECK(pair.allocator.calls() == 8U); // the two sets' four arrays each, nothing since
}

TEST_CASE("QUIC stream set: every stream frame error RFC 9000 names", "[net][4.2a][quic-stream-set]")
{
    CountingAllocator allocator;
    QuicStreamSet s;
    REQUIRE(s.init(allocator, limits(2U, 1U, 1024U), QuicSide::Server));
    QuicTransportParameters peer;
    peer.initial_max_streams_bidi = 4U;
    peer.initial_max_data = 1U << 20U;
    s.set_peer_parameters(peer);
    const u8 data[64] = {};
    const ConstSpan<u8> bytes(data, sizeof(data));
    using K = QuicFrameKind;
    // Streams this server never opened (server IDs end in 1 or 3).
    CHECK(frame_to(s, K::Stream, 1U, 0U, bytes) == QuicTransportError::StreamStateError);
    CHECK(frame_to(s, K::MaxStreamData, 5U, 10U) == QuicTransportError::StreamStateError);
    CHECK(frame_to(s, K::StopSending, 1U) == QuicTransportError::StreamStateError);
    // A send-only stream of its own receives no data, and a client's unidirectional stream takes no credit.
    CHECK(frame_to(s, K::Stream, 3U, 0U, bytes) == QuicTransportError::StreamStateError);
    CHECK(frame_to(s, K::ResetStream, 3U) == QuicTransportError::StreamStateError);
    CHECK(frame_to(s, K::StreamDataBlocked, 3U) == QuicTransportError::StreamStateError);
    CHECK(frame_to(s, K::MaxStreamData, 2U, 10U) == QuicTransportError::StreamStateError);
    CHECK(frame_to(s, K::StopSending, 2U) == QuicTransportError::StreamStateError);
    // Past this side's grant: two bidirectional, one unidirectional.
    CHECK(frame_to(s, K::Stream, 8U, 0U, bytes) == QuicTransportError::StreamLimitError);
    CHECK(frame_to(s, K::StreamDataBlocked, 6U) == QuicTransportError::StreamLimitError);
    CHECK(frame_to(s, K::MaxStreamData, 8U, 1U) == QuicTransportError::StreamLimitError);
    // Flow control: the stream's window, then the connection's (16384 across streams, 1024 per stream here).
    CHECK(frame_to(s, K::Stream, 0U, 0U, bytes, false, 1000U) == QuicTransportError::FlowControlError);
    QuicStreamSet c;
    QuicStreamLimits small = limits(4U, 1U, 1024U);
    small.connection_window = 1500U;
    small.connection_max_window = 1500U;
    REQUIRE(c.init(allocator, small, QuicSide::Server));
    CHECK(frame_to(c, K::Stream, 0U, 0U, bytes, false, 960U) == QuicTransportError::NoError);
    CHECK(frame_to(c, K::Stream, 4U, 0U, bytes, false, 960U) == QuicTransportError::FlowControlError);
    // A RESET_STREAM's final size takes credit too.
    QuicStreamSet r;
    REQUIRE(r.init(allocator, small, QuicSide::Server));
    CHECK(frame_to(r, K::ResetStream, 0U, 1025U) == QuicTransportError::FlowControlError);
    // Final sizes (section 4.5) and changed bytes (section 2.2).
    QuicStreamSet f;
    REQUIRE(f.init(allocator, limits(2U, 1U, 1024U), QuicSide::Server));
    CHECK(frame_to(f, K::Stream, 0U, 0U, bytes, true) == QuicTransportError::NoError);
    CHECK(frame_to(f, K::Stream, 0U, 0U, bytes, false, 10U) == QuicTransportError::FinalSizeError);
    CHECK(frame_to(f, K::ResetStream, 0U, 63U) == QuicTransportError::FinalSizeError);
    CHECK(frame_to(f, K::Stream, 4U, 0U, bytes) == QuicTransportError::NoError);
    CHECK(frame_to(f, K::ResetStream, 4U, 10U) == QuicTransportError::FinalSizeError); // below the data received
    u8 other[64] = {};
    other[5] = 1U;
    CHECK(frame_to(f, K::Stream, 4U, 0U, ConstSpan<u8>(other, sizeof(other))) == QuicTransportError::ProtocolViolation);
    CHECK(frame_to(f, K::Stream, 2U, 0U, bytes) == QuicTransportError::NoError);
}

TEST_CASE("QUIC stream set: a peer's frame opens every lower stream of its type, through each frame kind",
          "[net][4.2a][quic-stream-set]")
{
    using K = QuicFrameKind;
    const u8 data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const K kinds[] = {K::Stream, K::ResetStream, K::StreamDataBlocked, K::MaxStreamData, K::StopSending};
    for (const K kind : kinds)
    {
        CountingAllocator allocator;
        QuicStreamSet s;
        REQUIRE(s.init(allocator, limits(4U, 2U), QuicSide::Server));
        QuicTransportParameters peer;
        peer.initial_max_data = 1U << 20U;
        peer.initial_max_stream_data_bidi_local = 1U << 16U;
        s.set_peer_parameters(peer);
        INFO("kind " << static_cast<u32>(kind));
        REQUIRE(frame_to(s, kind, 8U, (kind == K::ResetStream) ? 8U : 100U, ConstSpan<u8>(data, sizeof(data))) ==
                QuicTransportError::NoError);
        CHECK(s.open_streams() == 3U); // 0, 4 and 8
        CHECK(s.recv_state(0U) == QuicRecvState::Recv);
        CHECK(s.send_state(4U) == QuicSendState::Ready);
        usize opened = 0U;
        QuicStreamEvent event;
        while (s.poll(event))
        {
            opened += ((event.events & kQuicStreamOpened) != 0U) ? 1U : 0U;
        }
        CHECK(opened == 3U);
        // Frames for the streams now open change nothing about the count; a reopened ID is never opened twice.
        CHECK(frame_to(s, K::StreamDataBlocked, 4U) == QuicTransportError::NoError);
        CHECK(s.open_streams() == 3U);
    }
    // A unidirectional stream opens by STREAM, RESET_STREAM or STREAM_DATA_BLOCKED.
    CountingAllocator allocator;
    QuicStreamSet s;
    REQUIRE(s.init(allocator, limits(4U, 2U), QuicSide::Server));
    CHECK(frame_to(s, K::StreamDataBlocked, 6U, 10U) == QuicTransportError::NoError);
    CHECK(s.open_streams() == 2U);
    CHECK(s.recv_state(2U) == QuicRecvState::Recv);
    CHECK(s.send_state(2U) == QuicSendState::None);
}

TEST_CASE("QUIC stream set: data crosses both credits, with blocked frames once per limit and updates",
          "[net][4.2a][quic-stream-set]")
{
    QuicStreamLimits client = limits(2U, 1U, 1024U);
    QuicStreamLimits server = limits(2U, 1U, 1024U);
    server.connection_window = 1536U;
    server.connection_max_window = 1536U;
    Pair pair(client, server);
    QuicStreamSet& c = pair.end[0];
    QuicStreamSet& s = pair.end[1];
    u64 a = 0U;
    u64 b = 0U;
    REQUIRE(c.open(true, a) == QuicStreamError::None);
    REQUIRE(c.open(true, b) == QuicStreamError::None);
    u64 written_a = 0U;
    u64 written_b = 0U;
    CHECK(write_pattern(c, a, written_a, 3000U, a));
    CHECK(write_pattern(c, b, written_b, 3000U, b));
    // Stream a takes its 1024 bytes, b the connection's last 512; then a's STREAM_DATA_BLOCKED and the
    // connection's DATA_BLOCKED go, once each.
    usize stream_blocked = 0U;
    usize data_blocked = 0U;
    for (Packet more = pair.take(0U); more.count != 0U; more = pair.take(0U))
    {
        REQUIRE(pair.deliver(more) == QuicTransportError::NoError);
        pair.acked(more);
        for (usize index = 0U; index < more.count; ++index)
        {
            const QuicStreamRecord& record = more.records[index];
            stream_blocked += (record.kind == QuicFrameKind::StreamDataBlocked) ? 1U : 0U;
            data_blocked += (record.kind == QuicFrameKind::DataBlocked) ? 1U : 0U;
            if (record.kind == QuicFrameKind::StreamDataBlocked)
            {
                CHECK(record.stream_id == a);
                CHECK(record.value == 1024U);
            }
            if (record.kind == QuicFrameKind::DataBlocked)
            {
                CHECK(record.value == 1536U);
            }
        }
    }
    CHECK(stream_blocked == 1U);
    CHECK(data_blocked == 1U);
    CHECK(c.connection_sent() == 1536U);
    CHECK(c.connection_credit().limit() == 1536U);
    CHECK_FALSE(c.has_frame()); // blocked once per limit: nothing more until credit arrives
    // The server reads: its window updates go once half of each window was read.
    u64 read_a = 0U;
    u64 read_b = 0U;
    CHECK_FALSE(drain(s, a, read_a, a));
    CHECK_FALSE(drain(s, b, read_b, b));
    CHECK(read_a + read_b == 1536U);
    REQUIRE(s.has_frame());
    pair.settle();
    // Settled with every update sent and every byte through: write the rest and carry it all.
    for (usize round = 0U; round < 200U && (read_a < 3000U || read_b < 3000U); ++round)
    {
        static_cast<void>(write_pattern(c, a, written_a, 3000U, a));
        static_cast<void>(write_pattern(c, b, written_b, 3000U, b));
        pair.settle();
        static_cast<void>(drain(s, a, read_a, a));
        static_cast<void>(drain(s, b, read_b, b));
    }
    CHECK(read_a == 3000U);
    CHECK(read_b == 3000U);
    CHECK(s.recv_state(a) == QuicRecvState::DataRead);
    CHECK(s.connection_window().consumed() == 6000U);
}

TEST_CASE("QUIC stream set: lost credit frames go again as section 13.3 says", "[net][4.2a][quic-stream-set]")
{
    Pair pair(limits(2U, 1U, 1024U), limits(2U, 1U, 1024U));
    QuicStreamSet& c = pair.end[0];
    QuicStreamSet& s = pair.end[1];
    u64 id = 0U;
    REQUIRE(c.open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    static_cast<void>(write_pattern(c, id, written, 2048U, id));
    pair.settle(); // 1024 bytes cross and the STREAM_DATA_BLOCKED
    u64 read = 0U;
    CHECK_FALSE(drain(s, id, read, id));
    CHECK(read == 1024U);
    // MAX_STREAM_DATA lost: the latest goes again; lost again after a newer one went: only the newer is owed.
    Packet update = pair.take(1U);
    REQUIRE(update.count == 1U);
    CHECK(update.records[0].kind == QuicFrameKind::MaxStreamData);
    CHECK_FALSE(s.has_frame());
    pair.lost(update);
    Packet again = pair.take(1U);
    REQUIRE(again.count == 1U);
    CHECK(again.records[0].value == update.records[0].value);
    REQUIRE(pair.deliver(again) == QuicTransportError::NoError);
    pair.acked(again);
    // The rest arrives with the FIN: once the final size is known no MAX_STREAM_DATA goes (section 13.3).
    pair.settle();
    CHECK(drain(s, id, read, id));
    CHECK(read == 2048U);
    pair.lost(again);
    CHECK_FALSE(s.has_frame());

    // MAX_STREAMS lost: owed again while it is the latest grant; MAX_DATA likewise.
    Pair grants(limits(2U, 1U, 1024U), limits(2U, 1U, 1024U));
    QuicStreamSet& gc = grants.end[0];
    QuicStreamSet& gs = grants.end[1];
    u64 ids[2] = {};
    REQUIRE(gc.open(true, ids[0]) == QuicStreamError::None);
    REQUIRE(gc.open(true, ids[1]) == QuicStreamError::None);
    for (const u64 stream : ids)
    {
        usize taken = 0U;
        REQUIRE(gc.write(stream, {}, true, taken) == QuicStreamError::None);
    }
    grants.settle();
    QuicStreamEvent event;
    while (gs.poll(event))
    {
    }
    for (const u64 stream : ids)
    {
        REQUIRE(gs.consume(stream, 0U) == QuicStreamError::None); // the FIN read: DataRead
        usize taken = 0U;
        REQUIRE(gs.write(stream, {}, true, taken) == QuicStreamError::None);
    }
    grants.settle(); // both ends' FINs acknowledged; the slots free once the events were polled
    while (gs.poll(event))
    {
    }
    while (gc.poll(event))
    {
    }
    for (const u64 stream : ids)
    {
        static_cast<void>(gc.consume(stream, 0U));
    }
    CHECK(gs.open_streams() == 0U);
    REQUIRE(gs.has_frame());
    Packet grant = grants.take(1U);
    REQUIRE(grant.count == 1U);
    CHECK(grant.records[0].kind == QuicFrameKind::MaxStreams);
    CHECK(grant.records[0].value == 4U);
    CHECK(gs.granted(true) == 4U);
    grants.lost(grant);
    Packet regrant = grants.take(1U);
    REQUIRE(regrant.count == 1U);
    CHECK(regrant.records[0].value == 4U);
    REQUIRE(grants.deliver(regrant) == QuicTransportError::NoError);
    CHECK(gc.peer_limit(true) == 4U);
    // A peer that says it is blocked gets a grant as soon as one more slot is free; DATA_BLOCKED is informational.
    CHECK(frame_to(gs, QuicFrameKind::StreamsBlocked, 0U, 4U) == QuicTransportError::NoError);
    CHECK_FALSE(gs.has_frame()); // nothing freed since the last grant
}

TEST_CASE("QUIC stream set: grants follow the slots freed, half the slots at a time", "[net][4.2a][quic-stream-set]")
{
    Pair pair(limits(4U, 1U), limits(4U, 1U));
    QuicStreamSet& c = pair.end[0];
    QuicStreamSet& s = pair.end[1];
    u64 ids[4] = {};
    for (u64& id : ids)
    {
        REQUIRE(c.open(true, id) == QuicStreamError::None);
    }
    CHECK(c.open(true, ids[0]) == QuicStreamError::NoSlot);
    // One stream ended both ways and freed: a quarter of the slots, no grant yet.
    finish_both(pair, ids[0]);
    CHECK(s.open_streams() == 0U); // the server only ever saw stream 0
    CHECK(c.open_streams() == 3U);
    CHECK(s.granted(true) == 4U);
    CHECK_FALSE(s.has_frame());
    // A slot whose events are not yet polled is not free: the grant waits for the application.
    usize taken = 0U;
    REQUIRE(c.write(ids[1], {}, true, taken) == QuicStreamError::None);
    pair.settle();
    REQUIRE(s.consume(ids[1], 0U) == QuicStreamError::None);
    REQUIRE(s.write(ids[1], {}, true, taken) == QuicStreamError::None);
    pair.settle();
    CHECK(s.open_streams() == 1U);
    CHECK(s.granted(true) == 4U);
    QuicStreamEvent event;
    while (s.poll(event))
    {
    }
    CHECK(s.open_streams() == 0U);
    // Half the slots freed: MAX_STREAMS grants the freed streams plus the slots.
    Packet grant = pair.take(1U);
    REQUIRE(grant.count == 1U);
    CHECK(grant.records[0].kind == QuicFrameKind::MaxStreams);
    CHECK(grant.records[0].value == 6U);
    REQUIRE(pair.deliver(grant) == QuicTransportError::NoError);
    pair.acked(grant);
    CHECK(c.peer_limit(true) == 6U);
    // The client frees its own end of the second stream and opens two more: the IDs keep rising.
    REQUIRE(c.consume(ids[1], 0U) == QuicStreamError::None);
    while (c.poll(event))
    {
    }
    u64 next = 0U;
    REQUIRE(c.open(true, next) == QuicStreamError::None);
    CHECK(next == 16U);
    REQUIRE(c.open(true, next) == QuicStreamError::None);
    CHECK(next == 20U);
    CHECK(c.open(true, next) == QuicStreamError::NoSlot); // 8, 12, 16 and 20 hold the four slots
}

TEST_CASE("QUIC stream set: a reset stream's lost data is never resent and counts as read to its final size",
          "[net][4.2a][quic-stream-set]")
{
    Pair pair(limits(2U, 1U, 4096U), limits(2U, 1U, 4096U));
    QuicStreamSet& c = pair.end[0];
    QuicStreamSet& s = pair.end[1];
    u64 id = 0U;
    REQUIRE(c.open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    static_cast<void>(write_pattern(c, id, written, 3000U, id));
    const Packet first = pair.take(0U);
    REQUIRE(pair.deliver(first) == QuicTransportError::NoError);
    pair.acked(first);
    const Packet second = pair.take(0U); // lost below
    CHECK(c.send_state(id) == QuicSendState::Send);
    REQUIRE(c.reset(id, 0x77U) == QuicStreamError::None);
    CHECK(c.send_state(id) == QuicSendState::ResetSent);
    CHECK(c.reset(id, 1U) == QuicStreamError::Ended);
    usize taken = 0U;
    CHECK(c.write(id, ConstSpan<u8>(), false, taken) == QuicStreamError::Ended);
    pair.lost(second);
    Packet reset = pair.take(0U);
    REQUIRE(reset.count == 1U); // the RESET_STREAM alone: the lost data does not go again
    CHECK(reset.records[0].kind == QuicFrameKind::ResetStream);
    pair.lost(reset); // and a lost RESET_STREAM goes again
    reset = pair.take(0U);
    REQUIRE(reset.count == 1U);
    CHECK(reset.records[0].kind == QuicFrameKind::ResetStream);
    REQUIRE(pair.deliver(reset) == QuicTransportError::NoError);
    pair.acked(reset);
    CHECK(c.send_state(id) == QuicSendState::ResetRecvd);
    // The receiver: the final size is the bytes sent, all counted as read; the reset is reported once.
    CHECK(s.recv_state(id) == QuicRecvState::ResetRecvd);
    CHECK(s.connection_window().consumed() == first.records[0].length + second.records[0].length);
    const QuicStreamRead read = s.peek(id);
    CHECK(read.reset);
    CHECK(read.reset_error == 0x77U);
    CHECK(s.consume(id, 0U) == QuicStreamError::Ended);
    u8 mask = 0U;
    CHECK(events_of(s, id, mask) == 1U);
    CHECK((mask & kQuicStreamReset) != 0U);
    CHECK(s.recv_state(id) == QuicRecvState::ResetRead);
    // The late data and a repeated RESET_STREAM change nothing and report nothing; one naming another final size is
    // FINAL_SIZE_ERROR.
    const u64 consumed = s.connection_window().consumed();
    REQUIRE(pair.deliver(second) == QuicTransportError::NoError);
    REQUIRE(pair.deliver(reset) == QuicTransportError::NoError);
    CHECK(events_of(s, id, mask) == 0U);
    CHECK(s.recv_state(id) == QuicRecvState::ResetRead);
    CHECK(s.connection_window().consumed() == consumed);
    CHECK(frame_to(s, QuicFrameKind::ResetStream, id, 5U) == QuicTransportError::FinalSizeError);
}

TEST_CASE("QUIC stream set: STOP_SENDING resets the peer's sending part with its code, resent until it lands",
          "[net][4.2a][quic-stream-set]")
{
    Pair pair(limits(2U, 1U, 4096U), limits(2U, 1U, 4096U));
    QuicStreamSet& c = pair.end[0];
    QuicStreamSet& s = pair.end[1];
    u64 id = 0U;
    REQUIRE(c.open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    static_cast<void>(write_pattern(c, id, written, 6000U, id));
    const Packet late = pair.take(0U); // held back: it arrives after the server stops
    pair.settle();
    REQUIRE(s.stop_sending(id, 0x42U) == QuicStreamError::None);
    CHECK(s.stop_sending(id, 0x42U) == QuicStreamError::Ended);
    CHECK(s.peek(id).data.empty()); // dropped, and its credit returned
    // Data arriving after STOP_SENDING is dropped as it comes, never readable, its credit returned at once.
    u8 mask = 0U;
    static_cast<void>(events_of(s, id, mask));
    REQUIRE(late.count >= 1U);
    REQUIRE(pair.deliver(late) == QuicTransportError::NoError);
    pair.acked(late);
    CHECK(s.peek(id).data.empty());
    CHECK(s.connection_window().consumed() == s.connection_window().received());
    CHECK(events_of(s, id, mask) == 0U);
    Packet stop = pair.take(1U);
    bool found = false;
    for (usize index = 0U; index < stop.count; ++index)
    {
        found = found || stop.records[index].kind == QuicFrameKind::StopSending;
    }
    REQUIRE(found);
    pair.lost(stop);
    stop = pair.take(1U);
    REQUIRE(stop.count >= 1U);
    REQUIRE(pair.deliver(stop) == QuicTransportError::NoError);
    pair.acked(stop);
    CHECK(c.send_state(id) == QuicSendState::ResetSent);
    QuicStreamEvent event;
    bool told = false;
    while (c.poll(event))
    {
        if (event.id == id && (event.events & kQuicStreamStopSending) != 0U)
        {
            told = event.stop_error == 0x42U;
        }
    }
    CHECK(told);
    Packet reset = pair.take(0U);
    REQUIRE(reset.count >= 1U);
    CHECK(reset.records[0].kind == QuicFrameKind::ResetStream);
    REQUIRE(pair.deliver(reset) == QuicTransportError::NoError);
    pair.acked(reset);
    CHECK(s.recv_state(id) == QuicRecvState::ResetRecvd);
    CHECK(s.peek(id).reset_error == 0x42U); // the RESET_STREAM carries the STOP_SENDING's code (section 3.5)
    CHECK(events_of(s, id, mask) >= 1U);
    CHECK((mask & kQuicStreamReset) != 0U);
    // The server's own sending part ends too; then the stream is freed on both sides.
    usize taken = 0U;
    REQUIRE(s.write(id, {}, true, taken) == QuicStreamError::None);
    pair.settle();
    static_cast<void>(c.poll(event));
    while (c.poll(event))
    {
    }
    REQUIRE(c.consume(id, 0U) == QuicStreamError::None);
    while (s.poll(event))
    {
    }
    CHECK(s.open_streams() == 0U);
    CHECK(c.open_streams() == 0U);
}

TEST_CASE("QUIC stream set: the FIN alone, reads across the buffer's end, and consumption bounds",
          "[net][4.2a][quic-stream-set]")
{
    Pair pair(limits(2U, 2U, 4096U), limits(2U, 2U, 4096U));
    QuicStreamSet& c = pair.end[0];
    QuicStreamSet& s = pair.end[1];
    u64 id = 0U;
    REQUIRE(c.open(false, id) == QuicStreamError::None);
    u64 written = 0U;
    CHECK(write_pattern(c, id, written, 7000U, id));
    u64 read = 0U;
    // 8192-byte buffers with a 4096-byte window: reading in uneven bites wraps the storage.
    for (usize round = 0U; round < 100U && read < 7000U; ++round)
    {
        pair.settle();
        QuicStreamRead peek = s.peek(id);
        const usize bite = (peek.data.size() > 1000U) ? 1000U : peek.data.size();
        for (usize at = 0U; at < bite; ++at)
        {
            REQUIRE(peek.data[at] == pattern(id, read + at));
        }
        CHECK(s.consume(id, peek.data.size() + 1U) == QuicStreamError::Invalid);
        REQUIRE(s.consume(id, bite) == QuicStreamError::None);
        read += bite;
    }
    CHECK(read == 7000U);
    CHECK(s.recv_state(id) == QuicRecvState::DataRead);
    // A stream finished without data sends one STREAM frame holding only the FIN.
    u64 empty = 0U;
    REQUIRE(c.open(false, empty) == QuicStreamError::None);
    usize taken = 0U;
    REQUIRE(c.write(empty, {}, true, taken) == QuicStreamError::None);
    const Packet fin = pair.take(0U);
    REQUIRE(fin.count == 1U);
    CHECK(fin.records[0].kind == QuicFrameKind::Stream);
    CHECK(fin.records[0].length == 0U);
    CHECK(fin.records[0].fin);
    REQUIRE(pair.deliver(fin) == QuicTransportError::NoError);
    pair.acked(fin);
    CHECK(c.send_state(empty) == QuicSendState::DataRecvd);
    const QuicStreamRead end = s.peek(empty);
    CHECK(end.fin);
    CHECK(end.data.empty());
    REQUIRE(s.consume(empty, 0U) == QuicStreamError::None);
    CHECK(s.recv_state(empty) == QuicRecvState::DataRead);
    CHECK(s.write(empty, {}, false, taken) == QuicStreamError::Unknown); // a receive-only stream
    CHECK(s.set_urgency(empty, kQuicStreamUrgencies) == QuicStreamError::Invalid);
    CHECK(c.peek(99U).data.empty());
    CHECK(c.consume(99U, 0U) == QuicStreamError::Unknown);
}

TEST_CASE("QUIC stream set: urgencies go first and streams of one urgency take turns", "[net][4.2a][quic-stream-set]")
{
    QuicStreamLimits wide = limits(4U, 1U, 8192U);
    wide.connection_window = 65536U;
    Pair pair(wide, wide);
    QuicStreamSet& c = pair.end[0];
    u64 ids[3] = {};
    for (u64& id : ids)
    {
        REQUIRE(c.open(true, id) == QuicStreamError::None);
        u64 written = 0U;
        static_cast<void>(write_pattern(c, id, written, 5000U, id));
    }
    REQUIRE(c.set_urgency(ids[2], 0U) == QuicStreamError::None);
    // The urgent stream's data goes first, whole; then the other two alternate packet by packet.
    u64 order[12] = {};
    usize count = 0U;
    for (Packet packet = pair.take(0U); packet.count != 0U && count < 12U; packet = pair.take(0U))
    {
        for (usize index = 0U; index < packet.count && count < 12U; ++index)
        {
            if (packet.records[index].kind == QuicFrameKind::Stream)
            {
                order[count++] = packet.records[index].stream_id;
            }
        }
        REQUIRE(pair.deliver(packet) == QuicTransportError::NoError);
        pair.acked(packet);
    }
    REQUIRE(count >= 8U);
    for (usize index = 0U; index < 5U; ++index)
    {
        CHECK(order[index] == ids[2]);
    }
    CHECK(order[5] != order[6]);
    CHECK(order[6] != order[7]);
}

TEST_CASE("QUIC stream set: many streams cross both ways over an exchange that loses, reorders and duplicates",
          "[net][4.2a][quic-stream-set]")
{
    for (u64 seed = 1U; seed <= 12U; ++seed)
    {
        QuicStreamLimits l = limits(3U, 1U, 1024U);
        l.connection_window = 4096U;
        l.connection_max_window = 32768U;
        auto pair = std::make_unique<Pair>(l, l);
        QuicStreamSet& c = pair->end[0];
        QuicStreamSet& s = pair->end[1];
        Rng rng;
        rng.state = seed;
        u64 size[kEchoStreams] = {};
        u64 sent[kEchoStreams] = {};
        u64 echoed[kEchoStreams] = {};
        u64 server_read[kEchoStreams] = {};
        u64 client_read[kEchoStreams] = {};
        bool done[kEchoStreams] = {};
        usize opened = 0U;
        usize finished = 0U;
        auto flight = std::make_unique<Packet[]>(kEchoFlight);
        usize in_flight = 0U;
        const usize allocations = pair->allocator.calls();
        INFO("seed " << seed);
        for (usize round = 0U; round < 20000U && finished < kEchoStreams; ++round)
        {
            // The client opens what it may, writes, and reads the echoes.
            u64 id = 0U;
            while (opened < kEchoStreams && c.open(true, id) == QuicStreamError::None)
            {
                REQUIRE(id == 4U * opened);
                size[opened] = rng.next() % 4000U;
                ++opened;
            }
            for (usize index = 0U; index < opened; ++index)
            {
                const u64 stream = 4U * index;
                const QuicSendState state = c.send_state(stream);
                if (state == QuicSendState::Ready || state == QuicSendState::Send)
                {
                    static_cast<void>(write_pattern(c, stream, sent[index], size[index], stream));
                }
                if (!done[index] && c.recv_state(stream) != QuicRecvState::None &&
                    drain(c, stream, client_read[index], stream + 1U))
                {
                    CHECK(client_read[index] == size[index]);
                    done[index] = true;
                    ++finished;
                }
            }
            // The server echoes each stream with the other pattern, as fast as it reads.
            QuicStreamEvent event;
            while (s.poll(event))
            {
            }
            while (c.poll(event))
            {
            }
            for (usize index = 0U; index < opened; ++index)
            {
                const u64 stream = 4U * index;
                if (s.recv_state(stream) == QuicRecvState::None)
                {
                    continue;
                }
                const bool ended =
                    s.recv_state(stream) == QuicRecvState::DataRead || drain(s, stream, server_read[index], stream);
                const QuicSendState echo = s.send_state(stream);
                if (echo == QuicSendState::Ready || echo == QuicSendState::Send)
                {
                    static_cast<void>(write_pattern(s, stream, echoed[index], server_read[index], stream + 1U, ended));
                }
            }
            // One packet each way joins the flight; one packet leaves it, out of order, maybe twice or lost.
            for (usize from = 0U; from < 2U; ++from)
            {
                if (in_flight < kEchoFlight && pair->end[from].has_frame())
                {
                    flight[in_flight++] = pair->take(from);
                }
            }
            if (in_flight != 0U)
            {
                const usize pick = static_cast<usize>(rng.next() % in_flight);
                const Packet packet = flight[pick];
                flight[pick] = flight[--in_flight];
                if (rng.chance(150U))
                {
                    pair->lost(packet);
                }
                else
                {
                    REQUIRE(pair->deliver(packet) == QuicTransportError::NoError);
                    if (rng.chance(80U))
                    {
                        REQUIRE(pair->deliver(packet) == QuicTransportError::NoError);
                    }
                    pair->acked(packet);
                }
            }
            pair->now += kMs;
        }
        CHECK(finished == kEchoStreams);
        CHECK(pair->allocator.calls() == allocations);
        CHECK(c.connection_sent() == s.connection_window().received());
        CHECK(s.connection_sent() == c.connection_window().received());
    }
}

TEST_CASE("QUIC stream set: each stream takes the peer's grant for its kind, and events say what changed",
          "[net][4.2a][quic-stream-set]")
{
    // The peer's three per-stream grants differ, so each must reach its own kind of stream (section 18.2).
    CountingAllocator allocator;
    QuicStreamSet c;
    REQUIRE(c.init(allocator, limits(4U, 2U, 4096U), QuicSide::Client));
    QuicTransportParameters peer;
    peer.initial_max_data = 1U << 20U;
    peer.initial_max_streams_bidi = 4U;
    peer.initial_max_streams_uni = 2U;
    peer.initial_max_stream_data_bidi_remote = 100U; // the client's own bidirectional streams
    peer.initial_max_stream_data_bidi_local = 200U;  // the server's bidirectional streams
    peer.initial_max_stream_data_uni = 300U;
    c.set_peer_parameters(peer);
    u64 own = 0U;
    u64 uni = 0U;
    REQUIRE(c.open(true, own) == QuicStreamError::None);
    REQUIRE(c.open(false, uni) == QuicStreamError::None);
    const u8 data[8] = {};
    REQUIRE(frame_to(c, QuicFrameKind::Stream, 1U, 0U, ConstSpan<u8>(data, sizeof(data))) ==
            QuicTransportError::NoError);
    for (const u64 id : {own, uni, u64{1}})
    {
        u64 offset = 0U;
        static_cast<void>(write_pattern(c, id, offset, 1000U, id));
        QuicFrame frame;
        QuicStreamRecord record;
        u64 sent = 0U;
        while (c.next_frame(kRoom, 0U, kRtt, frame, record))
        {
            sent += (frame.kind == QuicFrameKind::Stream) ? frame.data.size() : 0U;
        }
        CHECK(sent == ((id == own) ? 100U : (id == uni) ? 300U : 200U));
    }
    CHECK(c.connection_sent() == 600U);

    // A writer told it is writable only once acknowledgements free room at the front of its buffer.
    QuicStreamLimits wide = limits(2U, 1U, 8192U);
    wide.connection_window = 65536U;
    Pair pair(wide, wide);
    QuicStreamSet& client = pair.end[0];
    QuicStreamSet& server = pair.end[1];
    u64 id = 0U;
    REQUIRE(client.open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    CHECK_FALSE(write_pattern(client, id, written, 9000U, id));
    CHECK(written == 8192U);
    const Packet first = pair.take(0U);
    const Packet second = pair.take(0U);
    u8 mask = 0U;
    pair.acked(second);
    CHECK(events_of(client, id, mask) == 0U); // a gap at the front: no room yet
    pair.acked(first);
    CHECK(events_of(client, id, mask) == 1U);
    CHECK(mask == kQuicStreamWritable);
    // The server hears of it once the data arrives: opened and readable in one event.
    REQUIRE(pair.deliver(second) == QuicTransportError::NoError);
    CHECK(events_of(server, id, mask) == 1U);
    CHECK(mask == kQuicStreamOpened); // nothing readable while the front is missing
    REQUIRE(pair.deliver(first) == QuicTransportError::NoError);
    CHECK(events_of(server, id, mask) == 1U);
    CHECK(mask == kQuicStreamReadable);
}

TEST_CASE("QUIC stream set: DATA_BLOCKED names the connection only when it is what holds data back",
          "[net][4.2a][quic-stream-set]")
{
    QuicStreamLimits tight = limits(2U, 1U, 1024U);
    tight.connection_window = 1024U;
    tight.connection_max_window = 1024U;
    Pair pair(limits(2U, 1U, 1024U), tight);
    QuicStreamSet& c = pair.end[0];
    u64 a = 0U;
    REQUIRE(c.open(true, a) == QuicStreamError::None);
    u64 written = 0U;
    static_cast<void>(write_pattern(c, a, written, 3000U, a));
    usize stream_blocked = 0U;
    usize data_blocked = 0U;
    const auto count = [&](const Packet& packet)
    {
        for (usize index = 0U; index < packet.count; ++index)
        {
            stream_blocked += (packet.records[index].kind == QuicFrameKind::StreamDataBlocked) ? 1U : 0U;
            data_blocked += (packet.records[index].kind == QuicFrameKind::DataBlocked) ? 1U : 0U;
        }
    };
    for (Packet packet = pair.take(0U); packet.count != 0U; packet = pair.take(0U))
    {
        count(packet);
        REQUIRE(pair.deliver(packet) == QuicTransportError::NoError);
        pair.acked(packet);
    }
    // Both credits ran out together: the stream's own limit is what a new credit must lift first.
    CHECK(stream_blocked == 1U);
    CHECK(data_blocked == 0U);
    // A second stream with credit of its own is held back by the connection alone.
    u64 b = 0U;
    REQUIRE(c.open(true, b) == QuicStreamError::None);
    u64 more = 0U;
    static_cast<void>(write_pattern(c, b, more, 100U, b));
    REQUIRE(c.has_frame());
    const Packet blocked = pair.take(0U);
    count(blocked);
    CHECK(data_blocked == 1U);
    CHECK(blocked.count == 1U);

    // A peer that says it is blocked gets MAX_STREAMS as soon as any slot is free, not half of them.
    Pair grants(limits(4U, 1U), limits(4U, 1U));
    u64 id = 0U;
    REQUIRE(grants.end[0].open(true, id) == QuicStreamError::None);
    finish_both(grants, id);
    CHECK_FALSE(grants.end[1].has_frame());
    CHECK(frame_to(grants.end[1], QuicFrameKind::StreamsBlocked, 0U, 4U) == QuicTransportError::NoError);
    const Packet grant = grants.take(1U);
    REQUIRE(grant.count == 1U);
    CHECK(grant.records[0].kind == QuicFrameKind::MaxStreams);
    CHECK(grant.records[0].value == 5U);
}

TEST_CASE("QUIC stream set: remembered parameters for 0-RTT, then the server's, only raise, open streams included; "
          "a rejection resets everything",
          "[net][4.2a][quic-stream-set][quic-early]")
{
    CountingAllocator allocator;
    QuicStreamSet client;
    REQUIRE(client.init(allocator, limits(), QuicSide::Client));
    const usize allocations = allocator.calls();
    QuicTransportParameters remembered;
    remembered.initial_max_data = 16384U;
    remembered.initial_max_stream_data_bidi_remote = 2048U;
    remembered.initial_max_streams_bidi = 1U;
    client.set_peer_parameters(remembered);
    u64 id = 0U;
    REQUIRE(client.open(true, id) == QuicStreamError::None);
    CHECK(client.open(true, id) == QuicStreamError::Blocked);
    u64 offset = 0U;
    CHECK_FALSE(write_pattern(client, 0U, offset, 9000U, 3U, false)); // the send buffer fills
    const auto send_all = [&]
    {
        u64 sent = 0U;
        QuicFrame frame;
        QuicStreamRecord record;
        while (client.next_frame(kRoom, 0U, kRtt, frame, record))
        {
            sent += (frame.kind == QuicFrameKind::Stream) ? frame.data.size() : 0U;
        }
        return sent;
    };
    CHECK(send_all() == 2048U); // the stream's remembered credit

    QuicTransportParameters accepted = remembered;
    accepted.initial_max_stream_data_bidi_remote = 4096U;
    accepted.initial_max_streams_bidi = 3U;
    client.set_peer_parameters(accepted);
    CHECK(send_all() == 2048U); // the open stream's credit rose to the server's value
    CHECK(client.peer_limit(true) == 3U);
    REQUIRE(client.open(true, id) == QuicStreamError::None);
    CHECK(id == 4U);
    client.set_peer_parameters(remembered); // a lower grant never lowers one given
    CHECK(client.peer_limit(true) == 3U);
    CHECK(client.connection_credit().limit() == 16384U);

    // RFC 9001 section 4.6.2: everything starts again, events included.
    client.reset_all();
    CHECK(client.open_streams() == 0U);
    CHECK(client.connection_sent() == 0U);
    CHECK(client.peer_limit(true) == 0U);
    CHECK(client.connection_credit().limit() == 0U);
    CHECK(client.send_state(0U) == QuicSendState::None);
    QuicStreamEvent event;
    CHECK_FALSE(client.poll(event));
    CHECK_FALSE(client.has_frame());
    CHECK(client.open(true, id) == QuicStreamError::Blocked);
    client.set_peer_parameters(accepted);
    REQUIRE(client.open(true, id) == QuicStreamError::None);
    CHECK(id == 0U);
    offset = 0U;
    CHECK(write_pattern(client, 0U, offset, 1000U, 3U));
    CHECK(send_all() == 1000U);
    CHECK(allocator.calls() == allocations);
}

TEST_CASE("QUIC stream set: a server marks the bytes 0-RTT packets carried as replayable, up to their highest end",
          "[net][4.2a][quic-stream-set][quic-early]")
{
    Pair pair(limits(), limits());
    QuicStreamSet& server = pair.end[1];
    u8 data[300];
    for (usize at = 0U; at < sizeof(data); ++at)
    {
        data[at] = pattern(5U, at);
    }
    const auto frame_of = [&](u64 at, usize bytes)
    {
        QuicFrame frame;
        frame.kind = QuicFrameKind::Stream;
        frame.stream_id = 0U;
        frame.offset = at;
        frame.data = ConstSpan<u8>(data + at, bytes);
        return frame;
    };
    REQUIRE(server.on_frame(frame_of(100U, 50U), true) == QuicTransportError::NoError); // 0-RTT, ahead of a gap
    REQUIRE(server.on_frame(frame_of(0U, 100U)) == QuicTransportError::NoError);
    REQUIRE(server.on_frame(frame_of(150U, 150U)) == QuicTransportError::NoError);
    QuicStreamRead read = server.peek(0U);
    REQUIRE(read.data.size() == 300U);
    CHECK(read.early_bytes == 150U); // below the highest 0-RTT end, conservatively
    REQUIRE(server.consume(0U, 120U) == QuicStreamError::None);
    read = server.peek(0U);
    CHECK(read.early_bytes == 30U);
    REQUIRE(server.consume(0U, 30U) == QuicStreamError::None);
    CHECK(server.peek(0U).early_bytes == 0U);
    // Nothing is marked on a stream 0-RTT never reached.
    QuicFrame other = frame_of(0U, 10U);
    other.stream_id = 4U;
    REQUIRE(server.on_frame(other) == QuicTransportError::NoError);
    CHECK(server.peek(4U).early_bytes == 0U);
}

TEST_CASE("QUIC stream set: a reliable reset needs the peer's advertisement and written bytes",
          "[net][4.2a][quic-stream-set]")
{
    QuicStreamLimits in_order = reliable_limits();
    in_order.in_order = true;
    CHECK_FALSE(QuicStreamSet::limits_valid(in_order)); // RESET_STREAM_AT is no frame of QMux
    QuicTransportParameters filled;
    {
        Pair pair(reliable_limits(), reliable_limits());
        pair.end[0].fill_parameters(filled);
        CHECK(filled.reset_stream_at);
        CHECK(pair.end[0].peer_resets_at());
    }
    // A peer that did not advertise it gets a plain RESET_STREAM and nothing else.
    Pair plain(reliable_limits(), limits(2U, 1U, 4096U));
    u64 id = 0U;
    REQUIRE(plain.end[0].open(true, id) == QuicStreamError::None);
    CHECK_FALSE(plain.end[0].peer_resets_at());
    u64 written = 0U;
    static_cast<void>(write_pattern(plain.end[0], id, written, 100U, id, false));
    CHECK(plain.end[0].reset(id, 1U, 10U) == QuicStreamError::Invalid);
    REQUIRE(plain.end[0].reset(id, 1U, 0U) == QuicStreamError::None);
    CHECK(has_kind(plain.take(0U), QuicFrameKind::ResetStream));

    // And this side, having not advertised it, takes the frame as one of no known type.
    CHECK(reset_at_to(plain.end[1], 0U, 1U, 100U, 10U) == QuicTransportError::FrameEncodingError);

    Pair pair(reliable_limits(), reliable_limits());
    REQUIRE(pair.end[0].open(true, id) == QuicStreamError::None);
    written = 0U;
    static_cast<void>(write_pattern(pair.end[0], id, written, 100U, id, false));
    CHECK(pair.end[0].reset(id, 1U, 101U) == QuicStreamError::Invalid);
    CHECK(pair.end[0].send_state(id) == QuicSendState::Ready); // refused, nothing changed
    CHECK(pair.end[0].reset(4096U, 1U, 1U) == QuicStreamError::Unknown);
}

TEST_CASE("QUIC stream set: a reliable reset keeps sending and resending its bytes and no others",
          "[net][4.2a][quic-stream-set]")
{
    Pair pair(reliable_limits(), reliable_limits());
    QuicStreamSet& c = pair.end[0];
    QuicStreamSet& s = pair.end[1];
    u64 id = 0U;
    REQUIRE(c.open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    static_cast<void>(write_pattern(c, id, written, 3000U, id, false));
    const Packet first = pair.take(0U, 1000U); // about 980 bytes, lost below
    const u64 sent = first.records[0].length;
    REQUIRE(first.records[0].kind == QuicFrameKind::Stream);
    REQUIRE(sent > 500U);

    // Part of what was sent is kept: the frame can go at once, and only the kept bytes are resent.
    REQUIRE(c.reset(id, 0x55U, 200U) == QuicStreamError::None);
    CHECK(c.send_state(id) == QuicSendState::ResetSent);
    CHECK(c.reset(id, 0x56U, 100U) == QuicStreamError::Invalid); // the code never changes
    CHECK(c.reset(id, 0x55U, 201U) == QuicStreamError::Invalid); // and the size only falls
    usize taken = 0U;
    CHECK(c.write(id, ConstSpan<u8>(), false, taken) == QuicStreamError::Ended);
    pair.lost(first);
    Packet at = pair.take(0U);
    REQUIRE(at.count == 2U);
    CHECK(at.records[0].kind == QuicFrameKind::ResetStreamAt);
    CHECK(at.records[0].value == 200U);
    CHECK(at.records[1].kind == QuicFrameKind::Stream);
    CHECK(at.records[1].value == 0U);
    CHECK(at.records[1].length == 200U);
    CHECK_FALSE(c.has_frame());
    pair.lost(at); // both go again
    at = pair.take(0U);
    REQUIRE(at.count == 2U);
    REQUIRE(pair.deliver(at) == QuicTransportError::NoError);

    // The acknowledgement of a stale copy of the data above the size means nothing.
    pair.end[0].on_acked(first.records[0]);
    CHECK(c.send_state(id) == QuicSendState::ResetSent); // the frame is not yet acknowledged
    QuicStreamRecord frame_record = at.records[0];
    c.on_acked(frame_record);
    CHECK(c.send_state(id) == QuicSendState::ResetRecvd);

    // The receiver read to the reliable size, then sees the reset; its credit counts the whole final size.
    CHECK(s.recv_state(id) == QuicRecvState::DataRecvd);
    QuicStreamRead read = s.peek(id);
    CHECK_FALSE(read.fin);
    CHECK_FALSE(read.reset);
    REQUIRE(read.data.size() == 200U);
    for (usize index = 0U; index < read.data.size(); ++index)
    {
        REQUIRE(read.data[index] == pattern(id, index));
    }
    CHECK(s.consume(id, 201U) == QuicStreamError::Invalid);
    REQUIRE(s.consume(id, 200U) == QuicStreamError::None);
    CHECK(s.recv_state(id) == QuicRecvState::ResetRecvd);
    CHECK(s.connection_window().consumed() == sent);
    read = s.peek(id);
    CHECK(read.reset);
    CHECK(read.reset_error == 0x55U);
    u8 mask = 0U;
    CHECK(events_of(s, id, mask) >= 1U);
    CHECK((mask & kQuicStreamReset) != 0U);
    CHECK(s.recv_state(id) == QuicRecvState::ResetRead);
}

TEST_CASE("QUIC stream set: reliable bytes never sent go first, within credit, and the frame names them",
          "[net][4.2a][quic-stream-set]")
{
    QuicStreamLimits narrow = reliable_limits();
    narrow.stream_window = 64U;
    Pair pair(narrow, narrow);
    QuicStreamSet& c = pair.end[0];
    QuicStreamSet& s = pair.end[1];
    u64 id = 0U;
    REQUIRE(c.open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    static_cast<void>(write_pattern(c, id, written, 500U, id, false));
    REQUIRE(c.reset(id, 7U, 300U) == QuicStreamError::None);

    // The window lets 64 bytes go: the frame waits, because its final size of 300 would pass the credit.
    Packet packet = pair.take(0U);
    CHECK_FALSE(has_kind(packet, QuicFrameKind::ResetStreamAt));
    CHECK(has_kind(packet, QuicFrameKind::Stream));
    REQUIRE(pair.deliver(packet) == QuicTransportError::NoError);
    pair.acked(packet);
    CHECK(c.send_state(id) == QuicSendState::ResetSent);

    // Credit arrives as the receiver reads; the reset goes with the last reliable byte's flight.
    u64 got = 0U;
    bool frame_seen = false;
    for (usize round = 0U; round < 64U && !frame_seen; ++round)
    {
        for (QuicStreamRead read = s.peek(id); !read.data.empty(); read = s.peek(id))
        {
            for (usize index = 0U; index < read.data.size(); ++index)
            {
                REQUIRE(read.data[index] == pattern(id, got + index));
            }
            got += read.data.size();
            REQUIRE(s.consume(id, read.data.size()) == QuicStreamError::None);
        }
        for (const usize from : {usize{1U}, usize{0U}})
        {
            packet = pair.take(from);
            frame_seen = frame_seen || has_kind(packet, QuicFrameKind::ResetStreamAt);
            REQUIRE(pair.deliver(packet) == QuicTransportError::NoError);
            pair.acked(packet);
        }
        pair.now += kRtt;
    }
    REQUIRE(frame_seen);
    pair.settle();
    for (QuicStreamRead read = s.peek(id); !read.data.empty(); read = s.peek(id))
    {
        got += read.data.size();
        REQUIRE(s.consume(id, read.data.size()) == QuicStreamError::None);
    }
    CHECK(got == 300U);
    CHECK(s.peek(id).reset);
    CHECK(s.peek(id).reset_error == 7U);
    CHECK(c.send_state(id) == QuicSendState::ResetRecvd);
    CHECK(c.connection_sent() == 300U);
    CHECK(s.connection_window().consumed() == 300U);
}

TEST_CASE("QUIC stream set: a reliable size only falls, with the final size what was sent",
          "[net][4.2a][quic-stream-set]")
{
    Pair pair(reliable_limits(), reliable_limits());
    QuicStreamSet& c = pair.end[0];
    QuicStreamSet& s = pair.end[1];
    u64 id = 0U;
    REQUIRE(c.open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    static_cast<void>(write_pattern(c, id, written, 1000U, id, false));
    const Packet data = pair.take(0U, 300U); // bytes 0 to some 280 sent, none acknowledged
    const u64 sent = data.records[0].length;
    REQUIRE(c.reset(id, 9U, 800U) == QuicStreamError::None);

    // Lowered before any frame went, below what was sent: the final size is still every byte sent.
    REQUIRE(c.reset(id, 9U, 100U) == QuicStreamError::None);
    CHECK(c.reset(id, 9U, 101U) == QuicStreamError::Invalid);
    Packet first = pair.take(0U);
    REQUIRE(has_kind(first, QuicFrameKind::ResetStreamAt));
    REQUIRE(first.records[0].kind == QuicFrameKind::ResetStreamAt);
    WireReader in(ConstSpan<u8>(first.bytes, first.size));
    QuicFrame frame;
    REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
    CHECK(frame.value == sent);
    CHECK(frame.offset == 100U);
    CHECK(frame.error_code == 9U);

    // Lowered again after the frame went: a new frame; the old one's acknowledgement no longer settles the part and
    // the loss of the old one sends nothing.
    REQUIRE(c.reset(id, 9U, 40U) == QuicStreamError::None);
    const Packet lowered = pair.take(0U);
    REQUIRE(has_kind(lowered, QuicFrameKind::ResetStreamAt));
    pair.lost(first);
    pair.acked(data);
    pair.acked(first);
    CHECK(c.send_state(id) == QuicSendState::ResetSent);
    CHECK_FALSE(c.has_frame());
    pair.acked(lowered);
    CHECK(c.send_state(id) == QuicSendState::ResetRecvd);

    // The receiver takes the frames in the other order: the larger size that comes later is ignored.
    REQUIRE(pair.deliver(data) == QuicTransportError::NoError);
    REQUIRE(pair.deliver(lowered) == QuicTransportError::NoError);
    REQUIRE(pair.deliver(first) == QuicTransportError::NoError);
    CHECK(s.peek(id).data.size() == 40U);
    CHECK(s.consume(id, 41U) == QuicStreamError::Invalid);
    REQUIRE(s.consume(id, 40U) == QuicStreamError::None);
    CHECK(s.peek(id).reset);
    CHECK(s.connection_window().consumed() == sent);
}

TEST_CASE("QUIC stream set: STOP_SENDING gives up a reliable reset's bytes", "[net][4.2a][quic-stream-set]")
{
    Pair pair(reliable_limits(), reliable_limits());
    QuicStreamSet& c = pair.end[0];
    u64 id = 0U;
    REQUIRE(c.open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    static_cast<void>(write_pattern(c, id, written, 600U, id, false));
    const Packet data = pair.take(0U);
    REQUIRE(c.reset(id, 3U, 500U) == QuicStreamError::None);
    const Packet at = pair.take(0U);
    REQUIRE(has_kind(at, QuicFrameKind::ResetStreamAt));
    CHECK(frame_to(c, QuicFrameKind::StopSending, id) == QuicTransportError::NoError);
    pair.lost(data); // nothing is resent: the size is now zero
    const Packet zero = pair.take(0U);
    REQUIRE(zero.count == 1U);
    CHECK(zero.records[0].kind == QuicFrameKind::ResetStreamAt);
    CHECK(zero.records[0].value == 0U);
    pair.acked(zero);
    CHECK(c.send_state(id) == QuicSendState::ResetRecvd);
}

TEST_CASE("QUIC stream set: every receiver refusal and reordering of RESET_STREAM_AT", "[net][4.2a][quic-stream-set]")
{
    Pair pair(reliable_limits(), reliable_limits());
    QuicStreamSet& s = pair.end[1];
    const u8 bytes[16] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U};
    const ConstSpan<u8> data(bytes, sizeof(bytes));

    SECTION("a changed code is STREAM_STATE_ERROR, a changed final size FINAL_SIZE_ERROR")
    {
        REQUIRE(reset_at_to(s, 0U, 5U, 100U, 16U) == QuicTransportError::NoError);
        CHECK(reset_at_to(s, 0U, 6U, 100U, 16U) == QuicTransportError::StreamStateError);
        CHECK(frame_to(s, QuicFrameKind::ResetStream, 0U, 100U) == QuicTransportError::StreamStateError); // code 0
        CHECK(reset_at_to(s, 0U, 5U, 101U, 16U) == QuicTransportError::FinalSizeError);
        CHECK(reset_at_to(s, 0U, 5U, 99U, 16U) == QuicTransportError::FinalSizeError);
    }
    SECTION("the final size passes the flow-control credit")
    {
        CHECK(reset_at_to(s, 0U, 5U, 4097U, 0U) == QuicTransportError::FlowControlError);
    }
    SECTION("the final size must match a FIN, and the frame after the FIN turns the end into a reset")
    {
        REQUIRE(frame_to(s, QuicFrameKind::Stream, 0U, 0U, data, true) == QuicTransportError::NoError);
        CHECK(reset_at_to(s, 0U, 5U, 15U, 8U) == QuicTransportError::FinalSizeError);
        REQUIRE(reset_at_to(s, 0U, 5U, 16U, 8U) == QuicTransportError::NoError);
        QuicStreamRead read = s.peek(0U);
        CHECK(read.data.size() == 8U);
        CHECK_FALSE(read.fin);
        REQUIRE(s.consume(0U, 8U) == QuicStreamError::None);
        CHECK(s.peek(0U).reset);
    }
    SECTION("a unidirectional stream of this side's receives none")
    {
        CHECK(reset_at_to(s, 3U, 5U, 0U, 0U) == QuicTransportError::StreamStateError);
    }
    SECTION("a reliable size of zero, or one already read, shows the reset at once")
    {
        REQUIRE(frame_to(s, QuicFrameKind::Stream, 0U, 0U, data) == QuicTransportError::NoError);
        REQUIRE(s.consume(0U, 8U) == QuicStreamError::None);
        REQUIRE(reset_at_to(s, 0U, 5U, 100U, 8U) == QuicTransportError::NoError);
        CHECK(s.recv_state(0U) == QuicRecvState::ResetRecvd);
        CHECK(s.connection_window().consumed() == 100U);
    }
    SECTION("bytes in a gap are waited for, up to the reliable size and no further")
    {
        REQUIRE(reset_at_to(s, 0U, 5U, 100U, 32U) == QuicTransportError::NoError);
        CHECK(s.recv_state(0U) == QuicRecvState::SizeKnown);
        CHECK(s.peek(0U).data.empty());
        CHECK_FALSE(s.peek(0U).reset);
        REQUIRE(frame_to(s, QuicFrameKind::Stream, 0U, 0U, data.first(8U), false, 16U) == QuicTransportError::NoError);
        CHECK(s.peek(0U).data.empty()); // a gap below it
        REQUIRE(frame_to(s, QuicFrameKind::Stream, 0U, 0U, data, false, 0U) == QuicTransportError::NoError);
        CHECK(s.peek(0U).data.size() == 24U);
        u8 before[64] = {};
        bool stops = true;
        CHECK(s.copy_readable(0U, 0U, Span<u8>(before, sizeof(before)), stops) == 24U);
        CHECK_FALSE(stops); // bytes it keeps have yet to arrive
        CHECK(s.recv_state(0U) == QuicRecvState::SizeKnown);
        REQUIRE(frame_to(s, QuicFrameKind::Stream, 0U, 0U, data, false, 24U) == QuicTransportError::NoError);
        CHECK(s.recv_state(0U) == QuicRecvState::DataRecvd);
        CHECK(s.peek(0U).data.size() == 32U); // bytes above the reliable size arrived and are not offered
        u8 copy[64] = {};
        bool ends = true;
        CHECK(s.copy_readable(0U, 4U, Span<u8>(copy, sizeof(copy)), ends) == 28U);
        CHECK(ends); // the reliable size is the end of what can ever be read
        CHECK(s.copy_readable(0U, 40U, Span<u8>(copy, sizeof(copy)), ends) == 0U);
        REQUIRE(s.consume(0U, 32U) == QuicStreamError::None);
        CHECK(s.recv_state(0U) == QuicRecvState::ResetRecvd);
    }
    SECTION("a later RESET_STREAM stands for a reliable size of zero")
    {
        REQUIRE(frame_to(s, QuicFrameKind::Stream, 0U, 0U, data) == QuicTransportError::NoError);
        REQUIRE(reset_at_to(s, 0U, 0U, 100U, 16U) == QuicTransportError::NoError);
        CHECK(s.peek(0U).data.size() == 16U);
        REQUIRE(frame_to(s, QuicFrameKind::ResetStream, 0U, 100U) == QuicTransportError::NoError); // code 0, as before
        CHECK(s.peek(0U).reset);
        CHECK(s.connection_window().consumed() == 100U);
    }
    SECTION("a plain RESET_STREAM first: a RESET_STREAM_AT after it delivers nothing more")
    {
        REQUIRE(frame_to(s, QuicFrameKind::ResetStream, 0U, 100U) == QuicTransportError::NoError);
        REQUIRE(reset_at_to(s, 0U, 0U, 100U, 16U) == QuicTransportError::NoError);
        CHECK(s.peek(0U).reset);
        CHECK(reset_at_to(s, 0U, 1U, 100U, 16U) == QuicTransportError::StreamStateError);
    }
    SECTION("STOP_SENDING drops the reliable bytes too")
    {
        REQUIRE(frame_to(s, QuicFrameKind::Stream, 0U, 0U, data) == QuicTransportError::NoError);
        REQUIRE(s.stop_sending(0U, 1U) == QuicStreamError::None);
        REQUIRE(reset_at_to(s, 0U, 5U, 100U, 90U) == QuicTransportError::NoError);
        CHECK(s.recv_state(0U) == QuicRecvState::ResetRecvd);
        CHECK(s.connection_window().consumed() == 100U);
    }
}

TEST_CASE("QUIC stream set: reliable resets survive an exchange that loses, reorders and duplicates",
          "[net][4.2a][quic-stream-set]")
{
    for (u64 seed = 1U; seed <= 32U; ++seed)
    {
        Rng rng{seed};
        Pair pair(reliable_limits(), reliable_limits());
        QuicStreamSet& c = pair.end[0];
        QuicStreamSet& s = pair.end[1];
        u64 id = 0U;
        REQUIRE(c.open(true, id) == QuicStreamError::None);
        u64 written = 0U;
        static_cast<void>(write_pattern(c, id, written, 2000U + (rng.next() % 4000U), id, false));
        const Packet early = pair.take(0U, 500U);
        REQUIRE(pair.deliver(early) == QuicTransportError::NoError);
        const u64 reliable = 1U + (rng.next() % written);
        REQUIRE(c.reset(id, 0x11U, reliable) == QuicStreamError::None);
        pair.acked(early);

        Packet flight[16];
        usize held = 0U;
        u64 got = 0U;
        for (usize round = 0U; round < 20000U && c.send_state(id) != QuicSendState::ResetRecvd; ++round)
        {
            if (held < 16U && c.has_frame())
            {
                flight[held++] = pair.take(0U, 400U + (rng.next() % 800U));
            }
            if (held != 0U && (rng.chance(500U) || held == 16U))
            {
                const usize index = rng.next() % held;
                if (rng.chance(200U))
                {
                    pair.lost(flight[index]);
                }
                else
                {
                    REQUIRE(pair.deliver(flight[index]) == QuicTransportError::NoError);
                    if (rng.chance(200U))
                    {
                        REQUIRE(pair.deliver(flight[index]) == QuicTransportError::NoError);
                    }
                    pair.acked(flight[index]);
                }
                flight[index] = flight[--held];
            }
            for (QuicStreamRead read = s.peek(id); !read.data.empty(); read = s.peek(id))
            {
                for (usize at = 0U; at < read.data.size(); ++at)
                {
                    REQUIRE(read.data[at] == pattern(id, got + at));
                }
                got += read.data.size();
                REQUIRE(s.consume(id, read.data.size()) == QuicStreamError::None);
            }
            while (s.has_frame())
            {
                const Packet window = pair.take(1U);
                REQUIRE(pair.deliver(window) == QuicTransportError::NoError);
                pair.acked(window);
            }
            pair.now += kMs;
        }
        REQUIRE(c.send_state(id) == QuicSendState::ResetRecvd);
        for (usize index = 0U; index < held; ++index)
        {
            REQUIRE(pair.deliver(flight[index]) == QuicTransportError::NoError); // stale copies change nothing
        }
        for (QuicStreamRead read = s.peek(id); !read.data.empty(); read = s.peek(id))
        {
            got += read.data.size();
            REQUIRE(s.consume(id, read.data.size()) == QuicStreamError::None);
        }
        // Bytes above the size that were read before the frame arrived are legal (section 5); none after it.
        CHECK(got >= reliable);
        CHECK(got <= ((reliable > early.records[0].length) ? reliable : u64{early.records[0].length}));
        CHECK(s.peek(id).reset);
        CHECK(s.peek(id).reset_error == 0x11U);
    }
}

TEST_CASE("QUIC stream set: a transport that acknowledges by taking a frame passes 4 GiB on one stream",
          "[net][4.2a][quic-stream-set]")
{
    // on_all_acked() once named the whole stream in a 32-bit length, so a stream past 4 GiB stopped being acknowledged.
    QuicStreamLimits l = limits(1U, 0U, 1U << 20U);
    l.receive_bytes = 1U << 20U;
    l.send_bytes = 1U << 20U;
    l.connection_window = u64{1} << 33U;
    l.connection_max_window = l.connection_window;
    l.in_order = true;
    CountingAllocator allocator;
    QuicStreamSet set;
    REQUIRE(set.init(allocator, l, QuicSide::Client));
    QuicTransportParameters peer;
    peer.initial_max_data = u64{1} << 33U;
    peer.initial_max_stream_data_bidi_remote = u64{1} << 33U;
    peer.initial_max_streams_bidi = 1U;
    set.set_peer_parameters(peer);
    u64 id = 0U;
    REQUIRE(set.open(true, id) == QuicStreamError::None);
    constexpr usize piece_bytes = 1U << 20U;
    auto chunk = std::make_unique<u8[]>(piece_bytes);
    const u64 total = (u64{1} << 32U) + (u64{1} << 21U);
    u64 sent = 0U;
    for (u64 at = 0U; at < total; at += piece_bytes)
    {
        usize taken = 0U;
        REQUIRE(set.write(id, ConstSpan<u8>(chunk.get(), piece_bytes), false, taken) == QuicStreamError::None);
        REQUIRE(taken == piece_bytes); // room came back
        QuicFrame frame;
        QuicStreamRecord record;
        while (set.next_frame(65536U, 1000U, kRtt, frame, record))
        {
            sent += frame.data.size();
        }
        set.on_all_acked();
    }
    CHECK(sent == total);
}

TEST_CASE("QUIC stream set: a reliable reset's bytes owe STREAM_DATA_BLOCKED, and a layer can give them up",
          "[net][4.2a][quic-stream-set]")
{
    QuicStreamLimits narrow = reliable_limits();
    narrow.stream_window = 64U;
    Pair pair(narrow, narrow);
    QuicStreamSet& c = pair.end[0];
    QuicStreamSet& s = pair.end[1];
    u64 id = 0U;
    REQUIRE(c.open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    static_cast<void>(write_pattern(c, id, written, 500U, id, false));
    REQUIRE(c.reset(id, 7U, 300U) == QuicStreamError::None);
    const Packet first = pair.take(0U); // the 64 bytes the window allows, and the frame that says it is blocked
    CHECK(has_kind(first, QuicFrameKind::Stream));
    CHECK(has_kind(first, QuicFrameKind::StreamDataBlocked));
    CHECK_FALSE(has_kind(first, QuicFrameKind::ResetStreamAt));

    // The receiver's layer reads 10 bytes, learns a reset is coming, and gives the rest up.
    REQUIRE(pair.deliver(first) == QuicTransportError::NoError);
    CHECK_FALSE(s.peek(id).reset_coming);
    REQUIRE(s.consume(id, 10U) == QuicStreamError::None);
    CHECK(s.give_up_reliable(id) == QuicStreamError::Ended); // none arrived yet
    REQUIRE(reset_at_to(s, id, 7U, 64U, 64U) == QuicTransportError::NoError);
    u8 mask = 0U;
    CHECK(events_of(s, id, mask) >= 1U);
    CHECK((mask & kQuicStreamReadable) != 0U);
    CHECK(s.peek(id).reset_coming);
    REQUIRE(s.give_up_reliable(id) == QuicStreamError::None);
    CHECK(s.peek(id).reset);
    CHECK(s.connection_window().consumed() == 64U);
    CHECK(s.give_up_reliable(id) == QuicStreamError::Ended);
    CHECK(s.give_up_reliable(8U) == QuicStreamError::Unknown);
}

TEST_CASE("QUIC stream set: RESET_STREAM_AT after a FIN that was all read changes nothing and the frame is noticed",
          "[net][4.2a][quic-stream-set]")
{
    Pair pair(reliable_limits(), reliable_limits());
    QuicStreamSet& s = pair.end[1];
    const u8 bytes[4] = {1U, 2U, 3U, 4U};
    REQUIRE(frame_to(s, QuicFrameKind::Stream, 0U, 0U, ConstSpan<u8>(bytes, 4U), true) == QuicTransportError::NoError);
    u8 mask = 0U;
    static_cast<void>(events_of(s, 0U, mask));
    // With the FIN read to the end the part is over, and a late frame with the same sizes is ignored.
    REQUIRE(s.consume(0U, 4U) == QuicStreamError::None);
    REQUIRE(s.recv_state(0U) == QuicRecvState::DataRead);
    REQUIRE(reset_at_to(s, 0U, 5U, 4U, 2U) == QuicTransportError::NoError);
    CHECK(s.recv_state(0U) == QuicRecvState::DataRead);
    CHECK(events_of(s, 0U, mask) == 0U);
}
