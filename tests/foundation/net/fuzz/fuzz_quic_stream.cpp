// crd-fuzz-net-quic-stream: QUIC byte streams and flow control of crd-net (4.2a;
// docs/design/network-foundation.md#quic-streams) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: operations, each an operation byte and the parameter bytes it reads (0 past the end), on one receive buffer
// and one send buffer of 256 bytes and one receive window: frames inserted at any offset (below the read offset,
// past the window, with a FIN, with a changed byte), reads, writes, FINs, chunks sent under a flow-control limit,
// acknowledgements and losses of any sent chunk in any order and any number of times, and window operations.
//
// Oracles, against models that keep one record per stream byte. The receive buffer's verdict is the model's rule
// (final size, then window, then other bytes at a held offset), a refusal changes nothing, and the contiguous end,
// the received end, the final size and the readable bytes are the model's. The send buffer's next chunk is the
// model's (the lowest lost run, else the data never sent within the limit, never wrapping, the FIN with the last
// byte or alone), its bytes are what was written, and the acknowledged end, the lost bytes, the room and the FIN's
// state are the model's. The window's limit never falls, never passes the bytes read plus its maximum, admits every
// end at or below it and refuses every end above it.
#include <crd/fuzz/harness.hpp>
#include <crd/net/quic_header.hpp>
#include <crd/net/quic_stream.hpp>

#include <new>

namespace
{
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::net;

constexpr usize kCapacity   = 256U;
constexpr usize kWords      = quic_stream_bitmap_words(kCapacity);
constexpr u64   kModelBytes = 16384U;
constexpr usize kPackets    = 64U;

u8 receive_pattern(u64 offset)
{
    return static_cast<u8>((offset * 37U) ^ (offset >> 7U));
}

u8 send_pattern(u64 offset)
{
    return static_cast<u8>((offset * 151U) + 11U);
}

struct Input
{
    const u8* data;
    usize     size;
    usize     at = 0U;

    [[nodiscard]] bool more() const { return at < size; }
    u8                 byte() { return (at < size) ? data[at++] : u8{0}; }
};

struct ReceiveModel
{
    bool held[kModelBytes]  = {};
    u8   value[kModelBytes] = {};
    u64  read        = 0U;
    u64  received    = 0U;
    u64  final_size  = 0U;
    bool final_known = false;

    [[nodiscard]] u64 contiguous() const
    {
        u64 at = read;
        while (at < kModelBytes && at < read + kCapacity && held[at])
        {
            ++at;
        }
        return at;
    }
};

struct SendModel
{
    bool acked[kModelBytes] = {};
    bool lost[kModelBytes]  = {};
    u64  written   = 0U;
    u64  sent      = 0U;
    bool finished  = false;
    bool fin_sent  = false;
    bool fin_lost  = false;
    bool fin_acked = false;

    [[nodiscard]] u64 acked_end() const
    {
        u64 at = 0U;
        while (at < sent && acked[at])
        {
            ++at;
        }
        return at;
    }
    [[nodiscard]] u64 lost_count() const
    {
        u64 count = 0U;
        for (u64 i = acked_end(); i < sent; ++i)
        {
            count += lost[i] ? 1U : 0U;
        }
        return count;
    }
    [[nodiscard]] bool fin_owed() const { return finished && !fin_acked && (!fin_sent || fin_lost); }

