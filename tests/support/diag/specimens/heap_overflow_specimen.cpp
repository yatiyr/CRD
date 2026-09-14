// The sanitizer positive: under a sanitizer build it commits a heap-buffer-overflow
// the runtime catches and aborts (SanitizerCaught). With no sanitizer runtime there
// is nothing to catch a manufactured fault, so it announces SANITIZER=none and exits
// cleanly -- the harness then reports InstrumentAbsent, never a silent pass.
#include "specimen_common.hpp"

#include <cstdlib>

int main()
{
    crd_diag_harden();
    crd_diag_announce();
#if CRD_DIAG_HAS_ASAN
    volatile char* buf = static_cast<char*>(std::malloc(8));
    if (buf == nullptr)
    {
        return 3;
    }
    buf[8] = 'x'; // one past the end -- caught by AddressSanitizer
    const char sink = buf[8];
    std::free(const_cast<char*>(buf));
    return sink == 'x' ? 0 : 1;
#else
    return 0;
#endif
}
