// DIAG.10c -- the logger's file handle and installed sinks in the lifetime ledger, judged against the OS.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): a FileSink holds one FileHandle from the open in
// its constructor to the close in its destructor, and a rotation closes one file and opens the next without leaving
// either behind; each sink installed with add_sink() is one CallbackRegistration until clear_sinks() or shutdown()
// removes it. Whether the file is really held is asked of the OS (crd::os_file_open_in_process), which shares no
// code with the sink. The asynchronous logger's worker is one Thread from init to shutdown, and flush() returns only
// after every record logged before it, including one the worker has taken off the queue, has been written.

#include <crd/core/lifetime_ledger.hpp>
#include <crd/core/os_resource_probe.hpp>
#include <crd/log/log.hpp>
#include <crd/log/logger.hpp>
#include <crd/log/sinks/file_sink.hpp>
#include <crd/log/sinks/ring_buffer_sink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <source_location>
#include <system_error>
#include <thread>

CRD_DEFINE_LOG_CHANNEL(g_log_lifetime_test, "Lifetime", crd::log::LogLevel::Trace)

namespace
{
namespace fs = std::filesystem;
using crd::LifetimeClass;
using crd::LifetimeVerdict;
using crd::OsPresence;

crd::LifetimeReport since(const crd::LifetimeSnapshot& before)
{
    return crd::lifetime_report(before, crd::lifetime_ledger().snapshot());
}

// A fresh directory under the temp directory; removed with its contents by the destructor.
struct TempDir
{
    fs::path path;

    explicit TempDir(const char* stem)
    {
        path = fs::temp_directory_path() / stem;
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

crd::log::LogRecord record(std::string_view message)
{
    return crd::log::LogRecord{crd::log::LogLevel::Info,
                               &g_log_lifetime_test,
                               std::source_location::current(),
                               std::chrono::system_clock::now(),
                               0U,
                               message};
}

OsPresence held(const fs::path& p)
{
    const crd::OsProbeResult r = crd::os_file_open_in_process(p.string().c_str());
    if (r.presence == OsPresence::Unavailable)
    {
        FAIL("the OS probe is unavailable: " << r.reason);
    }
    return r.presence;
}
} // namespace

TEST_CASE("diag10c: a file sink holds one file handle from open to close, and the OS agrees",
          "[log][lifetime][diag10c]")
{
    const TempDir  dir("crd_diag10c_file_sink");
    const fs::path log_path = dir.path / "sink.log";

    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    {
        crd::log::FileSink sink(log_path.string(), /*max_bytes=*/0U, /*max_files=*/1U);
        REQUIRE(sink.is_open());
        const crd::LifetimeReport r = since(start);
        CHECK(r.verdict == LifetimeVerdict::Outstanding);
        CHECK(r.outstanding == crd::lifetime_class_bit(LifetimeClass::FileHandle));
        CHECK(r.live_change(LifetimeClass::FileHandle) == 1);
        CHECK(held(log_path) == OsPresence::Present);
        sink.write(record("held"));
        sink.flush();
    }
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);
    CHECK(held(log_path) == OsPresence::Absent);
}

TEST_CASE("diag10c: a rotation closes one log file and opens the next without leaving either behind",
          "[log][lifetime][diag10c]")
{
    const TempDir  dir("crd_diag10c_rotation");
    const fs::path log_path = dir.path / "rotate.log";
    const fs::path rotated  = dir.path / "rotate.1.log";

    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    {
        crd::log::FileSink sink(log_path.string(), /*max_bytes=*/128U, /*max_files=*/2U);
        REQUIRE(sink.is_open());
        for (int i = 0; i < 8; ++i)
        {
            sink.write(record("a line long enough that a few of them pass the sink's 128-byte rotation limit"));
        }
        sink.flush();
        REQUIRE(fs::exists(rotated));

        const crd::LifetimeSnapshot now = crd::lifetime_ledger().snapshot();
        const crd::LifetimeCounts&  b   = start.of(LifetimeClass::FileHandle);
        const crd::LifetimeCounts&  a   = now.of(LifetimeClass::FileHandle);
        CHECK(a.acquired - b.acquired >= 2U); // the first file and at least one reopen
        CHECK(a.acquired - b.acquired == a.released - b.released + 1U);
        CHECK(since(start).live_change(LifetimeClass::FileHandle) == 1);
        CHECK(held(log_path) == OsPresence::Present);
        CHECK(held(rotated) == OsPresence::Absent);
    }
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);
    CHECK(held(log_path) == OsPresence::Absent);
}

