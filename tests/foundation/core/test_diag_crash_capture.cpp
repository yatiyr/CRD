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
#include <crd/core/crash.hpp>    // WriteResult, for the injected-failure marker assertion
#include <crd/core/platform.hpp> // CRD_ARCH_ARM64

#include "minidump_probe.hpp" // Windows-only dump content probe (no-op off Windows)

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib> // strtoul
#include <cstring> // memcmp
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string_view>
#if defined(__linux__)
#include <cfenv> // feenableexcept, fegetexcept, fedisableexcept (the FPU trap probe)
#endif

namespace cont = crd::containers;
namespace cd   = crd::diag;
namespace fs   = std::filesystem;

// Platform-neutral helpers, shared by the Windows and Linux acceptance blocks below. The specimen path, a fresh
// output dir and the bounded harness invocation are identical across platforms, and both judge a sanitizer by the
// lane's promise (cd::lane_promise()); only the on-disk artifact (a Windows .dmp minidump vs a Linux crash_*.log
// record) and the fatal-reason codes differ.
namespace
{
cont::String specimen_path()
{
    return cont::String{CRD_DIAG_CRASH_CAPTURE_SPECIMEN};
}

std::atomic<unsigned> g_uniq{0};

fs::path fresh_dir(const char* mode)
{
    char leaf[96];
    (void)std::snprintf(leaf, sizeof(leaf), "crd_cc_%s_%u", mode, g_uniq.fetch_add(1U));
    fs::path p = fs::temp_directory_path() / leaf;
    std::error_code ec;
    fs::remove_all(p, ec); // clean slate; the specimen's install() creates it
    return p;
}

cd::Outcome run_mode(const char* mode, const fs::path& dir, const cd::Expectation& e)
{
    cont::Array<cont::String> args;
    args.push_back(cont::String{dir.string().c_str()});
    args.push_back(cont::String{mode});
    return cd::run_specimen(specimen_path(), args, e);
}

cd::Outcome run_mode(const char* mode, const fs::path& dir)
{
    cd::Expectation e;
    e.want       = cd::Expectation::Want::Crash;
    e.timeout_ms = 15000U; // a cold DbgHelp self-dump is not instant (Linux records are fast; the bound is shared)
    return run_mode(mode, dir, e);
}

// Read the specimen's marker file (decimal WriteResult), or -1 if absent. Windows reads it in every configuration;
// Linux reads it only in the injected-write-failure case, which needs asserts.
#if defined(_WIN32) || (defined(__linux__) && CRD_ENABLE_ASSERTS)
int read_marker(const fs::path& dir)
{
    const fs::path  marker = dir / "report.marker";
    std::error_code ec;
    if (!fs::exists(marker, ec))
    {
        return -1;
    }
    std::ifstream f{marker};
    int           v = -1;
    f >> v;
    return v;
}
#endif // defined(_WIN32) || (defined(__linux__) && CRD_ENABLE_ASSERTS)

#if defined(_WIN32) || defined(__linux__)
// The machine stream every specimen mode sets (crash_capture_specimen.cpp's main): byte i = (i * 37 + 11) mod 256.
cont::Array<unsigned char> specimen_machine()
{
    cont::Array<unsigned char> bytes;
    for (std::uint32_t i = 0U; i < crd::crash::kMaxMachineStreamBytes; ++i)
    {
        bytes.push_back(static_cast<unsigned char>((i * 37U + 11U) & 0xFFU));
    }
    return bytes;
}
#endif
} // namespace

// Windows crash capture; the acceptance codes are NTSTATUS values. The Linux acceptance block (its own signal codes)
// follows after this #endif -- not a skip, a different platform mechanism (both are DIAG.5b's remit on Linux).
#if defined(_WIN32)

