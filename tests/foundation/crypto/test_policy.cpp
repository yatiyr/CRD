// crd-crypto -- signed crypto policy generations (NET-STD; docs/design/network-foundation.md#agility and
// #policy-wire): the writers and the exact decoder, the hybrid trust set, monotonic adoption, dates and the offline
// grace period, migration windows, the build's ceiling, the remote disable, live swap between sessions, protocol
// minimums, ignore-unknown and every refusal; an installation's saved state (#policy-state: its generation and floor
// across restarts and processes, damage and failed saves) and the generated bootstrap verified under its trust set.

#include "policy_bootstrap_fixture.hpp"
#include "policy_test_signer.hpp"
#include "temp_directory.hpp"

#include <crd/containers/wire_bytes.hpp>
#include <crd/crypto/policy.hpp>
#include <crd/crypto/policy_state.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/platform/durable_file.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <thread>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireWriter;
using namespace crd::crypto;
using crd::crypto::test::kTestSignatureBytes;
using crd::crypto::test::kTestTrust;
using crd::crypto::test::test_key_id;
using crd::crypto::test::test_sign;
using crd::crypto::test::TempDirectory;
using crd::crypto::test::TestVerifier;
namespace fs = crd::platform::fs;

constexpr u64 kDay = 86400U;
constexpr u64 kT0  = 1800000000U; // 2027-01-15

// Every algorithm of the register except Secp256r1Mlkem768: the build's ceiling.
constexpr AlgorithmId kSupported[] = {
    AlgorithmId::X25519Mlkem768,
    AlgorithmId::Secp384r1Mlkem1024,
    AlgorithmId::X25519,
    AlgorithmId::Secp256r1,
    AlgorithmId::Secp384r1,
    AlgorithmId::MlDsa44,
    AlgorithmId::MlDsa65,
    AlgorithmId::MlDsa87,
    AlgorithmId::Ed25519,
    AlgorithmId::EcdsaSecp256r1Sha256,
    AlgorithmId::EcdsaSecp384r1Sha384,
    AlgorithmId::Aes128Gcm,
    AlgorithmId::Aes256Gcm,
    AlgorithmId::Chacha20Poly1305,
    AlgorithmId::HkdfSha256,
    AlgorithmId::HkdfSha384,
};

// A generation issued at `issued`: valid 30 days, 7 days of offline grace, game protocol at least 1.2, and four
// rules: the hybrid preferred forever, classical X25519 preferred for 5 days and refused after 10 (the dated
// refusal), ChaCha20-Poly1305 always accepted and preferred on days 2 to 4, and a hybrid the build does not hold.
PolicyContent base(u64 generation, u64 issued = kT0)
{
    PolicyContent content;
    content.generation    = generation;
    content.issued        = issued;
    content.expires       = issued + 30U * kDay;
    content.offline_grace = 7U * kDay;
    content.minimums[0]   = {0x0002U, 1U, 2U};
    content.minimum_count = 1U;
    content.rules[0]      = {AlgorithmId::X25519Mlkem768, kT0, kT0, kNever, kNever};
    content.rules[1]      = {AlgorithmId::X25519, kT0, kT0, kT0 + 5U * kDay, kT0 + 10U * kDay};
    content.rules[2]      = {AlgorithmId::Chacha20Poly1305, 0U, kT0 + 2U * kDay, kT0 + 4U * kDay, kNever};
    content.rules[3]      = {AlgorithmId::Secp256r1Mlkem768, 0U, 0U, 0U, kNever};
    content.rule_count    = 4U;
    return content;
}

struct Signed
{
    u8    bytes[4096] = {};
    usize size        = 0U;

    [[nodiscard]] ConstSpan<u8> view() const { return ConstSpan<u8>(bytes, size); }
};

// Who signs a generation in sign(): one bit per signer.
constexpr u32 kPostQuantum    = 1U;
constexpr u32 kClassical      = 2U;
constexpr u32 kStranger       = 4U;  // a key outside the trust set
constexpr u32 kForgeClassical = 8U;  // the classical key's signature with a flipped byte
constexpr u32 kWrongAlgorithm = 16U; // the classical key's signature labelled with another algorithm
constexpr u32 kHonest         = kPostQuantum | kClassical;

const u8    kStrangerPublic[] = {'s', 't', 'r', 'a', 'n', 'g', 'e', 'r'};
const KeyId kStrangerId       = test_key_id(0x30U);

Signed sign(const PolicyContent& content, u32 signers = kHonest)
{
    u8    body[2048];
    usize body_size = 0U;
    REQUIRE(write_policy_body(content, body, body_size) == PolicyError::None);
    const ConstSpan<u8> message(body, body_size);

    KeySignature    signatures[3];
    u8              tags[3][kTestSignatureBytes];
    u32             count = 0U;
    const auto      add   = [&](const KeyId& id, AlgorithmId algorithm, ConstSpan<u8> key, AlgorithmId label)
    {
        test_sign(algorithm, key, message, tags[count]);
        signatures[count] = {id, label, ConstSpan<u8>(tags[count])};
        ++count;
    };
    if ((signers & kPostQuantum) != 0U)
    {
        add(kTestTrust[0].id, kTestTrust[0].algorithm, kTestTrust[0].public_key, kTestTrust[0].algorithm);
    }
    if ((signers & (kClassical | kForgeClassical | kWrongAlgorithm)) != 0U)
    {
        const AlgorithmId label =
            (signers & kWrongAlgorithm) != 0U ? AlgorithmId::EcdsaSecp256r1Sha256 : kTestTrust[1].algorithm;
        add(kTestTrust[1].id, kTestTrust[1].algorithm, kTestTrust[1].public_key, label);
        if ((signers & kForgeClassical) != 0U)
        {
            tags[count - 1U][0] ^= 0x01U;
        }
    }
    if ((signers & kStranger) != 0U)
    {
        add(kStrangerId, AlgorithmId::Ed25519, ConstSpan<u8>(kStrangerPublic), AlgorithmId::Ed25519);
    }
    Signed out;
    REQUIRE(write_policy(message, ConstSpan<KeySignature>(signatures, count), out.bytes, out.size) ==
            PolicyError::None);
    return out;
}

PolicyConfig config(const TestVerifier& verifier, u64 floor = 0U)
{
    return {ConstSpan<TrustKey>(kTestTrust), &verifier, ConstSpan<AlgorithmId>(kSupported), floor};
}

usize body_size_of(const PolicyContent& content)
{
    return policy_body_size(content);
}

// A generation with hand-written signature entries after the body of `content`.
struct RawEntry
{
    KeyId       key;
    AlgorithmId algorithm{};
    u16         length = 0U;
};

Signed with_entries(const PolicyContent& content, u32 count, const RawEntry* entries, u32 written)
{
    Signed out;
    REQUIRE(write_policy_body(content, out.bytes, out.size) == PolicyError::None);
    WireWriter writer(Span<u8>(out.bytes + out.size, sizeof(out.bytes) - out.size));
    writer.put_u8(static_cast<u8>(count));
    for (u32 i = 0U; i < written; ++i)
    {
        writer.put_bytes(ConstSpan<u8>(entries[i].key.bytes, kKeyIdBytes));
        writer.put_u16(static_cast<u16>(entries[i].algorithm));
        writer.put_u16(entries[i].length);
        for (u16 b = 0U; b < entries[i].length && b < 64U; ++b)
        {
            writer.put_u8(0x5AU);
        }
    }
    out.size += writer.written();
    return out;
}

PolicyError decode(ConstSpan<u8> bytes)
{
    PolicyMessage message;
    return decode_policy(bytes, message);
}
} // namespace

