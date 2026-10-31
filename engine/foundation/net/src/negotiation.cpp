// crd-net -- version and extension negotiation (NET-STD). Wire layout, every integer big-endian:
//
//   offer   kind u8 (1) | format u8 | protocol u16 | version count u8 | count x (major u16, minor u16)
//           | extension count u8 | count x (id u16, length u16, length bytes)
//   answer  kind u8 (2) | format u8 | protocol u16 | selected major u16 | selected minor u16
//           | available version count u8 | count x (major u16, minor u16) | extension count u8 | extensions
//
// Contract and rationale: docs/design/network-foundation.md#negotiation.
#include <crd/net/negotiation.hpp>

#include <crd/core/assert.hpp>

#include <algorithm>

namespace crd::net
{
namespace
{
constexpr usize kVersionBytes        = 4U;
constexpr usize kExtensionHeadBytes  = 4U;
constexpr usize kOfferHeadBytes      = 4U; // kind, format, protocol
constexpr usize kAnswerSelectedBytes = 4U;
constexpr u8    kTranscriptLabel[8]  = {'c', 'r', 'd', 'n', 'e', 'g', '0', '1'};

class Writer
{
public:
    explicit Writer(cont::Span<u8> out) noexcept : m_out(out) {}

    void put_u8(u8 value) noexcept
    {
        CRD_ASSERT(m_at < m_out.size());
        m_out[m_at++] = value;
    }

    void put_u16(u16 value) noexcept
    {
        put_u8(static_cast<u8>(value >> 8U));
        put_u8(static_cast<u8>(value & 0xFFU));
    }

    void put_u32(u32 value) noexcept
    {
        put_u16(static_cast<u16>(value >> 16U));
        put_u16(static_cast<u16>(value & 0xFFFFU));
    }

    void put_bytes(cont::ConstSpan<u8> data) noexcept
    {
        for (const u8 byte : data)
        {
            put_u8(byte);
        }
    }

    [[nodiscard]] usize written() const noexcept { return m_at; }

private:
    cont::Span<u8> m_out;
    usize          m_at = 0U;
};

class Reader
{
public:
    explicit Reader(cont::ConstSpan<u8> in) noexcept : m_in(in) {}

    [[nodiscard]] bool take_u8(u8& out) noexcept
    {
        if (m_at >= m_in.size())
        {
            return false;
        }
        out = m_in[m_at++];
        return true;
    }

    [[nodiscard]] bool take_u16(u16& out) noexcept
    {
        u8 high = 0U;
        u8 low  = 0U;
        if (!take_u8(high) || !take_u8(low))
        {
            return false;
        }
        out = static_cast<u16>((static_cast<u16>(high) << 8U) | low);
        return true;
    }

    [[nodiscard]] bool take_bytes(usize count, cont::ConstSpan<u8>& out) noexcept
    {
        if (m_in.size() - m_at < count)
        {
            return false;
        }
        out = m_in.subspan(m_at, count);
        m_at += count;
        return true;
    }

