// crd-crypto capabilities (NET.2; docs/design/network-foundation.md#capability-tokens): tokens issued by a hybrid
// issuer set, attenuated by their holders, presented over real TLS sessions and verified against revocation
// generations and a policy snapshot, every refusal in the contract's order, with the provider's real signatures.
#include "policy_test_signer.hpp"

#include <crd/containers/array.hpp>
#include <crd/crypto/capability.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::StringView;
using namespace crd::crypto;

using Bytes = crd::containers::Array<u8>;

constexpr u64 kHour = 3600U;
constexpr u64 kDay  = 86400U;
constexpr u64 kT0   = 1800000000U; // 2027-01-15

constexpr AlgorithmId kSignatures[] = {
    AlgorithmId::MlDsa44, AlgorithmId::MlDsa65, AlgorithmId::MlDsa87,
    AlgorithmId::Ed25519, AlgorithmId::EcdsaSecp256r1Sha256, AlgorithmId::EcdsaSecp384r1Sha384,
};
constexpr AlgorithmId kGroups[] = {AlgorithmId::X25519Mlkem768, AlgorithmId::X25519};
constexpr AlgorithmId kAeads[]  = {AlgorithmId::Aes128Gcm, AlgorithmId::Chacha20Poly1305};

// A signing key from a fixed seed (an ECDSA scalar of 1..n-1: the bytes salt + 1, salt + 2, ...).
void make_key(SigningKey& key, AlgorithmId algorithm, u8 salt)
{
    u8 seed[64];
    for (usize i = 0U; i < sizeof(seed); ++i)
    {
        seed[i] = static_cast<u8>(i + 1U + salt);
    }
    REQUIRE(key.from_seed(algorithm, ConstSpan<u8>(seed, signature_sizes(algorithm).seed)) == CryptoError::None);
}

KeyId id_of(const SigningKey& key)
{
    u8 public_key[kMaxHolderKeyBytes];
    REQUIRE(key.public_key(Span<u8>(public_key, signature_sizes(key.algorithm()).public_key)) == CryptoError::None);
    return key_id_of(ConstSpan<u8>(public_key, signature_sizes(key.algorithm()).public_key));
}

CapabilityId id(u8 tag)
{
    CapabilityId out;
    for (usize i = 0U; i < kCapabilityIdBytes; ++i)
    {
        out.bytes[i] = static_cast<u8>(tag + i);
    }
    return out;
}

// A policy snapshot at kT0 accepting every signature algorithm but `refused`, through a real PolicyStore whose
// generation the deterministic test signer signs (the policy's own tests prove that path; here it only feeds a
// snapshot).
PolicySnapshot policy(AlgorithmId refused = AlgorithmId{})
{
    PolicyContent content;
    content.generation = 1U;
    content.issued     = kT0 - kDay;
    content.expires    = kT0 + 100U * kDay;
    for (const AlgorithmId algorithm : kSignatures)
    {
        if (algorithm != refused)
        {
            content.rules[content.rule_count++] = {algorithm, 0U, 0U, 0U, kNever};
        }
    }
    u8    body[2048];
    usize body_size = 0U;
    REQUIRE(write_policy_body(content, body, body_size) == PolicyError::None);
    u8           tags[2][test::kTestSignatureBytes];
    KeySignature signatures[2];
    for (usize i = 0U; i < 2U; ++i)
    {
        const TrustKey& key = test::kTestTrust[i];
        test::test_sign(key.algorithm, key.public_key, ConstSpan<u8>(body, body_size), tags[i]);
        signatures[i] = {key.id, key.algorithm, ConstSpan<u8>(tags[i])};
    }
    u8    generation[4096];
    usize size = 0U;
    REQUIRE(write_policy(ConstSpan<u8>(body, body_size), signatures, generation, size) == PolicyError::None);
    static const test::TestVerifier kVerifier;
    PolicyConfig                    config;
    config.trust     = test::kTestTrust;
    config.verifier  = &kVerifier;
    config.supported = provider_algorithms();
    PolicyStore store(config);
    REQUIRE(store.apply(ConstSpan<u8>(generation, size), kT0) == PolicyError::None);
    return store.snapshot();
}

// One issuer key pair (post-quantum and traditional) with its issuing window.
struct Pair
{
    Pair()                       = default;
    Pair(const Pair&)            = delete;
    Pair& operator=(const Pair&) = delete;
    Pair(Pair&&)                 = delete;
    Pair& operator=(Pair&&)      = delete;

    void init(AlgorithmId post_quantum, AlgorithmId traditional, u8 salt, u64 from = 0U, u64 until = kNever)
    {
        make_key(keys[0], post_quantum, salt);
        make_key(keys[1], traditional, static_cast<u8>(salt + 1U));
        for (usize i = 0U; i < 2U; ++i)
        {
            const usize size = signature_sizes(keys[i].algorithm()).public_key;
            REQUIRE(keys[i].public_key(Span<u8>(publics[i], size)) == CryptoError::None);
            issuers[i] = {{key_id_of(ConstSpan<u8>(publics[i], size)), keys[i].algorithm(),
                           ConstSpan<u8>(publics[i], size)},
                          from,
                          until};
            signers[i] = &keys[i];
        }
    }

    SigningKey    keys[2];
    u8            publics[2][kMaxHolderKeyBytes] = {};
    IssuerKey     issuers[2];
    const Signer* signers[2] = {};
};

struct Token
{
    Bytes bytes;

    [[nodiscard]] ConstSpan<u8> view() const { return ConstSpan<u8>(bytes.data(), bytes.size()); }
};

Token issue(const CapabilityRoot& root, ConstSpan<const Signer*> signers)
{
    Token out;
    out.bytes.resize(kMaxCapabilityBytes);
    usize written = 0U;
    REQUIRE(issue_capability(root, signers, Span<u8>(out.bytes.data(), out.bytes.size()), written) ==
            CapabilityError::None);
    out.bytes.resize(written);
    return out;
}

Token attenuate(const Token& token, const CapabilityAttenuation& block, const Signer& holder)
{
    Token out;
    out.bytes.resize(kMaxCapabilityBytes);
    usize written = 0U;
    REQUIRE(attenuate_capability(token.view(), block, holder, Span<u8>(out.bytes.data(), out.bytes.size()),
                                 written) == CapabilityError::None);
    out.bytes.resize(written);
    return out;
}

Bytes prove(const Token& token, ConstSpan<u8> binding, const Signer& holder)
{
    Bytes out;
    out.resize(kMaxCapabilityProofBytes);
    usize written = 0U;
    REQUIRE(prove_capability(token.view(), binding, holder, Span<u8>(out.data(), out.size()), written) ==
            CapabilityError::None);
    out.resize(written);
    return out;
}

Grant game_grant()
{
    Grant grant;
    grant.kind                         = ScopeKind::World;
    grant.scope                        = id(0x40U);
    grant.ranges[0]                    = {100U, 199U};
    grant.ranges[1]                    = {1000U, 1999U};
    grant.range_count                  = 2U;
    grant.operations                   = 0x0FU;
    grant.budget.operations_per_second = 120U;
    grant.budget.bytes_per_second      = 65536U;
    grant.budget.objects               = 64U;
    return grant;
}

