#pragma once

// ---------------------------------------------------------------------------
// crd-perf-ui -- CaptureViewSource (Detour D-003 v0g).
//
// Adapts a `crd::perf::CaptureView` to the IProfilerSource interface so
// the same panel code can render a loaded CPROF blob without knowing it
// came from a file.
//
// The view + the underlying buffer must outlive this source.
// ---------------------------------------------------------------------------

#include <crd/perf/capture_view.hpp>
#include <crd/perf/ui/profiler_source.hpp>

namespace crd::perf::ui
{

class CaptureViewSource final : public IProfilerSource
{
public:
    explicit CaptureViewSource(const CaptureView& view, const char* label = "capture") noexcept
        : m_view(&view), m_label(label != nullptr ? label : "capture")
    {
    }

    [[nodiscard]] const char* source_label() const noexcept override { return m_label; }
    [[nodiscard]] bool        is_live() const noexcept override     { return false; }

    [[nodiscard]] crd::u32 thread_count() const noexcept override
    {
        return m_view->thread_count();
    }
    [[nodiscard]] const char* thread_name(crd::u32 idx) const noexcept override
    {
        return m_view->thread_name(idx);
    }
    [[nodiscard]] crd::containers::ConstSpan<Sample>
    thread_samples(crd::u32 idx) const noexcept override
    {
        return m_view->thread_samples(idx);
    }
    [[nodiscard]] crd::u32 thread_dropped(crd::u32 idx) const noexcept override
    {
        return m_view->thread_dropped_count(idx);
    }
    [[nodiscard]] crd::u8 gpu_thread_index() const noexcept override
    {
        // The CPROF format doesn't currently mark a per-thread "is gpu"
        // bit; the convention is that the gpu track is whichever thread
        // is named "gpu". Find it by linear scan (small loop; up to 64).
        for (crd::u32 i = 0U; i < m_view->thread_count(); ++i)
        {
            const char* n = m_view->thread_name(i);
            if (n != nullptr && n[0] == 'g' && n[1] == 'p' && n[2] == 'u' && n[3] == '\0')
            {
                return static_cast<crd::u8>(i);
            }
        }
        return 0xFFU;
    }

    [[nodiscard]] crd::u32 counter_count() const noexcept override
    {
        return m_view->counter_count();
    }
    [[nodiscard]] CounterInfo counter_info(crd::u32 idx) const noexcept override
    {
        return m_view->counter_info(idx);
    }

    [[nodiscard]] crd::u32 allocator_count() const noexcept override
    {
        return m_view->allocator_count();
    }
    [[nodiscard]] AllocatorInfo allocator_info(crd::u32 idx) const noexcept override
    {
        return m_view->allocator_info(idx);
    }

    [[nodiscard]] crd::u32 frame_record_count() const noexcept override
    {
        return m_view->frame_record_count();
    }
    [[nodiscard]] const FrameRecord* frame_record(crd::u32 frames_back) const noexcept override
    {
        const auto recs = m_view->frame_records();
        if (frames_back >= recs.size())
        {
            return nullptr;
        }
        // CPROF stores oldest-first; "frames_back == 0" == latest.
        return &recs[recs.size() - 1U - frames_back];
    }

    [[nodiscard]] const char* resolve_name(NameId id) const noexcept override
    {
        return m_view->resolve_name(id);
    }

private:
    const CaptureView* m_view;
    const char*        m_label;
};

} // namespace crd::perf::ui
