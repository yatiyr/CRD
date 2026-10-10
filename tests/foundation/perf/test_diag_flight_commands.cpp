// DIAG.9b -- the flight policy and fault plans as authored assets, the flight.* commands and the doctor's live modes.
// The committed assets under assets/diag are parsed here (the default policy is held equal to FlightPolicy{}), every
// refusal names its key and leaves the output untouched, the commands run through a real DiagCommandService with the
// recorder's captures delivered to a FlightCommandHost, and the doctor's two live modes are checked against an
// independent probe of the fault seam. On a shipping lane fault.arm is refused Unavailable and the doctor reports
// fault injection compiled out: those cases assert that branch instead.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include <crd/perf/bundle.hpp>
#include <crd/perf/diag_assets.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/doctor.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_commands.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <crd/core/fault_seam.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers;

constexpr cp::DiagAuthoritySet kRead = cp::authority_bit(cp::DiagAuthority::Read);
constexpr cp::DiagAuthoritySet kReadRecord =
    cp::authority_bit(cp::DiagAuthority::Read) | cp::authority_bit(cp::DiagAuthority::Record);
constexpr cp::DiagAuthoritySet kReadInject =
    cp::authority_bit(cp::DiagAuthority::Read) | cp::authority_bit(cp::DiagAuthority::Inject);

bool contains(const cont::String& text, const char* needle)
{
    return std::strstr(text.c_str(), needle) != nullptr;
}

cont::String scratch_path(const char* name)
{
    cont::String path{cont::StringView{CRD_DIAG_COMMANDS_SCRATCH}};
    path.push_back('/');
    path.append(name);
    return path;
}

void write_text(const cont::String& path, const char* text)
{
    std::FILE* fp = nullptr;
#if defined(_MSC_VER)
    REQUIRE(fopen_s(&fp, path.c_str(), "wb") == 0);
#else
    fp = std::fopen(path.c_str(), "wb");
#endif
    REQUIRE(fp != nullptr);
    const std::size_t n = std::strlen(text);
    REQUIRE(std::fwrite(text, 1U, n, fp) == n);
    (void)std::fclose(fp);
}

cont::Array<crd::u8> read_bytes(const cont::String& path)
{
    cont::Array<crd::u8> bytes;
    std::FILE*           fp = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&fp, path.c_str(), "rb") != 0)
    {
        fp = nullptr;
    }
#else
    fp = std::fopen(path.c_str(), "rb");
#endif
    REQUIRE(fp != nullptr);
    crd::u8     chunk[4096];
    std::size_t got = 0U;
    while ((got = std::fread(chunk, 1U, sizeof(chunk), fp)) > 0U)
    {
        for (std::size_t i = 0U; i < got; ++i)
        {
            bytes.push_back(chunk[i]);
        }
    }
    (void)std::fclose(fp);
    return bytes;
}

cont::String asset_text(const char* name)
{
    cont::String path{cont::StringView{CRD_DIAG_ASSETS_DIR}};
    path.push_back('/');
    path.append(name);
    cont::String text;
    REQUIRE(cp::read_diag_asset_file(path.c_str(), text) == cp::DiagAssetRead::Ok);
    return text;
}

bool same_policy(const cp::FlightPolicy& a, const cp::FlightPolicy& b)
{
    return a.schema_version == b.schema_version && a.capacity == b.capacity && a.pre_events == b.pre_events &&
           a.post_events == b.post_events && a.post_window_ns == b.post_window_ns &&
           a.trigger_mask == b.trigger_mask && a.hitch_threshold_ns == b.hitch_threshold_ns;
}

bool same_plan(const cp::FaultPlan& a, const cp::FaultPlan& b)
{
    cont::String ta;
    cont::String tb;
    cp::fault_plan_to_text(a, ta);
    cp::fault_plan_to_text(b, tb);
    return ta == tb;
}

// A sentinel policy no document in these cases produces, so an untouched output is visible.
cp::FlightPolicy sentinel_policy()
{
    cp::FlightPolicy p;
    p.capacity    = 7U;
    p.pre_events  = 3U;
    p.post_events = 2U;
    return p;
}

// Parse `text` as a flight policy that must be refused with `needle` in the reason, leaving the output untouched.
void check_policy_refused(const char* text, const char* needle)
{
    cp::FlightPolicy out = sentinel_policy();
    cont::String     reason;
    INFO(text);
    CHECK_FALSE(cp::parse_flight_policy_asset(cont::StringView{text}, out, reason));
    INFO(reason.c_str());
    CHECK(contains(reason, needle));
    CHECK(same_policy(out, sentinel_policy()));
}

