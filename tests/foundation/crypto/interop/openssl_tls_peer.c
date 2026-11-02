/* crd-openssl-tls-peer -- one end of a TLS connection through the pinned OpenSSL 4.0 (cmake/pins.json openssl), the
 * independent peer of crd-crypto's TLS 1.3 sessions, driven over its standard streams by
 * tests/foundation/crypto/interop/tls_interop.py (NET.2; docs/design/network-foundation.md#tls-sessions). It links only
 * OpenSSL, never AWS-LC: the two libraries share symbol names and cannot share a process.
 *
 * Usage: crd-openssl-tls-peer --role client|server --groups LIST --seed HEX [--pin HEX] [--name NAME]
 *                             [--max-version 1.2] [--update requested|not-requested] [--session FILE]
 *                             [--ticket-key HEX] [--compression zstd|zlib|none] [--compressed-cert FILE]
 *                             [--dump-compressed FILE] [--alpn LIST] [--quic 1|2] [--parameters HEX]
 *                             [--suites LIST] [--ech-configs HEX] [--ech-grease 1] [--ech-pem FILE]
 *                             [--ech-previous-pem FILE]
 *        crd-openssl-tls-peer --ech-generate NAME   (a new ECH key and its ECHConfigList for the public name NAME, in
 *                             the ECH PEM format of RFC 9934, on standard output)
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
 *   alpn         application protocols, comma-separated: a client offers them, a server selects the first of its own
 *                that its client offers and refuses one that offers none of them (no_application_protocol)
 *   quic         the handshake runs over QUIC (RFC 9001) through OpenSSL's QUIC TLS interface instead of records, and
 *                the probes are made for QUIC version 1 or 2; frames then carry CRYPTO data pieces (see below)
 *   parameters   the transport parameters a QUIC end sends (hex, at most 256 bytes)
 *   suites       the TLS 1.3 cipher suites, in OpenSSL's syntax and this end's order (SSL_CTX_set_ciphersuites)
 *   ech-configs  a client's ECHConfigList (hex): it offers ECH with it (SSL_set1_ech_config_list)
 *   ech-grease   a client without ECH configurations sends a GREASE ECH extension (SSL_OP_ECH_GREASE)
 *   ech-pem, ech-previous-pem  a server's ECH key and configuration in an RFC 9934 PEM file: a current one, sent as a
 *                retry configuration, or a previous one, still decrypted but not sent (OSSL_ECHSTORE_read_pem)
 *
 * A server prefers its own group order (SSL_OP_CIPHER_SERVER_PREFERENCE), as a Cerid server does. The framing and the
 * script are crd-tls-peer's: 4-byte big-endian frame lengths, a client's first flight unasked,
 * one frame per frame read, "ping from openssl" from an established client, an echo from an established server and one
 * JSON line on standard error at the end of input, the KeyUpdate messages sent and received, the algorithm of a
 * CompressedCertificate sent and received (handshake type 25, RFC 8879) and whether the handshake resumed among it.
 * Over QUIC no application data flows (an established client reads post-handshake messages only), and the JSON line
 * adds the level of the alert sent, the application protocol, the peer's transport parameters and one probe per
 * secret: a fixed packet sealed and a fixed sample masked under the keys RFC 9001 section 5 derives from it, computed
 * here through OpenSSL's HKDF and ciphers, which crd-tls-peer computes through crd-crypto. Every report also carries
 * OpenSSL's ECH status (SSL_ech_get1_status), the inner and outer server names it reports and, on a client, the retry
 * configurations a rejecting server sent (SSL_ech_get1_retry_config). */

#include <openssl/bio.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/ech.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <ctype.h>
#include <stdint.h>
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
    if (g_peer_ok)
    {
        /* The pin is the trust anchor: the verification result OpenSSL reports (ECH's status reads it) is success. */
        X509_STORE_CTX_set_error(store, X509_V_OK);
    }
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

