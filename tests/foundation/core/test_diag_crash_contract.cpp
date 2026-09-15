// DIAG.5a (b): the checked-install + honest, collision-safe minidump-write contract, exercised WITHOUT a crash.
// The fatal path (an actual unhandled exception) is qualified by the subprocess specimens in a later sub-unit; here
// we prove install() reports its result, the resolved path is long-path-safe, and the live capture_dump() (the same
// writer the fatal filter uses) reports success only when a complete, non-empty file exists -- never otherwise.
#include <crd/core/crash.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <process.h> // _getpid
#include <windows.h> // GetCurrentThreadId
#else
#include <unistd.h> // getpid
#endif

#include "minidump_probe.hpp" // Windows-only dump content probe (no-op off Windows)

namespace fs = std::filesystem;

using crd::crash::InstallResult;
using crd::crash::WriteResult;

namespace
{
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<unsigned> g_unique{0};

// A unique, never-repo-root temp directory (dumps here are MB-scale; every case removes its own tree).
fs::path fresh_temp_dir()
{
    const unsigned n = g_unique.fetch_add(1U, std::memory_order_relaxed);
    fs::path       p =
        fs::temp_directory_path() / ("crd_crash_ct_" + std::to_string(n) + "_" + std::to_string(static_cast<unsigned>(
#if defined(_WIN32)
                                                                                     ::_getpid()
#else
                                                                                     ::getpid()
#endif
                                                                                     )));
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

// Count the .dmp files an install produced in dir.
std::size_t count_dumps(const fs::path& dir)
{
    std::size_t     n = 0;
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
        if (it->path().extension() == ".dmp")
            ++n;
    return n;
}

#if defined(_WIN32) && CRD_ENABLE_ASSERTS
struct HookState
{
    std::atomic<int>      total{0};
    std::atomic<int>      ok{0};
    std::atomic<int>      suppressed{0};
    std::atomic<unsigned> last_faulting_tid{0};
    std::atomic<unsigned> last_hook_thread{0};
};

void recording_hook(const crd::crash::CrashReport& r, void* user) noexcept
{
    auto* s = static_cast<HookState*>(user);
    s->total.fetch_add(1, std::memory_order_relaxed);
    if (r.write == WriteResult::Ok)
        s->ok.fetch_add(1, std::memory_order_relaxed);
    if (r.write == WriteResult::Suppressed)
        s->suppressed.fetch_add(1, std::memory_order_relaxed);
    s->last_faulting_tid.store(r.faulting_tid, std::memory_order_relaxed);
    s->last_hook_thread.store(static_cast<unsigned>(::GetCurrentThreadId()), std::memory_order_relaxed);
}
#endif
} // namespace

TEST_CASE("crash contract: install into a usable dir is Ok; a re-install is OkReinstalled", "[core][diag][crash]")
{
    crd::crash::uninstall(); // reset any prior static state in this process

    const fs::path dir = fresh_temp_dir();
    CHECK(InstallResult::Ok == crd::crash::install(dir.string().c_str()));
    CHECK(fs::exists(dir)); // install() created it
    CHECK(InstallResult::OkReinstalled == crd::crash::install(dir.string().c_str()));

    crd::crash::uninstall();
    crd::crash::uninstall(); // idempotent -- a second uninstall is harmless

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash contract: an output path that names an existing file is OutputDirUnusable", "[core][diag][crash]")
{
    crd::crash::uninstall();

    const fs::path dir  = fresh_temp_dir();
    fs::create_directories(dir);
    const fs::path file = dir / "not_a_dir";
    {
        std::ofstream f{file, std::ios::binary};
        REQUIRE(f.is_open());
        f << 'x';
    }

    CHECK(InstallResult::OutputDirUnusable == crd::crash::install(file.string().c_str()));

    crd::crash::uninstall();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#if defined(_WIN32)

TEST_CASE("crash contract: capture_dump before install reports NotInstalled", "[core][diag][crash]")
{
    crd::crash::uninstall();
    CHECK(WriteResult::NotInstalled == crd::crash::capture_dump(nullptr));
}

TEST_CASE("crash contract: a live capture writes a complete, long-path-safe, non-empty dump", "[core][diag][crash]")
{
    crd::crash::uninstall();
    const fs::path dir = fresh_temp_dir();
    REQUIRE(InstallResult::Ok == crd::crash::install(dir.string().c_str()));

    const wchar_t* path1 = nullptr;
    REQUIRE(WriteResult::Ok == crd::crash::capture_dump(&path1));
    REQUIRE(path1 != nullptr);

    const std::wstring wpath1{path1};
    CHECK(wpath1.rfind(L"\\\\?\\", 0) == 0); // resolved to the long-path form
    const fs::path dump1{wpath1};
    CHECK(fs::exists(dump1));
    CHECK(fs::file_size(dump1) > 0U);

    // A second back-to-back capture must not collide with or overwrite the first.
    const wchar_t* path2 = nullptr;
    REQUIRE(WriteResult::Ok == crd::crash::capture_dump(&path2));
    const fs::path dump2{std::wstring{path2}};
    CHECK(dump2 != dump1);
    CHECK(fs::exists(dump2));
    CHECK(fs::exists(dump1)); // the first is still there -- not clobbered

    crd::crash::uninstall();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash contract: a capture into a since-removed dir is OpenFailed and leaves nothing", "[core][diag][crash]")
{
    crd::crash::uninstall();
    const fs::path dir = fresh_temp_dir();
    REQUIRE(InstallResult::Ok == crd::crash::install(dir.string().c_str()));

    std::error_code ec;
    fs::remove_all(dir, ec); // the resolved path now points at a missing directory

    CHECK(WriteResult::OpenFailed == crd::crash::capture_dump(nullptr));
    CHECK_FALSE(fs::exists(dir)); // no directory or partial file was resurrected

    crd::crash::uninstall();
}

TEST_CASE("crash contract: a hang-kind live dump embeds and round-trips its evidence stream", "[core][diag][crash]")
{
    crd::crash::uninstall();
    const fs::path dir = fresh_temp_dir();
    REQUIRE(InstallResult::Ok == crd::crash::install(dir.string().c_str()));

    const std::array<unsigned char, 12> blob{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};
    crd::crash::DumpNote                 note{};
    note.kind           = crd::crash::DumpKind::Hang;
    note.evidence       = blob.data();
    note.evidence_bytes = static_cast<std::uint32_t>(blob.size());

    const wchar_t* path = nullptr;
    REQUIRE(WriteResult::Ok == crd::crash::capture_dump(note, &path));
    REQUIRE(path != nullptr);
    CHECK(std::wstring{path}.find(L"\\hang_") != std::wstring::npos); // the Hang prefix

    std::array<unsigned char, 64> read_back{};
    const std::size_t             n =
        crd::crash::read_dump_stream(path, crd::crash::kEvidenceStreamType, read_back.data(), read_back.size());
    REQUIRE(n == blob.size()); // the evidence stream is present with the exact byte count
    for (std::size_t i = 0; i < blob.size(); ++i)
        CHECK(read_back[i] == blob[i]); // ...and the exact bytes

    // The dump really captured threads (MiniDumpWriteDump suspended and walked them): NumberOfThreads >= 1.
    constexpr std::uint32_t     kThreadListStream = 3U; // MINIDUMP_STREAM_TYPE::ThreadListStream
    std::array<unsigned char, 8> head{};
    const std::size_t           tn = crd::crash::read_dump_stream(path, kThreadListStream, head.data(), head.size());
    REQUIRE(tn >= sizeof(std::uint32_t));
    std::uint32_t num_threads = 0;
    std::memcpy(&num_threads, head.data(), sizeof(num_threads)); // MINIDUMP_THREAD_LIST.NumberOfThreads (leading u32)
    CHECK(num_threads >= 1U);

    crd::crash::uninstall();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash contract: a manual live dump uses the live_ prefix", "[core][diag][crash]")
{
    crd::crash::uninstall();
    const fs::path dir = fresh_temp_dir();
    REQUIRE(InstallResult::Ok == crd::crash::install(dir.string().c_str()));

    const wchar_t* path = nullptr;
    REQUIRE(WriteResult::Ok == crd::crash::capture_dump(&path)); // the Manual overload
    REQUIRE(path != nullptr);
    CHECK(std::wstring{path}.find(L"\\live_") != std::wstring::npos);

    crd::crash::uninstall();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#if CRD_ENABLE_ASSERTS

TEST_CASE("crash contract: a simulated fatal writes one dump, off the faulting thread", "[core][diag][crash]")
{
    crd::crash::uninstall();
    const fs::path dir = fresh_temp_dir();
    REQUIRE(InstallResult::Ok == crd::crash::install(dir.string().c_str()));

    HookState hs;
    crd::crash::set_crash_report_handler(&recording_hook, &hs);

    const unsigned                caller = static_cast<unsigned>(::GetCurrentThreadId());
    const crd::crash::CrashReport r      = crd::crash::test_fatal_path(0xC0000005U); // synthetic access violation

    CHECK(r.write == WriteResult::Ok);
    CHECK(r.code == 0xC0000005U);
    CHECK(r.faulting_tid == caller);
    CHECK(count_dumps(dir) == 1U); // exactly one dump for one fault

    CHECK(hs.total.load() == 1);
    CHECK(hs.ok.load() == 1);
    CHECK(hs.last_faulting_tid.load() == caller);       // the report names the faulting thread...
    CHECK(hs.last_hook_thread.load() != caller);        // ...but the dump ran on the dedicated handler thread (the hop)

    crd::crash::set_crash_report_handler(nullptr, nullptr);
    crd::crash::uninstall();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash contract: a concurrent second fault is Suppressed with exactly one dump", "[core][diag][crash]")
{
    crd::crash::uninstall();
    const fs::path dir = fresh_temp_dir();
    REQUIRE(InstallResult::Ok == crd::crash::install(dir.string().c_str()));

    HookState hs;
    crd::crash::set_crash_report_handler(&recording_hook, &hs);

    std::atomic<int>        ok_returns{0};
    std::atomic<int>        suppressed_returns{0};
    std::atomic<bool>       go{false};
    auto                    fault = [&]() noexcept {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        const crd::crash::CrashReport r = crd::crash::test_fatal_path(0xC0000005U);
        if (r.write == WriteResult::Ok)
            ok_returns.fetch_add(1, std::memory_order_relaxed);
        else if (r.write == WriteResult::Suppressed)
            suppressed_returns.fetch_add(1, std::memory_order_relaxed);
    };

    std::thread t1{fault};
    std::thread t2{fault};
    go.store(true, std::memory_order_release);
    t1.join(); // both must return -- neither blocks forever
    t2.join();

    CHECK(ok_returns.load() == 1);         // exactly one winner
    CHECK(suppressed_returns.load() == 1); // exactly one suppressed
    CHECK(count_dumps(dir) == 1U);         // one dump per process, even under concurrent faults
    CHECK(hs.ok.load() == 1);
    CHECK(hs.suppressed.load() == 1);

    crd::crash::set_crash_report_handler(nullptr, nullptr);
    crd::crash::uninstall();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash contract: the fatal path before install reports NotInstalled", "[core][diag][crash]")
{
    crd::crash::uninstall();
    const crd::crash::CrashReport r = crd::crash::test_fatal_path(0xC0000005U);
    CHECK(r.write == WriteResult::NotInstalled);
}

TEST_CASE("crash contract: uninstall joins the handler thread and a re-install still works", "[core][diag][crash]")
{
    crd::crash::uninstall();
    const fs::path dir = fresh_temp_dir();
    REQUIRE(InstallResult::Ok == crd::crash::install(dir.string().c_str()));
    crd::crash::uninstall(); // joins the handler thread promptly (no hang)

    REQUIRE(InstallResult::Ok == crd::crash::install(dir.string().c_str())); // a fresh generation
    const crd::crash::CrashReport r = crd::crash::test_fatal_path(0xC0000005U);
    CHECK(r.write == WriteResult::Ok);

    crd::crash::uninstall();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash contract: an injected MiniDumpWriteDump failure is honest (DumpFailed, no file)",
          "[core][diag][crash]")
{
    // The denied test proves OpenFailed; the acceptance also names MiniDumpWriteDump *itself* failing -> the
    // DumpFailed step, reachable only by forcing the BOOL FALSE. The seam runs the real call, then forces the failure
    // branch, so the exact cleanup (close, delete the partial, honest result) is exercised. No real check is removed.
    crd::crash::uninstall();
    const fs::path dir = fresh_temp_dir();
    REQUIRE(InstallResult::Ok == crd::crash::install(dir.string().c_str()));

    crd::crash::test_inject_write_failure(WriteResult::DumpFailed);
    CHECK(WriteResult::DumpFailed == crd::crash::capture_dump(nullptr));

    std::size_t     files = 0;
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec))
        if (it->path().extension() == ".dmp")
            ++files;
    CHECK(files == 0U); // the forced failure deleted its partial; nothing plausible is left behind

    crd::crash::test_inject_write_failure(WriteResult::Ok); // clear -> the next write succeeds honestly
    CHECK(WriteResult::Ok == crd::crash::capture_dump(nullptr));

    crd::crash::uninstall();
    fs::remove_all(dir, ec);
}

TEST_CASE("crash contract: injected open- and flush-step failures are honest and leave no file", "[core][diag][crash]")
{
    // Completes the write-step failure mapping alongside the DumpFailed seam above: the open step (a denied or full
    // output path, before the write) and the flush step (a write that will not durably land) each report their exact
    // result and delete any partial -- so a reported success is never a truncated dump, on any failing step.
    crd::crash::uninstall();
    const fs::path dir = fresh_temp_dir();
    REQUIRE(InstallResult::Ok == crd::crash::install(dir.string().c_str()));

    for (const WriteResult step : {WriteResult::OpenFailed, WriteResult::FlushFailed})
    {
        crd::crash::test_inject_write_failure(step);
        CHECK(step == crd::crash::capture_dump(nullptr)); // the exact failing step is reported...
        CHECK(count_dumps(dir) == 0U);                    // ...and no partial dump is left behind
    }
    crd::crash::test_inject_write_failure(WriteResult::Ok);
    CHECK(WriteResult::Ok == crd::crash::capture_dump(nullptr)); // cleared -> an honest success

    crd::crash::uninstall();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("crash contract: the written dump is readable and identifies the fault and the binary", "[core][diag][crash]")
{
    // The "a successful report requires a readable, correctly identified dump" acceptance, proved by reading the dump
    // back: the ExceptionStream carries the injected fault code and the FAULTING thread's id (not the writer thread's
    // -- the dedicated handler wrote it but recorded the faulter), and the ModuleListStream names this binary and
    // carries its RSDS CV record. That CV identity is what later symbolization (DIAG.5d) matches or rejects -- the
    // "missing symbols" acceptance, answered without claiming a symbolized frame here. The simulated fatal path raises
    // no real exception, so ASan's VEH stays out and this holds identically on win-debug and win-asan.
    crd::crash::uninstall();
    const fs::path dir = fresh_temp_dir();
    REQUIRE(InstallResult::Ok == crd::crash::install(dir.string().c_str()));

    const unsigned                caller = static_cast<unsigned>(::GetCurrentThreadId());
    const crd::crash::CrashReport r      = crd::crash::test_fatal_path(0xC0000005U);
    REQUIRE(r.write == WriteResult::Ok);
    REQUIRE(r.dump_path != nullptr);
    const std::wstring dump_path{r.dump_path}; // valid until the next write overwrites the shared path buffer

    CHECK(crd_test_minidump::exception_code(dump_path) == 0xC0000005U); // the fault code landed in the dump
    CHECK(crd_test_minidump::exception_thread_id(dump_path) == caller); // ...tagged to the faulting thread

    wchar_t     exe[MAX_PATH] = {0};
    const DWORD n             = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    REQUIRE(n > 0U);
    const std::wstring exe_base = fs::path{std::wstring{exe, n}}.filename().wstring();
    CHECK(crd_test_minidump::names_module_with_cv(dump_path, exe_base)); // the crashing binary is named + CV-identified

    crd::crash::uninstall();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#endif // CRD_ENABLE_ASSERTS

#else // non-Windows: the in-process minidump writer is unsupported; the install contract still holds

TEST_CASE("crash contract: capture_dump is Unsupported off Windows", "[core][diag][crash]")
{
    crd::crash::uninstall();
    const fs::path dir = fresh_temp_dir();
    REQUIRE(InstallResult::Ok == crd::crash::install(dir.string().c_str()));
    CHECK(WriteResult::Unsupported == crd::crash::capture_dump(nullptr));
    crd::crash::uninstall();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#endif
