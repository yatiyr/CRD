// crd-net -- WebSockets over HTTP/2 and HTTP/3 (4.2a; docs/design/network-foundation.md#websocket-tunnels). Sources:
// RFC 8441, RFC 9220, RFC 6455 and RFC 8470.

#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/websocket_tunnel.hpp>

#include <cstring>
#include <new>

namespace crd::net
{
namespace
{
constexpr u32 kNoTunnel = ~u32{0};
constexpr u16 kStatusBad = 400U;
constexpr u16 kStatusTooEarly = 425U; // RFC 8470 section 5.2
constexpr usize kStatusDigits = 3U;
// The most an extended CONNECT carries: its subprotocols joined by ", ".
constexpr usize kProtocolListBytes = kWsMaxProtocols * (kWsMaxProtocolBytes + 2U);

template <usize N> [[nodiscard]] cont::ConstSpan<u8> text(const char (&literal)[N]) noexcept
{
    return cont::ConstSpan<u8>(reinterpret_cast<const u8*>(literal), N - 1U);
}

[[nodiscard]] HttpField field(cont::ConstSpan<u8> name, cont::ConstSpan<u8> value) noexcept
{
    HttpField out;
    out.name = name;
    out.value = value;
    return out;
}

void status_text(u16 status, u8 (&out)[kStatusDigits]) noexcept
{
    out[0] = static_cast<u8>('0' + (status / 100U) % 10U);
    out[1] = static_cast<u8>('0' + (status / 10U) % 10U);
    out[2] = static_cast<u8>('0' + status % 10U);
}

// What a refused extended CONNECT means for open().
[[nodiscard]] WsTunnelError opening_error(Http2Error error) noexcept
{
    switch (error)
    {
        case Http2Error::None:
            return WsTunnelError::None;
        case Http2Error::Blocked:
            return WsTunnelError::Blocked;
        case Http2Error::Closed:
        case Http2Error::GoingAway:
        case Http2Error::NoStream:
            return WsTunnelError::Refused;
        default:
            return WsTunnelError::Failed;
    }
}

[[nodiscard]] WsTunnelError opening_error(Http3Error error) noexcept
{
    switch (error)
    {
        case Http3Error::None:
            return WsTunnelError::None;
        case Http3Error::Blocked:
            return WsTunnelError::Blocked;
        case Http3Error::Closed:
        case Http3Error::GoingAway:
        case Http3Error::NoStream:
            return WsTunnelError::Refused;
        default:
            return WsTunnelError::Failed;
    }
}

[[nodiscard]] bool limits_valid(const WsTunnelConfig& config, bool client) noexcept
{
    const QmuxConfig& connection = config.connection;
    QuicStreamLimits streams = connection.streams;
    streams.in_order = true; // as QMux runs them
    return config.tunnels != 0U && config.tunnels <= kWsMaxTunnels && connection.tls == nullptr &&
           connection.tickets == nullptr && connection.tunnel == QmuxTunnel::None && connection.websocket != nullptr &&
           connection.early_context.empty() && ws_config_valid(*connection.websocket, client) &&
           QuicStreamSet::limits_valid(streams) && QuicDatagrams::limits_valid(connection.datagrams);
}

} // namespace

WebSocketTunnels::~WebSocketTunnels() noexcept
{
    if (m_slots == nullptr)
    {
        return;
    }
    for (u32 index = 0U; index < m_config.tunnels; ++index)
    {
        if (m_slots[index].connection != nullptr)
        {
            m_slots[index].connection->~QmuxConnection();
        }
    }
    memory::deallocate_array(*m_allocator, m_slots);
    memory::deallocate_array(*m_allocator, m_cells);
}

bool WebSocketTunnels::init(memory::IAllocator& allocator, Http2Connection& http2, const WsTunnelConfig& config) noexcept
{
    const bool client = http2.config().side == Http2Side::Client;
    if (m_slots != nullptr || (!client && !http2.config().connect_protocol) || !setup(allocator, config, client))
    {
        return false;
    }
    m_http2 = &http2;
    return true;
}

bool WebSocketTunnels::init(memory::IAllocator& allocator, Http3Connection& http3, const WsTunnelConfig& config) noexcept
{
    const bool client = http3.config().side == crypto::QuicSide::Client;
    if (m_slots != nullptr || (!client && !http3.config().connect_protocol) || !setup(allocator, config, client))
    {
        return false;
    }
    m_http3 = &http3;
    return true;
}

bool WebSocketTunnels::setup(memory::IAllocator& allocator, const WsTunnelConfig& config, bool client) noexcept
{
    if (!limits_valid(config, client))
    {
        return false;
    }
    Slot* const slots = memory::try_construct_array<Slot>(allocator, config.tunnels);
    QmuxConnection* const cells =
        (slots != nullptr) ? memory::try_allocate_array<QmuxConnection>(allocator, config.tunnels) : nullptr;
    if (cells == nullptr)
    {
        if (slots != nullptr)
        {
            memory::deallocate_array(allocator, slots);
        }
        return false;
    }
    m_allocator = &allocator;
    m_config = config;
    m_config.connection.tunnel = client ? QmuxTunnel::Client : QmuxTunnel::Server;
    m_slots = slots;
    m_cells = cells;
    return true;
}

// ---- Tunnels ----

WebSocketTunnels::Slot* WebSocketTunnels::find(u64 stream) noexcept
{
    for (u32 index = 0U; m_slots != nullptr && index < m_config.tunnels; ++index)
    {
        Slot& slot = m_slots[index];
        if (slot.connection != nullptr && slot.stream == stream && !slot.report_end)
        {
            return &slot;
        }
    }
    return nullptr;
}

u32 WebSocketTunnels::claim(u64 stream) noexcept
{
    for (u32 index = 0U; index < m_config.tunnels; ++index)
    {
        Slot& slot = m_slots[index];
        if (slot.connection != nullptr)
        {
            continue;
        }
        QmuxConnection* const connection = ::new (static_cast<void*>(m_cells + index)) QmuxConnection();
        if (!connection->init(*m_allocator, m_config.connection))
        {
            connection->~QmuxConnection();
            return kNoTunnel;
        }
        const u32 generation = slot.generation;
        slot = Slot{};
        slot.generation = generation;
        slot.connection = connection;
        slot.stream = stream;
        ++m_live;
        return index;
    }
    return kNoTunnel;
}

void WebSocketTunnels::forget(u32 index) noexcept
{
    Slot& slot = m_slots[index];
    if (slot.connection == nullptr)
    {
        return;
    }
    slot.connection->~QmuxConnection();
    const u32 generation = slot.generation + 1U;
    slot = Slot{};
    slot.generation = generation;
    --m_live;
}

QuicHandle WebSocketTunnels::handle_of(u32 index) const noexcept
{
    QuicHandle handle;
    handle.slot = index;
    handle.generation = m_slots[index].generation;
    return handle;
}

QmuxConnection* WebSocketTunnels::connection(QuicHandle handle) noexcept
{
    if (m_slots == nullptr || handle.slot >= m_config.tunnels)
    {
        return nullptr;
    }
    const Slot& slot = m_slots[handle.slot];
    return (slot.generation == handle.generation && !slot.opening) ? slot.connection : nullptr;
}

WsTunnelError WebSocketTunnels::open(u64 now_ns, QuicHandle& handle) noexcept
{
    handle = QuicHandle{};
    const bool client = m_config.connection.tunnel == QmuxTunnel::Client;
    if (m_slots == nullptr || !client)
    {
        return WsTunnelError::Failed;
    }
    if (bearer_failed())
    {
        return WsTunnelError::Refused;
    }
    const bool allowed = (m_http2 != nullptr)
                             ? m_http2->settings_known() && m_http2->peer_settings().enable_connect_protocol
                             : m_http3->settings_known() && m_http3->peer_settings().enable_connect_protocol;
    if (!allowed)
    {
        return WsTunnelError::NotReady;
    }
    if (m_live == m_config.tunnels)
    {
        return WsTunnelError::NoTunnel;
    }
    const u32 index = claim(0U);
    if (index == kNoTunnel)
    {
        return WsTunnelError::Failed;
    }
    const WebSocketConfig& websocket = *m_config.connection.websocket;
    u8 list[kProtocolListBytes];
    usize used = 0U;
    for (const cont::ConstSpan<u8> protocol : websocket.protocols)
    {
        if (used != 0U)
        {
            list[used++] = ',';
            list[used++] = ' ';
        }
        std::memcpy(list + used, protocol.data(), protocol.size());
        used += protocol.size();
    }
    HttpField fields[8] = {field(text(":method"), text("CONNECT")),
                           field(text(":protocol"), text("websocket")),
                           field(text(":scheme"), text("https")),
                           field(text(":authority"), websocket.host),
                           field(text(":path"), websocket.path),
                           field(text("sec-websocket-version"), text("13")),
                           field(text("sec-websocket-protocol"), cont::ConstSpan<u8>(list, used)),
                           field(text("origin"), websocket.origin)};
    const cont::ConstSpan<HttpField> request(fields, websocket.origin.empty() ? 7U : 8U);
    WsTunnelError error = WsTunnelError::None;
    u64 stream = 0U;
    if (m_http2 != nullptr)
    {
        u32 id = 0U;
        error = opening_error(m_http2->open_request(request, false, id));
        stream = id;
    }
    else
    {
        error = opening_error(m_http3->open_request(request, false, stream));
    }
    if (error != WsTunnelError::None)
    {
        forget(index);
        return error;
    }
    Slot& slot = m_slots[index];
    slot.stream = stream;
    slot.opening = true;
    const u64 timeout = m_config.connection.handshake_timeout_ns;
    slot.deadline_ns = (timeout != 0U) ? now_ns + timeout : kQmuxNoTimer;
    handle = handle_of(index);
    return WsTunnelError::None;
}

// ---- Events of the HTTP connection ----

bool WebSocketTunnels::on_event(const Http2Event& event, u64 now_ns) noexcept
{
    switch (event.kind)
    {
        case Http2EventKind::Headers:
            return on_headers(event.stream, event.fields, event.info, false, now_ns);
        case Http2EventKind::Interim:
            return find(event.stream) != nullptr;
        case Http2EventKind::Body:
        {
            Slot* const slot = find(event.stream);
            if (slot == nullptr)
            {
                return false;
            }
            deliver(*slot, event.payload, now_ns);
            static_cast<void>(m_http2->consume_body(static_cast<u32>(event.stream), event.payload.size()));
            return true;
        }
        case Http2EventKind::Finished:
            return ended(event.stream, true, 0U, now_ns);
        case Http2EventKind::Aborted:
            return ended(event.stream, false, event.error, now_ns);
        default:
            return false;
    }
}

bool WebSocketTunnels::on_event(const Http3Event& event, u64 now_ns) noexcept
{
    switch (event.kind)
    {
        case Http3EventKind::Headers:
            return on_headers(event.stream, event.fields, event.info, event.early, now_ns);
        case Http3EventKind::Interim:
        case Http3EventKind::Datagram: // RFC 9220 uses no HTTP datagrams
            return find(event.stream) != nullptr;
        case Http3EventKind::Body:
        {
            Slot* const slot = find(event.stream);
            if (slot == nullptr)
            {
                return false;
            }
            for (Http3Body body = m_http3->peek_body(event.stream); !body.data.empty();
                 body = m_http3->peek_body(event.stream))
            {
                deliver(*slot, body.data, now_ns);
                static_cast<void>(m_http3->consume_body(event.stream, body.data.size()));
            }
            return true;
        }
        case Http3EventKind::Finished:
            return ended(event.stream, true, 0U, now_ns);
        case Http3EventKind::Aborted:
            return ended(event.stream, false, event.error, now_ns);
        case Http3EventKind::Stopped:
            return stopped(event.stream, event.error);
        default:
            return false;
    }
}

bool WebSocketTunnels::on_headers(u64 stream, cont::ConstSpan<HttpField> fields, const HttpMessageInfo& info,
                                  bool early, u64 now_ns) noexcept
{
    if (m_config.connection.tunnel == QmuxTunnel::Server)
    {
        if (!ws_is_connect(info))
        {
            return false;
        }
        accept(stream, fields, info, early, now_ns);
        return true;
    }
    Slot* const slot = find(stream);
    if (slot == nullptr)
    {
        return false;
    }
    if (slot->opening)
    {
        answer(*slot, fields, info, now_ns);
    }
    return true;
}

void WebSocketTunnels::accept(u64 stream, cont::ConstSpan<HttpField> fields, const HttpMessageInfo& info, bool early,
                              u64 now_ns) noexcept
{
    WsHandshake handshake;
    const u16 status =
        early ? kStatusTooEarly : ws_check_connect(fields, info, *m_config.connection.websocket, handshake);
    if (status != kWsConnectAccepted)
    {
        u8 digits[kStatusDigits];
        status_text(status, digits);
        const HttpField refusal[2] = {field(text(":status"), cont::ConstSpan<u8>(digits, kStatusDigits)),
                                      field(text("sec-websocket-version"), text("13"))};
        const cont::ConstSpan<HttpField> response(refusal, (status == kStatusBad) ? 2U : 1U);
        // The answer ends the response, and what the request still sends is not wanted (RFC 9113 section 8.1).
        if (!respond(stream, response, true) || !cancel(stream, true))
        {
            refuse_stream(stream);
        }
        return;
    }
    const u32 index = (m_live < m_config.tunnels) ? claim(stream) : kNoTunnel;
    if (index == kNoTunnel)
    {
        refuse_stream(stream);
        return;
    }
    Slot& slot = m_slots[index];
    const HttpField accepted[2] = {
        field(text(":status"), text("200")),
        field(text("sec-websocket-protocol"), m_config.connection.websocket->protocols[handshake.protocol])};
    if (!slot.connection->start_tunnel(handshake.protocol, now_ns) ||
        !respond(stream, cont::ConstSpan<HttpField>(accepted, 2U), false))
    {
        forget(index);
        refuse_stream(stream);
        return;
    }
    slot.report_open = true;
}

void WebSocketTunnels::answer(Slot& slot, cont::ConstSpan<HttpField> fields, const HttpMessageInfo& info,
                              u64 now_ns) noexcept
{
    WsHandshake handshake;
    ws_read_connect_response(fields, info, *m_config.connection.websocket, handshake);
    slot.opening = false;
    slot.deadline_ns = kQmuxNoTimer;
    const bool accepted = handshake.status == kWsConnectAccepted;
    if (accepted && !slot.stopped && slot.connection->start_tunnel(handshake.protocol, now_ns))
    {
        slot.report_open = true;
        return;
    }
    slot.status = accepted ? 0U : handshake.status;
    static_cast<void>(cancel(slot.stream, false));
    slot.finished = true;
    slot.peer_ended = true;
    slot.refused = true;
    slot.report_end = true;
}

void WebSocketTunnels::deliver(Slot& slot, cont::ConstSpan<u8> bytes, u64 now_ns) noexcept
{
    if (slot.opening || bytes.empty())
    {
        return; // a body before a 2xx is no tunnel's, and is dropped with the stream
    }
    slot.connection->receive(bytes, now_ns);
    slot.report_service = true;
}

bool WebSocketTunnels::ended(u64 stream, bool clean, u64 error, u64 now_ns) noexcept
{
    Slot* const slot = find(stream);
    if (slot == nullptr)
    {
        return false;
    }
    slot->peer_ended = true;
    if (slot->opening)
    {
        slot->opening = false;
        slot->error = error;
        slot->finished = true;
        slot->refused = true;
        slot->report_end = true;
        return true;
    }
    slot->connection->transport_ended(clean, now_ns);
    if (!clean)
    {
        slot->finished = true; // the peer's reset ended both directions
    }
    return true;
}

bool WebSocketTunnels::stopped(u64 stream, u64 error) noexcept
{
    Slot* const slot = find(stream);
    if (slot == nullptr)
    {
        return false;
    }
    // This side's direction is gone; the peer's is read to its end (RFC 9000 section 3.5): an answer that still comes
    // complete (RFC 9114 section 4.1.1), or what a peer sent before it stopped reading, its Close among it. The
    // handshake timeout, the tunnel's own timers and the peer's end of the stream bound the wait.
    slot->stopped = true;
    slot->finished = true;
    slot->error = error;
    return true;
}

// ---- Serving ----

bool WebSocketTunnels::poll(WsTunnelEvent& event, u64 now_ns) noexcept
{
    if (m_slots == nullptr)
    {
        return false;
    }
    if (m_release != 0U)
    {
        forget(m_release - 1U);
        m_release = 0U;
    }
    if (m_walk == 0U)
    {
        const bool lost = bearer_failed();
        for (u32 index = 0U; index < m_config.tunnels; ++index)
        {
            Slot& slot = m_slots[index];
            if (slot.connection == nullptr || slot.report_end)
            {
                continue;
            }
            if (slot.opening)
            {
                if (lost || now_ns >= slot.deadline_ns)
                {
                    static_cast<void>(ended(slot.stream, false, 0U, now_ns));
                    static_cast<void>(cancel(slot.stream, false));
                }
                continue;
            }
            if (lost)
            {
                slot.connection->transport_ended(false, now_ns);
                slot.finished = true;
                slot.peer_ended = true;
            }
            else if (slot.connection->next_timer_ns() <= now_ns)
            {
                slot.connection->on_timeout(now_ns);
                slot.report_service = true;
            }
        }
        flush(now_ns);
    }
    for (; m_walk < m_config.tunnels; ++m_walk)
    {
        if (fill(m_slots[m_walk], event, m_walk))
        {
            return true;
        }
    }
    m_walk = 0U;
    return false;
}

bool WebSocketTunnels::fill(Slot& slot, WsTunnelEvent& event, u32 index) noexcept
{
    if (slot.connection == nullptr)
    {
        return false;
    }
    event = WsTunnelEvent{};
    event.tunnel = handle_of(index);
    event.stream = slot.stream;
    event.connection = slot.refused ? nullptr : slot.connection;
    if (slot.report_open)
    {
        slot.report_open = false;
        event.kind = WsTunnelEventKind::Opened;
        return true;
    }
    if (slot.report_service)
    {
        slot.report_service = false;
        event.kind = WsTunnelEventKind::Service;
        return true;
    }
    if (!slot.report_end)
    {
        return false;
    }
    event.kind = slot.refused ? WsTunnelEventKind::Refused : WsTunnelEventKind::Closed;
    event.status = slot.status;
    event.error = slot.error;
    m_release = index + 1U;
    ++m_walk;
    return true;
}

void WebSocketTunnels::flush(u64 now_ns) noexcept
{
    if (m_slots == nullptr || m_live == 0U)
    {
        return;
    }
    // Rounds of one view per tunnel, so one that has much to send cannot take every turn from the others.
    for (bool moved = true; moved;)
    {
        moved = false;
        for (u32 step = 0U; step < m_config.tunnels; ++step)
        {
            Slot& slot = m_slots[(m_first + step) % m_config.tunnels];
            if (slot.connection != nullptr && !slot.opening && !slot.report_end && pump(slot, now_ns))
            {
                moved = true;
            }
        }
    }
    m_first = (m_first + 1U) % m_config.tunnels;
    for (u32 index = 0U; index < m_config.tunnels; ++index)
    {
        Slot& slot = m_slots[index];
        if (slot.connection != nullptr && !slot.opening && !slot.report_end)
        {
            settle(slot);
        }
    }
}

bool WebSocketTunnels::pump(Slot& slot, u64 now_ns) noexcept
{
    if (slot.finished)
    {
        return false;
    }
    const cont::ConstSpan<u8> bytes = slot.connection->outgoing(now_ns);
    if (bytes.empty())
    {
        return false;
    }
    bool blocked = false;
    const usize sent = send(slot.stream, bytes, blocked);
    slot.connection->taken(sent, now_ns);
    return sent != 0U && !blocked;
}

void WebSocketTunnels::settle(Slot& slot) noexcept
{
    QmuxConnection& connection = *slot.connection;
    if (!slot.finished && connection.send_finished())
    {
        slot.finished = finish(slot.stream);
    }
    if (connection.state() != QmuxState::Closed)
    {
        return;
    }
    // A Closed tunnel's last bytes had their one try: a stream still open either way is cancelled, cleanly when this
    // side's end went.
    if ((!slot.finished || !slot.peer_ended) && !cancel(slot.stream, slot.finished))
    {
        return; // no room for the reset: the next flush tries again
    }
    slot.finished = true;
    slot.peer_ended = true;
    slot.report_end = true;
}

u64 WebSocketTunnels::next_deadline_ns() const noexcept
{
    u64 first = kQmuxNoTimer;
    for (u32 index = 0U; m_slots != nullptr && index < m_config.tunnels; ++index)
    {
        const Slot& slot = m_slots[index];
        if (slot.connection == nullptr || slot.report_end)
        {
            continue;
        }
        const u64 due = slot.opening ? slot.deadline_ns : slot.connection->next_timer_ns();
        first = (due < first) ? due : first;
    }
    return first;
}

// ---- The HTTP connection ----

bool WebSocketTunnels::bearer_failed() const noexcept
{
    return (m_http2 != nullptr) ? m_http2->error() != 0U : m_http3->error() != 0U;
}

bool WebSocketTunnels::respond(u64 stream, cont::ConstSpan<HttpField> fields, bool fin) noexcept
{
    if (m_http2 != nullptr)
    {
        return m_http2->send_response(static_cast<u32>(stream), fields, fin) == Http2Error::None;
    }
    return m_http3->send_response(stream, fields, fin) == Http3Error::None;
}

usize WebSocketTunnels::send(u64 stream, cont::ConstSpan<u8> bytes, bool& blocked) noexcept
{
    usize sent = 0U;
    while (sent < bytes.size())
    {
        usize taken = 0U;
        const bool done = (m_http2 != nullptr)
                              ? m_http2->send_body(static_cast<u32>(stream), bytes.subspan(sent), false, taken) ==
                                    Http2Error::None
                              : m_http3->send_body(stream, bytes.subspan(sent), false, taken) == Http3Error::None;
        sent += taken;
        if (!done || taken == 0U)
        {
            blocked = true;
            break;
        }
    }
    return sent;
}

bool WebSocketTunnels::finish(u64 stream) noexcept
{
    if (m_http2 != nullptr)
    {
        return m_http2->finish(static_cast<u32>(stream)) != Http2Error::Blocked;
    }
    return m_http3->finish(stream) != Http3Error::Blocked;
}

bool WebSocketTunnels::cancel(u64 stream, bool clean) noexcept
{
    if (m_http2 != nullptr)
    {
        return m_http2->cancel(static_cast<u32>(stream), clean ? kH2NoError : kH2Cancel) != Http2Error::Blocked;
    }
    return m_http3->cancel(stream, clean ? kH3NoError : kH3RequestCancelled) != Http3Error::Blocked;
}

void WebSocketTunnels::refuse_stream(u64 stream) noexcept
{
    if (m_http2 != nullptr)
    {
        static_cast<void>(m_http2->cancel(static_cast<u32>(stream), kH2RefusedStream));
        return;
    }
    static_cast<void>(m_http3->cancel(stream, kH3RequestRejected));
}

} // namespace crd::net
