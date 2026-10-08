#pragma once

// crd-ceir-cook -- run records: the reproducible inputs of one run of an authored program, and its replay.
//
// A record is an immutable, versioned, checksummed file. It holds everything a later process needs to run the same
// program on the same inputs without the checkout: the build and configuration that recorded it, the program as its
// cooked CRDR blob (the artifact itself, with its content hash and authored positions, never a path to re-read), the
// entry and its arguments, each replay input's need and state, a bounded trace of the run and its outcome. An input
// the recorder cannot capture is stored as missing, never assumed; a replay refuses a record that lacks an input its
// program needs.
//
// The trace is taken at the compiled-plan executor's safe points (plan::RunControl): one event per dispatched instr,
// in order, naming the instr's op stable id and call depth, with the instr's results read at the next safe point of
// the same frame (plan::read_value). An instr whose results cannot be read there (the last instr of a loop body
// before its back edge, or the last before a return) carries no values: that is recorded, and compared, as is. Only
// the first `max_events` events are kept; `events_total` counts every event, so a truncated trace says how much it
// lost. The outcome is the run error, the op it blamed, the results and the state cells after the run.
//
// A record also names where its program came from: the asset id and the generation number a hot-reloading host had
// installed when the run started (`InspectHost` records them; 0 and 0 when the program was not run by such a host).
// A run on an InspectHost cannot span a reload (a load is refused while an execution is attached), so one record
// always names one generation, and its blob is that generation's program even after the host has reloaded.
//
// Replay loads the record's own blob into a fresh Context, compiles the record's entry, runs it on the recorded
// arguments with the same trace bound and reports the first divergence: the first event whose op or depth differs
// (path), the first differing result value (value), a different event count (length), outcome, results or cells.
// A replay against another program (an edited checkout) is a separate, explicit request; it uses the same inputs.
//
// A record names the executor that ran it. A plan record is the compiled-plan executor's (above). A host record is
// the crd-jobs host provider's (crd-ceir-host): its trace is the submitting interpreter's, one event per dispatched
// op with up to four of its results read right after it ran (InterpreterRecorder), so the two traces are never
// compared with each other. Bodies the provider runs on its own sub-interpreters (parallel ranges, fold steps, pooled
// launch bodies) are not traced; what they produce is seen where the submitting thread reads it (the await, join,
// parallel or reduction op's results). A host record also holds the provider's schedule settings (its job split and
// per-body step budget); the provider's other schedule choices are fixed by the build (a race answers its first
// operand, the lowest failing index wins, folds run in index order).
//
// Host inputs (input.hpp: input.random's draws) are read through the run's input seam. A recorded run's reads go
// through an `InputRecorder` wrapped around the host's live source, which keeps every read in order: its kind,
// channel, whether the host had a value and the value. A replay installs an `InputFeed` over the record's reads
// instead and never asks a live host; a read the record cannot answer (another kind or channel at that position, or
// past its last read) stops the replay there with InputUnavailable and is reported as an `input` divergence at that
// op. The record keeps at most kReplayMaxInputReads reads and counts all of them; a run that read more holds an
// incomplete stream, so its `random` input is stored missing and a replay refuses it.
//
// Guarantee: a plan record is event replay of the integer, sequential compiled-plan executor; a program needing
// schedule choices records the schedule as missing there. A host record is schedule replay of the host provider.
// Backend-specific numeric replay is a different guarantee. Contract: docs/design/runtime-diagnostics.md (DIAG.9a).

#include <crd/ceir/cook/hot_reload.hpp> // Registrar
#include <crd/ceir/cook/program_cook.hpp> // ReadError
#include <crd/ceir/exec.hpp>
#include <crd/ceir/input.hpp> // InputSource, InputKind
#include <crd/ceir/plan.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <atomic>

namespace crd::ceir
{
class Context;
} // namespace crd::ceir

