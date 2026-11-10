/* crd-net congestion-control interoperability test (NET-PATH) -- a QUIC endpoint over the pinned picoquic and picotls,
 * real UDP on the loopback, the picoquic counterpart of cerid_quic_peer.cpp. picoquic_interop.py runs one of each and
 * reads both reports.
 *
 * Usage: crd-picoquic-quic-peer --role client|server --port N --seed HEX --version 1|2 --streams N --bytes N
 *                               --code N [--cc prague|cubic|newreno|bbr]
 *
 *   port     a server's to bind on 127.0.0.1 (0: any, printed as `port=N`), a client's to reach
 *   seed     the 32-byte Ed25519 private key of the server's self-signed credential; a client pins its public key and
 *            checks the handshake's signature with it
 *   streams  bidirectional streams a client opens, each carrying `bytes` bytes and a FIN; a server echoes as many bytes
 *            as it reads and its FIN after the client's. Both ends check every byte (http_exchange.h's pattern, tag 0
 *            from the client and 1 back)
 *   code     the application error the client closes with once every echo has ended
 *   cc       picoquic's own controller (prague.c, cubic.c, newreno.c, bbr.c; Prague by default). The socket marks every
 *            datagram with the codepoint that controller asks for, ECT(1) for Prague and ECT(0) for the others, as
 *            picoquic's own packet loop does (sockloop.c)
 *
 * The loop is picoquic's API over its own socket helpers (picosocks.h: ECN and the destination address read from
 * each datagram's control data), driven here so that a server binds port 0 and reports it and each end stops when its
 * one connection has ended. The report, one `name=value` per line from `connection=1` on standard output when the
 * connection has ended: `established`, `version`, `streams` (FINs read), `bytes_received`, `bytes_sent`, `verified`,
 * `close` (local, application, transport, reset or idle) and `close_code`, `pin`, `alpn`, the controller
 * (`congestion`) and the codepoint it marks (`ecn_mark`), the codepoints of the packets read (`ecn_received_ect0`,
 * `_ect1`, `_ce`) and those the peer's ACK frames reported of this end's (`ecn_acked_ect0`, `_ect1`, `_ce`), the
 * window when the connection ended (`cwin`), the packets declared lost (`losses`), the path MTU picoquic's search
 * reached (`pmtu`) and the peer's max_udp_payload_size (`peer_max_udp_payload`). The application is `cerid-interop`,
 * a private ALPN. */
#define _GNU_SOURCE /* ppoll */
#include "../../crypto/interop/openssl_self_signed.h"
#include "http_exchange.h"

#include <picoquic.h>
#include <picoquic_internal.h>
#include <picoquic_utils.h>
#include <picosocks.h>
#include <picotls.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
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
#include <unistd.h>

#define MAX_STREAMS 64U
#define RUN_US (60U * 1000000U)
#define WAKE_MAX_US 100000
#define DATAGRAM_BUFFER 1536U

static const char ALPN[]        = "cerid-interop";
static const char SERVER_NAME[] = "cerid.test";
static const uint16_t ED25519_ONLY[] = {PTLS_SIGNATURE_ED25519, UINT16_MAX};

typedef struct
{
    uint64_t received;
    uint64_t sent;
    int      fin_in;
    int      fin_out;
} stream_t;

/* What the report reads, taken while the connection lives (a server's picoquic deletes it once it has ended), and
 * `established` from the ready callback on. */
typedef struct
{
    int      established;
    unsigned version;
    char     alpn[32];
    char     congestion[16];
    uint64_t ecn_received[3]; /* ECT(0), ECT(1), CE */
    uint64_t ecn_acked[3];
    uint64_t cwin;
    uint64_t losses;
    uint64_t pmtu;
    uint64_t peer_max_udp_payload;
} snapshot_t;

typedef struct
{
    ptls_verify_certificate_t super;
    unsigned char             expected[32]; /* the pinned Ed25519 public key */
    int                       matched;
} pin_verifier_t;

