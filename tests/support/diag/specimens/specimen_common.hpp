#pragma once

// A DIAG.0 detector specimen: a minimal standalone program the specimen harness
// runs as a bounded child. It announces its stamped identity and whether it was
// built with a sanitizer BEFORE doing its action (and flushes), so the harness
// reads those even if the action then aborts the process. crd_diag_harden() makes
// a crash die fast and headless (no Windows error dialog or WER hang).

// set_assert_headless lives in crd-core. Only a specimen that links crd-core (directly, or transitively via
// crd-jobs) can fire a crd assert, so only those need it hardened; a minimal specimen -- e.g. the DIAG.1b TSan
// data-race one links only Threads -- has no crd asserts and no crd-core on its include path. Gate on header
// availability so both kinds build (a specimen with crd-core headers on its path also links crd-core, so the
// symbol resolves; one without simply skips the call it does not need).
#if defined(__has_include)
#if __has_include(<crd/core/assert.hpp>)
#include <crd/core/assert.hpp> // set_assert_headless: a spawned specimen must never block on a modal assert dialog
#define CRD_DIAG_HAS_CRD_ASSERT 1
#endif
#endif
#ifndef CRD_DIAG_HAS_CRD_ASSERT
#define CRD_DIAG_HAS_CRD_ASSERT 0
#endif

#include <cstdio>

#ifndef CRD_DIAG_SPECIMEN_ID
#define CRD_DIAG_SPECIMEN_ID "unknown"
#endif

// AddressSanitizer detection (heap/overflow specimens key on this specifically).
#if defined(__SANITIZE_ADDRESS__)
#define CRD_DIAG_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CRD_DIAG_HAS_ASAN 1
#endif
#endif
#ifndef CRD_DIAG_HAS_ASAN
#define CRD_DIAG_HAS_ASAN 0
#endif

// ThreadSanitizer detection (the DIAG.1b data-race specimen keys on this specifically).
#if defined(__SANITIZE_THREAD__)
#define CRD_DIAG_HAS_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CRD_DIAG_HAS_TSAN 1
#endif
#endif
#ifndef CRD_DIAG_HAS_TSAN
#define CRD_DIAG_HAS_TSAN 0
#endif

// Back-compat alias: any sanitizer runtime present.
#define CRD_DIAG_HAS_SANITIZER (CRD_DIAG_HAS_ASAN || CRD_DIAG_HAS_TSAN)

// A specimen may define the value-less flag CRD_DIAG_SPECIMEN_ROUTE_ABSENT before including this header to declare
// that its specific error ROUTE is unavailable on this toolchain even though a sanitizer is linked (e.g.
// stack-use-after-return is not reliably reported by MSVC ASan), or that no sanitizer is involved at all. The label
// is then "none" and the harness treats it as InstrumentAbsent rather than expecting a catch -- an explicit
// unqualified route, never a skip-pass. (A flag, not a string constant, keeps specimens free of constant macros.)
#if defined(CRD_DIAG_SPECIMEN_ROUTE_ABSENT)
#define CRD_DIAG_SPECIMEN_SANITIZER "none"
#elif CRD_DIAG_HAS_ASAN
#define CRD_DIAG_SPECIMEN_SANITIZER "asan"
#elif CRD_DIAG_HAS_TSAN
#define CRD_DIAG_SPECIMEN_SANITIZER "tsan"
#else
#define CRD_DIAG_SPECIMEN_SANITIZER "none"
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <cstdlib> // _set_abort_behavior, _WRITE_ABORT_MSG, _CALL_REPORTFAULT
#include <crtdbg.h>
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h> // setrlimit(RLIMIT_CORE): suppress core dumps from a crashing specimen
#endif

inline void crd_diag_harden()
{
    // A spawned specimen is headless by definition: the default assert path must terminate promptly with evidence,
    // never block on a modal dialog (Windows) waiting for a click that never comes. Only present when this specimen
    // links crd-core (see the header guard above); a specimen without crd-core has no crd asserts to harden.
#if CRD_DIAG_HAS_CRD_ASSERT
    crd::set_assert_headless(true);
#endif
#if defined(_WIN32)
    // No critical-error box, no crash dialog, no debug-CRT assert/abort dialog:
    // a crashing specimen must die immediately so the harness reads its exit code
    // instead of hanging on a modal window until the timeout.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
#elif defined(__linux__)
    // A crashing specimen must not write a core dump: the harness reads the exit code and the crd crash record is the
    // artifact of record, so a kernel core file only costs time and litters the output tree. Set the soft and hard
    // core limit to 0.
    struct rlimit rl;
    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    (void)::setrlimit(RLIMIT_CORE, &rl);
#endif
}

inline void crd_diag_announce()
{
    std::printf("CRD_DIAG_IDENTITY=%s\n", CRD_DIAG_SPECIMEN_ID);
    std::printf("CRD_DIAG_SANITIZER=%s\n", CRD_DIAG_SPECIMEN_SANITIZER);
    std::fflush(stdout);
}
