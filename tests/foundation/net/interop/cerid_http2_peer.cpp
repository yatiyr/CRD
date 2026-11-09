// crd-net interoperability test (4.2a) -- HTTP/2 over crd-net's Http2Connection and real TCP on the loopback, with
// prior knowledge. http2_interop.py runs it against crd-nghttp2-peer and h2spec.
//
// Usage: crd-http2-peer --role client|server --port N [--h2spec 1] [--max-streams N] [--window N]
//                       [--connection-window N] [--frame-bytes N] and the options of http_exchange.h
//
//   h2spec       a server of many connections that answers every request and runs until it is killed
//   max-streams  SETTINGS_MAX_CONCURRENT_STREAMS
//   window       SETTINGS_INITIAL_WINDOW_SIZE
//   connection-window  what the connection's receive window is, at least 65535
//   frame-bytes  SETTINGS_MAX_FRAME_SIZE
//
// The report of a connection goes to standard output when it has ended, the same names from both programs.

#include "http_exchange.h"

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/http2.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/stream_poller.hpp>
#include <crd/net/stream_socket.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
using namespace crd;
using namespace crd::net;
using crd::containers::ConstSpan;
using crd::containers::Span;

constexpr u64 kRunNs = 60'000'000'000U;
constexpr u64 kWaitNs = 1'000'000U;
constexpr u32 kSlots = 32U;
constexpr usize kMaxFields = 12U;
constexpr usize kNumberText = 24U;
constexpr usize kInputBytes = kHttp2FrameHeaderBytes + (1U << 20U);
constexpr usize kResponderBody = 256U;
constexpr u32 kResponderStreams = 256U;
constexpr u64 kNoMoreRequests = 1U;

struct Options
{
    httpx_options_t exchange{};
    u64 port = 0U;
    bool client = false;
    bool role_given = false;
    bool port_given = false;
    bool h2spec = false;
    u32 max_streams = 64U;
    u32 window = 1U << 20U;
    u32 connection_window = 1U << 24U;
    u32 frame_bytes = kHttp2MinFrameBytes;
};

[[nodiscard]] ConstSpan<u8> text_bytes(const char* text, usize size) noexcept
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(text), size);
}

class Fields
{
public:
    void add(const char* name, const char* value, usize size) noexcept
    {
        m_items[m_count].name = text_bytes(name, std::strlen(name));
        m_items[m_count].value = text_bytes(value, size);
        ++m_count;
    }

    void add_text(const char* name, const char* value) noexcept { add(name, value, std::strlen(value)); }

    [[nodiscard]] ConstSpan<HttpField> span() const noexcept { return ConstSpan<HttpField>(m_items, m_count); }

private:
    HttpField m_items[kMaxFields];
    usize m_count = 0U;
};

// What runs over one connection.
class Http2App
{
public:
    Http2App() noexcept = default;
    Http2App(const Http2App&) = delete;
    Http2App& operator=(const Http2App&) = delete;
    Http2App(Http2App&&) = delete;
    Http2App& operator=(Http2App&&) = delete;
    virtual ~Http2App() = default;

    virtual void on_event(Http2Connection& h2, const Http2Event& event) noexcept = 0;
    virtual void service(Http2Connection& h2) noexcept = 0;
};

// Answers every request with a body once it has ended: what h2spec asks of a server.
class Responder final : public Http2App
{
public:
    void reset() noexcept { m_count = 0U; }

    void on_event(Http2Connection& h2, const Http2Event& event) noexcept override
    {
        switch (event.kind)
        {
            case Http2EventKind::Headers:
                if (m_count < kResponderStreams)
                {
                    m_items[m_count] = Item{};
                    m_items[m_count].id = event.stream;
                    ++m_count;
                }
                break;
            case Http2EventKind::Body:
                static_cast<void>(h2.consume_body(event.stream, event.payload.size()));
                break;
            case Http2EventKind::Finished:
                mark(event.stream);
                break;
            case Http2EventKind::Aborted:
                forget(event.stream);
                break;
            case Http2EventKind::Settings:
            case Http2EventKind::GoAway:
            case Http2EventKind::Interim:
            case Http2EventKind::Trailers:
            case Http2EventKind::Writable:
                break;
        }
    }

    void service(Http2Connection& h2) noexcept override
    {
        for (u32 at = 0U; at < m_count;)
        {
            if (m_items[at].ready && advance(h2, m_items[at]))
            {
                m_items[at] = m_items[m_count - 1U];
                --m_count;
            }
            else
            {
                ++at;
            }
        }
    }

private:
    struct Item
    {
        u32 id = 0U;
        usize sent = 0U;
        bool ready = false;
        bool head = false;
    };

