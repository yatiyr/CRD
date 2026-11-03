// crd-crypto -- TLS 1.3 sessions over AWS-LC's libssl (NET.2; contract:
// docs/design/network-foundation.md#tls-sessions).
//
// The provider's context and connection objects sit behind the opaque ProviderTlsContext and ProviderTlsSession, and
// the connection reads from and writes to two memory buffers, never a socket. Every configuration is checked against
// the register identifiers before the provider sees it, so a provider refusal past those checks is a provider failure.
// The provider's callbacks (peer verification, the server name, the message observer) reach their session through the
// connection's ex_data slot and touch only the session's own copy of the settings.
//
// Tickets (decision S9) are sealed by the context's ticket AEAD method, not the provider's built-in ticket keys (which
// rotate inside one process and cannot be shared): each ticket is the key's public name, a random 32-byte salt, and its
// issue time, the SHA-256 of its session's early data context and the session state sealed with AES-256-GCM under a
// key and nonce derived from the ticket key and that salt (HKDF-SHA256), so every derived key seals one ticket and no
// count of tickets has to be shared across the servers of a fleet. A client keeps each ticket as Cerid's ticket record:
// a format version, the early data its NewSessionTicket allows and the server's QUIC transport parameters before the
// provider's session.
//
// Early data (decision S17, RFC 9846 section 2.3) is the provider's 0-RTT. A server that accepts it opens each ticket
// once: the first 16 bytes of the ticket's salt are its identifier in the context's ReplayFilter, filed by the sealed
// issue time, and a ticket the filter has seen resumes nothing. A records server's tickets also seal the SHA-256 of
// its early data context (decision S18): a ticket of another context resumes with early data turned off for that
// handshake, which settle_early() turns back on before the connection issues its own tickets (over QUIC the provider
// compares the context itself, and the sealed digest is zero). Early bytes reach a server's caller through
// read_early(), which peeks before it reads: bytes the provider returns while the handshake still runs are early ones,
// and a peek that completed the handshake leaves the first 1-RTT bytes pending for read().
//
// Certificate compression (decision S10, RFC 8879) runs through the engine's codecs (crd-compression): a server's
// compressed Certificate messages are made once at init() and held in the provider context's ex_data, so its sessions
// copy bytes instead of compressing per handshake; a client decodes into a buffer of exactly the claimed length.
//
// Encrypted Client Hello (decision S14, RFC 9849) is the provider's: a server context holds its ECH keys, a client's
// connection its ECHConfigList. What Cerid adds lives in the provider context's ex_data (EchContext): a client's list
// and a server's public names, which the name check reads during handshakes that may outlive the TlsContext. A client
// given a list proves its ClientHello encrypted an inner one (the provider reports it to the message observer) before
// any byte leaves, so a list the provider cannot use never becomes a hello that reveals the server name. Under a policy
// store (decision S21) a client hands the provider only the configurations whose every suite it could pick the
// generation allows, and a server whose keys publish a refused algorithm runs no session.
//
// RFC 9850 key logging (decision S19, threat T22) is the provider's key log callback, forwarded to the context's sink.
// It exists only where CRD_TLS_KEY_LOG (this module's CMakeLists.txt) is 1: a shipping build sets it to 0, and the
// provider's key log functions are then poisoned below, so any use of them there fails to compile.
#include <crd/crypto/tls.hpp>

#include "provider_internal.hpp"

#include <crd/compression/deflate.hpp>
#include <crd/compression/zstd.hpp>
#include <crd/crypto/hpke.hpp>
#include <crd/crypto/key_store.hpp>
#include <crd/crypto/replay.hpp>

#include <openssl/aead.h>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/bytestring.h>
#include <openssl/evp.h>
#include <openssl/hpke.h>
#include <openssl/mem.h>
#include <openssl/nid.h>
#include <openssl/obj.h>
#include <openssl/pool.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <iterator>
#include <new>
#include <utility>

#if !defined(CRD_TLS_KEY_LOG)
#error "CRD_TLS_KEY_LOG comes from engine/foundation/crypto/CMakeLists.txt"
#elif !CRD_TLS_KEY_LOG && (defined(__clang__) || defined(__GNUC__)) // clang-cl too: it defines __clang__
#pragma GCC poison SSL_CTX_set_keylog_callback SSL_CTX_get_keylog_callback
#elif !CRD_TLS_KEY_LOG && defined(_MSC_VER)
#pragma deprecated(SSL_CTX_set_keylog_callback, SSL_CTX_get_keylog_callback) // C4995, an error under /WX
#endif

namespace crd::crypto
{
using provider_internal::ErrorScope;
using provider_internal::key_of;
using provider_internal::label_of;
using provider_internal::pkey_of;

namespace
{
struct Codepoint
{
    AlgorithmId id;
    u16         value;
};

// The register's TLS Supported Groups values: the hybrid groups first, as the defaults prefer them.
constexpr Codepoint kGroups[] = {
    {AlgorithmId::X25519Mlkem768, iana::kGroupX25519mlkem768},
    {AlgorithmId::Secp256r1Mlkem768, iana::kGroupSecp256r1mlkem768},
    {AlgorithmId::Secp384r1Mlkem1024, iana::kGroupSecp384r1mlkem1024},
    {AlgorithmId::X25519, iana::kGroupX25519},
    {AlgorithmId::Secp256r1, iana::kGroupSecp256r1},
    {AlgorithmId::Secp384r1, iana::kGroupSecp384r1},
};

// The register's TLS SignatureScheme values.
constexpr Codepoint kSignatureSchemes[] = {
    {AlgorithmId::MlDsa44, iana::kSignatureSchemeMldsa44},
    {AlgorithmId::MlDsa65, iana::kSignatureSchemeMldsa65},
    {AlgorithmId::MlDsa87, iana::kSignatureSchemeMldsa87},
    {AlgorithmId::Ed25519, iana::kSignatureSchemeEd25519},
    {AlgorithmId::EcdsaSecp256r1Sha256, iana::kSignatureSchemeEcdsaSecp256r1Sha256},
    {AlgorithmId::EcdsaSecp384r1Sha384, iana::kSignatureSchemeEcdsaSecp384r1Sha384},
};

// The TLS 1.3 cipher suite of each AEAD (RFC 9846 appendix B.4) and the provider's identifier of its cipher.
struct CipherSuite
{
    AlgorithmId id;
    const char* name;
    int         nid;
};

constexpr CipherSuite kCipherSuites[] = {
    {AlgorithmId::Aes128Gcm, "TLS_AES_128_GCM_SHA256", NID_aes_128_gcm},
    {AlgorithmId::Aes256Gcm, "TLS_AES_256_GCM_SHA384", NID_aes_256_gcm},
    {AlgorithmId::Chacha20Poly1305, "TLS_CHACHA20_POLY1305_SHA256", NID_chacha20_poly1305},
};

// The random of a HelloRetryRequest, SHA-256("HelloRetryRequest") (RFC 9846 section 4.1.3).
constexpr u8 kHelloRetryRandom[32] = {
    0xCFU, 0x21U, 0xADU, 0x74U, 0xE5U, 0x9AU, 0x61U, 0x11U, 0xBEU, 0x1DU, 0x8CU, 0x02U, 0x1EU, 0x65U, 0xB8U, 0x91U,
    0xC2U, 0xA2U, 0x11U, 0x16U, 0x7AU, 0xBBU, 0x8CU, 0x5EU, 0x07U, 0x9EU, 0x09U, 0xE2U, 0xC8U, 0xA8U, 0x33U, 0x9CU,
};

constexpr u8    kBadCertificate        = SSL_AD_BAD_CERTIFICATE; // a client's refusal of an unpinned server key
constexpr u8    kServerHello           = 2U;      // HandshakeType server_hello
constexpr u8    kNewSessionTicket      = 4U;      // HandshakeType new_session_ticket
constexpr u8    kKeyUpdate             = 24U;     // HandshakeType key_update
constexpr u8    kCompressedCertificate = 25U;     // HandshakeType compressed_certificate (RFC 8879)
constexpr usize kCompressionAlgorithmAt = 4U;     // type (1), length (3), then the algorithm (2)
constexpr usize kKeyUpdateRequestAt    = 4U;      // type (1), length (3), then request_update
constexpr usize kServerHelloRandomAt   = 6U;      // type (1), length (3), legacy_version (2)
constexpr usize kMaxRecordPlaintext    = 16384U;  // 2^14 (RFC 9846 section 5.1)
constexpr usize kRecordOverhead        = 22U;     // header (5), inner content type (1) and AEAD tag (16)
constexpr usize kKeyUpdateRecordBytes  = 27U;     // the overhead, a handshake header (4) and request_update (1)
constexpr usize kMaxSignaturePublicKey = 2592U;   // ML-DSA-87's, the longest signature public key
constexpr usize kMaxCommonName         = 64U;     // ub-common-name (RFC 5280 appendix A)
constexpr usize kTicketNameBytes       = 16U;     // a ticket key's public name, which leads its tickets
constexpr usize kTicketSaltBytes       = 32U;     // a ticket's own random salt
constexpr usize kTicketIssuedBytes     = 8U;      // a ticket's sealed issue time, in seconds since 1970
constexpr usize kTicketContextBytes    = kSha256Bytes; // and its session's early data context's SHA-256
constexpr usize kTicketSealedHead      = kTicketIssuedBytes + kTicketContextBytes;
constexpr usize kTicketHeaderBytes     = kTicketNameBytes + kTicketSaltBytes;
constexpr usize kTicketOverhead        = kTicketHeaderBytes + kTicketSealedHead + kAeadTagBytes;
constexpr u8    kTicketRecordVersion   = 1U;      // the format of a client's ticket record
constexpr usize kTicketRecordHeader    = 7U;      // version (1), early data (4), parameters length (2)
constexpr usize kCertificateEntryOverhead = 9U;   // request context (1), list (3), entry (3), extensions (2)
constexpr usize kMinCertificateBytes   = 4U;      // an empty request context and an empty list
constexpr usize kCompressedOverhead    = 8U;      // algorithm (2), uncompressed_length (3), compressed length (3)
constexpr usize kZlibFrameBytes        = 6U;      // the zlib header (2) and Adler-32 (4) around raw DEFLATE
constexpr int   kCertificateZstdLevel  = 22;      // zstd's highest: a server compresses its Certificate once

constexpr char kTicketNameInfo[] = "cerid tls 1.3 ticket name";
constexpr char kTicketSealInfo[] = "cerid tls 1.3 ticket seal";
constexpr char kBindingLabel[]   = "cerid tls 1.3 binding";
constexpr char kQuicBindingLabel[] = "cerid quic binding";
static_assert(kTlsTicketKeyBytes == provider_internal::kSealingKeyBytes, "a ticket key is a sealing key");

// floor(2^24.5): the full-size records one AES-GCM key may protect (RFC 9846 section 5.5).
constexpr u64 kAesGcmRecordLimit = 23726566U;

SSL_CTX* ctx_of(ProviderTlsContext* context) noexcept
{
    return reinterpret_cast<SSL_CTX*>(context);
}

SSL* ssl_of(ProviderTlsSession* session) noexcept
{
    return reinterpret_cast<SSL*>(session);
}

X509* x509_of(ProviderCertificate* certificate) noexcept
{
    return reinterpret_cast<X509*>(certificate);
}

BIO* bio_of(ProviderBuffer* buffer) noexcept
{
    return reinterpret_cast<BIO*>(buffer);
}

EVP_HPKE_KEY* hpke_of(ProviderEchKey* key) noexcept
{
    return reinterpret_cast<EVP_HPKE_KEY*>(key);
}

// The provider's encryption level of a QUIC level, and the index of a provider level among the levels that carry
// CRYPTO data (kQuicLevels for the early data level, which carries none).
ssl_encryption_level_t encryption_level_of(QuicLevel level) noexcept
{
    switch (level)
    {
    case QuicLevel::Initial:
        return ssl_encryption_initial;
    case QuicLevel::Handshake:
        return ssl_encryption_handshake;
    case QuicLevel::Early:
        return ssl_encryption_early_data;
    case QuicLevel::Application:
        break;
    }
    return ssl_encryption_application;
}

usize quic_index_of(ssl_encryption_level_t level) noexcept
{
    switch (level)
    {
    case ssl_encryption_initial:
        return static_cast<usize>(QuicLevel::Initial);
    case ssl_encryption_handshake:
        return static_cast<usize>(QuicLevel::Handshake);
    case ssl_encryption_application:
        return static_cast<usize>(QuicLevel::Application);
    case ssl_encryption_early_data:
        break;
    }
    return kQuicLevels;
}

u16 codepoint_of(const Codepoint (&table)[6], AlgorithmId algorithm) noexcept
{
    for (const Codepoint& entry : table)
    {
        if (entry.id == algorithm)
        {
            return entry.value;
        }
    }
    return 0U;
}

AlgorithmId algorithm_of(const Codepoint (&table)[6], u16 value) noexcept
{
    for (const Codepoint& entry : table)
    {
        if (entry.value == value)
        {
            return entry.id;
        }
    }
    return AlgorithmId{};
}

const CipherSuite* cipher_suite_of(AlgorithmId algorithm) noexcept
{
    for (const CipherSuite& suite : kCipherSuites)
    {
        if (suite.id == algorithm)
        {
            return &suite;
        }
    }
    return nullptr;
}

// The AEAD of a negotiated cipher suite; AlgorithmId{} for none or another.
AlgorithmId aead_of(const SSL_CIPHER* cipher) noexcept
{
    for (const CipherSuite& suite : kCipherSuites)
    {
        if (cipher != nullptr && SSL_CIPHER_get_cipher_nid(cipher) == suite.nid)
        {
            return suite.id;
        }
    }
    return AlgorithmId{};
}

// Where `name` starts in an ALPN wire list (each name after its length byte); null when it is not listed. A list that
// runs past its end lists nothing more.
const u8* find_protocol(cont::ConstSpan<u8> list, cont::ConstSpan<u8> name) noexcept
{
    usize at = 0U;
    while (at < list.size() && list[at] <= list.size() - at - 1U)
    {
        const usize length = list[at];
        if (length == name.size() && std::memcmp(list.data() + at + 1U, name.data(), length) == 0)
        {
            return list.data() + at + 1U;
        }
        at += 1U + length;
    }
    return nullptr;
}

// The application protocols in ALPN's wire form: each 1 to 255 bytes, listed once, at most kTlsMaxProtocols and
// kTlsMaxProtocolListBytes in all. False, with nothing kept, otherwise.
bool protocols_of(cont::ConstSpan<cont::StringView> protocols, TlsSettings& settings) noexcept
{
    if (protocols.size() > kTlsMaxProtocols)
    {
        return false;
    }
    usize length = 0U;
    for (const cont::StringView protocol : protocols)
    {
        const cont::ConstSpan<u8> name(reinterpret_cast<const u8*>(protocol.data()), protocol.size());
        if (protocol.empty() || protocol.size() > 255U || 1U + protocol.size() > kTlsMaxProtocolListBytes - length ||
            find_protocol(cont::ConstSpan<u8>(settings.protocols, length), name) != nullptr)
        {
            settings.protocol_bytes = 0U;
            return false;
        }
        settings.protocols[length] = static_cast<u8>(protocol.size());
        std::memcpy(settings.protocols + length + 1U, protocol.data(), protocol.size());
        length += 1U + protocol.size();
    }
    settings.protocol_bytes = static_cast<u32>(length);
    return true;
}

// A host name as the server_name extension carries it (RFC 6066 section 3, RFC 1035 section 2.3.1): dot-separated
// labels of 1 to 63 letters, digits and hyphens, neither starting nor ending with a hyphen, at most 253 bytes, no
// trailing dot.
bool valid_host_name(cont::StringView name) noexcept
{
    if (name.empty() || name.size() > kTlsMaxNameBytes)
    {
        return false;
    }
    usize label = 0U;
    for (usize i = 0U; i < name.size(); ++i)
    {
        const char c = name[i];
        if (c == '.')
        {
            if (label == 0U || name[i - 1U] == '-')
            {
                return false;
            }
            label = 0U;
            continue;
        }
        const bool letter_or_digit = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!letter_or_digit && (c != '-' || label == 0U))
        {
            return false;
        }
        if (++label > 63U)
        {
            return false;
        }
    }
    return name.back() != '-' && name.back() != '.';
}

bool same_host_name(cont::StringView a, cont::StringView b) noexcept
{
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [&lower](char x, char y) { return lower(x) == lower(y); });
}

