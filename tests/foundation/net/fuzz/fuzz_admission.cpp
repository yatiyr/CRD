// crd-fuzz-net-admission: frames and admission stages S6 and S7 of crd-net (NET.3;
// docs/design/network-foundation.md#frames) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the next a session's shape (the writer's minor, whether a capability is present
// and what it grants, the delivery), the rest is the mode's data.
//   0  a stream's bytes, read whole and read again at splits the input chooses;
//   1  one datagram's bytes;
//   2  a generated life: fragments of generated messages, lost, repeated and reordered as the input says.
//
// Oracles, none of them admission's own verdict:
//   * an independent restatement of the frame layout and the admission rule, with open budgets, decides the verdict and
//     the object, length and body of every frame of a stream or a datagram;
//   * a stream read at any split gives exactly the frames it gives read whole, with no buffer when read whole;
//   * a generated message is delivered at most once, with exactly its bytes, and never one a refused first piece
//   opened;
//   * every quota, pending place, stream and buffer comes back, whatever happened; a refused frame takes no buffer.
#include <crd/fuzz/harness.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/net/reassembly.hpp>

#include <cstring>

namespace
{
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
using crd::memory::TlsfAllocator;
using crd::net::Admission;
using crd::net::AdmitAction;
using crd::net::AdmitVerdict;
using crd::net::ChannelState;
using crd::net::DatagramAssembler;
using crd::net::DatagramCursor;
using crd::net::FragmentSlot;
using crd::net::FrameHeader;
using crd::net::FrameStep;
using crd::net::PrincipalQuota;
using crd::net::QuotaLimits;
using crd::net::SessionSchema;
using crd::net::StreamAssembler;
using crd::net::wire::ChannelClass;
using crd::net::wire::ChannelDesc;
using crd::net::wire::MessageDesc;
using crd::net::wire::WireSchema;

// Budgets far above anything one input can spend: the restated rule covers shape and authority, not rates.
constexpr u64 kWide = 1ULL << 40U;

constexpr ChannelDesc kChannels[] = {
    {"control", ChannelClass::Control, true, true, false, 0U, 4000U, 1U << 30U, kWide, 0U},
    {"bulk", ChannelClass::Bulk, true, false, false, 4U, 4000U, 1U << 30U, kWide, 0U},
    {"state", ChannelClass::State, false, false, true, 3U, 4000U, 1U << 30U, kWide, 0U},
    {"media", ChannelClass::Media, false, false, false, 5U, 4000U, 1U << 30U, kWide, 0U},
};

constexpr MessageDesc kMessages[] = {
    {"Hello", 1U, 0U, 0U, 1ULL, 2U, 100U},  {"Late", 2U, 0U, 1U, 1ULL, 0U, 100U},
    {"Blob", 3U, 1U, 0U, 2ULL, 0U, 4000U},  {"Snapshot", 4U, 2U, 0U, 4ULL, 0U, 3000U},
    {"Voice", 5U, 3U, 0U, 8ULL, 1U, 4000U}, {"Wide", 200U, 0U, 0U, 16ULL, 0U, 50U},
};

constexpr WireSchema kSchema{
    "fuzz",
    {0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U, 0x70U, 0x80U, 0x90U, 0xA0U, 0xB0U, 0xC0U, 0xD0U, 0xE0U, 0xF0U, 0x01U},
    static_cast<u8>(ScopeKind::World),
    1U,
    1U,
    31ULL,
    4ULL,
    kChannels,
    kMessages,
};

constexpr u32 kSchemaMinor = 1U;

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
        ++total;
        ++live;
        return m_heap.try_allocate(size, alignment);
    }

    u32 total = 0U;
    u32 live = 0U;

private:
    TlsfAllocator m_heap;
};

struct Shape
{
    u32 writer_minor = 1U;
    bool has_capability = true;
    u64 granted = ~u64{0};
    bool one_range = false;
    Delivery delivery = Delivery::Established;

