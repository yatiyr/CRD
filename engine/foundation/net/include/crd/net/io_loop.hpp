#pragma once

// crd-net -- the transport's I/O loop and its two seams, the clock and the executor (4.2a; contract:
// docs/design/network-foundation.md#ingress).
//
//   clock     the loop reads time only through a TransportClock: HostClock in a process, NET-SIM's virtual clock in a
//             simulation, a test's own in a test. Nothing in the transport reads a host clock.
//   executor  poll() is one bounded turn that never blocks: S0's batched receive (at most `batches_per_poll` batches,
//             so timers run under a flood), S1 to S3 through the Ingress, routed, admitted and early datagrams and
//             short headers for no connection to the handler, the stateless answers sent, the handler's timers. It
//             returns when it next needs to run. Who calls it is the executor: run() on a thread of its own (waiting in
//             DatagramSocket::wait_readable until that time or a datagram), a game's frame, a job, or NET-SIM stepping
//             virtual time. The transport takes no job system and starts no thread.
//   memory    receive slots and the answer queue (a SendQueue) come from the injected allocator at construction; a
//             turn allocates nothing. An answer the host cannot take yet (Busy) waits in the queue; when the queue is
//             full, a new answer is dropped and counted (answers are stateless; the client's retransmission asks
//             again).
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

// A fixed ring of outgoing datagrams, each written in place into a slot of `slot_bytes`, handed to a socket in order:
// what the host cannot take yet (Busy) waits for the next flush, and a datagram the host refuses for good (an
// unreachable peer, a refused address) is dropped alone and counted. Its storage comes from the injected allocator at
// construction; nothing is allocated after.
class SendQueue
{
public:
    // `slots` from 1 to 65536 of `slot_bytes` from 1 to kMaxUdpPayloadV6.
    SendQueue(memory::IAllocator& allocator, u32 slots, usize slot_bytes) noexcept;
    ~SendQueue();
    SendQueue(const SendQueue&) = delete;
    SendQueue& operator=(const SendQueue&) = delete;
    SendQueue(SendQueue&&) = delete;
    SendQueue& operator=(SendQueue&&) = delete;

    [[nodiscard]] bool ready() const noexcept { return m_datagrams != nullptr; }
    // The next free slot to write a datagram into; empty when the queue is full.
    [[nodiscard]] cont::Span<u8> room() noexcept;
    // Queues the first `bytes` of room() for `peer`, its IP header marked `ecn` (stateless answers go Not-ECT).
    void push(usize bytes, const Endpoint& peer, Ecn ecn = Ecn::NotEct) noexcept;
    // Hands the queued datagrams to `socket` until it is Busy or none is left.
    [[nodiscard]] SendFlush flush(DatagramSocket& socket) noexcept;

    [[nodiscard]] u32 queued() const noexcept { return m_queued; }
    [[nodiscard]] bool full() const noexcept { return m_queued == m_slots; }

private:
    memory::IAllocator& m_allocator;
    u32 m_slots = 0U;
    usize m_slot_bytes = 0U;
    OutgoingDatagram* m_datagrams = nullptr;
    u8* m_bytes = nullptr;
    u32 m_head = 0U;
    u32 m_queued = 0U;
};

struct IoLoopConfig
{
    u32 batch = 32U;                // datagrams per receive
    u32 slot_bytes = 1500U;         // the largest datagram read whole; a longer one is truncated and dropped
    u32 batches_per_poll = 4U;      // receives per turn at most
    u32 queued_answers = 64U;       // answers waiting for the host at most
    u64 max_wait_ns = 100000000U;   // the longest run() waits without a turn (100 ms)
};

struct IoLoopCounters
{
    u64 polls = 0U;
    u64 received = 0U;
    u64 too_big = 0U; // of those, the socket's Packet Too Big reports
    u64 receive_errors = 0U;
    u64 answers_sent = 0U;
    u64 answers_dropped = 0U; // the queue was full, or the host refused one for good
};

class IoLoop
{
public:
    IoLoop(memory::IAllocator& allocator, DatagramSocket& socket, const TransportClock& clock, Ingress& ingress,
           IngressHandler& handler, const IoLoopConfig& config) noexcept;
    ~IoLoop();
    IoLoop(const IoLoop&) = delete;
    IoLoop& operator=(const IoLoop&) = delete;
    IoLoop(IoLoop&&) = delete;
    IoLoop& operator=(IoLoop&&) = delete;

    // False when the configuration is out of range (a zero count, slots under 1200 or over 65527 bytes) or the
    // allocator refused.
    [[nodiscard]] bool ready() const noexcept { return m_slots != nullptr; }

    // One turn (see the file comment); returns the monotonic time it next needs to run.
    [[nodiscard]] u64 poll() noexcept;
    // Turns until `stop` is set, waiting for a datagram between turns until the time poll() named, at most
    // max_wait_ns.
    void run(const std::atomic<bool>& stop) noexcept;

    [[nodiscard]] const IoLoopCounters& counters() const noexcept { return m_counters; }
    [[nodiscard]] u32 queued_answers() const noexcept { return m_answers.queued(); }

private:
    void receive_batch(u64 now_ns, u64 unix_seconds, BatchResult& result) noexcept;
    void queue_answer(const IngressDecision& decision, const Endpoint& peer, u64 unix_seconds) noexcept;
    void flush_answers() noexcept;

    memory::IAllocator& m_allocator;
    DatagramSocket& m_socket;
    const TransportClock& m_clock;
    Ingress& m_ingress;
    IngressHandler& m_handler;
    IoLoopConfig m_config;
    ReceivedDatagram* m_slots = nullptr;
    u8* m_buffers = nullptr;
    SendQueue m_answers; // `queued_answers` slots of kQuicMaxStatelessAnswerBytes
    IoLoopCounters m_counters;
};

} // namespace crd::net
