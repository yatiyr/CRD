#pragma once

// crd-net -- the transport's I/O loop and its two seams, the clock and the executor (4.2a; contract:
// docs/design/network-foundation.md#ingress).
//
//   clock     the loop reads time only through a TransportClock: HostClock in a process, NET-SIM's virtual clock in a
//             simulation, a test's own in a test. Nothing in the transport reads a host clock.
//   sockets   a loop serves the sockets of a SocketSet (udp.hpp): each turn reads every socket in order, each datagram
//             naming the one it arrived on (ReceivedDatagram::socket), and a stateless answer leaves by that socket.
//   executor  poll() is one bounded turn that never blocks: S0's batched receive (per socket, `batches_per_poll`
//             batches' worth of datagrams, a coalescing socket's trains counted by datagram, so timers run under a
//             flood), S1 to S3 through the Ingress, routed, admitted and early datagrams and short headers for no
//             connection to the handler, the stateless answers sent, the handler's timers. It returns when it next needs to run. Who calls it is the executor: run() on a thread of its own
//             (waiting in SocketSet::wait_readable until that time or a datagram), a game's frame, a job, or NET-SIM
//             stepping virtual time. The transport takes no job system and starts no thread.
//   memory    receive slots and the answer queue (a SendQueue) come from the injected allocator at construction; a
//             turn allocates nothing. A socket that coalesces (SocketFeatures::coalesced_receive) reads into slots of
//             a whole train each, which the loop splits in place: each datagram of a train is handed on as its own,
//             its bytes where the host put them. A socket that lends (SocketFeatures::lent_receive) fills empty slots
//             with buffers of its own, handed on in place and cut to slot_bytes as a slot of that size would cut
//             them, so the loop holds no storage for it. An answer the host cannot take yet (Busy) waits in the
//             queue; when the queue is full, a new answer is dropped and counted (answers are stateless; the client's
//             retransmission asks again).
//   threads   a loop runs on one thread at a time; its ingress, table, socket and handler are its own (the server's
//             HandshakeBuckets are shared, ingress.hpp).

#include <crd/core/types.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/udp.hpp>

#include <atomic>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
class TransportClock
{
public:
    virtual ~TransportClock() = default;
    // Nanoseconds on a clock that never steps back: every timer and rate of the transport.
    [[nodiscard]] virtual u64 monotonic_ns() const noexcept = 0;
    // Seconds since the Unix epoch on the fleet's clock: what address validation tokens carry between servers.
    [[nodiscard]] virtual u64 unix_seconds() const noexcept = 0;

protected:
    TransportClock() = default;
    TransportClock(const TransportClock&) = default;
    TransportClock(TransportClock&&) noexcept = default;
    TransportClock& operator=(const TransportClock&) = default;
    TransportClock& operator=(TransportClock&&) noexcept = default;
};

// The host's clocks: crd-time's TickClock and WallClock.
class HostClock final : public TransportClock
{
public:
    [[nodiscard]] u64 monotonic_ns() const noexcept override;
    [[nodiscard]] u64 unix_seconds() const noexcept override;
};

