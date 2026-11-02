// crd-crypto -- capabilities (NET.2). Layout, every integer big-endian:
//
//   holder      algorithm u16 | key id 32 bytes | binding u8
//   grant       kind u8 | scope 16 bytes | range count u8 | count x (first u64, last u64) | operations u64
//               | operations per second u32 | bytes per second u64 | objects u32
//   root        magic "CRDCAPTK" | format u8 | token 16 bytes | issued u64 | expires u64 | policy generation u64
//               | audience 16 bytes | subject kind u8 | subject key id 32 bytes | holder | grant
//   token       root | signature list (signed_artifact.hpp) over the root
//               | 0 to kMaxAttenuations x (expires u64 | holder | grant
//                                         | signer key length u16 | signer key | signature length u16 | signature)
//   proof       key length u16 | key | signature length u16 | signature, the signature over
//               "CRDCAPPR" | session binding 32 bytes | SHA-256 of the token
//   revocation  magic "CRDREVOK" | format u8 | generation u64 | issued u64 | expires u64 | revoke before u64
//               | minimum policy generation u64 | token count u16 | count x 16 bytes | key count u16
//               | count x 32 bytes | signature list over everything before it
//   schema      magic "CRDSCHEM" | format u8 | kind u8 | schema 16 bytes | major u16 | minor u16 | audience 16 bytes
//               | issued u64 | policy generation u64 | operations u64 | idempotent u64
//               | signature list over everything before it
//
// A block's signature covers the token from its first byte to the block's grant. Contract and rationale:
// docs/design/network-foundation.md#capability-tokens.
#include <crd/crypto/capability.hpp>

#include "byte_spans.hpp"
#include "signed_artifact.hpp"

#include <crd/containers/wire_bytes.hpp>

#include <algorithm>
#include <cstring>