namespace crd::ceir::cook
{
inline constexpr crd::u32 kReplayRecordSchema = 4U; // the record file layout (4: the host input reads)
inline constexpr crd::u32 kReplayExecutor     = 1U; // the executor semantics a trace is valid for

inline constexpr crd::u32 kReplayDefaultMaxEvents = 4096U;
inline constexpr crd::u32 kReplayMaxEvents        = 65536U;
inline constexpr crd::u32 kReplayEventValues      = 4U;  // results kept per event
inline constexpr crd::u32 kReplayMaxArgs          = 64U;
inline constexpr crd::u32 kReplayMaxStringBytes   = 256U;
inline constexpr crd::u32 kReplayMaxValues        = 65536U; // results, and state cells, in an outcome
inline constexpr crd::u64 kReplayMaxProgramBytes  = 16ULL * 1024ULL * 1024ULL;
inline constexpr crd::u32 kReplayInputs           = 9U;
inline constexpr crd::u32 kReplayMaxHostJobs      = 256U; // a host record's job split
inline constexpr crd::u32 kReplayMaxInputReads    = 65536U; // host input reads kept in a record

// Which executor ran a recorded run. A record replays only on its own executor.
// NOLINTNEXTLINE(performance-enum-size)
enum class ReplayExecutorKind : crd::u8
{
    Plan = 0, // the compiled-plan executor (crd-ceir-cook replays it)
    Host = 1, // the crd-jobs host provider's interpreter (crd-ceir-host replays it)
};

// "plan", "host".
[[nodiscard]] containers::StringView replay_executor_name(ReplayExecutorKind k) noexcept;

// The build and configuration a record was made by. Every field must match for a replay unless the request
// explicitly allows another build.
struct ReplayBuild
{
    explicit ReplayBuild(memory::IAllocator* alloc)
        : version(alloc), platform(alloc), compiler(alloc), arch(alloc), config(alloc)
    {
    }

    containers::String version;  // CRD_VERSION_STRING
    containers::String platform; // crd::platform_name()
    containers::String compiler; // crd::compiler_name()
    containers::String arch;     // crd::arch_name()
    containers::String config;   // "debug" or "release"
    bool               asserts  = false;
    crd::u32           executor = kReplayExecutor;
};

// This process's build.
[[nodiscard]] ReplayBuild current_build(memory::IAllocator* alloc);

// True when every field of `a` equals `b`'s; otherwise false, with the differing field names appended to `differing`
// comma-separated (version, platform, compiler, arch, config, asserts, executor).
[[nodiscard]] bool same_build(const ReplayBuild& a, const ReplayBuild& b, containers::String& differing);

// NOLINTNEXTLINE(performance-enum-size)
enum class ReplayNeed : crd::u8
{
    No      = 0,
    Yes     = 1,
    Unknown = 2, // no registered op needs it, but an opaque op might
};

// NOLINTNEXTLINE(performance-enum-size)
enum class ReplayInputState : crd::u8
{
    NotNeeded = 0,
    Recorded  = 1,
    Missing   = 2, // needed (or possibly needed) and not in the record
};

// "not-needed", "recorded", "missing".
[[nodiscard]] containers::StringView replay_input_state_name(ReplayInputState s) noexcept;

// The input names in record order: program, build, entry-arguments, random, clock, host-state, external-results,
// schedule, device-tolerance.
[[nodiscard]] containers::StringView replay_input_name(crd::u32 index) noexcept;

struct ReplayInput
{
    ReplayNeed       need  = ReplayNeed::Yes;
    ReplayInputState state = ReplayInputState::Missing;
};

// One dispatched instr.
struct ReplayEvent
{
    crd::u64 op     = 0U; // the instr's op stable id
    crd::u32 depth  = 0U; // the call depth at its safe point
    crd::u32 values = 0U; // results read (0..kReplayEventValues)
    crd::i64 value[kReplayEventValues]{};
};

[[nodiscard]] bool operator==(const ReplayEvent& a, const ReplayEvent& b) noexcept;

// One read of a host input, in the order the run made them.
struct ReplayInputRead
{
    input::InputKind kind      = input::InputKind::Random;
    crd::u32         channel   = 0U;    // the stream (Random)
    bool             delivered = false; // false: the host had no value, and the read failed InputUnavailable
    crd::i64         value     = 0;     // the raw value the host delivered (0 when not delivered)
};

[[nodiscard]] bool operator==(const ReplayInputRead& a, const ReplayInputRead& b) noexcept;

// A replay's first host input read the record could not answer.
struct ReplayInputRefusal
{
    bool             refused = false;
    crd::u64         read    = 0U; // its position in the read order
    crd::u64         event   = 0U; // the trace event of the op that asked (its index in the trace)
    input::InputKind kind    = input::InputKind::Random;
    crd::u32         channel = 0U;
};

// What one traced run produced. `sites` is parallel to `events` (the instr of each kept event), for blame; it is not
// stored in a record.
struct ReplayTrace
{
    explicit ReplayTrace(memory::IAllocator* alloc)
        : events(alloc), sites(alloc), results(alloc), cells(alloc), input_reads(alloc)
    {
    }

