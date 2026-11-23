/* crd-net QUIC interoperability test (NET-PATH) -- a QUIC endpoint over the pinned msquic and the peers' OpenSSL 4.0,
 * real UDP on the loopback, the msquic counterpart of cerid_quic_peer.cpp. msquic_interop.py runs one of each and reads
 * both reports.
 *
 * Usage: crd-msquic-quic-peer --role client|server --port N --seed HEX --version 1|2 --streams N --bytes N --code N
 *
 *   port     a server's to bind on 127.0.0.1 (0: any, printed as `port=N`), a client's to reach
 *   seed     the 32-byte Ed25519 private key of the server's self-signed credential; a client pins its public key, and
 *            OpenSSL checks the handshake's signature with it
 *   version  the one QUIC version each end offers and accepts (1, or 2 for RFC 9369)
 *   streams  bidirectional streams a client opens, each carrying `bytes` bytes and a FIN; a server echoes as many bytes
 *            as it reads and its FIN after the client's. Both ends check every byte (http_exchange.h's pattern, tag 0
 *            from the client and 1 back)
 *   code     the application error the client closes with once every echo has ended
 *
 * msquic runs its own threads and calls back on them, one connection's events in turn; the main thread waits until
 * the one connection has shut down or RUN_SECONDS have passed. The report, one `name=value` per line from
 * `connection=1` on standard output: whether it was `established`, its `version`, the FINs read (`streams`),
 * `bytes_received`, `bytes_sent` (those msquic's send completions confirm), whether every byte held its pattern
 * (`verified`), the close (`close`: local, application, transport or idle; `close_code`), whether a client's pin held
 * (`pin`) and the application protocol (`alpn`, the private `cerid-interop`). */
#include "http_exchange.h"
#include "msquic_common.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_STREAMS 64U
#define RUN_SECONDS 60
#define QUIC_V2 0x6b3343cfU
#define IDLE_MS 30000U

typedef struct peer peer_t;

typedef struct
{
    peer_t*  peer;
    uint64_t id;
    uint64_t received;
    uint64_t sent; /* bytes handed to msquic: the next byte's offset in this end's pattern */
} stream_t;

struct peer
{
    const QUIC_API_TABLE* api;
    HQUIC                 configuration;
    HQUIC                 connection; /* a client's */
    int                   client;
    uint32_t              version;
    uint64_t              streams;
    uint64_t              bytes;
    uint64_t              code;
    unsigned char         pin_key[32];
    pthread_mutex_t       lock;
    pthread_cond_t        changed;
    int                   done;
    int                   accepted; /* a server's one connection taken */
    /* The report. */
    int      established;
    uint32_t negotiated;
    uint64_t fins;
    uint64_t bytes_received;
    uint64_t bytes_sent;
    int      verified;
    int      pinned;
    char     close[16];
    uint64_t close_code;
    char     alpn[32];
};

static void finish(peer_t* peer)
{
    pthread_mutex_lock(&peer->lock);
    peer->done = 1;
    pthread_cond_signal(&peer->changed);
    pthread_mutex_unlock(&peer->lock);
}

static void set_close(peer_t* peer, const char* kind, uint64_t code)
{
    if (peer->close[0] == '\0')
    {
        snprintf(peer->close, sizeof(peer->close), "%s", kind);
        peer->close_code = code;
    }
}

/* Hands msquic `size` bytes of this end's pattern from the stream's next offset, with a FIN or not; the buffer goes
 * back on its send completion. */
static int send_pattern(HQUIC handle, stream_t* stream, uint64_t size, int fin)
{
    peer_t*      peer   = stream->peer;
    QUIC_BUFFER* buffer = malloc(sizeof(QUIC_BUFFER) + size);
    if (buffer == NULL)
    {
        return 0;
    }
    buffer->Buffer = (uint8_t*)(buffer + 1);
    buffer->Length = (uint32_t)size;
    for (uint64_t at = 0; at < size; ++at)
    {
        buffer->Buffer[at] = httpx_pattern(peer->client ? 0U : 1U, stream->id, stream->sent + at);
    }
    stream->sent += size;
    if (QUIC_FAILED(peer->api->StreamSend(handle, buffer, 1U, fin ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE,
                                          buffer)))
    {
        free(buffer);
        return 0;
    }
    return 1;
}

