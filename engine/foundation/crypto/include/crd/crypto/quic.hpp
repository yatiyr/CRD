#pragma once

// crd-crypto -- QUIC packet protection (NET.2; contract: docs/design/network-foundation.md#quic-protection).
//
// The cryptography of RFC 9001 section 5 for QUIC versions 1 and 2 (RFC 9369), over the provider interface: the
// Initial secrets, packet protection keys and their key updates, header protection, packet number encoding and the
// Retry integrity tag; and the address-validation tokens of RFC 9000 section 8.1. Which secrets exist and when is the
// TLS handshake's (the QUIC integration of tls.hpp); framing, connection IDs and loss recovery are the transport's
// (crd-net, 4.2a). Rules:
//
//   packets   a packet is one buffer: the header (its packet number already encoded in it) and then the payload, with
//             the AEAD tag's kQuicTagBytes at its end. Sealing and opening run in place; header protection reads its
//             sample from the protected payload, 4 bytes after the packet number's first byte (RFC 9001 section
//             5.4.2), so a packet whose payload is too short to sample is BadLength, never partly protected.
//   limits    a packet key counts the packets it seals and refuses (Exhausted) to seal past its AEAD's confidentiality
//             limit (RFC 9001 section 6.6), or a lower one the transport sets; the transport updates the key first,
//             when update_due() says so. Packets that
//             fail to open are counted by the connection across all its keys against quic_integrity_limit; a key
//             counts nothing on open, so a receiver never enforces its peer's limit.
//   threads   init, update and seal need the caller's exclusive access (one sender per key); open, header protection
//             and the Retry tag are read-only and run on several threads at once.
//   secrets   a packet key keeps its secret for the next key phase and zeroes it, with every derived key, when cleared
//             or destroyed; secrets the caller passes in remain the caller's to zero (secure_zero).
//   replay    a receiver keeps a QuicPacketWindow per packet number space: it checks a decoded packet number before
//             the AEAD opens (a duplicate costs no decryption) and accepts it after, so a duplicated or replayed
//             packet is discarded (RFC 9000 section 12.3) and a forged one never moves the window.
//   allocation nothing here allocates but a token replay filter's init: every key and window is held inline and the
//             Retry pseudo-packet and every token's associated data are built on the stack.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/register_ids.hpp>
#include <crd/crypto/replay.hpp>

namespace crd::crypto
{
// The QUIC versions whose packet protection is implemented: version 1 (RFC 9000) and version 2 (RFC 9369), which
// differs only in its salt, its labels and its Retry key.
enum class QuicVersion : u32
{
    V1 = 0x00000001U,
    V2 = 0x6B3343CFU,
};

// Whose packets a key protects: the client's or the server's Initial secret.
enum class QuicSide : u8
{
    Client,
    Server,
};

inline constexpr usize kQuicTagBytes             = kAeadTagBytes;
inline constexpr usize kQuicSampleBytes          = 16U;
inline constexpr usize kQuicMaskBytes            = 5U;  // the first byte's bits and up to 4 packet number bytes
inline constexpr usize kQuicMaxSecretBytes       = 48U; // a SHA-384 suite's traffic secret
inline constexpr usize kQuicInitialSecretBytes   = 32U; // SHA-256
inline constexpr usize kQuicMaxConnectionIdBytes = 20U; // RFC 9000 section 17.2
inline constexpr usize kQuicMaxRetryBytes        = 1472U; // the UDP payload of a 1500-byte IPv4 path
inline constexpr u64   kQuicMaxPacketNumber      = (u64{1} << 62U) - 1U;

// Whether this module protects packets of `version`.
[[nodiscard]] bool quic_version_supported(u32 version) noexcept;

// The secret of `side`'s Initial packets for a connection whose client chose `client_dcid` as its first Destination
// Connection ID (RFC 9001 section 5.2, RFC 9369 section 3.3.1): kQuicInitialSecretBytes into `secret`. A connection ID
// longer than kQuicMaxConnectionIdBytes is BadLength; an unknown version Unsupported.
[[nodiscard]] CryptoError quic_initial_secret(QuicVersion version, QuicSide side, cont::ConstSpan<u8> client_dcid,
                                              cont::Span<u8> secret) noexcept;

// The packets one key of `aead` may seal (RFC 9001 section 6.6): 2^23 for AES-GCM; ChaCha20-Poly1305's limit lies
// beyond every packet number (2^62), so it is that bound. The packets a connection may fail to open across all its
// keys before it closes with AEAD_LIMIT_REACHED: 2^52 for AES-GCM, 2^36 for ChaCha20-Poly1305. 0 for anything else.
[[nodiscard]] u64 quic_confidentiality_limit(AlgorithmId aead) noexcept;
[[nodiscard]] u64 quic_integrity_limit(AlgorithmId aead) noexcept;

// One direction's packet protection key for one key phase: the AEAD key and IV derived from a traffic secret with
// the version's "quic key" and "quic iv" labels ("quicv2 ..." for version 2). The suite is the TLS 1.3 cipher suite
// the handshake chose, named by its AEAD: AES-128-GCM and ChaCha20-Poly1305 with SHA-256, AES-256-GCM with SHA-384.
class QuicPacketKey
{
public:
    QuicPacketKey() noexcept = default;
    ~QuicPacketKey() noexcept;
    QuicPacketKey(const QuicPacketKey&)            = delete;
    QuicPacketKey& operator=(const QuicPacketKey&) = delete;
    QuicPacketKey(QuicPacketKey&&)                 = delete;
    QuicPacketKey& operator=(QuicPacketKey&&)      = delete;

