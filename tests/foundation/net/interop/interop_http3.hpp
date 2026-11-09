#pragma once

// crd-net interoperability test (4.2a) -- the HTTP/3 application crd-quic-peer runs over Http3Connection with --http3 1.
// The exchange and its options: http_exchange.h, which ngtcp2_http3.c runs over nghttp3.

#include "http_exchange.h"
#include "interop_echo.hpp"

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/http3.hpp>
#include <crd/net/quic_connection.hpp>
#include <crd/net/ticket_cache.hpp>

#include <cstdio>
#include <cstring>
#include <new>

namespace crd_net_interop
{
inline constexpr usize kHttp3MaxFields = 12U;
inline constexpr usize kHttp3IndexText = 24U;
inline constexpr u32 kHttp3DatagramBytes = 1400U;
inline constexpr usize kMaxVarintPrefix = 8U;

// What the exchange and one connection's run share.
struct Http3Run
{
    const httpx_options_t* options = nullptr;
    bool client = false;
    u64 datagrams = 0U;
    u64 datagram_bytes = 0U;
    const crd::net::Http3Settings* remembered = nullptr; // a resuming client's
    const crd::net::TicketCache* tickets = nullptr;      // a client's, when a later connection resumes on a ticket
};

inline crd::net::Http3Config http3_config(const httpx_options_t& options, bool client, u64 datagrams)
{
    crd::net::Http3Config config;
    config.side = client ? crd::crypto::QuicSide::Client : crd::crypto::QuicSide::Server;
    config.field_bytes = static_cast<u32>(options.field_limit);
    config.decoder.table_capacity = static_cast<u32>(options.table);
    config.decoder.blocked_streams = static_cast<u32>(options.blocked);
    config.encoder.table_capacity = static_cast<u32>(options.table);
    config.datagram_bytes = kHttp3DatagramBytes;
    config.connect_protocol = !client;
    config.datagrams = datagrams != 0U;
    config.grease = 0x0123456789ABCDEFULL;
    return config;
}

[[nodiscard]] inline ConstSpan<u8> text_bytes(const char* text, usize size) noexcept
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(text), size);
}

// The field lines of one call; they view the text given.
class Http3Fields
{
public:
    void add(const char* name, const char* value, usize size) noexcept
    {
        m_items[m_count].name = text_bytes(name, std::strlen(name));
        m_items[m_count].value = text_bytes(value, size);
        ++m_count;
    }

    void add_text(const char* name, const char* value) noexcept { add(name, value, std::strlen(value)); }

    [[nodiscard]] ConstSpan<crd::net::HttpField> span() const noexcept
    {
        return ConstSpan<crd::net::HttpField>(m_items, m_count);
    }

private:
    crd::net::HttpField m_items[kHttp3MaxFields];
    usize m_count = 0U;
};

// One connection's exchange, either end's.
class Http3Exchange
{
public:
    Http3Exchange() noexcept = default;
    Http3Exchange(const Http3Exchange&) = delete;
    Http3Exchange& operator=(const Http3Exchange&) = delete;
    Http3Exchange(Http3Exchange&&) = delete;
    Http3Exchange& operator=(Http3Exchange&&) = delete;
    ~Http3Exchange() = default;

    // The error to close the connection with, kNone to go on; `done` when this end should close it for having finished.
    [[nodiscard]] u64 service(crd::net::QuicConnection& connection, const Http3Run& run, const EchoTurn& turn,
                              bool& done) noexcept
    {
        m_run = run;
        m_now_ns = turn.now_ns;
        const crd::crypto::TlsEarlyData early = turn.early_data;
        if (m_run.client && early == crd::crypto::TlsEarlyData::Rejected && !m_forgot_early)
        {
            m_forgot_early = true;
            drop();
            forget();
        }
        if (m_h3 == nullptr && !make(connection))
        {
            m_unverified = true;
            return crd::net::kH3InternalError;
        }
        const bool may_start = turn.established || early == crd::crypto::TlsEarlyData::Offered ||
                               early == crd::crypto::TlsEarlyData::Accepted;
        if (!m_h3->started() && (!may_start || !m_h3->start()))
        {
            return m_h3->error();
        }
        crd::net::Http3Event event;
        while (m_h3->poll(event))
        {
            handle(event);
        }
        if (m_h3->error() != 0U)
        {
            return m_h3->error();
        }
        if (m_h3->settings_known() || early == crd::crypto::TlsEarlyData::Offered)
        {
            open_requests();
        }
        send_goaway();
        for (u64 index = 0U; index < options().requests; ++index)
        {
            pump(index);
        }
        service_datagrams();
        done = m_run.client && lingered(complete(connection.streams()));
        return m_h3->error();
    }

