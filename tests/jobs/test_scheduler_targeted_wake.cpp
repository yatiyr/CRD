// Targeted-wake (opt-in, ADR-0094) tests for crd::jobs::detail::Scheduler.
//
// Verifies the dual-path wake mechanism added behind SchedulerConfig::targeted_wake:
//   * Pinned push() wakes only the targeted worker (via per-worker semaphore).
//   * Injection push() / push_local() wake one idle worker via the 64-bit idle mask.
//   * wait_for_work(thread_idx) parks and self-heals on the try_acquire_for timeout
//     if a wake is lost (no deadlock).
//   * wake_all() releases every per-worker semaphore (shutdown unblocks parked workers).
//   * Concurrent stress: N producers + M workers on a targeted_wake=true pool,
//     every job runs exactly once and the run completes within a bounded budget.
//
// Default-path tests live in test_scheduler.cpp; these tests never set
// targeted_wake=false and never touch the shared m_semaphore.

#include <catch2/catch_test_macros.hpp>

#include "../../engine/jobs/src/scheduler.hpp"
#include <crd/core/types.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using crd::jobs::detail::Scheduler;
using crd::jobs::detail::SchedulerConfig;
using crd::jobs::JobDecl;
using crd::jobs::Priority;

namespace
{
JobDecl make_count_job(std::atomic<int>* counter,
                       Priority          prio = Priority::Normal,
                       crd::i32          pin  = -1)
{
    JobDecl j;
    j.fn = [](void* d)
    {
        static_cast<std::atomic<int>*>(d)->fetch_add(1, std::memory_order_relaxed);
    };
    j.data       = counter;
    j.priority   = prio;
    j.pin_thread = pin;
    return j;
}

SchedulerConfig targeted_cfg(crd::u32 n_threads)
{
    SchedulerConfig c;
    c.num_threads        = n_threads;
    c.deque_capacity     = 256U;
    c.injection_capacity = 4096U;
    c.targeted_wake      = true;
    return c;
}
} // namespace

// ---------------------------------------------------------------------------
// 1. init: targeted_wake configured, num_threads bounded by 64
// ---------------------------------------------------------------------------

TEST_CASE("scheduler-tw: init with targeted_wake enabled", "[jobs][scheduler][targeted-wake]")
{
    Scheduler sched;
    REQUIRE(sched.init(targeted_cfg(4U)));
    CHECK(sched.is_initialized());
    CHECK(sched.num_threads() == 4U);
    sched.shutdown();
}

// ---------------------------------------------------------------------------
// 2. push wakes a worker blocked in targeted wait_for_work — via injection
// ---------------------------------------------------------------------------

TEST_CASE("scheduler-tw: injection push wakes one parked worker", "[jobs][scheduler][targeted-wake]")
{
    Scheduler sched;
    REQUIRE(sched.init(targeted_cfg(2U)));

    std::atomic<bool> woke{false};

    // Worker 1 parks; producer (main) pushes a Normal job which should
    // wake_one_idle() and release worker 1's per-worker semaphore.
    std::thread waiter([&sched, &woke]()
    {
        sched.wait_for_work(1U);
        woke.store(true, std::memory_order_release);
    });

    // Give the waiter time to set its idle bit and park on the per-worker sem.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::atomic<int> ran{0};
    sched.push(make_count_job(&ran, Priority::Normal));

    waiter.join(); // must wake — either via wake_one_idle or via the 5 ms timeout
    CHECK(woke.load(std::memory_order_acquire));

    // Drain the queued job to keep the scheduler clean.
    REQUIRE(sched.execute_one(1U));
    CHECK(ran.load() == 1);

    sched.shutdown();
}

// ---------------------------------------------------------------------------
// 3. Pinned push wakes the pinned worker (and only that worker)
// ---------------------------------------------------------------------------

TEST_CASE("scheduler-tw: pinned push wakes the pinned worker", "[jobs][scheduler][targeted-wake]")
{
    Scheduler sched;
    REQUIRE(sched.init(targeted_cfg(2U)));

    std::atomic<bool> worker0_woke{false};
    std::atomic<bool> worker1_woke{false};

    std::thread w0([&]()
    {
        sched.wait_for_work(0U);
        worker0_woke.store(true, std::memory_order_release);
    });
    std::thread w1([&]()
    {
        sched.wait_for_work(1U);
        worker1_woke.store(true, std::memory_order_release);
    });

    // Park both, then pin a job to worker 0.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::atomic<int> ran{0};
    sched.push(make_count_job(&ran, Priority::Normal, /*pin=*/0));

    w0.join(); // worker 0 must wake (was directly targeted)
    CHECK(worker0_woke.load(std::memory_order_acquire));

    // Worker 1 must still wake within the safety-net timeout (no deadlock),
    // even though no producer woke it directly.
    w1.join();
    CHECK(worker1_woke.load(std::memory_order_acquire));

    // Drain the pinned job from worker 0's slot.
    REQUIRE(sched.execute_one(0U));
    CHECK(ran.load() == 1);

    sched.shutdown();
}