namespace crd::crypto
{
namespace
{
using detail::overlaps;
using signed_artifact::saturating_add;

constexpr usize kHolderBytes     = 2U + kKeyIdBytes + 1U;
constexpr usize kGrantFixedBytes = 1U + kCapabilityIdBytes + 1U + 8U + 4U + 8U + 4U;
constexpr usize kRangeBytes      = 16U;
constexpr usize kRootFixedBytes  = sizeof(kCapabilityMagic) + 1U + kCapabilityIdBytes + 8U + 8U + 8U +
                                  kCapabilityIdBytes + 1U + kKeyIdBytes + kHolderBytes;
constexpr usize kBlockFixedBytes = 8U + kHolderBytes;
constexpr usize kProofBytes      = sizeof(kCapabilityProofMagic) + kCapabilityBindingBytes + kSha256Bytes;
constexpr usize kRevocationFixedBytes = sizeof(kRevocationMagic) + 1U + 5U * 8U + 2U + 2U;
constexpr usize kSchemaBodyBytes =
    sizeof(kSchemaMagic) + 1U + 1U + kCapabilityIdBytes + 2U + 2U + kCapabilityIdBytes + 4U * 8U;
constexpr usize kLongestList = 1U + kMaxCapabilitySignatures * (signed_artifact::kEntryHeadBytes + kMaxSignatureBytes);

// A root with every signature at its bound, and a revocation generation with every list full, fit their bounds: only
// attenuation can reach kMaxCapabilityBytes.
static_assert(kRootFixedBytes + kGrantFixedBytes + kMaxCapabilityRanges * kRangeBytes + kLongestList <=
              kMaxCapabilityBytes);
static_assert(kRevocationFixedBytes + kMaxRevokedTokens * kCapabilityIdBytes + kMaxRevokedKeys * kKeyIdBytes +
                  kLongestList <=
              kMaxRevocationBytes);
static_assert(kSchemaBodyBytes + kLongestList <= kMaxSchemaBytes);
static_assert(sizeof(CapabilityId) == kCapabilityIdBytes && sizeof(KeyId) == kKeyIdBytes);

bool is_zero(const CapabilityId& id) noexcept
{
    return id == CapabilityId{};
}

CapabilityError capability_error_of(signed_artifact::Error error) noexcept
{
    switch (error)
    {
    case signed_artifact::Error::None:
        return CapabilityError::None;
    case signed_artifact::Error::Truncated:
        return CapabilityError::Truncated;
    case signed_artifact::Error::TooLarge:
        return CapabilityError::TooLarge;
    case signed_artifact::Error::Unsigned:
        return CapabilityError::Unsigned;
    case signed_artifact::Error::BadEntry:
        return CapabilityError::BadSignatureEntry;
    case signed_artifact::Error::Duplicate:
        return CapabilityError::DuplicateSignature;
    }
    return CapabilityError::BadSignatureEntry;
}

CapabilityError capability_error_of(signed_artifact::Trust trust) noexcept
{
    switch (trust)
    {
    case signed_artifact::Trust::Valid:
        return CapabilityError::None;
    case signed_artifact::Trust::Missing:
        return CapabilityError::MissingSignature;
    case signed_artifact::Trust::Unsupported:
        return CapabilityError::UnsupportedSignature;
    case signed_artifact::Trust::Mismatch:
    case signed_artifact::Trust::Invalid:
        break;
    }
    return CapabilityError::BadSignature;
}

// ---- Fields ----

bool valid_kind(ScopeKind kind) noexcept
{
    return static_cast<u8>(kind) >= static_cast<u8>(ScopeKind::World) &&
           static_cast<u8>(kind) <= static_cast<u8>(ScopeKind::Service);
}

bool valid_holder(const CapabilityHolder& holder) noexcept
{
    return signed_artifact::is_signature(holder.algorithm) &&
           (holder.binding == CapabilityBinding::Proof || holder.binding == CapabilityBinding::Peer);
}

CapabilityError validate_grant(const Grant& grant) noexcept
{
    if (grant.range_count > kMaxCapabilityRanges)
    {
        return CapabilityError::TooLarge;
    }
    if (!valid_kind(grant.kind))
    {
        return CapabilityError::BadField;
    }
    for (u32 i = 0U; i < grant.range_count; ++i)
    {
        const ObjectRange& range = grant.ranges[i];
        if (range.first > range.last)
        {
            return CapabilityError::BadField;
        }
        // Apart from the previous range by at least one object: the canonical form, so containment is per range.
        if (i > 0U && (grant.ranges[i - 1U].last == kNever || grant.ranges[i - 1U].last + 1U >= range.first))
        {
            return CapabilityError::BadField;
        }
    }
    return CapabilityError::None;
}

CapabilityError validate_attenuation(const CapabilityAttenuation& block) noexcept
{
    const CapabilityError grant = validate_grant(block.grant);
    if (grant != CapabilityError::None)
    {
        return grant;
    }
    return valid_holder(block.holder) ? CapabilityError::None : CapabilityError::BadField;
}

usize grant_size(const Grant& grant) noexcept
{
    return kGrantFixedBytes + grant.range_count * kRangeBytes;
}

void put_id(cont::WireWriter& writer, const CapabilityId& id) noexcept
{
    writer.put_bytes(cont::ConstSpan<u8>(id.bytes, kCapabilityIdBytes));
}

void put_key(cont::WireWriter& writer, const KeyId& key) noexcept
{
    writer.put_bytes(cont::ConstSpan<u8>(key.bytes, kKeyIdBytes));
}

void put_holder(cont::WireWriter& writer, const CapabilityHolder& holder) noexcept
{
    writer.put_u16(static_cast<u16>(holder.algorithm));
    put_key(writer, holder.key);
    writer.put_u8(static_cast<u8>(holder.binding));
}

void put_grant(cont::WireWriter& writer, const Grant& grant) noexcept
{
    writer.put_u8(static_cast<u8>(grant.kind));
    put_id(writer, grant.scope);
    writer.put_u8(static_cast<u8>(grant.range_count));
    for (u32 i = 0U; i < grant.range_count; ++i)
    {
        writer.put_u64(grant.ranges[i].first);
        writer.put_u64(grant.ranges[i].last);
    }
    writer.put_u64(grant.operations);
    writer.put_u32(grant.budget.operations_per_second);
    writer.put_u64(grant.budget.bytes_per_second);
    writer.put_u32(grant.budget.objects);
}

bool take_copy(cont::WireReader& reader, cont::Span<u8> out) noexcept
{
    cont::ConstSpan<u8> bytes;
    if (!reader.take_bytes(out.size(), bytes))
    {
        return false;
    }
    std::memcpy(out.data(), bytes.data(), out.size());
    return true;
}

bool take_holder(cont::WireReader& reader, CapabilityHolder& holder) noexcept
{
    u16 algorithm = 0U;
    u8  binding   = 0U;
    if (!reader.take_u16(algorithm) || !take_copy(reader, holder.key.bytes) || !reader.take_u8(binding))
    {
        return false;
    }
    holder.algorithm = static_cast<AlgorithmId>(algorithm);
    holder.binding   = static_cast<CapabilityBinding>(binding);
    return true;
}

// Truncated, then TooLarge for a range count above the bound, as the fields come.
CapabilityError take_grant(cont::WireReader& reader, Grant& grant) noexcept
{
    u8 kind   = 0U;
    u8 ranges = 0U;
    if (!reader.take_u8(kind) || !take_copy(reader, grant.scope.bytes) || !reader.take_u8(ranges))
    {
        return CapabilityError::Truncated;
    }
    grant.kind = static_cast<ScopeKind>(kind);
    if (ranges > kMaxCapabilityRanges)
    {
        return CapabilityError::TooLarge;
    }
    grant.range_count = ranges;
    for (u32 i = 0U; i < grant.range_count; ++i)
    {
        if (!reader.take_u64(grant.ranges[i].first) || !reader.take_u64(grant.ranges[i].last))
        {
            return CapabilityError::Truncated;
        }
    }
    if (!reader.take_u64(grant.operations) || !reader.take_u32(grant.budget.operations_per_second) ||
        !reader.take_u64(grant.budget.bytes_per_second) || !reader.take_u32(grant.budget.objects))
    {
        return CapabilityError::Truncated;
    }
    return CapabilityError::None;
}

// A u16 length and its bytes, 1 to `bound` of them: Truncated, BadField for none, TooLarge above the bound.
CapabilityError take_sized(cont::WireReader& reader, usize bound, CapabilityError empty,
                           cont::ConstSpan<u8>& out) noexcept
{
    u16 length = 0U;
    if (!reader.take_u16(length))
    {
        return CapabilityError::Truncated;
    }
    if (length == 0U)
    {
        return empty;
    }
    if (length > bound)
    {
        return CapabilityError::TooLarge;
    }
    return reader.take_bytes(length, out) ? CapabilityError::None : CapabilityError::Truncated;
}

bool is_magic(cont::ConstSpan<u8> bytes, const u8 (&magic)[8]) noexcept
{
    return std::memcmp(bytes.data(), magic, sizeof(magic)) == 0;
}

// The magic and the format byte: Truncated, BadMagic, UnsupportedFormat.
CapabilityError take_head(cont::WireReader& reader, const u8 (&magic)[8], u8& format) noexcept
{
    cont::ConstSpan<u8> bytes;
    if (!reader.take_bytes(sizeof(magic), bytes))
    {
        return CapabilityError::Truncated;
    }
    if (!is_magic(bytes, magic))
    {
        return CapabilityError::BadMagic;
    }
    if (!reader.take_u8(format))
    {
        return CapabilityError::Truncated;
    }
    return format < kCapabilityFormatOldest || format > kCapabilityFormat ? CapabilityError::UnsupportedFormat
                                                                         : CapabilityError::None;
}

// ---- Signing ----

// The signer's public key and identifier; BadConfig for a signer that is not a signature key.
CapabilityError signer_key(const Signer& signer, u8 (&key)[kMaxHolderKeyBytes], usize& size, KeyId& id) noexcept
{
    size = signature_sizes(signer.algorithm()).public_key;
    if (!signed_artifact::is_signature(signer.algorithm()) || size == 0U || size > kMaxHolderKeyBytes)
    {
        return CapabilityError::BadConfig;
    }
    if (signer.public_key(cont::Span<u8>(key, size)) != CryptoError::None)
    {
        return CapabilityError::ProviderFailure;
    }
    id = key_id_of(cont::ConstSpan<u8>(key, size));
    return CapabilityError::None;
}

// Appends the signature list of `issuers` over out[0, signed_end) to `out` from signed_end.
CapabilityError append_issuer_signatures(cont::ConstSpan<const Signer*> issuers, cont::Span<u8> out, usize signed_end,
                                         usize& written) noexcept
{
    if (issuers.empty() || issuers.size() > kMaxCapabilitySignatures)
    {
        return CapabilityError::BadConfig;
    }
    KeyId ids[kMaxCapabilitySignatures];
    for (usize i = 0U; i < issuers.size(); ++i)
    {
        if (issuers[i] == nullptr)
        {
            return CapabilityError::BadConfig;
        }
        u8                    key[kMaxHolderKeyBytes];
        usize                 key_size = 0U;
        const CapabilityError found    = signer_key(*issuers[i], key, key_size, ids[i]);
        if (found != CapabilityError::None)
        {
            return found;
        }
        for (usize j = 0U; j < i; ++j)
        {
            if (ids[j] == ids[i])
            {
                return CapabilityError::BadConfig;
            }
        }
    }
    if (out.size() < signed_end + 1U)
    {
        return CapabilityError::BufferTooSmall;
    }
    usize at = signed_end;
    out[at++] = static_cast<u8>(issuers.size());
    for (usize i = 0U; i < issuers.size(); ++i)
    {
        const usize longest = signature_sizes(issuers[i]->algorithm()).signature;
        if (out.size() < at + signed_artifact::kEntryHeadBytes + longest)
        {
            return CapabilityError::BufferTooSmall;
        }
        std::memcpy(out.data() + at, ids[i].bytes, kKeyIdBytes);
        cont::WireWriter head(out.subspan(at + kKeyIdBytes, 2U));
        head.put_u16(static_cast<u16>(issuers[i]->algorithm()));
        usize length = 0U;
        if (issuers[i]->sign(out.first(signed_end), out.subspan(at + signed_artifact::kEntryHeadBytes, longest),
                             length) != CryptoError::None)
        {
            return CapabilityError::ProviderFailure;
        }
        cont::WireWriter size(out.subspan(at + kKeyIdBytes + 2U, 2U));
        size.put_u16(static_cast<u16>(length));
        at += signed_artifact::kEntryHeadBytes + length;
    }
    written = at;
    return CapabilityError::None;
}

// ---- Verification ----

// The issuer keys whose windows hold `time`; UntrustedIssuer unless they are 1 to kMaxCapabilitySignatures keys with
// at least one post-quantum and one traditional algorithm.
CapabilityError required_issuers(const CapabilityConfig& config, u64 time, TrustKey (&out)[kMaxIssuerKeys],
                                 usize& count) noexcept
{
    count             = 0U;
    bool post_quantum = false;
    bool traditional  = false;
    for (const IssuerKey& issuer : config.issuers)
    {
        if (issuer.issues_from <= time && time < issuer.issues_until)
        {
            const AlgorithmClass type = algorithm_class(issuer.key.algorithm);
            out[count++]              = issuer.key;
            post_quantum              = post_quantum || type == AlgorithmClass::PostQuantumSignature;
            traditional               = traditional || type == AlgorithmClass::ClassicalSignature;
        }
    }
    const bool hybrid = post_quantum && traditional && count <= kMaxCapabilitySignatures;
    return hybrid ? CapabilityError::None : CapabilityError::UntrustedIssuer;
}

bool refused(const PolicySnapshot& policy, AlgorithmId algorithm, u64 now) noexcept
{
    return policy.status(algorithm, now) == AlgorithmStatus::Refused;
}

bool issuers_refused(cont::ConstSpan<TrustKey> required, const PolicySnapshot& policy, u64 now) noexcept
{
    return std::ranges::any_of(required, [&](const TrustKey& key) { return refused(policy, key.algorithm, now); });
}

CapabilityError verification_error(Verification verification) noexcept
{
    switch (verification)
    {
    case Verification::Valid:
        return CapabilityError::None;
    case Verification::Unsupported:
        return CapabilityError::UnsupportedSignature;
    case Verification::Invalid:
        break;
    }
    return CapabilityError::BadSignature;
}

// The previous holder signed each block, with the key it was named by.
CapabilityError verify_chain(const CapabilityToken& token, const SignatureVerifier& verifier) noexcept
{
    const CapabilityHolder* previous = &token.root.holder;
    for (u32 i = 0U; i < token.block_count; ++i)
    {
        const CapabilityBlock& block = token.blocks[i];
        if (key_id_of(block.signer_public) != previous->key)
        {
            return CapabilityError::WrongHolder;
        }
        const CapabilityError verified = verification_error(
            verifier.verify(previous->algorithm, block.signer_public, block.signed_bytes, block.signature));
        if (verified != CapabilityError::None)
        {
            return verified;
        }
        previous = &block.content.holder;
    }
    return CapabilityError::None;
}

void proof_message(cont::ConstSpan<u8> token, cont::ConstSpan<u8> binding, u8 (&out)[kProofBytes]) noexcept
{
    std::memcpy(out, kCapabilityProofMagic, sizeof(kCapabilityProofMagic));
    std::memcpy(out + sizeof(kCapabilityProofMagic), binding.data(), kCapabilityBindingBytes);
    sha256(token, cont::Span<u8, kSha256Bytes>(out + sizeof(kCapabilityProofMagic) + kCapabilityBindingBytes,
                                               kSha256Bytes));
}

CapabilityError verify_presentation(cont::ConstSpan<u8> token, const CapabilityHolder& holder,
                                    const CapabilityPresentation& presentation,
                                    const SignatureVerifier& verifier) noexcept
{
    if (holder.binding == CapabilityBinding::Peer)
    {
        const bool peer = presentation.peer_key == holder.key && presentation.peer_algorithm == holder.algorithm;
        return peer ? CapabilityError::None : CapabilityError::Unbound;
    }
    if (presentation.binding.size() != kCapabilityBindingBytes || presentation.proof.size() > kMaxCapabilityProofBytes)
    {
        return CapabilityError::Unbound;
    }
    cont::WireReader    reader(presentation.proof);
    cont::ConstSpan<u8> key;
    cont::ConstSpan<u8> signature;
    if (take_sized(reader, kMaxHolderKeyBytes, CapabilityError::BadField, key) != CapabilityError::None ||
        take_sized(reader, kMaxSignatureBytes, CapabilityError::BadField, signature) != CapabilityError::None ||
        !reader.done() || key_id_of(key) != holder.key)
    {
        return CapabilityError::Unbound;
    }
    u8 message[kProofBytes];
    proof_message(token, presentation.binding, message);
    switch (verifier.verify(holder.algorithm, key, message, signature))
    {
    case Verification::Valid:
        return CapabilityError::None;
    case Verification::Unsupported:
        return CapabilityError::UnsupportedSignature;
    case Verification::Invalid:
        break;
    }
    return CapabilityError::Unbound;
}

bool ascending(cont::ConstSpan<u8> entries, usize size) noexcept
{
    for (usize at = size; at < entries.size(); at += size)
    {
        if (std::memcmp(entries.data() + at - size, entries.data() + at, size) >= 0)
        {
            return false;
        }
    }
    return true;
}

template <typename Entry> bool listed(const Entry* entries, u32 count, const u8* bytes, usize size) noexcept
{
    u32 low  = 0U;
    u32 high = count;
    while (low < high)
    {
        const u32 middle = low + (high - low) / 2U;
        const int order  = std::memcmp(entries[middle].bytes, bytes, size);
        if (order == 0)
        {
            return true;
        }
        if (order < 0)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }
    return false;
}
} // namespace

cont::StringView capability_binding_label(ScopeKind kind) noexcept
{
    // RFC 5705 section 4 leaves labels beginning with "EXPERIMENTAL" to private use without registration.
    switch (kind)
    {
    case ScopeKind::World:
        return "EXPERIMENTAL Cerid capability world";
    case ScopeKind::Project:
        return "EXPERIMENTAL Cerid capability project";
    case ScopeKind::Room:
        return "EXPERIMENTAL Cerid capability room";
    case ScopeKind::Upload:
        return "EXPERIMENTAL Cerid capability upload";
    case ScopeKind::Tool:
        return "EXPERIMENTAL Cerid capability tool";
    case ScopeKind::Service:
        return "EXPERIMENTAL Cerid capability service";
    }
    return {};
}

bool grant_within(const Grant& narrower, const Grant& wider) noexcept
{
    const CapabilityBudget& a = narrower.budget;
    const CapabilityBudget& b = wider.budget;
    if (narrower.kind != wider.kind || narrower.scope != wider.scope ||
        (narrower.operations & ~wider.operations) != 0U || a.operations_per_second > b.operations_per_second ||
        a.bytes_per_second > b.bytes_per_second || a.objects > b.objects)
    {
        return false;
    }
    if (wider.range_count == 0U)
    {
        return true;
    }
    if (narrower.range_count == 0U)
    {
        return false; // every object, where the wider grant names some
    }
    for (u32 i = 0U; i < narrower.range_count; ++i)
    {
        const ObjectRange& range  = narrower.ranges[i];
        bool               inside = false;
        for (u32 j = 0U; j < wider.range_count && !inside; ++j)
        {
            inside = wider.ranges[j].first <= range.first && range.last <= wider.ranges[j].last;
        }
        if (!inside)
        {
            return false;
        }
    }
    return true;
}

const CapabilityHolder& CapabilityToken::holder() const noexcept
{
    return block_count == 0U ? root.holder : blocks[block_count - 1U].content.holder;
}

bool Capability::covers(u64 operations, u64 object) const noexcept
{
    if (operations == 0U || (operations & ~grant.operations) != 0U)
    {
        return false;
    }
    if (grant.range_count == 0U)
    {
        return true;
    }
    for (u32 i = 0U; i < grant.range_count; ++i)
    {
        if (grant.ranges[i].first <= object && object <= grant.ranges[i].last)
        {
            return true;
        }
    }
    return false;
}

cont::StringView capability_error_name(CapabilityError error) noexcept
{
    switch (error)
    {
    case CapabilityError::None:
        return "none";
    case CapabilityError::BadConfig:
        return "bad-config";
    case CapabilityError::BufferTooSmall:
        return "buffer-too-small";
    case CapabilityError::TooLarge:
        return "too-large";
    case CapabilityError::Truncated:
        return "truncated";
    case CapabilityError::TrailingBytes:
        return "trailing-bytes";
    case CapabilityError::BadMagic:
        return "bad-magic";
    case CapabilityError::UnsupportedFormat:
        return "unsupported-format";
    case CapabilityError::BadField:
        return "bad-field";
    case CapabilityError::BadDates:
        return "bad-dates";
    case CapabilityError::BadList:
        return "bad-list";
    case CapabilityError::Unsigned:
        return "unsigned";
    case CapabilityError::BadSignatureEntry:
        return "bad-signature-entry";
    case CapabilityError::DuplicateSignature:
        return "duplicate-signature";
    case CapabilityError::WrongHolder:
        return "wrong-holder";
    case CapabilityError::Widened:
        return "widened";
    case CapabilityError::WrongAudience:
        return "wrong-audience";
    case CapabilityError::TooLong:
        return "too-long";
    case CapabilityError::NotYetValid:
        return "not-yet-valid";
    case CapabilityError::Expired:
        return "expired";
    case CapabilityError::PeerRequired:
        return "peer-required";
    case CapabilityError::RevocationStale:
        return "revocation-stale";
    case CapabilityError::Revoked:
        return "revoked";
    case CapabilityError::UntrustedIssuer:
        return "untrusted-issuer";
    case CapabilityError::AlgorithmRefused:
        return "algorithm-refused";
    case CapabilityError::MissingSignature:
        return "missing-signature";
    case CapabilityError::UnsupportedSignature:
        return "unsupported-signature";
    case CapabilityError::BadSignature:
        return "bad-signature";
    case CapabilityError::Unbound:
        return "unbound";
    case CapabilityError::Rollback:
        return "rollback";
    case CapabilityError::Conflict:
        return "conflict";
    case CapabilityError::AlreadyCurrent:
        return "already-current";
    case CapabilityError::NotYetIssued:
        return "not-yet-issued";
    case CapabilityError::IssuedBeforeCurrent:
        return "issued-before-current";
    case CapabilityError::ProviderFailure:
        return "provider-failure";
    }
    return "unknown";
}

cont::StringView operation_verdict_name(OperationVerdict verdict) noexcept
{
    switch (verdict)
    {
    case OperationVerdict::Admitted:
        return "admitted";
    case OperationVerdict::OtherSchema:
        return "other-schema";
    case OperationVerdict::Undeclared:
        return "undeclared";
    case OperationVerdict::Replayable:
        return "replayable";
    case OperationVerdict::NotGranted:
        return "not-granted";
    }
    return "unknown";
}

bool AuthorizedSchema::early(u64 operations) const noexcept
{
    return operations != 0U && (operations & ~schema.idempotent) == 0U;
}

OperationVerdict authorize_operation(const AuthorizedSchema& schema, const Capability& capability, u64 operations,
                                     u64 object, Delivery delivery) noexcept
{
    if (capability.audience != schema.schema.audience || capability.grant.kind != schema.schema.kind)
    {
        return OperationVerdict::OtherSchema;
    }
    if (operations == 0U || (operations & ~schema.schema.operations) != 0U)
    {
        return OperationVerdict::Undeclared;
    }
    // Anything but a delivery after the handshake is judged as replayable.
    if (delivery != Delivery::Established && !schema.early(operations))
    {
        return OperationVerdict::Replayable;
    }
    return capability.covers(operations, object) ? OperationVerdict::Admitted : OperationVerdict::NotGranted;
}

CapabilityError validate_schema(const MessageSchema& schema) noexcept
{
    const bool declared = schema.operations != 0U && (schema.idempotent & ~schema.operations) == 0U;
    return valid_kind(schema.kind) && !is_zero(schema.schema) && !is_zero(schema.audience) && declared
               ? CapabilityError::None
               : CapabilityError::BadField;
}

CapabilityError issue_schema(const MessageSchema& schema, cont::ConstSpan<const Signer*> issuers, cont::Span<u8> out,
                             usize& written) noexcept
{
    written                     = 0U;
    const CapabilityError valid = validate_schema(schema);
    if (valid != CapabilityError::None)
    {
        return valid;
    }
    if (out.size() < kSchemaBodyBytes)
    {
        return CapabilityError::BufferTooSmall;
    }
    cont::WireWriter writer(out);
    writer.put_bytes(kSchemaMagic);
    writer.put_u8(kCapabilityFormat);
    writer.put_u8(static_cast<u8>(schema.kind));
    put_id(writer, schema.schema);
    writer.put_u16(schema.major);
    writer.put_u16(schema.minor);
    put_id(writer, schema.audience);
    writer.put_u64(schema.issued);
    writer.put_u64(schema.policy_generation);
    writer.put_u64(schema.operations);
    writer.put_u64(schema.idempotent);
    return append_issuer_signatures(issuers, out, kSchemaBodyBytes, written);
}

CapabilityError decode_schema(cont::ConstSpan<u8> bytes, SchemaMessage& out) noexcept
{
    if (bytes.size() > kMaxSchemaBytes)
    {
        return CapabilityError::TooLarge;
    }
    cont::WireReader      reader(bytes);
    const CapabilityError head = take_head(reader, kSchemaMagic, out.format);
    if (head != CapabilityError::None)
    {
        return head;
    }
    MessageSchema& schema = out.schema;
    u8             kind   = 0U;
    if (!reader.take_u8(kind) || !take_copy(reader, schema.schema.bytes) || !reader.take_u16(schema.major) ||
        !reader.take_u16(schema.minor) || !take_copy(reader, schema.audience.bytes) ||
        !reader.take_u64(schema.issued) || !reader.take_u64(schema.policy_generation) ||
        !reader.take_u64(schema.operations) || !reader.take_u64(schema.idempotent))
    {
        return CapabilityError::Truncated;
    }
    schema.kind                 = static_cast<ScopeKind>(kind);
    const CapabilityError valid = validate_schema(schema);
    if (valid != CapabilityError::None)
    {
        return valid;
    }
    out.signed_bytes           = bytes.first(reader.consumed());
    const CapabilityError list = capability_error_of(signed_artifact::read(
        reader, cont::Span<KeySignature>(out.signatures, kMaxCapabilitySignatures), out.signature_count));
    if (list != CapabilityError::None)
    {
        return list;
    }
    if (!reader.done())
    {
        return CapabilityError::TrailingBytes;
    }
    return capability_error_of(signed_artifact::validate(
        cont::ConstSpan<KeySignature>(out.signatures, out.signature_count), kMaxCapabilitySignatures));
}

CapabilityError validate_root(const CapabilityRoot& root) noexcept
{
    const CapabilityError grant = validate_grant(root.grant);
    if (grant != CapabilityError::None)
    {
        return grant;
    }
    const u8 subject = static_cast<u8>(root.subject.kind);
    if (is_zero(root.token) || is_zero(root.audience) || subject < static_cast<u8>(PrincipalKind::User) ||
        subject > static_cast<u8>(PrincipalKind::Agent) || !valid_holder(root.holder))
    {
        return CapabilityError::BadField;
    }
    const bool dated = root.expires > root.issued && root.expires - root.issued <= kMaxCapabilityLifetime;
    return dated ? CapabilityError::None : CapabilityError::BadDates;
}

CapabilityError issue_capability(const CapabilityRoot& root, cont::ConstSpan<const Signer*> issuers,
                                 cont::Span<u8> out, usize& written) noexcept
{
    written                    = 0U;
    const CapabilityError good = validate_root(root);
    if (good != CapabilityError::None)
    {
        return good;
    }
    const usize body = kRootFixedBytes + grant_size(root.grant);
    if (out.size() < body)
    {
        return CapabilityError::BufferTooSmall;
    }
    cont::WireWriter writer(out);
    writer.put_bytes(kCapabilityMagic);
    writer.put_u8(kCapabilityFormat);
    put_id(writer, root.token);
    writer.put_u64(root.issued);
    writer.put_u64(root.expires);
    writer.put_u64(root.policy_generation);
    put_id(writer, root.audience);
    writer.put_u8(static_cast<u8>(root.subject.kind));
    put_key(writer, root.subject.key);
    put_holder(writer, root.holder);
    put_grant(writer, root.grant);
    return append_issuer_signatures(issuers, out, body, written);
}

CapabilityError decode_capability(cont::ConstSpan<u8> bytes, CapabilityToken& out) noexcept
{
    if (bytes.size() > kMaxCapabilityBytes)
    {
        return CapabilityError::TooLarge;
    }
    cont::WireReader      reader(bytes);
    const CapabilityError head = take_head(reader, kCapabilityMagic, out.format);
    if (head != CapabilityError::None)
    {
        return head;
    }
    CapabilityRoot& root    = out.root;
    u8              subject = 0U;
    if (!take_copy(reader, root.token.bytes) || !reader.take_u64(root.issued) || !reader.take_u64(root.expires) ||
        !reader.take_u64(root.policy_generation) || !take_copy(reader, root.audience.bytes) ||
        !reader.take_u8(subject) || !take_copy(reader, root.subject.key.bytes) || !take_holder(reader, root.holder))
    {
        return CapabilityError::Truncated;
    }
    root.subject.kind                = static_cast<PrincipalKind>(subject);
    const CapabilityError root_grant = take_grant(reader, root.grant);
    if (root_grant != CapabilityError::None)
    {
        return root_grant;
    }
    const CapabilityError valid = validate_root(root);
    if (valid != CapabilityError::None)
    {
        return valid;
    }
    out.root_bytes = bytes.first(reader.consumed());
    const CapabilityError list = capability_error_of(signed_artifact::read(
        reader, cont::Span<KeySignature>(out.signatures, kMaxCapabilitySignatures), out.signature_count));
    if (list != CapabilityError::None)
    {
        return list;
    }
    const CapabilityError entries = capability_error_of(signed_artifact::validate(
        cont::ConstSpan<KeySignature>(out.signatures, out.signature_count), kMaxCapabilitySignatures));
    if (entries != CapabilityError::None)
    {
        return entries;
    }
    out.block_count = 0U;
    while (!reader.done())
    {
        if (out.block_count == kMaxAttenuations)
        {
            return CapabilityError::TooLarge;
        }
        CapabilityBlock&       block   = out.blocks[out.block_count];
        CapabilityAttenuation& content = block.content;
        if (!reader.take_u64(content.expires) || !take_holder(reader, content.holder))
        {
            return CapabilityError::Truncated;
        }
        const CapabilityError grant = take_grant(reader, content.grant);
        if (grant != CapabilityError::None)
        {
            return grant;
        }
        const CapabilityError attenuation = validate_attenuation(content);
        if (attenuation != CapabilityError::None)
        {
            return attenuation;
        }
        block.signed_bytes        = bytes.first(reader.consumed());
        const CapabilityError key =
            take_sized(reader, kMaxHolderKeyBytes, CapabilityError::BadField, block.signer_public);
        if (key != CapabilityError::None)
        {
            return key;
        }
        const CapabilityError signature =
            take_sized(reader, kMaxSignatureBytes, CapabilityError::BadSignatureEntry, block.signature);
        if (signature != CapabilityError::None)
        {
            return signature;
        }
        ++out.block_count;
    }
    return CapabilityError::None;
}

CapabilityError attenuate_capability(cont::ConstSpan<u8> token, const CapabilityAttenuation& block,
                                     const Signer& holder, cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    CapabilityToken       decoded;
    const CapabilityError parsed = decode_capability(token, decoded);
    if (parsed != CapabilityError::None)
    {
        return parsed;
    }
    const CapabilityError valid = validate_attenuation(block);
    if (valid != CapabilityError::None)
    {
        return valid;
    }
    u8                    key[kMaxHolderKeyBytes];
    usize                 key_size = 0U;
    KeyId                 id;
    const CapabilityError found = signer_key(holder, key, key_size, id);
    if (found != CapabilityError::None)
    {
        return found;
    }
    const CapabilityHolder& current = decoded.holder();
    if (holder.algorithm() != current.algorithm || id != current.key)
    {
        return CapabilityError::WrongHolder;
    }
    const u64    expires = decoded.block_count == 0U ? decoded.root.expires
                                                     : decoded.blocks[decoded.block_count - 1U].content.expires;
    const Grant& grant   = decoded.block_count == 0U ? decoded.root.grant
                                                     : decoded.blocks[decoded.block_count - 1U].content.grant;
    const bool loosened = current.binding == CapabilityBinding::Peer && block.holder.binding != CapabilityBinding::Peer;
    if (block.expires > expires || !grant_within(block.grant, grant) || loosened)
    {
        return CapabilityError::Widened;
    }
    if (decoded.block_count == kMaxAttenuations)
    {
        return CapabilityError::TooLarge;
    }
    if (overlaps(token, out))
    {
        return CapabilityError::BadConfig;
    }
    const usize longest  = signature_sizes(holder.algorithm()).signature;
    const usize body_end = token.size() + kBlockFixedBytes + grant_size(block.grant);
    if (body_end + 4U + key_size + longest > kMaxCapabilityBytes)
    {
        return CapabilityError::TooLarge; // judged by the longest signature, so the answer never depends on one
    }
    if (body_end + 4U + key_size + longest > out.size())
    {
        return CapabilityError::BufferTooSmall;
    }
    std::memcpy(out.data(), token.data(), token.size());
    cont::WireWriter writer(out.subspan(token.size()));
    writer.put_u64(block.expires);
    put_holder(writer, block.holder);
    put_grant(writer, block.grant);
    writer.put_u16(static_cast<u16>(key_size));
    writer.put_bytes(cont::ConstSpan<u8>(key, key_size));
    const usize at     = body_end + 2U + key_size;
    usize       length = 0U;
    if (holder.sign(out.first(body_end), out.subspan(at + 2U, longest), length) != CryptoError::None)
    {
        return CapabilityError::ProviderFailure;
    }
    cont::WireWriter size(out.subspan(at, 2U));
    size.put_u16(static_cast<u16>(length));
    written = at + 2U + length;
    return CapabilityError::None;
}

TlsError capability_binding(const detail::TlsHandshakeState& session, ScopeKind kind, cont::Span<u8> out) noexcept
{
    const cont::StringView label = capability_binding_label(kind);
    if (label.empty() || out.size() != kCapabilityBindingBytes)
    {
        secure_zero(out);
        return TlsError::BadConfig;
    }
    return session.export_keying_material(label, {}, out);
}

CapabilityError prove_capability(cont::ConstSpan<u8> token, cont::ConstSpan<u8> binding, const Signer& holder,
                                 cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    CapabilityToken       decoded;
    const CapabilityError parsed = decode_capability(token, decoded);
    if (parsed != CapabilityError::None)
    {
        return parsed;
    }
    if (binding.size() != kCapabilityBindingBytes || decoded.holder().binding != CapabilityBinding::Proof)
    {
        return CapabilityError::BadConfig;
    }
    u8                    key[kMaxHolderKeyBytes];
    usize                 key_size = 0U;
    KeyId                 id;
    const CapabilityError found = signer_key(holder, key, key_size, id);
    if (found != CapabilityError::None)
    {
        return found;
    }
    if (holder.algorithm() != decoded.holder().algorithm || id != decoded.holder().key)
    {
        return CapabilityError::WrongHolder;
    }
    const usize longest = signature_sizes(holder.algorithm()).signature;
    if (out.size() < 4U + key_size + longest)
    {
        return CapabilityError::BufferTooSmall;
    }
    u8 message[kProofBytes];
    proof_message(token, binding, message);
    cont::WireWriter writer(out);
    writer.put_u16(static_cast<u16>(key_size));
    writer.put_bytes(cont::ConstSpan<u8>(key, key_size));
    usize length = 0U;
    if (holder.sign(message, out.subspan(4U + key_size, longest), length) != CryptoError::None)
    {
        return CapabilityError::ProviderFailure;
    }
    writer.put_u16(static_cast<u16>(length));
    written = 4U + key_size + length;
    return CapabilityError::None;
}

CapabilityError validate_config(const CapabilityConfig& config) noexcept
{
    if (config.verifier == nullptr || config.issuers.empty() || config.issuers.size() > kMaxIssuerKeys ||
        !valid_kind(config.kind) || is_zero(config.audience) || config.max_lifetime > kMaxCapabilityLifetime)
    {
        return CapabilityError::BadConfig;
    }
    for (usize i = 0U; i < config.issuers.size(); ++i)
    {
        const IssuerKey& issuer = config.issuers[i];
        if (!signed_artifact::is_signature(issuer.key.algorithm) || issuer.key.public_key.empty() ||
            issuer.issues_from >= issuer.issues_until)
        {
            return CapabilityError::BadConfig;
        }
        for (usize j = 0U; j < i; ++j)
        {
            if (config.issuers[j].key.id == issuer.key.id)
            {
                return CapabilityError::BadConfig;
            }
        }
    }
    return CapabilityError::None;
}

CapabilityError issue_revocations(const RevocationList& list, cont::ConstSpan<const Signer*> issuers,
                                  cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    if (list.generation == 0U)
    {
        return CapabilityError::BadField;
    }
    if (list.expires <= list.issued || list.expires - list.issued > kMaxRevocationLifetime)
    {
        return CapabilityError::BadDates;
    }
    if (list.tokens.size() > kMaxRevokedTokens || list.keys.size() > kMaxRevokedKeys)
    {
        return CapabilityError::TooLarge;
    }
    // The entries' object representations, contiguous (the static_assert above): their wire form.
    const cont::ConstSpan<u8> tokens(reinterpret_cast<const u8*>(list.tokens.data()),
                                     list.tokens.size() * kCapabilityIdBytes);
    const cont::ConstSpan<u8> keys(reinterpret_cast<const u8*>(list.keys.data()), list.keys.size() * kKeyIdBytes);
    if (!ascending(tokens, kCapabilityIdBytes) || !ascending(keys, kKeyIdBytes))
    {
        return CapabilityError::BadList;
    }
    const usize body = kRevocationFixedBytes + tokens.size() + keys.size();
    if (out.size() < body)
    {
        return CapabilityError::BufferTooSmall;
    }
    cont::WireWriter writer(out);
    writer.put_bytes(kRevocationMagic);
    writer.put_u8(kCapabilityFormat);
    writer.put_u64(list.generation);
    writer.put_u64(list.issued);
    writer.put_u64(list.expires);
    writer.put_u64(list.revoke_before);
    writer.put_u64(list.min_policy_generation);
    writer.put_u16(static_cast<u16>(list.tokens.size()));
    writer.put_bytes(tokens);
    writer.put_u16(static_cast<u16>(list.keys.size()));
    writer.put_bytes(keys);
    return append_issuer_signatures(issuers, out, body, written);
}

CapabilityError decode_revocations(cont::ConstSpan<u8> bytes, RevocationMessage& out) noexcept
{
    if (bytes.size() > kMaxRevocationBytes)
    {
        return CapabilityError::TooLarge;
    }
    cont::WireReader      reader(bytes);
    const CapabilityError head = take_head(reader, kRevocationMagic, out.format);
    if (head != CapabilityError::None)
    {
        return head;
    }
    if (!reader.take_u64(out.generation) || !reader.take_u64(out.issued) || !reader.take_u64(out.expires) ||
        !reader.take_u64(out.revoke_before) || !reader.take_u64(out.min_policy_generation))
    {
        return CapabilityError::Truncated;
    }
    if (out.generation == 0U)
    {
        return CapabilityError::BadField;
    }
    if (out.expires <= out.issued || out.expires - out.issued > kMaxRevocationLifetime)
    {
        return CapabilityError::BadDates;
    }
    u16 tokens = 0U;
    if (!reader.take_u16(tokens))
    {
        return CapabilityError::Truncated;
    }
    if (tokens > kMaxRevokedTokens)
    {
        return CapabilityError::TooLarge;
    }
    out.token_count = tokens;
    if (!reader.take_bytes(usize{tokens} * kCapabilityIdBytes, out.tokens))
    {
        return CapabilityError::Truncated;
    }
    u16 keys = 0U;
    if (!reader.take_u16(keys))
    {
        return CapabilityError::Truncated;
    }
    if (keys > kMaxRevokedKeys)
    {
        return CapabilityError::TooLarge;
    }
    out.key_count = keys;
    if (!reader.take_bytes(usize{keys} * kKeyIdBytes, out.keys))
    {
        return CapabilityError::Truncated;
    }
    if (!ascending(out.tokens, kCapabilityIdBytes) || !ascending(out.keys, kKeyIdBytes))
    {
        return CapabilityError::BadList;
    }
    out.signed_bytes           = bytes.first(reader.consumed());
    const CapabilityError list = capability_error_of(signed_artifact::read(
        reader, cont::Span<KeySignature>(out.signatures, kMaxCapabilitySignatures), out.signature_count));
    if (list != CapabilityError::None)
    {
        return list;
    }
    if (!reader.done())
    {
        return CapabilityError::TrailingBytes;
    }
    return capability_error_of(signed_artifact::validate(
        cont::ConstSpan<KeySignature>(out.signatures, out.signature_count), kMaxCapabilitySignatures));
}

CapabilityAuthority::CapabilityAuthority(const CapabilityConfig& config) noexcept
    : m_config(config), m_floor(config.revocation_floor)
{
}

u64 CapabilityAuthority::revocation_floor() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_floor;
}

