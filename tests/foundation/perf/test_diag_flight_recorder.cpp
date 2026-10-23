// DIAG.9b -- triggerable bounded flight recording. The recorder keeps the window before a trigger and a bounded window
// after it, with explicit loss metadata and a replay recipe; each trigger source turns an existing seam's failure
// signal into a trigger. The positive cases drive the real seams (crd's assert path, check_invariant, the jobs
// starvation watchdog, a StreamingAllocator under budget pressure, an allocator that runs out); the fatal path runs in
// an isolated child (crd-diag-flight-fatal-specimen) whose bundle the parent reads back.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include "module_file_identity.hpp"

#include <crd/perf/build_identity.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_import.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/flight_recorder.hpp>
#include <crd/perf/flight_triggers.hpp>

#include <crd/core/assert.hpp>
#include <crd/diag/specimen_runner.hpp>
#include <crd/jobs/job_decl.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/streaming_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers;
namespace fs   = std::filesystem;

cp::FlightPolicy small_policy(crd::u32 capacity, crd::u32 pre, crd::u32 post)
{
    cp::FlightPolicy p;
    p.capacity       = capacity;
    p.pre_events     = pre;
    p.post_events    = post;
    p.post_window_ns = 0U; // bounded by events only unless a case says otherwise
    return p;
}

void record_n(cp::FlightRecorder& r, crd::u32 first, crd::u32 count)
{
    char text[32];
    for (crd::u32 i = first; i < first + count; ++i)
    {
        (void)std::snprintf(text, sizeof(text), "event %u", static_cast<unsigned>(i));
        r.record(0x0042U, cp::Severity::Info, cont::StringView{text}, i, 0U);
    }
}

cp::FlightTriggerInput manual(const char* message)
{
    cp::FlightTriggerInput in;
    in.kind    = cp::FlightTrigger::Manual;
    in.code    = cp::kFlightCodeManual;
    in.message = cont::StringView{message};
    return in;
}

// Every capture must satisfy these whatever produced it: pre events strictly before the trigger, post events at or
// after it, contiguous ascending sequences, and counts inside the policy bounds.
bool capture_is_consistent(const cp::FlightCapture& c)
{
    if (c.events.size() != static_cast<crd::usize>(c.pre_retained) + c.post_retained ||
        c.pre_retained > c.policy.pre_events || c.post_retained > c.policy.post_events ||
        c.recorded_before_trigger != c.trigger.sequence)
    {
        return false;
    }
    for (crd::usize i = 0U; i < c.events.size(); ++i)
    {
        const bool pre = i < c.pre_retained;
        if ((pre && c.events[i].sequence >= c.trigger.sequence) || (!pre && c.events[i].sequence < c.trigger.sequence))
        {
            return false;
        }
        if (i > 0U && c.events[i].sequence != c.events[i - 1U].sequence + 1U)
        {
            return false;
        }
    }
    return true;
}

struct SinkLog
{
    std::atomic<int> calls{0};
    cp::FlightCapture last;
};

void sink_into(const cp::FlightCapture& capture, void* user)
{
    auto* log = static_cast<SinkLog*>(user);
    log->last.trigger                 = capture.trigger;
    log->last.end                     = capture.end;
    log->last.end_ns                  = capture.end_ns;
    log->last.policy                  = capture.policy;
    log->last.policy_generation       = capture.policy_generation;
    log->last.recorded_before_trigger = capture.recorded_before_trigger;
    log->last.pre_retained            = capture.pre_retained;
    log->last.post_retained           = capture.post_retained;
    log->last.post_truncated          = capture.post_truncated;
    log->last.ring_unavailable        = capture.ring_unavailable;
    log->last.dropped_while_complete  = capture.dropped_while_complete;
    log->last.suppressed_triggers     = capture.suppressed_triggers;
    log->last.suppressed_kinds        = capture.suppressed_kinds;
    log->last.replay_available        = capture.replay_available;
    log->last.replay.clear();
    log->last.replay.append(cont::StringView{capture.replay.c_str(), capture.replay.size()});
    log->last.events.clear();
    for (crd::usize i = 0U; i < capture.events.size(); ++i)
    {
        log->last.events.push_back(capture.events[i]);
    }
    log->calls.fetch_add(1, std::memory_order_release);
}

int ignore_assert_dialog(const char* /*formatted*/)
{
    return 0; // in-process: no dialog, no abort
}

fs::path scratch_dir(const char* tag)
{
    fs::path p = fs::temp_directory_path();
    char     name[64];
    (void)std::snprintf(name, sizeof(name), "crd_flight_%s_%u", tag,
                        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFF));
    p /= name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

cont::Array<crd::u8> read_file_bytes(const fs::path& path)
{
    cont::Array<crd::u8> out;
    std::FILE*           f = nullptr;
#if defined(_MSC_VER)
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0)
    {
        f = nullptr;
    }
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (f == nullptr)
    {
        return out;
    }
    (void)std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    (void)std::fseek(f, 0, SEEK_SET);
    if (size > 0)
    {
        out.resize_uninitialized(static_cast<crd::usize>(size));
        const crd::usize got = std::fread(out.data(), 1, static_cast<crd::usize>(size), f);
        out.resize(got);
    }
    (void)std::fclose(f);
    return out;
}

bool built_with_asan()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}
} // namespace

TEST_CASE("flight policy: out-of-bounds candidates are refused and the last good policy stays",
          "[perf][diag][diag9b][flight]")
{
    CHECK(cp::validate_flight_policy(cp::FlightPolicy{}) == cp::FlightPolicyStatus::Committed);

    cp::FlightPolicy bad = small_policy(16U, 8U, 9U); // pre + post > capacity: the post window would eat the pre one
    CHECK(cp::validate_flight_policy(bad) == cp::FlightPolicyStatus::RejectedOutOfBounds);
    bad = small_policy(16U, 0U, 4U);
    CHECK(cp::validate_flight_policy(bad) == cp::FlightPolicyStatus::RejectedOutOfBounds);
    bad = small_policy(cp::kFlightMaxEvents + 1U, 8U, 4U);
    CHECK(cp::validate_flight_policy(bad) == cp::FlightPolicyStatus::RejectedOutOfBounds);
    bad              = small_policy(16U, 8U, 4U);
    bad.trigger_mask = 1U; // the None bit alone
    CHECK(cp::validate_flight_policy(bad) == cp::FlightPolicyStatus::RejectedOutOfBounds);
    bad                = small_policy(16U, 8U, 4U);
    bad.schema_version = 2U;
    CHECK(cp::validate_flight_policy(bad) == cp::FlightPolicyStatus::RejectedUnknownVersion);

    cp::FlightRecorder r;
    CHECK(r.reload_policy(cp::FlightPolicy{}) == cp::FlightPolicyStatus::RejectedNotInitialized);
    CHECK(r.init(small_policy(16U, 8U, 9U)) == cp::FlightPolicyStatus::RejectedOutOfBounds);
    CHECK_FALSE(r.is_initialized());
    REQUIRE(r.init(small_policy(16U, 8U, 4U)) == cp::FlightPolicyStatus::Committed);
    CHECK(r.init(small_policy(16U, 8U, 4U)) == cp::FlightPolicyStatus::RejectedBusy);
    CHECK(r.policy_generation() == 1U);

    record_n(r, 0U, 5U);
    CHECK(r.reload_policy(small_policy(8U, 6U, 4U)) == cp::FlightPolicyStatus::RejectedOutOfBounds);
    CHECK(r.policy_generation() == 1U);
    CHECK(r.policy().capacity == 16U);

    // A pending capture holds the active policy.
    REQUIRE(r.trigger(manual("pending")) == cp::FlightTriggerResult::Started);
    CHECK(r.reload_policy(small_policy(32U, 16U, 8U)) == cp::FlightPolicyStatus::RejectedBusy);
    record_n(r, 5U, 4U);
    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    CHECK(c.policy_generation == 1U);

    REQUIRE(r.reload_policy(small_policy(32U, 16U, 8U)) == cp::FlightPolicyStatus::Committed);
    CHECK(r.policy_generation() == 2U);
    CHECK(r.policy().capacity == 32U);
}

