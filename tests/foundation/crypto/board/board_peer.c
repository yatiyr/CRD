/* crd-crypto's boards (NET.2, CRD_BUILD_CRYPTO_VS_PEERS): one peer library's arm of crd-crypto-board, compiled once per
 * library against its own headers, each object defining only its table (CRD_BOARD_PEER names it). The BoringSSL family
 * (BoringSSL and AWS-LC) shares the EVP_AEAD, X25519 and SSL interfaces; ML-KEM and ML-DSA are
 * BoringSSL's own MLKEM768 and MLDSA65 functions and AWS-LC's EVP_PKEY_KEM and EVP_PKEY_PQDSA (the route crd-crypto
 * takes); OpenSSL 4.0 runs everything through EVP and its fetched algorithms. A context that can serve every
 * operation is made once, as each library's documentation advises; what crd-crypto's interface makes per call (the
 * parsed public keys) is made per call here too. */
#include "board_peer.h"

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* BoringSSL's interfaces (BoringSSL and AWS-LC, which names itself alone), and BoringSSL itself. */
#if defined(OPENSSL_IS_BORINGSSL) || defined(OPENSSL_IS_AWSLC)
#define BOARD_FAMILY 1
#endif
#if defined(OPENSSL_IS_BORINGSSL) && !defined(OPENSSL_IS_AWSLC)
#define BOARD_BORINGSSL 1
#endif

#if defined(BOARD_FAMILY)
#include <openssl/aead.h>
#include <openssl/bytestring.h>
#include <openssl/curve25519.h>
#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/nid.h>
#if defined(BOARD_BORINGSSL)
#include <openssl/mldsa.h>
#include <openssl/mlkem.h>
#endif
#else
#include <openssl/core_names.h>
#endif

#if !defined(CRD_BOARD_PEER) || !defined(CRD_BOARD_PEER_NAME) || !defined(CRD_BOARD_PEER_VERSION)
#error "the build names the table (CRD_BOARD_PEER), its arm and the library's pinned version"
#endif

#define BOARD_IO_BYTES (1U << 17)

/* Ed25519 keys from their raw bytes, and the key type a generated ML-DSA-65 key has (EVP's own only on AWS-LC). */
#if defined(BOARD_FAMILY)
#define BOARD_ED25519_PUBLIC(bytes, size) EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, bytes, size)
#define BOARD_ED25519_PRIVATE(bytes, size) EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, bytes, size)
#if defined(OPENSSL_IS_AWSLC)
#define BOARD_PKEY_ML_DSA EVP_PKEY_PQDSA
#else
#define BOARD_PKEY_ML_DSA EVP_PKEY_NONE
#endif
#else
#define BOARD_ED25519_PUBLIC(bytes, size) EVP_PKEY_new_raw_public_key_ex(NULL, "ED25519", NULL, bytes, size)
#define BOARD_ED25519_PRIVATE(bytes, size) EVP_PKEY_new_raw_private_key_ex(NULL, "ED25519", NULL, bytes, size)
#endif

/* ---- AEAD ---- */

#if defined(BOARD_FAMILY)
static const EVP_AEAD* board_aead_method(int algorithm)
{
    switch (algorithm)
    {
    case CRD_BOARD_AES_128_GCM:
        return EVP_aead_aes_128_gcm();
    case CRD_BOARD_AES_256_GCM:
        return EVP_aead_aes_256_gcm();
    case CRD_BOARD_CHACHA20_POLY1305:
        return EVP_aead_chacha20_poly1305();
    default:
        return NULL;
    }
}

static void* board_aead_new(int algorithm, const uint8_t* key)
{
    const EVP_AEAD* method = board_aead_method(algorithm);
    EVP_AEAD_CTX*   aead   = method != NULL ? malloc(sizeof(EVP_AEAD_CTX)) : NULL;
    if (aead == NULL)
    {
        return NULL;
    }
    if (!EVP_AEAD_CTX_init(aead, method, key, EVP_AEAD_key_length(method), CRD_BOARD_TAG_BYTES, NULL))
    {
        free(aead);
        return NULL;
    }
    return aead;
}

static int board_aead_seal(void* aead, const uint8_t* nonce, const uint8_t* in, size_t size, uint8_t* out)
{
    size_t written = 0;
    return EVP_AEAD_CTX_seal(aead, out, &written, size + CRD_BOARD_TAG_BYTES, nonce, CRD_BOARD_NONCE_BYTES, in, size,
                             NULL, 0) &&
           written == size + CRD_BOARD_TAG_BYTES;
}

