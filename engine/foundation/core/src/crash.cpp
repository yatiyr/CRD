#include <crd/core/crash.hpp>

#include <atomic>

// ---- Windows ---------------------------------------------------------------
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// DbgHelp must follow windows.h -- its types depend on it.
#include <DbgHelp.h>

#include <cstdio>

namespace
{

constexpr std::size_t kPathCap = 32768; // the NT long-path maximum, in wide chars

// Resolved, long-path-safe output directory (no trailing separator). Empty means "not installed".
wchar_t s_output_dir_w[kPathCap] = L"";

// Scratch used only by install() (never on the fatal path); install() is not concurrent.
wchar_t s_scratch_a[kPathCap];
wchar_t s_scratch_b[kPathCap];

// The last built dump path; returned to callers via out_path and stable until the next write (serialized
// by s_dump_lock). Lives in BSS so building it on the fatal path touches no allocator.
wchar_t s_dump_path[kPathCap];

LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter = nullptr; // the genuine previous filter (preserved across re-installs)
bool                         s_have_prev   = false;

std::atomic<crd::crash::CrashReportHandler> s_handler{nullptr};
std::atomic<void*>                          s_handler_user{nullptr};
std::atomic<unsigned>                       s_dump_serial{0};
std::atomic<unsigned>                       s_live_dumps{0}; // non-fatal dumps written this process (bounded)

constexpr unsigned kMaxLiveDumps = 16; // a bounded per-process cap on non-fatal dumps (2a-style policy, not a knob)

#if CRD_ENABLE_ASSERTS
// Test-only: force the next write to fail at a chosen step (Ok = no injection). Off by default; install() clears it.
std::atomic<crd::crash::WriteResult> s_inject_step{crd::crash::WriteResult::Ok};
#endif

SRWLOCK s_dump_lock = SRWLOCK_INIT; // serializes MiniDumpWriteDump (DbgHelp is not thread-safe)

// -- bounds-checked wide-string builders (no CRT locale/lock use on the fatal path) --

bool w_append(wchar_t* buf, std::size_t cap, std::size_t& i, const wchar_t* s) noexcept
{
    while (*s != L'\0')
    {
        if (i + 1 >= cap)
            return false;
        buf[i++] = *s++;
    }
    buf[i] = L'\0';
    return true;
}

bool w_append_u32_dec(wchar_t* buf, std::size_t cap, std::size_t& i, unsigned v) noexcept
{
    wchar_t tmp[11];
    int     n = 0;
    if (v == 0U)
        tmp[n++] = L'0';
    while (v != 0U)
    {
        tmp[n++] = static_cast<wchar_t>(L'0' + (v % 10U));
        v /= 10U;
    }
    while (n > 0)
    {
        if (i + 1 >= cap)
            return false;
        buf[i++] = tmp[--n];
    }
    buf[i] = L'\0';
    return true;
}

bool w_append_u64_hex(wchar_t* buf, std::size_t cap, std::size_t& i, unsigned long long v) noexcept
{
    wchar_t tmp[17];
    int     n = 0;
    if (v == 0ULL)
        tmp[n++] = L'0';
    while (v != 0ULL)
    {
        const unsigned d = static_cast<unsigned>(v & 0xFULL);
        tmp[n++]         = static_cast<wchar_t>(d < 10U ? (L'0' + d) : (L'a' + (d - 10U)));
        v >>= 4;
    }
    while (n > 0)
    {
        if (i + 1 >= cap)
            return false;
        buf[i++] = tmp[--n];
    }
    buf[i] = L'\0';
    return true;
}

// Prefix an absolute path with the \\?\ long-path form (UNC-aware), into out. full comes from GetFullPathNameW.
bool build_prefixed(const wchar_t* full, wchar_t* out) noexcept
{
    std::size_t i = 0;
    // Already \\?\-prefixed: copy as-is.
    if (full[0] == L'\\' && full[1] == L'\\' && full[2] == L'?' && full[3] == L'\\')
        return w_append(out, kPathCap, i, full);
    // UNC \\server\share -> \\?\UNC\server\share
    if (full[0] == L'\\' && full[1] == L'\\')
    {
        if (!w_append(out, kPathCap, i, L"\\\\?\\UNC\\"))
            return false;
        return w_append(out, kPathCap, i, full + 2);
    }
    // Drive path C:\... -> \\?\C:\...
    if (!w_append(out, kPathCap, i, L"\\\\?\\"))
        return false;
    return w_append(out, kPathCap, i, full);
}

// -- lock-free emergency stderr output --
//
// The fatal path must never touch the CRT stream lock: if the faulting thread crashed while holding it (e.g. mid
// fprintf, or a logger flushing), the handler thread would block on it until the bounded wait expires. WriteFile to
// the raw stderr handle takes no CRT lock. Each helper formats into a tiny stack buffer (stack-overflow-safe) and
// writes it; no buffering, so no flush is needed.

void emit(HANDLE h, const char* s, DWORD n) noexcept
{
    if (h != nullptr && h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        (void)WriteFile(h, s, n, &written, nullptr);
    }
}

void emit_cstr(HANDLE h, const char* s) noexcept
{
    DWORD n = 0;
    while (s[n] != '\0')
        ++n;
    emit(h, s, n);
}

void emit_u32_dec(HANDLE h, unsigned v) noexcept
{
    char tmp[10];
    int  n = 0;
    if (v == 0U)
        tmp[n++] = '0';
    while (v != 0U)
    {
        tmp[n++] = static_cast<char>('0' + (v % 10U));
        v /= 10U;
    }
    char out[10];
    for (int j = 0; j < n; ++j)
        out[j] = tmp[n - 1 - j];
    emit(h, out, static_cast<DWORD>(n));
}

// Fixed-width hex (width nibbles), zero-padded -- for exception codes (8) and addresses (16).
void emit_hex(HANDLE h, unsigned long long v, int width) noexcept
{
    char        out[16];
    const char* digits = "0123456789ABCDEF";
    for (int j = 0; j < width; ++j)
        out[width - 1 - j] = digits[(v >> (j * 4)) & 0xFULL];
    emit(h, out, static_cast<DWORD>(width));
}

// The single writer shared by the fatal filter and the live capture. mei == nullptr => a general (non-exception)
// dump. On Ok, *out_path (when non-null) points at s_dump_path. Caller holds s_dump_lock.
crd::crash::WriteResult write_dump(MINIDUMP_EXCEPTION_INFORMATION* mei, crd::crash::DumpKind kind,
                                   const crd::crash::DumpNote* note, const wchar_t** out_path,
                                   DWORD* out_last_error) noexcept
{
    using crd::crash::WriteResult;

    if (s_output_dir_w[0] == L'\0')
        return WriteResult::NotInstalled;

    const wchar_t* prefix = L"\\crash_";
    if (kind == crd::crash::DumpKind::Hang)
    {
        prefix = L"\\hang_";
    }
    else if (kind == crd::crash::DumpKind::Manual)
    {
        prefix = L"\\live_";
    }
    else if (kind == crd::crash::DumpKind::DeviceRemoved)
    {
        prefix = L"\\gpu_";
    }

    // <dir>\<prefix><pid>_<tick64hex>_<serial>_<attempt>.dmp -- backslash only (\\?\ suppresses '/' normalization).
    std::size_t base = 0;
    if (!w_append(s_dump_path, kPathCap, base, s_output_dir_w) ||
        !w_append(s_dump_path, kPathCap, base, prefix) ||
        !w_append_u32_dec(s_dump_path, kPathCap, base, GetCurrentProcessId()) ||
        !w_append(s_dump_path, kPathCap, base, L"_") ||
        !w_append_u64_hex(s_dump_path, kPathCap, base, GetTickCount64()) ||
        !w_append(s_dump_path, kPathCap, base, L"_") ||
        !w_append_u32_dec(s_dump_path, kPathCap, base, s_dump_serial.fetch_add(1U, std::memory_order_relaxed)) ||
        !w_append(s_dump_path, kPathCap, base, L"_"))
    {
        if (out_last_error != nullptr)
            *out_last_error = ERROR_BUFFER_OVERFLOW;
        return WriteResult::OpenFailed;
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 4096U; ++attempt)
    {
        std::size_t i = base;
        if (!w_append_u32_dec(s_dump_path, kPathCap, i, attempt) || !w_append(s_dump_path, kPathCap, i, L".dmp"))
        {
            if (out_last_error != nullptr)
                *out_last_error = ERROR_BUFFER_OVERFLOW;
            return WriteResult::OpenFailed;
        }
        file = CreateFileW(s_dump_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
            break;
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS)
            continue; // collision: next attempt index
        if (out_last_error != nullptr)
            *out_last_error = e;
        return WriteResult::OpenFailed;
    }
    if (file == INVALID_HANDLE_VALUE)
    {
        if (out_last_error != nullptr)
            *out_last_error = ERROR_FILE_EXISTS;
        return WriteResult::OpenFailed;
    }

#if CRD_ENABLE_ASSERTS
    if (s_inject_step.load(std::memory_order_relaxed) == WriteResult::OpenFailed)
    {
        (void)CloseHandle(file);
        (void)DeleteFileW(s_dump_path);
        if (out_last_error != nullptr)
            *out_last_error = ERROR_CANCELLED;
        return WriteResult::OpenFailed; // injected: exercise the open-failure cleanup path
    }
#endif

    // Embed the caller's evidence as one user stream (its bytes are copied into the dump during the call).
    MINIDUMP_USER_STREAM             us{};
    MINIDUMP_USER_STREAM_INFORMATION usi{};
    PMINIDUMP_USER_STREAM_INFORMATION usi_ptr = nullptr;
    if (note != nullptr && note->evidence != nullptr && note->evidence_bytes > 0U)
    {
        us.Type             = crd::crash::kEvidenceStreamType;
        us.BufferSize       = note->evidence_bytes;
        // MINIDUMP_USER_STREAM::Buffer is PVOID (non-const); MiniDumpWriteDump only reads it (the bytes are copied
        // into the dump during the call), so dropping const at this Win32 boundary is safe.
        us.Buffer           = const_cast<void*>(note->evidence); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        usi.UserStreamCount = 1U;
        usi.UserStreamArray = &us;
        usi_ptr             = &usi;
    }

    BOOL  ok       = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                      static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithHandleData |
                                                                 MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo |
                                                                 // DIAG.5d(d): carry ntdll's unloaded-module trace so a
                                                                 // module unloaded before the fault still yields its
                                                                 // identity (the UnloadedModuleListStream readback).
                                                                 MiniDumpWithUnloadedModules),
                                      mei, usi_ptr, nullptr);
    DWORD dump_err = GetLastError(); // an HRESULT for MiniDumpWriteDump; stored raw