TEST_CASE("Policy: an honest generation round-trips through the writers and the decoder", "[crypto][net-std]")
{
    const PolicyContent content = base(7U);
    const Signed        encoded = sign(content, kHonest | kStranger);
    REQUIRE(encoded.size == body_size_of(content) + 1U + 3U * (kKeyIdBytes + 4U + kTestSignatureBytes));

    PolicyMessage message;
    REQUIRE(decode_policy(encoded.view(), message) == PolicyError::None);
    CHECK(message.format == kPolicyFormat);
    CHECK(std::memcmp(encoded.bytes, kPolicyMagic, sizeof(kPolicyMagic)) == 0);
    CHECK(message.signed_bytes.data() == encoded.bytes);
    CHECK(message.signed_bytes.size() == body_size_of(content));
    CHECK(message.content.generation == 7U);
    CHECK(message.content.issued == kT0);
    CHECK(message.content.expires == kT0 + 30U * kDay);
    CHECK(message.content.offline_grace == 7U * kDay);
    REQUIRE(message.content.minimum_count == 1U);
    CHECK(message.content.minimums[0].protocol == 0x0002U);
    CHECK(message.content.minimums[0].major == 1U);
    CHECK(message.content.minimums[0].minor == 2U);
    REQUIRE(message.content.rule_count == 4U);
    for (u32 i = 0U; i < 4U; ++i)
    {
        CHECK(message.content.rules[i].algorithm == content.rules[i].algorithm);
        CHECK(message.content.rules[i].accepted_from == content.rules[i].accepted_from);
        CHECK(message.content.rules[i].preferred_from == content.rules[i].preferred_from);
        CHECK(message.content.rules[i].preferred_until == content.rules[i].preferred_until);
        CHECK(message.content.rules[i].refused_from == content.rules[i].refused_from);
    }
    REQUIRE(message.signature_count == 3U);
    CHECK(message.signatures[0].key == kTestTrust[0].id);
    CHECK(message.signatures[0].algorithm == AlgorithmId::MlDsa87);
    CHECK(message.signatures[1].key == kTestTrust[1].id);
    CHECK(message.signatures[2].key == kStrangerId);
    for (u32 i = 0U; i < 3U; ++i)
    {
        CHECK(message.signatures[i].signature.size() == kTestSignatureBytes);
        CHECK(message.signatures[i].signature.data() >= encoded.bytes);
        CHECK(message.signatures[i].signature.data() < encoded.bytes + encoded.size);
    }
}

TEST_CASE("Policy: content validation refuses every malformed field", "[crypto][net-std]")
{
    CHECK(validate_content(base(1U)) == PolicyError::None);

    PolicyContent content = base(0U);
    CHECK(validate_content(content) == PolicyError::BadGeneration);

    content = base(1U);
    content.expires = content.issued;
    CHECK(validate_content(content) == PolicyError::BadDates);
    content.expires = content.issued + kMaxPolicyLifetime;
    CHECK(validate_content(content) == PolicyError::None);
    content.expires = content.issued + kMaxPolicyLifetime + 1U;
    CHECK(validate_content(content) == PolicyError::BadDates);
    content = base(1U);
    content.offline_grace = kMaxOfflineGrace;
    CHECK(validate_content(content) == PolicyError::None);
    content.offline_grace = kMaxOfflineGrace + 1U;
    CHECK(validate_content(content) == PolicyError::BadDates);

    const AlgorithmRule broken[] = {
        {AlgorithmId{}, 0U, 0U, 0U, kNever},            // algorithm 0
        {AlgorithmId::Aes128Gcm, 5U, 4U, 6U, 7U},       // preferred before accepted
        {AlgorithmId::Aes128Gcm, 1U, 5U, 4U, 7U},       // preferred window reversed
        {AlgorithmId::Aes128Gcm, 1U, 2U, 8U, 7U},       // preferred after refusal
        {AlgorithmId::Aes128Gcm, 7U, 7U, 7U, 7U},       // refused when accepted: an empty window
    };
    for (const AlgorithmRule& rule : broken)
    {
        content          = base(1U);
        content.rules[0] = rule;
        CHECK(validate_content(content) == PolicyError::BadRule);
    }
    content          = base(1U);
    content.rules[1] = {AlgorithmId::Aes128Gcm, 0U, 0U, 0U, 1U}; // never preferred, accepted for one second
    CHECK(validate_content(content) == PolicyError::None);
    content.rules[2].algorithm = AlgorithmId::Aes128Gcm;
    CHECK(validate_content(content) == PolicyError::DuplicateRule);

    content               = base(1U);
    content.minimums[1]   = {0x0002U, 2U, 0U};
    content.minimum_count = 2U;
    CHECK(validate_content(content) == PolicyError::DuplicateMinimum);

    content               = base(1U);
    content.minimum_count = kMaxPolicyMinimums + 1U;
    CHECK(validate_content(content) == PolicyError::TooLarge);
    content            = base(1U);
    content.rule_count = kMaxPolicyRules + 1U;
    CHECK(validate_content(content) == PolicyError::TooLarge);

    u8    out[2048];
    usize written = 99U;
    CHECK(write_policy_body(base(0U), out, written) == PolicyError::BadGeneration);
    CHECK(written == 0U);
}

TEST_CASE("Policy: the most rules and minimums a generation holds round-trip", "[crypto][net-std]")
{
    PolicyContent content = base(3U);
    for (u32 i = 0U; i < kMaxPolicyMinimums; ++i)
    {
        content.minimums[i] = {static_cast<u16>(i + 1U), static_cast<u16>(i), static_cast<u16>(2U * i)};
    }
    content.minimum_count = kMaxPolicyMinimums;
    for (u32 i = 0U; i < kMaxPolicyRules; ++i)
    {
        content.rules[i] = {static_cast<AlgorithmId>(0x7F00U + i + 1U), i, i, i + 1U, i + 2U};
    }
    content.rule_count = kMaxPolicyRules;
    const Signed  encoded = sign(content);
    PolicyMessage message;
    REQUIRE(decode_policy(encoded.view(), message) == PolicyError::None);
    CHECK(message.content.minimum_count == kMaxPolicyMinimums);
    CHECK(message.content.minimums[kMaxPolicyMinimums - 1U].minor == 2U * (kMaxPolicyMinimums - 1U));
    CHECK(message.content.rule_count == kMaxPolicyRules);
    CHECK(message.content.rules[kMaxPolicyRules - 1U].algorithm == static_cast<AlgorithmId>(0x7F00U + kMaxPolicyRules));
}

TEST_CASE("Policy: decoding is exact and bounded", "[crypto][net-std]")
{
    const PolicyContent content = base(2U);
    const Signed        encoded = sign(content);
    const usize         body    = body_size_of(content);
    REQUIRE(decode(encoded.view()) == PolicyError::None);

    for (usize size = 0U; size < encoded.size; ++size)
    {
        INFO("prefix " << size);
        CHECK(decode(ConstSpan<u8>(encoded.bytes, size)) == PolicyError::Truncated);
    }
    Signed copy = encoded;
    copy.size += 1U;
    CHECK(decode(copy.view()) == PolicyError::TrailingBytes);

    copy = encoded;
    copy.bytes[3] = 'X';
    CHECK(decode(copy.view()) == PolicyError::BadMagic);
    copy = encoded;
    copy.bytes[8] = 0U;
    CHECK(decode(copy.view()) == PolicyError::UnsupportedFormat);
    copy.bytes[8] = kPolicyFormat + 1U;
    CHECK(decode(copy.view()) == PolicyError::UnsupportedFormat);
    copy = encoded;
    copy.bytes[37] = static_cast<u8>(kMaxPolicyMinimums + 1U); // the minimum count
    CHECK(decode(copy.view()) == PolicyError::TooLarge);
    copy = encoded;
    copy.bytes[44] = static_cast<u8>(kMaxPolicyRules + 1U); // the rule count after one minimum
    CHECK(decode(copy.view()) == PolicyError::TooLarge);
    copy = encoded;
    std::memset(copy.bytes + 9U, 0, 8U); // generation 0
    CHECK(decode(copy.view()) == PolicyError::BadGeneration);
    copy = encoded;
    copy.bytes[body] = 0U; // no signature
    CHECK(decode(ConstSpan<u8>(copy.bytes, body + 1U)) == PolicyError::Unsigned);
    copy.bytes[body] = static_cast<u8>(kMaxPolicySignatures + 1U);
    CHECK(decode(copy.view()) == PolicyError::TooLarge);
    copy = encoded;
    copy.bytes[body + 1U + kKeyIdBytes + 2U] = 0x20U; // the first signature's length: 8193 bytes
    copy.bytes[body + 1U + kKeyIdBytes + 3U] = 0x01U;
    CHECK(decode(copy.view()) == PolicyError::TooLarge);

    static u8 oversized[kMaxPolicyBytes + 1U];
    std::memcpy(oversized, encoded.bytes, encoded.size);
    CHECK(decode(ConstSpan<u8>(oversized, kMaxPolicyBytes + 1U)) == PolicyError::TooLarge);

    const RawEntry empty[] = {{kTestTrust[0].id, AlgorithmId::MlDsa87, 0U}};
    CHECK(decode(with_entries(content, 1U, empty, 1U).view()) == PolicyError::BadSignatureEntry);
    const RawEntry twice[] = {{kTestTrust[0].id, AlgorithmId::MlDsa87, 4U},
                              {kTestTrust[0].id, AlgorithmId::MlDsa87, 4U}};
    CHECK(decode(with_entries(content, 2U, twice, 2U).view()) == PolicyError::DuplicateSignature);
    CHECK(decode(with_entries(content, 2U, twice, 1U).view()) == PolicyError::Truncated);
}

