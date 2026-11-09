#pragma once

// What the HTTP/3 tests share (4.2a; docs/design/network-foundation.md#http3): two QUIC stream sets exchanging frames
// directly, an Http3Connection on either or both, and the bytes a hostile peer writes.

#include "text_fixture.hpp"

#include <crd/containers/wire_bytes.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/http3.hpp>
#include <crd/net/quic_datagram.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_header.hpp>
#include <crd/net/quic_stream_set.hpp>

#include <cstring>
#include <initializer_list>
#include <memory>

namespace crd::net::testing
{
namespace cont = crd::containers;

inline constexpr usize kH3Client = 0U;
inline constexpr usize kH3Server = 1U;
inline constexpr usize kH3FrameRoom = 1200U;
inline constexpr u64 kH3Rtt = 20000000U;
inline constexpr usize kH3MaxEvents = 128U;

inline QuicStreamLimits h3_limits()
{
    QuicStreamLimits limits;
    limits.peer_bidi = 8U;
    limits.peer_uni = 8U;
    limits.local_bidi = 8U;
    limits.local_uni = 8U;
    limits.receive_bytes = 8192U;
    limits.send_bytes = 8192U;
    limits.stream_window = 8192U;
    limits.connection_window = 1U << 20U;
    limits.connection_max_window = 1U << 22U;
    return limits;
}

inline QuicDatagramLimits h3_datagram_limits()
{
    QuicDatagramLimits limits;
    limits.max_frame_bytes = 1200U;
    limits.receive_bytes = 4096U;
    limits.receive_count = 8U;
    limits.send_bytes = 4096U;
    limits.send_count = 8U;
    return limits;
}

inline Http3Config h3_config(crypto::QuicSide side)
{
    Http3Config config;
    config.side = side;
    config.field_bytes = 4096U;
    config.decoder.table_capacity = 1024U;
    config.decoder.blocked_streams = 4U;
    config.encoder.table_capacity = 1024U;
    config.connect_protocol = true;
    config.datagrams = true;
    config.grease = 0x123456789ABCDEFULL;
    return config;
}

// Field lines for a call: they view the text given.
struct H3Fields
{
    HttpField items[32];
    usize count = 0U;

    H3Fields() = default;
    H3Fields(std::initializer_list<std::pair<const char*, const char*>> lines)
    {
        for (const auto& line : lines)
        {
            add(line.first, line.second);
        }
    }

    void add(const char* name, const char* value)
    {
        items[count].name = cont::ConstSpan<u8>(reinterpret_cast<const u8*>(name), std::strlen(name));
        items[count].value = cont::ConstSpan<u8>(reinterpret_cast<const u8*>(value), std::strlen(value));
        ++count;
    }

    [[nodiscard]] cont::ConstSpan<HttpField> span() const { return cont::ConstSpan<HttpField>(items, count); }
};

inline const H3Fields& h3_get()
{
    static const H3Fields fields{
        {":method", "GET"}, {":scheme", "https"}, {":authority", "cerid.test"}, {":path", "/index"}};
    return fields;
}

inline const H3Fields& h3_ok()
{
    static const H3Fields fields{{":status", "200"}, {"server", "cerid"}};
    return fields;
}

// A copy of an event that lasts.
struct H3Seen
{
    Http3Event event;
    Text names[32];
    Text values[32];
    Text payload;
    usize field_count = 0U;

    [[nodiscard]] bool has(const char* name, const char* value) const
    {
        for (usize at = 0U; at < field_count; ++at)
        {
            if (names[at] == name && values[at] == value)
            {
                return true;
            }
        }
        return false;
    }
};

// Bytes as a peer writes them.
struct H3Wire
{
    u8 bytes[16384] = {};
    usize size = 0U;

