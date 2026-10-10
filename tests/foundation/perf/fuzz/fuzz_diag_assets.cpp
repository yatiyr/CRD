// crd-fuzz-perf-assets: the authored diagnostic-policy readers -- parse_flight_policy_asset, parse_fault_plan_asset
// and parse_fault_plan (the plan text `fault.arm plan=` takes, which the asset reader also assembles and reads) --
// and crd-toml beneath the two documents, under the bounded fuzz harness (DIAG.10a; docs/design/test-instruments.md).
// Every input is offered to all three readers.
//
// Oracles:
//   * a refusal names its reason and leaves the output exactly as it was (each reader is handed a distinctive
//     policy or plan first, and it must come back unchanged);
//   * an accepted flight policy passes validate_flight_policy, and its canonical document
//     (flight_policy_to_asset) reads back to the same policy and writes the same document again;
//   * an accepted fault plan, from either reader, passes validate_fault_plan, its text (fault_plan_to_text) fits
//     the 800-byte plan bound unclipped, and that text reads back to the same plan and writes the same text again;
//   * reading the same input twice gives the same verdict.
// Seeds: the committed assets, documents each refusal names (an unknown key, wrong types, a negative or oversized
// value, an unknown trigger, detail level or site, a duplicate key, a smuggled ';' or a rule without '@', a policy or
// plan its validator refuses, invalid TOML), the depth and number limits of crd-toml (nesting 64 and 65 levels deep and
// 20,000 deep, an integer one past the 64-bit range, an over-long number token), and plan texts at their bounds.
#include <crd/fuzz/harness.hpp>

#include <crd/containers/string.hpp>
#include <crd/perf/diag_assets.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <cstring>

