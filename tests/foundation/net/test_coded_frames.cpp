// Coded bodies (NET.3, network foundation S32): a channel's models through frames, admission and deltas, what a coded
// frame costs, and every cap held before anything is reserved.

#include "admission_fixture.hpp"
#include "coding_fixture.hpp"

#include <crd/containers/checksum.hpp>
#include <crd/net/reassembly.hpp>

#include <catch2/catch_test_macros.hpp>
#include <random>

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
using crd::net::AdmittedFrame;
using crd::net::AdmitVerdict;
using crd::net::BaselineStore;
using crd::net::BodyCoding;
using crd::net::BodyModel;
using crd::net::ChannelState;
using crd::net::DatagramAssembler;
using crd::net::DatagramCursor;
using crd::net::decode_frame;
using crd::net::DeltaRead;
using crd::net::DeltaReader;
using crd::net::DeltaWriter;
using crd::net::expansion_holds;
using crd::net::FragmentSlot;
using crd::net::frame_hash;
using crd::net::frame_shape;
using crd::net::FrameHeader;
using crd::net::FrameShape;
using crd::net::FrameStep;
using crd::net::HeaderForm;
using crd::net::kDeltaHeaderMinBytes;
using crd::net::kMaxFrameHeaderBytes;
using crd::net::kNanosPerSecond;
using crd::net::kNeverNs;
using crd::net::PrincipalQuota;
using crd::net::QuotaLimits;
using crd::net::read_frame_number;
using crd::net::read_frame_tail;
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
using crd::net::test::Bytes;
using crd::net::test::bytes_of;
using crd::net::test::capability_for;
using crd::net::test::coded_models;
using crd::net::test::CodedModels;
using crd::net::test::common_note;
using crd::net::test::kCodedEvent;
using crd::net::test::kCodedState;
using crd::net::test::kCoderMessageBytes;
using crd::net::test::SimulatedWorld;
using crd::net::wire::ChannelClass;
using crd::net::wire::ChannelDesc;
using crd::net::wire::CodecDesc;
using crd::net::wire::CodecKind;
using crd::net::wire::MessageDesc;
using crd::net::wire::WireError;
using crd::net::wire::WireSchema;

namespace coding = crd::net::wire::coding_test;

namespace
{
constexpr u64 kObject = 77U;

// One session over the generated schema, its peer writing at `writer_minor`.
struct CodedSession
{
    explicit CodedSession(u16 writer_minor = coding::kMinor, const QuotaLimits& limits = QuotaLimits{})
        : quota(limits), admission(quota, buffers, channels)
    {
        authorized = authorized_for(coding::kSchema);
        capability = capability_for(authorized);
        const CodedModels& models = coded_models();
        const SessionSchema schemas[] = {
            {&coding::kSchema, &authorized, ConstSpan<const BodyModel*>(models.list), &coded_models().coder}};
        REQUIRE(admission.bind(schemas));
        REQUIRE(admission.announce(ConstSpan<u8>(coding::kSchema.id, sizeof(coding::kSchema.id)), coding::kMajor,
                                   writer_minor));
        admission.set_capability(&capability);
    }

    void check_all_given_back() const
    {
        CHECK(buffers.live == 0U);
        CHECK(quota.reassembly.held() == 0U);
        CHECK(quota.pending.held() == 0U);
    }

    Buffers buffers;
    PrincipalQuota quota;
    ChannelState channels[4];
    Admission admission;
    AuthorizedSchema authorized;
    Capability capability;
};

// Notes over and over, to `size` characters: at most kTextBytes.
constexpr usize kTextBytes = 8100U;

[[nodiscard]] std::string_view spaced_text(usize size)
{
    static char text[kTextBytes];
    static const bool kFilled = []
    {
        const std::string_view note = common_note();
        for (usize at = 0U; at < kTextBytes; ++at)
        {
            text[at] = note[at % note.size()];
        }
        return true;
    }();
    static_cast<void>(kFilled);
    return std::string_view(text, size);
}

[[nodiscard]] coding::Note note_of(std::string_view text, u32 serial = 9U)
{
    coding::Note note;
    note.serial = serial;
    note.text = text;
    return note;
}

[[nodiscard]] Bytes plain_of(const coding::Note& note, u16 minor = coding::kMinor)
{
    Bytes out;
    out.resize(crd::net::wire::encoded_bytes(note, minor));
    usize written = 0U;
    REQUIRE(crd::net::wire::encode(note, minor, Span<u8>(out.data(), out.size()), written) == WireError::Ok);
    return out;
}

// A stream with its preamble read, and its bytes fed until the whole of them has gone.
struct OpenStream
{
    explicit OpenStream(CodedSession& session, u32 channel = kCodedEvent) : assembler(session.admission)
    {
        u8 route[8];
        const usize used = write_route(Route{channel, RouteForm::Whole}, route);
        const FrameStep step = assembler.feed(ConstSpan<u8>(route, used), Delivery::Established, 0U);
        REQUIRE(step.consumed == used);
    }

    StreamAssembler assembler;
};

struct NoteRead
{
    u32 serial = 0U;
    Bytes text;
};

// Feeds a whole frame at `piece` bytes a time and decodes the note it holds.
[[nodiscard]] NoteRead read_note(CodedSession& session, OpenStream& stream, ConstSpan<u8> frame, usize piece)
{
    NoteRead out;
    usize at = 0U;
    bool delivered = false;
    while (at < frame.size())
    {
        const usize size = std::min(piece, frame.size() - at);
        const FrameStep step = stream.assembler.feed(frame.subspan(at, size), Delivery::Established, 0U);
        REQUIRE(step.action != AdmitAction::Close);
        REQUIRE(step.action != AdmitAction::Wait);
        at += step.consumed;
        if (step.action == AdmitAction::Deliver)
        {
            coding::Note note;
            REQUIRE(decode_frame(step.frame, note) == WireError::Ok);
            out.serial = note.serial;
            out.text = bytes_of(ConstSpan<u8>(reinterpret_cast<const u8*>(note.text.data()), note.text.size()));
            session.admission.finish(step.frame);
            delivered = true;
        }
    }
    REQUIRE(delivered);
    return out;
}

// The header a Sized frame carries, read back.
struct ReadHeader
{
    u32 message = 0U;
    u32 declared = 0U;
    FrameHeader tail;
    usize tail_bytes = 0U;
    usize head_bytes = 0U;
};

[[nodiscard]] ReadHeader read_sized(ConstSpan<u8> frame, FrameShape shape)
{
    ReadHeader out;
    usize used = 0U;
    REQUIRE(read_frame_number(frame, out.message, used) == crd::net::FrameRead::Ok);
    usize at = used;
    REQUIRE(read_frame_number(frame.subspan(at), out.declared, used) == crd::net::FrameRead::Ok);
    at += used;
    REQUIRE(read_frame_tail(frame.subspan(at), HeaderForm::Sized, shape, out.tail, used) == crd::net::FrameRead::Ok);
    out.tail_bytes = used;
    out.head_bytes = at + used;
    return out;
}

// A Sized frame on the event channel with this header and body bytes, whatever they say.
[[nodiscard]] Bytes hand_frame(u32 message, u32 expanded, ConstSpan<u8> body, u32 hash = 0U)
{
    Bytes out;
    out.resize(kMaxFrameHeaderBytes + body.size());
    FrameHeader header;
    header.message = message;
    header.length = static_cast<u32>(body.size());
    header.object = kObject;
    header.hash = hash;
    header.expanded = expanded;
    const usize used =
        write_frame_header(HeaderForm::Sized, FrameShape{true, true, true}, header, Span<u8>(out.data(), out.size()));
    REQUIRE(used != 0U);
    if (!body.empty())
    {
        std::memcpy(out.data() + used, body.data(), body.size());
    }
    out.resize(used + body.size());
    return out;
}

[[nodiscard]] Bytes write_note(CodedSession& session, const coding::Note& note, u16 minor = coding::kMinor)
{
    Bytes out;
    out.resize(32000U);
    usize written = 0U;
    REQUIRE(write_frame(coding::kSchema, session.admission.coding(kCodedEvent), HeaderForm::Sized, note, minor, kObject,
                        Span<u8>(out.data(), out.size()), written) == WireError::Ok);
    out.resize(written);
    return out;
}

// A byte blob most of whose bytes are zero: entropy coding pays. `share` of a hundred bytes are random otherwise.
[[nodiscard]] Bytes sparse_blob(usize size, u32 share, u32 seed)
{
    std::mt19937 rng(seed);
    Bytes out;
    out.resize(size);
    for (u8& byte : out)
    {
        byte = ((rng() % 100U) < share) ? static_cast<u8>(rng()) : u8{0U};
    }
    return out;
}

[[nodiscard]] coding::Glance glance_of(const Bytes& blob)
{
    coding::Glance glance;
    glance.blob = ConstSpan<u8>(blob.data(), blob.size());
    return glance;
}
} // namespace

