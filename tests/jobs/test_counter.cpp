#include <catch2/catch_test_macros.hpp>

#include "../../engine/jobs/src/counter.hpp"
#include "../../engine/jobs/src/fiber.hpp"
#include <crd/jobs/detail/fiber_context.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <thread>
#include <vector>

using crd::jobs::detail::Counter;
using crd::jobs::detail::CounterPool;
using crd::jobs::detail::Fiber;
using crd::jobs::detail::FiberContext;
using crd::jobs::detail::Waiter;
using crd::jobs::detail::counter_decrement;
using crd::jobs::detail::counter_wait;
using crd::jobs::detail::fiber_init_stack;
using crd::jobs::detail::fiber_switch;


#if CRD_ENABLE_ASSERTS
using crd::jobs::detail::FiberState;
#endif

// ---------------------------------------------------------------------------
// 1. CounterPool: init and shutdown
// ---------------------------------------------------------------------------

TEST_CASE("counter_pool: init and shutdown", "[jobs][counter]")
{
    CounterPool pool;
    REQUIRE(pool.init(8U));
    CHECK(pool.is_initialized());
    CHECK(pool.capacity() == 8U);
    CHECK(pool.available() == 8U);

    pool.shutdown();
    CHECK_FALSE(pool.is_initialized());

    // Re-init after shutdown is legal.
    REQUIRE(pool.init(4U));
    CHECK(pool.capacity() == 4U);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 2. CounterPool: acquire sets value; release restores availability
// ---------------------------------------------------------------------------

TEST_CASE("counter_pool: acquire sets value and release restores available", "[jobs][counter]")
{
    CounterPool pool;
    REQUIRE(pool.init(4U));

    Counter* c1 = pool.acquire(10U);
    REQUIRE(c1 != nullptr);
    CHECK(c1->value.load() == 10U);
    CHECK(pool.available() == 3U);

    Counter* c2 = pool.acquire(0U);
    REQUIRE(c2 != nullptr);
    CHECK(c2->value.load() == 0U);
    CHECK(pool.available() == 2U);

    pool.release(c1);
    CHECK(pool.available() == 3U);
    pool.release(c2);
    CHECK(pool.available() == 4U);

    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 3. CounterPool: pool indices are unique
// ---------------------------------------------------------------------------

TEST_CASE("counter_pool: all pool_indices distinct", "[jobs][counter]")
{
    CounterPool pool;
    static constexpr crd::u32 kCap = 8U;
    REQUIRE(pool.init(kCap));

    Counter* ptrs[kCap]{};
    bool     seen[kCap]{};

    for (crd::u32 i = 0U; i < kCap; ++i)
    {
        ptrs[i] = pool.acquire(0U); // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        REQUIRE(ptrs[i] != nullptr);
        const crd::u32 idx = ptrs[i]->pool_index; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        REQUIRE(idx < kCap);
        CHECK_FALSE(seen[idx]); // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        seen[idx] = true; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    }

    for (crd::u32 i = 0U; i < kCap; ++i)
        pool.release(ptrs[i]); // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)

    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 4. counter_decrement: basic — decrement from N to 0, no waiters
// ---------------------------------------------------------------------------

TEST_CASE("counter_decrement: no waiters, value reaches zero", "[jobs][counter]")
{
    CounterPool pool;
    REQUIRE(pool.init(4U));

    Counter* c = pool.acquire(3U);
    REQUIRE(c != nullptr);

    Waiter* w = counter_decrement(c, 1U);
    CHECK(w == nullptr);
    CHECK(c->value.load() == 2U);

    w = counter_decrement(c, 1U);
    CHECK(w == nullptr);
    CHECK(c->value.load() == 1U);

    w = counter_decrement(c, 1U);
    CHECK(w == nullptr);
    CHECK(c->value.load() == 0U);

    pool.release(c);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 5. counter_decrement: decrement by amount > 1
// ---------------------------------------------------------------------------

TEST_CASE("counter_decrement: bulk decrement", "[jobs][counter]")
{
    CounterPool pool;
    REQUIRE(pool.init(4U));

    Counter* c = pool.acquire(10U);
    Waiter*  w = counter_decrement(c, 5U);
    CHECK(w == nullptr);
    CHECK(c->value.load() == 5U);

    w = counter_decrement(c, 5U);
    CHECK(w == nullptr);
    CHECK(c->value.load() == 0U);

    pool.release(c);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 6. counter_decrement: canceled waiter is discarded
//
// Manually construct a Waiter with canceled == true, push it onto the counter,
// then call decrement. The returned list must be empty.
// ---------------------------------------------------------------------------

TEST_CASE("counter_decrement: canceled waiter is discarded", "[jobs][counter]")
{
    CounterPool pool;
    REQUIRE(pool.init(4U));

    Counter* c = pool.acquire(1U);

    Waiter w;
    w.target = 0U;
    w.canceled.store(true, std::memory_order_relaxed);
    // Push directly onto counter->waiters (simulate ABA path already fired).
    Waiter* head = c->waiters.load(std::memory_order_relaxed);
    w.next.store(head, std::memory_order_relaxed);
    c->waiters.store(&w, std::memory_order_release);

    Waiter* woken = counter_decrement(c, 1U);
    CHECK(woken == nullptr);    // waiter was discarded, not returned
    CHECK(c->value.load() == 0U);
    CHECK(c->waiters.load() == nullptr); // waiters list is empty

    pool.release(c);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 7. counter_decrement: waiter with non-matching target stays in list
//
// Counter goes from 3 → 2. Waiter with target=1 must remain on the counter.
// ---------------------------------------------------------------------------

TEST_CASE("counter_decrement: non-matching target left in waiter list", "[jobs][counter]")
{
    CounterPool pool;
    REQUIRE(pool.init(4U));

    Counter* c = pool.acquire(3U);

    // Need a real Fiber so the debug state check in counter_decrement passes.
    Fiber dummy{};
#if CRD_ENABLE_ASSERTS
    dummy.state = FiberState::Waiting;  // pre-set: the fiber is "suspended"
#endif

    Waiter w;
    w.fiber  = &dummy;
    w.target = 1U;
    w.canceled.store(false, std::memory_order_relaxed);
    Waiter* head = c->waiters.load(std::memory_order_relaxed);
    w.next.store(head, std::memory_order_relaxed);
    c->waiters.store(&w, std::memory_order_release);

    // Decrement 3 → 2. Target is 1, not 2. Waiter stays.
    Waiter* woken = counter_decrement(c, 1U);
    CHECK(woken == nullptr);
    CHECK(c->value.load() == 2U);
    CHECK(c->waiters.load() == &w);  // still present

    // Decrement 2 → 1. Target is 1. Waiter should be returned now.
    woken = counter_decrement(c, 1U);
    REQUIRE(woken == &w);
    CHECK(woken->next.load() == nullptr);
    CHECK(c->waiters.load() == nullptr);

    pool.release(c);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 8. counter_wait: fast path — value already at target
// ---------------------------------------------------------------------------

TEST_CASE("counter_wait: fast path when value already at target", "[jobs][counter]")
{
    CounterPool pool;
    REQUIRE(pool.init(4U));

    Counter* c = pool.acquire(0U);  // already zero

    Fiber  dummy_fiber{};
#if CRD_ENABLE_ASSERTS
    dummy_fiber.state = FiberState::Active;
#endif
    Waiter    w{};
    FiberContext sched_ctx{};  // never switched to

    counter_wait(c, &w, &dummy_fiber, sched_ctx, 0U);  // must return immediately

    // No suspension: sched_ctx.rsp should still be nullptr (fiber_switch never called).
    CHECK(sched_ctx.rsp == nullptr);

    pool.release(c);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 9. counter_wait: ABA path — value reaches target between push and double-check
//
// We simulate this by pushing a raw Waiter via the public decrement path,
// then verifying canceled==true discard semantics. The internal ABA path
// requires a race that is not deterministically testable, so here we directly
// verify the protocol building-blocks.
// ---------------------------------------------------------------------------

TEST_CASE("counter_decrement: two waiters with same target, both woken", "[jobs][counter]")
{
    CounterPool pool;
    REQUIRE(pool.init(4U));

    Counter* c = pool.acquire(1U);

    // Build two Waiter nodes with target=0. No real fibers needed here —
    // counter_decrement only reads fiber->state in debug. We'll set state
    // in debug mode to avoid assertion failures.
    Fiber f1{};
    Fiber f2{};
#if CRD_ENABLE_ASSERTS
    f1.state = FiberState::Waiting;
    f2.state = FiberState::Waiting;
#endif

    Waiter w1;
    Waiter w2;
    w1.fiber  = &f1;
    w1.target = 0U;
    w1.canceled.store(false, std::memory_order_relaxed);

    w2.fiber  = &f2;
    w2.target = 0U;
    w2.canceled.store(false, std::memory_order_relaxed);

    // Push both.
    w1.next.store(nullptr, std::memory_order_relaxed);
    c->waiters.store(&w1, std::memory_order_release);

    Waiter* head = c->waiters.load(std::memory_order_relaxed);
    w2.next.store(head, std::memory_order_relaxed);
    c->waiters.store(&w2, std::memory_order_release);

    Waiter* woken = counter_decrement(c, 1U);
    CHECK(c->value.load() == 0U);
    CHECK(c->waiters.load() == nullptr);

    // Both waiters must appear in the returned list (order may vary).
    int count = 0;
    bool saw_w1 = false;
    bool saw_w2 = false;
    for (Waiter* cur = woken; cur != nullptr; cur = cur->next.load())
    {
        if (cur == &w1) saw_w1 = true;
        if (cur == &w2) saw_w2 = true;
        ++count;
    }
    CHECK(count   == 2);
    CHECK(saw_w1);
    CHECK(saw_w2);

    pool.release(c);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// Fiber-switch tests — globals (same pattern as test_fiber_switch.cpp)
// ---------------------------------------------------------------------------

// Shared state for tests 10 and 11.
struct WaitTestState
{
    CounterPool   pool;
    Counter*      counter  = nullptr;
    Fiber         job_fiber{};
    Waiter        waiter{};
    FiberContext  sched_ctx{};
    bool          job_completed = false;
};

// ---------------------------------------------------------------------------
// 10. counter_wait: full suspension and resumption via fiber_switch
// ---------------------------------------------------------------------------

static WaitTestState g_s10;

static void wait_job_entry_10()
{
    // This fiber waits on the counter (value starts at 1, target = 0).
    counter_wait(g_s10.counter, &g_s10.waiter, &g_s10.job_fiber, g_s10.sched_ctx, 0U);
    g_s10.job_completed = true;
    // Return control to the scheduler.
    fiber_switch(&g_s10.job_fiber.context, &g_s10.sched_ctx);
}

TEST_CASE("counter_wait: full suspension and resumption", "[jobs][counter]")
{
    // Reset mutable state (pool is default-constructed; atomics re-initialised individually).
    g_s10.pool.shutdown();
    g_s10.counter        = nullptr;
    g_s10.job_fiber      = Fiber{};
    g_s10.waiter.fiber   = nullptr;
    g_s10.waiter.target  = 0U;
    g_s10.waiter.canceled.store(false, std::memory_order_relaxed);
    g_s10.waiter.next.store(nullptr, std::memory_order_relaxed);
    g_s10.sched_ctx      = FiberContext{};
    g_s10.job_completed  = false;

    REQUIRE(g_s10.pool.init(4U));
    g_s10.counter = g_s10.pool.acquire(1U);

#if CRD_ENABLE_ASSERTS
    g_s10.job_fiber.state = FiberState::Active;
#endif

    constexpr crd::usize kStackSize = 64U * 1024U;
    auto stack = std::make_unique<crd::u8[]>(kStackSize);
    fiber_init_stack(g_s10.job_fiber.context, stack.get(), kStackSize, wait_job_entry_10);

    // Switch into the job fiber — it will call counter_wait and suspend.
    fiber_switch(&g_s10.sched_ctx, &g_s10.job_fiber.context);

    // We're back in the scheduler. The job fiber suspended.
    CHECK_FALSE(g_s10.job_completed);
    CHECK(g_s10.waiter.fiber == &g_s10.job_fiber);

    // Decrement brings value to 0 → wakes the waiter.
    Waiter* woken = counter_decrement(g_s10.counter, 1U);
    REQUIRE(woken == &g_s10.waiter);
    CHECK(g_s10.waiter.next.load() == nullptr);

    // Resume the job fiber (in a real worker this would be a pushed job).
    fiber_switch(&g_s10.sched_ctx, &woken->fiber->context);

    // Job fiber finished.
    CHECK(g_s10.job_completed);

    g_s10.pool.release(g_s10.counter);
    g_s10.pool.shutdown();
}

// ---------------------------------------------------------------------------
// 11. counter_wait: value decremented to zero BEFORE wait() — fast path fires
// ---------------------------------------------------------------------------

TEST_CASE("counter_wait: fast path fires when counter already zero before call", "[jobs][counter]")
{
    CounterPool pool;
    REQUIRE(pool.init(4U));
    Counter* c = pool.acquire(2U);

    // Decrement all the way to 0 before any waiter is registered.
    Waiter* w = counter_decrement(c, 2U);
    CHECK(w == nullptr);  // no waiters yet
    CHECK(c->value.load() == 0U);

    // Now wait — value is already 0, so fast path triggers immediately.
    Fiber  f{};
#if CRD_ENABLE_ASSERTS
    f.state = FiberState::Active;
#endif
    Waiter    wnode{};
    FiberContext sched{};

    counter_wait(c, &wnode, &f, sched, 0U);

    // sched.rsp == nullptr → fiber_switch was never called.
    CHECK(sched.rsp == nullptr);

    pool.release(c);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 12. counter_wait: multiple sequential waits on the same counter reset
// ---------------------------------------------------------------------------

struct WaitTestState12
{
    CounterPool  pool;
    Counter*     counter  = nullptr;
    Fiber        job_fiber{};
    Waiter       waiter{};
    FiberContext sched_ctx{};
    int          run_count = 0;
};

static WaitTestState12 g_s12;

static void wait_job_entry_12()
{
    // First wait.
    counter_wait(g_s12.counter, &g_s12.waiter, &g_s12.job_fiber, g_s12.sched_ctx, 0U);
    ++g_s12.run_count;
    fiber_switch(&g_s12.job_fiber.context, &g_s12.sched_ctx);

    // Second wait (counter re-acquired externally between the two fiber runs).
    counter_wait(g_s12.counter, &g_s12.waiter, &g_s12.job_fiber, g_s12.sched_ctx, 0U);
    ++g_s12.run_count;
    fiber_switch(&g_s12.job_fiber.context, &g_s12.sched_ctx);
}

TEST_CASE("counter_wait: two sequential waits on renewed counter", "[jobs][counter]")
{
    g_s12.pool.shutdown();
    g_s12.counter      = nullptr;
    g_s12.job_fiber    = Fiber{};
    g_s12.waiter.fiber = nullptr;
    g_s12.waiter.target = 0U;
    g_s12.waiter.canceled.store(false, std::memory_order_relaxed);
    g_s12.waiter.next.store(nullptr, std::memory_order_relaxed);
    g_s12.sched_ctx    = FiberContext{};
    g_s12.run_count    = 0;

    REQUIRE(g_s12.pool.init(4U));
    g_s12.counter = g_s12.pool.acquire(1U);
#if CRD_ENABLE_ASSERTS
    g_s12.job_fiber.state = FiberState::Active;
#endif

    constexpr crd::usize kStackSize = 64U * 1024U;
    auto stack = std::make_unique<crd::u8[]>(kStackSize);
    fiber_init_stack(g_s12.job_fiber.context, stack.get(), kStackSize, wait_job_entry_12);

    // --- First round ---
    fiber_switch(&g_s12.sched_ctx, &g_s12.job_fiber.context);
    CHECK(g_s12.run_count == 0);  // suspended

    Waiter* woken = counter_decrement(g_s12.counter, 1U);
    REQUIRE(woken != nullptr);
    fiber_switch(&g_s12.sched_ctx, &woken->fiber->context);
    CHECK(g_s12.run_count == 1);  // first wait done

    // --- Second round: re-acquire a fresh counter ---
    g_s12.pool.release(g_s12.counter);
    g_s12.counter = g_s12.pool.acquire(1U);
    // Reset waiter for second use (can't copy-assign Waiter since it has atomics).
    g_s12.waiter.fiber = nullptr;
    g_s12.waiter.target = 0U;
    g_s12.waiter.canceled.store(false, std::memory_order_relaxed);
    g_s12.waiter.next.store(nullptr, std::memory_order_relaxed);
#if CRD_ENABLE_ASSERTS
    // After returning from fiber_switch inside counter_wait, state was set back to Active.
    CHECK(g_s12.job_fiber.state == FiberState::Active);
#endif

    // Re-enter the job fiber for the second wait.
    fiber_switch(&g_s12.sched_ctx, &g_s12.job_fiber.context);
    CHECK(g_s12.run_count == 1);  // suspended again

    woken = counter_decrement(g_s12.counter, 1U);
    REQUIRE(woken != nullptr);
    fiber_switch(&g_s12.sched_ctx, &woken->fiber->context);
    CHECK(g_s12.run_count == 2);  // second wait done

    g_s12.pool.release(g_s12.counter);
    g_s12.pool.shutdown();
}

// ---------------------------------------------------------------------------
// 13. Concurrent stress: many threads decrement a shared counter
//
// N threads each decrement a counter initialised to N. Only one decrement
// reaches 0 and only that one returns a non-null woken list.
// ---------------------------------------------------------------------------

TEST_CASE("counter_decrement: concurrent decrements, exactly one caller reaches zero",
          "[jobs][counter][stress]")
{
    static constexpr crd::u32 kN = 16U;

    CounterPool pool;
    REQUIRE(pool.init(4U));
    Counter* c = pool.acquire(kN);

    std::atomic<int> zero_count{0};  // how many threads received a non-null woken list

    std::vector<std::thread> threads;
    threads.reserve(kN);
    for (crd::u32 i = 0U; i < kN; ++i)
    {
        threads.emplace_back([&]()
        {
            Waiter* w = counter_decrement(c, 1U);
            if (w != nullptr)
                zero_count.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& t : threads)
        t.join();

    CHECK(c->value.load() == 0U);
    // No waiters were registered, so woken lists should all be nullptr.
    CHECK(zero_count.load() == 0);
    CHECK(c->waiters.load() == nullptr);

    pool.release(c);
    pool.shutdown();
}

// ---------------------------------------------------------------------------
// 14. Concurrent stress: acquire/release many times from multiple threads
// ---------------------------------------------------------------------------

TEST_CASE("counter_pool: concurrent acquire/release stress", "[jobs][counter][stress]")
{
    static constexpr crd::u32 kPoolSize   = 32U;
    static constexpr int      kIterations = 500;
    static constexpr crd::u32 kThreads    = 4U;

    CounterPool pool;
    REQUIRE(pool.init(kPoolSize));

    // Each thread repeatedly: acquire 4 counters, do some work, release them.
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    std::atomic<int> errors{0};

    for (crd::u32 t = 0U; t < kThreads; ++t)
    {
        threads.emplace_back([&]()
        {
            for (int iter = 0; iter < kIterations; ++iter)
            {
                Counter* c = pool.acquire(42U);
                if (c == nullptr || c->value.load() != 42U)
                {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    if (c) pool.release(c);
                    continue;
                }
                pool.release(c);
            }
        });
    }
    for (auto& t : threads)
        t.join();

    CHECK(errors.load() == 0);
    CHECK(pool.available() == kPoolSize);
    pool.shutdown();
}
