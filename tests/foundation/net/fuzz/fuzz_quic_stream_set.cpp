// crd-fuzz-net-quic-stream-set: a QUIC connection's streams in crd-net (4.2a;
// docs/design/network-foundation.md#quic-stream-set) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: limits for the two ends (stream slots, windows), then operations, each an operation byte and the parameter
// bytes it reads (0 past the end), on a client's and a server's stream sets: opens, writes with and without a FIN,
// reads, resets, STOP_SENDING, urgencies and polls by either application; packets taken from either end into a
// bounded flight, delivered in any order and any number of times, acknowledged or declared lost; and any frame bytes
// injected into either end as if from its peer.
//
// Oracles. has_frame() is exact: next_frame() gives a frame exactly when it said so, so a connection never spins on
// work it cannot do. Every frame given fits its room, is what write_quic_frame writes and reads back. Frames an end
// wrote never make its peer close (unless that peer was given injected frames), every byte read is the byte its
// writer wrote at that offset, the receiver never counts more than the sender sent, open streams stay within the
// slots, and nothing allocates after init().
#include <crd/containers/wire_bytes.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/quic_stream_set.hpp>

#include <new>

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

constexpr usize kRoom = 1200U;
constexpr usize kRecords = 48U;
constexpr usize kFlight = 16U;
constexpr u64 kIds = 64U; // per type: the stream indices the oracle follows
constexpr u64 kRttNs = 20000000U;

struct Input
{
    const u8* data;
    usize size;
    usize at = 0U;

    [[nodiscard]] bool more() const { return at < size; }
    u8 byte() { return (at < size) ? data[at++] : u8{0}; }
};

u8 pattern(usize writer, u64 id, u64 offset)
{
    return static_cast<u8>((offset * 29U) + (id * 13U) + (writer * 101U) + (offset >> 9U));
}

struct Packet
{
    u8 bytes[kRoom] = {};
    usize size = 0U;
    QuicStreamRecord records[kRecords];
    usize count = 0U;
    usize from = 0U;
};

struct World
{
    crd::fuzz::BudgetAllocator allocator;
    QuicStreamSet end[2];
    Packet flight[kFlight];
    usize in_flight = 0U;
    u64 written[2][4][kIds] = {}; // per writer, type and index: the bytes written
    u64 read[2][4][kIds] = {};    // per reader
    bool tainted[2] = {};         // given injected frames: may close
    bool closed[2] = {};
    u32 slots[2] = {};
    u64 now = 1000000000U;
};

// A stream ID of either initiator and kind among the indices the oracle follows.
u64 id_of(u8 pick)
{
    return (u64{static_cast<u8>(pick >> 2U) % kIds} << 2U) | (pick & 3U);
}

void take(World& w, usize from, usize room)
{
    if (w.closed[from] || w.in_flight == kFlight)
    {
        return;
    }
    QuicStreamSet& set = w.end[from];
    const bool has = set.has_frame();
    Packet& packet = w.flight[w.in_flight];
    packet = Packet{};
    packet.from = from;
    QuicFrame frame;
    QuicStreamRecord record;
    bool first = true;
    while (packet.count < kRecords && set.next_frame(room - packet.size, w.now, kRttNs, frame, record))
    {
        first = false;
        const usize bytes = write_quic_frame(frame, Span<u8>(packet.bytes + packet.size, room - packet.size));
        CRD_FUZZ_REQUIRE(bytes != 0U && bytes == quic_frame_bytes(frame));
        WireReader back(ConstSpan<u8>(packet.bytes + packet.size, bytes));
        QuicFrame reread;
        CRD_FUZZ_REQUIRE(read_quic_frame(back, reread) == QuicTransportError::NoError && back.done());
        CRD_FUZZ_REQUIRE(reread.kind == frame.kind && reread.stream_id == frame.stream_id);
        packet.size += bytes;
        packet.records[packet.count++] = record;
    }
    // With a whole packet's room, has_frame() promised exactly what came.
    CRD_FUZZ_REQUIRE(room < kRoom || has == !first);
    if (packet.count != 0U)
    {
        ++w.in_flight;
    }
}

void deliver(World& w, usize index)
{
    const Packet& packet = w.flight[index];
    const usize to = 1U - packet.from;
    if (w.closed[to])
    {
        return;
    }
    WireReader in(ConstSpan<u8>(packet.bytes, packet.size));
    while (!in.done())
    {
        QuicFrame frame;
        CRD_FUZZ_REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
        if (w.end[to].on_frame(frame) != QuicTransportError::NoError)
        {
            CRD_FUZZ_REQUIRE(w.tainted[to] || w.tainted[packet.from]);
            w.closed[to] = true;
            return;
        }
    }
}

void settle(World& w, Input& in, bool acked)
{
    if (w.in_flight == 0U)
    {
        return;
    }
    const usize index = in.byte() % w.in_flight;
    const Packet& packet = w.flight[index];
    if (!w.closed[packet.from])
    {
        for (usize at = 0U; at < packet.count; ++at)
        {
            if (acked)
            {
                w.end[packet.from].on_acked(packet.records[at]);
            }
            else
            {
                w.end[packet.from].on_lost(packet.records[at]);
            }
        }
    }
    w.flight[index] = w.flight[--w.in_flight];
}

