// DIAG.5a (e1-f): crash-capture acceptance, verified from OUTSIDE the process by the DIAG.0 harness. The
// crash_capture_specimen installs the real crd crash handler and faults; here we assert the parent-visible truth the
// design requires -- the process actually terminates, the exit code retains the original fault reason, and the
// on-disk dump outcome matches the mode.
//
// Sanitizer branch (measured, not assumed): under win-asan, ASan installs a vectored exception handler that owns
// access violations BEFORE any SEH unhandled-exception filter, so our crd handler is preempted -- the AV is still
// fatal (ASan reports and exits non-zero) but no crd dump is written. That is the honest reality of an ASan build
// (the sanitizer IS the crash reporter there); it is asserted explicitly, never skipped. fail-fast bypasses even
// ASan's VEH, so that mode is identical on both presets.
//
// Landed across the slice: logger-lock (e2: lock-free WriteFile fatal output), the three stack-overflow modes (e2),
// the injected-write-failure marker on the fatal path (e3), and the fatal-av dump-content check (f) -- the real dump
// is read back (minidump_probe.hpp) to confirm its ExceptionStream code and that the ModuleListStream names this
// specimen with an RSDS CV record (the "missing symbols" acceptance, answered without symbolized frames -- those are
// DIAG.5d). Out of scope by design: fiber/CET faults bypass SetUnhandledExceptionFilter and need a VEH (measured
// limitation below, future work); under ASan the sanitizer owns AVs (asserted per mode); Linux capture is DIAG.5b.
#include <crd/diag/specimen_runner.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/crash.hpp> // WriteResult, for the injected-failure marker assertion

#include "minidump_probe.hpp" // Windows-only dump content probe (no-op off Windows)

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace cont = crd::containers;
namespace cd   = crd::diag;
namespace fs   = std::filesystem;

// Platform-neutral helpers, shared by the Windows and Linux acceptance blocks below. The specimen path, a fresh
// output dir, the sanitizer-build probe and the bounded harness invocation are identical across platforms; only the
// on-disk artifact (a Windows .dmp minidump vs a Linux crash_*.log record) and the fatal-reason codes differ.
namespace
{
bool built_with_asan()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

cont::String specimen_path()
{
    return cont::String{CRD_DIAG_CRASH_CAPTURE_SPECIMEN};
}

std::atomic<unsigned> g_uniq{0};

fs::path fresh_dir(const char* mode)
{
    fs::path p =
        fs::temp_directory_path() / ("crd_cc_" + std::string{mode} + "_" + std::to_string(g_uniq.fetch_add(1U)));
    std::error_code ec;
    fs::remove_all(p, ec); // clean slate; the specimen's install() creates it
    return p;
}

cd::Outcome run_mode(const char* mode, const fs::path& dir)
{
    cont::Array<cont::String> args;
    args.push_back(cont::String{dir.string().c_str()});
    args.push_back(cont::String{mode});
    cd::Expectation e;
    e.want       = cd::Expectation::Want::Crash;
    e.timeout_ms = 15000U; // a cold DbgHelp self-dump is not instant (Linux records are fast; the bound is shared)
    return cd::run_specimen(specimen_path(), args, e);
}
} // namespace

// Windows crash capture; the acceptance codes are NTSTATUS values. The Linux acceptance block (its own signal codes)
// follows after this #endif -- not a skip, a different platform mechanism (both are DIAG.5b's remit on Linux).
#if defined(_WIN32)