/* Every byte read holds the other end's pattern; a server owes as many of its own back. */
static int receive(HQUIC handle, stream_t* stream, const QUIC_STREAM_EVENT* event)
{
    peer_t*        peer = stream->peer;
    const uint64_t tag  = peer->client ? 1U : 0U;
    uint64_t       read = 0U;
    for (uint32_t index = 0; index < event->RECEIVE.BufferCount; ++index)
    {
        const QUIC_BUFFER* buffer = &event->RECEIVE.Buffers[index];
        for (uint32_t at = 0; at < buffer->Length; ++at)
        {
            if (buffer->Buffer[at] != httpx_pattern(tag, stream->id, stream->received + read + at))
            {
                peer->verified = 0;
            }
        }
        read += buffer->Length;
    }
    stream->received += read;
    peer->bytes_received += read;
    return peer->client || read == 0U || send_pattern(handle, stream, read, 0);
}

static QUIC_STATUS QUIC_API on_stream(HQUIC handle, void* context, QUIC_STREAM_EVENT* event)
{
    stream_t* stream = context;
    peer_t*   peer   = stream->peer;
    switch (event->Type)
    {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        stream->id = event->START_COMPLETE.ID;
        if (QUIC_FAILED(event->START_COMPLETE.Status) || !send_pattern(handle, stream, peer->bytes, 1))
        {
            peer->verified = 0;
        }
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        if (!receive(handle, stream, event))
        {
            peer->verified = 0;
        }
        break;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
    {
        QUIC_BUFFER* buffer = event->SEND_COMPLETE.ClientContext;
        if (buffer != NULL && !event->SEND_COMPLETE.Canceled)
        {
            peer->bytes_sent += buffer->Length;
        }
        free(buffer);
        break;
    }
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        ++peer->fins;
        if (!peer->client && QUIC_FAILED(peer->api->StreamSend(handle, NULL, 0U, QUIC_SEND_FLAG_FIN, NULL)))
        {
            peer->verified = 0;
        }
        /* A client closes once every echo has ended. */
        if (peer->client && peer->fins == peer->streams && peer->close[0] == '\0')
        {
            set_close(peer, "local", peer->code);
            peer->api->ConnectionShutdown(peer->connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, peer->code);
        }
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        peer->verified = 0;
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress)
        {
            peer->api->StreamClose(handle);
        }
        free(stream);
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

static stream_t* new_stream(peer_t* peer)
{
    stream_t* stream = calloc(1U, sizeof(stream_t));
    if (stream != NULL)
    {
        stream->peer = peer;
    }
    return stream;
}

static QUIC_STATUS QUIC_API on_connection(HQUIC handle, void* context, QUIC_CONNECTION_EVENT* event)
{
    peer_t* peer = context;
    switch (event->Type)
    {
    case QUIC_CONNECTION_EVENT_CONNECTED:
    {
        uint32_t size        = sizeof(peer->negotiated);
        peer->established    = 1;
        const size_t length  = event->CONNECTED.NegotiatedAlpnLength;
        const size_t kept    = length < sizeof(peer->alpn) - 1U ? length : sizeof(peer->alpn) - 1U;
        memcpy(peer->alpn, event->CONNECTED.NegotiatedAlpn, kept);
        peer->alpn[kept] = '\0';
        if (QUIC_FAILED(peer->api->GetParam(handle, QUIC_PARAM_CONN_QUIC_VERSION, &size, &peer->negotiated)))
        {
            peer->negotiated = 0U;
        }
        for (uint64_t index = 0; peer->client && index < peer->streams; ++index)
        {
            stream_t* stream = new_stream(peer);
            HQUIC     opened = NULL;
            if (stream == NULL ||
                QUIC_FAILED(peer->api->StreamOpen(handle, QUIC_STREAM_OPEN_FLAG_NONE, on_stream, stream, &opened)) ||
                QUIC_FAILED(peer->api->StreamStart(opened, QUIC_STREAM_START_FLAG_NONE)))
            {
                peer->verified = 0;
                free(stream);
            }
        }
        break;
    }
    case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED:
        peer->pinned =
            msquic_pin_holds(peer->pin_key, (const QUIC_BUFFER*)event->PEER_CERTIFICATE_RECEIVED.Certificate);
        return peer->pinned ? QUIC_STATUS_SUCCESS : QUIC_STATUS_BAD_CERTIFICATE;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
    {
        stream_t* stream = new_stream(peer);
        if (stream == NULL)
        {
            return QUIC_STATUS_OUT_OF_MEMORY;
        }
        uint32_t size = sizeof(stream->id);
        (void)peer->api->GetParam(event->PEER_STREAM_STARTED.Stream, QUIC_PARAM_STREAM_ID, &size, &stream->id);
        peer->api->SetCallbackHandler(event->PEER_STREAM_STARTED.Stream, (void*)on_stream, stream);
        break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        set_close(peer, event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE ? "idle"
                                                                                                       : "transport",
                  event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        set_close(peer, "application", event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress)
        {
            peer->api->ConnectionClose(handle);
        }
        finish(peer);
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

static QUIC_STATUS QUIC_API on_listener(HQUIC handle, void* context, QUIC_LISTENER_EVENT* event)
{
    (void)handle;
    peer_t* peer = context;
    if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION)
    {
        return QUIC_STATUS_SUCCESS;
    }
    const QUIC_NEW_CONNECTION_INFO* info = event->NEW_CONNECTION.Info;
    if (peer->accepted || info->ServerName == NULL || info->ServerNameLength != strlen(MSQUIC_SERVER_NAME) ||
        memcmp(info->ServerName, MSQUIC_SERVER_NAME, info->ServerNameLength) != 0)
    {
        return QUIC_STATUS_CONNECTION_REFUSED;
    }
    peer->accepted = 1;
    peer->api->SetCallbackHandler(event->NEW_CONNECTION.Connection, (void*)on_connection, peer);
    return peer->api->ConnectionSetConfiguration(event->NEW_CONNECTION.Connection, peer->configuration);
}

static int open_configuration(peer_t* peer, HQUIC registration, EVP_PKEY* key)
{
    const QUIC_BUFFER alpn = {(uint32_t)strlen(MSQUIC_ALPN), (uint8_t*)MSQUIC_ALPN};
    QUIC_SETTINGS     settings;
    memset(&settings, 0, sizeof(settings));
    settings.IdleTimeoutMs                = IDLE_MS;
    settings.IsSet.IdleTimeoutMs          = 1;
    settings.PeerBidiStreamCount          = peer->client ? 0U : MAX_STREAMS;
    settings.IsSet.PeerBidiStreamCount    = 1;
    if (QUIC_FAILED(peer->api->ConfigurationOpen(registration, &alpn, 1U, &settings, sizeof(settings), NULL,
                                                 &peer->configuration)))
    {
        return 0;
    }
    QUIC_CREDENTIAL_CONFIG config;
    memset(&config, 0, sizeof(config));
    return msquic_offer_version(peer->api, peer->configuration, peer->version) &&
           msquic_load_credential(peer->api, peer->configuration, peer->client, key, &config);
}

/* A server listens on 127.0.0.1:`port` and prints the port it bound; a client connects there, naming
 * MSQUIC_SERVER_NAME. */
static int start(peer_t* peer, HQUIC registration, unsigned long port, HQUIC* listener, HQUIC* connection)
{
    QUIC_ADDR         address = msquic_loopback(port);
    const QUIC_BUFFER alpn    = {(uint32_t)strlen(MSQUIC_ALPN), (uint8_t*)MSQUIC_ALPN};
    if (!peer->client)
    {
        uint32_t size = sizeof(address);
        if (QUIC_FAILED(peer->api->ListenerOpen(registration, on_listener, peer, listener)) ||
            QUIC_FAILED(peer->api->ListenerStart(*listener, &alpn, 1U, &address)) ||
            QUIC_FAILED(peer->api->GetParam(*listener, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &size, &address)))
        {
            return 0;
        }
        printf("port=%u\n", (unsigned)QuicAddrGetPort(&address));
        fflush(stdout);
        return 1;
    }
    if (QUIC_FAILED(peer->api->ConnectionOpen(registration, on_connection, peer, connection)))
    {
        return 0;
    }
    peer->connection = *connection;
    return QUIC_SUCCEEDED(peer->api->SetParam(*connection, QUIC_PARAM_CONN_REMOTE_ADDRESS, sizeof(address),
                                              &address)) &&
           QUIC_SUCCEEDED(peer->api->ConnectionStart(*connection, peer->configuration, QUIC_ADDRESS_FAMILY_INET,
                                                     MSQUIC_SERVER_NAME, (uint16_t)port));
}

/* Whether the connection shut down within RUN_SECONDS. */
static int wait_done(peer_t* peer)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += RUN_SECONDS;
    pthread_mutex_lock(&peer->lock);
    int waited = 0;
    while (!peer->done && waited == 0)
    {
        waited = pthread_cond_timedwait(&peer->changed, &peer->lock, &until);
    }
    const int done = peer->done;
    pthread_mutex_unlock(&peer->lock);
    return done;
}

static void report(const peer_t* peer)
{
    const unsigned version = peer->negotiated == 1U ? 1U : (peer->negotiated == QUIC_V2 ? 2U : 0U);
    printf("connection=1\nestablished=%d\nversion=%u\nstreams=%llu\nbytes_received=%llu\nbytes_sent=%llu\n",
           peer->established, version, (unsigned long long)peer->fins, (unsigned long long)peer->bytes_received,
           (unsigned long long)peer->bytes_sent);
    printf("verified=%d\nclose=%s\nclose_code=%llu\npin=%d\nalpn=%s\n", peer->verified,
           peer->close[0] != '\0' ? peer->close : "none", (unsigned long long)peer->close_code, peer->pinned,
           peer->alpn);
    fflush(stdout);
}

static int usage(void)
{
    fputs("usage: crd-msquic-quic-peer --role client|server --port N --seed HEX --version 1|2 --streams N --bytes N "
          "--code N\n",
          stderr);
    return 2;
}

int main(int argc, char** argv)
{
    static peer_t peer;
    const char*   role    = NULL;
    const char*   seed    = NULL;
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
        else
        {
            return usage();
        }
    }
    unsigned char private_key[32];
    peer.client = role != NULL && strcmp(role, "client") == 0;
    if (role == NULL || (!peer.client && strcmp(role, "server") != 0) || (argc % 2) == 0 || port > 65535UL ||
        (version != 1UL && version != 2UL) || peer.streams > MAX_STREAMS || peer.bytes > UINT32_MAX ||
        !parse_seed(seed, private_key))
    {
        return usage();
    }
    peer.version  = version == 2UL ? QUIC_V2 : 1U;
    peer.verified = 1;
    pthread_mutex_init(&peer.lock, NULL);
    pthread_cond_init(&peer.changed, NULL);
    EVP_PKEY* key  = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key, sizeof(private_key));
    size_t    size = sizeof(peer.pin_key);
    if (key == NULL || EVP_PKEY_get_raw_public_key(key, peer.pin_key, &size) != 1)
    {
        fputs("crd-msquic-quic-peer: no Ed25519 key from the seed\n", stderr);
        return 1;
    }
    const QUIC_REGISTRATION_CONFIG registration_config = {"crd-msquic-quic-peer", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    HQUIC registration = NULL;
    HQUIC listener     = NULL;
    HQUIC connection   = NULL;
    if (QUIC_FAILED(MsQuicOpen2(&peer.api)) ||
        QUIC_FAILED(peer.api->RegistrationOpen(&registration_config, &registration)) ||
        !open_configuration(&peer, registration, key) || !start(&peer, registration, port, &listener, &connection))
    {
        fputs("crd-msquic-quic-peer: msquic setup failed\n", stderr);
        return 1;
    }
    EVP_PKEY_free(key);
    const int done = wait_done(&peer);
    report(&peer);
    if (!done)
    {
        fputs("crd-msquic-quic-peer: the connection did not end in time\n", stderr);
        _Exit(1); /* msquic's threads still hold the connection: no orderly teardown */
    }
    if (listener != NULL)
    {
        peer.api->ListenerClose(listener);
    }
    peer.api->ConfigurationClose(peer.configuration);
    peer.api->RegistrationClose(registration);
    MsQuicClose(peer.api);
    return 0;
}
