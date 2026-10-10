// DIAG.9b: the sandbox as a flight-recorder and fault-injection host (sandbox/src/flight_host.cpp, compiled into this
// test the way the panel's sources are, and driven the way the sandbox's main drives it, without a window or a device).
//
// Covered: the host starts the recorder from an authored policy asset and takes its module snapshot; served on a real
// DiagCommandService it adds flight.* and fault.* and the doctor reports the live recorder and injector; frames fed
// the way the frame loop feeds them make a frame hitch a capture, which the host delivers, logs nothing about and
// writes to its capture directory as a bundle naming this build and this executable, never over an existing file and
// keeping the newest ten; an injected allocation failure at the resource manager's allocator is one injected-fault
// capture and a real one an allocation-failure capture (the decorators' order); a read failed through the I/O hook the
// host installed; the jobs watchdog window the host gives the pool reports a real hang into the recorder; flight.status
// keeps the replay recipe of a capture whose trigger message and recipe are both at their bounds; refused starts leave
// nothing installed, and stop() restores every seam and delivers a pending capture. A shipping build compiles the
// fault seams out: those cases check the refusals instead. ASCII test names.

#include "flight_host.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/build_identity.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_import.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/diagnostics.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_recorder.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace
{
namespace cp   = crd::perf;
namespace cont = crd::containers;
namespace fs   = crd::platform::fs;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::sandbox::FlightHost;
using crd::sandbox::FlightHostOptions;
using crd::sandbox::FlightHostStart;

constexpr cp::DiagAuthoritySet kGrant = cp::authority_bit(cp::DiagAuthority::Read) |
                                        cp::authority_bit(cp::DiagAuthority::Record) |
                                        cp::authority_bit(cp::DiagAuthority::Inject);

constexpr u64 kMs = 1000000U;

// The flight policy these cases author: small windows, bounded by events only, a 20 ms hitch.
constexpr const char* kPolicyText = "kind = \"flight-policy\"\n"
                                    "schema_version = 1\n"
                                    "capacity = 64\n"
                                    "pre_events = 16\n"
                                    "post_events = 4\n"
                                    "post_window_ns = 0\n"
                                    "hitch_threshold_ns = 20000000\n"
                                    "triggers = [\"all\"]\n";

bool contains(const cont::String& text, const char* needle)
{
    return std::strstr(text.c_str(), needle) != nullptr;
}

bool starts_with(cont::StringView text, const char* prefix)
{
    const crd::usize n = std::strlen(prefix);
    return text.size() >= n && std::memcmp(text.data(), prefix, n) == 0;
}

bool ends_with(cont::StringView text, const char* suffix)
{
    const crd::usize n = std::strlen(suffix);
    return text.size() >= n && std::memcmp(text.data() + (text.size() - n), suffix, n) == 0;
}

// A fresh directory under the OS scratch directory, removed by the destructor.
struct Scratch
{
    explicit Scratch(const char* name) : path(fs::temp_directory() / cont::StringView{name})
    {
        (void)fs::remove_all(path);
        REQUIRE(fs::create_directories(path));
    }
    ~Scratch() { (void)fs::remove_all(path); }
    Scratch(const Scratch&)            = delete;
    Scratch& operator=(const Scratch&) = delete;
    Scratch(Scratch&&)                 = delete;
    Scratch& operator=(Scratch&&)      = delete;

    [[nodiscard]] cont::String text() const { return cont::String{path.generic()}; }
    [[nodiscard]] cont::String file(const char* name) const { return cont::String{(path / name).generic()}; }

    fs::Path path;
};

void write_text(const cont::String& path, const char* text)
{
    REQUIRE(fs::write_file_text(fs::Path{cont::StringView{path.data(), path.size()}}, cont::StringView{text}));
}

cont::Array<u8> read_bytes(const cont::String& path)
{
    cont::Array<u8> bytes;
    REQUIRE(fs::read_file_binary(fs::Path{cont::StringView{path.data(), path.size()}}, bytes));
    return bytes;
}

u32 count_bundles(const fs::Path& dir)
{
    cont::Array<fs::Path> entries;
    fs::list_directory(dir, entries);
    u32 n = 0U;
    for (const fs::Path& p : entries)
    {
        const cont::String full{p.generic()};
        const char* const  slash = std::strrchr(full.c_str(), '/');
        const cont::StringView name{slash != nullptr ? slash + 1 : full.c_str()};
        if (starts_with(name, "flight-") && ends_with(name, ".cdb"))
        {
            ++n;
        }
    }
    return n;
}

cp::DiagResult execute(cp::DiagCommandService& service, const char* command, const char* path = nullptr,
                       const char* arg_name = nullptr, const char* arg_value = nullptr)
{
    cp::DiagArg     arg{cont::StringView{arg_name != nullptr ? arg_name : ""},
                    cont::StringView{arg_value != nullptr ? arg_value : ""}};
    cp::DiagRequest req;
    req.command    = cont::StringView{command};
    req.path       = cont::StringView{path != nullptr ? path : ""};
    req.page_items = 256U;
    if (arg_name != nullptr)
    {
        req.args = cont::ConstSpan<cp::DiagArg>{&arg, 1U};
    }
    return service.execute(req);
}

// The capture a bundle on disk holds, with the import's build and module list.
struct BundleRead
{
    cp::BundleImport   import;
    cp::FlightRecordRead flight;
    bool               has_flight = false;
};

BundleRead read_flight_bundle(const cont::String& path)
{
    const cont::Array<u8> bytes = read_bytes(path);
    BundleRead             out;
    out.import = cp::import_bundle(cont::ConstSpan<u8>{bytes.data(), bytes.size()});
    for (crd::usize i = 0U; i < out.import.sections.size(); ++i)
    {
        if (out.import.sections[i].tag == cp::BundleSectionTag::FlightRecord)
        {
            out.flight     = cp::decode_flight_record(out.import.sections[i].payload);
            out.has_flight = true;
        }
    }
    return out;
}

const cp::ModuleIdentity* find_this_executable(const cont::Array<cp::ModuleIdentity>& modules)
{
    for (const cp::ModuleIdentity& m : modules)
    {
        if (contains(m.name, "crd-sandbox-inspect-tests"))
        {
            return &m;
        }
    }
    return nullptr;
}

// The frame loop: `count` frames of `step_ns` each, from `t`, fed the way the sandbox feeds them. Returns how many of
// them delivered a capture.
u32 run_frames(FlightHost& host, u64& t, u32 count, u64 step_ns)
{
    u32 delivered = 0U;
    for (u32 i = 0U; i < count; ++i)
    {
        if (host.frame(t, t + step_ns))
        {
            ++delivered;
        }
        t += step_ns;
    }
    return delivered;
}

// Record `count` events and poll, so a capture's post window fills and is delivered.
void fill_post_window(FlightHost& host, u32 count)
{
    for (u32 i = 0U; i < count; ++i)
    {
        host.recorder().record(0x0042U, cp::Severity::Info, cont::StringView{"after"}, i, 0U);
    }
    (void)host.recorder().poll();
}
} // namespace

