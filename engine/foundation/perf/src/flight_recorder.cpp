// crd-perf -- triggerable bounded flight recording (DIAG.9b). See flight_recorder.hpp.

#include <crd/perf/flight_recorder.hpp>

#include <crd/containers/atomic_array.hpp>
#include <crd/containers/static_array.hpp>
#include <crd/perf/build_identity.hpp>
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

// Armed -> Triggered (trigger(), under the trigger mutex) -> Complete (the claim that fills the post window, or a
// collector) -> Armed (collection, under both mutexes). A capture that ended Fatal moves to FatalEntered instead of
// re-arming: the process is ending, and a later fatal path (check_invariant's default reaches CRD_FATAL, which reaches
// the assert trigger) must not replace the capture that was already delivered. Triggers are then suppressed and events
// dropped; nothing more is delivered.
constexpr crd::u32 kStateArmed        = 0U;
constexpr crd::u32 kStateTriggered    = 1U;
constexpr crd::u32 kStateComplete     = 2U;
constexpr crd::u32 kStateFatalEntered = 3U;

// The recording word: the total above kStateBits; below it the state and, apart from it, the reloading flag, set
// while a policy reload swaps the ring (claims wait). A flag, not a state, so a fatal path's move to FatalEntered and
// the reload's end each keep the other's bits.
constexpr crd::u32 kStateBits = 3U;
constexpr crd::u64 kStateMask = 3U;
constexpr crd::u64 kReloadingBit = 4U;

// Left-out events are counted in stripes of a cache line each, a thread always in the same one: a single counter was a
// line every thread wrote at every left-out site, beside `keep_from`, which every site reads (DIAG.11b: 1.2 points of
// a four-thread jobs frame on linux-gcc-relwithdebinfo).
constexpr crd::u32 kFilteredStripes = 16U;

using FilteredStripes =
    crd::containers::StaticArray<crd::containers::CacheLinePadded<std::atomic<crd::u64>>, kFilteredStripes>;

void count_filtered(FilteredStripes& stripes) noexcept
{
    static std::atomic<crd::u32> next{0U};
    thread_local const crd::u32 kStripe = next.fetch_add(1U, std::memory_order_relaxed) % kFilteredStripes;
    stripes[kStripe].value.fetch_add(1U, std::memory_order_relaxed);
}

// A slot no claim has published since its ring was allocated.
constexpr crd::u64 kSlotEmpty = ~crd::u64{0};
static_assert(alignof(FlightEvent) >= std::atomic_ref<crd::u64>::required_alignment,
              "a slot's sequence is its publication stamp");

constexpr cont::StringView kNoRecipe{"no replay recipe was registered with the flight recorder"};

// The fatal path waits a bounded number of yields, shared by the collection lock and the claims still publishing: it
// must not block forever behind a thread that will never go on.
constexpr crd::u32 kFatalWaitAttempts = 2000U;

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

// Copy a source path into a fixed buffer of `cap` bytes. A path longer than cap - 1 keeps its tail, which names the
// file, after a "..." marker (the head is the least telling part of an absolute path); the cut never splits a UTF-8
// sequence. Returns the stored length.
crd::u16 copy_path_tail(char* dst, crd::u32 cap, cont::StringView src) noexcept
{
    const crd::usize limit = static_cast<crd::usize>(cap) - 1U;
    if (src.size() <= limit)
    {
        return copy_clipped(dst, cap, src, nullptr);
    }
    constexpr crd::usize marker = 3U;
    crd::usize           from   = src.size() - (limit - marker);
    while (from < src.size() && (static_cast<crd::u8>(src[from]) & 0xC0U) == 0x80U)
    {
        ++from;
    }
    const crd::usize n = src.size() - from;
    std::memcpy(dst, "...", marker);
    if (n > 0U)
    {
        std::memcpy(dst + marker, src.data() + from, n);
    }
    dst[marker + n] = '\0';
    return static_cast<crd::u16>(marker + n);
}

bool known_trigger(crd::u32 kind) noexcept
{
    return kind > 0U && kind < kFlightTriggerCount;
}

