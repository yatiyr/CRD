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
    constexpr int threads = 4;
    constexpr int messages_per_thread = 100;
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
    {
        ts.emplace_back(
            [t]
            {
                for (int i = 0; i < messages_per_thread; ++i)
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
    REQUIRE(records.size() == threads * messages_per_thread);
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

// =============================================================================
// Bridge: crd-core assert handler -> crd-log Critical
// =============================================================================

namespace
{
// Handler used by the next two tests. We capture all five fields to verify
// the bridge forwards them correctly.
struct CapturedAssert
{
    std::atomic<int> fire_count{0};
    std::atomic<int> capture_alloc_failures{0}; // set when the handler could not store the strings
    std::string expression;
    std::string file;
    int line = 0;
    std::string message;
};

CapturedAssert g_capture;

// In Release builds (CRD_RELEASE defined → CRD_ENABLE_ASSERTS == 0), the test
// case below short-circuits before this handler is referenced, so the symbol
// would otherwise trip GCC's -Wunused-function under -Werror.
[[maybe_unused]] int noop_assert_platform_handler(const char* formatted_message) noexcept
{
    (void)formatted_message;
    return 0;
}

void test_assert_handler(const char* expr, const char* file, int line, const char* msg) noexcept
{
    // The string assignments allocate, and an assert handler is `noexcept` by contract — a throw here is
    // std::terminate during someone else's failure path. Record what we can, swallow the rest.
    // (bugprone-exception-escape.)
    try
    {
        g_capture.expression = expr ? expr : "";
        g_capture.file = file ? file : "";
        g_capture.message = msg ? msg : "";
    }
    catch (...)
    {
        g_capture.capture_alloc_failures.fetch_add(1, std::memory_order_relaxed);
    }
    g_capture.line = line;
    g_capture.fire_count.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

TEST_CASE("set_assert_handler installs a custom handler", "[log][bridge][assert]")
{
    // Save whatever crd-log might have installed and restore at the end.
    auto* prev = crd::get_assert_handler();
    crd::set_assert_handler(&test_assert_handler);
    REQUIRE(crd::get_assert_handler() == &test_assert_handler);

    g_capture.fire_count.store(0);
    g_capture.expression.clear();
    g_capture.file.clear();
    g_capture.line = 0;
    g_capture.message.clear();

    // We can't safely call CRD_ASSERT(false) here (Windows MessageBox), but the
    // handler is fired inside detail::fire_assert_handler -- which we expose
    // indirectly by manually invoking the handler ourselves. The point of this
    // test is the install/get round-trip.
    crd::get_assert_handler()("test_expr", "tests/log/test_log.cpp", 42, "manual fire");
    REQUIRE(g_capture.fire_count.load() == 1);
    REQUIRE(g_capture.expression == "test_expr");
    REQUIRE(g_capture.line == 42);
    REQUIRE(g_capture.message == "manual fire");

    crd::set_assert_handler(prev);
}

TEST_CASE("crd-log init installs default assert handler that emits Critical", "[log][bridge][assert]")
{
    // Make sure no handler is installed before init() so the bridge actually runs.
    crd::set_assert_handler(nullptr);

    LoggerScope scope{};
    auto ring_owner = std::make_unique<RingBufferSink>(8);
    RingBufferSink* ring = ring_owner.get();
    add_sink(std::move(ring_owner));

    // After init() the bridge must have installed a handler.
    auto* handler = crd::get_assert_handler();
    REQUIRE(handler != nullptr);

    // Fire it directly, bypassing the platform UI in report_assert_failure().
    handler("simulated_expr", "tests/log/test_log.cpp", 1234, "bridge smoke");
    flush();

    auto records = ring->snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].level == LogLevel::Critical);
    REQUIRE(records[0].message.find("simulated_expr") != std::string::npos);
    REQUIRE(records[0].message.find("bridge smoke") != std::string::npos);
    REQUIRE(records[0].message.find("1234") != std::string::npos);
    // shutdown() (by LoggerScope dtor) will uninstall the handler.
}

TEST_CASE("CRD_ASSERT(false) reaches log bridge without platform UI", "[log][bridge][assert]")
{
#if defined(CRD_RELEASE)
    static_assert(CRD_ENABLE_ASSERTS == 0);
    SUCCEED("Assertions are compiled out in release builds.");
#else
    crd::set_assert_platform_handler(&noop_assert_platform_handler);
    crd::set_assert_handler(nullptr);

    LoggerScope scope{};
    auto ring_owner = std::make_unique<RingBufferSink>(8);
    RingBufferSink* ring = ring_owner.get();
    add_sink(std::move(ring_owner));

    CRD_ASSERT_MSG(false, "bridge end-to-end");
    flush();

    auto records = ring->snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].level == LogLevel::Critical);
    REQUIRE(records[0].message.find("ASSERT: false") != std::string::npos);
    REQUIRE(records[0].message.find("bridge end-to-end") != std::string::npos);

    crd::set_assert_platform_handler(nullptr);
#endif
}

TEST_CASE("crd-log shutdown uninstalls the bridge", "[log][bridge][assert]")
{
    crd::set_assert_handler(nullptr);
    {
        LoggerScope scope{};
        REQUIRE(crd::get_assert_handler() != nullptr);
    }
    REQUIRE(crd::get_assert_handler() == nullptr);
}
