// crd-net -- the plaintext integrity hash and padding on frames (NET.3, threats T12 and T15;
// docs/design/network-foundation.md#integrity-hash and #padding), and stepping over what a newer writer added.

#include "admission_fixture.hpp"

#include <crd/containers/checksum.hpp>
#include <crd/net/reassembly.hpp>
#include <crd/net/wire/session_control.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <generated/delta_test.hpp>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::crypto::AuthorizedSchema;
using crd::crypto::Capability;
using crd::crypto::Delivery;
using crd::crypto::ScopeKind;
using crd::net::Admission;
using crd::net::AdmitAction;
using crd::net::AdmitVerdict;
using crd::net::ChannelState;
using crd::net::DatagramAssembler;
using crd::net::DatagramCursor;
using crd::net::FragmentSlot;
using crd::net::frame_hash;
using crd::net::frame_shape;
using crd::net::FrameHeader;
using crd::net::FrameRead;
using crd::net::FrameShape;
using crd::net::FrameStep;
using crd::net::HeaderForm;
using crd::net::kFrameHashBytes;
using crd::net::kNanosPerSecond;
using crd::net::pad_datagram;
using crd::net::padded_size;
using crd::net::PrincipalQuota;
using crd::net::QuotaLimits;
using crd::net::read_route;
using crd::net::Route;
using crd::net::RouteForm;
using crd::net::SessionSchema;
using crd::net::StreamAssembler;
using crd::net::write_fragment;
using crd::net::write_frame;
using crd::net::write_frame_header;
using crd::net::write_route;
using crd::net::write_stream_padding;
using crd::net::test::authorized_for;
using crd::net::test::Buffers;
using crd::net::test::capability_for;
using crd::net::wire::ChannelClass;
using crd::net::wire::ChannelDesc;
using crd::net::wire::MessageDesc;
using crd::net::wire::WireError;
using crd::net::wire::WireSchema;
namespace control = crd::net::wire::session_control;

constexpr u64 kWide = 1ULL << 40U;

constexpr ChannelDesc kChannels[] = {
    {"reliable", ChannelClass::Event, true, false, false, 2U, 90000U, 1U << 30U, kWide, 64U, true},
    {"grams", ChannelClass::Input, false, false, false, 2U, 90000U, 1U << 30U, kWide, 64U, true},
    {"media", ChannelClass::Media, false, false, false, 5U, 90000U, 1U << 30U, kWide, 0U, false},
    {"plain", ChannelClass::Event, true, false, false, 2U, 90000U, 1U << 30U, kWide, 0U, true},
    {"narrow", ChannelClass::Event, true, false, false, 2U, 90000U, 1U << 30U, 100U, 64U, true},
    {"tight", ChannelClass::Input, false, false, false, 2U, 90000U, 1U << 30U, 100U, 64U, true},
    {"urgent", ChannelClass::Control, true, true, false, 0U, 90000U, 1U << 30U, kWide, 64U, true},
    {"next", ChannelClass::Input, false, false, false, 1U, 90000U, 1U << 30U, kWide, 0U, true},
};
constexpr u32 kReliable = 0U;
constexpr u32 kGrams = 1U;
constexpr u32 kMedia = 2U;
constexpr u32 kPlain = 3U;
constexpr u32 kNarrow = 4U;
constexpr u32 kTight = 5U;
constexpr u32 kUrgent = 6U;
constexpr u32 kNext = 7U;
constexpr u32 kChannelCount = 8U;

constexpr MessageDesc kMessages[] = {
    {"Note", 1U, 0U, 0U, 1ULL, 0U, 90000U},  {"Gram", 2U, 1U, 0U, 1ULL, 0U, 90000U},
    {"Voice", 3U, 2U, 0U, 1ULL, 0U, 90000U}, {"Plain", 4U, 3U, 0U, 1ULL, 0U, 90000U},
    {"Pinch", 5U, 4U, 0U, 1ULL, 0U, 90000U}, {"Squeezed", 6U, 5U, 0U, 1ULL, 0U, 90000U},
};
constexpr MessageDesc kNote = kMessages[0];
constexpr MessageDesc kGram = kMessages[1];
constexpr MessageDesc kVoice = kMessages[2];
constexpr MessageDesc kSqueezed = kMessages[5];

// Budgets wide open. With no credit, a narrow gate closes after one charge.
constexpr QuotaLimits open_limits(u64 tolerance_ns = kNanosPerSecond)
{
    QuotaLimits limits{1000U, kWide, kWide, kWide, kWide, 1U << 24U};
    limits.tolerance_ns = tolerance_ns;
    return limits;
}

constexpr WireSchema kSchema{
    "integrity",
    {0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U, 0x29U, 0x2AU, 0x2BU, 0x2CU, 0x2DU, 0x2EU, 0x2FU, 0x30U},
    static_cast<u8>(ScopeKind::World),
    1U,
    1U,
    1ULL,
    0ULL,
    kChannels,
    kMessages,
};

// A session whose budgets are open, unless a test narrows them. `writer_minor` above the schema's makes a newer writer.
struct Session
{
    explicit Session(u16 writer_minor = 1U, u64 tolerance_ns = kNanosPerSecond, u64 principal_bytes = kWide)
        : quota(open_limits(tolerance_ns)), admission(quota, buffers, channels)
    {
        quota.limits.bytes_per_s = principal_bytes;
        authorized = authorized_for(kSchema);
        capability = capability_for(authorized);
        const SessionSchema schemas[] = {{&kSchema, &authorized}};
        REQUIRE(admission.bind(schemas));
        REQUIRE(admission.announce(ConstSpan<u8>(kSchema.id, sizeof(kSchema.id)), kSchema.major, writer_minor));
        admission.set_capability(&capability);
    }

