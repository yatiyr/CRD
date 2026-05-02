#include <crd/core/crash.hpp>

// ---- Windows ---------------------------------------------------------------
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// DbgHelp must follow windows.h — its types depend on it.
#include <DbgHelp.h>

#include <cstdio>
#include <cstring>

namespace
{

char s_output_dir[MAX_PATH] = "./crashes";
LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter = nullptr;

LONG CALLBACK crash_filter(EXCEPTION_POINTERS* ep) noexcept
{
    CreateDirectoryA(s_output_dir, nullptr);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char path[MAX_PATH + 64];
    (void)snprintf(path, sizeof(path),
                   "%s/crash_%04d%02d%02d_%02d%02d%02d.dmp",
                   s_output_dir,
                   static_cast<int>(st.wYear),   static_cast<int>(st.wMonth),
                   static_cast<int>(st.wDay),    static_cast<int>(st.wHour),
                   static_cast<int>(st.wMinute), static_cast<int>(st.wSecond));

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers    = FALSE;

        MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            file,
            static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs     |
                                       MiniDumpWithHandleData    |
                                       MiniDumpWithFullMemoryInfo|
                                       MiniDumpWithThreadInfo),
            &mei, nullptr, nullptr);
        CloseHandle(file);
        std::fprintf(stderr, "[crd] crash dump: %s\n", path);
    }
    else
    {
        std::fprintf(stderr, "[crd] crash dump FAILED (error %lu): %s\n",
                     GetLastError(), path);
    }

    auto* rec = ep->ExceptionRecord;
    std::fprintf(stderr, "[crd] ExceptionCode=0x%08lX ExceptionAddress=%p\n",
                 rec->ExceptionCode, rec->ExceptionAddress);
    std::fflush(stderr);

    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

namespace crd::crash
{

void install(const char* output_dir) noexcept
{
    strncpy_s(s_output_dir, sizeof(s_output_dir), output_dir, _TRUNCATE);
    s_prev_filter = SetUnhandledExceptionFilter(&crash_filter);
}

void uninstall() noexcept
{
    SetUnhandledExceptionFilter(s_prev_filter);
    s_prev_filter = nullptr;
}

} // namespace crd::crash

// ---- Linux -----------------------------------------------------------------
#elif defined(__linux__)

#include <csignal>
#include <cstdio>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

char s_output_dir[512] = "./crashes";

struct sigaction s_prev_sigsegv{};
struct sigaction s_prev_sigabrt{};
struct sigaction s_prev_sigfpe{};
struct sigaction s_prev_sigill{};

// Only async-signal-safe functions used after the write() calls:
// open, write, close, backtrace, backtrace_symbols_fd, snprintf (accepted
// risk — glibc's snprintf is reentrant in practice for crash handlers).
void crash_signal_handler(int sig, siginfo_t* info, void* /*ctx*/) noexcept
{
    (void)mkdir(s_output_dir, 0755);

    char path[576];
    (void)snprintf(path, sizeof(path),
                   "%s/crash_pid%d_sig%d.log",
                   s_output_dir, static_cast<int>(getpid()), sig);

    char header[128];
    int header_len = snprintf(header, sizeof(header),
                              "signal %d at %p\n", sig, info->si_addr);

    (void)write(STDERR_FILENO, header, static_cast<size_t>(header_len));

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
        (void)write(fd, header, static_cast<size_t>(header_len));

        void* frames[64];
        int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, fd);
        (void)close(fd);

        char msg[576 + 32];
        int msg_len = snprintf(msg, sizeof(msg), "[crd] crash log: %s\n", path);
        (void)write(STDERR_FILENO, msg, static_cast<size_t>(msg_len));
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

void install(const char* output_dir) noexcept
{
    strncpy(s_output_dir, output_dir, sizeof(s_output_dir) - 1);
    s_output_dir[sizeof(s_output_dir) - 1] = '\0';

    struct sigaction sa{};
    sa.sa_sigaction = crash_signal_handler;
    sa.sa_flags     = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, &s_prev_sigsegv);
    sigaction(SIGABRT, &sa, &s_prev_sigabrt);
    sigaction(SIGFPE,  &sa, &s_prev_sigfpe);
    sigaction(SIGILL,  &sa, &s_prev_sigill);
}

void uninstall() noexcept
{
    sigaction(SIGSEGV, &s_prev_sigsegv, nullptr);
    sigaction(SIGABRT, &s_prev_sigabrt, nullptr);
    sigaction(SIGFPE,  &s_prev_sigfpe,  nullptr);
    sigaction(SIGILL,  &s_prev_sigill,  nullptr);
}

} // namespace crd::crash

#endif
