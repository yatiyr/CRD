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
cont::String use_after_free_specimen()
{
    return cont::String{CRD_DIAG_USE_AFTER_FREE_SPECIMEN};
}
cont::String use_after_return_specimen()
{
    return cont::String{CRD_DIAG_USE_AFTER_RETURN_SPECIMEN};
}
cont::String leak_specimen()
{
    return cont::String{CRD_DIAG_LEAK_SPECIMEN};
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

TEST_CASE("diag 3f: heap-use-after-free is caught by ASan, or reported absent", "[diag][harness][diag3f]")
{
    // A temporal-safety class core AddressSanitizer catches on every ASan lane (win-asan + linux).
    // Same falsifier as the heap-overflow case: CAUGHT with the runtime present, INSTRUMENT-ABSENT
    // without -- never a silent pass. Contract: docs/design/runtime-diagnostics.md#diag-3f.
    cd::Expectation e;
    e.want = cd::Expectation::Want::SanitizerCatch;
    const cd::Outcome o = cd::run_specimen(use_after_free_specimen(), cont::Array<cont::String>{}, e);
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

TEST_CASE("diag 3f: stack-use-after-return is qualified on the gcc/clang ASan lanes", "[diag][harness][diag3f]")
{
    // The design's named "MSVC use-after-return" class. The specimen self-reports its route: on gcc/clang
    // ASan it tags "asan" and commits a real stack-use-after-return the runtime catches (SanitizerCaught);
    // on MSVC ASan -- where the fake-stack route is not reliably reported -- it pre-tags "none", so the
    // route is InstrumentAbsent (explicitly unqualified, a partial-instrumentation dependency, never a
    // skip-pass). The harness judges purely on the specimen's echoed tag, so no lane #ifdef is needed.
    // Contract: docs/design/runtime-diagnostics.md#diag-3f.
    cd::Expectation e;
    e.want = cd::Expectation::Want::SanitizerCatch;
    const cd::Outcome o = cd::run_specimen(use_after_return_specimen(), cont::Array<cont::String>{}, e);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " sanitizer=" << o.sanitizer.c_str());
    if (cont::StringView{o.sanitizer} == cont::StringView{"asan"})
    {
        CHECK(o.verdict == cd::Verdict::SanitizerCaught); // gcc/clang ASan lane: the route is qualified
    }
    else
    {
        CHECK(o.verdict == cd::Verdict::InstrumentAbsent); // no ASan, or the MSVC-unqualified route
    }
}

TEST_CASE("diag 3f: a memory leak is caught by LeakSanitizer, or reported absent", "[diag][harness][diag3f]")
{
    // The "leak routes" class. On a LeakSanitizer build the specimen tags "asan" and aborts on a
    // recoverable leak check (SanitizerCaught); where no LSan route exists it tags "none" and exits
    // clean (InstrumentAbsent). Judged purely on the specimen's echoed tag -- no lane #ifdef, no
    // skip-pass. Qualified on linux-gcc-asan. Contract: docs/design/runtime-diagnostics.md#diag-3f.
    cd::Expectation e;
    e.want = cd::Expectation::Want::SanitizerCatch;
    const cd::Outcome o = cd::run_specimen(leak_specimen(), cont::Array<cont::String>{}, e);
    INFO("verdict=" << cd::verdict_name(o.verdict) << " sanitizer=" << o.sanitizer.c_str());
    if (cont::StringView{o.sanitizer} == cont::StringView{"asan"})
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
