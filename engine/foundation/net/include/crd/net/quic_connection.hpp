#pragma once

// crd-net -- a QUIC connection: the TLS handshake carried to confirmation, recovery, the connection's frames and its
// close (4.2a; contract: docs/design/network-foundation.md#quic-connection). Sources: RFC 9000 sections 2 to 4, 5.1,
// 7, 8, 10 (10.3's stateless reset), 12, 13, 14, 17.2, 19 and 21.4, RFC 9001 sections 4 to 6 and 9.2, RFC 9002
// sections 6, 6.4 and 7 and appendix A, RFC 9221 sections 3 to 5, RFC 9287, RFC 9368 sections 2 to 4 and RFC 9369
// sections 4 and 5; for paths RFC 9000 sections 8.2 and 9.
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
//   congestion 0-RTT and 1-RTT data go while the congestion controller's window has room for a whole datagram and
//              the pacer's release time has come (RFC 9002 sections 7 and 7.7; congestion.hpp); ACK-only packets,
//              probes and the handshake's CRYPTO data are not held (data_allowed()). Data held back tells the
//              controller its window is used (section 7.8), and a pacer's release time is a timer of next_timer_ns().
//              While the controller's bandwidth probe wants bytes the application has not given (probe_bytes(), GCC),
//              each 1-RTT packet is ack-eliciting and fills its datagram, inside the window and the pacer as any.
//              The policy is the configuration's, swappable live. A link that hands over on a timetable (handover.hpp,
//              LEO satellites) has its schedule: a packet sent where a handover may take it gives its controller no
//              loss or delay signal (StarQUIC's window freeze).
//   path MTU   datagrams take the path's size (quic_pmtu.hpp: RFC 8899, RFC 9000 section 14.3): 1200 bytes, or the
//              configured base, until the handshake is confirmed, then PMTU probes (one 1-RTT packet of PING and
//              PADDING, alone in its datagram and inside the window) search up to max_datagram_bytes and the peer's
//              max_udp_payload_size. A black hole falls back to 1200 bytes; datagrams keep to 1200 bytes while a probe
//              timeout is pending. The controller, the DATAGRAM size and the window's unit follow the size found;
//              probe losses are no congestion signal (section 14.4).
//   ecn        datagrams carry the controller's codepoint (ECT(1) for a scalable one) while the path is tested and once
//              proved to carry ECN (quic_ecn.hpp: RFC 9000 section 13.4); on a validated path each ACK frame's
//              marked packets and CE rise go to the controller (RFC 9002 section 7.1), and a path that turns out to
//              carry no ECN is reported to it once. Received codepoints are counted per space
//              and reported in ACK frames (QuicAckTracker). The host stamps marking() on each datagram it sends.
//   frames     every frame type is dispatched: stream and flow-control frames go to the QuicStreamSet (streams()),
//              DATAGRAM frames to QuicDatagrams (datagrams()); 1-RTT packets carry the connection's own frames, then
//              datagrams, then the stream set's. An ACK only carried along follows waiting datagrams.
//              The peer's connection IDs are held up to this side's active_connection_id_limit with retirement.
//   paths      every datagram arrives on a QuicPath and send() names the path of the one it wrote (sent_path(),
//              quic_path.hpp; design S75). A PATH_CHALLENGE is answered on its own path. A server follows its client:
//              once confirmed, the highest-numbered non-probing packet from another path moves its datagrams there
//              (a NAT rebinding or the client's migration), with an unused peer connection ID when the client changed
//              its own (section 9.5); the new path is validated and held to three times what arrived on it until
//              then, and the path before it is challenged too (section 9.3.3), or abandoned if never validated
//              (section 9.3.2). Datagrams to the new path keep to 1200 bytes until it is validated; then, unless
//              only the port changed, the path MTU search, ECN validation, congestion control, the RTT estimate and
//              the pacer start again, nothing sent before reaching the new estimate (section 9.4). A validation that
//              fails reverts to the last validated path, or closes silently without one (section 9.3.2). A validated
//              path left behind stays known, so the client may return without another validation, until a new path
//              needs its slot; its connection ID is then retired. A client takes datagrams from its server's address
//              alone (section 9).
//   addresses  draft-ietf-quic-address-discovery-01 behind parameters.address_discovery (design S77): an end that
//              offers reports to a peer that asks for them sends each path one OBSERVED_ADDRESS, the address the
//              peer's datagrams come from there, in the first 1-RTT packet on it (the handshake's path, a rebinding's,
//              a migration's), again there only when it is lost; off the active path it rides with a path frame
//              alone. Reports taken (only when asked for: PROTOCOL_VIOLATION otherwise) are kept per path, the newest
//              sequence winning (observed_address()).
//   IDs        both ends' IDs are held in QuicCids (quic_connection_ids.hpp), one sequence space per path ID; this
//              side's come from the host (QuicConnectionIds), which routes them and derives their stateless reset
//              tokens from its QuicResetKey: after confirmation the peer is kept at its
//              active_connection_id_limit (at most kQuicMaxLocalConnectionIds) with NEW_CONNECTION_ID frames resent
//              until acknowledged, a retired ID is handed back and replaced, and rotate_connection_ids() retires them
//              all with Retire Prior To. A datagram whose first packet does not open and that ends in the token of a
//              peer's ID this side used is a stateless reset: the connection drains (section 10.3.1).
//   close      an error closes with CONNECTION_CLOSE at the levels the peer may read (section 10.2.3), CRYPTO_ERROR
//              at an alert's level; closing resends it at a falling rate for three PTOs, a peer's close drains for
//              three PTOs, and the idle timeout (section 10.1) closes silently.
//   keyed      open_keyed() starts the raw UDP path (docs/design/network-foundation.md#raw-path) instead of a TLS
//              handshake: 1-RTT keys and each end's sequence 0 ID come from a secret the session already holds, the
//              transport parameters travel in CRYPTO frames of 1-RTT packets (a two-byte length, then the same
//              encoding, grease and version_information as over TLS), and a server stays under the 3x limit until its
//              client acknowledges one of its packets. Everything after is this connection's ordinary 1-RTT life.
//   memory     every buffer comes from the injected allocator at construction; receiving and sending allocate
//              nothing.
//   threads    a connection belongs to its loop's thread.

