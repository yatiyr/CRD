// crd-fuzz-crypto-capability: crd-crypto's capabilities (NET.2; docs/design/network-foundation.md#capability-tokens)
// under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data.
//   0  a token as bytes, decoded;
//   1  a revocation generation as bytes, decoded;
//   2  a generated life: a root issued by a hybrid issuer pair, up to five delegations (narrower or widened in one
//      field), a revocation generation, an audience configuration, a time, a presentation and a flipped byte, all read
//      from the bytes, the tokens and proofs signed by real keys;
//   3  a message schema as bytes, decoded;
//   4  a generated schema life (S18): a schema's kind, operations, idempotent ones, audience, issue time, policy
//      generation and signers, a revocation generation and a time, then an operation asked of a capability, its object
//      and its delivery (after the handshake, as early data, or unknown), and a flipped byte.
//
// Oracles, none of them the codec's or the authority's own verdict:
//   * an independent restatement of both layouts, their bounds and the contract's validation order decides each
//     decoder's exact result, and an accepted token's or generation's fields equal the restatement's;
//   * a restated containment rule (each narrower range inside the one wider range holding its first object) decides
//     whether a delegation is refused as Widened, and a delegation that is not never verifies as such;
//   * a model of the contract's verification order (lifetime bound, issue skew, the chain's earliest expiry, peer-only,
//     revocation freshness and listings, the presentation) decides the exact verdict;
//   * any single flipped byte of a token that verifies makes it fail verification;
//   * a restated schema layout and validation decide decode_schema's result; a model of the schema verification order
//     decides verify_schema's, a verified schema's digest is the SHA-256 of its signed bytes and any flipped byte of it
//     fails verification; and the restated admission rule (the same audience and kind, declared, idempotent unless
//     delivered after the handshake, granted on the object) decides authorize_operation's verdict.
#include "../policy_test_signer.hpp"

#include <crd/fuzz/harness.hpp>

#include <crd/crypto/capability.hpp>
#include <crd/crypto/provider.hpp>

#include <cstring>
#include <memory>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;

constexpr u64 kT0 = 1800000000U;

u16 be16(const u8* p)
{
    return static_cast<u16>((p[0] << 8U) | p[1]);
}

u64 be64(const u8* p)
{
    u64 value = 0U;
    for (usize i = 0U; i < 8U; ++i)
    {
        value = (value << 8U) | p[i];
    }
    return value;
}

// ---- The restated layouts ------------------------------------------------------------------------------------

// A cursor that answers how many bytes remain; every read is checked by the caller first.
struct Cursor
{
    const u8* data;
    usize     size;
    usize     at = 0U;

    [[nodiscard]] bool has(usize count) const { return size - at >= count; }
};

bool signature_class(u16 algorithm)
{
    const AlgorithmClass type = algorithm_class(static_cast<AlgorithmId>(algorithm));
    return type == AlgorithmClass::PostQuantumSignature || type == AlgorithmClass::ClassicalSignature;
}

bool all_zero(const u8* bytes, usize size)
{
    for (usize i = 0U; i < size; ++i)
    {
        if (bytes[i] != 0U)
        {
            return false;
        }
    }
    return true;
}

// A grant at the cursor: Truncated or TooLarge as the fields come, then the restated validation (kind, ranges).
CapabilityError restate_grant(Cursor& c)
{
    if (!c.has(18U))
    {
        return CapabilityError::Truncated;
    }
    const u8 kind  = c.data[c.at];
    const u8 count = c.data[c.at + 17U];
    c.at += 18U;
    if (count > 16U)
    {
        return CapabilityError::TooLarge;
    }
    if (!c.has(usize{count} * 16U + 24U))
    {
        return CapabilityError::Truncated;
    }
    const u8* ranges = c.data + c.at;
    c.at += usize{count} * 16U + 24U;
    if (kind < 1U || kind > 6U)
    {
        return CapabilityError::BadField;
    }
    for (u32 i = 0U; i < count; ++i)
    {
        const u64 first = be64(ranges + 16U * i);
        const u64 last  = be64(ranges + 16U * i + 8U);
        // Ascending with a gap of at least one object: the previous last is below first - 1.
        const bool gap = i == 0U || (first >= 2U && be64(ranges + 16U * (i - 1U) + 8U) < first - 1U);
        if (first > last || !gap)
        {
            return CapabilityError::BadField;
        }
    }
    return CapabilityError::None;
}

bool holder_valid(const u8* holder)
{
    return signature_class(be16(holder)) && (holder[34] == 1U || holder[34] == 2U);
}

// A u16-sized field: Truncated, `empty` for none, TooLarge above `bound`.
CapabilityError restate_sized(Cursor& c, usize bound, CapabilityError empty)
{
    if (!c.has(2U))
    {
        return CapabilityError::Truncated;
    }
    const usize length = be16(c.data + c.at);
    c.at += 2U;
    if (length == 0U)
    {
        return empty;
    }
    if (length > bound)
    {
        return CapabilityError::TooLarge;
    }
    if (!c.has(length))
    {
        return CapabilityError::Truncated;
    }
    c.at += length;
    return CapabilityError::None;
}

// The signature list: structure, then the entries (empty, duplicate keys). `out_count` is the entries read.
CapabilityError restate_list(Cursor& c, bool check_trailing, u32& out_count)
{
    if (!c.has(1U))
    {
        return CapabilityError::Truncated;
    }
    const u8 count = c.data[c.at++];
    if (count == 0U)
    {
        return CapabilityError::Unsigned;
    }
    if (count > 4U)
    {
        return CapabilityError::TooLarge;
    }
    const u8* keys[4]    = {};
    usize     lengths[4] = {};
    for (u32 i = 0U; i < count; ++i)
    {
        if (!c.has(36U))
        {
            return CapabilityError::Truncated;
        }
        keys[i]    = c.data + c.at;
        lengths[i] = be16(c.data + c.at + 34U);
        c.at += 36U;
        if (lengths[i] > kMaxSignatureBytes)
        {
            return CapabilityError::TooLarge;
        }
        if (!c.has(lengths[i]))
        {
            return CapabilityError::Truncated;
        }
        c.at += lengths[i];
    }
    if (check_trailing && c.at != c.size)
    {
        return CapabilityError::TrailingBytes;
    }
    for (u32 i = 0U; i < count; ++i)
    {
        if (lengths[i] == 0U)
        {
            return CapabilityError::BadSignatureEntry;
        }
        for (u32 j = 0U; j < i; ++j)
        {
            if (std::memcmp(keys[i], keys[j], 32U) == 0)
            {
                return CapabilityError::DuplicateSignature;
            }
        }
    }
    out_count = count;
    return CapabilityError::None;
}