TEST_CASE("Coded frames: the schema pins the models' sources", "[net][coding]")
{
    const CodedModels& models = coded_models();
    REQUIRE(models.dictionary.ready());
    REQUIRE(models.note.ready());
    REQUIRE(models.state.ready());
    REQUIRE(models.coder.ready());
    const ChannelDesc& event = coding::kChannels[kCodedEvent];
    const ChannelDesc& state = coding::kChannels[kCodedState];
    CHECK(event.codec_count == 2U);
    CHECK(event.codecs[0].model == models.dictionary.hash());
    CHECK(event.codecs[1].model == models.note.hash());
    CHECK(state.codecs[0].model == models.state.hash());
    CHECK(event.codecs[0].kind == CodecKind::Dictionary);
    CHECK(event.codecs[1].kind == CodecKind::Entropy);
    CHECK(state.codecs[0].max_ratio == 64U);
}

TEST_CASE("Coded frames: what a writer may declare", "[net][coding]")
{
    CHECK_FALSE(expansion_holds(0U, 1U, 64U));
    CHECK(expansion_holds(1U, 2U, 64U));
    CHECK_FALSE(expansion_holds(2U, 2U, 64U));
    CHECK_FALSE(expansion_holds(3U, 2U, 64U));
    CHECK(expansion_holds(10U, 640U, 64U));
    CHECK_FALSE(expansion_holds(10U, 641U, 64U));
    CHECK(expansion_holds(0xFFFFFFFFU, 0xFFFEFFFF0001ULL, 65535U)); // the ratio's product does not wrap
    CHECK_FALSE(expansion_holds(0xFFFFFFFFU, 0xFFFEFFFF0002ULL, 65535U));
}

TEST_CASE("Coded frames: a note travels coded at both revisions, whole and at every split", "[net][coding]")
{
    for (const u16 minor : {u16{0U}, u16{1U}})
    {
        CodedSession session(minor);
        const BodyCoding coding = session.admission.coding(kCodedEvent);
        REQUIRE(coding.model != nullptr);
        CHECK(coding.model->kind() == ((minor == 0U) ? CodecKind::Dictionary : CodecKind::Entropy));
        CHECK(coding.max_ratio == ((minor == 0U) ? 512U : 64U));
        CHECK(coding.max_bytes == 20000U);

        const coding::Note note = note_of(spaced_text(700U));
        const Bytes plain = plain_of(note, minor);
        const Bytes frame = write_note(session, note, minor);
        const FrameShape shape = frame_shape(coding::kChannels[kCodedEvent], coding::kMessages[0], true);
        CHECK(shape.object);
        CHECK(shape.hash);
        CHECK(shape.expanded);
        const ReadHeader header = read_sized(ConstSpan<u8>(frame.data(), frame.size()), shape);
        CHECK(header.message == 1U);
        CHECK(header.tail.object == kObject);
        CHECK(header.tail.expanded == plain.size());
        CHECK(header.tail.hash == frame_hash(1U, kObject, ConstSpan<u8>(plain.data(), plain.size())));
        CHECK(header.declared == header.tail_bytes + (frame.size() - header.head_bytes));
        // English the model knows.
        CHECK(frame.size() < plain.size() * ((minor == 0U) ? 2U : 3U) / 4U);

        OpenStream stream(session);
        for (const usize piece : {frame.size(), usize{1U}, usize{7U}})
        {
            const NoteRead read = read_note(session, stream, ConstSpan<u8>(frame.data(), frame.size()), piece);
            CHECK(read.serial == 9U);
            REQUIRE(read.text.size() == note.text.size());
            CHECK(std::memcmp(read.text.data(), note.text.data(), read.text.size()) == 0);
        }
        session.check_all_given_back();
        CHECK(session.buffers.total >= 3U);
    }
}

TEST_CASE("Coded frames: a body that does not pay travels as it is, and is a view", "[net][coding]")
{
    CodedSession session;
    // Few bytes never pay for a state's three bytes.
    const coding::Note note = note_of("hi");
    const Bytes plain = plain_of(note);
    const Bytes frame = write_note(session, note);
    const ReadHeader header = read_sized(ConstSpan<u8>(frame.data(), frame.size()), FrameShape{true, true, true});
    CHECK(header.tail.expanded == 0U);
    CHECK(frame.size() - header.head_bytes == plain.size());
    CHECK(std::memcmp(frame.data() + header.head_bytes, plain.data(), plain.size()) == 0);

    OpenStream stream(session);
    const FrameStep step = stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, 0U);
    REQUIRE(step.action == AdmitAction::Deliver);
    CHECK(step.frame.body.data() == frame.data() + header.head_bytes);
    CHECK(step.frame.expansion == nullptr);
    CHECK(session.buffers.total == 0U);
    session.admission.finish(step.frame);
    session.check_all_given_back();

    // Random bytes do not shrink, whatever the size.
    const Bytes blob = sparse_blob(3000U, 100U, 1U);
    Bytes out;
    out.resize(8000U);
    usize written = 0U;
    REQUIRE(write_frame(coding::kSchema, session.admission.coding(kCodedState), HeaderForm::ToEnd, glance_of(blob),
                        coding::kMinor, kObject, Span<u8>(out.data(), out.size()), written) == WireError::Ok);
    CHECK(written > blob.size());
    CHECK(written < blob.size() + 16U);
}

TEST_CASE("Coded frames: what a writer refuses", "[net][coding]")
{
    CodedSession session;
    const BodyCoding coding = session.admission.coding(kCodedState);
    const Bytes blob = sparse_blob(1000U, 100U, 7U);
    const u16 minor = coding::kMinor;
    Bytes out;
    usize written = 7U;

    SECTION("No room for the largest header, or for a body that travels as it is")
    {
        const usize plain = crd::net::wire::encoded_bytes(glance_of(blob), minor);
        for (const usize room :
             {usize{0U}, kMaxFrameHeaderBytes - 1U, kMaxFrameHeaderBytes, kMaxFrameHeaderBytes + plain - 1U})
        {
            out.resize(kMaxFrameHeaderBytes + plain);
            CHECK(write_frame(coding::kSchema, coding, HeaderForm::ToEnd, glance_of(blob), minor, kObject,
                              Span<u8>(out.data(), room), written) == WireError::NoRoom);
            CHECK(written == 0U);
        }
        CHECK(write_frame(coding::kSchema, coding, HeaderForm::ToEnd, glance_of(blob), minor, kObject,
                          Span<u8>(out.data(), kMaxFrameHeaderBytes + plain), written) == WireError::Ok);
        CHECK(written > plain);
    }
    SECTION("A message no encoder may write, and one that is a delta writer's")
    {
        out.resize(32000U);
        const BodyCoding event = session.admission.coding(kCodedEvent);
        const coding::Note long_note = note_of(spaced_text(8001U)); // one character past the field's most
        CHECK(write_frame(coding::kSchema, event, HeaderForm::Sized, long_note, minor, kObject,
                          Span<u8>(out.data(), out.size()), written) != WireError::Ok);
        CHECK(written == 0U);
        coding::World world;
        CHECK(write_frame(coding::kSchema, coding, HeaderForm::ToEnd, world, minor, kObject,
                          Span<u8>(out.data(), out.size()), written) == WireError::OutOfRange);
        CHECK(written == 0U);
    }
    SECTION("A body past what its channel lets a reader accept travels as it is")
    {
        const Bytes zeros = sparse_blob(3000U, 0U, 8U);
        out.resize(4000U);
        const CodedModels& models = coded_models();
        const usize coded = coding.coder->encode(models.state, 1024U, ConstSpan<u8>(zeros.data(), zeros.size()),
                                                 Span<u8>(out.data(), out.size()));
        REQUIRE(coded != 0U);
        REQUIRE(coded * 2U < zeros.size());
        const auto ratio = static_cast<u16>(zeros.size() / coded); // what it would need, rounded down
        CHECK(coding.coder->encode(models.state, ratio, ConstSpan<u8>(zeros.data(), zeros.size()),
                                   Span<u8>(out.data(), out.size())) == 0U);
        CHECK(coding.coder->encode(models.state, static_cast<u16>(ratio + 1U),
                                   ConstSpan<u8>(zeros.data(), zeros.size()),
                                   Span<u8>(out.data(), out.size())) == coded);
        // A body that does not shrink, or has nothing to shrink.
        const Bytes noise = sparse_blob(500U, 100U, 9U);
        CHECK(coding.coder->encode(models.state, 1024U, ConstSpan<u8>(noise.data(), noise.size()),
                                   Span<u8>(out.data(), out.size())) == 0U);
        CHECK(coding.coder->encode(models.state, 1024U, ConstSpan<u8>{}, Span<u8>(out.data(), out.size())) == 0U);
        CHECK(coding.coder->encode(models.dictionary, 1024U, ConstSpan<u8>{}, Span<u8>(out.data(), out.size())) == 0U);
    }
}