    [[nodiscard]] bool done() const noexcept { return m_at == m_in.size(); }

private:
    cont::ConstSpan<u8> m_in;
    usize               m_at = 0U;
};

// Where one message's GREASE goes and what it holds, drawn from its entropy: the version major and minor and its
// slot among the versions; two distinct extension identifiers, their slots among the extensions and 0 to 7 value
// bytes each.
struct GreasePlan
{
    ProtocolVersion version;
    u32             version_slot = 0U;
    u16             extension_ids[kGreaseExtensions]   = {};
    u32             extension_slots[kGreaseExtensions] = {};
    const u8*       values[kGreaseExtensions]          = {};
    u32             lengths[kGreaseExtensions]         = {};
};

GreasePlan plan_grease(const GreaseEntropy& entropy, u32 version_count, u32 extension_count) noexcept
{
    const u8*  e = entropy.bytes;
    GreasePlan plan;
    plan.version.major = kGreaseIds[e[0] & 0x0FU];
    plan.version.minor = static_cast<u16>((static_cast<u16>(e[1]) << 8U) | e[2]);
    plan.version_slot  = e[3] % (version_count + kGreaseVersions);

    const u32 first  = e[4] & 0x0FU;
    const u32 second = (first + 1U + ((e[4] >> 4U) % 15U)) & 0x0FU;
    plan.extension_ids[0] = kGreaseIds[first];
    plan.extension_ids[1] = kGreaseIds[second];

    const u32 slots = extension_count + kGreaseExtensions;
    plan.extension_slots[0] = e[5] % slots;
    plan.extension_slots[1] = e[6] % slots;
    if (plan.extension_slots[1] == plan.extension_slots[0])
    {
        plan.extension_slots[1] = (plan.extension_slots[0] + 1U) % slots;
    }
    plan.lengths[0] = e[7] % (kGreaseMaxValueBytes + 1U);
    plan.lengths[1] = (e[7] >> 3U) % (kGreaseMaxValueBytes + 1U);
    plan.values[0]  = e + 8U;
    plan.values[1]  = e + 8U + kGreaseMaxValueBytes;
    return plan;
}

static_assert(8U + 2U * kGreaseMaxValueBytes <= kGreaseEntropyBytes, "GREASE values must fit the entropy");

usize versions_bytes(u32 count) noexcept
{
    return 1U + (count + kGreaseVersions) * kVersionBytes;
}

usize extensions_bytes(cont::ConstSpan<ExtensionEntry> extensions, const GreasePlan& plan) noexcept
{
    usize size = 1U;
    for (const ExtensionEntry& extension : extensions)
    {
        size += kExtensionHeadBytes + extension.value.size();
    }
    for (u32 i = 0U; i < kGreaseExtensions; ++i)
    {
        size += kExtensionHeadBytes + plan.lengths[i];
    }
    return size;
}

void write_versions(Writer& writer, cont::ConstSpan<ProtocolVersion> versions, const GreasePlan& plan) noexcept
{
    const auto count = static_cast<u32>(versions.size());
    writer.put_u8(static_cast<u8>(count + kGreaseVersions));
    u32 next = 0U;
    for (u32 slot = 0U; slot < count + kGreaseVersions; ++slot)
    {
        const ProtocolVersion version = slot == plan.version_slot ? plan.version : versions[next++];
        writer.put_u16(version.major);
        writer.put_u16(version.minor);
    }
}

void write_extensions(Writer& writer, cont::ConstSpan<ExtensionEntry> extensions, const GreasePlan& plan) noexcept
{
    const auto count = static_cast<u32>(extensions.size());
    writer.put_u8(static_cast<u8>(count + kGreaseExtensions));
    u32 next = 0U;
    for (u32 slot = 0U; slot < count + kGreaseExtensions; ++slot)
    {
        u32 grease = kGreaseExtensions;
        for (u32 i = 0U; i < kGreaseExtensions; ++i)
        {
            if (slot == plan.extension_slots[i])
            {
                grease = i;
            }
        }
        if (grease < kGreaseExtensions)
        {
            writer.put_u16(plan.extension_ids[grease]);
            writer.put_u16(static_cast<u16>(plan.lengths[grease]));
            writer.put_bytes(cont::ConstSpan<u8>(plan.values[grease], plan.lengths[grease]));
            continue;
        }
        const ExtensionEntry& extension = extensions[next++];
        writer.put_u16(extension.id);
        writer.put_u16(static_cast<u16>(extension.value.size()));
        writer.put_bytes(extension.value);
    }
}

bool contains(cont::ConstSpan<u16> ids, u16 id) noexcept
{
    return std::ranges::find(ids, id) != ids.end();
}

const ProtocolVersion* find_major(const ProtocolVersion* versions, usize count, u16 major) noexcept
{
    for (usize i = 0U; i < count; ++i)
    {
        if (versions[i].major == major)
        {
            return &versions[i];
        }
    }
    return nullptr;
}

const ExtensionEntry* find_extension(const ExtensionEntry* extensions, usize count, u16 id) noexcept
{
    for (usize i = 0U; i < count; ++i)
    {
        if (extensions[i].id == id)
        {
            return &extensions[i];
        }
    }
    return nullptr;
}

// A list of extensions a writer emits: bounded, identifiers unique and `allowed` (else `outside`), values bounded.
template <typename Allowed>
NegotiationError check_extensions(cont::ConstSpan<ExtensionEntry> extensions, NegotiationError outside,
                                  const Allowed& allowed) noexcept
{
    if (extensions.size() > kMaxExtensions - kGreaseExtensions)
    {
        return NegotiationError::TooLarge;
    }
    for (usize i = 0U; i < extensions.size(); ++i)
    {
        if (!allowed(extensions[i].id))
        {
            return outside;
        }
        if (extensions[i].value.size() > kMaxExtensionBytes)
        {
            return NegotiationError::TooLarge;
        }
        if (find_extension(extensions.data(), i, extensions[i].id) != nullptr)
        {
            return NegotiationError::DuplicateExtension;
        }
    }
    return NegotiationError::None;
}

// The highest major both lists hold, GREASE skipped; 0 when none.
u16 highest_common_major(const NegotiationMessage& a, const NegotiationMessage& b) noexcept
{
    u16 best = 0U;
    for (u32 i = 0U; i < a.version_count; ++i)
    {
        const u16 major = a.versions[i].major;
        if (!is_grease(major) && major > best && find_major(b.versions, b.version_count, major) != nullptr)
        {
            best = major;
        }
    }
    return best;
}
} // namespace

cont::StringView negotiation_error_name(NegotiationError error) noexcept
{
    switch (error)
    {
    case NegotiationError::None:
        return "none";
    case NegotiationError::BadPolicy:
        return "bad-policy";
    case NegotiationError::BufferTooSmall:
        return "buffer-too-small";
    case NegotiationError::TooLarge:
        return "too-large";
    case NegotiationError::Truncated:
        return "truncated";
    case NegotiationError::TrailingBytes:
        return "trailing-bytes";
    case NegotiationError::WrongKind:
        return "wrong-kind";
    case NegotiationError::UnsupportedFormat:
        return "unsupported-format";
    case NegotiationError::NoVersions:
        return "no-versions";
    case NegotiationError::DuplicateVersion:
        return "duplicate-version";
    case NegotiationError::DuplicateExtension:
        return "duplicate-extension";
    case NegotiationError::WrongProtocol:
        return "wrong-protocol";
    case NegotiationError::NoCommonVersion:
        return "no-common-version";
    case NegotiationError::GreaseSelected:
        return "grease-selected";
    case NegotiationError::UnofferedVersion:
        return "unoffered-version";
    case NegotiationError::InconsistentAnswer:
        return "inconsistent-answer";
    case NegotiationError::Downgrade:
        return "downgrade";
    case NegotiationError::UnsolicitedExtension:
        return "unsolicited-extension";
    }
    return "unknown";
}

NegotiationError validate_policy(const NegotiationPolicy& policy) noexcept
{
    if (protocol_register_id(policy.protocol)[0] == '\0')
    {
        return NegotiationError::BadPolicy;
    }
    if (policy.versions.empty() || policy.versions.size() > kMaxVersions - kGreaseVersions ||
        policy.known_extensions.size() > kMaxExtensions - kGreaseExtensions)
    {
        return NegotiationError::BadPolicy;
    }
    for (usize i = 0U; i < policy.versions.size(); ++i)
    {
        const u16 major = policy.versions[i].major;
        if (major == 0U || is_grease(major) || find_major(policy.versions.data(), i, major) != nullptr)
        {
            return NegotiationError::BadPolicy;
        }
    }
    for (usize i = 0U; i < policy.known_extensions.size(); ++i)
    {
        const u16 id = policy.known_extensions[i];
        if (is_grease(id) || contains(policy.known_extensions.first(i), id))
        {
            return NegotiationError::BadPolicy;
        }
    }
    return NegotiationError::None;
}

NegotiationError decode_negotiation(cont::ConstSpan<u8> bytes, MessageKind expected, NegotiationMessage& out) noexcept
{
    if (bytes.size() > kMaxMessageBytes)
    {
        return NegotiationError::TooLarge;
    }
    Reader reader(bytes);
    u8     kind = 0U;
    if (!reader.take_u8(kind))
    {
        return NegotiationError::Truncated;
    }
    if (kind != static_cast<u8>(expected))
    {
        return NegotiationError::WrongKind;
    }
    out = NegotiationMessage{};
    out.kind = expected;
    if (!reader.take_u8(out.format))
    {
        return NegotiationError::Truncated;
    }
    if (out.format < kNegotiationFormatOldest || out.format > kNegotiationFormat)
    {
        return NegotiationError::UnsupportedFormat;
    }
    if (!reader.take_u16(out.protocol))
    {
        return NegotiationError::Truncated;
    }
    if (expected == MessageKind::Answer &&
        (!reader.take_u16(out.selected.major) || !reader.take_u16(out.selected.minor)))
    {
        return NegotiationError::Truncated;
    }
    u8 version_count = 0U;
    if (!reader.take_u8(version_count))
    {
        return NegotiationError::Truncated;
    }
    if (version_count == 0U)
    {
        return NegotiationError::NoVersions;
    }
    if (version_count > kMaxVersions)
    {
        return NegotiationError::TooLarge;
    }
    for (u32 i = 0U; i < version_count; ++i)
    {
        ProtocolVersion& version = out.versions[i];
        if (!reader.take_u16(version.major) || !reader.take_u16(version.minor))
        {
            return NegotiationError::Truncated;
        }
        if (find_major(out.versions, i, version.major) != nullptr)
        {
            return NegotiationError::DuplicateVersion;
        }
        out.version_count = i + 1U;
    }
    u8 extension_count = 0U;
    if (!reader.take_u8(extension_count))
    {
        return NegotiationError::Truncated;
    }
    if (extension_count > kMaxExtensions)
    {
        return NegotiationError::TooLarge;
    }
    for (u32 i = 0U; i < extension_count; ++i)
    {
        ExtensionEntry& extension = out.extensions[i];
        u16             length    = 0U;
        if (!reader.take_u16(extension.id) || !reader.take_u16(length))
        {
            return NegotiationError::Truncated;
        }
        if (length > kMaxExtensionBytes)
        {
            return NegotiationError::TooLarge;
        }
        if (!reader.take_bytes(length, extension.value))
        {
            return NegotiationError::Truncated;
        }
        if (find_extension(out.extensions, i, extension.id) != nullptr)
        {
            return NegotiationError::DuplicateExtension;
        }
        out.extension_count = i + 1U;
    }
    return reader.done() ? NegotiationError::None : NegotiationError::TrailingBytes;
}

NegotiationError write_offer(const NegotiationPolicy& policy, cont::ConstSpan<ExtensionEntry> requested,
                             const GreaseEntropy& grease, cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    if (validate_policy(policy) != NegotiationError::None)
    {
        return NegotiationError::BadPolicy;
    }
    const auto known = [&policy](u16 id)
    {
        return contains(policy.known_extensions, id);
    };
    const NegotiationError invalid = check_extensions(requested, NegotiationError::BadPolicy, known);
    if (invalid != NegotiationError::None)
    {
        return invalid;
    }
    const auto       version_count = static_cast<u32>(policy.versions.size());
    const GreasePlan plan          = plan_grease(grease, version_count, static_cast<u32>(requested.size()));
    const usize      size = kOfferHeadBytes + versions_bytes(version_count) + extensions_bytes(requested, plan);
    if (size > kMaxMessageBytes)
    {
        return NegotiationError::TooLarge;
    }
    if (out.size() < size)
    {
        return NegotiationError::BufferTooSmall;
    }
    Writer writer(out);
    writer.put_u8(static_cast<u8>(MessageKind::Offer));
    writer.put_u8(kNegotiationFormat);
    writer.put_u16(static_cast<u16>(policy.protocol));
    write_versions(writer, policy.versions, plan);
    write_extensions(writer, requested, plan);
    written = writer.written();
    return NegotiationError::None;
}

NegotiationError select_negotiation(const NegotiationPolicy& policy, const NegotiationMessage& offer,
                                    NegotiationSelection& out) noexcept
{
    if (validate_policy(policy) != NegotiationError::None)
    {
        return NegotiationError::BadPolicy;
    }
    if (offer.kind != MessageKind::Offer)
    {
        return NegotiationError::WrongKind;
    }
    if (offer.protocol != static_cast<u16>(policy.protocol))
    {
        return NegotiationError::WrongProtocol;
    }
    out = NegotiationSelection{};
    for (const ProtocolVersion& local : policy.versions)
    {
        const ProtocolVersion* offered = find_major(offer.versions, offer.version_count, local.major);
        if (offered != nullptr && local.major > out.version.major)
        {
            out.version.major = local.major;
            out.version.minor = offered->minor < local.minor ? offered->minor : local.minor;
        }
    }
    if (out.version.major == 0U)
    {
        return NegotiationError::NoCommonVersion;
    }
    for (u32 i = 0U; i < offer.extension_count; ++i)
    {
        if (contains(policy.known_extensions, offer.extensions[i].id))
        {
            out.offered[out.offered_count++] = offer.extensions[i];
        }
    }
    return NegotiationError::None;
}

NegotiationError write_answer(const NegotiationPolicy& policy, const NegotiationSelection& selection,
                              cont::ConstSpan<ExtensionEntry> accepted, const GreaseEntropy& grease,
                              cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    if (validate_policy(policy) != NegotiationError::None)
    {
        return NegotiationError::BadPolicy;
    }
    const ProtocolVersion* local =
        find_major(policy.versions.data(), policy.versions.size(), selection.version.major);
    if (local == nullptr || selection.version.minor > local->minor)
    {
        return NegotiationError::BadPolicy;
    }
    const auto offered = [&selection](u16 id)
    {
        return find_extension(selection.offered, selection.offered_count, id) != nullptr;
    };
    const NegotiationError invalid = check_extensions(accepted, NegotiationError::UnsolicitedExtension, offered);
    if (invalid != NegotiationError::None)
    {
        return invalid;
    }
    const auto       version_count = static_cast<u32>(policy.versions.size());
    const GreasePlan plan          = plan_grease(grease, version_count, static_cast<u32>(accepted.size()));
    const usize      size          = kOfferHeadBytes + kAnswerSelectedBytes + versions_bytes(version_count) +
                           extensions_bytes(accepted, plan);
    if (size > kMaxMessageBytes)
    {
        return NegotiationError::TooLarge;
    }
    if (out.size() < size)
    {
        return NegotiationError::BufferTooSmall;
    }
    Writer writer(out);
    writer.put_u8(static_cast<u8>(MessageKind::Answer));
    writer.put_u8(kNegotiationFormat);
    writer.put_u16(static_cast<u16>(policy.protocol));
    writer.put_u16(selection.version.major);
    writer.put_u16(selection.version.minor);
    write_versions(writer, policy.versions, plan);
    write_extensions(writer, accepted, plan);
    written = writer.written();
    return NegotiationError::None;
}

NegotiationError verify_answer(const NegotiationPolicy& policy, const NegotiationMessage& offer,
                               const NegotiationMessage& answer, NegotiationResult& out) noexcept
{
    if (validate_policy(policy) != NegotiationError::None)
    {
        return NegotiationError::BadPolicy;
    }
    if (offer.kind != MessageKind::Offer || answer.kind != MessageKind::Answer)
    {
        return NegotiationError::WrongKind;
    }
    if (answer.protocol != offer.protocol || answer.protocol != static_cast<u16>(policy.protocol))
    {
        return NegotiationError::WrongProtocol;
    }
    const ProtocolVersion selected = answer.selected;
    if (is_grease(selected.major))
    {
        return NegotiationError::GreaseSelected;
    }
    const ProtocolVersion* offered = find_major(offer.versions, offer.version_count, selected.major);
    if (offered == nullptr || selected.minor > offered->minor)
    {
        return NegotiationError::UnofferedVersion;
    }
    const ProtocolVersion* available = find_major(answer.versions, answer.version_count, selected.major);
    if (available == nullptr ||
        selected.minor != (offered->minor < available->minor ? offered->minor : available->minor))
    {
        return NegotiationError::InconsistentAnswer;
    }
    if (highest_common_major(offer, answer) != selected.major)
    {
        return NegotiationError::Downgrade;
    }
    out = NegotiationResult{};
    out.version = selected;
    for (u32 i = 0U; i < answer.extension_count; ++i)
    {
        const ExtensionEntry& extension = answer.extensions[i];
        if (is_grease(extension.id))
        {
            continue;
        }
        if (find_extension(offer.extensions, offer.extension_count, extension.id) != nullptr)
        {
            out.extensions[out.extension_count++] = extension;
        }
        else if (contains(policy.known_extensions, extension.id))
        {
            return NegotiationError::UnsolicitedExtension;
        }
    }
    return NegotiationError::None;
}

NegotiationError write_transcript(cont::ConstSpan<u8> offer, cont::ConstSpan<u8> answer, cont::Span<u8> out,
                                  usize& written) noexcept
{
    written = 0U;
    if (offer.size() > kMaxMessageBytes || answer.size() > kMaxMessageBytes)
    {
        return NegotiationError::TooLarge;
    }
    const usize size = transcript_size(offer.size(), answer.size());
    if (out.size() < size)
    {
        return NegotiationError::BufferTooSmall;
    }
    Writer writer(out);
    writer.put_bytes(kTranscriptLabel);
    writer.put_u32(static_cast<u32>(offer.size()));
    writer.put_bytes(offer);
    writer.put_u32(static_cast<u32>(answer.size()));
    writer.put_bytes(answer);
    written = writer.written();
    return NegotiationError::None;
}
} // namespace crd::net
