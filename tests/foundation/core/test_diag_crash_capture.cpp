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

// This slice is Windows crash capture; the acceptance codes are NTSTATUS values. Linux crash capture (its own
// signal codes) is DIAG.5b, so these cases are Windows-only -- not a skip, a different slice.
#if defined(_WIN32)

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

cd::Outcome run_mode(const char* mode, const fs::path& dir)
{
    cont::Array<cont::String> args;
    args.push_back(cont::String{dir.string().c_str()});
    args.push_back(cont::String{mode});
    cd::Expectation e;
    e.want       = cd::Expectation::Want::Crash;
    e.timeout_ms = 15000U; // a cold DbgHelp self-dump is not instant
    return cd::run_specimen(specimen_path(), args, e);
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

