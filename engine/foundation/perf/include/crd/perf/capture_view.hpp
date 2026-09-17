#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- read-only view into a loaded CPROF capture (D-003 v0f).
//
// `CaptureView` parses a CPROF buffer once at construction and provides
// read-only accessors mirroring the live profiler's introspection API:
//
//   live profiler                        CaptureView
//   -----------                          -----------
//   thread_count()                       thread_count()
//   thread_samples(idx)                  thread_samples(idx)
//   counter_count()                      counter_count()
//   counter_info(id)                     counter_info(id)
//   registered_allocator_count()         allocator_count()
//   allocator_info(idx)                  allocator_info(idx)
//   frame_record(N) / frame_record_count frame_record(N) / frame_record_count
//   resolve_name(id)                     resolve_name(id)
//
// The v0g UI panel code targets a common read-only interface so it can
// render either the live profiler or a loaded CaptureView -- replay /
// diffing / regression checking all fall out of the same view shape.
//
// The CaptureView holds a non-owning `ConstSpan<u8>` into the source
// buffer; the buffer must outlive the view. Designed for stack
// allocation (no virtual dispatch, no allocations after parse).
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/capture.hpp>
#include <crd/perf/counters.hpp>
#include <crd/perf/frame_record.hpp>
#include <crd/perf/memory.hpp>
#include <crd/perf/sample.hpp>

namespace crd::perf
{

class CaptureView
{
public:
    // Default-constructed = invalid (size 0).
    CaptureView() noexcept = default;

    // Parse a buffer. Sets is_valid() to false on any size / magic /
    // version mismatch. Cheap; just bookkeeps byte offsets.
    explicit CaptureView(crd::containers::ConstSpan<crd::u8> buf) noexcept;

    [[nodiscard]] bool is_valid() const noexcept { return m_valid; }

    // Header introspection.
    [[nodiscard]] crd::u32 thread_count() const noexcept;
    [[nodiscard]] crd::u32 counter_count() const noexcept;
    [[nodiscard]] crd::u32 allocator_count() const noexcept;
    [[nodiscard]] crd::u32 frame_record_count() const noexcept;
    [[nodiscard]] crd::u64 captured_at_ns() const noexcept;

    // Thread accessors. `thread_index` is the dense capture-side index
    // (0..thread_count()-1), NOT the live profiler's thread index.
    [[nodiscard]] const char* thread_name(crd::u32 thread_index) const noexcept;
    [[nodiscard]] crd::u32 thread_sample_count(crd::u32 thread_index) const noexcept;
    [[nodiscard]] crd::u32 thread_dropped_count(crd::u32 thread_index) const noexcept;
    [[nodiscard]] crd::containers::ConstSpan<Sample>
    thread_samples(crd::u32 thread_index) const noexcept;

    // Counter / allocator accessors.
    [[nodiscard]] CounterInfo counter_info(crd::u32 counter_idx) const noexcept;
    [[nodiscard]] AllocatorInfo allocator_info(crd::u32 allocator_idx) const noexcept;

    // Frame history.
    [[nodiscard]] crd::containers::ConstSpan<FrameRecord> frame_records() const noexcept;

    // DIAG.6a(d2): the allocator name recorded for slot `allocator_idx` at frame `frame_index` (dense capture-side
    // frame index, 0 = oldest). Prefers the per-record stamped name identity -- correct even after the slot was
    // unregistered and reused by a different allocator -- and falls back to the live-at-save AllocatorMeta name for
    // pre-(d2) records (`_pad == 0`). Returns "" if either index is out of range.
    [[nodiscard]] const char* frame_allocator_name(crd::u32 frame_index, crd::u32 allocator_idx) const noexcept;

    // Resolve an interned NameId via the in-blob string table.
    [[nodiscard]] const char* resolve_name(NameId id) const noexcept;

    // DIAG.6b(b): the optional correlation side table (queue/device/clock-domain/uncertainty/pass/resource per sample).
    // 0 when the capture carries no correlation (old files, or a save with none enabled). The records are sorted by
    // (thread_index, sample_ordinal). `correlation_at` returns the i-th record (nullptr if out of range);
    // `correlation_for` looks up a specific sample's record by (thread_index, ordinal) via binary search (nullptr if
    // that sample has no correlation). `thread_index` is the capture's `ThreadHeader.thread_index`, which the writer
    // keeps equal to the dense thread position -- so it matches the index passed to `thread_samples`/`thread_name`.
    [[nodiscard]] crd::u32 correlation_count() const noexcept;
    [[nodiscard]] const CorrelationRecord* correlation_at(crd::u32 i) const noexcept;
    [[nodiscard]] const CorrelationRecord* correlation_for(crd::u32 thread_index,
                                                           crd::u32 sample_ordinal) const noexcept;

private:
    [[nodiscard]] const CprofHeader*    header() const noexcept;
    [[nodiscard]] const ThreadHeader*   thread_header(crd::u32 i) const noexcept;
    [[nodiscard]] const CounterMeta*    counter_meta_at(crd::u32 i) const noexcept;
    [[nodiscard]] const AllocatorMeta*  allocator_meta_at(crd::u32 i) const noexcept;

    crd::containers::ConstSpan<crd::u8> m_buf;
    bool                                m_valid              = false;

    // Cached byte offsets (set in ctor; -1 == not present). All marked
    // [[maybe_unused]] because the reading code is itself gated behind
    // `#if CRD_PERF_ENABLED`; clang-cl in shipping (PROFILING=OFF) would
    // otherwise flag them as -Werror=unused-private-field.
    [[maybe_unused]] crd::usize m_off_thread_headers   = 0;
    [[maybe_unused]] crd::usize m_off_counter_meta     = 0;
    [[maybe_unused]] crd::usize m_off_allocator_meta   = 0;
    [[maybe_unused]] crd::usize m_off_name_blob        = 0;
    [[maybe_unused]] crd::usize m_off_frame_records    = 0;
    // Sample arrays are indexed via each ThreadHeader's sample_byte_offset.

    // Name blob layout: u32 count + u32 byte_size, then u32 offsets[count],
    // then packed strings.
    [[maybe_unused]] crd::u32 m_name_count       = 0U;
    [[maybe_unused]] crd::u64 m_name_blob_bytes  = 0ULL;

    // DIAG.6b(b): correlation section (0 offset/count when absent). Set in the ctor after strict validation.
    [[maybe_unused]] crd::usize m_off_correlation   = 0U;
    [[maybe_unused]] crd::u32   m_correlation_count = 0U;
};

} // namespace crd::perf
