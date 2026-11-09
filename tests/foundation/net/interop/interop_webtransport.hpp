#pragma once

// crd-net interoperability test (4.2a) -- the WebTransport application crd-quic-peer runs with --webtransport, over
// crd-net's WebTransport sessions. webtransport_peer/src/main.rs runs the same one over web-transport-quinn.
//
// The session: a client's extended CONNECT to /wt; `--wt-protocols 1` offers "cerid-a", "cerid-b" and a server picks
// the second; `--wt-origin` is the request's Origin, `--wt-allow` the one a server takes (403 for any other);
// `--wt-reject` a server's answer instead of a 2xx.
//
// Each end opens, as soon as the session is up, what the common options say; every stream starts with a label of eight
// bytes (kind, three zero bytes, index, big-endian):
//   control  one bidirectional stream, the client's, label only. Each end writes one acknowledgement byte and a FIN
//            once it has finished everything else, so nothing is closed under the other end
//   echo     `streams` bidirectional streams: a label and `bytes` bytes of pattern and a FIN; the acceptor reads to the
//            FIN and answers with the label's kind with the top bit set, as many bytes in that pattern, and a FIN
//   uni      `uni` unidirectional streams: a label and `uni-bytes` bytes and a FIN
//   reset    `resets` bidirectional streams: a label; the acceptor answers an acknowledgement byte and a FIN; the
//            opener then resets its side with 0x10000 + index, which the acceptor must read
//   stop     `stops` bidirectional streams: a label; the acceptor stops the opener's side with 0x20000 + index and
//            sends a FIN, and the opener must read the code
// Both send `datagrams` datagrams, the index in four bytes and a pattern tagged by the sending end. The closing end
// sends WT_CLOSE_SESSION with 0x0C1D05E and "cerid-interop done"; the other reads them and closes the connection.

#include "http_exchange.h"
#include "interop_echo.hpp"
#include "interop_http3.hpp"

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/http3.hpp>
#include <crd/net/quic_connection.hpp>
#include <crd/net/webtransport.hpp>

#include <cstdio>
#include <cstring>
#include <new>

namespace crd_net_interop
{
inline constexpr usize kWtMaxItems = 1024U;
inline constexpr usize kWtMaxDatagrams = 1024U;
inline constexpr usize kWtLabelBytes = 8U;
inline constexpr usize kWtChunkBytes = 4096U;
inline constexpr usize kWtTextBytes = 64U;
inline constexpr u64 kWtCloseLingerNs = 500000000U;
inline constexpr u64 kWtDatagramSettleNs = 1000000000U;
inline constexpr u32 kWtResetBase = 0x10000U;
inline constexpr u32 kWtStopBase = 0x20000U;
inline constexpr u32 kWtCloseCode = 0x0C1D05EU;
inline constexpr char kWtCloseReason[] = "cerid-interop done";
inline constexpr char kWtProtocolOffer[] = "\"cerid-a\", \"cerid-b\"";
inline constexpr char kWtProtocolChosen[] = "cerid-b";
inline constexpr u8 kWtAck = 0xA5U;
inline constexpr u8 kWtControl = 0U;
inline constexpr u8 kWtEcho = 1U;
inline constexpr u8 kWtUni = 2U;
inline constexpr u8 kWtReset = 3U;
inline constexpr u8 kWtStop = 4U;
inline constexpr u8 kWtReply = 0x80U;
inline constexpr u8 kWtUnknown = 0xFFU;
inline constexpr u64 kWtClientDatagramTag = 16U;
inline constexpr u64 kWtServerDatagramTag = 17U;
inline constexpr u64 kWtFlowStreams = 2048U;

enum class WtRevision : u8
{
    None,
    Legacy,  // :protocol `webtransport`
    Current, // :protocol `webtransport-h3`
};

struct WtOptions
{
    WtRevision revision = WtRevision::None;
    u64 window = 0U; // both ends' WT_MAX_DATA and a stream limit that does not bind: flow control on; 0 leaves it off
    u64 reject = 0U;
    bool protocols = false;
    const char* origin = nullptr;
    const char* allow = nullptr;
};

// True when `name` was one of this application's options.
inline bool wt_parse_option(WtOptions& options, const char* name, const char* value) noexcept
{
    if (std::strcmp(name, "--webtransport") == 0)
    {
        options.revision = std::strcmp(value, "legacy") == 0 ? WtRevision::Legacy : WtRevision::Current;
    }
    else if (std::strcmp(name, "--wt-window") == 0)
    {
        options.window = std::strtoull(value, nullptr, 10);
    }
    else if (std::strcmp(name, "--wt-reject") == 0)
    {
        options.reject = std::strtoull(value, nullptr, 10);
    }
    else if (std::strcmp(name, "--wt-protocols") == 0)
    {
        options.protocols = std::strcmp(value, "0") != 0;
    }
    else if (std::strcmp(name, "--wt-origin") == 0)
    {
        options.origin = value;
    }
    else if (std::strcmp(name, "--wt-allow") == 0)
    {
        options.allow = value;
    }
    else
    {
        return false;
    }
    return true;
}

// What the exchange and one connection's run share.
struct WtRun
{
    const EchoOptions* echo = nullptr;
    const WtOptions* wt = nullptr;
    const httpx_options_t* http3 = nullptr;
};

// One connection's exchange, either end's.
class WtExchange
{
public:
    WtExchange() noexcept = default;
    WtExchange(const WtExchange&) = delete;
    WtExchange& operator=(const WtExchange&) = delete;
    WtExchange(WtExchange&&) = delete;
    WtExchange& operator=(WtExchange&&) = delete;
    ~WtExchange() = default;

