// crd-net -- version and extension negotiation (NET-STD; docs/design/network-foundation.md#negotiation): the
// offer and answer round trip, GREASE in every message, ignore-unknown in both directions, downgrade detection,
// transcript binding, every decoder bound and every refusal.

#include <crd/net/negotiation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <initializer_list>

namespace
{
using crd::u16;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::net::decode_negotiation;
using crd::net::ExtensionEntry;
using crd::net::GreaseEntropy;
using crd::net::is_grease;
using crd::net::kGreaseEntropyBytes;
using crd::net::kGreaseExtensions;
using crd::net::kGreaseMaxValueBytes;
using crd::net::kGreaseVersions;
using crd::net::kMaxExtensionBytes;
using crd::net::kMaxExtensions;
using crd::net::kMaxMessageBytes;
using crd::net::kMaxVersions;
using crd::net::kNegotiationFormat;
using crd::net::kNegotiationFormatOldest;
using crd::net::MessageKind;
using crd::net::negotiation_error_name;
using crd::net::NegotiationError;
using crd::net::NegotiationMessage;
using crd::net::NegotiationPolicy;
using crd::net::NegotiationResult;
using crd::net::NegotiationSelection;
using crd::net::ProtocolId;
using crd::net::ProtocolVersion;
using crd::net::select_negotiation;
using crd::net::transcript_size;
using crd::net::validate_policy;
using crd::net::verify_answer;
using crd::net::write_answer;
using crd::net::write_offer;
using crd::net::write_transcript;

// One encoded message, or a hand-made one (big-endian fields, the codec's layout).
struct Wire
{
    u8    bytes[kMaxMessageBytes + 8U] = {};
    usize size                         = 0U;

    [[nodiscard]] ConstSpan<u8> view() const { return ConstSpan<u8>(bytes, size); }
    [[nodiscard]] Span<u8>      space() { return Span<u8>(bytes, kMaxMessageBytes); }

    Wire& byte(u32 value)
    {
        bytes[size++] = static_cast<u8>(value);
        return *this;
    }

    Wire& word(u32 value)
    {
        return byte(value >> 8U).byte(value & 0xFFU);
    }

    Wire& version(u16 major, u16 minor)
    {
        return word(major).word(minor);
    }

    Wire& extension(u16 id, const char* value)
    {
        const usize length = std::strlen(value);
        word(id).word(static_cast<u32>(length));
        for (usize i = 0U; i < length; ++i)
        {
            byte(static_cast<u8>(value[i]));
        }
        return *this;
    }
};

ConstSpan<u8> text(const char* value)
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(value), std::strlen(value));
}

bool same(ConstSpan<u8> bytes, const char* value)
{
    return bytes.size() == std::strlen(value) && (bytes.empty() || std::memcmp(bytes.data(), value, bytes.size()) == 0);
}

// Deterministic, varied GREASE entropy; the transport takes it from the CSPRNG.
GreaseEntropy entropy(u32 seed)
{
    GreaseEntropy result;
    for (u32 i = 0U; i < kGreaseEntropyBytes; ++i)
    {
        result.bytes[i] = static_cast<u8>(((seed * (2U * i + 1U)) >> (i % 3U)) ^ (i * 37U));
    }
    return result;
}

constexpr u16 kAlpha   = 0x0100U; // both sides know it
constexpr u16 kBeta    = 0x0200U; // both sides know it
constexpr u16 kClient  = 0x0777U; // only the client knows it
constexpr u16 kServer  = 0x0300U; // only the server knows it
constexpr u16 kUnknown = 0x0999U; // nobody knows it

const ProtocolVersion kClientVersions[] = {{2U, 3U}, {1U, 7U}};
const u16             kClientKnown[]    = {kAlpha, kBeta, kClient};
const ProtocolVersion kServerVersions[] = {{3U, 0U}, {2U, 1U}, {1U, 9U}};
const u16             kServerKnown[]    = {kAlpha, kServer, kBeta};

NegotiationPolicy client_policy()
{
    return NegotiationPolicy{ProtocolId::Game, kClientVersions, kClientKnown};
}

NegotiationPolicy server_policy()
{
    return NegotiationPolicy{ProtocolId::Game, kServerVersions, kServerKnown};
}

const ExtensionEntry kRequested[] = {{kAlpha, text("alpha")}, {kClient, text("c")}, {kBeta, text("")}};

u32 grease_versions(const NegotiationMessage& message)
{
    u32 count = 0U;
    for (u32 i = 0U; i < message.version_count; ++i)
    {
        count += is_grease(message.versions[i].major) ? 1U : 0U;
    }
    return count;
}

