/* crd-net interoperability test (4.2a) -- the exchange of http_exchange.h over nghttp2 and real TCP on the loopback,
 * with prior knowledge; crd-http2-peer runs the same over Http2Connection and http2_interop.py compares the reports.
 *
 * Usage: crd-nghttp2-peer --role client|server --port N [--max-streams N] [--window N] [--frame-bytes N] [--padding 1]
 *                         and the options of http_exchange.h
 *
 *   padding  frames carry nghttp2's padding, up to 64 bytes on each */
#define _POSIX_C_SOURCE 200809L

#include "http_exchange.h"

#include <nghttp2/nghttp2.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RUN_SECONDS 60
#define INPUT_BYTES 65536U
#define MAX_NV 12U
#define NUMBER_TEXT 24U
#define PADDING_STEP 64U
#define CONNECTION_WINDOW (1 << 24)

typedef struct
{
    httpx_section_t section;
    uint64_t        index;
    uint64_t        received;
    uint64_t        sent;
    int             used;
    int             final_seen;
    int             tunnel_open;
    int             ended;
    int             counted;
    int             trailers_seen;
    int             deferred;
    int             responded;
} stream_t;

typedef struct
{
    httpx_options_t  options;
    nghttp2_session* session;
    stream_t         streams[HTTPX_MAX_REQUESTS];
    int              fd;
    int              client;
    int              port;
    int              padding;
    uint32_t         max_streams;
    uint32_t         window;
    uint32_t         frame_bytes;
    int              settings_seen;
    int              opened_all;
    int              goaway_sent;
    int              failed;
    uint64_t         opened;
    uint64_t         seen;
    uint64_t         request_ended;
    uint64_t         completed;
    uint64_t         rejected;
    uint64_t         aborted;
    uint64_t         abort_code;
    uint64_t         cancelled;
    uint64_t         status_431;
    uint64_t         refused_after_goaway;
    uint64_t         probes;
    uint64_t         goaway_seen;
    uint64_t         goaway_id;
    uint64_t         body_received;
    uint64_t         body_sent;
    uint64_t         interims;
    uint64_t         trailers;
    uint64_t         tunnels;
    int              unverified;
    char             fill[HTTPX_MAX_FILL];
    const uint8_t*   pending;
    size_t           pending_bytes;
} peer_t;

static uint64_t index_of(int32_t id)
{
    return (uint64_t)(id - 1) / 2U;
}

static stream_t* stream_of(peer_t* peer, int32_t id)
{
    const uint64_t index = index_of(id);
    return &peer->streams[index < HTTPX_MAX_REQUESTS ? index : 0U];
}

static nghttp2_nv field(const char* name, const char* value, size_t size)
{
    nghttp2_nv nv;
    nv.name     = (uint8_t*)(uintptr_t)name;
    nv.value    = (uint8_t*)(uintptr_t)value;
    nv.namelen  = strlen(name);
    nv.valuelen = size;
    nv.flags    = NGHTTP2_NV_FLAG_NONE;
    return nv;
}

static nghttp2_nv text(const char* name, const char* value)
{
    return field(name, value, strlen(value));
}

static size_t common_fields(const peer_t* peer, nghttp2_nv* nv, size_t count, uint64_t index, const char* number)
{
    const uint64_t fill = httpx_fill_bytes(&peer->options, index);
    nv[count++] = text("x-cerid-repeat", HTTPX_REPEAT_VALUE);
    nv[count++] = text("x-cerid-index", number);
    if (fill != 0U)
    {
        nv[count++] = field("x-cerid-fill", peer->fill, fill);
    }
    return count;
}

static void submit_trailers(peer_t* peer, int32_t id, uint64_t index)
{
    char       number[NUMBER_TEXT];
    nghttp2_nv nv[2];
    snprintf(number, sizeof(number), "%llu", (unsigned long long)index);
    nv[0] = text("x-cerid-trailer", number);
    nv[1] = text("x-cerid-repeat", HTTPX_REPEAT_VALUE);
    peer->unverified = peer->unverified || nghttp2_submit_trailer(peer->session, id, nv, 2U) != 0;
}