// An ECH public name (RFC 9849 section 6.1.7): a host name whose last label is neither all digits nor "0x" or "0X"
// and hexadecimal digits, which an IPv4 parser would read as an address.
bool valid_public_name(cont::StringView name) noexcept
{
    if (!valid_host_name(name))
    {
        return false;
    }
    usize start = name.size();
    while (start != 0U && name[start - 1U] != '.')
    {
        --start;
    }
    const cont::StringView last(name.data() + start, name.size() - start);
    const auto             digits = [](cont::StringView text, bool hex) {
        return std::all_of(text.begin(), text.end(), [hex](char c) {
            return (c >= '0' && c <= '9') || (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
        });
    };
    const bool hex_literal = last.size() >= 2U && last[0] == '0' && (last[1] == 'x' || last[1] == 'X') &&
                             digits(cont::StringView(last.data() + 2U, last.size() - 2U), true);
    return !digits(last, false) && !hex_literal;
}

// Walks an ECHConfigList (RFC 9849 section 4): a 2-byte length holding the whole rest, then ECHConfigs of a 2-byte
// version and a 2-byte length each, tiling it exactly. False for any other shape; `current` tells whether a
// configuration has version 0xfe0d (the only one defined; the others a client skips).
bool walk_ech_configs(cont::ConstSpan<u8> list, bool& current) noexcept
{
    current = false;
    CBS cbs;
    CBS configs;
    CBS_init(&cbs, list.data(), list.size());
    if (!CBS_get_u16_length_prefixed(&cbs, &configs) || CBS_len(&cbs) != 0U || CBS_len(&configs) == 0U)
    {
        return false;
    }
    while (CBS_len(&configs) != 0U)
    {
        uint16_t version = 0U;
        CBS      contents;
        if (!CBS_get_u16(&configs, &version) || !CBS_get_u16_length_prefixed(&configs, &contents))
        {
            return false;
        }
        current = current || version == iana::kTlsExtensionEncryptedClientHello; // RFC 9849 section 4
    }
    return true;
}

// Decision S21: the status under `snapshot` of one ECHConfig's contents for a client. Refused unless the provider can
// use it, with DHKEM(X25519, HKDF-SHA256) (the only KEM its ECH takes) and a cipher suite of HKDF-SHA256 and an AEAD it
// implements; otherwise the combined status of every such suite, since the provider, not Cerid, picks among them. A
// suite it skips (another KDF or AEAD) is never picked and counts for nothing.
AlgorithmStatus ech_config_status(CBS contents, const PolicySnapshot& snapshot, u64 now, PolicyScope scope) noexcept
{
    uint8_t  config_id = 0U;
    uint16_t kem       = 0U;
    CBS      public_key;
    CBS      suites;
    if (!CBS_get_u8(&contents, &config_id) || !CBS_get_u16(&contents, &kem) ||
        !CBS_get_u16_length_prefixed(&contents, &public_key) || !CBS_get_u16_length_prefixed(&contents, &suites) ||
        kem != iana::kHpkeKemDhkemX25519HkdfSha256)
    {
        return AlgorithmStatus::Refused;
    }
    AlgorithmStatus combined = AlgorithmStatus::Refused;
    bool            usable   = false;
    while (CBS_len(&suites) != 0U)
    {
        uint16_t kdf  = 0U;
        uint16_t aead = 0U;
        if (!CBS_get_u16(&suites, &kdf) || !CBS_get_u16(&suites, &aead))
        {
            return AlgorithmStatus::Refused;
        }
        HpkeSuite suite;
        if (kdf != iana::kHpkeKdfHkdfSha256 || !hpke_suite_of({kem, kdf, aead}, suite))
        {
            continue;
        }
        const AlgorithmId     algorithms[] = {suite.kem, suite.kdf, suite.aead};
        const AlgorithmStatus status       = snapshot.status(algorithms, now, scope);
        combined                           = usable ? std::min(combined, status) : status;
        usable                             = true;
    }
    return combined;
}

// Decision S21: writes into `out` (at least list.size() bytes) the ECHConfigList of the configurations of `list` (one
// walk_ech_configs accepts) that a client may offer under `snapshot` at `now` in `scope`: version 0xfe0d, not refused,
// the preferred ones first, each kind in the list's order. Returns its length, 0 when none is left.
usize allowed_ech_configs(cont::ConstSpan<u8> list, const PolicySnapshot& snapshot, u64 now, PolicyScope scope,
                          cont::Span<u8> out) noexcept
{
    usize written = 2U;
    for (const AlgorithmStatus wanted : {AlgorithmStatus::Preferred, AlgorithmStatus::Accepted})
    {
        CBS cbs;
        CBS configs;
        CBS_init(&cbs, list.data(), list.size());
        if (!CBS_get_u16_length_prefixed(&cbs, &configs))
        {
            return 0U;
        }
        while (CBS_len(&configs) != 0U)
        {
            const uint8_t* start   = CBS_data(&configs);
            uint16_t       version = 0U;
            CBS            contents;
            if (!CBS_get_u16(&configs, &version) || !CBS_get_u16_length_prefixed(&configs, &contents))
            {
                return 0U;
            }
            const auto bytes = static_cast<usize>(CBS_data(&configs) - start);
            if (version == iana::kTlsExtensionEncryptedClientHello &&
                ech_config_status(contents, snapshot, now, scope) == wanted)
            {
                std::memcpy(out.data() + written, start, bytes);
                written += bytes;
            }
        }
    }
    if (written == 2U)
    {
        return 0U;
    }
    out[0] = static_cast<u8>((written - 2U) >> 8U);
    out[1] = static_cast<u8>(written - 2U);
    return written;
}

// Each algorithm of `list` once, of one of the two classes, implemented by the provider; at most `bound` of them.
bool valid_list(cont::ConstSpan<AlgorithmId> list, usize bound, AlgorithmClass first, AlgorithmClass second) noexcept
{
    if (list.size() > bound)
    {
        return false;
    }
    for (usize i = 0U; i < list.size(); ++i)
    {
        const AlgorithmClass kind = algorithm_class(list[i]);
        if ((kind != first && kind != second) || !provider_implements(list[i]) ||
            std::find(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(i), list[i]) !=
                list.begin() + static_cast<std::ptrdiff_t>(i))
        {
            return false;
        }
    }
    return true;
}

// A ticket key's public name, which leads its tickets.
CryptoError ticket_key_name(cont::ConstSpan<u8> key, cont::Span<u8> name) noexcept
{
    return provider_internal::sealing_key_name(key, label_of(kTicketNameInfo), name);
}

// The AES-256-GCM key and nonce that seal one ticket: derived from the ticket key and the ticket's own salt.
CryptoError ticket_aead(cont::ConstSpan<u8> key, cont::ConstSpan<u8> salt, AeadKey& aead,
                        u8 (&nonce)[kAeadNonceBytes]) noexcept
{
    return provider_internal::salted_aead(key, salt, label_of(kTicketSealInfo), aead, nonce);
}

// What a context's tickets are bound to (TlsBinding): the label, the role, the server name in lower case, the own key
// (zero without a credential), then the pins and the accepted signature algorithms as sets (sorted), each after its
// count. It is also the provider's session ID context, which both ends check before they resume a ticket.
TlsBinding binding_of(const TlsConfig& config, const KeyId& own) noexcept
{
    KeyId pins[kTlsMaxPins];
    std::copy(config.pinned.begin(), config.pinned.end(), pins);
    std::sort(pins, pins + config.pinned.size(),
              [](const KeyId& a, const KeyId& b) { return std::memcmp(a.bytes, b.bytes, sizeof(a.bytes)) < 0; });
    u16 signatures[kTlsMaxSignatures];
    for (usize i = 0U; i < config.signatures.size(); ++i)
    {
        signatures[i] = static_cast<u16>(config.signatures[i]);
    }
    std::sort(signatures, signatures + config.signatures.size());

    u8    buffer[sizeof(kBindingLabel) + 2U + kTlsMaxNameBytes + kKeyIdBytes * (1U + kTlsMaxPins) + 1U +
                 2U * kTlsMaxSignatures];
    usize at = sizeof(kBindingLabel) - 1U;
    std::memcpy(buffer, kBindingLabel, at);
    buffer[at++] = static_cast<u8>(config.role);
    buffer[at++] = static_cast<u8>(config.server_name.size());
    for (const char c : config.server_name)
    {
        buffer[at++] = static_cast<u8>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    std::memcpy(buffer + at, own.bytes, kKeyIdBytes);
    at += kKeyIdBytes;
    buffer[at++] = static_cast<u8>(config.pinned.size());
    for (usize i = 0U; i < config.pinned.size(); ++i)
    {
        std::memcpy(buffer + at, pins[i].bytes, kKeyIdBytes);
        at += kKeyIdBytes;
    }
    buffer[at++] = static_cast<u8>(config.signatures.size());
    for (usize i = 0U; i < config.signatures.size(); ++i)
    {
        buffer[at++] = static_cast<u8>(signatures[i] >> 8U);
        buffer[at++] = static_cast<u8>(signatures[i]);
    }
    const KeyId digest = key_id_of(cont::ConstSpan<u8>(buffer, at));
    TlsBinding  binding;
    std::memcpy(binding.bytes, digest.bytes, sizeof(binding.bytes));
    return binding;
}

// A QUIC handshake's binding: SHA-256 of its label and the context's binding, so the tickets of a context's QUIC
// handshakes and of its stream sessions never meet (the provider resumes a ticket only on the transport that issued
// it). Both ends use it as the session ID context, as they use the context's.
TlsBinding quic_binding_of(const TlsBinding& binding) noexcept
{
    u8          buffer[sizeof(kQuicBindingLabel) - 1U + sizeof(binding.bytes)];
    const usize label = sizeof(kQuicBindingLabel) - 1U;
    std::memcpy(buffer, kQuicBindingLabel, label);
    std::memcpy(buffer + label, binding.bytes, sizeof(binding.bytes));
    const KeyId digest = key_id_of(cont::ConstSpan<u8>(buffer));
    TlsBinding  quic;
    std::memcpy(quic.bytes, digest.bytes, sizeof(quic.bytes));
    return quic;
}

void zero_ticket_keys(TlsSettings& settings) noexcept
{
    secure_zero(cont::Span<u8>(&settings.ticket_keys[0][0], sizeof(settings.ticket_keys)));
}

// The wall clock's seconds since 1970, which the provider's ticket ages read too.
u64 unix_now() noexcept
{
    const std::time_t now = std::time(nullptr);
    return now > 0 ? static_cast<u64>(now) : 0U;
}

// The provider's TLS 1.3 cipher suite list of `aeads`, each a known AEAD:
// "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256" at most (75 bytes).
struct SuiteList
{
    char text[96] = {};
};

bool suite_list_of(const AlgorithmId* aeads, usize count, SuiteList& out) noexcept
{
    usize length = 0U;
    for (usize i = 0U; i < count; ++i)
    {
        const CipherSuite* suite = cipher_suite_of(aeads[i]);
        if (suite == nullptr)
        {
            return false;
        }
        const cont::StringView name = suite->name;
        if (length != 0U)
        {
            out.text[length++] = ':';
        }
        std::memcpy(out.text + length, name.data(), name.size());
        length += name.size();
    }
    out.text[length] = '\0';
    return true;
}

constexpr usize kMaxPolicyList = std::max({kTlsMaxGroups, kTlsMaxAeads, kTlsMaxSignatures});

// Decision S20: keeps the algorithms of list[0, count) that `snapshot` does not refuse at `now` in `scope`, the
// preferred ones first, each kind in the configured order; returns how many are left.
u32 keep_allowed(const PolicySnapshot& snapshot, u64 now, PolicyScope scope, AlgorithmId* list, u32 count) noexcept
{
    AlgorithmId kept[kMaxPolicyList];
    u32         left = 0U;
    for (const AlgorithmStatus wanted : {AlgorithmStatus::Preferred, AlgorithmStatus::Accepted})
    {
        for (u32 i = 0U; i < count; ++i)
        {
            if (snapshot.status(list[i], now, scope) == wanted)
            {
                kept[left++] = list[i];
            }
        }
    }
    std::copy(kept, kept + left, list);
    return left;
}

// What a client's ticket record holds besides its format version (TlsTicketStore).
struct TicketRecord
{
    u32 early = 0U;                 // the early data bytes the ticket allows
    cont::ConstSpan<u8> parameters; // the server's QUIC transport parameters; empty over records
    cont::ConstSpan<u8> session;    // the provider's session
};

// Reads a ticket record; false for any other bytes.
bool read_ticket_record(cont::ConstSpan<u8> bytes, TicketRecord& record) noexcept
{
    CBS in;
    CBS parameters;
    uint8_t version = 0U;
    uint32_t early = 0U;
    CBS_init(&in, bytes.data(), bytes.size());
    if (!CBS_get_u8(&in, &version) || version != kTicketRecordVersion || !CBS_get_u32(&in, &early) ||
        !CBS_get_u16_length_prefixed(&in, &parameters) || CBS_len(&parameters) > kQuicMaxTransportParameterBytes ||
        CBS_len(&in) == 0U)
    {
        return false;
    }
    record.early = early;
    record.parameters = cont::ConstSpan<u8>(CBS_data(&parameters), CBS_len(&parameters));
    record.session = cont::ConstSpan<u8>(CBS_data(&in), CBS_len(&in));
    return true;
}

// The early data a NewSessionTicket message allows (its early_data extension, RFC 9846 section 4.6.1); 0 without one
// and for a message that does not parse (the provider then refuses it).
u32 ticket_early_data(cont::ConstSpan<u8> message) noexcept
{
    CBS in;
    CBS body;
    CBS skipped;
    CBS extensions;
    uint8_t type = 0U;
    uint32_t lifetime = 0U;
    uint32_t age_add = 0U;
    CBS_init(&in, message.data(), message.size());
    if (!CBS_get_u8(&in, &type) || type != kNewSessionTicket || !CBS_get_u24_length_prefixed(&in, &body) ||
        !CBS_get_u32(&body, &lifetime) || !CBS_get_u32(&body, &age_add) ||
        !CBS_get_u8_length_prefixed(&body, &skipped) || !CBS_get_u16_length_prefixed(&body, &skipped) ||
        !CBS_get_u16_length_prefixed(&body, &extensions))
    {
        return 0U;
    }
    while (CBS_len(&extensions) != 0U)
    {
        uint16_t extension = 0U;
        CBS data;
        uint32_t early = 0U;
        if (!CBS_get_u16(&extensions, &extension) || !CBS_get_u16_length_prefixed(&extensions, &data))
        {
            return 0U;
        }
        if (extension == TLSEXT_TYPE_early_data)
        {
            return CBS_get_u32(&data, &early) && CBS_len(&data) == 0U ? early : 0U;
        }
    }
    return 0U;
}

// Offers the ticket of a record the store handed back when its session parses as a TLS 1.3 session of `binding` (the
// provider then also drops one that expired); any other is dropped and the handshake runs in full. `offered` keeps
// the record's parts (pointing into `ticket`) when it is offered. False only when the provider fails.
bool offer_ticket(SSL* ssl, const TlsBinding& binding, cont::ConstSpan<u8> ticket, TicketRecord& offered) noexcept
{
    TicketRecord record;
    if (ticket.empty() || !read_ticket_record(ticket, record))
    {
        return true;
    }
    const bssl::UniquePtr<SSL_SESSION> session(
        SSL_SESSION_from_bytes(record.session.data(), record.session.size(), SSL_get_SSL_CTX(ssl)));
    unsigned       length  = 0U;
    const uint8_t* context = session ? SSL_SESSION_get0_id_context(session.get(), &length) : nullptr;
    if (!session || SSL_SESSION_get_protocol_version(session.get()) != TLS1_3_VERSION ||
        length != sizeof(binding.bytes) || std::memcmp(context, binding.bytes, length) != 0)
    {
        return true;
    }
    if (SSL_set_session(ssl, session.get()) != 1)
    {
        return false;
    }
    offered = record;
    return true;
}

// Zeroes a local copy of the ticket keys on every way out of its scope.
class TicketKeysGuard
{
public:
    explicit TicketKeysGuard(TlsSettings& settings) noexcept : m_settings(settings) {}
    ~TicketKeysGuard() noexcept { zero_ticket_keys(m_settings); }
    TicketKeysGuard(const TicketKeysGuard&)            = delete;
    TicketKeysGuard& operator=(const TicketKeysGuard&) = delete;
    TicketKeysGuard(TicketKeysGuard&&)                 = delete;
    TicketKeysGuard& operator=(TicketKeysGuard&&)      = delete;

private:
    TlsSettings& m_settings;
};

// A server's ticket keys: its configured key or one drawn now (which only this context then opens), and the key before
// its last rotation, each with its name. False when the provider fails.
bool ticket_keys_of(const TlsConfig& config, TlsSettings& settings) noexcept
{
    if (config.ticket_key.empty())
    {
        if (random_bytes(cont::Span<u8>(settings.ticket_keys[0])) != CryptoError::None)
        {
            return false;
        }
    }
    else
    {
        std::copy(config.ticket_key.begin(), config.ticket_key.end(), settings.ticket_keys[0]);
    }
    std::copy(config.previous_ticket_key.begin(), config.previous_ticket_key.end(), settings.ticket_keys[1]);
    settings.ticket_key_count = config.previous_ticket_key.empty() ? 1U : 2U;
    for (u32 i = 0U; i < settings.ticket_key_count; ++i)
    {
        if (ticket_key_name(cont::ConstSpan<u8>(settings.ticket_keys[i]), cont::Span<u8>(settings.ticket_names[i])) !=
            CryptoError::None)
        {
            return false;
        }
    }
    return true;
}

// The connection's slot for its session: one index for the process, taken on first use.
int session_slot() noexcept
{
    static const int kSlot = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return kSlot;
}

// RFC 8879 algorithms Cerid decodes and compresses with, each at most once.
bool valid_compression(cont::ConstSpan<u16> algorithms) noexcept
{
    const u16* known_end = std::end(kTlsCertificateCompression);
    for (usize i = 0U; i < algorithms.size(); ++i)
    {
        const cont::ConstSpan<u16> earlier = algorithms.first(i);
        if (std::find(std::begin(kTlsCertificateCompression), known_end, algorithms[i]) == known_end ||
            std::find(earlier.begin(), earlier.end(), algorithms[i]) != earlier.end())
        {
            return false;
        }
    }
    return true;
}

// Whether `compressed_bytes` of `algorithm` can decode to a Certificate of `length` bytes: within the limit a plain
// Certificate has (RFC 8879 section 5) and, for zlib, no longer than DEFLATE's 1032:1 can make them, so a claim is
// refused before anything is reserved for it. A zstd block of 3 header bytes already decodes to 128 KiB, past the
// limit, so the limit alone bounds a zstd claim.
bool certificate_claim_fits(u16 algorithm, usize compressed_bytes, usize length) noexcept
{
    if (length < kMinCertificateBytes || length > kTlsMaxCertificateBytes)
    {
        return false;
    }
    return algorithm == iana::kCertCompressionZstd ||
           (algorithm == iana::kCertCompressionZlib && compressed_bytes > kZlibFrameBytes &&
            length <= compression::deflate_max_inflated_size(compressed_bytes - kZlibFrameBytes));
}

// A server context's Certificate (decision S10): the plain message the provider writes for its credential and its
// compressed forms, smallest first, made once at init(). The provider context's ex_data holds it, so it lives exactly
// as long as the provider context its sessions share.
struct CompressedCertificates
{
    CRYPTO_BUFFER* message = nullptr;
    u16            algorithms[std::size(kTlsCertificateCompression)] = {};
    CRYPTO_BUFFER* compressed[std::size(kTlsCertificateCompression)] = {};
    u32            count = 0U;
};

void free_compressed_certificates(void*, void* pointer, CRYPTO_EX_DATA*, int, long, void*) noexcept
{
    auto* certificates = static_cast<CompressedCertificates*>(pointer);
    if (certificates == nullptr)
    {
        return;
    }
    CRYPTO_BUFFER_free(certificates->message);
    for (CRYPTO_BUFFER* compressed : certificates->compressed)
    {
        CRYPTO_BUFFER_free(compressed);
    }
    certificates->~CompressedCertificates();
    OPENSSL_free(certificates);
}

// The provider context's slot for its compressed Certificate, freed with the provider context.
int context_slot() noexcept
{
    static const int kSlot = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, &free_compressed_certificates);
    return kSlot;
}

// A context's ECH data (decision S14): a client's ECHConfigList, or a server's public names. It lives in the provider
// context's ex_data, as long as the provider context its sessions share.
struct EchContext
{
    u8   configs[kTlsMaxEchConfigListBytes] = {};
    u32  config_bytes                       = 0U;
    char names[kTlsMaxEchKeys][kTlsMaxNameBytes + 1U] = {};
    u32  name_bytes[kTlsMaxEchKeys]                   = {};
    u32  name_count                                   = 0U;
    // A server's: every algorithm its ECH keys publish (the KEM, the KDF and each AEAD once), which decision S21
    // checks at each session's start.
    AlgorithmId published[2U + kTlsMaxEchAeads] = {};
    u32         published_count                 = 0U;

    [[nodiscard]] cont::ConstSpan<u8> list() const noexcept { return cont::ConstSpan<u8>(configs, config_bytes); }
    [[nodiscard]] cont::ConstSpan<AlgorithmId> publishes() const noexcept
    {
        return cont::ConstSpan<AlgorithmId>(published, published_count);
    }

    void publish(AlgorithmId algorithm) noexcept
    {
        if (std::find(published, published + published_count, algorithm) == published + published_count)
        {
            published[published_count++] = algorithm;
        }
    }

    [[nodiscard]] bool names_public(cont::StringView name) const noexcept
    {
        for (u32 i = 0U; i < name_count; ++i)
        {
            if (same_host_name(name, cont::StringView(names[i], name_bytes[i])))
            {
                return true;
            }
        }
        return false;
    }
};

void free_ech_context(void*, void* pointer, CRYPTO_EX_DATA*, int, long, void*) noexcept
{
    auto* ech = static_cast<EchContext*>(pointer);
    if (ech != nullptr)
    {
        ech->~EchContext();
        OPENSSL_free(ech);
    }
}

int ech_slot() noexcept
{
    static const int kSlot = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, &free_ech_context);
    return kSlot;
}