u32 grease_extensions(const NegotiationMessage& message)
{
    u32 count = 0U;
    for (u32 i = 0U; i < message.extension_count; ++i)
    {
        count += is_grease(message.extensions[i].id) ? 1U : 0U;
    }
    return count;
}

// The whole exchange over the wire: the client's offer, the server's selection and answer (accepting what it was
// offered with value "s:<id>"), the client's check.
struct Exchange
{
    Wire                 offer;
    Wire                 answer;
    NegotiationMessage   sent;
    NegotiationMessage   received;
    NegotiationSelection selection;
    NegotiationMessage   answered;
    NegotiationResult    result;
};

NegotiationError exchange(Exchange& x, u32 seed, ConstSpan<ExtensionEntry> requested = kRequested)
{
    NegotiationError error = write_offer(client_policy(), requested, entropy(seed), x.offer.space(), x.offer.size);
    if (error == NegotiationError::None)
    {
        error = decode_negotiation(x.offer.view(), MessageKind::Offer, x.sent);
    }
    if (error == NegotiationError::None)
    {
        error = decode_negotiation(x.offer.view(), MessageKind::Offer, x.received);
    }
    if (error == NegotiationError::None)
    {
        error = select_negotiation(server_policy(), x.received, x.selection);
    }
    ExtensionEntry accepted[kMaxExtensions];
    for (u32 i = 0U; i < x.selection.offered_count; ++i)
    {
        accepted[i] = ExtensionEntry{x.selection.offered[i].id, text("s")};
    }
    if (error == NegotiationError::None)
    {
        const ConstSpan<ExtensionEntry> accepting(accepted, x.selection.offered_count);
        error = write_answer(server_policy(), x.selection, accepting, entropy(seed + 7919U), x.answer.space(),
                             x.answer.size);
    }
    if (error == NegotiationError::None)
    {
        error = decode_negotiation(x.answer.view(), MessageKind::Answer, x.answered);
    }
    if (error == NegotiationError::None)
    {
        error = verify_answer(client_policy(), x.sent, x.answered, x.result);
    }
    return error;
}

// A valid hand-made answer to the client's offer of kClientVersions, which tests then alter.
Wire answer_wire(u16 major, u16 minor)
{
    Wire wire;
    wire.byte(2U).byte(kNegotiationFormat).word(static_cast<u16>(ProtocolId::Game)).version(major, minor);
    wire.byte(3U).version(3U, 0U).version(2U, 1U).version(1U, 9U);
    return wire;
}
} // namespace

TEST_CASE("Negotiation: the highest common major with the lower minor, and the known extensions", "[net][net-std]")
{
    Exchange x;
    REQUIRE(exchange(x, 1U) == NegotiationError::None);
    CHECK(x.selection.version.major == 2U);
    CHECK(x.selection.version.minor == 1U);
    CHECK(x.result.version.major == 2U);
    CHECK(x.result.version.minor == 1U);

    // The server kept alpha and beta (offer order) and dropped the client-only extension and GREASE.
    REQUIRE(x.selection.offered_count == 2U);
    CHECK(x.selection.offered[0].id == kAlpha);
    CHECK(same(x.selection.offered[0].value, "alpha"));
    CHECK(x.selection.offered[1].id == kBeta);
    CHECK(x.selection.offered[1].value.empty());

    // The client reads the server's values for exactly those.
    REQUIRE(x.result.extension_count == 2U);
    CHECK(x.result.extensions[0].id == kAlpha);
    CHECK(same(x.result.extensions[0].value, "s"));
    CHECK(x.result.extensions[1].id == kBeta);

    // The answer lists the server's available versions for the downgrade check.
    CHECK(x.answered.selected.major == 2U);
    CHECK(x.answered.version_count == 3U + kGreaseVersions);
    CHECK(x.answered.protocol == static_cast<u16>(ProtocolId::Game));
}

TEST_CASE("Negotiation: minors are compatible, so the lower of the two is selected", "[net][net-std]")
{
    const ProtocolVersion older[]   = {{2U, 0U}};
    const ProtocolVersion newer[]   = {{2U, 9U}};
    const NegotiationPolicy client  = {ProtocolId::Game, older, {}};
    const NegotiationPolicy server  = {ProtocolId::Game, newer, {}};
    Wire                    offer;
    REQUIRE(write_offer(client, {}, entropy(3U), offer.space(), offer.size) == NegotiationError::None);
    NegotiationMessage decoded;
    REQUIRE(decode_negotiation(offer.view(), MessageKind::Offer, decoded) == NegotiationError::None);
    NegotiationSelection selection;
    REQUIRE(select_negotiation(server, decoded, selection) == NegotiationError::None);
    CHECK(selection.version.major == 2U);
    CHECK(selection.version.minor == 0U);
    CHECK(selection.offered_count == 0U);
}

