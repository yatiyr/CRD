#pragma once

// crd-net -- QUIC frames (4.2a; contract: docs/design/network-foundation.md#quic-frames). Sources: RFC 9000 sections
// 12.4, 19 and 20.1, RFC 9221 section 4 (DATAGRAM) and RFC 9368 section 10.2 (VERSION_NEGOTIATION_ERROR).
//
//   reading   read_quic_frame takes one frame from a decrypted payload and returns the transport error that closes the
//             connection when it is malformed (FRAME_ENCODING_ERROR, or PROTOCOL_VIOLATION for a frame type in a
//             longer encoding than its shortest). Fields may use any varint encoding (section 16); every bound
//             section 19 sets is checked, an ACK's ranges included, so nothing a caller reads afterwards can
//             underflow. Data, tokens, reasons and connection IDs view the payload.
//   placing   which packet types may carry a frame (section 12.4, table 3) and which side may send it is
//             quic_frame_allowed's; a violation is PROTOCOL_VIOLATION.
//   writing   into the caller's span, in the shortest encodings, nothing allocated; a frame that does not fit
//             writes nothing and returns 0, so a packet builder sizes a frame first (quic_frame_bytes) or trims it.

#include <crd/containers/span.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/core/types.hpp>
#include <crd/net/quic_header.hpp>

namespace crd::net
{
// ---- Transport error codes (RFC 9000 section 20.1, RFC 9368 section 10.2) ----

// NOLINTNEXTLINE(performance-enum-size): a 62-bit varint codepoint of its IANA registry, as the wire carries it
enum class QuicTransportError : u64
{
    NoError                 = 0x00U,
    InternalError           = 0x01U,
    ConnectionRefused       = 0x02U,
    FlowControlError        = 0x03U,
    StreamLimitError        = 0x04U,
    StreamStateError        = 0x05U,
    FinalSizeError          = 0x06U,
    FrameEncodingError      = 0x07U,
    TransportParameterError = 0x08U,
    ConnectionIdLimitError  = 0x09U,
    ProtocolViolation       = 0x0AU,
    InvalidToken            = 0x0BU,
    ApplicationError        = 0x0CU,
    CryptoBufferExceeded    = 0x0DU,
    KeyUpdateError          = 0x0EU,
    AeadLimitReached        = 0x0FU,
    NoViablePath            = 0x10U,
    VersionNegotiationError = 0x11U,
    CryptoErrorFirst        = 0x0100U, // CRYPTO_ERROR: 0x0100 plus a TLS alert (crypto::quic_crypto_error)
};

// ---- Frame types ----

// The kinds of frame this module reads and writes; a kind covers every wire type of its family (the eight STREAM
// types, both ACK, MAX_STREAMS, STREAMS_BLOCKED, CONNECTION_CLOSE and DATAGRAM types).
enum class QuicFrameKind : u8
{
    Padding,
    Ping,
    Ack,
    ResetStream,
    StopSending,
    Crypto,
    NewToken,
    Stream,
    MaxData,
    MaxStreamData,
    MaxStreams,
    DataBlocked,
    StreamDataBlocked,
    StreamsBlocked,
    NewConnectionId,
    RetireConnectionId,
    PathChallenge,
    PathResponse,
    ConnectionClose,
    HandshakeDone,
    Datagram,
};

inline constexpr u64   kQuicMaxStreams         = u64{1} << 60U; // MAX_STREAMS and STREAMS_BLOCKED (section 19.11)
using crypto::kQuicResetTokenBytes; // a stateless reset token (section 10.3), crd-crypto's QuicResetKey
inline constexpr usize kQuicPathDataBytes      = 8U;            // PATH_CHALLENGE and PATH_RESPONSE data
inline constexpr u64   kQuicStreamTypeBase     = 0x08U;
inline constexpr u8    kQuicStreamBitFin       = 0x01U;
inline constexpr u8    kQuicStreamBitLength    = 0x02U;
inline constexpr u8    kQuicStreamBitOffset    = 0x04U;

// What a frame does to its packet (section 12.4 and RFC 9002 section 2): an ACK, PADDING or CONNECTION_CLOSE frame
// elicits no acknowledgement; an ACK is not congestion controlled; PADDING, NEW_CONNECTION_ID, PATH_CHALLENGE and
// PATH_RESPONSE are probing frames (section 9.1); STREAM frames are flow controlled.
[[nodiscard]] bool quic_frame_ack_eliciting(QuicFrameKind kind) noexcept;
[[nodiscard]] bool quic_frame_congestion_controlled(QuicFrameKind kind) noexcept;
[[nodiscard]] bool quic_frame_probing(QuicFrameKind kind) noexcept;

// Whether `kind` (with `application_close` for CONNECTION_CLOSE's type 0x1d) may arrive in a packet of `packet` from
// a server (`from_server`) or a client (section 12.4 table 3 and sections 19.7 and 19.20: only a server sends
// NEW_TOKEN and HANDSHAKE_DONE; RFC 9221: DATAGRAM in 0-RTT and 1-RTT). Initial and Handshake packets carry PADDING,
// PING, ACK, CRYPTO and the transport CONNECTION_CLOSE; 0-RTT packets carry neither ACK, CRYPTO, NEW_TOKEN,
// PATH_RESPONSE nor HANDSHAKE_DONE (and only clients send them); 1-RTT packets carry every frame. Version
// Negotiation, Retry and other versions carry none.
[[nodiscard]] bool quic_frame_allowed(QuicFrameKind kind, bool application_close, QuicPacketType packet,
                                      bool from_server) noexcept;

// ---- Frames ----

// One acknowledged range of packet numbers, smallest to largest inclusive.
struct QuicAckRange
{
    u64 smallest = 0U;
    u64 largest  = 0U;
};

// An ACK frame as read: its largest acknowledged, raw ACK Delay (scaled by the peer's ack_delay_exponent), its first
// range and the encoded further ranges, which QuicAckRanges walks, and the ECN counts of type 0x03.
struct QuicAck
{
    u64                 largest     = 0U;
    u64                 delay       = 0U;
    u64                 first_range = 0U;
    u64                 range_count = 0U; // the ranges after the first
    cont::ConstSpan<u8> ranges;           // their Gap and ACK Range Length pairs, already checked
    bool                ecn  = false;
    u64                 ect0 = 0U;
    u64                 ect1 = 0U;
    u64                 ce   = 0U;
};

// One frame. Which fields a kind uses:
//   Padding            data: the run of zero bytes (read: consecutive PADDING bytes are one frame)
//   Ack                ack (read only: write_quic_ack writes ACK frames)
//   ResetStream        stream_id, error_code, value (the final size)
//   StopSending        stream_id, error_code
//   Crypto             offset, data
//   NewToken           data (the token, never empty)
//   Stream             stream_id, offset, data, fin, to_end
//   MaxData            value
//   MaxStreamData      stream_id, value
//   MaxStreams         bidirectional, value (at most kQuicMaxStreams)
//   DataBlocked        value
//   StreamDataBlocked  stream_id, value
//   StreamsBlocked     bidirectional, value (at most kQuicMaxStreams)
//   NewConnectionId    sequence, retire_prior_to (at most sequence), data (the ID, 1 to 20 bytes), reset_token
//   RetireConnectionId sequence
//   PathChallenge      data (kQuicPathDataBytes)
//   PathResponse       data (kQuicPathDataBytes)
//   ConnectionClose    application (type 0x1d), error_code, frame_type (0x1c only), data (the reason phrase)
//   Datagram           data, to_end
// `to_end` is a STREAM or DATAGRAM frame without its Length field, whose data runs to the end of the packet: what
// read_quic_frame reports, and what a writer chooses for a packet's last frame.
struct QuicFrame
{
    QuicFrameKind       kind            = QuicFrameKind::Padding;
    u64                 stream_id       = 0U;
    u64                 offset          = 0U;
    u64                 value           = 0U;
    u64                 error_code      = 0U;
    u64                 frame_type      = 0U;
    u64                 sequence        = 0U;
    u64                 retire_prior_to = 0U;
    cont::ConstSpan<u8> data;
    cont::ConstSpan<u8> reset_token;
    QuicAck             ack;
    bool                fin           = false;
    bool                to_end        = false;
    bool                bidirectional = false;
    bool                application   = false;
};

// Takes one frame from `in`, a packet's decrypted payload: NoError with `out` filled, or the error the connection
// closes with, consuming nothing then. An unknown frame type is FRAME_ENCODING_ERROR (section 12.4).
[[nodiscard]] QuicTransportError read_quic_frame(cont::WireReader& in, QuicFrame& out) noexcept;

// Walks a read ACK frame's ranges from the largest packet numbers down; read_quic_frame checked them, so every range
// it yields lies in [0, largest] below the one before with at least one packet number between.
class QuicAckRanges
{
public:
    explicit QuicAckRanges(const QuicAck& ack) noexcept;

