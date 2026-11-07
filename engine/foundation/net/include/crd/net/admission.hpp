#pragma once

// crd-net -- stages S6 and S7 of bounded admission (NET.3, threats T07, T09 and T11; contract:
// docs/design/network-foundation.md#frames). A frame names its message and object before its body. Its declared length
// is held to the schema, the channel's budgets, the principal's quota and the capability's budget before any buffer is
// reserved, and S18's verdict is asked before a generated decoder reads a field. Every refusal is counted and none of
// them answers the peer; a stage that answers says so.
//
// An Admission is one session's: one I/O loop drives it. Its principal's quota may be shared with the principal's other
// sessions. Nothing here allocates except a reassembly, which asks the injected allocator's try_allocate for exactly
// what the quota has already reserved, and time arrives as an argument (network foundation, Transport).

#include <crd/crypto/capability.hpp>
#include <crd/net/coding.hpp>
#include <crd/net/frame.hpp>
#include <crd/net/quota.hpp>
#include <crd/net/wire.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{

class DeltaReader;
enum class DeltaRead : u8;

enum class AdmitVerdict : u8
{
    Admitted,
    NeedMore, // the frame is not all here yet: read on
    // The frame itself is wrong.
    Malformed,      // a number overlong or out of range, a header past the end of its datagram
    BadChannel,     // the route names no channel of the session
    WrongCarrier,   // a reliable channel in a datagram, or an unreliable one on a stream
    Unannounced,    // a schema the session has not announced its revision of
    UnknownMessage, // a message no revision up to the writer's has
    NewerMessage,   // a message of a revision newer than this reader's: stepped over, by its length
    NotInRevision,  // a message newer than the revision its writer announced
    OtherChannel,   // a message of another channel of the schema
    Undersized,     // shorter than the message's least
    Oversized,      // longer than the message's most at the writer's revision, or than the channel's most
    BadFragment,    // a fragment of a shape no encoder writes, or one of several in a datagram
    // S6: what the sender may spend.
    ChannelMessages, // the channel's messages_per_s
    ChannelBytes,    // the channel's bytes_per_s
    PrincipalDatagrams,
    PrincipalMessages,
    PrincipalBytes,
    BudgetOperations, // the capability's operations_per_second
    BudgetBytes,      // the capability's bytes_per_second
    PendingFull,      // the principal's admitted messages nothing has consumed yet
    ReassemblyFull,   // the principal's bytes held in partial messages
    NoMemory,         // the allocator refused what the quota allowed
    StreamLimit,      // the principal's open streams
    // S7: S18's verdict.
    NoCapability, // a schema above S18 with no capability, or no authorized schema
    OtherSchema,
    Undeclared,
    Replayable,
    NotGranted,
    // Fragments that change nothing.
    Buffered,          // a piece stored; the message is not whole yet
    StaleFragment,     // of a message already delivered, abandoned or superseded
    DuplicateFragment, // a piece already stored
    FragmentConflict,  // a piece that disagrees with what its message's first piece said
    // After the stages.
    IntegrityFailed, // the body is not what its writer hashed: nothing reads it
    PaddingFrame,    // a stream's padding: nothing to deliver
    BadPadding,      // padding that is not zero, or larger than its channel's block allows
    BadExpansion,    // a coded body past its channel's ratio, or one that does not decode to its declared size
};

inline constexpr usize kAdmitVerdictCount = static_cast<usize>(AdmitVerdict::BadExpansion) + 1U;

// "admitted", "need-more", "malformed", ... one name per verdict.
[[nodiscard]] cont::StringView admit_verdict_name(AdmitVerdict verdict) noexcept;

// What the caller does next.
enum class AdmitAction : u8
{
    Deliver,  // the frame is admitted: decode it and execute it
    Continue, // nothing to do: read on
    Wait,  // read nothing more on this carrier until retry_at_ns, or until finish() frees room, and read the same bytes
           // again: flow control then holds the sender back
    Skip,  // this frame or datagram is dropped: read on
    Close, // framing is lost or the peer broke the protocol: end the session (GoAway ProtocolError)
};

// A verdict's action on a reliable or an unreliable channel. A reliable channel cannot lose what it declared, so a
// limit holds its reader back and a message that cannot be stepped over ends the session; an S7 refusal keeps the
// framing, so it is stepped over. An unreliable channel just drops.
[[nodiscard]] AdmitAction admit_action(AdmitVerdict verdict, bool reliable) noexcept;

// One schema of a session. A schema above S18 has the verified artefact of its own identifier, kind and major; a
// transport schema has none. A schema whose channels declare models has each of them, compiled, and the loop's coder.
// All of them outlive the Admission.
struct SessionSchema
{
    const wire::WireSchema* wire = nullptr;
    const crypto::AuthorizedSchema* authorized = nullptr;
    cont::ConstSpan<const BodyModel*> models = {};
    BodyCoder* coder = nullptr;
};