#if CRD_ENABLE_ASSERTS
// The wrap guard needs 2^32 - 1 reloads, each allocating a ring; the test-only seam starts the counter near the end.
TEST_CASE("flight policy: a reload that would wrap the generation is refused and keeps the ring",
          "[perf][diag][diag9b][diag10b][flight]")
{
    cp::FlightRecorder r;
    CHECK_FALSE(r.test_set_policy_generation(7U)); // before init
    REQUIRE(r.init(small_policy(16U, 8U, 4U)) == cp::FlightPolicyStatus::Committed);
    CHECK_FALSE(r.test_set_policy_generation(0U)); // 0 is never a generation
    CHECK(r.policy_generation() == 1U);
    REQUIRE(r.test_set_policy_generation(0xFFFFFFFEU));
    CHECK(r.policy().capacity == 16U); // the seam moves only the counter

    // One more reload still fits: it takes the last generation.
    REQUIRE(r.reload_policy(small_policy(32U, 8U, 4U)) == cp::FlightPolicyStatus::Committed);
    CHECK(r.policy_generation() == 0xFFFFFFFFU);

    // The next valid reload would wrap to 0: refused, and neither the policy nor the ring moves.
    record_n(r, 0U, 6U);
    CHECK(r.reload_policy(small_policy(64U, 16U, 8U)) == cp::FlightPolicyStatus::RejectedGenerationWrap);
    CHECK(r.policy_generation() == 0xFFFFFFFFU);
    CHECK(r.policy().capacity == 32U);
    CHECK(r.policy().pre_events == 8U);
    // A malformed candidate is still refused for what it is: validation runs before the wrap guard.
    CHECK(r.reload_policy(small_policy(16U, 8U, 9U)) == cp::FlightPolicyStatus::RejectedOutOfBounds);

    // The ring was not reallocated: the six events recorded before the refusal are the capture's pre window.
    REQUIRE(r.trigger(manual("after the wrap")) == cp::FlightTriggerResult::Started);
    record_n(r, 6U, 4U);
    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    CHECK(capture_is_consistent(c));
    CHECK(c.policy_generation == 0xFFFFFFFFU);
    CHECK(c.policy.capacity == 32U);
    CHECK(c.end == cp::FlightEnd::PostEventsFull);
    CHECK(c.pre_retained == 6U);
    CHECK(c.post_retained == 4U);
    REQUIRE(c.events.size() == 10U);
    CHECK(c.events[0].value_a == 0U);
    CHECK(c.events[9].value_a == 9U);

    // Re-armed after collection, the guard still holds.
    CHECK(r.reload_policy(small_policy(64U, 16U, 8U)) == cp::FlightPolicyStatus::RejectedGenerationWrap);
    CHECK(r.policy_generation() == 0xFFFFFFFFU);
}
#endif

TEST_CASE("flight recorder: a trigger keeps the pre window and a bounded post window, with loss counted",
          "[perf][diag][diag9b][flight]")
{
    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(16U, 8U, 4U)) == cp::FlightPolicyStatus::Committed);
    r.set_replay_recipe("replay with: test recipe 42");

    record_n(r, 0U, 30U); // the ring holds 16; 22 events fall outside the 8-event pre window
    REQUIRE(r.trigger(manual("operator asked")) == cp::FlightTriggerResult::Started);
    CHECK(r.capture_pending());
    record_n(r, 30U, 10U); // 4 close the post window; 6 arrive while the capture waits

    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    CHECK_FALSE(r.capture_pending());
    CHECK(capture_is_consistent(c));
    CHECK(c.trigger.kind == cp::FlightTrigger::Manual);
    CHECK(c.trigger.sequence == 30U);
    CHECK(c.trigger.message_view() == "operator asked");
    CHECK(c.end == cp::FlightEnd::PostEventsFull);
    CHECK_FALSE(c.post_truncated);
    CHECK(c.recorded_before_trigger == 30U);
    CHECK(c.pre_retained == 8U);
    CHECK(c.post_retained == 4U);
    CHECK(c.dropped_while_complete == 6U);
    REQUIRE(c.events.size() == 12U);
    CHECK(c.events[0].sequence == 22U);
    CHECK(c.events[0].message_view() == "event 22");
    CHECK(c.events[7].message_view() == "event 29");
    CHECK(c.events[8].message_view() == "event 30");
    CHECK(c.events[11].message_view() == "event 33");
    CHECK(c.replay_available);
    CHECK(cont::StringView{c.replay.c_str(), c.replay.size()} == "replay with: test recipe 42");

    // Re-armed: the next trigger starts a new capture whose pre window holds the events since.
    record_n(r, 40U, 3U);
    REQUIRE(r.trigger(manual("again")) == cp::FlightTriggerResult::Started);
    record_n(r, 43U, 4U);
    cp::FlightCapture c2;
    REQUIRE(r.take_capture(c2));
    CHECK(capture_is_consistent(c2));
    CHECK(c2.dropped_while_complete == 0U);
    CHECK(c2.events[c2.pre_retained - 1U].message_view() == "event 42");
    CHECK(r.captures_delivered() == 2U);
}

TEST_CASE("flight recorder: a second trigger while one is pending is suppressed and counted, never replacing it",
          "[perf][diag][diag9b][flight]")
{
    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(16U, 8U, 4U)) == cp::FlightPolicyStatus::Committed);
    record_n(r, 0U, 3U);
    REQUIRE(r.trigger(manual("first")) == cp::FlightTriggerResult::Started);

    cp::FlightTriggerInput hitch;
    hitch.kind = cp::FlightTrigger::FrameHitch;
    hitch.code = cp::kFlightCodeFrameHitch;
    CHECK(r.trigger(hitch) == cp::FlightTriggerResult::Suppressed);
    record_n(r, 3U, 4U);
    CHECK(r.trigger(manual("third")) == cp::FlightTriggerResult::Suppressed); // complete, not yet collected

    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    CHECK(c.trigger.message_view() == "first");
    CHECK(c.suppressed_triggers == 2U);
    CHECK(c.suppressed_kinds ==
          (cp::flight_trigger_bit(cp::FlightTrigger::FrameHitch) | cp::flight_trigger_bit(cp::FlightTrigger::Manual)));
    CHECK(c.pre_retained == 3U); // fewer than pre_events existed; nothing was lost
    CHECK(c.recorded_before_trigger == 3U);

    // A trigger kind the policy does not arm is masked, not counted.
    REQUIRE(r.reload_policy([] {
                cp::FlightPolicy p = small_policy(16U, 8U, 4U);
                p.trigger_mask     = cp::flight_trigger_bit(cp::FlightTrigger::Manual);
                return p;
            }()) == cp::FlightPolicyStatus::Committed);
    CHECK(r.trigger(hitch) == cp::FlightTriggerResult::Masked);
    CHECK_FALSE(r.capture_pending());

    cp::FlightRecorder idle;
    CHECK(idle.trigger(manual("x")) == cp::FlightTriggerResult::NotInitialized);
    idle.record(1U, cp::Severity::Info, cont::StringView{"before init"}); // a no-op, never a fault
    CHECK(idle.total_recorded() == 0U);
}