namespace
{
// Count and size-check the dumps with this file-name prefix (crash_ fatal, live_ capture_dump) in dir.
std::size_t count_dumps(const fs::path& dir, std::string_view prefix)
{
    std::size_t     n = 0;
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
    {
        if (it->path().extension() == ".dmp" && it->path().filename().string().starts_with(prefix))
        {
            ++n;
            CHECK(fs::file_size(it->path()) > 0U); // a reported dump is a real, non-empty file
        }
    }
    return n;
}

std::size_t count_crash_dumps(const fs::path& dir)
{
    return count_dumps(dir, "crash_");
}

constexpr std::uint32_t kAccessViolation = 0xC0000005U;
constexpr std::uint32_t kFailFast        = 0xC0000409U;
constexpr std::uint32_t kStackOverflow   = 0xC00000FDU;

// The first fatal-path dump (prefix crash_) in dir, or an empty path if none was written.
fs::path first_crash_dump(const fs::path& dir)
{
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
    {
        if (it->path().extension() == ".dmp" && it->path().filename().string().starts_with("crash_"))
        {
            return it->path();
        }
    }
    return {};
}
} // namespace

TEST_CASE("crash capture: a wild write terminates with the fault code and writes exactly one dump",
          "[core][diag][crash-capture][diag-workflow]")
{
    const fs::path    dir = fresh_dir("av");
    const cd::Outcome o   = run_mode("av", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    if (cd::lane_promise().asan)
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
        // crashing specimen binary: the exception code round-trips and the module list names the specimen, with an
        // RSDS CV record exactly when the binary has one -- the identity later symbolization (DIAG.5d) matches or
        // rejects. win-release links without /DEBUG, so there the binary and its dump correctly carry none.
        const fs::path dump = first_crash_dump(dir);
        REQUIRE_FALSE(dump.empty());
        CHECK(crd_test_minidump::exception_code(dump) == kAccessViolation);
        const fs::path want = fs::path{CRD_DIAG_CRASH_CAPTURE_SPECIMEN}.filename();
        const crd_test_minidump::DumpModule module = crd_test_minidump::find_dump_module(dump, want.c_str());
        CHECK(module.named);
        CHECK(module.rsds == crd_test_minidump::image_has_rsds(fs::path{CRD_DIAG_CRASH_CAPTURE_SPECIMEN}.c_str()));
        // HW-RESILIENCE.2: the fatal dump carries the whole machine stream the specimen set.
        const cont::Array<unsigned char> want_machine = specimen_machine();
        cont::Array<unsigned char>       machine;
        machine.resize(want_machine.size() + 1U);
        const std::size_t machine_bytes =
            crd_test_minidump::read_stream(dump, crd::crash::kMachineStreamType, machine.data(), machine.size());
        REQUIRE(machine_bytes == want_machine.size());
        CHECK(std::memcmp(machine.data(), want_machine.data(), machine_bytes) == 0);
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

    if (cd::lane_promise().asan)
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

    if (cd::lane_promise().asan)
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

TEST_CASE("crash capture: a fatal assert with no handler under headless terminates promptly and dumps the aborting "
          "thread",
          "[core][diag][crash-capture]")
{
    // DIAG.5c (b): crd_diag_harden() declared the spawned specimen headless, so the DEFAULT assert path (no platform
    // handler) must terminate promptly instead of blocking on MessageBox (a blocked dialog would surface as a Timeout
    // verdict). abort() never reaches the last-chance filter; crd's SIGABRT handler dumps the aborting thread through
    // the fatal path (the emergency channel), then abort() exits 3.
    const fs::path    dir = fresh_dir("assert_default");
    const cd::Outcome o   = run_mode("assert_default", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    // The (b) guarantee: no modal dialog blocked the process to the harness timeout. Measured on win-debug and
    // win-asan alike: ASan's Windows runtime leaves abort() to the CRT, so the dump and the exit are crd's there too.
    CHECK(o.verdict != cd::Verdict::Timeout);
    CHECK(o.exit_code == 3); // abort()'s CRT exit code, after the dump
    REQUIRE(count_crash_dumps(dir) == 1U);
    const fs::path dump = first_crash_dump(dir);
    CHECK(crd_test_minidump::exception_code(dump) == crd::crash::kAbortExceptionCode);
    const std::string_view out{o.output};
    const std::size_t      at = out.find("CRD_ABORT_TID=");
    REQUIRE(at != std::string_view::npos);
    const auto aborting_tid = static_cast<std::uint32_t>(std::strtoul(out.data() + at + 14U, nullptr, 10));
    CHECK(crd_test_minidump::exception_thread_id(dump) == aborting_tid); // the aborting thread, not the writer
    const fs::path want = fs::path{CRD_DIAG_CRASH_CAPTURE_SPECIMEN}.filename();
    CHECK(crd_test_minidump::find_dump_module(dump, want.c_str()).named);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture: a fault inside a log sink, holding the log and stderr locks, still dumps",
          "[core][diag][crash-capture]")
{
    // The specimen faults inside a crd::log sink's write, holding the logger's sink mutex and the CRT stderr stream
    // lock, so a handler that logged or printed via fprintf(stderr) would block until the 30s HandlerTimeout
    // (harness -> Timeout). The dump is written with WriteFile and no lock, so it completes normally.
    const fs::path    dir = fresh_dir("logger_lock");
    const cd::Outcome o   = run_mode("logger_lock", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    if (cd::lane_promise().asan)
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

// DbgHelp dumps a PSS snapshot of the process (crash.cpp write_dump). On the live process it keeps every other thread
// suspended through the write, and a thread suspended inside the loader lock or the process heap's lock, both of which
// DbgHelp takes, deadlocks it: a contender looping on both hung every such dump on this host (2026-10-30).
TEST_CASE("crash capture: live dumps complete while a thread keeps taking the loader and heap locks",
          "[core][diag][crash-capture]")
{
    const fs::path  dir = fresh_dir("live_churn");
    cd::Expectation e;
    e.want       = cd::Expectation::Want::CleanExit;
    e.timeout_ms = 60000U; // four live dumps of a sanitized process; a deadlocked writer never finishes
    const cd::Outcome o = run_mode("live_churn", dir, e);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    CHECK(o.verdict == cd::Verdict::Clean);
    CHECK_FALSE(o.timed_out);
    CHECK(o.exit_code == 0);
    CHECK(count_dumps(dir, "live_") == 4U);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture: a fault while a thread keeps taking the loader and heap locks still dumps",
          "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("churn_av");
    const cd::Outcome o   = run_mode("churn_av", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    if (cd::lane_promise().asan)
    {
        CHECK(o.exit_code != 0); // ASan's VEH owns the AV: fatal, no crd dump, and not a hang
        CHECK_FALSE(o.timed_out);
        CHECK(count_crash_dumps(dir) == 0U);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK_FALSE(o.timed_out); // the fatal dump did not wait out the 30 s HandlerTimeout
        CHECK(static_cast<std::uint32_t>(o.exit_code) == kAccessViolation);
        REQUIRE(count_crash_dumps(dir) == 1U);
        // The snapshot keeps the exception stream and the module list.
        const fs::path dump = first_crash_dump(dir);
        CHECK(crd_test_minidump::exception_code(dump) == kAccessViolation);
        const fs::path want = fs::path{CRD_DIAG_CRASH_CAPTURE_SPECIMEN}.filename();
        CHECK(crd_test_minidump::find_dump_module(dump, want.c_str()).named);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture: a stack overflow on the installing thread still dumps", "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("overflow");
    const cd::Outcome o   = run_mode("overflow", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code));

    if (cd::lane_promise().asan)
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

TEST_CASE("crash capture: a stack overflow on a raw thread created after install() dumps",
          "[core][diag][crash-capture]")
{
    // The specimen's raw std::thread never calls guard_current_thread_stack. It used to rely on the OS's default
    // slack (about one page) for the exception dispatcher and the filter's hand-off, which an earlier comment called
    // a measured guarantee. Hosted Windows Server 2025 refuted it on 2026-10-04: exit 0xC00000FD with no dump on
    // MSVC win-debug, 0xC0000005 on clang-cl, while the workstation passed 30 of 30. crash.cpp now reserves the
    // last-chance guarantee for every thread created after install() through a TLS callback, so this thread is
    // guaranteed by construction and the dump no longer depends on slack. The marker shows how a failure happened:
    // -1 means the filter never ran, any other value is the WriteResult the handler reported.
    const fs::path    dir = fresh_dir("overflow_thread");
    const cd::Outcome o   = run_mode("overflow_thread", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code)
                    << std::dec << " marker=" << read_marker(dir));

    if (cd::lane_promise().asan)
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
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=0x" << std::hex << static_cast<std::uint32_t>(o.exit_code)
                    << std::dec << " marker=" << read_marker(dir));

    if (cd::lane_promise().asan)
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

#if CRD_DIAG_FIBER_MODES
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
    if (!cd::lane_promise().asan)
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(static_cast<std::uint32_t>(o.exit_code) == kAccessViolation);
        CHECK(read_marker(dir) == -1); // the hook never fired -> the handler genuinely did not run
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif

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

    if (cd::lane_promise().asan)
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
// Sanitizer lanes, measured on linux-gcc-asan (DIAG.12) and judged by the lane's promise: UBSan's null check stops a
// null store before it executes, so the null-store modes end in its report (exit 1) and crd's handler never runs;
// a fault the kernel raises reaches crd first, which records it and returns, so the instruction faults again under
// ASan's own handler (exit 1, its report names the real fault). SIGILL and SIGABRT are not ASan's (handle_sigill and
// handle_abort are off), so those modes match the plain lanes.
#if defined(__linux__)

namespace
{
// __builtin_trap is ud2 on x64 (SIGILL, 4) and brk on AArch64 (SIGTRAP, 5).
constexpr int kSigTrapInstruction = CRD_ARCH_ARM64 ? 5 : 4;
constexpr int kExitSegv           = 139; // 128 + SIGSEGV(11): a wild write or a stack fault
constexpr int kExitTrap           = 128 + kSigTrapInstruction; // __builtin_trap()
constexpr int kExitAbrt           = 134; // 128 + SIGABRT(6): abort() from the headless assert path
constexpr int kExitBus            = 135; // 128 + SIGBUS(7): a read past the end of a mapped file
constexpr int kExitFpe            = 136; // 128 + SIGFPE(8): a trapped floating-point division by zero
constexpr int kExitNoFpTrap       = 62;  // the specimen's exit when the FPU cannot arm the division trap
constexpr int kExitReport         = 1;   // a sanitizer's exit after its report (no exitcode option is set)
constexpr int kSigSegv            = 11;
constexpr int kSigAbrt            = 6;
constexpr int kSigBus             = 7;
constexpr int kSigFpe             = 8;
constexpr int kSegvAccerr         = 2;  // si_code SEGV_ACCERR = a guard-page hit (stack overflow); MAPERR(1) = unmapped
constexpr int kBusAdrerr          = 2;  // si_code BUS_ADRERR: no backing page at the address
constexpr int kFpeFltdiv          = 3;  // si_code FPE_FLTDIV
constexpr int kSiTkill            = -6; // si_code SI_TKILL: a sent signal (abort raises SIGABRT this way)

// Whether this host's FPU can trap a division by zero. ARMv8 makes the trap enables optional and the hosted ARM64
// cores implement none, which feenableexcept reports. The probe restores the enables it found.
bool fp_division_traps()
{
    const int  found = fegetexcept();
    const bool armed = feenableexcept(FE_DIVBYZERO) != -1 && (fegetexcept() & FE_DIVBYZERO) != 0;
    (void)fedisableexcept(FE_DIVBYZERO & ~found);
    return armed;
}

struct RecordFields
{
    long        pid    = -1;
    long        tid    = -1;
    int         signal = -1;
    int         code   = -1;
    cont::String addr;            // si_addr as written ("0x0" for a null write)
    cont::String exe;             // the "correctly identified binary" (its path)
    cont::String arch;            // the token after "regs" (e.g. "x86_64")
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
    {
        if (it->path().extension() == ".log" && it->path().filename().string().starts_with("crash_"))
        {
            ++n;
            CHECK(fs::file_size(it->path()) > 0U); // a written record is a real, non-empty file
        }
    }
    return n;
}

// The first crash record in dir, or an empty path if none was written.
fs::path first_crash_record(const fs::path& dir)
{
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
    {
        if (it->path().extension() == ".log" && it->path().filename().string().starts_with("crash_"))
        {
            return it->path();
        }
    }
    return {};
}

// Parse the record. The header fields (pid/tid/signal/code/addr/exe) are read only BEFORE the "regs" sentinel, so a
// bare keyword that ever appears inside the register dump cannot overwrite a header value; after "regs" we only note
// the dump is present and that its final "cr2" line was reached (a completeness/truncation check on x86_64).
// One whitespace-delimited token into a bounded buffer (the record's longest token is the exe path).
bool read_token(std::ifstream& f, char (&tok)[4096])
{
    return static_cast<bool>(f >> std::setw(static_cast<int>(sizeof(tok))) >> tok);
}

bool read_field(std::ifstream& f, cont::String& out)
{
    char tok[4096];
    if (!read_token(f, tok))
    {
        return false;
    }
    out = std::string_view{tok};
    return true;
}

RecordFields parse_record(const fs::path& p)
{
    RecordFields  r;
    std::ifstream f{p};
    char          tok[4096];
    bool          in_regs = false;
    while (read_token(f, tok))
    {
        const std::string_view t{tok};
        if (t == "regs")
        {
            in_regs    = true;
            r.has_regs = true;
            (void)read_field(f, r.arch); // "regs x86_64"
            continue;
        }
        if (!in_regs)
        {
            if (t == "pid")
            {
                f >> r.pid;
            }
            else if (t == "tid")
            {
                f >> r.tid;
            }
            else if (t == "signal")
            {
                f >> r.signal;
            }
            else if (t == "code")
            {
                f >> r.code;
            }
            else if (t == "addr")
            {
                (void)read_field(f, r.addr);
            }
            else if (t == "exe")
            {
                (void)read_field(f, r.exe);
            }
        }
        else if (t == "cr2")
        {
            r.has_cr2 = true;
        }
    }
    r.ok = (r.pid >= 0 && r.tid >= 0 && r.signal >= 0 && r.has_regs);
    return r;
}

bool output_has(const cd::Outcome& o, std::string_view text)
{
    return std::string_view{o.output}.find(text) != std::string_view::npos;
}

// A null-store mode on a UBSan lane: the sanitizer's report and exit, no record.
void check_null_store_reported(const cd::Outcome& o, const fs::path& dir)
{
    CHECK(o.exit_code == kExitReport);
    CHECK(output_has(o, "runtime error: store to null pointer"));
    CHECK(count_crash_records(dir) == 0U);
}

// A kernel-raised fault on an ASan lane: crd's record, then ASan's report of the same fault and its exit.
void check_asan_chained(const cd::Outcome& o, const fs::path& dir, std::string_view report)
{
    CHECK(o.exit_code == kExitReport);
    CHECK(output_has(o, report));
    CHECK(count_crash_records(dir) == 1U);
}
} // namespace

TEST_CASE("crash capture (linux): a wild write terminates with SIGSEGV and writes one record",
          "[core][diag][crash-capture][diag-workflow]")
{
    const fs::path    dir = fresh_dir("av");
    const cd::Outcome o   = run_mode("av", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (cd::lane_promise().ubsan)
    {
        check_null_store_reported(o, dir);
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
        // names this specimen
        CHECK(fs::path{rf.exe.c_str()}.filename() == fs::path{specimen_path().c_str()}.filename());
        CHECK(rf.has_regs);
        if (rf.arch == "x86_64")
        {
            CHECK(rf.has_cr2);        // the final register line was reached: the record is not truncated (cr2 == si_addr)
        }
        // HW-RESILIENCE.2: the record ends with the whole machine stream the specimen set, as one hex line.
        cont::String want_line{"\nmachine "};
        for (const unsigned char b : specimen_machine())
        {
            constexpr char hex_digits[] = "0123456789abcdef";
            want_line.push_back(hex_digits[b >> 4U]);
            want_line.push_back(hex_digits[b & 0x0FU]);
        }
        want_line.push_back('\n');
        std::ifstream f{first_crash_record(dir), std::ios::binary};
        cont::String  record;
        for (char c = 0; f.get(c);)
        {
            record.push_back(c);
        }
        CHECK(std::string_view{record}.ends_with(std::string_view{want_line}));
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

    if (cd::lane_promise().asan)
    {
        check_asan_chained(o, dir, "AddressSanitizer: stack-overflow");
        CHECK(parse_record(first_crash_record(dir)).signal == kSigSegv);
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

    if (cd::lane_promise().asan)
    {
        // ASan gives every thread an alternate signal stack, which crd borrows, so this worker is captured.
        check_asan_chained(o, dir, "AddressSanitizer: stack-overflow");
        const RecordFields rf = parse_record(first_crash_record(dir));
        CHECK(rf.tid != rf.pid);
        CHECK(rf.code == kSegvAccerr);
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

    if (cd::lane_promise().asan)
    {
        check_asan_chained(o, dir, "AddressSanitizer: stack-overflow");
        const RecordFields rf = parse_record(first_crash_record(dir));
        CHECK(rf.tid != rf.pid);
        CHECK(rf.code == kSegvAccerr);
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

#if CRD_DIAG_FIBER_MODES
TEST_CASE("crash capture (linux): a fiber-stack overflow on a background worker is captured (DIAG.5b (d))",
          "[core][diag][crash-capture]")
{
    // The (d) end-to-end proof: main does not pump, so the job runs on a background worker whose fiber stack
    // overflows; worker_loop's guard_current_thread_stack() gives that worker the alt stack that captures it.
    const fs::path    dir = fresh_dir("fiber_overflow_worker");
    const cd::Outcome o   = run_mode("fiber_overflow_worker", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (cd::lane_promise().asan)
    {
        check_asan_chained(o, dir, "AddressSanitizer: stack-overflow");
        const RecordFields rf = parse_record(first_crash_record(dir));
        CHECK(rf.tid != rf.pid);
        CHECK(rf.code == kSegvAccerr);
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
#endif

#if CRD_DIAG_FIBER_MODES
TEST_CASE("crash capture (linux): a fiber null-deref pumped onto main is captured (opposite of Windows)",
          "[core][diag][crash-capture]")
{
    // On Windows a fiber-stack fault bypasses SetUnhandledExceptionFilter and is NOT captured (needs a VEH). On Linux
    // the per-thread signal handler captures it: jobs::wait() pumps the job onto the guarded main thread, so the fault
    // lands there (pid == tid). Measured -- the platforms genuinely differ here.
    const fs::path    dir = fresh_dir("fiber");
    const cd::Outcome o   = run_mode("fiber", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (cd::lane_promise().ubsan)
    {
        check_null_store_reported(o, dir);
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
#endif

TEST_CASE("crash capture (linux): two threads faulting at once write exactly ONE record (single-shot gate)",
          "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("concurrent");
    const cd::Outcome o   = run_mode("concurrent", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (cd::lane_promise().ubsan)
    {
        check_null_store_reported(o, dir);
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

    if (cd::lane_promise().ubsan)
    {
        check_null_store_reported(o, dir);
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

TEST_CASE("crash capture (linux): __builtin_trap raises its trap signal and is captured (unlike Windows __fastfail)",
          "[core][diag][crash-capture]")
{
    // Measured platform difference: Windows __fastfail bypasses SEH (0 dumps); on Linux __builtin_trap raises SIGILL
    // (x64's ud2) or SIGTRAP (AArch64's brk), both of which crd's handler installs for, so it is captured. Exit code is
    // 128 + that signal.
    const fs::path    dir = fresh_dir("fastfail");
    const cd::Outcome o   = run_mode("fastfail", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    CHECK(o.verdict == cd::Verdict::Crashed);
    CHECK(o.exit_code == kExitTrap);
    CHECK(count_crash_records(dir) == 1U);
    CHECK(parse_record(first_crash_record(dir)).signal == kSigTrapInstruction);
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

TEST_CASE("crash capture (linux): a fault inside a log sink, holding the log and stdio locks, still records",
          "[core][diag][crash-capture]")
{
    // Logger lock ownership: the specimen faults inside a crd::log sink's write, holding the logger's sink mutex and
    // the stdio stream lock (flockfile). The handler runs on the faulting thread and records with write(2) only, so
    // neither held lock can deadlock it; a handler that logged would block on the non-recursive sink mutex.
    const fs::path    dir = fresh_dir("logger_lock");
    const cd::Outcome o   = run_mode("logger_lock", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (cd::lane_promise().ubsan)
    {
        check_null_store_reported(o, dir);
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

    if (cd::lane_promise().ubsan)
    {
        check_null_store_reported(o, dir);
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

    if (cd::lane_promise().ubsan)
    {
        check_null_store_reported(o, dir);
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

    CHECK(o.verdict == cd::Verdict::Crashed);
    CHECK(o.exit_code == kExitAbrt);
    CHECK(count_crash_records(dir) == 1U);
    const RecordFields rf = parse_record(first_crash_record(dir));
    CHECK(rf.ok);
    CHECK(rf.signal == kSigAbrt);
    CHECK(rf.code == kSiTkill); // sent by abort(), not raised by the kernel: crd re-raises it rather than returning
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): a trapped floating-point division raises SIGFPE and is recorded",
          "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("fpe");
    const cd::Outcome o   = run_mode("fpe", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (!fp_division_traps())
    {
        // No division can trap on this FPU: the specimen reports it and nothing is recorded.
        CHECK(o.verdict == cd::Verdict::Unexpected);
        CHECK(o.exit_code == kExitNoFpTrap);
        CHECK(count_crash_records(dir) == 0U);
    }
    else
    {
        if (cd::lane_promise().asan)
        {
            check_asan_chained(o, dir, "AddressSanitizer: FPE");
        }
        else
        {
            CHECK(o.verdict == cd::Verdict::Crashed);
            CHECK(o.exit_code == kExitFpe);
            CHECK(count_crash_records(dir) == 1U);
        }
        const RecordFields rf = parse_record(first_crash_record(dir));
        CHECK(rf.ok);
        CHECK(rf.signal == kSigFpe);
        CHECK(rf.code == kFpeFltdiv);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash capture (linux): a read past the end of a mapped file raises SIGBUS and is recorded",
          "[core][diag][crash-capture]")
{
    const fs::path    dir = fresh_dir("bus");
    const cd::Outcome o   = run_mode("bus", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code);

    if (cd::lane_promise().asan)
    {
        check_asan_chained(o, dir, "AddressSanitizer: BUS");
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitBus);
        CHECK(count_crash_records(dir) == 1U);
    }
    const RecordFields rf = parse_record(first_crash_record(dir));
    CHECK(rf.ok);
    CHECK(rf.signal == kSigBus);
    CHECK(rf.code == kBusAdrerr);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#if CRD_ENABLE_ASSERTS
TEST_CASE("crash capture (linux): an injected record write failure is honest on the fatal path",
          "[core][diag][crash-capture]")
{
    // The write runs, then the seam fails it: no record is left, the handler says FAILED, and the hook's marker shows
    // it ran and saw DumpFailed. On a UBSan lane the null store never reaches crd, so this is the plain lanes' case.
    const fs::path    dir = fresh_dir("inject_dump_fail");
    const cd::Outcome o   = run_mode("inject_dump_fail", dir);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code << " marker=" << read_marker(dir));

    if (cd::lane_promise().ubsan)
    {
        check_null_store_reported(o, dir);
        CHECK(read_marker(dir) == -1);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::Crashed);
        CHECK(o.exit_code == kExitSegv);
        CHECK(fs::exists(dir));
        CHECK(count_crash_records(dir) == 0U);
        CHECK(output_has(o, "[crd] crash record FAILED"));
        CHECK(read_marker(dir) == static_cast<int>(crd::crash::WriteResult::DumpFailed));
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif // CRD_ENABLE_ASSERTS

#endif // defined(__linux__)

