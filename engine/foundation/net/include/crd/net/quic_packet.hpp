#pragma once

// crd-net -- QUIC packets per encryption level (4.2a; contract: docs/design/network-foundation.md#quic-packets).
// Sources: RFC 9000 sections 12.2, 12.3, 14.1, 17.2 to 17.4 and appendix A, RFC 9001 sections 4.9, 5 and 6, RFC
// 9369 section 3 and RFC 9287 section 3.
//
//   writing   QuicPacketWriter lays one packet into the room a datagram has left: the header, the frames the caller
//             writes into frames(), then seal(): PADDING up to the largest of the sample's floor (RFC 9001 section
//             5.4.2), the caller's floor (1200 bytes for a datagram carrying an Initial, section 14.1) and NET.3's
//             padded_size() under the channel's block; the Length in its shortest encoding; the AEAD; header
//             protection. Nothing is allocated, and a packet that does not fit writes nothing.
//   reading   QuicDatagramPackets walks a datagram's coalesced packets (section 12.2: a packet whose Destination
//             Connection ID differs from the first's ends the walk), and QuicPacketProtection::open removes header
//             protection, decodes the packet number in its space, opens the AEAD and only then trusts what the header
//             said: the reserved bits (PROTOCOL_VIOLATION), the space's window (duplicates and stale packets dropped)
//             and the key phase.
//   keys      QuicPacketProtection holds a connection's keys per level and applies RFC 9001's rules in code: Initial
//             keys discarded by a client when it first sends a Handshake packet and by a server when it first opens
//             one (section 4.9.1), Handshake keys at confirmation (4.9.2), a client's 0-RTT keys when its 1-RTT keys
//             arrive (4.9.3); 1-RTT key phases with the next receive key derived ahead (section 6.3), updates
//             initiated only when allowed (6.1), followed (6.2) and checked (6.4: older keys never protect a higher
//             packet number), the failed opens across all keys against the integrity limit (6.6).
//   threads   a QuicPacketProtection belongs to its connection's thread.

#include <crd/containers/span.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_header.hpp>

namespace crd::crypto
{
class QuicHandshake;
} // namespace crd::crypto

namespace crd::net
{
// One direction's keys at one encryption level: packet protection and header protection.
struct QuicKeys
{
    crypto::QuicPacketKey packet;
    crypto::QuicHeaderKey header;

    [[nodiscard]] bool ready() const noexcept { return packet.ready() && header.ready(); }
    void clear() noexcept
    {
        packet.clear();
        header.clear();
    }
    // Both keys of `aead` (as crypto::QuicPacketKey::init) from `secret`, which stays the caller's to zero.
    [[nodiscard]] crypto::CryptoError init(crypto::QuicVersion version, crypto::AlgorithmId aead,
                                           cont::ConstSpan<u8> secret) noexcept;
};

// `side`'s Initial keys (AES-128-GCM under quic_initial_secret) for a connection whose client chose `client_dcid`.
[[nodiscard]] crypto::CryptoError quic_initial_keys(crypto::QuicVersion version, crypto::QuicSide side,
                                                    cont::ConstSpan<u8> client_dcid, QuicKeys& out) noexcept;

// What a packet's sender chooses for its header. `type` is Initial, ZeroRtt, Handshake or OneRtt; `version` is the
// connection's (a long header's field and its type bits). The packet number is encoded in `number_bytes` (1 to 4),
// or when 0 in quic_packet_number_bytes' length for the largest acknowledged; a length too short for that is
// refused. `clear_quic_bit` sends the QUIC bit as 0 (RFC 9287): only toward a peer that sent grease_quic_bit, chosen
// per packet from entropy (quic_grease_bit_clear).
struct QuicPacketSpec
{
    QuicPacketType type = QuicPacketType::OneRtt;
    u32 version = static_cast<u32>(crypto::QuicVersion::V1);
    cont::ConstSpan<u8> dcid;
    cont::ConstSpan<u8> scid;  // long headers
    cont::ConstSpan<u8> token; // Initial packets
    u64 number = 0U;
    usize number_bytes = 0U;
    u64 largest_acked = 0U;
    bool acknowledged = false; // whether the peer acknowledged any packet in the space
    bool spin = false;         // short headers: the latency spin bit (section 17.4)
    bool clear_quic_bit = false;
};

// Whether a packet toward a peer that sent grease_quic_bit clears the QUIC bit: one entropy bit per packet, so the
// value is unpredictable as RFC 9287 section 3.1 asks.
[[nodiscard]] constexpr bool quic_grease_bit_clear(bool peer_greases, u64 entropy) noexcept
{
    return peer_greases && (entropy & 1U) != 0U;
}

// The packet number space of a packet type (RFC 9000 section 12.3): 0-RTT and 1-RTT share the application space.
enum class QuicSpace : u8
{
    Initial,
    Handshake,
    Application,
};
inline constexpr usize kQuicSpaces = 3U;

[[nodiscard]] constexpr QuicSpace quic_space_of(QuicPacketType type) noexcept
{
    if (type == QuicPacketType::Initial)
    {
        return QuicSpace::Initial;
    }
    return (type == QuicPacketType::Handshake) ? QuicSpace::Handshake : QuicSpace::Application;
}

// How a packet is padded when it is sealed. The packet reaches at least `min_bytes` (the datagram's remaining share
// of 1200 bytes when it carries an Initial: quic_datagram_floor), and when `block` is not zero padded_size() of its
// size under it, never past the room the writer was given.
struct QuicPadding
{
    usize min_bytes = 0U;
    u32 block = 0U;
};

// The bytes the next packet of a datagram that already holds `used` bytes must reach so that the datagram meets
// section 14.1's 1200 bytes: a client pads every datagram carrying an Initial, a server every one carrying an
// ack-eliciting Initial. 0 when nothing is owed.
[[nodiscard]] constexpr usize quic_datagram_floor(bool owed, usize used) noexcept
{
    return (owed && used < kQuicMinInitialDatagram) ? kQuicMinInitialDatagram - used : 0U;
}

// Lays out and seals one packet at the start of the room it is given.
class QuicPacketWriter
{
public:
    // Writes `spec`'s header into the start of `room`: false, writing nothing, for a type other than the four, a
    // version crypto does not protect, connection IDs over 20 bytes, a token on a packet that is not an Initial, a
    // packet number past 2^62 - 1 or a length that cannot encode it, or a room too small for the header, the
    // sample's floor and the tag.
    [[nodiscard]] bool begin(const QuicPacketSpec& spec, cont::Span<u8> room) noexcept;