/* A tunnel's body waits for its opening (a client's) or the request's end (a server's). */
static int body_may_go(const peer_t* peer, const stream_t* stream, uint64_t index)
{
    if (!httpx_is_tunnel(&peer->options, index))
    {
        return 1;
    }
    return peer->client ? stream->tunnel_open : stream->ended;
}

static nghttp2_ssize read_body(nghttp2_session* session, int32_t id, uint8_t* out, size_t length, uint32_t* flags,
                               nghttp2_data_source* source, void* user)
{
    peer_t* const   peer   = user;
    stream_t* const stream = source->ptr;
    const uint64_t  index  = index_of(id);
    (void)session;
    if (!body_may_go(peer, stream, index))
    {
        stream->deferred = 1;
        return NGHTTP2_ERR_DEFERRED;
    }
    const uint64_t goal = httpx_body_bytes(&peer->options, index, !peer->client);
    const uint64_t tag  = peer->client ? 0U : 1U;
    const uint64_t left = goal - stream->sent;
    const size_t   size = left < length ? (size_t)left : length;
    for (size_t at = 0U; at < size; ++at)
    {
        out[at] = httpx_pattern(tag, (uint64_t)id, stream->sent + at);
    }
    stream->sent += size;
    peer->body_sent += size;
    if (stream->sent == goal)
    {
        *flags |= NGHTTP2_DATA_FLAG_EOF;
        if (httpx_has_trailers(&peer->options, index))
        {
            *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
            submit_trailers(peer, id, index);
        }
    }
    return (nghttp2_ssize)size;
}

static void resume(peer_t* peer, int32_t id, stream_t* stream)
{
    if (stream->deferred)
    {
        stream->deferred = 0;
        peer->unverified = peer->unverified || nghttp2_session_resume_data(peer->session, id) != 0;
    }
}

static void respond(peer_t* peer, int32_t id, uint64_t index, stream_t* stream)
{
    if (stream->responded)
    {
        return;
    }
    stream->responded = 1;
    if (peer->options.interim)
    {
        nghttp2_nv interim[2];
        interim[0] = text(":status", "103");
        interim[1] = text("link", "</cerid.css>; rel=preload");
        peer->unverified = peer->unverified ||
                           nghttp2_submit_headers(peer->session, NGHTTP2_FLAG_NONE, id, NULL, interim, 2U, NULL) < 0;
    }
    const uint64_t body   = httpx_body_bytes(&peer->options, index, 1);
    const int      tunnel = httpx_is_tunnel(&peer->options, index);
    char           number[NUMBER_TEXT];
    char           length[NUMBER_TEXT];
    nghttp2_nv     nv[MAX_NV];
    size_t         count = 0U;
    snprintf(number, sizeof(number), "%llu", (unsigned long long)index);
    snprintf(length, sizeof(length), "%llu", (unsigned long long)body);
    nv[count++] = text(":status", "200");
    count       = common_fields(peer, nv, count, index, number);
    if (httpx_has_length(&peer->options, index, body))
    {
        nv[count++] = text("content-length", length);
    }
    nghttp2_data_provider2 provider;
    provider.source.ptr    = stream;
    provider.read_callback = read_body;
    const int with_body    = body != 0U || httpx_has_trailers(&peer->options, index) || tunnel;
    peer->completed += 1U;
    peer->unverified = peer->unverified ||
                       nghttp2_submit_response2(peer->session, id, nv, count, with_body ? &provider : NULL) != 0;
}