    H3Wire& varint(u64 value)
    {
        cont::WireWriter out{cont::Span<u8>(bytes + size, sizeof(bytes) - size)};
        put_quic_varint(out, value);
        size += out.written();
        return *this;
    }
    H3Wire& raw(cont::ConstSpan<u8> data)
    {
        std::memcpy(bytes + size, data.data(), data.size());
        size += data.size();
        return *this;
    }
    H3Wire& raw(const H3Wire& other) { return raw(other.span()); }
    H3Wire& byte(u8 value)
    {
        bytes[size++] = value;
        return *this;
    }
    // A frame around `payload`.
    H3Wire& frame(u64 type, const H3Wire& payload)
    {
        varint(type);
        varint(payload.size);
        return raw(payload);
    }
    [[nodiscard]] cont::ConstSpan<u8> span() const { return cont::ConstSpan<u8>(bytes, size); }
};

// Which frames to hold back: a held stream's frames go back as lost and are sent again later.
class H3Rig
{
public:
    // `with_h3[side]`: whether that side runs an Http3Connection; the other is written by hand.
    H3Rig(memory::IAllocator& allocator_in, bool client_h3, bool server_h3,
          const Http3Config& client_config = h3_config(crypto::QuicSide::Client),
          const Http3Config& server_config = h3_config(crypto::QuicSide::Server),
          const QuicStreamLimits& limits = h3_limits(), bool collect = true)
        : allocator(allocator_in)
    {
        if (collect)
        {
            events[kH3Client] = std::make_unique<H3Seen[]>(kH3MaxEvents);
            events[kH3Server] = std::make_unique<H3Seen[]>(kH3MaxEvents);
        }
        ok = streams[kH3Client].init(allocator, limits, crypto::QuicSide::Client) &&
             streams[kH3Server].init(allocator, limits, crypto::QuicSide::Server) &&
             datagrams[kH3Client].init(allocator, h3_datagram_limits()) &&
             datagrams[kH3Server].init(allocator, h3_datagram_limits());
        if (!ok)
        {
            return;
        }
        QuicTransportParameters parameters[2];
        for (usize side = 0U; side < 2U; ++side)
        {
            streams[side].fill_parameters(parameters[side]);
            datagrams[side].fill_parameters(parameters[side]);
        }
        for (usize side = 0U; side < 2U; ++side)
        {
            streams[side].set_peer_parameters(parameters[1U - side]);
            datagrams[side].set_peer(parameters[1U - side].max_datagram_frame_size, kH3FrameRoom);
        }
        active[kH3Client] = client_h3;
        active[kH3Server] = server_h3;
        if (client_h3)
        {
            ok = ok && h3[kH3Client].init(allocator, streams[kH3Client], &datagrams[kH3Client], client_config) &&
                 h3[kH3Client].start();
        }
        if (server_h3)
        {
            ok = ok && h3[kH3Server].init(allocator, streams[kH3Server], &datagrams[kH3Server], server_config) &&
                 h3[kH3Server].start();
        }
    }

    memory::IAllocator& allocator;
    bool ok = false;
    QuicStreamSet streams[2];
    QuicDatagrams datagrams[2];
    Http3Connection h3[2];
    bool active[2] = {};
    u64 held_stream[2] = {~u64{0}, ~u64{0}}; // of frames each side sends
    u64 now = 1000000000U;
    usize max_frame_room = kH3FrameRoom;

    // One pass of every frame `from` has, delivered at once (a held stream's sent again later).
    bool deliver(usize from)
    {
        bool moved = false;
        QuicFrame frame;
        QuicStreamRecord record;
        u32 refused = 0U;
        while (refused < 64U && streams[from].next_frame(max_frame_room, now, kH3Rtt, frame, record))
        {
            const bool stream_frame = frame.kind == QuicFrameKind::Stream;
            if (stream_frame && frame.stream_id == held_stream[from])
            {
                streams[from].on_lost(record);
                ++refused;
                continue;
            }
            const QuicTransportError error = streams[1U - from].on_frame(frame);
            transport_error[1U - from] = (error != QuicTransportError::NoError) || transport_error[1U - from];
            streams[from].on_acked(record);
            moved = true;
        }
        u8 bytes[kH3FrameRoom];
        u64 id = 0U;
        for (usize written = datagrams[from].write_next(cont::Span<u8>(bytes, sizeof(bytes)), now, id); written != 0U;
             written = datagrams[from].write_next(cont::Span<u8>(bytes, sizeof(bytes)), now, id))
        {
            QuicFrame arrived;
            cont::WireReader in(cont::ConstSpan<u8>(bytes, written));
            if (read_quic_frame(in, arrived) == QuicTransportError::NoError)
            {
                static_cast<void>(datagrams[1U - from].on_frame(arrived));
            }
            moved = true;
        }
        return moved;
    }

