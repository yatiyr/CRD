// DIAG.6b(i): FrameGraphGpuBackend — see the header for the design. The frame graph times itself; this adapter's
// only real work is in resolve_completed_frames(), which walks the last execute's passes and emits a positioned GPU
// span for each pass whose raw ticks the graph exposes, counting the rest unavailable (never fabricating a begin).

#include <crd/perf/gpu/frame_graph_gpu_backend.hpp>

#include <crd/gpu/frame_graph.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>
#include <crd/time/gpu_timestamp.hpp> // DIAG.6b(j): gpu_tick_delta / gpu_round_ns for the comparison total

#include <bit> // DIAG.6b(j): bit_cast the reference total into the dedupe fingerprint

#if CRD_PERF_ENABLED

namespace crd::perf::gpu
{
namespace
{
// The backend-neutral queue-id contract (DIAG.6b(e)): a frame-graph queue maps onto the agreed perf queue ids so
// async work lands on its own track and two backends agree on what "queue 1" means.
[[nodiscard]] crd::u32 map_queue(crd::gpu::FgQueue q) noexcept
{
    if (q == crd::gpu::FgQueue::Async)
    {
        return crd::perf::kGpuQueueAsyncCompute; // TEETH-A: force kGpuQueueGraphics -> async work collapses onto q0
    }
    return crd::perf::kGpuQueueGraphics;
}

// A transfer pass MOVES pixels (its work is the row's "I/O"); every other kind is GPU execution.
[[nodiscard]] crd::perf::GpuSampleKind map_kind(crd::gpu::FgPassKind k) noexcept
{
    if (k == crd::gpu::FgPassKind::Transfer)
    {
        return crd::perf::GpuSampleKind::Transfer;
    }
    return crd::perf::GpuSampleKind::Execution;
}

void fnv_mix(crd::u64& h, crd::u64 v) noexcept
{
    h ^= v;
    h *= 1099511628211ULL; // FNV-1a prime
}

// DIAG.6b(j): the placed total's reduction is the SPAN (first begin -> last end, gaps INCLUDED -- gpu_ms_total's own
// definition). `sum_ticks` (durations only, gaps EXCLUDED) is the WRONG reduction; both arguments are named so the
// TEETH-C tooth (return sum_ticks) is a warning-clean one-line flip under /W4 /WX in either direction.
[[nodiscard]] crd::i64 pick_total(crd::i64 span_ticks, crd::i64 sum_ticks) noexcept
{
    (void)span_ticks;
    (void)sum_ticks;
    return span_ticks; // TEETH-C: return sum_ticks (gaps excluded) -> the gap test flips
}
} // namespace

FrameGraphGpuBackend::FrameGraphGpuBackend(crd::gpu::IFrameGraph& fg, crd::u32 device_id) noexcept
    : m_fg(fg), m_device(device_id)
{
}

void FrameGraphGpuBackend::begin_frame(crd::u64 frame_index) noexcept
{
    (void)frame_index; // the frame graph carries its own timings; the bridge needs no per-frame index
}

crd::perf::GpuSpanHandle FrameGraphGpuBackend::begin_span(void* cmd_buffer, crd::perf::NameId name) noexcept
{
    // The frame graph inserts its OWN timestamp queries around each pass; the cmd-buffer span API is not how its
    // timing is captured, so this is an inert no-op rather than a second, conflicting query.
    (void)cmd_buffer;
    (void)name;
    return crd::perf::kInvalidGpuSpan;
}

void FrameGraphGpuBackend::end_span(void* cmd_buffer, crd::perf::GpuSpanHandle span) noexcept
{
    (void)cmd_buffer;
    (void)span;
}

void FrameGraphGpuBackend::end_frame() noexcept {}

crd::f64 FrameGraphGpuBackend::ns_per_tick() const noexcept
{
    return m_fg.gpu_timestamp_period_ns();
}

crd::u64 FrameGraphGpuBackend::fingerprint() const noexcept
{
    crd::u64       h = 1469598103934665603ULL; // FNV-1a offset basis
    const crd::u32 n = m_fg.pass_count();
    fnv_mix(h, n);
    for (crd::u32 i = 0U; i < n; ++i)
    {
        crd::u64   begin_ticks = 0ULL;
        crd::u64   end_ticks   = 0ULL;
        const bool ok          = m_fg.pass_gpu_ticks(i, begin_ticks, end_ticks);
        fnv_mix(h, ok ? 1ULL : 0ULL);
        fnv_mix(h, begin_ticks);
        fnv_mix(h, end_ticks);
        fnv_mix(h, static_cast<crd::u64>(m_fg.pass_kind(i)));
        fnv_mix(h, static_cast<crd::u64>(m_fg.pass_queue(i)));
    }
    // Fold the reference total in too: on a real (i2-pending) device every pass is unavailable, so the per-pass bits
    // above are identical frame to frame -- without this, a genuine new execute would dedupe and the (j) comparison
    // (incomparable, today) would be counted once per PROCESS, not once per frame. gpu_ms_total changes per frame.
    fnv_mix(h, std::bit_cast<crd::u64>(m_fg.gpu_ms_total()));
    return h;
}

void FrameGraphGpuBackend::resolve_completed_frames() noexcept
{
    ++m_resolve_calls;

    // gpu_timing_available() is the device-level gate: false means the device has no timestamp support at all, so
    // there is nothing to place AND nothing to count (per-pass "unavailable" is a query-read failure on a
    // timing-capable device, a different signal). Reuse of REN-8's existing flag — no parallel gate.
    if (!m_fg.gpu_timing_available())
    {
        return;
    }

    // Tell crd-perf the device's timestamp width so emit_gpu_span_on can repair a single mid-frame wrap of a narrow
    // counter into a forward span instead of dropping it (DIAG.6b(f)). Set every resolve (idempotent, cold path): a
    // guarded once-only set would not survive a shutdown->init that reset the device back to the 64-bit default.
    crd::perf::set_gpu_timestamp_valid_bits(m_device, m_fg.gpu_timestamp_valid_bits());

    // The frame graph exposes no execute counter, so a second frame_mark() with no intervening execute() would
    // re-present identical timings. Dedupe on a content fingerprint so the spans are emitted exactly once per frame.
    const crd::u64 fp = fingerprint();
    if (m_have_last && fp == m_last_fingerprint)
    {
        return;
    }
    m_last_fingerprint = fp;
    m_have_last        = true;

    const crd::f64 period     = m_fg.gpu_timestamp_period_ns();
    const crd::u32 valid_bits = m_fg.gpu_timestamp_valid_bits();
    const crd::u32 n          = m_fg.pass_count();

    // An empty frame (no passes) is not INCOMPARABLE -- there is nothing to place and nothing to compare, so it counts
    // nothing. Return before the emit + comparison so a headless app that never executes does not accrue verdicts.
    if (n == 0U)
    {
        return;
    }

    // While emitting, accumulate the placed frame's own SPAN (first begin -> last end, gaps INCLUDED -- the reference's
    // definition) for the (j) comparison below. Every endpoint is a delta from the first placed pass's begin, via the
    // same crd-time helper the emit path uses, so a narrow-counter wrap is handled identically. `sum_ticks` (durations
    // only, gaps EXCLUDED) is the WRONG reduction, accumulated solely so a tooth can prove the span reduction is right.
    bool     any_unavailable = false;
    crd::u32 placed          = 0U;
    crd::u64 base            = 0ULL;
    crd::i64 min_begin_delta = 0;
    crd::i64 max_end_delta   = 0;
    crd::i64 sum_ticks       = 0;
    for (crd::u32 i = 0U; i < n; ++i)
    {
        crd::u64 begin_ticks = 0ULL;
        crd::u64 end_ticks   = 0ULL;
        if (!m_fg.pass_gpu_ticks(i, begin_ticks, end_ticks))
        {
            any_unavailable = true;
            crd::perf::note_gpu_timestamps_unavailable(1U); // TEETH-B: a pass we cannot PLACE is counted, not invented
            continue;
        }
        const char* const            name = m_fg.pass_name(i);
        const crd::perf::NameId       id   = crd::perf::intern_name(name != nullptr ? name : "gpu pass");
        const crd::perf::GpuTrackKey  key{m_device, map_queue(m_fg.pass_queue(i))};
        // Uncalibrated: no CPU+GPU paired-sample source exists yet, so the span lands in the device's raw GPU-tick
        // domain (sentinel uncertainty, counted by uncalibrated_span_count()) — DIAG.6b(d)'s "retain tracks" path.
        crd::perf::emit_gpu_span_on(key, map_kind(m_fg.pass_kind(i)), id, begin_ticks, end_ticks, period);
        ++m_emitted_spans;

        if (placed == 0U)
        {
            base = begin_ticks;
        }
        const crd::i64 bd = crd::time::gpu_tick_delta(begin_ticks, base, valid_bits);
        const crd::i64 ed = crd::time::gpu_tick_delta(end_ticks, base, valid_bits);
        if (placed == 0U)
        {
            min_begin_delta = bd;
            max_end_delta   = ed;
        }
        else
        {
            min_begin_delta = bd < min_begin_delta ? bd : min_begin_delta;
            max_end_delta   = ed > max_end_delta ? ed : max_end_delta;
        }
        sum_ticks += crd::time::gpu_tick_delta(end_ticks, begin_ticks, valid_bits);
        ++placed;
    }
    // DIAG.6b(j): compare the placed GPU total against the frame graph's OWN independent reduction -- `gpu_ms_total()`
    // (REN-8's backend-side "first pass start -> last end"), produced by a different code path from crd-perf's
    // tick->ns conversion. INCOMPARABLE (counted separately, never compared) when the total is incomplete: a pass
    // unavailable, an unknown period, or no placed passes -- the honest verdict a real device gives today (all passes
    // unavailable, (i2) pending). The comparison is duration-vs-duration in ONE domain (never cpu_ns - gpu_ns), and a
    // mismatch reports only the MAGNITUDE of the discrepancy, never which pass -- the "do not infer exact GPU failure
    // location" clause.
    if (period == 0.0 || placed == 0U || any_unavailable)
    {
        ++m_total_incomparable_count;
    }
    else
    {
        const crd::i64 total_ticks     = pick_total(max_end_delta - min_begin_delta, sum_ticks);
        const crd::i64 placed_total_ns =
            crd::time::gpu_round_ns(static_cast<crd::f64>(total_ticks) * period); // TEETH-D: * (period * 2.0) flips (a)
        const crd::i64 reference_ns = crd::time::gpu_round_ns(m_fg.gpu_ms_total() * 1.0e6); // ms -> ns, same domain
        const crd::i64 discrepancy  = placed_total_ns - reference_ns;
        m_last_total_discrepancy_ns = discrepancy;
        // Two independent conversions of the SAME tick span disagree only by rounding: each side applies one
        // gpu_round_ns (+-0.5 ns) and the reference's ms<->ns round-trip adds at most sub-ns. A disagreement as large
        // as one whole tick (`period` ns) would mean the two sides counted a different span -- a real mismatch, not
        // noise. Bound = one tick period + 1 ns for the two half-roundings; derived, not a magic constant.
        const crd::i64 tolerance_ns = crd::time::gpu_round_ns(period) + 1;
        const crd::i64 magnitude    = discrepancy < 0 ? -discrepancy : discrepancy;
        if (magnitude <= tolerance_ns)
        {
            ++m_total_match_count;
        }
        else
        {
            ++m_total_mismatch_count;
        }
    }
}
} // namespace crd::perf::gpu

#else // CRD_PERF_ENABLED

namespace crd::perf::gpu
{
FrameGraphGpuBackend::FrameGraphGpuBackend(crd::gpu::IFrameGraph& fg, crd::u32 device_id) noexcept
    : m_fg(fg), m_device(device_id)
{
}
void FrameGraphGpuBackend::begin_frame(crd::u64) noexcept {}
crd::perf::GpuSpanHandle FrameGraphGpuBackend::begin_span(void*, crd::perf::NameId) noexcept
{
    return crd::perf::kInvalidGpuSpan;
}
void     FrameGraphGpuBackend::end_span(void*, crd::perf::GpuSpanHandle) noexcept {}
void     FrameGraphGpuBackend::end_frame() noexcept {}
void     FrameGraphGpuBackend::resolve_completed_frames() noexcept {}
crd::f64 FrameGraphGpuBackend::ns_per_tick() const noexcept { return 0.0; }
crd::u64 FrameGraphGpuBackend::fingerprint() const noexcept { return 0ULL; }
} // namespace crd::perf::gpu

#endif // CRD_PERF_ENABLED