#include <crd/containers/span.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/congestion.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/ecn.hpp>
#include <crd/net/handover.hpp>
#include <crd/net/quic_connection_ids.hpp>
#include <crd/net/quic_datagram.hpp>
#include <crd/net/quic_ecn.hpp>
#include <crd/net/quic_packet.hpp>
#include <crd/net/quic_path.hpp>
#include <crd/net/quic_path_control.hpp>
#include <crd/net/quic_pmtu.hpp>
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
inline constexpr usize kQuicMaxReasonBytes = 64U; // of a CONNECTION_CLOSE reason phrase kept or sent
inline constexpr u32 kQuicMaxHeldPackets = 64U; // QuicConnectionConfig::held_packets at most
// A token a connection carries: a client's from Retry or NEW_TOKEN, this build's or another server's (RFC 9000 sets
// no bound; the Initial still holds 1200 bytes with the longest connection IDs and its CRYPTO data).
inline constexpr usize kQuicMaxCarriedTokenBytes = 256U;
// This side pads every datagram to kQuicResetPadding bytes more than its connection ID length, so that a peer's
// stateless reset answering it can look like a packet (RFC 9000 section 10.3).
inline constexpr usize kQuicResetPadding = 22U;

inline constexpr usize kQuicMinKeyedIdBytes = 8U; // a keyed connection's IDs at least (64 bits from the session secret)

// What opens a connection keyed by its session's secret (open_keyed()).
struct QuicKeyedStart
{
    // kQuicKeyedSecretBytes for this connection alone: the session's TLS exporter under a label and context of its own,
    // or a launcher's fresh random bytes given to both ends. open_keyed() zeroes them, whatever it answers.
    cont::Span<u8> secret;
    crypto::AlgorithmId aead = crypto::AlgorithmId::Aes128Gcm; // one of the three TLS 1.3 AEADs, the same at both ends
    usize local_id_bytes = 8U;                                 // this side's IDs (8 to 20): what its ingress routes
    usize peer_id_bytes = 8U;                                  // the other side's (8 to 20)
    // When set, an `aead` the policy refuses at `unix_seconds` in `policy_scope` is refused, as a TLS handshake refuses
    // it (S20); null where the secret came from a session whose handshake already ran under the policy.
    const crypto::PolicyStore* policy = nullptr;
    crypto::PolicyScope policy_scope = crypto::PolicyScope::Everywhere;
    u64 unix_seconds = 0U;
};

