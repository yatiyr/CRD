// A test executable can run many cases in one process: a kernel-level arm (crd_add_kernel_level_tests) or a direct run.
// crd_discover_tests (cmake/CrdTestDiscovery.cmake) links this listener into every test executable that links crd-jobs.
// A case that brings the jobs pool up and fails a REQUIRE before its shutdown leaves the pool up, and the next case's
// init would stop the run on "WorkerPool::init called twice" with every later case unreported. The listener shuts
// down a pool that a failed run of a case left up and that was down when the run started; a pool a listener or fixture
// keeps up across cases stays (a fixture that brings it up inside a case says so through keep_jobs_pool,
// jobs_pool_listener.hpp), and so does one a passing case leaks, whose next init still stops the run as the leak it is.

#include "jobs_pool_listener.hpp"

#include <crd/jobs/jobs.hpp>

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstdint>

namespace
{
bool g_kept = false; // set by keep_jobs_pool; the listener and the fixtures run on the main thread

struct JobsPoolListener final : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;

    void testCasePartialStarting(Catch::TestCaseInfo const& /*info*/, std::uint64_t /*part*/) override
    {
        m_was_up = crd::jobs::num_workers() != 0U;
    }

    void testCasePartialEnded(Catch::TestCaseStats const& stats, std::uint64_t /*part*/) override
    {
        if (!m_was_up && !g_kept && crd::jobs::num_workers() != 0U && !stats.totals.assertions.allOk())
        {
            crd::jobs::shutdown();
        }
    }

private:
    bool m_was_up = false;
};
} // namespace

void crd::test::keep_jobs_pool() noexcept
{
    g_kept = true;
}

CATCH_REGISTER_LISTENER(JobsPoolListener)
