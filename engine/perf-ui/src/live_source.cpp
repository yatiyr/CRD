// ---------------------------------------------------------------------------
// crd-perf-ui -- LiveProfilerSource (D-003 v0g).
// Trivial delegation to the global crd::perf state.
// ---------------------------------------------------------------------------

#include <crd/perf/ui/profiler_source.hpp>

#include <crd/perf/counters.hpp>
#include <crd/perf/gpu_scope.hpp>
#include <crd/perf/memory.hpp>
#include <crd/perf/profiler.hpp>

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

[[nodiscard]] crd::containers::ConstSpan<Sample>
LiveProfilerSource::thread_samples(crd::u32 idx) const noexcept
{
    const auto v = crd::perf::thread_samples(static_cast<crd::u8>(idx));
    if (v.data == nullptr || v.size == 0U)
    {
        return crd::containers::ConstSpan<Sample>{};
    }
    return crd::containers::ConstSpan<Sample>{v.data, v.size};
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
