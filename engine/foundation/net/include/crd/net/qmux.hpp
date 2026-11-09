#pragma once

// crd-net -- QMux (draft-ietf-quic-qmux-02): QUIC's streams and datagrams over TLS 1.3 on a byte stream, the path for
// networks that block UDP (4.2a; contract: docs/design/network-foundation.md#qmux). An application meets the same
// QuicStreamSet and QuicDatagrams as on QUIC; only the transport beneath them differs.
//
//   sans-I/O  bytes the transport received go in through receive(), bytes to send come out of take(); a StreamSocket,
//             NET-SIM's virtual stream and a test's pipe drive it alike.
//   records   each record is a Size and whole QUIC version 1 frames, 16382 bytes at most either way: larger records
//             buy nothing over TLS records of 16 KiB, so this side never advertises more and sends at most that.
//   parameters QX_TRANSPORT_PARAMETERS is the first frame each side sends, the moment TLS can carry it, and the first
//             a side reads; a parameter QUIC needs but QMux forbids is refused.
//   frames    PING, ACK, CRYPTO and the path, token and connection ID frames do not exist here; a peer's are refused.
//   streams   TCP delivers in order, so a stream frame that does not continue its stream is refused, and a frame is
//             acknowledged the moment TLS takes it. Reads never wait for the application: streams are bounded by flow
//             control and datagrams the application is late for are dropped.
//   liveness  the idle timer restarts when a whole record arrives or the last byte is handed to the transport, never
//             on TCP or TLS keep-alives; QX_PING keeps intermediaries awake and measures the RTT that tunes windows.
//   close     CONNECTION_CLOSE, then close_notify and a shutdown of the sending side, then the peer's end (data
//             arriving meanwhile is discarded). A stream ending without close_notify is a truncation, never a clean
//             end.
//   memory    records use two fixed buffers of the connection; streams and datagrams allocate at init(), never after.
//   threads   one thread at a time.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/quic_datagram.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_stream_set.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
// The default of max_record_size, and the largest record either side sends or accepts.
inline constexpr usize kQmuxMaxRecord = 16382U;
// The bytes of a close reason phrase kept, sent or read; a longer one is cut.
inline constexpr usize kQmuxCloseReasonBytes = 128U;
inline constexpr u64 kQmuxNoTimer = ~u64{0};

struct QmuxConfig
{
    const crypto::TlsContext* tls = nullptr;   // client or server, with ALPN; one accepting early data is refused
    crypto::TlsTicketStore* tickets = nullptr; // a client's tickets; nullptr offers and keeps none
    QuicStreamLimits streams;
    QuicDatagramLimits datagrams;
    u64 idle_timeout_ns = 30'000'000'000U;      // the idle timeout offered; 0 for none
    u64 handshake_timeout_ns = 10'000'000'000U; // from start() to the end of the TLS handshake
    u64 close_wait_ns = 1'000'000'000U;         // how long a closed side waits for the peer's end
    u64 keepalive_ns = 0U;                      // a QX_PING after this long with no record; 0 for none
    // Records pad to the next multiple of this with PADDING frames (NET.3's padded_size()); 0 pads nothing.
    u32 padding_block = 0U;
};

enum class QmuxState : u8
{
    Idle,        // before start()
    Handshaking, // the TLS handshake runs
    Established, // records flow (peer_parameters_known() tells whether the peer's arrived)
    Closing,     // this side closed: only the peer's end, or the wait for it, remains
    Closed,      // done: flush take(), shut the sending side, destroy
};

// Why a connection ended.
enum class QmuxCloseReason : u8
{
    None,
    Local,            // close() was called: `application`, error_code
    Error,            // this side closed with the transport error `error_code` (frame_type: the frame that caused it)
    Peer,             // the peer's CONNECTION_CLOSE: `application`, error_code, frame_type
    PeerShutdown,     // the peer's close_notify with no CONNECTION_CLOSE (its idle timeout, draft section 7.1)
    IdleTimeout,      // this side's idle timeout
    HandshakeTimeout, // the TLS handshake did not end in time
    TlsFailed,        // a fatal alert or a refusal; handshake() and tls_error() tell which
    Truncated,        // the transport ended without close_notify
    Reset,            // the transport was reset or failed
};

struct QmuxCloseInfo
{
    QmuxCloseReason reason = QmuxCloseReason::None;
    bool application = false;
    u64 error_code = 0U;
    u64 frame_type = 0U;
};

struct QmuxCounters
{
    u64 records_sent = 0U;
    u64 records_received = 0U;
    u64 bytes_padded = 0U;
    u64 pings_sent = 0U;
    u64 pongs_received = 0U;
};

class QmuxConnection
{
public:
    QmuxConnection() noexcept = default;
    ~QmuxConnection() = default;
    QmuxConnection(const QmuxConnection&) = delete;
    QmuxConnection& operator=(const QmuxConnection&) = delete;
    QmuxConnection(QmuxConnection&&) = delete;
    QmuxConnection& operator=(QmuxConnection&&) = delete;

    // Takes the streams' and datagrams' storage from `allocator`; false, holding nothing, for a configuration out of
    // range or a refusal. Once.
    [[nodiscard]] bool init(memory::IAllocator& allocator, const QmuxConfig& config) noexcept;

    // Starts the TLS session of the context's role: a client's first flight is ready for take(). False for a session
    // TLS refuses (tls_error()); the connection is then Closed.
    [[nodiscard]] bool start(u64 now_ns) noexcept;

    // Bytes the transport received, any amount, split anywhere.
    void receive(cont::ConstSpan<u8> bytes, u64 now_ns) noexcept;
    // The transport ended: `clean` for its orderly end (a FIN), false for a reset or a failure. Nothing more can be
    // sent, and take() has nothing.
    void transport_ended(bool clean, u64 now_ns) noexcept;

