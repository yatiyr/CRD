/* crd-net interoperability test (4.2a) -- the HTTP/3 exchange crd-quic-peer and crd-ngtcp2-quic-peer run with --http3 1:
 * its options and the fields it carries, in one place so that the two programs read and write the same thing. Plain C:
 * the ngtcp2 peer is a C program.
 *
 * A client opens `requests` requests, the first `tunnels` of them Extended CONNECT tunnels (`:protocol` cerid-tunnel). A
 * request carries `request-bytes` bytes of body, a response `response-bytes`, each in the pattern of its stream, and the
 * index of a stream is its ID over four. Every message carries a field that repeats, so a QPACK dynamic table has
 * something to index, its index, and `field-bytes` bytes of filler. A server answers when the request has ended, a tunnel
 * as soon as it opens, its body still waiting for the request's end. Both ends check every field and every byte.
 *
 *   table, blocked  the QPACK table capacity and blocked streams each end allows the other
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

#define HTTP3_MAX_REQUESTS 512U
#define HTTP3_MAX_FILL 48000U
#define HTTP3_CANCEL_CODE 0x10CU       /* H3_REQUEST_CANCELLED */
#define HTTP3_REJECTED_CODE 0x10BU     /* H3_REQUEST_REJECTED */
#define HTTP3_NO_ERROR 0x100U
#define HTTP3_ALPN "h3"
#define HTTP3_TUNNEL_PROTOCOL "cerid-tunnel"
#define HTTP3_SERVER_NAME "cerid.test"
#define HTTP3_REPEAT_VALUE "a field value every message of the exchange repeats"
#define HTTP3_DATA_FRAME_BYTES 8192U
#define HTTP3_LINGER_NS 100000000U /* a client's wait before closing, for QPACK's last acknowledgements to arrive */

enum
{
    HTTP3_GOAWAY_NONE,
    HTTP3_GOAWAY_NOTICE,
    HTTP3_GOAWAY_REJECT,
    HTTP3_GOAWAY_CLIENT
};

enum
{
    HTTP3_CANCEL_NONE,
    HTTP3_CANCEL_CLIENT,
    HTTP3_CANCEL_SERVER
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
} http3_options_t;

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
} http3_section_t;

static inline void http3_default_options(http3_options_t* options)
{
    memset(options, 0, sizeof(*options));
    options->field_limit = 16384U;
    options->table = 4096U;
    options->blocked = 16U;
}

/* Whether `name` was one of the exchange's. */
static inline int http3_parse_option(http3_options_t* options, const char* name, const char* value)
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
        options->goaway = strcmp(value, "notice") == 0   ? HTTP3_GOAWAY_NOTICE
                          : strcmp(value, "reject") == 0 ? HTTP3_GOAWAY_REJECT
                          : strcmp(value, "client") == 0 ? HTTP3_GOAWAY_CLIENT
                                                         : HTTP3_GOAWAY_NONE;
    }
    else if (strcmp(name, "--cancel") == 0)
    {
        options->cancel = strcmp(value, "client") == 0   ? HTTP3_CANCEL_CLIENT
                          : strcmp(value, "server") == 0 ? HTTP3_CANCEL_SERVER
                                                         : HTTP3_CANCEL_NONE;
    }
    else
    {
        return 0;
    }
    return 1;
}

static inline int http3_options_valid(const http3_options_t* options)
{
    return options->requests <= HTTP3_MAX_REQUESTS && options->tunnels <= options->requests &&
           options->field_bytes <= HTTP3_MAX_FILL && options->field_limit >= 1024U &&
           options->field_limit * 2U <= HTTP3_MAX_FILL;
}

static inline char http3_fill_byte(uint64_t at)
{
    return (char)('a' + (at % 26U));
}

/* The filler of a message of `index`: longer than the peer takes on the last request of an over-limit run. */
static inline uint64_t http3_fill_bytes(const http3_options_t* options, uint64_t index)
{
    return options->over_limit && index + 1U == options->requests ? options->field_limit + (options->field_limit / 2U)
                                                                  : options->field_bytes;
}

