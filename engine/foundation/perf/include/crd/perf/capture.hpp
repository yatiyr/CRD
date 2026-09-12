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
inline constexpr crd::u32 kCprofMagic   = 0x4F525043U; // 'CPRO' little-endian
inline constexpr crd::u32 kCprofVersion = 1U;

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
    crd::u32 _pad_a;               // 4
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

// ---- Validate API ------------------------------------------------------

// Validate a buffer's header + size invariants. Returns true iff the
// buffer is a well-formed CPROF v1 file. Cheap; does not parse the
// metadata tables.
[[nodiscard]] bool validate_capture_buffer(
    crd::containers::ConstSpan<crd::u8> buf) noexcept;

} // namespace crd::perf