TEST_CASE("Policy: the writers refuse what a reader would refuse", "[crypto][net-std]")
{
    const PolicyContent content = base(2U);
    u8                  body[2048];
    usize               body_size = 0U;
    REQUIRE(write_policy_body(content, body, body_size) == PolicyError::None);
    CHECK(body_size == body_size_of(content));
    usize written = 1U;
    CHECK(write_policy_body(content, Span<u8>(body, body_size - 1U), written) == PolicyError::BufferTooSmall);
    CHECK(written == 0U);

    static u8           out[kMaxPolicyBytes];
    static const u8     kLarge[kMaxSignatureBytes] = {1U};
    const u8            tag[4]                     = {1U, 2U, 3U, 4U};
    const ConstSpan<u8> message(body, body_size);
    KeySignature        signatures[kMaxPolicySignatures + 1U];
    for (u32 i = 0U; i <= kMaxPolicySignatures; ++i)
    {
        signatures[i] = {test_key_id(static_cast<u8>(0x40U + i)), AlgorithmId::Ed25519, ConstSpan<u8>(tag)};
    }
    CHECK(write_policy(message, ConstSpan<KeySignature>(), out, written) == PolicyError::Unsigned);
    CHECK(write_policy(message, ConstSpan<KeySignature>(signatures, kMaxPolicySignatures + 1U), out, written) ==
          PolicyError::TooLarge);
    CHECK(write_policy(message, ConstSpan<KeySignature>(signatures, kMaxPolicySignatures), out, written) ==
          PolicyError::None);

    KeySignature edited[2] = {signatures[0], signatures[1]};
    edited[1].signature    = ConstSpan<u8>();
    CHECK(write_policy(message, ConstSpan<KeySignature>(edited, 2U), out, written) ==
          PolicyError::BadSignatureEntry);
    edited[1] = signatures[0];
    CHECK(write_policy(message, ConstSpan<KeySignature>(edited, 2U), out, written) ==
          PolicyError::DuplicateSignature);
    const u8 oversize_tag[kMaxSignatureBytes + 1U] = {1U};
    edited[1] = {signatures[1].key, AlgorithmId::Ed25519, ConstSpan<u8>(oversize_tag)};
    CHECK(write_policy(message, ConstSpan<KeySignature>(edited, 2U), out, written) == PolicyError::TooLarge);

    KeySignature full[4];
    for (u32 i = 0U; i < 4U; ++i)
    {
        full[i] = {signatures[i].key, AlgorithmId::MlDsa87, ConstSpan<u8>(kLarge)};
    }
    CHECK(write_policy(message, ConstSpan<KeySignature>(full, 4U), out, written) == PolicyError::TooLarge);
    CHECK(write_policy(message, ConstSpan<KeySignature>(full, 3U), out, written) == PolicyError::None);
    CHECK(write_policy(message, ConstSpan<KeySignature>(full, 3U), Span<u8>(out, written - 1U), written) ==
          PolicyError::BufferTooSmall);

    u8 trailing[2048];
    std::memcpy(trailing, body, body_size);
    trailing[body_size] = 0U;
    CHECK(write_policy(ConstSpan<u8>(trailing, body_size + 1U), ConstSpan<KeySignature>(signatures, 1U), out,
                       written) == PolicyError::TrailingBytes);
    CHECK(write_policy(ConstSpan<u8>(body, body_size - 1U), ConstSpan<KeySignature>(signatures, 1U), out, written) ==
          PolicyError::Truncated);
    trailing[0] = 'c';
    CHECK(write_policy(ConstSpan<u8>(trailing, body_size), ConstSpan<KeySignature>(signatures, 1U), out, written) ==
          PolicyError::BadMagic);

    // What a signer checks before it signs: the body exactly, no more and no less.
    CHECK(check_policy_body(message) == PolicyError::None);
    CHECK(check_policy_body(ConstSpan<u8>(body, body_size - 1U)) == PolicyError::Truncated);
    CHECK(check_policy_body(ConstSpan<u8>(trailing, body_size)) == PolicyError::BadMagic);
    trailing[0] = body[0];
    CHECK(check_policy_body(ConstSpan<u8>(trailing, body_size + 1U)) == PolicyError::TrailingBytes);
    CHECK(check_policy_body(ConstSpan<u8>()) == PolicyError::Truncated);
}

TEST_CASE("Policy: the trust set is hybrid, distinct and complete", "[crypto][net-std]")
{
    const TestVerifier verifier;
    CHECK(validate_config(config(verifier)) == PolicyError::None);

    PolicyConfig broken = config(verifier);
    broken.verifier     = nullptr;
    CHECK(validate_config(broken) == PolicyError::BadConfig);
    broken.verifier = &verifier;
    broken.trust    = ConstSpan<TrustKey>(kTestTrust, 1U);
    CHECK(validate_config(broken) == PolicyError::BadConfig);

    const u8 key[] = {1U};
    TrustKey keys[kMaxTrustKeys + 1U];
    for (u32 i = 0U; i <= kMaxTrustKeys; ++i)
    {
        keys[i] = {test_key_id(static_cast<u8>(0x50U + i)), i == 0U ? AlgorithmId::MlDsa65 : AlgorithmId::Ed25519,
                   ConstSpan<u8>(key)};
    }
    broken.trust = ConstSpan<TrustKey>(keys, kMaxTrustKeys);
    CHECK(validate_config(broken) == PolicyError::None);
    broken.trust = ConstSpan<TrustKey>(keys, kMaxTrustKeys + 1U);
    CHECK(validate_config(broken) == PolicyError::BadConfig);

    TrustKey pair[2] = {kTestTrust[0], kTestTrust[1]};
    broken.trust     = ConstSpan<TrustKey>(pair, 2U);
    pair[1].id       = pair[0].id;
    CHECK(validate_config(broken) == PolicyError::BadConfig); // one key twice
    pair[1]           = kTestTrust[1];
    pair[1].algorithm = AlgorithmId::Aes128Gcm;
    CHECK(validate_config(broken) == PolicyError::BadConfig); // not a signature algorithm
    pair[1]            = kTestTrust[1];
    pair[1].public_key = ConstSpan<u8>();
    CHECK(validate_config(broken) == PolicyError::BadConfig);
    pair[1]           = kTestTrust[1];
    pair[1].algorithm = AlgorithmId::MlDsa44;
    CHECK(validate_config(broken) == PolicyError::BadConfig); // post-quantum only
    pair[0]           = kTestTrust[0];
    pair[0].algorithm = AlgorithmId::EcdsaSecp384r1Sha384;
    pair[1]           = kTestTrust[1];
    CHECK(validate_config(broken) == PolicyError::BadConfig); // traditional only

    PolicyStore store(broken);
    CHECK(store.apply(sign(base(1U)).view(), kT0) == PolicyError::BadConfig);
    CHECK(store.snapshot().generation() == 0U);
}

