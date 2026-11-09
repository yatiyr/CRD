/* crd-net interoperability test (4.2a) -- the HTTP/3 exchange (http_exchange.h) over the pinned nghttp3. */
#include "ngtcp2_http3.h"

#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS 12U
#define VARINT_MAX_BYTES 8U

typedef struct
{
    httpx_section_t section;
    uint64_t        received;
    uint64_t        offered;
    int             used;
    int             tunnel_open;  /* a client's: its 200 has arrived */
    int             ended;        /* the peer's message ended */
    int             responded;    /* a server's */
    int             counted;      /* it is among the completed, rejected, aborted or cancelled */
    int             trailers;
    int             trailers_seen;
    int             early;
    int             closed; /* a counted stream QUIC has finished with: a reset of this end acknowledged */
    int             close_due; /* QUIC has finished and nghttp3 is still working through it */
    uint32_t        close_flags;
    uint64_t        close_rx_error;
    uint64_t        close_tx_error;
} stream_t;

struct http3_app
{
    const httpx_options_t* options;
    int                    client;
    uint64_t               per_tunnel; /* HTTP datagrams of one tunnel */
    int                    datagrams;
    ngtcp2_conn*           conn;
    nghttp3_conn*          h3;
    nghttp3_proto_settings peer;
    int                    settings_seen;
    int                    datagrams_settled;
    uint64_t               done_ns; /* when the client's part was first over */
    uint64_t               opened;
    uint64_t               seen;
    uint64_t               request_ended;
    uint64_t               completed;
    uint64_t               rejected;
    uint64_t               aborted;
    uint64_t               abort_code;
    uint64_t               cancelled;
    int64_t                cancel_due;
    int64_t                cancelled_id; /* -1 for none */
    uint64_t               status_431;
    uint64_t               refused_after_goaway;
    uint64_t               goaway_seen;
    uint64_t               goaway_id;
    uint64_t               goaway_sent;
    uint64_t               body_received;
    uint64_t               body_sent;
    uint64_t               interims;
    uint64_t               trailers;
    uint64_t               tunnels;
    uint64_t               early_requests;
    int                    unverified;
    stream_t               table[HTTPX_MAX_REQUESTS];
};

static char fill_text[HTTPX_MAX_FILL + 1U];

static stream_t* stream_of(http3_app_t* app, int64_t id)
{
    const uint64_t index = (uint64_t)id >> 2U;
    return (index < HTTPX_MAX_REQUESTS && (id & 3) == 0) ? &app->table[index] : NULL;
}

static uint64_t index_of(int64_t id)
{
    return (uint64_t)id >> 2U;
}

static nghttp3_nv field_of(const char* name, const char* value, size_t size, uint8_t flags)
{
    nghttp3_nv nv;
    nv.name     = (const uint8_t*)name;
    nv.value    = (const uint8_t*)value;
    nv.namelen  = strlen(name);
    nv.valuelen = size;
    nv.flags    = flags;
    return nv;
}

/* What every section of `index` carries. */
static size_t add_common(const httpx_options_t* options, uint64_t index, const char* index_text, nghttp3_nv* nva,
                         size_t count)
{
    const uint64_t fill = httpx_fill_bytes(options, index);
    nva[count++] = field_of("x-cerid-repeat", HTTPX_REPEAT_VALUE, strlen(HTTPX_REPEAT_VALUE), NGHTTP3_NV_FLAG_TRY_INDEX);
    nva[count++] = field_of("x-cerid-index", index_text, strlen(index_text), NGHTTP3_NV_FLAG_TRY_INDEX);
    if (fill != 0U)
    {
        nva[count++] = field_of("x-cerid-fill", fill_text, (size_t)fill, NGHTTP3_NV_FLAG_NONE);
    }
    return count;
}

static int submit_trailers(http3_app_t* app, int64_t id)
{
    char       index_text[24];
    nghttp3_nv nva[2];
    snprintf(index_text, sizeof(index_text), "%llu", (unsigned long long)index_of(id));
    nva[0] = field_of("x-cerid-trailer", index_text, strlen(index_text), NGHTTP3_NV_FLAG_NONE);
    nva[1] = field_of("x-cerid-repeat", HTTPX_REPEAT_VALUE, strlen(HTTPX_REPEAT_VALUE), NGHTTP3_NV_FLAG_TRY_INDEX);
    return nghttp3_conn_submit_trailers(app->h3, id, nva, 2U);
}