CapabilityError CapabilityAuthority::apply_revocations(cont::ConstSpan<u8> generation, const PolicySnapshot& policy,
                                                       u64 now) noexcept
{
    if (validate_config(m_config) != CapabilityError::None)
    {
        return CapabilityError::BadConfig;
    }
    RevocationMessage     message;
    const CapabilityError decoded = decode_revocations(generation, message);
    if (decoded != CapabilityError::None)
    {
        return decoded;
    }
    if (message.generation < revocation_floor())
    {
        return CapabilityError::Rollback;
    }
    TrustKey              required[kMaxIssuerKeys];
    usize                 count  = 0U;
    const CapabilityError issuer = required_issuers(m_config, message.issued, required, count);
    if (issuer != CapabilityError::None)
    {
        return issuer;
    }
    const cont::ConstSpan<TrustKey> keys(required, count);
    if (issuers_refused(keys, policy, now))
    {
        return CapabilityError::AlgorithmRefused;
    }
    const CapabilityError verified = capability_error_of(signed_artifact::verify_all(
        keys, *m_config.verifier, message.signed_bytes,
        cont::ConstSpan<KeySignature>(message.signatures, message.signature_count)));
    if (verified != CapabilityError::None)
    {
        return verified;
    }

    const std::lock_guard<std::mutex> lock(m_mutex);
    if (message.generation < m_floor)
    {
        return CapabilityError::Rollback; // a newer generation landed while this one was verified
    }
    if (m_generation != 0U && message.generation == m_generation)
    {
        const bool same =
            message.issued == m_issued && message.expires == m_expires && message.revoke_before == m_revoke_before &&
            message.min_policy_generation == m_min_policy_generation && message.token_count == m_token_count &&
            message.key_count == m_key_count &&
            (m_token_count == 0U || std::memcmp(message.tokens.data(), m_tokens, message.tokens.size()) == 0) &&
            (m_key_count == 0U || std::memcmp(message.keys.data(), m_keys, message.keys.size()) == 0);
        return same ? CapabilityError::AlreadyCurrent : CapabilityError::Conflict;
    }
    if (message.issued > saturating_add(now, kMaxIssueSkew))
    {
        return CapabilityError::NotYetIssued;
    }
    if (now >= message.expires)
    {
        return CapabilityError::Expired;
    }
    if (m_generation != 0U && message.issued < m_issued)
    {
        return CapabilityError::IssuedBeforeCurrent;
    }
    m_generation            = message.generation;
    m_issued                = message.issued;
    m_expires               = message.expires;
    m_revoke_before         = message.revoke_before;
    m_min_policy_generation = message.min_policy_generation;
    m_token_count           = message.token_count;
    m_key_count             = message.key_count;
    if (!message.tokens.empty())
    {
        std::memcpy(m_tokens, message.tokens.data(), message.tokens.size());
    }
    if (!message.keys.empty())
    {
        std::memcpy(m_keys, message.keys.data(), message.keys.size());
    }
    m_floor = message.generation;
    return CapabilityError::None;
}