    containers::Array<ReplayEvent>    events;
    containers::Array<plan::InstrRef> sites;
    crd::u64                          events_total = 0U;
    plan::RunError                    error        = plan::RunError::None;
    plan::InstrRef                    fault;
    crd::u64                          fault_op = 0U; // the faulting instr's op stable id (0 when none)
    exec::ExecError                   host_error = exec::ExecError::None; // a host trace's run error
    containers::Array<crd::i64>       results;
    containers::Array<crd::i64>       cells;
    // The host input reads the run made (the first kReplayMaxInputReads), all of them counted, and for a replay the
    // first read its record could not answer.
    containers::Array<ReplayInputRead> input_reads;
    crd::u64                           input_reads_total = 0U;
    ReplayInputRefusal                 input_refusal;
};

struct ReplayRecord
{
    explicit ReplayRecord(memory::IAllocator* alloc)
        : build(alloc), program_path(alloc), program(alloc), entry(alloc), args(alloc), events(alloc), results(alloc),
          cells(alloc), input_reads(alloc)
    {
    }

    crd::u32                       schema = kReplayRecordSchema;
    ReplayBuild                    build;
    ReplayExecutorKind             executor = ReplayExecutorKind::Plan;
    crd::u32                       host_jobs     = 0U; // Host: the provider's job split (1..kReplayMaxHostJobs)
    crd::u64                       host_sub_fuel = 0U; // Host: the provider's step budget per body (at least 1)
    containers::String             program_path; // the authored path the program was cooked under
    crd::u64                       content_hash = 0U; // the cooked program's content hash
    crd::u64                       asset        = 0U; // the host's asset id of the program (0: no reloading host)
    crd::u64                       generation   = 0U; // the generation that ran (0: no reloading host)
    containers::Array<crd::u8>     program;           // the cooked CRDR blob
    containers::String             entry;
    containers::Array<crd::i64>    args;
    ReplayInput                    inputs[kReplayInputs];
    crd::u32                       max_events   = kReplayDefaultMaxEvents;
    crd::u64                       events_total = 0U;
    containers::Array<ReplayEvent> events; // the first min(events_total, max_events) events
    plan::RunError                 error      = plan::RunError::None;     // Plan: the run error (None for Host)
    exec::ExecError                host_error = exec::ExecError::None;    // Host: the run error (None for Plan)
    crd::u64                       fault_op   = 0U;
    containers::Array<crd::i64>    results;
    containers::Array<crd::i64>    cells;
    crd::u64                           input_reads_total = 0U; // every host input read the run made
    containers::Array<ReplayInputRead> input_reads;            // the first min(input_reads_total, kReplayMaxInputReads)
};

// Why a record did not decode.
// NOLINTNEXTLINE(performance-enum-size)
enum class RecordError : crd::u8
{
    Ok = 0,
    NotARecord,        // the leading bytes are not a run record's
    UnsupportedSchema, // a record layout this build does not read
    Truncated,         // fewer bytes than the header or a field says
    BadChecksum,       // the payload does not match its checksum
    Malformed,         // a field is out of its bounds or range, a field of the other executor is set, or bytes follow
                       // the last field
};

// "ok", "not-a-record", "unsupported-schema", "truncated", "bad-checksum", "malformed".
[[nodiscard]] containers::StringView record_error_name(RecordError e) noexcept;

// Whether `bytes` start with a run record's magic.
[[nodiscard]] bool is_replay_record(containers::ConstSpan<crd::u8> bytes) noexcept;

// Encode `record` into `out` (cleared first). Deterministic: one record always gives the same bytes.
void encode_record(const ReplayRecord& record, containers::Array<crd::u8>& out);

// Decode `bytes` into `out`. Every count and size is bounded before it is used; nothing is executed.
[[nodiscard]] RecordError decode_record(containers::ConstSpan<crd::u8> bytes, ReplayRecord& out);

// NOLINTNEXTLINE(performance-enum-size)
enum class RecordWrite : crd::u8
{
    Ok = 0,
    Exists, // a file is already at the path: refused, and left untouched
    Failed, // the file could not be created or fully written (a partial file is removed)
};
// "ok", "exists", "failed".
[[nodiscard]] containers::StringView record_write_name(RecordWrite w) noexcept;

// Encode `record` into a new file at `path`. The create is exclusive: a record is never overwritten.
[[nodiscard]] RecordWrite write_record_file(containers::StringView path, const ReplayRecord& record);

// A program loaded from a cooked blob into a caller's fresh Context, with one entry compiled. The Context must outlive
// it (the module and the plan's positions name it).
struct ReplayProgram
{
    explicit ReplayProgram(memory::IAllocator* alloc) : compiled(alloc) {}