    explicit Shape(u8 byte)
    {
        writer_minor = byte & 3U; // 0 and 1 known, 2 and 3 newer than this reader
        has_capability = (byte & 4U) == 0U;
        granted = ((byte & 8U) != 0U) ? 3ULL : ~u64{0};
        one_range = (byte & 16U) != 0U;
        delivery = ((byte & 32U) != 0U) ? Delivery::Early : Delivery::Established;
    }
};

struct Session
{
    explicit Session(const Shape& shape)
        : quota(QuotaLimits{1000U, kWide, kWide, kWide, kWide, 1U << 20U}), admission(quota, buffers, channels)
    {
        authorized.schema.kind = ScopeKind::World;
        std::memcpy(authorized.schema.schema.bytes, kSchema.id, sizeof(kSchema.id));
        authorized.schema.major = kSchema.major;
        authorized.schema.minor = kSchema.minor;
        authorized.schema.audience.bytes[0] = 1U;
        authorized.schema.operations = kSchema.operations;
        authorized.schema.idempotent = kSchema.idempotent;
        capability.audience = authorized.schema.audience;
        capability.grant.kind = ScopeKind::World;
        capability.grant.operations = shape.granted;
        capability.grant.budget.operations_per_second = 1U << 30U;
        capability.grant.budget.bytes_per_second = kWide;
        if (shape.one_range)
        {
            capability.grant.ranges[0] = crd::crypto::ObjectRange{10U, 1000U};
            capability.grant.range_count = 1U;
        }
        const SessionSchema schemas[] = {{&kSchema, &authorized}};
        CRD_FUZZ_REQUIRE(admission.bind(schemas));
        CRD_FUZZ_REQUIRE(admission.announce(ConstSpan<u8>(kSchema.id, sizeof(kSchema.id)), kSchema.major,
                                            static_cast<crd::u16>(shape.writer_minor)));
        admission.set_capability(shape.has_capability ? &capability : nullptr);
    }

    void require_clear()
    {
        CRD_FUZZ_REQUIRE(quota.reassembly.held() == 0U);
        CRD_FUZZ_REQUIRE(quota.pending.held() == 0U);
        CRD_FUZZ_REQUIRE(quota.streams.held() == 0U);
        CRD_FUZZ_REQUIRE(buffers.live == 0U);
    }

