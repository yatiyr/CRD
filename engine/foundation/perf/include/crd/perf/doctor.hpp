#pragma once

// crd-perf -- the diagnostics doctor.
//
// Reports each diagnostic mode as compiled / enabled / usable with its
// dependencies, so a host (or a test) can ask "what diagnostics does this build
// actually have?" Most modes are honestly unqualified or unsupported per
// the capability census; the doctor states that rather than pretending a route is a
// command. Three dependencies are live-probed (sanitizer runtime, symbolizer,
// output path); the mode table is census-sourced. Two modes are reported live when the host hands the doctor its
// instances (DoctorLive): the DIAG.9b flight recorder (installed and armed, or ended on a fatal path) and fault
// injection (compiled only in builds with fault seams; installed, and whether a plan is armed).
//
// Contract: docs/design/runtime-diagnostics.md; ADR-0133.

#include <crd/containers/array.hpp>
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
    cont::StringView disposition;      // census-sourced note (not a live probe)
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
    cont::String                         host_tuple; // os/isa/compiler
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
    const FlightRecorder* flight_recorder = nullptr;
    const FaultInjector*  fault_injector  = nullptr;
};

// Run the doctor. `output_dir`, when non-empty, is probed for writability (the
// "output_path" dependency); empty leaves that dependency reported as not checked.
// The "flight-recorder" and "fault-injection" modes read `live`:
//   flight-recorder  compiled always; enabled when a recorder is installed and initialised; usable when enabled and
//                    no capture has ended on a fatal path (a fatal capture is terminal).
//   fault-injection  compiled when the build has fault seams (crd::fault_seams_compiled(): every build but shipping);
//                    enabled when a plan is armed; usable when compiled and an injector is installed (arming still
//                    needs the inject grant).
// "external-io-replay" is never compiled, enabled or usable, in any build: a run record holds no external I/O
// results, and a replay of a program that needs them is refused before it runs (DIAG.9b).
DoctorReport run_doctor(cont::StringView output_dir = cont::StringView{}, const DoctorLive& live = DoctorLive{});

} // namespace crd::perf