    Module*             module       = nullptr;
    ReadError           read         = ReadError::Ok;
    crd::u64            content_hash = 0U; // as the blob's header records it
    plan::CompileResult compiled;
    [[nodiscard]] bool  ok() const noexcept { return module != nullptr && read == ReadError::Ok && compiled.ok(); }
};

// Register the host's dialects into `ctx` (fresh), read `blob`, assign stable ids and compile `entry`.
void load_replay_program(Context& ctx, containers::ConstSpan<crd::u8> blob, containers::StringView entry,
                         Registrar registrar, void* user, ReplayProgram& out);

// The trace of one run, taken at its safe points. `run_traced` uses it with the plan executor directly; a host that
// runs the plan under an `inspect::Session` passes `control()` to the session as its observer, so a debugger's stops,
// steps and value reads leave the trace exactly as an unobserved run's.
//
// Threads: construct it, run, and call `finish` on the EXECUTING thread; `out`'s arrays allocate from their own
// allocator there. Another thread may read `out` only after the run has ended and that thread is joined.
class ReplayRecorder
{
public:
    // Clear `out` and keep at most `max_events` events (capped at kReplayMaxEvents).
    ReplayRecorder(ReplayTrace& out, crd::u32 max_events);

    // The safe-point control that appends the events (`cancel` is passed through; a session ignores it).
    [[nodiscard]] plan::RunControl control(const std::atomic<bool>* cancel = nullptr) noexcept;

    // Record the run's outcome: its error, the faulting instr and op, its results and state cells.
    void finish(const plan::CompiledPlan& plan, const plan::RunResult& result);

private:
    static plan::SafePointAction on_safe_point(const plan::CompiledPlan& plan, const plan::SafePoint& at, void* user);

    ReplayTrace*                m_out;
    crd::u32                    m_max;
    containers::Array<crd::u64> m_pending; // per call depth: the kept event whose results are read next
};

// The trace of one run on a reference interpreter (the host provider's submitting interpreter), for a host record.
// `attach` installs its step hooks on the interpreter: the pre hook appends an event (the op's stable id and the call
// depth), the post hook, which runs only after a successful dispatch, reads up to kReplayEventValues of the op's
// results. `detach` removes the hooks and keeps the interpreter's state cells (their current values, in stable id
// order), so call it while the interpreter is still alive. Hooks are not copied to the provider's sub-interpreters,
// so the bodies they run leave no events.
//
// Threads: attach, the run, detach and `finish` on the thread that runs the interpreter.
class InterpreterRecorder
{
public:
    // Clear `out` and keep at most `max_events` events (capped at kReplayMaxEvents).
    InterpreterRecorder(ReplayTrace& out, crd::u32 max_events);