    Buffers buffers;
    PrincipalQuota quota;
    ChannelState channels[8];
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
    const usize limit = (bits == 0U) ? 1U : ((bits + 6U) / 7U);
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

struct RestatedFrame
{
    AdmitVerdict verdict = AdmitVerdict::NeedMore;
    u64 object = 0U;
    u32 message = 0U;
    u32 length = 0U;
    usize header = 0U;
};

// The authority's rule, restated, for a message that passed the budgets.
AdmitVerdict restated_authority(const MessageDesc& message, u64 object, const Shape& shape)
{
    if (!shape.has_capability)
    {
        return AdmitVerdict::NoCapability;
    }
    if (shape.delivery == Delivery::Early && (message.operation & kSchema.idempotent) != message.operation)
    {
        return AdmitVerdict::Replayable;
    }
    const bool in_ranges = !shape.one_range || (object >= 10U && object <= 1000U);
    return ((message.operation & shape.granted) == message.operation && in_ranges) ? AdmitVerdict::Admitted
                                                                                   : AdmitVerdict::NotGranted;
}

const MessageDesc* restated_find(u64 id)
{
    for (const MessageDesc& message : kMessages)
    {
        if (message.id == id)
        {
            return &message;
        }
    }
    return nullptr;
}

// A frame's header on `channel`: Sized when `sized`, else its body runs to the end.
RestatedFrame restated_read(const u8* data, usize size, u32 channel, bool sized, const Shape& shape)
{
    RestatedFrame out;
    const AdmitVerdict short_input = sized ? AdmitVerdict::NeedMore : AdmitVerdict::Malformed;
    u64 message = 0U;
    int taken = restated_number(data, size, 0xFFFFFFFFU, message);
    if (taken <= 0)
    {
        out.verdict = (taken == 0) ? short_input : AdmitVerdict::Malformed;
        return out;
    }
    usize at = static_cast<usize>(taken);
    u64 length = 0U;
    if (sized)
    {
        taken = restated_number(data + at, size - at, 0xFFFFFFFFU, length);
        if (taken <= 0)
        {
            out.verdict = (taken == 0) ? short_input : AdmitVerdict::Malformed;
            return out;
        }
        at += static_cast<usize>(taken);
    }
    out.message = static_cast<u32>(message);
    out.length = static_cast<u32>(length);
    const MessageDesc* described = restated_find(message);
    const bool newer = shape.writer_minor > kSchemaMinor;
    if (described == nullptr)
    {
        out.header = at;
        if (!newer)
        {
            out.verdict = AdmitVerdict::UnknownMessage;
        }
        else
        {
            out.verdict =
                (length > kChannels[channel].max_message_bytes) ? AdmitVerdict::Oversized : AdmitVerdict::NewerMessage;
        }
        return out;
    }
    if (described->since > shape.writer_minor)
    {
        out.verdict = AdmitVerdict::NotInRevision;
        return out;
    }
    if (described->channel != channel)
    {
        out.verdict = AdmitVerdict::OtherChannel;
        return out;
    }
    if (described->operation != 0U)
    {
        taken = restated_number(data + at, size - at, ~u64{0}, out.object);
        if (taken <= 0)
        {
            out.verdict = (taken == 0) ? short_input : AdmitVerdict::Malformed;
            return out;
        }
        at += static_cast<usize>(taken);
    }
    out.header = at;
    if (!sized)
    {
        length = size - at;
        out.length = static_cast<u32>(length);
    }
    if (length < described->min_bytes)
    {
        out.verdict = AdmitVerdict::Undersized;
    }
    else if ((!newer && length > described->max_bytes) || length > kChannels[channel].max_message_bytes)
    {
        out.verdict = AdmitVerdict::Oversized;
    }
    else
    {
        out.verdict = restated_authority(*described, out.object, shape);
    }
    return out;
}

// What one reading of a stream delivered and refused.
struct Trace
{
    u32 frames = 0U;
    u32 refused = 0U;
    u64 digest = 0xCBF29CE484222325ULL;
    AdmitVerdict closed_by = AdmitVerdict::Admitted;

