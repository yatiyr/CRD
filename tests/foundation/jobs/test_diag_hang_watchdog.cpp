// DIAG.4c -- progress-sensitive hang watchdog. The classifier hang_verdict() is pure, so the verdicts that
// cannot be reproduced in a live test -- a debugger pause / OS suspend, and a single long-running job -- are
// exercised by feeding synthetic samples and elapsed times. The end-to-end cases check the progress_snapshot
// signal it consumes: a completing stream advances completions and goes quiescent; a long/parked workload
// keeps a worker executing.
#include "../../../engine/foundation/jobs/src/hang_watchdog.hpp"

#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <initializer_list>
#include <thread>

using crd::jobs::HangKind;
using crd::jobs::ProgressSample;
using crd::jobs::WaitGraphNode;
using crd::jobs::detail::classify_hang;
using crd::jobs::detail::HangDetector;
using crd::jobs::detail::hang_verdict;
using crd::jobs::detail::HangVerdict;

namespace
{
// Build a wait-graph node carrying just the two ids classify_hang() reads: `own` (this fiber's own job counter,
// the edge source) and `waits_on` (the counter it is blocked on, the edge target). Field assignment, not a
// positional aggregate, so a future field reorder cannot silently mislabel these.
WaitGraphNode wg(crd::u64 own, crd::u64 waits_on) noexcept
{
    WaitGraphNode n{};
    n.own_task_id        = own;
    n.waiting_on_task_id = waits_on;
    return n;
}

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_gate{false};        // held closed to keep a child (and thus its parent) parked/executing
std::atomic<int>  g_ticks{0};           // bumped by cheap jobs so a completing stream is observable
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void tick_job(void* /*data*/) noexcept { g_ticks.fetch_add(1, std::memory_order_relaxed); }

void gated_child(void* /*data*/) noexcept
{
    while (!g_gate.load(std::memory_order_acquire))
        std::this_thread::yield(); // keeps this worker EXECUTING (a long/blocked job), not parked
}

// Feed a verdict sequence to a fresh K=3 detector and return the 1-based index of the single window it fires
// on, or 0 if it never fires (asserts it never fires twice).
int fire_index(std::initializer_list<HangVerdict> seq)
{
    HangDetector det{3U};
    int          fired_at = 0;
    int          i        = 0;
    for (const HangVerdict v : seq)
    {
        ++i;
        if (det.feed(v))
        {
            REQUIRE(fired_at == 0); // exactly once per episode
            fired_at = i;
        }
    }
    return fired_at;
}

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int>      g_hang_fires{0};
std::atomic<crd::u32> g_last_stale{0};
std::atomic<crd::u32> g_last_executing{0};
std::atomic<crd::u32> g_last_outstanding{0};
std::atomic<crd::u64> g_last_parked_total{0};
std::atomic<crd::u64> g_last_parent_task{0};
std::atomic<crd::u32> g_last_remaining{0};
std::atomic<int>      g_last_kind{-1};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void count_hang(const crd::jobs::HangReport& /*r*/, void* /*user*/) noexcept
{
    g_hang_fires.fetch_add(1, std::memory_order_relaxed);
}

// Records report fields into atomics -- `parked` is valid only during the call, so copy out here.
void record_hang(const crd::jobs::HangReport& r, void* /*user*/) noexcept
{
    g_hang_fires.fetch_add(1, std::memory_order_relaxed);
    g_last_stale.store(r.stale_windows, std::memory_order_relaxed);
    g_last_executing.store(r.executing, std::memory_order_relaxed);
    g_last_outstanding.store(r.outstanding, std::memory_order_relaxed);
    g_last_parked_total.store(r.parked_total, std::memory_order_relaxed);
    g_last_kind.store(static_cast<int>(r.kind), std::memory_order_relaxed);
    if (!r.parked.empty())
    {
        g_last_parent_task.store(r.parked[0].waiting_on_parent_task_id, std::memory_order_relaxed);
        g_last_remaining.store(r.parked[0].waiting_on_remaining, std::memory_order_relaxed);
    }
}

void parking_root(void* /*data*/) noexcept
{
    crd::jobs::JobDecl child{};
    child.fn = &gated_child;
    crd::jobs::run_and_wait(child); // this fiber PARKS on the child's counter while the child executes
}
} // namespace