    // The error to close the connection with, 0 to go on; `done` when this end should close it.
    [[nodiscard]] u64 service(crd::net::QuicConnection& connection, const WtRun& run, const EchoTurn& turn,
                              bool& done) noexcept
    {
        m_run = run;
        m_now_ns = turn.now_ns;
        if (m_h3 == nullptr && !make(connection))
        {
            m_unverified = true;
            return crd::net::kH3InternalError;
        }
        if (!m_h3->started() && (!turn.established || !m_h3->start()))
        {
            return m_h3->error();
        }
        crd::net::WtEvent event;
        while (m_wt->poll(event))
        {
            handle(event);
        }
        if (m_h3->error() != 0U)
        {
            return m_h3->error();
        }
        answer_or_connect();
        if (m_open)
        {
            open_items();
            for (usize at = 0U; at < m_item_count; ++at)
            {
                pump(m_items[at]);
            }
            service_datagrams();
            settle();
            close_when_finished();
        }
        done = m_refused || m_peer_closed || (m_close_sent && m_now_ns - m_close_at_ns >= kWtCloseLingerNs);
        return m_h3->error();
    }

    // The connection has ended: its report, and the sessions go before the connection they read.
    void report() noexcept
    {
        std::printf("wt_opened=%d\nwt_status=%llu\nwt_refused_error=%llu\nwt_legacy=%d\nwt_protocol=%s\nwt_origin=%s\n",
                    m_open ? 1 : 0, ull(m_status), ull(m_refused_error), m_legacy ? 1 : 0, m_protocol, m_origin);
        std::printf("wt_echo=%llu\nwt_uni=%llu\nwt_resets_sent=%llu\nwt_resets_seen=%llu\nwt_stops_sent=%llu\n"
                    "wt_stops_seen=%llu\nwt_bytes_received=%llu\nwt_bytes_sent=%llu\n",
                    ull(m_tally[kWtEcho]), ull(m_tally[kWtUni]), ull(m_resets_sent), ull(m_resets_seen),
                    ull(m_stops_sent), ull(m_stops_seen), ull(m_bytes_received), ull(m_bytes_sent));
        std::printf("wt_datagrams_sent=%llu\nwt_datagrams_received=%llu\nwt_control_done=%d\nwt_verified=%d\n"
                    "wt_close_seen=%d\nwt_close_code=%llu\nwt_close_reason=%s\nwt_close_error=%llu\n",
                    ull(m_datagrams_sent), ull(m_datagrams_received), m_control_done ? 1 : 0, m_unverified ? 0 : 1,
                    m_peer_closed ? 1 : 0, ull(m_close_code), m_reason, ull(m_close_error));
        if (m_wt != nullptr)
        {
            m_wt->~WebTransport();
            m_wt = nullptr;
        }
        if (m_h3 != nullptr)
        {
            m_h3->~Http3Connection();
            m_h3 = nullptr;
        }
    }

private:
    struct Item
    {
        u64 stream = 0U;
        u64 in = 0U;  // the bytes read after any label
        u64 out = 0U; // the bytes written, any label among them
        u32 index = 0U;
        u8 kind = kWtUnknown;
        u8 label[kWtLabelBytes] = {};
        u8 label_read = 0U;
        bool mine = false;
        bool in_end = false;
        bool out_end = false;
        bool aborted = false; // the peer's reset or STOP_SENDING came, with the code expected
        bool acted = false;   // this side's reset or STOP_SENDING went
        bool done = false;
    };