static void open_requests(peer_t* peer)
{
    for (uint64_t index = 0U; index < peer->options.requests; ++index)
    {
        const uint64_t body   = httpx_body_bytes(&peer->options, index, 0);
        const int      tunnel = httpx_is_tunnel(&peer->options, index);
        stream_t*      stream = &peer->streams[index];
        char           number[NUMBER_TEXT];
        char           length[NUMBER_TEXT];
        char           path[NUMBER_TEXT + 8U];
        nghttp2_nv     nv[MAX_NV];
        size_t         count = 0U;
        snprintf(number, sizeof(number), "%llu", (unsigned long long)index);
        snprintf(length, sizeof(length), "%llu", (unsigned long long)body);
        snprintf(path, sizeof(path), tunnel ? "/tunnel/%llu" : "/cerid/%llu", (unsigned long long)index);
        nv[count++] = text(":method", httpx_method(tunnel, body));
        nv[count++] = text(":scheme", "http");
        nv[count++] = text(":authority", HTTPX_SERVER_NAME);
        nv[count++] = text(":path", path);
        if (tunnel)
        {
            nv[count++] = text(":protocol", HTTPX_TUNNEL_PROTOCOL);
        }
        count = common_fields(peer, nv, count, index, number);
        if (httpx_has_length(&peer->options, index, body) && body != 0U)
        {
            nv[count++] = text("content-length", length);
        }
        nghttp2_data_provider2 provider;
        provider.source.ptr    = stream;
        provider.read_callback = read_body;
        const int with_body    = body != 0U || httpx_has_trailers(&peer->options, index) || tunnel;
        stream->used           = 1;
        stream->index          = index;
        const int32_t id =
            nghttp2_submit_request2(peer->session, NULL, nv, count, with_body ? &provider : NULL, stream);
        peer->unverified = peer->unverified || id != (int32_t)((index * 2U) + 1U);
        peer->opened += 1U;
    }
    peer->opened_all = 1;
}

static void probe_after_goaway(peer_t* peer)
{
    nghttp2_nv nv[4];
    nv[0] = text(":method", "GET");
    nv[1] = text(":scheme", "http");
    nv[2] = text(":authority", HTTPX_SERVER_NAME);
    nv[3] = text(":path", "/cerid/probe");
    if (nghttp2_submit_request2(peer->session, NULL, nv, 4U, NULL, NULL) > 0)
    {
        peer->probes += 1U;
    }
}

static int on_begin_headers(nghttp2_session* session, const nghttp2_frame* frame, void* user)
{
    peer_t* const   peer   = user;
    stream_t* const stream = stream_of(peer, frame->hd.stream_id);
    (void)session;
    if (frame->headers.cat == NGHTTP2_HCAT_REQUEST || frame->headers.cat == NGHTTP2_HCAT_RESPONSE ||
        (peer->client && !stream->final_seen))
    {
        memset(&stream->section, 0, sizeof(stream->section));
    }
    return 0;
}

static int on_header(nghttp2_session* session, const nghttp2_frame* frame, nghttp2_rcbuf* name, nghttp2_rcbuf* value,
                     uint8_t flags, void* user)
{
    peer_t* const    peer       = user;
    const int32_t    id         = frame->hd.stream_id;
    stream_t* const  stream     = stream_of(peer, id);
    const nghttp2_vec name_vec  = nghttp2_rcbuf_get_buf(name);
    const nghttp2_vec value_vec = nghttp2_rcbuf_get_buf(value);
    const uint64_t   index      = index_of(id);
    const int        trailer    = frame->headers.cat == NGHTTP2_HCAT_HEADERS && (!peer->client || stream->final_seen);
    (void)session;
    (void)flags;
    if (trailer)
    {
        if (httpx_text_is(name_vec.base, name_vec.len, "x-cerid-trailer"))
        {
            stream->trailers_seen = httpx_decimal(value_vec.base, value_vec.len) == index;
        }
        return 0;
    }
    httpx_take_field(&stream->section, &peer->options, index, name_vec.base, name_vec.len, value_vec.base,
                     value_vec.len);
    return 0;
}