struct QuicConnectionConfig
{
    crypto::QuicSide side = crypto::QuicSide::Client;
    crypto::QuicVersion version = crypto::QuicVersion::V1; // the version this connection runs
    // Of `side`'s role, with an application protocol. Null: the connection can only be opened keyed (open_keyed()), and
    // nothing of a handshake may be configured (tickets, token, early_context, versions, after_version_negotiation).
    const crypto::TlsContext* tls = nullptr;
    crypto::TlsTicketStore* tickets = nullptr;
    // Where this side's further connection IDs come from (null: sequence 0 alone, and no stateless reset token).
    QuicConnectionIds* ids = nullptr;
    // This side's parameters. max_receive_timestamps_per_ack (at most kQuicMaxReceiveTimestamps) is the NET-STD flag
    // of draft-ietf-quic-receive-ts-03: 0 (the default) neither asks for receive timestamps nor reports them; above 0
    // the connection asks for that many per ACK frame in receive_timestamps_exponent's unit, takes them as its
    // congestion controller's receive times, and reports its own when the peer asks too. address_discovery (Off by
    // default) is the NET-STD flag of draft-ietf-quic-address-discovery-01 ("addresses" above). The connection fills
    // the connection IDs and a server's stateless_reset_token, version_information, grease_quic_bit, the stream and
    // flow-control parameters (from `streams`) and max_datagram_frame_size (from `datagrams`), so those
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
    // What this side sends at most: the ceiling of the path MTU search, and each held packet's slot.
    u32 max_datagram_bytes = kQuicDefaultMaxDatagram;
    // What the path is known to carry (kQuicIpv6BaseDatagram on an IPv6 path, which QuicHost sets), 1200 to
    // max_datagram_bytes; the search starts above it.
    u32 base_datagram_bytes = kQuicPmtuFloor;
    // Probe for larger datagrams (quic_pmtu.hpp); without it datagrams keep base_datagram_bytes. QuicHost clears it
    // on a socket that cannot forbid fragmentation, through which probes would prove nothing.
    bool path_mtu_discovery = true;
    u32 padding_block = 0U; // packets pad to the next multiple of this (NET.3's padded_size()); 0 pads none
    u64 initial_rtt_ns = kQuicInitialRttNs;
    CongestionPolicy congestion; // the controller and pacing (congestion_policy_valid), swappable by set_congestion()
    // The scheduled handovers of the link this connection's path crosses (handover.hpp; handover_schedule_valid): a
    // packet sent where one may take it is a covered packet. Its loss is no congestion signal (no reduction, no loss
    // rate, no persistent congestion, no black hole for the path MTU search) and its RTT and receive time no delay
    // signal, while loss detection, retransmission and the RTT estimate stay as they are; no PMTU probe leaves while
    // one is covered. The schedule describes the link the congestion state was built on: a move that restarts that
    // state (RFC 9000 section 9.4) clears it. Live: set_handover().
    HandoverSchedule handover;
    // A link whose handovers come every period with these guards at a phase this side learns (handover.hpp,
    // handover_learning_valid; none learns nothing): while the learner holds a lock, `handover` is the schedule it
    // built, and none otherwise. An authored schedule is the stronger knowledge: a configuration naming both is
    // refused, and set_handover() ends learning. Losses and RTT samples reach the learner whether a schedule covers
    // them or not; a move that restarts the path's state starts it over. Live: set_handover_learning().
    HandoverLearning handover_learning;
    // Datagrams are marked while ECN validation allows (RFC 9000 section 13.4.2), with the controller's codepoint
    // (congestion_codepoint(): ECT(1) for Prague and a scalable SCReAM, ECT(0) for the rest). A host whose socket
    // cannot mark clears it.
    bool ecn = true;
    u64 key_update_interval = 0U; // 1-RTT packets one key seals before an update starts (0: the AEAD's limit alone)
    // The path the connection opens on (QuicHost sets the peer's address): a port of 0 takes the path of the first
    // packet that opens.
    QuicPath path;
    // A server's preferred address (RFC 9000 section 9.6): parameters.has_preferred_address with its IPv4 or IPv6
    // address and port (or both; the connection ID and token left empty), reached through the host's socket
    // `preferred_socket`. The connection issues the parameter's ID as sequence 1 from `ids` (refusing to start
    // without them) and sends none when it opened on that socket. It answers there at once, but moves its datagrams
    // there only once it validated the client's path there and the client's highest-numbered non-probing packet
    // arrived on it; from then on what reaches its other sockets is dropped (section 9.6.2). A server's datagrams
    // reaching any socket but its active path's and, until it moved, the preferred one are dropped: its address
    // changes in no other way (section 9).
    u32 preferred_socket = 0U;
    // A client validates its server's preferred address of its active path's family from its active socket once the
    // handshake is confirmed, and moves there once validated (section 9.6.1); again from a socket it migrates to
    // first (section 9.6.3), as a move to it migrate() asked for does without this.
    bool follow_preferred_address = true;
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

// A client's request to probe or move to a path of its own (RFC 9000 sections 9.1, 9.2 and 9.5).
enum class QuicMigration : u8
{
    Started,        // the path is known and validating (or proven already); migrate() moved datagrams there
    NotClient,      // a server follows its client and never moves itself (section 9)
    NotConfirmed,   // the handshake is not confirmed (section 9), or the connection is closing
    Disabled,       // the server sent disable_active_migration, and the path's peer is its handshake address (section
                    // 18.2: a preferred address, and every path once the client moved there, stay open)
    OtherServer,    // the path's peer is neither the server's address nor, until the client moved there, its
                    // preferred address (section 9.6), or the active path names no address (port 0)
    ActivePath,     // the path is the active one already
    NoConnectionId, // no peer connection ID unused on any other path is left for a new local address (section 9.5)
    NoConnection,   // the client host has no such connection (QuicClient: a stale handle)
    NoSocket,       // the client host's socket set has no such socket (QuicClient)
};

// Why a connection closed.
struct QuicCloseInfo
{
    bool application = false;     // an application error code (CONNECTION_CLOSE 0x1d)
    bool local = false;           // this side closed
    bool idle = false;            // the idle timeout ended it, without a frame
    bool stateless_reset = false; // the peer's stateless reset ended it (RFC 9000 section 10.3.1)
    bool path_lost = false;       // the active path failed validation with no validated one left: closed silently
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
    u64 packets_opened = 0U;
    u64 packets_dropped = 0U; // packets that did not open, duplicates and packets of another connection
    u64 packets_held = 0U;    // packets held until their keys came (RFC 9001 section 4.1.4)
    u64 ecn_signals = 0U;     // rises of the peer's CE counts taken as congestion (RFC 9002 section 7.1)
    u64 path_probes = 0U;     // PMTU probe datagrams sent
    u64 probe_fills = 0U;     // 1-RTT packets padded to their datagram for the controller's bandwidth probe
    u64 receive_timestamps = 0U; // acknowledged packets the peer's ACK_RECEIVE_TIMESTAMPS frames gave a receive time
    u64 migrations = 0U;         // moves of the datagrams to another path, following the peer or reverting
    u64 path_validations = 0U;   // paths a PATH_RESPONSE validated
    u64 path_failures = 0U;      // validations abandoned on their timeout
    u64 foreign_datagrams = 0U;  // datagrams from a path this side takes nothing from (a client's unknown server)
    u64 handover_losses = 0U;    // losses of packets a scheduled handover may have taken, kept from congestion control
    u64 handover_locks = 0U;     // learned schedules put in force where none was
    u64 address_observations = 0U; // OBSERVED_ADDRESS reports taken (each newer than its path's last)
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
    // Starts the raw UDP path on `start` (a connection built without a TLS context): Established and confirmed at once,
    // this side's parameters waiting for send(), the peer's arriving as its first packet. False, changing nothing but
    // the zeroed secret, when not Idle, for a connection built with a TLS context, or for a secret, AEAD or ID length
    // out of range or an AEAD the policy refuses.
    [[nodiscard]] bool open_keyed(const QuicKeyedStart& start, u64 now_ns) noexcept;

