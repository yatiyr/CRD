// DIAG.4c priority starvation. The scheduler drains strictly High -> Normal -> Low with no aging, so a
// sustained flood of a higher lane can indefinitely stall a lower lane's waiting work while the system keeps
// completing other jobs. That is DISTINCT from a hang (completions advance, so hang_verdict reads Progressing);
// the watchdog runs a separate per-lane starvation check keyed off LaneSample {backlog, pops}. The classifier
// starvation_verdict() and its debouncer are pure, so the decision table is unit-tested without a clock; the
// end-to-end case seeds a real Low-lane starvation under a Normal flood and asserts the report names the Low
// lane, and a healthy multi-lane stream never fires.
#include "../../../engine/foundation/jobs/src/hang_watchdog.hpp"

#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using crd::jobs::LaneSample;
using crd::jobs::detail::starvation_verdict;
using crd::jobs::detail::StarvationDetector;

namespace
{
constexpr crd::u8 kHigh   = 0U;
constexpr crd::u8 kNormal = 1U;
constexpr crd::u8 kLow    = 2U;

// Build a LaneSample from the three lanes' backlog and pop counts (indices 0=High,1=Normal,2=Low). Field
// assignment, not a nested positional aggregate, so the two arrays can never be transposed by accident.
LaneSample lane(crd::u32 b_high, crd::u32 b_norm, crd::u32 b_low, crd::u64 p_high, crd::u64 p_norm,
                crd::u64 p_low) noexcept
{
    LaneSample s{};
    s.backlog[0] = b_high;
    s.backlog[1] = b_norm;
    s.backlog[2] = b_low;
    s.pops[0]    = p_high;
    s.pops[1]    = p_norm;
    s.pops[2]    = p_low;
    return s;
}

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int>      g_starv_fires[3]      = {}; // per-lane fire count
std::atomic<crd::u32> g_starv_last_backlog  {0};
std::atomic<crd::u64> g_starv_last_compl    {0};
std::atomic<bool>     g_low_ran             {false};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void record_starv(const crd::jobs::StarvationReport& r, void* /*user*/) noexcept
{
    if (r.lane < 3U)
        g_starv_fires[r.lane].fetch_add(1, std::memory_order_relaxed);
    g_starv_last_backlog.store(r.backlog, std::memory_order_relaxed);
    g_starv_last_compl.store(r.completions, std::memory_order_relaxed);
}

void noop_job(void* /*data*/) noexcept {}
void low_job(void* /*data*/) noexcept { g_low_ran.store(true, std::memory_order_release); }

// A Normal job that busy-spins ~1ms of wall time. A deep batch of these keeps the single worker continuously
// busy on the Normal lane (its pops keep advancing) for far longer than the detection window, with no idle gap
// in which it could reach the Low lane -- a deterministic, refill-race-free flood.
void busy_1ms(void* /*data*/) noexcept
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    while (std::chrono::steady_clock::now() < end)
    { /* spin */
    }
}
} // namespace

TEST_CASE("starvation_verdict: a backlogged lane with flat pops while others advance is starved",
          "[jobs][diag][starvation]")
{
    // Low has one job waiting and made no pops; Normal drained 10 more this window -> Low is starved.
    const LaneSample before = lane(0U, 5U, 1U, 0U, 10U, 0U);
    const LaneSample after  = lane(0U, 5U, 1U, 0U, 20U, 0U);
    CHECK(starvation_verdict(before, after, 100U, 140U) == static_cast<crd::u8>(1U << kLow));
}

TEST_CASE("starvation_verdict: flat everything is a hang, not starvation", "[jobs][diag][starvation]")
{
    // Backlog present but nothing anywhere advanced (no pops, no completions) -> hang territory -> no starvation.
    const LaneSample s = lane(0U, 0U, 2U, 0U, 0U, 7U);
    CHECK(starvation_verdict(s, s, 50U, 50U) == 0U);
}

TEST_CASE("starvation_verdict: a backlogged lane that is draining is not starved", "[jobs][diag][starvation]")
{
    // Low has backlog but its own pops advanced (5 -> 9) -> it is being served, not starved.
    const LaneSample before = lane(0U, 0U, 3U, 0U, 0U, 5U);
    const LaneSample after  = lane(0U, 0U, 2U, 0U, 0U, 9U);
    CHECK(starvation_verdict(before, after, 0U, 0U) == 0U);
}

TEST_CASE("starvation_verdict: no backlog is never starvation", "[jobs][diag][starvation]")
{
    // Normal is popping away but no lane holds waiting work -> nothing starved.
    const LaneSample before = lane(0U, 0U, 0U, 0U, 5U, 0U);
    const LaneSample after  = lane(0U, 0U, 0U, 0U, 9U, 0U);
    CHECK(starvation_verdict(before, after, 0U, 1U) == 0U);
}

TEST_CASE("starvation_verdict: two lanes can be starved at once", "[jobs][diag][starvation]")
{
    // High and Low both backlogged with flat pops while Normal drains -> both flagged.
    const LaneSample before = lane(2U, 0U, 3U, 0U, 7U, 0U);
    const LaneSample after  = lane(2U, 0U, 3U, 0U, 15U, 0U);
    const crd::u8 expected = static_cast<crd::u8>((1U << kHigh) | (1U << kLow));
    CHECK(starvation_verdict(before, after, 0U, 0U) == expected);
}