    void add(u64 value) { digest = (digest ^ value) * 0x100000001B3ULL; }
};

// Reads the stream, handing the assembler pieces as the splits say, or all of it when there are none.
Trace read_stream(const Shape& shape, const u8* data, usize size, const u8* splits, usize split_count, Session& session)
{
    Trace trace;
    {
        StreamAssembler assembler(session.admission);
        usize at = 0U;
        usize split = 0U;
        while (at < size && trace.closed_by == AdmitVerdict::Admitted)
        {
            usize give = size - at;
            if (split_count != 0U)
            {
                const usize want = (splits[split % split_count] % 32U) + 1U;
                give = (want < give) ? want : give;
                ++split;
            }
            usize read = 0U;
            while (read < give && trace.closed_by == AdmitVerdict::Admitted)
            {
                const FrameStep step = assembler.feed(ConstSpan<u8>(data + at + read, give - read), shape.delivery, 0U);
                read += step.consumed;
                CRD_FUZZ_REQUIRE(step.action != AdmitAction::Wait); // the budgets are open
                if (step.action == AdmitAction::Deliver)
                {
                    trace.add(step.frame.message->id);
                    trace.add(step.frame.object);
                    trace.add(step.frame.length);
                    CRD_FUZZ_REQUIRE(step.frame.body.size() == step.frame.length);
                    for (const u8 byte : step.frame.body)
                    {
                        trace.add(byte);
                    }
                    ++trace.frames;
                    session.admission.finish(step.frame);
                }
                else if (step.action == AdmitAction::Skip)
                {
                    trace.add(static_cast<u64>(step.verdict));
                    ++trace.refused;
                }
                else if (step.action == AdmitAction::Close)
                {
                    trace.closed_by = step.verdict;
                }
            }
            at += give;
        }
    }
    session.require_clear();
    return trace;
}

// The restated walk of a stream, fed whole: the frames it delivers, their sum and how it ends.
Trace model_stream(const Shape& shape, const u8* data, usize size)
{
    Trace trace;
    u64 route = 0U;
    const int taken = restated_number(data, size, 0x3FFFFFFFFULL, route);
    if (taken == 0)
    {
        return trace;
    }
    if (taken < 0 || (route & 3U) != 0U)
    {
        trace.closed_by = AdmitVerdict::Malformed;
        return trace;
    }
    const u64 channel = route >> 2U;
    if (channel >= 4U)
    {
        trace.closed_by = AdmitVerdict::BadChannel;
        return trace;
    }
    if (!kChannels[channel].reliable)
    {
        trace.closed_by = AdmitVerdict::WrongCarrier;
        return trace;
    }
    usize at = static_cast<usize>(taken);
    while (at < size)
    {
        const RestatedFrame frame = restated_read(data + at, size - at, static_cast<u32>(channel), true, shape);
        if (frame.verdict == AdmitVerdict::NeedMore)
        {
            return trace;
        }
        if (frame.verdict == AdmitVerdict::Admitted)
        {
            const usize left = size - at - frame.header;
            if (left < frame.length)
            {
                return trace; // reassembling: it never completes
            }
            trace.add(frame.message);
            trace.add(frame.object);
            trace.add(frame.length);
            for (usize i = 0U; i < frame.length; ++i)
            {
                trace.add(data[at + frame.header + i]);
            }
            ++trace.frames;
            at += frame.header + frame.length;
        }
        else if (frame.verdict == AdmitVerdict::NewerMessage || frame.verdict == AdmitVerdict::NoCapability ||
                 frame.verdict == AdmitVerdict::Replayable || frame.verdict == AdmitVerdict::NotGranted)
        {
            trace.add(static_cast<u64>(frame.verdict));
            ++trace.refused;
            const usize left = size - at - frame.header;
            at += frame.header + ((left < frame.length) ? left : frame.length);
        }
        else
        {
            trace.closed_by = frame.verdict;
            return trace;
        }
    }
    return trace;
}

void check_stream(const Shape& shape, const u8* data, usize size)
{
    // The first bytes choose the splits, the rest is the stream.
    const usize split_count = (size == 0U) ? 0U : (data[0] % 16U);
    if (size < split_count + 1U)
    {
        return;
    }
    const u8* splits = data + 1U;
    const u8* stream = data + 1U + split_count;
    const usize length = size - 1U - split_count;

    const Trace expected = model_stream(shape, stream, length);

    Session whole_session(shape);
    const Trace whole = read_stream(shape, stream, length, nullptr, 0U, whole_session);
    CRD_FUZZ_REQUIRE(whole.frames == expected.frames);
    CRD_FUZZ_REQUIRE(whole.refused == expected.refused);
    CRD_FUZZ_REQUIRE(whole.digest == expected.digest);
    CRD_FUZZ_REQUIRE(whole.closed_by == expected.closed_by);

    if (split_count != 0U)
    {
        Session split_session(shape);
        const Trace pieces = read_stream(shape, stream, length, splits, split_count, split_session);
        CRD_FUZZ_REQUIRE(pieces.frames == expected.frames);
        CRD_FUZZ_REQUIRE(pieces.refused == expected.refused);
        CRD_FUZZ_REQUIRE(pieces.digest == expected.digest);
        CRD_FUZZ_REQUIRE(pieces.closed_by == expected.closed_by);
    }
}

// ---- One datagram -------------------------------------------------------------------------------------------------

void check_datagram(const Shape& shape, const u8* data, usize size)
{
    Session session(shape);
    {
        FragmentSlot slots[4];
        DatagramCursor cursor;
        const AdmitVerdict opened = session.admission.open_datagram(ConstSpan<u8>(data, size), 0U, cursor);

        u64 route = 0U;
        const int taken = restated_number(data, size, 0x3FFFFFFFFULL, route);
        if (taken <= 0 || (route & 3U) == 3U)
        {
            CRD_FUZZ_REQUIRE(opened == AdmitVerdict::Malformed);
            return;
        }
        const u64 channel = route >> 2U;
        if (channel >= 4U)
        {
            CRD_FUZZ_REQUIRE(opened == AdmitVerdict::BadChannel);
            return;
        }
        if (kChannels[channel].reliable)
        {
            CRD_FUZZ_REQUIRE(opened == AdmitVerdict::WrongCarrier);
            return;
        }
        CRD_FUZZ_REQUIRE(opened == AdmitVerdict::Admitted);
        CRD_FUZZ_REQUIRE(cursor.route.channel == channel);

        DatagramAssembler assembler(session.admission, static_cast<u32>(channel), slots, crd::net::kNanosPerSecond);
        const u64 form = route & 3U;
        usize at = static_cast<usize>(taken);
        if (form == 0U)
        {
            const RestatedFrame expected = restated_read(data + at, size - at, static_cast<u32>(channel), false, shape);
            const FrameStep step = assembler.next(cursor, shape.delivery, 0U);
            CRD_FUZZ_REQUIRE(!cursor.more);
            CRD_FUZZ_REQUIRE(step.verdict == expected.verdict);
            if (step.action == AdmitAction::Deliver)
            {
                CRD_FUZZ_REQUIRE(step.frame.object == expected.object);
                CRD_FUZZ_REQUIRE(step.frame.body.data() == data + at + expected.header);
                CRD_FUZZ_REQUIRE(step.frame.body.size() == expected.length);
                session.admission.finish(step.frame);
            }
        }
        else if (form == 1U)
        {
            bool first = true;
            while (cursor.more)
            {
                const RestatedFrame expected =
                    restated_read(data + at, size - at, static_cast<u32>(channel), true, shape);
                const FrameStep step = assembler.next(cursor, shape.delivery, 0U);
                const usize left = size - at - ((expected.header <= size - at) ? expected.header : 0U);
                const bool steps =
                    expected.verdict == AdmitVerdict::Admitted || expected.verdict == AdmitVerdict::NewerMessage ||
                    expected.verdict == AdmitVerdict::NoCapability || expected.verdict == AdmitVerdict::Replayable ||
                    expected.verdict == AdmitVerdict::NotGranted;
                if (!steps || expected.length > left || (first && expected.length == left))
                {
                    CRD_FUZZ_REQUIRE(!cursor.more);
                    CRD_FUZZ_REQUIRE(step.action != AdmitAction::Deliver);
                    const AdmitVerdict want = (expected.verdict == AdmitVerdict::NeedMore || steps)
                                                  ? AdmitVerdict::Malformed
                                                  : expected.verdict;
                    CRD_FUZZ_REQUIRE(step.verdict == want);
                    break;
                }
                CRD_FUZZ_REQUIRE(step.verdict == expected.verdict);
                if (step.action == AdmitAction::Deliver)
                {
                    CRD_FUZZ_REQUIRE(step.frame.object == expected.object);
                    CRD_FUZZ_REQUIRE(step.frame.body.data() == data + at + expected.header);
                    CRD_FUZZ_REQUIRE(step.frame.body.size() == expected.length);
                    session.admission.finish(step.frame);
                }
                at += expected.header + expected.length;
                first = false;
            }
        }
        else
        {
            const u32 before = session.buffers.total;
            const FrameStep step = assembler.next(cursor, shape.delivery, 0U);
            CRD_FUZZ_REQUIRE(!cursor.more);
            CRD_FUZZ_REQUIRE(step.action != AdmitAction::Close);
            // Only an admitted first piece takes a buffer, and one piece of several never completes a message.
            CRD_FUZZ_REQUIRE(step.action != AdmitAction::Deliver);
            if (step.verdict != AdmitVerdict::Buffered)
            {
                CRD_FUZZ_REQUIRE(session.buffers.total == before);
            }
            CRD_FUZZ_REQUIRE(session.quota.reassembly.held() <= 4000U);
        }
    }
    session.require_clear();
}

// ---- A generated life: fragments on a hostile path
// -------------------------------------------------------------------

struct Words
{
    const u8* data;
    usize size;
    usize at = 0U;

