#pragma once

// crd-perf -- triggerable bounded flight recording (DIAG.9b).
//
// A FlightRecorder keeps the most recent events in a preallocated ring of fixed-size records. When a trigger fires
// (an assertion, a fatal invariant, an allocation failure, a frame hitch, queue starvation, a hang, resource pressure,
// an injected fault or a manual request) it keeps the window before the trigger and records a bounded window after
// it, then freezes into one capture: the trigger and its causal inputs, the events from before and after it, the loss
// metadata and a replay recipe (or the precise reason replay is unavailable).
//
// Bounds and the post-window split. The ring holds `capacity` events. A policy declares `pre_events` and
// `post_events` with pre_events + post_events <= capacity, so the events that make up the post window can never
// overwrite the events the pre window keeps. The post window ends when `post_events` events landed after the trigger,
// when `post_window_ns` elapsed (checked by poll()), at shutdown, or on the fatal path. Shutdown and the fatal path
// end it early; the capture then says which, and that the post window is truncated.
//
// Recording and triggering never allocate. record() takes no lock (DIAG.11b): it claims the next sequence with one
// compare-exchange of the recorder's recording word (total and state), copies into that sequence's ring slot and
// publishes the slot with a release store; it waits only when the ring laps a claim stalled between its claim and its
// publication, or while a policy reload swaps the ring. An event the policy's detail leaves out is counted and dropped
// before the clock is read or a sequence claimed. CRD_FLIGHT_RECORD (below) is the site form: its message and scalars
// are not evaluated when no recorder runs or the detail leaves the event out. trigger() takes only its own mutex,
// never the collection lock, so a trigger fired on a thread that something else is blocking (or from a handler that
// runs while another thread records) still lands. While a capture is pending (the post window is open,
// or it is complete and not yet collected), a further trigger is suppressed and counted, with its kind; it never
// replaces the first. Events that arrive after the post window closed and before the capture is collected are not
// recorded (the ring is frozen so the windows stay intact) and are counted.
//
// Collection. poll() (the host calls it, e.g. once per frame) closes a post window whose time ran out and delivers a
// complete capture to the sink; take_capture() does the same without a sink. finish() closes a pending window early
// (Shutdown or Fatal) and delivers it. A capture that ended Fatal is terminal: the recorder does not re-arm, so a
// second fatal path in the same failure (check_invariant's default reaches CRD_FATAL, which reaches the assert
// trigger) cannot replace the capture already delivered. shutdown() calls finish(Shutdown) first, so a pending
// capture is never lost to teardown while a sink is set. Building a capture allocates (its event array), so
// collection happens on the host's thread or on the fatal path, which is an ordinary function call (an assert handler,
// a fatal invariant handler), never a signal handler. A hard signal crash leaves only the DIAG.2b emergency record and
// the DIAG.5a/5b crash record.
//
// The policy is validated before it takes effect; a rejected candidate leaves the active policy, its generation and
// the ring untouched (last known good). Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include <crd/perf/diagnostics.hpp>
#include <crd/perf/symbol_index.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/build_config.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::perf
{

// Foundation event codes the flight recorder and its trigger sources use (DIAG.9b owns 0x0900..0x09FF).
inline constexpr EventCode kFlightCodeManual            = 0x0900U;
inline constexpr EventCode kFlightCodeAssertion         = 0x0901U;
inline constexpr EventCode kFlightCodeAllocationFailure = 0x0903U;
inline constexpr EventCode kFlightCodeFrameHitch        = 0x0904U;
inline constexpr EventCode kFlightCodeQueueStarvation   = 0x0905U;
inline constexpr EventCode kFlightCodeHang              = 0x0906U;
inline constexpr EventCode kFlightCodeResourcePressure  = 0x0907U;
inline constexpr EventCode kFlightCodeFaultInjected     = 0x0908U; // a seeded fault fired (a = FaultSite, b = hit)
inline constexpr EventCode kFlightCodeFrame             = 0x0910U; // one frame boundary (a = interval, b = duration)

// What fired a trigger. A fatal invariant keeps its own event code; the others use the codes above.
enum class FlightTrigger : crd::u8
{
    None              = 0U,
    Manual            = 1U,
    Assertion         = 2U,
    FatalInvariant    = 3U,
    AllocationFailure = 4U,
    FrameHitch        = 5U,
    QueueStarvation   = 6U,
    Hang              = 7U,
    ResourcePressure  = 8U,
    InjectedFault     = 9U, // a FaultInjector fault fired (crd/perf/fault_injection.hpp)
};

inline constexpr crd::u32 kFlightTriggerCount = 10U;

// "none", "manual", "assertion", "fatal-invariant", "allocation-failure", "frame-hitch", "queue-starvation", "hang",
// "resource-pressure", "injected-fault".
[[nodiscard]] cont::StringView flight_trigger_name(FlightTrigger t) noexcept;

[[nodiscard]] constexpr crd::u32 flight_trigger_bit(FlightTrigger t) noexcept
{
    return 1U << static_cast<crd::u32>(t);
}

// Every real trigger kind (None excluded).
inline constexpr crd::u32 kFlightAllTriggers = 0x3FEU;

// Why a post window ended.
enum class FlightEnd : crd::u8
{
    None              = 0U, // the window is still open
    PostEventsFull    = 1U, // post_events events landed after the trigger
    PostWindowElapsed = 2U, // post_window_ns elapsed before post_events landed
    Shutdown          = 3U, // the recorder shut down while the window was open
    Fatal             = 4U, // a fatal path was entered while the window was open
};

// "open", "post-events-full", "post-window-elapsed", "shutdown", "fatal".
[[nodiscard]] cont::StringView flight_end_name(FlightEnd e) noexcept;

// Fixed sizes. An event stays under the 256-byte payload budget (sizeof(FlightEvent) == 152).
inline constexpr crd::u32 kFlightMessageBytes        = 112U;
inline constexpr crd::u32 kFlightTriggerFileBytes    = 128U;
inline constexpr crd::u32 kFlightTriggerMessageBytes = 192U;
inline constexpr crd::u32 kFlightRecipeBytes         = 256U;
inline constexpr crd::u32 kFlightMaxEvents           = 65536U; // hard capacity bound: 65536 * 152 B ~ 9.5 MiB
inline constexpr crd::u64 kFlightMaxPostWindowNs     = 60ULL * 1000ULL * 1000ULL * 1000ULL;

// One recorded event. Fixed size, no owned storage: copying it never allocates. `message_truncated` is set when the
// message was clipped to kFlightMessageBytes - 1 bytes; truncation is never silent.
struct FlightEvent
{
    crd::u64  sequence     = 0U; // position in the recorder's total order (0-based, never reused)
    crd::u64  timestamp_ns = 0U; // diagnostic_now_ns() at record
    crd::u64  value_a      = 0U; // code-specific scalars (documented with each code)
    crd::u64  value_b      = 0U;
    EventCode code         = 0U;
    Severity  severity     = Severity::Info;
    bool      message_truncated = false;
    crd::u16  message_length    = 0U;
    char      message[kFlightMessageBytes] = {};

    [[nodiscard]] cont::StringView message_view() const noexcept { return {message, message_length}; }
};

static_assert(sizeof(FlightEvent) <= 256U, "a flight event stays within the 256-byte event payload budget");

// The trigger and its causal inputs, captured when the trigger fires, before the faulty operation completes (or
// fails to). `sequence` is the boundary in the recorder's own order: the trigger moves the recording word to Triggered
// in one exchange, so every event claimed before it is in the pre window and every event claimed after it in the post
// window (an event racing the trigger lands on one side, never both).
struct FlightTriggerRecord
{
    FlightTrigger kind         = FlightTrigger::None;
    EventCode     code         = 0U;
    crd::u64      sequence     = 0U; // events recorded before the trigger; the first post-window event has this
    crd::u64      timestamp_ns = 0U;
    crd::u64      value_a      = 0U; // trigger-specific scalars (see each trigger source)
    crd::u64      value_b      = 0U;
    crd::u32      line         = 0U;
    // The source file. A path longer than kFlightTriggerFileBytes - 1 keeps its tail (the file name) after "...".
    crd::u16      file_length    = 0U;
    crd::u16      message_length = 0U;
    char          file[kFlightTriggerFileBytes]       = {};
    char          message[kFlightTriggerMessageBytes] = {};

    [[nodiscard]] cont::StringView file_view() const noexcept { return {file, file_length}; }
    [[nodiscard]] cont::StringView message_view() const noexcept { return {message, message_length}; }
};

// The authorable flight policy (versioned; validated before it takes effect).
inline constexpr crd::u32 kFlightPolicyVersion = 1U;

// How much an initialised recorder keeps (DIAG.9c). The third level, off, is no recorder at all: nothing records and
// no trigger fires (record() and trigger() before init are no-ops), which is the natural, uninstrumented run.
//   Full   every event record() receives (the recorder's behaviour before detail levels existed).
//   Basic  events of Severity::Warning and above only: anomalies, failures and injected faults. Informational events
//          (a frame boundary from FlightFrameWatch is one) are not recorded and are counted in total_filtered();
//          triggers fire exactly as at Full, so a hitch still opens a window, carrying its interval in the trigger.
//          Events a Basic policy leaves out are its declared scope, not loss: a capture's policy says which level it
//          was recorded at, and post_events counts only the events the level keeps.
// Full is 0, so a flight record written before detail levels existed reads as Full.
enum class FlightDetail : crd::u8
{
    Full  = 0U,
    Basic = 1U,
};

// "full", "basic".
[[nodiscard]] cont::StringView flight_detail_name(FlightDetail d) noexcept;

struct FlightPolicy
{
    crd::u32 schema_version     = kFlightPolicyVersion;
    crd::u32 capacity           = 1024U;      // ring events (1024 * 152 B = 152 KiB)
    crd::u32 pre_events         = 768U;       // kept from before the trigger; pre + post <= capacity
    crd::u32 post_events        = 256U;       // recorded after the trigger at most
    crd::u64 post_window_ns     = 2000000000ULL; // the post window's time bound (0: bounded by events only)
    crd::u32 trigger_mask       = kFlightAllTriggers;
    crd::u64 hitch_threshold_ns = 50000000ULL; // a frame interval above this is a hitch (50 ms)
    FlightDetail detail         = FlightDetail::Full;
};

enum class FlightPolicyStatus : crd::u8
{
    Committed,              // validated and active at a new generation
    RejectedUnknownVersion, // schema_version is not kFlightPolicyVersion
    RejectedOutOfBounds,    // a bound is zero, over its hard limit, or pre + post exceeds the capacity
    RejectedBusy,           // a capture is pending; the active policy stays until it is collected
    RejectedNotInitialized, // reload before init
    RejectedOutOfMemory,    // the ring could not be allocated (a reload keeps the old ring)
    RejectedGenerationWrap, // the policy generation would wrap; the active policy stays
};

[[nodiscard]] cont::StringView flight_policy_status_name(FlightPolicyStatus s) noexcept;

// Validate a candidate on its own (no recorder needed). Committed means it would be accepted.
[[nodiscard]] FlightPolicyStatus validate_flight_policy(const FlightPolicy& p) noexcept;

// One frozen capture.
struct FlightCapture
{
    FlightTriggerRecord trigger;
    FlightEnd           end    = FlightEnd::None;
    crd::u64            end_ns = 0U;

    FlightPolicy policy;                // the policy in effect at the trigger
    crd::u32     policy_generation = 0U;

    // Loss metadata. recorded_before_trigger = trigger.sequence; of those, pre_retained are kept and
    // recorded_before_trigger - pre_retained fell outside the pre window (overwritten or beyond pre_events).
    crd::u64 recorded_before_trigger = 0U;
    crd::u32 pre_retained            = 0U;
    crd::u32 post_retained           = 0U;
    bool     post_truncated          = false; // the window ended (Shutdown / Fatal) before post_events landed
    bool     ring_unavailable        = false; // the fatal path could not read the ring; events are absent
    crd::u64 dropped_while_complete  = 0U;    // events not recorded between the window closing and collection
    crd::u64 suppressed_triggers     = 0U;    // triggers that fired while this capture was pending
    crd::u32 suppressed_kinds        = 0U;    // their kinds, as flight_trigger_bit() flags

    // Replay: the recipe registered when the trigger fired, or the precise reason replay is unavailable.
    bool         replay_available = false;
    cont::String replay;

    // pre_retained events from before the trigger, then post_retained events after it, in sequence order.
    cont::Array<FlightEvent> events;
};

// Called with a complete capture by poll() and finish(); runs on the caller's thread, outside every recorder lock.
using FlightCaptureSink = void (*)(const FlightCapture& capture, void* user);

struct FlightTriggerInput
{
    FlightTrigger    kind = FlightTrigger::Manual;
    EventCode        code = kFlightCodeManual;
    cont::StringView message;
    crd::u64         value_a = 0U;
    crd::u64         value_b = 0U;
    SourceIdentity   source;
};

enum class FlightTriggerResult : crd::u8
{
    Started,        // the trigger was recorded and the post window opened
    Suppressed,     // a capture is pending; counted in its suppressed_triggers
    Masked,         // the active policy does not arm this trigger kind
    NotInitialized, // before init / after shutdown (a no-op, never a fault)
};

class FlightRecorder
{
public:
    FlightRecorder() = default;
    ~FlightRecorder() { shutdown(); }

    FlightRecorder(const FlightRecorder&)            = delete;
    FlightRecorder& operator=(const FlightRecorder&) = delete;

    // Validate `policy`, preallocate the ring and arm. Returns RejectedBusy if already initialised.
    FlightPolicyStatus init(const FlightPolicy& policy = FlightPolicy{}) noexcept;

    // Deliver a pending capture to the sink (finish(Shutdown)), then release the ring. Without a sink a pending capture
    // is dropped with the recorder. Safe to call twice.
    void shutdown() noexcept;
    [[nodiscard]] bool is_initialized() const noexcept;

    // Replace the active policy while no capture is pending. The ring is reallocated and cleared (its events belong to
    // the old bounds); a rejected candidate changes nothing.
    FlightPolicyStatus reload_policy(const FlightPolicy& candidate) noexcept;
    [[nodiscard]] FlightPolicy policy() const noexcept;
    [[nodiscard]] crd::u32     policy_generation() const noexcept;

    // The sink poll() and finish() deliver complete captures to (nullptr: take_capture() only).
    void set_sink(FlightCaptureSink sink, void* user) noexcept;

    // The replay recipe a capture carries when a trigger fires (clipped to kFlightRecipeBytes - 1). Until one is set,
    // or after set_replay_unavailable(), a capture carries the reason replay is unavailable instead.
    void set_replay_recipe(cont::StringView recipe) noexcept;
    void set_replay_unavailable(cont::StringView reason) noexcept;

    // Record one event (thread-safe, no allocation). A no-op before init and after shutdown.
    void record(EventCode code, Severity severity, cont::StringView message, crd::u64 value_a = 0U,
                crd::u64 value_b = 0U) noexcept;
    void record(const DiagnosticEvent& e) noexcept;

    // Whether record() keeps an event of `severity` under the active detail (DIAG.11b). Lock-free: one atomic load, no
    // clock read. Before init it answers false and counts nothing; when the detail leaves the event out it counts it in
    // total_filtered(), as record() would have, and answers false. An answer racing a policy reload may follow either
    // policy. CRD_FLIGHT_RECORD asks it before it evaluates a payload.
    [[nodiscard]] bool admit(Severity severity) noexcept;

    // Fire a trigger (thread-safe, no allocation, never takes the collection lock).
    FlightTriggerResult trigger(const FlightTriggerInput& input) noexcept;

    // Close a post window whose time bound elapsed, and deliver a complete capture to the sink. Returns true when a
    // capture was delivered (or, without a sink, completed and is waiting for take_capture()).
    bool poll(crd::u64 now_ns) noexcept;
    bool poll() noexcept { return poll(diagnostic_now_ns()); }

    // Move a complete capture into `out` and re-arm. Returns false when none is complete.
    bool take_capture(FlightCapture& out) noexcept;

    // End an open post window early with `reason` (Shutdown or Fatal) and deliver the capture. On the fatal path the
    // collection lock is taken with a bounded try-lock and claims still publishing are waited for within the same
    // bound; if either runs out, the capture carries the trigger alone and says ring_unavailable. Returns true when a
    // capture was delivered.
    bool finish(FlightEnd reason) noexcept;

    // Counters for tests and the doctor. total_filtered() counts the events a Basic policy did not record (over the
    // recorder's life, across policy reloads).
    [[nodiscard]] crd::u64 total_recorded() const noexcept;
    [[nodiscard]] crd::u64 total_filtered() const noexcept;
    [[nodiscard]] bool     capture_pending() const noexcept; // a post window is open or a capture awaits collection
    [[nodiscard]] bool     fatal_entered() const noexcept;   // a capture ended Fatal; the recorder records no more
    [[nodiscard]] crd::u64 captures_delivered() const noexcept;

#if CRD_ENABLE_ASSERTS
    // Test-only seams, compiled only where asserts are (never in a shipping build).
    //
    // Continue the policy generation from `generation` (set under both mutexes), so reload_policy()'s wrap guard is
    // reachable without 2^32 - 1 reloads. Only the counter moves. Returns false, changing nothing, before init or
    // for 0.
    [[nodiscard]] bool test_set_policy_generation(crd::u32 generation) noexcept;

    // Call `fn(user)` while this thread holds the collection lock, so a finish(Fatal) on another thread finds the
    // ring held by a collector and takes its ring_unavailable path. `fn` must not call into this recorder. Returns
    // false without calling `fn` before init or when `fn` is null.
    bool test_with_ring_held(void (*fn)(void* user), void* user) noexcept;

    // Claim the next sequence as record() does, call `fn(user)` before the claim is published, then publish an Info
    // event with code kFlightCodeManual, message "claim held" and value_a = its sequence. A collector meanwhile waits
    // for the claim, and finish(Fatal) only within its bound. Returns false without calling `fn` before init, when `fn`
    // is null or while the ring is frozen (the event is then counted as dropped).
    bool test_with_claim_unpublished(void (*fn)(void* user), void* user) noexcept;
#endif

    struct Impl; // opaque; public only so the implementation's helpers can name it

private:
    Impl* m_impl = nullptr; // pimpl keeps std::mutex out of the public header
};

// The event-site form (DIAG.11b). `recorder` is a FlightRecorder* (may be null: no recorder runs) and is evaluated
// once, as is `severity`. `message`, `value_a` and `value_b` are evaluated only when the recorder admits the event, so
// a site whose payload formats text or walks a structure costs one branch when diagnostics are off and one atomic load
// at a detail that leaves it out: a disabled site neither evaluates its payload nor changes what the program does.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): only a macro leaves its arguments unevaluated when the site is off.
#define CRD_FLIGHT_RECORD(recorder, code, severity, message, value_a, value_b)                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        ::crd::perf::FlightRecorder* const crd_flight_site_recorder_ = (recorder);                                     \
        const ::crd::perf::Severity crd_flight_site_severity_ = (severity);                                            \
        if (crd_flight_site_recorder_ != nullptr && crd_flight_site_recorder_->admit(crd_flight_site_severity_))       \
        {                                                                                                              \
            crd_flight_site_recorder_->record((code), crd_flight_site_severity_, (message), (value_a), (value_b));     \
        }                                                                                                              \
    } while (false)

