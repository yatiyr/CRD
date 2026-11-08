// crd-fuzz-net-quic-datagram: a QUIC connection's datagrams in crd-net (4.2a;
// docs/design/network-foundation.md#quic-datagrams) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: four bytes of limits (the send ring, send_count, outcomes and an age limit, the receive ring), then
// operations, each an operation byte and the parameter bytes it reads (0 past the end): queue a payload, write the
// next frame into a room of any size and keep it in a bounded flight, deliver a flight frame to the receiver (any
// number of times), acknowledge or lose it, poll reports, read and pop, advance time, and inject any frame bytes into
// the receiver.
//
// Oracles. has_frame() is exact: a room of a whole packet gets a frame exactly when it said so. Every frame written
// fits its room, reads back as a DATAGRAM with its Length and holds the payload queued under its id; the receiver
// accepts every frame the sender wrote and holds what it took in order, byte-exact; an injected frame is refused
// exactly when it passes the size advertised. The accounting holds: unreported() is the datagrams queued, and with
// outcomes those in flight and the reports not polled; every id is reported at most once, Acked or Lost only after
// it was written, Expired only unwritten; after a final drain every id is accounted for; nothing allocates after
// init().
#include <crd/containers/wire_bytes.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/quic_datagram.hpp>

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
using namespace crd::net;

constexpr usize kRoom = 1200U; // the packet: what has_frame() promises a frame fits
constexpr usize kFlight = 8U;  // frames written and not settled
constexpr usize kIds = 512U;   // datagrams the oracle follows
constexpr usize kHeld = 64U;   // the receiver's ring at most
constexpr u64 kMsNs = 1000000U;

struct Input
{
    const u8* data;
    usize size;
    usize at = 0U;

    [[nodiscard]] bool more() const { return at < size; }
    u8 byte() { return (at < size) ? data[at++] : u8{0}; }
};

u8 pattern(u64 id, usize offset)
{
    return static_cast<u8>((id * 151U) + (offset * 11U) + 3U);
}

enum class Fate : u8
{
    Unknown, // not queued
    Queued,
    Written,
    Settled, // its packet was acknowledged or lost
    Reported,
};

struct Frame
{
    u8 bytes[kRoom] = {};
    usize size = 0U;
    u64 id = 0U;
    bool live = false;
};

class Run
{
public:
    void go(Input& in)
    {
        m_in = &in;
        const u8 shape = m_in->byte();
        const u8 count = m_in->byte();
        const u8 age = m_in->byte();
        const u8 ring = m_in->byte();
        QuicDatagramLimits send;
        send.send_bytes = 64U + (u32{shape & 0x3FU} * 64U); // 64 to 4096
        send.send_count = 1U + (u32{count} % 16U);
        send.outcomes = (shape & 0x40U) != 0U;
        send.max_age_ns = ((age & 0x80U) != 0U) ? u64{age & 0x7FU} * kMsNs : 0U;
        QuicDatagramLimits receive;
        receive.max_frame_bytes = 2U + (u32{ring} * 5U); // 2 to 1277
        receive.receive_bytes = receive.max_frame_bytes - 1U + ((u32{count} >> 4U) * 97U);
        receive.receive_count = 1U + (u32{ring} % static_cast<u32>(kHeld - 1U));
        CRD_FUZZ_REQUIRE(QuicDatagrams::limits_valid(send) && QuicDatagrams::limits_valid(receive));
        CRD_FUZZ_REQUIRE(m_sender.init(m_allocator, send));
        CRD_FUZZ_REQUIRE(m_receiver.init(m_allocator, receive));
        const usize live = m_allocator.live_bytes();
        m_sender.set_peer(receive.max_frame_bytes, kRoom + kQuicShortPacketOverhead);
        m_outcomes = send.outcomes;
        while (m_in->more())
        {
            step(m_in->byte());
            check_accounting();
        }
        drain();
        CRD_FUZZ_REQUIRE(m_allocator.live_bytes() == live);
    }

private:
    void step(u8 operation)
    {
        switch (operation % 9U)
        {
            case 0U:
            case 1U:
                queue();
                break;
            case 2U:
                write(true);
                break;
            case 3U:
                write(false);
                break;
            case 4U:
                deliver(m_in->byte() % kFlight);
                break;
            case 5U:
                settle(m_in->byte());
                break;
            case 6U:
                poll();
                read();
                break;
            case 7U:
                m_now += u64{m_in->byte()} * kMsNs;
                break;
            default:
                inject();
                break;
        }
    }

