// crd-fuzz-net-quic-codec: QUIC frames and transport parameters of crd-net (4.2a;
// docs/design/network-foundation.md#quic-frames) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: byte 0 picks the codec (bit 0: frames, else transport parameters), the sender (bit 1: a server) and, for
// frames, the packet type the placement table is asked about (bits 2 and 3); bytes 1 to 8 are GREASE entropy for the
// writer; the rest is a decrypted payload or a quic_transport_parameters value.
//
// Oracles. Frames: a refused frame consumes nothing and is FRAME_ENCODING_ERROR or PROTOCOL_VIOLATION; an accepted one
// consumes bytes, views only the payload, writes back in its shortest form no longer than it was read and reads back to
// the same frame; an ACK's ranges descend with a packet number between each and below its largest, and written again
// from them read back to the same ranges; a 0-RTT packet from a server carries nothing. Transport parameters: a
// payload whose (identifier, length, value) layout is broken is refused; a refusal leaves the output untouched; an
// accepted set writes back (its reserved versions left to the writer) with exactly one reserved parameter and reads
// back to the same values; the version selections only return versions their inputs name.
#include <crd/containers/wire_bytes.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_transport_parameters.hpp>
#include <crd/net/register_ids.hpp>

#include <cstring>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireReader;
using crd::crypto::QuicSide;
using namespace crd::net;

constexpr usize kHeaderBytes = 9U;
constexpr usize kMaxRanges   = crd::fuzz::kMaxInputBytes / 2U + 1U;

bool inside(ConstSpan<u8> view, ConstSpan<u8> whole)
{
    return view.empty() || (view.data() >= whole.data() && view.data() + view.size() <= whole.data() + whole.size());
}

