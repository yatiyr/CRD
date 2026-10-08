// The host provider as the replay commands' host executor. Declared in crd/ceir/host/host_replay_diag.hpp.

#include <crd/ceir/host/host_replay_diag.hpp>

#include <crd/ceir/host/host_replay.hpp>
#include <crd/ceir/input.hpp> // HostClock, InputRouter, SeededInputs

#include <utility>

namespace crd::ceir::host
{
namespace
{
namespace cont = crd::containers;

// A refusal's status and its sentence. `detail` is the library's own detail (the differing build fields, the missing
// inputs or the record's executor).
[[nodiscard]] cook::HostExecutorStatus refuse(HostReplayStatus s, cont::StringView detail, cont::String& reason)
{
    switch (s) // no default (-Werror=switch)
    {
    case HostReplayStatus::Ok: return cook::HostExecutorStatus::Ok;
    case HostReplayStatus::BadSchedule:
        reason.append("the host executor's job split or step budget is out of range");
        return cook::HostExecutorStatus::BadArgument;
    case HostReplayStatus::NotLoaded:
        reason.append("the program did not load on the host executor");
        return cook::HostExecutorStatus::Failed;
    case HostReplayStatus::WrongExecutor:
        reason.append("incompatible replay: the record was made by the ");
        reason.append(detail);
        reason.append(" executor; the host executor replays host records only");
        return cook::HostExecutorStatus::Unavailable;
    case HostReplayStatus::OtherBuild:
        reason.append("incompatible replay: the record was made by another build (differs in ");
        reason.append(detail);
        reason.append("); pass build=any to replay it here anyway");
        return cook::HostExecutorStatus::Unavailable;
    case HostReplayStatus::MissingInputs:
        reason.append("incompatible replay: the record is missing inputs its program needs (");
        reason.append(detail);
        reason.append(")");
        return cook::HostExecutorStatus::Unavailable;
    case HostReplayStatus::ContentMismatch:
        reason.append("the record's program does not match the content hash it was recorded with");
        return cook::HostExecutorStatus::Failed;
    case HostReplayStatus::Cancelled:
        reason.append("the run was cancelled; a cancelled run is not recorded");
        return cook::HostExecutorStatus::Failed;
    }
    return cook::HostExecutorStatus::Failed;
}

cook::HostExecutorStatus record(const cook::HostRecordRequest& request, cook::ReplayRecord& out,
                                cook::OwnedReplaySite& fault, cont::String& missing, cont::String& reason)
{
    const HostSchedule  schedule{request.num_jobs, request.sub_fuel};
    input::SeededInputs seeded(request.seed, out.program.allocator()); // the host's random streams, when seeded
    input::HostClock    clock;                                         // the host's time domains
    cook::apply_clock(request.clock, clock);
    input::InputRouter inputs;
    inputs.route(input::InputKind::Random, request.has_seed ? seeded.source() : nullptr);
    inputs.route(input::InputKind::Clock, clock.source());
    inputs.route(input::InputKind::TimeStep, clock.source());
    const HostReplayStatus s =
        record_host_run(request.blob, request.path, request.entry, request.args, schedule, request.max_events,
                        request.registrar, request.user, out, &missing, &fault, inputs.source());
    return refuse(s, cont::StringView{}, reason);
}

cook::HostExecutorStatus replay(const cook::HostReplayRequest& request, cook::HostReplayAnswer& out,
                                cont::String& reason)
{
    HostReplayOptions options;
    options.against   = request.against;
    options.any_build = request.any_build;
    options.num_jobs  = request.num_jobs;
    HostReplay             replayed(out.trace.events.allocator());
    const HostReplayStatus s =
        replay_host_record(*request.record, request.registrar, request.user, options, replayed);
    if (s != HostReplayStatus::Ok)
    {
        return refuse(s, cont::StringView{replayed.reason.data(), replayed.reason.size()}, reason);
    }
    out.trace          = std::move(replayed.trace);
    out.divergence     = replayed.divergence;
    out.site           = std::move(replayed.site);
    out.fault          = std::move(replayed.fault);
    out.recorded_fault = std::move(replayed.recorded_fault);
    out.replayed_hash  = replayed.replayed_hash;
    out.num_jobs       = replayed.num_jobs;
    return cook::HostExecutorStatus::Ok;
}

const cook::ReplayHostExecutor kExecutor{&record, &replay};
} // namespace

const cook::ReplayHostExecutor& host_replay_executor() noexcept
{
    return kExecutor;
}
} // namespace crd::ceir::host
