// crd-mldsa-threads-cerid -- the Cerid arm of the host check's provider legs (NET.2; the register entry
// win-asan-ml-dsa-threaded-signing-refused of the nonreproduced register, tests/support/diag, and
// docs/design/test-instruments.md#host-separation-diag10a): crd-crypto-tests' "Provider: one verifier and one key
// serve several threads" through crd-crypto's SigningKey and ProviderVerifier exactly as the case makes them (one
// generated ML-DSA-65 key, each thread's signature in a containers::Array), without Catch2 or the test program, so a
// fault here is crd-crypto's use of the provider and a fault only in the case is the test program's. Arguments, which
// cross the key, the signing and the verification between crd-crypto and raw_signing.hpp's AWS-LC calls:
//   "gate"        holds the threads at a start gate until all four exist;
//   "touch"       has each thread touch its stack before its first signing (threads_harness.hpp);
//   "warm"        signs once on the main thread before they start, so no thread's signing is the key's first;
//   "std-buffer"  keeps each thread's signature in a std::unique_ptr array instead of a containers::Array;
//   "seed-key"    makes the SigningKey with from_seed over a RAND_bytes seed wiped by OPENSSL_cleanse, not generate
//                 (random_bytes and secure_zero);
//   "raw-verify"  verifies through raw_signing.hpp's calls instead of ProviderVerifier;
//   "no-verify"   does not verify;
//   "raw"         makes crd-mldsa-threads-control's AWS-LC calls instead of crd-crypto's, inside this program, so a
//                 fault there is this program's process and not crd-crypto's calls; with "copy" the raw signing and
//                 verification are SigningKey::sign's and ProviderVerifier::verify's calls exactly, written in this
//                 program's translation unit, and with "crd-verify" the raw signatures are verified by
//                 ProviderVerifier.
// A crd-crypto signing (SigningKey) with every other step raw failing while "raw copy" passes leaves what differs
// between them: where the code is built and placed, and the thread's stack layout (a raw session holds its 3,309-byte
// stack signature in the object even when it signs into the heap). Output as crd-mldsa-threads-control's
// (threads_harness.hpp).
#include "raw_signing.hpp"
#include "threads_harness.hpp"

#include <crd/containers/array.hpp>
#include <crd/crypto/provider.hpp>

#include <cstdio>
#include <memory>

namespace
{
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;

using Bytes = crd::containers::Array<u8>;

// The arguments' indices in main's names and flags.
constexpr unsigned kGate = 0U;
constexpr unsigned kWarm = 1U;
constexpr unsigned kStdBuffer = 2U;
constexpr unsigned kSeedKey = 3U;
constexpr unsigned kRawVerify = 4U;
constexpr unsigned kNoVerify = 5U;
constexpr unsigned kRaw = 6U;
constexpr unsigned kCopy = 7U;
constexpr unsigned kCrdVerify = 8U;
constexpr unsigned kTouch = 9U;
constexpr unsigned kFlagCount = 10U;

// One thread's signing state, as one thread of the case holds it.
struct Session
{
    const SigningKey* key = nullptr;
    const ProviderVerifier* verifier = nullptr;
    const Bytes* pub = nullptr;
    const bool* flags = nullptr;
    Bytes sig;
    std::unique_ptr<u8[]> std_sig;
    usize written = 0U;

    Span<u8> signature()
    {
        const usize size = signature_sizes(AlgorithmId::MlDsa65).signature;
        return std_sig ? Span<u8>(std_sig.get(), size) : Span<u8>(sig.data(), sig.size());
    }

    bool sign(char* reason, std::size_t size)
    {
        const CryptoError error = key->sign(mldsa_threads::kMessage, signature(), written);
        if (error == CryptoError::None)
        {
            return true;
        }
        std::snprintf(reason, size, "SigningKey::sign refused it (CryptoError %d)", static_cast<int>(error));
        return false;
    }

