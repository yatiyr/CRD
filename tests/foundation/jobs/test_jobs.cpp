#include <catch2/catch_test_macros.hpp>

#include "../../../engine/foundation/jobs/src/worker_pool.hpp"
#include <crd/jobs/jobs.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <chrono>
#include <span>
#include <thread>

// GCC and Clang AddressSanitizer builds, whose stacks have fake stacks (the fake-stack case below).
#if defined(__SANITIZE_ADDRESS__) && !defined(_MSC_VER)
#define CRD_JOBS_TESTS_FAKE_STACKS 1 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#elif defined(__has_feature) && !defined(_MSC_VER)
#if __has_feature(address_sanitizer)
#define CRD_JOBS_TESTS_FAKE_STACKS 1 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#endif
#endif

#if defined(CRD_JOBS_TESTS_FAKE_STACKS)
#include <sanitizer/asan_interface.h>

#include <algorithm>
#endif

using crd::jobs::detail::WorkerConfig;
using crd::jobs::detail::WorkerPool;
using crd::jobs::JobDecl;
using crd::jobs::Priority;

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
        {
            return false;
        }
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
    cfg.num_threads = 2U;
    REQUIRE(pool.init(cfg));
    CHECK(pool.is_initialized());
    CHECK(pool.num_threads() == 2U);
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
    cfg.num_threads = 2U;
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
    cfg.num_threads = 0U; // request automatic
    REQUIRE(pool.init(cfg));

    const crd::u32 hw = static_cast<crd::u32>(std::thread::hardware_concurrency());
    const crd::u32 expected = (hw > 0U) ? hw : 1U;
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
    cfg.num_threads = 2U;
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
    cfg.num_threads = 1U; // only thread 0, no background workers
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
    cfg.num_threads = 2U;
    REQUIRE(pool.init(cfg));

    std::atomic<int> done{0};
    pool.push(make_inc_job(&done, Priority::Normal, /*pin=*/0));

    // Thread 0 must pump — worker threads never check thread 0's pinned slot.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (done.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        (void)pool.pump();
    }

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
    cfg.num_threads       = 4U;
    cfg.small_fiber_count = 64U;
    REQUIRE(pool.init(cfg));

    constexpr int k_jobs = 100;
    std::atomic<int> done{0};
    for (int i = 0; i < k_jobs; ++i)
    {
        pool.push(make_inc_job(&done));
    }

    REQUIRE(spin_until(done, k_jobs, 5000));
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 8. Fibers have their own stack (stack pointer differs from OS thread stack)
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: job runs on a fiber stack", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads = 1U;
    REQUIRE(pool.init(cfg));

    // Capture a rough stack address from inside the job; the job runs in a fiber,
    // so its stack is the pool-allocated fiber stack — not the OS thread's stack.
    // We just verify a value is captured (not nullptr), indicating the job ran.
    std::atomic<crd::usize> captured_sp{0U};

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

    REQUIRE(captured_sp.load() != 0U);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 9. Pump returns false when queue is empty
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: pump returns false when queue empty", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads = 1U;
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
    cfg.num_threads       = 4U;
    cfg.small_fiber_count = 128U;
    REQUIRE(pool.init(cfg));

    constexpr int k_jobs = 1000;
    std::atomic<int> done{0};
    for (int i = 0; i < k_jobs; ++i)
    {
        pool.push(make_inc_job(&done));
    }

    REQUIRE(spin_until(done, k_jobs, 10000));
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
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    CHECK(crd::jobs::num_workers() == 2U);
    CHECK(crd::jobs::worker_index() == 0U);    // main thread is thread 0
    CHECK_FALSE(crd::jobs::is_worker_fiber()); // main thread is not inside a fiber

    crd::jobs::shutdown();

    // After shutdown the worker count must read 0, not a stale positive value:
    // num_workers()-driven dispatch (e.g. gemm_parallel_auto) would otherwise take
    // the parallel path and enqueue onto the now-dead scheduler -> crash.
    CHECK(crd::jobs::num_workers() == 0U);
}

// ---------------------------------------------------------------------------
// 12. run() executes jobs on worker threads; wait() from main thread (spin path)
// ---------------------------------------------------------------------------

