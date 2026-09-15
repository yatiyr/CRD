// DIAG.4c seeded-deadlock positive control: two sibling jobs each parked on the other's completion counter
// form a wait cycle. The hang watchdog must detect the stall AND classify it as HangKind::WaitCycle (not the
// executor-starved or parked-but-no-cycle shapes, which are proven in-process in test_diag_hang_watchdog.cpp).
//
// This one shape cannot be exercised in-process: a live deadlock leaves both counters permanently acquired,
// and CounterPool::shutdown asserts on that -- so the specimen runs as a bounded child through the DIAG.0
// harness and _Exit(42)s from the watchdog thread when it sees WaitCycle (96 = a hang classified as another
// kind, 97 = a stray assert, 0 = the watchdog never fired). Its path arrives as a compile definition from CMake.

#include <crd/diag/specimen_runner.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace cd   = crd::diag;
namespace cont = crd::containers;
} // namespace

TEST_CASE("diag harness: a seeded wait-cycle deadlock is classified as WaitCycle", "[jobs][diag][harness]")
{
    cd::Expectation e;
    e.want       = cd::Expectation::Want::CleanExit; // exit_code is the oracle here, not the verdict
    e.timeout_ms = 8000U;                            // > the specimen's 3s failure-path sleep, so exit 0 is seen (not a timeout)
    const cd::Outcome o =
        cd::run_specimen(cont::String{CRD_DIAG_DEADLOCK_SPECIMEN}, cont::Array<cont::String>{}, e);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code << " id=" << o.identity.c_str());

    CHECK(cont::StringView{o.identity} == cont::StringView{"crd-diag-deadlock-specimen"});
    // 42 = the watchdog fired and classified the mutual wait as WaitCycle; 96 = it fired but classified some
    // other shape (the classifier is wrong -- the failure this test exists to catch); 97 = a stray assert;
    // 0 = the watchdog never fired at all. Only 42 is the WaitCycle proof, so assert on the exit code directly.
    CHECK(o.exit_code == 42);
}
