#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- per-frame snapshot record (Detour D-003 v0b).
//
// At every frame_mark() the profiler snapshots:
//   - the frame index + begin / end timestamps,
//   - every registered counter's current value (as raw bits; type-decoded
//     at read time via the counter table),
//   - (future) per-thread head positions into the sample ring,
//   - (future) GPU pass tally.
//
// FrameRecord is a POD that lives inside the rolling history ring
// (kFrameHistorySlots = 240 slots by default). The ring is a producer-
// single-consumer-many surface: frame_mark() is called from one thread
// (the main game / sim thread); UI / capture readers may read any
// frame_index whose write has retired.
//
// Layout is sized to fit a comfortable number of counters per frame
// (kMaxCounters = 256). At 240 history slots * (32 B header + 256 * 8 B
// values) the ring is ~500 KB -- a fixed pre-allocation paid once at
// init(), no per-frame allocator pressure.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/memory/statistics_coverage.hpp> // DIAG.11b allocator-record coverage stamp
#include <crd/perf/config.hpp>
#include <crd/perf/sample.hpp> // NameId, kInvalidNameId (DIAG.6a(d2) allocator-record name identity)

namespace crd::perf
{

// One per-counter raw value. Type-decode happens at read time using
// CounterType from the counter table (looked up by index).
struct alignas(8) RawCounterValue
{
    crd::u64 bits = 0; // 8 -- i64 sign-extended, or f64 via bit_cast, or i64 ns for Duration
};

static_assert(sizeof(RawCounterValue) == 8, "RawCounterValue is 8 B");

// Per-allocator snapshot stamped at frame_mark(). Mirrors MemoryStats::Snapshot
// shape; layout is pinned for on-disk CPROF (v0f).
struct alignas(8) AllocatorRecord
{
    crd::u64 alloc_count   = 0; // 8
    crd::u64 dealloc_count = 0; // 8
    crd::u64 bytes_in_use  = 0; // 8
    crd::u64 peak_bytes    = 0; // 8
    crd::u64 total_bytes   = 0; // 8
    crd::u64 stamps        = 0; // 8 -- two stamps, each 0 when unset (every older capture reads unchanged):
                                //      bits 0-31: DIAG.6a(d2) the allocator's interned name identity AT THIS FRAME as
                                //      name_id + 1 (0 -> resolve via the live AllocatorMeta name); pins the label
                                //      across allocator slot reuse. bits 32-39: DIAG.11b what the counts cover, as
                                //      StatisticsCoverage + 1. Decoded only by the helpers below; bits 40-63 are 0.
};

static_assert(sizeof(AllocatorRecord) == 48, "AllocatorRecord is 48 B");

inline constexpr crd::u64 kAllocatorRecordNameBits = 0xFFFF'FFFFULL;
inline constexpr crd::u32 kAllocatorRecordCoverageShift = 32U;
inline constexpr crd::u64 kAllocatorRecordCoverageBits = 0xFFULL;

// The stamps of one record: `name_id` (kInvalidNameId when the slot has none) and the coverage of its counts.
[[nodiscard]] inline crd::u64 allocator_record_stamps(crd::u32 name_id,
                                                      crd::memory::StatisticsCoverage coverage) noexcept
{
    const crd::u64 name = name_id != kInvalidNameId.value ? static_cast<crd::u64>(name_id) + 1U : 0U;
    const crd::u64 cover = static_cast<crd::u64>(static_cast<crd::u8>(coverage)) + 1U;
    return name | (cover << kAllocatorRecordCoverageShift);
}

// DIAG.6a(d2): decode AllocatorRecord::stamps -> the NameId stamped at capture time (kInvalidNameId when unset). Both
// the file reader (CaptureView) and any live consumer decode the +1 sentinel through this one helper so it is never
// re-derived. A valid id resolves through the capture's name blob / the live intern table; an invalid id means the
// record predates (d2) (or interning was saturated) -- fall back to the slot's live name.
[[nodiscard]] inline NameId allocator_record_name_id(const AllocatorRecord& ar) noexcept
{
    const crd::u64 name = ar.stamps & kAllocatorRecordNameBits;
    return name != 0U ? NameId{static_cast<crd::u32>(name - 1U)} : kInvalidNameId;
}

// DIAG.11b: what the record's counts cover. Unrecorded for a record stamped before the coverage was (a capture from
// before 2026-10-19, whose counting allocators were then counted from construction) or with a value this build does
// not know (a capture is untrusted input).
[[nodiscard]] inline crd::memory::StatisticsCoverage allocator_record_coverage(const AllocatorRecord& ar) noexcept
{
    const crd::u64 cover = (ar.stamps >> kAllocatorRecordCoverageShift) & kAllocatorRecordCoverageBits;
    if (cover == 0U || cover - 1U >= static_cast<crd::u64>(crd::memory::StatisticsCoverage::Unrecorded))
    {
        return crd::memory::StatisticsCoverage::Unrecorded;
    }
    return static_cast<crd::memory::StatisticsCoverage>(cover - 1U);
}

struct FrameRecord
{
    crd::u64 frame_index     = 0; // 8
    crd::i64 frame_begin_ns  = 0; // 8 -- MonotonicClock at the frame's first sample / prior frame_mark
    crd::i64 frame_end_ns    = 0; // 8 -- MonotonicClock at this frame_mark
    crd::u32 counter_count   = 0; // 4 -- registered counter count at capture (table grows monotonically)
    crd::u32 allocator_count = 0; // 4 -- registered allocator count at capture
    RawCounterValue values[kMaxCounters]{};      // 256 * 8  = 2048
    AllocatorRecord allocators[kMaxAllocators]{}; // 32 * 48 = 1536
};

static_assert(sizeof(FrameRecord) == 32
                                       + sizeof(RawCounterValue) * kMaxCounters
                                       + sizeof(AllocatorRecord) * kMaxAllocators,
              "FrameRecord layout is pinned -- on-disk capture (v0f) depends on it");

} // namespace crd::perf
