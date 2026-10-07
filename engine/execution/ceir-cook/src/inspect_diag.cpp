#include <crd/ceir/cook/inspect_diag.hpp>

#include "bounded_file.hpp"
#include "diag_args.hpp"

#include <crd/ceir/cook/inspect_host.hpp>
#include <crd/ceir/cook/program_cook.hpp> // cook_error_name
#include <crd/ceir/inspect.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/diag_commands.hpp>

namespace crd::ceir::cook
{
namespace
{
namespace cont = crd::containers;
namespace insp = crd::ceir::inspect;

using crd::perf::DiagArg;
using crd::perf::DiagCall;
using crd::perf::DiagFields;
using crd::perf::DiagSnapshot;
using crd::perf::DiagStatus;

using detail::for_each_item;
using detail::parse_i64;
using detail::parse_u64;

[[nodiscard]] bool parse_line(cont::StringView text, crd::u32& out) noexcept
{
    crd::u64 v = 0U;
    if (!parse_u64(text, 0xFFFFFFFFULL, v) || v == 0U)
    {
        return false;
    }
    out = static_cast<crd::u32>(v);
    return true;
}

[[nodiscard]] bool valid_entry(cont::StringView name) noexcept
{
    return detail::valid_entry_name(name, kInspectMaxEntryBytes);
}

// The parsed script (the argument check parses with no Parsed: it validates and keeps nothing).
struct Parsed
{
    explicit Parsed(crd::memory::IAllocator* alloc) : args(alloc), breaks(alloc), watches(alloc), steps(alloc) {}

