/* crd-net interoperability test (4.2a) -- the HTTP exchange crd-quic-peer and crd-ngtcp2-quic-peer run with
 * --http3 1, and crd-http2-peer and crd-nghttp2-peer over TCP: its options and the fields it carries, in one place so
 * that the programs read and write the same thing. Plain C: the ngtcp2 and nghttp2 peers are C programs.
 *
 * A client opens `requests` requests, the first `tunnels` of them Extended CONNECT tunnels (`:protocol` cerid-tunnel). A
 * request carries `request-bytes` bytes of body, a response `response-bytes`, each in the pattern of its stream, and
 * the index of a stream is its ID over four ((ID - 1) / 2 on HTTP/2). Every message carries a field that repeats, so a
 * dynamic table has something to index, its index, and `field-bytes` bytes of filler. A server answers when the request
 * has ended, a tunnel as soon as it opens, its body still waiting for the request's end. Both ends check every field and
 * every byte.
 *
 *   table, blocked  the QPACK or HPACK table capacity, and the blocked streams (QPACK), each end allows the other
 *   field-limit     the largest field section each end takes
 *   trailers        each message but a tunnel's (only DATA may follow its opening) ends in trailers
 *   interim         a server sends a 103 before each response
 *   datagrams       HTTP datagrams over the tunnels (RFC 9297 section 2): the transport's --datagrams is their total, and
 *                   each carries the quarter stream ID of its tunnel, which datagram index / per-tunnel count names
 *   goaway          notice: a server's GOAWAY of the largest ID at the first request, a second naming the next request
 *                   when every request has ended, then a client tries one more request, refused by its own end
 *                   reject: a server's first GOAWAY names the next request at the first request's headers; requests
 *                   from there on are rejected, so completed + rejected = requests and completed = its ID / 4
 *                   client: a client sends both of its GOAWAYs after opening every request; responses wait for them
 *   cancel          client: a client cancels the last request at its first response byte, while the server writes on
 *                   server: a server cancels the last request at its headers
 *   over-limit      a client's last request carries a section over its peer's field limit; refused there or answered 431
 *
 * The report of each connection adds these to the transport's, the same from both programs. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HTTPX_MAX_REQUESTS 512U
#define HTTPX_MAX_FILL 48000U
#define HTTP3_CANCEL_CODE 0x10CU       /* H3_REQUEST_CANCELLED */
#define HTTP3_REJECTED_CODE 0x10BU     /* H3_REQUEST_REJECTED */
#define HTTP3_NO_ERROR 0x100U
#define HTTP3_ALPN "h3"
#define HTTPX_TUNNEL_PROTOCOL "cerid-tunnel"
#define HTTPX_SERVER_NAME "cerid.test"
#define HTTPX_REPEAT_VALUE "a field value every message of the exchange repeats"
#define HTTPX_DATA_FRAME_BYTES 8192U
#define HTTPX_LINGER_NS 100000000U /* a client's wait before closing, for QPACK's last acknowledgements to arrive */

enum
{
    HTTPX_GOAWAY_NONE,
    HTTPX_GOAWAY_NOTICE,
    HTTPX_GOAWAY_REJECT,
    HTTPX_GOAWAY_CLIENT
};

enum
{
    HTTPX_CANCEL_NONE,
    HTTPX_CANCEL_CLIENT,
    HTTPX_CANCEL_SERVER
};

typedef struct
{
    int      enabled;
    uint64_t requests;
    uint64_t request_bytes;
    uint64_t response_bytes;
    uint64_t tunnels;
    uint64_t table;
    uint64_t blocked;
    uint64_t field_bytes;
    uint64_t field_limit;
    int      trailers;
    int      interim;
    int      goaway;
    int      cancel;
    int      over_limit;
} httpx_options_t;

/* What one received section held. */
typedef struct
{
    uint64_t index;
    uint32_t status;
    int      index_seen;
    int      repeat_seen;
    int      fill_seen;
    int      protocol_seen;
    int      connect;
    int      get;
    int      post;
    int      wrong;
} httpx_section_t;