constexpr crd::u8 kFlightMaxDetail = static_cast<crd::u8>(FlightDetail::Basic);
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
    case FlightTrigger::InjectedFault:     return cont::StringView{"injected-fault"};
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

cont::StringView flight_detail_name(FlightDetail d) noexcept
{
    switch (d)
    {
    case FlightDetail::Full:  return cont::StringView{"full"};
    case FlightDetail::Basic: return cont::StringView{"basic"};
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
        (p.trigger_mask & ~kFlightAllTriggers) != 0U || static_cast<crd::u8>(p.detail) > kFlightMaxDetail)
    {
        return FlightPolicyStatus::RejectedOutOfBounds;
    }
    return FlightPolicyStatus::Committed;
}

struct FlightRecorder::Impl
{
    // The recording state is one word: the total recorded above kStateBits, the state below. record() claims a
    // sequence with one compare-exchange of it, only while Armed or Triggered, so no event is claimed after the window
    // closed and the trigger's boundary is exact in claim order. The record path takes no lock (DIAG.11b).
    std::atomic<crd::u64> word{kStateArmed};
    // Where the open post window ends (the trigger's sequence plus post_events); the claim that reaches it moves the
    // state to Complete in the same exchange. trigger() writes it before its exchange publishes Triggered.
    std::atomic<crd::u64> post_end{0U};
    // The ring. A slot's `sequence` is its publication stamp: kSlotEmpty, or the sequence whose payload the slot holds,
    // stored with release after the payload. The ring, `base` and the policy change only in reload_policy(), once
    // every claimed slot is published and further claims wait.
    cont::Array<FlightEvent> ring;
    crd::u64                 base = 0U; // the total at the ring's allocation: sequence s is slot (s - base) % capacity
    std::atomic<crd::u64>    dropped_while_complete{0U};
    // The lowest severity the active detail keeps, as its u8 value, and the events the detail left out (DIAG.11b).
    std::atomic<crd::u8>     keep_from{0U};
    FilteredStripes          filtered{};

    // Collection: poll(), take_capture(), finish() and reload_policy() hold collect_mutex; record() never takes it.
    // `end` is how a collector closed the window; None means the claim that reached post_end closed it.
    std::mutex collect_mutex;
    FlightEnd end = FlightEnd::None;
    crd::u64 end_ns = 0U;

    // The trigger and the replay recipe: trigger_mutex. The trigger record is written only while Armed, before the
    // exchange that publishes Triggered, so a thread that observes Triggered or Complete reads it safely.
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

    // The policy is written under both mutexes with no claim in flight, so a holder of either reads it safely, and so
    // does a claim holder (its capacity).
    FlightPolicy policy;
    crd::u32     policy_generation = 1U;

    std::atomic<FlightCaptureSink> sink{nullptr};
    std::atomic<void*>             sink_user{nullptr};
    std::atomic<crd::u64>          delivered{0U};
};