TEST_CASE("hang_verdict: quiescent is never a hang", "[jobs][diag][hang]")
{
    const ProgressSample before{10U, 0U, 0U, false};
    const ProgressSample after{10U, 0U, 0U, true}; // nothing outstanding
    CHECK(hang_verdict(before, after, 500U, 505U) == HangVerdict::None);
}

TEST_CASE("hang_verdict: advancing completions is progress", "[jobs][diag][hang]")
{
    const ProgressSample before{10U, 0U, 1U, false};
    const ProgressSample after{11U, 0U, 1U, false}; // work still outstanding, one job finished this window
    CHECK(hang_verdict(before, after, 500U, 505U) == HangVerdict::Progressing);
}

TEST_CASE("hang_verdict: a long job executing with flat completions is progress, not a hang",
          "[jobs][diag][hang]")
{
    // The acceptance's "long progressing offline work does not trigger a false deadlock claim": completions
    // are flat (the one long job has not finished) but a worker is still executing it.
    const ProgressSample before{10U, 1U, 1U, false};
    const ProgressSample after{10U, 1U, 1U, false}; // outstanding, flat completions, but a worker is executing
    CHECK(hang_verdict(before, after, 500U, 505U) == HangVerdict::Progressing);
}

TEST_CASE("hang_verdict: a badly overrun window is Paused (debugger/suspend), not a hang", "[jobs][diag][hang]")
{
    // Frozen process: the watchdog's own sleep overran ~10x. Even with the deadlock shape (flat completions,
    // nothing executing) this must read Paused so the freeze is not miscounted as a stall.
    const ProgressSample before{10U, 0U, 1U, false};
    const ProgressSample after{10U, 0U, 1U, false}; // deadlock shape, but the overrun means the process froze
    CHECK(hang_verdict(before, after, 500U, 5000U) == HangVerdict::Paused);
}

TEST_CASE("hang_verdict: outstanding work with no completion and no worker executing is a suspected hang",
          "[jobs][diag][hang]")
{
    const ProgressSample before{10U, 0U, 1U, false};
    const ProgressSample after{10U, 0U, 1U, false}; // outstanding, nothing finished, nobody executing
    CHECK(hang_verdict(before, after, 500U, 505U) == HangVerdict::SuspectedHang);
}

TEST_CASE("classify_hang: nothing parked but work outstanding is executor-starved", "[jobs][diag][hang]")
{
    // A queued job with no worker draining it: no fiber is parked, yet outstanding > 0. The pool is not
    // deadlocked -- it simply has no executor for the queued work.
    CHECK(classify_hang({}, 1U, 0U) == HangKind::ExecutorStarved);
}

TEST_CASE("classify_hang: parked fibers with no closing cycle are parked-stalled", "[jobs][diag][hang]")
{
    // A parked fiber blocked on a counter that no other parked fiber owns -- the awaited work was never
    // dispatched (a chain end), not a mutual wait.
    const WaitGraphNode chain1[] = {wg(/*own*/ 10U, /*waits_on*/ 20U)};
    CHECK(classify_hang(chain1, 1U, 0U) == HangKind::ParkedStalled);

    // A longer chain 10->20->30 that still terminates (nobody owns 30).
    const WaitGraphNode chain2[] = {wg(10U, 20U), wg(20U, 30U)};
    CHECK(classify_hang(chain2, 1U, 0U) == HangKind::ParkedStalled);

    // A fan-out to two chain-ends sharing one owner id (30 owned by two parked jobs, both waiting on nothing
    // owned) is still no cycle.
    const WaitGraphNode fan[] = {wg(10U, 30U), wg(30U, 40U), wg(30U, 50U)};
    CHECK(classify_hang(fan, 1U, 0U) == HangKind::ParkedStalled);

    // Two unresolved chain-ends both carrying own id 0 must NOT be linked into a spurious 0==0 cycle.
    const WaitGraphNode zeros[] = {wg(0U, 0U), wg(0U, 0U)};
    CHECK(classify_hang(zeros, 1U, 0U) == HangKind::ParkedStalled);
}

