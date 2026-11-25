// crd-mldsa-threads-control -- the Cerid-free control of the host check's provider legs (NET.2; the register entry
// win-asan-ml-dsa-threaded-signing-refused of the nonreproduced register, tests/support/diag, and
// docs/design/test-instruments.md#host-separation-diag10a). No Cerid code: it links only the pinned AWS-LC's libcrypto,
// built in the same tree with the same flags as crd-crypto's, and makes the case's provider calls in the case's order
// (raw_signing.hpp). crd-crypto installs nothing into AWS-LC (no allocator, randomness or thread hook, no
// initialization call). Arguments: "gate" holds the four threads at a start gate until all exist, so their first
// signings overlap; "touch" touches each thread's stack first (threads_harness.hpp); "storage" keeps each signing's
// state where SigningKey::sign keeps it; "copy" makes SigningKey::sign's and ProviderVerifier::verify's calls exactly,
// ErrorScope's queue peeks included (raw_signing.hpp). Output as threads_harness.hpp's: "result: clean" (0),
// "result: FAULT" (1) or "result: SETUP ..." (2).
#include "raw_signing.hpp"

int main(int argc, char** argv)
{
    const char* const names[] = {"gate", "storage", "copy", "touch"};
    bool flags[] = {false, false, false, false};
    if (!mldsa_threads::parse_flags(argc, argv, names, flags, 4U))
    {
        return 2;
    }
    mldsa_threads::raw::Options options;
    options.gate = flags[0];
    options.storage = flags[1];
    options.copy = flags[2];
    options.touch = flags[3];
    return mldsa_threads::raw::run(options);
}