CapabilityError restate_head(Cursor& c, const u8 (&magic)[8])
{
    if (!c.has(8U))
    {
        return CapabilityError::Truncated;
    }
    if (std::memcmp(c.data, magic, 8U) != 0)
    {
        return CapabilityError::BadMagic;
    }
    if (!c.has(9U))
    {
        return CapabilityError::Truncated;
    }
    c.at = 9U;
    return c.data[8] == 1U ? CapabilityError::None : CapabilityError::UnsupportedFormat;
}

struct RestatedToken
{
    u32 signatures = 0U;
    u32 blocks     = 0U;
    u64 issued     = 0U;
    u64 expires    = 0U;
};

CapabilityError restate_token(const u8* data, usize size, RestatedToken& out)
{
    if (size > 49152U)
    {
        return CapabilityError::TooLarge;
    }
    Cursor          c{data, size};
    CapabilityError error = restate_head(c, kCapabilityMagic);
    if (error != CapabilityError::None)
    {
        return error;
    }
    if (!c.has(124U)) // token, three dates, audience, subject kind and key, holder
    {
        return CapabilityError::Truncated;
    }
    const u8* root = data + 9U;
    c.at += 124U;
    error = restate_grant(c);
    if (error != CapabilityError::None)
    {
        return error;
    }
    const u8 subject = root[56];
    if (all_zero(root, 16U) || all_zero(root + 40U, 16U) || subject < 1U || subject > 5U || !holder_valid(root + 89U))
    {
        return CapabilityError::BadField;
    }
    out.issued  = be64(root + 16U);
    out.expires = be64(root + 24U);
    if (out.expires <= out.issued || out.expires - out.issued > 86400U)
    {
        return CapabilityError::BadDates;
    }
    error = restate_list(c, false, out.signatures);
    if (error != CapabilityError::None)
    {
        return error;
    }
    while (c.at != c.size)
    {
        if (out.blocks == 4U)
        {
            return CapabilityError::TooLarge;
        }
        if (!c.has(43U))
        {
            return CapabilityError::Truncated;
        }
        const u8* holder = data + c.at + 8U;
        c.at += 43U;
        error = restate_grant(c);
        if (error != CapabilityError::None)
        {
            return error;
        }
        if (!holder_valid(holder))
        {
            return CapabilityError::BadField;
        }
        error = restate_sized(c, kMaxHolderKeyBytes, CapabilityError::BadField);
        if (error == CapabilityError::None)
        {
            error = restate_sized(c, kMaxSignatureBytes, CapabilityError::BadSignatureEntry);
        }
        if (error != CapabilityError::None)
        {
            return error;
        }
        ++out.blocks;
    }
    return CapabilityError::None;
}

bool ascending(const u8* entries, usize count, usize size)
{
    for (usize i = 1U; i < count; ++i)
    {
        if (std::memcmp(entries + (i - 1U) * size, entries + i * size, size) >= 0)
        {
            return false;
        }
    }
    return true;
}

CapabilityError restate_revocations(const u8* data, usize size, u64& generation, u32& tokens, u32& keys)
{
    if (size > kMaxRevocationBytes)
    {
        return CapabilityError::TooLarge;
    }
    Cursor          c{data, size};
    CapabilityError error = restate_head(c, kRevocationMagic);
    if (error != CapabilityError::None)
    {
        return error;
    }
    if (!c.has(40U))
    {
        return CapabilityError::Truncated;
    }
    generation       = be64(data + 9U);
    const u64 issued = be64(data + 17U);
    const u64 expiry = be64(data + 25U);
    c.at += 40U;
    if (generation == 0U)
    {
        return CapabilityError::BadField;
    }
    if (expiry <= issued || expiry - issued > 604800U)
    {
        return CapabilityError::BadDates;
    }
    const u8* lists[2] = {};
    u32       counts[2] = {};
    for (u32 list = 0U; list < 2U; ++list)
    {
        if (!c.has(2U))
        {
            return CapabilityError::Truncated;
        }
        counts[list] = be16(data + c.at);
        c.at += 2U;
        if (counts[list] > (list == 0U ? 4096U : 512U))
        {
            return CapabilityError::TooLarge;
        }
        const usize bytes = usize{counts[list]} * (list == 0U ? 16U : 32U);
        if (!c.has(bytes))
        {
            return CapabilityError::Truncated;
        }
        lists[list] = data + c.at;
        c.at += bytes;
    }
    if (!ascending(lists[0], counts[0], 16U) || !ascending(lists[1], counts[1], 32U))
    {
        return CapabilityError::BadList;
    }
    u32 signatures = 0U;
    tokens         = counts[0];
    keys           = counts[1];
    return restate_list(c, true, signatures);
}

// A schema: the head, the fixed fields (Truncated as they come), the restated validation, then an exact list.
CapabilityError restate_schema(const u8* data, usize size, u64& operations, u64& idempotent, u32& signatures)
{
    if (size > 34816U)
    {
        return CapabilityError::TooLarge;
    }
    Cursor          c{data, size};
    CapabilityError error = restate_head(c, kSchemaMagic);
    if (error != CapabilityError::None)
    {
        return error;
    }
    if (!c.has(69U)) // kind, schema, major, minor, audience, issued, policy generation, operations, idempotent
    {
        return CapabilityError::Truncated;
    }
    c.at += 69U;
    const u8 kind = data[9];
    operations    = be64(data + 62U);
    idempotent    = be64(data + 70U);
    if (kind < 1U || kind > 6U || all_zero(data + 10U, 16U) || all_zero(data + 30U, 16U) || operations == 0U ||
        (idempotent | operations) != operations)
    {
        return CapabilityError::BadField;
    }
    return restate_list(c, true, signatures);
}

bool decode_schema_bytes(const u8* data, usize size)
{
    u64                   operations = 0U;
    u64                   idempotent = 0U;
    u32                   signatures = 0U;
    const CapabilityError want       = restate_schema(data, size, operations, idempotent, signatures);
    SchemaMessage         message;
    const CapabilityError got = decode_schema(ConstSpan<u8>(data, size), message);
    CRD_FUZZ_REQUIRE(got == want);
    if (got != CapabilityError::None)
    {
        return false;
    }
    CRD_FUZZ_REQUIRE(message.schema.operations == operations && message.schema.idempotent == idempotent);
    CRD_FUZZ_REQUIRE(message.signature_count == signatures && message.signed_bytes.data() == data &&
                     message.signed_bytes.size() == 78U);
    return true;
}

