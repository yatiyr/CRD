#pragma once

// crd-diag-overhead -- the DIAG.11b measurement harness: what every workload shares.
//
// A run measures one workload under one diagnostic mode in a fresh process (scripts/diag-overhead.py launches the
// processes, interleaves the modes and judges the budgets; this program only measures and states what it saw):
//
//   off    no diagnostic host, no flight recorder, profiler not started: the runtime-disabled mode. Built with
//          profiling compiled out (win-shipping, linux-gcc-shipping) it is also the compiled-out mode.
//   basic  the engine's DiagnosticHost started from the default host profile with the authored basic flight policy
//          (overhead_basic.flight.toml: Warning and above kept): crash handler, flight recorder and its triggers, the
//          jobs watchdog window, the allocator watch and one host frame per iteration.
//   full   the same host with the authored full flight policy (every event kept), plus the profiler started, the jobs
//          adapter installed, the CEIR plan profiled and the profiler's rings drained each frame.
//   tracy  crd-diag-overhead-tracy only (CRD_DIAG_PEER_TRACY): no Cerid diagnostic; Tracy Profiler started instead,
//          with its counterparts at the same sites (DIAG.11b clause (8), the peer on comparable features): a message
//          per site, a zone per zone site, per job and per dispatched CEIR op, a frame mark per frame and per mark
//          site, memory events with --memory. Tracy runs on demand: with no client connected every site checks the
//          connection and does nothing; --tracy-connect waits for a client (tracy-capture) before the frames.
//
// Each iteration is one frame of the workload's own work, timed from its start to the end of the host's frame call;
// the workload's application event sites are CRD_FLIGHT_RECORD sites with formatted payloads (the site form the
// modes contract holds to "a disabled site evaluates no payload"). Contract: docs/design/runtime-diagnostics.md
// (DIAG.11b); ADR-0133 ID-5.

#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/gpu/validation.hpp>
#include <crd/perf/diagnostic_host.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <atomic>
#include <cstdio>

#ifndef CRD_DIAG_OVERHEAD_TRACY
#define CRD_DIAG_OVERHEAD_TRACY 0 // NOLINT(cppcoreguidelines-macro-usage): set by the peer harness's target
#endif
#if CRD_DIAG_OVERHEAD_TRACY
#include <crd/ceir/plan.hpp>

#include <tracy/Tracy.hpp> // the pinned client's C++ macros; tracy-client's definitions configure them
#endif

namespace crd::memory
{
class IAllocator;
} // namespace crd::memory