CapabilityError CapabilityAuthority::check_generation(u64 issued, u64 policy_generation, u64 now) const noexcept
{
    if (m_generation == 0U || now >= m_expires)
    {
        return CapabilityError::RevocationStale;
    }
    return issued < m_revoke_before || policy_generation < m_min_policy_generation ? CapabilityError::Revoked
                                                                                    : CapabilityError::None;
}

CapabilityError CapabilityAuthority::check_revoked(const CapabilityToken& token, u64 now) const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    const CapabilityRoot&             root       = token.root;
    const CapabilityError             generation = check_generation(root.issued, root.policy_generation, now);
    if (generation != CapabilityError::None)
    {
        return generation;
    }
    if (listed(m_tokens, m_token_count, root.token.bytes, kCapabilityIdBytes) ||
        listed(m_keys, m_key_count, root.subject.key.bytes, kKeyIdBytes) ||
        listed(m_keys, m_key_count, root.holder.key.bytes, kKeyIdBytes))
    {
        return CapabilityError::Revoked;
    }
    for (u32 i = 0U; i < token.block_count; ++i)
    {
        if (listed(m_keys, m_key_count, token.blocks[i].content.holder.key.bytes, kKeyIdBytes))
        {
            return CapabilityError::Revoked;
        }
    }
    return CapabilityError::None;
}

