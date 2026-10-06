// DIAG.5c(d): an assert fired from inside a sink must not route back through the logger -- the delivering thread
// already holds sinks_mutex, so logging again would re-lock it on the owning thread (a hard self-deadlock, the
// acceptance's "assert from logger sink"). The bridge detects the in-delivery state and suppresses with evidence
// instead. Lifecycle: asserts before init / after shutdown bypass the logger (bridge not installed / uninstalled),
// and a log after shutdown returns promptly rather than hanging. The asserting cases are gated on
// CRD_ENABLE_ASSERTS -- CRD_ASSERT compiles out in shipping, so the sink would never assert and the expectations
// could not hold (same lesson as rt_sentinel).

#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/log/logger.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

using namespace crd;
using namespace crd::log;

CRD_DEFINE_LOG_CHANNEL(g_log_reentry_test, "Reentry", crd::log::LogLevel::Trace)

namespace
{
using clock = std::chrono::steady_clock;

auto elapsed_ms(clock::time_point start) -> long long
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();
}

// Returns 0 (ignore) so a deliberately-fired CRD_ASSERT does not terminate the test process; the bridge still ran
// first (report_assert_failure calls the handler before the platform handler). [[maybe_unused]]: only the
// CRD_ENABLE_ASSERTS cases reference it, and it must not trip -Wunused-function under -Werror otherwise.
[[maybe_unused]] int ignore_platform_handler(const char*) noexcept
{
    return 0;
}