namespace diag_overhead
{

namespace cont = crd::containers;
namespace cp   = crd::perf;

enum class Mode : crd::u8
{
    Off,
    Basic,
    Full,
    Tracy, // the peer: crd-diag-overhead-tracy only
};

// "off", "basic", "full", "tracy".
[[nodiscard]] const char* mode_name(Mode m) noexcept;

// The device validation the render workload's device runs with (--validation): none, the core layer, or the core
// layer and one more mode. ADR-0133 reports its cost apart from the modes' budgets (DIAG.11b clause (9)).
enum class Validation : crd::u8
{
    None,
    Core,
    Sync,
    Gpu,
};

// "none", "core", "sync", "gpu".
[[nodiscard]] const char* validation_name(Validation v) noexcept;

// What the device validation saw, read at teardown once every context it covers is destroyed. A capture that missed
// part of the run (installed after a device, no layer loaded) is an instrumentation failure, so it never reads clean.
struct ValidationOutcome
{
    crd::gpu::ValidationActivation activation{};
    crd::gpu::ValidationReport report{};
};

// The application event codes the workloads' sites record, above the foundation range.
inline constexpr cp::EventCode kCodeFirst      = 0x00A20000U;
inline constexpr cp::EventCode kCodeLast       = 0x00A2FFFFU;
inline constexpr cp::EventCode kCodeAllocBatch = 0x00A20001U; // a = operations so far, b = live bytes
inline constexpr cp::EventCode kCodeJobDone    = 0x00A20002U; // a = job, b = its sum
inline constexpr cp::EventCode kCodeProgramRan = 0x00A20003U; // a = iteration, b = the program's result
inline constexpr cp::EventCode kCodeEvent      = 0x00A20004U; // a = event, b = iteration
inline constexpr cp::EventCode kCodeRead       = 0x00A20005U; // a = read, b = its bytes
inline constexpr cp::EventCode kCodeFrame      = 0x00A20006U; // a = instances drawn, b = draws

// What a workload's sites write to. `payloads` counts payload evaluations (a site's message is built only when the
// recorder admits the event), across every thread the workload runs on.
struct Sites
{
    cp::FlightRecorder*   recorder = nullptr; // null in the off mode
    std::atomic<crd::u64> payloads{0U};
    bool                  tracy = false; // the tracy mode: every site is Tracy's counterpart
};

// An application event site. In the Cerid modes exactly CRD_FLIGHT_RECORD (the payload is evaluated only when the
// recorder keeps the event). In the tracy mode Tracy's message at the same site, guarded by the manual's connection
// status (TracyIsConnected), so its text is formatted only while a client is connected, as Cerid's is only for a kept
// event; a bare TracyMessage would format every text. Tracy keeps no scalars and filters no severity in the client.
#if CRD_DIAG_OVERHEAD_TRACY
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): only a macro leaves the payload unevaluated when the site is off.
#define DOV_SITE(sites, code, severity, message, value_a, value_b)                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((sites).tracy)                                                                                             \
        {                                                                                                              \
            if (TracyIsConnected)                                                                                      \
            {                                                                                                          \
                const ::crd::containers::StringView dov_site_text_ = (message);                                        \
                TracyMessage(dov_site_text_.data(), dov_site_text_.size());                                            \
            }                                                                                                          \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            CRD_FLIGHT_RECORD((sites).recorder, code, severity, message, value_a, value_b);                            \
        }                                                                                                              \
    } while (false)
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): only a macro leaves the payload unevaluated when the site is off.
#define DOV_SITE(sites, code, severity, message, value_a, value_b)                                                     \
    CRD_FLIGHT_RECORD((sites).recorder, code, severity, message, value_a, value_b)
#endif

struct WorkloadSetup
{
    Mode                mode       = Mode::Off;
    cp::DiagnosticHost* host       = nullptr; // started in basic and full; null in off
    Sites*              sites      = nullptr;
    crd::u32            threads    = 8U;      // jobs workload: the pool's threads (io runs two)
    const char*         program    = nullptr; // offline workload: the authored CEIR program
    const char*         scratch    = nullptr; // io and render: the directory their files are written to (--scratch)
    const char*         assets     = nullptr; // render workload: the engine asset root its authored frame graph is in
    Validation          validation = Validation::None; // render workload: its device's validation (--validation)
    bool                memory     = false; // alloc workload (--memory): full counts the heap's statistics through
                                            // the profiler's allocator registry, tracy reports every operation
};

