// crd-net -- QUIC byte streams and flow control (4.2a; docs/design/network-foundation.md#quic-streams): reassembly by
// offset with overlaps, gaps, duplicates and changed bytes (RFC 9000 section 2.2), the window and the final size
// (section 4.5), data held until acknowledged with lost data resent first and the FIN, the credit a peer grants and
// its blocked frames (section 4.1), the window this side grants with its updates, auto-tuning and lost updates
// (sections 4.2 and 13.3), and a stream carried end to end over a path that loses, reorders and duplicates.

#include <crd/net/quic_header.hpp>
#include <crd/net/quic_stream.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::net;

constexpr u64   kMs       = 1000000U;
constexpr u64   kTotal    = 6000U; // the stream carried end to end
constexpr usize kInFlight = 64U;   // packets the path holds

template <usize Capacity> struct Receiver
{
    u8                buffer_bytes[Capacity] = {};
    u64               bits[quic_stream_bitmap_words(Capacity)] = {};
    QuicReceiveBuffer buffer;

    Receiver() { REQUIRE(buffer.init(Span<u8>(buffer_bytes, Capacity), Span<u64>(bits, sizeof(bits) / 8U))); }

    QuicReceiveResult put(u64 offset, ConstSpan<u8> data, bool fin = false)
    {
        return buffer.insert(offset, data, fin);
    }
};

template <usize Capacity> struct Sender
{
    u8             buffer_bytes[Capacity] = {};
    u64            acked[quic_stream_bitmap_words(Capacity)] = {};
    u64            lost[quic_stream_bitmap_words(Capacity)]  = {};
    QuicSendBuffer buffer;

    Sender()
    {
        REQUIRE(buffer.init(Span<u8>(buffer_bytes, Capacity), Span<u64>(acked, sizeof(acked) / 8U),
                            Span<u64>(lost, sizeof(lost) / 8U)));
    }

    QuicSendChunk send(usize max_bytes, u64 limit = kQuicMaxVarint)
    {
        const QuicSendChunk chunk = buffer.next(max_bytes, limit);
        buffer.on_sent(chunk);
        return chunk;
    }
};

// The pattern byte at stream offset `offset`.
u8 pattern(u64 offset)
{
    return static_cast<u8>((offset * 131U) ^ (offset >> 8U));
}

struct Pattern
{
    u8 bytes[8192] = {};

    explicit Pattern(u64 from = 0U)
    {
        for (usize i = 0U; i < sizeof(bytes); ++i)
        {
            bytes[i] = pattern(from + i);
        }
    }
    [[nodiscard]] ConstSpan<u8> at(usize from, usize count) const { return ConstSpan<u8>(bytes + from, count); }
};

// Reads everything readable and checks it against the pattern.
template <usize Capacity> u64 drain(Receiver<Capacity>& receiver)
{
    u64 read = 0U;
    for (ConstSpan<u8> view = receiver.buffer.readable(); !view.empty(); view = receiver.buffer.readable())
    {
        const u64 from = receiver.buffer.read_offset();
        for (usize i = 0U; i < view.size(); ++i)
        {
            REQUIRE(view[i] == pattern(from + i));
        }
        receiver.buffer.consume(view.size());
        read += view.size();
    }
    return read;
}

// A deterministic generator for the path.
struct Lcg
{
    u64 state;
    u64 next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 33U;
    }
    bool percent(u64 p) { return next() % 100U < p; }
};
} // namespace

