/* crd-net transport budget board (NET-PATH) -- msquic's end of quic_board.py: the pinned msquic over the peers' OpenSSL
 * 4.0, real UDP on the loopback, against itself, the workloads of cerid_quic_board.cpp.
 *
 * Usage: crd-msquic-quic-board --role client|server --port N --seed HEX --connections N [--parallel N] [--bytes N]
 *                              [--window N] [--hold 0|1]
 *
 * The options and the report are cerid_quic_board.cpp's, with these differences, each msquic's own:
 *   - no --group: msquic over OpenSSL offers secp256r1 then x25519 on every connection (tls_openssl.c sets the list),
 *     so two msquic ends agree on secp256r1, and its handshake information leaves the group out: the report's `group`
 *     is `unreported`. The AEAD is AES-128-GCM (QUIC_CREDENTIAL_FLAG_SET_ALLOWED_CIPHER_SUITES).
 *   - no --offload: msquic uses the host's segmentation and coalescing wherever its start-up probe finds them, with no
 *     switch to turn them off.
 *   - a stream's receive window is `window` (StreamRecvWindowDefault, which msquic rounds up to a power of two), the
 *     connection's 16 of them; msquic buffers what the application sends (SendBufferingEnabled, its default), and a
 *     client keeps SEND_DEPTH sends of CHUNK_BYTES each from one prefilled buffer outstanding.
 *   - the datagram counts are msquic's own process-wide counters (QUIC_PARAM_GLOBAL_PERF_COUNTERS): UDP_SEND and
 *     UDP_RECV are `datagrams_sent` and `datagrams_received`; UDP_SEND_CALLS (one per send it hands its datapath, a
 *     segmented train or a datagram) is `send_messages`, and UDP_RECV_EVENTS (one per receive it delivers, which may
 *     join several of the host's messages) is `receive_messages`.
 *
 * msquic runs its own worker threads and calls back on them, one connection's events in turn; the main thread starts a
 * client's first connections and waits until `connections` have shut down, and shuts a holding client's connections
 * down once all have done their work. A run that does not end in time lists its live connections on standard error. */
#include "msquic_common.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RUN_SECONDS 300
#define IDLE_MS 30000U
#define CHUNK_BYTES 65536U
#define SEND_DEPTH 8U

static const uint8_t FILL[CHUNK_BYTES];

typedef struct work work_t;

typedef struct
{
    const QUIC_API_TABLE* api;
    HQUIC                 registration;
    HQUIC                 configuration;
    int                   client;
    unsigned long         port;
    uint64_t              connections;
    uint64_t              parallel;
    uint64_t              bytes;
    uint64_t              window;
    int                   hold;
    unsigned char         pin_key[32];
    pthread_mutex_t       lock;
    pthread_cond_t        changed;
    /* Under the lock. */
    uint64_t started;
    uint64_t finished; /* a client's connections done with their work: closing, or shut down before */
    uint64_t closed;
    uint64_t opened;    /* connections a client started or a server accepted */
    uint64_t peak_live; /* the most opened and not yet shut down at once */
    work_t*  live;      /* those connections, the latest first */
    int      released;  /* a holding client shut its connections down */
    uint64_t established;
    uint64_t received;
    uint64_t sent;
    int      failed;
    int      mixed;
    uint32_t suite;
    uint32_t version;
} board_t;

/* One connection's work, touched on its own worker thread alone (its place in the board's live list under the lock);
 * its counts join the board's when it shuts down. */
struct work
{
    board_t*    board;
    work_t*     previous;
    work_t*     next;
    HQUIC       connection;
    HQUIC       stream;
    uint64_t    queued; /* bytes handed to StreamSend */
    uint64_t    sent;   /* of those, the completed */
    uint64_t    received;
    uint32_t    outstanding;
    int         pumping;
    uint64_t    sends; /* StreamSend calls: the next one's descriptor is buffers[sends % SEND_DEPTH] */
    /* What msquic reads each send's bytes through until its completion, all over FILL; sends complete in order. */
    QUIC_BUFFER buffers[SEND_DEPTH];
    int         established;
    int         finished;
    int         failed;
    uint32_t    suite;
    uint32_t    version;
};

