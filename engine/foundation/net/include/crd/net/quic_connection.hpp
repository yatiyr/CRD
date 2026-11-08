#pragma once

// crd-net -- a QUIC connection: the TLS handshake carried to confirmation, recovery, the connection's frames and its
// close (4.2a; contract: docs/design/network-foundation.md#quic-connection). Sources: RFC 9000 sections 2 to 4, 5.1,
// 7, 8, 10 (10.3's stateless reset), 12, 13, 14, 17.2, 19 and 21.4, RFC 9001 sections 4 to 6 and 9.2, RFC 9002
// sections 6 and 6.4 and appendix A, RFC 9221 sections 3 to 5, RFC 9287, RFC 9368 sections 2 to 4 and RFC 9369
// sections 4 and 5.
//
//   sans-I/O   receive() takes one datagram from the peer with its arrival time, send() writes the next datagram into
//              the caller's room (called until it returns 0 after every receive() and on_timeout()), and
//              next_timer_ns() names when on_timeout() is due. Nothing reads a clock or a socket.
//   handshake  each level's CRYPTO data goes through S38's buffers into crypto::QuicHandshake and back; every key it
//              has ready is installed after each provide(); the peer's transport parameters are read and checked as
//              they arrive (section 7.3's connection IDs, RFC 9368's version_information); the server confirms on
//              completion and sends HANDSHAKE_DONE until acknowledged, the client confirms on reading it; Initial and
//              Handshake keys and their recovery spaces are discarded at RFC 9001 section 4.9's moments.
//   versions   compatible version negotiation (RFC 9368 section 2.3, RFC 9369 section 4.1): a server reads its
//              client's version_information while the TLS stack reads the ClientHello, before anything is sent, and
//              runs the connection in the first of its versions that the client lists and its first flight converts
//              to: its parameters name it and its CRYPTO frames, Handshake and 1-RTT packets go in it, while the
//              client's Initial packets still open in the original version until the Initial keys go. A client
//              learns the switch from the first server Initial in another version it offered that opens; a server
//              CRYPTO frame in the original version settles the original. Other versions' long headers are then
//              dropped, and tickets and tokens belong to the version the connection runs (RFC 9369 section 5).
//   0-RTT      a client whose ticket allows early data (RFC 9001 section 4.6) sends its streams and datagrams in 0-RTT
//              packets of the original version under the parameters it remembers (RFC 9000 section 7.4.1), until its
//              server's parameters arrive; acceptance with a lowered limit closes with PROTOCOL_VIOLATION, a
//              rejection discards the 0-RTT packets' recovery state (RFC 9002 section 6.4) and resets every stream
//              and the queued datagrams (RFC 9001 section 4.6.2), which handshake().early_data() reports. A server
//              names its early data context (the parameters a client remembers and the application's part), opens
//              0-RTT packets only once it accepted, marks what they carried as replayable (QuicStreamRead::
//              early_bytes, QuicDatagrams::oldest_early) and discards the 0-RTT keys three PTOs after its first 1-RTT
//              packet (section 4.9.3). Packets whose keys may still come are held in a bounded ring and read once
//              they come (section 4.1.4): a server's 0-RTT packets before its ClientHello is complete, Handshake and
//              1-RTT packets ahead of the flight that installs their keys; a server opens no 1-RTT packet before its
//              handshake completes (section 5.7).
//   sending    one datagram coalesces an Initial, a Handshake and a 1-RTT packet, each with its owed ACK; a datagram
//              carrying a client's Initial, a server's ack-eliciting Initial or a PATH_RESPONSE reaches 1200 bytes; a
//              server sends at most three times what it received before the client's address is proven (section
//              8.1); the QUIC bit is greased toward a peer that sent grease_quic_bit and 1-RTT packet numbers are
//              skipped from entropy (section 21.4), so an acknowledgement of one closes with PROTOCOL_VIOLATION.
//   frames     every frame type is dispatched: stream and flow-control frames go to the QuicStreamSet (streams()),
//              DATAGRAM frames to QuicDatagrams (datagrams()); 1-RTT packets carry the connection's own frames, then
//              datagrams, then the stream set's. An ACK only carried along follows waiting datagrams.
//              The peer's connection IDs are held up to this side's active_connection_id_limit with retirement; a
//              PATH_CHALLENGE is answered.
//   IDs        this side's connection IDs come from the host (QuicConnectionIds), which routes them and derives their
//              stateless reset tokens from its QuicResetKey: after confirmation the peer is kept at its
//              active_connection_id_limit (at most kQuicMaxLocalConnectionIds) with NEW_CONNECTION_ID frames resent
//              until acknowledged, a retired ID is handed back and replaced, and rotate_connection_ids() retires them
//              all with Retire Prior To. A datagram whose first packet does not open and that ends in the token of a
//              peer's ID this side used is a stateless reset: the connection drains (section 10.3.1).
//   close      an error closes with CONNECTION_CLOSE at the levels the peer may read (section 10.2.3), CRYPTO_ERROR
//              at an alert's level; closing resends it at a falling rate for three PTOs, a peer's close drains for
//              three PTOs, and the idle timeout (section 10.1) closes silently.
//   memory     every buffer comes from the injected allocator at construction; receiving and sending allocate
//              nothing.
//   threads    a connection belongs to its loop's thread.