inline constexpr usize kMaxSessionSchemas = 4U;

// One channel's admission state, kept by the session's pool.
struct ChannelState
{
    const wire::ChannelDesc* desc = nullptr;
    u32 slot = 0U;  // its schema
    u32 index = 0U; // within its schema
    RateGate messages;
    RateGate bytes;
    const wire::CodecDesc* codec = nullptr; // what it codes with at the session's revisions, once they are known
    const BodyModel* model = nullptr;
};

// A frame that passed S6 and S7.
struct AdmittedFrame
{
    const wire::WireSchema* schema = nullptr;
    const wire::MessageDesc* message = nullptr;
    u64 object = 0U;
    u32 channel = 0U; // in the session's flat table
    u16 writer_minor = 0U;
    u32 length = 0U;
    u32 hash = 0U;
    bool hashed = false;
    cont::ConstSpan<u8> body; // the input's bytes, or `buffer`'s
    bool reassembled = false; // it holds a reservation of the principal's reassembly quota
    u8* buffer = nullptr;     // its buffer, once allocated; finish() frees it
    u32 expanded = 0U;        // a coded body's size once decoded: it holds a reservation of that too
    u8* expansion = nullptr;  // where it decodes to; finish() frees it
};

// The result of one read.
struct FrameStep
{
    AdmitVerdict verdict = AdmitVerdict::NeedMore;
    AdmitAction action = AdmitAction::Continue;
    usize consumed = 0U;  // of the bytes given
    u64 retry_at_ns = 0U; // with Wait: the first time the same read can pass; kNeverNs until finish() frees room
    AdmittedFrame frame;  // with Deliver
};

// A datagram being read.
struct DatagramCursor
{
    Route route;
    cont::ConstSpan<u8> bytes;
    usize at = 0U;
    bool more = false; // whether next() has anything left to read
    bool first = true;
};

// A header read and understood, not yet admitted.
struct ParsedFrame
{
    const wire::MessageDesc* message = nullptr;
    FrameHeader header;
    usize header_bytes = 0U;
    bool hashed = false;
};

// What a message's header holds on its channel, `coded` saying whether the channel codes at the session's revisions. A
// delta body hashes the message it rebuilds and codes its own payload, so its header needs neither.
[[nodiscard]] constexpr FrameShape frame_shape(const wire::ChannelDesc& channel, const wire::MessageDesc& message,
                                               bool coded = false) noexcept
{
    return FrameShape{message.operation != 0U, channel.integrity && message.delta == nullptr,
                      coded && message.delta == nullptr};
}

class Admission
{
public:
    // `channels` is the session's storage for its channels' state, from the session's pool. `allocator` provides the
    // buffers of reassemblies.
    Admission(PrincipalQuota& quota, memory::IAllocator& allocator, cont::Span<ChannelState> channels) noexcept;

    // The session's schemas, in the order that gives their channels' flat numbers. False, binding nothing, for more
    // than kMaxSessionSchemas, too little channel storage, a missing schema, a schema above S18 without its own
    // verified artefact, a transport schema with one or with models, or a declared model not held or with no coder.
    [[nodiscard]] bool bind(cont::ConstSpan<SessionSchema> schemas) noexcept;

    // The revision the peer writes of a schema (RevisionAnnounce). False for a schema the session does not hold,
    // another major, or a second announcement that differs: the session then ends with GoAway RevisionRefused. A
    // transport schema is readable before its announcement, at its first minor revision, which is how the announcement
    // itself travels; a schema above S18 is not. It settles what the schema's channels code with: the newest model of
    // the lower of the two revisions.
    [[nodiscard]] bool announce(cont::ConstSpan<u8> schema_id, u16 major, u16 writer_minor) noexcept;

    // The capability this session presents, verified, or null when none is current. The session withdraws it when it
    // expires or a revocation generation refuses it, so S7 then refuses; renewal replaces it and keeps what was spent.
    void set_capability(const crypto::Capability* capability) noexcept;

    // A channel's flat number, for a sender's route.
    [[nodiscard]] u32 channel_number(u32 schema_slot, u32 channel_index) const noexcept;

    [[nodiscard]] u32 channel_count() const noexcept { return m_channel_count; }

    [[nodiscard]] const ChannelState& channel(u32 number) const noexcept { return m_channels[number]; }

    // What a channel's writers and readers code with at the session's revisions.
    [[nodiscard]] BodyCoding coding(u32 number) const noexcept;

