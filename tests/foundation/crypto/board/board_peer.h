/* crd-crypto's boards (NET.2, CRD_BUILD_CRYPTO_VS_PEERS): the operations every arm of crd-crypto-board times, as one
 * table per library. crypto_board.cpp holds crd-crypto's; board_peer.c, compiled once per peer library against that
 * library's own headers, holds AWS-LC's (called directly, without crd-crypto), BoringSSL's and OpenSSL's. Each table
 * runs the same work with its library's own best interface: the encodings are crd-crypto's (decision P4), so one
 * library's output is checked by every other before anything is timed. Every function returns 1 on success, 0 on any
 * failure; a state is NULL when it could not be made. */
#ifndef CRD_CRYPTO_BOARD_PEER_H
#define CRD_CRYPTO_BOARD_PEER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    CRD_BOARD_AES_128_GCM,
    CRD_BOARD_AES_256_GCM,
    CRD_BOARD_CHACHA20_POLY1305
};
enum
{
    CRD_BOARD_ED25519,
    CRD_BOARD_ECDSA_P256_SHA256,
    CRD_BOARD_ML_DSA_65
};
enum
{
    CRD_BOARD_X25519,
    CRD_BOARD_X25519_MLKEM768
};

#define CRD_BOARD_NONCE_BYTES 12U
#define CRD_BOARD_TAG_BYTES 16U
#define CRD_BOARD_KEM_PUBLIC_BYTES 1184U
#define CRD_BOARD_KEM_CIPHERTEXT_BYTES 1088U
#define CRD_BOARD_SECRET_BYTES 32U
#define CRD_BOARD_MAX_PUBLIC_BYTES 1952U    /* ML-DSA-65's public key */
#define CRD_BOARD_MAX_SIGNATURE_BYTES 3309U /* ML-DSA-65's signature */
#define CRD_BOARD_MAX_RECORD_BYTES 16384U   /* TLS 1.3's largest record plaintext */
#define CRD_BOARD_SERVER_NAME "game.cerid.test"

struct crd_board_peer
{
    const char* name;    /* the arm's label on the board */
    const char* version; /* the library's pinned version */

    /* AEAD (RFC 5116): a key of the algorithm's length, held expanded. seal writes size + tag bytes; open reads them
     * and writes size bytes. */
    void* (*aead_new)(int algorithm, const uint8_t* key);
    int (*aead_seal)(void* aead, const uint8_t* nonce, const uint8_t* in, size_t size, uint8_t* out);
    int (*aead_open)(void* aead, const uint8_t* nonce, const uint8_t* in, size_t size, uint8_t* out);
    void (*aead_free)(void* aead);

    /* One ephemeral X25519 agreement, as a TLS key share takes it: a new key pair, then its secret with `peer`. */
    int (*x25519)(const uint8_t* peer, uint8_t* secret);

    /* ML-KEM-768 (FIPS 203): a new key and its encapsulation key; encapsulation to an encoded key, which it parses
     * first; decapsulation with a held key. */
    void* (*kem_new)(uint8_t* public_key);
    int (*kem_encapsulate)(const uint8_t* public_key, uint8_t* ciphertext, uint8_t* secret);
    int (*kem_decapsulate)(void* kem, const uint8_t* ciphertext, uint8_t* secret);
    void (*kem_free)(void* kem);

    /* Signatures: a new key and its encoded public key; signing with it (ML-DSA pure with an empty context, Ed25519
     * without prehash, ECDSA over SHA-256 as DER); verification from the encoded public key, which it parses first. */
    void* (*signer_new)(int algorithm, uint8_t* public_key, size_t* public_bytes);
    int (*sign)(void* signer, const uint8_t* message, size_t size, uint8_t* signature, size_t* signature_bytes);
    int (*verify)(int algorithm, const uint8_t* public_key, size_t public_bytes, const uint8_t* message, size_t size,
                  const uint8_t* signature, size_t signature_bytes);
    void (*signer_free)(void* signer);

    /* TLS 1.3 in memory, the same profile on every arm: the one group (a key share for it alone), AES-128-GCM, a
     * server holding a self-signed Ed25519 certificate whose key the client pins, one stateless ticket per connection
     * kept by the client, GREASE where the library has it, no certificate compression, no session cache. tls_new
     * makes both contexts and an established pair; tls_connect runs one connection to its end (a full handshake, or a
     * resumption on the last ticket) with the client reading its new ticket, and checks the group, the AEAD and
     * resumption; tls_record moves `size` bytes from the pair's client to its server. */
    void* (*tls_new)(int group);
    int (*tls_connect)(void* tls, int resume);
    int (*tls_record)(void* tls, const uint8_t* data, size_t size, uint8_t* out);
    void (*tls_free)(void* tls);
};

extern const struct crd_board_peer crd_board_peer_awslc;
extern const struct crd_board_peer crd_board_peer_boringssl;
extern const struct crd_board_peer crd_board_peer_openssl;

#ifdef __cplusplus
}
#endif

#endif /* CRD_CRYPTO_BOARD_PEER_H */