const EchContext* ech_of(const SSL* ssl) noexcept
{
    return static_cast<const EchContext*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), ech_slot()));
}
} // namespace

bool detail::tls_decompress_certificate(u16 algorithm, cont::ConstSpan<u8> compressed,
                                        cont::Span<u8> certificate) noexcept
{
    if (!certificate_claim_fits(algorithm, compressed.size(), certificate.size()))
    {
        return false;
    }
    return algorithm == iana::kCertCompressionZstd ? compression::zstd_decompress_exact(compressed, certificate)
                                                   : compression::zlib_inflate(compressed, certificate);
}

// The provider's callbacks, with access to the session they serve.
struct TlsCallbacks
{
    static detail::TlsHandshakeState* session_of(const SSL* ssl) noexcept
    {
        return static_cast<detail::TlsHandshakeState*>(SSL_get_ex_data(ssl, session_slot()));
    }

    // Takes the peer's key from its leaf certificate when the key is of an accepted algorithm and pinned: during a
    // full handshake (verify_peer) and after a resumed one, whose ticket carried the certificate.
    static bool pinned_peer(const SSL* ssl, detail::TlsHandshakeState& session) noexcept
    {
        const STACK_OF(CRYPTO_BUFFER)* chain = SSL_get0_peer_certificates(ssl);
        if (chain == nullptr || sk_CRYPTO_BUFFER_num(chain) == 0U)
        {
            return false;
        }
        const bssl::UniquePtr<X509> leaf(X509_parse_from_buffer(sk_CRYPTO_BUFFER_value(chain, 0U)));
        const EVP_PKEY*             pkey = leaf ? X509_get0_pubkey(leaf.get()) : nullptr;
        const AlgorithmId algorithm = pkey != nullptr ? provider_internal::signature_algorithm_of(pkey) : AlgorithmId{};
        const TlsSettings& settings = session.m_settings;
        const AlgorithmId* accepted_end = settings.signatures + settings.signature_count;
        if (algorithm == AlgorithmId{} || std::find(settings.signatures, accepted_end, algorithm) == accepted_end)
        {
            return false;
        }
        u8                   public_key[kMaxSignaturePublicKey];
        const cont::Span<u8> bytes(public_key, signature_sizes(algorithm).public_key);
        if (provider_internal::signature_public_bytes(pkey, algorithm, bytes) != CryptoError::None)
        {
            return false;
        }
        const KeyId  key      = key_id_of(bytes);
        // A loop, not std::find: MSVC's STL routes a trivially comparable 32-byte element to its vectorized find,
        // which static_asserts on the size under clang-cl.
        const KeyId* pins_end = settings.pinned + settings.pin_count;
        bool         pinned   = false;
        for (const KeyId* pin = settings.pinned; pin != pins_end; ++pin)
        {
            if (*pin == key)
            {
                pinned = true;
                break;
            }
        }
        if (!pinned)
        {
            return false;
        }
        session.m_peer_algorithm = algorithm;
        session.m_peer_key       = key;
        return true;
    }

    // Accepts the peer when its leaf certificate's key is of an accepted algorithm and pinned. The certificate's own
    // signature is not looked at: the handshake's CertificateVerify proves that the peer holds the pinned key.
    static ssl_verify_result_t verify_peer(SSL* ssl, uint8_t* out_alert) noexcept
    {
        detail::TlsHandshakeState* session = session_of(ssl);
        if (session == nullptr)
        {
            *out_alert = SSL_AD_INTERNAL_ERROR;
            return ssl_verify_invalid;
        }
        if (!pinned_peer(ssl, *session))
        {
            // A server does not tell an unauthenticated client whether a key is pinned (the TLS 1.3 profile).
            return refuse(*session, out_alert,
                          session->m_settings.role == TlsRole::Server ? kTlsAlertGeneralError : kBadCertificate);
        }
        return ssl_verify_ok;
    }

    static ssl_verify_result_t refuse(detail::TlsHandshakeState& session, uint8_t* out_alert, u8 alert) noexcept
    {
        session.m_refused = true;
        *out_alert        = alert;
        return ssl_verify_invalid;
    }

    // A server with a name refuses a client that names another server or none, with general_error: the alert does not
    // tell the client which names the server holds. The provider names the inner ClientHello's server when it
    // decrypted one; a hello it did not decrypt may name a public name of the server's ECH keys, which the server
    // answers only so that a rejected client can authenticate its retry configurations (complete() refuses the
    // handshake if it gets that far). A server with application protocols refuses a client that offers none with
    // no_application_protocol (one that offers some meets select_protocol).
    static int check_name(SSL* ssl, int* out_alert, void*) noexcept
    {
        detail::TlsHandshakeState* session = session_of(ssl);
        if (session == nullptr)
        {
            *out_alert = SSL_AD_INTERNAL_ERROR;
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        const TlsSettings& settings = session->m_settings;
        if (settings.protocol_bytes != 0U && !session->m_protocol_offered)
        {
            session->m_refused = true;
            *out_alert         = kTlsAlertNoApplicationProtocol;
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        if (settings.name_bytes == 0U)
        {
            return SSL_TLSEXT_ERR_OK;
        }
        const char*       offered = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
        const EchContext* ech     = ech_of(ssl);
        if (offered != nullptr &&
            same_host_name(cont::StringView(offered), cont::StringView(settings.server_name, settings.name_bytes)))
        {
            return SSL_TLSEXT_ERR_OK;
        }
        if (offered != nullptr && ech != nullptr && SSL_ech_accepted(ssl) != 1 &&
            ech->names_public(cont::StringView(offered)))
        {
            session->m_public_name = true;
            return SSL_TLSEXT_ERR_OK;
        }
        session->m_refused = true;
        *out_alert         = kTlsAlertGeneralError;
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }

    // A client hands each ticket its server issues to the session's store as a ticket record: the early data its
    // NewSessionTicket allowed (the message observer read it just before), the server's QUIC transport parameters and
    // the serialized session, built in one allocation of its exact size so no copy of the secret is left behind.
    // Without a store the ticket is ignored. The provider keeps its session object (0: no reference taken).
    static int keep_ticket(SSL* ssl, SSL_SESSION* ticket) noexcept
    {
        detail::TlsHandshakeState* session = session_of(ssl);
        if (session == nullptr || session->m_tickets == nullptr)
        {
            return 0;
        }
        const u32 early = session->m_ticket_early;
        session->m_ticket_early = 0U;
        const uint8_t* parameters = nullptr;
        size_t parameter_bytes = 0U;
        SSL_get_peer_quic_transport_params(ssl, &parameters, &parameter_bytes);
        uint8_t* state = nullptr;
        size_t state_bytes = 0U;
        if (parameter_bytes > kQuicMaxTransportParameterBytes ||
            SSL_SESSION_to_bytes(ticket, &state, &state_bytes) != 1)
        {
            return 0;
        }
        const usize size = kTicketRecordHeader + parameter_bytes + state_bytes;
        auto* record = static_cast<u8*>(OPENSSL_malloc(size));
        if (record != nullptr)
        {
            record[0] = kTicketRecordVersion;
            for (usize i = 0U; i < 4U; ++i)
            {
                record[1U + i] = static_cast<u8>(early >> (24U - 8U * i));
            }
            record[5] = static_cast<u8>(parameter_bytes >> 8U);
            record[6] = static_cast<u8>(parameter_bytes);
            std::copy(parameters, parameters + parameter_bytes, record + kTicketRecordHeader);
            std::copy(state, state + state_bytes, record + kTicketRecordHeader + parameter_bytes);
            session->m_tickets->keep(session->m_settings.binding, cont::ConstSpan<u8>(record, size));
            secure_zero(cont::Span<u8>(record, size));
            OPENSSL_free(record);
        }
        secure_zero(cont::Span<u8>(state, state_bytes));
        OPENSSL_free(state);
        return 0;
    }

    static size_t ticket_overhead(SSL*) noexcept
    {
        return kTicketOverhead;
    }

    // Seals the issue time, the session's early data context digest and `in` (the session state) under the sealing
    // key: the key's name, the salt, then the ciphertext and tag. The name only selects the key: any other name selects
    // another key or none, and the salt derives the sealing key. `in` may be `out`, so the state moves into place
    // before anything is written.
    static int seal_ticket(SSL* ssl, uint8_t* out, size_t* out_len, size_t max_out_len, const uint8_t* in,
                           size_t in_len) noexcept
    {
        const detail::TlsHandshakeState* session = session_of(ssl);
        if (session == nullptr || session->m_settings.ticket_key_count == 0U || max_out_len < kTicketOverhead ||
            in_len > max_out_len - kTicketOverhead)
        {
            return 0;
        }
        const TlsSettings&        settings = session->m_settings;
        u8* const                 body     = out + kTicketHeaderBytes;
        const usize sealed = kTicketSealedHead + in_len;
        const cont::ConstSpan<u8> salt(out + kTicketNameBytes, kTicketSaltBytes);
        std::memmove(body + kTicketSealedHead, in, in_len);
        const u64 issued = unix_now();
        for (usize i = 0U; i < kTicketIssuedBytes; ++i)
        {
            body[i] = static_cast<u8>(issued >> (56U - 8U * i));
        }
        std::memcpy(body + kTicketIssuedBytes, settings.early_context, kTicketContextBytes);
        std::memcpy(out, settings.ticket_names[0], kTicketNameBytes);
        AeadKey key;
        u8      nonce[kAeadNonceBytes];
        if (random_bytes(cont::Span<u8>(out + kTicketNameBytes, kTicketSaltBytes)) != CryptoError::None ||
            ticket_aead(cont::ConstSpan<u8>(settings.ticket_keys[0]), salt, key, nonce) != CryptoError::None ||
            key.seal(cont::ConstSpan<u8>(nonce), cont::ConstSpan<u8>(body, sealed), {},
                     cont::Span<u8>(body, sealed + kAeadTagBytes)) != CryptoError::None)
        {
            return 0;
        }
        *out_len = in_len + kTicketOverhead;
        return 1;
    }

    // Opens a ticket sealed under the sealing key or the previous one. A ticket too short, under another key or forged
    // is ignored (the handshake runs in full); only a provider failure fails the handshake. A server that accepts early
    // data opens each ticket once (S17): the ticket's identifier, the first kReplayIdBytes of its salt (random, and
    // authenticated by the opening, as the salt derives the key), goes through the ReplayFilter by its sealed issue
    // time, and a ticket the filter refuses is ignored too. A records ticket sealed under another early data context
    // resumes with early data off for this handshake and spends no filter entry (S18).
    static ssl_ticket_aead_result_t open_ticket(SSL* ssl, uint8_t* out, size_t* out_len, size_t max_out_len,
                                                const uint8_t* in, size_t in_len) noexcept
    {
        detail::TlsHandshakeState* session = session_of(ssl);
        if (session == nullptr)
        {
            return ssl_ticket_aead_error;
        }
        const TlsSettings& settings = session->m_settings;
        if (in_len < kTicketOverhead || max_out_len < in_len - kTicketHeaderBytes)
        {
            return ssl_ticket_aead_ignore_ticket;
        }
        u32 index = 0U;
        while (index < settings.ticket_key_count &&
               std::memcmp(in, settings.ticket_names[index], kTicketNameBytes) != 0)
        {
            ++index;
        }
        if (index == settings.ticket_key_count)
        {
            return ssl_ticket_aead_ignore_ticket;
        }
        // `in` may be `out`: the salt is kept before the sealed bytes move to the front.
        u8 salt[kTicketSaltBytes];
        std::memcpy(salt, in + kTicketNameBytes, sizeof(salt));
        const usize sealed = in_len - kTicketHeaderBytes;
        std::memmove(out, in + kTicketHeaderBytes, sealed);
        AeadKey key;
        u8      nonce[kAeadNonceBytes];
        if (ticket_aead(cont::ConstSpan<u8>(settings.ticket_keys[index]), cont::ConstSpan<u8>(salt), key, nonce) !=
            CryptoError::None)
        {
            return ssl_ticket_aead_error;
        }
        const CryptoError opened = key.open(cont::ConstSpan<u8>(nonce), cont::ConstSpan<u8>(out, sealed), {},
                                            cont::Span<u8>(out, sealed - kAeadTagBytes));
        if (opened == CryptoError::Forged)
        {
            return ssl_ticket_aead_ignore_ticket;
        }
        if (opened != CryptoError::None)
        {
            return ssl_ticket_aead_error;
        }
        u64 issued = 0U;
        for (usize i = 0U; i < kTicketIssuedBytes; ++i)
        {
            issued = (issued << 8U) | out[i];
        }
        u8 context[kTicketContextBytes];
        std::memcpy(context, out + kTicketIssuedBytes, sizeof(context));
        const usize state = sealed - kAeadTagBytes - kTicketSealedHead;
        std::memmove(out, out + kTicketSealedHead, state);
        *out_len = state;
        if (settings.early_data && !constant_time_equal(context, settings.early_context))
        {
            SSL_set_early_data_enabled(ssl, 0);
            return ssl_ticket_aead_success;
        }
        if (settings.early_filter != nullptr &&
            !settings.early_filter->first_use(cont::ConstSpan<u8>(salt, kReplayIdBytes), issued, unix_now()))
        {
            session->m_ticket_replayed = true;
            return ssl_ticket_aead_ignore_ticket;
        }
        return ssl_ticket_aead_success;
    }

    // A server's compressed Certificate: the form made at init() for `algorithm`, copied. The provider asks only for
    // its own Certificate of the context's credential; any other message fails the handshake (internal_error).
    static int send_compressed(SSL* ssl, u16 algorithm, CBB* out, const uint8_t* in, size_t in_len) noexcept
    {
        const auto* certificates =
            static_cast<const CompressedCertificates*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), context_slot()));
        if (certificates == nullptr || CRYPTO_BUFFER_len(certificates->message) != in_len ||
            std::memcmp(CRYPTO_BUFFER_data(certificates->message), in, in_len) != 0)
        {
            return 0;
        }
        for (u32 i = 0U; i < certificates->count; ++i)
        {
            if (certificates->algorithms[i] == algorithm)
            {
                const CRYPTO_BUFFER* compressed = certificates->compressed[i];
                return CBB_add_bytes(out, CRYPTO_BUFFER_data(compressed), CRYPTO_BUFFER_len(compressed));
            }
        }
        return 0;
    }

    static int send_zstd(SSL* ssl, CBB* out, const uint8_t* in, size_t in_len) noexcept
    {
        return send_compressed(ssl, iana::kCertCompressionZstd, out, in, in_len);
    }

    static int send_zlib(SSL* ssl, CBB* out, const uint8_t* in, size_t in_len) noexcept
    {
        return send_compressed(ssl, iana::kCertCompressionZlib, out, in, in_len);
    }

    // A client decodes its server's compressed Certificate into a buffer of exactly the claimed length, reserved only
    // for a claim the compressed bytes can reach; the provider caps the claim at kTlsMaxCertificateBytes first
    // (max_cert_list, the limit of a plain Certificate). The provider takes the buffer.
    static int receive_compressed(u16 algorithm, CRYPTO_BUFFER** out, size_t length, const uint8_t* in,
                                  size_t in_len) noexcept
    {
        if (!certificate_claim_fits(algorithm, in_len, length))
        {
            return 0;
        }
        uint8_t*                       data = nullptr;
        bssl::UniquePtr<CRYPTO_BUFFER> buffer(CRYPTO_BUFFER_alloc(&data, length));
        if (!buffer || !detail::tls_decompress_certificate(algorithm, cont::ConstSpan<u8>(in, in_len),
                                                           cont::Span<u8>(data, length)))
        {
            return 0;
        }
        *out = buffer.release();
        return 1;
    }

    static int receive_zstd(SSL*, CRYPTO_BUFFER** out, size_t length, const uint8_t* in, size_t in_len) noexcept
    {
        return receive_compressed(iana::kCertCompressionZstd, out, length, in, in_len);
    }

    static int receive_zlib(SSL*, CRYPTO_BUFFER** out, size_t length, const uint8_t* in, size_t in_len) noexcept
    {
        return receive_compressed(iana::kCertCompressionZlib, out, length, in, in_len);
    }

