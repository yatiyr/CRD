// crd-fuzz-net-coded-frames: frames on channels that code their bodies, in crd-net (NET.3;
// docs/design/network-foundation.md#body-coding), under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the second how a stream is split and the third, where a mode damages a frame,
// at which byte. The rest is:
//   0: any bytes on a stream of the dictionary channel, 1: of the entropy channel's messages in a datagram;
//   2: a message both written honestly and damaged at that byte, on the dictionary channel; 3: the same, entropy.
//
// Oracles. A restated header layout and rule, sharing no code with admission, decide every verdict. A frame delivered
// is what crd-compression decodes the body to, with its writer's hash, and any split reads as the whole does. A
// writer's frame, undamaged, is delivered as exactly the message. Every byte of quota and every buffer comes back.
#include <crd/compression/entropy.hpp>
#include <crd/compression/zstd.hpp>
#include <crd/containers/checksum.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/net/reassembly.hpp>

#include <cstring>

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
using crd::memory::IAllocator;
using crd::net::Admission;
using crd::net::AdmitAction;
using crd::net::AdmitVerdict;
using crd::net::BodyCoder;
using crd::net::BodyModel;
using crd::net::ChannelState;
using crd::net::DatagramAssembler;
using crd::net::DatagramCursor;
using crd::net::FragmentSlot;
using crd::net::FrameStep;
using crd::net::HeaderForm;
using crd::net::PrincipalQuota;
using crd::net::QuotaLimits;
using crd::net::Route;
using crd::net::RouteForm;
using crd::net::SessionSchema;
using crd::net::StreamAssembler;
using crd::net::wire::ChannelClass;
using crd::net::wire::ChannelDesc;
using crd::net::wire::CodecDesc;
using crd::net::wire::CodecKind;
using crd::net::wire::MessageDesc;
using crd::net::wire::WireError;
using crd::net::wire::WireSchema;

constexpr u64 kWide = 1ULL << 40U;
constexpr u32 kMaxMessage = 4000U;
constexpr u16 kDictionaryRatio = 128U;
constexpr u16 kEntropyRatio = 32U;
constexpr u32 kBlobId = 3U;
constexpr u32 kFlakeId = 4U;
constexpr u32 kBlobMost = 3000U;
constexpr u32 kFlakeLeast = 1U;
constexpr u32 kFlakeMost = 2000U;

constexpr u8 kDictionary[] = "the quick brown fox jumps over the lazy dog; the five boxing wizards jump quickly; "
                             "pack my box with five dozen liquor jugs; sphinx of black quartz, judge my vow.";

// The models, and a second compiled copy for the restatement's decoder: one loop's memory, once per process.
struct Models
{
    Models()
        : heap(32U * 1024U * 1024U), dictionary_view(kDictionary, sizeof(kDictionary) - 1U),
          dictionary(heap, CodecKind::Dictionary, dictionary_view, kMaxMessage),
          entropy(heap, CodecKind::Entropy, entropy_view(), kMaxMessage), coder(heap, kMaxMessage),
          plain_dictionary(heap, dictionary_view, kMaxMessage, crd::compression::kZstdDefaultLevel),
          plain_coder(heap, kMaxMessage, crd::compression::kZstdDefaultLevel)
    {
        CRD_FUZZ_REQUIRE(dictionary.ready() && entropy.ready() && coder.ready());
        CRD_FUZZ_REQUIRE(plain_dictionary.ready() && plain_coder.ready());
        CRD_FUZZ_REQUIRE(plain_entropy.load(entropy_view()));
        codecs[0] = CodecDesc{0U, CodecKind::Dictionary, kDictionaryRatio, dictionary.hash()};
        codecs[1] = CodecDesc{0U, CodecKind::Entropy, kEntropyRatio, entropy.hash()};
        channels[0] =
            ChannelDesc{"stream", ChannelClass::Event, true, true, false, 2U, kMaxMessage, 1U << 30U, kWide, 0U,
                        true,     &codecs[0],          1U};
        channels[1] =
            ChannelDesc{"gram", ChannelClass::State, false, false, true, 3U, kMaxMessage, 1U << 30U, kWide, 0U,
                        true,   &codecs[1],          1U};
        messages[0] = MessageDesc{"Blob", kBlobId, 0U, 0U, 1ULL, 0U, kBlobMost};
        messages[1] = MessageDesc{"Flake", kFlakeId, 1U, 0U, 2ULL, kFlakeLeast, kFlakeMost};
        schema = WireSchema{"coded",
                            {0x10U, 0x21U},
                            static_cast<u8>(ScopeKind::World),
                            1U,
                            0U,
                            3ULL,
                            2ULL,
                            ConstSpan<ChannelDesc>(channels, 2U),
                            ConstSpan<MessageDesc>(messages, 2U)};
        list[0] = &dictionary;
        list[1] = &entropy;
    }

