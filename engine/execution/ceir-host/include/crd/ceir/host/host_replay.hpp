#pragma once

// crd-ceir-host -- run records of the host provider (DIAG.9a). A host record is a crd-ceir-cook run record whose
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
// ⛔ The caller owns the crd::jobs pool lifecycle, as for HostProvider. Contract: docs/design/runtime-diagnostics.md
// (DIAG.9a).

#include <crd/ceir/cook/hot_reload.hpp> // Registrar
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::ceir
{
class Context;
} // namespace crd::ceir

namespace crd::ceir::host
{
// The host provider's schedule settings for one run.
struct HostSchedule
{
    crd::u32 num_jobs = 8U;                 // the job split (1..cook::kReplayMaxHostJobs)
    crd::u64 sub_fuel = crd::u64{1} << 20U; // the step budget of each body run on a sub-interpreter (at least 1)
};

// Run `entry(args)` of `module` (in `ctx`, stable ids assigned) on a fresh HostProvider with `schedule`, keeping at
// most `max_events` events of its submitting interpreter in `out`.
void run_host_traced(Context& ctx, const Module& module, containers::StringView entry,
                     containers::ConstSpan<crd::i64> args, const HostSchedule& schedule, crd::u32 max_events,
                     cook::ReplayTrace& out);

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
};

// "ok", "bad-schedule", "not-loaded", "wrong-executor", "other-build", "missing-inputs", "content-mismatch".
[[nodiscard]] containers::StringView host_replay_status_name(HostReplayStatus s) noexcept;

// Run the cooked program `blob` (authored at `path`) once on `entry(args)` with `schedule` and make its host record
// in `out`. `registrar` installs the program's dialects into the fresh Context it is loaded in. `missing` (when not
// null) gains the inputs stored as missing. The record is made whatever the run's outcome (a failing run is recorded
// with its error and the op it blamed).
[[nodiscard]] HostReplayStatus record_host_run(containers::ConstSpan<crd::u8> blob, containers::StringView path,
                                               containers::StringView entry, containers::ConstSpan<crd::i64> args,
                                               const HostSchedule& schedule, crd::u32 max_events,
                                               cook::Registrar registrar, void* user, cook::ReplayRecord& out,
                                               containers::String* missing);

struct HostReplayOptions
{
    containers::ConstSpan<crd::u8> against{}; // empty: the record's own program; else this cooked program instead
    bool                           any_build = false;
    crd::u32                       num_jobs  = 0U; // 0: the recorded job split; else this split instead
};

// An authored position, owned (the Context it was read from is gone when the replay returns).
struct HostSite
{
    explicit HostSite(memory::IAllocator* a) : file(a) {}

    crd::u64           op = 0U;
    containers::String file;
    crd::u32           line = 0U;
    crd::u32           col  = 0U;
};

struct HostReplay
{
    explicit HostReplay(memory::IAllocator* a) : reason(a), trace(a), site(a), fault(a) {}

    containers::String reason; // a refusal's detail: the differing build fields, or the missing inputs
    cook::ReplayTrace  trace;
    cook::Divergence   divergence;
    HostSite           site;  // the divergence's op in the replayed program (op 0 when none names an op)
    HostSite           fault; // the replayed run's faulting op
    crd::u64           replayed_hash = 0U;
    crd::u32           num_jobs      = 0U; // the job split the replay ran with
    bool               build_differs = false;
};

// Replay `record` (see the header comment). `out.divergence.kind == None` when the run reproduced the record.
[[nodiscard]] HostReplayStatus replay_host_record(const cook::ReplayRecord& record, cook::Registrar registrar,
                                                  void* user, const HostReplayOptions& options, HostReplay& out);
} // namespace crd::ceir::host