#if CRD_TLS_KEY_LOG
    // Hands the provider's RFC 9850 line to the session's sink, the exporter secrets (EXPORTER_SECRET, and
    // EARLY_EXPORTER_SECRET should a provider log it) only where the context allows them. The provider zeroes its copy
    // of the line when it frees it.
    static void log_key(const SSL* ssl, const char* line) noexcept
    {
        const detail::TlsHandshakeState* session = session_of(ssl);
        if (session == nullptr || session->m_settings.key_log == nullptr)
        {
            return;
        }
        const TlsSettings&     settings = session->m_settings;
        const cont::StringView text(line, std::strlen(line));
        if (!settings.key_log_exporters &&
            (text.starts_with("EXPORTER_SECRET ") || text.starts_with("EARLY_EXPORTER_SECRET ")))
        {
            return;
        }
        settings.key_log(settings.key_log_user, text);
    }
#endif

    // Records the first alert each way, notices a HelloRetryRequest (a ServerHello with the special random), counts
    // the KeyUpdates each way, notes the algorithm of a compressed Certificate sent or received, notes that a client
    // encrypted an inner ClientHello (the provider reports it only when its outer hello offers ECH) and reads the
    // early data a client's NewSessionTicket allows, which keep_ticket records next. The provider reports a KeyUpdate
    // it writes when it queues it, sealed under the old key.
    static void observe(int is_write, int, int content_type, const void* buffer, size_t length, SSL* ssl,
                        void*) noexcept
    {
        detail::TlsHandshakeState* session = session_of(ssl);
        if (session == nullptr)
        {
            return;
        }
        const auto* bytes = static_cast<const u8*>(buffer);
        if (content_type == SSL3_RT_CLIENT_HELLO_INNER && is_write != 0)
        {
            session->m_ech_pending = false;
        }
        else if (content_type == SSL3_RT_ALERT && length >= 2U)
        {
            TlsAlert& alert = is_write != 0 ? session->m_alert_sent : session->m_alert_received;
            if (!alert.present)
            {
                alert = TlsAlert{true, bytes[1]};
            }
        }
        else if (content_type == SSL3_RT_HANDSHAKE && length >= kServerHelloRandomAt + sizeof(kHelloRetryRandom) &&
                 bytes[0] == kServerHello &&
                 std::memcmp(bytes + kServerHelloRandomAt, kHelloRetryRandom, sizeof(kHelloRetryRandom)) == 0)
        {
            session->m_hello_retry = true;
        }
        else if (content_type == SSL3_RT_HANDSHAKE && length >= kCompressionAlgorithmAt + 2U &&
                 bytes[0] == kCompressedCertificate)
        {
            session->m_certificate_compression =
                static_cast<u16>((bytes[kCompressionAlgorithmAt] << 8U) | bytes[kCompressionAlgorithmAt + 1U]);
        }
        else if (content_type == SSL3_RT_HANDSHAKE && is_write == 0 && length != 0U && bytes[0] == kNewSessionTicket)
        {
            session->m_ticket_early = ticket_early_data(cont::ConstSpan<u8>(bytes, length));
        }
        else if (content_type == SSL3_RT_HANDSHAKE && length > kKeyUpdateRequestAt && bytes[0] == kKeyUpdate)
        {
            if (is_write != 0)
            {
                ++session->m_updates_sent;
                if (bytes[kKeyUpdateRequestAt] == SSL_KEY_UPDATE_REQUESTED)
                {
                    session->m_update_requested = true;
                }
            }
            else
            {
                ++session->m_updates_received;
                session->m_update_requested = false;
            }
        }
    }

    // A server notes whether its client offers application protocols, before the name check reads it, and early data.
    static ssl_select_cert_result_t note_hello(const SSL_CLIENT_HELLO* hello) noexcept
    {
        detail::TlsHandshakeState* session = session_of(hello->ssl);
        const uint8_t*             data    = nullptr;
        size_t                     length  = 0U;
        if (session != nullptr)
        {
            session->m_protocol_offered = SSL_early_callback_ctx_extension_get(
                                              hello, TLSEXT_TYPE_application_layer_protocol_negotiation, &data,
                                              &length) == 1;
            session->m_early_offered =
                SSL_early_callback_ctx_extension_get(hello, TLSEXT_TYPE_early_data, &data, &length) == 1;
        }
        return ssl_select_cert_success;
    }

    // A server selects the first of its own application protocols that its client offers (`offered`, a wire list the
    // provider checked); none is no_application_protocol.
    static int select_protocol(SSL* ssl, const uint8_t** out, uint8_t* out_length, const uint8_t* offered,
                               unsigned offered_length, void*) noexcept
    {
        detail::TlsHandshakeState* session = session_of(ssl);
        if (session == nullptr)
        {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        const TlsSettings&        settings = session->m_settings;
        const cont::ConstSpan<u8> client(offered, offered_length);
        for (usize at = 0U; at < settings.protocol_bytes; at += 1U + settings.protocols[at])
        {
            const cont::ConstSpan<u8> name(settings.protocols + at + 1U, settings.protocols[at]);
            const u8*                 found = find_protocol(client, name);
            if (found != nullptr)
            {
                *out        = found;
                *out_length = static_cast<uint8_t>(name.size());
                return SSL_TLSEXT_ERR_OK;
            }
        }
        session->m_refused = true;
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }

    // Starts a connection of `context` for `state`: the provider's connection with the state in its slot, the
    // context's settings copied, a client's server name and ECH set (`retry`, a rejection's retry configurations, in
    // place of the context's list; a GREASE extension without either). NotReady for a context that holds none;
    // BadConfig for a server with a ticket store or retry configurations, a malformed `retry` and, with
    // `needs_protocol`, a context without application protocols.
    static TlsError begin(const TlsContext& context, TlsTicketStore* tickets, cont::ConstSpan<u8> retry,
                          cont::ConstSpan<u8> early_data_context, bool quic, detail::TlsHandshakeState& state,
                          bssl::UniquePtr<SSL>& ssl) noexcept
    {
        if (!context.ready())
        {
            return TlsError::NotReady;
        }
        const TlsSettings& settings = context.m_settings;
        bool               current  = false;
        // A server that accepts early data names the context its tickets' 0-RTT depends on; nothing else names one.
        const bool early_server = settings.role == TlsRole::Server && settings.early_data;
        if (((tickets != nullptr || !retry.empty()) && settings.role == TlsRole::Server) ||
            early_data_context.empty() == early_server || early_data_context.size() > kTlsMaxEarlyDataContextBytes ||
            (quic && settings.protocol_bytes == 0U) ||
            (!retry.empty() &&
             (retry.size() > kTlsMaxEchConfigListBytes || !walk_ech_configs(retry, current) || !current)))
        {
            return TlsError::BadConfig;
        }
        ssl.reset(SSL_new(ctx_of(context.m_context)));
        if (!ssl || !SSL_set_ex_data(ssl.get(), session_slot(), &state))
        {
            return TlsError::ProviderFailure;
        }
        state.m_settings            = settings;
        const EchContext*   ech     = ech_of(ssl.get());
        cont::ConstSpan<u8> configs = retry;
        if (configs.empty() && ech != nullptr)
        {
            configs = ech->list();
        }
        u8             allowed[kTlsMaxEchConfigListBytes];
        const TlsError policy = restrict_to_policy(ssl.get(), state, ech, configs, allowed);
        if (policy != TlsError::None)
        {
            return policy;
        }
        if (settings.role == TlsRole::Server)
        {
            if (early_server && !quic)
            {
                sha256(early_data_context, cont::Span<u8, kSha256Bytes>(state.m_settings.early_context, kSha256Bytes));
            }
            SSL_set_accept_state(ssl.get());
            return TlsError::None;
        }
        SSL_set_connect_state(ssl.get());
        state.m_tickets     = tickets;
        state.m_ech_retry   = !retry.empty();
        state.m_ech_pending = !configs.empty();
        SSL_set_enable_ech_grease(ssl.get(), configs.empty() && settings.ech_grease ? 1 : 0);
        if (!configs.empty() && !SSL_set1_ech_config_list(ssl.get(), configs.data(), configs.size()))
        {
            return TlsError::BadConfig;
        }
        return SSL_set_tlsext_host_name(ssl.get(), state.m_settings.server_name) == 1 ? TlsError::None
                                                                                      : TlsError::ProviderFailure;
    }

    // Decision S20: the session's offer and acceptance under the policy snapshot it takes now. The groups, AEADs and
    // peer signature algorithms the generation refuses in the context's scope drop out, the preferred ones lead; a
    // refused credential, or a list left empty, refuses the session before a byte is sent. Decision S21: a client's
    // ECH `configs` become those the generation allows, written into `allowed`, and a server whose ECH keys publish a
    // refused algorithm is refused the same way.
    static TlsError restrict_to_policy(SSL* ssl, detail::TlsHandshakeState& state, const EchContext* ech,
                                       cont::ConstSpan<u8>& configs, cont::Span<u8> allowed) noexcept
    {
        TlsSettings& settings = state.m_settings;
        if (settings.policy == nullptr)
        {
            return TlsError::None;
        }
        const PolicySnapshot snapshot = settings.policy->snapshot();
        const u64            now      = unix_now();
        const PolicyScope    scope    = settings.policy_scope;
        const bool           pinned   = settings.signature_count != 0U;
        state.m_policy_generation     = snapshot.generation();
        if (settings.credential_algorithm != AlgorithmId{} &&
            snapshot.status(settings.credential_algorithm, now, scope) == AlgorithmStatus::Refused)
        {
            return TlsError::PolicyRefused;
        }
        settings.group_count     = keep_allowed(snapshot, now, scope, settings.groups, settings.group_count);
        settings.aead_count      = keep_allowed(snapshot, now, scope, settings.aeads, settings.aead_count);
        settings.signature_count = keep_allowed(snapshot, now, scope, settings.signatures, settings.signature_count);
        if (settings.group_count == 0U || settings.aead_count == 0U || (pinned && settings.signature_count == 0U))
        {
            return TlsError::PolicyRefused;
        }
        if (ech != nullptr && !ech->publishes().empty() &&
            snapshot.status(ech->publishes(), now, scope) == AlgorithmStatus::Refused)
        {
            return TlsError::PolicyRefused;
        }
        if (!configs.empty())
        {
            const usize length = allowed_ech_configs(configs, snapshot, now, scope, allowed);
            if (length == 0U)
            {
                return TlsError::PolicyRefused;
            }
            configs = cont::ConstSpan<u8>(allowed.data(), length);
        }
        u16 groups[kTlsMaxGroups];
        for (u32 i = 0U; i < settings.group_count; ++i)
        {
            groups[i] = tls_group_codepoint(settings.groups[i]);
        }
        u16 schemes[kTlsMaxSignatures];
        for (u32 i = 0U; i < settings.signature_count; ++i)
        {
            schemes[i] = tls_signature_codepoint(settings.signatures[i]);
        }
        SuiteList suites;
        if (!suite_list_of(settings.aeads, settings.aead_count, suites) ||
            !SSL_set1_group_ids(ssl, groups, settings.group_count) || !SSL_set_ciphersuites(ssl, suites.text) ||
            (pinned && !SSL_set_verify_algorithm_prefs(ssl, schemes, settings.signature_count)))
        {
            return TlsError::ProviderFailure;
        }
        return TlsError::None;
    }

    // A client's first flight is out of the provider: a client given ECH configurations that encrypted no inner
    // ClientHello (none of them usable) is cleared before the transport takes a byte. BadConfig then.
    static TlsError first_flight(detail::TlsHandshakeState& state, TlsError advanced) noexcept
    {
        return advanced == TlsError::None && state.m_ech_pending ? TlsError::BadConfig : advanced;
    }

    // A client's ECH offer that the server rejected and then authenticated: the provider sends ech_required only after
    // the rejection handshake verified the server (by a pinned key here; RFC 9849 section 6.1.6), never on a GREASE
    // extension. Notes what the client may do next: retry once with the server's configurations, or report ECH
    // disabled, or, on a connection that already retried, refused.
    static bool ech_rejected(SSL* ssl, detail::TlsHandshakeState& state) noexcept
    {
        if (state.m_settings.role != TlsRole::Client || !state.m_alert_sent.present ||
            state.m_alert_sent.description != iana::kTlsAlertEchRequired)
        {
            return false;
        }
        if (state.m_ech_retry)
        {
            state.m_ech = TlsEch::Refused;
            return true;
        }
        const uint8_t* configs = nullptr;
        size_t         length  = 0U;
        SSL_get0_ech_retry_configs(ssl, &configs, &length);
        bool current = false;
        state.m_ech  = length != 0U && walk_ech_configs(cont::ConstSpan<u8>(configs, length), current) && current
                           ? TlsEch::Retry
                           : TlsEch::Disabled;
        return true;
    }

    // A client offers a ticket from its store under the connection's binding (offer_ticket) and keeps the early data
    // it allows; `parameters` is the record's server transport parameters, valid until the store's next call. False
    // when the provider fails.
    static bool offer(SSL* ssl, detail::TlsHandshakeState& state, cont::ConstSpan<u8>& parameters) noexcept
    {
        TicketRecord offered;
        if (state.m_tickets != nullptr &&
            !offer_ticket(ssl, state.m_settings.binding, state.m_tickets->take(state.m_settings.binding), offered))
        {
            return false;
        }
        state.m_early_allowance = offered.early;
        parameters = offered.parameters;
        return true;
    }

    // What became of early data: a client that offered learns whether it was accepted when its handshake ends (a
    // refusal that came during the handshake was noted then); a server, from its ClientHello on, reads its client's
    // offer, its own ReplayFilter's refusal and the provider's reason.
    static void settle_early(SSL* ssl, detail::TlsHandshakeState& state) noexcept
    {
        if (state.m_settings.role == TlsRole::Client)
        {
            if (state.m_early == TlsEarlyData::Offered)
            {
                state.m_early = SSL_early_data_accepted(ssl) == 1 ? TlsEarlyData::Accepted : TlsEarlyData::Rejected;
            }
            return;
        }
        // A ticket of another early data context turned early data off for its handshake (open_ticket): the decision
        // is made, so the connection's own tickets follow the context again.
        SSL_set_early_data_enabled(ssl, state.m_settings.early_data ? 1 : 0);
        if (!state.m_early_offered)
        {
            state.m_early = TlsEarlyData::None;
        }
        else if (state.m_ticket_replayed)
        {
            state.m_early = TlsEarlyData::Replayed;
        }
        else
        {
            switch (SSL_get_early_data_reason(ssl))
            {
            case ssl_early_data_unknown:
                state.m_early = TlsEarlyData::None;
                break;
            case ssl_early_data_accepted:
                state.m_early = TlsEarlyData::Accepted;
                break;
            case ssl_early_data_ticket_age_skew:
                state.m_early = TlsEarlyData::Stale;
                break;
            default:
                state.m_early = TlsEarlyData::Rejected;
                break;
            }
        }
    }

    // What a finished handshake settled: its group and AEAD, ECH, a resumption and its peer key, and the application
    // protocol a client asked for. A client with protocols whose server selected none sends no_application_protocol
    // (a QUIC provider refuses it earlier, RFC 9001 section 8.1). A server refuses, with general_error, a handshake
    // its client ran under an ECH public name alone: that name exists for a rejected client's retry only.
    static TlsError complete(SSL* ssl, detail::TlsHandshakeState& state) noexcept
    {
        state.m_group = algorithm_of(kGroups, SSL_get_group_id(ssl));
        state.m_aead  = aead_of(SSL_get_current_cipher(ssl));
        if (state.m_group == AlgorithmId{} || state.m_aead == AlgorithmId{})
        {
            return TlsError::ProviderFailure;
        }
        state.m_ech = SSL_ech_accepted(ssl) == 1 ? TlsEch::Accepted : TlsEch::None;
        if (state.m_public_name)
        {
            state.m_refused = true;
            (void)SSL_send_fatal_alert(ssl, kTlsAlertGeneralError);
            return TlsError::PeerRefused;
        }
        // A resumed handshake verified no certificate: the peer key is the one the ticket's handshake proved, which
        // the binding (the same pins) keeps pinned.
        state.m_resumed = SSL_session_reused(ssl) == 1;
        if (state.m_resumed && state.m_settings.pin_count != 0U && !pinned_peer(ssl, state))
        {
            return TlsError::PeerRefused;
        }
        const uint8_t* protocol = nullptr;
        unsigned       length   = 0U;
        SSL_get0_alpn_selected(ssl, &protocol, &length);
        if (state.m_settings.protocol_bytes != 0U && length == 0U)
        {
            state.m_refused = true;
            (void)SSL_send_fatal_alert(ssl, kTlsAlertNoApplicationProtocol);
            return TlsError::PeerRefused;
        }
        return TlsError::None;
    }

    // ---- The QUIC method (RFC 9001 section 4): the provider's hooks into a QuicHandshake ----

    static QuicHandshake* quic_of(const SSL* ssl) noexcept
    {
        // Only a QuicHandshake's connections carry the QUIC method.
        return static_cast<QuicHandshake*>(session_of(ssl));
    }

    // Keeps the secret of a Handshake or Application level, the peer's (read) or the handshake's own (write), or the
    // client's early data secret (a client's own, a server's to read), for derive_keys(). The provider never yields an
    // Initial secret.
    static int keep_secret(SSL* ssl, ssl_encryption_level_t level, const SSL_CIPHER* cipher, const uint8_t* secret,
                           size_t length, bool own) noexcept
    {
        QuicHandshake*    handshake = quic_of(ssl);
        const AlgorithmId aead      = aead_of(cipher);
        if (handshake == nullptr)
        {
            return 0;
        }
        const bool client = (handshake->m_settings.role == TlsRole::Client) == own;
        QuicLevel  quic   = QuicLevel::Initial;
        switch (level)
        {
        case ssl_encryption_handshake:
            quic = QuicLevel::Handshake;
            break;
        case ssl_encryption_application:
            quic = QuicLevel::Application;
            break;
        case ssl_encryption_early_data:
            quic = QuicLevel::Early;
            break;
        case ssl_encryption_initial:
            break;
        }
        const usize at = QuicHandshake::secret_level(quic, client ? QuicSide::Client : QuicSide::Server);
        if (aead == AlgorithmId{} || length == 0U || length > kQuicMaxSecretBytes || at == QuicHandshake::kNoSecret)
        {
            return 0;
        }
        QuicHandshake::Secret& kept = handshake->m_secrets[at][client ? 0U : 1U];
        std::memcpy(kept.bytes, secret, length);
        kept.size = static_cast<u8>(length);
        kept.aead = aead;
        return 1;
    }

    static int set_read_secret(SSL* ssl, ssl_encryption_level_t level, const SSL_CIPHER* cipher,
                               const uint8_t* secret, size_t length) noexcept
    {
        return keep_secret(ssl, level, cipher, secret, length, false);
    }

    static int set_write_secret(SSL* ssl, ssl_encryption_level_t level, const SSL_CIPHER* cipher,
                                const uint8_t* secret, size_t length) noexcept
    {
        return keep_secret(ssl, level, cipher, secret, length, true);
    }

    // Queues CRYPTO data at its level; more than kTlsMaxBufferedBytes waiting across the levels fails the handshake.
    static int add_handshake_data(SSL* ssl, ssl_encryption_level_t level, const uint8_t* data, size_t length) noexcept
    {
        QuicHandshake* handshake = quic_of(ssl);
        const usize    index     = quic_index_of(level);
        if (handshake == nullptr || index == kQuicLevels || length > kTlsMaxBufferedBytes)
        {
            return 0;
        }
        usize waiting = 0U;
        for (usize i = 0U; i < kQuicLevels; ++i)
        {
            waiting += handshake->pending(static_cast<QuicLevel>(i));
        }
        BIO* outgoing = bio_of(handshake->m_outgoing[index]);
        return waiting + length <= kTlsMaxBufferedBytes &&
                       BIO_write(outgoing, data, static_cast<int>(length)) == static_cast<int>(length)
                   ? 1
                   : 0;
    }

    // The data is already queued per level; the transport packs it when it takes it.
    static int flush_flight(SSL*) noexcept
    {
        return 1;
    }

    // Records the first fatal alert and its level: the transport closes the connection with CRYPTO_ERROR (0x0100 plus
    // the alert) at that level (RFC 9001 section 4.8).
    static int send_alert(SSL* ssl, ssl_encryption_level_t level, uint8_t alert) noexcept
    {
        QuicHandshake* handshake = quic_of(ssl);
        const usize    index     = quic_index_of(level);
        if (handshake == nullptr || index == kQuicLevels)
        {
            return 0;
        }
        if (!handshake->m_alert_sent.present)
        {
            handshake->m_alert_sent  = TlsAlert{true, alert};
            handshake->m_alert_level = static_cast<QuicLevel>(index);
        }
        return 1;
    }
};