void check_plan_refused(const char* text, const char* needle)
{
    cp::FaultPlan out;
    out.seed       = 99U;
    out.rule_count = 0U;
    cont::String reason;
    INFO(text);
    CHECK_FALSE(cp::parse_fault_plan_asset(cont::StringView{text}, out, reason));
    INFO(reason.c_str());
    CHECK(contains(reason, needle));
    CHECK(out.seed == 99U);
    CHECK(out.rule_count == 0U);
}

cp::FlightPolicy small_policy()
{
    cp::FlightPolicy p;
    p.capacity       = 32U;
    p.pre_events     = 8U;
    p.post_events    = 2U;
    p.post_window_ns = 0U;
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

cp::DiagRequest command(const char* name, const char* path = "")
{
    cp::DiagRequest r;
    r.command    = cont::StringView{name};
    r.path       = cont::StringView{path};
    r.page_items = cp::kDiagMaxPageItems;
    r.page_bytes = cp::kDiagMaxPageBytes;
    return r;
}

// Whether the I/O fault seam is compiled into this build, probed by installing a hook rather than asking the doctor.
bool seam_installs()
{
    const auto hook = [](void*, crd::IoFaultOp, const char*, crd::usize, crd::u64, crd::u64) noexcept
    {
        return crd::IoFault{};
    };
    if (!crd::install_io_fault_hook(+hook, nullptr))
    {
        return false;
    }
    crd::uninstall_io_fault_hook();
    return true;
}
} // namespace

TEST_CASE("diag assets: the committed flight policy is the in-code default, and policies round-trip",
          "[perf][diag][diag9b][flight][assets]")
{
    cp::FlightPolicy   committed = sentinel_policy();
    cont::String       reason;
    const cont::String text = asset_text("flight_default.flight.toml");
    REQUIRE(cp::parse_flight_policy_asset(cont::StringView{text.data(), text.size()}, committed, reason));
    CHECK(same_policy(committed, cp::FlightPolicy{}));

    cp::FlightPolicy custom;
    custom.capacity           = 4096U;
    custom.pre_events         = 1000U;
    custom.post_events        = 96U;
    custom.post_window_ns     = 0U;
    custom.hitch_threshold_ns = 16666667U;
    custom.trigger_mask       = cp::flight_trigger_bit(cp::FlightTrigger::Assertion) |
                          cp::flight_trigger_bit(cp::FlightTrigger::InjectedFault);
    cont::String document;
    cp::flight_policy_to_asset(custom, document);
    CHECK(contains(document, "triggers = [\"assertion\", \"injected-fault\"]"));
    cp::FlightPolicy back = sentinel_policy();
    reason.clear();
    REQUIRE(cp::parse_flight_policy_asset(cont::StringView{document.data(), document.size()}, back, reason));
    CHECK(same_policy(back, custom));

    cp::flight_policy_to_asset(cp::FlightPolicy{}, document);
    CHECK(contains(document, "triggers = [\"all\"]"));

    // Keys left out keep the in-code defaults.
    cp::FlightPolicy minimal = sentinel_policy();
    REQUIRE(cp::parse_flight_policy_asset("kind = \"flight-policy\"\nschema_version = 1\n", minimal, reason));
    CHECK(same_policy(minimal, cp::FlightPolicy{}));
}

TEST_CASE("diag assets: a refused flight-policy document names its key and leaves the policy untouched",
          "[perf][diag][diag9b][flight][assets]")
{
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 1\ncapcity = 64\n", "unknown key \"capcity\"");
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 1\n[capacity]\nx = 1\n", "capacity: expected an");
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 1\ncapacity = \"64\"\n", "capacity: expected an");
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 1\ncapacity = 64.0\n", "capacity: expected an");
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 1\npre_events = -1\n",
                         "pre_events: must not be negative");
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 1\npost_events = 4294967296\n",
                         "post_events: is larger than 4294967295");
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 1\ntriggers = [\"assertion\", \"oops\"]\n",
                         "triggers: unknown trigger \"oops\"");
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 1\ntriggers = \"all\"\n",
                         "triggers: expected an array");
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 1\ntriggers = []\n", "rejected-out-of-bounds");
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 1\ncapacity = 64\npre_events = 60\n"
                         "post_events = 5\n",
                         "rejected-out-of-bounds");
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 1\npost_window_ns = 60000000001\n",
                         "rejected-out-of-bounds");
    check_policy_refused("schema_version = 1\ncapacity = 64\n", "the document has no kind");
    check_policy_refused("kind = \"fault-plan\"\nschema_version = 1\n", "kind: expected \"flight-policy\"");
    check_policy_refused("kind = \"flight-policy\"\ncapacity = 64\n", "the document has no schema_version");
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 2\n",
                         "schema_version: this build reads version 1");
    check_policy_refused("kind = \"flight-policy\"\nschema_version = 1\ncapacity = = 3\n", "not valid TOML");
}

