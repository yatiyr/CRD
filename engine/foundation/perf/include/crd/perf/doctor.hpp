#pragma once

// crd-perf -- the diagnostics doctor (DIAG.0).
//
// Reports each diagnostic mode as compiled / enabled / usable with its
// dependencies, so a host (or a test) can ask "what diagnostics does this build
// actually have?" At DIAG.0 most modes are honestly unqualified or unsupported per
// the capability census; the doctor states that rather than pretending a route is a
// command. Three dependencies are live-probed (sanitizer runtime, symbolizer,
// output path); the mode table is census-sourced. No registration hook, bundle or
// recorder yet -- those are later DIAG leaves.
//
// Contract: docs/design/runtime-diagnostics.md; ADR-0133; owner ROADMAP DIAG.0.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

namespace crd::perf
{
namespace cont = crd::containers;

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

// Run the doctor. `output_dir`, when non-empty, is probed for writability (the
// "output_path" dependency); empty leaves that dependency reported as not checked.
DoctorReport run_doctor(cont::StringView output_dir = cont::StringView{});

} // namespace crd::perf