    [[nodiscard]] const EchoOptions& echo() const noexcept { return *m_run.echo; }
    [[nodiscard]] const WtOptions& wt() const noexcept { return *m_run.wt; }
    [[nodiscard]] static unsigned long long ull(u64 value) noexcept { return value; }
    [[nodiscard]] u64 per_end() const noexcept { return echo().streams + echo().uni + echo().resets + echo().stops; }

    [[nodiscard]] bool make(crd::net::QuicConnection& connection) noexcept
    {
        crd::net::Http3Config config = http3_config(*m_run.http3, echo().client, 1U);
        config.webtransport.enabled = true;
        config.webtransport.legacy_sessions = 1U;
        if (wt().window != 0U)
        {
            config.webtransport.max_streams_uni = kWtFlowStreams;
            config.webtransport.max_streams_bidi = kWtFlowStreams;
            config.webtransport.max_data = wt().window;
        }
        crd::memory::IAllocator& allocator = *crd::memory::default_allocator();
        m_h3 = new (m_h3_storage) crd::net::Http3Connection();
        if (!m_h3->init(allocator, connection.streams(), &connection.datagrams(), config))
        {
            return false;
        }
        crd::net::WebTransportConfig sessions;
        sessions.sessions = 1U;
        sessions.any_origin = wt().allow == nullptr;
        if (wt().allow != nullptr)
        {
            m_allowed = text_bytes(wt().allow, std::strlen(wt().allow));
            sessions.origins = ConstSpan<ConstSpan<u8>>(&m_allowed, 1U);
        }
        m_wt = new (m_wt_storage) crd::net::WebTransport();
        return m_wt->init(allocator, *m_h3, connection.streams(), sessions);
    }

    static void copy_text(ConstSpan<u8> from, char* to) noexcept
    {
        const usize size = from.size() < kWtTextBytes - 1U ? from.size() : kWtTextBytes - 1U;
        if (size != 0U)
        {
            std::memcpy(to, from.data(), size);
        }
        to[size] = '\0';
    }

