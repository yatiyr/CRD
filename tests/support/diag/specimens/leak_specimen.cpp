// DIAG.3f -- the memory-leak positive (the "leak routes" class). On a build with LeakSanitizer
// (gcc/clang ASan on Linux, i.e. linux-gcc-asan) it allocates a block, drops the only reference, then
// runs a recoverable leak check and aborts if the leak is reported -- turning LSan's finding into a
// crash the harness classifies as SanitizerCaught (LSan's normal at-exit path is a plain non-zero
// exit, which the harness would not treat as a catch). Where no LSan interface exists (MSVC ASan has
// no LeakSanitizer; plain builds) it pre-tags SANITIZER=none and exits clean -> InstrumentAbsent,
// an explicit unqualified route, never a skip-pass. Contract: docs/design/runtime-diagnostics.md#diag-3f.

// Decide LSan-route availability BEFORE including the common header so the specimen can pre-tag the
// route as absent (SANITIZER=none) when LeakSanitizer is not functional even though ASan may be. MSVC
// ships <sanitizer/lsan_interface.h> but has no working LeakSanitizer, so it is excluded here; the
// route is the gcc/clang LSan (linux-gcc-asan).
#if defined(__has_include) && !defined(_MSC_VER)
#if __has_include(<sanitizer/lsan_interface.h>)
#define CRD_DIAG_LSAN_ROUTE 1
#endif
#endif
#ifndef CRD_DIAG_LSAN_ROUTE
#define CRD_DIAG_LSAN_ROUTE 0
#endif
#if !CRD_DIAG_LSAN_ROUTE
#define CRD_DIAG_SPECIMEN_SANITIZER "none" // no functional LeakSanitizer route on this toolchain
#endif

#include "specimen_common.hpp"

#include <cstdlib>

#define CRD_DIAG_LEAK_ACTIVE (CRD_DIAG_HAS_ASAN && CRD_DIAG_LSAN_ROUTE)

#if CRD_DIAG_LEAK_ACTIVE
#include <sanitizer/lsan_interface.h>

static volatile void* g_sink = nullptr;

// Allocate and drop the reference in a separate frame so no live stack/register copy of the pointer
// survives into the leak check -- the block is genuinely unreachable when LSan scans.
__attribute__((noinline)) static void leak_now()
{
    void* p = std::malloc(1024);
    g_sink = p;       // force the allocation (defeat elision)
    g_sink = nullptr; // drop the only reference
}
#endif

int main()
{
    crd_diag_harden();
    crd_diag_announce();
#if CRD_DIAG_LEAK_ACTIVE
    leak_now();
    if (__lsan_do_recoverable_leak_check() != 0)
    {
        std::abort(); // a crash the harness classifies as SanitizerCaught
    }
    return 0;
#else
    return 0;
#endif
}