CapabilityError CapabilityAuthority::verify(cont::ConstSpan<u8> token, const CapabilityPresentation& presentation,
                                            const PolicySnapshot& policy, u64 now, Capability& out) const noexcept
{
    if (validate_config(m_config) != CapabilityError::None)
    {
        return CapabilityError::BadConfig;
    }
    CapabilityToken       decoded;
    const CapabilityError parsed = decode_capability(token, decoded);
    if (parsed != CapabilityError::None)
    {
        return parsed;
    }
    const CapabilityRoot& root = decoded.root;
    if (root.grant.kind != m_config.kind || root.audience != m_config.audience)
    {
        return CapabilityError::WrongAudience;
    }
    const u64 max_lifetime = m_config.max_lifetime == 0U ? kMaxCapabilityLifetime : m_config.max_lifetime;
    if (root.expires - root.issued > max_lifetime)
    {
        return CapabilityError::TooLong;
    }
    u64                     expires = root.expires;
    const Grant*            grant   = &root.grant;
    const CapabilityHolder* holder  = &root.holder;
    for (u32 i = 0U; i < decoded.block_count; ++i)
    {
        const CapabilityAttenuation& block    = decoded.blocks[i].content;
        const bool                   loosened = holder->binding == CapabilityBinding::Peer &&
                                                block.holder.binding != CapabilityBinding::Peer;
        if (block.expires > expires || !grant_within(block.grant, *grant) || loosened)
        {
            return CapabilityError::Widened;
        }
        expires = block.expires;
        grant   = &block.grant;
        holder  = &block.holder;
    }
    if (root.issued > saturating_add(now, kMaxIssueSkew))
    {
        return CapabilityError::NotYetValid;
    }
    if (now >= expires)
    {
        return CapabilityError::Expired;
    }
    if (m_config.peer_only && holder->binding != CapabilityBinding::Peer)
    {
        return CapabilityError::PeerRequired;
    }
    const CapabilityError revoked = check_revoked(decoded, now);
    if (revoked != CapabilityError::None)
    {
        return revoked;
    }
    TrustKey              required[kMaxIssuerKeys];
    usize                 count  = 0U;
    const CapabilityError issuer = required_issuers(m_config, root.issued, required, count);
    if (issuer != CapabilityError::None)
    {
        return issuer;
    }
    const cont::ConstSpan<TrustKey> keys(required, count);
    bool                            algorithm_refused = issuers_refused(keys, policy, now) ||
                                                        refused(policy, root.holder.algorithm, now);
    for (u32 i = 0U; i < decoded.block_count; ++i)
    {
        algorithm_refused = algorithm_refused || refused(policy, decoded.blocks[i].content.holder.algorithm, now);
    }
    if (algorithm_refused)
    {
        return CapabilityError::AlgorithmRefused;
    }
    const CapabilityError issued = capability_error_of(signed_artifact::verify_all(
        keys, *m_config.verifier, decoded.root_bytes,
        cont::ConstSpan<KeySignature>(decoded.signatures, decoded.signature_count)));
    if (issued != CapabilityError::None)
    {
        return issued;
    }
    const CapabilityError chain = verify_chain(decoded, *m_config.verifier);
    if (chain != CapabilityError::None)
    {
        return chain;
    }
    const CapabilityError bound = verify_presentation(token, *holder, presentation, *m_config.verifier);
    if (bound != CapabilityError::None)
    {
        return bound;
    }
    out.token             = root.token;
    out.audience          = root.audience;
    out.subject           = root.subject;
    out.holder            = *holder;
    out.grant             = *grant;
    out.issued            = root.issued;
    out.expires           = expires;
    out.policy_generation = root.policy_generation;
    out.depth             = decoded.block_count;
    return CapabilityError::None;
}