    // One datagram from the peer, arrived on `path` at `now_ns` with `ecn`; its bytes are decrypted in place. Without
    // a path it arrived on the active one.
    void receive(const QuicPath& path, cont::Span<u8> datagram, Ecn ecn, u64 now_ns) noexcept;
    void receive(cont::Span<u8> datagram, Ecn ecn, u64 now_ns) noexcept;
    // Writes the next datagram into `out` (at least kQuicMinInitialDatagram bytes) and returns its size; 0 when
    // nothing is to be sent now. A path whose socket is in `held_sockets` (bit i for socket i: the host's queue holds
    // it back) gets nothing and changes nothing; held_back() then says something waits for it.
    [[nodiscard]] usize send(cont::Span<u8> out, u64 now_ns, u32 held_sockets = 0U) noexcept;
    // Whether the last send() left a datagram unwritten for a path whose socket was held back.
    [[nodiscard]] bool held_back() const noexcept { return m_held_back; }
    // The codepoint the datagram the last send() wrote is sent with, and the path it goes on.
    [[nodiscard]] Ecn marking() const noexcept { return m_marking; }
    [[nodiscard]] const QuicPath& sent_path() const noexcept { return m_paths.path(m_send_path); }
    // The path datagrams go on, and every path the connection knows.
    [[nodiscard]] const QuicPath& path() const noexcept { return m_paths.path(m_paths.active()); }
    [[nodiscard]] const QuicPaths& paths() const noexcept { return m_paths; }
    // Whether datagrams moved to the server's preferred address (a client) or socket (a server): section 9.6.
    [[nodiscard]] bool on_preferred_address() const noexcept { return m_on_preferred; }
    // The address the peer last reported seeing this side's datagrams come from on the active path (OBSERVED_ADDRESS,
    // draft-ietf-quic-address-discovery-01), port 0 before any report; paths().observed() holds every path's. Reports
    // arrive only when parameters.address_discovery asks for them, and a peer may lie (section 6.1): a caller trusts
    // one only as far as other peers' reports agree.
    [[nodiscard]] const Endpoint& observed_address() const noexcept { return m_paths.observed(m_paths.active()); }
    // When on_timeout() is next due: kNeverNs for no timer.
    [[nodiscard]] u64 next_timer_ns() const noexcept;
    void on_timeout(u64 now_ns) noexcept;

