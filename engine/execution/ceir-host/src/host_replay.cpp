#include <crd/ceir/host/host_replay.hpp>

#include <crd/ceir/binary.hpp> // stable_hash
#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/host/host_provider.hpp>
#include <crd/ceir/inspect.hpp>
#include <crd/containers/array.hpp>

#include <utility>

namespace crd::ceir::host
{
namespace
{
namespace cont = crd::containers;

void attach_recorder(exec::Interpreter& in, void* user)
{
    static_cast<cook::InterpreterRecorder*>(user)->attach(in);
}

void detach_recorder(exec::Interpreter& in, void* user)
{
    static_cast<cook::InterpreterRecorder*>(user)->detach(in);
}

[[nodiscard]] bool valid_schedule(const HostSchedule& s) noexcept
{
    return s.num_jobs != 0U && s.num_jobs <= cook::kReplayMaxHostJobs && s.sub_fuel != 0U;
}

// Read `blob` into the fresh `ctx` with the host's dialects and assign stable ids. Null when it did not read.
[[nodiscard]] Module* load(Context& ctx, cont::ConstSpan<crd::u8> blob, cook::Registrar registrar, void* user,
                           crd::u64& content_hash)
{
    if (registrar != nullptr)
    {
        registrar(ctx, user);
    }
    const cook::ReadResult rr = cook::read_program(ctx, blob, ctx.allocator());
    if (!rr.ok() || rr.module == nullptr)
    {
        return nullptr;
    }
    ctx.assign_stable_ids(*rr.module);
    content_hash = rr.content_hash;
    return rr.module;
}

void site_of(const Context& ctx, const Module& module, crd::u64 op, HostSite& out)
{
    const cook::ReplaySite s = cook::replay_site_in_module(ctx, module, op, out.file.allocator());
    out.op                   = op;
    out.file.clear();
    out.file.append(s.file);
    out.line = s.line;
    out.col  = s.col;
}

// The replayed op a divergence points at: the op an event names, the faulting op, or the first event past the
// common prefix. 0 for a results or cells difference.
[[nodiscard]] crd::u64 blamed_op(const cook::Divergence& d, const cook::ReplayTrace& trace) noexcept
{
    switch (d.kind) // no default (-Werror=switch)
    {
    case cook::DivergenceKind::None: return 0U;
    case cook::DivergenceKind::Path:
    case cook::DivergenceKind::Value: return d.observed_op;
    case cook::DivergenceKind::Length:
        return d.index < trace.events.size() ? trace.events[static_cast<crd::usize>(d.index)].op : 0U;
    case cook::DivergenceKind::Outcome: return trace.fault_op;
    case cook::DivergenceKind::Input: return d.observed_op; // the op whose read the record could not answer, if any
    case cook::DivergenceKind::Results:
    case cook::DivergenceKind::Cells: return 0U;
    }
    return 0U;
}
} // namespace

cont::StringView host_replay_status_name(HostReplayStatus s) noexcept
{
    switch (s) // no default (-Werror=switch)
    {
    case HostReplayStatus::Ok: return cont::StringView{"ok"};
    case HostReplayStatus::BadSchedule: return cont::StringView{"bad-schedule"};
    case HostReplayStatus::NotLoaded: return cont::StringView{"not-loaded"};
    case HostReplayStatus::WrongExecutor: return cont::StringView{"wrong-executor"};
    case HostReplayStatus::OtherBuild: return cont::StringView{"other-build"};
    case HostReplayStatus::MissingInputs: return cont::StringView{"missing-inputs"};
    case HostReplayStatus::ContentMismatch: return cont::StringView{"content-mismatch"};
    case HostReplayStatus::Cancelled: return cont::StringView{"cancelled"};
    }
    return cont::StringView{"?"};
}

void run_host_traced(Context& ctx, const Module& module, cont::StringView entry, cont::ConstSpan<crd::i64> args,
                     const HostSchedule& schedule, crd::u32 max_events, cook::ReplayTrace& out,
                     inspect::Session* session, const input::InputSource* inputs)
{
    cook::InterpreterRecorder rec(out, max_events);
    HostProvider              provider(out.events.allocator(), schedule.num_jobs, schedule.sub_fuel);
    provider.set_input_source(inputs);
    const HostObserver observer{&attach_recorder, &detach_recorder, &rec};
    const exec::ExecResult    r = session != nullptr ? provider.execute(ctx, module, entry, args, *session, observer)
                                                     : provider.execute(ctx, module, entry, args, observer);
    rec.finish(r);
}

HostProgram::HostProgram(memory::IAllocator* alloc, cont::ConstSpan<crd::u8> blob, cont::StringView path,
                         cook::Registrar registrar, void* user)
    : m_ctx(alloc), m_blob(alloc), m_path(alloc)
{
    if (path.size() > cook::kReplayMaxStringBytes || blob.size() > cook::kReplayMaxProgramBytes)
    {
        m_status = HostReplayStatus::BadSchedule;
        return;
    }
    m_module = load(m_ctx, blob, registrar, user, m_hash);
    if (m_module == nullptr)
    {
        m_status = HostReplayStatus::NotLoaded;
        return;
    }
    m_blob.reserve(blob.size());
    for (const crd::u8 b : blob)
    {
        m_blob.push_back(b);
    }
    m_path.append(path);
    m_status = HostReplayStatus::Ok;
}

HostReplayStatus record_host_run(cont::ConstSpan<crd::u8> blob, cont::StringView path, cont::StringView entry,
                                 cont::ConstSpan<crd::i64> args, const HostSchedule& schedule, crd::u32 max_events,
                                 cook::Registrar registrar, void* user, cook::ReplayRecord& out, cont::String* missing,
                                 HostSite* fault, const input::InputSource* inputs)
{
    if (!valid_schedule(schedule) || max_events == 0U || args.size() > cook::kReplayMaxArgs ||
        entry.size() > cook::kReplayMaxStringBytes || path.size() > cook::kReplayMaxStringBytes ||
        blob.size() > cook::kReplayMaxProgramBytes)
    {
        return HostReplayStatus::BadSchedule;
    }
    HostProgram program(out.program.allocator(), blob, path, registrar, user);
    if (program.status() != HostReplayStatus::Ok)
    {
        return program.status();
    }
    return record_host_run(program, entry, args, schedule, max_events, nullptr, out, missing, fault, inputs);
}

HostReplayStatus record_host_run(HostProgram& program, cont::StringView entry, cont::ConstSpan<crd::i64> args,
                                 const HostSchedule& schedule, crd::u32 max_events, inspect::Session* session,
                                 cook::ReplayRecord& out, cont::String* missing, HostSite* fault,
                                 const input::InputSource* inputs)
{
    if (program.status() != HostReplayStatus::Ok)
    {
        return program.status();
    }
    if (!valid_schedule(schedule) || max_events == 0U || args.size() > cook::kReplayMaxArgs ||
        entry.size() > cook::kReplayMaxStringBytes)
    {
        return HostReplayStatus::BadSchedule;
    }
    memory::IAllocator* const alloc  = out.program.allocator();
    Context&                  ctx    = program.context();
    const Module&             module = *program.module();

    // Run first: a cancelled run leaves `out` as it was.
    cook::ReplayTrace   trace(alloc);
    cook::InputRecorder recorded_inputs(trace, inputs); // every host input read, kept for the record
    run_host_traced(ctx, module, entry, args, schedule, max_events, trace, session, recorded_inputs.source());
    if (trace.host_error == exec::ExecError::Cancelled)
    {
        return HostReplayStatus::Cancelled;
    }

    const cont::ConstSpan<crd::u8> blob = program.blob();

    out.schema        = cook::kReplayRecordSchema;
    out.build         = cook::current_build(alloc);
    out.executor      = cook::ReplayExecutorKind::Host;
    out.host_jobs     = schedule.num_jobs;
    out.host_sub_fuel = schedule.sub_fuel;
    out.program_path.clear();
    out.program_path.append(program.path());
    out.content_hash = program.content_hash();
    out.asset        = 0U;
    out.generation   = 0U;
    out.program.clear();
    out.program.reserve(blob.size());
    for (const crd::u8 b : blob)
    {
        out.program.push_back(b);
    }
    out.entry.clear();
    out.entry.append(entry);
    out.args.clear();
    for (const crd::i64 a : args)
    {
        out.args.push_back(a);
    }
    const bool held = trace.input_reads_total <= cook::kReplayMaxInputReads; // every read is in the record
    (void)cook::classify_replay_inputs(ctx, module, cook::ReplayExecutorKind::Host, held, alloc, nullptr, out.inputs,
                                       missing); // no cancel: always whole
    out.input_reads_total = trace.input_reads_total;
    out.input_reads       = std::move(trace.input_reads);

    out.max_events   = max_events < cook::kReplayMaxEvents ? max_events : cook::kReplayMaxEvents;
    out.events_total = trace.events_total;
    out.events       = std::move(trace.events);
    out.error        = plan::RunError::None;
    out.host_error   = trace.host_error;
    out.fault_op     = trace.fault_op;
    out.results      = std::move(trace.results);
    out.cells        = std::move(trace.cells);
    if (fault != nullptr)
    {
        site_of(ctx, module, out.fault_op, *fault);
    }
    return HostReplayStatus::Ok;
}

HostReplayStatus replay_host_record(const cook::ReplayRecord& record, cook::Registrar registrar, void* user,
                                    const HostReplayOptions& options, HostReplay& out)
{
    memory::IAllocator* const alloc = out.reason.allocator();
    out.reason.clear();
    if (record.executor != cook::ReplayExecutorKind::Host)
    {
        out.reason.append(cook::replay_executor_name(record.executor));
        return HostReplayStatus::WrongExecutor;
    }
    const HostSchedule schedule{options.num_jobs != 0U ? options.num_jobs : record.host_jobs, record.host_sub_fuel};
    if (!valid_schedule(schedule))
    {
        return HostReplayStatus::BadSchedule;
    }
    cont::String differing(alloc);
    out.build_differs = !cook::same_build(record.build, cook::current_build(alloc), differing);
    if (out.build_differs && !options.any_build)
    {
        out.reason.append(cont::StringView{differing.data(), differing.size()});
        return HostReplayStatus::OtherBuild;
    }
    if (!cook::record_missing_inputs(record, out.reason))
    {
        return HostReplayStatus::MissingInputs;
    }

    // The record's own artifact, never the checkout's file; its content must be the hash it was recorded with.
    Context       rctx(alloc);
    crd::u64      header_hash = 0U;
    Module* const recorded = load(rctx, {record.program.data(), record.program.size()}, registrar, user, header_hash);
    if (recorded == nullptr)
    {
        return HostReplayStatus::NotLoaded;
    }
    if (header_hash != record.content_hash || stable_hash(rctx, *recorded, alloc) != record.content_hash)
    {
        return HostReplayStatus::ContentMismatch;
    }

    // An explicit other program replays the same inputs.
    Context  actx(alloc);
    Context* ctx      = &rctx;
    Module*  module   = recorded;
    out.replayed_hash = header_hash;
    if (!options.against.empty())
    {
        module = load(actx, options.against, registrar, user, out.replayed_hash);
        if (module == nullptr)
        {
            return HostReplayStatus::NotLoaded;
        }
        ctx = &actx;
    }

    site_of(rctx, *recorded, record.fault_op, out.recorded_fault);
    out.num_jobs = schedule.num_jobs;
    cook::InputFeed feed({record.input_reads.data(), record.input_reads.size()}, out.trace); // never a live source
    run_host_traced(*ctx, *module, cont::StringView{record.entry.data(), record.entry.size()},
                    cont::as_const_span(record.args), schedule, record.max_events, out.trace, nullptr, feed.source());
    out.divergence = cook::first_divergence(record, out.trace);
    site_of(*ctx, *module, blamed_op(out.divergence, out.trace), out.site);
    site_of(*ctx, *module, out.trace.fault_op, out.fault);
    return HostReplayStatus::Ok;
}
} // namespace crd::ceir::host