TEST_CASE("quic receive buffer: storage is a multiple of a bitmap word", "[net][quic][stream]")
{
    u8                bytes[128] = {};
    u64               bits[2]    = {};
    QuicReceiveBuffer buffer;
    CHECK(buffer.insert(0U, {}, false) == QuicReceiveResult::Unusable);
    CHECK_FALSE(buffer.init(Span<u8>(bytes, 0U), Span<u64>(bits, 0U)));
    CHECK_FALSE(buffer.init(Span<u8>(bytes, 100U), Span<u64>(bits, 1U)));
    CHECK_FALSE(buffer.init(Span<u8>(bytes, 128U), Span<u64>(bits, 1U)));
    CHECK(buffer.insert(0U, ConstSpan<u8>(bytes, 1U), false) == QuicReceiveResult::Unusable);
    CHECK(buffer.readable().empty());
    buffer.consume(1U);
    bits[0] = ~u64{0};
    REQUIRE(buffer.init(Span<u8>(bytes, 128U), Span<u64>(bits, 2U)));
    CHECK(bits[0] == 0U); // init forgets what the storage held
    CHECK(buffer.capacity() == 128U);
}

TEST_CASE("quic receive buffer: in order, out of order, gaps and duplicates", "[net][quic][stream]")
{
    const Pattern  data;
    Receiver<256U> receiver;
    QuicReceiveBuffer& buffer = receiver.buffer;

    CHECK(receiver.put(0U, data.at(0U, 10U)) == QuicReceiveResult::Accepted);
    CHECK(buffer.contiguous_end() == 10U);
    CHECK(receiver.put(20U, data.at(20U, 10U)) == QuicReceiveResult::Accepted);
    CHECK(buffer.contiguous_end() == 10U); // a gap
    CHECK(buffer.received_end() == 30U);
    CHECK(buffer.readable().size() == 10U);
    CHECK(receiver.put(5U, data.at(5U, 20U)) == QuicReceiveResult::Accepted); // overlaps both sides of the gap
    CHECK(buffer.contiguous_end() == 30U);
    CHECK(receiver.put(0U, data.at(0U, 30U)) == QuicReceiveResult::Accepted); // a duplicate
    CHECK(drain(receiver) == 30U);
    CHECK(buffer.read_offset() == 30U);
    CHECK(receiver.put(0U, data.at(0U, 40U)) == QuicReceiveResult::Accepted); // part already read
    CHECK(buffer.contiguous_end() == 40U);
    CHECK(drain(receiver) == 10U);
    CHECK(receiver.put(90U, {}) == QuicReceiveResult::Accepted); // empty data still counts its offset
    CHECK(buffer.received_end() == 90U);
    CHECK(buffer.contiguous_end() == 40U);
}

TEST_CASE("quic receive buffer: other bytes at an offset held are refused", "[net][quic][stream]")
{
    const Pattern  data;
    Receiver<256U> receiver;
    QuicReceiveBuffer& buffer = receiver.buffer;
    REQUIRE(receiver.put(10U, data.at(10U, 10U)) == QuicReceiveResult::Accepted);

    u8 changed[30] = {};
    for (usize i = 0U; i < sizeof(changed); ++i)
    {
        changed[i] = pattern(i);
    }
    changed[15] ^= 0x01U; // inside the held range
    CHECK(receiver.put(0U, ConstSpan<u8>(changed, 30U)) == QuicReceiveResult::Changed);
    // Nothing changed: the data before and after the held range was not taken either.
    CHECK(buffer.contiguous_end() == 0U);
    CHECK(buffer.received_end() == 20U);
    changed[15] ^= 0x01U;
    changed[29] ^= 0x80U; // outside: new data, accepted
    CHECK(receiver.put(0U, ConstSpan<u8>(changed, 30U)) == QuicReceiveResult::Accepted);
    CHECK(receiver.put(29U, data.at(29U, 1U)) == QuicReceiveResult::Changed);
    CHECK(buffer.contiguous_end() == 30U);
    buffer.consume(30U);
    // Read bytes are gone, so other bytes there are not compared.
    CHECK(receiver.put(29U, data.at(29U, 1U)) == QuicReceiveResult::Accepted);
}