    cont::StringView          entry{"main"};
    cont::Array<crd::i64>     args;
    cont::Array<crd::u32>     breaks;
    cont::Array<crd::u32>     watches;
    cont::Array<ScriptAction> steps;
    crd::u32                  max_stops = kScriptDefaultMaxStops;
};

[[nodiscard]] DiagStatus bad(cont::String& reason, cont::StringView name, cont::StringView what)
{
    reason.append("the argument '");
    reason.append(name);
    reason.append("' ");
    reason.append(what);
    return DiagStatus::BadArgument;
}

// Parse every argument; `out` null validates only. Refuses before any file is opened.
[[nodiscard]] DiagStatus parse_args(const ProgramInspectCommand& command, cont::ConstSpan<DiagArg> args, Parsed* out,
                                    cont::String& reason)
{
    crd::u32 max_stops =
        command.max_stops_limit < kScriptDefaultMaxStops ? command.max_stops_limit : kScriptDefaultMaxStops;
    for (const DiagArg& a : args)
    {
        if (a.name == "entry")
        {
            if (!valid_entry(a.value))
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
            const bool ok = for_each_item(a.value,
                                          [out](cont::StringView item)
                                          {
                                              crd::i64 v = 0;
                                              if (!parse_i64(item, v))
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
                return bad(reason, a.name, "must be comma-separated i64 values");
            }
        }
        else if (a.name == "breaks" || a.name == "watches")
        {
            cont::Array<crd::u32>* into = nullptr;
            if (out != nullptr)
            {
                into = a.name == "breaks" ? &out->breaks : &out->watches;
            }
            const bool ok = for_each_item(a.value,
                                          [into](cont::StringView item)
                                          {
                                              crd::u32 line = 0U;
                                              if (!parse_line(item, line))
                                              {
                                                  return false;
                                              }
                                              if (into != nullptr)
                                              {
                                                  into->push_back(line);
                                              }
                                              return true;
                                          });
            if (!ok)
            {
                return bad(reason, a.name, "must be comma-separated 1-based line numbers");
            }
        }
        else if (a.name == "steps")
        {
            const bool ok = for_each_item(a.value,
                                          [out](cont::StringView item)
                                          {
                                              ScriptAction action = ScriptAction::Continue;
                                              if (!parse_script_action(item, action))
                                              {
                                                  return false;
                                              }
                                              if (out != nullptr)
                                              {
                                                  out->steps.push_back(action);
                                              }
                                              return true;
                                          });
            if (!ok)
            {
                return bad(reason, a.name, "must be comma-separated actions: continue, into, over, out, cancel");
            }
        }
        else if (a.name == "max_stops")
        {
            crd::u64 v = 0U;
            if (!parse_u64(a.value, command.max_stops_limit, v) || v == 0U)
            {
                reason.append("the argument 'max_stops' must be 1 to ");
                detail::append_decimal(reason, command.max_stops_limit);
                return DiagStatus::BadArgument;
            }
            max_stops = static_cast<crd::u32>(v);
        }
        else
        {
            reason.append("unknown argument '");
            reason.append(a.name);
            reason.append("'; program.inspect takes entry, args, breaks, watches, steps and max_stops");
            return DiagStatus::BadArgument;
        }
    }
    if (out != nullptr)
    {
        out->max_stops = max_stops;
    }
    return DiagStatus::Ok;
}

DiagStatus check_program_inspect(void* context, cont::ConstSpan<DiagArg> args, cont::String& reason)
{
    return parse_args(*static_cast<const ProgramInspectCommand*>(context), args, nullptr, reason);
}

void describe_load_failure(const HostLoadResult& lr, cont::StringView path, cont::String& reason)
{
    reason.append("the program did not load: ");
    reason.append(host_load_name(lr.status));
    if (lr.status == HostLoad::CookFailed)
    {
        reason.append(" (");
        reason.append(cook_error_name(lr.cook_error));
        reason.append(" at ");
        reason.append(path);
        reason.push_back(':');
        detail::append_decimal(reason, lr.cook_site.line);
        reason.push_back(':');
        detail::append_decimal(reason, lr.cook_site.col);
        reason.push_back(')');
    }
    if (lr.status == HostLoad::CompileFailed)
    {
        reason.append(" (");
        reason.append(plan::compile_error_name(lr.compile_error));
        reason.push_back(')');
    }
}

void write_answer(const InspectHost& host, const InspectReport& report, const Parsed& parsed, const DiagCall& call,
                  DiagSnapshot& out)
{
    DiagFields item(out.allocator());
    for (const ScriptBind& b : report.binds)
    {
        item.clear();
        item.str("kind", "breakpoint")
            .u64("line", b.line)
            .str("status", insp::bind_status_name(b.status))
            .u64("sites", b.sites);
        (void)out.add_item(item);
    }
    for (const ScriptStop& s : report.stops)
    {
        item.clear();
        item.str("kind", "stop")
            .u64("sequence", s.sequence)
            .str("reason", insp::stop_reason_name(s.reason))
            .str("file", s.line != 0U ? host.file_path(s.file_id) : cont::StringView{})
            .u64("line", s.line)
            .u64("col", s.col)
            .u64("depth", s.depth)
            .u64("op", s.op)
            .str("action", script_action_name(s.action));
        (void)out.add_item(item);
        // One item per watched value, so a stop with many watches never clips a value out of its stop's item.
        for (crd::usize k = 0U; k < parsed.watches.size(); ++k)
        {
            const ScriptValue& v = report.values[s.first_value + k];
            item.clear();
            item.str("kind", "value").u64("stop", s.sequence).u64("line", v.line);
            if (!v.answered)
            {
                item.str("status", "unanswered");
            }
            else
            {
                item.str("status", insp::value_status_name(v.status));
                if (v.status != insp::ValueStatus::NoSuchValue)
                {
                    item.str("type", report.type_text(v)).boolean("unit", v.has_unit);
                }
                if (v.status == insp::ValueStatus::Available)
                {
                    item.i64("value", v.bits);
                }
            }
            (void)out.add_item(item);
        }
    }
    for (crd::usize i = 0U; i < report.results.size(); ++i)
    {
        item.clear();
        item.str("kind", "result").u64("index", i).i64("value", report.results[i]);
        (void)out.add_item(item);
    }

    out.summary.str("path", call.request->path)
        .str("entry", parsed.entry)
        .u64("generation", report.generation)
        .u64("breakpoints", report.binds.size())
        .u64("stops", report.stops.size())
        .u64("max_stops", parsed.max_stops)
        .boolean("truncated", report.truncated)
        .str("outcome", script_outcome_name(report.outcome))
        .str("error", plan::run_error_name(report.error))
        .u64("fault_line", report.fault_line)
        .u64("fault_col", report.fault_col)
        .u64("results", report.results.size());
}

DiagStatus run_program_inspect(void* context, const DiagCall& call, DiagSnapshot& out)
{
    auto* const                    command = static_cast<ProgramInspectCommand*>(context);
    crd::memory::IAllocator* const alloc   = out.allocator();
    command->runs.fetch_add(1U, std::memory_order_relaxed);

    // The service already ran the check; parsing again keeps the lists (and cannot fail differently).
    Parsed parsed(alloc);
    if (const DiagStatus s = parse_args(*command, call.request->args, &parsed, out.reason); s != DiagStatus::Ok)
    {
        return s;
    }

    cont::Array<crd::u8> bytes(alloc);
    const DiagStatus     read =
        detail::read_bounded_file(call.file, command->max_program_bytes, bytes, out.reason, command->bytes_read);
    if (read != DiagStatus::Ok)
    {
        return read;
    }
    if (call.cancelled())
    {
        out.reason.append("cancelled after the program was read");
        return DiagStatus::Cancelled;
    }

    // Cooked under the request's own relative path: breakpoints and stops are positions in that name.
    const cont::StringView path = call.request->path;
    InspectHost            host(alloc, command->registrar, command->user);
    const HostLoadResult   lr   = host.load(
        AssetId{1U}, cont::StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size()), path, parsed.entry);
    if (!lr.ok())
    {
        describe_load_failure(lr, path, out.reason);
        return DiagStatus::Failed;
    }
    command->starts.fetch_add(1U, std::memory_order_relaxed);

    InspectScript script;
    script.file      = path;
    script.args      = cont::as_const_span(parsed.args);
    script.breaks    = cont::as_const_span(parsed.breaks);
    script.watches   = cont::as_const_span(parsed.watches);
    script.actions   = cont::as_const_span(parsed.steps);
    script.max_stops = parsed.max_stops;
    script.wait_ms   = command->wait_ms;
    script.cancel    = call.cancel;
    script.on_stop   = command->on_stop;
    script.user      = command->stop_user;
    InspectReport report(alloc);
    run_inspect_script(host, script, report);

    switch (report.outcome) // no default: every outcome is answered
    {
    case ScriptOutcome::NotStarted:
        out.reason.append("the program did not start: ");
        out.reason.append(insp::refusal_name(report.refusal));
        return DiagStatus::Failed;
    case ScriptOutcome::Unfinished:
        out.reason.append("the run did not finish within the host's wait bound: ");
        out.reason.append(insp::refusal_name(report.refusal));
        return DiagStatus::Failed;
    case ScriptOutcome::CallerCancelled:
        out.reason.append("cancelled by the caller after ");
        detail::append_decimal(out.reason, report.stops.size());
        out.reason.append(" stops");
        return DiagStatus::Cancelled;
    case ScriptOutcome::Finished:
    case ScriptOutcome::Cancelled:
    case ScriptOutcome::Error:
        break;
    }
    write_answer(host, report, parsed, call, out);
    return DiagStatus::Ok;
}
} // namespace

bool register_program_inspect(perf::DiagCommandService& service, ProgramInspectCommand& command)
{
    perf::DiagCommandSpec spec;
    spec.name       = kProgramInspectCommand;
    spec.owner      = containers::StringView{"ceir"};
    spec.summary    = containers::StringView{"run an authored program to a script of breakpoints, watches and steps"};
    spec.authority  = perf::DiagAuthority::Execute;
    spec.takes_path = true;
    return service.register_command(spec, &run_program_inspect, &command, &check_program_inspect);
}
} // namespace crd::ceir::cook