static int board_aead_open(void* aead, const uint8_t* nonce, const uint8_t* in, size_t size, uint8_t* out)
{
    size_t written = 0;
    return EVP_AEAD_CTX_open(aead, out, &written, size, nonce, CRD_BOARD_NONCE_BYTES, in, size + CRD_BOARD_TAG_BYTES,
                             NULL, 0) &&
           written == size;
}

static void board_aead_free(void* aead)
{
    if (aead != NULL)
    {
        EVP_AEAD_CTX_cleanup(aead);
        free(aead);
    }
}
#else
/* One context per direction, keyed once; each operation sets only its nonce. */
struct board_aead
{
    EVP_CIPHER*     cipher;
    EVP_CIPHER_CTX* seal;
    EVP_CIPHER_CTX* open;
};

static void board_aead_free(void* state)
{
    struct board_aead* aead = state;
    if (aead != NULL)
    {
        EVP_CIPHER_CTX_free(aead->seal);
        EVP_CIPHER_CTX_free(aead->open);
        EVP_CIPHER_free(aead->cipher);
        free(aead);
    }
}

static void* board_aead_new(int algorithm, const uint8_t* key)
{
    static const char* const names[] = {"AES-128-GCM", "AES-256-GCM", "ChaCha20-Poly1305"};
    if (algorithm < 0 || algorithm > CRD_BOARD_CHACHA20_POLY1305)
    {
        return NULL;
    }
    struct board_aead* aead = calloc(1, sizeof(struct board_aead));
    if (aead == NULL)
    {
        return NULL;
    }
    aead->cipher = EVP_CIPHER_fetch(NULL, names[algorithm], NULL);
    aead->seal   = EVP_CIPHER_CTX_new();
    aead->open   = EVP_CIPHER_CTX_new();
    if (aead->cipher == NULL || aead->seal == NULL || aead->open == NULL ||
        !EVP_EncryptInit_ex2(aead->seal, aead->cipher, key, NULL, NULL) ||
        !EVP_DecryptInit_ex2(aead->open, aead->cipher, key, NULL, NULL))
    {
        board_aead_free(aead);
        return NULL;
    }
    return aead;
}

static int board_aead_seal(void* state, const uint8_t* nonce, const uint8_t* in, size_t size, uint8_t* out)
{
    struct board_aead* aead    = state;
    int                written = 0;
    int                last    = 0;
    return EVP_EncryptInit_ex2(aead->seal, NULL, NULL, nonce, NULL) &&
           EVP_EncryptUpdate(aead->seal, out, &written, in, (int)size) && (size_t)written == size &&
           EVP_EncryptFinal_ex(aead->seal, out + written, &last) && last == 0 &&
           EVP_CIPHER_CTX_ctrl(aead->seal, EVP_CTRL_AEAD_GET_TAG, CRD_BOARD_TAG_BYTES, out + size);
}

static int board_aead_open(void* state, const uint8_t* nonce, const uint8_t* in, size_t size, uint8_t* out)
{
    struct board_aead* aead    = state;
    int                written = 0;
    int                last    = 0;
    return EVP_DecryptInit_ex2(aead->open, NULL, NULL, nonce, NULL) &&
           EVP_DecryptUpdate(aead->open, out, &written, in, (int)size) && (size_t)written == size &&
           EVP_CIPHER_CTX_ctrl(aead->open, EVP_CTRL_AEAD_SET_TAG, CRD_BOARD_TAG_BYTES, (void*)(in + size)) &&
           EVP_DecryptFinal_ex(aead->open, out + written, &last) > 0 && last == 0;
}
#endif

/* ---- X25519 ---- */

#if defined(BOARD_FAMILY)
static int board_x25519(const uint8_t* peer, uint8_t* secret)
{
    uint8_t public_key[32];
    uint8_t private_key[32];
    X25519_keypair(public_key, private_key);
    const int agreed = X25519(secret, private_key, peer);
    OPENSSL_cleanse(private_key, sizeof(private_key));
    return agreed;
}
#else
static int board_x25519(const uint8_t* peer, uint8_t* secret)
{
    EVP_PKEY*     own     = EVP_PKEY_Q_keygen(NULL, NULL, "X25519");
    EVP_PKEY*     other   = EVP_PKEY_new_raw_public_key_ex(NULL, "X25519", NULL, peer, 32);
    EVP_PKEY_CTX* context = own != NULL ? EVP_PKEY_CTX_new_from_pkey(NULL, own, NULL) : NULL;
    size_t        length  = CRD_BOARD_SECRET_BYTES;
    const int     agreed  = context != NULL && other != NULL && EVP_PKEY_derive_init(context) > 0 &&
                       EVP_PKEY_derive_set_peer(context, other) > 0 && EVP_PKEY_derive(context, secret, &length) > 0 &&
                       length == CRD_BOARD_SECRET_BYTES;
    EVP_PKEY_CTX_free(context);
    EVP_PKEY_free(other);
    EVP_PKEY_free(own);
    return agreed;
}
#endif

