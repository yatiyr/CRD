#pragma once

// A DIAG.0 detector specimen: a minimal standalone program the specimen harness
// runs as a bounded child. It announces its stamped identity and whether it was
// built with a sanitizer BEFORE doing its action (and flushes), so the harness
// reads those even if the action then aborts the process. crd_diag_harden() makes
// a crash die fast and headless (no Windows error dialog or WER hang).

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

// A specimen may pre-define CRD_DIAG_SPECIMEN_SANITIZER before including this header to declare that
// its specific error ROUTE is unavailable on this toolchain even though a sanitizer is linked (e.g.
// stack-use-after-return is not reliably reported by MSVC ASan). The harness then treats it as
// InstrumentAbsent rather than expecting a catch -- an explicit unqualified route, never a skip-pass.
#ifndef CRD_DIAG_SPECIMEN_SANITIZER
#if CRD_DIAG_HAS_ASAN
#define CRD_DIAG_SPECIMEN_SANITIZER "asan"
#elif CRD_DIAG_HAS_TSAN
#define CRD_DIAG_SPECIMEN_SANITIZER "tsan"
#else
#define CRD_DIAG_SPECIMEN_SANITIZER "none"
#endif
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <cstdlib> // _set_abort_behavior, _WRITE_ABORT_MSG, _CALL_REPORTFAULT
#include <crtdbg.h>
#include <windows.h>
#endif

inline void crd_diag_harden()
{
#if defined(_WIN32)
    // No critical-error box, no crash dialog, no debug-CRT assert/abort dialog:
    // a crashing specimen must die immediately so the harness reads its exit code
    // instead of hanging on a modal window until the timeout.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
#endif
}

inline void crd_diag_announce()
{
    std::printf("CRD_DIAG_IDENTITY=%s\n", CRD_DIAG_SPECIMEN_ID);
    std::printf("CRD_DIAG_SANITIZER=%s\n", CRD_DIAG_SPECIMEN_SANITIZER);
    std::fflush(stdout);
}