TEST_CASE("Policy: every trust key's valid signature is required, other keys are ignored", "[crypto][net-std]")
{
    const TestVerifier verifier;
    PolicyStore        store(config(verifier));
    CHECK(store.apply(sign(base(1U), kPostQuantum).view(), kT0) == PolicyError::MissingSignature);
    CHECK(store.apply(sign(base(1U), kClassical).view(), kT0) == PolicyError::MissingSignature);
    CHECK(store.apply(sign(base(1U), kPostQuantum | kForgeClassical).view(), kT0) == PolicyError::BadSignature);
    CHECK(store.apply(sign(base(1U), kPostQuantum | kWrongAlgorithm).view(), kT0) == PolicyError::BadSignature);
    CHECK(store.apply(sign(base(1U), kStranger).view(), kT0) == PolicyError::MissingSignature);
    CHECK(store.snapshot().generation() == 0U);
    CHECK(store.snapshot().freshness(kT0) == Freshness::None);
    CHECK(store.floor_generation() == 0U);

    Signed tampered = sign(base(1U));
    tampered.bytes[43] ^= 0x01U; // the game protocol's minimum minor, under both signatures
    CHECK(store.apply(tampered.view(), kT0) == PolicyError::BadSignature);

    const TestVerifier lacking(AlgorithmId::Ed25519);
    PolicyStore        partial(config(lacking));
    CHECK(partial.apply(sign(base(1U)).view(), kT0) == PolicyError::UnsupportedSignature);

    CHECK(store.apply(sign(base(1U), kHonest | kStranger).view(), kT0) == PolicyError::None);
    CHECK(store.snapshot().generation() == 1U);
    CHECK(store.floor_generation() == 1U);
}

TEST_CASE("Policy: generation numbers only grow", "[crypto][net-std]")
{
    const TestVerifier verifier;
    PolicyStore        store(config(verifier));
    REQUIRE(store.apply(sign(base(2U)).view(), kT0) == PolicyError::None);
    CHECK(store.floor_generation() == 2U);

    const u32 calls = verifier.calls();
    CHECK(store.apply(sign(base(1U)).view(), kT0) == PolicyError::Rollback);
    CHECK(verifier.calls() == calls); // refused before any signature is verified

    // The same generation again, with an extra signature: nothing changes.
    CHECK(store.apply(sign(base(2U), kHonest | kStranger).view(), kT0) == PolicyError::AlreadyCurrent);
    PolicyContent other = base(2U);
    other.offline_grace = kDay;
    CHECK(store.apply(sign(other).view(), kT0) == PolicyError::Conflict);
    CHECK(store.apply(sign(other, kPostQuantum | kForgeClassical).view(), kT0) == PolicyError::BadSignature);
    CHECK(store.snapshot().content().offline_grace == 7U * kDay);

    CHECK(store.apply(sign(base(3U, kT0 - 1U)).view(), kT0) == PolicyError::IssuedBeforeCurrent);
    CHECK(store.apply(sign(base(3U, kT0)).view(), kT0) == PolicyError::None);
    CHECK(store.apply(sign(base(9U, kT0 + kDay)).view(), kT0 + kDay) == PolicyError::None);
    CHECK(store.floor_generation() == 9U);
    CHECK(store.apply(sign(base(3U)).view(), kT0 + kDay) == PolicyError::Rollback);

    // A restart: the persisted floor refuses an older generation and readmits the current one.
    PolicyStore restarted(config(verifier, store.floor_generation()));
    CHECK(restarted.apply(sign(base(8U, kT0 + kDay)).view(), kT0 + kDay) == PolicyError::Rollback);
    CHECK(restarted.apply(sign(base(9U, kT0 + kDay)).view(), kT0 + kDay) == PolicyError::None);
    CHECK(restarted.apply(sign(base(9U, kT0 + kDay)).view(), kT0 + kDay) == PolicyError::AlreadyCurrent);
    CHECK(restarted.floor_generation() == 9U);
}

TEST_CASE("Policy: a store starts from the persisted generation, then the bootstrap", "[crypto][net-std]")
{
    const TestVerifier verifier;
    const Signed       persisted = sign(base(5U, kT0));
    const ConstSpan<u8> none;
    {
        // A fresh installation: no persisted generation, the bootstrap is adopted.
        PolicyStore       store(config(verifier));
        const PolicyStart started = store.start(none, persisted.view(), kT0);
        CHECK(started.persisted == PolicyError::Absent);
        CHECK(started.bootstrap == PolicyError::None);
        CHECK(store.snapshot().generation() == 5U);
    }
    {
        // A floor above the bootstrap keeps the persisted generation; the older bootstrap is a harmless rollback.
        PolicyStore       store(config(verifier, 5U));
        const PolicyStart started = store.start(persisted.view(), sign(base(2U, kT0)).view(), kT0 + kDay);
        CHECK(started.persisted == PolicyError::None);
        CHECK(started.bootstrap == PolicyError::Rollback);
        CHECK(store.snapshot().generation() == 5U);
        CHECK(store.floor_generation() == 5U);
    }
    {
        // The same generation persisted and built in.
        PolicyStore       store(config(verifier, 5U));
        const PolicyStart started = store.start(persisted.view(), persisted.view(), kT0);
        CHECK(started.persisted == PolicyError::None);
        CHECK(started.bootstrap == PolicyError::AlreadyCurrent);
        CHECK(store.snapshot().generation() == 5U);
    }
    {
        // A newer build's bootstrap supersedes the persisted generation.
        PolicyStore       store(config(verifier, 5U));
        const PolicyStart started = store.start(persisted.view(), sign(base(6U, kT0 + kDay)).view(), kT0 + kDay);
        CHECK(started.persisted == PolicyError::None);
        CHECK(started.bootstrap == PolicyError::None);
        CHECK(store.snapshot().generation() == 6U);
        CHECK(store.floor_generation() == 6U);
    }
    {
        // A persisted generation lost or damaged: the bootstrap stands in when the floor allows it.
        Signed damaged = persisted;
        damaged.bytes[damaged.size - 1U] ^= 0x01U;
        PolicyStore       store(config(verifier, 5U));
        const PolicyStart started = store.start(damaged.view(), persisted.view(), kT0);
        CHECK(started.persisted == PolicyError::BadSignature);
        CHECK(started.bootstrap == PolicyError::None);
        CHECK(store.snapshot().generation() == 5U);
    }
    {
        // Neither usable: the persisted one expired past its grace, the bootstrap below the floor. The store stays
        // empty and refuses everything until a newer generation arrives.
        const u64         late = kT0 + 30U * kDay + 7U * kDay;
        PolicyStore       store(config(verifier, 5U));
        const PolicyStart started = store.start(persisted.view(), sign(base(4U, kT0)).view(), late);
        CHECK(started.persisted == PolicyError::Expired);
        CHECK(started.bootstrap == PolicyError::Rollback);
        CHECK(store.snapshot().freshness(late) == Freshness::None);
        CHECK(store.snapshot().status(AlgorithmId::X25519Mlkem768, late) == AlgorithmStatus::Refused);
        CHECK(store.apply(sign(base(7U, late)).view(), late) == PolicyError::None);
        CHECK(store.snapshot().status(AlgorithmId::X25519Mlkem768, late) == AlgorithmStatus::Preferred);
    }
    {
        // Nothing held at all.
        PolicyStore       store(config(verifier));
        const PolicyStart started = store.start(none, none, kT0);
        CHECK(started.persisted == PolicyError::Absent);
        CHECK(started.bootstrap == PolicyError::Absent);
        CHECK(store.snapshot().freshness(kT0) == Freshness::None);
    }
}