// Runs `body` on a helper thread and waits at most `bound`. A shutdown hang leaves the helper blocked forever in
// shutdown(), and nothing can unblock it, so on expiry this writes one stderr line and ends the process with exit code
// 3: the regression fails in seconds rather than at the 3600 s CTest timeout. The body must not use Catch2
// assertions, which are not thread-safe; it returns its evidence and the caller checks it after the join.
template <typename Body> void run_bounded(const char* what, std::chrono::seconds bound, const Body& body)
{
    std::atomic<bool> done{false};
    std::thread runner(
        [&]
        {
            body();
            done.store(true, std::memory_order_release);
        });
    const auto deadline = clock::now() + bound;
    while (!done.load(std::memory_order_acquire))
    {
        if (clock::now() > deadline)
        {
            std::fprintf(stderr, "[test] %s: still blocked after %lld s (logger shutdown hang)\n", what,
                         static_cast<long long>(bound.count()));
            std::fflush(stderr);
            std::_Exit(3);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    runner.join();
}

#if CRD_ENABLE_ASSERTS
// A sink whose write() asserts exactly once, from inside the logger's sink-delivery critical section.
class AssertingSink final : public ISink
{
public:
    void write(const LogRecord&) override
    {
        if (!m_fired.exchange(true, std::memory_order_relaxed))
        {
            CRD_ASSERT(false); // fires while this thread holds sinks_mutex -> the bridge must suppress, not re-lock
        }
        m_delivered.fetch_add(1, std::memory_order_relaxed);
    }
    void flush() override {}
    int  delivered() const { return m_delivered.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> m_fired{false};
    std::atomic<int>  m_delivered{0};
};
#endif
} // namespace

#if CRD_ENABLE_ASSERTS
TEST_CASE("assert inside a sink write does not self-lock the logger (async)", "[log][diag][reentrancy]")
{
    auto* prev_h  = crd::get_assert_handler();
    auto* prev_pf = crd::get_assert_platform_handler();
    crd::set_assert_handler(nullptr);                          // let init install the bridge
    crd::set_assert_platform_handler(&ignore_platform_handler); // do not terminate on the deliberate assert

    LoggerConfig cfg;
    cfg.async = true;
    if (is_initialized())
    {
        shutdown();
    }
    clear_sinks();
    init(cfg);

    auto           owner = std::make_unique<AssertingSink>();
    AssertingSink* sink  = owner.get();
    add_sink(std::move(owner));

    const u64  before = sink_reentrant_assert_count();
    const auto start  = clock::now();

    CRD_LOG_INFO(g_log_reentry_test, "trigger"); // worker delivers -> write() asserts -> bridge suppresses
    CRD_LOG_INFO(g_log_reentry_test, "after");   // must still be delivered: the worker survived, no deadlock
    flush();

    CHECK(sink_reentrant_assert_count() > before);
    CHECK(sink->delivered() >= 2);
    CHECK(elapsed_ms(start) < 5000); // nowhere near a hang

    shutdown(); // completes (no wedged sink)
    crd::set_assert_handler(prev_h);
    crd::set_assert_platform_handler(prev_pf);
}

TEST_CASE("assert inside a sink write does not self-lock the logger (sync)", "[log][diag][reentrancy]")
{
    auto* prev_h  = crd::get_assert_handler();
    auto* prev_pf = crd::get_assert_platform_handler();
    crd::set_assert_handler(nullptr);
    crd::set_assert_platform_handler(&ignore_platform_handler);

    LoggerConfig cfg;
    cfg.async = false; // sync: the calling thread owns sinks_mutex during write() -- this was the UB case
    if (is_initialized())
    {
        shutdown();
    }
    clear_sinks();
    init(cfg);

    auto           owner = std::make_unique<AssertingSink>();
    AssertingSink* sink  = owner.get();
    add_sink(std::move(owner));

    const u64  before = sink_reentrant_assert_count();
    const auto start  = clock::now();

    CRD_LOG_INFO(g_log_reentry_test, "trigger"); // synchronous delivery -> write() asserts -> bridge suppresses

    CHECK(sink_reentrant_assert_count() > before);
    CHECK(sink->delivered() >= 1);
    CHECK(elapsed_ms(start) < 5000);

    shutdown();
    crd::set_assert_handler(prev_h);
    crd::set_assert_platform_handler(prev_pf);
}
#endif // CRD_ENABLE_ASSERTS

TEST_CASE("assert before init bypasses the logger (no bridge installed)", "[log][diag][reentrancy]")
{
    if (is_initialized())
    {
        shutdown();
    }
    crd::set_assert_handler(nullptr);
    REQUIRE(crd::get_assert_handler() == nullptr); // no bridge without init()

#if CRD_ENABLE_ASSERTS
    auto* prev_pf = crd::get_assert_platform_handler();
    crd::set_assert_platform_handler(&ignore_platform_handler);
    CRD_ASSERT(false); // no handler -> platform handler returns 0 -> returns, no logger involvement, no hang
    crd::set_assert_platform_handler(prev_pf);
#endif
    SUCCEED("assert before init returned without touching the logger");
}

TEST_CASE("after shutdown the bridge is uninstalled and a late log does not hang", "[log][diag][reentrancy]")
{
    crd::set_assert_handler(nullptr);

    LoggerConfig cfg;
    cfg.async = true;
    if (is_initialized())
    {
        shutdown();
    }
    clear_sinks();
    init(cfg);
    REQUIRE(crd::get_assert_handler() != nullptr); // init installed the bridge

    shutdown();
    CHECK(crd::get_assert_handler() == nullptr); // shutdown pulled it back out

    // A log after shutdown takes the sync path (initialized == false) to zero sinks; it and a following flush must
    // return promptly, never block on a drain with no worker.
    const auto start = clock::now();
    CRD_LOG_INFO(g_log_reentry_test, "after shutdown");
    flush();
    CHECK(elapsed_ms(start) < 2000);
}

// shutdown() used to clear `running` and notify outside queue_mutex. A worker that had just evaluated its wait
// predicate (queue empty, still running) then slept through the only notify, and join() waited forever. The window is
// widest right after init(), when the worker is just reaching its first wait, so cycle init/shutdown many times.
TEST_CASE("async init followed at once by shutdown never loses the stop wakeup", "[log][diag][reentrancy]")
{
    crd::set_assert_handler(nullptr);
    if (is_initialized())
    {
        shutdown();
    }
    clear_sinks();

    constexpr int k_cycles = 500;
    int completed = 0;
    bool stayed_up = true;
    run_bounded("init/shutdown cycles", std::chrono::seconds(60),
                [&]
                {
                    LoggerConfig cfg;
                    cfg.async = true;
                    for (int i = 0; i < k_cycles; ++i)
                    {
                        init(cfg);
                        if (!is_initialized())
                        {
                            stayed_up = false;
                        }
                        shutdown();
                        ++completed;
                    }
                });

    CHECK(stayed_up);
    CHECK(completed == k_cycles);
    CHECK_FALSE(is_initialized());
    CHECK(crd::get_assert_handler() == nullptr);
}

// A log racing shutdown must be dropped, not queued after the worker has exited: a stranded record keeps the queue
// non-empty, and shutdown()'s own flush() then waits for a drain that cannot happen. Each cycle starts a producer
// after init() and joins it before the next init(), so the only concurrency is the shutdown race under test.
TEST_CASE("a log racing async shutdown is dropped and never stranded in the queue", "[log][diag][reentrancy]")
{
    crd::set_assert_handler(nullptr);
    if (is_initialized())
    {
        shutdown();
    }
    clear_sinks();

    constexpr int k_cycles = 200;
    int completed = 0;
    run_bounded("logging across shutdown", std::chrono::seconds(60),
                [&]
                {
                    LoggerConfig cfg;
                    cfg.async = true;
                    for (int i = 0; i < k_cycles; ++i)
                    {
                        init(cfg);
                        std::atomic<bool> stop{false};
                        std::thread producer(
                            [&]
                            {
                                while (!stop.load(std::memory_order_acquire))
                                {
                                    CRD_LOG_INFO(g_log_reentry_test, "racing shutdown");
                                }
                            });
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                        shutdown();
                        stop.store(true, std::memory_order_release);
                        producer.join();
                        ++completed;
                    }
                });

    CHECK(completed == k_cycles);
    CHECK_FALSE(is_initialized());
}
