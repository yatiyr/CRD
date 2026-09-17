// DIAG.6b(f): resolve robustness + counts. A narrow (sub-64-bit) timestamp counter that wraps within a frame is
// repaired into a forward span (counted); an end<begin that is not a repairable wrap is dropped (counted), never a
// negative span. A backend reports what only it can see -- unreadable query results, a query slot reused before it
// resolved, and device loss (pending frames dropped) -- via note_* counters; nothing is fabricated to keep a count at
// zero. A frame held past its normal resolve still lands exactly once. Proof lanes: win-debug + win-asan.
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/gpu_scope.hpp>
#include <crd/perf/perf.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>
#include <crd/time/gpu_timestamp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#if CRD_PERF_ENABLED

namespace
{
constexpr crd::f64 kPeriod = 2.0;

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

// A robustness-focused fake producer. Records per-frame spans; resolve emits the out-of-flight frame via the tick-native
// path, or reports a failure (unavailable / slot reuse / device loss) via the note_* counters. Fixed C arrays only.
class FakeRobustBackend final : public crd::perf::IProfilerGpuBackend
{
public:
    static constexpr crd::u32 kFrames   = 8U;
    static constexpr crd::u32 kSpans    = 4U; // small, so recording more than this forces slot reuse
    static constexpr crd::u32 kInFlight = 2U;

    struct Span
    {
        crd::u32                 queue;
        crd::perf::GpuSampleKind kind;
        crd::perf::NameId        name;
        crd::u64                 begin_ticks;
        crd::u64                 end_ticks;
    };

    void record(crd::u64 frame, crd::perf::NameId name, crd::u64 b, crd::u64 e) noexcept
    {
        const crd::u32 slot = static_cast<crd::u32>(frame % kFrames);
        if (m_count[slot] < kSpans)
        {
            m_spans[slot][m_count[slot]] = Span{crd::perf::kGpuQueueGraphics, crd::perf::GpuSampleKind::Execution, name,
                                                b, e};
            ++m_count[slot];
        }
        else
        {
            ++m_overflow[slot]; // the slot was reused before this frame resolved
        }
    }
    void set_unavailable(crd::u64 frame, crd::u32 n) noexcept { m_unavail[frame % kFrames] = n; }
    void hold_frame(crd::u64 frame, crd::u32 marks) noexcept { m_hold[frame % kFrames] = marks; }
    void lose_device() noexcept { m_device_lost = true; }

    void                       begin_frame(crd::u64 idx) noexcept override { ++begin_frame_calls; m_current = idx; }
    [[nodiscard]] crd::perf::GpuSpanHandle begin_span(void*, crd::perf::NameId) noexcept override
    {
        return crd::perf::kInvalidGpuSpan;
    }
    void        end_span(void*, crd::perf::GpuSpanHandle) noexcept override {}
    void        end_frame() noexcept override
    {
        ++end_frame_calls;
        m_sub_frame[m_sub_tail % kFrames] = m_current;
        m_sub_delay[m_sub_tail % kFrames] = 1U + m_hold[m_current % kFrames]; // normal = resolve on the next resolve
        ++m_sub_tail;
    }
    [[nodiscard]] crd::f64 ns_per_tick() const noexcept override { return kPeriod; }

    void resolve_completed_frames() noexcept override
    {
        ++resolve_calls;
        if (m_device_lost)
        {
            if (!m_loss_reported)
            {
                crd::perf::note_gpu_device_lost(0U, m_sub_tail - m_sub_head); // report loss + pending once
                m_loss_reported = true;
            }
            m_sub_head = m_sub_tail; // drop every pending frame; further resolves are silent no-ops
            return;
        }
        // In-order resolve: the head frame must age `delay` resolves before it emits (models delayed/out-of-order host
        // readback). Stop at the first not-yet-ready head so a held frame blocks the ones behind it.
        while (m_sub_head != m_sub_tail)
        {
            const crd::u32 h = m_sub_head % kFrames;
            if (m_sub_delay[h] > 0U)
            {
                --m_sub_delay[h];
                break;
            }
            const crd::u64 frame = m_sub_frame[h];
            const crd::u32 slot  = static_cast<crd::u32>(frame % kFrames);
            ++m_sub_head;
            if (m_unavail[slot] > 0U)
            {
                crd::perf::note_gpu_timestamps_unavailable(m_unavail[slot]); // query results not readable -> counted
            }
            else
            {
                if (m_overflow[slot] > 0U)
                {
                    crd::perf::note_gpu_query_slot_reused(m_overflow[slot]); // slot lapped before resolve -> counted
                }
                for (crd::u32 i = 0U; i < m_count[slot]; ++i)
                {
                    const Span& s = m_spans[slot][i];
                    crd::perf::emit_gpu_span_on(crd::perf::GpuTrackKey{0U, s.queue}, s.kind, s.name, s.begin_ticks,
                                                s.end_ticks, ns_per_tick());
                }
            }
            m_count[slot]    = 0U;
            m_overflow[slot] = 0U;
            m_unavail[slot]  = 0U;
        }
    }