    void queue()
    {
        const usize wanted = (usize{m_in->byte()} << 8U) | m_in->byte();
        const usize bytes = wanted % (m_sender.max_payload() + 2U); // one past the largest
        if (m_queued == kIds)
        {
            return;
        }
        u8 payload[kRoom + 1U];
        for (usize offset = 0U; offset < bytes; ++offset)
        {
            payload[offset] = pattern(m_queued, offset);
        }
        u64 id = 0U;
        const QuicDatagramError error = m_sender.queue(ConstSpan<u8>(payload, bytes), m_now, id);
        expire_model();
        if (bytes > m_sender.max_payload())
        {
            CRD_FUZZ_REQUIRE(error == QuicDatagramError::TooLarge);
            return;
        }
        CRD_FUZZ_REQUIRE(error == QuicDatagramError::None || error == QuicDatagramError::Full);
        if (error == QuicDatagramError::None)
        {
            CRD_FUZZ_REQUIRE(id == m_queued);
            m_size[m_queued] = bytes;
            m_fate[m_queued++] = Fate::Queued;
        }
    }

    // The model of expiry: what the sender dropped at m_now, oldest first, is what is past the age limit.
    void expire_model()
    {
        for (; m_oldest < m_queued && m_fate[m_oldest] != Fate::Queued; ++m_oldest)
        {
        }
        const u64 expired = m_sender.counters().expired;
        for (; m_expired < expired; ++m_expired)
        {
            CRD_FUZZ_REQUIRE(m_oldest < m_queued && m_fate[m_oldest] == Fate::Queued);
            m_fate[m_oldest] = m_outcomes ? Fate::Settled : Fate::Reported;
            m_expired_ids[m_oldest] = true;
            for (++m_oldest; m_oldest < m_queued && m_fate[m_oldest] != Fate::Queued; ++m_oldest)
            {
            }
        }
    }

    void write(bool whole)
    {
        usize slot = kFlight;
        for (usize index = 0U; index < kFlight && slot == kFlight; ++index)
        {
            slot = m_flight[index].live ? kFlight : index;
        }
        if (slot == kFlight)
        {
            return;
        }
        Frame& frame = m_flight[slot];
        const usize room = whole ? kRoom : (usize{m_in->byte()} * 5U) % (kRoom + 1U);
        const bool promised = m_sender.has_frame(m_now);
        u64 id = kIds;
        frame.size = m_sender.write_next(Span<u8>(frame.bytes, room), m_now, id);
        expire_model();
        if (whole)
        {
            CRD_FUZZ_REQUIRE((frame.size != 0U) == promised);
        }
        if (frame.size == 0U)
        {
            return;
        }
        CRD_FUZZ_REQUIRE(frame.size <= room && id < m_queued && m_fate[id] == Fate::Queued);
        WireReader in(ConstSpan<u8>(frame.bytes, frame.size));
        QuicFrame read;
        CRD_FUZZ_REQUIRE(read_quic_frame(in, read) == QuicTransportError::NoError && in.done());
        CRD_FUZZ_REQUIRE(read.kind == QuicFrameKind::Datagram && !read.to_end && read.data.size() == m_size[id]);
        for (usize offset = 0U; offset < read.data.size(); ++offset)
        {
            CRD_FUZZ_REQUIRE(read.data[offset] == pattern(id, offset));
        }
        m_fate[id] = m_outcomes ? Fate::Written : Fate::Reported;
        frame.id = id;
        frame.live = true;
    }