TEST_CASE("quic receive buffer: the window and its wrap", "[net][quic][stream]")
{
    const Pattern  data;
    Receiver<128U> receiver;
    QuicReceiveBuffer& buffer = receiver.buffer;
    CHECK(receiver.put(0U, data.at(0U, 129U)) == QuicReceiveResult::BeyondWindow);
    CHECK(receiver.put(128U, {}) == QuicReceiveResult::Accepted);
    CHECK(receiver.put(128U, {}, true) == QuicReceiveResult::Accepted);
    CHECK(buffer.final_size() == 128U);

    Receiver<128U> second;
    CHECK(second.put(100U, data.at(100U, 28U)) == QuicReceiveResult::Accepted);
    CHECK(second.put(0U, data.at(0U, 100U)) == QuicReceiveResult::Accepted);
    second.buffer.consume(90U);
    CHECK(second.put(128U, data.at(128U, 91U)) == QuicReceiveResult::BeyondWindow);
    CHECK(second.put(128U, data.at(128U, 90U)) == QuicReceiveResult::Accepted); // wraps the storage
    CHECK(second.buffer.contiguous_end() == 218U);
    CHECK(second.buffer.readable().size() == 38U); // to the end of the storage
    CHECK(drain(second) == 128U);
    CHECK(second.buffer.read_offset() == 218U);

    Receiver<128U> third;
    CHECK(third.put(kQuicMaxVarint, data.at(0U, 1U)) == QuicReceiveResult::BeyondWindow);
    CHECK(third.put(kQuicMaxVarint + 1U, {}) == QuicReceiveResult::BeyondWindow);
    CHECK(third.buffer.received_end() == 0U);
}

TEST_CASE("quic receive buffer: the final size", "[net][quic][stream]")
{
    const Pattern  data;
    Receiver<256U> receiver;
    QuicReceiveBuffer& buffer = receiver.buffer;
    REQUIRE(receiver.put(0U, data.at(0U, 50U)) == QuicReceiveResult::Accepted);
    CHECK(receiver.put(0U, data.at(0U, 40U), true) == QuicReceiveResult::FinalSize); // below the data received
    CHECK(receiver.put(49U, {}, true) == QuicReceiveResult::FinalSize);               // one byte below it
    CHECK_FALSE(buffer.final_known());
    CHECK(receiver.put(60U, data.at(60U, 10U), true) == QuicReceiveResult::Accepted);
    CHECK(buffer.final_known());
    CHECK(buffer.final_size() == 70U);
    CHECK(receiver.put(60U, data.at(60U, 11U)) == QuicReceiveResult::FinalSize);      // past it
    CHECK(receiver.put(60U, data.at(60U, 9U), true) == QuicReceiveResult::FinalSize); // another one
    CHECK(receiver.put(70U, {}, true) == QuicReceiveResult::Accepted);                // the same, as a reset says it
    CHECK(receiver.put(50U, data.at(50U, 10U)) == QuicReceiveResult::Accepted);
    CHECK_FALSE(buffer.complete());
    CHECK(drain(receiver) == 70U);
    CHECK(buffer.complete());
}

TEST_CASE("quic receive buffer: any order of overlapping pieces rebuilds the stream", "[net][quic][stream]")
{
    const Pattern data;
    Lcg           random{7U};
    for (int round = 0; round < 50; ++round)
    {
        Receiver<1024U> receiver;
        u64             read = 0U;
        for (int piece = 0; piece < 5000 && !receiver.buffer.complete(); ++piece)
        {
            // Pieces start up to 100 bytes before the read offset (data read already, or the stream's start) and
            // anywhere in the window.
            const u64 window_from = receiver.buffer.read_offset();
            const u64 span        = (3000U - window_from < 1024U) ? 3000U - window_from : 1024U;
            const u64 shifted     = window_from + random.next() % (span + 100U);
            const u64 offset      = (shifted > 100U) ? shifted - 100U : 0U;
            const u64   wanted = offset + random.next() % 200U;
            const u64   end    = (wanted > 3000U) ? 3000U : wanted;
            const QuicReceiveResult result = receiver.put(
                offset, data.at(static_cast<usize>(offset), static_cast<usize>(end - offset)), end == 3000U);
            const bool beyond = end > window_from && end - window_from > 1024U;
            CHECK(result == (beyond ? QuicReceiveResult::BeyondWindow : QuicReceiveResult::Accepted));
            if (random.percent(30U))
            {
                read += drain(receiver);
            }
        }
        read += drain(receiver);
        CHECK(read == 3000U);
        CHECK(receiver.buffer.complete());
    }
}

