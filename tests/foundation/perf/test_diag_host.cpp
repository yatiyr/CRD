// DIAG.9b/11a: the engine's reusable diagnostic host (crd/perf/diagnostic_host.hpp), driven the way an application,
// a headless tool and the sandbox drive it, without a window or a device. The DIAG.9b cases began as the sandbox's own
// flight host; DIAG.11a moved that host into crd-perf and started it from an authored profile.
//
// Covered (DIAG.9b, now through the profile): the host starts the recorder from an authored policy asset and takes its
// module snapshot; served on a real DiagCommandService it adds flight.* (and fault.* while fault injection runs) and
// the doctor reports the live recorder and injector; frames fed the way a frame loop feeds them make a frame hitch a
// capture, which the host delivers, logs nothing about and writes to its capture directory as a bundle naming this
// build and this executable, never over an existing file and keeping the newest ten; an injected allocation failure at
// a wrapped allocator is one injected-fault capture and a real one an allocation-failure capture; a read failed
// through the I/O hook the host installed; the jobs watchdog window the host gives the pool reports a real hang into
// the recorder; flight.status keeps the replay recipe of a capture whose trigger message and recipe are both at their
// bounds; refused starts leave nothing installed, and stop() restores every seam and delivers a pending capture.
//
// Covered (DIAG.11a): the committed default profile equals DiagnosticHostProfile{} and every key round-trips; every
// refusal names its key and leaves the profile untouched; a change names its fields and only the capture directory,
// the bundle bound and the flight policy apply live; the modes follow the profile and the build, and a required mode
// the build lacks refuses with nothing installed; a start after the jobs pool runs is refused; apply() changes the live
// fields on the running host (an edited policy file included) and refuses the rest, a pending capture and a missing
// directory without changing anything; the crash handler is installed into the profile's directory; and an
// application that fails on purpose in a child process leaves an attributable bundle from its authored profile.
//
// A shipping build compiles the fault seams out: there fault injection is Unavailable, fault.* is not served and the
// cases check that instead. ASCII test names.

#include "module_file_identity.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/diag/specimen_runner.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/perf/build_identity.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/bundle_import.hpp>
#include <crd/perf/diag_assets.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/diagnostic_host.hpp>
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
using cp::DiagHostApply;
using cp::DiagHostStart;
using cp::DiagMode;
using cp::DiagModeState;
using cp::DiagnosticHost;
using cp::DiagnosticHostProfile;

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

bool same_text(const cont::String& a, const cont::String& b)
{
    return cont::StringView{a.data(), a.size()} == cont::StringView{b.data(), b.size()};
}

bool same_profile(const DiagnosticHostProfile& a, const DiagnosticHostProfile& b)
{
    return a.crash_handler == b.crash_handler && same_text(a.crash_dir, b.crash_dir) && a.flight == b.flight &&
           same_text(a.flight_policy, b.flight_policy) && same_text(a.capture_dir, b.capture_dir) &&
           a.max_bundles == b.max_bundles && a.fault_injection == b.fault_injection &&
           a.command_grant == b.command_grant && a.allow_remote == b.allow_remote &&
           a.watchdog_period_ms == b.watchdog_period_ms;
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
        const cont::String     full{p.generic()};
        const char* const      slash = std::strrchr(full.c_str(), '/');
        const cont::StringView name{slash != nullptr ? slash + 1 : full.c_str()};
        if (starts_with(name, "flight-") && ends_with(name, ".cdb"))
        {
            ++n;
        }
    }
    return n;
}

// The profile the in-process cases start from: the crash handler off (it has its own case), the small policy, fault
// injection on and a grant that may record and inject.
DiagnosticHostProfile host_profile(const cont::String& policy_path, const cont::String* capture_dir = nullptr)
{
    DiagnosticHostProfile p;
    p.crash_handler   = DiagMode::Off;
    p.flight_policy   = policy_path;
    p.fault_injection = DiagMode::On;
    p.command_grant   = kGrant;
    if (capture_dir != nullptr)
    {
        p.capture_dir = *capture_dir;
    }
    return p;
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
    cp::BundleImport     import;
    cp::FlightRecordRead flight;
    bool                 has_flight = false;
};

BundleRead read_flight_bundle(const cont::String& path)
{
    const cont::Array<u8> bytes = read_bytes(path);
    BundleRead            out;
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

const cp::ModuleIdentity* find_module(const cont::Array<cp::ModuleIdentity>& modules, const char* name)
{
    for (const cp::ModuleIdentity& m : modules)
    {
        if (contains(m.name, name))
        {
            return &m;
        }
    }
    return nullptr;
}

// The file name of a path, either separator.
const char* base_name(const char* path)
{
    const char* name = path;
    for (const char* p = path; *p != '\0'; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            name = p + 1;
        }
    }
    return name;
}

const cp::ModuleIdentity* find_this_executable(const cont::Array<cp::ModuleIdentity>& modules)
{
    return find_module(modules, "crd-perf-tests");
}

