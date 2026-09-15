#pragma once

#include <crd/core/build_config.hpp> // CRD_ENABLE_ASSERTS

#include <cstddef>
#include <cstdint>

namespace crd::crash
{

// How install() resolved. Every value except the three Ok* forms means no handler is active.
enum class InstallResult : std::uint32_t
{
    Ok,                      // installed fresh over whatever was there
    OkReinstalled,           // our own handler was already installed; re-registered, previous preserved
    OkReplacedForeignFilter, // a foreign handler had displaced ours since the last install; reinstalled
    OutputDirUnusable,       // the output directory could not be created, or the path names an existing file
    PathTooLong,             // the resolved absolute path did not fit the reserved buffer
    FilterInstallFailed,     // the platform install call itself failed
    Unsupported,             // this build has no crash backend for the platform
};

// How a dump write went. Only Ok means a complete, flushed, closed dump exists at the reported path.
enum class WriteResult : std::uint32_t
{
    Ok,
    NotInstalled,   // install() has not run (no resolved output path)
    OpenFailed,     // the output file could not be created (denied/full/collision-exhausted)
    DumpFailed,     // MiniDumpWriteDump returned FALSE; the partial file was deleted
    FlushFailed,    // the dump was written but flush/close failed; the file was deleted
    Suppressed,     // a concurrent second fault: one dump per process, so this fault wrote none
    HandlerTimeout, // the dedicated handler thread did not finish within the bounded wait
    Unsupported,    // no in-process dump mechanism on this platform build
};

// What produced a dump; selects the filename prefix (crash_ / hang_ / live_).
enum class DumpKind : std::uint32_t
{
    Crash,  // the fatal unhandled-exception path
    Hang,   // a non-fatal dump requested by a hang/watchdog observer (the process keeps running)
    Manual, // any other deliberate live capture
};

// The MINIDUMP user-stream type under which a live dump's evidence blob is stored (above LastReservedStream so it
// never collides with a system stream). Read it back with read_dump_stream(path, kEvidenceStreamType, ...).
inline constexpr std::uint32_t kEvidenceStreamType = 0x43524444U; // 'CRDD'

// Optional evidence embedded in a non-fatal dump as one user stream. `evidence` must stay valid for the duration of
// the capture_dump call; the bytes are copied into the dump. evidence_bytes == 0 embeds no stream.
struct DumpNote
{
    DumpKind      kind{DumpKind::Manual};
    const void*   evidence{nullptr};
    std::uint32_t evidence_bytes{0};
};

// Filled on the fatal path and passed to an installed report handler. All fields are preallocated;
// nothing here allocates or takes a lock. dump_path is the resolved wide path on Windows (may be null
// when the write never opened a file); it is null on platforms without an in-process dump path.
struct CrashReport
{
    std::uint32_t  code{0};          // exception code (Windows) / signal number (Linux)
    const void*    address{nullptr}; // faulting address, when the platform reports one
    std::uint32_t  faulting_tid{0};  // OS thread id of the faulting thread
    WriteResult    write{WriteResult::Unsupported};
    const wchar_t* dump_path{nullptr};
    std::uint32_t  last_error{0};    // raw GetLastError / HRESULT of the failing step (0 when write == Ok)
};

// Installed with the atomic-pair, no-default-action pattern: nothing fires unless a handler is set.
// For the fatal dump the handler runs on the dedicated crash handler thread (a fresh stack, so it is
// reached even when the faulting stack is exhausted); a suppressed concurrent-fault notification instead
// runs on that extra faulting thread. Either way it must not allocate or block; hosts route it to the
// preallocated emergency record.
using CrashReportHandler = void (*)(const CrashReport&, void* user) noexcept;
void set_crash_report_handler(CrashReportHandler handler, void* user) noexcept;

// Install the platform crash handler. The output path is resolved to an absolute, long-path-safe form
// once here (never on the fatal path). Returns a checked result; callers must inspect it.
//
// Windows: SetUnhandledExceptionFilter -> MiniDumpWriteDump. The dump filename carries pid + a 64-bit
//          tick + a counter and is opened CREATE_NEW, so concurrent or same-second crashes never collide
//          or overwrite. The write result is checked: a failed MiniDumpWriteDump deletes the partial file
//          and never reports success. ExceptionCode/Address are printed to stderr as the emergency
//          fallback regardless of whether the dump could be written.
//
// Linux:   sigaction(SIGSEGV | SIGABRT | SIGFPE | SIGILL); writes a crash log then re-raises with the
//          default handler so the OS writes a core dump. (Alternate signal stack and install-failure
//          hardening belong to the Linux crash-capture slice.)
//
// Safe to call multiple times; a re-install preserves the originally saved previous handler so
// uninstall() restores it rather than restoring our own filter.
[[nodiscard]] InstallResult install(const char* output_dir = "./crashes") noexcept;

// Restore whichever handler was active before the first install() call. Called by Application::~Application().
void uninstall() noexcept;

// Live, non-fatal capture of the current process through the same writer the fatal path uses. Returns the
// write result; on Ok, *out_path (when non-null) receives the resolved dump path. Requires a prior install()
// (it reuses the resolved output path). This is the shared entry point the hang path will drive later.
WriteResult capture_dump(const wchar_t** out_path = nullptr) noexcept;

// Live, non-fatal capture that also records what produced it and (optionally) an evidence blob as a user stream.
// This is the entry a hang/watchdog observer drives to capture the stuck worker stacks: MiniDumpWriteDump suspends
// every other thread and walks their stacks safely, so the cooperative snapshot names WHAT is stuck and this dump
// shows WHERE. Never a default action -- a host installs the hang handler that calls it (no second crash path).
// Bounded: after a small per-process cap of non-fatal dumps, further calls return Suppressed. Must not be called
// from a fiber (it captures OS stacks and holds a lock across a possible park); the fatal single-shot gate is
// untouched. Requires a prior install().
WriteResult capture_dump(const DumpNote& note, const wchar_t** out_path = nullptr) noexcept;

// Copy the bytes of one stream (system stream number or a user-stream type such as kEvidenceStreamType) out of a
// written dump file, up to cap. Returns the stream's byte count (which may exceed cap -> truncated copy), or 0 when
// the file, the stream, or DbgHelp is unavailable. Lets tests/tools read a dump without linking dbghelp themselves.
std::size_t read_dump_stream(const wchar_t* dump_path, std::uint32_t stream_type, void* out, std::size_t cap) noexcept;

// Reserve stack for the current thread's own last-chance unwinding so a stack-overflow fault still has room
// to signal the handler thread. Checked wrapper over SetThreadStackGuarantee (no-op / harmless off Windows).
// install() calls it for the installing thread; hosts may call it on threads they own.
void guard_current_thread_stack(std::uint32_t reserve_bytes = 65536U) noexcept;

#if CRD_ENABLE_ASSERTS
// Test-only seam: drive the fatal path with a SYNTHETIC exception record (no real fault, no filter chaining)
// so the handler-thread hop, the single-shot concurrent-fault gate and the honest write result can be
// exercised without crashing the test process. Returns the CrashReport the fatal path produced. Windows only;
// other platforms return { write = Unsupported }.
CrashReport test_fatal_path(std::uint32_t exception_code) noexcept;

// Test-only seam: force the NEXT dump write to fail at a chosen step so the honest-failure paths that only a real
// failure can reach (notably a FALSE MiniDumpWriteDump -> DumpFailed) are exercised. Pass Ok to clear. Only
// OpenFailed / DumpFailed / FlushFailed are meaningful; the check is off by default and install() clears it, so no
// real oracle is weakened -- the true checks still run, this only injects an additional failure. Windows only.
void test_inject_write_failure(WriteResult step) noexcept;
#endif

} // namespace crd::crash