    // Where the caller writes the packet's frames (write_quic_frame, write_quic_ack); valid after begin().
    [[nodiscard]] cont::Span<u8> frames() const noexcept;

    // Seals the packet whose first `used` bytes of frames() hold its frames: pads it with PADDING frames as
    // `padding` asks and to the sample's floor, writes the Length (long headers) and the key phase bit (short
    // headers), seals it under `packet`, protects its header under `header` and returns its size in `written`.
    // BadLength when `used` passes frames() or the padding does not fit; the AEAD's errors (Exhausted at its
    // limit) with nothing protected. A writer seals once per begin().
    [[nodiscard]] crypto::CryptoError seal(crypto::QuicPacketKey& packet, const crypto::QuicHeaderKey& header,
                                           bool key_phase, usize used, QuicPadding padding, usize& written) noexcept;

    [[nodiscard]] QuicPacketType type() const noexcept { return m_type; }
    [[nodiscard]] u64 number() const noexcept { return m_number; }

private:
    cont::Span<u8> m_room;
    usize m_before_length = 0U; // a long header's bytes before its Length, a short header's before its number
    usize m_length_bytes = 0U;  // the Length's reserved bytes (0 in a short header)
    usize m_number_bytes = 0U;
    u64 m_number = 0U;
    QuicPacketType m_type = QuicPacketType::OneRtt;
    bool m_open = false;
};

// The packets of one received datagram, in order (RFC 9000 section 12.2).
class QuicDatagramPackets
{
public:
    // `short_dcid_bytes` as parse_quic_header's.
    QuicDatagramPackets(cont::Span<u8> datagram, usize short_dcid_bytes) noexcept;

    // The next packet: its header and its extent within the datagram. False after the last, at a packet that does not
    // parse (error() tells why) and at one whose Destination Connection ID differs from the first packet's, which
    // with everything after it is dropped.
    [[nodiscard]] bool next(QuicHeader& header, cont::Span<u8>& packet) noexcept;