TEST_CASE("diag assets: a fault-plan document is the plan its text names, and malformed ones are refused",
          "[perf][diag][diag9b][fault][assets]")
{
    cp::FaultPlan      committed;
    cont::String       reason;
    const cont::String text = asset_text("allocation_failure_once.fault.toml");
    REQUIRE(cp::parse_fault_plan_asset(cont::StringView{text.data(), text.size()}, committed, reason));
    cp::FaultPlan from_text;
    REQUIRE(cp::parse_fault_plan("seed=1;allocation-failure@0+1%1000000", from_text, reason));
    CHECK(same_plan(committed, from_text));
    CHECK(committed.trigger_flight);

    cp::FaultPlan quiet;
    REQUIRE(cp::parse_fault_plan_asset("kind = \"fault-plan\"\nschema_version = 1\nseed = 42\n"
                                       "trigger_flight = false\nrules = [\"partial-read@2+0%250000=16\", "
                                       "\"delayed-completion@0+1%1000000=5000000\"]\n",
                                       quiet, reason));
    REQUIRE(cp::parse_fault_plan("seed=42;notrigger;partial-read@2+0%250000=16;delayed-completion@0+1%1000000=5000000",
                                 from_text, reason));
    CHECK(same_plan(quiet, from_text));
    CHECK_FALSE(quiet.trigger_flight);

    check_plan_refused("kind = \"fault-plan\"\nschema_version = 1\nseed = 1\nrule = [\"allocation-failure@0+1%1\"]\n",
                       "unknown key \"rule\"");
    check_plan_refused("kind = \"fault-plan\"\nschema_version = 1\nseed = -3\nrules = [\"allocation-failure@0+1%1\"]\n",
                       "seed: must not be negative");
    check_plan_refused("kind = \"fault-plan\"\nschema_version = 1\ntrigger_flight = 1\n"
                       "rules = [\"allocation-failure@0+1%1\"]\n",
                       "trigger_flight: expected true or false");
    check_plan_refused("kind = \"fault-plan\"\nschema_version = 1\n"
                       "rules = [\"allocation-failure@0+1%1;failed-read@0+0%1000000\"]\n",
                       "rules: a rule is");
    check_plan_refused("kind = \"fault-plan\"\nschema_version = 1\nrules = [\"notrigger\"]\n", "rules: a rule is");
    check_plan_refused("kind = \"fault-plan\"\nschema_version = 1\nrules = [\"allocation-failure@0+1%0\"]\n",
                       "bad-probability");
    check_plan_refused("kind = \"fault-plan\"\nschema_version = 1\nrules = []\n", "no-rules");
    check_plan_refused("kind = \"fault-plan\"\nschema_version = 1\nrules = [\"a@0+1%1\", \"a@0+1%1\", \"a@0+1%1\", "
                       "\"a@0+1%1\", \"a@0+1%1\", \"a@0+1%1\", \"a@0+1%1\", \"a@0+1%1\", \"a@0+1%1\"]\n",
                       "a rule is not");
    check_plan_refused("kind = \"fault-plan\"\nschema_version = 1\nrules = [\"failed-read@0+1%1000000\", "
                       "\"failed-read@1+1%1000000\", \"failed-read@2+1%1000000\", \"failed-read@3+1%1000000\", "
                       "\"failed-read@4+1%1000000\", \"failed-read@5+1%1000000\", \"failed-read@6+1%1000000\", "
                       "\"failed-read@7+1%1000000\", \"failed-read@8+1%1000000\"]\n",
                       "more than 8 rules");
    check_plan_refused("kind = \"flight-policy\"\nschema_version = 1\n", "kind: expected \"fault-plan\"");
}