static void submit_goaway(peer_t* peer, int32_t last)
{
    const int code = nghttp2_submit_goaway(peer->session, NGHTTP2_FLAG_NONE, last, NGHTTP2_NO_ERROR, NULL, 0U);
    peer->unverified = peer->unverified || code != 0;
}

static void first_request(peer_t* peer, int32_t id)
{
    if (peer->options.goaway == HTTPX_GOAWAY_NOTICE)
    {
        peer->unverified = peer->unverified || nghttp2_submit_shutdown_notice(peer->session) != 0;
    }
    else if (peer->options.goaway == HTTPX_GOAWAY_REJECT)
    {
        submit_goaway(peer, id);
    }
}

static void cancel(peer_t* peer, int32_t id, stream_t* stream)
{
    if (!stream->counted && nghttp2_submit_rst_stream(peer->session, NGHTTP2_FLAG_NONE, id, NGHTTP2_CANCEL) == 0)
    {
        stream->counted = 1;
        peer->cancelled += 1U;
    }
}

static void on_request_headers(peer_t* peer, int32_t id, uint64_t index, stream_t* stream)
{
    stream->used = 1;
    peer->unverified = peer->unverified || !httpx_section_good(&stream->section, &peer->options, index, 0);
    peer->seen += 1U;
    if (peer->seen == 1U)
    {
        first_request(peer, id);
    }
    if (peer->options.cancel == HTTPX_CANCEL_SERVER && httpx_last_request(&peer->options, index))
    {
        cancel(peer, id, stream);
        return;
    }
    if (httpx_is_tunnel(&peer->options, index))
    {
        peer->tunnels += 1U;
        respond(peer, id, index, stream);
    }
}

static void on_response_headers(peer_t* peer, int32_t id, uint64_t index, stream_t* stream)
{
    if (stream->section.status >= 100U && stream->section.status < 200U)
    {
        peer->interims += 1U;
        return;
    }
    if (stream->final_seen)
    {
        return;
    }
    stream->final_seen = 1;
    if (httpx_is_tunnel(&peer->options, index) && stream->section.status == 200U)
    {
        stream->tunnel_open = 1;
        peer->tunnels += 1U;
        resume(peer, id, stream);
    }
}

static void response_ended(peer_t* peer, uint64_t index, stream_t* stream)
{
    if (stream->counted)
    {
        return;
    }
    stream->counted = 1;
    peer->completed += 1U;
    if (peer->options.over_limit && httpx_last_request(&peer->options, index) && stream->section.status == 431U)
    {
        peer->status_431 += 1U;
        return;
    }
    const int trailers = httpx_has_trailers(&peer->options, index) && !stream->trailers_seen;
    peer->unverified   = peer->unverified || !httpx_section_good(&stream->section, &peer->options, index, 1) ||
                       stream->received != httpx_body_bytes(&peer->options, index, 1) || trailers;
    peer->trailers += stream->trailers_seen ? 1U : 0U;
}

static void request_ended(peer_t* peer, int32_t id, uint64_t index, stream_t* stream)
{
    const int trailers = httpx_has_trailers(&peer->options, index) && !stream->trailers_seen;
    stream->ended      = 1;
    peer->unverified   = peer->unverified || stream->received != httpx_body_bytes(&peer->options, index, 0) || trailers;
    peer->trailers += stream->trailers_seen ? 1U : 0U;
    peer->request_ended += 1U;
    if (peer->options.goaway == HTTPX_GOAWAY_NOTICE && peer->request_ended == peer->options.requests)
    {
        const int32_t last = nghttp2_session_get_last_proc_stream_id(peer->session);
        submit_goaway(peer, last);
    }
    if (httpx_is_tunnel(&peer->options, index))
    {
        resume(peer, id, stream);
    }
    else if (!stream->counted && (peer->options.goaway != HTTPX_GOAWAY_CLIENT || peer->goaway_seen != 0U))
    {
        respond(peer, id, index, stream);
    }
}