#if CRD_ENABLE_ASSERTS
    if (s_inject_step.load(std::memory_order_relaxed) == WriteResult::DumpFailed)
    {
        ok       = FALSE; // the real call ran; force the FALSE branch so the exact DumpFailed cleanup is exercised
        dump_err = ERROR_CANCELLED;
    }
#endif
    if (ok == FALSE)
    {
        (void)CloseHandle(file);
        (void)DeleteFileW(s_dump_path); // no plausible partial left behind
        if (out_last_error != nullptr)
            *out_last_error = dump_err;
        return WriteResult::DumpFailed;
    }
    BOOL flushed = FlushFileBuffers(file);
#if CRD_ENABLE_ASSERTS
    if (s_inject_step.load(std::memory_order_relaxed) == WriteResult::FlushFailed)
        flushed = FALSE;
#endif
    if (flushed == FALSE)
    {
        const DWORD e = GetLastError();
        (void)CloseHandle(file);
        (void)DeleteFileW(s_dump_path);
        if (out_last_error != nullptr)
            *out_last_error = e;
        return WriteResult::FlushFailed;
    }
    (void)CloseHandle(file);
    if (out_path != nullptr)
        *out_path = s_dump_path;
    return WriteResult::Ok;
}

// -- dedicated crash handler thread + single-shot concurrent-fault gate --
//
// The faulting thread does the minimum -- claim the gate, hand off the exception pointers and its own tid, wait --
// while the dedicated handler thread performs MiniDumpWriteDump on its OWN fresh stack, so a stack-overflow fault is
// still captured (the faulting stack may have no room left to run DbgHelp). Exactly one dump is written per process:
// a concurrent second fault is honestly Suppressed and still waits for the in-flight dump before returning, because
// returning first would let the OS tear the process down mid-dump.

HANDLE        s_handler_thread = nullptr;
DWORD         s_handler_tid    = 0;
HANDLE        s_req_event      = nullptr; // faulter -> handler: an exception is waiting (auto-reset)
HANDLE        s_done_event     = nullptr; // handler -> faulter(s): the dump attempt finished (manual-reset)
HANDLE        s_quit_event     = nullptr; // uninstall -> handler: leave the loop (manual-reset)
volatile LONG s_gate           = 0;       // 0 = no fault has claimed the single dump slot yet; 1 = one has

EXCEPTION_POINTERS*     s_req_ep  = nullptr; // the winner's pointers; its stack stays valid because it waits
DWORD                   s_req_tid = 0;       // the winner's thread id (dumped as mei.ThreadId, not the writer's)
crd::crash::CrashReport s_req_report{};      // the handler thread's result, read back by the winner after done

constexpr DWORD kHandlerWaitMs = 30000; // bounded: a wedged handler must not hang the crash forever

// Last-chance stack every thread created after install() reserves automatically (the guard_current_thread_stack
// default). Without it a thread that exhausts its stack has only the OS's default slack, about one page, to enter the
// exception dispatcher and run the filter's hand-off; hosted Windows Server 2025 runs showed that is not reliably
// enough (2026-10-04: an unguarded thread's overflow exited 0xC00000FD with no dump on MSVC, 0xC0000005 on
// clang-cl).
constexpr ULONG   kAutoGuardBytes = 65536U;
std::atomic<bool> s_auto_guard{false}; // set while installed; read by the TLS callback on every thread attach

// TLS callback: the loader calls it for every thread the process creates (DLL_THREAD_ATTACH), so threads created after
// install() -- engine workers, host threads, raw std::threads -- get the guarantee without calling anything. It runs
// under the loader lock: no allocation, no logging, one atomic load and one kernel32 call. A thread whose stack reserve
// cannot hold the guarantee gets FALSE back and keeps the default slack. Threads that already existed at install()
// are not touched (the installing thread is guarded by install() itself; hosts may call guard_current_thread_stack).
void NTAPI crash_tls_callback(PVOID /*module*/, DWORD reason, PVOID /*reserved*/) noexcept
{
    if (reason == DLL_THREAD_ATTACH && s_auto_guard.load(std::memory_order_acquire))
    {
        ULONG bytes = kAutoGuardBytes;
        (void)SetThreadStackGuarantee(&bytes);
    }
}