typedef struct
{
    int            client;
    uint64_t       streams;
    uint64_t       bytes;
    uint64_t       code;
    uint8_t        ecn_mark;
    stream_t       table[MAX_STREAMS];
    picoquic_cnx_t* cnx; /* a client's, from creation; a server's, from its first callback until it ends */
    snapshot_t     snapshot;
    uint64_t       bytes_received;
    uint64_t       bytes_sent;
    uint64_t       fins;
    int            unverified;
    int            closed_locally;
    const char*    close_kind;
    uint64_t       close_code;
    pin_verifier_t pin;
} peer_t;

static stream_t* stream_of(peer_t* peer, uint64_t id)
{
    const uint64_t index = id / 4U;
    if ((id & 3U) != 0U || index >= peer->streams || index >= MAX_STREAMS)
    {
        return NULL;
    }
    return &peer->table[index];
}

/* Reads one stream's bytes: each held to the pattern of its direction, a server's echo owed for each. */
static int on_data(peer_t* peer, picoquic_cnx_t* cnx, uint64_t id, const uint8_t* bytes, size_t length, int fin)
{
    stream_t* stream = stream_of(peer, id);
    if (stream == NULL || stream->fin_in)
    {
        peer->unverified = 1;
        return -1;
    }
    const uint64_t tag = peer->client ? 1U : 0U;
    for (size_t at = 0; at < length; ++at)
    {
        if (bytes[at] != httpx_pattern(tag, id, stream->received + at))
        {
            peer->unverified = 1;
        }
    }
    stream->received += length;
    peer->bytes_received += length;
    if (fin)
    {
        stream->fin_in = 1;
        ++peer->fins;
    }
    if (peer->client)
    {
        if (peer->fins == peer->streams && !peer->closed_locally)
        {
            peer->closed_locally = 1;
            peer->close_kind     = "local";
            peer->close_code     = peer->code;
            return picoquic_close(cnx, peer->code);
        }
        return 0;
    }
    return (stream->sent < stream->received || stream->fin_in) ? picoquic_mark_active_stream(cnx, id, 1, NULL) : 0;
}

/* Writes what a stream owes: a client its whole transfer, a server what it has read, each FIN after the last byte. */
static int on_send(peer_t* peer, uint64_t id, uint8_t* context, size_t room)
{
    stream_t* stream = stream_of(peer, id);
    if (stream == NULL || stream->fin_out)
    {
        (void)picoquic_provide_stream_data_buffer(context, 0U, 0, 0);
        return 0;
    }
    const uint64_t goal   = peer->client ? peer->bytes : stream->received;
    const uint64_t left   = goal - stream->sent;
    const size_t   amount = left < room ? (size_t)left : room;
    const int      fin    = (peer->client || stream->fin_in) && stream->sent + amount == goal;
    if (amount == 0U && !fin)
    {
        (void)picoquic_provide_stream_data_buffer(context, 0U, 0, 0); /* nothing owed until more is read */
        return 0;
    }
    uint8_t* out = picoquic_provide_stream_data_buffer(context, amount, fin, stream->sent + amount < goal);
    if (out == NULL)
    {
        return -1;
    }
    const uint64_t tag = peer->client ? 0U : 1U;
    for (size_t at = 0; at < amount; ++at)
    {
        out[at] = httpx_pattern(tag, id, stream->sent + at);
    }
    stream->sent += amount;
    peer->bytes_sent += amount;
    stream->fin_out = fin;
    return 0;
}

static int on_event(picoquic_cnx_t* cnx, uint64_t id, uint8_t* bytes, size_t length,
                    picoquic_call_back_event_t event, void* context, void* stream_context)
{
    (void)stream_context;
    peer_t* peer = (peer_t*)context;
    peer->cnx    = cnx;
    switch (event)
    {
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        return on_data(peer, cnx, id, bytes, length, event == picoquic_callback_stream_fin);
    case picoquic_callback_prepare_to_send:
        return on_send(peer, id, bytes, length);
    case picoquic_callback_ready:
        peer->snapshot.established = 1;
        if (peer->client)
        {
            for (uint64_t index = 0; index < peer->streams; ++index)
            {
                if (picoquic_mark_active_stream(cnx, index * 4U, 1, NULL) != 0)
                {
                    return -1;
                }
            }
        }
        return 0;
    case picoquic_callback_application_close:
        if (peer->close_kind == NULL)
        {
            peer->close_kind = "application";
            peer->close_code = picoquic_get_application_error(cnx);
        }
        return 0;
    case picoquic_callback_close:
        /* Also the last call of every connection picoquic deletes (picoquic_connection_disconnect), after the close
         * that ended it. */
        if (peer->close_kind == NULL)
        {
            peer->close_kind = "transport";
            peer->close_code = picoquic_get_remote_error(cnx);
        }
        return 0;
    case picoquic_callback_stateless_reset:
        if (peer->close_kind == NULL)
        {
            peer->close_kind = "reset";
        }
        return 0;
    case picoquic_callback_stream_reset:
    case picoquic_callback_stop_sending:
        peer->unverified = 1;
        return 0;
    default:
        return 0;
    }
}