#include <crd/containers/span.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/quic_datagram.hpp>
#include <crd/net/quic_packet.hpp>
#include <crd/net/quic_recovery.hpp>
#include <crd/net/quic_stream.hpp>
#include <crd/net/quic_stream_set.hpp>
#include <crd/net/quic_transport_parameters.hpp>

namespace crd::memory
{
class IAllocator;
} // namespace crd::memory

namespace crd::net
{
inline constexpr usize kQuicMaxPeerConnectionIds = 8U;  // the peer's IDs a connection holds at most
inline constexpr usize kQuicMaxReasonBytes = 64U;       // of a CONNECTION_CLOSE reason phrase kept or sent
inline constexpr usize kQuicMaxLocalConnectionIds = 8U; // this side's active IDs at most
// The application's part of a server's early data context at most (QuicConnectionConfig::early_context).
inline constexpr usize kQuicMaxEarlyContextBytes = crypto::kTlsMaxEarlyDataContextBytes - kQuicEarlyParameterBytes;
inline constexpr u32 kQuicMaxHeldPackets = 64U; // QuicConnectionConfig::held_packets at most
// A token a connection carries: a client's from Retry or NEW_TOKEN, this build's or another server's (RFC 9000 sets
// no bound; the Initial still holds 1200 bytes with the longest connection IDs and its CRYPTO data).
inline constexpr usize kQuicMaxCarriedTokenBytes = 256U;
// This side pads every datagram to kQuicResetPadding bytes more than its connection ID length, so that a peer's
// stateless reset answering it can look like a packet (RFC 9000 section 10.3).
inline constexpr usize kQuicResetPadding = 22U;

// The host's side of a connection's own connection IDs: it routes each one to the connection and derives its
// stateless reset token from its QuicResetKey (RFC 9000 section 10.3.2), so that it can still reset a connection
// whose state it lost. Called on the connection's thread; it outlives the connection.
class QuicConnectionIds
{
public:
    virtual ~QuicConnectionIds() = default;
    // A fresh ID of `id.size()` bytes (sequence 0's length) from at least 128 random bits, now routed to the
    // connection, and its token into `token` (kQuicResetTokenBytes). False when none can be routed.
    [[nodiscard]] virtual bool issue(cont::Span<u8> id, cont::Span<u8> token) noexcept = 0;
    // The token of an ID the host routes already: a server's sequence 0, sent as stateless_reset_token.
    [[nodiscard]] virtual bool token(cont::ConstSpan<u8> id, cont::Span<u8> token) noexcept = 0;
    // Stops routing `id`: the peer retired it, or the connection closed (every ID it holds, sequence 0 included).
    virtual void retire(cont::ConstSpan<u8> id) noexcept = 0;
    // The peer's stateless reset `token` is live: the connection has sent to its ID, which is not retired (RFC 9000
    // section 10.3.1). A host that matches datagrams for no connection against tokens keeps it until unwatch_reset().
    virtual void watch_reset(cont::ConstSpan<u8> token) noexcept;
    virtual void unwatch_reset(cont::ConstSpan<u8> token) noexcept;

protected:
    QuicConnectionIds() = default;
    QuicConnectionIds(const QuicConnectionIds&) = default;
    QuicConnectionIds(QuicConnectionIds&&) noexcept = default;
    QuicConnectionIds& operator=(const QuicConnectionIds&) = default;
    QuicConnectionIds& operator=(QuicConnectionIds&&) noexcept = default;
};

// The host side over a loop's ConnectionIdTable and its endpoint's QuicResetKey: IDs drawn from the provider's
// randomness and routed to `connection`, and, in `peer_tokens` when given, the connection's live peer tokens.
class QuicRoutedIds final : public QuicConnectionIds
{
public:
    QuicRoutedIds(ConnectionIdTable& routes, const crypto::QuicResetKey& reset, u32 connection,
                  ConnectionIdTable* peer_tokens = nullptr) noexcept
        : m_routes(routes), m_reset(reset), m_peer_tokens(peer_tokens), m_connection(connection)
    {
    }
    [[nodiscard]] bool issue(cont::Span<u8> id, cont::Span<u8> token) noexcept override;
    [[nodiscard]] bool token(cont::ConstSpan<u8> id, cont::Span<u8> token) noexcept override;
    void retire(cont::ConstSpan<u8> id) noexcept override;
    void watch_reset(cont::ConstSpan<u8> token) noexcept override;
    void unwatch_reset(cont::ConstSpan<u8> token) noexcept override;

private:
    ConnectionIdTable& m_routes;
    const crypto::QuicResetKey& m_reset;
    ConnectionIdTable* m_peer_tokens = nullptr;
    u32 m_connection = kNoConnection;
};

struct QuicConnectionConfig
{
    crypto::QuicSide side = crypto::QuicSide::Client;
    crypto::QuicVersion version = crypto::QuicVersion::V1; // the version this connection runs
    const crypto::TlsContext* tls = nullptr;               // of `side`'s role, with an application protocol
    crypto::TlsTicketStore* tickets = nullptr;
    // Where this side's further connection IDs come from (null: sequence 0 alone, and no stateless reset token).
    QuicConnectionIds* ids = nullptr;
    // This side's parameters. The connection fills the connection IDs and a server's stateless_reset_token,
    // version_information, grease_quic_bit, the
    // stream and flow-control parameters (from `streams`) and max_datagram_frame_size (from `datagrams`), so those
    // stay 0 here; active_connection_id_limit is at most kQuicMaxPeerConnectionIds.
    QuicTransportParameters parameters;
    QuicStreamLimits streams;     // what this side's streams hold (all zero: no streams)
    QuicDatagramLimits datagrams; // what this side's datagrams hold (all zero: none either way)
    // The versions this side accepts in its preference (version_information's list, containing `version`); empty
    // for `version` alone. A client's first flight converts to the others compatible with `version`, so its server
    // may switch to one of them; a server switches to the first of them its client lists (RFC 9368 section 2.3).
    cont::ConstSpan<u32> versions;
    // A client whose connection follows a Version Negotiation packet (RFC 9368 section 4).
    bool after_version_negotiation = false;
    // A client's NEW_TOKEN token from an earlier connection to this server that ran in `version` (at most
    // kQuicMaxCarriedTokenBytes; RFC 9369 section 5), carried in its Initial packets to prove its address (section
    // 8.1.3) until a Retry gives it another.
    cont::ConstSpan<u8> token;
    // A server whose TLS context accepts early data: the application's part of its early data context, at most
    // kQuicMaxEarlyContextBytes (its protocol's settings and its message schema's digest, encoded the same way on
    // every connection). The connection puts the parameters a client remembers before it (write_quic_early_parameters),
    // so a ticket's 0-RTT is accepted only while both are unchanged (RFC 9000 section 7.4.1). Refused elsewhere.
    cont::ConstSpan<u8> early_context;
    // Packets held while the keys that open them may still come (RFC 9001 section 4.1.4), each of at most
    // max_datagram_bytes; at most kQuicMaxHeldPackets, 0 for none.
    u32 held_packets = 8U;
    bool grease_quic_bit = true; // send grease_quic_bit and accept a cleared QUIC bit (RFC 9287)
    u32 sent_packets = 128U;     // per packet number space: what the connection holds in flight
    // The frames of those packets settled on acknowledgement or loss (CRYPTO, stream and credit frames, ...): eight
    // per packet, so a packet carrying several streams' frames still fits; a full ledger holds frames back.
    u32 sent_frames = 1024U;
    u32 crypto_bytes = 16384U; // per level, each direction: a multiple of kQuicStreamChunk, at least 4096
    u32 max_datagram_bytes = kQuicMinInitialDatagram; // what this side sends at most (NET-PATH raises it)
    u64 initial_rtt_ns = kQuicInitialRttNs;
    u64 key_update_interval = 0U; // 1-RTT packets one key seals before it is updated (0: the AEAD's limit)
    // kPrfKeyBytes seeding the connection's entropy (grease, skipped numbers) for a deterministic run (tests,
    // NET-SIM); empty draws a fresh key from the provider.
    cont::ConstSpan<u8> entropy_seed;
};

// A server's view of the client Initial that opens the connection (from the Ingress's decision).
struct QuicAccept
{
    cont::ConstSpan<u8> local_cid;     // the ID this server issues (sequence 0): the Ingress table's key
    cont::ConstSpan<u8> client_dcid;   // the Initial's Destination Connection ID: its keys
    cont::ConstSpan<u8> client_scid;   // the Initial's Source Connection ID
    cont::ConstSpan<u8> original_dcid; // the client's first Destination Connection ID (a Retry token's claim)
    bool retried = false;              // client_dcid is this server's Retry's Source Connection ID
    bool validated = false;            // a token proved the client's address
};

enum class QuicConnectionState : u8
{
    Idle,        // before connect() or accept()
    Handshaking, // the TLS handshake runs
    Established, // the handshake completed (confirmed() tells whether it is confirmed)
    Closing,     // this side closed: CONNECTION_CLOSE is resent for three PTOs
    Draining,    // the peer closed: nothing is sent for three PTOs
    Closed,      // done: the connection may be destroyed
};

// Why a connection closed.
struct QuicCloseInfo
{
    bool application = false;     // an application error code (CONNECTION_CLOSE 0x1d)
    bool local = false;           // this side closed
    bool idle = false;            // the idle timeout ended it, without a frame
    bool stateless_reset = false; // the peer's stateless reset ended it (RFC 9000 section 10.3.1)
    // A client's Version Negotiation packet listed none of the versions it runs: `retry_version` is the first of its
    // preference the packet offered (0 for none), for a new connection with after_version_negotiation set.
    bool version_negotiation = false;
    u32 retry_version = 0U;
    u64 error_code = 0U; // a QuicTransportError's value, or the application's
    u64 frame_type = 0U;
};

struct QuicConnectionCounters
{
    u64 datagrams_received = 0U;
    u64 bytes_received = 0U; // every byte of every datagram, opened or not
    u64 datagrams_sent = 0U;
    u64 bytes_sent = 0U;
    u64 packets_dropped = 0U; // packets that did not open, duplicates and packets of another connection
    u64 packets_held = 0U;    // packets held until their keys came (RFC 9001 section 4.1.4)
};

// (MSVC C4324 as for QuicPacketProtection, which it holds inline.)
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
class QuicConnection
{
public:
    QuicConnection(memory::IAllocator& allocator, const QuicConnectionConfig& config) noexcept;
    ~QuicConnection() noexcept;
    QuicConnection(const QuicConnection&) = delete;
    QuicConnection& operator=(const QuicConnection&) = delete;
    QuicConnection(QuicConnection&&) = delete;
    QuicConnection& operator=(QuicConnection&&) = delete;