// The audience of the tests: a game world server with one issuer pair, a player's device key as holder.
struct World
{
    explicit World(AlgorithmId post_quantum = AlgorithmId::MlDsa65, AlgorithmId traditional = AlgorithmId::Ed25519,
          AlgorithmId holder_algorithm = AlgorithmId::Ed25519)
    {
        pair.init(post_quantum, traditional, 0x10U);
        make_key(holder, holder_algorithm, 0x30U);
        make_key(delegate, AlgorithmId::Ed25519, 0x50U);
        config.issuers  = ConstSpan<IssuerKey>(pair.issuers);
        config.verifier = &verifier;
        config.kind     = ScopeKind::World;
        config.audience = id(0x20U);
        authority       = std::make_unique<CapabilityAuthority>(config);
        root.token      = id(0x01U);
        root.issued     = kT0;
        root.expires    = kT0 + kHour;
        root.policy_generation = 1U;
        root.audience          = config.audience;
        root.subject           = {PrincipalKind::User, id_of(holder)};
        root.holder            = {holder.algorithm(), id_of(holder), CapabilityBinding::Proof};
        root.grant             = game_grant();
        for (u8& byte : binding)
        {
            byte = 0xB1U;
        }
    }

    // A fresh revocation generation applied at kT0 (none listed unless `list` says so).
    [[nodiscard]] CapabilityError revoke(RevocationList list = {}, u64 now = kT0)
    {
        if (list.generation == 0U)
        {
            list.generation = authority->revocation_floor() + 1U;
            list.issued     = kT0 - kHour;
            list.expires    = kT0 + kDay;
        }
        Bytes bytes;
        bytes.resize(kMaxRevocationBytes);
        usize written = 0U;
        REQUIRE(issue_revocations(list, pair.signers, Span<u8>(bytes.data(), bytes.size()), written) ==
                CapabilityError::None);
        return authority->apply_revocations(ConstSpan<u8>(bytes.data(), written), snapshot, now);
    }

    [[nodiscard]] Token token() const { return issue(root, pair.signers); }

    [[nodiscard]] CapabilityError verify(const Token& token, ConstSpan<u8> proof, Capability& out,
                                         u64 now = kT0 + 60U) const
    {
        CapabilityPresentation presentation;
        presentation.binding = binding;
        presentation.proof   = proof;
        return authority->verify(token.view(), presentation, snapshot, now, out);
    }

    [[nodiscard]] CapabilityError verify(const Token& token, u64 now = kT0 + 60U) const
    {
        Capability  out;
        const Bytes proof = prove(token, binding, current_holder(token));
        return verify(token, ConstSpan<u8>(proof.data(), proof.size()), out, now);
    }

    // The signer of the token's last holder among the test's keys.
    [[nodiscard]] const Signer& current_holder(const Token& token) const
    {
        CapabilityToken decoded;
        REQUIRE(decode_capability(token.view(), decoded) == CapabilityError::None);
        return decoded.holder().key == id_of(delegate) ? static_cast<const Signer&>(delegate)
                                                        : static_cast<const Signer&>(holder);
    }

    Pair                                 pair;
    SigningKey                           holder;
    SigningKey                           delegate;
    ProviderVerifier                     verifier;
    CapabilityConfig                     config;
    std::unique_ptr<CapabilityAuthority> authority;
    CapabilityRoot                       root;
    PolicySnapshot                       snapshot = policy();
    u8                                   binding[kCapabilityBindingBytes] = {};
};

CapabilityAttenuation narrower(const World& world, u64 expires = kT0 + kHour / 2U)
{
    CapabilityAttenuation block;
    block.expires     = expires;
    block.holder      = {world.delegate.algorithm(), id_of(world.delegate), CapabilityBinding::Proof};
    block.grant       = world.root.grant;
    block.grant.ranges[0]   = {120U, 129U};
    block.grant.range_count = 1U;
    block.grant.operations  = 0x03U;
    block.grant.budget.objects = 8U;
    return block;
}
} // namespace

TEST_CASE("Capabilities: an issued token verifies at its audience and grants exactly its root",
          "[crypto][capability][net-2]")
{
    World world;
    REQUIRE(world.revoke() == CapabilityError::None);
    const Token token = world.token();
    // The root (191 bytes with two ranges and its fixed fields), the list count, ML-DSA-65's and Ed25519's entries
    // (a 36-byte head each, 3,309 and 64 bytes of signature): the bytes a hybrid issuer pays per token.
    CHECK(token.bytes.size() == 133U + 42U + 2U * 16U + 1U + 36U + 3309U + 36U + 64U);

    CapabilityToken decoded;
    REQUIRE(decode_capability(token.view(), decoded) == CapabilityError::None);
    CHECK(decoded.format == kCapabilityFormat);
    CHECK(decoded.root.token == world.root.token);
    CHECK(decoded.root.subject == world.root.subject);
    CHECK(decoded.root_bytes.size() == 133U + 42U + 32U);
    CHECK(decoded.signature_count == 2U);
    CHECK(decoded.block_count == 0U);

    Capability  granted;
    const Bytes proof = prove(token, world.binding, world.holder);
    CHECK(proof.size() == 4U + 32U + 64U); // an Ed25519 holder's key and signature
    REQUIRE(world.verify(token, ConstSpan<u8>(proof.data(), proof.size()), granted) == CapabilityError::None);
    CHECK(granted.token == world.root.token);
    CHECK(granted.audience == world.config.audience);
    CHECK(granted.subject == world.root.subject);
    CHECK(granted.holder.key == id_of(world.holder));
    CHECK(granted.issued == kT0);
    CHECK(granted.expires == kT0 + kHour);
    CHECK(granted.policy_generation == 1U);
    CHECK(granted.depth == 0U);
    CHECK(grant_within(granted.grant, world.root.grant));
    CHECK(grant_within(world.root.grant, granted.grant));

    // What it covers: every bit of the operations asked within the grant's, on an object inside a range.
    CHECK(granted.covers(0x01U, 100U));
    CHECK(granted.covers(0x0FU, 1999U));
    CHECK_FALSE(granted.covers(0x10U, 100U));
    CHECK_FALSE(granted.covers(0x11U, 100U));
    CHECK_FALSE(granted.covers(0U, 100U));
    CHECK_FALSE(granted.covers(0x01U, 99U));
    CHECK_FALSE(granted.covers(0x01U, 200U));
    CHECK_FALSE(granted.covers(0x01U, 2000U));
    granted.grant.range_count = 0U; // no ranges: every object of the scope
    CHECK(granted.covers(0x01U, ~u64{0U}));
}

TEST_CASE("Capabilities: every hybrid issuer pair and every holder algorithm verifies", "[crypto][capability][net-2]")
{
    const AlgorithmId post_quantum[] = {AlgorithmId::MlDsa44, AlgorithmId::MlDsa65, AlgorithmId::MlDsa87};
    const AlgorithmId traditional[]  = {AlgorithmId::Ed25519, AlgorithmId::EcdsaSecp256r1Sha256,
                                        AlgorithmId::EcdsaSecp384r1Sha384};
    for (usize i = 0U; i < 3U; ++i)
    {
        for (usize j = 0U; j < 3U; ++j)
        {
            World world(post_quantum[i], traditional[j], kSignatures[(i * 3U + j) % 6U]);
            REQUIRE(world.revoke() == CapabilityError::None);
            CHECK(world.verify(world.token()) == CapabilityError::None);
        }
    }
}

