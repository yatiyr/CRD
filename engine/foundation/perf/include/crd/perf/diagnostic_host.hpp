#pragma once

// crd-perf -- DIAG.11a: the reusable diagnostic host, the lifecycle every application, headless tool and the sandbox
// composes instead of wiring the crash handler, the flight recorder and the fault injector on its own.
//
// A DiagnosticHost owns, for one process:
//   - the crash handler (crd::crash::install into the profile's crash directory) when the profile asks for it;
//   - the FlightRecorder, started from the profile's flight-policy asset (or FlightPolicy{}), and its trigger sources:
//     assertions and fatal invariants (chained to the handlers installed before), the jobs watchdog's starvation and
//     hang reports (configure_jobs gives the pool the profile's watchdog window), frame hitches (frame(), once per
//     frame) and allocation failure at an allocator the host wraps (watch_allocator);
//   - a FlightCommandHost as the recorder's sink, its module snapshot taken at start and refreshed after the host
//     loads modules; with a capture directory every delivered capture is also written there as a flight bundle
//     (flight-<n>.cdb, the first free n, never overwriting, the newest `max_bundles` kept). The writer runs wherever
//     a capture is delivered, a fatal path included: it reads the snapshot with try_modules(), logs nothing and never
//     calls back into the recorder or a service;
//   - a FaultInjector attached to the recorder, consulted by the wrapped allocator and, with the I/O and GPU fault
//     seams' hooks the host installs at start, by every read site and device-loss seam. A shipping build compiles
//     the seams out, so there fault injection is Unavailable;
//   - the flight.* and fault.* commands, registered by serve() on a host's diagnostic service.
//
// The profile is an authorable asset (kind "diagnostic-host"), not C++: it chooses the modes and their bounds.
//
//     kind               = "diagnostic-host"
//     schema_version     = 1
//     crash_handler      = "on"            # "off", "on" (used when this build can) or "required" (refuse otherwise)
//     crash_dir          = "crashes"       # where the crash handler writes dumps
//     flight             = "on"            # the flight recorder: "off", "on" or "required"
//     flight_policy      = ""              # a flight-policy asset; "" is FlightPolicy{}
//     capture_dir        = ""              # an existing directory for flight bundles; "" keeps captures in memory
//     max_bundles        = 10              # the newest bundles kept in capture_dir (1 to 1000)
//     fault_injection    = "off"           # the fault seams' hooks and fault.*: "off", "on" or "required"
//     command_grant      = ["read"]        # the authority the host's diagnostic service is granted
//     allow_remote       = false           # remote-enable, upload and process-memory need this to be true
//     watchdog_period_ms = 250             # the jobs watchdog window (0 leaves the pool's own setting)
//
// Every key but the header is optional and keeps the default above when absent; an unknown key, a value of the wrong
// type or range and a profile that breaks a rule below are refused with the key named, as the other diagnostic assets
// are. Relative paths are relative to the process's working directory, as command-line paths are. The rules: a grant
// that names remote-enable, upload or process-memory needs allow_remote = true (the default policy is local and
// exposes no remote debugger); a grant that names inject needs fault_injection on or required; fault injection,
// flight_policy and capture_dir need flight on or required; crash_dir is not empty while the crash handler is asked
// for. The flight.* commands are served while the recorder runs and fault.* while fault injection runs: a shipping
// build, which compiles the fault seams out, reports injection Unavailable and serves no fault.* command.
//
// Startup chooses the modes before anything that depends on them exists: start() refuses once the jobs pool runs
// (its watchdog window and handlers are part of the choice), and the host must start before devices and tools load,
// so a "required" mode this build cannot provide refuses the run before any of them is created. A mode asked for as
// "on" that the build cannot provide is reported as Unavailable and the run continues.
//
// A changed profile is applied with apply(): a capture directory, the bundle bound and the flight policy (re-read from
// its asset, so an edited policy file counts as a change) apply live; every other field takes effect only at the next
// start, and apply() then says RestartRequired, names the fields and changes nothing. classify_diagnostic_host_change
// gives the same answer without a host.
//
// stop() (and the destructor) undo everything in order: the seams' hooks first (uninstall waits for calls in
// progress), the plan disarmed, the jobs handlers cleared, the assertion and fatal triggers removed, the recorder shut
// down (which delivers a pending capture, and writes it, before the ring is released), and the crash handler
// uninstalled. The pool's watchdog thread must be joined (crd::jobs::shutdown) before the host is stopped.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.11a); ADR-0133.

