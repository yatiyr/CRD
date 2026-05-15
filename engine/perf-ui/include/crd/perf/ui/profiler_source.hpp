#pragma once

// ---------------------------------------------------------------------------
// crd-perf-ui -- IProfilerSource (Detour D-003 v0g).
//
// The polymorphic seam that lets every panel render against either the
// live profiler (LiveProfilerSource) or a loaded CPROF capture (CaptureViewSource)
// through identical code. Both implementations live in this module; the
// panel rendering code in profiler_panel.cpp / *_panel.cpp targets only
// this interface.
//
// Read-only by design. The UI never writes to the underlying state;
// capture controls (record/save/load) go through the Profiler singleton
// or CaptureView constructors, not this interface.
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/counters.hpp>
#include <crd/perf/frame_record.hpp>
#include <crd/perf/memory.hpp>
#include <crd/perf/sample.hpp>

namespace crd::perf::ui
{

class IProfilerSource
{
public:
    virtual ~IProfilerSource() = default;

    // Identity --------------------------------------------------------
    [[nodiscard]] virtual const char* source_label() const noexcept = 0;
    [[nodiscard]] virtual bool        is_live() const noexcept      = 0;

    // Threads ---------------------------------------------------------
    [[nodiscard]] virtual crd::u32 thread_count() const noexcept = 0;
    [[nodiscard]] virtual const char* thread_name(crd::u32 idx) const noexcept = 0;
    [[nodiscard]] virtual crd::containers::ConstSpan<Sample>
    thread_samples(crd::u32 idx) const noexcept = 0;
    [[nodiscard]] virtual crd::u32 thread_dropped(crd::u32 idx) const noexcept = 0;
    [[nodiscard]] virtual crd::u8 gpu_thread_index() const noexcept = 0;

    // Counters --------------------------------------------------------
    [[nodiscard]] virtual crd::u32     counter_count() const noexcept = 0;
    [[nodiscard]] virtual CounterInfo  counter_info(crd::u32 idx) const noexcept = 0;

    // Allocators ------------------------------------------------------
    [[nodiscard]] virtual crd::u32       allocator_count() const noexcept = 0;
    [[nodiscard]] virtual AllocatorInfo  allocator_info(crd::u32 idx) const noexcept = 0;

    // Frame history ---------------------------------------------------
    [[nodiscard]] virtual crd::u32 frame_record_count() const noexcept = 0;
    // `frames_back == 0` == most-recent capture.
    [[nodiscard]] virtual const FrameRecord* frame_record(crd::u32 frames_back) const noexcept = 0;

    // Name resolution ------------------------------------------------
    [[nodiscard]] virtual const char* resolve_name(NameId id) const noexcept = 0;
};

// ---------------------------------------------------------------------------
// LiveProfilerSource -- delegates every accessor to the global crd::perf
// state. The default source for the panel when no capture is loaded.
// ---------------------------------------------------------------------------

class LiveProfilerSource final : public IProfilerSource
{
public:
    [[nodiscard]] const char* source_label() const noexcept override { return "live"; }
    [[nodiscard]] bool        is_live() const noexcept override     { return true; }

    [[nodiscard]] crd::u32 thread_count() const noexcept override;
    [[nodiscard]] const char* thread_name(crd::u32 idx) const noexcept override;
    [[nodiscard]] crd::containers::ConstSpan<Sample>
    thread_samples(crd::u32 idx) const noexcept override;
    [[nodiscard]] crd::u32 thread_dropped(crd::u32 idx) const noexcept override;
    [[nodiscard]] crd::u8  gpu_thread_index() const noexcept override;

    [[nodiscard]] crd::u32    counter_count() const noexcept override;
    [[nodiscard]] CounterInfo counter_info(crd::u32 idx) const noexcept override;

    [[nodiscard]] crd::u32      allocator_count() const noexcept override;
    [[nodiscard]] AllocatorInfo allocator_info(crd::u32 idx) const noexcept override;

    [[nodiscard]] crd::u32           frame_record_count() const noexcept override;
    [[nodiscard]] const FrameRecord* frame_record(crd::u32 frames_back) const noexcept override;

    [[nodiscard]] const char* resolve_name(NameId id) const noexcept override;
};

} // namespace crd::perf::ui
