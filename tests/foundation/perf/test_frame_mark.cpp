// crd-perf v0a -- frame_mark advances the frame counter.

#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#if CRD_PERF_ENABLED

namespace
{

struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

TEST_CASE("frame_count starts at 0 after init", "[perf][frame]")
{
    PerfFixture fx;
    CHECK(crd::perf::frame_count() == 0U);
    CHECK(crd::perf::current_frame_index() == 0U);
}

TEST_CASE("frame_mark increments frame_count", "[perf][frame]")
{
    PerfFixture fx;
    CRD_PERF_FRAME_MARK();
    CRD_PERF_FRAME_MARK();
    CRD_PERF_FRAME_MARK();
    CHECK(crd::perf::frame_count() == 3U);
}

TEST_CASE("frame_mark on inactive profiler is a no-op", "[perf][frame]")
{
    // No init -- inactive.
    CHECK_FALSE(crd::perf::is_active());
    CRD_PERF_FRAME_MARK(); // must not crash
    CHECK(crd::perf::frame_count() == 0U);
}

#endif // CRD_PERF_ENABLED