static void on_goaway(peer_t* peer, const nghttp2_goaway* goaway)
{
    peer->goaway_seen += 1U;
    peer->goaway_id = (uint64_t)goaway->last_stream_id;
    if (!peer->client && peer->options.goaway == HTTPX_GOAWAY_CLIENT)
    {
        for (uint64_t index = 0U; index < peer->options.requests; ++index)
        {
            if (peer->streams[index].ended && !httpx_is_tunnel(&peer->options, index))
            {
                respond(peer, (int32_t)((index * 2U) + 1U), index, &peer->streams[index]);
            }
        }
    }
    if (peer->client && peer->options.goaway == HTTPX_GOAWAY_NOTICE && peer->goaway_seen == 2U)
    {
        probe_after_goaway(peer);
    }
}

static int on_frame_recv(nghttp2_session* session, const nghttp2_frame* frame, void* user)
{
    peer_t* const peer = user;
    const int32_t id   = frame->hd.stream_id;
    (void)session;
    switch (frame->hd.type)
    {
        case NGHTTP2_SETTINGS:
            if ((frame->hd.flags & NGHTTP2_FLAG_ACK) == 0U)
            {
                peer->settings_seen = 1;
            }
            return 0;
        case NGHTTP2_GOAWAY:
            on_goaway(peer, &frame->goaway);
            return 0;
        case NGHTTP2_HEADERS:
            if (frame->headers.cat == NGHTTP2_HCAT_REQUEST)
            {
                on_request_headers(peer, id, index_of(id), stream_of(peer, id));
            }
            else if (peer->client && !stream_of(peer, id)->final_seen)
            {
                on_response_headers(peer, id, index_of(id), stream_of(peer, id));
            }
            break;
        case NGHTTP2_DATA:
            break;
        default:
            return 0;
    }
    if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U)
    {
        if (peer->client)
        {
            response_ended(peer, index_of(id), stream_of(peer, id));
        }
        else
        {
            request_ended(peer, id, index_of(id), stream_of(peer, id));
        }
    }
    return 0;
}

static int on_data(nghttp2_session* session, uint8_t flags, int32_t id, const uint8_t* data, size_t length, void* user)
{
    peer_t* const   peer   = user;
    stream_t* const stream = stream_of(peer, id);
    const uint64_t  tag    = peer->client ? 1U : 0U;
    (void)session;
    (void)flags;
    for (size_t at = 0U; at < length; ++at)
    {
        peer->unverified = peer->unverified || data[at] != httpx_pattern(tag, (uint64_t)id, stream->received + at);
    }
    stream->received += length;
    peer->body_received += length;
    if (peer->client && peer->options.cancel == HTTPX_CANCEL_CLIENT && httpx_last_request(&peer->options, index_of(id)))
    {
        cancel(peer, id, stream);
    }
    return 0;
}

static int on_stream_close(nghttp2_session* session, int32_t id, uint32_t error, void* user)
{
    peer_t* const   peer   = user;
    stream_t* const stream = stream_of(peer, id);
    (void)session;
    if (error == NGHTTP2_NO_ERROR || stream->counted)
    {
        return 0;
    }
    stream->counted = 1;
    if (error == NGHTTP2_REFUSED_STREAM)
    {
        peer->rejected += 1U;
    }
    else
    {
        peer->aborted += 1U;
        peer->abort_code = error;
    }
    return 0;
}

static int on_not_sent(nghttp2_session* session, const nghttp2_frame* frame, int code, void* user)
{
    peer_t* const peer = user;
    (void)session;
    if (frame->hd.type == NGHTTP2_HEADERS && code == NGHTTP2_ERR_START_STREAM_NOT_ALLOWED)
    {
        peer->refused_after_goaway += peer->probes != 0U ? 1U : 0U;
    }
    return 0;
}

