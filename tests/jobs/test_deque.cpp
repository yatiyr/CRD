#include <catch2/catch_test_macros.hpp>

#include "../../engine/jobs/src/work_stealing_deque.hpp"
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <thread>
#include <vector>

using crd::jobs::detail::WorkStealingDeque;

// ---------------------------------------------------------------------------
// 1. Construction: capacity, empty, size initialise correctly.
// ---------------------------------------------------------------------------

TEST_CASE("work_stealing_deque: construction", "[jobs][deque]")
{
    const WorkStealingDeque<crd::u32> d8(8U);
    CHECK(d8.capacity() == 8U);
    CHECK(d8.empty());

    const WorkStealingDeque<crd::u32> d1024(1024U);
    CHECK(d1024.capacity() == 1024U);
    CHECK(d1024.empty());
}

// ---------------------------------------------------------------------------
// 2. Push and pop single item round-trip.
// ---------------------------------------------------------------------------

TEST_CASE("work_stealing_deque: push and pop single item", "[jobs][deque]")
{
    WorkStealingDeque<crd::u32> d(8U);

    REQUIRE(d.push(42U));
    CHECK_FALSE(d.empty());
    CHECK(d.size() == 1);

    auto v = d.pop();
    REQUIRE(v.has_value());
    CHECK(*v == 42U);
    CHECK(d.empty());
}

// ---------------------------------------------------------------------------
// 3. Pop on an empty deque returns nullopt without modifying state.
// ---------------------------------------------------------------------------

TEST_CASE("work_stealing_deque: pop empty returns nullopt", "[jobs][deque]")
{
    WorkStealingDeque<crd::u32> d(4U);
    CHECK_FALSE(d.pop().has_value());
    CHECK(d.empty());

    // Multiple pops on empty must all return nullopt.
    CHECK_FALSE(d.pop().has_value());
    CHECK_FALSE(d.pop().has_value());
}

// ---------------------------------------------------------------------------
// 4. Steal on an empty deque returns nullopt.
// ---------------------------------------------------------------------------

TEST_CASE("work_stealing_deque: steal empty returns nullopt", "[jobs][deque]")
{
    WorkStealingDeque<crd::u32> d(4U);
    CHECK_FALSE(d.steal().has_value());
    CHECK_FALSE(d.steal().has_value());
}

// ---------------------------------------------------------------------------
// 5. Pop yields items in LIFO order (owner side).
// ---------------------------------------------------------------------------

TEST_CASE("work_stealing_deque: LIFO ordering via pop", "[jobs][deque]")
{
    static constexpr crd::u32 n = 5U;
    WorkStealingDeque<crd::u32> d(16U);

    for (crd::u32 i = 0U; i < n; ++i)
    {
        REQUIRE(d.push(i));
    }

    // Pop should give n-1, n-2, ..., 0.
    for (crd::u32 i = n; i-- > 0U;)
    {
        auto v = d.pop();
        REQUIRE(v.has_value());
        CHECK(*v == i);
    }
    CHECK(d.empty());
}

// ---------------------------------------------------------------------------
// 6. Steal yields items in FIFO order (from a single-thread-filled deque).
//    Items stolen from another thread come out in push order.
// ---------------------------------------------------------------------------

TEST_CASE("work_stealing_deque: FIFO ordering via steal", "[jobs][deque]")
{
    static constexpr crd::u32 n = 6U;
    WorkStealingDeque<crd::u32> d(16U);

    for (crd::u32 i = 0U; i < n; ++i)
    {
        REQUIRE(d.push(i));
    }

    // Steal from a separate thread to avoid the "owner" restriction.
    std::vector<crd::u32> stolen;
    stolen.reserve(n);
    std::thread thief([&]()
    {
        for (crd::u32 i = 0U; i < n; ++i)
        {
            auto v = d.steal();
            if (v.has_value())
            {
                stolen.push_back(*v);
            }
        }
    });
    thief.join();

    REQUIRE(stolen.size() == n);
    for (crd::u32 i = 0U; i < n; ++i)
    {
        CHECK(stolen.at(i) == i); // first pushed is first stolen (FIFO)
    }
}

// ---------------------------------------------------------------------------
// 7. Full deque: push() returns false (and asserts in debug).
// ---------------------------------------------------------------------------