/* ---- ML-KEM-768 ---- */

#if defined(BOARD_BORINGSSL)
static void* board_kem_new(uint8_t* public_key)
{
    struct MLKEM768_private_key* kem = malloc(sizeof(struct MLKEM768_private_key));
    if (kem != NULL)
    {
        MLKEM768_generate_key(public_key, NULL, kem);
    }
    return kem;
}

static int board_kem_encapsulate(const uint8_t* public_key, uint8_t* ciphertext, uint8_t* secret)
{
    struct MLKEM768_public_key* parsed = malloc(sizeof(struct MLKEM768_public_key));
    CBS                         encoded;
    CBS_init(&encoded, public_key, CRD_BOARD_KEM_PUBLIC_BYTES);
    const int ok = parsed != NULL && MLKEM768_parse_public_key(parsed, &encoded) && CBS_len(&encoded) == 0;
    if (ok)
    {
        MLKEM768_encap(ciphertext, secret, parsed);
    }
    free(parsed);
    return ok;
}

static int board_kem_decapsulate(void* kem, const uint8_t* ciphertext, uint8_t* secret)
{
    return MLKEM768_decap(secret, ciphertext, CRD_BOARD_KEM_CIPHERTEXT_BYTES, kem);
}

static void board_kem_free(void* kem)
{
    free(kem);
}
#else
/* The key and a decapsulation context made once from it. */
struct board_kem
{
    EVP_PKEY*     key;
    EVP_PKEY_CTX* decapsulation;
};

static void board_kem_free(void* state)
{
    struct board_kem* kem = state;
    if (kem != NULL)
    {
        EVP_PKEY_CTX_free(kem->decapsulation);
        EVP_PKEY_free(kem->key);
        free(kem);
    }
}

static void* board_kem_new(uint8_t* public_key)
{
    struct board_kem* kem = calloc(1, sizeof(struct board_kem));
    if (kem == NULL)
    {
        return NULL;
    }
#if defined(OPENSSL_IS_AWSLC)
    EVP_PKEY_CTX* generation = EVP_PKEY_CTX_new_id(EVP_PKEY_KEM, NULL);
    if (generation == NULL || !EVP_PKEY_CTX_kem_set_params(generation, NID_MLKEM768) ||
        !EVP_PKEY_keygen_init(generation) || !EVP_PKEY_keygen(generation, &kem->key))
    {
        kem->key = NULL;
    }
    EVP_PKEY_CTX_free(generation);
    kem->decapsulation = kem->key != NULL ? EVP_PKEY_CTX_new(kem->key, NULL) : NULL;
    const int ready    = kem->decapsulation != NULL;
#else
    kem->key           = EVP_PKEY_Q_keygen(NULL, NULL, "ML-KEM-768");
    kem->decapsulation = kem->key != NULL ? EVP_PKEY_CTX_new_from_pkey(NULL, kem->key, NULL) : NULL;
    const int ready    = kem->decapsulation != NULL && EVP_PKEY_decapsulate_init(kem->decapsulation, NULL) > 0;
#endif
    size_t length = CRD_BOARD_KEM_PUBLIC_BYTES;
    if (!ready || !EVP_PKEY_get_raw_public_key(kem->key, public_key, &length) || length != CRD_BOARD_KEM_PUBLIC_BYTES)
    {
        board_kem_free(kem);
        return NULL;
    }
    return kem;
}

static int board_kem_encapsulate(const uint8_t* public_key, uint8_t* ciphertext, uint8_t* secret)
{
#if defined(OPENSSL_IS_AWSLC)
    EVP_PKEY*     key     = EVP_PKEY_kem_new_raw_public_key(NID_MLKEM768, public_key, CRD_BOARD_KEM_PUBLIC_BYTES);
    EVP_PKEY_CTX* context = key != NULL ? EVP_PKEY_CTX_new(key, NULL) : NULL;
    const int     ready   = context != NULL;
#else
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, public_key, CRD_BOARD_KEM_PUBLIC_BYTES);
    EVP_PKEY_CTX* context = key != NULL ? EVP_PKEY_CTX_new_from_pkey(NULL, key, NULL) : NULL;
    const int     ready   = context != NULL && EVP_PKEY_encapsulate_init(context, NULL) > 0;
