// ---------------------------------------------------------------------------
// crd-perf -- CPROF v1 save (Detour D-003 v0f).
//
// Layout (little-endian only; offsets all 8-aligned to keep the reader
// simple):
//
//   [CprofHeader]                                72 B
//   [ThreadHeader] x thread_count                56 B each
//   [CounterMeta]  x counter_count               64 B each
//   [AllocatorMeta] x allocator_count            64 B each
//   [NameBlob]                                   sized; see below
//     u32 capacity      -- intern table size at save
//     u32 string_bytes  -- packed string storage size
//     u32 offsets[capacity]  -- offset into strings[] per NameId; 0xFFFFFFFF = empty
//     char strings[string_bytes]
//     (8-byte padding to keep the next section aligned)
//   [FrameRecord] x frame_count                  3616 B each
//   [Sample]      x sum(thread sample counts)    32 B each
//
// Save policy: at save time we snapshot each thread's ring under an
// acquire-load of head/tail (the same pattern the v0a `thread_samples`
// accessor uses). Concurrent push_region during save may or may not be
// captured -- well-defined; not guaranteed. The recommended pattern is
// to drive save from the main thread between frame boundaries.
// ---------------------------------------------------------------------------

#include <crd/perf/capture.hpp>

#include <crd/core/assert.hpp>
#include <crd/perf/profiler.hpp>
#include <crd/time/clocks.hpp>

#include <cstdio>
#include <cstring>
#include <new>