    // False when the configuration is out of range (no TLS context, stream credit or a datagram size in the
    // parameters, stream or datagram limits out of range, a version list without `version`, sizes out of bounds, a seed
    // of another length) or the allocator or the provider refused.
    [[nodiscard]] bool ready() const noexcept { return m_storage != nullptr; }

    // A client's start: it issues `local_cid` (0 to 20 bytes) and sends its first Initial to `initial_dcid` (8 to 20
    // bytes, from entropy, RFC 9000 section 7.2). A server's: the client Initial `accept` describes, which the
    // caller then passes to receive(). False, changing nothing, when not Idle, for the other side, for connection
    // IDs out of range or when the handshake refuses its configuration.
    [[nodiscard]] bool connect(cont::ConstSpan<u8> local_cid, cont::ConstSpan<u8> initial_dcid, u64 now_ns) noexcept;
    [[nodiscard]] bool accept(const QuicAccept& accept, u64 now_ns) noexcept;

    // One datagram from the peer, arrived at `now_ns` with `ecn`; its bytes are decrypted in place.
    void receive(cont::Span<u8> datagram, QuicEcn ecn, u64 now_ns) noexcept;
    // Writes the next datagram into `out` (at least kQuicMinInitialDatagram bytes) and returns its size; 0 when
    // nothing is to be sent now.
    [[nodiscard]] usize send(cont::Span<u8> out, u64 now_ns) noexcept;
    // When on_timeout() is next due: kNeverNs for no timer.
    [[nodiscard]] u64 next_timer_ns() const noexcept;
    void on_timeout(u64 now_ns) noexcept;