    [[nodiscard]] QuicHeaderError error() const noexcept { return m_error; }
    // Whether the walk stopped at a packet of another connection.
    [[nodiscard]] bool foreign() const noexcept { return m_foreign; }

private:
    cont::Span<u8> m_datagram;
    cont::ConstSpan<u8> m_first_dcid;
    usize m_short_dcid_bytes = 0U;
    usize m_at = 0U;
    QuicHeaderError m_error = QuicHeaderError::None;
    bool m_foreign = false;
};

// Why a packet was not opened or sealed. Drop-only errors close nothing; the others name the transport error the
// connection closes with (quic_packet_close).
enum class QuicPacketError : u8
{
    None,
    NoKeys,         // no keys at the packet's level (never installed, or discarded): dropped
    QuicBit,        // a cleared QUIC bit from a peer that was not told grease_quic_bit: dropped
    Unprotectable,  // header protection could not be removed: dropped
    Forged,         // the AEAD did not open: dropped, counted against the integrity limit
    Duplicate,      // opened before: dropped
    Stale,          // older than the space's window: dropped
    ReservedBits,   // the reserved bits were not zero: PROTOCOL_VIOLATION
    Empty,          // a packet without frames: PROTOCOL_VIOLATION (section 12.4)
    KeyUpdate,      // a key phase RFC 9001 section 6 forbids: KEY_UPDATE_ERROR
    IntegrityLimit, // failed opens reached the integrity limit: AEAD_LIMIT_REACHED
    Exhausted,      // a key reached its confidentiality limit and could not be updated: AEAD_LIMIT_REACHED
    Refused,        // seal: the writer refused the layout or the padding
};

// The transport error a packet error closes the connection with; NoError for the errors that only drop the packet.
[[nodiscard]] QuicTransportError quic_packet_close(QuicPacketError error) noexcept;

// What open() read out of a packet.
struct QuicOpened
{
    QuicPacketType type = QuicPacketType::OneRtt;
    u64 number = 0U;
    cont::Span<u8> payload;  // the decrypted frames, in place in the datagram
    bool spin = false;       // short headers
    bool key_update = false; // this packet moved the receive key phase: the previous key is now held
};

// A connection's packet protection: its keys per level and the receive windows of its packet number spaces. (MSVC
// C4324 as for crypto::AeadKey: the keys it holds inline are 16-byte aligned; ARM64 MSVC reports the padding.)
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
class QuicPacketProtection
{
public:
    QuicPacketProtection() noexcept = default;
    ~QuicPacketProtection() noexcept;
    QuicPacketProtection(const QuicPacketProtection&) = delete;
    QuicPacketProtection& operator=(const QuicPacketProtection&) = delete;
    QuicPacketProtection(QuicPacketProtection&&) = delete;
    QuicPacketProtection& operator=(QuicPacketProtection&&) = delete;

    // Starts a connection of `side` in `version` (its original version): its Initial keys for `client_dcid` (the
    // client's first Destination Connection ID), every other key and window cleared. rekey_initial() after a Retry
    // re-derives the Initial keys, the other version's receive key too, for the Retry's ID and keeps the rest.
    [[nodiscard]] crypto::CryptoError start(crypto::QuicSide side, crypto::QuicVersion version,
                                            cont::ConstSpan<u8> client_dcid) noexcept;
    [[nodiscard]] crypto::CryptoError rekey_initial(cont::ConstSpan<u8> client_dcid) noexcept;
    // Starts a keyed connection (the raw UDP path) of `side` in `version`: every key and window cleared and only 1-RTT
    // keys ever to come, through install() at the Application level.
    void start_keyed(crypto::QuicSide side, crypto::QuicVersion version) noexcept;

    // Compatible version negotiation (RFC 9368 section 2.3, RFC 9369 section 4.1). A client that offers `version`
    // also opens its server's Initial packets in it (expect_version), so that it learns the version its server
    // switched to from the first such packet. change_version() runs the connection in `version` from now on: its
    // Initial keys of both directions for `client_dcid`, and the Handshake and 1-RTT keys installed later; the Initial
    // receive key of the version it ran in stays to open the peer's Initial packets still in it (a server's client
    // that has not learned the switch, a client's server acknowledging before it switched) until the Initial keys go.
    // 0-RTT keys stay in the original version. NotReady once the Initial keys are gone or a Handshake key exists,
    // Overlap for the version it runs or, from expect_version(), a second one.
    [[nodiscard]] crypto::CryptoError expect_version(crypto::QuicVersion version,
                                                     cont::ConstSpan<u8> client_dcid) noexcept;
    [[nodiscard]] crypto::CryptoError change_version(crypto::QuicVersion version,
                                                     cont::ConstSpan<u8> client_dcid) noexcept;
    // The version the connection runs, the one it started in, and whether Initial packets of `version` open.
    [[nodiscard]] crypto::QuicVersion version() const noexcept { return m_version; }
    [[nodiscard]] crypto::QuicVersion original_version() const noexcept { return m_original; }
    [[nodiscard]] bool opens_initial(u32 version) const noexcept;

