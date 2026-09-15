// DIAG.4c livelock. A livelock -- tasks running but making no forward progress -- cannot be seen by any
// aggregate: a spin loop keeps a worker executing, identical to a legitimate long job (both read Progressing).
// So detection is OPT-IN: a long task calls jobs::note_progress() as it advances, and the watchdog reports a
// monitored task whose progress epoch stays flat across several windows while it keeps a worker busy. The
// tracker (LivelockTracker) is pure, so its fire policy is unit-tested without a clock; the live cases prove a
// real mutual livelock is reported (and is DISTINCT from a hang -- the hang handler stays silent), and that a
// ticking task, an unmonitored long job, and a parked monitored task are never flagged.
#include "../../../engine/foundation/jobs/src/hang_watchdog.hpp"

#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <initializer_list>
#include <thread>

using crd::jobs::ProgressNode;
using crd::jobs::detail::LivelockTracker;

namespace
{
// Build a monitored-task node. parent_task_id is fixed (irrelevant to the tracker, which keys on task_id).
ProgressNode pn(crd::u32 index, crd::u8 tier, crd::u64 task_id, crd::u32 epoch) noexcept
{
    ProgressNode n{};
    n.fiber_index    = index;
    n.tier           = tier;
    n.task_id        = task_id;
    n.parent_task_id = 1U;
    n.progress_epoch = epoch;
    return n;
}

// Feed one window to the tracker and report how many fired plus their task ids.
struct FeedResult
{
    crd::usize count;
    crd::u64   ids[8];
};
FeedResult feed_window(LivelockTracker& t, std::initializer_list<ProgressNode> nodes)
{
    ProgressNode in[16];
    crd::usize   n = 0U;
    for (const ProgressNode& node : nodes)
        in[n++] = node;
    ProgressNode out[16]{};
    const crd::usize c = t.feed(std::span<const ProgressNode>(in, n), std::span<ProgressNode>(out, 16));
    FeedResult   r{c, {}};
    for (crd::usize i = 0U; i < c && i < 8U; ++i)
        r.ids[i] = out[i].task_id;
    return r;
}

bool contains(const FeedResult& r, crd::u64 id) noexcept
{
    for (crd::usize i = 0U; i < r.count && i < 8U; ++i)
        if (r.ids[i] == id)
            return true;
    return false;
}

// --- live-case globals ---
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool>     g_a_flag{false};
std::atomic<bool>     g_b_flag{false};
std::atomic<bool>     g_escape{false};
std::atomic<crd::u64> g_a_task{0};
std::atomic<crd::u64> g_b_task{0};
std::atomic<int>      g_ll_fires{0};
std::atomic<crd::u64> g_ll_ids[8]  = {};
std::atomic<int>      g_hang_fires{0};
std::atomic<bool>     g_gate{false};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void record_livelock(const crd::jobs::LivelockReport& r, void* /*user*/) noexcept
{
    const int i = g_ll_fires.fetch_add(1, std::memory_order_relaxed);
    if (i >= 0 && i < 8)
        g_ll_ids[i].store(r.task_id, std::memory_order_relaxed);
}
void count_hang(const crd::jobs::HangReport& /*r*/, void* /*user*/) noexcept
{
    g_hang_fires.fetch_add(1, std::memory_order_relaxed);
}
bool ll_fired_for(crd::u64 id) noexcept
{
    const int n = g_ll_fires.load(std::memory_order_relaxed);
    for (int i = 0; i < n && i < 8; ++i)
        if (g_ll_ids[i].load(std::memory_order_relaxed) == id)
            return true;
    return false;
}

// Two jobs that livelock each other: each opts in once, then spins waiting for a flag the other sets only AFTER
// its own wait -- so neither wait ever completes. Both keep a worker executing with a flat epoch.
void job_a(void* /*data*/) noexcept
{
    g_a_task.store(crd::jobs::current_task_id(), std::memory_order_release);
    crd::jobs::note_progress(); // opt in (epoch -> 1), then never again
    while (!g_b_flag.load(std::memory_order_acquire) && !g_escape.load(std::memory_order_acquire))
        std::this_thread::yield();
    g_a_flag.store(true, std::memory_order_release);
}
void job_b(void* /*data*/) noexcept
{
    g_b_task.store(crd::jobs::current_task_id(), std::memory_order_release);
    crd::jobs::note_progress();
    while (!g_a_flag.load(std::memory_order_acquire) && !g_escape.load(std::memory_order_acquire))
        std::this_thread::yield();
    g_b_flag.store(true, std::memory_order_release);
}

// A monitored task that keeps TICKING: it advances its epoch every iteration, so it must never be flagged.
void ticking_job(void* /*data*/) noexcept
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < end && !g_escape.load(std::memory_order_acquire))
    {
        crd::jobs::note_progress();
        std::this_thread::yield();
    }
}