    void mark(u32 id) noexcept
    {
        for (u32 at = 0U; at < m_count; ++at)
        {
            if (m_items[at].id == id)
            {
                m_items[at].ready = true;
            }
        }
    }

    void forget(u32 id) noexcept
    {
        for (u32 at = 0U; at < m_count; ++at)
        {
            if (m_items[at].id == id)
            {
                m_items[at] = m_items[m_count - 1U];
                --m_count;
                return;
            }
        }
    }

    // True when the item has nothing left to send.
    [[nodiscard]] static bool advance(Http2Connection& h2, Item& item) noexcept
    {
        if (!item.head)
        {
            Fields fields;
            fields.add_text(":status", "200");
            const Http2Error error = h2.send_response(item.id, fields.span(), false);
            if (error != Http2Error::None)
            {
                return error != Http2Error::Blocked;
            }
            item.head = true;
        }
        u8 body[kResponderBody];
        std::memset(body, 'x', sizeof(body));
        while (item.sent < sizeof(body))
        {
            usize taken = 0U;
            const Http2Error error = h2.send_body(item.id, ConstSpan<u8>(body + item.sent, sizeof(body) - item.sent),
                                                  true, taken);
            item.sent += taken;
            if (error != Http2Error::None)
            {
                return error != Http2Error::Blocked;
            }
        }
        return true;
    }

    Item m_items[kResponderStreams];
    u32 m_count = 0U;
};

// The exchange of http_exchange.h.
class Exchange final : public Http2App
{
public:
    Exchange(const httpx_options_t& options, bool client) noexcept : m_options(options), m_client(client) {}

    void on_event(Http2Connection& h2, const Http2Event& event) noexcept override
    {
        Stream& stream = m_streams[slot_of(index_of(event.stream))];
        const u64 index = index_of(event.stream);
        switch (event.kind)
        {
            case Http2EventKind::Settings:
            case Http2EventKind::Writable:
                break;
            case Http2EventKind::GoAway:
                on_goaway(event.stream);
                break;
            case Http2EventKind::Headers:
                on_headers(h2, event, index, stream);
                break;
            case Http2EventKind::Interim:
                ++m_interims;
                break;
            case Http2EventKind::Body:
                on_body(h2, event, index, stream);
                break;
            case Http2EventKind::Trailers:
                on_trailers(event, index, stream);
                break;
            case Http2EventKind::Finished:
                on_finished(h2, index, stream);
                break;
            case Http2EventKind::Aborted:
                on_aborted(event.error, stream);
                break;
        }
    }

    void service(Http2Connection& h2) noexcept override
    {
        if (m_client && h2.settings_known())
        {
            open_requests(h2);
            client_goaway(h2);
        }
        for (u64 index = 0U; index < m_options.requests; ++index)
        {
            pump(h2, index);
        }
    }

    [[nodiscard]] bool complete(const Http2Connection& h2) const noexcept
    {
        if (!h2.settings_known() || m_opened < m_options.requests ||
            m_completed + m_rejected + m_aborted + m_cancelled + m_refused_too_large < m_options.requests)
        {
            return false;
        }
        if (m_options.goaway == HTTPX_GOAWAY_NOTICE)
        {
            return m_goaway_seen >= 2U && m_refused_after_goaway != 0U;
        }
        return m_options.goaway != HTTPX_GOAWAY_REJECT || m_goaway_seen >= 1U;
    }

