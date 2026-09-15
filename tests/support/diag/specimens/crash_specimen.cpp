// The expected-crash positive: announces, then faults hard (a null write). Under
// crd_diag_harden() this dies immediately with an access-violation / SIGSEGV exit,
// which the harness classifies as Crashed.
#include "specimen_common.hpp"

#if CRD_DIAG_HAS_ASAN
// This specimen is the positive control for the harness's CRASHED classification, so it must die with a
// genuine hardware fault (access-violation / 0xC0000005) on every lane. Under AddressSanitizer the default
// is for ASan to trap the null write and abort with a non-crash exit code, which the harness would read as
// Unexpected -- defeating the control on the ASan lane. handle_segv=0 tells the ASan runtime not to install
// its exception handler, so the fault propagates as a real crash the harness classifies as Crashed. Scoped
// to this specimen process only (ASan-only compile); it does not affect the engine or other specimens.
extern "C" const char* __asan_default_options()
{
    return "handle_segv=0";
}
#endif

int main()
{
    crd_diag_harden();
    crd_diag_announce();
    volatile int* p = nullptr;
    *p = 42; // access violation / SIGSEGV
    return 0;
}