static inline void httpx_default_options(httpx_options_t* options)
{
    memset(options, 0, sizeof(*options));
    options->field_limit = 16384U;
    options->table = 4096U;
    options->blocked = 16U;
}

/* Whether `name` was one of the exchange's. */
static inline int httpx_parse_option(httpx_options_t* options, const char* name, const char* value)
{
    const uint64_t number = strtoull(value, NULL, 10);
    if (strcmp(name, "--http3") == 0)
    {
        options->enabled = number != 0U;
    }
    else if (strcmp(name, "--requests") == 0)
    {
        options->requests = number;
    }
    else if (strcmp(name, "--request-bytes") == 0)
    {
        options->request_bytes = number;
    }
    else if (strcmp(name, "--response-bytes") == 0)
    {
        options->response_bytes = number;
    }
    else if (strcmp(name, "--tunnels") == 0)
    {
        options->tunnels = number;
    }
    else if (strcmp(name, "--table") == 0)
    {
        options->table = number;
    }
    else if (strcmp(name, "--blocked") == 0)
    {
        options->blocked = number;
    }
    else if (strcmp(name, "--field-bytes") == 0)
    {
        options->field_bytes = number;
    }
    else if (strcmp(name, "--field-limit") == 0)
    {
        options->field_limit = number;
    }
    else if (strcmp(name, "--trailers") == 0)
    {
        options->trailers = number != 0U;
    }
    else if (strcmp(name, "--interim") == 0)
    {
        options->interim = number != 0U;
    }
    else if (strcmp(name, "--over-limit") == 0)
    {
        options->over_limit = number != 0U;
    }
    else if (strcmp(name, "--goaway") == 0)
    {
        options->goaway = strcmp(value, "notice") == 0   ? HTTPX_GOAWAY_NOTICE
                          : strcmp(value, "reject") == 0 ? HTTPX_GOAWAY_REJECT
                          : strcmp(value, "client") == 0 ? HTTPX_GOAWAY_CLIENT
                                                         : HTTPX_GOAWAY_NONE;
    }
    else if (strcmp(name, "--cancel") == 0)
    {
        options->cancel = strcmp(value, "client") == 0   ? HTTPX_CANCEL_CLIENT
                          : strcmp(value, "server") == 0 ? HTTPX_CANCEL_SERVER
                                                         : HTTPX_CANCEL_NONE;
    }
    else
    {
        return 0;
    }
    return 1;
}

static inline int httpx_options_valid(const httpx_options_t* options)
{
    return options->requests <= HTTPX_MAX_REQUESTS && options->tunnels <= options->requests &&
           options->field_bytes <= HTTPX_MAX_FILL && options->field_limit >= 1024U &&
           options->field_limit * 2U <= HTTPX_MAX_FILL;
}

/* Byte `offset` of the body of `stream` in the pattern of `tag`. */
static inline uint8_t httpx_pattern(uint64_t tag, uint64_t stream, uint64_t offset)
{
    const uint64_t at = (offset + (stream * 37U)) & 0xFFFFU;
    return (uint8_t)((at * 31U) + (at >> 8U) + (tag * 7U));
}

static inline char httpx_fill_byte(uint64_t at)
{
    return (char)('a' + (at % 26U));
}

/* What a request's :method is. */
static inline const char* httpx_method(int tunnel, uint64_t body)
{
    if (tunnel)
    {
        return "CONNECT";
    }
    return body != 0U ? "POST" : "GET";
}

/* The filler of a message of `index`: longer than the peer takes on the last request of an over-limit run. */
static inline uint64_t httpx_fill_bytes(const httpx_options_t* options, uint64_t index)
{
    return options->over_limit && index + 1U == options->requests ? options->field_limit + (options->field_limit / 2U)
                                                                  : options->field_bytes;
}

static inline int httpx_is_tunnel(const httpx_options_t* options, uint64_t index)
{
    return index < options->tunnels;
}

static inline int httpx_has_trailers(const httpx_options_t* options, uint64_t index)
{
    return options->trailers && !httpx_is_tunnel(options, index);
}

static inline int httpx_last_request(const httpx_options_t* options, uint64_t index)
{
    return index + 1U == options->requests;
}

