/* crd-openssl-tls-peer -- one end of a TLS connection through the pinned OpenSSL 3.5 (cmake/pins.json openssl), the
 * independent peer of crd-crypto's TLS 1.3 sessions, driven over its standard streams by
 * tests/foundation/crypto/interop/tls_interop.py (NET.2; docs/design/network-foundation.md#tls-sessions). It links only
 * OpenSSL, never AWS-LC: the two libraries share symbol names and cannot share a process.
 *
 * Usage: crd-openssl-tls-peer --role client|server --groups LIST --seed HEX [--pin HEX] [--name NAME]
 *                             [--max-version 1.2] [--update requested|not-requested] [--session FILE]
 *                             [--ticket-key HEX] [--compression zstd|zlib|none] [--compressed-cert FILE]
 *                             [--dump-compressed FILE]
 *   groups       OpenSSL's group list syntax (SSL_CTX_set1_groups_list; a '*' marks a group that gets a key share)
 *   seed         the 32-byte Ed25519 private key of this end's self-signed certificate
 *   pin          the peer's 32-byte Ed25519 public key: the peer's certificate must carry it (a server then asks for
 *                one)
 *   max-version  1.2 offers TLS 1.2 only (the refusal case)
 *   update       an established client updates its key once after its ping (SSL_key_update), asking the server for
 *                an update or not; either end sends a KeyUpdate it owes at once (a zero-length SSL_write)
 *   session      a client's session store: the session in FILE (PEM) is offered once, and the newest one its server's
 *                tickets make replaces it
 *   ticket-key   a server's 32-byte ticket key, from which its 80 bytes of OpenSSL ticket key material are made (the
 *                key three times over), so two server processes open each other's tickets
 *   compression  the one RFC 8879 algorithm a client decodes or a server compresses its certificate with (a server
 *                compresses it at start-up, and keeps the form only when it is smaller); none turns it off
 *   compressed-cert  a server's compressed certificate in FILE (a 4-byte big-endian uncompressed length, then the
 *                compressed bytes), installed as it is under --compression's algorithm, whatever it decodes to
 *   dump-compressed  a server writes the form it compressed to FILE in the same layout
 *
 * A server prefers its own group order (SSL_OP_CIPHER_SERVER_PREFERENCE), as a Cerid server does. The framing and the
 * script are crd-tls-peer's: 4-byte big-endian frame lengths, a client's first flight unasked,
 * one frame per frame read, "ping from openssl" from an established client, an echo from an established server and one
 * JSON line on standard error at the end of input, the KeyUpdate messages sent and received, the algorithm of a
 * CompressedCertificate sent and received (handshake type 25, RFC 8879) and whether the handshake resumed among it. */

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_BYTES 65536

static const char k_ping[] = "ping from openssl";

static unsigned char g_frame[FRAME_BYTES];
static unsigned char g_pin[32];
static int           g_has_pin;
static int           g_peer_ok;
static int           g_hello_retry;
static int           g_alert_sent     = -1;
static int           g_alert_received = -1;
static int           g_updates_sent;
static int           g_updates_received;
static int           g_compressed_sent;
static int           g_compressed_received;
static const char*   g_session_path;

/* SHA-256("HelloRetryRequest"), the random of a HelloRetryRequest (RFC 9846 section 4.1.3). */
static const unsigned char k_hello_retry[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
};

static int parse_hex(const char* text, unsigned char* out, size_t size)
{
    if (strlen(text) != size * 2)
    {
        return 0;
    }
    for (size_t i = 0; i < size * 2; ++i)
    {
        const char c = text[i];
        int        v = -1;
        if (c >= '0' && c <= '9')
        {
            v = c - '0';
        }
        else if (c >= 'a' && c <= 'f')
        {
            v = c - 'a' + 10;
        }
        if (v < 0)
        {
            return 0;
        }
        out[i / 2] = (unsigned char)(i % 2 == 0 ? v << 4 : out[i / 2] | v);
    }
    return 1;
}

static int read_frame(size_t* size)
{
    unsigned char header[4];
    if (fread(header, 1, 4, stdin) != 4)
    {
        return 0;
    }
    *size = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) | ((size_t)header[2] << 8) | header[3];
    return *size <= sizeof(g_frame) && fread(g_frame, 1, *size, stdin) == *size;
}

