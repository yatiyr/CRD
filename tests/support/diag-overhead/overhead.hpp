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
//
// Each iteration is one frame of the workload's own work, timed from its start to the end of the host's frame call;
// the workload's application event sites are CRD_FLIGHT_RECORD sites with formatted payloads (the site form the
// modes contract holds to "a disabled site evaluates no payload"). Contract: docs/design/runtime-diagnostics.md
// (DIAG.11b); ADR-0133 ID-5.

#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/diagnostic_host.hpp>
#include <crd/perf/flight_recorder.hpp>

#include <atomic>
#include <cstdio>

namespace diag_overhead
{

namespace cont = crd::containers;
namespace cp   = crd::perf;

enum class Mode : crd::u8
{
    Off,
    Basic,
    Full,
};

// "off", "basic", "full".
[[nodiscard]] const char* mode_name(Mode m) noexcept;

// The application event codes the workloads' sites record, above the foundation range.
inline constexpr cp::EventCode kCodeFirst      = 0x00A20000U;
inline constexpr cp::EventCode kCodeLast       = 0x00A2FFFFU;
inline constexpr cp::EventCode kCodeAllocBatch = 0x00A20001U; // a = operations so far, b = live bytes
inline constexpr cp::EventCode kCodeJobDone    = 0x00A20002U; // a = job, b = its sum
inline constexpr cp::EventCode kCodeProgramRan = 0x00A20003U; // a = iteration, b = the program's result
inline constexpr cp::EventCode kCodeEvent      = 0x00A20004U; // a = event, b = iteration

// What a workload's sites write to. `payloads` counts payload evaluations (a site's message is built only when the
// recorder admits the event), across every thread the workload runs on.
struct Sites
{
    cp::FlightRecorder*   recorder = nullptr; // null in the off mode
    std::atomic<crd::u64> payloads{0U};
};

struct WorkloadSetup
{
    Mode                mode    = Mode::Off;
    cp::DiagnosticHost* host    = nullptr; // started in basic and full; null in off
    Sites*              sites   = nullptr;
    crd::u32            threads = 8U;      // jobs workload: worker threads
    const char*         program = nullptr; // offline workload: the authored CEIR program
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
};

// The workloads by name ("alloc", "jobs", "offline", "events"); null for an unknown name. The object is static: one
// workload per process.
[[nodiscard]] Workload* find_workload(const char* name) noexcept;

// Open `path` for binary reading (fopen_s where the CRT deprecates fopen); null when it cannot be opened.
[[nodiscard]] std::FILE* open_for_reading(const char* path) noexcept;

} // namespace diag_overhead
