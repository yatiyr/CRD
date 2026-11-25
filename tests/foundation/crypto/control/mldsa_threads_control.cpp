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
// Output: one line per thread ("thread T: clean, first signing M ms" or "thread T: FAULT ..."), then "control: clean"
// (exit 0) or "control: FAULT" (exit 1); a setup failure prints "control: SETUP ..." (exit 2).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/nid.h>
#include <openssl/rand.h>
#include <thread>

namespace
{
constexpr std::size_t kSeed = 32U;
constexpr std::size_t kPublicKey = 1952U;
constexpr std::size_t kSignature = 3309U;
constexpr unsigned kThreads = 4U;
constexpr unsigned kSignings = 8U;
constexpr std::uint8_t kMessage[] = {'t', 'h', 'r', 'e', 'a', 'd', 's'};

// Each thread's outcome; printed by the main thread after the joins.
struct Outcome
{
    unsigned iteration = kSignings;     // the faulting iteration, kSignings when clean
    const char* refused_call = nullptr; // the signing call that returned 0
    std::uint32_t error = 0U;           // the first record of the provider's error queue after the refusal
    long long refused_ms = 0;           // how long the refused signing took
    bool resigned = false;              // a second signing at once succeeded
    bool invalid = false;               // a signature did not verify
    bool invalid_again = false;         // and the second verification at once did not either
    long long first_ms = 0;             // how long the thread's first signing took
};

long long elapsed_ms(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

// One signing as SigningKey::sign makes it: the refused call's name, or nullptr.
const char* sign_once(EVP_PKEY* key, std::uint8_t* out, std::size_t& length)
{
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const char* refused = nullptr;
    length = kSignature;
    if (context == nullptr)
    {
        refused = "EVP_MD_CTX_new";
    }
    else if (EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) != 1)
    {
        refused = "EVP_DigestSignInit";
    }
    else if (EVP_DigestSign(context, out, &length, kMessage, sizeof(kMessage)) != 1 || length > kSignature)
    {
        refused = "EVP_DigestSign";
    }
    EVP_MD_CTX_free(context);
    return refused;
}

// One verification as ProviderVerifier::verify makes it, through a public key made for the call.
bool verify_once(const std::uint8_t* public_key, const std::uint8_t* signature, std::size_t length)
{
    EVP_PKEY* key = EVP_PKEY_pqdsa_new_raw_public_key(NID_MLDSA65, public_key, kPublicKey);
    if (key == nullptr)
    {
        ERR_clear_error();
        return false;
    }
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const bool valid = context != nullptr && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
                       EVP_DigestVerify(context, signature, length, kMessage, sizeof(kMessage)) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if (!valid)
    {
        ERR_clear_error();
    }
    return valid;
}

void run_thread(EVP_PKEY* key, const std::uint8_t* public_key, Outcome& outcome)
{
    std::uint8_t signature[kSignature];
    for (unsigned i = 0U; i < kSignings && outcome.iteration == kSignings; ++i)
    {
        std::size_t length = 0U;
        const auto start = std::chrono::steady_clock::now();
        const char* refused = sign_once(key, signature, length);
        if (i == 0U)
        {
            outcome.first_ms = elapsed_ms(start);
        }
        if (refused != nullptr)
        {
            outcome.iteration = i;
            outcome.refused_call = refused;
            outcome.refused_ms = elapsed_ms(start);
            outcome.error = ERR_get_error();
            ERR_clear_error();
            outcome.resigned = sign_once(key, signature, length) == nullptr;
            ERR_clear_error();
        }
        else if (!verify_once(public_key, signature, length))
        {
            outcome.iteration = i;
            outcome.invalid = true;
            outcome.invalid_again = !verify_once(public_key, signature, length);
        }
    }
}

int report(const Outcome (&outcomes)[kThreads])
{
    bool faulted = false;
    for (unsigned t = 0U; t < kThreads; ++t)
    {
        const Outcome& outcome = outcomes[t];
        if (outcome.iteration == kSignings)
        {
            std::printf("thread %u: clean, first signing %lld ms\n", t, outcome.first_ms);
            continue;
        }
        faulted = true;
        if (outcome.refused_call != nullptr)
        {
            char text[256];
            ERR_error_string_n(outcome.error, text, sizeof(text));
            std::printf("thread %u: FAULT at iteration %u: %s refused after %lld ms (provider error %s), signing again "
                        "at once %s\n",
                        t, outcome.iteration, outcome.refused_call, outcome.refused_ms,
                        outcome.error == 0U ? "none queued" : text, outcome.resigned ? "succeeded" : "refused");
        }
        else
        {
            std::printf("thread %u: FAULT at iteration %u: the signature did not verify, verifying again at once %s\n",
                        t, outcome.iteration, outcome.invalid_again ? "invalid" : "valid");
        }
    }
    std::printf("control: %s\n", faulted ? "FAULT" : "clean");
    return faulted ? 1 : 0;
}
} // namespace

int main()
{
    std::uint8_t seed[kSeed];
    std::uint8_t public_key[kPublicKey];
    std::size_t public_length = sizeof(public_key);
    if (RAND_bytes(seed, sizeof(seed)) != 1)
    {
        std::printf("control: SETUP RAND_bytes refused\n");
        return 2;
    }
    EVP_PKEY* key = EVP_PKEY_pqdsa_new_raw_private_key(NID_MLDSA65, seed, sizeof(seed));
    OPENSSL_cleanse(seed, sizeof(seed));
    if (key == nullptr || EVP_PKEY_get_raw_public_key(key, public_key, &public_length) != 1 ||
        public_length != kPublicKey)
    {
        std::printf("control: SETUP the ML-DSA-65 key was refused\n");
        EVP_PKEY_free(key);
        return 2;
    }
    Outcome outcomes[kThreads];
    std::thread threads[kThreads];
    for (unsigned t = 0U; t < kThreads; ++t)
    {
        threads[t] = std::thread([key, &public_key, &outcomes, t] { run_thread(key, public_key, outcomes[t]); });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    EVP_PKEY_free(key);
    return report(outcomes);
}
