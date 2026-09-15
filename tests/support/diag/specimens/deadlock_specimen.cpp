// Seeded-deadlock positive control: two sibling jobs each parked on the OTHER's completion counter form a
// wait cycle (A waits on B, B waits on A), which nothing can ever break. This proves the hang watchdog's
// WaitCycle classification end-to-end -- the one HangKind that cannot be reproduced in-process, because a
// live deadlock leaves counters permanently acquired and CounterPool::shutdown asserts on that. So, like the
// other engine-fatal controls, it runs as a bounded child: the watchdog fires on its own thread and, seeing
// kind == WaitCycle, exits deterministically with 42. A wrong kind exits 96; a stray assert exits 97; if the
// watchdog never fires, both fibers stay parked forever and the harness's timeout is the failure signal.
//
// Why the classifier reads WaitCycle and not a transient wrong kind: the watchdog only counts a window as a
// suspected hang when NO worker is executing, and a job that is still spinning (waiting for the other counter
// to be published) keeps its worker executing -> that window reads Progressing and resets the debouncer. So by
// the time the K-th consecutive stale window fires, BOTH fibers have parked and the wait-graph snapshot carries
// the complete two-node cycle.

#define CRD_DIAG_SPECIMEN_SANITIZER "none" // no sanitizer involved: this is a scheduler-liveness control

#include "specimen_common.hpp"

#include <crd/core/assert.hpp>
#include <crd/jobs/jobs.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace
{
// The two completion counters, published by main once each job is submitted. Each job spins until the other's
// counter is visible, then parks on it -- so neither can park until both exist, and the cycle is symmetric.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<crd::jobs::Counter*> g_counter_a{nullptr};
std::atomic<crd::jobs::Counter*> g_counter_b{nullptr};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// Installed as the hang handler. Runs on the watchdog thread with no lock held; _Exit terminates the process
// before any counter is released, so CounterPool::shutdown's still-acquired assert never runs.
void on_hang(const crd::jobs::HangReport& report, void* /*user*/) noexcept
{
    std::printf("CRD_DIAG_HANG_KIND=%d\n", static_cast<int>(report.kind));
    std::printf("CRD_DIAG_PARKED_TOTAL=%zu\n", static_cast<std::size_t>(report.parked_total));
    std::fflush(stdout);
    if (report.kind == crd::jobs::HangKind::WaitCycle)
        std::_Exit(42); // the deadlock was classified as a wait cycle, as expected
    std::_Exit(96);     // a hang was detected but classified as some other shape
}

int deadlock_assert_handler(const char* /*formatted_message*/)
{
    std::_Exit(97); // a stray engine assert fired before the watchdog could classify the deadlock
}

void job_a(void* /*data*/) noexcept
{
    while (g_counter_b.load(std::memory_order_acquire) == nullptr)
        std::this_thread::yield(); // keeps this worker EXECUTING until B is submitted (so no premature verdict)
    crd::jobs::wait(g_counter_b.load(std::memory_order_acquire)); // park on B's completion -- never satisfied
}

void job_b(void* /*data*/) noexcept
{
    while (g_counter_a.load(std::memory_order_acquire) == nullptr)
        std::this_thread::yield();
    crd::jobs::wait(g_counter_a.load(std::memory_order_acquire)); // park on A's completion -- never satisfied
}
} // namespace

int main()
{
    crd_diag_harden();
    crd::set_assert_platform_handler(&deadlock_assert_handler);
    crd::jobs::set_hang_handler(&on_hang, nullptr);
    crd_diag_announce();

    crd::jobs::Config cfg;
    cfg.num_threads             = 3U;  // main (thread 0) + two background workers, so A and B run in parallel
    cfg.hang_watchdog_period_ms = 20U; // ~60ms to the K=3 report once both fibers are parked
    crd::jobs::init(cfg);

    crd::jobs::JobDecl a{};
    a.fn = &job_a;
    crd::jobs::JobDecl b{};
    b.fn = &job_b;

    // Submit both, publishing each counter only after run() has returned it. Each job spins until it sees the
    // other's counter, then parks -- the mutual wait closes into a cycle nothing can decrement.
    g_counter_a.store(crd::jobs::run(a), std::memory_order_release);
    g_counter_b.store(crd::jobs::run(b), std::memory_order_release);

    // The watchdog fires from its own thread and _Exit(42)s. If it never does, the two fibers are parked
    // forever; this sleep expires and we exit non-42 so the harness (which demands 42) fails loudly rather
    // than the process hanging until the timeout.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::_Exit(0);
}
