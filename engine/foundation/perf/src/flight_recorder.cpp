// crd-perf -- triggerable bounded flight recording (DIAG.9b). See flight_recorder.hpp.

#include <crd/perf/flight_recorder.hpp>

#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_manifest.hpp>

#include "json_writer.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

namespace crd::perf
{
namespace
{
using detail::append_json_escaped;
using detail::append_u64;

// Armed -> Triggered (trigger(), under the trigger mutex) -> Complete (the ring mutex) -> Armed (collection, under
// both mutexes). A capture that ended Fatal moves to FatalEntered instead of re-arming: the process is ending, and a
// later fatal path (check_invariant's default reaches CRD_FATAL, which reaches the assert trigger) must not replace
// the capture that was already delivered. Triggers are then suppressed and events dropped; nothing more is delivered.
constexpr crd::u32 kStateArmed        = 0U;
constexpr crd::u32 kStateTriggered    = 1U;
constexpr crd::u32 kStateComplete     = 2U;
constexpr crd::u32 kStateFatalEntered = 3U;

constexpr cont::StringView kNoRecipe{"no replay recipe was registered with the flight recorder"};

// The fatal path takes the ring with a bounded try-lock: it must not block forever behind a thread that will never
// release it.
constexpr crd::u32 kFatalLockAttempts = 2000U;

// Copy `src` into a fixed buffer of `cap` bytes, clipped to cap - 1 and NUL-terminated. Returns the stored length.
crd::u16 copy_clipped(char* dst, crd::u32 cap, cont::StringView src, bool* truncated) noexcept
{
    const crd::usize limit = static_cast<crd::usize>(cap) - 1U;
    const crd::usize n     = (src.size() < limit) ? src.size() : limit;
    if (n > 0U)
    {
        std::memcpy(dst, src.data(), n);
    }
    dst[n] = '\0';
    if (truncated != nullptr)
    {
        *truncated = src.size() > limit;
    }
    return static_cast<crd::u16>(n);
}

bool known_trigger(crd::u32 kind) noexcept
{
    return kind > 0U && kind < kFlightTriggerCount;
}
} // namespace

cont::StringView flight_trigger_name(FlightTrigger t) noexcept
{
    switch (t)
    {
    case FlightTrigger::None:              return cont::StringView{"none"};
    case FlightTrigger::Manual:            return cont::StringView{"manual"};
    case FlightTrigger::Assertion:         return cont::StringView{"assertion"};
    case FlightTrigger::FatalInvariant:    return cont::StringView{"fatal-invariant"};
    case FlightTrigger::AllocationFailure: return cont::StringView{"allocation-failure"};
    case FlightTrigger::FrameHitch:        return cont::StringView{"frame-hitch"};
    case FlightTrigger::QueueStarvation:   return cont::StringView{"queue-starvation"};
    case FlightTrigger::Hang:              return cont::StringView{"hang"};
    case FlightTrigger::ResourcePressure:  return cont::StringView{"resource-pressure"};
    }
    return cont::StringView{"unknown"};
}

cont::StringView flight_end_name(FlightEnd e) noexcept
{
    switch (e)
    {
    case FlightEnd::None:              return cont::StringView{"open"};
    case FlightEnd::PostEventsFull:    return cont::StringView{"post-events-full"};
    case FlightEnd::PostWindowElapsed: return cont::StringView{"post-window-elapsed"};
    case FlightEnd::Shutdown:          return cont::StringView{"shutdown"};
    case FlightEnd::Fatal:             return cont::StringView{"fatal"};
    }
    return cont::StringView{"unknown"};
}

cont::StringView flight_policy_status_name(FlightPolicyStatus s) noexcept
{
    switch (s)
    {
    case FlightPolicyStatus::Committed:              return cont::StringView{"committed"};
    case FlightPolicyStatus::RejectedUnknownVersion: return cont::StringView{"rejected-unknown-version"};
    case FlightPolicyStatus::RejectedOutOfBounds:    return cont::StringView{"rejected-out-of-bounds"};
    case FlightPolicyStatus::RejectedBusy:           return cont::StringView{"rejected-busy"};
    case FlightPolicyStatus::RejectedNotInitialized: return cont::StringView{"rejected-not-initialized"};
    case FlightPolicyStatus::RejectedOutOfMemory:    return cont::StringView{"rejected-out-of-memory"};
    case FlightPolicyStatus::RejectedGenerationWrap: return cont::StringView{"rejected-generation-wrap"};
    }
    return cont::StringView{"unknown"};
}

FlightPolicyStatus validate_flight_policy(const FlightPolicy& p) noexcept
{
    if (p.schema_version != kFlightPolicyVersion)
    {
        return FlightPolicyStatus::RejectedUnknownVersion;
    }
    if (p.capacity < 2U || p.capacity > kFlightMaxEvents || p.pre_events == 0U || p.post_events == 0U ||
        p.pre_events > p.capacity || p.post_events > p.capacity - p.pre_events ||
        p.post_window_ns > kFlightMaxPostWindowNs || p.hitch_threshold_ns == 0U || p.trigger_mask == 0U ||
        (p.trigger_mask & ~kFlightAllTriggers) != 0U)
    {
        return FlightPolicyStatus::RejectedOutOfBounds;
    }
    return FlightPolicyStatus::Committed;
}

struct FlightRecorder::Impl
{
    // The ring and everything the post window touches: ring_mutex.
    std::mutex               ring_mutex;
    cont::Array<FlightEvent> ring;
    crd::u32                 head                   = 0U; // next write index
    crd::u32                 live                   = 0U; // events held (<= capacity)
    FlightEnd                end                    = FlightEnd::None;
    crd::u64                 end_ns                 = 0U;
    crd::u64                 dropped_while_complete = 0U;
    // The post window starts at the first event recorded after a trigger became visible (or, when none was, at the
    // total when the window closes). It is fixed under the ring mutex: trigger() cannot take it, and a total read
    // there could be passed by events recorded before the trigger's state store, which would then be miscounted.
    bool                     anchored               = false;
    crd::u64                 post_start             = 0U;
    std::atomic<crd::u64>    total{0U}; // written under ring_mutex; trigger() reads it for the provisional sequence