bool decode_token(const u8* data, usize size)
{
    RestatedToken         expected;
    const CapabilityError want = restate_token(data, size, expected);
    CapabilityToken       token;
    const CapabilityError got  = decode_capability(ConstSpan<u8>(data, size), token);
    CRD_FUZZ_REQUIRE(got == want);
    if (got != CapabilityError::None)
    {
        return false;
    }
    CRD_FUZZ_REQUIRE(token.signature_count == expected.signatures && token.block_count == expected.blocks);
    CRD_FUZZ_REQUIRE(token.root.issued == expected.issued && token.root.expires == expected.expires);
    CRD_FUZZ_REQUIRE(token.root_bytes.data() == data);
    for (u32 i = 0U; i < token.block_count; ++i)
    {
        const CapabilityBlock& block = token.blocks[i];
        CRD_FUZZ_REQUIRE(block.signed_bytes.data() == data && block.signer_public.data() > block.signed_bytes.data());
        CRD_FUZZ_REQUIRE(block.signer_public.data() == data + block.signed_bytes.size() + 2U);
    }
    return true;
}

bool decode_generation(const u8* data, usize size)
{
    u64                   generation = 0U;
    u32                   tokens     = 0U;
    u32                   keys       = 0U;
    const CapabilityError want       = restate_revocations(data, size, generation, tokens, keys);
    RevocationMessage     message;
    const CapabilityError got = decode_revocations(ConstSpan<u8>(data, size), message);
    CRD_FUZZ_REQUIRE(got == want);
    if (got != CapabilityError::None)
    {
        return false;
    }
    CRD_FUZZ_REQUIRE(message.generation == generation && message.token_count == tokens && message.key_count == keys);
    CRD_FUZZ_REQUIRE(message.tokens.size() == usize{tokens} * 16U && message.keys.size() == usize{keys} * 32U);
    return true;
}

// ---- The generated life --------------------------------------------------------------------------------------

// The bytes of a generated life at fixed offsets, zero past the input's end.
struct Plan
{
    const u8* data;
    usize     size;

    [[nodiscard]] u8  byte(usize at) const { return at < size ? data[at] : u8{0U}; }
    [[nodiscard]] u16 word(usize at) const { return static_cast<u16>((byte(at) << 8U) | byte(at + 1U)); }
};

// The life's header, then kAttemptBytes per delegation attempt.
//   0 lifetime word | 2 choices (bits 0-1 the root holder, bit 2 Peer) | 3 token id | 4 policy generation
//   | 5 range count | 6 four (gap, length) pairs | 14 operations word | 16 operations per second | 17 bytes per
//   second word | 19 objects | 20 attempts | 21 audience bits (0 its own lifetime bound, 1 peer-only)
//   | 22 lifetime bound word | 24 revocation choice | 25 revocation expiry word | 27 listed key | 28 time word
//   | 30 presentation | 31 proof flip word | 33 token flip word
// An attempt: 0 how | 1 next holder | 2 Peer | 3 expiry cut word | 5 operations mask word | 7 operations per second
//   cut | 8 objects cut | 9 widened bit | 10 four (keep, first, last) triples
constexpr usize kLifeHeader   = 35U;
constexpr usize kAttemptBytes = 24U;

struct Keys
{
    SigningKey     issuer_pq;
    SigningKey     issuer_classical;
    u8             publics[2][kMaxHolderKeyBytes] = {};
    IssuerKey      issuers[2];
    const Signer*  signers[2] = {};
    SigningKey     holders[4]; // Ed25519 twice, ECDSA P-256 twice
    KeyId          holder_ids[4];
    PolicySnapshot policy;
};

void key_from(SigningKey& key, AlgorithmId algorithm, u8 salt)
{
    u8 seed[32];
    for (usize i = 0U; i < sizeof(seed); ++i)
    {
        seed[i] = static_cast<u8>(salt + i + 1U);
    }
    CRD_FUZZ_REQUIRE(key.from_seed(algorithm, ConstSpan<u8>(seed, signature_sizes(algorithm).seed)) ==
                     CryptoError::None);
}

const Keys& keys()
{
    static const std::unique_ptr<Keys> kMade = [] {
        auto k = std::make_unique<Keys>();
        key_from(k->issuer_pq, AlgorithmId::MlDsa44, 0x10U);
        key_from(k->issuer_classical, AlgorithmId::Ed25519, 0x20U);
        const SigningKey* issuers[2] = {&k->issuer_pq, &k->issuer_classical};
        for (usize i = 0U; i < 2U; ++i)
        {
            const usize size = signature_sizes(issuers[i]->algorithm()).public_key;
            CRD_FUZZ_REQUIRE(issuers[i]->public_key(Span<u8>(k->publics[i], size)) == CryptoError::None);
            k->issuers[i] = {{key_id_of(ConstSpan<u8>(k->publics[i], size)), issuers[i]->algorithm(),
                              ConstSpan<u8>(k->publics[i], size)},
                             0U,
                             kNever};
            k->signers[i] = issuers[i];
        }
        for (u8 i = 0U; i < 4U; ++i)
        {
            key_from(k->holders[i], i < 2U ? AlgorithmId::Ed25519 : AlgorithmId::EcdsaSecp256r1Sha256,
                     static_cast<u8>(0x40U + 0x10U * i));
            u8          public_key[kMaxHolderKeyBytes];
            const usize size = signature_sizes(k->holders[i].algorithm()).public_key;
            CRD_FUZZ_REQUIRE(k->holders[i].public_key(Span<u8>(public_key, size)) == CryptoError::None);
            k->holder_ids[i] = key_id_of(ConstSpan<u8>(public_key, size));
        }
        // A policy accepting the four signature algorithms, through a store the test signer feeds.
        PolicyContent content;
        content.generation = 1U;
        content.issued     = kT0 - 86400U;
        content.expires    = kT0 + 100U * 86400U;
        const AlgorithmId accepted[] = {AlgorithmId::MlDsa44, AlgorithmId::Ed25519, AlgorithmId::EcdsaSecp256r1Sha256};
        for (const AlgorithmId algorithm : accepted)
        {
            content.rules[content.rule_count++] = {algorithm, 0U, 0U, 0U, kNever};
        }
        u8    body[1024];
        usize body_size = 0U;
        CRD_FUZZ_REQUIRE(write_policy_body(content, body, body_size) == PolicyError::None);
        u8           tags[2][test::kTestSignatureBytes];
        KeySignature signatures[2];
        for (usize i = 0U; i < 2U; ++i)
        {
            const TrustKey& key = test::kTestTrust[i];
            test::test_sign(key.algorithm, key.public_key, ConstSpan<u8>(body, body_size), tags[i]);
            signatures[i] = {key.id, key.algorithm, ConstSpan<u8>(tags[i])};
        }
        u8    generation[2048];
        usize size = 0U;
        CRD_FUZZ_REQUIRE(write_policy(ConstSpan<u8>(body, body_size), signatures, generation, size) ==
                         PolicyError::None);
        static const test::TestVerifier kVerifier;
        PolicyConfig                    config;
        config.trust     = test::kTestTrust;
        config.verifier  = &kVerifier;
        config.supported = provider_algorithms();
        PolicyStore store(config);
        CRD_FUZZ_REQUIRE(store.apply(ConstSpan<u8>(generation, size), kT0) == PolicyError::None);
        k->policy = store.snapshot();
        return k;
    }();
    return *kMade;
}