namespace
{
using Impl = FlightRecorder::Impl;

crd::u32 state_of(crd::u64 word) noexcept
{
    return static_cast<crd::u32>(word & kStateMask);
}

crd::u64 total_of(crd::u64 word) noexcept
{
    return word >> kStateBits;
}

crd::u64 make_word(crd::u64 total, crd::u32 state) noexcept
{
    return (total << kStateBits) | state;
}

// The lowest severity `detail` keeps: Full keeps every event, Basic Warning and above.
crd::u8 keep_from_of(FlightDetail detail) noexcept
{
    const Severity lowest = (detail == FlightDetail::Basic) ? Severity::Warning : Severity::Info;
    return static_cast<crd::u8>(lowest);
}

bool allocate_ring(cont::Array<FlightEvent>& ring, crd::u32 capacity) noexcept
{
    ring.clear();
    ring.resize(capacity);
    if (ring.size() != capacity)
    {
        return false;
    }
    for (FlightEvent& slot : ring)
    {
        slot.sequence = kSlotEmpty;
    }
    return true;
}

FlightEvent& slot_of(Impl& impl, crd::u64 s) noexcept
{
    return impl.ring[static_cast<crd::usize>((s - impl.base) % impl.policy.capacity)];
}

// The oldest sequence the ring still holds when `total` events were claimed.
crd::u64 oldest_held(const Impl& impl, crd::u64 total) noexcept
{
    const crd::u64 cap = impl.policy.capacity;
    return (total - impl.base > cap) ? total - cap : impl.base;
}

// Claim the next sequence into `seq`; the claim that fills the post window closes it in the same exchange. Returns
// false, counting the event, while the ring is frozen.
bool claim(Impl& impl, crd::u64& seq) noexcept
{
    crd::u64 word = impl.word.load(std::memory_order_acquire);
    for (;;)
    {
        if ((word & kReloadingBit) != 0U)
        {
            std::this_thread::yield();
            word = impl.word.load(std::memory_order_acquire);
            continue;
        }
        const crd::u32 state = state_of(word);
        if (state == kStateComplete || state == kStateFatalEntered)
        {
            // Frozen until collected (for good after a fatal): the windows stay intact.
            impl.dropped_while_complete.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        seq = total_of(word);
        const bool fills = state == kStateTriggered && seq + 1U >= impl.post_end.load(std::memory_order_relaxed);
        const crd::u64 next = make_word(seq + 1U, fills ? kStateComplete : state);
        if (impl.word.compare_exchange_weak(word, next, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return true;
        }
    }
}

// Fill the slot of claimed sequence `seq` and publish it. The slot is this claim's once its previous occupant, a full
// ring earlier, is published: the wait is reached only when the ring laps a claim stalled before its publication.
void publish(Impl& impl, crd::u64 seq, crd::u64 now, EventCode code, Severity severity, cont::StringView message,
             crd::u64 value_a, crd::u64 value_b) noexcept
{
    const crd::u64 capacity = impl.policy.capacity;
    FlightEvent& slot = slot_of(impl, seq);
    const std::atomic_ref<crd::u64> stamp(slot.sequence);
    const crd::u64 previous = (seq - impl.base >= capacity) ? seq - capacity : kSlotEmpty;
    while (stamp.load(std::memory_order_acquire) != previous)
    {
        std::this_thread::yield();
    }
    slot.timestamp_ns = now;
    slot.value_a = value_a;
    slot.value_b = value_b;
    slot.code = code;
    slot.severity = severity;
    slot.message_length = copy_clipped(slot.message, kFlightMessageBytes, message, &slot.message_truncated);
    stamp.store(seq, std::memory_order_release);
}

// Wait until the claim of sequence `s` published its slot. `budget` (yields left, shared by the caller's waits)
// bounds the fatal path; nullptr waits as long as the claim holder takes. Returns false when the budget ran out.
bool wait_published(Impl& impl, crd::u64 s, crd::u32* budget) noexcept
{
    const std::atomic_ref<crd::u64> stamp(slot_of(impl, s).sequence);
    while (stamp.load(std::memory_order_acquire) != s)
    {
        if (budget != nullptr)
        {
            if (*budget == 0U)
            {
                return false;
            }
            --*budget;
        }
        std::this_thread::yield();
    }
    return true;
}

// With the trigger mutex held: the capture of the trigger alone, said so (the fatal path could not read the ring).
// Terminal, as after any Fatal capture.
void trigger_only_capture(Impl& impl, FlightCapture& out, crd::u64 now) noexcept
{
    out.trigger = impl.trigger;
    out.end = FlightEnd::Fatal;
    out.end_ns = now;
    out.policy = impl.policy;
    out.policy_generation = impl.policy_generation;
    out.recorded_before_trigger = impl.trigger.sequence;
    out.suppressed_triggers = impl.suppressed;
    out.suppressed_kinds = impl.suppressed_kinds;
    out.replay_available = impl.trigger_replay_available;
    out.replay.clear();
    out.replay.append(cont::StringView{impl.trigger_replay, impl.trigger_replay_length});
    out.events.clear();
    out.pre_retained = 0U;
    out.post_retained = 0U;
    out.dropped_while_complete = 0U;
    out.ring_unavailable = true;
    out.post_truncated = true;
    // From Triggered (1) or Complete (2) to FatalEntered (3); record() and the collectors leave that state alone.
    impl.word.fetch_or(kStateFatalEntered, std::memory_order_acq_rel);
}

// With the collect mutex held: when the state is Complete, build `out` and re-arm. `budget` bounds the wait for
// claims still publishing (the fatal path); when it runs out, `out` is the trigger alone. Returns false when nothing
// is complete.
bool build_capture(Impl& impl, FlightCapture& out, crd::u32* budget) noexcept
{
    const crd::u64 word = impl.word.load(std::memory_order_acquire);
    if (state_of(word) != kStateComplete)
    {
        return false;
    }
    const crd::u64 total = total_of(word); // no claim moves it while Complete

    std::lock_guard<std::mutex> trigger_guard(impl.trigger_mutex);
    const crd::u64 trig = impl.trigger.sequence;
    const crd::u64 want = (trig > impl.policy.pre_events) ? trig - impl.policy.pre_events : 0U;
    const crd::u64 old = oldest_held(impl, total);
    const crd::u64 first = (want > old) ? want : old;
    for (crd::u64 s = first; s < total; ++s)
    {
        if (!wait_published(impl, s, budget))
        {
            trigger_only_capture(impl, out, diagnostic_now_ns());
            return true;
        }
    }

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
    out.recorded_before_trigger = trig;
    out.pre_retained            = 0U;
    out.post_retained           = 0U;
    out.ring_unavailable        = false;
    out.events.clear();
    out.events.reserve(static_cast<crd::usize>(total - first));
    for (crd::u64 s = first; s < total; ++s)
    {
        out.events.push_back(slot_of(impl, s));
        if (s < trig)
        {
            ++out.pre_retained;
        }
        else
        {
            ++out.post_retained;
        }
    }
    if (out.end == FlightEnd::None) // the claim that filled the post window closed it, at its own time
    {
        out.end = FlightEnd::PostEventsFull;
        out.end_ns = out.events.empty() ? 0U : out.events[out.events.size() - 1U].timestamp_ns;
    }
    out.post_truncated = (out.end == FlightEnd::Shutdown || out.end == FlightEnd::Fatal) &&
                         out.post_retained < impl.policy.post_events;

    // Re-arm, unless the capture ended Fatal (terminal). A drop is counted once, by the first capture collected after
    // it: one whose thread saw the ring frozen but counted only after this exchange belongs to the next capture.
    impl.end = FlightEnd::None;
    impl.end_ns = 0U;
    impl.suppressed = 0U;
    impl.suppressed_kinds = 0U;
    crd::u64 expected = word;
    (void)impl.word.compare_exchange_strong(
        expected, make_word(total, out.end == FlightEnd::Fatal ? kStateFatalEntered : kStateArmed),
        std::memory_order_acq_rel, std::memory_order_acquire);
    out.dropped_while_complete = impl.dropped_while_complete.exchange(0U, std::memory_order_relaxed);
    return true;
}

// With the collect mutex held: close an open window with `reason`. A claim that fills the window at the same time
// wins, and the capture ends PostEventsFull.
void close_window(Impl& impl, FlightEnd reason, crd::u64 now_ns) noexcept
{
    crd::u64 word = impl.word.load(std::memory_order_acquire);
    while (state_of(word) == kStateTriggered)
    {
        if (impl.word.compare_exchange_weak(word, make_word(total_of(word), kStateComplete), std::memory_order_acq_rel,
                                            std::memory_order_acquire))
        {
            impl.end = reason;
            impl.end_ns = now_ns;
            return;
        }
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
    impl->keep_from.store(keep_from_of(policy.detail), std::memory_order_release);
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

    std::lock_guard<std::mutex> collect_guard(impl->collect_mutex);
    std::lock_guard<std::mutex> trigger_guard(impl->trigger_mutex);
    crd::u64 word = impl->word.load(std::memory_order_acquire);
    if (state_of(word) != kStateArmed)
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
    // Further claims wait, and every claimed slot of the old ring is published, so no claim holder touches the ring,
    // `base` or the capacity while they change. The flag leaves the state alone and the total no longer moves.
    word = impl->word.fetch_or(kReloadingBit, std::memory_order_acq_rel);
    const crd::u64 total = total_of(word);
    for (crd::u64 s = oldest_held(*impl, total); s < total; ++s)
    {
        (void)wait_published(*impl, s, nullptr);
    }
    impl->ring   = static_cast<cont::Array<FlightEvent>&&>(ring);
    impl->base   = total;
    impl->policy = candidate;
    impl->keep_from.store(keep_from_of(candidate.detail), std::memory_order_release);
    ++impl->policy_generation;
    impl->word.fetch_and(~kReloadingBit, std::memory_order_acq_rel);
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
    // Outside the active detail's scope: not recorded, not in the sequence, counted, and decided before the clock is
    // read or a sequence claimed.
    if (static_cast<crd::u8>(severity) < impl->keep_from.load(std::memory_order_acquire))
    {
        count_filtered(impl->filtered);
        return;
    }
    const crd::u64 now = diagnostic_now_ns();
    crd::u64 seq = 0U;
    if (claim(*impl, seq))
    {
        publish(*impl, seq, now, code, severity, message, value_a, value_b);
    }
}

void FlightRecorder::record(const DiagnosticEvent& e) noexcept
{
    record(e.code, e.severity, cont::StringView{e.message.c_str(), e.message.size()}, 0U, 0U);
}

bool FlightRecorder::admit(Severity severity) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return false;
    }
    if (static_cast<crd::u8>(severity) < impl->keep_from.load(std::memory_order_acquire))
    {
        count_filtered(impl->filtered);
        return false;
    }
    return true;
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
    // Only claims move the word while Armed and this mutex is held (a reload holds it too).
    crd::u64 word = impl->word.load(std::memory_order_acquire);
    if (state_of(word) != kStateArmed)
    {
        ++impl->suppressed;
        impl->suppressed_kinds |= flight_trigger_bit(input.kind);
        return FlightTriggerResult::Suppressed;
    }

    FlightTriggerRecord& t = impl->trigger;
    t.kind                 = input.kind;
    t.code                 = input.code;
    t.timestamp_ns         = now;
    t.value_a              = input.value_a;
    t.value_b              = input.value_b;
    t.line                 = input.source.line;
    t.file_length          = copy_path_tail(t.file, kFlightTriggerFileBytes, input.source.file);
    t.message_length       = copy_clipped(t.message, kFlightTriggerMessageBytes, input.message, nullptr);

    std::memcpy(impl->trigger_replay, impl->replay, sizeof(impl->trigger_replay));
    impl->trigger_replay_length    = impl->replay_length;
    impl->trigger_replay_available = impl->replay_available;

    // The exchange fixes the boundary: every claim before it is in the pre window, every claim after it sees
    // Triggered and is in the post window.
    do
    {
        t.sequence = total_of(word);
        impl->post_end.store(t.sequence + impl->policy.post_events, std::memory_order_relaxed);
    } while (!impl->word.compare_exchange_weak(word, make_word(total_of(word), kStateTriggered),
                                               std::memory_order_acq_rel, std::memory_order_acquire));
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
        std::lock_guard<std::mutex> guard(impl->collect_mutex);
        if (state_of(impl->word.load(std::memory_order_acquire)) == kStateTriggered &&
            impl->policy.post_window_ns > 0U && now_ns - impl->trigger.timestamp_ns >= impl->policy.post_window_ns)
        {
            close_window(*impl, FlightEnd::PostWindowElapsed, now_ns);
        }
        if (impl->sink.load(std::memory_order_acquire) == nullptr)
        {
            return state_of(impl->word.load(std::memory_order_acquire)) == kStateComplete; // left for take_capture()
        }
        built = build_capture(*impl, capture, nullptr);
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
    std::lock_guard<std::mutex> guard(impl->collect_mutex);
    const bool built = build_capture(*impl, out, nullptr);
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
    crd::u32       budget  = kFatalWaitAttempts;
    if (reason == FlightEnd::Fatal)
    {
        while (budget > 0U && !locked)
        {
            --budget;
            locked = impl->collect_mutex.try_lock();
            if (!locked)
            {
                std::this_thread::yield();
            }
        }
    }
    else
    {
        impl->collect_mutex.lock();
        locked = true;
    }

    if (locked)
    {
        close_window(*impl, reason, now);
        built = build_capture(*impl, capture, (reason == FlightEnd::Fatal) ? &budget : nullptr);
        impl->collect_mutex.unlock();
    }
    else if (const crd::u32 state = state_of(impl->word.load(std::memory_order_acquire));
             state == kStateTriggered || state == kStateComplete)
    {
        // A collector stays in the way: deliver the trigger alone, said so, rather than nothing.
        std::lock_guard<std::mutex> trigger_guard(impl->trigger_mutex);
        trigger_only_capture(*impl, capture, now);
        built = true;
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
    return (impl != nullptr) ? total_of(impl->word.load(std::memory_order_acquire)) : 0U;
}

crd::u64 FlightRecorder::total_filtered() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return 0U;
    }
    crd::u64 total = 0U;
    for (const auto& stripe : impl->filtered)
    {
        total += stripe.value.load(std::memory_order_relaxed);
    }
    return total;
}

bool FlightRecorder::capture_pending() const noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr)
    {
        return false;
    }
    const crd::u32 state = state_of(impl->word.load(std::memory_order_acquire));
    return state == kStateTriggered || state == kStateComplete;
}

