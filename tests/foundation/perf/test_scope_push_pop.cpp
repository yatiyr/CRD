// crd-perf v0a -- scope push/pop produces well-formed Samples.

#include <crd/perf/diagnostics.hpp>
#include <crd/perf/perf.hpp>
#include <crd/time/clocks.hpp>

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

#if CRD_ENABLE_ASSERTS
TEST_CASE("zones, frame marks and the diagnostic clock read TickClock", "[perf][scope][diag11b]")
{
    // A map no MonotonicClock read gives: a millisecond per cycle-counter tick from 10^15 ns. Stamped by TickClock, a
    // zone lasts at least a millisecond for every tick between its two reads (there are dozens); stamped by
    // MonotonicClock it would last nanoseconds (DIAG.11b clause (8): the profiler's per-event clock).
    constexpr crd::i64 base = 1'000'000'000'000'000;
    PerfFixture        fx;
    crd::time::TickClock::test_force_map(crd::time::CycleCounter::now(), base, 1'000'000.0);
    {
        CRD_PERF_SCOPE("test.tick");
    }
    crd::perf::frame_mark();
    const crd::u64 diagnostic = crd::perf::diagnostic_now_ns();
    crd::time::TickClock::test_reset();

    const auto view = crd::perf::thread_samples(crd::perf::current_thread_index());
    REQUIRE(view.size == 1U);
    CHECK(view.data[0].begin_ns >= base);
    CHECK(view.data[0].end_ns - view.data[0].begin_ns >= 1'000'000);
    crd::perf::FrameRecord record;
    REQUIRE(crd::perf::copy_frame_record(0U, record));
    CHECK(record.frame_end_ns >= base);
    CHECK(diagnostic >= static_cast<crd::u64>(base));
}
#endif

TEST_CASE("a zone site first reached with no profiler running is named once one runs", "[perf][scope][diag11b]")
{
    // The site's name is interned on its first pass while a profiler runs: a site whose first pass came before init()
    // used to keep kInvalidNameId for good (its static was initialised by intern_name with none running).
    const auto pass = []
    {
        CRD_PERF_SCOPE("test.first-reached-off");
    };
    REQUIRE_FALSE(crd::perf::is_active());
    pass(); // no profiler: nothing recorded, nothing interned
    PerfFixture fx;
    pass();
    const auto view = crd::perf::thread_samples(crd::perf::current_thread_index());
    REQUIRE(view.size == 1U);
    const char* const name = crd::perf::resolve_name(crd::perf::NameId{view.data[0].name_id});
    REQUIRE(name != nullptr);
    CHECK(std::strcmp(name, "test.first-reached-off") == 0);
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
