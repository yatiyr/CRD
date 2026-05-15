// crd-perf v0a -- scope push/pop produces well-formed Samples.

#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{

struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

#if CRD_PERF_ENABLED

TEST_CASE("ScopedRegion writes one Sample to the recording thread's ring", "[perf][scope]")
{
    PerfFixture fx;
    {
        CRD_PERF_SCOPE("test.alpha");
    }

    const auto idx  = crd::perf::current_thread_index();
    const auto view = crd::perf::thread_samples(idx);
    REQUIRE(view.size == 1U);
    const auto& s = view.data[0];
    CHECK(s.begin_ns <= s.end_ns);
    CHECK(s.depth == 0U);
    CHECK(s.begin_thread == idx);
    CHECK(s.end_thread == idx);
    CHECK(s.fiber_id == 0U);
    CHECK(std::strcmp(crd::perf::resolve_name(crd::perf::NameId{s.name_id}), "test.alpha") == 0);
}

TEST_CASE("nested ScopedRegion records depth correctly", "[perf][scope][nesting]")
{
    PerfFixture fx;
    {
        CRD_PERF_SCOPE("outer");
        {
            CRD_PERF_SCOPE("inner");
            {
                CRD_PERF_SCOPE("innermost");
            }
        }
    }

    const auto idx  = crd::perf::current_thread_index();
    const auto view = crd::perf::thread_samples(idx);
    REQUIRE(view.size == 3U);

    // Children close before parents -- ring records innermost-first.
    CHECK(std::strcmp(crd::perf::resolve_name(crd::perf::NameId{view.data[0].name_id}), "innermost") == 0);
    CHECK(view.data[0].depth == 2U);
    CHECK(std::strcmp(crd::perf::resolve_name(crd::perf::NameId{view.data[1].name_id}), "inner") == 0);
    CHECK(view.data[1].depth == 1U);
    CHECK(std::strcmp(crd::perf::resolve_name(crd::perf::NameId{view.data[2].name_id}), "outer") == 0);
    CHECK(view.data[2].depth == 0U);
}

TEST_CASE("CRD_PERF_SCOPE_CATEGORY tags the Sample category", "[perf][scope][category]")
{
    PerfFixture fx;
    {
        CRD_PERF_SCOPE_CATEGORY("render.geometry", crd::perf::Category::Pass);
    }
    const auto view = crd::perf::thread_samples(crd::perf::current_thread_index());
    REQUIRE(view.size == 1U);
    CHECK(view.data[0].category == static_cast<crd::u8>(crd::perf::Category::Pass));
}

TEST_CASE("CRD_PERF_SCOPE_COLOR tags color_rgba", "[perf][scope][color]")
{
    PerfFixture fx;
    {
        CRD_PERF_SCOPE_COLOR("hot_loop", 0xFFAABBCCU);
    }
    const auto view = crd::perf::thread_samples(crd::perf::current_thread_index());
    REQUIRE(view.size == 1U);
    CHECK(view.data[0].color_rgba == 0xFFAABBCCU);
}

TEST_CASE("clear_samples drains the ring without resetting frame_count", "[perf][scope][clear]")
{
    PerfFixture fx;
    {
        CRD_PERF_SCOPE("a");
        CRD_PERF_SCOPE("b");
    }
    CRD_PERF_FRAME_MARK();
    CHECK(crd::perf::frame_count() == 1U);
    crd::perf::clear_samples();
    const auto view = crd::perf::thread_samples(crd::perf::current_thread_index());
    CHECK(view.size == 0U);
    CHECK(crd::perf::frame_count() == 1U);
}

#endif // CRD_PERF_ENABLED
