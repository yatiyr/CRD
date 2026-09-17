// ---------------------------------------------------------------------------
// crd-perf -- CaptureView read-only access (Detour D-003 v0f).
// ---------------------------------------------------------------------------

#include <crd/perf/capture_view.hpp>

#include <crd/perf/capture.hpp>

#include <cstring>

namespace crd::perf
{

#if CRD_PERF_ENABLED

namespace
{
constexpr crd::u32 kEmptyNameOffset = 0xFFFF'FFFFU;
} // namespace

CaptureView::CaptureView(crd::containers::ConstSpan<crd::u8> buf) noexcept
    : m_buf(buf)
{
    if (!validate_capture_buffer(buf))
    {
        return;
    }
    const auto* hdr = reinterpret_cast<const CprofHeader*>(buf.data());

    crd::usize cursor = sizeof(CprofHeader);

    m_off_thread_headers = cursor;
    cursor += static_cast<crd::usize>(hdr->thread_count) * sizeof(ThreadHeader);

    m_off_counter_meta = cursor;
    cursor += static_cast<crd::usize>(hdr->counter_count) * sizeof(CounterMeta);

    m_off_allocator_meta = cursor;
    cursor += static_cast<crd::usize>(hdr->allocator_count) * sizeof(AllocatorMeta);

    m_off_name_blob = cursor;
    m_name_blob_bytes = hdr->name_blob_byte_size;
    cursor += static_cast<crd::usize>(hdr->name_blob_byte_size);

    m_off_frame_records = cursor;

    // Parse the in-blob header (u32 capacity + u32 string_bytes).
    if (m_off_name_blob + 8U > buf.size())
    {
        return;
    }
    m_name_count = *reinterpret_cast<const crd::u32*>(buf.data() + m_off_name_blob);

    // DIAG.6b(b): locate the optional correlation section. validate_capture_buffer (called above) has already
    // strictly bounds-checked it when the flag is set; re-derive the record offset/count from the header here.
    if ((hdr->flags & kCprofFlagCorrelation) != 0U)
    {
        const crd::u64 off = hdr->correlation_section_offset;
        if ((off & 7U) == 0U && off + 8U <= buf.size())
        {
            const crd::u32 count    = *reinterpret_cast<const crd::u32*>(buf.data() + off);
            const crd::u32 rec_size = *reinterpret_cast<const crd::u32*>(buf.data() + off + sizeof(crd::u32));
            if (rec_size == sizeof(CorrelationRecord)
                && off + 8U + static_cast<crd::u64>(count) * rec_size <= buf.size())
            {
                m_off_correlation   = static_cast<crd::usize>(off + 8U);
                m_correlation_count = count;
            }
        }
    }

    m_valid = true;
}

[[nodiscard]] const CprofHeader* CaptureView::header() const noexcept
{
    return m_valid ? reinterpret_cast<const CprofHeader*>(m_buf.data()) : nullptr;
}

[[nodiscard]] const ThreadHeader* CaptureView::thread_header(crd::u32 i) const noexcept
{
    if (!m_valid)
    {
        return nullptr;
    }
    if (i >= header()->thread_count)
    {
        return nullptr;
    }
    return reinterpret_cast<const ThreadHeader*>(m_buf.data() + m_off_thread_headers) + i;
}

[[nodiscard]] const CounterMeta* CaptureView::counter_meta_at(crd::u32 i) const noexcept
{
    if (!m_valid || i >= header()->counter_count)
    {
        return nullptr;
    }
    return reinterpret_cast<const CounterMeta*>(m_buf.data() + m_off_counter_meta) + i;
}

[[nodiscard]] const AllocatorMeta* CaptureView::allocator_meta_at(crd::u32 i) const noexcept
{
    if (!m_valid || i >= header()->allocator_count)
    {
        return nullptr;
    }
    return reinterpret_cast<const AllocatorMeta*>(m_buf.data() + m_off_allocator_meta) + i;
}

[[nodiscard]] crd::u32 CaptureView::thread_count() const noexcept
{
    return m_valid ? header()->thread_count : 0U;
}

[[nodiscard]] crd::u32 CaptureView::counter_count() const noexcept
{
    return m_valid ? header()->counter_count : 0U;
}

[[nodiscard]] crd::u32 CaptureView::allocator_count() const noexcept
{
    return m_valid ? header()->allocator_count : 0U;
}

[[nodiscard]] crd::u32 CaptureView::frame_record_count() const noexcept
{
    return m_valid ? header()->frame_count : 0U;
}

[[nodiscard]] crd::u64 CaptureView::captured_at_ns() const noexcept
{
    return m_valid ? header()->captured_at_ns : 0ULL;
}

[[nodiscard]] const char* CaptureView::thread_name(crd::u32 i) const noexcept
{
    const auto* th = thread_header(i);
    return th != nullptr ? th->name : "";
}

[[nodiscard]] crd::u32 CaptureView::thread_sample_count(crd::u32 i) const noexcept
{
    const auto* th = thread_header(i);
    return th != nullptr ? th->sample_count : 0U;
}

[[nodiscard]] crd::u32 CaptureView::thread_dropped_count(crd::u32 i) const noexcept
{
    const auto* th = thread_header(i);
    return th != nullptr ? th->dropped_count : 0U;
}

[[nodiscard]] crd::containers::ConstSpan<Sample>
CaptureView::thread_samples(crd::u32 i) const noexcept
{
    const auto* th = thread_header(i);
    if (th == nullptr || th->sample_count == 0U)
    {
        return crd::containers::ConstSpan<Sample>{};
    }
    const auto* base = reinterpret_cast<const Sample*>(m_buf.data() + th->sample_byte_offset);
    return crd::containers::ConstSpan<Sample>{base, th->sample_count};
}

[[nodiscard]] CounterInfo CaptureView::counter_info(crd::u32 i) const noexcept
{
    CounterInfo info{};
    const auto* m = counter_meta_at(i);
    if (m != nullptr)
    {
        info.name = m->name;
        info.kind = static_cast<CounterKind>(m->kind);
        info.type = static_cast<CounterType>(m->type);
    }
    return info;
}

[[nodiscard]] AllocatorInfo CaptureView::allocator_info(crd::u32 i) const noexcept
{
    AllocatorInfo info{};
    const auto* m = allocator_meta_at(i);
    if (m != nullptr)
    {
        info.name = m->name;
        info.allocator = nullptr; // loaded captures don't carry live pointers
    }
    return info;
}

[[nodiscard]] crd::containers::ConstSpan<FrameRecord>
CaptureView::frame_records() const noexcept
{
    if (!m_valid)
    {
        return crd::containers::ConstSpan<FrameRecord>{};
    }
    const auto* base = reinterpret_cast<const FrameRecord*>(m_buf.data() + m_off_frame_records);
    return crd::containers::ConstSpan<FrameRecord>{base, header()->frame_count};
}

[[nodiscard]] const char*
CaptureView::frame_allocator_name(crd::u32 frame_index, crd::u32 allocator_idx) const noexcept
{
    if (!m_valid || allocator_idx >= kMaxAllocators)
    {
        return "";
    }
    const auto frames = frame_records();
    if (frame_index >= frames.size())
    {
        return "";
    }
    // DIAG.6a(d2): prefer the identity STAMPED into this record -- correct even after the slot was unregistered and
    // reused -- and fall back to the live-at-save slot name for pre-(d2) records (_pad == 0).
    const NameId nid = allocator_record_name_id(frames[frame_index].allocators[allocator_idx]);
    if (nid.is_valid())
    {
        return resolve_name(nid);
    }
    return allocator_info(allocator_idx).name;
}

[[nodiscard]] const char* CaptureView::resolve_name(NameId id) const noexcept
{
    if (!m_valid || !id.is_valid() || id.value >= m_name_count)
    {
        return "";
    }
    // Layout: [u32 capacity][u32 string_bytes][u32 offsets[capacity]][char strings[string_bytes]]
    const crd::u8* blob = m_buf.data() + m_off_name_blob;
    const auto*    offsets = reinterpret_cast<const crd::u32*>(blob + 8U);
    const crd::u32 offset  = offsets[id.value];
    if (offset == kEmptyNameOffset)
    {
        return "";
    }
    const auto* strings_base = reinterpret_cast<const char*>(
        blob + 8U + static_cast<crd::u64>(m_name_count) * sizeof(crd::u32));
    return strings_base + offset;
}

[[nodiscard]] crd::u32 CaptureView::correlation_count() const noexcept
{
    return m_valid ? m_correlation_count : 0U;
}

[[nodiscard]] const CorrelationRecord* CaptureView::correlation_at(crd::u32 i) const noexcept
{
    if (!m_valid || i >= m_correlation_count)
    {
        return nullptr;
    }
    return reinterpret_cast<const CorrelationRecord*>(m_buf.data() + m_off_correlation) + i;
}

[[nodiscard]] const CorrelationRecord*
CaptureView::correlation_for(crd::u32 thread_index, crd::u32 sample_ordinal) const noexcept
{
    if (!m_valid || m_correlation_count == 0U)
    {
        return nullptr;
    }
    // Records are written sorted by (thread_index, sample_ordinal); binary-search the composite key.
    const auto*    base = reinterpret_cast<const CorrelationRecord*>(m_buf.data() + m_off_correlation);
    const crd::u64 key  = (static_cast<crd::u64>(thread_index) << 32) | sample_ordinal;
    crd::u32       lo   = 0U;
    crd::u32       hi   = m_correlation_count;
    while (lo < hi)
    {
        const crd::u32 mid = lo + (hi - lo) / 2U;
        const crd::u64 mk  = (static_cast<crd::u64>(base[mid].thread_index) << 32) | base[mid].sample_ordinal;
        if (mk == key)
        {
            return base + mid;
        }
        if (mk < key)
        {
            lo = mid + 1U;
        }
        else
        {
            hi = mid;
        }
    }
    return nullptr;
}

#else // CRD_PERF_ENABLED == 0

CaptureView::CaptureView(crd::containers::ConstSpan<crd::u8>) noexcept {}

[[nodiscard]] const CprofHeader*    CaptureView::header() const noexcept { return nullptr; }
[[nodiscard]] const ThreadHeader*   CaptureView::thread_header(crd::u32) const noexcept { return nullptr; }
[[nodiscard]] const CounterMeta*    CaptureView::counter_meta_at(crd::u32) const noexcept { return nullptr; }
[[nodiscard]] const AllocatorMeta*  CaptureView::allocator_meta_at(crd::u32) const noexcept { return nullptr; }

[[nodiscard]] crd::u32 CaptureView::thread_count() const noexcept { return 0U; }
[[nodiscard]] crd::u32 CaptureView::counter_count() const noexcept { return 0U; }
[[nodiscard]] crd::u32 CaptureView::allocator_count() const noexcept { return 0U; }
[[nodiscard]] crd::u32 CaptureView::frame_record_count() const noexcept { return 0U; }
[[nodiscard]] crd::u64 CaptureView::captured_at_ns() const noexcept { return 0U; }
[[nodiscard]] const char* CaptureView::thread_name(crd::u32) const noexcept { return ""; }
[[nodiscard]] crd::u32 CaptureView::thread_sample_count(crd::u32) const noexcept { return 0U; }
[[nodiscard]] crd::u32 CaptureView::thread_dropped_count(crd::u32) const noexcept { return 0U; }
[[nodiscard]] crd::containers::ConstSpan<Sample> CaptureView::thread_samples(crd::u32) const noexcept
{
    return {};
}
[[nodiscard]] CounterInfo CaptureView::counter_info(crd::u32) const noexcept { return {}; }
[[nodiscard]] AllocatorInfo CaptureView::allocator_info(crd::u32) const noexcept { return {}; }
[[nodiscard]] crd::containers::ConstSpan<FrameRecord> CaptureView::frame_records() const noexcept
{
    return {};
}
[[nodiscard]] const char* CaptureView::frame_allocator_name(crd::u32, crd::u32) const noexcept { return ""; }
[[nodiscard]] const char* CaptureView::resolve_name(NameId) const noexcept { return ""; }
[[nodiscard]] crd::u32 CaptureView::correlation_count() const noexcept { return 0U; }
[[nodiscard]] const CorrelationRecord* CaptureView::correlation_at(crd::u32) const noexcept { return nullptr; }
[[nodiscard]] const CorrelationRecord* CaptureView::correlation_for(crd::u32, crd::u32) const noexcept
{
    return nullptr;
}

#endif

} // namespace crd::perf
