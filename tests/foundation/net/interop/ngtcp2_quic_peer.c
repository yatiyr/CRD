/* crd-net interoperability test (4.2a) -- a QUIC endpoint over the pinned ngtcp2 and OpenSSL 4.0, real UDP on the
 * loopback, the counterpart of cerid_quic_peer.cpp. quic_interop.py runs one of each and reads both reports.
 *
 * Usage: crd-ngtcp2-quic-peer --role client|server --port N --seed HEX --group NAME --cipher NAME --version 1|2
 *                             --streams N --bytes N --closer client|server --code N --max-streams N [--compat 1]
 *                             [--retry 1] [--key-update N]
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
 *
 * The report, one `name=value` per line on standard output when the connection has ended, is the same as
 * cerid_quic_peer.cpp's plus what OpenSSL negotiated. The application is `cerid-interop`, a private ALPN. */
#define _GNU_SOURCE /* ppoll */
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define PATTERN_BYTES 65536U
#define MAX_STREAMS 4096U
#define DATAGRAM_BYTES 1452U
#define RUN_SECONDS 60U
#define CLOSING_QUIET_NS (300U * NGTCP2_MILLISECONDS)
#define CLOSING_NS (2U * NGTCP2_SECONDS)
#define RETRY_TOKEN_NS (10U * NGTCP2_SECONDS)

static const unsigned char ALPN[] = "\x0D" "cerid-interop";
static const char SERVER_NAME[]   = "cerid.test";
static const uint8_t RETRY_SECRET[32] = {0x52, 0x65, 0x74};
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
    uint64_t                opened;
    uint64_t                done;
    uint64_t                cursor;
    uint64_t                bytes_received;
    uint64_t                bytes_sent;
    uint64_t                key_update_packets; /* 0: this end starts none */
    uint64_t                packets_since_update;
    uint64_t                key_updates;
    int                     compat;
    int                     retry;
    int                     verified;
    int                     established;
    int                     pin_matched;
} peer_t;

/* The byte at `offset` of a stream's direction (tag 0: client to server, 1: server to client); a 64 KiB period, so
 * what ngtcp2 keeps until it is acknowledged can point straight into the table. */
static uint8_t pattern[2][PATTERN_BYTES];

static size_t pattern_index(int64_t stream, uint64_t offset)
{
    return (size_t)((offset + ((uint64_t)stream * 37U)) & (PATTERN_BYTES - 1U));
}

static void fill_pattern(void)
{
    for (uint32_t tag = 0; tag < 2U; ++tag)
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
    if (RAND_bytes(cid->data, (int)size) != 1 || RAND_bytes(token->data, sizeof(token->data)) != 1)
    {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    cid->datalen = size;
    return 0;
}

static stream_t* stream_of(peer_t* peer, int64_t id)
{
    const uint64_t index = (uint64_t)id >> 2U;
    return (index < MAX_STREAMS && (id & 3) == 0) ? &peer->table[index] : NULL;
}

static int stream_open_cb(ngtcp2_conn* conn, int64_t id, void* user)
{
    (void)conn;
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
    peer_t*   peer   = user;
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
            peer->verified = 0;
        }
    }
    if (offset != stream->received)
    {
        peer->verified = 0;
    }
    stream->received += size;
    peer->bytes_received += size;
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
    (void)flags;
    (void)rx_error;
    (void)tx_error;
    (void)stream_user;
    peer_t*   peer   = user;
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