#endif
    size_t    ciphertext_length = CRD_BOARD_KEM_CIPHERTEXT_BYTES;
    size_t    secret_length     = CRD_BOARD_SECRET_BYTES;
    const int ok = ready && EVP_PKEY_encapsulate(context, ciphertext, &ciphertext_length, secret, &secret_length) > 0 &&
                   ciphertext_length == CRD_BOARD_KEM_CIPHERTEXT_BYTES && secret_length == CRD_BOARD_SECRET_BYTES;
    EVP_PKEY_CTX_free(context);
    EVP_PKEY_free(key);
    return ok;
}

static int board_kem_decapsulate(void* state, const uint8_t* ciphertext, uint8_t* secret)
{
    struct board_kem* kem    = state;
    size_t            length = CRD_BOARD_SECRET_BYTES;
    return EVP_PKEY_decapsulate(kem->decapsulation, secret, &length, ciphertext, CRD_BOARD_KEM_CIPHERTEXT_BYTES) > 0 &&
           length == CRD_BOARD_SECRET_BYTES;
}
#endif

/* ---- Signatures ---- */

struct board_signer
{
    int       algorithm;
    EVP_PKEY* key; /* Ed25519 and ECDSA everywhere, ML-DSA-65 on AWS-LC and OpenSSL */
#if defined(BOARD_BORINGSSL)
    struct MLDSA65_private_key* ml_dsa;
#endif
};

static void board_signer_free(void* state)
{
    struct board_signer* signer = state;
    if (signer != NULL)
    {
        EVP_PKEY_free(signer->key);
#if defined(BOARD_BORINGSSL)
        free(signer->ml_dsa);
#endif
        free(signer);
    }
}

/* The uncompressed SEC 1 point of a P-256 key. */
static int board_p256_public(const EVP_PKEY* key, uint8_t* out, size_t* length)
{
#if defined(BOARD_FAMILY)
    const EC_KEY* ec = EVP_PKEY_get0_EC_KEY(key);
    *length = ec != NULL ? EC_POINT_point2oct(EC_KEY_get0_group(ec), EC_KEY_get0_public_key(ec),
                                              POINT_CONVERSION_UNCOMPRESSED, out, 65, NULL)
                         : 0;
    return *length == 65;
#else
    return EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, out, 65, length) && *length == 65;
#endif
}

static EVP_PKEY* board_generate(int algorithm)
{
#if defined(BOARD_FAMILY)
    static const int types[] = {EVP_PKEY_ED25519, EVP_PKEY_EC, BOARD_PKEY_ML_DSA};
    EVP_PKEY_CTX*    context = EVP_PKEY_CTX_new_id(types[algorithm], NULL);
    EVP_PKEY*        key     = NULL;
    int              ready   = context != NULL;
#if defined(OPENSSL_IS_AWSLC)
    if (ready && algorithm == CRD_BOARD_ML_DSA_65)
    {
        ready = EVP_PKEY_CTX_pqdsa_set_params(context, NID_MLDSA65);
    }
#endif
    ready = ready && EVP_PKEY_keygen_init(context);
    if (ready && algorithm == CRD_BOARD_ECDSA_P256_SHA256)
    {
        ready = EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context, NID_X9_62_prime256v1);
    }
    if (!ready || !EVP_PKEY_keygen(context, &key))
    {
        key = NULL;
    }
    EVP_PKEY_CTX_free(context);
    return key;
#else
    switch (algorithm)
    {
    case CRD_BOARD_ED25519:
        return EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
    case CRD_BOARD_ECDSA_P256_SHA256:
        return EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
    default:
        return EVP_PKEY_Q_keygen(NULL, NULL, "ML-DSA-65");
    }
#endif
}