TEST_CASE("classify_hang: a wait cycle among parked fibers is a deadlock", "[jobs][diag][hang]")
{
    // Two jobs each parked on the other's counter: 10 waits on 20, 20 waits on 10.
    const WaitGraphNode two[] = {wg(10U, 20U), wg(20U, 10U)};
    CHECK(classify_hang(two, 2U, 0U) == HangKind::WaitCycle);

    // A three-node cycle 10->20->30->10.
    const WaitGraphNode three[] = {wg(10U, 20U), wg(20U, 30U), wg(30U, 10U)};
    CHECK(classify_hang(three, 3U, 0U) == HangKind::WaitCycle);

    // A job waiting on its OWN counter is a self-edge -- a real (if degenerate) deadlock.
    const WaitGraphNode self[] = {wg(10U, 10U)};
    CHECK(classify_hang(self, 1U, 0U) == HangKind::WaitCycle);

    // A cycle wins even when an extra parked job shares an owner id and dangles off the graph: 10<->20 closes
    // while a second owner-20 job waits on an unowned 99. The cycle must still be found.
    const WaitGraphNode shared[] = {wg(10U, 20U), wg(20U, 10U), wg(20U, 99U)};
    CHECK(classify_hang(shared, 3U, 0U) == HangKind::WaitCycle);
}

TEST_CASE("hang watchdog: a completing stream advances progress and goes quiescent", "[jobs][diag][hang]")
{
    g_ticks.store(0, std::memory_order_relaxed);

    crd::jobs::Config cfg;
    cfg.num_threads = 4U;
    crd::jobs::init(cfg);

    const ProgressSample idle = crd::jobs::progress_snapshot();
    CHECK(idle.quiescent);              // nothing submitted yet
    CHECK(idle.executing == 0U);

    constexpr int kJobs = 200;
    for (int i = 0; i < kJobs; ++i)
    {
        crd::jobs::JobDecl j{};
        j.fn = &tick_job;
        crd::jobs::run_and_wait(j);
    }

    const ProgressSample after = crd::jobs::progress_snapshot();
    CHECK(after.completions >= static_cast<crd::u64>(kJobs)); // every job's completion was counted
    CHECK(after.quiescent);                                   // drained again after the last wait
    CHECK(g_ticks.load(std::memory_order_relaxed) == kJobs);

    crd::jobs::shutdown();
}

TEST_CASE("hang watchdog: a parked root over an executing child reads executing>0 and non-quiescent",
          "[jobs][diag][hang]")
{
    g_gate.store(false, std::memory_order_relaxed);

    crd::jobs::Config cfg;
    cfg.num_threads = 4U;
    crd::jobs::init(cfg);

    crd::jobs::JobDecl root{};
    root.fn                    = &parking_root;
    crd::jobs::Counter* handle = crd::jobs::run(root);

    // Wait until the workload is live: the root has parked on the child and the child is executing (spinning).
    // This is the "long work" shape -- flat completions but a worker executing -- so the watchdog must NOT
    // read it as a hang.
    ProgressSample s{};
    const auto     deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        s = crd::jobs::progress_snapshot();
        if (!s.quiescent && s.executing >= 1U)
            break;
        std::this_thread::yield();
    }
    CHECK_FALSE(s.quiescent);
    CHECK(s.executing >= 1U);

    // A window over this state, with completions flat, classifies as Progressing (a worker is executing).
    const ProgressSample before = s;
    const ProgressSample after  = crd::jobs::progress_snapshot();
    CHECK(hang_verdict(before, after, 500U, 505U) == HangVerdict::Progressing);

    g_gate.store(true, std::memory_order_release);
    crd::jobs::wait(handle);
    crd::jobs::shutdown();
}

