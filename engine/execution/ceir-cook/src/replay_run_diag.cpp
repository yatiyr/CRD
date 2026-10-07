// replay.record and replay.run: write a run record of an authored program, and replay one. Declared in
// crd/ceir/cook/replay_diag.hpp; the record format and the traced run are crd/ceir/cook/replay_record.hpp.

#include <crd/ceir/cook/replay_diag.hpp>

#include "bounded_file.hpp"
#include "diag_args.hpp"
#include "program_load.hpp"
#include "replay_needs.hpp"

#include <crd/ceir/binary.hpp> // stable_hash
#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/diag_commands.hpp>

#include <utility>

namespace crd::ceir::cook
{
namespace
{
namespace cont = crd::containers;

using crd::perf::DiagArg;
using crd::perf::DiagCall;
using crd::perf::DiagFields;
using crd::perf::DiagSnapshot;
using crd::perf::DiagStatus;

constexpr crd::u32 kMaxEntryBytes = 64U;
constexpr crd::u64 kCookedAssetId = 1U;

[[nodiscard]] DiagStatus bad(cont::String& reason, cont::StringView name, cont::StringView what)
{
    reason.append("the argument '");
    reason.append(name);
    reason.append("' ");
    reason.append(what);
    return DiagStatus::BadArgument;
}

constexpr cont::StringView kPathRule{"must be a relative path of plain [A-Za-z0-9._-] names separated by '/'"};

// `root` joined to `relative` (already accepted by perf::diag_path_is_safe), as the service joins a request path.
void join(cont::StringView root, cont::StringView relative, cont::String& out)
{
    out.clear();
    out.append(root);
    if (!root.empty() && root[root.size() - 1U] != '/' && root[root.size() - 1U] != '\\')
    {
        out.push_back('/');
    }
    out.append(relative);
}

// ---- replay.record --------------------------------------------------------------------------------------------------

struct RecordArgs
{
    explicit RecordArgs(crd::memory::IAllocator* alloc) : args(alloc) {}