    void attach(exec::Interpreter& in) noexcept;
    void detach(exec::Interpreter& in);

    // Record the run's outcome: its error, the op it blamed and its results.
    void finish(const exec::ExecResult& result);

private:
    static void on_pre(const Operation& op, void* user);
    static void on_post(const Operation& op, void* user);

    struct Open
    {
        const Operation* op    = nullptr;
        crd::u64         event = 0U; // the kept event (kReplayMaxEvents or more: not kept)
    };

    ReplayTrace*            m_out;
    crd::u32                m_max;
    exec::Interpreter*      m_in = nullptr;
    containers::Array<Open> m_open; // dispatched ops whose post hook has not run yet, innermost last
};

// The host input reads of a recorded run. Install `source()` as the run's input seam: each read is passed to `live`
// (the host's own source; null answers none) and kept in `out.input_reads` in order, the first kReplayMaxInputReads of
// them, with every read counted in `out.input_reads_total`. Clears both. Threads: the executing thread.
class InputRecorder
{
public:
    InputRecorder(ReplayTrace& out, const input::InputSource* live);
    InputRecorder(const InputRecorder&)            = delete;
    InputRecorder& operator=(const InputRecorder&) = delete;
    InputRecorder(InputRecorder&&)                 = delete;
    InputRecorder& operator=(InputRecorder&&)      = delete;
    ~InputRecorder()                               = default;

    [[nodiscard]] const input::InputSource* source() const noexcept { return &m_source; }

private:
    static bool next(input::InputKind kind, crd::u32 channel, crd::i64& out, void* user);

    ReplayTrace*              m_out;
    const input::InputSource* m_live;
    input::InputSource        m_source;
};

// The host inputs of a replay: install `source()` as the run's input seam and it answers each read from `reads` in
// order, never from a live host. A read of another kind or channel than the recorded one at its position, or past the
// last recorded read, is refused (the read fails InputUnavailable) and noted in `out.input_refusal` with the trace
// event of the op that asked; later reads are refused too. Answered reads are kept in `out.input_reads`. Clears both.
// Threads: the executing thread.
class InputFeed
{
public:
    InputFeed(containers::ConstSpan<ReplayInputRead> reads, ReplayTrace& out);
    InputFeed(const InputFeed&)            = delete;
    InputFeed& operator=(const InputFeed&) = delete;
    InputFeed(InputFeed&&)                 = delete;
    InputFeed& operator=(InputFeed&&)      = delete;
    ~InputFeed()                           = default;

    [[nodiscard]] const input::InputSource* source() const noexcept { return &m_source; }

private:
    static bool next(input::InputKind kind, crd::u32 channel, crd::i64& out, void* user);

    containers::ConstSpan<ReplayInputRead> m_reads;
    ReplayTrace*                           m_out;
    input::InputSource                     m_source;
};

// Run `program` on `args`, keeping at most `max_events` events. `cancel` stops the run at its next safe point
// (RunError::Cancelled). `inputs` is the run's host input seam (null: every read fails InputUnavailable); pass an
// `InputRecorder`'s or an `InputFeed`'s source to record or replay the reads.
void run_traced(const ReplayProgram& program, containers::ConstSpan<crd::i64> args, crd::u32 max_events,
                const std::atomic<bool>* cancel, ReplayTrace& out, const input::InputSource* inputs = nullptr);

// The authored position of instr `at` of `program`, as plain values (`file` is a view into `ctx`).
struct ReplaySite
{
    crd::u64               op = 0U;
    containers::StringView file;
    crd::u32               line = 0U;
    crd::u32               col  = 0U;
};
[[nodiscard]] ReplaySite replay_site(const Context& ctx, const ReplayProgram& program, plan::InstrRef at) noexcept;

// An authored position that owns its file name, for an answer that outlives the Context it was read from.
struct OwnedReplaySite
{
    explicit OwnedReplaySite(memory::IAllocator* a) : file(a) {}

