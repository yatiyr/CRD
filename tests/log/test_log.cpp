#include <crd/log/log.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace crd;
using namespace crd::log;

// Test channel local to this TU. (Define implicitly declares the variable.)
CRD_DEFINE_LOG_CHANNEL(g_log_test, "Test", crd::log::LogLevel::Trace)

// Helper: get a fresh logger for each test (shutdown wipes sinks).
struct LoggerScope
{
    explicit LoggerScope(LoggerConfig cfg = {})
    {
        if (is_initialized())
        {
            shutdown();
        }
        clear_sinks();
        init(cfg);
    }
    ~LoggerScope() { shutdown(); }
};

TEST_CASE("LogLevel string round-trip", "[log][level]")
{
    REQUIRE(std::string("Trace") == to_string(LogLevel::Trace));
    REQUIRE(std::string("Critical") == to_string(LogLevel::Critical));
    REQUIRE(std::string("INF") == to_short_string(LogLevel::Info));
    REQUIRE(from_string("warn") == LogLevel::Warn);
    REQUIRE(from_string("WARNING") == LogLevel::Warn);
    REQUIRE(from_string("fatal") == LogLevel::Critical);
    REQUIRE(from_string("nope") == LogLevel::Off);
}

TEST_CASE("Channel is registered and discoverable", "[log][channel]")
{
    Channel* found = find_channel("Test");
    REQUIRE(found != nullptr);
    REQUIRE(found == &g_log_test);
}

TEST_CASE("Sync logging delivers to sinks", "[log][sync]")
{
    LoggerScope scope{};
    auto ring_owner = std::make_unique<RingBufferSink>(16);
    RingBufferSink* ring = ring_owner.get();
    add_sink(std::move(ring_owner));

    g_log_test.runtime_level = LogLevel::Trace;
    CRD_LOG_INFO(g_log_test, "hello {}", 42);
    CRD_LOG_WARN(g_log_test, "watch out: {}", "warn");
    flush();

    auto records = ring->snapshot();
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].level == LogLevel::Info);
    REQUIRE(records[0].channel_name == "Test");
    REQUIRE(records[0].message == "hello 42");
    REQUIRE(records[1].level == LogLevel::Warn);
    REQUIRE(records[1].message == "watch out: warn");
}

TEST_CASE("Channel runtime level filters at producer", "[log][filter]")
{
    LoggerScope scope{};
    auto ring_owner = std::make_unique<RingBufferSink>(16);
    RingBufferSink* ring = ring_owner.get();
    add_sink(std::move(ring_owner));

    g_log_test.runtime_level = LogLevel::Warn;
    CRD_LOG_TRACE(g_log_test, "trace"); // dropped
    CRD_LOG_DEBUG(g_log_test, "debug"); // dropped
    CRD_LOG_INFO(g_log_test, "info");   // dropped
    CRD_LOG_WARN(g_log_test, "warn");   // kept
    CRD_LOG_ERROR(g_log_test, "error"); // kept
    flush();

    auto records = ring->snapshot();
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].message == "warn");
    REQUIRE(records[1].message == "error");

    g_log_test.runtime_level = LogLevel::Trace;
}

TEST_CASE("Per-sink min_level filters at consumer", "[log][filter]")
{
    LoggerScope scope{};
    auto ring_owner = std::make_unique<RingBufferSink>(16);
    RingBufferSink* ring = ring_owner.get();
    ring->set_min_level(LogLevel::Error);
    add_sink(std::move(ring_owner));

    g_log_test.runtime_level = LogLevel::Trace;
    CRD_LOG_INFO(g_log_test, "info");
    CRD_LOG_WARN(g_log_test, "warn");
    CRD_LOG_ERROR(g_log_test, "error");
    flush();

    auto records = ring->snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].level == LogLevel::Error);
}

