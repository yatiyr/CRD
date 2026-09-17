// DIAG.6b(i): the frame-graph -> crd-perf GPU bridge, verified against a FakeFrameGraph.
//
// The bridge turns an IFrameGraph's per-pass GPU timings into resolved crd-perf GPU spans on per-(device,queue)
// tracks. The proofs here are the same shape a real-device test would take; the DEFAULT-UNAVAILABLE case is the
// verdict this box actually gives today, because no raster backend overrides pass_gpu_ticks yet (DIAG.6b(i2)):
//   1. timing available but no ticks retained  -> every pass counted unavailable, ZERO positioned samples;
//   2. all passes carry ticks                  -> one positioned span per pass, on the mapped track + kind, uncalibrated;
//   3. mixed                                   -> available placed, unavailable counted;
//   4. no new execute across frames            -> dedupe, spans emitted exactly once;
//   5. device reports no timestamp support     -> the bridge does nothing (and counts nothing).
// Fixed C arrays only (the container hygiene gate applies to tests too). Proof lanes: win-debug + win-asan.
#include <crd/gpu/frame_graph.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/gpu/frame_graph_gpu_backend.hpp>
#include <crd/perf/gpu_scope.hpp>
#include <crd/perf/perf.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#if CRD_PERF_ENABLED

namespace
{
[[nodiscard]] crd::u32 find_track(const crd::perf::CaptureView& v, const char* name) noexcept
{
    for (crd::u32 i = 0U; i < v.thread_count(); ++i)
    {
        const char* const n = v.thread_name(i);
        if (n != nullptr && std::strcmp(n, name) == 0)
        {
            return i;
        }
    }
    return 0xFFFF'FFFFU;
}

[[nodiscard]] crd::u32 track_sample_count(const crd::perf::CaptureView& v, const char* name) noexcept
{
    const crd::u32 idx = find_track(v, name);
    return idx == 0xFFFF'FFFFU ? 0U : static_cast<crd::u32>(v.thread_samples(idx).size());
}

// A pass builder that no-ops every fluent setter — the bridge never records passes, so this only has to satisfy the
// pure-virtual surface that add_pass() returns.
class FakePassBuilder final : public crd::gpu::IFramePassBuilder
{
public:
    crd::gpu::IFramePassBuilder& reads(crd::gpu::FgImage) override { return *this; }
    crd::gpu::IFramePassBuilder& reads(crd::gpu::FgBuffer) override { return *this; }
    crd::gpu::IFramePassBuilder& writes(crd::gpu::FgImage) override { return *this; }
    crd::gpu::IFramePassBuilder& writes(crd::gpu::FgBuffer) override { return *this; }
    crd::gpu::IFramePassBuilder& read_writes(crd::gpu::FgImage) override { return *this; }
    crd::gpu::IFramePassBuilder& read_writes(crd::gpu::FgBuffer) override { return *this; }
    crd::gpu::IFramePassBuilder& reads_depth(crd::gpu::FgImage) override { return *this; }
    crd::gpu::IFramePassBuilder& execute(crd::gpu::FgExecuteFn, void*) override { return *this; }
    crd::gpu::IFramePassBuilder& present(crd::gpu::IPresentSurface&) override { return *this; }
};

// A frame graph whose ONLY live behaviour is the DIAG.6b(i) timing introspection, driven from a small table. Every
// other pure virtual is an inert stub — the bridge never calls them.
class FakeFrameGraph final : public crd::gpu::IFrameGraph
{
public:
    static constexpr crd::u32 kMax = 8U;

    void add(const char* name, bool avail, crd::u64 begin_ticks, crd::u64 end_ticks, crd::gpu::FgPassKind kind,
             crd::gpu::FgQueue queue) noexcept
    {
        if (m_pass_count >= kMax)
        {
            return;
        }
        const crd::u32 i = m_pass_count;
        m_names[i]       = name;
        m_avail[i]       = avail;
        m_begin[i]       = begin_ticks;
        m_end[i]         = end_ticks;
        m_kind[i]        = kind;
        m_queue[i]       = queue;
        ++m_pass_count;
    }

