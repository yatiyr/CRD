// DIAG.6b(e): frame_mark() drives the GPU frame lifecycle (fix the disconnected assembly). Per CPU frame it calls the
// installed backend's end_frame() -> resolve_completed_frames() -> begin_frame(next); set_gpu_backend opens the first
// frame on install. A fake frame-graph producer records per-pass spans and, on resolve, emits them via the tick-native
// (d) path -- so a single stream of frame_mark() calls (no manual resolve) drives the whole (b)->(c)->(d) chain end to
// end. Oracles: lifecycle call counts + indices; spans appear only after their frame is out of flight; routing by the
// backend-agnostic queue contract onto the right (device,queue) track + category; resolve_gpu_frames stays resolve-only.
// Proof lanes: win-debug + win-asan.
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/gpu_scope.hpp>
#include <crd/perf/perf.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#if CRD_PERF_ENABLED

namespace
{
constexpr crd::f64 kPeriod = 2.0;

// A fake frame-graph GPU backend. It does NOT use the begin_span/end_span RAII path (that is the mock's job); instead a
// test records per-frame spans directly, and resolve emits the frame that is now out of flight (2 frames deep) via the
// tick-native emit_gpu_span_on. Fixed C arrays only (tests are container-checked); single-threaded, so plain counters.
class FakeFrameGraphBackend final : public crd::perf::IProfilerGpuBackend
{
public:
    static constexpr crd::u32 kFrames = 8U;   // ring of tracked frames (index % kFrames)
    static constexpr crd::u32 kSpans  = 8U;   // spans per frame
    static constexpr crd::u32 kInFlight = 2U; // a frame resolves once this many are submitted ahead of it

    struct Span
    {
        crd::u32                 queue;
        crd::perf::GpuSampleKind kind;
        crd::perf::NameId        name;
        crd::u64                 begin_ticks;
        crd::u64                 end_ticks;
    };

    // Test-side: stage a span for `frame`.
    void record(crd::u64 frame, crd::u32 queue, crd::perf::GpuSampleKind kind, crd::perf::NameId name,
                crd::u64 begin_ticks, crd::u64 end_ticks) noexcept
    {
        const crd::u32 slot = static_cast<crd::u32>(frame % kFrames);
        if (m_count[slot] < kSpans)
        {
            m_spans[slot][m_count[slot]] = Span{queue, kind, name, begin_ticks, end_ticks};
            ++m_count[slot];
        }
    }

    void begin_frame(crd::u64 frame_index) noexcept override
    {
        ++begin_frame_calls;
        last_begin_index = frame_index;
        m_current        = frame_index;
    }

    [[nodiscard]] crd::perf::GpuSpanHandle begin_span(void*, crd::perf::NameId) noexcept override
    {
        return crd::perf::kInvalidGpuSpan; // unused by this producer
    }
    void end_span(void*, crd::perf::GpuSpanHandle) noexcept override {}

    void end_frame() noexcept override
    {
        ++end_frame_calls;
        m_submitted[m_sub_tail % kFrames] = m_current;
        ++m_sub_tail;
    }

    void resolve_completed_frames() noexcept override
    {
        ++resolve_calls;
        // A frame is out of flight once kInFlight frames have been submitted.
        while ((m_sub_tail - m_sub_head) >= kInFlight)
        {
            const crd::u64 frame = m_submitted[m_sub_head % kFrames];
            ++m_sub_head;
            const crd::u32 slot = static_cast<crd::u32>(frame % kFrames);
            for (crd::u32 i = 0U; i < m_count[slot]; ++i)
            {
                const Span& s = m_spans[slot][i];
                crd::perf::emit_gpu_span_on(crd::perf::GpuTrackKey{0U, s.queue}, s.kind, s.name, s.begin_ticks,
                                            s.end_ticks, ns_per_tick());
            }
            m_count[slot] = 0U;
        }
    }

    [[nodiscard]] crd::f64 ns_per_tick() const noexcept override { return kPeriod; }

    crd::u32 begin_frame_calls  = 0U;
    crd::u32 end_frame_calls    = 0U;
    crd::u32 resolve_calls      = 0U;
    crd::u64 last_begin_index   = 0U;

private:
    Span     m_spans[kFrames][kSpans]{};
    crd::u32 m_count[kFrames]{};
    crd::u64 m_submitted[kFrames]{};
    crd::u32 m_sub_head = 0U;
    crd::u32 m_sub_tail = 0U;
    crd::u64 m_current  = 0U;
};

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

// Install a calibration for device 0 so emitted spans are flag-calibrated with an honest bound.
void install_calibration() noexcept
{
    crd::time::GpuClockCalibration c{};
    c.cpu_ns           = 0;
    c.gpu_ticks        = 0;
    c.ns_per_tick      = kPeriod;
    c.max_deviation_ns = 10U;
    crd::perf::set_gpu_clock_calibration(0U, c);
}
} // namespace