    void handle(const crd::net::WtEvent& event) noexcept
    {
        using crd::net::WtEventKind;
        switch (event.kind)
        {
            case WtEventKind::SessionRequest:
                on_request(event);
                break;
            case WtEventKind::SessionReady:
                m_status = event.status != 0U ? event.status : 200U;
                m_open = true;
                copy_text(event.bytes, m_protocol);
                break;
            case WtEventKind::SessionRefused:
                m_refused = true;
                m_status = event.status;
                m_refused_error = event.error;
                break;
            case WtEventKind::SessionClosed:
                m_peer_closed = true;
                m_close_code = event.code;
                m_close_error = event.error;
                copy_text(event.bytes, m_reason);
                break;
            case WtEventKind::StreamOpened:
                on_stream_opened(event.stream);
                break;
            case WtEventKind::StreamAborted:
                on_aborted(event, kWtReset, kWtResetBase, m_resets_seen);
                break;
            case WtEventKind::StreamStopped:
                on_aborted(event, kWtStop, kWtStopBase, m_stops_seen);
                break;
            case WtEventKind::Datagram:
                on_datagram(event.bytes);
                break;
            case WtEventKind::Http:
            case WtEventKind::SessionDraining:
            case WtEventKind::SessionWritable:
            case WtEventKind::StreamReadable:
            case WtEventKind::StreamWritable:
                break;
        }
    }

    void on_request(const crd::net::WtEvent& event) noexcept
    {
        m_session = event.session;
        m_legacy = event.legacy;
        m_answer_due = true;
        for (const crd::net::HttpField& field : event.fields)
        {
            if (field.name.size() == 6U && std::memcmp(field.name.data(), "origin", 6U) == 0)
            {
                copy_text(field.value, m_origin);
            }
        }
        usize at = 0U;
        usize bytes = 0U;
        u8 scratch[kWtTextBytes];
        usize offered = 0U;
        while (crd::net::wt_next_protocol(event.bytes, at, crd::containers::Span<u8>(scratch), bytes))
        {
            ++offered;
        }
        if (wt().protocols && offered != 2U)
        {
            m_unverified = true;
        }
    }

    void answer_or_connect() noexcept
    {
        if (m_answer_due)
        {
            using crd::net::Http3Error;
            const Http3Error error =
                wt().reject != 0U
                    ? m_wt->reject(m_session, static_cast<crd::u16>(wt().reject))
                    : m_wt->accept(m_session, {},
                                   wt().protocols ? text_bytes(kWtProtocolChosen, sizeof(kWtProtocolChosen) - 1U)
                                                  : ConstSpan<u8>());
            if (error == Http3Error::Blocked)
            {
                return;
            }
            m_answer_due = false;
            m_status = wt().reject != 0U ? wt().reject : 200U;
            m_open = wt().reject == 0U && error == Http3Error::None;
            m_unverified = m_unverified || error != Http3Error::None;
            if (m_open && wt().protocols)
            {
                std::memcpy(m_protocol, kWtProtocolChosen, sizeof(kWtProtocolChosen));
            }
        }
        if (echo().client && !m_connected && m_h3->settings_known())
        {
            connect();
        }
    }

    void connect() noexcept
    {
        Http3Fields fields;
        fields.add_text(":method", "CONNECT");
        fields.add_text(":protocol", wt().revision == WtRevision::Legacy ? "webtransport" : "webtransport-h3");
        fields.add_text(":scheme", "https");
        fields.add_text(":authority", "cerid.test");
        fields.add_text(":path", "/wt");
        if (wt().protocols)
        {
            fields.add_text("wt-available-protocols", kWtProtocolOffer);
        }
        if (wt().origin != nullptr)
        {
            fields.add_text("origin", wt().origin);
        }
        const crd::net::Http3Error error = m_wt->connect(fields.span(), m_session, wt().protocols);
        m_connected = error == crd::net::Http3Error::None;
        m_legacy = wt().revision == WtRevision::Legacy;
        m_unverified = m_unverified || (error != crd::net::Http3Error::None && error != crd::net::Http3Error::Blocked &&
                                        error != crd::net::Http3Error::NotReady);
    }

    Item* add_item() noexcept
    {
        if (m_item_count == kWtMaxItems)
        {
            m_unverified = true;
            return nullptr;
        }
        return &m_items[m_item_count++];
    }

    Item* find(u64 stream) noexcept
    {
        for (usize at = 0U; at < m_item_count; ++at)
        {
            if (m_items[at].stream == stream)
            {
                return &m_items[at];
            }
        }
        return nullptr;
    }

