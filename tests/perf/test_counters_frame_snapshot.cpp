// crd-perf v0b -- frame_mark snapshots counters into the history ring;
//                  Add-kind counters reset; Set-kind survive.

#include <crd/perf/perf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>

#if CRD_PERF_ENABLED

namespace
{

struct PerfFixture
{
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

TEST_CASE("frame_mark captures a FrameRecord", "[perf][counter][frame]")
{
    PerfFixture fx;
    REQUIRE(crd::perf::frame_record_count() == 0U);
    CRD_PERF_FRAME_MARK();
    CHECK(crd::perf::frame_record_count() == 1U);
    const auto* rec = crd::perf::frame_record(0U);
    REQUIRE(rec != nullptr);
    CHECK(rec->frame_index == 0U); // first frame's index = 0
    CHECK(rec->frame_end_ns >= rec->frame_begin_ns);
}

TEST_CASE("Set-kind counter value survives frame_mark", "[perf][counter][frame][set]")
{
    PerfFixture fx;
    const auto id = crd::perf::register_counter_i64("set.survive", crd::perf::CounterKind::Set);
    crd::perf::counter_set_i64(id, 100);
    CRD_PERF_FRAME_MARK();
    // Set-kind: value survives.
    CHECK(crd::perf::counter_current_i64(id) == 100);
    const auto* rec = crd::perf::frame_record(0U);
    REQUIRE(rec != nullptr);
    REQUIRE(rec->counter_count >= 1U);
    CHECK(static_cast<crd::i64>(rec->values[id.value].bits) == 100);
}

TEST_CASE("Add-kind counter resets after frame_mark", "[perf][counter][frame][add]")
{
    PerfFixture fx;
    const auto id = crd::perf::register_counter_i64("add.reset", crd::perf::CounterKind::Add);
    crd::perf::counter_add_i64(id, 7);
    crd::perf::counter_add_i64(id, 8);
    CRD_PERF_FRAME_MARK();
    // Add-kind: value resets.
    CHECK(crd::perf::counter_current_i64(id) == 0);
    const auto* rec = crd::perf::frame_record(0U);
    REQUIRE(rec != nullptr);
    REQUIRE(rec->counter_count >= 1U);
    CHECK(static_cast<crd::i64>(rec->values[id.value].bits) == 15);
}

TEST_CASE("frame_record(N) reaches into the ring", "[perf][counter][frame][history]")
{
    PerfFixture fx;
    const auto id = crd::perf::register_counter_i64("history", crd::perf::CounterKind::Set);
    for (crd::i64 i = 1; i <= 5; ++i)
    {
        crd::perf::counter_set_i64(id, i * 10);
        CRD_PERF_FRAME_MARK();
    }
    CHECK(crd::perf::frame_record_count() == 5U);
    // Most-recent = 50, prev = 40, prev = 30, etc.
    CHECK(static_cast<crd::i64>(crd::perf::frame_record(0)->values[id.value].bits) == 50);
    CHECK(static_cast<crd::i64>(crd::perf::frame_record(1)->values[id.value].bits) == 40);
    CHECK(static_cast<crd::i64>(crd::perf::frame_record(4)->values[id.value].bits) == 10);
    CHECK(crd::perf::frame_record(5) == nullptr); // past the captured history
}

TEST_CASE("frame_record saturates at ring slot count", "[perf][counter][frame][saturation]")
{
    // Small history ring to test wrap.
    crd::perf::InitConfig cfg{};
    cfg.frame_history_slots = 4U;
    crd::perf::init(cfg);

    const auto id = crd::perf::register_counter_i64("wrap.test", crd::perf::CounterKind::Set);
    for (crd::i64 i = 1; i <= 10; ++i)
    {
        crd::perf::counter_set_i64(id, i);
        CRD_PERF_FRAME_MARK();
    }
    // History ring capped at 4 frames.
    CHECK(crd::perf::frame_record_count() == 4U);
    CHECK(static_cast<crd::i64>(crd::perf::frame_record(0)->values[id.value].bits) == 10);
    CHECK(static_cast<crd::i64>(crd::perf::frame_record(3)->values[id.value].bits) == 7);
    CHECK(crd::perf::frame_record(4) == nullptr);

    crd::perf::shutdown();
}

TEST_CASE("FrameRecord.frame_begin_ns chains to previous frame_end_ns",
          "[perf][counter][frame][chain]")
{
    PerfFixture fx;
    CRD_PERF_FRAME_MARK();
    CRD_PERF_FRAME_MARK();
    const auto* prev = crd::perf::frame_record(1U);
    const auto* curr = crd::perf::frame_record(0U);
    REQUIRE(prev != nullptr);
    REQUIRE(curr != nullptr);
    CHECK(curr->frame_begin_ns == prev->frame_end_ns);
    CHECK(curr->frame_index == prev->frame_index + 1U);
}

TEST_CASE("f64 values round-trip through the FrameRecord ring",
          "[perf][counter][frame][f64]")
{
    PerfFixture fx;
    const auto id = crd::perf::register_counter_f64("f64.snapshot", crd::perf::CounterKind::Set);
    crd::perf::counter_set_f64(id, 1.25);
    CRD_PERF_FRAME_MARK();
    const auto* rec = crd::perf::frame_record(0U);
    REQUIRE(rec != nullptr);
    CHECK(std::bit_cast<crd::f64>(rec->values[id.value].bits) == 1.25);
}

#endif // CRD_PERF_ENABLED