TEST_CASE("Capabilities: decoding is exact and bounded, and no other signed artefact decodes as a token",
          "[crypto][capability][net-2]")
{
    World           world;
    const Token     token = world.token();
    CapabilityToken decoded;
    // Every proper prefix: the root ends inside a field (Truncated), and between the root and its list the count is
    // missing; the list and nothing after it is the whole token.
    for (usize size = 0U; size < token.bytes.size(); ++size)
    {
        CHECK(decode_capability(ConstSpan<u8>(token.bytes.data(), size), decoded) == CapabilityError::Truncated);
    }
    Bytes edited = token.bytes;
    edited[0] ^= 0x01U;
    CHECK(decode_capability(ConstSpan<u8>(edited.data(), edited.size()), decoded) == CapabilityError::BadMagic);
    edited    = token.bytes;
    edited[8] = 2U;
    CHECK(decode_capability(ConstSpan<u8>(edited.data(), edited.size()), decoded) ==
          CapabilityError::UnsupportedFormat);
    edited[8] = 0U;
    CHECK(decode_capability(ConstSpan<u8>(edited.data(), edited.size()), decoded) ==
          CapabilityError::UnsupportedFormat);
    // A range count above the bound (the grant's count byte follows its kind and scope).
    edited       = token.bytes;
    const usize grant_at = 133U;
    edited[grant_at + 17U] = static_cast<u8>(kMaxCapabilityRanges + 1U);
    CHECK(decode_capability(ConstSpan<u8>(edited.data(), edited.size()), decoded) == CapabilityError::TooLarge);
    // Bytes after the list are a block: a short one is Truncated.
    edited = token.bytes;
    edited.push_back(0U);
    CHECK(decode_capability(ConstSpan<u8>(edited.data(), edited.size()), decoded) == CapabilityError::Truncated);
    Bytes huge;
    huge.resize(kMaxCapabilityBytes + 1U);
    std::memcpy(huge.data(), token.bytes.data(), token.bytes.size());
    CHECK(decode_capability(ConstSpan<u8>(huge.data(), huge.size()), decoded) == CapabilityError::TooLarge);

    // Policy generations, revocation generations and tokens each refuse the others by their magic.
    Bytes revocations;
    revocations.resize(kMaxRevocationBytes);
    usize          written = 0U;
    RevocationList list;
    list.generation = 1U;
    list.issued     = kT0;
    list.expires    = kT0 + kDay;
    REQUIRE(issue_revocations(list, world.pair.signers, Span<u8>(revocations.data(), revocations.size()), written) ==
            CapabilityError::None);
    const ConstSpan<u8> revocation_bytes(revocations.data(), written);
    RevocationMessage   message;
    REQUIRE(decode_revocations(revocation_bytes, message) == CapabilityError::None);
    CHECK(decode_capability(revocation_bytes, decoded) == CapabilityError::BadMagic);
    CHECK(decode_revocations(token.view(), message) == CapabilityError::BadMagic);
    PolicyMessage policy_message;
    CHECK(decode_policy(token.view(), policy_message) == PolicyError::BadMagic);
    CHECK(decode_policy(revocation_bytes, policy_message) == PolicyError::BadMagic);
    u8            body[2048];
    usize         body_size = 0U;
    PolicyContent content;
    content.generation = 1U;
    content.issued     = kT0;
    content.expires    = kT0 + kDay;
    REQUIRE(write_policy_body(content, body, body_size) == PolicyError::None);
    CHECK(decode_capability(ConstSpan<u8>(body, body_size), decoded) == CapabilityError::BadMagic);
    CHECK(decode_revocations(ConstSpan<u8>(body, body_size), message) == CapabilityError::BadMagic);
}

TEST_CASE("Capabilities: a root is refused for every malformed field before anything is signed",
          "[crypto][capability][net-2]")
{
    World world;
    Bytes out;
    out.resize(kMaxCapabilityBytes);
    usize      written = 0U;
    const auto refused = [&](const CapabilityRoot& root) {
        written                     = 1U;
        const CapabilityError error = issue_capability(root, world.pair.signers, Span<u8>(out.data(), out.size()),
                                                       written);
        CHECK((error == CapabilityError::None) == (written > 0U));
        return error;
    };
    CapabilityRoot root = world.root;
    root.token          = CapabilityId{};
    CHECK(refused(root) == CapabilityError::BadField);
    root          = world.root;
    root.audience = CapabilityId{};
    CHECK(refused(root) == CapabilityError::BadField);
    for (const u8 kind : {u8{0U}, u8{6U}})
    {
        root              = world.root;
        root.subject.kind = static_cast<PrincipalKind>(kind);
        CHECK(refused(root) == CapabilityError::BadField);
    }
    root                  = world.root;
    root.holder.algorithm = AlgorithmId::X25519; // not a signature algorithm
    CHECK(refused(root) == CapabilityError::BadField);
    for (const u8 binding : {u8{0U}, u8{3U}})
    {
        root                = world.root;
        root.holder.binding = static_cast<CapabilityBinding>(binding);
        CHECK(refused(root) == CapabilityError::BadField);
    }
    for (const u8 kind : {u8{0U}, u8{7U}})
    {
        root            = world.root;
        root.grant.kind = static_cast<ScopeKind>(kind);
        CHECK(refused(root) == CapabilityError::BadField);
    }
    const ObjectRange bad_ranges[][2] = {
        {{5U, 4U}, {10U, 20U}},       // reversed
        {{0U, 10U}, {10U, 20U}},      // overlapping
        {{0U, 9U}, {10U, 20U}},       // touching: the canonical form merges them
        {{11U, 20U}, {0U, 9U}},       // descending
        {{0U, ~u64{0U}}, {0U, 1U}},   // after the last object
    };
    for (const auto& ranges : bad_ranges)
    {
        root                   = world.root;
        root.grant.ranges[0]   = ranges[0];
        root.grant.ranges[1]   = ranges[1];
        root.grant.range_count = 2U;
        CHECK(refused(root) == CapabilityError::BadField);
    }
    root                   = world.root;
    root.grant.range_count = kMaxCapabilityRanges + 1U;
    CHECK(refused(root) == CapabilityError::TooLarge);
    root         = world.root;
    root.expires = root.issued;
    CHECK(refused(root) == CapabilityError::BadDates);
    root.expires = root.issued + kMaxCapabilityLifetime + 1U;
    CHECK(refused(root) == CapabilityError::BadDates);
    root.expires = root.issued + kMaxCapabilityLifetime;
    CHECK(refused(root) == CapabilityError::None);
    written = 1U;
    CHECK(issue_capability(world.root, world.pair.signers, Span<u8>(out.data(), 190U), written) ==
          CapabilityError::BufferTooSmall);

    // The signers: one to four distinct keys, none missing; a buffer that cannot hold a signature.
    CHECK(issue_capability(world.root, ConstSpan<const Signer*>(), Span<u8>(out.data(), out.size()), written) ==
          CapabilityError::BadConfig);
    const Signer* twice[] = {world.pair.signers[0], world.pair.signers[0]};
    CHECK(issue_capability(world.root, twice, Span<u8>(out.data(), out.size()), written) ==
          CapabilityError::BadConfig);
    const Signer* missing[] = {world.pair.signers[0], nullptr};
    CHECK(issue_capability(world.root, missing, Span<u8>(out.data(), out.size()), written) ==
          CapabilityError::BadConfig);
    const Signer* five[] = {&world.pair.keys[0], &world.pair.keys[1], &world.holder, &world.delegate,
                            &world.pair.keys[0]};
    CHECK(issue_capability(world.root, five, Span<u8>(out.data(), out.size()), written) ==
          CapabilityError::BadConfig);
    SigningKey    empty;
    const Signer* unready[] = {&empty};
    CHECK(issue_capability(world.root, unready, Span<u8>(out.data(), out.size()), written) ==
          CapabilityError::BadConfig);
    CHECK(issue_capability(world.root, world.pair.signers, Span<u8>(out.data(), 207U + 1U + 36U + 100U), written) ==
          CapabilityError::BufferTooSmall);
    CHECK(written == 0U);
}

