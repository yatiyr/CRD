#pragma once

// crd-perf -- flight-recorder trigger sources on existing seams (DIAG.9b).
//
// Each source turns a failure signal an existing module already raises into a FlightRecorder trigger, so the
// recorder keeps the window around it. None of them changes what the seam does: the previous handler still runs,
// the allocator still allocates, the residency policy still decides what to shed.
//
//   assertion          crd::set_assert_handler (crd-core). Chains to the handler that was installed before (crd-log
//                      installs one). When the assert path will terminate (headless, no platform handler) the open
//                      window is closed as Fatal and delivered before the previous handler runs.
//   fatal invariant    set_fatal_invariant_handler. Triggers with the invariant's own code, closes the window as
//                      Fatal, delivers it, then chains to the previous handler or the default fatal path.
//   queue starvation   jobs::set_starvation_handler (the host installs flight_on_starvation, user = the recorder).
//                      value_a = lane, value_b = backlog.
//   hang               jobs::set_hang_handler (flight_on_hang). value_a = HangKind, value_b = outstanding counters.
//   frame hitch        FlightFrameWatch::note_frame, called by the host once per frame. Records each frame as a
//                      kFlightCodeFrame event (a = interval, b = duration) and triggers when the interval exceeds the
//                      policy's hitch_threshold_ns. value_a = interval, value_b = threshold.
//   allocation failure FlightAllocatorWatch, an IAllocator decorator. A failed try_allocate triggers (a = size,
//                      b = alignment). A failed allocate triggers, closes the window as Fatal and delivers it before
//                      the backing allocator's own out-of-memory fatal runs.
//   resource pressure  FlightPressurePolicy, an IResidencyPolicy decorator for StreamingAllocator: each evict() the
//                      allocator asks for under budget pressure triggers (a = category, b = bytes needed).
//
// One recorder at a time can own the assert and fatal-invariant slots; installing a second is refused. Uninstall
// restores the handler that was there before. Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include <crd/perf/flight_recorder.hpp>

#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/streaming_allocator.hpp>

#include <atomic>

namespace crd::jobs
{
struct StarvationReport;
struct HangReport;
} // namespace crd::jobs

namespace crd::perf
{

// Install the assertion trigger for `recorder`. Returns false (and changes nothing) when a flight assert trigger is
// already installed or `recorder` is null.
bool install_flight_assert_trigger(FlightRecorder* recorder) noexcept;
void uninstall_flight_assert_trigger() noexcept;

// Install the fatal-invariant trigger for `recorder`. Same refusal rules.
bool install_flight_fatal_trigger(FlightRecorder* recorder) noexcept;
void uninstall_flight_fatal_trigger() noexcept;

// Handler-compatible trigger functions for the jobs watchdog; `recorder` is the FlightRecorder*.
void flight_on_starvation(const crd::jobs::StarvationReport& report, void* recorder) noexcept;
void flight_on_hang(const crd::jobs::HangReport& report, void* recorder) noexcept;

// Frame-hitch detection over host frame boundaries (monotonic nanoseconds). Not thread-safe: one frame loop calls it.
class FlightFrameWatch
{
public:
    explicit FlightFrameWatch(FlightRecorder* recorder) noexcept : m_recorder(recorder) {}

    // Returns true when this frame's interval was a hitch and its trigger started.
    bool note_frame(crd::u64 begin_ns, crd::u64 end_ns) noexcept;

    [[nodiscard]] crd::u64 hitches() const noexcept { return m_hitches; }

private:
    FlightRecorder* m_recorder;
    crd::u64        m_prev_begin_ns = 0U;
    bool            m_have_prev     = false;
    crd::u64        m_hitches       = 0U;
};

// IAllocator decorator that reports allocation failure. It adds no state of its own beyond two pointers; its own
// MemoryStats stay zero and report coverage Elsewhere (the backing allocator keeps the real ones).
class FlightAllocatorWatch final : public crd::memory::IAllocator
{
public:
    FlightAllocatorWatch(crd::memory::IAllocator* backing, FlightRecorder* recorder,
                         const char* name = "FlightAllocatorWatch") noexcept;

    void* allocate(crd::usize size, crd::usize alignment = crd::memory::kDefaultAlignment) override;
    [[nodiscard]] void* try_allocate(crd::usize size, crd::usize alignment = crd::memory::kDefaultAlignment) override;
    void  deallocate(void* p) noexcept override;
    [[nodiscard]] bool owns(const void* p) const noexcept override;
    [[nodiscard]] crd::usize allocation_size(const void* p) const noexcept override;

    [[nodiscard]] crd::u64 failures() const noexcept { return m_failures.load(std::memory_order_relaxed); }

private:
    void report(crd::usize size, crd::usize alignment) noexcept;

    crd::memory::IAllocator* m_backing;
    FlightRecorder*          m_recorder;
    std::atomic<crd::u64>    m_failures{0U};
};

// IResidencyPolicy decorator that reports resource pressure, then lets `inner` (or nothing, when null) shed.
class FlightPressurePolicy final : public crd::memory::IResidencyPolicy
{
public:
    FlightPressurePolicy(crd::memory::IResidencyPolicy* inner, FlightRecorder* recorder) noexcept
        : m_inner(inner), m_recorder(recorder)
    {
    }

    [[nodiscard]] crd::u64 evict(crd::memory::StreamingAllocator& sa, crd::memory::CategoryId category,
                                 crd::u64 needed_bytes) override;

private:
    crd::memory::IResidencyPolicy* m_inner;
    FlightRecorder*                m_recorder;
};

} // namespace crd::perf