    cont::StringView      entry{"main"};
    cont::Array<crd::i64> args;
    cont::StringView      out;
    crd::u32              max_events = kReplayDefaultMaxEvents;
};

// Parse every argument; `out` null validates only.
[[nodiscard]] DiagStatus parse_record_args(cont::ConstSpan<DiagArg> args, RecordArgs* out, cont::String& reason)
{
    bool have_out = false;
    for (const DiagArg& a : args)
    {
        if (a.name == "entry")
        {
            if (!detail::valid_entry_name(a.value, kMaxEntryBytes))
            {
                return bad(reason, a.name, "must be 1 to 64 bytes of [A-Za-z0-9_.]");
            }
            if (out != nullptr)
            {
                out->entry = a.value;
            }
        }
        else if (a.name == "args")
        {
            crd::u32   count = 0U;
            const bool ok    = detail::for_each_item(a.value,
                                                     [out, &count](cont::StringView item)
                                                     {
                                                         crd::i64 v = 0;
                                                         if (!detail::parse_i64(item, v) || ++count > kReplayMaxArgs)
                                                         {
                                                             return false;
                                                         }
                                                         if (out != nullptr)
                                                         {
                                                             out->args.push_back(v);
                                                         }
                                                         return true;
                                                     });
            if (!ok)
            {
                return bad(reason, a.name, "must be at most 64 comma-separated i64 values");
            }
        }
        else if (a.name == "out")
        {
            if (!perf::diag_path_is_safe(a.value))
            {
                return bad(reason, a.name, kPathRule);
            }
            have_out = true;
            if (out != nullptr)
            {
                out->out = a.value;
            }
        }
        else if (a.name == "max_events")
        {
            crd::u64 v = 0U;
            if (!detail::parse_u64(a.value, kReplayMaxEvents, v) || v == 0U)
            {
                return bad(reason, a.name, "must be 1 to 65536");
            }
            if (out != nullptr)
            {
                out->max_events = static_cast<crd::u32>(v);
            }
        }
        else
        {
            reason.append("unknown argument '");
            reason.append(a.name);
            reason.append("'; replay.record takes out, entry, args and max_events");
            return DiagStatus::BadArgument;
        }
    }
    if (!have_out)
    {
        reason.append("replay.record needs the argument 'out', the record's path under the root");
        return DiagStatus::BadArgument;
    }
    return DiagStatus::Ok;
}

DiagStatus check_replay_record(void* /*context*/, cont::ConstSpan<DiagArg> args, cont::String& reason)
{
    return parse_record_args(args, nullptr, reason);
}

// The immutable artifact of a loaded program: a cooked file is kept byte for byte; text and binary are cooked (text
// keeps the authored positions it was parsed under).
[[nodiscard]] DiagStatus cook_artifact(Context& src, const detail::LoadedProgram& loaded, cont::Array<crd::u8>& bytes,
                                       cont::StringView path, cont::Array<crd::u8>& blob, cont::String& reason)
{
    crd::memory::IAllocator* const alloc = blob.allocator();
    if (loaded.form == detail::ProgramForm::Cooked)
    {
        blob = std::move(bytes);
    }
    else
    {
        CookResult cr = cook_program(src, *loaded.module, kCookedAssetId, alloc, alloc);
        if (!cr.ok())
        {
            reason.append("the program did not cook: ");
            reason.append(cook_error_name(cr.error));
            reason.append(" at ");
            reason.append(path);
            reason.push_back(':');
            detail::append_decimal(reason, cr.site.line);
            reason.push_back(':');
            detail::append_decimal(reason, cr.site.col);
            return DiagStatus::Failed;
        }
        blob = std::move(cr.blob);
    }
    if (blob.size() > kReplayMaxProgramBytes)
    {
        reason.append("the cooked program is larger than a record holds");
        return DiagStatus::Oversized;
    }
    return DiagStatus::Ok;
}

// Why a loaded artifact cannot run: its blob did not read, or its entry did not compile.
[[nodiscard]] DiagStatus describe_unrunnable(const ReplayProgram& p, cont::StringView whose, cont::String& reason)
{
    reason.append(whose);
    if (p.module == nullptr)
    {
        reason.append(" program did not load: ");
        reason.append(read_error_name(p.read));
    }
    else
    {
        reason.append(" entry did not compile: ");
        reason.append(plan::compile_error_name(p.compiled.error));
    }
    return DiagStatus::Failed;
}

DiagStatus run_replay_record(void* context, const DiagCall& call, DiagSnapshot& out)
{
    auto* const                    cmd   = static_cast<ReplayCommands*>(context);
    crd::memory::IAllocator* const alloc = out.allocator();
    cmd->record_runs.fetch_add(1U, std::memory_order_relaxed);

    RecordArgs parsed(alloc);
    if (const DiagStatus s = parse_record_args(call.request->args, &parsed, out.reason); s != DiagStatus::Ok)
    {
        return s;
    }

    // An existing record is refused before anything is read or run (the create below is exclusive as well).
    cont::String target(alloc);
    join(call.root, parsed.out, target);
    if (detail::file_exists({target.data(), target.size()}))
    {
        out.reason.append("refusing to overwrite an existing file");
        return DiagStatus::Failed;
    }

    // The authored program, bounded before a byte is read, then its immutable artifact.
    Context               src(alloc);
    cont::Array<crd::u8>  bytes(alloc);
    detail::LoadedProgram loaded;
    if (const DiagStatus s = detail::load_program(call, cmd->max_program_bytes, cmd->registrar, cmd->user, src, bytes,
                                                  loaded, out.reason, cmd->bytes_read);
        s != DiagStatus::Ok)
    {
        return s;
    }
    cont::Array<crd::u8> blob(alloc);
    if (const DiagStatus s = cook_artifact(src, loaded, bytes, call.request->path, blob, out.reason);
        s != DiagStatus::Ok)
    {
        return s;
    }

    // The artifact is loaded exactly as a replay will load it, so the trace is the artifact's own.
    Context       ctx(alloc);
    ReplayProgram program(alloc);
    load_replay_program(ctx, {blob.data(), blob.size()}, parsed.entry, cmd->registrar, cmd->user, program);
    if (!program.ok())
    {
        return describe_unrunnable(program, "the cooked", out.reason);
    }
    detail::ProgramNeeds needs;
    if (!detail::analyze_needs(ctx, *program.module, alloc, call.cancel, needs) || call.cancelled())
    {
        out.reason.append("cancelled before the program ran");
        return DiagStatus::Cancelled;
    }

    cmd->executions.fetch_add(1U, std::memory_order_relaxed);
    ReplayTrace trace(alloc);
    run_traced(program, cont::as_const_span(parsed.args), parsed.max_events, call.cancel, trace);
    if (trace.error == plan::RunError::Cancelled)
    {
        out.reason.append("cancelled while the program ran; no record was written");
        return DiagStatus::Cancelled;
    }

    ReplayRecord record(alloc);
    record.build = current_build(alloc);
    record.program_path.append(call.request->path);
    record.content_hash = program.content_hash;
    record.program      = std::move(blob);
    record.entry.append(parsed.entry);
    record.args = std::move(parsed.args);
    cont::String missing(alloc);
    detail::record_inputs(needs, record.inputs, &missing);
    record.max_events   = parsed.max_events;
    record.events_total = trace.events_total;
    record.events       = std::move(trace.events);
    record.error        = trace.error;
    record.fault_op     = trace.fault_op;
    record.results      = std::move(trace.results);
    record.cells        = std::move(trace.cells);

    cont::Array<crd::u8> file_bytes(alloc);
    encode_record(record, file_bytes);
    if (file_bytes.size() > cmd->max_record_bytes)
    {
        out.reason.append("the record is larger than the host's record limit; nothing was written");
        return DiagStatus::Oversized;
    }
    if (const DiagStatus s =
            detail::write_new_file({target.data(), target.size()}, {file_bytes.data(), file_bytes.size()}, out.reason);
        s != DiagStatus::Ok)
    {
        return s;
    }
    cmd->records_written.fetch_add(1U, std::memory_order_relaxed);

    DiagFields item(alloc);
    for (crd::u32 i = 0U; i < kReplayInputs; ++i)
    {
        const detail::ReplayInputSpec& spec = detail::replay_input(i);
        item.clear();
        item.str("kind", "input")
            .str("input", spec.name)
            .str("guarantee", spec.guarantee)
            .str("needed", detail::need_name(record.inputs[i].need))
            .str("state", replay_input_state_name(record.inputs[i].state));
        (void)out.add_item(item);
    }
    for (crd::usize i = 0U; i < record.results.size(); ++i)
    {
        item.clear();
        item.str("kind", "result").u64("index", i).i64("value", record.results[i]);
        (void)out.add_item(item);
    }

    const ReplaySite fault = replay_site(ctx, program, trace.fault);
    out.summary.str("path", call.request->path)
        .str("out", parsed.out)
        .str("entry", parsed.entry)
        .u64("args", record.args.size())
        .u64("content_hash", record.content_hash)
        .u64("record_bytes", file_bytes.size())
        .u64("max_events", record.max_events)
        .u64("events", record.events.size())
        .u64("events_total", record.events_total)
        .u64("lost", record.events_total - record.events.size())
        .str("error", plan::run_error_name(record.error))
        .u64("fault_op", record.fault_op)
        .str("fault_file", fault.file)
        .u64("fault_line", fault.line)
        .u64("fault_col", fault.col)
        .u64("results", record.results.size())
        .str("missing_inputs", cont::StringView{missing.data(), missing.size()})
        .str("replay", missing.empty() ? cont::StringView{"replayable"} : cont::StringView{"incomplete"});
    return DiagStatus::Ok;
}

// ---- replay.run -----------------------------------------------------------------------------------------------------

struct RunArgs
{
    cont::StringView program;           // empty: the record's own program
    bool             any_build = false; // build=any
};

[[nodiscard]] DiagStatus parse_run_args(cont::ConstSpan<DiagArg> args, RunArgs* out, cont::String& reason)
{
    for (const DiagArg& a : args)
    {
        if (a.name == "program")
        {
            if (!perf::diag_path_is_safe(a.value))
            {
                return bad(reason, a.name, kPathRule);
            }
            if (out != nullptr)
            {
                out->program = a.value;
            }
        }
        else if (a.name == "build")
        {
            if (a.value != "match" && a.value != "any")
            {
                return bad(reason, a.name, "must be 'match' or 'any'");
            }
            if (out != nullptr)
            {
                out->any_build = a.value == "any";
            }
        }
        else
        {
            reason.append("unknown argument '");
            reason.append(a.name);
            reason.append("'; replay.run takes program and build");
            return DiagStatus::BadArgument;
        }
    }
    return DiagStatus::Ok;
}

DiagStatus check_replay_run(void* /*context*/, cont::ConstSpan<DiagArg> args, cont::String& reason)
{
    return parse_run_args(args, nullptr, reason);
}

void add_site(DiagFields& item, const ReplaySite& site)
{
    item.str("file", site.file).u64("line", site.line).u64("col", site.col);
}

DiagStatus run_replay_run(void* context, const DiagCall& call, DiagSnapshot& out)
{
    auto* const                    cmd   = static_cast<ReplayCommands*>(context);
    crd::memory::IAllocator* const alloc = out.allocator();
    cmd->replay_runs.fetch_add(1U, std::memory_order_relaxed);

    RunArgs parsed;
    if (const DiagStatus s = parse_run_args(call.request->args, &parsed, out.reason); s != DiagStatus::Ok)
    {
        return s;
    }

    cont::Array<crd::u8> bytes(alloc);
    if (const DiagStatus s = detail::read_bounded_file(call.file, cmd->max_record_bytes, bytes, out.reason,
                                                       cmd->bytes_read, cont::StringView{"the record"});
        s != DiagStatus::Ok)
    {
        return s;
    }
    ReplayRecord record(alloc);
    if (const RecordError e = decode_record({bytes.data(), bytes.size()}, record); e != RecordError::Ok)
    {
        out.reason.append("the file is not a usable run record: ");
        out.reason.append(record_error_name(e));
        return DiagStatus::Failed;
    }

    // Compatibility, before anything runs: the build, then the inputs the program needs.
    cont::String differing(alloc);
    const bool   same = same_build(record.build, current_build(alloc), differing);
    if (!same && !parsed.any_build)
    {
        out.reason.append("incompatible replay: the record was made by another build (differs in ");
        out.reason.append(cont::StringView{differing.data(), differing.size()});
        out.reason.append("); pass build=any to replay it here anyway");
        return DiagStatus::Unavailable;
    }
    cont::String missing(alloc);
    for (crd::u32 i = 0U; i < kReplayInputs; ++i)
    {
        if (record.inputs[i].state == ReplayInputState::Missing)
        {
            if (!missing.empty())
            {
                missing.push_back(',');
            }
            missing.append(replay_input_name(i));
        }
    }
    if (!missing.empty())
    {
        out.reason.append("incompatible replay: the record is missing inputs its program needs (");
        out.reason.append(cont::StringView{missing.data(), missing.size()});
        out.reason.append(")");
        return DiagStatus::Unavailable;
    }

    // The record's own artifact, never the checkout's file; its content must be the hash it was recorded with.
    const cont::StringView entry{record.entry.data(), record.entry.size()};
    Context                rctx(alloc);
    ReplayProgram          recorded(alloc);
    load_replay_program(rctx, {record.program.data(), record.program.size()}, entry, cmd->registrar, cmd->user,
                        recorded);
    if (recorded.module == nullptr)
    {
        return describe_unrunnable(recorded, "the record's", out.reason);
    }
    if (recorded.content_hash != record.content_hash ||
        stable_hash(rctx, *recorded.module, alloc) != record.content_hash)
    {
        out.reason.append("the record's program does not match the content hash it was recorded with");
        return DiagStatus::Failed;
    }
    if (!recorded.compiled.ok())
    {
        return describe_unrunnable(recorded, "the record's", out.reason);
    }

    // An explicit other program (an edited checkout) replays the same inputs.
    Context              actx(alloc);
    ReplayProgram        against(alloc);
    const ReplayProgram* replayed     = &recorded;
    const Context*       replayed_ctx = &rctx;
    if (!parsed.program.empty())
    {
        cont::String file(alloc);
        join(call.root, parsed.program, file);
        Context               src(alloc);
        cont::Array<crd::u8>  pbytes(alloc);
        detail::LoadedProgram loaded;
        if (const DiagStatus s = detail::load_program_file({file.data(), file.size()}, parsed.program, call.cancel,
                                                           cmd->max_program_bytes, cmd->registrar, cmd->user, src,
                                                           pbytes, loaded, out.reason, cmd->bytes_read);
            s != DiagStatus::Ok)
        {
            return s;
        }
        cont::Array<crd::u8> blob(alloc);
        if (const DiagStatus s = cook_artifact(src, loaded, pbytes, parsed.program, blob, out.reason);
            s != DiagStatus::Ok)
        {
            return s;
        }
        load_replay_program(actx, {blob.data(), blob.size()}, entry, cmd->registrar, cmd->user, against);
        if (!against.ok())
        {
            return describe_unrunnable(against, "the given", out.reason);
        }
        replayed     = &against;
        replayed_ctx = &actx;
    }
    if (call.cancelled())
    {
        out.reason.append("cancelled before the program ran");
        return DiagStatus::Cancelled;
    }

    cmd->executions.fetch_add(1U, std::memory_order_relaxed);
    ReplayTrace trace(alloc);
    run_traced(*replayed, cont::as_const_span(record.args), record.max_events, call.cancel, trace);
    if (trace.error == plan::RunError::Cancelled && call.cancelled())
    {
        out.reason.append("cancelled while the program ran");
        return DiagStatus::Cancelled;
    }
    const Divergence d = first_divergence(record, trace);

    DiagFields item(alloc);
    if (d.kind != DivergenceKind::None)
    {
        item.clear();
        item.str("kind", "divergence")
            .str("divergence", divergence_kind_name(d.kind))
            .u64("index", d.index)
            .u64("value_index", d.value_index)
            .boolean("count", d.count)
            .i64("recorded", d.recorded)
            .i64("observed", d.observed)
            .u64("recorded_op", d.recorded_op)
            .u64("observed_op", d.observed_op);
        add_site(item, replay_site(*replayed_ctx, *replayed, d.site));
        (void)out.add_item(item);
    }
    item.clear();
    item.str("kind", "outcome")
        .str("run", "recorded")
        .str("error", plan::run_error_name(record.error))
        .u64("op", record.fault_op);
    add_site(item, replay_site_of_op(rctx, recorded, record.fault_op));
    (void)out.add_item(item);
    item.clear();
    item.str("kind", "outcome")
        .str("run", "replayed")
        .str("error", plan::run_error_name(trace.error))
        .u64("op", trace.fault_op);
    add_site(item, replay_site(*replayed_ctx, *replayed, trace.fault));
    (void)out.add_item(item);

    const crd::usize verified = record.events.size() < trace.events.size() ? record.events.size() : trace.events.size();
    out.summary.str("path", call.request->path)
        .str("program_source", parsed.program.empty() ? cont::StringView{"record"} : cont::StringView{"argument"})
        .str("program",
             parsed.program.empty() ? cont::StringView{record.program_path.data(), record.program_path.size()}
                                    : parsed.program)
        .u64("asset", record.asset)
        .u64("generation", record.generation)
        .u64("recorded_hash", record.content_hash)
        .u64("replayed_hash", replayed->content_hash)
        .boolean("program_matches", replayed->content_hash == record.content_hash)
        .str("build", same ? cont::StringView{"same"} : cont::StringView{"differs"})
        .str("build_differs", cont::StringView{differing.data(), differing.size()})
        .str("entry", entry)
        .u64("args", record.args.size())
        .u64("max_events", record.max_events)
        .u64("recorded_events", record.events_total)
        .u64("replayed_events", trace.events_total)
        .u64("verified_events", verified)
        .str("result", d.kind == DivergenceKind::None ? cont::StringView{"reproduced"} : cont::StringView{"diverged"})
        .str("divergence", divergence_kind_name(d.kind));
    return DiagStatus::Ok;
}
} // namespace

bool register_replay_record(perf::DiagCommandService& service, ReplayCommands& commands)
{
    perf::DiagCommandSpec spec;
    spec.name       = kReplayRecordCommand;
    spec.owner      = containers::StringView{"ceir"};
    spec.summary    = containers::StringView{"run an authored program once and write its inputs and trace as a record"};
    spec.authority  = perf::DiagAuthority::Execute;
    spec.also       = perf::DiagAuthority::Record;
    spec.takes_path = true;
    return service.register_command(spec, &run_replay_record, &commands, &check_replay_record);
}

bool register_replay_run(perf::DiagCommandService& service, ReplayCommands& commands)
{
    perf::DiagCommandSpec spec;
    spec.name       = kReplayRunCommand;
    spec.owner      = containers::StringView{"ceir"};
    spec.summary    = containers::StringView{"replay a run record and report the first divergence"};
    spec.authority  = perf::DiagAuthority::Execute;
    spec.takes_path = true;
    return service.register_command(spec, &run_replay_run, &commands, &check_replay_run);
}
} // namespace crd::ceir::cook
