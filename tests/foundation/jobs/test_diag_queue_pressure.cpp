// DIAG.10c queue pressure and exhausted budgets at the crd-jobs boundary.
//
// The pinned lane of a thread was a single slot: a second job pinned to a thread before that thread drained the
// first found it occupied, which asserted where asserts are on and silently overwrote (lost) the first job
// elsewhere, so its wait never returned. Concurrent pinned pushes also raced on the plain slot. These cases hold
// the lane to a bounded queue.
//
// crd::jobs::pressure_snapshot() reports every bounded resource of the pool as a crd-core BudgetReading. The cases
// hold a known amount of work (a chain of jobs, each parked on the next, the last spinning on a flag; or jobs left
// queued on a one-thread pool that never pumps) and check the readings against the test's own count, then judge
// windows with the pressure probe: Near and Saturated from the high water, Relaxed only when the high water proves
// it, and never Relaxed for an injection queue, whose high water is not kept. A full queue or lane is a fatal, so
// its refusal is shown by the queue-full specimen in a child process.

#include <crd/core/budget_pressure.hpp>
#include <crd/diag/specimen_runner.hpp>
#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <span>
#include <thread>

namespace
{
struct PinnedTally
{
    std::atomic<crd::u32> ran{0U};
    std::atomic<crd::u32> off_thread{0U};
};

void pinned_body(void* data) noexcept
{
    auto* const t = static_cast<PinnedTally*>(data);
    if (crd::jobs::worker_index() != 0U)
    {
        t->off_thread.fetch_add(1U, std::memory_order_relaxed);
    }
    t->ran.fetch_add(1U, std::memory_order_relaxed);
}

crd::jobs::JobDecl pinned_job(PinnedTally* tally)
{
    crd::jobs::JobDecl j{};
    j.fn         = &pinned_body;
    j.data       = tally;
    j.pin_thread = 0;
    j.priority   = crd::jobs::Priority::Normal;
    return j;
}

struct FanOut
{
    PinnedTally* tally   = nullptr;
    crd::u32     per_job = 0U;
};

void fan_out_body(void* data) noexcept
{
    auto* const f = static_cast<FanOut*>(data);
    for (crd::u32 i = 0U; i < f->per_job; ++i)
    {
        const crd::jobs::JobDecl  j = pinned_job(f->tally);
        crd::jobs::Counter* const c = crd::jobs::run(j);
        crd::jobs::wait(c);
    }
}

// A chain of `depth` jobs: link i submits link i + 1 and waits on it, and the last spins until released. While the
// last spins, the chain holds exactly `depth` counters (one per run(), the first submitted by the test) and `depth`
// small fibers (the parked links and the spinning one).
struct Chain;
struct Link
{
    Chain*   chain = nullptr;
    crd::u32 index = 0U;
};
struct Chain
{
    std::atomic<bool> release{false};
    crd::u32          depth = 0U;
    Link              links[16];
};

void link_body(void* data) noexcept
{
    const auto* const l = static_cast<const Link*>(data);
    Chain* const      c = l->chain;
    if (l->index + 1U < c->depth)
    {
        crd::jobs::JobDecl j{};
        j.fn                        = &link_body;
        j.data                      = &c->links[l->index + 1U];
        crd::jobs::Counter* const k = crd::jobs::run(j);
        crd::jobs::wait(k);
        return;
    }
    while (!c->release.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
}

// Poll `done` for up to ten seconds; false if it never held.
template <typename Pred>
bool eventually(const Pred& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done())
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// Run a chain of `depth` jobs; `held` sees the pressure while the whole chain is held.
template <typename Held>
void run_chain(crd::u32 depth, const Held& held)
{
    Chain chain;
    chain.depth = depth;
    for (crd::u32 i = 0U; i < depth; ++i)
    {
        chain.links[i].chain = &chain;
        chain.links[i].index = i;
    }
    crd::jobs::JobDecl first{};
    first.fn                    = &link_body;
    first.data                  = &chain.links[0];
    crd::jobs::Counter* const c = crd::jobs::run(first);
    const bool all_held = eventually([depth] { return crd::jobs::pressure_snapshot().fibers[0].in_use == depth; });
    CHECK(all_held);
    held(crd::jobs::pressure_snapshot());
    chain.release.store(true, std::memory_order_release);
    crd::jobs::wait(c);
    // The last link's fiber goes back to the pool just after its counter reaches zero.
    const bool all_back = eventually([] { return crd::jobs::pressure_snapshot().fibers[0].in_use == 0U; });
    CHECK(all_back);
}

crd::jobs::Config small_pool()
{
    crd::jobs::Config cfg;
    cfg.num_threads       = 2U; // one worker runs the chain; the test thread watches
    cfg.max_counters      = 10U;
    cfg.small_fiber_count = 10U;
    return cfg;
}
} // namespace

TEST_CASE("jobs diag10c: several jobs pinned to thread 0 in one submission all run there", "[jobs][diag][diag10c]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    PinnedTally        tally;
    crd::jobs::JobDecl jobs[4] = {pinned_job(&tally), pinned_job(&tally), pinned_job(&tally), pinned_job(&tally)};
    crd::jobs::run_and_wait(std::span<const crd::jobs::JobDecl>(jobs, 4U)); // wait() pumps thread 0

    CHECK(tally.ran.load() == 4U);
    CHECK(tally.off_thread.load() == 0U);

    crd::jobs::shutdown();
}

TEST_CASE("jobs diag10c: concurrent pinned pushes from workers all run on thread 0", "[jobs][diag][diag10c]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4U;
    crd::jobs::init(cfg);

    PinnedTally        tally;
    FanOut             fan{&tally, 16U};
    crd::jobs::JobDecl producers[6] = {};
    for (crd::jobs::JobDecl& p : producers)
    {
        p.fn   = &fan_out_body;
        p.data = &fan;
    }
    // Six producers on the workers each push sixteen jobs pinned to thread 0 and wait for each; the main thread's
    // wait pumps them. Up to six pinned jobs are queued at once.
    crd::jobs::run_and_wait(std::span<const crd::jobs::JobDecl>(producers, 6U));

    CHECK(tally.ran.load() == 96U);
    CHECK(tally.off_thread.load() == 0U);

    crd::jobs::shutdown();
}

TEST_CASE("jobs diag10c: concurrent pinned pushes leave the pinned reading within its lane", "[jobs][diag][diag10c]")
{
    crd::jobs::Config cfg;
    cfg.num_threads           = 4U;
    cfg.pinned_queue_capacity = 8U;
    crd::jobs::init(cfg);
    const crd::BudgetReading before = crd::jobs::pressure_snapshot().pinned;

    PinnedTally        tally;
    FanOut             fan{&tally, 8U};
    crd::jobs::JobDecl producers[3] = {};
    for (crd::jobs::JobDecl& p : producers)
    {
        p.fn   = &fan_out_body;
        p.data = &fan;
    }
    crd::jobs::run_and_wait(std::span<const crd::jobs::JobDecl>(producers, 3U));
    const crd::BudgetReading after = crd::jobs::pressure_snapshot().pinned;

    CHECK(tally.ran.load() == 24U);
    CHECK(after.read);
    CHECK(after.capacity == 8U);
    CHECK(after.in_use == 0U);
    CHECK(after.refusals == 0U);
    // Each producer has at most one pinned job queued at a time, so the lane never held more than three.
    CHECK(after.high_water >= 1U);
    CHECK(after.high_water <= 3U);
    const crd::PressureReport rep = crd::pressure_report(before, after);
    CHECK(rep.verdict == crd::PressureVerdict::Relaxed);
    CHECK(rep.peak_at_most == after.high_water);

    crd::jobs::shutdown();
}

TEST_CASE("jobs diag10c: the pressure readings are unread before init and empty after it", "[jobs][diag][diag10c]")
{
    const crd::jobs::JobsPressure none = crd::jobs::pressure_snapshot();
    CHECK_FALSE(none.counters.read);
    CHECK_FALSE(none.fibers[0].read);
    CHECK_FALSE(none.injection[1].read);
    CHECK_FALSE(none.pinned.read);
    CHECK(crd::pressure_level(none.counters, 0U) == crd::PressureLevel::Unread);

    crd::jobs::Config cfg        = small_pool();
    cfg.medium_fiber_count       = 3U;
    cfg.large_fiber_count        = 2U;
    cfg.injection_queue_capacity = 64U;
    cfg.pinned_queue_capacity    = 4U;
    crd::jobs::init(cfg);
    const crd::jobs::JobsPressure p = crd::jobs::pressure_snapshot();

    CHECK(p.counters.capacity == 10U);
    CHECK(p.fibers[0].capacity == 10U);
    CHECK(p.fibers[1].capacity == 3U);
    CHECK(p.fibers[2].capacity == 2U);
    CHECK(p.pinned.capacity == 4U);
    const crd::BudgetReading* const tracked[5] = {&p.counters, &p.fibers[0], &p.fibers[1], &p.fibers[2], &p.pinned};
    for (const crd::BudgetReading* r : tracked)
    {
        CHECK(r->read);
        CHECK(r->bounded);
        CHECK(r->high_water_tracked);
        CHECK(r->in_use == 0U);
        CHECK(r->high_water == 0U);
        CHECK(r->refusals == 0U);
    }
    for (const crd::BudgetReading& r : p.injection)
    {
        CHECK(r.read);
        CHECK(r.bounded);
        CHECK_FALSE(r.high_water_tracked);
        CHECK(r.capacity == 64U);
        CHECK(r.in_use == 0U);
        CHECK(r.refusals == 0U);
    }

    crd::jobs::shutdown();
}

TEST_CASE("jobs diag10c: held counters and fibers show in the readings and the probe judges near and saturated",
          "[jobs][diag][diag10c]")
{
    crd::jobs::init(small_pool());
    const crd::jobs::JobsPressure start = crd::jobs::pressure_snapshot();

    run_chain(9U,
              [](const crd::jobs::JobsPressure& held)
              {
                  CHECK(held.counters.in_use == 9U);
                  CHECK(held.counters.high_water == 9U);
                  CHECK(held.fibers[0].in_use == 9U);
                  CHECK(held.fibers[0].high_water == 9U);
                  CHECK(held.fibers[1].in_use == 0U);
                  CHECK(crd::pressure_level(held.counters, held.counters.in_use) == crd::PressureLevel::Near);
                  CHECK(crd::pressure_level(held.fibers[0], held.fibers[0].in_use) == crd::PressureLevel::Near);
              });
    const crd::jobs::JobsPressure nine = crd::jobs::pressure_snapshot();
    CHECK(nine.counters.in_use == 0U);
    CHECK(nine.counters.high_water == 9U);
    CHECK(nine.fibers[0].high_water == 9U);
    const crd::PressureReport counters9 = crd::pressure_report(start.counters, nine.counters);
    CHECK(counters9.verdict == crd::PressureVerdict::Near);
    CHECK(counters9.peak_at_least == 9U);
    CHECK(counters9.level_now == crd::PressureLevel::Within);
    CHECK(crd::pressure_report(start.fibers[0], nine.fibers[0]).verdict == crd::PressureVerdict::Near);

    run_chain(10U,
              [](const crd::jobs::JobsPressure& held)
              {
                  CHECK(held.counters.in_use == 10U);
                  CHECK(held.fibers[0].in_use == 10U);
                  CHECK(crd::pressure_level(held.counters, held.counters.in_use) == crd::PressureLevel::Full);
                  CHECK(crd::pressure_level(held.fibers[0], held.fibers[0].in_use) == crd::PressureLevel::Full);
              });
    const crd::jobs::JobsPressure ten = crd::jobs::pressure_snapshot();
    CHECK(crd::pressure_report(nine.counters, ten.counters).verdict == crd::PressureVerdict::Saturated);
    CHECK(crd::pressure_report(nine.fibers[0], ten.fibers[0]).verdict == crd::PressureVerdict::Saturated);
    CHECK(ten.counters.refusals == 0U);
    CHECK(ten.fibers[0].refusals == 0U);

    // An idle window after that: both ends are empty, but the all-time high water is the bound, so the window is
    // not proven below near.
    const crd::jobs::JobsPressure idle = crd::jobs::pressure_snapshot();
    CHECK(crd::pressure_report(ten.counters, idle.counters).verdict == crd::PressureVerdict::Undetermined);

    crd::jobs::shutdown();
}

TEST_CASE("jobs diag10c: light work is proven relaxed by the high water", "[jobs][diag][diag10c]")
{
    crd::jobs::init(small_pool());
    const crd::jobs::JobsPressure start = crd::jobs::pressure_snapshot();
    run_chain(2U, [](const crd::jobs::JobsPressure& held) { CHECK(held.counters.in_use == 2U); });
    const crd::jobs::JobsPressure after = crd::jobs::pressure_snapshot();

    const crd::PressureReport counters = crd::pressure_report(start.counters, after.counters);
    CHECK(counters.verdict == crd::PressureVerdict::Relaxed);
    CHECK(counters.peak_at_least == 2U);
    CHECK(counters.peak_at_most == 2U);
    CHECK(crd::pressure_report(start.fibers[0], after.fibers[0]).verdict == crd::PressureVerdict::Relaxed);
    // An injection queue keeps no high water: the same light window is never called relaxed there.
    const crd::PressureReport lane = crd::pressure_report(start.injection[1], after.injection[1]);
    CHECK(lane.verdict == crd::PressureVerdict::Undetermined);
    CHECK_FALSE(lane.peak_bounded);

    crd::jobs::shutdown();
}

TEST_CASE("jobs diag10c: queued jobs fill an injection queue and a pinned lane to their bounds",
          "[jobs][diag][diag10c]")
{
    crd::jobs::Config cfg;
    cfg.num_threads              = 1U; // nothing drains a queue until the test thread waits (and so pumps)
    cfg.injection_queue_capacity = 4U;
    cfg.pinned_queue_capacity    = 4U;
    crd::jobs::init(cfg);
    const crd::jobs::JobsPressure start = crd::jobs::pressure_snapshot();

    PinnedTally         tally;
    crd::jobs::Counter* normal[4] = {};
    crd::jobs::Counter* pinned[4] = {};
    for (crd::u32 i = 0U; i < 4U; ++i)
    {
        crd::jobs::JobDecl j{};
        j.fn      = &pinned_body;
        j.data    = &tally;
        normal[i] = crd::jobs::run(j);
        pinned[i] = crd::jobs::run(pinned_job(&tally));
    }
    const crd::jobs::JobsPressure full = crd::jobs::pressure_snapshot();
    CHECK(full.injection[1].in_use == 4U);
    CHECK(full.injection[0].in_use == 0U);
    CHECK(full.pinned.in_use == 4U);
    CHECK(full.pinned.high_water == 4U);
    CHECK(full.counters.in_use == 8U);
    CHECK(crd::pressure_level(full.injection[1], full.injection[1].in_use) == crd::PressureLevel::Full);
    CHECK(crd::pressure_level(full.pinned, full.pinned.in_use) == crd::PressureLevel::Full);
    CHECK(crd::pressure_report(start.injection[1], full.injection[1]).verdict == crd::PressureVerdict::Saturated);

    for (crd::u32 i = 0U; i < 4U; ++i)
    {
        crd::jobs::wait(normal[i]);
        crd::jobs::wait(pinned[i]);
    }
    const crd::jobs::JobsPressure drained = crd::jobs::pressure_snapshot();
    CHECK(tally.ran.load() == 8U);
    CHECK(drained.injection[1].in_use == 0U);
    CHECK(drained.pinned.in_use == 0U);
    CHECK(drained.pinned.high_water == 4U);
    CHECK(drained.injection[1].refusals == 0U);
    CHECK(drained.pinned.refusals == 0U);
    // The pinned lane keeps its high water, so a window over the whole run is saturated after draining.
    CHECK(crd::pressure_report(start.pinned, drained.pinned).verdict == crd::PressureVerdict::Saturated);

    crd::jobs::shutdown();
}

TEST_CASE("jobs diag10c: a push into a full injection queue or pinned lane is a counted fatal",
          "[jobs][diag][diag10c][harness]")
{
    namespace cd   = crd::diag;
    namespace cont = crd::containers;
    const char* const modes[2]    = {"injection", "pinned"};
    const char* const expected[2] = {
        "CRD_DIAG_PRESSURE=injection.normal: exhausted (in use 2/2, peak 2+, refused +1)",
        "CRD_DIAG_PRESSURE=pinned: exhausted (in use 2/2, peak 2, refused +1)"};
    for (crd::u32 i = 0U; i < 2U; ++i)
    {
        cd::Expectation e;
        e.want       = cd::Expectation::Want::CleanExit; // the exit code is the oracle here, not the verdict
        e.timeout_ms = 5000U;
        cont::Array<cont::String> args;
        args.push_back(cont::String{modes[i]});
        const cd::Outcome o = cd::run_specimen(cont::String{CRD_DIAG_QUEUE_FULL_SPECIMEN}, args, e);
        INFO("mode=" << modes[i] << " verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code
                     << " output=" << o.output.c_str());
        CHECK(cont::StringView{o.identity} == cont::StringView{"crd-diag-queue-full-specimen"});
        // 42 = the fatal fired and the reading shows the refusal at the bound; 60 = it fired without that; 97 = a
        // different assert; 0 = no fatal (the job was dropped); a timeout = the push blocked or hung.
        CHECK(o.exit_code == 42);
        CHECK(std::strstr(o.output.c_str(), expected[i]) != nullptr);
    }
}
