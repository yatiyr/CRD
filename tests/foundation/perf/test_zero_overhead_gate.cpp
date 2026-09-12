// crd-perf v0a -- zero-overhead-gate behavioural test.
//
// Verifies the "off"-path contract at the program-behaviour level:
//
//   1. When CRD_PERF_ENABLED == 0, the macros compile to ((void)0) and
//      the singleton is never constructed -- is_active() stays false
//      even after init().
//   2. Even with CRD_PERF_ENABLED == 1, if init() was never called,
//      the hot path (push_region/pop_region via the macro) must not
//      crash and must produce no Samples.
//
// The full byte-equivalent objdump test lives in a separate CMake-time
// rule (added at v0a close after we know the precise sandbox/preset
// pairs that exercise it).

#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("inactive profiler accepts CRD_PERF_SCOPE without crashing", "[perf][gate]")
{
    // No init. The macro must still expand and the path must be inert.
    CHECK_FALSE(crd::perf::is_active());
    {
        CRD_PERF_SCOPE("inactive_path");
    }
    CRD_PERF_FRAME_MARK();
    CHECK(crd::perf::frame_count() == 0U);
    // No assertion on samples -- t_thread_index is invalid, no ring,
    // and the test passes as long as no crash occurred.
}

#if CRD_PERF_ENABLED == 0
TEST_CASE("CRD_PERF_ENABLED == 0 -- substrate is inert at runtime", "[perf][gate][off]")
{
    crd::perf::init({});
    CHECK_FALSE(crd::perf::is_active());
    {
        CRD_PERF_SCOPE("off_path");
    }
    CRD_PERF_FRAME_MARK();
    CHECK(crd::perf::frame_count() == 0U);
}
#endif

#if CRD_PERF_ENABLED
TEST_CASE("CRD_PERF_ENABLED == 1 -- substrate activates on init()", "[perf][gate][on]")
{
    crd::perf::init({});
    CHECK(crd::perf::is_active());
    CRD_PERF_FRAME_MARK();
    CHECK(crd::perf::frame_count() == 1U);
    crd::perf::shutdown();
    CHECK_FALSE(crd::perf::is_active());
}
#endif