    void report(const Http2Connection& h2) const noexcept
    {
        std::printf("connection=1\nh2_requests=%llu\nh2_seen=%llu\nh2_completed=%llu\nh2_rejected=%llu\n"
                    "h2_aborted=%llu\nh2_abort_code=%llu\n",
                    ull(m_opened), ull(m_seen), ull(m_completed), ull(m_rejected), ull(m_aborted), ull(m_abort_code));
        std::printf("h2_cancelled=%llu\nh2_status_431=%llu\nh2_refused_too_large=%llu\nh2_refused_after_goaway=%llu\n"
                    "h2_goaway_seen=%llu\nh2_goaway_id=%llu\n",
                    ull(m_cancelled), ull(m_status_431), ull(m_refused_too_large), ull(m_refused_after_goaway),
                    ull(m_goaway_seen), ull(m_goaway_id));
        std::printf("h2_body_received=%llu\nh2_body_sent=%llu\nh2_interims=%llu\nh2_trailers=%llu\nh2_tunnels=%llu\n"
                    "h2_verified=%d\nh2_error=%llu\n",
                    ull(m_body_received), ull(m_body_sent), ull(m_interims), ull(m_trailers), ull(m_tunnels),
                    m_unverified ? 0 : 1, ull(h2.error()));
        const Http2Settings& peer = h2.peer_settings();
        std::printf("h2_peer_known=%d\nh2_peer_table=%llu\nh2_peer_field_limit=%llu\nh2_peer_connect=%d\n"
                    "h2_peer_streams=%llu\nh2_peer_window=%llu\nh2_peer_frame=%llu\n",
                    h2.settings_known() ? 1 : 0, ull(peer.header_table_size), ull(peer.max_field_section_size),
                    peer.enable_connect_protocol ? 1 : 0, ull(peer.max_concurrent_streams), ull(peer.initial_window),
                    ull(peer.max_frame_bytes));
        std::printf("h2_inserts_received=%llu\nh2_inserts_sent=%llu\n", ull(h2.hpack_decoder().table().inserted()),
                    ull(h2.hpack_encoder().table().inserted()));
        std::fflush(stdout);
    }

private:
    struct Stream
    {
        httpx_section_t section{};
        u64 received = 0U;
        u64 sent = 0U;
        bool used = false;
        bool tunnel_open = false;
        bool ended = false;
        bool answer_due = false;
        bool interim_sent = false;
        bool head_sent = false;
        bool out_end = false;
        bool counted = false;
        bool trailers_seen = false;
    };

    [[nodiscard]] static unsigned long long ull(u64 value) noexcept { return value; }
    [[nodiscard]] static u64 index_of(u64 id) noexcept { return id != 0U ? (id - 1U) / 2U : 0U; }
    [[nodiscard]] static u64 slot_of(u64 index) noexcept { return index < HTTPX_MAX_REQUESTS ? index : 0U; }

    void take_section(const Http2Event& event, u64 index, Stream& stream) const noexcept
    {
        stream.section = httpx_section_t{};
        for (const HttpField& field : event.fields)
        {
            httpx_take_field(&stream.section, &m_options, index, field.name.data(), field.name.size(),
                             field.value.data(), field.value.size());
        }
    }

    void on_headers(Http2Connection& h2, const Http2Event& event, u64 index, Stream& stream) noexcept
    {
        take_section(event, index, stream);
        if (m_client)
        {
            if (httpx_is_tunnel(&m_options, index) != 0 && stream.section.status == 200U)
            {
                stream.tunnel_open = true;
                ++m_tunnels;
            }
            return;
        }
        stream.used = true;
        m_unverified = m_unverified || httpx_section_good(&stream.section, &m_options, index, 0) == 0;
        ++m_seen;
        if (m_seen == 1U)
        {
            first_request(h2);
        }
        if (m_options.cancel == HTTPX_CANCEL_SERVER && httpx_last_request(&m_options, index) != 0)
        {
            cancel(h2, event.stream, stream);
            return;
        }
        if (httpx_is_tunnel(&m_options, index) != 0)
        {
            ++m_tunnels;
            stream.answer_due = true;
        }
    }

    void first_request(Http2Connection& h2) noexcept
    {
        if (m_options.goaway == HTTPX_GOAWAY_NOTICE)
        {
            m_unverified = m_unverified || !h2.goaway(false);
        }
        else if (m_options.goaway == HTTPX_GOAWAY_REJECT)
        {
            m_unverified = m_unverified || !h2.goaway(true);
        }
    }

    void cancel(Http2Connection& h2, u32 id, Stream& stream) noexcept
    {
        if (!stream.counted && h2.cancel(id, kH2Cancel) == Http2Error::None)
        {
            stream.counted = true;
            stream.out_end = true;
            ++m_cancelled;
        }
    }

    void on_body(Http2Connection& h2, const Http2Event& event, u64 index, Stream& stream) noexcept
    {
        const u64 tag = m_client ? 1U : 0U;
        for (usize at = 0U; at < event.payload.size(); ++at)
        {
            m_unverified = m_unverified || event.payload[at] != httpx_pattern(tag, event.stream, stream.received + at);
        }
        stream.received += event.payload.size();
        m_body_received += event.payload.size();
        const Http2Error given = h2.consume_body(event.stream, event.payload.size());
        m_unverified = m_unverified || (given != Http2Error::None && given != Http2Error::Unknown);
        if (m_client && m_options.cancel == HTTPX_CANCEL_CLIENT && httpx_last_request(&m_options, index) != 0)
        {
            cancel(h2, event.stream, stream);
        }
    }