TEST_CASE("Coded frames: datagrams, whole, listed and in fragments", "[net][coding]")
{
    CodedSession session;
    const BodyCoding coding = session.admission.coding(kCodedState);
    REQUIRE(coding.model != nullptr);
    FragmentSlot slots[4];
    DatagramAssembler assembler(session.admission, kCodedState, slots, kNanosPerSecond);

    const auto expect_blob = [&](const FrameStep& step, const Bytes& blob)
    {
        REQUIRE(step.action == AdmitAction::Deliver);
        coding::Glance glance;
        REQUIRE(decode_frame(step.frame, glance) == WireError::Ok);
        REQUIRE(glance.blob.size() == blob.size());
        CHECK(std::memcmp(glance.blob.data(), blob.data(), blob.size()) == 0);
        session.admission.finish(step.frame);
    };

    SECTION("Whole")
    {
        const Bytes blob = sparse_blob(900U, 10U, 2U);
        u8 datagram[1400];
        const usize route = write_route(Route{kCodedState, RouteForm::Whole}, datagram);
        usize written = 0U;
        REQUIRE(write_frame(coding::kSchema, coding, HeaderForm::ToEnd, glance_of(blob), coding::kMinor, kObject,
                            Span<u8>(datagram + route, sizeof(datagram) - route), written) == WireError::Ok);
        CHECK(written < blob.size());

        DatagramCursor cursor;
        REQUIRE(session.admission.open_datagram(ConstSpan<u8>(datagram, route + written), 0U, cursor) ==
                AdmitVerdict::Admitted);
        expect_blob(assembler.next(cursor, Delivery::Established, 0U), blob);
        CHECK_FALSE(cursor.more);
    }
    SECTION("Listed, each decoding alone")
    {
        const Bytes first = sparse_blob(400U, 8U, 3U);
        const Bytes second = sparse_blob(500U, 12U, 4U);
        u8 datagram[1400];
        usize at = write_route(Route{kCodedState, RouteForm::Listed}, datagram);
        for (const Bytes* blob : {&first, &second})
        {
            usize written = 0U;
            REQUIRE(write_frame(coding::kSchema, coding, HeaderForm::Sized, glance_of(*blob), coding::kMinor, kObject,
                                Span<u8>(datagram + at, sizeof(datagram) - at), written) == WireError::Ok);
            at += written;
        }
        DatagramCursor cursor;
        REQUIRE(session.admission.open_datagram(ConstSpan<u8>(datagram, at), 0U, cursor) == AdmitVerdict::Admitted);
        expect_blob(assembler.next(cursor, Delivery::Established, 0U), first);
        REQUIRE(cursor.more);
        expect_blob(assembler.next(cursor, Delivery::Established, 0U), second);
    }
    SECTION("Fragments in any order, each naming what the message expands to")
    {
        const Bytes blob = sparse_blob(3800U, 20U, 5U);
        Bytes plain;
        plain.resize(crd::net::wire::encoded_bytes(glance_of(blob), coding::kMinor));
        usize plain_bytes = 0U;
        REQUIRE(crd::net::wire::encode(glance_of(blob), coding::kMinor, Span<u8>(plain.data(), plain.size()),
                                       plain_bytes) == WireError::Ok);
        Bytes coded;
        coded.resize(plain.size());
        const usize coded_bytes =
            coding.coder->encode(*coding.model, coding.max_ratio, ConstSpan<u8>(plain.data(), plain_bytes),
                                 Span<u8>(coded.data(), coded.size()));
        REQUIRE(coded_bytes != 0U);
        REQUIRE(coded_bytes < plain_bytes);

        const MessageDesc& glance_desc = coding::kMessages[2];
        const FrameShape shape = frame_shape(coding::kChannels[kCodedState], glance_desc, true);
        FrameHeader header;
        header.message = glance_desc.id;
        header.length = static_cast<u32>(coded_bytes);
        header.object = kObject;
        header.hash = frame_hash(glance_desc.id, kObject, ConstSpan<u8>(plain.data(), plain_bytes));
        header.expanded = static_cast<u32>(plain_bytes);
        header.chunk = 300U;
        header.sequence = 5U;
        const u32 pieces = crd::net::fragment_count(header.length, header.chunk);
        REQUIRE(pieces >= 2U);

        const auto put = [&](const FrameHeader& with, u32 index, Span<u8> out)
        {
            FrameHeader piece = with;
            piece.index = index;
            const usize size = write_fragment(kCodedState, shape, piece, ConstSpan<u8>(coded.data(), coded_bytes), out);
            REQUIRE(size != 0U);
            return size;
        };
        FrameStep last;
        const auto arrive = [&](const FrameHeader& with, u32 index)
        {
            u8 datagram[600];
            const usize size = put(with, index, datagram);
            DatagramCursor cursor;
            REQUIRE(session.admission.open_datagram(ConstSpan<u8>(datagram, size), 0U, cursor) ==
                    AdmitVerdict::Admitted);
            last = assembler.next(cursor, Delivery::Established, 0U);
        };
        for (u32 index = pieces; index != 1U; --index)
        {
            arrive(header, index - 1U);
            CHECK(last.verdict == AdmitVerdict::Buffered);
        }
        // A piece that declares another expansion is not of this message.
        FrameHeader other = header;
        other.expanded += 1U;
        arrive(other, 0U);
        CHECK(last.verdict == AdmitVerdict::FragmentConflict);
        arrive(header, 1U);
        CHECK(last.verdict == AdmitVerdict::DuplicateFragment);
        // The reservation covers the pieces and what they expand to.
        CHECK(session.quota.reassembly.held() == u64{header.length} + header.expanded);
        arrive(header, 0U);
        expect_blob(last, blob);
    }
    session.check_all_given_back();
}

