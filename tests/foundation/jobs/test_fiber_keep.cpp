// JOBS-HARD.3: the kept fiber. A thread keeps the fiber of the job it completed, still acquired from the FiberPool,
// and reuses it for its next new job of the same tier; it returns it before a resume, before a job of another tier,
// before it blocks waiting for work or leaves its dispatch loop, and when the main thread's wait() or pump returns.
// The pool's readings count the kept fiber (no other job can have it), so they stay exact, and a refusal still needs
// every other fiber running, parked or kept: the sizing bound (parked fibers + threads) is unchanged.
//
// The white-box cases drive a WorkerPool of one thread by hand (the calling thread is thread 0, nothing else
// dispatches), so each count is read at a known step. The public cases run the job system as a program does, with
// jobs pinned to threads to fix who runs what, and read jobs::pressure_snapshot(). The schedule-check twin's count of
// the pool's shared operations is in test_sched_check.cpp.

#include "../../../engine/foundation/jobs/src/worker_pool.hpp"

#include <crd/core/assert.hpp>
#include <crd/core/lifetime_ledger.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/jobs/observer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using crd::jobs::detail::Fiber;
using crd::jobs::detail::FiberTier;
using crd::jobs::detail::WorkerConfig;
using crd::jobs::detail::WorkerPool;

