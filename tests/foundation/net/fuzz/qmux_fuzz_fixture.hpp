#pragma once

// The QMux fuzz targets' shared parts: an input cursor and the oracle for what a QMux side sends.

#include <crd/containers/wire_bytes.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/qmux.hpp>
#include <crd/net/quic_header.hpp>
#include <crd/net/register_ids.hpp>

#include <cstring>

namespace crd::net::testing
{

struct FuzzInput
{
    const u8* data;
    usize size;
    usize at = 0U;

    [[nodiscard]] bool more() const { return at < size; }
    u8 byte() { return (at < size) ? data[at++] : u8{0}; }
    containers::ConstSpan<u8> run()
    {
        const usize wanted = (usize{byte()} << 8U) | byte();
        const usize length = (wanted < size - at) ? wanted : size - at;
        const containers::ConstSpan<u8> bytes(data + at, length);
        at += length;
        return bytes;
    }
};

// Checks what a QMux side sends: whole records of allowed frames, the transport parameters first and nothing after a
// CONNECTION_CLOSE.
class QmuxRecordOracle
{
public:
    void plaintext(containers::ConstSpan<u8> bytes)
    {
        for (const u8 byte : bytes)
        {
            CRD_FUZZ_REQUIRE(m_fill < sizeof(m_buffer));
            m_buffer[m_fill++] = byte;
        }
        usize at = 0U;
        while (at < m_fill)
        {
            containers::WireReader header(containers::ConstSpan<u8>(m_buffer + at, m_fill - at));
            u64 size = 0U;
            if (!take_quic_varint(header, size))
            {
                break;
            }
            CRD_FUZZ_REQUIRE(size != 0U && size <= kQmuxMaxRecord);
            const usize head = header.consumed();
            if (m_fill - at - head < size)
            {
                break;
            }
            record(containers::ConstSpan<u8>(m_buffer + at + head, static_cast<usize>(size)));
            at += head + static_cast<usize>(size);
        }
        if (at != 0U)
        {
            std::memmove(m_buffer, m_buffer + at, m_fill - at);
            m_fill -= at;
        }
    }

private:
    void record(containers::ConstSpan<u8> frames)
    {
        CRD_FUZZ_REQUIRE(!m_closed);
        containers::WireReader in(frames);
        while (!in.done())
        {
            containers::WireReader probe(in.rest());
            u64 type = 0U;
            CRD_FUZZ_REQUIRE(take_quic_varint(probe, type));
            if (!m_first_seen)
            {
                CRD_FUZZ_REQUIRE(type == iana::kQuicFrameQxTransportParameters);
                m_first_seen = true;
            }
            if (type == iana::kQuicFrameQxTransportParameters || type == iana::kQuicFrameQxPingRequest ||
                type == iana::kQuicFrameQxPingResponse)
            {
                u64 value = 0U;
                CRD_FUZZ_REQUIRE(take_quic_varint(probe, value));
                containers::ConstSpan<u8> skipped;
                const bool parameters = type == iana::kQuicFrameQxTransportParameters;
                const usize extra = parameters ? static_cast<usize>(value) : 0U;
                CRD_FUZZ_REQUIRE(in.take_bytes(probe.consumed() + extra, skipped));
                continue;
            }
            QuicFrame frame;
            CRD_FUZZ_REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
            switch (frame.kind)
            {
                case QuicFrameKind::Padding:
                case QuicFrameKind::Stream:
                case QuicFrameKind::ResetStream:
                case QuicFrameKind::StopSending:
                case QuicFrameKind::MaxData:
                case QuicFrameKind::MaxStreamData:
                case QuicFrameKind::MaxStreams:
                case QuicFrameKind::DataBlocked:
                case QuicFrameKind::StreamDataBlocked:
                case QuicFrameKind::StreamsBlocked:
                case QuicFrameKind::Datagram:
                    break;
                case QuicFrameKind::ConnectionClose:
                    m_closed = true;
                    break;
                default:
                    CRD_FUZZ_REQUIRE(false);
            }
        }
    }

    u8 m_buffer[2U * kQmuxMaxRecord + 16U] = {};
    usize m_fill = 0U;
    bool m_first_seen = false;
    bool m_closed = false;
};

} // namespace crd::net::testing
