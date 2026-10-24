// DIAG.3f -- the stack-use-after-scope positive. Under ASan the specimen escapes the address of a block-scoped local,
// leaves the block and reads it while the frame is still live; the runtime poisons the slot at the end of its scope
// (gcc and clang by default, MSVC /fsanitize=address as well) and reports stack-use-after-scope. Without ASan it
// announces SANITIZER=none and exits clean -> InstrumentAbsent, never a silent pass. Mirrors use_after_free_specimen.
#define CRD_DIAG_SPECIMEN_ASAN_CLASS // an AddressSanitizer-only error class (see specimen_common.hpp)
#include "specimen_common.hpp"

#if CRD_DIAG_HAS_ASAN
CRD_DIAG_SITE static int crd_diag_read_dead_scope(const volatile int* escaped)
{
    return *escaped; // the local's scope has ended -- stack-use-after-scope
}

CRD_DIAG_SITE static int leave_scope_then_read()
{
    const volatile int* escaped = nullptr;
    {
        volatile int local = 0x5A5A;
        escaped = &local;
    }
    return crd_diag_read_dead_scope(escaped);
}
#endif

int main()
{
    crd_diag_harden();
    crd_diag_announce();
#if CRD_DIAG_HAS_ASAN
    return leave_scope_then_read() == 0x5A5A ? 0 : 1;
#else
    return 0;
#endif
}