TEST_CASE("work_stealing_deque: full deque returns false on push", "[jobs][deque]")
{
    static constexpr crd::u32 cap = 4U;

#if CRD_ENABLE_ASSERTS
    crd::set_assert_platform_handler([](const char*) -> int { return 0; });
#endif

    WorkStealingDeque<crd::u32> d(cap);
    for (crd::u32 i = 0U; i < cap; ++i)
    {
        REQUIRE(d.push(i));
    }

    // One more push on a full deque must fail.
    CHECK_FALSE(d.push(99U));
    CHECK(d.size() == static_cast<crd::i64>(cap));

#if CRD_ENABLE_ASSERTS
    crd::set_assert_platform_handler(nullptr);
#endif

    // Drain.
    for (crd::u32 i = 0U; i < cap; ++i)
    {
        (void)d.pop();
    }
}

// ---------------------------------------------------------------------------
// 8. Interleaved push/pop/steal (single thread, exercises internal paths).
// ---------------------------------------------------------------------------

TEST_CASE("work_stealing_deque: interleaved push pop steal", "[jobs][deque]")
{
    WorkStealingDeque<crd::u32> d(8U);

    REQUIRE(d.push(1U));
    REQUIRE(d.push(2U));
    CHECK(d.size() == 2);

    // steal one from the front (FIFO: value 1).
    auto s = d.steal();
    REQUIRE(s.has_value());
    CHECK(*s == 1U);

    // pop one from the back (LIFO: value 2).
    auto p = d.pop();
    REQUIRE(p.has_value());
    CHECK(*p == 2U);

    CHECK(d.empty());

    // Push three more and alternate pop/steal.
    REQUIRE(d.push(10U));
    REQUIRE(d.push(20U));
    REQUIRE(d.push(30U));

    auto p2 = d.pop();   // LIFO: 30
    REQUIRE(p2.has_value());
    CHECK(*p2 == 30U);

    auto s2 = d.steal(); // FIFO: 10
    REQUIRE(s2.has_value());
    CHECK(*s2 == 10U);

    auto p3 = d.pop();   // only 20 left
    REQUIRE(p3.has_value());
    CHECK(*p3 == 20U);

    CHECK(d.empty());
}

// ---------------------------------------------------------------------------
// 9. size() and empty() track push/pop/steal correctly.
// ---------------------------------------------------------------------------

TEST_CASE("work_stealing_deque: size tracking", "[jobs][deque]")
{
    WorkStealingDeque<crd::u32> d(16U);
    CHECK(d.empty());

    for (crd::u32 i = 1U; i <= 5U; ++i)
    {
        REQUIRE(d.push(i));
        CHECK(d.size() == static_cast<crd::i64>(i));
        CHECK_FALSE(d.empty());
    }

    (void)d.pop();
    CHECK(d.size() == 4);

    (void)d.steal();
    CHECK(d.size() == 3);

    while (d.pop()) {}
    CHECK(d.empty());
}

// ---------------------------------------------------------------------------
// 10. Last-element race: push 1 item, then owner pops and a thief steals
//     concurrently. Exactly one must succeed.
// ---------------------------------------------------------------------------

TEST_CASE("work_stealing_deque: last element race", "[jobs][deque]")
{
    // Push 1 item, then pop and steal race concurrently. Exactly one must win
    // each trial. We do not assert which side wins — that depends on scheduling
    // and is not a correctness property. The invariant is that the item is
    // never lost and never duplicated.
    static constexpr int trials = 4000;

    for (int trial = 0; trial < trials; ++trial)
    {
        WorkStealingDeque<crd::u32> d(4U);
        REQUIRE(d.push(1U));

        std::atomic<bool> go{false};
        std::atomic<int>  steal_result{-1};

        std::thread thief([&]()
        {
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            auto v = d.steal();
            steal_result.store(v.has_value() ? 1 : 0, std::memory_order_release);
        });

        go.store(true, std::memory_order_release);
        auto pop_v = d.pop();
        thief.join();

        const bool thief_got = steal_result.load() == 1;
        const bool owner_got = pop_v.has_value();

        // Exactly one of {pop, steal} must retrieve the item.
        REQUIRE((owner_got ^ thief_got) == true);
    }
}