TEST_CASE("flight host: a frame hitch is a capture the host writes as a bundle naming this build and executable",
          "[sandbox][diag][diag9b][flight-host]")
{
    const Scratch scratch("crd-sandbox-flight-hitch");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);
    const Scratch      captures("crd-sandbox-flight-hitch-out");
    const cont::String capture_dir = captures.text();

    FlightHost        host;
    FlightHostOptions options;
    options.policy_asset = policy_path.c_str();
    options.capture_dir  = capture_dir.c_str();
    cont::String reason;
    REQUIRE(host.start(options, reason) == FlightHostStart::Started);
    const cp::FlightPolicy policy = host.recorder().policy();
    CHECK(policy.capacity == 64U);
    CHECK(policy.pre_events == 16U);
    CHECK(policy.post_events == 4U);
    CHECK(policy.hitch_threshold_ns == 20U * kMs);

    // The snapshot taken at start names this executable.
    const cont::Array<cp::ModuleIdentity> snapshot = host.command_host().modules();
    REQUIRE(find_this_executable(snapshot) != nullptr);

    // Served on a real service: the commands are listed and the doctor reports the live recorder and injector.
    cp::DiagServiceConfig config;
    config.root = cont::StringView{capture_dir.data(), capture_dir.size()};
    host.configure_service(config);
    CHECK(config.flight_recorder == &host.recorder());
    CHECK(config.fault_injector == &host.injector());
    cp::DiagCommandService service(kGrant, config);
    REQUIRE(host.serve(service));
    const bool seams = crd::fault_seams_compiled();
    CHECK(host.hooks_installed() == seams);
    const cp::DiagResult commands = execute(service, "diag.commands");
    REQUIRE(commands.status == cp::DiagStatus::Ok);
    for (const char* name : {"flight.status", "flight.trigger", "flight.policy", "flight.write", "fault.arm",
                             "fault.disarm", "fault.status"})
    {
        cont::String quoted(R"("name":")");
        quoted.append(name);
        quoted.push_back('"');
        CHECK(contains(commands.json, quoted.c_str()));
    }
    const cp::DiagResult caps = execute(service, "diag.capabilities");
    REQUIRE(caps.status == cp::DiagStatus::Ok);
    CHECK(contains(caps.json, "\"name\":\"flight-recorder\",\"compiled\":true,\"enabled\":true,\"usable\":true"));
    CHECK(contains(caps.json, seams ? "\"name\":\"fault-injection\",\"compiled\":true,\"enabled\":false,\"usable\":true"
                                    : "\"name\":\"fault-injection\",\"compiled\":false,\"enabled\":false,"
                                      "\"usable\":false"));

    // Twenty steady frames, then one that runs 45 ms: the frame after it starts 45 ms after it, which is the hitch, and
    // four more frames fill the post window; the capture is delivered on the fourth. Nothing was delivered before.
    u64 t = 1000U * kMs;
    CHECK(run_frames(host, t, 20U, 16U * kMs) == 0U);
    CHECK(run_frames(host, t, 1U, 45U * kMs) == 0U); // this frame runs 45 ms, so the next one starts late
    CHECK(host.hitches() == 0U);
    CHECK(run_frames(host, t, 1U, 16U * kMs) == 0U); // the hitch: its interval is 45 ms
    CHECK(host.hitches() == 1U);
    CHECK(run_frames(host, t, 3U, 16U * kMs) == 0U);
    CHECK(run_frames(host, t, 1U, 16U * kMs) == 1U);
    CHECK(host.command_host().received() == 1U);

    // The writer wrote it under the directory, as the first free name.
    REQUIRE(host.bundles_written() == 1U);
    CHECK(host.bundle_failures() == 0U);
    const cont::String written = host.last_bundle();
    CHECK(written == captures.file("flight-0.cdb"));
    const BundleRead bundle = read_flight_bundle(written);
    REQUIRE(bundle.import.status == cp::ImportStatus::Ok);
    REQUIRE(bundle.has_flight);
    REQUIRE(bundle.flight.ok);
    const cp::FlightCapture& c = bundle.flight.capture;
    CHECK(c.trigger.kind == cp::FlightTrigger::FrameHitch);
    CHECK(c.trigger.value_a == 45U * kMs); // the interval
    CHECK(c.trigger.value_b == 20U * kMs); // the threshold
    CHECK(c.end == cp::FlightEnd::PostEventsFull);
    CHECK(c.pre_retained == 16U);
    CHECK(c.post_retained == 4U);
    REQUIRE(c.events.size() == 20U);
    for (const cp::FlightEvent& e : c.events)
    {
        CHECK(e.code == cp::kFlightCodeFrame);
    }
    CHECK(c.events[15].value_a == 45U * kMs); // the hitch frame ends the pre window
    REQUIRE(bundle.import.has_build);
    cont::String differing;
    CHECK(cp::same_build_identity(bundle.import.build, cp::current_build_identity(), differing));
    REQUIRE(bundle.import.has_symbols);
    const cp::ModuleIdentity* exe = find_this_executable(bundle.import.symbols);
    REQUIRE(exe != nullptr);
    CHECK_FALSE(exe->unloaded);
    CHECK(cp::identity_matches(*exe, *find_this_executable(snapshot)));

    // The commands read the same capture.
    const cp::DiagResult status = execute(service, "flight.status");
    REQUIRE(status.status == cp::DiagStatus::Ok);
    CHECK(contains(status.json, "\"retained\":true,\"trigger\":\"frame-hitch\""));
    CHECK(contains(status.json, "\"pre_retained\":16,\"pre_dropped\":6,\"post_retained\":4"));
    const cp::DiagResult manual = execute(service, "flight.write", "manual.cdb");
    REQUIRE(manual.status == cp::DiagStatus::Ok);
    CHECK(contains(manual.json, "\"trigger\":\"frame-hitch\",\"end\":\"post-events-full\",\"events\":20"));
    CHECK(fs::is_file(captures.path / "manual.cdb"));
    host.stop();
}