#include <crd/containers/string.hpp>
#include <crd/core/crash.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/bundle.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_commands.hpp>
#include <crd/perf/flight_recorder.hpp>
#include <crd/perf/flight_triggers.hpp>

#include <atomic>
#include <memory>
#include <mutex>

namespace crd::jobs
{
struct Config;
} // namespace crd::jobs

namespace crd::perf
{
namespace cont = crd::containers;

// How a profile asks for one mode.
enum class DiagMode : crd::u8
{
    Off,
    On,       // used when this build can provide it; otherwise reported Unavailable and the run continues
    Required, // the run is refused when this build cannot provide it
};

// "off", "on", "required".
[[nodiscard]] cont::StringView diag_mode_name(DiagMode m) noexcept;

inline constexpr crd::u32 kDiagHostMaxBundles     = 1000U;
inline constexpr crd::u32 kDiagHostMaxWatchdogMs  = 60000U;
inline constexpr crd::u32 kDiagHostDefaultWatchMs = 250U;

struct DiagnosticHostProfile
{
    DiagMode         crash_handler      = DiagMode::On;
    cont::String     crash_dir          = cont::String{"crashes"};
    DiagMode         flight             = DiagMode::On;
    cont::String     flight_policy      = cont::String{}; // a flight-policy asset path; empty: FlightPolicy{}
    cont::String     capture_dir        = cont::String{}; // an existing directory; empty: captures kept in memory
    crd::u32         max_bundles        = kDefaultMaxBundles;
    DiagMode         fault_injection    = DiagMode::Off;
    DiagAuthoritySet command_grant      = authority_bit(DiagAuthority::Read);
    bool             allow_remote       = false;
    crd::u32         watchdog_period_ms = kDiagHostDefaultWatchMs;
};

// The rules above, for a profile built in code. False with `reason` naming the key on refusal.
[[nodiscard]] bool validate_diagnostic_host_profile(const DiagnosticHostProfile& p, cont::String& reason);

// Parse a diagnostic-host document. A refusal leaves `out` untouched.
[[nodiscard]] bool parse_diagnostic_host_profile(cont::StringView text, DiagnosticHostProfile& out,
                                                 cont::String& reason);

// Read and parse a profile asset file (at most kDiagAssetMaxBytes).
[[nodiscard]] bool load_diagnostic_host_profile(const char* path, DiagnosticHostProfile& out, cont::String& reason);

// The canonical document for `p` (every key); parse_diagnostic_host_profile reads it back to the same profile.
void diagnostic_host_profile_to_asset(const DiagnosticHostProfile& p, cont::String& out);

// The profile's fields, one bit each, for change reports.
enum class DiagHostField : crd::u16
{
    CrashHandler     = 1U << 0U,
    CrashDir         = 1U << 1U,
    Flight           = 1U << 2U,
    FlightPolicy     = 1U << 3U,
    CaptureDir       = 1U << 4U,
    MaxBundles       = 1U << 5U,
    FaultInjection   = 1U << 6U,
    CommandGrant     = 1U << 7U,
    AllowRemote      = 1U << 8U,
    WatchdogPeriodMs = 1U << 9U,
};

inline constexpr crd::u32 kDiagHostFieldCount = 10U;
// The fields apply() changes on a running host; every other field needs a restart.
inline constexpr crd::u32 kDiagHostLiveFields = static_cast<crd::u32>(DiagHostField::FlightPolicy) |
                                                static_cast<crd::u32>(DiagHostField::CaptureDir) |
                                                static_cast<crd::u32>(DiagHostField::MaxBundles);

// The asset key of one field ("crash_handler", ..., "watchdog_period_ms").
[[nodiscard]] cont::StringView diag_host_field_name(DiagHostField f) noexcept;

// Append the names of the fields set in `fields`, comma-separated in bit order ("none" for no field).
void append_diag_host_fields(cont::String& out, crd::u32 fields);

enum class DiagHostChangeKind : crd::u8
{
    Unchanged,       // no field differs
    Live,            // only live fields differ: apply() changes them on the running host
    RestartRequired, // a field that is chosen at start differs: it takes effect at the next start
};

// "unchanged", "live", "restart-required".
[[nodiscard]] cont::StringView diag_host_change_kind_name(DiagHostChangeKind k) noexcept;

struct DiagHostChange
{
    DiagHostChangeKind kind           = DiagHostChangeKind::Unchanged;
    crd::u32           live_fields    = 0U; // live fields that differ
    crd::u32           restart_fields = 0U; // restart fields that differ
};

// What changes between the active profile and a candidate. flight_policy is compared by path here; apply() also
// re-reads the asset, so an edited policy file is a live change.
[[nodiscard]] DiagHostChange classify_diagnostic_host_change(const DiagnosticHostProfile& active,
                                                             const DiagnosticHostProfile& candidate) noexcept;

// What this build can provide. A host passes another set only to model a build it is not (tests).
struct DiagnosticHostCapabilities
{
    bool crash_backend = false; // this platform build has an in-process crash handler
    bool fault_seams   = false; // the I/O and GPU fault seams are compiled in (crd::fault_seams_compiled())
};

[[nodiscard]] DiagnosticHostCapabilities diagnostic_host_capabilities() noexcept;

// What a started host runs, mode by mode.
enum class DiagModeState : crd::u8
{
    Off,         // the profile asked for it off
    Active,      // running
    Unavailable, // asked for "on" and this build or this machine cannot provide it (the reason says why)
};

// "off", "active", "unavailable".
[[nodiscard]] cont::StringView diag_mode_state_name(DiagModeState s) noexcept;

struct DiagnosticHostModes
{
    DiagModeState         crash         = DiagModeState::Off;
    DiagModeState         flight        = DiagModeState::Off;
    DiagModeState         faults        = DiagModeState::Off;
    crd::crash::InstallResult crash_install = crd::crash::InstallResult::Unsupported; // when crash was attempted
};

enum class DiagHostStart : crd::u8
{
    Started,
    AlreadyStarted,   // start() was called before
    LateStart,        // the jobs pool already runs: the host must start before the pool, devices and tools
    ProfileRefused,   // the profile breaks a rule (reason names the key)
    PolicyRefused,    // the flight-policy asset could not be read or was refused
    DirectoryMissing, // the capture directory is not an existing directory
    ModeUnavailable,  // a "required" mode this build or this machine cannot provide (reason names it)
    RecorderRefused,  // the recorder refused the policy or its ring
    TriggersRefused,  // another recorder owns the assertion or fatal-invariant slot
};

// "started", "already-started", "late-start", "profile-refused", "policy-refused", "directory-missing",
// "mode-unavailable", "recorder-refused", "triggers-refused".
[[nodiscard]] cont::StringView diag_host_start_name(DiagHostStart s) noexcept;

enum class DiagHostApply : crd::u8
{
    Applied,         // the live fields that differed now hold the candidate's values
    Unchanged,       // nothing differed
    RestartRequired, // a start-time field differs (reason names them); nothing was changed
    Busy,            // the recorder holds a pending capture, so the policy cannot change now; nothing was changed
    Refused,         // the candidate breaks a rule, its policy asset or capture directory is refused; nothing changed
    NotStarted,      // the host is not started
};

// "applied", "unchanged", "restart-required", "busy", "refused", "not-started".
[[nodiscard]] cont::StringView diag_host_apply_name(DiagHostApply a) noexcept;

class DiagnosticHost
{
public:
    DiagnosticHost() noexcept;
    ~DiagnosticHost();