    void on_stream_opened(u64 stream) noexcept
    {
        Item* const item = add_item();
        if (item != nullptr)
        {
            item->stream = stream;
        }
    }

    // A reset read by a stream's acceptor, or a STOP_SENDING read by its opener.
    void on_aborted(const crd::net::WtEvent& event, u8 kind, u32 base, u64& count) noexcept
    {
        Item* const item = find(event.stream);
        if (item == nullptr || (kind == kWtReset && item->kind == kWtStop)) // RFC 9000 3.5: a stopped sender resets
        {
            return;
        }
        if (item->kind != kind || item->mine != (kind == kWtStop) || !event.has_code ||
            event.code != base + item->index)
        {
            m_unverified = true;
            return;
        }
        item->aborted = true;
        ++count;
        pump(*item);
    }

    [[nodiscard]] static bool uni_id(u64 stream) noexcept { return (stream & 2U) != 0U; }

    // Opens what this end owes, as far as the limits let it.
    void open_items() noexcept
    {
        const u64 counts[] = {echo().client ? 1U : 0U, echo().streams, echo().uni, echo().resets, echo().stops};
        for (; m_open_kind < 5U; ++m_open_kind, m_open_index = 0U)
        {
            for (; m_open_index < counts[m_open_kind]; ++m_open_index)
            {
                u64 stream = 0U;
                const crd::net::Http3Error error = m_wt->open_stream(m_session, m_open_kind != kWtUni, stream);
                if (error == crd::net::Http3Error::NoStream || error == crd::net::Http3Error::Blocked)
                {
                    return;
                }
                Item* const item = error == crd::net::Http3Error::None ? add_item() : nullptr;
                if (item == nullptr)
                {
                    m_unverified = true;
                    return;
                }
                item->stream = stream;
                item->index = static_cast<u32>(m_open_index);
                item->kind = m_open_kind;
                item->mine = true;
            }
        }
    }

    [[nodiscard]] static bool label_in(const Item& item) noexcept { return !item.mine || item.kind == kWtEcho; }
    [[nodiscard]] static bool label_out(const Item& item) noexcept { return item.mine || item.kind == kWtEcho; }

    [[nodiscard]] u64 body_in(const Item& item) const noexcept
    {
        switch (item.kind)
        {
            case kWtEcho:
                return echo().bytes;
            case kWtUni:
                return echo().uni_bytes;
            case kWtReset:
                return item.mine ? 1U : 0U;
            case kWtControl:
                return 1U;
            default:
                return 0U;
        }
    }

    [[nodiscard]] u64 body_out(const Item& item) const noexcept
    {
        switch (item.kind)
        {
            case kWtEcho:
                return echo().bytes;
            case kWtUni:
                return item.mine ? echo().uni_bytes : 0U;
            case kWtReset:
                return item.mine ? 0U : 1U;
            case kWtControl:
                return m_complete ? 1U : 0U;
            default:
                return 0U;
        }
    }

    [[nodiscard]] u64 kind_count(u8 kind) const noexcept
    {
        switch (kind)
        {
            case kWtControl:
                return 1U;
            case kWtEcho:
                return echo().streams;
            case kWtUni:
                return echo().uni;
            case kWtReset:
                return echo().resets;
            case kWtStop:
                return echo().stops;
            default:
                return 0U;
        }
    }

