// DIAG.0 -- the specimen harness's own controls: every positive (a specimen did
// exactly what a working detector makes it do) and every instrument failure (the
// tool broke, not the code) is a distinct, named verdict, never a silent pass.
//
// Contract: docs/design/runtime-diagnostics.md (shared acceptance rules). The
// specimen executable paths arrive as compile definitions from the CMake target.

#include <crd/diag/specimen_runner.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace cd = crd::diag;
namespace cont = crd::containers;

cont::String clean_specimen()
{
    return cont::String{CRD_DIAG_CLEAN_SPECIMEN};
}
cont::String crash_specimen()
{
    return cont::String{CRD_DIAG_CRASH_SPECIMEN};
}
cont::String heap_overflow_specimen()
{
    return cont::String{CRD_DIAG_HEAP_OVERFLOW_SPECIMEN};
}

bool built_with_asan()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}
} // namespace

TEST_CASE("diag harness: the clean specimen is an expected-clean positive", "[diag][harness]")
{
    cd::Expectation e;
    e.want = cd::Expectation::Want::CleanExit;
    e.expected_identity = cont::String{"crd-diag-clean-specimen"};
    const cd::Outcome o = cd::run_specimen(clean_specimen(), cont::Array<cont::String>{}, e);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " reason=" << o.reason.c_str());
    CHECK(o.verdict == cd::Verdict::Clean);
    CHECK_FALSE(cont::StringView{o.host_tuple}.empty());
}

TEST_CASE("diag harness: the crash specimen is an expected-crash positive", "[diag][harness]")
{
    cd::Expectation e;
    e.want = cd::Expectation::Want::Crash;
    const cd::Outcome o = cd::run_specimen(crash_specimen(), cont::Array<cont::String>{}, e);
    INFO("verdict=" << cd::verdict_name(o.verdict));
    CHECK(o.verdict == cd::Verdict::Crashed);
}

TEST_CASE("diag harness: the sanitizer specimen is caught, or reported absent", "[diag][harness]")
{
    // The falsifier of the whole fixture: an expected sanitizer catch must be a
    // CATCH when the runtime is present and INSTRUMENT-ABSENT when it is not --
    // never a silent pass either way. The specimen is built with the same flags as
    // this test, so the build's sanitizer decides which verdict is correct.
    cd::Expectation e;
    e.want = cd::Expectation::Want::SanitizerCatch;
    const cd::Outcome o = cd::run_specimen(heap_overflow_specimen(), cont::Array<cont::String>{}, e);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " sanitizer=" << o.sanitizer.c_str());
    if (built_with_asan())
    {
        CHECK(o.verdict == cd::Verdict::SanitizerCaught);
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::InstrumentAbsent);
    }
}

TEST_CASE("diag harness: a mismatched binary is detected, not passed", "[diag][harness]")
{
    cd::Expectation e;
    e.want = cd::Expectation::Want::CleanExit;
    e.expected_identity = cont::String{"a-different-specimen"};
    const cd::Outcome o = cd::run_specimen(clean_specimen(), cont::Array<cont::String>{}, e);
    CHECK(o.verdict == cd::Verdict::MismatchedBinary);
}

TEST_CASE("diag harness: a denied output path is detected before spawning", "[diag][harness]")
{
    cd::Expectation e;
    e.want = cd::Expectation::Want::CleanExit;
    // An existing FILE used as an output directory is unwritable on every platform,
    // unlike a read-only directory an elevated user could still write.
    e.output_dir = clean_specimen();
    const cd::Outcome o = cd::run_specimen(clean_specimen(), cont::Array<cont::String>{}, e);
    CHECK(o.verdict == cd::Verdict::DeniedOutput);
}

TEST_CASE("diag harness: a zero-specimen selection fails, never a vacuous pass", "[diag][harness]")
{
    cont::Array<cont::String> candidates;
    candidates.push_back(clean_specimen());
    candidates.push_back(crash_specimen());
    cd::Expectation e;
    e.want = cd::Expectation::Want::CleanExit;
    const cd::Outcome o = cd::select_and_run(candidates, cont::String{"no-such-specimen"}, e);
    CHECK(o.verdict == cd::Verdict::ZeroSelection);
}

TEST_CASE("diag harness: a missing executable is an instrument failure", "[diag][harness]")
{
    cd::Expectation e;
    e.want = cd::Expectation::Want::CleanExit;
    const cd::Outcome o =
        cd::run_specimen(cont::String{"./no-such-specimen-binary"}, cont::Array<cont::String>{}, e);
    CHECK(o.verdict == cd::Verdict::MissingExecutable);
}
