/* crd-net interoperability tests and boards (NET-PATH) -- what the programs over msquic set up alike
 * (msquic_quic_peer.c and msquic_quic_board.c): the server's credential, a client's pin, the one version offered and
 * the loopback address. */
#ifndef CRD_MSQUIC_COMMON_H
#define CRD_MSQUIC_COMMON_H

#define QUIC_API_ENABLE_PREVIEW_FEATURES 1 /* QUIC_VERSION_SETTINGS */
#include "openssl_self_signed.h"

#include <msquic.h>
#include <openssl/pkcs12.h>

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

#define MSQUIC_PKCS12_PASSWORD "cerid-interop"

static const char MSQUIC_ALPN[]        = "cerid-interop";
static const char MSQUIC_SERVER_NAME[] = "cerid.test";

/* A client's pin: the leaf's raw Ed25519 public key is `pin`. */
static inline int msquic_pin_holds(const unsigned char pin[32], const QUIC_BUFFER* der)
{
    const unsigned char* bytes = der != NULL ? der->Buffer : NULL;
    X509*                leaf  = bytes != NULL ? d2i_X509(NULL, &bytes, (long)der->Length) : NULL;
    EVP_PKEY*            key   = leaf != NULL ? X509_get_pubkey(leaf) : NULL;
    unsigned char        found[32];
    size_t               size = sizeof(found);
    const int held = key != NULL && EVP_PKEY_get_raw_public_key(key, found, &size) == 1 && size == sizeof(found) &&
                     memcmp(found, pin, sizeof(found)) == 0;
    EVP_PKEY_free(key);
    X509_free(leaf);
    return held;
}

/* Loads `configuration`'s credential: a server's the seed's key and its self-signed certificate as PKCS #12, as msquic
 * reads it from memory; a client's none, its pin checked when msquic hands it the certificate. `config` carries what
 * the caller adds (flags, allowed cipher suites); its type, flags for the role and certificate are set here. */
static inline int msquic_load_credential(const QUIC_API_TABLE* api, HQUIC configuration, int client, EVP_PKEY* key,
                                         QUIC_CREDENTIAL_CONFIG* config)
{
    if (client)
    {
        config->Type = QUIC_CREDENTIAL_TYPE_NONE;
        config->Flags |= QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION |
                         QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED |
                         QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES;
        return QUIC_SUCCEEDED(api->ConfigurationLoadCredential(configuration, config));
    }
    X509*          certificate = self_signed(key, MSQUIC_SERVER_NAME);
    PKCS12*        bundle      = certificate != NULL ? PKCS12_create(MSQUIC_PKCS12_PASSWORD, NULL, key, certificate, NULL,
                                                                     0, 0, 0, 0, 0)
                                                     : NULL;
    unsigned char* der         = NULL;
    const int      size        = bundle != NULL ? i2d_PKCS12(bundle, &der) : -1;
    int            loaded      = 0;
    if (size > 0)
    {
        QUIC_CERTIFICATE_PKCS12 pkcs12 = {der, (uint32_t)size, MSQUIC_PKCS12_PASSWORD};
        config->Type                   = QUIC_CREDENTIAL_TYPE_CERTIFICATE_PKCS12;
        config->CertificatePkcs12      = &pkcs12;
        loaded = QUIC_SUCCEEDED(api->ConfigurationLoadCredential(configuration, config));
    }
    OPENSSL_free(der);
    PKCS12_free(bundle);
    X509_free(certificate);
    return loaded;
}

/* `version` is the one QUIC version `configuration` offers and accepts. */
static inline int msquic_offer_version(const QUIC_API_TABLE* api, HQUIC configuration, uint32_t version)
{
    const uint32_t        versions[1] = {version};
    QUIC_VERSION_SETTINGS offered     = {versions, versions, versions, 1U, 1U, 1U};
    return QUIC_SUCCEEDED(
        api->SetParam(configuration, QUIC_PARAM_CONFIGURATION_VERSION_SETTINGS, sizeof(offered), &offered));
}

/* 127.0.0.1:`port`. */
static inline QUIC_ADDR msquic_loopback(unsigned long port)
{
    QUIC_ADDR address;
    memset(&address, 0, sizeof(address));
    QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_INET);
    address.Ipv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    QuicAddrSetPort(&address, (uint16_t)port);
    return address;
}

#endif
