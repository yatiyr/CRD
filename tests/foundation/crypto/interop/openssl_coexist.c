/* The OpenSSL half of crd-crypto-coexist-tests (NET.2 decision P5): this unit alone is compiled against the pinned
 * OpenSSL 4.0's headers, and the program links OpenSSL's libcrypto and libssl beside crd-crypto's AWS-LC, whose
 * symbols carry the provider prefix. Each function runs through OpenSSL's own code, so the test proves that both
 * libraries work in one process, each call bound to its own library. */

#include "openssl_coexist.h"

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <string.h>

unsigned long crd_coexist_openssl_version(void)
{
    return OpenSSL_version_num();
}

int crd_coexist_openssl_sha256(const unsigned char* data, size_t size, unsigned char out[32])
{
    unsigned int length = 0;
    return EVP_Digest(data, size, out, &length, EVP_sha256(), NULL) == 1 && length == 32U;
}

int crd_coexist_openssl_open(const unsigned char key[32], const unsigned char nonce[12], const unsigned char* sealed,
                             size_t sealed_size, const unsigned char* associated, size_t associated_size,
                             unsigned char* out)
{
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    unsigned char   tag[16];
    int             length = 0;
    int             ok     = context != NULL && sealed_size >= sizeof tag;
    size_t          plain  = ok ? sealed_size - sizeof tag : 0U;
    if (ok)
    {
        memcpy(tag, sealed + plain, sizeof tag);
    }
    ok = ok && EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), NULL, key, nonce) == 1;
    ok = ok && EVP_DecryptUpdate(context, NULL, &length, associated, (int)associated_size) == 1;
    ok = ok && EVP_DecryptUpdate(context, out, &length, sealed, (int)plain) == 1 && (size_t)length == plain;
    ok = ok && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, (int)sizeof tag, tag) == 1;
    ok = ok && EVP_DecryptFinal_ex(context, out + plain, &length) == 1;
    EVP_CIPHER_CTX_free(context);
    return ok;
}

static X509* self_signed(EVP_PKEY* key, const char* name)
{
    X509*      certificate = X509_new();
    X509_NAME* subject     = NULL;
    int        ok          = certificate != NULL;
    ok = ok && X509_set_version(certificate, X509_VERSION_3) == 1;
    ok = ok && ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) == 1;
    ok = ok && X509_gmtime_adj(X509_getm_notBefore(certificate), -60) != NULL;
    ok = ok && X509_gmtime_adj(X509_getm_notAfter(certificate), 3600) != NULL;
    ok = ok && X509_set_pubkey(certificate, key) == 1;
    if (ok)
    {
        subject = X509_get_subject_name(certificate);
    }
    ok = ok && X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, (const unsigned char*)name, -1, -1, 0) == 1;
    ok = ok && X509_set_issuer_name(certificate, subject) == 1;
    ok = ok && X509_sign(certificate, key, NULL) > 0;
    if (!ok)
    {
        X509_free(certificate);
        return NULL;
    }
    return certificate;
}

static int pending(SSL* ssl, int result)
{
    int error = SSL_get_error(ssl, result);
    return result == 1 || error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE;
}

int crd_coexist_openssl_tls(void)
{
    static const char name[] = "coexist.cerid.test";
    static const char ping[] = "ping from openssl";
    char              echo[sizeof ping];
    EVP_PKEY*         key         = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
    X509*             certificate = key != NULL ? self_signed(key, name) : NULL;
    SSL_CTX*          server_ctx  = SSL_CTX_new(TLS_server_method());
    SSL_CTX*          client_ctx  = SSL_CTX_new(TLS_client_method());
    SSL*              server      = NULL;
    SSL*              client      = NULL;
    BIO*              client_bio  = NULL;
    BIO*              server_bio  = NULL;
    int               ok          = certificate != NULL && server_ctx != NULL && client_ctx != NULL;
    int               done        = 0;
    ok = ok && SSL_CTX_set_min_proto_version(server_ctx, TLS1_3_VERSION) == 1;
    ok = ok && SSL_CTX_set_min_proto_version(client_ctx, TLS1_3_VERSION) == 1;
    ok = ok && SSL_CTX_use_certificate(server_ctx, certificate) == 1;
    ok = ok && SSL_CTX_use_PrivateKey(server_ctx, key) == 1;
    ok = ok && X509_STORE_add_cert(SSL_CTX_get_cert_store(client_ctx), certificate) == 1;
    if (ok)
    {
        SSL_CTX_set_verify(client_ctx, SSL_VERIFY_PEER, NULL);
        server = SSL_new(server_ctx);
        client = SSL_new(client_ctx);
    }
    ok = ok && server != NULL && client != NULL && SSL_set1_host(client, name) == 1;
    ok = ok && BIO_new_bio_pair(&client_bio, 0, &server_bio, 0) == 1;
    if (ok)
    {
        SSL_set_bio(client, client_bio, client_bio);
        SSL_set_bio(server, server_bio, server_bio);
        SSL_set_connect_state(client);
        SSL_set_accept_state(server);
    }
    for (int round = 0; ok && !done && round < 16; ++round)
    {
        int client_result = SSL_do_handshake(client);
        int server_result = SSL_do_handshake(server);
        ok   = pending(client, client_result) && pending(server, server_result);
        done = client_result == 1 && server_result == 1;
    }
    ok = ok && done && SSL_version(client) == TLS1_3_VERSION && SSL_get_verify_result(client) == X509_V_OK;
    ok = ok && SSL_write(client, ping, (int)sizeof ping) == (int)sizeof ping;
    ok = ok && SSL_read(server, echo, (int)sizeof echo) == (int)sizeof echo && memcmp(echo, ping, sizeof ping) == 0;
    ok = ok && SSL_write(server, echo, (int)sizeof echo) == (int)sizeof echo;
    ok = ok && SSL_read(client, echo, (int)sizeof echo) == (int)sizeof echo && memcmp(echo, ping, sizeof ping) == 0;
    SSL_free(client);
    SSL_free(server);
    SSL_CTX_free(client_ctx);
    SSL_CTX_free(server_ctx);
    X509_free(certificate);
    EVP_PKEY_free(key);
    return ok;
}