static int write_frame(BIO* outgoing)
{
    const int           pending = (int)BIO_pending(outgoing);
    const int           taken   = pending > 0 ? BIO_read(outgoing, g_frame, pending) : 0;
    const size_t        size    = taken > 0 ? (size_t)taken : 0;
    const unsigned char header[4] = {(unsigned char)(size >> 24), (unsigned char)(size >> 16),
                                     (unsigned char)(size >> 8), (unsigned char)size};
    return fwrite(header, 1, 4, stdout) == 4 && fwrite(g_frame, 1, size, stdout) == size && fflush(stdout) == 0;
}

/* The chain is self-signed, so the library's own verdict is set aside: the leaf must carry the pinned key. */
static int verify_pin(int preverified, X509_STORE_CTX* store)
{
    (void)preverified;
    if (X509_STORE_CTX_get_error_depth(store) != 0)
    {
        return 1;
    }
    EVP_PKEY*     key    = X509_get0_pubkey(X509_STORE_CTX_get_current_cert(store));
    unsigned char raw[32];
    size_t        length = sizeof(raw);
    g_peer_ok            = key != NULL && EVP_PKEY_get_raw_public_key(key, raw, &length) == 1 &&
                length == sizeof(raw) && memcmp(raw, g_pin, sizeof(raw)) == 0;
    return g_peer_ok;
}

static void observe(int write_p, int version, int content_type, const void* buffer, size_t length, SSL* ssl,
                    void* arg)
{
    const unsigned char* bytes = (const unsigned char*)buffer;
    (void)version;
    (void)ssl;
    (void)arg;
    if (content_type == 21 && length >= 2)
    {
        int* alert = write_p ? &g_alert_sent : &g_alert_received;
        if (*alert < 0)
        {
            *alert = bytes[1];
        }
    }
    else if (content_type == 22 && length == 5 && bytes[0] == 24)
    {
        *(write_p ? &g_updates_sent : &g_updates_received) += 1;
    }
    else if (content_type == 22 && length >= 6 && bytes[0] == 25)
    {
        *(write_p ? &g_compressed_sent : &g_compressed_received) = (bytes[4] << 8) | bytes[5];
    }
    else if (content_type == 22 && length >= 38 && bytes[0] == 2 &&
             memcmp(bytes + 6, k_hello_retry, sizeof(k_hello_retry)) == 0)
    {
        g_hello_retry = 1;
    }
}

/* Every session a server's ticket makes replaces the one in the session file. */
static int keep_session(SSL* ssl, SSL_SESSION* session)
{
    FILE* file = fopen(g_session_path, "w");
    (void)ssl;
    if (file != NULL)
    {
        PEM_write_SSL_SESSION(file, session);
        fclose(file);
    }
    return 0;
}

/* The session in the session file, removed as it is taken (a session is offered once); NULL when there is none. */
static SSL_SESSION* take_session(void)
{
    FILE*        file    = fopen(g_session_path, "r");
    SSL_SESSION* session = file != NULL ? PEM_read_SSL_SESSION(file, NULL, NULL, NULL) : NULL;
    if (file != NULL)
    {
        fclose(file);
        remove(g_session_path);
    }
    return session;
}

/* The name is the subject's common name when it fits (64 characters, RFC 5280's ub-common-name) and the
 * subjectAltName's always, critical when the subject is empty, as on a Cerid credential. */
static X509* self_signed(EVP_PKEY* key, const char* name)
{
    char            alt[300];
    const int       common      = strlen(name) <= 64;
    X509*           certificate = X509_new();
    X509_EXTENSION* extension   = NULL;
    if (snprintf(alt, sizeof(alt), "%sDNS:%s", common ? "" : "critical,", name) >= (int)sizeof(alt))
    {
        X509_free(certificate);
        return NULL;
    }
    extension = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name, alt);
    if (certificate == NULL || extension == NULL || !X509_set_version(certificate, 2) ||
        !ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) ||
        X509_gmtime_adj(X509_getm_notBefore(certificate), -3600) == NULL ||
        X509_gmtime_adj(X509_getm_notAfter(certificate), 86400) == NULL || !X509_set_pubkey(certificate, key) ||
        (common && !X509_NAME_add_entry_by_txt(X509_get_subject_name(certificate), "CN", MBSTRING_ASC,
                                               (const unsigned char*)name, -1, -1, 0)) ||
        !X509_set_issuer_name(certificate, X509_get_subject_name(certificate)) ||
        !X509_add_ext(certificate, extension, -1) || X509_sign(certificate, key, NULL) <= 0)
    {
        X509_EXTENSION_free(extension);
        X509_free(certificate);
        return NULL;
    }
    X509_EXTENSION_free(extension);
    return certificate;
}

