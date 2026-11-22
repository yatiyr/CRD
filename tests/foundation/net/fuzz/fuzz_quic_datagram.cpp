// crd-fuzz-net-quic-datagram: a QUIC connection's datagrams in crd-net (4.2a, NET-PATH;
// docs/design/network-foundation.md#quic-datagrams) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: four bytes of limits (the send ring, send_count, outcomes, redundant datagrams and an age limit, the receive
// ring), then operations, each an operation byte and the parameter bytes it reads (0 past the end): queue a payload
// (redundant or not, and once a path slot was opened or stopped, pinned to a slot's path ID, a stopped one's or one
// never opened), write the next frame for a path slot (with the slots carrying data, and whether it writes only
// copies) into a room of any size and keep it in a bounded flight, deliver a flight frame to the receiver (any number
// of times), acknowledge or lose it, poll reports, read and pop, advance time, inject any frame bytes into the
// receiver, and (an operation byte from 0xF0) open a path slot under a new path ID or stop an open one.
//
// Oracles, against a model of the send ring (the datagrams from the oldest held to the newest, each with the slots
// that sent it and its copies in flight). has_frame() is exact and names the model's next datagram for the slot: the
// oldest it may write (never sent, or a redundant one's copy owed to it among the slots carrying data; only copies when
// it yields, and one pinned to it), pinned to no other slot, that has not expired; a room of a whole packet gets that
// frame exactly when it said so. Every frame written fits its room, reads back as a DATAGRAM with its Length and holds
// that datagram's payload, a plain one ticketed by its id; the receiver accepts every frame the sender wrote and holds
// what it took in order, byte-exact; an injected frame is refused exactly when it passes the size advertised. The ring
// leaves exactly as the model's: expiry at max_age_ns, datagrams with nothing left to send to the slots carrying data,
// the copies owed for a datagram acknowledged, and only sent datagrams (owing copies) and pinned ones holding sent
// ones back giving way for the ring's room. queued(), unreported(), the expired, copies and unrouted counters and the
// redundant records held (a Full at queue()) are the model's; a pin is refused exactly when its path ID is open in no
// slot (NoPath) or it is redundant (Unsupported), and a pinned datagram is written only by its slot; every id is
// reported exactly once, as the model expects: a plain one Acked or Lost by its frame, a redundant one Acked at its
// first acknowledged copy, Lost once every copy was lost and none can go, Expired only unsent, a pinned one Lost unsent
// when its slot stops or it gives way; after a final drain on every slot every id is accounted for; nothing allocates
// after init().
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
constexpr u8 kEverySlot = (1U << kQuicMaxPaths) - 1U;
constexpr u8 kNoPin = 0xFFU;
constexpr usize kSlots = 4U; // the path slots the operations name

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

struct Frame
{
    u8 bytes[kRoom] = {};
    usize size = 0U;
    u64 id = 0U;
    u64 ticket = 0U;
    bool live = false;
};