void write(World& w, Input& in, usize side)
{
    const u64 id = id_of(in.byte());
    const usize length = in.byte() * 4U;
    const bool fin = (in.byte() & 1U) != 0U;
    u64& offset = w.written[side][id & 3U][id >> 2U];
    u8 chunk[1024];
    for (usize at = 0U; at < length; ++at)
    {
        chunk[at] = pattern(side, id, offset + at);
    }
    usize taken = 0U;
    const QuicStreamError error = w.end[side].write(id, ConstSpan<u8>(chunk, length), fin, taken);
    CRD_FUZZ_REQUIRE(error == QuicStreamError::None || taken == 0U);
    CRD_FUZZ_REQUIRE(taken <= length);
    offset += taken;
}

void read(World& w, Input& in, usize side)
{
    const u64 id = id_of(in.byte());
    const QuicStreamRead peek = w.end[side].peek(id);
    const usize want = (peek.data.size() == 0U) ? 0U : in.byte() % (peek.data.size() + 1U);
    u64& offset = w.read[side][id & 3U][id >> 2U];
    if (!w.tainted[side])
    {
        for (usize at = 0U; at < want; ++at)
        {
            CRD_FUZZ_REQUIRE(peek.data[at] == pattern(1U - side, id, offset + at));
        }
    }
    const QuicStreamError error = w.end[side].consume(id, want);
    CRD_FUZZ_REQUIRE(error != QuicStreamError::Invalid);
    if (error == QuicStreamError::None)
    {
        offset += want;
    }
}

void inject(World& w, Input& in, usize side)
{
    // Up to 40 bytes as a frame from the peer: whatever read_quic_frame accepts goes to the end.
    u8 bytes[40];
    const usize size = in.byte() % (sizeof(bytes) + 1U);
    for (usize at = 0U; at < size; ++at)
    {
        bytes[at] = in.byte();
    }
    WireReader reader(ConstSpan<u8>(bytes, size));
    QuicFrame frame;
    if (read_quic_frame(reader, frame) != QuicTransportError::NoError)
    {
        return;
    }
    switch (frame.kind)
    {
        case QuicFrameKind::Stream:
        case QuicFrameKind::ResetStream:
        case QuicFrameKind::StopSending:
        case QuicFrameKind::MaxData:
        case QuicFrameKind::MaxStreamData:
        case QuicFrameKind::MaxStreams:
        case QuicFrameKind::DataBlocked:
        case QuicFrameKind::StreamDataBlocked:
        case QuicFrameKind::StreamsBlocked:
            break;
        default:
            return;
    }
    w.tainted[side] = true;
    if (!w.closed[side] && w.end[side].on_frame(frame) != QuicTransportError::NoError)
    {
        w.closed[side] = true;
    }
}

QuicStreamLimits limits_from(Input& in)
{
    QuicStreamLimits l;
    l.peer_bidi = in.byte() % 4U;
    l.peer_uni = in.byte() % 3U;
    l.local_bidi = in.byte() % 4U;
    l.local_uni = in.byte() % 3U;
    l.receive_bytes = 64U * (1U + (in.byte() % 8U));
    l.send_bytes = 64U * (1U + (in.byte() % 8U));
    l.stream_window = 1U + (in.byte() % l.receive_bytes);
    l.connection_window = 64U * in.byte();
    l.connection_max_window = l.connection_window * (1U + (in.byte() % 4U));
    return l;
}

