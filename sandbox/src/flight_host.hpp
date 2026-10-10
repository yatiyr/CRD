#pragma once

// sandbox — DIAG.9b: the sandbox as a real flight-recorder and fault-injection host.
//
// A FlightHost owns the process's FlightRecorder and everything that feeds and reads it:
//   - the recorder, initialised with the default policy (FlightPolicy{}, which crd-perf-tests holds equal to the
//     committed assets/diag/flight_default.flight.toml) or an authored flight-policy asset (`--flight-policy`); a
//     refused asset refuses the run, as an asked-for and unavailable option does elsewhere in the sandbox;
//   - its trigger sources: assertions and fatal invariants (chained to the handlers installed before), the jobs
//     watchdog's starvation and hang reports (configure_jobs gives the pool the watchdog period they need), frame
//     hitches (frame(), once per frame loop iteration) and allocation failure at the resource manager's allocator
//     (watch_allocator). The sandbox has no StreamingAllocator, so the resource-pressure trigger has no seam here;
//   - a FlightCommandHost as the recorder's sink, with its module snapshot taken at start and refreshed by the host
//     after it loads modules (the GPU driver); with a capture directory (`--flight-dir`) every delivered capture is
//     also written there as a flight bundle (flight-<n>.cdb, the first free n, never overwriting; the newest
//     kDefaultMaxBundles are kept). The writer runs wherever a capture is delivered, including a fatal path, so it
//     reads the snapshot with try_modules(), logs nothing and never calls back into the recorder or a service;
//   - a FaultInjector attached to the recorder, consulted by the resource manager's allocator (allocation-failure)
//     and, once serve() installs its hooks, by the I/O and GPU fault seams (every read site and device-loss). A
//     shipping build compiles the seams out: the hooks are refused and every arm answers Unavailable;
//   - the flight.* and fault.* commands, registered by serve() on the sandbox's diagnostic service, and the live
//     instances the doctor reports through diag.capabilities (configure_service).
//
// stop() (and the destructor) undo all of it in order: the seams' hooks first (uninstall waits for calls in
// progress), the plan disarmed, the jobs handlers cleared, the assertion and fatal triggers removed, then the recorder
// shut down, which delivers a pending capture (and writes it) before the ring is released. The pool's watchdog thread
// must be joined (crd::jobs::shutdown) before the host is destroyed.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9b); ADR-0133.

#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/fault_injection.hpp>
#include <crd/perf/flight_commands.hpp>
#include <crd/perf/flight_recorder.hpp>
#include <crd/perf/flight_triggers.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string_view>

namespace crd::jobs
{
struct Config;
} // namespace crd::jobs

namespace crd::perf
{
class DiagCommandService;
struct DiagServiceConfig;
} // namespace crd::perf

namespace crd::sandbox
{

struct FlightHostOptions
{
    const char* policy_asset = nullptr; // an authored flight-policy asset; nullptr: FlightPolicy{}
    const char* capture_dir  = nullptr; // every delivered capture is written here as a bundle; nullptr: none
};

enum class FlightHostStart : u8
{
    Started,
    AlreadyStarted,   // start() was called before
    PolicyRefused,    // the policy asset could not be read or was refused (reason names the key)
    RecorderRefused,  // the recorder refused the policy or its ring
    DirectoryMissing, // the capture directory is not an existing directory
    TriggersRefused,  // another recorder owns the assertion or fatal-invariant slot
};

// "started", "already-started", "policy-refused", "recorder-refused", "directory-missing", "triggers-refused".
[[nodiscard]] std::string_view flight_host_start_name(FlightHostStart s) noexcept;

class FlightHost
{
public:
    // The jobs watchdog window configure_jobs gives a pool that has none: the starvation and hang reports need it.
    static constexpr u32 kWatchdogPeriodMs = 250U;

    FlightHost() noexcept;
    ~FlightHost();

    FlightHost(const FlightHost&)            = delete;
    FlightHost& operator=(const FlightHost&) = delete;
    FlightHost(FlightHost&&)                 = delete;
    FlightHost& operator=(FlightHost&&)      = delete;

