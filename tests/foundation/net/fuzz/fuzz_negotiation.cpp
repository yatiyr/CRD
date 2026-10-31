// crd-fuzz-net-negotiation: the version and extension negotiation of crd-net (NET-STD;
// docs/design/network-foundation.md#negotiation) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data.
//   0  an offer as bytes, received by a fixed server;
//   1  an answer as bytes, received by a fixed client that sent a fixed offer;
//   2  a generated exchange: a client and a server policy, the client's requested values and both entropies are
//      read from the bytes, and the whole exchange runs through the writers and readers.
//
// Oracles, none of them the codec's own verdict:
//   * an independent restatement of the wire layout and its bounds decides the decoder's exact result for every
//     input, and an accepted message's fields equal the restatement's;
//   * a server that accepts an offer answers it so that a client which could have sent that offer (its versions and
//     extensions) accepts the answer, with exactly the server's version and values;
//   * every message a writer produces decodes, carries one GREASE version and two GREASE extensions and never
//     selects a GREASE value;
//   * an answer to the fixed offer is accepted exactly when it selects the highest common major with the lower minor
//     and accepts only offered extensions, restated from the contract;
//   * a generated exchange succeeds exactly when the policies are valid and share a major.
#include <crd/fuzz/harness.hpp>

#include <crd/net/negotiation.hpp>

#include <cstring>