TEST_CASE("Capabilities: holders delegate only narrower authority", "[crypto][capability][net-2]")
{
    World world;
    REQUIRE(world.revoke() == CapabilityError::None);
    const Token                 token = world.token();
    const CapabilityAttenuation block = narrower(world);
    const Token                 child = attenuate(token, block, world.holder);
    // The block: expiry, holder and a one-range grant, then the Ed25519 signer's key and signature.
    CHECK(child.bytes.size() == token.bytes.size() + 8U + 35U + 42U + 16U + 2U + 32U + 2U + 64U);

    Capability  granted;
    const Bytes proof = prove(child, world.binding, world.delegate);
    REQUIRE(world.verify(child, ConstSpan<u8>(proof.data(), proof.size()), granted) == CapabilityError::None);
    CHECK(granted.depth == 1U);
    CHECK(granted.expires == block.expires);
    CHECK(granted.holder.key == id_of(world.delegate));
    CHECK(granted.subject == world.root.subject); // the subject stays the user the issuer granted
    CHECK(granted.covers(0x02U, 125U));
    CHECK_FALSE(granted.covers(0x04U, 125U));
    CHECK_FALSE(granted.covers(0x02U, 150U));
    // The old holder can no longer present the delegated token, and the delegate cannot present the parent.
    Bytes        out;
    out.resize(kMaxCapabilityProofBytes);
    usize written = 0U;
    CHECK(prove_capability(child.view(), world.binding, world.holder, Span<u8>(out.data(), out.size()), written) ==
          CapabilityError::WrongHolder);
    CHECK(prove_capability(token.view(), world.binding, world.delegate, Span<u8>(out.data(), out.size()), written) ==
          CapabilityError::WrongHolder);

    // Each way of widening is refused before anything is signed.
    Bytes attenuated;
    attenuated.resize(kMaxCapabilityBytes);
    const auto widened = [&](const CapabilityAttenuation& wider) {
        return attenuate_capability(token.view(), wider, world.holder,
                                    Span<u8>(attenuated.data(), attenuated.size()), written);
    };
    CapabilityAttenuation wider = block;
    wider.expires               = kT0 + kHour + 1U;
    CHECK(widened(wider) == CapabilityError::Widened);
    wider                  = block;
    wider.grant.operations = 0x13U;
    CHECK(widened(wider) == CapabilityError::Widened);
    wider                                    = block;
    wider.grant.budget.operations_per_second = 121U;
    CHECK(widened(wider) == CapabilityError::Widened);
    wider                               = block;
    wider.grant.budget.bytes_per_second = 65537U;
    CHECK(widened(wider) == CapabilityError::Widened);
    wider                      = block;
    wider.grant.budget.objects = 65U;
    CHECK(widened(wider) == CapabilityError::Widened);
    wider                 = block;
    wider.grant.scope     = id(0x41U);
    CHECK(widened(wider) == CapabilityError::Widened);
    wider            = block;
    wider.grant.kind = ScopeKind::Project;
    CHECK(widened(wider) == CapabilityError::Widened);
    wider                 = block;
    wider.grant.ranges[0] = {150U, 250U}; // across the end of [100, 199]
    CHECK(widened(wider) == CapabilityError::Widened);
    wider                   = block;
    wider.grant.range_count = 0U; // every object, where the parent names ranges
    CHECK(widened(wider) == CapabilityError::Widened);
    wider = block;
    wider.grant.ranges[0] = {100U, 199U};
    wider.grant.ranges[1] = {1000U, 1999U};
    wider.grant.range_count = 2U;
    CHECK(widened(wider) == CapabilityError::None); // equal is within
    wider                = block;
    wider.holder.binding = CapabilityBinding::Peer; // tightening is allowed
    REQUIRE(widened(wider) == CapabilityError::None);
    Token peer;
    peer.bytes.resize(written);
    std::memcpy(peer.bytes.data(), attenuated.data(), written);
    wider                = block;
    wider.holder.binding = CapabilityBinding::Proof; // loosening a Peer holder is not
    CHECK(attenuate_capability(peer.view(), wider, world.delegate, Span<u8>(out.data(), out.size()), written) ==
          CapabilityError::Widened);
    // A stranger's key, a field outside its set, an overlapping or short buffer.
    CHECK(attenuate_capability(token.view(), block, world.delegate, Span<u8>(attenuated.data(), attenuated.size()),
                               written) == CapabilityError::WrongHolder);
    wider                  = block;
    wider.holder.algorithm = AlgorithmId::Aes128Gcm;
    CHECK(widened(wider) == CapabilityError::BadField);
    Bytes in_place = token.bytes;
    in_place.resize(kMaxCapabilityBytes);
    CHECK(attenuate_capability(ConstSpan<u8>(in_place.data(), token.bytes.size()), block, world.holder,
                               Span<u8>(in_place.data(), in_place.size()), written) == CapabilityError::BadConfig);
    CHECK(attenuate_capability(token.view(), block, world.holder, Span<u8>(attenuated.data(), token.bytes.size()),
                               written) == CapabilityError::BufferTooSmall);
}

TEST_CASE("Capabilities: a chain holds at most its bound, and each block's signer and signature are checked",
          "[crypto][capability][net-2]")
{
    World world;
    REQUIRE(world.revoke() == CapabilityError::None);
    // Four blocks, the holder handing the token to itself with less each time: the bound.
    Token                 token = world.token();
    CapabilityAttenuation block = narrower(world);
    block.holder                = world.root.holder;
    for (u32 depth = 0U; depth < kMaxAttenuations; ++depth)
    {
        block.expires -= 60U;
        token = attenuate(token, block, world.holder);
        CHECK(world.verify(token) == CapabilityError::None);
    }
    Bytes out;
    out.resize(kMaxCapabilityBytes);
    usize written = 0U;
    CHECK(attenuate_capability(token.view(), block, world.holder, Span<u8>(out.data(), out.size()), written) ==
          CapabilityError::TooLarge);
    // A fifth block written by hand is refused by the decoder.
    const usize block_bytes = (token.bytes.size() - world.token().bytes.size()) / kMaxAttenuations;
    Bytes       five        = token.bytes;
    for (usize i = 0U; i < block_bytes; ++i)
    {
        five.push_back(token.bytes[token.bytes.size() - block_bytes + i]);
    }
    CapabilityToken decoded;
    CHECK(decode_capability(ConstSpan<u8>(five.data(), five.size()), decoded) == CapabilityError::TooLarge);

    // One delegation: a flipped signature byte, a block whose signer is not the previous holder and a block widened
    // after it was signed (judged before any signature).
    const Token child = attenuate(world.token(), narrower(world), world.holder);
    Token       forged = child;
    forged.bytes[forged.bytes.size() - 1U] ^= 0x01U;
    CHECK(world.verify(forged) == CapabilityError::BadSignature);
    // The signer key field holds the holder's key: replace it with the delegate's (same length), and re-sign the
    // block with the delegate's own key, which the root did not name.
    const Token  parent = world.token();
    const usize  key_at = parent.bytes.size() + 8U + 35U + 42U + 16U + 2U;
    Token        stolen = child;
    u8           delegate_public[32];
    REQUIRE(world.delegate.public_key(delegate_public) == CryptoError::None);
    std::memcpy(stolen.bytes.data() + key_at, delegate_public, 32U);
    usize length = 0U;
    REQUIRE(world.delegate.sign(ConstSpan<u8>(stolen.bytes.data(), key_at - 2U),
                                Span<u8>(stolen.bytes.data() + key_at + 34U, 64U), length) == CryptoError::None);
    CHECK(world.verify(stolen) == CapabilityError::WrongHolder);
    // The block's operations: after its expiry, holder, grant kind, scope, range count and one range.
    const usize operations_at = parent.bytes.size() + 8U + 35U + 1U + 16U + 1U + 16U;
    Token       widened       = child;
    widened.bytes[operations_at + 7U] |= 0x10U; // 0x13: a bit the root does not grant
    CHECK(world.verify(widened) == CapabilityError::Widened);
}