TEST_CASE("Coded frames: the caps hold before anything is reserved", "[net][coding]")
{
    u8 body[1200] = {};
    const auto coded = [&](usize size)
    {
        return ConstSpan<u8>(body, size);
    };

    // Refused by read_frame(): no gate spent, no buffer asked for, and the stream ends.
    const auto refused_at_header =
        [](u32 expanded, ConstSpan<u8> bytes, AdmitVerdict verdict, u16 minor = coding::kMinor)
    {
        CodedSession session(minor);
        OpenStream stream(session);
        const Bytes frame = hand_frame(1U, expanded, bytes);
        const FrameStep step =
            stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, 0U);
        CHECK(step.verdict == verdict);
        CHECK(step.action == AdmitAction::Close);
        CHECK(session.buffers.total == 0U);
        CHECK(session.admission.count(verdict) == 1U);
        session.check_all_given_back();
    };

    SECTION("A body no smaller than it says it expands to")
    {
        refused_at_header(10U, coded(10U), AdmitVerdict::BadExpansion);
        refused_at_header(10U, coded(11U), AdmitVerdict::BadExpansion);
    }
    SECTION("Past the channel's ratio, at this revision's ratio")
    {
        refused_at_header(641U, coded(10U), AdmitVerdict::BadExpansion);      // entropy: 64
        refused_at_header(5121U, coded(10U), AdmitVerdict::BadExpansion, 0U); // dictionary: 512
    }
    SECTION("What it decodes to is held to the message")
    {
        refused_at_header(2U, coded(1U), AdmitVerdict::Undersized);
        refused_at_header(8009U, coded(1000U), AdmitVerdict::Oversized);
    }
    SECTION("A newer writer may pass this reader's most, never the channel's")
    {
        refused_at_header(20001U, coded(1000U), AdmitVerdict::Oversized, 5U);
        CodedSession session(5U);
        OpenStream stream(session);
        const Bytes frame = hand_frame(1U, 20000U, coded(1000U));
        const FrameStep step =
            stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, 0U);
        // It passes the header, takes its buffer, and only then fails to decode.
        CHECK(step.verdict == AdmitVerdict::BadExpansion);
        CHECK(session.buffers.total == 1U);
        session.check_all_given_back();
    }
    SECTION("At the ratio it is read, and what it holds decides")
    {
        CodedSession session;
        OpenStream stream(session);
        const Bytes frame = hand_frame(1U, 640U, coded(10U));
        const FrameStep step =
            stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::BadExpansion);
        CHECK(step.action == AdmitAction::Close);
        CHECK(session.buffers.total == 1U);
        session.check_all_given_back();
    }
    SECTION("An unreliable channel only drops")
    {
        CodedSession session;
        u8 datagram[64];
        const usize route = write_route(Route{kCodedState, RouteForm::Whole}, datagram);
        FrameHeader header;
        header.message = 3U;
        header.object = kObject;
        header.expanded = 100U;
        const usize used = write_frame_header(HeaderForm::ToEnd, FrameShape{true, true, true}, header,
                                              Span<u8>(datagram + route, sizeof(datagram) - route));
        REQUIRE(used != 0U);
        const usize size = route + used + 1U; // a ratio of a hundred
        FragmentSlot slots[1];
        DatagramAssembler assembler(session.admission, kCodedState, slots, kNanosPerSecond);
        DatagramCursor cursor;
        REQUIRE(session.admission.open_datagram(ConstSpan<u8>(datagram, size), 0U, cursor) == AdmitVerdict::Admitted);
        const FrameStep step = assembler.next(cursor, Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::BadExpansion);
        CHECK(step.action == AdmitAction::Skip);
        CHECK(session.buffers.total == 0U);
    }
}

TEST_CASE("Coded frames: S7 refuses before anything is decoded, and the next frame still reads", "[net][coding]")
{
    CodedSession session;
    OpenStream stream(session);
    const coding::Note note = note_of(spaced_text(500U));
    const Bytes frame = write_note(session, note);

    Capability none = session.capability;
    none.grant.operations = 0U;
    session.admission.set_capability(&none);
    const FrameStep refused =
        stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, 0U);
    CHECK(refused.verdict == AdmitVerdict::NotGranted);
    CHECK(refused.action == AdmitAction::Skip);
    CHECK(refused.consumed == frame.size());
    CHECK(session.buffers.total == 0U);
    session.check_all_given_back();

    // Nothing depended on that frame's being read.
    session.admission.set_capability(&session.capability);
    const NoteRead read = read_note(session, stream, ConstSpan<u8>(frame.data(), frame.size()), frame.size());
    CHECK(read.text.size() == note.text.size());
    session.check_all_given_back();
}

TEST_CASE("Coded frames: S6 charges what a frame expands to", "[net][coding]")
{
    const coding::Note note = note_of(spaced_text(1000U));
    const Bytes plain = plain_of(note);
    const u64 expanded = plain.size();

    // Past the channel's own message gate, which closes for a millisecond.
    constexpr u64 later_ns = 2U * 1000U * 1000U;

    SECTION("The principal's bytes")
    {
        QuotaLimits limits;
        limits.bytes_per_s = 100000U;
        limits.tolerance_ns = 0U;
        CodedSession session(coding::kMinor, limits);
        OpenStream stream(session);
        const Bytes frame = write_note(session, note);
        REQUIRE(frame.size() < expanded);
        const FrameStep first =
            stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, 0U);
        REQUIRE(first.action == AdmitAction::Deliver);
        session.admission.finish(first.frame);
        const FrameStep second =
            stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, later_ns);
        CHECK(second.verdict == AdmitVerdict::PrincipalBytes);
        CHECK(second.action == AdmitAction::Wait);
        CHECK(second.consumed == 0U);
        CHECK(second.retry_at_ns == expanded * 10000U); // ten microseconds a byte: the expanded size, not the wire's
        const FrameStep later =
            stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, second.retry_at_ns);
        REQUIRE(later.action == AdmitAction::Deliver);
        session.admission.finish(later.frame);
        session.check_all_given_back();
    }
    SECTION("The capability's bytes")
    {
        CodedSession session;
        session.capability.grant.budget.bytes_per_second = 100000U;
        session.quota.limits.tolerance_ns = 0U;
        OpenStream stream(session);
        const Bytes frame = write_note(session, note);
        const FrameStep first =
            stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, 0U);
        REQUIRE(first.action == AdmitAction::Deliver);
        session.admission.finish(first.frame);
        const FrameStep second =
            stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, later_ns);
        CHECK(second.verdict == AdmitVerdict::BudgetBytes);
        CHECK(second.retry_at_ns == expanded * 10000U);
    }
    SECTION("What the principal may hold")
    {
        QuotaLimits limits;
        // At the channel's priority of two, three quarters of it.
        limits.reassembly_bytes = (expanded + (expanded / 2U)) * 4U / 3U;
        CodedSession session(coding::kMinor, limits);
        OpenStream stream(session);
        const Bytes frame = write_note(session, note);
        const FrameStep first =
            stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, 0U);
        REQUIRE(first.action == AdmitAction::Deliver);
        CHECK(session.quota.reassembly.held() == expanded);

        // A second, while the first is still with its handler: held back, nothing consumed, nothing spent.
        const FrameStep second =
            stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, 0U);
        CHECK(second.verdict == AdmitVerdict::ReassemblyFull);
        CHECK(second.action == AdmitAction::Wait);
        CHECK(second.consumed == 0U);
        CHECK(second.retry_at_ns == kNeverNs);
        CHECK(session.quota.reassembly.held() == expanded);
        CHECK(session.buffers.total == 1U);

        session.admission.finish(first.frame);
        const FrameStep third =
            stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, 0U);
        REQUIRE(third.action == AdmitAction::Deliver);
        session.admission.finish(third.frame);
        session.check_all_given_back();
    }
    SECTION("A reservation of a frame in pieces covers both its buffers")
    {
        CodedSession session;
        OpenStream stream(session);
        const Bytes frame = write_note(session, note);
        const usize head = 10U;
        const FrameStep start = stream.assembler.feed(ConstSpan<u8>(frame.data(), head), Delivery::Established, 0U);
        CHECK(start.verdict == AdmitVerdict::NeedMore);
        const ReadHeader header = read_sized(ConstSpan<u8>(frame.data(), frame.size()), FrameShape{true, true, true});
        CHECK(session.quota.reassembly.held() == u64{header.declared} - header.tail_bytes + header.tail.expanded);
        CHECK(session.buffers.live == 2U);
    }
    SECTION("An allocator's refusal holds the reader back, and the same bytes then pass")
    {
        CodedSession session;
        OpenStream stream(session);
        const Bytes frame = write_note(session, note);
        session.buffers.refuse_all = true;
        const FrameStep held =
            stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, 0U);
        CHECK(held.verdict == AdmitVerdict::NoMemory);
        CHECK(held.action == AdmitAction::Wait);
        CHECK(held.consumed == 0U);
        session.check_all_given_back();
        session.buffers.refuse_all = false;
        const NoteRead read = read_note(session, stream, ConstSpan<u8>(frame.data(), frame.size()), frame.size());
        CHECK(read.text.size() == note.text.size());
        session.check_all_given_back();
    }
    SECTION("A frame in pieces whose buffer is refused gives back what its expansion took")
    {
        CodedSession session;
        OpenStream stream(session);
        const Bytes frame = write_note(session, note);
        // The expansion's buffer comes first: leave room for it alone, by refusing the second request.
        class RefusesSecond final : public crd::memory::IAllocator
        {
        public:
            explicit RefusesSecond(crd::memory::IAllocator& inner) : m_inner(inner) {}
            void* allocate(usize size, usize alignment) override { return try_allocate(size, alignment); }
            void deallocate(void* p) noexcept override { m_inner.deallocate(p); }
            [[nodiscard]] bool owns(const void* p) const noexcept override { return m_inner.owns(p); }
            void* try_allocate(usize size, usize alignment) override
            {
                ++m_calls;
                return (m_calls == 2U) ? nullptr : m_inner.try_allocate(size, alignment);
            }

        private:
            crd::memory::IAllocator& m_inner;
            u32 m_calls = 0U;
        } refusing(session.buffers);
        PrincipalQuota quota{QuotaLimits{}};
        ChannelState channels[4];
        Admission admission(quota, refusing, channels);
        const CodedModels& models = coded_models();
        const SessionSchema schemas[] = {
            {&coding::kSchema, &session.authorized, ConstSpan<const BodyModel*>(models.list), &coded_models().coder}};
        REQUIRE(admission.bind(schemas));
        REQUIRE(admission.announce(ConstSpan<u8>(coding::kSchema.id, sizeof(coding::kSchema.id)), coding::kMajor,
                                   coding::kMinor));
        admission.set_capability(&session.capability);
        StreamAssembler assembler(admission);
        u8 route[8];
        const usize used = write_route(Route{kCodedEvent, RouteForm::Whole}, route);
        REQUIRE(assembler.feed(ConstSpan<u8>(route, used), Delivery::Established, 0U).consumed == used);
        const FrameStep held = assembler.feed(ConstSpan<u8>(frame.data(), 10U), Delivery::Established, 0U);
        CHECK(held.verdict == AdmitVerdict::NoMemory);
        CHECK(held.action == AdmitAction::Wait);
        CHECK(quota.reassembly.held() == 0U);
        CHECK(quota.pending.held() == 0U);
        CHECK(session.buffers.live == 0U);
    }
}