static nghttp2_ssize select_padding(nghttp2_session* session, const nghttp2_frame* frame, size_t max_payload,
                                    void* user)
{
    const peer_t* const peer   = user;
    const size_t        padded = frame->hd.length + PADDING_STEP - (frame->hd.length % PADDING_STEP);
    (void)session;
    if (!peer->padding)
    {
        return (nghttp2_ssize)frame->hd.length;
    }
    return (nghttp2_ssize)(padded < max_payload ? padded : max_payload);
}

static nghttp2_ssize read_length(nghttp2_session* session, uint8_t type, int32_t id, int32_t session_window,
                                 int32_t stream_window, uint32_t remote_frame, void* user)
{
    const int32_t window = session_window < stream_window ? session_window : stream_window;
    (void)session;
    (void)type;
    (void)id;
    (void)user;
    return (nghttp2_ssize)((uint32_t)window < remote_frame ? (uint32_t)window : remote_frame);
}

static int make_session(peer_t* peer)
{
    nghttp2_session_callbacks* callbacks = NULL;
    nghttp2_option*            option    = NULL;
    int                        code      = nghttp2_session_callbacks_new(&callbacks);
    code = code != 0 ? code : nghttp2_option_new(&option);
    if (code != 0)
    {
        return code;
    }
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback2(callbacks, on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close);
    nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks, on_not_sent);
    nghttp2_session_callbacks_set_select_padding_callback2(callbacks, select_padding);
    nghttp2_session_callbacks_set_data_source_read_length_callback2(callbacks, read_length);
    nghttp2_option_set_max_deflate_dynamic_table_size(option, peer->options.table);
    code = peer->client ? nghttp2_session_client_new2(&peer->session, callbacks, peer, option)
                        : nghttp2_session_server_new2(&peer->session, callbacks, peer, option);
    nghttp2_session_callbacks_del(callbacks);
    nghttp2_option_del(option);
    if (code != 0)
    {
        return code;
    }
    const nghttp2_settings_entry settings[] = {
        {NGHTTP2_SETTINGS_ENABLE_PUSH, 0U},
        {NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, (uint32_t)peer->options.table},
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, peer->max_streams},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, peer->window},
        {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, peer->frame_bytes},
        {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, (uint32_t)peer->options.field_limit},
        {NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, peer->client ? 0U : 1U},
    };
    /* A server does not send ENABLE_PUSH; a client does not send ENABLE_CONNECT_PROTOCOL. */
    const nghttp2_settings_entry* first = peer->client ? settings : settings + 1;
    const size_t                  count = (sizeof(settings) / sizeof(settings[0])) - 1U;
    code = nghttp2_submit_settings(peer->session, NGHTTP2_FLAG_NONE, first, count);
    return code != 0 ? code
                     : nghttp2_session_set_local_window_size(peer->session, NGHTTP2_FLAG_NONE, 0, CONNECTION_WINDOW);
}

static int set_socket(int fd)
{
    const int on    = 1;
    const int flags = fcntl(fd, F_GETFL, 0);
    return (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0)
               ? -1
               : 0;
}

static int open_socket(peer_t* peer)
{
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = htons((uint16_t)peer->port);
    const int fd            = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    if (peer->client)
    {
        if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0)
        {
            return -1;
        }
        peer->fd = fd;
        return set_socket(fd);
    }
    socklen_t size = sizeof(address);
    if (bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0 || listen(fd, 1) != 0 ||
        getsockname(fd, (struct sockaddr*)&address, &size) != 0)
    {
        return -1;
    }
    printf("port=%u\n", (unsigned)ntohs(address.sin_port));
    fflush(stdout);
    peer->fd = accept(fd, NULL, NULL);
    close(fd);
    return peer->fd < 0 ? -1 : set_socket(peer->fd);
}

/* Sends what nghttp2 has until the socket has no room; 0, or the failure. */
static int send_out(peer_t* peer)
{
    for (;;)
    {
        if (peer->pending_bytes == 0U)
        {
            const nghttp2_ssize size = nghttp2_session_mem_send2(peer->session, &peer->pending);
            if (size < 0)
            {
                return -1;
            }
            if (size == 0)
            {
                return 0;
            }
            peer->pending_bytes = (size_t)size;
        }
        const ssize_t written = send(peer->fd, peer->pending, peer->pending_bytes, MSG_NOSIGNAL);
        if (written < 0)
        {
            return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
        }
        peer->pending += written;
        peer->pending_bytes -= (size_t)written;
    }
}