namespace
{
using crd::u16;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::net;

// ---- The restated layout ---------------------------------------------------------------------------------------

struct Restated
{
    NegotiationError error = NegotiationError::None;
    u16              protocol = 0U;
    ProtocolVersion  selected;
    ProtocolVersion  versions[kMaxVersions];
    u32              version_count = 0U;
    u16              ids[kMaxExtensions]   = {};
    usize            offsets[kMaxExtensions] = {};
    usize            lengths[kMaxExtensions] = {};
    u32              extension_count = 0U;
};

u16 be16(const u8* p)
{
    return static_cast<u16>((p[0] << 8U) | p[1]);
}

Restated restate(const u8* data, usize size, MessageKind kind)
{
    Restated r;
    if (size > kMaxMessageBytes)
    {
        r.error = NegotiationError::TooLarge;
        return r;
    }
    usize at = 0U;
    const auto fail = [&r](NegotiationError error)
    {
        r.error = error;
        return r;
    };
    if (size < 1U)
    {
        return fail(NegotiationError::Truncated);
    }
    if (data[0] != static_cast<u8>(kind))
    {
        return fail(NegotiationError::WrongKind);
    }
    if (size < 2U)
    {
        return fail(NegotiationError::Truncated);
    }
    if (data[1] < kNegotiationFormatOldest || data[1] > kNegotiationFormat)
    {
        return fail(NegotiationError::UnsupportedFormat);
    }
    if (size < 4U)
    {
        return fail(NegotiationError::Truncated);
    }
    r.protocol = be16(data + 2U);
    at         = 4U;
    if (kind == MessageKind::Answer)
    {
        if (size < 8U)
        {
            return fail(NegotiationError::Truncated);
        }
        r.selected = ProtocolVersion{be16(data + 4U), be16(data + 6U)};
        at         = 8U;
    }
    if (at >= size)
    {
        return fail(NegotiationError::Truncated);
    }
    const u32 versions = data[at++];
    if (versions == 0U)
    {
        return fail(NegotiationError::NoVersions);
    }
    if (versions > kMaxVersions)
    {
        return fail(NegotiationError::TooLarge);
    }
    for (u32 i = 0U; i < versions; ++i)
    {
        if (size - at < 4U)
        {
            return fail(NegotiationError::Truncated);
        }
        const ProtocolVersion version{be16(data + at), be16(data + at + 2U)};
        at += 4U;
        for (u32 j = 0U; j < i; ++j)
        {
            if (r.versions[j].major == version.major)
            {
                return fail(NegotiationError::DuplicateVersion);
            }
        }
        r.versions[r.version_count++] = version;
    }
    if (at >= size)
    {
        return fail(NegotiationError::Truncated);
    }
    const u32 extensions = data[at++];
    if (extensions > kMaxExtensions)
    {
        return fail(NegotiationError::TooLarge);
    }
    for (u32 i = 0U; i < extensions; ++i)
    {
        if (size - at < 4U)
        {
            return fail(NegotiationError::Truncated);
        }
        const u16   id     = be16(data + at);
        const usize length = be16(data + at + 2U);
        at += 4U;
        if (length > kMaxExtensionBytes)
        {
            return fail(NegotiationError::TooLarge);
        }
        if (size - at < length)
        {
            return fail(NegotiationError::Truncated);
        }
        for (u32 j = 0U; j < i; ++j)
        {
            if (r.ids[j] == id)
            {
                return fail(NegotiationError::DuplicateExtension);
            }
        }
        r.ids[i]     = id;
        r.offsets[i] = at;
        r.lengths[i] = length;
        r.extension_count++;
        at += length;
    }
    return at == size ? r : fail(NegotiationError::TrailingBytes);
}

// Decode `bytes` and hold the decoder to the restatement; true when it accepted.
bool decode_checked(ConstSpan<u8> bytes, MessageKind kind, NegotiationMessage& out)
{
    const Restated         expected = restate(bytes.data(), bytes.size(), kind);
    const NegotiationError error    = decode_negotiation(bytes, kind, out);
    CRD_FUZZ_REQUIRE(error == expected.error);
    if (error != NegotiationError::None)
    {
        return false;
    }
    CRD_FUZZ_REQUIRE(out.kind == kind && out.protocol == expected.protocol);
    CRD_FUZZ_REQUIRE(kind == MessageKind::Offer || (out.selected.major == expected.selected.major &&
                                                    out.selected.minor == expected.selected.minor));
    CRD_FUZZ_REQUIRE(out.version_count == expected.version_count && out.extension_count == expected.extension_count);
    for (u32 i = 0U; i < out.version_count; ++i)
    {
        CRD_FUZZ_REQUIRE(out.versions[i].major == expected.versions[i].major &&
                         out.versions[i].minor == expected.versions[i].minor);
    }
    for (u32 i = 0U; i < out.extension_count; ++i)
    {
        CRD_FUZZ_REQUIRE(out.extensions[i].id == expected.ids[i]);
        CRD_FUZZ_REQUIRE(out.extensions[i].value.data() == bytes.data() + expected.offsets[i]);
        CRD_FUZZ_REQUIRE(out.extensions[i].value.size() == expected.lengths[i]);
    }
    return true;
}

u32 count_grease(const NegotiationMessage& m, bool versions)
{
    u32 count = 0U;
    const u32 total = versions ? m.version_count : m.extension_count;
    for (u32 i = 0U; i < total; ++i)
    {
        count += is_grease(versions ? m.versions[i].major : m.extensions[i].id) ? 1U : 0U;
    }
    return count;
}

// A message a writer produced: decodes, holds its GREASE and selects none of it.
void check_written(ConstSpan<u8> bytes, MessageKind kind, NegotiationMessage& out)
{
    CRD_FUZZ_REQUIRE(decode_checked(bytes, kind, out));
    CRD_FUZZ_REQUIRE(count_grease(out, true) == kGreaseVersions);
    CRD_FUZZ_REQUIRE(count_grease(out, false) == kGreaseExtensions);
    CRD_FUZZ_REQUIRE(!is_grease(out.selected.major));
}

GreaseEntropy entropy_from(const u8* data, usize size, usize& at)
{
    GreaseEntropy entropy;
    for (usize i = 0U; i < kGreaseEntropyBytes && at < size; ++i)
    {
        entropy.bytes[i] = data[at++];
    }
    return entropy;
}

// The answer an honest server writes: every offered extension it knows, valued by its identifier.
struct Answered
{
    u8                   bytes[kMaxMessageBytes] = {};
    usize                size                    = 0U;
    u8                   values[kMaxExtensions][2] = {};
    ExtensionEntry       accepted[kMaxExtensions];
    NegotiationSelection selection;
};

NegotiationError honest_answer(const NegotiationPolicy& server, const NegotiationMessage& offer,
                               const GreaseEntropy& grease, Answered& out)
{
    const NegotiationError selected = select_negotiation(server, offer, out.selection);
    if (selected != NegotiationError::None)
    {
        return selected;
    }
    for (u32 i = 0U; i < out.selection.offered_count; ++i)
    {
        const u16 id     = out.selection.offered[i].id;
        out.values[i][0] = static_cast<u8>(id >> 8U);
        out.values[i][1] = static_cast<u8>(id);
        out.accepted[i]  = ExtensionEntry{id, ConstSpan<u8>(out.values[i], 2U)};
    }
    return write_answer(server, out.selection, ConstSpan<ExtensionEntry>(out.accepted, out.selection.offered_count),
                        grease, Span<u8>(out.bytes, kMaxMessageBytes), out.size);
}

// The client side of an honest exchange: the answer is accepted with the server's version and values, and the
// transcript of the two messages has its exact size.
void check_client(const NegotiationPolicy& client, ConstSpan<u8> offer_bytes, const NegotiationMessage& offer,
                  const Answered& server)
{
    NegotiationMessage answered;
    check_written(ConstSpan<u8>(server.bytes, server.size), MessageKind::Answer, answered);
    NegotiationResult result;
    CRD_FUZZ_REQUIRE(verify_answer(client, offer, answered, result) == NegotiationError::None);
    CRD_FUZZ_REQUIRE(result.version.major == server.selection.version.major &&
                     result.version.minor == server.selection.version.minor);
    CRD_FUZZ_REQUIRE(result.extension_count == server.selection.offered_count);
    for (u32 i = 0U; i < result.extension_count; ++i)
    {
        CRD_FUZZ_REQUIRE(result.extensions[i].id == server.accepted[i].id);
        CRD_FUZZ_REQUIRE(result.extensions[i].value.size() == 2U &&
                         std::memcmp(result.extensions[i].value.data(), server.values[i], 2U) == 0);
    }
    static u8           transcript[transcript_size(kMaxMessageBytes, kMaxMessageBytes)];
    usize               written = 0U;
    const ConstSpan<u8> answer_bytes(server.bytes, server.size);
    CRD_FUZZ_REQUIRE(write_transcript(offer_bytes, answer_bytes, transcript, written) == NegotiationError::None);
    CRD_FUZZ_REQUIRE(written == transcript_size(offer_bytes.size(), answer_bytes.size()));
}

const ProtocolVersion kServerVersions[] = {{4U, 2U}, {2U, 5U}, {1U, 0U}};
const u16             kServerKnown[]    = {0x0001U, 0x0100U, 0x0200U, 0x0300U};
const ProtocolVersion kClientVersions[] = {{3U, 1U}, {2U, 9U}, {1U, 4U}};
const u16             kClientKnown[]    = {0x0100U, 0x0200U, 0x0777U};

NegotiationPolicy fixed_server()
{
    return NegotiationPolicy{ProtocolId::Game, kServerVersions, kServerKnown};
}

NegotiationPolicy fixed_client()
{
    return NegotiationPolicy{ProtocolId::Game, kClientVersions, kClientKnown};
}

// Mode 0: an offer from anyone. A client that could have sent it (its versions and extensions, GREASE aside)
// accepts the honest answer.
void receive_offer(const u8* data, usize size)
{
    NegotiationMessage offer;
    if (!decode_checked(ConstSpan<u8>(data, size), MessageKind::Offer, offer))
    {
        return;
    }
    static Answered server;
    server = Answered{};
    usize at = 0U;
    const NegotiationError error = honest_answer(fixed_server(), offer, entropy_from(data, size, at), server);
    if (error != NegotiationError::None)
    {
        CRD_FUZZ_REQUIRE(error == NegotiationError::WrongProtocol || error == NegotiationError::NoCommonVersion);
        return;
    }
    ProtocolVersion versions[kMaxVersions];
    u16             known[kMaxExtensions];
    u32             version_count = 0U;
    u32             known_count   = 0U;
    for (u32 i = 0U; i < offer.version_count; ++i)
    {
        if (!is_grease(offer.versions[i].major))
        {
            versions[version_count++] = offer.versions[i];
        }
    }
    for (u32 i = 0U; i < offer.extension_count; ++i)
    {
        if (!is_grease(offer.extensions[i].id))
        {
            known[known_count++] = offer.extensions[i].id;
        }
    }
    const NegotiationPolicy client = {static_cast<ProtocolId>(offer.protocol),
                                      ConstSpan<ProtocolVersion>(versions, version_count),
                                      ConstSpan<u16>(known, known_count)};
    if (validate_policy(client) == NegotiationError::None)
    {
        check_client(client, ConstSpan<u8>(data, size), offer, server);
    }
}

// The fixed client's verdict on an answer, restated from the contract.
NegotiationError expected_verdict(const NegotiationMessage& offer, const NegotiationMessage& answer)
{
    if (answer.protocol != static_cast<u16>(ProtocolId::Game))
    {
        return NegotiationError::WrongProtocol;
    }
    const ProtocolVersion s = answer.selected;
    if (is_grease(s.major))
    {
        return NegotiationError::GreaseSelected;
    }
    const ProtocolVersion* offered   = nullptr;
    const ProtocolVersion* available = nullptr;
    u16                    common    = 0U;
    for (u32 i = 0U; i < offer.version_count; ++i)
    {
        offered = offer.versions[i].major == s.major ? &offer.versions[i] : offered;
        for (u32 j = 0U; j < answer.version_count; ++j)
        {
            const u16 major = answer.versions[j].major;
            if (major == offer.versions[i].major && !is_grease(major) && major > common)
            {
                common = major;
            }
        }
    }
    for (u32 j = 0U; j < answer.version_count; ++j)
    {
        available = answer.versions[j].major == s.major ? &answer.versions[j] : available;
    }
    if (offered == nullptr || s.minor > offered->minor)
    {
        return NegotiationError::UnofferedVersion;
    }
    if (available == nullptr || s.minor != (offered->minor < available->minor ? offered->minor : available->minor))
    {
        return NegotiationError::InconsistentAnswer;
    }
    if (common != s.major)
    {
        return NegotiationError::Downgrade;
    }
    for (u32 i = 0U; i < answer.extension_count; ++i)
    {
        const u16 id      = answer.extensions[i].id;
        bool      in_offer = false;
        for (u32 j = 0U; j < offer.extension_count; ++j)
        {
            in_offer = in_offer || (offer.extensions[j].id == id && !is_grease(id));
        }
        bool known = false;
        for (const u16 k : kClientKnown)
        {
            known = known || k == id;
        }
        if (!is_grease(id) && !in_offer && known)
        {
            return NegotiationError::UnsolicitedExtension;
        }
    }
    return NegotiationError::None;
}

// Mode 1: an answer from anyone, to the fixed client's offer.
void receive_answer(const u8* data, usize size)
{
    static u8          offer_bytes[kMaxMessageBytes];
    usize              offer_size = 0U;
    const ExtensionEntry requested[] = {{0x0100U, {}}, {0x0777U, {}}};
    CRD_FUZZ_REQUIRE(write_offer(fixed_client(), requested, GreaseEntropy{}, offer_bytes, offer_size) ==
                     NegotiationError::None);
    NegotiationMessage offer;
    check_written(ConstSpan<u8>(offer_bytes, offer_size), MessageKind::Offer, offer);
    NegotiationMessage answered;
    if (!decode_checked(ConstSpan<u8>(data, size), MessageKind::Answer, answered))
    {
        return;
    }
    NegotiationResult      result;
    const NegotiationError verdict = verify_answer(fixed_client(), offer, answered, result);
    CRD_FUZZ_REQUIRE(verdict == expected_verdict(offer, answered));
    if (verdict == NegotiationError::None)
    {
        CRD_FUZZ_REQUIRE(result.version.major == answered.selected.major);
        for (u32 i = 0U; i < result.extension_count; ++i)
        {
            CRD_FUZZ_REQUIRE(result.extensions[i].id == 0x0100U || result.extensions[i].id == 0x0777U);
        }
    }
}

// Mode 2: two policies from the bytes and the whole exchange.
struct Generated
{
    ProtocolVersion versions[kMaxVersions];
    u16             known[kMaxExtensions];
    NegotiationPolicy policy;
};

void read_policy(const u8* data, usize size, usize& at, Generated& g)
{
    const auto next = [&]() -> u8
    {
        return at < size ? data[at++] : static_cast<u8>(0U);
    };
    const u32 versions = next() % kMaxVersions;
    const u32 known    = next() % kMaxExtensions;
    for (u32 i = 0U; i < versions; ++i)
    {
        g.versions[i] = ProtocolVersion{static_cast<u16>(next() % 6U), static_cast<u16>(next() % 4U)};
    }
    for (u32 i = 0U; i < known; ++i)
    {
        g.known[i] = static_cast<u16>(0x0100U + next() % 40U);
    }
    g.policy = NegotiationPolicy{ProtocolId::Agent, ConstSpan<ProtocolVersion>(g.versions, versions),
                                 ConstSpan<u16>(g.known, known)};
}

void generated_exchange(const u8* data, usize size)
{
    usize     at = 0U;
    Generated client;
    Generated server;
    read_policy(data, size, at, client);
    read_policy(data, size, at, server);
    const GreaseEntropy offer_grease  = entropy_from(data, size, at);
    const GreaseEntropy answer_grease = entropy_from(data, size, at);
    const bool          valid = validate_policy(client.policy) == NegotiationError::None &&
                       validate_policy(server.policy) == NegotiationError::None;

    // The client requests every known extension with a one-byte value.
    u8             values[kMaxExtensions] = {};
    ExtensionEntry requested[kMaxExtensions];
    for (usize i = 0U; i < client.policy.known_extensions.size(); ++i)
    {
        values[i]    = static_cast<u8>(i);
        requested[i] = ExtensionEntry{client.policy.known_extensions[i], ConstSpan<u8>(values + i, 1U)};
    }
    static u8 offer_bytes[kMaxMessageBytes];
    usize     offer_size = 0U;
    const NegotiationError written =
        write_offer(client.policy, ConstSpan<ExtensionEntry>(requested, client.policy.known_extensions.size()),
                    offer_grease, offer_bytes, offer_size);
    if (validate_policy(client.policy) != NegotiationError::None)
    {
        CRD_FUZZ_REQUIRE(written == NegotiationError::BadPolicy);
        return;
    }
    CRD_FUZZ_REQUIRE(written == NegotiationError::None);
    NegotiationMessage offer;
    check_written(ConstSpan<u8>(offer_bytes, offer_size), MessageKind::Offer, offer);

    u16 common = 0U;
    u16 minor  = 0U;
    for (const ProtocolVersion& c : client.policy.versions)
    {
        for (const ProtocolVersion& s : server.policy.versions)
        {
            if (c.major == s.major && c.major > common)
            {
                common = c.major;
                minor  = c.minor < s.minor ? c.minor : s.minor;
            }
        }
    }
    static Answered answered;
    answered = Answered{};
    const NegotiationError error = honest_answer(server.policy, offer, answer_grease, answered);
    if (!valid)
    {
        CRD_FUZZ_REQUIRE(error == NegotiationError::BadPolicy);
        return;
    }
    if (common == 0U)
    {
        CRD_FUZZ_REQUIRE(error == NegotiationError::NoCommonVersion);
        return;
    }
    CRD_FUZZ_REQUIRE(error == NegotiationError::None);
    CRD_FUZZ_REQUIRE(answered.selection.version.major == common && answered.selection.version.minor == minor);
    check_client(client.policy, ConstSpan<u8>(offer_bytes, offer_size), offer, answered);
}

void add(crd::fuzz::SeedSink& sink, const char* name, u8 mode, const u8* data, usize size)
{
    static u8 input[kMaxMessageBytes + 1U];
    input[0] = mode;
    std::memcpy(input + 1U, data, size);
    sink.add(name, input, size + 1U);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    switch (data[0] % 3U)
    {
    case 0U:
        receive_offer(data + 1U, size - 1U);
        break;
    case 1U:
        receive_answer(data + 1U, size - 1U);
        break;
    default:
        generated_exchange(data + 1U, size - 1U);
        break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Honest messages from the writers, their truncations and single-byte changes.
    static u8      offer[kMaxMessageBytes];
    usize          offer_size = 0U;
    const ExtensionEntry requested[] = {{0x0100U, {}}, {0x0777U, {}}};
    GreaseEntropy  grease;
    for (usize i = 0U; i < kGreaseEntropyBytes; ++i)
    {
        grease.bytes[i] = static_cast<u8>(i * 29U + 3U);
    }
    if (write_offer(fixed_client(), requested, grease, offer, offer_size) != NegotiationError::None)
    {
        return;
    }
    add(sink, "offer", 0U, offer, offer_size);
    add(sink, "offer-truncated", 0U, offer, offer_size - 3U);
    static u8 changed[kMaxMessageBytes];
    std::memcpy(changed, offer, offer_size);
    changed[4] = 0U; // no versions
    add(sink, "offer-no-versions", 0U, changed, offer_size);
    std::memcpy(changed, offer, offer_size);
    changed[1] = 9U; // a future format
    add(sink, "offer-future-format", 0U, changed, offer_size);
    std::memcpy(changed, offer, offer_size);
    changed[3] = static_cast<u8>(ProtocolId::Project);
    add(sink, "offer-other-protocol", 0U, changed, offer_size);
    add(sink, "offer-trailing", 0U, offer, offer_size + 1U);

    NegotiationMessage decoded;
    if (decode_negotiation(ConstSpan<u8>(offer, offer_size), MessageKind::Offer, decoded) != NegotiationError::None)
    {
        return;
    }
    static Answered server;
    if (honest_answer(fixed_server(), decoded, grease, server) != NegotiationError::None)
    {
        return;
    }
    add(sink, "answer", 1U, server.bytes, server.size);
    add(sink, "answer-truncated", 1U, server.bytes, server.size - 1U);
    std::memcpy(changed, server.bytes, server.size);
    changed[5] = 1U; // select major 1 at the server's minor, above the offer's
    add(sink, "answer-lower-major", 1U, changed, server.size);
    changed[7] = 0U; // select 1.0, consistent with the server's 1.0 but below the common major 2: a downgrade
    add(sink, "answer-downgrade", 1U, changed, server.size);
    std::memcpy(changed, server.bytes, server.size);
    changed[4] = 0x3AU; // select a GREASE major
    changed[5] = 0x3AU;
    add(sink, "answer-grease-selected", 1U, changed, server.size);

    // Hand-made: an answer accepting 0x0200, which the client knows but did not offer, and an offer repeating 0x0100.
    const u8 unsolicited[] = {2U, kNegotiationFormat, 0U, 2U, 0U, 2U, 0U, 5U, 3U, 0U, 4U, 0U, 2U, 0U, 2U, 0U, 5U,
                              0U, 1U, 0U, 0U, 1U, 2U, 0U, 0U, 0U};
    add(sink, "answer-unsolicited", 1U, unsolicited, sizeof(unsolicited));
    const u8 repeated[] = {1U, kNegotiationFormat, 0U, 2U, 1U, 0U, 2U, 0U, 1U, 2U, 1U, 0U, 0U, 0U, 1U, 0U, 0U, 1U,
                           0x41U};
    add(sink, "offer-repeated-extension", 0U, repeated, sizeof(repeated));

    // Generated exchanges: sharing a major, sharing none, and invalid policies.
    const u8 shared[] = {3U, 2U, 2U, 1U, 1U, 0U, 4U, 1U, 2U, 3U, 2U, 2U, 4U, 3U, 2U, 2U, 3U, 5U};
    add(sink, "exchange-shared", 2U, shared, sizeof(shared));
    const u8 disjoint[] = {1U, 0U, 1U, 0U, 1U, 0U, 2U, 0U};
    add(sink, "exchange-disjoint", 2U, disjoint, sizeof(disjoint));
    const u8 invalid[] = {2U, 0U, 3U, 0U, 3U, 1U};
    add(sink, "exchange-invalid", 2U, invalid, sizeof(invalid));
}
