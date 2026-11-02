#pragma once

// crd-crypto -- what every signed Cerid artefact shares (policy generations, capability tokens and revocation
// generations): the signature list after the signed bytes and the check that every required key signed them. Layout,
// every integer big-endian:
//
//   count u8 | count x (key id 32 bytes, algorithm u16, length u16, length bytes)
//
// Private to engine/foundation/crypto/src; each artefact maps these results to its own errors, in its own order.

#include <crd/containers/wire_bytes.hpp>
#include <crd/crypto/policy.hpp>

namespace crd::crypto::signed_artifact
{
inline constexpr usize kEntryHeadBytes = kKeyIdBytes + 4U;

// a + b, or kNever where that would pass it: dates near kNever never wrap.
[[nodiscard]] inline u64 saturating_add(u64 a, u64 b) noexcept
{
    return a > kNever - b ? kNever : a + b;
}

enum class Error : u8
{
    None,
    Truncated, // the bytes end inside the list
    TooLarge,  // more entries than the artefact allows, or a signature above kMaxSignatureBytes
    Unsigned,  // no entry
    BadEntry,  // an empty signature
    Duplicate, // two entries by one key
};

[[nodiscard]] bool is_signature(AlgorithmId algorithm) noexcept;

// Reads the count and the entries into `out` (its size is the artefact's bound); the signatures view the reader's
// bytes. Checks the structure only, in reading order: a count of 0 is Unsigned, one above out.size() and a length
// above kMaxSignatureBytes TooLarge; empty and duplicate entries are validate()'s.
[[nodiscard]] Error read(cont::WireReader& reader, cont::Span<KeySignature> out, u32& count) noexcept;

// Unsigned, TooLarge (above `capacity`), then per entry BadEntry, TooLarge and Duplicate.
[[nodiscard]] Error validate(cont::ConstSpan<KeySignature> signatures, usize capacity) noexcept;

[[nodiscard]] usize encoded_size(cont::ConstSpan<KeySignature> signatures) noexcept;
void                write(cont::WireWriter& writer, cont::ConstSpan<KeySignature> signatures) noexcept;

enum class Trust : u8
{
    Valid,
    Missing,     // a required key did not sign
    Mismatch,    // its entry names another algorithm than the key
    Unsupported, // the verifier does not implement the key's algorithm
    Invalid,     // its signature does not verify
};

// Whether every key of `required`, in its order, signed `message`: the first failure is the answer. Entries by other
// keys are ignored.
[[nodiscard]] Trust verify_all(cont::ConstSpan<TrustKey> required, const SignatureVerifier& verifier,
                               cont::ConstSpan<u8> message, cont::ConstSpan<KeySignature> signatures) noexcept;
} // namespace crd::crypto::signed_artifact
