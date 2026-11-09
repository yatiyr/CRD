// crd-net -- QUIC frames (4.2a; docs/design/network-foundation.md#quic-frames).

#include <crd/net/quic_frame.hpp>
#include <crd/net/register_ids.hpp>

namespace crd::net
{
namespace
{
constexpr u64 kTypePadding            = 0x00U;
constexpr u64 kTypePing               = 0x01U;
constexpr u64 kTypeAck                = 0x02U;
constexpr u64 kTypeAckEcn             = 0x03U;
constexpr u64 kTypeResetStream        = 0x04U;
constexpr u64 kTypeResetStreamAt      = iana::kQuicFrameResetStreamAt;
constexpr u64 kTypeStopSending        = 0x05U;
constexpr u64 kTypeCrypto             = 0x06U;
constexpr u64 kTypeNewToken           = 0x07U;
constexpr u64 kTypeStreamLast         = 0x0FU;
constexpr u64 kTypeMaxData            = 0x10U;
constexpr u64 kTypeMaxStreamData      = 0x11U;
constexpr u64 kTypeMaxStreamsBidi     = 0x12U;
constexpr u64 kTypeMaxStreamsUni      = 0x13U;
constexpr u64 kTypeDataBlocked        = 0x14U;
constexpr u64 kTypeStreamDataBlocked  = 0x15U;
constexpr u64 kTypeStreamsBlockedBidi = 0x16U;
constexpr u64 kTypeStreamsBlockedUni  = 0x17U;
constexpr u64 kTypeNewConnectionId    = 0x18U;
constexpr u64 kTypeRetireConnectionId = 0x19U;
constexpr u64 kTypePathChallenge      = 0x1AU;
constexpr u64 kTypePathResponse       = 0x1BU;
constexpr u64 kTypeCloseTransport     = 0x1CU;
constexpr u64 kTypeCloseApplication   = 0x1DU;
constexpr u64 kTypeHandshakeDone      = 0x1EU;
constexpr u64 kTypeDatagram           = 0x30U; // RFC 9221: data to the end of the packet
constexpr u64 kTypeDatagramLength     = 0x31U; // RFC 9221: with a Length field

// Takes a varint length field and that many bytes.
bool take_length_bytes(cont::WireReader& in, cont::ConstSpan<u8>& out) noexcept
{
    u64 length = 0U;
    if (!take_quic_varint(in, length) || length > in.rest().size())
    {
        return false;
    }
    return in.take_bytes(static_cast<usize>(length), out);
}

// Adds the varint bytes of `value` to `total`; false above kQuicMaxVarint.
bool add_varint(usize& total, u64 value) noexcept
{
    const usize bytes = quic_varint_bytes(value);
    total += bytes;
    return bytes != 0U;
}

// Whether `offset` + `length` stays within the largest stream or CRYPTO offset (sections 19.6 and 19.8).
bool end_fits(u64 offset, u64 length) noexcept
{
    return offset <= kQuicMaxVarint && length <= kQuicMaxVarint - offset;
}

QuicTransportError read_ack(cont::WireReader& in, bool ecn, QuicAck& ack) noexcept
{
    if (!take_quic_varint(in, ack.largest) || !take_quic_varint(in, ack.delay) ||
        !take_quic_varint(in, ack.range_count) || !take_quic_varint(in, ack.first_range))
    {
        return QuicTransportError::FrameEncodingError;
    }
    if (ack.first_range > ack.largest)
    {
        return QuicTransportError::FrameEncodingError; // section 19.3.1: a negative smallest
    }
    // Every further range takes at least two bytes, so a count past what remains is refused before any walk.
    if (ack.range_count > in.rest().size() / 2U)
    {
        return QuicTransportError::FrameEncodingError;
    }
    const usize ranges_start = in.consumed();
    const cont::ConstSpan<u8> rest = in.rest();
    u64 smallest = ack.largest - ack.first_range;
    for (u64 index = 0U; index < ack.range_count; ++index)
    {
        u64 gap = 0U;
        u64 length = 0U;
        if (!take_quic_varint(in, gap) || !take_quic_varint(in, length))
        {
            return QuicTransportError::FrameEncodingError;
        }
        // The next range's largest is smallest - gap - 2, and its smallest that minus length: both non-negative.
        if (smallest < 2U || gap > smallest - 2U)
        {
            return QuicTransportError::FrameEncodingError;
        }
        const u64 largest = smallest - gap - 2U;
        if (length > largest)
        {
            return QuicTransportError::FrameEncodingError;
        }
        smallest = largest - length;
    }
    ack.ranges = rest.first(in.consumed() - ranges_start);
    ack.ecn = ecn;
    if (ecn && (!take_quic_varint(in, ack.ect0) || !take_quic_varint(in, ack.ect1) || !take_quic_varint(in, ack.ce)))
    {
        return QuicTransportError::FrameEncodingError;
    }
    return QuicTransportError::NoError;
}

QuicTransportError read_stream(cont::WireReader& in, u64 type, QuicFrame& frame) noexcept
{
    const u8 bits = static_cast<u8>(type - kQuicStreamTypeBase);
    frame.kind = QuicFrameKind::Stream;
    frame.fin = (bits & kQuicStreamBitFin) != 0U;
    frame.to_end = (bits & kQuicStreamBitLength) == 0U;
    if (!take_quic_varint(in, frame.stream_id))
    {
        return QuicTransportError::FrameEncodingError;
    }
    if ((bits & kQuicStreamBitOffset) != 0U && !take_quic_varint(in, frame.offset))
    {
        return QuicTransportError::FrameEncodingError;
    }
    if (frame.to_end)
    {
        const bool taken = in.take_bytes(in.rest().size(), frame.data);
        static_cast<void>(taken); // the rest is always there
    }
    else if (!take_length_bytes(in, frame.data))
    {
        return QuicTransportError::FrameEncodingError;
    }
    return end_fits(frame.offset, frame.data.size()) ? QuicTransportError::NoError
                                                     : QuicTransportError::FrameEncodingError;
}

QuicTransportError read_new_connection_id(cont::WireReader& in, QuicFrame& frame) noexcept
{
    u8 length = 0U;
    if (!take_quic_varint(in, frame.sequence) || !take_quic_varint(in, frame.retire_prior_to) || !in.take_u8(length) ||
        length == 0U || length > crypto::kQuicMaxConnectionIdBytes || !in.take_bytes(length, frame.data) ||
        !in.take_bytes(kQuicResetTokenBytes, frame.reset_token))
    {
        return QuicTransportError::FrameEncodingError;
    }
    // Section 19.15: a Retire Prior To above the Sequence Number.
    return (frame.retire_prior_to <= frame.sequence) ? QuicTransportError::NoError
                                                     : QuicTransportError::FrameEncodingError;
}

QuicTransportError read_close(cont::WireReader& in, bool application, QuicFrame& frame) noexcept
{
    frame.kind = QuicFrameKind::ConnectionClose;
    frame.application = application;
    if (!take_quic_varint(in, frame.error_code) || (!application && !take_quic_varint(in, frame.frame_type)) ||
        !take_length_bytes(in, frame.data))
    {
        return QuicTransportError::FrameEncodingError;
    }
    return QuicTransportError::NoError;
}

// A frame of one varint field (`field`) and the kind it is.
QuicTransportError read_one(cont::WireReader& in, QuicFrameKind kind, u64& field, QuicFrame& frame) noexcept
{
    frame.kind = kind;
    return take_quic_varint(in, field) ? QuicTransportError::NoError : QuicTransportError::FrameEncodingError;
}

// A frame of a stream ID and one more varint field.
QuicTransportError read_two(cont::WireReader& in, QuicFrameKind kind, u64& second, QuicFrame& frame) noexcept
{
    frame.kind = kind;
    return (take_quic_varint(in, frame.stream_id) && take_quic_varint(in, second))
               ? QuicTransportError::NoError
               : QuicTransportError::FrameEncodingError;
}

QuicTransportError read_streams_limit(cont::WireReader& in, QuicFrameKind kind, bool bidirectional,
                                      QuicFrame& frame) noexcept
{
    frame.kind = kind;
    frame.bidirectional = bidirectional;
    if (!take_quic_varint(in, frame.value) || frame.value > kQuicMaxStreams)
    {
        return QuicTransportError::FrameEncodingError; // sections 19.11 and 19.14
    }
    return QuicTransportError::NoError;
}

QuicTransportError read_body(cont::WireReader& in, u64 type, QuicFrame& frame) noexcept
{
    if (type >= kQuicStreamTypeBase && type <= kTypeStreamLast)
    {
        return read_stream(in, type, frame);
    }
    switch (type)
    {
    case kTypePing:
        frame.kind = QuicFrameKind::Ping;
        return QuicTransportError::NoError;
    case kTypeAck:
    case kTypeAckEcn:
        frame.kind = QuicFrameKind::Ack;
        return read_ack(in, type == kTypeAckEcn, frame.ack);
    case kTypeResetStream:
        frame.kind = QuicFrameKind::ResetStream;
        return (take_quic_varint(in, frame.stream_id) && take_quic_varint(in, frame.error_code) &&
                take_quic_varint(in, frame.value))
                   ? QuicTransportError::NoError
                   : QuicTransportError::FrameEncodingError;
    case kTypeResetStreamAt:
        frame.kind = QuicFrameKind::ResetStreamAt;
        // The Reliable Size must not pass the Final Size (draft-ietf-quic-reliable-stream-reset section 4).
        return (take_quic_varint(in, frame.stream_id) && take_quic_varint(in, frame.error_code) &&
                take_quic_varint(in, frame.value) && take_quic_varint(in, frame.offset) && frame.offset <= frame.value)
                   ? QuicTransportError::NoError
                   : QuicTransportError::FrameEncodingError;
    case kTypeStopSending:
        return read_two(in, QuicFrameKind::StopSending, frame.error_code, frame);
    case kTypeCrypto:
        frame.kind = QuicFrameKind::Crypto;
        if (!take_quic_varint(in, frame.offset) || !take_length_bytes(in, frame.data))
        {
            return QuicTransportError::FrameEncodingError;
        }
        return end_fits(frame.offset, frame.data.size()) ? QuicTransportError::NoError
                                                         : QuicTransportError::FrameEncodingError;
    case kTypeNewToken:
        frame.kind = QuicFrameKind::NewToken;
        return (take_length_bytes(in, frame.data) && !frame.data.empty()) ? QuicTransportError::NoError
                                                                         : QuicTransportError::FrameEncodingError;
    case kTypeMaxData:
        return read_one(in, QuicFrameKind::MaxData, frame.value, frame);
    case kTypeMaxStreamData:
        return read_two(in, QuicFrameKind::MaxStreamData, frame.value, frame);
    case kTypeMaxStreamsBidi:
    case kTypeMaxStreamsUni:
        return read_streams_limit(in, QuicFrameKind::MaxStreams, type == kTypeMaxStreamsBidi, frame);
    case kTypeDataBlocked:
        return read_one(in, QuicFrameKind::DataBlocked, frame.value, frame);
    case kTypeStreamDataBlocked:
        return read_two(in, QuicFrameKind::StreamDataBlocked, frame.value, frame);
    case kTypeStreamsBlockedBidi:
    case kTypeStreamsBlockedUni:
        return read_streams_limit(in, QuicFrameKind::StreamsBlocked, type == kTypeStreamsBlockedBidi, frame);
    case kTypeNewConnectionId:
        frame.kind = QuicFrameKind::NewConnectionId;
        return read_new_connection_id(in, frame);
    case kTypeRetireConnectionId:
        return read_one(in, QuicFrameKind::RetireConnectionId, frame.sequence, frame);
    case kTypePathChallenge:
    case kTypePathResponse:
        frame.kind = (type == kTypePathChallenge) ? QuicFrameKind::PathChallenge : QuicFrameKind::PathResponse;
        return in.take_bytes(kQuicPathDataBytes, frame.data) ? QuicTransportError::NoError
                                                             : QuicTransportError::FrameEncodingError;
    case kTypeCloseTransport:
    case kTypeCloseApplication:
        return read_close(in, type == kTypeCloseApplication, frame);
    case kTypeHandshakeDone:
        frame.kind = QuicFrameKind::HandshakeDone;
        return QuicTransportError::NoError;
    case kTypeDatagram:
        frame.kind = QuicFrameKind::Datagram;
        frame.to_end = true;
        return in.take_bytes(in.rest().size(), frame.data) ? QuicTransportError::NoError
                                                          : QuicTransportError::FrameEncodingError;
    case kTypeDatagramLength:
        frame.kind = QuicFrameKind::Datagram;
        return take_length_bytes(in, frame.data) ? QuicTransportError::NoError
                                                 : QuicTransportError::FrameEncodingError;
    default:
        return QuicTransportError::FrameEncodingError; // section 12.4: an unknown frame type
    }
}

// The wire type `frame` is written with.
u64 wire_type(const QuicFrame& frame) noexcept
{
    switch (frame.kind)
    {
    case QuicFrameKind::Padding:
        return kTypePadding;
    case QuicFrameKind::Ping:
        return kTypePing;
    case QuicFrameKind::Ack:
        return kTypeAck;
    case QuicFrameKind::ResetStream:
        return kTypeResetStream;
    case QuicFrameKind::ResetStreamAt:
        return kTypeResetStreamAt;
    case QuicFrameKind::StopSending:
        return kTypeStopSending;
    case QuicFrameKind::Crypto:
        return kTypeCrypto;
    case QuicFrameKind::NewToken:
        return kTypeNewToken;
    case QuicFrameKind::Stream:
        return kQuicStreamTypeBase | (frame.offset != 0U ? kQuicStreamBitOffset : 0U) |
               (frame.to_end ? 0U : kQuicStreamBitLength) | (frame.fin ? kQuicStreamBitFin : 0U);
    case QuicFrameKind::MaxData:
        return kTypeMaxData;
    case QuicFrameKind::MaxStreamData:
        return kTypeMaxStreamData;
    case QuicFrameKind::MaxStreams:
        return frame.bidirectional ? kTypeMaxStreamsBidi : kTypeMaxStreamsUni;
    case QuicFrameKind::DataBlocked:
        return kTypeDataBlocked;
    case QuicFrameKind::StreamDataBlocked:
        return kTypeStreamDataBlocked;
    case QuicFrameKind::StreamsBlocked:
        return frame.bidirectional ? kTypeStreamsBlockedBidi : kTypeStreamsBlockedUni;
    case QuicFrameKind::NewConnectionId:
        return kTypeNewConnectionId;
    case QuicFrameKind::RetireConnectionId:
        return kTypeRetireConnectionId;
    case QuicFrameKind::PathChallenge:
        return kTypePathChallenge;
    case QuicFrameKind::PathResponse:
        return kTypePathResponse;
    case QuicFrameKind::ConnectionClose:
        return frame.application ? kTypeCloseApplication : kTypeCloseTransport;
    case QuicFrameKind::HandshakeDone:
        return kTypeHandshakeDone;
    case QuicFrameKind::Datagram:
        return frame.to_end ? kTypeDatagram : kTypeDatagramLength;
    }
    return kTypePadding;
}

// Adds the bytes of `frame`'s body after its type to `total`; false for a body write_quic_frame refuses.
bool add_body(const QuicFrame& frame, usize& total) noexcept
{
    const usize data = frame.data.size();
    switch (frame.kind)
    {
    case QuicFrameKind::Padding:
    case QuicFrameKind::Ack:
        return false; // PADDING is sized by quic_frame_bytes itself; ACK frames are write_quic_ack's
    case QuicFrameKind::Ping:
    case QuicFrameKind::HandshakeDone:
        return true;
    case QuicFrameKind::ResetStream:
        return add_varint(total, frame.stream_id) && add_varint(total, frame.error_code) &&
               add_varint(total, frame.value);
    case QuicFrameKind::ResetStreamAt:
        return frame.offset <= frame.value && add_varint(total, frame.stream_id) &&
               add_varint(total, frame.error_code) && add_varint(total, frame.value) && add_varint(total, frame.offset);
    case QuicFrameKind::StopSending:
        return add_varint(total, frame.stream_id) && add_varint(total, frame.error_code);
    case QuicFrameKind::Crypto:
        total += data;
        return end_fits(frame.offset, data) && add_varint(total, frame.offset) && add_varint(total, data);
    case QuicFrameKind::NewToken:
        total += data;
        return data != 0U && add_varint(total, data);
    case QuicFrameKind::Stream:
        total += data;
        return end_fits(frame.offset, data) && add_varint(total, frame.stream_id) &&
               (frame.offset == 0U || add_varint(total, frame.offset)) && (frame.to_end || add_varint(total, data));
    case QuicFrameKind::MaxData:
    case QuicFrameKind::DataBlocked:
        return add_varint(total, frame.value);
    case QuicFrameKind::MaxStreamData:
    case QuicFrameKind::StreamDataBlocked:
        return add_varint(total, frame.stream_id) && add_varint(total, frame.value);
    case QuicFrameKind::MaxStreams:
    case QuicFrameKind::StreamsBlocked:
        return frame.value <= kQuicMaxStreams && add_varint(total, frame.value);
    case QuicFrameKind::NewConnectionId:
        total += 1U + data + kQuicResetTokenBytes;
        return data != 0U && data <= crypto::kQuicMaxConnectionIdBytes &&
               frame.reset_token.size() == kQuicResetTokenBytes && frame.retire_prior_to <= frame.sequence &&
               add_varint(total, frame.sequence) && add_varint(total, frame.retire_prior_to);
    case QuicFrameKind::RetireConnectionId:
        return add_varint(total, frame.sequence);
    case QuicFrameKind::PathChallenge:
    case QuicFrameKind::PathResponse:
        total += data;
        return data == kQuicPathDataBytes;
    case QuicFrameKind::ConnectionClose:
        total += data;
        return add_varint(total, frame.error_code) && (frame.application || add_varint(total, frame.frame_type)) &&
               add_varint(total, data);
    case QuicFrameKind::Datagram:
        total += data;
        return frame.to_end || add_varint(total, data);
    }
    return false;
}

void write_body(const QuicFrame& frame, cont::WireWriter& out) noexcept
{
    switch (frame.kind)
    {
    case QuicFrameKind::Padding:
    case QuicFrameKind::Ack:
    case QuicFrameKind::Ping:
    case QuicFrameKind::HandshakeDone:
        break;
    case QuicFrameKind::ResetStream:
        put_quic_varint(out, frame.stream_id);
        put_quic_varint(out, frame.error_code);
        put_quic_varint(out, frame.value);
        break;
    case QuicFrameKind::ResetStreamAt:
        put_quic_varint(out, frame.stream_id);
        put_quic_varint(out, frame.error_code);
        put_quic_varint(out, frame.value);
        put_quic_varint(out, frame.offset);
        break;
    case QuicFrameKind::StopSending:
        put_quic_varint(out, frame.stream_id);
        put_quic_varint(out, frame.error_code);
        break;
    case QuicFrameKind::Crypto:
        put_quic_varint(out, frame.offset);
        put_quic_varint(out, frame.data.size());
        out.put_bytes(frame.data);
        break;
    case QuicFrameKind::NewToken:
        put_quic_varint(out, frame.data.size());
        out.put_bytes(frame.data);
        break;
    case QuicFrameKind::Stream:
        put_quic_varint(out, frame.stream_id);
        if (frame.offset != 0U)
        {
            put_quic_varint(out, frame.offset);
        }
        if (!frame.to_end)
        {
            put_quic_varint(out, frame.data.size());
        }
        out.put_bytes(frame.data);
        break;
    case QuicFrameKind::MaxData:
    case QuicFrameKind::DataBlocked:
    case QuicFrameKind::MaxStreams:
    case QuicFrameKind::StreamsBlocked:
        put_quic_varint(out, frame.value);
        break;
    case QuicFrameKind::MaxStreamData:
    case QuicFrameKind::StreamDataBlocked:
        put_quic_varint(out, frame.stream_id);
        put_quic_varint(out, frame.value);
        break;
    case QuicFrameKind::NewConnectionId:
        put_quic_varint(out, frame.sequence);
        put_quic_varint(out, frame.retire_prior_to);
        out.put_u8(static_cast<u8>(frame.data.size()));
        out.put_bytes(frame.data);
        out.put_bytes(frame.reset_token);
        break;
    case QuicFrameKind::RetireConnectionId:
        put_quic_varint(out, frame.sequence);
        break;
    case QuicFrameKind::PathChallenge:
    case QuicFrameKind::PathResponse:
        out.put_bytes(frame.data);
        break;
    case QuicFrameKind::ConnectionClose:
        put_quic_varint(out, frame.error_code);
        if (!frame.application)
        {
            put_quic_varint(out, frame.frame_type);
        }
        put_quic_varint(out, frame.data.size());
        out.put_bytes(frame.data);
        break;
    case QuicFrameKind::Datagram:
        if (!frame.to_end)
        {
            put_quic_varint(out, frame.data.size());
        }
        out.put_bytes(frame.data);
        break;
    }
}

// Whether `ranges` are in the order write_quic_ack takes: largest first, disjoint and not adjacent.
bool ack_ranges_valid(cont::ConstSpan<QuicAckRange> ranges) noexcept
{
    if (ranges.empty())
    {
        return false;
    }
    for (usize index = 0U; index < ranges.size(); ++index)
    {
        const QuicAckRange& range = ranges[index];
        if (range.smallest > range.largest || range.largest > kQuicMaxVarint)
        {
            return false;
        }
        if (index != 0U && (ranges[index - 1U].smallest < 2U || range.largest > ranges[index - 1U].smallest - 2U))
        {
            return false;
        }
    }
    return true;
}
} // namespace

bool quic_frame_ack_eliciting(QuicFrameKind kind) noexcept
{
    return kind != QuicFrameKind::Ack && kind != QuicFrameKind::Padding && kind != QuicFrameKind::ConnectionClose;
}

bool quic_frame_congestion_controlled(QuicFrameKind kind) noexcept
{
    return kind != QuicFrameKind::Ack;
}

bool quic_frame_probing(QuicFrameKind kind) noexcept
{
    return kind == QuicFrameKind::Padding || kind == QuicFrameKind::NewConnectionId ||
           kind == QuicFrameKind::PathChallenge || kind == QuicFrameKind::PathResponse;
}

bool quic_frame_allowed(QuicFrameKind kind, bool application_close, QuicPacketType packet, bool from_server) noexcept
{
    switch (packet)
    {
    case QuicPacketType::Initial:
    case QuicPacketType::Handshake:
        return kind == QuicFrameKind::Padding || kind == QuicFrameKind::Ping || kind == QuicFrameKind::Ack ||
               kind == QuicFrameKind::Crypto || (kind == QuicFrameKind::ConnectionClose && !application_close);
    case QuicPacketType::ZeroRtt:
        return !from_server && kind != QuicFrameKind::Ack && kind != QuicFrameKind::Crypto &&
               kind != QuicFrameKind::NewToken && kind != QuicFrameKind::PathResponse &&
               kind != QuicFrameKind::HandshakeDone;
    case QuicPacketType::OneRtt:
        return from_server || (kind != QuicFrameKind::NewToken && kind != QuicFrameKind::HandshakeDone);
    case QuicPacketType::Retry:
    case QuicPacketType::VersionNegotiation:
    case QuicPacketType::OtherVersion:
        return false;
    }
    return false;
}

QuicTransportError read_quic_frame(cont::WireReader& in, QuicFrame& out) noexcept
{
    cont::WireReader reader = in;
    const usize start = reader.consumed();
    u64 type = 0U;
    if (!take_quic_varint(reader, type))
    {
        return QuicTransportError::FrameEncodingError;
    }
    if (reader.consumed() - start != quic_varint_bytes(type))
    {
        return QuicTransportError::ProtocolViolation; // section 12.4: a type in a longer encoding than its shortest
    }
    QuicFrame frame;
    if (type == kTypePadding)
    {
        // A run of PADDING bytes is one frame: a padded packet is read in one step, not one per byte.
        const cont::ConstSpan<u8> rest = reader.rest();
        usize run = 0U;
        while (run < rest.size() && rest[run] == 0U)
        {
            ++run;
        }
        cont::ConstSpan<u8> zeros;
        const bool taken = reader.take_bytes(run, zeros);
        static_cast<void>(taken); // the run lies within the rest
        frame.kind = QuicFrameKind::Padding;
        frame.data = in.rest().first(1U + run);
    }
    else
    {
        const QuicTransportError error = read_body(reader, type, frame);
        if (error != QuicTransportError::NoError)
        {
            return error;
        }
    }
    in = reader;
    out = frame;
    return QuicTransportError::NoError;
}

QuicAckRanges::QuicAckRanges(const QuicAck& ack) noexcept
    : m_in(ack.ranges), m_largest(ack.largest), m_smallest(ack.largest - ack.first_range), m_left(ack.range_count)
{
}

bool QuicAckRanges::next(QuicAckRange& out) noexcept
{
    if (m_first)
    {
        m_first = false;
        out = QuicAckRange{m_smallest, m_largest};
        return true;
    }
    if (m_left == 0U)
    {
        return false;
    }
    u64 gap = 0U;
    u64 length = 0U;
    if (!take_quic_varint(m_in, gap) || !take_quic_varint(m_in, length))
    {
        m_left = 0U; // not reached for an ACK read_quic_frame accepted
        return false;
    }
    --m_left;
    const u64 largest = m_smallest - gap - 2U;
    m_smallest = largest - length;
    out = QuicAckRange{m_smallest, largest};
    return true;
}

usize quic_frame_bytes(const QuicFrame& frame) noexcept
{
    if (frame.kind == QuicFrameKind::Padding)
    {
        return frame.data.size();
    }
    usize total = quic_varint_bytes(wire_type(frame));
    return add_body(frame, total) ? total : 0U;
}

usize write_quic_frame(const QuicFrame& frame, cont::Span<u8> out) noexcept
{
    const usize bytes = quic_frame_bytes(frame);
    if (bytes == 0U || bytes > out.size())
    {
        return 0U;
    }
    if (frame.kind == QuicFrameKind::Padding)
    {
        for (usize index = 0U; index < bytes; ++index)
        {
            out[index] = 0U;
        }
        return bytes;
    }
    cont::WireWriter writer(out.first(bytes));
    put_quic_varint(writer, wire_type(frame));
    write_body(frame, writer);
    return bytes;
}

usize quic_ack_bytes(cont::ConstSpan<QuicAckRange> ranges, u64 delay, const QuicEcnCounts* ecn) noexcept
{
    if (!ack_ranges_valid(ranges))
    {
        return 0U;
    }
    usize total = 1U;
    if (!add_varint(total, ranges[0].largest) || !add_varint(total, delay) || !add_varint(total, ranges.size() - 1U) ||
        !add_varint(total, ranges[0].largest - ranges[0].smallest))
    {
        return 0U;
    }
    for (usize index = 1U; index < ranges.size(); ++index)
    {
        // Both values are below kQuicMaxVarint: ack_ranges_valid bounded every range.
        static_cast<void>(add_varint(total, ranges[index - 1U].smallest - ranges[index].largest - 2U));
        static_cast<void>(add_varint(total, ranges[index].largest - ranges[index].smallest));
    }
    if (ecn != nullptr &&
        (!add_varint(total, ecn->ect0) || !add_varint(total, ecn->ect1) || !add_varint(total, ecn->ce)))
    {
        return 0U;
    }
    return total;
}

usize write_quic_ack(cont::ConstSpan<QuicAckRange> ranges, u64 delay, const QuicEcnCounts* ecn,
                     cont::Span<u8> out) noexcept
{
    const usize bytes = quic_ack_bytes(ranges, delay, ecn);
    if (bytes == 0U || bytes > out.size())
    {
        return 0U;
    }
    cont::WireWriter writer(out.first(bytes));
    writer.put_u8(static_cast<u8>(ecn != nullptr ? kTypeAckEcn : kTypeAck));
    put_quic_varint(writer, ranges[0].largest);
    put_quic_varint(writer, delay);
    put_quic_varint(writer, ranges.size() - 1U);
    put_quic_varint(writer, ranges[0].largest - ranges[0].smallest);
    for (usize index = 1U; index < ranges.size(); ++index)
    {
        put_quic_varint(writer, ranges[index - 1U].smallest - ranges[index].largest - 2U);
        put_quic_varint(writer, ranges[index].largest - ranges[index].smallest);
    }
    if (ecn != nullptr)
    {
        put_quic_varint(writer, ecn->ect0);
        put_quic_varint(writer, ecn->ect1);
        put_quic_varint(writer, ecn->ce);
    }
    return bytes;
}

} // namespace crd::net