CapabilityError CapabilityAuthority::verify(cont::ConstSpan<u8> token, const detail::TlsHandshakeState& session,
                                            cont::ConstSpan<u8> proof, const PolicySnapshot& policy, u64 now,
                                            Capability& out) const noexcept
{
    // A session outside Established presents nothing: no binding, no peer.
    u8                     binding[kCapabilityBindingBytes] = {};
    CapabilityPresentation presentation;
    presentation.proof = proof;
    if (capability_binding(session, m_config.kind, binding) == TlsError::None)
    {
        presentation.binding        = binding;
        presentation.peer_key       = session.peer_key();
        presentation.peer_algorithm = session.peer_algorithm();
    }
    return verify(token, presentation, policy, now, out);
}

CapabilityError CapabilityAuthority::verify_early(cont::ConstSpan<u8> token, const detail::TlsHandshakeState& session,
                                                  const PolicySnapshot& policy, u64 now,
                                                  Capability& out) const noexcept
{
    // Only the peer a resumed ticket's handshake proved: no binding, so a Proof token is Unbound.
    CapabilityPresentation presentation;
    if (session.early_data() == TlsEarlyData::Accepted && session.resumed() && session.peer_key() != KeyId{})
    {
        presentation.peer_key       = session.peer_key();
        presentation.peer_algorithm = session.peer_algorithm();
    }
    return verify(token, presentation, policy, now, out);
}

