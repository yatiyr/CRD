// crd-crypto -- the provider interface over AWS-LC (NET.2; contract: docs/design/network-foundation.md#provider).
//
// The only unit that includes the provider's headers (the crypto.providers signatures refuse them elsewhere). Every
// public entry point checks the algorithm's class and every length before the provider sees a byte, so a provider
// refusal past those checks means a bad key or a forged message, and the provider's thread-local error queue is
// cleared on every refusal so nothing accumulates across calls.
#include <crd/crypto/provider.hpp>

#include "byte_spans.hpp"
#include "provider_internal.hpp"

#include <openssl/aead.h>
#include <openssl/base.h>
#include <openssl/bn.h>
#include <openssl/curve25519.h>
#include <openssl/digest.h>
#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/ecdh.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/experimental/kem_deterministic_api.h>
#include <openssl/hkdf.h>
#include <openssl/hmac.h>
#include <openssl/mem.h>
#include <openssl/nid.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/siphash.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace crd::crypto
{
static_assert(sizeof(EVP_AEAD_CTX) <= kAeadStateBytes && alignof(EVP_AEAD_CTX) <= 16U,
              "kAeadStateBytes no longer holds the provider's AEAD context");
static_assert(sizeof(KeyId::bytes) == SHA256_DIGEST_LENGTH);

using detail::overlaps;
using detail::refuse;
using provider_internal::ErrorScope;
using provider_internal::key_of;
using provider_internal::pkey_of;
using provider_internal::signature_digest;

namespace
{
// The algorithms the provider implements, in identifier order. The hybrid key exchange groups run inside its TLS
// stack (tls.hpp), proven against its own key shares and OpenSSL's (decision P6).
constexpr AlgorithmId kImplemented[] = {
    AlgorithmId::X25519Mlkem768,   AlgorithmId::Secp256r1Mlkem768, AlgorithmId::Secp384r1Mlkem1024,
    AlgorithmId::X25519,           AlgorithmId::Secp256r1, AlgorithmId::Secp384r1,
    AlgorithmId::MlDsa44,          AlgorithmId::MlDsa65,   AlgorithmId::MlDsa87,
    AlgorithmId::Ed25519,          AlgorithmId::EcdsaSecp256r1Sha256,
    AlgorithmId::EcdsaSecp384r1Sha384,
    AlgorithmId::Aes128Gcm,        AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305,
    AlgorithmId::HkdfSha256,       AlgorithmId::HkdfSha384,
    AlgorithmId::MlKem768,         AlgorithmId::MlKem1024,
    AlgorithmId::Siphash24,
};

struct SignatureAlgorithm
{
    AlgorithmId    id;
    int            type; // the provider's key type
    int            nid;  // the ML-DSA parameter set or the curve
    SignatureSizes sizes;
};

constexpr SignatureAlgorithm kSignatures[] = {
    {AlgorithmId::MlDsa44, EVP_PKEY_PQDSA, NID_MLDSA44, {1312U, 32U, 2420U}},
    {AlgorithmId::MlDsa65, EVP_PKEY_PQDSA, NID_MLDSA65, {1952U, 32U, 3309U}},
    {AlgorithmId::MlDsa87, EVP_PKEY_PQDSA, NID_MLDSA87, {2592U, 32U, 4627U}},
    {AlgorithmId::Ed25519, EVP_PKEY_ED25519, NID_ED25519, {32U, 32U, 64U}},
    {AlgorithmId::EcdsaSecp256r1Sha256, EVP_PKEY_EC, NID_X9_62_prime256v1, {65U, 32U, 72U}},
    {AlgorithmId::EcdsaSecp384r1Sha384, EVP_PKEY_EC, NID_secp384r1, {97U, 48U, 104U}},
};

struct AgreementAlgorithm
{
    AlgorithmId    id;
    int            nid; // 0 for X25519, else the curve
    AgreementSizes sizes;
};

constexpr AgreementAlgorithm kAgreements[] = {
    {AlgorithmId::X25519, 0, {32U, 32U, 32U}},
    {AlgorithmId::Secp256r1, NID_X9_62_prime256v1, {65U, 32U, 32U}},
    {AlgorithmId::Secp384r1, NID_secp384r1, {97U, 48U, 48U}},
};

struct KemAlgorithm
{
    AlgorithmId id;
    int         nid; // the ML-KEM parameter set
    KemSizes    sizes;
};

constexpr KemAlgorithm kKems[] = {
    {AlgorithmId::MlKem768, NID_MLKEM768, {1184U, 64U, 1088U, 32U}},
    {AlgorithmId::MlKem1024, NID_MLKEM1024, {1568U, 64U, 1568U, 32U}},
};

// The entry of `table` for `algorithm`; null when the table's class does not hold it.
template <typename Entry, usize N> const Entry* find_entry(const Entry (&table)[N], AlgorithmId algorithm) noexcept
{
    for (const Entry& entry : table)
    {
        if (entry.id == algorithm)
        {
            return &entry;
        }
    }
    return nullptr;
}

const EVP_AEAD* find_aead(AlgorithmId algorithm) noexcept
{
    switch (algorithm)
    {
    case AlgorithmId::Aes128Gcm:
        return EVP_aead_aes_128_gcm();
    case AlgorithmId::Aes256Gcm:
        return EVP_aead_aes_256_gcm();
    case AlgorithmId::Chacha20Poly1305:
        return EVP_aead_chacha20_poly1305();
    default:
        return nullptr;
    }
}

const EVP_MD* hkdf_digest(AlgorithmId algorithm) noexcept
{
    switch (algorithm)
    {
    case AlgorithmId::HkdfSha256:
        return EVP_sha256();
    case AlgorithmId::HkdfSha384:
        return EVP_sha384();
    default:
        return nullptr;
    }
}

// Whether an AEAD output aliases its inputs in a way the provider does not allow: only the text may share the
// output's bytes, and then from the same first byte (in place).
bool bad_aead_alias(cont::ConstSpan<u8> out, cont::ConstSpan<u8> text, cont::ConstSpan<u8> associated,
                    cont::ConstSpan<u8> nonce) noexcept
{
    return (overlaps(out, text) && out.data() != text.data()) || overlaps(out, associated) || overlaps(out, nonce);
}

// An EVP_PKEY holding its own reference to `key`.
bssl::UniquePtr<EVP_PKEY> ec_pkey(EC_KEY* key) noexcept
{
    bssl::UniquePtr<EVP_PKEY> pkey(EVP_PKEY_new());
    if (!pkey || !EVP_PKEY_set1_EC_KEY(pkey.get(), key))
    {
        return nullptr;
    }
    return pkey;
}

// An EC key whose private scalar is `scalar` (big-endian, exactly the curve's length), with its public point.
// Refuses a scalar outside [1, n - 1].
bssl::UniquePtr<EVP_PKEY> ec_private_key(int curve, cont::ConstSpan<u8> scalar) noexcept
{
    bssl::UniquePtr<EC_KEY> key(EC_KEY_new_by_curve_name(curve));
    bssl::UniquePtr<BIGNUM> secret(BN_bin2bn(scalar.data(), scalar.size(), nullptr));
    if (!key || !secret)
    {
        return nullptr;
    }
    const EC_GROUP*           group = EC_KEY_get0_group(key.get());
    bssl::UniquePtr<EC_POINT> point(EC_POINT_new(group));
    // EC_KEY_set_private_key refuses zero and a scalar at or above the group order.
    if (!point || !EC_KEY_set_private_key(key.get(), secret.get()) ||
        !EC_POINT_mul(group, point.get(), secret.get(), nullptr, nullptr, nullptr) ||
        !EC_KEY_set_public_key(key.get(), point.get()))
    {
        BN_clear(secret.get());
        return nullptr;
    }
    BN_clear(secret.get());
    return ec_pkey(key.get());
}

// A fresh EC key on `curve` from the provider's randomness.
bssl::UniquePtr<EVP_PKEY> ec_generated_key(int curve) noexcept
{
    const bssl::UniquePtr<EC_KEY> key(EC_KEY_new_by_curve_name(curve));
    if (!key || !EC_KEY_generate_key(key.get()))
    {
        return nullptr;
    }
    return ec_pkey(key.get());
}

// A peer's uncompressed SEC 1 point on `group`; null for another form, a wrong length or a point off the curve. The
// form byte is Cerid's to check: AWS-LC also decodes the hybrid form (0x06, 0x07) at the uncompressed length, which
// TLS 1.3 forbids.
bssl::UniquePtr<EC_POINT> ec_point(const EC_GROUP* group, cont::ConstSpan<u8> encoded, usize length) noexcept
{
    if (encoded.size() != length || encoded[0] != POINT_CONVERSION_UNCOMPRESSED)
    {
        return nullptr;
    }
    bssl::UniquePtr<EC_POINT> point(EC_POINT_new(group));
    if (!point || !EC_POINT_oct2point(group, point.get(), encoded.data(), encoded.size(), nullptr))
    {
        return nullptr;
    }
    return point;
}

bssl::UniquePtr<EVP_PKEY> ec_public_key(int curve, cont::ConstSpan<u8> encoded, usize length) noexcept
{
    const bssl::UniquePtr<EC_KEY> key(EC_KEY_new_by_curve_name(curve));
    if (!key)
    {
        return nullptr;
    }
    const bssl::UniquePtr<EC_POINT> point = ec_point(EC_KEY_get0_group(key.get()), encoded, length);
    if (!point || !EC_KEY_set_public_key(key.get(), point.get()))
    {
        return nullptr;
    }
    return ec_pkey(key.get());
}

CryptoError ec_public_bytes(const EVP_PKEY* pkey, cont::Span<u8> out) noexcept
{
    const EC_KEY* key = EVP_PKEY_get0_EC_KEY(pkey);
    const usize   written =
        EC_POINT_point2oct(EC_KEY_get0_group(key), EC_KEY_get0_public_key(key), POINT_CONVERSION_UNCOMPRESSED,
                           out.data(), out.size(), nullptr);
    return written == out.size() ? CryptoError::None : CryptoError::ProviderFailure;
}

bssl::UniquePtr<EVP_PKEY> signature_public_key(const SignatureAlgorithm& entry, cont::ConstSpan<u8> encoded) noexcept
{
    if (encoded.size() != entry.sizes.public_key)
    {
        return nullptr;
    }
    switch (entry.type)
    {
    case EVP_PKEY_PQDSA:
        return bssl::UniquePtr<EVP_PKEY>(EVP_PKEY_pqdsa_new_raw_public_key(entry.nid, encoded.data(), encoded.size()));
    case EVP_PKEY_ED25519:
        return bssl::UniquePtr<EVP_PKEY>(
            EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, encoded.data(), encoded.size()));
    default:
        return ec_public_key(entry.nid, encoded, entry.sizes.public_key);
    }
}

bssl::UniquePtr<EVP_PKEY> signature_private_key(const SignatureAlgorithm& entry, cont::ConstSpan<u8> seed) noexcept
{
    switch (entry.type)
    {
    case EVP_PKEY_PQDSA:
        return bssl::UniquePtr<EVP_PKEY>(EVP_PKEY_pqdsa_new_raw_private_key(entry.nid, seed.data(), seed.size()));
    case EVP_PKEY_ED25519:
        return bssl::UniquePtr<EVP_PKEY>(
            EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
    default:
        return ec_private_key(entry.nid, seed);
    }
}

u64 load_le64(const u8* bytes) noexcept
{
    u64 value = 0U;
    for (usize i = 0U; i < 8U; ++i)
    {
        value |= static_cast<u64>(bytes[i]) << (8U * i);
    }
    return value;
}
} // namespace

namespace provider_internal
{
AlgorithmId signature_algorithm_of(const EVP_PKEY* pkey) noexcept
{
    int type = EVP_PKEY_id(pkey);
    int nid  = 0;
    if (type == EVP_PKEY_PQDSA)
    {
        nid = EVP_PKEY_pqdsa_get_type(pkey);
    }
    else if (type == EVP_PKEY_EC)
    {
        const EC_KEY* key = EVP_PKEY_get0_EC_KEY(pkey);
        nid               = key == nullptr ? 0 : EC_GROUP_get_curve_name(EC_KEY_get0_group(key));
    }
    else if (type == EVP_PKEY_ED25519)
    {
        nid = NID_ED25519;
    }
    else
    {
        type = 0;
    }
    for (const SignatureAlgorithm& entry : kSignatures)
    {
        if (entry.type == type && entry.nid == nid)
        {
            return entry.id;
        }
    }
    return AlgorithmId{};
}

CryptoError signature_public_bytes(const EVP_PKEY* pkey, AlgorithmId algorithm, cont::Span<u8> out) noexcept
{
    const SignatureAlgorithm* entry = find_entry(kSignatures, algorithm);
    if (entry == nullptr || out.size() != entry->sizes.public_key)
    {
        return CryptoError::BadLength;
    }
    if (entry->type == EVP_PKEY_EC)
    {
        return ec_public_bytes(pkey, out);
    }
    usize written = out.size();
    return EVP_PKEY_get_raw_public_key(pkey, out.data(), &written) && written == out.size()
               ? CryptoError::None
               : CryptoError::ProviderFailure;
}

const EVP_MD* signature_digest(AlgorithmId algorithm) noexcept
{
    switch (algorithm)
    {
    case AlgorithmId::EcdsaSecp256r1Sha256:
        return EVP_sha256();
    case AlgorithmId::EcdsaSecp384r1Sha384:
        return EVP_sha384();
    default:
        return nullptr;
    }
}
} // namespace provider_internal

cont::StringView crypto_error_name(CryptoError error) noexcept
{
    switch (error)
    {
    case CryptoError::None:
        return "none";
    case CryptoError::Unsupported:
        return "unsupported";
    case CryptoError::NotReady:
        return "not-ready";
    case CryptoError::BadLength:
        return "bad-length";
    case CryptoError::Overlap:
        return "overlap";
    case CryptoError::BadKey:
        return "bad-key";
    case CryptoError::Forged:
        return "forged";
    case CryptoError::WeakSecret:
        return "weak-secret";
    case CryptoError::Exhausted:
        return "exhausted";
    case CryptoError::ProviderFailure:
        return "provider-failure";
    case CryptoError::Expired:
        return "expired";
    case CryptoError::Replayed:
        return "replayed";
    case CryptoError::OutOfWindow:
        return "out-of-window";
    case CryptoError::Malformed:
        return "malformed";
    }
    return "unknown";
}

cont::StringView provider_name() noexcept
{
    return AWSLC_VERSION_NAME " " AWSLC_VERSION_NUMBER_STRING;
}

cont::ConstSpan<AlgorithmId> provider_algorithms() noexcept
{
    return kImplemented;
}

bool provider_implements(AlgorithmId algorithm) noexcept
{
    return std::ranges::any_of(kImplemented, [algorithm](AlgorithmId implemented) { return implemented == algorithm; });
}

CryptoError random_bytes(cont::Span<u8> out) noexcept
{
    if (out.empty())
    {
        return CryptoError::None;
    }
    if (RAND_bytes(out.data(), out.size()) != 1)
    {
        ERR_clear_error();
        secure_zero(out);
        return CryptoError::ProviderFailure;
    }
    return CryptoError::None;
}

bool constant_time_equal(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept
{
    return a.size() == b.size() && (a.empty() || CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0);
}

void secure_zero(cont::Span<u8> bytes) noexcept
{
    if (!bytes.empty())
    {
        OPENSSL_cleanse(bytes.data(), bytes.size());
    }
}

void sha256(cont::ConstSpan<u8> bytes, cont::Span<u8, kSha256Bytes> out) noexcept
{
    SHA256(bytes.data(), bytes.size(), out.data());
}

KeyId key_id_of(cont::ConstSpan<u8> public_key) noexcept
{
    KeyId id;
    sha256(public_key, id.bytes);
    return id;
}

// ---- AEAD ----

usize aead_key_bytes(AlgorithmId algorithm) noexcept
{
    const EVP_AEAD* aead = find_aead(algorithm);
    return aead != nullptr ? EVP_AEAD_key_length(aead) : 0U;
}

AeadKey::~AeadKey() noexcept
{
    clear();
}

CryptoError AeadKey::init(AlgorithmId algorithm, cont::ConstSpan<u8> key) noexcept
{
    clear();
    const EVP_AEAD* aead = find_aead(algorithm);
    if (aead == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (key.size() != EVP_AEAD_key_length(aead))
    {
        return CryptoError::BadLength;
    }
    EVP_AEAD_CTX* context = ::new (static_cast<void*>(m_state)) EVP_AEAD_CTX;
    if (!EVP_AEAD_CTX_init(context, aead, key.data(), key.size(), kAeadTagBytes, nullptr))
    {
        ERR_clear_error();
        secure_zero(m_state);
        return CryptoError::ProviderFailure;
    }
    m_algorithm = algorithm;
    m_ready     = true;
    return CryptoError::None;
}

void AeadKey::clear() noexcept
{
    if (m_ready)
    {
        EVP_AEAD_CTX_cleanup(std::launder(reinterpret_cast<EVP_AEAD_CTX*>(m_state)));
        m_ready = false;
    }
    secure_zero(m_state);
    m_algorithm = AlgorithmId{};
}

CryptoError AeadKey::seal(cont::ConstSpan<u8> nonce, cont::ConstSpan<u8> plaintext, cont::ConstSpan<u8> associated,
                          cont::Span<u8> out) const noexcept
{
    if (!m_ready)
    {
        return CryptoError::NotReady;
    }
    if (nonce.size() != kAeadNonceBytes || plaintext.size() > ~usize{0} - kAeadTagBytes ||
        out.size() != plaintext.size() + kAeadTagBytes)
    {
        return CryptoError::BadLength;
    }
    if (bad_aead_alias(out, plaintext, associated, nonce))
    {
        return CryptoError::Overlap;
    }
    const auto* context = std::launder(reinterpret_cast<const EVP_AEAD_CTX*>(m_state));
    usize       written = 0U;
    if (!EVP_AEAD_CTX_seal(context, out.data(), &written, out.size(), nonce.data(), nonce.size(), plaintext.data(),
                           plaintext.size(), associated.data(), associated.size()) ||
        written != out.size())
    {
        ERR_clear_error(); // the provider has zeroed `out`
        return CryptoError::ProviderFailure;
    }
    return CryptoError::None;
}

CryptoError AeadKey::open(cont::ConstSpan<u8> nonce, cont::ConstSpan<u8> sealed, cont::ConstSpan<u8> associated,
                          cont::Span<u8> out) const noexcept
{
    if (!m_ready)
    {
        return CryptoError::NotReady;
    }
    if (nonce.size() != kAeadNonceBytes || sealed.size() < kAeadTagBytes ||
        out.size() != sealed.size() - kAeadTagBytes)
    {
        return CryptoError::BadLength;
    }
    if (bad_aead_alias(out, sealed, associated, nonce))
    {
        return CryptoError::Overlap;
    }
    const auto* context = std::launder(reinterpret_cast<const EVP_AEAD_CTX*>(m_state));
    usize       written = 0U;
    if (!EVP_AEAD_CTX_open(context, out.data(), &written, out.size(), nonce.data(), nonce.size(), sealed.data(),
                           sealed.size(), associated.data(), associated.size()) ||
        written != out.size())
    {
        ERR_clear_error(); // the provider has zeroed `out` (EVP_AEAD_CTX_open clears it on every failure)
        return CryptoError::Forged;
    }
    return CryptoError::None;
}

// ---- HKDF ----

usize hkdf_hash_bytes(AlgorithmId algorithm) noexcept
{
    const EVP_MD* digest = hkdf_digest(algorithm);
    return digest != nullptr ? EVP_MD_size(digest) : 0U;
}

CryptoError kdf_hash(AlgorithmId algorithm, cont::ConstSpan<u8> bytes, cont::Span<u8> out) noexcept
{
    const EVP_MD* digest = hkdf_digest(algorithm);
    if (digest == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (out.size() != EVP_MD_size(digest))
    {
        return CryptoError::BadLength;
    }
    const ErrorScope errors;
    unsigned int     written = 0U;
    if (!EVP_Digest(bytes.data(), bytes.size(), out.data(), &written, digest, nullptr) || written != out.size())
    {
        secure_zero(out);
        return CryptoError::ProviderFailure;
    }
    return CryptoError::None;
}

CryptoError kdf_mac(AlgorithmId algorithm, cont::ConstSpan<u8> key, cont::ConstSpan<u8> bytes,
                    cont::Span<u8> out) noexcept
{
    const EVP_MD* digest = hkdf_digest(algorithm);
    if (digest == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (out.size() != EVP_MD_size(digest))
    {
        return CryptoError::BadLength;
    }
    // An empty key still names a buffer: the provider reads no byte of it.
    const u8         none    = 0U;
    const ErrorScope errors;
    unsigned int     written = 0U;
    if (HMAC(digest, key.empty() ? &none : key.data(), key.size(), bytes.data(), bytes.size(), out.data(),
             &written) == nullptr ||
        written != out.size())
    {
        secure_zero(out);
        return CryptoError::ProviderFailure;
    }
    return CryptoError::None;
}

CryptoError hkdf_extract(AlgorithmId algorithm, cont::ConstSpan<u8> salt, cont::ConstSpan<u8> ikm,
                         cont::Span<u8> prk) noexcept
{
    const EVP_MD* digest = hkdf_digest(algorithm);
    if (digest == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (prk.size() != EVP_MD_size(digest))
    {
        return CryptoError::BadLength;
    }
    const ErrorScope errors;
    usize            written = 0U;
    if (!HKDF_extract(prk.data(), &written, digest, ikm.data(), ikm.size(), salt.data(), salt.size()) ||
        written != prk.size())
    {
        secure_zero(prk);
        return CryptoError::ProviderFailure;
    }
    return CryptoError::None;
}

CryptoError hkdf_expand(AlgorithmId algorithm, cont::ConstSpan<u8> prk, cont::ConstSpan<u8> info,
                        cont::Span<u8> out) noexcept
{
    const EVP_MD* digest = hkdf_digest(algorithm);
    if (digest == nullptr)
    {
        return CryptoError::Unsupported;
    }
    const usize hash = EVP_MD_size(digest);
    if (prk.size() < hash || out.empty() || out.size() > 255U * hash)
    {
        return CryptoError::BadLength;
    }
    const ErrorScope errors;
    if (!HKDF_expand(out.data(), out.size(), digest, prk.data(), prk.size(), info.data(), info.size()))
    {
        secure_zero(out);
        return CryptoError::ProviderFailure;
    }
    return CryptoError::None;
}

// ---- SHA-3 ----

namespace
{
// One SHA-3 digest or SHAKE output of `bytes` into `out`.
CryptoError sha3(const EVP_MD* digest, bool xof, cont::ConstSpan<u8> bytes, cont::Span<u8> out) noexcept
{
    const ErrorScope                  errors;
    const bssl::UniquePtr<EVP_MD_CTX> context(EVP_MD_CTX_new());
    unsigned int                      written = 0U;
    const bool                        done =
        context && EVP_DigestInit_ex(context.get(), digest, nullptr) &&
        EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) &&
        (xof ? EVP_DigestFinalXOF(context.get(), out.data(), out.size()) != 0
             : EVP_DigestFinal_ex(context.get(), out.data(), &written) != 0 && written == out.size());
    if (!done)
    {
        secure_zero(out);
        return CryptoError::ProviderFailure;
    }
    return CryptoError::None;
}
} // namespace

CryptoError sha3_256(cont::ConstSpan<u8> bytes, cont::Span<u8> out) noexcept
{
    return out.size() == kSha3DigestBytes ? sha3(EVP_sha3_256(), false, bytes, out) : CryptoError::BadLength;
}

CryptoError shake256(cont::ConstSpan<u8> bytes, cont::Span<u8> out) noexcept
{
    return !out.empty() ? sha3(EVP_shake256(), true, bytes, out) : CryptoError::BadLength;
}

// ---- Sealing keys ----

namespace provider_internal
{
CryptoError sealing_key_name(cont::ConstSpan<u8> key, cont::ConstSpan<u8> label, cont::Span<u8> name) noexcept
{
    u8          prk[32];
    CryptoError error = hkdf_extract(AlgorithmId::HkdfSha256, {}, key, cont::Span<u8>(prk));
    if (error == CryptoError::None)
    {
        error = hkdf_expand(AlgorithmId::HkdfSha256, cont::ConstSpan<u8>(prk), label, name);
    }
    secure_zero(cont::Span<u8>(prk));
    return error;
}

CryptoError salted_aead(cont::ConstSpan<u8> key, cont::ConstSpan<u8> salt, cont::ConstSpan<u8> label, AeadKey& aead,
                        u8 (&nonce)[kAeadNonceBytes]) noexcept
{
    u8          prk[32];
    u8          okm[kSealingKeyBytes + kAeadNonceBytes];
    CryptoError error = hkdf_extract(AlgorithmId::HkdfSha256, salt, key, cont::Span<u8>(prk));
    if (error == CryptoError::None)
    {
        error = hkdf_expand(AlgorithmId::HkdfSha256, cont::ConstSpan<u8>(prk), label, cont::Span<u8>(okm));
    }
    if (error == CryptoError::None)
    {
        error = aead.init(AlgorithmId::Aes256Gcm, cont::ConstSpan<u8>(okm, kSealingKeyBytes));
    }
    std::memcpy(nonce, okm + kSealingKeyBytes, kAeadNonceBytes);
    secure_zero(cont::Span<u8>(prk));
    secure_zero(cont::Span<u8>(okm));
    return error;
}
} // namespace provider_internal

// ---- Provider key handles ----

namespace detail
{
ProviderKeyHandle::~ProviderKeyHandle() noexcept
{
    reset();
}

ProviderKeyHandle::ProviderKeyHandle(ProviderKeyHandle&& other) noexcept
    : m_key(other.m_key), m_algorithm(other.m_algorithm)
{
    other.m_key       = nullptr;
    other.m_algorithm = AlgorithmId{};
}

ProviderKeyHandle& ProviderKeyHandle::operator=(ProviderKeyHandle&& other) noexcept
{
    if (this != &other)
    {
        reset(other.m_key, other.m_algorithm);
        other.m_key       = nullptr;
        other.m_algorithm = AlgorithmId{};
    }
    return *this;
}

void ProviderKeyHandle::reset(ProviderKey* key, AlgorithmId algorithm) noexcept
{
    // EVP_PKEY_free releases the key through OPENSSL_free, which zeroes every allocation it returns.
    EVP_PKEY_free(pkey_of(m_key));
    m_key       = key;
    m_algorithm = key != nullptr ? algorithm : AlgorithmId{};
}
} // namespace detail

// ---- Signatures ----

SignatureSizes signature_sizes(AlgorithmId algorithm) noexcept
{
    const SignatureAlgorithm* entry = find_entry(kSignatures, algorithm);
    return entry != nullptr ? entry->sizes : SignatureSizes{};
}

CryptoError SigningKey::generate(AlgorithmId algorithm) noexcept
{
    clear();
    const SignatureAlgorithm* entry = find_entry(kSignatures, algorithm);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    const ErrorScope errors;
    if (entry->type == EVP_PKEY_EC)
    {
        bssl::UniquePtr<EVP_PKEY> pkey = ec_generated_key(entry->nid);
        if (!pkey)
        {
            return CryptoError::ProviderFailure;
        }
        m_key.reset(key_of(pkey.release()), algorithm);
        return CryptoError::None;
    }
    // ML-DSA and Ed25519 keys are their seeds' functions: a fresh seed is a fresh key.
    u8          seed[32];
    const auto  seed_span = cont::Span<u8>(seed, entry->sizes.seed);
    CryptoError result    = random_bytes(seed_span);
    if (result == CryptoError::None)
    {
        result = from_seed(algorithm, seed_span);
    }
    secure_zero(seed);
    return result;
}

CryptoError SigningKey::from_seed(AlgorithmId algorithm, cont::ConstSpan<u8> seed) noexcept
{
    clear();
    const SignatureAlgorithm* entry = find_entry(kSignatures, algorithm);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (seed.size() != entry->sizes.seed)
    {
        return CryptoError::BadLength;
    }
    const ErrorScope          errors;
    bssl::UniquePtr<EVP_PKEY> pkey = signature_private_key(*entry, seed);
    if (!pkey)
    {
        return CryptoError::BadKey;
    }
    m_key.reset(key_of(pkey.release()), algorithm);
    return CryptoError::None;
}

CryptoError SigningKey::public_key(cont::Span<u8> out) const noexcept
{
    if (!ready())
    {
        return CryptoError::NotReady;
    }
    const ErrorScope errors;
    return provider_internal::signature_public_bytes(pkey_of(m_key.get()), algorithm(), out);
}

CryptoError SigningKey::sign(cont::ConstSpan<u8> message, cont::Span<u8> out, usize& written) const noexcept
{
    written                         = 0U;
    const SignatureAlgorithm* entry = ready() ? find_entry(kSignatures, algorithm()) : nullptr;
    if (entry == nullptr)
    {
        return CryptoError::NotReady;
    }
    if (out.size() < entry->sizes.signature)
    {
        return CryptoError::BadLength;
    }
    const ErrorScope       errors;
    bssl::ScopedEVP_MD_CTX context;
    usize                  length = out.size();
    if (!EVP_DigestSignInit(context.get(), nullptr, signature_digest(algorithm()), nullptr, pkey_of(m_key.get())) ||
        !EVP_DigestSign(context.get(), out.data(), &length, message.data(), message.size()) ||
        length > entry->sizes.signature)
    {
        secure_zero(out);
        return CryptoError::ProviderFailure;
    }
    written = length;
    return CryptoError::None;
}

Verification ProviderVerifier::verify(AlgorithmId algorithm, cont::ConstSpan<u8> public_key,
                                      cont::ConstSpan<u8> message, cont::ConstSpan<u8> signature) const noexcept
{
    const SignatureAlgorithm* entry = find_entry(kSignatures, algorithm);
    if (entry == nullptr)
    {
        return Verification::Unsupported;
    }
    if (signature.empty() || signature.size() > entry->sizes.signature)
    {
        return Verification::Invalid;
    }
    const ErrorScope                errors;
    const bssl::UniquePtr<EVP_PKEY> pkey = signature_public_key(*entry, public_key);
    if (!pkey)
    {
        return Verification::Invalid;
    }
    bssl::ScopedEVP_MD_CTX context;
    const bool             valid =
        EVP_DigestVerifyInit(context.get(), nullptr, signature_digest(algorithm), nullptr, pkey.get()) &&
        EVP_DigestVerify(context.get(), signature.data(), signature.size(), message.data(), message.size()) == 1;
    return valid ? Verification::Valid : Verification::Invalid;
}

// ---- Key agreement ----

AgreementSizes agreement_sizes(AlgorithmId algorithm) noexcept
{
    const AgreementAlgorithm* entry = find_entry(kAgreements, algorithm);
    return entry != nullptr ? entry->sizes : AgreementSizes{};
}

CryptoError check_agreement_public_key(AlgorithmId algorithm, cont::ConstSpan<u8> peer_public) noexcept
{
    const AgreementAlgorithm* entry = find_entry(kAgreements, algorithm);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (peer_public.size() != entry->sizes.public_key)
    {
        return CryptoError::BadLength;
    }
    const ErrorScope errors;
    if (entry->nid == 0)
    {
        // A public scalar: X25519 clears its low three bits, so the product is zero exactly for a point whose order
        // divides 8 (on the curve or its twist).
        constexpr u8 scalar[32] = {0x41U, 0x67U, 0x25U, 0x19U, 0x41U, 0x67U, 0x25U, 0x19U, 0x41U, 0x67U, 0x25U,
                                   0x19U, 0x41U, 0x67U, 0x25U, 0x19U, 0x41U, 0x67U, 0x25U, 0x19U, 0x41U, 0x67U,
                                   0x25U, 0x19U, 0x41U, 0x67U, 0x25U, 0x19U, 0x41U, 0x67U, 0x25U, 0x19U};
        u8 product[32];
        return X25519(product, scalar, peer_public.data()) == 1 ? CryptoError::None : CryptoError::WeakSecret;
    }
    const bssl::UniquePtr<EC_GROUP> group(EC_GROUP_new_by_curve_name(entry->nid));
    if (!group)
    {
        return CryptoError::ProviderFailure;
    }
    return ec_point(group.get(), peer_public, entry->sizes.public_key) ? CryptoError::None : CryptoError::BadKey;
}

CryptoError AgreementKey::generate(AlgorithmId algorithm) noexcept
{
    clear();
    const AgreementAlgorithm* entry = find_entry(kAgreements, algorithm);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    const ErrorScope errors;
    if (entry->nid == 0)
    {
        u8          secret[32];
        CryptoError result = random_bytes(secret);
        if (result == CryptoError::None)
        {
            result = from_private(algorithm, secret);
        }
        secure_zero(secret);
        return result;
    }
    bssl::UniquePtr<EVP_PKEY> pkey = ec_generated_key(entry->nid);
    if (!pkey)
    {
        return CryptoError::ProviderFailure;
    }
    m_key.reset(key_of(pkey.release()), algorithm);
    return CryptoError::None;
}

CryptoError AgreementKey::from_private(AlgorithmId algorithm, cont::ConstSpan<u8> private_key) noexcept
{
    clear();
    const AgreementAlgorithm* entry = find_entry(kAgreements, algorithm);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (private_key.size() != entry->sizes.private_key)
    {
        return CryptoError::BadLength;
    }
    const ErrorScope          errors;
    bssl::UniquePtr<EVP_PKEY> pkey(
        entry->nid == 0 ? EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, private_key.data(), private_key.size())
                        : ec_private_key(entry->nid, private_key).release());
    if (!pkey)
    {
        return CryptoError::BadKey;
    }
    m_key.reset(key_of(pkey.release()), algorithm);
    return CryptoError::None;
}

CryptoError AgreementKey::public_key(cont::Span<u8> out) const noexcept
{
    const AgreementAlgorithm* entry = ready() ? find_entry(kAgreements, algorithm()) : nullptr;
    if (entry == nullptr)
    {
        return CryptoError::NotReady;
    }
    if (out.size() != entry->sizes.public_key)
    {
        return CryptoError::BadLength;
    }
    const ErrorScope errors;
    if (entry->nid != 0)
    {
        return ec_public_bytes(pkey_of(m_key.get()), out);
    }
    usize written = out.size();
    return EVP_PKEY_get_raw_public_key(pkey_of(m_key.get()), out.data(), &written) && written == out.size()
               ? CryptoError::None
               : CryptoError::ProviderFailure;
}

CryptoError AgreementKey::derive(cont::ConstSpan<u8> peer_public, cont::Span<u8> secret) const noexcept
{
    const AgreementAlgorithm* entry = ready() ? find_entry(kAgreements, algorithm()) : nullptr;
    if (entry == nullptr)
    {
        return CryptoError::NotReady;
    }
    if (peer_public.size() != entry->sizes.public_key || secret.size() != entry->sizes.secret)
    {
        return refuse(CryptoError::BadLength, secret);
    }
    if (overlaps(secret, peer_public))
    {
        return CryptoError::Overlap;
    }
    const ErrorScope errors;
    if (entry->nid == 0)
    {
        u8    own[32];
        usize length = sizeof(own);
        if (!EVP_PKEY_get_raw_private_key(pkey_of(m_key.get()), own, &length) || length != sizeof(own))
        {
            secure_zero(own);
            return refuse(CryptoError::ProviderFailure, secret);
        }
        // X25519 returns 0 exactly when the result, which `secret` then holds, is all zeros (a small-order peer).
        const bool contributory = X25519(secret.data(), own, peer_public.data()) == 1;
        secure_zero(own);
        return contributory ? CryptoError::None : CryptoError::WeakSecret;
    }
    const EC_KEY*                   key   = EVP_PKEY_get0_EC_KEY(pkey_of(m_key.get()));
    const bssl::UniquePtr<EC_POINT> point = ec_point(EC_KEY_get0_group(key), peer_public, entry->sizes.public_key);
    if (!point)
    {
        return refuse(CryptoError::BadKey, secret);
    }
    const int length = ECDH_compute_key(secret.data(), secret.size(), point.get(), key, nullptr);
    if (length < 0 || static_cast<usize>(length) != secret.size())
    {
        return refuse(CryptoError::ProviderFailure, secret);
    }
    return CryptoError::None;
}

// ---- Key encapsulation ----

KemSizes kem_sizes(AlgorithmId algorithm) noexcept
{
    const KemAlgorithm* entry = find_entry(kKems, algorithm);
    return entry != nullptr ? entry->sizes : KemSizes{};
}

namespace
{
// A context over the encapsulation key `peer_public` (entry.sizes.public_key bytes) once FIPS 203 section 7.2's
// modulus check passed: the import only copies the bytes, so the check runs here and a key that fails it (BadKey) is
// told apart from a provider failure.
CryptoError kem_peer(const KemAlgorithm& entry, cont::ConstSpan<u8> peer_public,
                     bssl::UniquePtr<EVP_PKEY_CTX>& context) noexcept
{
    const bssl::UniquePtr<EVP_PKEY> pkey(
        EVP_PKEY_kem_new_raw_public_key(entry.nid, peer_public.data(), peer_public.size()));
    context.reset(pkey ? EVP_PKEY_CTX_new(pkey.get(), nullptr) : nullptr);
    if (!context)
    {
        return CryptoError::ProviderFailure;
    }
    return EVP_PKEY_public_check(context.get()) == 1 ? CryptoError::None : CryptoError::BadKey;
}
} // namespace

CryptoError check_kem_public_key(AlgorithmId algorithm, cont::ConstSpan<u8> peer_public) noexcept
{
    const KemAlgorithm* entry = find_entry(kKems, algorithm);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (peer_public.size() != entry->sizes.public_key)
    {
        return CryptoError::BadLength;
    }
    const ErrorScope              errors;
    bssl::UniquePtr<EVP_PKEY_CTX> context;
    return kem_peer(*entry, peer_public, context);
}

CryptoError kem_encapsulate(AlgorithmId algorithm, cont::ConstSpan<u8> peer_public, cont::Span<u8> ciphertext,
                            cont::Span<u8> secret) noexcept
{
    const KemAlgorithm* entry = find_entry(kKems, algorithm);
    if (entry == nullptr)
    {
        return refuse(CryptoError::Unsupported, ciphertext, secret);
    }
    if (peer_public.size() != entry->sizes.public_key || ciphertext.size() != entry->sizes.ciphertext ||
        secret.size() != entry->sizes.secret)
    {
        return refuse(CryptoError::BadLength, ciphertext, secret);
    }
    if (overlaps(ciphertext, peer_public) || overlaps(secret, peer_public) || overlaps(ciphertext, secret))
    {
        return CryptoError::Overlap;
    }
    const ErrorScope              errors;
    bssl::UniquePtr<EVP_PKEY_CTX> context;
    const CryptoError             checked = kem_peer(*entry, peer_public, context);
    if (checked != CryptoError::None)
    {
        return refuse(checked, ciphertext, secret);
    }
    usize ciphertext_length = ciphertext.size();
    usize secret_length     = secret.size();
    if (!EVP_PKEY_encapsulate(context.get(), ciphertext.data(), &ciphertext_length, secret.data(), &secret_length) ||
        ciphertext_length != ciphertext.size() || secret_length != secret.size())
    {
        return refuse(CryptoError::ProviderFailure, ciphertext, secret);
    }
    return CryptoError::None;
}

CryptoError KemKey::generate(AlgorithmId algorithm) noexcept
{
    clear();
    const KemAlgorithm* entry = find_entry(kKems, algorithm);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    u8          seed[64];
    CryptoError result = random_bytes(seed);
    if (result == CryptoError::None)
    {
        result = from_seed(algorithm, seed);
    }
    secure_zero(seed);
    return result;
}

CryptoError KemKey::from_seed(AlgorithmId algorithm, cont::ConstSpan<u8> seed) noexcept
{
    clear();
    const KemAlgorithm* entry = find_entry(kKems, algorithm);
    if (entry == nullptr)
    {
        return CryptoError::Unsupported;
    }
    if (seed.size() != entry->sizes.seed)
    {
        return CryptoError::BadLength;
    }
    const ErrorScope                    errors;
    const bssl::UniquePtr<EVP_PKEY_CTX> context(EVP_PKEY_CTX_new_id(EVP_PKEY_KEM, nullptr));
    EVP_PKEY*                           pkey   = nullptr;
    usize                               length = seed.size();
    // The provider's deterministic key generation (its experimental KEM API) is the only route from a seed to a key.
    if (!context || !EVP_PKEY_CTX_kem_set_params(context.get(), entry->nid) || !EVP_PKEY_keygen_init(context.get()) ||
        !EVP_PKEY_keygen_deterministic(context.get(), &pkey, seed.data(), &length))
    {
        EVP_PKEY_free(pkey);
        return CryptoError::ProviderFailure;
    }
    m_key.reset(key_of(pkey), algorithm);
    return CryptoError::None;
}

CryptoError KemKey::public_key(cont::Span<u8> out) const noexcept
{
    const KemAlgorithm* entry = ready() ? find_entry(kKems, algorithm()) : nullptr;
    if (entry == nullptr)
    {
        return CryptoError::NotReady;
    }
    if (out.size() != entry->sizes.public_key)
    {
        return CryptoError::BadLength;
    }
    const ErrorScope errors;
    usize            written = out.size();
    return EVP_PKEY_get_raw_public_key(pkey_of(m_key.get()), out.data(), &written) && written == out.size()
               ? CryptoError::None
               : CryptoError::ProviderFailure;
}

CryptoError KemKey::decapsulate(cont::ConstSpan<u8> ciphertext, cont::Span<u8> secret) const noexcept
{
    const KemAlgorithm* entry = ready() ? find_entry(kKems, algorithm()) : nullptr;
    if (entry == nullptr)
    {
        return CryptoError::NotReady;
    }
    if (ciphertext.size() != entry->sizes.ciphertext || secret.size() != entry->sizes.secret)
    {
        return refuse(CryptoError::BadLength, secret);
    }
    if (overlaps(secret, ciphertext))
    {
        return CryptoError::Overlap;
    }
    const ErrorScope                    errors;
    const bssl::UniquePtr<EVP_PKEY_CTX> context(EVP_PKEY_CTX_new(pkey_of(m_key.get()), nullptr));
    usize                               length = secret.size();
    if (!context ||
        !EVP_PKEY_decapsulate(context.get(), secret.data(), &length, ciphertext.data(), ciphertext.size()) ||
        length != secret.size())
    {
        return refuse(CryptoError::ProviderFailure, secret);
    }
    return CryptoError::None;
}

// ---- Keyed pseudorandom function ----

PrfKey::~PrfKey() noexcept
{
    clear();
}

CryptoError PrfKey::generate(AlgorithmId algorithm) noexcept
{
    clear();
    if (algorithm != AlgorithmId::Siphash24)
    {
        return CryptoError::Unsupported;
    }
    u8          key[kPrfKeyBytes];
    CryptoError result = random_bytes(key);
    if (result == CryptoError::None)
    {
        result = init(algorithm, key);
    }
    secure_zero(key);
    return result;
}

CryptoError PrfKey::init(AlgorithmId algorithm, cont::ConstSpan<u8> key) noexcept
{
    clear();
    if (algorithm != AlgorithmId::Siphash24)
    {
        return CryptoError::Unsupported;
    }
    if (key.size() != kPrfKeyBytes)
    {
        return CryptoError::BadLength;
    }
    m_key[0]    = load_le64(key.data());
    m_key[1]    = load_le64(key.data() + 8U);
    m_algorithm = algorithm;
    m_ready     = true;
    return CryptoError::None;
}

void PrfKey::clear() noexcept
{
    secure_zero(cont::Span<u8>(reinterpret_cast<u8*>(m_key), sizeof(m_key)));
    m_algorithm = AlgorithmId{};
    m_ready     = false;
}

CryptoError PrfKey::hash(cont::ConstSpan<u8> bytes, u64& out) const noexcept
{
    out = 0U;
    if (!m_ready)
    {
        return CryptoError::NotReady;
    }
    out = SIPHASH_24(m_key, bytes.data(), bytes.size());
    return CryptoError::None;
}
} // namespace crd::crypto
