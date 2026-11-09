// crd-net -- WebTransport sessions over an HTTP/3 connection (4.2a; docs/design/network-foundation.md#webtransport).

#include <crd/containers/wire_bytes.hpp>
#include <crd/core/utf8.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp>
#include <crd/net/http_message.hpp>
#include <crd/net/quic_header.hpp>
#include <crd/net/webtransport.hpp>

#include <cstring>

namespace crd::net
{
namespace
{
// Sections 9.6 and, for the two only named as prohibited, draft-ietf-webtrans-http2.
constexpr u64 kCapsuleClose = 0x2843U;
constexpr u64 kCapsuleDrain = 0x78AEU;
constexpr u64 kCapsuleMaxData = 0x190B4D3DU;
constexpr u64 kCapsuleMaxStreamData = 0x190B4D3EU;
constexpr u64 kCapsuleMaxStreamsBidi = 0x190B4D3FU;
constexpr u64 kCapsuleMaxStreamsUni = 0x190B4D40U;
constexpr u64 kCapsuleDataBlocked = 0x190B4D41U;
constexpr u64 kCapsuleStreamDataBlocked = 0x190B4D42U;
constexpr u64 kCapsuleStreamsBlockedBidi = 0x190B4D43U;
constexpr u64 kCapsuleStreamsBlockedUni = 0x190B4D44U;

constexpr usize kVarintBytes = 8U;
constexpr usize kCapsuleHeadBytes = 16U;
constexpr usize kCloseValueBytes = 4U + kWtMaxCloseMessage;
constexpr usize kDataFrameHeadBytes = 9U;
constexpr usize kExtraResponseFields = 30U;
constexpr usize kMaxProtocolBytes = 512U;

constexpr u8 kOweClose = 1U << 0U;
constexpr u8 kOweFin = 1U << 1U;
constexpr u8 kOweMaxData = 1U << 2U;
constexpr u8 kOweMaxUni = 1U << 3U;
constexpr u8 kOweMaxBidi = 1U << 4U;
constexpr u8 kOweDataBlocked = 1U << 5U;
constexpr u8 kOweUniBlocked = 1U << 6U;
constexpr u8 kOweBidiBlocked = 1U << 7U;

constexpr usize kBadList = ~usize{0};

template <usize N> [[nodiscard]] cont::ConstSpan<u8> text_of(const char (&text)[N]) noexcept
{
    return cont::ConstSpan<u8>(reinterpret_cast<const u8*>(text), N - 1U);
}

[[nodiscard]] bool same(cont::ConstSpan<u8> a, cont::ConstSpan<u8> b) noexcept
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

[[nodiscard]] u64 min_of(u64 a, u64 b) noexcept
{
    return (a < b) ? a : b;
}

void owe(u8& owed, u8 bit) noexcept
{
    owed = static_cast<u8>(owed | bit);
}

[[nodiscard]] bool any_limit(const Http3WebTransportSettings& settings) noexcept
{
    return settings.max_streams_uni != 0U || settings.max_streams_bidi != 0U || settings.max_data != 0U;
}

// Raises a limit by what was used, once half a window has come free (section 5.6: never waiting for a BLOCKED capsule).
[[nodiscard]] bool grant(u64 window, u64 used, u64 ceiling, u64& limit) noexcept
{
    const u64 wanted = min_of(used + window, ceiling);
    const u64 step = (window > 1U) ? window / 2U : 1U;
    if (wanted <= limit || wanted - limit < step)
    {
        return false;
    }
    limit = wanted;
    return true;
}

[[nodiscard]] bool is_space(u8 c) noexcept
{
    return c == ' ' || c == '\t';
}

[[nodiscard]] bool is_lower(u8 c) noexcept
{
    return c >= 'a' && c <= 'z';
}

[[nodiscard]] bool is_digit(u8 c) noexcept
{
    return c >= '0' && c <= '9';
}

[[nodiscard]] bool is_alpha(u8 c) noexcept
{
    return is_lower(c) || (c >= 'A' && c <= 'Z');
}

// RFC 9110's tchar, which with ':' and '/' is what continues an RFC 9651 Token.
[[nodiscard]] bool is_token_char(u8 c) noexcept
{
    return is_alpha(c) || is_digit(c) || (c != 0U && std::strchr("!#$%&'*+-.^_`|~:/", c) != nullptr);
}

void skip_spaces(cont::ConstSpan<u8> in, usize& at) noexcept
{
    while (at < in.size() && is_space(in[at]))
    {
        ++at;
    }
}

// RFC 9651 section 4.2.5. `bytes` is the whole string's length however small `out` is.
[[nodiscard]] bool take_string(cont::ConstSpan<u8> in, usize& at, cont::Span<u8> out, usize& bytes) noexcept
{
    bytes = 0U;
    const auto keep = [&](u8 c)
    {
        if (bytes < out.size())
        {
            out[bytes] = c;
        }
        ++bytes;
    };
    if (at >= in.size() || in[at] != '"')
    {
        return false;
    }
    ++at;
    while (at < in.size())
    {
        const u8 c = in[at++];
        if (c == '"')
        {
            return true;
        }
        if (c == '\\')
        {
            if (at == in.size() || (in[at] != '"' && in[at] != '\\'))
            {
                return false;
            }
            keep(in[at++]);
        }
        else if (c < 0x20U || c >= 0x7FU)
        {
            return false;
        }
        else
        {
            keep(c);
        }
    }
    return false;
}

// A parameter's value: its meaning is ignored (section 3.3), its syntax decides whether the field counts at all.
[[nodiscard]] bool skip_bare_item(cont::ConstSpan<u8> in, usize& at) noexcept
{
    if (at >= in.size())
    {
        return false;
    }
    const u8 first = in[at];
    usize ignored = 0U;
    if (first == '"')
    {
        return take_string(in, at, {}, ignored);
    }
    if (first == '?')
    {
        at += 2U;
        return at <= in.size() && (in[at - 1U] == '0' || in[at - 1U] == '1');
    }
    if (first == '%')
    {
        ++at;
        return take_string(in, at, {}, ignored);
    }
    if (first == ':')
    {
        for (++at; at < in.size() && in[at] != ':'; ++at)
        {
            if (!is_alpha(in[at]) && !is_digit(in[at]) && in[at] != '+' && in[at] != '/' && in[at] != '=')
            {
                return false;
            }
        }
        ++at;
        return at <= in.size();
    }
    if (first == '-' || first == '@' || is_digit(first))
    {
        ++at;
        const usize digits = at;
        while (at < in.size() && (is_digit(in[at]) || in[at] == '.'))
        {
            ++at;
        }
        return is_digit(first) || at != digits;
    }
    if (is_alpha(first) || first == '*')
    {
        while (at < in.size() && is_token_char(in[at]))
        {
            ++at;
        }
        return true;
    }
    return false;
}

[[nodiscard]] bool skip_parameters(cont::ConstSpan<u8> in, usize& at) noexcept
{
    while (at < in.size() && in[at] == ';')
    {
        ++at;
        while (at < in.size() && in[at] == ' ')
        {
            ++at;
        }
        if (at >= in.size() || (!is_lower(in[at]) && in[at] != '*'))
        {
            return false;
        }
        while (at < in.size() && (is_lower(in[at]) || is_digit(in[at]) || in[at] == '_' || in[at] == '-' ||
                                  in[at] == '.' || in[at] == '*'))
        {
            ++at;
        }
        if (at < in.size() && in[at] == '=')
        {
            ++at;
            if (!skip_bare_item(in, at))
            {
                return false;
            }
        }
    }
    return true;
}

// Whether `value` is an Item that is a String (section 3.3); it is written to `out` when true.
[[nodiscard]] bool take_item_string(cont::ConstSpan<u8> value, cont::Span<u8> out, usize& bytes) noexcept
{
    usize at = 0U;
    skip_spaces(value, at);
    if (!take_string(value, at, out, bytes) || !skip_parameters(value, at))
    {
        return false;
    }
    skip_spaces(value, at);
    return at == value.size();
}

[[nodiscard]] bool list_valid(cont::ConstSpan<u8> list) noexcept
{
    usize at = 0U;
    usize bytes = 0U;
    while (wt_next_protocol(list, at, {}, bytes))
    {
    }
    return at != kBadList;
}

[[nodiscard]] bool utf8_valid(cont::ConstSpan<u8> text) noexcept
{
    for (usize at = 0U; at < text.size();)
    {
        const usize length = utf8_sequence_length(text.data() + at, text.size() - at);
        if (length == 0U)
        {
            return false;
        }
        at += length;
    }
    return true;
}

} // namespace

bool wt_next_protocol(cont::ConstSpan<u8> list, usize& at, cont::Span<u8> out, usize& bytes) noexcept
{
    if (at == kBadList)
    {
        return false;
    }
    skip_spaces(list, at);
    if (at >= list.size())
    {
        at = list.size();
        return false;
    }
    if (!take_string(list, at, out, bytes) || !skip_parameters(list, at))
    {
        at = kBadList;
        return false;
    }
    skip_spaces(list, at);
    if (at < list.size())
    {
        if (list[at] != ',')
        {
            at = kBadList;
            return false;
        }
        ++at;
        skip_spaces(list, at);
        if (at >= list.size())
        {
            at = kBadList; // a trailing comma
            return false;
        }
    }
    return true;
}

usize wt_exporter_context(u64 session, cont::ConstSpan<u8> label, cont::ConstSpan<u8> context,
                          cont::Span<u8> out) noexcept
{
    const usize need = sizeof(u64) + 1U + label.size() + 1U + context.size();
    if (label.size() > kWtMaxExporterInput || context.size() > kWtMaxExporterInput || need > out.size())
    {
        return 0U;
    }
    cont::WireWriter writer{out};
    writer.put_u64(session);
    writer.put_u8(static_cast<u8>(label.size()));
    for (const u8 c : label)
    {
        writer.put_u8(c);
    }
    writer.put_u8(static_cast<u8>(context.size()));
    for (const u8 c : context)
    {
        writer.put_u8(c);
    }
    return writer.written();
}

WebTransport::~WebTransport() noexcept
{
    if (m_allocator == nullptr)
    {
        return;
    }
    memory::destroy_array(*m_allocator, m_sessions, usize{m_config.sessions});
    memory::destroy_array(*m_allocator, m_records, usize{m_record_count});
    memory::destroy_array(*m_allocator, m_held, usize{m_config.waiting_datagrams});
    memory::destroy_array(*m_allocator, m_held_bytes, usize{m_config.waiting_datagrams} * m_config.datagram_bytes);
    memory::destroy_array(*m_allocator, m_text, usize{m_config.sessions} * m_text_each);
}

bool WebTransport::init(memory::IAllocator& allocator, Http3Connection& http3, QuicStreamSet& streams,
                        const WebTransportConfig& config) noexcept
{
    if (m_allocator != nullptr || config.sessions == 0U || config.sessions > kWtMaxSessions ||
        config.waiting_streams > kQuicMaxStreamSlots || config.waiting_datagrams > kQuicMaxStreamSlots ||
        config.datagram_bytes == 0U || config.datagram_bytes > 65535U || !http3.config().webtransport.enabled)
    {
        return false;
    }
    m_text_each = usize{config.datagram_bytes} + kCapsuleHeadBytes;
    if (m_text_each < kCapsuleHeadBytes + kCloseValueBytes)
    {
        m_text_each = kCapsuleHeadBytes + kCloseValueBytes;
    }
    m_config = config;
    m_record_count = streams.slots();
    m_sessions = memory::try_construct_array<Session>(allocator, usize{config.sessions});
    m_records = memory::try_construct_array<Stream>(allocator, usize{m_record_count});
    m_held = memory::try_construct_array<HeldDatagram>(allocator, usize{config.waiting_datagrams});
    m_held_bytes =
        memory::try_construct_array<u8>(allocator, usize{config.waiting_datagrams} * usize{config.datagram_bytes});
    m_text = memory::try_construct_array<u8>(allocator, usize{config.sessions} * m_text_each);
    m_allocator = &allocator;
    if (m_sessions == nullptr || m_records == nullptr || m_text == nullptr ||
        (config.waiting_datagrams != 0U && (m_held == nullptr || m_held_bytes == nullptr)))
    {
        memory::destroy_array(allocator, m_sessions, usize{config.sessions});
        memory::destroy_array(allocator, m_records, usize{m_record_count});
        memory::destroy_array(allocator, m_held, usize{config.waiting_datagrams});
        memory::destroy_array(allocator, m_held_bytes, usize{config.waiting_datagrams} * config.datagram_bytes);
        memory::destroy_array(allocator, m_text, usize{config.sessions} * m_text_each);
        m_sessions = nullptr;
        m_records = nullptr;
        m_held = nullptr;
        m_held_bytes = nullptr;
        m_text = nullptr;
        m_allocator = nullptr;
        return false;
    }
    m_http3 = &http3;
    m_streams = &streams;
    m_offer = http3.config().webtransport;
    m_side = http3.config().side;
    for (u32 at = 0U; at < config.sessions; ++at)
    {
        m_sessions[at].text = m_text + (usize{at} * m_text_each);
    }
    return true;
}

u32 WebTransport::sessions_alive() const noexcept
{
    u32 alive = 0U;
    for (u32 at = 0U; at < m_config.sessions; ++at)
    {
        alive += (m_sessions[at].state != State::Free) ? 1U : 0U;
    }
    return alive;
}

u32 WebTransport::session_slot(u64 id) const noexcept
{
    for (u32 at = 0U; at < m_config.sessions; ++at)
    {
        if (m_sessions[at].state != State::Free && m_sessions[at].id == id)
        {
            return at;
        }
    }
    return kNone;
}

const WebTransport::Session* WebTransport::find_session(u64 id) const noexcept
{
    const u32 slot = session_slot(id);
    return (slot != kNone) ? &m_sessions[slot] : nullptr;
}

WebTransport::Session* WebTransport::find_session(u64 id) noexcept
{
    const u32 slot = session_slot(id);
    return (slot != kNone) ? &m_sessions[slot] : nullptr;
}

u32 WebTransport::stream_slot(u64 id) const noexcept
{
    const u32 slot = m_streams->slot_of(id);
    const bool ours = slot != kQuicNoStreamSlot && m_records[slot].used && m_records[slot].id == id &&
                      m_records[slot].session != kNone;
    return ours ? slot : kNone;
}

const WebTransport::Stream* WebTransport::find_stream(u64 id) const noexcept
{
    const u32 slot = stream_slot(id);
    return (slot != kNone) ? &m_records[slot] : nullptr;
}

WebTransport::Stream* WebTransport::find_stream(u64 id) noexcept
{
    const u32 slot = stream_slot(id);
    return (slot != kNone) ? &m_records[slot] : nullptr;
}

WebTransport::Stream* WebTransport::event_stream(const Http3Event& in) noexcept
{
    Stream& record = m_records[in.slot];
    return (record.used && record.id == in.stream) ? &record : nullptr;
}

u32 WebTransport::session_index(const Session& session) const noexcept
{
    return static_cast<u32>(&session - m_sessions);
}

bool WebTransport::admits(bool legacy) const noexcept
{
    const Http3Settings& peer = m_http3->peer_settings();
    if (legacy)
    {
        const u64 allowed = (m_side == crypto::QuicSide::Server) ? m_offer.legacy_sessions : peer.wt.legacy_sessions;
        return sessions_alive() < allowed;
    }
    return (any_limit(m_offer) && any_limit(peer.wt)) || sessions_alive() == 0U; // section 5.1
}

bool WebTransport::session_unanswered(const Session& session) noexcept
{
    return session.state == State::Requested || session.state == State::Connecting;
}

WebTransport::Session* WebTransport::claim_session(u64 id, bool legacy) noexcept
{
    for (u32 at = 0U; at < m_config.sessions; ++at)
    {
        Session& session = m_sessions[at];
        if (session.state != State::Free)
        {
            continue;
        }
        const Http3WebTransportSettings& peer = m_http3->peer_settings().wt;
        session.id = id;
        session.state = State::Requested;
        session.legacy = legacy;
        session.flow = !legacy && any_limit(m_offer) && any_limit(peer);
        if (session.flow)
        {
            session.peer_data = peer.max_data;
            session.peer_streams[0] = peer.max_streams_uni;
            session.peer_streams[1] = peer.max_streams_bidi;
            session.local_data = m_offer.max_data;
            session.local_streams[0] = m_offer.max_streams_uni;
            session.local_streams[1] = m_offer.max_streams_bidi;
        }
        return &session;
    }
    return nullptr;
}

void WebTransport::free_session(Session& session) noexcept
{
    u8* const text = session.text;
    session = Session{};
    session.text = text;
}

// ---- Events ----

bool WebTransport::poll(WtEvent& event) noexcept
{
    if (m_reported != kNone)
    {
        m_held[m_reported].used = false;
        m_reported = kNone;
    }
    for (;;)
    {
        for (u32 at = 0U; at < m_config.sessions; ++at)
        {
            flush(m_sessions[at]);
        }
        if (service(event))
        {
            return true;
        }
        Http3Event in;
        if (!m_http3->poll(in))
        {
            return false;
        }
        if (on_http3(in, event))
        {
            return true;
        }
    }
}

bool WebTransport::service(WtEvent& event) noexcept
{
    for (u32 at = 0U; at < m_config.sessions; ++at)
    {
        Session& session = m_sessions[at];
        if ((session.state == State::Open || session.state == State::Ending) && session.pump &&
            read_capsules(session, event))
        {
            return true;
        }
        if (session.state != State::Open)
        {
            continue;
        }
        if (session.announce && announce(session, event))
        {
            return true;
        }
        if (session.credit_arrived)
        {
            session.credit_arrived = false;
            session.credit_wanted = false;
            event = WtEvent{};
            event.kind = WtEventKind::SessionWritable;
            event.session = session.id;
            return true;
        }
    }
    return false;
}

bool WebTransport::on_http3(const Http3Event& in, WtEvent& event) noexcept
{
    switch (in.kind)
    {
        case Http3EventKind::WtStream:
            return on_stream_opened(in, event);
        case Http3EventKind::WtReadable:
            return on_stream_readable(in, event);
        case Http3EventKind::WtAborted:
            return on_stream_aborted(in, event);
        case Http3EventKind::WtStopped:
            return on_stream_stopped(in, event);
        case Http3EventKind::WtWritable:
        {
            const Stream* const record = event_stream(in);
            if (record == nullptr || record->waiting || record->send_over)
            {
                return false;
            }
            event = WtEvent{};
            event.kind = WtEventKind::StreamWritable;
            event.session = record->session_id;
            event.stream = record->id;
            return true;
        }
        case Http3EventKind::Datagram:
            return on_datagram(in, event);
        case Http3EventKind::Headers:
        case Http3EventKind::Interim:
        case Http3EventKind::Body:
        case Http3EventKind::Trailers:
        case Http3EventKind::Finished:
        case Http3EventKind::Aborted:
        case Http3EventKind::Stopped:
        case Http3EventKind::Writable:
        {
            Session* const session = find_session(in.stream);
            if (session != nullptr)
            {
                return on_session_event(*session, in, event);
            }
            if (in.kind == Http3EventKind::Headers && m_side == crypto::QuicSide::Server)
            {
                return on_request(in, event);
            }
            break;
        }
        case Http3EventKind::Settings:
        case Http3EventKind::GoAway:
            break;
    }
    event = WtEvent{};
    event.kind = WtEventKind::Http;
    event.http = in;
    return true;
}

bool WebTransport::on_session_event(Session& session, const Http3Event& in, WtEvent& event) noexcept
{
    switch (in.kind)
    {
        case Http3EventKind::Headers:
            return (session.state == State::Connecting) && on_response(session, in, event);
        case Http3EventKind::Body:
            session.pump = true;
            return false;
        case Http3EventKind::Writable:
            flush(session);
            return false;
        case Http3EventKind::Finished:
            if (session.state == State::Ending)
            {
                session.peer_ended = true;
                settle_ending(session);
                return false;
            }
            if (session.state != State::Open)
            {
                return false;
            }
            if (!session.reader.at_boundary())
            {
                return end_session(session, kH3MessageError, kH3MessageError, event); // RFC 9297 section 3.3
            }
            event = WtEvent{}; // section 6: a clean end is code 0 and no message
            event.kind = WtEventKind::SessionClosed;
            event.session = session.id;
            event.has_code = true;
            event.legacy = session.legacy;
            release_waiting(session.id);
            end_streams(session);
            drop_held(session.id);
            session.state = State::Ending;
            session.peer_ended = true;
            session.owed = kOweFin;
            flush(session);
            return true;
        case Http3EventKind::Aborted:
        case Http3EventKind::Stopped:
            if (session.state == State::Ending)
            {
                session.owed = 0U; // no FIN can go, or none is wanted
                session.peer_ended = session.peer_ended || in.kind == Http3EventKind::Aborted;
                settle_ending(session);
                return false;
            }
            if (session.state == State::Closing)
            {
                finish_close(session);
                return false;
            }
            if (in.kind == Http3EventKind::Stopped &&
                (session.state == State::Connecting || (session.state == State::Open && in.error == kWtSessionGone)))
            {
                return false; // a complete response may still be read; a peer's close travels with its capsule and FIN
            }
            return end_session(session, in.error, kWtSessionGone, event);
        default:
            return false;
    }
}

bool WebTransport::on_request(const Http3Event& in, WtEvent& event) noexcept
{
    bool legacy = false;
    if (!in.info.extended || !http3_wt_token(in.info.protocol, legacy))
    {
        release_waiting(in.stream);
        drop_held(in.stream); // an extended CONNECT of another kind loses what came before it, as a datagram may
        event = WtEvent{};
        event.kind = WtEventKind::Http;
        event.http = in;
        return true;
    }
    Session* const session =
        (same(in.info.scheme, text_of("https")) && origin_allowed(in.fields) && admits(legacy))
            ? claim_session(in.stream, legacy)
            : nullptr;
    if (session == nullptr)
    {
        if (!same(in.info.scheme, text_of("https")))
        {
            static_cast<void>(m_http3->cancel(in.stream, kH3MessageError));
        }
        else if (origin_allowed(in.fields) || refuse(in.stream, 403U) != Http3Error::None)
        {
            static_cast<void>(m_http3->cancel(in.stream, kH3RequestRejected)); // section 5.2
        }
        release_waiting(in.stream);
        drop_held(in.stream);
        return false;
    }
    static_cast<void>(retain_offer(*session, in.fields)); // anything else counts as absent (section 3.3)
    event = WtEvent{};
    event.kind = WtEventKind::SessionRequest;
    event.session = in.stream;
    event.legacy = legacy;
    event.bytes = cont::ConstSpan<u8>(session->text, session->text_bytes);
    event.fields = in.fields;
    event.info = in.info;
    return true;
}

bool WebTransport::origin_allowed(cont::ConstSpan<HttpField> fields) const noexcept
{
    const HttpField* found = nullptr;
    for (const HttpField& field : fields)
    {
        if (same(field.name, text_of("origin")))
        {
            if (found != nullptr)
            {
                return false;
            }
            found = &field;
        }
    }
    if (found == nullptr || m_config.any_origin)
    {
        return true;
    }
    bool allowed = false;
    for (const cont::ConstSpan<u8> origin : m_config.origins)
    {
        allowed = allowed || same(found->value, origin);
    }
    return allowed;
}

bool WebTransport::retain_offer(Session& session, cont::ConstSpan<HttpField> fields) noexcept
{
    usize size = 0U;
    for (const HttpField& field : fields)
    {
        if (!same(field.name, text_of("wt-available-protocols")))
        {
            continue;
        }
        const usize separator = (size != 0U) ? 2U : 0U;
        if (field.value.size() + separator > m_text_each - size)
        {
            session.text_bytes = 0U;
            return false;
        }
        if (separator != 0U)
        {
            session.text[size] = ',';
            session.text[size + 1U] = ' ';
        }
        if (!field.value.empty())
        {
            std::memcpy(session.text + size + separator, field.value.data(), field.value.size());
        }
        size += separator + field.value.size();
    }
    if (!list_valid(cont::ConstSpan<u8>(session.text, size)))
    {
        session.text_bytes = 0U;
        return false;
    }
    session.text_bytes = static_cast<u32>(size);
    return true;
}

bool WebTransport::read_choice(Session& session, cont::ConstSpan<HttpField> fields, usize& chosen) noexcept
{
    chosen = 0U;
    if (session.text_bytes == 0U)
    {
        return !session.require_protocol; // nothing was offered, so nothing can be chosen
    }
    const HttpField* found = nullptr;
    usize instances = 0U;
    for (const HttpField& field : fields)
    {
        if (same(field.name, text_of("wt-protocol")))
        {
            found = &field;
            ++instances;
        }
    }
    u8 choice[kMaxProtocolBytes];
    usize choice_bytes = 0U;
    if (instances != 1U || !take_item_string(found->value, cont::Span<u8>(choice, sizeof(choice)), choice_bytes))
    {
        return !session.require_protocol; // an Item that is no String is ignored (section 3.3)
    }
    if (choice_bytes > sizeof(choice))
    {
        return false;
    }
    const cont::ConstSpan<u8> offered(session.text, session.text_bytes);
    u8 entry[kMaxProtocolBytes];
    usize entry_bytes = 0U;
    for (usize at = 0U; wt_next_protocol(offered, at, cont::Span<u8>(entry, sizeof(entry)), entry_bytes);)
    {
        if (entry_bytes == choice_bytes && std::memcmp(entry, choice, choice_bytes) == 0)
        {
            std::memcpy(session.text, choice, choice_bytes);
            chosen = choice_bytes;
            return true;
        }
    }
    return false;
}

bool WebTransport::on_response(Session& session, const Http3Event& in, WtEvent& event) noexcept
{
    if (in.info.status < 200U || in.info.status >= 300U)
    {
        const bool reported = end_session(session, 0U, kH3RequestCancelled, event);
        event.status = in.info.status;
        event.fields = in.fields;
        event.info = in.info;
        return reported;
    }
    usize chosen = 0U;
    if (!read_choice(session, in.fields, chosen))
    {
        session.state = State::Open; // from here it is an established session's error
        return end_session(session, kWtAlpnError, kWtAlpnError, event);
    }
    session.state = State::Open;
    session.announce = true;
    session.text_bytes = 0U;
    event = WtEvent{};
    event.kind = WtEventKind::SessionReady;
    event.session = session.id;
    event.status = in.info.status;
    event.legacy = session.legacy;
    event.bytes = cont::ConstSpan<u8>(session.text, chosen);
    event.fields = in.fields;
    event.info = in.info;
    return true;
}

bool WebTransport::on_datagram(const Http3Event& in, WtEvent& event) noexcept
{
    Session* const session = find_session(in.stream);
    if (session == nullptr && !in.unread)
    {
        event = WtEvent{};
        event.kind = WtEventKind::Http;
        event.http = in;
        return true; // another kind of extended CONNECT
    }
    if (in.early)
    {
        return false; // no session ever opens in 0-RTT
    }
    if (session != nullptr && session->state == State::Open)
    {
        event = WtEvent{};
        event.kind = WtEventKind::Datagram;
        event.session = session->id;
        event.bytes = in.payload;
        return true;
    }
    const bool waits = (session != nullptr) ? session_unanswered(*session) : m_http3->wt_session_pending(in.stream);
    if (waits)
    {
        hold_datagram(in.stream, in.payload);
    }
    return false;
}

void WebTransport::hold_datagram(u64 session, cont::ConstSpan<u8> payload) noexcept
{
    if (m_config.waiting_datagrams == 0U || payload.size() > m_config.datagram_bytes)
    {
        return;
    }
    u32 slot = 0U;
    for (u32 at = 0U; at < m_config.waiting_datagrams; ++at)
    {
        if (!m_held[at].used)
        {
            slot = at;
            break;
        }
        if (m_held[at].order < m_held[slot].order)
        {
            slot = at; // full so far: the oldest goes (section 4.6)
        }
    }
    if (slot == m_reported)
    {
        return;
    }
    if (!payload.empty())
    {
        std::memcpy(m_held_bytes + (usize{slot} * m_config.datagram_bytes), payload.data(), payload.size());
    }
    m_held[slot].session = session;
    m_held[slot].order = m_held_order++;
    m_held[slot].bytes = static_cast<u32>(payload.size());
    m_held[slot].used = true;
}

void WebTransport::drop_held(u64 session) noexcept
{
    for (u32 at = 0U; at < m_config.waiting_datagrams; ++at)
    {
        if (at != m_reported && m_held[at].used && m_held[at].session == session)
        {
            m_held[at].used = false;
        }
    }
}

// ---- Streams ----

bool WebTransport::on_stream_opened(const Http3Event& in, WtEvent& event) noexcept
{
    Stream& record = m_records[in.slot];
    record = Stream{};
    record.id = in.stream;
    record.session_id = in.id;
    record.header = in.header;
    record.used = true;
    record.by_peer = true;
    record.send_over = (in.stream & 2U) != 0U;
    Session* const session = find_session(in.id);
    if (session != nullptr && session->state == State::Open)
    {
        return attach(*session, record, event);
    }
    const bool waits = (session != nullptr) ? session_unanswered(*session) : m_http3->wt_session_pending(in.id);
    if (waits)
    {
        hold_stream(record);
        return false;
    }
    static_cast<void>(m_http3->abort_wt(in.stream, kWtSessionGone)); // section 6
    record.used = false;
    return false;
}

void WebTransport::hold_stream(Stream& stream) noexcept
{
    if (m_waiting >= m_config.waiting_streams)
    {
        // Streams naming what has since turned out to be no session should not hold the room.
        for (u32 at = 0U; at < m_record_count; ++at)
        {
            Stream& other = m_records[at];
            if (other.used && other.waiting && find_session(other.session_id) == nullptr &&
                !m_http3->wt_session_pending(other.session_id))
            {
                static_cast<void>(m_http3->abort_wt(other.id, kWtSessionGone));
                other.used = false;
                other.waiting = false;
                --m_waiting;
            }
        }
    }
    if (m_waiting >= m_config.waiting_streams)
    {
        static_cast<void>(m_http3->abort_wt(stream.id, kWtBufferedStreamRejected));
        stream.used = false;
        return;
    }
    stream.waiting = true;
    ++m_waiting;
}

void WebTransport::release_waiting(u64 session) noexcept
{
    for (u32 at = 0U; at < m_record_count && m_waiting != 0U; ++at)
    {
        Stream& stream = m_records[at];
        if (stream.used && stream.waiting && stream.session_id == session)
        {
            static_cast<void>(m_http3->abort_wt(stream.id, kWtSessionGone));
            stream.used = false;
            stream.waiting = false;
            --m_waiting;
        }
    }
}

void WebTransport::end_streams(Session& session) noexcept
{
    const u32 index = session_index(session);
    for (u32 at = 0U; at < m_record_count; ++at)
    {
        Stream& stream = m_records[at];
        if (stream.used && stream.session == index)
        {
            if (!stream.read_over || !stream.send_over)
            {
                static_cast<void>(m_http3->abort_wt(stream.id, kWtSessionGone));
            }
            stream.read_over = true;
            stream.send_over = true;
            stream.session = kNone; // its session's slot may go to another
        }
    }
}

bool WebTransport::announce(Session& session, WtEvent& event) noexcept
{
    for (u32 at = 0U; at < m_record_count && m_waiting != 0U; ++at)
    {
        Stream& stream = m_records[at];
        if (stream.used && stream.waiting && stream.session_id == session.id)
        {
            stream.waiting = false;
            --m_waiting;
            return attach(session, stream, event);
        }
    }
    u32 oldest = kNone;
    for (u32 at = 0U; at < m_config.waiting_datagrams; ++at)
    {
        if (m_held[at].used && m_held[at].session == session.id && at != m_reported &&
            (oldest == kNone || m_held[at].order < m_held[oldest].order))
        {
            oldest = at;
        }
    }
    if (oldest == kNone)
    {
        session.announce = false;
        return false;
    }
    m_reported = oldest;
    event = WtEvent{};
    event.kind = WtEventKind::Datagram;
    event.session = session.id;
    event.bytes =
        cont::ConstSpan<u8>(m_held_bytes + (usize{oldest} * m_config.datagram_bytes), usize{m_held[oldest].bytes});
    return true;
}

bool WebTransport::attach(Session& session, Stream& stream, WtEvent& event) noexcept
{
    stream.session = session_index(session);
    const usize kind = ((stream.id & 2U) != 0U) ? 0U : 1U;
    if (session.flow)
    {
        if (session.peer_opened[kind] >= session.local_streams[kind])
        {
            return end_session(session, kWtFlowControlError, kWtFlowControlError, event);
        }
        ++session.peer_opened[kind];
    }
    if (!account(session, stream, m_streams->received_end(stream.id)))
    {
        return end_session(session, kWtFlowControlError, kWtFlowControlError, event);
    }
    event = WtEvent{};
    event.kind = WtEventKind::StreamOpened;
    event.session = session.id;
    event.stream = stream.id;
    return true;
}

bool WebTransport::account(Session& session, Stream& stream, u64 end) noexcept
{
    const u64 head = stream.by_peer ? u64{stream.header} : 0U;
    const u64 body = (end > head) ? end - head : 0U;
    if (body > stream.seen)
    {
        session.seen += body - stream.seen;
        stream.seen = body;
    }
    return !session.flow || session.seen <= session.local_data;
}

void WebTransport::give_back(Session& session, Stream& stream, u64 final_size) noexcept
{
    const u64 head = stream.by_peer ? 0U : u64{stream.header};
    const u64 body = (final_size > head) ? final_size - head : 0U;
    if (body < stream.written)
    {
        session.sent -= stream.written - body;
        stream.written = body;
        session.credit_arrived = session.credit_wanted;
    }
}

void WebTransport::settle(Session& session, Stream& stream) noexcept
{
    if (!stream.read_over || !stream.send_over || stream.closed)
    {
        return;
    }
    stream.closed = true;
    if (stream.by_peer)
    {
        ++session.peer_closed[((stream.id & 2U) != 0U) ? 0U : 1U];
        refill(session);
    }
}

void WebTransport::refill(Session& session) noexcept
{
    if (!session.flow)
    {
        return;
    }
    if (grant(m_offer.max_data, session.read, kQuicMaxVarint, session.local_data))
    {
        owe(session.owed, kOweMaxData);
    }
    if (grant(m_offer.max_streams_uni, session.peer_closed[0], kWtMaxStreams, session.local_streams[0]))
    {
        owe(session.owed, kOweMaxUni);
    }
    if (grant(m_offer.max_streams_bidi, session.peer_closed[1], kWtMaxStreams, session.local_streams[1]))
    {
        owe(session.owed, kOweMaxBidi);
    }
}

bool WebTransport::on_stream_readable(const Http3Event& in, WtEvent& event) noexcept
{
    Stream* const record = event_stream(in);
    if (record == nullptr || record->waiting || record->read_over || record->session == kNone)
    {
        return false;
    }
    Session& session = m_sessions[record->session];
    if (!account(session, *record, m_streams->received_end(in.stream)))
    {
        return end_session(session, kWtFlowControlError, kWtFlowControlError, event);
    }
    event = WtEvent{};
    event.kind = WtEventKind::StreamReadable;
    event.session = session.id;
    event.stream = in.stream;
    return true;
}

bool WebTransport::on_stream_aborted(const Http3Event& in, WtEvent& event) noexcept
{
    Stream* const record = event_stream(in);
    if (record == nullptr)
    {
        return false;
    }
    if (record->waiting)
    {
        record->used = false;
        record->waiting = false;
        --m_waiting;
        return false;
    }
    if (record->read_over || record->session == kNone)
    {
        return false;
    }
    Session& session = m_sessions[record->session];
    if (!account(session, *record, in.final_size))
    {
        return end_session(session, kWtFlowControlError, kWtFlowControlError, event);
    }
    session.read += record->seen - record->read; // what was never read is no longer held
    record->read = record->seen;
    record->read_over = true;
    settle(session, *record);
    refill(session);
    event = WtEvent{};
    event.kind = WtEventKind::StreamAborted;
    event.session = session.id;
    event.stream = in.stream;
    event.has_code = wt_application_code(in.error, event.code);
    event.error = in.error;
    return true;
}

bool WebTransport::on_stream_stopped(const Http3Event& in, WtEvent& event) noexcept
{
    Stream* const record = event_stream(in);
    if (record == nullptr || record->waiting || record->session == kNone)
    {
        return false;
    }
    Session& session = m_sessions[record->session];
    give_back(session, *record, in.final_size);
    const bool already = record->send_over;
    record->send_over = true;
    settle(session, *record);
    if (already)
    {
        return false;
    }
    event = WtEvent{};
    event.kind = WtEventKind::StreamStopped;
    event.session = session.id;
    event.stream = in.stream;
    event.has_code = wt_application_code(in.error, event.code);
    event.error = in.error;
    return true;
}

// ---- Capsules ----

bool WebTransport::read_capsules(Session& session, WtEvent& event) noexcept
{
    const u64 id = session.id;
    for (;;)
    {
        const Http3Body body = m_http3->peek_body(id);
        if (body.data.empty())
        {
            session.pump = false;
            return false;
        }
        if (session.state == State::Ending)
        {
            static_cast<void>(m_http3->cancel(id, kH3MessageError)); // section 6: nothing may follow the close
            free_session(session);
            return false;
        }
        HttpCapsuleChunk chunk;
        bool got = false;
        const usize used = session.reader.next(body.data, chunk, got);
        const bool produced = got && on_capsule(session, chunk, event);
        if (session.state == State::Free)
        {
            return produced;
        }
        if (m_http3->consume_body(id, used) != Http3Error::None || (used == 0U && !got))
        {
            session.pump = false;
            return produced;
        }
        if (produced)
        {
            return true;
        }
    }
}

bool WebTransport::on_capsule(Session& session, const HttpCapsuleChunk& chunk, WtEvent& event) noexcept
{
    usize most = kVarintBytes;
    switch (chunk.type)
    {
        case kCapsuleClose:
            most = kCloseValueBytes;
            break;
        case kHttpCapsuleDatagram:
            if (chunk.length > m_config.datagram_bytes)
            {
                return false;
            }
            most = m_config.datagram_bytes;
            break;
        case kCapsuleDrain:
            most = 0U;
            break;
        case kCapsuleMaxData:
        case kCapsuleMaxStreamsBidi:
        case kCapsuleMaxStreamsUni:
        case kCapsuleDataBlocked:
        case kCapsuleStreamsBlockedBidi:
        case kCapsuleStreamsBlockedUni:
            break;
        case kCapsuleMaxStreamData:
        case kCapsuleStreamDataBlocked:
            return end_session(session, kWtFlowControlError, kWtFlowControlError, event); // section 5.4
        default:
            return false; // RFC 9297 section 3.2: skipped
    }
    if (chunk.length > most)
    {
        return end_session(session, kH3MessageError, kH3MessageError, event);
    }
    if (!chunk.value.empty())
    {
        std::memcpy(session.text + chunk.offset, chunk.value.data(), chunk.value.size());
    }
    return chunk.complete && on_complete_capsule(session, chunk.type, static_cast<usize>(chunk.length), event);
}

bool WebTransport::on_complete_capsule(Session& session, u64 type, usize size, WtEvent& event) noexcept
{
    if (type == kCapsuleClose)
    {
        return on_close_capsule(session, size, event);
    }
    event = WtEvent{};
    event.session = session.id;
    if (type == kHttpCapsuleDatagram)
    {
        event.kind = WtEventKind::Datagram;
        event.bytes = cont::ConstSpan<u8>(session.text, size);
        return true;
    }
    if (type == kCapsuleDrain)
    {
        event.kind = WtEventKind::SessionDraining;
        return true;
    }
    if (!session.flow)
    {
        return false; // section 5.1
    }
    cont::WireReader in(cont::ConstSpan<u8>(session.text, size));
    u64 value = 0U;
    if (!take_quic_varint(in, value) || !in.rest().empty())
    {
        return end_session(session, kH3MessageError, kH3MessageError, event);
    }
    return on_flow_capsule(session, type, value, event);
}

bool WebTransport::on_flow_capsule(Session& session, u64 type, u64 value, WtEvent& event) noexcept
{
    const bool streams = type != kCapsuleMaxData && type != kCapsuleDataBlocked;
    if (streams && value > kWtMaxStreams)
    {
        return end_session(session, kWtFlowControlError, kWtFlowControlError, event);
    }
    if (type == kCapsuleDataBlocked || type == kCapsuleStreamsBlockedBidi || type == kCapsuleStreamsBlockedUni)
    {
        return false;
    }
    const usize kind = (type == kCapsuleMaxStreamsBidi) ? 1U : 0U;
    u64& limit = (type == kCapsuleMaxData) ? session.peer_data : session.peer_streams[kind];
    if (value <= limit)
    {
        return end_session(session, kWtFlowControlError, kWtFlowControlError, event);
    }
    limit = value;
    session.credit_arrived = session.credit_wanted;
    return false;
}

bool WebTransport::on_close_capsule(Session& session, usize size, WtEvent& event) noexcept
{
    const cont::ConstSpan<u8> value(session.text, size);
    if (size < sizeof(u32) || !utf8_valid(value.subspan(sizeof(u32))))
    {
        return end_session(session, kH3MessageError, kH3MessageError, event);
    }
    event = WtEvent{};
    event.kind = WtEventKind::SessionClosed;
    event.session = session.id;
    event.code = cont::wire_load<u32>(value, 0U);
    event.has_code = true;
    event.legacy = session.legacy;
    event.bytes = value.subspan(sizeof(u32));
    release_waiting(session.id);
    end_streams(session);
    drop_held(session.id);
    session.state = State::Ending;
    session.owed = kOweFin;
    session.pump = true; // anything after the capsule is an error
    flush(session);
    return true;
}

bool WebTransport::end_session(Session& session, u64 error, u64 cancel_code, WtEvent& event) noexcept
{
    event = WtEvent{};
    event.kind = (session.state == State::Connecting) ? WtEventKind::SessionRefused : WtEventKind::SessionClosed;
    event.session = session.id;
    event.error = error;
    event.legacy = session.legacy;
    static_cast<void>(m_http3->cancel(session.id, cancel_code));
    release_waiting(session.id);
    end_streams(session);
    drop_held(session.id);
    free_session(session);
    return true;
}

void WebTransport::finish_close(Session& session) noexcept
{
    static_cast<void>(m_http3->cancel(session.id, kWtSessionGone));
    free_session(session);
}

void WebTransport::settle_ending(Session& session) noexcept
{
    if (session.state == State::Ending && session.peer_ended && session.owed == 0U)
    {
        free_session(session);
    }
}

bool WebTransport::write_capsule(Session& session, cont::ConstSpan<u8> bytes, bool fin) noexcept
{
    const usize need = bytes.size() + kDataFrameHeadBytes;
    if (m_streams->send_room(session.id, need) < need)
    {
        return false; // a Writable event is owed
    }
    usize taken = 0U;
    if (m_http3->send_body(session.id, bytes, fin, taken) == Http3Error::None && taken == bytes.size())
    {
        return true;
    }
    session.owed = 0U; // its sending part has gone
    return false;
}

void WebTransport::flush(Session& session) noexcept
{
    while (session.owed != 0U && m_http3->error() == 0U)
    {
        const u8 bit = static_cast<u8>(session.owed & static_cast<u8>(-session.owed));
        if (bit == kOweClose)
        {
            if (write_capsule(session, cont::ConstSpan<u8>(session.text, session.text_bytes), true) ||
                session.owed == 0U)
            {
                finish_close(session);
            }
            return;
        }
        if (bit == kOweFin)
        {
            static_cast<void>(m_http3->finish(session.id));
            session.owed = static_cast<u8>(session.owed & ~bit);
            settle_ending(session);
            continue;
        }
        u64 type = 0U;
        u64 value = 0U;
        switch (bit)
        {
            case kOweMaxData:
                type = kCapsuleMaxData;
                value = session.local_data;
                break;
            case kOweMaxUni:
                type = kCapsuleMaxStreamsUni;
                value = session.local_streams[0];
                break;
            case kOweMaxBidi:
                type = kCapsuleMaxStreamsBidi;
                value = session.local_streams[1];
                break;
            case kOweDataBlocked:
                type = kCapsuleDataBlocked;
                value = session.peer_data;
                break;
            case kOweUniBlocked:
                type = kCapsuleStreamsBlockedUni;
                value = session.peer_streams[0];
                break;
            default:
                type = kCapsuleStreamsBlockedBidi;
                value = session.peer_streams[1];
                break;
        }
        u8 bytes[kCapsuleHeadBytes + kVarintBytes];
        cont::WireWriter out{cont::Span<u8>(bytes, sizeof(bytes))};
        put_http_capsule_header(out, type, quic_varint_bytes(value));
        put_quic_varint(out, value);
        if (!write_capsule(session, cont::ConstSpan<u8>(bytes, out.written()), false))
        {
            return;
        }
        session.owed = static_cast<u8>(session.owed & ~bit);
    }
}

// ---- The application's side ----

Http3Error WebTransport::connect(cont::ConstSpan<HttpField> fields, u64& session, bool require_protocol) noexcept
{
    if (m_http3->error() != 0U)
    {
        return Http3Error::Closed;
    }
    HttpMessageInfo info;
    bool legacy = false;
    if (m_side != crypto::QuicSide::Client || !http_check_request(fields, info) || !info.extended ||
        !http3_wt_token(info.protocol, legacy) || !same(info.scheme, text_of("https")))
    {
        return Http3Error::Invalid;
    }
    if (!m_http3->wt_ready(legacy))
    {
        return Http3Error::NotReady;
    }
    Session* const claimed = admits(legacy) ? claim_session(0U, legacy) : nullptr;
    if (claimed == nullptr)
    {
        return Http3Error::NoStream;
    }
    if (!retain_offer(*claimed, fields))
    {
        free_session(*claimed);
        return Http3Error::Invalid;
    }
    const Http3Error error = m_http3->open_request(fields, false, session);
    if (error != Http3Error::None)
    {
        free_session(*claimed);
        return error;
    }
    claimed->id = session;
    claimed->state = State::Connecting;
    claimed->require_protocol = require_protocol;
    return Http3Error::None;
}

Http3Error WebTransport::accept(u64 session, cont::ConstSpan<HttpField> fields, cont::ConstSpan<u8> protocol) noexcept
{
    if (m_http3->error() != 0U)
    {
        return Http3Error::Closed;
    }
    Session* const found = find_session(session);
    if (found == nullptr || found->state != State::Requested)
    {
        return Http3Error::Unknown;
    }
    if (fields.size() > kExtraResponseFields)
    {
        return Http3Error::TooLarge;
    }
    HttpField out[kExtraResponseFields + 2U];
    usize count = 0U;
    out[count].name = text_of(":status");
    out[count].value = text_of("200");
    ++count;
    for (const HttpField& field : fields)
    {
        out[count++] = field;
    }
    u8 quoted[(2U * kMaxProtocolBytes) + 2U];
    if (!protocol.empty())
    {
        if (protocol.size() > kMaxProtocolBytes)
        {
            return Http3Error::TooLarge;
        }
        const cont::ConstSpan<u8> offered(found->text, found->text_bytes);
        u8 entry[kMaxProtocolBytes];
        usize entry_bytes = 0U;
        bool listed = false;
        const cont::Span<u8> room(entry, sizeof(entry));
        for (usize at = 0U; !listed && wt_next_protocol(offered, at, room, entry_bytes);)
        {
            listed = same(cont::ConstSpan<u8>(entry, (entry_bytes <= sizeof(entry)) ? entry_bytes : 0U), protocol);
        }
        if (!listed)
        {
            return Http3Error::Invalid;
        }
        usize size = 0U;
        quoted[size++] = '"';
        for (const u8 c : protocol)
        {
            if (c == '"' || c == '\\')
            {
                quoted[size++] = '\\';
            }
            quoted[size++] = c;
        }
        quoted[size++] = '"';
        out[count].name = text_of("wt-protocol");
        out[count].value = cont::ConstSpan<u8>(quoted, size);
        ++count;
    }
    const Http3Error error = m_http3->send_response(session, cont::ConstSpan<HttpField>(out, count), false);
    if (error != Http3Error::None)
    {
        return error;
    }
    found->state = State::Open;
    found->pump = true;
    found->announce = true;
    found->text_bytes = 0U;
    return Http3Error::None;
}

Http3Error WebTransport::refuse(u64 id, u16 status) noexcept
{
    u8 digits[3] = {static_cast<u8>('0' + ((status / 100U) % 10U)), static_cast<u8>('0' + ((status / 10U) % 10U)),
                    static_cast<u8>('0' + (status % 10U))};
    HttpField field;
    field.name = text_of(":status");
    field.value = cont::ConstSpan<u8>(digits, sizeof(digits));
    const Http3Error error = m_http3->send_response(id, cont::ConstSpan<HttpField>(&field, 1U), true);
    if (error == Http3Error::Blocked)
    {
        return error;
    }
    static_cast<void>(m_http3->cancel(id, (error == Http3Error::None) ? kH3NoError : kH3RequestRejected));
    return Http3Error::None;
}

Http3Error WebTransport::reject(u64 session, u16 status) noexcept
{
    if (m_http3->error() != 0U)
    {
        return Http3Error::Closed;
    }
    Session* const found = find_session(session);
    if (found == nullptr || found->state != State::Requested)
    {
        return Http3Error::Unknown;
    }
    if (status < 300U || status > 599U)
    {
        return Http3Error::Invalid;
    }
    const Http3Error error = refuse(session, status);
    if (error != Http3Error::None)
    {
        return error;
    }
    release_waiting(session);
    drop_held(session);
    free_session(*found);
    return Http3Error::None;
}

Http3Error WebTransport::close(u64 session, u32 code, cont::ConstSpan<u8> message) noexcept
{
    if (m_http3->error() != 0U)
    {
        return Http3Error::Closed;
    }
    Session* const found = find_session(session);
    if (found == nullptr || (found->state != State::Open && found->state != State::Connecting))
    {
        return Http3Error::Unknown;
    }
    usize keep = 0U;
    while (keep < message.size())
    {
        const usize length = utf8_sequence_length(message.data() + keep, message.size() - keep);
        if (length == 0U)
        {
            return Http3Error::Invalid;
        }
        if (keep + length > kWtMaxCloseMessage)
        {
            break;
        }
        keep += length;
    }
    cont::WireWriter out{cont::Span<u8>(found->text, m_text_each)};
    put_http_capsule_header(out, kCapsuleClose, sizeof(u32) + keep);
    out.put_u32(code);
    for (usize at = 0U; at < keep; ++at)
    {
        out.put_u8(message[at]);
    }
    found->text_bytes = static_cast<u32>(out.written());
    release_waiting(session);
    end_streams(*found);
    drop_held(session);
    found->state = State::Closing;
    found->owed = kOweClose;
    flush(*found);
    return Http3Error::None;
}

Http3Error WebTransport::drain(u64 session) noexcept
{
    if (m_http3->error() != 0U)
    {
        return Http3Error::Closed;
    }
    Session* const found = find_session(session);
    if (found == nullptr || found->state != State::Open)
    {
        return Http3Error::Unknown;
    }
    u8 bytes[kCapsuleHeadBytes];
    cont::WireWriter out{cont::Span<u8>(bytes, sizeof(bytes))};
    put_http_capsule_header(out, kCapsuleDrain, 0U);
    return write_capsule(*found, cont::ConstSpan<u8>(bytes, out.written()), false) ? Http3Error::None
                                                                                   : Http3Error::Blocked;
}

Http3Error WebTransport::open_stream(u64 session, bool bidirectional, u64& stream) noexcept
{
    if (m_http3->error() != 0U)
    {
        return Http3Error::Closed;
    }
    Session* const found = find_session(session);
    if (found == nullptr || (found->state != State::Open && found->state != State::Connecting))
    {
        return Http3Error::Unknown;
    }
    const usize kind = bidirectional ? 1U : 0U;
    if (found->flow && found->opened[kind] >= found->peer_streams[kind])
    {
        if (found->streams_blocked[kind] != found->peer_streams[kind])
        {
            found->streams_blocked[kind] = found->peer_streams[kind];
            owe(found->owed, bidirectional ? kOweBidiBlocked : kOweUniBlocked);
            flush(*found);
        }
        found->credit_wanted = true;
        return Http3Error::NoStream;
    }
    const Http3Error error = m_http3->open_wt_stream(session, bidirectional, stream);
    if (error != Http3Error::None)
    {
        return error;
    }
    ++found->opened[kind];
    Stream& record = m_records[m_streams->slot_of(stream)];
    record = Stream{};
    record.id = stream;
    record.session_id = session;
    record.session = session_index(*found);
    record.header = m_http3->wt_header(stream);
    record.used = true;
    record.read_over = !bidirectional;
    return Http3Error::None;
}

Http3Error WebTransport::write(u64 stream, cont::ConstSpan<u8> data, bool fin, usize& taken) noexcept
{
    taken = 0U;
    if (m_http3->error() != 0U)
    {
        return Http3Error::Closed;
    }
    Stream* const record = find_stream(stream);
    if (record == nullptr || record->send_over)
    {
        return Http3Error::Unknown;
    }
    Session& session = m_sessions[record->session];
    usize offered = data.size();
    if (session.flow && session.peer_data - session.sent < offered)
    {
        offered = static_cast<usize>(session.peer_data - session.sent);
        session.credit_wanted = true;
        if (session.data_blocked != session.peer_data)
        {
            session.data_blocked = session.peer_data;
            owe(session.owed, kOweDataBlocked);
        }
    }
    if (offered == 0U && !data.empty())
    {
        flush(session);
        return Http3Error::Blocked;
    }
    const bool ends = fin && offered == data.size();
    switch (m_streams->write(stream, data.subspan(0U, offered), ends, taken))
    {
        case QuicStreamError::None:
            break;
        case QuicStreamError::Ended:
        case QuicStreamError::Unknown:
            return Http3Error::Unknown;
        default:
            return Http3Error::Invalid;
    }
    session.sent += taken;
    record->written += taken;
    if (ends && taken == data.size())
    {
        record->send_over = true;
        settle(session, *record);
    }
    flush(session);
    return Http3Error::None;
}

Http3Error WebTransport::reset(u64 stream, u32 code, u64 offset) noexcept
{
    if (m_http3->error() != 0U)
    {
        return Http3Error::Closed;
    }
    Stream* const record = find_stream(stream);
    if (record == nullptr)
    {
        return Http3Error::Unknown;
    }
    const Http3Error error = m_http3->reset_wt(stream, code, offset);
    if (error != Http3Error::None)
    {
        return error;
    }
    Session& session = m_sessions[record->session];
    give_back(session, *record, m_streams->reset_size(stream));
    record->send_over = true;
    settle(session, *record);
    flush(session);
    return Http3Error::None;
}

Http3Error WebTransport::stop(u64 stream, u32 code) noexcept
{
    if (m_http3->error() != 0U)
    {
        return Http3Error::Closed;
    }
    return (find_stream(stream) != nullptr) ? m_http3->stop_wt(stream, code) : Http3Error::Unknown;
}

QuicStreamRead WebTransport::peek(u64 stream) const noexcept
{
    return (find_stream(stream) != nullptr) ? m_streams->peek(stream) : QuicStreamRead{};
}

Http3Error WebTransport::consume(u64 stream, usize count) noexcept
{
    if (m_http3->error() != 0U)
    {
        return Http3Error::Closed;
    }
    Stream* const record = find_stream(stream);
    if (record == nullptr || record->read_over)
    {
        return Http3Error::Unknown;
    }
    const QuicStreamRead in = m_streams->peek(stream);
    if (count > in.data.size())
    {
        return Http3Error::Invalid;
    }
    Session& session = m_sessions[record->session];
    const bool ends = in.fin && count == in.data.size();
    if (m_streams->consume(stream, count) != QuicStreamError::None)
    {
        return Http3Error::Invalid;
    }
    record->read += count;
    session.read += count;
    record->read_over = ends;
    settle(session, *record);
    refill(session);
    flush(session);
    return Http3Error::None;
}

Http3Error WebTransport::send_datagram(u64 session, cont::ConstSpan<u8> payload, u64 now_ns) noexcept
{
    if (m_http3->error() != 0U)
    {
        return Http3Error::Closed;
    }
    const Session* const found = find_session(session);
    if (found == nullptr || (found->state != State::Open && found->state != State::Connecting))
    {
        return Http3Error::Unknown;
    }
    return m_http3->send_datagram(session, payload, now_ns);
}

crypto::TlsError WebTransport::export_keying_material(const crypto::QuicHandshake& handshake, u64 session,
                                                      cont::ConstSpan<u8> label, cont::ConstSpan<u8> context,
                                                      cont::Span<u8> out) const noexcept
{
    const Session* const found = find_session(session);
    if (found == nullptr || found->state != State::Open)
    {
        return crypto::TlsError::NotReady;
    }
    u8 bytes[kWtMaxExporterContext];
    const usize size = wt_exporter_context(session, label, context, cont::Span<u8>(bytes, sizeof(bytes)));
    if (size == 0U)
    {
        return crypto::TlsError::BadConfig;
    }
    return handshake.export_keying_material("EXPORTER-WebTransport", cont::ConstSpan<u8>(bytes, size), out);
}

} // namespace crd::net