    crd::u64           op = 0U;
    containers::String file;
    crd::u32           line = 0U;
    crd::u32           col  = 0U;
};

// The authored position of the op with stable id `op` in `program` (its first compiled instr), or an empty site.
[[nodiscard]] ReplaySite replay_site_of_op(const Context& ctx, const ReplayProgram& program, crd::u64 op) noexcept;

// The authored position of the op with stable id `op` in `module` (no compiled plan needed: a host record's blame),
// or an empty site. `scratch` backs a temporary origin list.
[[nodiscard]] ReplaySite replay_site_in_module(const Context& ctx, const Module& module, crd::u64 op,
                                               memory::IAllocator* scratch);

// The input states a record of `module` holds when `executor` runs it: the program, build and entry arguments are
// recorded, and so is the schedule on the host executor (its settings are in the record); `random` is recorded when
// `host_inputs_held` (the record holds every host input read the run made) and every op that reads randomness is an
// input op reading through the seam; any other input the program needs, or may need through an opaque op, is missing,
// never assumed; the rest are not needed. `missing` (when not null) gains the missing inputs' names, comma-separated,
// in record order. Returns false, with `inputs` incomplete, when `cancel` was raised during the walk.
[[nodiscard]] bool classify_replay_inputs(const Context& ctx, const Module& module, ReplayExecutorKind executor,
                                          bool host_inputs_held, memory::IAllocator* alloc,
                                          const std::atomic<bool>* cancel, ReplayInput (&inputs)[kReplayInputs],
                                          containers::String* missing);

// The names of `record`'s missing inputs, comma-separated in record order, appended to `out`. True when none is.
[[nodiscard]] bool record_missing_inputs(const ReplayRecord& record, containers::String& out);

// NOLINTNEXTLINE(performance-enum-size)
enum class DivergenceKind : crd::u8
{
    None = 0,
    Path,    // event `index` names another op or depth
    Value,   // event `index`'s result `value_index` (or its count of read results) differs
    Length,  // the runs dispatched a different number of instrs
    Outcome, // the run error (the executor's own) or the op it blamed differs
    Results, // result `index` (or the result count) differs
    Cells,   // state cell `index` (or the cell count) differs
    Input,   // host input read `index` asked for another kind or channel than recorded, or the read counts differ
};

// "none", "path", "value", "length", "outcome", "results", "cells", "input".
[[nodiscard]] containers::StringView divergence_kind_name(DivergenceKind k) noexcept;

struct Divergence
{
    DivergenceKind kind        = DivergenceKind::None;
    crd::u64       index       = 0U;
    crd::u32       value_index = 0U;
    crd::u64       recorded_op = 0U; // Path, Value, Outcome: the op the record names
    crd::u64       observed_op = 0U; // Path, Value, Outcome: the op the replay reached
    crd::i64       recorded    = 0;  // the recorded value, count (when `count`), run error or input channel
    crd::i64       observed    = 0;
    bool           count       = false; // `recorded` and `observed` are counts (events, read results, values)
    plan::InstrRef site;             // the replay's instr to blame (invalid when none, and for a host trace)
};

// The first divergence of `trace` from `record` (kind None: the replay reproduced the record). Only the kept events
// are compared one by one; `events_total` is compared whole. In run order: the kept events up to the op whose host
// input read was refused, then that refusal (Input: `recorded` and `observed` are the recorded and requested channels,
// or the read counts when the record had no read left), then the rest of the events, length, a different number of
// host input reads (Input, counts), outcome, results and cells.
[[nodiscard]] Divergence first_divergence(const ReplayRecord& record, const ReplayTrace& trace) noexcept;
} // namespace crd::ceir::cook