// Perform the dump for s_req_ep / s_req_tid, print, record s_req_report, fire the hook. Runs on the handler
// thread (the fresh-stack path); the caller holds nothing (this takes s_dump_lock itself).
void do_fatal_dump() noexcept
{
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = s_req_tid;
    mei.ExceptionPointers = s_req_ep;
    mei.ClientPointers    = FALSE;

    const wchar_t* dump_path  = nullptr;
    DWORD          last_error = 0;
    AcquireSRWLockExclusive(&s_dump_lock);
    const crd::crash::WriteResult wr = write_dump(&mei, crd::crash::DumpKind::Crash, nullptr, &dump_path, &last_error);
    ReleaseSRWLockExclusive(&s_dump_lock);

    const EXCEPTION_RECORD* rec  = (s_req_ep != nullptr) ? s_req_ep->ExceptionRecord : nullptr;
    const HANDLE            herr = GetStdHandle(STD_ERROR_HANDLE);
    if (wr == crd::crash::WriteResult::Ok)
    {
        emit_cstr(herr, "[crd] crash dump: ");
        // Narrow the wide path for the emergency line (Win32, no CRT lock). Static buffer: only the single gate
        // winner ever reaches this, so there is no concurrent writer.
        static char narrow_path[kPathCap];
        const int   pn = WideCharToMultiByte(CP_UTF8, 0, dump_path, -1, narrow_path, static_cast<int>(kPathCap),
                                             nullptr, nullptr);
        if (pn > 1)
            emit(herr, narrow_path, static_cast<DWORD>(pn - 1)); // pn includes the NUL
        emit_cstr(herr, "\n");
    }
    else
    {
        emit_cstr(herr, "[crd] crash dump FAILED (result ");
        emit_u32_dec(herr, static_cast<unsigned>(wr));
        emit_cstr(herr, ", error ");
        emit_u32_dec(herr, last_error);
        emit_cstr(herr, ")\n");
    }
    if (rec != nullptr)
    {
        emit_cstr(herr, "[crd] ExceptionCode=0x");
        emit_hex(herr, rec->ExceptionCode, 8);
        emit_cstr(herr, " ExceptionAddress=0x");
        emit_hex(herr, reinterpret_cast<unsigned long long>(rec->ExceptionAddress), 16);
        emit_cstr(herr, "\n");
    }

    crd::crash::CrashReport report{};
    report.code         = (rec != nullptr) ? rec->ExceptionCode : 0UL;
    report.address      = (rec != nullptr) ? rec->ExceptionAddress : nullptr;
    report.faulting_tid = s_req_tid;
    report.write        = wr;
    report.dump_path    = (wr == crd::crash::WriteResult::Ok) ? dump_path : nullptr;
    report.last_error   = last_error;
    s_req_report        = report;

    if (crd::crash::CrashReportHandler h = s_handler.load(std::memory_order_acquire); h != nullptr)
        h(report, s_handler_user.load(std::memory_order_relaxed));
}

DWORD WINAPI handler_thread_proc(LPVOID) noexcept
{
    HANDLE waits[2] = {s_req_event, s_quit_event};
    for (;;)
    {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) // a fault is waiting
        {
            do_fatal_dump();
            SetEvent(s_done_event);
            continue;
        }
        break; // quit signalled, or the wait failed -- either way, leave
    }
    return 0;
}

// Best-effort direct dump on the CURRENT thread (the handler thread is unavailable: it faulted itself, or timed
// out). Uses a try-lock so a dump already in progress cannot deadlock the fallback. Fills and returns r.
crd::crash::CrashReport direct_fallback_dump(EXCEPTION_POINTERS* ep, DWORD tid) noexcept
{
    using crd::crash::WriteResult;
    crd::crash::CrashReport r{};
    r.faulting_tid              = tid;
    const EXCEPTION_RECORD* rec = (ep != nullptr) ? ep->ExceptionRecord : nullptr;
    r.code                      = (rec != nullptr) ? rec->ExceptionCode : 0UL;
    r.address                   = (rec != nullptr) ? rec->ExceptionAddress : nullptr;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = tid;
    mei.ExceptionPointers = ep;
    mei.ClientPointers    = FALSE;

    const wchar_t* p  = nullptr;
    DWORD          le = 0;
    if (TryAcquireSRWLockExclusive(&s_dump_lock))
    {
        r.write = write_dump(&mei, crd::crash::DumpKind::Crash, nullptr, &p, &le);
        ReleaseSRWLockExclusive(&s_dump_lock);
        r.dump_path  = (r.write == WriteResult::Ok) ? p : nullptr;
        r.last_error = le;
    }
    else
    {
        r.write = WriteResult::HandlerTimeout; // a dump is already in progress under the lock
    }
    if (crd::crash::CrashReportHandler h = s_handler.load(std::memory_order_acquire); h != nullptr)
        h(r, s_handler_user.load(std::memory_order_relaxed));
    return r;
}

// The core of the fatal path -- gate, hand off to the handler thread, wait, return the report. Shared by the real
// filter and the test seam so both exercise identical logic. tid is the faulting thread's id.
crd::crash::CrashReport handle_fatal(EXCEPTION_POINTERS* ep, DWORD tid) noexcept
{
    using crd::crash::CrashReport;
    using crd::crash::WriteResult;

    if (s_output_dir_w[0] == L'\0' || s_handler_thread == nullptr)
    {
        CrashReport r{};
        r.faulting_tid              = tid;
        r.write                     = WriteResult::NotInstalled;
        const EXCEPTION_RECORD* rec = (ep != nullptr) ? ep->ExceptionRecord : nullptr;
        r.code                      = (rec != nullptr) ? rec->ExceptionCode : 0UL;
        r.address                   = (rec != nullptr) ? rec->ExceptionAddress : nullptr;
        return r;
    }

    // The handler thread itself faulted (e.g. inside DbgHelp): it cannot signal itself and wait. Fall back to a
    // direct in-thread dump.
    if (tid == s_handler_tid)
    {
        emit_cstr(GetStdHandle(STD_ERROR_HANDLE), "[crd] fault on the crash handler thread; direct fallback\n");
        return direct_fallback_dump(ep, tid);
    }

    // Single-shot: the first fault to flip the gate owns the one dump.
    if (InterlockedCompareExchange(&s_gate, 1, 0) == 0)
    {
        s_req_ep  = ep;
        s_req_tid = tid;
        SetEvent(s_req_event);
        if (WaitForSingleObject(s_done_event, kHandlerWaitMs) == WAIT_OBJECT_0)
            return s_req_report; // published by the handler thread before it set s_done_event

        emit_cstr(GetStdHandle(STD_ERROR_HANDLE), "[crd] crash handler timed out; direct fallback\n");
        return direct_fallback_dump(ep, tid);
    }

    // A concurrent second fault: exactly one dump per process. Report Suppressed and notify, then wait for the
    // in-flight dump to finish before returning (returning first would let the OS kill the process mid-dump).
    CrashReport r{};
    r.faulting_tid              = tid;
    r.write                     = WriteResult::Suppressed;
    const EXCEPTION_RECORD* rec = (ep != nullptr) ? ep->ExceptionRecord : nullptr;
    r.code                      = (rec != nullptr) ? rec->ExceptionCode : 0UL;
    r.address                   = (rec != nullptr) ? rec->ExceptionAddress : nullptr;
    if (crd::crash::CrashReportHandler h = s_handler.load(std::memory_order_acquire); h != nullptr)
        h(r, s_handler_user.load(std::memory_order_relaxed));
    (void)WaitForSingleObject(s_done_event, kHandlerWaitMs);
    return r;
}

