// crd-net -- a TLS connection that agreed on h2: HTTP/2 carrying WebSocket tunnels (4.2a;
// docs/design/network-foundation.md#h2-bearer). Sources: RFC 9113, RFC 8441, RFC 8470 and RFC 7301.

#include <crd/memory/construct.hpp>
#include <crd/net/http2.hpp>
#include <crd/net/qmux.hpp>
#include <crd/net/websocket_tunnel.hpp>

#include <cstring>

namespace crd::net
{
struct QmuxHttp2
{
    Http2Connection http2;
    WebSocketTunnels tunnels;
    bool peer_goaway = false; // the peer's GOAWAY came: the connection ends with its last tunnel
};

namespace
{
constexpr char kH2[] = "h2";

template <usize N> [[nodiscard]] cont::ConstSpan<u8> text(const char (&literal)[N]) noexcept
{
    return cont::ConstSpan<u8>(reinterpret_cast<const u8*>(literal), N - 1U);
}
} // namespace

bool QmuxConnection::http2_config_valid(const QmuxConfig& config, bool client) noexcept
{
    Http2Config http2 = *config.http2;
    http2.side = client ? Http2Side::Client : Http2Side::Server;
    // The connection's input buffer holds one frame and its header: what a TLS record carries.
    return config.websocket != nullptr && config.http2_tunnels != 0U && config.http2_tunnels <= kWsMaxTunnels &&
           http2.frame_bytes == kHttp2MinFrameBytes && Http2Connection::config_valid(http2) &&
           (client ? !config.tls->early_data() : http2.connect_protocol);
}

bool QmuxConnection::takes_http2() const noexcept
{
    return m_config.http2 != nullptr && m_tls.application_protocol() == cont::StringView(kH2);
}

void QmuxConnection::release_http2() noexcept
{
    if (m_h2 != nullptr)
    {
        memory::destroy_array(*m_allocator, m_h2, 1U);
        m_h2 = nullptr;
    }
}

void QmuxConnection::begin_http2(u64 now_ns) noexcept
{
    Http2Config http2 = *m_config.http2;
    http2.side = is_client() ? Http2Side::Client : Http2Side::Server;
    WsTunnelConfig tunnels;
    tunnels.connection = m_config;
    tunnels.connection.tls = nullptr;
    tunnels.connection.tickets = nullptr;
    tunnels.connection.early_context = {};
    tunnels.connection.http2 = nullptr;
    tunnels.tunnels = m_config.http2_tunnels;
    m_h2 = memory::try_construct_array<QmuxHttp2>(*m_allocator, 1U);
    if (m_h2 == nullptr || !m_h2->http2.init(*m_allocator, http2, now_ns) ||
        !m_h2->tunnels.init(*m_allocator, m_h2->http2, tunnels))
    {
        release_http2();
        m_close.reason = QmuxCloseReason::Http2Failed;
        m_close.error_code = kH2InternalError;
        finish_sending();
        m_state = QmuxState::Closed;
        return;
    }
    m_framing = Framing::Http2;
    m_parameters_sent = true; // HTTP/2 carries no QX_TRANSPORT_PARAMETERS
    established(now_ns);
}

// ---- Reading ----

void QmuxConnection::parse_http2(u64 now_ns) noexcept
{
    Http2Connection& http2 = m_h2->http2;
    if (m_in_early != 0U)
    {
        http2.early_input(m_in_early);
        m_in_early = 0U;
    }
    usize at = 0U;
    while (m_state == QmuxState::Established)
    {
        usize taken = 0U;
        Http2Event event;
        const bool more = http2.poll(cont::ConstSpan<u8>(m_in + at, m_in_fill - at), now_ns, taken, event);
        at += taken;
        if (!more)
        {
            break;
        }
        on_http2_event(event, now_ns); // a Body payload is read here, before the next poll() ends its view
    }
    if (at != 0U)
    {
        std::memmove(m_in, m_in + at, m_in_fill - at);
        m_in_fill -= at;
        m_last_activity = now_ns;
    }
    if (m_state == QmuxState::Established && http2.error() != 0U)
    {
        close_http2(QmuxCloseReason::Http2Failed, http2.error()); // its GOAWAY is written
    }
}

void QmuxConnection::on_http2_event(const Http2Event& event, u64 now_ns) noexcept
{
    if (m_h2->tunnels.on_event(event, now_ns))
    {
        return;
    }
    Http2Connection& http2 = m_h2->http2;
    switch (event.kind)
    {
        case Http2EventKind::Headers:
        {
            if (is_client())
            {
                break; // a client opens tunnels only, whose answers are theirs
            }
            // A request that is no WebSocket's: this connection serves nothing else. The answer ends the response, and
            // what the request still sends is not wanted (RFC 9113 section 8.1).
            HttpField not_found;
            not_found.name = text(":status");
            not_found.value = text("404");
            if (http2.send_response(event.stream, cont::ConstSpan<HttpField>(&not_found, 1U), true) != Http2Error::None ||
                http2.cancel(event.stream, kH2NoError) == Http2Error::Blocked)
            {
                static_cast<void>(http2.cancel(event.stream, kH2RefusedStream));
            }
            break;
        }
        case Http2EventKind::Body:
            static_cast<void>(http2.consume_body(event.stream, event.payload.size()));
            break;
        case Http2EventKind::GoAway:
            m_h2->peer_goaway = true;
            if (event.error != kH2NoError)
            {
                close_http2(QmuxCloseReason::Peer, event.error);
            }
            break;
        case Http2EventKind::Writable:
            m_h2->tunnels.flush(now_ns);
            break;
        default:
            break;
    }
}

// ---- Writing ----

bool QmuxConnection::http2_pending() const noexcept
{
    return m_h2 != nullptr && !m_send_shut && !m_h2->http2.output().empty();
}

crypto::TlsError QmuxConnection::send_http2() noexcept
{
    if (m_h2 == nullptr)
    {
        return crypto::TlsError::None;
    }
    Http2Connection& http2 = m_h2->http2;
    for (cont::ConstSpan<u8> out = http2.output(); !out.empty(); out = http2.output())
    {
        const usize piece = (out.size() < kHttp2MinFrameBytes) ? out.size() : kHttp2MinFrameBytes; // a TLS record
        const crypto::TlsError error = send_record(out.first(piece));
        if (error == crypto::TlsError::Full)
        {
            return crypto::TlsError::None; // the transport is behind: the bytes wait in HTTP/2's output
        }
        if (error != crypto::TlsError::None)
        {
            return error;
        }
        http2.output_taken(piece);
    }
    return crypto::TlsError::None;
}

bool QmuxConnection::flush_http2() noexcept
{
    const crypto::TlsError error = send_http2();
    if (error != crypto::TlsError::None)
    {
        tls_failed(error);
        return false;
    }
    return true;
}

void QmuxConnection::produce_http2(u64 now_ns) noexcept
{
    // Once close_notify is out (or the peer's came first) what still waits in HTTP/2 is dropped: TLS takes no more.
    if (m_h2 == nullptr || m_send_shut || !flush_http2())
    {
        return;
    }
    if (m_state == QmuxState::Established)
    {
        if (m_in_fill != 0U)
        {
            drain(now_ns); // reading may have waited for room to answer, and TLS for room in the input
        }
        if (m_state == QmuxState::Established)
        {
            m_h2->tunnels.flush(now_ns);
        }
        if (m_state == QmuxState::Established && m_h2->peer_goaway && m_h2->tunnels.live() == 0U)
        {
            close_http2(QmuxCloseReason::PeerShutdown, kH2NoError);
        }
        if (!flush_http2())
        {
            return;
        }
    }
    if (m_close_owed && m_h2->http2.output().empty())
    {
        finish_sending(); // close_notify behind the GOAWAY
        m_close_deadline = now_ns + m_config.close_wait_ns;
    }
}

// ---- Ending ----

void QmuxConnection::wind_down_http2() noexcept
{
    m_h2->tunnels.bearer_ended();
    static_cast<void>(m_h2->http2.goaway(true)); // a connection error wrote its own; with no room the close goes alone
}

void QmuxConnection::close_http2(QmuxCloseReason reason, u64 error_code) noexcept
{
    if (m_state != QmuxState::Established)
    {
        return;
    }
    m_close.reason = reason;
    m_close.application = reason == QmuxCloseReason::Local;
    m_close.error_code = error_code;
    m_close.frame_type = 0U;
    wind_down_http2();
    m_close_owed = true;
    m_close_deadline = kQmuxNoTimer; // set when close_notify is written
    m_state = QmuxState::Closing;
}

u64 QmuxConnection::http2_deadline() const noexcept
{
    if (m_h2 == nullptr)
    {
        return kQmuxNoTimer;
    }
    const u64 settings = m_h2->http2.next_deadline_ns();
    const u64 tunnels = m_h2->tunnels.next_deadline_ns();
    return (settings < tunnels) ? settings : tunnels;
}

// ---- Tunnels ----

bool QmuxConnection::poll_tunnel(WsTunnelEvent& event, u64 now_ns) noexcept
{
    if (m_h2 == nullptr)
    {
        return false;
    }
    if (m_state != QmuxState::Established)
    {
        m_h2->tunnels.bearer_ended();
    }
    return m_h2->tunnels.poll(event, now_ns);
}

WsTunnelError QmuxConnection::open_tunnel(u64 now_ns, QuicHandle& handle) noexcept
{
    handle = QuicHandle{};
    if (m_h2 == nullptr || m_state != QmuxState::Established)
    {
        return WsTunnelError::Refused;
    }
    return m_h2->tunnels.open(now_ns, handle);
}

bool QmuxConnection::tag_tunnel(QuicHandle handle, u32 tag) noexcept
{
    return m_h2 != nullptr && m_h2->tunnels.tag(handle, tag);
}

bool QmuxConnection::wake_tunnel(QuicHandle handle) noexcept
{
    return m_h2 != nullptr && m_h2->tunnels.wake(handle);
}

u32 QmuxConnection::tunnels_live() const noexcept
{
    return (m_h2 != nullptr) ? m_h2->tunnels.live() : 0U;
}

} // namespace crd::net
