// The sanitizer positive: under a sanitizer build it commits a heap-buffer-overflow
// the runtime catches and aborts (SanitizerCaught). With no sanitizer runtime there
// is nothing to catch a manufactured fault, so it announces SANITIZER=none and exits
// cleanly -- the harness then reports InstrumentAbsent, never a silent pass.
#define CRD_DIAG_SPECIMEN_ASAN_CLASS // an AddressSanitizer-only error class (see specimen_common.hpp)
#include "specimen_common.hpp"

#include <cstdlib>

#if CRD_DIAG_HAS_ASAN
CRD_DIAG_SITE static char crd_diag_overflow_site(volatile char* buf)
{
    buf[8] = 'x'; // one past the end -- caught by AddressSanitizer
    return buf[8];
}
#endif

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
    const char sink = crd_diag_overflow_site(buf);
    std::free(const_cast<char*>(buf));
    return sink == 'x' ? 0 : 1;
#else
    return 0;
#endif
}
