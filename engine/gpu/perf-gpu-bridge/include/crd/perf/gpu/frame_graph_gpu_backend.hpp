// DIAG.6b(i): FrameGraphGpuBackend — adapt an IFrameGraph's per-pass GPU timings onto crd-perf's GPU tracks.
//
// The frame graph times ITSELF: it writes device TIMESTAMP queries around each pass in the frame's one command
// buffer and reads them back after the submit's fence (REN-8). This backend is therefore NOT the cmd-buffer span
// producer the generic IProfilerGpuBackend models — `begin_span`/`end_span` are inert no-ops here. Its real work
// happens in `resolve_completed_frames()`: it walks the last execute's passes and, for each pass whose RAW ticks
// the graph exposes (`pass_gpu_ticks`), emits a positioned GPU span via `emit_gpu_span_on` on the pass's mapped
// (device, queue) track with its mapped kind. A pass whose ticks are not available is COUNTED unavailable
// (`note_gpu_timestamps_unavailable`) — never invented from `pass_gpu_ms` (a duration with no begin).
//
// ⛔ (i2)-PENDING: no raster backend overrides `pass_gpu_ticks` yet (they retain only ms), so on real hardware
// TODAY every pass reports unavailable and this bridge emits zero positioned samples — the honest state, proven by
// the default-unavailable test. Retaining the raw tick pairs in the DX12/Vulkan `resolve_timestamps()` is (i2).
//
// The bridge installs NO clock calibration (there is no CPU+GPU paired-sample source yet), so its spans land in the
// device's raw GPU-tick domain: uncalibrated, sentinel uncertainty, counted by `uncalibrated_span_count()` — per
// DIAG.6b(d)'s "retain separate tracks" rule. Install once per process via `crd::perf::set_gpu_backend(&bridge)`;
// `frame_mark()` then drives resolve once per CPU frame. One backend per process.

#pragma once

#include <crd/core/types.hpp>
#include <crd/perf/gpu_scope.hpp>

namespace crd::gpu
{
class IFrameGraph;
} // namespace crd::gpu

namespace crd::perf::gpu
{
// Adapts `fg`'s per-pass GPU timings onto crd-perf. `device_id` names the adapter/device the frame graph runs on
// (the track key's device); default 0 for the single-device host. The bridge holds a reference to `fg` — `fg` must
// outlive the bridge, which is the natural lifetime (both live for the process).
class FrameGraphGpuBackend final : public crd::perf::IProfilerGpuBackend
{
public:
    explicit FrameGraphGpuBackend(crd::gpu::IFrameGraph& fg, crd::u32 device_id = 0U) noexcept;

    // IProfilerGpuBackend — the frame graph is its own timer, so the span API is inert.
    void begin_frame(crd::u64 frame_index) noexcept override;
    [[nodiscard]] crd::perf::GpuSpanHandle begin_span(void* cmd_buffer, crd::perf::NameId name) noexcept override;
    void end_span(void* cmd_buffer, crd::perf::GpuSpanHandle span) noexcept override;
    void end_frame() noexcept override;
    void resolve_completed_frames() noexcept override;
    [[nodiscard]] crd::f64 ns_per_tick() const noexcept override;

    // Diagnostics (the RET pattern — tests assert on counters, not eyeballed logs).
    [[nodiscard]] crd::u32 resolve_calls() const noexcept { return m_resolve_calls; }
    [[nodiscard]] crd::u32 emitted_spans() const noexcept { return m_emitted_spans; }

    // DIAG.6b(j): the placed GPU total vs the frame graph's own independent reduction (gpu_ms_total). A frame with
    // passes is counted match / mismatch / incomparable exactly once (gpu_ms_total is folded into the dedupe
    // fingerprint, so a real device's all-unavailable frames still count per frame). Incomparable = the total is
    // incomplete: a pass unavailable or an unknown period. An empty frame (no passes) counts nothing.
    // last_total_discrepancy_ns is the signed discrepancy (placed - reference) of the most recent comparison, never a
    // per-pass attribution.
    [[nodiscard]] crd::u32 total_match_count() const noexcept { return m_total_match_count; }
    [[nodiscard]] crd::u32 total_mismatch_count() const noexcept { return m_total_mismatch_count; }
    [[nodiscard]] crd::u32 total_incomparable_count() const noexcept { return m_total_incomparable_count; }
    [[nodiscard]] crd::i64 last_total_discrepancy_ns() const noexcept { return m_last_total_discrepancy_ns; }

private:
    // A stable content fingerprint of the last execute's per-pass timings. Re-resolving the SAME data (e.g. a
    // second `frame_mark` with no intervening `execute()`) must not re-emit the spans — the frame graph exposes no
    // execute counter, so the bridge dedupes on content. Invariant the doc states: an app calls `execute()` at
    // most once per `frame_mark()`, so distinct frames produce distinct fingerprints (their ticks differ).
    [[nodiscard]] crd::u64 fingerprint() const noexcept;

    // The fields read only under CRD_PERF_ENABLED are [[maybe_unused]] so the disabled-build TU (where the method
    // bodies are inert stubs) does not trip clang-cl's -Wunused-private-field -- the capture_view.hpp precedent.
    [[maybe_unused]] crd::gpu::IFrameGraph& m_fg;
    [[maybe_unused]] crd::u32               m_device;
    [[maybe_unused]] crd::u64 m_last_fingerprint = 0ULL;
    [[maybe_unused]] bool     m_have_last        = false;
    crd::u32                  m_resolve_calls    = 0U; // read by the always-compiled resolve_calls() getter
    crd::u32                  m_emitted_spans    = 0U; // read by the always-compiled emitted_spans() getter

    // (j) comparison counters -- read by always-compiled inline getters, so no [[maybe_unused]] needed.
    crd::u32 m_total_match_count        = 0U;
    crd::u32 m_total_mismatch_count     = 0U;
    crd::u32 m_total_incomparable_count = 0U;
    crd::i64 m_last_total_discrepancy_ns = 0;
};
} // namespace crd::perf::gpu