/* The Ed25519 signature of the handshake, by the pinned key the certificate carried; called once, or with no data to
 * release the key when the handshake fails first. */
static int verify_sign(void* context, uint16_t algorithm, ptls_iovec_t data, ptls_iovec_t signature)
{
    EVP_PKEY* key = (EVP_PKEY*)context;
    int       ret = PTLS_ALERT_DECRYPT_ERROR;
    if (data.base == NULL)
    {
        ret = 0;
    }
    else if (algorithm == PTLS_SIGNATURE_ED25519)
    {
        EVP_MD_CTX* digest = EVP_MD_CTX_new();
        if (digest != NULL && EVP_DigestVerifyInit(digest, NULL, NULL, NULL, key) == 1 &&
            EVP_DigestVerify(digest, signature.base, signature.len, data.base, data.len) == 1)
        {
            ret = 0;
        }
        EVP_MD_CTX_free(digest);
    }
    EVP_PKEY_free(key);
    return ret;
}

/* A client's check of the server's certificate: its key must be the pinned one, which then checks the signature. */
static int verify_certificate(ptls_verify_certificate_t* self, ptls_t* tls, const char* server_name,
                              int (**verifier)(void*, uint16_t, ptls_iovec_t, ptls_iovec_t), void** verify_data,
                              ptls_iovec_t* certs, size_t num_certs)
{
    (void)tls;
    pin_verifier_t*      pin  = (pin_verifier_t*)self;
    const unsigned char* der  = num_certs > 0U ? certs[0].base : NULL;
    X509*                leaf = der != NULL ? d2i_X509(NULL, &der, (long)certs[0].len) : NULL;
    EVP_PKEY*            key  = leaf != NULL ? X509_get_pubkey(leaf) : NULL;
    unsigned char        found[32];
    size_t               found_size = sizeof(found);
    pin->matched = server_name != NULL && strcmp(server_name, SERVER_NAME) == 0 && key != NULL &&
                   EVP_PKEY_get_raw_public_key(key, found, &found_size) == 1 && found_size == sizeof(found) &&
                   memcmp(found, pin->expected, sizeof(found)) == 0;
    X509_free(leaf);
    if (!pin->matched)
    {
        EVP_PKEY_free(key);
        return PTLS_ALERT_BAD_CERTIFICATE;
    }
    *verifier    = verify_sign;
    *verify_data = key;
    return 0;
}

static void keep_verifier(ptls_verify_certificate_t* verifier)
{
    (void)verifier; /* the peer owns it */
}

/* The server's credential as picoquic reads it, from PEM files: written into a private directory, read by
 * picoquic_create and removed. */
static picoquic_quic_t* create_server(peer_t* peer, EVP_PKEY* key, uint64_t now)
{
    char  directory[] = "/tmp/crd-picoquic-XXXXXX";
    char  cert_path[64];
    char  key_path[64];
    X509* certificate = self_signed(key, SERVER_NAME);
    if (certificate == NULL || mkdtemp(directory) == NULL)
    {
        X509_free(certificate);
        return NULL;
    }
    (void)snprintf(cert_path, sizeof(cert_path), "%s/cert.pem", directory);
    (void)snprintf(key_path, sizeof(key_path), "%s/key.pem", directory);
    FILE* cert_file = fopen(cert_path, "wb");
    FILE* key_file  = fopen(key_path, "wb");
    int   written   = cert_file != NULL && key_file != NULL && PEM_write_X509(cert_file, certificate) == 1 &&
                   PEM_write_PrivateKey(key_file, key, NULL, NULL, 0, NULL, NULL) == 1;
    written = (cert_file == NULL || fclose(cert_file) == 0) && written;
    written = (key_file == NULL || fclose(key_file) == 0) && written;
    X509_free(certificate);
    picoquic_quic_t* quic = written ? picoquic_create(1U, cert_path, key_path, NULL, ALPN, on_event, peer, NULL, NULL,
                                                      NULL, now, NULL, NULL, NULL, 0U)
                                    : NULL;
    (void)unlink(cert_path);
    (void)unlink(key_path);
    (void)rmdir(directory);
    return quic;
}

