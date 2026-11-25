// The raw AWS-LC arm of the host check's provider legs (NET.2;
// docs/design/test-instruments.md#host-separation-diag10a): the provider calls of crd-crypto-tests' "Provider: one
// verifier and one key serve several threads", made directly on the pinned AWS-LC in the case's order, run by
// crd-mldsa-threads-control (a program linking only AWS-LC) and by crd-mldsa-threads-cerid's "raw" mode (the same calls
// inside a program linking crd-crypto). The main thread draws a 32-byte seed (RAND_bytes, as random_bytes), makes one
// ML-DSA-65 private key from it (EVP_PKEY_pqdsa_new_raw_private_key, as SigningKey::from_seed) and reads its public
// key; each thread signs with that key, a new context per signing (EVP_DigestSignInit with no digest, then
// EVP_DigestSign, as SigningKey::sign), and verifies through a public key made for the call (as
// ProviderVerifier::verify). A refusal names the refused call and the provider's first queued error. "storage" keeps
// each signing's state where SigningKey::sign keeps it (the digest context on the stack, bssl::ScopedEVP_MD_CTX, and
// the signature in a heap buffer).
#pragma once

#include "threads_harness.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <openssl/digest.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/nid.h>
#include <openssl/rand.h>

namespace mldsa_threads::raw
{
using mldsa_threads::kMessage;

constexpr std::size_t kSeed = 32U;
constexpr std::size_t kPublicKey = 1952U;
constexpr std::size_t kSignature = 3309U;

// The two calls of SigningKey::sign on `context`: the refused call's name, or nullptr.
inline const char* sign_with(EVP_MD_CTX* context, EVP_PKEY* key, std::uint8_t* out, std::size_t& length)
{
    if (EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) != 1)
    {
        return "EVP_DigestSignInit";
    }
    if (EVP_DigestSign(context, out, &length, kMessage, sizeof(kMessage)) != 1 || length > kSignature)
    {
        return "EVP_DigestSign";
    }
    return nullptr;
}

// One thread's signing state: plain (a heap context, the signature on the thread's stack) or the case's storage.
struct Session
{
    EVP_PKEY* key = nullptr;
    const std::uint8_t* public_key = nullptr;
    bool storage = false;
    std::uint8_t stack_signature[kSignature] = {};
    std::unique_ptr<std::uint8_t[]> heap_signature;
    std::size_t length = 0U;

    std::uint8_t* signature() { return storage ? heap_signature.get() : stack_signature; }

    bool sign(char* reason, std::size_t size)
    {
        length = kSignature;
        const char* refused = nullptr;
        if (storage)
        {
            bssl::ScopedEVP_MD_CTX context;
            refused = sign_with(context.get(), key, signature(), length);
        }
        else
        {
            EVP_MD_CTX* context = EVP_MD_CTX_new();
            refused = context == nullptr ? "EVP_MD_CTX_new" : sign_with(context, key, signature(), length);
            EVP_MD_CTX_free(context);
        }
        if (refused == nullptr)
        {
            return true;
        }
        const std::uint32_t error = ERR_get_error();
        char text[128];
        ERR_error_string_n(error, text, sizeof(text));
        std::snprintf(reason, size, "%s refused it (provider error %s)", refused, error == 0U ? "none queued" : text);
        ERR_clear_error();
        return false;
    }

    // As ProviderVerifier::verify: through a public key made for the call.
    bool verify()
    {
        EVP_PKEY* parsed = EVP_PKEY_pqdsa_new_raw_public_key(NID_MLDSA65, public_key, kPublicKey);
        if (parsed == nullptr)
        {
            ERR_clear_error();
            return false;
        }
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        const bool valid = context != nullptr &&
                           EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, parsed) == 1 &&
                           EVP_DigestVerify(context, signature(), length, kMessage, sizeof(kMessage)) == 1;
        EVP_MD_CTX_free(context);
        EVP_PKEY_free(parsed);
        if (!valid)
        {
            ERR_clear_error();
        }
        return valid;
    }
};

// Makes the key, runs the four threads on it and reports them: 0 clean, 1 a fault, 2 a setup failure.
inline int run(bool gate, bool storage)
{
    std::uint8_t seed[kSeed];
    std::uint8_t public_key[kPublicKey];
    std::size_t public_length = sizeof(public_key);
    if (RAND_bytes(seed, sizeof(seed)) != 1)
    {
        std::printf("result: SETUP RAND_bytes refused\n");
        return 2;
    }
    EVP_PKEY* key = EVP_PKEY_pqdsa_new_raw_private_key(NID_MLDSA65, seed, sizeof(seed));
    OPENSSL_cleanse(seed, sizeof(seed));
    if (key == nullptr || EVP_PKEY_get_raw_public_key(key, public_key, &public_length) != 1 ||
        public_length != kPublicKey)
    {
        std::printf("result: SETUP the ML-DSA-65 key was refused\n");
        EVP_PKEY_free(key);
        return 2;
    }
    const int result = mldsa_threads::run(gate,
                                          [&]
                                          {
                                              Session session;
                                              session.key = key;
                                              session.public_key = public_key;
                                              session.storage = storage;
                                              if (storage)
                                              {
                                                  session.heap_signature = std::make_unique<std::uint8_t[]>(kSignature);
                                              }
                                              return session;
                                          });
    EVP_PKEY_free(key);
    return result;
}
} // namespace mldsa_threads::raw