/* ---- QUIC (--quic): the handshake over OpenSSL's QUIC TLS interface (SSL_set_quic_tls_cbs, OpenSSL 3.5) ----
 *
 * A frame carries CRYPTO data as pieces: a level byte (0 Initial, 1 Handshake, 2 Application), a 24-bit big-endian
 * length and that many bytes. OpenSSL sends at its latest write level and reads at its latest read level, so received
 * pieces wait in one queue per level until it reads there. The secrets it yields are kept for the probes. */

#define QUIC_LEVELS 4 /* OpenSSL's protection levels: none (Initial), early, handshake, application */
#define PROBE_NUMBER 0x2A

static int           g_quic_version;
static unsigned char g_quic_in[QUIC_LEVELS][FRAME_BYTES];
static size_t        g_quic_in_size[QUIC_LEVELS];
static size_t        g_quic_in_read[QUIC_LEVELS];
static uint32_t      g_read_level;
static uint32_t      g_write_level;
static uint32_t      g_record_level;
static unsigned char g_quic_out[FRAME_BYTES];
static size_t        g_quic_out_size;
static unsigned char g_secrets[QUIC_LEVELS][2][48];
static size_t        g_secret_size[QUIC_LEVELS][2];
static unsigned char g_peer_parameters[1024];
static size_t        g_peer_parameters_size;
static unsigned char g_parameters[256];
static size_t        g_parameters_size;
static int           g_alert_level = -1;
static unsigned char g_protocols[256];
static size_t        g_protocols_size;

/* The probe's packet: a 4-byte header (the associated data), 16 bytes of payload, and the sample its mask is made of
 * (tests/foundation/crypto/interop/cerid_tls_peer.cpp computes the same through crd-crypto). */