TEST_CASE("Negotiation: every message carries one GREASE version and two distinct GREASE extensions",
          "[net][net-std][grease]")
{
    u32  version_slots[kMaxVersions]     = {};
    u32  extension_slots[kMaxExtensions] = {};
    u32  lengths[kGreaseMaxValueBytes + 1U] = {};
    bool ids[16]                         = {};
    for (u32 seed = 0U; seed < 4096U; ++seed)
    {
        Exchange x;
        REQUIRE(exchange(x, seed) == NegotiationError::None);
        for (const NegotiationMessage* message : {&x.sent, &x.answered})
        {
            REQUIRE(grease_versions(*message) == kGreaseVersions);
            REQUIRE(grease_extensions(*message) == kGreaseExtensions);
        }
        // The real entries keep their order around the GREASE ones.
        u32 real = 0U;
        for (u32 i = 0U; i < x.sent.version_count; ++i)
        {
            if (is_grease(x.sent.versions[i].major))
            {
                ++version_slots[i];
                continue;
            }
            CHECK(x.sent.versions[i].major == kClientVersions[real].major);
            CHECK(x.sent.versions[i].minor == kClientVersions[real].minor);
            ++real;
        }
        u16 first = 0U;
        real      = 0U;
        for (u32 i = 0U; i < x.sent.extension_count; ++i)
        {
            const ExtensionEntry& extension = x.sent.extensions[i];
            if (!is_grease(extension.id))
            {
                CHECK(extension.id == kRequested[real++].id);
                continue;
            }
            CHECK(extension.id != first);
            first = extension.id;
            ++extension_slots[i];
            ++lengths[extension.value.size()];
            ids[(extension.id >> 12U) & 0x0FU] = true;
        }
    }
    // Over the seeds every slot, every value length and every GREASE value occurs.
    for (u32 slot = 0U; slot < 2U + kGreaseVersions; ++slot)
    {
        CHECK(version_slots[slot] > 0U);
    }
    for (u32 slot = 0U; slot < 3U + kGreaseExtensions; ++slot)
    {
        CHECK(extension_slots[slot] > 0U);
    }
    for (const u32 count : lengths)
    {
        CHECK(count > 0U);
    }
    for (const bool seen : ids)
    {
        CHECK(seen);
    }
}

TEST_CASE("Negotiation: the same entropy writes the same bytes", "[net][net-std][grease]")
{
    Exchange a;
    Exchange b;
    REQUIRE(exchange(a, 42U) == NegotiationError::None);
    REQUIRE(exchange(b, 42U) == NegotiationError::None);
    REQUIRE(a.offer.size == b.offer.size);
    CHECK(std::memcmp(a.offer.bytes, b.offer.bytes, a.offer.size) == 0);
    REQUIRE(a.answer.size == b.answer.size);
    CHECK(std::memcmp(a.answer.bytes, b.answer.bytes, a.answer.size) == 0);
}

TEST_CASE("Negotiation: unknown extensions and versions are ignored in both directions", "[net][net-std]")
{
    // An offer from a newer client: an unknown major, an unknown extension and the shared ones.
    Wire offer;
    offer.byte(1U).byte(kNegotiationFormat).word(static_cast<u16>(ProtocolId::Game));
    offer.byte(3U).version(9U, 4U).version(2U, 3U).version(1U, 7U);
    offer.byte(3U).extension(kUnknown, "future").extension(kAlpha, "a").extension(0x1A1AU, "");
    NegotiationMessage received;
    REQUIRE(decode_negotiation(offer.view(), MessageKind::Offer, received) == NegotiationError::None);
    NegotiationSelection selection;
    REQUIRE(select_negotiation(server_policy(), received, selection) == NegotiationError::None);
    CHECK(selection.version.major == 2U);
    REQUIRE(selection.offered_count == 1U);
    CHECK(selection.offered[0].id == kAlpha);

    // An answer from a newer server: an extension the client does not know, a GREASE one and an accepted one.
    Wire sent;
    REQUIRE(write_offer(client_policy(), kRequested, entropy(5U), sent.space(), sent.size) == NegotiationError::None);
    NegotiationMessage offered;
    REQUIRE(decode_negotiation(sent.view(), MessageKind::Offer, offered) == NegotiationError::None);
    Wire answer = answer_wire(2U, 1U);
    answer.byte(3U).extension(kUnknown, "x").extension(0x2A2AU, "g").extension(kBeta, "b");
    NegotiationMessage answered;
    REQUIRE(decode_negotiation(answer.view(), MessageKind::Answer, answered) == NegotiationError::None);
    NegotiationResult result;
    REQUIRE(verify_answer(client_policy(), offered, answered, result) == NegotiationError::None);
    REQUIRE(result.extension_count == 1U);
    CHECK(result.extensions[0].id == kBeta);
    CHECK(same(result.extensions[0].value, "b"));
}

