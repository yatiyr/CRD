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
// which is what the record then holds), `clock` (`wall`: input.clock {domain = "wall"} reads the host's monotonic
// clock live, in nanoseconds since the run's clock was made; without it the wall has no reading), `sim_time` and
// `sim_step` (i64 nanoseconds: the sim domain's reading and current step; without them the sim domain has none). The
// other time domains have no reading or step on this one-shot host. `events` (at most 32 comma-separated events, see
// parse_events_argument) opens the host's input event queue 0 holding those events in order, so input.event
// {queue = 0} takes them and then reads none; without it the host has no event queue and a read fails
// input-unavailable. Every host input read is kept in the record (`random`, `clock` and `host-state` recorded, the
// last when no op reads host state outside the seam); a replay feeds those reads back and never reads a live source,
// so it needs no seed, no clock and no events.
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
#include <crd/ceir/input.hpp> // HostClock
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp> // RunInputs

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

// The clock a one-shot host gives a recorded run: replay.record's `clock`, `sim_time` and `sim_step`.
struct HostClockSpec
{
    bool     live_wall    = false; // the wall domain reads the host's monotonic clock live
    bool     has_sim_time = false;
    crd::i64 sim_time     = 0;     // the sim domain's reading, in nanoseconds
    bool     has_sim_step = false;
    crd::i64 sim_step     = 0;     // the sim domain's current step, in nanoseconds
};

// The built-in time domains a host sets by ordinal (time::builtin_domain_index; the seam channel).
inline constexpr crd::u32 kWallDomain  = 0U;
inline constexpr crd::u32 kSimDomain   = 1U;
inline constexpr crd::u32 kFrameDomain = 2U;

// Nanoseconds on crd-time's monotonic clock (an input::HostClock::MonotonicReader; `user` is unused).
[[nodiscard]] crd::i64 monotonic_ns(void* user) noexcept;

// Set `clock` to what `spec` says, from no reading at all: a live wall's epoch is this call.
void apply_clock(const HostClockSpec& spec, input::HostClock& clock) noexcept;

// How one clock argument parsed (see parse_clock_argument).
// NOLINTNEXTLINE(performance-enum-size)
enum class ClockArgument : crd::u8
{
    NotClock = 0, // the name is none of the clock arguments
    Ok,           // a clock argument with a valid value
    Refused,      // a clock argument with a malformed value (`requirement` says what it must be)
};

// The clock arguments every one-shot host shares: `clock` (only `wall`: the live wall), `sim_time` and `sim_step`
// (decimal i64 nanoseconds: the sim domain's reading and current step). replay.record and program.inspect take them
// as named arguments, ceridc inspect as --clock, --sim-time and --sim-step, crd-sandbox as --inspect-clock,
// --inspect-sim-time and --inspect-sim-step. An Ok value is set in `spec` when it is not null.
[[nodiscard]] ClockArgument parse_clock_argument(containers::StringView name, containers::StringView value,
                                                 HostClockSpec* spec, containers::StringView& requirement) noexcept;

// The input event queue a one-shot host gives a recorded run: replay.record's `events`. Every event goes to queue
// kEventQueue.
inline constexpr crd::u32 kEventQueue        = 0U;
inline constexpr crd::u32 kMaxArgumentEvents = 32U;
struct HostEventsSpec
{
    bool                            open = false; // the queue exists (an `events` argument was given, even empty)
    containers::ConstSpan<crd::i64> events;       // packed (input::pack_event), in delivery order
};

// Set `events` to what `spec` says, from no queue at all: none, or queue kEventQueue holding spec's events.
void apply_events(const HostEventsSpec& spec, input::HostEvents& events);

// `events`, the events a one-shot host queues: at most kMaxArgumentEvents comma-separated events, each one of
//   key_down:<key>[:<mods>]   key_up:<key>[:<mods>]   key_repeat:<key>[:<mods>]
//   mouse_down:<button>[:<mods>]   mouse_up:<button>[:<mods>]
//   mouse_move:<x>:<y>   scroll:<x>:<y>   resize:<x>:<y>
// with <key> and <button> in [0, 65535], <mods> in [0, 15] (shift 1, ctrl 2, alt 4, super 8) and <x>, <y> in
// [-32768, 32767] (decimal). An empty value is an open queue with no event. Each event is appended to `out` packed
// (when `out` is not null). False, with `requirement` saying what the value must be, when it is malformed.
[[nodiscard]] bool parse_events_argument(containers::StringView value, containers::Array<crd::i64>* out,
                                         containers::StringView& requirement);

// The host inputs of one run on a one-shot or interactive host: input.random reads the input::SeededInputs of a seed
// (no random source without one), input.clock / input.time_step read a HostClock set from a HostClockSpec (a domain
// the spec leaves unset has no value) and input.event reads the input::HostEvents a HostEventsSpec describes (no queue
// without one). The seeded source and the event queues grow on their own allocator, because a run reads them on its
// executing thread while the controller allocates from the caller's (the inspect host's threading rule). A run's host
// keeps this object alive until the run has ended, so declare it before the host; change it only between runs.
class RunInputs
{
public:
    explicit RunInputs(const char* name);
    RunInputs(const RunInputs&)            = delete;
    RunInputs& operator=(const RunInputs&) = delete;
    RunInputs(RunInputs&&)                 = delete;
    RunInputs& operator=(RunInputs&&)      = delete;
    ~RunInputs()                           = default;

    // The next run's inputs: seeded (`seeded`, from draw 0 of every stream of `seed`) or no random source, the clock
    // `clock` describes (a live wall's epoch is this call) and the event queue `events` describes (none unless it is
    // open). Every call starts over: the queues an earlier run read are gone.
    void set(bool seeded, crd::u64 seed, const HostClockSpec& clock, const HostEventsSpec& events = {});

    // The clock, for a host that sets more than a HostClockSpec does (the sandbox's frame domain). Between runs only.
    [[nodiscard]] input::HostClock& clock() noexcept { return m_clock; }
    // The event queues, for a host that queues more than a HostEventsSpec does (the sandbox's window events). Between
    // runs only.
    [[nodiscard]] input::HostEvents& events() noexcept { return m_events; }

    // The source to install for the run; it points at this object.
    [[nodiscard]] const input::InputSource* source() const noexcept { return m_router.source(); }

private:
    memory::GrowableTlsfAllocator m_alloc;
    input::SeededInputs           m_seeded;
    input::HostClock              m_clock;
    input::HostEvents             m_events;
    input::InputRouter            m_router;
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
    HostClockSpec                   clock;              // the run's time domains
    HostEventsSpec                  events;             // the run's input event queue
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