static const unsigned char k_probe_header[4]   = {0x41, 0xAA, 0xBB, 0xCC};
static const unsigned char k_probe_payload[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const unsigned char k_probe_sample[16]  = {0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61,
                                                  0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69};

/* The frame's level of an OpenSSL protection level, and back; -1 for the early level, which Cerid never uses. */
static int frame_level(uint32_t level)
{
    return level == OSSL_RECORD_PROTECTION_LEVEL_NONE        ? 0
           : level == OSSL_RECORD_PROTECTION_LEVEL_HANDSHAKE   ? 1
           : level == OSSL_RECORD_PROTECTION_LEVEL_APPLICATION ? 2
                                                               : -1;
}

static const uint32_t k_protection_level[3] = {OSSL_RECORD_PROTECTION_LEVEL_NONE,
                                               OSSL_RECORD_PROTECTION_LEVEL_HANDSHAKE,
                                               OSSL_RECORD_PROTECTION_LEVEL_APPLICATION};

static int quic_send(SSL* s, const unsigned char* buf, size_t buf_len, size_t* consumed, void* arg)
{
    const int level = frame_level(g_write_level);
    (void)s;
    (void)arg;
    if (level < 0 || buf_len > sizeof(g_quic_out) - g_quic_out_size - 4)
    {
        return 0;
    }
    g_quic_out[g_quic_out_size]     = (unsigned char)level;
    g_quic_out[g_quic_out_size + 1] = (unsigned char)(buf_len >> 16);
    g_quic_out[g_quic_out_size + 2] = (unsigned char)(buf_len >> 8);
    g_quic_out[g_quic_out_size + 3] = (unsigned char)buf_len;
    memcpy(g_quic_out + g_quic_out_size + 4, buf, buf_len);
    g_quic_out_size += 4 + buf_len;
    *consumed = buf_len;
    return 1;
}

static int quic_receive(SSL* s, const unsigned char** buf, size_t* bytes_read, void* arg)
{
    (void)s;
    (void)arg;
    g_record_level = g_read_level;
    *buf           = g_quic_in[g_record_level] + g_quic_in_read[g_record_level];
    *bytes_read    = g_quic_in_size[g_record_level] - g_quic_in_read[g_record_level];
    return 1;
}

static int quic_release(SSL* s, size_t bytes_read, void* arg)
{
    (void)s;
    (void)arg;
    g_quic_in_read[g_record_level] += bytes_read;
    return 1;
}

static int quic_secret(SSL* s, uint32_t level, int direction, const unsigned char* secret, size_t secret_len,
                       void* arg)
{
    (void)s;
    (void)arg;
    if (frame_level(level) < 0 || secret_len > sizeof(g_secrets[0][0]) || direction < 0 || direction > 1)
    {
        return 0;
    }
    memcpy(g_secrets[level][direction], secret, secret_len);
    g_secret_size[level][direction] = secret_len;
    *(direction == 1 ? &g_write_level : &g_read_level) = level;
    return 1;
}

static int quic_parameters(SSL* s, const unsigned char* params, size_t params_len, void* arg)
{
    (void)s;
    (void)arg;
    if (params_len > sizeof(g_peer_parameters))
    {
        return 0;
    }
    memcpy(g_peer_parameters, params, params_len);
    g_peer_parameters_size = params_len;
    return 1;
}

static int quic_alert(SSL* s, unsigned char alert_code, void* arg)
{
    (void)s;
    (void)arg;
    if (g_alert_sent < 0)
    {
        g_alert_sent  = alert_code;
        g_alert_level = frame_level(g_write_level);
    }
    return 1;
}

static const OSSL_DISPATCH k_quic_callbacks[] = {
    {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND, (void (*)(void))quic_send},
    {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RECV_RCD, (void (*)(void))quic_receive},
    {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RELEASE_RCD, (void (*)(void))quic_release},
    {OSSL_FUNC_SSL_QUIC_TLS_YIELD_SECRET, (void (*)(void))quic_secret},
    {OSSL_FUNC_SSL_QUIC_TLS_GOT_TRANSPORT_PARAMS, (void (*)(void))quic_parameters},
    {OSSL_FUNC_SSL_QUIC_TLS_ALERT, (void (*)(void))quic_alert},
    {0, NULL},
};

/* Queues a received frame's pieces at their levels. */
static int quic_take_frame(size_t size)
{
    size_t at = 0;
    while (at + 4 <= size)
    {
        const size_t length = ((size_t)g_frame[at + 1] << 16) | ((size_t)g_frame[at + 2] << 8) | g_frame[at + 3];
        if (g_frame[at] > 2 || length > size - at - 4)
        {
            return 0;
        }
        const uint32_t level = k_protection_level[g_frame[at]];
        if (length > sizeof(g_quic_in[0]) - g_quic_in_size[level])
        {
            return 0;
        }
        memcpy(g_quic_in[level] + g_quic_in_size[level], g_frame + at + 4, length);
        g_quic_in_size[level] += length;
        at += 4 + length;
    }
    return at == size;
}

static int quic_write_frame(void)
{
    const unsigned char header[4] = {(unsigned char)(g_quic_out_size >> 24), (unsigned char)(g_quic_out_size >> 16),
                                     (unsigned char)(g_quic_out_size >> 8), (unsigned char)g_quic_out_size};
    const int written = fwrite(header, 1, 4, stdout) == 4 &&
                        fwrite(g_quic_out, 1, g_quic_out_size, stdout) == g_quic_out_size && fflush(stdout) == 0;
    g_quic_out_size = 0;
    return written;
}

/* HKDF-Expand-Label (RFC 8446 section 7.1) with an empty context, through OpenSSL's HKDF. */
static int expand_label(const EVP_MD* digest, const unsigned char* secret, size_t secret_size, const char* label,
                        unsigned char* out, size_t out_size)
{
    unsigned char info[2 + 1 + 6 + 32 + 1];
    const size_t  label_size = strlen(label);
    size_t        at         = 0;
    int           mode       = EVP_KDF_HKDF_MODE_EXPAND_ONLY;
    if (label_size > 32)
    {
        return 0;
    }
    info[at++] = (unsigned char)(out_size >> 8);
    info[at++] = (unsigned char)out_size;
    info[at++] = (unsigned char)(6 + label_size);
    memcpy(info + at, "tls13 ", 6);
    at += 6;
    memcpy(info + at, label, label_size);
    at += label_size;
    info[at++] = 0;
    EVP_KDF*     kdf     = EVP_KDF_fetch(NULL, "HKDF", NULL);
    EVP_KDF_CTX* context = kdf != NULL ? EVP_KDF_CTX_new(kdf) : NULL;
    OSSL_PARAM   params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, (char*)EVP_MD_get0_name(digest), 0),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void*)secret, secret_size),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, info, at),
        OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE, &mode),
        OSSL_PARAM_construct_end(),
    };
    const int derived = context != NULL && EVP_KDF_derive(context, out, out_size, params) > 0;
    EVP_KDF_CTX_free(context);
    EVP_KDF_free(kdf);
    return derived;
}

