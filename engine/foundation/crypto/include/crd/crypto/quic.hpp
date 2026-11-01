#pragma once

// crd-crypto -- QUIC packet protection (NET.2; contract: docs/design/network-foundation.md#quic-protection).
//
// The cryptography of RFC 9001 section 5 for QUIC versions 1 and 2 (RFC 9369), over the provider interface: the
// Initial secrets, packet protection keys and their key updates, header protection, packet number encoding, and the
// Retry integrity tag. Which secrets exist and when is the TLS handshake's (the QUIC integration of tls.hpp); framing,
// connection IDs and loss recovery are the transport's (crd-net, 4.2a). Rules:
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
//   allocation nothing here allocates: every key is held inline and the Retry pseudo-packet is built on the stack.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/register_ids.hpp>

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
    // packets); NotReady when it holds no key, Overlap when it is this key.
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
} // namespace crd::crypto