    // The next range, false after the last.
    [[nodiscard]] bool next(QuicAckRange& out) noexcept;

private:
    cont::WireReader m_in;
    u64              m_largest  = 0U; // the first range's
    u64              m_smallest = 0U; // the smallest of the range yielded last
    u64              m_left     = 0U; // ranges still to yield
    bool             m_first    = true;
};

// The bytes write_quic_frame writes for `frame`; 0 for a frame it refuses (an Ack, a field above kQuicMaxVarint or
// above its section 19 bound, a NEW_CONNECTION_ID without a 1- to 20-byte ID and a 16-byte token, path data not 8
// bytes, an empty NEW_TOKEN token, a STREAM or CRYPTO frame whose end passes 2^62 - 1, an empty PADDING frame).
[[nodiscard]] usize quic_frame_bytes(const QuicFrame& frame) noexcept;

// Writes `frame` at the start of `out` and returns its bytes; 0, writing nothing, when it is refused or does not fit.
// A Padding frame writes data.size() zero bytes (its contents are not read).
[[nodiscard]] usize write_quic_frame(const QuicFrame& frame, cont::Span<u8> out) noexcept;

// The ECN counts an ACK frame of type 0x03 reports.
struct QuicEcnCounts
{
    u64 ect0 = 0U;
    u64 ect1 = 0U;
    u64 ce   = 0U;
};

// The bytes of an ACK frame for `ranges` (largest first, each range's smallest at least two above the next range's
// largest, at least one range), ACK Delay `delay` and, when `ecn` is given, type 0x03 with its counts; 0 for ranges
// out of that order or a value above kQuicMaxVarint.
[[nodiscard]] usize quic_ack_bytes(cont::ConstSpan<QuicAckRange> ranges, u64 delay,
                                   const QuicEcnCounts* ecn = nullptr) noexcept;

// Writes that ACK frame at the start of `out` and returns its bytes; 0, writing nothing, when refused or too large.
[[nodiscard]] usize write_quic_ack(cont::ConstSpan<QuicAckRange> ranges, u64 delay, const QuicEcnCounts* ecn,
                                   cont::Span<u8> out) noexcept;

} // namespace crd::net