TEST_CASE("flight recorder: the time bound closes a quiet post window, and shutdown delivers a truncated one",
          "[perf][diag][diag9b][flight]")
{
    SinkLog            log;
    cp::FlightRecorder r;
    cp::FlightPolicy   p = small_policy(16U, 8U, 8U);
    p.post_window_ns     = 1000000U; // 1 ms
    REQUIRE(r.init(p) == cp::FlightPolicyStatus::Committed);
    r.set_sink(&sink_into, &log);

    record_n(r, 0U, 5U);
    REQUIRE(r.trigger(manual("quiet")) == cp::FlightTriggerResult::Started);
    record_n(r, 5U, 2U);
    const crd::u64 t0 = cp::diagnostic_now_ns();
    CHECK_FALSE(r.poll(t0)); // within the window: nothing yet (the trigger's own clock read precedes t0 by < 1 ms)
    CHECK(r.poll(t0 + 5000000U));
    REQUIRE(log.calls.load() == 1);
    CHECK(log.last.end == cp::FlightEnd::PostWindowElapsed);
    CHECK(log.last.post_retained == 2U);
    CHECK_FALSE(log.last.post_truncated); // the declared window ran its course
    CHECK(capture_is_consistent(log.last));
    CHECK_FALSE(log.last.replay_available);
    CHECK(cont::StringView{log.last.replay.c_str(), log.last.replay.size()} ==
          "no replay recipe was registered with the flight recorder");

    REQUIRE(r.trigger(manual("teardown")) == cp::FlightTriggerResult::Started);
    record_n(r, 7U, 1U);
    r.shutdown();
    REQUIRE(log.calls.load() == 2);
    CHECK(log.last.end == cp::FlightEnd::Shutdown);
    CHECK(log.last.post_truncated);
    CHECK(log.last.post_retained == 1U);
    CHECK(log.last.trigger.message_view() == "teardown");
}

TEST_CASE("flight record: a capture round-trips through a bundle, and malformed payloads are refused",
          "[perf][diag][diag9b][flight]")
{
    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(16U, 8U, 4U)) == cp::FlightPolicyStatus::Committed);
    r.set_replay_recipe("ceridc replay.run record=run.crpl");
    record_n(r, 0U, 12U);
    char long_message[300];
    std::memset(long_message, 'x', sizeof(long_message) - 1U);
    long_message[sizeof(long_message) - 1U] = '\0';
    r.record(7U, cp::Severity::Warning, cont::StringView{long_message});
    REQUIRE(r.trigger(manual("round trip")) == cp::FlightTriggerResult::Started);
    record_n(r, 12U, 4U);
    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    REQUIRE(c.events[c.pre_retained - 1U].message_truncated); // clipped, and says so
    CHECK(c.events[c.pre_retained - 1U].message_length == cp::kFlightMessageBytes - 1U);

    const cont::Array<crd::u8> bundle = cp::write_flight_bundle(c);
    const cp::BundleImport     imp    = cp::import_bundle(cont::ConstSpan<crd::u8>{bundle.data(), bundle.size()});
    REQUIRE(imp.status == cp::ImportStatus::Ok);
    REQUIRE(imp.has_manifest);
    const cp::ImportedSection* flight = nullptr;
    for (crd::usize i = 0U; i < imp.sections.size(); ++i)
    {
        if (imp.sections[i].tag == cp::BundleSectionTag::FlightRecord)
        {
            flight = &imp.sections[i];
        }
    }
    REQUIRE(flight != nullptr);
    const cp::FlightRecordRead back = cp::decode_flight_record(flight->payload);
    REQUIRE(back.ok);
    CHECK(back.capture.trigger.message_view() == "round trip");
    CHECK(back.capture.trigger.sequence == c.trigger.sequence);
    CHECK(back.capture.end == c.end);
    CHECK(back.capture.pre_retained == c.pre_retained);
    CHECK(back.capture.post_retained == c.post_retained);
    CHECK(back.capture.policy.capacity == 16U);
    CHECK(cont::StringView{back.capture.replay.c_str(), back.capture.replay.size()} ==
          "ceridc replay.run record=run.crpl");
    REQUIRE(back.capture.events.size() == c.events.size());
    for (crd::usize i = 0U; i < c.events.size(); ++i)
    {
        CHECK(back.capture.events[i].sequence == c.events[i].sequence);
        CHECK(back.capture.events[i].message_view() == c.events[i].message_view());
        CHECK(back.capture.events[i].message_truncated == c.events[i].message_truncated);
    }

    cont::String json;
    cp::flight_capture_summary_json(c, json);
    CHECK(cont::StringView{json.c_str(), json.size()}.find("\"trigger\":\"manual\"") != cont::StringView::npos);
    CHECK(cont::StringView{json.c_str(), json.size()}.find("\"end\":\"post-events-full\"") != cont::StringView::npos);

    // Every strict prefix is refused (never read past the end), as are a wrong magic, an inflated event count and a
    // trailing byte.
    const cont::Array<crd::u8> payload = cp::encode_flight_record(c);
    int                        accepted_prefixes = 0;
    for (crd::usize n = 0U; n < payload.size(); ++n)
    {
        if (cp::decode_flight_record(cont::ConstSpan<crd::u8>{payload.data(), n}).ok)
        {
            ++accepted_prefixes;
        }
    }
    CHECK(accepted_prefixes == 0);
    REQUIRE(cp::decode_flight_record(cont::ConstSpan<crd::u8>{payload.data(), payload.size()}).ok);

    cont::Array<crd::u8> bad = payload;
    bad[0]                   = 0x00U;
    CHECK_FALSE(cp::decode_flight_record(cont::ConstSpan<crd::u8>{bad.data(), bad.size()}).ok);

    bad = payload;
    bad.push_back(0U);
    const cp::FlightRecordRead trailing = cp::decode_flight_record(cont::ConstSpan<crd::u8>{bad.data(), bad.size()});
    CHECK_FALSE(trailing.ok);
    CHECK(trailing.error == "trailing bytes after the last event");

    // The u32 event count sits just before the first event; inflate it.
    bad                       = payload;
    const crd::usize count_at = payload.size() - [&c] {
        crd::usize n = 0U;
        for (crd::usize i = 0U; i < c.events.size(); ++i)
        {
            n += 40U + c.events[i].message_length;
        }
        return n + 4U;
    }();
    bad[count_at] = static_cast<crd::u8>(bad[count_at] + 1U);
    const cp::FlightRecordRead inflated = cp::decode_flight_record(cont::ConstSpan<crd::u8>{bad.data(), bad.size()});
    CHECK_FALSE(inflated.ok);
    CHECK(inflated.error == "event count disagrees with the retained counts");
}

