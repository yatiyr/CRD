#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- GPU timestamp scope (Detour D-003 v0d).
//
// Records pairs of GPU timestamps on a command buffer; once the GPU has
// retired the work and the host has resolved the query pool, the timing
// surfaces as a Sample on the profiler track for that (device,queue).
// DIAG.6b(c): GPU work is modelled on separate per-(device,queue) tracks (execution / transfer / queue-wait),
// not one aliased "gpu" track -- see emit_gpu_sample_on / GpuTrackKey / GpuSampleKind below.
//
// DIAG.6b(g): NO shipping backend exists yet. `IProfilerGpuBackend` is implemented today only by the test mock
// (`MockGpuBackend` in tests/foundation/perf/test_gpu_scope.cpp) -- the reference for the contract below. A production
// backend (a VkQueryPool wrapper with multi-frame-in-flight resolve for Vulkan; the D3D12 query-heap analogue) belongs
// in the current per-backend GPU-context module (`crd-gpu-context-vulkan` / `crd-gpu-context-dx12`) and is DIAG.6b(i);
// the old `crd-rhi` / `crd-rhi-vulkan` RHI was retired (ADR-0105) and no longer exists. The macro and the public
// surface here are backend-agnostic -- the cmd buffer is opaque (`void*`); the backend casts back to its concrete type.
//
// Usage (contract; the production factory is DIAG.6b(i), not yet provided):
//
//   // Once at startup, after the GPU device/context is created:
//   MyGpuBackend be{device};            // implements IProfilerGpuBackend (see MockGpuBackend for the shape)
//   crd::perf::set_gpu_backend(&be);
//
//   // Per-frame, around every render pass (cmd is the backend's concrete command-buffer type, passed as void*):
//   void Renderer::render_shadow_pass(void* cmd)
//   {
//       CRD_PERF_GPU_SCOPE(cmd, "shadow_pass");
//       // ... command recording ...
//   }
//
//   // Once per CPU frame:
//   crd::perf::frame_mark();  // DIAG.6b(e): drives end_frame() -> resolve_completed_frames() -> begin_frame(next)
//
// DIAG.6b(e): `frame_mark()` now drives the GPU frame lifecycle -- per CPU frame it calls the installed backend's
// `end_frame()` -> `resolve_completed_frames()` -> `begin_frame(next)` (and `set_gpu_backend` opens the first frame on
// install). `resolve_gpu_frames()` remains a manual resolve-only convenience (idempotent; a legacy call after a
// `frame_mark` is a harmless no-op). Today no production code installs a shipping backend, so the GPU track is inert
// outside tests (this is DG11); a real DX12/Vulkan producer hookup is DIAG.6b(i).
//
// When CRD_PERF_ENABLED == 0 every macro collapses to `((void)0)`; the
// backend is never queried.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>
#include <crd/time/gpu_timestamp.hpp>

