// crd-perf-ui v0g -- LiveProfilerSource + CaptureViewSource round-trip.

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/perf.hpp>
#include <crd/perf/ui/ui.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#if CRD_PERF_ENABLED

namespace
{

struct PerfFixture
{
    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "perf-ui-test"};
    PerfFixture() { crd::perf::init({}); }
    ~PerfFixture() { crd::perf::shutdown(); }
};

} // namespace

TEST_CASE("LiveProfilerSource forwards counts to the global profiler",
          "[perf-ui][source][live]")
{
    PerfFixture fx;
    crd::perf::ui::LiveProfilerSource src;
    CHECK(src.is_live());
    CHECK(std::strcmp(src.source_label(), "live") == 0);
    CHECK(src.thread_count() == crd::perf::thread_count());
    CHECK(src.counter_count() == crd::perf::counter_count());
    CHECK(src.allocator_count() == crd::perf::registered_allocator_count());
    CHECK(src.frame_record_count() == crd::perf::frame_record_count());
}

TEST_CASE("LiveProfilerSource resolves names through the live intern table",
          "[perf-ui][source][live]")
{
    PerfFixture fx;
    const auto id = crd::perf::intern_name("ui_resolve_live");
    crd::perf::ui::LiveProfilerSource src;
    CHECK(std::strcmp(src.resolve_name(id), "ui_resolve_live") == 0);
}

TEST_CASE("CaptureViewSource round-trips a saved capture",
          "[perf-ui][source][capture-view]")
{
    PerfFixture fx;
    {
        CRD_PERF_SCOPE("ui_test_alpha");
    }
    {
        CRD_PERF_SCOPE("ui_test_beta");
    }
    const auto cid = crd::perf::register_counter_i64("ui_counter", crd::perf::CounterKind::Set);
    crd::perf::counter_set_i64(cid, 99);

    crd::memory::TlsfAllocator tlsf{1U << 16, nullptr, "ui_alloc"};
    [[maybe_unused]] const auto aid = crd::perf::register_allocator("ui_alloc", &tlsf);
    void* p = tlsf.allocate(2048U);
    REQUIRE(p != nullptr);

    CRD_PERF_FRAME_MARK();

    auto buf = crd::perf::save_capture_to_buffer(&fx.alloc);
    REQUIRE(buf.size() > 0U);
    crd::perf::CaptureView view{
        crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());

    crd::perf::ui::CaptureViewSource src{view};
    CHECK_FALSE(src.is_live());
    CHECK(std::strcmp(src.source_label(), "capture") == 0);
    CHECK(src.thread_count() == view.thread_count());
    CHECK(src.counter_count() >= 1U);
    CHECK(src.allocator_count() >= 1U);
    CHECK(src.frame_record_count() >= 1U);
    // Counter[cid] metadata round-trips: the name is recorded in
    // CounterMeta, not NameBlob, so look it up via counter_info().
    const auto cinfo = src.counter_info(cid.value);
    CHECK(std::strcmp(cinfo.name, "ui_counter") == 0);
    // Most-recent frame_record(0) returns a valid record.
    const auto* rec = src.frame_record(0U);
    REQUIRE(rec != nullptr);
    CHECK(rec->counter_count >= 1U);

    tlsf.deallocate(p);
}

TEST_CASE("CaptureViewSource gpu_thread_index returns 0xFF when no 'gpu' thread present",
          "[perf-ui][source][capture-view][gpu]")
{
    PerfFixture fx;
    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "capture-no-gpu"};
    auto buf = crd::perf::save_capture_to_buffer(&alloc);
    crd::perf::CaptureView view{
        crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    crd::perf::ui::CaptureViewSource src{view};
    CHECK(src.gpu_thread_index() == 0xFFU);
}

TEST_CASE("ProfilerPanel default source is the live profiler",
          "[perf-ui][panel]")
{
    PerfFixture fx;
    crd::perf::ui::ProfilerPanel panel{&fx.alloc};
    CHECK(panel.current_source().is_live());
    panel.set_source(nullptr); // reset = live
    CHECK(panel.current_source().is_live());
}

#endif // CRD_PERF_ENABLED
