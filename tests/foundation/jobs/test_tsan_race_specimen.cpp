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

bool built_with_tsan()
{
#if defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

const char* yes_no(bool value)
{
    return value ? "yes" : "no";
}
} // namespace

TEST_CASE("tsan harness: the data-race specimen is caught, or reported absent", "[jobs][tsan][diag][harness]")
{
    const cd::LanePromise lane = cd::lane_promise();
    cd::Expectation e;
    e.want = cd::Expectation::Want::SanitizerCatch;
    const cd::Outcome o = cd::run_specimen(cont::String{CRD_DIAG_DATA_RACE_SPECIMEN}, cont::Array<cont::String>{}, e);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " sanitizer=" << o.sanitizer.c_str()
                    << " reason=" << o.reason.c_str() << " lane_tsan=" << yes_no(lane.tsan)
                    << " binary_tsan=" << yes_no(built_with_tsan()));
    // The specimen tags "tsan" only on a ThreadSanitizer build (where the race is caught) and "none"
    // otherwise -- including an ASan build, where ASan cannot detect a data race. Which verdict is correct
    // is the lane's promise (DIAG.10b), and this binary must carry it: judged on the specimen's own tag, a
    // specimen or a lane that lost -fsanitize=thread would report InstrumentAbsent and pass.
    CHECK(built_with_tsan() == lane.tsan);
    if (lane.tsan)
    {
        CHECK(cont::StringView{o.sanitizer} == cont::StringView{"tsan"});
        CHECK(o.verdict == cd::Verdict::SanitizerCaught);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::InstrumentAbsent);
    }
}