namespace crd::perf
{

#if CRD_PERF_ENABLED

namespace
{

constexpr crd::u32 kEmptyNameOffset = 0xFFFF'FFFFU;

// Align an offset upward to an 8-byte boundary.
[[nodiscard]] constexpr crd::usize align_up_8(crd::usize n) noexcept
{
    return (n + 7U) & ~static_cast<crd::usize>(7U);
}

// Compute the byte size of the name blob section for the current intern
// table. Returns: total_bytes, string_bytes (== sum of strlen+1 over
// filled slots).
struct NameBlobSizing
{
    crd::u64 total_bytes;   // including the 8 B blob-header + offsets array + strings + padding
    crd::u32 capacity;
    crd::u32 string_bytes;
};

[[nodiscard]] NameBlobSizing measure_name_blob() noexcept
{
    NameBlobSizing s{};
    s.capacity = intern_name_capacity();
    crd::u64 string_bytes = 0U;
    for (crd::u32 i = 0U; i < s.capacity; ++i)
    {
        const char* name = resolve_name(NameId{i});
        if (name != nullptr && name[0] != '\0')
        {
            string_bytes += std::strlen(name) + 1U; // include NUL
        }
    }
    // Some slots may be empty + return ""; treat as zero string bytes.
    s.string_bytes = static_cast<crd::u32>(string_bytes);

    const crd::u64 hdr_bytes      = 8U; // u32 capacity + u32 string_bytes
    const crd::u64 offsets_bytes  = static_cast<crd::u64>(s.capacity) * sizeof(crd::u32);
    const crd::u64 unaligned      = hdr_bytes + offsets_bytes + string_bytes;
    s.total_bytes                 = align_up_8(unaligned);
    return s;
}

// Stamp a string into a fixed-size null-padded char buffer.
void copy_to_fixed(char* dst, crd::usize dst_size, const char* src) noexcept
{
    if (src == nullptr)
    {
        std::memset(dst, 0, dst_size);
        return;
    }
    const crd::usize n = std::strlen(src);
    const crd::usize copy_n = n < dst_size - 1U ? n : dst_size - 1U;
    std::memcpy(dst, src, copy_n);
    std::memset(dst + copy_n, 0, dst_size - copy_n);
}

// Append a snapshot of one thread's ring into `out_samples`. Returns the
// number of samples actually written + the dropped count at snapshot time.
struct ThreadSnap
{
    crd::u32 thread_index;
    crd::u32 sample_count;
    crd::u32 dropped_count;
    const char* name;
    crd::u64 byte_offset;     // filled in by caller after layout pass
};

} // namespace

[[nodiscard]] crd::containers::Array<crd::u8>
save_capture_to_buffer(crd::memory::IAllocator* alloc) noexcept
{
    crd::containers::Array<crd::u8> out{alloc};
    if (!is_active() || alloc == nullptr)
    {
        return out;
    }

    // ----- Pass 1: measure sizes ------------------------------------
    const crd::u32 thread_n    = thread_count();
    const crd::u32 counter_n   = counter_count();
    const crd::u32 alloc_n     = registered_allocator_count();
    const crd::u32 frame_n     = frame_record_count();

    // Snapshot every thread's sample-count up front (live writers can
    // append after this; the snapshot we copy below uses the same
    // acquire-load and may include a few more, but the size estimate
    // bounds the buffer).
    crd::containers::Array<ThreadSnap> threads{alloc};
    threads.reserve(thread_n);
    for (crd::u32 i = 0U; i < thread_n; ++i)
    {
        const auto view = thread_samples(static_cast<crd::u8>(i));
        ThreadSnap s{};
        s.thread_index  = i;
        s.sample_count  = view.size;
        s.dropped_count = view.dropped;
        s.name          = view.name != nullptr ? view.name : "";
        threads.push_back(s);
    }

    const NameBlobSizing name_blob = measure_name_blob();

    // Total bytes upfront.
    const crd::u64 hdr_bytes        = sizeof(CprofHeader);
    const crd::u64 thread_hdr_bytes = static_cast<crd::u64>(thread_n) * sizeof(ThreadHeader);
    const crd::u64 counter_meta_bytes = static_cast<crd::u64>(counter_n) * sizeof(CounterMeta);
    const crd::u64 alloc_meta_bytes   = static_cast<crd::u64>(alloc_n) * sizeof(AllocatorMeta);
    const crd::u64 frame_bytes        = static_cast<crd::u64>(frame_n) * sizeof(FrameRecord);

    crd::u64 total_sample_bytes = 0U;
    for (const auto& t : threads)
    {
        total_sample_bytes += static_cast<crd::u64>(t.sample_count) * sizeof(Sample);
    }

    const crd::u64 total_bytes = hdr_bytes + thread_hdr_bytes + counter_meta_bytes
                                  + alloc_meta_bytes + name_blob.total_bytes
                                  + frame_bytes + total_sample_bytes;

    out.resize(static_cast<crd::usize>(total_bytes));
    std::memset(out.data(), 0, out.size());

    // ----- Pass 2: write the header (placeholder; fill sample offsets after) -----
    crd::usize cursor = 0U;

    auto* hdr             = reinterpret_cast<CprofHeader*>(out.data() + cursor);
    hdr->magic            = kCprofMagic;
    hdr->version          = kCprofVersion;
    hdr->flags            = 0U;
    hdr->captured_at_ns   = static_cast<crd::u64>(
        crd::time::MonotonicClock::now().ns_since_epoch());
    hdr->thread_count     = thread_n;
    hdr->counter_count    = counter_n;
    hdr->allocator_count  = alloc_n;
    hdr->frame_count      = frame_n;
    hdr->sample_struct_size  = sizeof(Sample);
    hdr->frame_record_size   = sizeof(FrameRecord);
    hdr->name_blob_byte_size = name_blob.total_bytes;
    hdr->name_blob_count     = name_blob.capacity;
    hdr->_pad_a              = 0U;
    cursor += hdr_bytes;

    // ----- Write thread headers (sample_byte_offset patched below) -----
    const crd::usize thread_hdr_start = cursor;
    cursor += thread_hdr_bytes;

    // ----- Write counter metadata -----
    auto* counter_meta = reinterpret_cast<CounterMeta*>(out.data() + cursor);
    for (crd::u32 i = 0U; i < counter_n; ++i)
    {
        const auto info = counter_info(CounterId{i});
        counter_meta[i].index  = i;
        counter_meta[i].kind   = static_cast<crd::u8>(info.kind);
        counter_meta[i].type   = static_cast<crd::u8>(info.type);
        counter_meta[i]._pad_a = 0U;
        copy_to_fixed(counter_meta[i].name, sizeof(counter_meta[i].name), info.name);
    }
    cursor += counter_meta_bytes;

    // ----- Write allocator metadata -----
    auto* alloc_meta = reinterpret_cast<AllocatorMeta*>(out.data() + cursor);
    for (crd::u32 i = 0U; i < alloc_n; ++i)
    {
        const auto info = allocator_info(i);
        alloc_meta[i].index  = i;
        alloc_meta[i]._pad_a = 0U;
        copy_to_fixed(alloc_meta[i].name, sizeof(alloc_meta[i].name), info.name);
    }
    cursor += alloc_meta_bytes;

    // ----- Write name blob -----
    [[maybe_unused]] const crd::usize name_blob_start = cursor;
    auto* nb_capacity     = reinterpret_cast<crd::u32*>(out.data() + cursor);
    *nb_capacity          = name_blob.capacity;
    cursor += sizeof(crd::u32);
    auto* nb_string_bytes = reinterpret_cast<crd::u32*>(out.data() + cursor);
    *nb_string_bytes      = name_blob.string_bytes;
    cursor += sizeof(crd::u32);

    auto*           offsets       = reinterpret_cast<crd::u32*>(out.data() + cursor);
    const crd::usize offsets_byte_size = name_blob.capacity * sizeof(crd::u32);
    cursor += offsets_byte_size;

    char*           strings = reinterpret_cast<char*>(out.data() + cursor);
    crd::u32        sb_cursor = 0U;
    for (crd::u32 i = 0U; i < name_blob.capacity; ++i)
    {
        const char* name = resolve_name(NameId{i});
        if (name != nullptr && name[0] != '\0')
        {
            const crd::u32 n = static_cast<crd::u32>(std::strlen(name));
            offsets[i] = sb_cursor;
            std::memcpy(strings + sb_cursor, name, n);
            strings[sb_cursor + n] = '\0';
            sb_cursor += n + 1U;
        }
        else
        {
            offsets[i] = kEmptyNameOffset;
        }
    }
    cursor += name_blob.string_bytes;

    // Align to 8 before frame records.
    while ((cursor & 7U) != 0U)
    {
        out[cursor++] = 0;
    }
    CRD_ASSERT_MSG(cursor == name_blob_start + name_blob.total_bytes,
                   "save_capture: name-blob cursor diverged from precomputed size");

    // ----- Write frame records -----
    auto* frame_dst = reinterpret_cast<FrameRecord*>(out.data() + cursor);
    for (crd::u32 i = 0U; i < frame_n; ++i)
    {
        const FrameRecord* rec = frame_record(frame_n - 1U - i); // oldest first
        if (rec == nullptr)
        {
            std::memset(&frame_dst[i], 0, sizeof(FrameRecord));
        }
        else
        {
            std::memcpy(&frame_dst[i], rec, sizeof(FrameRecord));
        }
    }
    cursor += frame_bytes;

    // ----- Write per-thread sample arrays + patch ThreadHeader offsets -----
    auto* thread_hdrs = reinterpret_cast<ThreadHeader*>(out.data() + thread_hdr_start);
    for (crd::u32 i = 0U; i < thread_n; ++i)
    {
        const auto&   t           = threads[i];
        ThreadHeader& th          = thread_hdrs[i];
        th.thread_index           = t.thread_index;
        th.sample_count           = t.sample_count;
        th.sample_byte_offset     = cursor;
        th.dropped_count          = t.dropped_count;
        th._pad_a                 = 0U;
        copy_to_fixed(th.name, sizeof(th.name), t.name);

        // Acquire-snapshot the ring contents again. The size we read here
        // is the saved size at snapshot time -- may differ slightly from
        // the pre-pass; cap to the smaller value to keep the buffer
        // within bounds.
        const auto view = thread_samples(static_cast<crd::u8>(t.thread_index));
        const crd::u32 to_copy = view.size < t.sample_count ? view.size : t.sample_count;

        // Ring sample addresses are not contiguous in the source ring
        // (head/tail wrap), but `view.data` returns the raw underlying
        // buffer + size = head-tail. For v0f we copy whatever
        // thread_samples returned -- this means the oldest-first order
        // matches the live profiler's ordering. Future versions may
        // reorder for capture stability.
        const crd::u64 copy_bytes = static_cast<crd::u64>(to_copy) * sizeof(Sample);
        if (copy_bytes > 0U)
        {
            std::memcpy(out.data() + cursor, view.data, copy_bytes);
        }
        // If the live ring was shorter at copy time, zero the remainder
        // so the file size matches the pre-pass total.
        const crd::u64 leftover_bytes =
            static_cast<crd::u64>(t.sample_count - to_copy) * sizeof(Sample);
        if (leftover_bytes > 0U)
        {
            std::memset(out.data() + cursor + copy_bytes, 0,
                        static_cast<crd::usize>(leftover_bytes));
        }
        cursor += static_cast<crd::usize>(t.sample_count) * sizeof(Sample);
    }

    CRD_ASSERT_MSG(cursor == total_bytes,
                   "save_capture: cursor diverged from precomputed total");
    return out;
}

[[nodiscard]] bool save_capture_to_file(const char* path,
                                        crd::memory::IAllocator* alloc) noexcept
{
    if (path == nullptr || alloc == nullptr)
    {
        return false;
    }
    auto buf = save_capture_to_buffer(alloc);
    if (buf.size() == 0U)
    {
        return false;
    }
    std::FILE* fp = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&fp, path, "wb") != 0)
    {
        return false;
    }
