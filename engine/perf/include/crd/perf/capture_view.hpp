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

    // Resolve an interned NameId via the in-blob string table.
    [[nodiscard]] const char* resolve_name(NameId id) const noexcept;

private:
    [[nodiscard]] const CprofHeader*    header() const noexcept;
    [[nodiscard]] const ThreadHeader*   thread_header(crd::u32 i) const noexcept;
    [[nodiscard]] const CounterMeta*    counter_meta_at(crd::u32 i) const noexcept;
    [[nodiscard]] const AllocatorMeta*  allocator_meta_at(crd::u32 i) const noexcept;

    crd::containers::ConstSpan<crd::u8> m_buf;
    bool                                m_valid              = false;

    // Cached byte offsets (set in ctor; -1 == not present).
    crd::usize m_off_thread_headers   = 0;
    crd::usize m_off_counter_meta     = 0;
    crd::usize m_off_allocator_meta   = 0;
    crd::usize m_off_name_blob        = 0;
    crd::usize m_off_frame_records    = 0;
    // Sample arrays are indexed via each ThreadHeader's sample_byte_offset.

    // Name blob layout: u32 count + u32 byte_size, then u32 offsets[count],
    // then packed strings.
    crd::u32 m_name_count       = 0U;
    crd::u64 m_name_blob_bytes  = 0ULL;
};

} // namespace crd::perf
