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
// Replay loads the record's own blob into a fresh Context, compiles the record's entry, runs it on the recorded
// arguments with the same trace bound and reports the first divergence: the first event whose op or depth differs
// (path), the first differing result value (value), a different event count (length), outcome, results or cells.
// A replay against another program (an edited checkout) is a separate, explicit request; it uses the same inputs.
//
// Guarantee: this is event replay of the compiled-plan executor, whose semantics are integer and sequential. Schedule
// replay (pooled or parallel work) and backend-specific numeric replay are different guarantees; a program needing
// them records those inputs as missing. Contract: docs/design/runtime-diagnostics.md (DIAG.9a).

#include <crd/ceir/cook/hot_reload.hpp> // Registrar
#include <crd/ceir/cook/program_cook.hpp> // ReadError
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
inline constexpr crd::u32 kReplayRecordSchema = 1U; // the record file layout
inline constexpr crd::u32 kReplayExecutor     = 1U; // the compiled-plan semantics a trace is valid for

inline constexpr crd::u32 kReplayDefaultMaxEvents = 4096U;
inline constexpr crd::u32 kReplayMaxEvents        = 65536U;
inline constexpr crd::u32 kReplayEventValues      = 4U;  // results kept per event
inline constexpr crd::u32 kReplayMaxArgs          = 64U;
inline constexpr crd::u32 kReplayMaxStringBytes   = 256U;
inline constexpr crd::u32 kReplayMaxValues        = 65536U; // results, and state cells, in an outcome
inline constexpr crd::u64 kReplayMaxProgramBytes  = 16ULL * 1024ULL * 1024ULL;
inline constexpr crd::u32 kReplayInputs           = 9U;

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

// What one traced run produced. `sites` is parallel to `events` (the instr of each kept event), for blame; it is not
// stored in a record.
struct ReplayTrace
{
    explicit ReplayTrace(memory::IAllocator* alloc) : events(alloc), sites(alloc), results(alloc), cells(alloc) {}

    containers::Array<ReplayEvent>    events;
    containers::Array<plan::InstrRef> sites;
    crd::u64                          events_total = 0U;
    plan::RunError                    error        = plan::RunError::None;
    plan::InstrRef                    fault;
    crd::u64                          fault_op = 0U; // the faulting instr's op stable id (0 when none)
    containers::Array<crd::i64>       results;
    containers::Array<crd::i64>       cells;
};

struct ReplayRecord
{
    explicit ReplayRecord(memory::IAllocator* alloc)
        : build(alloc), program_path(alloc), program(alloc), entry(alloc), args(alloc), events(alloc), results(alloc),
          cells(alloc)
    {
    }

    crd::u32                       schema = kReplayRecordSchema;
    ReplayBuild                    build;
    containers::String             program_path; // the authored path the program was cooked under
    crd::u64                       content_hash = 0U; // the cooked program's content hash
    containers::Array<crd::u8>     program;           // the cooked CRDR blob
    containers::String             entry;
    containers::Array<crd::i64>    args;
    ReplayInput                    inputs[kReplayInputs];
    crd::u32                       max_events   = kReplayDefaultMaxEvents;
    crd::u64                       events_total = 0U;
    containers::Array<ReplayEvent> events; // the first min(events_total, max_events) events
    plan::RunError                 error    = plan::RunError::None;
    crd::u64                       fault_op = 0U;
    containers::Array<crd::i64>    results;
    containers::Array<crd::i64>    cells;
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
    Malformed,         // a field is out of its bounds or range, or bytes follow the last field
};

// "ok", "not-a-record", "unsupported-schema", "truncated", "bad-checksum", "malformed".
[[nodiscard]] containers::StringView record_error_name(RecordError e) noexcept;

// Whether `bytes` start with a run record's magic.
[[nodiscard]] bool is_replay_record(containers::ConstSpan<crd::u8> bytes) noexcept;

// Encode `record` into `out` (cleared first). Deterministic: one record always gives the same bytes.
void encode_record(const ReplayRecord& record, containers::Array<crd::u8>& out);

// Decode `bytes` into `out`. Every count and size is bounded before it is used; nothing is executed.
[[nodiscard]] RecordError decode_record(containers::ConstSpan<crd::u8> bytes, ReplayRecord& out);

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

// Run `program` on `args`, keeping at most `max_events` events. `cancel` stops the run at its next safe point
// (RunError::Cancelled).
void run_traced(const ReplayProgram& program, containers::ConstSpan<crd::i64> args, crd::u32 max_events,
                const std::atomic<bool>* cancel, ReplayTrace& out);

// The authored position of instr `at` of `program`, as plain values (`file` is a view into `ctx`).
struct ReplaySite
{
    crd::u64               op = 0U;
    containers::StringView file;
    crd::u32               line = 0U;
    crd::u32               col  = 0U;
};
[[nodiscard]] ReplaySite replay_site(const Context& ctx, const ReplayProgram& program, plan::InstrRef at) noexcept;

// The authored position of the op with stable id `op` in `program` (its first compiled instr), or an empty site.
[[nodiscard]] ReplaySite replay_site_of_op(const Context& ctx, const ReplayProgram& program, crd::u64 op) noexcept;

// NOLINTNEXTLINE(performance-enum-size)
enum class DivergenceKind : crd::u8
{
    None = 0,
    Path,    // event `index` names another op or depth
    Value,   // event `index`'s result `value_index` (or its count of read results) differs
    Length,  // the runs dispatched a different number of instrs
    Outcome, // the run error or the op it blamed differs
    Results, // result `index` (or the result count) differs
    Cells,   // state cell `index` (or the cell count) differs
};

// "none", "path", "value", "length", "outcome", "results", "cells".
[[nodiscard]] containers::StringView divergence_kind_name(DivergenceKind k) noexcept;

struct Divergence
{
    DivergenceKind kind        = DivergenceKind::None;
    crd::u64       index       = 0U;
    crd::u32       value_index = 0U;
    crd::u64       recorded_op = 0U; // Path, Value, Outcome: the op the record names
    crd::u64       observed_op = 0U; // Path, Value, Outcome: the op the replay reached
    crd::i64       recorded    = 0;  // the recorded value, count (when `count`) or run error
    crd::i64       observed    = 0;
    bool           count       = false; // `recorded` and `observed` are counts (events, read results, values)
    plan::InstrRef site;             // the replay's instr to blame (invalid when none)
};

// The first divergence of `trace` from `record` (kind None: the replay reproduced the record). Only the kept events
// are compared one by one; `events_total` is compared whole.
[[nodiscard]] Divergence first_divergence(const ReplayRecord& record, const ReplayTrace& trace) noexcept;
} // namespace crd::ceir::cook
