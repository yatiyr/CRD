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