TEST_CASE("flight host: the writer never overwrites and keeps the newest ten bundles",
          "[sandbox][diag][diag9b][flight-host]")
{
    const Scratch      scratch("crd-sandbox-flight-retention");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);
    const Scratch      captures("crd-sandbox-flight-retention-out");
    const cont::String capture_dir = captures.text();
    const cont::String taken       = captures.file("flight-0.cdb");
    write_text(taken, "not a bundle");

    FlightHost        host;
    FlightHostOptions options;
    options.policy_asset = policy_path.c_str();
    options.capture_dir  = capture_dir.c_str();
    cont::String reason;
    REQUIRE(host.start(options, reason) == FlightHostStart::Started);

    cp::FlightTriggerInput manual;
    manual.kind    = cp::FlightTrigger::Manual;
    manual.code    = cp::kFlightCodeManual;
    manual.message = cont::StringView{"retention"};
    REQUIRE(host.recorder().trigger(manual) == cp::FlightTriggerResult::Started);
    fill_post_window(host, 4U);
    REQUIRE(host.bundles_written() == 1U);
    CHECK(host.last_bundle() == captures.file("flight-1.cdb")); // flight-0.cdb was taken
    const cont::Array<u8> kept = read_bytes(taken);
    // Compared as a bool: a failing comparison would hand Catch2 a bundle's binary bytes to print.
    const cont::StringView kept_text{reinterpret_cast<const char*>(kept.data()), kept.size()};
    const bool             sentinel_kept = kept_text == "not a bundle";
    CHECK(sentinel_kept);

    for (u32 i = 0U; i < 11U; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(15)); // distinct write times for the retention order
        REQUIRE(host.recorder().trigger(manual) == cp::FlightTriggerResult::Started);
        fill_post_window(host, 4U);
    }
    CHECK(host.bundles_written() == 12U);
    CHECK(host.bundle_failures() == 0U);
    CHECK(host.last_bundle() == captures.file("flight-12.cdb"));
    CHECK(count_bundles(captures.path) == crd::perf::kDefaultMaxBundles);
    CHECK_FALSE(fs::exists(fs::Path{cont::StringView{taken.data(), taken.size()}})); // the oldest went first
    CHECK(fs::is_file(captures.path / "flight-12.cdb"));
    host.stop();
}