TEST_CASE("Coded frames: any damage is refused, never delivered as something else", "[net][coding]")
{
    for (const u16 minor : {u16{0U}, u16{1U}})
    {
        const coding::Note note = note_of(spaced_text(300U));
        Bytes clean;
        usize head = 0U;
        {
            CodedSession session(minor);
            clean = write_note(session, note, minor);
            head = read_sized(ConstSpan<u8>(clean.data(), clean.size()), FrameShape{true, true, true}).head_bytes;
        }
        const Bytes plain = plain_of(note, minor);
        for (usize at = head; at < clean.size(); ++at)
        {
            for (const u8 flip : {u8{0x01U}, u8{0x80U}})
            {
                Bytes frame = clean;
                frame[at] = static_cast<u8>(frame[at] ^ flip);
                CodedSession session(minor);
                OpenStream stream(session);
                const FrameStep step =
                    stream.assembler.feed(ConstSpan<u8>(frame.data(), frame.size()), Delivery::Established, 0U);
                if (step.action == AdmitAction::Deliver)
                {
                    // Only a frame that still means the same message: zstd has spare bits, the entropy coder none.
                    REQUIRE(minor == 0U);
                    REQUIRE(step.frame.body.size() == plain.size());
                    CHECK(std::memcmp(step.frame.body.data(), plain.data(), plain.size()) == 0);
                    session.admission.finish(step.frame);
                }
                else
                {
                    const bool refused =
                        step.verdict == AdmitVerdict::BadExpansion || step.verdict == AdmitVerdict::IntegrityFailed;
                    CHECK(refused);
                    CHECK(step.action == AdmitAction::Close);
                }
                session.check_all_given_back();
            }
        }
    }

    SECTION("A hash over the message, not the bytes that carried it")
    {
        CodedSession session;
        OpenStream stream(session);
        const coding::Note note = note_of(spaced_text(300U));
        Bytes frame = write_note(session, note);
        const ReadHeader header = read_sized(ConstSpan<u8>(frame.data(), frame.size()), FrameShape{true, true, true});
        const Bytes wrong = hand_frame(
            1U, header.tail.expanded, ConstSpan<u8>(frame.data() + header.head_bytes, frame.size() - header.head_bytes),
            header.tail.hash ^ 1U);
        const FrameStep step =
            stream.assembler.feed(ConstSpan<u8>(wrong.data(), wrong.size()), Delivery::Established, 0U);
        CHECK(step.verdict == AdmitVerdict::IntegrityFailed);
        CHECK(session.buffers.total == 1U);
        session.check_all_given_back();
    }
}

TEST_CASE("Coded frames: padding counts what is sent, after coding", "[net][coding]")
{
    CodedSession session;
    OpenStream stream(session);
    const coding::Note note = note_of(spaced_text(400U));
    Bytes frame = write_note(session, note);
    const usize coded_size = frame.size();
    frame.resize(coded_size + 128U);
    const usize padding = write_stream_padding(coded_size, 64U, Span<u8>(frame.data() + coded_size, 128U));
    REQUIRE(padding != 0U);
    CHECK((coded_size + padding) % 64U == 0U);
    usize at = 0U;
    u32 delivered = 0U;
    while (at < coded_size + padding)
    {
        const FrameStep step = stream.assembler.feed(ConstSpan<u8>(frame.data() + at, coded_size + padding - at),
                                                     Delivery::Established, 0U);
        at += step.consumed;
        if (step.action == AdmitAction::Deliver)
        {
            ++delivered;
            session.admission.finish(step.frame);
        }
        REQUIRE(step.action != AdmitAction::Close);
    }
    CHECK(delivered == 1U);
    session.check_all_given_back();
}