namespace
{
// Count and size-check the fatal-path dumps (prefix crash_) the specimen wrote into dir.
std::size_t count_crash_dumps(const fs::path& dir)
{
    std::size_t     n = 0;
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
    {
        if (it->path().extension() == ".dmp" && it->path().filename().string().rfind("crash_", 0) == 0)
        {
            ++n;
            CHECK(fs::file_size(it->path()) > 0U); // a reported dump is a real, non-empty file
        }
    }
    return n;
}

constexpr std::uint32_t kAccessViolation = 0xC0000005U;
constexpr std::uint32_t kFailFast        = 0xC0000409U;
constexpr std::uint32_t kStackOverflow   = 0xC00000FDU;

// The first fatal-path dump (prefix crash_) in dir, or an empty path if none was written.
fs::path first_crash_dump(const fs::path& dir)
{
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
        if (it->path().extension() == ".dmp" && it->path().filename().string().rfind("crash_", 0) == 0)
            return it->path();
    return {};
}

// Read the specimen's marker file (decimal WriteResult), or -1 if absent.
int read_marker(const fs::path& dir)
{
    const fs::path  marker = dir / "report.marker";
    std::error_code ec;
    if (!fs::exists(marker, ec))
        return -1;
    std::ifstream f{marker};
    int           v = -1;
    f >> v;
    return v;
}
} // namespace