TEST_CASE("flight host: allocation failure at the resource allocator, injected and real, is one capture each",
          "[sandbox][diag][diag9b][flight-host]")
{
    const Scratch      scratch("crd-sandbox-flight-alloc");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);

    FlightHost        host;
    FlightHostOptions options;
    options.policy_asset = policy_path.c_str();
    cont::String reason;
    REQUIRE(host.start(options, reason) == FlightHostStart::Started);
    crd::memory::TlsfAllocator     arena(64U * 1024U);
    crd::memory::IAllocator* const alloc = host.watch_allocator(&arena);
    REQUIRE(alloc != nullptr);
    CHECK(host.watch_allocator(&arena) == nullptr); // one per host

    cp::DiagServiceConfig config;
    host.configure_service(config);
    cp::DiagCommandService service(kGrant, config);
    REQUIRE(host.serve(service));
    const bool           seams = crd::fault_seams_compiled();
    const cp::DiagResult armed =
        execute(service, "fault.arm", nullptr, "plan", "seed=3;allocation-failure@0+1%1000000");
    if (seams)
    {
        REQUIRE(armed.status == cp::DiagStatus::Ok);
        CHECK(alloc->try_allocate(64U) == nullptr); // injected: the watch and the arena below are never asked
        fill_post_window(host, 4U);
        REQUIRE(host.command_host().received() == 1U);
        cp::FlightCapture injected;
        REQUIRE(host.command_host().retained(injected));
        CHECK(injected.trigger.kind == cp::FlightTrigger::InjectedFault);
        CHECK(injected.suppressed_triggers == 0U); // the watch below never saw the injected null
        CHECK(injected.replay_available);
        CHECK(starts_with(cont::StringView{injected.replay.data(), injected.replay.size()},
                          "fault-plan/1 fired=allocation-failure#0 minimal="));
        void* const after = alloc->try_allocate(64U); // hit 1 is outside the rule
        CHECK(after != nullptr);
        alloc->deallocate(after);
        REQUIRE(execute(service, "fault.disarm").status == cp::DiagStatus::Ok);
    }
    else
    {
        CHECK(armed.status == cp::DiagStatus::Unavailable);
        void* const plain = alloc->try_allocate(64U);
        CHECK(plain != nullptr);
        alloc->deallocate(plain);
        CHECK(host.command_host().received() == 0U);
    }

    // A real failure of the arena reaches the watch: an allocation-failure capture with the size asked for.
    const u64 before = host.command_host().received();
    CHECK(alloc->try_allocate(1024U * 1024U) == nullptr);
    fill_post_window(host, 4U);
    REQUIRE(host.command_host().received() == before + 1U);
    cp::FlightCapture real;
    REQUIRE(host.command_host().retained(real));
    CHECK(real.trigger.kind == cp::FlightTrigger::AllocationFailure);
    CHECK(real.trigger.value_a == 1024U * 1024U);
    CHECK(real.suppressed_triggers == 0U);
    host.stop();
}