TEST_CASE("Negotiation: a stripped offer is detected as a downgrade, and the transcripts differ",
          "[net][net-std][downgrade]")
{
    Exchange x;
    REQUIRE(exchange(x, 11U) == NegotiationError::None);

    // A middlebox removes major 2 from the offer in flight; the server sees only major 1 and selects it.
    Wire stripped;
    stripped.byte(1U).byte(kNegotiationFormat).word(static_cast<u16>(ProtocolId::Game));
    stripped.byte(1U).version(1U, 7U).byte(0U);
    NegotiationMessage received;
    REQUIRE(decode_negotiation(stripped.view(), MessageKind::Offer, received) == NegotiationError::None);
    NegotiationSelection selection;
    REQUIRE(select_negotiation(server_policy(), received, selection) == NegotiationError::None);
    CHECK(selection.version.major == 1U);
    Wire answer;
    REQUIRE(write_answer(server_policy(), selection, {}, entropy(12U), answer.space(), answer.size) ==
            NegotiationError::None);
    NegotiationMessage answered;
    REQUIRE(decode_negotiation(answer.view(), MessageKind::Answer, answered) == NegotiationError::None);

    // The client checks the answer against what it really sent: both speak 2, so selecting 1 is a downgrade.
    NegotiationResult result;
    CHECK(verify_answer(client_policy(), x.sent, answered, result) == NegotiationError::Downgrade);

    // And the transcripts the handshake binds differ, so a tampered answer would fail there too.
    u8    client_side[transcript_size(kMaxMessageBytes, kMaxMessageBytes)];
    u8    server_side[transcript_size(kMaxMessageBytes, kMaxMessageBytes)];
    usize client_size = 0U;
    usize server_size = 0U;
    REQUIRE(write_transcript(x.offer.view(), answer.view(), client_side, client_size) == NegotiationError::None);
    REQUIRE(write_transcript(stripped.view(), answer.view(), server_side, server_size) == NegotiationError::None);
    CHECK((client_size != server_size || std::memcmp(client_side, server_side, client_size) != 0));
}

TEST_CASE("Negotiation: the transcript is the label and both messages with their lengths", "[net][net-std]")
{
    Exchange x;
    REQUIRE(exchange(x, 21U) == NegotiationError::None);
    u8    client_side[transcript_size(kMaxMessageBytes, kMaxMessageBytes)];
    u8    server_side[transcript_size(kMaxMessageBytes, kMaxMessageBytes)];
    usize client_size = 0U;
    usize server_size = 0U;
    REQUIRE(write_transcript(x.offer.view(), x.answer.view(), client_side, client_size) == NegotiationError::None);
    REQUIRE(write_transcript(x.offer.view(), x.answer.view(), server_side, server_size) == NegotiationError::None);
    REQUIRE(client_size == transcript_size(x.offer.size, x.answer.size));
    CHECK(client_size == server_size);
    CHECK(std::memcmp(client_side, server_side, client_size) == 0);

    CHECK(std::memcmp(client_side, "crdneg01", 8U) == 0);
    const usize offer_size = (static_cast<usize>(client_side[8]) << 24U) | (static_cast<usize>(client_side[9]) << 16U) |
                             (static_cast<usize>(client_side[10]) << 8U) | client_side[11];
    CHECK(offer_size == x.offer.size);
    CHECK(std::memcmp(client_side + 12U, x.offer.bytes, x.offer.size) == 0);
    const u8*   tail        = client_side + 12U + x.offer.size;
    const usize answer_size = (static_cast<usize>(tail[0]) << 24U) | (static_cast<usize>(tail[1]) << 16U) |
                              (static_cast<usize>(tail[2]) << 8U) | tail[3];
    CHECK(answer_size == x.answer.size);
    CHECK(std::memcmp(tail + 4U, x.answer.bytes, x.answer.size) == 0);

    // Moving a byte from the offer into the answer keeps the concatenation but not the transcript.
    usize moved_size = 0U;
    u8    moved[transcript_size(kMaxMessageBytes, kMaxMessageBytes)];
    REQUIRE(write_transcript(x.offer.view().first(x.offer.size - 1U),
                             ConstSpan<u8>(x.offer.bytes + x.offer.size - 1U, 1U), moved, moved_size) ==
            NegotiationError::None);
    CHECK(std::memcmp(moved + 8U, client_side + 8U, 4U) != 0);

    usize written = 99U;
    CHECK(write_transcript(x.offer.view(), x.answer.view(), Span<u8>(client_side, client_size - 1U), written) ==
          NegotiationError::BufferTooSmall);
    CHECK(written == 0U);
    u8 huge[kMaxMessageBytes + 1U] = {};
    CHECK(write_transcript(huge, x.answer.view(), client_side, written) == NegotiationError::TooLarge);
}