TEST_CASE("flight record: bundle.inspect names the flight section of a written flight bundle",
          "[perf][diag][diag9b][flight]")
{
    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(16U, 8U, 2U)) == cp::FlightPolicyStatus::Committed);
    record_n(r, 0U, 4U);
    cp::FlightTriggerInput in = manual("inspect me");
    in.source.file            = cont::StringView{"src/orders.cpp"};
    in.source.line            = 321U;
    in.value_a                = 7U;
    in.value_b                = 9U;
    REQUIRE(r.trigger(in) == cp::FlightTriggerResult::Started);
    record_n(r, 4U, 2U);
    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    const cont::Array<crd::u8> bundle = cp::write_flight_bundle(c);

    char path[512];
    (void)std::snprintf(path, sizeof(path), "%s/diag_flight_inspect.cdb", CRD_DIAG_COMMANDS_SCRATCH);
    REQUIRE(cp::write_bundle_atomic(path, cont::ConstSpan<crd::u8>{bundle.data(), bundle.size()}));

    cp::DiagServiceConfig config;
    config.root = cont::StringView{CRD_DIAG_COMMANDS_SCRATCH};
    cp::DiagCommandService svc{cp::authority_bit(cp::DiagAuthority::Read), config, crd::memory::default_allocator()};
    cp::DiagRequest        req;
    req.command                = "bundle.inspect";
    req.path                   = "diag_flight_inspect.cdb";
    req.page_items             = 64U;
    const cp::DiagResult found = svc.execute(req);
    REQUIRE(found.status == cp::DiagStatus::Ok);
    // Four sections (manifest, flight record, build identity, the absent symbol index), the build item and, decoded
    // (DIAG.11a), the trigger, the flight window and the six retained events.
    CHECK(found.total == 13U);
    CHECK(found.complete);
    const char* json = found.json.c_str();
    CHECK(std::strstr(json, "\"tag_name\":\"flight-record\",\"absent\":false") != nullptr);
    CHECK(std::strstr(json, "\"tag_name\":\"build-identity\",\"absent\":false") != nullptr);
    CHECK(std::strstr(json, "\"tag_name\":\"symbol-index\",\"absent\":true") != nullptr);
    CHECK(std::strstr(json, "\"tag_name\":\"reserved\"") == nullptr);
    CHECK(std::strstr(json, "\"same_as_host\":true,\"differing\":\"\"") != nullptr);
    CHECK(std::strstr(json, "\"flight_record\":true,\"flight_refused\":false,\"flight_error\":\"\"") != nullptr);
    CHECK(std::strstr(json, "{\"kind\":\"trigger\",\"trigger\":\"manual\",\"code\":2304,\"file\":\"src/orders.cpp\","
                            "\"line\":321,\"message\":\"inspect me\",\"value_a\":7,\"value_b\":9,\"sequence\":4,") !=
          nullptr);
    CHECK(std::strstr(json, "{\"kind\":\"flight\",\"end\":\"post-events-full\",") != nullptr);
    CHECK(std::strstr(json, "\"pre_retained\":4,\"post_retained\":2,\"post_truncated\":false,"
                            "\"ring_unavailable\":false,") != nullptr);
    CHECK(std::strstr(json, "\"events\":6,\"replay_available\":false,") != nullptr);
    // Every retained event, on its side of the trigger, with its own scalars and message.
    char expected[160];
    for (crd::u32 i = 0U; i < 6U; ++i)
    {
        (void)std::snprintf(expected, sizeof(expected),
                            R"({"kind":"event","index":%u,"side":"%s","sequence":%u,)", static_cast<unsigned>(i),
                            i < 4U ? "before" : "after", static_cast<unsigned>(i));
        CHECK(std::strstr(json, expected) != nullptr);
        (void)std::snprintf(expected, sizeof(expected),
                            R"("code":66,"severity":"info","value_a":%u,"value_b":0,"message":"event %u",)"
                            R"("message_truncated":false})",
                            static_cast<unsigned>(i), static_cast<unsigned>(i));
        CHECK(std::strstr(json, expected) != nullptr);
    }
    CHECK(std::strstr(json, "\"index\":6,") == nullptr);
    (void)std::remove(path);
}

TEST_CASE("flight trigger: a source path longer than the field keeps its file name", "[perf][diag][diag11a][flight]")
{
    // An application built in a deep directory: the head of its path is the least telling part.
    char long_path[400];
    crd::usize at = 0U;
    for (crd::u32 i = 0U; i < 30U; ++i)
    {
        at += static_cast<crd::usize>(
            std::snprintf(long_path + at, sizeof(long_path) - at, "/dir%02u", static_cast<unsigned>(i)));
    }
    // A two-byte UTF-8 letter straddles the cut, which must not split it: its continuation byte sits exactly where a
    // byte-count cut would start the kept tail (124 bytes from the end: 127 - "...").
    long_path[at++] = '/';
    long_path[at++] = static_cast<char>(0xC3);
    long_path[at++] = static_cast<char>(0xA7);
    long_path[at++] = '/';
    for (crd::u32 i = 0U; i < 98U; ++i)
    {
        long_path[at++] = 'x';
    }
    (void)std::snprintf(long_path + at, sizeof(long_path) - at, "/consumer/src/orders.cpp");
    const cont::StringView path{long_path};
    REQUIRE(static_cast<crd::u8>(path[path.size() - 124U]) == 0xA7U);
    REQUIRE(path.size() > cp::kFlightTriggerFileBytes);

    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(16U, 8U, 1U)) == cp::FlightPolicyStatus::Committed);
    cp::FlightTriggerInput in = manual("deep");
    in.source.file            = path;
    in.source.line            = 77U;
    REQUIRE(r.trigger(in) == cp::FlightTriggerResult::Started);
    record_n(r, 0U, 1U);
    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    const cont::StringView stored = c.trigger.file_view();
    CHECK(stored.size() <= cp::kFlightTriggerFileBytes - 1U);
    CHECK(stored.size() >= cp::kFlightTriggerFileBytes - 2U); // at most the straddling lead byte is dropped
    CHECK(stored.substr(0U, 3U) == "...");
    const cont::StringView tail{"/consumer/src/orders.cpp"};
    CHECK(stored.substr(stored.size() - tail.size()) == tail);
    // The kept tail is the path's own tail, and starts on a UTF-8 lead or ASCII byte, never a continuation byte.
    CHECK(path.substr(path.size() - (stored.size() - 3U)) == stored.substr(3U));
    CHECK((static_cast<crd::u8>(stored[3]) & 0xC0U) != 0x80U);
    CHECK(c.trigger.line == 77U);

    // A path that fits is stored whole.
    cp::FlightRecorder short_r;
    REQUIRE(short_r.init(small_policy(16U, 8U, 1U)) == cp::FlightPolicyStatus::Committed);
    in.source.file = cont::StringView{"src/orders.cpp"};
    REQUIRE(short_r.trigger(in) == cp::FlightTriggerResult::Started);
    record_n(short_r, 0U, 1U);
    REQUIRE(short_r.take_capture(c));
    CHECK(c.trigger.file_view() == "src/orders.cpp");
}

TEST_CASE("flight record: bundle.inspect reports a flight record it cannot decode, and lists no trigger or event",
          "[perf][diag][diag11a][flight]")
{
    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(16U, 8U, 2U)) == cp::FlightPolicyStatus::Committed);
    record_n(r, 0U, 3U);
    REQUIRE(r.trigger(manual("refuse me")) == cp::FlightTriggerResult::Started);
    record_n(r, 3U, 2U);
    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    cont::Array<crd::u8> payload = cp::encode_flight_record(c);
    REQUIRE(payload.size() > 8U);
    payload[4] = static_cast<crd::u8>(payload[4] + 1U); // the version word: a record no reader knows

    cp::BundleWriter writer;
    REQUIRE(writer.add_section(cp::BundleSectionTag::FlightRecord,
                               cont::ConstSpan<crd::u8>{payload.data(), payload.size()}) ==
            cp::BundleWriter::AddStatus::Ok);
    const cont::Array<crd::u8> bundle = writer.finish();
    char path[512];
    (void)std::snprintf(path, sizeof(path), "%s/diag_flight_refused.cdb", CRD_DIAG_COMMANDS_SCRATCH);
    REQUIRE(cp::write_bundle_atomic(path, cont::ConstSpan<crd::u8>{bundle.data(), bundle.size()}));

    cp::DiagServiceConfig config;
    config.root = cont::StringView{CRD_DIAG_COMMANDS_SCRATCH};
    cp::DiagCommandService svc{cp::authority_bit(cp::DiagAuthority::Read), config, crd::memory::default_allocator()};
    cp::DiagRequest        req;
    req.command                = "bundle.inspect";
    req.path                   = "diag_flight_refused.cdb";
    const cp::DiagResult found = svc.execute(req);
    REQUIRE(found.status == cp::DiagStatus::Ok);
    CHECK(found.total == 1U); // the section alone
    const char* json = found.json.c_str();
    CHECK(std::strstr(json, "\"flight_record\":false,\"flight_refused\":true,\"flight_error\":\"") != nullptr);
    CHECK(std::strstr(json, "\"flight_error\":\"\"") == nullptr);
    CHECK(std::strstr(json, "\"kind\":\"trigger\"") == nullptr);
    CHECK(std::strstr(json, "\"kind\":\"event\"") == nullptr);
    (void)std::remove(path);
}