// The frame loop: `count` frames of `step_ns` each, from `t`, fed the way a frame loop feeds them. Returns how many of
// them delivered a capture.
u32 run_frames(DiagnosticHost& host, u64& t, u32 count, u64 step_ns)
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
void fill_post_window(DiagnosticHost& host, u32 count)
{
    for (u32 i = 0U; i < count; ++i)
    {
        host.recorder().record(0x0042U, cp::Severity::Info, cont::StringView{"after"}, i, 0U);
    }
    (void)host.poll();
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

TEST_CASE("diagnostic host: a frame hitch is a capture the host writes as a bundle naming this build and executable",
          "[perf][diag][diag9b][diag11a][diag-host]")
{
    const Scratch      scratch("crd-diag-host-hitch");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);
    const Scratch      captures("crd-diag-host-hitch-out");
    const cont::String capture_dir = captures.text();

    DiagnosticHost host;
    cont::String   reason;
    REQUIRE(host.start(host_profile(policy_path, &capture_dir), reason) == DiagHostStart::Started);
    const cp::FlightPolicy policy = host.recorder().policy();
    CHECK(policy.capacity == 64U);
    CHECK(policy.pre_events == 16U);
    CHECK(policy.post_events == 4U);
    CHECK(policy.hitch_threshold_ns == 20U * kMs);

    // The snapshot taken at start names this executable.
    const cont::Array<cp::ModuleIdentity> snapshot = host.command_host().modules();
    REQUIRE(find_this_executable(snapshot) != nullptr);

    // Served on a real service: the commands are listed and the doctor reports the live recorder and injector.
    const bool            seams = crd::fault_seams_compiled();
    cp::DiagServiceConfig config;
    config.root = cont::StringView{capture_dir.data(), capture_dir.size()};
    host.configure_service(config);
    CHECK(config.flight_recorder == &host.recorder());
    CHECK(config.fault_injector == (seams ? &host.injector() : nullptr));
    cp::DiagCommandService service(kGrant, config);
    REQUIRE(host.serve(service));
    CHECK(host.hooks_installed() == seams);
    CHECK(host.modes().faults == (seams ? DiagModeState::Active : DiagModeState::Unavailable));
    const cp::DiagResult commands = execute(service, "diag.commands");
    REQUIRE(commands.status == cp::DiagStatus::Ok);
    for (const char* name : {"flight.status", "flight.trigger", "flight.policy", "flight.write"})
    {
        cont::String quoted(R"("name":")");
        quoted.append(name);
        quoted.push_back('"');
        CHECK(contains(commands.json, quoted.c_str()));
    }
    for (const char* name : {"fault.arm", "fault.disarm", "fault.status"})
    {
        cont::String quoted(R"("name":")");
        quoted.append(name);
        quoted.push_back('"');
        CHECK(contains(commands.json, quoted.c_str()) == seams); // served only while fault injection runs
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
    CHECK(host.frames() == 26U);

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

TEST_CASE("diagnostic host: the writer never overwrites and keeps the newest ten bundles",
          "[perf][diag][diag9b][diag11a][diag-host]")
{
    const Scratch      scratch("crd-diag-host-retention");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);
    const Scratch      captures("crd-diag-host-retention-out");
    const cont::String capture_dir = captures.text();
    const cont::String taken       = captures.file("flight-0.cdb");
    write_text(taken, "not a bundle");

    DiagnosticHost host;
    cont::String   reason;
    REQUIRE(host.start(host_profile(policy_path, &capture_dir), reason) == DiagHostStart::Started);

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

TEST_CASE("diagnostic host: allocation failure at a wrapped allocator, injected and real, is one capture each",
          "[perf][diag][diag9b][diag11a][diag-host]")
{
    const Scratch      scratch("crd-diag-host-alloc");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);

    DiagnosticHost host;
    cont::String   reason;
    REQUIRE(host.start(host_profile(policy_path), reason) == DiagHostStart::Started);
    crd::memory::TlsfAllocator     arena(64U * 1024U);
    crd::memory::IAllocator* const alloc = host.watch_allocator(&arena, "test arena");
    REQUIRE(alloc != nullptr);
    CHECK(host.watch_allocator(&arena, "again") == nullptr); // one per host

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
        CHECK(armed.status == cp::DiagStatus::UnknownCommand); // fault injection is unavailable, so not served
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

TEST_CASE("diagnostic host: a read failed through the I/O hook the host installed is an injected-fault capture",
          "[perf][diag][diag9b][diag11a][diag-host]")
{
    const Scratch      scratch("crd-diag-host-read");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);
    const cont::String target = scratch.file("target.bin");
    write_text(target, "payload");

    DiagnosticHost host;
    cont::String   reason;
    REQUIRE(host.start(host_profile(policy_path), reason) == DiagHostStart::Started);
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
        CHECK(armed.status == cp::DiagStatus::UnknownCommand);
        CHECK(read);
        CHECK(host.command_host().received() == 0U);
    }
    host.stop();
}

namespace
{
void starved_job(void* /*data*/) noexcept {}
} // namespace

TEST_CASE("diagnostic host: the watchdog window the host gives the pool reports a real hang into the recorder",
          "[perf][diag][diag9b][diag11a][diag-host]")
{
    const Scratch      scratch("crd-diag-host-hang");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);
    DiagnosticHost host;
    cont::String   reason;
    REQUIRE(host.start(host_profile(policy_path), reason) == DiagHostStart::Started);

    crd::jobs::Config kept;
    kept.hang_watchdog_period_ms = 40U;
    host.configure_jobs(kept);
    CHECK(kept.hang_watchdog_period_ms == 40U); // a pool with a window keeps it

    crd::jobs::Config cfg;
    cfg.num_threads = 1U; // the main thread is the only executor and does not pump: the queued job is starved
    host.configure_jobs(cfg);
    REQUIRE(cfg.hang_watchdog_period_ms == cp::kDiagHostDefaultWatchMs);
    crd::jobs::init(cfg);
    crd::jobs::JobDecl job{};
    job.fn                           = &starved_job;
    crd::jobs::Counter* const handle = crd::jobs::run(job);
    bool                      got    = false;
    for (u32 tick = 0U; tick < 3000U && !got; ++tick) // up to ~30 s; the report comes after a few 250 ms windows
    {
        host.recorder().record(0x0042U, cp::Severity::Info, cont::StringView{"waiting"}, tick, 0U);
        got = host.command_host().received() > 0U || host.poll();
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

    // A profile with no window of its own leaves the pool's setting alone.
    DiagnosticHostProfile none = host_profile(policy_path);
    none.watchdog_period_ms    = 0U;
    REQUIRE(host.start(none, reason) == DiagHostStart::Started);
    crd::jobs::Config untouched;
    host.configure_jobs(untouched);
    CHECK(untouched.hang_watchdog_period_ms == 0U);
    host.stop();
}

