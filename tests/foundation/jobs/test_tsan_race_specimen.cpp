// DIAG.1b -- the TSan lane's instrument-present control. The deliberately racy specimen must be
// CAUGHT when this test (and the specimen, built with the same flags) is a ThreadSanitizer build,
// and reported INSTRUMENT-ABSENT otherwise -- never a silent pass either way. This is the negative
// control the positive-control suite (test_tsan_fiber_model.cpp) cannot contain, because an
// intentional race aborts under halt_on_error; it runs through the DIAG.0 specimen harness as a
// bounded child, mirroring the ASan heap-overflow case in tests/foundation/perf/test_diag_harness.cpp.
//
// The specimen executable path arrives as a compile definition (CRD_DIAG_DATA_RACE_SPECIMEN) from
// CMake. Contract: docs/design/runtime-diagnostics.md#diag-1b; ADR-0133.

#include <crd/diag/specimen_runner.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace cd = crd::diag;
namespace cont = crd::containers;
} // namespace

TEST_CASE("tsan harness: the data-race specimen is caught, or reported absent", "[jobs][tsan][diag][harness]")
{
    cd::Expectation e;
    e.want = cd::Expectation::Want::SanitizerCatch;
    const cd::Outcome o = cd::run_specimen(cont::String{CRD_DIAG_DATA_RACE_SPECIMEN}, cont::Array<cont::String>{}, e);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " sanitizer=" << o.sanitizer.c_str());
    // The specimen self-reports its route: "tsan" only on a ThreadSanitizer build (where the race is
    // caught), "none" otherwise -- including an ASan build, where ASan is present but cannot detect a
    // data race. Judge on the echoed tag so an ASan lane reports InstrumentAbsent, not a false failure.
    if (cont::StringView{o.sanitizer} == cont::StringView{"tsan"})
    {
        CHECK(o.verdict == cd::Verdict::SanitizerCaught);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::InstrumentAbsent);
    }
}