    // Installs every key `handshake` has ready at the Handshake, Application and Early levels, in this connection's
    // version; called after each provide(). A client's 1-RTT keys discard its 0-RTT keys.
    [[nodiscard]] crypto::CryptoError install(crypto::QuicHandshake& handshake) noexcept;
    // Installs one key from a traffic secret (the caller zeroes it): `sending` this side's or else the peer's, at
    // the Handshake, Application or Early level, with the suite's `aead`. BadLength, Unsupported as QuicKeys::init;
    // Exists when the key is installed; Overlap for a level and direction this side never has (a server's Early
    // send key, a client's Early receive key).
    [[nodiscard]] crypto::CryptoError install(crypto::QuicLevel level, bool sending, crypto::AlgorithmId aead,
                                              cont::ConstSpan<u8> secret) noexcept;

    // Whether this side can send or open packets of `type` now.
    [[nodiscard]] bool can_send(QuicPacketType type) const noexcept;
    [[nodiscard]] bool can_open(QuicPacketType type) const noexcept;
    // Whether the keys that open the peer's packets of `type` may still be installed: none yet, and none discarded at
    // that level (Handshake, a server's 0-RTT and 1-RTT; never Initial). A packet of such a type may be held until
    // they come (RFC 9001 section 4.1.4).
    [[nodiscard]] bool keys_pending(QuicPacketType type) const noexcept;

    // Discards the keys of `level` (both directions): the Early level's when a server's 0-RTT timer ends (RFC 9001
    // section 4.9.3) or the server refused 0-RTT; Initial and Handshake keys go by themselves.
    void discard(crypto::QuicLevel level) noexcept;
    // The handshake is confirmed (RFC 9001 section 4.1.2): the Handshake keys are discarded and key updates allowed.
    void confirm() noexcept;
    [[nodiscard]] bool confirmed() const noexcept { return m_confirmed; }

    // Whether a cleared QUIC bit is accepted: true once this endpoint sent grease_quic_bit (RFC 9287 section 3).
    void accept_cleared_quic_bit(bool accept) noexcept { m_accept_cleared_bit = accept; }

    // Seals `writer`'s packet under this side's keys at its level, as QuicPacketWriter::seal. A Handshake packet
    // from a client discards the Initial keys. A 1-RTT packet first starts a key update when the key is due and
    // one is allowed (section 6.1). NoKeys, Exhausted, Refused.
    [[nodiscard]] QuicPacketError seal(QuicPacketWriter& writer, usize used, QuicPadding padding,
                                       usize& written) noexcept;

    // Opens `packet` (a QuicDatagramPackets extent, modified in place) whose header is `header`.
    [[nodiscard]] QuicPacketError open(const QuicHeader& header, cont::Span<u8> packet, QuicOpened& out) noexcept;

    // The largest packet number opened in `space`, which the next packet's decoding expects one past; false before
    // any.
    [[nodiscard]] bool largest_opened(QuicSpace space, u64& out) const noexcept;

    // The peer acknowledged 1-RTT packets up to `largest`: a key update is allowed again once a packet of the current
    // send phase is acknowledged (section 6.1).
    void acknowledged(u64 largest) noexcept;
    // This side sent an acknowledgement in a 1-RTT packet of the current send phase: the peer may update again (a
    // second update before it is KEY_UPDATE_ERROR, section 6.2).
    void sent_acknowledgement() noexcept;
    // Starts a key update now: false when one is not allowed (handshake not confirmed, no packet of the current phase
    // acknowledged, the previous receive key still held, or the peer has not answered the last update).
    [[nodiscard]] bool update_keys() noexcept;
    [[nodiscard]] bool can_update_keys() const noexcept;
    // Whether the receive key of the previous phase is still held, and its discard, which the connection schedules
    // three PTOs after the update (section 6.5).
    [[nodiscard]] bool previous_held() const noexcept { return m_phases.previous_held; }
    void discard_previous() noexcept;

    // Starts a key update once one send key has sealed three quarters of `packets` (a transport's own interval; 0: only
    // the AEAD's limit does), as soon as RFC 9001 section 6 lets the next update start: a peer slow to acknowledge the
    // current phase holds the update back and the key keeps sealing, so an interval never ends a connection.
    void set_key_update_interval(u64 packets) noexcept { m_update_interval = packets; }
    // Lowers the 1-RTT packets one send key seals at most (never above the AEAD's limit; 0 restores it), at which it
    // refuses (Exhausted), and the failed opens the connection tolerates (0: the AEAD's integrity limit). Both make
    // the limits' paths provable in a test: 2^23 packets, or the integrity limit's failed opens, are out of a test's
    // reach.
    void limit_sealed_packets(u64 limit) noexcept;
    void limit_failed_opens(u64 limit) noexcept { m_failed_limit = limit; }

