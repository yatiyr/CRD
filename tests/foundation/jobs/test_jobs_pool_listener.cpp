// The jobs pool listener (tests/support/catch/jobs_pool_listener.cpp): a case that fails with the pool up does not stop
// the cases after it in the same process. The test re-enters this executable on two hidden cases; without the
// listener the second case's init stops the child on "WorkerPool::init called twice".

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
    const cont::String self = cd::self_executable();
    REQUIRE_FALSE(self.empty());
    cont::Array<cont::String> args;
    args.push_back(cont::String{"[.jobs-pool-child]"});
    cd::Expectation e;
    e.timeout_ms = 30000U;
    const cd::Outcome o = cd::run_specimen(self, args, e);
    INFO(std::string_view(o.output.data(), o.output.size()));
    CHECK_FALSE(o.crashed);
    CHECK_FALSE(o.timed_out);
    CHECK(o.exit_code == 42); // Catch2 3's exit code for a run with failed cases
    CHECK(contains(o, "test cases: 2 | 1 passed | 1 failed"));
    CHECK_FALSE(contains(o, "init called twice"));
}
