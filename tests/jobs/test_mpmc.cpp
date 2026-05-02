#include <catch2/catch_test_macros.hpp>

#include "../../engine/jobs/src/mpmc_queue.hpp"
#include <crd/core/types.hpp>

#include <atomic>
#include <thread>
#include <vector>

using crd::jobs::detail::MpmcQueue;

// ---------------------------------------------------------------------------
// 1. Construction: capacity and empty initialised correctly.
// ---------------------------------------------------------------------------

TEST_CASE("mpmc_queue: construction", "[jobs][mpmc]")
{
    const MpmcQueue<crd::u32> q8(8U);
    CHECK(q8.capacity() == 8U);
    CHECK(q8.empty());

    const MpmcQueue<crd::u32> q1024(1024U);
    CHECK(q1024.capacity() == 1024U);
    CHECK(q1024.empty());
}

// ---------------------------------------------------------------------------
// 2. Enqueue and dequeue single item.
// ---------------------------------------------------------------------------

TEST_CASE("mpmc_queue: enqueue and dequeue single item", "[jobs][mpmc]")
{
    MpmcQueue<crd::u32> q(8U);

    REQUIRE(q.enqueue(42U));
    CHECK_FALSE(q.empty());

    crd::u32 val{0U};
    REQUIRE(q.dequeue(val));
    CHECK(val == 42U);
    CHECK(q.empty());
}

// ---------------------------------------------------------------------------
// 3. Dequeue on empty returns false without modifying the output parameter.
// ---------------------------------------------------------------------------

TEST_CASE("mpmc_queue: dequeue empty returns false", "[jobs][mpmc]")
{
    MpmcQueue<crd::u32> q(4U);

    crd::u32 val{0xDEADBEEFU};
    CHECK_FALSE(q.dequeue(val));
    // Output parameter must not be written on failure.
    CHECK(val == 0xDEADBEEFU);

    // Multiple dequeues on empty must all return false.
    CHECK_FALSE(q.dequeue(val));
    CHECK_FALSE(q.dequeue(val));
}

// ---------------------------------------------------------------------------
// 4. Enqueue on full returns false.
// ---------------------------------------------------------------------------

TEST_CASE("mpmc_queue: enqueue full returns false", "[jobs][mpmc]")
{
    static constexpr crd::u32 kCap = 4U;
    MpmcQueue<crd::u32> q(kCap);

    for (crd::u32 i = 0U; i < kCap; ++i)
    {
        REQUIRE(q.enqueue(i));
    }
    CHECK_FALSE(q.enqueue(99U)); // full

    // Drain.
    for (crd::u32 i = 0U; i < kCap; ++i)
    {
        crd::u32 v;
        REQUIRE(q.dequeue(v));
        CHECK(v == i);
    }
    CHECK(q.empty());
}

// ---------------------------------------------------------------------------
// 5. FIFO ordering: items come out in the same order they went in.
// ---------------------------------------------------------------------------

TEST_CASE("mpmc_queue: FIFO ordering single-threaded", "[jobs][mpmc]")
{
    static constexpr crd::u32 kN = 6U;
    MpmcQueue<crd::u32> q(16U);

    for (crd::u32 i = 0U; i < kN; ++i)
    {
        REQUIRE(q.enqueue(i));
    }

    for (crd::u32 i = 0U; i < kN; ++i)
    {
        crd::u32 val;
        REQUIRE(q.dequeue(val));
        CHECK(val == i);
    }
    CHECK(q.empty());
}

// ---------------------------------------------------------------------------
// 6. Wrap-around: enqueue and dequeue across multiple full laps of the ring.
//    Exercises sequence arithmetic beyond the first capacity iteration.
// ---------------------------------------------------------------------------