// What the model knows of one datagram.
struct Datagram
{
    usize size = 0U;
    u64 queued_ns = 0U;
    bool redundant = false;
    u8 pin = kNoPin;      // the slot of its path ID, pinned
    u8 written = 0U;      // the slots that sent it (kEverySlot once a copy was acknowledged, or dropped where it stood)
    u8 flying = 0U;       // a redundant one's copies in flight
    bool settled = false; // a redundant one's: a copy acknowledged, or its report decided
    bool freed = false;   // its redundant record came free
    bool pending = false; // a report is due and not polled
    bool polled = false;
    QuicDatagramOutcome expect = QuicDatagramOutcome::Acked;
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
        send.redundant_count = ((shape & 0x80U) != 0U) ? 1U + ((u32{count} >> 4U) % send.send_count) : 0U;
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
        m_limits = send;
        CRD_FUZZ_REQUIRE(m_sender.settles() == (send.outcomes || send.redundant_count != 0U));
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
        if (operation >= 0xF0U)
        {
            path(operation % kSlots);
            return;
        }
        switch (operation % 9U)
        {
            case 0U:
            case 1U:
                queue();
                break;
            case 2U:
                write(true, paths());
                break;
            case 3U:
                write(false, paths());
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

    // With redundant datagrams or once a slot was opened or stopped, a byte names the writing slot, the slots carrying
    // data and copies_only.
    QuicDatagramPaths paths()
    {
        QuicDatagramPaths paths;
        if (m_limits.redundant_count != 0U || m_paths_used)
        {
            const u8 choice = m_in->byte();
            paths.slot = choice & 3U;
            paths.copies = (choice >> 2U) & kEverySlot;
            paths.copies_only = (choice & 0x40U) != 0U;
        }
        return paths;
    }

    // ---- The model ----

    [[nodiscard]] bool expired(const Datagram& datagram) const
    {
        return m_limits.max_age_ns != 0U && m_now >= datagram.queued_ns &&
               m_now - datagram.queued_ns > m_limits.max_age_ns;
    }

    void expect(u64 id, QuicDatagramOutcome outcome)
    {
        Datagram& datagram = m_datagrams[id];
        CRD_FUZZ_REQUIRE(!datagram.pending && !datagram.polled); // once
        datagram.pending = true;
        datagram.expect = outcome;
    }

    void finish(u64 id)
    {
        Datagram& datagram = m_datagrams[id];
        if (!datagram.redundant || datagram.freed || id >= m_front || datagram.flying != 0U)
        {
            return;
        }
        if (!datagram.settled && m_limits.outcomes)
        {
            expect(id, QuicDatagramOutcome::Lost);
        }
        datagram.settled = true;
        datagram.freed = true;
        --m_records;
    }

    void pop_front(QuicDatagramOutcome unsent)
    {
        const u64 id = m_front++;
        Datagram& datagram = m_datagrams[id];
        if (datagram.written == 0U)
        {
            if (m_limits.outcomes)
            {
                expect(id, unsent);
            }
            datagram.settled = true;
        }
        finish(id);
    }

    void expire()
    {
        while (m_front < m_queued && expired(m_datagrams[m_front]))
        {
            m_expired += (m_datagrams[m_front].written == 0U) ? 1U : 0U;
            pop_front(QuicDatagramOutcome::Expired);
        }
    }

    void pop_done(u32 copies)
    {
        while (m_front < m_queued)
        {
            const Datagram& oldest = m_datagrams[m_front];
            if (oldest.written == 0U || (oldest.redundant && (copies & ~u32{oldest.written} & kEverySlot) != 0U))
            {
                return;
            }
            pop_front(QuicDatagramOutcome::Lost);
        }
    }

    // The datagram write_next() writes next for `paths`: kIds for none.
    [[nodiscard]] u64 next_for(const QuicDatagramPaths& paths) const
    {
        const u32 bit = 1U << paths.slot;
        for (u64 id = m_front; id < m_queued; ++id)
        {
            const Datagram& datagram = m_datagrams[id];
            bool writable = datagram.redundant && (datagram.written & bit) == 0U && (paths.copies & bit) != 0U;
            if (datagram.written == 0U)
            {
                writable = (datagram.pin == kNoPin) ? !paths.copies_only : datagram.pin == paths.slot;
            }
            if (writable && !expired(datagram))
            {
                return id;
            }
        }
        return kIds;
    }

    // Whether the oldest gives way for the ring's room: sent (owing copies), or never sent, pinned, with one sent
    // behind.
    [[nodiscard]] bool gives_way() const
    {
        if (m_front == m_queued)
        {
            return false;
        }
        const Datagram& oldest = m_datagrams[m_front];
        bool behind = false;
        for (u64 id = m_front + 1U; id < m_queued && oldest.written == 0U && oldest.pin != kNoPin; ++id)
        {
            behind = behind || m_datagrams[id].written != 0U;
        }
        return oldest.written != 0U || behind;
    }

    [[nodiscard]] u32 unreported() const
    {
        u32 count = 0U;
        for (u64 id = 0U; id < m_queued; ++id)
        {
            const Datagram& datagram = m_datagrams[id];
            // With outcomes, until its report is polled; without, while it is queued unsent or redundant.
            const bool held = id >= m_front && (datagram.written == 0U || datagram.redundant);
            const bool counted = m_limits.outcomes ? !datagram.polled : held;
            count += counted ? 1U : 0U;
        }
        return count;
    }

    // ---- Operations ----

    void queue()
    {
        const usize wanted = (usize{m_in->byte()} << 8U) | m_in->byte();
        const usize bytes = (wanted & 0x7FFFU) % (m_sender.max_payload() + 2U); // one past the largest
        const bool redundant = m_limits.redundant_count != 0U && (wanted & 0x8000U) != 0U;
        // Once a slot was opened or stopped, a byte pins it: to a slot's path ID (open or stopped), or one never open.
        const u8 pinning = m_paths_used ? m_in->byte() : u8{0};
        const bool pinned = (pinning & 0x80U) != 0U;
        const u64 path_id = ((pinning & 0x40U) != 0U) ? m_next_path : m_path_ids[pinning % kSlots];
        usize named = kSlots; // the slot holding path_id, if any
        for (usize slot = 0U; slot < kSlots; ++slot)
        {
            named = ((m_open & (1U << slot)) != 0U && m_path_ids[slot] == path_id) ? slot : named;
        }
        const bool open = named != kSlots;
        if (m_queued == kIds)
        {
            return;
        }
        u8 payload[kRoom + 1U];
        for (usize offset = 0U; offset < bytes; ++offset)
        {
            payload[offset] = pattern(m_queued, offset);
        }
        QuicDatagramSend how;
        how.redundant = redundant;
        how.pinned = pinned;
        how.path_id = path_id;
        u64 id = 0U;
        const u32 before = m_sender.queued();
        const QuicDatagramError error = m_sender.queue(ConstSpan<u8>(payload, bytes), m_now, id, how);
        if (redundant && pinned)
        {
            CRD_FUZZ_REQUIRE(error == QuicDatagramError::Unsupported && m_sender.queued() == before);
            return;
        }
        if (pinned && !open)
        {
            CRD_FUZZ_REQUIRE(error == QuicDatagramError::NoPath && m_sender.queued() == before);
            return;
        }
        if (bytes > m_sender.max_payload())
        {
            CRD_FUZZ_REQUIRE(error == QuicDatagramError::TooLarge && m_sender.queued() == before);
            return;
        }
        expire();
        if (redundant && m_records == m_limits.redundant_count)
        {
            CRD_FUZZ_REQUIRE(error == QuicDatagramError::Full);
            return;
        }
        while (unreported() == m_limits.send_count && m_front < m_queued && m_datagrams[m_front].written != 0U)
        {
            pop_front(QuicDatagramOutcome::Lost); // owed copies give way
        }
        if (unreported() == m_limits.send_count)
        {
            CRD_FUZZ_REQUIRE(error == QuicDatagramError::Full);
            return;
        }
        // The byte ring's room is not modelled: whatever it took, only sent datagrams at the front gave way.
        CRD_FUZZ_REQUIRE(error == QuicDatagramError::None || error == QuicDatagramError::Full);
        const u32 added = (error == QuicDatagramError::None) ? 1U : 0U;
        CRD_FUZZ_REQUIRE(m_sender.queued() >= added && m_sender.queued() - added <= m_queued - m_front);
        while (m_queued - m_front > m_sender.queued() - added)
        {
            CRD_FUZZ_REQUIRE(gives_way());
            m_unrouted += (m_datagrams[m_front].written == 0U) ? 1U : 0U;
            pop_front(QuicDatagramOutcome::Lost);
        }
        if (error == QuicDatagramError::Full)
        {
            CRD_FUZZ_REQUIRE(!gives_way()); // nothing more gives way
            return;
        }
        CRD_FUZZ_REQUIRE(id == m_queued);
        Datagram& datagram = m_datagrams[m_queued++];
        datagram.size = bytes;
        datagram.queued_ns = m_now;
        datagram.redundant = redundant;
        datagram.pin = pinned ? static_cast<u8>(named) : kNoPin;
        m_records += redundant ? 1U : 0U;
    }

    // Opens `slot` under a path ID never used, or stops it: what is pinned to it and unsent is dropped as Lost.
    void path(usize slot)
    {
        m_paths_used = true;
        const auto bit = static_cast<u8>(1U << slot);
        if ((m_open & bit) == 0U)
        {
            m_path_ids[slot] = m_next_path++;
            m_open = static_cast<u8>(m_open | bit);
            m_sender.open_path(slot, m_path_ids[slot]);
            return;
        }
        m_open = static_cast<u8>(m_open & ~bit);
        m_sender.stop_path(slot);
        for (u64 id = m_front; id < m_queued; ++id)
        {
            Datagram& datagram = m_datagrams[id];
            if (datagram.written == 0U && datagram.pin == slot)
            {
                datagram.written = kEverySlot;
                ++m_unrouted;
                if (m_limits.outcomes)
                {
                    expect(id, QuicDatagramOutcome::Lost);
                }
            }
        }
    }

    void write(bool whole, const QuicDatagramPaths& paths)
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
        const bool promised = m_sender.has_frame(m_now, paths);
        const u64 next = next_for(paths);
        CRD_FUZZ_REQUIRE(promised == (next != kIds));
        u64 ticket = ~u64{0};
        frame.size = m_sender.write_next(Span<u8>(frame.bytes, room), m_now, ticket, paths);
        expire();
        pop_done(paths.copies);
        CRD_FUZZ_REQUIRE(next_for(paths) == next); // what is due is never what gave way
        if (whole)
        {
            CRD_FUZZ_REQUIRE((frame.size != 0U) == promised);
        }
        if (frame.size == 0U)
        {
            return;
        }
        Datagram& datagram = m_datagrams[next];
        CRD_FUZZ_REQUIRE(frame.size <= room);
        WireReader in(ConstSpan<u8>(frame.bytes, frame.size));
        QuicFrame read;
        CRD_FUZZ_REQUIRE(read_quic_frame(in, read) == QuicTransportError::NoError && in.done());
        CRD_FUZZ_REQUIRE(read.kind == QuicFrameKind::Datagram && !read.to_end && read.data.size() == datagram.size);
        for (usize offset = 0U; offset < read.data.size(); ++offset)
        {
            CRD_FUZZ_REQUIRE(read.data[offset] == pattern(next, offset));
        }
        CRD_FUZZ_REQUIRE(datagram.redundant || ticket == next);
        CRD_FUZZ_REQUIRE(datagram.pin == kNoPin || (datagram.pin == paths.slot && !datagram.redundant));
        m_copies += (datagram.written != 0U) ? 1U : 0U;
        datagram.written = static_cast<u8>(datagram.written | (1U << paths.slot));
        datagram.flying = static_cast<u8>(datagram.flying + (datagram.redundant ? 1U : 0U));
        pop_done(paths.copies);
        frame.id = next;
        frame.ticket = ticket;
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
        const bool acked = (choice & 0x80U) != 0U;
        if (acked)
        {
            m_sender.on_acked(frame.ticket);
        }
        else
        {
            m_sender.on_lost(frame.ticket);
        }
        Datagram& datagram = m_datagrams[frame.id];
        if (!datagram.redundant)
        {
            if (m_limits.outcomes)
            {
                expect(frame.id, acked ? QuicDatagramOutcome::Acked : QuicDatagramOutcome::Lost);
            }
            return;
        }
        CRD_FUZZ_REQUIRE(datagram.flying != 0U);
        --datagram.flying;
        if (acked && !datagram.settled)
        {
            datagram.settled = true;
            if (m_limits.outcomes)
            {
                expect(frame.id, QuicDatagramOutcome::Acked);
            }
            if (frame.id >= m_front)
            {
                datagram.written = kEverySlot; // the peer has it: no more copies
                pop_done(kEverySlot);
                return;
            }
        }
        finish(frame.id);
    }

