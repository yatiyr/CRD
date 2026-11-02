#pragma once

// crd-crypto -- hybrid public key encryption, RFC 9180 (NET.2; contract: docs/design/network-foundation.md#hpke).
//
// Base mode, single shot: a sender seals one message to a recipient's public key (SealBase) and the recipient opens
// it (OpenBase), which is every use MLS makes of HPKE (RFC 9420 section 5.1.3). The KEM is DHKEM over a key agreement
// group of the provider (X25519, P-256, P-384), so a KEM is named by that group's identifier; the KDF by an HKDF
// identifier and the AEAD by its own. Every step is RFC 9180's construction over the provider's primitives (decision
// H1): Cerid adds no primitive, and the RFC's test vectors hold the composition to the byte.
//
//   lengths     public keys, private keys and encapsulations have hpke_sizes(kem)'s lengths; a ciphertext is the
//               plaintext plus kHpkeTagBytes. A wrong one is BadLength.
//   refusals    a suite outside the profile is Unsupported; an encapsulation or public key off the curve is BadKey,
//               an X25519 one of small order WeakSecret (section 7.1.4); a ciphertext that does not authenticate is
//               Forged. Every refusal zeroes the outputs.
//   secrets     the shared secret, the key schedule and the AEAD key live on the stack of one call and are zeroed
//               before it returns; HpkeKey zeroes its private key as AgreementKey does.
//   allocation  none for an info and associated data that fit with their labels in 256 bytes; a longer info is laid
//               out once from the default allocator (an MLS group context on the handshake path).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/register_ids.hpp>

namespace crd::crypto
{
// The three algorithms of an HPKE suite: `kem` is the DHKEM's group (X25519 for DHKEM(X25519, HKDF-SHA256),
// Secp256r1 for DHKEM(P-256, HKDF-SHA256), Secp384r1 for DHKEM(P-384, HKDF-SHA384)), `kdf` HkdfSha256 or HkdfSha384
// and `aead` Aes128Gcm, Aes256Gcm or Chacha20Poly1305.
struct HpkeSuite
{
    AlgorithmId kem{};
    AlgorithmId kdf{};
    AlgorithmId aead{};
};

inline constexpr usize kHpkeTagBytes = kAeadTagBytes;

// A DHKEM's public key (Npk, RFC 9180 section 7.1: the encapsulation is the ephemeral public key, so Nenc = Npk) and
// private key (Nsk) lengths; all zero for a group that is not a DHKEM of the profile.
struct HpkeSizes
{
    usize public_key  = 0U;
    usize private_key = 0U;
};

[[nodiscard]] HpkeSizes hpke_sizes(AlgorithmId kem) noexcept;

// Whether every algorithm of `suite` is in the profile.
[[nodiscard]] bool hpke_supported(const HpkeSuite& suite) noexcept;

// A recipient's key pair.
class HpkeKey
{
public:
    // A fresh key from the provider's randomness.
    [[nodiscard]] CryptoError generate(AlgorithmId kem) noexcept;
    // DeriveKeyPair(ikm) (section 7.1.3): the key pair `ikm` determines, by the KEM's labelled extract and expand
    // (rejection sampling of the scalar for P-256 and P-384). `ikm` holds at least Nsk bytes (BadLength otherwise).
    [[nodiscard]] CryptoError derive(AlgorithmId kem, cont::ConstSpan<u8> ikm) noexcept;
    // The key whose SerializePrivateKey form is `private_key` (Nsk bytes; a P-256 or P-384 scalar in [1, n - 1]).
    [[nodiscard]] CryptoError from_private(AlgorithmId kem, cont::ConstSpan<u8> private_key) noexcept;
    void                      clear() noexcept { m_key.clear(); }

    [[nodiscard]] bool        ready() const noexcept { return m_key.ready(); }
    [[nodiscard]] AlgorithmId kem() const noexcept { return m_key.algorithm(); }

    // SerializePublicKey(pk): hpke_sizes(kem()).public_key bytes.
    [[nodiscard]] CryptoError public_key(cont::Span<u8> out) const noexcept;

    // OpenBase(enc, skR, info, aad, ciphertext) for the first message of a context (sequence number 0): `out` is
    // ciphertext.size() - kHpkeTagBytes long and overlaps nothing. `suite.kem` is this key's KEM (Unsupported
    // otherwise).
    [[nodiscard]] CryptoError open(const HpkeSuite& suite, cont::ConstSpan<u8> enc, cont::ConstSpan<u8> info,
                                   cont::ConstSpan<u8> aad, cont::ConstSpan<u8> ciphertext,
                                   cont::Span<u8> out) const noexcept;

private:
    AgreementKey m_key;
};

// SealBase(pkR, info, aad, plaintext) with a fresh ephemeral key: `enc` is hpke_sizes(suite.kem).public_key long and
// `ciphertext` plaintext.size() + kHpkeTagBytes; the outputs overlap no input and each other (Overlap, nothing
// written).
[[nodiscard]] CryptoError hpke_seal(const HpkeSuite& suite, cont::ConstSpan<u8> peer_public, cont::ConstSpan<u8> info,
                                    cont::ConstSpan<u8> aad, cont::ConstSpan<u8> plaintext, cont::Span<u8> enc,
                                    cont::Span<u8> ciphertext) noexcept;
} // namespace crd::crypto