    std::atomic<crd::u32> state{kStateArmed};

    // The trigger and the replay recipe: trigger_mutex. The trigger record is written only while Armed, before the
    // state's release store moves to Triggered, so a thread that observes Triggered or Complete reads it safely.
    std::mutex          trigger_mutex;
    FlightTriggerRecord trigger;
    char                trigger_replay[kFlightRecipeBytes] = {};
    crd::u16            trigger_replay_length               = 0U;
    bool                trigger_replay_available            = false;
    char                replay[kFlightRecipeBytes]          = {};
    crd::u16            replay_length                       = 0U;
    bool                replay_available                    = false;
    crd::u64            suppressed                          = 0U;
    crd::u32            suppressed_kinds                    = 0U;

    // The policy is written under both mutexes, so a holder of either reads it safely.
    FlightPolicy policy;
    crd::u32     policy_generation = 1U;

    std::atomic<FlightCaptureSink> sink{nullptr};
    std::atomic<void*>             sink_user{nullptr};
    std::atomic<crd::u64>          delivered{0U};
};

namespace
{
using Impl = FlightRecorder::Impl;

bool allocate_ring(cont::Array<FlightEvent>& ring, crd::u32 capacity) noexcept
{
    ring.clear();
    ring.resize(capacity);
    return ring.size() == capacity;
}

// The index of sequence `s` in the ring. The caller checks total - s <= live.
crd::u32 ring_index(const Impl& impl, crd::u64 total, crd::u64 s) noexcept
{
    const crd::u32 cap  = impl.policy.capacity;
    const crd::u32 back = static_cast<crd::u32>(total - s); // 1 = the newest event
    return (impl.head + cap - back) % cap;
}

// With the ring mutex held (or `ring_held` false on the fatal path when it could not be taken): when the state is
// Complete, build `out` and re-arm. Returns false when nothing is complete.
bool build_capture(Impl& impl, FlightCapture& out, bool ring_held) noexcept
{
    if (impl.state.load(std::memory_order_acquire) != kStateComplete)
    {
        return false;
    }

    std::lock_guard<std::mutex> trigger_guard(impl.trigger_mutex);
    out.trigger             = impl.trigger;
    out.end                 = impl.end;
    out.end_ns              = impl.end_ns;
    out.policy              = impl.policy;
    out.policy_generation   = impl.policy_generation;
    out.suppressed_triggers = impl.suppressed;
    out.suppressed_kinds    = impl.suppressed_kinds;
    out.replay_available    = impl.trigger_replay_available;
    out.replay.clear();
    out.replay.append(cont::StringView{impl.trigger_replay, impl.trigger_replay_length});
    out.events.clear();
    if (ring_held && impl.anchored)
    {
        out.trigger.sequence = impl.post_start;
    }
    out.recorded_before_trigger = out.trigger.sequence;
    out.pre_retained            = 0U;
    out.post_retained           = 0U;
    out.ring_unavailable        = !ring_held;
    out.dropped_while_complete  = ring_held ? impl.dropped_while_complete : 0U;

    if (ring_held)
    {
        const crd::u64 total  = impl.total.load(std::memory_order_relaxed);
        const crd::u64 oldest = total - impl.live;
        const crd::u64 trig   = out.trigger.sequence;
        const crd::u64 want   = (trig > impl.policy.pre_events) ? trig - impl.policy.pre_events : 0U;
        const crd::u64 first  = (want > oldest) ? want : oldest;
        out.events.reserve(static_cast<crd::usize>(total - first));
        for (crd::u64 s = first; s < total; ++s)
        {
            out.events.push_back(impl.ring[ring_index(impl, total, s)]);
            if (s < trig)
            {
                ++out.pre_retained;
            }
            else
            {
                ++out.post_retained;
            }
        }
    }
    out.post_truncated = (out.end == FlightEnd::Shutdown || out.end == FlightEnd::Fatal) &&
                         out.post_retained < impl.policy.post_events;

    // Re-arm, unless the capture ended Fatal (terminal). On the fatal path without the ring the process is ending;
    // leave the state as it is.
    if (ring_held)
    {
        const bool fatal            = impl.end == FlightEnd::Fatal;
        impl.end                    = FlightEnd::None;
        impl.end_ns                 = 0U;
        impl.dropped_while_complete = 0U;
        impl.suppressed             = 0U;
        impl.suppressed_kinds       = 0U;
        impl.anchored               = false;
        impl.state.store(fatal ? kStateFatalEntered : kStateArmed, std::memory_order_release);
    }
    return true;
}

// With the ring mutex held: close an open window with `reason`.
void close_window(Impl& impl, FlightEnd reason, crd::u64 now_ns) noexcept
{
    if (impl.state.load(std::memory_order_acquire) == kStateTriggered)
    {
        if (!impl.anchored)
        {
            impl.anchored   = true; // nothing was recorded after the trigger: the post window is empty
            impl.post_start = impl.total.load(std::memory_order_relaxed);
        }
        impl.end    = reason;
        impl.end_ns = now_ns;
        impl.state.store(kStateComplete, std::memory_order_release);
    }
}

void hand_to_sink(Impl& impl, const FlightCapture& capture) noexcept
{
    const FlightCaptureSink sink = impl.sink.load(std::memory_order_acquire);
    if (sink != nullptr)
    {
        sink(capture, impl.sink_user.load(std::memory_order_acquire));
    }
    impl.delivered.fetch_add(1U, std::memory_order_relaxed);
}
} // namespace

FlightPolicyStatus FlightRecorder::init(const FlightPolicy& policy) noexcept
{
    if (m_impl != nullptr)
    {
        return FlightPolicyStatus::RejectedBusy;
    }
    const FlightPolicyStatus status = validate_flight_policy(policy);
    if (status != FlightPolicyStatus::Committed)
    {
        return status;
    }

    Impl* impl = new (std::nothrow) Impl();
    if (impl == nullptr)
    {
        return FlightPolicyStatus::RejectedOutOfMemory;
    }
    impl->policy = policy;
    if (!allocate_ring(impl->ring, policy.capacity))
    {
        delete impl;
        return FlightPolicyStatus::RejectedOutOfMemory;
    }
    impl->replay_length = copy_clipped(impl->replay, kFlightRecipeBytes, kNoRecipe, nullptr);
    m_impl              = impl;
    return FlightPolicyStatus::Committed;
}

void FlightRecorder::shutdown() noexcept
{
    if (m_impl == nullptr)
    {
        return;
    }
    (void)finish(FlightEnd::Shutdown);
    delete m_impl;
    m_impl = nullptr;
}

bool FlightRecorder::is_initialized() const noexcept
{
    return m_impl != nullptr;
}

FlightPolicyStatus FlightRecorder::reload_policy(const FlightPolicy& candidate) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return FlightPolicyStatus::RejectedNotInitialized;
    }
    const FlightPolicyStatus status = validate_flight_policy(candidate);
    if (status != FlightPolicyStatus::Committed)
    {
        return status;
    }

    std::lock_guard<std::mutex> ring_guard(impl->ring_mutex);
    std::lock_guard<std::mutex> trigger_guard(impl->trigger_mutex);
    if (impl->state.load(std::memory_order_acquire) != kStateArmed)
    {
        return FlightPolicyStatus::RejectedBusy;
    }
    if (impl->policy_generation == 0xFFFFFFFFU)
    {
        return FlightPolicyStatus::RejectedGenerationWrap;
    }

    cont::Array<FlightEvent> ring;
    if (!allocate_ring(ring, candidate.capacity))
    {
        return FlightPolicyStatus::RejectedOutOfMemory;
    }
    impl->ring   = static_cast<cont::Array<FlightEvent>&&>(ring);
    impl->head   = 0U;
    impl->live   = 0U;
    impl->policy = candidate;
    ++impl->policy_generation;
    return FlightPolicyStatus::Committed;
}

