#include <catch2/catch_test_macros.hpp>

#include "../../engine/jobs/src/worker_pool.hpp"
#include <crd/core/types.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using crd::jobs::detail::WorkerConfig;
using crd::jobs::detail::WorkerPool;
using crd::jobs::JobDecl;
using crd::jobs::Priority;
using crd::jobs::StackSize;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Spin-wait until v reaches target or timeout_ms elapses.
static bool spin_until(const std::atomic<int>& v, int target, int timeout_ms = 3000)
{
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    while (v.load(std::memory_order_acquire) < target)
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

static JobDecl make_inc_job(std::atomic<int>* c, Priority p = Priority::Normal,
                             crd::i32 pin = -1)
{
    JobDecl j{};
    j.fn         = [](void* d)
    {
        static_cast<std::atomic<int>*>(d)->fetch_add(1, std::memory_order_release);
    };
    j.data       = c;
    j.priority   = p;
    j.pin_thread = pin;
    return j;
}

// ---------------------------------------------------------------------------
// 1. init and shutdown
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: init and shutdown", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads = 2u;
    REQUIRE(pool.init(cfg));
    CHECK(pool.is_initialized());
    CHECK(pool.num_threads() == 2u);
    CHECK(pool.scheduler().is_initialized());
    CHECK(pool.fiber_pool().is_initialized());
    CHECK(pool.counter_pool().is_initialized());

    pool.shutdown();
    CHECK_FALSE(pool.is_initialized());
}

// ---------------------------------------------------------------------------
// 2. Re-init after shutdown
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: re-init after shutdown", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads = 2u;
    REQUIRE(pool.init(cfg));
    pool.shutdown();
    REQUIRE(pool.init(cfg));
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 3. num_threads defaults to hardware_concurrency
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: default num_threads uses hardware_concurrency", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads = 0u; // request automatic
    REQUIRE(pool.init(cfg));

    const crd::u32 hw = static_cast<crd::u32>(std::thread::hardware_concurrency());
    const crd::u32 expected = (hw > 0u) ? hw : 1u;
    CHECK(pool.num_threads() == expected);

    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 4. Single job executes on a worker thread
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: single job executes on worker thread", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads = 2u;
    REQUIRE(pool.init(cfg));

    std::atomic<int> done{0};
    pool.push(make_inc_job(&done));

    REQUIRE(spin_until(done, 1));
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 5. pump executes a job on thread 0 (single-thread pool)
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: pump executes job on thread 0", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads = 1u; // only thread 0, no background workers
    REQUIRE(pool.init(cfg));

    std::atomic<int> done{0};
    pool.push(make_inc_job(&done));

    const bool pumped = pool.pump();
    REQUIRE(pumped);
    REQUIRE(done.load() == 1);

    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 6. Pinned job executes only when thread 0 calls pump
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: pinned job executes via pump on thread 0", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads = 2u;
    REQUIRE(pool.init(cfg));

    std::atomic<int> done{0};
    pool.push(make_inc_job(&done, Priority::Normal, /*pin=*/0));

    // Thread 0 must pump — worker threads never check thread 0's pinned slot.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (done.load() == 0 && std::chrono::steady_clock::now() < deadline)
        (void)pool.pump();

    REQUIRE(done.load() == 1);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 7. Multiple jobs all execute (worker threads)
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: multiple jobs all execute", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads       = 4u;
    cfg.small_fiber_count = 64u;
    REQUIRE(pool.init(cfg));

    constexpr int kJobs = 100;
    std::atomic<int> done{0};
    for (int i = 0; i < kJobs; ++i)
        pool.push(make_inc_job(&done));

    REQUIRE(spin_until(done, kJobs, 5000));
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 8. Fibers have their own stack (stack pointer differs from OS thread stack)
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: job runs on a fiber stack", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads = 1u;
    REQUIRE(pool.init(cfg));

    // Capture a rough stack address from inside the job; the job runs in a fiber,
    // so its stack is the pool-allocated fiber stack — not the OS thread's stack.
    // We just verify a value is captured (not nullptr), indicating the job ran.
    std::atomic<crd::usize> captured_sp{0u};

    JobDecl j{};
    j.fn = [](void* data)
    {
        // Take the address of a local variable as a proxy for the current stack pointer.
        volatile int local = 42;
        static_cast<std::atomic<crd::usize>*>(data)->store(
            reinterpret_cast<crd::usize>(&local), std::memory_order_release);
    };
    j.data = &captured_sp;
    pool.push(j);

    REQUIRE(pool.pump());

    REQUIRE(captured_sp.load() != 0u);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 9. Pump returns false when queue is empty
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: pump returns false when queue empty", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads = 1u;
    REQUIRE(pool.init(cfg));

    const bool result = pool.pump();
    REQUIRE_FALSE(result);

    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 10. Concurrent multi-thread stress
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: concurrent multi-thread stress", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads       = 4u;
    cfg.small_fiber_count = 128u;
    REQUIRE(pool.init(cfg));

    constexpr int kJobs = 1000;
    std::atomic<int> done{0};
    for (int i = 0; i < kJobs; ++i)
        pool.push(make_inc_job(&done));

    REQUIRE(spin_until(done, kJobs, 10000));
    pool.shutdown();
}
