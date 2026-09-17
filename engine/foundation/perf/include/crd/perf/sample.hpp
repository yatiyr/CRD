#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- the canonical 32-byte Sample POD (Detour D-003).
//
// One Sample = one scope (begin + end paired). Fiber migration is captured
// by the two thread fields:
//
//   begin_thread  -- OS-thread index when ScopedRegion was constructed
//   end_thread    -- OS-thread index when ScopedRegion was destroyed
//
// If they differ, the scope migrated across a fiber yield. UI renders the
// region with a split-gap. The per-fiber assembly is reconstructed from
// fiber-yield events emitted by the JobObserver (v0c).
//
// Layout is fixed -- the on-disk capture format (v0f) memcpy's Sample arrays
// verbatim. Any change to this struct bumps the CPROF version.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>

namespace crd::perf
{

// Strongly-typed name handle. Returned by intern_name(); cached by the
// CRD_PERF_SCOPE macro in a TU-local static so the lookup happens once
// per call site at first hit.
struct NameId
{
    crd::u32 value = 0xFFFF'FFFFU;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0xFFFF'FFFFU; }
};

inline constexpr NameId kInvalidNameId{0xFFFF'FFFFU};

// Strongly-typed category for region color-coding. Reserved for v0c when
// the auto-instrumentation hooks tag jobs / scene-systems / frame-graph /
// rhi-cmd regions with their domain.
enum class Category : crd::u8
{
    User       = 0,
    Job        = 1,
    System     = 2,
    Pass       = 3,
    Render     = 4,
    Gpu        = 5,
    Memory     = 6,
    Io         = 7,
    Wait       = 8,
};

// 32-byte paired Sample. POD; trivially-copyable for memcpy into the
// capture buffer. Layout is verified by static_assert in sample.cpp.
struct Sample
{
    crd::i64 begin_ns;        // 8  -- MonotonicClock-relative ns since epoch
    crd::i64 end_ns;          // 8  -- ditto
    crd::u32 name_id;         // 4  -- index into the interned name table
    crd::u32 color_rgba;      // 4  -- premultiplied RGBA; 0 = inherit-from-category
    crd::u8  begin_thread;    // 1  -- index into kMaxThreads
    crd::u8  end_thread;      // 1  -- migrated when begin != end
    crd::u8  depth;           // 1  -- nesting depth at scope-begin (0-based)
    crd::u8  category;        // 1  -- Category enum
    crd::u32 fiber_id;        // 4  -- 0 = no fiber / OS-thread context only (v0c sets this)
};

static_assert(sizeof(Sample) == 32, "Sample is the canonical 32-byte POD; on-disk format depends on this");
static_assert(alignof(Sample) == 8, "Sample alignment must be 8 (i64 fields)");

// DIAG.6b(b): per-sample GPU/queue correlation, carried SLOT-PARALLEL to the sample ring (not inside Sample, which is a
// pinned 32 B format). A thread that opts in (enable_thread_correlation) gets a CorrelationRecord array indexed by the
// same ring slot as its samples; the copier copies both arrays in lockstep, so a saved sample's ordinal indexes its
// record by construction (no key, wrap-safe). On disk the records are a SPARSE list (only `kValid` ones) in an appended
// CPROF section keyed by (thread_index, sample_ordinal) -- see capture.hpp. `thread_index`/`sample_ordinal` are 0 in the
// live slot array and stamped at save. Pinned POD like Sample; the on-disk section depends on this size.
inline constexpr crd::u32 kCorrelationValid = 0x1U; // flags bit0: this slot carries a real record (else all-zero = none)
inline constexpr crd::u32 kNoCorrelationName = 0xFFFF'FFFFU; // pass_id/resource_id sentinel = no name

// DIAG.6b(d): CPU<->GPU clock calibration flags/sentinels for CorrelationRecord.
// Rule: kCorrelationCalibrated set  => begin_ns/end_ns are on the CPU timeline AND clock_uncertainty_ns is a real bound
//       (strictly > 0); clear => clock_uncertainty_ns MUST be kUnknownClockUncertainty (never a misleading 0).
inline constexpr crd::u32 kCorrelationCalibrated   = 0x2U;    // flags bit1: crd-perf calibrated this sample's endpoints
inline constexpr crd::u64 kUnknownClockUncertainty = ~0ULL;   // clock_uncertainty_ns when NOT calibrated (sentinel)
inline constexpr crd::u32 kClockDomainCpu          = 0U;      // clock_domain: CPU monotonic-ns timeline
inline constexpr crd::u32 kClockDomainGpuBase      = 1U;      // clock_domain: device d's raw GPU-tick domain == base + d

struct CorrelationRecord
{
    crd::u32 flags;                // 4  -- bit0 = kCorrelationValid
    crd::u32 thread_index;         // 4  -- stamped at save (0 in the live slot array)
    crd::u32 sample_ordinal;       // 4  -- stamped at save: position in the saved thread sample array
    crd::u32 queue_id;             // 4  -- device queue this work ran on
    crd::u32 device_id;            // 4  -- device/adapter index (multi-device)
    crd::u32 clock_domain;         // 4  -- timestamp domain tag (CPU-ns vs a GPU tick domain)
    crd::u64 clock_uncertainty_ns; // 8  -- explicit calibration error bound (DIAG.6b(d) sets a real value)
    crd::u32 pass_id;              // 4  -- interned NameId of the frame-graph pass (kNoCorrelationName = none)
    crd::u32 resource_id;          // 4  -- interned NameId of the resource (kNoCorrelationName = none)
};

static_assert(sizeof(CorrelationRecord) == 40, "CorrelationRecord is 40 B; on-disk section depends on this");
static_assert(alignof(CorrelationRecord) == 8, "CorrelationRecord alignment must be 8 (u64 field)");

} // namespace crd::perf