// The restated containment rule: each narrower range lies inside the wider range that holds its first object.
bool restated_within(const Grant& narrower, const Grant& wider)
{
    if (narrower.kind != wider.kind || std::memcmp(narrower.scope.bytes, wider.scope.bytes, 16U) != 0 ||
        (narrower.operations | wider.operations) != wider.operations ||
        narrower.budget.operations_per_second > wider.budget.operations_per_second ||
        narrower.budget.bytes_per_second > wider.budget.bytes_per_second ||
        narrower.budget.objects > wider.budget.objects)
    {
        return false;
    }
    if (wider.range_count == 0U)
    {
        return true;
    }
    for (u32 i = 0U; i < narrower.range_count; ++i)
    {
        const ObjectRange& range = narrower.ranges[i];
        u32                j     = 0U;
        while (j < wider.range_count && wider.ranges[j].last < range.first)
        {
            ++j;
        }
        if (j == wider.range_count || wider.ranges[j].first > range.first || wider.ranges[j].last < range.last)
        {
            return false;
        }
    }
    return narrower.range_count > 0U;
}

// Canonical ranges from the plan: each after a gap of at least one object.
u32 ranges_from(const Plan& plan, ObjectRange* out)
{
    const u32 count = plan.byte(5U) % 5U;
    u64       at    = 0U;
    for (u32 i = 0U; i < count; ++i)
    {
        const u64 first = at + 1U + plan.byte(6U + 2U * i) % 32U;
        const u64 last  = first + plan.byte(7U + 2U * i) % 64U;
        out[i]          = {first, last};
        at              = last + 1U;
    }
    return count;
}

// A delegation of `parent` from the attempt at `a`: narrower, or (by `how`) wider in exactly one way.
Grant delegated(const Plan& plan, usize a, const Grant& parent, u8 how)
{
    Grant grant                        = parent;
    grant.operations                   = parent.operations & plan.word(a + 5U);
    grant.budget.operations_per_second = parent.budget.operations_per_second - plan.byte(a + 7U) % 4U;
    grant.budget.objects               = parent.budget.objects - plan.byte(a + 8U) % 4U;
    if (parent.range_count > 0U)
    {
        // A slice of each kept range of the parent (the last kept when none was before it).
        grant.range_count = 0U;
        for (u32 i = 0U; i < parent.range_count; ++i)
        {
            const ObjectRange& range = parent.ranges[i];
            const usize        at    = a + 10U + 3U * i;
            if ((plan.byte(at) & 1U) != 0U || (i + 1U == parent.range_count && grant.range_count == 0U))
            {
                const u64 first = range.first + plan.byte(at + 1U) % (range.last - range.first + 1U);
                const u64 last  = first + plan.byte(at + 2U) % (range.last - first + 1U);
                grant.ranges[grant.range_count++] = {first, last};
            }
        }
    }
    switch (how)
    {
    case 1U:
        grant.operations |= u64{1U} << (16U + plan.byte(a + 9U) % 16U);
        break;
    case 2U:
        grant.budget.bytes_per_second = parent.budget.bytes_per_second + 1U;
        break;
    case 3U:
        if (parent.range_count > 0U)
        {
            grant.ranges[grant.range_count - 1U].last = parent.ranges[parent.range_count - 1U].last + 1U;
        }
        break;
    case 4U:
        grant.range_count = 0U;
        break;
    case 5U:
        grant.scope.bytes[0] ^= 0x01U;
        break;
    default:
        break;
    }
    return grant;
}