    // Closes with an application error (sent as APPLICATION_ERROR without a reason in Initial and Handshake
    // packets, section 10.2.3) and up to kQuicMaxReasonBytes of `reason`. Nothing happens once closing.
    void close(u64 application_error, cont::ConstSpan<u8> reason, u64 now_ns) noexcept;
    // Owes one ack-eliciting 1-RTT packet: a keep-alive before the idle timeout (section 10.1.2).
    void ping() noexcept { m_ping_owed = true; }
    // Issues a fresh set of connection IDs whose NEW_CONNECTION_ID frames retire every earlier one (Retire Prior To,
    // section 5.1.2); the earlier IDs stay routed until the peer retires them. False, changing nothing, before
    // confirmation, without a host, with zero-length IDs, while an earlier rotation waits for retirements, or when
    // the host routes no new ID.
    [[nodiscard]] bool rotate_connection_ids() noexcept;
    // Whether `datagram`, which reached this connection's peer address and opened nothing, is the peer's stateless
    // reset: it ends in the token of a peer's ID this side used and has not retired, compared in constant time. The
    // connection then drains (section 10.3.1). receive() checks its own datagrams; the host calls this for a datagram
    // from the peer's address that routes to no connection.
    bool stateless_reset(cont::ConstSpan<u8> datagram, u64 now_ns) noexcept;

