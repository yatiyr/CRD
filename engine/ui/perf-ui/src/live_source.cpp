// ---------------------------------------------------------------------------
// crd-perf-ui -- LiveProfilerSource (D-003 v0g).
// Trivial delegation to the global crd::perf state.
// ---------------------------------------------------------------------------

#include <crd/perf/ui/profiler_source.hpp>

#include <crd/perf/counters.hpp>
#include <crd/perf/gpu_scope.hpp>
#include <crd/perf/memory.hpp>
#include <crd/perf/profiler.hpp>

#include <new>

namespace crd::perf::ui
{

[[nodiscard]] crd::u32 LiveProfilerSource::thread_count() const noexcept
{
    return crd::perf::thread_count();
}

[[nodiscard]] const char* LiveProfilerSource::thread_name(crd::u32 idx) const noexcept
{
    const auto v = crd::perf::thread_samples(static_cast<crd::u8>(idx));
    return v.name != nullptr ? v.name : "";
}

LiveProfilerSource::~LiveProfilerSource()
{
    delete[] m_sample_buf;
    m_sample_buf = nullptr;
    m_sample_cap = 0U;
}

[[nodiscard]] crd::containers::ConstSpan<Sample>
LiveProfilerSource::thread_samples(crd::u32 idx) const noexcept
{
    // DIAG.6a(c3-ui): read through copy_thread_samples -- wrap-correct after a clear_samples() and single-consumer-safe,
    // unlike the base-indexed zero-copy view. One reusable buffer sized to the ring capacity backs the returned span,
    // which is valid only until the next thread_samples() call on this source (see the interface contract). Contention
    // with a concurrent save/clear yields an empty span for that thread this frame -- acceptable for a live view, so we
    // pass nullptr for out_contended and tolerate the skip.
    const crd::u32 cap = crd::perf::per_thread_ring_capacity();
    if (cap == 0U)
    {
        return crd::containers::ConstSpan<Sample>{}; // profiler inactive
    }
    if (m_sample_buf == nullptr || m_sample_cap != cap)
    {
        delete[] m_sample_buf;
        m_sample_buf = new (std::nothrow) Sample[cap];
        m_sample_cap = m_sample_buf != nullptr ? cap : 0U;
        if (m_sample_buf == nullptr)
        {
            return crd::containers::ConstSpan<Sample>{}; // allocation failed: render empty rather than crash
        }
    }
    // m_sample_cap == per_thread_ring_capacity() (kept in sync by the realloc above), so the copy's max_samples equals
    // the ring's slot count and is NEVER truncated; a stale cap would silently drop the newest samples.
    const crd::u32 n = crd::perf::copy_thread_samples(static_cast<crd::u8>(idx), m_sample_buf, m_sample_cap);
    if (n == 0U)
    {
        return crd::containers::ConstSpan<Sample>{};
    }
    return crd::containers::ConstSpan<Sample>{m_sample_buf, n};
}

[[nodiscard]] crd::u32 LiveProfilerSource::thread_dropped(crd::u32 idx) const noexcept
{
    return crd::perf::thread_samples(static_cast<crd::u8>(idx)).dropped;
}

[[nodiscard]] crd::u8 LiveProfilerSource::gpu_thread_index() const noexcept
{
    return crd::perf::gpu_thread_index();
}

[[nodiscard]] crd::u32 LiveProfilerSource::counter_count() const noexcept
{
    return crd::perf::counter_count();
}

[[nodiscard]] CounterInfo LiveProfilerSource::counter_info(crd::u32 idx) const noexcept
{
    return crd::perf::counter_info(CounterId{idx});
}

[[nodiscard]] crd::u32 LiveProfilerSource::allocator_count() const noexcept
{
    return crd::perf::registered_allocator_count();
}

[[nodiscard]] AllocatorInfo LiveProfilerSource::allocator_info(crd::u32 idx) const noexcept
{
    return crd::perf::allocator_info(idx);
}

[[nodiscard]] crd::u32 LiveProfilerSource::frame_record_count() const noexcept
{
    return crd::perf::frame_record_count();
}

[[nodiscard]] const FrameRecord* LiveProfilerSource::frame_record(crd::u32 frames_back) const noexcept
{
    return crd::perf::frame_record(frames_back);
}

[[nodiscard]] const char* LiveProfilerSource::resolve_name(NameId id) const noexcept
{
    return crd::perf::resolve_name(id);
}

} // namespace crd::perf::ui
