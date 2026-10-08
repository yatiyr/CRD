#pragma once

// crd-ceir-cook -- the program-inspection diagnostic command.
//
// `program.inspect` runs one authored CEIR text program under the diagnostic host's file root under a runtime
// inspection session, to a script given as the request's named arguments, and answers what the run showed: how each
// breakpoint bound, every stop's authored position and depth with each watched line's typed value, the scripted
// action at each stop, and how the run ended. It is registered into a crd-perf DiagCommandService, so a native caller,
// the CLI verb and the MCP tool get the same bounded, paginated answer under the same checks.
//
// Authority: Execute. Running an authored program is not a snapshot of evidence that already exists (Read), so a host
// grants it separately; Read never implies it. The program is cooked under the request's relative path, so breakpoints
// and stops are positions in that name and the host's root never appears in an answer.
//
// Arguments (all optional; lists are comma-separated, an empty value is an empty list):
//   entry      the function to run (default "main"; [A-Za-z0-9_.], at most 64 bytes)
//   args       the entry's i64 arguments
//   breaks     1-based authored lines to stop at
//   watches    1-based authored lines whose first result is read at every stop
//   steps      the action at each stop in order: continue, into, over, out, cancel (then continue)
//   max_stops  stops to record before the run is cancelled and the answer says truncated (1 to the host's limit)
//   seed       a u64: the run's host random streams (input.random reads `SeededInputs` of it); without it the host
//              has no random source and a draw fails input-unavailable (DIAG.9a)
// They are checked before the file is opened; a malformed argument is refused bad-argument with nothing read.
//
// The run happens inside the request, on the calling thread as the controller, so the service is held for its length;
// every wait is bounded by the host's `wait_ms` and the run by `max_stops`. The caller's cancel flag is polled while
// the program runs and at every stop: a raised flag cancels the run at its next safe point and the request is answered
// cancelled. Contract: docs/design/runtime-diagnostics.md.

#include <crd/ceir/cook/hot_reload.hpp>    // Registrar
#include <crd/ceir/cook/inspect_script.hpp> // ScriptStopFn, kScriptDefaultWaitMs
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

#include <atomic>

namespace crd::perf
{
class DiagCommandService;
} // namespace crd::perf

namespace crd::ceir::cook
{
inline constexpr containers::StringView kProgramInspectCommand{"program.inspect"};

inline constexpr crd::u32 kInspectMaxEntryBytes = 64U;

// The host's configuration of the command, and evidence of the work it did. The host owns it and keeps it alive at
// least as long as every service it is registered with.
struct ProgramInspectCommand
{
    Registrar    registrar         = nullptr; // registers the host's dialects into each program's Context (null: none)
    void*        user              = nullptr;
    crd::u64     max_program_bytes = 16ULL * 1024ULL * 1024ULL; // a larger file is refused Oversized, unread
    crd::u32     max_stops_limit   = 256U;                       // the largest max_stops a request may ask for
    crd::u32     wait_ms           = kScriptDefaultWaitMs;       // the bound on every wait inside a run
    ScriptStopFn on_stop           = nullptr; // the host's observer of every recorded stop (e.g. a progress line)
    void*        stop_user         = nullptr;

    std::atomic<crd::u64> runs{0U};       // handler runs (requests that passed every service check)
    std::atomic<crd::u64> bytes_read{0U}; // program bytes read from files
    std::atomic<crd::u64> starts{0U};     // programs that cooked and started
};

// Register `program.inspect` (Execute, takes a path and named arguments) with `service`. Returns false when the
// service refuses the registration (a duplicate name or a full table).
[[nodiscard]] bool register_program_inspect(perf::DiagCommandService& service, ProgramInspectCommand& command);
} // namespace crd::ceir::cook