// ---------------------------------------------------------------------------
// 11. Concurrent steal stress: pre-fill, then drain with owner pop + thieves.
//
// Every item must be consumed exactly once. This exercises the CAS races
// between pop() and multiple concurrent steal() calls.
// ---------------------------------------------------------------------------

TEST_CASE("work_stealing_deque: concurrent steal from pre-filled deque (stress)",
          "[jobs][deque][stress]")
{
    static constexpr crd::u32 cap     = 512U;
    static constexpr crd::u32 items   = 400U;
    static constexpr crd::u32 thieves = 4U;

    WorkStealingDeque<crd::u32> deque(cap);

    // Phase 1: fill (single-threaded, no concurrent access).
    for (crd::u32 i = 0U; i < items; ++i)
    {
        REQUIRE(deque.push(i));
    }

    // Track how many times each item value was consumed.
    std::vector<std::atomic<int>> seen(items);
    for (auto& a : seen)
    {
        a.store(0, std::memory_order_relaxed);
    }

    std::atomic<bool> done{false};

    // Phase 2: thieves drain concurrently.
    std::vector<std::thread> thief_threads;
    thief_threads.reserve(thieves);
    for (crd::u32 t = 0U; t < thieves; ++t)
    {
        thief_threads.emplace_back([&]()
        {
            while (!done.load(std::memory_order_acquire))
            {
                if (auto v = deque.steal())
                {
                    // NOLINT: *v is always in [0, items); bounds verified by test correctness
                    seen[*v].fetch_add(1, std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            }
            // Drain any items remaining after done is signalled.
            while (auto v = deque.steal())
            {
                seen[*v].fetch_add(1, std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
        });
    }

    // Owner also pops.
    while (auto v = deque.pop())
    {
        seen[*v].fetch_add(1, std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }

    done.store(true, std::memory_order_release);
    for (auto& th : thief_threads)
    {
        th.join();
    }

    // Every item must have been consumed exactly once.
    for (crd::u32 i = 0U; i < items; ++i)
    {
        CHECK(seen.at(i).load(std::memory_order_relaxed) == 1);
    }
}

// ---------------------------------------------------------------------------
// 12. Concurrent push + pop + steal stress: owner pushes and pops while
//     thieves steal. Exercises all three operations racing simultaneously.
//
// Items are identified by value; each must be consumed exactly once.
// Back-pressure: owner pops when the deque is near-full to prevent
// push() from ever returning false.
// ---------------------------------------------------------------------------

TEST_CASE("work_stealing_deque: concurrent push pop steal stress",
          "[jobs][deque][stress]")
{
    static constexpr crd::u32 cap     = 256U;
    static constexpr crd::u32 items   = 10'000U;
    static constexpr crd::u32 thieves = 3U;
    static constexpr auto     hi_water = static_cast<crd::i64>(cap * 3U / 4U);

    WorkStealingDeque<crd::u32> deque(cap);

    std::vector<std::atomic<int>> seen(items);
    for (auto& a : seen)
    {
        a.store(0, std::memory_order_relaxed);
    }

    std::atomic<bool> done{false};

    std::vector<std::thread> thief_threads;
    thief_threads.reserve(thieves);
    for (crd::u32 t = 0U; t < thieves; ++t)
    {
        thief_threads.emplace_back([&]()
        {
            while (!done.load(std::memory_order_acquire))
            {
                if (auto v = deque.steal())
                {
                    seen[*v].fetch_add(1, std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            }
            while (auto v = deque.steal())
            {
                seen[*v].fetch_add(1, std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
        });
    }

    // Owner: push all items, draining via pop when the deque approaches full.
    for (crd::u32 i = 0U; i < items; ++i)
    {
        while (deque.size() >= hi_water)
        {
            if (auto v = deque.pop())
            {
                seen[*v].fetch_add(1, std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
        }
        REQUIRE(deque.push(i));
    }

    // Drain remaining.
    while (auto v = deque.pop())
    {
        seen[*v].fetch_add(1, std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }

    done.store(true, std::memory_order_release);
    for (auto& th : thief_threads)
    {
        th.join();
    }

    for (crd::u32 i = 0U; i < items; ++i)
    {
        CHECK(seen.at(i).load(std::memory_order_relaxed) == 1);
    }
}
