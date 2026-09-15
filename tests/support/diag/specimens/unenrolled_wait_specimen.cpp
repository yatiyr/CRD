// No-blocking-context negative control: an unenrolled thread (a raw std::thread, not thread 0 and not
// a pool worker) that waits on a single-thread pool must trip the deadlock fatal -- nothing can ever
// decrement the counter. Like observer_swap_specimen it installs an assert platform handler that turns
// the fatal into a deterministic _Exit(42) (see that file for why "Crashed" is not a usable verdict).
//
// The one difference: if the check does NOT fire, wait() spins forever -- the failure mode is a HANG,
// not a clean exit. So the consuming test runs this with a short timeout; a timeout (exit code != 42)
// is the failure signal. _Exit runs on the worker thread and kills the process directly; we never
// return to join().

#define CRD_DIAG_SPECIMEN_SANITIZER "none" // no sanitizer involved: this is an engine-fatal control

#include "specimen_common.hpp"

#include <crd/core/assert.hpp>
#include <crd/jobs/jobs.hpp>

#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
int unenrolled_wait_assert_handler(const char* formatted_message)
{
    if (formatted_message != nullptr &&
        std::strstr(formatted_message, "an unenrolled thread cannot wait on a single-thread pool") != nullptr)
    {
        std::_Exit(42); // the no-blocking-context fatal fired, as expected
    }
    std::_Exit(97); // a different assert fired first -- distinguishable from the expected one
}
} // namespace

int main()
{
    crd_diag_harden();
    crd::set_assert_platform_handler(&unenrolled_wait_assert_handler);
    crd_diag_announce();

    crd::jobs::Config cfg;
    cfg.num_threads = 1U; // only thread 0 exists; no background worker can decrement a counter
    crd::jobs::init(cfg);

    // The violation: an unenrolled thread waits on a single-thread pool. wait() must CRD_FATAL, which
    // the handler turns into _Exit(42) from this thread -- terminating the process before join().
    std::thread t(
        []()
        {
            crd::jobs::JobDecl job{};
            job.fn = [](void*) noexcept {};
            crd::jobs::Counter* const c = crd::jobs::run(job);
            crd::jobs::wait(c);
        });
    t.join(); // not reached on the expected path

    // Failure path only: the check did not fire (and somehow did not hang). Exit clean so the test,
    // which demands 42, fails rather than passing by accident.
    crd::jobs::shutdown();
    return 0;
}