TEST_CASE("Policy: issue skew, expiry and the offline grace period", "[crypto][net-std]")
{
    const TestVerifier verifier;
    {
        PolicyStore store(config(verifier));
        CHECK(store.apply(sign(base(1U, kT0 + kMaxIssueSkew + 1U)).view(), kT0) == PolicyError::NotYetIssued);
        CHECK(store.apply(sign(base(1U, kT0 + kMaxIssueSkew)).view(), kT0) == PolicyError::None);
    }
    {
        PolicyStore store(config(verifier));
        const u64   end = kT0 + 30U * kDay + 7U * kDay;
        CHECK(store.apply(sign(base(1U)).view(), end) == PolicyError::Expired);
        CHECK(store.floor_generation() == 0U);
        CHECK(store.apply(sign(base(1U)).view(), end - 1U) == PolicyError::None); // adopted in its grace period

        const PolicySnapshot snapshot = store.snapshot();
        CHECK(snapshot.freshness(kT0) == Freshness::Current);
        CHECK(snapshot.freshness(kT0 + 30U * kDay - 1U) == Freshness::Current);
        CHECK(snapshot.freshness(kT0 + 30U * kDay) == Freshness::Grace);
        CHECK(snapshot.status(AlgorithmId::X25519Mlkem768, kT0 + 30U * kDay) == AlgorithmStatus::Preferred);
        CHECK(snapshot.freshness(end - 1U) == Freshness::Grace);
        CHECK(snapshot.freshness(end) == Freshness::Expired);
        CHECK(snapshot.status(AlgorithmId::X25519Mlkem768, end) == AlgorithmStatus::Refused);
        CHECK(snapshot.status(AlgorithmId::Aes128Gcm, end) == AlgorithmStatus::Refused);
    }
    {
        PolicyContent far = base(1U, kNever - 10U * kDay); // the grace end saturates instead of wrapping
        far.expires       = kNever - kDay;
        PolicyStore store(config(verifier));
        CHECK(store.apply(sign(far).view(), kNever - 10U * kDay) == PolicyError::None);
        CHECK(store.snapshot().freshness(kNever - 1U) == Freshness::Grace);
    }
}

TEST_CASE("Policy: migration windows, the build's ceiling and unknown algorithms", "[crypto][net-std]")
{
    const TestVerifier verifier;
    PolicyStore        store(config(verifier));
    PolicyContent      content = base(1U);
    content.rules[4]           = {static_cast<AlgorithmId>(0x7F01U), 0U, 0U, 0U, kNever}; // a future algorithm
    content.rule_count         = 5U;
    REQUIRE(store.apply(sign(content).view(), kT0) == PolicyError::None);
    const PolicySnapshot snapshot = store.snapshot();

    CHECK(snapshot.status(AlgorithmId::X25519, kT0 - 1U) == AlgorithmStatus::Refused);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0) == AlgorithmStatus::Preferred);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 5U * kDay - 1U) == AlgorithmStatus::Preferred);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 5U * kDay) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 10U * kDay - 1U) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 10U * kDay) == AlgorithmStatus::Refused);
    CHECK(snapshot.status(AlgorithmId::Chacha20Poly1305, kT0) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::Chacha20Poly1305, kT0 + 2U * kDay) == AlgorithmStatus::Preferred);
    CHECK(snapshot.status(AlgorithmId::Chacha20Poly1305, kT0 + 4U * kDay) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::X25519Mlkem768, kT0 + 29U * kDay) == AlgorithmStatus::Preferred);

    // No rule, a rule the build cannot use, and an identifier no build knows: all refused.
    CHECK(snapshot.status(AlgorithmId::Aes256Gcm, kT0) == AlgorithmStatus::Refused);
    CHECK(snapshot.status(AlgorithmId::Secp256r1Mlkem768, kT0) == AlgorithmStatus::Refused);
    CHECK(snapshot.status(static_cast<AlgorithmId>(0x7F01U), kT0) == AlgorithmStatus::Refused);
    REQUIRE(snapshot.content().rule_count == 3U);
    CHECK(snapshot.content().rules[0].algorithm == AlgorithmId::X25519Mlkem768);
    CHECK(snapshot.content().rules[1].algorithm == AlgorithmId::X25519);
    CHECK(snapshot.content().rules[2].algorithm == AlgorithmId::Chacha20Poly1305);

    // A suite (HPKE's, MLS's) is judged whole: Refused when one algorithm is, Preferred when all are, else Accepted;
    // an empty one is Refused.
    const AlgorithmId preferred[] = {AlgorithmId::X25519, AlgorithmId::X25519Mlkem768};
    const AlgorithmId mixed[]     = {AlgorithmId::X25519, AlgorithmId::Chacha20Poly1305};
    const AlgorithmId refused[]   = {AlgorithmId::X25519, AlgorithmId::Aes256Gcm};
    CHECK(snapshot.status(ConstSpan<AlgorithmId>(preferred), kT0) == AlgorithmStatus::Preferred);
    CHECK(snapshot.status(ConstSpan<AlgorithmId>(mixed), kT0) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(ConstSpan<AlgorithmId>(mixed), kT0 + 10U * kDay) == AlgorithmStatus::Refused);
    CHECK(snapshot.status(ConstSpan<AlgorithmId>(refused), kT0) == AlgorithmStatus::Refused);
    CHECK(snapshot.status(ConstSpan<AlgorithmId>(), kT0) == AlgorithmStatus::Refused);

    const PolicySnapshot empty;
    CHECK(empty.freshness(kT0) == Freshness::None);
    CHECK(empty.status(AlgorithmId::X25519Mlkem768, kT0) == AlgorithmStatus::Refused);
    CHECK(empty.status(ConstSpan<AlgorithmId>(preferred), kT0) == AlgorithmStatus::Refused);
}

TEST_CASE("Policy: Cerid service sessions follow their own rules first", "[crypto][net-std]")
{
    // Classical X25519: every session accepts it until day 10 (base), Cerid services only until day 3; a services rule
    // for ChaCha20-Poly1305 alone, which no other session follows; an AES-256-GCM rule in a scope no build knows yet.
    const TestVerifier verifier;
    PolicyStore store(config(verifier));
    PolicyContent content = base(1U);
    content.rules[4] = {AlgorithmId::X25519, kT0, kT0, kT0, kT0 + 3U * kDay, PolicyScope::CeridServices};
    content.rules[5] = {AlgorithmId::Aes128Gcm, 0U, 0U, 0U, kNever, PolicyScope::CeridServices};
    content.rules[6] = {AlgorithmId::Aes256Gcm, 0U, 0U, kNever, kNever, static_cast<PolicyScope>(7U)};
    content.rule_count = 7U;
    REQUIRE(validate_content(content) == PolicyError::None);
    const Signed encoded = sign(content);
    REQUIRE(encoded.size == body_size_of(content) + 1U + 2U * (kKeyIdBytes + 4U + kTestSignatureBytes));
    PolicyMessage message;
    REQUIRE(decode_policy(encoded.view(), message) == PolicyError::None);
    for (u32 i = 0U; i < content.rule_count; ++i)
    {
        CHECK(message.content.rules[i].scope == content.rules[i].scope);
    }
    REQUIRE(store.apply(encoded.view(), kT0) == PolicyError::None);
    const PolicySnapshot snapshot = store.snapshot();

    constexpr PolicyScope services = PolicyScope::CeridServices;
    CHECK(snapshot.status(AlgorithmId::X25519, kT0) == AlgorithmStatus::Preferred);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0, services) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 3U * kDay - 1U, services) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 3U * kDay, services) == AlgorithmStatus::Refused);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 3U * kDay) == AlgorithmStatus::Preferred);
    CHECK(snapshot.status(AlgorithmId::X25519, kT0 + 10U * kDay) == AlgorithmStatus::Refused);
    // Without a services rule a service session follows the Everywhere rule; a services-only rule binds nothing else.
    CHECK(snapshot.status(AlgorithmId::X25519Mlkem768, kT0, services) == AlgorithmStatus::Preferred);
    CHECK(snapshot.status(AlgorithmId::Chacha20Poly1305, kT0 + 2U * kDay, services) == AlgorithmStatus::Preferred);
    CHECK(snapshot.status(AlgorithmId::Aes128Gcm, kT0, services) == AlgorithmStatus::Accepted);
    CHECK(snapshot.status(AlgorithmId::Aes128Gcm, kT0) == AlgorithmStatus::Refused);
    // The unknown scope's rule stays signed but governs nothing in this build.
    CHECK(snapshot.status(AlgorithmId::Aes256Gcm, kT0) == AlgorithmStatus::Refused);
    CHECK(snapshot.status(AlgorithmId::Aes256Gcm, kT0, services) == AlgorithmStatus::Refused);
    CHECK(snapshot.status(AlgorithmId::Aes256Gcm, kT0, static_cast<PolicyScope>(7U)) == AlgorithmStatus::Refused);
    REQUIRE(snapshot.content().rule_count == 5U);
    CHECK(snapshot.content().rules[3].scope == services);

    // One rule per algorithm and scope; the same generation number with a rule moved to another scope conflicts.
    PolicyContent twice = content;
    twice.rules[5] = {AlgorithmId::X25519, 0U, 0U, 0U, kNever, services};
    CHECK(validate_content(twice) == PolicyError::DuplicateRule);
    PolicyContent moved = content;
    moved.rules[5].scope = PolicyScope::Everywhere;
    CHECK(store.apply(sign(moved).view(), kT0) == PolicyError::Conflict);
    CHECK(store.apply(sign(content).view(), kT0) == PolicyError::AlreadyCurrent);
}

