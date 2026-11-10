/* crd-net interoperability test (4.2a) -- a QUIC endpoint over the pinned ngtcp2 and OpenSSL 4.0, real UDP on the
 * loopback, the counterpart of cerid_quic_peer.cpp. quic_interop.py runs one of each and reads both reports.
 *
 * Usage: crd-ngtcp2-quic-peer --role client|server --port N --seed HEX --group NAME --cipher NAME --version 1|2
 *                             --streams N --bytes N --closer client|server --code N --max-streams N [--compat 1]
 *                             [--retry 1] [--key-update N] [--datagrams N --datagram-bytes N [--datagram-limit N]
 *                             [--datagram-probe 1]] [--connections 2 [--early 1] [--reject-early 1]]
 *                             [--only-version 1] [--progress-at N] [--resets 1] [--http3 1 ...]
 *
 *   port         a server's to bind (0: any, printed as `port=N`), a client's to reach
 *   seed         the 32-byte Ed25519 private key of the server's self-signed certificate; a client pins its public key
 *   group        an OpenSSL group name; cipher an OpenSSL TLS 1.3 suite name
 *   streams      bidirectional streams a client opens, each carrying `bytes` bytes and a FIN; a server echoes as many
 *                bytes as it reads and its FIN after the client's. Both ends check every byte (the pattern below)
 *   closer       the end that closes once every stream is done, with the application error `code`
 *   max-streams  the bidirectional streams this end lets its peer have open at once
 *   compat       compatible version negotiation (RFC 9368): a client offers versions 1 and 2, a server prefers 2
 *   retry        a server answers a client Initial without a token with a Retry
 *   key-update   this end starts a key update every N 1-RTT packets it sends
 *   datagrams    DATAGRAM frames a client sends, `datagram-bytes` each: the index in four bytes and a pattern; a server
 *                sends each back in another pattern
 *   datagram-limit  the max_datagram_frame_size this end advertises
 *   datagram-probe  this end also offers one payload a byte over `datagram-bytes`, which its peer's limit must refuse
 *   connections  connections run one after the other; a client resumes the second on a ticket of the first
 *   early        0-RTT: a resuming client writes its streams and datagrams at once
 *   reject-early a server refuses the second connection's 0-RTT
 *   only-version a server runs `version` alone and answers others with Version Negotiation
 *   progress-at  a server prints `progress=1` once it has read this many bytes
 *   resets       a server answers a short header for no connection with a stateless reset: its tokens are derived from
 *                a fixed key, so a restarted process answers for the connections of the one before
 *
 *   http3        HTTP/3 over nghttp3 instead of the echo: http_exchange.h has its options, ALPN `h3` instead
 *
 * The report of each connection, one `name=value` per line from `connection=N` on standard output when it has ended,
 * is the same as cerid_quic_peer.cpp's plus what OpenSSL negotiated, and ECN as this end saw it: ngtcp2 keeps its
 * validation (RFC 9000 section 13.4.2) to itself, so the report counts the datagrams it marked and the codepoints read.
 * More than NGTCP2_ECN_MAX_NUM_VALIDATION_PKTS (10) datagrams marked ECT(0) means it found the path capable, as it
 * marks no more while testing; the last packet unmarked means it stopped. `pmtu` is the datagram size its path MTU
 * discovery reached (RFC 8899 over its fixed probe sizes) and `peer_max_udp_payload` the max_udp_payload_size its peer
 * advertised. The application is `cerid-interop`, a private ALPN. */
#define _GNU_SOURCE /* ppoll */
#include "ngtcp2_http3.h"
#include "openssl_self_signed.h"

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>

#define MAX_STREAMS 4096U
#define DATAGRAM_BYTES 1452U
#define RUN_SECONDS 60U
#define CLOSING_QUIET_NS (300U * NGTCP2_MILLISECONDS)
#define CLOSING_NS (2U * NGTCP2_SECONDS)
#define RETRY_TOKEN_NS (10U * NGTCP2_SECONDS)
#define MAX_DATAGRAMS 4096U
#define MAX_DATAGRAM_BYTES 1400U
#define DATAGRAM_INDEX_BYTES 4U
#define DATAGRAM_SETTLE_NS NGTCP2_SECONDS /* no datagram for this long: those still out are lost */
#define DATAGRAM_WAKE_NS (20U * NGTCP2_MILLISECONDS)
#define SERVER_CID_BYTES 16U
#define MAX_EARLY_PARAMETERS 256U
#define MIN_INITIAL_DATAGRAM 1200U /* RFC 9000 section 14.1 */
#define HTTP3_UNI_STREAMS 16U
#define HTTP3_WRITE_PIECES 16U

static const unsigned char ALPN[] = "\x0D" "cerid-interop";
static const unsigned char H3_ALPN[] = "\x02" HTTP3_ALPN;
static const char SERVER_NAME[]   = "cerid.test";
static const uint8_t RETRY_SECRET[32] = {0x52, 0x65, 0x74};
static const uint8_t RESET_SECRET[32] = {0x52, 0x73, 0x74};
static const uint32_t PREFER_V2[2] = {NGTCP2_PROTO_VER_V2, NGTCP2_PROTO_VER_V1};
static const uint32_t OFFER_BOTH[2] = {NGTCP2_PROTO_VER_V1, NGTCP2_PROTO_VER_V2};

typedef struct
{
    uint64_t received;
    uint64_t sent;
    int      opened;
    int      in_end;  /* the peer's FIN was read */
    int      out_end; /* this end's FIN was written */
    int      closed;
    int      blocked; /* no flow-control credit in this write round */
} stream_t;

typedef struct
{
    int                     client;
    int                     closer;
    uint64_t                code;
    uint64_t                streams;
    uint64_t                bytes;
    uint64_t                max_streams;
    uint32_t                version;
    int                     fd;
    struct sockaddr_in      local;
    struct sockaddr_in      remote;
    SSL_CTX*                ctx;
    SSL*                    ssl;
    ngtcp2_crypto_ossl_ctx* ossl;
    ngtcp2_conn*            conn;
    ngtcp2_crypto_conn_ref  ref;
    unsigned char           pin[32];
    stream_t*               table;
    uint64_t                key_update_packets; /* 0: this end starts none */
    uint64_t                datagrams;
    uint64_t                datagram_bytes;
    uint64_t                datagram_limit;
    uint64_t                connections;
    uint64_t                number;           /* this connection's, from 1 */
    uint64_t                progress_at;
    uint32_t                original_version; /* a client's first attempt, after Version Negotiation */
    uint32_t                offered_version;  /* what Version Negotiation's list chooses */
    int                     compat;
    int                     retry;
    int                     early;
    int                     reject_early;
    int                     only_version;
    int                     resets;
    int                     datagram_probe;
    SSL_SESSION*            session; /* a client's newest ticket */
    uint8_t                 early_parameters[MAX_EARLY_PARAMETERS];
    size_t                  early_parameters_size;
    httpx_options_t         http3;
    http3_app_t*            app; /* one connection's, null for the echo */
    /* One connection's, cleared between connections from here on (next_connection). */
    uint64_t                opened;
    uint64_t                done;
    uint64_t                cursor;
    uint64_t                bytes_received;
    uint64_t                bytes_sent;
    uint64_t                packets_since_update;
    uint64_t                key_updates;
    uint64_t                early_bytes;
    uint64_t                early_datagrams;
    uint64_t                datagrams_sent;
    uint64_t                datagrams_received;
    uint64_t                datagrams_acked;
    uint64_t                last_datagram_ns;
    uint64_t                first_ns;
    uint64_t                ecn_sent[4];     /* datagrams written, by the codepoint ngtcp2 chose (NGTCP2_ECN_*) */
    uint64_t                ecn_received[4]; /* datagrams read, by the codepoint the IP header carried */
    uint32_t                echoes[MAX_DATAGRAMS]; /* a server's: the indices to send back */
    uint64_t                echo_first;
    uint64_t                echo_count;
    uint8_t                 seen[MAX_DATAGRAMS / 8U];
    int                     datagrams_unverified;
    int                     datagram_refused;
    int                     probed;
    int                     sending_early; /* a client's, until its server answers */
    int                     progressed;
    int                     stateless_reset;
    int                     unverified;
    int                     established;
    int                     pin_matched;
    uint8_t                 ecn_last; /* the codepoint of the last packet written that was not a close */
} peer_t;

/* The byte at `offset` of a stream's direction (tag 0: client to server, 1: server to client) or of a datagram's (2 and
 * 3); a 64 KiB period, so what ngtcp2 keeps until it is acknowledged can point straight into the table. */
uint8_t pattern[4][PATTERN_BYTES];

size_t pattern_index(int64_t stream, uint64_t offset)
{
    return (size_t)((offset + ((uint64_t)stream * 37U)) & (PATTERN_BYTES - 1U));
}

static void fill_pattern(void)
{
    for (uint32_t tag = 0; tag < 4U; ++tag)
    {
        for (uint32_t at = 0; at < PATTERN_BYTES; ++at)
        {
            pattern[tag][at] = (uint8_t)((at * 31U) + (at >> 8U) + (tag * 7U));
        }
    }
}