TEST_CASE("flight host: a read failed through the I/O hook the host installed is an injected-fault capture",
          "[sandbox][diag][diag9b][flight-host]")
{
    const Scratch      scratch("crd-sandbox-flight-read");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);
    const cont::String target = scratch.file("target.bin");
    write_text(target, "payload");

    FlightHost        host;
    FlightHostOptions options;
    options.policy_asset = policy_path.c_str();
    cont::String reason;
    REQUIRE(host.start(options, reason) == FlightHostStart::Started);
    cp::DiagServiceConfig config;
    host.configure_service(config);
    cp::DiagCommandService service(kGrant, config);
    REQUIRE(host.serve(service));

    const bool           seams = crd::fault_seams_compiled();
    const cp::DiagResult armed = execute(service, "fault.arm", nullptr, "plan", "seed=5;failed-read@0+1%1000000");
    cont::Array<u8>      bytes;
    const bool           read = fs::read_file_binary(fs::Path{cont::StringView{target.data(), target.size()}}, bytes);
    if (seams)
    {
        REQUIRE(armed.status == cp::DiagStatus::Ok);
        CHECK_FALSE(read);
        fill_post_window(host, 4U);
        cp::FlightCapture c;
        REQUIRE(host.command_host().retained(c));
        CHECK(c.trigger.kind == cp::FlightTrigger::InjectedFault);
        bool named = false;
        for (const cp::FlightEvent& e : c.events)
        {
            named = named || (e.code == cp::kFlightCodeFaultInjected && ends_with(e.message_view(), "on target.bin"));
        }
        CHECK(named);
        REQUIRE(execute(service, "fault.disarm").status == cp::DiagStatus::Ok);
    }
    else
    {
        CHECK(armed.status == cp::DiagStatus::Unavailable);
        CHECK(read);
        CHECK(host.command_host().received() == 0U);
    }
    host.stop();
}

namespace
{
void starved_job(void* /*data*/) noexcept {}
} // namespace

TEST_CASE("flight host: the watchdog window the host gives the pool reports a real hang into the recorder",
          "[sandbox][diag][diag9b][flight-host]")
{
    const Scratch      scratch("crd-sandbox-flight-hang");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);
    FlightHost        host;
    FlightHostOptions options;
    options.policy_asset = policy_path.c_str();
    cont::String reason;
    REQUIRE(host.start(options, reason) == FlightHostStart::Started);

    crd::jobs::Config kept;
    kept.hang_watchdog_period_ms = 40U;
    host.configure_jobs(kept);
    CHECK(kept.hang_watchdog_period_ms == 40U); // a pool with a window keeps it

    crd::jobs::Config cfg;
    cfg.num_threads = 1U; // the main thread is the only executor and does not pump: the queued job is starved
    host.configure_jobs(cfg);
    REQUIRE(cfg.hang_watchdog_period_ms == FlightHost::kWatchdogPeriodMs);
    crd::jobs::init(cfg);
    crd::jobs::JobDecl job{};
    job.fn                           = &starved_job;
    crd::jobs::Counter* const handle = crd::jobs::run(job);
    bool                      got    = false;
    for (u32 tick = 0U; tick < 3000U && !got; ++tick) // up to ~30 s; the report comes after a few 250 ms windows
    {
        host.recorder().record(0x0042U, cp::Severity::Info, cont::StringView{"waiting"}, tick, 0U);
        got = host.command_host().received() > 0U || host.recorder().poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)crd::jobs::pump_main_thread_until_idle();
    crd::jobs::wait(handle);
    crd::jobs::shutdown();

    REQUIRE(got);
    cp::FlightCapture c;
    REQUIRE(host.command_host().retained(c));
    CHECK(c.trigger.kind == cp::FlightTrigger::Hang);
    CHECK(c.trigger.value_a == static_cast<u64>(crd::jobs::HangKind::ExecutorStarved));
    CHECK(c.trigger.value_b >= 1U);
    CHECK(c.pre_retained > 0U);
    host.stop();
}

