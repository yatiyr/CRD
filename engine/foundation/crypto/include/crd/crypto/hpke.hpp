#pragma once

// crd-crypto -- hybrid public key encryption, RFC 9180 (NET.2; contract: docs/design/network-foundation.md#hpke).
//
// Base mode, single shot: a sender seals one message to a recipient's public key (SealBase) and the recipient opens
// it (OpenBase), or both derive one secret from a context's exporter (section 5.3): every use MLS makes of HPKE (RFC
// 9420 sections 5.1.3 and 8.3, the latter an external joiner's init secret). A KEM is named by an identifier of the
// provider (decision H2): a key agreement group is DHKEM over it (X25519, P-256, P-384, RFC 9180 section 4.1), an
// ML-KEM parameter set is that KEM (ML-KEM-768 and ML-KEM-1024, draft-ietf-hpke-pq section 3) and a hybrid group is
// the post-quantum/traditional hybrid KEM of the same components (MLKEM768-X25519, which is X-Wing, MLKEM768-P256
// and MLKEM1024-P384: draft-irtf-cfrg-concrete-hybrid-kems through draft-ietf-hpke-pq section 4). The KDF is named
// by an HKDF identifier and the AEAD by its own. Every step is the specifications' construction over the provider's
// primitives (decision H1): Cerid adds no primitive, and the drafts' and the RFC's test vectors hold the composition
// to the byte.
//
//   lengths     public keys, encapsulations and private keys have hpke_sizes(kem)'s lengths; a ciphertext is the
//               plaintext plus kHpkeTagBytes. A wrong one is BadLength.
//   refusals    a suite outside the profile is Unsupported; an EC encapsulation or public key off the curve, or an
//               ML-KEM encapsulation key that fails FIPS 203's input check, is BadKey; a DHKEM(X25519) one of small
//               order is WeakSecret (section 7.1.4), while a hybrid's X25519 half, which its combiner binds, is
//               taken as RFC 7748 computes it; a ciphertext that does not authenticate is Forged (an ML-KEM
//               ciphertext of the right length always decapsulates, by implicit rejection). Every refusal zeroes the
//               outputs.
//   secrets     the shared secret, the key schedule and the AEAD key live on the stack of one call and are zeroed
//               before it returns; HpkeKey's halves zero their private keys as AgreementKey and KemKey do.
//   allocation  none for an info and associated data that fit with their labels in 256 bytes; a longer info is laid
//               out once from the default allocator (an MLS group context on the handshake path).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/register_ids.hpp>

namespace crd::crypto
{
// The three algorithms of an HPKE suite: `kem` names the KEM (X25519, Secp256r1 and Secp384r1 for DHKEM(X25519,
// HKDF-SHA256), DHKEM(P-256, HKDF-SHA256) and DHKEM(P-384, HKDF-SHA384); MlKem768 and MlKem1024; X25519Mlkem768,
// Secp256r1Mlkem768 and Secp384r1Mlkem1024 for MLKEM768-X25519, MLKEM768-P256 and MLKEM1024-P384), `kdf` HkdfSha256
// or HkdfSha384 and `aead` Aes128Gcm, Aes256Gcm or Chacha20Poly1305.
struct HpkeSuite
{
    AlgorithmId kem{};
    AlgorithmId kdf{};
    AlgorithmId aead{};
};

inline constexpr usize kHpkeTagBytes = kAeadTagBytes;

// A KEM's public key (Npk), encapsulation (Nenc) and private key (Nsk) lengths: a DHKEM's encapsulation is the
// ephemeral public key (Nenc = Npk) and its private key the scalar; an ML-KEM's private key is the 64-byte seed d || z
// and a hybrid's the 32-byte seed both halves expand from. All zero for a KEM outside the profile.
struct HpkeSizes
{
    usize public_key    = 0U;
    usize encapsulation = 0U;
    usize private_key   = 0U;
};

[[nodiscard]] HpkeSizes hpke_sizes(AlgorithmId kem) noexcept;

// Whether every algorithm of `suite` is in the profile.
[[nodiscard]] bool hpke_supported(const HpkeSuite& suite) noexcept;

// Whether every encapsulation to `public_key` can succeed, checked once when a long-lived key enters shared state (an
// MLS leaf's, init or node key, which later commits encrypt to): Unsupported for a KEM outside the profile, BadLength
// for another length, BadKey for an EC point or half off the curve or an ML-KEM key or half failing FIPS 203's input
// check, WeakSecret for a DHKEM(X25519) key of small order. A hybrid's X25519 half is any 32 bytes, as X-Wing takes
// it.
[[nodiscard]] CryptoError hpke_check_public_key(AlgorithmId kem, cont::ConstSpan<u8> public_key) noexcept;

// A recipient's key pair.
class HpkeKey
{
public:
    HpkeKey() noexcept  = default;
    ~HpkeKey() noexcept = default;
    HpkeKey(const HpkeKey&)            = delete;
    HpkeKey& operator=(const HpkeKey&) = delete;
    HpkeKey(HpkeKey&& other) noexcept;
    HpkeKey& operator=(HpkeKey&& other) noexcept;