namespace
{
constexpr SSL_QUIC_METHOD kQuicMethod = {
    &TlsCallbacks::set_read_secret, &TlsCallbacks::set_write_secret, &TlsCallbacks::add_handshake_data,
    &TlsCallbacks::flush_flight,    &TlsCallbacks::send_alert,
};
} // namespace

namespace
{
constexpr SSL_TICKET_AEAD_METHOD kTicketMethod = {&TlsCallbacks::ticket_overhead, &TlsCallbacks::seal_ticket,
                                                  &TlsCallbacks::open_ticket};

// One compressed form of a Certificate: false when the codec fails.
bool compress_with(u16 algorithm, cont::ConstSpan<u8> plain, cont::Array<u8>& form) noexcept
{
    if (algorithm == iana::kCertCompressionZstd)
    {
        return compression::zstd_compress(plain, kCertificateZstdLevel, form);
    }
    form = compression::zlib_deflate(plain, crd::memory::default_allocator());
    return !form.empty();
}

// Makes a server context's compressed Certificate (decision S10): the plain message the provider writes for `der` (an
// empty request context and one entry without extensions, RFC 9846 section 4.4.2), compressed once with each algorithm
// of `allowed` and kept when its CompressedCertificate is smaller than the plain message. The provider context
// compresses with the kept forms, smallest first, and with none when none is kept.
TlsError compress_certificate(SSL_CTX* context, cont::ConstSpan<u8> der, cont::ConstSpan<u16> allowed) noexcept
{
    constexpr usize most  = std::size(kTlsCertificateCompression);
    const usize     size  = der.size() + kCertificateEntryOverhead;
    cont::Array<u8> message;
    message.resize(size);
    u8* at = message.data();
    *at++  = 0U; // the request context, empty
    for (const usize length : {der.size() + 5U, der.size()})
    {
        *at++ = static_cast<u8>(length >> 16U);
        *at++ = static_cast<u8>(length >> 8U);
        *at++ = static_cast<u8>(length);
    }
    std::memcpy(at, der.data(), der.size());
    at[der.size()]      = 0U; // the entry's extensions, none
    at[der.size() + 1U] = 0U;

    cont::Array<u8> forms[most];
    u16             algorithms[most] = {};
    usize           count             = 0U;
    for (const u16 algorithm : allowed)
    {
        if (!compress_with(algorithm, cont::ConstSpan<u8>(message.data(), size), forms[count]))
        {
            return TlsError::ProviderFailure;
        }
        if (forms[count].size() + kCompressedOverhead < size)
        {
            algorithms[count++] = algorithm;
        }
    }
    if (count == 0U)
    {
        return TlsError::None;
    }
    if (count == most && forms[1].size() < forms[0].size())
    {
        std::swap(forms[0], forms[1]);
        std::swap(algorithms[0], algorithms[1]);
    }
    // The ex_data slot owns what is kept from here on: the provider context frees it, however init() ends.
    void* storage = OPENSSL_zalloc(sizeof(CompressedCertificates));
    auto* kept    = storage != nullptr ? new (storage) CompressedCertificates() : nullptr;
    if (kept == nullptr || !SSL_CTX_set_ex_data(context, context_slot(), kept))
    {
        free_compressed_certificates(nullptr, kept, nullptr, 0, 0, nullptr);
        return TlsError::ProviderFailure;
    }
    kept->message = CRYPTO_BUFFER_new(message.data(), size, nullptr);
    for (usize i = 0U; i < count && kept->message != nullptr; ++i)
    {
        kept->compressed[i] = CRYPTO_BUFFER_new(forms[i].data(), forms[i].size(), nullptr);
        kept->algorithms[i] = algorithms[i];
        kept->count         = static_cast<u32>(i + 1U);
        const bool zstd     = algorithms[i] == iana::kCertCompressionZstd;
        if (kept->compressed[i] == nullptr ||
            !SSL_CTX_add_cert_compression_alg(context, algorithms[i],
                                              zstd ? &TlsCallbacks::send_zstd : &TlsCallbacks::send_zlib, nullptr))
        {
            return TlsError::ProviderFailure;
        }
    }
    return kept->message != nullptr ? TlsError::None : TlsError::ProviderFailure;
}

// A context's ECH configuration (decision S14): a server's keys, at most kTlsMaxEchKeys in all, each ready, with
// distinct config_ids (the provider tries every key of a config_id on each offer), previous ones only beside current
// ones; a client's list of at most kTlsMaxEchConfigListBytes, well formed, with a configuration of version 0xfe0d.
// Neither role takes the other's.
bool valid_ech(const TlsConfig& config) noexcept
{
    const bool  server = config.role == TlsRole::Server;
    const usize count  = config.ech_keys.size() + config.previous_ech_keys.size();
    if (count > kTlsMaxEchKeys || (server ? !config.ech_configs.empty() : count != 0U) ||
        (config.ech_keys.empty() && !config.previous_ech_keys.empty()))
    {
        return false;
    }
    bool seen[256] = {};
    for (const cont::ConstSpan<const TlsEchKey*> keys : {config.ech_keys, config.previous_ech_keys})
    {
        for (const TlsEchKey* key : keys)
        {
            if (key == nullptr || !key->ready() || seen[key->config_id()])
            {
                return false;
            }
            seen[key->config_id()] = true;
        }
    }
    bool current = false;
    return config.ech_configs.empty() || (config.ech_configs.size() <= kTlsMaxEchConfigListBytes &&
                                          walk_ech_configs(config.ech_configs, current) && current);
}

// The provider context's ECH data, owned by its ex_data slot from here on (freed with the provider context however
// init() ends). Null when the provider fails.
// Early data: a server that accepts it opens tickets through a ready ReplayFilter whose period covers a ticket's whole
// lifetime and the provider's freshness window, so only a ticket the provider would accept no longer is ever too old
// for the filter; a filter goes with a server's early data only.
bool valid_early_data(const TlsConfig& config) noexcept
{
    if (config.early_data_filter == nullptr)
    {
        return !(config.early_data && config.role == TlsRole::Server);
    }
    const u64 lifetime = config.ticket_lifetime != 0U ? config.ticket_lifetime : kTlsDefaultTicketLifetime;
    return config.early_data && config.role == TlsRole::Server && config.early_data_filter->ready() &&
           config.early_data_filter->period() >= lifetime + kTlsMaxTicketAgeSkew;
}

EchContext* attach_ech(SSL_CTX* context) noexcept
{
    void* storage = OPENSSL_zalloc(sizeof(EchContext));
    auto* ech     = storage != nullptr ? new (storage) EchContext() : nullptr;
    if (ech == nullptr || !SSL_CTX_set_ex_data(context, ech_slot(), ech))
    {
        free_ech_context(nullptr, ech, nullptr, 0, 0, nullptr);
        return nullptr;
    }
    return ech;
}
} // namespace

cont::StringView tls_error_name(TlsError error) noexcept
{
    switch (error)
    {
    case TlsError::None:
        return "none";
    case TlsError::BadConfig:
        return "bad-config";
    case TlsError::NotReady:
        return "not-ready";
    case TlsError::Full:
        return "full";
    case TlsError::PeerRefused:
        return "peer-refused";
    case TlsError::Failed:
        return "failed";
    case TlsError::Closed:
        return "closed";
    case TlsError::EchRejected:
        return "ech-rejected";
    case TlsError::ProviderFailure:
        return "provider-failure";
    case TlsError::PolicyRefused:
        return "policy-refused";
    }
    return "unknown";
}

bool tls_key_log_compiled() noexcept
{
    return CRD_TLS_KEY_LOG != 0;
}

cont::StringView tls_ech_name(TlsEch ech) noexcept
{
    switch (ech)
    {
    case TlsEch::None:
        return "none";
    case TlsEch::Accepted:
        return "accepted";
    case TlsEch::Retry:
        return "retry";
    case TlsEch::Disabled:
        return "disabled";
    case TlsEch::Refused:
        return "refused";
    }
    return "unknown";
}

cont::StringView tls_early_data_name(TlsEarlyData early) noexcept
{
    switch (early)
    {
        case TlsEarlyData::None:
            return "none";
        case TlsEarlyData::Offered:
            return "offered";
        case TlsEarlyData::Accepted:
            return "accepted";
        case TlsEarlyData::Rejected:
            return "rejected";
        case TlsEarlyData::Replayed:
            return "replayed";
        case TlsEarlyData::Stale:
            break;
    }
    return "stale";
}

cont::ConstSpan<AlgorithmId> tls_aead_preference() noexcept
{
    static constexpr AlgorithmId kWithAes[]    = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm,
                                                  AlgorithmId::Chacha20Poly1305};
    static constexpr AlgorithmId kWithoutAes[] = {AlgorithmId::Chacha20Poly1305, AlgorithmId::Aes128Gcm,
                                                  AlgorithmId::Aes256Gcm};
    return EVP_has_aes_hardware() ? cont::ConstSpan<AlgorithmId>(kWithAes) : cont::ConstSpan<AlgorithmId>(kWithoutAes);
}

u64 tls_key_usage_limit(AlgorithmId aead) noexcept
{
    switch (aead)
    {
    case AlgorithmId::Aes128Gcm:
    case AlgorithmId::Aes256Gcm:
        return kAesGcmRecordLimit;
    case AlgorithmId::Chacha20Poly1305:
        return ~u64{0};
    default:
        return 0U;
    }
}

u16 tls_group_codepoint(AlgorithmId group) noexcept
{
    return codepoint_of(kGroups, group);
}

u16 tls_signature_codepoint(AlgorithmId signature) noexcept
{
    return codepoint_of(kSignatureSchemes, signature);
}

// ---- Credentials ----

namespace
{
// A heap copy of a device key's signer, released by free_device: a credential's own, and each context's.
DeviceSigner* copy_device(const DeviceSigner& signer) noexcept
{
    void* storage = OPENSSL_malloc(sizeof(DeviceSigner));
    return storage != nullptr ? new (storage) DeviceSigner(signer) : nullptr;
}

void free_device(DeviceSigner* signer) noexcept
{
    if (signer != nullptr)
    {
        signer->~DeviceSigner();
        OPENSSL_free(signer);
    }
}

void free_device_slot(void*, void* pointer, CRYPTO_EX_DATA*, int, long, void*) noexcept
{
    free_device(static_cast<DeviceSigner*>(pointer));
}

// The provider context's slot for the device signer its private-key method signs with, freed with the context.
int device_slot() noexcept
{
    static const int kSlot = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, &free_device_slot);
    return kSlot;
}

// The provider's private-key method for a device key (decision K12): each handshake signature through the device
// signer in the context's ex_data, synchronously, under the one signature scheme the context offers for it.
ssl_private_key_result_t device_sign(SSL* ssl, uint8_t* out, size_t* out_len, size_t max_out,
                                     uint16_t signature_algorithm, const uint8_t* in, size_t in_len) noexcept
{
    const auto* signer = static_cast<const DeviceSigner*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), device_slot()));
    usize       written = 0U;
    if (signer == nullptr || signature_algorithm != tls_signature_codepoint(signer->algorithm()) ||
        signer->sign(cont::ConstSpan<u8>(in, in_len), cont::Span<u8>(out, max_out), written) != CryptoError::None)
    {
        return ssl_private_key_failure;
    }
    *out_len = written;
    return ssl_private_key_success;
}

ssl_private_key_result_t device_decrypt(SSL*, uint8_t*, size_t*, size_t, const uint8_t*, size_t) noexcept
{
    return ssl_private_key_failure; // TLS 1.3 never decrypts with a credential's key
}

ssl_private_key_result_t device_complete(SSL*, uint8_t*, size_t*, size_t) noexcept
{
    return ssl_private_key_failure; // device_sign never leaves an operation pending
}

constexpr SSL_PRIVATE_KEY_METHOD kDeviceKeyMethod = {&device_sign, &device_decrypt, &device_complete};

// Signs a credential's certificate: by its provider key, or by `device` over the re-encoded TBSCertificate after the
// signature algorithm is set in both of its places (ecdsa-with-SHA256 or -SHA384, the curve naming the digest).
[[nodiscard]] bool sign_certificate(X509* certificate, EVP_PKEY* pkey, AlgorithmId algorithm,
                                    const DeviceSigner* device) noexcept
{
    if (device == nullptr)
    {
        return X509_sign(certificate, pkey, provider_internal::signature_digest(algorithm)) != 0;
    }
    const int nid = algorithm == AlgorithmId::EcdsaSecp384r1Sha384 ? NID_ecdsa_with_SHA384 : NID_ecdsa_with_SHA256;
    const bssl::UniquePtr<X509_ALGOR> signature_algorithm(X509_ALGOR_new());
    if (!signature_algorithm || !X509_ALGOR_set0(signature_algorithm.get(), OBJ_nid2obj(nid), V_ASN1_UNDEF, nullptr) ||
        !X509_set1_signature_algo(certificate, signature_algorithm.get()))
    {
        return false;
    }
    uint8_t*                       tbs       = nullptr;
    const int                      tbs_bytes = i2d_re_X509_tbs(certificate, &tbs);
    const bssl::UniquePtr<uint8_t> owner(tbs);
    u8                             signature[128]; // a DER ECDSA signature on P-384 is at most 104 bytes
    usize                          written = 0U;
    // The finished certificate is verified under its own key, as a peer that checks it would: the signature must
    // cover the TBSCertificate exactly as it is encoded in the certificate.
    return tbs_bytes > 0 &&
           device->sign(cont::ConstSpan<u8>(tbs, static_cast<usize>(tbs_bytes)), cont::Span<u8>(signature), written) ==
               CryptoError::None &&
           X509_set1_signature_value(certificate, signature, written) != 0 && X509_verify(certificate, pkey) == 1;
}