TEST_CASE("quic send buffer: storage, writing and room", "[net][quic][stream]")
{
    u8             bytes[128] = {};
    u64            acked[2]   = {};
    u64            lost[2]    = {};
    QuicSendBuffer buffer;
    CHECK(buffer.write(ConstSpan<u8>(bytes, 1U)) == 0U);
    CHECK(buffer.next(10U, kQuicMaxVarint).empty());
    CHECK_FALSE(buffer.init(Span<u8>(bytes, 128U), Span<u64>(acked, 2U), Span<u64>(lost, 1U)));
    CHECK_FALSE(buffer.init(Span<u8>(bytes, 96U), Span<u64>(acked, 1U), Span<u64>(lost, 1U)));
    buffer.finish();
    CHECK_FALSE(buffer.finished());

    const Pattern data;
    Sender<128U>  sender;
    CHECK(sender.buffer.write(data.at(0U, 100U)) == 100U);
    CHECK(sender.buffer.write(data.at(100U, 100U)) == 28U);
    CHECK(sender.buffer.room() == 0U);
    const QuicSendChunk chunk = sender.send(64U);
    CHECK(chunk.offset == 0U);
    CHECK(chunk.length == 64U);
    sender.buffer.on_acked(10U, 54U, false); // not from the start: no room yet
    CHECK(sender.buffer.room() == 0U);
    CHECK(sender.buffer.acked_end() == 0U);
    sender.buffer.on_acked(0U, 10U, false);
    CHECK(sender.buffer.acked_end() == 64U);
    CHECK(sender.buffer.room() == 64U);
    CHECK(sender.buffer.write(data.at(128U, 64U)) == 64U); // wraps the storage
    const QuicSendChunk rest = sender.send(1000U);
    CHECK(rest.offset == 64U);
    CHECK(rest.length == 64U); // a chunk stops at the end of the storage
    const QuicSendChunk wrapped = sender.send(1000U);
    CHECK(wrapped.offset == 128U);
    CHECK(wrapped.length == 64U);
    const ConstSpan<u8> view = sender.buffer.bytes_of(wrapped);
    REQUIRE(view.size() == 64U);
    for (usize i = 0U; i < view.size(); ++i)
    {
        CHECK(view[i] == pattern(128U + i));
    }
    CHECK(sender.buffer.bytes_of(QuicSendChunk{}).empty());
    sender.buffer.finish();
    CHECK(sender.buffer.write(data.at(0U, 1U)) == 0U);
}

TEST_CASE("quic send buffer: flow control holds fresh data, never lost data or the FIN alone", "[net][quic][stream]")
{
    const Pattern data;
    Sender<256U>  sender;
    REQUIRE(sender.buffer.write(data.at(0U, 100U)) == 100U);
    sender.buffer.finish();
    const QuicSendChunk first = sender.send(1000U, 60U);
    CHECK(first.length == 60U);
    CHECK_FALSE(first.fin);
    CHECK(sender.buffer.sent_end() == 60U);
    CHECK(sender.buffer.next(1000U, 60U).empty()); // blocked
    CHECK(sender.buffer.pending());
    sender.buffer.on_lost(0U, 60U, false);
    CHECK(sender.buffer.lost_bytes() == 60U);
    const QuicSendChunk again = sender.send(1000U, 60U); // lost data needs no new credit
    CHECK(again.offset == 0U);
    CHECK(again.length == 60U);
    CHECK(sender.buffer.lost_bytes() == 0U);
    const QuicSendChunk last = sender.send(1000U, 100U);
    CHECK(last.offset == 60U);
    CHECK(last.length == 40U);
    CHECK(last.fin); // with the last byte
    CHECK_FALSE(sender.buffer.pending());
    CHECK(sender.buffer.next(1000U, 100U).empty());
}

