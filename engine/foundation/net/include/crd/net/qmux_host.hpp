#pragma once

// crd-net -- the TCP and TLS path's host: QMux connections behind a StreamPoller on one thread (4.2a; contract:
// docs/design/network-foundation.md#stream-host). The stream path's counterpart of QuicHost; a host can accept,
// connect, or both.
//
//   admit     an accepted connection takes a slot of a fixed pool and starts its TLS. A TCP peer has proved its
//             address, so it is charged to the HandshakeBuckets the QUIC ingress shares as a validated one. A
//             connection that the pool or the rate has no room for is reset at once (threat T01). Under a shortage of
//             descriptors, accepting pauses for `accept_retry_ns` rather than spinning on a listener that stays
//             readable.
//   connect   connect() starts the TLS session with the socket: the handshake timeout covers the TCP connection too.
//   service   a connection that read, whose timer is due or that the application woke is served once per turn: its
//             timeout, the application's on_service, then its bytes to the socket until none is left, the socket is
//             full or its share of the turn is spent. Deadlines are a heap, so a turn costs O(log n) per connection
//             served, never a scan. A connection that ended on what it read or its timer is served once more, Closed,
//             so the application reads what arrived before the end.
//   fairness  a turn takes one read of at most kQmuxHostChunkBytes, `write_bytes_per_turn` of writes per connection and
//             `accepts_per_turn` connections; what is left is reported by the next wait(), level-triggered.
//   writes    bytes stay in the connection's TLS output until the socket has taken them, so a partial write needs no
//             second buffer per connection.
//   ends      a write that finds the peer gone leaves what it sent in the socket: that is read first, so a peer that
//             reset behind its last bytes and its close is reported by them, within a bounded number of reads. The one
//             exception to a turn's share of reads: it happens once per connection and the host's buffer bounds it.
//   close     a Closed connection's last bytes are tried once. It ends with a FIN when everything went out, and with a
//             reset when bytes could not be sent, the handshake timed out or the transport had failed. Then it is
//             reported and its slot reused under a new generation.
//   HTTP/2    a connection whose TLS agrees on h2 (QmuxConfig::http2) is the bearer of WebSocket tunnels (RFC 8441):
//             each tunnel is a connection of its own to the application, with its own handle from the same pool, so
//             `connections` bounds both. A server's tunnel is accepted when its extended CONNECT is (with the bearer's
//             peer), and refused with CONNECTION_REFUSED when no slot is free; a client opens one with open_tunnel().
//             A tunnel is served when its bearer is, and every tunnel is reported closed before its bearer.
//   memory    the pool, the heap and the turn's buffers come from the injected allocator at construction; a
//             connection's streams and datagrams when it is opened, its HTTP/2 state when its TLS agrees on h2, and a
//             tunnel's when it opens, so a host bounds them with its allocator. A turn allocates nothing else.
//   threads   the thread that calls poll(): the events, connect(), wake() and connection() too.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/deadline_heap.hpp>
#include <crd/net/host_handle.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/qmux.hpp>
#include <crd/net/stream_poller.hpp>

#include <atomic>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
class HandshakeBuckets;

// The most a turn reads from one connection, and the largest piece of a connection's output one write offers.
inline constexpr usize kQmuxHostChunkBytes = crypto::kTlsMaxBufferedBytes;
// The reads a connection whose write failed still gets: more than a host's socket buffer holds.
inline constexpr u32 kReadsAfterFailure = 256U;

struct QmuxHostConfig
{
    // Every connection's: `tls` is a server's context for the connections it accepts, null for a host that only
    // connects, whose connect() names one for each. What it points at must outlive the host.
    QmuxConfig connection;
    u32 connections = 256U;                    // live connections and tunnels at most (1 to kQuicMaxHostConnections)
    u32 ready_per_turn = 256U;                 // readiness reports one turn takes, at least 1
    u32 accepts_per_turn = 32U;                // connections one turn accepts, at least 1
    u64 accept_retry_ns = 100'000'000U;        // accepting pauses this long when the host is out of descriptors
    usize write_bytes_per_turn = 256U * 1024U; // of one connection, at least one chunk
    u64 max_wait_ns = 100'000'000U;            // the longest run() waits without a turn
};