TEST_CASE("Capabilities: verification refuses in the contract's order", "[crypto][capability][net-2]")
{
    World world;
    const Token token = world.token();
    Capability  granted;
    const Bytes proof = prove(token, world.binding, world.holder);
    const ConstSpan<u8> presented(proof.data(), proof.size());
    // No revocation generation yet: nothing is admitted.
    CHECK(world.verify(token, presented, granted) == CapabilityError::RevocationStale);
    REQUIRE(world.revoke() == CapabilityError::None);
    REQUIRE(world.verify(token, presented, granted) == CapabilityError::None);

    // Another audience, another session type, a lifetime above the audience's bound.
    {
        World other;
        other.root.audience = id(0x21U);
        REQUIRE(other.revoke() == CapabilityError::None);
        CHECK(other.verify(other.token()) == CapabilityError::WrongAudience);
        other.root.audience   = other.config.audience;
        other.root.grant.kind = ScopeKind::Project;
        CHECK(other.verify(other.token()) == CapabilityError::WrongAudience);
    }
    {
        World short_lived;
        short_lived.config.max_lifetime = kHour - 1U;
        short_lived.authority           = std::make_unique<CapabilityAuthority>(short_lived.config);
        REQUIRE(short_lived.revoke() == CapabilityError::None);
        CHECK(short_lived.verify(short_lived.token()) == CapabilityError::TooLong);
        short_lived.config.max_lifetime = kHour;
        short_lived.authority           = std::make_unique<CapabilityAuthority>(short_lived.config);
        REQUIRE(short_lived.revoke() == CapabilityError::None);
        CHECK(short_lived.verify(short_lived.token()) == CapabilityError::None);
    }
    // Issue skew and expiry, at their boundaries.
    CHECK(world.verify(token, presented, granted, kT0 - kMaxIssueSkew) == CapabilityError::None);
    CHECK(world.verify(token, presented, granted, kT0 - kMaxIssueSkew - 1U) == CapabilityError::NotYetValid);
    CHECK(world.verify(token, presented, granted, kT0 + kHour - 1U) == CapabilityError::None);
    CHECK(world.verify(token, presented, granted, kT0 + kHour) == CapabilityError::Expired);
    // A delegated token expires at its block's time.
    const Token child = attenuate(token, narrower(world), world.holder);
    CHECK(world.verify(child, kT0 + kHour / 2U - 1U) == CapabilityError::None);
    CHECK(world.verify(child, kT0 + kHour / 2U) == CapabilityError::Expired);

    // A peer-only audience refuses Proof tokens before anything costly.
    {
        World service;
        service.config.peer_only = true;
        service.authority        = std::make_unique<CapabilityAuthority>(service.config);
        REQUIRE(service.revoke() == CapabilityError::None);
        CHECK(service.verify(service.token()) == CapabilityError::PeerRequired);
    }
    // Issuer windows: none covers the issue time, or only one kind of key does.
    {
        // The authority's configuration views the pair's windows, so moving them after the revocation generation
        // was adopted moves them for the token.
        World late;
        REQUIRE(late.revoke() == CapabilityError::None);
        late.pair.issuers[0].issues_from = kT0 + 1U;
        late.pair.issuers[1].issues_from = kT0 + 1U;
        CHECK(late.verify(late.token()) == CapabilityError::UntrustedIssuer);
        late.pair.issuers[1].issues_from = 0U; // the traditional key alone
        CHECK(late.verify(late.token()) == CapabilityError::UntrustedIssuer);
    }
    // The policy snapshot refuses the holder's algorithm, or an issuer's.
    {
        World refused;
        REQUIRE(refused.revoke() == CapabilityError::None);
        refused.snapshot = policy(AlgorithmId::Ed25519);
        CHECK(refused.verify(refused.token()) == CapabilityError::AlgorithmRefused);
        refused.snapshot = policy(AlgorithmId::MlDsa65);
        CHECK(refused.verify(refused.token()) == CapabilityError::AlgorithmRefused);
        refused.snapshot = policy(AlgorithmId::MlDsa87);
        CHECK(refused.verify(refused.token()) == CapabilityError::None);
    }
    // The issuers' signatures: one missing, one forged, one labelled with another algorithm.
    {
        const Signer* only_one[] = {world.pair.signers[0]};
        CHECK(world.verify(issue(world.root, only_one)) == CapabilityError::MissingSignature);
        Token forged = token;
        forged.bytes[forged.bytes.size() - 1U] ^= 0x01U; // the Ed25519 signature's last byte
        CHECK(world.verify(forged) == CapabilityError::BadSignature);
        Token relabelled = token;
        const usize ed_entry = relabelled.bytes.size() - 64U - 36U;
        relabelled.bytes[ed_entry + 33U] ^= 0x01U; // the algorithm's low byte
        CHECK(world.verify(relabelled) == CapabilityError::BadSignature);
    }
    // The presentation: no proof, another session's binding, a proof for another token, by another key, malformed.
    CHECK(world.verify(token, ConstSpan<u8>(), granted) == CapabilityError::Unbound);
    u8 other_binding[kCapabilityBindingBytes];
    std::memset(other_binding, 0xB2, sizeof(other_binding));
    const Bytes wrong_session = prove(token, other_binding, world.holder);
    CHECK(world.verify(token, ConstSpan<u8>(wrong_session.data(), wrong_session.size()), granted) ==
          CapabilityError::Unbound);
    World       sibling;
    sibling.root.token        = id(0x02U);
    const Token sibling_token = sibling.token();
    const Bytes other_token   = prove(sibling_token, world.binding, world.holder);
    CHECK(world.verify(token, ConstSpan<u8>(other_token.data(), other_token.size()), granted) ==
          CapabilityError::Unbound);
    Bytes by_delegate = proof;
    u8    delegate_public[32];
    REQUIRE(world.delegate.public_key(delegate_public) == CryptoError::None);
    std::memcpy(by_delegate.data() + 2U, delegate_public, 32U);
    CHECK(world.verify(token, ConstSpan<u8>(by_delegate.data(), by_delegate.size()), granted) ==
          CapabilityError::Unbound);
    for (usize size = 0U; size < proof.size(); ++size)
    {
        CHECK(world.verify(token, ConstSpan<u8>(proof.data(), size), granted) == CapabilityError::Unbound);
    }
    Bytes trailing = proof;
    trailing.push_back(0U);
    CHECK(world.verify(token, ConstSpan<u8>(trailing.data(), trailing.size()), granted) == CapabilityError::Unbound);
    // The proof refuses a binding of another length and a Peer token.
    Bytes out;
    out.resize(kMaxCapabilityProofBytes);
    usize written = 0U;
    CHECK(prove_capability(token.view(), ConstSpan<u8>(world.binding, 31U), world.holder,
                           Span<u8>(out.data(), out.size()), written) == CapabilityError::BadConfig);
    CHECK(prove_capability(token.view(), world.binding, world.holder, Span<u8>(out.data(), 99U), written) ==
          CapabilityError::BufferTooSmall);
    World peer;
    peer.root.holder.binding = CapabilityBinding::Peer;
    CHECK(prove_capability(peer.token().view(), peer.binding, peer.holder, Span<u8>(out.data(), out.size()),
                           written) == CapabilityError::BadConfig);
}