    void deliver(usize slot)
    {
        const Frame& frame = m_flight[slot];
        if (frame.size == 0U)
        {
            return;
        }
        WireReader in(ConstSpan<u8>(frame.bytes, frame.size));
        QuicFrame read;
        CRD_FUZZ_REQUIRE(read_quic_frame(in, read) == QuicTransportError::NoError);
        const u64 received = m_receiver.counters().received;
        CRD_FUZZ_REQUIRE(m_receiver.on_frame(read) == QuicTransportError::NoError);
        if (m_receiver.counters().received != received)
        {
            CRD_FUZZ_REQUIRE(m_held_count < kHeld);
            m_held[(m_held_first + m_held_count++) % kHeld] = frame.id;
        }
    }

    void settle(u8 choice)
    {
        Frame& frame = m_flight[choice % kFlight];
        if (!frame.live)
        {
            return;
        }
        frame.live = false;
        if ((choice & 0x80U) != 0U)
        {
            m_sender.on_acked(frame.id);
        }
        else
        {
            m_sender.on_lost(frame.id);
        }
        if (m_outcomes)
        {
            m_fate[frame.id] = Fate::Settled;
            m_acked[frame.id] = (choice & 0x80U) != 0U;
        }
    }

    void poll()
    {
        QuicDatagramReport report;
        while (m_sender.poll(report))
        {
            CRD_FUZZ_REQUIRE(m_outcomes && report.id < m_queued && m_fate[report.id] == Fate::Settled);
            QuicDatagramOutcome expected = m_acked[report.id] ? QuicDatagramOutcome::Acked : QuicDatagramOutcome::Lost;
            if (m_expired_ids[report.id])
            {
                expected = QuicDatagramOutcome::Expired;
            }
            CRD_FUZZ_REQUIRE(report.outcome == expected);
            m_fate[report.id] = Fate::Reported;
        }
    }

    void read()
    {
        ConstSpan<u8> held;
        while (m_receiver.peek(held))
        {
            CRD_FUZZ_REQUIRE(m_held_count != 0U);
            const u64 id = m_held[m_held_first];
            if (id == kIds) // an injected payload
            {
                CRD_FUZZ_REQUIRE(held.size() == m_injected[m_held_first]);
            }
            else
            {
                CRD_FUZZ_REQUIRE(held.size() == m_size[id]);
                for (usize offset = 0U; offset < held.size(); ++offset)
                {
                    CRD_FUZZ_REQUIRE(held[offset] == pattern(id, offset));
                }
            }
            m_receiver.pop();
            m_held_first = (m_held_first + 1U) % kHeld;
            --m_held_count;
        }
        CRD_FUZZ_REQUIRE(m_held_count == 0U);
    }

    void inject()
    {
        const usize length = (usize{m_in->byte()} << 8U) | m_in->byte();
        u8 data[1400] = {};
        QuicFrame frame;
        frame.kind = QuicFrameKind::Datagram;
        frame.to_end = (length & 1U) != 0U;
        frame.data = ConstSpan<u8>(data, (length >> 1U) % sizeof(data));
        const u64 received = m_receiver.counters().received;
        const QuicTransportError error = m_receiver.on_frame(frame);
        CRD_FUZZ_REQUIRE((error == QuicTransportError::ProtocolViolation) ==
                         (quic_frame_bytes(frame) > m_receiver_max()));
        if (m_receiver.counters().received != received)
        {
            CRD_FUZZ_REQUIRE(m_held_count < kHeld);
            m_injected[(m_held_first + m_held_count) % kHeld] = frame.data.size();
            m_held[(m_held_first + m_held_count++) % kHeld] = kIds;
        }
    }

