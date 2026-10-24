// Out of memory while a sink formats a record (crd-log-fatal-* CTests, tests/foundation/log; DIAG.5c). The engine's
// sinks format into a crd String, whose refused allocation is its out-of-memory fatal; this sink formats the same way
// on a refusing allocator. The fatal fires inside the logger's sink delivery, with the sink lock held, so the assert
// bridge must suppress its own Critical record (logging would lock the sink lock again on this thread) and the process
// must reach the fatal promptly, never deadlock. One mode per run (argv[1]); see expected_fatal.hpp for the protocol:
//   sync-sink-format-refused    a synchronous logger: the fatal fires on the logging thread
//   async-sink-format-refused   an asynchronous logger: the fatal fires on its worker while the caller flushes

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is an engine-fatal control

#include "expected_fatal.hpp"
#include "refusing_allocator.hpp"
#include "specimen_common.hpp"

#include <crd/containers/string.hpp>
#include <crd/log/log.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iterator>
#include <memory>

CRD_DEFINE_LOG_CHANNEL(g_log_fatal_specimen, "LogFatal", crd::log::LogLevel::Trace)

namespace
{
// Formats each record into a String on an allocator that refuses every request, as a sink formats under exhaustion.
class RefusedFormatSink final : public crd::log::ISink
{
public:
    RefusedFormatSink() noexcept { m_refusing.refuse_all(); }

    void write(const crd::log::LogRecord& rec) override
    {
        crd::containers::String line(&m_refusing);
        std::format_to(std::back_inserter(line), "{}", rec.message);
    }
    void flush() override {}

private:
    crd_diag_fatal::RefusingAllocator m_refusing{"log-fatal-refusing"};
};

// The bridge runs before the platform handler, so by the time the fatal reaches it the bridge must have suppressed its
// own record and counted that; otherwise it logged from inside the sink (a deadlock, or a refused relock where the
// mutex detects it).
int suppressed_then_fatal(const char* formatted_message)
{
    if (crd::log::sink_reentrant_assert_count() != 1U)
    {
        std::printf("CRD_DIAG_FATAL_OTHER the assert bridge did not suppress its record inside the sink\n");
        std::fflush(stdout);
        std::_Exit(97);
    }
    return crd_diag_fatal::handler(formatted_message);
}

[[nodiscard]] bool is(const char* mode, const char* name)
{
    return std::strcmp(mode, name) == 0;
}

// Runs one mode. Returns false for an unknown mode; returning at all for a known mode means the fatal did not fire.
[[nodiscard]] bool run_mode(const char* mode)
{
    crd::log::LoggerConfig cfg;
    if (is(mode, "sync-sink-format-refused"))
    {
        cfg.async = false;
    }
    else if (is(mode, "async-sink-format-refused"))
    {
        cfg.async = true;
    }
    else
    {
        return false;
    }
    crd_diag_fatal::arm(mode, "String: out of memory");
    crd::set_assert_platform_handler(&suppressed_then_fatal);
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<RefusedFormatSink>());
    CRD_LOG_INFO(g_log_fatal_specimen, "a record longer than the String's inline buffer");
    crd::log::flush();
    crd::log::shutdown();
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    crd_diag_announce();
    if (argc < 2)
    {
        return 96;
    }
    // Exit 0 when the violation returned: the fatal did not fire, and the CTest (the marker line) rejects it.
    return run_mode(argv[1]) ? 0 : 96;
}