    void require_clear() const
    {
        CHECK(quota.pending.held() == 0U);
        CHECK(quota.reassembly.held() == 0U);
        CHECK(buffers.live == 0U);
    }

    Buffers buffers;
    PrincipalQuota quota;
    ChannelState channels[16];
    Admission admission;
    AuthorizedSchema authorized;
    Capability capability;
};

// A Sized frame with a right hash over a pattern, appended at `at`.
usize put_sealed(Span<u8> out, usize at, const MessageDesc& message, u64 object, u32 length, u8 fill = 0x5AU)
{
    const FrameShape shape = frame_shape(kChannels[message.channel], message);
    FrameHeader header;
    header.message = message.id;
    header.length = length;
    header.object = object;
    const usize used = write_frame_header(HeaderForm::Sized, shape, header, out.subspan(at));
    REQUIRE(used != 0U);
    std::memset(out.data() + at + used, fill, length);
    header.hash = frame_hash(message.id, object, ConstSpan<u8>(out.data() + at + used, length));
    REQUIRE(write_frame_header(HeaderForm::Sized, shape, header, out.subspan(at)) == used);
    return at + used + length;
}

usize put_preamble(Span<u8> out, u32 channel)
{
    const usize used = write_route(Route{channel, RouteForm::Whole}, out);
    REQUIRE(used != 0U);
    return used;
}

// One Whole datagram with a right hash over a pattern.
usize put_gram(Span<u8> out, u32 channel, const MessageDesc& message, u64 object, u32 length, u8 fill = 0x5AU)
{
    const FrameShape shape = frame_shape(kChannels[message.channel], message);
    const usize route = write_route(Route{channel, RouteForm::Whole}, out);
    FrameHeader header;
    header.message = message.id;
    header.object = object;
    const usize used = write_frame_header(HeaderForm::ToEnd, shape, header, out.subspan(route));
    std::memset(out.data() + route + used, fill, length);
    header.hash = frame_hash(message.id, object, ConstSpan<u8>(out.data() + route + used, length));
    REQUIRE(write_frame_header(HeaderForm::ToEnd, shape, header, out.subspan(route)) == used);
    return route + used + length;
}
} // namespace

TEST_CASE("Frames: a newer writer's message with an object is stepped over and the stream stays in step",
          "[net][frames][integrity]")
{
    // Its description is unknown here, so its length alone must reach the next frame, past its object and hash.
    Session session(9U);
    u8 bytes[256];
    usize size = put_preamble(bytes, kReliable);
    const FrameShape shape{true, true};
    FrameHeader newer;
    newer.message = 77U;
    newer.length = 5U;
    newer.object = 300U; // two bytes
    const usize head = write_frame_header(HeaderForm::Sized, shape, newer, Span<u8>(bytes + size, 64U));
    CHECK(bytes[size + 1U] == 5U + 2U + kFrameHashBytes);
    size += head;
    std::memset(bytes + size, 0xEE, 5U);
    size += 5U;
    size = put_sealed(bytes, size, kNote, 1U, 40U);

    // Whole, and one byte at a time.
    for (const usize part : {sizeof(bytes), usize{1}})
    {
        Session reader(9U);
        StreamAssembler assembler(reader.admission);
        usize at = 0U;
        u32 delivered = 0U;
        u32 stepped = 0U;
        while (at < size)
        {
            const usize give = (part < size - at) ? part : (size - at);
            const FrameStep step = assembler.feed(ConstSpan<u8>(bytes + at, give), Delivery::Established, 0U);
            at += step.consumed;
            stepped += (step.verdict == AdmitVerdict::NewerMessage) ? 1U : 0U;
            if (step.action == AdmitAction::Deliver)
            {
                ++delivered;
                CHECK(step.frame.message->id == kNote.id);
                CHECK(step.frame.object == 1U);
                CHECK(step.frame.body.size() == 40U);
                reader.admission.finish(step.frame);
            }
            REQUIRE(step.action != AdmitAction::Close);
        }
        CHECK(delivered == 1U);
        CHECK(stepped == 1U);
    }
}

TEST_CASE("Frames: a length shorter than what it counts is malformed", "[net][frames][integrity]")
{
    Session session;
    StreamAssembler assembler(session.admission);
    u8 bytes[32];
    const usize route = put_preamble(bytes, kReliable);
    const FrameStep opened = assembler.feed(ConstSpan<u8>(bytes, route), Delivery::Established, 0U);
    REQUIRE(opened.consumed == route);
    // Message 1, length 2, but an object and a hash follow it.
    const u8 frame[] = {1U, 2U, 7U, 1U, 2U, 3U, 4U, 0U, 0U, 0U};
    const FrameStep step = assembler.feed(frame, Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::Malformed);
    CHECK(step.action == AdmitAction::Close);
    session.require_clear();
}