    [[nodiscard]] QuicConnectionState state() const noexcept { return m_state; }
    [[nodiscard]] bool confirmed() const noexcept { return m_confirmed; }
    // A client accepted its server's Retry; a server's connection follows its own Retry.
    [[nodiscard]] bool retried() const noexcept { return m_retried; }
    // The version the connection runs: the configured one until a compatible switch (final once a server has read
    // its client's parameters, and once a client has read its server's first packet in another version or CRYPTO
    // data in this one).
    [[nodiscard]] crypto::QuicVersion version() const noexcept { return m_protection.version(); }
    [[nodiscard]] bool address_validated() const noexcept { return m_validated; }
    [[nodiscard]] const QuicCloseInfo& close_info() const noexcept { return m_close; }
    // The reason phrase sent or received (truncated to kQuicMaxReasonBytes).
    [[nodiscard]] cont::ConstSpan<u8> close_reason() const noexcept
    {
        return cont::ConstSpan<u8>(m_reason, m_reason_bytes);
    }
    [[nodiscard]] const QuicConnectionCounters& counters() const noexcept { return m_counters; }
    // The TLS handshake: early_data() tells a client whether its server accepted 0-RTT (Rejected: every stream was
    // reset and the queued datagrams dropped, so the application opens and sends them again) and a server whether
    // it accepted; a server's application checks capability tokens of early data against it (verify_early).
    [[nodiscard]] const crypto::QuicHandshake& handshake() const noexcept { return m_handshake; }
    // The connection's streams: opened, written, read and polled by the application, their frames sent by send().
    [[nodiscard]] QuicStreamSet& streams() noexcept { return m_streams; }
    [[nodiscard]] const QuicStreamSet& streams() const noexcept { return m_streams; }
    // The connection's datagrams: queued, read and polled by the application, their frames sent by send().
    [[nodiscard]] QuicDatagrams& datagrams() noexcept { return m_datagrams; }
    [[nodiscard]] const QuicDatagrams& datagrams() const noexcept { return m_datagrams; }
    [[nodiscard]] const QuicPacketProtection& protection() const noexcept { return m_protection; }
    [[nodiscard]] const QuicRecovery& recovery() const noexcept { return *m_recovery; }
    // The peer's parameters, valid once peer_parameters_known().
    [[nodiscard]] bool peer_parameters_known() const noexcept { return m_peer_known; }
    [[nodiscard]] const QuicTransportParameters& peer_parameters() const noexcept { return m_peer; }
    // The Destination Connection ID this side sends to, and the peer's IDs held (sequence 0 first among them).
    [[nodiscard]] cont::ConstSpan<u8> peer_cid() const noexcept;
    [[nodiscard]] usize peer_cid_count() const noexcept { return m_peer_cid_count; }
    // This side's IDs still routed (sequence 0 included until retired).
    [[nodiscard]] usize local_cid_count() const noexcept { return m_local_id_count; }
    // A client: the latest NEW_TOKEN token its server sent (empty for none), for its next connection there in
    // version(). A server: the token it sends.
    [[nodiscard]] cont::ConstSpan<u8> new_token() const noexcept
    {
        return cont::ConstSpan<u8>(m_new_token, m_new_token_bytes);
    }
    // A server's NEW_TOKEN token for its client's next connection (crypto::QuicTokenKeys::mint_new_token for
    // version(), which is final once peer_parameters_known(): RFC 9369 section 5), sent once the handshake is
    // confirmed and again until acknowledged (section 8.1.3). False, changing nothing, on a client, before
    // peer_parameters_known(), once closing, for an empty token or one over kQuicMaxCarriedTokenBytes, or when it
    // has one.
    [[nodiscard]] bool give_new_token(cont::ConstSpan<u8> token) noexcept;
    // The packet number a space sends next, and whether `number` was skipped there (tests and NET-DEBUG).
    [[nodiscard]] u64 next_packet_number(QuicSpace space) const noexcept;
    [[nodiscard]] bool skipped(QuicSpace space, u64 number) const noexcept;

private:
    enum class Settle : u8
    {
        Acked,
        Lost,
        Discarded,
    };