TEST_CASE("mpmc_queue: wrap-around across full cycles", "[jobs][mpmc]")
{
    static constexpr crd::u32 kCap    = 4U;
    static constexpr crd::u32 kPasses = 3U;

    MpmcQueue<crd::u32> q(kCap);

    for (crd::u32 pass = 0U; pass < kPasses; ++pass)
    {
        for (crd::u32 i = 0U; i < kCap; ++i)
        {
            REQUIRE(q.enqueue(pass * kCap + i));
        }
        // Queue must be exactly full.
        CHECK_FALSE(q.enqueue(0xFFFFU));

        for (crd::u32 i = 0U; i < kCap; ++i)
        {
            crd::u32 val{0U};
            REQUIRE(q.dequeue(val));
            CHECK(val == pass * kCap + i);
        }
        // Queue must be exactly empty.
        crd::u32 dummy{0U};
        CHECK_FALSE(q.dequeue(dummy));
    }
}

// ---------------------------------------------------------------------------
// 7. SPSC stress: one producer, one consumer, 10 000 items.
//    All items must be consumed exactly once.
// ---------------------------------------------------------------------------

TEST_CASE("mpmc_queue: SPSC concurrent stress", "[jobs][mpmc][stress]")
{
    static constexpr crd::u32 kCap        = 256U;
    static constexpr crd::u32 kTotalItems = 10000U;

    MpmcQueue<crd::u32> q(kCap);

    std::vector<std::atomic<int>> seen(kTotalItems);
    for (auto& a : seen)
    {
        a.store(0, std::memory_order_relaxed);
    }

    std::atomic<crd::u32> remaining{kTotalItems};

    std::thread consumer([&]()
    {
        while (remaining.load(std::memory_order_acquire) > 0U)
        {
            crd::u32 val;
            if (q.dequeue(val))
            {
                seen[val].fetch_add(1, std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                remaining.fetch_sub(1U, std::memory_order_acq_rel);
            }
        }
    });

    for (crd::u32 i = 0U; i < kTotalItems; ++i)
    {
        while (!q.enqueue(i)) { /* spin: queue full */ }
    }

    consumer.join();

    for (crd::u32 i = 0U; i < kTotalItems; ++i)
    {
        CHECK(seen.at(i).load(std::memory_order_relaxed) == 1);
    }
}

// ---------------------------------------------------------------------------
// 8. MPSC stress: 4 producers, 1 consumer, 10 000 items total.
//    Each producer owns a disjoint value range; every item is consumed once.
// ---------------------------------------------------------------------------

TEST_CASE("mpmc_queue: MPSC concurrent stress", "[jobs][mpmc][stress]")
{
    static constexpr crd::u32 kCap          = 256U;
    static constexpr crd::u32 kNumProducers = 4U;
    static constexpr crd::u32 kPerProducer  = 2500U;
    static constexpr crd::u32 kTotalItems   = kNumProducers * kPerProducer;

    MpmcQueue<crd::u32> q(kCap);

    std::vector<std::atomic<int>> seen(kTotalItems);
    for (auto& a : seen)
    {
        a.store(0, std::memory_order_relaxed);
    }

    // Single consumer: loops until it has consumed exactly kTotalItems items.
    std::thread consumer([&]()
    {
        crd::u32 consumed = 0U;
        while (consumed < kTotalItems)
        {
            crd::u32 val;
            if (q.dequeue(val))
            {
                seen[val].fetch_add(1, std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                ++consumed;
            }
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(kNumProducers);
    for (crd::u32 p = 0U; p < kNumProducers; ++p)
    {
        producers.emplace_back([&, p]()
        {
            const crd::u32 start = p * kPerProducer;
            const crd::u32 end   = start + kPerProducer;
            for (crd::u32 i = start; i < end; ++i)
            {
                while (!q.enqueue(i)) { /* spin: queue full */ }
            }
        });
    }

    for (auto& t : producers)
    {
        t.join();
    }
    consumer.join();

    for (crd::u32 i = 0U; i < kTotalItems; ++i)
    {
        CHECK(seen.at(i).load(std::memory_order_relaxed) == 1);
    }
}

// ---------------------------------------------------------------------------
// 9. SPMC stress: 1 producer, 4 consumers, 10 000 items.
//    Every item must be consumed exactly once.
// ---------------------------------------------------------------------------

TEST_CASE("mpmc_queue: SPMC concurrent stress", "[jobs][mpmc][stress]")
{
    static constexpr crd::u32 kCap          = 256U;
    static constexpr crd::u32 kNumConsumers = 4U;
    static constexpr crd::u32 kTotalItems   = 10000U;

    MpmcQueue<crd::u32> q(kCap);

    std::vector<std::atomic<int>> seen(kTotalItems);
    for (auto& a : seen)
    {
        a.store(0, std::memory_order_relaxed);
    }

    // remaining tracks items not yet dequeued. Consumers exit when it hits 0.
    std::atomic<crd::u32> remaining{kTotalItems};

    std::vector<std::thread> consumers;
    consumers.reserve(kNumConsumers);
    for (crd::u32 c = 0U; c < kNumConsumers; ++c)
    {
        consumers.emplace_back([&]()
        {
            while (remaining.load(std::memory_order_acquire) > 0U)
            {
                crd::u32 val;
                if (q.dequeue(val))
                {
                    seen[val].fetch_add(1, std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                    remaining.fetch_sub(1U, std::memory_order_acq_rel);
                }
            }
        });
    }

    for (crd::u32 i = 0U; i < kTotalItems; ++i)
    {
        while (!q.enqueue(i)) { /* spin: queue full */ }
    }

    for (auto& t : consumers)
    {
        t.join();
    }

    for (crd::u32 i = 0U; i < kTotalItems; ++i)
    {
        CHECK(seen.at(i).load(std::memory_order_relaxed) == 1);
    }
}

// ---------------------------------------------------------------------------
// 10. MPMC stress: 4 producers × 4 consumers, 10 000 items.
//     Exercises concurrent enqueue and concurrent dequeue simultaneously.
//     Every item must be consumed exactly once.
// ---------------------------------------------------------------------------

TEST_CASE("mpmc_queue: MPMC concurrent stress", "[jobs][mpmc][stress]")
{
    static constexpr crd::u32 kCap          = 256U;
    static constexpr crd::u32 kNumProducers = 4U;
    static constexpr crd::u32 kNumConsumers = 4U;
    static constexpr crd::u32 kPerProducer  = 2500U;
    static constexpr crd::u32 kTotalItems   = kNumProducers * kPerProducer;

    MpmcQueue<crd::u32> q(kCap);

    std::vector<std::atomic<int>> seen(kTotalItems);
    for (auto& a : seen)
    {
        a.store(0, std::memory_order_relaxed);
    }

    std::atomic<crd::u32> remaining{kTotalItems};

    std::vector<std::thread> consumers;
    consumers.reserve(kNumConsumers);
    for (crd::u32 c = 0U; c < kNumConsumers; ++c)
    {
        consumers.emplace_back([&]()
        {
            while (remaining.load(std::memory_order_acquire) > 0U)
            {
                crd::u32 val;
                if (q.dequeue(val))
                {
                    seen[val].fetch_add(1, std::memory_order_relaxed); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                    remaining.fetch_sub(1U, std::memory_order_acq_rel);
                }
            }
        });
    }

    std::vector<std::thread> producers;
    producers.reserve(kNumProducers);
    for (crd::u32 p = 0U; p < kNumProducers; ++p)
    {
        producers.emplace_back([&, p]()
        {
            const crd::u32 start = p * kPerProducer;
            const crd::u32 end   = start + kPerProducer;
            for (crd::u32 i = start; i < end; ++i)
            {
                while (!q.enqueue(i)) { /* spin: queue full */ }
            }
        });
    }

    for (auto& t : producers)
    {
        t.join();
    }
    for (auto& t : consumers)
    {
        t.join();
    }

    for (crd::u32 i = 0U; i < kTotalItems; ++i)
    {
        CHECK(seen.at(i).load(std::memory_order_relaxed) == 1);
    }
}