static int update_key(ngtcp2_conn* conn, uint8_t* rx_secret, uint8_t* tx_secret, ngtcp2_crypto_aead_ctx* rx_aead_ctx,
                      uint8_t* rx_iv, ngtcp2_crypto_aead_ctx* tx_aead_ctx, uint8_t* tx_iv,
                      const uint8_t* current_rx_secret, const uint8_t* current_tx_secret, size_t secret_size,
                      void* user)
{
    ++((peer_t*)user)->key_updates;
    return ngtcp2_crypto_update_key_cb(conn, rx_secret, tx_secret, rx_aead_ctx, rx_iv, tx_aead_ctx, tx_iv,
                                       current_rx_secret, current_tx_secret, secret_size, user);
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
    (void)user;
    unsigned char* selected = NULL;
    if (SSL_select_next_proto(&selected, out_size, ALPN, sizeof(ALPN) - 1U, in, in_size) != OPENSSL_NPN_NEGOTIATED)
    {
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    *out = selected;
    return SSL_TLSEXT_ERR_OK;
}

/* A connection's SSL over the context; made anew when a server drops a connection that could not start. */
static int make_session(peer_t* peer)
{
    peer->ssl = SSL_new(peer->ctx);
    if (peer->ssl == NULL || ngtcp2_crypto_ossl_ctx_new(&peer->ossl, NULL) != 0)
    {
        return 0;
    }
    ngtcp2_crypto_ossl_ctx_set_ssl(peer->ossl, peer->ssl);
    peer->ref.get_conn  = conn_of;
    peer->ref.user_data = peer;
    SSL_set_app_data(peer->ssl, &peer->ref);
    if (peer->client)
    {
        SSL_set_connect_state(peer->ssl);
        return ngtcp2_crypto_ossl_configure_client_session(peer->ssl) == 0 &&
               SSL_set_alpn_protos(peer->ssl, ALPN, sizeof(ALPN) - 1U) == 0 &&
               SSL_set_tlsext_host_name(peer->ssl, SERVER_NAME) == 1;
    }
    SSL_set_accept_state(peer->ssl);
    return ngtcp2_crypto_ossl_configure_server_session(peer->ssl) == 0;
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
        SSL_CTX_set_alpn_select_cb(peer->ctx, select_alpn, NULL);
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

static void default_callbacks(ngtcp2_callbacks* callbacks)
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
    if (peer->compat)
    {
        settings->preferred_versions    = PREFER_V2;
        settings->preferred_versionslen = 2U;
        settings->available_versions    = peer->client ? OFFER_BOTH : PREFER_V2;
        settings->available_versionslen = 2U;
    }
}

static int start_client(peer_t* peer)
{
    ngtcp2_callbacks        callbacks;
    ngtcp2_settings         settings;
    ngtcp2_transport_params params;
    ngtcp2_cid              dcid;
    ngtcp2_cid              scid;
    default_callbacks(&callbacks);
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
    return 1;
}

static int send_datagram(peer_t* peer, const uint8_t* data, size_t size);

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
    return wrote > 0 && send_datagram(peer, packet, (size_t)wrote);
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
    default_callbacks(&callbacks);
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
    scid.datalen                         = 16U;
    if (RAND_bytes(scid.data, (int)scid.datalen) != 1 ||
        RAND_bytes(params.stateless_reset_token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1)
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

static int send_datagram(peer_t* peer, const uint8_t* data, size_t size)
{
    ssize_t sent;
    do
    {
        sent = sendto(peer->fd, data, size, 0, (const struct sockaddr*)&peer->remote, sizeof(peer->remote));
    } while (sent < 0 && errno == EINTR);
    /* A full socket buffer loses a datagram, as a path may. */
    return sent >= 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS;
}

static void open_streams(peer_t* peer)
{
    while (peer->client && peer->opened < peer->streams && ngtcp2_conn_get_handshake_completed2(peer->conn))
    {
        int64_t id = -1;
        if (ngtcp2_conn_open_bidi_stream(peer->conn, &id, NULL) != 0)
        {
            return;
        }
        stream_t* stream = stream_of(peer, id);
        if (stream == NULL)
        {
            abort();
        }
        stream->opened = 1;
        ++peer->opened;
    }
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

static int write_streams(peer_t* peer)
{
    uint8_t            datagram[DATAGRAM_BYTES];
    ngtcp2_path_storage storage;
    ngtcp2_path_storage_zero(&storage);
    for (uint64_t index = 0; index < MAX_STREAMS; ++index)
    {
        peer->table[index].blocked = 0;
    }
    const int tag = peer->client ? 0 : 1;
    for (;;)
    {
        const uint64_t now = now_ns();
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
        ngtcp2_pkt_info info;
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
        if (!send_datagram(peer, datagram, (size_t)wrote))
        {
            return 0;
        }
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
    }
}

static int all_done(const peer_t* peer)
{
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
    printf("established=%d\nversion=%u\nstreams=%llu\nbytes_received=%llu\nbytes_sent=%llu\nverified=%d\n",
           peer->established,
           negotiated == NGTCP2_PROTO_VER_V2 ? 2U : (negotiated == NGTCP2_PROTO_VER_V1 ? 1U : 0U),
           (unsigned long long)ended, (unsigned long long)peer->bytes_received,
           (unsigned long long)peer->bytes_sent, peer->verified);
    printf("close=%s\nclose_code=%llu\npin=%d\nkey_updates=%llu\ngroup=%s\ncipher=%s\nalpn=%.*s\n", close_kind,
           (unsigned long long)close_code, peer->pin_matched, (unsigned long long)peer->key_updates,
           group != NULL ? group : "none",
           cipher != NULL ? SSL_CIPHER_get_name(cipher) : "none", (int)length, alpn != NULL ? (const char*)alpn : "");
    fflush(stdout);
}

/* The terminal packet of a close this end chooses, sent again for what still arrives (RFC 9000 section 10.2.1) until
 * the peer goes quiet, and the report. */
static void close_connection(peer_t* peer, uint64_t code)
{
    uint8_t             datagram[DATAGRAM_BYTES];
    ngtcp2_path_storage storage;
    ngtcp2_pkt_info     info;
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
    (void)send_datagram(peer, datagram, (size_t)wrote);
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
                (void)send_datagram(peer, datagram, (size_t)wrote);
                quiet = now_ns() + CLOSING_QUIET_NS;
            }
        }
    }
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

/* Reads every datagram waiting; 0 when the connection has ended (the report written). */
static int read_datagrams(peer_t* peer)
{
    uint8_t datagram[65536];
    for (;;)
    {
        struct sockaddr_in from;
        socklen_t          from_size = sizeof(from);
        const ssize_t      size      = recvfrom(peer->fd, datagram, sizeof(datagram), MSG_DONTWAIT,
                                                (struct sockaddr*)&from, &from_size);
        if (size < 0)
        {
            return 1;
        }
        if (peer->conn == NULL)
        {
            peer->remote = from;
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
        ngtcp2_pkt_info info;
        memset(&info, 0, sizeof(info));
        ngtcp2_path path = path_of(peer);
        const int result = ngtcp2_conn_read_pkt(peer->conn, &path, &info, datagram, (size_t)size, now_ns());
        if (result == NGTCP2_ERR_DRAINING)
        {
            const ngtcp2_ccerr* error = ngtcp2_conn_get_ccerr2(peer->conn);
            report(peer, close_name(error->type), error->error_code);
            return 0;
        }
        if ((result == NGTCP2_ERR_RETRY || result == NGTCP2_ERR_DROP_CONN) && !peer->client && !peer->established)
        {
            /* The client's first flight arrived out of order: drop the attempt and take its next Initial. */
            ngtcp2_conn_del(peer->conn);
            peer->conn = NULL;
            ngtcp2_crypto_ossl_ctx_del(peer->ossl);
            peer->ossl = NULL;
            SSL_free(peer->ssl);
            peer->ssl = NULL;
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
                return 0;
            }
        }
        open_streams(peer);
        if (!write_streams(peer))
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
          " [--key-update N]\n",
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
    peer.verified    = 1;
    peer.max_streams = 16U;
    for (int at = 1; at + 1 < argc; at += 2)
    {
        const char* value = argv[at + 1];
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
        else
        {
            return usage();
        }
    }
    if (argc % 2 == 0 || role == NULL || group == NULL || cipher == NULL || seed == NULL ||
        (version != 1UL && version != 2UL) || peer.streams > MAX_STREAMS || !parse_seed(seed, peer.pin))
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
    if (peer.fd < 0 || bind(peer.fd, (struct sockaddr*)&bound, sizeof(bound)) != 0 ||
        getsockname(peer.fd, (struct sockaddr*)&peer.local, &size) != 0)
    {
        perror("crd-ngtcp2-quic-peer: socket");
        return 1;
    }
    if (peer.client)
    {
        peer.remote           = bound;
        peer.remote.sin_port  = htons((uint16_t)port);
        if (!start_client(&peer))
        {
            fputs("crd-ngtcp2-quic-peer: the connection could not be made\n", stderr);
            return 1;
        }
        if (!write_streams(&peer))
        {
            return 1;
        }
    }
    else
    {
        printf("port=%u\n", (unsigned)ntohs(peer.local.sin_port));
        fflush(stdout);
    }
    return run(&peer) ? 0 : 1;
}