    // A server's answer to its client's transport parameters (RFC 9368 section 2.3), asked by the TLS stack.
    class HelloAnswer final : public crypto::QuicHelloAnswer
    {
    public:
        explicit HelloAnswer(QuicConnection& connection) noexcept : m_connection(connection) {}
        [[nodiscard]] usize answer(cont::ConstSpan<u8> client, crypto::QuicVersion& version,
                                   cont::Span<u8> out) noexcept override;

    private:
        QuicConnection& m_connection;
    };

    // Recovery's per-packet events, settling the frames each packet carried.
    class Events final : public QuicRecoveryEvents
    {
    public:
        explicit Events(QuicConnection& connection) noexcept : m_connection(connection) {}
        void on_acked(QuicSpace space, const QuicSentPacket& packet) noexcept override;
        void on_lost(QuicSpace space, const QuicSentPacket& packet) noexcept override;
        void on_discarded(QuicSpace space, const QuicSentPacket& packet) noexcept override;

    private:
        QuicConnection& m_connection;
    };

    struct Level
    {
        QuicReceiveBuffer in;
        QuicSendBuffer out;
    };

    // A frame a sent packet carried that is settled when the packet is acknowledged, lost or discarded. A packet's
    // records are consecutive in the ring, the first named by its QuicSentPacket::token.
    struct SentFrame
    {
        u64 packet = 0U;
        QuicStreamRecord frame; // CRYPTO: value and length; RETIRE_CONNECTION_ID: value; a stream set's frame
        u8 space = 0U;
        u8 kind = 0U; // 0 for a settled record
    };

    // What write_frames put into a packet.
    struct Written
    {
        u64 carried_ack = kQuicNoPacket;
        u32 token = ~u32{0};
        bool eliciting = false;
        bool path_response = false;
    };

    struct PeerCid
    {
        u64 sequence = 0U;
        QuicConnectionId id;
        u8 reset_token[kQuicResetTokenBytes] = {};
        bool has_token = false;
        bool used = false; // a packet was sent to it: its token is checked (section 10.3.1)
    };

    // A packet held until its keys come; its bytes are a max_datagram_bytes slot of m_held_bytes.
    struct Held
    {
        u32 bytes = 0U;
        QuicPacketType type = QuicPacketType::OneRtt;
        QuicEcn ecn = QuicEcn::NotEct;
    };

    struct LocalCid
    {
        u64 sequence = 0U;
        QuicConnectionId id;
        u8 reset_token[kQuicResetTokenBytes] = {};
        bool owed = false;  // its NEW_CONNECTION_ID frame waits to be sent
        bool acked = false; // the peer acknowledged it (sequence 0: the handshake carried it)
    };

    [[nodiscard]] bool start(u64 now_ns) noexcept;
    // This side's parameters encoded into `out` with fresh grease: their size, 0 when they do not fit.
    [[nodiscard]] usize encode_parameters(cont::Span<u8> out) noexcept;
    // Whether a long header's version is one this connection reads now (RFC 9369 section 4.1).
    [[nodiscard]] bool version_routed(const QuicHeader& header) const noexcept;
    [[nodiscard]] u64 entropy() noexcept;

