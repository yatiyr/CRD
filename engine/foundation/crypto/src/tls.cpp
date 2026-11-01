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
// rotate inside one process and cannot be shared): each ticket is the key's public name, a random 32-byte salt and the
// session state sealed with AES-256-GCM under a key and nonce derived from the ticket key and that salt (HKDF-SHA256),
// so every derived key seals one ticket and no count of tickets has to be shared across the servers of a fleet.
//
// Certificate compression (decision S10, RFC 8879) runs through the engine's codecs (crd-compression): a server's
// compressed Certificate messages are made once at init() and held in the provider context's ex_data, so its sessions
// copy bytes instead of compressing per handshake; a client decodes into a buffer of exactly the claimed length.
#include <crd/crypto/tls.hpp>

#include "provider_internal.hpp"

#include <crd/compression/deflate.hpp>
#include <crd/compression/zstd.hpp>

#include <openssl/aead.h>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/bytestring.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/nid.h>
#include <openssl/pool.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <iterator>
#include <new>
#include <utility>

namespace crd::crypto
{
using provider_internal::ErrorScope;
using provider_internal::key_of;
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
constexpr usize kTicketHeaderBytes     = kTicketNameBytes + kTicketSaltBytes;
constexpr usize kTicketOverhead        = kTicketHeaderBytes + kAeadTagBytes;
constexpr usize kCertificateEntryOverhead = 9U;   // request context (1), list (3), entry (3), extensions (2)
constexpr usize kMinCertificateBytes   = 4U;      // an empty request context and an empty list
constexpr usize kCompressedOverhead    = 8U;      // algorithm (2), uncompressed_length (3), compressed length (3)
constexpr usize kZlibFrameBytes        = 6U;      // the zlib header (2) and Adler-32 (4) around raw DEFLATE
constexpr int   kCertificateZstdLevel  = 22;      // zstd's highest: a server compresses its Certificate once

constexpr char kTicketNameInfo[] = "cerid tls 1.3 ticket name";
constexpr char kTicketSealInfo[] = "cerid tls 1.3 ticket seal";
constexpr char kBindingLabel[]   = "cerid tls 1.3 binding";

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

template <usize N>
cont::ConstSpan<u8> label_of(const char (&text)[N]) noexcept
{
    return cont::ConstSpan<u8>(reinterpret_cast<const u8*>(text), N - 1U);
}

// A ticket key's public name: HKDF-SHA256 of the key with an empty salt, expanded under its own label.
CryptoError ticket_key_name(cont::ConstSpan<u8> key, cont::Span<u8> name) noexcept
{
    u8          prk[32];
    CryptoError error = hkdf_extract(AlgorithmId::HkdfSha256, {}, key, cont::Span<u8>(prk));
    if (error == CryptoError::None)
    {
        error = hkdf_expand(AlgorithmId::HkdfSha256, cont::ConstSpan<u8>(prk), label_of(kTicketNameInfo), name);
    }
    secure_zero(cont::Span<u8>(prk));
    return error;
}

// The AES-256-GCM key and nonce that seal one ticket: HKDF-SHA256 of the ticket key salted with the ticket's salt.
CryptoError ticket_aead(cont::ConstSpan<u8> key, cont::ConstSpan<u8> salt, AeadKey& aead,
                        u8 (&nonce)[kAeadNonceBytes]) noexcept
{
    u8          prk[32];
    u8          okm[kTlsTicketKeyBytes + kAeadNonceBytes];
    CryptoError error = hkdf_extract(AlgorithmId::HkdfSha256, salt, key, cont::Span<u8>(prk));
    if (error == CryptoError::None)
    {
        error = hkdf_expand(AlgorithmId::HkdfSha256, cont::ConstSpan<u8>(prk), label_of(kTicketSealInfo),
                            cont::Span<u8>(okm));
    }
    if (error == CryptoError::None)
    {
        error = aead.init(AlgorithmId::Aes256Gcm, cont::ConstSpan<u8>(okm, kTlsTicketKeyBytes));
    }
    std::memcpy(nonce, okm + kTlsTicketKeyBytes, kAeadNonceBytes);
    secure_zero(cont::Span<u8>(prk));
    secure_zero(cont::Span<u8>(okm));
    return error;
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

void zero_ticket_keys(TlsSettings& settings) noexcept
{
    secure_zero(cont::Span<u8>(&settings.ticket_keys[0][0], sizeof(settings.ticket_keys)));
}

// Offers a ticket the store handed back when it parses as a TLS 1.3 session of `binding` (the provider then also drops
// one that expired); any other is dropped and the handshake runs in full. False only when the provider fails.
bool offer_ticket(SSL* ssl, const TlsBinding& binding, cont::ConstSpan<u8> ticket) noexcept
{
    if (ticket.empty())
    {
        return true;
    }
    const bssl::UniquePtr<SSL_SESSION> session(
        SSL_SESSION_from_bytes(ticket.data(), ticket.size(), SSL_get_SSL_CTX(ssl)));
    unsigned       length  = 0U;
    const uint8_t* context = session ? SSL_SESSION_get0_id_context(session.get(), &length) : nullptr;
    if (!session || SSL_SESSION_get_protocol_version(session.get()) != TLS1_3_VERSION ||
        length != sizeof(binding.bytes) || std::memcmp(context, binding.bytes, length) != 0)
    {
        return true;
    }
    return SSL_set_session(ssl, session.get()) == 1;
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
    static TlsSession* session_of(const SSL* ssl) noexcept
    {
        return static_cast<TlsSession*>(SSL_get_ex_data(ssl, session_slot()));
    }

    // Takes the peer's key from its leaf certificate when the key is of an accepted algorithm and pinned: during a
    // full handshake (verify_peer) and after a resumed one, whose ticket carried the certificate.
    static bool pinned_peer(const SSL* ssl, TlsSession& session) noexcept
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
        TlsSession* session = session_of(ssl);
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

    static ssl_verify_result_t refuse(TlsSession& session, uint8_t* out_alert, u8 alert) noexcept
    {
        session.m_refused = true;
        *out_alert        = alert;
        return ssl_verify_invalid;
    }

    // A server with a name refuses a client that names another server or none, with general_error: the alert does not
    // tell the client which names the server holds.
    static int check_name(SSL* ssl, int* out_alert, void*) noexcept
    {
        TlsSession* session = session_of(ssl);
        if (session == nullptr)
        {
            *out_alert = SSL_AD_INTERNAL_ERROR;
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        const TlsSettings& settings = session->m_settings;
        if (settings.name_bytes == 0U)
        {
            return SSL_TLSEXT_ERR_OK;
        }
        const char* offered = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
        if (offered == nullptr ||
            !same_host_name(cont::StringView(offered), cont::StringView(settings.server_name, settings.name_bytes)))
        {
            session->m_refused = true;
            *out_alert         = kTlsAlertGeneralError;
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        return SSL_TLSEXT_ERR_OK;
    }

    // A client hands each ticket its server issues to the session's store, serialized; without a store the ticket is
    // ignored. The provider keeps its session object (0: no reference taken).
    static int keep_ticket(SSL* ssl, SSL_SESSION* ticket) noexcept
    {
        const TlsSession* session = session_of(ssl);
        if (session == nullptr || session->m_tickets == nullptr)
        {
            return 0;
        }
        uint8_t* bytes = nullptr;
        size_t   size  = 0U;
        if (SSL_SESSION_to_bytes(ticket, &bytes, &size) == 1)
        {
            session->m_tickets->keep(session->m_settings.binding, cont::ConstSpan<u8>(bytes, size));
            secure_zero(cont::Span<u8>(bytes, size));
            OPENSSL_free(bytes);
        }
        return 0;
    }

    static size_t ticket_overhead(SSL*) noexcept
    {
        return kTicketOverhead;
    }

    // Seals `in` (the session state) under the sealing key: the key's name, the salt, then the ciphertext and tag. The
    // name only selects the key: any other name selects another key or none, and the salt derives the sealing key.
    // `in` may be `out`, so the state moves into place before anything is written.
    static int seal_ticket(SSL* ssl, uint8_t* out, size_t* out_len, size_t max_out_len, const uint8_t* in,
                           size_t in_len) noexcept
    {
        const TlsSession* session = session_of(ssl);
        if (session == nullptr || session->m_settings.ticket_key_count == 0U || max_out_len < kTicketOverhead ||
            in_len > max_out_len - kTicketOverhead)
        {
            return 0;
        }
        const TlsSettings&        settings = session->m_settings;
        u8* const                 body     = out + kTicketHeaderBytes;
        const cont::ConstSpan<u8> salt(out + kTicketNameBytes, kTicketSaltBytes);
        std::memmove(body, in, in_len);
        std::memcpy(out, settings.ticket_names[0], kTicketNameBytes);
        AeadKey key;
        u8      nonce[kAeadNonceBytes];
        if (random_bytes(cont::Span<u8>(out + kTicketNameBytes, kTicketSaltBytes)) != CryptoError::None ||
            ticket_aead(cont::ConstSpan<u8>(settings.ticket_keys[0]), salt, key, nonce) != CryptoError::None ||
            key.seal(cont::ConstSpan<u8>(nonce), cont::ConstSpan<u8>(body, in_len), {},
                     cont::Span<u8>(body, in_len + kAeadTagBytes)) != CryptoError::None)
        {
            return 0;
        }
        *out_len = in_len + kTicketOverhead;
        return 1;
    }

    // Opens a ticket sealed under the sealing key or the previous one. A ticket too short, under another key or forged
    // is ignored (the handshake runs in full); only a provider failure fails the handshake.
    static ssl_ticket_aead_result_t open_ticket(SSL* ssl, uint8_t* out, size_t* out_len, size_t max_out_len,
                                                const uint8_t* in, size_t in_len) noexcept
    {
        const TlsSession* session = session_of(ssl);
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
        *out_len = sealed - kAeadTagBytes;
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

    // Records the first alert each way, notices a HelloRetryRequest (a ServerHello with the special random), counts
    // the KeyUpdates each way and notes the algorithm of a compressed Certificate sent or received. The provider
    // reports a KeyUpdate it writes when it queues it, sealed under the old key.
    static void observe(int is_write, int, int content_type, const void* buffer, size_t length, SSL* ssl,
                        void*) noexcept
    {
        TlsSession* session = session_of(ssl);
        if (session == nullptr)
        {
            return;
        }
        const auto* bytes = static_cast<const u8*>(buffer);
        if (content_type == SSL3_RT_ALERT && length >= 2U)
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
};

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
    case TlsError::ProviderFailure:
        return "provider-failure";
    }
    return "unknown";
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

TlsCredential::~TlsCredential() noexcept
{
    clear();
}

void TlsCredential::clear() noexcept
{
    X509_free(x509_of(m_certificate));
    m_certificate = nullptr;
    m_key.reset();
    m_key_id = KeyId{};
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
    const ErrorScope  errors;
    const AlgorithmId algorithm = key.algorithm();
    EVP_PKEY*         pkey      = pkey_of(key.m_key.get());
    u8                public_key[kMaxSignaturePublicKey];
    const cont::Span<u8> bytes(public_key, signature_sizes(algorithm).public_key);
    if (provider_internal::signature_public_bytes(pkey, algorithm, bytes) != CryptoError::None)
    {
        return TlsError::ProviderFailure;
    }
    const KeyId id = key_id_of(bytes);

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
                           X509V3_ADD_DEFAULT) ||
        !X509_sign(certificate.get(), pkey, provider_internal::signature_digest(algorithm)))
    {
        return TlsError::ProviderFailure;
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
        config.ticket_lifetime > kTlsMaxTicketLifetime || !valid_compression(config.certificate_compression))
    {
        return TlsError::BadConfig;
    }

    TlsSettings settings;
    settings.role = config.role;
    std::copy(config.signatures.begin(), config.signatures.end(), settings.signatures);
    settings.signature_count = static_cast<u32>(config.signatures.size());
    std::copy(config.pinned.begin(), config.pinned.end(), settings.pinned);
    settings.pin_count = static_cast<u32>(config.pinned.size());
    std::copy(config.server_name.begin(), config.server_name.end(), settings.server_name);
    settings.name_bytes         = static_cast<u32>(config.server_name.size());
    settings.key_update_records = config.key_update_records;
    settings.max_key_updates    = config.max_key_updates != 0U ? config.max_key_updates : kTlsMaxKeyUpdates;
    settings.binding = binding_of(config, config.credential != nullptr ? config.credential->key_id() : KeyId{});
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
    // "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256" at most (75 bytes).
    char  suites[96] = {};
    usize length     = 0U;
    for (const AlgorithmId aead : config.aeads)
    {
        const CipherSuite* suite = cipher_suite_of(aead);
        if (suite == nullptr)
        {
            return TlsError::BadConfig;
        }
        const cont::StringView name = suite->name;
        if (length != 0U)
        {
            suites[length++] = ':';
        }
        std::memcpy(suites + length, name.data(), name.size());
        length += name.size();
    }

    const ErrorScope          errors;
    bssl::UniquePtr<SSL_CTX> context(SSL_CTX_new(TLS_method()));
    if (!context || !SSL_CTX_set_min_proto_version(context.get(), TLS1_3_VERSION) ||
        !SSL_CTX_set_max_proto_version(context.get(), TLS1_3_VERSION) ||
        !SSL_CTX_set1_group_ids(context.get(), groups, config.groups.size()) ||
        !SSL_CTX_set_ciphersuites(context.get(), suites))
    {
        return TlsError::ProviderFailure;
    }
    // Server preference orders the groups (the provider's TLS 1.3 AEAD choice follows the client, see tls.hpp).
    SSL_CTX_set_options(context.get(), SSL_OP_CIPHER_SERVER_PREFERENCE);
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
        const u16 scheme = tls_signature_codepoint(config.credential->algorithm());
        if (!SSL_CTX_use_certificate(context.get(), x509_of(config.credential->m_certificate)) ||
            !SSL_CTX_use_PrivateKey(context.get(), pkey_of(config.credential->m_key.get())) ||
            !SSL_CTX_set_signing_algorithm_prefs(context.get(), &scheme, 1U))
        {
            return TlsError::ProviderFailure;
        }
    }
    if (server)
    {
        SSL_CTX_set_tlsext_servername_callback(context.get(), &TlsCallbacks::check_name);
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
    SSL_CTX_set_msg_callback(context.get(), &TlsCallbacks::observe);
    if (session_slot() < 0)
    {
        return TlsError::ProviderFailure;
    }
    m_context  = reinterpret_cast<ProviderTlsContext*>(context.release());
    m_settings = settings;
    return TlsError::None;
}

// ---- Sessions ----

TlsSession::~TlsSession() noexcept
{
    clear();
}

void TlsSession::clear() noexcept
{
    // SSL_free releases the connection, its keys (zeroed) and both memory buffers.
    SSL_free(ssl_of(m_session));
    m_session = nullptr;
    m_tickets = nullptr;
    zero_ticket_keys(m_settings);
    m_settings           = TlsSettings{};
    m_state              = TlsState::Idle;
    m_group              = AlgorithmId{};
    m_aead               = AlgorithmId{};
    m_peer_algorithm     = AlgorithmId{};
    m_peer_key           = KeyId{};
    m_key_update_records = 0U;
    m_updates_sent       = 0U;
    m_updates_received   = 0U;
    m_certificate_compression = 0U;
    m_hello_retry        = false;
    m_resumed            = false;
    m_refused            = false;
    m_update_requested   = false;
    m_alert_sent         = TlsAlert{};
    m_alert_received     = TlsAlert{};
}

TlsError TlsSession::init(const TlsContext& context, TlsTicketStore* tickets) noexcept
{
    clear();
    if (!context.ready())
    {
        return TlsError::NotReady;
    }
    if (tickets != nullptr && context.m_settings.role == TlsRole::Server)
    {
        return TlsError::BadConfig;
    }
    const ErrorScope     errors;
    bssl::UniquePtr<SSL> ssl(SSL_new(ctx_of(context.m_context)));
    bssl::UniquePtr<BIO> incoming(BIO_new(BIO_s_mem()));
    bssl::UniquePtr<BIO> outgoing(BIO_new(BIO_s_mem()));
    if (!ssl || !incoming || !outgoing || !SSL_set_ex_data(ssl.get(), session_slot(), this))
    {
        return TlsError::ProviderFailure;
    }
    // An empty incoming buffer means "more bytes later", never the end of the stream.
    BIO_set_mem_eof_return(incoming.get(), -1);
    SSL_set_bio(ssl.get(), incoming.release(), outgoing.release());
    m_settings = context.m_settings;
    if (m_settings.role == TlsRole::Client)
    {
        SSL_set_connect_state(ssl.get());
        if (!SSL_set_tlsext_host_name(ssl.get(), m_settings.server_name) ||
            (tickets != nullptr && !offer_ticket(ssl.get(), m_settings.binding, tickets->take(m_settings.binding))))
        {
            return TlsError::ProviderFailure;
        }
        m_tickets = tickets;
    }
    else
    {
        SSL_set_accept_state(ssl.get());
    }
    m_session = reinterpret_cast<ProviderTlsSession*>(ssl.release());
    m_state   = TlsState::Handshaking;
    return m_settings.role == TlsRole::Client ? advance() : TlsError::None;
}

TlsError TlsSession::fail(TlsError error) noexcept
{
    m_state = TlsState::Failed;
    return error;
}

TlsError TlsSession::advance() noexcept
{
    SSL*      ssl    = ssl_of(m_session);
    const int result = SSL_do_handshake(ssl);
    if (result == 1)
    {
        const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl);
        m_group                  = algorithm_of(kGroups, SSL_get_group_id(ssl));
        m_aead                   = AlgorithmId{};
        for (const CipherSuite& suite : kCipherSuites)
        {
            if (cipher != nullptr && SSL_CIPHER_get_cipher_nid(cipher) == suite.nid)
            {
                m_aead = suite.id;
            }
        }
        if (m_group == AlgorithmId{} || m_aead == AlgorithmId{})
        {
            return fail(TlsError::ProviderFailure);
        }
        // A resumed handshake verified no certificate: the peer key is the one the ticket's handshake proved, which
        // the binding (the same pins) keeps pinned.
        m_resumed = SSL_session_reused(ssl) == 1;
        if (m_resumed && m_settings.pin_count != 0U && !TlsCallbacks::pinned_peer(ssl, *this))
        {
            return fail(TlsError::PeerRefused);
        }
        const u64 limit      = tls_key_usage_limit(m_aead);
        const u64 configured = m_settings.key_update_records;
        m_key_update_records = configured != 0U && configured < limit ? configured : limit;
        m_state              = TlsState::Established;
        // A server's ticket waits in the provider until the server writes: it goes out with the handshake instead.
        if (m_settings.role == TlsRole::Server)
        {
            flush();
        }
        return TlsError::None;
    }
    switch (SSL_get_error(ssl, result))
    {
    case SSL_ERROR_WANT_READ:
        return TlsError::None;
    case SSL_ERROR_ZERO_RETURN:
        // The peer's close_notify, after a user_canceled or not: it ended the handshake without a protocol failure.
        m_state = TlsState::Closed;
        return TlsError::Closed;
    default:
        return fail(m_refused ? TlsError::PeerRefused : TlsError::Failed);
    }
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
    return m_state == TlsState::Handshaking ? advance() : TlsError::None;
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
    SSL*             ssl    = ssl_of(m_session);
    const int        result = SSL_read(ssl, out.data(), static_cast<int>(std::min<usize>(out.size(), INT_MAX)));
    const int        reason = result > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl, result);
    if (reason != SSL_ERROR_NONE && reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_ZERO_RETURN)
    {
        return fail(TlsError::Failed);
    }
    written = result > 0 ? static_cast<usize>(result) : 0U;
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
} // namespace crd::crypto