    void poll(usize side)
    {
        if (!active[side])
        {
            return;
        }
        Http3Event event;
        while (h3[side].poll(event))
        {
            if (observe != nullptr)
            {
                observe(event);
            }
            if (events[side] != nullptr && count[side] < kH3MaxEvents)
            {
                H3Seen& seen = events[side][count[side]++];
                seen.event = event;
                seen.field_count = event.fields.size();
                for (usize at = 0U; at < event.fields.size() && at < 32U; ++at)
                {
                    seen.names[at] =
                        Text(reinterpret_cast<const char*>(event.fields[at].name.data()), event.fields[at].name.size());
                    seen.values[at] = Text(reinterpret_cast<const char*>(event.fields[at].value.data()),
                                           event.fields[at].value.size());
                }
                seen.payload = Text(reinterpret_cast<const char*>(event.payload.data()), event.payload.size());
            }
            if (event.kind == Http3EventKind::Body && drain_bodies)
            {
                drain_body(side, event.stream);
            }
        }
    }

    void drain_body(usize side, u64 stream)
    {
        for (Http3Body body = h3[side].peek_body(stream); !body.data.empty(); body = h3[side].peek_body(stream))
        {
            for (const u8 c : body.data)
            {
                received[side] += static_cast<char>(c);
            }
            if (h3[side].consume_body(stream, body.data.size()) != Http3Error::None)
            {
                break;
            }
        }
    }

    // Until nothing moves.
    void settle()
    {
        for (u32 round = 0U; round < 1024U; ++round)
        {
            poll(kH3Client);
            poll(kH3Server);
            const bool a = deliver(kH3Client);
            const bool b = deliver(kH3Server);
            if (!a && !b)
            {
                break;
            }
        }
        poll(kH3Client);
        poll(kH3Server);
    }

    [[nodiscard]] const H3Seen* find(usize side, Http3EventKind kind, usize nth = 0U) const
    {
        for (usize at = 0U; at < count[side]; ++at)
        {
            if (events[side][at].event.kind == kind)
            {
                if (nth == 0U)
                {
                    return &events[side][at];
                }
                --nth;
            }
        }
        return nullptr;
    }
    [[nodiscard]] usize how_many(usize side, Http3EventKind kind) const
    {
        usize total = 0U;
        for (usize at = 0U; at < count[side]; ++at)
        {
            total += (events[side][at].event.kind == kind) ? 1U : 0U;
        }
        return total;
    }

    // Writes bytes on a stream a hand-written peer holds.
    void write(usize side, u64 id, cont::ConstSpan<u8> bytes, bool fin = false)
    {
        usize taken = 0U;
        const QuicStreamError error = streams[side].write(id, bytes, fin, taken);
        write_ok = write_ok && error == QuicStreamError::None && taken == bytes.size();
    }
    void write(usize side, u64 id, const H3Wire& wire, bool fin = false) { write(side, id, wire.span(), fin); }

    // Opens a stream on a hand-written peer.
    [[nodiscard]] u64 open(usize side, bool bidirectional)
    {
        u64 id = 0U;
        write_ok = write_ok && streams[side].open(bidirectional, id) == QuicStreamError::None;
        return id;
    }

    // A hand-written peer's unidirectional stream of `type`.
    [[nodiscard]] u64 open_typed(usize side, u64 type)
    {
        const u64 id = open(side, false);
        H3Wire wire;
        wire.varint(type);
        write(side, id, wire);
        return id;
    }

    // A hand-written peer's control stream, its SETTINGS sent (the three QPACK-free defaults).
    [[nodiscard]] u64 open_control(usize side)
    {
        const u64 id = open_typed(side, 0U);
        H3Wire settings;
        H3Wire empty;
        settings.frame(4U, empty);
        write(side, id, settings);
        return id;
    }

    std::unique_ptr<H3Seen[]> events[2]; // null when the rig was built not to collect
    usize count[2] = {};
    Text received[2];
    bool drain_bodies = true;
    void (*observe)(const Http3Event&) = nullptr; // sees every event before it is copied
    bool transport_error[2] = {};
    bool write_ok = true;
};

} // namespace crd::net::testing