    // Closes with an application error (sent as APPLICATION_ERROR without a reason in Initial and Handshake
    // packets, section 10.2.3) and up to kQuicMaxReasonBytes of `reason`. Nothing happens once closing.
    void close(u64 application_error, cont::ConstSpan<u8> reason, u64 now_ns) noexcept;
    // Owes one ack-eliciting 1-RTT packet: a keep-alive before the idle timeout (section 10.1.2).
    void ping() noexcept { m_ping_owed = true; }
    // A client's own paths: `path` is a socket of its own to the server's address. probe_path() validates it,
    // datagrams staying where they are (section 9.1); migrate() moves them there now, validating it unless proven
    // (section 9.2), and congestion control, the RTT estimate, the pacer, ECN validation and the path MTU search start
    // again for it (section 9.4). Each path sends to a peer connection ID no other path uses (section 9.5) and carries
    // no amplification limit (the server's address is proven); one that fails validation is forgotten, and a failed
    // active path reverts to the last validated one (section 9.3.2). A path to the server's preferred address is
    // validated before datagrams move there (section 9.6.2): migrate() moves at once only when it is proven, and
    // otherwise once validated, from whatever socket the client is on by then; on that move every path to the
    // server's other address but the one moved from is forgotten, and that one once the server follows. Anything but
    // Started changes nothing.
    [[nodiscard]] QuicMigration probe_path(const QuicPath& path, u64 now_ns) noexcept;
    [[nodiscard]] QuicMigration migrate(const QuicPath& path, u64 now_ns) noexcept;
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
    // The host's report (an ICMP Packet Too Big message, RFC 8899 section 4.6) that a datagram sent to this
    // connection's peer address met a hop carrying `limit` bytes of UDP payload, `quote` the start of that datagram.
    // True when `quote` is one of this connection's unacknowledged 1-RTT packets (section 4.6.1): the peer's
    // connection ID in use, then the four protected bytes the packet was sent with, which no one off the path can
    // know. The search then takes the hint or finds it inconsistent (QuicPathMtu::on_too_big); any other report
    // changes nothing.
    // `from` names where the report arrived: only one on the active path, quoting a packet sent there since the path
    // state last started (QuicRecovery::on_path), moves the search, so a report about a path left behind never
    // shrinks the new one. The form without a path is the active one.
    bool path_too_big(const QuicPath& from, cont::ConstSpan<u8> quote, u32 limit, u64 now_ns) noexcept;
    bool path_too_big(cont::ConstSpan<u8> quote, u32 limit, u64 now_ns) noexcept;

    [[nodiscard]] QuicConnectionState state() const noexcept { return m_state; }
    [[nodiscard]] bool confirmed() const noexcept { return m_confirmed; }
    // A client accepted its server's Retry; a server's connection follows its own Retry.
    [[nodiscard]] bool retried() const noexcept { return m_retried; }
    // The version the connection runs: the configured one until a compatible switch (final once a server has read
    // its client's parameters, and once a client has read its server's first packet in another version or CRYPTO
    // data in this one).
    [[nodiscard]] crypto::QuicVersion version() const noexcept { return m_protection.version(); }
    // The active path's address is validated.
    [[nodiscard]] bool address_validated() const noexcept { return m_paths.validated(m_paths.active()); }
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
    // The handshake's path's sending state (quic_path_control.hpp): its controller, sampler, ECN validation and path
    // MTU search, whose size() is what its datagrams take now.
    [[nodiscard]] const QuicPathControl& path_control() const noexcept { return *m_controls[0]; }
    [[nodiscard]] const CongestionController& congestion() const noexcept { return path_control().congestion(); }
    [[nodiscard]] const DeliveryRateSampler& delivery() const noexcept { return path_control().delivery(); }
    [[nodiscard]] const QuicEcnValidator& ecn() const noexcept { return path_control().ecn(); }
    [[nodiscard]] const QuicPathMtu& path_mtu() const noexcept { return path_control().path_mtu(); }
    // Replaces the congestion policy live on every path: each new controller starts from its path's window, slow start
    // threshold and recovery period. False, changing nothing, for a policy congestion_policy_valid() refuses.
    [[nodiscard]] bool set_congestion(const CongestionPolicy& policy) noexcept;
    // Replaces the handshake's path's handover schedule live (QuicConnectionConfig::handover), for packets sent from
    // now; none clears it. False, changing nothing, for a schedule handover_schedule_valid() refuses.
    [[nodiscard]] bool set_handover(const HandoverSchedule& schedule) noexcept;
    [[nodiscard]] const HandoverSchedule& handover() const noexcept { return path_control().handover(); }
    // Learns the handover phase of `learning`'s link from now (QuicConnectionConfig::handover_learning), clearing the
    // schedule in force; none ends learning and clears it. False, changing nothing, for one handover_learning_valid()
    // refuses.
    [[nodiscard]] bool set_handover_learning(const HandoverLearning& learning) noexcept;
    [[nodiscard]] const HandoverLearning& handover_learning() const noexcept
    {
        return path_control().handover_learning();
    }
    // Whether handover() is a schedule learned rather than authored (none is neither).
    [[nodiscard]] bool handover_learned() const noexcept
    {
        return handover_learning().period_ns != 0U && handover().period_ns != 0U;
    }
    // The peer's parameters, valid once peer_parameters_known().
    [[nodiscard]] bool peer_parameters_known() const noexcept { return m_peer_known; }
    [[nodiscard]] const QuicTransportParameters& peer_parameters() const noexcept { return m_peer; }
    // The Destination Connection ID this side sends to, and the peer's IDs held (sequence 0 first among them).
    [[nodiscard]] cont::ConstSpan<u8> peer_cid() const noexcept;
    [[nodiscard]] usize peer_cid_count() const noexcept { return m_cids.peer_count(); }
    // This side's IDs still routed (sequence 0 included until retired).
    [[nodiscard]] usize local_cid_count() const noexcept { return m_cids.local_count(); }
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
    // has one, unless that one is for another path than the validated path() (a client that moved: section 9.3).
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

