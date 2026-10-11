// DIAG.10c -- warm/cold, soak and shutdown workloads over the job system, one changed stressor at a time.
// Declared contract (docs/design/runtime-diagnostics.md, DIAG.10c): crd::run_workload drives cycles of jobs::init,
// many parallel sums with a known answer, and jobs::shutdown. Every iteration's sum must be the closed-form value
// (cold or warm, first cycle or last); each cycle's lifetime-ledger window, from before init to after shutdown, must
// balance, worker threads and the watchdog included; every worker thread the OS saw running during a cycle must be
// gone once that cycle has shut down; and progress must never stall. A variant that changes the thread count stays
// clean (nothing to attribute); a variant that injects one long delay stalls, and the stall is attributed to the delay
// and to nothing else. Wall-clock timing is recorded as evidence, never asserted: hosted lanes judge progress, results
// and lifetimes, not speed.

#include <crd/core/lifetime_ledger.hpp>
#include <crd/core/os_resource_probe.hpp>
#include <crd/core/workload_probe.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <span>
#include <thread>

namespace
{
using crd::FindingAttribution;
using crd::LifetimeClass;
using crd::LifetimeVerdict;
using crd::OsPresence;
using crd::ProgressVerdict;
using crd::Stressor;
using crd::WorkloadVerdict;

constexpr crd::u32 kChunks     = 16U;
constexpr crd::u32 kChunkItems = 1024U;
constexpr crd::u32 kMaxThreads = 8U;
constexpr crd::u32 kMaxCycles  = 4U;
constexpr crd::u64 kSecond     = 1000ULL * 1000ULL * 1000ULL;

// The sum of i * i for i in [0, n): (n - 1) n (2n - 1) / 6.
constexpr crd::u64 squares_below(crd::u64 n)
{
    return (n - 1U) * n * (2U * n - 1U) / 6U;
}

struct Chunk
{
    crd::u64 begin = 0U;
    crd::u64 sum   = 0U;
};

void sum_chunk(void* data) noexcept
{
    auto* c = static_cast<Chunk*>(data);
    c->sum  = 0U;
    for (crd::u64 i = c->begin; i < c->begin + kChunkItems; ++i)
    {
        c->sum += i * i;
    }
}

void record_identity(void* data) noexcept
{
    *static_cast<crd::OsThreadIdentity*>(data) = crd::os_current_thread_identity();
}

struct JobsWorkload
{
    crd::u32 threads         = 2U;
    crd::u32 delay_cycle     = ~0U;
    crd::u32 delay_iteration = ~0U;
    crd::u32 delay_ms        = 0U;

    crd::LifetimeSnapshot before_init;
    crd::i64              threads_held[kMaxCycles]{}; // Thread live change in each cycle's first iteration
    crd::OsThreadIdentity workers[kMaxCycles][kMaxThreads]{};
    crd::u32              present_while_running = 0U;
    Chunk                 chunks[kChunks]{};
};

bool jobs_setup(void* user, crd::u32 /*cycle*/) noexcept
{
    auto* w        = static_cast<JobsWorkload*>(user);
    w->before_init = crd::lifetime_ledger().snapshot();
    crd::jobs::Config c;
    c.num_threads             = w->threads;
    c.small_fiber_count       = 32U;
    c.medium_fiber_count      = 2U;
    c.large_fiber_count       = 1U;
    c.hang_watchdog_period_ms = 50U; // the watchdog thread is part of what a shutdown must give back
    crd::jobs::init(c);
    return true;
}

void jobs_iterate(void* user, crd::u32 cycle, crd::u32 iteration, bool /*injected*/,
                  crd::WorkloadIteration& out) noexcept
{
    auto* w = static_cast<JobsWorkload*>(user);
    if (iteration == 0U && cycle < kMaxCycles)
    {
        // Cold: ask each worker who it is, and ask the OS whether that thread runs.
        w->threads_held[cycle] = crd::lifetime_report(w->before_init, crd::lifetime_ledger().snapshot())
                                     .live_change(LifetimeClass::Thread);
        for (crd::u32 t = 1U; t < w->threads && t < kMaxThreads; ++t)
        {
            crd::jobs::JobDecl d;
            d.fn         = &record_identity;
            d.data       = &w->workers[cycle][t];
            d.pin_thread = static_cast<crd::i32>(t);
            crd::jobs::wait(crd::jobs::run(d));
            if (crd::os_thread_alive(w->workers[cycle][t]).presence == OsPresence::Present)
            {
                ++w->present_while_running;
            }
        }
    }
    if (cycle == w->delay_cycle && iteration == w->delay_iteration)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(w->delay_ms));
    }
    crd::jobs::JobDecl decls[kChunks]{};
    for (crd::u32 k = 0U; k < kChunks; ++k)
    {
        w->chunks[k].begin = static_cast<crd::u64>(k) * kChunkItems;
        decls[k].fn        = &sum_chunk;
        decls[k].data      = &w->chunks[k];
    }
    crd::jobs::wait(crd::jobs::run(std::span<const crd::jobs::JobDecl>(decls, kChunks)));
    crd::u64 total = 0U;
    for (const Chunk& c : w->chunks)
    {
        total += c.sum;
    }
    out.ok             = true;
    out.signature      = total;
    out.expected       = squares_below(static_cast<crd::u64>(kChunks) * kChunkItems);
    out.expected_known = true;
    out.progress       = kChunks;
}