    static ConstSpan<u8> entropy_view()
    {
        static u8 source[crd::compression::kEntropyModelBytes];
        static const bool kTrained = []
        {
            crd::compression::EntropyTrainer trainer;
            u8 sample[2048] = {};
            for (usize i = 0U; i < sizeof(sample); i += 5U)
            {
                sample[i] = static_cast<u8>((i * 7U) & 0x1FU);
            }
            trainer.add(sample);
            trainer.write_model(source);
            return true;
        }();
        static_cast<void>(kTrained);
        return source;
    }

    crd::memory::TlsfAllocator heap;
    ConstSpan<u8> dictionary_view;
    BodyModel dictionary;
    BodyModel entropy;
    BodyCoder coder;
    crd::compression::ZstdDictionary plain_dictionary;
    crd::compression::ZstdMessageCoder plain_coder;
    crd::compression::EntropyModel plain_entropy;
    CodecDesc codecs[2];
    ChannelDesc channels[2];
    MessageDesc messages[2];
    WireSchema schema;
    const BodyModel* list[2] = {};
};

Models& models()
{
    static Models instance;
    return instance;
}

class Buffers final : public IAllocator
{
public:
    Buffers() : m_heap(256U * 1024U) {}

    void* allocate(usize size, usize alignment) override { return try_allocate(size, alignment); }

    void deallocate(void* p) noexcept override
    {
        if (p != nullptr)
        {
            --live;
            m_heap.deallocate(p);
        }
    }

    bool owns(const void* p) const noexcept override { return m_heap.owns(p); }

    void* try_allocate(usize size, usize alignment) override
    {
        ++live;
        return m_heap.try_allocate(size, alignment);
    }

    u32 live = 0U;

private:
    crd::memory::TlsfAllocator m_heap;
};

struct Session
{
    Session() : quota(QuotaLimits{1000U, kWide, kWide, kWide, kWide, 1U << 20U}), admission(quota, buffers, channels)
    {
        const WireSchema& schema = models().schema;
        authorized.schema.kind = ScopeKind::World;
        std::memcpy(authorized.schema.schema.bytes, schema.id, sizeof(schema.id));
        authorized.schema.major = schema.major;
        authorized.schema.minor = schema.minor;
        authorized.schema.audience.bytes[0] = 1U;
        authorized.schema.operations = schema.operations;
        authorized.schema.idempotent = schema.idempotent;
        capability.audience = authorized.schema.audience;
        capability.grant.kind = ScopeKind::World;
        capability.grant.operations = ~u64{0};
        capability.grant.budget.operations_per_second = 1U << 30U;
        capability.grant.budget.bytes_per_second = kWide;
        const SessionSchema schemas[] = {
            {&schema, &authorized, ConstSpan<const BodyModel*>(models().list), &models().coder}};
        CRD_FUZZ_REQUIRE(admission.bind(schemas));
        CRD_FUZZ_REQUIRE(admission.announce(ConstSpan<u8>(schema.id, sizeof(schema.id)), schema.major, 0U));
        admission.set_capability(&capability);
    }

    void require_clear() const
    {
        CRD_FUZZ_REQUIRE(quota.reassembly.held() == 0U);
        CRD_FUZZ_REQUIRE(quota.pending.held() == 0U);
        CRD_FUZZ_REQUIRE(quota.streams.held() == 0U);
        CRD_FUZZ_REQUIRE(buffers.live == 0U);
    }

    Buffers buffers;
    PrincipalQuota quota;
    ChannelState channels[2];
    Admission admission;
    AuthorizedSchema authorized;
    Capability capability;
};

// ---- The restated layout and rule ----------------------------------------------------------------------------------