TEST_CASE("Frames: the header holds the hash, seeded with the message and object", "[net][frames][integrity]")
{
    const u8 body[] = {1U, 2U, 3U};
    const u64 seed = (u64{300U} << 32U) ^ 5U;
    const u32 expected = static_cast<u32>(crd::containers::xxh3_64(ConstSpan<u8>(body, sizeof(body)), seed));
    CHECK(frame_hash(300U, 5U, body) == expected);
    CHECK(frame_hash(301U, 5U, body) != expected);
    CHECK(frame_hash(300U, 6U, body) != expected);

    FrameHeader header;
    header.message = 300U;
    header.length = 3U;
    header.object = 5U;
    header.hash = 0x01020304U;
    header.chunk = 128U;
    header.sequence = 9U;
    header.index = 1U;
    u8 out[64];
    {
        const usize used = write_frame_header(HeaderForm::Sized, FrameShape{true, true}, header, out);
        const u8 known[] = {0xACU, 2U, 8U, 5U, 1U, 2U, 3U, 4U};
        REQUIRE(used == sizeof(known));
        CHECK(std::memcmp(out, known, used) == 0);
    }
    {
        const usize used = write_frame_header(HeaderForm::ToEnd, FrameShape{false, true}, header, out);
        const u8 known[] = {0xACU, 2U, 1U, 2U, 3U, 4U};
        REQUIRE(used == sizeof(known));
        CHECK(std::memcmp(out, known, used) == 0);
    }
    {
        const usize used = write_frame_header(HeaderForm::Fragment, FrameShape{true, true}, header, out);
        const u8 known[] = {0xACU, 2U, 3U, 5U, 1U, 2U, 3U, 4U, 0x80U, 1U, 9U, 1U};
        REQUIRE(used == sizeof(known));
        CHECK(std::memcmp(out, known, used) == 0);
        FrameHeader tail;
        usize consumed = 0U;
        REQUIRE(crd::net::read_frame_tail(ConstSpan<u8>(out + 3U, used - 3U), HeaderForm::Fragment,
                                          FrameShape{true, true}, tail, consumed) == FrameRead::Ok);
        CHECK(consumed == used - 3U);
        CHECK(tail.hash == 0x01020304U);
        CHECK(tail.chunk == 128U);
        for (usize size = 0U; size < used - 3U; ++size)
        {
            CHECK(crd::net::read_frame_tail(ConstSpan<u8>(out + 3U, size), HeaderForm::Fragment, FrameShape{true, true},
                                            tail, consumed) == FrameRead::Short);
        }
    }

    // The shape of a message on its channel: a hash unless the channel turns it off or a delta body has its own.
    CHECK(frame_shape(kChannels[kReliable], kNote).hash);
    CHECK(frame_shape(kChannels[kReliable], kNote).object);
    CHECK_FALSE(frame_shape(kChannels[kMedia], kVoice).hash);
    MessageDesc transport = kNote;
    transport.operation = 0U;
    CHECK_FALSE(frame_shape(kChannels[kReliable], transport).object);
    bool any_delta = false;
    for (const MessageDesc& message : crd::net::wire::delta_test::kSchema.messages)
    {
        any_delta = any_delta || message.delta != nullptr;
        CHECK(frame_shape(kChannels[kReliable], message).hash == (message.delta == nullptr));
    }
    CHECK(any_delta);
}

TEST_CASE("Integrity: generated messages travel hashed, and any flipped bit of a stream's frame is refused",
          "[net][integrity]")
{
    control::StateHash hash;
    hash.step = 123456789U;
    hash.hash = 0x0123456789ABCDEFULL;
    hash.chain = 0xFEDCBA9876543210ULL;
    u8 stream[128];
    usize size = write_route(Route{0U, RouteForm::Whole}, stream);
    const usize frame_at = size;
    usize used = 0U;
    REQUIRE(write_frame(control::kSchema, HeaderForm::Sized, hash, 0U, 0U,
                        Span<u8>(stream + size, sizeof(stream) - size), used) == WireError::Ok);
    size += used;

    const auto read = [&](const u8* bytes, usize part, bool expect_whole)
    {
        Buffers buffers;
        PrincipalQuota quota{open_limits()};
        ChannelState channels[2];
        Admission admission(quota, buffers, channels);
        const SessionSchema schemas[] = {{&control::kSchema, nullptr}};
        REQUIRE(admission.bind(schemas));
        u32 delivered = 0U;
        {
            StreamAssembler assembler(admission);
            usize at = 0U;
            bool closed = false;
            while (at < size && !closed)
            {
                const usize give = (part < size - at) ? part : (size - at);
                const FrameStep step = assembler.feed(ConstSpan<u8>(bytes + at, give), Delivery::Established, 0U);
                at += step.consumed;
                closed = step.action == AdmitAction::Close;
                if (step.action == AdmitAction::Deliver)
                {
                    ++delivered;
                    control::StateHash decoded;
                    CHECK(crd::net::decode_frame(step.frame, decoded) == WireError::Ok);
                    if (expect_whole)
                    {
                        CHECK(decoded.step == hash.step);
                        CHECK(decoded.chain == hash.chain);
                    }
                    admission.finish(step.frame);
                }
            }
        }
        CHECK(quota.pending.held() == 0U);
        CHECK(quota.reassembly.held() == 0U);
        CHECK(buffers.live == 0U);
        return delivered;
    };

    CHECK(read(stream, sizeof(stream), true) == 1U);
    CHECK(read(stream, 1U, true) == 1U);

    // Every bit from the frame's hash on: a flip in the hash or the body is never delivered, whole or in pieces.
    const usize hash_at = frame_at + 2U;
    REQUIRE(stream[frame_at + 1U] == size - hash_at); // no object: the hash follows the length
    for (usize byte = hash_at; byte < size; ++byte)
    {
        for (u32 bit = 0U; bit < 8U; ++bit)
        {
            u8 damaged[sizeof(stream)];
            std::memcpy(damaged, stream, size);
            damaged[byte] = static_cast<u8>(damaged[byte] ^ (1U << bit));
            CHECK(read(damaged, sizeof(stream), false) == 0U);
            CHECK(read(damaged, 3U, false) == 0U);
        }
    }

    // The header's numbers are the hash's seed: the message and object cannot change under it.
    Session session;
    u8 bytes[128];
    const usize route = put_preamble(bytes, kReliable);
    const usize end = put_sealed(bytes, route, kNote, 5U, 8U);
    bytes[route + 2U] = 6U; // the object
    StreamAssembler assembler(session.admission);
    REQUIRE(assembler.feed(ConstSpan<u8>(bytes, route), Delivery::Established, 0U).consumed == route);
    const FrameStep refused = assembler.feed(ConstSpan<u8>(bytes + route, end - route), Delivery::Established, 0U);
    CHECK(refused.verdict == AdmitVerdict::IntegrityFailed);
    CHECK(refused.action == AdmitAction::Close);
    CHECK(session.admission.count(AdmitVerdict::IntegrityFailed) == 1U);
    // A stream that lost a message stays closed.
    CHECK(assembler.feed(ConstSpan<u8>(bytes, 4U), Delivery::Established, 0U).verdict == AdmitVerdict::IntegrityFailed);
}

