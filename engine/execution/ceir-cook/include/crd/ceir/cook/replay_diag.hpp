#pragma once

// crd-ceir-cook -- the replay-preparation diagnostic command.
//
// `replay.prepare` answers, for one authored program file under the diagnostic host's file root, what a replay of a
// run of that program would need and which of those inputs exist. It prepares; it never runs the program and never
// claims a replay. Nothing in this build records a run's inputs, so every input except the program's own content hash
// answers missing with the precise reason, and the summary says the replay is unavailable and names the missing
// inputs.
//
// One item per input, in a fixed order, each tagged with the replay guarantee it serves:
//   program           identity  always needed; available: the program's content hash (and a cooked file's recorded one)
//   build             identity  always needed; a program file names no build, and nothing records the build a run used
//   entry-arguments   event     always needed; nothing records the arguments and initial state a run was given
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
// The program is loaded exactly as `program.provenance` loads it (cooked, binary or text under the request's relative
// path, into a fresh Context with the host's dialects), the file's size is bounded before a byte is read, and the
// command needs Read. Contract: docs/design/runtime-diagnostics.md.

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
} // namespace crd::ceir::cook
