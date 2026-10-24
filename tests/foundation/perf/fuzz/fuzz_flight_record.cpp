// crd-fuzz-perf-flight: decode_flight_record -- the reader of a flight bundle's FlightRecord section -- under the
// bounded fuzz harness (DIAG.10a; docs/design/test-instruments.md). bundle.inspect and every offline consumer of a
// flight capture go through it, so a record it accepts is presented as evidence of what happened.
//
// Oracles:
//   * a refusal names its reason, and decoding the same bytes twice gives the same verdict;
//   * an accepted record is one a FlightRecorder can write: recorder_could_write() below restates, independently of
//     the decoder, what FlightRecorder::trigger(), build_capture() and finish() guarantee (the trigger and every
//     suppressed kind armed by the recorded policy, a suppression count exactly when there are suppressed kinds, one
//     contiguous run of events across the trigger, the end reason and the truncation flag derived from the post window,
//     a capture without the ring fatal and empty, a clipped message filling its buffer, a restricted event or trigger
//     holding no text, a Basic policy holding Warning and above only, a replay that is unavailable naming why);
//   * an accepted record re-encodes to exactly its own bytes (the decoder refuses trailing bytes and the encoder is
//     canonical), and its JSON summary is produced without fault.
// decode_flight_record takes no allocator: the harness budget does not bound it. Its own check that every event takes
// at least 40 payload bytes bounds the event array (at most 1,638 events in a 64 KiB input).
//
// Seeds: captures written by a real FlightRecorder (each end reason, a Basic policy, a suppressed trigger, a replay
// recipe, a clipped message, an empty post window, a restricted event and trigger) with their clock readings replaced
// by fixed values so the committed corpus is the same bytes on every lane; a capture without the ring derived the way
// finish() builds one; records that contradict themselves in exactly one way (each must be refused); and structural
// damage (truncation at several points, a trailing byte, a count past the payload, a text length at its bound, an
// unknown detail level, garbage).
#include <crd/fuzz/harness.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <cstring>
#include <utility>

