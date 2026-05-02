#include <catch2/catch_test_macros.hpp>

#include "../../engine/jobs/src/fiber_pool.hpp"
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

using crd::jobs::detail::Fiber;
using crd::jobs::detail::FiberPool;
using crd::jobs::detail::FiberPoolConfig;
using crd::jobs::detail::FiberTier;


#if CRD_ENABLE_ASSERTS
using crd::jobs::detail::FiberState;
#endif

// ---------------------------------------------------------------------------
// Test trampoline — must never actually execute during pool tests.
// The pool calls fiber_init_stack with this; the tests never context-switch
// into any fiber, so this function is never reached.
// ---------------------------------------------------------------------------

[[noreturn]] static void test_fiber_trampoline()
{
    // If this executes, a test incorrectly switched into a fiber.
    CRD_FATAL("test_fiber_trampoline: fiber was unexpectedly entered during pool tests");
    // CRD_FATAL calls CRD_DEBUGBREAK which is not [[noreturn]], so appease the compiler:
    for (;;) {}
}

// ---------------------------------------------------------------------------
// Helper: create a minimal pool for tests.
// Default: small=8, medium=4, large=2 — tiny counts keep test runtime fast
// while still exercising multi-element list operations.
// ---------------------------------------------------------------------------

static FiberPoolConfig make_test_config(crd::u32 small_count  = 8U,
                                        crd::u32 medium_count = 4U,
                                        crd::u32 large_count  = 2U)
{
    FiberPoolConfig cfg;
    cfg.small_count  = small_count;
    cfg.medium_count = medium_count;
    cfg.large_count  = large_count;
    cfg.trampoline   = test_fiber_trampoline;
    return cfg;
}

// ---------------------------------------------------------------------------
// 1. Lifecycle: init() succeeds and shutdown() is safe to call.
// ---------------------------------------------------------------------------

