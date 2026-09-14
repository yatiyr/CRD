// The DIAG.1b instrument-present control: two threads commit a deliberate data race on a
// plain, unsynchronized int. Under ThreadSanitizer the runtime reports the race and, with
// TSAN_OPTIONS=halt_on_error=1, aborts -- the harness classifies that as SanitizerCaught,
// proving the TSan lane actually detects races rather than being silently disabled. With no
// TSan runtime there is nothing to catch a manufactured race, so it announces SANITIZER=none
// and exits cleanly -- the harness then reports InstrumentAbsent, never a silent pass. This
// mirrors heap_overflow_specimen.cpp for AddressSanitizer.
//
// This is the negative control the positive-control suite (test_tsan_fiber_model.cpp) cannot
// hold: an intentional race aborts the process under halt_on_error, so it lives here as a
// specimen the harness runs as a bounded child, not as an ordinary ctest case.
// The data-race route needs ThreadSanitizer specifically. Under any non-TSan build -- including an
// ASan build (win-asan / linux-gcc-asan), where ASan is present but does NOT detect data races -- the
// route is absent, so pre-tag SANITIZER=none and let the harness report InstrumentAbsent rather than
// expect a catch a non-TSan sanitizer cannot deliver. Detected before including the common header.
#if defined(__SANITIZE_THREAD__)
#define CRD_DIAG_ROUTE_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CRD_DIAG_ROUTE_TSAN 1
#endif
#endif
#ifndef CRD_DIAG_ROUTE_TSAN
#define CRD_DIAG_ROUTE_TSAN 0
#endif
#if !CRD_DIAG_ROUTE_TSAN
#define CRD_DIAG_SPECIMEN_SANITIZER "none"
#endif

#include "specimen_common.hpp"

#include <thread>

int main()
{
    crd_diag_harden();
    crd_diag_announce();
#if CRD_DIAG_HAS_TSAN
    int         shared = 0; // plain, non-atomic -- the two threads race on it with no ordering
    std::thread a([&shared] {
        for (int i = 0; i < 200000; ++i)
        {
            shared += 1;
        }
    });
    std::thread b([&shared] {
        for (int i = 0; i < 200000; ++i)
        {
            shared += 2;
        }
    });
    a.join();
    b.join();
    // Under halt_on_error TSan aborts before here; the sink stops the racy writes being elided.
    return shared == 0 ? 5 : 0;
#else
    return 0;
#endif
}
