/* The OpenSSL half of crd-crypto-coexist-tests (NET.2 decision P5), defined in openssl_coexist.c over the pinned
 * OpenSSL 4.0; its callers see no OpenSSL header. Each function returns 1 when every step held, else 0. */
#ifndef CRD_TESTS_CRYPTO_OPENSSL_COEXIST_H
#define CRD_TESTS_CRYPTO_OPENSSL_COEXIST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* OpenSSL_version_num(): 0xMNN00PP0L for version M.NN.PP. */
unsigned long crd_coexist_openssl_version(void);

/* The SHA-256 of `size` bytes at `data` into `out`. */
int crd_coexist_openssl_sha256(const unsigned char* data, size_t size, unsigned char out[32]);

/* Opens AES-256-GCM `sealed` (ciphertext then a 16-byte tag) under `key` and a 12-byte `nonce` into `out`
 * (sealed_size - 16 bytes). */
int crd_coexist_openssl_open(const unsigned char key[32], const unsigned char nonce[12], const unsigned char* sealed,
                             size_t sealed_size, const unsigned char* associated, size_t associated_size,
                             unsigned char* out);

/* A TLS 1.3 connection between two of OpenSSL's own ends over a BIO pair: the server's self-signed Ed25519
 * certificate verified against its name, then one record each way. */
int crd_coexist_openssl_tls(void);

#ifdef __cplusplus
}
#endif

#endif