FlightPolicy FlightRecorder::policy() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return FlightPolicy{};
    }
    std::lock_guard<std::mutex> guard(impl->trigger_mutex);
    return impl->policy;
}

crd::u32 FlightRecorder::policy_generation() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return 0U;
    }
    std::lock_guard<std::mutex> guard(impl->trigger_mutex);
    return impl->policy_generation;
}

void FlightRecorder::set_sink(FlightCaptureSink sink, void* user) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return;
    }
    impl->sink_user.store(user, std::memory_order_release);
    impl->sink.store(sink, std::memory_order_release);
}

void FlightRecorder::set_replay_recipe(cont::StringView recipe) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> guard(impl->trigger_mutex);
    impl->replay_length    = copy_clipped(impl->replay, kFlightRecipeBytes, recipe, nullptr);
    impl->replay_available = true;
}

void FlightRecorder::set_replay_unavailable(cont::StringView reason) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> guard(impl->trigger_mutex);
    impl->replay_length    = copy_clipped(impl->replay, kFlightRecipeBytes, reason.empty() ? kNoRecipe : reason,
                                          nullptr);
    impl->replay_available = false;
}

void FlightRecorder::record(EventCode code, Severity severity, cont::StringView message, crd::u64 value_a,
                            crd::u64 value_b) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr) // early-init / late-shutdown: a no-op, never a fault
    {
        return;
    }
    const crd::u64 now = diagnostic_now_ns();

    std::lock_guard<std::mutex> guard(impl->ring_mutex);
    const crd::u32              state = impl->state.load(std::memory_order_acquire);
    if (state == kStateComplete || state == kStateFatalEntered)
    {
        ++impl->dropped_while_complete; // frozen until collected (for good after a fatal): the windows stay intact
        return;
    }

    const crd::u64 seq  = impl->total.load(std::memory_order_relaxed);
    FlightEvent&   slot = impl->ring[impl->head];
    slot.sequence       = seq;
    slot.timestamp_ns   = now;
    slot.value_a        = value_a;
    slot.value_b        = value_b;
    slot.code           = code;
    slot.severity       = severity;
    slot.message_length = copy_clipped(slot.message, kFlightMessageBytes, message, &slot.message_truncated);
    impl->head          = (impl->head + 1U) % impl->policy.capacity;
    if (impl->live < impl->policy.capacity)
    {
        ++impl->live;
    }
    impl->total.store(seq + 1U, std::memory_order_release);

    // Read the state after the write: the first event to see a trigger anchors the post window, so the boundary is
    // fixed in the ring's own order and the window holds exactly post_events events when it closes.
    if (impl->state.load(std::memory_order_acquire) == kStateTriggered)
    {
        if (!impl->anchored)
        {
            impl->anchored   = true;
            impl->post_start = seq;
        }
        if (seq + 1U - impl->post_start >= impl->policy.post_events)
        {
            close_window(*impl, FlightEnd::PostEventsFull, now);
        }
    }
}