TEST_CASE("gpu-bridge: frame_mark drives end/resolve/begin; install opens the first frame", "[perf][diag][gpu-bridge]")
{
    crd::perf::init({});
    FakeFrameGraphBackend be;
    crd::perf::set_gpu_backend(&be);

    // Install opened frame 0.
    CHECK(be.begin_frame_calls == 1U);
    CHECK(be.last_begin_index == crd::perf::frame_count());
    CHECK(be.end_frame_calls == 0U);
    CHECK(be.resolve_calls == 0U);

    // Three frame marks, NO manual resolve. TEETH: unwire gpu_frame_advance -> these stay at 1/0/0. Each mark opens the
    // next frame at the new frame_count(), so the begin index tracks consecutively.
    for (crd::u32 k = 0U; k < 3U; ++k)
    {
        crd::perf::frame_mark();
        CHECK(be.last_begin_index == crd::perf::frame_count());
    }
    CHECK(be.end_frame_calls == 3U);
    CHECK(be.resolve_calls == 3U);
    CHECK(be.begin_frame_calls == 4U); // install + 3 marks

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

TEST_CASE("gpu-bridge: a recorded frame's spans reach their tracks only after it is out of flight, driven by frame_mark",
          "[perf][diag][gpu-bridge]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-bridge-cap"};
    crd::perf::init({});
    install_calibration();
    FakeFrameGraphBackend be;
    crd::perf::set_gpu_backend(&be);

    const crd::perf::NameId raster = crd::perf::intern_name("raster");
    const crd::perf::NameId copy   = crd::perf::intern_name("copy");
    // Frame 0: a graphics-queue execution pass and a transfer-queue copy.
    be.record(0U, crd::perf::kGpuQueueGraphics, crd::perf::GpuSampleKind::Execution, raster, 100U, 200U);
    be.record(0U, crd::perf::kGpuQueueTransfer, crd::perf::GpuSampleKind::Transfer, copy, 300U, 350U);

    // After ONE mark the frame is still in flight (kInFlight == 2): nothing emitted yet.
    crd::perf::frame_mark();
    {
        const auto buf = crd::perf::save_capture_to_buffer(&cap);
        const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
        REQUIRE(view.is_valid());
        // The default (0,0) track exists (install pre-registered it) but holds no samples yet (positive control for the
        // second-mark check below).
        const crd::u32 g = find_track(view, "gpu d0 q0");
        REQUIRE(g != 0xFFFF'FFFFU);
        CHECK(view.thread_samples(g).size() == 0U);
        // The transfer track registers lazily on first emit -- its absence is the strongest "nothing emitted yet".
        CHECK(find_track(view, "gpu d0 q2") == 0xFFFF'FFFFU);
    }

    // A second mark takes frame 0 out of flight -> its spans resolve and route to their tracks.
    crd::perf::frame_mark();
    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());

    const crd::u32 gq = find_track(view, "gpu d0 q0"); // graphics execution
    REQUIRE(gq != 0xFFFF'FFFFU);
    const auto gs = view.thread_samples(gq);
    REQUIRE(gs.size() == 1U);
    CHECK(gs[0].category == static_cast<crd::u8>(crd::perf::Category::Gpu));
    const auto* grec = view.correlation_for(gq, 0U);
    REQUIRE(grec != nullptr);
    CHECK(grec->queue_id == crd::perf::kGpuQueueGraphics);
    CHECK((grec->flags & crd::perf::kCorrelationCalibrated) != 0U);
    CHECK(grec->clock_uncertainty_ns != crd::perf::kUnknownClockUncertainty);

    const crd::u32 tq = find_track(view, "gpu d0 q2"); // transfer copy
    REQUIRE(tq != 0xFFFF'FFFFU);
    const auto ts = view.thread_samples(tq);
    REQUIRE(ts.size() == 1U);
    CHECK(ts[0].category == static_cast<crd::u8>(crd::perf::Category::Io));
    const auto* trec = view.correlation_for(tq, 0U);
    REQUIRE(trec != nullptr);
    CHECK(trec->queue_id == crd::perf::kGpuQueueTransfer);

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

TEST_CASE("gpu-bridge: resolve_gpu_frames is resolve-only (no end/begin), idempotent alongside frame_mark",
          "[perf][diag][gpu-bridge]")
{
    crd::perf::init({});
    FakeFrameGraphBackend be;
    crd::perf::set_gpu_backend(&be); // begin_frame_calls == 1

    crd::perf::frame_mark();          // end==1, resolve==1, begin==2
    crd::perf::resolve_gpu_frames();  // resolve-only: resolve==2, end and begin unchanged

    CHECK(be.end_frame_calls == 1U);
    CHECK(be.begin_frame_calls == 2U);
    CHECK(be.resolve_calls == 2U);

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

#endif // CRD_PERF_ENABLED
