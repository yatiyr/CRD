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

// Generation `number` issued at `issued`, valid 30 days with 7 days' grace, accepting every algorithm of the register
// everywhere and preferring the hybrid groups: the tests narrow it with refuse_rule() and prefer_rule().
[[nodiscard]] inline PolicyContent accepting_all(u64 number, u64 issued) noexcept
{
    constexpr u64 day = 86400U;
    PolicyContent content;
    content.generation    = number;
    content.issued        = issued;
    content.expires       = issued + 30U * day;
    content.offline_grace = static_cast<u32>(7U * day);
    for (const AlgorithmId algorithm : kAlgorithmIds)
    {
        const bool hybrid = algorithm_class(algorithm) == AlgorithmClass::HybridKeyExchange;
        content.rules[content.rule_count++] = {algorithm, 0U, 0U, hybrid ? kNever : 0U, kNever};
    }
    return content;
}

// Sets `algorithm`'s rule in `scope` (appended when the generation has none) to `rule`'s window.
inline void set_rule(PolicyContent& content, AlgorithmRule rule) noexcept
{
    for (u32 i = 0U; i < content.rule_count; ++i)
    {
        if (content.rules[i].algorithm == rule.algorithm && content.rules[i].scope == rule.scope)
        {
            content.rules[i] = rule;
            return;
        }
    }
    content.rules[content.rule_count++] = rule;
}

// `algorithm` refused in `scope` since 1970, or from `from`; preferred there from 1970.
inline void refuse_rule(PolicyContent& content, AlgorithmId algorithm, PolicyScope scope = PolicyScope::Everywhere,
                        u64 from = 1U) noexcept
{
    set_rule(content, {algorithm, 0U, 0U, 0U, from, scope});
}

inline void prefer_rule(PolicyContent& content, AlgorithmId algorithm,
                        PolicyScope scope = PolicyScope::Everywhere) noexcept
{
    set_rule(content, {algorithm, 0U, 0U, kNever, kNever, scope});
}

// A product's two-key trust set (decision S25): its own keys, none of them Cerid's.
inline constexpr u8 kProductPostQuantumPublic[] = {'p', 'r', 'o', 'd', '-', 'p', 'q'};
inline constexpr u8 kProductClassicalPublic[]   = {'p', 'r', 'o', 'd', '-', 'e', 'c'};

inline const TrustKey kProductTrust[] = {
    {test_key_id(0x40U), AlgorithmId::MlDsa65, containers::ConstSpan<u8>(kProductPostQuantumPublic)},
    {test_key_id(0x50U), AlgorithmId::EcdsaSecp256r1Sha256, containers::ConstSpan<u8>(kProductClassicalPublic)},
};

// `content` signed by every key of `trust` (at most kMaxTrustKeys) and written to `out`.
[[nodiscard]] inline PolicyError sign_generation(containers::ConstSpan<TrustKey> trust, const PolicyContent& content,
                                                 containers::Span<u8> out, usize& size) noexcept
{
    u8          body[2048];
    usize       body_size = 0U;
    PolicyError error     = write_policy_body(content, body, body_size);
    if (error != PolicyError::None)
    {
        return error;
    }
    const containers::ConstSpan<u8> message(body, body_size);
    u8                              tags[kMaxTrustKeys][kTestSignatureBytes];
    KeySignature                    signatures[kMaxTrustKeys];
    const usize                     count = trust.size() < kMaxTrustKeys ? trust.size() : kMaxTrustKeys;
    for (usize k = 0U; k < count; ++k)
    {
        test_sign(trust[k].algorithm, trust[k].public_key, message, tags[k]);
        signatures[k] = {trust[k].id, trust[k].algorithm, containers::ConstSpan<u8>(tags[k])};
    }
    return write_policy(message, containers::ConstSpan<KeySignature>(signatures, count), out, size);
}

// A test installation: a trust set (the test one by default), a build supporting the whole register, its store (a
// product layer over `base` when one is given), and apply(), which signs a generation with every trust key and
// applies it at `now`.
struct TestInstallation
{
    explicit TestInstallation(containers::ConstSpan<TrustKey> keys = containers::ConstSpan<TrustKey>(kTestTrust),
                              const PolicyStore* base = nullptr) noexcept
        : trust(keys), store(PolicyConfig{keys, &verifier, containers::ConstSpan<AlgorithmId>(kAlgorithmIds), 0U, base})
    {
    }

    containers::ConstSpan<TrustKey> trust;
    TestVerifier                    verifier;
    PolicyStore                     store;

    [[nodiscard]] PolicyError apply(const PolicyContent& content, u64 now) noexcept
    {
        u8                bytes[4096];
        usize             size  = 0U;
        const PolicyError error = sign_generation(trust, content, bytes, size);
        return error != PolicyError::None ? error : store.apply(containers::ConstSpan<u8>(bytes, size), now);
    }
};
} // namespace crd::crypto::test