bool life(const Plan& plan)
{
    const Keys& k        = keys();
    const u64   lifetime = 60U + plan.word(0U) % 86341U;
    u32         holder   = plan.byte(2U) % 4U;

    CapabilityRoot root;
    root.token.bytes[0]                     = 0x01U;
    root.token.bytes[1]                     = plan.byte(3U);
    root.issued                             = kT0;
    root.expires                            = kT0 + lifetime;
    root.policy_generation                  = 1U + plan.byte(4U) % 3U;
    root.audience.bytes[0]                  = 0xA0U;
    root.subject                            = {PrincipalKind::User, k.holder_ids[holder]};
    root.holder                             = {k.holders[holder].algorithm(), k.holder_ids[holder],
                                               (plan.byte(2U) & 4U) != 0U ? CapabilityBinding::Peer
                                                                          : CapabilityBinding::Proof};
    root.grant.kind                         = ScopeKind::World;
    root.grant.scope.bytes[0]               = 0x55U;
    root.grant.range_count                  = ranges_from(plan, root.grant.ranges);
    root.grant.operations                   = plan.word(14U);
    root.grant.budget.operations_per_second = 100U + plan.byte(16U);
    root.grant.budget.bytes_per_second      = 10000U + plan.word(17U);
    root.grant.budget.objects               = 50U + plan.byte(19U);

    static u8 token[kMaxCapabilityBytes];
    static u8 next[kMaxCapabilityBytes];
    usize     size = 0U;
    CRD_FUZZ_REQUIRE(issue_capability(root, k.signers, token, size) == CapabilityError::None);

    // Delegations: the restated rule decides each refusal; accepted ones extend the chain.
    Grant             grant                        = root.grant;
    u64               expires                      = root.expires;
    CapabilityBinding binding                      = root.holder.binding;
    KeyId             chain[kMaxAttenuations + 1U] = {k.holder_ids[holder]};
    u32               depth                        = 0U;
    const u32         attempts                     = plan.byte(20U) % 6U;
    for (u32 attempt = 0U; attempt < attempts; ++attempt)
    {
        const usize           a           = kLifeHeader + attempt * kAttemptBytes;
        const u8              how         = plan.byte(a) % 10U;
        const u32             next_holder = plan.byte(a + 1U) % 4U;
        const bool            peer        = binding == CapabilityBinding::Peer || (plan.byte(a + 2U) & 1U) != 0U;
        CapabilityAttenuation block;
        block.holder  = {k.holders[next_holder].algorithm(), k.holder_ids[next_holder],
                         peer ? CapabilityBinding::Peer : CapabilityBinding::Proof};
        block.expires = how == 6U ? expires + 1U : expires - plan.word(a + 3U) % 120U;
        block.grant   = delegated(plan, a, grant, how);
        if (how == 7U)
        {
            block.holder.binding = CapabilityBinding::Proof; // loosens a Peer parent
        }
        const bool loosened = binding == CapabilityBinding::Peer && block.holder.binding != CapabilityBinding::Peer;
        const bool within   = restated_within(block.grant, grant) && block.expires <= expires && !loosened;
        usize      written  = 0U;
        const CapabilityError result =
            attenuate_capability(ConstSpan<u8>(token, size), block, k.holders[holder], next, written);
        if (!within)
        {
            CRD_FUZZ_REQUIRE(result == CapabilityError::Widened);
            continue;
        }
        if (depth == kMaxAttenuations)
        {
            CRD_FUZZ_REQUIRE(result == CapabilityError::TooLarge);
            continue;
        }
        CRD_FUZZ_REQUIRE(result == CapabilityError::None);
        std::memcpy(token, next, written);
        size           = written;
        grant          = block.grant;
        expires        = block.expires;
        binding        = block.holder.binding;
        holder         = next_holder;
        chain[++depth] = k.holder_ids[holder];
    }

    // The audience, its revocation generation and the time.
    static const ProviderVerifier kVerifier;
    CapabilityConfig              config;
    config.issuers                = ConstSpan<IssuerKey>(k.issuers);
    config.verifier               = &kVerifier;
    config.kind                   = ScopeKind::World;
    config.audience               = root.audience;
    config.max_lifetime           = (plan.byte(21U) & 1U) != 0U ? 1U + plan.word(22U) % 86400U : 0U;
    config.peer_only              = (plan.byte(21U) & 2U) != 0U;
    const auto authority          = std::make_unique<CapabilityAuthority>(config);
    const u8   revocation         = plan.byte(24U) % 7U;
    const u64  revocation_expires = revocation == 2U ? kT0 + 1U + plan.word(25U) % 4000U : kT0 + 90000U;
    if (revocation != 0U)
    {
        const KeyId    listed = chain[plan.byte(27U) % (depth + 1U)];
        RevocationList list;
        list.generation = 1U;
        list.issued     = kT0 - 3600U;
        list.expires    = revocation_expires;
        list.tokens = revocation == 3U ? ConstSpan<CapabilityId>(&root.token, 1U) : ConstSpan<CapabilityId>();
        list.keys   = revocation == 4U ? ConstSpan<KeyId>(&listed, 1U) : ConstSpan<KeyId>();
        list.revoke_before         = revocation == 5U ? kT0 + 1U : 0U;
        list.min_policy_generation = revocation == 6U ? 2U : 0U;
        static u8 generation[kMaxRevocationBytes];
        usize     generation_size = 0U;
        CRD_FUZZ_REQUIRE(issue_revocations(list, k.signers, generation, generation_size) == CapabilityError::None);
        CRD_FUZZ_REQUIRE(authority->apply_revocations(ConstSpan<u8>(generation, generation_size), k.policy, kT0) ==
                         CapabilityError::None);
    }
    const u64 now = kT0 - 400U + plan.word(28U) % (lifetime + 800U);

    // The presentation: honest, under another session's binding, with a flipped proof byte, or by another peer.
    const u8 shown = plan.byte(30U) % 4U;
    u8       session[kCapabilityBindingBytes];
    std::memset(session, 0xC3, sizeof(session));
    u8    proof[kMaxCapabilityProofBytes];
    usize proof_size = 0U;
    if (binding == CapabilityBinding::Proof)
    {
        u8 proven_for[kCapabilityBindingBytes];
        std::memcpy(proven_for, session, sizeof(session));
        proven_for[0] ^= shown == 1U ? 0x01U : 0x00U;
        CRD_FUZZ_REQUIRE(prove_capability(ConstSpan<u8>(token, size), proven_for, k.holders[holder], proof,
                                          proof_size) == CapabilityError::None);
        if (shown == 2U)
        {
            proof[plan.word(31U) % proof_size] ^= 0x01U;
        }
    }
    const u32              peer = shown == 3U ? (holder + 1U) % 4U : holder;
    CapabilityPresentation presentation;
    presentation.binding        = session;
    presentation.proof          = ConstSpan<u8>(proof, proof_size);
    presentation.peer_key       = k.holder_ids[peer];
    presentation.peer_algorithm = k.holders[peer].algorithm();

    // The model of the verification order.
    const u64  max_life = config.max_lifetime == 0U ? 86400U : config.max_lifetime;
    const bool revoked  = (revocation >= 3U && revocation <= 5U) || (revocation == 6U && root.policy_generation < 2U);
    const bool unbound  = binding == CapabilityBinding::Proof ? shown == 1U || shown == 2U : shown == 3U;
    CapabilityError want = CapabilityError::None;
    if (lifetime > max_life)
    {
        want = CapabilityError::TooLong;
    }
    else if (kT0 > now + kMaxIssueSkew)
    {
        want = CapabilityError::NotYetValid;
    }
    else if (now >= expires)
    {
        want = CapabilityError::Expired;
    }
    else if (config.peer_only && binding != CapabilityBinding::Peer)
    {
        want = CapabilityError::PeerRequired;
    }
    else if (revocation == 0U || now >= revocation_expires)
    {
        want = CapabilityError::RevocationStale;
    }
    else if (revoked)
    {
        want = CapabilityError::Revoked;
    }
    else if (unbound)
    {
        want = CapabilityError::Unbound;
    }
    Capability            granted;
    const CapabilityError got = authority->verify(ConstSpan<u8>(token, size), presentation, k.policy, now, granted);
    CRD_FUZZ_REQUIRE(got == want);
    if (got == CapabilityError::None)
    {
        CRD_FUZZ_REQUIRE(granted.depth == depth && granted.expires == expires && granted.holder.key == chain[depth]);
        CRD_FUZZ_REQUIRE(grant_within(granted.grant, root.grant) && grant_within(granted.grant, grant));
        // Every byte of the token is signed, a signature or a signer's key: one flipped bit never verifies.
        const u16 flip = plan.word(33U);
        if (flip != 0U)
        {
            token[flip % size] ^= static_cast<u8>(1U << (flip % 8U));
            CRD_FUZZ_REQUIRE(authority->verify(ConstSpan<u8>(token, size), presentation, k.policy, now, granted) !=
                             CapabilityError::None);
        }
    }
    return got == CapabilityError::None;
}