static void* board_signer_new(int algorithm, uint8_t* public_key, size_t* public_bytes)
{
    struct board_signer* signer = algorithm >= 0 && algorithm <= CRD_BOARD_ML_DSA_65
                                      ? calloc(1, sizeof(struct board_signer))
                                      : NULL;
    if (signer == NULL)
    {
        return NULL;
    }
    signer->algorithm = algorithm;
    int ok            = 0;
#if defined(BOARD_BORINGSSL)
    if (algorithm == CRD_BOARD_ML_DSA_65)
    {
        uint8_t seed[MLDSA_SEED_BYTES];
        signer->ml_dsa = malloc(sizeof(struct MLDSA65_private_key));
        ok = signer->ml_dsa != NULL && MLDSA65_generate_key(public_key, seed, signer->ml_dsa);
        *public_bytes = MLDSA65_PUBLIC_KEY_BYTES;
        OPENSSL_cleanse(seed, sizeof(seed));
    }
    else
#endif
    {
        signer->key = board_generate(algorithm);
        *public_bytes = CRD_BOARD_MAX_PUBLIC_BYTES;
        if (signer->key != NULL)
        {
            ok = algorithm == CRD_BOARD_ECDSA_P256_SHA256
                     ? board_p256_public(signer->key, public_key, public_bytes)
                     : EVP_PKEY_get_raw_public_key(signer->key, public_key, public_bytes);
        }
    }
    if (!ok)
    {
        board_signer_free(signer);
        return NULL;
    }
    return signer;
}

static int board_sign(void* state, const uint8_t* message, size_t size, uint8_t* signature, size_t* signature_bytes)
{
    struct board_signer* signer = state;
#if defined(BOARD_BORINGSSL)
    if (signer->algorithm == CRD_BOARD_ML_DSA_65)
    {
        *signature_bytes = MLDSA65_SIGNATURE_BYTES;
        return MLDSA65_sign(signature, signer->ml_dsa, message, size, NULL, 0);
    }
#endif
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    *signature_bytes    = CRD_BOARD_MAX_SIGNATURE_BYTES;
#if defined(BOARD_FAMILY)
    const EVP_MD* digest = signer->algorithm == CRD_BOARD_ECDSA_P256_SHA256 ? EVP_sha256() : NULL;
    const int     ready  = context != NULL && EVP_DigestSignInit(context, NULL, digest, NULL, signer->key);
#else
    const char* digest = signer->algorithm == CRD_BOARD_ECDSA_P256_SHA256 ? "SHA256" : NULL;
    const int   ready  = context != NULL && EVP_DigestSignInit_ex(context, NULL, digest, NULL, NULL, signer->key, NULL);
#endif
    const int ok = ready && EVP_DigestSign(context, signature, signature_bytes, message, size) > 0;
    EVP_MD_CTX_free(context);
    return ok;
}

/* A public key parsed from its encoding, as a verifier receives it. */
static EVP_PKEY* board_public_key(int algorithm, const uint8_t* public_key, size_t public_bytes)
{
    if (algorithm == CRD_BOARD_ED25519)
    {
        return BOARD_ED25519_PUBLIC(public_key, public_bytes);
    }
#if defined(BOARD_FAMILY)
    if (algorithm == CRD_BOARD_ML_DSA_65)
    {
#if defined(OPENSSL_IS_AWSLC)
        return EVP_PKEY_pqdsa_new_raw_public_key(NID_MLDSA65, public_key, public_bytes);
#else
        return NULL;
#endif
    }
    EC_KEY*         ec    = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    const EC_GROUP* group = ec != NULL ? EC_KEY_get0_group(ec) : NULL;
    EC_POINT*       point = group != NULL ? EC_POINT_new(group) : NULL;
    EVP_PKEY*       key   = EVP_PKEY_new();
    if (point == NULL || key == NULL || !EC_POINT_oct2point(group, point, public_key, public_bytes, NULL) ||
        !EC_KEY_set_public_key(ec, point) || !EVP_PKEY_assign_EC_KEY(key, ec))
    {
        EVP_PKEY_free(key);
        key = NULL;
        EC_KEY_free(ec);
    }
    EC_POINT_free(point);
    return key;
#else
    if (algorithm == CRD_BOARD_ML_DSA_65)
    {
        return EVP_PKEY_new_raw_public_key_ex(NULL, "ML-DSA-65", NULL, public_key, public_bytes);
    }
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, "P-256", 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, (void*)public_key, public_bytes),
        OSSL_PARAM_construct_end(),
    };
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    EVP_PKEY*     key     = NULL;
    if (context == NULL || EVP_PKEY_fromdata_init(context) <= 0 ||
        EVP_PKEY_fromdata(context, &key, EVP_PKEY_PUBLIC_KEY, params) <= 0)
    {
        key = NULL;
    }
    EVP_PKEY_CTX_free(context);
    return key;
#endif
}