    bool verify()
    {
        if (flags[kNoVerify])
        {
            return true;
        }
        if (flags[kRawVerify])
        {
            return mldsa_threads::raw::verify(pub->data(), signature().data(), written, false);
        }
        return verifier->verify(AlgorithmId::MlDsa65, *pub, mldsa_threads::kMessage,
                                ConstSpan<u8>(signature().data(), written)) == Verification::Valid;
    }
};

// "raw crd-verify": a raw signature verified by ProviderVerifier.
bool provider_verifies(const std::uint8_t* public_key, const std::uint8_t* signature, std::size_t length)
{
    const ProviderVerifier verifier;
    return verifier.verify(AlgorithmId::MlDsa65, ConstSpan<u8>(public_key, mldsa_threads::raw::kPublicKey),
                           mldsa_threads::kMessage, ConstSpan<u8>(signature, length)) == Verification::Valid;
}

// The flags' crossings that choose nothing: a raw-only flag without "raw", a crd-crypto-only one with it, and two
// verifications at once. Returns the refused flag's complaint, or nullptr.
const char* refused_crossing(const bool* flags)
{
    if (flags[kRaw] && (flags[kWarm] || flags[kStdBuffer] || flags[kSeedKey] || flags[kRawVerify]))
    {
        return "warm, std-buffer, seed-key and raw-verify need crd-crypto's signing (no raw)";
    }
    if (!flags[kRaw] && (flags[kCopy] || flags[kCrdVerify]))
    {
        return "copy and crd-verify need raw";
    }
    if (flags[kNoVerify] && (flags[kRawVerify] || flags[kCrdVerify]))
    {
        return "no-verify cannot be crossed with a verification";
    }
    return nullptr;
}

// The SigningKey: generated, or from a RAND_bytes seed wiped by OPENSSL_cleanse ("seed-key").
CryptoError make_key(SigningKey& key, bool seed_key)
{
    if (!seed_key)
    {
        return key.generate(AlgorithmId::MlDsa65);
    }
    u8 seed[mldsa_threads::raw::kSeed];
    if (RAND_bytes(seed, sizeof(seed)) != 1)
    {
        return CryptoError::ProviderFailure;
    }
    const CryptoError error = key.from_seed(AlgorithmId::MlDsa65, ConstSpan<u8>(seed, sizeof(seed)));
    OPENSSL_cleanse(seed, sizeof(seed));
    return error;
}
} // namespace

int main(int argc, char** argv)
{
    const char* const names[] = {"gate",      "warm", "std-buffer", "seed-key",   "raw-verify",
                                 "no-verify", "raw",  "copy",       "crd-verify", "touch"};
    bool flags[kFlagCount] = {};
    if (!mldsa_threads::parse_flags(argc, argv, names, flags, kFlagCount))
    {
        return 2;
    }
    if (const char* complaint = refused_crossing(flags); complaint != nullptr)
    {
        std::printf("result: SETUP %s\n", complaint);
        return 2;
    }
    if (flags[kRaw])
    {
        mldsa_threads::raw::Options options;
        options.gate = flags[kGate];
        options.touch = flags[kTouch];
        options.copy = flags[kCopy];
        options.verify = !flags[kNoVerify];
        options.verifier = flags[kCrdVerify] ? &provider_verifies : nullptr;
        return mldsa_threads::raw::run(options);
    }
    const ProviderVerifier verifier;
    SigningKey key;
    Bytes pub;
    pub.resize(signature_sizes(AlgorithmId::MlDsa65).public_key);
    if (make_key(key, flags[kSeedKey]) != CryptoError::None || key.public_key(pub) != CryptoError::None)
    {
        std::printf("result: SETUP the ML-DSA-65 key was refused\n");
        return 2;
    }
    const auto make = [&]
    {
        Session session;
        session.key = &key;
        session.verifier = &verifier;
        session.pub = &pub;
        session.flags = flags;
        if (flags[kStdBuffer])
        {
            session.std_sig = std::make_unique<u8[]>(signature_sizes(AlgorithmId::MlDsa65).signature);
        }
        else
        {
            session.sig.resize(signature_sizes(AlgorithmId::MlDsa65).signature);
        }
        return session;
    };
    if (flags[kWarm])
    {
        Session warm = make();
        char reason[192];
        if (!warm.sign(reason, sizeof(reason)) || !warm.verify())
        {
            std::printf("result: SETUP the main thread's signing failed\n");
            return 2;
        }
    }
    return mldsa_threads::run(flags[kGate], make, flags[kTouch]);
}