void FlightRecorder::record(const DiagnosticEvent& e) noexcept
{
    record(e.code, e.severity, cont::StringView{e.message.c_str(), e.message.size()}, 0U, 0U);
}

FlightTriggerResult FlightRecorder::trigger(const FlightTriggerInput& input) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return FlightTriggerResult::NotInitialized;
    }
    const crd::u64 now = diagnostic_now_ns();

    std::lock_guard<std::mutex> guard(impl->trigger_mutex);
    if (!known_trigger(static_cast<crd::u32>(input.kind)) ||
        (impl->policy.trigger_mask & flight_trigger_bit(input.kind)) == 0U)
    {
        return FlightTriggerResult::Masked;
    }
    if (impl->state.load(std::memory_order_acquire) != kStateArmed)
    {
        ++impl->suppressed;
        impl->suppressed_kinds |= flight_trigger_bit(input.kind);
        return FlightTriggerResult::Suppressed;
    }

    FlightTriggerRecord& t = impl->trigger;
    t.kind                 = input.kind;
    t.code                 = input.code;
    t.sequence             = impl->total.load(std::memory_order_acquire);
    t.timestamp_ns         = now;
    t.value_a              = input.value_a;
    t.value_b              = input.value_b;
    t.line                 = input.source.line;
    t.file_length          = copy_clipped(t.file, kFlightTriggerFileBytes, input.source.file, nullptr);
    t.message_length       = copy_clipped(t.message, kFlightTriggerMessageBytes, input.message, nullptr);

    std::memcpy(impl->trigger_replay, impl->replay, sizeof(impl->trigger_replay));
    impl->trigger_replay_length    = impl->replay_length;
    impl->trigger_replay_available = impl->replay_available;

    impl->state.store(kStateTriggered, std::memory_order_release);
    return FlightTriggerResult::Started;
}