TEST_CASE("crash capture: a wild write terminates with the fault code and writes exactly one dump",
          "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("av");
    const cd::Outcome o   = run_mode("av", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);             // ASan's VEH owns the AV: still fatal...
        CHECK(count_crash_dumps(dir) == 0U); // ...but our SEH handler is preempted, so no crd dump
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);                           // the process actually died
        CHECK(static_cast<std::uint32_t>(o.exit_code) == kAccessViolation); // and the exit retains the fault reason
        CHECK(count_crash_dumps(dir) == 1U);                                // one fault -> one dump (the handler ran)

        // The real fatal-filter dump (not a simulated one) is readable and correctly identifies the fault and the
        // crashing specimen binary: the exception code round-trips and the module list names the specimen with an
        // RSDS CV record -- the identity later symbolization (DIAG.5d) matches or rejects.
        const fs::path dump = first_crash_dump(dir);
        REQUIRE_FALSE(dump.empty());
        CHECK(crd_test_minidump::exception_code(dump.wstring()) == kAccessViolation);
        const std::wstring want = fs::path{CRD_DIAG_CRASH_CAPTURE_SPECIMEN}.filename().wstring();
        CHECK(crd_test_minidump::names_module_with_cv(dump.wstring(), want));
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture: concurrent faults write exactly one dump (the single-shot gate)",
          "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("concurrent");
    const cd::Outcome o   = run_mode("concurrent", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
        CHECK(count_crash_dumps(dir) == 0U);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(static_cast<std::uint32_t>(o.exit_code) == kAccessViolation);
        CHECK(count_crash_dumps(dir) == 1U); // two concurrent faults, still exactly one dump per process
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture: fail-fast bypasses the handler -- 0xC0000409 and no dump", "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("fastfail");
    const cd::Outcome o   = run_mode("fastfail", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    // __fastfail bypasses SEH *and* ASan's VEH, so this mode is identical on both presets.
    CHECK(o.verdict == cd::Verdict::Crashed);
    CHECK(static_cast<std::uint32_t>(o.exit_code) == kFailFast); // the fail-fast code is retained
    CHECK(count_crash_dumps(dir) == 0U);                         // SEH is bypassed; no dump can be claimed

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture: a denied output path fails the write, still terminates honestly",
          "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("denied");
    const cd::Outcome o   = run_mode("denied", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0); // ASan owns the AV; the write path is not reached, but the fault is still fatal
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(static_cast<std::uint32_t>(o.exit_code) == kAccessViolation);
    }
    CHECK_FALSE(fs::exists(dir)); // the specimen removed the dir; no dump, no partial resurrected it

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture: a fatal assert with no handler under headless terminates promptly, no modal dialog",
          "[core][diag][crash-capture]")
{
    // DIAG.5c (b): crd_diag_harden() declared the spawned specimen headless, so the DEFAULT assert path (no platform
    // handler) must terminate promptly instead of blocking on MessageBox. On Windows abort() -> prompt exit 3, no
    // dialog (a blocked dialog would surface as a Timeout verdict), and no SEH minidump (abort does not reach the
    // last-chance filter -- the emergency-channel routing for a fatal assert is a Linux property here).
    const fs::path    dir = fresh_dir("assert_default");
    const cd::Outcome o   = run_mode("assert_default", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    // The (b) guarantee on both builds: no modal dialog blocked the process to the harness timeout.
    CHECK(o.verdict != cd::Verdict::Timeout);
    if (built_with_asan())
    {
        // ASan intercepts abort() and owns the process exit (its own report + exit code), so the exact code is not
        // crd's; assert only that the process terminated abnormally -- the same scoping the other modes use under ASan.
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.exit_code == 3);             // abort()'s CRT exit code on win-debug
        CHECK(count_crash_dumps(dir) == 0U); // abort does not route to the SEH minidump path on Windows
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture: a fault while the faulter holds the stderr lock still dumps (lock-free fatal output)",
          "[core][diag][crash-capture]")
{
    // The regression guard for the lock-free stderr fix: the faulting thread holds the CRT stderr stream lock, so a
    // handler that printed via fprintf(stderr) would block until the 30s HandlerTimeout (harness -> Timeout). With
    // WriteFile on the fatal path the dump completes normally.
    const fs::path    dir = fresh_dir("logger_lock");
    const cd::Outcome o   = run_mode("logger_lock", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    if (built_with_asan())
    {
        // ASan owns the AV and writes via its own path, not the CRT lock -> fatal, no crd dump, and NOT a timeout.
        CHECK(o.exit_code != 0);
        CHECK_FALSE(o.timed_out);
        CHECK(count_crash_dumps(dir) == 0U);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK_FALSE(o.timed_out);                                           // no 30s hang: the fix removed the lock
        CHECK(static_cast<std::uint32_t>(o.exit_code) == kAccessViolation);
        CHECK(count_crash_dumps(dir) == 1U);                               // the dump still completed
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture: a stack overflow on the installing thread still dumps", "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("overflow");
    const cd::Outcome o   = run_mode("overflow", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0); // ASan reports the stack-overflow itself
        CHECK(count_crash_dumps(dir) == 0U);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(static_cast<std::uint32_t>(o.exit_code) == kStackOverflow);
        CHECK(count_crash_dumps(dir) == 1U); // install()'s stack guarantee left room for the filter + hand-off
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture: a stack overflow on an UNguarded worker thread still dumps", "[core][diag][crash-capture]")
{
    // Measured: because the dump runs on the dedicated handler thread's fresh stack, the overflowed faulting thread
    // only needs room for the tiny filter hand-off (CAS + two stores + SetEvent + wait), which the OS's default
    // guard-page slack covers. So a worker thread dumps even WITHOUT SetThreadStackGuarantee -- the guarantee is
    // defensive, not required for dumpability (this is why worker-loop guard wiring is optional in a later slice).
    const fs::path    dir = fresh_dir("overflow_thread");
    const cd::Outcome o   = run_mode("overflow_thread", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
        CHECK(count_crash_dumps(dir) == 0U);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(static_cast<std::uint32_t>(o.exit_code) == kStackOverflow);
        CHECK(count_crash_dumps(dir) == 1U);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture: a stack overflow on a guarded worker thread still dumps", "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("overflow_thread_guarded");
    const cd::Outcome o   = run_mode("overflow_thread_guarded", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
        CHECK(count_crash_dumps(dir) == 0U);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(static_cast<std::uint32_t>(o.exit_code) == kStackOverflow);
        CHECK(count_crash_dumps(dir) == 1U); // guard_current_thread_stack() reserved the last-chance stack
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture: a fault inside a fiber job is NOT caught by the last-chance filter (measured limitation)",
          "[core][diag][crash-capture]")
{
    // Measured (3/3): a fault raised while a worker runs on a FIBER stack terminates the process (0xC0000005) but is
    // NOT delivered to SetUnhandledExceptionFilter -- our handler never runs, no dump, no marker. jobs installs no
    // SEH/VEH of its own (grep-confirmed), so this is inherent: a last-chance filter does not reliably fire for a
    // fault on a fiber stack (the same reason main_diag installs a VEH for its own runs). Capturing fiber/CET faults
    // needs a VEH, which the owned crash API does not install by default (a general VEH would fire on every C++
    // exception). This is the honest limitation, asserted like fail-fast -- not a 5a acceptance item; a VEH-based
    // path is future work. (d)'s hang-triggered dump of a fiber pool still works because it dumps from a healthy
    // non-fault thread.
    const fs::path    dir = fresh_dir("fiber");
    const cd::Outcome o   = run_mode("fiber", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code)
                    << " marker=" << read_marker(dir));

    CHECK(o.exit_code != 0);             // the fault is fatal on every build
    CHECK(count_crash_dumps(dir) == 0U); // ...but the last-chance filter does not capture a fiber-stack fault
    if (!built_with_asan())
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(static_cast<std::uint32_t>(o.exit_code) == kAccessViolation);
        CHECK(read_marker(dir) == -1); // the hook never fired -> the handler genuinely did not run
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

#if CRD_ENABLE_ASSERTS
TEST_CASE("crash capture: an injected MiniDumpWriteDump failure is honest on the fatal path",
          "[core][diag][crash-capture]")
{
    // The acceptance names MiniDumpWriteDump *itself* failing. The specimen injects a forced FALSE, then faults; the
    // marker the hook drops proves the handler RAN and saw DumpFailed -- so "zero dumps" is an honest write failure,
    // not a handler that never fired. (Also the machine-readable form of "failed MiniDumpWriteDump cannot print
    // success": the success line is emitted only under wr == Ok, and the hook here saw DumpFailed.)
    const fs::path    dir = fresh_dir("inject_dump_fail");
    const cd::Outcome o   = run_mode("inject_dump_fail", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code)
                    << " marker=" << read_marker(dir));

    if (built_with_asan())
    {
        // ASan's VEH preempts the AV -> our handler never runs -> no marker at all.
        CHECK(o.exit_code != 0);
        CHECK(read_marker(dir) == -1);
        CHECK(count_crash_dumps(dir) == 0U);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(static_cast<std::uint32_t>(o.exit_code) == kAccessViolation);
        CHECK(count_crash_dumps(dir) == 0U);                                            // the forced failure wrote none
        CHECK(read_marker(dir) == static_cast<int>(crd::crash::WriteResult::DumpFailed)); // ...and the handler saw it
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif // CRD_ENABLE_ASSERTS

#endif // defined(_WIN32)

// ---------------------------------------------------------------------------
// Linux crash capture (DIAG.5b). The artifact is an async-signal-safe crash_*.log RECORD (not a minidump); the fatal
// reason is a signal, so the parent exit code is 128 + signo; capture is per-OS-thread via sigaction + SA_ONSTACK.
// Every expectation below was MEASURED against the built specimen before being asserted -- several differ from the
// Windows results and are called out inline (notably `fiber` is captured here, and `fastfail` -> SIGILL is captured).
// ASan builds are handled as in the Windows block: on Linux crd's sigaction replaces ASan's own SIGSEGV handler, so
// the exact records under ASan are uncharacterized (there is no linux-asan tree here) -- that branch asserts only that
// the fault stays fatal, never skip-passes, and the full record assertions run on the linux-gcc-debug lane. Out of
// scope, tracked as (e2) in the 5b session doc: inject_dump_fail + the marker are Windows-only (the Linux writer does
// not honor the injection seam), and abort/SIGFPE are not yet specimen modes.
#if defined(__linux__)

namespace
{
constexpr int kExitSegv   = 139; // 128 + SIGSEGV(11): a wild write or a stack fault
constexpr int kExitIll    = 132; // 128 + SIGILL(4):  __builtin_trap()
constexpr int kExitAbrt   = 134; // 128 + SIGABRT(6): abort() from the headless assert path
constexpr int kSigSegv    = 11;
constexpr int kSigIll     = 4;
constexpr int kSigAbrt    = 6;
constexpr int kSegvAccerr = 2; // si_code SEGV_ACCERR = a guard-page hit (stack overflow); SEGV_MAPERR(1) = unmapped

struct RecordFields
{
    long        pid    = -1;
    long        tid    = -1;
    int         signal = -1;
    int         code   = -1;
    std::string addr;             // si_addr as written ("0x0" for a null write)
    std::string exe;              // the "correctly identified binary" (its path)
    std::string arch;             // the token after "regs" (e.g. "x86_64")
    bool        has_regs = false; // the register dump is present
    bool        has_cr2  = false; // completeness: the last register line reached (not truncated) on x86_64
    bool        ok       = false;
};

// Count and size-check the async-signal-safe crash records (prefix crash_, ext .log) the handler wrote into dir.
std::size_t count_crash_records(const fs::path& dir)
{
    std::size_t     n = 0;
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
        if (it->path().extension() == ".log" && it->path().filename().string().rfind("crash_", 0) == 0)
        {
            ++n;
            CHECK(fs::file_size(it->path()) > 0U); // a written record is a real, non-empty file
        }
    return n;
}

// The first crash record in dir, or an empty path if none was written.
fs::path first_crash_record(const fs::path& dir)
{
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
        if (it->path().extension() == ".log" && it->path().filename().string().rfind("crash_", 0) == 0)
            return it->path();
    return {};
}

// Parse the record. The header fields (pid/tid/signal/code/addr/exe) are read only BEFORE the "regs" sentinel, so a
// bare keyword that ever appears inside the register dump cannot overwrite a header value; after "regs" we only note
// the dump is present and that its final "cr2" line was reached (a completeness/truncation check on x86_64).
RecordFields parse_record(const fs::path& p)
{
    RecordFields  r;
    std::ifstream f{p};
    std::string   tok;
    bool          in_regs = false;
    while (f >> tok)
    {
        if (tok == "regs")
        {
            in_regs    = true;
            r.has_regs = true;
            f >> r.arch; // "regs x86_64"
            continue;
        }
        if (!in_regs)
        {
            if (tok == "pid")
                f >> r.pid;
            else if (tok == "tid")
                f >> r.tid;
            else if (tok == "signal")
                f >> r.signal;
            else if (tok == "code")
                f >> r.code;
            else if (tok == "addr")
                f >> r.addr;
            else if (tok == "exe")
                f >> r.exe;
        }
        else if (tok == "cr2")
        {
            r.has_cr2 = true;
        }
    }
    r.ok = (r.pid >= 0 && r.tid >= 0 && r.signal >= 0 && r.code >= 0);
    return r;
}
} // namespace

TEST_CASE("crash capture (linux): a wild write terminates with SIGSEGV and writes one record",
          "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("av");
    const cd::Outcome o   = run_mode("av", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0); // ASan owns SIGSEGV on Linux; the fault stays fatal (records uncharacterized under ASan)
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitSegv);
        CHECK(count_crash_records(dir) == 1U);
        const RecordFields rf = parse_record(first_crash_record(dir));
        CHECK(rf.ok);
        CHECK(rf.signal == kSigSegv);
        CHECK(rf.pid == rf.tid);      // a main-thread fault: on Linux the main thread's tid equals the pid
        // Record read-back (the 5b analog of 5a's read_dump_stream check): the fault address and the correctly
        // identified binary are recovered from the on-disk record, and the register dump is present and complete.
        CHECK(rf.addr == "0x0");      // a wild null write -> si_addr == 0
        CHECK_FALSE(rf.exe.empty());
        CHECK(fs::path{rf.exe}.filename() == fs::path{specimen_path().c_str()}.filename()); // names this specimen
        CHECK(rf.has_regs);
        if (rf.arch == "x86_64")
            CHECK(rf.has_cr2);        // the final register line was reached: the record is not truncated (cr2 == si_addr)
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): a main-thread stack overflow is captured on the alternate signal stack",
          "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("overflow");
    const cd::Outcome o   = run_mode("overflow", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitSegv);
        CHECK(count_crash_records(dir) == 1U); // install() gave the main thread its alt stack (would be 0 without it)
        CHECK(parse_record(first_crash_record(dir)).signal == kSigSegv);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): an UNguarded worker stack overflow writes NO record (honest negative)",
          "[core][diag][crash-capture]")
{
    // The discriminator for the (d) wiring: a worker OS thread with no alternate signal stack cannot run the handler
    // when its stack is exhausted, so the fault is fatal but unrecorded. This is the "before" of the (d) proof, kept
    // as a standing negative -- it must NOT become a false positive if a worker ever gains an alt stack elsewhere.
    const fs::path    dir = fresh_dir("overflow_thread");
    const cd::Outcome o   = run_mode("overflow_thread", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitSegv);
        CHECK(fs::exists(dir));                 // the output dir survives (unlike `denied`)...
        CHECK(count_crash_records(dir) == 0U);  // ...but no record: the handler never ran on the unguarded worker
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): a GUARDED worker stack overflow is captured with tid != pid",
          "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("overflow_thread_guarded");
    const cd::Outcome o   = run_mode("overflow_thread_guarded", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitSegv);
        CHECK(count_crash_records(dir) == 1U);
        const RecordFields rf = parse_record(first_crash_record(dir));
        CHECK(rf.ok);
        CHECK(rf.signal == kSigSegv);
        CHECK(rf.tid != rf.pid);          // captured on a real worker OS thread, not the main thread
        CHECK(rf.code == kSegvAccerr);    // a guard-page hit, i.e. a genuine stack overflow
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): a fiber-stack overflow on a background worker is captured (DIAG.5b (d))",
          "[core][diag][crash-capture]")
{
    // The (d) end-to-end proof: main does not pump, so the job runs on a background worker whose fiber stack
    // overflows; worker_loop's guard_current_thread_stack() gives that worker the alt stack that captures it.
    const fs::path    dir = fresh_dir("fiber_overflow_worker");
    const cd::Outcome o   = run_mode("fiber_overflow_worker", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitSegv);
        CHECK(count_crash_records(dir) == 1U);
        const RecordFields rf = parse_record(first_crash_record(dir));
        CHECK(rf.ok);
        CHECK(rf.signal == kSigSegv);
        CHECK(rf.tid != rf.pid);       // the carry-forward assertion: proves a worker ran it, not main via pump()
        CHECK(rf.code == kSegvAccerr); // guard-page hit on the fiber stack
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): a fiber null-deref pumped onto main is captured (opposite of Windows)",
          "[core][diag][crash-capture]")
{
    // On Windows a fiber-stack fault bypasses SetUnhandledExceptionFilter and is NOT captured (needs a VEH). On Linux
    // the per-thread signal handler captures it: jobs::wait() pumps the job onto the guarded main thread, so the fault
    // lands there (pid == tid). Measured -- the platforms genuinely differ here.
    const fs::path    dir = fresh_dir("fiber");
    const cd::Outcome o   = run_mode("fiber", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitSegv);
        CHECK(count_crash_records(dir) == 1U);
        CHECK(parse_record(first_crash_record(dir)).signal == kSigSegv);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): two threads faulting at once write exactly ONE record (single-shot gate)",
          "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("concurrent");
    const cd::Outcome o   = run_mode("concurrent", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitSegv);
        CHECK(count_crash_records(dir) == 1U); // the winner/loser gate: exactly one record under concurrent faults
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): a denied output path fails the write, still terminates honestly",
          "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("denied");
    const cd::Outcome o   = run_mode("denied", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitSegv);
    }
    CHECK_FALSE(fs::exists(dir)); // the specimen rmdir'd the output dir; no record, no partial resurrected it

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): __builtin_trap raises SIGILL and is captured (unlike Windows __fastfail)",
          "[core][diag][crash-capture]")
{
    // Measured platform difference: Windows __fastfail bypasses SEH (0 dumps); on Linux __builtin_trap raises SIGILL,
    // which crd's handler installs for, so it is captured. Exit code is 128 + SIGILL.
    const fs::path    dir = fresh_dir("fastfail");
    const cd::Outcome o   = run_mode("fastfail", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitIll);
        CHECK(count_crash_records(dir) == 1U);
        CHECK(parse_record(first_crash_record(dir)).signal == kSigIll);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): SIGKILL writes no record and retains a prior one (uncatchable termination)",
          "[core][diag][crash-capture]")
{
    // The acceptance's SIGKILL/OOM clause: an uncatchable signal runs no handler, so no final capture is made, and any
    // previously written record must be retained. kill(getpid, SIGKILL) is that mechanism deterministically (no need to
    // actually exhaust memory). Not ASan-guarded: SIGKILL is uncatchable on every build.
    const fs::path  dir = fresh_dir("sigkill");
    std::error_code ec;
    fs::create_directories(dir);
    const fs::path seed = dir / "crash_seed_prior_0.log"; // a prior record, in the crash_*.log namespace
    {
        std::ofstream f{seed};
        f << "crd crash record\n(prior)\n";
    }

    const cd::Outcome o = run_mode("sigkill", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    CHECK(o.verdict == cd::Verdict::Crashed);
    CHECK(o.exit_code == 137);             // 128 + SIGKILL(9)
    CHECK(fs::exists(seed));               // the prior record is retained...
    CHECK(count_crash_records(dir) == 1U); // ...and no new record was written (the handler never ran)

    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): a fault holding the stdio lock still records (write(2)-only handler)",
          "[core][diag][crash-capture]")
{
    // logger/allocator lock ownership: the specimen holds the stdio stream lock (flockfile) then faults. The handler
    // records with write(2) only (no fprintf/fputs -- grep-confirmed in crash.cpp's Linux block), so the held lock
    // cannot deadlock it. (On Linux flockfile is also recursive and the handler runs on the faulting thread, so a
    // same-thread stdio call would not self-deadlock anyway; the real guarantee is the write(2)-only handler.)
    const fs::path    dir = fresh_dir("logger_lock");
    const cd::Outcome o   = run_mode("logger_lock", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitSegv);
        CHECK(count_crash_records(dir) == 1U);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): crd records then chains to the previous handler (signal chaining)",
          "[core][diag][crash-capture]")
{
    // Signal chaining asserted, not just implemented: a prior SIGSEGV handler is installed before crd::install(); crd
    // records, then restores + re-raises, so the prior handler runs next and drops chain.marker.
    const fs::path    dir = fresh_dir("chain");
    const cd::Outcome o   = run_mode("chain", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitSegv);
        CHECK(count_crash_records(dir) == 1U);      // crd wrote its record...
        CHECK(fs::exists(dir / "chain.marker"));    // ...then chained: the previously-installed handler ran
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): a faulting crash-report hook is bounded and the record survives (recursive fault)",
          "[core][diag][crash-capture]")
{
    // Recursive faults: the crash-report hook itself faults. The record is written BEFORE the hook fires, so it must
    // survive; the recursive fault is bounded by the handler's signal mask (forced to SIG_DFL), so the process
    // terminates within the harness bound -- verdict Crashed, never Timeout.
    const fs::path    dir = fresh_dir("hook_fault");
    const cd::Outcome o   = run_mode("hook_fault", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed); // NOT Timeout: the recursive fault did not hang
        CHECK(o.exit_code == kExitSegv);
        CHECK(count_crash_records(dir) == 1U);    // the record was written before the hook faulted
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): a fatal assert with no handler under headless aborts and is recorded",
          "[core][diag][crash-capture]")
{
    // DIAG.5c (b): crd_diag_harden() declared the spawned specimen headless, so the DEFAULT assert path (no platform
    // handler) terminates via abort() -> SIGABRT instead of the interactive return-2 debug-trap. crd's crash handler
    // records the SIGABRT -- the failure is routed to the emergency channel, and the process does not hang.
    const fs::path    dir = fresh_dir("assert_default");
    const cd::Outcome o   = run_mode("assert_default", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (built_with_asan())
    {
        CHECK(o.exit_code != 0);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitAbrt);
        CHECK(count_crash_records(dir) == 1U);
        CHECK(parse_record(first_crash_record(dir)).signal == kSigAbrt);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#endif // defined(__linux__)