TEST_CASE("Negotiation: decoding refuses every malformed message", "[net][net-std][decode]")
{
    Exchange x;
    REQUIRE(exchange(x, 31U) == NegotiationError::None);
    NegotiationMessage message;

    SECTION("every proper prefix is truncated and an extra byte trails")
    {
        for (usize size = 0U; size < x.offer.size; ++size)
        {
            CHECK(decode_negotiation(x.offer.view().first(size), MessageKind::Offer, message) ==
                  NegotiationError::Truncated);
        }
        for (usize size = 0U; size < x.answer.size; ++size)
        {
            CHECK(decode_negotiation(x.answer.view().first(size), MessageKind::Answer, message) ==
                  NegotiationError::Truncated);
        }
        x.offer.byte(0U);
        CHECK(decode_negotiation(x.offer.view(), MessageKind::Offer, message) == NegotiationError::TrailingBytes);
    }
    SECTION("the kind and the format")
    {
        CHECK(decode_negotiation(x.offer.view(), MessageKind::Answer, message) == NegotiationError::WrongKind);
        CHECK(decode_negotiation(x.answer.view(), MessageKind::Offer, message) == NegotiationError::WrongKind);
        x.offer.bytes[0] = 7U;
        CHECK(decode_negotiation(x.offer.view(), MessageKind::Offer, message) == NegotiationError::WrongKind);
        x.offer.bytes[0] = 1U;
        for (u32 format = 0U; format < 256U; ++format)
        {
            x.offer.bytes[1]       = static_cast<u8>(format);
            const bool readable    = format >= kNegotiationFormatOldest && format <= kNegotiationFormat;
            const auto expectation = readable ? NegotiationError::None : NegotiationError::UnsupportedFormat;
            CHECK(decode_negotiation(x.offer.view(), MessageKind::Offer, message) == expectation);
        }
    }
    SECTION("version lists: empty, above the bound, a repeated major")
    {
        Wire empty;
        empty.byte(1U).byte(kNegotiationFormat).word(2U).byte(0U).byte(0U);
        CHECK(decode_negotiation(empty.view(), MessageKind::Offer, message) == NegotiationError::NoVersions);
        Wire many;
        many.byte(1U).byte(kNegotiationFormat).word(2U).byte(kMaxVersions + 1U);
        for (u32 i = 0U; i <= kMaxVersions; ++i)
        {
            many.version(static_cast<u16>(i + 1U), 0U);
        }
        many.byte(0U);
        CHECK(decode_negotiation(many.view(), MessageKind::Offer, message) == NegotiationError::TooLarge);
        Wire full;
        full.byte(1U).byte(kNegotiationFormat).word(2U).byte(kMaxVersions);
        for (u32 i = 0U; i < kMaxVersions; ++i)
        {
            full.version(static_cast<u16>(i + 1U), 0U);
        }
        full.byte(0U);
        CHECK(decode_negotiation(full.view(), MessageKind::Offer, message) == NegotiationError::None);
        CHECK(message.version_count == kMaxVersions);
        Wire repeated;
        repeated.byte(1U).byte(kNegotiationFormat).word(2U).byte(2U).version(2U, 0U).version(2U, 1U).byte(0U);
        CHECK(decode_negotiation(repeated.view(), MessageKind::Offer, message) == NegotiationError::DuplicateVersion);
    }
    SECTION("extension lists: above the bound, a value above its bound, a repeated identifier")
    {
        Wire head;
        head.byte(1U).byte(kNegotiationFormat).word(2U).byte(1U).version(1U, 0U);
        Wire many = head;
        many.byte(kMaxExtensions + 1U);
        for (u32 i = 0U; i <= kMaxExtensions; ++i)
        {
            many.extension(static_cast<u16>(i + 1U), "");
        }
        CHECK(decode_negotiation(many.view(), MessageKind::Offer, message) == NegotiationError::TooLarge);
        Wire full = head;
        full.byte(kMaxExtensions);
        for (u32 i = 0U; i < kMaxExtensions; ++i)
        {
            full.extension(static_cast<u16>(i + 1U), "v");
        }
        CHECK(decode_negotiation(full.view(), MessageKind::Offer, message) == NegotiationError::None);
        CHECK(message.extension_count == kMaxExtensions);
        Wire large = head;
        large.byte(1U).word(5U).word(kMaxExtensionBytes + 1U);
        for (u32 i = 0U; i <= kMaxExtensionBytes; ++i)
        {
            large.byte(0U);
        }
        CHECK(decode_negotiation(large.view(), MessageKind::Offer, message) == NegotiationError::TooLarge);
        Wire longest = head;
        longest.byte(1U).word(5U).word(kMaxExtensionBytes);
        for (u32 i = 0U; i < kMaxExtensionBytes; ++i)
        {
            longest.byte(0U);
        }
        CHECK(decode_negotiation(longest.view(), MessageKind::Offer, message) == NegotiationError::None);
        Wire repeated = head;
        repeated.byte(2U).extension(0x1A1AU, "").extension(0x1A1AU, "x");
        CHECK(decode_negotiation(repeated.view(), MessageKind::Offer, message) ==
              NegotiationError::DuplicateExtension);
        Wire claims = head;
        claims.byte(1U).word(5U).word(9U).byte(1U);
        CHECK(decode_negotiation(claims.view(), MessageKind::Offer, message) == NegotiationError::Truncated);
    }
    SECTION("a message above the size bound")
    {
        u8 huge[kMaxMessageBytes + 1U] = {1U, kNegotiationFormat};
        CHECK(decode_negotiation(huge, MessageKind::Offer, message) == NegotiationError::TooLarge);
    }
}