    void poll()
    {
        QuicDatagramReport report;
        while (m_sender.poll(report))
        {
            CRD_FUZZ_REQUIRE(m_limits.outcomes && report.id < m_queued);
            Datagram& datagram = m_datagrams[report.id];
            CRD_FUZZ_REQUIRE(datagram.pending && report.outcome == datagram.expect);
            datagram.pending = false;
            datagram.polled = true;
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
                CRD_FUZZ_REQUIRE(held.size() == m_datagrams[id].size);
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
        CRD_FUZZ_REQUIRE(m_sender.queued() == m_queued - m_front && m_sender.unreported() == unreported());
        CRD_FUZZ_REQUIRE(m_sender.counters().expired == m_expired && m_sender.counters().copies == m_copies);
        CRD_FUZZ_REQUIRE(m_sender.counters().unrouted == m_unrouted);
        CRD_FUZZ_REQUIRE(m_receiver.held() == m_held_count);
    }

    // Everything written on every slot (slot 0 alone carrying data), every flight frame acknowledged and every report
    // polled: each id accounted for.
    void drain()
    {
        for (usize round = 0U; round <= 2U * kIds; ++round)
        {
            for (usize slot = 0U; slot < kSlots; ++slot)
            {
                QuicDatagramPaths paths;
                paths.slot = slot;
                write(true, paths);
            }
            for (usize slot = 0U; slot < kFlight; ++slot)
            {
                settle(static_cast<u8>(slot | 0x80U));
            }
            poll();
            read();
            check_accounting();
        }
        for (u64 id = 0U; id < m_queued; ++id)
        {
            CRD_FUZZ_REQUIRE(!m_datagrams[id].pending && (m_datagrams[id].polled || !m_limits.outcomes));
        }
        CRD_FUZZ_REQUIRE(m_sender.unreported() == 0U && m_sender.queued() == 0U && m_records == 0U);
    }

    Input* m_in = nullptr;
    crd::fuzz::BudgetAllocator m_allocator;
    QuicDatagrams m_sender;
    QuicDatagrams m_receiver;
    QuicDatagramLimits m_limits;
    u64 m_now = 0U;
    Frame m_flight[kFlight];
    Datagram m_datagrams[kIds];
    u64 m_queued = 0U;  // ids given
    u64 m_front = 0U;   // the oldest held: the send ring holds ids m_front to m_queued - 1
    u32 m_records = 0U; // redundant ones not freed
    u64 m_expired = 0U;
    u64 m_copies = 0U;
    u64 m_unrouted = 0U;
    u64 m_path_ids[kSlots] = {}; // each slot's newest path ID: slot 0 holds path ID 0 from the start
    u64 m_next_path = 1U;
    u8 m_open = 1U;
    bool m_paths_used = false;
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
    // Redundant datagrams with outcomes over slots 0 and 1: one queued redundant and one plain, the first copy on slot
    // 0, the copy on slot 1 while it yields, the plain one on slot 1; one copy lost, the other acknowledged; then a
    // slot that leaves the slots carrying data.
    const u8 redundant[] = {0xC8U, 0x33U, 0x00U, 0x40U, 0x00U, 0x80U, 0x20U, 0x01U, 0x00U, 0x20U, 0x02U, 0x0CU,
                            0x02U, 0x4DU, 0x02U, 0x0DU, 0x04U, 0x00U, 0x04U, 0x01U, 0x05U, 0x00U, 0x05U, 0x81U,
                            0x05U, 0x82U, 0x00U, 0x80U, 0x10U, 0x02U, 0x0CU, 0x02U, 0x04U, 0x06U};
    sink.add("redundant", redundant, sizeof(redundant));
    // Pinned datagrams with outcomes: slot 1 opened under path ID 1, one pinned to it and one plain; slot 0 yielding
    // writes nothing, slot 1 yielding its pinned one, slot 0 the plain one; a pin to a path ID never open is refused;
    // one more pinned to slot 1, which stops before it goes (Lost); then a pin to its path ID is refused.
    const u8 pinned[] = {0x48U, 0x03U, 0x00U, 0x40U, 0xF1U, 0x00U, 0x00U, 0x20U, 0x81U, 0x00U, 0x00U, 0x21U, 0x00U,
                         0x02U, 0x40U, 0x02U, 0x41U, 0x02U, 0x00U, 0x00U, 0x00U, 0x10U, 0xC1U, 0x00U, 0x00U, 0x22U,
                         0x81U, 0xF1U, 0x00U, 0x00U, 0x20U, 0x81U, 0x05U, 0x80U, 0x05U, 0x81U, 0x05U, 0x82U, 0x06U};
    sink.add("pinned", pinned, sizeof(pinned));
    // A pinned datagram whose slot sends nothing while slot 0 sends three behind it, each acknowledged and polled: the
    // ring is full, and the next plain one takes its room (Lost, unrouted).
    const u8 gives_way[] = {0x48U, 0x03U, 0x00U, 0x40U, 0xF1U, 0x00U, 0x00U, 0x08U, 0x81U, 0x00U, 0x00U, 0x08U,
                            0x00U, 0x02U, 0x00U, 0x05U, 0x80U, 0x06U, 0x00U, 0x00U, 0x08U, 0x00U, 0x02U, 0x00U,
                            0x05U, 0x80U, 0x06U, 0x00U, 0x00U, 0x08U, 0x00U, 0x02U, 0x00U, 0x05U, 0x80U, 0x06U,
                            0x00U, 0x00U, 0x08U, 0x00U, 0x02U, 0x00U, 0x05U, 0x80U, 0x06U};
    sink.add("pinned-gives-way", gives_way, sizeof(gives_way));
}
