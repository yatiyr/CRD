#pragma once

// crd-ceir-host -- run records of the host provider. A host record is a crd-ceir-cook run record whose
// executor is `ReplayExecutorKind::Host`: one run of a cooked program on a fresh `HostProvider`, traced on its
// submitting interpreter (`cook::InterpreterRecorder`, attached through the provider's `HostObserver`), with the
// provider's schedule settings (job split, per-body step budget) stored as the record's schedule input. Bodies the
// provider runs on its own sub-interpreters leave no events; their effect is the result of the op that joins them.
// The provider's other schedule choices are fixed by the build: a race answers its first operand, the lowest failing
// index wins and folds run in index order.
//
// Replay loads the record's own blob into a fresh Context (never the checkout's file), runs the recorded entry and
// arguments on a fresh provider with the recorded schedule and reports the first divergence. It refuses, before
// anything runs, a plan record, a record from another build (unless allowed), one missing an input its program needs
// and one whose blob is not the content it was recorded with. Running the same inputs against another program, or on
// another job split, is an explicit option.
//
// A run may be recorded while an inspection session stops, steps and reads values in it (`HostProgram`, then
// `record_host_run` with the session): the recorder's step hooks run inside the session's, so the record is the one
// an unobserved run makes. A run the session cancels is not recorded.
//
// Host inputs: a recorded run's input reads (input.random's draws) go through a `cook::InputRecorder` around the
// host's live source (`inputs`; null: the host has none and every read fails InputUnavailable, which is recorded) and
// are kept in the record; a replay feeds them back through a `cook::InputFeed` and never asks a live source.
//
// ⛔ The caller owns the crd::jobs pool lifecycle, as for HostProvider. Contract: docs/design/runtime-diagnostics.md.

#include <crd/ceir/cook/hot_reload.hpp> // Registrar
#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir::inspect
{
class Session;
} // namespace crd::ceir::inspect

namespace crd::ceir::host
{
// The host provider's schedule settings for one run.
struct HostSchedule
{
    crd::u32 num_jobs = 8U;                 // the job split (1..cook::kReplayMaxHostJobs)
    crd::u64 sub_fuel = crd::u64{1} << 20U; // the step budget of each body run on a sub-interpreter (at least 1)
};

// Run `entry(args)` of `module` (in `ctx`, stable ids assigned) on a fresh HostProvider with `schedule`, keeping at
// most `max_events` events of its submitting interpreter in `out`. Under `session` (bound to `module`, may be null)
// the execution stops, steps and answers value reads as the session's controller asks; the trace is the same.
// `inputs` is the run's host input seam (null: none).
void run_host_traced(Context& ctx, const Module& module, containers::StringView entry,
                     containers::ConstSpan<crd::i64> args, const HostSchedule& schedule, crd::u32 max_events,
                     cook::ReplayTrace& out, inspect::Session* session = nullptr,
                     const input::InputSource* inputs = nullptr);

// An authored position, owned (the Context it was read from is gone when a call returns).
using HostSite = cook::OwnedReplaySite;

// NOLINTNEXTLINE(performance-enum-size)
enum class HostReplayStatus : crd::u8
{
    Ok = 0,
    BadSchedule,     // a job split or step budget out of range, or a request past a record's bounds
    NotLoaded,       // the program blob did not read
    WrongExecutor,   // a plan record: crd-ceir-cook's replay.run replays it
    OtherBuild,      // made by another build (`any_build` replays it anyway)
    MissingInputs,   // the record lacks an input its program needs
    ContentMismatch, // the record's blob is not the content it was recorded with
    Cancelled,       // the run was cancelled (an inspection session's cancel): a replay would not stop there
};

// "ok", "bad-schedule", "not-loaded", "wrong-executor", "other-build", "missing-inputs", "content-mismatch",
// "cancelled".
[[nodiscard]] containers::StringView host_replay_status_name(HostReplayStatus s) noexcept;

// Run the cooked program `blob` (authored at `path`) once on `entry(args)` with `schedule` and make its host record
// in `out`. `registrar` installs the program's dialects into the fresh Context it is loaded in. `missing` (when not
// null) gains the inputs stored as missing. The record is made whatever the run's outcome (a failing run is recorded
// with its error and the op it blamed); `fault` (when not null) gets that op's authored position. `inputs` is the
// host's live input source (null: none); every read through it is kept in the record.
[[nodiscard]] HostReplayStatus record_host_run(containers::ConstSpan<crd::u8> blob, containers::StringView path,
                                               containers::StringView entry, containers::ConstSpan<crd::i64> args,
                                               const HostSchedule& schedule, crd::u32 max_events,
                                               cook::Registrar registrar, void* user, cook::ReplayRecord& out,
                                               containers::String* missing, HostSite* fault = nullptr,
                                               const input::InputSource* inputs = nullptr);

// A cooked program loaded for recording: its own Context with the host's dialects, the program read and stable ids
// assigned, and a copy of the artifact the record will hold. A host that inspects the recorded run binds its session
// to `module()` in `context()` (the module form of `inspect::Session::bind`) before recording, so breakpoints resolve
// against the ops that run.
class HostProgram
{
public:
    // Read the cooked program `blob` authored at `path`; `registrar` installs its dialects. `status()` says whether it
    // loaded: `BadSchedule` for a blob or path past a record's bounds, `NotLoaded` for one that did not read.
    HostProgram(memory::IAllocator* alloc, containers::ConstSpan<crd::u8> blob, containers::StringView path,
                cook::Registrar registrar, void* user);
    HostProgram(const HostProgram&)            = delete;
    HostProgram& operator=(const HostProgram&) = delete;
    HostProgram(HostProgram&&)                 = delete;
    HostProgram& operator=(HostProgram&&)      = delete;
    ~HostProgram()                             = default;

