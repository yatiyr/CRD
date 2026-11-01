// crd-crypto -- QUIC packet protection (NET.2; contract: docs/design/network-foundation.md#quic-protection).
//
// RFC 9001 section 5 and RFC 9369 section 3.3 over the provider interface (HKDF and the AEAD keys of provider.hpp);
// only header protection reaches the provider directly, for its AES block and ChaCha20 stream functions.

#include <crd/crypto/quic.hpp>

#include <openssl/aes.h>
#include <openssl/chacha.h>

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
    constexpr char  kPrefix[]       = "tls13 ";
    constexpr usize kPrefixBytes    = sizeof(kPrefix) - 1U;
    constexpr usize kMaxLabelBytes  = 16U; // "quicv2 key" is the longest label QUIC uses
    const usize     label_bytes     = std::strlen(label);
    u8              info[4U + kPrefixBytes + kMaxLabelBytes];
    if (label_bytes > kMaxLabelBytes || out.size() > 0xFFFFU)
    {
        return CryptoError::BadLength;
    }
    info[0] = static_cast<u8>(out.size() >> 8U);
    info[1] = static_cast<u8>(out.size());
    info[2] = static_cast<u8>(kPrefixBytes + label_bytes);
    std::memcpy(info + 3, kPrefix, kPrefixBytes);
    std::memcpy(info + 3 + kPrefixBytes, label, label_bytes);
    info[3U + kPrefixBytes + label_bytes] = 0U; // an empty context
    return hkdf_expand(hkdf, secret, cont::ConstSpan<u8>(info, 4U + kPrefixBytes + label_bytes), out);
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
    const AlgorithmId aead = current.algorithm();
    const usize       size = current.m_secret_bytes;
    u8                next[kQuicMaxSecretBytes];
    CryptoError       error = expand_label(suite_hash(aead), cont::ConstSpan<u8>(current.m_secret, size),
                                           parameters_of(current.m_version)->ku, cont::Span<u8>(next, size));
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
        m_limit           = packets == 0U ? u64{1} : (packets < ceiling ? packets : ceiling);
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
} // namespace crd::crypto
