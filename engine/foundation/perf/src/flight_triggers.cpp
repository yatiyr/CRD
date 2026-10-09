// crd-perf -- flight-recorder trigger sources on existing seams (DIAG.9b). See flight_triggers.hpp.

#include <crd/perf/flight_triggers.hpp>

#include <crd/core/assert.hpp>
#include <crd/jobs/jobs.hpp>

#include <atomic>
#include <cstdio>

namespace crd::perf
{
namespace
{
// The single recorder owning each handler slot, and the handler it replaced.
std::atomic<FlightRecorder*>        g_assert_recorder{nullptr};
std::atomic<crd::AssertHandler>     g_prev_assert{nullptr};
std::atomic<FlightRecorder*>        g_fatal_recorder{nullptr};
std::atomic<FatalInvariantHandler>  g_prev_fatal{nullptr};

void flight_assert_handler(const char* expression, const char* file, int line, const char* message)
{
    FlightRecorder* recorder = g_assert_recorder.load(std::memory_order_acquire);
    if (recorder != nullptr)
    {
        char text[kFlightTriggerMessageBytes];
        (void)std::snprintf(text, sizeof(text), "%s%s%s", (expression != nullptr) ? expression : "",
                            (message != nullptr) ? ": " : "", (message != nullptr) ? message : "");
        FlightTriggerInput input;
        input.kind    = FlightTrigger::Assertion;
        input.code    = kFlightCodeAssertion;
        input.message = cont::StringView{text};
        input.source  = SourceIdentity{cont::StringView{(file != nullptr) ? file : ""}, cont::StringView{},
                                       static_cast<crd::u32>(line)};
        (void)recorder->trigger(input);

        // The default assert path terminates when headless and no platform handler takes over: deliver the window
        // now, while the process can still write it.
        if (crd::get_assert_headless() && crd::get_assert_platform_handler() == nullptr)
        {
            (void)recorder->finish(FlightEnd::Fatal);
        }
    }

    const crd::AssertHandler previous = g_prev_assert.load(std::memory_order_acquire);
    if (previous != nullptr)
    {
        previous(expression, file, line, message);
    }
}

void flight_fatal_handler(const DiagnosticEvent& e)
{
    FlightRecorder* recorder = g_fatal_recorder.load(std::memory_order_acquire);
    if (recorder != nullptr)
    {
        FlightTriggerInput input;
        input.kind    = FlightTrigger::FatalInvariant;
        input.code    = e.code;
        input.message = cont::StringView{e.message.c_str(), e.message.size()};
        input.source  = e.source;
        (void)recorder->trigger(input);
        (void)recorder->finish(FlightEnd::Fatal);
    }

    const FatalInvariantHandler previous = g_prev_fatal.load(std::memory_order_acquire);
    if (previous != nullptr)
    {
        previous(e);
        return;
    }
    default_fatal_invariant(e);
}

FlightTriggerResult fire(FlightRecorder* recorder, FlightTrigger kind, EventCode code, const char* text,
                         crd::u64 value_a, crd::u64 value_b) noexcept
{
    FlightTriggerInput input;
    input.kind    = kind;
    input.code    = code;
    input.message = cont::StringView{text};
    input.value_a = value_a;
    input.value_b = value_b;
    return recorder->trigger(input);
}
} // namespace

bool install_flight_assert_trigger(FlightRecorder* recorder) noexcept
{
    FlightRecorder* expected = nullptr;
    if (recorder == nullptr || !g_assert_recorder.compare_exchange_strong(expected, recorder))
    {
        return false;
    }
    g_prev_assert.store(crd::get_assert_handler(), std::memory_order_release);
    crd::set_assert_handler(&flight_assert_handler);
    return true;
}

void uninstall_flight_assert_trigger() noexcept
{
    if (g_assert_recorder.load(std::memory_order_acquire) == nullptr)
    {
        return;
    }
    if (crd::get_assert_handler() == &flight_assert_handler)
    {
        crd::set_assert_handler(g_prev_assert.load(std::memory_order_acquire));
    }
    g_prev_assert.store(nullptr, std::memory_order_release);
    g_assert_recorder.store(nullptr, std::memory_order_release);
}

bool install_flight_fatal_trigger(FlightRecorder* recorder) noexcept
{
    FlightRecorder* expected = nullptr;
    if (recorder == nullptr || !g_fatal_recorder.compare_exchange_strong(expected, recorder))
    {
        return false;
    }
    g_prev_fatal.store(get_fatal_invariant_handler(), std::memory_order_release);
    set_fatal_invariant_handler(&flight_fatal_handler);
    return true;
}

void uninstall_flight_fatal_trigger() noexcept
{
    if (g_fatal_recorder.load(std::memory_order_acquire) == nullptr)
    {
        return;
    }
    if (get_fatal_invariant_handler() == &flight_fatal_handler)
    {
        set_fatal_invariant_handler(g_prev_fatal.load(std::memory_order_acquire));
    }
    g_prev_fatal.store(nullptr, std::memory_order_release);
    g_fatal_recorder.store(nullptr, std::memory_order_release);
}

void flight_on_starvation(const crd::jobs::StarvationReport& report, void* recorder) noexcept
{
    if (recorder == nullptr)
    {
        return;
    }
    char text[kFlightTriggerMessageBytes];
    (void)std::snprintf(text, sizeof(text), "priority lane %u starved: backlog %u for %u windows, %llu completions",
                        static_cast<unsigned>(report.lane), report.backlog, report.stale_windows,
                        static_cast<unsigned long long>(report.completions));
    (void)fire(static_cast<FlightRecorder*>(recorder), FlightTrigger::QueueStarvation, kFlightCodeQueueStarvation,
               text, report.lane, report.backlog);
}

void flight_on_hang(const crd::jobs::HangReport& report, void* recorder) noexcept
{
    if (recorder == nullptr)
    {
        return;
    }
    char text[kFlightTriggerMessageBytes];
    (void)std::snprintf(text, sizeof(text), "job system hang (kind %u): %u outstanding counters for %u windows",
                        static_cast<unsigned>(report.kind), report.outstanding, report.stale_windows);
    (void)fire(static_cast<FlightRecorder*>(recorder), FlightTrigger::Hang, kFlightCodeHang, text,
               static_cast<crd::u64>(report.kind), report.outstanding);
}

bool FlightFrameWatch::note_frame(crd::u64 begin_ns, crd::u64 end_ns) noexcept
{
    if (m_recorder == nullptr)
    {
        return false;
    }
    const crd::u64 duration = (end_ns >= begin_ns) ? end_ns - begin_ns : 0U;
    const crd::u64 interval = (m_have_prev && begin_ns >= m_prev_begin_ns) ? begin_ns - m_prev_begin_ns : 0U;
    const bool     measured = m_have_prev && begin_ns >= m_prev_begin_ns;
    m_prev_begin_ns         = begin_ns;
    m_have_prev             = true;

    // The frame itself goes in first, so a hitch frame is the last event before its trigger.
    m_recorder->record(kFlightCodeFrame, Severity::Info, cont::StringView{"frame"}, interval, duration);
    if (!measured)
    {
        return false;
    }
    const crd::u64 threshold = m_recorder->policy().hitch_threshold_ns;
    if (interval <= threshold)
    {
        return false;
    }

    ++m_hitches;
    char text[kFlightTriggerMessageBytes];
    (void)std::snprintf(text, sizeof(text), "frame interval %llu ns exceeded the hitch threshold %llu ns",
                        static_cast<unsigned long long>(interval), static_cast<unsigned long long>(threshold));
    return fire(m_recorder, FlightTrigger::FrameHitch, kFlightCodeFrameHitch, text, interval, threshold) ==
           FlightTriggerResult::Started;
}

FlightAllocatorWatch::FlightAllocatorWatch(crd::memory::IAllocator* backing, FlightRecorder* recorder,
                                           const char* name) noexcept
    : m_backing(backing), m_recorder(recorder)
{
    m_name = name;
}

void FlightAllocatorWatch::report(crd::usize size, crd::usize alignment) noexcept
{
    m_failures.fetch_add(1U, std::memory_order_relaxed);
    if (m_recorder == nullptr)
    {
        return;
    }
    char text[kFlightTriggerMessageBytes];
    (void)std::snprintf(text, sizeof(text), "allocator %s failed to allocate %llu bytes (alignment %llu)",
                        (m_backing->name() != nullptr) ? m_backing->name() : "?",
                        static_cast<unsigned long long>(size), static_cast<unsigned long long>(alignment));
    (void)fire(m_recorder, FlightTrigger::AllocationFailure, kFlightCodeAllocationFailure, text, size, alignment);
}

void* FlightAllocatorWatch::allocate(crd::usize size, crd::usize alignment)
{
    if (size == 0U)
    {
        return m_backing->allocate(size, alignment);
    }
    void* p = m_backing->try_allocate(size, alignment);
    if (p != nullptr)
    {
        return p;
    }
    // Record the failure before the backing allocator's own out-of-memory path runs: for a fatal-on-OOM allocator
    // that path is CRD_FATAL, which reaches the assert trigger (when installed) and closes this window as Fatal.
    report(size, alignment);
    return m_backing->allocate(size, alignment);
}

void* FlightAllocatorWatch::try_allocate(crd::usize size, crd::usize alignment)
{
    void* p = m_backing->try_allocate(size, alignment);
    if (p == nullptr && size > 0U)
    {
        report(size, alignment);
    }
    return p;
}

void FlightAllocatorWatch::deallocate(void* p) noexcept
{
    m_backing->deallocate(p);
}

bool FlightAllocatorWatch::owns(const void* p) const noexcept
{
    return m_backing->owns(p);
}

crd::usize FlightAllocatorWatch::allocation_size(const void* p) const noexcept
{
    return m_backing->allocation_size(p);
}

crd::u64 FlightPressurePolicy::evict(crd::memory::StreamingAllocator& sa, crd::memory::CategoryId category,
                                     crd::u64 needed_bytes)
{
    if (m_recorder != nullptr)
    {
        char text[kFlightTriggerMessageBytes];
        (void)std::snprintf(text, sizeof(text), "%s category %u over budget: %llu bytes needed (%llu used of %llu)",
                            (sa.name() != nullptr) ? sa.name() : "?", static_cast<unsigned>(category),
                            static_cast<unsigned long long>(needed_bytes),
                            static_cast<unsigned long long>(sa.used(category)),
                            static_cast<unsigned long long>(sa.budget(category)));
        (void)fire(m_recorder, FlightTrigger::ResourcePressure, kFlightCodeResourcePressure, text, category,
                   needed_bytes);
    }
    return (m_inner != nullptr) ? m_inner->evict(sa, category, needed_bytes) : 0U;
}

} // namespace crd::perf