TEST_CASE("flight host: flight.status keeps the replay recipe when the trigger message and recipe are at their bounds",
          "[sandbox][diag][diag9b][flight-host]")
{
    const Scratch      scratch("crd-sandbox-flight-status");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);
    FlightHost        host;
    FlightHostOptions options;
    options.policy_asset = policy_path.c_str();
    cont::String reason;
    REQUIRE(host.start(options, reason) == FlightHostStart::Started);
    crd::memory::TlsfAllocator     arena(64U * 1024U);
    crd::memory::IAllocator* const alloc = host.watch_allocator(&arena);
    cp::DiagServiceConfig          config;
    host.configure_service(config);
    cp::DiagCommandService service(kGrant, config);
    REQUIRE(host.serve(service));

    // Eight rules: the recipe (also the injected fault's trigger message) is clipped at the recorder's bound.
    const char* const plan = "seed=18446744073709551615;allocation-failure@0+1%1000000;failed-read@100000+1%1000000;"
                             "partial-read@100000+1%1000000=16;delayed-completion@100000+1%1000000=5;"
                             "cancelled-completion@100000+1%1000000;device-loss@100000+1%1000000;"
                             "failed-read@200000+1%1000000;partial-read@200000+1%1000000=8";
    const cp::DiagResult armed = execute(service, "fault.arm", nullptr, "plan", plan);
    if (!crd::fault_seams_compiled())
    {
        CHECK(armed.status == cp::DiagStatus::Unavailable);
        host.stop();
        return;
    }
    REQUIRE(armed.status == cp::DiagStatus::Ok);
    CHECK(alloc->try_allocate(64U) == nullptr);
    fill_post_window(host, 4U);
    cp::FlightCapture c;
    REQUIRE(host.command_host().retained(c));
    CHECK(c.trigger.kind == cp::FlightTrigger::InjectedFault);
    CHECK(c.trigger.message_view().size() == cp::kFlightTriggerMessageBytes - 1U);
    CHECK(c.replay.size() == cp::kFlightRecipeBytes - 1U);

    const cp::DiagResult status = execute(service, "flight.status");
    REQUIRE(status.status == cp::DiagStatus::Ok);
    CHECK(contains(status.json, "\"replay_available\":true"));
    CHECK(contains(status.json, "\"replay\":\"fault-plan/1 fired=allocation-failure#0 minimal="));
    // Every field is there, the replay last; the two long strings are cut at the 160-byte field bound (which is what
    // marks the summary clipped), and the one-hit plan that reproduces the fault alone survives the cut whole.
    CHECK(contains(status.json, "\"suppressed\":0,\"replay_available\":true,\"replay\":\"fault-plan/1 "
                                "fired=allocation-failure#0 minimal=seed=18446744073709551615;"
                                "allocation-failure@0+1%1000000 plan="));
    REQUIRE(execute(service, "fault.disarm").status == cp::DiagStatus::Ok);
    host.stop();
}

namespace
{
int g_probe_ios = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

crd::IoFault probe_io_hook(void* /*user*/, crd::IoFaultOp /*op*/, const char* /*path*/, crd::usize /*bytes*/,
                           u64 /*offset*/, u64 /*size*/) noexcept
{
    ++g_probe_ios;
    return crd::IoFault{};
}
} // namespace