/* RFC 9001 section 5 restated over OpenSSL: the probe packet sealed under the packet key and IV the secret derives
 * (the nonce the IV exclusive-or the packet number) and the 5-byte header protection mask of the probe sample, as
 * hex into `out` (2 * (16 + 16 + 5) + 1 bytes). */
static int quic_probe(const SSL_CIPHER* cipher, const unsigned char* secret, size_t secret_size, char* out)
{
    const uint32_t     suite    = SSL_CIPHER_get_id(cipher) & 0xFFFF;
    const EVP_MD*      digest   = suite == 0x1302 ? EVP_sha384() : EVP_sha256();
    const EVP_CIPHER*  aead     = suite == 0x1301   ? EVP_aes_128_gcm()
                                  : suite == 0x1302 ? EVP_aes_256_gcm()
                                                    : EVP_chacha20_poly1305();
    const EVP_CIPHER*  hp       = suite == 0x1301   ? EVP_aes_128_ecb()
                                  : suite == 0x1302 ? EVP_aes_256_ecb()
                                                    : EVP_chacha20();
    const size_t       key_size = suite == 0x1301 ? 16 : 32;
    const char*        prefix   = g_quic_version == 2 ? "quicv2 " : "quic ";
    char               label[16];
    unsigned char      key[32], iv[12], hp_key[32], sealed[32], mask[16];
    int                length = 0;
    EVP_CIPHER_CTX*    context = EVP_CIPHER_CTX_new();
    int                ok      = context != NULL && (suite == 0x1301 || suite == 0x1302 || suite == 0x1303);
    snprintf(label, sizeof(label), "%skey", prefix);
    ok = ok && expand_label(digest, secret, secret_size, label, key, key_size);
    snprintf(label, sizeof(label), "%siv", prefix);
    ok = ok && expand_label(digest, secret, secret_size, label, iv, sizeof(iv));
    snprintf(label, sizeof(label), "%shp", prefix);
    ok = ok && expand_label(digest, secret, secret_size, label, hp_key, key_size);
    iv[11] ^= PROBE_NUMBER;
    ok = ok && EVP_EncryptInit_ex(context, aead, NULL, NULL, NULL) == 1 &&
         EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_SET_IVLEN, sizeof(iv), NULL) == 1 &&
         EVP_EncryptInit_ex(context, NULL, NULL, key, iv) == 1 &&
         EVP_EncryptUpdate(context, NULL, &length, k_probe_header, sizeof(k_probe_header)) == 1 &&
         EVP_EncryptUpdate(context, sealed, &length, k_probe_payload, sizeof(k_probe_payload)) == 1 &&
         EVP_EncryptFinal_ex(context, sealed + length, &length) == 1 &&
         EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_GET_TAG, 16, sealed + 16) == 1;
    /* AES-ECB of the sample, or ChaCha20 keyed by the header key with the sample as its counter and nonce (OpenSSL's
     * 16-byte ChaCha20 IV is exactly that), over zeros. */
    memset(mask, 0, sizeof(mask));
    ok = ok && EVP_CIPHER_CTX_reset(context) == 1 &&
         EVP_EncryptInit_ex(context, hp, NULL, hp_key, suite == 0x1303 ? k_probe_sample : NULL) == 1 &&
         EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
         EVP_EncryptUpdate(context, mask, &length, suite == 0x1303 ? mask : k_probe_sample, 16) == 1;
    EVP_CIPHER_CTX_free(context);
    for (size_t i = 0; ok && i < 32 + 5; ++i)
    {
        snprintf(out + 2 * i, 3, "%02x", i < 32 ? sealed[i] : mask[i - 32]);
    }
    return ok;
}