// One connection to start.
struct QmuxConnect
{
    Endpoint server;
    const crypto::TlsContext* tls = nullptr; // a client's context: its server name, pins and ALPN
};

// The application's side, called on the thread that polls.
class QmuxHostEvents
{
public:
    virtual ~QmuxHostEvents() = default;
    // A connection was accepted from `peer`, before it reads anything; or a tunnel, once its extended CONNECT was, with
    // its bearer's peer.
    virtual void on_accept(QuicHandle handle, QmuxConnection& connection, const Endpoint& peer) noexcept = 0;
    // The connection read, timed out or was woken: the application reads and writes its streams and datagrams; what it
    // writes is sent right after. A connection that ended on what it read or its timer comes here once more, Closed,
    // before on_closed: what arrived before the end can still be read, and nothing more can be sent.
    virtual void on_service(QuicHandle handle, QmuxConnection& connection, u64 now_ns) noexcept = 0;
    // The connection is Closed and is destroyed after this call; `transport` names how the socket failed (None when it
    // did not).
    virtual void on_closed(QuicHandle handle, const QmuxConnection& connection, SocketError transport) noexcept = 0;

protected:
    QmuxHostEvents() = default;
    QmuxHostEvents(const QmuxHostEvents&) = default;
    QmuxHostEvents(QmuxHostEvents&&) noexcept = default;
    QmuxHostEvents& operator=(const QmuxHostEvents&) = default;
    QmuxHostEvents& operator=(QmuxHostEvents&&) noexcept = default;
};

struct QmuxHostCounters
{
    u64 accepted = 0U;
    u64 refused_full = 0U;    // accepted connections reset: every slot was in use
    u64 refused_rate = 0U;    // accepted connections reset: the handshake rate was spent
    u64 refused_start = 0U;   // accepted connections reset: no connection could be built for them
    u64 accept_pauses = 0U;   // times the host ran out of descriptors
    u64 connected = 0U;       // connect() calls that started one
    u64 connect_refused = 0U; // connect() calls that started nothing
    u64 closed = 0U;
    u64 resets_sent = 0U; // connections ended with a reset
    u64 bytes_read = 0U;
    u64 bytes_written = 0U;
    u64 write_blocks = 0U; // writes the socket stopped
    u64 tunnels = 0U;         // tunnels opened on HTTP/2 connections, either end's
    u64 tunnels_refused = 0U; // a server's: tunnels refused, every slot was in use
};

class QmuxHost
{
public:
    // `poller` must hold connections + 1 slots (the last is where a connection without room is accepted and reset).
    // `buckets` admits accepted connections, null for a host that only connects. All must outlive the host.
    QmuxHost(memory::IAllocator& allocator, StreamPoller& poller, const TransportClock& clock,
             HandshakeBuckets* buckets, QmuxHostEvents& events, const QmuxHostConfig& config) noexcept;
    ~QmuxHost();
    QmuxHost(const QmuxHost&) = delete;
    QmuxHost& operator=(const QmuxHost&) = delete;
    QmuxHost(QmuxHost&&) = delete;
    QmuxHost& operator=(QmuxHost&&) = delete;

    // False when the configuration is out of range (counts out of bounds, a server's context without buckets or a
    // client's where a server's belongs) or the allocator refused.
    [[nodiscard]] bool ready() const noexcept { return m_slots != nullptr; }

    // One turn, waiting at most `wait_ns` for something to happen and never past the next deadline. Returns the
    // monotonic time it next needs to run.
    [[nodiscard]] u64 poll(u64 wait_ns) noexcept;
    // Turns until `stop` is set.
    void run(const std::atomic<bool>& stop) noexcept;

    // Starts a connection, served in the next turn (the application writes its first streams in on_service). An invalid
    // handle when every connection is in use, the context is null, the port is 0, the connection refuses its
    // configuration (error Failed) or the socket could not start (`error` names why).
    [[nodiscard]] QuicHandle connect(const QmuxConnect& request, u64 now_ns, SocketError& error) noexcept;