bool FlightRecorder::fatal_entered() const noexcept
{
    Impl* impl = m_impl;
    return impl != nullptr && state_of(impl->word.load(std::memory_order_acquire)) == kStateFatalEntered;
}

crd::u64 FlightRecorder::captures_delivered() const noexcept
{
    Impl* impl = m_impl;
    return (impl != nullptr) ? impl->delivered.load(std::memory_order_relaxed) : 0U;
}

#if CRD_ENABLE_ASSERTS
bool FlightRecorder::test_set_policy_generation(crd::u32 generation) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr || generation == 0U)
    {
        return false;
    }
    std::lock_guard<std::mutex> collect_guard(impl->collect_mutex);
    std::lock_guard<std::mutex> trigger_guard(impl->trigger_mutex);
    impl->policy_generation = generation;
    return true;
}

bool FlightRecorder::test_with_ring_held(void (*fn)(void* user), void* user) noexcept
{
    Impl* impl = m_impl;
    if (impl == nullptr || fn == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> collect_guard(impl->collect_mutex);
    fn(user);
    return true;
}

bool FlightRecorder::test_with_claim_unpublished(void (*fn)(void* user), void* user) noexcept
{
    Impl* impl = m_impl;
    crd::u64 seq = 0U;
    if (impl == nullptr || fn == nullptr || !claim(*impl, seq))
    {
        return false;
    }
    fn(user);
    publish(*impl, seq, diagnostic_now_ns(), kFlightCodeManual, Severity::Info, cont::StringView{"claim held"}, seq,
            0U);
    return true;
}
#endif

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
    put_u8(out, static_cast<crd::u8>(c.policy.detail)); // the pad of records before detail levels: 0 is Full

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
    const crd::u8  flags  = in.u8();
    const crd::u8  detail = in.u8();
    if (!in.ok || !known_trigger(kind) || end > static_cast<crd::u8>(FlightEnd::Fatal) || end == 0U ||
        (flags & ~kRecordKnownFlags) != 0U)
    {
        return refuse(r, "bad trigger kind, end reason or flags");
    }
    if (detail > kFlightMaxDetail)
    {
        return refuse(r, "unknown detail level");
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
    c.policy.detail             = static_cast<FlightDetail>(detail);
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
    // What a recorder writes and nothing else (DIAG.10a: a record that contradicts itself is not evidence). trigger()
    // starts or suppresses only a kind the policy arms, and counts every suppression with its kind; build_capture() and
    // finish() derive the end and the truncation from the post window: it ends post-events-full exactly when
    // post_events events landed, it can elapse only under a time bound, and a capture without the ring is a fatal one
    // with no events and no drop count.
    if ((c.policy.trigger_mask & flight_trigger_bit(c.trigger.kind)) == 0U ||
        (c.suppressed_kinds & ~c.policy.trigger_mask) != 0U ||
        (c.suppressed_triggers == 0U) != (c.suppressed_kinds == 0U))
    {
        return refuse(r, "the trigger or a suppressed kind is not armed by the recorded policy, or the suppression "
                         "count disagrees with its kinds");
    }
    const bool ended_early = c.end == FlightEnd::Shutdown || c.end == FlightEnd::Fatal;
    if ((c.end == FlightEnd::PostEventsFull) != (c.post_retained == c.policy.post_events) ||
        c.post_truncated != (ended_early && c.post_retained < c.policy.post_events) ||
        (c.end == FlightEnd::PostWindowElapsed && c.policy.post_window_ns == 0U))
    {
        return refuse(r, "the end reason disagrees with the post window");
    }
    if (c.ring_unavailable &&
        (c.end != FlightEnd::Fatal || c.pre_retained != 0U || c.post_retained != 0U || c.dropped_while_complete != 0U))
    {
        return refuse(r, "a capture without the ring must be fatal and hold no events");
    }

    char           replay[kFlightRecipeBytes] = {};
    const crd::u16 replay_n                   = in.u16();
    if (!in.text(replay, kFlightRecipeBytes, replay_n))
    {
        return refuse(r, "replay text over its bound or past the payload");
    }
    if (!c.replay_available && replay_n == 0U)
    {
        return refuse(r, "replay unavailable without its reason");
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
        // The capture copies one contiguous run of the ring, from pre_retained before the trigger's sequence on.
        if (e.sequence != c.trigger.sequence - c.pre_retained + i)
        {
            return refuse(r, "event sequence not contiguous across the trigger");
        }
        // A clipped message fills its buffer; a Basic policy records Warning and above only.
        if ((e.message_truncated && e.message_length != kFlightMessageBytes - 1U) ||
            (c.policy.detail == FlightDetail::Basic && e.severity < Severity::Warning))
        {
            return refuse(r, "an event the recorder could not have written (clip flag or detail level)");
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

cont::Array<crd::u8> write_flight_bundle(const FlightCapture& capture, cont::ConstSpan<ModuleIdentity> modules)
{
    BundleManifest manifest;
    manifest.schema_version = kDiagnosticSchemaVersion;
    if (modules.empty())
    {
        manifest.absent_tags.push_back(static_cast<crd::u32>(BundleSectionTag::SymbolIndex));
    }
    const cont::Array<crd::u8> manifest_bytes = serialize_manifest(manifest);
    const cont::Array<crd::u8> record_bytes   = encode_flight_record(capture);
    const cont::Array<crd::u8> build_bytes    = encode_build_identity(current_build_identity());

    BundleWriter writer;
    (void)writer.add_section(BundleSectionTag::Manifest,
                             cont::ConstSpan<crd::u8>{manifest_bytes.data(), manifest_bytes.size()});
    (void)writer.add_section(BundleSectionTag::FlightRecord,
                             cont::ConstSpan<crd::u8>{record_bytes.data(), record_bytes.size()});
    (void)writer.add_section(BundleSectionTag::BuildIdentity,
                             cont::ConstSpan<crd::u8>{build_bytes.data(), build_bytes.size()});
    if (modules.empty())
    {
        (void)writer.add_absent(BundleSectionTag::SymbolIndex);
    }
    else
    {
        const cont::Array<crd::u8> symbol_bytes = serialize_symbol_index(modules);
        (void)writer.add_section(BundleSectionTag::SymbolIndex,
                                 cont::ConstSpan<crd::u8>{symbol_bytes.data(), symbol_bytes.size()});
    }
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
    key("detail", true);
    str(flight_detail_name(c.policy.detail));
    key("replay_available", true);
    out.append(c.replay_available ? "true" : "false");
    key("replay", true);
    str(cont::StringView{c.replay.c_str(), c.replay.size()});
    out.push_back('}');
}

} // namespace crd::perf
