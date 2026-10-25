// JOBS-HARD.3: one wake per batch of submitted jobs. jobs::run of a span pushes its jobs without a wake each and posts
// at most one token per thread with one wake (Scheduler::wake_for), after its first injected job and after its last.
// A token is a wake hint that only an idle worker consumes, and a woken worker runs jobs until the queues are empty:
// a token per job, as one push() each posts, left a frame of 2048 tiny jobs up to 2048 tokens for idle workers to
// spin through, an empty scan of every queue each, and cost the producer a wake per job (a futex syscall on Linux).
// With the kept fiber that spin made linux-gcc-shipping's off mode 25% slower at four threads; with the batch wake it
// is 46% faster than before both.

#include "../../../engine/foundation/jobs/src/scheduler.hpp"

#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <span>
#include <thread>

using crd::jobs::detail::Scheduler;
using crd::jobs::detail::SchedulerConfig;

namespace
{
void noop(void* /*data*/) noexcept
{
}

crd::jobs::JobDecl job(crd::i32 pin = -1)
{
    crd::jobs::JobDecl j{};
    j.fn         = &noop;
    j.pin_thread = pin;
    return j;
}

// Jobs that meet: each arrives and waits (bounded) until all of them have, so they finish only when that many threads
// ran them at once.
struct Rendezvous
{
    std::atomic<crd::u32> arrived{0U};
    std::atomic<crd::u32> timed_out{0U};
    crd::u32              expected = 0U;
};

void meet(void* data) noexcept
{
    auto* const r        = static_cast<Rendezvous*>(data);
    const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    r->arrived.fetch_add(1U, std::memory_order_acq_rel);
    while (r->arrived.load(std::memory_order_acquire) < r->expected)
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            r->timed_out.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        std::this_thread::yield();
    }
}
} // namespace

TEST_CASE("batch wake: a batch posts at most one token per thread, a push one per job", "[jobs][scheduler][batch_wake]")
{
    Scheduler       sched;
    SchedulerConfig cfg;
    cfg.num_threads        = 4U;
    cfg.injection_capacity = 1024U;
    REQUIRE(sched.init(cfg));

    for (crd::u32 i = 0U; i < 100U; ++i)
    {
        REQUIRE(sched.push_unwoken(job()));
    }
    CHECK(sched.wake_tokens() == 0U); // pushed without a wake each
    sched.wake_for(100U);
    CHECK(sched.wake_tokens() == 4U); // one token per thread wakes every worker the batch can use
    sched.wake_for(2U);
    CHECK(sched.wake_tokens() == 6U); // a smaller batch posts one token per job
    sched.wake_for(0U);
    CHECK(sched.wake_tokens() == 6U);
    sched.push(job());
    CHECK(sched.wake_tokens() == 7U); // a single push still posts its own token
    CHECK_FALSE(sched.push_unwoken(job(2))); // a pinned job wakes its thread itself (a broadcast token)
    CHECK(sched.wake_tokens() == 8U);

    crd::u32 popped = 0U;
    while (sched.try_pop(0U).has_value() || sched.try_pop(2U).has_value())
    {
        ++popped;
    }
    CHECK(popped == 102U);
    sched.shutdown();
}

TEST_CASE("batch wake: a batch wakes every sleeping worker it can use", "[jobs][batch_wake]")
{
    constexpr crd::u32 thread_count = 4U;
    crd::jobs::Config  cfg;
    cfg.num_threads = thread_count;
    crd::jobs::init(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the workers run dry and sleep

    for (crd::u32 round = 0U; round < 20U; ++round)
    {
        // Four jobs that only finish together: the test thread runs one while it waits, so the batch's wake must
        // bring all three sleeping workers.
        Rendezvous         r;
        crd::jobs::JobDecl jobs[thread_count];
        r.expected = thread_count;
        for (crd::jobs::JobDecl& j : jobs)
        {
            j      = crd::jobs::JobDecl{};
            j.fn   = &meet;
            j.data = &r;
        }
        crd::jobs::run_and_wait(std::span<const crd::jobs::JobDecl>(jobs));
        INFO("round " << round);
        CHECK(r.timed_out.load(std::memory_order_relaxed) == 0U);
        CHECK(r.arrived.load(std::memory_order_relaxed) == thread_count);
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // asleep again before the next batch
    }
    crd::jobs::shutdown();
}