TEST_CASE("starvation detector: fires once per lane per episode after K consecutive windows",
          "[jobs][diag][starvation]")
{
    constexpr crd::u8 low = static_cast<crd::u8>(1U << kLow);

    StarvationDetector det{3U};
    CHECK(det.feed(low) == 0U);   // 1
    CHECK(det.feed(low) == 0U);   // 2
    CHECK(det.feed(low) == low);  // 3 -> fires
    CHECK(det.feed(low) == 0U);   // stays silent this episode
    CHECK(det.feed(0U) == 0U);    // a healthy window resets the lane
    CHECK(det.feed(low) == 0U);   // 1
    CHECK(det.feed(low) == 0U);   // 2
    CHECK(det.feed(low) == low);  // 3 -> fires again (new episode)
}

TEST_CASE("starvation detector: lanes are debounced independently", "[jobs][diag][starvation]")
{
    constexpr crd::u8 high = static_cast<crd::u8>(1U << kHigh);
    constexpr crd::u8 low  = static_cast<crd::u8>(1U << kLow);

    StarvationDetector det{3U};
    CHECK(det.feed(low) == 0U);              // Low:1
    CHECK(det.feed(static_cast<crd::u8>(low | high)) == 0U); // Low:2 High:1
    CHECK(det.feed(static_cast<crd::u8>(low | high)) == low); // Low:3 -> fires; High:2
    CHECK(det.feed(high) == high);           // High:3 -> fires; Low reset (not in mask)
}

TEST_CASE("starvation watchdog: a Low lane flooded by Normal work is reported as starved",
          "[jobs][diag][starvation]")
{
    for (auto& f : g_starv_fires)
        f.store(0, std::memory_order_relaxed);
    g_starv_last_backlog.store(0, std::memory_order_relaxed);
    g_low_ran.store(false, std::memory_order_relaxed);
    crd::jobs::set_starvation_handler(&record_starv, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads             = 2U;  // main (thread 0, not pumping) + one background worker: the sole drainer
    cfg.hang_watchdog_period_ms = 20U;
    crd::jobs::init(cfg);

    // Queue a deep batch of time-consuming Normal jobs UPFRONT (one run(), no feeder/refill race): each spins
    // ~1ms, so the single background worker is continuously busy on the Normal lane for ~kNormalJobs ms --
    // far beyond the K=3 (~60ms) detection window -- and never idles into the Low lane. Normal pops advance the
    // whole time (each job completes), which is exactly what tells starvation apart from a hang. Held unwaited
    // (main does not pump), so only the background worker drains it during detection.
    constexpr int      kNormalJobs = 300;
    crd::jobs::JobDecl  normals[kNormalJobs];
    for (auto& j : normals)
        j.fn = &busy_1ms; // default priority = Normal
    crd::jobs::Counter* const nc = crd::jobs::run({normals, static_cast<crd::usize>(kNormalJobs)});

    // Seed one Low-priority job behind the deep Normal queue: it sits in the Low injection queue, which the busy
    // worker never reaches while Normal is non-empty.
    crd::jobs::JobDecl low{};
    low.fn                        = &low_job;
    low.priority                  = crd::jobs::Priority::Low;
    crd::jobs::Counter* const lc  = crd::jobs::run(low);

    // Poll (bounded) for the watchdog to report Low starvation -- it fires after K=3 windows (~60ms). main does
    // not pump while polling (sleep, not wait), so only the busy worker drains, keeping Low starved.
    for (int i = 0; i < 400 && g_starv_fires[kLow].load(std::memory_order_relaxed) == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    CHECK(g_starv_fires[kLow].load(std::memory_order_relaxed) >= 1);   // the Low lane was reported starved
    CHECK(g_starv_fires[kHigh].load(std::memory_order_relaxed) == 0);  // High has no backlog -> never flagged
    CHECK(g_starv_last_backlog.load(std::memory_order_relaxed) >= 1U); // the waiting Low job at detection
    CHECK_FALSE(g_low_ran.load(std::memory_order_acquire));            // still starved: it has not run yet

    // Teardown: drain the Normal batch (main now pumps too), after which the worker finally reaches the Low job.
    crd::jobs::wait(nc);
    crd::jobs::wait(lc);
    CHECK(g_low_ran.load(std::memory_order_acquire));

    crd::jobs::shutdown();
    crd::jobs::set_starvation_handler(nullptr, nullptr);
}

TEST_CASE("starvation watchdog: a healthy multi-lane stream never fires", "[jobs][diag][starvation]")
{
    for (auto& f : g_starv_fires)
        f.store(0, std::memory_order_relaxed);
    crd::jobs::set_starvation_handler(&record_starv, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads             = 4U;
    cfg.hang_watchdog_period_ms = 20U;
    crd::jobs::init(cfg);

    // A balanced stream across all three lanes; every lane is drained promptly, so none stays backlogged
    // without progress. Interleave so no single lane is ever the only one waiting.
    for (int i = 0; i < 600; ++i)
    {
        crd::jobs::JobDecl j{};
        j.fn       = &noop_job;
        j.priority = (i % 3 == 0)   ? crd::jobs::Priority::High
                     : (i % 3 == 1) ? crd::jobs::Priority::Normal
                                    : crd::jobs::Priority::Low;
        crd::jobs::run_and_wait(j);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    CHECK(g_starv_fires[kHigh].load(std::memory_order_relaxed) == 0);
    CHECK(g_starv_fires[kNormal].load(std::memory_order_relaxed) == 0);
    CHECK(g_starv_fires[kLow].load(std::memory_order_relaxed) == 0);

    crd::jobs::shutdown();
    crd::jobs::set_starvation_handler(nullptr, nullptr);
}