TEST_CASE("jobs: run and wait from main thread", "[jobs][public-api]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U; // background worker required for the spin-wait path
    crd::jobs::init(cfg);

    constexpr int n = 20;
    std::atomic<int> count{0};

    crd::jobs::JobDecl jobs[n];
    for (auto& j : jobs)
    {
        j.fn   = [](void* d)
        {
            static_cast<std::atomic<int>*>(d)->fetch_add(1, std::memory_order_release);
        };
        j.data = &count;
    }

    crd::jobs::Counter* c = crd::jobs::run(std::span(jobs, n));
    crd::jobs::wait(c); // spin-waits on main thread until all 20 jobs complete

    CHECK(count.load() == n);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 13. run_and_wait() from inside a worker fiber
// ---------------------------------------------------------------------------

TEST_CASE("jobs: run_and_wait from inside a worker fiber", "[jobs][public-api]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
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

        constexpr int k_children = 8;
        crd::jobs::JobDecl children[k_children];
        for (auto& child : children)
        {
            child.fn   = [](void* dc)
            {
                static_cast<std::atomic<int>*>(dc)->fetch_add(1, std::memory_order_release);
            };
            child.data = data->child_count;
        }

        // wait() here runs the fiber-suspend path (we are inside a fiber).
        crd::jobs::run_and_wait(std::span(children, k_children));

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
    cfg.num_threads = 2U;
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
    cfg.num_threads       = 4U;
    cfg.small_fiber_count = 128U;
    crd::jobs::init(cfg);

    constexpr int n = 500;
    std::atomic<int> count{0};

    crd::jobs::JobDecl jobs[n];
    for (auto& j : jobs)
    {
        j.fn   = [](void* d)
        {
            static_cast<std::atomic<int>*>(d)->fetch_add(1, std::memory_order_release);
        };
        j.data = &count;
    }

    crd::jobs::wait(crd::jobs::run(std::span(jobs, n)));
    CHECK(count.load() == n);

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
    cfg.num_threads = 2U;
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
    cfg.num_threads = 2U; // two threads so inner job can run while outer fiber waits
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
    cfg.num_threads = 2U;
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
    cfg.num_threads = 4U;
    crd::jobs::init(cfg);

    constexpr crd::u32 k_count   = 100U;
    constexpr crd::u32 k_num_jobs = 4U;
    std::atomic<int> sum{0};

    // Each job accumulates (end - begin) into sum. Total must equal k_count.
    crd::jobs::Counter* c = crd::jobs::parallel_for(
        k_count, k_num_jobs,
        [&sum](crd::u32 begin, crd::u32 end)
        {
            sum.fetch_add(static_cast<int>(end - begin), std::memory_order_release);
        });

    crd::jobs::wait(c);
    CHECK(sum.load() == static_cast<int>(k_count));

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 20. parallel_for: num_jobs clamped to count when num_jobs > count
// ---------------------------------------------------------------------------

TEST_CASE("jobs: parallel_for clamps num_jobs to count", "[jobs][sbo]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4U;
    crd::jobs::init(cfg);

    // count=3, num_jobs=10 → clamped to 3. Each of the 3 items is processed once.
    constexpr crd::u32 k_count = 3U;
    std::atomic<int> processed{0};

    crd::jobs::Counter* c = crd::jobs::parallel_for(
        k_count, 10U,
        [&processed](crd::u32 /*begin*/, crd::u32 /*end*/)
        {
            processed.fetch_add(1, std::memory_order_release);
        });

    crd::jobs::wait(c);
    // 3 jobs ran (one per item after clamping), covering all 3 items.
    CHECK(processed.load() == static_cast<int>(k_count));

    crd::jobs::shutdown();
}

// ===========================================================================
// v1j — per-thread frame allocator
// ===========================================================================

// ---------------------------------------------------------------------------
// 21. frame_alloc: returns a non-null pointer with correct alignment (main thread)
// ---------------------------------------------------------------------------

TEST_CASE("jobs: frame_alloc returns aligned pointer from main thread", "[jobs][frame-alloc]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    void* p = crd::jobs::frame_alloc(64U, 8U);
    REQUIRE(p != nullptr);
    CHECK((reinterpret_cast<crd::usize>(p) & 7U) == 0U);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 22. frame_alloc: alignment padding is applied correctly
//     Alloc 3 bytes (cursor = 3), then alloc with align=8 — must land at offset 8.
// ---------------------------------------------------------------------------

TEST_CASE("jobs: frame_alloc respects alignment padding", "[jobs][frame-alloc]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    [[maybe_unused]] const void* first = crd::jobs::frame_alloc(3U, 1U); // cursor → 3
    void* second                 = crd::jobs::frame_alloc(8U, 8U); // must align to 8
    REQUIRE(second != nullptr);
    CHECK((reinterpret_cast<crd::usize>(second) & 7U) == 0U);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 23. frame_alloc: works inside a worker fiber
// ---------------------------------------------------------------------------

TEST_CASE("jobs: frame_alloc works from a worker fiber", "[jobs][frame-alloc]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    std::atomic<crd::usize> addr{0U};

    auto j = crd::jobs::make_job([&addr]()
    {
        const void* p = crd::jobs::frame_alloc(32U, 8U);
        addr.store(reinterpret_cast<crd::usize>(p), std::memory_order_release);
    });

    crd::jobs::wait(crd::jobs::run(j));
    CHECK(addr.load() != 0U);
    CHECK((addr.load() & 7U) == 0U);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 24. frame_reset: resets all arenas — allows the full capacity to be reused
// ---------------------------------------------------------------------------

TEST_CASE("jobs: frame_reset allows full capacity to be reused", "[jobs][frame-alloc]")
{
    crd::jobs::Config cfg;
    cfg.num_threads       = 2U;
    cfg.frame_alloc_bytes = 1024U; // small arena so we can fill it
    crd::jobs::init(cfg);

    // Consume half the arena from the main thread.
    void* p1 = crd::jobs::frame_alloc(512U, 1U);
    REQUIRE(p1 != nullptr);

    crd::jobs::frame_reset();

    // After reset, cursor is 0 — the full 1024 bytes are available again.
    void* p2 = crd::jobs::frame_alloc(1024U, 1U);
    REQUIRE(p2 != nullptr);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 24b. frame_try_alloc refuses what does not fit, leaves the cursor and earlier bytes alone, counts the refusal, and
//      aligns the address (not the offset) past the base's own alignment.
// ---------------------------------------------------------------------------

TEST_CASE("jobs: frame_try_alloc refuses without moving the cursor and aligns the address", "[jobs][frame-alloc]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    cfg.frame_alloc_bytes = 1024U;
    crd::jobs::init(cfg);

    auto* const head = static_cast<crd::u8*>(crd::jobs::frame_alloc(3U, 1U));
    head[0] = 0xA5U;
    head[2] = 0x5AU;
    const crd::usize mark = crd::jobs::frame_get_mark();
    CHECK(crd::jobs::frame_try_alloc(1022U, 1U) == nullptr);
    CHECK(crd::jobs::frame_try_alloc(~crd::usize{0}, 8U) == nullptr);       // no wrap past the capacity
    CHECK(crd::jobs::frame_try_alloc(8U, crd::usize{1} << 62U) == nullptr); // nor through the padding
    CHECK(crd::jobs::frame_get_mark() == mark);
    CHECK(crd::jobs::pressure_snapshot().frame.refusals == 3U);

    void* const line = crd::jobs::frame_try_alloc(64U, 256U);
    REQUIRE(line != nullptr);
    CHECK((reinterpret_cast<crd::usize>(line) & 255U) == 0U);
    CHECK(head[0] == 0xA5U);
    CHECK(head[2] == 0x5AU);
    crd::jobs::frame_set_mark(mark);
    CHECK(crd::jobs::frame_try_alloc(1021U, 1U) != nullptr); // the rest of the arena, exactly
    CHECK(crd::jobs::frame_try_alloc(1U, 1U) == nullptr);
    const crd::BudgetReading frame = crd::jobs::pressure_snapshot().frame;
    CHECK(frame.capacity == 1024U);
    CHECK(frame.in_use == 1024U);
    CHECK(frame.high_water == 1024U);
    CHECK(frame.refusals == 4U);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 24c. parallel_for and parallel_reduce reclaim their job arrays before they return: thousands of calls on a 4 KiB
//      arena leave the cursor where it was and a caller's earlier bytes untouched, with no reset anywhere.
// ---------------------------------------------------------------------------

TEST_CASE("jobs: parallel ranges leave the frame arena where they found it", "[jobs][frame-alloc]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4U;
    cfg.frame_alloc_bytes = 4096U; // 64 JobDecls: an unreclaimed loop below would exhaust it within 8 calls
    crd::jobs::init(cfg);

    auto* const caller = static_cast<crd::u64*>(crd::jobs::frame_alloc(8U * sizeof(crd::u64), alignof(crd::u64)));
    for (crd::u32 i = 0U; i < 8U; ++i)
    {
        caller[i] = 0x9E3779B97F4A7C15ULL * (i + 1U);
    }
    const crd::usize mark = crd::jobs::frame_get_mark();

    std::atomic<crd::u64> sum{0U};
    crd::u64 slots[8];
    bool marks_held = true;
    bool reduced = true;
    const auto count_range = [&sum](crd::u32 b, crd::u32 e)
    {
        sum.fetch_add(e - b, std::memory_order_relaxed);
    };
    const auto range_size = [](crd::u32 b, crd::u32 e)
    {
        return static_cast<crd::u64>(e - b);
    };
    const auto add = [](crd::u64 a, crd::u64 b)
    {
        return a + b;
    };
    for (crd::u32 round = 0U; round < 2000U; ++round)
    {
        crd::jobs::wait(crd::jobs::parallel_for(64U, 8U, count_range));
        const crd::u64 total =
            crd::jobs::parallel_reduce<crd::u64>(100U, std::span<crd::u64>(slots), 0U, range_size, add);
        reduced = reduced && total == 100U;
        marks_held = marks_held && crd::jobs::frame_get_mark() == mark;
    }
    CHECK(reduced);
    CHECK(marks_held);
    CHECK(sum.load() == 2000U * 64U);
    bool caller_intact = true;
    for (crd::u32 i = 0U; i < 8U; ++i)
    {
        caller_intact = caller_intact && caller[i] == 0x9E3779B97F4A7C15ULL * (i + 1U);
    }
    CHECK(caller_intact);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 24d. A job's parallel_reduce keeps its slots on its own stack: the wait may resume it on another thread, so no
//      frame arena could hold them. Sixteen jobs on four threads each reduce through nested ranges; afterwards every
//      thread's arena is empty again.
// ---------------------------------------------------------------------------

namespace
{
crd::u64 sum_of_indices(crd::u32 lo, crd::u32 hi)
{
    crd::u64 s = 0U;
    for (crd::u32 i = lo; i < hi; ++i)
    {
        s += i;
    }
    return s;
}

// Fifty reductions of [0, 1000 + job) from inside a job, counting the wrong totals.
void reduce_in_job(crd::u32 job, std::atomic<crd::u32>& wrong)
{
    for (crd::u32 round = 0U; round < 50U; ++round)
    {
        crd::u64 slots[4]; // 16 waiting jobs + 64 inner ones stay within 128 fibers
        const crd::u64 count = 1000U + job;
        const crd::u64 total =
            crd::jobs::parallel_reduce<crd::u64>(static_cast<crd::u32>(count), std::span<crd::u64>(slots), 0U,
                                                 &sum_of_indices, [](crd::u64 x, crd::u64 y) { return x + y; });
        if (total != count * (count - 1U) / 2U)
        {
            wrong.fetch_add(1U, std::memory_order_relaxed);
        }
    }
}
} // namespace

TEST_CASE("jobs: nested parallel_reduce in jobs is exact and leaves every arena empty", "[jobs][frame-alloc]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4U;
    cfg.frame_alloc_bytes = 4096U;
    crd::jobs::init(cfg);

    std::atomic<crd::u32> wrong{0U};
    crd::jobs::wait(crd::jobs::parallel_for(16U, 16U,
                                            [&wrong](crd::u32 b, crd::u32 e)
                                            {
                                                for (crd::u32 job = b; job < e; ++job)
                                                {
                                                    reduce_in_job(job, wrong);
                                                }
                                            }));
    CHECK(wrong.load() == 0U);
    CHECK(crd::jobs::pressure_snapshot().frame.in_use == 0U);

    crd::jobs::shutdown();
}

// ===========================================================================
// Main-thread pump + wait() deadlock fix
// ===========================================================================

// ---------------------------------------------------------------------------
// 25. pump_main_thread_once() returns true when there is a queued job,
//     false when the queues are empty.
// ---------------------------------------------------------------------------

TEST_CASE("jobs: pump_main_thread_once returns true/false correctly", "[jobs][pump]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 1U; // only thread 0 — no background workers
    crd::jobs::init(cfg);

    std::atomic<int> done{0};
    auto j = crd::jobs::make_job([&done]()
    {
        done.fetch_add(1, std::memory_order_release);
    });
    crd::jobs::Counter* c = crd::jobs::run(j);

    // Queue has one job — pump must return true.
    const bool had_work = crd::jobs::pump_main_thread_once();
    REQUIRE(had_work);
    CHECK(done.load() == 1);

    // Queue is now empty — pump must return false.
    const bool no_work = crd::jobs::pump_main_thread_once();
    CHECK_FALSE(no_work);

    // Counter was acquired by run() — wait() releases it.
    crd::jobs::wait(c);
    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 26. wait() from main thread on a thread-0-pinned job with a single-thread
//     pool does NOT deadlock. Before the fix, the spin loop called yield()
//     only, so the pinned job never ran and wait() spun forever.
// ---------------------------------------------------------------------------

TEST_CASE("jobs: wait on thread-0-pinned job single-thread no deadlock", "[jobs][pump]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 1U; // only thread 0 — no background workers
    crd::jobs::init(cfg);

    std::atomic<int> done{0};

    // Raw job pinned to thread 0.
    crd::jobs::JobDecl pinned{};
    pinned.fn = [](void* d)
    {
        static_cast<std::atomic<int>*>(d)->fetch_add(1, std::memory_order_release);
    };
    pinned.data       = &done;
    pinned.pin_thread = 0;
    pinned.priority   = crd::jobs::Priority::Normal;

    crd::jobs::Counter* c = crd::jobs::run(pinned);
    // wait() must pump thread 0's queues internally — must not deadlock.
    crd::jobs::wait(c);
    CHECK(done.load() == 1);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 27. JobFence basic usage: run → fence → fence.wait() → job complete.
// ---------------------------------------------------------------------------

TEST_CASE("jobs: JobFence basic usage", "[jobs][fence]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    std::atomic<int> done{0};
    auto j = crd::jobs::make_job([&done]()
    {
        done.fetch_add(1, std::memory_order_release);
    });

    crd::jobs::JobFence fence(crd::jobs::run(j));
    CHECK(fence.valid());

    fence.wait();
    CHECK_FALSE(fence.valid());
    CHECK(done.load() == 1);

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 28. JobFence move semantics: moved-from fence is empty; moved-to fence owns it.
// ---------------------------------------------------------------------------

TEST_CASE("jobs: JobFence move semantics", "[jobs][fence]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    std::atomic<int> done{0};
    auto j = crd::jobs::make_job([&done]()
    {
        done.fetch_add(1, std::memory_order_release);
    });

    crd::jobs::JobFence a(crd::jobs::run(j));
    REQUIRE(a.valid());

    crd::jobs::JobFence b(std::move(a));
    CHECK_FALSE(a.valid()); // NOLINT(clang-analyzer-cplusplus.Move) — intentional post-move check
    CHECK(b.valid());

    b.wait();
    CHECK(done.load() == 1);
    CHECK_FALSE(b.valid());

    crd::jobs::shutdown();
}

// ---------------------------------------------------------------------------
// 29. Shutdown with raw (no-counter) pending jobs in the queue does not crash.
//     Jobs pushed via pool.push() bypass the counter system; they may not run
//     at all if workers are stopped first.
// ---------------------------------------------------------------------------

TEST_CASE("worker_pool: shutdown with pending raw jobs does not crash", "[jobs][worker_pool]")
{
    WorkerPool pool;
    WorkerConfig cfg;
    cfg.num_threads = 1U; // only thread 0
    REQUIRE(pool.init(cfg));

    // Push raw jobs (no counter) without consuming them.
    // Shutdown must not assert or crash due to leftover items in the queue.
    std::atomic<int> counter{0};
    for (int i = 0; i < 8; ++i)
    {
        pool.push(make_inc_job(&counter));
    }

    pool.shutdown(); // must succeed cleanly
    // counter may be < 8 — no jobs ran — that is expected and acceptable.
    CHECK(pool.is_initialized() == false);
}

// ---------------------------------------------------------------------------
// 30. Regression: cross-thread fiber resume under contention.
//
// A root job that calls run_and_wait() from inside its fiber suspends and is
// frequently resumed on a *different* OS thread than the one that dispatched it.
// Many workers (> hardware_concurrency on a small box) + many deeply-nested
// run_and_wait cycles drive both the fiber-migration rate and the park / wakeup
// race window high. This is the runtime backstop for two fixes it would crash
// without:
//   (1) job_fiber_trampoline re-reads the thread-local scheduler context and
//       current-fiber slot AFTER the job returns (CRD_JOBS_TLS_OPAQUE in
//       worker_pool.cpp) — pre-fix, an optimized build cached the per-thread TLS
//       base across the job call and, on a cross-thread resume, switched onto the
//       wrong thread's scheduler stack (the historical linux-gcc-release
//       "transient SEGFAULT in jobs: run_and_wait from inside a worker fiber").
//   (2) counter_wait is "switch then publish": the fiber switches to the
//       scheduler (saving its context) *before* its Waiter is published, and the
//       cancel/wakeup handoff is a single CAS on WaiterClaim — so a resume can
//       never fire into a not-yet-parked fiber, and a Waiter is never both
//       canceled by its fiber and woken by a decrement (counter.cpp).
// ---------------------------------------------------------------------------

TEST_CASE("jobs: cross-thread fiber resume stress", "[jobs][stress]")
{
    // Kept to a unit-test-sized workload (a few thousand fiber switches): enough
    // to hit cross-thread resumes + the park/wakeup race window on a CI box,
    // without being a multi-minute fuzz run on a 1-core VM. Heavier fuzzing of
    // this path lives outside the suite.
    crd::jobs::Config cfg;
    cfg.num_threads       = 4U; // > 1 so fibers migrate; oversubscribes typical CI runners
    cfg.small_fiber_count = 128U;
    crd::jobs::init(cfg);

    struct RootData { std::atomic<long>* total; };
    std::atomic<long> total{0};
    static RootData rd; rd.total = &total;

    constexpr int rounds   = 30;
    constexpr int k_roots    = 6;
    constexpr int k_children = 5;

    for (int r = 0; r < rounds; ++r)
    {
        crd::jobs::JobDecl roots[k_roots];
        for (auto& root : roots)
        {
            root.fn = [](void* d)
            {
                auto* data = static_cast<RootData*>(d);
                crd::jobs::JobDecl children[k_children];
                for (auto& child : children)
                {
                    child.fn   = [](void* dc)
                    {
                        static_cast<std::atomic<long>*>(dc)->fetch_add(1, std::memory_order_relaxed);
                    };
                    child.data = data->total;
                }
                // Suspends this fiber; resume may land on any worker thread.
                crd::jobs::run_and_wait(std::span(children, k_children));
            };
            root.data = &rd;
        }
        crd::jobs::wait(crd::jobs::run(std::span(roots, k_roots)));
    }

    crd::jobs::shutdown();
    CHECK(total.load() == static_cast<long>(rounds) * k_roots * k_children);
}

// ASan's use-after-return detection gives each stack a fake stack (GCC and Clang; MSVC has none without
// /fsanitize-address-use-after-return). A fiber keeps one across the jobs it runs: the trampoline restores the fake
// stack the fiber saved when its previous job completed (sanitizer_fibers.hpp). Restoring none mapped a new fake stack
// for every job and leaked the old one, 41 MB a frame on the overhead harness's jobs workload (DIAG.11b).
#if defined(CRD_JOBS_TESTS_FAKE_STACKS)
TEST_CASE("asan: a fiber keeps one fake stack across the jobs it runs", "[jobs][asan][diag]")
{
    constexpr crd::u32 k_fibers = 4U + 2U + 1U;
    crd::jobs::Config cfg;
    cfg.num_threads        = 2U;
    cfg.small_fiber_count  = 4U;
    cfg.medium_fiber_count = 2U;
    cfg.large_fiber_count  = 1U;
    crd::jobs::init(cfg);

    void*   seen = nullptr;
    JobDecl job{};
    job.fn   = [](void* data) { *static_cast<void**>(data) = __asan_get_current_fake_stack(); };
    job.data = &seen;
    void*    distinct[k_fibers + 1U] = {}; // NOLINT(modernize-avoid-c-arrays): a fixed local table
    crd::u32 count = 0U;
    for (int i = 0; i < 1000 && count <= k_fibers; ++i)
    {
        seen = nullptr;
        crd::jobs::wait(crd::jobs::run(job));
        // Null only when the runtime has detect_stack_use_after_return off, which leaves this oracle nothing to judge.
        REQUIRE(seen != nullptr);
        if (std::find(distinct, distinct + count, seen) == distinct + count)
        {
            distinct[count] = seen;
            ++count;
        }
    }

    crd::jobs::shutdown();
    CHECK(count <= k_fibers);
}
#endif
