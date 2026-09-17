// ---------------------------------------------------------------------------
// crd-perf -- GPU backend registration + sample emission (Detour D-003 v0d).
//
// Single-subscriber backend pointer (atomic acquire/release). The backend
// is supplied by a gpu-context backend (DIAG.6b(i); today only the test mock) -- crd-perf itself
// has no Vulkan dependency.
//
// emit_gpu_sample_on() is the bridge from "I resolved a query pool slot" to
// "now a Sample lives on the track for this (device,queue)." DIAG.6b(c): GPU work is modelled on separate
// per-(device,queue) tracks, each a dedicated external ring slot (register_external_track, NOT the caller's own thread)
// named "gpu d<dev> q<queue>" and correlation-ready. A GpuSampleKind routes the Sample onto Category Gpu (execution),
// Io (transfer) or Wait (queue-wait). Tracks register lazily on first emit; the table is bounded by kMaxGpuTracks and
// over-cap keys fold onto a shared "gpu overflow" track (counted). emit_gpu_sample() is the single-track shim onto the
// default execution track (device 0, queue 0).
// ---------------------------------------------------------------------------

#include <crd/perf/gpu_scope.hpp>

#include <crd/core/assert.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>

#include <atomic>
#include <cstdio>

namespace crd::perf
{

#if CRD_PERF_ENABLED

namespace
{

std::atomic<IProfilerGpuBackend*> g_gpu_backend{nullptr};

// DIAG.6b(c): the per-(device,queue) GPU track table. Written only on the cold registration path (guarded by a spinlock
// against a second emitter); the sample-emit fast path scans lock-free up to g_track_count (published release). Bounded
// by kMaxGpuTracks; further keys fold onto g_overflow_track and bump g_overflow_count.
struct GpuTrack
{
    crd::u32 device_id;
    crd::u32 queue_id;
    crd::u8  thread_index;
};

GpuTrack              g_tracks[kMaxGpuTracks]{};
std::atomic<crd::u32> g_track_count{0U};
std::atomic<crd::u32> g_overflow_count{0U};
std::atomic<crd::u8>  g_overflow_track{0xFFU};

// DIAG.6b(d): per-device CPU<->GPU calibration store. `cur` is the latest sample; `prev` (when has_prev) is the one
// before it, so emit can bound a span by the real drift between the two. Written by set_gpu_clock_calibration and read
// by emit_gpu_span_on, both under g_track_lock (cold path) -- emit reads it AFTER ensure_gpu_track has released the lock
// (never nested; atomic_flag is not recursive).
struct DeviceCalib
{
    crd::time::GpuClockCalibration cur;
    crd::time::GpuClockCalibration prev;
    bool                           has_cur    = false;
    bool                           has_prev   = false;
    crd::u32                       valid_bits = 64U; // DIAG.6b(f): timestamp counter width (default full 64)
};

DeviceCalib           g_calib[kMaxGpuDevices]{};
std::atomic<crd::u32> g_uncalibrated_count{0U};

// DIAG.6b(f): resolve-robustness counters (reset by reset_gpu_state).
std::atomic<crd::u32> g_wrap_repaired_count{0U};
std::atomic<crd::u32> g_invalid_span_count{0U};
std::atomic<crd::u32> g_timestamps_unavailable_count{0U};
std::atomic<crd::u32> g_query_slot_reused_count{0U};
std::atomic<crd::u32> g_device_lost_count{0U};
std::atomic<crd::u32> g_pending_frames_dropped_count{0U};
std::atomic_flag      g_track_lock = ATOMIC_FLAG_INIT;

// Register a fresh external track named "gpu d<dev> q<queue>", correlation-ready. External (not the caller's own track):
// resolved GPU samples reach it via write_external_sample. Returns 0xFF if the profiler is down or the table is full.
[[nodiscard]] crd::u8 register_gpu_track(crd::u32 device_id, crd::u32 queue_id) noexcept
{
    char name[32];
    // Worst case "gpu d4294967295 q4294967295" == 27 chars; the 32-byte buffer and the 48-byte name arena both fit it.
    std::snprintf(name, sizeof(name), "gpu d%u q%u", device_id, queue_id);
    const crd::u8 idx = register_external_track(name);
    if (idx != 0xFFU)
    {
        // DIAG.6b(b): make the track correlation-ready once, so resolved samples can carry queue/device/clock identity.
        enable_thread_correlation(idx);
    }
    return idx;
}

// Fold an over-cap key onto the shared overflow track (registered lazily), counting it. Assumes g_track_lock held.
[[nodiscard]] crd::u8 fold_to_overflow_locked() noexcept
{
    crd::u8 ov = g_overflow_track.load(std::memory_order_relaxed);
    if (ov == 0xFFU)
    {
        ov = register_external_track("gpu overflow");
        if (ov != 0xFFU)
        {
            enable_thread_correlation(ov);
        }
        g_overflow_track.store(ov, std::memory_order_relaxed);
    }
    g_overflow_count.fetch_add(1U, std::memory_order_relaxed);
    return ov; // 0xFF if even the overflow track could not be allocated -> caller drops the sample
}

// Resolve the track index for (device,queue), registering it on first use. 0xFF -> profiler down / slots exhausted.
[[nodiscard]] crd::u8 ensure_gpu_track(crd::u32 device_id, crd::u32 queue_id) noexcept
{
    // Fast path: scan the published entries lock-free.
    const crd::u32 count = g_track_count.load(std::memory_order_acquire);
    for (crd::u32 i = 0U; i < count; ++i)
    {
        if (g_tracks[i].device_id == device_id && g_tracks[i].queue_id == queue_id)
        {
            return g_tracks[i].thread_index;
        }
    }

    // Cold path: register under the spinlock. Single backend today, but a second emitter must not corrupt the table.
    while (g_track_lock.test_and_set(std::memory_order_acquire))
    {
        // spin -- registration is a rare cold path
    }
    crd::u8        result = 0xFFU;
    const crd::u32 c2     = g_track_count.load(std::memory_order_relaxed);
    // Re-scan under the lock: another emitter may have inserted our key while we waited.
    bool found = false;
    for (crd::u32 i = 0U; i < c2; ++i)
    {
        if (g_tracks[i].device_id == device_id && g_tracks[i].queue_id == queue_id)
        {
            result = g_tracks[i].thread_index;
            found  = true;
            break;
        }
    }
    if (!found)
    {
        if (c2 < kMaxGpuTracks)
        {
            const crd::u8 idx = register_gpu_track(device_id, queue_id);
            if (idx == 0xFFU)
            {
                result = fold_to_overflow_locked(); // profiler thread table exhausted -> fold + count
            }
            else
            {
                g_tracks[c2].device_id    = device_id;
                g_tracks[c2].queue_id     = queue_id;
                g_tracks[c2].thread_index = idx;
                g_track_count.store(c2 + 1U, std::memory_order_release); // publish the entry
                result = idx;
            }
        }
        else
        {
            result = fold_to_overflow_locked(); // kMaxGpuTracks reached -> fold + count
        }
    }
    g_track_lock.clear(std::memory_order_release);
    return result;
}

[[nodiscard]] crd::u8 category_for(GpuSampleKind kind) noexcept
{
    switch (kind)
    {
    case GpuSampleKind::Transfer:
        return static_cast<crd::u8>(Category::Io);
    case GpuSampleKind::QueueWait:
        return static_cast<crd::u8>(Category::Wait);
    case GpuSampleKind::Execution:
    default:
        return static_cast<crd::u8>(Category::Gpu);
    }
}

} // namespace

void set_gpu_backend(IProfilerGpuBackend* backend) noexcept
{
    g_gpu_backend.store(backend, std::memory_order_release);
    if (backend != nullptr)
    {
        // DIAG.6b(c): pre-register the default execution track (device 0, queue 0) so the resolve loop and
        // gpu_thread_index() have a slot without taking the lazy-register path on the emit path. Other (device,queue)
        // keys register lazily on their first emit_gpu_sample_on().
        (void)ensure_gpu_track(0U, 0U);
        // DIAG.6b(e): open the first frame on install so spans recorded before the first frame_mark() have an open
        // frame; frame_mark() then closes each frame and opens the next.
        backend->begin_frame(frame_count());
    }
    else
    {
        // Clearing the backend leaves the registered tracks in place -- resolved Samples that arrived earlier remain
        // queryable in their rings. A reinstalled backend reuses the same (device,queue) slots. DIAG.6b(f) decision:
        // we deliberately do NOT call end_frame() on the outgoing backend -- a lost device may reject the call, and the
        // backend's own destructor reclaims its query pool. The abandoned open frame is harmless (no more spans arrive).
    }
}

[[nodiscard]] IProfilerGpuBackend* current_gpu_backend() noexcept
{
    return g_gpu_backend.load(std::memory_order_acquire);
}

void resolve_gpu_frames() noexcept
{
    IProfilerGpuBackend* be = current_gpu_backend();
    if (be == nullptr)
    {
        return;
    }
    be->resolve_completed_frames();
}

[[nodiscard]] crd::u8 gpu_thread_index() noexcept
{
    // DIAG.6b(c): the default execution track (device 0, queue 0) -- the slot set_gpu_backend pre-registers. 0xFF until
    // a backend is installed. Multi-queue consumers should enumerate tracks via the capture's thread list instead.
    const crd::u32 count = g_track_count.load(std::memory_order_acquire);
    for (crd::u32 i = 0U; i < count; ++i)
    {
        if (g_tracks[i].device_id == 0U && g_tracks[i].queue_id == 0U)
        {
            return g_tracks[i].thread_index;
        }
    }
    return 0xFFU;
}

[[nodiscard]] crd::u32 gpu_track_overflow_count() noexcept
{
    return g_overflow_count.load(std::memory_order_acquire);
}

// ---- emit_gpu_sample_on / emit_gpu_sample ------------------------------
//
// Backend-side helper: build a fully-formed Sample and stamp it into the resolved (device,queue) track's ring. The
// caller (a backend's resolve_completed_frames, e.g. the test MockGpuBackend) is responsible for converting GPU tick
// counts to ns and assigning begin_ns / end_ns on the monotonic-clock scale.
//
// Implementation note: the existing push_region / pop_region path is thread-local; it always writes to the *current*
// thread's ring. The GPU resolve runs on the CPU thread that called resolve_gpu_frames() (the main thread, typically),
// so its t_ring points to the main thread, not a GPU track. We bypass that and write directly into the target track's
// ring using a dedicated helper that knows the target thread index.
//
// This is the only legitimate "external sample write" path in crd-perf. User code that wants to push a sample into a
// non-current-thread ring should use jobs / scope / counter macros instead.

namespace detail
{
// Forward declaration of the SPSC ring's external-write helper, defined in profiler.cpp where the ring's storage is
// private. DIAG.6b(b): the optional trailing correlation record defaults to none. DIAG.6b(c): emit_gpu_sample_on passes
// a record carrying the (device,queue) identity + pass name; clock_domain / clock_uncertainty_ns are filled by (d).
void write_external_sample(crd::u8 thread_index, const Sample& s,
                           const CorrelationRecord* corr = nullptr) noexcept;
} // namespace detail

void emit_gpu_sample_on(Sample sample, GpuTrackKey key, GpuSampleKind kind) noexcept
{
    const crd::u8 idx = ensure_gpu_track(key.device_id, key.queue_id);
    if (idx == 0xFFU)
    {
        return; // Profiler not initialised, or all track slots exhausted -> drop (counted for the overflow case).
    }
    // Force the category + thread fields so a backend cannot mis-route a Sample onto the wrong track/category.
    sample.category     = category_for(kind);
    sample.begin_thread = idx;
    sample.end_thread   = idx;

    // DIAG.6b(c): attach the (device,queue) identity + the pass name. DIAG.6b(d): this ns-native path did NOT calibrate
    // (the backend supplied begin_ns/end_ns), so it is honest about that -- flag clear, uncertainty = the sentinel
    // (never a misleading 0). A backend that has ticks + a calibration should call emit_gpu_span_on instead.
    CorrelationRecord corr{};
    corr.flags                = kCorrelationValid;
    corr.queue_id             = key.queue_id;
    corr.device_id            = key.device_id;
    corr.clock_domain         = kClockDomainCpu; // begin_ns/end_ns are on the CPU timeline (backend's word)
    corr.clock_uncertainty_ns = kUnknownClockUncertainty;
    corr.pass_id              = sample.name_id;
    corr.resource_id          = kNoCorrelationName;
    detail::write_external_sample(idx, sample, &corr);
}

void emit_gpu_sample(Sample sample) noexcept
{
    // DIAG.6b(c): single-track convenience -> default execution track (device 0, queue 0).
    emit_gpu_sample_on(sample, GpuTrackKey{0U, 0U}, GpuSampleKind::Execution);
}

void set_gpu_clock_calibration(crd::u32 device_id, const crd::time::GpuClockCalibration& calibration) noexcept
{
    if (device_id >= kMaxGpuDevices)
    {
        return; // out of range -> this device stays uncalibrated (emit_gpu_span_on records + counts it)
    }
    // Cold path: serialize writers and exclude the emit reader (which reads under the same lock).
    while (g_track_lock.test_and_set(std::memory_order_acquire))
    {
        // spin
    }
    DeviceCalib& d = g_calib[device_id];
    if (d.has_cur)
    {
        d.prev     = d.cur; // shift the latest down so a span can be bounded by the drift between the two
        d.has_prev = true;
    }
    d.cur     = calibration;
    d.has_cur = true;
    g_track_lock.clear(std::memory_order_release);
}

[[nodiscard]] crd::u32 uncalibrated_span_count() noexcept
{
    return g_uncalibrated_count.load(std::memory_order_acquire);
}

void set_gpu_timestamp_valid_bits(crd::u32 device_id, crd::u32 valid_bits) noexcept
{
    if (device_id >= kMaxGpuDevices)
    {
        return;
    }
    const crd::u32 bits = (valid_bits == 0U || valid_bits > 64U) ? 64U : valid_bits; // clamp to [1,64]; 0/oversize = full
    while (g_track_lock.test_and_set(std::memory_order_acquire))
    {
        // spin
    }
    g_calib[device_id].valid_bits = bits;
    g_track_lock.clear(std::memory_order_release);
}

[[nodiscard]] crd::u32 gpu_wrap_repaired_count() noexcept
{
    return g_wrap_repaired_count.load(std::memory_order_acquire);
}
[[nodiscard]] crd::u32 gpu_invalid_span_count() noexcept
{
    return g_invalid_span_count.load(std::memory_order_acquire);
}
[[nodiscard]] crd::u32 gpu_timestamps_unavailable_count() noexcept
{
    return g_timestamps_unavailable_count.load(std::memory_order_acquire);
}
[[nodiscard]] crd::u32 gpu_query_slot_reused_count() noexcept
{
    return g_query_slot_reused_count.load(std::memory_order_acquire);
}
[[nodiscard]] crd::u32 gpu_device_lost_count() noexcept
{
    return g_device_lost_count.load(std::memory_order_acquire);
}
[[nodiscard]] crd::u32 gpu_pending_frames_dropped_count() noexcept
{
    return g_pending_frames_dropped_count.load(std::memory_order_acquire);
}

void note_gpu_timestamps_unavailable(crd::u32 n) noexcept
{
    g_timestamps_unavailable_count.fetch_add(n, std::memory_order_relaxed);
}
void note_gpu_query_slot_reused(crd::u32 n) noexcept
{
    g_query_slot_reused_count.fetch_add(n, std::memory_order_relaxed);
}
void note_gpu_device_lost(crd::u32 /*device_id*/, crd::u32 pending_frames_dropped) noexcept
{
    g_device_lost_count.fetch_add(1U, std::memory_order_relaxed);
    g_pending_frames_dropped_count.fetch_add(pending_frames_dropped, std::memory_order_relaxed);
}

void emit_gpu_span_on(GpuTrackKey key, GpuSampleKind kind, NameId name, crd::u64 begin_ticks, crd::u64 end_ticks,
                      crd::f64 ns_per_tick) noexcept
{
    const crd::u8 idx = ensure_gpu_track(key.device_id, key.queue_id); // locks internally; returns unlocked
    if (idx == 0xFFU)
    {
        return; // profiler down or slots exhausted
    }

    // Snapshot this device's calibration under the lock (cold path; ensure_gpu_track has already released it, so this is
    // not a nested acquire). set_gpu_clock_calibration writes the (prev, cur) pair under this same lock, so the snapshot
    // is never torn -- mutual exclusion, not tolerance. (Re-init safety is the profiler's hot-path contract: producers
    // quiesce before shutdown, same as the existing external-sample write below.)
    crd::time::GpuClockCalibration cur{};
    crd::time::GpuClockCalibration prev{};
    bool                           has_cur    = false;
    bool                           has_prev   = false;
    crd::u32                       valid_bits = 64U;
    if (key.device_id < kMaxGpuDevices)
    {
        while (g_track_lock.test_and_set(std::memory_order_acquire))
        {
            // spin
        }
        const DeviceCalib& d = g_calib[key.device_id];
        has_cur              = d.has_cur;
        cur                  = d.cur;
        has_prev             = d.has_prev;
        prev                 = d.prev;
        valid_bits           = d.valid_bits;
        g_track_lock.clear(std::memory_order_release);
    }

    // DIAG.6b(f): resolve the span duration in the device's (possibly narrow) tick window. gpu_tick_delta masks + sign-
    // extends, so a single wrap (`end` numerically < `begin` inside the window) becomes a small positive duration. A
    // negative modular delta is not a repairable wrap -- reject it and count it rather than emit a reversed span. Note a
    // window limit: with B < 64 a span >= half the window (e.g. ~4.3 s at 2 ns/tick for B=32) is indistinguishable from a
    // reversed span and is rejected too; (i) documents each backend's real timestampValidBits so this stays theoretical.
    const crd::u64 mask     = valid_bits >= 64U ? ~static_cast<crd::u64>(0)
                                                : ((static_cast<crd::u64>(1) << valid_bits) - static_cast<crd::u64>(1));
    const crd::u64 b_masked = begin_ticks & mask;
    const crd::u64 e_masked = end_ticks & mask;
    const crd::i64 dur_ticks = crd::time::gpu_tick_delta(e_masked, b_masked, valid_bits);
    if (dur_ticks < 0)
    {
        g_invalid_span_count.fetch_add(1U, std::memory_order_relaxed);
        return; // end is genuinely before begin (a 64-bit counter cannot wrap in a frame) -> drop, never a negative span
    }
    if (e_masked < b_masked)
    {
        // The masked end wrapped past the window boundary but the modular delta is forward -> a single wrap, repaired.
        g_wrap_repaired_count.fetch_add(1U, std::memory_order_relaxed);
    }

    Sample sample{};
    sample.name_id      = name.value;
    sample.category     = category_for(kind);
    sample.begin_thread = idx;
    sample.end_thread   = idx;

    CorrelationRecord corr{};
    corr.flags       = kCorrelationValid;
    corr.queue_id    = key.queue_id;
    corr.device_id   = key.device_id;
    corr.pass_id     = name.value;
    corr.resource_id = kNoCorrelationName;

    if (has_cur)
    {
        // Calibrated: place begin on the CPU timeline (modular, so a wrapped begin lands right) and derive end from the
        // repaired duration -- never re-mask a computed end. The calibration owns the authoritative period; the
        // ns_per_tick argument (the uncalibrated fallback's period) is unused here.
        sample.begin_ns = crd::time::gpu_ticks_to_cpu_ns(cur, b_masked, valid_bits);
        sample.end_ns   = sample.begin_ns + crd::time::gpu_round_ns(static_cast<crd::f64>(dur_ticks) * cur.ns_per_tick);
        corr.flags |= kCorrelationCalibrated;
        corr.clock_domain         = kClockDomainCpu;
        corr.clock_uncertainty_ns =
            crd::time::gpu_span_uncertainty_ns(has_prev ? &prev : nullptr, cur, b_masked, valid_bits);
    }
    else
    {
        // Uncalibrated: retain a separate track in the device's raw GPU-tick domain (no offset, no cross-clock
        // subtraction). Flag stays clear, uncertainty is the sentinel (NOT 0), and the span is counted. End is begin +
        // the repaired duration.
        sample.begin_ns           = crd::time::gpu_round_ns(static_cast<crd::f64>(b_masked) * ns_per_tick);
        sample.end_ns             = sample.begin_ns + crd::time::gpu_round_ns(static_cast<crd::f64>(dur_ticks) * ns_per_tick);
        corr.clock_domain         = kClockDomainGpuBase + key.device_id;
        corr.clock_uncertainty_ns = kUnknownClockUncertainty;
        g_uncalibrated_count.fetch_add(1U, std::memory_order_relaxed);
    }
    detail::write_external_sample(idx, sample, &corr);
}

namespace detail
{

// Called by profiler.cpp::shutdown(). The profiler is tearing down the
// thread table, so any cached "gpu" thread index becomes stale. Clear it
// alongside the backend pointer so the next init() starts fresh.
void reset_gpu_state() noexcept
{
    // DIAG.6b(c): the profiler is tearing down the thread table, so every cached track index becomes stale. Clear the
    // whole table (under the lock, against a concurrent registration) so the next init() starts fresh.
    while (g_track_lock.test_and_set(std::memory_order_acquire))
    {
        // spin
    }
    g_gpu_backend.store(nullptr, std::memory_order_release);
    g_track_count.store(0U, std::memory_order_release);
    g_overflow_count.store(0U, std::memory_order_release);
    g_overflow_track.store(0xFFU, std::memory_order_release);
    // DIAG.6b(d): drop the per-device calibrations and the uncalibrated counter too.
    for (crd::u32 i = 0U; i < kMaxGpuDevices; ++i)
    {
        g_calib[i] = DeviceCalib{};
    }
    g_uncalibrated_count.store(0U, std::memory_order_release);
    // DIAG.6b(f): drop the robustness counters too.
    g_wrap_repaired_count.store(0U, std::memory_order_release);
    g_invalid_span_count.store(0U, std::memory_order_release);
    g_timestamps_unavailable_count.store(0U, std::memory_order_release);
    g_query_slot_reused_count.store(0U, std::memory_order_release);
    g_device_lost_count.store(0U, std::memory_order_release);
    g_pending_frames_dropped_count.store(0U, std::memory_order_release);
    g_track_lock.clear(std::memory_order_release);
}

// DIAG.6b(e): drive the GPU frame lifecycle from frame_mark() -- close the frame that just ended, resolve completed
// frames (emitting their samples), then open the next frame. No-op when no backend is installed (one acquire load + a
// not-taken branch, the only cost frame_mark pays without a GPU backend). Called under frame_mark's StateGuard, so
// shutdown's g_in_flight drain waits for it -- the backend can't be torn down mid-advance.
void gpu_frame_advance(crd::u64 next_frame_index) noexcept
{
    IProfilerGpuBackend* be = g_gpu_backend.load(std::memory_order_acquire);
    if (be == nullptr)
    {
        return;
    }
    be->end_frame();
    be->resolve_completed_frames();
    be->begin_frame(next_frame_index);
}

} // namespace detail

#else // CRD_PERF_ENABLED == 0

void set_gpu_backend(IProfilerGpuBackend*) noexcept {}
[[nodiscard]] IProfilerGpuBackend* current_gpu_backend() noexcept { return nullptr; }
void resolve_gpu_frames() noexcept {}
[[nodiscard]] crd::u8 gpu_thread_index() noexcept { return 0xFFU; }
void emit_gpu_sample(Sample) noexcept {}
void emit_gpu_sample_on(Sample, GpuTrackKey, GpuSampleKind) noexcept {}
[[nodiscard]] crd::u32 gpu_track_overflow_count() noexcept { return 0U; }
void set_gpu_clock_calibration(crd::u32, const crd::time::GpuClockCalibration&) noexcept {}
void emit_gpu_span_on(GpuTrackKey, GpuSampleKind, NameId, crd::u64, crd::u64, crd::f64) noexcept {}
[[nodiscard]] crd::u32 uncalibrated_span_count() noexcept { return 0U; }
void set_gpu_timestamp_valid_bits(crd::u32, crd::u32) noexcept {}
[[nodiscard]] crd::u32 gpu_wrap_repaired_count() noexcept { return 0U; }
[[nodiscard]] crd::u32 gpu_invalid_span_count() noexcept { return 0U; }
[[nodiscard]] crd::u32 gpu_timestamps_unavailable_count() noexcept { return 0U; }
[[nodiscard]] crd::u32 gpu_query_slot_reused_count() noexcept { return 0U; }
[[nodiscard]] crd::u32 gpu_device_lost_count() noexcept { return 0U; }
[[nodiscard]] crd::u32 gpu_pending_frames_dropped_count() noexcept { return 0U; }
void note_gpu_timestamps_unavailable(crd::u32) noexcept {}
void note_gpu_query_slot_reused(crd::u32) noexcept {}
void note_gpu_device_lost(crd::u32, crd::u32) noexcept {}

#endif

} // namespace crd::perf