static void take_snapshot(peer_t* peer, picoquic_cnx_t* cnx)
{
    snapshot_t*    out     = &peer->snapshot;
    const uint32_t version = picoquic_supported_versions[cnx->version_index].version;
    out->version = version == 1U ? 1U : (version == PICOQUIC_V2_VERSION ? 2U : 0U);
    const char* alpn = picoquic_tls_get_negotiated_alpn(cnx);
    (void)snprintf(out->alpn, sizeof(out->alpn), "%s", alpn != NULL ? alpn : "");
    (void)snprintf(out->congestion, sizeof(out->congestion), "%s",
                   cnx->congestion_alg != NULL ? cnx->congestion_alg->congestion_algorithm_id : "none");
    const picoquic_ack_context_t*    acks    = &cnx->ack_ctx[picoquic_packet_context_application];
    const picoquic_packet_context_t* packets = &cnx->pkt_ctx[picoquic_packet_context_application];
    out->ecn_received[0] = acks->ecn_ect0_total_local;
    out->ecn_received[1] = acks->ecn_ect1_total_local;
    out->ecn_received[2] = acks->ecn_ce_total_local;
    out->ecn_acked[0]    = packets->ecn_ect0_total_remote;
    out->ecn_acked[1]    = packets->ecn_ect1_total_remote;
    out->ecn_acked[2]    = packets->ecn_ce_total_remote;
    out->cwin            = cnx->path[0]->cwin;
    out->losses          = cnx->nb_retransmission_total;
    out->pmtu            = cnx->path[0]->send_mtu;
    out->peer_max_udp_payload = cnx->remote_parameters_received ? cnx->remote_parameters.max_packet_size : 0U;
}

static void report(const peer_t* peer)
{
    static const char* const codepoints[4] = {"not-ect", "ect1", "ect0", "ce"};
    const snapshot_t*        s             = &peer->snapshot;
    printf("connection=1\nestablished=%d\nversion=%u\nstreams=%llu\nbytes_received=%llu\nbytes_sent=%llu\n"
           "verified=%d\n",
           s->established, s->version, (unsigned long long)peer->fins, (unsigned long long)peer->bytes_received,
           (unsigned long long)peer->bytes_sent, !peer->unverified);
    printf("close=%s\nclose_code=%llu\npin=%d\nalpn=%s\ncongestion=%s\necn_mark=%s\n",
           peer->close_kind != NULL ? peer->close_kind : "idle", (unsigned long long)peer->close_code,
           peer->pin.matched, s->alpn, s->congestion, codepoints[peer->ecn_mark & 3U]);
    printf("ecn_received_ect0=%llu\necn_received_ect1=%llu\necn_received_ce=%llu\n"
           "ecn_acked_ect0=%llu\necn_acked_ect1=%llu\necn_acked_ce=%llu\n",
           (unsigned long long)s->ecn_received[0], (unsigned long long)s->ecn_received[1],
           (unsigned long long)s->ecn_received[2], (unsigned long long)s->ecn_acked[0],
           (unsigned long long)s->ecn_acked[1], (unsigned long long)s->ecn_acked[2]);
    printf("cwin=%llu\nlosses=%llu\npmtu=%llu\npeer_max_udp_payload=%llu\n", (unsigned long long)s->cwin,
           (unsigned long long)s->losses, (unsigned long long)s->pmtu, (unsigned long long)s->peer_max_udp_payload);
    fflush(stdout);
}