bool same(ConstSpan<u8> a, ConstSpan<u8> b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

bool same_frame(const QuicFrame& a, const QuicFrame& b)
{
    return a.kind == b.kind && a.stream_id == b.stream_id && a.offset == b.offset && a.value == b.value &&
           a.error_code == b.error_code && a.frame_type == b.frame_type && a.sequence == b.sequence &&
           a.retire_prior_to == b.retire_prior_to && same(a.data, b.data) && same(a.reset_token, b.reset_token) &&
           a.fin == b.fin && a.to_end == b.to_end && a.bidirectional == b.bidirectional &&
           a.application == b.application;
}

void check_ack(const QuicAck& ack)
{
    static QuicAckRange ranges[kMaxRanges];
    QuicAckRanges walk(ack);
    usize count = 0U;
    QuicAckRange range;
    while (walk.next(range))
    {
        CRD_FUZZ_REQUIRE(count < kMaxRanges);
        CRD_FUZZ_REQUIRE(range.smallest <= range.largest && range.largest <= ack.largest);
        CRD_FUZZ_REQUIRE(count == 0U || range.largest + 2U <= ranges[count - 1U].smallest);
        ranges[count++] = range;
    }
    CRD_FUZZ_REQUIRE(count == ack.range_count + 1U);
    const EcnCounts counts{ack.ect0, ack.ect1, ack.ce};
    static u8 out[crd::fuzz::kMaxInputBytes + 64U];
    const ConstSpan<QuicAckRange> view(ranges, count);
    const usize bytes = write_quic_ack(view, ack.delay, ack.ecn ? &counts : nullptr, Span<u8>(out, sizeof(out)));
    CRD_FUZZ_REQUIRE(bytes != 0U);
    WireReader in(ConstSpan<u8>(out, bytes));
    QuicFrame again;
    CRD_FUZZ_REQUIRE(read_quic_frame(in, again) == QuicTransportError::NoError && in.done());
    CRD_FUZZ_REQUIRE(again.ack.largest == ack.largest && again.ack.delay == ack.delay &&
                     again.ack.range_count == ack.range_count && again.ack.ecn == ack.ecn);
    QuicAckRanges reread(again.ack);
    for (usize index = 0U; index < count; ++index)
    {
        CRD_FUZZ_REQUIRE(reread.next(range) && range.smallest == ranges[index].smallest &&
                         range.largest == ranges[index].largest);
    }
    CRD_FUZZ_REQUIRE(!reread.next(range));
}

void check_frames(ConstSpan<u8> payload, u8 control)
{
    constexpr QuicPacketType packets[4] = {QuicPacketType::Initial, QuicPacketType::Handshake, QuicPacketType::ZeroRtt,
                                           QuicPacketType::OneRtt};
    const QuicPacketType packet = packets[(control >> 2U) & 3U];
    const bool from_server = (control & 2U) != 0U;
    WireReader in(payload);
    while (!in.done())
    {
        const usize before = in.consumed();
        QuicFrame frame;
        const QuicTransportError error = read_quic_frame(in, frame);
        if (error != QuicTransportError::NoError)
        {
            CRD_FUZZ_REQUIRE(error == QuicTransportError::FrameEncodingError ||
                             error == QuicTransportError::ProtocolViolation);
            CRD_FUZZ_REQUIRE(in.consumed() == before);
            return;
        }
        const usize read_bytes = in.consumed() - before;
        CRD_FUZZ_REQUIRE(read_bytes != 0U);
        CRD_FUZZ_REQUIRE(inside(frame.data, payload) && inside(frame.reset_token, payload) &&
                         inside(frame.ack.ranges, payload));
        CRD_FUZZ_REQUIRE(!(packet == QuicPacketType::ZeroRtt && from_server &&
                           quic_frame_allowed(frame.kind, frame.application, packet, from_server)));
        if (frame.kind == QuicFrameKind::Ack)
        {
            check_ack(frame.ack);
            continue;
        }
        static u8 out[crd::fuzz::kMaxInputBytes + 64U];
        const usize bytes = write_quic_frame(frame, Span<u8>(out, sizeof(out)));
        CRD_FUZZ_REQUIRE(bytes != 0U && bytes == quic_frame_bytes(frame) && bytes <= read_bytes);
        WireReader again(ConstSpan<u8>(out, bytes));
        QuicFrame reread;
        CRD_FUZZ_REQUIRE(read_quic_frame(again, reread) == QuicTransportError::NoError);
        if (frame.kind != QuicFrameKind::Padding)
        {
            CRD_FUZZ_REQUIRE(again.done() && reread.kind == frame.kind);
            // Views differ in place, never in content.
            CRD_FUZZ_REQUIRE(same_frame(reread, frame));
        }
        else
        {
            CRD_FUZZ_REQUIRE(reread.data.size() == frame.data.size());
        }
    }
}

// Whether `bytes` is a sequence of (varint identifier, varint length, value) with nothing left over.
bool layout_intact(ConstSpan<u8> bytes)
{
    WireReader in(bytes);
    while (!in.done())
    {
        u64 id = 0U;
        u64 length = 0U;
        ConstSpan<u8> value;
        if (!take_quic_varint(in, id) || !take_quic_varint(in, length) || length > in.rest().size() ||
            !in.take_bytes(static_cast<usize>(length), value))
        {
            return false;
        }
    }
    return true;
}

usize reserved_parameters(ConstSpan<u8> bytes)
{
    WireReader in(bytes);
    usize count = 0U;
    while (!in.done())
    {
        u64 id = 0U;
        u64 length = 0U;
        ConstSpan<u8> value;
        CRD_FUZZ_REQUIRE(take_quic_varint(in, id) && take_quic_varint(in, length) &&
                         in.take_bytes(static_cast<usize>(length), value));
        count += (id % 31U == 27U) ? 1U : 0U;
    }
    return count;
}

void check_versions(const QuicTransportParameters& p)
{
    const u32 preference[2] = {iana::kQuicVersion2, iana::kQuicVersion1};
    const ConstSpan<u32> view(preference, 2U);
    const u32 original = p.version_information.chosen;
    const u32 selected = quic_server_select_version(p, original, view);
    CRD_FUZZ_REQUIRE(selected == 0U || selected == iana::kQuicVersion1 || selected == iana::kQuicVersion2);
    const u32 chosen = quic_select_version(view, p.version_information.available_view());
    CRD_FUZZ_REQUIRE(chosen == 0U || p.version_information.available_count != 0U);
    for (const bool after : {false, true})
    {
        for (const u32 attempted : {iana::kQuicVersion1, iana::kQuicVersion2})
        {
            const QuicTransportError error =
                quic_client_check_version_information(p, iana::kQuicVersion2, attempted, after, view);
            CRD_FUZZ_REQUIRE(error == QuicTransportError::NoError ||
                             error == QuicTransportError::VersionNegotiationError);
            // Never accepted when the server chose another version.
            CRD_FUZZ_REQUIRE(error != QuicTransportError::NoError || !p.has_version_information ||
                             p.version_information.chosen == iana::kQuicVersion2);
        }
    }
}

void check_parameters(ConstSpan<u8> bytes, u8 control, u64 entropy)
{
    const QuicSide side = (control & 2U) != 0U ? QuicSide::Server : QuicSide::Client;
    QuicTransportParameters read;
    read.max_idle_timeout = 0x5EED5EEDU; // a sentinel a refusal must leave
    const QuicTransportError error = read_quic_transport_parameters(bytes, side, read);
    if (error != QuicTransportError::NoError)
    {
        CRD_FUZZ_REQUIRE(error == QuicTransportError::TransportParameterError);
        CRD_FUZZ_REQUIRE(read.max_idle_timeout == 0x5EED5EEDU);
        return;
    }
    CRD_FUZZ_REQUIRE(layout_intact(bytes));
    CRD_FUZZ_REQUIRE(side == QuicSide::Server ||
                     !(read.has_original_destination_connection_id || read.has_stateless_reset_token ||
                       read.has_preferred_address || read.has_retry_source_connection_id));
    check_versions(read);

    // The writer adds its own reserved version: the peer's are left out, and a list left empty with them.
    QuicTransportParameters write = read;
    QuicVersionInformation& information = write.version_information;
    usize kept = 0U;
    for (usize index = 0U; index < information.available_count; ++index)
    {
        if (!quic_version_reserved(information.available[index]))
        {
            information.available[kept++] = information.available[index];
        }
    }
    information.available_count = kept;
    if (kept == 0U || kept >= kQuicMaxAvailableVersions || quic_version_reserved(information.chosen))
    {
        write.has_version_information = false;
        information = QuicVersionInformation{};
    }
    static u8 out[crd::fuzz::kMaxInputBytes + 512U];
    const usize written =
        write_quic_transport_parameters(write, side, QuicTransportParameterGrease{entropy}, Span<u8>(out, sizeof(out)));
    CRD_FUZZ_REQUIRE(written != 0U && reserved_parameters(ConstSpan<u8>(out, written)) == 1U);
    QuicTransportParameters again;
    CRD_FUZZ_REQUIRE(read_quic_transport_parameters(ConstSpan<u8>(out, written), side, again) ==
                     QuicTransportError::NoError);
    CRD_FUZZ_REQUIRE(again.max_idle_timeout == write.max_idle_timeout &&
                     again.max_udp_payload_size == write.max_udp_payload_size &&
                     again.initial_max_data == write.initial_max_data &&
                     again.initial_max_streams_bidi == write.initial_max_streams_bidi &&
                     again.initial_max_streams_uni == write.initial_max_streams_uni &&
                     again.ack_delay_exponent == write.ack_delay_exponent &&
                     again.max_ack_delay == write.max_ack_delay &&
                     again.active_connection_id_limit == write.active_connection_id_limit &&
                     again.max_datagram_frame_size == write.max_datagram_frame_size &&
                     again.grease_quic_bit == write.grease_quic_bit &&
                     again.disable_active_migration == write.disable_active_migration);
    CRD_FUZZ_REQUIRE(same(again.initial_source_connection_id.view(), write.initial_source_connection_id.view()) &&
                     again.has_preferred_address == write.has_preferred_address &&
                     same(again.preferred_address.connection_id.view(), write.preferred_address.connection_id.view()));
    CRD_FUZZ_REQUIRE(again.has_version_information == write.has_version_information &&
                     (!write.has_version_information ||
                      (again.version_information.chosen == information.chosen &&
                       again.version_information.available_count == information.available_count + 1U)));
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < kHeaderBytes || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    u64 entropy = 0U;
    for (usize index = 1U; index < kHeaderBytes; ++index)
    {
        entropy = (entropy << 8U) | data[index];
    }
    const ConstSpan<u8> body(data + kHeaderBytes, size - kHeaderBytes);
    if ((data[0] & 1U) != 0U)
    {
        check_frames(body, data[0]);
    }
    else
    {
        check_parameters(body, data[0], entropy);
    }
    return 0;
}

namespace
{
// A seed: the control byte, eight entropy bytes, then `body`.
void add_seed(crd::fuzz::SeedSink& sink, const char* name, u8 control, ConstSpan<u8> body)
{
    static u8 seed[kHeaderBytes + 512U];
    seed[0] = control;
    for (usize index = 1U; index < kHeaderBytes; ++index)
    {
        seed[index] = static_cast<u8>(0x31U * index);
    }
    std::memcpy(seed + kHeaderBytes, body.data(), body.size());
    sink.add(name, seed, kHeaderBytes + body.size());
}

// One frame of each written kind and an ACK with ranges and ECN counts, in one payload.
usize frames_payload(u8* out, usize capacity)
{
    static const u8 kData[5] = {1, 2, 3, 4, 5};
    static const u8 kId[8] = {9, 8, 7, 6, 5, 4, 3, 2};
    static const u8 kToken[16] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
                                  0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};
    usize at = 0U;
    const QuicAckRange ranges[3] = {{90U, 100U}, {40U, 70U}, {0U, 5U}};
    const EcnCounts counts{3U, 0U, 1U};
    at += write_quic_ack(ConstSpan<QuicAckRange>(ranges, 3U), 2500U, &counts, Span<u8>(out + at, capacity - at));
    const auto put = [&](QuicFrame frame) { at += write_quic_frame(frame, Span<u8>(out + at, capacity - at)); };
    QuicFrame frame;
    frame.kind = QuicFrameKind::Ping;
    put(frame);
    frame.kind = QuicFrameKind::Crypto;
    frame.offset = 300U;
    frame.data = ConstSpan<u8>(kData, 5U);
    put(frame);
    frame.kind = QuicFrameKind::Stream;
    frame.stream_id = 4U;
    frame.fin = true;
    put(frame);
    frame = QuicFrame{};
    frame.kind = QuicFrameKind::NewConnectionId;
    frame.sequence = 2U;
    frame.retire_prior_to = 1U;
    frame.data = ConstSpan<u8>(kId, 8U);
    frame.reset_token = ConstSpan<u8>(kToken, 16U);
    put(frame);
    frame = QuicFrame{};
    frame.kind = QuicFrameKind::MaxStreams;
    frame.bidirectional = true;
    frame.value = 100U;
    put(frame);
    frame = QuicFrame{};
    frame.kind = QuicFrameKind::ConnectionClose;
    frame.error_code = 0x0AU;
    frame.frame_type = 0x06U;
    frame.data = ConstSpan<u8>(kData, 3U);
    put(frame);
    frame = QuicFrame{};
    frame.kind = QuicFrameKind::Datagram;
    frame.data = ConstSpan<u8>(kData, 5U);
    frame.to_end = true;
    put(frame);
    return at;
}
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    static u8 body[512];
    const usize frames = frames_payload(body, sizeof(body));
    add_seed(sink, "frames-one-rtt", 0x0DU, ConstSpan<u8>(body, frames));
    add_seed(sink, "frames-zero-rtt-from-server", 0x0BU, ConstSpan<u8>(body, frames));
    const u8 initial[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x04, 0x16, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00};
    add_seed(sink, "frames-initial", 0x03U, ConstSpan<u8>(initial, sizeof(initial)));
    const u8 refused[] = {0x01, 0x40, 0x01};
    add_seed(sink, "frames-long-type", 0x0DU, ConstSpan<u8>(refused, sizeof(refused)));

    // RFC 9001 appendix A.2's client parameters.
    const u8 client[] = {0x04, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x05, 0x04, 0x80, 0x00, 0xFF,
                         0xFF, 0x07, 0x04, 0x80, 0x00, 0xFF, 0xFF, 0x08, 0x01, 0x10, 0x01, 0x04, 0x80, 0x00, 0x75,
                         0x30, 0x09, 0x01, 0x10, 0x0F, 0x08, 0x83, 0x94, 0xC8, 0xF0, 0x3E, 0x51, 0x57, 0x08, 0x06,
                         0x04, 0x80, 0x00, 0xFF, 0xFF};
    add_seed(sink, "parameters-rfc-9001-client", 0x00U, ConstSpan<u8>(client, sizeof(client)));

    QuicTransportParameters server;
    server.has_original_destination_connection_id = true;
    static_cast<void>(server.original_destination_connection_id.assign(ConstSpan<u8>(client + 36U, 8U)));
    server.has_stateless_reset_token = true;
    server.has_preferred_address = true;
    static_cast<void>(server.preferred_address.connection_id.assign(ConstSpan<u8>(client + 36U, 4U)));
    server.has_initial_source_connection_id = true;
    static_cast<void>(server.initial_source_connection_id.assign(ConstSpan<u8>(client + 36U, 8U)));
    server.has_retry_source_connection_id = true;
    static_cast<void>(server.retry_source_connection_id.assign(ConstSpan<u8>(client + 36U, 2U)));
    server.max_udp_payload_size = 1500U;
    server.ack_delay_exponent = 10U;
    server.max_ack_delay = 100U;
    server.active_connection_id_limit = 4U;
    server.initial_max_streams_bidi = 100U;
    server.max_datagram_frame_size = 1200U;
    server.grease_quic_bit = true;
    server.disable_active_migration = true;
    server.has_version_information = true;
    server.version_information.chosen = iana::kQuicVersion2;
    server.version_information.available[0] = iana::kQuicVersion2;
    server.version_information.available[1] = iana::kQuicVersion1;
    server.version_information.available_count = 2U;
    const usize written = write_quic_transport_parameters(server, QuicSide::Server, QuicTransportParameterGrease{77U},
                                                          Span<u8>(body, sizeof(body)));
    add_seed(sink, "parameters-full-server", 0x02U, ConstSpan<u8>(body, written));
    add_seed(sink, "parameters-server-only-from-client", 0x00U, ConstSpan<u8>(body, written));
}