namespace
{
// Poll `done` for up to ten seconds; false if it never held.
template <typename Pred>
bool eventually(const Pred& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done())
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// What a white-box job saw: the fiber it ran on and the pool's acquired counts while it ran.
// `monitored` makes the job opt in to the livelock monitor (jobs::note_progress) and read its fiber's epoch.
struct Seen
{
    WorkerPool* pool      = nullptr;
    Fiber*      fiber     = nullptr;
    crd::u32    small     = 0U;
    crd::u32    medium    = 0U;
    bool        monitored = false;
    crd::u32    epoch     = 0U;
};

void see(void* data) noexcept
{
    auto* const s = static_cast<Seen*>(data);
    s->fiber      = crd::jobs::detail::tl_current_fiber_ref();
    s->small      = s->pool->fiber_pool().acquired_count(FiberTier::Small);
    s->medium     = s->pool->fiber_pool().acquired_count(FiberTier::Medium);
    if (s->monitored)
    {
        crd::jobs::note_progress();
        s->epoch = s->fiber->progress_epoch.load(std::memory_order_relaxed);
    }
}

crd::jobs::JobDecl see_job(Seen* s, crd::jobs::StackSize stack = crd::jobs::StackSize::Small)
{
    crd::jobs::JobDecl j{};
    j.fn    = &see;
    j.data  = s;
    j.stack = stack;
    return j;
}

WorkerConfig one_thread()
{
    WorkerConfig cfg;
    cfg.num_threads        = 1U; // the calling thread only: nothing dispatches unless the test pumps
    cfg.small_fiber_count  = 4U;
    cfg.medium_fiber_count = 2U;
    cfg.large_fiber_count  = 1U;
    return cfg;
}

// Asserts that fire while a case runs (Ignore, so a broken drain is a failed check, not a stopped test binary).
std::atomic<crd::u32> g_asserts{0U}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

struct CountAsserts
{
    CountAsserts()
    {
        g_asserts.store(0U, std::memory_order_relaxed);
        crd::set_assert_platform_handler(
            [](const char*) -> int
            {
                g_asserts.fetch_add(1U, std::memory_order_relaxed);
                return 0;
            });
    }
    ~CountAsserts() { crd::set_assert_platform_handler(nullptr); }
    CountAsserts(const CountAsserts&)            = delete;
    CountAsserts& operator=(const CountAsserts&) = delete;
};

// A job that spins until released, saying so first; `pressure` is read while it spins.
struct Spin
{
    std::atomic<bool> running{false};
    std::atomic<bool> release{false};
};

void spin(void* data) noexcept
{
    auto* const s = static_cast<Spin*>(data);
    s->running.store(true, std::memory_order_release);
    while (!s->release.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
}

crd::jobs::JobDecl pinned(void (*fn)(void*) noexcept, void* data, crd::i32 thread)
{
    crd::jobs::JobDecl j{};
    j.fn         = fn;
    j.data       = data;
    j.pin_thread = thread;
    return j;
}

// A parent pinned to `thread` that runs `child` and waits for it (parking its fiber), then spins on `after`.
struct Parent
{
    crd::jobs::JobDecl child{};
    Spin*              after = nullptr;
};

void parent(void* data) noexcept
{
    auto* const p = static_cast<Parent*>(data);
    crd::jobs::Counter* const c = crd::jobs::run(p->child);
    crd::jobs::wait(c);
    if (p->after != nullptr)
    {
        spin(p->after);
    }
}

void tiny(void* data) noexcept
{
    static_cast<std::atomic<crd::u32>*>(data)->fetch_add(1U, std::memory_order_relaxed);
}

crd::BudgetReading small_reading()
{
    return crd::jobs::pressure_snapshot().fibers[0];
}
} // namespace

TEST_CASE("fiber keep: a completed job's fiber stays acquired for the thread's next job until it stops iterating",
          "[jobs][fiber_keep]")
{
    WorkerPool pool;
    REQUIRE(pool.init(one_thread()));
    const auto acquired = [&pool] { return pool.fiber_pool().acquired_count(FiberTier::Small); };

    Seen first{&pool};
    Seen second{&pool};
    first.monitored = true;
    pool.push(see_job(&first));
    pool.push(see_job(&second));
    REQUIRE(pool.pump_keeping());
    CHECK(first.small == 1U);
    CHECK(acquired() == 1U); // kept: still acquired, so still counted
    CHECK(first.epoch == 1U);
    // A kept fiber reads as unmonitored, as one on the free list does: a finished monitored task is not reported.
    CHECK(first.fiber->progress_epoch.load(std::memory_order_relaxed) == 0U);
    REQUIRE(pool.pump_keeping());
    CHECK(second.fiber == first.fiber); // the next job ran on the kept fiber
    CHECK(second.small == 1U);          // ... without a second acquisition
    CHECK(acquired() == 1U);
    CHECK(pool.fiber_pool().peak_acquired(FiberTier::Small) == 1U);

    CHECK_FALSE(pool.pump_keeping()); // no job yet: a thread still iterating keeps its fiber for the next one
    CHECK(acquired() == 1U);
    pool.release_kept_fiber(0U); // ... and returns it when it stops iterating, as wait() does
    CHECK(acquired() == 0U);

    Seen third{&pool};
    pool.push(see_job(&third));
    REQUIRE(pool.pump()); // one iteration driven from outside keeps nothing past it
    CHECK(third.small == 1U);
    CHECK(acquired() == 0U);

    Seen fourth{&pool};
    pool.push(see_job(&fourth));
    REQUIRE(pool.pump_keeping());
    CHECK(acquired() == 1U);
    pool.release_kept_fiber(0U); // a caller that stops iterating returns it
    CHECK(acquired() == 0U);
    CHECK(pool.fiber_pool().available_count(FiberTier::Small) == 4U);
    pool.shutdown();
}

TEST_CASE("fiber keep: a job of another tier returns the kept fiber before it takes its own",
          "[jobs][fiber_keep]")
{
    WorkerPool pool;
    REQUIRE(pool.init(one_thread()));

    Seen small{&pool};
    Seen medium{&pool};
    Seen small_again{&pool};
    pool.push(see_job(&small));
    pool.push(see_job(&medium, crd::jobs::StackSize::Medium));
    pool.push(see_job(&small_again));
    REQUIRE(pool.pump_keeping());
    REQUIRE(pool.pump_keeping());
    CHECK(medium.small == 0U);  // the kept small fiber was not held through the medium job
    CHECK(medium.medium == 1U);
    REQUIRE(pool.pump_keeping());
    CHECK(small_again.small == 1U);
    CHECK(small_again.medium == 0U); // ... nor the kept medium fiber through the small one
    CHECK_FALSE(pool.pump_keeping());
    pool.release_kept_fiber(0U);
    CHECK(pool.fiber_pool().acquired_count(FiberTier::Small) == 0U);
    CHECK(pool.fiber_pool().acquired_count(FiberTier::Medium) == 0U);
    CHECK(pool.fiber_pool().peak_acquired(FiberTier::Small) == 1U);
    CHECK(pool.fiber_pool().peak_acquired(FiberTier::Medium) == 1U);
    pool.shutdown();
}

TEST_CASE("fiber keep: shutdown drains a kept fiber, so no fiber is left acquired and every stack is given back",
          "[jobs][fiber_keep][lifetime]")
{
    const CountAsserts          asserts;
    const crd::LifetimeSnapshot start = crd::lifetime_ledger().snapshot();
    {
        WorkerPool pool;
        REQUIRE(pool.init(one_thread()));
        Seen job{&pool};
        pool.push(see_job(&job));
        REQUIRE(pool.pump_keeping()); // thread 0 keeps the fiber and does not iterate again
        CHECK(pool.fiber_pool().acquired_count(FiberTier::Small) == 1U);
        pool.shutdown(); // FiberPool::shutdown asserts that no fiber is acquired (where asserts are compiled)
    }
    const crd::LifetimeReport after = crd::lifetime_report(start, crd::lifetime_ledger().snapshot());
    CHECK(g_asserts.load(std::memory_order_relaxed) == 0U);
    CHECK(after.verdict == crd::LifetimeVerdict::Balanced);

    // The same through the public API, shut down while the workers may still be between jobs (keeping fibers).
    crd::jobs::Config cfg;
    cfg.num_threads       = 4U;
    cfg.small_fiber_count = 8U;
    const crd::LifetimeSnapshot again = crd::lifetime_ledger().snapshot();
    for (crd::u32 round = 0U; round < 20U; ++round)
    {
        crd::jobs::init(cfg);
        std::atomic<crd::u32> ran{0U};
        crd::jobs::JobDecl    jobs[64];
        for (crd::jobs::JobDecl& j : jobs)
        {
            j      = crd::jobs::JobDecl{};
            j.fn   = &tiny;
            j.data = &ran;
        }
        crd::jobs::run_and_wait(std::span<const crd::jobs::JobDecl>(jobs));
        CHECK(ran.load(std::memory_order_relaxed) == 64U);
        crd::jobs::shutdown();
    }
    CHECK(g_asserts.load(std::memory_order_relaxed) == 0U);
    CHECK(crd::lifetime_report(again, crd::lifetime_ledger().snapshot()).verdict == crd::LifetimeVerdict::Balanced);
}

TEST_CASE("fiber keep: a resume returns the kept fiber first, so the readings hold only the fibers jobs have",
          "[jobs][fiber_keep][diag10c]")
{
    crd::jobs::Config cfg;
    cfg.num_threads       = 2U; // thread 1 runs everything; the test thread only reads
    cfg.small_fiber_count = 4U;
    crd::jobs::init(cfg);

    std::atomic<crd::u32> ran{0U};
    Spin                  after;
    Parent                p;
    p.child = pinned(&tiny, &ran, 1);
    p.after = &after;
    // Thread 1 runs the parent, which parks on its child; it then runs the child, keeps the child's fiber, and
    // resumes the parent: the kept fiber goes back before the resume, so only the parent's fiber is held.
    crd::jobs::Counter* const c = crd::jobs::run(pinned(&parent, &p, 1));
    REQUIRE(eventually([&after] { return after.running.load(std::memory_order_acquire); }));
    const crd::BudgetReading held = small_reading();
    CHECK(held.in_use == 1U);
    CHECK(held.high_water == 2U); // the parent and its child at once
    after.release.store(true, std::memory_order_release);
    crd::jobs::wait(c);
    CHECK(eventually([] { return small_reading().in_use == 0U; }));
    CHECK(small_reading().high_water == 2U);
    CHECK(small_reading().refusals == 0U);
    crd::jobs::shutdown();
}

namespace
{
// Where the parent of the cross-worker case began and ended (the observer's thread indices).
struct Track
{
    std::atomic<crd::jobs::FiberHandle> parent{nullptr};
    std::atomic<crd::i32>               began{-1};
    std::atomic<crd::i32>               ended{-1};
};
Track g_track; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void track_begin(crd::jobs::FiberHandle fiber, crd::u8 thread, crd::u8 /*priority*/, crd::u8 /*tier*/) noexcept
{
    crd::jobs::FiberHandle none = nullptr;
    if (thread == 1U && g_track.parent.compare_exchange_strong(none, fiber, std::memory_order_acq_rel))
    {
        g_track.began.store(thread, std::memory_order_release);
    }
}

void track_end(crd::jobs::FiberHandle fiber, crd::u8 thread) noexcept
{
    if (fiber == g_track.parent.load(std::memory_order_acquire))
    {
        g_track.ended.store(thread, std::memory_order_release);
    }
}
} // namespace

TEST_CASE("fiber keep: a fiber parked on one worker completes on another, which keeps it and returns it when idle",
          "[jobs][fiber_keep][diag10c]")
{
    crd::jobs::Config cfg;
    cfg.num_threads       = 4U; // the test thread never pumps until the end
    cfg.small_fiber_count = 6U;
    crd::jobs::init(cfg);
    g_track.parent.store(nullptr, std::memory_order_relaxed);
    g_track.began.store(-1, std::memory_order_relaxed);
    g_track.ended.store(-1, std::memory_order_relaxed);
    crd::jobs::JobObserver observer{};
    observer.on_job_begin = &track_begin;
    observer.on_job_end   = &track_end;
    crd::jobs::set_observer(&observer);

    // The parent runs on thread 1 and parks on a child pinned to thread 2; thread 1 is then held by a blocker, so
    // the parent's resume (a high-priority job any thread may take) is taken by thread 2 or 3.
    Spin   child;
    Parent p;
    p.child = pinned(&spin, &child, 2);
    crd::jobs::Counter* const pc = crd::jobs::run(pinned(&parent, &p, 1));
    REQUIRE(eventually([&child] { return child.running.load(std::memory_order_acquire); }));
    REQUIRE(eventually([] { return small_reading().in_use == 2U; })); // the parked parent and the child
    Spin                      blocker;
    crd::jobs::Counter* const bc = crd::jobs::run(pinned(&spin, &blocker, 1));
    REQUIRE(eventually([&blocker] { return blocker.running.load(std::memory_order_acquire); }));
    child.release.store(true, std::memory_order_release);

    // The parent completed elsewhere; its fiber is returned by the thread that completed it once that thread idles
    // (the test thread has not pumped, so it is not the one that returns it).
    REQUIRE(eventually([] { return g_track.ended.load(std::memory_order_acquire) >= 0; }));
    CHECK(g_track.began.load() == 1);
    CHECK(g_track.ended.load() != 1);
    CHECK(eventually([] { return small_reading().in_use == 1U; })); // the blocker alone
    blocker.release.store(true, std::memory_order_release);
    crd::jobs::wait(pc);
    crd::jobs::wait(bc);
    CHECK(eventually([] { return small_reading().in_use == 0U; }));
    CHECK(small_reading().high_water == 3U); // parent, child and blocker at once
    CHECK(small_reading().refusals == 0U);
    crd::jobs::set_observer(nullptr);
    crd::jobs::shutdown();
}

namespace
{
// A chain of `depth` jobs, each parked on the next, the last spinning until released (as in the pressure tests).
struct Chain;
struct Link
{
    Chain*   chain = nullptr;
    crd::u32 index = 0U;
};
struct Chain
{
    std::atomic<bool> release{false};
    std::atomic<bool> last_running{false};
    crd::u32          depth = 0U;
    Link              links[16];
};

void link_body(void* data) noexcept
{
    const auto* const l = static_cast<const Link*>(data);
    Chain* const      c = l->chain;
    if (l->index + 1U < c->depth)
    {
        crd::jobs::JobDecl j{};
        j.fn                        = &link_body;
        j.data                      = &c->links[l->index + 1U];
        crd::jobs::Counter* const k = crd::jobs::run(j);
        crd::jobs::wait(k);
        return;
    }
    c->last_running.store(true, std::memory_order_release);
    while (!c->release.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
}

// Streams rounds of tiny jobs from a worker until told to stop: the other threads keep their fibers between jobs.
struct Stream
{
    std::atomic<bool>     stop{false};
    std::atomic<crd::u32> ran{0U};
    std::atomic<crd::u32> rounds{0U};
};

void stream_body(void* data) noexcept
{
    auto* const s = static_cast<Stream*>(data);
    crd::jobs::JobDecl jobs[32];
    for (crd::jobs::JobDecl& j : jobs)
    {
        j      = crd::jobs::JobDecl{};
        j.fn   = &tiny;
        j.data = &s->ran;
    }
    while (!s->stop.load(std::memory_order_acquire))
    {
        crd::jobs::run_and_wait(std::span<const crd::jobs::JobDecl>(jobs)); // parks this fiber on each round
        s->rounds.fetch_add(1U, std::memory_order_relaxed);
    }
}
} // namespace

TEST_CASE("fiber keep: at the sizing bound a worker still gets a fiber while the others keep theirs",
          "[jobs][fiber_keep][diag10c]")
{
    // Four threads; the small tier holds exactly the bound: the fibers that may be parked at once (the chain's
    // eight links, of which the last runs, and the stream's driver) plus one for each thread.
    constexpr crd::u32 thread_count = 4U;
    constexpr crd::u32 chain_depth  = 8U;
    crd::jobs::Config  cfg;
    cfg.num_threads       = thread_count;
    cfg.small_fiber_count = (chain_depth - 1U) + 1U + thread_count;
    crd::jobs::init(cfg);

    Stream                    stream;
    crd::jobs::Counter* const sc = crd::jobs::run(crd::jobs::JobDecl{&stream_body, &stream});
    REQUIRE(eventually([&stream] { return stream.rounds.load(std::memory_order_relaxed) >= 4U; }));

    // While every other thread streams tiny jobs (and keeps a fiber between them), the chain's links each need a
    // fresh fiber on whichever thread takes them.
    Chain chain;
    chain.depth = chain_depth;
    for (crd::u32 i = 0U; i < chain_depth; ++i)
    {
        chain.links[i].chain = &chain;
        chain.links[i].index = i;
    }
    crd::jobs::Counter* const cc = crd::jobs::run(crd::jobs::JobDecl{&link_body, &chain.links[0]});
    REQUIRE(eventually([&chain] { return chain.last_running.load(std::memory_order_acquire); }));
    const crd::BudgetReading busy = small_reading();
    CHECK(busy.refusals == 0U);
    CHECK(busy.in_use >= chain_depth);
    CHECK(busy.high_water <= cfg.small_fiber_count);

    // The stream stops: its threads go idle and give their kept fibers back, leaving exactly the chain held.
    stream.stop.store(true, std::memory_order_release);
    CHECK(eventually([] { return small_reading().in_use == chain_depth; }));
    chain.release.store(true, std::memory_order_release);
    crd::jobs::wait(cc);
    crd::jobs::wait(sc);
    CHECK(eventually([] { return small_reading().in_use == 0U; }));
    CHECK(small_reading().refusals == 0U);
    CHECK(stream.ran.load(std::memory_order_relaxed) == 32U * stream.rounds.load(std::memory_order_relaxed));
    crd::jobs::shutdown();
}

namespace
{
// Frames of tiny jobs on a pool of `thread_count` threads while a reader outside the pool samples the small tier.
void stream_and_read(bool targeted)
{
    INFO("targeted wake " << targeted);
    constexpr crd::u32 thread_count = 4U;
    crd::jobs::Config  cfg;
    cfg.num_threads   = thread_count;
    cfg.pcore_routing = targeted;
    crd::jobs::init(cfg);

    // None of the jobs parks, so a thread holds at most one fiber: the one it runs on or the one it keeps between two
    // jobs.
    std::atomic<bool>     reading{true};
    std::atomic<crd::u32> most_seen{0U};
    std::atomic<crd::u32> broken{0U};
    std::atomic<crd::u32> samples{0U};
    std::thread           reader(
        [&]
        {
            while (reading.load(std::memory_order_acquire))
            {
                const crd::BudgetReading r = small_reading();
                if (r.in_use > r.high_water || r.in_use > thread_count || r.high_water > thread_count)
                {
                    broken.fetch_add(1U, std::memory_order_relaxed);
                }
                crd::u32 seen = most_seen.load(std::memory_order_relaxed);
                while (r.in_use > seen &&
                       !most_seen.compare_exchange_weak(seen, static_cast<crd::u32>(r.in_use)))
                {
                }
                samples.fetch_add(1U, std::memory_order_relaxed);
            }
        });

    std::atomic<crd::u32> ran{0U};
    crd::jobs::JobDecl    jobs[512];
    for (crd::jobs::JobDecl& j : jobs)
    {
        j      = crd::jobs::JobDecl{};
        j.fn   = &tiny;
        j.data = &ran;
    }
    for (crd::u32 frame = 0U; frame < 200U; ++frame)
    {
        crd::jobs::run_and_wait(std::span<const crd::jobs::JobDecl>(jobs));
    }
    reading.store(false, std::memory_order_release);
    reader.join();

    CHECK(ran.load(std::memory_order_relaxed) == 512U * 200U);
    CHECK(samples.load(std::memory_order_relaxed) > 0U);
    CHECK(broken.load(std::memory_order_relaxed) == 0U);
    const crd::BudgetReading idle_soon = small_reading();
    CHECK(idle_soon.high_water >= most_seen.load(std::memory_order_relaxed));
    CHECK(idle_soon.high_water >= 1U);
    CHECK(idle_soon.high_water <= thread_count);
    CHECK(eventually([] { return small_reading().in_use == 0U; })); // blocked threads give their kept fibers back
    CHECK(small_reading().refusals == 0U);
    crd::jobs::shutdown();
}
} // namespace

TEST_CASE("fiber keep: while tiny jobs stream the readings stay exact and bounded, and an idle pool holds nothing",
          "[jobs][fiber_keep][diag10c]")
{
    stream_and_read(false); // the shared semaphore
    stream_and_read(true);  // the opt-in targeted wake (ADR-0094): an idle worker parks on its own semaphore
}

namespace
{
// A short job (a few microseconds of arithmetic), so the producer's pushes stay ahead of the consumers and a
// thread's run of jobs lasts the frame on any lane.
struct Frame
{
    std::atomic<crd::u32> done{0U};
};

void short_job(void* data) noexcept
{
    volatile crd::u64 sum = 0U; // volatile: the optimizer keeps the work
    for (crd::u64 i = 0U; i < 2048U; ++i)
    {
        sum = sum + i * i;
    }
    static_cast<Frame*>(data)->done.fetch_add(1U, std::memory_order_release);
}
} // namespace

TEST_CASE("fiber keep: frames of short jobs reach the pool's shared free list only as threads start and stop",
          "[jobs][fiber_keep]")
{
    // The free list is LIFO, so a fiber's identity cannot tell a kept fiber from one released and acquired again on
    // the same thread; the pool's own tally of acquisitions from the list can.
    constexpr crd::u32 job_count   = 512U;
    constexpr crd::u32 frame_count = 50U;
    WorkerPool         pool;
    WorkerConfig       cfg;
    cfg.num_threads = 4U;
    REQUIRE(pool.init(cfg));
    Frame frame;
    for (crd::u32 f = 0U; f < frame_count; ++f)
    {
        for (crd::u32 i = 0U; i < job_count; ++i)
        {
            crd::jobs::JobDecl j{};
            j.fn   = &short_job;
            j.data = &frame;
            pool.push(j);
        }
        // The calling thread helps, as wait() does, until the frame's jobs are done.
        while (frame.done.load(std::memory_order_acquire) < (f + 1U) * job_count)
        {
            if (!pool.pump_keeping())
            {
                std::this_thread::yield();
            }
        }
        pool.release_kept_fiber(0U); // as wait() does when its counter is done
    }
    CHECK(eventually([&pool] { return pool.fiber_pool().acquired_count(FiberTier::Small) == 0U; }));
    const crd::u64 acquisitions = pool.fiber_pool().acquisitions(FiberTier::Small);
    INFO("free-list acquisitions " << acquisitions << " for " << job_count * frame_count << " jobs");
    CHECK(acquisitions >= 1U);
    // A thread acquires when its run of jobs starts and returns the fiber when it idles: about one a thread a frame,
    // against one a job without the kept fiber. At least eight jobs an acquisition is the bound checked.
    CHECK(acquisitions * 8U <= crd::u64{job_count} * frame_count);
    CHECK(pool.fiber_pool().available_count(FiberTier::Small) == 128U);
    pool.shutdown();
}

TEST_CASE("fiber keep: the main thread's pumps return its kept fiber when they stop", "[jobs][fiber_keep][diag10c]")
{
    crd::jobs::Config cfg;
    cfg.num_threads       = 1U; // the calling thread runs every job, only when it pumps
    cfg.small_fiber_count = 2U;
    crd::jobs::init(cfg);

    std::atomic<crd::u32> ran{0U};
    crd::jobs::JobDecl    jobs[3];
    for (crd::jobs::JobDecl& j : jobs)
    {
        j      = crd::jobs::JobDecl{};
        j.fn   = &tiny;
        j.data = &ran;
    }
    crd::jobs::Counter* const all = crd::jobs::run(std::span<const crd::jobs::JobDecl>(jobs));
    CHECK(crd::jobs::pump_main_thread_until_idle()); // runs all three on one kept fiber, then returns it
    CHECK(ran.load(std::memory_order_relaxed) == 3U);
    CHECK(small_reading().in_use == 0U);
    CHECK(small_reading().high_water == 1U);
    crd::jobs::Counter* const one = crd::jobs::run(jobs[0]);
    CHECK(crd::jobs::pump_main_thread_once());
    CHECK(small_reading().in_use == 0U); // one iteration keeps nothing past it
    crd::jobs::wait(all);
    crd::jobs::wait(one);
    CHECK(ran.load(std::memory_order_relaxed) == 4U);
    crd::jobs::shutdown();
}