TEST_CASE("Policy: the N-1 reader takes format 1 generations as Everywhere rules", "[crypto][net-std]")
{
    // Format 1 is format 2 without the scope byte after each rule's algorithm.
    const PolicyContent content = base(4U);
    u8 body[2048];
    usize size = 0U;
    REQUIRE(write_policy_body(content, body, size) == PolicyError::None);
    REQUIRE(body[8] == 2U);
    const usize rules = 38U + 6U * content.minimum_count + 1U;
    REQUIRE(size == rules + 35U * content.rule_count);
    u8 older[2048];
    usize at = rules;
    std::memcpy(older, body, rules);
    older[8] = 1U;
    for (u32 i = 0U; i < content.rule_count; ++i)
    {
        const u8* rule = body + rules + 35U * i;
        CHECK(rule[2] == static_cast<u8>(PolicyScope::Everywhere));
        std::memcpy(older + at, rule, 2U);
        std::memcpy(older + at + 2U, rule + 3U, 32U);
        at += 34U;
    }
    const ConstSpan<u8> message(older, at);
    u8 tags[2][kTestSignatureBytes];
    KeySignature signatures[2];
    for (u32 k = 0U; k < 2U; ++k)
    {
        test_sign(kTestTrust[k].algorithm, kTestTrust[k].public_key, message, tags[k]);
        signatures[k] = {kTestTrust[k].id, kTestTrust[k].algorithm, ConstSpan<u8>(tags[k])};
    }
    Signed encoded;
    REQUIRE(write_policy(message, ConstSpan<KeySignature>(signatures), encoded.bytes, encoded.size) ==
            PolicyError::None);

    PolicyMessage decoded;
    REQUIRE(decode_policy(encoded.view(), decoded) == PolicyError::None);
    CHECK(decoded.format == 1U);
    CHECK(decoded.signed_bytes.size() == at);
    REQUIRE(decoded.content.rule_count == content.rule_count);
    for (u32 i = 0U; i < content.rule_count; ++i)
    {
        CHECK(decoded.content.rules[i].algorithm == content.rules[i].algorithm);
        CHECK(decoded.content.rules[i].scope == PolicyScope::Everywhere);
        CHECK(decoded.content.rules[i].accepted_from == content.rules[i].accepted_from);
        CHECK(decoded.content.rules[i].refused_from == content.rules[i].refused_from);
    }
    // A truncated format 1 rule, and format 1 bytes read as format 2, are refused.
    CHECK(decode(ConstSpan<u8>(encoded.bytes, at - 1U)) == PolicyError::Truncated);
    Signed relabelled = encoded;
    relabelled.bytes[8] = 2U;
    CHECK(decode(relabelled.view()) != PolicyError::None);

    const TestVerifier verifier;
    PolicyStore store(config(verifier));
    REQUIRE(store.apply(encoded.view(), kT0) == PolicyError::None);
    CHECK(store.snapshot().status(AlgorithmId::X25519, kT0, PolicyScope::CeridServices) == AlgorithmStatus::Preferred);
    // The same content again in format 2 is the current generation, not a conflict.
    CHECK(store.apply(sign(content).view(), kT0) == PolicyError::AlreadyCurrent);
}

TEST_CASE("Policy: a remote disable reaches new sessions while running ones keep their snapshot", "[crypto][net-std]")
{
    const TestVerifier verifier;
    PolicyStore        store(config(verifier));
    REQUIRE(store.apply(sign(base(1U)).view(), kT0) == PolicyError::None);
    const PolicySnapshot running = store.snapshot();

    // Generation 2 refuses ChaCha20-Poly1305 from its own issue time (a broken algorithm) and adds AES-256-GCM.
    PolicyContent disable = base(2U, kT0 + kDay);
    disable.rules[2]      = {AlgorithmId::Chacha20Poly1305, 0U, 0U, 0U, kT0 + kDay};
    disable.rules[3]      = {AlgorithmId::Aes256Gcm, 0U, 0U, kNever, kNever};
    REQUIRE(store.apply(sign(disable).view(), kT0 + kDay) == PolicyError::None);
    const PolicySnapshot next = store.snapshot();

    CHECK(running.generation() == 1U);
    CHECK(running.status(AlgorithmId::Chacha20Poly1305, kT0 + kDay) == AlgorithmStatus::Accepted);
    CHECK(running.status(AlgorithmId::Aes256Gcm, kT0 + kDay) == AlgorithmStatus::Refused);
    CHECK(next.generation() == 2U);
    CHECK(next.status(AlgorithmId::Chacha20Poly1305, kT0 + kDay) == AlgorithmStatus::Refused);
    CHECK(next.status(AlgorithmId::Aes256Gcm, kT0 + kDay) == AlgorithmStatus::Preferred);
}

TEST_CASE("Policy: protocol minimum versions", "[crypto][net-std]")
{
    const TestVerifier verifier;
    PolicyStore        store(config(verifier));
    REQUIRE(store.apply(sign(base(1U)).view(), kT0) == PolicyError::None);
    const PolicySnapshot snapshot = store.snapshot();
    CHECK_FALSE(snapshot.allows_version(0x0002U, 0U, 9U));
    CHECK_FALSE(snapshot.allows_version(0x0002U, 1U, 1U));
    CHECK(snapshot.allows_version(0x0002U, 1U, 2U));
    CHECK(snapshot.allows_version(0x0002U, 1U, 3U));
    CHECK(snapshot.allows_version(0x0002U, 2U, 0U));
    CHECK(snapshot.allows_version(0x0003U, 0U, 0U)); // no minimum for the project protocol
}

TEST_CASE("Policy: snapshots taken while generations are applied are whole generations", "[crypto][net-std]")
{
    const TestVerifier verifier;
    PolicyStore        store(config(verifier));
    constexpr u32      generation_count = 64U;
    static Signed      generations[generation_count];
    for (u32 i = 0U; i < generation_count; ++i)
    {
        PolicyContent content     = base(i + 1U, kT0 + i);
        content.minimums[0].minor = static_cast<u16>(i);
        generations[i]            = sign(content);
    }
    std::atomic<bool> done{false};
    u32               refused = 0U; // written by the applier only, read after join
    std::thread       applier(
        [&store, &done, &refused]
        {
            for (u32 i = 0U; i < generation_count; ++i)
            {
                refused += store.apply(generations[i].view(), kT0 + generation_count) == PolicyError::None ? 0U : 1U;
            }
            done.store(true, std::memory_order_release);
        });
    u64  last    = 0U;
    bool ordered = true;
    bool whole   = true;
    while (!done.load(std::memory_order_acquire))
    {
        const PolicySnapshot snapshot = store.snapshot();
        ordered = ordered && snapshot.generation() >= last;
        if (snapshot.generation() != 0U)
        {
            whole = whole && snapshot.content().issued == kT0 + snapshot.generation() - 1U &&
                    snapshot.content().minimums[0].minor == snapshot.generation() - 1U;
        }
        last = snapshot.generation();
    }
    applier.join();
    CHECK(refused == 0U);
    CHECK(ordered);
    CHECK(whole);
    CHECK(store.floor_generation() == generation_count);
}

