/* crd-net interoperability test (4.2a) -- the HTTP/3 application crd-ngtcp2-quic-peer runs over nghttp3 with --http3 1
 * (the exchange: http3_exchange.h). The program owns the QUIC connection and forwards its callbacks here. */
#pragma once

#include "http3_exchange.h"

#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>

#include <stdint.h>
#include <stdio.h>

#define PATTERN_BYTES 65536U

/* The peer's pattern tables: tag 0 client to server, 1 server to client, 2 and 3 a datagram's. */
extern uint8_t pattern[4][PATTERN_BYTES];
size_t         pattern_index(int64_t stream, uint64_t offset);

typedef struct http3_app http3_app_t;

/* `datagrams`: the HTTP datagrams of the whole run, 0 for none. Null when out of memory. */
http3_app_t* http3_new(const http3_options_t* options, int client, uint64_t datagrams);
void         http3_del(http3_app_t* app);

/* The QUIC callbacks: 0, or an NGTCP2_ERR_* to stop the connection. */
int  http3_recv_stream_data(http3_app_t* app, ngtcp2_conn* conn, uint32_t flags, int64_t id, const uint8_t* data,
                            size_t size, uint64_t now);
int  http3_acked(http3_app_t* app, int64_t id, uint64_t size);
int  http3_stream_close(http3_app_t* app, uint32_t flags, int64_t id, uint64_t rx_error, uint64_t tx_error);
int  http3_stream_reset(http3_app_t* app, int64_t id, uint64_t code);
int  http3_stream_stop(http3_app_t* app, int64_t id);
int  http3_unblock(http3_app_t* app, int64_t id);
void http3_set_max_bidi(http3_app_t* app, uint64_t maximum);

/* One service turn: starts HTTP/3 once `can_open` and opens requests, on 0-RTT (`early`) without waiting for the peer's
 * SETTINGS. 0 when the connection cannot go on. */
int http3_turn(http3_app_t* app, ngtcp2_conn* conn, int can_open, int early, int datagrams_settled);

/* The next stream data to write, as nghttp3_conn_writev_stream. */
nghttp3_ssize http3_writev(http3_app_t* app, int64_t* id, int* fin, nghttp3_vec* vec, size_t count);
void          http3_wrote(http3_app_t* app, int64_t id, size_t size);
void          http3_block(http3_app_t* app, int64_t id);
void          http3_shut_write(http3_app_t* app, int64_t id);

/* Datagrams: the quarter stream ID before datagram `index`, its bytes returned; whether the tunnel of `index` is open; a
 * received datagram with the ID taken off, which must be the one of the index that follows it. */
size_t http3_datagram_prefix(const http3_app_t* app, uint32_t index, uint8_t* out);
int    http3_datagram_ready(const http3_app_t* app, uint32_t index);
int    http3_strip_datagram(const http3_app_t* app, const uint8_t** data, size_t* size);

/* True once the client's part is over, and a moment has passed for what the peer still owes it. */
int  http3_done(http3_app_t* app, uint64_t now);
void http3_report(const http3_app_t* app);