    // A client's new WebSocket tunnel on the HTTP/2 connection `bearer`: the handle it is reported by, invalid when no
    // slot is free (NoTunnel) or the connection refused it (`error`: NotReady until the server's SETTINGS allow it).
    [[nodiscard]] QuicHandle open_tunnel(QuicHandle bearer, u64 now_ns, WsTunnelError& error) noexcept;

    // The live connection `handle` names; null for a stale or unknown handle, and for a tunnel not open yet.
    [[nodiscard]] QmuxConnection* connection(QuicHandle handle) noexcept;
    // Serves the connection in the next turn (the application wrote to it outside on_service).
    void wake(QuicHandle handle) noexcept;

    [[nodiscard]] u32 live() const noexcept { return m_live; }
    [[nodiscard]] const QmuxHostCounters& counters() const noexcept { return m_counters; }

private:
    struct Slot
    {
        QmuxConnection* connection = nullptr; // live: in `cell`
        StreamSocket* socket = nullptr;
        u32 generation = 0U;
        u32 next_free = kNoConnection;
        SocketError transport = SocketError::None; // how the socket failed
        u32 bearer = kNoConnection;                // a tunnel's: the slot of the connection it runs on
        QuicHandle tunnel;                         // a tunnel's handle on that connection
        u8 interests = 0U;                         // what the poller was last asked for
        bool connecting = false;                   // its TCP connection is not made yet
        bool write_blocked = false;                // the socket is full: writes wait for it to be writable
        bool send_shut = false;                    // the sending side has been shut
    };

    [[nodiscard]] QmuxConnection* open(u32 index, StreamSocket& socket, const crypto::TlsContext& tls, bool connecting,
                                       u64 now_ns) noexcept;
    void accept_some(u64 now_ns) noexcept;
    void on_ready(const StreamReady& ready, u64 now_ns) noexcept;
    void finish_connect(u32 index, u64 now_ns) noexcept;
    void read_some(u32 index, u64 now_ns) noexcept;
    // Whether the connection's share of the turn ran out with bytes left.
    [[nodiscard]] bool write_some(u32 index, u64 now_ns) noexcept;
    // Reads what the socket still holds after a failed write, a bounded amount.
    void read_left(u32 index, u64 now_ns) noexcept;
    void transport_failed(Slot& slot, SocketError error, u64 now_ns) noexcept;
    void set_interest(u32 index, u8 interests) noexcept;
    void serve(u32 index, u64 now_ns) noexcept;
    // Reports what happened to the tunnels of the HTTP/2 connection at `index`.
    void serve_tunnels(u32 index, u64 now_ns) noexcept;
    void adopt_tunnel(u32 bearer, const WsTunnelEvent& event, u64 now_ns) noexcept;
    [[nodiscard]] u32 claim_tunnel(u32 bearer, QuicHandle tunnel) noexcept;
    void release_tunnel(u32 index) noexcept;
    void reclaim(u32 index, u64 now_ns) noexcept;
    [[nodiscard]] QuicHandle handle_of(u32 index) const noexcept;

    memory::IAllocator& m_allocator;
    StreamPoller& m_poller;
    const TransportClock& m_clock;
    HandshakeBuckets* m_buckets = nullptr;
    QmuxHostEvents& m_events;
    QmuxHostConfig m_config;
    DeadlineHeap m_heap;
    Slot* m_slots = nullptr;
    QmuxConnection* m_cells = nullptr; // uninitialised storage, one per slot
    u32* m_due = nullptr;              // the slots one turn serves
    StreamReady* m_ready = nullptr;    // what one wait() reported
    u8* m_chunk = nullptr;             // kQmuxHostChunkBytes: a read's bytes, and where written output is taken into
    u32 m_free = kNoConnection;
    u32 m_live = 0U;
    u64 m_accept_resume_ns = kNeverNs; // accepting is paused until then
    QmuxHostCounters m_counters;
};

} // namespace crd::net
