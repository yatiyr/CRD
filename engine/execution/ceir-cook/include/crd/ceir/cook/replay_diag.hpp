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
// (comma-separated i64), `max_events` (1 to 65536, default 4096), `seed` (a u64: the host random streams the run
// draws from, input::SeededInputs; without it the host has no random source and a draw fails input-unavailable,
// which is what the record then holds). Every host input read is kept in the record (`random` recorded); a replay
// feeds those reads back and never draws from a live source, so it needs no seed.
//
// `replay.run` (Execute, a record path) replays a record in this process, refusing an incompatible one before it runs
// anything: a file that is not a record, another record schema, a bad checksum, a program blob whose content hash is
// not the recorded one (Failed); a record from another build unless `build=any`, or one missing an input its program
// needs (Unavailable). It loads the record's own program, never the checkout's, runs the recorded entry on the recorded
// arguments with the recorded trace bound, and reports the first divergence or that the run reproduced. With
// `program=<path>` it replays the same inputs against that program instead (an edited checkout) and says the program
// differs. Every command loads programs with the host's dialects and bounds every file's size before reading it.
//
// Host records. `replay.record executor=host` runs the program once on the crd-jobs host provider instead (with
// `jobs`, its job split, 1 to 256, default 8, and `sub_fuel`, its step budget per body, default 2^20) and writes a
// host record; `replay.run` replays a host record on the same executor (`jobs=` replays it on another job split). This
// module stays free of crd-jobs, so it cannot run the host provider itself: crd-ceir-host provides the executor
// (crd/ceir/host/host_replay_diag.hpp) and the host binds it on its ReplayCommands. With none bound, `executor=host`
// and a host record are refused Unavailable before anything is read or run. The process owns the crd::jobs pool: it
// must be initialised before a request reaches the host executor.
// Contract: docs/design/runtime-diagnostics.md.

#include <crd/ceir/cook/hot_reload.hpp> // Registrar
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

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

// How the host executor answered a record or replay request.
// NOLINTNEXTLINE(performance-enum-size)
enum class HostExecutorStatus : crd::u8
{
    Ok = 0,
    Unavailable, // an incompatible replay: a plan record, another build, a missing input
    Failed,      // a program did not load, or the record's program is not the content it was recorded with
    BadArgument, // a job split or step budget out of range
};

// One run of a cooked program on the host executor, to be recorded.
struct HostRecordRequest
{
    containers::ConstSpan<crd::u8>  blob;  // the program's immutable artifact
    containers::StringView          path;  // the authored path it was cooked under
    containers::StringView          entry;
    containers::ConstSpan<crd::i64> args;
    crd::u32                        num_jobs   = 8U;
    crd::u64                        sub_fuel   = crd::u64{1} << 20U;
    crd::u32                        max_events = kReplayDefaultMaxEvents;
    Registrar                       registrar  = nullptr;
    void*                           user       = nullptr;
    bool                            has_seed   = false; // false: the run has no host random source
    crd::u64                        seed       = 0U;    // the input::SeededInputs seed when `has_seed`
};

// One replay of a host record.
struct HostReplayRequest
{
    const ReplayRecord*            record = nullptr;
    containers::ConstSpan<crd::u8> against;            // empty: the record's own program; else this cooked program
    bool                           any_build = false;  // replay a record of another build anyway
    crd::u32                       num_jobs  = 0U;     // 0: the recorded job split; else this one
    Registrar                      registrar = nullptr;
    void*                          user      = nullptr;
};

// A host replay's answer. Every position is owned: the Contexts it was read from are gone when the call returns.
struct HostReplayAnswer
{
    explicit HostReplayAnswer(memory::IAllocator* a) : trace(a), site(a), fault(a), recorded_fault(a) {}

    ReplayTrace     trace;          // the replayed run
    Divergence      divergence;     // kind None: the replay reproduced the record
    OwnedReplaySite site;           // the divergence's op in the replayed program (op 0 when none names an op)
    OwnedReplaySite fault;          // the replayed run's faulting op
    OwnedReplaySite recorded_fault; // the recorded faulting op, in the record's own program
    crd::u64        replayed_hash = 0U;
    crd::u32        num_jobs      = 0U; // the job split the replay ran with
};

// The host executor (crd-ceir-host provides one). Both functions run on the calling thread and need the process's
// crd::jobs pool. A non-Ok status leaves `reason` saying why and runs nothing when it is a refusal.
struct ReplayHostExecutor
{
    // Run the request's program once and make its host record in `out` (whatever the run's outcome). `fault` gets
    // the faulting op's authored position; `missing` gains the inputs stored missing, comma-separated.
    HostExecutorStatus (*record)(const HostRecordRequest& request, ReplayRecord& out, OwnedReplaySite& fault,
                                 containers::String& missing, containers::String& reason) = nullptr;
    // Replay a host record, refusing an incompatible one before anything runs.
    HostExecutorStatus (*replay)(const HostReplayRequest& request, HostReplayAnswer& out,
                                 containers::String& reason) = nullptr;
};

// The host's configuration of `replay.record` and `replay.run`, and evidence of their work. One configuration serves
// both commands; the host keeps it alive at least as long as every service it is registered with.
struct ReplayCommands
{
    Registrar registrar = nullptr; // registers the host's dialects into each fresh Context (null: none)
    void*     user      = nullptr;
    crd::u64  max_program_bytes = 16ULL * 1024ULL * 1024ULL; // a larger program is refused Oversized, unread
    crd::u64  max_record_bytes  = 64ULL * 1024ULL * 1024ULL; // a larger record is refused Oversized, unread
    const ReplayHostExecutor* host = nullptr; // the host executor (null: host records are refused Unavailable)

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
