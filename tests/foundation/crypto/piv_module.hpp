// The PIV test card's control interface (NET.2 decision K14): piv_module.cpp is a PKCS #11 module that presents one
// PIV card the way Yubico's ykcs11 2.7 does (fixed slots by CKA_ID 1 to 25, generation under the security officer's
// login, a certificate written with every key, C_DestroyObject deleting a slot's certificate but never its key), with
// the cryptography handed to it by the test through CrdPivHooks and every refusal a fault bit away. The test resolves
// these functions from the same loaded module the key device opens, so both see one card.
#pragma once

extern "C"
{
    // The card's cryptography, the test's: `generate` makes an EC key of `field` bytes (32 or 48) and writes its
    // uncompressed point (1 + 2 * field bytes), answering the key's number or -1; `sign` signs `digest` with key `key`
    // as r || s (2 * field bytes), answering 0 on success.
    struct CrdPivHooks
    {
        void* context;
        int (*generate)(void* context, unsigned field, unsigned char* point);
        int (*sign)(void* context, int key, const unsigned char* digest, unsigned long digest_bytes,
                    unsigned char* signature, unsigned long signature_bytes);
    };

    // What the card does wrong, one bit each.
    enum CrdPivFault : unsigned short
    {
        CrdPivSoRefused          = 1U << 0U,  // C_Login(CKU_SO): CKR_PIN_INCORRECT
        CrdPivSoFinalTry         = 1U << 1U,  // the token reports CKF_SO_PIN_FINAL_TRY
        CrdPivUserRefused        = 1U << 2U,  // C_Login(CKU_USER): CKR_PIN_INCORRECT
        CrdPivImported           = 1U << 3U,  // a new key reads back CKA_LOCAL false
        CrdPivBadPoint           = 1U << 4U,  // CKA_EC_POINT is no DER OCTET STRING around the point
        CrdPivShortSignature     = 1U << 5U,  // C_Sign answers one byte short of r || s
        CrdPivGenerationFails    = 1U << 6U,  // C_GenerateKeyPair: CKR_DEVICE_ERROR
        CrdPivNoEcGeneration     = 1U << 7U,  // no CKM_EC_KEY_PAIR_GEN
        CrdPivAlwaysAuthenticate = 1U << 8U,  // keys have CKA_ALWAYS_AUTHENTICATE (PIN policy "always")
        CrdPivReadOnlyExists     = 1U << 9U,  // C_Login(CKU_SO): CKR_SESSION_READ_ONLY_EXISTS
        CrdPivRemoved            = 1U << 10U, // no token present
    };

    // One slot (CKA_ID 1 to 25) as the card holds it.
    struct CrdPivSlot
    {
        int           key;         // the test's key number, -1 for none
        int           certificate; // whether the slot's certificate is present
        unsigned      field;       // 32 or 48 for a key
        unsigned      generations; // keys made in the slot so far
        unsigned char point[97];
    };

    // The logins the card was asked for, accepted or not.
    struct CrdPivCounts
    {
        unsigned user;
        unsigned so;
        unsigned context;
    };

    // Every slot empty, no session or login, these hooks and no fault.
    using CrdPivReset = void (*)(const CrdPivHooks* hooks);
    using CrdPivFaults = void (*)(unsigned faults);
    // A slot holding `key` (-1 for none) and, if `certificate`, its certificate, as another application left it.
    using CrdPivOccupy = void (*)(unsigned id, int key, int certificate);
    using CrdPivRead   = void (*)(unsigned id, CrdPivSlot* out);
    using CrdPivLogins = void (*)(CrdPivCounts* out);
}

// The test values the card is made with: its token label, the user PIN and the security officer's secret (ykcs11's
// default management key in hexadecimal).
inline constexpr char kCrdPivLabel[] = "cerid piv test";
inline constexpr char kCrdPivPin[]   = "123456";
inline constexpr char kCrdPivSo[]    = "010203040506070801020304050607080102030405060708";