class Workload
{
public:
    Workload()                           = default;
    virtual ~Workload()                  = default;
    Workload(const Workload&)            = delete;
    Workload& operator=(const Workload&) = delete;
    Workload(Workload&&)                 = delete;
    Workload& operator=(Workload&&)      = delete;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    // Fill the process-lifetime caches the workload's path keeps (the Vulkan backend's shader compiler library),
    // before the lifetime ledger's window opens: they outlive every mode and are not the run's to release.
    virtual bool warm(cont::String& reason)
    {
        (void)reason;
        return true;
    }
    // Prepare everything an iteration needs. False with `reason` when the workload cannot run here.
    virtual bool setup(const WorkloadSetup& setup, cont::String& reason) = 0;
    // One frame of work. Its result signature is compared with expected(); false when the work itself failed.
    virtual bool iterate(crd::u64& signature) = 0;
    virtual void teardown() = 0;
    // The application event sites one iteration executes, and the signature every iteration must produce.
    [[nodiscard]] virtual crd::u64 sites_per_iteration() const noexcept = 0;
    [[nodiscard]] virtual crd::u64 expected() const noexcept           = 0;
    // The threads setup() starts for the workload itself (the jobs pool's workers), and those a mode adds to it (the
    // jobs watchdog the host's window asks for), as the lifetime ledger counts them.
    [[nodiscard]] virtual crd::u64 own_threads(crd::u32 threads) const noexcept
    {
        (void)threads;
        return 0U;
    }
    [[nodiscard]] virtual crd::u64 diagnostic_threads(Mode mode) const noexcept
    {
        (void)mode;
        return 0U;
    }
    // The allocator the frames allocate from, when the workload owns one (alloc: the TLSF heap itself, never the
    // host's watch over it), alive until teardown(). No mode requests its statistics, so the harness holds them to
    // what the build counts unasked: everything on CRD_DEBUG builds, nothing on the others (DIAG.11b).
    [[nodiscard]] virtual const crd::memory::IAllocator* measured_allocator() const noexcept { return nullptr; }
    // After teardown: what the device validation the setup asked for saw; null when none was asked for.
    [[nodiscard]] virtual const ValidationOutcome* validation() const noexcept { return nullptr; }
    // The frame marks one iteration makes besides the harness's own one per frame (the marks workload).
    [[nodiscard]] virtual crd::u64 marks_per_iteration() const noexcept { return 0U; }
    // The profiler zones the workload's own zone sites recorded over the run, read before the full mode's consumer
    // drains them each frame, and the zones one iteration opens (the zones workload).
    [[nodiscard]] virtual crd::u64 zones_recorded() const noexcept { return 0U; }
    [[nodiscard]] virtual crd::u64 zones_per_iteration() const noexcept { return 0U; }
    // The tracy mode: the Tracy zones the workload emitted while a client was connected (its zone sites, its jobs and
    // its CEIR ops), which a connected capture must hold.
    [[nodiscard]] virtual crd::u64 tracy_zones() const noexcept { return 0U; }
};

// The workloads by name ("alloc", "jobs", "offline", "io", "render", "events"); null for an unknown name. The object is
// static: one workload per process.
[[nodiscard]] Workload* find_workload(const char* name) noexcept;

// Open `path` for binary reading (fopen_s where the CRT deprecates fopen); null when it cannot be opened.
[[nodiscard]] std::FILE* open_for_reading(const char* path) noexcept;

#if CRD_DIAG_OVERHEAD_TRACY
// The peer's out-of-line half (tracy_peer.cpp): Tracy's lifetime and connection, and its counterparts of the two
// Cerid observers that are callbacks (the jobs adapter's JobObserver and the CEIR bridge's RunHooks).
namespace tracy_peer
{
void start() noexcept;
void stop() noexcept;
[[nodiscard]] bool connected() noexcept;
// Wait up to `ms` milliseconds for a client; true once one is connected.
[[nodiscard]] bool wait_connected(crd::u32 ms) noexcept;
// A Tracy zone per job through crd-jobs' observer seam, as the jobs adapter's region per job. The jobs zoned while
// connected are counted, so a capture can be held to them; a job that ends with no zone open on its thread (none here:
// no job of these workloads waits, so none moves) is counted apart.
void install_jobs_observer() noexcept;
void uninstall_jobs_observer() noexcept;
[[nodiscard]] crd::u64 jobs_zoned() noexcept;
[[nodiscard]] crd::u64 jobs_unmatched() noexcept;
// A Tracy zone per dispatched CEIR op, named by its op class, through the plan's RunHooks as the profiling bridge's
// hooks run (the bridge aggregates a dispatch count and self time per op class instead; Tracy derives both from the
// zones). One run at a time, on the offline workload's thread; ops nested past the bridge's 256 levels are not zoned
// and counted apart, as a post with no open zone is.
[[nodiscard]] crd::ceir::plan::RunHooks op_zone_hooks() noexcept;
[[nodiscard]] crd::u64 ops_zoned() noexcept;     // ops zoned while connected
[[nodiscard]] crd::u64 ops_unmatched() noexcept; // unzoned nesting, unmatched posts and zones still open
} // namespace tracy_peer
#endif

} // namespace diag_overhead