TEST_CASE("Policy: error names are distinct and stable", "[crypto][net-std]")
{
    const char* const names[] = {
        "none",
        "bad-config",
        "buffer-too-small",
        "too-large",
        "truncated",
        "trailing-bytes",
        "bad-magic",
        "unsupported-format",
        "bad-generation",
        "bad-dates",
        "bad-rule",
        "duplicate-rule",
        "duplicate-minimum",
        "unsigned",
        "bad-signature-entry",
        "duplicate-signature",
        "missing-signature",
        "unsupported-signature",
        "bad-signature",
        "rollback",
        "conflict",
        "already-current",
        "not-yet-issued",
        "issued-before-current",
        "expired",
        "absent",
    };
    constexpr usize count = sizeof(names) / sizeof(names[0]);
    for (usize i = 0U; i < count; ++i)
    {
        const auto name = policy_error_name(static_cast<PolicyError>(i));
        CHECK(name == names[i]);
    }
    CHECK(policy_error_name(static_cast<PolicyError>(count)) == "unknown");
}

TEST_CASE("Policy: a raised floor refuses older generations and keeps the current one", "[crypto][net-std]")
{
    const TestVerifier verifier;
    PolicyStore        store(config(verifier));
    REQUIRE(store.apply(sign(base(5U)).view(), kT0) == PolicyError::None);
    store.raise_floor(8U);
    CHECK(store.floor_generation() == 8U);
    CHECK(store.snapshot().generation() == 5U);
    CHECK(store.apply(sign(base(7U)).view(), kT0) == PolicyError::Rollback);
    store.raise_floor(3U); // never lowered
    CHECK(store.floor_generation() == 8U);
    CHECK(store.apply(sign(base(9U)).view(), kT0) == PolicyError::None);
    CHECK(store.floor_generation() == 9U);
}

namespace
{
// The newest save of an installation's state: its durable-file generation (how many saves were made).
u64 saves_of(const TempDirectory& dir)
{
    Array<u8> payload;
    return fs::load_generation(dir.path() / "policy-state", 1U, payload).generation;
}

void flip_middle_byte(const fs::Path& path)
{
    Array<u8> bytes;
    REQUIRE(fs::read_file_binary(path, bytes));
    bytes[bytes.size() / 2U] ^= 0x10U;
    REQUIRE(fs::write_file_binary(path, ConstSpan<u8>(bytes.data(), bytes.size())));
}
} // namespace

TEST_CASE("Policy: an installation saves its generation and floor and starts from them", "[crypto][net-std]")
{
    const TestVerifier  verifier;
    const TempDirectory dir("policy-state");
    const Signed        five = sign(base(5U, kT0));
    const Signed        six  = sign(base(6U, kT0 + kDay));
    {
        // A fresh installation adopts the bootstrap and saves it.
        PolicyInstallation installation(config(verifier));
        const PolicyOpen   opened = installation.open(dir.dir(), five.view(), kT0);
        CHECK(opened.read == PolicyStateError::None);
        CHECK_FALSE(opened.recovered);
        CHECK(opened.start.persisted == PolicyError::Absent);
        CHECK(opened.start.bootstrap == PolicyError::None);
        CHECK(opened.saved == PolicyStateError::None);
        CHECK(installation.store().snapshot().generation() == 5U);
        CHECK(saves_of(dir) == 1U);

        // A generation received at run time is adopted and saved; a refused one saves nothing.
        PolicyStateError saved = PolicyStateError::NotOpen;
        CHECK(installation.apply(six.view(), kT0 + kDay, saved) == PolicyError::None);
        CHECK(saved == PolicyStateError::None);
        CHECK(installation.apply(five.view(), kT0 + kDay, saved) == PolicyError::Rollback);
        CHECK(saved == PolicyStateError::None);
        CHECK(saves_of(dir) == 2U);
    }
    {
        // A restart: the saved generation is current, the older bootstrap a harmless rollback, nothing to save.
        PolicyInstallation installation(config(verifier));
        const PolicyOpen   opened = installation.open(dir.dir(), five.view(), kT0 + kDay);
        CHECK(opened.start.persisted == PolicyError::None);
        CHECK(opened.start.bootstrap == PolicyError::Rollback);
        CHECK(installation.store().snapshot().generation() == 6U);
        CHECK(installation.store().floor_generation() == 6U);
        CHECK(saves_of(dir) == 2U);
    }
    {
        // A newer build's bootstrap supersedes the saved generation and is saved.
        PolicyInstallation installation(config(verifier));
        const PolicyOpen   opened = installation.open(dir.dir(), sign(base(7U, kT0 + kDay)).view(), kT0 + kDay);
        CHECK(opened.start.persisted == PolicyError::None);
        CHECK(opened.start.bootstrap == PolicyError::None);
        CHECK(opened.saved == PolicyStateError::None);
        CHECK(installation.store().snapshot().generation() == 7U);
        CHECK(saves_of(dir) == 3U);
    }
}

TEST_CASE("Policy: the processes of one installation share its floor", "[crypto][net-std]")
{
    const TestVerifier  verifier;
    const TempDirectory dir("policy-shared");
    const Signed        five = sign(base(5U, kT0));
    PolicyInstallation  editor(config(verifier));
    PolicyInstallation  game(config(verifier));
    REQUIRE(editor.open(dir.dir(), five.view(), kT0).start.bootstrap == PolicyError::None);
    const PolicyOpen joined = game.open(dir.dir(), five.view(), kT0);
    CHECK(joined.start.persisted == PolicyError::None);
    CHECK(joined.start.bootstrap == PolicyError::AlreadyCurrent);

    // The editor adopts generation 7; the game, offered 6 later, first catches up with 7 and refuses 6.
    PolicyStateError saved = PolicyStateError::NotOpen;
    REQUIRE(editor.apply(sign(base(7U, kT0 + kDay)).view(), kT0 + kDay, saved) == PolicyError::None);
    CHECK(game.apply(sign(base(6U, kT0 + kDay)).view(), kT0 + kDay, saved) == PolicyError::Rollback);
    CHECK(game.store().snapshot().generation() == 7U);
    CHECK(game.store().floor_generation() == 7U);
}

TEST_CASE("Policy: an installation's saved floor outlives an expired generation", "[crypto][net-std]")
{
    // Generation 5 expires past its grace; a build whose bootstrap is the older generation 4 cannot roll the
    // installation back to it, so the store stays empty and refuses everything (fail closed, S23).
    const TestVerifier  verifier;
    const TempDirectory dir("policy-expired");
    {
        PolicyInstallation installation(config(verifier));
        REQUIRE(installation.open(dir.dir(), sign(base(5U, kT0)).view(), kT0).start.bootstrap == PolicyError::None);
    }
    const u64          late = kT0 + 30U * kDay + 7U * kDay + 1U;
    PolicyInstallation installation(config(verifier));
    const PolicyOpen   opened = installation.open(dir.dir(), sign(base(4U, late)).view(), late);
    CHECK(opened.start.persisted == PolicyError::Expired);
    CHECK(opened.start.bootstrap == PolicyError::Rollback);
    CHECK(opened.saved == PolicyStateError::None);
    CHECK(installation.store().floor_generation() == 5U);
    CHECK(installation.store().snapshot().status(AlgorithmId::X25519Mlkem768, late) == AlgorithmStatus::Refused);
    CHECK(saves_of(dir) == 1U);
}