CapabilityError CapabilityAuthority::verify_schema(cont::ConstSpan<u8> schema, const PolicySnapshot& policy, u64 now,
                                                   AuthorizedSchema& out) const noexcept
{
    if (validate_config(m_config) != CapabilityError::None)
    {
        return CapabilityError::BadConfig;
    }
    SchemaMessage         decoded;
    const CapabilityError parsed = decode_schema(schema, decoded);
    if (parsed != CapabilityError::None)
    {
        return parsed;
    }
    const MessageSchema& content = decoded.schema;
    if (content.kind != m_config.kind || content.audience != m_config.audience)
    {
        return CapabilityError::WrongAudience;
    }
    if (content.issued > saturating_add(now, kMaxIssueSkew))
    {
        return CapabilityError::NotYetValid;
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const CapabilityError             revoked = check_generation(content.issued, content.policy_generation, now);
        if (revoked != CapabilityError::None)
        {
            return revoked;
        }
    }
    TrustKey              required[kMaxIssuerKeys];
    usize                 count  = 0U;
    const CapabilityError issuer = required_issuers(m_config, content.issued, required, count);
    if (issuer != CapabilityError::None)
    {
        return issuer;
    }
    const cont::ConstSpan<TrustKey> keys(required, count);
    if (issuers_refused(keys, policy, now))
    {
        return CapabilityError::AlgorithmRefused;
    }
    const CapabilityError signed_by = capability_error_of(signed_artifact::verify_all(
        keys, *m_config.verifier, decoded.signed_bytes,
        cont::ConstSpan<KeySignature>(decoded.signatures, decoded.signature_count)));
    if (signed_by != CapabilityError::None)
    {
        return signed_by;
    }
    out.schema = content;
    sha256(decoded.signed_bytes, cont::Span<u8, kSha256Bytes>(out.digest, kSha256Bytes));
    return CapabilityError::None;
}
} // namespace crd::crypto