    // `secret` is the suite's hash length (32 or 48 bytes; BadLength otherwise); an AEAD outside the three suites is
    // Unsupported. Replaces any previous key and starts its count at zero.
    [[nodiscard]] CryptoError init(QuicVersion version, AlgorithmId aead, cont::ConstSpan<u8> secret) noexcept;
    // The next key phase after `current` (RFC 9001 section 6.1): its secret expanded with "quic ku" ("quicv2 ku"),
    // the same suite and version, a fresh count. `current` stays as it is (a receiver keeps it for reordered
    // packets); NotReady when it holds no key, Overlap when it is this key, Unsupported for a version init would
    // refuse (never for a ready key).
    [[nodiscard]] CryptoError update(const QuicPacketKey& current) noexcept;
    void                      clear() noexcept;

    [[nodiscard]] bool        ready() const noexcept { return m_aead.ready(); }
    [[nodiscard]] AlgorithmId algorithm() const noexcept { return m_aead.algorithm(); }
    [[nodiscard]] QuicVersion version() const noexcept { return m_version; }
    [[nodiscard]] u64         sealed() const noexcept { return m_sealed; }
    [[nodiscard]] u64         limit() const noexcept { return m_limit; }

    // Lowers the packets this key may seal (a transport's own key update interval), never above the AEAD's
    // confidentiality limit and never below one packet; init() and update() restore the AEAD's limit. A key that holds
    // none ignores it.
    void set_limit(u64 packets) noexcept;

    // Whether the sender should update this key before sealing more: from three quarters of its limit, so a key
    // update completes (the peer acknowledges the new phase) long before the limit refuses a packet.
    [[nodiscard]] bool update_due() const noexcept { return ready() && m_sealed >= m_limit - m_limit / 4U; }

    // Seals packet number `packet_number`'s packet in place: `packet` holds the header (`header_bytes`, the
    // associated data), the plaintext payload, then kQuicTagBytes the tag is written to. The nonce is the IV
    // exclusive-or the packet number. BadLength when the packet number passes kQuicMaxPacketNumber or the packet has
    // no room for a tag; Exhausted at the confidentiality limit, with nothing written.
    [[nodiscard]] CryptoError seal(u64 packet_number, cont::Span<u8> packet, usize header_bytes) noexcept;

    // Opens a packet sealed as above, in place: the plaintext payload is then packet[header_bytes, size -
    // kQuicTagBytes). Forged zeroes it (no unauthenticated byte is released); the tag bytes stay as received.
    [[nodiscard]] CryptoError open(u64 packet_number, cont::Span<u8> packet, usize header_bytes) const noexcept;

private:
    AeadKey     m_aead;
    u8          m_iv[kAeadNonceBytes]       = {};
    u8          m_secret[kQuicMaxSecretBytes] = {};
    u8          m_secret_bytes              = 0U;
    QuicVersion m_version                   = QuicVersion::V1;
    u64         m_sealed                    = 0U;
    u64         m_limit                     = 0U;
};

inline constexpr usize kQuicHeaderStateBytes = 256U; // the provider's expanded AES key, held inline

// One direction's header protection key, derived from the first traffic secret of an encryption level with "quic hp"
// ("quicv2 hp") and kept across key updates (RFC 9001 section 5.4): AES-ECB on the sample for the AES-GCM suites,
// ChaCha20 with the sample as counter and nonce for ChaCha20-Poly1305.
class QuicHeaderKey
{
public:
    QuicHeaderKey() noexcept = default;
    ~QuicHeaderKey() noexcept;
    QuicHeaderKey(const QuicHeaderKey&)            = delete;
    QuicHeaderKey& operator=(const QuicHeaderKey&) = delete;
    QuicHeaderKey(QuicHeaderKey&&)                 = delete;
    QuicHeaderKey& operator=(QuicHeaderKey&&)      = delete;

