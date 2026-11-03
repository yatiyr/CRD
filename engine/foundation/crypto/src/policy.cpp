// crd-crypto -- signed crypto policy generations (NET-STD). Layout, every integer big-endian:
//
//   body        magic "CRDPOLCY" | format u8 | generation u64 | issued u64 | expires u64 | offline grace u32
//               | minimum count u8 | count x (protocol u16, major u16, minor u16)
//               | rule count u8 | count x (algorithm u16, scope u8, accepted from u64, preferred from u64,
//                                          preferred until u64, refused from u64)
//               (format 1, read only: no scope byte, every rule an Everywhere rule)
//   signatures  count u8 | count x (key id 32 bytes, algorithm u16, length u16, length bytes)
//
// Every signature covers the body. Contract and rationale: docs/design/network-foundation.md#policy-wire.
#include <crd/crypto/policy.hpp>

#include "signed_artifact.hpp"

#include <crd/containers/wire_bytes.hpp>

#include <algorithm>

namespace crd::crypto
{
namespace
{
using signed_artifact::saturating_add;

constexpr usize kHeadBytes    = sizeof(kPolicyMagic) + 1U + 8U + 8U + 8U + 4U;
constexpr usize kMinimumBytes = 6U;
constexpr usize kRuleBytes    = 35U;

bool known_scope(PolicyScope scope) noexcept
{
    return scope == PolicyScope::Everywhere || scope == PolicyScope::CeridServices;
}

PolicyError validate_rule(const AlgorithmRule& rule) noexcept
{
    const bool ordered = rule.accepted_from <= rule.preferred_from && rule.preferred_from <= rule.preferred_until &&
                         rule.preferred_until <= rule.refused_from && rule.accepted_from < rule.refused_from;
    return static_cast<u16>(rule.algorithm) != 0U && ordered ? PolicyError::None : PolicyError::BadRule;
}

PolicyError read_body(cont::WireReader& reader, PolicyMessage& out) noexcept
{
    cont::ConstSpan<u8> magic;
    if (!reader.take_bytes(sizeof(kPolicyMagic), magic))
    {
        return PolicyError::Truncated;
    }
    for (usize i = 0U; i < sizeof(kPolicyMagic); ++i)
    {
        if (magic[i] != kPolicyMagic[i])
        {
            return PolicyError::BadMagic;
        }
    }
    if (!reader.take_u8(out.format))
    {
        return PolicyError::Truncated;
    }
    if (out.format < kPolicyFormatOldest || out.format > kPolicyFormat)
    {
        return PolicyError::UnsupportedFormat;
    }
    PolicyContent& content = out.content;
    if (!reader.take_u64(content.generation) || !reader.take_u64(content.issued) ||
        !reader.take_u64(content.expires) || !reader.take_u32(content.offline_grace))
    {
        return PolicyError::Truncated;
    }
    u8 minimums = 0U;
    if (!reader.take_u8(minimums))
    {
        return PolicyError::Truncated;
    }
    if (minimums > kMaxPolicyMinimums)
    {
        return PolicyError::TooLarge;
    }
    content.minimum_count = minimums;
    for (u32 i = 0U; i < content.minimum_count; ++i)
    {
        MinimumVersion& minimum = content.minimums[i];
        if (!reader.take_u16(minimum.protocol) || !reader.take_u16(minimum.major) || !reader.take_u16(minimum.minor))
        {
            return PolicyError::Truncated;
        }
    }
    u8 rules = 0U;
    if (!reader.take_u8(rules))
    {
        return PolicyError::Truncated;
    }
    if (rules > kMaxPolicyRules)
    {
        return PolicyError::TooLarge;
    }
    content.rule_count = rules;
    for (u32 i = 0U; i < content.rule_count; ++i)
    {
        AlgorithmRule& rule      = content.rules[i];
        u16            algorithm = 0U;
        u8             scope     = 0U;
        if (!reader.take_u16(algorithm) || (out.format >= 2U && !reader.take_u8(scope)) ||
            !reader.take_u64(rule.accepted_from) || !reader.take_u64(rule.preferred_from) ||
            !reader.take_u64(rule.preferred_until) || !reader.take_u64(rule.refused_from))
        {
            return PolicyError::Truncated;
        }
        rule.algorithm = static_cast<AlgorithmId>(algorithm);
        rule.scope = static_cast<PolicyScope>(scope);
    }
    return validate_content(content);
}

PolicyError policy_error_of(signed_artifact::Error error) noexcept
{
    switch (error)
    {
    case signed_artifact::Error::None:
        return PolicyError::None;
    case signed_artifact::Error::Truncated:
        return PolicyError::Truncated;
    case signed_artifact::Error::TooLarge:
        return PolicyError::TooLarge;
    case signed_artifact::Error::Unsigned:
        return PolicyError::Unsigned;
    case signed_artifact::Error::BadEntry:
        return PolicyError::BadSignatureEntry;
    case signed_artifact::Error::Duplicate:
        return PolicyError::DuplicateSignature;
    }
    return PolicyError::BadSignatureEntry;
}

PolicyError validate_signatures(cont::ConstSpan<KeySignature> signatures) noexcept
{
    return policy_error_of(signed_artifact::validate(signatures, kMaxPolicySignatures));
}

// Every trust key's signature over the body verifies; signatures by other keys are ignored.
PolicyError verify_signatures(const PolicyConfig& config, const PolicyMessage& message) noexcept
{
    switch (signed_artifact::verify_all(config.trust, *config.verifier, message.signed_bytes,
                                       cont::ConstSpan<KeySignature>(message.signatures, message.signature_count)))
    {
    case signed_artifact::Trust::Valid:
        return PolicyError::None;
    case signed_artifact::Trust::Missing:
        return PolicyError::MissingSignature;
    case signed_artifact::Trust::Unsupported:
        return PolicyError::UnsupportedSignature;
    case signed_artifact::Trust::Mismatch:
    case signed_artifact::Trust::Invalid:
        break;
    }
    return PolicyError::BadSignature;
}

bool same_content(const PolicyContent& a, const PolicyContent& b) noexcept
{
    if (a.generation != b.generation || a.issued != b.issued || a.expires != b.expires ||
        a.offline_grace != b.offline_grace || a.minimum_count != b.minimum_count || a.rule_count != b.rule_count)
    {
        return false;
    }
    for (u32 i = 0U; i < a.minimum_count; ++i)
    {
        const MinimumVersion& x = a.minimums[i];
        const MinimumVersion& y = b.minimums[i];
        if (x.protocol != y.protocol || x.major != y.major || x.minor != y.minor)
        {
            return false;
        }
    }
    for (u32 i = 0U; i < a.rule_count; ++i)
    {
        const AlgorithmRule& x = a.rules[i];
        const AlgorithmRule& y = b.rules[i];
        if (x.algorithm != y.algorithm || x.scope != y.scope || x.accepted_from != y.accepted_from ||
            x.preferred_from != y.preferred_from || x.preferred_until != y.preferred_until ||
            x.refused_from != y.refused_from)
        {
            return false;
        }
    }
    return true;
}

// The generation as this build uses it: the rules of the algorithms it supports in the scopes it knows, in order.
PolicyContent within_ceiling(const PolicyContent& content, cont::ConstSpan<AlgorithmId> supported) noexcept
{
    PolicyContent usable = content;
    usable.rule_count    = 0U;
    for (u32 i = 0U; i < content.rule_count; ++i)
    {
        if (known_scope(content.rules[i].scope) &&
            std::ranges::find(supported, content.rules[i].algorithm) != supported.end())
        {
            usable.rules[usable.rule_count++] = content.rules[i];
        }
    }
    return usable;
}
} // namespace

cont::StringView policy_error_name(PolicyError error) noexcept
{
    switch (error)
    {
    case PolicyError::None:
        return "none";
    case PolicyError::BadConfig:
        return "bad-config";
    case PolicyError::BufferTooSmall:
        return "buffer-too-small";
    case PolicyError::TooLarge:
        return "too-large";
    case PolicyError::Truncated:
        return "truncated";
    case PolicyError::TrailingBytes:
        return "trailing-bytes";
    case PolicyError::BadMagic:
        return "bad-magic";
    case PolicyError::UnsupportedFormat:
        return "unsupported-format";
    case PolicyError::BadGeneration:
        return "bad-generation";
    case PolicyError::BadDates:
        return "bad-dates";
    case PolicyError::BadRule:
        return "bad-rule";
    case PolicyError::DuplicateRule:
        return "duplicate-rule";
    case PolicyError::DuplicateMinimum:
        return "duplicate-minimum";
    case PolicyError::Unsigned:
        return "unsigned";
    case PolicyError::BadSignatureEntry:
        return "bad-signature-entry";
    case PolicyError::DuplicateSignature:
        return "duplicate-signature";
    case PolicyError::MissingSignature:
        return "missing-signature";
    case PolicyError::UnsupportedSignature:
        return "unsupported-signature";
    case PolicyError::BadSignature:
        return "bad-signature";
    case PolicyError::Rollback:
        return "rollback";
    case PolicyError::Conflict:
        return "conflict";
    case PolicyError::AlreadyCurrent:
        return "already-current";
    case PolicyError::NotYetIssued:
        return "not-yet-issued";
    case PolicyError::IssuedBeforeCurrent:
        return "issued-before-current";
    case PolicyError::Expired:
        return "expired";
    }
    return "unknown";
}

PolicyError validate_content(const PolicyContent& content) noexcept
{
    if (content.generation == 0U)
    {
        return PolicyError::BadGeneration;
    }
    if (content.expires <= content.issued || content.expires - content.issued > kMaxPolicyLifetime ||
        content.offline_grace > kMaxOfflineGrace)
    {
        return PolicyError::BadDates;
    }
    if (content.minimum_count > kMaxPolicyMinimums || content.rule_count > kMaxPolicyRules)
    {
        return PolicyError::TooLarge;
    }
    for (u32 i = 0U; i < content.minimum_count; ++i)
    {
        for (u32 j = 0U; j < i; ++j)
        {
            if (content.minimums[j].protocol == content.minimums[i].protocol)
            {
                return PolicyError::DuplicateMinimum;
            }
        }
    }
    for (u32 i = 0U; i < content.rule_count; ++i)
    {
        const PolicyError rule = validate_rule(content.rules[i]);
        if (rule != PolicyError::None)
        {
            return rule;
        }
        for (u32 j = 0U; j < i; ++j)
        {
            const AlgorithmRule& earlier = content.rules[j];
            if (earlier.algorithm == content.rules[i].algorithm && earlier.scope == content.rules[i].scope)
            {
                return PolicyError::DuplicateRule;
            }
        }
    }
    return PolicyError::None;
}

usize policy_body_size(const PolicyContent& content) noexcept
{
    return kHeadBytes + 1U + content.minimum_count * kMinimumBytes + 1U + content.rule_count * kRuleBytes;
}

PolicyError write_policy_body(const PolicyContent& content, cont::Span<u8> out, usize& written) noexcept
{
    written                 = 0U;
    const PolicyError valid = validate_content(content);
    if (valid != PolicyError::None)
    {
        return valid;
    }
    if (out.size() < policy_body_size(content))
    {
        return PolicyError::BufferTooSmall;
    }
    cont::WireWriter writer(out);
    writer.put_bytes(cont::ConstSpan<u8>(kPolicyMagic, sizeof(kPolicyMagic)));
    writer.put_u8(kPolicyFormat);
    writer.put_u64(content.generation);
    writer.put_u64(content.issued);
    writer.put_u64(content.expires);
    writer.put_u32(content.offline_grace);
    writer.put_u8(static_cast<u8>(content.minimum_count));
    for (u32 i = 0U; i < content.minimum_count; ++i)
    {
        writer.put_u16(content.minimums[i].protocol);
        writer.put_u16(content.minimums[i].major);
        writer.put_u16(content.minimums[i].minor);
    }
    writer.put_u8(static_cast<u8>(content.rule_count));
    for (u32 i = 0U; i < content.rule_count; ++i)
    {
        const AlgorithmRule& rule = content.rules[i];
        writer.put_u16(static_cast<u16>(rule.algorithm));
        writer.put_u8(static_cast<u8>(rule.scope));
        writer.put_u64(rule.accepted_from);
        writer.put_u64(rule.preferred_from);
        writer.put_u64(rule.preferred_until);
        writer.put_u64(rule.refused_from);
    }
    written = writer.written();
    return PolicyError::None;
}

PolicyError write_policy(cont::ConstSpan<u8> body, cont::ConstSpan<KeySignature> signatures, cont::Span<u8> out,
                         usize& written) noexcept
{
    written = 0U;
    PolicyMessage    parsed;
    cont::WireReader reader(body);
    const PolicyError valid = read_body(reader, parsed);
    if (valid != PolicyError::None)
    {
        return valid;
    }
    if (!reader.done())
    {
        return PolicyError::TrailingBytes;
    }
    const PolicyError signed_ok = validate_signatures(signatures);
    if (signed_ok != PolicyError::None)
    {
        return signed_ok;
    }
    const usize size = body.size() + signed_artifact::encoded_size(signatures);
    if (size > kMaxPolicyBytes)
    {
        return PolicyError::TooLarge;
    }
    if (out.size() < size)
    {
        return PolicyError::BufferTooSmall;
    }
    cont::WireWriter writer(out);
    writer.put_bytes(body);
    signed_artifact::write(writer, signatures);
    written = writer.written();
    return PolicyError::None;
}

PolicyError decode_policy(cont::ConstSpan<u8> bytes, PolicyMessage& out) noexcept
{
    if (bytes.size() > kMaxPolicyBytes)
    {
        return PolicyError::TooLarge;
    }
    cont::WireReader  reader(bytes);
    const PolicyError body = read_body(reader, out);
    if (body != PolicyError::None)
    {
        return body;
    }
    out.signed_bytes       = bytes.first(reader.consumed());
    const PolicyError list = policy_error_of(signed_artifact::read(
        reader, cont::Span<KeySignature>(out.signatures, kMaxPolicySignatures), out.signature_count));
    if (list != PolicyError::None)
    {
        return list;
    }
    if (!reader.done())
    {
        return PolicyError::TrailingBytes;
    }
    return validate_signatures(cont::ConstSpan<KeySignature>(out.signatures, out.signature_count));
}

PolicyError validate_config(const PolicyConfig& config) noexcept
{
    if (config.verifier == nullptr || config.trust.size() < 2U || config.trust.size() > kMaxTrustKeys)
    {
        return PolicyError::BadConfig;
    }
    bool post_quantum = false;
    bool traditional  = false;
    for (usize i = 0U; i < config.trust.size(); ++i)
    {
        const TrustKey& key = config.trust[i];
        if (!signed_artifact::is_signature(key.algorithm) || key.public_key.empty())
        {
            return PolicyError::BadConfig;
        }
        for (usize j = 0U; j < i; ++j)
        {
            if (config.trust[j].id == key.id)
            {
                return PolicyError::BadConfig;
            }
        }
        post_quantum = post_quantum || algorithm_class(key.algorithm) == AlgorithmClass::PostQuantumSignature;
        traditional  = traditional || algorithm_class(key.algorithm) == AlgorithmClass::ClassicalSignature;
    }
    return post_quantum && traditional ? PolicyError::None : PolicyError::BadConfig;
}

Freshness PolicySnapshot::freshness(u64 now) const noexcept
{
    if (m_content.generation == 0U)
    {
        return Freshness::None;
    }
    if (now < m_content.expires)
    {
        return Freshness::Current;
    }
    return now < saturating_add(m_content.expires, m_content.offline_grace) ? Freshness::Grace : Freshness::Expired;
}

AlgorithmStatus PolicySnapshot::status(AlgorithmId algorithm, u64 now, PolicyScope scope) const noexcept
{
    const Freshness fresh = freshness(now);
    if (fresh != Freshness::Current && fresh != Freshness::Grace)
    {
        return AlgorithmStatus::Refused;
    }
    // The scope's own rule wins over the Everywhere rule; the snapshot holds known scopes only.
    const AlgorithmRule* governing = nullptr;
    for (u32 i = 0U; i < m_content.rule_count; ++i)
    {
        const AlgorithmRule& rule = m_content.rules[i];
        if (rule.algorithm == algorithm && (rule.scope == scope || rule.scope == PolicyScope::Everywhere) &&
            (governing == nullptr || rule.scope == scope))
        {
            governing = &rule;
        }
    }
    if (governing == nullptr || now < governing->accepted_from || now >= governing->refused_from)
    {
        return AlgorithmStatus::Refused;
    }
    const bool preferred = now >= governing->preferred_from && now < governing->preferred_until;
    return preferred ? AlgorithmStatus::Preferred : AlgorithmStatus::Accepted;
}

bool PolicySnapshot::allows_version(u16 protocol, u16 major, u16 minor) const noexcept
{
    for (u32 i = 0U; i < m_content.minimum_count; ++i)
    {
        const MinimumVersion& minimum = m_content.minimums[i];
        if (minimum.protocol == protocol)
        {
            return major > minimum.major || (major == minimum.major && minor >= minimum.minor);
        }
    }
    return true;
}

PolicyStore::PolicyStore(const PolicyConfig& config) noexcept : m_config(config), m_floor(config.floor_generation)
{
}

PolicyError PolicyStore::apply(cont::ConstSpan<u8> generation, u64 now) noexcept
{
    if (validate_config(m_config) != PolicyError::None)
    {
        return PolicyError::BadConfig;
    }
    PolicyMessage     message;
    const PolicyError decoded = decode_policy(generation, message);
    if (decoded != PolicyError::None)
    {
        return decoded;
    }
    const PolicyContent& content = message.content;
    if (content.generation < floor_generation())
    {
        return PolicyError::Rollback;
    }
    const PolicyError verified = verify_signatures(m_config, message);
    if (verified != PolicyError::None)
    {
        return verified;
    }

    const std::lock_guard<std::mutex> lock(m_mutex);
    if (content.generation < m_floor)
    {
        return PolicyError::Rollback; // a newer generation landed while this one was verified
    }
    const bool has_current = m_accepted.generation != 0U;
    if (has_current && content.generation == m_accepted.generation)
    {
        return same_content(content, m_accepted) ? PolicyError::AlreadyCurrent : PolicyError::Conflict;
    }
    if (content.issued > saturating_add(now, kMaxIssueSkew))
    {
        return PolicyError::NotYetIssued;
    }
    if (now >= saturating_add(content.expires, content.offline_grace))
    {
        return PolicyError::Expired;
    }
    if (has_current && content.issued < m_accepted.issued)
    {
        return PolicyError::IssuedBeforeCurrent;
    }
    m_accepted          = content;
    m_current.m_content = within_ceiling(content, m_config.supported);
    m_floor             = content.generation;
    return PolicyError::None;
}

PolicySnapshot PolicyStore::snapshot() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_current;
}

u64 PolicyStore::floor_generation() const noexcept
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_floor;
}
} // namespace crd::crypto