    // ── DIAG.6b(i) timing introspection (the live surface) ──
    [[nodiscard]] bool        gpu_timing_available() const noexcept override { return m_timing_available; }
    [[nodiscard]] crd::u32    pass_count() const noexcept override { return m_pass_count; }
    [[nodiscard]] const char* pass_name(crd::u32 i) const noexcept override
    {
        return i < m_pass_count ? m_names[i] : nullptr;
    }
    [[nodiscard]] bool pass_gpu_ticks(crd::u32 i, crd::u64& begin_ticks, crd::u64& end_ticks) const noexcept override
    {
        if (i >= m_pass_count || !m_avail[i])
        {
            return false;
        }
        begin_ticks = m_begin[i];
        end_ticks   = m_end[i];
        return true;
    }
    [[nodiscard]] double   gpu_timestamp_period_ns() const noexcept override { return m_period; }
    [[nodiscard]] crd::u32 gpu_timestamp_valid_bits() const noexcept override { return m_valid_bits; }
    [[nodiscard]] double   gpu_ms_total() const noexcept override { return m_gpu_ms_total; } // (j) independent reference
    [[nodiscard]] crd::gpu::FgPassKind pass_kind(crd::u32 i) const noexcept override
    {
        return i < m_pass_count ? m_kind[i] : crd::gpu::FgPassKind::Raster;
    }
    [[nodiscard]] crd::gpu::FgQueue pass_queue(crd::u32 i) const noexcept override
    {
        return i < m_pass_count ? m_queue[i] : crd::gpu::FgQueue::Graphics;
    }

    // ── inert stubs (never invoked by the bridge) ──
    [[nodiscard]] crd::gpu::FgImage  import_target(crd::gpu::IRasterTarget&) override { return crd::gpu::FgImage{0U}; }
    [[nodiscard]] crd::gpu::FgBuffer import_storage(crd::gpu::IStorageBuffer&) override { return crd::gpu::FgBuffer{0U}; }
    [[nodiscard]] crd::gpu::FgImage  create_transient_image(const crd::gpu::FgImageDesc&) override
    {
        return crd::gpu::FgImage{0U};
    }
    [[nodiscard]] crd::gpu::FgBuffer create_transient_buffer(crd::u32) override { return crd::gpu::FgBuffer{0U}; }
    [[nodiscard]] crd::gpu::IFramePassBuilder& add_pass(const char*, crd::gpu::FgPassKind) override
    {
        return m_builder;
    }
    [[nodiscard]] bool     build() override { return true; }
    void                   execute() override {}
    void                   reset() override {}
    [[nodiscard]] crd::u32 last_barrier_count() const noexcept override { return 0U; }
    [[nodiscard]] crd::u32 last_submit_count() const noexcept override { return 0U; }
    [[nodiscard]] crd::u32 transient_memory_bytes() const noexcept override { return 0U; }
    [[nodiscard]] crd::u32 transient_logical_bytes() const noexcept override { return 0U; }

    bool                 m_timing_available = true;
    crd::u32             m_pass_count       = 0U;
    const char*          m_names[kMax]{};
    bool                 m_avail[kMax]{};
    crd::u64             m_begin[kMax]{};
    crd::u64             m_end[kMax]{};
    crd::gpu::FgPassKind m_kind[kMax]{};
    crd::gpu::FgQueue    m_queue[kMax]{};
    double               m_period       = 2.0;
    crd::u32             m_valid_bits   = 64U;
    double               m_gpu_ms_total = 0.0; // (j) the frame graph's own independent total (ms)
    FakePassBuilder      m_builder{};
};

[[nodiscard]] crd::perf::CaptureView save_view(crd::memory::GrowableTlsfAllocator& cap,
                                               crd::containers::Array<crd::u8>& out) noexcept
{
    out = crd::perf::save_capture_to_buffer(&cap);
    return crd::perf::CaptureView{crd::containers::ConstSpan<crd::u8>{out.data(), out.size()}};
}
} // namespace