TEST_CASE("Negotiation: a policy breaking its rules is refused", "[net][net-std]")
{
    const ProtocolVersion zero[]      = {{0U, 1U}};
    const ProtocolVersion grease[]    = {{0x3A3AU, 0U}};
    const ProtocolVersion twice[]     = {{2U, 0U}, {2U, 1U}};
    const ProtocolVersion too_many[]  = {{1U, 0U}, {2U, 0U}, {3U, 0U}, {4U, 0U},
                                         {5U, 0U}, {6U, 0U}, {7U, 0U}, {8U, 0U}};
    const ProtocolVersion most[]      = {{1U, 0U}, {2U, 0U}, {3U, 0U}, {4U, 0U}, {5U, 0U}, {6U, 0U}, {7U, 0U}};
    const u16             grease_id[] = {0x0A0AU};
    const u16             repeated[]  = {kAlpha, kAlpha};
    u16                   known[kMaxExtensions - kGreaseExtensions + 1U];
    for (u32 i = 0U; i < kMaxExtensions - kGreaseExtensions + 1U; ++i)
    {
        known[i] = static_cast<u16>(i + 1U);
    }
    const auto check = [](NegotiationPolicy policy, NegotiationError expected)
    {
        CHECK(validate_policy(policy) == expected);
    };
    check({ProtocolId::Game, kClientVersions, kClientKnown}, NegotiationError::None);
    check({ProtocolId::Game, {}, {}}, NegotiationError::BadPolicy);
    check({ProtocolId::Game, zero, {}}, NegotiationError::BadPolicy);
    check({ProtocolId::Game, grease, {}}, NegotiationError::BadPolicy);
    check({ProtocolId::Game, twice, {}}, NegotiationError::BadPolicy);
    check({ProtocolId::Game, too_many, {}}, NegotiationError::BadPolicy);
    check({ProtocolId::Game, most, {}}, NegotiationError::None);
    check({ProtocolId::Game, kClientVersions, grease_id}, NegotiationError::BadPolicy);
    check({ProtocolId::Game, kClientVersions, repeated}, NegotiationError::BadPolicy);
    check({ProtocolId::Game, kClientVersions, ConstSpan<u16>(known, kMaxExtensions - kGreaseExtensions)},
          NegotiationError::None);
    check({ProtocolId::Game, kClientVersions, known}, NegotiationError::BadPolicy);
    check({static_cast<ProtocolId>(0x0042U), kClientVersions, {}}, NegotiationError::BadPolicy);

    // Every entry point refuses a bad policy before reading anything else.
    const NegotiationPolicy bad = {ProtocolId::Game, zero, {}};
    Wire                    wire;
    CHECK(write_offer(bad, {}, entropy(1U), wire.space(), wire.size) == NegotiationError::BadPolicy);
    NegotiationMessage   message;
    NegotiationSelection selection;
    NegotiationResult    result;
    CHECK(select_negotiation(bad, message, selection) == NegotiationError::BadPolicy);
    CHECK(write_answer(bad, selection, {}, entropy(1U), wire.space(), wire.size) == NegotiationError::BadPolicy);
    CHECK(verify_answer(bad, message, message, result) == NegotiationError::BadPolicy);
}