TEST_CASE("Integrity: a reassembled stream frame is checked when it is whole, and gives back what it held",
          "[net][integrity]")
{
    Session session;
    u8 bytes[4096];
    const usize route = put_preamble(bytes, kReliable);
    const usize end = put_sealed(bytes, route, kNote, 5U, 1000U);
    StreamAssembler assembler(session.admission);
    REQUIRE(assembler.feed(ConstSpan<u8>(bytes, route), Delivery::Established, 0U).consumed == route);

    FrameStep step = assembler.feed(ConstSpan<u8>(bytes + route, 100U), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::NeedMore);
    CHECK(session.buffers.total == 1U);
    CHECK(session.quota.reassembly.held() == 1000U);
    bytes[end - 1U] = static_cast<u8>(bytes[end - 1U] ^ 1U);
    step = assembler.feed(ConstSpan<u8>(bytes + route + 100U, end - route - 100U), Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::IntegrityFailed);
    CHECK(step.action == AdmitAction::Close);
    CHECK(step.frame.buffer == nullptr);
    session.require_clear();
}

TEST_CASE("Integrity: datagrams drop what fails, and a Listed datagram reads on", "[net][integrity]")
{
    Session session;
    FragmentSlot slots[2];
    DatagramAssembler assembler(session.admission, kGrams, slots, kNanosPerSecond);
    u8 bytes[512];
    DatagramCursor cursor;

    // Whole.
    usize size = put_gram(bytes, kGrams, kGram, 5U, 40U);
    REQUIRE(session.admission.open_datagram(ConstSpan<u8>(bytes, size), 0U, cursor) == AdmitVerdict::Admitted);
    FrameStep step = assembler.next(cursor, Delivery::Established, 0U);
    REQUIRE(step.verdict == AdmitVerdict::Admitted);
    CHECK(step.frame.body.data() == bytes + size - 40U); // zero copy
    CHECK(step.frame.length == 40U);
    session.admission.finish(step.frame);
    bytes[size - 1U] = static_cast<u8>(bytes[size - 1U] ^ 0x80U);
    REQUIRE(session.admission.open_datagram(ConstSpan<u8>(bytes, size), 0U, cursor) == AdmitVerdict::Admitted);
    step = assembler.next(cursor, Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::IntegrityFailed);
    CHECK(step.action == AdmitAction::Skip);
    session.require_clear();

    // Listed: the damaged one goes and its neighbour stays, the length having framed it.
    size = write_route(Route{kGrams, RouteForm::Listed}, bytes);
    const usize first_end = put_sealed(bytes, size, kGram, 5U, 30U);
    const usize second_end = put_sealed(bytes, first_end, kGram, 6U, 30U, 0x33U);
    bytes[first_end - 1U] = static_cast<u8>(bytes[first_end - 1U] ^ 1U);
    REQUIRE(session.admission.open_datagram(ConstSpan<u8>(bytes, second_end), 0U, cursor) == AdmitVerdict::Admitted);
    step = assembler.next(cursor, Delivery::Established, 0U);
    CHECK(step.verdict == AdmitVerdict::IntegrityFailed);
    CHECK(cursor.more);
    step = assembler.next(cursor, Delivery::Established, 0U);
    REQUIRE(step.verdict == AdmitVerdict::Admitted);
    CHECK(step.frame.object == 6U);
    session.admission.finish(step.frame);
    CHECK_FALSE(cursor.more);
    session.require_clear();

    // A channel that turned the hash off carries none, and delivers what it got.
    DatagramAssembler media(session.admission, kMedia, Span<FragmentSlot>{}, kNanosPerSecond);
    size = put_gram(bytes, kMedia, kVoice, 5U, 40U);
    CHECK(size == 1U + 1U + 1U + 40U);
    bytes[size - 1U] = static_cast<u8>(bytes[size - 1U] ^ 1U);
    REQUIRE(session.admission.open_datagram(ConstSpan<u8>(bytes, size), 0U, cursor) == AdmitVerdict::Admitted);
    step = media.next(cursor, Delivery::Established, 0U);
    REQUIRE(step.verdict == AdmitVerdict::Admitted);
    session.admission.finish(step.frame);
    session.require_clear();
}

