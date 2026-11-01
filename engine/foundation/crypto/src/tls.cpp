// crd-crypto -- TLS 1.3 sessions over AWS-LC's libssl (NET.2; contract:
// docs/design/network-foundation.md#tls-sessions).
//
// The provider's context and connection objects sit behind the opaque ProviderTlsContext and ProviderTlsSession, and
// the connection reads from and writes to two memory buffers, never a socket. Every configuration is checked against
// the register identifiers before the provider sees it, so a provider refusal past those checks is a provider failure.
// The provider's callbacks (peer verification, the server name, the message observer) reach their session through the
// connection's ex_data slot and touch only the session's own copy of the settings.
#include <crd/crypto/tls.hpp>

#include "provider_internal.hpp"

#include <openssl/aead.h>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/nid.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <climits>
#include <cstring>
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
constexpr usize kKeyUpdateRequestAt    = 4U;      // type (1), length (3), then request_update
constexpr usize kServerHelloRandomAt   = 6U;      // type (1), length (3), legacy_version (2)
constexpr usize kMaxRecordPlaintext    = 16384U;  // 2^14 (RFC 9846 section 5.1)
constexpr usize kRecordOverhead        = 22U;     // header (5), inner content type (1) and AEAD tag (16)
constexpr usize kKeyUpdateRecordBytes  = 27U;     // the overhead, a handshake header (4) and request_update (1)
constexpr usize kMaxSignaturePublicKey = 2592U;   // ML-DSA-87's, the longest signature public key

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

// The connection's slot for its session: one index for the process, taken on first use.
int session_slot() noexcept
{
    static const int kSlot = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return kSlot;
}
} // namespace

// The provider's callbacks, with access to the session they serve.
struct TlsCallbacks
{
    static TlsSession* session_of(const SSL* ssl) noexcept
    {
        return static_cast<TlsSession*>(SSL_get_ex_data(ssl, session_slot()));
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
        // A server does not tell an unauthenticated client whether a key is pinned (the TLS 1.3 profile).
        const u8 refusal = session->m_settings.role == TlsRole::Server ? kTlsAlertGeneralError : kBadCertificate;
        const STACK_OF(CRYPTO_BUFFER)* chain = SSL_get0_peer_certificates(ssl);
        if (chain == nullptr || sk_CRYPTO_BUFFER_num(chain) == 0U)
        {
            return refuse(*session, out_alert, refusal);
        }
        const bssl::UniquePtr<X509> leaf(X509_parse_from_buffer(sk_CRYPTO_BUFFER_value(chain, 0U)));
        const EVP_PKEY*             pkey = leaf ? X509_get0_pubkey(leaf.get()) : nullptr;
        const AlgorithmId algorithm = pkey != nullptr ? provider_internal::signature_algorithm_of(pkey) : AlgorithmId{};
        const TlsSettings& settings = session->m_settings;
        const AlgorithmId* accepted_end = settings.signatures + settings.signature_count;
        if (algorithm == AlgorithmId{} || std::find(settings.signatures, accepted_end, algorithm) == accepted_end)
        {
            return refuse(*session, out_alert, refusal);
        }
        u8                   public_key[kMaxSignaturePublicKey];
        const cont::Span<u8> bytes(public_key, signature_sizes(algorithm).public_key);
        if (provider_internal::signature_public_bytes(pkey, algorithm, bytes) != CryptoError::None)
        {
            return refuse(*session, out_alert, refusal);
        }
        const KeyId  key      = key_id_of(bytes);
        const KeyId* pins_end = settings.pinned + settings.pin_count;
        if (std::find(settings.pinned, pins_end, key) == pins_end)
        {
            return refuse(*session, out_alert, refusal);
        }
        session->m_peer_algorithm = algorithm;
        session->m_peer_key       = key;
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

    // Records the first alert each way, notices a HelloRetryRequest (a ServerHello with the special random) and counts
    // the KeyUpdates each way. The provider reports a KeyUpdate it writes when it queues it, sealed under the old key.
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
    // well-defined expiration date (RFC 5280 section 4.1.2.5).
    const auto*  name_bytes = reinterpret_cast<const unsigned char*>(name.data());
    const auto   name_size  = static_cast<int>(name.size());
    X509_NAME*   subject    = X509_get_subject_name(certificate.get());
    if (!X509_set_version(certificate.get(), X509_VERSION_3) ||
        !X509_set_serialNumber(certificate.get(), serial_field.get()) ||
        !X509_NAME_add_entry_by_NID(subject, NID_commonName, MBSTRING_ASC, name_bytes, name_size, -1, 0) ||
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
        !X509_add1_ext_i2d(certificate.get(), NID_subject_alt_name, alt_names.get(), 0, X509V3_ADD_DEFAULT) ||
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
    m_context  = nullptr;
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
        config.max_key_updates > kTlsMaxKeyUpdates)
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
    // Resumption and tickets are a later slice of NET.2; until then every connection runs a full handshake. Server
    // preference orders the groups (the provider's TLS 1.3 AEAD choice follows the client, see tls.hpp).
    SSL_CTX_set_options(context.get(), SSL_OP_NO_TICKET | SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set_session_cache_mode(context.get(), SSL_SESS_CACHE_OFF);
    // GREASE (RFC 8701, ietf.grease) in every build: a client's hello carries reserved values, chosen per connection,
    // in its cipher suites, supported groups, supported versions, key shares and two extensions, so a peer or
    // middlebox that refuses an unknown value fails in testing rather than in the field.
    SSL_CTX_set_grease_enabled(context.get(), 1);
    if (!SSL_CTX_set_num_tickets(context.get(), 0U))
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
    m_session            = nullptr;
    m_settings           = TlsSettings{};
    m_state              = TlsState::Idle;
    m_group              = AlgorithmId{};
    m_aead               = AlgorithmId{};
    m_peer_algorithm     = AlgorithmId{};
    m_peer_key           = KeyId{};
    m_key_update_records = 0U;
    m_updates_sent       = 0U;
    m_updates_received   = 0U;
    m_hello_retry        = false;
    m_refused            = false;
    m_update_requested   = false;
    m_alert_sent         = TlsAlert{};
    m_alert_received     = TlsAlert{};
}

TlsError TlsSession::init(const TlsContext& context) noexcept
{
    clear();
    if (!context.ready())
    {
        return TlsError::NotReady;
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
        if (!SSL_set_tlsext_host_name(ssl.get(), m_settings.server_name))
        {
            return TlsError::ProviderFailure;
        }
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
        const u64 limit      = tls_key_usage_limit(m_aead);
        const u64 configured = m_settings.key_update_records;
        m_key_update_records = configured != 0U && configured < limit ? configured : limit;
        m_state              = TlsState::Established;
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

bool TlsSession::flush_key_update() noexcept
{
    // A zero-length write moves the queued KeyUpdate into the outgoing bytes. Its result reads as an error either way,
    // so success is the provider no longer holding an update back.
    SSL* ssl = ssl_of(m_session);
    (void)SSL_write(ssl, &m_state, 0);
    return SSL_get_key_update_type(ssl) == SSL_KEY_UPDATE_NONE;
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