// ---- The generated schema life ------------------------------------------------------------------------------

// 0 kind | 1 operations (8 bytes) | 9 idempotent (8 bytes) | 17 choices (bit 0 idempotent unmasked, bit 1 another
// audience) | 18 issue offset word | 20 revocation | 21 policy generation | 22 signers | 23 time word | 25 operations
// asked (8 bytes) | 33 object word | 35 delivery | 36 capability choices (bit 0 asked unmasked, bit 1 another
// audience, bit 2 another kind, bit 3 every object) | 37 granted operations (8 bytes) | 45 range length | 46 flip word
constexpr usize kSchemaLife = 48U;

u64 plan_u64(const Plan& plan, usize at)
{
    u64 value = 0U;
    for (usize i = 0U; i < 8U; ++i)
    {
        value = (value << 8U) | plan.byte(at + i);
    }
    return value;
}

OperationVerdict restated_verdict(const MessageSchema& schema, const Capability& capability, u64 asked, u64 object,
                                  Delivery delivery)
{
    if (std::memcmp(capability.audience.bytes, schema.audience.bytes, 16U) != 0 ||
        capability.grant.kind != schema.kind)
    {
        return OperationVerdict::OtherSchema;
    }
    if (asked == 0U || (asked | schema.operations) != schema.operations)
    {
        return OperationVerdict::Undeclared;
    }
    if (delivery != Delivery::Established && (asked | schema.idempotent) != schema.idempotent)
    {
        return OperationVerdict::Replayable;
    }
    const bool inside = capability.grant.range_count == 0U ||
                        (capability.grant.ranges[0].first <= object && object <= capability.grant.ranges[0].last);
    return (asked | capability.grant.operations) == capability.grant.operations && inside
               ? OperationVerdict::Admitted
               : OperationVerdict::NotGranted;
}

bool schema_life(const Plan& plan)
{
    const Keys&   k = keys();
    MessageSchema schema;
    schema.kind              = static_cast<ScopeKind>(plan.byte(0U) % 8U);
    schema.schema.bytes[0]   = 0x5CU;
    schema.major             = 1U;
    schema.audience.bytes[0] = (plan.byte(17U) & 2U) != 0U ? 0xA1U : 0xA0U;
    schema.issued            = kT0 - 3600U + plan.word(18U) % 8000U;
    schema.policy_generation = 1U + plan.byte(21U) % 3U;
    schema.operations        = plan_u64(plan, 1U);
    schema.idempotent        = plan_u64(plan, 9U) & ((plan.byte(17U) & 1U) != 0U ? ~u64{0U} : schema.operations);
    const u8      signers    = plan.byte(22U) % 3U;
    const Signer* one[]      = {k.signers[signers == 1U ? 0U : 1U]};
    const ConstSpan<const Signer*> signed_by =
        signers == 0U ? ConstSpan<const Signer*>(k.signers) : ConstSpan<const Signer*>(one);
    static u8             bytes[kMaxSchemaBytes];
    usize                 size   = 0U;
    const bool            valid  = schema.kind >= ScopeKind::World && schema.kind <= ScopeKind::Service &&
                                   schema.operations != 0U &&
                                   (schema.idempotent | schema.operations) == schema.operations;
    const CapabilityError issued = issue_schema(schema, signed_by, bytes, size);
    CRD_FUZZ_REQUIRE(issued == (valid ? CapabilityError::None : CapabilityError::BadField));
    if (!valid)
    {
        return false;
    }

    static const ProviderVerifier kVerifier;
    CapabilityConfig              config;
    config.issuers                = ConstSpan<IssuerKey>(k.issuers);
    config.verifier               = &kVerifier;
    config.kind                   = ScopeKind::World;
    config.audience.bytes[0]      = 0xA0U;
    const auto authority          = std::make_unique<CapabilityAuthority>(config);
    const u8   revocation         = plan.byte(20U) % 4U;
    if (revocation != 0U)
    {
        RevocationList list;
        list.generation            = 1U;
        list.issued                = kT0 - 3600U;
        list.expires               = kT0 + 90000U;
        list.revoke_before         = revocation == 2U ? kT0 + 1U : 0U;
        list.min_policy_generation = revocation == 3U ? 2U : 0U;
        static u8 generation[kMaxRevocationBytes];
        usize     generation_size = 0U;
        CRD_FUZZ_REQUIRE(issue_revocations(list, k.signers, generation, generation_size) == CapabilityError::None);
        CRD_FUZZ_REQUIRE(authority->apply_revocations(ConstSpan<u8>(generation, generation_size), k.policy, kT0) ==
                         CapabilityError::None);
    }
    const u64 now = kT0 + plan.word(23U) % 4000U;

    // The model of the verification order.
    CapabilityError want = CapabilityError::None;
    if (schema.kind != ScopeKind::World || schema.audience.bytes[0] != 0xA0U)
    {
        want = CapabilityError::WrongAudience;
    }
    else if (schema.issued > now + kMaxIssueSkew)
    {
        want = CapabilityError::NotYetValid;
    }
    else if (revocation == 0U)
    {
        want = CapabilityError::RevocationStale;
    }
    else if ((revocation == 2U && schema.issued < kT0 + 1U) || (revocation == 3U && schema.policy_generation < 2U))
    {
        want = CapabilityError::Revoked;
    }
    else if (signers != 0U)
    {
        want = CapabilityError::MissingSignature;
    }
    AuthorizedSchema      authorized;
    const CapabilityError got = authority->verify_schema(ConstSpan<u8>(bytes, size), k.policy, now, authorized);
    CRD_FUZZ_REQUIRE(got == want);
    if (got != CapabilityError::None)
    {
        return false;
    }
    u8 digest[kSha256Bytes];
    sha256(ConstSpan<u8>(bytes, 78U), Span<u8, kSha256Bytes>(digest, kSha256Bytes));
    CRD_FUZZ_REQUIRE(std::memcmp(digest, authorized.digest, kSha256Bytes) == 0);
    CRD_FUZZ_REQUIRE(authorized.schema.operations == schema.operations &&
                     authorized.schema.idempotent == schema.idempotent);

    // An operation asked of a capability, as authorize_operation and the restated rule judge it.
    const u8   choices = plan.byte(36U);
    Capability capability;
    capability.audience.bytes[0] = (choices & 2U) != 0U ? 0xA2U : 0xA0U;
    capability.grant.kind        = (choices & 4U) != 0U ? ScopeKind::Project : ScopeKind::World;
    capability.grant.operations  = plan_u64(plan, 37U);
    if ((choices & 8U) == 0U)
    {
        capability.grant.ranges[0]   = {100U, 100U + plan.byte(45U)};
        capability.grant.range_count = 1U;
    }
    const u64          asked        = plan_u64(plan, 25U) & ((choices & 1U) != 0U ? ~u64{0U} : schema.operations);
    const u64          object       = plan.word(33U) % 512U;
    constexpr Delivery deliveries[] = {Delivery::Established, Delivery::Early, Delivery{}}; // the last one unknown
    const Delivery     delivery     = deliveries[plan.byte(35U) % 3U];
    CRD_FUZZ_REQUIRE(authorize_operation(authorized, capability, asked, object, delivery) ==
                     restated_verdict(schema, capability, asked, object, delivery));
    CRD_FUZZ_REQUIRE(authorized.early(asked) == (asked != 0U && (asked | schema.idempotent) == schema.idempotent));

    // Every byte is signed or a signature: one flipped bit never verifies.
    const u16 flip = plan.word(46U);
    if (flip != 0U)
    {
        bytes[flip % size] ^= static_cast<u8>(1U << (flip % 8U));
        CRD_FUZZ_REQUIRE(authority->verify_schema(ConstSpan<u8>(bytes, size), k.policy, now, authorized) !=
                         CapabilityError::None);
    }
    return true;
}