    [[nodiscard]] HostReplayStatus status() const noexcept { return m_status; }
    [[nodiscard]] Context&         context() noexcept { return m_ctx; }
    [[nodiscard]] const Module*    module() const noexcept { return m_module; } // null unless status() is Ok
    [[nodiscard]] crd::u64         content_hash() const noexcept { return m_hash; }
    [[nodiscard]] containers::ConstSpan<crd::u8> blob() const noexcept { return {m_blob.data(), m_blob.size()}; }
    [[nodiscard]] containers::StringView         path() const noexcept { return {m_path.data(), m_path.size()}; }

private:
    Context                    m_ctx;
    Module*                    m_module = nullptr;
    crd::u64                   m_hash   = 0U;
    containers::Array<crd::u8> m_blob;
    containers::String         m_path;
    HostReplayStatus           m_status = HostReplayStatus::NotLoaded;
};

// Record one run of a loaded `program` on `entry(args)` with `schedule`, as the blob form above does, optionally under
// an inspection `session` bound to `program.module()` (null: no session). The session's stops, steps and value reads
// leave the record exactly as an unobserved run's: the recorder's step hooks run inside the session's, before it
// decides whether to stop. A run the session cancelled is refused `Cancelled` and `out` is left as it was. The program
// may be recorded more than once; each run is on a fresh provider.
[[nodiscard]] HostReplayStatus record_host_run(HostProgram& program, containers::StringView entry,
                                               containers::ConstSpan<crd::i64> args, const HostSchedule& schedule,
                                               crd::u32 max_events, inspect::Session* session,
                                               cook::ReplayRecord& out, containers::String* missing,
                                               HostSite* fault = nullptr, const input::InputSource* inputs = nullptr);

struct HostReplayOptions
{
    containers::ConstSpan<crd::u8> against{}; // empty: the record's own program; else this cooked program instead
    bool                           any_build = false;
    crd::u32                       num_jobs  = 0U; // 0: the recorded job split; else this split instead
};

struct HostReplay
{
    explicit HostReplay(memory::IAllocator* a) : reason(a), trace(a), site(a), fault(a), recorded_fault(a) {}

    containers::String reason; // a refusal's detail: the differing build fields, or the missing inputs
    cook::ReplayTrace  trace;
    cook::Divergence   divergence;
    HostSite           site;  // the divergence's op in the replayed program (op 0 when none names an op)
    HostSite           fault; // the replayed run's faulting op
    HostSite           recorded_fault; // the recorded faulting op, in the record's own program
    crd::u64           replayed_hash = 0U;
    crd::u32           num_jobs      = 0U; // the job split the replay ran with
    bool               build_differs = false;
};

// Replay `record` (see the header comment). `out.divergence.kind == None` when the run reproduced the record.
[[nodiscard]] HostReplayStatus replay_host_record(const cook::ReplayRecord& record, cook::Registrar registrar,
                                                  void* user, const HostReplayOptions& options, HostReplay& out);
} // namespace crd::ceir::host
