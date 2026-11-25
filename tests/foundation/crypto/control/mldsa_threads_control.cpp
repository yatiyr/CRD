// crd-mldsa-threads-control -- the Cerid-free control of the host check's provider leg (NET.2; the register entry
// win-asan-ml-dsa-threaded-signing-refused of the nonreproduced register, tests/support/diag, and
// docs/design/test-instruments.md#host-separation-diag10a). No Cerid code: it links only the pinned AWS-LC's libcrypto,
// built in the same tree with the same flags as crd-crypto's, and makes the provider calls of crd-crypto-tests'
// "Provider: one verifier and one key serve several threads" in the same order. The main thread draws a 32-byte seed
// (RAND_bytes, as random_bytes), makes one ML-DSA-65 private key from it (EVP_PKEY_pqdsa_new_raw_private_key, as
// SigningKey::from_seed) and reads its public key (EVP_PKEY_get_raw_public_key). Four threads then sign seven bytes
// up to eight times each with that one key, a new context per signing (EVP_DigestSignInit with no digest, then
// EVP_DigestSign, as SigningKey::sign), and verify each signature through a public key made for that call (as
// ProviderVerifier::verify). A thread stops at its first fault; a refused signing is timed, the provider's error queue
// is printed and the thread signs once more at once; an invalid verification is verified once more.
// crd-crypto installs nothing into AWS-LC (no allocator, randomness or thread hook, no initialization call), so what
// the Cerid case runs beyond this program is Catch2's runner, crd-crypto's error-queue scope (it clears the queue
// after a failed call) and its key wrapper.
// Arguments, each one difference from the plain run: "gate" holds the four threads at a start gate until all exist,
// so their first signings overlap; "storage" keeps each signing's state where SigningKey::sign keeps it (the digest
// context on the stack, bssl::ScopedEVP_MD_CTX, and the signature in a heap buffer).
// Output: one line per thread ("thread T: clean, first signing M ms" or "thread T: FAULT ..."), then "result: clean"
// (exit 0) or "result: FAULT" (exit 1); a setup failure or an unknown argument prints "result: SETUP ..." (exit 2).
#include "threads_harness.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <openssl/digest.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/nid.h>
#include <openssl/rand.h>

namespace
{
using mldsa_threads::kMessage;

constexpr std::size_t kSeed = 32U;
constexpr std::size_t kPublicKey = 1952U;
constexpr std::size_t kSignature = 3309U;

// The two calls of SigningKey::sign on `context`: the refused call's name, or nullptr.
const char* sign_with(EVP_MD_CTX* context, EVP_PKEY* key, std::uint8_t* out, std::size_t& length)
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
} // namespace

int main(int argc, char** argv)
{
    const char* const names[] = {"gate", "storage"};
    bool flags[] = {false, false};
    if (!mldsa_threads::parse_flags(argc, argv, names, flags, 2U))
    {
        return 2;
    }
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
    const bool storage = flags[1];
    const int result = mldsa_threads::run(flags[0],
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