/* The probes of every secret held, named by level and by whose packets it protects. */
static void quic_print_probes(SSL* ssl, int client)
{
    static const char* const k_levels[] = {"handshake", "application"};
    static const uint32_t    k_levels_of[] = {OSSL_RECORD_PROTECTION_LEVEL_HANDSHAKE,
                                              OSSL_RECORD_PROTECTION_LEVEL_APPLICATION};
    const SSL_CIPHER*        cipher = SSL_get_current_cipher(ssl);
    int                      first  = 1;
    fputs("\"probes\": {", stderr);
    for (int i = 0; i < 2 && cipher != NULL; ++i)
    {
        for (int direction = 0; direction < 2; ++direction)
        {
            const uint32_t level = k_levels_of[i];
            char           probe[2 * 37 + 1];
            if (g_secret_size[level][direction] == 0 ||
                !quic_probe(cipher, g_secrets[level][direction], g_secret_size[level][direction], probe))
            {
                continue;
            }
            /* A write secret protects this end's packets, a read secret its peer's. */
            const int client_side = (direction == 1) == client;
            fprintf(stderr, "%s\"%s-%s\": \"%s\"", first ? "" : ", ", k_levels[i], client_side ? "client" : "server",
                    probe);
            first = 0;
        }
    }
    fputs("}, ", stderr);
}

/* A comma-separated protocol list in ALPN's wire form. */
static int parse_protocols(const char* list)
{
    g_protocols_size = 0;
    for (const char* at = list; *at != '\0';)
    {
        const char*  end    = strchr(at, ',');
        const size_t length = end != NULL ? (size_t)(end - at) : strlen(at);
        if (length == 0 || length > 255 || length + 1 > sizeof(g_protocols) - g_protocols_size)
        {
            return 0;
        }
        g_protocols[g_protocols_size] = (unsigned char)length;
        memcpy(g_protocols + g_protocols_size + 1, at, length);
        g_protocols_size += 1 + length;
        at += length + (end != NULL ? 1 : 0);
    }
    return g_protocols_size > 0;
}

/* A server selects the first of its protocols its client offers; none is no_application_protocol. */
static int select_protocol(SSL* ssl, const unsigned char** out, unsigned char* out_len, const unsigned char* in,
                           unsigned int in_len, void* arg)
{
    unsigned char* selected = NULL;
    (void)ssl;
    (void)arg;
    if (SSL_select_next_proto(&selected, out_len, g_protocols, (unsigned int)g_protocols_size, in, in_len) !=
        OPENSSL_NPN_NEGOTIATED)
    {
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    *out = selected;
    return SSL_TLSEXT_ERR_OK;
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
    X509_NAME*      subject     = X509_NAME_new();
    X509_EXTENSION* extension   = NULL;
    int             made        = 0;
    if (snprintf(alt, sizeof(alt), "%sDNS:%s", common ? "" : "critical,", name) < (int)sizeof(alt))
    {
        extension = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name, alt);
        /* OpenSSL 4 hands out a certificate's names read-only: the subject is built apart, then set as both. */
        made = certificate != NULL && subject != NULL && extension != NULL && X509_set_version(certificate, 2) &&
               ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) &&
               X509_gmtime_adj(X509_getm_notBefore(certificate), -3600) != NULL &&
               X509_gmtime_adj(X509_getm_notAfter(certificate), 86400) != NULL &&
               X509_set_pubkey(certificate, key) &&
               (!common ||
                X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, (const unsigned char*)name, -1, -1, 0)) &&
               X509_set_subject_name(certificate, subject) && X509_set_issuer_name(certificate, subject) &&
               X509_add_ext(certificate, extension, -1) && X509_sign(certificate, key, NULL) > 0;
    }
    X509_EXTENSION_free(extension);
    X509_NAME_free(subject);
    if (!made)
    {
        X509_free(certificate);
        return NULL;
    }
    return certificate;
}