// A shortest-form number of 7-bit groups, least significant first: 0 short, -1 malformed, else its bytes.
int restated_number(const u8* data, usize size, u64 most, u64& value)
{
    u32 bits = 0U;
    for (u64 probe = most; probe != 0U; probe >>= 1U)
    {
        ++bits;
    }
    const usize limit = (bits + 6U) / 7U;
    value = 0U;
    for (usize index = 0U;; ++index)
    {
        if (index >= size)
        {
            return 0;
        }
        const u8 byte = data[index];
        if (index >= limit)
        {
            return -1;
        }
        const u64 group = byte & 0x7FU;
        const u32 shift = static_cast<u32>(index) * 7U;
        if (shift != 0U && group > (~u64{0} >> shift))
        {
            return -1;
        }
        value |= group << shift;
        if ((byte & 0x80U) == 0U)
        {
            if (group == 0U && index != 0U)
            {
                return -1;
            }
            return (value > most) ? -1 : static_cast<int>(index + 1U);
        }
    }
}

struct Restated
{
    AdmitVerdict verdict = AdmitVerdict::NeedMore;
    u64 object = 0U;
    u32 hash = 0U;
    u64 body = 0U;
    u64 expanded = 0U;
    usize header = 0U;
};

// A frame at the start of `data`. `sized` when it carries its length (a stream's); otherwise its body runs to the end.
Restated restated_frame(const u8* data, usize size, bool sized, u16 ratio, u32 own_id, u32 least, u32 most)
{
    constexpr u64 all_u32 = 0xFFFFFFFFULL;
    Restated out;
    const auto wanting = [&](int read)
    {
        return read == 0 ? AdmitVerdict::NeedMore : AdmitVerdict::Malformed;
    };
    const auto end = [&](int read)
    {
        out.verdict = (read == 0 && !sized) ? AdmitVerdict::Malformed : wanting(read);
        return out;
    };
    u64 number = 0U;
    usize at = 0U;
    int read = restated_number(data, size, all_u32, number);
    if (read <= 0)
    {
        return end(read);
    }
    const u64 message = number;
    at += static_cast<usize>(read);
    u64 length = 0U;
    if (sized)
    {
        read = restated_number(data + at, size - at, all_u32, length);
        if (read <= 0)
        {
            return end(read);
        }
        at += static_cast<usize>(read);
    }
    if (message != kBlobId && message != kFlakeId)
    {
        out.verdict = AdmitVerdict::UnknownMessage;
        return out;
    }
    if (message != own_id)
    {
        out.verdict = AdmitVerdict::OtherChannel;
        return out;
    }
    const usize tail = at;
    read = restated_number(data + at, size - at, ~u64{0}, out.object);
    if (read <= 0)
    {
        return end(read);
    }
    at += static_cast<usize>(read);
    if (size - at < 4U)
    {
        return end(0);
    }
    out.hash = (u32{data[at]} << 24U) | (u32{data[at + 1U]} << 16U) | (u32{data[at + 2U]} << 8U) | data[at + 3U];
    at += 4U;
    read = restated_number(data + at, size - at, all_u32, out.expanded);
    if (read <= 0)
    {
        return end(read);
    }
    at += static_cast<usize>(read);
    out.header = at;
    if (sized)
    {
        if (at - tail > length)
        {
            out.verdict = AdmitVerdict::Malformed;
            return out;
        }
        out.body = length - (at - tail);
    }
    else
    {
        out.body = size - at;
    }
    if (out.expanded != 0U && (out.body == 0U || out.body >= out.expanded || out.expanded > out.body * ratio))
    {
        out.verdict = AdmitVerdict::BadExpansion;
        return out;
    }
    const u64 plain = (out.expanded != 0U) ? out.expanded : out.body;
    if (plain < least)
    {
        out.verdict = AdmitVerdict::Undersized;
    }
    else if (plain > most)
    {
        out.verdict = AdmitVerdict::Oversized;
    }
    else
    {
        out.verdict = AdmitVerdict::Admitted;
    }
    return out;
}

u8 g_decoded[kMaxMessage + 1U];

