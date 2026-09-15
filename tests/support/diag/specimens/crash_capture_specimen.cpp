// DIAG.5a (e1): the crash-capture acceptance specimen. A bounded child that installs the real crd crash handler
// and then faults in one of several ways, so the DIAG.0 harness can verify the fatal path from the outside: the
// process actually terminates, the exit code retains the original fault reason, and the on-disk dump outcome matches
// what the mode should produce. The crash-report hook only RECORDS -- it never _Exit()s, so the fault reason reaches
// the exit code via the filter chain -> OS (an _Exit(42) from the hook would destroy the "retains original fault
// reason" acceptance).
//
//   argv[1] = output directory (absolute)   argv[2] = mode (one of the modes below)
//
// Modes:
//   av                      -- a wild null write: one dump, exit 0xC0000005.
//   concurrent              -- two threads fault at once: the single-shot gate writes exactly ONE dump, exit 0xC0000005.
//   fastfail                -- __fastfail bypasses SEH entirely: NO dump is written, exit 0xC0000409 (honest limitation).
//   denied                  -- install() succeeds, the dir is then removed, so the write fails (no partial), 0xC0000005.
//   logger_lock             -- fault while holding the CRT stderr lock: the lock-free WriteFile fatal path still dumps.
//   overflow                -- unbounded recursion on the main thread: stack overflow (0xC00000FD), still dumps.
//   overflow_thread         -- overflow on an UNguarded worker thread: still dumps (the dump runs on the handler stack).
//   overflow_thread_guarded -- overflow on a worker that reserved a last-chance guard first: still dumps.
//   fiber                   -- fault on a fiber stack: NOT caught by the last-chance filter (0 dumps) -- honest measured
//                              limitation (needs a VEH; links crd-jobs to drive a real fiber).
//   inject_dump_fail        -- (asserts only) force MiniDumpWriteDump FALSE, then fault: 0 dumps, marker == DumpFailed.
#define CRD_DIAG_SPECIMEN_ID "crd-diag-crash-capture-specimen"
#include "specimen_common.hpp"

#include <crd/core/crash.hpp>
#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <intrin.h>  // __fastfail
#include <windows.h> // RemoveDirectoryA, FAST_FAIL_FATAL_APP_EXIT, CreateFileA/WriteFile
#endif

namespace
{
// A wild write through a null pointer -> an access violation. volatile so the store is not optimized away.
void null_write() noexcept
{
    volatile int* p = nullptr;
    *p              = 42;
}

// A job body that faults inside a fiber (whichever worker runs it).
void fiber_fault_job(void* /*data*/) noexcept
{
    null_write();
}

char              g_output_dir[1024] = {0};  // argv[1], captured at startup for the marker path
std::atomic<bool> g_marker_written{false};   // write the marker once (concurrent modes fire the hook twice)

// The hook records only (never terminates). It drops a machine-readable marker so the parent can tell an honest
// write failure ("handler ran, saw DumpFailed") from "the handler never ran": the marker holds the decimal
// WriteResult. Written with Win32 CreateFileA/WriteFile on the handler thread -- no CRT lock.
void recording_hook(const crd::crash::CrashReport& report, void* /*user*/) noexcept
{
#if defined(_WIN32)
    if (g_marker_written.exchange(true, std::memory_order_acq_rel))
        return;
    if (g_output_dir[0] == '\0')
        return;

    char path[1088];
    int  n = 0;
    for (int i = 0; g_output_dir[i] != '\0' && n < 1000; ++i)
        path[n++] = g_output_dir[i];
    const char* suffix = "\\report.marker";
    for (int i = 0; suffix[i] != '\0'; ++i)
        path[n++] = suffix[i];
    path[n] = '\0';

    char     body[16];
    int      bn = 0;
    unsigned v  = static_cast<unsigned>(report.write);
    if (v == 0U)
        body[bn++] = '0';
    char tmp[12];
    int  tn = 0;
    while (v != 0U)
    {
        tmp[tn++] = static_cast<char>('0' + (v % 10U));
        v /= 10U;
    }
    while (tn > 0)
        body[bn++] = tmp[--tn];

    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        (void)WriteFile(f, body, static_cast<DWORD>(bn), &written, nullptr);
        (void)CloseHandle(f);
    }
#else
    (void)report;
#endif
}

