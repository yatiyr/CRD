#pragma once

// ---------------------------------------------------------------------------
// crd-perf -- CPROF capture file format (Detour D-003 v0f).
//
// A "capture" is a serialised snapshot of the profiler's state at one
// instant: every thread's live sample ring + the frame history + counter
// metadata + allocator metadata + the interned-name table.
//
// File format -- CPROF v1 (FourCC 'CPRO' + u32 version + u64 flags):
//
//   [CprofHeader]                       -- magic + version + sizes
//   [ThreadHeader] x thread_count        -- name + sample count + index
//   [CounterMeta]  x counter_count       -- name + kind + type
//   [AllocatorMeta] x allocator_count    -- name + index
//   [NameBlob]                           -- packed interned-name strings
//   [FrameRecord] x frame_count          -- per-frame snapshot (counters + allocators)
//   [Sample]      x sum(thread sample counts)
//   [Correlation section]                -- DIAG.6b(b): OPTIONAL, 8-aligned, present iff
//                                           (flags & kCprofFlagCorrelation); located by
//                                           CprofHeader.correlation_section_offset. Layout:
//                                             u32 count; u32 record_size (== sizeof(CorrelationRecord));
//                                             CorrelationRecord x count   -- sparse, sorted by (thread_index, ordinal)
//                                           Old readers ignore the trailing section (validation tolerates a tail);
//                                           old files have flags bit clear / offset 0 -- new readers skip it.
//
// All structs are pinned POD; `FrameRecord` and `Sample` come from the
// existing layout (`frame_record.hpp` / `sample.hpp`) and are memcpy'd
// verbatim. Layout pins were established by previous v0a/v0b/v0e
// `static_assert`s; v0f's contribution is the wrapper headers.
//
// Endianness: little-endian only (x64 + ARM64 little). Cross-arch
// big-endian devices are not supported by Cerid today.
//
// Threading: `save_capture_to_buffer()` may be called from any thread.
// Each per-thread sample ring is read under an acquire-load of head/tail;
// concurrent push_region during save may or may not be included in the
// resulting capture (well-defined; not guaranteed). The recommended
// pattern is to drive save from the main thread between frame boundaries.
//
// Loading: `CaptureView` (capture_view.hpp) provides a READ-ONLY view of
// a loaded buffer. Loading does NOT replace the live profiler state --
// the v0g UI will render either a CaptureView or the live profiler
// through the same panel code by using a polymorphic introspection
// surface. Keeping load-replace out of v0f avoids the SPSC ring
// race that would otherwise make a "drop loaded samples onto thread 3"
// path unsafe.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/config.hpp>
#include <crd/perf/counters.hpp>
#include <crd/perf/frame_record.hpp>
#include <crd/perf/memory.hpp>
#include <crd/perf/sample.hpp>

