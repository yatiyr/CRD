#pragma once

// crd-perf -- the diagnostics doctor.
//
// Reports each diagnostic mode as compiled / enabled / usable with its
// dependencies, so a host (or a test) can ask "what diagnostics does this build
// actually have?" Each row is read from this build or this host, never assumed: the build identity (platform,
// arch, compiler version, configuration and its sanitizer) names the tuple and the instrument modes, a mode that needs
// another build says which switch builds it, and a platform with no backend reports the mode absent and names its
// later-platform owner (MAC.DIAG, WEB.DIAG). Live modes read the host's instances (DoctorLive): the DIAG.9b flight
// recorder and fault injection, and the upper-module command names registered with its service (controlled replay and
// authored provenance, served by crd-ceir-cook). Three dependencies are live-probed (sanitizer runtime, symbolizer,
// output path). GPU validation and device faults belong to the GPU device the host creates, not to this report.
//
// Contract: docs/design/runtime-diagnostics.md; ADR-0133.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

namespace crd::perf
{
namespace cont = crd::containers;

class FlightRecorder;
class FaultInjector;

// One diagnostic mode's standing in this build.
struct DiagnosticMode
{
    cont::StringView name;
    bool             compiled = false; // the code exists in this build
    bool             enabled = false;  // active in this build configuration
    bool             usable = false;   // dependencies present, so it could actually run
    cont::StringView disposition;      // what the mode is here, or what provides it when absent
};

// One live-probed dependency.
struct DiagnosticDependency
{
    cont::StringView name;
    bool             present = false;
    cont::String     detail;
};

// The doctor's report. `schema` is the versioned diagnostics schema name.
struct DoctorReport
{
    cont::StringView                     schema;
    cont::String                         host_tuple; // platform/arch/compiler version/configuration
    cont::Array<DiagnosticMode>          modes;
    cont::Array<DiagnosticDependency>    dependencies;

    // Convenience lookups (nullptr when absent).
    const DiagnosticMode*       find_mode(cont::StringView name) const noexcept;
    const DiagnosticDependency* find_dependency(cont::StringView name) const noexcept;
};

// The host's live diagnostic instances, when it has them (nullptr: not installed by this host). They must outlive the
// run_doctor call.
struct DoctorLive
{
    const FlightRecorder*             flight_recorder = nullptr;
    const FaultInjector*              fault_injector  = nullptr;
    cont::ConstSpan<cont::StringView> commands; // the command names its service has registered
};

// Run the doctor. `output_dir`, when non-empty, is probed for writability (the
// "output_path" dependency); empty leaves that dependency reported as not checked.
// The instrument modes follow this binary's sanitizer: "memory-check" (AddressSanitizer) and "race-check"
// (ThreadSanitizer); each needs its own build, and an absent one names the CMake switch that builds it.
// "crash-capture" is compiled where crd::crash has a backend (Windows, Linux; diagnostic_host_capabilities()).
// The modes that read `live`:
//   flight-recorder     compiled always; enabled when a recorder is installed and initialised; usable when enabled
//                       and no capture has ended on a fatal path (a fatal capture is terminal).
//   fault-injection     compiled when the build has fault seams (crd::fault_seams_compiled(): every build but
//                       shipping); enabled when a plan is armed; usable when compiled and an injector is installed
//                       (arming still needs the inject grant).
//   controlled-replay   served when `live.commands` has replay.record and replay.run (DIAG.9a).
//   authored-provenance served when `live.commands` has program.provenance (DIAG.8a).
// "external-io-replay" is never compiled, enabled or usable, in any build: a run record holds no external I/O
// results, and a replay of a program that needs them is refused before it runs (DIAG.9b).
DoctorReport run_doctor(cont::StringView output_dir = cont::StringView{}, const DoctorLive& live = DoctorLive{});

} // namespace crd::perf