static uint64_t now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((uint64_t)now.tv_sec * NGTCP2_SECONDS) + (uint64_t)now.tv_nsec;
}

static void random_bytes(uint8_t* dest, size_t size, const ngtcp2_rand_ctx* context)
{
    (void)context;
    if (RAND_bytes(dest, (int)size) != 1)
    {
        abort();
    }
}

static int new_connection_id(ngtcp2_conn* conn, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, size_t size,
                             void* user)
{
    (void)conn;
    (void)user;
    if (RAND_bytes(cid->data, (int)size) != 1)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    cid->datalen = size;
    return ngtcp2_crypto_generate_stateless_reset_token(token->data, RESET_SECRET, sizeof(RESET_SECRET), cid) == 0
               ? 0
               : NGTCP2_ERR_CALLBACK_FAILURE;
}

static stream_t* stream_of(peer_t* peer, int64_t id)
{
    const uint64_t index = (uint64_t)id >> 2U;
    return (index < MAX_STREAMS && (id & 3) == 0) ? &peer->table[index] : NULL;
}

static int stream_open_cb(ngtcp2_conn* conn, int64_t id, void* user)
{
    (void)conn;
    if (((peer_t*)user)->app != NULL)
    {
        return 0;
    }
    stream_t* stream = stream_of((peer_t*)user, id);
    if (stream == NULL)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    stream->opened = 1;
    return 0;
}