TEST_CASE("diag assets: an asset file over the bound is refused, and a missing one says so",
          "[perf][diag][diag9b][assets]")
{
    const cont::String big = scratch_path("diag_assets_big.toml");
    {
        cont::String text;
        for (crd::u32 i = 0U; i <= cp::kDiagAssetMaxBytes; ++i)
        {
            text.push_back('#');
        }
        write_text(big, text.c_str());
    }
    cont::String out{cont::StringView{"stale"}};
    CHECK(cp::read_diag_asset_file(big.c_str(), out) == cp::DiagAssetRead::TooLarge);
    CHECK(out.empty());
    (void)std::remove(big.c_str());
    const cont::String missing = scratch_path("diag_assets_missing.toml");
    CHECK(cp::read_diag_asset_file(missing.c_str(), out) == cp::DiagAssetRead::Missing);
}

TEST_CASE("flight commands: trigger, status, write and policy reload through the command service",
          "[perf][diag][diag9b][flight][commands]")
{
    cp::FlightRecorder recorder;
    REQUIRE(recorder.init(small_policy()) == cp::FlightPolicyStatus::Committed);
    cp::FlightCommandHost host(recorder);
    host.install_sink();

    cp::DiagServiceConfig config;
    config.root            = cont::StringView{CRD_DIAG_COMMANDS_SCRATCH};
    config.flight_recorder = &recorder;
    cp::DiagCommandService service{kReadRecord, config};
    REQUIRE(cp::register_flight_commands(service, host));

    const cont::String bundle_path = scratch_path("diag_flight_commands.cdb");
    (void)std::remove(bundle_path.c_str());

    // Nothing delivered yet: status answers the recorder and no retained capture; write has nothing to write.
    const cp::DiagResult empty = service.execute(command("flight.status"));
    REQUIRE(empty.status == cp::DiagStatus::Ok);
    CHECK(contains(empty.json, "\"initialized\":true,\"generation\":1,\"capacity\":32,\"pre_events\":8,"
                               "\"post_events\":2"));
    CHECK(contains(empty.json, "\"triggers\":\"all\""));
    CHECK(contains(empty.json, "\"retained\":false"));
    const cp::DiagResult nothing = service.execute(command("flight.write", "diag_flight_commands.cdb"));
    CHECK(nothing.status == cp::DiagStatus::Failed);
    CHECK(contains(nothing.reason, "no flight capture"));

    record_n(recorder, 0U, 12U);
    cp::DiagArg     message{"message", "agent probe"};
    cp::DiagRequest trigger = command("flight.trigger");
    trigger.args            = cont::ConstSpan<cp::DiagArg>{&message, 1U};
    const cp::DiagResult started = service.execute(trigger);
    REQUIRE(started.status == cp::DiagStatus::Ok);
    CHECK(contains(started.json, "\"result\":\"started\""));
    CHECK(recorder.capture_pending());

    // A second trigger while one is pending is refused and counted, and a reload is refused busy.
    const cp::DiagResult again = service.execute(command("flight.trigger"));
    CHECK(again.status == cp::DiagStatus::Failed);
    CHECK(contains(again.reason, "suppressed"));
    const cont::String good_asset = scratch_path("diag_flight_policy_good.flight.toml");
    write_text(good_asset, "kind = \"flight-policy\"\nschema_version = 1\ncapacity = 64\npre_events = 40\n"
                           "post_events = 4\npost_window_ns = 0\ntriggers = [\"assertion\", \"injected-fault\"]\n");
    const cp::DiagResult busy = service.execute(command("flight.policy", "diag_flight_policy_good.flight.toml"));
    CHECK(busy.status == cp::DiagStatus::Failed);
    CHECK(contains(busy.reason, "rejected-busy"));
    CHECK(contains(busy.reason, "the active policy stays at generation 1"));
    CHECK(recorder.policy_generation() == 1U);

    // The post window fills; flight.write collects the capture (poll) and writes it.
    record_n(recorder, 12U, 2U);
    REQUIRE(host.received() == 0U);
    const cp::DiagResult written = service.execute(command("flight.write", "diag_flight_commands.cdb"));
    REQUIRE(written.status == cp::DiagStatus::Ok);
    CHECK(host.received() == 1U);
    CHECK(contains(written.json, "\"trigger\":\"manual\",\"end\":\"post-events-full\",\"events\":10"));

    const cont::Array<crd::u8> bytes = read_bytes(bundle_path);
    const cp::BundleReadResult read  = cp::read_bundle(cont::ConstSpan<crd::u8>{bytes.data(), bytes.size()});
    REQUIRE(read.status == cp::BundleReadStatus::Ok);
    const cp::BundleSectionView* section = nullptr;
    for (const cp::BundleSectionView& s : read.sections)
    {
        if (s.tag == cp::BundleSectionTag::FlightRecord)
        {
            section = &s;
        }
    }
    REQUIRE(section != nullptr);
    const cp::FlightRecordRead decoded = cp::decode_flight_record(section->payload);
    REQUIRE(decoded.ok);
    CHECK(decoded.capture.trigger.message_view() == cont::StringView{"agent probe"});
    CHECK(decoded.capture.pre_retained == 8U);
    CHECK(decoded.capture.post_retained == 2U);
    CHECK(decoded.capture.suppressed_triggers == 1U);
    REQUIRE(decoded.capture.events.size() == 10U);
    CHECK(decoded.capture.events[0].sequence == 4U);
    CHECK(decoded.capture.events[9].sequence == 13U);

    // Never overwrite: a second write is refused and the file keeps its bytes.
    const cp::DiagResult overwrite = service.execute(command("flight.write", "diag_flight_commands.cdb"));
    CHECK(overwrite.status == cp::DiagStatus::Failed);
    CHECK(contains(overwrite.reason, "refusing to overwrite"));
    const cont::Array<crd::u8> after = read_bytes(bundle_path);
    REQUIRE(after.size() == bytes.size());
    CHECK(std::memcmp(after.data(), bytes.data(), bytes.size()) == 0);

    // Status now carries the retained capture: its summary, and its events as items.
    const cp::DiagResult seen = service.execute(command("flight.status"));
    REQUIRE(seen.status == cp::DiagStatus::Ok);
    CHECK(seen.total == 10U);
    CHECK(contains(seen.json, "\"retained\":true,\"trigger\":\"manual\""));
    CHECK(contains(seen.json, "\"trigger_message\":\"agent probe\""));
    CHECK(contains(seen.json, "\"pre_retained\":8,\"pre_dropped\":4,\"post_retained\":2"));
    CHECK(contains(seen.json, "{\"sequence\":4,\"code\":66,\"severity\":\"info\",\"a\":4,\"b\":0,"
                              "\"message\":\"event 4\""));

    // A refused document keeps the active policy and names the key.
    const cont::String bad_asset = scratch_path("diag_flight_policy_bad.flight.toml");
    write_text(bad_asset, "kind = \"flight-policy\"\nschema_version = 1\npre_event = 40\n");
    const cp::DiagResult refused = service.execute(command("flight.policy", "diag_flight_policy_bad.flight.toml"));
    CHECK(refused.status == cp::DiagStatus::BadArgument);
    CHECK(contains(refused.reason, "unknown key \"pre_event\""));
    CHECK(contains(refused.reason, "stays at generation 1"));
    CHECK(recorder.policy_generation() == 1U);
    CHECK(recorder.policy().capacity == 32U);
    const cp::DiagResult absent = service.execute(command("flight.policy", "diag_flight_policy_none.flight.toml"));
    CHECK(absent.status == cp::DiagStatus::Failed);
    CHECK(contains(absent.reason, "could not be opened"));

    // The good document commits at the next generation; manual triggers are no longer armed by it.
    const cp::DiagResult reloaded = service.execute(command("flight.policy", "diag_flight_policy_good.flight.toml"));
    REQUIRE(reloaded.status == cp::DiagStatus::Ok);
    CHECK(contains(reloaded.json, "\"status\":\"committed\",\"previous_generation\":1"));
    CHECK(contains(reloaded.json, "\"triggers\":\"assertion,injected-fault\""));
    CHECK(recorder.policy_generation() == 2U);
    CHECK(recorder.policy().capacity == 64U);
    CHECK(recorder.policy().pre_events == 40U);
    const cp::DiagResult masked = service.execute(command("flight.trigger"));
    CHECK(masked.status == cp::DiagStatus::Failed);
    CHECK(contains(masked.reason, "does not arm manual triggers"));
    CHECK_FALSE(recorder.capture_pending());

    // The service's own path checks run first.
    const crd::u64 runs = service.handler_runs();
    CHECK(service.execute(command("flight.write", "../escape.cdb")).status == cp::DiagStatus::BadArgument);
    CHECK(service.execute(command("flight.policy")).status == cp::DiagStatus::BadArgument);
    CHECK(service.handler_runs() == runs);

    (void)std::remove(bundle_path.c_str());
    (void)std::remove(good_asset.c_str());
    (void)std::remove(bad_asset.c_str());
}

