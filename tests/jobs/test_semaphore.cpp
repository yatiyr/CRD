// White-box tests for crd::jobs::detail::Semaphore — the worker-sleep primitive
// that replaced std::counting_semaphore after the CI moat-test hang (2026-07-02):
// libstdc++ (GCC 13.3) lost a wake under oversubscription, leaving a worker
// futex-parked on an available token while shutdown()'s join() blocked forever.
// These tests are boundary adversaries (SANITY #3) for the sleep/wake races:
// they hammer the exact windows the protocol must close (release landing
// between a failed drain and the kernel sleep; shutdown wakes; timed waits).

#include "../../engine/jobs/src/semaphore.hpp"

#include <crd/jobs/jobs.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>

using crd::jobs::detail::Semaphore;

TEST_CASE("Semaphore: single-thread token accounting", "[jobs][semaphore]")
{
    Semaphore s;
    CHECK_FALSE(s.try_acquire_for_ms(0U)); // empty — must not block forever

    s.release(1U);
    CHECK(s.try_acquire_for_ms(0U));
    CHECK_FALSE(s.try_acquire_for_ms(0U)); // consumed

    s.release(3U);
    CHECK(s.try_acquire_for_ms(0U));
    CHECK(s.try_acquire_for_ms(0U));
    CHECK(s.try_acquire_for_ms(0U));
    CHECK_FALSE(s.try_acquire_for_ms(0U));
}

TEST_CASE("Semaphore: timed wait returns without a token", "[jobs][semaphore]")
{
    Semaphore s;
    // Must return false promptly (bounded sleep), not hang.
    CHECK_FALSE(s.try_acquire_for_ms(1U));
    // A token posted after the timeout is still consumable.
    s.release(1U);
    CHECK(s.try_acquire_for_ms(1U));
}

// The lost-wake adversary: one releaser posts tokens one at a time while
// sleepers race in and out of the kernel wait. Every posted token must be
// consumed and every acquire() must return — with the libstdc++ defect this
// shape (release landing in a waiter's pre-sleep window) eventually stranded
// a sleeper. Bounded: the test itself is the timeout (CI kills a hang).
TEST_CASE("Semaphore: no lost wake under sleep/release races", "[jobs][semaphore]")
{
    constexpr crd::u32 num_consumers = 4U;
    constexpr crd::u32 tokens_per_round = 64U;
    constexpr crd::u32 num_rounds = 50U;

    for (crd::u32 round = 0U; round < num_rounds; ++round)
    {
        Semaphore s;
        std::atomic<crd::u32> consumed{0U};

        std::thread consumers[num_consumers];
        for (crd::u32 c = 0U; c < num_consumers; ++c)
        {
            consumers[c] = std::thread(
                [&s, &consumed]
                {
                    for (;;)
                    {
                        s.acquire();
                        // A consumption whose previous count reached the work total is a stop signal.
                        if (consumed.fetch_add(1U, std::memory_order_relaxed) >= tokens_per_round)
                        {
                            return;
                        }
                    }
                });
        }

        // Post work tokens one at a time so each release races a sleeper's
        // wait-entry, then post one stop token per consumer.
        for (crd::u32 t = 0U; t < tokens_per_round; ++t)
        {
            s.release(1U);
        }
        for (crd::u32 c = 0U; c < num_consumers; ++c)
        {
            s.release(1U);
        }

        for (auto& th : consumers)
        {
            th.join(); // a lost wake hangs exactly here
        }
        CHECK(consumed.load(std::memory_order_relaxed) == tokens_per_round + num_consumers);
    }
}

// The CI-hang shape end-to-end: rapid jobs init/shutdown cycles across worker
// counts (the determinism-moat pattern) with a parallel workload right before
// shutdown, so wake_all's release races workers heading into the kernel sleep.
// The original defect hung shutdown()'s join() intermittently on exactly this.
TEST_CASE("Semaphore: jobs init/shutdown cycling does not hang (moat-pattern regression)", "[jobs][semaphore]")
{
    constexpr crd::u32 num_cycles = 40U;
    const crd::u32 counts[] = {1U, 2U, 4U, 8U, 16U};

    std::atomic<crd::u64> sink{0U};
    for (crd::u32 i = 0U; i < num_cycles; ++i)
    {
        crd::jobs::Config cfg;
        cfg.num_threads = counts[i % 5U];
        crd::jobs::init(cfg);

        crd::jobs::Counter* c = crd::jobs::parallel_for(64U, cfg.num_threads,
                                                        [&sink](crd::u32 b, crd::u32 e)
                                                        {
                                                            crd::u64 local = 0U;
                                                            for (crd::u32 k = b; k < e; ++k)
                                                            {
                                                                local += k;
                                                            }
                                                            sink.fetch_add(local, std::memory_order_relaxed);
                                                        });
        crd::jobs::wait(c);
        crd::jobs::frame_reset();

        crd::jobs::shutdown(); // the original hang: join() on a worker asleep on an available token
    }
    CHECK(sink.load(std::memory_order_relaxed) == static_cast<crd::u64>(num_cycles) * (64U * 63U / 2U));
}