static int board_verify(int algorithm, const uint8_t* public_key, size_t public_bytes, const uint8_t* message,
                        size_t size, const uint8_t* signature, size_t signature_bytes)
{
#if defined(BOARD_BORINGSSL)
    if (algorithm == CRD_BOARD_ML_DSA_65)
    {
        struct MLDSA65_public_key* parsed = malloc(sizeof(struct MLDSA65_public_key));
        CBS                        encoded;
        CBS_init(&encoded, public_key, public_bytes);
        const int valid = parsed != NULL && MLDSA65_parse_public_key(parsed, &encoded) && CBS_len(&encoded) == 0 &&
                          MLDSA65_verify(parsed, signature, signature_bytes, message, size, NULL, 0);
        free(parsed);
        return valid;
    }
#endif
    EVP_PKEY*   key     = board_public_key(algorithm, public_key, public_bytes);
    EVP_MD_CTX* context = key != NULL ? EVP_MD_CTX_new() : NULL;
#if defined(BOARD_FAMILY)
    const EVP_MD* digest = algorithm == CRD_BOARD_ECDSA_P256_SHA256 ? EVP_sha256() : NULL;
    const int     ready  = context != NULL && EVP_DigestVerifyInit(context, NULL, digest, NULL, key);
#else
    const char* digest = algorithm == CRD_BOARD_ECDSA_P256_SHA256 ? "SHA256" : NULL;
    const int   ready  = context != NULL && EVP_DigestVerifyInit_ex(context, NULL, digest, NULL, NULL, key, NULL);
#endif
    const int valid = ready && EVP_DigestVerify(context, signature, signature_bytes, message, size) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return valid;
}

/* ---- TLS 1.3 ---- */

struct board_tls
{
    int          group;
    SSL_CTX*     server;
    SSL_CTX*     client;
    uint8_t      pin[32];      /* the server's Ed25519 key, which the client pins */
    SSL_SESSION* ticket;       /* the client's newest ticket */
    unsigned     tickets_kept; /* how many the client has kept */
    SSL*         writer;       /* the established pair tls_record runs on: its client */
    SSL*         reader;       /* and its server */
    uint8_t      flight[BOARD_IO_BYTES]; /* the transport's hop between the ends */
};

static int board_tls_pinned(const EVP_PKEY* key, const struct board_tls* tls)
{
    uint8_t raw[32];
    size_t  length = sizeof(raw);
    return key != NULL && EVP_PKEY_id(key) == EVP_PKEY_ED25519 && EVP_PKEY_get_raw_public_key(key, raw, &length) &&
           length == sizeof(raw) && memcmp(raw, tls->pin, sizeof(raw)) == 0;
}

#if defined(BOARD_FAMILY)
/* The pin check, in place of path validation, as crd-crypto's own verify callback makes it. */
static enum ssl_verify_result_t board_tls_verify(SSL* ssl, uint8_t* alert)
{
    const struct board_tls* tls    = SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl));
    X509*                   leaf   = SSL_get_peer_certificate(ssl); /* parsed once, with the Certificate message */
    const int               pinned = leaf != NULL && board_tls_pinned(X509_get0_pubkey(leaf), tls);
    X509_free(leaf);
    if (!pinned)
    {
        *alert = SSL_AD_BAD_CERTIFICATE;
        return ssl_verify_invalid;
    }
    return ssl_verify_ok;
}
#else
static int board_tls_verify(X509_STORE_CTX* store, void* argument)
{
    return board_tls_pinned(X509_get0_pubkey(X509_STORE_CTX_get0_cert(store)), argument);
}
#endif

static int board_tls_keep(SSL* ssl, SSL_SESSION* session)
{
    struct board_tls* tls = SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl));
    SSL_SESSION_free(tls->ticket);
    tls->ticket = session;
    ++tls->tickets_kept;
    return 1; /* the session is the callback's now */
}

/* A self-signed certificate over a fixed Ed25519 key. */
static int board_tls_credential(struct board_tls* tls)
{
    uint8_t seed[32];
    for (size_t i = 0; i < sizeof(seed); ++i)
    {
        seed[i] = (uint8_t)(i + 1U);
    }
    EVP_PKEY*  key    = BOARD_ED25519_PRIVATE(seed, sizeof(seed));
    X509*      cert   = X509_new();
    X509_NAME* name   = X509_NAME_new();
    size_t     length = sizeof(tls->pin);
    const int  ok =
        key != NULL && cert != NULL && name != NULL && X509_set_version(cert, X509_VERSION_3) &&
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) && X509_gmtime_adj(X509_getm_notBefore(cert), -3600) != NULL &&
        X509_gmtime_adj(X509_getm_notAfter(cert), 86400) != NULL &&
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)CRD_BOARD_SERVER_NAME, -1, -1, 0) &&
        X509_set_subject_name(cert, name) && X509_set_issuer_name(cert, name) && X509_set_pubkey(cert, key) &&
        X509_sign(cert, key, NULL) > 0 && SSL_CTX_use_certificate(tls->server, cert) &&
        SSL_CTX_use_PrivateKey(tls->server, key) && EVP_PKEY_get_raw_public_key(key, tls->pin, &length) &&
        length == sizeof(tls->pin);
    X509_NAME_free(name);
    X509_free(cert);
    EVP_PKEY_free(key);
    return ok;
}