/* Hex of any even length up to `capacity` bytes. */
static int parse_hex_bytes(const char* text, unsigned char* out, size_t capacity, size_t* size)
{
    const size_t length = strlen(text);
    *size               = length / 2;
    return length % 2 == 0 && *size <= capacity && parse_hex(text, out, *size);
}

/* Adds the ECH key and configuration of an RFC 9934 PEM file to `store`, sent as retry configurations or not. */
static int read_ech_pem(OSSL_ECHSTORE* store, const char* path, int for_retry)
{
    BIO*      file   = BIO_new_file(path, "r");
    const int loaded = file != NULL && OSSL_ECHSTORE_read_pem(store, file, for_retry) == 1;
    BIO_free(file);
    return loaded;
}

/* A new ECH key of OpenSSL's default suite (DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, AES-128-GCM) and its
 * ECHConfigList for `name`, as RFC 9934 PEM on standard output. */
static int generate_ech(const char* name)
{
    OSSL_ECHSTORE* store = OSSL_ECHSTORE_new(NULL, NULL);
    BIO*           out   = BIO_new_fp(stdout, BIO_NOCLOSE);
    const int      made  = store != NULL && out != NULL &&
                     OSSL_ECHSTORE_new_config(store, OSSL_ECH_CURRENT_VERSION, 0, name,
                                              (OSSL_HPKE_SUITE)OSSL_HPKE_SUITE_DEFAULT) == 1 &&
                     OSSL_ECHSTORE_write_pem(store, 0, out) == 1;
    BIO_free(out);
    OSSL_ECHSTORE_free(store);
    if (!made)
    {
        ERR_print_errors_fp(stderr);
    }
    return made ? 0 : 1;
}

/* What OpenSSL reports about ECH: its status, the inner and outer names and a client's retry configurations. */
static void print_ech(SSL* ssl, int client)
{
    char*          inner  = NULL;
    char*          outer  = NULL;
    unsigned char* retry  = NULL;
    size_t         length = 0;
    const int      status = SSL_ech_get1_status(ssl, &inner, &outer);
    fprintf(stderr, "\"ech_status\": %d, \"ech_inner\": \"%s\", \"ech_outer\": \"%s\", \"ech_retry\": \"", status,
            inner != NULL ? inner : "", outer != NULL ? outer : "");
    if (client && SSL_ech_get1_retry_config(ssl, &retry, &length) == 1)
    {
        for (size_t i = 0; i < length; ++i)
        {
            fprintf(stderr, "%02x", retry[i]);
        }
    }
    fputs("\", ", stderr);
    OPENSSL_free(retry);
    OPENSSL_free(inner);
    OPENSSL_free(outer);
}