    // A fresh key from the provider's randomness (GenerateKeyPair).
    [[nodiscard]] CryptoError generate(AlgorithmId kem) noexcept;
    // DeriveKeyPair(ikm): the key pair `ikm` determines. A DHKEM's by its labelled extract and expand (RFC 9180
    // section 7.1.3; rejection sampling of the scalar for P-256 and P-384) from at least Nsk bytes; an ML-KEM's or a
    // hybrid's by SHAKE256's LabeledDerive into its seed (draft-ietf-hpke-pq sections 3 and 4) from at least 32
    // bytes, the strength of every such KEM (decision H2: an MLS suite derives its keys from Nh bytes, fewer than an
    // ML-KEM's Nsk). A shorter `ikm` is BadLength.
    [[nodiscard]] CryptoError derive(AlgorithmId kem, cont::ConstSpan<u8> ikm) noexcept;
    // The key whose SerializePrivateKey form is `private_key` (Nsk bytes): a DHKEM's scalar (for P-256 and P-384 in
    // [1, n - 1]), an ML-KEM's or a hybrid's seed (every seed of that length is a key).
    [[nodiscard]] CryptoError from_private(AlgorithmId kem, cont::ConstSpan<u8> private_key) noexcept;
    void                      clear() noexcept;

    [[nodiscard]] bool        ready() const noexcept { return m_kem != AlgorithmId{}; }
    [[nodiscard]] AlgorithmId kem() const noexcept { return m_kem; }

    // SerializePublicKey(pk): hpke_sizes(kem()).public_key bytes.
    [[nodiscard]] CryptoError public_key(cont::Span<u8> out) const noexcept;

    // OpenBase(enc, skR, info, aad, ciphertext) for the first message of a context (sequence number 0): `out` is
    // ciphertext.size() - kHpkeTagBytes long and overlaps nothing. `suite.kem` is this key's KEM (Unsupported
    // otherwise).
    [[nodiscard]] CryptoError open(const HpkeSuite& suite, cont::ConstSpan<u8> enc, cont::ConstSpan<u8> info,
                                   cont::ConstSpan<u8> aad, cont::ConstSpan<u8> ciphertext,
                                   cont::Span<u8> out) const noexcept;
    // SetupBaseR(enc, skR, info) and the context's Export(exporter_context, out.size()) (section 5.3): `out` is 1 to
    // 255 * Nh bytes of the suite's KDF (BadLength otherwise) and overlaps nothing. `suite.kem` is this key's KEM.
    [[nodiscard]] CryptoError export_secret(const HpkeSuite& suite, cont::ConstSpan<u8> enc, cont::ConstSpan<u8> info,
                                            cont::ConstSpan<u8> exporter_context, cont::Span<u8> out) const noexcept;

private:
    // The halves an ML-KEM's or a hybrid's Nsk-byte seed expands to (Unsupported for a KEM outside the profile).
    [[nodiscard]] CryptoError expand(AlgorithmId kem, cont::ConstSpan<u8> seed) noexcept;
    // Decap(enc) into `shared_secret` under this key's KEM (NotReady for a key that holds none).
    [[nodiscard]] CryptoError decapsulate(cont::ConstSpan<u8> enc, cont::Span<u8> shared_secret) const noexcept;

    AgreementKey m_key; // a DHKEM's key, or a hybrid's traditional half
    KemKey       m_pq;  // an ML-KEM's key, or a hybrid's post-quantum half
    AlgorithmId  m_kem{};
};

// SealBase(pkR, info, aad, plaintext) with a fresh encapsulation: `enc` is hpke_sizes(suite.kem).encapsulation long
// and `ciphertext` plaintext.size() + kHpkeTagBytes; the outputs overlap no input and each other (Overlap, nothing
// written).
[[nodiscard]] CryptoError hpke_seal(const HpkeSuite& suite, cont::ConstSpan<u8> peer_public, cont::ConstSpan<u8> info,
                                    cont::ConstSpan<u8> aad, cont::ConstSpan<u8> plaintext, cont::Span<u8> enc,
                                    cont::Span<u8> ciphertext) noexcept;

// SetupBaseS(pkR, info) with a fresh encapsulation and the context's Export(exporter_context, out.size()) (section
// 5.3): `enc` as hpke_seal's, `out` as HpkeKey::export_secret's; the outputs overlap no input and each other.
[[nodiscard]] CryptoError hpke_export(const HpkeSuite& suite, cont::ConstSpan<u8> peer_public, cont::ConstSpan<u8> info,
                                      cont::ConstSpan<u8> exporter_context, cont::Span<u8> enc,
                                      cont::Span<u8> out) noexcept;
} // namespace crd::crypto