TEST_CASE("Capabilities: revocation generations revoke tokens, keys, issue times and policy generations",
          "[crypto][capability][net-2]")
{
    World       world;
    const Token token = world.token();
    const Token child = attenuate(token, narrower(world), world.holder);
    REQUIRE(world.revoke() == CapabilityError::None);
    REQUIRE(world.verify(token) == CapabilityError::None);
    REQUIRE(world.verify(child) == CapabilityError::None);

    const auto with = [&](RevocationList list) {
        list.generation = world.authority->revocation_floor() + 1U;
        list.issued     = kT0 - kHour;
        list.expires    = kT0 + kDay;
        REQUIRE(world.revoke(list) == CapabilityError::None);
    };
    const CapabilityId tokens[] = {id(0x00U), world.root.token, id(0x09U)};
    RevocationList     list;
    list.tokens = tokens;
    with(list);
    CHECK(world.verify(token) == CapabilityError::Revoked);
    CHECK(world.verify(child) == CapabilityError::Revoked);
    with(RevocationList{});
    CHECK(world.verify(token) == CapabilityError::None);

    const KeyId delegate = id_of(world.delegate);
    list                 = RevocationList{};
    list.keys            = ConstSpan<KeyId>(&delegate, 1U);
    with(list);
    CHECK(world.verify(token) == CapabilityError::None);
    CHECK(world.verify(child) == CapabilityError::Revoked); // the delegate's key
    const KeyId holder = id_of(world.holder);
    list.keys          = ConstSpan<KeyId>(&holder, 1U);
    with(list);
    CHECK(world.verify(token) == CapabilityError::Revoked); // the subject's and the holder's key
    CHECK(world.verify(child) == CapabilityError::Revoked);

    list               = RevocationList{};
    list.revoke_before = kT0 + 1U;
    with(list);
    CHECK(world.verify(token) == CapabilityError::Revoked);
    list.revoke_before = kT0;
    with(list);
    CHECK(world.verify(token) == CapabilityError::None);
    list                       = RevocationList{};
    list.min_policy_generation = 2U;
    with(list);
    CHECK(world.verify(token) == CapabilityError::Revoked);

    // A current generation that expires stops every admission until the next one arrives.
    list            = RevocationList{};
    list.generation = world.authority->revocation_floor() + 1U;
    list.issued     = kT0 - kHour;
    list.expires    = kT0 + 120U;
    REQUIRE(world.revoke(list) == CapabilityError::None);
    CHECK(world.verify(token, kT0 + 119U) == CapabilityError::None);
    CHECK(world.verify(token, kT0 + 120U) == CapabilityError::RevocationStale);
}

TEST_CASE("Capabilities: revocation generations are monotonic, signed, dated and exact", "[crypto][capability][net-2]")
{
    World world;
    Bytes bytes;
    bytes.resize(kMaxRevocationBytes);
    usize      written = 0U;
    const auto encode  = [&](const RevocationList& list) {
        REQUIRE(issue_revocations(list, world.pair.signers, Span<u8>(bytes.data(), bytes.size()), written) ==
                CapabilityError::None);
        return ConstSpan<u8>(bytes.data(), written);
    };
    RevocationList list;
    list.generation = 5U;
    list.issued     = kT0 - kHour;
    list.expires    = kT0 + kDay;
    CHECK(world.authority->apply_revocations(encode(list), world.snapshot, kT0) == CapabilityError::None);
    CHECK(world.authority->revocation_floor() == 5U);
    CHECK(world.authority->apply_revocations(encode(list), world.snapshot, kT0) == CapabilityError::AlreadyCurrent);
    list.revoke_before = 1U;
    CHECK(world.authority->apply_revocations(encode(list), world.snapshot, kT0) == CapabilityError::Conflict);
    list.revoke_before = 0U;
    list.generation    = 4U;
    CHECK(world.authority->apply_revocations(encode(list), world.snapshot, kT0) == CapabilityError::Rollback);
    list.generation = 6U;
    list.issued     = kT0 - 2U * kHour;
    CHECK(world.authority->apply_revocations(encode(list), world.snapshot, kT0) ==
          CapabilityError::IssuedBeforeCurrent);
    list.issued = kT0 + kMaxIssueSkew + 1U;
    list.expires = list.issued + kDay;
    CHECK(world.authority->apply_revocations(encode(list), world.snapshot, kT0) == CapabilityError::NotYetIssued);
    list.issued  = kT0;
    list.expires = kT0 + 10U;
    CHECK(world.authority->apply_revocations(encode(list), world.snapshot, kT0 + 10U) == CapabilityError::Expired);
    CHECK(world.authority->revocation_floor() == 5U);

    // Signatures: one issuer missing, a forged one, an algorithm the policy refuses.
    list.expires                = kT0 + kDay;
    const Signer* only_one[] = {world.pair.signers[1]};
    REQUIRE(issue_revocations(list, only_one, Span<u8>(bytes.data(), bytes.size()), written) == CapabilityError::None);
    CHECK(world.authority->apply_revocations(ConstSpan<u8>(bytes.data(), written), world.snapshot, kT0) ==
          CapabilityError::MissingSignature);
    ConstSpan<u8> good = encode(list);
    bytes[written - 1U] ^= 0x01U;
    CHECK(world.authority->apply_revocations(good, world.snapshot, kT0) == CapabilityError::BadSignature);
    bytes[written - 1U] ^= 0x01U;
    CHECK(world.authority->apply_revocations(good, policy(AlgorithmId::Ed25519), kT0) ==
          CapabilityError::AlgorithmRefused);
    CHECK(world.authority->apply_revocations(good, world.snapshot, kT0) == CapabilityError::None);

    // A restart with the persisted floor refuses the old generations before verifying them.
    CapabilityConfig restarted = world.config;
    restarted.revocation_floor = world.authority->revocation_floor();
    const auto again           = std::make_unique<CapabilityAuthority>(restarted);
    list.generation            = 5U;
    CHECK(again->apply_revocations(encode(list), world.snapshot, kT0) == CapabilityError::Rollback);

    // The lists: ascending, each entry once, within their bounds; the dates and the generation number.
    const CapabilityId unsorted[] = {id(0x09U), id(0x01U)};
    const CapabilityId twice[]    = {id(0x01U), id(0x01U)};
    list.generation = 7U;
    list.tokens     = unsorted;
    CHECK(issue_revocations(list, world.pair.signers, Span<u8>(bytes.data(), bytes.size()), written) ==
          CapabilityError::BadList);
    list.tokens = twice;
    CHECK(issue_revocations(list, world.pair.signers, Span<u8>(bytes.data(), bytes.size()), written) ==
          CapabilityError::BadList);
    list.tokens = {};
    const KeyId keys[] = {id_of(world.holder), id_of(world.holder)};
    list.keys          = keys;
    CHECK(issue_revocations(list, world.pair.signers, Span<u8>(bytes.data(), bytes.size()), written) ==
          CapabilityError::BadList);
    list.keys = {};
    const auto many = std::make_unique<CapabilityId[]>(kMaxRevokedTokens + 1U);
    for (u32 i = 0U; i <= kMaxRevokedTokens; ++i)
    {
        many[i].bytes[14] = static_cast<u8>(i >> 8U);
        many[i].bytes[15] = static_cast<u8>(i);
    }
    list.tokens = ConstSpan<CapabilityId>(many.get(), kMaxRevokedTokens + 1U);
    CHECK(issue_revocations(list, world.pair.signers, Span<u8>(bytes.data(), bytes.size()), written) ==
          CapabilityError::TooLarge);
    list.tokens = ConstSpan<CapabilityId>(many.get(), kMaxRevokedTokens);
    const ConstSpan<u8> full = encode(list); // the largest list, with the hybrid signatures
    CHECK(world.authority->apply_revocations(full, world.snapshot, kT0) == CapabilityError::None);
    CHECK(world.verify(world.token()) == CapabilityError::None); // not listed: a binary search among 4,096
    list.tokens     = {};
    list.generation = 0U;
    CHECK(issue_revocations(list, world.pair.signers, Span<u8>(bytes.data(), bytes.size()), written) ==
          CapabilityError::BadField);
    list.generation = 8U;
    list.expires    = list.issued + kMaxRevocationLifetime + 1U;
    CHECK(issue_revocations(list, world.pair.signers, Span<u8>(bytes.data(), bytes.size()), written) ==
          CapabilityError::BadDates);
    list.expires = list.issued;
    CHECK(issue_revocations(list, world.pair.signers, Span<u8>(bytes.data(), bytes.size()), written) ==
          CapabilityError::BadDates);

    // Decoding: every proper prefix and a trailing byte.
    list.expires          = kT0 + kDay;
    list.tokens           = ConstSpan<CapabilityId>(many.get(), 2U);
    const ConstSpan<u8> small = encode(list);
    RevocationMessage   message;
    REQUIRE(decode_revocations(small, message) == CapabilityError::None);
    CHECK(message.token_count == 2U);
    CHECK(message.key_count == 0U);
    for (usize size = 0U; size < small.size(); ++size)
    {
        CHECK(decode_revocations(small.first(size), message) == CapabilityError::Truncated);
    }
    Bytes trailing(small.begin(), small.end());
    trailing.push_back(0U);
    CHECK(decode_revocations(ConstSpan<u8>(trailing.data(), trailing.size()), message) ==
          CapabilityError::TrailingBytes);
}