    DiagnosticHost(const DiagnosticHost&)            = delete;
    DiagnosticHost& operator=(const DiagnosticHost&) = delete;
    DiagnosticHost(DiagnosticHost&&)                 = delete;
    DiagnosticHost& operator=(DiagnosticHost&&)      = delete;

    // Choose the modes and install what they need (see the header). Nothing stays installed when it refuses; `reason`
    // says why. `caps` is what this build provides.
    DiagHostStart start(const DiagnosticHostProfile& profile, cont::String& reason,
                        const DiagnosticHostCapabilities& caps = diagnostic_host_capabilities());

    // Apply a changed profile to the running host (see the header). Nothing changes unless it answers Applied.
    DiagHostApply apply(const DiagnosticHostProfile& candidate, cont::String& reason);

    // Give the pool the profile's watchdog window, when the recorder runs and the pool has none of its own.
    void configure_jobs(crd::jobs::Config& config) const noexcept;

    // Point the service's doctor at the live recorder and injector (only those that run).
    void configure_service(DiagServiceConfig& config) const noexcept;

    // Register flight.* (when the recorder runs) and fault.* (when fault injection runs) with `service`. False when
    // a command could not be registered. Call once, after start(); the service must be destroyed before this host.
    [[nodiscard]] bool serve(DiagCommandService& service);

