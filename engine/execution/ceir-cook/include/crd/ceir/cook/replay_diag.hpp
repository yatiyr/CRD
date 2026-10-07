#pragma once

// crd-ceir-cook -- the replay diagnostic commands: prepare, record and run.
//
// `replay.prepare` (Read, a path) answers, for one authored program file or one run record under the diagnostic
// host's file root, what a replay of that program would need and which of those inputs exist. It prepares; it never
// runs the program and never claims a replay. A program file holds only its own content hash, so every other input it
// needs answers missing with the precise reason. A run record (crd/ceir/cook/replay_record.hpp) holds its build, entry
// arguments and the state of every input, so those answer from the record.
//
// One item per input, in a fixed order, each tagged with the replay guarantee it serves:
//   program           identity  always needed; available: the program's content hash (and a cooked file's recorded one)
//   build             identity  always needed; a program file names no build, a run record does
//   entry-arguments   event     always needed; a program file holds none, a run record does
//   random            event     RandomRead
//   clock             event     TimeRead
//   host-state        event     HostStateRead, SceneRead, EcsRead, PhysicsRead, AudioRead, DocumentRead,
//                               ConstraintRead, UIRead
//   external-results  event     FileIO, NetworkIO, DeviceIO, ExternalCall, AgentAction
//   schedule          schedule  Synchronization, Nondeterministic
//   device-tolerance  numeric   GPUCommand (a declared tolerance or oracle; never a bit-identity claim)
// The effect-derived needs come from each op's effective effects: a func.call's are its callee's, resolved through the
// module's symbol table, so an effect inside a callee is blamed on the call that reaches it. An input is needed when a
// registered op's effects say so (the item names the first such op in pre-order at its authored file:line:col); it is
// unknown when no op says so but an opaque op exists (an unregistered op, or one whose effects include ExternalCall:
// empty is not unknown), and an unknown need is reported missing; otherwise it is not needed.
//
// The summary also gives the weakest determinism class the program's registered ops claim and how many ops make no
// claim, so a reader sees which replay guarantee could be asked for at all.
//
// `replay.record` (Execute and Record, a program path) runs the program's entry once on the given arguments through
// the compiled-plan executor and writes a run record under the root: the program cooked (or kept, when already
// cooked) as an immutable blob, the build, the arguments, every input's need and state (an input the recorder cannot
// capture is stored missing), a bounded trace and the outcome. The record is created exclusively: an existing file is
// never overwritten. Arguments: `out` (the record's path under the root, required), `entry` (default main), `args`
// (comma-separated i64), `max_events` (1 to 65536, default 4096).
//
// `replay.run` (Execute, a record path) replays a record in this process, refusing an incompatible one before it runs
// anything: a file that is not a record, another record schema, a bad checksum, a program blob whose content hash is
// not the recorded one (Failed); a record from another build unless `build=any`, or one missing an input its program
// needs (Unavailable). It loads the record's own program, never the checkout's, runs the recorded entry on the recorded
// arguments with the recorded trace bound, and reports the first divergence or that the run reproduced. With
// `program=<path>` it replays the same inputs against that program instead (an edited checkout) and says the program
// differs. Every command loads programs with the host's dialects and bounds every file's size before reading it.
// Contract: docs/design/runtime-diagnostics.md.

#include <crd/ceir/cook/hot_reload.hpp> // Registrar
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

#include <atomic>

namespace crd::perf
{
class DiagCommandService;
} // namespace crd::perf

namespace crd::ceir::cook
{
inline constexpr containers::StringView kReplayPrepareCommand{"replay.prepare"};
inline constexpr containers::StringView kReplayRecordCommand{"replay.record"};
inline constexpr containers::StringView kReplayRunCommand{"replay.run"};

// The host's configuration of the command, and evidence of the work it did. The host owns it and keeps it alive at
// least as long as every service it is registered with.
struct ReplayPrepareCommand
{
    Registrar registrar = nullptr; // registers the host's dialects into each fresh Context (null: none)
    void*     user      = nullptr;
    crd::u64  max_program_bytes = 16ULL * 1024ULL * 1024ULL; // a larger file is refused Oversized, unread

    std::atomic<crd::u64> runs{0U};       // handler runs (requests that passed every service check)
    std::atomic<crd::u64> bytes_read{0U}; // program bytes read from files
};

// Register `replay.prepare` (Read, takes a path) with `service`. Returns false when the service refuses the
// registration (a duplicate name or a full table).
[[nodiscard]] bool register_replay_prepare(perf::DiagCommandService& service, ReplayPrepareCommand& command);

// The host's configuration of `replay.record` and `replay.run`, and evidence of their work. One configuration serves
// both commands; the host keeps it alive at least as long as every service it is registered with.
struct ReplayCommands
{
    Registrar registrar = nullptr; // registers the host's dialects into each fresh Context (null: none)
    void*     user      = nullptr;
    crd::u64  max_program_bytes = 16ULL * 1024ULL * 1024ULL; // a larger program is refused Oversized, unread
    crd::u64  max_record_bytes  = 64ULL * 1024ULL * 1024ULL; // a larger record is refused Oversized, unread

    std::atomic<crd::u64> record_runs{0U}; // replay.record handler runs
    std::atomic<crd::u64> replay_runs{0U}; // replay.run handler runs
    std::atomic<crd::u64> executions{0U};  // programs actually started (both commands)
    std::atomic<crd::u64> bytes_read{0U};  // program and record bytes read from files
    std::atomic<crd::u64> records_written{0U};
};

// Register `replay.record` (Execute, also Record; takes a path and arguments) with `service`.
[[nodiscard]] bool register_replay_record(perf::DiagCommandService& service, ReplayCommands& commands);

// Register `replay.run` (Execute; takes a path and arguments) with `service`.
[[nodiscard]] bool register_replay_run(perf::DiagCommandService& service, ReplayCommands& commands);
} // namespace crd::ceir::cook