TEST_CASE("Capabilities: issuer rotation keeps old tokens until they expire and signs new ones with the new pair",
          "[crypto][capability][net-2]")
{
    // The old pair issues until kT0 + 30 minutes, the new pair from then on.
    const u64 rotation = kT0 + kHour / 2U;
    World     world;
    Pair      next;
    next.init(AlgorithmId::MlDsa87, AlgorithmId::EcdsaSecp384r1Sha384, 0x70U, rotation);
    world.pair.issuers[0].issues_until = rotation;
    world.pair.issuers[1].issues_until = rotation;
    const IssuerKey issuers[] = {world.pair.issuers[0], world.pair.issuers[1], next.issuers[0], next.issuers[1]};
    world.config.issuers      = issuers;
    world.authority           = std::make_unique<CapabilityAuthority>(world.config);
    REQUIRE(world.revoke() == CapabilityError::None);

    const Token old_token = world.token(); // issued at kT0 by the old pair
    CHECK(world.verify(old_token, rotation + 60U) == CapabilityError::None);
    world.root.issued  = rotation;
    world.root.expires = rotation + kHour;
    CHECK(world.verify(issue(world.root, world.pair.signers), rotation + 60U) == CapabilityError::MissingSignature);
    CHECK(world.verify(issue(world.root, next.signers), rotation + 60U) == CapabilityError::None);
    const Signer* both[] = {next.signers[0], next.signers[1], world.pair.signers[0], world.pair.signers[1]};
    CHECK(world.verify(issue(world.root, both), rotation + 60U) == CapabilityError::None); // others are ignored

    // Configuration: an empty window, a key twice, too many keys, a non-signature key.
    CapabilityConfig config = world.config;
    IssuerKey        bad[]  = {world.pair.issuers[0], world.pair.issuers[1]};
    bad[0].issues_from      = bad[0].issues_until;
    config.issuers          = bad;
    CHECK(validate_config(config) == CapabilityError::BadConfig);
    bad[0]         = world.pair.issuers[1];
    CHECK(validate_config(config) == CapabilityError::BadConfig);
    bad[0]               = world.pair.issuers[0];
    bad[0].key.algorithm = AlgorithmId::X25519;
    CHECK(validate_config(config) == CapabilityError::BadConfig);
    IssuerKey nine[kMaxIssuerKeys + 1U];
    for (usize i = 0U; i < kMaxIssuerKeys + 1U; ++i)
    {
        nine[i]                  = world.pair.issuers[0];
        nine[i].key.id.bytes[0]  = static_cast<u8>(i);
    }
    config.issuers = ConstSpan<IssuerKey>(nine, kMaxIssuerKeys + 1U);
    CHECK(validate_config(config) == CapabilityError::BadConfig);
    config.issuers = ConstSpan<IssuerKey>(nine, kMaxIssuerKeys);
    CHECK(validate_config(config) == CapabilityError::None);
    config          = world.config;
    config.verifier = nullptr;
    CHECK(validate_config(config) == CapabilityError::BadConfig);
    config          = world.config;
    config.audience = CapabilityId{};
    CHECK(validate_config(config) == CapabilityError::BadConfig);
    config      = world.config;
    config.kind = static_cast<ScopeKind>(0U);
    CHECK(validate_config(config) == CapabilityError::BadConfig);
    config              = world.config;
    config.max_lifetime = kMaxCapabilityLifetime + 1U;
    CHECK(validate_config(config) == CapabilityError::BadConfig);
    const CapabilityAuthority broken(config);
    Capability                granted;
    CHECK(broken.verify(old_token.view(), CapabilityPresentation{}, world.snapshot, kT0, granted) ==
          CapabilityError::BadConfig);
}

namespace
{
// Two TLS 1.3 sessions over memory: a game server with an Ed25519 credential and a client, mutually authenticated by
// `client_key` when it is given.
struct Link
{
    explicit Link(const SigningKey* client_key = nullptr)
    {
        make_key(server_key, AlgorithmId::Ed25519, 0x90U);
        REQUIRE(server_credential.init(server_key, "game.cerid.test") == TlsError::None);
        const KeyId server_pin = server_credential.key_id();
        TlsConfig   client_config;
        client_config.role        = TlsRole::Client;
        client_config.groups      = kGroups;
        client_config.signatures  = kSignatures;
        client_config.aeads       = kAeads;
        client_config.pinned      = ConstSpan<KeyId>(&server_pin, 1U);
        client_config.server_name = "game.cerid.test";
        KeyId client_pin;
        if (client_key != nullptr)
        {
            REQUIRE(client_credential.init(*client_key, "player.cerid.test") == TlsError::None);
            client_config.credential = &client_credential;
            client_pin               = client_credential.key_id();
        }
        REQUIRE(client_context.init(client_config) == TlsError::None);
        TlsConfig server_config;
        server_config.role        = TlsRole::Server;
        server_config.groups      = kGroups;
        server_config.aeads       = kAeads;
        server_config.credential  = &server_credential;
        server_config.server_name = "game.cerid.test";
        if (client_key != nullptr)
        {
            server_config.signatures = kSignatures;
            server_config.pinned     = ConstSpan<KeyId>(&client_pin, 1U);
        }
        REQUIRE(server_context.init(server_config) == TlsError::None);
        connect();
    }

    void connect()
    {
        REQUIRE(client.init(client_context) == TlsError::None);
        REQUIRE(server.init(server_context) == TlsError::None);
        for (int round = 0; round < 8; ++round)
        {
            move(client, server);
            move(server, client);
        }
        REQUIRE(client.state() == TlsState::Established);
        REQUIRE(server.state() == TlsState::Established);
    }

    static void move(TlsSession& from, TlsSession& to)
    {
        Bytes bytes;
        bytes.resize(from.pending());
        usize written = 0U;
        REQUIRE(from.take(Span<u8>(bytes.data(), bytes.size()), written) == TlsError::None);
        if (written > 0U)
        {
            REQUIRE(to.receive(ConstSpan<u8>(bytes.data(), written)) == TlsError::None);
        }
    }

    [[nodiscard]] Bytes binding(const TlsSession& session, ScopeKind kind = ScopeKind::World) const
    {
        Bytes out;
        out.resize(kCapabilityBindingBytes);
        REQUIRE(capability_binding(session, kind, Span<u8>(out.data(), out.size())) == TlsError::None);
        return out;
    }

    SigningKey    server_key;
    TlsCredential server_credential;
    TlsCredential client_credential;
    TlsContext    client_context;
    TlsContext    server_context;
    TlsSession    client;
    TlsSession    server;
};
} // namespace