    // What the peer's SETTINGS said, for a later connection resuming on this one's ticket; null before they came.
    [[nodiscard]] const crd::net::Http3Settings* peer_settings() const noexcept
    {
        return m_settings_known ? &m_settings : nullptr;
    }

    // The connection has ended: its report, and the HTTP/3 connection goes before the streams it reads.
    void report() noexcept
    {
        std::printf("h3_requests=%llu\nh3_seen=%llu\nh3_completed=%llu\nh3_rejected=%llu\nh3_aborted=%llu\n"
                    "h3_abort_code=%llu\n",
                    ull(m_opened), ull(m_seen), ull(m_completed), ull(m_rejected), ull(m_aborted), ull(m_abort_code));
        std::printf("h3_cancelled=%llu\nh3_status_431=%llu\nh3_refused_too_large=%llu\nh3_refused_after_goaway=%llu\n"
                    "h3_goaway_seen=%llu\nh3_goaway_id=%llu\n",
                    ull(m_cancelled), ull(m_status_431), ull(m_refused_too_large), ull(m_refused_after_goaway),
                    ull(m_goaway_seen), ull(m_goaway_id));
        std::printf("h3_body_received=%llu\nh3_body_sent=%llu\nh3_interims=%llu\nh3_trailers=%llu\nh3_tunnels=%llu\n"
                    "h3_early_requests=%llu\nh3_verified=%d\n",
                    ull(m_body_received), ull(m_body_sent), ull(m_interims), ull(m_trailers), ull(m_tunnels),
                    ull(m_early_requests), m_unverified ? 0 : 1);
        if (m_h3 == nullptr)
        {
            return;
        }
        const crd::net::Http3Settings& peer = m_h3->peer_settings();
        m_settings = peer;
        m_settings_known = m_h3->settings_known();
        std::printf("h3_peer_known=%d\nh3_peer_table=%llu\nh3_peer_blocked=%llu\nh3_peer_field_limit=%llu\n"
                    "h3_peer_connect=%d\nh3_peer_datagram=%d\n",
                    m_h3->settings_known() ? 1 : 0, ull(peer.qpack_max_table_capacity), ull(peer.qpack_blocked_streams),
                    ull(peer.max_field_section_size), peer.enable_connect_protocol ? 1 : 0, peer.h3_datagram ? 1 : 0);
        std::printf("h3_qpack_inserts_received=%llu\nh3_qpack_inserts_sent=%llu\nh3_qpack_acknowledged=%llu\n"
                    "h3_datagrams_sent=%llu\nh3_datagrams_received=%llu\nh3_datagrams_verified=%d\n",
                    ull(m_h3->qpack_decoder().table().inserted()), ull(m_h3->qpack_encoder().table().inserted()),
                    ull(m_h3->qpack_encoder().known_received()), ull(m_datagrams_sent), ull(m_datagrams_received),
                    m_datagrams_unverified ? 0 : 1);
        drop();
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
        bool answer_due = false; // a server's: the response may go
        bool interim_sent = false;
        bool head_sent = false;
        bool out_end = false;
        bool counted = false;
        bool trailers_seen = false;
        bool stopped = false;
    };

    [[nodiscard]] const httpx_options_t& options() const noexcept { return *m_run.options; }
    [[nodiscard]] static unsigned long long ull(u64 value) noexcept { return value; }
    [[nodiscard]] static u64 index_of(u64 id) noexcept { return id / 4U; }
    [[nodiscard]] u64 per_tunnel() const noexcept
    {
        const u64 count = options().tunnels != 0U ? m_run.datagrams / options().tunnels : 1U;
        return count != 0U ? count : 1U;
    }

    void drop() noexcept
    {
        if (m_h3 != nullptr)
        {
            m_h3->~Http3Connection();
            m_h3 = nullptr;
        }
    }