TEST_CASE("flight commands: a read grant sees the status but cannot trigger, reload or write",
          "[perf][diag][diag9b][flight][commands]")
{
    cp::FlightRecorder recorder;
    REQUIRE(recorder.init(small_policy()) == cp::FlightPolicyStatus::Committed);
    cp::FlightCommandHost host(recorder);
    host.install_sink();
    cp::DiagServiceConfig config;
    config.root = cont::StringView{CRD_DIAG_COMMANDS_SCRATCH};
    cp::DiagCommandService service{kRead, config};
    REQUIRE(cp::register_flight_commands(service, host));

    const crd::u64 runs = service.handler_runs();
    CHECK(service.execute(command("flight.trigger")).status == cp::DiagStatus::Unauthorized);
    CHECK(service.execute(command("flight.policy", "x.flight.toml")).status == cp::DiagStatus::Unauthorized);
    CHECK(service.execute(command("flight.write", "x.cdb")).status == cp::DiagStatus::Unauthorized);
    CHECK(service.handler_runs() == runs);
    CHECK_FALSE(recorder.capture_pending());
    CHECK(service.execute(command("flight.status")).status == cp::DiagStatus::Ok);

    // A host whose recorder never initialised answers status, and refuses a trigger as unavailable.
    cp::FlightRecorder     idle;
    cp::FlightCommandHost  idle_host(idle);
    cp::DiagCommandService idle_service{kReadRecord, config};
    REQUIRE(cp::register_flight_commands(idle_service, idle_host));
    const cp::DiagResult idle_status = idle_service.execute(command("flight.status"));
    REQUIRE(idle_status.status == cp::DiagStatus::Ok);
    CHECK(contains(idle_status.json, "\"initialized\":false"));
    CHECK(idle_service.execute(command("flight.trigger")).status == cp::DiagStatus::Unavailable);
}

