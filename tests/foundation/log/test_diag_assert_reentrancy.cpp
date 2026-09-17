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
#include <memory>

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