LONG CALLBACK crash_filter(EXCEPTION_POINTERS* ep) noexcept
{
    (void)handle_fatal(ep, GetCurrentThreadId());
    // Chain to whatever filter we replaced (a host/plugin/sanitizer filter) rather than silently dropping it;
    // with no previous filter the process still exits with the exception code.
    return (s_prev_filter != nullptr) ? s_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

// Register crash_tls_callback with the loader: a pointer in the CRT's TLS-callback section (.CRT$XLB sorts between
// the CRT's own XLA/XLZ bounds), plus linker includes so the TLS directory (_tls_used) and this pointer survive in a
// static library and under /OPT:REF. extern "C" keeps the x64 symbol name literal for the /INCLUDE directive.
#pragma section(".CRT$XLB", read)
extern "C" __declspec(allocate(".CRT$XLB")) const PIMAGE_TLS_CALLBACK kCrdCrashTlsCallback = &crash_tls_callback;
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:kCrdCrashTlsCallback")

namespace crd::crash
{

void set_crash_report_handler(CrashReportHandler handler, void* user) noexcept
{
    s_handler_user.store(user, std::memory_order_relaxed);
    s_handler.store(handler, std::memory_order_release);
}

InstallResult install(const char* output_dir) noexcept
{
    if (output_dir == nullptr)
        return InstallResult::OutputDirUnusable;

    const int wn = MultiByteToWideChar(CP_UTF8, 0, output_dir, -1, s_scratch_a, static_cast<int>(kPathCap));
    if (wn == 0)
        return InstallResult::OutputDirUnusable;

    const DWORD fn = GetFullPathNameW(s_scratch_a, static_cast<DWORD>(kPathCap), s_scratch_b, nullptr);
    if (fn == 0)
        return InstallResult::OutputDirUnusable;
    if (fn >= kPathCap)
        return InstallResult::PathTooLong;

    // Strip a single trailing separator (GetFullPathNameW rarely leaves one, but a user path may).
    if (fn >= 2 && s_scratch_b[fn - 1] == L'\\' && s_scratch_b[fn - 2] != L':')
        s_scratch_b[fn - 1] = L'\0';

    if (!build_prefixed(s_scratch_b, s_output_dir_w))
    {
        s_output_dir_w[0] = L'\0';
        return InstallResult::PathTooLong;
    }

    if (CreateDirectoryW(s_output_dir_w, nullptr) == FALSE)
    {
        const DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS)
        {
            s_output_dir_w[0] = L'\0';
            return InstallResult::OutputDirUnusable;
        }
    }
    // ERROR_ALREADY_EXISTS is also returned when the path names a *file*; confirm it is a directory.
    const DWORD attrs = GetFileAttributesW(s_output_dir_w);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0U)
    {
        s_output_dir_w[0] = L'\0';
        return InstallResult::OutputDirUnusable;
    }

    // Create the dedicated handler thread + its events once, before arming the filter (never in the filter itself:
    // CreateThread under a crash inside loader/DllMain code would deadlock on the loader lock).
    if (s_handler_thread == nullptr)
    {
        s_req_event  = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset: one setter, one consumer
        s_done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset: releases the winner and any loser
        s_quit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset
        if (s_req_event == nullptr || s_done_event == nullptr || s_quit_event == nullptr)
        {
            if (s_req_event != nullptr)
                (void)CloseHandle(s_req_event);
            if (s_done_event != nullptr)
                (void)CloseHandle(s_done_event);
            if (s_quit_event != nullptr)
                (void)CloseHandle(s_quit_event);
            s_req_event = s_done_event = s_quit_event = nullptr;
            s_output_dir_w[0]                         = L'\0';
            return InstallResult::FilterInstallFailed;
        }
        s_handler_thread = CreateThread(nullptr, 1UL << 20, &handler_thread_proc, nullptr, 0, &s_handler_tid);
        if (s_handler_thread == nullptr)
        {
            (void)CloseHandle(s_req_event);
            (void)CloseHandle(s_done_event);
            (void)CloseHandle(s_quit_event);
            s_req_event = s_done_event = s_quit_event = nullptr;
            s_output_dir_w[0]                         = L'\0';
            return InstallResult::FilterInstallFailed;
        }
    }

    // A fresh install is a fresh single-shot generation.
    (void)InterlockedExchange(&s_gate, 0);
    (void)ResetEvent(s_done_event);
    s_live_dumps.store(0U, std::memory_order_relaxed);
#if CRD_ENABLE_ASSERTS
    s_inject_step.store(WriteResult::Ok, std::memory_order_relaxed); // no test injection carries across an install
#endif

    guard_current_thread_stack(); // reserve last-chance stack for the installing thread
    s_auto_guard.store(true, std::memory_order_release); // and for every thread created from now on (TLS callback)

    const LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(&crash_filter);
    if (!s_have_prev)
    {
        s_prev_filter = prev; // first install: the genuine previous filter
        s_have_prev   = true;
        return InstallResult::Ok;
    }
    if (prev == &crash_filter)
        return InstallResult::OkReinstalled; // still ours; keep the originally saved previous
    return InstallResult::OkReplacedForeignFilter; // a foreign filter had displaced us; do not adopt it
}

void uninstall() noexcept
{
    s_auto_guard.store(false, std::memory_order_release); // threads created from now on are not guarded
    if (s_have_prev)
    {
        (void)SetUnhandledExceptionFilter(s_prev_filter);
        s_prev_filter = nullptr;
        s_have_prev   = false;
    }
    if (s_handler_thread != nullptr)
    {
        (void)SetEvent(s_quit_event);
        (void)WaitForSingleObject(s_handler_thread, 5000);
        (void)CloseHandle(s_handler_thread);
        (void)CloseHandle(s_req_event);
        (void)CloseHandle(s_done_event);
        (void)CloseHandle(s_quit_event);
        s_handler_thread = nullptr;
        s_req_event = s_done_event = s_quit_event = nullptr;
        s_handler_tid              = 0;
    }
    (void)InterlockedExchange(&s_gate, 0);
    s_output_dir_w[0] = L'\0';
}

void guard_current_thread_stack(std::uint32_t reserve_bytes) noexcept
{
    ULONG bytes = reserve_bytes;
    (void)SetThreadStackGuarantee(&bytes); // best-effort; a failure here is non-fatal
}

WriteResult capture_dump(const DumpNote& note, const wchar_t** out_path) noexcept
{
    if (s_output_dir_w[0] == L'\0')
        return WriteResult::NotInstalled;
    // Bounded: a runaway observer cannot fill the disk with live dumps. The fatal single-shot gate is separate.
    if (s_live_dumps.fetch_add(1U, std::memory_order_relaxed) >= kMaxLiveDumps)
        return WriteResult::Suppressed;

    DWORD last_error = 0;
    AcquireSRWLockExclusive(&s_dump_lock);
    const WriteResult wr = write_dump(nullptr, note.kind, &note, out_path, &last_error);
    ReleaseSRWLockExclusive(&s_dump_lock);
    return wr;
}

WriteResult capture_dump(const wchar_t** out_path) noexcept
{
    return capture_dump(DumpNote{DumpKind::Manual, nullptr, 0U}, out_path);
}

std::size_t read_dump_stream(const wchar_t* dump_path, std::uint32_t stream_type, void* out, std::size_t cap) noexcept
{
    if (dump_path == nullptr)
        return 0;
    HANDLE file = CreateFileW(dump_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr)
    {
        (void)CloseHandle(file);
        return 0;
    }
    void* base = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (base == nullptr)
    {
        (void)CloseHandle(mapping);
        (void)CloseHandle(file);
        return 0;
    }

    std::size_t          produced = 0;
    MINIDUMP_DIRECTORY*  dir      = nullptr;
    void*                stream   = nullptr;
    ULONG                size     = 0;
    if (MiniDumpReadDumpStream(base, stream_type, &dir, &stream, &size) != FALSE && stream != nullptr)
    {
        produced             = static_cast<std::size_t>(size);
        const std::size_t nc = (produced < cap) ? produced : cap;
        if (out != nullptr && nc > 0)
        {
            auto*       d = static_cast<unsigned char*>(out);
            const auto* s = static_cast<const unsigned char*>(stream);
            for (std::size_t i = 0; i < nc; ++i)
                d[i] = s[i];
        }
    }

    (void)UnmapViewOfFile(base);
    (void)CloseHandle(mapping);
    (void)CloseHandle(file);
    return produced;
}

#if CRD_ENABLE_ASSERTS
CrashReport test_fatal_path(std::uint32_t exception_code) noexcept
{
    CONTEXT ctx{};
    RtlCaptureContext(&ctx); // a real context for this thread; no fault is actually raised

    EXCEPTION_RECORD rec{};
    rec.ExceptionCode    = static_cast<DWORD>(exception_code);
    rec.ExceptionAddress = &rec; // a valid, non-null address (no function-pointer cast)

    EXCEPTION_POINTERS ep{};
    ep.ExceptionRecord = &rec;
    ep.ContextRecord   = &ctx;

    // Straight into the fatal core -- NOT crash_filter, so the test never chains to Catch2's own fatal filter.
    return handle_fatal(&ep, GetCurrentThreadId());
}

void test_inject_write_failure(WriteResult step) noexcept
{
    s_inject_step.store(step, std::memory_order_relaxed);
}
#endif

} // namespace crd::crash

