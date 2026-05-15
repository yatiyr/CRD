#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- umbrella header (Detour D-003).
//
// Pulls in the public surface: config gate, Sample POD, Profiler singleton,
// ScopedRegion RAII + CRD_PERF_* macros.
//
// Counters / GPU scopes / file capture / auto-instrumentation hooks ship in
// follow-up slices and have their own headers (v0b-v0g). This umbrella
// stays narrow on purpose -- v0a substrate only.
// ---------------------------------------------------------------------------

#include <crd/perf/capture.hpp>
#include <crd/perf/capture_view.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/counters.hpp>
#include <crd/perf/frame_record.hpp>
#include <crd/perf/gpu_scope.hpp>
#include <crd/perf/jobs_adapter.hpp>
#include <crd/perf/memory.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/perf/sample.hpp>
#include <crd/perf/scope.hpp>
