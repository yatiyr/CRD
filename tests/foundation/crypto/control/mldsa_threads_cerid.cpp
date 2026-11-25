// crd-mldsa-threads-cerid -- the Cerid arm of the host check's provider legs (NET.2; the register entry
// win-asan-ml-dsa-threaded-signing-refused of the nonreproduced register, tests/support/diag, and
// docs/design/test-instruments.md#host-separation-diag10a): crd-crypto-tests' "Provider: one verifier and one key
// serve several threads" through crd-crypto's SigningKey and ProviderVerifier exactly as the case makes them (one
// generated ML-DSA-65 key, each thread's signature in a containers::Array), without Catch2 or the test program, so a
// fault here is crd-crypto's use of the provider and a fault only in the case is the test program's. Arguments:
// "gate" holds the threads at a start gate until all four exist; "warm" signs once on the main thread before they
// start, so no thread's signing is the key's first; "std-buffer" keeps each thread's signature in a std::unique_ptr
// array instead of a containers::Array (the engine's allocator). Output as crd-mldsa-threads-control's
// (threads_harness.hpp).
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

// One thread's signing state, as one thread of the case holds it.
struct Session
{
    const SigningKey* key = nullptr;
    const ProviderVerifier* verifier = nullptr;
    const Bytes* pub = nullptr;
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
        return verifier->verify(AlgorithmId::MlDsa65, *pub, mldsa_threads::kMessage,
                                ConstSpan<u8>(signature().data(), written)) == Verification::Valid;
    }
};
} // namespace

int main(int argc, char** argv)
{
    const char* const names[] = {"gate", "warm", "std-buffer"};
    bool flags[] = {false, false, false};
    if (!mldsa_threads::parse_flags(argc, argv, names, flags, 3U))
    {
        return 2;
    }
    const ProviderVerifier verifier;
    SigningKey key;
    Bytes pub;
    pub.resize(signature_sizes(AlgorithmId::MlDsa65).public_key);
    if (key.generate(AlgorithmId::MlDsa65) != CryptoError::None || key.public_key(pub) != CryptoError::None)
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
        if (flags[2])
        {
            session.std_sig = std::make_unique<u8[]>(signature_sizes(AlgorithmId::MlDsa65).signature);
        }
        else
        {
            session.sig.resize(signature_sizes(AlgorithmId::MlDsa65).signature);
        }
        return session;
    };
    if (flags[1])
    {
        Session warm = make();
        char reason[192];
        if (!warm.sign(reason, sizeof(reason)) || !warm.verify())
        {
            std::printf("result: SETUP the main thread's signing failed\n");
            return 2;
        }
    }
    return mldsa_threads::run(flags[0], make);
}