    // What a packet write_packet writes carries.
    enum class Carry : u8
    {
        Frames, // what the space owes
        Close,  // CONNECTION_CLOSE
        Probe,  // a PMTU probe: PING, padded to the whole room
        Path,   // the path frames owed on m_send_path, not the active path: a probing packet (section 9.1)
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
        void on_spurious(QuicSpace space, const QuicSentPacket& packet, u64 lost_ns) noexcept override;

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
        bool path_challenge = false; // its data is m_challenge
        bool fill = false; // the packet fills its datagram for a bandwidth probe
    };

    // A packet held until its keys come; its bytes are a max_datagram_bytes slot of m_held_bytes.
    struct Held
    {
        u32 bytes = 0U;
        QuicPacketType type = QuicPacketType::OneRtt;
        Ecn ecn = Ecn::NotEct;
    };

    [[nodiscard]] bool start(u64 now_ns) noexcept;
    [[nodiscard]] bool start_keyed(const QuicKeyedStart& start, u64 now_ns) noexcept;
    // This side's parameters from the configuration and its IDs, and the packet protection's settings.
    void fill_local_parameters() noexcept;
    // A keyed connection's CRYPTO data: the peer's hello once whole; anything more is a protocol violation.
    [[nodiscard]] bool read_keyed_hello(Level& level, u64 now_ns) noexcept;
    // This side's parameters encoded into `out` with fresh grease: their size, 0 when they do not fit.
    [[nodiscard]] usize encode_parameters(cont::Span<u8> out) noexcept;
    // Whether a long header's version is one this connection reads now (RFC 9369 section 4.1).
    [[nodiscard]] bool version_routed(const QuicHeader& header) const noexcept;
    [[nodiscard]] u64 entropy() noexcept;