TEST_CASE("flight trigger: crd's assert path triggers, and the previous assert handler still runs",
          "[perf][diag][diag9b][flight]")
{
    static std::atomic<int> previous_calls{0};
    previous_calls.store(0);
    const crd::AssertHandler         saved_handler  = crd::get_assert_handler();
    const crd::AssertPlatformHandler saved_platform = crd::get_assert_platform_handler();
    crd::set_assert_handler([](const char*, const char*, int, const char*) { previous_calls.fetch_add(1); });
    crd::set_assert_platform_handler(&ignore_assert_dialog);

    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(16U, 8U, 4U)) == cp::FlightPolicyStatus::Committed);
    REQUIRE(cp::install_flight_assert_trigger(&r));
    cp::FlightRecorder other;
    REQUIRE(other.init(small_policy(16U, 8U, 4U)) == cp::FlightPolicyStatus::Committed);
    CHECK_FALSE(cp::install_flight_assert_trigger(&other)); // one owner at a time

    record_n(r, 0U, 6U);
    const int line = __LINE__ + 1;
    (void)crd::detail::report_assert_failure("count > 0", __FILE__, line, "queue empty");
    CHECK(previous_calls.load() == 1);
    CHECK(r.capture_pending());   // a platform handler took over, so the process continues: the window stays open
    CHECK_FALSE(r.fatal_entered());
    record_n(r, 6U, 4U);

    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    CHECK(c.trigger.kind == cp::FlightTrigger::Assertion);
    CHECK(c.trigger.code == cp::kFlightCodeAssertion);
    CHECK(c.trigger.message_view() == "count > 0: queue empty");
    CHECK(c.trigger.line == static_cast<crd::u32>(line));
    CHECK(c.trigger.file_view().find("test_diag_flight_recorder") != cont::StringView::npos);
    CHECK(c.pre_retained == 6U);
    CHECK(c.post_retained == 4U);

    cp::uninstall_flight_assert_trigger();
    (void)crd::detail::report_assert_failure("after", __FILE__, __LINE__, nullptr);
    CHECK(previous_calls.load() == 2); // the previous handler is back in its slot
    CHECK_FALSE(r.capture_pending());  // and the recorder no longer hears asserts

    crd::set_assert_handler(saved_handler);
    crd::set_assert_platform_handler(saved_platform);
}

TEST_CASE("flight trigger: a failed invariant closes the window as fatal, delivers it, then chains",
          "[perf][diag][diag9b][flight]")
{
    static std::atomic<int>      previous_calls{0};
    static std::atomic<crd::u32> previous_code{0};
    previous_calls.store(0);
    cp::set_fatal_invariant_handler([](const cp::DiagnosticEvent& e) {
        previous_calls.fetch_add(1);
        previous_code.store(e.code);
    });

    SinkLog            log;
    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(16U, 8U, 4U)) == cp::FlightPolicyStatus::Committed);
    r.set_sink(&sink_into, &log);
    REQUIRE(cp::install_flight_fatal_trigger(&r));

    record_n(r, 0U, 5U);
    CRD_DIAG_INVARIANT(false, 0x0BADU, "ledger out of balance");
    REQUIRE(log.calls.load() == 1);
    CHECK(previous_calls.load() == 1);
    CHECK(previous_code.load() == 0x0BADU);
    CHECK(log.last.trigger.kind == cp::FlightTrigger::FatalInvariant);
    CHECK(log.last.trigger.code == 0x0BADU);
    CHECK(log.last.trigger.message_view() == "ledger out of balance");
    CHECK(log.last.end == cp::FlightEnd::Fatal);
    CHECK(log.last.post_truncated);
    CHECK(log.last.pre_retained == 5U);
    CHECK(log.last.post_retained == 0U);

    // Terminal: a second fatal path in the same failure cannot replace the delivered capture.
    CHECK(r.fatal_entered());
    CHECK(r.trigger(manual("later")) == cp::FlightTriggerResult::Suppressed);
    CHECK_FALSE(r.finish(cp::FlightEnd::Fatal));
    CHECK(log.calls.load() == 1);

    cp::uninstall_flight_fatal_trigger();
    CHECK(cp::get_fatal_invariant_handler() != nullptr); // the previous (test) handler is restored
    cp::set_fatal_invariant_handler(nullptr);
}