// What the body must come to: Admitted with the bytes in g_decoded, or the verdict that refuses it.
AdmitVerdict restated_body(const Restated& frame, const u8* body, u32 message, bool dictionary, ConstSpan<u8>& plain)
{
    const ConstSpan<u8> coded(body, static_cast<usize>(frame.body));
    plain = coded;
    if (frame.expanded != 0U)
    {
        const Span<u8> out(g_decoded, static_cast<usize>(frame.expanded));
        const bool decoded = dictionary ? models().plain_coder.decompress_exact(models().plain_dictionary, coded, out)
                                        : models().plain_entropy.decode(coded, out);
        if (!decoded)
        {
            return AdmitVerdict::BadExpansion;
        }
        plain = out;
    }
    const u64 hash = crd::containers::xxh3_64(plain, (u64{message} << 32U) ^ frame.object);
    return (static_cast<u32>(hash) == frame.hash) ? AdmitVerdict::Admitted : AdmitVerdict::IntegrityFailed;
}

void same_bytes(ConstSpan<u8> left, ConstSpan<u8> right)
{
    CRD_FUZZ_REQUIRE(left.size() == right.size());
    CRD_FUZZ_REQUIRE(left.empty() || std::memcmp(left.data(), right.data(), left.size()) == 0);
}

// ---- Any bytes on a stream --------------------------------------------------------------------------------------

void feed_stream(StreamAssembler& assembler, const u8* route, usize route_bytes)
{
    const FrameStep step = assembler.feed(ConstSpan<u8>(route, route_bytes), Delivery::Established, 0U);
    CRD_FUZZ_REQUIRE(step.consumed == route_bytes);
}

void check_stream(u8 split, const u8* data, usize size)
{
    Session whole;
    u8 route[8];
    const usize route_bytes = crd::net::write_route(Route{0U, RouteForm::Whole}, route);
    {
        StreamAssembler assembler(whole.admission);
        feed_stream(assembler, route, route_bytes);
        usize at = 0U;
        while (at < size)
        {
            const Restated expect =
                restated_frame(data + at, size - at, true, kDictionaryRatio, kBlobId, 0U, kBlobMost);
            const bool in_hand = expect.verdict == AdmitVerdict::Admitted && expect.body <= size - at - expect.header;
            const FrameStep step = assembler.feed(ConstSpan<u8>(data + at, size - at), Delivery::Established, 0U);
            if (expect.verdict == AdmitVerdict::NeedMore)
            {
                CRD_FUZZ_REQUIRE(step.verdict == AdmitVerdict::NeedMore);
                break;
            }
            if (expect.verdict != AdmitVerdict::Admitted)
            {
                CRD_FUZZ_REQUIRE(step.verdict == expect.verdict);
                CRD_FUZZ_REQUIRE(step.action == AdmitAction::Close);
                break;
            }
            if (!in_hand)
            {
                CRD_FUZZ_REQUIRE(step.verdict == AdmitVerdict::NeedMore);
                break;
            }
            ConstSpan<u8> plain;
            const AdmitVerdict body = restated_body(expect, data + at + expect.header, kBlobId, true, plain);
            CRD_FUZZ_REQUIRE(step.verdict == body);
            CRD_FUZZ_REQUIRE(step.consumed == expect.header + expect.body);
            if (body != AdmitVerdict::Admitted)
            {
                CRD_FUZZ_REQUIRE(step.action == AdmitAction::Close);
                break;
            }
            CRD_FUZZ_REQUIRE(step.action == AdmitAction::Deliver);
            same_bytes(step.frame.body, plain);
            whole.admission.finish(step.frame);
            at += step.consumed;
        }
    }
    whole.require_clear();

    // In pieces it delivers the same frames: nothing depends on where a carrier cut it.
    Session pieces;
    {
        StreamAssembler assembler(pieces.admission);
        feed_stream(assembler, route, route_bytes);
        const usize piece = 1U + (split % 23U);
        usize at = 0U;
        while (at < size)
        {
            const usize take = (size - at < piece) ? (size - at) : piece;
            const FrameStep step = assembler.feed(ConstSpan<u8>(data + at, take), Delivery::Established, 0U);
            if (step.action == AdmitAction::Close)
            {
                break;
            }
            CRD_FUZZ_REQUIRE(step.action != AdmitAction::Wait);
            CRD_FUZZ_REQUIRE(step.consumed != 0U);
            if (step.action == AdmitAction::Deliver)
            {
                pieces.admission.finish(step.frame);
            }
            at += step.consumed;
        }
    }
    pieces.require_clear();
    CRD_FUZZ_REQUIRE(pieces.admission.count(AdmitVerdict::Admitted) == whole.admission.count(AdmitVerdict::Admitted));
}