    void on_trailers(const Http2Event& event, u64 index, Stream& stream) const noexcept
    {
        for (const HttpField& field : event.fields)
        {
            if (httpx_text_is(field.name.data(), field.name.size(), "x-cerid-trailer") != 0)
            {
                stream.trailers_seen = httpx_decimal(field.value.data(), field.value.size()) == index;
            }
        }
    }

    void on_finished(Http2Connection& h2, u64 index, Stream& stream) noexcept
    {
        stream.ended = true;
        if (m_client)
        {
            response_ended(index, stream);
            return;
        }
        const bool trailers = httpx_has_trailers(&m_options, index) != 0 && !stream.trailers_seen;
        m_unverified = m_unverified || stream.received != httpx_body_bytes(&m_options, index, 0) || trailers;
        m_trailers += stream.trailers_seen ? 1U : 0U;
        ++m_request_ended;
        if (m_options.goaway == HTTPX_GOAWAY_NOTICE && m_request_ended == m_options.requests)
        {
            m_unverified = m_unverified || !h2.goaway(true);
        }
        if (httpx_is_tunnel(&m_options, index) == 0)
        {
            stream.answer_due = m_options.goaway != HTTPX_GOAWAY_CLIENT || m_goaway_seen >= kNoMoreRequests;
        }
    }

    void response_ended(u64 index, Stream& stream) noexcept
    {
        if (stream.counted)
        {
            return;
        }
        stream.counted = true;
        ++m_completed;
        if (m_options.over_limit != 0 && httpx_last_request(&m_options, index) != 0 && stream.section.status == 431U)
        {
            ++m_status_431;
            return;
        }
        const bool trailers = httpx_has_trailers(&m_options, index) != 0 && !stream.trailers_seen;
        m_unverified = m_unverified || httpx_section_good(&stream.section, &m_options, index, 1) == 0 ||
                       stream.received != httpx_body_bytes(&m_options, index, 1) || trailers;
        m_trailers += stream.trailers_seen ? 1U : 0U;
    }

    void on_aborted(u32 error, Stream& stream) noexcept
    {
        if (stream.counted)
        {
            return;
        }
        stream.counted = true;
        if (error == kH2RefusedStream)
        {
            ++m_rejected;
        }
        else
        {
            ++m_aborted;
            m_abort_code = error;
        }
    }

    void on_goaway(u32 last) noexcept
    {
        ++m_goaway_seen;
        m_goaway_id = last;
        if (!m_client && m_options.goaway == HTTPX_GOAWAY_CLIENT)
        {
            for (u64 index = 0U; index < m_options.requests; ++index)
            {
                m_streams[index].answer_due = m_streams[index].ended && httpx_is_tunnel(&m_options, index) == 0;
            }
        }
    }

    void common_fields(Fields& fields, u64 index, const char* text) const noexcept
    {
        const u64 fill = httpx_fill_bytes(&m_options, index);
        fields.add_text("x-cerid-repeat", HTTPX_REPEAT_VALUE);
        fields.add_text("x-cerid-index", text);
        if (fill != 0U)
        {
            fields.add("x-cerid-fill", fill_text(), static_cast<usize>(fill));
        }
    }

    [[nodiscard]] static const char* fill_text() noexcept
    {
        static const FillText kText;
        return kText.bytes;
    }

    struct FillText
    {
        char bytes[HTTPX_MAX_FILL];
        FillText() noexcept
        {
            for (usize at = 0U; at < sizeof(bytes); ++at)
            {
                bytes[at] = httpx_fill_byte(at);
            }
        }
    };