TEST_CASE("Async logging delivers all records", "[log][async]")
{
    LoggerConfig cfg;
    cfg.async = true;
    cfg.async_queue_capacity = 1024;
    cfg.drop_on_overflow = false;
    LoggerScope scope{cfg};

    auto ring_owner = std::make_unique<RingBufferSink>(2048);
    RingBufferSink* ring = ring_owner.get();
    add_sink(std::move(ring_owner));

    g_log_test.runtime_level = LogLevel::Trace;
    constexpr int kThreads = 4;
    constexpr int kMessagesPerThread = 100;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
    {
        ts.emplace_back(
            [t]
            {
                for (int i = 0; i < kMessagesPerThread; ++i)
                {
                    CRD_LOG_INFO(g_log_test, "t={} i={}", t, i);
                }
            });
    }
    for (auto& th : ts)
    {
        th.join();
    }

    flush();
    auto records = ring->snapshot();
    REQUIRE(records.size() == kThreads * kMessagesPerThread);
}

TEST_CASE("RingBufferSink overwrites oldest", "[log][ring]")
{
    LoggerScope scope{};
    auto ring_owner = std::make_unique<RingBufferSink>(4);
    RingBufferSink* ring = ring_owner.get();
    add_sink(std::move(ring_owner));

    g_log_test.runtime_level = LogLevel::Trace;
    for (int i = 0; i < 10; ++i)
    {
        CRD_LOG_INFO(g_log_test, "n={}", i);
    }
    flush();
    auto records = ring->snapshot();
    REQUIRE(records.size() == 4);
    // Most recent four are 6..9.
    REQUIRE(records[0].message == "n=6");
    REQUIRE(records[3].message == "n=9");
}

TEST_CASE("FileSink writes to disk and rotates", "[log][file]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "crd-log-tests";
    fs::create_directories(dir);
    const fs::path log_path = dir / "rotate.log";
    // Clean old artifacts.
    for (int i = 0; i <= 5; ++i)
    {
        fs::remove(dir / (i == 0 ? "rotate.log" : ("rotate." + std::to_string(i) + ".log")));
    }

    LoggerScope scope{};
    add_sink(std::make_unique<FileSink>(log_path.string(), /*max_bytes*/ 256, /*max_files*/ 3));

    g_log_test.runtime_level = LogLevel::Trace;
    for (int i = 0; i < 50; ++i)
    {
        CRD_LOG_INFO(g_log_test, "rotation-test-message-{:03}", i);
    }
    flush();

    REQUIRE(fs::exists(log_path));
    REQUIRE(fs::exists(dir / "rotate.1.log"));
    // We wrote enough that at least one rotation must have happened.
    REQUIRE(fs::file_size(log_path) <= 4096);
}

TEST_CASE("Critical bypasses async queue", "[log][critical]")
{
    LoggerConfig cfg;
    cfg.async = true;
    cfg.async_queue_capacity = 1024;
    cfg.flush_on_critical = true;
    LoggerScope scope{cfg};

    auto ring_owner = std::make_unique<RingBufferSink>(8);
    RingBufferSink* ring = ring_owner.get();
    add_sink(std::move(ring_owner));

    g_log_test.runtime_level = LogLevel::Trace;
    CRD_LOG_CRITICAL(g_log_test, "boom");
    // No flush() yet -- the critical should already be there.
    auto records = ring->snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].level == LogLevel::Critical);
}

TEST_CASE("NullSink accepts records without crashing", "[log][null]")
{
    LoggerScope scope{};
    add_sink(std::make_unique<NullSink>());
    g_log_test.runtime_level = LogLevel::Trace;
    for (int i = 0; i < 10; ++i)
    {
        CRD_LOG_INFO(g_log_test, "to /dev/null #{}", i);
    }
    flush();
    SUCCEED();
}

TEST_CASE("set_all_channels_level affects every channel", "[log][channel]")
{
    set_all_channels_level(LogLevel::Error);
    REQUIRE(g_log_test.runtime_level == LogLevel::Error);
    set_all_channels_level(LogLevel::Trace);
    REQUIRE(g_log_test.runtime_level == LogLevel::Trace);
}
