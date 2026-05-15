#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- compile-time gate + sizing knobs (Detour D-003).
//
// The gate is `CRD_ENABLE_PROFILING` (a #cmakedefine01 in build_config.hpp).
// When 0, every CRD_PERF_* macro collapses to `((void)0)` and the substrate
// state is reduced to inert stubs -- zero codegen overhead. Verified by the
// v0a objdump-equality test.
//
// Sizing knobs are compile-time constants exposed via this header so the
// hot-path code can rely on `if constexpr` over them, never branches.
// ---------------------------------------------------------------------------

#include <crd/core/build_config.hpp>
#include <crd/core/types.hpp>

namespace crd::perf
{

// ---- Compile-time enable bit --------------------------------------------
//
// The build_config flag is exposed as `CRD_ENABLE_PROFILING` (0 or 1). The
// macros use it directly so a single grep reveals every gated site.
#ifndef CRD_PERF_ENABLED
#define CRD_PERF_ENABLED CRD_ENABLE_PROFILING
#endif

inline constexpr bool kEnabled = CRD_PERF_ENABLED != 0;

// ---- Sizing knobs -------------------------------------------------------
//
// All counts are compile-time constants. Anything that crosses the
// hot-path boundary (Sample size, ring slot count, per-thread frame
// budget) is fixed at build time so the code generator can fold it.

// Per-thread ring buffer slot count. Default 4096 = 128 KB per thread
// at 32 B/sample. SPSC; the recorder thread is the only writer. The
// reader (frame_mark / capture flush) snapshots head + tail under a
// relaxed-atomic load.
inline constexpr crd::u32 kPerThreadRingSlots = 4096U;

// Maximum threads the profiler tracks. Hard cap to keep the per-thread
// array indexable by a 1-byte field in Sample (must fit u8).
inline constexpr crd::u32 kMaxThreads = 64U;
static_assert(kMaxThreads <= 255U, "Sample::begin_thread is u8; cap is 255");

// Maximum interned region names. Indexed by Sample::name_id (u32, but
// table-bounded). Engine-side scope names are static strings; a few
// thousand is plenty.
inline constexpr crd::u32 kMaxRegionNames = 4096U;

// Per-frame max GPU spans (multi-frame in-flight). 256 GPU regions per
// frame at 4 frames-in-flight = 1024 query slots default.
inline constexpr crd::u32 kMaxGpuSpansPerFrame = 256U;
inline constexpr crd::u32 kGpuFramesInFlight   = 4U;

// Rolling frame snapshot history. UI line plots read from this ring.
inline constexpr crd::u32 kFrameHistorySlots = 240U;

// Counter table size (named user counters: i64 / f64 / Duration).
inline constexpr crd::u32 kMaxCounters = 256U;

// Memory-allocator registry size. Engine has ~10 active allocators
// (TLSF root + GrowablePool buckets + scene ChunkAllocator pools); test
// fixtures bring transient ones. 32 slots is generous; bump if the
// register call ever asserts on saturation.
inline constexpr crd::u32 kMaxAllocators = 32U;

} // namespace crd::perf
