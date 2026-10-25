// JOBS-HARD.3 / DIAG.10c: the snapshots across a pool restart. Any thread may call progress_snapshot,
// pressure_snapshot, lane_snapshot, wait_graph_snapshot, monitored_snapshot, worker_snapshot and is_quiescent at any
// time, also while another thread runs init() or shutdown(): a snapshot pins itself against the teardown and reads
// nothing while the pool is closed (jobs.cpp, g_snapshot_open). Before the pin, the guard was the pool's plain
// is_initialized(), which shutdown() clears only after freeing the storage the snapshots walk, so a sampler racing a
// restart read freed memory: a use-after-free under ASan, wrong capacities or a crash without it.

#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <span>
#include <thread>

namespace
{
constexpr crd::u32 kSmall    = 8U; // NOLINT(readability-identifier-naming): file-scope constant
constexpr crd::u32 kMedium   = 4U; // NOLINT(readability-identifier-naming)
constexpr crd::u32 kLarge    = 2U; // NOLINT(readability-identifier-naming)
constexpr crd::u32 kCounters = 64U; // NOLINT(readability-identifier-naming)

void tiny(void* data) noexcept
{
    static_cast<std::atomic<crd::u32>*>(data)->fetch_add(1U, std::memory_order_relaxed);
}

// What the sampler saw: readings taken while the pool was open, readings refused while it was closed, and readings
// that could only come from freed or foreign memory.
struct Sampler
{
    std::atomic<bool>     run{true};
    std::atomic<crd::u64> read{0U};
    std::atomic<crd::u64> unread{0U};
    std::atomic<crd::u64> wrong{0U};
};

void sample(Sampler& s) noexcept
{
    crd::jobs::WaitGraphNode parked[kSmall + kMedium + kLarge];
    crd::jobs::ProgressNode  monitored[kSmall + kMedium + kLarge];
    crd::jobs::WorkerNode    workers[8];
    crd::u64                 pass = 0U;
    while (s.run.load(std::memory_order_acquire))
    {
        const crd::jobs::JobsPressure p = crd::jobs::pressure_snapshot();
        if (p.counters.read)
        {
            s.read.fetch_add(1U, std::memory_order_relaxed);
            const bool sizes = p.counters.capacity == kCounters && p.fibers[0].capacity == kSmall &&
                               p.fibers[1].capacity == kMedium && p.fibers[2].capacity == kLarge;
            const bool bounded = p.fibers[0].in_use <= kSmall && p.fibers[0].high_water <= kSmall &&
                                 p.counters.in_use <= kCounters;
            if (!sizes || !bounded)
            {
                s.wrong.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        else
        {
            s.unread.fetch_add(1U, std::memory_order_relaxed);
        }
        const crd::jobs::ProgressSample progress = crd::jobs::progress_snapshot();
        const crd::jobs::LaneSample     lanes    = crd::jobs::lane_snapshot();
        const crd::usize parked_count    = crd::jobs::wait_graph_snapshot(std::span<crd::jobs::WaitGraphNode>(parked));
        const crd::usize monitored_count = crd::jobs::monitored_snapshot(std::span<crd::jobs::ProgressNode>(monitored));
        // The worker snapshot polls for acknowledgements (up to 1 ms here), so it runs on every 256th pass.
        const crd::jobs::WorkerSnapshotResult w =
            (++pass % 256U) == 0U ? crd::jobs::worker_snapshot(std::span<crd::jobs::WorkerNode>(workers), 1U)
                                  : crd::jobs::WorkerSnapshotResult{0U, 0U, 0U, true};
        (void)crd::jobs::is_quiescent();
        if (progress.outstanding > kCounters || parked_count > kSmall + kMedium + kLarge ||
            monitored_count > kSmall + kMedium + kLarge || w.total > 8U || lanes.backlog[1] > 4096U)
        {
            s.wrong.fetch_add(1U, std::memory_order_relaxed);
        }
    }
}
} // namespace

TEST_CASE("diag10c: a sampler outside the pool reads every snapshot safely across init and shutdown",
          "[jobs][diag][diag10c][snapshot_restart]")
{
    Sampler     s;
    std::thread sampler([&s] { sample(s); });

    crd::jobs::Config cfg;
    cfg.num_threads        = 3U;
    cfg.small_fiber_count  = kSmall;
    cfg.medium_fiber_count = kMedium;
    cfg.large_fiber_count  = kLarge;
    cfg.max_counters       = kCounters;
    std::atomic<crd::u32> ran{0U};
    crd::jobs::JobDecl    jobs[32];
    for (crd::jobs::JobDecl& j : jobs)
    {
        j      = crd::jobs::JobDecl{};
        j.fn   = &tiny;
        j.data = &ran;
    }
    constexpr crd::u32 cycles = 100U;
    for (crd::u32 cycle = 0U; cycle < cycles; ++cycle)
    {
        crd::jobs::init(cfg);
        for (crd::u32 frame = 0U; frame < 20U; ++frame) // long enough for the sampler to catch the pool open
        {
            crd::jobs::run_and_wait(std::span<const crd::jobs::JobDecl>(jobs));
        }
        crd::jobs::shutdown();
    }
    s.run.store(false, std::memory_order_release);
    sampler.join();

    INFO("read " << s.read.load() << ", unread " << s.unread.load() << ", wrong " << s.wrong.load());
    CHECK(ran.load(std::memory_order_relaxed) == 32U * 20U * cycles);
    CHECK(s.wrong.load(std::memory_order_relaxed) == 0U);
    CHECK(s.read.load(std::memory_order_relaxed) > 0U);   // it sampled running pools
    CHECK(s.unread.load(std::memory_order_relaxed) > 0U); // and closed ones, between them
    // After the last shutdown every snapshot reads nothing.
    CHECK_FALSE(crd::jobs::pressure_snapshot().counters.read);
    CHECK(crd::jobs::lane_snapshot().pops[1] == 0U);
}