// What the loop hands on: the connection layer. A datagram's bytes are valid for the call, and the handler may decrypt
// them in place.
class IngressHandler
{
public:
    virtual ~IngressHandler() = default;
    // A datagram S2 routed to `decision.connection`.
    virtual void on_route(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept = 0;
    // A client Initial S3 admitted: the handler makes the connection and routes its IDs (the client's original
    // Destination Connection ID among them) through the loop's table.
    virtual void on_admit(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept = 0;
    // 0-RTT for no connection (IngressAction::Early): the handler may hold it until its Initial creates the
    // connection (RFC 9000 section 5.2.2), or drop it.
    virtual void on_early(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept = 0;
    // A short header for no connection, before the ingress's stateless reset answers it, or one too short to be a
    // packet: true when it was a peer's stateless reset ending one of the handler's connections (RFC 9000 section
    // 10.3.1), which is not answered.
    [[nodiscard]] virtual bool on_unrouted(const IngressDecision& decision, const ReceivedDatagram& datagram,
                                           u64 now_ns) noexcept = 0;
    // The socket's report that a datagram sent to `report.peer` was too big for a hop (ReceivedDatagram::too_big),
    // which no admission stage reads: the handler matches it to its connections or drops it (the default).
    virtual void on_too_big(const ReceivedDatagram& report, u64 now_ns) noexcept;
    // Runs what is due at `now_ns`; returns when it next needs to run, kNeverNs for no timer.
    [[nodiscard]] virtual u64 on_timers(u64 now_ns) noexcept = 0;

protected:
    IngressHandler() = default;
    IngressHandler(const IngressHandler&) = default;
    IngressHandler(IngressHandler&&) noexcept = default;
    IngressHandler& operator=(const IngressHandler&) = default;
    IngressHandler& operator=(IngressHandler&&) noexcept = default;
};

// What one SendQueue::flush did.
struct SendFlush
{
    usize sent = 0U;
    usize dropped = 0U; // refused for good
};

// A fixed queue of outgoing datagrams, each written in place into a slot of `slot_bytes` and naming the socket of a
// SocketSet it leaves by, handed to each socket in the order queued: what a socket cannot take yet (Busy) waits for
// the next flush without holding back another socket's (a connection's new path leaves while its old socket is
// busy), and a datagram the host refuses for good (an unreachable peer, a refused address) is dropped alone and
// counted, as is one naming a socket the flushed set lacks. Its storage comes from the injected allocator at
// construction; nothing is allocated after, and no datagram's bytes move once written.
//
// Lent slots: the queue hands its datagrams over with send_stable(), as its bytes stay put. A slot whose datagram a
// lending socket (SocketFeatures::lent_send) sent is lent until that socket's sends_released() passes it, read at
// each flush: the host may still be sending from it, so it is neither written nor reused meanwhile, and the queue's
// destructor waits until every socket that lent one holds none.
//
// Admission: the `sockets` sockets share the slots, each with a guaranteed share of slots / (2 * sockets), at least
// one where the slots allow one for each (none for fewer slots than sockets). A socket takes a free slot only while
// as many stay free as the other sockets' shares still lack, so a stalled socket fills at most the rest and every
// other socket always finds room for its share: a connection's datagrams for a working socket are never held behind a
// busy one's. A lent slot counts against its socket as a queued one does. A queue of one socket admits into every
// slot.
class SendQueue
{
public:
    // `slots` from 1 to 65536 of `slot_bytes` from 1 to kMaxUdpPayloadV6, shared by `sockets` from 1 to
    // kMaxSetSockets.
    SendQueue(memory::IAllocator& allocator, u32 slots, usize slot_bytes, u32 sockets = 1U) noexcept;
    ~SendQueue();
    SendQueue(const SendQueue&) = delete;
    SendQueue& operator=(const SendQueue&) = delete;
    SendQueue(SendQueue&&) = delete;
    SendQueue& operator=(SendQueue&&) = delete;

    [[nodiscard]] bool ready() const noexcept { return m_entries != nullptr; }
    // The slot a datagram for `socket` is written into; empty when admission holds that socket back (always when the
    // queue is full, lent slots included) or for a socket from `sockets` on.
    [[nodiscard]] cont::Span<u8> room(u32 socket) noexcept;
    // The sockets admission holds back now, bit i for socket i: all of them exactly when the queue is full.
    [[nodiscard]] u32 held() const noexcept;
    // Queues the first `bytes` of room(socket) for `peer` through the set's socket `socket`, its IP header marked
    // `ecn` (stateless answers go Not-ECT); false, queuing nothing, when room(socket) is empty or shorter.
    [[nodiscard]] bool push(usize bytes, const Endpoint& peer, Ecn ecn = Ecn::NotEct, u32 socket = 0U) noexcept;
    // Hands each socket of `sockets` its queued datagrams until it is Busy or none of its own is left, and frees the
    // lent slots their sockets released.
    [[nodiscard]] SendFlush flush(const SocketSet& sockets) noexcept;

    [[nodiscard]] u32 queued() const noexcept { return m_queued; }
    // The datagrams queued for `socket` (below `sockets`), and the slots each socket is guaranteed beside the others
    // (0 for a queue of one socket).
    [[nodiscard]] u32 queued(u32 socket) const noexcept
    {
        // m_sockets never passes kMaxSetSockets: the second bound says so to GCC's LTO, which cannot prove it.
        return (socket < m_sockets && socket < kMaxSetSockets) ? m_per_socket[socket] : 0U;
    }
    // The slots whose datagrams the host may still be sending from, and those of `socket`.
    [[nodiscard]] u32 lent() const noexcept { return m_lent; }
    [[nodiscard]] u32 lent(u32 socket) const noexcept
    {
        return (socket < m_sockets && socket < kMaxSetSockets) ? m_lent_per_socket[socket] : 0U;
    }
    [[nodiscard]] u32 share() const noexcept { return m_share; }
    [[nodiscard]] bool full() const noexcept { return m_queued + m_lent == m_slots; }

private:
    // A slot's datagram and the socket it leaves by; `done` once it left or was dropped, until the flush ends, and
    // `lent` while the host may read it, the socket's datagram `number`.
    struct Entry
    {
        OutgoingDatagram datagram;
        u32 socket = 0U;
        bool done = false;
        bool lent = false;
        u64 number = 0U;
    };

    [[nodiscard]] bool admits(u32 socket) const noexcept;
    // Sends socket `index`'s datagrams in the order queued, marking each one that left or was dropped.
    void send_through(DatagramSocket& socket, u32 index, SendFlush& flushed) noexcept;

    memory::IAllocator& m_allocator;
    u32 m_slots = 0U;
    usize m_slot_bytes = 0U;
    u32 m_sockets = 0U;
    u32 m_share = 0U;
    Entry* m_entries = nullptr; // per slot
    u32* m_order = nullptr;     // the queued slots in order, the lent ones, then the free ones
    u8* m_bytes = nullptr;
    u32 m_queued = 0U;
    u32 m_lent = 0U;
    u32 m_per_socket[kMaxSetSockets] = {};
    u32 m_lent_per_socket[kMaxSetSockets] = {};
    DatagramSocket* m_lenders[kMaxSetSockets] = {}; // each socket that lent a slot, which outlives the queue
};

struct IoLoopConfig
{
    u32 batch = 32U;                // datagrams per receive
    u32 slot_bytes = 1500U;         // the largest datagram read whole; a longer one is truncated and dropped
    u32 train_slots = 4U;           // trains per receive of a coalescing socket, kMaxTrainBytes each
    u32 batches_per_poll = 4U;      // per socket a turn: batch * batches_per_poll datagrams handed on at most
    u32 queued_answers = 64U;       // answers waiting for the host at most
    u64 max_wait_ns = 100000000U;   // the longest run() waits without a turn (100 ms)
};

struct IoLoopCounters
{
    u64 polls = 0U;
    u64 received = 0U;
    u64 too_big = 0U; // of those, the socket's Packet Too Big reports
    u64 trains = 0U;  // the trains coalescing sockets joined, each of their datagrams in `received`
    u64 receive_errors = 0U;
    u64 answers_sent = 0U;
    u64 answers_dropped = 0U; // the queue was full, or the host refused one for good
};

class IoLoop
{
public:
    IoLoop(memory::IAllocator& allocator, const SocketSet& sockets, const TransportClock& clock, Ingress& ingress,
           IngressHandler& handler, const IoLoopConfig& config) noexcept;
    ~IoLoop();
    IoLoop(const IoLoop&) = delete;
    IoLoop& operator=(const IoLoop&) = delete;
    IoLoop(IoLoop&&) = delete;
    IoLoop& operator=(IoLoop&&) = delete;

    // False when the configuration is out of range (a zero count, slots under 1200 or over 65527 bytes; train_slots
    // counts only where a socket of the set coalesces), a socket holds less than slot_bytes of a datagram
    // (SocketFeatures::receive_bytes), the socket set is empty or the allocator refused.
    [[nodiscard]] bool ready() const noexcept { return m_slots != nullptr; }

    // One turn (see the file comment); returns the monotonic time it next needs to run.
    [[nodiscard]] u64 poll() noexcept;
    // Turns until `stop` is set, waiting for a datagram on any socket between turns until the time poll() named, at
    // most max_wait_ns.
    void run(const std::atomic<bool>& stop) noexcept;

    [[nodiscard]] const IoLoopCounters& counters() const noexcept { return m_counters; }
    [[nodiscard]] u32 queued_answers() const noexcept { return m_answers.queued(); }

private:
    // One receive from `socket`; true when it filled every slot, so the host may hold more.
    [[nodiscard]] bool receive_batch(u32 socket, u64 now_ns, u64 unix_seconds, BatchResult& result) noexcept;
    void deliver(ReceivedDatagram& datagram, u64 now_ns, u64 unix_seconds) noexcept;
    void queue_answer(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 unix_seconds) noexcept;
    void flush_answers() noexcept;

    memory::IAllocator& m_allocator;
    SocketSet m_sockets;
    const TransportClock& m_clock;
    Ingress& m_ingress;
    IngressHandler& m_handler;
    IoLoopConfig m_config;
    ReceivedDatagram* m_slots = nullptr; // `batch` slots, with storage where a socket reads into it
    u8* m_buffers = nullptr;
    usize m_buffer_bytes = 0U;
    u32 m_coalescing = 0U; // bit i for socket i, which reads into the train slots unless it lends
    ReceivedDatagram* m_trains = nullptr;
    u8* m_train_buffers = nullptr;
    u32 m_train_count = 0U;
    u32 m_lending = 0U; // bit i for socket i, which lends its own buffers to the empty slots of m_lent
    ReceivedDatagram* m_lent = nullptr;
    u32 m_lent_count = 0U;
    SendQueue m_answers; // `queued_answers` slots of kQuicMaxStatelessAnswerBytes
    IoLoopCounters m_counters;
};

} // namespace crd::net