TEST_CASE("Capabilities: a proof binds a presentation to its TLS session and no other", "[crypto][capability][net-2]")
{
    World world;
    REQUIRE(world.revoke() == CapabilityError::None);
    const Token token = world.token();
    Link        link;
    // The client proves with its end's binding; the server verifies with its own end's.
    const Bytes binding = link.binding(link.client);
    const Bytes proof   = prove(token, ConstSpan<u8>(binding.data(), binding.size()), world.holder);
    Capability  granted;
    CHECK(world.authority->verify(token.view(), link.server, ConstSpan<u8>(proof.data(), proof.size()),
                                  world.snapshot, kT0 + 60U, granted) == CapabilityError::None);
    CHECK(granted.holder.key == id_of(world.holder));
    // The same proof in another connection (a replay, or a token and proof stolen together) is Unbound.
    Link other;
    CHECK(world.authority->verify(token.view(), other.server, ConstSpan<u8>(proof.data(), proof.size()),
                                  world.snapshot, kT0 + 60U, granted) == CapabilityError::Unbound);
    link.connect(); // the same contexts, a new connection
    CHECK(world.authority->verify(token.view(), link.server, ConstSpan<u8>(proof.data(), proof.size()),
                                  world.snapshot, kT0 + 60U, granted) == CapabilityError::Unbound);
    // A proof made under another session type's label does not bind a world session.
    const Bytes project = link.binding(link.client, ScopeKind::Project);
    const Bytes wrong   = prove(token, ConstSpan<u8>(project.data(), project.size()), world.holder);
    CHECK(world.authority->verify(token.view(), link.server, ConstSpan<u8>(wrong.data(), wrong.size()),
                                  world.snapshot, kT0 + 60U, granted) == CapabilityError::Unbound);
    // A session that has not finished its handshake presents nothing.
    TlsSession idle;
    REQUIRE(idle.init(link.server_context) == TlsError::None);
    CHECK(world.authority->verify(token.view(), idle, ConstSpan<u8>(proof.data(), proof.size()), world.snapshot,
                                  kT0 + 60U, granted) == CapabilityError::Unbound);
}

TEST_CASE("Capabilities: service links admit Peer tokens over mutual authentication only",
          "[crypto][capability][net-2]")
{
    // A fleet server (the holder) presents a service token to a matchmaking service over mutual TLS.
    World service;
    service.config.kind      = ScopeKind::Service;
    service.config.peer_only = true;
    service.authority        = std::make_unique<CapabilityAuthority>(service.config);
    REQUIRE(service.revoke() == CapabilityError::None);
    service.root.subject.kind   = PrincipalKind::Server;
    service.root.holder.binding = CapabilityBinding::Peer;
    service.root.grant.kind     = ScopeKind::Service;
    const Token token           = service.token();

    Link       mutual(&service.holder);
    Capability granted;
    REQUIRE(mutual.server.peer_key() == id_of(service.holder));
    CHECK(service.authority->verify(token.view(), mutual.server, ConstSpan<u8>(), service.snapshot, kT0 + 60U,
                                    granted) == CapabilityError::None);
    CHECK(granted.holder.binding == CapabilityBinding::Peer);
    CHECK(granted.subject.kind == PrincipalKind::Server);
    // The same token over a session without client authentication, or authenticated by another key.
    Link anonymous;
    CHECK(service.authority->verify(token.view(), anonymous.server, ConstSpan<u8>(), service.snapshot, kT0 + 60U,
                                    granted) == CapabilityError::Unbound);
    Link stranger(&service.delegate);
    CHECK(service.authority->verify(token.view(), stranger.server, ConstSpan<u8>(), service.snapshot, kT0 + 60U,
                                    granted) == CapabilityError::Unbound);
    // A Proof token is refused at a peer-only service even with a valid proof.
    service.root.holder.binding = CapabilityBinding::Proof;
    const Token proof_token     = service.token();
    const Bytes binding         = mutual.binding(mutual.client, ScopeKind::Service);
    const Bytes proof = prove(proof_token, ConstSpan<u8>(binding.data(), binding.size()), service.holder);
    CHECK(service.authority->verify(proof_token.view(), mutual.server, ConstSpan<u8>(proof.data(), proof.size()),
                                    service.snapshot, kT0 + 60U, granted) == CapabilityError::PeerRequired);
}

TEST_CASE("Capabilities: verification and revocation run on several threads at once", "[crypto][capability][net-2]")
{
    World world;
    REQUIRE(world.revoke() == CapabilityError::None);
    const Token token = world.token();
    const Bytes proof = prove(token, world.binding, world.holder);
    // Twenty later generations, signed before the threads start (assertions stay on this thread).
    constexpr u32 generation_count = 20U;
    Bytes         generations[generation_count];
    for (u32 i = 0U; i < generation_count; ++i)
    {
        RevocationList list;
        list.generation = 2U + i;
        list.issued     = kT0 - kHour;
        list.expires    = kT0 + kDay;
        generations[i].resize(kMaxRevocationBytes);
        usize written = 0U;
        REQUIRE(issue_revocations(list, world.pair.signers, Span<u8>(generations[i].data(), generations[i].size()),
                                  written) == CapabilityError::None);
        generations[i].resize(written);
    }
    std::atomic<u32> applied{0U};
    std::atomic<u32> verified{0U};
    std::thread      revoker([&] {
        for (const Bytes& generation : generations)
        {
            if (world.authority->apply_revocations(ConstSpan<u8>(generation.data(), generation.size()),
                                                   world.snapshot, kT0) == CapabilityError::None)
            {
                applied.fetch_add(1U);
            }
        }
    });
    std::thread threads[4];
    for (std::thread& thread : threads)
    {
        thread = std::thread([&] {
            for (int i = 0; i < 25; ++i)
            {
                Capability granted;
                if (world.verify(token, ConstSpan<u8>(proof.data(), proof.size()), granted) == CapabilityError::None)
                {
                    verified.fetch_add(1U);
                }
            }
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    revoker.join();
    CHECK(applied.load() == generation_count);
    CHECK(verified.load() == 100U);
    CHECK(world.authority->revocation_floor() == 1U + generation_count);
}

TEST_CASE("Capabilities: names and labels", "[crypto][capability][net-2]")
{
    CHECK(capability_error_name(CapabilityError::None) == "none");
    CHECK(capability_error_name(CapabilityError::Widened) == "widened");
    CHECK(capability_error_name(CapabilityError::ProviderFailure) == "provider-failure");
    CHECK(capability_error_name(static_cast<CapabilityError>(200U)) == "unknown");
    // Every error has its own name.
    for (u8 a = 0U; a <= static_cast<u8>(CapabilityError::ProviderFailure); ++a)
    {
        CHECK(capability_error_name(static_cast<CapabilityError>(a)) != "unknown");
        for (u8 b = 0U; b < a; ++b)
        {
            CHECK(capability_error_name(static_cast<CapabilityError>(a)) !=
                  capability_error_name(static_cast<CapabilityError>(b)));
        }
    }
    // Each session type has its own private-use exporter label (RFC 5705 section 4).
    for (u8 a = 1U; a <= 6U; ++a)
    {
        const StringView label = capability_binding_label(static_cast<ScopeKind>(a));
        CHECK(label.starts_with("EXPERIMENTAL Cerid capability "));
        for (u8 b = 1U; b < a; ++b)
        {
            CHECK(label != capability_binding_label(static_cast<ScopeKind>(b)));
        }
    }
    CHECK(capability_binding_label(static_cast<ScopeKind>(0U)).empty());
    CHECK(capability_binding_label(static_cast<ScopeKind>(7U)).empty());
}
