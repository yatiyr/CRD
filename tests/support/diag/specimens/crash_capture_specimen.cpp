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
//   logger_lock             -- fault holding the stdio stream lock (Win: _lock_file; Linux: flockfile): the handler's
//                              write(2)-only fatal path still records (a stdio-using handler would deadlock).
//   chain                   -- (linux) a prior SIGSEGV handler is installed before crd::install(); crd records, then
//                              chains to it -> it drops chain.marker and re-raises. Proves signal chaining. Win: plain fault.
//   hook_fault              -- the crash-report hook itself faults: the record (written before the hook) survives and the
//                              recursive fault is bounded (no hang). Proves recursive-fault safety.
//   assert_default          -- a CRD_FATAL with NO assert platform handler under declared-headless: the default assert
//                              path terminates promptly (abort -> SIGABRT) with evidence, never a modal dialog/hang.
//   overflow                -- unbounded recursion on the main thread: stack overflow (0xC00000FD), still dumps.
//   overflow_thread         -- overflow on an UNguarded worker thread: still dumps (the dump runs on the handler stack).
//   overflow_thread_guarded -- overflow on a worker that reserved a last-chance guard first: still dumps.
//   fiber                   -- fault on a fiber stack: NOT caught by the last-chance filter (0 dumps) -- honest measured
//                              limitation (needs a VEH; links crd-jobs to drive a real fiber).
//   fiber_overflow          -- overflow a fiber stack (whichever thread runs the job): Windows -- not caught (fiber
//                              limitation, as `fiber`); Linux -- the running OS thread's alt stack captures it.
//   fiber_overflow_worker   -- like fiber_overflow but forces the fault onto a BACKGROUND WORKER OS thread (main does
//                              not pump). Linux: captured only when worker_loop registered the worker's alt stack
//                              (DIAG.5b (d)). Falls back to exit 61 if no fault occurs (never expected).
//   sigkill                 -- (linux) kill(getpid, SIGKILL): uncatchable, no handler runs, NO record written -- used
//                              to prove a pre-seeded prior record is retained. Windows: falls through to a plain fault.
//   inject_dump_fail        -- (asserts only) force MiniDumpWriteDump FALSE, then fault: 0 dumps, marker == DumpFailed.
#include "specimen_common.hpp"

#include <crd/core/crash.hpp>
#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <intrin.h>  // __fastfail
#include <windows.h> // RemoveDirectoryA, FAST_FAIL_FATAL_APP_EXIT, CreateFileA/WriteFile
#elif defined(__linux__)
#include <fcntl.h>  // open, O_WRONLY/O_CREAT/O_TRUNC (chain.marker)
#include <signal.h> // kill/SIGKILL (sigkill), sigaction/raise/SA_SIGINFO (chain)
#include <unistd.h> // rmdir (denied), getpid, write, close
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
#if defined(_WIN32)
std::atomic<bool> g_marker_written{false};   // write the marker once (concurrent modes fire the hook twice)
#endif