TEST_CASE("Negotiation: writing an offer refuses bad requests and short buffers", "[net][net-std]")
{
    Wire wire;
    const ExtensionEntry unknown[] = {{kServer, text("")}};
    CHECK(write_offer(client_policy(), unknown, entropy(1U), wire.space(), wire.size) == NegotiationError::BadPolicy);
    const ExtensionEntry twice[] = {{kAlpha, text("1")}, {kAlpha, text("2")}};
    CHECK(write_offer(client_policy(), twice, entropy(1U), wire.space(), wire.size) ==
          NegotiationError::DuplicateExtension);
    u8                   big[kMaxExtensionBytes + 1U] = {};
    const ExtensionEntry large[]                      = {{kAlpha, ConstSpan<u8>(big, kMaxExtensionBytes + 1U)}};
    CHECK(write_offer(client_policy(), large, entropy(1U), wire.space(), wire.size) == NegotiationError::TooLarge);

    // Five values of 1 KiB pass their own bound but not the message's.
    u16            ids[5];
    ExtensionEntry heavy[5];
    for (u32 i = 0U; i < 5U; ++i)
    {
        ids[i]   = static_cast<u16>(0x0400U + i);
        heavy[i] = ExtensionEntry{ids[i], ConstSpan<u8>(big, kMaxExtensionBytes)};
    }
    const NegotiationPolicy wide = {ProtocolId::Game, kClientVersions, ids};
    CHECK(write_offer(wide, heavy, entropy(1U), wire.space(), wire.size) == NegotiationError::TooLarge);
    CHECK(write_offer(wide, ConstSpan<ExtensionEntry>(heavy, 3U), entropy(1U), wire.space(), wire.size) ==
          NegotiationError::None);

    // Every buffer shorter than the message is refused with nothing written; the exact size succeeds.
    usize needed = 0U;
    REQUIRE(write_offer(client_policy(), kRequested, entropy(9U), wire.space(), needed) == NegotiationError::None);
    for (usize size = 0U; size < needed; ++size)
    {
        usize written = 99U;
        CHECK(write_offer(client_policy(), kRequested, entropy(9U), Span<u8>(wire.bytes, size), written) ==
              NegotiationError::BufferTooSmall);
        CHECK(written == 0U);
    }
    usize written = 0U;
    CHECK(write_offer(client_policy(), kRequested, entropy(9U), Span<u8>(wire.bytes, needed), written) ==
          NegotiationError::None);
    CHECK(written == needed);
}

TEST_CASE("Negotiation: the server refuses another protocol, no common major and a non-offer", "[net][net-std]")
{
    Exchange x;
    REQUIRE(exchange(x, 41U) == NegotiationError::None);
    NegotiationSelection    selection;
    const NegotiationPolicy project = {ProtocolId::Project, kServerVersions, kServerKnown};
    CHECK(select_negotiation(project, x.received, selection) == NegotiationError::WrongProtocol);
    const ProtocolVersion   later[] = {{5U, 0U}};
    const NegotiationPolicy ahead   = {ProtocolId::Game, later, {}};
    CHECK(select_negotiation(ahead, x.received, selection) == NegotiationError::NoCommonVersion);
    CHECK(select_negotiation(server_policy(), x.answered, selection) == NegotiationError::WrongKind);

    // An offer of nothing but GREASE majors has no common version either.
    Wire only;
    only.byte(1U).byte(kNegotiationFormat).word(static_cast<u16>(ProtocolId::Game));
    only.byte(1U).version(0x0A0AU, 0U).byte(0U);
    NegotiationMessage offer;
    REQUIRE(decode_negotiation(only.view(), MessageKind::Offer, offer) == NegotiationError::None);
    CHECK(select_negotiation(server_policy(), offer, selection) == NegotiationError::NoCommonVersion);
}

TEST_CASE("Negotiation: writing an answer refuses what was not offered and a foreign selection", "[net][net-std]")
{
    Exchange x;
    REQUIRE(exchange(x, 51U) == NegotiationError::None);
    Wire                 wire;
    const ExtensionEntry unsolicited[] = {{kServer, text("")}};
    CHECK(write_answer(server_policy(), x.selection, unsolicited, entropy(1U), wire.space(), wire.size) ==
          NegotiationError::UnsolicitedExtension);
    const ExtensionEntry twice[] = {{kAlpha, text("")}, {kAlpha, text("")}};
    CHECK(write_answer(server_policy(), x.selection, twice, entropy(1U), wire.space(), wire.size) ==
          NegotiationError::DuplicateExtension);
    NegotiationSelection foreign = x.selection;
    foreign.version              = ProtocolVersion{7U, 0U};
    CHECK(write_answer(server_policy(), foreign, {}, entropy(1U), wire.space(), wire.size) ==
          NegotiationError::BadPolicy);
    foreign.version = ProtocolVersion{2U, 2U}; // above the server's own minor
    CHECK(write_answer(server_policy(), foreign, {}, entropy(1U), wire.space(), wire.size) ==
          NegotiationError::BadPolicy);
    usize needed = 0U;
    REQUIRE(write_answer(server_policy(), x.selection, {}, entropy(1U), wire.space(), needed) ==
            NegotiationError::None);
    usize written = 99U;
    CHECK(write_answer(server_policy(), x.selection, {}, entropy(1U), Span<u8>(wire.bytes, needed - 1U), written) ==
          NegotiationError::BufferTooSmall);
    CHECK(written == 0U);
}