TEST_CASE("Datagrams: a route one past the session's channels, and the principal's datagrams at priority one",
          "[net][integrity]")
{
    Session session(1U, 0U);
    session.quota.limits.datagrams_per_s = 1U;
    u8 bytes[8];
    DatagramCursor cursor;
    REQUIRE(write_route(Route{kChannelCount, RouteForm::Whole}, bytes) == 1U);
    CHECK(session.admission.open_datagram(ConstSpan<u8>(bytes, 1U), 0U, cursor) == AdmitVerdict::BadChannel);
    REQUIRE(write_route(Route{kNext, RouteForm::Whole}, bytes) == 1U);
    CHECK(session.admission.open_datagram(ConstSpan<u8>(bytes, 1U), 0U, cursor) == AdmitVerdict::Admitted);
    CHECK(session.admission.open_datagram(ConstSpan<u8>(bytes, 1U), 0U, cursor) == AdmitVerdict::PrincipalDatagrams);
}

TEST_CASE("Integrity: a fragmented message is checked whole, and its pieces hold to one hash", "[net][integrity]")
{
    constexpr u32 total = 500U;
    constexpr u32 chunk = 200U;
    u8 body[total];
    for (u32 i = 0U; i < total; ++i)
    {
        body[i] = static_cast<u8>(i * 3U);
    }
    FrameHeader header;
    header.message = kGram.id;
    header.length = total;
    header.object = 5U;
    header.chunk = chunk;
    header.sequence = 1U;
    header.hash = frame_hash(kGram.id, 5U, ConstSpan<u8>(body, total));
    const FrameShape shape = frame_shape(kChannels[kGrams], kGram);

    const auto send = [](Session& session, DatagramAssembler& assembler, const FrameHeader& head, ConstSpan<u8> whole,
                         FrameShape frame_shape_of, bool flip)
    {
        u8 datagram[512];
        const usize used = write_fragment(kGrams, frame_shape_of, head, whole, datagram);
        REQUIRE(used != 0U);
        if (flip)
        {
            datagram[used - 1U] = static_cast<u8>(datagram[used - 1U] ^ 4U);
        }
        DatagramCursor cursor;
        REQUIRE(session.admission.open_datagram(ConstSpan<u8>(datagram, used), 0U, cursor) == AdmitVerdict::Admitted);
        return assembler.next(cursor, Delivery::Established, 0U);
    };

    SECTION("whole and right")
    {
        Session session;
        FragmentSlot slots[2];
        DatagramAssembler assembler(session.admission, kGrams, slots, kNanosPerSecond);
        for (u32 index = 0U; index < 2U; ++index)
        {
            header.index = index;
            CHECK(send(session, assembler, header, body, shape, false).verdict == AdmitVerdict::Buffered);
        }
        header.index = 2U;
        const FrameStep step = send(session, assembler, header, body, shape, false);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        CHECK(std::memcmp(step.frame.body.data(), body, total) == 0);
        session.admission.finish(step.frame);
        session.require_clear();
    }

    SECTION("a flipped bit in a piece fails when the message is whole, and gives everything back")
    {
        Session session;
        FragmentSlot slots[2];
        DatagramAssembler assembler(session.admission, kGrams, slots, kNanosPerSecond);
        header.index = 1U;
        CHECK(send(session, assembler, header, body, shape, true).verdict == AdmitVerdict::Buffered);
        header.index = 0U;
        CHECK(send(session, assembler, header, body, shape, false).verdict == AdmitVerdict::Buffered);
        header.index = 2U;
        const FrameStep step = send(session, assembler, header, body, shape, false);
        CHECK(step.verdict == AdmitVerdict::IntegrityFailed);
        CHECK(step.action == AdmitAction::Skip);
        CHECK(session.admission.count(AdmitVerdict::IntegrityFailed) == 1U);
        session.require_clear();
        // It is over: a repeat of its pieces opens nothing.
        CHECK(send(session, assembler, header, body, shape, false).verdict == AdmitVerdict::StaleFragment);
    }

    SECTION("a piece naming another hash conflicts with its message's first")
    {
        Session session;
        FragmentSlot slots[2];
        DatagramAssembler assembler(session.admission, kGrams, slots, kNanosPerSecond);
        header.index = 0U;
        CHECK(send(session, assembler, header, body, shape, false).verdict == AdmitVerdict::Buffered);
        FrameHeader other = header;
        other.index = 1U;
        other.hash = header.hash ^ 1U;
        CHECK(send(session, assembler, other, body, shape, false).verdict == AdmitVerdict::FragmentConflict);
    }
}