// ---- Linux -----------------------------------------------------------------
#elif defined(__linux__)

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring> // memcpy/memcmp for the build-id note walk (DIAG.5d(c2))
#include <ctime>
#include <fcntl.h>
#include <link.h> // dl_iterate_phdr, ElfW, struct dl_phdr_info, PT_NOTE/NT_GNU_BUILD_ID (via <elf.h>) -- DIAG.5d(c2)
#include <sys/stat.h>
#include <sys/syscall.h> // SYS_gettid
#include <ucontext.h>    // ucontext_t / gregs (g++/clang++ predefine _GNU_SOURCE on Linux)
#include <unistd.h>

namespace
{

// -- Install-time state: populated before any fault; only READ (never written) in the signal handler. --
char        s_output_dir[4096] = ""; // cached at install(); the dir is mkdir'd there, never in the handler
char        s_exe_path[4096]   = ""; // readlink("/proc/self/exe") cached at install() -- the "identified binary"
std::size_t s_exe_path_len     = 0;
bool        s_have_prev        = false;

// -- DIAG.5d(c2): per-module ELF build-id, captured at install() (dl_iterate_phdr takes the loader lock and may
// allocate -- forbidden in the async-signal handler), then only READ when the record is written. The build-id is the
// Linux analogue of the Windows RSDS GUID: it lets an offline symbolizer find the matching debug file and REJECT a
// wrong one. This slice ends at "the record carries it"; turning the record into a SymbolIndex section is a later
// composer's job. --
constexpr std::size_t kMaxModuleNotes = 64;   // bounded; a busier process marks s_modules_overflow
constexpr std::size_t kBuildIdBytes   = 20;   // GNU build-id is SHA-1 (20 B) by default; longer ids are truncated
constexpr std::size_t kModulePathCap  = 128;  // basename only

struct ModuleNote
{
    std::uintptr_t base;
    unsigned char  id[kBuildIdBytes];
    unsigned char  id_len;
    char           path[kModulePathCap]; // basename; "" for the main executable
};

ModuleNote    s_modules[kMaxModuleNotes];
std::size_t   s_module_count     = 0;
bool          s_modules_overflow = false;
unsigned char s_exe_build_id[kBuildIdBytes] = {};
unsigned char s_exe_build_id_len            = 0;

// Walk the PT_NOTE segments of one loaded object for NT_GNU_BUILD_ID. Bounds-checked against p_memsz so a malformed
// or truncated note can never overrun. Returns the number of build-id bytes copied into out (0 if none).
std::size_t extract_build_id(const ElfW(Phdr)* phdr, int phnum, ElfW(Addr) load_base, unsigned char* out,
                             std::size_t out_cap) noexcept
{
    for (int i = 0; i < phnum; ++i)
    {
        if (phdr[i].p_type != PT_NOTE)
            continue;
        const unsigned char* p   = reinterpret_cast<const unsigned char*>(load_base + phdr[i].p_vaddr);
        std::size_t          rem = static_cast<std::size_t>(phdr[i].p_memsz);
        while (rem >= sizeof(ElfW(Nhdr)))
        {
            ElfW(Nhdr) nh{};
            std::memcpy(&nh, p, sizeof(nh));
            const std::size_t name_pad = (static_cast<std::size_t>(nh.n_namesz) + 3U) & ~static_cast<std::size_t>(3);
            const std::size_t desc_pad = (static_cast<std::size_t>(nh.n_descsz) + 3U) & ~static_cast<std::size_t>(3);
            const std::size_t total    = sizeof(ElfW(Nhdr)) + name_pad + desc_pad;
            if (total > rem || total < sizeof(ElfW(Nhdr))) // second test guards a wrapped add
                break;
            if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4U &&
                std::memcmp(p + sizeof(ElfW(Nhdr)), "GNU", 4) == 0)
            {
                std::size_t take = nh.n_descsz;
                if (take > out_cap)
                    take = out_cap;
                std::memcpy(out, p + sizeof(ElfW(Nhdr)) + name_pad, take);
                return take;
            }
            p += total;
            rem -= total;
        }
    }
    return 0;
}

// dl_iterate_phdr callback: record one module's base + build-id + basename. Runs at install() only.
int phdr_cb(struct dl_phdr_info* info, size_t /*size*/, void* /*data*/) noexcept
{
    if (s_module_count >= kMaxModuleNotes)
    {
        s_modules_overflow = true;
        return 0;
    }
    unsigned char     id[kBuildIdBytes];
    const std::size_t idlen = extract_build_id(info->dlpi_phdr, info->dlpi_phnum, info->dlpi_addr, id, sizeof(id));

    ModuleNote& m = s_modules[s_module_count];
    m.base        = static_cast<std::uintptr_t>(info->dlpi_addr);
    m.id_len      = static_cast<unsigned char>(idlen);
    std::memcpy(m.id, id, idlen);

    const char* name = (info->dlpi_name != nullptr) ? info->dlpi_name : "";
    const char* bn   = name;
    for (const char* q = name; *q != '\0'; ++q)
        if (*q == '/')
            bn = q + 1;
    std::size_t j = 0;
    for (; bn[j] != '\0' && j + 1U < kModulePathCap; ++j)
        m.path[j] = bn[j];
    m.path[j] = '\0';

    if (name[0] == '\0' && idlen != 0U) // dlpi_name == "" is the main executable
    {
        std::memcpy(s_exe_build_id, id, idlen);
        s_exe_build_id_len = static_cast<unsigned char>(idlen);
    }
    ++s_module_count;
    return 0;
}

// Populate the module-note table. Called from install() (never the handler).
void capture_module_notes() noexcept
{
    s_module_count     = 0;
    s_modules_overflow = false;
    s_exe_build_id_len = 0;
    (void)dl_iterate_phdr(&phdr_cb, nullptr);
}

struct sigaction s_prev_sigsegv{};
struct sigaction s_prev_sigabrt{};
struct sigaction s_prev_sigfpe{};
struct sigaction s_prev_sigill{};

std::atomic<crd::crash::CrashReportHandler> s_handler{nullptr};
std::atomic<void*>                          s_handler_user{nullptr};
std::atomic<unsigned>                       s_record_serial{0};
static_assert(std::atomic<unsigned>::is_always_lock_free, "the crash serial must be lock-free for the handler");

// Single-shot concurrent-fault gate: the first faulting thread (any of the four signals) wins and writes the one
// record; a simultaneous second fault on another thread is honestly Suppressed but still waits for the winner's
// record to land before it chains, so the process is not torn down mid-write. Both must be lock-free to be usable
// from a signal handler.
std::atomic<int>  s_gate{0};      // 0 = free; a CAS to 1 claims the single record slot
std::atomic<bool> s_done{false};  // set by the winner once its record is fsync'd -- the loser waits on this
static_assert(std::atomic<int>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
              "the crash gate must be lock-free for the handler");

// Per-thread alternate signal stack. A stack-EXHAUSTED fault has no room on its own stack to run the handler, so
// SA_ONSTACK diverts the handler onto this buffer. thread_local => zero-initialised .tbss (no malloc, nothing to
// free); each thread that may fault registers it via guard_current_thread_stack(). Fixed 64 KiB deliberately:
// SIGSTKSZ is not a constant expression on glibc >= 2.34, so a static-sized buffer cannot use it.
constexpr std::size_t                  kAltStackBytes = 64U * 1024U;
alignas(64) thread_local unsigned char t_alt_stack[kAltStackBytes];
thread_local bool                      t_alt_installed = false; // this thread has an alternate stack for SA_ONSTACK
thread_local bool                      t_alt_owned     = false; // ...and it is t_alt_stack (ours to disable)

// -- Async-signal-safe primitives: format into a fixed buffer (no libc formatting) then write(2). --

// Append the C-string s into buf[.. cap), advancing n; clamped to leave room for a terminator. Pure memory.
std::size_t ap_str(char* buf, std::size_t cap, std::size_t n, const char* s) noexcept
{
    while (*s != '\0' && n + 1U < cap)
        buf[n++] = *s++;
    return n;
}