void jobs_teardown(void* /*user*/, crd::u32 /*cycle*/) noexcept
{
    crd::jobs::shutdown();
}

crd::WorkloadSpec jobs_spec(JobsWorkload& w, crd::u32 cycles, crd::u32 iterations)
{
    crd::WorkloadSpec spec;
    spec.name       = "jobs-soak-shutdown";
    spec.cycles     = cycles;
    spec.iterations = iterations;
    spec.user       = &w;
    spec.setup      = &jobs_setup;
    spec.iterate    = &jobs_iterate;
    spec.teardown   = &jobs_teardown;
    spec.stall_ns   = 5U * kSecond; // far above an iteration's or an init/shutdown's time on any runner
    spec.stressors.set(Stressor::Cycles, cycles);
    spec.stressors.set(Stressor::Iterations, iterations);
    spec.stressors.set(Stressor::Threads, w.threads);
    spec.stressors.set(Stressor::Delay, static_cast<crd::u64>(w.delay_ms) * 1000ULL * 1000ULL);
    return spec;
}

crd::WorkloadResult run(const crd::WorkloadSpec& spec)
{
    crd::WorkloadResult r;
    crd::run_workload(spec, r);
    char line[512];
    (void)crd::format_workload_result(spec.name, r, line, sizeof(line));
    WARN(line); // evidence: the timing verdict is printed on every run and judged by nothing
    return r;
}

// The checks every clean jobs run must pass, whatever its stressors.
void check_clean(const crd::WorkloadResult& r, const JobsWorkload& w, crd::u32 cycles, crd::u32 iterations)
{
    CHECK(r.verdict == WorkloadVerdict::Clean);
    CHECK(r.cycles_run == cycles);
    CHECK(r.iterations_run == static_cast<crd::u64>(cycles) * iterations);
    CHECK(r.diverged == 0U);
    CHECK(r.worst_cycle.verdict == LifetimeVerdict::Balanced);
    CHECK(r.progress.verdict == ProgressVerdict::Progressing);
    CHECK(r.progress.done == static_cast<crd::u64>(cycles) * iterations * kChunks);
    // Measured, whatever it measured: both windows hold every warm iteration but the cold ones.
    CHECK(r.warm_count == cycles * (iterations - 1U));
    CHECK(r.slowdown.baseline.samples == r.warm_count / 2U);
    for (crd::u32 c = 0U; c < cycles; ++c)
    {
        INFO("cycle " << c);
        CHECK(w.threads_held[c] == static_cast<crd::i64>(w.threads)); // threads - 1 workers and the watchdog
        for (crd::u32 t = 1U; t < w.threads; ++t)
        {
            INFO("worker " << t);
            REQUIRE(w.workers[c][t].known);
            CHECK(crd::os_thread_alive(w.workers[c][t]).presence == OsPresence::Absent);
        }
    }
    CHECK(w.present_while_running == cycles * (w.threads - 1U));
}
} // namespace

TEST_CASE("diag10c: a jobs soak across init/shutdown cycles keeps its result, its threads and its progress",
          "[jobs][workload][lifetime][diag10c]")
{
    constexpr crd::u32 cycles     = 3U;
    constexpr crd::u32 iterations = 64U;

    JobsWorkload              base;
    const crd::WorkloadResult baseline = run(jobs_spec(base, cycles, iterations));
    check_clean(baseline, base, cycles, iterations);
    CHECK(baseline.reference == 0U); // every iteration states its expected sum, so no cold reference is taken

    // One changed stressor: more threads. Still clean, so there is nothing to attribute.
    JobsWorkload wide;
    wide.threads                      = 4U;
    const crd::WorkloadResult variant = run(jobs_spec(wide, cycles, iterations));
    check_clean(variant, wide, cycles, iterations);
    const crd::AttributionReport a = crd::attribute_finding(baseline, variant);
    CHECK(a.comparison.changed == Stressor::Threads);
    CHECK(a.attribution == FindingAttribution::NoNewFinding);
}