    // Receiving.
    // Whether the packet opened.
    [[nodiscard]] bool receive_packet(const QuicHeader& header, cont::Span<u8> packet, usize datagram_bytes,
                                      QuicEcn ecn, u64 now_ns) noexcept;
    [[nodiscard]] bool on_frame(const QuicFrame& frame, QuicSpace space, u64 now_ns) noexcept;
    [[nodiscard]] bool crypto_frame(const QuicFrame& frame, QuicSpace space, u64 now_ns) noexcept;
    [[nodiscard]] bool advance_handshake(u64 now_ns) noexcept;
    [[nodiscard]] bool read_peer_parameters(u64 now_ns) noexcept;
    // The peer's grants to this side's streams and datagrams: a client's remembered ones for 0-RTT, then its server's.
    void apply_peer_limits(const QuicTransportParameters& peer) noexcept;
    // A client: starts 0-RTT on the parameters its ticket remembers. Both: acts on the server's 0-RTT decision.
    [[nodiscard]] bool begin_early() noexcept;
    [[nodiscard]] bool settle_early(u64 now_ns) noexcept;
    void reject_early(u64 now_ns) noexcept;
    void hold(QuicPacketType type, cont::ConstSpan<u8> packet, QuicEcn ecn) noexcept;
    // Reads the held packets whose keys came, until none more opens; drops those whose keys went.
    void read_held(u64 now_ns) noexcept;
    [[nodiscard]] bool new_connection_id(const QuicFrame& frame, u64 now_ns) noexcept;
    [[nodiscard]] bool owe_retire(u64 sequence, u64 now_ns) noexcept;
    // The host learns or forgets a peer ID's token, which counts once it is known and the ID has been sent to.
    void watch(const PeerCid& held) noexcept;
    void unwatch(const PeerCid& held) noexcept;
    [[nodiscard]] bool retire_local(u64 sequence, u64 now_ns) noexcept;
    [[nodiscard]] usize local_index(cont::ConstSpan<u8> id) const noexcept;
    // Issues IDs until the peer holds its active_connection_id_limit of this side's (after confirmation).
    void issue_local_ids() noexcept;
    // Hands every ID held back to the host and closes the connection.
    void finish() noexcept;
    // A stateless reset by `tail`, a datagram's last kQuicResetTokenBytes.
    [[nodiscard]] bool reset_by(cont::ConstSpan<u8> tail, u64 now_ns) noexcept;
    void version_negotiation(const QuicHeader& header, cont::ConstSpan<u8> packet) noexcept;
    // A client's Retry: true when it was accepted and the Initial goes again with its token (section 17.2.5.2).
    [[nodiscard]] bool retry(const QuicHeader& header, cont::ConstSpan<u8> packet, u64 now_ns) noexcept;
    void discard_space(QuicSpace space, u64 now_ns) noexcept;
    void confirm(u64 now_ns) noexcept;

    // Sending.
    // The packet type `space` sends now: a client's 0-RTT for the Application space before it has 1-RTT keys.
    [[nodiscard]] QuicPacketType send_type(QuicSpace space) const noexcept;
    [[nodiscard]] bool has_content(QuicSpace space, u64 now_ns) const noexcept;
    // Whether a datagram can go now: one waits, and its outcome has a ledger record free when it is reported.
    [[nodiscard]] bool datagram_ready(u64 now_ns) const noexcept;
    // `pad`: the datagram reaches 1200 bytes (section 14.1), by this packet when it is the `last` or when what it
    // leaves is too small for another.
    [[nodiscard]] usize write_packet(QuicSpace space, cont::Span<u8> room, usize datagram_used, bool pad, bool last,
                                     bool closing, u64 now_ns) noexcept;
    [[nodiscard]] usize write_frames(QuicSpace space, bool early, cont::Span<u8> frames, u64 packet, u64 now_ns,
                                     Written& written) noexcept;
    [[nodiscard]] usize write_close(QuicSpace space, cont::Span<u8> frames) const noexcept;
    [[nodiscard]] usize send_closing(cont::Span<u8> out, u64 now_ns) noexcept;
    // Appends a record of a frame `written` holds to the ledger, naming it the packet's first when it is.
    [[nodiscard]] bool record(QuicSpace space, u64 packet, u8 kind, const QuicStreamRecord& frame,
                              Written& written) noexcept;
    void settle(QuicSpace space, const QuicSentPacket& packet, Settle how) noexcept;
    void pull_crypto() noexcept;
    [[nodiscard]] usize amplification_credit() const noexcept;
    // Whether send() can write `space`'s owed ACK: a timer nothing serves is never reported due.
    [[nodiscard]] bool can_acknowledge(QuicSpace space) const noexcept;

    // Closing and timers.
    void fail(QuicTransportError error, u64 frame_type, u64 now_ns) noexcept;
    void fail_code(u64 error, u64 frame_type, crypto::QuicLevel level, u64 now_ns) noexcept;
    void enter(QuicConnectionState state, u64 now_ns) noexcept;
    void restart_idle(u64 now_ns) noexcept;
    [[nodiscard]] u64 three_ptos() const noexcept;