bool FlightRecorder::poll(crd::u64 now_ns) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return false;
    }

    FlightCapture capture;
    bool          built = false;
    {
        std::lock_guard<std::mutex> guard(impl->ring_mutex);
        if (impl->state.load(std::memory_order_acquire) == kStateTriggered && impl->policy.post_window_ns > 0U &&
            now_ns - impl->trigger.timestamp_ns >= impl->policy.post_window_ns)
        {
            close_window(*impl, FlightEnd::PostWindowElapsed, now_ns);
        }
        if (impl->sink.load(std::memory_order_acquire) == nullptr)
        {
            return impl->state.load(std::memory_order_acquire) == kStateComplete; // left for take_capture()
        }
        built = build_capture(*impl, capture, true);
    }
    if (built)
    {
        hand_to_sink(*impl, capture);
    }
    return built;
}

bool FlightRecorder::take_capture(FlightCapture& out) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> guard(impl->ring_mutex);
    const bool built = build_capture(*impl, out, true);
    if (built)
    {
        impl->delivered.fetch_add(1U, std::memory_order_relaxed);
    }
    return built;
}

bool FlightRecorder::finish(FlightEnd reason) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr || (reason != FlightEnd::Shutdown && reason != FlightEnd::Fatal))
    {
        return false;
    }
    if (impl->sink.load(std::memory_order_acquire) == nullptr && reason == FlightEnd::Shutdown)
    {
        return false; // nobody to deliver to; take_capture() is the only reader and the recorder is going away
    }

    const crd::u64 now     = diagnostic_now_ns();
    FlightCapture  capture;
    bool           built   = false;
    bool           locked  = false;
    if (reason == FlightEnd::Fatal)
    {
        for (crd::u32 attempt = 0U; attempt < kFatalLockAttempts && !locked; ++attempt)
        {
            locked = impl->ring_mutex.try_lock();
            if (!locked)
            {
                std::this_thread::yield();
            }
        }
    }
    else
    {
        impl->ring_mutex.lock();
        locked = true;
    }

    if (locked)
    {
        close_window(*impl, reason, now);
        built = build_capture(*impl, capture, true);
        impl->ring_mutex.unlock();
    }
    else if (const crd::u32 state = impl->state.load(std::memory_order_acquire);
             state == kStateTriggered || state == kStateComplete)
    {
        // The ring stays held: deliver the trigger alone, said so, rather than nothing. The state cannot be moved
        // without the ring mutex, so the capture is built from a Complete view of the trigger.
        std::lock_guard<std::mutex> trigger_guard(impl->trigger_mutex);
        capture.trigger           = impl->trigger;
        capture.end               = FlightEnd::Fatal;
        capture.end_ns            = now;
        capture.policy            = impl->policy;
        capture.policy_generation = impl->policy_generation;
        capture.recorded_before_trigger = impl->trigger.sequence;
        capture.suppressed_triggers     = impl->suppressed;
        capture.suppressed_kinds        = impl->suppressed_kinds;
        capture.replay_available        = impl->trigger_replay_available;
        capture.replay.append(cont::StringView{impl->trigger_replay, impl->trigger_replay_length});
        capture.ring_unavailable = true;
        capture.post_truncated   = true;
        built                    = true;
        // Terminal, as after any Fatal capture; record() and close_window() leave this state alone.
        impl->state.store(kStateFatalEntered, std::memory_order_release);
    }

    if (built)
    {
        hand_to_sink(*impl, capture);
    }
    return built;
}

crd::u64 FlightRecorder::total_recorded() const noexcept
{
    Impl* impl = m_impl;
    return (impl != nullptr) ? impl->total.load(std::memory_order_acquire) : 0U;
}

bool FlightRecorder::capture_pending() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return false;
    }
    const crd::u32 state = impl->state.load(std::memory_order_acquire);
    return state == kStateTriggered || state == kStateComplete;
}

bool FlightRecorder::fatal_entered() const noexcept
{
    Impl* impl = m_impl;
    return impl != nullptr && impl->state.load(std::memory_order_acquire) == kStateFatalEntered;
}