/* How much of a message's body a request or response of `index` carries; a cancelled response goes on until it is. */
static inline uint64_t httpx_body_bytes(const httpx_options_t* options, uint64_t index, int response)
{
    if (response && options->cancel == HTTPX_CANCEL_CLIENT && httpx_last_request(options, index))
    {
        return UINT64_MAX;
    }
    return response ? options->response_bytes : options->request_bytes;
}

/* content-length on the even plain requests and responses, nothing on the odd ones or a tunnel. */
static inline int httpx_has_length(const httpx_options_t* options, uint64_t index, uint64_t body)
{
    return index % 2U == 0U && !httpx_is_tunnel(options, index) && body != UINT64_MAX;
}

static inline int httpx_text_is(const uint8_t* bytes, size_t size, const char* text)
{
    return size == strlen(text) && memcmp(bytes, text, size) == 0;
}

/* The decimal value of `bytes`; UINT64_MAX when they are not one. */
static inline uint64_t httpx_decimal(const uint8_t* bytes, size_t size)
{
    uint64_t value = size != 0U ? 0U : UINT64_MAX;
    for (size_t at = 0; at < size; ++at)
    {
        if (bytes[at] < '0' || bytes[at] > '9')
        {
            return UINT64_MAX;
        }
        value = (value * 10U) + (uint64_t)(bytes[at] - '0');
    }
    return value;
}

static inline int httpx_fill_matches(const uint8_t* value, size_t size)
{
    for (size_t at = 0; at < size; ++at)
    {
        if (value[at] != (uint8_t)httpx_fill_byte(at))
        {
            return 0;
        }
    }
    return 1;
}

static inline void httpx_take_path(httpx_section_t* section, const uint8_t* value, size_t size, uint64_t index)
{
    size_t at = size;
    while (at != 0U && value[at - 1U] != '/')
    {
        --at;
    }
    section->wrong = section->wrong || httpx_decimal(value + at, size - at) != index;
}

/* Holds one received field of the section of stream `index` to what the exchange wrote. */
static inline void httpx_take_field(httpx_section_t* section, const httpx_options_t* options, uint64_t index,
                                    const uint8_t* name, size_t name_size, const uint8_t* value, size_t value_size)
{
    if (httpx_text_is(name, name_size, ":status"))
    {
        section->status = (uint32_t)httpx_decimal(value, value_size);
    }
    else if (httpx_text_is(name, name_size, ":method"))
    {
        section->connect = httpx_text_is(value, value_size, "CONNECT");
        section->get     = httpx_text_is(value, value_size, "GET");
        section->post    = httpx_text_is(value, value_size, "POST");
    }
    else if (httpx_text_is(name, name_size, ":path"))
    {
        httpx_take_path(section, value, value_size, index);
    }
    else if (httpx_text_is(name, name_size, ":protocol"))
    {
        section->protocol_seen = httpx_text_is(value, value_size, HTTPX_TUNNEL_PROTOCOL);
    }
    else if (httpx_text_is(name, name_size, "x-cerid-repeat"))
    {
        section->repeat_seen = httpx_text_is(value, value_size, HTTPX_REPEAT_VALUE);
    }
    else if (httpx_text_is(name, name_size, "x-cerid-index"))
    {
        section->index = httpx_decimal(value, value_size);
        section->index_seen = section->index == index;
    }
    else if (httpx_text_is(name, name_size, "x-cerid-fill"))
    {
        section->fill_seen = value_size == httpx_fill_bytes(options, index) && httpx_fill_matches(value, value_size);
    }
}

/* Whether a request's or a final response's section held everything it had to. */
static inline int httpx_section_good(const httpx_section_t* section, const httpx_options_t* options, uint64_t index,
                                     int response)
{
    const uint64_t body = httpx_body_bytes(options, index, 0);
    const int      fill = httpx_fill_bytes(options, index) == 0U || section->fill_seen;
    if (section->wrong || !section->repeat_seen || !section->index_seen || !fill)
    {
        return 0;
    }
    if (response)
    {
        return section->status == 200U;
    }
    if (httpx_is_tunnel(options, index))
    {
        return section->connect && section->protocol_seen;
    }
    return body != 0U ? section->post : section->get;
}