    // A stream's preamble has been read: whether the route may open a stream, taking one of the principal's streams.
    [[nodiscard]] AdmitVerdict open_stream(const Route& route) noexcept;

    // Gives back what open_stream() took.
    void close_stream() noexcept;

    // Reads a datagram's route, charges the principal's datagrams and checks the carrier, and takes off any padding,
    // charged to the channel and the principal as bytes; the frames follow through the channel's DatagramAssembler.
    [[nodiscard]] AdmitVerdict open_datagram(cont::ConstSpan<u8> bytes, u64 now_ns, DatagramCursor& cursor) noexcept;

    // Reads a header on `channel` and holds its length to the message and the channel. Costs nothing and spends
    // nothing. Short input on HeaderForm::Sized is NeedMore. A message a newer writer added comes back NewerMessage
    // with the length to step over, and a stream's padding PaddingFrame with the length that is all zero.
    [[nodiscard]] AdmitVerdict read_frame(u32 channel, HeaderForm form, cont::ConstSpan<u8> bytes,
                                          ParsedFrame& parsed) const noexcept;

    // S6 and S7 of a frame read_frame() understood, in order. `reassembled` reserves the frame's length from the
    // principal's reassembly quota first. On Admitted, `step.frame` is filled, body aside, and the frame holds a place
    // among the principal's pending messages until finish(). A refusal in S6 spends nothing and can be retried; a
    // refusal in S7 has been charged, and gives back what it held.
    [[nodiscard]] AdmitVerdict admit(u32 channel, const ParsedFrame& parsed, crypto::Delivery delivery, u64 now_ns,
                                     bool reassembled, FrameStep& step) noexcept;

    // Charges the bytes of a message stepped over as NewerMessage; false, spending nothing, when the principal's gate
    // is closed, and `retry_at_ns` says until when.
    [[nodiscard]] bool charge_stepped_over(const ParsedFrame& parsed, u64 now_ns, u64& retry_at_ns) noexcept;

    // Charges a stream's padding frame as bytes of its channel and its principal. Admitted, or the gate that is closed,
    // spending nothing, with `retry_at_ns` saying until when.
    [[nodiscard]] AdmitVerdict charge_padding(u32 channel, u32 bytes, u64 now_ns, u64& retry_at_ns) noexcept;

    // The admitted frame's whole body is here: decodes it when it is coded and fills step.frame.body when it is what
    // its writer hashed. When it is not, gives back what the frame holds and makes the step BadExpansion or
    // IntegrityFailed.
    void open_body(FrameStep& step, cont::ConstSpan<u8> body) noexcept;

    // Reads an admitted frame's delta body. What it rebuilds cannot be declared, so a closed byte gate holds it back
    // first (Throttled, with `retry_at_ns`, reading the same frame again then) and what it did expand to is charged
    // after, paid for by the wait that follows. The rest is DeltaReader::read().
    [[nodiscard]] DeltaRead read_delta(const AdmittedFrame& frame, DeltaReader& reader, u64 now_ns, u64& retry_at_ns,
                                       cont::ConstSpan<u8>& message, wire::WireError& error) noexcept;

    // Gives back what an admitted frame holds: its place among the pending messages and its buffer. The caller has
    // decoded and executed it, or dropped it.
    void finish(const AdmittedFrame& frame) noexcept;

    // Allocates a reassembled frame's buffer; null, for the caller to give back with finish(), when the allocator
    // refuses.
    [[nodiscard]] u8* allocate_buffer(u32 bytes) noexcept;

    // How many steps ended in `verdict`.
    [[nodiscard]] u64 count(AdmitVerdict verdict) const noexcept { return m_counts[static_cast<usize>(verdict)]; }

    void count_verdict(AdmitVerdict verdict) noexcept { ++m_counts[static_cast<usize>(verdict)]; }

    [[nodiscard]] PrincipalQuota& quota() noexcept { return m_quota; }

private:
    struct SchemaState
    {
        const wire::WireSchema* wire = nullptr;
        const crypto::AuthorizedSchema* authorized = nullptr;
        cont::ConstSpan<const BodyModel*> models;
        BodyCoder* coder = nullptr;
        u16 writer_minor = 0U;
        bool announced = false;
        u32 first_channel = 0U;
    };

    [[nodiscard]] GateRule gate_rule(u64 per_second) const noexcept;
    // The byte gates a frame of `operation` passes: the first that is closed, with `retry_at_ns`, or Admitted.
    [[nodiscard]] AdmitVerdict byte_gates(const ChannelState& state, u64 operation, u64 now_ns,
                                          u64& retry_at_ns) const noexcept;
    void charge_bytes(ChannelState& state, u64 operation, u32 bytes, u64 now_ns) noexcept;
    [[nodiscard]] AdmitVerdict take_padding(u32 channel, cont::ConstSpan<u8> bytes, u64 now_ns,
                                            DatagramCursor& cursor) noexcept;
    [[nodiscard]] AdmitVerdict authorize(const SchemaState& schema, const wire::ChannelDesc& channel,
                                         const wire::MessageDesc& message, u64 object,
                                         crypto::Delivery delivery) const noexcept;

