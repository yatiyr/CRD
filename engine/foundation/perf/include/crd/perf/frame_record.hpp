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
#include <crd/perf/config.hpp>

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
    crd::u64 _pad          = 0; // 8 -- pad to 48 B / multiple of 8 alignment + room for v0f flags
};

static_assert(sizeof(AllocatorRecord) == 48, "AllocatorRecord is 48 B");

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