TEST_CASE("fault commands: fault.arm arms a fault-plan asset under the root and refuses an unsafe path",
          "[perf][diag][diag9b][fault][assets]")
{
    cp::FaultInjector     injector;
    cp::DiagServiceConfig config;
    config.root = cont::StringView{CRD_DIAG_ASSETS_DIR};
    cp::DiagCommandService service{kReadInject, config};
    REQUIRE(cp::register_fault_commands(service, injector));

    cp::DiagArg     unsafe{"asset", "../diag/allocation_failure_once.fault.toml"};
    cp::DiagRequest arm = command("fault.arm");
    arm.args            = cont::ConstSpan<cp::DiagArg>{&unsafe, 1U};
    const crd::u64       runs     = service.handler_runs();
    const cp::DiagResult traverse = service.execute(arm);
    CHECK(traverse.status == cp::DiagStatus::BadArgument);
    CHECK(service.handler_runs() == runs);

    cp::DiagArg both[2] = {{"asset", "allocation_failure_once.fault.toml"}, {"plan", "seed=1;failed-read@0+1%1"}};
    arm.args            = cont::ConstSpan<cp::DiagArg>{both, 2U};
    CHECK(service.execute(arm).status == cp::DiagStatus::BadArgument);

    cp::DiagArg missing{"asset", "no_such_plan.fault.toml"};
    arm.args = cont::ConstSpan<cp::DiagArg>{&missing, 1U};
    const cp::DiagResult absent = service.execute(arm);
    CHECK(absent.status == cp::DiagStatus::Failed);
    CHECK(contains(absent.reason, "could not be opened"));
    CHECK_FALSE(injector.armed());

    cp::DiagArg asset{"asset", "allocation_failure_once.fault.toml"};
    arm.args                  = cont::ConstSpan<cp::DiagArg>{&asset, 1U};
    const cp::DiagResult armed = service.execute(arm);
    if (seam_installs())
    {
        REQUIRE(armed.status == cp::DiagStatus::Ok);
        CHECK(injector.armed());
        CHECK(contains(armed.json, "\"plan\":\"seed=1;allocation-failure@0+1%1000000\""));
        injector.disarm();
    }
    else
    {
        CHECK(armed.status == cp::DiagStatus::Unavailable);
        CHECK(contains(armed.reason, "compiled out"));
        CHECK_FALSE(injector.armed());
    }

    // Without a file root an asset cannot be named.
    cp::DiagCommandService rootless{kReadInject, cp::DiagServiceConfig{}};
    REQUIRE(cp::register_fault_commands(rootless, injector));
    const cp::DiagResult no_root = rootless.execute(arm);
    CHECK(no_root.status == cp::DiagStatus::Unavailable);
    CHECK(contains(no_root.reason, "no file root"));
    CHECK_FALSE(injector.armed());
}