// ---- Any bytes in a datagram ------------------------------------------------------------------------------------

void check_datagram(const u8* data, usize size)
{
    Session session;
    u8 datagram[crd::fuzz::kMaxInputBytes + 8U];
    const usize route = crd::net::write_route(Route{1U, RouteForm::Whole}, datagram);
    std::memcpy(datagram + route, data, size);
    FragmentSlot slots[2];
    {
        DatagramAssembler assembler(session.admission, 1U, slots, 1000U);
        DatagramCursor cursor;
        CRD_FUZZ_REQUIRE(session.admission.open_datagram(ConstSpan<u8>(datagram, route + size), 0U, cursor) ==
                         AdmitVerdict::Admitted);
        const Restated expect = restated_frame(data, size, false, kEntropyRatio, kFlakeId, kFlakeLeast, kFlakeMost);
        const FrameStep step = assembler.next(cursor, Delivery::Established, 0U);
        if (expect.verdict != AdmitVerdict::Admitted)
        {
            CRD_FUZZ_REQUIRE(step.verdict == expect.verdict);
            CRD_FUZZ_REQUIRE(step.action == AdmitAction::Skip);
        }
        else
        {
            ConstSpan<u8> plain;
            const AdmitVerdict body = restated_body(expect, data + expect.header, kFlakeId, false, plain);
            CRD_FUZZ_REQUIRE(step.verdict == body);
            if (body == AdmitVerdict::Admitted)
            {
                same_bytes(step.frame.body, plain);
                session.admission.finish(step.frame);
            }
        }
    }
    session.require_clear();
}

// ---- A writer's frame, honest and damaged --------------------------------------------------------------------------

u8 g_delivered[kMaxMessage + 1U];

// One frame through a fresh session's assembler for its channel; `delivered` holds a copy of what came out, the frame's
// buffers being given back at once.
AdmitVerdict read_one(bool dictionary, const u8* bytes, usize size, ConstSpan<u8>& delivered)
{
    Session session;
    AdmitVerdict verdict = AdmitVerdict::NeedMore;
    const auto take = [&](const FrameStep& step)
    {
        verdict = step.verdict;
        if (step.action == AdmitAction::Deliver)
        {
            if (!step.frame.body.empty())
            {
                std::memcpy(g_delivered, step.frame.body.data(), step.frame.body.size());
            }
            delivered = ConstSpan<u8>(g_delivered, step.frame.body.size());
            session.admission.finish(step.frame);
        }
    };
    if (dictionary)
    {
        u8 route[8];
        const usize route_bytes = crd::net::write_route(Route{0U, RouteForm::Whole}, route);
        StreamAssembler assembler(session.admission);
        feed_stream(assembler, route, route_bytes);
        take(assembler.feed(ConstSpan<u8>(bytes, size), Delivery::Established, 0U));
    }
    else
    {
        u8 datagram[crd::net::kMaxFrameHeaderBytes + kMaxMessage + 8U];
        const usize route_bytes = crd::net::write_route(Route{1U, RouteForm::Whole}, datagram);
        std::memcpy(datagram + route_bytes, bytes, size);
        FragmentSlot slots[1];
        DatagramAssembler assembler(session.admission, 1U, slots, 1000U);
        DatagramCursor cursor;
        CRD_FUZZ_REQUIRE(session.admission.open_datagram(ConstSpan<u8>(datagram, route_bytes + size), 0U, cursor) ==
                         AdmitVerdict::Admitted);
        take(assembler.next(cursor, Delivery::Established, 0U));
    }
    session.require_clear();
    return verdict;
}