    // After a refused 0-RTT everything that was written goes again, on a new HTTP/3 connection.
    void forget() noexcept
    {
        for (Stream& stream : m_streams)
        {
            stream = Stream{};
        }
        m_opened = 0U;
        m_seen = 0U;
        m_completed = 0U;
        m_body_received = 0U;
        m_body_sent = 0U;
        m_datagrams_sent = 0U;
        m_datagrams_received = 0U;
        std::memset(m_datagrams_seen, 0, sizeof(m_datagrams_seen));
        m_goaway_sent = false;
    }

    [[nodiscard]] bool make(crd::net::QuicConnection& connection) noexcept
    {
        crd::net::Http3Config config = http3_config(options(), m_run.client, m_run.datagrams);
        config.remembered = m_run.remembered;
        m_h3 = new (m_storage) crd::net::Http3Connection();
        if (!m_h3->init(*crd::memory::default_allocator(), connection.streams(), &connection.datagrams(), config))
        {
            drop();
            return false;
        }
        return true;
    }

    void handle(const crd::net::Http3Event& event) noexcept
    {
        const u64 index = index_of(event.stream);
        Stream& stream = m_streams[index < HTTPX_MAX_REQUESTS ? index : 0U];
        switch (event.kind)
        {
            case crd::net::Http3EventKind::Settings:
            case crd::net::Http3EventKind::Writable:
            case crd::net::Http3EventKind::WtStream: // these programs offer no WebTransport
            case crd::net::Http3EventKind::WtReadable:
            case crd::net::Http3EventKind::WtWritable:
            case crd::net::Http3EventKind::WtAborted:
            case crd::net::Http3EventKind::WtStopped:
                break;
            case crd::net::Http3EventKind::GoAway:
                on_goaway(event.id);
                break;
            case crd::net::Http3EventKind::Headers:
                on_headers(event, index, stream);
                break;
            case crd::net::Http3EventKind::Interim:
                ++m_interims;
                break;
            case crd::net::Http3EventKind::Body:
                on_body(event.stream, index, stream);
                break;
            case crd::net::Http3EventKind::Trailers:
                on_trailers(event, index, stream);
                break;
            case crd::net::Http3EventKind::Finished:
                on_finished(index, stream);
                break;
            case crd::net::Http3EventKind::Aborted:
                on_aborted(event.error, stream);
                break;
            case crd::net::Http3EventKind::Stopped:
                stream.stopped = true;
                if (event.error != crd::net::kH3NoError)
                {
                    on_aborted(event.error, stream);
                }
                break;
            case crd::net::Http3EventKind::Datagram:
                on_datagram(event);
                break;
        }
    }

    void take_section(const crd::net::Http3Event& event, u64 index, Stream& stream) const noexcept
    {
        stream.section = httpx_section_t{};
        for (const crd::net::HttpField& field : event.fields)
        {
            httpx_take_field(&stream.section, &options(), index, field.name.data(), field.name.size(),
                             field.value.data(), field.value.size());
        }
    }

    void on_headers(const crd::net::Http3Event& event, u64 index, Stream& stream) noexcept
    {
        take_section(event, index, stream);
        if (m_run.client)
        {
            if (httpx_is_tunnel(&options(), index) && stream.section.status == 200U)
            {
                stream.tunnel_open = true;
                ++m_tunnels;
            }
            return;
        }
        stream.used = true;
        m_unverified = m_unverified || httpx_section_good(&stream.section, &options(), index, 0) == 0;
        ++m_seen;
        m_early_requests += event.early ? 1U : 0U;
        if (m_seen == 1U)
        {
            first_request();
        }
        if (options().cancel == HTTPX_CANCEL_SERVER && httpx_last_request(&options(), index) != 0)
        {
            cancel(event.stream, stream);
            return;
        }
        if (httpx_is_tunnel(&options(), index) != 0)
        {
            ++m_tunnels;
            stream.answer_due = true;
        }
    }

    void first_request() noexcept
    {
        if (options().goaway == HTTPX_GOAWAY_NOTICE)
        {
            m_unverified = m_unverified || !m_h3->goaway(false);
        }
        else if (options().goaway == HTTPX_GOAWAY_REJECT)
        {
            m_unverified = m_unverified || !m_h3->goaway(true);
        }
    }

    void cancel(u64 id, Stream& stream) noexcept
    {
        if (!stream.counted && m_h3->cancel(id, HTTP3_CANCEL_CODE) == crd::net::Http3Error::None)
        {
            stream.counted = true;
            stream.out_end = true;
            ++m_cancelled;
            m_cancelled_id = id;
        }
    }