TEST_CASE("fiber_pool: init and shutdown", "[jobs][fiber_pool]")
{
    FiberPool pool;
    REQUIRE(pool.init(make_test_config(4U, 2U, 1U)));
    REQUIRE(pool.is_initialized());

    CHECK(pool.available_count(FiberTier::Small)  == 4U);
    CHECK(pool.available_count(FiberTier::Medium) == 2U);
    CHECK(pool.available_count(FiberTier::Large)  == 1U);

    pool.shutdown();
    CHECK_FALSE(pool.is_initialized());

    // A second shutdown on an already-shut-down pool must be a no-op.
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 2. acquire() returns a non-null fiber with the correct tier tag.
// ---------------------------------------------------------------------------

TEST_CASE("fiber_pool: acquire returns correct tier", "[jobs][fiber_pool]")
{
    FiberPool pool;
    REQUIRE(pool.init(make_test_config()));

    Fiber* small  = pool.acquire(FiberTier::Small);
    Fiber* medium = pool.acquire(FiberTier::Medium);
    Fiber* large  = pool.acquire(FiberTier::Large);

    REQUIRE(small  != nullptr);
    REQUIRE(medium != nullptr);
    REQUIRE(large  != nullptr);

    CHECK(small->tier  == FiberTier::Small);
    CHECK(medium->tier == FiberTier::Medium);
    CHECK(large->tier  == FiberTier::Large);

    pool.release(small);
    pool.release(medium);
    pool.release(large);

    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 3. Stack context is initialised: context.rsp is non-null after init.
// ---------------------------------------------------------------------------

TEST_CASE("fiber_pool: stack context is initialised", "[jobs][fiber_pool]")
{
    FiberPool pool;
    REQUIRE(pool.init(make_test_config(2U, 1U, 1U)));

    Fiber* f = pool.acquire(FiberTier::Small);
    REQUIRE(f != nullptr);
    CHECK(f->context.rsp != nullptr); // fiber_init_stack must have set this

    pool.release(f);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 4. pool_index is unique and in range for every fiber in a tier.
// ---------------------------------------------------------------------------

TEST_CASE("fiber_pool: pool_index is unique and in range", "[jobs][fiber_pool]")
{
    static constexpr crd::u32 kCount = 6U;
    FiberPool pool;
    REQUIRE(pool.init(make_test_config(kCount, 1U, 1U)));

    std::vector<Fiber*> acquired;
    acquired.reserve(kCount);
    for (crd::u32 i = 0; i < kCount; ++i)
    {
        Fiber* f = pool.acquire(FiberTier::Small);
        REQUIRE(f != nullptr);
        acquired.push_back(f);
    }

    // All indices must be in [0, kCount) and pairwise distinct.
    std::vector<crd::u32> indices;
    indices.reserve(kCount);
    for (Fiber* f : acquired)
    {
        CHECK(f->pool_index < kCount);
        indices.push_back(f->pool_index);
    }
    std::sort(indices.begin(), indices.end());
    const bool all_unique = std::adjacent_find(indices.begin(), indices.end()) == indices.end();
    CHECK(all_unique);

    for (Fiber* f : acquired)
        pool.release(f);

    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 5. available_count tracks outstanding acquires correctly.
// ---------------------------------------------------------------------------

TEST_CASE("fiber_pool: available_count tracks acquires", "[jobs][fiber_pool]")
{
    static constexpr crd::u32 kCount = 4U;
    FiberPool pool;
    REQUIRE(pool.init(make_test_config(kCount, 1U, 1U)));

    CHECK(pool.available_count(FiberTier::Small) == kCount);

    std::vector<Fiber*> held;
    for (crd::u32 i = 0; i < kCount; ++i)
    {
        held.push_back(pool.acquire(FiberTier::Small));
        CHECK(pool.available_count(FiberTier::Small) == (kCount - i - 1U));
    }

    for (crd::u32 i = 0; i < kCount; ++i)
    {
        pool.release(held[i]);
        CHECK(pool.available_count(FiberTier::Small) == i + 1U);
    }

    CHECK(pool.available_count(FiberTier::Small) == kCount);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 6. All fibers can be acquired and re-acquired after a full release cycle.
// ---------------------------------------------------------------------------

TEST_CASE("fiber_pool: full acquire-release cycle is repeatable", "[jobs][fiber_pool]")
{
    static constexpr crd::u32 kCount = 5U;
    FiberPool pool;
    REQUIRE(pool.init(make_test_config(kCount, 1U, 1U)));

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        std::vector<Fiber*> held;
        held.reserve(kCount);
        for (crd::u32 i = 0; i < kCount; ++i)
        {
            Fiber* f = pool.acquire(FiberTier::Small);
            REQUIRE(f != nullptr);
            held.push_back(f);
        }
        CHECK(pool.available_count(FiberTier::Small) == 0U);

        for (Fiber* f : held)
            pool.release(f);
        CHECK(pool.available_count(FiberTier::Small) == kCount);
    }

    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 7. Tiers are fully independent: exhausting Small does not affect Medium/Large.
// ---------------------------------------------------------------------------

TEST_CASE("fiber_pool: tiers are independent", "[jobs][fiber_pool]")
{
    FiberPool pool;
    REQUIRE(pool.init(make_test_config(4U, 3U, 2U)));

    // Exhaust the Small tier.
    std::vector<Fiber*> smalls;
    for (crd::u32 i = 0; i < 4U; ++i)
        smalls.push_back(pool.acquire(FiberTier::Small));
    CHECK(pool.available_count(FiberTier::Small) == 0U);

    // Medium and Large must still be fully available.
    CHECK(pool.available_count(FiberTier::Medium) == 3U);
    CHECK(pool.available_count(FiberTier::Large)  == 2U);

    Fiber* m = pool.acquire(FiberTier::Medium);
    Fiber* l = pool.acquire(FiberTier::Large);
    REQUIRE(m != nullptr);
    REQUIRE(l != nullptr);

    pool.release(m);
    pool.release(l);
    for (Fiber* f : smalls)
        pool.release(f);

    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 8. Exhaustion: acquire() returns nullptr when the pool is empty.
//    In debug builds the assert fires but execution continues (Ignore = 0).
// ---------------------------------------------------------------------------

TEST_CASE("fiber_pool: exhaustion returns nullptr", "[jobs][fiber_pool]")
{
    static constexpr crd::u32 kCount = 3U;

#if CRD_ENABLE_ASSERTS
    // Suppress the assert UI so the test runner does not block.
    crd::set_assert_platform_handler([](const char*) -> int { return 0; });
#endif

    FiberPool pool;
    REQUIRE(pool.init(make_test_config(kCount, 1U, 1U)));

    std::vector<Fiber*> held;
    for (crd::u32 i = 0; i < kCount; ++i)
        held.push_back(pool.acquire(FiberTier::Small));

    // One more acquire on an empty pool.
    Fiber* extra = pool.acquire(FiberTier::Small);
    CHECK(extra == nullptr);

#if CRD_ENABLE_ASSERTS
    crd::set_assert_platform_handler(nullptr);
#endif

    for (Fiber* f : held)
        pool.release(f);

    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 9. Re-acquire after release returns the same pool_index for a single-element tier.
//    This verifies the Treiber push/pop round-trip is lossless.
// ---------------------------------------------------------------------------

TEST_CASE("fiber_pool: reacquire returns correct fiber after release", "[jobs][fiber_pool]")
{
    FiberPool pool;
    REQUIRE(pool.init(make_test_config(1U, 1U, 1U)));

    Fiber* f1 = pool.acquire(FiberTier::Small);
    REQUIRE(f1 != nullptr);
    const crd::u32 idx = f1->pool_index;
    pool.release(f1);

    Fiber* f2 = pool.acquire(FiberTier::Small);
    REQUIRE(f2 != nullptr);
    CHECK(f2->pool_index == idx);
    pool.release(f2);

    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 10. Peak-usage tracking.
// ---------------------------------------------------------------------------

TEST_CASE("fiber_pool: peak usage tracking", "[jobs][fiber_pool]")
{
    static constexpr crd::u32 kCount = 6U;
    FiberPool pool;
    REQUIRE(pool.init(make_test_config(kCount, 1U, 1U)));

    CHECK(pool.peak_acquired(FiberTier::Small) == 0U);

    // Acquire half the tier.
    std::vector<Fiber*> batch1;
    for (crd::u32 i = 0; i < 3U; ++i)
        batch1.push_back(pool.acquire(FiberTier::Small));
    CHECK(pool.peak_acquired(FiberTier::Small) == 3U);

    // Release and re-acquire at higher watermark.
    for (Fiber* f : batch1)
        pool.release(f);

    std::vector<Fiber*> batch2;
    for (crd::u32 i = 0; i < kCount; ++i)
        batch2.push_back(pool.acquire(FiberTier::Small));
    CHECK(pool.peak_acquired(FiberTier::Small) == kCount);

    for (Fiber* f : batch2)
        pool.release(f);

    // Peak should remain at kCount even after full release.
    CHECK(pool.peak_acquired(FiberTier::Small) == kCount);

    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 11. Debug-only state machine assertions.
// ---------------------------------------------------------------------------

#if CRD_ENABLE_ASSERTS
TEST_CASE("fiber_pool: state is Active after acquire", "[jobs][fiber_pool]")
{
    FiberPool pool;
    REQUIRE(pool.init(make_test_config(2U, 1U, 1U)));

    Fiber* f = pool.acquire(FiberTier::Small);
    REQUIRE(f != nullptr);
    CHECK(f->state == FiberState::Active);

    pool.release(f);
    pool.shutdown();
}

TEST_CASE("fiber_pool: state is Idle after release", "[jobs][fiber_pool]")
{
    FiberPool pool;
    REQUIRE(pool.init(make_test_config(2U, 1U, 1U)));

    Fiber* f = pool.acquire(FiberTier::Small);
    REQUIRE(f != nullptr);
    pool.release(f);
    CHECK(f->state == FiberState::Idle);

    pool.shutdown();
}
#endif

// ---------------------------------------------------------------------------
// 12. Concurrent acquire/release stress test — ABA and free-list integrity.
//
// Each thread repeatedly acquires one fiber, marks its index as in-use,
// yields, clears the mark, and releases. If the Treiber stack is corrupted
// (e.g., by ABA), the same fiber index will be handed to two threads
// simultaneously, which the in_use[] vector detects.
//
// In debug builds the state-machine assert provides an additional check:
// a doubly-acquired fiber would fire "not Idle" before the in_use[] check.
// ---------------------------------------------------------------------------

TEST_CASE("fiber_pool: concurrent acquire-release stress (ABA safety)", "[jobs][fiber_pool][stress]")
{
    static constexpr crd::u32 kSmallCount  = 16U;
    static constexpr crd::u32 kThreadCount = 4U;
    static constexpr crd::u32 kIterations  = 8'000U;

    FiberPool pool;
    REQUIRE(pool.init(make_test_config(kSmallCount, 1U, 1U)));

    // One flag per fiber index; true while that fiber is held by a thread.
    std::vector<std::atomic<bool>> in_use(kSmallCount);
    for (auto& b : in_use)
        b.store(false, std::memory_order_relaxed);

    std::atomic<bool> corruption_detected{false};

    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (crd::u32 t = 0; t < kThreadCount; ++t)
    {
        threads.emplace_back([&]()
        {
            for (crd::u32 iter = 0; iter < kIterations; ++iter)
            {
                Fiber* f = pool.acquire(FiberTier::Small);
                if (!f)
                    continue; // pool momentarily exhausted — rare, skip iteration

                const crd::u32 idx = f->pool_index;

                // Mark as in-use: if already marked, another thread holds this fiber
                // → the free list was corrupted (ABA or incorrect state tracking).
                const bool already_held = in_use[idx].exchange(true, std::memory_order_acq_rel);
                if (already_held)
                    corruption_detected.store(true, std::memory_order_relaxed);

                std::this_thread::yield();

                in_use[idx].store(false, std::memory_order_release);
                pool.release(f);
            }
        });
    }

    for (auto& th : threads)
        th.join();

    CHECK_FALSE(corruption_detected.load());
    // After all threads complete, every fiber must be back in the pool.
    CHECK(pool.available_count(FiberTier::Small) == kSmallCount);

    pool.shutdown();
}