static int complete(const peer_t* peer)
{
    if (!peer->settings_seen || !peer->opened_all ||
        peer->completed + peer->rejected + peer->aborted + peer->cancelled < peer->options.requests)
    {
        return 0;
    }
    if (peer->options.goaway == HTTPX_GOAWAY_NOTICE)
    {
        return peer->goaway_seen >= 2U && peer->refused_after_goaway != 0U;
    }
    return peer->options.goaway != HTTPX_GOAWAY_REJECT || peer->goaway_seen >= 1U;
}

static void report(const peer_t* peer)
{
    printf("connection=1\nh2_requests=%llu\nh2_seen=%llu\nh2_completed=%llu\nh2_rejected=%llu\nh2_aborted=%llu\n"
           "h2_abort_code=%llu\n",
           (unsigned long long)peer->opened, (unsigned long long)peer->seen, (unsigned long long)peer->completed,
           (unsigned long long)peer->rejected, (unsigned long long)peer->aborted, (unsigned long long)peer->abort_code);
    printf("h2_cancelled=%llu\nh2_status_431=%llu\nh2_refused_too_large=0\nh2_refused_after_goaway=%llu\n"
           "h2_goaway_seen=%llu\nh2_goaway_id=%llu\n",
           (unsigned long long)peer->cancelled, (unsigned long long)peer->status_431,
           (unsigned long long)peer->refused_after_goaway, (unsigned long long)peer->goaway_seen,
           (unsigned long long)peer->goaway_id);
    printf("h2_body_received=%llu\nh2_body_sent=%llu\nh2_interims=%llu\nh2_trailers=%llu\nh2_tunnels=%llu\n"
           "h2_verified=%d\nh2_error=%d\n",
           (unsigned long long)peer->body_received, (unsigned long long)peer->body_sent,
           (unsigned long long)peer->interims, (unsigned long long)peer->trailers, (unsigned long long)peer->tunnels,
           peer->unverified ? 0 : 1, peer->failed);
    const nghttp2_settings_id ids[] = {NGHTTP2_SETTINGS_HEADER_TABLE_SIZE,      NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,
                                       NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL,
                                       NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,
                                       NGHTTP2_SETTINGS_MAX_FRAME_SIZE};
    printf("h2_peer_known=%d\nh2_peer_table=%u\nh2_peer_field_limit=%u\nh2_peer_connect=%u\nh2_peer_streams=%u\n"
           "h2_peer_window=%u\nh2_peer_frame=%u\n",
           peer->settings_seen, nghttp2_session_get_remote_settings(peer->session, ids[0]),
           nghttp2_session_get_remote_settings(peer->session, ids[1]),
           nghttp2_session_get_remote_settings(peer->session, ids[2]),
           nghttp2_session_get_remote_settings(peer->session, ids[3]),
           nghttp2_session_get_remote_settings(peer->session, ids[4]),
           nghttp2_session_get_remote_settings(peer->session, ids[5]));
    fflush(stdout);
}

/* Reads what has come; 1 when the peer's end or a failure ended the run. */
static int read_in(peer_t* peer)
{
    uint8_t input[INPUT_BYTES];
    for (;;)
    {
        const ssize_t got = recv(peer->fd, input, sizeof(input), 0);
        if (got == 0)
        {
            return 1;
        }
        if (got < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                return 0;
            }
            peer->failed = errno;
            return 1;
        }
        const nghttp2_ssize used = nghttp2_session_mem_recv2(peer->session, input, (size_t)got);
        if (used < 0)
        {
            fprintf(stderr, "crd-nghttp2-peer: %s\n", nghttp2_strerror((int)used));
            peer->failed = (int)used;
            return 1;
        }
    }
}