    // As QuicPacketKey::init.
    [[nodiscard]] CryptoError init(QuicVersion version, AlgorithmId aead, cont::ConstSpan<u8> secret) noexcept;
    void                      clear() noexcept;

    [[nodiscard]] bool        ready() const noexcept { return m_algorithm != AlgorithmId{}; }
    [[nodiscard]] AlgorithmId algorithm() const noexcept { return m_algorithm; }

    // The kQuicMaskBytes mask of a kQuicSampleBytes sample.
    [[nodiscard]] CryptoError mask(cont::ConstSpan<u8> sample, cont::Span<u8> out) const noexcept;

    // Protects a sealed packet's header in place: `packet_number_offset` is where its packet number begins, and the
    // packet number's length is read from the still unprotected first byte. The first byte's low four bits (long
    // header) or five bits (short header) and the packet number are masked.
    [[nodiscard]] CryptoError protect(cont::Span<u8> packet, usize packet_number_offset) const noexcept;

    // Removes the protection in place and reports the packet number's length (1 to 4) and its truncated value, which
    // quic_decode_packet_number expands. The packet's AEAD is still to be opened: until it opens, nothing read here
    // (the key phase bit and the reserved bits included) may change the connection's state.
    [[nodiscard]] CryptoError unprotect(cont::Span<u8> packet, usize packet_number_offset, usize& packet_number_bytes,
                                        u64& truncated) const noexcept;

private:
    alignas(16) u8 m_state[kQuicHeaderStateBytes] = {};
    AlgorithmId m_algorithm{};
};

// The bytes (1 to 4) a sender encodes packet number `packet_number` in when the largest packet its peer acknowledged
// is `largest_acked` (`acknowledged` false before any): enough for twice the unacknowledged range (RFC 9000 section
// 17.1 and appendix A.2). 0 when the packet number passes kQuicMaxPacketNumber or does not exceed `largest_acked`.
[[nodiscard]] usize quic_packet_number_bytes(u64 packet_number, u64 largest_acked, bool acknowledged) noexcept;

// The full packet number a receiver expects `truncated` (the low `packet_number_bytes` bytes) to stand for, the
// closest to `expected`, one more than the largest it has processed in the space (0 before any) (RFC 9000 appendix
// A.3). A length outside 1 to 4 decodes nothing: `truncated` comes back as it is.
[[nodiscard]] u64 quic_decode_packet_number(u64 truncated, usize packet_number_bytes, u64 expected) noexcept;

// The packet numbers a receiver processed in one packet number space (RFC 9000 section 12.3), and the largest, whose
// successor is quic_decode_packet_number's `expected`. A packet arriving more than kSpan (192) packets after a later
// one is Stale: RFC 9002 declared it lost long before (three later packets, or 9/8 of a round trip), so its frames are
// already being sent again, and dropping it costs at most that retransmission. Initial, Handshake and Application
// packets each have their own; a key update keeps the space's window.
inline constexpr usize kQuicPacketWindowBits = 256U;
using QuicPacketWindow                       = ReplayWindow<kQuicPacketWindowBits>;

// The Retry integrity tag (RFC 9001 section 5.8, RFC 9369 section 3.3.3): AES-128-GCM under the version's fixed key
// and nonce over the Retry pseudo-packet, the client's original Destination Connection ID with its length byte, then
// `retry` (the Retry packet without its tag, at most kQuicMaxRetryBytes - kQuicTagBytes long). kQuicTagBytes into
// `tag`.
[[nodiscard]] CryptoError quic_retry_tag(QuicVersion version, cont::ConstSpan<u8> original_dcid,
                                         cont::ConstSpan<u8> retry, cont::Span<u8> tag) noexcept;

// Checks a received Retry packet (`retry` ends with its tag; at most kQuicMaxRetryBytes): None when the tag
// authenticates it for `original_dcid`, Forged when not, BadLength for a packet too short to hold a tag or too long.
[[nodiscard]] CryptoError quic_retry_verify(QuicVersion version, cont::ConstSpan<u8> original_dcid,
                                            cont::ConstSpan<u8> retry) noexcept;

// ---- Address validation tokens (RFC 9000 section 8.1; contract: network-foundation.md#address-validation) ----
//
// A server validates a client's address before it sends more than three times what it received, statelessly: the
// token it hands out holds everything it needs back. Tokens are minted and checked here; the transport (crd-net, 4.2a)
// builds the Retry packet around a token (quic_retry_tag), carries tokens in Initial packets and NEW_TOKEN frames,
// and puts the claims' original connection ID and the Retry's connection ID into its transport parameters (RFC 9000
// section 7.3). Rules:
//
//   format    kind (one byte, in the clear: RFC 9000 section 8.1.1), the sealing key's 4-byte public name, a 16-byte
//             random salt, then the sealed claims (the issue time, and a Retry token's original Destination
//             Connection ID) and the AEAD tag. Each token is sealed with AES-256-GCM under its own key and nonce,
//             HKDF-SHA256 of the fleet key and the token's salt, so the servers of a fleet that share the key share
//             no count of tokens and no nonce ever repeats; the salt also makes every token unique (section 8.1.3).
//   binding   the associated data binds the clear bytes, the QUIC version and the client's IP address; a Retry token
//             also its UDP port and both connection IDs of the client's next Initial (the Retry's Source Connection ID,
//             now its Destination, and the client's own Source, which it may not change, section 17.2.5.2). A
//             NEW_TOKEN token binds no port: a client's port changes between connections (section 8.1.3).
//   time      `now` and the issue time are seconds of the fleet's clock (Unix time). A Retry token is accepted for
//             kQuicRetryTokenLifetime, a NEW_TOKEN token for the keys' configured lifetime, each up to
//             kQuicTokenClockSkew before it was issued (servers' clocks differ); outside that it is Expired.
//   rotation  the keys hold a sealing key and the key before its last rotation, which only opens: every server of a
//             fleet rotates to the same next key, and tokens issued under the previous one stay valid until they
//             expire. A token under any other key is Forged, never told apart from a forgery.
//   replay    a NEW_TOKEN token is accepted once (section 8.1.4) when the server checks it against a ReplayFilter;
//             a Retry token is limited by its short lifetime and its port and connection ID binding.
//   threads   minting and validation are read-only on the keys and run on several threads at once; a replay filter's
//             check runs on several threads too. init and clear need the caller's exclusive access.
//   allocation minting and validation allocate nothing; a replay filter allocates its bits once, at init.

enum class QuicTokenKind : u8
{
    None     = 0x00U, // not a token this build mints
    Retry    = 0x11U, // format 1, sent in a Retry packet
    NewToken = 0x12U, // format 1, sent in a NEW_TOKEN frame
};

inline constexpr usize kQuicTokenKeyBytes       = 32U;
inline constexpr usize kQuicTokenNameBytes      = 4U;
inline constexpr usize kQuicTokenSaltBytes      = 16U; // 128 bits of entropy (RFC 9000 section 8.1.4)
inline constexpr usize kQuicTokenHeaderBytes    = 1U + kQuicTokenNameBytes + kQuicTokenSaltBytes;
inline constexpr usize kQuicNewTokenBytes       = kQuicTokenHeaderBytes + 8U + kQuicTagBytes;
inline constexpr usize kQuicMaxRetryTokenBytes  = kQuicNewTokenBytes + 1U + kQuicMaxConnectionIdBytes;
inline constexpr usize kQuicMaxTokenBytes       = kQuicMaxRetryTokenBytes;
inline constexpr u64   kQuicRetryTokenLifetime  = 30U; // seconds: four Initial retransmissions from a 1 s PTO
inline constexpr u64   kQuicNewTokenLifetime    = 86400U;      // seconds, unless configured
inline constexpr u64   kQuicMaxNewTokenLifetime = 7U * 86400U; // seconds
inline constexpr u64   kQuicTokenClockSkew      = 10U;         // seconds a token may come from the future

// Who a token is bound to: the client's IP address as the transport canonicalises it (4 bytes for IPv4, an IPv4-mapped
// IPv6 address included, 16 for IPv6) and its UDP port, which only a Retry token binds.
struct QuicTokenPeer
{
    cont::ConstSpan<u8> address;
    u16                 port = 0U;
};

// What a valid token carried.
struct QuicTokenClaims
{
    QuicTokenKind kind   = QuicTokenKind::None;
    u64           issued = 0U;
    u8            original_dcid_bytes                     = 0U; // a Retry token's; 0 for a NEW_TOKEN token
    u8            original_dcid[kQuicMaxConnectionIdBytes] = {};