    void open_requests(Http2Connection& h2) noexcept
    {
        while (m_opened < m_options.requests)
        {
            const u64 index = m_opened;
            const u64 body = httpx_body_bytes(&m_options, index, 0);
            const bool tunnel = httpx_is_tunnel(&m_options, index) != 0;
            char text[kNumberText];
            char length[kNumberText];
            char path[kNumberText + 8U];
            std::snprintf(text, sizeof(text), "%llu", ull(index));
            std::snprintf(length, sizeof(length), "%llu", ull(body));
            std::snprintf(path, sizeof(path), tunnel ? "/tunnel/%llu" : "/cerid/%llu", ull(index));
            Fields fields;
            fields.add_text(":method", httpx_method(tunnel, body));
            fields.add_text(":scheme", "http");
            fields.add_text(":authority", HTTPX_SERVER_NAME);
            fields.add_text(":path", path);
            if (tunnel)
            {
                fields.add_text(":protocol", HTTPX_TUNNEL_PROTOCOL);
            }
            common_fields(fields, index, text);
            if (httpx_has_length(&m_options, index, body) != 0 && body != 0U)
            {
                fields.add_text("content-length", length);
            }
            const bool fin = body == 0U && httpx_has_trailers(&m_options, index) == 0 && !tunnel;
            u32 id = 0U;
            const Http2Error error = h2.open_request(fields.span(), fin, id);
            if (error == Http2Error::TooLarge && m_options.over_limit != 0 &&
                httpx_last_request(&m_options, index) != 0)
            {
                ++m_refused_too_large;
                ++m_opened;
                continue;
            }
            if (error == Http2Error::GoingAway && m_options.goaway == HTTPX_GOAWAY_REJECT)
            {
                ++m_rejected;
                ++m_opened;
                continue;
            }
            if (error != Http2Error::None)
            {
                m_unverified = m_unverified || (error != Http2Error::NotReady && error != Http2Error::NoStream &&
                                                error != Http2Error::Blocked);
                return;
            }
            m_unverified = m_unverified || id != (index * 2U) + 1U;
            m_streams[index].used = true;
            m_streams[index].out_end = fin;
            ++m_opened;
        }
    }

    void client_goaway(Http2Connection& h2) noexcept
    {
        if (m_options.goaway == HTTPX_GOAWAY_CLIENT && m_opened == m_options.requests && !m_goaway_sent)
        {
            m_goaway_sent = h2.goaway(true);
        }
        if (m_options.goaway == HTTPX_GOAWAY_NOTICE && m_goaway_seen >= 2U && m_refused_after_goaway == 0U)
        {
            Fields fields;
            fields.add_text(":method", "GET");
            fields.add_text(":scheme", "http");
            fields.add_text(":authority", HTTPX_SERVER_NAME);
            fields.add_text(":path", "/cerid/probe");
            u32 id = 0U;
            m_refused_after_goaway = h2.open_request(fields.span(), true, id) == Http2Error::GoingAway ? 1U : 0U;
        }
    }

    void pump(Http2Connection& h2, u64 index) noexcept
    {
        Stream& stream = m_streams[index];
        const u32 id = static_cast<u32>((index * 2U) + 1U);
        if (!stream.used || stream.counted || stream.out_end)
        {
            return;
        }
        if (!m_client && (!stream.answer_due || !send_head(h2, index, id, stream)))
        {
            return;
        }
        const bool tunnel = httpx_is_tunnel(&m_options, index) != 0;
        if (tunnel && (m_client ? !stream.tunnel_open : !stream.ended))
        {
            return;
        }
        if (send_body(h2, index, id, stream))
        {
            finish(h2, index, id, stream);
        }
    }

    [[nodiscard]] bool send_head(Http2Connection& h2, u64 index, u32 id, Stream& stream) noexcept
    {
        if (stream.head_sent)
        {
            return true;
        }
        if (m_options.interim != 0 && !stream.interim_sent)
        {
            Fields interim;
            interim.add_text(":status", "103");
            interim.add_text("link", "</cerid.css>; rel=preload");
            if (h2.send_response(id, interim.span(), false) != Http2Error::None)
            {
                return false;
            }
            stream.interim_sent = true;
        }
        const u64 body = httpx_body_bytes(&m_options, index, 1);
        const bool tunnel = httpx_is_tunnel(&m_options, index) != 0;
        char text[kNumberText];
        char length[kNumberText];
        std::snprintf(text, sizeof(text), "%llu", ull(index));
        std::snprintf(length, sizeof(length), "%llu", ull(body));
        Fields fields;
        fields.add_text(":status", "200");
        common_fields(fields, index, text);
        if (httpx_has_length(&m_options, index, body) != 0)
        {
            fields.add_text("content-length", length);
        }
        const bool fin = body == 0U && httpx_has_trailers(&m_options, index) == 0 && !tunnel;
        const Http2Error error = h2.send_response(id, fields.span(), fin);
        if (error != Http2Error::None)
        {
            m_unverified = m_unverified || error != Http2Error::Blocked;
            return false;
        }
        stream.head_sent = true;
        stream.out_end = fin;
        ++m_completed;
        return !fin;
    }