TEST_CASE("Coded frames: both ends code at the lower revision's newest model", "[net][coding]")
{
    // A reader at the schema's minor one and any writer.
    CodedSession older(0U);
    CHECK(older.admission.coding(kCodedEvent).model == &coded_models().dictionary);
    CodedSession same(1U);
    CHECK(same.admission.coding(kCodedEvent).model == &coded_models().note);
    CodedSession newer(9U);
    CHECK(newer.admission.coding(kCodedEvent).model == &coded_models().note);
    CHECK(newer.admission.coding(kCodedState).model == &coded_models().state);

    // A channel whose first model begins at a later minor codes nothing before it, not even the expanded size.
    static constexpr CodecDesc kLate[] = {{1U, CodecKind::Entropy, 16U, 0U}};
    static CodecDesc late[1] = {kLate[0]};
    late[0].model = coded_models().note.hash();
    const ChannelDesc channels[] = {
        {"later", ChannelClass::Event, true, false, false, 2U, 4000U, 1000U, 100000000ULL, 0U, true, late, 1U}};
    const MessageDesc messages[] = {{"Say", 1U, 0U, 0U, 1ULL, 0U, 4000U}};
    const WireSchema schema{
        "later", {0x31U}, static_cast<u8>(ScopeKind::World), 1U, 1U, 1ULL, 0ULL, channels, messages,
    };
    Buffers buffers;
    PrincipalQuota quota{QuotaLimits{}};
    ChannelState states[1];
    Admission admission(quota, buffers, states);
    AuthorizedSchema authorized = authorized_for(schema);
    Capability capability = capability_for(authorized);
    const CodedModels& models = coded_models();
    const SessionSchema schemas[] = {
        {&schema, &authorized, ConstSpan<const BodyModel*>(models.list), &coded_models().coder}};
    REQUIRE(admission.bind(schemas));
    admission.set_capability(&capability);
    REQUIRE(admission.announce(ConstSpan<u8>(schema.id, sizeof(schema.id)), 1U, 0U));
    CHECK(admission.coding(0U).model == nullptr);
    u8 body[20] = {};
    crd::net::ParsedFrame parsed;
    FrameHeader header;
    header.message = 1U;
    header.length = sizeof(body);
    header.object = 3U;
    u8 frame[64];
    const usize used = write_frame_header(HeaderForm::Sized, FrameShape{true, true, false}, header, frame);
    std::memcpy(frame + used, body, sizeof(body));
    CHECK(admission.read_frame(0U, HeaderForm::Sized, ConstSpan<u8>(frame, used + sizeof(body)), parsed) ==
          AdmitVerdict::Admitted);
    CHECK(parsed.header.expanded == 0U);
    CHECK(parsed.header.length == sizeof(body));

    // A second announcement that says the same keeps it; then a session at minor one does code.
    REQUIRE(admission.announce(ConstSpan<u8>(schema.id, sizeof(schema.id)), 1U, 0U));
    CHECK(admission.coding(0U).model == nullptr);
    ChannelState other_states[1];
    Admission other(quota, buffers, other_states);
    REQUIRE(other.bind(schemas));
    REQUIRE(other.announce(ConstSpan<u8>(schema.id, sizeof(schema.id)), 1U, 1U));
    CHECK(other.coding(0U).model == &coded_models().note);
    CHECK(other.coding(0U).max_ratio == 16U);
}

TEST_CASE("Coded frames: a session binds only with every model it declares", "[net][coding]")
{
    Buffers buffers;
    PrincipalQuota quota{QuotaLimits{}};
    ChannelState states[4];
    Admission admission(quota, buffers, states);
    AuthorizedSchema authorized = authorized_for(coding::kSchema);
    CodedModels& models = coded_models();

    SECTION("All of them, and a coder")
    {
        const SessionSchema schemas[] = {
            {&coding::kSchema, &authorized, ConstSpan<const BodyModel*>(models.list), &models.coder}};
        CHECK(admission.bind(schemas));
    }
    SECTION("Without a coder")
    {
        const SessionSchema schemas[] = {
            {&coding::kSchema, &authorized, ConstSpan<const BodyModel*>(models.list), nullptr}};
        CHECK_FALSE(admission.bind(schemas));
    }
    SECTION("A coder that could not start")
    {
        Buffers refused;
        refused.refuse_all = true;
        crd::net::BodyCoder none(refused, kCoderMessageBytes);
        CHECK_FALSE(none.ready());
        const SessionSchema schemas[] = {
            {&coding::kSchema, &authorized, ConstSpan<const BodyModel*>(models.list), &none}};
        CHECK_FALSE(admission.bind(schemas));
    }
    SECTION("One short")
    {
        const SessionSchema schemas[] = {
            {&coding::kSchema, &authorized, ConstSpan<const BodyModel*>(models.list, 2U), &models.coder}};
        CHECK_FALSE(admission.bind(schemas));
    }
    SECTION("Another kind with the same bytes")
    {
        BodyModel wrong(models.heap, CodecKind::Dictionary,
                        ConstSpan<u8>(models.state_source.data(), models.state_source.size()), kCoderMessageBytes);
        REQUIRE(wrong.ready());
        CHECK(wrong.hash() == models.state.hash());
        const BodyModel* list[] = {&models.dictionary, &models.note, &wrong};
        const SessionSchema schemas[] = {
            {&coding::kSchema, &authorized, ConstSpan<const BodyModel*>(list), &models.coder}};
        CHECK_FALSE(admission.bind(schemas));
    }
    SECTION("A model that is not one")
    {
        BodyModel broken(models.heap, CodecKind::Entropy, ConstSpan<u8>(models.state_source.data(), 100U),
                         kCoderMessageBytes);
        CHECK_FALSE(broken.ready());
        const BodyModel* list[] = {&models.dictionary, &models.note, &broken, nullptr};
        const SessionSchema schemas[] = {
            {&coding::kSchema, &authorized, ConstSpan<const BodyModel*>(list), &models.coder}};
        CHECK_FALSE(admission.bind(schemas));
    }
    SECTION("A transport schema, which runs below everything a model could say")
    {
        const CodecDesc codec{0U, CodecKind::Entropy, 16U, models.state.hash()};
        const ChannelDesc channels[] = {
            {"control", ChannelClass::Control, true, true, false, 0U, 1000U, 10U, 10000U, 0U, true, &codec, 1U}};
        const WireSchema transport{"t", {0x41U}, 0U, 1U, 0U, 0ULL, 0ULL, channels, ConstSpan<MessageDesc>{}};
        const SessionSchema schemas[] = {
            {&transport, nullptr, ConstSpan<const BodyModel*>(models.list), &models.coder}};
        CHECK_FALSE(admission.bind(schemas));
    }
    SECTION("A model a refusing allocator could not compile")
    {
        Buffers refused;
        refused.refuse_all = true;
        const BodyModel entropy(refused, CodecKind::Entropy,
                                ConstSpan<u8>(models.state_source.data(), models.state_source.size()),
                                kCoderMessageBytes);
        CHECK_FALSE(entropy.ready());
        const BodyModel dictionary(refused, CodecKind::Dictionary,
                                   ConstSpan<u8>(models.dictionary_source.data(), models.dictionary_source.size()),
                                   kCoderMessageBytes);
        CHECK_FALSE(dictionary.ready());
    }
}

namespace
{
// A writer and a reader over a channel with its model. One store on each side.
struct CodedPair
{
    explicit CodedPair(const BodyCoding& coding)
        : writer(coding::kWorldLayout, sender, coding), reader(coding::kWorldLayout, received, allocator, coding)
    {
        frame.resize(70000U);
    }

    ConstSpan<u8> send(u64 sequence, const Bytes& message)
    {
        REQUIRE(sender.put(sequence, ConstSpan<u8>(message.data(), message.size())));
        usize size = 0U;
        REQUIRE(writer.write(sequence, coding::kMinor, Span<u8>(frame.data(), frame.size()), size) == WireError::Ok);
        return ConstSpan<u8>(frame.data(), size);
    }

    Buffers allocator;
    BaselineStore sender{allocator, 32U, 1024U * 1024U};
    BaselineStore received{allocator, 32U, 1024U * 1024U};
    DeltaWriter writer;
    DeltaReader reader;
    Bytes frame;
};

// Bodies at rest at the origin: most of what it encodes to is zero bits.
[[nodiscard]] Bytes still_world(u32 count)
{
    crd::containers::Array<coding::Body> bodies;
    bodies.resize(count);
    for (u32 index = 0U; index < count; ++index)
    {
        bodies[index].id = static_cast<u16>(index);
    }
    coding::World world;
    world.tick = 1U;
    world.bodies = crd::net::wire::WireList<coding::BodyCodec>(ConstSpan<coding::Body>(bodies.data(), bodies.size()));
    Bytes out;
    out.resize(crd::net::wire::encoded_bytes(world, coding::kMinor));
    usize written = 0U;
    REQUIRE(crd::net::wire::encode(world, coding::kMinor, Span<u8>(out.data(), out.size()), written) == WireError::Ok);
    return out;
}

[[nodiscard]] AdmittedFrame delta_frame(ConstSpan<u8> body)
{
    AdmittedFrame frame;
    frame.schema = &coding::kSchema;
    frame.message = &coding::kMessages[1];
    frame.channel = kCodedState;
    frame.writer_minor = coding::kMinor;
    frame.length = static_cast<u32>(body.size());
    frame.body = body;
    return frame;
}
} // namespace