    // The label's bytes `data` holds, `used` past them; a complete one names a peer's stream.
    void read_label(Item& item, ConstSpan<u8> data, usize& used) noexcept
    {
        while (item.label_read < kWtLabelBytes && used < data.size())
        {
            item.label[item.label_read] = data[used];
            ++item.label_read;
            ++used;
        }
        if (item.label_read != kWtLabelBytes)
        {
            return;
        }
        const u8 kind = item.label[0];
        const u32 index = (static_cast<u32>(item.label[4]) << 24U) | (static_cast<u32>(item.label[5]) << 16U) |
                          (static_cast<u32>(item.label[6]) << 8U) | static_cast<u32>(item.label[7]);
        if (item.mine)
        {
            m_unverified = m_unverified || kind != (item.kind | kWtReply) || index != item.index;
        }
        else if (item.kind == kWtUnknown)
        {
            item.kind = kind;
            item.index = index;
            m_unverified = m_unverified || index >= kind_count(kind) || (kind == kWtUni) != uni_id(item.stream);
        }
    }

    void read_item(Item& item) noexcept
    {
        if (item.in_end || (item.aborted && !item.mine) || (item.mine && item.kind == kWtUni))
        {
            return;
        }
        for (;;)
        {
            const crd::net::QuicStreamRead read = m_wt->peek(item.stream);
            if (read.reset)
            {
                return;
            }
            usize used = 0U;
            if (label_in(item) && item.label_read < kWtLabelBytes)
            {
                read_label(item, read.data, used);
            }
            const bool ready = !label_in(item) || item.label_read == kWtLabelBytes;
            const u64 expected = ready ? body_in(item) : 0U;
            const bool ack = item.kind == kWtReset || item.kind == kWtControl;
            const u64 tag = static_cast<u64>(item.kind) | (item.mine ? kWtReply : 0U);
            for (; ready && used < read.data.size(); ++used)
            {
                const u8 want = ack ? kWtAck : pattern(tag, item.index, item.in);
                m_unverified = m_unverified || item.in >= expected || read.data[used] != want;
                ++item.in;
            }
            const bool fin = read.fin && used == read.data.size();
            if (used != 0U && m_wt->consume(item.stream, used) != crd::net::Http3Error::None)
            {
                m_unverified = true;
                return;
            }
            if (fin)
            {
                item.in_end = true;
                m_unverified = m_unverified || !ready || item.in != expected;
                m_bytes_received += item.kind == kWtEcho || item.kind == kWtUni ? item.in : 0U;
                return;
            }
            if (used == 0U)
            {
                return;
            }
        }
    }

    [[nodiscard]] u8 out_byte(const Item& item, u64 at, usize label_bytes) const noexcept
    {
        const u64 tag = static_cast<u64>(item.kind) | (item.mine ? 0U : kWtReply);
        if (at < label_bytes)
        {
            const u8 kind = item.mine ? item.kind : static_cast<u8>(item.kind | kWtReply);
            const u8 label[kWtLabelBytes] = {kind,
                                             0U,
                                             0U,
                                             0U,
                                             static_cast<u8>(item.index >> 24U),
                                             static_cast<u8>(item.index >> 16U),
                                             static_cast<u8>(item.index >> 8U),
                                             static_cast<u8>(item.index)};
            return label[at];
        }
        const bool ack = item.kind == kWtReset || item.kind == kWtControl;
        return ack ? kWtAck : pattern(tag, item.index, at - label_bytes);
    }

    // Whether this side may write what it owes on the stream yet, and whether a FIN ends it.
    [[nodiscard]] bool may_write(const Item& item, bool& fin) const noexcept
    {
        fin = item.kind == kWtEcho || item.kind == kWtUni;
        if (item.mine)
        {
            fin = fin || (item.kind == kWtControl && m_complete);
            return true;
        }
        const bool known = item.kind != kWtUnknown;
        switch (item.kind)
        {
            case kWtEcho:
                return known && item.in_end;
            case kWtReset:
                fin = true;
                return known;
            case kWtStop:
                fin = true;
                return item.acted;
            case kWtControl:
                fin = m_complete;
                return known;
            default:
                return false;
        }
    }