    // Writes what fits; true when the whole body has gone and the message has not been ended by it.
    [[nodiscard]] bool send_body(Http2Connection& h2, u64 index, u32 id, Stream& stream) noexcept
    {
        const u64 goal = httpx_body_bytes(&m_options, index, !m_client ? 1 : 0);
        const u64 tag = m_client ? 0U : 1U;
        const bool trailers = httpx_has_trailers(&m_options, index) != 0;
        u8 chunk[HTTPX_DATA_FRAME_BYTES];
        while (stream.sent < goal)
        {
            const u64 left = goal - stream.sent;
            const usize want = left < sizeof(chunk) ? static_cast<usize>(left) : sizeof(chunk);
            for (usize at = 0U; at < want; ++at)
            {
                chunk[at] = httpx_pattern(tag, id, stream.sent + at);
            }
            const bool last = want == left && !trailers;
            usize taken = 0U;
            const Http2Error error = h2.send_body(id, ConstSpan<u8>(chunk, want), last, taken);
            stream.sent += taken;
            m_body_sent += taken;
            if (error != Http2Error::None)
            {
                m_unverified = m_unverified || error != Http2Error::Blocked;
                return false;
            }
            if (taken == want && last)
            {
                stream.out_end = true;
                return false;
            }
        }
        return true;
    }

    void finish(Http2Connection& h2, u64 index, u32 id, Stream& stream) noexcept
    {
        if (httpx_has_trailers(&m_options, index) == 0)
        {
            stream.out_end = h2.finish(id) == Http2Error::None;
            return;
        }
        char text[kNumberText];
        std::snprintf(text, sizeof(text), "%llu", ull(index));
        Fields fields;
        fields.add_text("x-cerid-trailer", text);
        fields.add_text("x-cerid-repeat", HTTPX_REPEAT_VALUE);
        const Http2Error error = h2.send_trailers(id, fields.span());
        m_unverified = m_unverified || (error != Http2Error::None && error != Http2Error::Blocked);
        stream.out_end = error == Http2Error::None;
    }

    httpx_options_t m_options;
    bool m_client;
    Stream m_streams[HTTPX_MAX_REQUESTS];
    u64 m_opened = 0U;
    u64 m_seen = 0U;
    u64 m_request_ended = 0U;
    u64 m_completed = 0U;
    u64 m_rejected = 0U;
    u64 m_aborted = 0U;
    u64 m_abort_code = 0U;
    u64 m_cancelled = 0U;
    u64 m_status_431 = 0U;
    u64 m_refused_too_large = 0U;
    u64 m_refused_after_goaway = 0U;
    u64 m_goaway_seen = 0U;
    u64 m_goaway_id = 0U;
    u64 m_body_received = 0U;
    u64 m_body_sent = 0U;
    u64 m_interims = 0U;
    u64 m_trailers = 0U;
    u64 m_tunnels = 0U;
    bool m_unverified = false;
    bool m_goaway_sent = false;
};

// One connection over one socket.
class Link
{
public:
    [[nodiscard]] bool open(StreamSocket* socket, const Http2Config& config, Http2App* app, u64 now_ns) noexcept
    {
        m_socket = socket;
        m_app = app;
        m_used = 0U;
        m_ended = false;
        m_failed = false;
        m_h2.~Http2Connection();
        new (static_cast<void*>(&m_h2)) Http2Connection();
        return m_h2.init(*memory::default_allocator(), config, now_ns);
    }

    [[nodiscard]] bool in_use() const noexcept { return m_socket != nullptr; }
    void release() noexcept { m_socket = nullptr; }
    [[nodiscard]] Http2Connection& connection() noexcept { return m_h2; }
    [[nodiscard]] bool wants_write() const noexcept { return !m_h2.output().empty(); }

    // Runs the connection until nothing moves; false when it is over and its socket can be closed.
    [[nodiscard]] bool turn(u64 now_ns) noexcept
    {
        bool moved = true;
        while (moved && !m_failed)
        {
            moved = read_some();
            moved = drain(now_ns) || moved;
            if (m_h2.error() == 0U)
            {
                const usize before = m_h2.output().size();
                m_app->service(m_h2);
                moved = m_h2.output().size() != before || moved;
            }
            moved = flush() || moved;
        }
        return !m_failed && !m_ended && (m_h2.error() == 0U || wants_write());
    }

private:
    [[nodiscard]] bool read_some() noexcept
    {
        if (m_used == sizeof(m_input) || m_ended)
        {
            return false;
        }
        const StreamResult result = m_socket->read(Span<u8>(m_input + m_used, sizeof(m_input) - m_used));
        m_used += result.bytes;
        m_ended = result.ended;
        m_failed = result.error != SocketError::None && result.error != SocketError::Busy;
        return result.bytes != 0U;
    }

