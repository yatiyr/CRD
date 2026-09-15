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
        prefix = L"\\hang_";
    else if (kind == crd::crash::DumpKind::Manual)
        prefix = L"\\live_";

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
        us.Buffer           = const_cast<void*>(note->evidence);
        usi.UserStreamCount = 1U;
        usi.UserStreamArray = &us;
        usi_ptr             = &usi;
    }

    BOOL  ok       = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                      static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithHandleData |
                                                                 MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo),
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
#include <cstdio>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

char s_output_dir[512] = "";
bool s_have_prev       = false;

struct sigaction s_prev_sigsegv{};
struct sigaction s_prev_sigabrt{};
struct sigaction s_prev_sigfpe{};
struct sigaction s_prev_sigill{};

std::atomic<crd::crash::CrashReportHandler> s_handler{nullptr};
std::atomic<void*>                          s_handler_user{nullptr};

// Only async-signal-safe functions used after the write() calls. (SA_ONSTACK / alternate-stack and
// install-failure hardening belong to the Linux crash-capture slice; this branch is left behaviourally
// as-is beyond returning results.)
void crash_signal_handler(int sig, siginfo_t* info, void* /*ctx*/) noexcept
{
    (void)mkdir(s_output_dir, 0755);

    char path[576];
    (void)snprintf(path, sizeof(path), "%s/crash_pid%d_sig%d.log", s_output_dir, static_cast<int>(getpid()), sig);

    char      header[128];
    const int header_len = snprintf(header, sizeof(header), "signal %d at %p\n", sig, info->si_addr);

    [[maybe_unused]] ssize_t r = write(STDERR_FILENO, header, static_cast<size_t>(header_len));

    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
        r = write(fd, header, static_cast<size_t>(header_len));

        void*     frames[64];
        const int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, fd);
        (void)close(fd);

        char      msg[576 + 32];
        const int msg_len = snprintf(msg, sizeof(msg), "[crd] crash log: %s\n", path);
        r                 = write(STDERR_FILENO, msg, static_cast<size_t>(msg_len));
    }

    if (crd::crash::CrashReportHandler h = s_handler.load(std::memory_order_acquire); h != nullptr)
    {
        crd::crash::CrashReport report{};
        report.code         = static_cast<std::uint32_t>(sig);
        report.address      = info->si_addr;
        report.faulting_tid = 0U;
        report.write        = crd::crash::WriteResult::Unsupported; // the .log is not a minidump
        report.dump_path    = nullptr;
        report.last_error   = 0U;
        h(report, s_handler_user.load(std::memory_order_relaxed));
    }

    // Re-raise with the default handler so the OS writes a core dump.
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    (void)sigaction(sig, &dfl, nullptr);
    raise(sig);
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

    strncpy(s_output_dir, output_dir, sizeof(s_output_dir) - 1);
    s_output_dir[sizeof(s_output_dir) - 1] = '\0';

    if (mkdir(s_output_dir, 0755) != 0 && errno != EEXIST)
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

    struct sigaction sa{};
    sa.sa_sigaction = crash_signal_handler;
    sa.sa_flags     = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

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
    s_output_dir[0] = '\0';
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
    // The alternate-signal-stack equivalent is handled by the Linux crash-capture slice; nothing to reserve here.
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