/* Every datagram waiting on the socket, handed to picoquic with its ECN codepoint and destination. */
static void receive_all(picoquic_quic_t* quic, int fd, uint16_t port)
{
    uint8_t buffer[DATAGRAM_BUFFER];
    for (;;)
    {
        struct sockaddr_storage from;
        struct sockaddr_storage to;
        int                     interface = 0;
        unsigned char           ecn       = 0;
        memset(&to, 0, sizeof(to));
        const int length =
            picoquic_recvmsg_ex(fd, &from, &to, &interface, &ecn, buffer, (int)sizeof(buffer), MSG_DONTWAIT);
        if (length <= 0)
        {
            return;
        }
        struct sockaddr_in* local = (struct sockaddr_in*)&to;
        if (local->sin_port == 0U)
        {
            local->sin_port = htons(port);
        }
        picoquic_cnx_t* last = NULL;
        (void)picoquic_incoming_packet_ex(quic, buffer, (size_t)length, (struct sockaddr*)&from,
                                          (struct sockaddr*)&to, interface, ecn, &last, picoquic_current_time());
    }
}

/* Every datagram picoquic has ready now, its pacer and window permitting. */
static void send_all(picoquic_quic_t* quic, int fd)
{
    uint8_t buffer[DATAGRAM_BUFFER];
    for (;;)
    {
        struct sockaddr_storage to;
        struct sockaddr_storage from;
        size_t                  length    = 0U;
        int                     interface = 0;
        picoquic_cnx_t*         last      = NULL;
        if (picoquic_prepare_next_packet(quic, picoquic_current_time(), buffer, sizeof(buffer), &length, &to, &from,
                                         &interface, NULL, &last) != 0 ||
            length == 0U)
        {
            return;
        }
        /* A full socket buffer drops the datagram, a loss picoquic recovers from like any other. */
        (void)sendto(fd, buffer, length, 0, (struct sockaddr*)&to, sizeof(struct sockaddr_in));
    }
}

/* Runs until this end's one connection has ended: a client's reaches the disconnected state, a server's is deleted. */
static int run(peer_t* peer, picoquic_quic_t* quic, int fd, uint16_t port)
{
    const uint64_t deadline = picoquic_current_time() + RUN_US;
    int            seen     = peer->client;
    for (;;)
    {
        send_all(quic, fd);
        picoquic_cnx_t* cnx = peer->client ? peer->cnx : picoquic_get_first_cnx(quic);
        if (cnx != NULL)
        {
            seen = 1;
            take_snapshot(peer, cnx);
        }
        if (seen && (cnx == NULL || picoquic_get_cnx_state(cnx) == picoquic_state_disconnected))
        {
            return 0;
        }
        const uint64_t now = picoquic_current_time();
        if (now >= deadline)
        {
            return 1;
        }
        const int64_t   delay = picoquic_get_next_wake_delay(quic, now, WAKE_MAX_US);
        struct pollfd   ready = {fd, POLLIN, 0};
        struct timespec wait  = {0, (long)(delay > 0 ? delay : 0) * 1000L};
        if (ppoll(&ready, 1U, &wait, NULL) > 0)
        {
            receive_all(quic, fd, port);
        }
    }
}

static int parse_seed(const char* text, unsigned char* out)
{
    if (text == NULL || strlen(text) != 64U)
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
    fputs("usage: crd-picoquic-quic-peer --role client|server --port N --seed HEX --version 1|2 --streams N --bytes N "
          "--code N [--cc prague|cubic|newreno|bbr]\n",
          stderr);
    return 2;
}

