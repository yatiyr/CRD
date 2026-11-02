#pragma once

// crd-crypto -- what provider.cpp shares with tls.cpp: the provider's key objects behind the opaque ProviderKey, the
// error-queue scope and a provider key's signature algorithm and public key in the interface's encoding. Private to
// engine/foundation/crypto/src, the only directory that includes the provider's headers.

#include <crd/crypto/provider.hpp>

#include "byte_spans.hpp"

#include <openssl/base.h>
#include <openssl/err.h>

namespace crd::crypto::provider_internal
{
// Clears the provider's error queue when a scope ends: a refused operation leaves no state behind on its thread.
struct ErrorScope
{
    ErrorScope() noexcept = default;
    ErrorScope(const ErrorScope&)            = delete;
    ErrorScope& operator=(const ErrorScope&) = delete;
    ~ErrorScope() { ERR_clear_error(); }
};

// The key objects hold the provider's EVP_PKEY behind the opaque ProviderKey: a pointer cast there and back, never
// dereferenced as ProviderKey, so the public header names no provider type and holding a key costs no extra object.
inline EVP_PKEY* pkey_of(ProviderKey* key) noexcept
{
    return reinterpret_cast<EVP_PKEY*>(key);
}

inline ProviderKey* key_of(EVP_PKEY* pkey) noexcept
{
    return reinterpret_cast<ProviderKey*>(pkey);
}

// The signature algorithm of a provider key from any source (a peer's certificate included): ML-DSA-44/65/87,
// Ed25519 or ECDSA on P-256 or P-384, the curve naming the digest; AlgorithmId{} for every other key.
[[nodiscard]] AlgorithmId signature_algorithm_of(const EVP_PKEY* pkey) noexcept;

// The public key of `pkey`, a key of signature algorithm `algorithm`, in the interface's encoding (decision P4) into
// `out`, which is signature_sizes(algorithm).public_key long.
[[nodiscard]] CryptoError signature_public_bytes(const EVP_PKEY* pkey, AlgorithmId algorithm,
                                                 cont::Span<u8> out) noexcept;

// The digest an ECDSA identifier names; null for ML-DSA and Ed25519, which sign the message itself.
[[nodiscard]] const EVP_MD* signature_digest(AlgorithmId algorithm) noexcept;

using detail::label_of;

// ---- Sealing keys: what a server seals for itself alone (tls.cpp's tickets, quic.cpp's address tokens) ----

inline constexpr usize kSealingKeyBytes = 32U; // a sealing key and the AES-256-GCM key derived from it

// A sealing key's public name, which leads what it seals: HKDF-SHA256 of the key with an empty salt, expanded under
// `label` into `name` (1 to 32 bytes).
[[nodiscard]] CryptoError sealing_key_name(cont::ConstSpan<u8> key, cont::ConstSpan<u8> label,
                                           cont::Span<u8> name) noexcept;

// The AES-256-GCM key and nonce that seal one item: HKDF-SHA256 of the sealing key (kSealingKeyBytes) salted with the
// item's own random salt, expanded under `label`. Every derived key seals one item, so no count of items has to be
// shared across the servers of a fleet that share the sealing key.
[[nodiscard]] CryptoError salted_aead(cont::ConstSpan<u8> key, cont::ConstSpan<u8> salt, cont::ConstSpan<u8> label,
                                      AeadKey& aead, u8 (&nonce)[kAeadNonceBytes]) noexcept;
} // namespace crd::crypto::provider_internal