static SSL_CTX* board_tls_context(struct board_tls* tls, int server)
{
    static const uint8_t binding[32] = {0};
    SSL_CTX*             context     = SSL_CTX_new(TLS_method());
    if (context == NULL || !SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) ||
        !SSL_CTX_set_max_proto_version(context, TLS1_3_VERSION) ||
        !SSL_CTX_set1_groups_list(context, tls->group == CRD_BOARD_X25519 ? "X25519" : "X25519MLKEM768") ||
        !SSL_CTX_set_app_data(context, tls))
    {
        SSL_CTX_free(context);
        return NULL;
    }
#if defined(BOARD_FAMILY)
    SSL_CTX_set_grease_enabled(context, 1);
#endif
    int ok = 1;
    if (server)
    {
        SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
        ok = SSL_CTX_set_num_tickets(context, 1) && SSL_CTX_set_session_id_context(context, binding, sizeof(binding));
    }
    else
    {
        SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_sess_set_new_cb(context, board_tls_keep);
        /* The client accepts the server's Ed25519 key alone, as crd-crypto's pinned client does. */
#if defined(BOARD_FAMILY)
        static const uint16_t accepted[] = {SSL_SIGN_ED25519};
        SSL_CTX_set_custom_verify(context, SSL_VERIFY_PEER, board_tls_verify);
        ok = SSL_CTX_set_verify_algorithm_prefs(context, accepted, 1);
#else
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_cert_verify_callback(context, board_tls_verify, tls);
        ok = SSL_CTX_set1_sigalgs_list(context, "ed25519") == 1;
#endif
#if !defined(BOARD_BORINGSSL)
        ok = ok && SSL_CTX_set_ciphersuites(context, "TLS_AES_128_GCM_SHA256");
#endif
    }
    if (!ok)
    {
        SSL_CTX_free(context);
        return NULL;
    }
    return context;
}

/* Whether a connection agreed what the board measures: the group, AES-128-GCM and, when asked, a resumption. */
static int board_tls_agreed(const struct board_tls* tls, SSL* ssl, int resumed)
{
    const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl);
#if defined(BOARD_FAMILY)
    const int group_ok = SSL_get_group_id(ssl) == (tls->group == CRD_BOARD_X25519 ? SSL_GROUP_X25519
                                                                                  : SSL_GROUP_X25519_MLKEM768);
#else
    const char* name     = SSL_get0_group_name(ssl);
    const int   group_ok = name != NULL &&
                         strcasecmp(name, tls->group == CRD_BOARD_X25519 ? "x25519" : "X25519MLKEM768") == 0;
#endif
    return group_ok && cipher != NULL && SSL_CIPHER_get_protocol_id(cipher) == 0x1301 &&
           (SSL_session_reused(ssl) != 0) == (resumed != 0);
}

/* One connection to its end: the handshake both ways, then the client reads the ticket its server sent. */
/* An end of a connection over two memory BIOs: what it writes waits in one until the caller moves it, as a transport
 * would, into the other end's. */
static SSL* board_tls_end(SSL_CTX* context)
{
    SSL* ssl      = SSL_new(context);
    BIO* incoming = BIO_new(BIO_s_mem());
    BIO* outgoing = BIO_new(BIO_s_mem());
    if (ssl == NULL || incoming == NULL || outgoing == NULL)
    {
        BIO_free(incoming);
        BIO_free(outgoing);
        SSL_free(ssl);
        return NULL;
    }
    SSL_set_bio(ssl, incoming, outgoing);
    return ssl;
}

/* Moves what `from` wrote to `to` through the caller's buffer, the hop crd-crypto's take() and receive() make. */
static int board_tls_move(struct board_tls* tls, SSL* from, SSL* to)
{
    const size_t pending = BIO_ctrl_pending(SSL_get_wbio(from));
    if (pending == 0)
    {
        return 1;
    }
    if (pending > sizeof(tls->flight))
    {
        return 0;
    }
    const int bytes = (int)pending;
    return BIO_read(SSL_get_wbio(from), tls->flight, bytes) == bytes &&
           BIO_write(SSL_get_rbio(to), tls->flight, bytes) == bytes;
}