TEST_CASE("quic send buffer: lost data goes first, lowest first, and acknowledged copies stay acknowledged",
          "[net][quic][stream]")
{
    const Pattern data;
    Sender<512U>  sender;
    REQUIRE(sender.buffer.write(data.at(0U, 400U)) == 400U);
    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(sender.send(100U).length == 100U);
    }
    sender.buffer.on_lost(300U, 100U, false);
    sender.buffer.on_lost(100U, 100U, false);
    sender.buffer.on_acked(150U, 20U, false); // another copy of part of it arrived
    CHECK(sender.buffer.lost_bytes() == 180U);
    QuicSendChunk chunk = sender.send(1000U);
    CHECK(chunk.offset == 100U);
    CHECK(chunk.length == 50U);
    chunk = sender.send(1000U);
    CHECK(chunk.offset == 170U);
    CHECK(chunk.length == 30U);
    chunk = sender.send(10U); // trimmed to the room
    CHECK(chunk.offset == 300U);
    CHECK(chunk.length == 10U);
    sender.buffer.on_acked(0U, 400U, false);
    CHECK(sender.buffer.lost_bytes() == 0U);
    CHECK(sender.buffer.acked_end() == 400U);
    sender.buffer.on_lost(0U, 400U, false); // late, spurious: changes nothing
    CHECK(sender.buffer.lost_bytes() == 0U);
    sender.buffer.on_acked(400U, 100U, false); // never sent
    CHECK(sender.buffer.acked_end() == 400U);
    CHECK_FALSE(sender.buffer.pending());
    CHECK(sender.buffer.room() == 512U);

    // A loss reported after another copy of some of its bytes was acknowledged leaves those bytes acknowledged.
    REQUIRE(sender.buffer.write(data.at(400U, 100U)) == 100U);
    REQUIRE(sender.send(100U).length == 100U);
    sender.buffer.on_acked(450U, 20U, false);
    sender.buffer.on_lost(400U, 100U, false);
    CHECK(sender.buffer.lost_bytes() == 80U);
    sender.buffer.on_acked(400U, 100U, false);
    CHECK(sender.buffer.acked_end() == 500U);
    CHECK(sender.buffer.lost_bytes() == 0U);
}

TEST_CASE("quic send buffer: the FIN is sent, lost, resent alone and acknowledged", "[net][quic][stream]")
{
    const Pattern data;
    Sender<128U>  sender;
    REQUIRE(sender.buffer.write(data.at(0U, 20U)) == 20U);
    const QuicSendChunk body = sender.send(1000U);
    CHECK_FALSE(body.fin);
    sender.buffer.finish();
    CHECK(sender.buffer.write(data.at(20U, 1U)) == 0U); // nothing after the FIN, though there is room
    CHECK(sender.buffer.pending());
    const QuicSendChunk fin = sender.send(1000U, 0U); // a FIN alone takes no credit
    CHECK(fin.fin);
    CHECK(fin.length == 0U);
    CHECK(fin.offset == 20U);
    CHECK_FALSE(sender.buffer.pending());
    sender.buffer.on_acked(0U, 20U, false);
    CHECK_FALSE(sender.buffer.all_acked());
    sender.buffer.on_lost(20U, 0U, false); // the right range without the FIN
    CHECK_FALSE(sender.buffer.pending());
    sender.buffer.on_lost(20U, 0U, true);
    CHECK(sender.buffer.pending());
    const QuicSendChunk resent = sender.send(1000U);
    CHECK(resent.fin);
    CHECK(resent.offset == 20U);
    sender.buffer.on_acked(10U, 5U, true); // a FIN that is not at the final size
    CHECK_FALSE(sender.buffer.all_acked());
    sender.buffer.on_acked(20U, 0U, true);
    CHECK(sender.buffer.all_acked());
    sender.buffer.on_lost(20U, 0U, true); // after its acknowledgement
    CHECK_FALSE(sender.buffer.pending());
    CHECK(sender.buffer.all_acked());

    Sender<128U> empty;
    empty.buffer.finish();
    const QuicSendChunk only = empty.send(0U);
    CHECK(only.fin);
    CHECK(only.offset == 0U);
}