    [[nodiscard]] u64 m_receiver_max() const
    {
        QuicTransportParameters local;
        m_receiver.fill_parameters(local);
        return local.max_datagram_frame_size;
    }

    void check_accounting() const
    {
        u32 queued = 0U;
        u32 unreported = 0U;
        for (usize id = 0U; id < m_queued; ++id)
        {
            queued += (m_fate[id] == Fate::Queued) ? 1U : 0U;
            unreported += (m_fate[id] != Fate::Reported) ? 1U : 0U;
        }
        CRD_FUZZ_REQUIRE(m_sender.queued() == queued && m_sender.unreported() == unreported);
        CRD_FUZZ_REQUIRE(m_receiver.held() == m_held_count);
    }

    // Everything written, every flight frame settled and every report polled: each id accounted for.
    void drain()
    {
        for (usize round = 0U; round <= kIds; ++round)
        {
            write(true);
            for (usize slot = 0U; slot < kFlight; ++slot)
            {
                settle(static_cast<u8>(slot | 0x80U));
            }
            poll();
            read();
        }
        for (usize id = 0U; id < m_queued; ++id)
        {
            CRD_FUZZ_REQUIRE(m_fate[id] == Fate::Reported);
        }
        CRD_FUZZ_REQUIRE(m_sender.unreported() == 0U && m_sender.queued() == 0U);
    }

    Input* m_in = nullptr;
    crd::fuzz::BudgetAllocator m_allocator;
    QuicDatagrams m_sender;
    QuicDatagrams m_receiver;
    bool m_outcomes = false;
    u64 m_now = 0U;
    Frame m_flight[kFlight];
    usize m_size[kIds] = {};
    Fate m_fate[kIds] = {};
    bool m_acked[kIds] = {};
    bool m_expired_ids[kIds] = {};
    usize m_queued = 0U;
    usize m_oldest = 0U;
    u64 m_expired = 0U;
    u64 m_held[kHeld] = {};
    usize m_injected[kHeld] = {};
    usize m_held_first = 0U;
    usize m_held_count = 0U;
};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    // The run's tables are too large for a fuzzer's stack: one static run, rebuilt per input.
    alignas(Run) static unsigned char storage[sizeof(Run)];
    static Run* held = nullptr;
    if (held != nullptr)
    {
        held->~Run();
    }
    held = new (storage) Run;
    Input in{data, size};
    held->go(in);
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Queue three, write them whole, deliver, acknowledge one and lose one, poll and read.
    const u8 plain[] = {0x7FU, 0x08U, 0x00U, 0xF0U, 0x00U, 0x00U, 0x40U, 0x00U, 0x01U,
                        0x30U, 0x01U, 0x00U, 0x10U, 0x02U, 0x02U, 0x02U, 0x04U, 0x00U,
                        0x04U, 0x01U, 0x04U, 0x02U, 0x05U, 0x80U, 0x05U, 0x01U, 0x06U};
    sink.add("plain", plain, sizeof(plain));
    // Outcomes with an age limit: time passes between queueing and writing.
    const u8 aged[] = {0x48U, 0x03U, 0x8AU, 0x20U, 0x00U, 0x00U, 0x20U, 0x07U, 0x08U, 0x00U,
                       0x00U, 0x30U, 0x07U, 0x05U, 0x02U, 0x03U, 0x40U, 0x05U, 0x80U, 0x06U};
    sink.add("aged-outcomes", aged, sizeof(aged));
    // Injected frames at and past the size advertised, and a small receive ring filling.
    const u8 injected[] = {0x10U, 0x31U, 0x00U, 0x05U, 0x08U, 0x00U, 0x30U, 0x08U, 0x00U, 0x31U,
                           0x08U, 0x00U, 0x32U, 0x08U, 0x00U, 0x33U, 0x06U, 0x08U, 0x0FU, 0xFFU};
    sink.add("injected", injected, sizeof(injected));
}