TEST_CASE("Coded deltas: what a snapshot channel carries shrinks, and rebuilds exactly", "[net][coding][delta]")
{
    CodedSession session;
    const BodyCoding coding = session.admission.coding(kCodedState);
    REQUIRE(coding.model == &coded_models().state);
    CodedPair pair(coding);
    REQUIRE(pair.reader.ready());
    const usize calls = pair.allocator.total;

    SimulatedWorld world(200U, 99U); // not the world the model was trained on
    u64 coded_total = 0U;
    u64 plain_total = 0U;
    u64 sequence = 1000U;
    Bytes previous = world.tick();
    {
        const ConstSpan<u8> first = pair.send(sequence, previous);
        ConstSpan<u8> message;
        WireError error = WireError::Ok;
        REQUIRE(pair.reader.read(first, coding::kMinor, message, error) == DeltaRead::Delivered);
        REQUIRE(message.size() == previous.size());
        CHECK(std::memcmp(message.data(), previous.data(), message.size()) == 0);
    }
    pair.writer.on_ack(pair.reader.ack());
    Bytes room;
    room.resize(64000U);
    for (u32 round = 0U; round < 200U; ++round)
    {
        ++sequence;
        const Bytes current = world.tick();
        const ConstSpan<u8> body = pair.send(sequence, current);
        // Against the same baseline, the delta it would have been uncoded.
        usize plain = 0U;
        REQUIRE(crd::net::wire::delta_encode(coding::kWorldLayout, ConstSpan<u8>(previous.data(), previous.size()),
                                             ConstSpan<u8>(current.data(), current.size()), coding::kMinor,
                                             Span<u8>(room.data(), room.size()), plain) == WireError::Ok);
        plain_total += plain;
        // Sequence, distance, hash and the expanded size stay outside the coding.
        coded_total += body.size() - (kDeltaHeaderMinBytes + 2U);

        ConstSpan<u8> message;
        WireError error = WireError::Ok;
        REQUIRE(pair.reader.read(body, coding::kMinor, message, error) == DeltaRead::Delivered);
        REQUIRE(message.size() == current.size());
        REQUIRE(std::memcmp(message.data(), current.data(), message.size()) == 0);
        CHECK(pair.reader.expanded_bytes() == current.size());
        pair.writer.on_ack(pair.reader.ack());
        previous = current;
    }
    CHECK(pair.writer.delta_frames() == 200U);
    CHECK(pair.allocator.total == calls);
    // A model trained on another run's deltas of the same world: 15% on what is left after delta coding (about 100
    // bytes a tick).
    CHECK(coded_total * 100U < plain_total * 90U);
    INFO("coded " << coded_total << " plain " << plain_total);
}

TEST_CASE("Coded deltas: the body's layout and what a reader refuses", "[net][coding][delta]")
{
    CodedSession session;
    const BodyCoding coding = session.admission.coding(kCodedState);
    CodedPair pair(coding);
    SimulatedWorld world(100U, 5U);
    ConstSpan<u8> message;
    WireError error = WireError::Ok;

    const Bytes first = world.tick();
    const ConstSpan<u8> whole = pair.send(100000U, first);
    // Distance zero, then an expanded size, of a message random bytes make hard to shrink, or zero if it did not pay.
    CHECK(whole[2] == 0U);
    REQUIRE(pair.reader.read(whole, coding::kMinor, message, error) == DeltaRead::Delivered);
    pair.writer.on_ack(pair.reader.ack());

    const Bytes second = world.tick();
    Bytes body = bytes_of(pair.send(100001U, second));
    CHECK(body[2] == 1U); // a distance of one
    CHECK(body.size() < second.size());

    SECTION("Damage anywhere in the coded payload")
    {
        for (usize at = kDeltaHeaderMinBytes + 2U; at < body.size(); ++at)
        {
            Bytes damaged = body;
            damaged[at] = static_cast<u8>(damaged[at] ^ 0x10U);
            const DeltaRead read =
                pair.reader.read(ConstSpan<u8>(damaged.data(), damaged.size()), coding::kMinor, message, error);
            const bool refused = read == DeltaRead::Malformed || read == DeltaRead::Mismatch;
            CHECK(refused);
        }
        CHECK(pair.reader.read(ConstSpan<u8>(body.data(), body.size()), coding::kMinor, message, error) ==
              DeltaRead::Delivered);
    }
    SECTION("Past the ratio, or larger than the channel's most")
    {
        // [sequence 2][distance 1][hash 4][expanded varuint][coded...]
        Bytes hostile;
        hostile.resize(7U);
        std::memcpy(hostile.data(), body.data(), 7U);
        for (const u8 byte : {u8{0x81U}, u8{0x01U}}) // 129
        {
            hostile.push_back(byte);
        }
        hostile.push_back(0x55U);
        hostile.push_back(0x55U);
        const DeltaRead read =
            pair.reader.read(ConstSpan<u8>(hostile.data(), hostile.size()), coding::kMinor, message, error);
        CHECK(read == DeltaRead::Malformed);
        CHECK(error == WireError::BadCoding);
        CHECK(pair.reader.expanded_bytes() == 0U);

        hostile.resize(7U);
        for (const u8 byte : {u8{0x84U}, u8{0xFFU}, u8{0x7FU}}) // past the channel's 60000
        {
            hostile.push_back(byte);
        }
        hostile.push_back(0x55U);
        CHECK(pair.reader.read(ConstSpan<u8>(hostile.data(), hostile.size()), coding::kMinor, message, error) ==
              DeltaRead::Malformed);
        CHECK(error == WireError::OutOfRange);
    }
    SECTION("A body with no expanded size is cut short")
    {
        CHECK(pair.reader.read(ConstSpan<u8>(body.data(), 7U), coding::kMinor, message, error) == DeltaRead::Malformed);
        CHECK(error == WireError::Truncated);
    }
    SECTION("Nothing is decoded for what is dropped first")
    {
        REQUIRE(pair.reader.read(ConstSpan<u8>(body.data(), body.size()), coding::kMinor, message, error) ==
                DeltaRead::Delivered);
        CHECK(pair.reader.read(ConstSpan<u8>(body.data(), body.size()), coding::kMinor, message, error) ==
              DeltaRead::Stale);
        CHECK(pair.reader.expanded_bytes() == 0U);

        // The next sequence, with a baseline this reader never held: a distance of 40 000.
        const u8 gone[] = {0x86U, 0xA2U, 0xC0U, 0xB8U, 0x02U, 1U, 2U, 3U, 4U, 200U, 1U, 0x55U, 0x55U, 0x55U};
        CHECK(pair.reader.read(gone, coding::kMinor, message, error) == DeltaRead::MissingBaseline);
        CHECK(pair.reader.expanded_bytes() == 0U);
    }
}

TEST_CASE("Coded deltas: a whole message codes too, and costs what it expands to", "[net][coding][delta]")
{
    QuotaLimits limits;
    limits.bytes_per_s = 1000000U;
    limits.tolerance_ns = 0U;
    CodedSession session(coding::kMinor, limits);
    const BodyCoding coding = session.admission.coding(kCodedState);
    CodedPair pair(coding);
    const Bytes still = still_world(300U);
    const Bytes body = bytes_of(pair.send(9U, still));
    CHECK(body[2] == 0U); // a distance of zero: the whole message
    REQUIRE(body.size() * 2U < still.size());

    ConstSpan<u8> message;
    WireError error = WireError::Ok;
    u64 retry = 0U;
    REQUIRE(session.admission.read_delta(delta_frame(ConstSpan<u8>(body.data(), body.size())), pair.reader, 0U, retry,
                                         message, error) == DeltaRead::Delivered);
    REQUIRE(message.size() == still.size());
    CHECK(std::memcmp(message.data(), still.data(), still.size()) == 0);
    CHECK(pair.reader.expanded_bytes() == still.size());
    CHECK(session.quota.bytes.open_at(crd::net::GateRule{limits.bytes_per_s, 0U}) ==
          (still.size() - body.size()) * 1000U);

    SECTION("A reader holds its own channel's ratio, whatever a writer did")
    {
        BodyCoding strict = coding;
        strict.max_ratio = 2U;
        CodedPair other(coding);
        Buffers allocator;
        BaselineStore store(allocator, 4U, 1024U * 1024U);
        DeltaReader reader(coding::kWorldLayout, store, allocator, strict);
        const ConstSpan<u8> again = other.send(10U, still);
        CHECK(reader.read(again, coding::kMinor, message, error) == DeltaRead::Malformed);
        CHECK(error == WireError::BadCoding);
        CHECK(reader.expanded_bytes() == 0U);
    }
    SECTION("An allocator that refuses a coded reader's room")
    {
        Buffers allocator;
        BaselineStore store(allocator, 4U, 1024U);
        REQUIRE(store.ready());
        allocator.refuse_all = true;
        const DeltaReader reader(coding::kWorldLayout, store, allocator, coding);
        CHECK_FALSE(reader.ready());
    }
}

