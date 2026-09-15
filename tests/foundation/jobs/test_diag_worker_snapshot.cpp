// DIAG.4c (e) cooperative worker snapshot + honest-incomplete. The safe stop/snapshot protocol for the RUNNING
// half of the pool: worker_snapshot() asks each background worker to acknowledge at its loop-top safe point and
// polls with a bound. A responsive pool completes; a worker stuck inside one job never acks, so the result is
// honestly INCOMPLETE (it never blocks forever and never walks a live changing stack) while still naming the
// task that worker is stuck on. Actual running-thread stack capture is DIAG.5a/5b's external, qualified domain.
#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

using crd::jobs::WorkerNode;
using crd::jobs::WorkerSnapshotResult;

namespace
{
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool>     g_spin_gate{false};
std::atomic<crd::u64> g_spin_task_id{0};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// Records its own task id, then spins (keeping its worker executing and away from the loop-top safe point)
// until released -- the seeded "unresponsive worker".
void spin_job(void* /*data*/) noexcept
{
    g_spin_task_id.store(crd::jobs::current_task_id(), std::memory_order_release);
    while (!g_spin_gate.load(std::memory_order_acquire))
        std::this_thread::yield();
}
} // namespace

TEST_CASE("worker snapshot: an idle pool is fully responsive and complete", "[jobs][diag][worker-snapshot]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 3U; // thread 0 (pump) + two background workers
    crd::jobs::init(cfg);

    std::array<WorkerNode, 8> nodes{};
    const WorkerSnapshotResult res = crd::jobs::worker_snapshot(nodes, 200U);

    CHECK(res.total == 3U);      // three worker slots reported
    CHECK(res.expected == 2U);   // two background workers polled
    CHECK(res.responded == 2U);  // both acked
    CHECK(res.complete);
    for (crd::u32 i = 0U; i < 3U; ++i)
    {
        CHECK(nodes[i].responsive);
        CHECK(nodes[i].current_task_id == 0U); // nothing running
        CHECK_FALSE(nodes[i].executing);
    }

    crd::jobs::shutdown();
}

TEST_CASE("worker snapshot: a worker stuck in one job is honestly incomplete and names the task",
          "[jobs][diag][worker-snapshot]")
{
    g_spin_gate.store(false, std::memory_order_relaxed);
    g_spin_task_id.store(0, std::memory_order_relaxed);

    crd::jobs::Config cfg;
    cfg.num_threads = 3U;
    crd::jobs::init(cfg);

    crd::jobs::JobDecl spin{};
    spin.fn                      = &spin_job;
    crd::jobs::Counter* const sc = crd::jobs::run(spin);

    while (g_spin_task_id.load(std::memory_order_acquire) == 0U) // wait until the spin job is actually running
        std::this_thread::yield();
    const crd::u64 spin_id = g_spin_task_id.load(std::memory_order_relaxed);

    std::array<WorkerNode, 8> nodes{};
    const WorkerSnapshotResult res = crd::jobs::worker_snapshot(nodes, 150U);

    CHECK(res.total == 3U);
    CHECK(res.expected == 2U);
    CHECK(res.responded == 1U);   // the free worker acked; the stuck one did not
    CHECK_FALSE(res.complete);    // honest incomplete

    bool found_stuck = false;
    for (crd::u32 i = 1U; i < 3U; ++i) // background workers only
    {
        if (nodes[i].executing)
        {
            CHECK_FALSE(nodes[i].responsive);                 // stuck in one job -> never reached its safe point
            CHECK(nodes[i].current_task_id == spin_id);       // the dump still names WHAT is stuck
            found_stuck = true;
        }
        else
        {
            CHECK(nodes[i].responsive); // the other background worker is idle and acked
        }
    }
    CHECK(found_stuck);

    g_spin_gate.store(true, std::memory_order_release); // release the spinner
    crd::jobs::wait(sc);
    crd::jobs::shutdown();
}

TEST_CASE("worker snapshot: the attempt is time-bounded, not a hang", "[jobs][diag][worker-snapshot]")
{
    g_spin_gate.store(false, std::memory_order_relaxed);
    g_spin_task_id.store(0, std::memory_order_relaxed);

    crd::jobs::Config cfg;
    cfg.num_threads = 2U; // thread 0 + one background worker, which we will stick
    crd::jobs::init(cfg);

    crd::jobs::JobDecl spin{};
    spin.fn                      = &spin_job;
    crd::jobs::Counter* const sc = crd::jobs::run(spin);
    while (g_spin_task_id.load(std::memory_order_acquire) == 0U)
        std::this_thread::yield();

    std::array<WorkerNode, 8> nodes{};
    const auto                 t0  = std::chrono::steady_clock::now();
    const WorkerSnapshotResult res = crd::jobs::worker_snapshot(nodes, 100U);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    CHECK_FALSE(res.complete);        // the single background worker is stuck
    CHECK(res.expected == 1U);
    CHECK(res.responded == 0U);
    CHECK(elapsed_ms < 1000);         // bounded -- it returned rather than hanging
    CHECK(elapsed_ms >= 50);          // and it did poll roughly to the ~100ms deadline

    g_spin_gate.store(true, std::memory_order_release);
    crd::jobs::wait(sc);
    crd::jobs::shutdown();
}

TEST_CASE("worker snapshot: truncation is honest (total exceeds the buffer)", "[jobs][diag][worker-snapshot]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4U; // thread 0 + three background workers
    crd::jobs::init(cfg);

    std::array<WorkerNode, 2> small{}; // smaller than the worker count
    const WorkerSnapshotResult res = crd::jobs::worker_snapshot(small, 200U);

    CHECK(res.total == 4U);      // reports the true worker count even though only 2 nodes were written
    CHECK(res.expected == 3U);   // three background workers
    CHECK(res.responded == 3U);
    CHECK(res.complete);

    crd::jobs::shutdown();
}