TEST_CASE("gpu-bridge-real: timing available but no ticks retained -> every pass counted unavailable, zero samples",
          "[perf][diag][gpu-bridge-real]")
{
    // This is the verdict a REAL device gives today: no raster backend overrides pass_gpu_ticks (DIAG.6b(i2)), so
    // every pass is unplaceable. The bridge counts each one and invents nothing.
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "bridge-unavail"};
    crd::perf::init({});
    FakeFrameGraph fg;
    fg.m_timing_available = true;
    fg.add("shadow", false, 0U, 0U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
    fg.add("gbuffer", false, 0U, 0U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
    fg.add("light", false, 0U, 0U, crd::gpu::FgPassKind::Compute, crd::gpu::FgQueue::Graphics);

    crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
    crd::perf::set_gpu_backend(&bridge);
    crd::perf::frame_mark();
    crd::perf::frame_mark();

    CHECK(crd::perf::gpu_timestamps_unavailable_count() == 3U); // one per pass, once (dedupe suppresses the re-resolve)
    CHECK(bridge.emitted_spans() == 0U);

    crd::containers::Array<crd::u8> buf;
    const crd::perf::CaptureView    view = save_view(cap, buf);
    REQUIRE(view.is_valid());
    CHECK(track_sample_count(view, "gpu d0 q0") == 0U); // TEETH-B: fabricate a begin=0 span -> a track appears here

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

TEST_CASE("gpu-bridge-real: all passes carry ticks -> one positioned span each, mapped track+kind, uncalibrated",
          "[perf][diag][gpu-bridge-real]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "bridge-all"};
    crd::perf::init({});
    FakeFrameGraph fg;
    fg.m_period = 2.0;
    fg.add("gbuffer", true, 100U, 200U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);  // q0, Gpu, 200ns
    fg.add("light", true, 300U, 450U, crd::gpu::FgPassKind::Compute, crd::gpu::FgQueue::Async);      // q1, Gpu, 300ns
    fg.add("readback", true, 500U, 560U, crd::gpu::FgPassKind::Transfer, crd::gpu::FgQueue::Graphics); // q0, Io, 120ns

    crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
    crd::perf::set_gpu_backend(&bridge);
    crd::perf::frame_mark();
    crd::perf::frame_mark();

    CHECK(bridge.emitted_spans() == 3U);
    CHECK(crd::perf::uncalibrated_span_count() == 3U); // no calibration installed -> all land in the raw GPU-tick domain

    crd::containers::Array<crd::u8> buf;
    const crd::perf::CaptureView    view = save_view(cap, buf);
    REQUIRE(view.is_valid());

    // The async pass lands on its OWN track; the two graphics passes share q0.
    CHECK(track_sample_count(view, "gpu d0 q0") == 2U);
    CHECK(track_sample_count(view, "gpu d0 q1") == 1U); // TEETH-A: map async->graphics -> this collapses to 0

    // q0: a Gpu-category 200ns span (raster) and an Io-category 120ns span (transfer), order-independent.
    const crd::u32 q0 = find_track(view, "gpu d0 q0");
    REQUIRE(q0 != 0xFFFF'FFFFU);
    const auto s0 = view.thread_samples(q0);
    REQUIRE(s0.size() == 2U);
    bool saw_raster = false;
    bool saw_io     = false;
    for (crd::u32 i = 0U; i < 2U; ++i)
    {
        const crd::i64 dur = s0[i].end_ns - s0[i].begin_ns;
        if (s0[i].category == static_cast<crd::u8>(crd::perf::Category::Gpu) && dur == 200)
        {
            saw_raster = true;
        }
        if (s0[i].category == static_cast<crd::u8>(crd::perf::Category::Io) && dur == 120)
        {
            saw_io = true;
        }
    }
    CHECK(saw_raster);
    CHECK(saw_io);

    // q1: the async compute pass, 300ns, uncalibrated (sentinel uncertainty, calibrated flag clear, right queue id).
    const crd::u32 q1 = find_track(view, "gpu d0 q1");
    REQUIRE(q1 != 0xFFFF'FFFFU);
    const auto s1 = view.thread_samples(q1);
    REQUIRE(s1.size() == 1U);
    CHECK(s1[0].category == static_cast<crd::u8>(crd::perf::Category::Gpu));
    CHECK(s1[0].end_ns - s1[0].begin_ns == 300);
    const auto* rec = view.correlation_for(q1, 0U);
    REQUIRE(rec != nullptr);
    CHECK(rec->queue_id == crd::perf::kGpuQueueAsyncCompute);
    CHECK(rec->device_id == 0U);
    CHECK((rec->flags & crd::perf::kCorrelationCalibrated) == 0U);
    CHECK(rec->clock_uncertainty_ns == crd::perf::kUnknownClockUncertainty);
    CHECK(rec->pass_id == crd::perf::intern_name("light").value);

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

TEST_CASE("gpu-bridge-real: mixed availability -> available passes placed, unavailable passes counted",
          "[perf][diag][gpu-bridge-real]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "bridge-mixed"};
    crd::perf::init({});
    FakeFrameGraph fg;
    fg.add("a", true, 100U, 200U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
    fg.add("b", false, 0U, 0U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);   // unplaceable
    fg.add("c", true, 500U, 560U, crd::gpu::FgPassKind::Transfer, crd::gpu::FgQueue::Graphics);

    crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
    crd::perf::set_gpu_backend(&bridge);
    crd::perf::frame_mark();
    crd::perf::frame_mark();

    CHECK(bridge.emitted_spans() == 2U);
    CHECK(crd::perf::gpu_timestamps_unavailable_count() == 1U);

    crd::containers::Array<crd::u8> buf;
    const crd::perf::CaptureView    view = save_view(cap, buf);
    REQUIRE(view.is_valid());
    CHECK(track_sample_count(view, "gpu d0 q0") == 2U);

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

TEST_CASE("gpu-bridge-real: no new execute across frames -> spans emitted exactly once (dedupe)",
          "[perf][diag][gpu-bridge-real]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "bridge-dedup"};
    crd::perf::init({});
    FakeFrameGraph fg;
    fg.add("a", true, 100U, 200U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);

    crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
    crd::perf::set_gpu_backend(&bridge);
    crd::perf::frame_mark();
    crd::perf::frame_mark();
    crd::perf::frame_mark(); // three resolves, one unchanged execute

    CHECK(bridge.emitted_spans() == 1U); // TEETH: drop the fingerprint dedupe -> becomes 3

    crd::containers::Array<crd::u8> buf;
    const crd::perf::CaptureView    view = save_view(cap, buf);
    REQUIRE(view.is_valid());
    CHECK(track_sample_count(view, "gpu d0 q0") == 1U);

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

TEST_CASE("gpu-bridge-real: device reports no timestamp support -> the bridge does nothing",
          "[perf][diag][gpu-bridge-real]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "bridge-notiming"};
    crd::perf::init({});
    FakeFrameGraph fg;
    fg.m_timing_available = false; // device does not support timestamps at all
    fg.add("a", true, 100U, 200U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);

    crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
    crd::perf::set_gpu_backend(&bridge);
    crd::perf::frame_mark();
    crd::perf::frame_mark();

    CHECK(bridge.emitted_spans() == 0U);
    // Not a per-pass query-read failure: device-level "no timing" counts nothing (it is a different signal).
    CHECK(crd::perf::gpu_timestamps_unavailable_count() == 0U);

    crd::containers::Array<crd::u8> buf;
    const crd::perf::CaptureView    view = save_view(cap, buf);
    REQUIRE(view.is_valid());
    CHECK(track_sample_count(view, "gpu d0 q0") == 0U);

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

// ── DIAG.6b(j): compare the placed GPU total against the frame graph's own independent reduction (gpu_ms_total). ──
// period == 2.0 throughout, so placed_total_ns == span_ticks * 2; the tolerance is gpu_round_ns(2.0) + 1 == 3 ns.

TEST_CASE("gpu-total: placed total matches the frame graph's own independent reduction (span, gaps included)",
          "[perf][diag][gpu-total]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "total-match"};
    crd::perf::init({});
    FakeFrameGraph fg;
    fg.add("a", true, 100U, 200U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
    fg.add("b", true, 300U, 450U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
    // span = 450 - 100 = 350 ticks -> 700 ns; reference set to exactly that.
    fg.m_gpu_ms_total = 700.0 / 1.0e6;

    crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
    crd::perf::set_gpu_backend(&bridge);
    crd::perf::frame_mark();
    crd::perf::frame_mark();

    CHECK(bridge.total_match_count() == 1U);       // TEETH-D: convert with period*2 -> discrepancy 700 -> flips
    CHECK(bridge.total_mismatch_count() == 0U);
    CHECK(bridge.total_incomparable_count() == 0U);
    CHECK(bridge.last_total_discrepancy_ns() == 0);

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

TEST_CASE("gpu-total: a reference off by more than tolerance is a counted mismatch (magnitude only)",
          "[perf][diag][gpu-total]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "total-mismatch"};
    crd::perf::init({});
    FakeFrameGraph fg;
    fg.add("a", true, 100U, 200U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
    fg.add("b", true, 300U, 450U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
    fg.m_gpu_ms_total = (700.0 + 1000.0) / 1.0e6; // reference 1700 ns; placed 700 -> discrepancy -1000

    crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
    crd::perf::set_gpu_backend(&bridge);
    crd::perf::frame_mark();
    crd::perf::frame_mark();

    CHECK(bridge.total_mismatch_count() == 1U);
    CHECK(bridge.total_match_count() == 0U);
    CHECK(bridge.last_total_discrepancy_ns() == -1000); // magnitude+sign of placed - reference, no per-pass blame

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

TEST_CASE("gpu-total: the tolerance boundary -- at tolerance matches, one ns past it mismatches",
          "[perf][diag][gpu-total]")
{
    // tolerance_ns = gpu_round_ns(2.0) + 1 == 3.
    SECTION("exactly at tolerance -> match")
    {
        crd::perf::init({});
        FakeFrameGraph fg;
        fg.add("a", true, 100U, 200U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
        fg.add("b", true, 300U, 450U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
        fg.m_gpu_ms_total = (700.0 + 3.0) / 1.0e6; // discrepancy -3, |−3| <= 3
        crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
        crd::perf::set_gpu_backend(&bridge);
        crd::perf::frame_mark();
        crd::perf::frame_mark();
        CHECK(bridge.total_match_count() == 1U);
        CHECK(bridge.total_mismatch_count() == 0U);
        crd::perf::set_gpu_backend(nullptr);
        crd::perf::shutdown();
    }
    SECTION("one ns past tolerance -> mismatch")
    {
        crd::perf::init({});
        FakeFrameGraph fg;
        fg.add("a", true, 100U, 200U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
        fg.add("b", true, 300U, 450U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
        fg.m_gpu_ms_total = (700.0 + 4.0) / 1.0e6; // discrepancy -4, |−4| > 3
        crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
        crd::perf::set_gpu_backend(&bridge);
        crd::perf::frame_mark();
        crd::perf::frame_mark();
        CHECK(bridge.total_mismatch_count() == 1U);
        CHECK(bridge.total_match_count() == 0U);
        crd::perf::set_gpu_backend(nullptr);
        crd::perf::shutdown();
    }
}

TEST_CASE("gpu-total: an incomplete total is counted incomparable, never compared (the box's real verdict today)",
          "[perf][diag][gpu-total]")
{
    crd::perf::init({});
    FakeFrameGraph fg;
    fg.add("a", true, 100U, 200U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
    fg.add("b", false, 0U, 0U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics); // unplaceable
    fg.add("c", true, 500U, 560U, crd::gpu::FgPassKind::Transfer, crd::gpu::FgQueue::Graphics);
    fg.m_gpu_ms_total = 0.001; // present, but the placed total is incomplete -> must not be compared

    crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
    crd::perf::set_gpu_backend(&bridge);
    crd::perf::frame_mark();
    crd::perf::frame_mark();

    CHECK(bridge.total_incomparable_count() == 1U);
    CHECK(bridge.total_match_count() == 0U);
    CHECK(bridge.total_mismatch_count() == 0U);

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

TEST_CASE("gpu-total: with a gap between passes the reduction is the SPAN (gaps included), not the sum of durations",
          "[perf][diag][gpu-total]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "total-span"};
    crd::perf::init({});
    FakeFrameGraph fg;
    fg.add("a", true, 100U, 200U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);   // dur 100
    fg.add("b", true, 500U, 560U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);   // dur 60, gap 200..500
    // span = 560 - 100 = 460 ticks -> 920 ns; sum of durations would be (100+60)*2 = 320 ns.
    fg.m_gpu_ms_total = 920.0 / 1.0e6;

    crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
    crd::perf::set_gpu_backend(&bridge);
    crd::perf::frame_mark();
    crd::perf::frame_mark();

    CHECK(bridge.total_match_count() == 1U);    // TEETH-C: sum-of-durations reduction -> placed 320 -> mismatch -> flips
    CHECK(bridge.total_mismatch_count() == 0U);

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

TEST_CASE("gpu-total: incomparable is counted PER FRAME on the real-device path, not once per process",
          "[perf][diag][gpu-total]")
{
    // The (i2)-pending reality: every pass unavailable, so the per-pass fingerprint bits are identical frame to frame.
    // gpu_ms_total is folded into the fingerprint, so two genuine executes (distinct totals) are two distinct frames
    // and the incomparable verdict is counted twice -- not swallowed by dedupe.
    crd::perf::init({});
    FakeFrameGraph fg;
    fg.add("a", false, 0U, 0U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);
    fg.add("b", false, 0U, 0U, crd::gpu::FgPassKind::Raster, crd::gpu::FgQueue::Graphics);

    crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
    crd::perf::set_gpu_backend(&bridge);
    fg.m_gpu_ms_total = 0.001; // frame 0's independent total
    crd::perf::frame_mark();
    fg.m_gpu_ms_total = 0.002; // a genuine new execute -> a distinct frame
    crd::perf::frame_mark();

    CHECK(bridge.total_incomparable_count() == 2U); // per frame; folding gpu_ms_total in prevents the dedupe swallow
    CHECK(bridge.total_match_count() == 0U);
    CHECK(bridge.total_mismatch_count() == 0U);

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

TEST_CASE("gpu-total: an empty frame (no passes) counts nothing -- not incomparable", "[perf][diag][gpu-total]")
{
    crd::perf::init({});
    FakeFrameGraph fg; // timing available, but zero passes
    fg.m_gpu_ms_total = 0.001;

    crd::perf::gpu::FrameGraphGpuBackend bridge{fg, 0U};
    crd::perf::set_gpu_backend(&bridge);
    crd::perf::frame_mark();
    crd::perf::frame_mark();

    CHECK(bridge.total_incomparable_count() == 0U);
    CHECK(bridge.total_match_count() == 0U);
    CHECK(bridge.total_mismatch_count() == 0U);
    CHECK(bridge.emitted_spans() == 0U);

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

#endif // CRD_PERF_ENABLED