    // Whether take() would give bytes now.
    [[nodiscard]] bool wants_write(u64 now_ns) const noexcept;
    // Assembles what is due and moves up to out.size() bytes to send into `out`; the bytes it wrote.
    [[nodiscard]] usize take(cont::Span<u8> out, u64 now_ns) noexcept;
    // Assembles what is due and shows the bytes to send in place, valid until the next call: a host writes them to its
    // transport and take()s what the transport accepted, so a partial write needs no second buffer.
    [[nodiscard]] cont::ConstSpan<u8> outgoing(u64 now_ns) noexcept;
    // True once this side has closed and every byte has been taken: time to shut the transport's sending side.
    [[nodiscard]] bool send_finished() const noexcept;

    [[nodiscard]] u64 next_timer_ns() const noexcept;
    void on_timeout(u64 now_ns) noexcept;

    // Closes with `application_error` (CONNECTION_CLOSE 0x1d) and the first kQmuxCloseReasonBytes of `reason`. During
    // the handshake the session is abandoned, as TLS allows no application close there.
    void close(u64 application_error, cont::ConstSpan<u8> reason, u64 now_ns) noexcept;
    // Sends a QX_PING request.
    void ping() noexcept { m_ping_owed = true; }

    [[nodiscard]] QmuxState state() const noexcept { return m_state; }
    [[nodiscard]] const QmuxCloseInfo& close_info() const noexcept { return m_close; }
    [[nodiscard]] cont::ConstSpan<u8> close_reason() const noexcept
    {
        return cont::ConstSpan<u8>(m_reason, m_reason_bytes);
    }
    [[nodiscard]] crypto::TlsError tls_error() const noexcept { return m_tls_error; }
    [[nodiscard]] const crypto::TlsSession& handshake() const noexcept { return m_tls; }
    [[nodiscard]] bool peer_parameters_known() const noexcept { return m_peer_known; }
    [[nodiscard]] const QuicTransportParameters& peer_parameters() const noexcept { return m_peer; }
    // The smoothed RTT from QX_PING exchanges; 0 before the first answer.
    [[nodiscard]] u64 rtt_ns() const noexcept { return m_rtt_ns; }
    [[nodiscard]] const QmuxCounters& counters() const noexcept { return m_counters; }

    [[nodiscard]] QuicStreamSet& streams() noexcept { return m_streams; }
    [[nodiscard]] const QuicStreamSet& streams() const noexcept { return m_streams; }
    [[nodiscard]] QuicDatagrams& datagrams() noexcept { return m_datagrams; }
    [[nodiscard]] const QuicDatagrams& datagrams() const noexcept { return m_datagrams; }

private:
    void advance(u64 now_ns) noexcept;
    void established(u64 now_ns) noexcept;
    void drain(u64 now_ns) noexcept;
    void parse_records(u64 now_ns) noexcept;
    [[nodiscard]] bool on_record(cont::ConstSpan<u8> record, u64 now_ns) noexcept;
    [[nodiscard]] bool on_parameters(cont::ConstSpan<u8> bytes) noexcept;
    [[nodiscard]] bool on_ping(u64 type, u64 sequence, u64 now_ns) noexcept;
    [[nodiscard]] bool on_frame(const QuicFrame& frame) noexcept;
    void produce(u64 now_ns) noexcept;
    [[nodiscard]] usize build_record(u64 now_ns) noexcept;
    [[nodiscard]] usize build_close() noexcept;
    [[nodiscard]] usize write_parameters(cont::Span<u8> out) noexcept;
    [[nodiscard]] usize write_ping(u64 type, u64 sequence, cont::Span<u8> out) noexcept;
    [[nodiscard]] bool frames_due(u64 now_ns) const noexcept;
    [[nodiscard]] u64 idle_deadline() const noexcept;
    void fail(QuicTransportError error) noexcept;
    void tls_failed(crypto::TlsError error) noexcept;
    void peer_shutdown() noexcept;
    void finish_sending() noexcept;
    void keep_reason(cont::ConstSpan<u8> reason) noexcept;

    QmuxConfig m_config;
    QmuxState m_state = QmuxState::Idle;
    bool m_ready = false;
    crypto::TlsSession m_tls;
    crypto::TlsError m_tls_error = crypto::TlsError::None;
    QuicStreamSet m_streams;
    QuicDatagrams m_datagrams;
    QuicTransportParameters m_peer;
    bool m_peer_known = false;
    bool m_parameters_sent = false;
    bool m_close_owed = false;
    bool m_send_shut = false;      // close_notify is queued (or the alert of a failed handshake)
    bool m_transport_over = false; // the transport ended: nothing is sent any more
    bool m_ping_owed = false;
    bool m_pong_owed = false;
    bool m_peer_pinged = false;
    u64 m_pong_sequence = 0U;
    u64 m_ping_next = 0U;
    u64 m_ping_sent_ns = 0U;
    u64 m_rtt_ns = 0U;
    u64 m_idle_ns = 0U;
    u64 m_handshake_deadline = kQmuxNoTimer;
    u64 m_close_deadline = kQmuxNoTimer;
    u64 m_last_activity = 0U;
    u64 m_frame_type = 0U;
    QmuxCloseInfo m_close;
    u8 m_reason[kQmuxCloseReasonBytes] = {};
    usize m_reason_bytes = 0U;
    QmuxCounters m_counters;
    usize m_in_fill = 0U;
    u8 m_in[kQmuxMaxRecord + 8U] = {};  // received plaintext: at most one partial record and its Size
    u8 m_out[kQmuxMaxRecord + 2U] = {}; // the record being assembled, its Size in the first bytes
};

} // namespace crd::net
