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

#if defined(__SANITIZE_ADDRESS__)
#define CRD_DIAG_HAS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CRD_DIAG_HAS_SANITIZER 1
#endif
#endif
#ifndef CRD_DIAG_HAS_SANITIZER
#define CRD_DIAG_HAS_SANITIZER 0
#endif

#if CRD_DIAG_HAS_SANITIZER
#define CRD_DIAG_SPECIMEN_SANITIZER "asan"
#else
#define CRD_DIAG_SPECIMEN_SANITIZER "none"
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
