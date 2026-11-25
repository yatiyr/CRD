// crd-mldsa-threads-control -- the Cerid-free control of the host check's provider legs (NET.2; the register entry
// win-asan-ml-dsa-threaded-signing-refused of the nonreproduced register, tests/support/diag, and
// docs/design/test-instruments.md#host-separation-diag10a). No Cerid code: it links only the pinned AWS-LC's libcrypto,
// built in the same tree with the same flags as crd-crypto's, and makes the case's provider calls in the case's order
// (raw_signing.hpp). crd-crypto installs nothing into AWS-LC (no allocator, randomness or thread hook, no
// initialization call). Arguments: the harness's own ("gate", "touch", "touch-stack", "shift=N"; threads_harness.hpp);
// "storage" keeps each signing's state where SigningKey::sign keeps it; "copy" makes SigningKey::sign's and
// ProviderVerifier::verify's calls exactly, ErrorScope's queue peeks included; "slim" keeps the storage in a session
// without a signature array of its own (raw_signing.hpp). Output as threads_harness.hpp's: "result: clean" (0),
// "result: FAULT" (1) or "result: SETUP ..." (2).
#include "raw_signing.hpp"

int main(int argc, char** argv)
{
    const char* const names[] = {"storage", "copy", "slim"};
    bool flags[] = {false, false, false};
    mldsa_threads::raw::Options options;
    if (!mldsa_threads::parse_flags(argc, argv, names, flags, 3U, options.start))
    {
        return 2;
    }
    options.storage = flags[0];
    options.copy = flags[1];
    options.slim = flags[2];
    return mldsa_threads::raw::run(options);
}
