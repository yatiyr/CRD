#pragma once
// crd-perf -- DIAG.6c(c): bounded Perfetto/Chrome trace-events JSON export from a parsed CPROF capture.
//
// A CaptureView (any loaded .cprof buffer) is rendered as a Chrome trace-events JSON document that
// ui.perfetto.dev loads directly -- OFFLINE, no dependency, no service account. This is the interoperable
// export ADR-0133 DG14 asks for; the native viewer (crd-perf-ui) stays optional and is NOT a dependency of
// this path.
//
// Domain honesty (DIAG.6b): CPU-timeline samples and calibrated GPU samples go on process 0 in microseconds;
// UNCALIBRATED GPU-tick samples (a correlation record whose clock_domain is a raw GPU domain) go on their own
// process id `1 + device`, with their timestamps emitted as the RAW tick values they are -- never rescaled into
// the CPU timeline. The process name states they are raw ticks.
//
// The exporter never blocks, allocates only into the caller's `out`/allocator, and uses crd:: containers only.

#include <crd/perf/config.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::perf
{

// Counts of what the export actually wrote -- lets a caller/test assert coverage rather than trust it.
struct PerfettoExportStats
{
    crd::u64 sample_events   = 0; // "X" complete events emitted for scope samples
    crd::u64 frame_events    = 0; // "X" complete events emitted for frame spans
    crd::u64 counter_events  = 0; // "C" counter events emitted
    crd::u64 metadata_events = 0; // "M" process_name / thread_name events emitted
    crd::u64 instant_events  = 0; // "i" instants emitted (dropped-sample loss markers)
    crd::u64 dropped_samples = 0; // sum of ThreadHeader.dropped_count reported (loss, never hidden)
};

// Render `view` as a Perfetto trace-events JSON document appended to `out`. Returns false if the view is invalid
// or profiling is compiled out (the #else stub). `out` must be constructed with a real allocator.
[[nodiscard]] bool export_perfetto_json(const CaptureView& view,
                                        crd::containers::String& out,
                                        PerfettoExportStats& stats) noexcept;

// Convenience twin: build the document with `alloc` and write it to `path`. Returns false on invalid view,
// profiling-off, or a file error.
[[nodiscard]] bool export_perfetto_json_to_file(const CaptureView& view,
                                                const char* path,
                                                PerfettoExportStats& stats,
                                                crd::memory::IAllocator* alloc) noexcept;

} // namespace crd::perf