    PrincipalQuota& m_quota;
    memory::IAllocator& m_allocator;
    cont::Span<ChannelState> m_channels;
    SchemaState m_schemas[kMaxSessionSchemas];
    u32 m_schema_count = 0U;
    u32 m_channel_count = 0U;
    const crypto::Capability* m_capability = nullptr;
    u64 m_counts[kAdmitVerdictCount] = {};
};

// Writes `message` at `minor` as one Sized or ToEnd frame: its header, hashed over its fields, then its fields. On any
// refusal `written` is zero. A message the schema does not list is OutOfRange, and so is a delta message, whose body
// DeltaWriter writes.
template <typename M>
[[nodiscard]] inline wire::WireError write_frame(const wire::WireSchema& schema, HeaderForm form, const M& message,
                                                 u16 minor, u64 object, cont::Span<u8> out, usize& written) noexcept
{
    written = 0U;
    const wire::MessageDesc* found = wire::find_message(schema, M::WireCodec::kMessageId);
    if (found == nullptr || found->delta != nullptr)
    {
        return wire::WireError::OutOfRange;
    }
    FrameHeader header;
    header.message = found->id;
    header.length = static_cast<u32>(wire::encoded_bytes(message, minor));
    header.object = object;
    const FrameShape shape = frame_shape(schema.channels[found->channel], *found);
    const usize used = write_frame_header(form, shape, header, out);
    if (used == 0U)
    {
        return wire::WireError::NoRoom;
    }
    usize body = 0U;
    const wire::WireError error = wire::encode(message, minor, out.subspan(used), body);
    if (error != wire::WireError::Ok)
    {
        return error;
    }
    if (shape.hash)
    {
        header.hash = frame_hash(header.message, object, cont::ConstSpan<u8>(out.data() + used, body));
        static_cast<void>(write_frame_header(form, shape, header, out)); // the same size: only the hash differs
    }
    written = used + body;
    return wire::WireError::Ok;
}

// Writes one Sized or ToEnd frame of `plain`, the encoding of a message on `channel`, coded when that pays, its hash
// taken over `plain`. A header's size depends on the coded body's, so `out` needs kMaxFrameHeaderBytes on top of the
// body.
[[nodiscard]] wire::WireError write_coded_body(const wire::ChannelDesc& channel, const wire::MessageDesc& message,
                                               const BodyCoding& coding, HeaderForm form, u64 object,
                                               cont::ConstSpan<u8> plain, cont::Span<u8> out, usize& written) noexcept;

// write_frame() on a channel that may code its bodies: `coding` is Admission::coding() of its channel, and empty just
// writes the frame.
template <typename M>
[[nodiscard]] inline wire::WireError write_frame(const wire::WireSchema& schema, const BodyCoding& coding,
                                                 HeaderForm form, const M& message, u16 minor, u64 object,
                                                 cont::Span<u8> out, usize& written) noexcept
{
    if (coding.model == nullptr)
    {
        return write_frame(schema, form, message, minor, object, out, written);
    }
    written = 0U;
    const wire::MessageDesc* found = wire::find_message(schema, M::WireCodec::kMessageId);
    if (found == nullptr || found->delta != nullptr)
    {
        return wire::WireError::OutOfRange;
    }
    usize plain = 0U;
    const cont::Span<u8> scratch = coding.coder->scratch();
    const wire::WireError error = wire::encode(message, minor, scratch, plain);
    if (error != wire::WireError::Ok)
    {
        return error;
    }
    return write_coded_body(schema.channels[found->channel], *found, coding, form, object, scratch.first(plain), out,
                            written);
}

// Decodes an admitted frame's body as message type M, written at the revision its writer announced. A frame of another
// message is OutOfRange: a dispatcher's mistake must not read bytes as the wrong layout. So is a delta message:
// DeltaReader reads its body, and the generated decoder then reads the whole message that gives.
template <typename M> [[nodiscard]] inline wire::WireError decode_frame(const AdmittedFrame& frame, M& message) noexcept
{
    if (frame.message == nullptr || frame.message->id != M::WireCodec::kMessageId || frame.message->delta != nullptr)
    {
        return wire::WireError::OutOfRange;
    }
    return wire::decode(frame.body, frame.writer_minor, message);
}

} // namespace crd::net