static void service_client(peer_t* peer)
{
    if (peer->client && peer->settings_seen && !peer->opened_all)
    {
        open_requests(peer);
        if (peer->options.goaway == HTTPX_GOAWAY_CLIENT && !peer->goaway_sent)
        {
            peer->goaway_sent = 1;
            submit_goaway(peer, 0);
        }
    }
}

static int run(peer_t* peer)
{
    const time_t deadline = time(NULL) + RUN_SECONDS;
    int          closing  = 0;
    if (open_socket(peer) != 0 || make_session(peer) != 0)
    {
        fputs("crd-nghttp2-peer: the connection could not be made\n", stderr);
        return 1;
    }
    while (time(NULL) < deadline)
    {
        struct pollfd item = {peer->fd, POLLIN, 0};
        service_client(peer);
        if (send_out(peer) != 0)
        {
            peer->failed = 1;
            break;
        }
        if (peer->client && !closing && complete(peer))
        {
            closing = 1;
            submit_goaway(peer, 0);
            continue;
        }
        if (closing && peer->pending_bytes == 0U && !nghttp2_session_want_write(peer->session))
        {
            break;
        }
        if (peer->pending_bytes == 0U && !nghttp2_session_want_read(peer->session) &&
            !nghttp2_session_want_write(peer->session))
        {
            break; /* the session is over; the report says how far it came */
        }
        item.events = (short)(POLLIN | (peer->pending_bytes != 0U ? POLLOUT : 0));
        if (poll(&item, 1, 100) < 0 && errno != EINTR)
        {
            peer->failed = errno;
            break;
        }
        if ((item.revents & (POLLIN | POLLHUP | POLLERR)) != 0 && read_in(peer) != 0)
        {
            break;
        }
    }
    if (time(NULL) >= deadline)
    {
        fputs("crd-nghttp2-peer: the run outlasted its deadline\n", stderr);
        report(peer);
        return 1;
    }
    report(peer);
    close(peer->fd);
    return peer->failed != 0 ? 1 : 0;
}

static int usage(void)
{
    fputs("usage: crd-nghttp2-peer --role client|server --port N [--max-streams N] [--window N] [--frame-bytes N] "
          "[--padding 1] and the options of http_exchange.h\n",
          stderr);
    return 2;
}

int main(int argc, char** argv)
{
    static peer_t peer;
    int           role = 0;
    int           port = 0;
    httpx_default_options(&peer.options);
    peer.max_streams = 64U;
    peer.window      = 1U << 20U;
    peer.frame_bytes = 16384U;
    for (int at = 1; at + 1 < argc; at += 2)
    {
        const char* const name  = argv[at];
        const char* const value = argv[at + 1];
        const unsigned    size  = (unsigned)strtoul(value, NULL, 10);
        if (strcmp(name, "--role") == 0)
        {
            peer.client = strcmp(value, "client") == 0;
            role        = 1;
        }
        else if (strcmp(name, "--port") == 0)
        {
            peer.port = (int)size;
            port      = 1;
        }
        else if (strcmp(name, "--max-streams") == 0)
        {
            peer.max_streams = size;
        }
        else if (strcmp(name, "--window") == 0)
        {
            peer.window = size;
        }
        else if (strcmp(name, "--frame-bytes") == 0)
        {
            peer.frame_bytes = size;
        }
        else if (strcmp(name, "--padding") == 0)
        {
            peer.padding = size != 0U;
        }
        else if (httpx_parse_option(&peer.options, name, value) == 0)
        {
            return usage();
        }
    }
    if (argc % 2 == 0 || !role || !port || !httpx_options_valid(&peer.options))
    {
        return usage();
    }
    for (size_t at = 0U; at < sizeof(peer.fill); ++at)
    {
        peer.fill[at] = httpx_fill_byte(at);
    }
    return run(&peer);
}