namespace
{
using crd::RedactionClass;
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using namespace crd::perf;

bool terminated_at(const char* text, usize length, usize capacity) noexcept
{
    return length < capacity && text[length] == '\0';
}

// The recorder's own guarantees, restated without calling the decoder or the policy validator.
bool recorder_could_write(const FlightCapture& c) noexcept
{
    const FlightPolicy& p = c.policy;
    const u32 kind = static_cast<u32>(c.trigger.kind);
    if (kind == 0U || kind > static_cast<u32>(FlightTrigger::InjectedFault) || c.end == FlightEnd::None ||
        static_cast<u32>(c.end) > static_cast<u32>(FlightEnd::Fatal))
    {
        return false;
    }
    // A policy the recorder accepts.
    if (p.schema_version != kFlightPolicyVersion || p.capacity < 2U || p.capacity > kFlightMaxEvents ||
        p.pre_events == 0U || p.post_events == 0U || static_cast<u64>(p.pre_events) + p.post_events > p.capacity ||
        p.post_window_ns > kFlightMaxPostWindowNs || p.hitch_threshold_ns == 0U || p.trigger_mask == 0U ||
        (p.trigger_mask & ~kFlightAllTriggers) != 0U || static_cast<u32>(p.detail) > 1U)
    {
        return false;
    }
    // trigger(): only an armed kind starts a window or is suppressed, and each suppression is counted with its kind.
    if ((p.trigger_mask & (1U << kind)) == 0U || (c.suppressed_kinds & ~p.trigger_mask) != 0U ||
        (c.suppressed_triggers == 0U) != (c.suppressed_kinds == 0U))
    {
        return false;
    }
    // build_capture(): the pre window is what the ring still held of pre_events, the post window at most post_events,
    // copied as one contiguous run.
    if (c.recorded_before_trigger != c.trigger.sequence || c.pre_retained > p.pre_events ||
        c.pre_retained > c.trigger.sequence || c.post_retained > p.post_events ||
        c.events.size() != static_cast<usize>(c.pre_retained) + c.post_retained)
    {
        return false;
    }
    for (usize i = 0U; i < c.events.size(); ++i)
    {
        const FlightEvent& e = c.events[i];
        if (e.sequence != c.trigger.sequence - c.pre_retained + i ||
            static_cast<u32>(e.severity) > static_cast<u32>(Severity::FatalInvariant) ||
            !terminated_at(e.message, e.message_length, kFlightMessageBytes) ||
            (e.message_truncated && e.message_length != kFlightMessageBytes - 1U) ||
            (e.redaction == RedactionClass::Restricted && (e.message_length != 0U || e.message_truncated)) ||
            (p.detail == FlightDetail::Basic && e.severity < Severity::Warning))
        {
            return false;
        }
    }
    // The end reason: post-events-full exactly when post_events landed; elapsed only under a time bound; shutdown and
    // fatal close an open window early, which is what post_truncated says.
    const bool early = c.end == FlightEnd::Shutdown || c.end == FlightEnd::Fatal;
    if ((c.end == FlightEnd::PostEventsFull) != (c.post_retained == p.post_events) ||
        c.post_truncated != (early && c.post_retained < p.post_events) ||
        (c.end == FlightEnd::PostWindowElapsed && p.post_window_ns == 0U))
    {
        return false;
    }
    // finish() on the fatal path without the ring: the trigger alone.
    if (c.ring_unavailable && (c.end != FlightEnd::Fatal || !c.events.empty() || c.dropped_while_complete != 0U))
    {
        return false;
    }
    return terminated_at(c.trigger.file, c.trigger.file_length, kFlightTriggerFileBytes) &&
           terminated_at(c.trigger.message, c.trigger.message_length, kFlightTriggerMessageBytes) &&
           (c.trigger.redaction == RedactionClass::Public || c.trigger.message_length == 0U) &&
           c.replay.size() < kFlightRecipeBytes && (c.replay_available || !c.replay.empty());
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const ConstSpan<u8>    input(data, size);
    const FlightRecordRead a = decode_flight_record(input);
    const FlightRecordRead b = decode_flight_record(input);
    CRD_FUZZ_REQUIRE(a.ok == b.ok && a.error == b.error);
    if (!a.ok)
    {
        CRD_FUZZ_REQUIRE(!a.error.empty());
        return 0;
    }
    CRD_FUZZ_REQUIRE(a.error.empty());
    CRD_FUZZ_REQUIRE(recorder_could_write(a.capture));

    crd::fuzz::BudgetAllocator root;
    const Array<u8>            again = encode_flight_record(a.capture, &root);
    CRD_FUZZ_REQUIRE(crd::fuzz::bytes_equal(again.data(), again.size(), data, size));

    String summary;
    flight_capture_summary_json(a.capture, summary);
    CRD_FUZZ_REQUIRE(!summary.empty() && summary.data()[0] == '{' && summary.data()[summary.size() - 1U] == '}');
    return 0;
}

namespace
{
// Fixed clock readings, so a seed is the same bytes on every lane and every run.
void normalize_clock(FlightCapture& c) noexcept
{
    c.trigger.timestamp_ns = 1000U;
    c.end_ns               = 2000U;
    for (usize i = 0U; i < c.events.size(); ++i)
    {
        c.events[i].timestamp_ns = 100U + i;
    }
}

FlightPolicy small_policy(FlightDetail detail = FlightDetail::Full)
{
    FlightPolicy p;
    p.capacity    = 16U;
    p.pre_events  = 8U;
    p.post_events = 4U;
    p.detail      = detail;
    return p;
}

FlightTriggerInput manual_trigger(StringView message)
{
    FlightTriggerInput t;
    t.kind        = FlightTrigger::Manual;
    t.code        = kFlightCodeManual;
    t.message     = message;
    t.value_a     = 7U;
    t.source.file = StringView{"seed_source.cpp"};
    t.source.line = 42U;
    return t;
}

void record_numbered(FlightRecorder& r, u32 count, Severity severity, u32 first = 0U)
{
    for (u32 i = 0U; i < count; ++i)
    {
        r.record(EventCode{0x0100U + first + i}, severity, StringView{"seed event"}, first + i, 2U * (first + i));
    }
}

void keep_capture(const FlightCapture& capture, void* user)
{
    *static_cast<FlightCapture*>(user) = capture;
}

void add_capture(crd::fuzz::SeedSink& sink, const char* name, FlightCapture c)
{
    normalize_clock(c);
    const Array<u8> bytes = encode_flight_record(c);
    sink.add(name, bytes.data(), bytes.size());
}

// Post window full: 10 events before the trigger (8 kept), 4 after.
FlightCapture capture_post_events_full()
{
    FlightRecorder r;
    (void)r.init(small_policy());
    record_numbered(r, 10U, Severity::Info);
    (void)r.trigger(manual_trigger(StringView{"seed: manual"}));
    record_numbered(r, 4U, Severity::Warning, 10U);
    FlightCapture c;
    (void)r.take_capture(c);
    return c;
}

// The time bound ran out after one post-window event; the recorder carries a replay recipe.
FlightCapture capture_post_window_elapsed()
{
    FlightRecorder r;
    (void)r.init(small_policy());
    r.set_replay_recipe(StringView{"fault-plan/1 plan=seed=0;allocation-failure@0+1%1000000"});
    record_numbered(r, 3U, Severity::Info);
    (void)r.trigger(manual_trigger(StringView{"seed: elapsed"}));
    record_numbered(r, 1U, Severity::Info, 3U);
    (void)r.poll(~0ULL);
    FlightCapture c;
    (void)r.take_capture(c);
    return c;
}

// A window ended early by shutdown (or the fatal path), with a second, suppressed trigger of another kind.
FlightCapture capture_ended_early(FlightEnd end)
{
    FlightCapture  c;
    FlightRecorder r;
    (void)r.init(small_policy());
    r.set_sink(&keep_capture, &c);
    record_numbered(r, 2U, Severity::Info);
    (void)r.trigger(manual_trigger(StringView{"seed: early end"}));
    FlightTriggerInput hang = manual_trigger(StringView{"seed: suppressed"});
    hang.kind               = FlightTrigger::Hang;
    hang.code               = kFlightCodeHang;
    (void)r.trigger(hang);
    record_numbered(r, 2U, Severity::RecoverableError, 2U);
    (void)r.finish(end);
    return c;
}

// A Basic policy: the informational events are filtered out, the warnings recorded.
FlightCapture capture_basic()
{
    FlightRecorder r;
    (void)r.init(small_policy(FlightDetail::Basic));
    record_numbered(r, 3U, Severity::Info);
    record_numbered(r, 3U, Severity::Warning, 3U);
    (void)r.trigger(manual_trigger(StringView{"seed: basic"}));
    record_numbered(r, 2U, Severity::Info, 6U);
    record_numbered(r, 4U, Severity::DeveloperAssertion, 8U);
    FlightCapture c;
    (void)r.take_capture(c);
    return c;
}

// A message longer than an event holds (clipped, flagged) and an empty post window that elapsed.
FlightCapture capture_clipped_empty_window()
{
    FlightRecorder r;
    (void)r.init(small_policy());
    char long_message[200];
    std::memset(long_message, 'm', sizeof(long_message));
    r.record(EventCode{0x0200U}, Severity::Warning, StringView{long_message, sizeof(long_message)}, 1U, 2U);
    (void)r.trigger(manual_trigger(StringView{"seed: clipped"}));
    (void)r.poll(~0ULL);
    FlightCapture c;
    (void)r.take_capture(c);
    return c;
}

// A restricted event and a restricted trigger: their class kept, their text never recorded.
FlightCapture capture_restricted()
{
    FlightRecorder r;
    (void)r.init(small_policy());
    record_numbered(r, 2U, Severity::Info);
    r.record(EventCode{0x0300U}, Severity::Warning, StringView{"seed: restricted"}, 3U, 4U, RedactionClass::Restricted);
    FlightTriggerInput t = manual_trigger(StringView{"seed: restricted trigger"});
    t.redaction          = RedactionClass::Restricted;
    (void)r.trigger(t);
    record_numbered(r, 4U, Severity::Info, 2U);
    FlightCapture c;
    (void)r.take_capture(c);
    return c;
}

// What finish(Fatal) delivers when the ring stays held: the trigger alone, said so.
FlightCapture without_ring(FlightCapture c)
{
    c.events.clear();
    c.pre_retained           = 0U;
    c.post_retained          = 0U;
    c.dropped_while_complete = 0U;
    c.end                    = FlightEnd::Fatal;
    c.ring_unavailable       = true;
    c.post_truncated         = true;
    return c;
}

// Byte offsets of the encoding (crd/perf/flight_recorder.hpp: encode_flight_record).
constexpr usize kDetailOffset     = 11U;
constexpr usize kFileLengthOffset = 52U;

void add_patched(crd::fuzz::SeedSink& sink, const char* name, const Array<u8>& base, usize offset, const void* value,
                 usize bytes)
{
    Array<u8> b = base;
    if (offset + bytes <= b.size())
    {
        std::memcpy(&b[offset], value, bytes);
        sink.add(name, b.data(), b.size());
    }
}
} // namespace

namespace
{
bool flight_accepted(const u8* data, usize size)
{
    return decode_flight_record(ConstSpan<u8>(data, size)).ok;
}

// The captures a recorder wrote; every other seed must be refused.
constexpr const char* kAcceptedSeeds[] = {
    "post-events-full", "post-window-elapsed-replay", "shutdown-suppressed", "fatal-suppressed",
    "basic-detail",     "clipped-empty-window",       "ring-unavailable",    "restricted-event-and-trigger"};
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& out)
{
    crd::fuzz::VerdictSink sink(out, &flight_accepted, kAcceptedSeeds,
                                sizeof(kAcceptedSeeds) / sizeof(kAcceptedSeeds[0]));
    const FlightCapture full     = capture_post_events_full();
    const FlightCapture elapsed  = capture_post_window_elapsed();
    const FlightCapture shutdown = capture_ended_early(FlightEnd::Shutdown);
    const FlightCapture fatal    = capture_ended_early(FlightEnd::Fatal);
    const FlightCapture basic    = capture_basic();
    const FlightCapture clipped  = capture_clipped_empty_window();
    const FlightCapture hidden   = capture_restricted();

    add_capture(sink, "post-events-full", full);
    add_capture(sink, "post-window-elapsed-replay", elapsed);
    add_capture(sink, "shutdown-suppressed", shutdown);
    add_capture(sink, "fatal-suppressed", fatal);
    add_capture(sink, "basic-detail", basic);
    add_capture(sink, "clipped-empty-window", clipped);
    add_capture(sink, "ring-unavailable", without_ring(fatal));
    add_capture(sink, "restricted-event-and-trigger", hidden);

    // Each contradicts what a recorder writes in exactly one way; each must be refused.
    {
        FlightCapture c    = fatal; // a fatal capture whose events say the ring was taken
        c.ring_unavailable = true;
        add_capture(sink, "forged-ring-unavailable-with-events", std::move(c));
    }
    {
        FlightCapture c  = shutdown;
        c.post_truncated = false;
        add_capture(sink, "forged-shutdown-not-truncated", std::move(c));
    }
    {
        FlightCapture c = elapsed;
        c.end           = FlightEnd::PostEventsFull;
        add_capture(sink, "forged-full-with-short-window", std::move(c));
    }
    {
        FlightCapture c         = elapsed;
        c.policy.post_window_ns = 0U;
        add_capture(sink, "forged-elapsed-without-time-bound", std::move(c));
    }
    {
        FlightCapture c       = full;
        c.policy.trigger_mask = kFlightAllTriggers & ~flight_trigger_bit(FlightTrigger::Manual);
        add_capture(sink, "forged-masked-trigger", std::move(c));
    }
    {
        FlightCapture c       = full;
        c.suppressed_kinds    = flight_trigger_bit(FlightTrigger::Hang);
        c.suppressed_triggers = 0U;
        add_capture(sink, "forged-suppressed-kind-without-count", std::move(c));
    }
    {
        FlightCapture c = full;
        c.events[c.events.size() - 1U].sequence += 1U;
        add_capture(sink, "forged-sequence-gap", std::move(c));
    }
    {
        FlightCapture c = basic;
        c.events[0].severity = Severity::Info;
        add_capture(sink, "forged-basic-info-event", std::move(c));
    }
    {
        FlightCapture c            = hidden; // the restricted event is the third
        c.events[2].message[0]     = 's';
        c.events[2].message_length = 1U;
        add_capture(sink, "forged-restricted-event-with-text", std::move(c));
    }
    {
        FlightCapture c          = hidden;
        c.trigger.message[0]     = 's';
        c.trigger.message_length = 1U;
        add_capture(sink, "forged-restricted-trigger-with-text", std::move(c));
    }
    {
        FlightCapture c                 = full;
        c.events[0].message_truncated   = true;
        add_capture(sink, "forged-clip-flag-short-message", std::move(c));
    }
    {
        FlightCapture c    = full;
        c.replay_available = false;
        c.replay.clear();
        add_capture(sink, "forged-unavailable-without-reason", std::move(c));
    }

    // Structural damage on the full capture's bytes.
    FlightCapture normalized = full;
    normalize_clock(normalized);
    const Array<u8> bytes = encode_flight_record(normalized);
    {
        Array<u8> b = bytes;
        b.push_back(0U);
        sink.add("trailing-byte", b.data(), b.size());
    }
    const usize cuts[] = {3U, 8U, 12U, kFileLengthOffset + 1U, 120U, bytes.size() / 2U, bytes.size() - 1U};
    const char* names[] = {"cut-in-magic",     "cut-after-version", "cut-after-flags", "cut-in-text-lengths",
                           "cut-in-policy",    "cut-mid-events",    "cut-last-byte"};
    for (usize i = 0U; i < sizeof(cuts) / sizeof(cuts[0]); ++i)
    {
        if (cuts[i] < bytes.size())
        {
            sink.add(names[i], bytes.data(), cuts[i]);
        }
    }
    {
        // The event count is the u32 before the events; a count that disagrees with the retained counts.
        usize event_bytes = 0U;
        for (usize i = 0U; i < normalized.events.size(); ++i)
        {
            event_bytes += 40U + normalized.events[i].message_length;
        }
        const u32 huge = 0x7FFFFFFFU;
        add_patched(sink, "count-disagrees-with-retained", bytes, bytes.size() - event_bytes - 4U, &huge, sizeof(huge));
    }
    const u16 at_cap = static_cast<u16>(kFlightTriggerFileBytes);
    add_patched(sink, "trigger-file-at-bound", bytes, kFileLengthOffset, &at_cap, sizeof(at_cap));
    const u8 unknown_detail = 2U;
    add_patched(sink, "unknown-detail-level", bytes, kDetailOffset, &unknown_detail, sizeof(unknown_detail));
    sink.add_text("empty", "");
    sink.add_text("garbage", "this is not a flight record");
}