static int usage(void)
{
    fputs("usage: crd-openssl-tls-peer --role client|server --groups LIST --seed HEX [--pin HEX] [--name NAME] "
          "[--max-version 1.2] [--update requested|not-requested] [--session FILE] [--ticket-key HEX] "
          "[--compression zstd|zlib|none] [--compressed-cert FILE] [--dump-compressed FILE]\n",
          stderr);
    return 2;
}

/* The algorithm --compression names: TLSEXT_comp_cert_none for "none", -1 for anything else. */
static int compression_of(const char* name)
{
    if (strcmp(name, "zstd") == 0)
    {
        return TLSEXT_comp_cert_zstd;
    }
    if (strcmp(name, "zlib") == 0)
    {
        return TLSEXT_comp_cert_zlib;
    }
    return strcmp(name, "none") == 0 ? TLSEXT_comp_cert_none : -1;
}

/* Installs the compressed certificate in `path` (a 4-byte big-endian uncompressed length, then the bytes). */
static int install_compressed(SSL_CTX* context, int algorithm, const char* path)
{
    FILE*         file   = fopen(path, "rb");
    unsigned char header[4];
    size_t        size   = 0;
    int           result = 0;
    if (file != NULL && fread(header, 1, 4, file) == 4)
    {
        size = fread(g_frame, 1, sizeof(g_frame), file);
        result = size > 0 && size < sizeof(g_frame) &&
                 SSL_CTX_set1_compressed_cert(context, algorithm, g_frame, size,
                                              ((size_t)header[0] << 24) | ((size_t)header[1] << 16) |
                                                  ((size_t)header[2] << 8) | header[3]) == 1;
    }
    if (file != NULL)
    {
        fclose(file);
    }
    return result;
}

/* Writes the form the server compressed its certificate to under `algorithm` to `path`, in install_compressed's
 * layout. */
static int dump_compressed(SSL_CTX* context, int algorithm, const char* path)
{
    unsigned char* data   = NULL;
    size_t         length = 0;
    const size_t   size   = SSL_CTX_get1_compressed_cert(context, algorithm, &data, &length);
    FILE*          file   = size > 0 ? fopen(path, "wb") : NULL;
    int            result = 0;
    if (file != NULL)
    {
        const unsigned char header[4] = {(unsigned char)(length >> 24), (unsigned char)(length >> 16),
                                         (unsigned char)(length >> 8), (unsigned char)length};
        result = fwrite(header, 1, 4, file) == 4 && fwrite(data, 1, size, file) == size;
        fclose(file);
    }
    OPENSSL_free(data);
    return result;
}