TEST_CASE("diag10c: one injected delay longer than the progress bound stalls the jobs workload and is blamed",
          "[jobs][workload][diag10c]")
{
    constexpr crd::u32 cycles     = 2U;
    constexpr crd::u32 iterations = 6U;

    JobsWorkload              base;
    const crd::WorkloadResult baseline = run(jobs_spec(base, cycles, iterations));
    REQUIRE(baseline.verdict == WorkloadVerdict::Clean);

    JobsWorkload slow;
    slow.delay_cycle     = 1U;
    slow.delay_iteration = 3U;
    slow.delay_ms        = 7000U; // two seconds past the five-second bound
    const crd::WorkloadResult variant = run(jobs_spec(slow, cycles, iterations));
    CHECK(variant.verdict == WorkloadVerdict::Stalled);
    CHECK(variant.progress.verdict == ProgressVerdict::Stalled);
    CHECK(variant.progress.longest_gap >= 7000ULL * 1000ULL * 1000ULL);
    CHECK(variant.diverged == 0U); // slow, not wrong
    CHECK(variant.worst_cycle.verdict == LifetimeVerdict::Balanced);

    const crd::AttributionReport a = crd::attribute_finding(baseline, variant);
    CHECK(a.attribution == FindingAttribution::Attributed);
    CHECK(a.stressor == Stressor::Delay);
    CHECK(a.finding == WorkloadVerdict::Stalled);
}

namespace
{
void mark_ran(void* data) noexcept
{
    static_cast<std::atomic<crd::u32>*>(data)->fetch_add(1U, std::memory_order_release);
}
} // namespace

// Found by the soak above (2026-10-11): on the default (shared-semaphore) wake path a job pinned to a worker woke one
// arbitrary sleeper; when that was another worker, it took the token, found nothing it could run and slept again, and
// the target slept on with the job in its lane. Each push here targets a worker after every worker has gone idle, and
// waits for it with a deadline instead of jobs::wait, so a lost wake fails the case rather than hanging it (a worker
// snapshot, which wakes every sleeper, then releases the stranded job).
TEST_CASE("diag10c: a job pinned to an idle worker runs on the default wake path", "[jobs][workload][diag10c]")
{
    crd::jobs::Config c;
    c.num_threads        = 8U;
    c.small_fiber_count  = 32U;
    c.medium_fiber_count = 2U;
    c.large_fiber_count  = 1U;
    crd::jobs::init(c);
    REQUIRE_FALSE(crd::jobs::is_pcore_routing());

    crd::u32 lost = 0U;
    for (crd::u32 round = 0U; round < 8U && lost == 0U; ++round)
    {
        for (crd::u32 t = 1U; t < c.num_threads && lost == 0U; ++t)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); // every worker goes back to sleep
            std::atomic<crd::u32> ran{0U};
            crd::jobs::JobDecl    d;
            d.fn                         = &mark_ran;
            d.data                       = &ran;
            d.pin_thread                 = static_cast<crd::i32>(t);
            crd::jobs::Counter* const cn = crd::jobs::run(d);
            const auto deadline          = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (ran.load(std::memory_order_acquire) == 0U && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (ran.load(std::memory_order_acquire) == 0U)
            {
                ++lost;
                INFO("round " << round << ", worker " << t);
                CHECK(ran.load(std::memory_order_acquire) == 1U);
                // A worker snapshot wakes every sleeper; then the stranded job runs and its counter can be waited on.
                crd::jobs::WorkerNode nodes[8]{};
                (void)crd::jobs::worker_snapshot(std::span<crd::jobs::WorkerNode>(nodes, 8U), 1000U);
                const auto rescue = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (ran.load(std::memory_order_acquire) == 0U && std::chrono::steady_clock::now() < rescue)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (ran.load(std::memory_order_acquire) == 0U)
                {
                    break; // still stranded: waiting on its counter would hang
                }
            }
            crd::jobs::wait(cn);
        }
    }
    crd::jobs::shutdown();
    CHECK(lost == 0U);
}