/* The body of a message: straight from the pattern table, which outlives what nghttp3 keeps until it is acknowledged. */
static nghttp3_ssize read_body(nghttp3_conn* h3, int64_t id, nghttp3_vec* vec, size_t count, uint32_t* flags, void* user,
                               void* stream_user)
{
    (void)h3;
    (void)count;
    (void)stream_user;
    http3_app_t*   app    = user;
    stream_t*      stream = stream_of(app, id);
    const uint64_t index  = index_of(id);
    if (stream == NULL || count == 0U)
    {
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    /* A tunnel's client FIN waits for its HTTP datagrams, which need the send part open. */
    if (httpx_is_tunnel(app->options, index) &&
        (app->client ? !stream->tunnel_open || !app->datagrams_settled : !stream->ended))
    {
        return NGHTTP3_ERR_WOULDBLOCK;
    }
    const uint64_t goal   = httpx_body_bytes(app->options, index, !app->client);
    const size_t   at     = pattern_index(id, stream->offered);
    const uint64_t left   = goal - stream->offered;
    const size_t   amount = left < (PATTERN_BYTES - at) ? (size_t)left : (PATTERN_BYTES - at);
    stream->offered += amount;
    app->body_sent += amount;
    if (stream->offered == goal)
    {
        *flags |= NGHTTP3_DATA_FLAG_EOF;
        if (stream->trailers)
        {
            *flags |= NGHTTP3_DATA_FLAG_NO_END_STREAM;
            if (submit_trailers(app, id) != 0)
            {
                return NGHTTP3_ERR_CALLBACK_FAILURE;
            }
        }
    }
    if (amount == 0U)
    {
        return 0;
    }
    vec[0].base = &pattern[app->client ? 0 : 1][at];
    vec[0].len  = amount;
    return 1;
}

static const nghttp3_data_reader BODY_READER = {read_body};

static int respond(http3_app_t* app, int64_t id, stream_t* stream)
{
    const uint64_t index = index_of(id);
    const uint64_t body  = httpx_body_bytes(app->options, index, 1);
    char           index_text[24];
    char           length_text[24];
    nghttp3_nv     nva[MAX_FIELDS];
    size_t         count = 0U;
    stream->responded    = 1;
    snprintf(index_text, sizeof(index_text), "%llu", (unsigned long long)index);
    snprintf(length_text, sizeof(length_text), "%llu", (unsigned long long)body);
    if (app->options->interim)
    {
        nva[0] = field_of(":status", "103", 3U, NGHTTP3_NV_FLAG_NONE);
        nva[1] = field_of("link", "</cerid.css>; rel=preload", 25U, NGHTTP3_NV_FLAG_NONE);
        if (nghttp3_conn_submit_info(app->h3, id, nva, 2U) != 0)
        {
            return 0;
        }
    }
    nva[count++] = field_of(":status", "200", 3U, NGHTTP3_NV_FLAG_NONE);
    count        = add_common(app->options, index, index_text, nva, count);
    if (httpx_has_length(app->options, index, body))
    {
        nva[count++] = field_of("content-length", length_text, strlen(length_text), NGHTTP3_NV_FLAG_NONE);
    }
    stream->trailers = httpx_has_trailers(app->options, index);
    const int needs_reader = body != 0U || stream->trailers || httpx_is_tunnel(app->options, index);
    ++app->completed;
    return nghttp3_conn_submit_response(app->h3, id, nva, count, needs_reader ? &BODY_READER : NULL) == 0;
}

static int respond_waiting(http3_app_t* app)
{
    for (uint64_t index = 0; index < HTTPX_MAX_REQUESTS; ++index)
    {
        stream_t* stream = &app->table[index];
        if (stream->used && stream->ended && !stream->responded && !stream->counted &&
            !httpx_is_tunnel(app->options, index) && !respond(app, (int64_t)(index * 4U), stream))
        {
            return 0;
        }
    }
    return 1;
}

static int begin_headers(nghttp3_conn* h3, int64_t id, void* user, void* stream_user)
{
    (void)h3;
    (void)stream_user;
    stream_t* stream = stream_of(user, id);
    if (stream == NULL)
    {
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    stream->used = 1;
    memset(&stream->section, 0, sizeof(stream->section));
    return 0;
}

static int recv_header(nghttp3_conn* h3, int64_t id, int32_t token, nghttp3_rcbuf* name, nghttp3_rcbuf* value,
                       uint8_t flags, void* user, void* stream_user)
{
    (void)h3;
    (void)token;
    (void)flags;
    (void)stream_user;
    http3_app_t*     app        = user;
    stream_t*        stream     = stream_of(app, id);
    const nghttp3_vec name_vec  = nghttp3_rcbuf_get_buf(name);
    const nghttp3_vec value_vec = nghttp3_rcbuf_get_buf(value);
    httpx_take_field(&stream->section, app->options, index_of(id), name_vec.base, name_vec.len, value_vec.base,
                     value_vec.len);
    return 0;
}

static int recv_trailer(nghttp3_conn* h3, int64_t id, int32_t token, nghttp3_rcbuf* name, nghttp3_rcbuf* value,
                        uint8_t flags, void* user, void* stream_user)
{
    (void)h3;
    (void)token;
    (void)flags;
    (void)stream_user;
    http3_app_t*      app       = user;
    stream_t*         stream    = stream_of(app, id);
    const nghttp3_vec name_vec  = nghttp3_rcbuf_get_buf(name);
    const nghttp3_vec value_vec = nghttp3_rcbuf_get_buf(value);
    if (httpx_text_is(name_vec.base, name_vec.len, "x-cerid-trailer"))
    {
        stream->trailers_seen = httpx_decimal(value_vec.base, value_vec.len) == index_of(id);
    }
    return 0;
}

/* A section of a request: the first one's GOAWAY, and a tunnel opens at once. */
static int request_headers(http3_app_t* app, int64_t id, stream_t* stream)
{
    const uint64_t index = index_of(id);
    if (!httpx_section_good(&stream->section, app->options, index, 0))
    {
        app->unverified = 1;
    }
    ++app->seen;
    app->early_requests += stream->early ? 1U : 0U;
    if (app->seen == 1U && app->options->goaway == HTTPX_GOAWAY_NOTICE &&
        nghttp3_conn_submit_shutdown_notice(app->h3) != 0)
    {
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    if (app->seen == 1U && app->options->goaway == HTTPX_GOAWAY_REJECT && nghttp3_conn_shutdown(app->h3) != 0)
    {
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    if (app->options->cancel == HTTPX_CANCEL_SERVER && httpx_last_request(app->options, index))
    {
        app->cancel_due = id;
        return 0;
    }
    if (httpx_is_tunnel(app->options, index))
    {
        ++app->tunnels;
        return respond(app, id, stream) ? 0 : NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int end_headers(nghttp3_conn* h3, int64_t id, int fin, void* user, void* stream_user)
{
    (void)fin;
    (void)stream_user;
    http3_app_t* app    = user;
    stream_t*    stream = stream_of(app, id);
    if (!app->client)
    {
        return request_headers(app, id, stream);
    }
    if (stream->section.status >= 100U && stream->section.status < 200U)
    {
        ++app->interims;
        return 0;
    }
    if (httpx_is_tunnel(app->options, index_of(id)) && stream->section.status == 200U)
    {
        stream->tunnel_open = 1;
        ++app->tunnels;
        return nghttp3_conn_resume_stream(h3, id) == 0 ? 0 : NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int recv_data(nghttp3_conn* h3, int64_t id, const uint8_t* data, size_t size, void* user, void* stream_user)
{
    (void)h3;
    (void)stream_user;
    http3_app_t*   app      = user;
    stream_t*      stream   = stream_of(app, id);
    const uint8_t* expected = pattern[app->client ? 1 : 0];
    for (size_t at = 0; at < size; ++at)
    {
        if (data[at] != expected[pattern_index(id, stream->received + at)])
        {
            app->unverified = 1;
        }
    }
    stream->received += size;
    app->body_received += size;
    if (app->client && app->options->cancel == HTTPX_CANCEL_CLIENT && httpx_last_request(app->options, index_of(id)) &&
        size != 0U && !stream->counted)
    {
        app->cancel_due = id;
    }
    ngtcp2_conn_extend_max_stream_offset(app->conn, id, size);
    ngtcp2_conn_extend_max_offset(app->conn, size);
    return 0;
}

static int deferred_consume(nghttp3_conn* h3, int64_t id, size_t size, void* user, void* stream_user)
{
    (void)h3;
    (void)stream_user;
    http3_app_t* app = user;
    ngtcp2_conn_extend_max_stream_offset(app->conn, id, size);
    ngtcp2_conn_extend_max_offset(app->conn, size);
    return 0;
}

/* A client's response has ended: held to what the exchange wrote. */
static void response_ended(http3_app_t* app, int64_t id, stream_t* stream)
{
    const uint64_t index = index_of(id);
    stream->counted      = 1;
    ++app->completed;
    if (app->options->over_limit && httpx_last_request(app->options, index) && stream->section.status == 431U)
    {
        ++app->status_431;
        return;
    }
    if (!httpx_section_good(&stream->section, app->options, index, 1) ||
        stream->received != httpx_body_bytes(app->options, index, 1) ||
        (httpx_has_trailers(app->options, index) && !stream->trailers_seen))
    {
        app->unverified = 1;
    }
    app->trailers += stream->trailers_seen ? 1U : 0U;
}

/* A server's request has ended: the response goes, after the GOAWAYs when they come first. */
static int request_ended(http3_app_t* app, int64_t id, stream_t* stream)
{
    const uint64_t index = index_of(id);
    if (stream->received != httpx_body_bytes(app->options, index, 0) ||
        (httpx_has_trailers(app->options, index) && !stream->trailers_seen))
    {
        app->unverified = 1;
    }
    app->trailers += stream->trailers_seen ? 1U : 0U;
    ++app->request_ended;
    if (app->options->goaway == HTTPX_GOAWAY_NOTICE && app->request_ended == app->options->requests &&
        nghttp3_conn_shutdown(app->h3) != 0)
    {
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    if (httpx_is_tunnel(app->options, index))
    {
        return nghttp3_conn_resume_stream(app->h3, id) == 0 ? 0 : NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    if (app->options->goaway == HTTPX_GOAWAY_CLIENT && app->goaway_seen < 2U)
    {
        return 0;
    }
    return respond(app, id, stream) ? 0 : NGHTTP3_ERR_CALLBACK_FAILURE;
}

static int end_stream(nghttp3_conn* h3, int64_t id, void* user, void* stream_user)
{
    (void)h3;
    (void)stream_user;
    http3_app_t* app    = user;
    stream_t*    stream = stream_of(app, id);
    stream->ended       = 1;
    if (app->client)
    {
        response_ended(app, id, stream);
        return 0;
    }
    return request_ended(app, id, stream);
}

static int stop_sending(nghttp3_conn* h3, int64_t id, uint64_t code, void* user, void* stream_user)
{
    (void)h3;
    (void)stream_user;
    const int result = ngtcp2_conn_shutdown_stream_read(((http3_app_t*)user)->conn, 0U, id, code);
    return result == 0 || result == NGTCP2_ERR_STREAM_NOT_FOUND ? 0 : NGHTTP3_ERR_CALLBACK_FAILURE;
}

static int reset_stream(nghttp3_conn* h3, int64_t id, uint64_t code, void* user, void* stream_user)
{
    (void)h3;
    (void)stream_user;
    const int result = ngtcp2_conn_shutdown_stream_write(((http3_app_t*)user)->conn, 0U, id, code);
    return result == 0 || result == NGTCP2_ERR_STREAM_NOT_FOUND ? 0 : NGHTTP3_ERR_CALLBACK_FAILURE;
}

static int shutdown_received(nghttp3_conn* h3, int64_t id, void* user)
{
    (void)h3;
    http3_app_t* app = user;
    ++app->goaway_seen;
    app->goaway_id = (uint64_t)id;
    if (!app->client && app->options->goaway == HTTPX_GOAWAY_CLIENT && app->goaway_seen == 2U)
    {
        return respond_waiting(app) ? 0 : NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int recv_settings(nghttp3_conn* h3, const nghttp3_proto_settings* settings, void* user)
{
    (void)h3;
    http3_app_t* app   = user;
    app->peer          = *settings;
    app->settings_seen = 1;
    return 0;
}

static void default_h3_callbacks(nghttp3_callbacks* callbacks)
{
    memset(callbacks, 0, sizeof(*callbacks));
    callbacks->recv_data        = recv_data;
    callbacks->deferred_consume = deferred_consume;
    callbacks->begin_headers    = begin_headers;
    callbacks->recv_header      = recv_header;
    callbacks->end_headers      = end_headers;
    callbacks->recv_trailer     = recv_trailer;
    callbacks->stop_sending     = stop_sending;
    callbacks->end_stream       = end_stream;
    callbacks->reset_stream     = reset_stream;
    callbacks->shutdown         = shutdown_received;
    callbacks->recv_settings2   = recv_settings;
}

static int start(http3_app_t* app, ngtcp2_conn* conn)
{
    if (app->h3 != NULL)
    {
        return 1;
    }
    app->conn = conn;
    nghttp3_callbacks callbacks;
    nghttp3_settings  settings;
    int64_t           control = -1;
    int64_t           encoder = -1;
    int64_t           decoder = -1;
    default_h3_callbacks(&callbacks);
    nghttp3_settings_default(&settings);
    settings.max_field_section_size            = app->options->field_limit;
    settings.qpack_max_dtable_capacity         = (size_t)app->options->table;
    settings.qpack_encoder_max_dtable_capacity = (size_t)app->options->table;
    settings.qpack_blocked_streams             = (size_t)app->options->blocked;
    settings.enable_connect_protocol           = app->client ? 0U : 1U;
    settings.h3_datagram                       = app->datagrams ? 1U : 0U;
    const int made = app->client ? nghttp3_conn_client_new(&app->h3, &callbacks, &settings, nghttp3_mem_default(), app)
                                 : nghttp3_conn_server_new(&app->h3, &callbacks, &settings, nghttp3_mem_default(), app);
    if (made != 0)
    {
        return 0;
    }
    if (!app->client)
    {
        nghttp3_conn_set_max_client_streams_bidi(app->h3, ngtcp2_conn_get_streams_bidi_left(conn));
    }
    return ngtcp2_conn_open_uni_stream(conn, &control, NULL) == 0 &&
           nghttp3_conn_bind_control_stream(app->h3, control) == 0 &&
           ngtcp2_conn_open_uni_stream(conn, &encoder, NULL) == 0 &&
           ngtcp2_conn_open_uni_stream(conn, &decoder, NULL) == 0 &&
           nghttp3_conn_bind_qpack_streams(app->h3, encoder, decoder) == 0;
}

/* The request of `index` on `id`, which the client has opened. 0 when nghttp3 refused it. */
static int submit_request(http3_app_t* app, uint64_t index, int64_t id, void* stream_user)
{
    const httpx_options_t* options = app->options;
    const uint64_t         body    = httpx_body_bytes(options, index, 0);
    const int              tunnel  = httpx_is_tunnel(options, index);
    char                   index_text[24];
    char                   length_text[24];
    char                   path[40];
    nghttp3_nv             nva[MAX_FIELDS];
    size_t                 count = 0U;
    snprintf(index_text, sizeof(index_text), "%llu", (unsigned long long)index);
    snprintf(length_text, sizeof(length_text), "%llu", (unsigned long long)body);
    snprintf(path, sizeof(path), tunnel ? "/tunnel/%llu" : "/cerid/%llu", (unsigned long long)index);
    const char* method = httpx_method(tunnel, body);
    nva[count++]       = field_of(":method", method, strlen(method), NGHTTP3_NV_FLAG_NONE);
    nva[count++]       = field_of(":scheme", "https", 5U, NGHTTP3_NV_FLAG_NONE);
    nva[count++]       = field_of(":authority", HTTPX_SERVER_NAME, strlen(HTTPX_SERVER_NAME), NGHTTP3_NV_FLAG_NONE);
    nva[count++]       = field_of(":path", path, strlen(path), NGHTTP3_NV_FLAG_NONE);
    if (tunnel)
    {
        nva[count++] = field_of(":protocol", HTTPX_TUNNEL_PROTOCOL, strlen(HTTPX_TUNNEL_PROTOCOL), NGHTTP3_NV_FLAG_NONE);
    }
    count = add_common(options, index, index_text, nva, count);
    if (httpx_has_length(options, index, body) && body != 0U)
    {
        nva[count++] = field_of("content-length", length_text, strlen(length_text), NGHTTP3_NV_FLAG_NONE);
    }
    const int needs_reader = body != 0U || httpx_has_trailers(options, index) || tunnel;
    return nghttp3_conn_submit_request(app->h3, id, nva, count, needs_reader ? &BODY_READER : NULL, stream_user) == 0;
}

/* Opens what the stream limits allow. 0 when something else refused. */
static int open_requests(http3_app_t* app)
{
    while (app->opened < app->options->requests)
    {
        int64_t id = -1;
        if (ngtcp2_conn_open_bidi_stream(app->conn, &id, NULL) != 0)
        {
            return 1;
        }
        stream_t* stream = stream_of(app, id);
        if (stream == NULL)
        {
            return 0;
        }
        stream->used     = 1;
        stream->trailers = httpx_has_trailers(app->options, index_of(id));
        if (!submit_request(app, index_of(id), id, stream))
        {
            return 0;
        }
        ++app->opened;
    }
    return 1;
}

static int close_due_streams(http3_app_t* app);

static void cancel_stream(http3_app_t* app)
{
    if (app->cancel_due < 0)
    {
        return;
    }
    stream_t* stream = stream_of(app, app->cancel_due);
    if (!stream->counted)
    {
        stream->counted = 1;
        ++app->cancelled;
        app->cancelled_id = app->cancel_due;
        nghttp3_conn_shutdown_stream_write(app->h3, app->cancel_due);
        (void)ngtcp2_conn_shutdown_stream(app->conn, 0U, app->cancel_due, HTTP3_CANCEL_CODE);
    }
    app->cancel_due = -1;
}

/* After a GOAWAY a client's new request is refused by its own end, before anything is written. */
static void probe_after_goaway(http3_app_t* app)
{
    if (app->client && app->options->goaway == HTTPX_GOAWAY_NOTICE && app->goaway_seen >= 2U &&
        app->refused_after_goaway == 0U && app->opened == app->options->requests)
    {
        app->refused_after_goaway =
            submit_request(app, app->opened, (int64_t)(app->opened * 4U), NULL) == 0 ? 1U : 0U;
    }
}

int http3_turn(http3_app_t* app, ngtcp2_conn* conn, int can_open, int early, int datagrams_settled)
{
    if (!can_open)
    {
        return 1;
    }
    if (!start(app, conn))
    {
        return 0;
    }
    if (datagrams_settled && !app->datagrams_settled)
    {
        app->datagrams_settled = 1;
        for (uint64_t index = 0; index < app->options->tunnels; ++index)
        {
            (void)nghttp3_conn_resume_stream(app->h3, (int64_t)(index * 4U));
        }
    }
    if ((app->client && (app->settings_seen || early) && !open_requests(app)) || !close_due_streams(app))
    {
        return 0;
    }
    cancel_stream(app);
    probe_after_goaway(app);
    if (app->client && app->options->goaway == HTTPX_GOAWAY_CLIENT && app->opened == app->options->requests &&
        app->goaway_sent == 0U)
    {
        /* Both GOAWAYs go together: the notice first, its value the larger. */
        app->goaway_sent = 2U;
        return nghttp3_conn_submit_shutdown_notice(app->h3) == 0 && nghttp3_conn_shutdown(app->h3) == 0;
    }
    return 1;
}

int http3_recv_stream_data(http3_app_t* app, ngtcp2_conn* conn, uint32_t flags, int64_t id, const uint8_t* data,
                           size_t size, uint64_t now)
{
    if (!start(app, conn))
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    stream_t* stream = stream_of(app, id);
    if (stream != NULL && (flags & NGTCP2_STREAM_DATA_FLAG_0RTT) != 0U)
    {
        stream->early = 1;
    }
    const int           fin      = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0U;
    const nghttp3_ssize consumed = nghttp3_conn_read_stream2(app->h3, id, data, size, fin, now);
    if (consumed < 0)
    {
        fprintf(stderr, "nghttp3_conn_read_stream2: %s\n", nghttp3_strerror((int)consumed));
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    ngtcp2_conn_extend_max_stream_offset(conn, id, (uint64_t)consumed);
    ngtcp2_conn_extend_max_offset(conn, (uint64_t)consumed);
    return 0;
}

int http3_acked(http3_app_t* app, int64_t id, uint64_t size)
{
    if (app->h3 == NULL)
    {
        return 0;
    }
    const int result = nghttp3_conn_add_ack_offset(app->h3, id, size);
    return result == 0 ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}

static void count_abort(http3_app_t* app, int64_t id, uint64_t code);

static int close_h3_stream(http3_app_t* app, uint32_t flags, int64_t id, uint64_t rx_error, uint64_t tx_error)
{
    uint32_t h3_flags = NGHTTP3_STREAM_CLOSE_FLAG_NONE;
    h3_flags |= (flags & NGTCP2_STREAM_CLOSE2_FLAG_RX_APP_ERROR_CODE_SET) != 0U
                    ? NGHTTP3_STREAM_CLOSE_FLAG_RX_APP_ERROR_CODE_SET
                    : 0U;
    h3_flags |= (flags & NGTCP2_STREAM_CLOSE2_FLAG_TX_APP_ERROR_CODE_SET) != 0U
                    ? NGHTTP3_STREAM_CLOSE_FLAG_TX_APP_ERROR_CODE_SET
                    : 0U;
    const int result = nghttp3_conn_close_stream2(app->h3, h3_flags, id, rx_error, tx_error);
    if (result != 0 && result != NGHTTP3_ERR_STREAM_NOT_FOUND)
    {
        fprintf(stderr, "nghttp3_conn_close_stream2: %s\n", nghttp3_strerror(result));
        return 0;
    }
    return 1;
}

/* Streams whose close waited for a section QPACK held. */
static int close_due_streams(http3_app_t* app)
{
    for (uint64_t index = 0; index < HTTPX_MAX_REQUESTS; ++index)
    {
        stream_t* stream = &app->table[index];
        if (stream->close_due && stream->ended)
        {
            stream->close_due = 0;
            if (!close_h3_stream(app, stream->close_flags, (int64_t)(index * 4U), stream->close_rx_error,
                                 stream->close_tx_error))
            {
                return 0;
            }
        }
    }
    return 1;
}

int http3_stream_close(http3_app_t* app, uint32_t flags, int64_t id, uint64_t rx_error, uint64_t tx_error)
{
    if (app->h3 == NULL)
    {
        return 0;
    }
    stream_t* stream = (id & 3) == 0 ? stream_of(app, id) : NULL;
    if ((flags & NGTCP2_STREAM_CLOSE2_FLAG_TX_APP_ERROR_CODE_SET) != 0U)
    {
        count_abort(app, id, tx_error);
    }
    /* ngtcp2 calls a stream closed once it has delivered the last byte; nghttp3 deletes a closed stream with whatever a
     * QPACK-blocked section still holds, so that close waits for the message's end. */
    if (stream != NULL && stream->used && !stream->ended && !stream->counted &&
        (flags & NGTCP2_STREAM_CLOSE2_FLAG_RX_APP_ERROR_CODE_SET) == 0U)
    {
        stream->close_due      = 1;
        stream->close_flags    = flags;
        stream->close_rx_error = rx_error;
        stream->close_tx_error = tx_error;
    }
    else if (!close_h3_stream(app, flags, id, rx_error, tx_error))
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    if ((id & 3) != 0)
    {
        return 0;
    }
    if (stream != NULL && stream->counted)
    {
        stream->closed = 1;
    }
    if (!app->client)
    {
        ngtcp2_conn_extend_max_streams_bidi(app->conn, 1U);
    }
    return 0;
}

/* The peer's RESET_STREAM, or the reset its STOP_SENDING made QUIC send: a request it cancelled or rejected. */
static void count_abort(http3_app_t* app, int64_t id, uint64_t code)
{
    stream_t* stream = (id & 3) == 0 ? stream_of(app, id) : NULL;
    if (stream == NULL || stream->counted || code == HTTP3_NO_ERROR)
    {
        return;
    }
    stream->counted = 1;
    if (code == HTTP3_REJECTED_CODE)
    {
        ++app->rejected;
    }
    else
    {
        ++app->aborted;
        app->abort_code = code;
    }
}

int http3_stream_stop(http3_app_t* app, int64_t id)
{
    if (app->h3 == NULL)
    {
        return 0;
    }
    return nghttp3_conn_shutdown_stream_read(app->h3, id) == 0 ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}

int http3_stream_reset(http3_app_t* app, int64_t id, uint64_t code)
{
    count_abort(app, id, code);
    return http3_stream_stop(app, id);
}

int http3_unblock(http3_app_t* app, int64_t id)
{
    if (app->h3 == NULL)
    {
        return 0;
    }
    const int result = nghttp3_conn_unblock_stream(app->h3, id);
    return result == 0 || result == NGHTTP3_ERR_STREAM_NOT_FOUND ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}

void http3_set_max_bidi(http3_app_t* app, uint64_t maximum)
{
    if (app->h3 != NULL)
    {
        nghttp3_conn_set_max_client_streams_bidi(app->h3, maximum);
    }
}

nghttp3_ssize http3_writev(http3_app_t* app, int64_t* id, int* fin, nghttp3_vec* vec, size_t count)
{
    *id  = -1;
    *fin = 0;
    return app->h3 != NULL ? nghttp3_conn_writev_stream(app->h3, id, fin, vec, count) : 0;
}

void http3_wrote(http3_app_t* app, int64_t id, size_t size)
{
    if (id >= 0)
    {
        (void)nghttp3_conn_add_write_offset(app->h3, id, size);
    }
}

void http3_block(http3_app_t* app, int64_t id)
{
    nghttp3_conn_block_stream(app->h3, id);
}

void http3_shut_write(http3_app_t* app, int64_t id)
{
    nghttp3_conn_shutdown_stream_write(app->h3, id);
}

/* RFC 9297 section 2 over RFC 9000's variable-length integer: the quarter stream IDs here fit two bytes. */
size_t http3_datagram_prefix(const http3_app_t* app, uint32_t index, uint8_t* out)
{
    const uint64_t tunnel = index / app->per_tunnel;
    if (tunnel < 64U)
    {
        out[0] = (uint8_t)tunnel;
        return 1U;
    }
    out[0] = (uint8_t)(0x40U | (tunnel >> 8U));
    out[1] = (uint8_t)tunnel;
    return 2U;
}

int http3_datagram_ready(const http3_app_t* app, uint32_t index)
{
    return app->table[index / app->per_tunnel].tunnel_open;
}

int http3_strip_datagram(const http3_app_t* app, const uint8_t** data, size_t* size)
{
    if (*size == 0U)
    {
        return 0;
    }
    const size_t prefix = (size_t)1U << ((*data)[0] >> 6U);
    if (prefix > VARINT_MAX_BYTES || *size < prefix + 4U)
    {
        return 0;
    }
    uint64_t quarter = (uint64_t)((*data)[0] & 0x3FU);
    for (size_t at = 1U; at < prefix; ++at)
    {
        quarter = (quarter << 8U) | (*data)[at];
    }
    *data += prefix;
    *size -= prefix;
    uint32_t index = 0U;
    for (size_t at = 0; at < 4U; ++at)
    {
        index |= (uint32_t)(*data)[at] << (at * 8U);
    }
    return quarter == index / app->per_tunnel;
}

http3_app_t* http3_new(const httpx_options_t* options, int client, uint64_t datagrams)
{
    http3_app_t* app = calloc(1U, sizeof(http3_app_t));
    if (app == NULL)
    {
        return NULL;
    }
    app->options    = options;
    app->client     = client;
    app->cancel_due = -1;
    app->cancelled_id = -1;
    app->datagrams  = datagrams != 0U;
    app->per_tunnel = options->tunnels != 0U ? datagrams / options->tunnels : 1U;
    app->per_tunnel = app->per_tunnel != 0U ? app->per_tunnel : 1U;
    for (size_t at = 0; at < HTTPX_MAX_FILL; ++at)
    {
        fill_text[at] = httpx_fill_byte(at);
    }
    return app;
}

void http3_del(http3_app_t* app)
{
    if (app != NULL)
    {
        nghttp3_conn_del(app->h3);
        free(app);
    }
}

static int http3_exchange_over(const http3_app_t* app)
{
    const httpx_options_t* options = app->options;
    if (!app->client || !app->settings_seen || app->opened < options->requests ||
        app->completed + app->rejected + app->aborted + app->cancelled < options->requests ||
        (app->cancelled_id >= 0 && !app->table[index_of(app->cancelled_id)].closed))
    {
        return 0;
    }
    switch (options->goaway)
    {
    case HTTPX_GOAWAY_NOTICE:
        return app->goaway_seen >= 2U && app->refused_after_goaway != 0U;
    case HTTPX_GOAWAY_REJECT:
        return app->goaway_seen >= 1U;
    default:
        return 1;
    }
}

int http3_done(http3_app_t* app, uint64_t now)
{
    if (!http3_exchange_over(app))
    {
        return 0;
    }
    app->done_ns = app->done_ns != 0U ? app->done_ns : now;
    return now - app->done_ns >= HTTPX_LINGER_NS;
}

void http3_report(const http3_app_t* app)
{
    printf("h3_requests=%llu\nh3_seen=%llu\nh3_completed=%llu\nh3_rejected=%llu\nh3_aborted=%llu\nh3_abort_code=%llu\n",
           (unsigned long long)app->opened, (unsigned long long)app->seen, (unsigned long long)app->completed,
           (unsigned long long)app->rejected, (unsigned long long)app->aborted, (unsigned long long)app->abort_code);
    printf("h3_cancelled=%llu\nh3_status_431=%llu\nh3_refused_too_large=0\nh3_refused_after_goaway=%llu\n"
           "h3_goaway_seen=%llu\nh3_goaway_id=%llu\n",
           (unsigned long long)app->cancelled, (unsigned long long)app->status_431,
           (unsigned long long)app->refused_after_goaway, (unsigned long long)app->goaway_seen,
           (unsigned long long)app->goaway_id);
    printf("h3_body_received=%llu\nh3_body_sent=%llu\nh3_interims=%llu\nh3_trailers=%llu\nh3_tunnels=%llu\n"
           "h3_early_requests=%llu\nh3_verified=%d\n",
           (unsigned long long)app->body_received, (unsigned long long)app->body_sent,
           (unsigned long long)app->interims, (unsigned long long)app->trailers, (unsigned long long)app->tunnels,
           (unsigned long long)app->early_requests, !app->unverified);
    printf("h3_peer_known=%d\nh3_peer_table=%llu\nh3_peer_blocked=%llu\nh3_peer_field_limit=%llu\nh3_peer_connect=%d\n"
           "h3_peer_datagram=%d\n",
           app->settings_seen, (unsigned long long)app->peer.qpack_max_dtable_capacity,
           (unsigned long long)app->peer.qpack_blocked_streams, (unsigned long long)app->peer.max_field_section_size,
           app->peer.enable_connect_protocol, app->peer.h3_datagram);
}
