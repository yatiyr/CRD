// DIAG.3f -- the stack-use-after-return positive (the design's named "MSVC use-after-return" class).
//
// On gcc/clang ASan (linux-gcc-asan) the fake-stack instrumentation reliably reports a read through a
// pointer to a dead frame: the specimen enables detect_stack_use_after_return, escapes the address of a
// local, reads it after that frame dies, and ASan aborts (SanitizerCaught). With no sanitizer it
// announces SANITIZER=none and exits clean -> InstrumentAbsent, never a silent pass.
//
// MSVC ASan does NOT reliably report this route (its fake-stack instrumentation flags the address-escape
// itself as a spurious stack-buffer-underflow rather than the post-return read), so this specimen
// declares the route UNAVAILABLE under MSVC by pre-tagging SANITIZER=none -- the harness then reports
// InstrumentAbsent, an explicit unqualified route (the acceptance's partial-instrumentation dependency),
// never converted into a skip-pass. Mirrors heap_overflow_specimen for the qualified case.
#if defined(_MSC_VER)
#define CRD_DIAG_SPECIMEN_SANITIZER "none" // stack-use-after-return not qualified on MSVC ASan
#define CRD_DIAG_IS_MSVC 1
#else
#define CRD_DIAG_IS_MSVC 0
#endif
#include "specimen_common.hpp"

#define CRD_DIAG_UAR_ACTIVE (CRD_DIAG_HAS_ASAN && !CRD_DIAG_IS_MSVC)

#if CRD_DIAG_UAR_ACTIVE
// Enable stack-use-after-return detection at ASan init (reading ASAN_OPTIONS in main would be too
// late). halt_on_error keeps the first report fatal so the harness classifies the abort.
extern "C" const char* __asan_default_options()
{
    return "detect_stack_use_after_return=1:halt_on_error=1";
}

static volatile int* g_escaped = nullptr;

__attribute__((noinline)) static void stash_local()
{
    int local = 0x5A5A;  // lives only for this frame
    g_escaped = &local;  // escape the address; the frame dies on return
}
#endif

int main()
{
    crd_diag_harden();
    crd_diag_announce();
#if CRD_DIAG_UAR_ACTIVE
    stash_local();
    const int observed = *g_escaped; // read the now-dead frame -- stack-use-after-return
    return observed == 0x5A5A ? 0 : 1;
#else
    return 0;
#endif
}
