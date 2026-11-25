// The raw AWS-LC arm of the host check's provider legs (NET.2;
// docs/design/test-instruments.md#host-separation-diag10a): the provider calls of crd-crypto-tests' "Provider: one
// verifier and one key serve several threads", made directly on the pinned AWS-LC in the case's order, run by
// crd-mldsa-threads-control (a program linking only AWS-LC) and by crd-mldsa-threads-cerid's "raw" mode (the same calls
// inside a program linking crd-crypto). The main thread draws a 32-byte seed (RAND_bytes, as random_bytes), makes one
// ML-DSA-65 private key from it (EVP_PKEY_pqdsa_new_raw_private_key, as SigningKey::from_seed) and reads its public
// key; each thread signs with that key, a new context per signing (EVP_DigestSignInit with no digest, then
// EVP_DigestSign, as SigningKey::sign), and verifies through a public key made for the call (as
// ProviderVerifier::verify). A refusal names the refused call and the provider's first queued error with its source
// line. Options: "storage" keeps each signing's state where SigningKey::sign keeps it (the digest context on the stack,
// bssl::ScopedEVP_MD_CTX, and the signature in a heap buffer); "copy" is SigningKey::sign's and
// ProviderVerifier::verify's calls exactly, written here: the storage, a stack context for each verification too, and
// ErrorScope's ERR_peek_error, clearing a queued error, after each call that succeeded; "slim" keeps the case's storage
// in a session that holds no signature array of its own, so a thread's stack holds what SigningKey::sign's holds.
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

// A verification of `length` signature bytes of kMessage under the encoded public key.
using Verifier = bool (*)(const std::uint8_t* public_key, const std::uint8_t* signature, std::size_t length);

struct Options
{
    Start start;
    bool storage = false;
    bool copy = false;           // implies storage
    bool slim = false;           // implies storage
    bool verify = true;          // false: each signing is not verified
    Verifier verifier = nullptr; // nullptr: the raw verification below
};

// ErrorScope's destructor after a call that succeeded: a queued error is cleared.
inline void peek_and_clear()
{
    if (ERR_peek_error() != 0U)
    {
        ERR_clear_error();
    }
}

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

// As ProviderVerifier::verify: through a public key made for the call; `scoped` keeps its context on the stack and
// peeks the queue afterwards, as ProviderVerifier::verify does.
inline bool verify(const std::uint8_t* public_key, const std::uint8_t* signature, std::size_t length, bool scoped)
{
    EVP_PKEY* parsed = EVP_PKEY_pqdsa_new_raw_public_key(NID_MLDSA65, public_key, kPublicKey);
    bool valid = false;
    if (parsed != nullptr && scoped)
    {
        bssl::ScopedEVP_MD_CTX context;
        valid = EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, parsed) == 1 &&
                EVP_DigestVerify(context.get(), signature, length, kMessage, sizeof(kMessage)) == 1;
    }
    else if (parsed != nullptr)
    {
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        valid = context != nullptr && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, parsed) == 1 &&
                EVP_DigestVerify(context, signature, length, kMessage, sizeof(kMessage)) == 1;
        EVP_MD_CTX_free(context);
    }
    EVP_PKEY_free(parsed);
    if (scoped)
    {
        peek_and_clear();
    }
    else if (!valid)
    {
        ERR_clear_error();
    }
    return valid;
}

// The refused call and the provider's first queued error with its source line, into `reason`; clears the queue.
inline void describe_refusal(const char* refused, char* reason, std::size_t size)
{
    const char* file = nullptr;
    int line = 0;
    const std::uint32_t error = ERR_get_error_line(&file, &line);
    char text[128];
    if (error == 0U)
    {
        std::snprintf(text, sizeof(text), "none queued");
    }
    else
    {
        ERR_error_string_n(error, text, sizeof(text));
    }
    std::snprintf(reason, size, "%s refused it (provider error %s at %s:%d)", refused, text,
                  file == nullptr ? "?" : file, line);
    ERR_clear_error();
}

// A session's in-object signature bytes, laid out as std::array's (one empty byte when there are none).
template <std::size_t Inline> struct InlineBytes
{
    std::uint8_t bytes[Inline] = {};

    std::uint8_t* data() { return bytes; }
};

template <> struct InlineBytes<0U>
{
    std::uint8_t* data() { return nullptr; }
};

// One thread's signing state: plain (a heap context, the signature in the session's `Inline` bytes on the thread's
// stack) or the case's storage; a slim session (`Inline` 0) has only the case's.
template <std::size_t Inline> struct Session
{
    EVP_PKEY* key = nullptr;
    const std::uint8_t* public_key = nullptr;
    const Options* options = nullptr;
    InlineBytes<Inline> stack_signature = {};
    std::unique_ptr<std::uint8_t[]> heap_signature;
    std::size_t length = 0U;

    std::uint8_t* signature() { return heap_signature ? heap_signature.get() : stack_signature.data(); }

    bool sign(char* reason, std::size_t size)
    {
        length = kSignature;
        const char* refused = nullptr;
        if (heap_signature)
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
            if (options->copy)
            {
                peek_and_clear();
            }
            return true;
        }
        describe_refusal(refused, reason, size);
        return false;
    }

    bool verify()
    {
        if (!options->verify)
        {
            return true;
        }
        if (options->verifier != nullptr)
        {
            return options->verifier(public_key, signature(), length);
        }
        return raw::verify(public_key, signature(), length, options->copy);
    }
};

// Runs the four threads on `key`, each with a session of `Inline` signature bytes.
template <std::size_t Inline> int run_sessions(const Options& options, EVP_PKEY* key, const std::uint8_t* public_key)
{
    return mldsa_threads::run(options.start,
                              [&]
                              {
                                  Session<Inline> session;
                                  session.key = key;
                                  session.public_key = public_key;
                                  session.options = &options;
                                  if (options.storage || options.copy || options.slim)
                                  {
                                      session.heap_signature = std::make_unique<std::uint8_t[]>(kSignature);
                                  }
                                  return session;
                              });
}

// Makes the key, runs the four threads on it and reports them: 0 clean, 1 a fault, 2 a setup failure.
inline int run(const Options& options)
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
    const int result =
        options.slim ? run_sessions<0U>(options, key, public_key) : run_sessions<kSignature>(options, key, public_key);
    EVP_PKEY_free(key);
    return result;
}
} // namespace mldsa_threads::raw