void check(World& w)
{
    for (usize side = 0U; side < 2U; ++side)
    {
        CRD_FUZZ_REQUIRE(w.end[side].open_streams() <= w.slots[side]);
        if (!w.tainted[0] && !w.tainted[1])
        {
            CRD_FUZZ_REQUIRE(w.end[1U - side].connection_window().received() <= w.end[side].connection_sent());
        }
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    alignas(World) static unsigned char storage[sizeof(World)];
    static World* held = nullptr;
    if (held != nullptr)
    {
        held->~World();
    }
    held = new (storage) World;
    World& w = *held;
    Input in{data, size};
    const QuicStreamLimits client = limits_from(in);
    const QuicStreamLimits server = limits_from(in);
    CRD_FUZZ_REQUIRE(QuicStreamSet::limits_valid(client) && QuicStreamSet::limits_valid(server));
    CRD_FUZZ_REQUIRE(w.end[0].init(w.allocator, client, QuicSide::Client));
    CRD_FUZZ_REQUIRE(w.end[1].init(w.allocator, server, QuicSide::Server));
    w.slots[0] = client.peer_bidi + client.peer_uni + client.local_bidi + client.local_uni;
    w.slots[1] = server.peer_bidi + server.peer_uni + server.local_bidi + server.local_uni;
    QuicTransportParameters parameters[2];
    w.end[0].fill_parameters(parameters[0]);
    w.end[1].fill_parameters(parameters[1]);
    w.end[0].set_peer_parameters(parameters[1]);
    w.end[1].set_peer_parameters(parameters[0]);
    const usize live = w.allocator.live_bytes();
    while (in.more())
    {
        const u8 op = in.byte();
        const usize side = (op >> 7U) & 1U;
        QuicStreamSet& set = w.end[side];
        u64 id = 0U;
        QuicStreamEvent event;
        switch (op & 0x0FU)
        {
            case 0U:
            case 1U:
                if (set.open((op & 1U) == 0U, id) == QuicStreamError::None)
                {
                    CRD_FUZZ_REQUIRE(set.send_state(id) == QuicSendState::Ready);
                }
                break;
            case 2U:
            case 3U:
                write(w, in, side);
                break;
            case 4U:
                read(w, in, side);
                break;
            case 5U:
                static_cast<void>(set.reset(id_of(in.byte()), in.byte()));
                break;
            case 6U:
                static_cast<void>(set.stop_sending(id_of(in.byte()), in.byte()));
                break;
            case 7U:
                static_cast<void>(set.set_urgency(id_of(in.byte()), in.byte() % 9U));
                break;
            case 8U:
                while (set.poll(event))
                {
                    CRD_FUZZ_REQUIRE(event.events != 0U);
                }
                break;
            case 9U:
            case 10U:
                take(w, side, (op & 1U) != 0U ? kRoom : 8U + (in.byte() * 4U) % kRoom);
                break;
            case 11U:
                if (w.in_flight != 0U)
                {
                    deliver(w, in.byte() % w.in_flight);
                }
                break;
            case 12U:
                settle(w, in, true);
                break;
            case 13U:
                settle(w, in, false);
                break;
            case 14U:
                inject(w, in, side);
                break;
            default:
                w.now += u64{in.byte()} * 1000000U;
                break;
        }
        check(w);
    }
    CRD_FUZZ_REQUIRE(w.allocator.live_bytes() == live);
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Limits (both ends: 3 bidirectional and 2 unidirectional slots each way, 256-byte buffers, a 128-byte window,
    // a 512-byte connection window), then: the client opens two streams and writes, packets cross, are acknowledged
    // and lost, the server reads and echoes, resets and STOP_SENDING, and an injected frame.
    const u8 limits[] = {3U, 2U, 3U, 2U, 3U, 3U, 127U, 8U, 1U, 3U, 2U, 3U, 2U, 3U, 3U, 127U, 8U, 1U};
    const u8 exchange[] = {0x00U, 0x00U, 0x02U, 0x00U, 0x40U, 0x00U, 0x02U, 0x04U, 0x40U, 0x01U, 0x0AU, 0x0BU,
                           0x00U, 0x0CU, 0x00U, 0x8AU, 0x0BU, 0x00U, 0x0CU, 0x00U, 0x84U, 0x00U, 0xFFU, 0x84U,
                           0x04U, 0xFFU, 0x82U, 0x00U, 0x20U, 0x01U, 0x8AU, 0x0BU, 0x00U, 0x0DU, 0x00U, 0x0AU,
                           0x0BU, 0x00U, 0x0CU, 0x00U, 0x88U, 0x08U, 0x0FU, 0x20U, 0x0AU, 0x0BU, 0x00U, 0x0CU,
                           0x00U, 0x85U, 0x04U, 0x09U, 0x86U, 0x00U, 0x07U, 0x8AU, 0x0BU, 0x00U, 0x0CU, 0x00U};
    u8 seed[sizeof(limits) + sizeof(exchange)];
    for (usize at = 0U; at < sizeof(limits); ++at)
    {
        seed[at] = limits[at];
    }
    for (usize at = 0U; at < sizeof(exchange); ++at)
    {
        seed[sizeof(limits) + at] = exchange[at];
    }
    sink.add("exchange", seed, sizeof(seed));
    // Frames from a hostile peer: a STREAM frame past the grant, a RESET_STREAM and a MAX_STREAMS.
    const u8 hostile[] = {3U,    2U,    3U,    2U,    3U,    3U,    127U,  8U,    1U,    3U,
                          2U,    3U,    2U,    3U,    3U,    127U,  8U,    1U,    0x8EU, 0x04U,
                          0x0AU, 0x40U, 0x01U, 0x8EU, 0x04U, 0x04U, 0x00U, 0x05U, 0x02U, 0x0EU,
                          0x02U, 0x12U, 0x05U, 0x0EU, 0x05U, 0x08U, 0x04U, 0x10U, 0x01U, 0x00U};
    sink.add("hostile", hostile, sizeof(hostile));
    // Many small packets, every one lost once, with credit frames among them.
    u8 losses[sizeof(limits) + 96U];
    for (usize at = 0U; at < sizeof(limits); ++at)
    {
        losses[at] = limits[at];
    }
    const u8 round[] = {0x00U, 0x02U, 0x00U, 0x80U, 0x01U, 0x09U, 0x40U, 0x0DU, 0x00U, 0x0AU, 0x0BU, 0x00U};
    for (usize at = 0U; at < 96U; ++at)
    {
        losses[sizeof(limits) + at] = round[at % sizeof(round)];
    }
    sink.add("losses", losses, sizeof(losses));
}
