#pragma once

// crd-crypto -- the signature verification interface (NET-STD; contract: docs/design/network-foundation.md#crypto).
//
// Cerid writes no cryptographic primitive. A provider (AWS-LC, OpenSSL, BoringSSL or a verified library, NET.2)
// implements this interface; code that checks a signed artefact (policy generations here, SUPPLY's signed fixes and
// rule generations later) names the algorithm by its register identifier and never by a provider's own constant, so
// swapping the provider changes no artefact.

#include <crd/containers/span.hpp>
#include <crd/crypto/register_ids.hpp>

namespace crd::crypto
{
enum class Verification : u8
{
    Valid,       // the signature is the key's signature of the message
    Invalid,     // it is not, or the key or the signature is malformed for the algorithm
    Unsupported, // this provider does not implement the algorithm
};

class SignatureVerifier
{
public:
    SignatureVerifier()                                    = default;
    SignatureVerifier(const SignatureVerifier&)            = delete;
    SignatureVerifier& operator=(const SignatureVerifier&) = delete;
    SignatureVerifier(SignatureVerifier&&)                 = delete;
    SignatureVerifier& operator=(SignatureVerifier&&)      = delete;
    virtual ~SignatureVerifier()                           = default;

    // Whether `signature` is the signature of `message` under `public_key` with `algorithm` (pure signing of the
    // whole message: ML-DSA with an empty context string, Ed25519 without prehash, ECDSA over the hash its
    // identifier names). Safe to call from several threads at once.
    [[nodiscard]] virtual Verification verify(AlgorithmId algorithm, containers::ConstSpan<u8> public_key,
                                              containers::ConstSpan<u8> message,
                                              containers::ConstSpan<u8> signature) const noexcept = 0;
};
} // namespace crd::crypto