TEST_CASE("diagnostic host: flight.status keeps the replay recipe when the trigger message and recipe are at bounds",
          "[perf][diag][diag9b][diag11a][diag-host]")
{
    const Scratch      scratch("crd-diag-host-status");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);
    DiagnosticHost host;
    cont::String   reason;
    REQUIRE(host.start(host_profile(policy_path), reason) == DiagHostStart::Started);
    crd::memory::TlsfAllocator     arena(64U * 1024U);
    crd::memory::IAllocator* const alloc = host.watch_allocator(&arena, "test arena");
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
        // Without the seams injection is unavailable: fault.* is not served and the allocator only watches.
        CHECK(armed.status == cp::DiagStatus::UnknownCommand);
        CHECK(host.modes().faults == DiagModeState::Unavailable);
        void* const plain = alloc->try_allocate(64U);
        CHECK(plain != nullptr);
        alloc->deallocate(plain);
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

TEST_CASE("diagnostic host: refused starts install nothing, and stop restores every seam and delivers a pending "
          "capture",
          "[perf][diag][diag9b][diag11a][diag-host]")
{
    const Scratch                   scratch("crd-diag-host-stop");
    const crd::AssertHandler        previous_assert = crd::get_assert_handler();
    const cp::FatalInvariantHandler previous_fatal  = cp::get_fatal_invariant_handler();
    cont::String                    reason;

    // A policy asset with an unknown key is refused by name; a missing directory is refused; nothing is installed.
    const cont::String bad = scratch.file("bad.flight.toml");
    write_text(bad, "kind = \"flight-policy\"\nschema_version = 1\ncapacity = 64\nprevents = 3\n");
    {
        DiagnosticHost refused;
        CHECK(refused.start(host_profile(bad), reason) == DiagHostStart::PolicyRefused);
        CHECK(contains(reason, "prevents"));
        CHECK_FALSE(refused.recorder().is_initialized());
        CHECK_FALSE(refused.started());
        CHECK(crd::get_assert_handler() == previous_assert);
        reason.clear();
        const cont::String    missing = scratch.file("no-such-dir");
        DiagnosticHostProfile no_dir  = host_profile(cont::String{});
        no_dir.capture_dir            = missing;
        CHECK(refused.start(no_dir, reason) == DiagHostStart::DirectoryMissing);
        CHECK_FALSE(refused.recorder().is_initialized());
        CHECK(crd::get_assert_handler() == previous_assert);
    }

    const cont::String dir = scratch.text();
    {
        DiagnosticHost host;
        reason.clear();
        const DiagnosticHostProfile profile = host_profile(cont::String{}, &dir);
        REQUIRE(host.start(profile, reason) == DiagHostStart::Started);
        CHECK(host.start(profile, reason) == DiagHostStart::AlreadyStarted);
        CHECK(crd::get_assert_handler() != previous_assert);
        CHECK(cp::get_fatal_invariant_handler() != previous_fatal);
        // The default policy, held equal to the committed default asset by crd-perf-tests.
        const cp::FlightPolicy p = host.recorder().policy();
        CHECK(p.capacity == cp::FlightPolicy{}.capacity);
        CHECK(p.hitch_threshold_ns == cp::FlightPolicy{}.hitch_threshold_ns);

        // A second host while this one runs: the trigger slots are taken, so it refuses and leaves nothing behind.
        const bool seams = crd::fault_seams_compiled();
        {
            DiagnosticHost second;
            reason.clear();
            CHECK(second.start(host_profile(cont::String{}), reason) == DiagHostStart::TriggersRefused);
            CHECK_FALSE(second.recorder().is_initialized());
            CHECK_FALSE(second.hooks_installed());
        }
        CHECK(host.recorder().is_initialized());
        CHECK(host.hooks_installed() == seams); // the second host's refusal left this host's hooks in place

        cp::DiagServiceConfig config;
        host.configure_service(config);
        cp::DiagCommandService service(kGrant, config);
        REQUIRE(host.serve(service));
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
    DiagnosticHost again;
    reason.clear();
    CHECK(again.start(host_profile(cont::String{}), reason) == DiagHostStart::Started);
    again.stop();
}

// ---- DIAG.11a: the authored profile, the modes, the change rules ---------------------------------------------------

TEST_CASE("diagnostic host profile: the committed default equals the in-code default, and every key round-trips",
          "[perf][diag][diag11a][diag-host]")
{
    cont::String path{cont::StringView{CRD_DIAG_ASSETS_DIR}};
    path.append("/host_default.host.toml");
    DiagnosticHostProfile committed;
    committed.max_bundles = 7U; // overwritten by a successful load
    cont::String reason;
    REQUIRE(cp::load_diagnostic_host_profile(path.c_str(), committed, reason));
    CHECK(same_profile(committed, DiagnosticHostProfile{}));
    CHECK(committed.crash_handler == DiagMode::On);
    CHECK(committed.flight == DiagMode::On);
    CHECK(committed.fault_injection == DiagMode::Off);
    CHECK(committed.command_grant == cp::authority_bit(cp::DiagAuthority::Read));
    CHECK_FALSE(committed.allow_remote);

    // The investigation profile turns injection on and may record and inject, still without remote access.
    cont::String investigate_path{cont::StringView{CRD_DIAG_ASSETS_DIR}};
    investigate_path.append("/host_investigate.host.toml");
    DiagnosticHostProfile investigate;
    REQUIRE(cp::load_diagnostic_host_profile(investigate_path.c_str(), investigate, reason));
    CHECK(investigate.flight == DiagMode::Required);
    CHECK(investigate.fault_injection == DiagMode::On);
    CHECK(investigate.command_grant == kGrant);
    CHECK_FALSE(investigate.allow_remote);

    // Every key differs from its default, a path with a quote, a backslash and a tab included: the canonical document
    // reads back to the same profile.
    DiagnosticHostProfile custom;
    custom.crash_handler      = DiagMode::Required;
    custom.crash_dir          = cont::String{"dumps/a \"quoted\"\\dir\tname"};
    custom.flight             = DiagMode::Required;
    custom.flight_policy      = cont::String{"policies/small.flight.toml"};
    custom.capture_dir        = cont::String{"C:/captures"};
    custom.max_bundles        = 1000U;
    custom.fault_injection    = DiagMode::Required;
    custom.command_grant      = cp::kDiagAllAuthorities;
    custom.allow_remote       = true;
    custom.watchdog_period_ms = 60000U;
    cont::String text;
    cp::diagnostic_host_profile_to_asset(custom, text);
    DiagnosticHostProfile back;
    REQUIRE(cp::parse_diagnostic_host_profile(cont::StringView{text.data(), text.size()}, back, reason));
    CHECK(same_profile(back, custom));
    cp::diagnostic_host_profile_to_asset(DiagnosticHostProfile{}, text);
    REQUIRE(cp::parse_diagnostic_host_profile(cont::StringView{text.data(), text.size()}, back, reason));
    CHECK(same_profile(back, DiagnosticHostProfile{}));

    // Only the header is required; an empty grant is the empty set.
    REQUIRE(cp::parse_diagnostic_host_profile("kind = \"diagnostic-host\"\nschema_version = 1\ncommand_grant = []\n",
                                              back, reason));
    CHECK(back.command_grant == 0U);
    CHECK(back.crash_handler == DiagMode::On);
}

TEST_CASE("diagnostic host profile: every refusal names its key and leaves the profile untouched",
          "[perf][diag][diag11a][diag-host]")
{
    struct Refusal
    {
        const char* body;   // the document after the header
        const char* reason; // a fragment the reason must hold
    };
    const Refusal refusals[] = {
        {"prevents = 3\n", R"(unknown key "prevents" in a diagnostic-host document)"},
        {"crash_handler = \"maybe\"\n", R"(crash_handler: expected "off", "on" or "required")"},
        {"flight = true\n", R"(flight: expected "off", "on" or "required")"},
        {"fault_injection = 1\n", "fault_injection: expected"},
        {"crash_dir = 5\n", "crash_dir: expected a path string"},
        {"crash_dir = \"\"\n", "crash_dir: must name a directory while the crash handler is asked for"},
        {"crash_dir = \"a\\u0000b\"\n", "crash_dir: must not contain a NUL character"},
        {"flight = \"off\"\nflight_policy = \"x.flight.toml\"\n", "flight_policy: needs flight on or required"},
        {"flight = \"off\"\ncapture_dir = \"captures\"\n", "capture_dir: flight bundles need flight on or required"},
        {"flight = \"off\"\nfault_injection = \"on\"\n", "fault_injection: needs flight on or required"},
        {"max_bundles = 0\n", "max_bundles: must be 1 to 1000"},
        {"max_bundles = 1001\n", "max_bundles: must be 1 to 1000"},
        {"max_bundles = -1\n", "max_bundles: must not be negative"},
        {"watchdog_period_ms = 60001\n", "watchdog_period_ms: is larger than 60000"},
        {"watchdog_period_ms = 1.5\n", "watchdog_period_ms: expected an integer"},
        {"command_grant = \"read\"\n", "command_grant: expected an array of authority names"},
        {"command_grant = [\"read\", \"root\"]\n", "command_grant: unknown authority \"root\""},
        {"command_grant = [1]\n", "command_grant: every element must be an authority name"},
        {"command_grant = [\"remote-enable\"]\n", "remote-enable, upload and process-memory need allow_remote = true"},
        {"command_grant = [\"upload\"]\n", "need allow_remote = true"},
        {"command_grant = [\"process-memory\"]\n", "need allow_remote = true"},
        {"command_grant = [\"inject\"]\n", "command_grant: inject needs fault_injection on or required"},
        {"allow_remote = \"yes\"\n", "allow_remote: expected true or false"},
    };
    for (const Refusal& r : refusals)
    {
        cont::String text("kind = \"diagnostic-host\"\nschema_version = 1\n");
        text.append(r.body);
        DiagnosticHostProfile out;
        out.max_bundles = 7U;
        cont::String reason;
        INFO(r.body);
        CHECK_FALSE(cp::parse_diagnostic_host_profile(cont::StringView{text.data(), text.size()}, out, reason));
        INFO(reason.c_str());
        CHECK(contains(reason, r.reason));
        CHECK(out.max_bundles == 7U); // untouched
    }

    // The header: another kind, another version, no kind, and a document that is not TOML.
    cont::String          reason;
    DiagnosticHostProfile out;
    CHECK_FALSE(cp::parse_diagnostic_host_profile("kind = \"flight-policy\"\nschema_version = 1\n", out, reason));
    CHECK(contains(reason, "kind: expected \"diagnostic-host\""));
    reason.clear();
    CHECK_FALSE(cp::parse_diagnostic_host_profile("kind = \"diagnostic-host\"\nschema_version = 2\n", out, reason));
    CHECK(contains(reason, "schema_version: this build reads version 1"));
    reason.clear();
    CHECK_FALSE(cp::parse_diagnostic_host_profile("schema_version = 1\n", out, reason));
    CHECK(contains(reason, "the document has no kind"));
    reason.clear();
    CHECK_FALSE(cp::parse_diagnostic_host_profile("kind = = 3\n", out, reason));
    CHECK(contains(reason, "the document is not valid TOML"));
    reason.clear();
    CHECK_FALSE(cp::load_diagnostic_host_profile("no/such/profile.host.toml", out, reason));
    CHECK(contains(reason, "could not be read: missing"));

    // A profile built in code is held to the same rules, a path longer than 1024 bytes and an unknown bit included.
    DiagnosticHostProfile long_path;
    long_path.crash_dir = cont::String(1025U, 'd');
    reason.clear();
    CHECK_FALSE(cp::validate_diagnostic_host_profile(long_path, reason));
    CHECK(contains(reason, "crash_dir: is longer than 1024 bytes"));
    DiagnosticHostProfile unknown_bit;
    unknown_bit.command_grant = 1U << 7U;
    reason.clear();
    CHECK_FALSE(cp::validate_diagnostic_host_profile(unknown_bit, reason));
    CHECK(contains(reason, "command_grant: names an authority this build does not know"));
    reason.clear();
    CHECK(cp::validate_diagnostic_host_profile(DiagnosticHostProfile{}, reason));
}

TEST_CASE("diagnostic host: a change names its fields, and only the capture directory, the bundle bound and the "
          "policy apply live",
          "[perf][diag][diag11a][diag-host]")
{
    const DiagnosticHostProfile base;
    const cp::DiagHostChange    same = cp::classify_diagnostic_host_change(base, base);
    CHECK(same.kind == cp::DiagHostChangeKind::Unchanged);
    CHECK(same.live_fields == 0U);
    CHECK(same.restart_fields == 0U);

    struct Case
    {
        cp::DiagHostField field;
        bool              live;
        void (*edit)(DiagnosticHostProfile&);
    };
    const Case cases[] = {
        {cp::DiagHostField::CrashHandler, false, [](DiagnosticHostProfile& p) { p.crash_handler = DiagMode::Off; }},
        {cp::DiagHostField::CrashDir, false, [](DiagnosticHostProfile& p) { p.crash_dir = cont::String{"dumps"}; }},
        {cp::DiagHostField::Flight, false, [](DiagnosticHostProfile& p) { p.flight = DiagMode::Required; }},
        {cp::DiagHostField::FlightPolicy, true,
         [](DiagnosticHostProfile& p) { p.flight_policy = cont::String{"p.flight.toml"}; }},
        {cp::DiagHostField::CaptureDir, true, [](DiagnosticHostProfile& p) { p.capture_dir = cont::String{"c"}; }},
        {cp::DiagHostField::MaxBundles, true, [](DiagnosticHostProfile& p) { p.max_bundles = 3U; }},
        {cp::DiagHostField::FaultInjection, false,
         [](DiagnosticHostProfile& p) { p.fault_injection = DiagMode::On; }},
        {cp::DiagHostField::CommandGrant, false, [](DiagnosticHostProfile& p) { p.command_grant = 0U; }},
        {cp::DiagHostField::AllowRemote, false, [](DiagnosticHostProfile& p) { p.allow_remote = true; }},
        {cp::DiagHostField::WatchdogPeriodMs, false, [](DiagnosticHostProfile& p) { p.watchdog_period_ms = 0U; }},
    };
    u32 seen = 0U;
    for (const Case& c : cases)
    {
        DiagnosticHostProfile candidate = base;
        c.edit(candidate);
        const cp::DiagHostChange change = cp::classify_diagnostic_host_change(base, candidate);
        const auto               bit    = static_cast<u32>(c.field);
        INFO(cp::diag_host_field_name(c.field).data());
        CHECK(change.kind == (c.live ? cp::DiagHostChangeKind::Live : cp::DiagHostChangeKind::RestartRequired));
        CHECK(change.live_fields == (c.live ? bit : 0U));
        CHECK(change.restart_fields == (c.live ? 0U : bit));
        CHECK(((cp::kDiagHostLiveFields & bit) != 0U) == c.live);
        seen |= bit;
    }
    CHECK(seen == (1U << cp::kDiagHostFieldCount) - 1U); // every field has a case

    // A live and a restart field together: the restart wins and both are named.
    DiagnosticHostProfile both = base;
    both.capture_dir           = cont::String{"c"};
    both.command_grant         = 0U;
    const cp::DiagHostChange mixed = cp::classify_diagnostic_host_change(base, both);
    CHECK(mixed.kind == cp::DiagHostChangeKind::RestartRequired);
    cont::String names;
    cp::append_diag_host_fields(names, mixed.live_fields | mixed.restart_fields);
    CHECK(names == "capture_dir,command_grant");
    names.clear();
    cp::append_diag_host_fields(names, 0U);
    CHECK(names == "none");
    CHECK(cp::diag_host_change_kind_name(mixed.kind) == "restart-required");
}

TEST_CASE("diagnostic host: modes follow the profile and the build, and a required mode the build lacks refuses",
          "[perf][diag][diag11a][diag-host]")
{
    const crd::AssertHandler previous_assert = crd::get_assert_handler();
    cont::String             reason;

    // Everything off: nothing runs, nothing is served, no frame is counted.
    {
        DiagnosticHostProfile off;
        off.crash_handler = DiagMode::Off;
        off.flight        = DiagMode::Off;
        DiagnosticHost host;
        REQUIRE(host.start(off, reason) == DiagHostStart::Started);
        CHECK(host.modes().crash == DiagModeState::Off);
        CHECK(host.modes().flight == DiagModeState::Off);
        CHECK(host.modes().faults == DiagModeState::Off);
        CHECK_FALSE(host.recorder().is_initialized());
        CHECK(crd::get_assert_handler() == previous_assert);
        CHECK_FALSE(host.hooks_installed());
        crd::memory::TlsfAllocator arena(16U * 1024U);
        CHECK(host.watch_allocator(&arena, "arena") == nullptr);
        CHECK_FALSE(host.frame(1U, 2U));
        CHECK(host.frames() == 0U);
        cp::DiagServiceConfig config;
        host.configure_service(config);
        CHECK(config.flight_recorder == nullptr);
        CHECK(config.fault_injector == nullptr);
        cp::DiagCommandService service(cp::authority_bit(cp::DiagAuthority::Read), config);
        REQUIRE(host.serve(service));
        const cp::DiagResult commands = execute(service, "diag.commands");
        CHECK_FALSE(contains(commands.json, "flight."));
        CHECK_FALSE(contains(commands.json, "fault."));
        crd::jobs::Config cfg;
        host.configure_jobs(cfg);
        CHECK(cfg.hang_watchdog_period_ms == 0U); // no recorder, so no window is asked for
        host.stop();
    }

    // A build without the fault seams or a crash backend: "on" is reported Unavailable and the run continues.
    cp::DiagnosticHostCapabilities lacking;
    lacking.crash_backend = false;
    lacking.fault_seams   = false;
    {
        DiagnosticHostProfile on;
        on.fault_injection = DiagMode::On;
        DiagnosticHost host;
        REQUIRE(host.start(on, reason, lacking) == DiagHostStart::Started);
        CHECK(host.modes().crash == DiagModeState::Unavailable);
        CHECK(host.modes().flight == DiagModeState::Active);
        CHECK(host.modes().faults == DiagModeState::Unavailable);
        CHECK_FALSE(host.hooks_installed());
        cp::DiagServiceConfig config;
        host.configure_service(config);
        CHECK(config.fault_injector == nullptr);
        cp::DiagCommandService service(cp::authority_bit(cp::DiagAuthority::Read), config);
        REQUIRE(host.serve(service));
        const cp::DiagResult commands = execute(service, "diag.commands");
        CHECK(contains(commands.json, "\"name\":\"flight.status\""));
        CHECK_FALSE(contains(commands.json, "fault."));
        host.stop();
    }

    // "required" on such a build refuses before anything is installed.
    {
        DiagnosticHostProfile needs_faults;
        needs_faults.crash_handler   = DiagMode::Off;
        needs_faults.fault_injection = DiagMode::Required;
        DiagnosticHost host;
        reason.clear();
        CHECK(host.start(needs_faults, reason, lacking) == DiagHostStart::ModeUnavailable);
        CHECK(contains(reason, "fault_injection is required"));
        CHECK_FALSE(host.started());
        CHECK_FALSE(host.recorder().is_initialized());
        CHECK(crd::get_assert_handler() == previous_assert);

        DiagnosticHostProfile needs_crash;
        needs_crash.crash_handler = DiagMode::Required;
        reason.clear();
        CHECK(host.start(needs_crash, reason, lacking) == DiagHostStart::ModeUnavailable);
        CHECK(contains(reason, "crash_handler is required"));
        CHECK_FALSE(host.recorder().is_initialized());
        CHECK(crd::get_assert_handler() == previous_assert);

        // A profile that breaks a rule is refused the same way, by key.
        DiagnosticHostProfile broken;
        broken.max_bundles = 0U;
        reason.clear();
        CHECK(host.start(broken, reason) == DiagHostStart::ProfileRefused);
        CHECK(contains(reason, "max_bundles"));
    }

    // This build: injection runs exactly when the seams are compiled in.
    {
        DiagnosticHostProfile on;
        on.crash_handler   = DiagMode::Off;
        on.fault_injection = DiagMode::On;
        DiagnosticHost host;
        REQUIRE(host.start(on, reason) == DiagHostStart::Started);
        const bool seams = crd::fault_seams_compiled();
        CHECK(host.modes().faults == (seams ? DiagModeState::Active : DiagModeState::Unavailable));
        CHECK(host.hooks_installed() == seams);
        host.stop();
    }
    CHECK(cp::diag_mode_state_name(DiagModeState::Unavailable) == "unavailable");
    CHECK(cp::diag_host_start_name(DiagHostStart::ModeUnavailable) == "mode-unavailable");
}

TEST_CASE("diagnostic host: a start after the jobs pool runs is refused", "[perf][diag][diag11a][diag-host]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 2U;
    crd::jobs::init(cfg);
    DiagnosticHost        host;
    DiagnosticHostProfile profile;
    profile.crash_handler = DiagMode::Off;
    cont::String reason;
    const DiagHostStart start = host.start(profile, reason);
    crd::jobs::shutdown();
    CHECK(start == DiagHostStart::LateStart);
    CHECK(contains(reason, "before the pool, devices and tools"));
    CHECK_FALSE(host.started());
    CHECK_FALSE(host.recorder().is_initialized());

    // Once the pool is gone the same host starts.
    reason.clear();
    REQUIRE(host.start(profile, reason) == DiagHostStart::Started);
    host.stop();
}

TEST_CASE("diagnostic host: apply changes the live fields on the running host and refuses the rest unchanged",
          "[perf][diag][diag11a][diag-host]")
{
    const Scratch      scratch("crd-diag-host-apply");
    const cont::String policy_path = scratch.file("small.flight.toml");
    write_text(policy_path, kPolicyText);
    const Scratch      first_out("crd-diag-host-apply-a");
    const Scratch      second_out("crd-diag-host-apply-b");
    const cont::String first_dir  = first_out.text();
    const cont::String second_dir = second_out.text();

    DiagnosticHost        host;
    cont::String          reason;
    DiagnosticHostProfile profile = host_profile(policy_path, &first_dir);
    CHECK(host.apply(profile, reason) == DiagHostApply::NotStarted);
    REQUIRE(host.start(profile, reason) == DiagHostStart::Started);
    const u32 generation = host.recorder().policy_generation();
    reason.clear();
    CHECK(host.apply(profile, reason) == DiagHostApply::Unchanged);

    cp::FlightTriggerInput manual;
    manual.kind    = cp::FlightTrigger::Manual;
    manual.code    = cp::kFlightCodeManual;
    manual.message = cont::StringView{"apply"};

    // A restart field: refused by name, nothing changes.
    // The new grant is still valid: injection is on.
    DiagnosticHostProfile restart = profile;
    restart.command_grant = cp::authority_bit(cp::DiagAuthority::Read) | cp::authority_bit(cp::DiagAuthority::Inject);
    restart.watchdog_period_ms = 500U;
    restart.capture_dir        = second_dir; // a live field alongside does not apply either
    reason.clear();
    CHECK(host.apply(restart, reason) == DiagHostApply::RestartRequired);
    CHECK_FALSE(contains(reason, "capture_dir"));
    CHECK(contains(reason, "command_grant,watchdog_period_ms"));
    CHECK(host.profile().command_grant == kGrant);
    CHECK(same_text(host.profile().capture_dir, first_dir));
    REQUIRE(host.recorder().trigger(manual) == cp::FlightTriggerResult::Started);
    fill_post_window(host, 4U);
    CHECK(host.last_bundle() == first_out.file("flight-0.cdb"));

    // A candidate that breaks a rule, names a missing directory or a refused policy: refused, nothing changes.
    DiagnosticHostProfile broken = profile;
    broken.max_bundles           = 0U;
    reason.clear();
    CHECK(host.apply(broken, reason) == DiagHostApply::Refused);
    DiagnosticHostProfile missing = profile;
    missing.capture_dir           = scratch.file("no-such-dir");
    reason.clear();
    CHECK(host.apply(missing, reason) == DiagHostApply::Refused);
    CHECK(contains(reason, "capture directory does not exist"));
    const cont::String bad_policy = scratch.file("bad.flight.toml");
    write_text(bad_policy, "kind = \"flight-policy\"\nschema_version = 1\ncapacity = 1\n");
    DiagnosticHostProfile refused_policy = profile;
    refused_policy.flight_policy         = bad_policy;
    refused_policy.capture_dir           = second_dir;
    reason.clear();
    CHECK(host.apply(refused_policy, reason) == DiagHostApply::Refused);
    CHECK(host.recorder().policy_generation() == generation);
    CHECK(same_text(host.profile().capture_dir, first_dir));

    // The live fields: a new directory and a smaller bound take effect for the next bundle.
    DiagnosticHostProfile live = profile;
    live.capture_dir           = second_dir;
    live.max_bundles           = 2U;
    reason.clear();
    REQUIRE(host.apply(live, reason) == DiagHostApply::Applied);
    CHECK(host.recorder().policy_generation() == generation); // the policy did not change
    for (u32 i = 0U; i < 3U; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(15)); // distinct write times for the retention order
        REQUIRE(host.recorder().trigger(manual) == cp::FlightTriggerResult::Started);
        fill_post_window(host, 4U);
    }
    CHECK(host.last_bundle() == second_out.file("flight-3.cdb"));
    CHECK(count_bundles(second_out.path) == 2U);
    CHECK(count_bundles(first_out.path) == 1U);

    // The same policy path with edited contents is a live change: the recorder reloads at a new generation.
    write_text(policy_path, "kind = \"flight-policy\"\nschema_version = 1\ncapacity = 128\npre_events = 32\n"
                            "post_events = 8\npost_window_ns = 0\n");
    reason.clear();
    REQUIRE(host.apply(live, reason) == DiagHostApply::Applied);
    CHECK(host.recorder().policy_generation() == generation + 1U);
    CHECK(host.recorder().policy().capacity == 128U);

    // A pending capture holds the policy: Busy, and nothing else changes either.
    REQUIRE(host.recorder().trigger(manual) == cp::FlightTriggerResult::Started);
    DiagnosticHostProfile busy = live;
    busy.flight_policy         = cont::String{};
    busy.max_bundles           = 9U;
    reason.clear();
    CHECK(host.apply(busy, reason) == DiagHostApply::Busy);
    CHECK(host.profile().max_bundles == 2U);
    CHECK(host.recorder().policy().capacity == 128U);
    fill_post_window(host, 8U);
    reason.clear();
    CHECK(host.apply(busy, reason) == DiagHostApply::Applied);
    CHECK(host.recorder().policy().capacity == cp::FlightPolicy{}.capacity);
    CHECK(host.profile().max_bundles == 9U);
    CHECK(cp::diag_host_apply_name(DiagHostApply::Busy) == "busy");
    host.stop();
}

