// DIAG.4c pool-exhaustion positive controls. Counter-pool and fiber-pool exhaustion must each fail as a
// DISTINCT, always-on, COUNTED report -- never a silent nullptr the caller dereferences (counter pool) nor a
// silently dropped job (fiber pool). Both were previously guarded only by CRD_ASSERT_MSG, a no-op in Release.
//
// A live exhaustion aborts the process, so like the other detector controls each runs as a bounded child
// through the DIAG.0 harness. Each specimen installs an assert platform handler that, on its exhaustion
// message, reads the pool's event tally back through progress_snapshot().exhaustions and _Exit(42) only if the
// count surfaced (60 = message fired but count absent; 97 = a different assert; 0/timeout = the guard did not
// fire). The healthy-pool half is proven in-process: a pool that never exhausts reports 0.

#include <crd/diag/specimen_runner.hpp>
#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace cd   = crd::diag;
namespace cont = crd::containers;

void noop_job(void* /*data*/) noexcept {}
} // namespace

TEST_CASE("pool exhaustion: a healthy pool reports zero exhaustions", "[jobs][diag][exhaustion]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);

    CHECK(crd::jobs::progress_snapshot().exhaustions == 0U); // nothing acquired yet

    for (int i = 0; i < 64; ++i) // well within the default pool sizes -> never exhausts
    {
        crd::jobs::JobDecl j{};
        j.fn = &noop_job;
        crd::jobs::run_and_wait(j);
    }

    CHECK(crd::jobs::progress_snapshot().exhaustions == 0U); // a healthy run never bumps the tally

    crd::jobs::shutdown();
}

TEST_CASE("diag harness: counter-pool exhaustion is a counted, always-on fatal", "[jobs][diag][harness]")
{
    cd::Expectation e;
    e.want       = cd::Expectation::Want::CleanExit; // exit_code is the oracle here, not the verdict
    e.timeout_ms = 5000U;
    const cd::Outcome o =
        cd::run_specimen(cont::String{CRD_DIAG_COUNTER_EXHAUSTION_SPECIMEN}, cont::Array<cont::String>{}, e);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code << " id=" << o.identity.c_str());

    CHECK(cont::StringView{o.identity} == cont::StringView{"crd-diag-counter-exhaustion-specimen"});
    // 42 = the CounterPool-exhausted fatal fired AND its event tally surfaced via progress_snapshot; 60 = the
    // message fired but the count did not surface; 97 = a different assert; 0 = the guard did not fire at all.
    CHECK(o.exit_code == 42);
}

TEST_CASE("diag harness: fiber-pool exhaustion is a counted, always-on fatal", "[jobs][diag][harness]")
{
    cd::Expectation e;
    e.want       = cd::Expectation::Want::CleanExit;
    e.timeout_ms = 8000U; // > the specimen's 3s failure-path sleep, so exit 0 (not a timeout) is seen on failure
    const cd::Outcome o =
        cd::run_specimen(cont::String{CRD_DIAG_FIBER_EXHAUSTION_SPECIMEN}, cont::Array<cont::String>{}, e);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code << " id=" << o.identity.c_str());

    CHECK(cont::StringView{o.identity} == cont::StringView{"crd-diag-fiber-exhaustion-specimen"});
    // 42 = the fiber-pool-exhausted fatal fired AND its event tally surfaced; 60/97/0 as above.
    CHECK(o.exit_code == 42);
}