int main(int argc, char** argv)
{
    const char* role        = NULL;
    const char* groups      = NULL;
    const char* seed        = NULL;
    const char* pin         = NULL;
    const char* name        = "game.cerid.test";
    const char* max_version = NULL;
    const char* update      = NULL;
    const char* ticket_key  = NULL;
    const char* compression = NULL;
    const char* compressed  = NULL;
    const char* dump        = NULL;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        if (strcmp(argv[i], "--role") == 0)
        {
            role = argv[i + 1];
        }
        else if (strcmp(argv[i], "--groups") == 0)
        {
            groups = argv[i + 1];
        }
        else if (strcmp(argv[i], "--seed") == 0)
        {
            seed = argv[i + 1];
        }
        else if (strcmp(argv[i], "--pin") == 0)
        {
            pin = argv[i + 1];
        }
        else if (strcmp(argv[i], "--name") == 0)
        {
            name = argv[i + 1];
        }
        else if (strcmp(argv[i], "--max-version") == 0)
        {
            max_version = argv[i + 1];
        }
        else if (strcmp(argv[i], "--session") == 0)
        {
            g_session_path = argv[i + 1];
        }
        else if (strcmp(argv[i], "--ticket-key") == 0)
        {
            ticket_key = argv[i + 1];
        }
        else if (strcmp(argv[i], "--compression") == 0 && compression_of(argv[i + 1]) >= 0)
        {
            compression = argv[i + 1];
        }
        else if (strcmp(argv[i], "--compressed-cert") == 0)
        {
            compressed = argv[i + 1];
        }
        else if (strcmp(argv[i], "--dump-compressed") == 0)
        {
            dump = argv[i + 1];
        }
        else if (strcmp(argv[i], "--update") == 0 &&
                 (strcmp(argv[i + 1], "requested") == 0 || strcmp(argv[i + 1], "not-requested") == 0))
        {
            update = argv[i + 1];
        }
        else
        {
            return usage();
        }
    }
    unsigned char seed_bytes[32];
    unsigned char key_bytes[32];
    if (argc % 2 == 0 || role == NULL || groups == NULL || seed == NULL ||
        !parse_hex(seed, seed_bytes, sizeof(seed_bytes)) || (pin != NULL && !parse_hex(pin, g_pin, sizeof(g_pin))) ||
        (max_version != NULL && strcmp(max_version, "1.2") != 0) ||
        (ticket_key != NULL && !parse_hex(ticket_key, key_bytes, sizeof(key_bytes))))
    {
        return usage();
    }
    g_has_pin         = pin != NULL;
    const int client  = strcmp(role, "client") == 0;
    const int version = max_version != NULL ? TLS1_2_VERSION : TLS1_3_VERSION;

    EVP_PKEY* key         = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed_bytes, sizeof(seed_bytes));
    X509*     certificate = key != NULL ? self_signed(key, name) : NULL;
    SSL_CTX*  context     = SSL_CTX_new(TLS_method());
    if (certificate == NULL || context == NULL || !SSL_CTX_set_min_proto_version(context, version) ||
        !SSL_CTX_set_max_proto_version(context, version) || !SSL_CTX_set1_groups_list(context, groups) ||
        !SSL_CTX_use_certificate(context, certificate) || !SSL_CTX_use_PrivateKey(context, key))
    {
        ERR_print_errors_fp(stderr);
        return 1;
    }
    if (!client)
    {
        /* The server's group order decides, as on a Cerid server. A server that pins its client resumes only within
         * its session ID context. */
        static const unsigned char k_context[] = "crd-openssl-tls-peer";
        SSL_CTX_set_options(context, SSL_OP_CIPHER_SERVER_PREFERENCE);
        if (!SSL_CTX_set_session_id_context(context, k_context, sizeof(k_context) - 1))
        {
            return 1;
        }
    }
    if (ticket_key != NULL)
    {
        unsigned char material[80];
        memcpy(material, key_bytes, 32);
        memcpy(material + 32, key_bytes, 32);
        memcpy(material + 64, key_bytes, 16);
        if (client || SSL_CTX_set_tlsext_ticket_keys(context, material, sizeof(material)) != 1)
        {
            return usage();
        }
    }
    if (compression != NULL)
    {
        /* One algorithm, or none: a client offers it alone, a server compresses its certificate with it alone. */
        int       algorithm = compression_of(compression);
        const int off       = algorithm == TLSEXT_comp_cert_none;
        if (off)
        {
            SSL_CTX_set_options(context, client ? SSL_OP_NO_RX_CERTIFICATE_COMPRESSION
                                                : SSL_OP_NO_TX_CERTIFICATE_COMPRESSION);
        }
        else if (!SSL_CTX_set1_cert_comp_preference(context, &algorithm, 1) ||
                 (!client && compressed == NULL && !SSL_CTX_compress_certs(context, algorithm)) ||
                 (!client && compressed != NULL && !install_compressed(context, algorithm, compressed)) ||
                 (!client && dump != NULL && !dump_compressed(context, algorithm, dump)))
        {
            ERR_print_errors_fp(stderr);
            return 1;
        }
        if (client && (compressed != NULL || dump != NULL))
        {
            return usage();
        }
    }
    else if (compressed != NULL || dump != NULL)
    {
        return usage();
    }
    if (g_session_path != NULL)
    {
        if (!client)
        {
            return usage();
        }
        SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_sess_set_new_cb(context, keep_session);
    }
    if (g_has_pin)
    {
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER | (client ? 0 : SSL_VERIFY_FAIL_IF_NO_PEER_CERT), verify_pin);
    }
    SSL_CTX_set_msg_callback(context, observe);
    SSL* ssl      = SSL_new(context);
    BIO* incoming = BIO_new(BIO_s_mem());
    BIO* outgoing = BIO_new(BIO_s_mem());
    if (ssl == NULL || incoming == NULL || outgoing == NULL)
    {
        return 1;
    }
    BIO_set_mem_eof_return(incoming, -1);
    SSL_set_bio(ssl, incoming, outgoing);
    if (client)
    {
        SSL_set_connect_state(ssl);
        SSL_set_tlsext_host_name(ssl, name);
        SSL_SESSION* offered = g_session_path != NULL ? take_session() : NULL;
        if (offered != NULL)
        {
            SSL_set_session(ssl, offered);
            SSL_SESSION_free(offered);
        }
        SSL_do_handshake(ssl);
        if (!write_frame(outgoing))
        {
            return 1;
        }
    }
    else
    {
        SSL_set_accept_state(ssl);
    }

    char   received[256] = {0};
    size_t received_size = 0;
    int    failed        = 0;
    int    pinged        = 0;
    size_t size          = 0;
    while (read_frame(&size))
    {
        if (!failed && size > 0 && BIO_write(incoming, g_frame, (int)size) != (int)size)
        {
            return 1;
        }
        if (!failed && !SSL_is_init_finished(ssl))
        {
            const int result = SSL_do_handshake(ssl);
            failed           = result <= 0 && SSL_get_error(ssl, result) != SSL_ERROR_WANT_READ;
        }
        if (!failed && SSL_is_init_finished(ssl))
        {
            unsigned char plain[4096];
            for (;;)
            {
                const int got = SSL_read(ssl, plain, sizeof(plain));
                if (got <= 0)
                {
                    const int reason = SSL_get_error(ssl, got);
                    failed           = reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_ZERO_RETURN;
                    break;
                }
                const size_t keep = (size_t)got < sizeof(received) - 1 - received_size
                                        ? (size_t)got
                                        : sizeof(received) - 1 - received_size;
                memcpy(received + received_size, plain, keep);
                received_size += keep;
                if (!client)
                {
                    SSL_write(ssl, plain, got);
                }
            }
            if (client && !pinged && !failed)
            {
                pinged = SSL_write(ssl, k_ping, (int)(sizeof(k_ping) - 1)) > 0;
                if (pinged && update != NULL)
                {
                    const int type = strcmp(update, "requested") == 0 ? SSL_KEY_UPDATE_REQUESTED
                                                                       : SSL_KEY_UPDATE_NOT_REQUESTED;
                    failed = !SSL_key_update(ssl, type);
                }
            }
            /* A KeyUpdate owed (the client's own, or the answer to the peer's request, which OpenSSL otherwise holds
             * until its next record) goes out now: a zero-length write runs the state machine. */
            if (!failed && SSL_get_key_update_type(ssl) != SSL_KEY_UPDATE_NONE)
            {
                failed = SSL_write(ssl, k_ping, 0) < 0;
            }
        }
        if (!write_frame(outgoing))
        {
            return 1;
        }
    }

    const char* group = SSL_is_init_finished(ssl) ? SSL_get0_group_name(ssl) : NULL;
    const int established = !failed && SSL_is_init_finished(ssl);
    fprintf(stderr, "{\"state\": \"%s\", \"group\": \"", established ? "established" : "failed");
    for (const char* c = group != NULL ? group : ""; *c != '\0'; ++c)
    {
        fputc(tolower((unsigned char)*c), stderr);
    }
    fprintf(stderr, "\", \"hello_retry\": %s, \"peer_ok\": %s, \"alert_sent\": %d, \"alert_received\": %d, ",
            g_hello_retry ? "true" : "false", g_peer_ok ? "true" : "false", g_alert_sent, g_alert_received);
    fprintf(stderr, "\"key_updates_sent\": %d, \"key_updates_received\": %d, \"resumed\": %s, ", g_updates_sent,
            g_updates_received, established && SSL_session_reused(ssl) ? "true" : "false");
    fprintf(stderr, "\"compressed_sent\": %d, \"compressed_received\": %d, ", g_compressed_sent,
            g_compressed_received);
    fprintf(stderr, "\"version\": \"%s\", \"received\": \"", SSL_get_version(ssl));
    for (size_t i = 0; i < received_size; ++i)
    {
        const char c = received[i];
        fputc(c >= ' ' && c <= '~' && c != '"' && c != '\\' ? c : '?', stderr);
    }
    fputs("\"}\n", stderr);
    SSL_free(ssl);
    SSL_CTX_free(context);
    X509_free(certificate);
    EVP_PKEY_free(key);
    return 0;
}