    // Initialise the recorder, take the module snapshot, install the sinks and the trigger sources. Nothing stays
    // installed when it refuses; `reason` says why.
    FlightHostStart start(const FlightHostOptions& options, crd::containers::String& reason);

    // Give the pool the watchdog window the starvation and hang triggers need (only when it has none).
    void configure_jobs(crd::jobs::Config& config) const noexcept;

    // Point the service's doctor at the live recorder and injector.
    void configure_service(crd::perf::DiagServiceConfig& config) const noexcept;

    // Register flight.* and fault.* with `service` and install the injector's I/O and GPU fault hooks (refused in a
    // shipping build, where the commands still answer). False when a command could not be registered. Call once,
    // after start(); the service must be destroyed before this host.
    [[nodiscard]] bool serve(crd::perf::DiagCommandService& service);

    // The allocator to hand the resource manager instead of `backing`: injected failures first (FaultAllocator), then
    // real failures reported as allocation-failure triggers (FlightAllocatorWatch), then `backing`. One per host; it
    // must not outlive the host.
    [[nodiscard]] crd::memory::IAllocator* watch_allocator(crd::memory::IAllocator* backing);

    // Merge the loaded modules into the snapshot bundles carry. Call after loading modules, on the host's thread.
    bool refresh_modules();

    // One frame-loop iteration, as monotonic diagnostic nanoseconds (crd::perf::diagnostic_now_ns): the frame-hitch
    // trigger sees it, then a capture whose post window has closed is delivered. True when a capture was delivered.
    bool frame(u64 begin_ns, u64 end_ns);

    // Undo everything start() and serve() installed (see the header). Safe to call twice.
    void stop() noexcept;

    [[nodiscard]] bool started() const noexcept { return m_started; }
    [[nodiscard]] bool hooks_installed() const noexcept { return m_io_hook || m_gpu_hook; }
    [[nodiscard]] crd::perf::FlightRecorder&    recorder() noexcept { return m_recorder; }
    [[nodiscard]] crd::perf::FlightCommandHost& command_host() noexcept { return m_command_host; }
    [[nodiscard]] crd::perf::FaultInjector&     injector() noexcept { return m_injector; }
    [[nodiscard]] u64                           hitches() const noexcept { return m_frame_watch.hitches(); }

    // Bundles written to the capture directory, and the path of the last one ("" before one was written).
    [[nodiscard]] u64 bundles_written() const noexcept { return m_bundles_written.load(std::memory_order_acquire); }
    [[nodiscard]] crd::containers::String last_bundle() const;
    // Captures the writer could not write (the bundle could not be built, no free name, or the write failed).
    [[nodiscard]] u64 bundle_failures() const noexcept { return m_bundle_failures.load(std::memory_order_acquire); }

private:
    static void write_capture(const crd::perf::FlightCapture& capture, void* host);

    crd::perf::FlightRecorder                     m_recorder;
    crd::perf::FlightCommandHost                  m_command_host{m_recorder};
    crd::perf::FaultInjector                      m_injector;
    crd::perf::FlightFrameWatch                   m_frame_watch{&m_recorder};
    std::unique_ptr<crd::perf::FlightAllocatorWatch> m_alloc_watch;
    std::unique_ptr<crd::perf::FaultAllocator>       m_alloc_faults;
    bool                                          m_started        = false;
    bool                                          m_assert_trigger = false;
    bool                                          m_fatal_trigger  = false;
    bool                                          m_jobs_handlers  = false;
    bool                                          m_io_hook        = false;
    bool                                          m_gpu_hook       = false;
    mutable std::mutex                            m_bundle_mutex; // the capture directory, the next name, last_bundle
    crd::containers::String                       m_capture_dir;
    u64                                           m_next_name = 0U;
    crd::containers::String                       m_last_bundle;
    std::atomic<u64>                              m_bundles_written{0U};
    std::atomic<u64>                              m_bundle_failures{0U};
};

} // namespace crd::sandbox