    [[nodiscard]] u64 send_phase() const noexcept { return m_phases.send; }
    [[nodiscard]] u64 receive_phase() const noexcept { return m_phases.recv; }
    [[nodiscard]] u64 failed_opens() const noexcept { return m_forged; }

private:
    // 1-RTT key phase bookkeeping (RFC 9001 section 6).
    struct Phases
    {
        u64 send = 0U;          // the send key phase
        u64 send_first = 0U;    // the first packet number sealed in it
        u64 recv = 0U;          // the receive key phase
        u64 recv_smallest = 0U; // the packet numbers opened under the current receive key
        u64 recv_largest = 0U;
        u64 older_largest = 0U; // the largest opened under any older key
        bool send_first_known = false;
        bool send_acked = false; // a packet of the current send phase is acknowledged
        bool recv_any = false;   // a packet opened under the current receive key
        bool older_any = false;  // a packet opened under an older key
        bool previous_held = false;
        bool answer_owed = false; // the peer updated and has not yet seen an acknowledgement in the new phase
    };

    [[nodiscard]] crypto::CryptoError finish_install(crypto::QuicLevel level, bool sending) noexcept;
    [[nodiscard]] QuicPacketError open_application(cont::Span<u8> packet, usize header_bytes, u64 number, bool phase,
                                                   QuicOpened& out) noexcept;
    [[nodiscard]] QuicPacketError forged(crypto::AlgorithmId aead) noexcept;
    // What a 1-RTT send key may seal: the transport's cap, or above every AEAD's limit, which set_limit lowers to its
    // own.
    [[nodiscard]] u64 send_limit() const noexcept { return (m_sealed_limit != 0U) ? m_sealed_limit : ~u64{0}; }
    [[nodiscard]] bool update_due(const crypto::QuicPacketKey& key) const noexcept
    {
        return key.update_due() ||
               (m_update_interval != 0U && key.sealed() >= m_update_interval - (m_update_interval / 4U));
    }
    void rotate_receive(u64 number) noexcept;
    [[nodiscard]] QuicKeys& initial_receive() noexcept { return m_initial[m_initial_receive]; }
    [[nodiscard]] const QuicKeys& initial_receive() const noexcept { return m_initial[m_initial_receive]; }
    [[nodiscard]] QuicKeys& initial_other() noexcept { return m_initial[3U - m_initial_receive]; }
    [[nodiscard]] const QuicKeys& initial_other() const noexcept { return m_initial[3U - m_initial_receive]; }
    void clear_all() noexcept;

    // [send, receive, receive]: the receive key of m_version at m_initial_receive, the other one m_other's
    // (expect_version, change_version).
    QuicKeys m_initial[3];
    usize m_initial_receive = 1U;
    QuicKeys m_handshake[2]; // [send, receive]
    QuicKeys m_early;        // a client's to send, a server's to open

    // 1-RTT: the send key of the current phase (indexed by the phase's bit) and, during an update, its successor; one
    // header key per direction; three receive keys rotating as previous, current and next.
    crypto::QuicPacketKey m_send[2];
    crypto::QuicHeaderKey m_send_header;
    crypto::QuicPacketKey m_recv[3];
    crypto::QuicHeaderKey m_recv_header;
    usize m_recv_current = 0U;
    Phases m_phases;

    crypto::QuicPacketWindow m_windows[kQuicSpaces];

    crypto::QuicVersion m_version = crypto::QuicVersion::V1;
    crypto::QuicVersion m_original = crypto::QuicVersion::V1;
    crypto::QuicVersion m_other = crypto::QuicVersion::V1; // valid while its receive key is ready
    crypto::QuicSide m_side = crypto::QuicSide::Client;
    u64 m_forged = 0U;       // failed opens across all keys (section 6.6)
    u64 m_failed_limit = 0U; // configuration, kept by start()
    u64 m_update_interval = 0U;
    u64 m_sealed_limit = 0U;
    bool m_started = false;
    bool m_confirmed = false;
    bool m_accept_cleared_bit = false;
    bool m_initial_gone = false;
    bool m_handshake_gone = false;
    bool m_early_gone = false;
};
#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif

} // namespace crd::net