TEST_CASE("Frames: a fragment's room, at every size", "[net][frames][integrity]")
{
    u8 body[300];
    std::memset(body, 0x44, sizeof(body));
    FrameHeader header;
    header.message = 300U;
    header.length = sizeof(body);
    header.object = 7U;
    header.chunk = 100U;
    header.sequence = 1U;
    header.hash = frame_hash(300U, 7U, body);
    const FrameShape shape{true, true};
    u8 out[256];
    const usize needed = write_fragment(1U, shape, header, body, out);
    REQUIRE(needed == 1U + 2U + 2U + 1U + 4U + 1U + 1U + 1U + 100U);
    for (usize room = 0U; room < needed; ++room)
    {
        CHECK(write_fragment(1U, shape, header, body, Span<u8>(out, room)) == 0U);
    }
    CHECK(write_fragment(1U, shape, header, body, Span<u8>(out, needed)) == needed);
    header.chunk = 0U;
    CHECK(write_fragment(1U, shape, header, body, out) == 0U);
    header.chunk = 100U;
    header.index = 3U;
    CHECK(write_fragment(1U, shape, header, body, out) == 0U);
}

TEST_CASE("Padding: lengths within a block take one size, and room caps it", "[net][padding]")
{
    CHECK(padded_size(10U, 0U, 100U) == 10U); // no policy
    for (const u32 block : {16U, 64U, 256U})
    {
        CHECK(padded_size(0U, block, 1000U) == 0U);
        CHECK(padded_size(block, block, 1000U) == block);
        for (usize size = 1U; size <= block; ++size)
        {
            CHECK(padded_size(size, block, 1000U) == block);
            CHECK(padded_size(block + size, block, 1000U) == 2U * block);
        }
    }
    // A last block the room cuts short ends at the room; what is at or past it stays.
    CHECK(padded_size(1201U, 64U, 1232U) == 1216U);
    CHECK(padded_size(1217U, 64U, 1232U) == 1232U);
    CHECK(padded_size(1231U, 64U, 1232U) == 1232U);
    CHECK(padded_size(1232U, 64U, 1232U) == 1232U);
    CHECK(padded_size(1250U, 64U, 1232U) == 1250U);
}

TEST_CASE("Padding: a padded datagram reads as the one it wraps, at every size", "[net][padding]")
{
    for (usize length = 0U; length <= 300U; ++length)
    {
        Session session;
        FragmentSlot slots[1];
        DatagramAssembler assembler(session.admission, kGrams, slots, kNanosPerSecond);
        u8 bytes[1200];
        const usize used = put_gram(bytes, kGrams, kGram, 5U, static_cast<u32>(length));
        const usize padded = pad_datagram(kGrams, 64U, used, bytes);
        // The next multiple its own two bytes of header fit before, and nothing past it.
        const usize gap = 64U - (used % 64U);
        CHECK(padded == used + ((used % 64U == 0U) ? 0U : ((gap < 2U) ? (gap + 64U) : gap)));

        DatagramCursor cursor;
        REQUIRE(session.admission.open_datagram(ConstSpan<u8>(bytes, padded), 0U, cursor) == AdmitVerdict::Admitted);
        const FrameStep step = assembler.next(cursor, Delivery::Established, 0U);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        CHECK(step.frame.object == 5U);
        CHECK(step.frame.body.size() == length);
        CHECK(step.frame.body.data() == bytes + padded - length);
        session.admission.finish(step.frame);
        session.require_clear();
    }

    // Already a multiple: left as it is. No room for the padding's own header: left as it is.
    u8 bytes[128];
    std::memset(bytes, 0x11, sizeof(bytes));
    CHECK(pad_datagram(kGrams, 64U, 64U, bytes) == 64U);
    CHECK(pad_datagram(kGrams, 0U, 10U, bytes) == 10U);
    CHECK(pad_datagram(kGrams, 64U, 63U, Span<u8>(bytes, 64U)) == 63U);
    CHECK(bytes[0] == 0x11U);
    // One byte past the multiple is room enough, and no more is taken.
    CHECK(pad_datagram(kGrams, 64U, 63U, Span<u8>(bytes, 65U)) == 65U);
    std::memset(bytes, 0x11, sizeof(bytes));
    // The room caps it, short of a multiple.
    CHECK(pad_datagram(kGrams, 64U, 70U, Span<u8>(bytes, 100U)) == 100U);
    // A gap a number's width jumps past lands on the next multiple.
    u8 wide[1024];
    std::memset(wide, 0, sizeof(wide));
    const usize padded = pad_datagram(kGrams, 256U, 256U - 130U, wide);
    CHECK(padded % 256U == 0U);
    // The largest channel's route still fits its reader.
    Route route;
    usize route_bytes = 0U;
    REQUIRE(read_route(wide, route, route_bytes) == FrameRead::Ok);
    CHECK(route.form == RouteForm::Padded);
    CHECK(route.channel == kGrams);

    // A padded datagram's padding is charged to the channel and the principal as bytes: at 100 bytes per second with no
    // credit, its own frame then waits for what the padding spent, and an unpadded one does not.
    FragmentSlot slots[1];
    {
        Session session(1U, 0U);
        DatagramAssembler assembler(session.admission, kTight, slots, kNanosPerSecond);
        const usize used = put_gram(bytes, kTight, kSqueezed, 5U, 4U);
        DatagramCursor cursor;
        REQUIRE(session.admission.open_datagram(ConstSpan<u8>(bytes, used), 0U, cursor) == AdmitVerdict::Admitted);
        const FrameStep step = assembler.next(cursor, Delivery::Established, 0U);
        REQUIRE(step.verdict == AdmitVerdict::Admitted);
        session.admission.finish(step.frame);
    }
    {
        Session session(1U, 0U);
        DatagramAssembler assembler(session.admission, kTight, slots, kNanosPerSecond);
        const usize used = put_gram(bytes, kTight, kSqueezed, 5U, 4U);
        const usize total = pad_datagram(kTight, 64U, used, bytes);
        DatagramCursor cursor;
        REQUIRE(session.admission.open_datagram(ConstSpan<u8>(bytes, total), 0U, cursor) == AdmitVerdict::Admitted);
        const FrameStep step = assembler.next(cursor, Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::ChannelBytes);
        CHECK(step.retry_at_ns == (total - used) * (kNanosPerSecond / 100U));
    }
}