// The hook records only (never terminates). It drops a machine-readable marker so the parent can tell an honest
// write failure ("handler ran, saw DumpFailed") from "the handler never ran": the marker holds the decimal
// WriteResult. Written with Win32 CreateFileA/WriteFile on the handler thread -- no CRT lock.
void recording_hook(const crd::crash::CrashReport& report, void* /*user*/) noexcept
{
#if defined(_WIN32)
    if (g_marker_written.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    if (g_output_dir[0] == '\0')
    {
        return;
    }

    char path[1088];
    int  n = 0;
    for (int i = 0; g_output_dir[i] != '\0' && n < 1000; ++i)
    {
        path[n++] = g_output_dir[i];
    }
    const char* suffix = "\\report.marker";
    for (int i = 0; suffix[i] != '\0'; ++i)
    {
        path[n++] = suffix[i];
    }
    path[n] = '\0';

    char     body[16];
    int      bn = 0;
    unsigned v  = static_cast<unsigned>(report.write);
    if (v == 0U)
    {
        body[bn++] = '0';
    }
    char tmp[12];
    int  tn = 0;
    while (v != 0U)
    {
        tmp[tn++] = static_cast<char>('0' + (v % 10U));
        v /= 10U;
    }
    while (tn > 0)
    {
        body[bn++] = tmp[--tn];
    }

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
#if defined(__clang__) // clang-cl defines _MSC_VER but not __GNUC__, so clang must be handled before both
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winfinite-recursion" // deliberate: this is the overflow specimen
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4717) // recursive on all control paths -- deliberate: this is the overflow specimen
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion" // deliberate unbounded recursion
#endif
int recurse(int depth) noexcept
{
    volatile char pad[1024];
    pad[0]               = static_cast<char>(depth & 0x7F);
    pad[sizeof(pad) - 1] = static_cast<char>(depth & 0x7F);
    return pad[0] + recurse(depth + 1) + pad[sizeof(pad) - 1];
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// Body for the worker-thread overflow modes: optionally reserve a last-chance stack guarantee first (so the crash
// filter + dispatch have room), then overflow.
void overflow_body(bool guard) noexcept
{
    if (guard)
    {
        crd::crash::guard_current_thread_stack();
    }
    (void)recurse(0);
}

// A job body that overflows its FIBER stack (whichever worker runs it) -> the fiber's guard page faults. Capturing it
// needs the worker OS thread to carry an alternate signal stack (the fiber stack is exhausted); that is what DIAG.5b
// (d)'s worker_pool wiring provides on Linux.
void fiber_overflow_job(void* /*data*/) noexcept
{
    (void)recurse(0);
}

// For `hook_fault`: a crash-report hook that itself faults. crd writes the record BEFORE firing the hook, so the record
// must still exist; the hook's recursive fault is bounded by the handler's signal mask (forced to SIG_DFL, no re-entry),
// so the process terminates without hanging.
void faulting_hook(const crd::crash::CrashReport& /*report*/, void* /*user*/) noexcept
{
    null_write();
}

#if defined(__linux__)
// The `chain` mode's PRIOR SIGSEGV handler, installed before crd::install() so crd saves + chains to it. crd records
// first, then restores this disposition and re-raises, so this runs next: it drops an async-signal-safe chain.marker
// (proving crd chained), then forces SIG_DFL and re-raises to terminate (returning would re-run the faulting insn).
void chain_prev_handler(int sig, siginfo_t* /*info*/, void* /*ctx*/) noexcept
{
    if (g_output_dir[0] != '\0')
    {
        char path[1088];
        int  n = 0;
        for (int i = 0; g_output_dir[i] != '\0' && n < 1000; ++i)
        {
            path[n++] = g_output_dir[i];
        }
        const char* suffix = "/chain.marker";
        for (int i = 0; suffix[i] != '\0'; ++i)
        {
            path[n++] = suffix[i];
        }
        path[n]        = '\0';
        const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
        {
            // GCC's warn_unused_result ignores a (void) cast; the marker's existence (open succeeded) is the
            // evidence, so the byte count is deliberately unused. Only async-signal-safe calls in this handler.
            const ssize_t written = ::write(fd, "1", 1);
            (void)written;
            (void)::close(fd);
        }
    }
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    (void)::sigemptyset(&dfl.sa_mask);
    dfl.sa_flags = 0;
    (void)::sigaction(sig, &dfl, nullptr);
    (void)::raise(sig);
}

void install_chain_prev_handler() noexcept
{
    struct sigaction sa{};
    sa.sa_sigaction = chain_prev_handler;
    (void)::sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    (void)::sigaction(SIGSEGV, &sa, nullptr);
}
#endif
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
    {
        g_output_dir[i] = dir[i];
    }

#if defined(__linux__)
    // `chain`: install a prior SIGSEGV handler BEFORE crd::install() so crd saves it as the previous disposition and
    // chains to it after recording (the prior handler drops chain.marker, then re-raises to terminate).
    if (std::strcmp(mode, "chain") == 0)
    {
        install_chain_prev_handler();
    }
#endif

    const crd::crash::InstallResult ir = crd::crash::install(dir);
    if (ir != crd::crash::InstallResult::Ok && ir != crd::crash::InstallResult::OkReinstalled &&
        ir != crd::crash::InstallResult::OkReplacedForeignFilter)
    {
        std::printf("install failed: %u\n", static_cast<unsigned>(ir));
        std::fflush(stdout);
        return 60;
    }
    crd::crash::set_crash_report_handler(&recording_hook, nullptr);

    // Each mode is a distinct entry point the tests select by name. On a given platform several reduce to the same
    // plain fault (av, chain and sigkill on Windows; unload_av too on Linux) by design, not by copy-paste.
    // NOLINTBEGIN(bugprone-branch-clone)
    if (std::strcmp(mode, "av") == 0)
    {
        null_write();
    }
    else if (std::strcmp(mode, "unload_av") == 0)
    {
        // DIAG.5d(d): make an unloaded-module GENERATION then a live one, then fault. winhttp.dll is not pre-loaded in
        // a console exe, so FreeLibrary drops its refcount to 0 and ntdll traces the unload; the reload puts it back in
        // the live module list. At crash time the dump carries winhttp in BOTH the UnloadedModuleListStream (old) and
        // the ModuleListStream (live) -- two generations, one dump.
#if defined(_WIN32)
        HMODULE h1 = LoadLibraryA("winhttp.dll");
        if (h1 != nullptr)
        {
            (void)FreeLibrary(h1);        // refcount -> 0 -> traced unload
        }
        (void)LoadLibraryA("winhttp.dll"); // reload -> live generation (deliberately not freed; the process now faults)
#endif
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
#elif defined(__linux__)
        (void)::rmdir(dir); // remove the just-created (empty) output dir so the record open fails (ENOENT -> OpenFailed)
#endif
        null_write();
    }
    else if (std::strcmp(mode, "sigkill") == 0)
    {
        // Uncatchable termination -- the OOM-killer mechanism. SIGKILL cannot be handled, so no handler runs and NO new
        // record is written; a pre-existing record in the output dir must be retained. This is the design's "retain
        // previous records or explicitly report no final capture" without promising a handler for uncatchable exits.
#if defined(__linux__)
        (void)::kill(::getpid(), SIGKILL); // immediate; the line below is never reached on Linux
#endif
        null_write(); // Windows / unreachable-on-Linux fallback so the process still terminates as a crash
    }
    else if (std::strcmp(mode, "logger_lock") == 0)
    {
#if defined(_WIN32)
        _lock_file(stderr); // hold the CRT stderr stream lock, then fault: a handler that fprintf's stderr would hang
#elif defined(__linux__)
        flockfile(stderr); // hold the stdio stream lock, then fault: a handler using stdio (not write(2)) would hang
#endif
        null_write();
    }
    else if (std::strcmp(mode, "chain") == 0)
    {
        // crd records, then chains to the prior handler installed before crd::install() (Linux); that handler drops
        // chain.marker and re-raises. On Windows there is no prior-handler pre-install here, so this is a plain fault.
        null_write();
    }
    else if (std::strcmp(mode, "hook_fault") == 0)
    {
        crd::crash::set_crash_report_handler(&faulting_hook, nullptr); // the crash-report hook itself faults
        null_write();
    }
    else if (std::strcmp(mode, "assert_default") == 0)
    {
        // A fatal assertion with NO assert platform handler installed. crd_diag_harden() declared this process
        // headless, so the DEFAULT assert path must terminate promptly (abort -> SIGABRT) with evidence -- never a
        // modal dialog, never a silent ignore, never a hang. On Linux the installed crash handler records the SIGABRT
        // (the emergency channel); on Windows it terminates promptly with the stderr evidence.
        CRD_FATAL("forced fatal assertion (assert_default specimen mode)"); // headless -> abort(); never returns
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
    else if (std::strcmp(mode, "fiber_overflow") == 0)
    {
        crd::jobs::Config cfg;
        cfg.num_threads = 2U;
        crd::jobs::init(cfg);
        crd::jobs::JobDecl j{};
        j.fn                        = &fiber_overflow_job; // overflows the fiber stack on whichever worker runs it
        crd::jobs::Counter* const c = crd::jobs::run(j);
        crd::jobs::wait(c); // never returns: the fiber stack overflow terminates the process
        crd::jobs::shutdown();
    }
    else if (std::strcmp(mode, "fiber_overflow_worker") == 0)
    {
        // Force the fiber-stack overflow onto a BACKGROUND WORKER OS thread, not the main thread. With two threads
        // there is one background worker; main deliberately does NOT wait()/pump() (either would run the job on the
        // guarded main thread), so the worker steals and runs the job and its fiber stack overflows on the worker OS
        // thread. Capturing that fault on Linux needs the worker OS thread to carry an alternate signal stack, which
        // the pool registers once per worker (guard_current_thread_stack() at worker-loop entry). The worker's fault
        // terminates the process, so the bounded wait below never actually returns; its fallback is a distinct
        // non-fault exit code so the harness never hangs if (contrary to expectation) no fault occurs.
        crd::jobs::Config cfg;
        cfg.num_threads = 2U;
        crd::jobs::init(cfg);
        crd::jobs::JobDecl j{};
        j.fn = &fiber_overflow_job;
        (void)crd::jobs::run(j); // a background worker runs it; do NOT wait() -- that would pump it onto main
        for (int i = 0; i < 120; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(25)); // ~3s safety bound; the worker fault kills us first
        }
        crd::jobs::shutdown();
        return 61; // reached only if the worker never faulted (never expected)
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
    // NOLINTEND(bugprone-branch-clone)

    return 0; // unreachable in every crash mode
}