TEST_CASE("hang watchdog: a completed-but-unwaited counter is not a hang", "[jobs][diag][hang]")
{
    // The common frame pattern: issue work, hold the counter, do other things, wait() later. The job finishes
    // (value hits 0) but the counter stays ACQUIRED until wait() releases it, so is_quiescent() is false. That
    // must NOT read as a hang -- there is no outstanding work, only a held-but-satisfied counter.
    g_ticks.store(0, std::memory_order_relaxed);
    g_hang_fires.store(0, std::memory_order_relaxed);
    crd::jobs::set_hang_handler(&count_hang, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads             = 4U;
    cfg.hang_watchdog_period_ms = 20U;
    crd::jobs::init(cfg);

    crd::jobs::JobDecl j{};
    j.fn                       = &tick_job;
    crd::jobs::Counter* handle = crd::jobs::run(j);

    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // ~15 windows with the job done but unwaited
    CHECK(g_hang_fires.load(std::memory_order_relaxed) == 0);

    crd::jobs::wait(handle);
    crd::jobs::shutdown();
    crd::jobs::set_hang_handler(nullptr, nullptr);
}

TEST_CASE("hang detector: fires once on K consecutive stale windows, resets on any progress",
          "[jobs][diag][hang]")
{
    constexpr HangVerdict S = HangVerdict::SuspectedHang;
    constexpr HangVerdict P = HangVerdict::Progressing;
    constexpr HangVerdict Z = HangVerdict::Paused;
    constexpr HangVerdict N = HangVerdict::None;

    CHECK(fire_index({S, S}) == 0);          // two is not enough
    CHECK(fire_index({S, S, S}) == 3);       // third consecutive fires
    CHECK(fire_index({S, S, S, S}) == 3);    // and only once
    CHECK(fire_index({S, S, P, S, S, S}) == 6); // progress resets the run
    CHECK(fire_index({S, S, Z, S, S, S}) == 6); // a pause window resets it too
    CHECK(fire_index({S, S, N, S, S, S}) == 6); // quiescence resets it
    CHECK(fire_index({P, P, P, P}) == 0);    // never on progress
}

TEST_CASE("hang watchdog: enabled watchdog starts and shuts down cleanly", "[jobs][diag][hang]")
{
    g_ticks.store(0, std::memory_order_relaxed);
    g_hang_fires.store(0, std::memory_order_relaxed);
    crd::jobs::set_hang_handler(&count_hang, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads             = 4U;
    cfg.hang_watchdog_period_ms = 20U; // watchdog thread live
    crd::jobs::init(cfg);

    for (int i = 0; i < 500; ++i)
    {
        crd::jobs::JobDecl j{};
        j.fn = &tick_job;
        crd::jobs::run_and_wait(j);
    }

    crd::jobs::shutdown(); // must stop+join the watchdog before pool teardown (ASan would catch a bad order)
    crd::jobs::set_hang_handler(nullptr, nullptr);

    CHECK(g_ticks.load(std::memory_order_relaxed) == 500);
    CHECK(g_hang_fires.load(std::memory_order_relaxed) == 0); // a completing stream is never a hang
}

TEST_CASE("hang watchdog: work queued with no executor fires once with an honest report", "[jobs][diag][hang]")
{
    // A genuine stall: one background-worker-less pool (num_threads=1), a job queued, and the main thread not
    // pumping. Nothing can run the job -> outstanding stays 1, nobody executes, completions flat. The watchdog
    // must fire exactly once (once-per-episode, proven live) with a truthful report.
    g_hang_fires.store(0, std::memory_order_relaxed);
    g_last_stale.store(0, std::memory_order_relaxed);
    g_last_executing.store(99U, std::memory_order_relaxed);
    g_last_outstanding.store(0, std::memory_order_relaxed);
    g_last_parked_total.store(99U, std::memory_order_relaxed);
    g_last_kind.store(-1, std::memory_order_relaxed);
    crd::jobs::set_hang_handler(&record_hang, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads             = 1U; // main is the only executor; it will not pump
    cfg.hang_watchdog_period_ms = 20U;
    crd::jobs::init(cfg);

    crd::jobs::JobDecl j{};
    j.fn                       = &tick_job;
    crd::jobs::Counter* handle = crd::jobs::run(j); // queued, starved of an executor

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(g_hang_fires.load(std::memory_order_relaxed) == 1);          // fired exactly once per episode
    CHECK(g_last_stale.load(std::memory_order_relaxed) == 3U);         // after K=3 stale windows
    CHECK(g_last_outstanding.load(std::memory_order_relaxed) >= 1U);   // real outstanding work
    CHECK(g_last_executing.load(std::memory_order_relaxed) == 0U);     // nobody executing
    CHECK(g_last_parked_total.load(std::memory_order_relaxed) == 0U);  // queued, not parked -> no wait-graph node
    CHECK(g_last_kind.load(std::memory_order_relaxed) ==
          static_cast<int>(HangKind::ExecutorStarved)); // no fiber parked, work queued -> starved of an executor

    (void)crd::jobs::pump_main_thread_until_idle(); // run the starved job
    crd::jobs::wait(handle);
    crd::jobs::shutdown();
    crd::jobs::set_hang_handler(nullptr, nullptr);
}

TEST_CASE("hang watchdog: a parked fiber fires with wait-graph evidence", "[jobs][diag][hang]")
{
    // The full (a)+(b) stack: a parked root over an un-run child, on a pump-only pool, with no executor to make
    // progress. The watchdog fires and the report carries the parked-fiber evidence identifying the counter.
    g_gate.store(false, std::memory_order_relaxed);
    g_hang_fires.store(0, std::memory_order_relaxed);
    g_last_parked_total.store(0, std::memory_order_relaxed);
    g_last_parent_task.store(0, std::memory_order_relaxed);
    g_last_remaining.store(0, std::memory_order_relaxed);
    g_last_kind.store(-1, std::memory_order_relaxed);
    crd::jobs::set_hang_handler(&record_hang, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads             = 1U;
    cfg.hang_watchdog_period_ms = 20U;
    crd::jobs::init(cfg);

    crd::jobs::JobDecl root{};
    root.fn                    = &parking_root;
    crd::jobs::Counter* handle = crd::jobs::run(root);

    (void)crd::jobs::pump_main_thread_once(); // main runs the root, which submits the child and PARKS; pump returns

    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // child queued, nobody to run it -> stall
    CHECK(g_hang_fires.load(std::memory_order_relaxed) >= 1);
    CHECK(g_last_parked_total.load(std::memory_order_relaxed) == 1U);      // exactly the parked root
    CHECK(g_last_parent_task.load(std::memory_order_relaxed) != 0U);       // child submitted from the root fiber
    CHECK(g_last_remaining.load(std::memory_order_relaxed) == 1U);         // the child's counter still at 1
    CHECK(g_last_kind.load(std::memory_order_relaxed) ==
          static_cast<int>(HangKind::ParkedStalled)); // root parked on an un-dispatched child -> no cycle, stalled

    g_gate.store(true, std::memory_order_release); // let the child return once it finally runs
    (void)crd::jobs::pump_main_thread_until_idle();       // runs child -> resumes root -> root completes
    crd::jobs::wait(handle);
    crd::jobs::shutdown();
    crd::jobs::set_hang_handler(nullptr, nullptr);
}

TEST_CASE("hang watchdog: long executing work over many windows never fires", "[jobs][diag][hang]")
{
    g_gate.store(false, std::memory_order_relaxed);
    g_hang_fires.store(0, std::memory_order_relaxed);
    crd::jobs::set_hang_handler(&count_hang, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads             = 4U;
    cfg.hang_watchdog_period_ms = 20U;
    crd::jobs::init(cfg);

    crd::jobs::JobDecl root{};
    root.fn                    = &parking_root;
    crd::jobs::Counter* handle = crd::jobs::run(root);

    // Hold the long (executing) workload across ~15 watchdog windows; a worker stays executing the whole time,
    // so the verdict is Progressing every window and the handler must never fire.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(g_hang_fires.load(std::memory_order_relaxed) == 0);

    g_gate.store(true, std::memory_order_release);
    crd::jobs::wait(handle);
    crd::jobs::shutdown();
    crd::jobs::set_hang_handler(nullptr, nullptr);
}
