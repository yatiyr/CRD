#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- allocator memory tracking (Detour D-003 v0e).
//
// Register one or more `crd::memory::IAllocator*` with the profiler. At
// every `frame_mark()` the profiler reads each allocator's `MemoryStats`
// snapshot and stamps an `AllocatorRecord` into the FrameRecord ring.
// The UI surfaces these as a per-allocator panel with line plots over
// the 240-frame history (alloc count, dealloc count, bytes_in_use, peak,
// total bytes ever).
//
// Allocator stats live on the allocator instance itself (`IAllocator::stats()`)
// and are updated by every concrete allocator implementation
// (TlsfAllocator, GrowablePoolAllocator, MallocAllocator, ChunkAllocator).
// **The widened `CRD_MEMORY_STATS_TRACKING` gate (v0e)** ensures stats are
// kept up to date in `CRD_DEBUG` builds AND in any release/shipping build
// where `CRD_ENABLE_PROFILING=1` -- so `win-shipping-profile` shows real
// numbers instead of zeros.
//
// Usage:
//
//   auto& tlsf = my_engine.tlsf_root();
//   crd::perf::register_allocator("TLSF root", &tlsf);
//
//   // ... per-frame:
//   crd::perf::frame_mark();
//
//   // ... UI / capture:
//   const auto snap = crd::perf::allocator_snapshot(0);
//   imgui::TextFmt("alloc={} bytes_in_use={}", snap.alloc_count, snap.bytes_in_use);
//
// Registration is mutex-protected (cold path). The per-frame stat read is
// a single relaxed-atomic load on each counter (~5 ns per allocator).
// Saturation past `kMaxAllocators` returns `kInvalidAllocatorIdx` and
// asserts in debug builds.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/frame_record.hpp>

namespace crd::memory
{
class IAllocator;
} // namespace crd::memory

namespace crd::perf
{

inline constexpr crd::u32 kInvalidAllocatorIdx = 0xFFFF'FFFFU;

// Friendly metadata for one registered allocator -- the name shown in the
// UI, plus the underlying allocator pointer (so backends / tests can call
// stats() directly when needed).
struct AllocatorInfo
{
    const char*              name      = "";
    crd::memory::IAllocator* allocator = nullptr;
};

// Snapshot of a single allocator's stats -- decoupled from the per-frame
// AllocatorRecord (which is the on-disk-format-pinned struct). Returned
// by `allocator_snapshot()` for live UI display.
struct AllocatorSnapshot
{
    const char* name           = "";
    crd::u64    alloc_count    = 0;
    crd::u64    dealloc_count  = 0;
    crd::u64    bytes_in_use   = 0;
    crd::u64    peak_bytes     = 0;
    crd::u64    total_bytes    = 0;
};

// Register an allocator under `name`. Returns the slot index assigned.
// Re-registering the SAME allocator pointer returns the existing index.
// Returns `kInvalidAllocatorIdx` if the profiler isn't initialised, if
// the table is saturated, or if either argument is null.
//
// `name` must be a static-storage string (lifetime >= profiler lifetime).
[[nodiscard]] crd::u32 register_allocator(const char* name,
                                          crd::memory::IAllocator* allocator) noexcept;

// Unregister by index. Idempotent; out-of-range or already-unregistered
// indices are no-ops.
void unregister_allocator(crd::u32 allocator_idx) noexcept;

// Number of currently-registered allocators. May change between calls;
// take a copy if you iterate.
[[nodiscard]] crd::u32 registered_allocator_count() noexcept;

// Metadata for one slot. Returns `{nullptr, nullptr}` if slot is
// out-of-range or unregistered.
[[nodiscard]] AllocatorInfo allocator_info(crd::u32 allocator_idx) noexcept;

// Live snapshot (reads the allocator's MemoryStats directly). Returns
// the canonical zero snapshot if slot is empty / out-of-range.
[[nodiscard]] AllocatorSnapshot allocator_snapshot(crd::u32 allocator_idx) noexcept;

// Historical snapshot from the FrameRecord ring. `frames_back == 0`
// returns the most recent capture; `frames_back == 1` the previous; up
// to `kFrameHistorySlots - 1`. Returns the canonical zero snapshot if
// not enough frames have been captured.
[[nodiscard]] AllocatorSnapshot allocator_snapshot_history(crd::u32 allocator_idx,
                                                          crd::u32 frames_back) noexcept;

} // namespace crd::perf
