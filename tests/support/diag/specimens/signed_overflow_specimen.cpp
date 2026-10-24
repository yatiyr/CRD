// DIAG.3f -- the UndefinedBehaviorSanitizer positive: a signed integer overflow. Under UBSan (linux-gcc-asan, built
// without recovery) the add is reported as a runtime error and the process dies; the default options print the stack,
// so the report names the site. Without UBSan it announces SANITIZER=none and exits clean -> InstrumentAbsent, never a
// silent pass. MSVC has no UBSan; GCC defines no macro for it, so the target passes CRD_DIAG_UBSAN_BUILD.
#define CRD_DIAG_SPECIMEN_UBSAN_CLASS // an UndefinedBehaviorSanitizer-only error class (see specimen_common.hpp)
#include "specimen_common.hpp"

#include <climits>

#if CRD_DIAG_HAS_UBSAN
// Read at UBSan init: stacks are printed (the site check reads a frame) and the first report ends the process.
extern "C" const char* __ubsan_default_options()
{
    return "print_stacktrace=1:halt_on_error=1";
}

CRD_DIAG_SITE static int crd_diag_add_overflow(int a, int b)
{
    const volatile int lhs = a; // volatile: the add is never folded at compile time
    const volatile int rhs = b;
    return lhs + rhs; // INT_MAX + 1 -- signed-integer-overflow
}
#endif

int main()
{
    crd_diag_harden();
    crd_diag_announce();
#if CRD_DIAG_HAS_UBSAN
    return crd_diag_add_overflow(INT_MAX, 1) == INT_MIN ? 0 : 1;
#else
    return 0;
#endif
}