static QUIC_STATUS QUIC_API on_connection(HQUIC handle, void* context, QUIC_CONNECTION_EVENT* event);

/* Keeps SEND_DEPTH chunks of a client's bytes outstanding, the last with the FIN. msquic may complete a send inside
 * StreamSend on the connection's own thread: each send is counted before it is made, and a completion inside the loop
 * leaves the refill to it. */
static void pump(work_t* work)
{
    board_t* board = work->board;
    if (work->pumping)
    {
        return;
    }
    work->pumping = 1;
    while (work->outstanding < SEND_DEPTH && work->queued < board->bytes && !work->failed)
    {
        const uint64_t left   = board->bytes - work->queued;
        const uint32_t size   = left < CHUNK_BYTES ? (uint32_t)left : CHUNK_BYTES;
        QUIC_BUFFER*   buffer = &work->buffers[work->sends % SEND_DEPTH];
        const int      last   = work->queued + size == board->bytes;
        buffer->Length        = size;
        buffer->Buffer        = (uint8_t*)FILL;
        work->queued += size;
        ++work->sends;
        ++work->outstanding;
        if (QUIC_FAILED(board->api->StreamSend(work->stream, buffer, 1U,
                                               last ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE, buffer)))
        {
            work->failed = 1;
        }
    }
    work->pumping = 0;
}

/* The connection's work is done: its place among the `parallel` under way is free while it closes. */
static void finish(work_t* work)
{
    board_t* board = work->board;
    if (work->finished)
    {
        return;
    }
    work->finished = 1;
    pthread_mutex_lock(&board->lock);
    ++board->finished;
    pthread_cond_signal(&board->changed);
    pthread_mutex_unlock(&board->lock);
}