// Append v as decimal.
std::size_t ap_dec(char* buf, std::size_t cap, std::size_t n, std::uint64_t v) noexcept
{
    char        tmp[20];
    std::size_t t = 0;
    if (v == 0U)
        tmp[t++] = '0';
    while (v != 0U)
    {
        tmp[t++] = static_cast<char>('0' + static_cast<int>(v % 10U));
        v /= 10U;
    }
    while (t != 0U && n + 1U < cap)
        buf[n++] = tmp[--t];
    return n;
}

// Append v as 0x-prefixed hex (no leading-zero padding).
std::size_t ap_hex(char* buf, std::size_t cap, std::size_t n, std::uint64_t v) noexcept
{
    n = ap_str(buf, cap, n, "0x");
    char        tmp[16];
    std::size_t t = 0;
    if (v == 0U)
        tmp[t++] = '0';
    while (v != 0U)
    {
        const int d = static_cast<int>(v & 0xFU);
        tmp[t++]    = static_cast<char>(d < 10 ? ('0' + d) : ('a' + d - 10));
        v >>= 4U;
    }
    while (t != 0U && n + 1U < cap)
        buf[n++] = tmp[--t];
    return n;
}

// Append `len` bytes as fixed 2-hex-digit-per-byte, no 0x prefix (the build-id encoding). Pure memory.
std::size_t ap_hex_bytes(char* buf, std::size_t cap, std::size_t n, const unsigned char* p, std::size_t len) noexcept
{
    for (std::size_t i = 0; i < len; ++i)
    {
        const int hi = (p[i] >> 4) & 0xF;
        const int lo = p[i] & 0xF;
        if (n + 1U < cap)
            buf[n++] = static_cast<char>(hi < 10 ? ('0' + hi) : ('a' + hi - 10));
        if (n + 1U < cap)
            buf[n++] = static_cast<char>(lo < 10 ? ('0' + lo) : ('a' + lo - 10));
    }
    return n;
}

// write() the whole buffer, retrying short writes and EINTR. false on any hard error.
bool write_all(int fd, const char* p, std::size_t n) noexcept
{
    std::size_t off = 0;
    while (off < n)
    {
        const ssize_t w = write(fd, p + off, n - off);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (w == 0)
            return false;
        off += static_cast<std::size_t>(w);
    }
    return true;
}

std::uint64_t mono_ns() noexcept // a monotonic stamp for the record filename -- async-signal-safe
{
    struct timespec ts{};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

long current_tid() noexcept
{
    return static_cast<long>(syscall(SYS_gettid)); // gettid: async-signal-safe, no glibc-version dependency
}

// Write ONE crash record file: the signal, si_code/si_addr, the ORIGINAL registers and the process/thread identity,
// into a collision-safe O_EXCL file. Async-signal-safe (hand-formatted, no libc formatting). Returns the WriteResult;
// fills out_path with the (attempted) path and *out_err with the failing errno. It does NOT walk or symbolize the
// stack -- backtrace (via dl_iterate_phdr) takes the loader lock and can allocate, which the design forbids in
// compromised execution ("Symbolization/backtraces occur outside compromised execution"); the call stack is recovered
// offline from the OS core, symbolized in a later slice.
crd::crash::WriteResult write_crash_record(int sig, void* ctx, long tid, std::uint64_t addr, int si_code,
                                           char* out_path, std::size_t out_cap, int* out_err) noexcept
{
    int                 fd    = -1;
    int                 err   = 0;
    const std::uint64_t stamp = mono_ns();
    for (int attempt = 0; attempt < 4096 && fd < 0; ++attempt) // <dir>/crash_<pid>_<tid>_<ns-hex>_<serial>.log, O_EXCL
    {
        std::size_t n = 0;
        n             = ap_str(out_path, out_cap, n, s_output_dir);
        n             = ap_str(out_path, out_cap, n, "/crash_");
        n             = ap_dec(out_path, out_cap, n, static_cast<std::uint64_t>(getpid()));
        n             = ap_str(out_path, out_cap, n, "_");
        n             = ap_dec(out_path, out_cap, n, static_cast<std::uint64_t>(tid));
        n             = ap_str(out_path, out_cap, n, "_");
        n             = ap_hex(out_path, out_cap, n, stamp);
        n             = ap_str(out_path, out_cap, n, "_");
        n             = ap_dec(out_path, out_cap, n, s_record_serial.fetch_add(1U, std::memory_order_relaxed));
        n             = ap_str(out_path, out_cap, n, ".log");
        out_path[n]   = '\0';

        fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0 && errno != EEXIST)
        {
            err = errno;
            break;
        }
    }
    if (fd < 0)
    {
        *out_err = err;
        return crd::crash::WriteResult::OpenFailed;
    }

    // Assemble the whole record on the alt stack, then one durable write + fsync.
    char        rec[8192];
    std::size_t n = 0;
    n             = ap_str(rec, sizeof(rec), n, "crd crash record\npid ");
    n             = ap_dec(rec, sizeof(rec), n, static_cast<std::uint64_t>(getpid()));
    n             = ap_str(rec, sizeof(rec), n, " tid ");
    n             = ap_dec(rec, sizeof(rec), n, static_cast<std::uint64_t>(tid));
    n             = ap_str(rec, sizeof(rec), n, "\nsignal ");
    n             = ap_dec(rec, sizeof(rec), n, static_cast<std::uint64_t>(sig));
    n             = ap_str(rec, sizeof(rec), n, " code ");
    n             = ap_dec(rec, sizeof(rec), n, static_cast<std::uint64_t>(static_cast<unsigned>(si_code)));
    n             = ap_str(rec, sizeof(rec), n, " addr ");
    n             = ap_hex(rec, sizeof(rec), n, addr);
    n             = ap_str(rec, sizeof(rec), n, "\nexe ");
    if (s_exe_path_len != 0U)
        n = ap_str(rec, sizeof(rec), n, s_exe_path);
    n = ap_str(rec, sizeof(rec), n, "\nregs ");

    auto* uc = static_cast<ucontext_t*>(ctx);
#if defined(__x86_64__)
    n = ap_str(rec, sizeof(rec), n, "x86_64\n");
    if (uc != nullptr)
    {
        static const char* const kNames[NGREG] = {
            "r8",  "r9",  "r10", "r11",    "r12", "r13",    "r14",    "r15",     "rdi", "rsi",    "rbp", "rbx",
            "rdx", "rax", "rcx", "rsp",    "rip", "efl",    "csgsfs", "err",     "trapno", "oldmask", "cr2"};
        for (int i = 0; i < NGREG; ++i)
        {
            n = ap_str(rec, sizeof(rec), n, kNames[i]);
            n = ap_str(rec, sizeof(rec), n, " ");
            n = ap_hex(rec, sizeof(rec), n, static_cast<std::uint64_t>(uc->uc_mcontext.gregs[i]));
            n = ap_str(rec, sizeof(rec), n, ((i % 4) == 3) ? "\n" : " ");
        }
        n = ap_str(rec, sizeof(rec), n, "\n");
    }
#elif defined(__aarch64__)
    n = ap_str(rec, sizeof(rec), n, "aarch64\n");
    if (uc != nullptr)
    {
        for (int i = 0; i < 31; ++i)
        {
            n = ap_str(rec, sizeof(rec), n, "x");
            n = ap_dec(rec, sizeof(rec), n, static_cast<std::uint64_t>(i));
            n = ap_str(rec, sizeof(rec), n, " ");
            n = ap_hex(rec, sizeof(rec), n, static_cast<std::uint64_t>(uc->uc_mcontext.regs[i]));
            n = ap_str(rec, sizeof(rec), n, ((i % 4) == 3) ? "\n" : " ");
        }
        n = ap_str(rec, sizeof(rec), n, "\nsp ");
        n = ap_hex(rec, sizeof(rec), n, static_cast<std::uint64_t>(uc->uc_mcontext.sp));
        n = ap_str(rec, sizeof(rec), n, " pc ");
        n = ap_hex(rec, sizeof(rec), n, static_cast<std::uint64_t>(uc->uc_mcontext.pc));
        n = ap_str(rec, sizeof(rec), n, "\n");
    }
#else
    n = ap_str(rec, sizeof(rec), n, "unsupported-arch\n");
    (void)uc;
#endif

    // DIAG.5d(c2): binary identities, captured at install() (static reads only -- async-signal-safe). Appended AFTER
    // the 5b fields so the existing record parsers are unaffected. The exe build_id is the load-bearing line; the
    // module list is bounded-best-effort (honest truncation markers), and basenames keep each line small.
    if (s_exe_build_id_len != 0U)
    {
        n = ap_str(rec, sizeof(rec), n, "build_id ");
        n = ap_hex_bytes(rec, sizeof(rec), n, s_exe_build_id, s_exe_build_id_len);
        n = ap_str(rec, sizeof(rec), n, "\n");
    }
    n = ap_str(rec, sizeof(rec), n, "modules ");
    n = ap_dec(rec, sizeof(rec), n, s_module_count);
    if (s_modules_overflow)
        n = ap_str(rec, sizeof(rec), n, " truncated 1");
    n = ap_str(rec, sizeof(rec), n, "\n");
    for (std::size_t i = 0; i < s_module_count; ++i)
    {
        if (n + 300U >= sizeof(rec)) // leave headroom; mark that the record buffer, not the table, cut the list
        {
            n = ap_str(rec, sizeof(rec), n, "modules_record_truncated 1\n");
            break;
        }
        n = ap_str(rec, sizeof(rec), n, "module ");
        n = ap_hex(rec, sizeof(rec), n, static_cast<std::uint64_t>(s_modules[i].base));
        n = ap_str(rec, sizeof(rec), n, " ");
        if (s_modules[i].id_len != 0U)
            n = ap_hex_bytes(rec, sizeof(rec), n, s_modules[i].id, s_modules[i].id_len);
        else
            n = ap_str(rec, sizeof(rec), n, "-"); // no build-id for this module (refuse, don't guess)
        n = ap_str(rec, sizeof(rec), n, " ");
        n = ap_str(rec, sizeof(rec), n, (s_modules[i].path[0] != '\0') ? s_modules[i].path : "(exe)");
        n = ap_str(rec, sizeof(rec), n, "\n");
    }

    const bool ok = write_all(fd, rec, n) && (fsync(fd) == 0);
    if (!ok)
        err = errno; // capture the write/fsync errno before close() can change it
    (void)close(fd);
    if (!ok)
    {
        (void)unlink(out_path); // no plausible partial record left behind
        *out_err = err;
        return crd::crash::WriteResult::DumpFailed;
    }
    *out_err = 0;
    return crd::crash::WriteResult::Ok;
}