    void write_item(Item& item) noexcept
    {
        bool fin = false;
        if (item.out_end || !may_write(item, fin))
        {
            return;
        }
        const usize label_bytes = label_out(item) ? kWtLabelBytes : 0U;
        const u64 total = label_bytes + body_out(item);
        u8 chunk[kWtChunkBytes];
        for (;;)
        {
            const u64 left = total - item.out;
            const usize size = left < kWtChunkBytes ? static_cast<usize>(left) : kWtChunkBytes;
            if (size == 0U && !fin)
            {
                return;
            }
            for (usize at = 0U; at < size; ++at)
            {
                chunk[at] = out_byte(item, item.out + at, label_bytes);
            }
            usize taken = 0U;
            const bool last = size == left;
            const crd::net::Http3Error error =
                m_wt->write(item.stream, ConstSpan<u8>(chunk, size), fin && last, taken);
            if (error != crd::net::Http3Error::None && error != crd::net::Http3Error::Blocked)
            {
                m_unverified = true;
                return;
            }
            item.out += taken;
            if (error == crd::net::Http3Error::Blocked || taken != size)
            {
                return;
            }
            if (last)
            {
                item.out_end = fin;
                m_bytes_sent += fin && (item.kind == kWtEcho || item.kind == kWtUni) ? item.out - label_bytes : 0U;
                return;
            }
        }
    }

    // What a reset's or a stop's owner does once it has heard what it waits for.
    void act(Item& item) noexcept
    {
        if (item.acted)
        {
            return;
        }
        if (item.kind == kWtReset && item.mine && item.in == 1U)
        {
            item.acted = m_wt->reset(item.stream, kWtResetBase + item.index) == crd::net::Http3Error::None;
            m_resets_sent += item.acted ? 1U : 0U;
        }
        if (item.kind == kWtStop && !item.mine && item.label_read == kWtLabelBytes)
        {
            item.acted = m_wt->stop(item.stream, kWtStopBase + item.index) == crd::net::Http3Error::None;
            m_stops_sent += item.acted ? 1U : 0U;
        }
    }

    [[nodiscard]] static bool finished(const Item& item) noexcept
    {
        switch (item.kind)
        {
            case kWtEcho:
                return item.in_end && item.out_end;
            case kWtUni:
                return item.mine ? item.out_end : item.in_end;
            case kWtReset:
                return item.mine ? item.acted : item.aborted && item.out_end;
            case kWtStop:
                return item.mine ? item.aborted && item.in_end : item.acted && item.out_end;
            default:
                return false;
        }
    }

    void pump(Item& item) noexcept
    {
        read_item(item);
        act(item);
        write_item(item);
        if (item.kind == kWtControl)
        {
            m_control_done = item.in_end && item.out_end;
        }
        else if (!item.done && finished(item))
        {
            item.done = true;
            ++m_done;
            ++m_tally[item.kind & 7U];
        }
    }

    void on_datagram(ConstSpan<u8> bytes) noexcept
    {
        if (bytes.size() != echo().datagram_bytes || bytes.size() < kDatagramIndexBytes)
        {
            m_unverified = true;
            return;
        }
        const u32 index = (static_cast<u32>(bytes[0]) << 24U) | (static_cast<u32>(bytes[1]) << 16U) |
                          (static_cast<u32>(bytes[2]) << 8U) | static_cast<u32>(bytes[3]);
        const u64 tag = echo().client ? kWtServerDatagramTag : kWtClientDatagramTag;
        if (index >= echo().datagrams || index >= kWtMaxDatagrams)
        {
            m_unverified = true;
            return;
        }
        for (usize at = kDatagramIndexBytes; at < bytes.size(); ++at)
        {
            m_unverified = m_unverified || bytes[at] != pattern(tag, index, at);
        }
        if (!m_datagram_seen[index])
        {
            m_datagram_seen[index] = true;
            ++m_datagrams_received;
        }
    }