// Issues a credential's self-signed certificate for `pkey` (a key of `algorithm`; its public half at least) and its
// key identifier, signed by sign_certificate().
[[nodiscard]] TlsError issue_certificate(EVP_PKEY* pkey, AlgorithmId algorithm, cont::StringView name,
                                         const DeviceSigner* device, KeyId& id,
                                         bssl::UniquePtr<X509>& out) noexcept
{
    u8                   public_key[kMaxSignaturePublicKey];
    const cont::Span<u8> bytes(public_key, signature_sizes(algorithm).public_key);
    if (provider_internal::signature_public_bytes(pkey, algorithm, bytes) != CryptoError::None)
    {
        return TlsError::ProviderFailure;
    }
    id = key_id_of(bytes);

    // The serial number is the key identifier's first 16 bytes, made positive and full length: unique per key, the
    // only issuer of a self-signed certificate (RFC 5280 section 4.1.2.2).
    u8 serial[16];
    std::memcpy(serial, id.bytes, sizeof(serial));
    serial[0] = static_cast<u8>((serial[0] & 0x7FU) | 0x40U);

    bssl::UniquePtr<X509>         certificate(X509_new());
    const bssl::UniquePtr<BIGNUM> serial_number(BN_bin2bn(serial, sizeof(serial), nullptr));
    bssl::UniquePtr<ASN1_INTEGER> serial_field(
        serial_number ? BN_to_ASN1_INTEGER(serial_number.get(), nullptr) : nullptr);
    bssl::UniquePtr<ASN1_TIME>       not_before(ASN1_TIME_new());
    bssl::UniquePtr<ASN1_TIME>       not_after(ASN1_TIME_new());
    bssl::UniquePtr<GENERAL_NAMES>   alt_names(GENERAL_NAMES_new());
    bssl::UniquePtr<GENERAL_NAME>    dns_name(GENERAL_NAME_new());
    bssl::UniquePtr<ASN1_IA5STRING>  dns_text(ASN1_IA5STRING_new());
    if (!certificate || !serial_field || !not_before || !not_after || !alt_names || !dns_name || !dns_text)
    {
        return TlsError::ProviderFailure;
    }
    // The dates carry nothing: trust comes from the pin, so the certificate is valid from the epoch and has no
    // well-defined expiration date (RFC 5280 section 4.1.2.5). The name is the subjectAltName's; the common name
    // repeats it when it fits (64 characters, RFC 5280 appendix A's ub-common-name), and a longer name leaves the
    // subject empty and the subjectAltName critical (RFC 5280 section 4.1.2.6).
    const auto* name_bytes = reinterpret_cast<const unsigned char*>(name.data());
    const auto  name_size  = static_cast<int>(name.size());
    const bool  common     = name.size() <= kMaxCommonName;
    X509_NAME*  subject    = X509_get_subject_name(certificate.get());
    if (!X509_set_version(certificate.get(), X509_VERSION_3) ||
        !X509_set_serialNumber(certificate.get(), serial_field.get()) ||
        (common && !X509_NAME_add_entry_by_NID(subject, NID_commonName, MBSTRING_ASC, name_bytes, name_size, -1, 0)) ||
        !X509_set_issuer_name(certificate.get(), subject) ||
        !ASN1_TIME_set_string_X509(not_before.get(), "19700101000000Z") ||
        !ASN1_TIME_set_string_X509(not_after.get(), "99991231235959Z") ||
        !X509_set1_notBefore(certificate.get(), not_before.get()) ||
        !X509_set1_notAfter(certificate.get(), not_after.get()) || !X509_set_pubkey(certificate.get(), pkey) ||
        !ASN1_STRING_set(dns_text.get(), name.data(), name_size))
    {
        return TlsError::ProviderFailure;
    }
    GENERAL_NAME_set0_value(dns_name.get(), GEN_DNS, dns_text.release());
    if (!bssl::PushToStack(alt_names.get(), std::move(dns_name)) ||
        !X509_add1_ext_i2d(certificate.get(), NID_subject_alt_name, alt_names.get(), common ? 0 : 1,
                           X509V3_ADD_DEFAULT))
    {
        return TlsError::ProviderFailure;
    }
    if (!sign_certificate(certificate.get(), pkey, algorithm, device))
    {
        return TlsError::ProviderFailure;
    }
    out = std::move(certificate);
    return TlsError::None;
}
} // namespace

TlsCredential::~TlsCredential() noexcept
{
    clear();
}

void TlsCredential::clear() noexcept
{
    X509_free(x509_of(m_certificate));
    m_certificate = nullptr;
    m_key.reset();
    free_device(m_device);
    m_device = nullptr;
    m_key_id = KeyId{};
}

AlgorithmId TlsCredential::algorithm() const noexcept
{
    return m_device != nullptr ? m_device->algorithm() : m_key.algorithm();
}

TlsError TlsCredential::init(const StoredSigner& signer, cont::StringView name) noexcept
{
    const DeviceSigner* device = signer.device();
    if (device == nullptr)
    {
        return init(signer.m_key, name);
    }
    clear();
    if (!valid_host_name(name))
    {
        return TlsError::BadConfig;
    }
    const ErrorScope     errors;
    const AlgorithmId    algorithm = device->algorithm();
    u8                   public_key[kDevicePublicKeyMaxBytes];
    const cont::Span<u8> bytes(public_key, signature_sizes(algorithm).public_key);
    if (device->public_key(bytes) != CryptoError::None)
    {
        return TlsError::ProviderFailure;
    }
    const bssl::UniquePtr<EVP_PKEY> pkey(provider_internal::signature_public_pkey(algorithm, bytes));
    bssl::UniquePtr<X509>           certificate;
    KeyId                           id;
    if (!pkey)
    {
        return TlsError::ProviderFailure;
    }
    const TlsError issued = issue_certificate(pkey.get(), algorithm, name, device, id, certificate);
    if (issued != TlsError::None)
    {
        return issued;
    }
    m_device = copy_device(*device);
    if (m_device == nullptr)
    {
        return TlsError::ProviderFailure;
    }
    m_certificate = reinterpret_cast<ProviderCertificate*>(certificate.release());
    m_key_id      = id;
    return TlsError::None;
}

TlsError TlsCredential::init(const SigningKey& key, cont::StringView name) noexcept
{
    clear();
    if (!key.ready())
    {
        return TlsError::NotReady;
    }
    if (!valid_host_name(name))
    {
        return TlsError::BadConfig;
    }
    const ErrorScope      errors;
    const AlgorithmId     algorithm = key.algorithm();
    EVP_PKEY*             pkey      = pkey_of(key.m_key.get());
    bssl::UniquePtr<X509> certificate;
    KeyId                 id;
    const TlsError        issued = issue_certificate(pkey, algorithm, name, nullptr, id, certificate);
    if (issued != TlsError::None)
    {
        return issued;
    }
    if (!EVP_PKEY_up_ref(pkey))
    {
        return TlsError::ProviderFailure;
    }
    m_key.reset(key_of(pkey), algorithm);
    m_certificate = reinterpret_cast<ProviderCertificate*>(certificate.release());
    m_key_id      = id;
    return TlsError::None;
}

// ---- ECH keys ----

TlsEchKey::~TlsEchKey() noexcept
{
    clear();
}

void TlsEchKey::clear() noexcept
{
    // The provider zeroes the key's memory when it frees it.
    EVP_HPKE_KEY_free(hpke_of(m_key));
    m_key          = nullptr;
    m_config_bytes = 0U;
    m_name_bytes   = 0U;
    m_config_id    = 0U;
    m_aead_count   = 0U;
    std::memset(m_name, 0, sizeof(m_name));
}

TlsError TlsEchKey::init(cont::ConstSpan<u8> private_key, u8 config_id, cont::StringView public_name,
                         u8 max_name_length, cont::ConstSpan<AlgorithmId> aeads) noexcept
{
    clear();
    if (private_key.size() != kTlsEchKeyBytes || !valid_public_name(public_name) || aeads.empty() ||
        aeads.size() > kTlsMaxEchAeads)
    {
        return TlsError::BadConfig;
    }
    u16 suites[kTlsMaxEchAeads] = {};
    for (usize i = 0U; i < aeads.size(); ++i)
    {
        suites[i] = hpke_codepoints({AlgorithmId::X25519, AlgorithmId::HkdfSha256, aeads[i]}).aead;
        const auto earlier_end = aeads.begin() + static_cast<std::ptrdiff_t>(i);
        if (suites[i] == 0U || std::find(aeads.begin(), earlier_end, aeads[i]) != earlier_end)
        {
            return TlsError::BadConfig;
        }
    }
    const ErrorScope              errors;
    bssl::UniquePtr<EVP_HPKE_KEY> key(EVP_HPKE_KEY_new());
    u8                            public_key[kTlsEchKeyBytes] = {};
    size_t                        public_bytes                = 0U;
    if (!key ||
        !EVP_HPKE_KEY_init(key.get(), EVP_hpke_x25519_hkdf_sha256(), private_key.data(), private_key.size()) ||
        !EVP_HPKE_KEY_public_key(key.get(), public_key, &public_bytes, sizeof(public_key)))
    {
        return TlsError::ProviderFailure;
    }
    // RFC 9849 section 4: version and length, then the HpkeKeyConfig (config_id, kem_id, public_key and the
    // cipher_suites, each HKDF-SHA256 with one AEAD), maximum_name_length, public_name and no extensions.
    bssl::ScopedCBB cbb;
    CBB             contents;
    CBB             child;
    size_t          length = 0U;
    bool            ok = CBB_init_fixed(cbb.get(), m_config, sizeof(m_config)) == 1 &&
              CBB_add_u16(cbb.get(), iana::kTlsExtensionEncryptedClientHello) == 1 &&
              CBB_add_u16_length_prefixed(cbb.get(), &contents) == 1 && CBB_add_u8(&contents, config_id) == 1 &&
              CBB_add_u16(&contents, iana::kHpkeKemDhkemX25519HkdfSha256) == 1 &&
              CBB_add_u16_length_prefixed(&contents, &child) == 1 &&
              CBB_add_bytes(&child, public_key, public_bytes) == 1 &&
              CBB_add_u16_length_prefixed(&contents, &child) == 1;
    for (usize i = 0U; ok && i < aeads.size(); ++i)
    {
        ok = CBB_add_u16(&child, iana::kHpkeKdfHkdfSha256) == 1 && CBB_add_u16(&child, suites[i]) == 1;
    }
    ok = ok && CBB_add_u8(&contents, max_name_length) == 1 && CBB_add_u8_length_prefixed(&contents, &child) == 1 &&
         CBB_add_bytes(&child, reinterpret_cast<const uint8_t*>(public_name.data()), public_name.size()) == 1 &&
         CBB_add_u16(&contents, 0U) == 1 && CBB_finish(cbb.get(), nullptr, &length) == 1;
    if (!ok)
    {
        clear();
        return TlsError::ProviderFailure;
    }
    std::memcpy(m_name, public_name.data(), public_name.size());
    std::copy(aeads.begin(), aeads.end(), m_aeads);
    m_aead_count   = static_cast<u32>(aeads.size());
    m_config_bytes = static_cast<u32>(length);
    m_name_bytes   = static_cast<u32>(public_name.size());
    m_config_id    = config_id;
    m_key          = reinterpret_cast<ProviderEchKey*>(key.release());
    return TlsError::None;
}

TlsError tls_ech_config_list(cont::ConstSpan<const TlsEchKey*> keys, cont::Span<u8> out, usize& written) noexcept
{
    written     = 0U;
    usize total = 2U;
    if (keys.empty() || keys.size() > kTlsMaxEchKeys)
    {
        return TlsError::BadConfig;
    }
    for (const TlsEchKey* key : keys)
    {
        if (key == nullptr || !key->ready())
        {
            return TlsError::BadConfig;
        }
        total += key->config().size();
    }
    if (out.size() < total)
    {
        return TlsError::Full;
    }
    out[0]   = static_cast<u8>((total - 2U) >> 8U);
    out[1]   = static_cast<u8>(total - 2U);
    usize at = 2U;
    for (const TlsEchKey* key : keys)
    {
        std::memcpy(out.data() + at, key->config().data(), key->config().size());
        at += key->config().size();
    }
    written = total;
    return TlsError::None;
}

// ---- Contexts ----

TlsContext::~TlsContext() noexcept
{
    clear();
}

void TlsContext::clear() noexcept
{
    SSL_CTX_free(ctx_of(m_context));
    m_context = nullptr;
    zero_ticket_keys(m_settings);
    m_settings = TlsSettings{};
}

TlsError TlsContext::init(const TlsConfig& config) noexcept
{
    clear();
    const bool server = config.role == TlsRole::Server;
    if (config.groups.empty() ||
        !valid_list(config.groups, kTlsMaxGroups, AlgorithmClass::HybridKeyExchange,
                    AlgorithmClass::ClassicalKeyExchange) ||
        !valid_list(config.signatures, kTlsMaxSignatures, AlgorithmClass::PostQuantumSignature,
                    AlgorithmClass::ClassicalSignature) ||
        config.aeads.empty() || !valid_list(config.aeads, kTlsMaxAeads, AlgorithmClass::Aead, AlgorithmClass::Aead) ||
        config.pinned.size() > kTlsMaxPins || (config.pinned.empty() != config.signatures.empty()) ||
        (!server && config.pinned.empty()) || (server && config.credential == nullptr) ||
        (config.credential != nullptr && !config.credential->ready()) ||
        (!config.server_name.empty() && !valid_host_name(config.server_name)) ||
        (!server && config.server_name.empty()) ||
        (config.key_update_records != 0U && config.key_update_records < kTlsMinKeyUpdateRecords) ||
        config.max_key_updates > kTlsMaxKeyUpdates ||
        (!server &&
         (!config.ticket_key.empty() || !config.previous_ticket_key.empty() || config.ticket_lifetime != 0U)) ||
        (!config.ticket_key.empty() && config.ticket_key.size() != kTlsTicketKeyBytes) ||
        (!config.previous_ticket_key.empty() &&
         (config.previous_ticket_key.size() != kTlsTicketKeyBytes || config.ticket_key.empty())) ||
        config.ticket_lifetime > kTlsMaxTicketLifetime || !valid_compression(config.certificate_compression) ||
        !valid_ech(config) || !valid_early_data(config) ||
        (config.key_log == nullptr && (config.key_log_user != nullptr || config.key_log_exporters)) ||
        (config.key_log != nullptr && !tls_key_log_compiled()) ||
        (config.policy == nullptr && config.policy_scope != PolicyScope::Everywhere) ||
        (config.policy_scope != PolicyScope::Everywhere && config.policy_scope != PolicyScope::CeridServices))
    {
        return TlsError::BadConfig;
    }

    TlsSettings settings;
    settings.role = config.role;
    std::copy(config.signatures.begin(), config.signatures.end(), settings.signatures);
    settings.signature_count = static_cast<u32>(config.signatures.size());
    std::copy(config.groups.begin(), config.groups.end(), settings.groups);
    settings.group_count = static_cast<u32>(config.groups.size());
    std::copy(config.aeads.begin(), config.aeads.end(), settings.aeads);
    settings.aead_count           = static_cast<u32>(config.aeads.size());
    settings.credential_algorithm = config.credential != nullptr ? config.credential->algorithm() : AlgorithmId{};
    settings.policy               = config.policy;
    settings.policy_scope         = config.policy_scope;
    std::copy(config.pinned.begin(), config.pinned.end(), settings.pinned);
    settings.pin_count = static_cast<u32>(config.pinned.size());
    std::copy(config.server_name.begin(), config.server_name.end(), settings.server_name);
    settings.name_bytes         = static_cast<u32>(config.server_name.size());
    settings.key_update_records = config.key_update_records;
    settings.max_key_updates    = config.max_key_updates != 0U ? config.max_key_updates : kTlsMaxKeyUpdates;
    settings.ech_grease         = !server && config.ech_grease;
    settings.early_data = config.early_data;
    settings.early_filter = config.early_data_filter;
    settings.key_log           = config.key_log;
    settings.key_log_user      = config.key_log_user;
    settings.key_log_exporters = config.key_log_exporters;
    settings.binding = binding_of(config, config.credential != nullptr ? config.credential->key_id() : KeyId{});
    if (!protocols_of(config.application_protocols, settings))
    {
        return TlsError::BadConfig;
    }
    const TicketKeysGuard guard(settings);
    if (server && !ticket_keys_of(config, settings))
    {
        return TlsError::ProviderFailure;
    }

    u16 groups[kTlsMaxGroups];
    for (usize i = 0U; i < config.groups.size(); ++i)
    {
        groups[i] = tls_group_codepoint(config.groups[i]);
    }
    u16 schemes[kTlsMaxSignatures];
    for (usize i = 0U; i < config.signatures.size(); ++i)
    {
        schemes[i] = tls_signature_codepoint(config.signatures[i]);
    }
    SuiteList suites;
    if (!suite_list_of(config.aeads.data(), config.aeads.size(), suites))
    {
        return TlsError::BadConfig;
    }

    const ErrorScope          errors;
    bssl::UniquePtr<SSL_CTX> context(SSL_CTX_new(TLS_method()));
    if (!context || !SSL_CTX_set_min_proto_version(context.get(), TLS1_3_VERSION) ||
        !SSL_CTX_set_max_proto_version(context.get(), TLS1_3_VERSION) ||
        !SSL_CTX_set1_group_ids(context.get(), groups, config.groups.size()) ||
        !SSL_CTX_set_ciphersuites(context.get(), suites.text))
    {
        return TlsError::ProviderFailure;
    }
    // Server preference orders the groups (the provider's TLS 1.3 AEAD choice follows the client, see tls.hpp).
    SSL_CTX_set_options(context.get(), SSL_OP_CIPHER_SERVER_PREFERENCE);
    // Early data (S17): a client offers it on a ticket that allows it, a server issues such tickets and accepts it.
    SSL_CTX_set_early_data_enabled(context.get(), config.early_data ? 1 : 0);
    // Tickets are stateless: a server keeps no session cache (no memory an unauthenticated peer could fill) and issues
    // one ticket per connection through its ticket method, valid for the configured lifetime and never longer than
    // seven days after the handshake that authenticated the peer (the provider's non-renewable authentication
    // timeout); a client reports each ticket to keep_ticket. The binding is the session ID context, which both ends
    // check before resuming.
    SSL_CTX_set_session_cache_mode(context.get(), server ? SSL_SESS_CACHE_NO_INTERNAL : SSL_SESS_CACHE_CLIENT);
    if (!SSL_CTX_set_session_id_context(context.get(), settings.binding.bytes, sizeof(settings.binding.bytes)))
    {
        return TlsError::ProviderFailure;
    }
    // A server's tickets live its configured lifetime; a client keeps each for the lifetime its server states (the
    // provider caps a client's ticket at its own context's lifetime, here the profile's seven days).
    u32 lifetime = kTlsMaxTicketLifetime;
    if (server)
    {
        lifetime = config.ticket_lifetime != 0U ? config.ticket_lifetime : kTlsDefaultTicketLifetime;
    }
    SSL_CTX_set_session_psk_dhe_timeout(context.get(), lifetime);
    if (server)
    {
        SSL_CTX_set_ticket_aead_method(context.get(), &kTicketMethod);
    }
    else
    {
        SSL_CTX_sess_set_new_cb(context.get(), &TlsCallbacks::keep_ticket);
    }
    // GREASE (RFC 8701, ietf.grease) in every build: a client's hello carries reserved values, chosen per connection,
    // in its cipher suites, supported groups, supported versions, key shares and two extensions, so a peer or
    // middlebox that refuses an unknown value fails in testing rather than in the field.
    SSL_CTX_set_grease_enabled(context.get(), 1);
    // A peer's Certificate, compressed or not, is at most kTlsMaxCertificateBytes (RFC 8879 section 5: the same limit
    // either way); the provider refuses a longer one, and a compressed one claiming more, before decoding it.
    SSL_CTX_set_max_cert_list(context.get(), kTlsMaxCertificateBytes);
    if (!SSL_CTX_set_num_tickets(context.get(), server ? 1U : 0U))
    {
        return TlsError::ProviderFailure;
    }
    if (!config.signatures.empty())
    {
        if (!SSL_CTX_set_verify_algorithm_prefs(context.get(), schemes, config.signatures.size()))
        {
            return TlsError::ProviderFailure;
        }
        const int mode = server ? SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT : SSL_VERIFY_PEER;
        SSL_CTX_set_custom_verify(context.get(), mode, &TlsCallbacks::verify_peer);
    }
    if (config.credential != nullptr)
    {
        const TlsCredential& credential = *config.credential;
        const u16            scheme     = tls_signature_codepoint(credential.algorithm());
        if (!SSL_CTX_use_certificate(context.get(), x509_of(credential.m_certificate)) ||
            !SSL_CTX_set_signing_algorithm_prefs(context.get(), &scheme, 1U))
        {
            return TlsError::ProviderFailure;
        }
        if (credential.m_device == nullptr)
        {
            if (!SSL_CTX_use_PrivateKey(context.get(), pkey_of(credential.m_key.get())))
            {
                return TlsError::ProviderFailure;
            }
        }
        else
        {
            // The context keeps its own copy of the device signer, freed with it however init() ends.
            DeviceSigner* kept = copy_device(*credential.m_device);
            if (kept == nullptr || !SSL_CTX_set_ex_data(context.get(), device_slot(), kept))
            {
                free_device(kept);
                return TlsError::ProviderFailure;
            }
            SSL_CTX_set_private_key_method(context.get(), &kDeviceKeyMethod);
        }
    }
    if (server)
    {
        SSL_CTX_set_tlsext_servername_callback(context.get(), &TlsCallbacks::check_name);
        SSL_CTX_set_select_certificate_cb(context.get(), &TlsCallbacks::note_hello);
        uint8_t*                       der       = nullptr;
        const int                      der_bytes = i2d_X509(x509_of(config.credential->m_certificate), &der);
        const bssl::UniquePtr<uint8_t> owner(der);
        if (der_bytes <= 0)
        {
            return TlsError::ProviderFailure;
        }
        const TlsError compressed = compress_certificate(
            context.get(), cont::ConstSpan<u8>(der, static_cast<usize>(der_bytes)), config.certificate_compression);
        if (compressed != TlsError::None)
        {
            return compressed;
        }
    }
    else
    {
        // A client offers the algorithms it decodes in its order; the provider's server never asks for a client's
        // Certificate compressed, so a client compresses nothing.
        for (const u16 algorithm : config.certificate_compression)
        {
            const bool zstd = algorithm == iana::kCertCompressionZstd;
            if (!SSL_CTX_add_cert_compression_alg(context.get(), algorithm, nullptr,
                                                  zstd ? &TlsCallbacks::receive_zstd : &TlsCallbacks::receive_zlib))
            {
                return TlsError::ProviderFailure;
            }
        }
    }
    // Application protocols (RFC 7301): a client offers its list, a server selects from it in its own order and,
    // through the name check, refuses a client that offers none.
    if (settings.protocol_bytes != 0U)
    {
        if (!server && SSL_CTX_set_alpn_protos(context.get(), settings.protocols, settings.protocol_bytes) != 0)
        {
            return TlsError::ProviderFailure;
        }
        if (server)
        {
            SSL_CTX_set_alpn_select_cb(context.get(), &TlsCallbacks::select_protocol, nullptr);
        }
    }
    // ECH (decision S14): a server's keys, the current ones its retry configurations, and their public names for the
    // name check; a client's list, which each connection takes.
    if (!config.ech_keys.empty() || !config.ech_configs.empty())
    {
        EchContext* ech = attach_ech(context.get());
        if (ech == nullptr)
        {
            return TlsError::ProviderFailure;
        }
        std::copy(config.ech_configs.begin(), config.ech_configs.end(), ech->configs);
        ech->config_bytes = static_cast<u32>(config.ech_configs.size());
        const bssl::UniquePtr<SSL_ECH_KEYS> keys(server ? SSL_ECH_KEYS_new() : nullptr);
        for (const bool retry : {true, false})
        {
            for (const TlsEchKey* key : retry ? config.ech_keys : config.previous_ech_keys)
            {
                const cont::ConstSpan<u8> published = key->config();
                const cont::StringView    name      = key->public_name();
                if (!keys || !SSL_ECH_KEYS_add(keys.get(), retry ? 1 : 0, published.data(), published.size(),
                                               hpke_of(key->m_key)))
                {
                    return TlsError::ProviderFailure;
                }
                std::memcpy(ech->names[ech->name_count], name.data(), name.size());
                ech->name_bytes[ech->name_count++] = static_cast<u32>(name.size());
                ech->publish(AlgorithmId::X25519);
                ech->publish(AlgorithmId::HkdfSha256);
                for (const AlgorithmId aead : key->aeads())
                {
                    ech->publish(aead);
                }
            }
        }
        if (server && !SSL_CTX_set1_ech_keys(context.get(), keys.get()))
        {
            return TlsError::ProviderFailure;
        }
    }
    SSL_CTX_set_msg_callback(context.get(), &TlsCallbacks::observe);
#if CRD_TLS_KEY_LOG
    if (config.key_log != nullptr)
    {
        SSL_CTX_set_keylog_callback(context.get(), &TlsCallbacks::log_key);
    }
#endif
    if (session_slot() < 0 || ech_slot() < 0)
    {
        return TlsError::ProviderFailure;
    }
    m_context  = reinterpret_cast<ProviderTlsContext*>(context.release());
    m_settings = settings;
    return TlsError::None;
}