TEST_CASE("Padding: what a datagram's padding may not be", "[net][padding]")
{
    const auto open = [](ConstSpan<u8> bytes)
    {
        Session session;
        DatagramCursor cursor;
        return session.admission.open_datagram(bytes, 0U, cursor);
    };
    const u8 grams = static_cast<u8>((kGrams * 4U) + 3U);
    const u8 inner = static_cast<u8>(kGrams * 4U);

    const u8 right[] = {grams, 2U, 0U, 0U, inner};
    CHECK(open(right) == AdmitVerdict::Admitted);
    const u8 nothing[] = {grams, 0U, inner};
    CHECK(open(nothing) == AdmitVerdict::Admitted);

    const u8 undeclared[] = {static_cast<u8>((kMedia * 4U) + 3U), 0U, static_cast<u8>(kMedia * 4U)};
    CHECK(open(undeclared) == AdmitVerdict::BadPadding);
    const u8 set_bit[] = {grams, 2U, 0U, 1U, inner};
    CHECK(open(set_bit) == AdmitVerdict::BadPadding);
    const u8 past_end[] = {grams, 5U, 0U, 0U};
    CHECK(open(past_end) == AdmitVerdict::BadPadding);
    const u8 one_past[] = {grams, 3U, 0U, 0U};
    CHECK(open(one_past) == AdmitVerdict::BadPadding);
    const u8 no_count[] = {grams};
    CHECK(open(no_count) == AdmitVerdict::BadPadding);
    const u8 no_route[] = {grams, 1U, 0U};
    CHECK(open(no_route) == AdmitVerdict::BadPadding);
    const u8 twice[] = {grams, 0U, grams, 0U, inner};
    CHECK(open(twice) == AdmitVerdict::BadPadding);
    const u8 other_channel[] = {grams, 0U, static_cast<u8>(kMedia * 4U)};
    CHECK(open(other_channel) == AdmitVerdict::BadPadding);
    // Two blocks is more than a writer ever needs: this channel's block is 64.
    u8 too_many[140];
    std::memset(too_many, 0, sizeof(too_many));
    too_many[0] = grams;
    too_many[1] = 0x80U; // 128, in two bytes
    too_many[2] = 1U;
    too_many[131] = inner;
    CHECK(open(ConstSpan<u8>(too_many, 132U)) == AdmitVerdict::BadPadding);
    too_many[1] = 127U;
    too_many[2] = 0U;
    too_many[129] = inner;
    CHECK(open(ConstSpan<u8>(too_many, 130U)) == AdmitVerdict::Admitted);

    // A stream refuses the form outright.
    Session session;
    StreamAssembler assembler(session.admission);
    const u8 padded_stream[] = {static_cast<u8>((kReliable * 4U) + 3U), 0U};
    CHECK(assembler.feed(padded_stream, Delivery::Established, 0U).verdict == AdmitVerdict::Malformed);
}