    u32 next()
    {
        u32 value = 0U;
        for (u32 i = 0U; i < 2U; ++i)
        {
            value = (value << 8U) | ((at < size) ? data[at] : 0U);
            ++at;
        }
        return value;
    }
};

u8 body_byte(u64 sequence, u32 offset)
{
    return static_cast<u8>((sequence * 131U) + (offset * 7U) + (offset >> 8U));
}

void check_life(const Shape& shape, const u8* data, usize size)
{
    Words words{data, size};
    Session session(shape);
    {
        constexpr u32 channel = 3U; // media
        FragmentSlot slots[4];
        DatagramAssembler assembler(session.admission, channel, slots, 50U);

        static u8 body[4000];
        static u8 datagram[1200];

        constexpr u32 messages_lived = 12U;
        bool delivered[messages_lived] = {};
        u32 totals[messages_lived] = {};
        u64 now = 0U;

        for (u32 round = 0U; round < 48U && words.at < words.size; ++round)
        {
            const u32 sequence = words.next() % messages_lived;
            if (totals[sequence] == 0U)
            {
                totals[sequence] = 200U + (words.next() % 3800U);
            }
            const u32 total = totals[sequence];
            const bool admitted = shape.has_capability && (shape.granted & 8U) != 0U &&
                                  (!shape.one_range || sequence >= 10U) && shape.delivery == Delivery::Established;
            const u32 chunk = 64U + (((sequence * 37U) + 11U) % 900U);
            const u32 pieces = crd::net::fragment_count(total, chunk);
            if (pieces > crd::net::kMaxFragments || pieces < 2U)
            {
                continue;
            }
            for (u32 offset = 0U; offset < total; ++offset)
            {
                body[offset] = body_byte(sequence, offset);
            }
            FrameHeader header;
            header.message = 5U;
            header.length = total;
            header.object = sequence;
            header.chunk = chunk;
            header.sequence = sequence;
            for (u32 piece = 0U; piece < pieces; ++piece)
            {
                const u32 rule = words.next();
                // Lost, kept or repeated, as the input says.
                u32 copies = 1U;
                if ((rule & 7U) == 0U)
                {
                    copies = 0U;
                }
                else if ((rule & 7U) == 1U)
                {
                    copies = 2U;
                }
                // Backwards when the input says.
                header.index = ((rule & 8U) != 0U) ? (pieces - 1U - piece) : piece;
                now += (rule >> 12U);
                for (u32 copy = 0U; copy < copies; ++copy)
                {
                    const usize written = crd::net::write_fragment(channel, true, header, ConstSpan<u8>(body, total),
                                                                   Span<u8>(datagram, sizeof(datagram)));
                    CRD_FUZZ_REQUIRE(written != 0U);
                    DatagramCursor cursor;
                    CRD_FUZZ_REQUIRE(session.admission.open_datagram(ConstSpan<u8>(datagram, written), now, cursor) ==
                                     AdmitVerdict::Admitted);
                    const u32 before = session.buffers.total;
                    const FrameStep step = assembler.next(cursor, shape.delivery, now);
                    CRD_FUZZ_REQUIRE(step.verdict != AdmitVerdict::BadFragment);
                    CRD_FUZZ_REQUIRE(step.verdict != AdmitVerdict::Malformed);
                    CRD_FUZZ_REQUIRE(step.verdict != AdmitVerdict::FragmentConflict);
                    if (!admitted)
                    {
                        CRD_FUZZ_REQUIRE(step.action != AdmitAction::Deliver);
                        CRD_FUZZ_REQUIRE(session.buffers.total == before); // a refused piece takes no memory
                    }
                    if (step.action == AdmitAction::Deliver)
                    {
                        CRD_FUZZ_REQUIRE(!delivered[sequence]);
                        delivered[sequence] = true;
                        CRD_FUZZ_REQUIRE(step.frame.object == sequence);
                        CRD_FUZZ_REQUIRE(step.frame.body.size() == total);
                        for (u32 offset = 0U; offset < total; ++offset)
                        {
                            CRD_FUZZ_REQUIRE(step.frame.body[offset] == body_byte(sequence, offset));
                        }
                        session.admission.finish(step.frame);
                    }
                }
            }
            // A message whole is a message done: `delivered` holds a repeat of its sequence to never again.
            CRD_FUZZ_REQUIRE(session.quota.reassembly.held() <= 4U * 4000U);
        }
    }
    session.require_clear();
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const Shape shape(data[1]);
    switch (data[0] % 3U)
    {
        case 0U:
            check_stream(shape, data + 2U, size - 2U);
            break;
        case 1U:
            check_datagram(shape, data + 2U, size - 2U);
            break;
        default:
            check_life(shape, data + 2U, size - 2U);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // After the mode and the shape, a stream's input is its split count, its splits, its route and its frames.
    // The control channel in pieces: Hello, Late, then a message of another channel.
    const u8 stream[] = {0U, 1U, 3U, 1U, 2U, 5U, 0U, 1U, 2U, 3U, 0xAAU, 0xBBU, 2U, 0U, 3U, 3U, 2U, 1U, 7U, 8U};
    sink.add("stream-control", stream, sizeof(stream));
    // The bulk channel in small pieces, with a body that never completes.
    const u8 split[] = {0U, 1U, 3U, 5U, 4U, 3U, 4U, 3U, 0xFFU, 0x01U, 9U, 1U, 2U, 3U};
    sink.add("stream-split", split, sizeof(split));
    // A newer writer: a message this reader has not heard of is stepped over.
    const u8 newer[] = {0U, 2U, 0U, 0U, 77U, 2U, 9U, 9U, 1U, 2U, 3U, 0xAAU, 0xBBU};
    sink.add("stream-newer", newer, sizeof(newer));
    // No capability; early data.
    const u8 refused[] = {0U, 5U, 0U, 0U, 1U, 2U, 3U, 0xAAU, 0xBBU};
    sink.add("stream-no-capability", refused, sizeof(refused));
    const u8 early[] = {0U, 0x21U, 0U, 0U, 1U, 2U, 3U, 0xAAU, 0xBBU};
    sink.add("stream-early", early, sizeof(early));
    // Whole, listed and fragmented datagrams on state and media.
    const u8 whole[] = {1U, 1U, 8U, 4U, 3U, 1U, 2U, 3U};
    sink.add("datagram-whole", whole, sizeof(whole));
    const u8 listed[] = {1U, 1U, 9U, 4U, 1U, 3U, 0xAAU, 4U, 2U, 4U, 0xBBU, 0xCCU};
    sink.add("datagram-listed", listed, sizeof(listed));
    u8 fragment[100] = {1U, 1U, 14U, 5U, 0xC8U, 1U, 3U, 64U, 2U, 0U};
    sink.add("datagram-fragment", fragment, 10U + 64U);
    const u8 reliable[] = {1U, 1U, 0U, 1U, 2U};
    sink.add("datagram-reliable-channel", reliable, sizeof(reliable));
    // Lives: all kept, some lost, repeated and backwards.
    u8 kept[64] = {2U, 1U};
    std::memset(kept + 2U, 2, sizeof(kept) - 2U);
    sink.add("life-kept", kept, sizeof(kept));
    u8 lost[96] = {2U, 1U};
    for (usize i = 2U; i < sizeof(lost); ++i)
    {
        lost[i] = static_cast<u8>((i * 37U) + 5U);
    }
    sink.add("life-hostile", lost, sizeof(lost));
    u8 denied[64] = {2U, 9U};
    std::memset(denied + 2U, 2, sizeof(denied) - 2U);
    sink.add("life-denied", denied, sizeof(denied));
}