int main(int argc, char** argv)
{
    static peer_t peer;
    const char*   role    = NULL;
    const char*   seed    = NULL;
    const char*   cc      = "prague";
    unsigned long port    = 0UL;
    unsigned long version = 1UL;
    for (int at = 1; at + 1 < argc; at += 2)
    {
        const char* name  = argv[at];
        const char* value = argv[at + 1];
        if (strcmp(name, "--role") == 0)
        {
            role = value;
        }
        else if (strcmp(name, "--port") == 0)
        {
            port = strtoul(value, NULL, 10);
        }
        else if (strcmp(name, "--seed") == 0)
        {
            seed = value;
        }
        else if (strcmp(name, "--version") == 0)
        {
            version = strtoul(value, NULL, 10);
        }
        else if (strcmp(name, "--streams") == 0)
        {
            peer.streams = strtoull(value, NULL, 10);
        }
        else if (strcmp(name, "--bytes") == 0)
        {
            peer.bytes = strtoull(value, NULL, 10);
        }
        else if (strcmp(name, "--code") == 0)
        {
            peer.code = strtoull(value, NULL, 10);
        }
        else if (strcmp(name, "--cc") == 0)
        {
            cc = value;
        }
        else
        {
            return usage();
        }
    }
    unsigned char private_key[32];
    picoquic_register_all_congestion_control_algorithms();
    const picoquic_congestion_algorithm_t* algorithm = picoquic_get_congestion_algorithm(cc);
    peer.client = role != NULL && strcmp(role, "client") == 0;
    if (role == NULL || (!peer.client && strcmp(role, "server") != 0) || (argc % 2) == 0 || port > 65535UL ||
        (version != 1UL && version != 2UL) || peer.streams > MAX_STREAMS || !parse_seed(seed, private_key) ||
        algorithm == NULL)
    {
        return usage();
    }
    peer.ecn_mark  = algorithm->ecn_mark;
    EVP_PKEY* key  = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key, sizeof(private_key));
    size_t    size = sizeof(peer.pin.expected);
    if (key == NULL || EVP_PKEY_get_raw_public_key(key, peer.pin.expected, &size) != 1)
    {
        fputs("crd-picoquic-quic-peer: no Ed25519 key from the seed\n", stderr);
        return 1;
    }
    const uint64_t   now  = picoquic_current_time();
    picoquic_quic_t* quic = peer.client ? picoquic_create(1U, NULL, NULL, NULL, ALPN, on_event, &peer, NULL, NULL, NULL,
                                                          now, NULL, NULL, NULL, 0U)
                                        : create_server(&peer, key, now);
    EVP_PKEY_free(key);
    if (quic == NULL || picoquic_set_default_congestion_algorithm_by_name(quic, cc) != 0)
    {
        fputs("crd-picoquic-quic-peer: no picoquic context\n", stderr);
        return 1;
    }
    if (peer.client)
    {
        peer.pin.super.cb    = verify_certificate;
        peer.pin.super.algos = ED25519_ONLY;
        picoquic_set_verify_certificate_callback(quic, &peer.pin.super, keep_verifier);
        picoquic_set_client_cert_verification_policy(quic, 1);
    }

    struct sockaddr_in local = {0};
    local.sin_family         = AF_INET;
    local.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    local.sin_port           = htons(peer.client ? 0U : (uint16_t)port);
    socklen_t length         = sizeof(local);
    int       recv_set       = 0;
    int       send_set       = 0;
    const int fd             = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0 || bind(fd, (struct sockaddr*)&local, sizeof(local)) != 0 ||
        getsockname(fd, (struct sockaddr*)&local, &length) != 0 ||
        picoquic_socket_set_ecn_options_ex(fd, AF_INET, &recv_set, &send_set, peer.ecn_mark) != 0 ||
        picoquic_socket_set_pkt_info(fd, AF_INET) != 0 || picoquic_socket_set_pmtud_options(fd, AF_INET) != 0 ||
        !recv_set || (peer.ecn_mark != 0U && !send_set))
    {
        fprintf(stderr, "crd-picoquic-quic-peer: socket setup failed (%d)\n", errno);
        return 1;
    }
    if (peer.client)
    {
        struct sockaddr_in remote = {0};
        remote.sin_family         = AF_INET;
        remote.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
        remote.sin_port           = htons((uint16_t)port);
        peer.cnx = picoquic_create_cnx(quic, picoquic_null_connection_id, picoquic_null_connection_id,
                                       (struct sockaddr*)&remote, picoquic_current_time(),
                                       version == 2UL ? PICOQUIC_V2_VERSION : 1U, SERVER_NAME, ALPN, 1);
        if (peer.cnx == NULL)
        {
            return 1;
        }
        picoquic_set_callback(peer.cnx, on_event, &peer);
        if (picoquic_start_client_cnx(peer.cnx) != 0)
        {
            return 1;
        }
    }
    else
    {
        printf("port=%u\n", (unsigned)ntohs(local.sin_port));
        fflush(stdout);
    }
    const int timed_out = run(&peer, quic, fd, ntohs(local.sin_port));
    report(&peer);
    picoquic_free(quic);
    (void)close(fd);
    return timed_out;
}
