// Example: the jobs pool under the diagnostic host (DIAG.11a).
//
//   diag-consumer jobs <profile.host.toml>
//
// The host starts first and gives the pool its watchdog window (configure_jobs): start() is refused once the pool
// runs, because the watchdog window and the pool's starvation and hang handlers are part of what the profile
// chooses. The pool then runs the application's work, 64 jobs that each sum a slice of 1..64000 and record the slice
// into the flight recorder from whichever worker ran it; the slices add up to the known total.
//
// Then the application's defect: a job pinned to the main thread (JobDecl::pin_thread = 0), which only runs while the
// main thread pumps or waits, and a main thread that does neither. The workers cannot take it, so nothing completes;
// after a few stale windows the watchdog reports a hang (executor starved) into the flight recorder, and the capture,
// with the slices before it, is written to the profile's capture directory. The application then pumps, the job
// runs, and the pool and the host shut down in order: the pool's watchdog thread is joined before the host stops.
#include "consumer_support.hpp"
#include "examples.hpp"

#include <crd/jobs/jobs.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

namespace diag_consumer
{

namespace containers = crd::containers;

namespace
{

constexpr crd::u32 kThreads     = 4U;
constexpr crd::u32 kSlices      = 64U;
constexpr crd::u64 kSliceLength = 1000U;
constexpr crd::u32 kStallWaitMs = 30000U; // the report comes after a few watchdog windows; this bounds a defect

struct Slice
{
    crd::u64            first    = 0U;
    crd::u64            last     = 0U;
    crd::u64            sum      = 0U;
    crd::u32            index    = 0U;
    cp::FlightRecorder* recorder = nullptr;
};

Slice g_slices[kSlices]; // NOLINT(modernize-avoid-c-arrays)

void sum_slice(void* data)
{
    auto* const slice = static_cast<Slice*>(data);
    crd::u64    sum   = 0U;
    for (crd::u64 v = slice->first; v <= slice->last; ++v)
    {
        sum += v;
    }
    slice->sum = sum;
    char text[48];
    (void)std::snprintf(text, sizeof(text), "slice %u summed", static_cast<unsigned>(slice->index));
    slice->recorder->record(kSliceSummed, cp::Severity::Info, containers::StringView{text}, slice->index, sum);
}

bool g_late_job_ran = false;

void late_job(void* /*data*/)
{
    g_late_job_ran = true;
}

} // namespace

int example_jobs(const char* profile_path)
{
    cp::DiagnosticHost host;
    if (!start_host(host, profile_path, "diag-consumer jobs <profile.host.toml>"))
    {
        return 4;
    }
    crd::jobs::Config config;
    config.num_threads = kThreads;
    host.configure_jobs(config); // the profile's watchdog window, before the pool exists
    std::printf("CONSUMER_JOBS_POOL threads=%u watchdog_ms=%u\n", static_cast<unsigned>(config.num_threads),
                static_cast<unsigned>(config.hang_watchdog_period_ms));
    crd::jobs::init(config);

    // The work: every slice on whichever worker takes it, recorded where it ran.
    crd::jobs::JobDecl decls[kSlices]; // NOLINT(modernize-avoid-c-arrays)
    for (crd::u32 i = 0U; i < kSlices; ++i)
    {
        g_slices[i].first    = (i * kSliceLength) + 1U;
        g_slices[i].last     = (i + 1U) * kSliceLength;
        g_slices[i].index    = i;
        g_slices[i].recorder = &host.recorder();
        decls[i].fn          = &sum_slice;
        decls[i].data        = &g_slices[i];
    }
    crd::jobs::run_and_wait(crd::containers::ConstSpan<crd::jobs::JobDecl>{decls, kSlices});
    crd::u64 total = 0U;
    for (const Slice& slice : g_slices)
    {
        total += slice.sum;
    }
    const crd::u64 n = kSlices * kSliceLength;
    std::printf("CONSUMER_JOBS_SUM slices=%u total=%llu expected=%llu\n", static_cast<unsigned>(kSlices),
                static_cast<unsigned long long>(total), static_cast<unsigned long long>((n * (n + 1U)) / 2U));

    // The defect: a main-thread job, and a main thread that neither pumps nor waits.
    crd::jobs::JobDecl late{};
    late.fn         = &late_job;
    late.pin_thread = 0;
    crd::jobs::Counter* const late_done = crd::jobs::run(late);
    const bool                captured  = wait_for_bundles(host, 1U, kStallWaitMs);
    std::printf("CONSUMER_JOBS_STALL captured=%d late_job_ran_before_pump=%d\n", captured ? 1 : 0,
                g_late_job_ran ? 1 : 0);
    if (captured)
    {
        print_last_bundle(host, "hang");
    }

    // The repair: the main thread pumps, the job runs, and the counter is waited on.
    (void)crd::jobs::pump_main_thread_until_idle();
    crd::jobs::wait(late_done);
    std::printf("CONSUMER_JOBS_RECOVERED late_job_ran=%d\n", g_late_job_ran ? 1 : 0);
    crd::jobs::shutdown(); // joins the watchdog thread before the host it reports into stops
    host.stop();
    return captured ? 0 : 8;
}

} // namespace diag_consumer
