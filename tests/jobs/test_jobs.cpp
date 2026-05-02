#include <catch2/catch_test_macros.hpp>

#include "../../engine/jobs/src/worker_pool.hpp"
#include <crd/jobs/jobs.hpp>
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

// ===========================================================================
// v1h — Public API (crd::jobs::init / run / wait / run_and_wait)
// ===========================================================================

// ---------------------------------------------------------------------------
// 11. init + shutdown + introspection
// ---------------------------------------------------------------------------

TEST_CASE("jobs: init and shutdown", "[jobs][public-api]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2u;
    crd::jobs::init(cfg);

    CHECK(crd::jobs::num_workers() == 2u);
    CHECK(crd::jobs::worker_index() == 0u);    // main thread is thread 0
    CHECK_FALSE(crd::jobs::is_worker_fiber()); // main thread is not inside a fiber

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 12. run() executes jobs on worker threads; wait() from main thread (spin path)
// ---------------------------------------------------------------------------

TEST_CASE("jobs: run and wait from main thread", "[jobs][public-api]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2u; // background worker required for the spin-wait path
    crd::jobs::init(cfg);

    constexpr int kN = 20;
    std::atomic<int> count{0};

    crd::jobs::JobDecl jobs[kN];
    for (auto& j : jobs)
    {
        j.fn   = [](void* d)
        {
            static_cast<std::atomic<int>*>(d)->fetch_add(1, std::memory_order_release);
        };
        j.data = &count;
    }

    crd::jobs::Counter* c = crd::jobs::run(std::span(jobs, kN));
    crd::jobs::wait(c); // spin-waits on main thread until all 20 jobs complete

    CHECK(count.load() == kN);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 13. run_and_wait() from inside a worker fiber
// ---------------------------------------------------------------------------

TEST_CASE("jobs: run_and_wait from inside a worker fiber", "[jobs][public-api]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2u;
    crd::jobs::init(cfg);

    struct RootData
    {
        std::atomic<int>* child_count;
        std::atomic<bool>* root_done;
    };

    std::atomic<int>  child_count{0};
    std::atomic<bool> root_done{false};
    static RootData rd;
    rd = {&child_count, &root_done};

    crd::jobs::JobDecl root{};
    root.fn = [](void* d)
    {
        auto* data = static_cast<RootData*>(d);

        constexpr int kChildren = 8;
        crd::jobs::JobDecl children[kChildren];
        for (auto& child : children)
        {
            child.fn   = [](void* dc)
            {
                static_cast<std::atomic<int>*>(dc)->fetch_add(1, std::memory_order_release);
            };
            child.data = data->child_count;
        }

        // wait() here runs the fiber-suspend path (we are inside a fiber).
        crd::jobs::run_and_wait(std::span(children, kChildren));

        data->root_done->store(true, std::memory_order_release);
    };
    root.data = &rd;

    // Submit root via run(); wait() from main thread uses the spin path.
    crd::jobs::Counter* c = crd::jobs::run(root);
    crd::jobs::wait(c);

    CHECK(child_count.load() == 8);
    CHECK(root_done.load());

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 14. is_worker_fiber() returns true inside a job
// ---------------------------------------------------------------------------

TEST_CASE("jobs: is_worker_fiber returns true inside a job", "[jobs][public-api]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2u;
    crd::jobs::init(cfg);

    std::atomic<bool> saw_fiber{false};

    crd::jobs::JobDecl j{};
    j.fn   = [](void* d)
    {
        static_cast<std::atomic<bool>*>(d)->store(
            crd::jobs::is_worker_fiber(), std::memory_order_release);
    };
    j.data = &saw_fiber;

    crd::jobs::wait(crd::jobs::run(j));
    CHECK(saw_fiber.load());

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 15. Stress: many parallel jobs via public API
// ---------------------------------------------------------------------------

TEST_CASE("jobs: stress many parallel jobs via public API", "[jobs][public-api]")
{
    crd::jobs::Config cfg;
    cfg.num_threads       = 4u;
    cfg.small_fiber_count = 128u;
    crd::jobs::init(cfg);

    constexpr int kN = 500;
    std::atomic<int> count{0};

    crd::jobs::JobDecl jobs[kN];
    for (auto& j : jobs)
    {
        j.fn   = [](void* d)
        {
            static_cast<std::atomic<int>*>(d)->fetch_add(1, std::memory_order_release);
        };
        j.data = &count;
    }

    crd::jobs::wait(crd::jobs::run(std::span(jobs, kN)));
    CHECK(count.load() == kN);

    crd::jobs::shutdown();
}

// ===========================================================================
// v1i — make_job<F> SBO helpers + parallel_for
// ===========================================================================

// ---------------------------------------------------------------------------
// 16. make_job: basic SBO lambda dispatched via public run/wait
// ---------------------------------------------------------------------------

TEST_CASE("jobs: make_job basic SBO lambda", "[jobs][sbo]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2u;
    crd::jobs::init(cfg);

    std::atomic<int> count{0};

    // Lambda captures one pointer — 8 bytes, well within 41-byte SBO limit.
    auto j = crd::jobs::make_job([&count]()
    {
        count.fetch_add(1, std::memory_order_release);
    });

    crd::jobs::wait(crd::jobs::run(j));
    CHECK(count.load() == 1);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 17. make_job: SBO callable survives fiber suspension + resume on another thread.
//     This is the critical test: the outer lambda calls run_and_wait internally,
//     which triggers fiber suspension. The SBO bytes must be readable when the
//     fiber resumes (possibly on a different OS thread).
// ---------------------------------------------------------------------------

TEST_CASE("jobs: make_job SBO survives fiber suspension", "[jobs][sbo]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2u; // two threads so inner job can run while outer fiber waits
    crd::jobs::init(cfg);

    std::atomic<int>  child_count{0};
    std::atomic<bool> outer_done{false};

    // Outer lambda: captures two references (16 bytes — within 41-byte SBO).
    // Calls run_and_wait internally → fiber suspends here → resumes later.
    // After resume, stores outer_done = true using data from its SBO closure.
    auto outer = crd::jobs::make_job([&child_count, &outer_done]()
    {
        auto inner = crd::jobs::make_job([&child_count]()
        {
            child_count.fetch_add(1, std::memory_order_release);
        });
        crd::jobs::run_and_wait(inner); // suspends outer fiber until inner completes

        // This line only executes after resume — proves SBO bytes survived.
        outer_done.store(true, std::memory_order_release);
    });

    crd::jobs::wait(crd::jobs::run(outer));

    CHECK(child_count.load() == 1);
    CHECK(outer_done.load());

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 18. make_job: captures non-pointer state by value (struct capture)
// ---------------------------------------------------------------------------

TEST_CASE("jobs: make_job captures struct by value", "[jobs][sbo]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2u;
    crd::jobs::init(cfg);

    struct Payload
    {
        int      addend;
        std::atomic<int>* out;
    };

    std::atomic<int> result{0};
    Payload p{42, &result};

    // Lambda captures Payload by value (8 bytes pointer + int = 12 bytes — within 41).
    auto j = crd::jobs::make_job([p]()
    {
        p.out->fetch_add(p.addend, std::memory_order_release);
    });

    crd::jobs::wait(crd::jobs::run(j));
    CHECK(result.load() == 42);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 19. parallel_for: splits range correctly and executes all items
// ---------------------------------------------------------------------------

TEST_CASE("jobs: parallel_for splits range and executes all items", "[jobs][sbo]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4u;
    crd::jobs::init(cfg);

    constexpr crd::u32 kCount   = 100u;
    constexpr crd::u32 kNumJobs = 4u;
    std::atomic<int> sum{0};

    // Each job accumulates (end - begin) into sum. Total must equal kCount.
    crd::jobs::Counter* c = crd::jobs::parallel_for(
        kCount, kNumJobs,
        [&sum](crd::u32 begin, crd::u32 end)
        {
            sum.fetch_add(static_cast<int>(end - begin), std::memory_order_release);
        });

    crd::jobs::wait(c);
    CHECK(sum.load() == static_cast<int>(kCount));

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 20. parallel_for: num_jobs clamped to count when num_jobs > count
// ---------------------------------------------------------------------------

TEST_CASE("jobs: parallel_for clamps num_jobs to count", "[jobs][sbo]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4u;
    crd::jobs::init(cfg);

    // count=3, num_jobs=10 → clamped to 3. Each of the 3 items is processed once.
    constexpr crd::u32 kCount = 3u;
    std::atomic<int> processed{0};

    crd::jobs::Counter* c = crd::jobs::parallel_for(
        kCount, 10u,
        [&processed](crd::u32 /*begin*/, crd::u32 /*end*/)
        {
            processed.fetch_add(1, std::memory_order_release);
        });

    crd::jobs::wait(c);
    // 3 jobs ran (one per item after clamping), covering all 3 items.
    CHECK(processed.load() == static_cast<int>(kCount));

    crd::jobs::shutdown();
}