TEST_CASE("diagnostic host: the crash handler is installed into the profile's directory",
          "[perf][diag][diag11a][diag-host]")
{
    const Scratch      scratch("crd-diag-host-crash");
    const cont::String crash_dir = scratch.file("crashes");
    CHECK_FALSE(fs::exists(fs::Path{cont::StringView{crash_dir.data(), crash_dir.size()}}));

    DiagnosticHostProfile profile;
    profile.crash_handler = DiagMode::Required;
    profile.crash_dir     = crash_dir;
    profile.flight        = DiagMode::Off;
    DiagnosticHost host;
    cont::String   reason;
    const bool     backend = cp::diagnostic_host_capabilities().crash_backend;
    const DiagHostStart start = host.start(profile, reason);
    if (!backend)
    {
        CHECK(start == DiagHostStart::ModeUnavailable);
        return;
    }
    INFO(reason.c_str());
    REQUIRE(start == DiagHostStart::Started);
    CHECK(host.modes().crash == DiagModeState::Active);
    CHECK((host.modes().crash_install == crd::crash::InstallResult::Ok ||
           host.modes().crash_install == crd::crash::InstallResult::OkReinstalled));
    CHECK(fs::is_directory(fs::Path{cont::StringView{crash_dir.data(), crash_dir.size()}})); // install creates it
    host.stop();
    CHECK_FALSE(host.started());

    // A directory the handler cannot use: "on" continues without it, "required" refuses.
    const cont::String file_in_the_way = scratch.file("a-file");
    write_text(file_in_the_way, "not a directory");
    profile.crash_dir     = file_in_the_way;
    profile.crash_handler = DiagMode::On;
    reason.clear();
    REQUIRE(host.start(profile, reason) == DiagHostStart::Started);
    CHECK(host.modes().crash == DiagModeState::Unavailable);
    CHECK(host.modes().crash_install == crd::crash::InstallResult::OutputDirUnusable);
    host.stop();
    profile.crash_handler = DiagMode::Required;
    reason.clear();
    CHECK(host.start(profile, reason) == DiagHostStart::ModeUnavailable);
    CHECK(contains(reason, "output-dir-unusable"));
    CHECK_FALSE(host.started());
}