TEST_CASE("quic send credit: the limit only rises and a blocked frame is owed once per limit", "[net][quic][stream]")
{
    QuicSendCredit credit;
    credit.reset(100U);
    CHECK(credit.available(40U) == 60U);
    CHECK(credit.available(140U) == 0U);
    credit.raise(80U);
    CHECK(credit.limit() == 100U);
    CHECK_FALSE(credit.blocked_owed(99U, true));
    CHECK_FALSE(credit.blocked_owed(100U, false)); // nothing waits
    CHECK(credit.blocked_owed(100U, true));
    credit.on_blocked_sent();
    CHECK_FALSE(credit.blocked_owed(100U, true));
    credit.on_blocked_lost(90U); // another limit's
    CHECK_FALSE(credit.blocked_owed(100U, true));
    credit.on_blocked_lost(100U);
    CHECK(credit.blocked_owed(100U, true));
    credit.on_blocked_sent();
    credit.raise(200U);
    CHECK_FALSE(credit.blocked_owed(150U, true));
    CHECK(credit.blocked_owed(200U, true));
    credit.on_blocked_lost(100U); // the old limit's, superseded
    credit.on_blocked_sent();
    CHECK_FALSE(credit.blocked_owed(200U, true));
}

TEST_CASE("quic receive window: limits, updates, auto-tuning and lost updates", "[net][quic][stream]")
{
    QuicReceiveWindow window;
    window.reset(1000U, 4000U);
    CHECK(window.limit() == 1000U);
    CHECK(window.on_received(1000U));
    CHECK_FALSE(window.on_received(1001U));
    CHECK(window.received() == 1000U);
    window.on_consumed(499U);
    CHECK_FALSE(window.update_owed());
    window.on_consumed(500U);
    CHECK(window.update_owed());
    CHECK(window.take_update(0U, 100U * kMs) == 1500U);
    CHECK_FALSE(window.update_owed());

    // An update within two round trips of the last doubles the window.
    window.on_consumed(1000U);
    REQUIRE(window.update_owed());
    CHECK(window.take_update(150U * kMs, 100U * kMs) == 3000U);
    CHECK(window.window() == 2000U);
    window.on_consumed(2000U);
    CHECK(window.take_update(250U * kMs, 100U * kMs) == 6000U); // up to the maximum
    CHECK(window.window() == 4000U);
    window.on_consumed(4000U);
    CHECK(window.take_update(300U * kMs, 100U * kMs) == 8000U);
    CHECK(window.window() == 4000U);
    // A slow reader keeps the window.
    window.on_consumed(6000U);
    CHECK(window.take_update(1000U * kMs, 100U * kMs) == 10000U);
    CHECK(window.window() == 4000U);

    // A lost update is owed again while it is the latest.
    window.on_update_lost(8000U);
    CHECK_FALSE(window.update_owed());
    window.on_update_lost(10000U);
    CHECK(window.update_owed());
    CHECK(window.take_update(5000U * kMs, 100U * kMs) == 10000U);
    CHECK_FALSE(window.update_owed());
    // Resent at once, within two round trips: the same limit, and the window is not tuned by it.
    window.on_update_lost(10000U);
    CHECK(window.take_update(5001U * kMs, 100U * kMs) == 10000U);
    CHECK(window.window() == 4000U);
    CHECK_FALSE(window.update_owed());

    // A blocked peer gets an update only when it would raise the limit.
    window.on_peer_blocked();
    CHECK_FALSE(window.update_owed());
    window.on_consumed(6001U);
    CHECK(window.update_owed());
    CHECK(window.take_update(9000U * kMs, 100U * kMs) == 10001U);

    // Consumption is bounded by the limit and never goes back.
    window.on_consumed(20000U);
    CHECK(window.consumed() == 10001U);
    window.on_consumed(5U);
    CHECK(window.consumed() == 10001U);

    QuicReceiveWindow none;
    none.reset(0U, 0U);
    none.on_peer_blocked();
    CHECK_FALSE(none.update_owed());
    CHECK_FALSE(none.on_received(1U));
    QuicReceiveWindow huge;
    huge.reset(kQuicMaxVarint, kQuicMaxVarint);
    huge.on_consumed(kQuicMaxVarint);
    CHECK_FALSE(huge.update_owed()); // the limit cannot pass 2^62 - 1
}