#else
    fp = std::fopen(path, "wb");
#endif
    if (fp == nullptr)
    {
        return false;
    }
    const auto written = std::fwrite(buf.data(), 1U, buf.size(), fp);
    std::fclose(fp);
    return written == buf.size();
}

[[nodiscard]] bool validate_capture_buffer(
    crd::containers::ConstSpan<crd::u8> buf) noexcept
{
    if (buf.size() < sizeof(CprofHeader))
    {
        return false;
    }
    const auto* hdr = reinterpret_cast<const CprofHeader*>(buf.data());
    if (hdr->magic != kCprofMagic)
    {
        return false;
    }
    if (hdr->version != kCprofVersion)
    {
        return false;
    }
    if (hdr->sample_struct_size != sizeof(Sample))
    {
        return false;
    }
    if (hdr->frame_record_size != sizeof(FrameRecord))
    {
        return false;
    }
    // Verify the buffer is large enough to hold every section.
    const crd::u64 min_bytes =
        sizeof(CprofHeader)
        + static_cast<crd::u64>(hdr->thread_count) * sizeof(ThreadHeader)
        + static_cast<crd::u64>(hdr->counter_count) * sizeof(CounterMeta)
        + static_cast<crd::u64>(hdr->allocator_count) * sizeof(AllocatorMeta)
        + hdr->name_blob_byte_size
        + static_cast<crd::u64>(hdr->frame_count) * sizeof(FrameRecord);
    if (buf.size() < min_bytes)
    {
        return false;
    }
    return true;
}

#else // CRD_PERF_ENABLED == 0

[[nodiscard]] crd::containers::Array<crd::u8>
save_capture_to_buffer(crd::memory::IAllocator* alloc) noexcept
{
    return crd::containers::Array<crd::u8>{alloc};
}

[[nodiscard]] bool save_capture_to_file(const char*, crd::memory::IAllocator*) noexcept
{
    return false;
}

[[nodiscard]] bool validate_capture_buffer(
    crd::containers::ConstSpan<crd::u8>) noexcept
{
    return false;
}

#endif

} // namespace crd::perf