namespace crd::perf
{

// FourCC + version. Bump CprofVersion on any layout change to
// CprofHeader / ThreadHeader / CounterMeta / AllocatorMeta / NameBlob,
// or any change that breaks the meaning of FrameRecord / Sample.
//
// DIAG.6c(b) -- versioning procedure (the "version CPROF without silently changing pinned layouts" contract):
//   * The on-disk layout is pinned field-by-field: sizeof() pins here + offsetof() pins in
//     tests/foundation/perf/test_diag_cprof_version.cpp. A same-size field reorder fails that build, so a layout
//     change CANNOT reach disk silently -- it is a deliberate act that also bumps N below.
//   * The flag-bit trick (an OPTIONAL appended section, version unchanged so old readers still accept new files)
//     works ONLY for a *purely additive* section that needs no room in the fixed structs. It is NOT free forever:
//     the correlation section (6b(b)) already consumed CprofHeader's last spare slot (`correlation_section_offset`,
//     the former `_pad_a`). A SECOND optional section has nowhere to store its offset -> it forces a v2 bump (or a
//     redesign to a section table). So do not assume another additive section is free.
//   * On a bump: raise kCprofVersion to N; `validate_capture_buffer`/`CaptureView` gain a version switch and KEEP a
//     tested reader for N-1 (ADR-0133 ID-1). An offline export always writes the version it produced.
inline constexpr crd::u32 kCprofMagic   = 0x4F525043U; // 'CPRO' little-endian
inline constexpr crd::u32 kCprofVersion = 1U;

// DIAG.6b(b): CprofHeader.flags bits. A bit signals an OPTIONAL appended section; the version stays 1 so old readers
// still accept new files and new readers still accept old files (compatibility with existing captures). Bit0 =
// a correlation side table is present at CprofHeader.correlation_section_offset (see the layout note below).
inline constexpr crd::u64 kCprofFlagCorrelation = 0x1ULL;

// ---- On-disk POD layouts ------------------------------------------------
//
// All structs use explicit sizing so the format is platform-portable
// (modulo endianness, see file header above).

struct CprofHeader
{
    crd::u32 magic;                // 4
    crd::u32 version;              // 4
    crd::u64 flags;                // 8 -- reserved (always 0 in v1)
    crd::u64 captured_at_ns;       // 8 -- MonotonicClock snapshot at save time
    crd::u32 thread_count;         // 4
    crd::u32 counter_count;        // 4
    crd::u32 allocator_count;      // 4
    crd::u32 frame_count;          // 4
    crd::u64 sample_struct_size;   // 8 -- sizeof(Sample) sanity check
    crd::u64 frame_record_size;    // 8 -- sizeof(FrameRecord) sanity check
    crd::u64 name_blob_byte_size;  // 8 -- intern-name table footprint
    crd::u32 name_blob_count;      // 4 -- number of distinct names
    crd::u32 correlation_section_offset; // 4 -- DIAG.6b(b): byte offset of the correlation section, or 0 if none
                                         //      (was _pad_a, always 0 in v1; same bytes, gated by kCprofFlagCorrelation)
};

static_assert(sizeof(CprofHeader) == 72, "CprofHeader is 72 B; on-disk pin");
static_assert(alignof(CprofHeader) == 8, "CprofHeader is 8-aligned");

struct ThreadHeader
{
    crd::u32 thread_index;     // 4 -- index in the live profiler at save time
    crd::u32 sample_count;     // 4
    crd::u64 sample_byte_offset; // 8 -- byte offset of this thread's Sample array
                                 //      relative to the start of the buffer
    char     name[32];           // 32 -- null-padded
    crd::u32 dropped_count;      // 4 -- total samples dropped to overflow
    crd::u32 _pad_a;             // 4
};

static_assert(sizeof(ThreadHeader) == 56, "ThreadHeader is 56 B; on-disk pin");

struct CounterMeta
{
    crd::u32 index;        // 4
    crd::u8  kind;         // 1 -- CounterKind
    crd::u8  type;         // 1 -- CounterType
    crd::u16 _pad_a;       // 2
    char     name[56];     // 56 -- null-padded
};

static_assert(sizeof(CounterMeta) == 64, "CounterMeta is 64 B; on-disk pin");

struct AllocatorMeta
{
    crd::u32 index;        // 4
    crd::u32 _pad_a;       // 4
    char     name[56];     // 56 -- null-padded
};

static_assert(sizeof(AllocatorMeta) == 64, "AllocatorMeta is 64 B; on-disk pin");

// NameBlobEntry is what the in-blob array of u32 offsets points to. The
// blob layout is:
//
//   u32 offsets[count]               -- offset (from start of strings[])
//                                       into the packed string storage
//                                       for name_id == i
//   char strings[name_blob_byte_size - count * sizeof(u32)]
//
// strings[] is packed: each name is followed by a single NUL terminator;
// total bytes = sum of (strlen(name) + 1). offsets[i] is the byte offset
// to the first character of name i.

// ---- Save API ----------------------------------------------------------

// Snapshot the current profiler state into a freshly-allocated buffer.
// The buffer is owned by the caller; `alloc` is used for the buffer
// storage. Returns an empty buffer if the profiler is inactive.
[[nodiscard]] crd::containers::Array<crd::u8>
save_capture_to_buffer(crd::memory::IAllocator* alloc) noexcept;

// Save to disk. Returns true on success (writes a CPROF v1 file).
// Path is opened with stdio; fopen failure returns false.
[[nodiscard]] bool save_capture_to_file(const char* path,
                                        crd::memory::IAllocator* alloc) noexcept;

// DIAG.6c(e): load a CPROF file into a freshly-allocated caller-owned buffer (the inverse of save_capture_to_file;
// `alloc` backs it). Returns an EMPTY buffer on a null path, a file/read error, or (profiling compiled out) the stub.
// The bytes are NOT validated here -- pass the buffer to validate_capture_buffer / CaptureView.
[[nodiscard]] crd::containers::Array<crd::u8>
load_capture_from_file(const char* path, crd::memory::IAllocator* alloc) noexcept;

// (c3) Number of per-thread sample copies a save had to abandon because another consumer (a live UI copy or a
// clear_samples) held the ring across every bounded retry. Such a thread is written with 0 samples -- never stale or
// torn ones -- and this counter makes that rare loss visible rather than silent. Monotonic across saves in a process.
[[nodiscard]] crd::u64 capture_contended_thread_count() noexcept;

// ---- Validate API ------------------------------------------------------

// Validate a buffer's header + size invariants. Returns true iff the
// buffer is a well-formed CPROF v1 file. Cheap; does not parse the
// metadata tables.
[[nodiscard]] bool validate_capture_buffer(
    crd::containers::ConstSpan<crd::u8> buf) noexcept;

} // namespace crd::perf