// A long job that never opts in: epoch stays 0, so it is unmonitored and never flagged.
void unmonitored_job(void* /*data*/) noexcept
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < end && !g_escape.load(std::memory_order_acquire))
        std::this_thread::yield();
}

void gated_child(void* /*data*/) noexcept
{
    while (!g_gate.load(std::memory_order_acquire))
        std::this_thread::yield();
}

// A monitored root that opts in and then PARKS on a gated child. While parked (waiting_on != nullptr) it is
// excluded from the monitored snapshot, so it must not be flagged even though its epoch is flat.
void parking_monitored_root(void* /*data*/) noexcept
{
    crd::jobs::note_progress();
    crd::jobs::JobDecl child{};
    child.fn = &gated_child;
    crd::jobs::run_and_wait(child);
}
} // namespace

TEST_CASE("livelock tracker: fires on the K-th flat window, once per episode", "[jobs][diag][livelock]")
{
    LivelockTracker t{3U};
    const ProgressNode a = pn(0U, 0U, 100U, 5U);
    CHECK(feed_window(t, {a}).count == 0U); // window 1: baseline
    CHECK(feed_window(t, {a}).count == 0U); // window 2: flat, stale 1
    CHECK(feed_window(t, {a}).count == 0U); // window 3: flat, stale 2
    CHECK(feed_window(t, {a}).count == 1U); // window 4: flat, stale 3 -> fires
    CHECK(feed_window(t, {a}).count == 0U); // stays silent this episode
}

TEST_CASE("livelock tracker: an advancing epoch never fires", "[jobs][diag][livelock]")
{
    LivelockTracker t{3U};
    for (crd::u32 e = 1U; e <= 8U; ++e)
        CHECK(feed_window(t, {pn(0U, 0U, 100U, e)}).count == 0U); // epoch advances every window
}

TEST_CASE("livelock tracker: two monitored tasks fire independently", "[jobs][diag][livelock]")
{
    LivelockTracker t{3U};
    const ProgressNode a = pn(0U, 0U, 100U, 5U);
    const ProgressNode b = pn(1U, 0U, 200U, 9U);
    CHECK(feed_window(t, {a, b}).count == 0U);
    CHECK(feed_window(t, {a, b}).count == 0U);
    CHECK(feed_window(t, {a, b}).count == 0U);
    const FeedResult r = feed_window(t, {a, b}); // both reach K together
    CHECK(r.count == 2U);
    CHECK(contains(r, 100U));
    CHECK(contains(r, 200U));
}

TEST_CASE("livelock tracker: a task absent for a window restarts its episode", "[jobs][diag][livelock]")
{
    LivelockTracker t{3U};
    const ProgressNode a = pn(0U, 0U, 100U, 5U);
    CHECK(feed_window(t, {a}).count == 0U); // baseline
    CHECK(feed_window(t, {a}).count == 0U); // stale 1
    CHECK(feed_window(t, {}).count == 0U);  // absent -> evicted (task completed or parked)
    CHECK(feed_window(t, {a}).count == 0U); // new baseline
    CHECK(feed_window(t, {a}).count == 0U); // stale 1
    CHECK(feed_window(t, {a}).count == 0U); // stale 2
    CHECK(feed_window(t, {a}).count == 1U); // stale 3 -> fires (staleness did not carry across the gap)
}

TEST_CASE("livelock tracker: a reused fiber slot with a new task id is a fresh entry", "[jobs][diag][livelock]")
{
    LivelockTracker t{3U};
    const ProgressNode t1 = pn(0U, 0U, 100U, 5U); // same slot (index 0, tier 0)...
    const ProgressNode t2 = pn(0U, 0U, 200U, 0U); // ...different task id
    CHECK(feed_window(t, {t1}).count == 0U); // baseline for task 100
    CHECK(feed_window(t, {t1}).count == 0U); // stale 1
    CHECK(feed_window(t, {t1}).count == 0U); // stale 2
    CHECK(feed_window(t, {t2}).count == 0U); // task 100 evicted; task 200 is a fresh baseline, not stale 3
    CHECK(feed_window(t, {t2}).count == 0U); // stale 1 for task 200
}