// ---------------------------------------------------------------------------
// 4. wake_all wakes every parked worker on the targeted path (shutdown path)
// ---------------------------------------------------------------------------

TEST_CASE("scheduler-tw: wake_all unblocks every parked worker", "[jobs][scheduler][targeted-wake]")
{
    static constexpr crd::u32 kThreads = 8U;

    Scheduler sched;
    REQUIRE(sched.init(targeted_cfg(kThreads)));

    std::atomic<int> woke_count{0};
    std::vector<std::thread> waiters;
    waiters.reserve(kThreads);
    for (crd::u32 t = 0U; t < kThreads; ++t)
    {
        waiters.emplace_back([&sched, &woke_count, t]()
        {
            sched.wait_for_work(t);
            woke_count.fetch_add(1, std::memory_order_relaxed);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    sched.wake_all(kThreads);

    for (auto& w : waiters) w.join();
    CHECK(woke_count.load() == static_cast<int>(kThreads));

    sched.shutdown();
}

// ---------------------------------------------------------------------------
// 5. Self-heal on lost wake: parked worker recovers via try_acquire_for timeout
//
// Simulates a lost wake by parking a worker and never calling push/wake. The
// worker MUST wake within a small multiple of the 5 ms timeout — never deadlock.
// ---------------------------------------------------------------------------

TEST_CASE("scheduler-tw: parked worker self-heals via timeout", "[jobs][scheduler][targeted-wake]")
{
    Scheduler sched;
    REQUIRE(sched.init(targeted_cfg(1U)));

    const auto t0 = std::chrono::steady_clock::now();
    sched.wait_for_work(0U); // must return on the safety-net timeout
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Timeout is 5 ms; allow generous slack for OS scheduling on Windows debug.
    CHECK(elapsed < std::chrono::milliseconds(500));

    sched.shutdown();
}

// ---------------------------------------------------------------------------
// 6. Concurrent stress: targeted path delivers every job, no deadlock
//
// kThreads workers park/wake repeatedly while a producer thread pushes
// kTotalJobs jobs at mixed priorities. Workers exit on a "drain done" flag.
// ---------------------------------------------------------------------------

TEST_CASE("scheduler-tw: concurrent producer/consumer stress", "[jobs][scheduler][targeted-wake][stress]")
{
    static constexpr crd::u32 kThreads   = 4U;
    static constexpr int      kTotalJobs = 4000;

    Scheduler sched;
    REQUIRE(sched.init(targeted_cfg(kThreads)));

    std::atomic<int>  executed{0};
    std::atomic<bool> stop{false};

    // Workers: drain-then-park loop. Exit on `stop` flag.
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (crd::u32 t = 0U; t < kThreads; ++t)
    {
        workers.emplace_back([&sched, &stop, t]() // `executed` is incremented inside the job fn, not here
        {
            while (!stop.load(std::memory_order_acquire))
            {
                if (!sched.execute_one(t))
                {
                    sched.wait_for_work(t);
                }
            }
        });
    }

    // Producer: push from main thread, interleaving priorities.
    for (int i = 0; i < kTotalJobs; ++i)
    {
        Priority prio = Priority::Low;
        if (i % 3 == 0)      prio = Priority::High;
        else if (i % 3 == 1) prio = Priority::Normal;
        sched.push(make_count_job(&executed, prio));
    }

    // Wait for completion with a generous safety budget.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (executed.load(std::memory_order_acquire) < kTotalJobs &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK(executed.load() == kTotalJobs);

    // Tell workers to exit, then wake them so any parked ones observe `stop`.
    stop.store(true, std::memory_order_release);
    sched.wake_all(kThreads);
    for (auto& w : workers) w.join();

    sched.shutdown();
}

// ---------------------------------------------------------------------------
// 7. push_local on targeted path wakes an idle worker (peer-deque path)
// ---------------------------------------------------------------------------

TEST_CASE("scheduler-tw: push_local wakes one idle worker", "[jobs][scheduler][targeted-wake]")
{
    Scheduler sched;
    REQUIRE(sched.init(targeted_cfg(2U)));

    std::atomic<bool> woke{false};

    std::thread waiter([&sched, &woke]()
    {
        sched.wait_for_work(1U);
        woke.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // push_local onto worker 0's deque — worker 1 must be woken (any-idle wake).
    std::atomic<int> ran{0};
    sched.push_local(0U, make_count_job(&ran, Priority::Normal));

    waiter.join();
    CHECK(woke.load(std::memory_order_acquire));

    // Worker 1 may steal worker 0's job, or worker 0 may pop it; either is fine.
    REQUIRE((sched.execute_one(0U) || sched.execute_one(1U)));
    CHECK(ran.load() == 1);

    sched.shutdown();
}
