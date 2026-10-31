#pragma once

// A deterministic stand-in for a signature provider, shared by the policy tests and the policy fuzz target. A
// "signature" is 16 bytes derived with FNV-1a from the algorithm, the public key and the message: it lets the tests
// sign any generation and forge any signature, and proves the policy logic, never cryptography (Cerid writes no
// primitive; NET.2 qualifies its providers with real keys and known-answer vectors).

#include <crd/containers/hash.hpp>
#include <crd/core/fnv1a.hpp>
#include <crd/crypto/policy.hpp>

#include <atomic>

namespace crd::crypto::test
{
inline constexpr usize kTestSignatureBytes = 16U;

inline void test_sign(AlgorithmId algorithm, containers::ConstSpan<u8> public_key, containers::ConstSpan<u8> message,
                      u8 (&out)[kTestSignatureBytes]) noexcept
{
    const u64 keyed = fnv1a_64(message.data(), message.size()) ^
                      containers::hash_u64(fnv1a_64(public_key.data(), public_key.size()));
    const u64 words[2] = {keyed, containers::hash_u64(keyed + static_cast<u16>(algorithm))};
    for (usize i = 0U; i < kTestSignatureBytes; ++i)
    {
        out[i] = static_cast<u8>(words[i / 8U] >> (8U * (i % 8U)));
    }
}

class TestVerifier final : public SignatureVerifier
{
public:
    explicit TestVerifier(AlgorithmId unsupported = AlgorithmId{}) noexcept : m_unsupported(unsupported) {}

    [[nodiscard]] Verification verify(AlgorithmId algorithm, containers::ConstSpan<u8> public_key,
                                      containers::ConstSpan<u8> message,
                                      containers::ConstSpan<u8> signature) const noexcept override
    {
        m_calls.fetch_add(1U, std::memory_order_relaxed);
        if (algorithm == m_unsupported)
        {
            return Verification::Unsupported;
        }
        u8 expected[kTestSignatureBytes];
        test_sign(algorithm, public_key, message, expected);
        if (signature.size() != kTestSignatureBytes)
        {
            return Verification::Invalid;
        }
        for (usize i = 0U; i < kTestSignatureBytes; ++i)
        {
            if (signature[i] != expected[i])
            {
                return Verification::Invalid;
            }
        }
        return Verification::Valid;
    }

    [[nodiscard]] u32 calls() const noexcept { return m_calls.load(std::memory_order_relaxed); }

private:
    AlgorithmId              m_unsupported;
    mutable std::atomic<u32> m_calls{0U};
};

// The two-key trust set of the tests: one post-quantum key and one traditional key.
inline constexpr u8 kPostQuantumPublic[] = {'p', 'q', '-', 'k', 'e', 'y'};
inline constexpr u8 kClassicalPublic[]   = {'e', 'd', '-', 'k', 'e', 'y'};

[[nodiscard]] inline KeyId test_key_id(u8 tag) noexcept
{
    KeyId id;
    for (usize i = 0U; i < kKeyIdBytes; ++i)
    {
        id.bytes[i] = static_cast<u8>(tag + i);
    }
    return id;
}

inline const TrustKey kTestTrust[] = {
    {test_key_id(0x10U), AlgorithmId::MlDsa87, containers::ConstSpan<u8>(kPostQuantumPublic)},
    {test_key_id(0x20U), AlgorithmId::Ed25519, containers::ConstSpan<u8>(kClassicalPublic)},
};
} // namespace crd::crypto::test