bool run(const u8* data, usize size)
{
    if (size == 0U)
    {
        return false;
    }
    switch (data[0] % 5U)
    {
    case 0U:
        return decode_token(data + 1U, size - 1U);
    case 1U:
        return decode_generation(data + 1U, size - 1U);
    case 2U:
        return life(Plan{data + 1U, size - 1U});
    case 3U:
        return decode_schema_bytes(data + 1U, size - 1U);
    default:
        return schema_life(Plan{data + 1U, size - 1U});
    }
}

void add(crd::fuzz::SeedSink& sink, const char* name, const u8* data, usize size)
{
    sink.add(name, data, size);
}

constexpr const char* kAccepted[] = {
    "token-honest",
    "token-delegated",
    "revocations-honest",
    "life-honest",
    "life-delegated-peer",
    "life-widened-refused",
    "life-flipped",
    "schema-honest",
    "schema-life-honest",
    "schema-life-early",
    "schema-life-flipped",
    "schema-life-not-granted",
    "schema-life-replayable",
    "schema-life-undeclared",
    "schema-life-other-capability",
};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    static_cast<void>(run(data, size));
    return 0;
}

// Honest tokens and generations decode and their damaged copies do not; honest lives verify and the refused ones
// do not (VerdictSink checks each seed).
void crd_fuzz_seeds(crd::fuzz::SeedSink& next)
{
    crd::fuzz::VerdictSink sink(next, run, kAccepted, sizeof(kAccepted) / sizeof(kAccepted[0]));
    const Keys&            k = keys();
    static u8              input[1U + kMaxCapabilityBytes];

    CapabilityRoot root;
    root.token.bytes[0]                 = 0x01U;
    root.issued                         = kT0;
    root.expires                        = kT0 + 3600U;
    root.audience.bytes[0]              = 0xA0U;
    root.subject                        = {PrincipalKind::User, k.holder_ids[0]};
    root.holder                         = {AlgorithmId::Ed25519, k.holder_ids[0], CapabilityBinding::Proof};
    root.grant.kind                     = ScopeKind::World;
    root.grant.ranges[0]                = {10U, 20U};
    root.grant.range_count              = 1U;
    root.grant.operations               = 0xFFU;
    root.grant.budget.operations_per_second = 10U;
    usize size = 0U;
    input[0]   = 0U;
    CRD_FUZZ_REQUIRE(issue_capability(root, k.signers, Span<u8>(input + 1U, kMaxCapabilityBytes), size) ==
                     CapabilityError::None);
    add(sink, "token-honest", input, 1U + size);
    add(sink, "token-truncated", input, size);
    input[1U + size] = 0U;
    add(sink, "token-trailing-byte", input, 2U + size);
    input[1] ^= 0x20U;
    add(sink, "token-bad-magic", input, 1U + size);
    input[1] ^= 0x20U;
    input[9] = 2U;
    add(sink, "token-future-format", input, 1U + size);
    input[9]   = 1U;
    input[151] = 17U; // the grant's range count
    add(sink, "token-too-many-ranges", input, 1U + size);
    input[151] = 1U;
    input[1U + 9U + 24U] ^= 0x7FU; // the expiry's first byte: a lifetime of years
    add(sink, "token-bad-dates", input, 1U + size);
    input[1U + 9U + 24U] ^= 0x7FU;

    CapabilityAttenuation block;
    block.expires                 = kT0 + 1800U;
    block.holder                  = {AlgorithmId::Ed25519, k.holder_ids[1], CapabilityBinding::Proof};
    block.grant                   = root.grant;
    block.grant.ranges[0]         = {12U, 14U};
    static u8 delegated[kMaxCapabilityBytes];
    usize     delegated_size = 0U;
    CRD_FUZZ_REQUIRE(attenuate_capability(ConstSpan<u8>(input + 1U, size), block, k.holders[0], delegated,
                                          delegated_size) == CapabilityError::None);
    std::memcpy(input + 1U, delegated, delegated_size);
    add(sink, "token-delegated", input, 1U + delegated_size);
    input[delegated_size - 64U - 2U + 1U] = 0U; // the block's signature length: none
    input[delegated_size - 64U - 1U + 1U] = 0U;
    add(sink, "token-delegated-empty-signature", input, 1U + delegated_size);

    RevocationList list;
    list.generation          = 3U;
    list.issued              = kT0;
    list.expires             = kT0 + 86400U;
    const CapabilityId ids[] = {CapabilityId{}, root.token};
    list.tokens              = ids;
    input[0]                 = 1U;
    CRD_FUZZ_REQUIRE(issue_revocations(list, k.signers, Span<u8>(input + 1U, kMaxCapabilityBytes), size) ==
                     CapabilityError::None);
    add(sink, "revocations-honest", input, 1U + size);
    add(sink, "revocations-truncated", input, size);
    input[1U + size] = 0U;
    add(sink, "revocations-trailing-byte", input, 2U + size);
    std::memcpy(input + 1U + 51U, root.token.bytes, 16U); // the first id equal to the second
    add(sink, "revocations-unsorted", input, 1U + size);

    // Lives: honest (an hour, one range [10, 14], a fresh revocation generation, an honest proof at kT0 + 60); one
    // delegated to an ECDSA holder bound to its peer and presented by it; one refusing a widened delegation; one
    // flipped after verifying; one presented after its expiry; one revoked by its token; one under another session's
    // binding; one Proof token at a peer-only audience.
    static u8  life_input[1U + kLifeHeader + kAttemptBytes];
    const auto plan = [&](const char* name, usize attempts, auto&& edit) {
        std::memset(life_input, 0, sizeof(life_input));
        u8* h         = life_input + 1U;
        life_input[0] = 2U;
        h[0]          = 0x0DU; // 3,540 + 60 seconds
        h[1]          = 0xD4U;
        h[3]          = 5U;
        h[5]          = 1U;
        h[6]          = 9U; // [10, 14]
        h[7]          = 4U;
        h[14]         = 0x0FU;
        h[15]         = 0xF0U;
        h[16]         = 7U;
        h[18]         = 1U;
        h[19]         = 9U;
        h[20]         = static_cast<u8>(attempts);
        h[24]         = 1U;
        h[28]         = 0x01U; // kT0 - 400 + 460
        h[29]         = 0xCCU;
        edit(h);
        add(sink, name, life_input, 1U + kLifeHeader + attempts * kAttemptBytes);
    };
    plan("life-honest", 0U, [](u8*) {});
    plan("life-delegated-peer", 1U, [](u8* h) {
        u8* a = h + kLifeHeader;
        a[1]  = 2U;    // the ECDSA holder
        a[2]  = 1U;    // bound to its peer
        a[5]  = 0x0FU; // operations 0x0F00
        a[7]  = 1U;
        a[8]  = 1U;
        a[11] = 1U; // [11, 13]
        a[12] = 2U;
    });
    plan("life-widened-refused", 1U, [](u8* h) { h[kLifeHeader] = 2U; });
    plan("life-flipped", 0U, [](u8* h) {
        h[33] = 0x01U;
        h[34] = 0x23U;
    });
    plan("life-late", 0U, [](u8* h) {
        h[28] = 0x10U; // kT0 - 400 + 4,100: past the expiry
        h[29] = 0x04U;
    });
    plan("life-revoked-token", 0U, [](u8* h) { h[24] = 3U; });
    plan("life-other-binding", 0U, [](u8* h) { h[30] = 1U; });
    plan("life-peer-only", 0U, [](u8* h) { h[21] = 2U; });

    // Schemas: an honest one decodes, its damaged copies do not.
    MessageSchema schema;
    schema.kind              = ScopeKind::World;
    schema.schema.bytes[0]   = 0x5CU;
    schema.audience.bytes[0] = 0xA0U;
    schema.issued            = kT0;
    schema.operations        = 0x1FU;
    schema.idempotent        = 0x0AU;
    input[0]                 = 3U;
    CRD_FUZZ_REQUIRE(issue_schema(schema, k.signers, Span<u8>(input + 1U, kMaxSchemaBytes), size) ==
                     CapabilityError::None);
    add(sink, "schema-honest", input, 1U + size);
    add(sink, "schema-truncated", input, size);
    input[1U + size] = 0U;
    add(sink, "schema-trailing-byte", input, 2U + size);
    input[1U + 9U] = 7U; // the kind
    add(sink, "schema-bad-kind", input, 1U + size);
    input[1U + 9U]  = 1U;
    input[1U + 70U] = 0x80U; // bit 63 idempotent, never declared
    add(sink, "schema-undeclared-idempotent", input, 1U + size);
    input[1U + 70U] = 0U;
    input[1U + 78U] = 5U; // five signatures
    add(sink, "schema-too-many-signatures", input, 1U + size);

    // Schema lives: honest (operations 0x1F, 0x0A idempotent, issued at kT0, a fresh revocation generation, verified
    // at kT0 + 60, operation 0x01 on object 101 after the handshake, granted 0xFF on [100, 110]); idempotent
    // operations as early data; a flipped byte; an operation outside the grant; a replayable one as early data; an
    // undeclared one; a capability of another audience; then refused schemas: another audience, issued ahead, revoked
    // by the cut-off, signed by one issuer, invalid.
    static u8  schema_input[1U + kSchemaLife];
    const auto schema_plan = [&](const char* name, auto&& edit) {
        std::memset(schema_input, 0, sizeof(schema_input));
        u8* h           = schema_input + 1U;
        schema_input[0] = 4U;
        h[0]            = 1U;
        h[8]            = 0x1FU;
        h[16]           = 0x0AU;
        h[18]           = 0x0EU; // kT0 - 3600 + 3600
        h[19]           = 0x10U;
        h[20]           = 1U;
        h[24]           = 60U;
        h[32]           = 0x01U;
        h[34]           = 101U;
        h[44]           = 0xFFU;
        h[45]           = 10U;
        edit(h);
        add(sink, name, schema_input, sizeof(schema_input));
    };
    schema_plan("schema-life-honest", [](u8*) {});
    schema_plan("schema-life-early", [](u8* h) {
        h[32] = 0x0AU;
        h[35] = 1U;
    });
    schema_plan("schema-life-flipped", [](u8* h) {
        h[46] = 0x01U;
        h[47] = 0x11U;
    });
    schema_plan("schema-life-not-granted", [](u8* h) { h[34] = 0xFFU; });
    schema_plan("schema-life-replayable", [](u8* h) { h[35] = 2U; });
    schema_plan("schema-life-undeclared", [](u8* h) {
        h[36] = 1U; // asked unmasked: bit 8, which the schema does not declare
        h[31] = 0x01U;
    });
    schema_plan("schema-life-other-capability", [](u8* h) { h[36] = 2U; });
    schema_plan("schema-life-other-audience", [](u8* h) { h[17] = 2U; });
    schema_plan("schema-life-ahead", [](u8* h) {
        h[18] = 0x1FU; // issued kT0 + 4,399
        h[19] = 0x3FU;
    });
    schema_plan("schema-life-revoked", [](u8* h) { h[20] = 2U; });
    schema_plan("schema-life-one-signer", [](u8* h) { h[22] = 1U; });
    schema_plan("schema-life-invalid", [](u8* h) { h[17] = 1U; h[16] = 0xFFU; });
}
