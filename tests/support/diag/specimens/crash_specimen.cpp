// The expected-crash positive: announces, then faults hard (a null write). Under
// crd_diag_harden() this dies immediately with an access-violation / SIGSEGV exit,
// which the harness classifies as Crashed.
#include "specimen_common.hpp"

int main()
{
    crd_diag_harden();
    crd_diag_announce();
    volatile int* p = nullptr;
    *p = 42; // access violation / SIGSEGV
    return 0;
}