crd::u64 FlightRecorder::captures_delivered() const noexcept
{
    Impl* impl = m_impl;
    return (impl != nullptr) ? impl->delivered.load(std::memory_order_relaxed) : 0U;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

namespace
{
constexpr crd::u32 kRecordFlagPostTruncated   = 1U << 0U;
constexpr crd::u32 kRecordFlagRingUnavailable = 1U << 1U;
constexpr crd::u32 kRecordFlagReplay          = 1U << 2U;
constexpr crd::u32 kRecordKnownFlags          = 0x07U;

void put_u8(cont::Array<crd::u8>& out, crd::u8 v)
{
    out.push_back(v);
}

void put_u16(cont::Array<crd::u8>& out, crd::u16 v)
{
    for (crd::u32 i = 0U; i < 2U; ++i)
    {
        out.push_back(static_cast<crd::u8>((v >> (8U * i)) & 0xFFU));
    }
}

void put_u32(cont::Array<crd::u8>& out, crd::u32 v)
{
    for (crd::u32 i = 0U; i < 4U; ++i)
    {
        out.push_back(static_cast<crd::u8>((v >> (8U * i)) & 0xFFU));
    }
}

void put_u64(cont::Array<crd::u8>& out, crd::u64 v)
{
    for (crd::u32 i = 0U; i < 8U; ++i)
    {
        out.push_back(static_cast<crd::u8>((v >> (8U * i)) & 0xFFU));
    }
}

void put_bytes(cont::Array<crd::u8>& out, const char* p, crd::usize n)
{
    for (crd::usize i = 0U; i < n; ++i)
    {
        out.push_back(static_cast<crd::u8>(p[i]));
    }
}

// A bounds-checked little-endian cursor over untrusted bytes. Every read fails (and stays failed) past the end.
struct Cursor
{
    cont::ConstSpan<crd::u8> bytes;
    crd::usize               at = 0U;
    bool                     ok = true;

    bool need(crd::usize n) noexcept
    {
        if (!ok || bytes.size() - at < n)
        {
            ok = false;
        }
        return ok;
    }

    crd::u64 uint(crd::u32 width) noexcept
    {
        if (!need(width))
        {
            return 0U;
        }
        crd::u64 v = 0U;
        for (crd::u32 i = 0U; i < width; ++i)
        {
            v |= static_cast<crd::u64>(bytes[at + i]) << (8U * i);
        }
        at += width;
        return v;
    }

    crd::u8  u8() noexcept { return static_cast<crd::u8>(uint(1U)); }
    crd::u16 u16() noexcept { return static_cast<crd::u16>(uint(2U)); }
    crd::u32 u32() noexcept { return static_cast<crd::u32>(uint(4U)); }
    crd::u64 u64() noexcept { return uint(8U); }

    // Copy `n` bytes into a fixed buffer of `cap` bytes (n must leave room for the NUL).
    bool text(char* dst, crd::u32 cap, crd::u16 n) noexcept
    {
        if (n >= cap)
        {
            ok = false;
            return false;
        }
        if (!need(n))
        {
            return false;
        }
        if (n > 0U)
        {
            std::memcpy(dst, bytes.data() + at, n);
        }
        dst[n] = '\0';
        at += n;
        return true;
    }
};

FlightRecordRead refuse(FlightRecordRead& r, const char* why)
{
    r.ok    = false;
    r.error = cont::StringView{why};
    r.capture.events.clear();
    return static_cast<FlightRecordRead&&>(r);
}
} // namespace

cont::Array<crd::u8> encode_flight_record(const FlightCapture& c, crd::memory::IAllocator* alloc)
{
    cont::Array<crd::u8> out(alloc);
    out.reserve(160U + kFlightTriggerFileBytes + kFlightTriggerMessageBytes + kFlightRecipeBytes +
                c.events.size() * (40U + kFlightMessageBytes));

    put_u32(out, kFlightRecordMagic);
    put_u32(out, kFlightRecordVersion);

    crd::u32 flags = 0U;
    flags |= c.post_truncated ? kRecordFlagPostTruncated : 0U;
    flags |= c.ring_unavailable ? kRecordFlagRingUnavailable : 0U;
    flags |= c.replay_available ? kRecordFlagReplay : 0U;
    put_u8(out, static_cast<crd::u8>(c.trigger.kind));
    put_u8(out, static_cast<crd::u8>(c.end));
    put_u8(out, static_cast<crd::u8>(flags));
    put_u8(out, 0U);

    put_u32(out, c.trigger.code);
    put_u64(out, c.trigger.sequence);
    put_u64(out, c.trigger.timestamp_ns);
    put_u64(out, c.trigger.value_a);
    put_u64(out, c.trigger.value_b);
    put_u32(out, c.trigger.line);
    put_u16(out, c.trigger.file_length);
    put_u16(out, c.trigger.message_length);
    put_bytes(out, c.trigger.file, c.trigger.file_length);
    put_bytes(out, c.trigger.message, c.trigger.message_length);
    put_u64(out, c.end_ns);

    put_u32(out, c.policy.schema_version);
    put_u32(out, c.policy.capacity);
    put_u32(out, c.policy.pre_events);
    put_u32(out, c.policy.post_events);
    put_u64(out, c.policy.post_window_ns);
    put_u32(out, c.policy.trigger_mask);
    put_u64(out, c.policy.hitch_threshold_ns);
    put_u32(out, c.policy_generation);

    put_u64(out, c.recorded_before_trigger);
    put_u32(out, c.pre_retained);
    put_u32(out, c.post_retained);
    put_u64(out, c.dropped_while_complete);
    put_u64(out, c.suppressed_triggers);
    put_u32(out, c.suppressed_kinds);

    const crd::usize replay_n = (c.replay.size() < kFlightRecipeBytes) ? c.replay.size() : kFlightRecipeBytes - 1U;
    put_u16(out, static_cast<crd::u16>(replay_n));
    put_bytes(out, c.replay.c_str(), replay_n);

    put_u32(out, static_cast<crd::u32>(c.events.size()));
    for (crd::usize i = 0U; i < c.events.size(); ++i)
    {
        const FlightEvent& e = c.events[i];
        put_u64(out, e.sequence);
        put_u64(out, e.timestamp_ns);
        put_u64(out, e.value_a);
        put_u64(out, e.value_b);
        put_u32(out, e.code);
        put_u8(out, static_cast<crd::u8>(e.severity));
        put_u8(out, e.message_truncated ? 1U : 0U);
        put_u16(out, e.message_length);
        put_bytes(out, e.message, e.message_length);
    }
    return out;
}

FlightRecordRead decode_flight_record(cont::ConstSpan<crd::u8> payload)
{
    FlightRecordRead r;
    Cursor           in{payload};

    if (in.u32() != kFlightRecordMagic || !in.ok)
    {
        return refuse(r, "not a flight record (magic)");
    }
    if (in.u32() != kFlightRecordVersion)
    {
        return refuse(r, "unsupported flight record version");
    }

    FlightCapture& c   = r.capture;
    const crd::u8  kind  = in.u8();
    const crd::u8  end   = in.u8();
    const crd::u8  flags = in.u8();
    const crd::u8  pad   = in.u8();
    if (!in.ok || !known_trigger(kind) || end > static_cast<crd::u8>(FlightEnd::Fatal) || end == 0U ||
        (flags & ~kRecordKnownFlags) != 0U || pad != 0U)
    {
        return refuse(r, "bad trigger kind, end reason or flags");
    }
    c.trigger.kind     = static_cast<FlightTrigger>(kind);
    c.end              = static_cast<FlightEnd>(end);
    c.post_truncated   = (flags & kRecordFlagPostTruncated) != 0U;
    c.ring_unavailable = (flags & kRecordFlagRingUnavailable) != 0U;
    c.replay_available = (flags & kRecordFlagReplay) != 0U;

    c.trigger.code           = in.u32();
    c.trigger.sequence       = in.u64();
    c.trigger.timestamp_ns   = in.u64();
    c.trigger.value_a        = in.u64();
    c.trigger.value_b        = in.u64();
    c.trigger.line           = in.u32();
    c.trigger.file_length    = in.u16();
    c.trigger.message_length = in.u16();
    if (!in.text(c.trigger.file, kFlightTriggerFileBytes, c.trigger.file_length) ||
        !in.text(c.trigger.message, kFlightTriggerMessageBytes, c.trigger.message_length))
    {
        return refuse(r, "trigger text over its bound or past the payload");
    }
    c.end_ns = in.u64();

    c.policy.schema_version     = in.u32();
    c.policy.capacity           = in.u32();
    c.policy.pre_events         = in.u32();
    c.policy.post_events        = in.u32();
    c.policy.post_window_ns     = in.u64();
    c.policy.trigger_mask       = in.u32();
    c.policy.hitch_threshold_ns = in.u64();
    c.policy_generation         = in.u32();
    if (!in.ok || validate_flight_policy(c.policy) != FlightPolicyStatus::Committed)
    {
        return refuse(r, "recorded policy out of bounds");
    }

    c.recorded_before_trigger = in.u64();
    c.pre_retained            = in.u32();
    c.post_retained           = in.u32();
    c.dropped_while_complete  = in.u64();
    c.suppressed_triggers     = in.u64();
    c.suppressed_kinds        = in.u32();
    if (!in.ok || c.pre_retained > c.policy.pre_events || c.post_retained > c.policy.post_events ||
        c.pre_retained > c.recorded_before_trigger || c.recorded_before_trigger != c.trigger.sequence ||
        (c.suppressed_kinds & ~kFlightAllTriggers) != 0U)
    {
        return refuse(r, "inconsistent loss metadata");
    }

    char           replay[kFlightRecipeBytes] = {};
    const crd::u16 replay_n                   = in.u16();
    if (!in.text(replay, kFlightRecipeBytes, replay_n))
    {
        return refuse(r, "replay text over its bound or past the payload");
    }
    c.replay.clear();
    c.replay.append(cont::StringView{replay, replay_n});

    const crd::u32 count = in.u32();
    if (!in.ok || count != c.pre_retained + c.post_retained || count > kFlightMaxEvents)
    {
        return refuse(r, "event count disagrees with the retained counts");
    }
    // Each event takes at least 40 bytes, so a count the payload cannot hold is refused before reserving for it.
    if ((payload.size() - in.at) / 40U < count)
    {
        return refuse(r, "event count past the payload");
    }
    c.events.reserve(count);
    for (crd::u32 i = 0U; i < count; ++i)
    {
        FlightEvent e;
        e.sequence     = in.u64();
        e.timestamp_ns = in.u64();
        e.value_a      = in.u64();
        e.value_b      = in.u64();
        e.code         = in.u32();
        const crd::u8 severity  = in.u8();
        const crd::u8 truncated = in.u8();
        e.message_length        = in.u16();
        if (!in.ok || severity > static_cast<crd::u8>(Severity::FatalInvariant) || truncated > 1U ||
            !in.text(e.message, kFlightMessageBytes, e.message_length))
        {
            return refuse(r, "malformed event");
        }
        e.severity          = static_cast<Severity>(severity);
        e.message_truncated = truncated != 0U;
        const bool pre      = i < c.pre_retained;
        if ((pre && e.sequence >= c.trigger.sequence) || (!pre && e.sequence < c.trigger.sequence) ||
            (i > 0U && e.sequence <= c.events[i - 1U].sequence))
        {
            return refuse(r, "event sequence out of order or on the wrong side of the trigger");
        }
        c.events.push_back(e);
    }
    if (in.at != payload.size())
    {
        return refuse(r, "trailing bytes after the last event");
    }
    r.ok = true;
    return r;
}

cont::Array<crd::u8> write_flight_bundle(const FlightCapture& capture)
{
    BundleManifest manifest;
    manifest.schema_version = kDiagnosticSchemaVersion;
    const cont::Array<crd::u8> manifest_bytes = serialize_manifest(manifest);
    const cont::Array<crd::u8> record_bytes   = encode_flight_record(capture);

    BundleWriter writer;
    (void)writer.add_section(BundleSectionTag::Manifest,
                             cont::ConstSpan<crd::u8>{manifest_bytes.data(), manifest_bytes.size()});
    (void)writer.add_section(BundleSectionTag::FlightRecord,
                             cont::ConstSpan<crd::u8>{record_bytes.data(), record_bytes.size()});
    return writer.finish();
}

void flight_capture_summary_json(const FlightCapture& c, cont::String& out)
{
    const auto key = [&out](const char* name, bool comma)
    {
        if (comma)
        {
            out.push_back(',');
        }
        out.push_back('"');
        out.append(name);
        out.append("\":");
    };
    const auto str = [&out](cont::StringView v)
    {
        out.push_back('"');
        append_json_escaped(out, v);
        out.push_back('"');
    };

    out.push_back('{');
    key("trigger", false);
    str(flight_trigger_name(c.trigger.kind));
    key("code", true);
    append_u64(out, c.trigger.code);
    key("sequence", true);
    append_u64(out, c.trigger.sequence);
    key("message", true);
    str(c.trigger.message_view());
    key("file", true);
    str(c.trigger.file_view());
    key("line", true);
    append_u64(out, c.trigger.line);
    key("value_a", true);
    append_u64(out, c.trigger.value_a);
    key("value_b", true);
    append_u64(out, c.trigger.value_b);
    key("end", true);
    str(flight_end_name(c.end));
    key("pre_retained", true);
    append_u64(out, c.pre_retained);
    key("pre_dropped", true);
    append_u64(out, c.recorded_before_trigger - c.pre_retained);
    key("post_retained", true);
    append_u64(out, c.post_retained);
    key("post_truncated", true);
    out.append(c.post_truncated ? "true" : "false");
    key("ring_unavailable", true);
    out.append(c.ring_unavailable ? "true" : "false");
    key("dropped_while_complete", true);
    append_u64(out, c.dropped_while_complete);
    key("suppressed_triggers", true);
    append_u64(out, c.suppressed_triggers);
    key("policy_generation", true);
    append_u64(out, c.policy_generation);
    key("replay_available", true);
    out.append(c.replay_available ? "true" : "false");
    key("replay", true);
    str(cont::StringView{c.replay.c_str(), c.replay.size()});
    out.push_back('}');
}

} // namespace crd::perf