    void service_datagrams() noexcept
    {
        u8 payload[kMaxDatagramBytes];
        const u64 tag = echo().client ? kWtClientDatagramTag : kWtServerDatagramTag;
        while (m_datagrams_sent < echo().datagrams)
        {
            const u64 index = m_datagrams_sent;
            payload[0] = static_cast<u8>(index >> 24U);
            payload[1] = static_cast<u8>(index >> 16U);
            payload[2] = static_cast<u8>(index >> 8U);
            payload[3] = static_cast<u8>(index);
            for (usize at = kDatagramIndexBytes; at < echo().datagram_bytes; ++at)
            {
                payload[at] = pattern(tag, index, at);
            }
            const crd::net::Http3Error error =
                m_wt->send_datagram(m_session, ConstSpan<u8>(payload, echo().datagram_bytes), m_now_ns);
            if (error == crd::net::Http3Error::Blocked)
            {
                return;
            }
            m_unverified = m_unverified || error != crd::net::Http3Error::None;
            ++m_datagrams_sent;
        }
    }

    void settle() noexcept
    {
        if (m_complete || m_done < 2U * per_end() || m_datagrams_sent < echo().datagrams)
        {
            return;
        }
        if (m_items_done_ns == 0U)
        {
            m_items_done_ns = m_now_ns;
        }
        m_complete =
            m_datagrams_received >= echo().datagrams || m_now_ns - m_items_done_ns >= kWtDatagramSettleNs;
        for (usize at = 0U; m_complete && at < m_item_count; ++at)
        {
            if (m_items[at].kind == kWtControl)
            {
                pump(m_items[at]);
            }
        }
    }

    void close_when_finished() noexcept
    {
        if (!m_control_done || m_close_sent || m_peer_closed || echo().client_closes != echo().client)
        {
            return;
        }
        const crd::net::Http3Error error =
            m_wt->close(m_session, kWtCloseCode, text_bytes(kWtCloseReason, sizeof(kWtCloseReason) - 1U));
        if (error == crd::net::Http3Error::Blocked)
        {
            return;
        }
        m_unverified = m_unverified || error != crd::net::Http3Error::None;
        m_close_sent = true;
        m_close_at_ns = m_now_ns;
        m_close_code = kWtCloseCode;
    }

    WtRun m_run;
    crd::net::Http3Connection* m_h3 = nullptr;
    crd::net::WebTransport* m_wt = nullptr;
    ConstSpan<u8> m_allowed;
    u64 m_now_ns = 0U;
    u64 m_session = 0U;
    u64 m_status = 0U;
    u64 m_refused_error = 0U;
    u64 m_close_code = 0U;
    u64 m_close_error = 0U;
    u64 m_close_at_ns = 0U;
    u64 m_items_done_ns = 0U;
    u64 m_done = 0U;
    u64 m_tally[8] = {};
    u64 m_resets_sent = 0U;
    u64 m_resets_seen = 0U;
    u64 m_stops_sent = 0U;
    u64 m_stops_seen = 0U;
    u64 m_bytes_received = 0U;
    u64 m_bytes_sent = 0U;
    u64 m_datagrams_sent = 0U;
    u64 m_datagrams_received = 0U;
    u64 m_open_index = 0U;
    usize m_item_count = 0U;
    u8 m_open_kind = 0U;
    bool m_answer_due = false;
    bool m_connected = false;
    bool m_open = false;
    bool m_legacy = false;
    bool m_refused = false;
    bool m_complete = false;
    bool m_control_done = false;
    bool m_close_sent = false;
    bool m_peer_closed = false;
    bool m_unverified = false;
    char m_protocol[kWtTextBytes] = {};
    char m_origin[kWtTextBytes] = {};
    char m_reason[kWtTextBytes] = {};
    bool m_datagram_seen[kWtMaxDatagrams] = {};
    Item m_items[kWtMaxItems];
    alignas(crd::net::Http3Connection) unsigned char m_h3_storage[sizeof(crd::net::Http3Connection)];
    alignas(crd::net::WebTransport) unsigned char m_wt_storage[sizeof(crd::net::WebTransport)];
};
} // namespace crd_net_interop