static inline int http3_is_tunnel(const http3_options_t* options, uint64_t index)
{
    return index < options->tunnels;
}

static inline int http3_has_trailers(const http3_options_t* options, uint64_t index)
{
    return options->trailers && !http3_is_tunnel(options, index);
}

static inline int http3_last_request(const http3_options_t* options, uint64_t index)
{
    return index + 1U == options->requests;
}

/* How much of a message's body a request or response of `index` carries; a cancelled response goes on until it is. */
static inline uint64_t http3_body_bytes(const http3_options_t* options, uint64_t index, int response)
{
    if (response && options->cancel == HTTP3_CANCEL_CLIENT && http3_last_request(options, index))
    {
        return UINT64_MAX;
    }
    return response ? options->response_bytes : options->request_bytes;
}

/* content-length on the even plain requests and responses, nothing on the odd ones or a tunnel. */
static inline int http3_has_length(const http3_options_t* options, uint64_t index, uint64_t body)
{
    return index % 2U == 0U && !http3_is_tunnel(options, index) && body != UINT64_MAX;
}

static inline int http3_text_is(const uint8_t* bytes, size_t size, const char* text)
{
    return size == strlen(text) && memcmp(bytes, text, size) == 0;
}

/* The decimal value of `bytes`; UINT64_MAX when they are not one. */
static inline uint64_t http3_decimal(const uint8_t* bytes, size_t size)
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

static inline int http3_fill_matches(const uint8_t* value, size_t size)
{
    for (size_t at = 0; at < size; ++at)
    {
        if (value[at] != (uint8_t)http3_fill_byte(at))
        {
            return 0;
        }
    }
    return 1;
}

static inline void http3_take_path(http3_section_t* section, const uint8_t* value, size_t size, uint64_t index)
{
    size_t at = size;
    while (at != 0U && value[at - 1U] != '/')
    {
        --at;
    }
    section->wrong = section->wrong || http3_decimal(value + at, size - at) != index;
}

/* Holds one received field of the section of stream `index` to what the exchange wrote. */
static inline void http3_take_field(http3_section_t* section, const http3_options_t* options, uint64_t index,
                                    const uint8_t* name, size_t name_size, const uint8_t* value, size_t value_size)
{
    if (http3_text_is(name, name_size, ":status"))
    {
        section->status = (uint32_t)http3_decimal(value, value_size);
    }
    else if (http3_text_is(name, name_size, ":method"))
    {
        section->connect = http3_text_is(value, value_size, "CONNECT");
        section->get     = http3_text_is(value, value_size, "GET");
        section->post    = http3_text_is(value, value_size, "POST");
    }
    else if (http3_text_is(name, name_size, ":path"))
    {
        http3_take_path(section, value, value_size, index);
    }
    else if (http3_text_is(name, name_size, ":protocol"))
    {
        section->protocol_seen = http3_text_is(value, value_size, HTTP3_TUNNEL_PROTOCOL);
    }
    else if (http3_text_is(name, name_size, "x-cerid-repeat"))
    {
        section->repeat_seen = http3_text_is(value, value_size, HTTP3_REPEAT_VALUE);
    }
    else if (http3_text_is(name, name_size, "x-cerid-index"))
    {
        section->index = http3_decimal(value, value_size);
        section->index_seen = section->index == index;
    }
    else if (http3_text_is(name, name_size, "x-cerid-fill"))
    {
        section->fill_seen = value_size == http3_fill_bytes(options, index) && http3_fill_matches(value, value_size);
    }
}

/* Whether a request's or a final response's section held everything it had to. */
static inline int http3_section_good(const http3_section_t* section, const http3_options_t* options, uint64_t index,
                                     int response)
{
    const uint64_t body = http3_body_bytes(options, index, 0);
    const int      fill = http3_fill_bytes(options, index) == 0U || section->fill_seen;
    if (section->wrong || !section->repeat_seen || !section->index_seen || !fill)
    {
        return 0;
    }
    if (response)
    {
        return section->status == 200U;
    }
    if (http3_is_tunnel(options, index))
    {
        return section->connect && section->protocol_seen;
    }
    return body != 0U ? section->post : section->get;
}