    void on_body(u64 id, u64 index, Stream& stream) noexcept
    {
        const u64 tag = m_run.client ? 1U : 0U;
        for (;;)
        {
            const crd::net::Http3Body body = m_h3->peek_body(id);
            if (body.data.empty())
            {
                return;
            }
            for (usize at = 0U; at < body.data.size(); ++at)
            {
                m_unverified = m_unverified || body.data[at] != pattern(tag, id, stream.received + at);
            }
            stream.received += body.data.size();
            m_body_received += body.data.size();
            m_unverified = m_unverified || m_h3->consume_body(id, body.data.size()) != crd::net::Http3Error::None;
            if (m_run.client && options().cancel == HTTPX_CANCEL_CLIENT &&
                httpx_last_request(&options(), index) != 0)
            {
                cancel(id, stream);
                return;
            }
        }
    }

    void on_trailers(const crd::net::Http3Event& event, u64 index, Stream& stream) noexcept
    {
        for (const crd::net::HttpField& field : event.fields)
        {
            if (httpx_text_is(field.name.data(), field.name.size(), "x-cerid-trailer") != 0)
            {
                stream.trailers_seen = httpx_decimal(field.value.data(), field.value.size()) == index;
            }
        }
    }

    void on_finished(u64 index, Stream& stream) noexcept
    {
        stream.ended = true;
        if (m_run.client)
        {
            response_ended(index, stream);
            return;
        }
        const bool trailers = httpx_has_trailers(&options(), index) != 0 && !stream.trailers_seen;
        m_unverified = m_unverified || stream.received != httpx_body_bytes(&options(), index, 0) || trailers;
        m_trailers += stream.trailers_seen ? 1U : 0U;
        ++m_request_ended;
        if (options().goaway == HTTPX_GOAWAY_NOTICE && m_request_ended == options().requests)
        {
            m_unverified = m_unverified || !m_h3->goaway(true);
        }
        if (httpx_is_tunnel(&options(), index) == 0)
        {
            stream.answer_due = options().goaway != HTTPX_GOAWAY_CLIENT || m_goaway_seen >= 2U;
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
        if (options().over_limit != 0 && httpx_last_request(&options(), index) != 0 && stream.section.status == 431U)
        {
            ++m_status_431;
            return;
        }
        const bool trailers = httpx_has_trailers(&options(), index) != 0 && !stream.trailers_seen;
        m_unverified = m_unverified || httpx_section_good(&stream.section, &options(), index, 1) == 0 ||
                       stream.received != httpx_body_bytes(&options(), index, 1) || trailers;
        m_trailers += stream.trailers_seen ? 1U : 0U;
    }

    void on_aborted(u64 error, Stream& stream) noexcept
    {
        if (stream.counted)
        {
            return;
        }
        stream.counted = true;
        if (error == HTTP3_REJECTED_CODE)
        {
            ++m_rejected;
        }
        else
        {
            ++m_aborted;
            m_abort_code = error;
        }
    }

    void on_goaway(u64 id) noexcept
    {
        ++m_goaway_seen;
        m_goaway_id = id;
        if (!m_run.client && options().goaway == HTTPX_GOAWAY_CLIENT && m_goaway_seen == 2U)
        {
            for (u64 index = 0U; index < options().requests; ++index)
            {
                m_streams[index].answer_due = m_streams[index].ended && httpx_is_tunnel(&options(), index) == 0;
            }
        }
    }

    void common_fields(Http3Fields& fields, u64 index, const char* text) const noexcept
    {
        const u64 fill = httpx_fill_bytes(&options(), index);
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

    void open_requests() noexcept
    {
        while (m_run.client && m_opened < options().requests)
        {
            const u64 index = m_opened;
            const u64 body = httpx_body_bytes(&options(), index, 0);
            const bool tunnel = httpx_is_tunnel(&options(), index) != 0;
            char text[kHttp3IndexText];
            char length[kHttp3IndexText];
            char path[kHttp3IndexText + 8U];
            std::snprintf(text, sizeof(text), "%llu", ull(index));
            std::snprintf(length, sizeof(length), "%llu", ull(body));
            std::snprintf(path, sizeof(path), tunnel ? "/tunnel/%llu" : "/cerid/%llu", ull(index));
            Http3Fields fields;
            fields.add_text(":method", httpx_method(tunnel, body));
            fields.add_text(":scheme", "https");
            fields.add_text(":authority", HTTPX_SERVER_NAME);
            fields.add_text(":path", path);
            if (tunnel)
            {
                fields.add_text(":protocol", HTTPX_TUNNEL_PROTOCOL);
            }
            common_fields(fields, index, text);
            if (httpx_has_length(&options(), index, body) != 0 && body != 0U)
            {
                fields.add_text("content-length", length);
            }
            const bool fin = body == 0U && httpx_has_trailers(&options(), index) == 0 && !tunnel;
            u64 id = 0U;
            const crd::net::Http3Error error = m_h3->open_request(fields.span(), fin, id);
            if (error == crd::net::Http3Error::TooLarge && options().over_limit != 0 &&
                httpx_last_request(&options(), index) != 0)
            {
                ++m_refused_too_large;
                ++m_opened;
                continue;
            }
            if (error != crd::net::Http3Error::None)
            {
                m_unverified = m_unverified || (error != crd::net::Http3Error::NotReady &&
                                                error != crd::net::Http3Error::NoStream &&
                                                error != crd::net::Http3Error::Blocked);
                return;
            }
            m_unverified = m_unverified || id != index * 4U;
            m_streams[index].used = true;
            m_streams[index].out_end = fin;
            ++m_opened;
        }
    }

    void send_goaway() noexcept
    {
        if (m_run.client && options().goaway == HTTPX_GOAWAY_CLIENT && m_opened == options().requests && !m_goaway_sent)
        {
            m_goaway_sent = m_h3->goaway(false) && m_h3->goaway(true);
        }
        if (m_run.client && options().goaway == HTTPX_GOAWAY_NOTICE && m_goaway_seen >= 2U &&
            m_refused_after_goaway == 0U)
        {
            refuse_probe();
        }
    }

    // A new request after a GOAWAY is refused by this end, before anything is written.
    void refuse_probe() noexcept
    {
        Http3Fields fields;
        fields.add_text(":method", "GET");
        fields.add_text(":scheme", "https");
        fields.add_text(":authority", HTTPX_SERVER_NAME);
        fields.add_text(":path", "/cerid/probe");
        u64 id = 0U;
        m_refused_after_goaway = m_h3->open_request(fields.span(), true, id) == crd::net::Http3Error::GoingAway ? 1U : 0U;
    }

    void pump(u64 index) noexcept
    {
        Stream& stream = m_streams[index];
        const u64 id = index * 4U;
        if (!stream.used || stream.stopped || stream.counted || stream.out_end)
        {
            return;
        }
        if (!m_run.client && (!stream.answer_due || !send_head(index, id, stream)))
        {
            return;
        }
        // A tunnel's FIN waits for its HTTP datagrams, which need the send part open.
        const bool tunnel = httpx_is_tunnel(&options(), index) != 0;
        if (tunnel && (m_run.client ? !stream.tunnel_open || !datagrams_settled() : !stream.ended || m_echo_count != 0U))
        {
            return;
        }
        if (send_body(index, id, stream))
        {
            finish(index, id, stream);
        }
    }

    [[nodiscard]] bool send_head(u64 index, u64 id, Stream& stream) noexcept
    {
        if (stream.head_sent)
        {
            return true;
        }
        if (options().interim != 0 && !stream.interim_sent)
        {
            Http3Fields interim;
            interim.add_text(":status", "103");
            interim.add_text("link", "</cerid.css>; rel=preload");
            if (m_h3->send_response(id, interim.span(), false) != crd::net::Http3Error::None)
            {
                return false;
            }
            stream.interim_sent = true;
        }
        const u64 body = httpx_body_bytes(&options(), index, 1);
        const bool tunnel = httpx_is_tunnel(&options(), index) != 0;
        char text[kHttp3IndexText];
        char length[kHttp3IndexText];
        std::snprintf(text, sizeof(text), "%llu", ull(index));
        std::snprintf(length, sizeof(length), "%llu", ull(body));
        Http3Fields fields;
        fields.add_text(":status", "200");
        common_fields(fields, index, text);
        if (httpx_has_length(&options(), index, body) != 0)
        {
            fields.add_text("content-length", length);
        }
        const bool fin = body == 0U && httpx_has_trailers(&options(), index) == 0 && !tunnel;
        const crd::net::Http3Error error = m_h3->send_response(id, fields.span(), fin);
        if (error != crd::net::Http3Error::None)
        {
            m_unverified = m_unverified || error != crd::net::Http3Error::Blocked;
            return false;
        }
        stream.head_sent = true;
        stream.out_end = fin;
        ++m_completed;
        return !fin;
    }

    // Writes what fits; true when the whole body has gone and the message has not been ended by it.
    [[nodiscard]] bool send_body(u64 index, u64 id, Stream& stream) noexcept
    {
        const u64 goal = httpx_body_bytes(&options(), index, !m_run.client ? 1 : 0);
        const u64 tag = m_run.client ? 0U : 1U;
        const bool trailers = httpx_has_trailers(&options(), index) != 0;
        u8 chunk[HTTPX_DATA_FRAME_BYTES];
        while (stream.sent < goal)
        {
            const u64 left = goal - stream.sent;
            const usize want = left < sizeof(chunk) ? static_cast<usize>(left) : sizeof(chunk);
            for (usize at = 0U; at < want; ++at)
            {
                chunk[at] = pattern(tag, id, stream.sent + at);
            }
            const bool last = want == left && !trailers;
            usize taken = 0U;
            const crd::net::Http3Error error = m_h3->send_body(id, ConstSpan<u8>(chunk, want), last, taken);
            if (error != crd::net::Http3Error::None)
            {
                m_unverified = m_unverified || error != crd::net::Http3Error::Blocked;
                return false;
            }
            stream.sent += taken;
            m_body_sent += taken;
            if (taken < want)
            {
                return false;
            }
            if (last)
            {
                stream.out_end = true;
                return false;
            }
        }
        return true;
    }

    // The end of a message whose body has gone: trailers, or a FIN no body byte could carry.
    void finish(u64 index, u64 id, Stream& stream) noexcept
    {
        if (httpx_has_trailers(&options(), index) == 0)
        {
            stream.out_end = m_h3->finish(id) == crd::net::Http3Error::None;
            return;
        }
        char text[kHttp3IndexText];
        std::snprintf(text, sizeof(text), "%llu", ull(index));
        Http3Fields fields;
        fields.add_text("x-cerid-trailer", text);
        fields.add_text("x-cerid-repeat", HTTPX_REPEAT_VALUE);
        const crd::net::Http3Error error = m_h3->send_trailers(id, fields.span());
        m_unverified = m_unverified || (error != crd::net::Http3Error::None && error != crd::net::Http3Error::Blocked);
        stream.out_end = error == crd::net::Http3Error::None;
    }

    [[nodiscard]] usize fill_datagram(u8* out, u64 index, u64 tag) const noexcept
    {
        const usize size = static_cast<usize>(m_run.datagram_bytes);
        for (usize at = 0U; at < kDatagramIndexBytes; ++at)
        {
            out[at] = static_cast<u8>(index >> (at * 8U));
        }
        for (usize at = kDatagramIndexBytes; at < size; ++at)
        {
            out[at] = pattern(tag, index, at);
        }
        return size;
    }

    void on_datagram(const crd::net::Http3Event& event) noexcept
    {
        ConstSpan<u8> payload = event.payload;
        u64 index = 0U;
        for (usize at = 0U; at < kDatagramIndexBytes && at < payload.size(); ++at)
        {
            index |= static_cast<u64>(payload[at]) << (at * 8U);
        }
        const u64 tag = m_run.client ? 3U : 2U;
        if (payload.size() != m_run.datagram_bytes || index >= m_run.datagrams ||
            index / per_tunnel() != index_of(event.stream) ||
            (m_datagrams_seen[index / 8U] & (1U << (index % 8U))) != 0U)
        {
            m_datagrams_unverified = true;
            return;
        }
        m_datagrams_seen[index / 8U] = static_cast<u8>(m_datagrams_seen[index / 8U] | (1U << (index % 8U)));
        for (usize at = kDatagramIndexBytes; at < payload.size(); ++at)
        {
            m_datagrams_unverified = m_datagrams_unverified || payload[at] != pattern(tag, index, at);
        }
        ++m_datagrams_received;
        m_last_datagram_ns = m_now_ns;
        if (!m_run.client)
        {
            m_echoes[(m_echo_first + m_echo_count) % kMaxDatagrams] = static_cast<u32>(index);
            ++m_echo_count;
        }
    }

    void service_datagrams() noexcept
    {
        u8 payload[kMaxDatagramBytes];
        while (m_run.datagrams != 0U)
        {
            u64 index = 0U;
            if (m_run.client)
            {
                index = m_datagrams_sent;
                if (index >= m_run.datagrams || !m_streams[index / per_tunnel()].tunnel_open)
                {
                    return;
                }
            }
            else
            {
                if (m_echo_count == 0U)
                {
                    return;
                }
                index = m_echoes[m_echo_first];
            }
            const usize size = fill_datagram(payload, index, m_run.client ? 2U : 3U);
            const crd::net::Http3Error error =
                m_h3->send_datagram((index / per_tunnel()) * 4U, ConstSpan<u8>(payload, size), m_now_ns);
            if (error == crd::net::Http3Error::Blocked)
            {
                return;
            }
            m_datagrams_unverified = m_datagrams_unverified || error != crd::net::Http3Error::None;
            ++m_datagrams_sent;
            m_last_datagram_ns = m_now_ns;
            if (!m_run.client)
            {
                m_echo_first = (m_echo_first + 1U) % kMaxDatagrams;
                --m_echo_count;
            }
        }
    }

    [[nodiscard]] bool lingered(bool over) noexcept
    {
        if (over && m_done_ns == 0U)
        {
            m_done_ns = m_now_ns;
        }
        return over && m_now_ns - m_done_ns >= HTTPX_LINGER_NS;
    }

    [[nodiscard]] bool datagrams_settled() const noexcept
    {
        if (m_run.datagrams == 0U)
        {
            return true;
        }
        if (m_datagrams_sent < m_run.datagrams)
        {
            return false;
        }
        return m_datagrams_received >= m_run.datagrams || m_now_ns - m_last_datagram_ns >= kDatagramSettleNs;
    }

    // The close would take a reset still unacknowledged with it, so a cancel waits for its acknowledgement.
    [[nodiscard]] bool complete(const crd::net::QuicStreamSet& set) const noexcept
    {
        const crd::net::QuicRecvState cancelled = m_cancelled != 0U ? set.recv_state(m_cancelled_id)
                                                                    : crd::net::QuicRecvState::None;
        if (cancelled != crd::net::QuicRecvState::None && cancelled != crd::net::QuicRecvState::ResetRecvd &&
            cancelled != crd::net::QuicRecvState::ResetRead)
        {
            return false;
        }
        if (!m_h3->settings_known() || m_opened < options().requests ||
            m_completed + m_rejected + m_aborted + m_cancelled + m_refused_too_large < options().requests ||
            !datagrams_settled() || (m_run.tickets != nullptr && m_run.tickets->size() == 0U))
        {
            return false;
        }
        if (options().goaway == HTTPX_GOAWAY_NOTICE)
        {
            return m_goaway_seen >= 2U && m_refused_after_goaway != 0U;
        }
        return options().goaway != HTTPX_GOAWAY_REJECT || m_goaway_seen >= 1U;
    }

    Http3Run m_run;
    crd::net::Http3Settings m_settings;
    bool m_settings_known = false;
    crd::net::Http3Connection* m_h3 = nullptr;
    alignas(crd::net::Http3Connection) unsigned char m_storage[sizeof(crd::net::Http3Connection)];
    Stream m_streams[HTTPX_MAX_REQUESTS];
    u64 m_now_ns = 0U;
    u64 m_done_ns = 0U;
    u64 m_opened = 0U;
    u64 m_seen = 0U;
    u64 m_request_ended = 0U;
    u64 m_completed = 0U;
    u64 m_rejected = 0U;
    u64 m_aborted = 0U;
    u64 m_abort_code = 0U;
    u64 m_cancelled = 0U;
    u64 m_cancelled_id = 0U;
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
    u64 m_early_requests = 0U;
    u64 m_datagrams_sent = 0U;
    u64 m_datagrams_received = 0U;
    u64 m_last_datagram_ns = 0U;
    u32 m_echoes[kMaxDatagrams] = {};
    u64 m_echo_first = 0U;
    u64 m_echo_count = 0U;
    u8 m_datagrams_seen[kMaxDatagrams / 8U] = {};
    bool m_unverified = false;
    bool m_datagrams_unverified = false;
    bool m_goaway_sent = false;
    bool m_forgot_early = false;
};
} // namespace crd_net_interop