    [[nodiscard]] cont::ConstSpan<u8> original_dcid_view() const noexcept
    {
        return cont::ConstSpan<u8>(original_dcid, original_dcid_bytes);
    }
};

// The kind a token claims, read from its first byte and not authenticated: what tells a server whether an invalid
// token came from a Retry, so that it closes with INVALID_TOKEN (RFC 9000 section 8.1.2), or from anything else, so
// that it proceeds as if the client had none (section 8.1.3). None for an empty token or one of another format.
[[nodiscard]] QuicTokenKind quic_token_kind(cont::ConstSpan<u8> token) noexcept;

class QuicTokenKeys
{
public:
    QuicTokenKeys() noexcept = default;
    ~QuicTokenKeys() noexcept;
    QuicTokenKeys(const QuicTokenKeys&)            = delete;
    QuicTokenKeys& operator=(const QuicTokenKeys&) = delete;
    QuicTokenKeys(QuicTokenKeys&&)                 = delete;
    QuicTokenKeys& operator=(QuicTokenKeys&&)      = delete;

    // `key` (kQuicTokenKeyBytes) seals and opens; `previous` (empty, or the key before the last rotation) only opens.
    // `new_token_lifetime` is 1 second to kQuicMaxNewTokenLifetime. BadLength for a key of another length or a
    // lifetime out of range, BadKey when the two keys are equal or their names collide.
    [[nodiscard]] CryptoError init(cont::ConstSpan<u8> key, cont::ConstSpan<u8> previous,
                                   u64 new_token_lifetime = kQuicNewTokenLifetime) noexcept;
    void                      clear() noexcept;