TEST_CASE("doctor: the flight-recorder and fault-injection modes are reported live",
          "[perf][diag][diag9b][doctor]")
{
    const bool seams = seam_installs();

    const cp::DoctorReport bare = cp::run_doctor();
    const cp::DiagnosticMode* flight = bare.find_mode("flight-recorder");
    const cp::DiagnosticMode* faults = bare.find_mode("fault-injection");
    REQUIRE(flight != nullptr);
    REQUIRE(faults != nullptr);
    CHECK(flight->compiled);
    CHECK_FALSE(flight->enabled);
    CHECK_FALSE(flight->usable);
    CHECK(faults->compiled == seams);
    CHECK_FALSE(faults->enabled);
    CHECK_FALSE(faults->usable);

    cp::FlightRecorder recorder;
    cp::FaultInjector  injector;
    cp::DoctorLive     live;
    live.flight_recorder = &recorder;
    live.fault_injector  = &injector;
    {
        const cp::DoctorReport r = cp::run_doctor(cont::StringView{}, live);
        CHECK_FALSE(r.find_mode("flight-recorder")->enabled); // installed, not initialised
        CHECK(r.find_mode("fault-injection")->usable == seams);
        CHECK_FALSE(r.find_mode("fault-injection")->enabled);
    }

    REQUIRE(recorder.init(small_policy()) == cp::FlightPolicyStatus::Committed);
    cp::FaultPlan plan;
    cont::String  reason;
    REQUIRE(cp::parse_fault_plan("seed=3;failed-read@0+1%1000000", plan, reason));
    const cp::FaultArmStatus arm = injector.arm(plan, kReadInject);
    CHECK(arm == (seams ? cp::FaultArmStatus::Armed : cp::FaultArmStatus::Unavailable));
    {
        const cp::DoctorReport r = cp::run_doctor(cont::StringView{}, live);
        CHECK(r.find_mode("flight-recorder")->enabled);
        CHECK(r.find_mode("flight-recorder")->usable);
        CHECK(r.find_mode("fault-injection")->compiled == seams);
        CHECK(r.find_mode("fault-injection")->enabled == seams);
        CHECK(r.find_mode("fault-injection")->usable == seams);
    }

    // diag.capabilities reports the same live modes through the service's config.
    cp::DiagServiceConfig config;
    config.flight_recorder = &recorder;
    config.fault_injector  = &injector;
    cp::DiagCommandService service{kRead, config};
    const cp::DiagResult   caps = service.execute(command("diag.capabilities"));
    REQUIRE(caps.status == cp::DiagStatus::Ok);
    CHECK(contains(caps.json, "{\"kind\":\"mode\",\"name\":\"flight-recorder\",\"compiled\":true,\"enabled\":true,"
                              "\"usable\":true,"));
    CHECK(contains(caps.json, seams ? "{\"kind\":\"mode\",\"name\":\"fault-injection\",\"compiled\":true,"
                                      "\"enabled\":true,\"usable\":true,"
                                    : "{\"kind\":\"mode\",\"name\":\"fault-injection\",\"compiled\":false,"
                                      "\"enabled\":false,\"usable\":false,"));
    injector.disarm();

    // A capture that ended on a fatal path is terminal: the recorder is no longer usable.
    cp::FlightTriggerInput input;
    REQUIRE(recorder.trigger(input) == cp::FlightTriggerResult::Started);
    REQUIRE(recorder.finish(cp::FlightEnd::Fatal));
    const cp::DoctorReport after = cp::run_doctor(cont::StringView{}, live);
    CHECK(after.find_mode("flight-recorder")->enabled);
    CHECK_FALSE(after.find_mode("flight-recorder")->usable);
}
