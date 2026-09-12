#include "../log_formatter.hpp"

#include <crd/log/sinks/debugger_sink.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace crd::log
{
void DebuggerSink::write(const LogRecord& rec)
{
#if defined(_WIN32)
    std::string line = detail::format_record(rec, /*color*/ false, /*short*/ true);
    line.push_back('\n');
    ::OutputDebugStringA(line.c_str());
#else
    (void)rec; // platform no-op
#endif
}

void DebuggerSink::flush()
{
    // OutputDebugString is unbuffered; nothing to do.
}
} // namespace crd::log
