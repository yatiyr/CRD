// crd-crypto -- QUIC packet protection (NET.2; contract: docs/design/network-foundation.md#quic-protection).
//
// RFC 9001 section 5 and RFC 9369 section 3.3 over the provider interface (HKDF and the AEAD keys of provider.hpp);
// only header protection reaches the provider directly, for its AES block and ChaCha20 stream functions. The
// address-validation tokens of RFC 9000 section 8.1 (contract: network-foundation.md#address-validation) are sealed
// as tls.cpp seals its tickets, through the sealing keys of provider_internal.hpp.

#include <crd/crypto/quic.hpp>

#include "byte_spans.hpp"
#include "provider_internal.hpp"

#include <openssl/aes.h>
#include <openssl/chacha.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace crd::crypto
{
namespace
{
static_assert(sizeof(AES_KEY) <= kQuicHeaderStateBytes && alignof(AES_KEY) <= 16U,
              "QuicHeaderKey holds the provider's expanded AES key inline");

constexpr usize kChachaKeyBytes = 32U;
static_assert(kChachaKeyBytes <= kQuicHeaderStateBytes);

// What separates the versions (RFC 9001 sections 5.2 and 5.8, RFC 9369 section 3.3).
struct VersionParameters
{
    QuicVersion version;
    const char* key;
    const char* iv;
    const char* hp;
    const char* ku;
    u8          salt[20];
    u8          retry_key[16];
    u8          retry_nonce[kAeadNonceBytes];
};

constexpr VersionParameters kVersions[] = {
    {QuicVersion::V1,
     "quic key",
     "quic iv",
     "quic hp",
     "quic ku",
     {0x38, 0x76, 0x2C, 0xF7, 0xF5, 0x59, 0x34, 0xB3, 0x4D, 0x17,
      0x9A, 0xE6, 0xA4, 0xC8, 0x0C, 0xAD, 0xCC, 0xBB, 0x7F, 0x0A},
     {0xBE, 0x0C, 0x69, 0x0B, 0x9F, 0x66, 0x57, 0x5A, 0x1D, 0x76, 0x6B, 0x54, 0xE3, 0x68, 0xC8, 0x4E},
     {0x46, 0x15, 0x99, 0xD3, 0x5D, 0x63, 0x2B, 0xF2, 0x23, 0x98, 0x25, 0xBB}},
    {QuicVersion::V2,
     "quicv2 key",
     "quicv2 iv",
     "quicv2 hp",
     "quicv2 ku",
     {0x0D, 0xED, 0xE3, 0xDE, 0xF7, 0x00, 0xA6, 0xDB, 0x81, 0x93,
      0x81, 0xBE, 0x6E, 0x26, 0x9D, 0xCB, 0xF9, 0xBD, 0x2E, 0xD9},
     {0x8F, 0xB4, 0xB0, 0x1B, 0x56, 0xAC, 0x48, 0xE2, 0x60, 0xFB, 0xCB, 0xCE, 0xAD, 0x7C, 0xCC, 0x92},
     {0xD8, 0x69, 0x69, 0xBC, 0x2D, 0x7C, 0x6D, 0x99, 0x90, 0xEF, 0xB0, 0x4A}},
};

const VersionParameters* parameters_of(QuicVersion version) noexcept
{
    for (const VersionParameters& parameters : kVersions)
    {
        if (parameters.version == version)
        {
            return &parameters;
        }
    }
    return nullptr;
}

// The HKDF hash of the TLS 1.3 suite an AEAD names; AlgorithmId{} outside the three suites.
AlgorithmId suite_hash(AlgorithmId aead) noexcept
{
    switch (aead)
    {
    case AlgorithmId::Aes128Gcm:
    case AlgorithmId::Chacha20Poly1305:
        return AlgorithmId::HkdfSha256;
    case AlgorithmId::Aes256Gcm:
        return AlgorithmId::HkdfSha384;
    default:
        return AlgorithmId{};
    }
}

// HKDF-Expand-Label(secret, label, "", out.size()) of TLS 1.3 (RFC 8446 section 7.1), which QUIC uses unchanged.
CryptoError expand_label(AlgorithmId hkdf, cont::ConstSpan<u8> secret, const char* label, cont::Span<u8> out) noexcept
{
    constexpr char  prefix[]        = "tls13 ";
    constexpr usize prefix_bytes    = sizeof(prefix) - 1U;
    constexpr usize max_label_bytes = 16U; // "quicv2 key" is the longest label QUIC uses
    const usize     label_bytes     = std::strlen(label);
    u8              info[4U + prefix_bytes + max_label_bytes];
    if (label_bytes > max_label_bytes || out.size() > 0xFFFFU)
    {
        return CryptoError::BadLength;
    }
    info[0] = static_cast<u8>(out.size() >> 8U);
    info[1] = static_cast<u8>(out.size());
    info[2] = static_cast<u8>(prefix_bytes + label_bytes);
    std::memcpy(info + 3, prefix, prefix_bytes);
    std::memcpy(info + 3 + prefix_bytes, label, label_bytes);
    info[3U + prefix_bytes + label_bytes] = 0U; // an empty context
    return hkdf_expand(hkdf, secret, cont::ConstSpan<u8>(info, 4U + prefix_bytes + label_bytes), out);
}

// `label`'s output from a session secret: its PRK under the keyed path's own salt, so these secrets are nothing else's.
CryptoError keyed_expand(AlgorithmId hkdf, cont::ConstSpan<u8> session_secret, const char* label,
                         cont::Span<u8> out) noexcept
{
    constexpr u8 salt[] = {'c', 'e', 'r', 'i', 'd', ' ', 'k', 'e', 'y', 'e', 'd', ' ', 'p', 'a', 't', 'h'};
    if (session_secret.size() != kQuicKeyedSecretBytes)
    {
        return CryptoError::BadLength;
    }
    u8 prk[kQuicMaxSecretBytes];
    const cont::Span<u8> key(prk, hkdf_hash_bytes(hkdf));
    CryptoError error = hkdf_extract(hkdf, salt, session_secret, key);
    if (error == CryptoError::None)
    {
        error = expand_label(hkdf, key, label, out);
    }
    secure_zero(prk);
    return error;
}

// The checks init() shares: the version's parameters and the suite's hash, or the refusal.
CryptoError suite_of(QuicVersion version, AlgorithmId aead, cont::ConstSpan<u8> secret,
                     const VersionParameters*& parameters, AlgorithmId& hkdf) noexcept
{
    parameters = parameters_of(version);
    hkdf       = suite_hash(aead);
    if (parameters == nullptr || hkdf == AlgorithmId{})
    {
        return CryptoError::Unsupported;
    }
    return secret.size() == hkdf_hash_bytes(hkdf) ? CryptoError::None : CryptoError::BadLength;
}

void packet_nonce(const u8 (&iv)[kAeadNonceBytes], u64 packet_number, u8 (&nonce)[kAeadNonceBytes]) noexcept
{
    std::memcpy(nonce, iv, kAeadNonceBytes);
    for (usize i = 0U; i < 8U; ++i)
    {
        nonce[kAeadNonceBytes - 1U - i] ^= static_cast<u8>(packet_number >> (8U * i));
    }
}

// The sample's offset (RFC 9001 section 5.4.2: as if the packet number were 4 bytes long), or 0 when the packet is too
// short to hold the sample.
usize sample_offset(usize packet_bytes, usize packet_number_offset) noexcept
{
    if (packet_number_offset == 0U || packet_number_offset > packet_bytes ||
        packet_bytes - packet_number_offset < 4U + kQuicSampleBytes)
    {
        return 0U;
    }
    return packet_number_offset + 4U;
}

u8 first_byte_mask(u8 first) noexcept
{
    return (first & 0x80U) != 0U ? u8{0x0FU} : u8{0x1FU};
}

constexpr u64   kAesGcmConfidentialityLimit = u64{1} << 23U;
constexpr u64   kAesGcmIntegrityLimit       = u64{1} << 52U;
constexpr u64   kChachaIntegrityLimit       = u64{1} << 36U;
constexpr usize kRetryPseudoBytes           = 1U + kQuicMaxConnectionIdBytes + kQuicMaxRetryBytes;
} // namespace

bool quic_version_supported(u32 version) noexcept
{
    return parameters_of(static_cast<QuicVersion>(version)) != nullptr;
}

CryptoError quic_initial_secret(QuicVersion version, QuicSide side, cont::ConstSpan<u8> client_dcid,
                                cont::Span<u8> secret) noexcept
{
    const VersionParameters* parameters = parameters_of(version);
    if (parameters == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (client_dcid.size() > kQuicMaxConnectionIdBytes || secret.size() != kQuicInitialSecretBytes)
    {
        return CryptoError::BadLength;
    }
    u8          initial[kQuicInitialSecretBytes];
    CryptoError error = hkdf_extract(AlgorithmId::HkdfSha256, parameters->salt, client_dcid, initial);
    if (error == CryptoError::None)
    {
        error = expand_label(AlgorithmId::HkdfSha256, initial, side == QuicSide::Client ? "client in" : "server in",
                             secret);
    }
    secure_zero(initial);
    if (error != CryptoError::None)
    {
        secure_zero(secret);
    }
    return error;
}

CryptoError quic_keyed_traffic_secret(AlgorithmId aead, cont::ConstSpan<u8> session_secret, QuicSide side,
                                      cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    const AlgorithmId hkdf = suite_hash(aead);
    if (hkdf == AlgorithmId{})
    {
        return CryptoError::Unsupported;
    }
    if (out.size() < kQuicMaxSecretBytes)
    {
        return CryptoError::BadLength;
    }
    const usize bytes = hkdf_hash_bytes(hkdf);
    const CryptoError error = keyed_expand(
        hkdf, session_secret, side == QuicSide::Client ? "keyed c traffic" : "keyed s traffic", out.first(bytes));
    if (error != CryptoError::None)
    {
        secure_zero(out);
        return error;
    }
    written = bytes;
    return CryptoError::None;
}

CryptoError quic_keyed_connection_id(cont::ConstSpan<u8> session_secret, QuicSide side, cont::Span<u8> out) noexcept
{
    if (out.size() > kQuicMaxConnectionIdBytes)
    {
        return CryptoError::BadLength;
    }
    return keyed_expand(AlgorithmId::HkdfSha256, session_secret, side == QuicSide::Client ? "keyed c id" : "keyed s id",
                        out);
}

u64 quic_confidentiality_limit(AlgorithmId aead) noexcept
{
    switch (aead)
    {
    case AlgorithmId::Aes128Gcm:
    case AlgorithmId::Aes256Gcm:
        return kAesGcmConfidentialityLimit;
    case AlgorithmId::Chacha20Poly1305:
        return kQuicMaxPacketNumber + 1U;
    default:
        return 0U;
    }
}

u64 quic_integrity_limit(AlgorithmId aead) noexcept
{
    switch (aead)
    {
    case AlgorithmId::Aes128Gcm:
    case AlgorithmId::Aes256Gcm:
        return kAesGcmIntegrityLimit;
    case AlgorithmId::Chacha20Poly1305:
        return kChachaIntegrityLimit;
    default:
        return 0U;
    }
}

// ---- QuicPacketKey ----

QuicPacketKey::~QuicPacketKey() noexcept
{
    clear();
}

CryptoError QuicPacketKey::init(QuicVersion version, AlgorithmId aead, cont::ConstSpan<u8> secret) noexcept
{
    clear();
    const VersionParameters* parameters = nullptr;
    AlgorithmId              hkdf{};
    CryptoError              error = suite_of(version, aead, secret, parameters, hkdf);
    if (error != CryptoError::None)
    {
        return error;
    }
    u8          key[32];
    const usize key_bytes = aead_key_bytes(aead);
    error                 = expand_label(hkdf, secret, parameters->key, cont::Span<u8>(key, key_bytes));
    if (error == CryptoError::None)
    {
        error = expand_label(hkdf, secret, parameters->iv, m_iv);
    }
    if (error == CryptoError::None)
    {
        error = m_aead.init(aead, cont::ConstSpan<u8>(key, key_bytes));
    }
    secure_zero(key);
    if (error != CryptoError::None)
    {
        clear();
        return error;
    }
    std::memcpy(m_secret, secret.data(), secret.size());
    m_secret_bytes = static_cast<u8>(secret.size());
    m_version      = version;
    m_limit        = quic_confidentiality_limit(aead);
    return CryptoError::None;
}

CryptoError QuicPacketKey::update(const QuicPacketKey& current) noexcept
{
    if (&current == this)
    {
        return CryptoError::Overlap;
    }
    if (!current.ready())
    {
        return CryptoError::NotReady;
    }
    // A ready key's version is one init accepted; the lookup is checked all the same rather than assumed.
    const VersionParameters* parameters = parameters_of(current.m_version);
    if (parameters == nullptr)
    {
        return CryptoError::Unsupported;
    }
    const AlgorithmId aead = current.algorithm();
    const usize       size = current.m_secret_bytes;
    u8                next[kQuicMaxSecretBytes];
    CryptoError       error = expand_label(suite_hash(aead), cont::ConstSpan<u8>(current.m_secret, size),
                                           parameters->ku, cont::Span<u8>(next, size));
    if (error == CryptoError::None)
    {
        error = init(current.m_version, aead, cont::ConstSpan<u8>(next, size));
    }
    else
    {
        clear();
    }
    secure_zero(next);
    return error;
}

void QuicPacketKey::clear() noexcept
{
    m_aead.clear();
    secure_zero(m_iv);
    secure_zero(m_secret);
    m_secret_bytes = 0U;
    m_version      = QuicVersion::V1;
    m_sealed       = 0U;
    m_limit        = 0U;
}

CryptoError QuicPacketKey::seal(u64 packet_number, cont::Span<u8> packet, usize header_bytes) noexcept
{
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    if (packet_number > kQuicMaxPacketNumber || header_bytes > packet.size() ||
        packet.size() - header_bytes < kQuicTagBytes)
    {
        return CryptoError::BadLength;
    }
    if (m_sealed >= m_limit)
    {
        return CryptoError::Exhausted;
    }
    u8 nonce[kAeadNonceBytes];
    packet_nonce(m_iv, packet_number, nonce);
    const usize       payload = packet.size() - header_bytes - kQuicTagBytes;
    const CryptoError error   = m_aead.seal(nonce, packet.subspan(header_bytes, payload), packet.first(header_bytes),
                                            packet.subspan(header_bytes));
    if (error == CryptoError::None)
    {
        ++m_sealed;
    }
    return error;
}

CryptoError QuicPacketKey::open(u64 packet_number, cont::Span<u8> packet, usize header_bytes) const noexcept
{
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    if (packet_number > kQuicMaxPacketNumber || header_bytes > packet.size() ||
        packet.size() - header_bytes < kQuicTagBytes)
    {
        return CryptoError::BadLength;
    }
    u8 nonce[kAeadNonceBytes];
    packet_nonce(m_iv, packet_number, nonce);
    const usize payload = packet.size() - header_bytes - kQuicTagBytes;
    return m_aead.open(nonce, packet.subspan(header_bytes), packet.first(header_bytes),
                       packet.subspan(header_bytes, payload));
}

void QuicPacketKey::set_limit(u64 packets) noexcept
{
    if (ready())
    {
        const u64 ceiling = quic_confidentiality_limit(algorithm());
        m_limit           = packets == 0U ? u64{1} : std::min(packets, ceiling);
    }
}

// ---- QuicHeaderKey ----

QuicHeaderKey::~QuicHeaderKey() noexcept
{
    clear();
}

CryptoError QuicHeaderKey::init(QuicVersion version, AlgorithmId aead, cont::ConstSpan<u8> secret) noexcept
{
    clear();
    const VersionParameters* parameters = nullptr;
    AlgorithmId              hkdf{};
    CryptoError              error = suite_of(version, aead, secret, parameters, hkdf);
    if (error != CryptoError::None)
    {
        return error;
    }
    u8          key[32];
    const usize key_bytes = aead_key_bytes(aead);
    error                 = expand_label(hkdf, secret, parameters->hp, cont::Span<u8>(key, key_bytes));
    if (error == CryptoError::None)
    {
        if (aead == AlgorithmId::Chacha20Poly1305)
        {
            std::memcpy(m_state, key, kChachaKeyBytes);
        }
        else
        {
            AES_KEY* expanded = ::new (static_cast<void*>(m_state)) AES_KEY;
            if (AES_set_encrypt_key(key, static_cast<unsigned>(key_bytes * 8U), expanded) != 0)
            {
                error = CryptoError::ProviderFailure;
            }
        }
    }
    secure_zero(key);
    if (error != CryptoError::None)
    {
        clear();
        return error;
    }
    m_algorithm = aead;
    return CryptoError::None;
}

void QuicHeaderKey::clear() noexcept
{
    secure_zero(m_state);
    m_algorithm = AlgorithmId{};
}

CryptoError QuicHeaderKey::mask(cont::ConstSpan<u8> sample, cont::Span<u8> out) const noexcept
{
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    if (sample.size() != kQuicSampleBytes || out.size() != kQuicMaskBytes)
    {
        return CryptoError::BadLength;
    }
    u8 block[kQuicSampleBytes] = {};
    if (m_algorithm == AlgorithmId::Chacha20Poly1305)
    {
        // RFC 9001 section 5.4.4: the sample's first 4 bytes are the block counter (little-endian), the rest the nonce.
        const u32 counter = static_cast<u32>(sample[0]) | (static_cast<u32>(sample[1]) << 8U) |
                            (static_cast<u32>(sample[2]) << 16U) | (static_cast<u32>(sample[3]) << 24U);
        CRYPTO_chacha_20(block, block, kQuicMaskBytes, m_state, sample.data() + 4, counter);
    }
    else
    {
        AES_encrypt(sample.data(), block, std::launder(reinterpret_cast<const AES_KEY*>(m_state)));
    }
    std::memcpy(out.data(), block, kQuicMaskBytes);
    secure_zero(block);
    return CryptoError::None;
}

CryptoError QuicHeaderKey::protect(cont::Span<u8> packet, usize packet_number_offset) const noexcept
{
    const usize sample = sample_offset(packet.size(), packet_number_offset);
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    if (sample == 0U)
    {
        return CryptoError::BadLength;
    }
    u8                mask_bytes[kQuicMaskBytes];
    const CryptoError error = mask(packet.subspan(sample, kQuicSampleBytes), mask_bytes);
    if (error != CryptoError::None)
    {
        return error;
    }
    const usize length = (packet[0] & 0x03U) + 1U;
    packet[0] ^= static_cast<u8>(mask_bytes[0] & first_byte_mask(packet[0]));
    for (usize i = 0U; i < length; ++i)
    {
        packet[packet_number_offset + i] ^= mask_bytes[1U + i];
    }
    return CryptoError::None;
}

CryptoError QuicHeaderKey::unprotect(cont::Span<u8> packet, usize packet_number_offset, usize& packet_number_bytes,
                                     u64& truncated) const noexcept
{
    packet_number_bytes = 0U;
    truncated           = 0U;
    const usize sample  = sample_offset(packet.size(), packet_number_offset);
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    if (sample == 0U)
    {
        return CryptoError::BadLength;
    }
    u8                mask_bytes[kQuicMaskBytes];
    const CryptoError error = mask(packet.subspan(sample, kQuicSampleBytes), mask_bytes);
    if (error != CryptoError::None)
    {
        return error;
    }
    packet[0] ^= static_cast<u8>(mask_bytes[0] & first_byte_mask(packet[0]));
    packet_number_bytes = (packet[0] & 0x03U) + 1U;
    for (usize i = 0U; i < packet_number_bytes; ++i)
    {
        packet[packet_number_offset + i] ^= mask_bytes[1U + i];
        truncated = (truncated << 8U) | packet[packet_number_offset + i];
    }
    return CryptoError::None;
}

// ---- Packet numbers ----

usize quic_packet_number_bytes(u64 packet_number, u64 largest_acked, bool acknowledged) noexcept
{
    if (packet_number > kQuicMaxPacketNumber || (acknowledged && packet_number <= largest_acked))
    {
        return 0U;
    }
    // RFC 9000 appendix A.2: the fewest bits that represent twice the unacknowledged range.
    const u64 range = 2U * (acknowledged ? packet_number - largest_acked : packet_number + 1U);
    usize     bits  = 1U;
    while ((u64{1} << bits) < range)
    {
        ++bits;
    }
    const usize bytes = (bits + 7U) / 8U;
    return bytes <= 4U ? bytes : 0U;
}

u64 quic_decode_packet_number(u64 truncated, usize packet_number_bytes, u64 expected) noexcept
{
    if (packet_number_bytes == 0U || packet_number_bytes > 4U)
    {
        return truncated;
    }
    const usize bits    = 8U * packet_number_bytes;
    const u64   window  = u64{1} << bits;
    const u64   half    = window / 2U;
    const u64   mask    = window - 1U;
    const u64   decoded = (expected & ~mask) | (truncated & mask);
    // RFC 9000 appendix A.3, the candidate moved by one window toward `expected`, never below 0 or past 2^62.
    if (decoded + half <= expected && decoded < (u64{1} << 62U) - window)
    {
        return decoded + window;
    }
    if (decoded > expected + half && decoded >= window)
    {
        return decoded - window;
    }
    return decoded;
}

// ---- Retry ----

namespace
{
CryptoError retry_seal(QuicVersion version, cont::ConstSpan<u8> original_dcid, cont::ConstSpan<u8> retry,
                       cont::Span<u8> tag) noexcept
{
    const VersionParameters* parameters = parameters_of(version);
    if (parameters == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (original_dcid.size() > kQuicMaxConnectionIdBytes || retry.size() > kQuicMaxRetryBytes - kQuicTagBytes ||
        tag.size() != kQuicTagBytes)
    {
        return CryptoError::BadLength;
    }
    u8 pseudo[kRetryPseudoBytes];
    pseudo[0] = static_cast<u8>(original_dcid.size());
    std::memcpy(pseudo + 1, original_dcid.data(), original_dcid.size());
    std::memcpy(pseudo + 1 + original_dcid.size(), retry.data(), retry.size());
    AeadKey     key;
    CryptoError error = key.init(AlgorithmId::Aes128Gcm, parameters->retry_key);
    if (error == CryptoError::None)
    {
        error = key.seal(parameters->retry_nonce, cont::ConstSpan<u8>(),
                         cont::ConstSpan<u8>(pseudo, 1U + original_dcid.size() + retry.size()), tag);
    }
    return error;
}
} // namespace

CryptoError quic_retry_tag(QuicVersion version, cont::ConstSpan<u8> original_dcid, cont::ConstSpan<u8> retry,
                           cont::Span<u8> tag) noexcept
{
    const CryptoError error = retry_seal(version, original_dcid, retry, tag);
    if (error != CryptoError::None && tag.size() == kQuicTagBytes)
    {
        secure_zero(tag);
    }
    return error;
}

CryptoError quic_retry_verify(QuicVersion version, cont::ConstSpan<u8> original_dcid,
                              cont::ConstSpan<u8> retry) noexcept
{
    if (retry.size() < kQuicTagBytes || retry.size() > kQuicMaxRetryBytes)
    {
        return CryptoError::BadLength;
    }
    u8                expected[kQuicTagBytes];
    const usize       body  = retry.size() - kQuicTagBytes;
    const CryptoError error = retry_seal(version, original_dcid, retry.first(body), expected);
    if (error != CryptoError::None)
    {
        return error;
    }
    return constant_time_equal(expected, retry.subspan(body)) ? CryptoError::None : CryptoError::Forged;
}
// ---- Address validation tokens ----

namespace
{
using detail::same_bytes;
using provider_internal::label_of;

constexpr char  kTokenNameInfo[]    = "cerid quic token name";
constexpr char  kTokenSealInfo[]    = "cerid quic token seal";
constexpr usize kTokenNameAt        = 1U;
constexpr usize kTokenSaltAt        = kTokenNameAt + kQuicTokenNameBytes;
constexpr usize kTokenTimeBytes     = 8U;
constexpr usize kMaxTokenBodyBytes  = kTokenTimeBytes + 1U + kQuicMaxConnectionIdBytes;
// The clear bytes, the version, an IPv6 address with its length, the port and two connection IDs with their lengths.
constexpr usize kMaxTokenAssociated =
    kQuicTokenHeaderBytes + 4U + 1U + 16U + 2U + 2U * (1U + kQuicMaxConnectionIdBytes);
static_assert(kQuicTokenKeyBytes == provider_internal::kSealingKeyBytes, "a token key is a sealing key");
static_assert(kQuicNewTokenBytes == kQuicTokenHeaderBytes + kTokenTimeBytes + kQuicTagBytes);
static_assert(kQuicTokenSaltBytes == kReplayIdBytes, "a token's salt is its replay filter identifier");

bool address_ok(const QuicTokenPeer& peer) noexcept
{
    return peer.address.size() == 4U || peer.address.size() == 16U;
}

void put_u64(u8* out, u64 value) noexcept
{
    for (usize i = 0U; i < 8U; ++i)
    {
        out[i] = static_cast<u8>(value >> (56U - 8U * i));
    }
}

u64 get_u64(const u8* in) noexcept
{
    u64 value = 0U;
    for (usize i = 0U; i < 8U; ++i)
    {
        value = (value << 8U) | in[i];
    }
    return value;
}

// Writes a connection ID after its length byte; the bytes written.
usize put_connection_id(u8* out, cont::ConstSpan<u8> id) noexcept
{
    out[0] = static_cast<u8>(id.size());
    if (!id.empty())
    {
        std::memcpy(out + 1, id.data(), id.size());
    }
    return 1U + id.size();
}

// A token's associated data: its clear bytes, the version and the client's address; for a Retry token also the port
// and the client's next Initial's connection IDs (`dcid`, the Retry's Source Connection ID, and `scid`, the client's).
usize token_associated(cont::ConstSpan<u8> header, QuicVersion version, QuicTokenKind kind, const QuicTokenPeer& peer,
                       cont::ConstSpan<u8> dcid, cont::ConstSpan<u8> scid, u8 (&out)[kMaxTokenAssociated]) noexcept
{
    usize n = kQuicTokenHeaderBytes;
    std::memcpy(out, header.data(), kQuicTokenHeaderBytes);
    const u32 wire = static_cast<u32>(version);
    for (usize i = 0U; i < 4U; ++i)
    {
        out[n++] = static_cast<u8>(wire >> (24U - 8U * i));
    }
    out[n++] = static_cast<u8>(peer.address.size());
    std::memcpy(out + n, peer.address.data(), peer.address.size());
    n += peer.address.size();
    if (kind == QuicTokenKind::Retry)
    {
        out[n++] = static_cast<u8>(peer.port >> 8U);
        out[n++] = static_cast<u8>(peer.port & 0xFFU);
        n += put_connection_id(out + n, dcid);
        n += put_connection_id(out + n, scid);
    }
    return n;
}
} // namespace

QuicTokenKind quic_token_kind(cont::ConstSpan<u8> token) noexcept
{
    if (token.empty())
    {
        return QuicTokenKind::None;
    }
    switch (static_cast<QuicTokenKind>(token[0]))
    {
    case QuicTokenKind::Retry:
        return QuicTokenKind::Retry;
    case QuicTokenKind::NewToken:
        return QuicTokenKind::NewToken;
    default:
        return QuicTokenKind::None;
    }
}

QuicTokenKeys::~QuicTokenKeys() noexcept
{
    clear();
}

CryptoError QuicTokenKeys::init(cont::ConstSpan<u8> key, cont::ConstSpan<u8> previous, u64 new_token_lifetime) noexcept
{
    clear();
    if (key.size() != kQuicTokenKeyBytes || (!previous.empty() && previous.size() != kQuicTokenKeyBytes) ||
        new_token_lifetime == 0U || new_token_lifetime > kQuicMaxNewTokenLifetime)
    {
        return CryptoError::BadLength;
    }
    if (!previous.empty() && constant_time_equal(key, previous))
    {
        return CryptoError::BadKey;
    }
    std::memcpy(m_keys[0], key.data(), kQuicTokenKeyBytes);
    u32 count = 1U;
    if (!previous.empty())
    {
        std::memcpy(m_keys[1], previous.data(), kQuicTokenKeyBytes);
        count = 2U;
    }
    for (u32 i = 0U; i < count; ++i)
    {
        const CryptoError error = provider_internal::sealing_key_name(
            cont::ConstSpan<u8>(m_keys[i]), label_of(kTokenNameInfo), cont::Span<u8>(m_names[i]));
        if (error != CryptoError::None)
        {
            clear();
            return error;
        }
    }
    if (count == 2U && std::memcmp(m_names[0], m_names[1], kQuicTokenNameBytes) == 0)
    {
        clear();
        return CryptoError::BadKey;
    }
    m_count              = count;
    m_new_token_lifetime = new_token_lifetime;
    return CryptoError::None;
}

void QuicTokenKeys::clear() noexcept
{
    secure_zero(cont::Span<u8>(&m_keys[0][0], sizeof(m_keys)));
    secure_zero(cont::Span<u8>(&m_names[0][0], sizeof(m_names)));
    m_count              = 0U;
    m_new_token_lifetime = 0U;
}

CryptoError QuicTokenKeys::mint_retry(QuicVersion version, const QuicTokenPeer& peer, cont::ConstSpan<u8> original_dcid,
                                      cont::ConstSpan<u8> client_scid, cont::ConstSpan<u8> retry_scid, u64 now,
                                      cont::Span<u8> out, usize& written) const noexcept
{
    return mint(version, QuicTokenKind::Retry, peer, original_dcid, client_scid, retry_scid, now, out, written);
}

CryptoError QuicTokenKeys::mint_new_token(QuicVersion version, const QuicTokenPeer& peer, u64 now, cont::Span<u8> out,
                                          usize& written) const noexcept
{
    return mint(version, QuicTokenKind::NewToken, peer, {}, {}, {}, now, out, written);
}

CryptoError QuicTokenKeys::mint(QuicVersion version, QuicTokenKind kind, const QuicTokenPeer& peer,
                                cont::ConstSpan<u8> original_dcid, cont::ConstSpan<u8> client_scid,
                                cont::ConstSpan<u8> retry_scid, u64 now, cont::Span<u8> out,
                                usize& written) const noexcept
{
    written = 0U;
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    if (!quic_version_supported(static_cast<u32>(version)))
    {
        return CryptoError::Unsupported;
    }
    const bool  retry = kind == QuicTokenKind::Retry;
    const usize body  = kTokenTimeBytes + (retry ? 1U + original_dcid.size() : 0U);
    const usize total = kQuicTokenHeaderBytes + body + kQuicTagBytes;
    if (!address_ok(peer) || original_dcid.size() > kQuicMaxConnectionIdBytes ||
        client_scid.size() > kQuicMaxConnectionIdBytes || retry_scid.size() > kQuicMaxConnectionIdBytes ||
        out.size() < total)
    {
        return CryptoError::BadLength;
    }
    if (retry && same_bytes(retry_scid, original_dcid))
    {
        return CryptoError::Overlap;
    }
    u8* const token = out.data();
    token[0]        = static_cast<u8>(kind);
    std::memcpy(token + kTokenNameAt, m_names[0], kQuicTokenNameBytes);
    CryptoError error  = random_bytes(cont::Span<u8>(token + kTokenSaltAt, kQuicTokenSaltBytes));
    u8* const   sealed = token + kQuicTokenHeaderBytes;
    put_u64(sealed, now);
    if (retry)
    {
        static_cast<void>(put_connection_id(sealed + kTokenTimeBytes, original_dcid));
    }
    u8          associated[kMaxTokenAssociated];
    const usize associated_bytes = token_associated(cont::ConstSpan<u8>(token, kQuicTokenHeaderBytes), version, kind,
                                                    peer, retry_scid, client_scid, associated);
    AeadKey     key;
    u8          nonce[kAeadNonceBytes];
    if (error == CryptoError::None)
    {
        error = provider_internal::salted_aead(cont::ConstSpan<u8>(m_keys[0]),
                                               cont::ConstSpan<u8>(token + kTokenSaltAt, kQuicTokenSaltBytes),
                                               label_of(kTokenSealInfo), key, nonce);
    }
    if (error == CryptoError::None)
    {
        error = key.seal(cont::ConstSpan<u8>(nonce), cont::ConstSpan<u8>(sealed, body),
                         cont::ConstSpan<u8>(associated, associated_bytes),
                         cont::Span<u8>(sealed, body + kQuicTagBytes));
    }
    secure_zero(cont::Span<u8>(nonce));
    if (error != CryptoError::None)
    {
        secure_zero(out.first(total));
        return error;
    }
    written = total;
    return CryptoError::None;
}

CryptoError QuicTokenKeys::validate(QuicVersion version, cont::ConstSpan<u8> token, const QuicTokenPeer& peer,
                                    cont::ConstSpan<u8> dcid, cont::ConstSpan<u8> scid, u64 now,
                                    QuicTokenClaims& claims, ReplayFilter* replay) const noexcept
{
    // The filter's slots hold the periods a valid token can come from: its period covers the lifetime and the skew.
    if (!ready() || (replay != nullptr && (!replay->ready() || replay->period() < m_new_token_lifetime ||
                                           replay->period() < kQuicTokenClockSkew)))
    {
        return CryptoError::NotReady;
    }
    if (!quic_version_supported(static_cast<u32>(version)))
    {
        return CryptoError::Unsupported;
    }
    if (token.size() < kQuicNewTokenBytes || token.size() > kQuicMaxTokenBytes || !address_ok(peer) ||
        dcid.size() > kQuicMaxConnectionIdBytes || scid.size() > kQuicMaxConnectionIdBytes)
    {
        return CryptoError::BadLength;
    }
    const QuicTokenKind kind   = quic_token_kind(token);
    const usize         body   = token.size() - kQuicTokenHeaderBytes - kQuicTagBytes;
    const bool          shaped = (kind == QuicTokenKind::NewToken && body == kTokenTimeBytes) ||
                        (kind == QuicTokenKind::Retry && body > kTokenTimeBytes);
    u32 index = 0U;
    while (index < m_count && std::memcmp(token.data() + kTokenNameAt, m_names[index], kQuicTokenNameBytes) != 0)
    {
        ++index;
    }
    if (!shaped || index == m_count)
    {
        return CryptoError::Forged;
    }
    const cont::ConstSpan<u8> salt = token.subspan(kTokenSaltAt, kQuicTokenSaltBytes);
    u8                        associated[kMaxTokenAssociated];
    const usize               associated_bytes =
        token_associated(token.first(kQuicTokenHeaderBytes), version, kind, peer, dcid, scid, associated);
    AeadKey     key;
    u8          nonce[kAeadNonceBytes];
    u8          plain[kMaxTokenBodyBytes];
    CryptoError error = provider_internal::salted_aead(cont::ConstSpan<u8>(m_keys[index]), salt,
                                                       label_of(kTokenSealInfo), key, nonce);
    if (error == CryptoError::None)
    {
        error = key.open(cont::ConstSpan<u8>(nonce), token.subspan(kQuicTokenHeaderBytes),
                         cont::ConstSpan<u8>(associated, associated_bytes), cont::Span<u8>(plain, body));
    }
    secure_zero(cont::Span<u8>(nonce));
    // An authentic Retry body whose length byte disagrees with its length was never sealed by these keys' mint.
    if (error == CryptoError::None && kind == QuicTokenKind::Retry &&
        static_cast<usize>(plain[kTokenTimeBytes]) != body - kTokenTimeBytes - 1U)
    {
        error = CryptoError::Forged;
    }
    const u64 issued   = error == CryptoError::None ? get_u64(plain) : 0U;
    const u64 lifetime = kind == QuicTokenKind::Retry ? kQuicRetryTokenLifetime : m_new_token_lifetime;
    if (error == CryptoError::None &&
        ((issued > now && issued - now > kQuicTokenClockSkew) || (now > issued && now - issued > lifetime)))
    {
        error = CryptoError::Expired;
    }
    if (error == CryptoError::None && kind == QuicTokenKind::NewToken && replay != nullptr &&
        !replay->first_use(salt, issued, now))
    {
        error = CryptoError::Replayed;
    }
    if (error == CryptoError::None)
    {
        claims.kind                = kind;
        claims.issued              = issued;
        claims.original_dcid_bytes = 0U;
        secure_zero(cont::Span<u8>(claims.original_dcid));
        if (kind == QuicTokenKind::Retry)
        {
            claims.original_dcid_bytes = plain[kTokenTimeBytes];
            std::memcpy(claims.original_dcid, plain + kTokenTimeBytes + 1U, claims.original_dcid_bytes);
        }
    }
    secure_zero(cont::Span<u8>(plain));
    return error;
}

// ---- Stateless reset tokens ----

QuicResetKey::~QuicResetKey() noexcept
{
    clear();
}

CryptoError QuicResetKey::init(cont::ConstSpan<u8> key) noexcept
{
    clear();
    if (key.size() != kQuicResetKeyBytes)
    {
        return CryptoError::BadLength;
    }
    std::memcpy(m_key, key.data(), kQuicResetKeyBytes);
    m_ready = true;
    return CryptoError::None;
}

CryptoError QuicResetKey::generate() noexcept
{
    clear();
    const CryptoError error = random_bytes(cont::Span<u8>(m_key));
    m_ready = error == CryptoError::None;
    if (!m_ready)
    {
        clear();
    }
    return error;
}

void QuicResetKey::clear() noexcept
{
    secure_zero(cont::Span<u8>(m_key));
    m_ready = false;
}

CryptoError QuicResetKey::token(cont::ConstSpan<u8> connection_id, cont::Span<u8> out) const noexcept
{
    if (!out.empty())
    {
        secure_zero(out);
    }
    if (out.size() != kQuicResetTokenBytes || connection_id.empty() || connection_id.size() > kQuicMaxConnectionIdBytes)
    {
        return CryptoError::BadLength;
    }
    if (!m_ready)
    {
        return CryptoError::NotReady;
    }
    u8 mac[32];
    const CryptoError error =
        kdf_mac(AlgorithmId::HkdfSha256, cont::ConstSpan<u8>(m_key), connection_id, cont::Span<u8>(mac));
    if (error == CryptoError::None)
    {
        std::memcpy(out.data(), mac, kQuicResetTokenBytes);
    }
    secure_zero(cont::Span<u8>(mac));
    return error;
}
} // namespace crd::crypto
