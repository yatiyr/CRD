#pragma once

// crd-crypto -- what provider.cpp shares with tls.cpp: the provider's key objects behind the opaque ProviderKey, the
// error-queue scope and a provider key's signature algorithm and public key in the interface's encoding. Private to
// engine/foundation/crypto/src, the only directory that includes the provider's headers.

#include <crd/crypto/provider.hpp>

#include "byte_spans.hpp"

#include <crd/core/assert.hpp>

#include <openssl/base.h>
#include <openssl/err.h>

namespace crd::crypto::provider_internal
{
// Clears the provider's error queue when a scope ends: a refused operation leaves no state behind on its thread. An
// empty queue is left as it is: clearing walks every slot of the thread's queue, a cost each record of a session paid
// for nothing (the crypto boards, docs/bench/2026-11-03-net-2-crypto-boards.md). A path whose every provider call
// succeeded says so with succeeded(): a call that succeeds queues no error, so the scope skips even the look at the
// queue (a thread-local lookup in the provider, each record's three cost about 10 ns), and an asserting build checks
// that the queue is indeed empty.
struct ErrorScope
{
    ErrorScope() noexcept = default;
    ErrorScope(const ErrorScope&)            = delete;
    ErrorScope& operator=(const ErrorScope&) = delete;
    ~ErrorScope()
    {
        if (m_succeeded)
        {
            CRD_ASSERT_MSG(ERR_peek_error() == 0U, "a provider call that succeeded queued an error");
            return;
        }
        if (ERR_peek_error() != 0U)
        {
            ERR_clear_error();
        }
    }
    void succeeded() noexcept { m_succeeded = true; }

private:
    bool m_succeeded = false;
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

// ---- ECDSA keys a device holds (key_store.cpp's DeviceSigner, tls.cpp's credentials: decision K12) ----

// The provider key object (the caller owns it) of a signature public key in the interface's encoding; null for an
// algorithm or a key it refuses.
[[nodiscard]] EVP_PKEY* signature_public_pkey(AlgorithmId algorithm, cont::ConstSpan<u8> encoded) noexcept;

// The length of an ECDSA curve's field elements (32 for P-256, 48 for P-384), the length of each of r and s in the
// r || s form devices sign into; 0 for any other algorithm.
[[nodiscard]] usize ecdsa_field_bytes(AlgorithmId algorithm) noexcept;

// The digest an ECDSA identifier names, over `message`, into `out` (exactly the digest's length).
[[nodiscard]] CryptoError ecdsa_digest(AlgorithmId algorithm, cont::ConstSpan<u8> message, cont::Span<u8> out) noexcept;

// A device's r || s (2 * ecdsa_field_bytes) as the DER Ecdsa-Sig-Value of decision P4 into `out` (at least
// signature_sizes(algorithm).signature long); `written` is its length.
[[nodiscard]] CryptoError ecdsa_der_of(AlgorithmId algorithm, cont::ConstSpan<u8> raw, cont::Span<u8> out,
                                       usize& written) noexcept;

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