void check_honest(bool dictionary, u8 damage_at, const u8* data, usize size)
{
    const u32 most = dictionary ? kBlobMost : kFlakeMost;
    const u32 least = dictionary ? 0U : kFlakeLeast;
    if (size < least)
    {
        return;
    }
    const ConstSpan<u8> plain(data, (size < most) ? size : most);
    const u32 channel = dictionary ? 0U : 1U;
    const MessageDesc& message = models().messages[channel];
    const u16 ratio = dictionary ? kDictionaryRatio : kEntropyRatio;

    u8 frame[crd::net::kMaxFrameHeaderBytes + kMaxMessage];
    usize written = 0U;
    {
        Session session;
        CRD_FUZZ_REQUIRE(crd::net::write_coded_body(models().channels[channel], message,
                                                    session.admission.coding(channel),
                                                    dictionary ? HeaderForm::Sized : HeaderForm::ToEnd, 7U, plain,
                                                    frame, written) == WireError::Ok);
    }
    const Restated honest = restated_frame(frame, written, dictionary, ratio, message.id, least, most);
    CRD_FUZZ_REQUIRE(honest.verdict == AdmitVerdict::Admitted);
    CRD_FUZZ_REQUIRE(honest.header + honest.body == written);
    CRD_FUZZ_REQUIRE(honest.body <= plain.size()); // coding never makes a body larger than it travels stored
    ConstSpan<u8> restated;
    CRD_FUZZ_REQUIRE(restated_body(honest, frame + honest.header, message.id, dictionary, restated) ==
                     AdmitVerdict::Admitted);
    same_bytes(restated, plain);
    ConstSpan<u8> delivered;
    CRD_FUZZ_REQUIRE(read_one(dictionary, frame, written, delivered) == AdmitVerdict::Admitted);
    same_bytes(delivered, plain);

    // Damaged, the restated rule says what it comes to, and a body that still delivers is what its hash says.
    frame[damage_at % written] = static_cast<u8>(frame[damage_at % written] ^ (1U << (damage_at % 8U)));
    const Restated seen = restated_frame(frame, written, dictionary, ratio, message.id, least, most);
    AdmitVerdict expect = seen.verdict;
    if (seen.verdict == AdmitVerdict::Admitted)
    {
        expect = (seen.body > written - seen.header)
                     ? AdmitVerdict::NeedMore
                     : restated_body(seen, frame + seen.header, message.id, dictionary, restated);
    }
    delivered = ConstSpan<u8>{};
    CRD_FUZZ_REQUIRE(read_one(dictionary, frame, written, delivered) == expect);
    if (expect == AdmitVerdict::Admitted)
    {
        same_bytes(delivered, restated);
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 3U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const u8* rest = data + 3U;
    const usize left = size - 3U;
    switch (data[0] % 4U)
    {
        case 0U:
            check_stream(data[1], rest, left);
            break;
        case 1U:
            check_datagram(rest, left);
            break;
        case 2U:
            check_honest(true, data[2], rest, left);
            break;
        default:
            check_honest(false, data[2], rest, left);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    u8 sparse[3U + 600U] = {2U, 3U, 5U};
    for (usize i = 3U; i < sizeof(sparse); i += 11U)
    {
        sparse[i] = static_cast<u8>(i);
    }
    sink.add("honest-dictionary", sparse, sizeof(sparse));
    sparse[0] = 3U;
    sink.add("honest-entropy", sparse, sizeof(sparse));
    constexpr char text[] = "\x02\x07\x09the quick brown fox jumps over the lazy dog; pack my box with five dozen jugs";
    sink.add("honest-text", reinterpret_cast<const u8*>(text), sizeof(text) - 1U);
    // A stored frame and an expansion past the ratio on a stream; a datagram's expansion past its ratio; a short
    // header.
    const u8 stored[] = {0U, 5U, 0U, kBlobId, 7U, 9U, 1U, 2U, 3U, 4U, 0U, 0xAAU, 0xBBU};
    sink.add("stream-stored", stored, sizeof(stored));
    const u8 past[] = {0U, 1U, 0U, kBlobId, 8U, 9U, 1U, 2U, 3U, 4U, 0xFFU, 0x7FU, 0xAAU};
    sink.add("stream-past-ratio", past, sizeof(past));
    const u8 gram[] = {1U, 0U, 0U, kFlakeId, 9U, 1U, 2U, 3U, 4U, 40U, 0xAAU};
    sink.add("gram-past-ratio", gram, sizeof(gram));
    const u8 cut[] = {0U, 0U, 0U, kBlobId, 30U, 9U};
    sink.add("stream-cut", cut, sizeof(cut));
    const u8 other[] = {0U, 0U, 0U, kFlakeId, 3U, 9U, 1U, 2U};
    sink.add("stream-other-channel", other, sizeof(other));
}
