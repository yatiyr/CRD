#pragma once

// crd-crypto -- the provider interface (NET.2; contract: docs/design/network-foundation.md#provider).
//
// Cerid writes no cryptographic primitive. Every operation here runs in a vetted provider (AWS-LC, pinned in
// cmake/pins.json) behind this interface, and names its algorithm by the NET-STD register identifier, never by a
// provider constant, so a provider swap changes neither a caller nor a byte on the wire. No provider type reaches this
// header. Rules:
//
//   lengths     every buffer has the exact length its algorithm defines (the *_sizes() tables); a wrong one is
//               BadLength, never a truncation or a partial write.
//   refusals    an operation of the wrong class, or an algorithm this build does not implement, is Unsupported; a
//               malformed key, a peer key off its curve or a non-canonical encoding is BadKey.
//   secrets     key material lives inside the key objects, which zero it when cleared or destroyed; outputs that are
//               secrets (shared secrets, derived keys) are the caller's to zero (secure_zero).
//   threads     a key object is read-only after init(): seal, open, sign, derive and verify may run on several
//               threads at once. init() and clear() need the caller's exclusive access.
//   allocation  AEAD and PRF keys and the operations on them allocate nothing (the per-packet and per-lookup
//               paths); signature, agreement and KEM keys hold one provider object each and allocate at init(), and
//               encapsulation and decapsulation allocate one provider context each (the handshake path).

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/policy.hpp>
#include <crd/crypto/register_ids.hpp>
#include <crd/crypto/verifier.hpp>