namespace
{
using crd::u32;
using crd::usize;
using crd::containers::String;
using crd::containers::StringView;
using namespace crd::perf;

bool same_policy(const FlightPolicy& a, const FlightPolicy& b) noexcept
{
    return a.schema_version == b.schema_version && a.capacity == b.capacity && a.pre_events == b.pre_events &&
           a.post_events == b.post_events && a.post_window_ns == b.post_window_ns &&
           a.trigger_mask == b.trigger_mask && a.hitch_threshold_ns == b.hitch_threshold_ns && a.detail == b.detail;
}

bool same_plan(const FaultPlan& a, const FaultPlan& b) noexcept
{
    if (a.schema_version != b.schema_version || a.seed != b.seed || a.rule_count != b.rule_count ||
        a.trigger_flight != b.trigger_flight || a.rule_count > kFaultMaxRules)
    {
        return false;
    }
    for (u32 i = 0U; i < a.rule_count; ++i)
    {
        const FaultRule& x = a.rules[i];
        const FaultRule& y = b.rules[i];
        if (x.site != y.site || x.first_hit != y.first_hit || x.hit_limit != y.hit_limit ||
            x.probability_ppm != y.probability_ppm || x.value != y.value)
        {
            return false;
        }
    }
    return true;
}

// Distinctive outputs a refusal must leave untouched (neither equals a default nor anything a seed asks for).
FlightPolicy sentinel_policy() noexcept
{
    FlightPolicy p;
    p.capacity           = 333U;
    p.pre_events         = 111U;
    p.post_events        = 99U;
    p.post_window_ns     = 12345U;
    p.trigger_mask       = flight_trigger_bit(FlightTrigger::Manual) | flight_trigger_bit(FlightTrigger::Hang);
    p.hitch_threshold_ns = 777U;
    p.detail             = FlightDetail::Basic;
    return p;
}

FaultPlan sentinel_plan() noexcept
{
    FaultPlan p;
    p.seed                     = 4242U;
    p.trigger_flight           = false;
    p.rule_count               = 1U;
    p.rules[0].site            = FaultSite::DelayedCompletion;
    p.rules[0].first_hit       = 3U;
    p.rules[0].hit_limit       = 5U;
    p.rules[0].probability_ppm = 250000U;
    p.rules[0].value           = 1000U;
    return p;
}

// An accepted plan's text fits unclipped, reads back to the same plan and is a fixed point of write-then-read.
void check_plan_round_trip(const FaultPlan& plan)
{
    CRD_FUZZ_REQUIRE(validate_fault_plan(plan) == FaultPlanError::None);
    String text;
    fault_plan_to_text(plan, text);
    CRD_FUZZ_REQUIRE(text.size() <= kFaultMaxPlanText);
    FaultPlan back = sentinel_plan();
    String    reason;
    CRD_FUZZ_REQUIRE(parse_fault_plan(StringView{text.data(), text.size()}, back, reason));
    CRD_FUZZ_REQUIRE(same_plan(back, plan));
    String again;
    fault_plan_to_text(back, again);
    CRD_FUZZ_REQUIRE(again == text);
}

void check_policy_asset(StringView text)
{
    FlightPolicy out = sentinel_policy();
    String       reason;
    const bool   ok = parse_flight_policy_asset(text, out, reason);
    if (!ok)
    {
        CRD_FUZZ_REQUIRE(!reason.empty());
        CRD_FUZZ_REQUIRE(same_policy(out, sentinel_policy()));
        return;
    }
    CRD_FUZZ_REQUIRE(validate_flight_policy(out) == FlightPolicyStatus::Committed);
    String canonical;
    flight_policy_to_asset(out, canonical);
    FlightPolicy back = sentinel_policy();
    String       back_reason;
    CRD_FUZZ_REQUIRE(parse_flight_policy_asset(StringView{canonical.data(), canonical.size()}, back, back_reason));
    CRD_FUZZ_REQUIRE(same_policy(back, out));
    String again;
    flight_policy_to_asset(back, again);
    CRD_FUZZ_REQUIRE(again == canonical);

    FlightPolicy second = sentinel_policy();
    String       second_reason;
    CRD_FUZZ_REQUIRE(parse_flight_policy_asset(text, second, second_reason) && same_policy(second, out));
}

void check_plan_asset(StringView text)
{
    FaultPlan  out = sentinel_plan();
    String     reason;
    const bool ok = parse_fault_plan_asset(text, out, reason);
    if (!ok)
    {
        CRD_FUZZ_REQUIRE(!reason.empty());
        CRD_FUZZ_REQUIRE(same_plan(out, sentinel_plan()));
        return;
    }
    check_plan_round_trip(out);
    FaultPlan second = sentinel_plan();
    String    second_reason;
    CRD_FUZZ_REQUIRE(parse_fault_plan_asset(text, second, second_reason) && same_plan(second, out));
}

void check_plan_text(StringView text)
{
    FaultPlan  out = sentinel_plan();
    String     reason;
    const bool ok = parse_fault_plan(text, out, reason);
    if (!ok)
    {
        CRD_FUZZ_REQUIRE(!reason.empty());
        CRD_FUZZ_REQUIRE(same_plan(out, sentinel_plan()));
        return;
    }
    check_plan_round_trip(out);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const StringView text{reinterpret_cast<const char*>(data), size};
    check_policy_asset(text);
    check_plan_asset(text);
    check_plan_text(text);
    return 0;
}

namespace
{
void add(crd::fuzz::SeedSink& sink, const char* name, const String& text)
{
    sink.add(name, reinterpret_cast<const crd::u8*>(text.data()), text.size());
}

String repeated(const char* head, const char* open, u32 count, const char* middle, const char* close)
{
    String s{head};
    for (u32 i = 0U; i < count; ++i)
    {
        s.append(StringView{open});
    }
    s.append(StringView{middle});
    for (u32 i = 0U; i < count; ++i)
    {
        s.append(StringView{close});
    }
    s.push_back('\n');
    return s;
}

constexpr const char* kPolicyHead = "kind = \"flight-policy\"\nschema_version = 1\n";
constexpr const char* kPlanHead   = "kind = \"fault-plan\"\nschema_version = 1\n";

String document(const char* head, const char* body)
{
    String s{head};
    s.append(StringView{body});
    return s;
}
} // namespace

namespace
{
bool policy_accepted(const crd::u8* data, usize size)
{
    FlightPolicy out;
    String       reason;
    return parse_flight_policy_asset(StringView{reinterpret_cast<const char*>(data), size}, out, reason);
}

bool plan_asset_accepted(const crd::u8* data, usize size)
{
    FaultPlan out;
    String    reason;
    return parse_fault_plan_asset(StringView{reinterpret_cast<const char*>(data), size}, out, reason);
}

bool plan_text_accepted(const crd::u8* data, usize size)
{
    FaultPlan out;
    String    reason;
    return parse_fault_plan(StringView{reinterpret_cast<const char*>(data), size}, out, reason);
}

// The seeds each reader accepts; every other seed is refused by that reader.
constexpr const char* kPolicySeeds[]    = {"asset-flight-default", "policy-basic-listed-triggers"};
constexpr const char* kPlanAssetSeeds[] = {"asset-allocation-failure-once", "plan-every-site"};
constexpr const char* kPlanTextSeeds[]  = {"text-minimal", "text-notrigger-max-seed", "text-leading-zeros",
                                           "text-widest-plan"};
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& out)
{
    crd::fuzz::VerdictSink text_sink(out, &plan_text_accepted, kPlanTextSeeds, 4U);
    crd::fuzz::VerdictSink plan_sink(text_sink, &plan_asset_accepted, kPlanAssetSeeds, 2U);
    crd::fuzz::VerdictSink sink(plan_sink, &policy_accepted, kPolicySeeds, 2U);
    // The committed assets (assets/diag), as authored.
    sink.add_text("asset-flight-default",
                  "# The default flight policy.\n\nkind           = \"flight-policy\"\nschema_version = 1\n\n"
                  "capacity    = 1024\npre_events  = 768\npost_events = 256\n\npost_window_ns     = 2000000000\n"
                  "hitch_threshold_ns = 50000000\n\ndetail = \"full\"\n\ntriggers = [\"all\"]\n");
    sink.add_text("asset-allocation-failure-once",
                  "kind           = \"fault-plan\"\nschema_version = 1\n\nseed           = 1\n"
                  "trigger_flight = true # a fired fault also triggers the attached flight recorder\n"
                  "rules          = [\"allocation-failure@0+1%1000000\"]\n");

    // Accepted documents that use every key.
    add(sink, "policy-basic-listed-triggers",
        document(kPolicyHead, "capacity = 64\npre_events = 40\npost_events = 24\npost_window_ns = 0\n"
                              "hitch_threshold_ns = 1\ndetail = \"basic\"\ntriggers = [\"manual\", \"hang\"]\n"));
    add(sink, "plan-every-site",
        document(kPlanHead, "seed = 9223372036854775807\ntrigger_flight = false\nrules = ["
                            "\"allocation-failure@0+0%1\", \"failed-read@1+2%500000\", "
                            "\"partial-read@0+1%1000000=16\", \"delayed-completion@5+1%1000000=10000000000\", "
                            "\"cancelled-completion@0+1%1000000\", \"device-loss@18446744073709551614+1%1000000\"]\n"));

    // Refusals the readers name.
    add(sink, "policy-unknown-key", document(kPolicyHead, "capacty = 64\n"));
    add(sink, "policy-wrong-type", document(kPolicyHead, "capacity = \"64\"\n"));
    add(sink, "policy-negative", document(kPolicyHead, "pre_events = -1\n"));
    add(sink, "policy-oversized", document(kPolicyHead, "capacity = 4294967296\n"));
    add(sink, "policy-out-of-bounds", document(kPolicyHead, "capacity = 8\npre_events = 6\npost_events = 3\n"));
    add(sink, "policy-unknown-trigger", document(kPolicyHead, "triggers = [\"manual\", \"panic\"]\n"));
    add(sink, "policy-empty-triggers", document(kPolicyHead, "triggers = []\n"));
    add(sink, "policy-unknown-detail", document(kPolicyHead, "detail = \"verbose\"\n"));
    add(sink, "policy-duplicate-key", document(kPolicyHead, "capacity = 64\ncapacity = 1\n"));
    sink.add_text("policy-wrong-kind", "kind = \"fault-plan\"\nschema_version = 1\ncapacity = 64\n");
    sink.add_text("policy-wrong-version", "kind = \"flight-policy\"\nschema_version = 2\n");
    sink.add_text("policy-no-kind", "schema_version = 1\n");
    add(sink, "plan-smuggled-field", document(kPlanHead, "rules = [\"allocation-failure@0+1%1000000;notrigger\"]\n"));
    add(sink, "plan-rule-without-at", document(kPlanHead, "rules = [\"notrigger\"]\n"));
    add(sink, "plan-unknown-site", document(kPlanHead, "rules = [\"disk-full@0+1%1000000\"]\n"));
    add(sink, "plan-no-rules", document(kPlanHead, "rules = []\n"));
    add(sink, "plan-nine-rules",
        document(kPlanHead, "rules = [\"failed-read@0+1%1\", \"failed-read@1+1%1\", \"failed-read@2+1%1\", "
                            "\"failed-read@3+1%1\", \"failed-read@4+1%1\", \"failed-read@5+1%1\", "
                            "\"failed-read@6+1%1\", \"failed-read@7+1%1\", \"failed-read@8+1%1\"]\n"));
    add(sink, "plan-range-overflow", document(kPlanHead, "rules = [\"failed-read@18446744073709551615+2%1000000\"]\n"));
    add(sink, "plan-value-on-plain-site", document(kPlanHead, "rules = [\"failed-read@0+1%1000000=4\"]\n"));
    add(sink, "plan-trigger-not-bool", document(kPlanHead, "trigger_flight = 1\nrules = [\"device-loss@0+1%1\"]\n"));
    sink.add_text("invalid-toml", "kind = \"flight-policy\nschema_version = 1\n");

    // crd-toml's limits beneath both documents (DIAG.10a findings): nesting exactly at and one past 64 levels, far
    // past it (a stack overflow before the limit existed; 15,000 inline tables stay under the 64 KiB input bound), an
    // integer one past the 64-bit range (read as INT64_MAX before) and a number token longer than 63 characters (read
    // as its first 63 before).
    add(sink, "toml-nesting-64", repeated("x = ", "[", 64U, "1", "]"));
    add(sink, "toml-nesting-65", repeated("x = ", "[", 65U, "1", "]"));
    add(sink, "toml-nesting-20000", repeated("x = ", "[", 20000U, "", "]"));
    add(sink, "toml-inline-tables-15000", repeated("x = ", "{a=", 15000U, "1", "}"));
    {
        String header{"[t"};
        for (u32 i = 0U; i < 10000U; ++i)
        {
            header.append(StringView{".t"});
        }
        header.append(StringView{"]\nk = 1\n"});
        add(sink, "toml-header-path-10000", header);
    }
    add(sink, "toml-integer-past-range",
        document(kPlanHead, "seed = 9223372036854775808\nrules = [\"device-loss@0+1%1\"]\n"));
    {
        String body{"hitch_threshold_ns = 1"};
        for (u32 i = 0U; i < 70U; ++i)
        {
            body.push_back('0');
        }
        body.push_back('\n');
        add(sink, "toml-number-too-long", document(kPolicyHead, body.c_str()));
    }

    // Plan texts (the fault.arm grammar) at their bounds.
    sink.add_text("text-minimal", "seed=0;allocation-failure@0+1%1000000");
    sink.add_text("text-notrigger-max-seed", "seed=18446744073709551615;notrigger;partial-read@0+0%1=1");
    sink.add_text("text-leading-zeros", "seed=007;failed-read@00+01%0001000");
    sink.add_text("text-seed-overflow", "seed=18446744073709551616;failed-read@0+1%1");
    sink.add_text("text-trailing-separator", "seed=1;failed-read@0+1%1;");
    sink.add_text("text-notrigger-late", "seed=1;failed-read@0+1%1;notrigger");
    sink.add_text("text-delay-too-long", "seed=1;delayed-completion@0+1%1000000=10000000001");
    {
        // Eight wide rules after the widest seed and the flag; one more rule is refused.
        String widest{"seed=18446744073709551615;notrigger"};
        for (u32 i = 0U; i < kFaultMaxRules; ++i)
        {
            widest.append(StringView{";cancelled-completion@18446744073709551614+1%1000000"});
        }
        add(sink, "text-widest-plan", widest);
        widest.append(StringView{";cancelled-completion@0+1%1"});
        add(sink, "text-nine-rules", widest);
    }
}
