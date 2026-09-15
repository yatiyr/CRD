// DIAG.4a observer-lifetime negative control: a mid-flight set_observer() must trip the job system's
// quiescence contract. The positive path (installing an observer while quiescent) is proven in-process
// by test_diag_observer_lifetime.cpp; the violation aborts, so like the other detector controls it runs
// as a bounded child through the DIAG.0 harness. The specimen turns the fatal into a deterministic
// exit code via an assert platform handler (see observer_swap_specimen.cpp for why Crashed is not a
// usable verdict here); its path arrives as a compile definition from CMake.

#include <crd/diag/specimen_runner.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace cd   = crd::diag;
namespace cont = crd::containers;
} // namespace

TEST_CASE("diag harness: a mid-flight set_observer trips the quiescence fatal", "[jobs][diag][harness]")
{
    cd::Expectation e;
    e.want = cd::Expectation::Want::CleanExit; // exit_code is the oracle here, not the verdict
    const cd::Outcome o =
        cd::run_specimen(cont::String{CRD_DIAG_OBSERVER_SWAP_SPECIMEN}, cont::Array<cont::String>{}, e);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code << " id=" << o.identity.c_str());

    CHECK(cont::StringView{o.identity} == cont::StringView{"crd-diag-observer-swap-specimen"});
    // 42 = the specimen's handler saw the quiescence-contract message and exited deterministically;
    // 97 = a different assert fired first; 0 = the check did NOT fire (the failure this test exists to
    // catch). The verdict is Unexpected under CleanExit (exit != 0) by design -- the exit code is what
    // proves the contract fired, so assert on it directly and not on the verdict.
    CHECK(o.exit_code == 42);
}
