#pragma once

// crd-crypto -- what provider.cpp shares with tls.cpp: the provider's key objects behind the opaque ProviderKey, the
// error-queue scope and a provider key's signature algorithm and public key in the interface's encoding. Private to
// engine/foundation/crypto/src, the only directory that includes the provider's headers.

#include <crd/crypto/provider.hpp>

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
} // namespace crd::crypto::provider_internal