// ---- What sessions and QUIC handshakes share ----

void detail::TlsHandshakeState::release() noexcept
{
    // SSL_free releases the connection and its keys (zeroed).
    SSL_free(ssl_of(m_session));
    m_session = nullptr;
    m_tickets = nullptr;
    zero_ticket_keys(m_settings);
    m_settings                = TlsSettings{};
    m_state                   = TlsState::Idle;
    m_group                   = AlgorithmId{};
    m_aead                    = AlgorithmId{};
    m_peer_algorithm          = AlgorithmId{};
    m_peer_key                = KeyId{};
    m_updates_sent            = 0U;
    m_updates_received        = 0U;
    m_policy_generation = 0U;
    m_early_allowance = 0U;
    m_early_sent = 0U;
    m_ticket_early = 0U;
    m_certificate_compression = 0U;
    m_hello_retry             = false;
    m_resumed                 = false;
    m_refused                 = false;
    m_update_requested        = false;
    m_protocol_offered        = false;
    m_ech_pending             = false;
    m_ech_retry               = false;
    m_public_name             = false;
    m_early_offered = false;
    m_ticket_replayed = false;
    m_ech                     = TlsEch::None;
    m_early = TlsEarlyData::None;
    m_alert_sent              = TlsAlert{};
    m_alert_received          = TlsAlert{};
}

TlsError detail::TlsHandshakeState::fail(TlsError error) noexcept
{
    m_state = TlsState::Failed;
    return error;
}

TlsError detail::TlsHandshakeState::check_policy() const noexcept
{
    if (m_state != TlsState::Established)
    {
        return TlsError::NotReady;
    }
    if (m_settings.policy == nullptr)
    {
        return TlsError::None;
    }
    const PolicySnapshot snapshot  = m_settings.policy->snapshot();
    const u64            now       = unix_now();
    const AlgorithmId    running[] = {m_group, m_aead, m_peer_algorithm, m_settings.credential_algorithm};
    for (const AlgorithmId algorithm : running)
    {
        if (algorithm != AlgorithmId{} &&
            snapshot.status(algorithm, now, m_settings.policy_scope) == AlgorithmStatus::Refused)
        {
            return TlsError::PolicyRefused;
        }
    }
    return TlsError::None;
}

cont::ConstSpan<u8> detail::TlsHandshakeState::ech_retry_configs() const noexcept
{
    if (m_ech != TlsEch::Retry)
    {
        return {};
    }
    const uint8_t* configs = nullptr;
    size_t         length  = 0U;
    SSL_get0_ech_retry_configs(ssl_of(m_session), &configs, &length);
    return cont::ConstSpan<u8>(configs, length);
}

cont::StringView detail::TlsHandshakeState::application_protocol() const noexcept
{
    if (m_session == nullptr)
    {
        return {};
    }
    const uint8_t* protocol = nullptr;
    unsigned       length   = 0U;
    SSL_get0_alpn_selected(ssl_of(m_session), &protocol, &length);
    return cont::StringView(reinterpret_cast<const char*>(protocol), length);
}

TlsError detail::TlsHandshakeState::export_keying_material(cont::StringView label, cont::ConstSpan<u8> context,
                                                            cont::Span<u8> out) const noexcept
{
    secure_zero(out);
    if (label.empty() || label.size() > kTlsMaxExporterLabelBytes || out.empty() || out.size() > kTlsMaxExporterBytes)
    {
        return TlsError::BadConfig;
    }
    if (m_state != TlsState::Established)
    {
        return TlsError::NotReady;
    }
    const ErrorScope errors;
    // TLS 1.3 gives no context and an empty one the same exporter (RFC 9846 section 7.5): the context is always used.
    if (SSL_export_keying_material(ssl_of(m_session), out.data(), out.size(), label.data(), label.size(),
                                   context.data(), context.size(), 1) != 1)
    {
        secure_zero(out);
        return TlsError::ProviderFailure;
    }
    return TlsError::None;
}

// ---- Sessions ----

TlsSession::~TlsSession() noexcept
{
    clear();
}

void TlsSession::clear() noexcept
{
    // The connection frees both memory buffers with itself.
    release();
    m_key_update_records = 0U;
}

TlsError TlsSession::init(const TlsContext& context, TlsTicketStore* tickets, cont::ConstSpan<u8> ech_retry_configs,
                          cont::ConstSpan<u8> early_data_context) noexcept
{
    clear();
    const ErrorScope     errors;
    bssl::UniquePtr<SSL> ssl;
    const TlsError       begun =
        TlsCallbacks::begin(context, tickets, ech_retry_configs, early_data_context, false, *this, ssl);
    bssl::UniquePtr<BIO> incoming(begun == TlsError::None ? BIO_new(BIO_s_mem()) : nullptr);
    bssl::UniquePtr<BIO> outgoing(begun == TlsError::None ? BIO_new(BIO_s_mem()) : nullptr);
    if (begun != TlsError::None || !incoming || !outgoing)
    {
        clear();
        return begun != TlsError::None ? begun : TlsError::ProviderFailure;
    }
    // An empty incoming buffer means "more bytes later", never the end of the stream.
    BIO_set_mem_eof_return(incoming.get(), -1);
    SSL_set_bio(ssl.get(), incoming.release(), outgoing.release());
    cont::ConstSpan<u8> parameters;
    if (!TlsCallbacks::offer(ssl.get(), *this, parameters))
    {
        clear();
        return TlsError::ProviderFailure;
    }
    m_session = reinterpret_cast<ProviderTlsSession*>(ssl.release());
    m_state   = TlsState::Handshaking;
    if (m_settings.role == TlsRole::Server)
    {
        return TlsError::None;
    }
    const TlsError first = TlsCallbacks::first_flight(*this, advance());
    if (first == TlsError::BadConfig)
    {
        clear();
    }
    return first;
}

TlsError TlsSession::advance() noexcept
{
    SSL*      ssl    = ssl_of(m_session);
    const int result = SSL_do_handshake(ssl);
    if (m_settings.role == TlsRole::Server)
    {
        TlsCallbacks::settle_early(ssl, *this);
    }
    if (result == 1)
    {
        if (SSL_in_early_data(ssl) == 0)
        {
            return established();
        }
        // A client sent its ClientHello with early data allowed (send_early until its server answers); a server took
        // its client's early data, after the checks of a finished handshake (its pins hold before a byte reaches the
        // caller), and the handshake goes on through read_early().
        if (m_settings.role == TlsRole::Client)
        {
            m_early = TlsEarlyData::Offered;
            return TlsError::None;
        }
        const TlsError checked = TlsCallbacks::complete(ssl, *this);
        return checked != TlsError::None ? fail(checked) : TlsError::None;
    }
    switch (SSL_get_error(ssl, result))
    {
    case SSL_ERROR_WANT_READ:
        return TlsError::None;
    case SSL_ERROR_EARLY_DATA_REJECTED:
        // The server refused the client's early data: what it sent under it is gone, and the handshake goes on as one
        // that offered none (the provider forgets every value it predicted).
        m_early = TlsEarlyData::Rejected;
        SSL_reset_early_data_reject(ssl);
        return advance();
    case SSL_ERROR_ZERO_RETURN:
        // The peer's close_notify, after a user_canceled or not: it ended the handshake without a protocol failure.
        m_state = TlsState::Closed;
        return TlsError::Closed;
    default:
        if (TlsCallbacks::ech_rejected(ssl, *this))
        {
            return fail(TlsError::EchRejected);
        }
        return fail(m_refused ? TlsError::PeerRefused : TlsError::Failed);
    }
}

TlsError TlsSession::established() noexcept
{
    SSL* ssl = ssl_of(m_session);
    const TlsError completed = TlsCallbacks::complete(ssl, *this);
    if (completed != TlsError::None)
    {
        return fail(completed);
    }
    TlsCallbacks::settle_early(ssl, *this);
    const u64 limit = tls_key_usage_limit(m_aead);
    const u64 configured = m_settings.key_update_records;
    m_key_update_records = configured != 0U && configured < limit ? configured : limit;
    m_state = TlsState::Established;
    // A server's ticket waits in the provider until the server writes: it goes out with the handshake instead.
    if (m_settings.role == TlsRole::Server)
    {
        flush();
    }
    return TlsError::None;
}

TlsError TlsSession::receive(cont::ConstSpan<u8> bytes) noexcept
{
    if (m_state == TlsState::Idle || m_state == TlsState::Failed)
    {
        return TlsError::NotReady;
    }
    if (m_state == TlsState::Closed)
    {
        return TlsError::Closed;
    }
    const ErrorScope errors;
    BIO*             incoming = SSL_get_rbio(ssl_of(m_session));
    if (bytes.size() > kTlsMaxBufferedBytes - BIO_pending(incoming))
    {
        return TlsError::Full;
    }
    if (!bytes.empty() && BIO_write(incoming, bytes.data(), static_cast<int>(bytes.size())) !=
                              static_cast<int>(bytes.size()))
    {
        return fail(TlsError::ProviderFailure);
    }
    // A server reading its client's early data goes on through read_early(): the provider's handshake returns at once
    // while early bytes wait.
    const bool early_reading = m_settings.role == TlsRole::Server && m_early == TlsEarlyData::Accepted;
    return m_state == TlsState::Handshaking && !early_reading ? advance() : TlsError::None;
}

usize TlsSession::pending() const noexcept
{
    return m_session != nullptr ? BIO_pending(SSL_get_wbio(ssl_of(m_session))) : 0U;
}

TlsError TlsSession::take(cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    if (m_session == nullptr)
    {
        return TlsError::NotReady;
    }
    const usize count = std::min(out.size(), pending());
    if (count == 0U)
    {
        return TlsError::None;
    }
    const int read = BIO_read(SSL_get_wbio(ssl_of(m_session)), out.data(), static_cast<int>(count));
    if (read != static_cast<int>(count))
    {
        return TlsError::ProviderFailure;
    }
    written = count;
    return TlsError::None;
}

TlsError TlsSession::send(cont::ConstSpan<u8> plaintext) noexcept
{
    if (m_state != TlsState::Established)
    {
        return m_state == TlsState::Closed ? TlsError::Closed : TlsError::NotReady;
    }
    if (plaintext.empty())
    {
        return TlsError::None;
    }
    if (plaintext.size() > kTlsMaxBufferedBytes)
    {
        return TlsError::Full;
    }
    // Each record goes under a key with room left for it, a KeyUpdate and an alert; the key updates first when not.
    SSL*        ssl         = ssl_of(m_session);
    const usize records     = (plaintext.size() + kMaxRecordPlaintext - 1U) / kMaxRecordPlaintext;
    const u64   per_key     = m_key_update_records - 2U;
    const u64   used        = records_under_key();
    const u64   on_this_key = per_key > used ? per_key - used : 0U;
    const u64   updates     = records > on_this_key ? 1U + (records - on_this_key - 1U) / per_key : 0U;
    const usize queued      = SSL_get_key_update_type(ssl) != SSL_KEY_UPDATE_NONE ? 1U : 0U;
    const usize needed      = plaintext.size() + records * kRecordOverhead +
                              (static_cast<usize>(updates) + queued) * kKeyUpdateRecordBytes;
    if (needed > kTlsMaxBufferedBytes - std::min(pending(), kTlsMaxBufferedBytes))
    {
        return TlsError::Full;
    }
    const ErrorScope errors;
    if (updates != 0U && m_updates_sent + updates >= m_settings.max_key_updates)
    {
        return close_at_bound();
    }
    if (updates != 0U && check_policy() == TlsError::PolicyRefused)
    {
        return close_refused(); // before any record, so a refused send writes nothing
    }
    for (usize at = 0U; at < plaintext.size(); at += kMaxRecordPlaintext)
    {
        if (records_under_key() >= per_key)
        {
            const TlsError rotated = rekey(false);
            if (rotated != TlsError::None)
            {
                return rotated;
            }
        }
        const int size = static_cast<int>(std::min(kMaxRecordPlaintext, plaintext.size() - at));
        if (SSL_write(ssl, plaintext.data() + at, size) != size)
        {
            return fail(TlsError::Failed);
        }
    }
    return TlsError::None;
}