    [[nodiscard]] bool drain(u64 now_ns) noexcept
    {
        usize at = 0U;
        bool moved = false;
        for (;;)
        {
            usize taken = 0U;
            Http2Event event;
            const bool more = m_h2.poll(ConstSpan<u8>(m_input + at, m_used - at), now_ns, taken, event);
            at += taken;
            moved = moved || taken != 0U;
            if (!more)
            {
                break;
            }
            moved = true;
            m_app->on_event(m_h2, event);
        }
        std::memmove(m_input, m_input + at, m_used - at);
        m_used -= at;
        return moved;
    }

    [[nodiscard]] bool flush() noexcept
    {
        const ConstSpan<u8> out = m_h2.output();
        if (out.empty())
        {
            return false;
        }
        const StreamResult result = m_socket->write(out);
        m_h2.output_taken(result.bytes);
        m_failed = result.error != SocketError::None && result.error != SocketError::Busy;
        return result.bytes != 0U;
    }

    StreamSocket* m_socket = nullptr;
    Http2App* m_app = nullptr;
    Http2Connection m_h2;
    usize m_used = 0U;
    bool m_ended = false;
    bool m_failed = false;
    u8 m_input[kInputBytes];
};

Http2Config connection_config(const Options& options) noexcept
{
    Http2Config config;
    config.side = options.client ? Http2Side::Client : Http2Side::Server;
    config.field_bytes = static_cast<u32>(options.exchange.field_limit);
    config.streams = options.max_streams;
    config.stream_window = options.window;
    config.connection_window = options.connection_window;
    config.frame_bytes = options.frame_bytes;
    config.table_bytes = static_cast<u32>(options.exchange.table);
    config.encoder_table_bytes = static_cast<u32>(options.exchange.table);
    config.output_bytes = 256U * 1024U;
    config.connect_protocol = !options.client;
    return config;
}

void set_interest(StreamPoller& poller, u32 slot, const Link& link) noexcept
{
    poller.interest(slot, static_cast<u8>(kStreamReadable | (link.wants_write() ? kStreamWritable : 0U)));
}

// Many connections, each answered by a Responder, until the process is killed.
int serve_h2spec(TcpPoller& poller, const Http2Config& config, const HostClock& clock)
{
    static Link links[kSlots];
    static Responder responders[kSlots];
    for (;;)
    {
        StreamReady ready[kSlots + 1U];
        const usize count = poller.wait(Span<StreamReady>(ready, kSlots + 1U), kWaitNs);
        const u64 now_ns = clock.monotonic_ns();
        for (usize at = 0U; at < count; ++at)
        {
            if (ready[at].token != kStreamListener)
            {
                continue;
            }
            for (u32 slot = 0U; slot < kSlots; ++slot)
            {
                if (links[slot].in_use())
                {
                    continue;
                }
                SocketError error = SocketError::None;
                StreamSocket* const socket = poller.accept(slot, error);
                if (socket != nullptr)
                {
                    responders[slot].reset();
                    if (!links[slot].open(socket, config, &responders[slot], now_ns))
                    {
                        return 1;
                    }
                }
                break;
            }
        }
        for (u32 slot = 0U; slot < kSlots; ++slot)
        {
            if (!links[slot].in_use())
            {
                continue;
            }
            if (links[slot].turn(now_ns))
            {
                set_interest(poller, slot, links[slot]);
            }
            else
            {
                poller.close(slot, false);
                links[slot].release();
            }
        }
    }
}