TEST_CASE("livelock watchdog: a mutual livelock is reported for both tasks and no hang fires",
          "[jobs][diag][livelock]")
{
    g_a_flag.store(false, std::memory_order_relaxed);
    g_b_flag.store(false, std::memory_order_relaxed);
    g_escape.store(false, std::memory_order_relaxed);
    g_a_task.store(0, std::memory_order_relaxed);
    g_b_task.store(0, std::memory_order_relaxed);
    g_ll_fires.store(0, std::memory_order_relaxed);
    g_hang_fires.store(0, std::memory_order_relaxed);
    crd::jobs::set_livelock_handler(&record_livelock, nullptr);
    crd::jobs::set_hang_handler(&count_hang, nullptr); // must stay silent -- livelock is not a hang

    crd::jobs::Config cfg;
    cfg.num_threads             = 3U; // two background workers run A and B; main is thread 0
    cfg.hang_watchdog_period_ms = 20U;
    crd::jobs::init(cfg);

    crd::jobs::JobDecl a{};
    a.fn = &job_a;
    crd::jobs::JobDecl b{};
    b.fn                          = &job_b;
    crd::jobs::Counter* const ca  = crd::jobs::run(a);
    crd::jobs::Counter* const cb  = crd::jobs::run(b);

    // Wait until both tasks are live (so we have their ids), then poll for the livelock report.
    while (g_a_task.load(std::memory_order_acquire) == 0 || g_b_task.load(std::memory_order_acquire) == 0)
        std::this_thread::yield();
    const crd::u64 ida = g_a_task.load(std::memory_order_relaxed);
    const crd::u64 idb = g_b_task.load(std::memory_order_relaxed);
    for (int i = 0; i < 400 && !(ll_fired_for(ida) && ll_fired_for(idb)); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    CHECK(ll_fired_for(ida));                                     // task A reported livelocked
    CHECK(ll_fired_for(idb));                                     // task B reported livelocked
    CHECK(g_hang_fires.load(std::memory_order_relaxed) == 0);     // DISTINCT from a hang: the hang handler is silent

    // Break the livelock: both loops exit, set their flags, and complete.
    g_escape.store(true, std::memory_order_release);
    crd::jobs::wait(ca);
    crd::jobs::wait(cb);
    crd::jobs::shutdown();
    crd::jobs::set_livelock_handler(nullptr, nullptr);
    crd::jobs::set_hang_handler(nullptr, nullptr);
}

TEST_CASE("livelock watchdog: a monitored task that keeps ticking is never flagged", "[jobs][diag][livelock]")
{
    g_escape.store(false, std::memory_order_relaxed);
    g_ll_fires.store(0, std::memory_order_relaxed);
    crd::jobs::set_livelock_handler(&record_livelock, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads             = 2U;
    cfg.hang_watchdog_period_ms = 20U;
    crd::jobs::init(cfg);

    crd::jobs::JobDecl j{};
    j.fn                         = &ticking_job; // advances its epoch every iteration for ~300ms
    crd::jobs::Counter* const c  = crd::jobs::run(j);

    std::this_thread::sleep_for(std::chrono::milliseconds(350)); // ~17 windows of steady ticking
    CHECK(g_ll_fires.load(std::memory_order_relaxed) == 0);

    crd::jobs::wait(c);
    crd::jobs::shutdown();
    crd::jobs::set_livelock_handler(nullptr, nullptr);
}

TEST_CASE("livelock watchdog: an unmonitored long job is never flagged (opt-in)", "[jobs][diag][livelock]")
{
    g_escape.store(false, std::memory_order_relaxed);
    g_ll_fires.store(0, std::memory_order_relaxed);
    crd::jobs::set_livelock_handler(&record_livelock, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads             = 2U;
    cfg.hang_watchdog_period_ms = 20U;
    crd::jobs::init(cfg);

    crd::jobs::JobDecl j{};
    j.fn                         = &unmonitored_job; // spins ~300ms but never calls note_progress()
    crd::jobs::Counter* const c  = crd::jobs::run(j);

    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    CHECK(g_ll_fires.load(std::memory_order_relaxed) == 0); // epoch 0 -> unmonitored -> never a livelock

    crd::jobs::wait(c);
    crd::jobs::shutdown();
    crd::jobs::set_livelock_handler(nullptr, nullptr);
}

TEST_CASE("livelock watchdog: a monitored task that parks is not flagged while parked", "[jobs][diag][livelock]")
{
    g_escape.store(false, std::memory_order_relaxed);
    g_gate.store(false, std::memory_order_relaxed);
    g_ll_fires.store(0, std::memory_order_relaxed);
    crd::jobs::set_livelock_handler(&record_livelock, nullptr);

    crd::jobs::Config cfg;
    cfg.num_threads             = 3U;
    cfg.hang_watchdog_period_ms = 20U;
    crd::jobs::init(cfg);

    crd::jobs::JobDecl root{};
    root.fn                       = &parking_monitored_root; // opts in, then parks on a gated child
    crd::jobs::Counter* const rc  = crd::jobs::run(root);

    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // parked the whole time (gate closed)
    CHECK(g_ll_fires.load(std::memory_order_relaxed) == 0);      // parked monitored task is excluded, not flagged

    g_gate.store(true, std::memory_order_release); // let the child finish -> root resumes and completes
    crd::jobs::wait(rc);
    crd::jobs::shutdown();
    crd::jobs::set_livelock_handler(nullptr, nullptr);
}
