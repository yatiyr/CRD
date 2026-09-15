// Observer-lifetime negative control: a set_observer() call made while a job is in flight must trip
// the quiescence contract (a CRD_FATAL). This specimen drives the real scheduler + observer, so unlike
// the other specimens it links crd-jobs.
//
// A CRD_FATAL cannot be read as a clean "Crashed" verdict by the harness: on Windows its default path
// pops a modal assert dialog (a headless child would hang -> timeout), and even via std::abort() the
// exit code (3) is below the harness's crash threshold. So the specimen installs the documented assert
// platform handler (the same hook tests use to run asserts without blocking): when the quiescence fatal
// fires, the handler exits deterministically with 42; a different assert exits 97. The consuming test
// judges on that exit code. If set_observer does NOT fatal, control falls through to a clean exit 0 --
// which the test rejects, so a broken check can never pass.

#define CRD_DIAG_SPECIMEN_SANITIZER "none" // no sanitizer involved: this is an engine-fatal control

#include "specimen_common.hpp"

#include <crd/core/assert.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/jobs/observer.hpp>

#include <cstdlib>
#include <cstring>

namespace
{
// Installed as the assert platform handler so report_assert_failure delegates here instead of showing
// the Windows dialog / aborting. Never returns -- exits with a code the harness can read.
int observer_swap_assert_handler(const char* formatted_message)
{
    if (formatted_message != nullptr &&
        std::strstr(formatted_message, "set_observer requires a quiescent job system") != nullptr)
    {
        std::_Exit(42); // the quiescence-contract fatal fired, as expected
    }
    std::_Exit(97); // some other assert fired first -- distinguishable from the expected one
}
} // namespace

int main()
{
    crd_diag_harden();
    crd::set_assert_platform_handler(&observer_swap_assert_handler);
    crd_diag_announce();

    crd::jobs::Config cfg;
    cfg.num_threads = 1U;
    crd::jobs::init(cfg);

    // Submit a job but do NOT wait: run() acquires a counter synchronously, so the system is
    // non-quiescent from here until wait() -- regardless of whether the job has run yet.
    crd::jobs::JobDecl job{};
    job.fn = [](void*) noexcept {};
    crd::jobs::Counter* const c = crd::jobs::run(job);

    // The violation: replace the observer while a counter is outstanding. This must CRD_FATAL, which
    // the handler above turns into _Exit(42).
    crd::jobs::JobObserver obs{};
    crd::jobs::set_observer(&obs);

    // Failure path only reached if the contract check did NOT fire. Drain and exit clean so the test
    // (which demands exit 42) fails loudly rather than passing by accident.
    crd::jobs::wait(c);
    crd::jobs::shutdown();
    return 0;
}