namespace crd::perf
{

// Opaque per-span handle. The concrete backend defines its meaning
// (Vulkan: index into a VkQueryPool slot pair).
struct GpuSpanHandle
{
    crd::u32 value = 0xFFFF'FFFFU;
    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0xFFFF'FFFFU; }
};

inline constexpr GpuSpanHandle kInvalidGpuSpan{0xFFFF'FFFFU};

// Resolved span -- timestamp pair in GPU ticks plus the NameId the caller
// recorded. Backend converts to a Sample at resolve time.
struct ResolvedGpuSpan
{
    NameId   name;
    crd::u64 begin_ticks;
    crd::u64 end_ticks;
};

// Backend interface. Implemented today only by the test mock (`MockGpuBackend`); a production Vulkan/DX12 backend in a
// gpu-context module is DIAG.6b(i). One backend per process. `set_gpu_backend(nullptr)` clears.
class IProfilerGpuBackend
{
public:
    virtual ~IProfilerGpuBackend() = default;

    // Called once per profiler frame, before any begin_span for that frame.
    // Resets query slots for the new frame slot. The frame_index matches
    // crd::perf::frame_count().
    virtual void begin_frame(crd::u64 frame_index) noexcept = 0;

    // Record a "begin timestamp" on `cmd_buffer`. Returns a handle for the
    // matching end_span. `cmd_buffer` is the backend-typed command buffer
    // (cast inside the backend implementation).
    [[nodiscard]] virtual GpuSpanHandle begin_span(void* cmd_buffer, NameId name) noexcept = 0;

    // Record an "end timestamp" on `cmd_buffer` for the span returned by
    // begin_span.
    virtual void end_span(void* cmd_buffer, GpuSpanHandle span) noexcept = 0;

    // Bookkeeping after all spans for this frame are recorded. DIAG.6b(e): called by `frame_mark()` (before the
    // per-frame resolve) to mark the in-flight frame as "pending host readback."
    virtual void end_frame() noexcept = 0;

    // Poll completed frames; convert GPU-tick timestamps to ns; call emit_gpu_sample_on() per resolved span (or
    // emit_gpu_sample() for the default execution track) so each lands on its (device,queue) track. Idempotent: a
    // frame is resolved at most once.
    virtual void resolve_completed_frames() noexcept = 0;

    // GPU "tick rate" in nanoseconds per tick. Vulkan:
    // `VkPhysicalDeviceLimits::timestampPeriod`. Backend caches at init.
    [[nodiscard]] virtual crd::f64 ns_per_tick() const noexcept = 0;
};

// Install / replace / clear the backend. Pass nullptr to clear.
void set_gpu_backend(IProfilerGpuBackend* backend) noexcept;

// Returns the currently installed backend, or nullptr if none.
[[nodiscard]] IProfilerGpuBackend* current_gpu_backend() noexcept;

// Manual resolve-only convenience (the backend skips frames whose GPU work hasn't retired yet). DIAG.6b(e): `frame_mark`
// already drives resolve once per CPU frame, so this is now redundant in a frame-marked app -- it stays for apps that do
// not call `frame_mark`, and is a harmless idempotent no-op if called in addition. It does NOT end/begin frames.
void resolve_gpu_frames() noexcept;

// ---- The profiler's "gpu" track ----
//
// Resolved GPU spans land on a dedicated thread track registered by the
// backend at construction time. Use this index to query the spans from
// the timeline UI. Returns 0xFF when no GPU backend is active.
[[nodiscard]] crd::u8 gpu_thread_index() noexcept;

// Backend-side helper -- write a fully-formed Sample into the gpu thread's
// ring. The backend builds the Sample after resolving the query pool and
// converting ticks to ns. Public because the backend lives in a separate
// module (a gpu-context backend, DIAG.6b(i)); not intended for application code.
//
// DIAG.6b(c): this is the single-track convenience form -- it routes onto the default execution track (device 0, queue 0)
// with no correlation identity. Multi-queue / multi-device backends should call emit_gpu_sample_on() instead so each
// queue lands on its own track and carries its (device,queue) identity.
void emit_gpu_sample(Sample sample) noexcept;

// ---- DIAG.6b(c): separate per-(device,queue) GPU tracks -----------------
//
// GPU work is modelled on separate tracks, not one "gpu" track aliased onto the registering CPU thread. Each distinct
// (device_id, queue_id) key gets its own profiler slot, named "gpu d<dev> q<queue>", registered lazily on first emit and
// made correlation-ready. Bounded: at most kMaxGpuTracks distinct keys; further keys fold onto a shared "gpu overflow"
// track and bump gpu_track_overflow_count() -- folded, never dropped silently.

struct GpuTrackKey
{
    crd::u32 device_id = 0U; // device / adapter index (multi-device)
    crd::u32 queue_id  = 0U; // queue within the device (graphics / compute / copy / ...)
};

// The kind of GPU work, mapped to a profiler Category. An enum (not a raw Category) so a backend cannot ship an
// unrelated category (e.g. Memory) onto a GPU track. QueueWait models submit->execution-start latency; its producer (a
// submit timestamp) is DIAG.6b(d)/(f)/(i) -- (c) only plumbs the track + category so a backend can route it.
enum class GpuSampleKind : crd::u8
{
    Execution = 0, // -> Category::Gpu  : the executed pass / dispatch
    Transfer  = 1, // -> Category::Io   : copy / transfer-queue work (the row's "I/O")
    QueueWait = 2, // -> Category::Wait : submit -> execution-start latency
};

// At most this many distinct (device,queue) tracks; leaves the rest of kMaxThreads (64) for CPU/job threads.
inline constexpr crd::u32 kMaxGpuTracks = 16U;

// Emit a resolved GPU Sample onto the track for `key`, tagged by `kind`. The Sample's category and thread fields are
// forced from `kind` / the resolved track (a backend cannot mis-route). A correlation record carrying the (device,queue)
// identity and the pass name (from sample.name_id) is attached; clock_domain / clock_uncertainty_ns stay 0 -- DIAG.6b(d)
// fills those from real calibration. No-op if the profiler is down or all track slots are exhausted.
void emit_gpu_sample_on(Sample sample, GpuTrackKey key, GpuSampleKind kind) noexcept;

// Number of GPU samples folded onto the shared overflow track: one per emit whose (device,queue) key exceeded
// kMaxGpuTracks. Over-cap keys are NOT tabled, so each such emit re-folds under the registration lock -- a real backend
// with more than kMaxGpuTracks live queues (DIAG.6b(i)) pays a cold-path spinlock per over-cap sample.
[[nodiscard]] crd::u32 gpu_track_overflow_count() noexcept;

// ---- DIAG.6b(d): CPU<->GPU clock calibration + explicit uncertainty ------
//
// A backend that can sample both clocks at one instant installs a calibration per device. crd-perf then converts GPU
// ticks to the CPU timeline itself (via emit_gpu_span_on) and stamps each sample's CorrelationRecord with a real
// clock_uncertainty_ns bound -- a backend cannot hand-roll an offset through this path. A device with no calibration
// still records on its own track in the raw GPU-tick domain (clock_domain = kClockDomainGpuBase + device), flagged
// uncalibrated with a sentinel uncertainty and counted by uncalibrated_span_count() -- "retain separate tracks".

// At most this many distinct devices carry a calibration; device_id >= kMaxGpuDevices takes the uncalibrated path.
inline constexpr crd::u32 kMaxGpuDevices = 4U;

// ---- DIAG.6b(e): backend-agnostic queue-id contract ---------------------
//
// The GpuTrackKey.queue_id a producer passes must mean the same thing across backends, or two backends disagree on what
// "queue 1" is and the UI cannot label tracks. These are the agreed values; a frame-graph producer maps its own queue
// enum (e.g. FgQueue) onto them. Plain u32 constants -- crd-perf never names the frame-graph's types (no upward dep).
inline constexpr crd::u32 kGpuQueueGraphics     = 0U; // the primary graphics/direct queue
inline constexpr crd::u32 kGpuQueueAsyncCompute = 1U; // an async-compute queue
inline constexpr crd::u32 kGpuQueueTransfer     = 2U; // a copy/transfer queue (the row's "I/O")

// Install/update the calibration for `device_id`. Keeps the previous calibration too, so emit_gpu_span_on can bound a
// span by the real drift measured between the two. Cold path (per-frame or per-N-frames); no-op if device_id is out of
// range. The measured instant must NOT be derived by subtracting a CPU-submit from a GPU-execute timestamp.
void set_gpu_clock_calibration(crd::u32 device_id, const crd::time::GpuClockCalibration& calibration) noexcept;

// Emit a resolved GPU span given in raw GPU ticks. crd-perf performs the tick->ns conversion: if `key.device_id` has a
// calibration the endpoints land on the CPU timeline (clock_domain = kClockDomainCpu, flag kCorrelationCalibrated set,
// a real uncertainty bound); otherwise they stay in the device's raw GPU-tick domain (uncalibrated, sentinel
// uncertainty, counted). `ns_per_tick` is used only for the uncalibrated GPU-domain conversion -- the calibrated path
// uses the calibration's own period. No-op if the profiler is down or all track slots are exhausted.
void emit_gpu_span_on(GpuTrackKey key, GpuSampleKind kind, NameId name, crd::u64 begin_ticks, crd::u64 end_ticks,
                      crd::f64 ns_per_tick) noexcept;

// Number of GPU spans emitted (via emit_gpu_span_on) onto a device that had no calibration installed.
[[nodiscard]] crd::u32 uncalibrated_span_count() noexcept;

// ---- DIAG.6b(f): resolve robustness -- narrow counters, invalid spans, and backend-reported failures --------------
//
// A hardware timestamp counter may be narrower than 64 bits and wrap within a frame. Set the device's valid bit-width so
// emit_gpu_span_on can repair a single wrap (`end < begin` within the window) into a forward span instead of a negative
// one. Default 64 (full width) when unset; clamped to [1, 64]; out-of-range device_id is ignored.
void set_gpu_timestamp_valid_bits(crd::u32 device_id, crd::u32 valid_bits) noexcept;

// Robustness counters. crd-perf increments the first two itself inside emit_gpu_span_on (it can see the tick values);
// the rest are reported BY the backend via the note_* calls below (crd-perf cannot see inside a query pool). All reset
// by shutdown. Never fabricate a timing to keep a count at zero -- an unreadable/invalid span is counted, not invented.
[[nodiscard]] crd::u32 gpu_wrap_repaired_count() noexcept;          // single-wrap spans repaired (narrow counter)
[[nodiscard]] crd::u32 gpu_invalid_span_count() noexcept;           // end<begin that is NOT a repairable wrap -> dropped
[[nodiscard]] crd::u32 gpu_timestamps_unavailable_count() noexcept; // backend query results not readable this resolve
[[nodiscard]] crd::u32 gpu_query_slot_reused_count() noexcept;      // a query slot reused before its span resolved
[[nodiscard]] crd::u32 gpu_device_lost_count() noexcept;            // device-loss events the backend reported
[[nodiscard]] crd::u32 gpu_pending_frames_dropped_count() noexcept; // in-flight frames dropped on device loss

// Backend-reported robustness notes (a producer calls these from its resolve/loss handling).
void note_gpu_timestamps_unavailable(crd::u32 n) noexcept;
void note_gpu_query_slot_reused(crd::u32 n) noexcept;
void note_gpu_device_lost(crd::u32 device_id, crd::u32 pending_frames_dropped) noexcept;

// ---------------------------------------------------------------------------
// GpuScopedRegion -- RAII pair of begin_span / end_span on a cmd buffer.
// ---------------------------------------------------------------------------

#if CRD_PERF_ENABLED

class GpuScopedRegion
{
public:
    GpuScopedRegion(void* cmd_buffer, NameId name) noexcept
        : m_cmd_buffer(cmd_buffer)
    {
        IProfilerGpuBackend* be = current_gpu_backend();
        if (be != nullptr && cmd_buffer != nullptr)
        {
            m_span = be->begin_span(cmd_buffer, name);
        }
    }

    ~GpuScopedRegion() noexcept
    {
        if (!m_span.is_valid())
        {
            return;
        }
        IProfilerGpuBackend* be = current_gpu_backend();
        if (be != nullptr && m_cmd_buffer != nullptr)
        {
            be->end_span(m_cmd_buffer, m_span);
        }
    }

    GpuScopedRegion(const GpuScopedRegion&)            = delete;
    GpuScopedRegion& operator=(const GpuScopedRegion&) = delete;
    GpuScopedRegion(GpuScopedRegion&&)                 = delete;
    GpuScopedRegion& operator=(GpuScopedRegion&&)      = delete;

private:
    void*         m_cmd_buffer;
    GpuSpanHandle m_span{kInvalidGpuSpan};
};

#else

class GpuScopedRegion
{
public:
    GpuScopedRegion(void*, NameId) noexcept {}
    ~GpuScopedRegion() noexcept = default;

    GpuScopedRegion(const GpuScopedRegion&)            = delete;
    GpuScopedRegion& operator=(const GpuScopedRegion&) = delete;
    GpuScopedRegion(GpuScopedRegion&&)                 = delete;
    GpuScopedRegion& operator=(GpuScopedRegion&&)      = delete;
};

#endif

} // namespace crd::perf

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

#define CRD_PERF_GPU_DETAIL_CAT_INNER(a, b) a##b
#define CRD_PERF_GPU_DETAIL_CAT(a, b) CRD_PERF_GPU_DETAIL_CAT_INNER(a, b)

#if CRD_PERF_ENABLED

// CRD_PERF_GPU_SCOPE(cmd_void_ptr, "name") -- declare a GpuScopedRegion in
// the enclosing scope. The NameId is interned once per call site via a
// TU-local static. `cmd_void_ptr` must point to the backend's concrete
// command-buffer type (e.g. a `VkCommandBuffer` handle for a Vulkan backend).
//
// Cost when no backend installed: one nullptr load + branch-not-taken
// per call site (begin + end).
#define CRD_PERF_GPU_SCOPE(cmd_void_ptr, name_literal)                                                 \
    static const ::crd::perf::NameId CRD_PERF_GPU_DETAIL_CAT(_crd_perf_gpu_name_, __LINE__) =          \
        ::crd::perf::intern_name(name_literal);                                                        \
    ::crd::perf::GpuScopedRegion CRD_PERF_GPU_DETAIL_CAT(_crd_perf_gpu_scope_, __LINE__)               \
    {                                                                                                  \
        (cmd_void_ptr), CRD_PERF_GPU_DETAIL_CAT(_crd_perf_gpu_name_, __LINE__)                         \
    }

#else

#define CRD_PERF_GPU_SCOPE(cmd_void_ptr, name_literal) ((void)0)

#endif