TEST_CASE("diag10c: each installed sink is one callback registration until the sinks are cleared",
          "[log][lifetime][diag10c]")
{
    if (crd::log::is_initialized())
    {
        crd::log::shutdown();
    }
    crd::log::clear_sinks(); // the window starts with no sink installed and no logger thread

    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    crd::log::init();
    crd::log::add_sink(std::make_unique<crd::log::RingBufferSink>(4));
    crd::log::add_sink(std::make_unique<crd::log::RingBufferSink>(4));
    crd::log::add_sink(nullptr); // refused: nothing is installed
    crd::LifetimeReport r = since(start);
    CHECK(r.live_change(LifetimeClass::CallbackRegistration) == 2);

    crd::log::clear_sinks();
    CHECK(since(start).live_change(LifetimeClass::CallbackRegistration) == 0);

    crd::log::add_sink(std::make_unique<crd::log::RingBufferSink>(4));
    CHECK(since(start).live_change(LifetimeClass::CallbackRegistration) == 1);
    crd::log::shutdown(); // shutdown removes the sinks too
    r = since(start);
    CHECK(r.verdict == LifetimeVerdict::Balanced);
    CHECK(r.underflowed == 0U);
}

namespace
{
// Records which OS thread delivered the records; the logger owns the sink, the test owns this.
struct DeliverySeen
{
    crd::OsThreadIdentity thread;
    int                   records = 0;
};

class IdentitySink final : public crd::log::ISink
{
public:
    explicit IdentitySink(DeliverySeen* seen) : m_seen(seen) {}
    void write(const crd::log::LogRecord& /*rec*/) override
    {
        m_seen->thread = crd::os_current_thread_identity();
        ++m_seen->records;
    }
    void flush() override {}

private:
    DeliverySeen* m_seen;
};
} // namespace

TEST_CASE("diag10c: the asynchronous logger's worker is one thread from init to shutdown, and the OS sees it end",
          "[log][lifetime][diag10c]")
{
    if (crd::log::is_initialized())
    {
        crd::log::shutdown();
    }
    crd::log::clear_sinks();

    static DeliverySeen seen; // outlives the case: a failed REQUIRE below leaves the logger holding the sink
    seen                              = DeliverySeen{};
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    crd::log::LoggerConfig      cfg;
    cfg.async = true;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<IdentitySink>(&seen));
    crd::log::detail::dispatch(crd::log::LogLevel::Info, g_log_lifetime_test, std::source_location::current(),
                               "delivered");
    crd::log::flush(); // returns once the worker has delivered the record

    crd::LifetimeReport r = since(start);
    CHECK(r.live_change(LifetimeClass::Thread) == 1);
    REQUIRE(seen.records == 1);
    REQUIRE(seen.thread.known);
    CHECK(seen.thread.id != crd::os_current_thread_identity().id); // delivered on the worker, not here
    CHECK(crd::os_thread_alive(seen.thread).presence == OsPresence::Present);

    crd::log::shutdown(); // joins the worker
    r = since(start);
    CHECK(r.verdict == LifetimeVerdict::Balanced);
    CHECK(crd::os_thread_alive(seen.thread).presence == OsPresence::Absent);
}

namespace
{
// A sink that counts what it was given to write.
class CountingSink final : public crd::log::ISink
{
public:
    explicit CountingSink(std::atomic<int>* written) : m_written(written) {}
    void write(const crd::log::LogRecord& /*rec*/) override
    {
        m_written->fetch_add(1, std::memory_order_acq_rel);
    }
    void flush() override {}

private:
    std::atomic<int>* m_written;
};
} // namespace

// Found by the case above under linux-clang-tsan (2026-10-11): the worker pops a record before it takes the sink lock,
// and flush() waited only for an empty queue, so it could take the sink lock first and return before the record was
// written. Each round logs one record and flushes after a pause that sweeps the worker's pop-to-lock window; every
// flush must return with that record written.
TEST_CASE("diag10c: flush returns only after the record the worker is delivering is written", "[log][diag10c]")
{
    if (crd::log::is_initialized())
    {
        crd::log::shutdown();
    }
    crd::log::clear_sinks();
    static std::atomic<int> written{0}; // outlives the case: the logger holds the sink until shutdown
    written.store(0, std::memory_order_relaxed);
    crd::log::LoggerConfig cfg;
    cfg.async = true;
    crd::log::init(cfg);
    crd::log::add_sink(std::make_unique<CountingSink>(&written));

    constexpr int rounds = 4000;
    int           early  = 0;
    for (int round = 0; round < rounds; ++round)
    {
        crd::log::detail::dispatch(crd::log::LogLevel::Info, g_log_lifetime_test, std::source_location::current(),
                                   "counted");
        const auto pause = std::chrono::steady_clock::now() + std::chrono::microseconds(round % 64);
        while (std::chrono::steady_clock::now() < pause)
        {
            std::this_thread::yield();
        }
        crd::log::flush();
        if (written.load(std::memory_order_acquire) != round + 1)
        {
            ++early;
            crd::log::flush(); // catch up, so the next round starts even
        }
    }
    crd::log::shutdown();
    CHECK(early == 0);
    CHECK(written.load(std::memory_order_acquire) == rounds);
}
