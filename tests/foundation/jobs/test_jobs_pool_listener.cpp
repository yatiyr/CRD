// The jobs pool listener (tests/support/catch/jobs_pool_listener.cpp): a case that fails with the pool up does not stop
// the cases after it in the same process. The test re-enters this executable on two hidden cases; without the
// listener the second case's init stops the child on "WorkerPool::init called twice". A pool a fixture keeps for the
// run (keep_jobs_pool) stays up after a failed case: two more hidden cases; without the keep the listener shut the
// fixture's pool down and the second case met it down.

#include "jobs_pool_listener.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/diag/specimen_runner.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>
#include <string_view>

namespace
{
namespace cont = crd::containers;
namespace cd = crd::diag;

bool contains(const cd::Outcome& o, std::string_view text)
{
    return std::string_view{o.output.data(), o.output.size()}.find(text) != std::string_view::npos;
}

// The child run's fixture: brought up by the first case that needs it, kept for the run, down at exit.
struct KeptPool
{
    KeptPool()
    {
        crd::jobs::init(crd::jobs::Config{.num_threads = 2U});
        crd::test::keep_jobs_pool();
    }
    ~KeptPool() { crd::jobs::shutdown(); }
    KeptPool(const KeptPool&) = delete;
    KeptPool& operator=(const KeptPool&) = delete;
    KeptPool(KeptPool&&) = delete;
    KeptPool& operator=(KeptPool&&) = delete;
};

KeptPool& kept_pool()
{
    static KeptPool pool;
    return pool;
}

// The child run's outcome on the hidden cases of tag.
cd::Outcome run_child(const char* tag)
{
    const cont::String self = cd::self_executable();
    REQUIRE_FALSE(self.empty());
    cont::Array<cont::String> args;
    args.push_back(cont::String{tag});
    cd::Expectation e;
    e.timeout_ms = 30000U;
    return cd::run_specimen(self, args, e);
}
} // namespace

TEST_CASE("jobs pool child: a case fails with the pool up", "[.jobs-pool-child]")
{
    crd::jobs::init(crd::jobs::Config{.num_threads = 2U});
    REQUIRE(crd::jobs::num_workers() == 0U); // fails on purpose before the shutdown
    crd::jobs::shutdown();
}

TEST_CASE("jobs pool child: the next case brings its own pool up", "[.jobs-pool-child]")
{
    crd::jobs::init(crd::jobs::Config{.num_threads = 2U});
    CHECK(crd::jobs::num_workers() == 2U);
    crd::jobs::shutdown();
    CHECK(crd::jobs::num_workers() == 0U);
}

TEST_CASE("jobs pool listener: a case that fails with the pool up does not stop the cases after it", "[jobs]")
{
    const cd::Outcome o = run_child("[.jobs-pool-child]");
    INFO(std::string_view(o.output.data(), o.output.size()));
    CHECK_FALSE(o.crashed);
    CHECK_FALSE(o.timed_out);
    CHECK(o.exit_code == 42); // Catch2 3's exit code for a run with failed cases
    CHECK(contains(o, "test cases: 2 | 1 passed | 1 failed"));
    CHECK_FALSE(contains(o, "init called twice"));
}

TEST_CASE("jobs pool kept child: a case brings the kept pool up and fails", "[.jobs-pool-kept-child]")
{
    kept_pool();
    REQUIRE(crd::jobs::num_workers() == 0U); // fails on purpose with the kept pool up
}

TEST_CASE("jobs pool kept child: the next case finds the kept pool up", "[.jobs-pool-kept-child]")
{
    kept_pool();
    CHECK(crd::jobs::num_workers() == 2U);
}

TEST_CASE("jobs pool listener: a pool a fixture keeps stays up after a failed case", "[jobs]")
{
    const cd::Outcome o = run_child("[.jobs-pool-kept-child]");
    INFO(std::string_view(o.output.data(), o.output.size()));
    CHECK_FALSE(o.crashed);
    CHECK_FALSE(o.timed_out);
    CHECK(o.exit_code == 42);
    CHECK(contains(o, "test cases: 2 | 1 passed | 1 failed"));
}