#if CRD_ENABLE_ASSERTS
namespace
{
// A crd-jobs worker holds the recorder's collection lock (through the test-only seam) until the test releases it, so
// the test's own thread can run finish(Fatal) against a held ring. A std::mutex cannot be try-locked by its owner.
struct RingHold
{
    cp::FlightRecorder* recorder = nullptr;
    std::atomic<bool>   held{false};
    std::atomic<bool>   release{false};
    std::atomic<bool>   seam_ran{false};
};

void hold_until_released(void* data)
{
    auto* hold = static_cast<RingHold*>(data);
    hold->held.store(true, std::memory_order_release);
    while (!hold->release.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
}

void hold_ring_job(void* data) noexcept
{
    auto* hold = static_cast<RingHold*>(data);
    hold->seam_ran.store(hold->recorder->test_with_ring_held(&hold_until_released, hold), std::memory_order_release);
}

struct HeldFinish
{
    bool held      = false; // the worker held the ring when finish(Fatal) ran
    bool seam_ran  = false;
    bool delivered = false; // finish(Fatal)'s answer
};

HeldFinish finish_fatal_with_ring_held(cp::FlightRecorder& r)
{
    RingHold hold;
    hold.recorder = &r;
    crd::jobs::JobDecl job{};
    job.fn                         = &hold_ring_job;
    job.data                       = &hold;
    crd::jobs::Counter* const done = crd::jobs::run(job);

    HeldFinish out;
    for (int i = 0; i < 5000 && !out.held; ++i)
    {
        out.held = hold.held.load(std::memory_order_acquire);
        if (!out.held)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (out.held)
    {
        out.delivered = r.finish(cp::FlightEnd::Fatal);
    }
    hold.release.store(true, std::memory_order_release); // always, so the worker never outlives the case
    crd::jobs::wait(done);
    out.seam_ran = hold.seam_ran.load(std::memory_order_acquire);
    return out;
}

int g_seam_calls = 0;

void count_call(void* /*user*/)
{
    ++g_seam_calls;
}
} // namespace

TEST_CASE("flight trigger: the fatal path that finds the ring held delivers the trigger alone and is terminal",
          "[perf][diag][diag9b][diag10b][flight]")
{
    SinkLog            log;
    cp::FlightRecorder r;
    g_seam_calls = 0;
    CHECK_FALSE(r.test_with_ring_held(&count_call, nullptr)); // before init: `fn` is not called
    CHECK(g_seam_calls == 0);
    REQUIRE(r.init(small_policy(16U, 8U, 4U)) == cp::FlightPolicyStatus::Committed);
    CHECK_FALSE(r.test_with_ring_held(nullptr, nullptr));
    r.set_sink(&sink_into, &log);
    r.set_replay_recipe(cont::StringView{"ceridc replay run --record held.crun"});

    crd::jobs::Config cfg;
    cfg.num_threads = 2U; // main (not pumping) + one worker that holds the ring
    crd::jobs::init(cfg);

    // Nothing is pending: a fatal finish with the ring held has nothing to deliver and leaves the recorder armed.
    record_n(r, 0U, 5U);
    const HeldFinish armed = finish_fatal_with_ring_held(r);

    // A trigger is pending and two post events landed; the ring is held when the fatal path runs.
    const cp::FlightTriggerResult started = r.trigger(manual("ring held"));
    record_n(r, 5U, 2U);
    const HeldFinish pending = finish_fatal_with_ring_held(r);
    crd::jobs::shutdown();

    CHECK(armed.held);
    CHECK(armed.seam_ran);
    CHECK_FALSE(armed.delivered);
    REQUIRE(started == cp::FlightTriggerResult::Started);

    REQUIRE(pending.held);
    CHECK(pending.seam_ran);
    REQUIRE(pending.delivered);
    REQUIRE(log.calls.load() == 1);
    CHECK(r.captures_delivered() == 1U);
    const cp::FlightCapture& c = log.last;
    CHECK(c.ring_unavailable);
    CHECK(c.end == cp::FlightEnd::Fatal);
    CHECK(c.post_truncated);
    CHECK(c.pre_retained == 0U);
    CHECK(c.post_retained == 0U);
    CHECK(c.events.empty());
    CHECK(c.dropped_while_complete == 0U);
    CHECK(c.trigger.kind == cp::FlightTrigger::Manual);
    CHECK(c.trigger.message_view() == "ring held");
    CHECK(c.trigger.sequence == 5U); // the provisional total the trigger read
    CHECK(c.recorded_before_trigger == c.trigger.sequence);
    CHECK(c.policy_generation == 1U);
    CHECK(c.policy.capacity == 16U);
    CHECK(c.replay_available);
    CHECK(cont::StringView{c.replay.c_str(), c.replay.size()} == "ceridc replay run --record held.crun");

    // Terminal, as after any fatal capture: nothing is recorded, triggered or delivered again.
    CHECK(r.fatal_entered());
    CHECK_FALSE(r.capture_pending());
    const crd::u64 recorded = r.total_recorded();
    record_n(r, 7U, 3U);
    CHECK(r.total_recorded() == recorded);
    CHECK(r.trigger(manual("later")) == cp::FlightTriggerResult::Suppressed);
    CHECK_FALSE(r.finish(cp::FlightEnd::Fatal));
    cp::FlightCapture none;
    CHECK_FALSE(r.take_capture(none));
    r.shutdown();
    CHECK(log.calls.load() == 1);

    // The capture survives the record encoding, whose reader requires a capture without the ring to end fatal and to
    // hold no events.
    const cont::Array<crd::u8> bytes = cp::encode_flight_record(c);
    const cp::FlightRecordRead read  = cp::decode_flight_record(cont::ConstSpan<crd::u8>{bytes.data(), bytes.size()});
    REQUIRE(read.ok);
    CHECK(read.capture.ring_unavailable);
    CHECK(read.capture.end == cp::FlightEnd::Fatal);
    CHECK(read.capture.events.empty());
    CHECK(read.capture.trigger.sequence == 5U);
}

namespace
{
// A claim held unpublished on its own thread until `release`, bounded so a collector that waits forever fails the
// case instead of hanging it.
struct ClaimHold
{
    cp::FlightRecorder* recorder = nullptr;
    std::atomic<bool> claimed{false};
    std::atomic<bool> release{false};
    std::atomic<bool> seam_ran{false};
};

void hold_claim(void* data)
{
    auto* const hold = static_cast<ClaimHold*>(data);
    hold->claimed.store(true, std::memory_order_release);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!hold->release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until)
    {
        std::this_thread::yield();
    }
}

std::thread start_claim_hold(ClaimHold& hold)
{
    std::thread holder(
        [&hold]
        {
            hold.seam_ran.store(hold.recorder->test_with_claim_unpublished(&hold_claim, &hold),
                                std::memory_order_release);
        });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!hold.claimed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until)
    {
        std::this_thread::yield();
    }
    return holder;
}
} // namespace

TEST_CASE("flight recorder: a collector waits for a claim still publishing; the fatal path only within its bound",
          "[perf][diag][diag11b][flight]")
{
    SECTION("take_capture keeps the event the held claim publishes")
    {
        cp::FlightRecorder r;
        REQUIRE(r.init(small_policy(16U, 8U, 2U)) == cp::FlightPolicyStatus::Committed);
        record_n(r, 0U, 3U);
        REQUIRE(r.trigger(manual("held")) == cp::FlightTriggerResult::Started);
        record_n(r, 3U, 1U);
        ClaimHold hold;
        hold.recorder = &r;
        std::thread holder = start_claim_hold(hold); // sequence 4 fills the post window
        const bool claimed = hold.claimed.load(std::memory_order_acquire);
        std::thread releaser(
            [&hold]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                hold.release.store(true, std::memory_order_release);
            });
        cp::FlightCapture c;
        const bool taken = r.take_capture(c);
        releaser.join();
        holder.join();

        REQUIRE(claimed);
        CHECK(hold.seam_ran.load(std::memory_order_acquire));
        REQUIRE(taken);
        CHECK(capture_is_consistent(c));
        CHECK(c.end == cp::FlightEnd::PostEventsFull);
        CHECK_FALSE(c.ring_unavailable);
        CHECK(c.pre_retained == 3U);
        REQUIRE(c.post_retained == 2U);
        const cp::FlightEvent& held = c.events[c.events.size() - 1U];
        CHECK(held.sequence == 4U);
        CHECK(held.value_a == 4U);
        CHECK(held.message_view() == "claim held");
        CHECK(c.end_ns == held.timestamp_ns); // the claim that filled the window closed it at its own time
    }

    SECTION("finish(Fatal) gives up on the held claim and delivers the trigger alone")
    {
        SinkLog log;
        cp::FlightRecorder r;
        REQUIRE(r.init(small_policy(16U, 8U, 4U)) == cp::FlightPolicyStatus::Committed);
        r.set_sink(&sink_into, &log);
        record_n(r, 0U, 3U);
        REQUIRE(r.trigger(manual("held")) == cp::FlightTriggerResult::Started);
        ClaimHold hold;
        hold.recorder = &r;
        std::thread holder = start_claim_hold(hold); // sequence 3, the first post event
        const bool claimed = hold.claimed.load(std::memory_order_acquire);
        const bool delivered = r.finish(cp::FlightEnd::Fatal);
        const bool still_held = !hold.seam_ran.load(std::memory_order_acquire); // the claim had not published
        hold.release.store(true, std::memory_order_release);
        holder.join();

        REQUIRE(claimed);
        CHECK(still_held);
        CHECK(hold.seam_ran.load(std::memory_order_acquire));
        REQUIRE(delivered);
        REQUIRE(log.calls.load() == 1);
        const cp::FlightCapture& c = log.last;
        CHECK(c.ring_unavailable);
        CHECK(c.end == cp::FlightEnd::Fatal);
        CHECK(c.post_truncated);
        CHECK(c.events.empty());
        CHECK(c.trigger.sequence == 3U);
        CHECK(r.fatal_entered());
        CHECK(r.total_recorded() == 4U); // the held claim published after the fatal path gave up on it
        record_n(r, 4U, 2U);
        CHECK(r.total_recorded() == 4U);
        r.shutdown();
        CHECK(log.calls.load() == 1);
    }
}
#endif