namespace crd::crypto
{
namespace cont = crd::containers;

enum class CryptoError : u8
{
    None,
    Unsupported,     // the algorithm is not of the operation's class, or the provider does not implement it
    NotReady,        // the key object holds no key
    BadLength,       // an input or output has the wrong length for the algorithm
    Overlap,         // an output overlaps an input other than exactly (only seal and open run in place)
    BadKey,          // a key, own or the peer's, is malformed or invalid for the algorithm
    Forged,          // AEAD open: the tag does not authenticate the ciphertext, nonce and associated data
    WeakSecret,      // key agreement produced the all-zero secret (an X25519 peer key of small order)
    Exhausted,       // the key reached its usage limit: update it (quic.hpp's packet keys)
    ProviderFailure, // the provider failed (no randomness, no memory)
    Expired,         // an authentic token outside its lifetime (quic.hpp's address-validation tokens)
    Replayed,        // an authentic single-use token presented again (quic.hpp's address-validation tokens), a
                     // message key already used (mls.hpp's secret tree)
    OutOfWindow,     // a generation or a sender outside what a key ratchet keeps (mls.hpp's secret tree)
    Malformed,       // an authentic message whose content does not decode, or a sender and content its framing
                     // refuses (mls_message.hpp)
};

// "none", "unsupported", "not-ready", "bad-length", "overlap", "bad-key", "forged", "weak-secret", "exhausted",
// "provider-failure", "expired", "replayed", "out-of-window", "malformed".
[[nodiscard]] cont::StringView crypto_error_name(CryptoError error) noexcept;

// The provider and its version ("AWS-LC 5.12.0").
[[nodiscard]] cont::StringView provider_name() noexcept;

// The algorithms the provider implements, in identifier order: a policy's ceiling (PolicyConfig::supported).
[[nodiscard]] cont::ConstSpan<AlgorithmId> provider_algorithms() noexcept;
[[nodiscard]] bool                         provider_implements(AlgorithmId algorithm) noexcept;

// Randomness from the provider's DRBG (NIST SP 800-90A CTR_DRBG), seeded from the operating system's source and CPU
// jitter as a second independent source (decision P2). ProviderFailure leaves `out` zeroed.
[[nodiscard]] CryptoError random_bytes(cont::Span<u8> out) noexcept;

// Equality of two byte strings in time that depends only on their lengths, which are public.
[[nodiscard]] bool constant_time_equal(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept;

// Zeroes `bytes` in a way the compiler cannot remove.
void secure_zero(cont::Span<u8> bytes) noexcept;

inline constexpr usize kSha256Bytes = 32U;

// The SHA-256 of `bytes` (FIPS 180-4).
void sha256(cont::ConstSpan<u8> bytes, cont::Span<u8, kSha256Bytes> out) noexcept;

// A trust key's identifier: the SHA-256 of its public key (policy.hpp's KeyId).
[[nodiscard]] KeyId key_id_of(cont::ConstSpan<u8> public_key) noexcept;

// ---- Authenticated encryption (AES-128-GCM, AES-256-GCM, ChaCha20-Poly1305) ----

inline constexpr usize kAeadNonceBytes = 12U; // RFC 5116 N_MIN = N_MAX for all three
inline constexpr usize kAeadTagBytes   = 16U;
inline constexpr usize kAeadStateBytes = 584U; // the provider's context, held inline (checked where it is defined)

// The key length of an AEAD the provider implements (16 or 32); 0 for anything else.
[[nodiscard]] usize aead_key_bytes(AlgorithmId algorithm) noexcept;

class AeadKey
{
public:
    AeadKey() noexcept = default;
    ~AeadKey() noexcept;
    AeadKey(const AeadKey&)            = delete;
    AeadKey& operator=(const AeadKey&) = delete;
    AeadKey(AeadKey&&)                 = delete;
    AeadKey& operator=(AeadKey&&)      = delete;

    // Expands `key` (aead_key_bytes(algorithm) long) for `algorithm`, replacing any previous key.
    [[nodiscard]] CryptoError init(AlgorithmId algorithm, cont::ConstSpan<u8> key) noexcept;
    void                      clear() noexcept;

    [[nodiscard]] bool        ready() const noexcept { return m_ready; }
    [[nodiscard]] AlgorithmId algorithm() const noexcept { return m_algorithm; }

    // `out` is the ciphertext followed by the tag: plaintext.size() + kAeadTagBytes bytes. It may begin exactly at
    // `plaintext` (in place) and must not overlap it otherwise. A nonce must never repeat under one key.
    [[nodiscard]] CryptoError seal(cont::ConstSpan<u8> nonce, cont::ConstSpan<u8> plaintext,
                                   cont::ConstSpan<u8> associated, cont::Span<u8> out) const noexcept;

    // `sealed` is a ciphertext followed by its tag; `out` receives sealed.size() - kAeadTagBytes bytes, in place or
    // apart as for seal. On Forged `out` is zeroed (by the provider; the tests hold every provider to it): no
    // unauthenticated byte is ever released.
    [[nodiscard]] CryptoError open(cont::ConstSpan<u8> nonce, cont::ConstSpan<u8> sealed,
                                   cont::ConstSpan<u8> associated, cont::Span<u8> out) const noexcept;

private:
    alignas(16) u8 m_state[kAeadStateBytes] = {};
    AlgorithmId m_algorithm{};
    bool        m_ready = false;
};

// ---- Key derivation (HKDF, RFC 5869, with SHA-256 or SHA-384) ----

// The hash length of an HKDF the provider implements (32 or 48); 0 for anything else.
[[nodiscard]] usize hkdf_hash_bytes(AlgorithmId algorithm) noexcept;

// PRK = HKDF-Extract(salt, ikm); `prk` is hkdf_hash_bytes(algorithm) long. An empty salt is HashLen zero bytes.
[[nodiscard]] CryptoError hkdf_extract(AlgorithmId algorithm, cont::ConstSpan<u8> salt, cont::ConstSpan<u8> ikm,
                                       cont::Span<u8> prk) noexcept;

// OKM = HKDF-Expand(prk, info, out.size()); `prk` is at least HashLen long and `out` 1 to 255 * HashLen bytes.
[[nodiscard]] CryptoError hkdf_expand(AlgorithmId algorithm, cont::ConstSpan<u8> prk, cont::ConstSpan<u8> info,
                                      cont::Span<u8> out) noexcept;

// The hash an HKDF is built on (SHA-256 for HKDF-SHA256, SHA-384 for HKDF-SHA384, FIPS 180-4) over `bytes`; `out` is
// hkdf_hash_bytes(algorithm) long. HPKE and MLS cipher suites name their hash by their KDF (RFC 9420 section 5.1).
[[nodiscard]] CryptoError kdf_hash(AlgorithmId algorithm, cont::ConstSpan<u8> bytes, cont::Span<u8> out) noexcept;

// HMAC (RFC 2104, FIPS 198-1) with that hash, keyed by `key` (any length) over `bytes`; `out` is
// hkdf_hash_bytes(algorithm) long, the untruncated tag. MLS's MAC (RFC 9420 section 5.1).
[[nodiscard]] CryptoError kdf_mac(AlgorithmId algorithm, cont::ConstSpan<u8> key, cont::ConstSpan<u8> bytes,
                                  cont::Span<u8> out) noexcept;

// ---- SHA-3 (FIPS 202) ----

// SHA3-256 of `bytes` into `out` (kSha3DigestBytes long) and SHAKE256 of `bytes` into `out` (1 byte or more): the KDF
// and the PRG of the post-quantum/traditional hybrid KEMs and the SHAKE256 derivation of HPKE's post-quantum KEMs
// (draft-irtf-cfrg-hybrid-kems, draft-ietf-hpke-pq). On a provider failure `out` is zeroed.
inline constexpr usize kSha3DigestBytes = 32U;

[[nodiscard]] CryptoError sha3_256(cont::ConstSpan<u8> bytes, cont::Span<u8> out) noexcept;
[[nodiscard]] CryptoError shake256(cont::ConstSpan<u8> bytes, cont::Span<u8> out) noexcept;

// ---- Signatures (ML-DSA-44/65/87, Ed25519, ECDSA P-256/SHA-256 and P-384/SHA-384) ----

// Encodings (decision P4): ML-DSA public keys and signatures as FIPS 204 encodes them, signed purely with an empty
// context; Ed25519 as RFC 8032; ECDSA public keys as uncompressed SEC 1 points and signatures as DER Ecdsa-Sig-Value
// (the TLS 1.3 and X.509 forms). `seed` is the private input from_seed() takes: ML-DSA's 32-byte seed xi, Ed25519's
// 32-byte private key, ECDSA's big-endian private scalar.
struct SignatureSizes
{
    usize public_key = 0U;
    usize seed       = 0U;
    usize signature  = 0U; // the longest signature (ECDSA's DER form is shorter by a few bytes now and then)
};

// All zero for anything but an implemented signature algorithm.
[[nodiscard]] SignatureSizes signature_sizes(AlgorithmId algorithm) noexcept;

struct ProviderKey; // the provider's key object, defined only inside crd-crypto

namespace detail
{
// The provider key object a signature, agreement or KEM key owns, with its algorithm: moved, never copied, and
// released (the provider zeroes it) on reset and destruction; a moved-from handle holds nothing.
class ProviderKeyHandle
{
public:
    ProviderKeyHandle() noexcept = default;
    ~ProviderKeyHandle() noexcept;
    ProviderKeyHandle(const ProviderKeyHandle&)            = delete;
    ProviderKeyHandle& operator=(const ProviderKeyHandle&) = delete;
    ProviderKeyHandle(ProviderKeyHandle&& other) noexcept;
    ProviderKeyHandle& operator=(ProviderKeyHandle&& other) noexcept;

    void                       reset(ProviderKey* key = nullptr, AlgorithmId algorithm = {}) noexcept;
    [[nodiscard]] ProviderKey* get() const noexcept { return m_key; }
    [[nodiscard]] AlgorithmId  algorithm() const noexcept { return m_algorithm; }

private:
    ProviderKey* m_key = nullptr;
    AlgorithmId  m_algorithm{};
};
} // namespace detail

class TlsCredential; // tls.hpp: shares a signing key with the TLS stack

// What signs for a principal: a provider key (SigningKey) or a hardware-held one (the key store). Code that signs a
// Cerid artefact (capability tokens, their attenuations and proofs) takes a Signer and never a key's private input.
class Signer
{
public:
    [[nodiscard]] virtual AlgorithmId algorithm() const noexcept = 0;

    // `out` is signature_sizes(algorithm()).public_key long.
    [[nodiscard]] virtual CryptoError public_key(cont::Span<u8> out) const noexcept = 0;

    // Signs `message` into `out` (at least signature_sizes(algorithm()).signature long); `written` is the signature's
    // length. Safe to call from several threads at once.
    [[nodiscard]] virtual CryptoError sign(cont::ConstSpan<u8> message, cont::Span<u8> out,
                                           usize& written) const noexcept = 0;

protected:
    Signer() noexcept                         = default;
    Signer(const Signer&) noexcept            = default;
    Signer& operator=(const Signer&) noexcept = default;
    Signer(Signer&&) noexcept                 = default;
    Signer& operator=(Signer&&) noexcept      = default;
    ~Signer() noexcept                        = default;
};

class SigningKey final : public Signer
{
public:
    // A fresh key from the provider's randomness.
    [[nodiscard]] CryptoError generate(AlgorithmId algorithm) noexcept;
    // The key `seed` determines (signature_sizes(algorithm).seed bytes; an ECDSA scalar must lie in [1, n - 1]).
    [[nodiscard]] CryptoError from_seed(AlgorithmId algorithm, cont::ConstSpan<u8> seed) noexcept;
    void                      clear() noexcept { m_key.reset(); }

    [[nodiscard]] bool        ready() const noexcept { return m_key.get() != nullptr; }
    [[nodiscard]] AlgorithmId algorithm() const noexcept override { return m_key.algorithm(); }

    [[nodiscard]] CryptoError public_key(cont::Span<u8> out) const noexcept override;

    // ML-DSA signs hedged (FIPS 204 with fresh randomness), ECDSA with a fresh nonce, Ed25519 deterministically.
    [[nodiscard]] CryptoError sign(cont::ConstSpan<u8> message, cont::Span<u8> out,
                                   usize& written) const noexcept override;

private:
    friend class TlsCredential;

    detail::ProviderKeyHandle m_key;
};

// Verification through the provider; stateless, so one instance serves every thread.
class ProviderVerifier final : public SignatureVerifier
{
public:
    [[nodiscard]] Verification verify(AlgorithmId algorithm, cont::ConstSpan<u8> public_key,
                                      cont::ConstSpan<u8> message,
                                      cont::ConstSpan<u8> signature) const noexcept override;
};

// ---- Key agreement (X25519, ECDH over P-256 and P-384) ----

// X25519 keys and secrets are RFC 7748's 32 bytes; P-256 and P-384 public keys are uncompressed SEC 1 points (65 and
// 97 bytes, the only form TLS 1.3 accepts), private keys big-endian scalars and secrets the x-coordinate (32, 48).
struct AgreementSizes
{
    usize public_key  = 0U;
    usize private_key = 0U;
    usize secret      = 0U;
};

// All zero for anything but an implemented key agreement.
[[nodiscard]] AgreementSizes agreement_sizes(AlgorithmId algorithm) noexcept;

// Whether every agreement with `peer_public` can succeed, checked once when a long-lived key enters shared state (an
// MLS leaf's or a node's HPKE key, which every later commit encrypts to): Unsupported for another algorithm,
// BadLength for another length, BadKey for an EC point that is not uncompressed and on the curve, WeakSecret for an
// X25519 key of small order (any clamped scalar times it is zero, so every agreement with it is all zero).
[[nodiscard]] CryptoError check_agreement_public_key(AlgorithmId algorithm, cont::ConstSpan<u8> peer_public) noexcept;

class AgreementKey
{
public:
    [[nodiscard]] CryptoError generate(AlgorithmId algorithm) noexcept;
    // An X25519 private key is any 32 bytes (clamped as RFC 7748 says); an EC scalar must lie in [1, n - 1].
    [[nodiscard]] CryptoError from_private(AlgorithmId algorithm, cont::ConstSpan<u8> private_key) noexcept;
    void                      clear() noexcept { m_key.reset(); }

    [[nodiscard]] bool        ready() const noexcept { return m_key.get() != nullptr; }
    [[nodiscard]] AlgorithmId algorithm() const noexcept { return m_key.algorithm(); }

    [[nodiscard]] CryptoError public_key(cont::Span<u8> out) const noexcept;

    // The shared secret with `peer_public`, which `secret` must not overlap (Overlap, nothing written). An EC peer
    // point must be uncompressed and on the curve (BadKey otherwise, the invalid-curve defence); an X25519 secret of
    // all zeros is WeakSecret (RFC 7748 section 6.1, RFC 8446 section 7.4.2). On any other error `secret` is zeroed.
    [[nodiscard]] CryptoError derive(cont::ConstSpan<u8> peer_public, cont::Span<u8> secret) const noexcept;

private:
    detail::ProviderKeyHandle m_key;
};

// ---- Key encapsulation (ML-KEM-768 and ML-KEM-1024, FIPS 203) ----

// Encodings (decision P7): encapsulation keys and ciphertexts as FIPS 203 encodes them, 32-byte shared secrets, and
// as the private input the 64-byte seed d || z that ML-KEM.KeyGen_internal takes, never the expanded decapsulation
// key (a seed cannot be malformed; an expanded key can).
struct KemSizes
{
    usize public_key = 0U; // the encapsulation key
    usize seed       = 0U;
    usize ciphertext = 0U;
    usize secret     = 0U;
};

// All zero for anything but an implemented key encapsulation mechanism.
[[nodiscard]] KemSizes kem_sizes(AlgorithmId algorithm) noexcept;

// Encapsulates a fresh secret to `peer_public`: `ciphertext` and `secret` have kem_sizes(algorithm)'s lengths and
// overlap neither `peer_public` nor each other (Overlap, nothing written). The encapsulation key must pass FIPS 203's
// input check (section 7.2: every coefficient it encodes is below q), or the result is BadKey. On any other error
// both outputs are zeroed.
[[nodiscard]] CryptoError kem_encapsulate(AlgorithmId algorithm, cont::ConstSpan<u8> peer_public,
                                          cont::Span<u8> ciphertext, cont::Span<u8> secret) noexcept;

// Whether every encapsulation to `peer_public` can succeed, checked once when a long-lived key enters shared state (as
// check_agreement_public_key): Unsupported for another algorithm, BadLength for another length, BadKey for a key that
// fails FIPS 203's input check.
[[nodiscard]] CryptoError check_kem_public_key(AlgorithmId algorithm, cont::ConstSpan<u8> peer_public) noexcept;

class KemKey
{
public:
    // A fresh key: a seed from the provider's randomness.
    [[nodiscard]] CryptoError generate(AlgorithmId algorithm) noexcept;
    // The key `seed` (kem_sizes(algorithm).seed bytes, d || z) determines; every seed of that length is a key.
    [[nodiscard]] CryptoError from_seed(AlgorithmId algorithm, cont::ConstSpan<u8> seed) noexcept;
    void                      clear() noexcept { m_key.reset(); }

    [[nodiscard]] bool        ready() const noexcept { return m_key.get() != nullptr; }
    [[nodiscard]] AlgorithmId algorithm() const noexcept { return m_key.algorithm(); }

    // The encapsulation key: kem_sizes(algorithm()).public_key bytes.
    [[nodiscard]] CryptoError public_key(cont::Span<u8> out) const noexcept;

    // The secret `ciphertext` carries; `secret` must not overlap it (Overlap, nothing written). A ciphertext of the
    // right length never fails: one not made for this key yields ML-KEM's implicit-rejection secret, a pseudorandom
    // function of the key's z and the ciphertext, in the same time as a genuine one, so neither the answer nor its
    // timing tells whether the ciphertext was genuine (FIPS 203 section 6.3); the protocol notices only when keys
    // derived from the secret fail to authenticate. Another length is BadLength, with `secret` zeroed.
    [[nodiscard]] CryptoError decapsulate(cont::ConstSpan<u8> ciphertext, cont::Span<u8> secret) const noexcept;

private:
    detail::ProviderKeyHandle m_key;
};

// ---- Keyed pseudorandom function for tables (SipHash-2-4) ----

// Every table an adversary can fill (connection identifiers, address buckets, object tables) is indexed by this PRF
// under a secret key, so colliding inputs cannot be found faster than by trying them (threat T10, decision P8).
inline constexpr usize kPrfKeyBytes = 16U;

class PrfKey
{
public:
    PrfKey() noexcept = default;
    ~PrfKey() noexcept;
    PrfKey(const PrfKey&)            = delete;
    PrfKey& operator=(const PrfKey&) = delete;
    PrfKey(PrfKey&&)                 = delete;
    PrfKey& operator=(PrfKey&&)      = delete;

    // A fresh key from the provider's randomness: every table, or every process, generates its own.
    [[nodiscard]] CryptoError generate(AlgorithmId algorithm) noexcept;
    // A key that cooperating processes share (kPrfKeyBytes, SipHash's k0 then k1, each little-endian), such as the
    // servers behind one load balancer that route the same connection identifiers.
    [[nodiscard]] CryptoError init(AlgorithmId algorithm, cont::ConstSpan<u8> key) noexcept;
    void                      clear() noexcept;

    [[nodiscard]] bool        ready() const noexcept { return m_ready; }
    [[nodiscard]] AlgorithmId algorithm() const noexcept { return m_algorithm; }

    // The PRF of `bytes` (SipHash's 64-bit output; its byte form is this value little-endian). NotReady, with `out`
    // zero, when the key holds none: a table must never fall back to a public function.
    [[nodiscard]] CryptoError hash(cont::ConstSpan<u8> bytes, u64& out) const noexcept;

private:
    u64         m_key[2] = {};
    AlgorithmId m_algorithm{};
    bool        m_ready = false;
};
} // namespace crd::crypto