TEST_CASE("flight host: refused starts install nothing, and stop restores every seam and delivers a pending capture",
          "[sandbox][diag][diag9b][flight-host]")
{
    const Scratch scratch("crd-sandbox-flight-stop");
    const crd::AssertHandler                previous_assert = crd::get_assert_handler();
    const cp::FatalInvariantHandler         previous_fatal  = cp::get_fatal_invariant_handler();
    cont::String                            reason;

    // A policy asset with an unknown key is refused by name; a missing directory is refused; nothing is installed.
    const cont::String bad = scratch.file("bad.flight.toml");
    write_text(bad, "kind = \"flight-policy\"\nschema_version = 1\ncapacity = 64\nprevents = 3\n");
    {
        FlightHost        refused;
        FlightHostOptions options;
        options.policy_asset = bad.c_str();
        CHECK(refused.start(options, reason) == FlightHostStart::PolicyRefused);
        CHECK(contains(reason, "prevents"));
        CHECK_FALSE(refused.recorder().is_initialized());
        CHECK(crd::get_assert_handler() == previous_assert);
        reason.clear();
        const cont::String missing = scratch.file("no-such-dir");
        FlightHostOptions  no_dir;
        no_dir.capture_dir = missing.c_str();
        CHECK(refused.start(no_dir, reason) == FlightHostStart::DirectoryMissing);
        CHECK_FALSE(refused.recorder().is_initialized());
        CHECK(crd::get_assert_handler() == previous_assert);
    }

    const cont::String dir = scratch.text();
    {
        FlightHost        host;
        FlightHostOptions options;
        options.capture_dir = dir.c_str();
        reason.clear();
        REQUIRE(host.start(options, reason) == FlightHostStart::Started);
        CHECK(host.start(options, reason) == FlightHostStart::AlreadyStarted);
        CHECK(crd::get_assert_handler() != previous_assert);
        CHECK(cp::get_fatal_invariant_handler() != previous_fatal);
        // The default policy, held equal to the committed default asset by crd-perf-tests.
        const cp::FlightPolicy p = host.recorder().policy();
        CHECK(p.capacity == cp::FlightPolicy{}.capacity);
        CHECK(p.hitch_threshold_ns == cp::FlightPolicy{}.hitch_threshold_ns);

        // A second host while this one runs: the trigger slots are taken, so it refuses and leaves no recorder.
        {
            FlightHost second;
            reason.clear();
            CHECK(second.start(FlightHostOptions{}, reason) == FlightHostStart::TriggersRefused);
            CHECK_FALSE(second.recorder().is_initialized());
        }
        CHECK(host.recorder().is_initialized());

        cp::DiagServiceConfig config;
        host.configure_service(config);
        cp::DiagCommandService service(kGrant, config);
        REQUIRE(host.serve(service));
        const bool seams = crd::fault_seams_compiled();
        CHECK_FALSE(crd::install_io_fault_hook(&probe_io_hook, nullptr)); // the host's hook holds the seam

        // A capture still pending at stop is delivered, marked truncated by the shutdown, and written.
        REQUIRE(execute(service, "flight.trigger", nullptr, "message", "pending at stop").status == cp::DiagStatus::Ok);
        CHECK(host.command_host().received() == 0U);
        host.stop();
        CHECK(host.command_host().received() == 1U);
        cp::FlightCapture c;
        REQUIRE(host.command_host().retained(c));
        CHECK(c.trigger.message_view() == "pending at stop");
        CHECK(c.end == cp::FlightEnd::Shutdown);
        CHECK(c.post_truncated);
        CHECK(host.bundles_written() == 1U);

        // Every seam is back as it was.
        CHECK_FALSE(host.started());
        CHECK_FALSE(host.hooks_installed());
        CHECK(crd::get_assert_handler() == previous_assert);
        CHECK(cp::get_fatal_invariant_handler() == previous_fatal);
        CHECK(crd::install_io_fault_hook(&probe_io_hook, nullptr) == seams);
        if (seams)
        {
            g_probe_ios = 0;
            cont::Array<u8> bytes;
            (void)fs::read_file_binary(fs::Path{cont::StringView{host.last_bundle().c_str()}}, bytes);
            CHECK(g_probe_ios > 0);
            crd::uninstall_io_fault_hook();
        }
        host.stop(); // twice is safe
        CHECK(host.command_host().received() == 1U);
    }

    // The slots are free again: a new host starts.
    FlightHost again;
    reason.clear();
    CHECK(again.start(FlightHostOptions{}, reason) == FlightHostStart::Started);
    again.stop();
}