// Non-tail recursion with a volatile local so the compiler cannot fold it away or turn it into a loop; each frame
// touches a page of stack (__chkstk) so the guard page is reached quickly -> STATUS_STACK_OVERFLOW (0xC00000FD).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4717) // recursive on all control paths -- deliberate: this is the overflow specimen
#endif
int recurse(int depth) noexcept
{
    volatile char pad[1024];
    pad[0]               = static_cast<char>(depth & 0x7F);
    pad[sizeof(pad) - 1] = static_cast<char>(depth & 0x7F);
    return pad[0] + recurse(depth + 1) + pad[sizeof(pad) - 1];
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Body for the worker-thread overflow modes: optionally reserve a last-chance stack guarantee first (so the crash
// filter + dispatch have room), then overflow.
void overflow_body(bool guard) noexcept
{
    if (guard)
        crd::crash::guard_current_thread_stack();
    (void)recurse(0);
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();   // headless: no error dialog, so the harness reads the exit code rather than hanging
    crd_diag_announce(); // identity + sanitizer to stdout (flushed) before anything can fault

    if (argc < 3)
    {
        std::printf("usage: crash_capture_specimen <dir> <mode>\n");
        std::fflush(stdout);
        return 60; // a distinct instrument-failure code, never a crash
    }
    const char* dir  = argv[1];
    const char* mode = argv[2];

    for (int i = 0; dir[i] != '\0' && i < 1023; ++i) // capture the dir for the marker path before anything can fault
        g_output_dir[i] = dir[i];

    const crd::crash::InstallResult ir = crd::crash::install(dir);
    if (ir != crd::crash::InstallResult::Ok && ir != crd::crash::InstallResult::OkReinstalled &&
        ir != crd::crash::InstallResult::OkReplacedForeignFilter)
    {
        std::printf("install failed: %u\n", static_cast<unsigned>(ir));
        std::fflush(stdout);
        return 60;
    }
    crd::crash::set_crash_report_handler(&recording_hook, nullptr);

    if (std::strcmp(mode, "av") == 0)
    {
        null_write();
    }
    else if (std::strcmp(mode, "concurrent") == 0)
    {
        std::thread a(&null_write);
        std::thread b(&null_write);
        a.join(); // the process dies mid-fault; these joins may never return
        b.join();
    }
    else if (std::strcmp(mode, "fastfail") == 0)
    {
#if defined(_WIN32)
        __fastfail(FAST_FAIL_FATAL_APP_EXIT); // bypasses SEH: our filter never runs, no dump, exit 0xC0000409
#else
        __builtin_trap();
#endif
    }
    else if (std::strcmp(mode, "denied") == 0)
    {
#if defined(_WIN32)
        (void)RemoveDirectoryA(dir); // the just-created (empty) output dir is gone -> the dump write will fail
#endif
        null_write();
    }
    else if (std::strcmp(mode, "logger_lock") == 0)
    {
#if defined(_WIN32)
        _lock_file(stderr); // hold the CRT stderr stream lock, then fault: a handler that fprintf's stderr would hang
#endif
        null_write();
    }
    else if (std::strcmp(mode, "overflow") == 0)
    {
        recurse(0); // unbounded recursion on the main thread -> stack overflow (0xC00000FD)
    }
    else if (std::strcmp(mode, "overflow_thread") == 0)
    {
        std::thread t(&overflow_body, false); // a worker thread with NO stack guarantee overflows
        t.join();
    }
    else if (std::strcmp(mode, "overflow_thread_guarded") == 0)
    {
        std::thread t(&overflow_body, true); // the worker reserves a last-chance guard first, then overflows
        t.join();
    }
    else if (std::strcmp(mode, "fiber") == 0)
    {
        crd::jobs::Config cfg;
        cfg.num_threads = 2U;
        crd::jobs::init(cfg);
        crd::jobs::JobDecl j{};
        j.fn                       = &fiber_fault_job; // runs in a fiber on whichever worker picks it up, then faults
        crd::jobs::Counter* const c = crd::jobs::run(j);
        crd::jobs::wait(c); // never returns: the fiber fault terminates the process
        crd::jobs::shutdown();
    }
    else if (std::strcmp(mode, "inject_dump_fail") == 0)
    {
#if CRD_ENABLE_ASSERTS
        crd::crash::test_inject_write_failure(crd::crash::WriteResult::DumpFailed); // force MiniDumpWriteDump FALSE
        null_write();
#else
        std::printf("inject_dump_fail needs CRD_ENABLE_ASSERTS\n");
        std::fflush(stdout);
        return 60;
#endif
    }
    else
    {
        std::printf("bad mode: %s\n", mode);
        std::fflush(stdout);
        return 60;
    }

    return 0; // unreachable in every crash mode
}