TEST_CASE("Padding: a stream's padding frame", "[net][padding]")
{
    SECTION("brings every count of unsent bytes to a multiple, and reads as nothing, at any split")
    {
        for (u64 pending = 0U; pending < 300U; ++pending)
        {
            u8 frame[600];
            std::memset(frame, 0xFF, sizeof(frame));
            const usize written = write_stream_padding(pending, 64U, frame);
            if (pending % 64U == 0U)
            {
                CHECK(written == 0U);
                continue;
            }
            REQUIRE(written != 0U);
            const usize gap = 64U - static_cast<usize>(pending % 64U);
            CHECK((pending + written) % 64U == 0U);
            CHECK(written == ((gap < 2U) ? (gap + 64U) : gap));
            CHECK(frame[written] == 0xFFU); // nothing past its size
            for (const usize part : {sizeof(frame), usize{1}, usize{3}})
            {
                Session session;
                StreamAssembler assembler(session.admission);
                u8 bytes[800];
                usize size = put_preamble(bytes, kReliable);
                std::memcpy(bytes + size, frame, written);
                size += written;
                const usize note_at = size;
                size = put_sealed(bytes, size, kNote, 1U, 4U);
                usize at = 0U;
                u32 delivered = 0U;
                while (at < size)
                {
                    const usize give = (part < size - at) ? part : (size - at);
                    const FrameStep step = assembler.feed(ConstSpan<u8>(bytes + at, give), Delivery::Established, 0U);
                    at += step.consumed;
                    REQUIRE((step.action == AdmitAction::Continue || step.action == AdmitAction::Deliver));
                    if (step.action == AdmitAction::Deliver)
                    {
                        CHECK(at == size);
                        ++delivered;
                        session.admission.finish(step.frame);
                    }
                }
                CHECK(delivered == 1U);
                CHECK(note_at == 1U + written);
                CHECK(session.buffers.total == ((part == sizeof(frame)) ? 0U : 1U));
                CHECK(session.admission.count(AdmitVerdict::PaddingFrame) >= 1U);
            }
        }
    }

    SECTION("needs its channel to declare it, and is all zero, and bounded")
    {
        const auto first_refusal = [](u32 channel, ConstSpan<u8> frame, usize part)
        {
            Session session;
            StreamAssembler assembler(session.admission);
            u8 route[8];
            const usize used = put_preamble(route, channel);
            REQUIRE(assembler.feed(ConstSpan<u8>(route, used), Delivery::Established, 0U).consumed == used);
            usize at = 0U;
            while (at < frame.size())
            {
                const usize give = (part < frame.size() - at) ? part : (frame.size() - at);
                const FrameStep step = assembler.feed(frame.subspan(at, give), Delivery::Established, 0U);
                at += step.consumed;
                if (step.action == AdmitAction::Close)
                {
                    return step.verdict;
                }
            }
            return AdmitVerdict::NeedMore;
        };
        const u8 zeros[] = {0U, 3U, 0U, 0U, 0U};
        CHECK(first_refusal(kReliable, zeros, 64U) == AdmitVerdict::NeedMore);
        CHECK(first_refusal(kPlain, zeros, 64U) == AdmitVerdict::UnknownMessage);
        const u8 set_bit[] = {0U, 3U, 0U, 2U, 0U};
        CHECK(first_refusal(kReliable, set_bit, 64U) == AdmitVerdict::BadPadding);
        CHECK(first_refusal(kReliable, set_bit, 1U) == AdmitVerdict::BadPadding);
        const u8 too_long[] = {0U, 0x80U, 1U}; // 128: two blocks
        CHECK(first_refusal(kReliable, too_long, 64U) == AdmitVerdict::BadPadding);
    }

    SECTION("is charged, and a closed gate holds the reader back with nothing consumed")
    {
        Session session(1U, 0U);
        StreamAssembler assembler(session.admission);
        u8 bytes[128];
        const usize route = put_preamble(bytes, kNarrow);
        REQUIRE(assembler.feed(ConstSpan<u8>(bytes, route), Delivery::Established, 0U).consumed == route);
        const usize first = write_stream_padding(1U, 64U, Span<u8>(bytes, sizeof(bytes)));
        REQUIRE(first == 63U);
        FrameStep step = assembler.feed(ConstSpan<u8>(bytes, first), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::PaddingFrame);
        CHECK(step.consumed == first);
        // 63 bytes at 100 per second: its gate is closed now, and open after.
        step = assembler.feed(ConstSpan<u8>(bytes, first), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::ChannelBytes);
        CHECK(step.action == AdmitAction::Wait);
        CHECK(step.consumed == 0U);
        CHECK(step.retry_at_ns > 0U);
        step = assembler.feed(ConstSpan<u8>(bytes, first), Delivery::Established, step.retry_at_ns);
        CHECK(step.verdict == AdmitVerdict::PaddingFrame);
    }

    SECTION("the principal's closed gate holds it back, unless its channel has priority zero")
    {
        for (const u32 channel : {kReliable, kUrgent})
        {
            Session session(1U, 0U, 100U);
            StreamAssembler assembler(session.admission);
            u8 bytes[128];
            const usize route = put_preamble(bytes, channel);
            REQUIRE(assembler.feed(ConstSpan<u8>(bytes, route), Delivery::Established, 0U).consumed == route);
            const usize first = write_stream_padding(1U, 64U, bytes);
            CHECK(assembler.feed(ConstSpan<u8>(bytes, first), Delivery::Established, 0U).verdict ==
                  AdmitVerdict::PaddingFrame);
            // A wide gate takes a nanosecond to reopen; the principal's narrow one far longer.
            CHECK(assembler.feed(ConstSpan<u8>(bytes, first), Delivery::Established, 1000U).verdict ==
                  ((channel == kUrgent) ? AdmitVerdict::PaddingFrame : AdmitVerdict::PrincipalBytes));
        }
    }

    SECTION("no room leaves the stream unpadded, and exactly enough does not")
    {
        u8 frame[64];
        CHECK(write_stream_padding(1U, 64U, Span<u8>(frame, 8U)) == 0U);
        CHECK(write_stream_padding(1U, 64U, Span<u8>(frame, 62U)) == 0U);
        CHECK(write_stream_padding(1U, 64U, Span<u8>(frame, 63U)) == 63U);
        CHECK(write_stream_padding(1U, 0U, frame) == 0U);
    }

    SECTION("a block wide enough to need a number of three bytes")
    {
        static u8 frame[40000];
        const usize written = write_stream_padding(5U, 40000U, frame);
        CHECK(written == 40000U - 5U);
        CHECK(frame[0] == 0U);
        // 39995 less the message and its number of three bytes.
        CHECK(frame[1] == ((39991U & 0x7FU) | 0x80U));
        // A gap a number's width jumps past takes the next block.
        CHECK(write_stream_padding(256U - 130U, 256U, frame) == 130U + 256U);
    }
}

TEST_CASE("Padding: session control declares its block", "[net][padding]")
{
    CHECK(control::kSchema.channels[0].padding_block == 128U);
    CHECK(control::kSchema.channels[0].integrity);
    CHECK(padded_size(1U, control::kSchema.channels[0].padding_block, 1232U) == 128U);
}
