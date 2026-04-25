#include "../log_formatter.hpp"

#include <crd/log/sinks/console_sink.hpp>

#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#define crd_isatty _isatty
#define crd_fileno _fileno
#else
#include <unistd.h>
#define crd_isatty isatty
#define crd_fileno fileno
#endif

namespace crd::log
{
namespace
{
bool stream_is_tty(std::FILE* f) noexcept
{
    return f && crd_isatty(crd_fileno(f)) != 0;
}

// Best-effort enable of ANSI virtual-terminal handling on a Windows console.
// No-op on other OSes; harmless if it fails.
void enable_vt_processing(std::FILE* f) noexcept
{
#if defined(_WIN32)
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(crd_fileno(f)));
    if (h == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode))
    {
        return;
    }
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    (void)SetConsoleMode(h, mode);
#else
    (void)f;
#endif
}
} // namespace

ConsoleSink::ConsoleSink() noexcept : ConsoleSink(false) {}

ConsoleSink::ConsoleSink(bool force_color) noexcept
{
    const bool stdout_tty = stream_is_tty(stdout);
    const bool stderr_tty = stream_is_tty(stderr);

    m_color = force_color || stdout_tty || stderr_tty;

    if (stdout_tty)
    {
        enable_vt_processing(stdout);
    }
    if (stderr_tty)
    {
        enable_vt_processing(stderr);
    }
}

void ConsoleSink::write(const LogRecord& rec)
{
    const std::string line = detail::format_record(rec, m_color, /*short_path*/ true);
    std::FILE* dst = (rec.level >= LogLevel::Error) ? stderr : stdout;
    std::fwrite(line.data(), 1, line.size(), dst);
    std::fputc('\n', dst);
}

void ConsoleSink::flush()
{
    std::fflush(stdout);
    std::fflush(stderr);
}
} // namespace crd::log
