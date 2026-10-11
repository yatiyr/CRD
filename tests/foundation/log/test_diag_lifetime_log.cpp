// DIAG.10c -- the logger's file handle and installed sinks in the lifetime ledger, judged against the OS.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): a FileSink holds one FileHandle from the open in
// its constructor to the close in its destructor, and a rotation closes one file and opens the next without leaving
// either behind; each sink installed with add_sink() is one CallbackRegistration until clear_sinks() or shutdown()
// removes it. Whether the file is really held is asked of the OS (crd::os_file_open_in_process), which shares no
// code with the sink.

#include <crd/core/lifetime_ledger.hpp>
#include <crd/core/os_resource_probe.hpp>
#include <crd/log/log.hpp>
#include <crd/log/logger.hpp>
#include <crd/log/sinks/file_sink.hpp>
#include <crd/log/sinks/ring_buffer_sink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <source_location>
#include <system_error>

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
    crd::log::clear_sinks(); // the window starts with no sink installed
    crd::log::init();

    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    crd::log::add_sink(std::make_unique<crd::log::RingBufferSink>(4));
    crd::log::add_sink(std::make_unique<crd::log::RingBufferSink>(4));
    crd::log::add_sink(nullptr); // refused: nothing is installed
    crd::LifetimeReport r = since(start);
    CHECK(r.live_change(LifetimeClass::CallbackRegistration) == 2);

    crd::log::clear_sinks();
    CHECK(since(start).verdict == LifetimeVerdict::Balanced);

    crd::log::add_sink(std::make_unique<crd::log::RingBufferSink>(4));
    CHECK(since(start).live_change(LifetimeClass::CallbackRegistration) == 1);
    crd::log::shutdown(); // shutdown removes the sinks too
    r = since(start);
    CHECK(r.verdict == LifetimeVerdict::Balanced);
    CHECK(r.underflowed == 0U);
}