    // Receiving.
    // Whether the packet opened.
    [[nodiscard]] bool receive_packet(const QuicHeader& header, cont::Span<u8> packet, usize datagram_bytes, Ecn ecn,
                                      u64 now_ns) noexcept;
    // Paths (S75).
    // A server follows its client to other paths once confirmed.
    [[nodiscard]] bool follows_peer() const noexcept;
    // A packet from m_candidate, a path not known, opened: the path joins, with the peer ID it is answered on.
    void admit_path(usize datagram_bytes, u64 now_ns) noexcept;
    // A path joins (the least recently used one given up when the table is full).
    [[nodiscard]] usize take_path(const QuicPath& path, u64 now_ns) noexcept;
    // Each path but the active one that has no peer ID to send to takes an unused one; with none left, every path
    // left behind on the least recently used other ID is forgotten and that ID retired, for the peer's replacement.
    void address_paths(u64 now_ns) noexcept;
    // A peer ID's sequence no path but `index` sends to and none has sent to; kNoSequence for none.
    [[nodiscard]] u64 unused_peer_sequence(usize index) const noexcept;
    // probe_path()'s checks, then open_path(); `index` names the path on Started.
    [[nodiscard]] QuicMigration open_own_path(const QuicPath& path, u64 now_ns, usize& index) noexcept;
    // A client's own path: its slot and a peer ID unused elsewhere, validating unless proven (NoConnectionId or
    // Started); with `wait_for_id` one without an unused ID opens to wait for the peer's next.
    [[nodiscard]] QuicMigration open_path(const QuicPath& path, u64 now_ns, usize& index,
                                          bool wait_for_id = false) noexcept;
    // The server's preferred address of `peer`'s family, in `peer`'s form (IPv4-mapped too); port 0 for none.
    [[nodiscard]] Endpoint preferred_for(const Endpoint& peer) const noexcept;
    // Whether `peer` is the preferred address, which the client has not moved to and which is not its server's
    // address already, from an active path that names one.
    [[nodiscard]] bool toward_preferred(const Endpoint& peer) const noexcept;
    // A client validates the preferred address from its active socket, to move there once validated (section 9.6.1).
    void follow_preferred(u64 now_ns) noexcept;
    // Datagrams go on `index`, at the preferred address: a server forgets its other sockets' paths, a client every
    // path to the server's other address but the one it moved from (sections 9.6.2 and 9.6.3).
    void take_preferred(usize index, u64 now_ns) noexcept;
    // Every ID of this side's the peer has not acknowledged goes again: from a new path, the one announced on the
    // path left behind may never arrive.
    void reannounce_local_ids() noexcept;
    // An opened Application packet of m_receive_path: the migration it may be (section 9.3).
    void follow_packet(u64 number, bool probing, u64 now_ns) noexcept;
    // Datagrams go on `index` from now on: the peer moved there, or a failed validation reverts there.
    void move_to(usize index, u64 now_ns) noexcept;
    // A client proven on its active path, and followed there by its server, forgets the fallback it moved from.
    void leave_fallback(u64 now_ns) noexcept;
    // Once the active path is validated, the path MTU search, ECN validation, congestion control, the RTT estimate
    // and the pacer start again unless only the port changed since the path they were built on (section 9.4).
    void follow_congestion(u64 now_ns) noexcept;
    void on_validated(usize index, u64 now_ns) noexcept;
    // Paths whose validation timed out: reverted from, forgotten, or the connection closes (sections 8.2.4, 9.3.2).
    void expire_paths(u64 now_ns) noexcept;
    // Forgets a path that is not the active one, retiring its peer ID unless another path sends to it.
    void drop_path(usize index, u64 now_ns) noexcept;
    // The peer ID a path sends to (its sequence), whether the peer's ID of a sequence is held, and whether a path has
    // the room and an ID to send a datagram now.
    [[nodiscard]] u64 path_sequence(usize index) const noexcept;
    [[nodiscard]] bool peer_held(u64 sequence) const noexcept { return m_cids.peer(0U, sequence) != nullptr; }
    [[nodiscard]] bool path_sendable(usize index) const noexcept;
    // Whether path `index`'s socket is one of `held_sockets`.
    [[nodiscard]] bool path_held(usize index, u32 held_sockets) const noexcept;
    [[nodiscard]] u64 validation_timeout_ns() const noexcept;
    [[nodiscard]] u64 path_timer_ns() const noexcept;
    [[nodiscard]] bool on_frame(const QuicFrame& frame, QuicSpace space, u64 now_ns) noexcept;
    [[nodiscard]] bool crypto_frame(const QuicFrame& frame, QuicSpace space, u64 now_ns) noexcept;
    [[nodiscard]] bool advance_handshake(u64 now_ns) noexcept;
    [[nodiscard]] bool read_peer_parameters(cont::ConstSpan<u8> encoded, u64 now_ns) noexcept;
    // The peer's grants to this side's streams and datagrams: a client's remembered ones for 0-RTT, then its server's.
    void apply_peer_limits(const QuicTransportParameters& peer) noexcept;
    // A client: starts 0-RTT on the parameters its ticket remembers. Both: acts on the server's 0-RTT decision.
    [[nodiscard]] bool begin_early() noexcept;
    [[nodiscard]] bool settle_early(u64 now_ns) noexcept;
    void reject_early(u64 now_ns) noexcept;
    void hold(QuicPacketType type, cont::ConstSpan<u8> packet, Ecn ecn) noexcept;
    // Reads the held packets whose keys came, until none more opens; drops those whose keys went.
    void read_held(u64 now_ns) noexcept;
    [[nodiscard]] bool new_connection_id(const QuicFrame& frame, u64 now_ns) noexcept;
    [[nodiscard]] bool retire_local(u64 sequence, u64 now_ns) noexcept;
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
    // Whether `space` holds data to send that only congestion control holds back.
    [[nodiscard]] bool has_data(QuicSpace space, u64 now_ns) const noexcept;
    // Whether `space` may carry data now: its storage has room and, in the Application space, congestion control lets
    // data go (m_data_open).
    [[nodiscard]] bool data_allowed(QuicSpace space) const noexcept;
    // Opens or holds 0-RTT and 1-RTT data for the datagram of up to `datagram` bytes send() writes at `now_ns`
    // (m_data_open): it goes while probing, or when the window has room for all of it and the pacer's release time
    // has come. Such data held back while some waits tells the controller, and a window with room arms the pacer. A
    // bandwidth probe wanting bytes counts as data waiting (m_probe_fill).
    void gate_data(usize datagram, u64 now_ns) noexcept;
    // Whether a datagram can go now: one waits, and its outcome has a ledger record free when it is reported.
    [[nodiscard]] bool datagram_ready(u64 now_ns) const noexcept;
    // `pad`: the datagram reaches 1200 bytes (section 14.1), by this packet when it is the `last` or when what it
    // leaves is too small for another.
    [[nodiscard]] usize write_packet(QuicSpace space, cont::Span<u8> room, usize datagram_used, bool pad, bool last,
                                     Carry carry, u64 now_ns) noexcept;
    // The path frames owed on a path other than the active one and not on a held socket, a probing datagram written
    // into `out`: its size, 0 for none.
    [[nodiscard]] usize send_path_frames(cont::Span<u8> out, u64 now_ns, u32 held_sockets) noexcept;
    // The PMTU probe due now, written alone into `out`: its size, 0 when none goes (none due, a probe timeout or an
    // owed ACK pending, or the window, the pacer or the amplification limit holding it; a pacer arms its timer).
    [[nodiscard]] usize send_path_probe(cont::Span<u8> out, u64 now_ns) noexcept;
    // The sending state of path slot `slot` (open in the recovery), and the slot of the datagram being written: the
    // handshake's until multipath opens others (S78's scheduling).
    [[nodiscard]] QuicPathControl& control(usize slot) noexcept { return *m_controls[slot]; }
    [[nodiscard]] static constexpr usize send_slot() noexcept { return 0U; }
    // The datagram frames' room follows the smallest datagram of the open paths, any of which may carry one.
    void follow_datagram_size() noexcept;
    void count(const QuicPathSignals& signals) noexcept;
    [[nodiscard]] usize write_frames(QuicSpace space, bool early, cont::Span<u8> frames, u64 packet, u64 now_ns,
                                     Written& written) noexcept;
    [[nodiscard]] usize write_close(QuicSpace space, cont::Span<u8> frames) const noexcept;
    // m_send_path's PATH_RESPONSE when owed, then a PATH_CHALLENGE when due (one a packet, section 8.2.1). Neither is
    // sent again when lost: a response answers one challenge, and a challenge's timer sends another. Then the path's
    // OBSERVED_ADDRESS when due, recorded so that a lost one is owed there again (`packet` names the record).
    [[nodiscard]] usize write_path_frames(cont::Span<u8> frames, u64 packet, u64 now_ns, Written& written) noexcept;
    // Whether path `index` is to carry an OBSERVED_ADDRESS now: this side offered reports, its peer asked for them,
    // the path owes one and is bound, and the ledger has room for its record.
    [[nodiscard]] bool observation_due(usize index) const noexcept;
    // A peer's OBSERVED_ADDRESS on the path of the packet being read.
    [[nodiscard]] bool observed_address_frame(const QuicFrame& frame, u64 now_ns) noexcept;
    [[nodiscard]] usize send_closing(cont::Span<u8> out, u64 now_ns) noexcept;
    // Appends a record of a frame `written` holds to the ledger, naming it the packet's first when it is.
    [[nodiscard]] bool record(QuicSpace space, u64 packet, u8 kind, const QuicStreamRecord& frame,
                              Written& written) noexcept;
    void settle(QuicSpace space, const QuicSentPacket& packet, Settle how) noexcept;
    void pull_crypto() noexcept;
    [[nodiscard]] usize amplification_credit() const noexcept;
    // Whether send() can write `space`'s owed ACK (the active path's amplification limit included): a timer nothing
    // serves is never reported due.
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
    // Each open path slot's sending state: slot 0's is m_first_control, the others' are allocated with their slot.
    QuicPathControl m_first_control;
    QuicPathControl* m_controls[kQuicMaxPaths] = {&m_first_control};
    u32 m_datagram_bytes = kQuicPmtuFloor; // the smallest open path's size, which the datagrams last took
    QuicPeerReceiveTimes m_peer_times;     // the receive times of the ACK frame being processed
    Ecn m_marking = Ecn::NotEct;      // the codepoint of the datagram being written, then of the last one
    bool m_data_open = true;          // congestion control lets data go in the datagram being written
    bool m_probe_fill = false;        // the controller's bandwidth probe wants bytes: 1-RTT packets fill the datagram
    u64 m_event_ns = 0U;              // the time recovery's events happen at (they carry none)
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
    bool m_keyed = false;           // opened by open_keyed()
    bool m_version_settled = false; // a client's: its server's version is known
    u32 m_packet_version = 0U;      // the long header version of the packet being read (0 for a short header)
    QuicCids m_cids;                // both ends' connection IDs, path ID 0's until multipath opens others
    QuicCidRef m_packet_local;      // this side's ID the packet being read was sent to (none: not found())
    u8 m_new_token[kQuicMaxCarriedTokenBytes] = {};
    usize m_new_token_bytes = 0U;
    QuicPath m_new_token_path; // the path whose address the token is for
    u64 m_new_token_generation = 0U;
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
    QuicPaths m_paths;
    QuicPath m_candidate;      // the path of a datagram being read from no known path
    usize m_receive_path = 0U; // the path of the datagram being read (kQuicNoPath: m_candidate, not yet admitted)
    usize m_send_path = 0U;    // the path of the datagram being written, then of the last one
    u64 m_largest_non_probing = kQuicNoPacket; // the Application packet that last chose the path (section 9.3)
    bool m_peer_followed = true; // a client's server sends on the client's active path: the latest move is followed
    // The path datagrams move to once validated (section 9.6.2): a client's to the preferred address, a server's
    // where its client's highest-numbered non-probing packet reached the preferred socket. kQuicNoPath for none.
    usize m_deferred_move = kQuicNoPath;
    bool m_on_preferred = false;
    u8 m_challenge[kQuicPathDataBytes] = {};   // the PATH_CHALLENGE data of the packet being written
    u64 m_observation_next = 0U;               // the next OBSERVED_ADDRESS sequence (rising on every path)
    bool m_ping_owed = false;
    bool m_confirmed = false;
    bool m_held_back = false; // the last send() skipped a path whose socket was held back
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