TEST_CASE("flight trigger: the fatal path in a child process leaves a bundle with the window before it",
          "[perf][diag][diag9b][flight]")
{
    namespace cd = crd::diag;
    const fs::path dir = scratch_dir("fatal");
    for (const char* mode : {"invariant", "assert"})
    {
        char file_name[32];
        (void)std::snprintf(file_name, sizeof(file_name), "%s.cdb", mode);
        const fs::path out = dir / file_name;
        cd::Expectation e;
        e.want              = cd::Expectation::Want::CleanExit; // the bundle and the exit code are the oracle here
        e.expected_identity = "crd-diag-flight-fatal-specimen";
        e.timeout_ms        = 30000U;
        cont::Array<cont::String> args;
        args.push_back(cont::String{mode});
        args.push_back(cont::String{out.string().c_str()});
        const cd::Outcome o = cd::run_specimen(cont::String{CRD_DIAG_FLIGHT_FATAL_SPECIMEN}, args, e);
        INFO("mode=" << mode << " verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code
                     << " reason=" << o.reason.c_str());
        CHECK(o.verdict != cd::Verdict::MissingExecutable);
        CHECK(o.verdict != cd::Verdict::MismatchedBinary);
        CHECK_FALSE(o.timed_out);
        CHECK((o.crashed || o.exit_code != 0)); // the fatal path terminated it; exit 0 means it returned
        if (!built_with_asan())
        {
#if defined(_WIN32)
            CHECK(o.exit_code == 3); // abort()
#else
            CHECK(o.crashed); // SIGABRT
#endif
        }

        const cont::Array<crd::u8> bytes = read_file_bytes(out);
        REQUIRE(bytes.size() > 0U);
        const cp::BundleImport imp = cp::import_bundle(cont::ConstSpan<crd::u8>{bytes.data(), bytes.size()});
        REQUIRE(imp.status == cp::ImportStatus::Ok);
        const cp::ImportedSection* flight = nullptr;
        for (crd::usize i = 0U; i < imp.sections.size(); ++i)
        {
            if (imp.sections[i].tag == cp::BundleSectionTag::FlightRecord)
            {
                flight = &imp.sections[i];
            }
        }
        REQUIRE(flight != nullptr);
        const cp::FlightRecordRead back = cp::decode_flight_record(flight->payload);
        REQUIRE(back.ok);
        const cp::FlightCapture& c = back.capture;
        CHECK(capture_is_consistent(c));
        if (std::strcmp(mode, "invariant") == 0)
        {
            CHECK(c.trigger.kind == cp::FlightTrigger::FatalInvariant);
            CHECK(c.trigger.code == 0x0777U);
            CHECK(c.trigger.message_view() == "flight specimen invariant");
        }
        else
        {
            CHECK(c.trigger.kind == cp::FlightTrigger::Assertion);
            CHECK(c.trigger.message_view() == "flight_specimen_condition: flight specimen assertion");
        }
        CHECK(c.end == cp::FlightEnd::Fatal);
        CHECK(c.post_truncated);
        CHECK(c.recorded_before_trigger == 50U);
        CHECK(c.pre_retained == 32U);
        REQUIRE(c.events.size() == 32U);
        CHECK(c.events[0].message_view() == "event 18");
        CHECK(c.events[31].message_view() == "event 49");
        CHECK(c.replay_available);
        CHECK(cont::StringView{c.replay.c_str(), c.replay.size()} ==
              "crd-diag-flight-fatal-specimen invariant|assert <bundle path>");

        // The build that wrote it is this lane's own build, and its module snapshot names the specimen's executable
        // with the identity its file on disk carries (read by a parser separate from the live walk).
        REQUIRE(imp.has_build);
        cont::String differing;
        CHECK(cp::same_build_identity(imp.build, cp::current_build_identity(), differing));
        CHECK(cont::StringView{differing.c_str(), differing.size()}.empty());
        REQUIRE(imp.has_symbols);
        const fs::path            specimen = fs::path(CRD_DIAG_FLIGHT_FATAL_SPECIMEN);
        const cp::ModuleIdentity* named    = nullptr;
        for (const cp::ModuleIdentity& m : imp.symbols)
        {
            if (specimen.filename().string() == m.name.c_str())
            {
                named = &m;
            }
        }
        REQUIRE(named != nullptr);
        CHECK_FALSE(named->unloaded);
        cp::ModuleIdentity from_file;
        REQUIRE(crd::test::module_file::file_identity(specimen.string().c_str(), from_file));
        CHECK(crd::test::module_file::same_identity(*named, from_file));
#if !defined(_WIN32)
        CHECK(from_file.id_kind == cp::SymbolIdKind::GnuBuildId);
#endif
        if (from_file.id_kind != cp::SymbolIdKind::None)
        {
            CHECK(cp::identity_matches(*named, from_file));
        }
        else
        {
            // No CodeView record in the file: a Windows build linked without a PDB (win-release). The bundle claims
            // none either, so it can never be matched to a debug file; its PE fields above still name the file.
            CHECK(named->id_kind == cp::SymbolIdKind::None);
            CHECK(named->timestamp != 0U);
            CHECK_FALSE(cp::identity_matches(*named, from_file));
        }
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

namespace
{
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
cp::FlightRecorder* g_job_recorder = nullptr;
std::atomic<bool>   g_low_ran{false};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void busy_recording_1ms(void* /*data*/) noexcept
{
    g_job_recorder->record(0x0050U, cp::Severity::Info, cont::StringView{"normal job"});
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    while (std::chrono::steady_clock::now() < end)
    { /* spin */
    }
}

void low_job(void* /*data*/) noexcept
{
    g_low_ran.store(true, std::memory_order_release);
}
} // namespace

TEST_CASE("flight trigger: the jobs watchdog's starvation report triggers while workers keep recording",
          "[perf][diag][diag9b][flight]")
{
    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(256U, 64U, 16U)) == cp::FlightPolicyStatus::Committed);
    g_job_recorder = &r;
    g_low_ran.store(false);
    crd::jobs::set_starvation_handler(&cp::flight_on_starvation, &r);

    crd::jobs::Config cfg;
    cfg.num_threads             = 2U; // main (not pumping) + one worker: the sole drainer
    cfg.hang_watchdog_period_ms = 20U;
    crd::jobs::init(cfg);

    constexpr int      normal_jobs = 300;
    crd::jobs::JobDecl normals[normal_jobs];
    for (auto& j : normals)
    {
        j.fn = &busy_recording_1ms;
    }
    crd::jobs::Counter* const nc = crd::jobs::run({normals, static_cast<crd::usize>(normal_jobs)});
    crd::jobs::JobDecl        low{};
    low.fn                       = &low_job;
    low.priority                 = crd::jobs::Priority::Low;
    crd::jobs::Counter* const lc = crd::jobs::run(low);

    // The watchdog fires after ~3 windows; the busy worker keeps recording, which fills the 16-event post window.
    for (int i = 0; i < 400 && !(r.capture_pending() && r.poll()); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    cp::FlightCapture c;
    const bool        got = r.take_capture(c);

    crd::jobs::wait(nc);
    crd::jobs::wait(lc);
    crd::jobs::shutdown();
    crd::jobs::set_starvation_handler(nullptr, nullptr);
    g_job_recorder = nullptr;

    REQUIRE(got);
    CHECK(capture_is_consistent(c));
    CHECK(c.trigger.kind == cp::FlightTrigger::QueueStarvation);
    CHECK(c.trigger.code == cp::kFlightCodeQueueStarvation);
    CHECK(c.trigger.value_a == 2U); // the Low lane
    CHECK(c.trigger.value_b >= 1U); // its waiting job
    CHECK(c.end == cp::FlightEnd::PostEventsFull);
    CHECK(c.pre_retained > 0U);
    CHECK(c.post_retained == 16U);
    CHECK(c.events[0].message_view() == "normal job");
    CHECK(g_low_ran.load(std::memory_order_acquire));
}

namespace
{
void starved_job(void* /*data*/) noexcept {}
} // namespace

TEST_CASE("flight trigger: the jobs watchdog's hang report triggers with its classified kind",
          "[perf][diag][diag9b][flight]")
{
    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(64U, 16U, 4U)) == cp::FlightPolicyStatus::Committed);
    crd::jobs::set_hang_handler(&cp::flight_on_hang, &r);

    crd::jobs::Config cfg;
    cfg.num_threads             = 1U; // main is the only executor and does not pump: the queued job is starved
    cfg.hang_watchdog_period_ms = 20U;
    crd::jobs::init(cfg);
    crd::jobs::JobDecl j{};
    j.fn                            = &starved_job;
    crd::jobs::Counter* const handle = crd::jobs::run(j);

    // Main keeps recording while it waits, so the window has events on both sides of the watchdog's report.
    crd::u32 i = 0U;
    for (int tick = 0; tick < 1000 && !(r.capture_pending() && r.poll()); ++tick)
    {
        record_n(r, i, 1U);
        ++i;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    cp::FlightCapture c;
    const bool        got = r.take_capture(c);

    (void)crd::jobs::pump_main_thread_until_idle();
    crd::jobs::wait(handle);
    crd::jobs::shutdown();
    crd::jobs::set_hang_handler(nullptr, nullptr);

    REQUIRE(got);
    CHECK(capture_is_consistent(c));
    CHECK(c.trigger.kind == cp::FlightTrigger::Hang);
    CHECK(c.trigger.code == cp::kFlightCodeHang);
    CHECK(c.trigger.value_a == static_cast<crd::u64>(crd::jobs::HangKind::ExecutorStarved));
    CHECK(c.trigger.value_b >= 1U); // the starved job's counter
    CHECK(c.pre_retained > 0U);
    CHECK(c.post_retained == 4U);
}