static int recv_stream_data(ngtcp2_conn* conn, uint32_t flags, int64_t id, uint64_t offset, const uint8_t* data,
                            size_t size, void* user, void* stream_user)
{
    (void)stream_user;
    peer_t* peer = user;
    if (peer->app != NULL)
    {
        peer->early_bytes += (flags & NGTCP2_STREAM_DATA_FLAG_0RTT) != 0U ? size : 0U;
        return http3_recv_stream_data(peer->app, conn, flags, id, data, size, now_ns());
    }
    stream_t* stream = stream_of(peer, id);
    if (stream == NULL)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    const uint8_t* expected = pattern[peer->client ? 1 : 0];
    for (size_t at = 0; at < size; ++at)
    {
        if (data[at] != expected[pattern_index(id, offset + at)])
        {
            peer->unverified = 1;
        }
    }
    if (offset != stream->received)
    {
        peer->unverified = 1;
    }
    stream->received += size;
    peer->bytes_received += size;
    if ((flags & NGTCP2_STREAM_DATA_FLAG_0RTT) != 0U)
    {
        peer->early_bytes += size;
    }
    if (peer->progress_at != 0U && !peer->progressed && peer->bytes_received >= peer->progress_at)
    {
        peer->progressed = 1;
        puts("progress=1");
        fflush(stdout);
    }
    if ((flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0U)
    {
        stream->in_end = 1;
    }
    ngtcp2_conn_extend_max_stream_offset(conn, id, size);
    ngtcp2_conn_extend_max_offset(conn, size);
    return 0;
}

static int stream_close_cb(ngtcp2_conn* conn, uint32_t flags, int64_t id, uint64_t rx_error, uint64_t tx_error,
                           void* user, void* stream_user)
{
    (void)stream_user;
    peer_t* peer = user;
    if (peer->app != NULL)
    {
        return http3_stream_close(peer->app, flags, id, rx_error, tx_error);
    }
    stream_t* stream = stream_of(peer, id);
    if (stream == NULL)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    stream->closed = 1;
    ++peer->done;
    if (!peer->client)
    {
        ngtcp2_conn_extend_max_streams_bidi(conn, 1U);
    }
    return 0;
}

static int datagrams_settled(const peer_t* peer);

static int acked_stream_data(ngtcp2_conn* conn, int64_t id, uint64_t offset, uint64_t size, void* user, void* stream_user)
{
    (void)conn;
    (void)offset;
    (void)stream_user;
    return http3_acked(((peer_t*)user)->app, id, size);
}

static int extend_stream_data(ngtcp2_conn* conn, int64_t id, uint64_t maximum, void* user, void* stream_user)
{
    (void)conn;
    (void)maximum;
    (void)stream_user;
    return http3_unblock(((peer_t*)user)->app, id);
}

static int stream_reset_cb(ngtcp2_conn* conn, int64_t id, uint64_t final_size, uint64_t code, void* user,
                           void* stream_user)
{
    (void)conn;
    (void)final_size;
    (void)stream_user;
    return http3_stream_reset(((peer_t*)user)->app, id, code);
}

static int stop_sending_cb(ngtcp2_conn* conn, int64_t id, uint64_t code, void* user, void* stream_user)
{
    (void)conn;
    (void)code;
    (void)stream_user;
    return http3_stream_stop(((peer_t*)user)->app, id);
}

static int extend_remote_bidi(ngtcp2_conn* conn, uint64_t maximum, void* user)
{
    (void)conn;
    http3_set_max_bidi(((peer_t*)user)->app, maximum);
    return 0;
}

static int update_key(ngtcp2_conn* conn, uint8_t* rx_secret, uint8_t* tx_secret, ngtcp2_crypto_aead_ctx* rx_aead_ctx,
                      uint8_t* rx_iv, ngtcp2_crypto_aead_ctx* tx_aead_ctx, uint8_t* tx_iv,
                      const uint8_t* current_rx_secret, const uint8_t* current_tx_secret, size_t secret_size,
                      void* user)
{
    ++((peer_t*)user)->key_updates;
    return ngtcp2_crypto_update_key_cb(conn, rx_secret, tx_secret, rx_aead_ctx, rx_iv, tx_aead_ctx, tx_iv,
                                       current_rx_secret, current_tx_secret, secret_size, user);
}

static uint32_t datagram_index(const uint8_t* data, size_t size)
{
    uint32_t index = 0U;
    for (size_t at = 0; at < DATAGRAM_INDEX_BYTES && at < size; ++at)
    {
        index |= (uint32_t)data[at] << (at * 8U);
    }
    return index;
}

/* Holds a payload to its pattern, its size and its one delivery; a server sends it back. */
static int recv_datagram(ngtcp2_conn* conn, uint32_t flags, const uint8_t* data, size_t size, void* user)
{
    (void)conn;
    peer_t* peer = user;
    if (peer->app != NULL && !http3_strip_datagram(peer->app, &data, &size))
    {
        peer->datagrams_unverified = 1;
        return 0;
    }
    const uint32_t index = datagram_index(data, size);
    const uint8_t* expected = pattern[peer->client ? 3 : 2];
    if (size != peer->datagram_bytes || index >= peer->datagrams ||
        (peer->seen[index / 8U] & (1U << (index % 8U))) != 0U)
    {
        peer->datagrams_unverified = 1;
        return 0;
    }
    peer->seen[index / 8U] = (uint8_t)(peer->seen[index / 8U] | (1U << (index % 8U)));
    for (size_t at = DATAGRAM_INDEX_BYTES; at < size; ++at)
    {
        if (data[at] != expected[pattern_index(index, at)])
        {
            peer->datagrams_unverified = 1;
        }
    }
    ++peer->datagrams_received;
    peer->last_datagram_ns = now_ns();
    if ((flags & NGTCP2_DATAGRAM_FLAG_0RTT) != 0U)
    {
        ++peer->early_datagrams;
    }
    if (!peer->client)
    {
        peer->echoes[(peer->echo_first + peer->echo_count) % MAX_DATAGRAMS] = index;
        ++peer->echo_count;
    }
    return 0;
}

static int ack_datagram(ngtcp2_conn* conn, uint64_t id, void* user)
{
    (void)conn;
    (void)id;
    ++((peer_t*)user)->datagrams_acked;
    return 0;
}

static int recv_stateless_reset(ngtcp2_conn* conn, const ngtcp2_pkt_stateless_reset2* reset, void* user)
{
    (void)conn;
    (void)reset;
    ((peer_t*)user)->stateless_reset = 1;
    return 0;
}

static int recv_version_negotiation(ngtcp2_conn* conn, const ngtcp2_pkt_hd* header, const uint32_t* offered,
                                    size_t count, void* user)
{
    (void)conn;
    (void)header;
    peer_t* peer          = user;
    peer->offered_version = ngtcp2_select_version(OFFER_BOTH, 2U, offered, count);
    return 0;
}

/* RFC 9001 section 4.6.2: what the 0-RTT packets carried is gone; the streams and datagrams start again. */
static int early_data_rejected(ngtcp2_conn* conn, void* user)
{
    (void)conn;
    peer_t* peer        = user;
    peer->sending_early = 0;
    peer->opened        = 0U;
    peer->done          = 0U;
    peer->cursor        = 0U;
    peer->bytes_received = 0U;
    peer->bytes_sent     = 0U;
    peer->datagrams_sent = 0U;
    peer->datagrams_received = 0U;
    peer->datagrams_acked    = 0U;
    memset(peer->seen, 0, sizeof(peer->seen));
    memset(peer->table, 0, MAX_STREAMS * sizeof(stream_t));
    if (peer->app != NULL)
    {
        http3_del(peer->app);
        peer->app = http3_new(&peer->http3, peer->client, peer->datagrams);
    }
    return peer->http3.enabled && peer->app == NULL ? NGTCP2_ERR_CALLBACK_FAILURE : 0;
}

static int handshake_completed(ngtcp2_conn* conn, void* user)
{
    peer_t* peer = user;
    if (!peer->client)
    {
        return 0;
    }
    if (peer->sending_early && SSL_get_early_data_status(peer->ssl) != SSL_EARLY_DATA_ACCEPTED)
    {
        const int rejected = ngtcp2_conn_tls_early_data_rejected(conn);
        if (rejected != 0)
        {
            return rejected;
        }
    }
    peer->sending_early = 0;
    const ngtcp2_ssize size = ngtcp2_conn_encode_0rtt_transport_params2(conn, peer->early_parameters,
                                                                        sizeof(peer->early_parameters));
    peer->early_parameters_size = size > 0 ? (size_t)size : 0U;
    return 0;
}

static ngtcp2_conn* conn_of(ngtcp2_crypto_conn_ref* ref)
{
    return ((peer_t*)ref->user_data)->conn;
}

/* The certificate of `seed`'s key, installed on the server's context. */
static int use_identity(SSL_CTX* ctx, const unsigned char* seed)
{
    EVP_PKEY* key         = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    X509*     certificate = key != NULL ? self_signed(key, SERVER_NAME) : NULL;
    const int used        = certificate != NULL && SSL_CTX_use_certificate(ctx, certificate) == 1 &&
                     SSL_CTX_use_PrivateKey(ctx, key) == 1;
    X509_free(certificate);
    EVP_PKEY_free(key);
    return used;
}

static int select_alpn(SSL* ssl, const unsigned char** out, unsigned char* out_size, const unsigned char* in,
                       unsigned int in_size, void* user)
{
    (void)ssl;
    const peer_t*        peer      = user;
    const unsigned char* offer     = peer->http3.enabled ? H3_ALPN : ALPN;
    const size_t         offer_len = peer->http3.enabled ? sizeof(H3_ALPN) - 1U : sizeof(ALPN) - 1U;
    unsigned char*       selected  = NULL;
    if (SSL_select_next_proto(&selected, out_size, offer, (unsigned)offer_len, in, in_size) != OPENSSL_NPN_NEGOTIATED)
    {
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    *out = selected;
    return SSL_TLSEXT_ERR_OK;
}

/* Keeps a client's newest ticket for the next connection. */
static int keep_session(SSL* ssl, SSL_SESSION* session)
{
    peer_t* peer = ((ngtcp2_crypto_conn_ref*)SSL_get_app_data(ssl))->user_data;
    SSL_SESSION_free(peer->session);
    peer->session = session;
    return 1;
}

/* A connection's SSL over the context; made anew when a server drops a connection that could not start. */
static int make_session(peer_t* peer)
{
    peer->ssl = SSL_new(peer->ctx);
    if (peer->ssl == NULL || ngtcp2_crypto_ossl_ctx_new(&peer->ossl, NULL) != 0)
    {
        return 0;
    }
    if (peer->http3.enabled)
    {
        peer->app = http3_new(&peer->http3, peer->client, peer->datagrams);
        if (peer->app == NULL)
        {
            return 0;
        }
    }
    ngtcp2_crypto_ossl_ctx_set_ssl(peer->ossl, peer->ssl);
    peer->ref.get_conn  = conn_of;
    peer->ref.user_data = peer;
    SSL_set_app_data(peer->ssl, &peer->ref);
    if (peer->client)
    {
        SSL_set_connect_state(peer->ssl);
        if (ngtcp2_crypto_ossl_configure_client_session(peer->ssl) != 0 ||
            SSL_set_alpn_protos(peer->ssl, peer->http3.enabled ? H3_ALPN : ALPN,
                                peer->http3.enabled ? sizeof(H3_ALPN) - 1U : sizeof(ALPN) - 1U) != 0 ||
            SSL_set_tlsext_host_name(peer->ssl, SERVER_NAME) != 1)
        {
            return 0;
        }
        if (peer->session != NULL)
        {
            if (SSL_set_session(peer->ssl, peer->session) != 1)
            {
                return 0;
            }
            if (peer->early && SSL_SESSION_get_max_early_data(peer->session) != 0U)
            {
                SSL_set_quic_tls_early_data_enabled(peer->ssl, 1);
                peer->sending_early = peer->early_parameters_size != 0U;
            }
        }
        return 1;
    }
    SSL_set_accept_state(peer->ssl);
    if (ngtcp2_crypto_ossl_configure_server_session(peer->ssl) != 0)
    {
        return 0;
    }
    return !peer->early || (peer->reject_early && peer->number > 1U) ||
           SSL_set_quic_tls_early_data_enabled(peer->ssl, 1) == 1;
}

static int make_tls(peer_t* peer, const char* group, const char* cipher, const unsigned char* seed)
{
    peer->ctx = SSL_CTX_new(peer->client ? TLS_client_method() : TLS_server_method());
    if (peer->ctx == NULL || SSL_CTX_set_min_proto_version(peer->ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_ciphersuites(peer->ctx, cipher) != 1 || SSL_CTX_set1_groups_list(peer->ctx, group) != 1)
    {
        return 0;
    }
    if (!peer->client)
    {
        if (!use_identity(peer->ctx, seed))
        {
            return 0;
        }
        SSL_CTX_set_alpn_select_cb(peer->ctx, select_alpn, peer);
        if (peer->early)
        {
            SSL_CTX_set_max_early_data(peer->ctx, UINT32_MAX);
            SSL_CTX_set_options(peer->ctx, SSL_OP_NO_ANTI_REPLAY);
        }
    }
    else
    {
        SSL_CTX_set_session_cache_mode(peer->ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL);
        SSL_CTX_sess_set_new_cb(peer->ctx, keep_session);
    }
    return make_session(peer);
}

static ngtcp2_path path_of(peer_t* peer)
{
    ngtcp2_path path;
    memset(&path, 0, sizeof(path));
    path.local.addr     = (ngtcp2_sockaddr*)&peer->local;
    path.local.addrlen  = sizeof(peer->local);
    path.remote.addr    = (ngtcp2_sockaddr*)&peer->remote;
    path.remote.addrlen = sizeof(peer->remote);
    return path;
}

static void default_callbacks(const peer_t* peer, ngtcp2_callbacks* callbacks)
{
    memset(callbacks, 0, sizeof(*callbacks));
    callbacks->recv_crypto_data          = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks->encrypt                   = ngtcp2_crypto_encrypt_cb;
    callbacks->decrypt                   = ngtcp2_crypto_decrypt_cb;
    callbacks->hp_mask                   = ngtcp2_crypto_hp_mask_cb;
    callbacks->recv_stream_data          = recv_stream_data;
    callbacks->stream_open               = stream_open_cb;
    callbacks->rand                      = random_bytes;
    callbacks->update_key                = update_key;
    callbacks->delete_crypto_aead_ctx    = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks->delete_crypto_cipher_ctx  = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks->version_negotiation       = ngtcp2_crypto_version_negotiation_cb;
    callbacks->get_new_connection_id2    = new_connection_id;
    callbacks->get_path_challenge_data2  = ngtcp2_crypto_get_path_challenge_data2_cb;
    callbacks->stream_close2             = stream_close_cb;
    callbacks->recv_datagram             = recv_datagram;
    callbacks->ack_datagram              = ack_datagram;
    callbacks->recv_stateless_reset2     = recv_stateless_reset;
    callbacks->recv_version_negotiation  = recv_version_negotiation;
    callbacks->handshake_completed       = handshake_completed;
    callbacks->tls_early_data_rejected   = early_data_rejected;
    if (peer->app != NULL)
    {
        callbacks->acked_stream_data_offset       = acked_stream_data;
        callbacks->extend_max_stream_data         = extend_stream_data;
        callbacks->stream_reset                   = stream_reset_cb;
        callbacks->stream_stop_sending            = stop_sending_cb;
        callbacks->extend_max_remote_streams_bidi = extend_remote_bidi;
    }
}

static void default_parameters(peer_t* peer, ngtcp2_settings* settings, ngtcp2_transport_params* params)
{
    ngtcp2_settings_default(settings);
    settings->initial_ts = now_ns();
    ngtcp2_transport_params_default(params);
    params->initial_max_stream_data_bidi_local  = 1U << 20U;
    params->initial_max_stream_data_bidi_remote = 1U << 20U;
    params->initial_max_data                    = 4U << 20U;
    params->initial_max_streams_bidi            = peer->max_streams;
    params->max_idle_timeout                    = 30U * NGTCP2_SECONDS;
    params->active_connection_id_limit          = 7U;
    params->grease_quic_bit                     = 1U;
    params->max_datagram_frame_size             = peer->datagrams != 0U ? peer->datagram_limit : 0U;
    if (peer->app != NULL)
    {
        params->initial_max_streams_uni     = HTTP3_UNI_STREAMS;
        params->initial_max_stream_data_uni = 1U << 20U;
    }
    if (peer->compat)
    {
        settings->preferred_versions    = PREFER_V2;
        settings->preferred_versionslen = 2U;
        settings->available_versions    = peer->client ? OFFER_BOTH : PREFER_V2;
        settings->available_versionslen = 2U;
    }
    if (peer->only_version)
    {
        settings->preferred_versions    = &peer->version;
        settings->preferred_versionslen = 1U;
        settings->available_versions    = &peer->version;
        settings->available_versionslen = 1U;
    }
    settings->original_version = peer->original_version;
}

static int start_client(peer_t* peer)
{
    ngtcp2_callbacks        callbacks;
    ngtcp2_settings         settings;
    ngtcp2_transport_params params;
    ngtcp2_cid              dcid;
    ngtcp2_cid              scid;
    default_callbacks(peer, &callbacks);
    callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_retry     = ngtcp2_crypto_recv_retry_cb;
    default_parameters(peer, &settings, &params);
    dcid.datalen = NGTCP2_MIN_INITIAL_DCIDLEN;
    scid.datalen = 8U;
    if (RAND_bytes(dcid.data, (int)dcid.datalen) != 1 || RAND_bytes(scid.data, (int)scid.datalen) != 1)
    {
        return 0;
    }
    ngtcp2_path path = path_of(peer);
    if (ngtcp2_conn_client_new(&peer->conn, &dcid, &scid, &path, peer->version, &callbacks, &settings, &params, NULL,
                               peer) != 0)
    {
        return 0;
    }
    ngtcp2_conn_set_tls_native_handle(peer->conn, peer->ossl);
    if (peer->sending_early &&
        ngtcp2_conn_decode_and_set_0rtt_transport_params(peer->conn, peer->early_parameters,
                                                         peer->early_parameters_size) != 0)
    {
        return 0;
    }
    return 1;
}

static int send_datagram(peer_t* peer, const uint8_t* data, size_t size, uint8_t ecn);

/* A Retry for `header`'s client Initial, with a token proving its address. */
static int send_retry(peer_t* peer, const ngtcp2_pkt_hd* header)
{
    uint8_t         token[NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN2];
    uint8_t         packet[DATAGRAM_BYTES];
    ngtcp2_cid      scid;
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    scid.datalen = 16U;
    if (RAND_bytes(scid.data, (int)scid.datalen) != 1)
    {
        return 0;
    }
    const ngtcp2_ssize token_bytes = ngtcp2_crypto_generate_retry_token2(
        token, RETRY_SECRET, sizeof(RETRY_SECRET), header->version, (const ngtcp2_sockaddr*)&peer->remote,
        sizeof(peer->remote), &scid, &header->dcid, ((uint64_t)wall.tv_sec * NGTCP2_SECONDS) + (uint64_t)wall.tv_nsec);
    if (token_bytes < 0)
    {
        return 0;
    }
    const ngtcp2_ssize wrote = ngtcp2_crypto_write_retry(packet, sizeof(packet), header->version, &header->scid, &scid,
                                                         &header->dcid, token, (size_t)token_bytes);
    return wrote > 0 && send_datagram(peer, packet, (size_t)wrote, NGTCP2_ECN_NOT_ECT);
}

/* A client Initial makes the connection; a datagram before it that is none is dropped. */
static int start_server(peer_t* peer, const uint8_t* data, size_t size)
{
    ngtcp2_pkt_hd header;
    if (ngtcp2_accept(&header, data, size) != 0)
    {
        return 1;
    }
    ngtcp2_cid original = header.dcid;
    int        retried  = 0;
    if (peer->retry)
    {
        if (header.tokenlen == 0U)
        {
            return send_retry(peer, &header);
        }
        struct timespec wall;
        clock_gettime(CLOCK_REALTIME, &wall);
        if (ngtcp2_crypto_verify_retry_token2(&original, header.token, header.tokenlen, RETRY_SECRET,
                                              sizeof(RETRY_SECRET), header.version, (ngtcp2_sockaddr*)&peer->remote,
                                              sizeof(peer->remote), &header.dcid, RETRY_TOKEN_NS,
                                              ((uint64_t)wall.tv_sec * NGTCP2_SECONDS) + (uint64_t)wall.tv_nsec) != 0)
        {
            return 1;
        }
        retried = 1;
    }
    ngtcp2_callbacks        callbacks;
    ngtcp2_settings         settings;
    ngtcp2_transport_params params;
    ngtcp2_cid              scid;
    default_callbacks(peer, &callbacks);
    callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    default_parameters(peer, &settings, &params);
    params.original_dcid                 = original;
    params.original_dcid_present         = 1U;
    if (retried)
    {
        params.retry_scid         = header.dcid;
        params.retry_scid_present = 1U;
        settings.token            = header.token;
        settings.tokenlen         = header.tokenlen;
        settings.token_type       = NGTCP2_TOKEN_TYPE_RETRY;
    }
    params.stateless_reset_token_present = 1U;
    scid.datalen                         = SERVER_CID_BYTES;
    if (RAND_bytes(scid.data, (int)scid.datalen) != 1 ||
        ngtcp2_crypto_generate_stateless_reset_token(params.stateless_reset_token, RESET_SECRET, sizeof(RESET_SECRET),
                                                     &scid) != 0)
    {
        return 0;
    }
    ngtcp2_path path = path_of(peer);
    if (ngtcp2_conn_server_new(&peer->conn, &header.scid, &scid, &path, header.version, &callbacks, &settings, &params,
                               NULL, peer) != 0)
    {
        return 0;
    }
    ngtcp2_conn_set_tls_native_handle(peer->conn, peer->ossl);
    return 1;
}

/* A control message of one int, as IP_TOS takes on sending; IP_RECVTOS delivers one byte. */
typedef union
{
    char           bytes[CMSG_SPACE(sizeof(int))];
    struct cmsghdr align;
} control_t;

/* One datagram, its IP header carrying `ecn` (RFC 3168) as ngtcp2 chose it. */
static int send_datagram(peer_t* peer, const uint8_t* data, size_t size, uint8_t ecn)
{
    control_t     control;
    struct iovec  piece = {(void*)(uintptr_t)data, size}; /* sendmsg reads it only */
    struct msghdr message;
    memset(&control, 0, sizeof(control));
    memset(&message, 0, sizeof(message));
    message.msg_name       = &peer->remote;
    message.msg_namelen    = sizeof(peer->remote);
    message.msg_iov        = &piece;
    message.msg_iovlen     = 1U;
    message.msg_control    = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    struct cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level     = IPPROTO_IP;
    header->cmsg_type      = IP_TOS;
    header->cmsg_len       = CMSG_LEN(sizeof(int));
    const int tos          = ecn & NGTCP2_ECN_MASK;
    memcpy(CMSG_DATA(header), &tos, sizeof(tos));
    ssize_t sent;
    do
    {
        sent = sendmsg(peer->fd, &message, 0);
    } while (sent < 0 && errno == EINTR);
    /* A full socket buffer loses a datagram, as a path may. */
    return sent >= 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS;
}

/* RFC 9000 section 10.3: for a short header of no connection, a reset no longer than what it answers. */
static int send_reset(peer_t* peer, const uint8_t* data, size_t size)
{
    uint8_t                      packet[DATAGRAM_BYTES];
    uint8_t                      filler[DATAGRAM_BYTES];
    ngtcp2_stateless_reset_token token;
    if (size < 1U + SERVER_CID_BYTES || size < 1U + NGTCP2_MIN_STATELESS_RESET_RANDLEN + sizeof(token.data) + 1U ||
        size > DATAGRAM_BYTES)
    {
        return 1;
    }
    ngtcp2_cid cid;
    ngtcp2_cid_init(&cid, data + 1U, SERVER_CID_BYTES);
    const size_t filler_size = size - sizeof(token.data) - 2U;
    if (ngtcp2_crypto_generate_stateless_reset_token(token.data, RESET_SECRET, sizeof(RESET_SECRET), &cid) != 0 ||
        RAND_bytes(filler, (int)filler_size) != 1)
    {
        return 0;
    }
    const ngtcp2_ssize wrote = ngtcp2_pkt_write_stateless_reset2(packet, sizeof(packet), &token, filler, filler_size);
    return wrote > 0 && send_datagram(peer, packet, (size_t)wrote, NGTCP2_ECN_NOT_ECT);
}

/* Version Negotiation for a client Initial of another version: 1 when it was one, and answered. */
static int answer_other_version(peer_t* peer, const uint8_t* data, size_t size)
{
    ngtcp2_version_cid version;
    const int          decoded = ngtcp2_pkt_decode_version_cid(&version, data, size, SERVER_CID_BYTES);
    if ((decoded != 0 && decoded != NGTCP2_ERR_VERSION_NEGOTIATION) || (data[0] & 0x80U) == 0U ||
        version.version == peer->version || size < MIN_INITIAL_DATAGRAM)
    {
        return 0;
    }
    uint8_t            packet[DATAGRAM_BYTES];
    uint8_t            unused = 0U;
    const ngtcp2_ssize wrote  = RAND_bytes(&unused, 1) == 1
                                    ? ngtcp2_pkt_write_version_negotiation(
                                          packet, sizeof(packet), unused, version.scid, version.scidlen, version.dcid,
                                          version.dcidlen, &peer->version, 1U)
                                    : -1;
    return wrote > 0 && send_datagram(peer, packet, (size_t)wrote, NGTCP2_ECN_NOT_ECT);
}

/* A client's streams, or a turn of HTTP/3; 0 when the connection cannot go on. */
static int open_streams(peer_t* peer)
{
    const int running = ngtcp2_conn_get_handshake_completed2(peer->conn) || peer->sending_early;
    if (peer->app != NULL)
    {
        return http3_turn(peer->app, peer->conn, running, peer->sending_early, datagrams_settled(peer));
    }
    while (peer->client && peer->opened < peer->streams && running)
    {
        int64_t id = -1;
        if (ngtcp2_conn_open_bidi_stream(peer->conn, &id, NULL) != 0)
        {
            return 1;
        }
        stream_t* stream = stream_of(peer, id);
        if (stream == NULL)
        {
            abort();
        }
        stream->opened = 1;
        ++peer->opened;
    }
    return 1;
}

/* The next stream with bytes or a FIN to write, from the cursor; -1 for none. */
static int64_t next_stream(peer_t* peer)
{
    const uint64_t range = peer->client ? peer->opened : MAX_STREAMS;
    for (uint64_t step = 0; step < range; ++step)
    {
        const uint64_t index  = (peer->cursor + step) % range;
        stream_t*      stream = &peer->table[index];
        if (!stream->opened || stream->blocked || stream->out_end)
        {
            continue;
        }
        const uint64_t goal = peer->client ? peer->bytes : stream->received;
        if (stream->sent < goal || peer->client || stream->in_end)
        {
            peer->cursor = (index + 1U) % range;
            return (int64_t)(index << 2U);
        }
    }
    return -1;
}

/* Writes datagram `index` of `tag`'s pattern into `out`. */
static size_t fill_datagram(const peer_t* peer, uint8_t* out, uint32_t index, int tag)
{
    for (size_t at = 0; at < DATAGRAM_INDEX_BYTES; ++at)
    {
        out[at] = (uint8_t)(index >> (at * 8U));
    }
    for (size_t at = DATAGRAM_INDEX_BYTES; at < peer->datagram_bytes; ++at)
    {
        out[at] = pattern[tag][pattern_index(index, at)];
    }
    return (size_t)peer->datagram_bytes;
}

static int datagram_ready(const peer_t* peer)
{
    if (peer->client)
    {
        return peer->datagrams_sent < peer->datagrams &&
               (ngtcp2_conn_get_handshake_completed2(peer->conn) || peer->sending_early) &&
               (peer->app == NULL || http3_datagram_ready(peer->app, (uint32_t)peer->datagrams_sent));
    }
    return peer->echo_count != 0U && ngtcp2_conn_get_handshake_completed2(peer->conn);
}

/* What goes after a packet is written, marked `ecn`: 1 to carry on, 0 to give up. */
static int packet_written(peer_t* peer, const uint8_t* packet, size_t size, uint8_t ecn, uint64_t now)
{
    if (!send_datagram(peer, packet, size, ecn))
    {
        return 0;
    }
    ++peer->ecn_sent[ecn & NGTCP2_ECN_MASK];
    peer->ecn_last = ecn & NGTCP2_ECN_MASK;
    ++peer->packets_since_update;
    if (peer->key_update_packets != 0U && peer->packets_since_update >= peer->key_update_packets &&
        ngtcp2_conn_get_handshake_completed2(peer->conn))
    {
        /* Refused while the last update is unconfirmed: the next packet asks again. */
        if (ngtcp2_conn_initiate_key_update(peer->conn, now) == 0)
        {
            peer->packets_since_update = 0U;
        }
    }
    return 1;
}

/* Offers once a payload a byte over the peer's limit, which ngtcp2 must refuse without writing a packet. */
static void probe_datagram_limit(peer_t* peer, ngtcp2_path_storage* storage)
{
    if (!peer->datagram_probe || peer->probed || !ngtcp2_conn_get_handshake_completed2(peer->conn))
    {
        return;
    }
    static uint8_t  over[MAX_DATAGRAM_BYTES];
    uint8_t         packet[DATAGRAM_BYTES];
    ngtcp2_pkt_info info     = {0};
    ngtcp2_vec      piece    = {over, (size_t)peer->datagram_bytes + 1U};
    int             accepted = 0;
    peer->probed             = 1;
    peer->datagram_refused   = ngtcp2_conn_writev_datagram(peer->conn, &storage->path, &info, packet, sizeof(packet),
                                                           &accepted, 0U, 0U, &piece, 1U, now_ns()) ==
                             NGTCP2_ERR_INVALID_ARGUMENT;
}

/* One datagram queued or a packet of them written: 0 when the connection cannot go on, and *held when there is no room
 * in this write round. `info` is the write round's: ngtcp2 keeps a UDP datagram's codepoint in it across
 * NGTCP2_ERR_WRITE_MORE and resets it for the next datagram. */
static int write_one_datagram(peer_t* peer, ngtcp2_path_storage* storage, ngtcp2_pkt_info* info, uint8_t* datagram,
                              int* held, uint64_t now)
{
    uint8_t         payload[MAX_DATAGRAM_BYTES];
    const uint32_t  index  = peer->client ? (uint32_t)peer->datagrams_sent : peer->echoes[peer->echo_first];
    const size_t    prefix = peer->app != NULL ? http3_datagram_prefix(peer->app, index, payload) : 0U;
    ngtcp2_vec      piece  = {payload, prefix + fill_datagram(peer, payload + prefix, index, peer->client ? 2 : 3)};
    int             accepted = 0;
    const ngtcp2_ssize wrote =
        ngtcp2_conn_writev_datagram(peer->conn, &storage->path, info, datagram, DATAGRAM_BYTES, &accepted,
                                    NGTCP2_WRITE_DATAGRAM_FLAG_MORE, index, &piece, 1U, now);
    if (accepted)
    {
        peer->last_datagram_ns = now;
        ++peer->datagrams_sent;
        if (!peer->client)
        {
            peer->echo_first = (peer->echo_first + 1U) % MAX_DATAGRAMS;
            --peer->echo_count;
        }
    }
    if (wrote == NGTCP2_ERR_WRITE_MORE)
    {
        return 1;
    }
    if (wrote < 0)
    {
        fprintf(stderr, "ngtcp2_conn_writev_datagram: %s\n", ngtcp2_strerror((int)wrote));
        return 0;
    }
    if (wrote == 0)
    {
        *held = 1;
        return 1;
    }
    return packet_written(peer, datagram, (size_t)wrote, info->ecn, now);
}

/* What nghttp3 has to write, and HTTP datagrams beside it. */
static int write_http3(peer_t* peer)
{
    uint8_t             datagram[DATAGRAM_BYTES];
    ngtcp2_path_storage storage;
    ngtcp2_pkt_info     info = {0};
    ngtcp2_path_storage_zero(&storage);
    int held = 0;
    for (;;)
    {
        const uint64_t now = now_ns();
        if (!held && datagram_ready(peer))
        {
            if (!write_one_datagram(peer, &storage, &info, datagram, &held, now))
            {
                return 0;
            }
            continue;
        }
        int64_t       id  = -1;
        int           fin = 0;
        nghttp3_vec   pieces[HTTP3_WRITE_PIECES];
        nghttp3_ssize count = ngtcp2_conn_get_max_data_left(peer->conn) != 0U
                                  ? http3_writev(peer->app, &id, &fin, pieces, HTTP3_WRITE_PIECES)
                                  : 0;
        if (count < 0)
        {
            fprintf(stderr, "nghttp3_conn_writev_stream: %s\n", nghttp3_strerror((int)count));
            return 0;
        }
        const uint32_t  flags = NGTCP2_WRITE_STREAM_FLAG_MORE | (fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0U);
        ngtcp2_ssize    taken = -1;
        ngtcp2_ssize    wrote = ngtcp2_conn_writev_stream(peer->conn, &storage.path, &info, datagram, sizeof(datagram),
                                                          &taken, flags, id, (const ngtcp2_vec*)pieces, (size_t)count,
                                                          now);
        if (wrote == NGTCP2_ERR_WRITE_MORE)
        {
            http3_wrote(peer->app, id, (size_t)taken);
            continue;
        }
        if (wrote == NGTCP2_ERR_STREAM_DATA_BLOCKED)
        {
            http3_block(peer->app, id);
            continue;
        }
        if (wrote == NGTCP2_ERR_STREAM_SHUT_WR)
        {
            http3_shut_write(peer->app, id);
            continue;
        }
        if (wrote < 0)
        {
            fprintf(stderr, "ngtcp2_conn_writev_stream: %s\n", ngtcp2_strerror((int)wrote));
            return 0;
        }
        if (taken >= 0)
        {
            http3_wrote(peer->app, id, (size_t)taken);
        }
        if (wrote == 0)
        {
            ngtcp2_conn_update_pkt_tx_time(peer->conn, now);
            return 1;
        }
        if (!packet_written(peer, datagram, (size_t)wrote, info.ecn, now))
        {
            return 0;
        }
    }
}

static int write_streams(peer_t* peer)
{
    if (peer->app != NULL)
    {
        return write_http3(peer);
    }
    uint8_t            datagram[DATAGRAM_BYTES];
    ngtcp2_path_storage storage;
    ngtcp2_path_storage_zero(&storage);
    for (uint64_t index = 0; index < MAX_STREAMS; ++index)
    {
        peer->table[index].blocked = 0;
    }
    probe_datagram_limit(peer, &storage);
    const int       tag  = peer->client ? 0 : 1;
    int             held = 0; /* no room for a datagram in this write round */
    ngtcp2_pkt_info info = {0};
    for (;;)
    {
        const uint64_t now = now_ns();
        if (!held && datagram_ready(peer))
        {
            if (!write_one_datagram(peer, &storage, &info, datagram, &held, now))
            {
                return 0;
            }
            continue;
        }
        int64_t        id  = next_stream(peer);
        ngtcp2_vec     vector;
        size_t         vectors = 0U;
        uint32_t       flags   = NGTCP2_WRITE_STREAM_FLAG_MORE;
        stream_t*      stream  = id >= 0 ? stream_of(peer, id) : NULL;
        size_t         offered = 0U;
        if (stream != NULL)
        {
            const uint64_t goal = peer->client ? peer->bytes : stream->received;
            const size_t   at   = pattern_index(id, stream->sent);
            const uint64_t left = goal - stream->sent;
            offered             = left < (PATTERN_BYTES - at) ? (size_t)left : (PATTERN_BYTES - at);
            vector.base         = &pattern[tag][at];
            vector.len          = offered;
            vectors             = offered != 0U ? 1U : 0U;
            if (stream->sent + offered == goal && (peer->client || stream->in_end))
            {
                flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
            }
        }
        ngtcp2_ssize    taken = -1;
        ngtcp2_ssize    wrote = ngtcp2_conn_writev_stream(peer->conn, &storage.path, &info, datagram, sizeof(datagram),
                                                          &taken, flags, id, vectors != 0U ? &vector : NULL, vectors,
                                                          now);
        if (stream != NULL && taken >= 0)
        {
            stream->sent += (uint64_t)taken;
            peer->bytes_sent += (uint64_t)taken;
            if ((flags & NGTCP2_WRITE_STREAM_FLAG_FIN) != 0U && (size_t)taken == offered)
            {
                stream->out_end = 1;
            }
        }
        if (wrote == NGTCP2_ERR_WRITE_MORE)
        {
            continue;
        }
        if (wrote == NGTCP2_ERR_STREAM_DATA_BLOCKED || wrote == NGTCP2_ERR_STREAM_SHUT_WR)
        {
            if (stream == NULL)
            {
                return 0;
            }
            stream->blocked = 1;
            continue;
        }
        if (wrote < 0)
        {
            fprintf(stderr, "ngtcp2_conn_writev_stream: %s\n", ngtcp2_strerror((int)wrote));
            return 0;
        }
        if (wrote == 0)
        {
            if (stream != NULL && taken < 0)
            {
                /* The congestion window or the pacer holds it: nothing more this round. */
                ngtcp2_conn_update_pkt_tx_time(peer->conn, now);
                return 1;
            }
            if (stream == NULL)
            {
                ngtcp2_conn_update_pkt_tx_time(peer->conn, now);
                return 1;
            }
            continue;
        }
        if (!packet_written(peer, datagram, (size_t)wrote, info.ecn, now))
        {
            return 0;
        }
    }
}

static int datagrams_settled(const peer_t* peer)
{
    if (peer->datagrams == 0U)
    {
        return 1;
    }
    if (peer->echo_count != 0U || (peer->client && peer->datagrams_sent < peer->datagrams))
    {
        return 0;
    }
    const uint64_t since = peer->last_datagram_ns != 0U ? peer->last_datagram_ns : peer->first_ns;
    return peer->datagrams_received >= peer->datagrams || now_ns() - since >= DATAGRAM_SETTLE_NS;
}

static int all_done(const peer_t* peer)
{
    if (!datagrams_settled(peer) || (peer->client && peer->number < peer->connections && peer->session == NULL))
    {
        return 0;
    }
    if (peer->app != NULL)
    {
        return http3_done(peer->app, now_ns());
    }
    if (!peer->client)
    {
        return peer->done >= peer->streams;
    }
    if (peer->opened < peer->streams)
    {
        return 0;
    }
    for (uint64_t index = 0; index < peer->streams; ++index)
    {
        const stream_t* stream = &peer->table[index];
        if (!stream->in_end || !stream->out_end || stream->received != peer->bytes)
        {
            return 0;
        }
    }
    return 1;
}

static int check_pin(peer_t* peer)
{
    if (!peer->client)
    {
        return 1;
    }
    unsigned char expected[32];
    unsigned char found[32];
    size_t        expected_size = sizeof(expected);
    size_t        found_size    = sizeof(found);
    EVP_PKEY*     key           = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, peer->pin, 32);
    X509*         certificate   = SSL_get1_peer_certificate(peer->ssl);
    const int     read          = key != NULL && certificate != NULL &&
                     EVP_PKEY_get_raw_public_key(key, expected, &expected_size) == 1 &&
                     EVP_PKEY_get_raw_public_key(X509_get0_pubkey(certificate), found, &found_size) == 1;
    X509_free(certificate);
    EVP_PKEY_free(key);
    return read && expected_size == found_size && memcmp(expected, found, found_size) == 0;
}

static void report(peer_t* peer, const char* close_kind, uint64_t close_code)
{
    const SSL_CIPHER*    cipher = peer->established ? SSL_get_current_cipher(peer->ssl) : NULL;
    const char*          group  = peer->established ? SSL_get0_group_name(peer->ssl) : NULL;
    const unsigned char* alpn   = NULL;
    unsigned int         length = 0U;
    SSL_get0_alpn_selected(peer->ssl, &alpn, &length);
    const uint32_t negotiated = peer->conn != NULL ? ngtcp2_conn_get_negotiated_version2(peer->conn) : 0U;
    uint64_t       ended      = 0U;
    for (uint64_t index = 0; index < MAX_STREAMS; ++index)
    {
        ended += peer->table[index].in_end ? 1U : 0U;
    }
    const int   status = peer->established ? SSL_get_early_data_status(peer->ssl) : SSL_EARLY_DATA_NOT_SENT;
    const char* early  = status == SSL_EARLY_DATA_ACCEPTED ? "accepted"
                         : status == SSL_EARLY_DATA_REJECTED ? "rejected"
                                                              : "none";
    printf("connection=%llu\n", (unsigned long long)peer->number);
    printf("established=%d\nversion=%u\nstreams=%llu\nbytes_received=%llu\nbytes_sent=%llu\nverified=%d\n",
           peer->established,
           negotiated == NGTCP2_PROTO_VER_V2 ? 2U : (negotiated == NGTCP2_PROTO_VER_V1 ? 1U : 0U),
           (unsigned long long)ended, (unsigned long long)peer->bytes_received,
           (unsigned long long)peer->bytes_sent, !peer->unverified);
    printf("resumed=%d\nearly=%s\nearly_bytes=%llu\nearly_datagrams=%llu\n",
           peer->established ? (SSL_session_reused(peer->ssl) ? 1 : 0) : 0, early,
           (unsigned long long)peer->early_bytes, (unsigned long long)peer->early_datagrams);
    printf("datagrams_sent=%llu\ndatagrams_received=%llu\ndatagrams_acked=%llu\ndatagrams_verified=%d\n"
           "datagrams_refused=%d\n",
           (unsigned long long)peer->datagrams_sent, (unsigned long long)peer->datagrams_received,
           (unsigned long long)peer->datagrams_acked, !peer->datagrams_unverified, peer->datagram_refused);
    printf("close=%s\nclose_code=%llu\npin=%d\nkey_updates=%llu\ngroup=%s\ncipher=%s\nalpn=%.*s\n", close_kind,
           (unsigned long long)close_code, peer->pin_matched, (unsigned long long)peer->key_updates,
           group != NULL ? group : "none",
           cipher != NULL ? SSL_CIPHER_get_name(cipher) : "none", (int)length, alpn != NULL ? (const char*)alpn : "");
    ngtcp2_conn_info info;
    memset(&info, 0, sizeof(info));
    info.ssthresh = UINT64_MAX;
    if (peer->conn != NULL)
    {
        ngtcp2_conn_get_conn_info(peer->conn, &info);
    }
    static const char* const codepoints[4] = {"not-ect", "ect1", "ect0", "ce"};
    printf("ecn_sent_ect0=%llu\necn_last=%s\necn_received_ect0=%llu\necn_received_ect1=%llu\necn_received_ce=%llu\n"
           "ssthresh_set=%d\n",
           (unsigned long long)peer->ecn_sent[NGTCP2_ECN_ECT_0], codepoints[peer->ecn_last],
           (unsigned long long)peer->ecn_received[NGTCP2_ECN_ECT_0],
           (unsigned long long)peer->ecn_received[NGTCP2_ECN_ECT_1],
           (unsigned long long)peer->ecn_received[NGTCP2_ECN_CE], info.ssthresh != UINT64_MAX);
    const ngtcp2_transport_params* remote =
        peer->conn != NULL ? ngtcp2_conn_get_remote_transport_params(peer->conn) : NULL;
    printf("pmtu=%zu\npeer_max_udp_payload=%llu\n",
           peer->conn != NULL ? ngtcp2_conn_get_path_max_tx_udp_payload_size2(peer->conn) : (size_t)0,
           remote != NULL ? (unsigned long long)remote->max_udp_payload_size : 0ULL);
    if (peer->app != NULL)
    {
        http3_report(peer->app);
    }
    fflush(stdout);
}

/* The terminal packet of a close this end chooses, sent again for what still arrives (RFC 9000 section 10.2.1) until
 * the peer goes quiet, and the report. */
static void close_connection(peer_t* peer, uint64_t code)
{
    uint8_t             datagram[DATAGRAM_BYTES];
    ngtcp2_path_storage storage;
    ngtcp2_pkt_info     info = {0};
    ngtcp2_ccerr        error;
    ngtcp2_path_storage_zero(&storage);
    ngtcp2_ccerr_default(&error);
    ngtcp2_ccerr_set_application_error(&error, code, NULL, 0U);
    const ngtcp2_ssize wrote = ngtcp2_conn_write_connection_close(peer->conn, &storage.path, &info, datagram,
                                                                  sizeof(datagram), &error, now_ns());
    report(peer, "local", code);
    if (wrote <= 0)
    {
        return;
    }
    (void)send_datagram(peer, datagram, (size_t)wrote, info.ecn);
    const uint64_t deadline = now_ns() + CLOSING_NS;
    uint64_t       quiet    = now_ns() + CLOSING_QUIET_NS;
    while (now_ns() < deadline && now_ns() < quiet)
    {
        struct pollfd   entry   = {peer->fd, POLLIN, 0};
        struct timespec timeout = {0, (long)(10U * NGTCP2_MILLISECONDS)};
        if (ppoll(&entry, 1U, &timeout, NULL) > 0)
        {
            uint8_t ignored[DATAGRAM_BYTES];
            while (recv(peer->fd, ignored, sizeof(ignored), MSG_DONTWAIT) > 0)
            {
                (void)send_datagram(peer, datagram, (size_t)wrote, info.ecn);
                quiet = now_ns() + CLOSING_QUIET_NS;
            }
        }
    }
}

/* Frees what one connection held. QUIC's close stands for TLS's: without the flag, OpenSSL takes the free for a broken
 * connection and marks its sessions unresumable, the ticket kept for the next connection among them. */
static void free_connection(peer_t* peer)
{
    http3_del(peer->app);
    peer->app = NULL;
    ngtcp2_conn_del(peer->conn);
    peer->conn = NULL;
    ngtcp2_crypto_ossl_ctx_del(peer->ossl);
    peer->ossl = NULL;
    SSL_set_shutdown(peer->ssl, SSL_SENT_SHUTDOWN);
    SSL_free(peer->ssl);
    peer->ssl = NULL;
}

static int begin_client(peer_t* peer);

/* A client's first attempt met Version Negotiation: the next attempt, in the version it chose (RFC 9368 section 4). */
static int restart_after_negotiation(peer_t* peer)
{
    if (peer->offered_version == 0U || peer->offered_version == peer->version || peer->original_version != 0U)
    {
        return 0;
    }
    peer->original_version = peer->version;
    peer->version          = peer->offered_version;
    free_connection(peer);
    return make_session(peer) && begin_client(peer);
}

static const char* close_name(ngtcp2_ccerr_type type)
{
    switch (type)
    {
    case NGTCP2_CCERR_TYPE_TRANSPORT:
        return "transport";
    case NGTCP2_CCERR_TYPE_APPLICATION:
        return "application";
    case NGTCP2_CCERR_TYPE_IDLE_CLOSE:
        return "idle";
    default:
        return "other";
    }
}

/* The codepoint a received datagram's IP header carried (IP_RECVTOS's control message), NOT_ECT for none. */
static uint8_t received_ecn(struct msghdr* message)
{
    for (struct cmsghdr* header = CMSG_FIRSTHDR(message); header != NULL; header = CMSG_NXTHDR(message, header))
    {
        if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_TOS && header->cmsg_len > CMSG_LEN(0))
        {
            return (uint8_t)(*CMSG_DATA(header) & NGTCP2_ECN_MASK);
        }
    }
    return NGTCP2_ECN_NOT_ECT;
}

/* Reads every datagram waiting; 0 when the connection has ended (the report written). */
static int read_datagrams(peer_t* peer)
{
    uint8_t datagram[65536];
    for (;;)
    {
        struct sockaddr_in from;
        control_t          control;
        struct iovec       piece = {datagram, sizeof(datagram)};
        struct msghdr      message;
        memset(&message, 0, sizeof(message));
        message.msg_name       = &from;
        message.msg_namelen    = sizeof(from);
        message.msg_iov        = &piece;
        message.msg_iovlen     = 1U;
        message.msg_control    = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        const ssize_t size     = recvmsg(peer->fd, &message, MSG_DONTWAIT);
        if (size < 0)
        {
            return 1;
        }
        const uint8_t ecn = received_ecn(&message);
        ++peer->ecn_received[ecn];
        if (peer->conn == NULL)
        {
            peer->remote = from;
            if (peer->resets && size > 0 && (datagram[0] & 0x80U) == 0U)
            {
                if (!send_reset(peer, datagram, (size_t)size))
                {
                    return 0;
                }
                continue;
            }
            if (peer->only_version && answer_other_version(peer, datagram, (size_t)size))
            {
                continue;
            }
            if (!start_server(peer, datagram, (size_t)size))
            {
                report(peer, "failed", 0U);
                return 0;
            }
            if (peer->conn == NULL)
            {
                continue;
            }
        }
        ngtcp2_pkt_info info = {ecn};
        ngtcp2_path     path = path_of(peer);
        const int result = ngtcp2_conn_read_pkt(peer->conn, &path, &info, datagram, (size_t)size, now_ns());
        if (result == NGTCP2_ERR_DRAINING)
        {
            const ngtcp2_ccerr* error = ngtcp2_conn_get_ccerr2(peer->conn);
            report(peer, peer->stateless_reset ? "reset" : close_name(error->type), error->error_code);
            return 0;
        }
        if (result == NGTCP2_ERR_RECV_VERSION_NEGOTIATION && peer->client)
        {
            if (!restart_after_negotiation(peer))
            {
                report(peer, "failed", 0U);
                return 0;
            }
            continue;
        }
        if ((result == NGTCP2_ERR_RETRY || result == NGTCP2_ERR_DROP_CONN) && !peer->client && !peer->established)
        {
            /* The client's first flight arrived out of order: drop the attempt and take its next Initial. */
            free_connection(peer);
            if (!make_session(peer))
            {
                return 0;
            }
            continue;
        }
        if (result != 0)
        {
            fprintf(stderr, "ngtcp2_conn_read_pkt: %s\n", ngtcp2_strerror(result));
            report(peer, "failed", 0U);
            return 0;
        }
        if (ngtcp2_conn_get_handshake_completed2(peer->conn) && !peer->established)
        {
            peer->established = 1;
            peer->pin_matched = check_pin(peer);
        }
    }
}

static int run(peer_t* peer)
{
    const uint64_t deadline = now_ns() + (RUN_SECONDS * NGTCP2_SECONDS);
    for (;;)
    {
        const uint64_t now = now_ns();
        if (now >= deadline)
        {
            fputs("crd-ngtcp2-quic-peer: the run outlasted its deadline\n", stderr);
            return 0;
        }
        uint64_t wait = deadline - now;
        if (peer->conn != NULL)
        {
            const uint64_t expiry = ngtcp2_conn_get_expiry2(peer->conn);
            wait                  = expiry <= now ? 0U : (expiry - now < wait ? expiry - now : wait);
            /* A datagram that never comes tells ngtcp2 nothing: the wait for it is this program's, and HTTP/3's turns. */
            wait = ((peer->datagrams != 0U || peer->app != NULL) && wait > DATAGRAM_WAKE_NS) ? DATAGRAM_WAKE_NS : wait;
        }
        struct pollfd   entry   = {peer->fd, POLLIN, 0};
        struct timespec timeout = {(time_t)(wait / NGTCP2_SECONDS), (long)(wait % NGTCP2_SECONDS)};
        (void)ppoll(&entry, 1U, &timeout, NULL);
        if (!read_datagrams(peer))
        {
            return peer->established;
        }
        if (peer->conn == NULL)
        {
            continue;
        }
        const uint64_t current = now_ns();
        if (ngtcp2_conn_get_expiry2(peer->conn) <= current)
        {
            const int result = ngtcp2_conn_handle_expiry(peer->conn, current);
            if (result != 0)
            {
                fprintf(stderr, "ngtcp2_conn_handle_expiry: %s\n", ngtcp2_strerror(result));
                report(peer, result == NGTCP2_ERR_IDLE_CLOSE ? "idle" : "failed", 0U);
                return result == NGTCP2_ERR_IDLE_CLOSE && peer->established;
            }
        }
        peer->first_ns = peer->first_ns != 0U ? peer->first_ns : current;
        if (!open_streams(peer) || !write_streams(peer))
        {
            report(peer, "failed", 0U);
            return 0;
        }
        if (peer->closer == peer->client && all_done(peer))
        {
            close_connection(peer, peer->code);
            return peer->established;
        }
    }
}

/* A client's next attempt on the socket: its parameters, its streams when 0-RTT carries them, its first flight. */
static int begin_client(peer_t* peer)
{
    if (!start_client(peer))
    {
        return 0;
    }
    return open_streams(peer) && write_streams(peer);
}

/* Forgets the connection that ended and makes the next one's SSL. */
static int next_connection(peer_t* peer)
{
    free_connection(peer);
    memset(&peer->opened, 0, sizeof(*peer) - offsetof(peer_t, opened));
    memset(peer->table, 0, MAX_STREAMS * sizeof(stream_t));
    peer->original_version = 0U;
    peer->offered_version  = 0U;
    ++peer->number;
    return make_session(peer);
}

static int parse_seed(const char* text, unsigned char* out)
{
    if (strlen(text) != 64U)
    {
        return 0;
    }
    for (size_t at = 0; at < 32U; ++at)
    {
        unsigned value = 0U;
        if (sscanf(text + (at * 2U), "%2x", &value) != 1)
        {
            return 0;
        }
        out[at] = (unsigned char)value;
    }
    return 1;
}

static int usage(void)
{
    fputs("usage: crd-ngtcp2-quic-peer --role client|server --port N --seed HEX --group NAME --cipher NAME "
          "--version 1|2 --streams N --bytes N --closer client|server --code N --max-streams N [--compat 1] [--retry 1]"
          " [--key-update N] [--datagrams N --datagram-bytes N [--datagram-limit N] [--datagram-probe 1]]"
          " [--connections 2 [--early 1] [--reject-early 1]] [--only-version 1] [--progress-at N] [--resets 1]"
          " [--http3 1 ...]\n",
          stderr);
    return 2;
}

int main(int argc, char** argv)
{
    const char*     role       = NULL;
    const char*     group      = NULL;
    const char*     cipher     = NULL;
    const char*     seed       = NULL;
    const char*     closer     = "client";
    unsigned long   port       = 0UL;
    unsigned long   version    = 1UL;
    static peer_t   peer;
    peer.max_streams    = 16U;
    peer.datagram_limit = MAX_DATAGRAM_BYTES;
    peer.connections    = 1U;
    peer.number         = 1U;
    httpx_default_options(&peer.http3);
    for (int at = 1; at + 1 < argc; at += 2)
    {
        const char* value = argv[at + 1];
        if (httpx_parse_option(&peer.http3, argv[at], value))
        {
            continue;
        }
        if (strcmp(argv[at], "--role") == 0)
        {
            role = value;
        }
        else if (strcmp(argv[at], "--port") == 0)
        {
            port = strtoul(value, NULL, 10);
        }
        else if (strcmp(argv[at], "--seed") == 0)
        {
            seed = value;
        }
        else if (strcmp(argv[at], "--group") == 0)
        {
            group = value;
        }
        else if (strcmp(argv[at], "--cipher") == 0)
        {
            cipher = value;
        }
        else if (strcmp(argv[at], "--version") == 0)
        {
            version = strtoul(value, NULL, 10);
        }
        else if (strcmp(argv[at], "--streams") == 0)
        {
            peer.streams = strtoull(value, NULL, 10);
        }
        else if (strcmp(argv[at], "--bytes") == 0)
        {
            peer.bytes = strtoull(value, NULL, 10);
        }
        else if (strcmp(argv[at], "--closer") == 0)
        {
            closer = value;
        }
        else if (strcmp(argv[at], "--code") == 0)
        {
            peer.code = strtoull(value, NULL, 10);
        }
        else if (strcmp(argv[at], "--max-streams") == 0)
        {
            peer.max_streams = strtoull(value, NULL, 10);
        }
        else if (strcmp(argv[at], "--compat") == 0)
        {
            peer.compat = atoi(value);
        }
        else if (strcmp(argv[at], "--retry") == 0)
        {
            peer.retry = atoi(value);
        }
        else if (strcmp(argv[at], "--key-update") == 0)
        {
            peer.key_update_packets = strtoull(value, NULL, 10);
        }
        else if (strcmp(argv[at], "--datagrams") == 0)
        {
            peer.datagrams = strtoull(value, NULL, 10);
        }
        else if (strcmp(argv[at], "--datagram-bytes") == 0)
        {
            peer.datagram_bytes = strtoull(value, NULL, 10);
        }
        else if (strcmp(argv[at], "--datagram-limit") == 0)
        {
            peer.datagram_limit = strtoull(value, NULL, 10);
        }
        else if (strcmp(argv[at], "--datagram-probe") == 0)
        {
            peer.datagram_probe = atoi(value);
        }
        else if (strcmp(argv[at], "--connections") == 0)
        {
            peer.connections = strtoull(value, NULL, 10);
        }
        else if (strcmp(argv[at], "--early") == 0)
        {
            peer.early = atoi(value);
        }
        else if (strcmp(argv[at], "--reject-early") == 0)
        {
            peer.reject_early = atoi(value);
        }
        else if (strcmp(argv[at], "--only-version") == 0)
        {
            peer.only_version = atoi(value);
        }
        else if (strcmp(argv[at], "--progress-at") == 0)
        {
            peer.progress_at = strtoull(value, NULL, 10);
        }
        else if (strcmp(argv[at], "--resets") == 0)
        {
            peer.resets = atoi(value);
        }
        else
        {
            return usage();
        }
    }
    if (argc % 2 == 0 || role == NULL || group == NULL || cipher == NULL || seed == NULL ||
        !httpx_options_valid(&peer.http3) || (version != 1UL && version != 2UL) || peer.streams > MAX_STREAMS || !parse_seed(seed, peer.pin) ||
        peer.datagrams > MAX_DATAGRAMS || peer.datagram_bytes + 1U > MAX_DATAGRAM_BYTES || peer.connections == 0U ||
        (peer.datagrams != 0U && peer.datagram_bytes < DATAGRAM_INDEX_BYTES))
    {
        return usage();
    }
    peer.client  = strcmp(role, "client") == 0;
    peer.closer  = strcmp(closer, "client") == 0;
    peer.version = version == 2UL ? NGTCP2_PROTO_VER_V2 : NGTCP2_PROTO_VER_V1;
    peer.table   = calloc(MAX_STREAMS, sizeof(stream_t));
    fill_pattern();
    if (peer.table == NULL || ngtcp2_crypto_ossl_init() != 0 || !make_tls(&peer, group, cipher, peer.pin))
    {
        ERR_print_errors_fp(stderr);
        fputs("crd-ngtcp2-quic-peer: TLS could not be set up\n", stderr);
        return 1;
    }
    peer.fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in bound;
    memset(&bound, 0, sizeof(bound));
    bound.sin_family      = AF_INET;
    bound.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bound.sin_port        = peer.client ? 0U : htons((uint16_t)port);
    socklen_t size        = sizeof(bound);
    const int receive_tos = 1;
    if (peer.fd < 0 || setsockopt(peer.fd, IPPROTO_IP, IP_RECVTOS, &receive_tos, sizeof(receive_tos)) != 0 ||
        bind(peer.fd, (struct sockaddr*)&bound, sizeof(bound)) != 0 ||
        getsockname(peer.fd, (struct sockaddr*)&peer.local, &size) != 0)
    {
        perror("crd-ngtcp2-quic-peer: socket");
        return 1;
    }
    if (peer.client)
    {
        peer.remote           = bound;
        peer.remote.sin_port  = htons((uint16_t)port);
    }
    else
    {
        printf("port=%u\n", (unsigned)ntohs(peer.local.sin_port));
        fflush(stdout);
    }
    const uint64_t connections = peer.connections;
    for (uint64_t connection = 1U; connection <= connections; ++connection)
    {
        if (peer.client && !begin_client(&peer))
        {
            fputs("crd-ngtcp2-quic-peer: the connection could not be made\n", stderr);
            return 1;
        }
        if (!run(&peer) || (connection < connections && !next_connection(&peer)))
        {
            return 1;
        }
    }
    return 0;
}