    // The chunk the send buffer must give, by its rule restated over the bytes.
    [[nodiscard]] QuicSendChunk expected(usize max_bytes, u64 limit) const
    {
        QuicSendChunk chunk;
        const u64     low = acked_end();
        u64           start = low;
        while (start < sent && !lost[start])
        {
            ++start;
        }
        if (start < sent)
        {
            u64 end = start;
            while (end < sent && lost[end] && end - start < max_bytes && (end == start || end % kCapacity != 0U))
            {
                ++end;
            }
            chunk.offset = start;
            chunk.length = static_cast<usize>(end - start);
            chunk.fin    = chunk.length != 0U && fin_owed() && end == written;
            return chunk;
        }
        const u64 top = (limit < written) ? limit : written;
        if (sent < top)
        {
            u64 end = sent;
            while (end < top && end - sent < max_bytes && (end == sent || end % kCapacity != 0U))
            {
                ++end;
            }
            if (end > sent)
            {
                chunk.offset = sent;
                chunk.length = static_cast<usize>(end - sent);
                chunk.fin    = fin_owed() && end == written;
                return chunk;
            }
        }
        if (fin_owed() && sent == written)
        {
            chunk.offset = written;
            chunk.fin    = true;
        }
        return chunk;
    }
};

struct World
{
    u8                receive_bytes[kCapacity] = {};
    u64               receive_bits[kWords]     = {};
    u8                send_bytes[kCapacity]    = {};
    u64               acked_bits[kWords]       = {};
    u64               lost_bits[kWords]        = {};
    QuicReceiveBuffer receive;
    QuicSendBuffer    send;
    QuicReceiveWindow window;
    ReceiveModel      receive_model;
    SendModel         send_model;
    QuicSendChunk     packets[kPackets];
    usize             packet_count = 0U;
    u64               window_max   = 0U;
};

void check_receive(World& w)
{
    const ReceiveModel& model = w.receive_model;
    const u64           contiguous = model.contiguous();
    CRD_FUZZ_REQUIRE(w.receive.read_offset() == model.read);
    CRD_FUZZ_REQUIRE(w.receive.contiguous_end() == contiguous);
    CRD_FUZZ_REQUIRE(w.receive.received_end() == model.received);
    CRD_FUZZ_REQUIRE(w.receive.final_known() == model.final_known);
    CRD_FUZZ_REQUIRE(!model.final_known || w.receive.final_size() == model.final_size);
    CRD_FUZZ_REQUIRE(w.receive.complete() == (model.final_known && model.read == model.final_size));
    const ConstSpan<u8> view = w.receive.readable();
    const u64           wrap = kCapacity - model.read % kCapacity;
    const u64           want = (contiguous - model.read < wrap) ? contiguous - model.read : wrap;
    CRD_FUZZ_REQUIRE(view.size() == want);
    for (usize i = 0U; i < view.size(); ++i)
    {
        CRD_FUZZ_REQUIRE(view[i] == model.value[model.read + i]);
    }
}

void insert(World& w, Input& in, u8 op)
{
    ReceiveModel& model = w.receive_model;
    const u8      a     = in.byte();
    const u8      b     = in.byte();
    const bool    fin   = (op & 0x10U) != 0U;
    const bool    far   = (op & 0x20U) != 0U;
    const bool    flip  = (op & 0x40U) != 0U;
    const u64     base  = (model.read > 32U) ? model.read - 32U : 0U;
    const u64     offset = base + (far ? u64{a} * 4U : u64{a});
    const usize   length = b % 97U;
    const u64     end    = offset + length;
    if (end >= kModelBytes)
    {
        return;
    }
    u8 data[96] = {};
    for (usize i = 0U; i < length; ++i)
    {
        data[i] = receive_pattern(offset + i);
    }
    if (flip && length != 0U)
    {
        data[a % length] ^= 0x5AU;
    }

    QuicReceiveResult expected = QuicReceiveResult::Accepted;
    if (model.final_known ? (end > model.final_size || (fin && end != model.final_size))
                          : (fin && end < model.received))
    {
        expected = QuicReceiveResult::FinalSize;
    }
    else if (end > model.read && end - model.read > kCapacity)
    {
        expected = QuicReceiveResult::BeyondWindow;
    }
    else
    {
        for (u64 i = (offset > model.read) ? offset : model.read; i < end; ++i)
        {
            if (model.held[i] && model.value[i] != data[i - offset])
            {
                expected = QuicReceiveResult::Changed;
            }
        }
    }
    const QuicReceiveResult result = w.receive.insert(offset, ConstSpan<u8>(data, length), fin);
    CRD_FUZZ_REQUIRE(result == expected);
    if (result == QuicReceiveResult::Accepted)
    {
        for (u64 i = (offset > model.read) ? offset : model.read; i < end; ++i)
        {
            model.held[i]  = true;
            model.value[i] = data[i - offset];
        }
        model.received = (end > model.received) ? end : model.received;
        if (fin)
        {
            model.final_known = true;
            model.final_size  = end;
        }
    }
    check_receive(w);
}

void consume(World& w, Input& in)
{
    ReceiveModel& model = w.receive_model;
    const u64     count = in.byte();
    const u64     ready = model.contiguous() - model.read;
    const u64     taken = (count < ready) ? count : ready;
    w.receive.consume(count);
    for (u64 i = model.read; i < model.read + taken; ++i)
    {
        model.held[i] = false;
    }
    model.read += taken;
    check_receive(w);
}

void check_send(World& w)
{
    const SendModel& model = w.send_model;
    CRD_FUZZ_REQUIRE(w.send.written_end() == model.written);
    CRD_FUZZ_REQUIRE(w.send.sent_end() == model.sent);
    CRD_FUZZ_REQUIRE(w.send.acked_end() == model.acked_end());
    CRD_FUZZ_REQUIRE(w.send.lost_bytes() == model.lost_count());
    CRD_FUZZ_REQUIRE(w.send.room() == kCapacity - (model.written - model.acked_end()));
    CRD_FUZZ_REQUIRE(w.send.finished() == model.finished);
    CRD_FUZZ_REQUIRE(w.send.all_acked() == (model.fin_acked && model.acked_end() == model.written));
    CRD_FUZZ_REQUIRE(w.send.pending() == (model.lost_count() > 0U || model.sent < model.written || model.fin_owed()));
}

void write(World& w, Input& in)
{
    SendModel& model = w.send_model;
    u64        count = in.byte();
    if (model.written + count > kModelBytes)
    {
        count = kModelBytes - model.written;
    }
    u8 data[256] = {};
    for (usize i = 0U; i < count; ++i)
    {
        data[i] = send_pattern(model.written + i);
    }
    const u64 room     = model.finished ? 0U : kCapacity - (model.written - model.acked_end());
    const u64 expected = (count < room) ? count : room;
    CRD_FUZZ_REQUIRE(w.send.write(ConstSpan<u8>(data, static_cast<usize>(count))) == expected);
    model.written += expected;
    check_send(w);
}

void send(World& w, Input& in)
{
    SendModel&  model = w.send_model;
    const usize max   = in.byte();
    const u8    l     = in.byte();
    const u64   limit = (l == 0xFFU) ? kQuicMaxVarint : model.sent + l - ((model.sent > 64U) ? 64U : model.sent);
    const QuicSendChunk chunk    = w.send.next(max, limit);
    const QuicSendChunk expected = model.expected(max, limit);
    CRD_FUZZ_REQUIRE(chunk.offset == expected.offset || chunk.empty());
    CRD_FUZZ_REQUIRE(chunk.length == expected.length);
    CRD_FUZZ_REQUIRE(chunk.fin == expected.fin);
    const ConstSpan<u8> view = w.send.bytes_of(chunk);
    CRD_FUZZ_REQUIRE(view.size() == chunk.length);
    for (usize i = 0U; i < view.size(); ++i)
    {
        CRD_FUZZ_REQUIRE(view[i] == send_pattern(chunk.offset + i));
    }
    if (chunk.empty())
    {
        return;
    }
    w.send.on_sent(chunk);
    const u64 end = chunk.offset + chunk.length;
    for (u64 i = chunk.offset; i < end; ++i)
    {
        model.lost[i] = false;
    }
    model.sent = (end > model.sent) ? end : model.sent;
    if (chunk.fin)
    {
        model.fin_sent = true;
        model.fin_lost = false;
    }
    w.packets[w.packet_count % kPackets] = chunk;
    ++w.packet_count;
    check_send(w);
}

void settle(World& w, Input& in, bool acked)
{
    SendModel& model = w.send_model;
    if (w.packet_count == 0U)
    {
        return;
    }
    const usize         held  = (w.packet_count < kPackets) ? w.packet_count : kPackets;
    const QuicSendChunk chunk = w.packets[in.byte() % held];
    const u64           end   = chunk.offset + chunk.length;
    const u64           low   = model.acked_end();
    if (acked)
    {
        w.send.on_acked(chunk.offset, chunk.length, chunk.fin);
        for (u64 i = chunk.offset; i < end; ++i)
        {
            model.acked[i] = true;
            model.lost[i]  = false;
        }
        if (chunk.fin && model.fin_sent && end == model.written)
        {
            model.fin_acked = true;
            model.fin_lost  = false;
        }
    }
    else
    {
        w.send.on_lost(chunk.offset, chunk.length, chunk.fin);
        for (u64 i = (chunk.offset > low) ? chunk.offset : low; i < end; ++i)
        {
            model.lost[i] = !model.acked[i];
        }
        if (chunk.fin && model.fin_sent && !model.fin_acked && end == model.written)
        {
            model.fin_lost = true;
        }
    }
    check_send(w);
}

void window_op(World& w, Input& in)
{
    QuicReceiveWindow& window = w.window;
    const u8           kind   = in.byte();
    const u64          value  = in.byte();
    const u64          before = window.limit();
    switch (kind % 5U)
    {
    case 0U:
    {
        const u64  end      = window.received() + value - 128U * ((value >= 128U) ? 1U : 0U);
        const bool admitted = window.on_received(end);
        CRD_FUZZ_REQUIRE(admitted == (end <= before));
        break;
    }
    case 1U:
        window.on_consumed(window.consumed() + value);
        CRD_FUZZ_REQUIRE(window.consumed() <= window.limit());
        break;
    case 2U:
        window.on_peer_blocked();
        break;
    case 3U:
        if (window.update_owed())
        {
            const u64 limit = window.take_update(value * 1000000U, 50000000U);
            CRD_FUZZ_REQUIRE(limit == window.limit());
            CRD_FUZZ_REQUIRE(limit == window.consumed() + window.window() || limit == before);
            CRD_FUZZ_REQUIRE(!window.update_owed());
        }
        break;
    default:
        window.on_update_lost((value & 1U) != 0U ? window.limit() : value);
        break;
    }
    CRD_FUZZ_REQUIRE(window.limit() >= before);
    CRD_FUZZ_REQUIRE(window.limit() <= window.consumed() + w.window_max);
    CRD_FUZZ_REQUIRE(window.window() <= w.window_max);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    // One world per input, in static storage: the models are too large for the stack.
    alignas(World) static unsigned char storage[sizeof(World)];
    static World*                       held = nullptr;
    if (held != nullptr)
    {
        held->~World();
    }
    held     = new (storage) World;
    World* w = held;
    CRD_FUZZ_REQUIRE(w->receive.init(Span<u8>(w->receive_bytes, kCapacity), Span<u64>(w->receive_bits, kWords)));
    CRD_FUZZ_REQUIRE(w->send.init(Span<u8>(w->send_bytes, kCapacity), Span<u64>(w->acked_bits, kWords),
                                  Span<u64>(w->lost_bits, kWords)));
    Input in{data, size};
    w->window_max = 64U + in.byte() * 4U;
    w->window.reset(32U + in.byte(), w->window_max);
    w->window_max = (w->window.window() > w->window_max) ? w->window.window() : w->window_max;
    while (in.more())
    {
        const u8 op = in.byte();
        switch (op & 0x0FU)
        {
        case 0U:
            insert(*w, in, op);
            break;
        case 1U:
            consume(*w, in);
            break;
        case 2U:
            write(*w, in);
            break;
        case 3U:
            w->send.finish();
            w->send_model.finished = true;
            check_send(*w);
            break;
        case 4U:
            send(*w, in);
            break;
        case 5U:
            settle(*w, in, true);
            break;
        case 6U:
            settle(*w, in, false);
            break;
        default:
            window_op(*w, in);
            break;
        }
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Window bytes, then operations. A stream received out of order with a gap, a duplicate, a changed byte, a FIN
    // and reads across the storage's end.
    const u8 receive[] = {0x10U, 0x40U, 0x00U, 0x40U, 0x30U, 0x00U, 0x20U, 0x40U, 0x00U, 0x00U, 0x50U,
                          0x40U, 0x50U, 0x20U, 0x01U, 0xFFU, 0x00U, 0x40U, 0x60U, 0x01U, 0xFFU, 0x10U,
                          0x60U, 0x10U, 0x01U, 0xFFU, 0x30U, 0x50U, 0x30U, 0x01U, 0xFFU};
    sink.add("receive-gaps-fin", receive, sizeof(receive));
    // A stream sent under a limit with losses, spurious losses, acknowledgements out of order and a lost FIN.
    const u8 sending[] = {0x10U, 0x40U, 0x02U, 0xC8U, 0x04U, 0x40U, 0xFFU, 0x04U, 0x40U, 0x50U, 0x06U, 0x00U,
                          0x04U, 0x80U, 0xFFU, 0x05U, 0x01U, 0x03U, 0x04U, 0xFFU, 0xFFU, 0x06U, 0x02U, 0x04U,
                          0xFFU, 0xFFU, 0x05U, 0x00U, 0x05U, 0x03U, 0x06U, 0x01U, 0x05U, 0x02U, 0x02U, 0xFFU,
                          0x04U, 0xFFU, 0xFFU, 0x05U, 0x04U};
    sink.add("send-loss-fin", sending, sizeof(sending));
    // The window read, updated, tuned, blocked and its update lost.
    const u8 window[] = {0x08U, 0x20U, 0x07U, 0x00U, 0x30U, 0x07U, 0x01U, 0x30U, 0x07U, 0x03U, 0x01U, 0x07U,
                         0x01U, 0x40U, 0x07U, 0x03U, 0x02U, 0x07U, 0x04U, 0x01U, 0x07U, 0x03U, 0x03U, 0x07U,
                         0x02U, 0x00U, 0x07U, 0x01U, 0x01U, 0x07U, 0x03U, 0x04U, 0x07U, 0x00U, 0xFFU};
    sink.add("window", window, sizeof(window));
}