TEST_CASE("flight trigger: a frame interval over the threshold is a hitch whose frame ends the pre window",
          "[perf][diag][diag9b][flight]")
{
    cp::FlightRecorder r;
    cp::FlightPolicy   p = small_policy(64U, 16U, 4U);
    p.hitch_threshold_ns = 20000000U; // 20 ms
    REQUIRE(r.init(p) == cp::FlightPolicyStatus::Committed);
    cp::FlightFrameWatch watch(&r);

    crd::u64 t = 1000000000U;
    for (int i = 0; i < 10; ++i)
    {
        CHECK_FALSE(watch.note_frame(t, t + 4000000U)); // 16.6 ms cadence, 4 ms of work
        t += 16600000U;
    }
    t += 30000000U; // a 46.6 ms gap
    CHECK(watch.note_frame(t, t + 4000000U));
    CHECK(watch.hitches() == 1U);
    for (int i = 0; i < 4; ++i)
    {
        t += 16600000U;
        CHECK_FALSE(watch.note_frame(t, t + 4000000U));
    }

    cp::FlightCapture c;
    REQUIRE(r.take_capture(c));
    CHECK(capture_is_consistent(c));
    CHECK(c.trigger.kind == cp::FlightTrigger::FrameHitch);
    CHECK(c.trigger.value_a == 46600000U);
    CHECK(c.trigger.value_b == 20000000U);
    CHECK(c.pre_retained == 11U);
    const cp::FlightEvent& hitch_frame = c.events[c.pre_retained - 1U];
    CHECK(hitch_frame.code == cp::kFlightCodeFrame);
    CHECK(hitch_frame.value_a == 46600000U);
    CHECK(hitch_frame.value_b == 4000000U);
    CHECK(c.events[0].value_a == 0U); // the first frame has no interval
    CHECK(c.post_retained == 4U);
}

TEST_CASE("flight trigger: allocation failure and resource pressure trigger through the allocator seams",
          "[perf][diag][diag9b][flight]")
{
    SECTION("an allocator that runs out")
    {
        cp::FlightRecorder r;
        REQUIRE(r.init(small_policy(16U, 8U, 2U)) == cp::FlightPolicyStatus::Committed);
        crd::memory::LinearAllocator linear(4096U, nullptr, "scratch-linear");
        cp::FlightAllocatorWatch     watch(&linear, &r);

        record_n(r, 0U, 3U);
        void* ok = watch.try_allocate(1024U, 16U);
        CHECK(ok != nullptr);
        CHECK(watch.owns(ok));
        CHECK(watch.failures() == 0U);
        CHECK(watch.try_allocate(1U << 20U, 16U) == nullptr);
        CHECK(watch.failures() == 1U);
        record_n(r, 3U, 2U);

        cp::FlightCapture c;
        REQUIRE(r.take_capture(c));
        CHECK(c.trigger.kind == cp::FlightTrigger::AllocationFailure);
        CHECK(c.trigger.value_a == (1U << 20U));
        CHECK(c.trigger.value_b == 16U);
        CHECK(c.trigger.message_view().find("scratch-linear") != cont::StringView::npos);
        CHECK(c.pre_retained == 3U);
        watch.deallocate(ok);
    }

    SECTION("a streaming category over its budget")
    {
        cp::FlightRecorder r;
        REQUIRE(r.init(small_policy(16U, 8U, 2U)) == cp::FlightPolicyStatus::Committed);
        cp::FlightPressurePolicy                pressure(nullptr, &r);
        crd::memory::StreamingAllocator::Config cfg;
        cfg.reserve_bytes        = crd::usize{64} << 20;
        cfg.resident_chunk_bytes = crd::usize{4} << 20;
        cfg.staging_bytes        = crd::usize{4} << 20;
        crd::memory::StreamingAllocator streaming(cfg, &pressure, "flight-streaming");
        streaming.set_budget(3U, 8192U);

        void* fits = streaming.try_allocate_resident(3U, 4096U);
        REQUIRE(fits != nullptr);
        CHECK_FALSE(r.capture_pending());
        CHECK(streaming.try_allocate_resident(3U, 8192U) == nullptr); // over budget, and nothing to shed
        REQUIRE(r.capture_pending());
        record_n(r, 0U, 2U);

        cp::FlightCapture c;
        REQUIRE(r.take_capture(c));
        CHECK(c.trigger.kind == cp::FlightTrigger::ResourcePressure);
        CHECK(c.trigger.value_a == 3U);
        CHECK(c.trigger.value_b >= 4096U);
        CHECK(c.trigger.message_view().find("flight-streaming category 3") != cont::StringView::npos);
        streaming.release_resident(3U, fits);
    }
}

// This case caught the post window counting events that landed between a trigger's total read and its state store (a
// capture with more than post_events after the trigger). Since DIAG.11b the trigger and every claim move one recording
// word by compare-exchange, so the boundary is exact and no event is claimed after the window closed.
TEST_CASE("flight recorder: concurrent recording, triggering and collection keep every capture consistent",
          "[perf][diag][diag9b][flight][concurrency]")
{
    cp::FlightRecorder r;
    REQUIRE(r.init(small_policy(128U, 64U, 32U)) == cp::FlightPolicyStatus::Committed);

    std::atomic<bool> stop{false};
    std::atomic<int>  bad{0};
    std::atomic<int>  captures{0};
    std::thread       recorders[4];
    for (auto& t : recorders)
    {
        t = std::thread([&r, &stop] {
            crd::u32 i = 0U;
            while (!stop.load(std::memory_order_acquire))
            {
                r.record(0x0060U, cp::Severity::Info, cont::StringView{"worker"}, i++, 0U);
            }
        });
    }
    std::thread triggerer([&r, &stop] {
        while (!stop.load(std::memory_order_acquire))
        {
            (void)r.trigger(manual("concurrent"));
            std::this_thread::yield();
        }
    });

    for (int round = 0; round < 600; ++round)
    {
        cp::FlightCapture c;
        if (r.take_capture(c))
        {
            captures.fetch_add(1);
            if (!capture_is_consistent(c) || c.end != cp::FlightEnd::PostEventsFull || c.post_retained != 32U ||
                c.trigger.message_view() != "concurrent")
            {
                bad.fetch_add(1);
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : recorders)
    {
        t.join();
    }
    triggerer.join();

    CHECK(bad.load() == 0);
    CHECK(captures.load() > 0);
}
