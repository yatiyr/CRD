// DIAG.5c(f) defect 9: the per-site assert-ignore table must have a defined, bounded behaviour under churn -- many
// distinct ignored sites -- instead of silently refusing to record once full. It is now a 256-entry FIFO ring:
// recording a 257th distinct site evicts the oldest (which fires again), counted by assert_ignore_eviction_count()
// with one stderr line on the first eviction. Not CRD_ENABLE_ASSERTS-gated: report_assert_failure() and
// ignore_assert_site() exist in every build (CRD_FATAL uses the former). NOTE: a headless build never reaches this
// table (it aborts before the dialog); this is interactive/opt-in behaviour, exercised here via the public API.
// DIAG.5c: an assert raised inside the assert handler (the log bridge out of memory outside a sink) skips the handler
// through its per-thread guard and goes straight to the platform handler.

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>

using namespace crd;

namespace
{
std::atomic<int> g_ignore_test_fired{0};

void counting_handler(const char* /*expr*/, const char* /*file*/, int /*line*/, const char* /*msg*/) noexcept
{
    g_ignore_test_fired.fetch_add(1, std::memory_order_relaxed);
}

int ignore_platform_handler(const char* /*formatted*/) noexcept
{
    return 0; // ignore: no dialog, no abort -- report_assert_failure returns 0
}

std::atomic<int> g_platform_reports{0};
int g_handler_depth = 0; // bounds the recursion, so a lost guard fails the case instead of overflowing

int counting_platform_handler(const char* /*formatted*/) noexcept
{
    g_platform_reports.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void reasserting_handler(const char* /*expr*/, const char* /*file*/, int /*line*/, const char* /*msg*/) noexcept
{
    g_ignore_test_fired.fetch_add(1, std::memory_order_relaxed);
    if (++g_handler_depth < 3)
    {
        (void)crd::detail::report_assert_failure("nested", "synthetic_nested_site", 2);
    }
}
} // namespace

TEST_CASE("assert ignore table is a bounded FIFO ring: churn evicts the oldest, counted (not silent)",
          "[core][assert][ignore]")
{
    auto* prev_h  = crd::get_assert_handler();
    auto* prev_pf = crd::get_assert_platform_handler();
    crd::set_assert_handler(&counting_handler);
    crd::set_assert_platform_handler(&ignore_platform_handler);

    // A single static file pointer (compared by pointer) + distinct line numbers = 300 distinct sites, unique to
    // this test (so none pre-exist in the shared table).
    static const char* const kFile = "synthetic_ignore_churn_site";

    const u64 evict_before = crd::assert_ignore_eviction_count();
    for (int i = 0; i < 300; ++i)
    {
        REQUIRE(crd::ignore_assert_site(kFile, i)); // always records (FIFO), never silently refuses
    }

    // 300 distinct into a 256 ring => at least 44 evictions (exactly 44 from empty; more if the shared table already
    // held entries). The point is it is counted, not silent.
    CHECK(crd::assert_ignore_eviction_count() - evict_before >= 44);

    // After 300 sequential inserts the ring holds the most-recent 256 = lines 44..299, regardless of prior state.
    // A still-ignored site returns 0 WITHOUT firing the handler (report_assert_failure short-circuits on ignore).
    g_ignore_test_fired.store(0, std::memory_order_relaxed);
    const int r_recent = crd::detail::report_assert_failure("x", kFile, 299);
    CHECK(r_recent == 0);
    CHECK(g_ignore_test_fired.load(std::memory_order_relaxed) == 0); // ignored -> handler never ran

    // The oldest site (line 0) was evicted -> no longer ignored -> the handler fires again (churn is not permanent).
    g_ignore_test_fired.store(0, std::memory_order_relaxed);
    (void)crd::detail::report_assert_failure("x", kFile, 0);
    CHECK(g_ignore_test_fired.load(std::memory_order_relaxed) == 1); // evicted -> fires again

    crd::set_assert_handler(prev_h);
    crd::set_assert_platform_handler(prev_pf);
}

TEST_CASE("an assert raised inside the assert handler skips the handler and reaches the platform handler",
          "[core][assert][reentrancy]")
{
    auto* prev_h = crd::get_assert_handler();
    auto* prev_pf = crd::get_assert_platform_handler();
    crd::set_assert_handler(&reasserting_handler);
    crd::set_assert_platform_handler(&counting_platform_handler);
    g_ignore_test_fired.store(0, std::memory_order_relaxed);
    g_platform_reports.store(0, std::memory_order_relaxed);
    g_handler_depth = 0;

    (void)crd::detail::report_assert_failure("outer", "synthetic_nested_site", 1);

    CHECK(g_ignore_test_fired.load(std::memory_order_relaxed) == 1); // the nested report did not run the handler
    CHECK(g_platform_reports.load(std::memory_order_relaxed) == 2);  // the nested and the outer report both did

    crd::set_assert_handler(prev_h);
    crd::set_assert_platform_handler(prev_pf);
}
