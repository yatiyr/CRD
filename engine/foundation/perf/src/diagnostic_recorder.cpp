// crd-perf -- lifecycle-safe diagnostic recording + emergency record.
// See diagnostic_recorder.hpp.

#include <crd/perf/diagnostic_recorder.hpp>

#include <atomic>
#include <cstring>
#include <mutex>

namespace crd::perf
{
namespace
{
// Bounded copy of a view into a fixed char buffer, always NUL-terminated.
void copy_bounded(char* dst, crd::usize dst_cap, cont::StringView src) noexcept
{
    if (dst_cap == 0U)
        return;
    const crd::usize n = (src.size() < dst_cap - 1U) ? src.size() : dst_cap - 1U;
    if (n > 0U)
        std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

struct ReaderSlot
{
    bool     active     = false;
    crd::u32 generation = 0U;
};
} // namespace

struct DiagnosticRecorder::Impl
{
    // Ring: preallocated to `cap`; record() overwrites the oldest when full.
    mutable std::mutex          ring_mutex;
    cont::Array<DiagnosticEvent> ring;
    crd::u32                     cap   = 0U;
    crd::u32                     head  = 0U; // next write index
    crd::u32                     live  = 0U; // live records (<= cap)
    crd::u64                     total = 0U; // monotonic total recorded

    // Reader table: bounded, generation-tagged so a stale token cannot retire a reused slot.
    mutable std::mutex      reader_mutex;
    cont::Array<ReaderSlot> readers;
    crd::u32                reader_live = 0U;

    // Emergency record: its OWN lock, independent of ring_mutex.
    mutable std::mutex emergency_mutex;
    EmergencyRecord    emergency;
};

bool DiagnosticRecorder::init(crd::u32 capacity) noexcept
{
    if (m_impl != nullptr || capacity == 0U || capacity > kMaxPolicyEvents)
        return false;

    m_impl = new (std::nothrow) Impl();
    if (m_impl == nullptr)
        return false;

    m_impl->cap = capacity;
    m_impl->ring.reserve(capacity);
    for (crd::u32 i = 0; i < capacity; ++i)
        m_impl->ring.emplace_back(); // in-place default ctor (DiagnosticEvent{} would need String's explicit default ctor)

    static constexpr crd::u32 kMaxReaders = 256U;
    m_impl->readers.reserve(kMaxReaders);
    for (crd::u32 i = 0; i < kMaxReaders; ++i)
        m_impl->readers.emplace_back();

    return true;
}

void DiagnosticRecorder::shutdown() noexcept
{
    delete m_impl;
    m_impl = nullptr;
}

bool DiagnosticRecorder::is_initialized() const noexcept
{
    return m_impl != nullptr;
}

void DiagnosticRecorder::record(const DiagnosticEvent& e)
{
    Impl* impl = m_impl;
    if (impl == nullptr) // early-init / late-shutdown: a no-op, never a fault
        return;

    std::lock_guard<std::mutex> guard(impl->ring_mutex);
    impl->ring[impl->head] = e; // String move/copy-assign; overwrites the oldest slot
    impl->head             = (impl->head + 1U) % impl->cap;
    if (impl->live < impl->cap)
        ++impl->live;
    ++impl->total;
}

void DiagnosticRecorder::snapshot(cont::Array<DiagnosticEvent>& out) const
{
    out.clear();
    Impl* impl = m_impl;
    if (impl == nullptr)
        return;

    std::lock_guard<std::mutex> guard(impl->ring_mutex);
    out.reserve(impl->live);
    // Oldest first. When full, the oldest is at `head`; otherwise records start at 0.
    const crd::u32 start = (impl->live == impl->cap) ? impl->head : 0U;
    for (crd::u32 i = 0; i < impl->live; ++i)
    {
        const crd::u32 idx = (start + i) % impl->cap;
        out.push_back(impl->ring[idx]);
    }
}

crd::u32 DiagnosticRecorder::count() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
        return 0U;
    std::lock_guard<std::mutex> guard(impl->ring_mutex);
    return impl->live;
}

crd::u64 DiagnosticRecorder::total_recorded() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
        return 0U;
    std::lock_guard<std::mutex> guard(impl->ring_mutex);
    return impl->total;
}

crd::u32 DiagnosticRecorder::capacity() const noexcept
{
    Impl* impl = m_impl;
    return (impl != nullptr) ? impl->cap : 0U;
}

ReaderToken DiagnosticRecorder::register_reader() noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
        return ReaderToken{};

    std::lock_guard<std::mutex> guard(impl->reader_mutex);
    for (crd::u32 i = 0; i < impl->readers.size(); ++i)
    {
        if (!impl->readers[i].active)
        {
            impl->readers[i].active = true;
            ++impl->reader_live;
            return ReaderToken{i, impl->readers[i].generation};
        }
    }
    return ReaderToken{}; // table full
}

bool DiagnosticRecorder::deregister_reader(ReaderToken token) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr || !token.valid())
        return false;

    std::lock_guard<std::mutex> guard(impl->reader_mutex);
    if (token.slot >= impl->readers.size())
        return false;

    ReaderSlot& slot = impl->readers[token.slot];
    // Generation-checked retirement: a stale token whose slot was reused is rejected.
    if (!slot.active || slot.generation != token.generation)
        return false;

    slot.active = false;
    ++slot.generation; // retire: any outstanding copy of this token is now stale
    --impl->reader_live;
    return true;
}

crd::u32 DiagnosticRecorder::reader_count() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
        return 0U;
    std::lock_guard<std::mutex> guard(impl->reader_mutex);
    return impl->reader_live;
}

void DiagnosticRecorder::capture_emergency(const DiagnosticEvent& e) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
        return;

    // Deliberately NOT ring_mutex: the emergency record must be writable even while a thread
    // holds the ring/log lock (the "crash while the log mutex is held" case).
    std::lock_guard<std::mutex> guard(impl->emergency_mutex);
    EmergencyRecord&            r = impl->emergency;
    r.present      = true;
    r.code         = e.code;
    r.severity     = e.severity;
    r.timestamp_ns = e.timestamp_ns;
    r.line         = e.source.line;
    copy_bounded(r.file, sizeof(r.file), e.source.file);
    copy_bounded(r.message, sizeof(r.message), cont::StringView{e.message.c_str(), e.message.size()});
}

EmergencyRecord DiagnosticRecorder::emergency() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
        return EmergencyRecord{};
    std::lock_guard<std::mutex> guard(impl->emergency_mutex);
    return impl->emergency;
}

} // namespace crd::perf