    memory::IAllocator& m_allocator;
    QuicConnectionConfig m_config; // its spans are copied below and cleared
    QuicSentEntry* m_storage = nullptr;
    SentFrame* m_frames = nullptr;
    u8* m_crypto_bytes = nullptr;
    u64* m_crypto_bits = nullptr;
    QuicRecovery* m_recovery = nullptr;
    QuicStreamSet m_streams;
    QuicDatagrams m_datagrams;
    Events m_events;
    HelloAnswer m_hello;
    u32 m_frame_head = 0U;
    u32 m_frame_count = 0U;
    Level m_levels[kQuicSpaces];
    crypto::QuicHandshake m_handshake;
    QuicPacketProtection m_protection;
    crypto::PrfKey m_entropy;
    u64 m_entropy_counter = 0U;
    u32 m_versions[kQuicMaxAvailableVersions] = {};
    usize m_version_count = 0U;
    u8 m_token[kQuicMaxCarriedTokenBytes] = {};
    usize m_token_bytes = 0U;
    u8* m_early_context = nullptr; // a server's with early data: kQuicEarlyParameterBytes, then the application's part
    usize m_early_context_bytes = 0U;
    u8* m_held_bytes = nullptr;
    Held* m_held = nullptr;
    u32 m_held_count = 0U;

    QuicConnectionState m_state = QuicConnectionState::Idle;
    QuicTransportParameters m_local;
    QuicTransportParameters m_peer;
    bool m_peer_known = false;
    QuicTransportParameters m_remembered; // a client's for 0-RTT: its server's from the ticket's connection
    bool m_early_sending = false;         // a client sends 0-RTT under m_remembered until its server decides
    bool m_early_settled = false;         // a server decided on 0-RTT
    bool m_packet_early = false;          // the packet being read is a 0-RTT packet
    u64 m_early_key_ns = kNeverNs;        // when a server's 0-RTT keys go (RFC 9001 section 4.9.3)
    QuicConnectionId m_local_cid;
    QuicConnectionId m_initial_dcid;  // a client's first Destination Connection ID; a server's client_dcid
    QuicConnectionId m_original_dcid; // what original_destination_connection_id names
    QuicConnectionId m_peer_scid;     // the Source Connection ID of the peer's long headers
    bool m_peer_scid_known = false;
    bool m_retried = false;
    bool m_version_settled = false; // a client's: its server's version is known
    u32 m_packet_version = 0U;      // the long header version of the packet being read (0 for a short header)
    LocalCid m_local_ids[2U * kQuicMaxLocalConnectionIds]; // the active ones and those a rotation retires
    usize m_local_id_count = 0U;
    usize m_packet_local = 0U; // which of them the packet being read was sent to (m_local_id_count: none)
    u64 m_local_next_sequence = 1U;
    u64 m_local_retire_prior_to = 0U; // this side's Retire Prior To
    PeerCid m_peer_cids[kQuicMaxPeerConnectionIds];
    usize m_peer_cid_count = 0U;
    usize m_peer_cid_current = 0U;
    u64 m_retire_prior_to = 0U;
    u64 m_retire_owed[2U * kQuicMaxPeerConnectionIds] = {};
    usize m_retire_owed_count = 0U;
    usize m_retire_outstanding = 0U; // owed or in flight
    u8 m_new_token[kQuicMaxCarriedTokenBytes] = {};
    usize m_new_token_bytes = 0U;
    bool m_new_token_owed = false; // a server's, until acknowledged
    bool m_new_token_acked = false;

    u64 m_next_number[kQuicSpaces] = {};
    u64 m_skipped[kQuicSpaces] = {kQuicNoPacket, kQuicNoPacket, kQuicNoPacket}; // the latest number skipped
    u8 m_probes[kQuicSpaces] = {};
    u32 m_ack_only[kQuicSpaces] = {}; // ACK-only packets sent since the last ack-eliciting one
    bool m_space_gone[kQuicSpaces] = {};
    bool m_handshake_keys = false;
    bool m_handshake_done_owed = false;
    bool m_handshake_done_acked = false;
    u8 m_path_response[kQuicPathDataBytes] = {};
    bool m_path_response_owed = false;
    bool m_ping_owed = false;
    bool m_confirmed = false;
    bool m_validated = false;
    bool m_peer_greases = false;
    bool m_sent_since_receive = false;

    u64 m_idle_ns = 0U; // the negotiated idle timeout, 0 for none
    u64 m_idle_deadline_ns = kNeverNs;
    u64 m_close_deadline_ns = kNeverNs;
    u64 m_previous_key_ns = kNeverNs; // when the previous 1-RTT receive key goes
    u64 m_close_owed_after = 1U;      // datagrams received in Closing before the close goes again
    u64 m_close_received = 0U;
    bool m_close_owed = false;
    crypto::QuicLevel m_close_level = crypto::QuicLevel::Initial; // the highest level the close is sent at
    QuicCloseInfo m_close;
    u8 m_reason[kQuicMaxReasonBytes] = {};
    usize m_reason_bytes = 0U;

    QuicConnectionCounters m_counters;
};
#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif

} // namespace crd::net