TEST_CASE("diagnostic host: an application that fails on purpose leaves an attributable bundle from its profile",
          "[perf][diag][diag11a][diag-host]")
{
    namespace cd = crd::diag;
    for (const char* mode : {"invariant", "assert"})
    {
        char name[64];
        (void)std::snprintf(name, sizeof(name), "crd-diag-host-app-%s", mode);
        const Scratch      scratch(name);
        const cont::String captures = scratch.file("captures");
        REQUIRE(fs::create_directories(fs::Path{cont::StringView{captures.data(), captures.size()}}));
        const cont::String crashes = scratch.file("crashes");

        // The application's profile, authored as a file: the crash handler on, the default policy, captures written.
        DiagnosticHostProfile profile;
        profile.crash_dir   = crashes;
        profile.capture_dir = captures;
        cont::String text;
        cp::diagnostic_host_profile_to_asset(profile, text);
        const cont::String profile_path = scratch.file("app.host.toml");
        write_text(profile_path, text.c_str());

        cd::Expectation e;
        e.want              = cd::Expectation::Want::CleanExit; // the bundle and the exit code are the oracle here
        e.expected_identity = "crd-diag-host-specimen";
        e.timeout_ms        = 30000U;
        cont::Array<cont::String> args;
        args.push_back(cont::String{profile_path.c_str()});
        args.push_back(cont::String{mode});
        const cd::Outcome o = cd::run_specimen(cont::String{CRD_DIAG_HOST_SPECIMEN}, args, e);
        INFO("mode=" << mode << " verdict=" << cd::verdict_name(o.verdict) << " exit=" << o.exit_code
                     << " reason=" << o.reason.c_str());
        CHECK(o.verdict != cd::Verdict::MissingExecutable);
        CHECK(o.verdict != cd::Verdict::MismatchedBinary);
        CHECK_FALSE(o.timed_out);
        CHECK((o.crashed || o.exit_code != 0)); // the fatal path ended it; exit 0 means it returned
        if (!built_with_asan())
        {
#if defined(_WIN32)
            CHECK(o.exit_code == 3); // abort()
#else
            CHECK(o.crashed); // SIGABRT
#endif
        }
        // The host started from the profile before the failure, the crash handler with it.
        CHECK(contains(o.output, "CRD_HOST_STARTED crash=active flight=active faults=off"));
        CHECK(fs::is_directory(fs::Path{cont::StringView{crashes.data(), crashes.size()}}));

        // The bundle names what failed, the events before it, the replay recipe, this build and the executable.
        const cont::String bundle_path = scratch.file("captures/flight-0.cdb");
        const BundleRead   bundle      = read_flight_bundle(bundle_path);
        REQUIRE(bundle.import.status == cp::ImportStatus::Ok);
        REQUIRE(bundle.has_flight);
        REQUIRE(bundle.flight.ok);
        const cp::FlightCapture& c = bundle.flight.capture;
        if (std::strcmp(mode, "invariant") == 0)
        {
            CHECK(c.trigger.kind == cp::FlightTrigger::FatalInvariant);
            CHECK(c.trigger.code == 0x0711U);
            CHECK(c.trigger.message_view() == "the order total went negative");
        }
        else
        {
            CHECK(c.trigger.kind == cp::FlightTrigger::Assertion);
            CHECK(c.trigger.message_view() == "total >= 0: the order total went negative");
        }
        CHECK(c.end == cp::FlightEnd::Fatal);
        CHECK(c.recorded_before_trigger == 40U);
        REQUIRE(c.events.size() == 40U);
        CHECK(c.events[0].message_view() == "order 0");
        CHECK(c.events[39].message_view() == "order 39");
        CHECK(c.replay_available);
        CHECK(cont::StringView{c.replay.c_str(), c.replay.size()} ==
              "crd-diag-host-specimen <profile> invariant|assert");
        REQUIRE(bundle.import.has_build);
        cont::String differing;
        CHECK(cp::same_build_identity(bundle.import.build, cp::current_build_identity(), differing));
        REQUIRE(bundle.import.has_symbols);
        const cp::ModuleIdentity* named = find_module(bundle.import.symbols, base_name(CRD_DIAG_HOST_SPECIMEN));
        REQUIRE(named != nullptr);
        cp::ModuleIdentity from_file;
        REQUIRE(crd::test::module_file::file_identity(CRD_DIAG_HOST_SPECIMEN, from_file));
        CHECK(cp::identity_matches(*named, from_file));
    }
}