    crd::u32 begin_frame_calls = 0U;
    crd::u32 end_frame_calls   = 0U;
    crd::u32 resolve_calls     = 0U;

private:
    Span     m_spans[kFrames][kSpans]{};
    crd::u32 m_count[kFrames]{};
    crd::u32 m_overflow[kFrames]{};
    crd::u32 m_unavail[kFrames]{};
    crd::u32 m_hold[kFrames]{};
    crd::u64 m_sub_frame[kFrames]{};
    crd::u32 m_sub_delay[kFrames]{};
    crd::u32 m_sub_head      = 0U;
    crd::u32 m_sub_tail      = 0U;
    crd::u64 m_current       = 0U;
    bool     m_device_lost   = false;
    bool     m_loss_reported = false;
};
} // namespace

TEST_CASE("gpu-resolve: a narrow-counter wrap is repaired into a forward span and counted (uncalibrated)",
          "[perf][diag][gpu-resolve]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-resolve-wrap"};
    crd::perf::init({});
    crd::perf::set_gpu_timestamp_valid_bits(0U, 32U); // 32-bit counter
    const crd::perf::NameId pass = crd::perf::intern_name("wrap");

    // begin near the top of the 32-bit range, end just past the wrap -> true duration 0x20 ticks.
    crd::perf::emit_gpu_span_on(crd::perf::GpuTrackKey{0U, 0U}, crd::perf::GpuSampleKind::Execution, pass,
                                0xFFFF'FFF0ULL, 0x10ULL, kPeriod);
    CHECK(crd::perf::gpu_wrap_repaired_count() == 1U); // TEETH: drop the modular delta -> this becomes invalid instead
    CHECK(crd::perf::gpu_invalid_span_count() == 0U);

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    const crd::u32 idx = find_track(view, "gpu d0 q0");
    REQUIRE(idx != 0xFFFF'FFFFU);
    const auto s = view.thread_samples(idx);
    REQUIRE(s.size() == 1U);
    CHECK(s[0].end_ns > s[0].begin_ns);                                       // TEETH: no repair -> end < begin
    CHECK(s[0].end_ns - s[0].begin_ns == 0x20LL * static_cast<crd::i64>(kPeriod)); // 32 ticks * period

    crd::perf::shutdown();
}

TEST_CASE("gpu-resolve: a wrap under calibration keeps a correct forward duration on the CPU timeline",
          "[perf][diag][gpu-resolve]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-resolve-wrap-cal"};
    crd::perf::init({});
    crd::perf::set_gpu_timestamp_valid_bits(0U, 32U);
    crd::time::GpuClockCalibration c{};
    c.cpu_ns           = 5'000'000;
    c.gpu_ticks        = 0xFFFF'FF00ULL; // calibrated near the wrap boundary
    c.ns_per_tick      = kPeriod;
    c.max_deviation_ns = 10U;
    crd::perf::set_gpu_clock_calibration(0U, c);
    const crd::perf::NameId pass = crd::perf::intern_name("wrapcal");

    crd::perf::emit_gpu_span_on(crd::perf::GpuTrackKey{0U, 0U}, crd::perf::GpuSampleKind::Execution, pass,
                                0xFFFF'FFF0ULL, 0x10ULL, kPeriod);
    CHECK(crd::perf::gpu_wrap_repaired_count() == 1U);

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    const crd::u32 idx = find_track(view, "gpu d0 q0");
    REQUIRE(idx != 0xFFFF'FFFFU);
    const auto s = view.thread_samples(idx);
    REQUIRE(s.size() == 1U);
    CHECK(s[0].end_ns > s[0].begin_ns);
    CHECK(s[0].end_ns - s[0].begin_ns == 0x20LL * static_cast<crd::i64>(kPeriod));
    const auto* rec = view.correlation_for(idx, 0U);
    REQUIRE(rec != nullptr);
    CHECK((rec->flags & crd::perf::kCorrelationCalibrated) != 0U);
    CHECK(rec->clock_uncertainty_ns != crd::perf::kUnknownClockUncertainty);

    crd::perf::shutdown();
}

TEST_CASE("gpu-resolve: a 64-bit end<begin is not a wrap -- dropped and counted, never a negative span",
          "[perf][diag][gpu-resolve]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-resolve-invalid"};
    crd::perf::init({});
    const crd::perf::NameId pass = crd::perf::intern_name("bad");

    // Default valid_bits == 64: a 64-bit counter cannot wrap in a frame, so end < begin is genuinely invalid.
    crd::perf::emit_gpu_span_on(crd::perf::GpuTrackKey{0U, 0U}, crd::perf::GpuSampleKind::Execution, pass, 1000U, 500U,
                                kPeriod);
    CHECK(crd::perf::gpu_invalid_span_count() == 1U);   // TEETH: don't count -> flips
    CHECK(crd::perf::gpu_wrap_repaired_count() == 0U);

    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    CHECK(track_sample_count(view, "gpu d0 q0") == 0U); // no reversed sample emitted

    crd::perf::shutdown();
}

TEST_CASE("gpu-resolve: unavailable, slot-reuse and device-loss are counted by the backend, nothing fabricated",
          "[perf][diag][gpu-resolve]")
{
    crd::perf::init({});

    SECTION("timestamps unavailable")
    {
        crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-resolve-unavail"};
        FakeRobustBackend be;
        crd::perf::set_gpu_backend(&be);
        const crd::perf::NameId pass = crd::perf::intern_name("u");
        be.record(0U, pass, 100U, 200U);
        be.record(0U, pass, 100U, 200U);
        be.set_unavailable(0U, 2U); // the two spans' query results are not readable
        crd::perf::frame_mark();
        crd::perf::frame_mark(); // frame 0 out of flight -> resolved as unavailable
        CHECK(crd::perf::gpu_timestamps_unavailable_count() == 2U); // TEETH: don't count -> flips
        const auto buf = crd::perf::save_capture_to_buffer(&cap);
        const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
        REQUIRE(view.is_valid());
        CHECK(track_sample_count(view, "gpu d0 q0") == 0U); // nothing fabricated
        crd::perf::set_gpu_backend(nullptr);
    }

    SECTION("query slot reused")
    {
        crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-resolve-reuse"};
        FakeRobustBackend be;
        crd::perf::set_gpu_backend(&be);
        const crd::perf::NameId pass = crd::perf::intern_name("r");
        // Record kSpans + 2 into one frame before it resolves -> 2 lapped.
        for (crd::u32 i = 0U; i < FakeRobustBackend::kSpans + 2U; ++i)
        {
            be.record(0U, pass, 100U + i, 150U + i);
        }
        crd::perf::frame_mark();
        crd::perf::frame_mark();
        CHECK(crd::perf::gpu_query_slot_reused_count() == 2U); // TEETH: don't count -> flips
        const auto buf = crd::perf::save_capture_to_buffer(&cap);
        const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
        REQUIRE(view.is_valid());
        CHECK(track_sample_count(view, "gpu d0 q0") == FakeRobustBackend::kSpans); // dropped, not doubled
        crd::perf::set_gpu_backend(nullptr);
    }

    SECTION("device loss drops pending frames, counted, no crash")
    {
        crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-resolve-loss"};
        FakeRobustBackend be;
        crd::perf::set_gpu_backend(&be);
        const crd::perf::NameId pass = crd::perf::intern_name("l");
        be.record(0U, pass, 100U, 200U);
        crd::perf::frame_mark(); // frame 0 submitted (still in flight; not yet resolved)
        be.record(1U, pass, 100U, 200U);
        be.lose_device();        // lose BEFORE the next resolve, while both frames are pending
        crd::perf::frame_mark(); // resolve sees loss -> drop both pending, count once
        CHECK(crd::perf::gpu_device_lost_count() == 1U);             // TEETH: don't count -> flips
        CHECK(crd::perf::gpu_pending_frames_dropped_count() == 2U);  // frames 0 and 1 were pending at loss
        crd::perf::frame_mark(); // a further mark is a silent no-op (no crash, no re-count)
        CHECK(crd::perf::gpu_device_lost_count() == 1U);
        const auto buf = crd::perf::save_capture_to_buffer(&cap);
        const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
        REQUIRE(view.is_valid());
        CHECK(track_sample_count(view, "gpu d0 q0") == 0U); // dropped frames emitted nothing
        crd::perf::set_gpu_backend(nullptr);
    }

    crd::perf::shutdown();
}

TEST_CASE("gpu-resolve: a frame held past its normal resolve still lands exactly once", "[perf][diag][gpu-resolve]")
{
    crd::memory::GrowableTlsfAllocator cap{64ULL << 20, nullptr, "gpu-resolve-delay"};
    crd::perf::init({});
    FakeRobustBackend be;
    crd::perf::set_gpu_backend(&be);
    const crd::perf::NameId pass = crd::perf::intern_name("d");
    be.record(0U, pass, 100U, 200U);
    be.record(0U, pass, 300U, 400U);
    be.hold_frame(0U, 3U); // resolve 3 marks later than usual

    for (crd::u32 k = 0U; k < 6U; ++k)
    {
        crd::perf::frame_mark();
    }
    const auto buf = crd::perf::save_capture_to_buffer(&cap);
    const crd::perf::CaptureView view{crd::containers::ConstSpan<crd::u8>{buf.data(), buf.size()}};
    REQUIRE(view.is_valid());
    // Exactly the two frame-0 spans -- present (not zero) and not doubled by the extra marks.
    CHECK(track_sample_count(view, "gpu d0 q0") == 2U);

    crd::perf::set_gpu_backend(nullptr);
    crd::perf::shutdown();
}

#endif // CRD_PERF_ENABLED