static int usage(void)
{
    fputs("usage: crd-openssl-tls-peer --role client|server --groups LIST --seed HEX [--pin HEX] [--name NAME] "
          "[--max-version 1.2] [--update requested|not-requested] [--session FILE] [--ticket-key HEX] "
          "[--compression zstd|zlib|none] [--compressed-cert FILE] [--dump-compressed FILE] [--alpn LIST] "
          "[--quic 1|2] [--parameters HEX] [--suites LIST] [--ech-configs HEX] [--ech-grease 1] [--ech-pem FILE] "
          "[--ech-previous-pem FILE]\n",
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
    if (argc == 3 && strcmp(argv[1], "--ech-generate") == 0)
    {
        return generate_ech(argv[2]);
    }
    static unsigned char ech_configs[8192];
    size_t               ech_config_bytes = 0;
    int                  ech_grease       = 0;
    const char*          ech_pem          = NULL;
    const char*          ech_previous_pem = NULL;
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
    const char* suites      = NULL;
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
        else if (strcmp(argv[i], "--suites") == 0)
        {
            suites = argv[i + 1];
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
        else if (strcmp(argv[i], "--alpn") == 0)
        {
            if (!parse_protocols(argv[i + 1]))
            {
                return usage();
            }
        }
        else if (strcmp(argv[i], "--quic") == 0 && (strcmp(argv[i + 1], "1") == 0 || strcmp(argv[i + 1], "2") == 0))
        {
            g_quic_version = argv[i + 1][0] - '0';
        }
        else if (strcmp(argv[i], "--parameters") == 0 && strlen(argv[i + 1]) % 2 == 0 &&
                 strlen(argv[i + 1]) / 2 <= sizeof(g_parameters) &&
                 parse_hex(argv[i + 1], g_parameters, strlen(argv[i + 1]) / 2))
        {
            g_parameters_size = strlen(argv[i + 1]) / 2;
        }
        else if (strcmp(argv[i], "--update") == 0 &&
                 (strcmp(argv[i + 1], "requested") == 0 || strcmp(argv[i + 1], "not-requested") == 0))
        {
            update = argv[i + 1];
        }
        else if (strcmp(argv[i], "--ech-configs") == 0 &&
                 parse_hex_bytes(argv[i + 1], ech_configs, sizeof(ech_configs), &ech_config_bytes))
        {
            continue;
        }
        else if (strcmp(argv[i], "--ech-grease") == 0 && strcmp(argv[i + 1], "1") == 0)
        {
            ech_grease = 1;
        }
        else if (strcmp(argv[i], "--ech-pem") == 0)
        {
            ech_pem = argv[i + 1];
        }
        else if (strcmp(argv[i], "--ech-previous-pem") == 0)
        {
            ech_previous_pem = argv[i + 1];
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
        (suites != NULL && !SSL_CTX_set_ciphersuites(context, suites)) ||
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
    if (g_protocols_size > 0)
    {
        if (client && SSL_CTX_set_alpn_protos(context, g_protocols, (unsigned int)g_protocols_size) != 0)
        {
            return 1;
        }
        if (!client)
        {
            SSL_CTX_set_alpn_select_cb(context, select_protocol, NULL);
        }
    }
    if (ech_pem != NULL || ech_previous_pem != NULL)
    {
        /* A server's ECH keys: the current one's configuration is its retry configuration. */
        OSSL_ECHSTORE* store = OSSL_ECHSTORE_new(NULL, NULL);
        const int      ready = !client && ech_pem != NULL && store != NULL && read_ech_pem(store, ech_pem, 1) &&
                          (ech_previous_pem == NULL || read_ech_pem(store, ech_previous_pem, 0)) &&
                          SSL_CTX_set1_echstore(context, store) == 1;
        OSSL_ECHSTORE_free(store);
        if (!ready)
        {
            ERR_print_errors_fp(stderr);
            return usage();
        }
    }
    if (!client && (ech_config_bytes > 0 || ech_grease))
    {
        return usage();
    }
    SSL_CTX_set_msg_callback(context, observe);
    SSL* ssl      = SSL_new(context);
    BIO* incoming = BIO_new(BIO_s_mem());
    BIO* outgoing = BIO_new(BIO_s_mem());
    if (ssl == NULL || incoming == NULL || outgoing == NULL || (g_quic_version != 0) != (g_parameters_size > 0) ||
        (g_quic_version != 0 && (!SSL_set_quic_tls_cbs(ssl, k_quic_callbacks, NULL) ||
                                 !SSL_set_quic_tls_transport_params(ssl, g_parameters, g_parameters_size))))
    {
        ERR_print_errors_fp(stderr);
        return 1;
    }
    BIO_set_mem_eof_return(incoming, -1);
    SSL_set_bio(ssl, incoming, outgoing);
    if (client)
    {
        SSL_set_connect_state(ssl);
        SSL_set_tlsext_host_name(ssl, name);
        if (ech_grease)
        {
            SSL_set_options(ssl, SSL_OP_ECH_GREASE);
        }
        if (ech_config_bytes > 0 && SSL_set1_ech_config_list(ssl, ech_configs, ech_config_bytes) != 1)
        {
            ERR_print_errors_fp(stderr);
            return 1;
        }
        SSL_SESSION* offered = g_session_path != NULL ? take_session() : NULL;
        if (offered != NULL)
        {
            SSL_set_session(ssl, offered);
            SSL_SESSION_free(offered);
        }
        const int started = SSL_do_handshake(ssl);
        if (started <= 0 && SSL_get_error(ssl, started) != SSL_ERROR_WANT_READ)
        {
            /* The library's own reasons, on standard error ahead of the report: a client that cannot even start. */
            ERR_print_errors_fp(stderr);
        }
        if (!(g_quic_version != 0 ? quic_write_frame() : write_frame(outgoing)))
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
        if (g_quic_version != 0 ? !quic_take_frame(size)
                                : !failed && size > 0 && BIO_write(incoming, g_frame, (int)size) != (int)size)
        {
            return 1;
        }
        if (!failed && !SSL_is_init_finished(ssl))
        {
            const int result = SSL_do_handshake(ssl);
            failed           = result <= 0 && SSL_get_error(ssl, result) != SSL_ERROR_WANT_READ;
        }
        if (!failed && SSL_is_init_finished(ssl) && g_quic_version != 0)
        {
            /* Post-handshake messages (a client's tickets) are read through SSL_read; no application data comes. */
            unsigned char plain[16];
            size_t        got = 0;
            if (!SSL_read_ex(ssl, plain, sizeof(plain), &got))
            {
                const int reason = SSL_get_error(ssl, 0);
                failed           = reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_ZERO_RETURN;
            }
        }
        else if (!failed && SSL_is_init_finished(ssl))
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
        if (!(g_quic_version != 0 ? quic_write_frame() : write_frame(outgoing)))
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
    const unsigned char* protocol        = NULL;
    unsigned int         protocol_length = 0;
    SSL_get0_alpn_selected(ssl, &protocol, &protocol_length);
    const SSL_CIPHER*    cipher          = established ? SSL_get_current_cipher(ssl) : NULL;
    fprintf(stderr, "\"alpn\": \"%.*s\", \"suite\": \"%s\", ", (int)protocol_length,
            protocol != NULL ? (const char*)protocol : "", cipher != NULL ? SSL_CIPHER_get_name(cipher) : "");
    print_ech(ssl, client);
    /* The exporter under Cerid's interop label and context, and Cerid's world capability binding (no context). */
    static const char k_interop_label[] = "EXPERIMENTAL Cerid interop";
    static const char k_binding_label[] = "EXPERIMENTAL Cerid capability world";
    unsigned char     exported[32];
    unsigned char     binding[32];
    const int         exports =
        established &&
        SSL_export_keying_material(ssl, exported, sizeof(exported), k_interop_label, strlen(k_interop_label),
                                   (const unsigned char*)"cerid", 5, 1) == 1 &&
        SSL_export_keying_material(ssl, binding, sizeof(binding), k_binding_label, strlen(k_binding_label), NULL, 0,
                                   1) == 1;
    fputs("\"exporter\": \"", stderr);
    for (size_t i = 0; exports && i < sizeof(exported); ++i)
    {
        fprintf(stderr, "%02x", exported[i]);
    }
    fputs("\", \"binding\": \"", stderr);
    for (size_t i = 0; exports && i < sizeof(binding); ++i)
    {
        fprintf(stderr, "%02x", binding[i]);
    }
    fputs("\", ", stderr);
    if (g_quic_version != 0)
    {
        fprintf(stderr, "\"alert_level\": %d, \"peer_parameters\": \"", g_alert_level);
        for (size_t i = 0; i < g_peer_parameters_size; ++i)
        {
            fprintf(stderr, "%02x", g_peer_parameters[i]);
        }
        fputs("\", ", stderr);
        quic_print_probes(ssl, client);
    }
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