// The signal handler: async-signal-safe ONLY. A single-shot gate makes exactly one thread write the record under
// concurrent faults; the loser is honestly Suppressed but waits for the record to land before chaining. Both branches
// chain to the previously-installed handler and re-raise, so the process terminates with the original fault reason
// (the harness reads it from WTERMSIG). dump_path stays nullptr (a Windows wide minidump path); the record path is
// reported to stderr instead.
void crash_signal_handler(int sig, siginfo_t* info, void* ctx) noexcept
{
    const long          tid = current_tid();
    const std::uint64_t addr =
        (info != nullptr) ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info->si_addr)) : 0U;
    const int si_code = (info != nullptr) ? info->si_code : 0;

    int expected = 0;
    if (s_gate.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) // winner: write the one record
    {
        char                          path[4200];
        int                           err = 0;
        const crd::crash::WriteResult wr  = write_crash_record(sig, ctx, tid, addr, si_code, path, sizeof(path), &err);

        // A minimal emergency line to stderr -- "record:" (with the path) ONLY on a complete record; "FAILED"
        // otherwise. A failed write can never print success (mirrors the Windows fatal path).
        if (wr == crd::crash::WriteResult::Ok)
        {
            char        msg[4300];
            std::size_t m = 0;
            m             = ap_str(msg, sizeof(msg), m, "[crd] crash record: ");
            m             = ap_str(msg, sizeof(msg), m, path);
            m             = ap_str(msg, sizeof(msg), m, "\n");
            (void)write_all(STDERR_FILENO, msg, m);
        }
        else
        {
            char        msg[64];
            std::size_t m = 0;
            m             = ap_str(msg, sizeof(msg), m, "[crd] crash record FAILED (errno ");
            m             = ap_dec(msg, sizeof(msg), m, static_cast<std::uint64_t>(static_cast<unsigned>(err)));
            m             = ap_str(msg, sizeof(msg), m, ")\n");
            (void)write_all(STDERR_FILENO, msg, m);
        }

        if (crd::crash::CrashReportHandler h = s_handler.load(std::memory_order_acquire); h != nullptr)
        {
            crd::crash::CrashReport report{};
            report.code         = static_cast<std::uint32_t>(sig);
            report.address      = (info != nullptr) ? info->si_addr : nullptr;
            report.faulting_tid = static_cast<std::uint32_t>(tid);
            report.write        = wr;
            report.dump_path    = nullptr; // the Linux record path is reported to stderr, not via a wide dump_path
            report.last_error   = static_cast<std::uint32_t>(static_cast<unsigned>(err));
            h(report, s_handler_user.load(std::memory_order_relaxed));
        }
        s_done.store(true, std::memory_order_release); // record + hook complete -> release any waiting loser
    }
    else // loser: a concurrent second fault. Do not race on the output; wait (bounded) then report Suppressed.
    {
        // Returning/chaining before the winner finishes could let the OS tear the process down mid-write; wait ~5 s so
        // a stuck winner/hook still yields bounded termination.
        for (int i = 0; i < 5000 && !s_done.load(std::memory_order_acquire); ++i)
        {
            struct timespec ts{};
            ts.tv_sec  = 0;
            ts.tv_nsec = 1000000L; // 1 ms
            (void)nanosleep(&ts, nullptr);
        }
        if (crd::crash::CrashReportHandler h = s_handler.load(std::memory_order_acquire); h != nullptr)
        {
            crd::crash::CrashReport report{};
            report.code         = static_cast<std::uint32_t>(sig);
            report.address      = (info != nullptr) ? info->si_addr : nullptr;
            report.faulting_tid = static_cast<std::uint32_t>(tid);
            report.write        = crd::crash::WriteResult::Suppressed;
            report.dump_path    = nullptr;
            report.last_error   = 0U;
            h(report, s_handler_user.load(std::memory_order_relaxed));
        }
        char        msg[64];
        std::size_t m = 0;
        m             = ap_str(msg, sizeof(msg), m, "[crd] crash record suppressed (concurrent fault)\n");
        (void)write_all(STDERR_FILENO, msg, m);
    }

    // Chain to the previously-installed handler, then re-raise: a default previous disposition terminates with the
    // original signal (retained fault reason, read from WTERMSIG); a sanitizer's previous handler also gets to report.
    // The handler is NOT SA_RESETHAND -- that resets the disposition process-wide on entry, so a concurrent same-signal
    // fault on another thread would take SIG_DFL and kill the process mid-record. Instead the four fault signals are
    // blocked in sa_mask during the handler, so a RECURSIVE fault on this thread is forced to SIG_DFL (bounded, no
    // re-entry); restoring prev here replaces our handler so the re-raise reaches the correct next link.
    struct sigaction* prev = nullptr;
    switch (sig)
    {
    case SIGSEGV: prev = &s_prev_sigsegv; break;
    case SIGABRT: prev = &s_prev_sigabrt; break;
    case SIGFPE: prev = &s_prev_sigfpe; break;
    case SIGILL: prev = &s_prev_sigill; break;
    default: break;
    }
    if (prev != nullptr)
        (void)sigaction(sig, prev, nullptr);
    (void)raise(sig);
}

} // namespace