    [[nodiscard]] bool ready() const noexcept { return m_count != 0U; }
    [[nodiscard]] u64  new_token_lifetime() const noexcept { return m_new_token_lifetime; }

    // A Retry token for the client Initial whose Destination Connection ID was `original_dcid` and whose Source
    // Connection ID was `client_scid`, from `peer`, answered by a Retry whose Source Connection ID is `retry_scid`:
    // kQuicNewTokenBytes + 1 + original_dcid.size() bytes into `out`, `written` set. BadLength when `out` is too small,
    // an address is not 4 or 16 bytes or a connection ID is longer than kQuicMaxConnectionIdBytes; Overlap when
    // `retry_scid` equals `original_dcid` (RFC 9000 section 17.2.5.1); NotReady without keys; Unsupported for a
    // version quic_version_supported() refuses.
    [[nodiscard]] CryptoError mint_retry(QuicVersion version, const QuicTokenPeer& peer,
                                         cont::ConstSpan<u8> original_dcid, cont::ConstSpan<u8> client_scid,
                                         cont::ConstSpan<u8> retry_scid, u64 now, cont::Span<u8> out,
                                         usize& written) const noexcept;

    // A NEW_TOKEN token for the client at `peer`'s address (its port is not bound): kQuicNewTokenBytes into `out`.
    [[nodiscard]] CryptoError mint_new_token(QuicVersion version, const QuicTokenPeer& peer, u64 now,
                                             cont::Span<u8> out, usize& written) const noexcept;

    // Validates the token of a client Initial from `peer` whose connection IDs are `dcid` and `scid`: None with
    // `claims` filled when it is authentic, bound to all of them and within its lifetime; Forged when it is not
    // authentic or bound to anything else (another address, port, connection ID or version, another kind, a key
    // these keys do not hold, a malformed body), Expired outside its lifetime, Replayed for a NEW_TOKEN token
    // `replay` saw before, BadLength for a token too short or too long to be one or an address not 4 or 16 bytes,
    // NotReady without keys or with a replay filter that is not ready or whose period is shorter than the NEW_TOKEN
    // lifetime or kQuicTokenClockSkew, Unsupported for a version quic_version_supported() refuses. `claims` is only
    // written on None.
    [[nodiscard]] CryptoError validate(QuicVersion version, cont::ConstSpan<u8> token, const QuicTokenPeer& peer,
                                       cont::ConstSpan<u8> dcid, cont::ConstSpan<u8> scid, u64 now,
                                       QuicTokenClaims& claims, ReplayFilter* replay = nullptr) const noexcept;

private:
    [[nodiscard]] CryptoError mint(QuicVersion version, QuicTokenKind kind, const QuicTokenPeer& peer,
                                   cont::ConstSpan<u8> original_dcid, cont::ConstSpan<u8> client_scid,
                                   cont::ConstSpan<u8> retry_scid, u64 now, cont::Span<u8> out,
                                   usize& written) const noexcept;

    u8  m_keys[2][kQuicTokenKeyBytes]   = {};
    u8  m_names[2][kQuicTokenNameBytes] = {};
    u32 m_count                         = 0U;
    u64 m_new_token_lifetime            = 0U;
};
} // namespace crd::crypto
