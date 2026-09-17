// DIAG.5c(c): the assert / headless path must never hang forever draining a stuck sink. flush_for(timeout) bounds
// both the async drain and the sink-lock acquisition, and on a give-up it leaves evidence (flush_timeout_count()
// + a stderr line) rather than blocking. These tests prove the bound holds in wall-clock time (steady_clock,
// asserted well under 5x the requested timeout so a slow CI box never flakes) and that a reentrant flush_for from
// inside a sink on the log worker detects itself instead of deadlocking / try_lock'ing a mutex it already owns.

#include <crd/log/log.hpp>
#include <crd/log/logger.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

using namespace crd;
using namespace crd::log;

CRD_DEFINE_LOG_CHANNEL(g_log_flush_test, "FlushTest", crd::log::LogLevel::Trace)

namespace
{
using clock = std::chrono::steady_clock;

auto elapsed_ms(clock::time_point start) -> long long
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();
}

// A sink whose write() parks until the test releases it, holding the logger's sink mutex the whole time (the
// worker calls write() under that mutex). This is the canonical "stuck flush" shape: the worker is wedged inside
// a sink, so a foreground flush must give up rather than block on the sink lock.
class BlockingSink final : public ISink
{
public:
    void write(const LogRecord&) override
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_entered = true;
        }
        m_entered_cv.notify_all();
        std::unique_lock<std::mutex> lk(m_mtx);
        m_release_cv.wait(lk, [&] { return m_released; });
    }

    void flush() override {} // never blocks: flush_for only reaches this on the success path

    [[nodiscard]] bool wait_until_entered(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(m_mtx);
        return m_entered_cv.wait_for(lk, timeout, [&] { return m_entered; });
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_released = true;
        }
        m_release_cv.notify_all();
    }

private:
    std::mutex              m_mtx;
    std::condition_variable m_entered_cv;
    std::condition_variable m_release_cv;
    bool                    m_entered  = false;
    bool                    m_released = false;
};

// A sink whose write() itself calls flush_for(), simulating an assert firing inside a sink on the worker thread.
// flush_for must detect it is on the worker and bail without deadlocking or invoking undefined behaviour.
class ReentrantFlushSink final : public ISink
{
public:
    void write(const LogRecord&) override
    {
        const auto start = clock::now();
        m_result.store(flush_for(200), std::memory_order_relaxed);
        m_elapsed.store(elapsed_ms(start), std::memory_order_relaxed);
        m_called.store(true, std::memory_order_release);
    }

    void flush() override {}

    bool      called() const { return m_called.load(std::memory_order_acquire); }
    bool      result() const { return m_result.load(std::memory_order_relaxed); }
    long long elapsed() const { return m_elapsed.load(std::memory_order_relaxed); }

private:
    std::atomic<bool>      m_called{false};
    std::atomic<bool>      m_result{true};
    std::atomic<long long> m_elapsed{0};
};
} // namespace

TEST_CASE("flush_for: a healthy async logger drains and returns true", "[log][diag][flush]")
{
    LoggerConfig cfg;
    cfg.async = true;
    if (is_initialized())
    {
        shutdown();
    }
    clear_sinks();
    init(cfg);

    for (int i = 0; i < 8; ++i)
    {
        CRD_LOG_INFO(g_log_flush_test, "healthy {}", i);
    }

    // flush_timeouts is a process-global that shutdown() never resets (like dropped), so assert on a delta rather
    // than an absolute -- otherwise this flakes under Catch2 --order rand or a file reorder.
    const u64  before = flush_timeout_count();
    const bool ok     = flush_for(2000);
    CHECK(ok);
    CHECK(flush_timeout_count() == before);

    shutdown();
}

TEST_CASE("flush_for: bounded give-up when the worker is stuck inside a sink", "[log][diag][flush]")
{
    LoggerConfig cfg;
    cfg.async = true;
    if (is_initialized())
    {
        shutdown();
    }
    clear_sinks();
    init(cfg);

    auto        owner   = std::make_unique<BlockingSink>();
    BlockingSink* sink  = owner.get();
    add_sink(std::move(owner));

    // Record #1 wedges the worker inside write() (holding the sink lock); #2 then stays stuck in the queue, so
    // both give-up reasons are live: the drain never completes and the sink lock is unavailable.
    CRD_LOG_INFO(g_log_flush_test, "wedge");
    REQUIRE(sink->wait_until_entered(std::chrono::seconds(2))); // bounded: fast fail, never a suite hang
    CRD_LOG_INFO(g_log_flush_test, "stuck-behind");

    const int  timeout = 150;
    const auto start   = clock::now();
    const bool ok      = flush_for(timeout);
    const auto took    = elapsed_ms(start);

    CHECK_FALSE(ok);
    CHECK(flush_timeout_count() >= 1);
    CHECK(took < 5 * timeout); // bounded: nowhere near a hang

    // Release the wedged sink BEFORE shutdown -- shutdown()'s flush() is unbounded and would hang otherwise.
    sink->release();
    shutdown();
}

TEST_CASE("flush_for: a reentrant call from the log worker is detected, not deadlocked", "[log][diag][flush]")
{
    LoggerConfig cfg;
    cfg.async = true;
    if (is_initialized())
    {
        shutdown();
    }
    clear_sinks();
    init(cfg);

    auto                 owner = std::make_unique<ReentrantFlushSink>();
    ReentrantFlushSink*  sink  = owner.get();
    add_sink(std::move(owner));

    CRD_LOG_INFO(g_log_flush_test, "reentrant");

    // Wait (bounded) for the worker to run write() -> flush_for. If the self-detection were missing this would
    // deadlock or invoke UB; the poll gives it well over the 200 ms internal timeout before we assert.
    const auto deadline = clock::now() + std::chrono::seconds(2);
    while (!sink->called() && clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(sink->called());
    CHECK_FALSE(sink->result());          // worker-thread reentrancy is reported as a give-up
    CHECK(sink->elapsed() < 200);         // returned immediately, did NOT wait out the 200 ms timeout
    CHECK(flush_timeout_count() >= 1);

    shutdown();
}
