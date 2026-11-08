/* crd-crypto and crd-net interoperability tests -- a peer's self-signed certificate over OpenSSL, as the programs that
 * link OpenSSL make it (openssl_tls_peer.c and tests/foundation/net/interop/ngtcp2_quic_peer.c). */
#ifndef CRD_OPENSSL_SELF_SIGNED_H
#define CRD_OPENSSL_SELF_SIGNED_H

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <stdio.h>
#include <string.h>

/* The name is the subject's common name when it fits (64 characters, RFC 5280's ub-common-name) and the
 * subjectAltName's always, critical when the subject is empty, as on a Cerid credential. */
static inline X509* self_signed(EVP_PKEY* key, const char* name)
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

#endif