namespace crd::crash
{

void set_crash_report_handler(CrashReportHandler handler, void* user) noexcept
{
    s_handler_user.store(user, std::memory_order_relaxed);
    s_handler.store(handler, std::memory_order_release);
}

InstallResult install(const char* output_dir) noexcept
{
    if (output_dir == nullptr)
        return InstallResult::OutputDirUnusable;

    std::size_t di = 0; // bounded manual copy (avoids strncpy truncation diagnostics under -Werror)
    for (; output_dir[di] != '\0' && di + 1U < sizeof(s_output_dir); ++di)
        s_output_dir[di] = output_dir[di];
    s_output_dir[di] = '\0';

    if (mkdir(s_output_dir, 0755) != 0 && errno != EEXIST) // created ONCE here, never in the async-signal handler
    {
        s_output_dir[0] = '\0';
        return InstallResult::OutputDirUnusable;
    }
    struct stat st{};
    if (stat(s_output_dir, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        s_output_dir[0] = '\0';
        return InstallResult::OutputDirUnusable;
    }

    // Cache the executable path once (readlink is not async-signal-safe, so it must not run in the handler): the
    // "correctly identified binary" the record names. Symbol resolution against it is done offline in a later slice.
    const ssize_t ep = readlink("/proc/self/exe", s_exe_path, sizeof(s_exe_path) - 1);
    if (ep > 0)
    {
        s_exe_path[ep] = '\0';
        s_exe_path_len = static_cast<std::size_t>(ep);
    }
    else
    {
        s_exe_path[0]  = '\0';
        s_exe_path_len = 0;
    }

    // Cache each loaded module's ELF build-id ONCE here (dl_iterate_phdr takes the loader lock / may allocate, both
    // forbidden in the handler); the record reads this table. Re-install re-captures (cheap; module set may have grown).
    capture_module_notes();

    guard_current_thread_stack(0U); // the installing thread gets its alternate signal stack (SA_ONSTACK target)

    s_gate.store(0, std::memory_order_relaxed); // fresh generation: no fault has claimed the record slot yet
    s_done.store(false, std::memory_order_relaxed);

    struct sigaction sa{};
    sa.sa_sigaction = crash_signal_handler;
    // ONSTACK: run the handler on the alt stack so an exhausted stack is still captured. NOT RESETHAND: it resets the
    // disposition process-wide on entry, so a concurrent same-signal fault on another thread would take SIG_DFL and
    // kill the process mid-record. Instead block all four fault signals during the handler (below): a recursive fault
    // on the handling thread is then forced to SIG_DFL (bounded, no re-entry), while other threads' faults still reach
    // the handler and hit the gate (the mask is per-thread).
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    (void)sigaddset(&sa.sa_mask, SIGSEGV);
    (void)sigaddset(&sa.sa_mask, SIGABRT);
    (void)sigaddset(&sa.sa_mask, SIGFPE);
    (void)sigaddset(&sa.sa_mask, SIGILL);

    const bool first = !s_have_prev;
    if (first)
    {
        if (sigaction(SIGSEGV, &sa, &s_prev_sigsegv) != 0 || sigaction(SIGABRT, &sa, &s_prev_sigabrt) != 0 ||
            sigaction(SIGFPE, &sa, &s_prev_sigfpe) != 0 || sigaction(SIGILL, &sa, &s_prev_sigill) != 0)
        {
            s_output_dir[0] = '\0';
            return InstallResult::FilterInstallFailed;
        }
        s_have_prev = true;
        return InstallResult::Ok;
    }
    // Re-install without clobbering the originally saved previous handlers.
    if (sigaction(SIGSEGV, &sa, nullptr) != 0 || sigaction(SIGABRT, &sa, nullptr) != 0 ||
        sigaction(SIGFPE, &sa, nullptr) != 0 || sigaction(SIGILL, &sa, nullptr) != 0)
        return InstallResult::FilterInstallFailed;
    return InstallResult::OkReinstalled;
}

void uninstall() noexcept
{
    if (s_have_prev)
    {
        (void)sigaction(SIGSEGV, &s_prev_sigsegv, nullptr);
        (void)sigaction(SIGABRT, &s_prev_sigabrt, nullptr);
        (void)sigaction(SIGFPE, &s_prev_sigfpe, nullptr);
        (void)sigaction(SIGILL, &s_prev_sigill, nullptr);
        s_have_prev = false;
    }
    s_output_dir[0]    = '\0';
    s_exe_path[0]      = '\0';
    s_exe_path_len     = 0;
    s_module_count     = 0;
    s_modules_overflow = false;
    s_exe_build_id_len = 0;
    s_gate.store(0, std::memory_order_relaxed);
    s_done.store(false, std::memory_order_relaxed);

    if (t_alt_owned) // disable OUR alt stack on this thread; a borrowed one stays with its owner
    {
        stack_t ss{};
        ss.ss_flags = SS_DISABLE;
        (void)sigaltstack(&ss, nullptr);
        t_alt_owned = false;
    }
    t_alt_installed = false;
}

WriteResult capture_dump(const DumpNote& /*note*/, const wchar_t** /*out_path*/) noexcept
{
    return WriteResult::Unsupported; // no in-process minidump equivalent; core dumps are OS-driven
}

WriteResult capture_dump(const wchar_t** /*out_path*/) noexcept
{
    return WriteResult::Unsupported;
}

std::size_t read_dump_stream(const wchar_t* /*dump_path*/, std::uint32_t /*stream_type*/, void* /*out*/,
                             std::size_t /*cap*/) noexcept
{
    return 0; // minidump files are a Windows artifact
}

void guard_current_thread_stack(std::uint32_t /*reserve_bytes*/) noexcept
{
    // Register this thread's alternate signal stack so SA_ONSTACK can run the crash handler even when this thread's
    // own stack is exhausted. reserve_bytes is ignored (the Windows analog is a SetThreadStackGuarantee byte count;
    // here the alt stack is a fixed kAltStackBytes thread_local buffer). Idempotent per thread. Every thread that may
    // fault -- workers/fibers included (wired in a later sub-unit) -- must call this, or a stack-exhausted fault on it
    // cannot be recorded (there is no separate handler thread as on Windows).
    if (t_alt_installed)
        return;
    // Keep an alternate stack another runtime already installed on this thread: it owns that stack's lifetime.
    // AddressSanitizer gives every thread an mmap'ed alternate stack and munmaps whatever stack is current at thread
    // exit, so replacing it with t_alt_stack made ASan die with "Failed to munmap" on every worker exit (hosted
    // linux-gcc-asan, 2026-10-04, listing crd-stress-tests). SA_ONSTACK runs our handler on the existing stack.
    stack_t current{};
    if (sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) == 0 && current.ss_sp != nullptr)
    {
        t_alt_installed = true; // borrowed: uninstall() leaves it to its owner
        return;
    }
    stack_t ss{};
    ss.ss_sp    = t_alt_stack;
    ss.ss_size  = kAltStackBytes;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) == 0)
    {
        t_alt_installed = true;
        t_alt_owned     = true;
    }
}

#if CRD_ENABLE_ASSERTS
CrashReport test_fatal_path(std::uint32_t exception_code) noexcept
{
    CrashReport r{};
    r.code  = exception_code;
    r.write = WriteResult::Unsupported; // the synthetic-record path is Windows-only
    return r;
}

void test_inject_write_failure(WriteResult /*step*/) noexcept {} // no in-process minidump write to inject into
#endif

} // namespace crd::crash

// ---- Other platforms -------------------------------------------------------
#else

namespace crd::crash
{

void          set_crash_report_handler(CrashReportHandler, void*) noexcept {}
InstallResult install(const char*) noexcept { return InstallResult::Unsupported; }
void          uninstall() noexcept {}
WriteResult   capture_dump(const DumpNote&, const wchar_t**) noexcept { return WriteResult::Unsupported; }
WriteResult   capture_dump(const wchar_t**) noexcept { return WriteResult::Unsupported; }
std::size_t   read_dump_stream(const wchar_t*, std::uint32_t, void*, std::size_t) noexcept { return 0; }
void          guard_current_thread_stack(std::uint32_t) noexcept {}

#if CRD_ENABLE_ASSERTS
CrashReport test_fatal_path(std::uint32_t exception_code) noexcept
{
    CrashReport r{};
    r.code  = exception_code;
    r.write = WriteResult::Unsupported;
    return r;
}

void test_inject_write_failure(WriteResult /*step*/) noexcept {}
#endif

} // namespace crd::crash

#endif