// ---------------------------------------------------------------------------
// Serialization -- the FlightRecord bundle section (BundleSectionTag::FlightRecord) and a JSON summary.
// ---------------------------------------------------------------------------

// The byte after the record's flags carries the policy's detail level. Records written before detail levels existed
// hold 0 there (it was a must-be-zero pad), which is Full, the only level they had; a value that names no level is
// refused, so a reader that predates a level never misreads it.
inline constexpr crd::u32 kFlightRecordMagic   = 0x31524643U; // bytes 'C','F','R','1'
inline constexpr crd::u32 kFlightRecordVersion = 1U;

// Encode a capture as a FlightRecord section payload (little-endian, versioned; the bundle section CRC covers it).
[[nodiscard]] cont::Array<crd::u8>
encode_flight_record(const FlightCapture& capture, crd::memory::IAllocator* alloc = crd::memory::default_allocator());

struct FlightRecordRead
{
    bool             ok = false;
    cont::StringView error; // why the payload was refused (static text)
    FlightCapture    capture;
};

// Decode an untrusted FlightRecord payload. Every length is bounded by its fixed limit and by the payload size before
// anything is copied; a wrong magic or version, a short or over-long payload, or an inconsistent count is refused.
[[nodiscard]] FlightRecordRead decode_flight_record(cont::ConstSpan<crd::u8> payload);

// A complete flight bundle: a Manifest, the FlightRecord, the BuildIdentity of this build
// (crd/perf/build_identity.hpp) and a SymbolIndex of `modules` (the host's module snapshot, crd/perf/symbol_index.hpp).
// With no modules the SymbolIndex is an explicit absent section the manifest declares, never a silently missing one.
// Nothing here enumerates modules or touches the loader, so the fatal path may call it with a snapshot taken earlier.
[[nodiscard]] cont::Array<crd::u8> write_flight_bundle(const FlightCapture&           capture,
                                                       cont::ConstSpan<ModuleIdentity> modules = {});

// A one-line JSON summary of a capture (trigger, end, loss, replay; not the events).
void flight_capture_summary_json(const FlightCapture& capture, cont::String& out);

} // namespace crd::perf
