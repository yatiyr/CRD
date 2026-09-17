// DIAG.5c(e): logger failure policy. A sink that violates the ISink "write must not throw" contract must be a
// counted, contained failure -- not std::terminate on the noexcept async worker, and not a lost record for the
// OTHER sinks. Plus the declared queue-pressure contract: newest-dropped under overflow (drop_on_overflow, the
// headless default), and Critical retained because flush_on_critical delivers it synchronously, bypassing the
// queue. Throwing does not depend on CRD_ENABLE_ASSERTS (/EHsc is global), so these are not assert-gated.

#include <crd/log/log.hpp>
#include <crd/log/logger.hpp>
#include <crd/log/sinks/ring_buffer_sink.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>

using namespace crd;
using namespace crd::log;

CRD_DEFINE_LOG_CHANNEL(g_log_sinkfail_test, "SinkFail", crd::log::LogLevel::Trace)

namespace
{
using clock = std::chrono::steady_clock;

auto elapsed_ms(clock::time_point start) -> long long
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();
}

// Violates the contract: write() throws once. A second write() (if any) succeeds, so a sink placed after it still
// proves delivery continued.
class ThrowingSink final : public ISink
{
public:
    void write(const LogRecord&) override
    {
        if (!m_thrown.exchange(true, std::memory_order_relaxed))
        {
            throw std::runtime_error("sink boom");
        }
    }
    void flush() override {}

private:
    std::atomic<bool> m_thrown{false};
};

// write() parks until released (for the queue-pressure test: wedge the worker so the queue fills).
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
    void flush() override {}

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

void fresh_logger(LoggerConfig cfg)
{
    if (is_initialized())
    {
        shutdown();
    }
    clear_sinks();
    init(cfg);
}
} // namespace

TEST_CASE("a throwing sink is a counted failure, not std::terminate; other sinks still get the record (async)",
          "[log][diag][sinkfail]")
{
    LoggerConfig cfg;
    cfg.async = true;
    fresh_logger(cfg);

    add_sink(std::make_unique<ThrowingSink>()); // first: it throws
    auto            ring_owner = std::make_unique<RingBufferSink>(8);
    RingBufferSink* ring       = ring_owner.get();
    add_sink(std::move(ring_owner)); // second: must still receive the record

    const u64 before = sink_failure_count();
    CRD_LOG_INFO(g_log_sinkfail_test, "survive");
    flush();

    CHECK(sink_failure_count() > before); // the throw was contained, not fatal
    CHECK(ring->snapshot().size() >= 1);  // delivery continued to the healthy sink

    shutdown();
}

TEST_CASE("a throwing sink does not lose the record for other sinks (sync)", "[log][diag][sinkfail]")
{
    LoggerConfig cfg;
    cfg.async = false; // sync: without the per-sink catch, dispatch()'s outer catch would drop it for EVERY sink
    fresh_logger(cfg);

    add_sink(std::make_unique<ThrowingSink>());
    auto            ring_owner = std::make_unique<RingBufferSink>(8);
    RingBufferSink* ring       = ring_owner.get();
    add_sink(std::move(ring_owner));

    const u64 before = sink_failure_count();
    CRD_LOG_INFO(g_log_sinkfail_test, "survive-sync");

    CHECK(sink_failure_count() > before);
    CHECK(ring->snapshot().size() >= 1);

    shutdown();
}

TEST_CASE("queue pressure drops the newest records and never blocks the producer (drop_on_overflow)",
          "[log][diag][sinkfail]")
{
    LoggerConfig cfg;
    cfg.async                = true;
    cfg.async_queue_capacity = 4;
    cfg.drop_on_overflow     = true;
    fresh_logger(cfg);

    auto         owner = std::make_unique<BlockingSink>();
    BlockingSink* sink = owner.get();
    add_sink(std::move(owner));

    CRD_LOG_INFO(g_log_sinkfail_test, "wedge");
    REQUIRE(sink->wait_until_entered(std::chrono::seconds(2))); // worker is now stuck in write(), queue will fill

    const u64  before = dropped_count();
    const auto start  = clock::now();
    for (int i = 0; i < 32; ++i)
    {
        CRD_LOG_INFO(g_log_sinkfail_test, "flood {}", i); // capacity 4 -> most are dropped, producer never blocks
    }
    const auto took = elapsed_ms(start);

    CHECK(dropped_count() - before >= 4); // newest-dropped under overflow
    CHECK(took < 2000);                   // the producer returned promptly, never blocked on the wedged sink

    sink->release(); // release BEFORE shutdown -- shutdown()'s flush() is unbounded
    shutdown();
}

TEST_CASE("Critical is retained under pressure: it bypasses the queue (delivered synchronously)",
          "[log][diag][sinkfail]")
{
    LoggerConfig cfg;
    cfg.async             = true;
    cfg.flush_on_critical = true;
    fresh_logger(cfg);

    auto            ring_owner = std::make_unique<RingBufferSink>(8);
    RingBufferSink* ring       = ring_owner.get();
    add_sink(std::move(ring_owner));

    CRD_LOG_CRITICAL(g_log_sinkfail_test, "last words");

    // No flush(): a Critical record is delivered synchronously, so the sink already has it -- which is exactly why
    // queue pressure can never drop a Critical.
    auto records = ring->snapshot();
    bool found   = false;
    for (const auto& r : records)
    {
        if (r.level == LogLevel::Critical && std::string_view{r.message}.find("last words") != std::string_view::npos)
        {
            found = true;
        }
    }
    CHECK(found);

    shutdown();
}
