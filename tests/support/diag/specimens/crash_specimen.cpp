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

namespace
{
// The fault itself. UndefinedBehaviorSanitizer (the linux-gcc-asan lane builds with non-recovering
// -fsanitize=undefined) reports a null store and aborts before the write reaches the hardware, which the harness
// reads as Unexpected; exempting this one store from the null check keeps the control a genuine SIGSEGV there,
// exactly as handle_segv=0 does for ASan. Every other check and every other specimen stays instrumented.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((no_sanitize("null")))
#endif
void fault_on_null_write()
{
    volatile int* p = nullptr;
    *p = 42; // access violation / SIGSEGV
}
} // namespace

int main()
{
    crd_diag_harden();
    crd_diag_announce();
    fault_on_null_write();
    return 0;
}