    // The allocator to hand an owner instead of `backing`: injected failures first (when fault injection runs), then
    // real failures reported as allocation-failure triggers, then `backing`. One per host; nullptr when the recorder
    // does not run, `backing` is null or one was already made. It must not outlive the host.
    [[nodiscard]] crd::memory::IAllocator* watch_allocator(crd::memory::IAllocator* backing, const char* name);

    // Merge the loaded modules into the snapshot bundles carry. Call after loading modules, on the host's thread.
    bool refresh_modules();

    // One frame-loop iteration, as monotonic diagnostic nanoseconds (diagnostic_now_ns): the frame-hitch trigger sees
    // it (the interval is start to start), then a capture whose post window has closed is delivered. True when a
    // capture was delivered.
    bool frame(crd::u64 begin_ns, crd::u64 end_ns);

    // Deliver a capture whose post window has closed, for a host with no frame loop. True when one was delivered.
    bool poll();

    // Undo everything start() installed (see the header). Safe to call twice.
    void stop() noexcept;

    [[nodiscard]] bool                         started() const noexcept { return m_started; }
    [[nodiscard]] const DiagnosticHostModes&   modes() const noexcept { return m_modes; }
    [[nodiscard]] const DiagnosticHostProfile& profile() const noexcept { return m_profile; }
    [[nodiscard]] bool hooks_installed() const noexcept { return m_io_hook || m_gpu_hook; }
    [[nodiscard]] FlightRecorder&    recorder() noexcept { return m_recorder; }
    [[nodiscard]] FlightCommandHost& command_host() noexcept { return m_command_host; }
    [[nodiscard]] FaultInjector&     injector() noexcept { return m_injector; }
    [[nodiscard]] crd::u64           hitches() const noexcept { return m_frame_watch.hitches(); }
    // Frames fed through frame() since start.
    [[nodiscard]] crd::u64 frames() const noexcept { return m_frames; }

    // Bundles written to the capture directory, and the path of the last one ("" before one was written).
    [[nodiscard]] crd::u64 bundles_written() const noexcept
    {
        return m_bundles_written.load(std::memory_order_acquire);
    }
    [[nodiscard]] cont::String last_bundle() const;
    // Captures the writer could not write (the bundle could not be built, no free name, or the write failed).
    [[nodiscard]] crd::u64 bundle_failures() const noexcept
    {
        return m_bundle_failures.load(std::memory_order_acquire);
    }

private:
    static void write_capture(const FlightCapture& capture, void* host);
    void        undo_start() noexcept;

    FlightRecorder                          m_recorder;
    FlightCommandHost                       m_command_host{m_recorder};
    FaultInjector                           m_injector;
    FlightFrameWatch                        m_frame_watch{&m_recorder};
    std::unique_ptr<FlightAllocatorWatch>   m_alloc_watch;
    std::unique_ptr<FaultAllocator>         m_alloc_faults;
    DiagnosticHostProfile                   m_profile;
    DiagnosticHostModes                     m_modes;
    crd::u64                                m_frames         = 0U;
    bool                                    m_started        = false;
    bool                                    m_crash          = false;
    bool                                    m_assert_trigger = false;
    bool                                    m_fatal_trigger  = false;
    bool                                    m_jobs_handlers  = false;
    bool                                    m_io_hook        = false;
    bool                                    m_gpu_hook       = false;
    mutable std::mutex                      m_bundle_mutex; // the capture directory, the bound, the name, last_bundle
    cont::String                            m_capture_dir;
    crd::u32                                m_max_bundles = kDefaultMaxBundles;
    crd::u64                                m_next_name   = 0U;
    cont::String                            m_last_bundle;
    std::atomic<crd::u64>                   m_bundles_written{0U};
    std::atomic<crd::u64>                   m_bundle_failures{0U};
};

} // namespace crd::perf
