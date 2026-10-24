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
//   allocation failure FlightAllocatorWatch, the observer of one allocator's refusals (IRefusalObserver). Each
//                      refused request triggers (a = size, b = alignment); a fatal-on-OOM allocate reports before its
//                      own out-of-memory fatal, which closes the window as Fatal and delivers it.
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

// Reports the requests one allocator refuses as allocation-failure triggers. It observes instead of decorating, so the
// allocator's successful calls never reach it (DIAG.11b). Either side may go first: the watch detaches when it is
// destroyed or released, and the allocator tells it when the allocator is destroyed.
class FlightAllocatorWatch final : public crd::memory::IRefusalObserver
{
public:
    explicit FlightAllocatorWatch(FlightRecorder* recorder) noexcept : m_recorder(recorder) {}
    ~FlightAllocatorWatch() { release(); }
    FlightAllocatorWatch(const FlightAllocatorWatch&)            = delete;
    FlightAllocatorWatch& operator=(const FlightAllocatorWatch&) = delete;

    // Observe `allocator` until release() or its destruction. False (nothing changes) when this watch already observes
    // one, `allocator` is null or another observer has it. Install before the allocator is handed out.
    [[nodiscard]] bool watch(crd::memory::IAllocator* allocator) noexcept;
    // Stop observing (idempotent). Call after the observed allocator's last call.
    void release() noexcept;
    [[nodiscard]] const crd::memory::IAllocator* watched() const noexcept { return m_allocator; }

    void on_refused(const crd::memory::IAllocator& allocator, crd::usize size, crd::usize alignment) noexcept override;
    void on_allocator_gone(const crd::memory::IAllocator& allocator) noexcept override;

    [[nodiscard]] crd::u64 failures() const noexcept { return m_failures.load(std::memory_order_relaxed); }

private:
    crd::memory::IAllocator* m_allocator = nullptr;
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