TEST_CASE("Policy: a damaged saved state falls back to the older save, then to the bootstrap", "[crypto][net-std]")
{
    const TestVerifier  verifier;
    const Signed        five = sign(base(5U, kT0));
    const ConstSpan<u8> none;
    {
        // The newest save damaged: the older one is read and saved again; the newer generation it lost arrives
        // again through the update channel.
        const TempDirectory dir("policy-recover");
        {
            PolicyInstallation installation(config(verifier));
            REQUIRE(installation.open(dir.dir(), five.view(), kT0).saved == PolicyStateError::None);
            PolicyStateError saved = PolicyStateError::NotOpen;
            REQUIRE(installation.apply(sign(base(6U, kT0)).view(), kT0, saved) == PolicyError::None);
        }
        flip_middle_byte(dir.path() / "policy-state");
        PolicyInstallation installation(config(verifier));
        const PolicyOpen   opened = installation.open(dir.dir(), none, kT0);
        CHECK(opened.read == PolicyStateError::None);
        CHECK(opened.recovered);
        CHECK(opened.start.persisted == PolicyError::None);
        CHECK(opened.start.bootstrap == PolicyError::Absent);
        CHECK(opened.saved == PolicyStateError::None);
        CHECK(installation.store().snapshot().generation() == 5U);
        CHECK(installation.store().floor_generation() == 5U);
    }
    {
        // Every save damaged: read as none, and the bootstrap is adopted and saved over it.
        const TempDirectory dir("policy-corrupt");
        {
            PolicyInstallation installation(config(verifier));
            REQUIRE(installation.open(dir.dir(), five.view(), kT0).saved == PolicyStateError::None);
        }
        flip_middle_byte(dir.path() / "policy-state");
        PolicyInstallation installation(config(verifier));
        const PolicyOpen   opened = installation.open(dir.dir(), five.view(), kT0);
        CHECK(opened.read == PolicyStateError::Damaged);
        CHECK(opened.start.persisted == PolicyError::Absent);
        CHECK(opened.start.bootstrap == PolicyError::None);
        CHECK(opened.saved == PolicyStateError::None);
        PolicyInstallation again(config(verifier));
        CHECK(again.open(dir.dir(), none, kT0).read == PolicyStateError::None);
        CHECK(again.store().snapshot().generation() == 5U);
    }
    // A save whose file checks pass but whose payload this build does not read: another format, a generation shorter
    // than its length, one longer than kMaxPolicyBytes, trailing bytes.
    const u8 payloads[][14] = {
        {2U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 5U, 0U, 0U, 0U, 0U, 0U},
        {1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 5U, 0U, 0U, 0U, 9U, 0U},
        {1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 5U, 0U, 0U, 0x80U, 1U, 0U},
        {1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 5U, 0U, 0U, 0U, 0U, 0U},
    };
    for (const auto& payload : payloads)
    {
        const TempDirectory dir("policy-payload");
        REQUIRE(fs::create_private_directory(dir.path()));
        REQUIRE(fs::save_generation(dir.path() / "policy-state", ConstSpan<u8>(payload), 1U).ok);
        PolicyInstallation installation(config(verifier));
        const PolicyOpen   opened = installation.open(dir.dir(), five.view(), kT0);
        CHECK(opened.read == PolicyStateError::Damaged);
        CHECK(opened.start.bootstrap == PolicyError::None);
        CHECK(installation.store().floor_generation() == 5U);
    }
    {
        // The shortest well-formed payload: floor 5, no generation.
        const u8            floor_only[] = {1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 5U, 0U, 0U, 0U, 0U};
        const TempDirectory dir("policy-floor");
        REQUIRE(fs::create_private_directory(dir.path()));
        REQUIRE(fs::save_generation(dir.path() / "policy-state", ConstSpan<u8>(floor_only), 1U).ok);
        PolicyInstallation installation(config(verifier));
        const PolicyOpen   opened = installation.open(dir.dir(), sign(base(4U)).view(), kT0);
        CHECK(opened.read == PolicyStateError::None);
        CHECK(opened.start.bootstrap == PolicyError::Rollback);
        CHECK(installation.store().floor_generation() == 5U);
    }
}

TEST_CASE("Policy: an installation that cannot read or save its state", "[crypto][net-std]")
{
    const TestVerifier verifier;
    const Signed       five  = sign(base(5U, kT0));
    PolicyStateError   saved = PolicyStateError::None;
    {
        // Not opened, or no directory: nothing is adopted and every algorithm is refused.
        PolicyInstallation installation(config(verifier));
        CHECK(installation.apply(five.view(), kT0, saved) == PolicyError::BadConfig);
        CHECK(saved == PolicyStateError::NotOpen);
        CHECK(installation.open("", five.view(), kT0).read == PolicyStateError::Unavailable);
        const TempDirectory file("policy-file");
        REQUIRE(fs::write_file_binary(file.path(), five.view()));
        CHECK(installation.open(file.dir(), five.view(), kT0).read == PolicyStateError::Unavailable);
        CHECK(installation.store().snapshot().freshness(kT0) == Freshness::None);
        CHECK(installation.apply(five.view(), kT0, saved) == PolicyError::BadConfig);
    }
    {
        // A state file that cannot be replaced (a directory in its place): the store adopts the generation for this
        // run, and the failed save is reported.
        const TempDirectory dir("policy-unsaved");
        REQUIRE(fs::create_directories(dir.path() / "policy-state"));
        PolicyInstallation installation(config(verifier));
        const PolicyOpen   opened = installation.open(dir.dir(), five.view(), kT0);
        CHECK(opened.start.bootstrap == PolicyError::None);
        CHECK(opened.saved == PolicyStateError::Unsaved);
        CHECK(installation.apply(sign(base(6U)).view(), kT0, saved) == PolicyError::None);
        CHECK(saved == PolicyStateError::Unsaved);
        CHECK(installation.store().snapshot().generation() == 6U);
    }
    CHECK(policy_state_error_name(PolicyStateError::None) == "none");
    CHECK(policy_state_error_name(PolicyStateError::NotOpen) == "not-open");
    CHECK(policy_state_error_name(PolicyStateError::Unavailable) == "unavailable");
    CHECK(policy_state_error_name(PolicyStateError::Damaged) == "damaged");
    CHECK(policy_state_error_name(PolicyStateError::Unsaved) == "unsaved");
    CHECK(policy_state_error_name(static_cast<PolicyStateError>(5)) == "unknown");
}

namespace
{
// Starts a store from `bootstrap` alone on its day of issue, under the provider's verifier.
void check_bootstrap(const PolicyBootstrap& bootstrap)
{
    for (const TrustKey& key : bootstrap.trust)
    {
        CHECK(key.id == key_id_of(key.public_key));
    }
    const ProviderVerifier verifier;
    const PolicyConfig     trusted{bootstrap.trust, &verifier, provider_algorithms(), 0U};
    REQUIRE(validate_config(trusted) == PolicyError::None);
    PolicyMessage message;
    REQUIRE(decode_policy(bootstrap.generation, message) == PolicyError::None);
    const u64   issued = message.content.issued;
    PolicyStore store(trusted);
    CHECK(store.start({}, bootstrap.generation, issued).bootstrap == PolicyError::None);
    CHECK(store.snapshot().generation() == bootstrap.number);
    CHECK(store.snapshot().status(AlgorithmId::X25519Mlkem768, issued) == AlgorithmStatus::Preferred);

    // One changed byte of a signature is refused.
    Array<u8> forged;
    forged.resize(bootstrap.generation.size());
    std::memcpy(forged.data(), bootstrap.generation.data(), forged.size());
    forged[forged.size() - 1U] ^= 0x01U;
    PolicyStore other(trusted);
    CHECK(other.apply(ConstSpan<u8>(forged.data(), forged.size()), issued) == PolicyError::BadSignature);
}
} // namespace

TEST_CASE("Policy: a generated bootstrap verifies under its own trust set", "[crypto][net-std]")
{
    // The populated shape: a test trust set (ML-DSA-65 and ECDSA P-256) and generation 1 signed by it with ceridc.
    REQUIRE(crd::crypto::test::bootstrap::kBootstrap.trust.size() == 2U);
    check_bootstrap(crd::crypto::test::bootstrap::kBootstrap);

    // The build's: empty until the key holders' ceremony commits it, and then held to the same checks. Without a
    // trust set no generation is accepted, so an installation refuses every algorithm.
    const PolicyBootstrap built = policy_bootstrap();
    if (built.trust.empty())
    {
        CHECK(built.generation.empty());
        const ProviderVerifier verifier;
        const PolicyConfig     untrusted{built.trust, &verifier, provider_algorithms(), 0U};
        CHECK(validate_config(untrusted) == PolicyError::BadConfig);
        PolicyStore store(untrusted);
        CHECK(store.apply(crd::crypto::test::bootstrap::kBootstrap.generation, kT0) == PolicyError::BadConfig);
    }
    else if (!built.generation.empty())
    {
        check_bootstrap(built);
    }
}