TEST_CASE("Coded deltas: a stored payload when coding does not pay", "[net][coding][delta]")
{
    CodedSession session;
    const BodyCoding coding = session.admission.coding(kCodedState);
    CodedPair pair(coding);
    // One body: fewer bytes than the coder's state is worth.
    SimulatedWorld world(1U, 7U);
    const Bytes first = world.tick();
    const ConstSpan<u8> body = pair.send(5U, first);
    ConstSpan<u8> message;
    WireError error = WireError::Ok;
    REQUIRE(pair.reader.read(body, coding::kMinor, message, error) == DeltaRead::Delivered);
    REQUIRE(message.size() == first.size());
    CHECK(std::memcmp(message.data(), first.data(), first.size()) == 0);

    // No room for the expanded size is refused up front, and room is counted with it.
    usize written = 0U;
    REQUIRE(pair.sender.put(6U, ConstSpan<u8>(first.data(), first.size())));
    u8 small[64];
    CHECK(pair.writer.write(6U, coding::kMinor, Span<u8>(small, 13U), written) == WireError::NoRoom);
    CHECK(written == 0U);
    const usize room = kDeltaHeaderMinBytes + crd::net::kDeltaExpandedMaxBytes + first.size();
    CHECK(pair.writer.write(6U, coding::kMinor, Span<u8>(small, room - 1U), written) == WireError::NoRoom);
    CHECK(pair.writer.write(6U, coding::kMinor, Span<u8>(small, room), written) == WireError::Ok);
}

TEST_CASE("Coded deltas: a hostile path delivers nothing but what was written", "[net][coding][delta]")
{
    CodedSession session;
    const BodyCoding coding = session.admission.coding(kCodedState);
    CodedPair pair(coding);
    SimulatedWorld world(120U, 31U);
    std::mt19937 rng(0xBAD);

    struct Sent
    {
        u64 sequence = 0U;
        Bytes message;
        Bytes body;
    };
    crd::containers::Array<Sent> flight;
    crd::containers::Array<Bytes> by_sequence;
    u32 delivered = 0U;
    for (u32 round = 0U; round < 400U; ++round)
    {
        Sent sent;
        sent.sequence = 100U + round;
        sent.message = world.tick();
        sent.body = bytes_of(pair.send(sent.sequence, sent.message));
        by_sequence.push_back(sent.message);
        // Loss, repeats and overtaking.
        if ((rng() % 100U) >= 30U)
        {
            flight.push_back(sent);
            if ((rng() % 100U) < 10U)
            {
                flight.push_back(sent);
            }
        }
        while (flight.size() > (rng() % 4U))
        {
            const usize pick = rng() % flight.size();
            const Sent arrived = flight[pick];
            flight[pick] = flight[flight.size() - 1U];
            flight.pop_back();
            ConstSpan<u8> message;
            WireError error = WireError::Ok;
            const DeltaRead read = pair.reader.read(ConstSpan<u8>(arrived.body.data(), arrived.body.size()),
                                                    coding::kMinor, message, error);
            const bool expected =
                read == DeltaRead::Delivered || read == DeltaRead::Stale || read == DeltaRead::MissingBaseline;
            REQUIRE(expected);
            if (read == DeltaRead::Delivered)
            {
                ++delivered;
                REQUIRE(message.size() == arrived.message.size());
                REQUIRE(std::memcmp(message.data(), arrived.message.data(), message.size()) == 0);
                pair.writer.on_ack(pair.reader.ack());
            }
        }
    }
    CHECK(delivered > 100U);
    CHECK(pair.writer.delta_frames() > 100U);
}

TEST_CASE("Coded deltas: admission holds a read back and charges what it expanded to", "[net][coding][delta]")
{
    QuotaLimits limits;
    limits.bytes_per_s = 1000000U;
    limits.tolerance_ns = 0U;
    CodedSession session(coding::kMinor, limits);
    const BodyCoding coding = session.admission.coding(kCodedState);
    CodedPair pair(coding);
    SimulatedWorld world(100U, 11U);

    const Bytes first = world.tick();
    const Bytes first_body = bytes_of(pair.send(1U, first));
    ConstSpan<u8> message;
    WireError error = WireError::Ok;
    u64 retry = 0U;

    // The gate is open: read at once, and charged past what admit() charged for the body.
    REQUIRE(session.admission.read_delta(delta_frame(ConstSpan<u8>(first_body.data(), first_body.size())), pair.reader,
                                         0U, retry, message, error) == DeltaRead::Delivered);
    pair.writer.on_ack(pair.reader.ack());
    const u64 expanded = first.size();
    const u64 first_extra = (expanded > first_body.size()) ? expanded - first_body.size() : 0U;
    const u64 open = session.quota.bytes.open_at(crd::net::GateRule{limits.bytes_per_s, 0U});
    CHECK(open == first_extra * 1000U);

    // Past that, the same frame is held back without being read.
    const Bytes second = world.tick();
    const Bytes body = bytes_of(pair.send(2U, second));
    const AdmittedFrame frame = delta_frame(ConstSpan<u8>(body.data(), body.size()));
    if (open > 0U)
    {
        CHECK(session.admission.read_delta(frame, pair.reader, open - 1U, retry, message, error) ==
              DeltaRead::Throttled);
        CHECK(retry == open);
        CHECK(session.admission.count(AdmitVerdict::PrincipalBytes) == 1U);
        CHECK(pair.reader.count(DeltaRead::Delivered) == 1U);
    }
    // And read when its time has come: a small delta costs what it rebuilt.
    REQUIRE(session.admission.read_delta(frame, pair.reader, open, retry, message, error) == DeltaRead::Delivered);
    CHECK(pair.reader.expanded_bytes() == second.size());
    REQUIRE(second.size() > body.size());
    CHECK(session.quota.bytes.open_at(crd::net::GateRule{limits.bytes_per_s, 0U}) ==
          open + ((second.size() - body.size()) * 1000U));

    SECTION("The channel's gate and the capability's close it too")
    {
        const u64 now = 4U * kNanosPerSecond;
        session.capability.grant.budget.bytes_per_second = 1000U;
        const Bytes third = world.tick();
        const Bytes third_body = bytes_of(pair.send(3U, third));
        const AdmittedFrame next = delta_frame(ConstSpan<u8>(third_body.data(), third_body.size()));
        REQUIRE(session.admission.read_delta(next, pair.reader, now, retry, message, error) == DeltaRead::Delivered);
        const Bytes fourth = world.tick();
        const Bytes fourth_body = bytes_of(pair.send(4U, fourth));
        const AdmittedFrame after = delta_frame(ConstSpan<u8>(fourth_body.data(), fourth_body.size()));
        // They reopen in the order of their rates, each held back in turn until the read goes through.
        u64 at = now;
        DeltaRead read = session.admission.read_delta(after, pair.reader, at, retry, message, error);
        while (read == DeltaRead::Throttled)
        {
            REQUIRE(retry > at);
            at = retry;
            read = session.admission.read_delta(after, pair.reader, at, retry, message, error);
        }
        CHECK(read == DeltaRead::Delivered);
        CHECK(session.admission.count(AdmitVerdict::ChannelBytes) == 1U);
        CHECK(session.admission.count(AdmitVerdict::PrincipalBytes) == 1U);
        CHECK(session.admission.count(AdmitVerdict::BudgetBytes) == 1U);
    }
}