TEST_CASE("Negotiation: the client refuses every answer that does not follow from its offer",
          "[net][net-std][downgrade]")
{
    Exchange x;
    REQUIRE(exchange(x, 61U) == NegotiationError::None);
    NegotiationResult  result;
    NegotiationMessage answered;
    const auto verdict = [&](const Wire& answer)
    {
        REQUIRE(decode_negotiation(answer.view(), MessageKind::Answer, answered) == NegotiationError::None);
        return verify_answer(client_policy(), x.sent, answered, result);
    };

    Wire honest = answer_wire(2U, 1U);
    honest.byte(0U);
    CHECK(verdict(honest) == NegotiationError::None);

    SECTION("a GREASE major is never selected")
    {
        Wire wire;
        wire.byte(2U).byte(kNegotiationFormat).word(static_cast<u16>(ProtocolId::Game)).version(0x5A5AU, 0U);
        wire.byte(2U).version(0x5A5AU, 0U).version(2U, 1U).byte(0U);
        CHECK(verdict(wire) == NegotiationError::GreaseSelected);
    }
    SECTION("a major the offer did not hold, or a minor above the offer's")
    {
        Wire three = answer_wire(3U, 0U);
        three.byte(0U);
        CHECK(verdict(three) == NegotiationError::UnofferedVersion);
        Wire minor = answer_wire(2U, 4U);
        minor.byte(0U);
        CHECK(verdict(minor) == NegotiationError::UnofferedVersion);
    }
    SECTION("a selection its own available versions contradict")
    {
        Wire absent;
        absent.byte(2U).byte(kNegotiationFormat).word(static_cast<u16>(ProtocolId::Game)).version(1U, 7U);
        absent.byte(1U).version(3U, 0U).byte(0U);
        CHECK(verdict(absent) == NegotiationError::InconsistentAnswer);
        Wire low = answer_wire(2U, 0U); // the server lists 2.1, the client offered 2.3: the selection is 2.1
        low.byte(0U);
        CHECK(verdict(low) == NegotiationError::InconsistentAnswer);
    }
    SECTION("a lower major while both sides speak a higher one")
    {
        Wire lower = answer_wire(1U, 7U);
        lower.byte(0U);
        CHECK(verdict(lower) == NegotiationError::Downgrade);
    }
    SECTION("an extension the client knows but did not offer")
    {
        Wire wire = answer_wire(2U, 1U);
        wire.byte(1U).extension(kClient, "");
        Exchange bare;
        REQUIRE(exchange(bare, 62U, ConstSpan<ExtensionEntry>(kRequested, 1U)) == NegotiationError::None);
        REQUIRE(decode_negotiation(wire.view(), MessageKind::Answer, answered) == NegotiationError::None);
        CHECK(verify_answer(client_policy(), bare.sent, answered, result) == NegotiationError::UnsolicitedExtension);
    }
    SECTION("another protocol and swapped messages")
    {
        Wire other = answer_wire(2U, 1U);
        other.byte(0U);
        other.bytes[3] = static_cast<u8>(ProtocolId::Project);
        CHECK(verdict(other) == NegotiationError::WrongProtocol);
        CHECK(verify_answer(client_policy(), x.answered, x.answered, result) == NegotiationError::WrongKind);
        CHECK(verify_answer(client_policy(), x.sent, x.sent, result) == NegotiationError::WrongKind);
        const NegotiationPolicy project = {ProtocolId::Project, kClientVersions, kClientKnown};
        CHECK(verify_answer(project, x.sent, x.answered, result) == NegotiationError::WrongProtocol);
    }
}

TEST_CASE("Negotiation: every error has its own name", "[net][net-std]")
{
    const auto last = static_cast<u32>(NegotiationError::UnsolicitedExtension);
    for (u32 i = 0U; i <= last; ++i)
    {
        const auto name = negotiation_error_name(static_cast<NegotiationError>(i));
        CHECK(name != "unknown");
        for (u32 j = 0U; j < i; ++j)
        {
            CHECK(negotiation_error_name(static_cast<NegotiationError>(j)) != name);
        }
    }
    CHECK(negotiation_error_name(NegotiationError::Downgrade) == "downgrade");
    CHECK(negotiation_error_name(static_cast<NegotiationError>(last + 1U)) == "unknown");
}