static QUIC_STATUS QUIC_API on_stream(HQUIC handle, void* context, QUIC_STREAM_EVENT* event)
{
    work_t*  work  = context;
    board_t* board = work->board;
    switch (event->Type)
    {
    case QUIC_STREAM_EVENT_RECEIVE:
        work->received += event->RECEIVE.TotalBufferLength;
        break;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (event->SEND_COMPLETE.ClientContext == NULL)
        {
            break; /* a FIN alone */
        }
        --work->outstanding;
        if (!event->SEND_COMPLETE.Canceled)
        {
            work->sent += ((const QUIC_BUFFER*)event->SEND_COMPLETE.ClientContext)->Length;
            pump(work);
        }
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        /* A server answers with its FIN alone; a client closes once it came, or a holding one once all came. */
        if (!board->client && QUIC_FAILED(board->api->StreamSend(handle, NULL, 0U, QUIC_SEND_FLAG_FIN, NULL)))
        {
            work->failed = 1;
        }
        if (board->client)
        {
            finish(work);
            if (!board->hold)
            {
                board->api->ConnectionShutdown(work->connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0U);
            }
        }
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        work->failed = 1;
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress)
        {
            board->api->StreamClose(handle);
        }
        work->stream = NULL;
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

static void connected(work_t* work, HQUIC handle)
{
    board_t*            board = work->board;
    QUIC_HANDSHAKE_INFO info;
    uint32_t            size = sizeof(info);
    work->established        = 1;
    memset(&info, 0, sizeof(info));
    (void)board->api->GetParam(handle, QUIC_PARAM_TLS_HANDSHAKE_INFO, &size, &info);
    work->suite = (uint32_t)info.CipherSuite;
    size        = sizeof(work->version);
    (void)board->api->GetParam(handle, QUIC_PARAM_CONN_QUIC_VERSION, &size, &work->version);
    if (!board->client)
    {
        return;
    }
    if (QUIC_FAILED(board->api->StreamOpen(handle, QUIC_STREAM_OPEN_FLAG_NONE, on_stream, work, &work->stream)) ||
        QUIC_FAILED(board->api->StreamStart(work->stream, QUIC_STREAM_START_FLAG_NONE)) ||
        (board->bytes == 0U &&
         QUIC_FAILED(board->api->StreamSend(work->stream, NULL, 0U, QUIC_SEND_FLAG_FIN, NULL))))
    {
        work->failed = 1;
        board->api->ConnectionShutdown(handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0U);
        return;
    }
    pump(work);
}

/* A connection leaves its board's live list; the caller holds the lock. */
static void unlink_work(work_t* work)
{
    board_t* board = work->board;
    if (work->previous != NULL)
    {
        work->previous->next = work->next;
    }
    else
    {
        board->live = work->next;
    }
    if (work->next != NULL)
    {
        work->next->previous = work->previous;
    }
}

/* The connection's counts join the board's, it leaves the live list, and the main thread hears of it (a client's starts
 * the next one). */
static void shut_down(work_t* work)
{
    board_t* board = work->board;
    finish(work);
    pthread_mutex_lock(&board->lock);
    unlink_work(work);
    ++board->closed;
    board->received += work->received;
    board->sent += work->sent;
    board->failed = board->failed || work->failed;
    if (work->established)
    {
        if (board->established != 0U && (work->suite != board->suite || work->version != board->version))
        {
            board->mixed = 1;
        }
        board->suite   = work->suite;
        board->version = work->version;
        ++board->established;
    }
    pthread_cond_signal(&board->changed);
    pthread_mutex_unlock(&board->lock);
    free(work);
}

static QUIC_STATUS QUIC_API on_connection(HQUIC handle, void* context, QUIC_CONNECTION_EVENT* event)
{
    work_t*  work  = context;
    board_t* board = work->board;
    switch (event->Type)
    {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        connected(work, handle);
        break;
    case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED:
        return msquic_pin_holds(board->pin_key, (const QUIC_BUFFER*)event->PEER_CERTIFICATE_RECEIVED.Certificate)
                   ? QUIC_STATUS_SUCCESS
                   : QUIC_STATUS_BAD_CERTIFICATE;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        work->stream = event->PEER_STREAM_STARTED.Stream;
        board->api->SetCallbackHandler(event->PEER_STREAM_STARTED.Stream, (void*)on_stream, work);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
    {
        /* Out of the live list before its handle closes: the main thread shuts down what the list holds. */
        const BOOLEAN closing = event->SHUTDOWN_COMPLETE.AppCloseInProgress;
        shut_down(work);
        if (!closing)
        {
            board->api->ConnectionClose(handle);
        }
        break;
    }
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

static work_t* new_work(board_t* board)
{
    work_t* work = calloc(1U, sizeof(work_t));
    if (work != NULL)
    {
        work->board = board;
    }
    return work;
}

/* A connection joins the live list, its board's under the lock, before msquic can call back for it. */
static void enlist(work_t* work)
{
    board_t* board = work->board;
    pthread_mutex_lock(&board->lock);
    work->next = board->live;
    if (board->live != NULL)
    {
        board->live->previous = work;
    }
    board->live = work;
    ++board->opened;
    if (board->opened - board->closed > board->peak_live)
    {
        board->peak_live = board->opened - board->closed;
    }
    pthread_mutex_unlock(&board->lock);
}

/* A connection msquic never started leaves the live list, uncounted. */
static void delist(work_t* work)
{
    board_t* board = work->board;
    pthread_mutex_lock(&board->lock);
    unlink_work(work);
    --board->opened;
    pthread_mutex_unlock(&board->lock);
}

static int start_connection(board_t* board)
{
    work_t*         work    = new_work(board);
    const QUIC_ADDR address = msquic_loopback(board->port);
    if (work == NULL ||
        QUIC_FAILED(board->api->ConnectionOpen(board->registration, on_connection, work, &work->connection)))
    {
        free(work);
        return 0;
    }
    const BOOLEAN share = TRUE;
    enlist(work);
    if (QUIC_FAILED(board->api->SetParam(work->connection, QUIC_PARAM_CONN_SHARE_UDP_BINDING, sizeof(share),
                                         &share)) ||
        QUIC_FAILED(board->api->SetParam(work->connection, QUIC_PARAM_CONN_REMOTE_ADDRESS, sizeof(address),
                                         &address)) ||
        QUIC_FAILED(board->api->ConnectionStart(work->connection, board->configuration, QUIC_ADDRESS_FAMILY_INET,
                                                MSQUIC_SERVER_NAME, (uint16_t)board->port)))
    {
        delist(work);
        board->api->ConnectionClose(work->connection);
        free(work);
        return 0;
    }
    return 1;
}

static QUIC_STATUS QUIC_API on_listener(HQUIC handle, void* context, QUIC_LISTENER_EVENT* event)
{
    (void)handle;
    board_t* board = context;
    if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION)
    {
        return QUIC_STATUS_SUCCESS;
    }
    const QUIC_NEW_CONNECTION_INFO* info = event->NEW_CONNECTION.Info;
    work_t*                         work = new_work(board);
    if (work == NULL || info->ServerName == NULL || info->ServerNameLength != strlen(MSQUIC_SERVER_NAME) ||
        memcmp(info->ServerName, MSQUIC_SERVER_NAME, info->ServerNameLength) != 0)
    {
        free(work);
        return QUIC_STATUS_CONNECTION_REFUSED;
    }
    work->connection = event->NEW_CONNECTION.Connection;
    board->api->SetCallbackHandler(event->NEW_CONNECTION.Connection, (void*)on_connection, work);
    enlist(work);
    const QUIC_STATUS status = board->api->ConnectionSetConfiguration(event->NEW_CONNECTION.Connection,
                                                                      board->configuration);
    if (QUIC_FAILED(status))
    {
        delist(work);
        free(work);
    }
    return status;
}

static int open_configuration(board_t* board, EVP_PKEY* key)
{
    const QUIC_BUFFER alpn = {(uint32_t)strlen(MSQUIC_ALPN), (uint8_t*)MSQUIC_ALPN};
    QUIC_SETTINGS     settings;
    memset(&settings, 0, sizeof(settings));
    settings.IdleTimeoutMs                   = IDLE_MS;
    settings.IsSet.IdleTimeoutMs             = 1;
    settings.PeerBidiStreamCount             = board->client ? 0U : 1U;
    settings.IsSet.PeerBidiStreamCount       = 1;
    settings.StreamRecvWindowDefault         = (uint32_t)board->window;
    settings.IsSet.StreamRecvWindowDefault   = 1;
    settings.ConnFlowControlWindow           = (uint32_t)(board->window * 16U);
    settings.IsSet.ConnFlowControlWindow     = 1;
    if (QUIC_FAILED(board->api->ConfigurationOpen(board->registration, &alpn, 1U, &settings, sizeof(settings), NULL,
                                                  &board->configuration)))
    {
        return 0;
    }
    QUIC_CREDENTIAL_CONFIG config;
    memset(&config, 0, sizeof(config));
    config.Flags               = QUIC_CREDENTIAL_FLAG_SET_ALLOWED_CIPHER_SUITES;
    config.AllowedCipherSuites = QUIC_ALLOWED_CIPHER_SUITE_AES_128_GCM_SHA256;
    return msquic_offer_version(board->api, board->configuration, 1U) &&
           msquic_load_credential(board->api, board->configuration, board->client, key, &config);
}

/* A server listens and prints its port; a client's connections start in wait_done. */
static int start(board_t* board, HQUIC* listener)
{
    if (board->client)
    {
        return 1;
    }
    QUIC_ADDR         address = msquic_loopback(board->port);
    const QUIC_BUFFER alpn    = {(uint32_t)strlen(MSQUIC_ALPN), (uint8_t*)MSQUIC_ALPN};
    uint32_t          size    = sizeof(address);
    if (QUIC_FAILED(board->api->ListenerOpen(board->registration, on_listener, board, listener)) ||
        QUIC_FAILED(board->api->ListenerStart(*listener, &alpn, 1U, &address)) ||
        QUIC_FAILED(board->api->GetParam(*listener, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &size, &address)))
    {
        return 0;
    }
    printf("port=%u\n", (unsigned)QuicAddrGetPort(&address));
    fflush(stdout);
    return 1;
}

/* Whether `connections` shut down within RUN_SECONDS. A client's are started here, on the main thread, `parallel` under
 * way at once (closing ones not counted): none from msquic's callbacks, from which starts left connections hanging. A
 * holding client's are shut down here too, once every one has done its work: ConnectionShutdown only queues the
 * shutdown on the connection's worker, which takes the connection out of the live list before closing its handle. */
static int wait_done(board_t* board)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += RUN_SECONDS;
    pthread_mutex_lock(&board->lock);
    int waited = 0;
    while (waited == 0 && !board->failed)
    {
        while (board->client && board->started < board->connections &&
               board->started - board->finished < board->parallel && !board->failed)
        {
            ++board->started;
            pthread_mutex_unlock(&board->lock);
            const int started = start_connection(board);
            pthread_mutex_lock(&board->lock);
            board->failed = board->failed || !started;
        }
        if (board->hold && board->client && !board->released && board->finished >= board->connections)
        {
            board->released = 1;
            for (const work_t* work = board->live; work != NULL; work = work->next)
            {
                board->api->ConnectionShutdown(work->connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0U);
            }
        }
        if (board->closed >= board->connections)
        {
            break;
        }
        waited = pthread_cond_timedwait(&board->changed, &board->lock, &until);
    }
    const int done = board->closed >= board->connections && !board->failed;
    pthread_mutex_unlock(&board->lock);
    return done;
}

static void report(board_t* board)
{
    pthread_mutex_lock(&board->lock);
    printf("connections=%llu\nestablished=%llu\nbytes_received=%llu\nbytes_sent=%llu\n",
           (unsigned long long)board->closed, (unsigned long long)board->established,
           (unsigned long long)board->received, (unsigned long long)board->sent);
    const char* cipher = board->suite == QUIC_CIPHER_SUITE_TLS_AES_128_GCM_SHA256 ? "aes128" : "other";
    printf("group=unreported\ncipher=%s\nversion=%s\npeak_live=%llu\n", board->mixed ? "mixed" : cipher,
           board->mixed ? "mixed" : (board->version == 1U ? "1" : "other"), (unsigned long long)board->peak_live);
    pthread_mutex_unlock(&board->lock);
    uint64_t counters[QUIC_PERF_COUNTER_MAX];
    uint32_t size = sizeof(counters);
    if (QUIC_SUCCEEDED(board->api->GetParam(NULL, QUIC_PARAM_GLOBAL_PERF_COUNTERS, &size, counters)))
    {
        printf("datagrams_sent=%llu\nsend_messages=%llu\ndatagrams_received=%llu\nreceive_messages=%llu\n",
               (unsigned long long)counters[QUIC_PERF_COUNTER_UDP_SEND],
               (unsigned long long)counters[QUIC_PERF_COUNTER_UDP_SEND_CALLS],
               (unsigned long long)counters[QUIC_PERF_COUNTER_UDP_RECV],
               (unsigned long long)counters[QUIC_PERF_COUNTER_UDP_RECV_EVENTS]);
    }
    fflush(stdout);
}

/* A run that did not end: what each connection still live had done, on standard error for the driver's message. */
static void list_live(board_t* board)
{
    pthread_mutex_lock(&board->lock);
    for (const work_t* work = board->live; work != NULL; work = work->next)
    {
        fprintf(stderr, "live: established=%d finished=%d failed=%d stream=%d received=%llu sent=%llu queued=%llu\n",
                work->established, work->finished, work->failed, work->stream != NULL ? 1 : 0,
                (unsigned long long)work->received, (unsigned long long)work->sent,
                (unsigned long long)work->queued);
    }
    pthread_mutex_unlock(&board->lock);
}

static int usage(void)
{
    fputs("usage: crd-msquic-quic-board --role client|server --port N --seed HEX --connections N [--parallel N] "
          "[--bytes N] [--window N] [--hold 0|1]\n",
          stderr);
    return 2;
}

int main(int argc, char** argv)
{
    static board_t board;
    const char*    role = NULL;
    const char*    seed = NULL;
    board.parallel      = 1U;
    board.window        = 1U << 20U;
    for (int at = 1; at + 1 < argc; at += 2)
    {
        const char*              name   = argv[at];
        const char*              value  = argv[at + 1];
        const unsigned long long number = strtoull(value, NULL, 10);
        if (strcmp(name, "--role") == 0)
        {
            role = value;
        }
        else if (strcmp(name, "--seed") == 0)
        {
            seed = value;
        }
        else if (strcmp(name, "--port") == 0)
        {
            board.port = (unsigned long)number;
        }
        else if (strcmp(name, "--connections") == 0)
        {
            board.connections = number;
        }
        else if (strcmp(name, "--parallel") == 0)
        {
            board.parallel = number;
        }
        else if (strcmp(name, "--bytes") == 0)
        {
            board.bytes = number;
        }
        else if (strcmp(name, "--window") == 0)
        {
            board.window = number;
        }
        else if (strcmp(name, "--hold") == 0)
        {
            board.hold = number != 0U;
        }
        else
        {
            return usage();
        }
    }
    unsigned char private_key[32];
    board.client = role != NULL && strcmp(role, "client") == 0;
    if (role == NULL || (!board.client && strcmp(role, "server") != 0) || (argc % 2) == 0 || board.port > 65535UL ||
        (board.client && board.port == 0UL) || board.connections == 0U || board.parallel == 0U ||
        board.window < 4096U || board.window > (1U << 26U) || seed == NULL || !parse_seed(seed, private_key))
    {
        return usage();
    }
    pthread_mutex_init(&board.lock, NULL);
    pthread_cond_init(&board.changed, NULL);
    EVP_PKEY* key  = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key, sizeof(private_key));
    size_t    size = sizeof(board.pin_key);
    if (key == NULL || EVP_PKEY_get_raw_public_key(key, board.pin_key, &size) != 1)
    {
        fputs("crd-msquic-quic-board: no Ed25519 key from the seed\n", stderr);
        return 1;
    }
    const QUIC_REGISTRATION_CONFIG registration = {"crd-msquic-quic-board", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    HQUIC                          listener     = NULL;
    if (QUIC_FAILED(MsQuicOpen2(&board.api)) ||
        QUIC_FAILED(board.api->RegistrationOpen(&registration, &board.registration)) ||
        !open_configuration(&board, key) || !start(&board, &listener))
    {
        fputs("crd-msquic-quic-board: msquic setup failed\n", stderr);
        return 1;
    }
    EVP_PKEY_free(key);
    const int done = wait_done(&board);
    report(&board);
    if (!done)
    {
        list_live(&board);
        fputs("crd-msquic-quic-board: the run failed or did not end in time\n", stderr);
        _Exit(1); /* msquic's threads may still hold connections: no orderly teardown */
    }
    if (listener != NULL)
    {
        board.api->ListenerClose(listener);
    }
    board.api->ConfigurationClose(board.configuration);
    board.api->RegistrationClose(board.registration);
    MsQuicClose(board.api);
    return 0;
}