TlsError TlsSession::send_early(cont::ConstSpan<u8> plaintext) noexcept
{
    if (m_settings.role != TlsRole::Client || m_state != TlsState::Handshaking || m_early != TlsEarlyData::Offered)
    {
        return TlsError::NotReady;
    }
    if (plaintext.empty())
    {
        return TlsError::None;
    }
    const usize records = (plaintext.size() + kMaxRecordPlaintext - 1U) / kMaxRecordPlaintext;
    if (plaintext.size() > early_data_room() || plaintext.size() > kTlsMaxBufferedBytes ||
        plaintext.size() + records * kRecordOverhead > kTlsMaxBufferedBytes - std::min(pending(), kTlsMaxBufferedBytes))
    {
        return TlsError::Full;
    }
    const ErrorScope errors;
    const int size = static_cast<int>(plaintext.size());
    if (SSL_write(ssl_of(m_session), plaintext.data(), size) != size)
    {
        return fail(TlsError::Failed);
    }
    m_early_sent += static_cast<u32>(plaintext.size());
    return TlsError::None;
}

u32 TlsSession::early_data_room() const noexcept
{
    return m_settings.role == TlsRole::Client && m_early == TlsEarlyData::Offered ? m_early_allowance - m_early_sent
                                                                                  : 0U;
}

TlsError TlsSession::read_early(cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    if (m_settings.role != TlsRole::Server || m_early != TlsEarlyData::Accepted)
    {
        return TlsError::NotReady;
    }
    if (m_state == TlsState::Established)
    {
        return TlsError::None;
    }
    if (m_state != TlsState::Handshaking)
    {
        return m_state == TlsState::Closed ? TlsError::Closed : TlsError::NotReady;
    }
    if (out.empty())
    {
        return TlsError::None;
    }
    // A peek runs the provider's read: bytes it returns while the handshake still runs are early data, which a read
    // then takes; a peek that ran the rest of the handshake (the client's early data ended) leaves its 1-RTT bytes
    // pending for read().
    const ErrorScope errors;
    SSL* ssl = ssl_of(m_session);
    const int asked = static_cast<int>(std::min<usize>(out.size(), INT_MAX));
    const int peeked = SSL_peek(ssl, out.data(), asked);
    const int reason = peeked > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl, peeked);
    if (reason == SSL_ERROR_ZERO_RETURN)
    {
        m_state = TlsState::Closed;
        return TlsError::Closed;
    }
    if (reason != SSL_ERROR_NONE && reason != SSL_ERROR_WANT_READ)
    {
        return fail(m_refused ? TlsError::PeerRefused : TlsError::Failed);
    }
    if (SSL_in_init(ssl) == 0)
    {
        return established();
    }
    if (peeked > 0)
    {
        if (SSL_read(ssl, out.data(), peeked) != peeked)
        {
            return fail(TlsError::Failed);
        }
        written = static_cast<usize>(peeked);
    }
    return TlsError::None;
}

u64 TlsSession::records_under_key() const noexcept
{
    return m_state == TlsState::Established ? SSL_get_write_sequence(ssl_of(m_session)) : 0U;
}

void TlsSession::flush() noexcept
{
    // A zero-length write moves what the provider holds back (a queued KeyUpdate, a server's NewSessionTicket) into the
    // outgoing bytes. Its result reads as an error either way.
    (void)SSL_write(ssl_of(m_session), &m_state, 0);
}

bool TlsSession::flush_key_update() noexcept
{
    // Success is the provider no longer holding an update back.
    flush();
    return SSL_get_key_update_type(ssl_of(m_session)) == SSL_KEY_UPDATE_NONE;
}

TlsError TlsSession::rekey(bool request_peer) noexcept
{
    if (m_updates_sent + 1U >= m_settings.max_key_updates)
    {
        return close_at_bound();
    }
    if (check_policy() == TlsError::PolicyRefused)
    {
        return close_refused();
    }
    SSL* ssl = ssl_of(m_session);
    // A queued answer to the peer goes out first, or the provider would take this update for it.
    if (SSL_get_key_update_type(ssl) != SSL_KEY_UPDATE_NONE && !flush_key_update())
    {
        return fail(TlsError::Failed);
    }
    const int request = request_peer && !m_update_requested ? SSL_KEY_UPDATE_REQUESTED : SSL_KEY_UPDATE_NOT_REQUESTED;
    if (!SSL_key_update(ssl, request) || !flush_key_update())
    {
        return fail(TlsError::Failed);
    }
    return TlsError::None;
}

TlsError TlsSession::close_at_bound() noexcept
{
    // The last permitted key update is kept for a peer's request, which the provider answers by itself: the session
    // ends cleanly rather than update past the bound (RFC 9846 section 4.7.3).
    if (SSL_shutdown(ssl_of(m_session)) < 0)
    {
        return fail(TlsError::Failed);
    }
    m_state = TlsState::Closed;
    return TlsError::Closed;
}

TlsError TlsSession::close_refused() noexcept
{
    // Decision S20: the policy now refuses what the session runs on. It ends cleanly at the key update rather than
    // carry on under new keys of a refused algorithm.
    if (SSL_shutdown(ssl_of(m_session)) < 0)
    {
        return fail(TlsError::Failed);
    }
    m_state = TlsState::Closed;
    return TlsError::PolicyRefused;
}

TlsError TlsSession::update_keys(bool request_peer) noexcept
{
    if (m_state != TlsState::Established)
    {
        return m_state == TlsState::Closed ? TlsError::Closed : TlsError::NotReady;
    }
    // Room for a queued answer to the peer and this update.
    if (2U * kKeyUpdateRecordBytes > kTlsMaxBufferedBytes - std::min(pending(), kTlsMaxBufferedBytes))
    {
        return TlsError::Full;
    }
    const ErrorScope errors;
    return rekey(request_peer);
}

TlsError TlsSession::read(cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    if (m_state != TlsState::Established)
    {
        return m_state == TlsState::Closed ? TlsError::Closed : TlsError::NotReady;
    }
    if (out.empty())
    {
        return TlsError::None;
    }
    const ErrorScope errors;
    SSL*             ssl      = ssl_of(m_session);
    const u64        received = m_updates_received;
    const int        result   = SSL_read(ssl, out.data(), static_cast<int>(std::min<usize>(out.size(), INT_MAX)));
    const int        reason   = result > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl, result);
    if (reason != SSL_ERROR_NONE && reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_ZERO_RETURN)
    {
        return fail(TlsError::Failed);
    }
    written = result > 0 ? static_cast<usize>(result) : 0U;
    // The peer updated its key: the session's next key update is the moment its policy is checked again (S20).
    if (m_updates_received != received && reason != SSL_ERROR_ZERO_RETURN &&
        check_policy() == TlsError::PolicyRefused)
    {
        return close_refused();
    }
    // The provider queued its answer to a peer's update request: it goes out now when the outgoing bytes have room for
    // it, else ahead of the next record. An answer that reached max_key_updates ends the session after it.
    if (SSL_get_key_update_type(ssl) != SSL_KEY_UPDATE_NONE &&
        pending() + kKeyUpdateRecordBytes <= kTlsMaxBufferedBytes && !flush_key_update())
    {
        return fail(TlsError::Failed);
    }
    if (reason == SSL_ERROR_ZERO_RETURN)
    {
        m_state = TlsState::Closed;
        return TlsError::Closed;
    }
    if (m_updates_sent >= m_settings.max_key_updates)
    {
        const TlsError closed = close_at_bound();
        return written != 0U && closed == TlsError::Closed ? TlsError::None : closed;
    }
    return TlsError::None;
}

TlsError TlsSession::close() noexcept
{
    if (m_state != TlsState::Established && m_state != TlsState::Closed)
    {
        return TlsError::NotReady;
    }
    SSL* ssl = ssl_of(m_session);
    if ((SSL_get_shutdown(ssl) & SSL_SENT_SHUTDOWN) != 0 || SSL_in_init(ssl))
    {
        return TlsError::Closed;
    }
    const ErrorScope errors;
    // 0: close_notify sent, the peer's not yet read; 1: both sent. Either way nothing more is written.
    if (SSL_shutdown(ssl) < 0)
    {
        return fail(TlsError::Failed);
    }
    m_state = TlsState::Closed;
    return TlsError::None;
}

// ---- QUIC handshakes (RFC 9001) ----

QuicHandshake::~QuicHandshake() noexcept
{
    clear();
}

void QuicHandshake::clear() noexcept
{
    release();
    for (ProviderBuffer*& outgoing : m_outgoing)
    {
        BIO_free(bio_of(outgoing));
        outgoing = nullptr;
    }
    for (auto& level : m_secrets)
    {
        for (Secret& secret : level)
        {
            forget(secret);
        }
    }
    m_remembered.clear();
    m_alert_level = QuicLevel::Initial;
}

void QuicHandshake::forget(Secret& secret) noexcept
{
    secure_zero(cont::Span<u8>(secret.bytes));
    secret = Secret{};
}

usize QuicHandshake::secret_level(QuicLevel level, QuicSide side) noexcept
{
    switch (level)
    {
        case QuicLevel::Handshake:
            return 0U;
        case QuicLevel::Application:
            return 1U;
        case QuicLevel::Early:
            return side == QuicSide::Client ? kEarlySecrets : kNoSecret;
        case QuicLevel::Initial:
            break;
    }
    return kNoSecret;
}

TlsError QuicHandshake::init(const TlsContext& context, cont::ConstSpan<u8> transport_parameters,
                             TlsTicketStore* tickets, cont::ConstSpan<u8> ech_retry_configs,
                             cont::ConstSpan<u8> early_data_context) noexcept
{
    clear();
    const ErrorScope     errors;
    bssl::UniquePtr<SSL> ssl;
    const TlsError       begun =
        TlsCallbacks::begin(context, tickets, ech_retry_configs, early_data_context, true, *this, ssl);
    if (begun != TlsError::None || transport_parameters.empty() ||
        transport_parameters.size() > kQuicMaxTransportParameterBytes)
    {
        clear();
        return begun != TlsError::None ? begun : TlsError::BadConfig;
    }
    for (ProviderBuffer*& outgoing : m_outgoing)
    {
        outgoing = reinterpret_cast<ProviderBuffer*>(BIO_new(BIO_s_mem()));
    }
    // Tickets and the session ID context under the QUIC binding; the provider takes the quic_transport_parameters
    // extension's standard codepoint (57), never the drafts' 0xffa5.
    m_settings.binding = quic_binding_of(m_settings.binding);
    cont::ConstSpan<u8> remembered;
    if (m_outgoing[0] == nullptr || m_outgoing[1] == nullptr || m_outgoing[2] == nullptr ||
        !SSL_set_quic_method(ssl.get(), &kQuicMethod) ||
        !SSL_set_quic_transport_params(ssl.get(), transport_parameters.data(), transport_parameters.size()) ||
        !SSL_set_session_id_context(ssl.get(), m_settings.binding.bytes, sizeof(m_settings.binding.bytes)) ||
        (!early_data_context.empty() &&
         !SSL_set_quic_early_data_context(ssl.get(), early_data_context.data(), early_data_context.size())) ||
        !TlsCallbacks::offer(ssl.get(), *this, remembered))
    {
        clear();
        return TlsError::ProviderFailure;
    }
    m_remembered.resize(remembered.size());
    std::copy(remembered.begin(), remembered.end(), m_remembered.data());
    m_session = reinterpret_cast<ProviderTlsSession*>(ssl.release());
    m_state   = TlsState::Handshaking;
    if (m_settings.role == TlsRole::Server)
    {
        return TlsError::None;
    }
    const TlsError first = TlsCallbacks::first_flight(*this, advance());
    if (first == TlsError::BadConfig)
    {
        clear();
    }
    return first;
}

TlsError QuicHandshake::advance() noexcept
{
    SSL*      ssl    = ssl_of(m_session);
    const int result = SSL_do_handshake(ssl);
    if (m_settings.role == TlsRole::Server)
    {
        TlsCallbacks::settle_early(ssl, *this);
    }
    if (result == 1)
    {
        // Early data: a client sent its ClientHello with its 0-RTT keys ready; a server accepted its client's early
        // data after the checks of a finished handshake. Either way the handshake goes on.
        const bool early = SSL_in_early_data(ssl) == 1;
        if (early && m_settings.role == TlsRole::Client)
        {
            m_early = TlsEarlyData::Offered;
            return TlsError::None;
        }
        const TlsError completed = TlsCallbacks::complete(ssl, *this);
        if (completed != TlsError::None)
        {
            return fail(completed);
        }
        TlsCallbacks::settle_early(ssl, *this);
        if (!early)
        {
            m_state = TlsState::Established;
        }
        return TlsError::None;
    }
    const int reason = SSL_get_error(ssl, result);
    if (reason == SSL_ERROR_WANT_READ)
    {
        return TlsError::None;
    }
    if (reason == SSL_ERROR_EARLY_DATA_REJECTED)
    {
        // The server refused the client's 0-RTT packets: their keys go, and the handshake goes on as one that offered
        // none (the transport sends their data again in 1-RTT packets).
        forget(m_secrets[kEarlySecrets][0]);
        m_early = TlsEarlyData::Rejected;
        SSL_reset_early_data_reject(ssl);
        return advance();
    }
    if (TlsCallbacks::ech_rejected(ssl, *this))
    {
        return fail(TlsError::EchRejected);
    }
    return fail(m_refused ? TlsError::PeerRefused : TlsError::Failed);
}

TlsError QuicHandshake::provide(QuicLevel level, cont::ConstSpan<u8> bytes) noexcept
{
    if (static_cast<usize>(level) >= kQuicLevels)
    {
        return TlsError::BadConfig;
    }
    if (m_state != TlsState::Handshaking && m_state != TlsState::Established)
    {
        return TlsError::NotReady;
    }
    if (bytes.empty())
    {
        return TlsError::None;
    }
    const ErrorScope errors;
    SSL*             ssl = ssl_of(m_session);
    if (!SSL_provide_quic_data(ssl, encryption_level_of(level), bytes.data(), bytes.size()))
    {
        // Past the level's limit nothing was taken; a level the handshake does not read at now is a violation.
        return ERR_GET_REASON(ERR_peek_last_error()) == SSL_R_EXCESSIVE_MESSAGE_SIZE ? TlsError::Full
                                                                                    : fail(TlsError::Failed);
    }
    if (m_state == TlsState::Handshaking)
    {
        return advance();
    }
    // After the handshake only a client's tickets may come (at the Application level); a TLS KeyUpdate, which QUIC
    // forbids (RFC 9001 section 6), and anything else fail the handshake.
    return SSL_process_quic_post_handshake(ssl) == 1 ? TlsError::None
                                                     : fail(m_refused ? TlsError::PeerRefused : TlsError::Failed);
}

usize QuicHandshake::pending(QuicLevel level) const noexcept
{
    const usize index = static_cast<usize>(level);
    return index < kQuicLevels && m_outgoing[index] != nullptr ? BIO_pending(bio_of(m_outgoing[index])) : 0U;
}

TlsError QuicHandshake::take(QuicLevel level, cont::Span<u8> out, usize& written) noexcept
{
    written           = 0U;
    const usize index = static_cast<usize>(level);
    if (index >= kQuicLevels)
    {
        return TlsError::BadConfig;
    }
    if (m_outgoing[index] == nullptr)
    {
        return TlsError::NotReady;
    }
    const usize count = std::min(out.size(), pending(level));
    if (count == 0U)
    {
        return TlsError::None;
    }
    if (BIO_read(bio_of(m_outgoing[index]), out.data(), static_cast<int>(count)) != static_cast<int>(count))
    {
        return TlsError::ProviderFailure;
    }
    written = count;
    return TlsError::None;
}

usize QuicHandshake::receive_limit(QuicLevel level) const noexcept
{
    return m_session != nullptr && static_cast<usize>(level) < kQuicLevels
               ? SSL_quic_max_handshake_flight_len(ssl_of(m_session), encryption_level_of(level))
               : 0U;
}

bool QuicHandshake::has_keys(QuicLevel level, QuicSide side) const noexcept
{
    const usize at = secret_level(level, side);
    return at != kNoSecret && m_secrets[at][side == QuicSide::Client ? 0U : 1U].size != 0U;
}

TlsError QuicHandshake::derive_keys(QuicLevel level, QuicSide side, QuicVersion version, QuicPacketKey& packet,
                                    QuicHeaderKey& header) noexcept
{
    const usize at = secret_level(level, side);
    if ((at == kNoSecret && level != QuicLevel::Early) || !quic_version_supported(static_cast<u32>(version)))
    {
        return TlsError::BadConfig;
    }
    if (!has_keys(level, side))
    {
        return TlsError::NotReady;
    }
    Secret&                   secret = m_secrets[at][side == QuicSide::Client ? 0U : 1U];
    const cont::ConstSpan<u8> bytes(secret.bytes, secret.size);
    if (packet.init(version, secret.aead, bytes) != CryptoError::None ||
        header.init(version, secret.aead, bytes) != CryptoError::None)
    {
        packet.clear();
        header.clear();
        return TlsError::ProviderFailure;
    }
    forget(secret);
    return TlsError::None;
}

cont::ConstSpan<u8> QuicHandshake::remembered_transport_parameters() const noexcept
{
    return cont::ConstSpan<u8>(m_remembered.data(), m_remembered.size());
}

cont::ConstSpan<u8> QuicHandshake::peer_transport_parameters() const noexcept
{
    if (m_session == nullptr)
    {
        return {};
    }
    const uint8_t* parameters = nullptr;
    size_t         length     = 0U;
    SSL_get_peer_quic_transport_params(ssl_of(m_session), &parameters, &length);
    return cont::ConstSpan<u8>(parameters, length);
}
} // namespace crd::crypto