static int board_tls_pair(struct board_tls* tls, int resume, SSL** client_out, SSL** server_out)
{
    SSL* client = board_tls_end(tls->client);
    SSL* server = board_tls_end(tls->server);
    int  ok     = client != NULL && server != NULL;
    if (ok)
    {
        SSL_set_connect_state(client);
        SSL_set_accept_state(server);
        ok = SSL_set_tlsext_host_name(client, CRD_BOARD_SERVER_NAME) &&
             (!resume || (tls->ticket != NULL && SSL_set_session(client, tls->ticket)));
    }
    int done = 0;
    for (int round = 0; ok && !done && round < 8; ++round)
    {
        const int at_client = SSL_do_handshake(client);
        ok                  = (at_client == 1 || SSL_get_error(client, at_client) == SSL_ERROR_WANT_READ) &&
             board_tls_move(tls, client, server);
        const int at_server = ok ? SSL_do_handshake(server) : -1;
        ok   = ok && (at_server == 1 || SSL_get_error(server, at_server) == SSL_ERROR_WANT_READ) &&
             board_tls_move(tls, server, client);
        done = at_client == 1 && at_server == 1;
    }
    uint8_t byte = 0;
#if defined(BOARD_FAMILY)
    /* A BoringSSL-family server holds its ticket back until it next writes; a zero-byte write sends it, as
     * crd-crypto's sessions make it (OpenSSL's server sent it with its handshake). The write reads as an error. */
    if (ok && done)
    {
        (void)SSL_write(server, &byte, 0);
        ok = board_tls_move(tls, server, client);
    }
#endif
    const unsigned kept = tls->tickets_kept;
    const int      read = ok && done ? SSL_read(client, &byte, 1) : 1;
    ok = ok && done && read <= 0 && SSL_get_error(client, read) == SSL_ERROR_WANT_READ &&
         tls->tickets_kept == kept + 1U && board_tls_agreed(tls, client, resume);
    if (ok && client_out != NULL)
    {
        *client_out = client;
        *server_out = server;
        return 1;
    }
#if !defined(BOARD_FAMILY)
    /* OpenSSL marks the session of a connection freed before its shutdown not resumable, the client's new ticket
     * included: the connection ends as quietly as the other arms' (no close_notify is sent). */
    SSL_set_shutdown(client, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
#endif
    SSL_free(client);
    SSL_free(server);
    return ok;
}

static void board_tls_free(void* state)
{
    struct board_tls* tls = state;
    if (tls != NULL)
    {
        SSL_free(tls->writer);
        SSL_free(tls->reader);
        SSL_SESSION_free(tls->ticket);
        SSL_CTX_free(tls->client);
        SSL_CTX_free(tls->server);
        free(tls);
    }
}

static void* board_tls_new(int group)
{
    struct board_tls* tls = calloc(1, sizeof(struct board_tls));
    if (tls == NULL)
    {
        return NULL;
    }
    tls->group  = group;
    tls->server = board_tls_context(tls, 1);
    tls->client = board_tls_context(tls, 0);
    if (tls->server == NULL || tls->client == NULL || !board_tls_credential(tls) ||
        !board_tls_pair(tls, 0, &tls->writer, &tls->reader))
    {
        board_tls_free(tls);
        return NULL;
    }
    return tls;
}

static int board_tls_connect(void* tls, int resume)
{
    return board_tls_pair(tls, resume, NULL, NULL);
}

static int board_tls_record(void* state, const uint8_t* data, size_t size, uint8_t* out)
{
    struct board_tls* tls = state;
    if (SSL_write(tls->writer, data, (int)size) != (int)size || !board_tls_move(tls, tls->writer, tls->reader))
    {
        return 0;
    }
    for (size_t got = 0; got < size;)
    {
        const int read = SSL_read(tls->reader, out + got, (int)(size - got));
        if (read <= 0)
        {
            return 0;
        }
        got += (size_t)read;
    }
    return 1;
}

const struct crd_board_peer CRD_BOARD_PEER = {
    CRD_BOARD_PEER_NAME,  CRD_BOARD_PEER_VERSION, board_aead_new,   board_aead_seal,       board_aead_open,
    board_aead_free,      board_x25519,           board_kem_new,    board_kem_encapsulate, board_kem_decapsulate,
    board_kem_free,       board_signer_new,       board_sign,       board_verify,          board_signer_free,
    board_tls_new,        board_tls_connect,      board_tls_record, board_tls_free,
};