// One connection of the exchange: the report goes out when it has ended.
int run_exchange(TcpPoller& poller, const Options& options, const Http2Config& config, const HostClock& clock)
{
    static Link link;
    static Exchange exchange(options.exchange, options.client);
    const u64 deadline = clock.monotonic_ns() + kRunNs;
    bool connected = !options.client;
    bool closing = false;
    if (options.client)
    {
        SocketError error = SocketError::None;
        StreamSocket* const socket =
            poller.connect(0U, Endpoint{IpAddress::v4_loopback(), static_cast<u16>(options.port)}, error);
        if (socket == nullptr || !link.open(socket, config, &exchange, clock.monotonic_ns()))
        {
            std::fputs("crd-http2-peer: the connection could not be started\n", stderr);
            return 1;
        }
    }
    while (clock.monotonic_ns() < deadline)
    {
        StreamReady ready[2];
        const usize count = poller.wait(Span<StreamReady>(ready, 2U), kWaitNs);
        const u64 now_ns = clock.monotonic_ns();
        for (usize at = 0U; at < count; ++at)
        {
            if (ready[at].token == kStreamListener && !link.in_use())
            {
                SocketError error = SocketError::None;
                StreamSocket* const socket = poller.accept(0U, error);
                if (socket != nullptr && !link.open(socket, config, &exchange, now_ns))
                {
                    return 1;
                }
            }
        }
        if (!link.in_use())
        {
            continue;
        }
        if (!connected)
        {
            const SocketError error = poller.connected(0U);
            if (error == SocketError::Busy)
            {
                continue;
            }
            if (error != SocketError::None)
            {
                std::fputs("crd-http2-peer: the connection failed\n", stderr);
                return 1;
            }
            connected = true;
        }
        const bool alive = link.turn(now_ns);
        if (options.client && !closing && exchange.complete(link.connection()))
        {
            closing = link.connection().goaway(true);
        }
        if (!alive || (closing && !link.wants_write()))
        {
            exchange.report(link.connection());
            poller.close(0U, false);
            return link.connection().error() == 0U ? 0 : 1;
        }
        set_interest(poller, 0U, link);
    }
    std::fputs("crd-http2-peer: the run outlasted its deadline\n", stderr);
    return 1;
}

int run(memory::IAllocator& allocator, const Options& options)
{
    TcpListener listener;
    if (!options.client)
    {
        TcpListenOptions listen;
        listen.bind = Endpoint{IpAddress::v4_loopback(), static_cast<u16>(options.port)};
        SocketError error = SocketError::None;
        listener = TcpListener::open(listen, error);
        if (error != SocketError::None)
        {
            std::fputs("crd-http2-peer: the socket could not be bound\n", stderr);
            return 1;
        }
    }
    const HostClock clock;
    TcpPoller poller(allocator, kSlots, options.client ? nullptr : &listener);
    if (!poller.ready())
    {
        std::fputs("crd-http2-peer: the poller could not be made\n", stderr);
        return 1;
    }
    if (!options.client)
    {
        std::printf("port=%u\n", static_cast<unsigned>(listener.local_endpoint().port));
        std::fflush(stdout);
    }
    const Http2Config config = connection_config(options);
    return options.h2spec ? serve_h2spec(poller, config, clock) : run_exchange(poller, options, config, clock);
}

int usage()
{
    std::fputs("usage: crd-http2-peer --role client|server --port N [--h2spec 1] [--max-streams N] [--window N] "
               "[--connection-window N] [--frame-bytes N] and the options of http_exchange.h\n",
               stderr);
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    Options options;
    httpx_default_options(&options.exchange);
    for (int at = 1; at + 1 < argc; at += 2)
    {
        const char* const name = argv[at];
        const char* const value = argv[at + 1];
        const u64 number = std::strtoull(value, nullptr, 10);
        if (std::strcmp(name, "--role") == 0)
        {
            options.client = std::strcmp(value, "client") == 0;
            options.role_given = true;
        }
        else if (std::strcmp(name, "--port") == 0)
        {
            options.port = number;
            options.port_given = true;
        }
        else if (std::strcmp(name, "--h2spec") == 0)
        {
            options.h2spec = number != 0U;
        }
        else if (std::strcmp(name, "--max-streams") == 0)
        {
            options.max_streams = static_cast<u32>(number);
        }
        else if (std::strcmp(name, "--window") == 0)
        {
            options.window = static_cast<u32>(number);
        }
        else if (std::strcmp(name, "--connection-window") == 0)
        {
            options.connection_window = static_cast<u32>(number);
        }
        else if (std::strcmp(name, "--frame-bytes") == 0)
        {
            options.frame_bytes = static_cast<u32>(number);
        }
        else if (httpx_parse_option(&options.exchange, name, value) == 0)
        {
            return usage();
        }
    }
    if (argc % 2 == 0 || !options.role_given || !options.port_given || httpx_options_valid(&options.exchange) == 0 ||
        options.frame_bytes > (1U << 20U))
    {
        return usage();
    }
    return run(*memory::default_allocator(), options);
}