TEST_CASE("quic stream: delivered whole over a path that loses, reorders and duplicates", "[net][quic][stream]")
{
    const Pattern data;

    struct Packet
    {
        QuicSendChunk chunk;
        u8            bytes[200] = {};
        bool          used = false;
        bool          delivered = false;
    };

    for (u64 seed = 1U; seed <= 20U; ++seed)
    {
        Lcg               random{seed};
        Sender<1024U>     sender;
        Receiver<1024U>   receiver;
        QuicSendCredit    credit;
        QuicReceiveWindow window;
        window.reset(512U, 1024U);
        credit.reset(window.limit());
        Packet packets[kInFlight];
        u64    written = 0U;
        u64    read    = 0U;
        int    steps   = 0;

        while (!(sender.buffer.all_acked() && receiver.buffer.complete()) && steps < 200000)
        {
            ++steps;
            if (written < kTotal)
            {
                const usize want = static_cast<usize>((kTotal - written < 300U) ? kTotal - written : 300U);
                written += sender.buffer.write(data.at(static_cast<usize>(written), want));
                if (written == kTotal)
                {
                    sender.buffer.finish();
                }
            }
            // Send one packet into a free slot.
            Packet& slot = packets[random.next() % kInFlight];
            if (!slot.used)
            {
                const QuicSendChunk chunk = sender.buffer.next(1U + random.next() % 200U, credit.limit());
                if (!chunk.empty())
                {
                    const ConstSpan<u8> view = sender.buffer.bytes_of(chunk);
                    for (usize i = 0U; i < view.size(); ++i)
                    {
                        slot.bytes[i] = view[i];
                    }
                    slot.chunk     = chunk;
                    slot.used      = true;
                    slot.delivered = false;
                    sender.buffer.on_sent(chunk);
                }
            }
            // Deliver, duplicate, lose or acknowledge a packet in any order.
            Packet& pick = packets[random.next() % kInFlight];
            if (pick.used)
            {
                const u64 roll = random.next() % 100U;
                if (roll < 15U)
                {
                    sender.buffer.on_lost(pick.chunk.offset, pick.chunk.length, pick.chunk.fin);
                    pick.used = false;
                }
                else if (roll < 70U)
                {
                    const QuicReceiveResult result = receiver.put(
                        pick.chunk.offset, ConstSpan<u8>(pick.bytes, pick.chunk.length), pick.chunk.fin);
                    REQUIRE(result == QuicReceiveResult::Accepted);
                    REQUIRE(window.on_received(receiver.buffer.received_end()));
                    pick.delivered = true;
                }
                else if (pick.delivered)
                {
                    sender.buffer.on_acked(pick.chunk.offset, pick.chunk.length, pick.chunk.fin);
                    pick.used = false;
                }
            }
            if (random.percent(20U))
            {
                read += drain(receiver);
                window.on_consumed(receiver.buffer.read_offset());
                if (window.update_owed())
                {
                    credit.raise(window.take_update(static_cast<u64>(steps), 10U));
                }
            }
        }
        read += drain(receiver);
        CHECK(read == kTotal);
        CHECK(receiver.buffer.complete());
        CHECK(sender.buffer.all_acked());
        CHECK(sender.buffer.sent_end() == kTotal);
    }
}
