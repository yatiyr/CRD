// DIAG.3f -- the heap-use-after-free positive (a temporal-safety class). Under a sanitizer build it
// frees a heap block and reads through the dangling pointer; AddressSanitizer reports + aborts
// (SanitizerCaught). With no sanitizer runtime there is nothing to catch it, so it announces
// SANITIZER=none and exits cleanly -- the harness then reports InstrumentAbsent, never a silent pass.
// Core ASan detects this on every ASan lane (win-asan and linux). Mirrors heap_overflow_specimen.
#include "specimen_common.hpp"

#include <cstdlib>

int main()
{
    crd_diag_harden();
    crd_diag_announce();
#if CRD_DIAG_HAS_ASAN
    volatile char* buf = static_cast<char*>(std::malloc(32));
    if (buf == nullptr)
    {
        return 3;
    }
    buf[0] = 'x';
    std::free(const_cast<char*>(buf));
    const char sink = buf[0]; // read after free -- caught by AddressSanitizer
    return sink == 'x' ? 0 : 1;
#else
    return 0;
#endif
}
