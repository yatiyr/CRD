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
#include <crd/ceir/input.hpp> // HostClock, InputRouter, SeededInputs
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/time/clocks.hpp> // MonotonicClock: the live wall domain

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
    explicit RecordArgs(crd::memory::IAllocator* alloc) : args(alloc), events(alloc) {}

    cont::StringView      entry{"main"};
    cont::Array<crd::i64> args;
    cont::StringView      out;
    crd::u32              max_events = kReplayDefaultMaxEvents;
    bool                  host       = false; // executor=host
    crd::u32              jobs       = 8U;
    crd::u64              sub_fuel   = crd::u64{1} << 20U;
    bool                  have_seed  = false; // seed=: the run's host random streams (none without it)
    crd::u64              seed       = 0U;
    HostClockSpec         clock;              // clock=, sim_time=, sim_step=: the run's time domains
    bool                  have_events = false; // events=: the run's input event queue (none without it)
    cont::Array<crd::i64> events;              // packed, in delivery order

    bool             device        = false; // executor=device
    bool             have_envelope = false;
    DeviceEnvelope   envelope;   // envelope=: exact or ulp:N
    cont::StringView kernel_dir; // kernel_dir=: the folder of the kernels' CKIR texts
    cont::StringView buffers;    // buffers=: the initial contents' files, one per declaration

    [[nodiscard]] HostEventsSpec events_spec() const noexcept
    {
        return HostEventsSpec{have_events, cont::as_const_span(events)};
    }
};

constexpr crd::u64 kMaxSubFuel = crd::u64{1} << 32U;

// The host executor's answer as the service's status.
[[nodiscard]] DiagStatus diag_status_of(HostExecutorStatus s) noexcept
{
    switch (s) // no default (-Werror=switch)
    {
    case HostExecutorStatus::Ok: return DiagStatus::Ok;
    case HostExecutorStatus::Unavailable: return DiagStatus::Unavailable;
    case HostExecutorStatus::Failed: return DiagStatus::Failed;
    case HostExecutorStatus::BadArgument: return DiagStatus::BadArgument;
    }
    return DiagStatus::Failed;
}

constexpr cont::StringView kUlpPrefix{"ulp:"};

// A device record's declared envelope: `exact` or `ulp:N` (N at most kReplayMaxUlps).
[[nodiscard]] bool parse_envelope(cont::StringView value, DeviceEnvelope& out) noexcept
{
    if (value == "exact")
    {
        out = DeviceEnvelope{DeviceEnvelopeKind::Exact, 0U};
        return true;
    }
    crd::u64 ulps = 0U;
    if (!value.starts_with(kUlpPrefix) || !detail::parse_u64(value.substr(kUlpPrefix.size()), kReplayMaxUlps, ulps))
    {
        return false;
    }
    out = DeviceEnvelope{DeviceEnvelopeKind::Ulp, static_cast<crd::u32>(ulps)};
    return true;
}

// `buffers`: 1 to kReplayMaxDeviceBuffers comma-separated safe paths.
[[nodiscard]] bool valid_buffer_files(cont::StringView value) noexcept
{
    if (value.empty())
    {
        return false;
    }
    crd::u32 count = 0U;
    return detail::for_each_item(value,
                                 [&count](cont::StringView item)
                                 {
                                     count += 1U;
                                     return count <= kReplayMaxDeviceBuffers && perf::diag_path_is_safe(item);
                                 });
}

// Parse every argument; `out` null validates only.
[[nodiscard]] DiagStatus parse_record_args(cont::ConstSpan<DiagArg> args, RecordArgs* out, cont::String& reason)
{
    bool             have_out        = false;
    bool             host            = false;
    bool             device          = false;
    bool             have_schedule   = false;
    bool             have_envelope   = false;
    bool             have_kernel_dir = false;
    bool             have_buffers    = false;
    cont::StringView run_argument;    // the first argument of a run with an entry (a device program has none)
    cont::StringView device_argument; // the first of envelope, kernel_dir and buffers
    cont::StringView requirement;
    for (const DiagArg& a : args)
    {
        if (a.name == "entry" || a.name == "args" || a.name == "max_events" || a.name == "seed" || a.name == "events" ||
            a.name == "clock" || a.name == "sim_time" || a.name == "sim_step")
        {
            run_argument = run_argument.empty() ? a.name : run_argument;
        }
        if (a.name == "envelope" || a.name == "kernel_dir" || a.name == "buffers")
        {
            device_argument = device_argument.empty() ? a.name : device_argument;
        }

        if (a.name == "envelope")
        {
            DeviceEnvelope envelope;
            if (!parse_envelope(a.value, envelope))
            {
                return bad(reason, a.name, "must be 'exact' or 'ulp:N' with N at most 16777216");
            }
            have_envelope = true;
            if (out != nullptr)
            {
                out->have_envelope = true;
                out->envelope      = envelope;
            }
        }
        else if (a.name == "kernel_dir")
        {
            if (!perf::diag_path_is_safe(a.value))
            {
                return bad(reason, a.name, kPathRule);
            }
            have_kernel_dir = true;
            if (out != nullptr)
            {
                out->kernel_dir = a.value;
            }
        }
        else if (a.name == "buffers")
        {
            if (!valid_buffer_files(a.value))
            {
                return bad(reason, a.name,
                           "must be 1 to 16 comma-separated relative paths of plain [A-Za-z0-9._-] names separated "
                           "by '/'");
            }
            have_buffers = true;
            if (out != nullptr)
            {
                out->buffers = a.value;
            }
        }
        else if (a.name == "entry")
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
        else if (a.name == "executor")
        {
            if (a.value != "plan" && a.value != "host" && a.value != "device")
            {
                return bad(reason, a.name, "must be 'plan', 'host' or 'device'");
            }
            host   = a.value == "host";
            device = a.value == "device";
            if (out != nullptr)
            {
                out->host   = host;
                out->device = device;
            }
        }
        else if (a.name == "jobs")
        {
            crd::u64 v = 0U;
            if (!detail::parse_u64(a.value, kReplayMaxHostJobs, v) || v == 0U)
            {
                return bad(reason, a.name, "must be 1 to 256");
            }
            have_schedule = true;
            if (out != nullptr)
            {
                out->jobs = static_cast<crd::u32>(v);
            }
        }
        else if (a.name == "seed")
        {
            crd::u64 v = 0U;
            if (!detail::parse_u64(a.value, ~crd::u64{0U}, v))
            {
                return bad(reason, a.name, "must be a u64");
            }
            if (out != nullptr)
            {
                out->have_seed = true;
                out->seed      = v;
            }
        }
        else if (a.name == "events")
        {
            if (!parse_events_argument(a.value, out != nullptr ? &out->events : nullptr, requirement))
            {
                return bad(reason, a.name, requirement);
            }
            if (out != nullptr)
            {
                out->have_events = true;
            }
        }
        else if (const ClockArgument c =
                     parse_clock_argument(a.name, a.value, out != nullptr ? &out->clock : nullptr, requirement);
                 c != ClockArgument::NotClock)
        {
            if (c == ClockArgument::Refused)
            {
                return bad(reason, a.name, requirement);
            }
        }
        else if (a.name == "sub_fuel")
        {
            crd::u64 v = 0U;
            if (!detail::parse_u64(a.value, kMaxSubFuel, v) || v == 0U)
            {
                return bad(reason, a.name, "must be 1 to 4294967296");
            }
            have_schedule = true;
            if (out != nullptr)
            {
                out->sub_fuel = v;
            }
        }
        else
        {
            reason.append("unknown argument '");
            reason.append(a.name);
            reason.append("'; replay.record takes out, entry, args, max_events, seed, clock, sim_time, sim_step, "
                          "events, executor, jobs, sub_fuel, envelope, kernel_dir and buffers");
            return DiagStatus::BadArgument;
        }
    }
    if (have_schedule && !host)
    {
        reason.append("the arguments 'jobs' and 'sub_fuel' are the host executor's schedule; they need executor=host");
        return DiagStatus::BadArgument;
    }
    if (!device_argument.empty() && !device)
    {
        reason.append("the argument '");
        reason.append(device_argument);
        reason.append("' describes a device program's run; 'envelope', 'kernel_dir' and 'buffers' need "
                      "executor=device");
        return DiagStatus::BadArgument;
    }
    if (device && !run_argument.empty())
    {
        reason.append("the argument '");
        reason.append(run_argument);
        reason.append("' is refused with executor=device: a device program has no entry, arguments, trace or host "
                      "inputs");
        return DiagStatus::BadArgument;
    }
    if (device && (!have_envelope || !have_kernel_dir || !have_buffers))
    {
        reason.append("replay.record executor=device needs 'envelope' (exact or ulp:N, declared and never inferred), "
                      "'kernel_dir' (the folder of the kernels' .ckir texts) and 'buffers' (one initial-contents file "
                      "per declared buffer)");
        return DiagStatus::BadArgument;
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

// Where a recorded run failed, as the answer names it (the file views a Context or string the caller keeps alive).
struct FaultSite
{
    cont::StringView file;
    crd::u32         line = 0U;
    crd::u32         col  = 0U;
};

// Encode `record` into `file_bytes` and create its file exclusively.
[[nodiscard]] DiagStatus write_record(ReplayCommands& cmd, const ReplayRecord& record, cont::StringView target,
                                      cont::Array<crd::u8>& file_bytes, cont::String& reason)
{
    encode_record(record, file_bytes);
    if (file_bytes.size() > cmd.max_record_bytes)
    {
        reason.append("the record is larger than the host's record limit; nothing was written");
        return DiagStatus::Oversized;
    }
    if (const DiagStatus s = detail::write_new_file(target, {file_bytes.data(), file_bytes.size()}, reason);
        s != DiagStatus::Ok)
    {
        return s;
    }
    cmd.records_written.fetch_add(1U, std::memory_order_relaxed);
    return DiagStatus::Ok;
}

// One item per replay input: its need and state in `record`.
void add_input_items(const ReplayRecord& record, DiagFields& item, DiagSnapshot& out)
{
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
}

// Encode `record`, create its file exclusively and answer one item per input and per result and the summary.
[[nodiscard]] DiagStatus write_and_answer(ReplayCommands& cmd, const DiagCall& call, const RecordArgs& parsed,
                                          const ReplayRecord& record, cont::StringView target,
                                          cont::StringView missing, const FaultSite& fault, DiagSnapshot& out)
{
    crd::memory::IAllocator* const alloc = out.allocator();
    cont::Array<crd::u8>           file_bytes(alloc);
    if (const DiagStatus s = write_record(cmd, record, target, file_bytes, out.reason); s != DiagStatus::Ok)
    {
        return s;
    }

    DiagFields item(alloc);
    add_input_items(record, item, out);
    for (crd::usize i = 0U; i < record.results.size(); ++i)
    {
        item.clear();
        item.str("kind", "result").u64("index", i).i64("value", record.results[i]);
        (void)out.add_item(item);
    }

    const bool host = record.executor == ReplayExecutorKind::Host;
    out.summary.str("path", call.request->path).str("executor", replay_executor_name(record.executor));
    if (host)
    {
        out.summary.u64("jobs", record.host_jobs).u64("sub_fuel", record.host_sub_fuel);
    }
    out.summary.str("out", parsed.out)
        .str("entry", parsed.entry)
        .u64("args", record.args.size())
        .u64("content_hash", record.content_hash)
        .u64("record_bytes", file_bytes.size())
        .u64("max_events", record.max_events)
        .u64("events", record.events.size())
        .u64("events_total", record.events_total)
        .u64("lost", record.events_total - record.events.size())
        .str("error", host ? exec::exec_error_name(record.host_error) : plan::run_error_name(record.error))
        .u64("fault_op", record.fault_op)
        .str("fault_file", fault.file)
        .u64("fault_line", fault.line)
        .u64("fault_col", fault.col)
        .u64("results", record.results.size())
        .str("random_source", parsed.have_seed ? cont::StringView{"seeded"} : cont::StringView{"none"})
        .u64("seed", parsed.seed);
    detail::clock_fields(out.summary, parsed.clock);
    detail::event_fields(out.summary, parsed.events_spec());
    out.summary.u64("input_reads", record.input_reads_total)
        .str("missing_inputs", missing)
        .str("replay", missing.empty() ? cont::StringView{"replayable"} : cont::StringView{"incomplete"});
    return DiagStatus::Ok;
}

// The host executor's run of the artifact `blob`, recorded.
[[nodiscard]] DiagStatus record_on_host(ReplayCommands& cmd, const DiagCall& call, const RecordArgs& parsed,
                                        const cont::Array<crd::u8>& blob, cont::StringView target, DiagSnapshot& out)
{
    crd::memory::IAllocator* const alloc = out.allocator();
    if (call.cancelled())
    {
        out.reason.append("cancelled before the program ran");
        return DiagStatus::Cancelled;
    }
    HostRecordRequest request;
    request.blob       = {blob.data(), blob.size()};
    request.path       = call.request->path;
    request.entry      = parsed.entry;
    request.args       = cont::as_const_span(parsed.args);
    request.num_jobs   = parsed.jobs;
    request.sub_fuel   = parsed.sub_fuel;
    request.max_events = parsed.max_events;
    request.registrar  = cmd.registrar;
    request.user       = cmd.user;
    request.has_seed   = parsed.have_seed;
    request.seed       = parsed.seed;
    request.clock      = parsed.clock;
    request.events     = parsed.events_spec();
    ReplayRecord    record(alloc);
    OwnedReplaySite fault(alloc);
    cont::String    missing(alloc);
    if (const HostExecutorStatus s = cmd.host->record(request, record, fault, missing, out.reason);
        s != HostExecutorStatus::Ok)
    {
        return diag_status_of(s);
    }
    cmd.executions.fetch_add(1U, std::memory_order_relaxed);
    const FaultSite site{cont::StringView{fault.file.data(), fault.file.size()}, fault.line, fault.col};
    return write_and_answer(cmd, call, parsed, record, target, cont::StringView{missing.data(), missing.size()}, site,
                            out);
}

// ---- device records -------------------------------------------------------------------------------------------------

// The device executor's answer as the service's status.
[[nodiscard]] DiagStatus diag_status_of(DeviceReplayStatus s) noexcept
{
    switch (s) // no default (-Werror=switch)
    {
    case DeviceReplayStatus::Ok: return DiagStatus::Ok;
    case DeviceReplayStatus::BadRequest: return DiagStatus::BadArgument;
    case DeviceReplayStatus::WrongExecutor:
    case DeviceReplayStatus::OtherBuild:
    case DeviceReplayStatus::OtherAdapter:
    case DeviceReplayStatus::MissingInputs: return DiagStatus::Unavailable;
    case DeviceReplayStatus::NotLoaded:
    case DeviceReplayStatus::Unsupported:
    case DeviceReplayStatus::ContentMismatch:
    case DeviceReplayStatus::DeviceFailed: return DiagStatus::Failed;
    }
    return DiagStatus::Failed;
}

// A device refusal's sentence: its status name and the library's detail.
[[nodiscard]] DiagStatus refuse_device(DeviceReplayStatus s, cont::StringView detail, cont::String& reason)
{
    const DiagStatus status = diag_status_of(s);
    if (status == DiagStatus::Unavailable)
    {
        reason.append("incompatible replay: ");
    }
    reason.append(device_replay_status_name(s));
    if (!detail.empty())
    {
        reason.append(": ");
        reason.append(detail);
    }
    return status;
}

// Read the file `relative` under the root, bounded by `max_bytes` before a byte is read; a refusal names `relative`.
[[nodiscard]] DiagStatus read_root_file(ReplayCommands& cmd, const DiagCall& call, cont::StringView relative,
                                        crd::u64 max_bytes, cont::Array<crd::u8>& out, cont::String& reason)
{
    cont::String file(out.allocator());
    join(call.root, relative, file);
    return detail::read_bounded_file({file.data(), file.size()}, max_bytes, out, reason, cmd.bytes_read, relative);
}

// The CKIR text of kernel @`symbol`: the file `<kernel_dir>/<symbol>.ckir` under the root (`relative` gets its path).
[[nodiscard]] DiagStatus read_kernel(ReplayCommands& cmd, const DiagCall& call, cont::StringView kernel_dir,
                                     cont::StringView symbol, cont::String& relative, cont::String& text,
                                     cont::String& reason)
{
    relative.clear();
    relative.append(kernel_dir);
    relative.push_back('/');
    relative.append(symbol);
    relative.append(".ckir");
    if (!perf::diag_path_is_safe(cont::StringView{relative.data(), relative.size()}))
    {
        reason.append("kernel @");
        reason.append(symbol);
        reason.append(" is not a plain [A-Za-z0-9._-] file name, so it has no file in 'kernel_dir'");
        return DiagStatus::Failed;
    }
    cont::Array<crd::u8> bytes(text.allocator());
    if (const DiagStatus s = read_root_file(cmd, call, cont::StringView{relative.data(), relative.size()},
                                            kReplayMaxKernelBytes, bytes, reason);
        s != DiagStatus::Ok)
    {
        return s;
    }
    text.clear();
    for (const crd::u8 b : bytes)
    {
        text.push_back(static_cast<char>(b));
    }
    return DiagStatus::Ok;
}

// The initial contents of every declared buffer: each file of `buffers` (one per declaration, in order) read as
// little-endian 32-bit elements, at least one per file and kReplayMaxDeviceWords over all, every file bounded by what
// is left of that budget before a byte of it is read.
[[nodiscard]] DiagStatus read_buffers(ReplayCommands& cmd, const DiagCall& call, cont::StringView buffers,
                                      crd::u32 declared, cont::Array<cont::Array<crd::u32>>& out, cont::String& reason)
{
    crd::memory::IAllocator* const alloc = out.allocator();
    crd::u32                       files = 0U;
    (void)detail::for_each_item(buffers,
                                [&files](cont::StringView /*file*/)
                                {
                                    ++files;
                                    return true;
                                });
    if (files != declared)
    {
        reason.append("the program declares ");
        detail::append_decimal(reason, declared);
        reason.append(" buffers and 'buffers' names ");
        detail::append_decimal(reason, files);
        reason.append(" files; it names one initial-contents file per declared buffer, in declaration order");
        return DiagStatus::BadArgument;
    }
    crd::u64   words  = 0U;
    DiagStatus status = DiagStatus::Ok;
    (void)detail::for_each_item(
        buffers,
        [&](cont::StringView file)
        {
            cont::Array<crd::u8> bytes(alloc);
            status = read_root_file(cmd, call, file, (kReplayMaxDeviceWords - words) * 4U, bytes, reason);
            if (status != DiagStatus::Ok)
            {
                return false;
            }
            if (bytes.empty() || bytes.size() % 4U != 0U)
            {
                reason.append(file);
                reason.append(" holds ");
                detail::append_decimal(reason, bytes.size());
                reason.append(" bytes; a buffer's initial contents are at least one 4-byte element");
                status = DiagStatus::BadArgument;
                return false;
            }
            cont::Array<crd::u32> elements(alloc);
            elements.reserve(bytes.size() / 4U);
            for (crd::usize i = 0U; i < bytes.size(); i += 4U)
            {
                elements.push_back(static_cast<crd::u32>(bytes[i]) | (static_cast<crd::u32>(bytes[i + 1U]) << 8U) |
                                   (static_cast<crd::u32>(bytes[i + 2U]) << 16U) |
                                   (static_cast<crd::u32>(bytes[i + 3U]) << 24U));
            }
            words += elements.size();
            out.push_back(std::move(elements));
            return true;
        });
    return status;
}

void add_owned_site_as(DiagFields& item, const char* file, const char* line, const char* col,
                       const OwnedReplaySite& site)
{
    item.str(file, cont::StringView{site.file.data(), site.file.size()}).u64(line, site.line).u64(col, site.col);
}

// The device executor's run of the device program `blob`, recorded: its kernels' texts from `kernel_dir`, its
// buffers' initial contents from `buffers` and the declared envelope.
[[nodiscard]] DiagStatus record_on_device(ReplayCommands& cmd, const DiagCall& call, const RecordArgs& parsed,
                                          const cont::Array<crd::u8>& blob, cont::StringView target, DiagSnapshot& out)
{
    crd::memory::IAllocator* const alloc = out.allocator();
    DeviceProgramShape             shape(alloc);
    cont::String                   detail_text(alloc);
    if (const DeviceReplayStatus s =
            describe_device_program({blob.data(), blob.size()}, cmd.registrar, cmd.user, shape, detail_text);
        s != DeviceReplayStatus::Ok)
    {
        return refuse_device(s, cont::StringView{detail_text.data(), detail_text.size()}, out.reason);
    }

    // Every input file is bounded before a byte of it is read; nothing runs until all of them are.
    cont::Array<cont::String> kernel_files(alloc);
    cont::Array<cont::String> kernel_texts(alloc);
    for (const cont::String& k : shape.kernels)
    {
        cont::String relative(alloc);
        cont::String text(alloc);
        if (const DiagStatus s = read_kernel(cmd, call, parsed.kernel_dir, cont::StringView{k.data(), k.size()},
                                             relative, text, out.reason);
            s != DiagStatus::Ok)
        {
            return s;
        }
        kernel_files.push_back(std::move(relative));
        kernel_texts.push_back(std::move(text));
    }
    cont::Array<cont::Array<crd::u32>> contents(alloc);
    if (const DiagStatus s = read_buffers(cmd, call, parsed.buffers, shape.buffers, contents, out.reason);
        s != DiagStatus::Ok)
    {
        return s;
    }
    if (call.cancelled())
    {
        out.reason.append("cancelled before the program ran");
        return DiagStatus::Cancelled;
    }

    cont::Array<DeviceKernelSource> kernels(alloc);
    for (crd::usize i = 0U; i < shape.kernels.size(); ++i)
    {
        kernels.push_back(DeviceKernelSource{cont::StringView{shape.kernels[i].data(), shape.kernels[i].size()},
                                             cont::StringView{kernel_texts[i].data(), kernel_texts[i].size()}});
    }
    cont::Array<cont::ConstSpan<crd::u32>> buffers(alloc);
    crd::u64                               elements = 0U;
    for (const cont::Array<crd::u32>& c : contents)
    {
        buffers.push_back(cont::ConstSpan<crd::u32>{c.data(), c.size()});
        elements += c.size();
    }
    DeviceRecordRequest request;
    request.blob     = {blob.data(), blob.size()};
    request.path     = call.request->path;
    request.kernels  = cont::as_const_span(kernels);
    request.buffers  = cont::as_const_span(buffers);
    request.envelope = parsed.envelope;
    ReplayRecord             record(alloc);
    OwnedReplaySite          fault(alloc);
    const DeviceReplayStatus s =
        record_device_run(request, *cmd.device, cmd.registrar, cmd.user, record, detail_text, &fault);
    if (s == DeviceReplayStatus::Ok || s == DeviceReplayStatus::DeviceFailed)
    {
        cmd.executions.fetch_add(1U, std::memory_order_relaxed);
    }
    if (s != DeviceReplayStatus::Ok)
    {
        return refuse_device(s, cont::StringView{detail_text.data(), detail_text.size()}, out.reason);
    }

    cont::Array<crd::u8> file_bytes(alloc);
    if (const DiagStatus w = write_record(cmd, record, target, file_bytes, out.reason); w != DiagStatus::Ok)
    {
        return w;
    }
    DiagFields item(alloc);
    add_input_items(record, item, out);
    for (crd::usize i = 0U; i < record.device_kernels.size(); ++i)
    {
        const DeviceKernelRecord& k = record.device_kernels[i];
        item.clear();
        item.str("kind", "kernel")
            .str("symbol", cont::StringView{k.symbol.data(), k.symbol.size()})
            .str("file", cont::StringView{kernel_files[i].data(), kernel_files[i].size()})
            .u64("bytes", k.ckir.size())
            .u64("hash", k.hash);
        (void)out.add_item(item);
    }
    crd::u64 written = 0U;
    for (crd::usize i = 0U; i < record.device_buffers.size(); ++i)
    {
        const DeviceBufferRecord& b = record.device_buffers[i];
        written += b.written ? 1U : 0U;
        item.clear();
        item.str("kind", "buffer")
            .u64("index", i)
            .str("element", device_element_name(b.element))
            .u64("elements", b.initial.size())
            .boolean("written", b.written);
        (void)out.add_item(item);
    }

    cont::String               missing(alloc);
    const bool                 complete = record_missing_inputs(record, missing);
    const DeviceAdapterRecord& adapter  = record.device_adapter;
    out.summary.str("path", call.request->path)
        .str("executor", replay_executor_name(record.executor))
        .str("out", parsed.out)
        .u64("content_hash", record.content_hash)
        .u64("record_bytes", file_bytes.size())
        .str("backend", cont::StringView{adapter.backend.data(), adapter.backend.size()})
        .str("adapter", cont::StringView{adapter.name.data(), adapter.name.size()})
        .u64("vendor", adapter.vendor)
        .u64("device", adapter.device)
        .u64("driver", adapter.driver)
        .u64("api", adapter.api)
        .str("envelope", device_envelope_name(record.device_envelope.kind))
        .u64("ulps", record.device_envelope.ulps)
        .u64("kernels", record.device_kernels.size())
        .u64("buffers", record.device_buffers.size())
        .u64("elements", elements)
        .u64("written_buffers", written)
        .u64("error", record.device_error)
        .u64("fault_op", record.fault_op);
    add_owned_site_as(out.summary, "fault_file", "fault_line", "fault_col", fault);
    out.summary.str("missing_inputs", cont::StringView{missing.data(), missing.size()})
        .str("replay", complete ? cont::StringView{"replayable"} : cont::StringView{"incomplete"});
    return DiagStatus::Ok;
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
    if (parsed.host && (cmd->host == nullptr || cmd->host->record == nullptr))
    {
        out.reason.append("this host binds no host executor; replay.record executor=host is unavailable here");
        return DiagStatus::Unavailable;
    }
    if (parsed.device && cmd->device == nullptr)
    {
        out.reason.append("this host binds no device executor; replay.record executor=device is unavailable here");
        return DiagStatus::Unavailable;
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
    if (parsed.host)
    {
        return record_on_host(*cmd, call, parsed, blob, {target.data(), target.size()}, out);
    }
    if (parsed.device)
    {
        return record_on_device(*cmd, call, parsed, blob, {target.data(), target.size()}, out);
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
    // The host's random streams (seeded, or none), its clock and its event queue, every read kept in the trace
    // through the recorder.
    input::SeededInputs seeded(parsed.seed, alloc);
    input::HostClock    clock;
    apply_clock(parsed.clock, clock);
    input::HostEvents events(alloc);
    apply_events(parsed.events_spec(), events);
    input::InputRouter host_inputs;
    host_inputs.route(input::InputKind::Random, parsed.have_seed ? seeded.source() : nullptr);
    host_inputs.route(input::InputKind::Clock, clock.source());
    host_inputs.route(input::InputKind::TimeStep, clock.source());
    host_inputs.route(input::InputKind::Event, events.source());
    InputRecorder inputs(trace, host_inputs.source());
    run_traced(program, cont::as_const_span(parsed.args), parsed.max_events, call.cancel, trace, inputs.source());
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
    const bool   held = trace.input_reads_total <= kReplayMaxInputReads; // every read is in the record
    detail::record_inputs(needs, ReplayExecutorKind::Plan, held, record.inputs, &missing);
    record.input_reads_total = trace.input_reads_total;
    record.input_reads       = std::move(trace.input_reads);
    record.max_events   = parsed.max_events;
    record.events_total = trace.events_total;
    record.events       = std::move(trace.events);
    record.error        = trace.error;
    record.fault_op     = trace.fault_op;
    record.results      = std::move(trace.results);
    record.cells        = std::move(trace.cells);

    const ReplaySite fault = replay_site(ctx, program, trace.fault);
    return write_and_answer(*cmd, call, parsed, record, {target.data(), target.size()},
                            cont::StringView{missing.data(), missing.size()},
                            FaultSite{fault.file, fault.line, fault.col}, out);
}

// ---- replay.run -----------------------------------------------------------------------------------------------------

struct RunArgs
{
    cont::StringView program;           // empty: the record's own program
    bool             any_build = false; // build=any
    crd::u32         jobs      = 0U;    // a host record's job split for this replay (0: the recorded one)
    cont::StringView kernel_dir;        // a device record's kernels from this folder (empty: the record's own)
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
        else if (a.name == "jobs")
        {
            crd::u64 v = 0U;
            if (!detail::parse_u64(a.value, kReplayMaxHostJobs, v) || v == 0U)
            {
                return bad(reason, a.name, "must be 1 to 256");
            }
            if (out != nullptr)
            {
                out->jobs = static_cast<crd::u32>(v);
            }
        }
        else if (a.name == "kernel_dir")
        {
            if (!perf::diag_path_is_safe(a.value))
            {
                return bad(reason, a.name, kPathRule);
            }
            if (out != nullptr)
            {
                out->kernel_dir = a.value;
            }
        }
        else
        {
            reason.append("unknown argument '");
            reason.append(a.name);
            reason.append("'; replay.run takes program, build, jobs and kernel_dir");
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

void add_owned_site(DiagFields& item, const OwnedReplaySite& site)
{
    item.str("file", cont::StringView{site.file.data(), site.file.size()})
        .u64("line", site.line)
        .u64("col", site.col);
}

void add_divergence(DiagFields& item, const Divergence& d)
{
    item.str("kind", "divergence")
        .str("divergence", divergence_kind_name(d.kind))
        .u64("index", d.index)
        .u64("value_index", d.value_index)
        .boolean("count", d.count)
        .i64("recorded", d.recorded)
        .i64("observed", d.observed)
        .u64("recorded_op", d.recorded_op)
        .u64("observed_op", d.observed_op);
    if (d.kind == DivergenceKind::Input && !d.count)
    {
        item.str("recorded_input", input::input_kind_name(d.recorded_input))
            .str("observed_input", input::input_kind_name(d.observed_input));
    }
}

// The artifact of the explicit other program `path` (an edited checkout), bounded before a byte is read.
[[nodiscard]] DiagStatus cook_other_program(ReplayCommands& cmd, const DiagCall& call, cont::StringView path,
                                            cont::Array<crd::u8>& blob, cont::String& reason)
{
    crd::memory::IAllocator* const alloc = blob.allocator();
    cont::String                   file(alloc);
    join(call.root, path, file);
    Context               src(alloc);
    cont::Array<crd::u8>  bytes(alloc);
    detail::LoadedProgram loaded;
    if (const DiagStatus s =
            detail::load_program_file({file.data(), file.size()}, path, call.cancel, cmd.max_program_bytes,
                                      cmd.registrar, cmd.user, src, bytes, loaded, reason, cmd.bytes_read);
        s != DiagStatus::Ok)
    {
        return s;
    }
    return cook_artifact(src, loaded, bytes, path, blob, reason);
}

// A host record's replay on the host executor (the record's executor, build and inputs were already checked).
[[nodiscard]] DiagStatus replay_on_host(ReplayCommands& cmd, const DiagCall& call, const RunArgs& parsed,
                                        const ReplayRecord& record, bool same, cont::StringView differing,
                                        DiagSnapshot& out)
{
    crd::memory::IAllocator* const alloc = out.allocator();
    cont::Array<crd::u8>           against(alloc);
    if (!parsed.program.empty())
    {
        if (const DiagStatus s = cook_other_program(cmd, call, parsed.program, against, out.reason);
            s != DiagStatus::Ok)
        {
            return s;
        }
    }
    if (call.cancelled())
    {
        out.reason.append("cancelled before the program ran");
        return DiagStatus::Cancelled;
    }

    HostReplayRequest request;
    request.record    = &record;
    request.against   = {against.data(), against.size()};
    request.any_build = parsed.any_build;
    request.num_jobs  = parsed.jobs;
    request.registrar = cmd.registrar;
    request.user      = cmd.user;
    HostReplayAnswer answer(alloc);
    if (const HostExecutorStatus s = cmd.host->replay(request, answer, out.reason); s != HostExecutorStatus::Ok)
    {
        return diag_status_of(s);
    }
    cmd.executions.fetch_add(1U, std::memory_order_relaxed);
    const Divergence& d = answer.divergence;

    DiagFields item(alloc);
    if (d.kind != DivergenceKind::None)
    {
        item.clear();
        add_divergence(item, d);
        add_owned_site(item, answer.site);
        (void)out.add_item(item);
    }
    item.clear();
    item.str("kind", "outcome")
        .str("run", "recorded")
        .str("error", exec::exec_error_name(record.host_error))
        .u64("op", record.fault_op);
    add_owned_site(item, answer.recorded_fault);
    (void)out.add_item(item);
    item.clear();
    item.str("kind", "outcome")
        .str("run", "replayed")
        .str("error", exec::exec_error_name(answer.trace.host_error))
        .u64("op", answer.trace.fault_op);
    add_owned_site(item, answer.fault);
    (void)out.add_item(item);

    const crd::usize verified =
        record.events.size() < answer.trace.events.size() ? record.events.size() : answer.trace.events.size();
    out.summary.str("path", call.request->path)
        .str("executor", replay_executor_name(record.executor))
        .u64("recorded_jobs", record.host_jobs)
        .u64("jobs", answer.num_jobs)
        .u64("sub_fuel", record.host_sub_fuel)
        .str("program_source", parsed.program.empty() ? cont::StringView{"record"} : cont::StringView{"argument"})
        .str("program",
             parsed.program.empty() ? cont::StringView{record.program_path.data(), record.program_path.size()}
                                    : parsed.program)
        .u64("asset", record.asset)
        .u64("generation", record.generation)
        .u64("recorded_hash", record.content_hash)
        .u64("replayed_hash", answer.replayed_hash)
        .boolean("program_matches", answer.replayed_hash == record.content_hash)
        .str("build", same ? cont::StringView{"same"} : cont::StringView{"differs"})
        .str("build_differs", differing)
        .str("entry", cont::StringView{record.entry.data(), record.entry.size()})
        .u64("args", record.args.size())
        .u64("max_events", record.max_events)
        .u64("recorded_events", record.events_total)
        .u64("replayed_events", answer.trace.events_total)
        .u64("verified_events", verified)
        .u64("recorded_input_reads", record.input_reads_total)
        .u64("replayed_input_reads", answer.trace.input_reads_total)
        .str("result", d.kind == DivergenceKind::None ? cont::StringView{"reproduced"} : cont::StringView{"diverged"})
        .str("divergence", divergence_kind_name(d.kind));
    return DiagStatus::Ok;
}

// A device record's replay on the device executor: the record's own program and kernels unless `program=` or
// `kernel_dir=` name others.
[[nodiscard]] DiagStatus replay_on_device(ReplayCommands& cmd, const DiagCall& call, const RunArgs& parsed,
                                          const ReplayRecord& record, DiagSnapshot& out)
{
    crd::memory::IAllocator* const alloc = out.allocator();
    cont::Array<crd::u8>           against(alloc);
    if (!parsed.program.empty())
    {
        if (const DiagStatus s = cook_other_program(cmd, call, parsed.program, against, out.reason);
            s != DiagStatus::Ok)
        {
            return s;
        }
    }
    cont::Array<cont::String>       texts(alloc);
    cont::Array<DeviceKernelSource> kernels(alloc);
    if (!parsed.kernel_dir.empty())
    {
        for (const DeviceKernelRecord& k : record.device_kernels)
        {
            cont::String relative(alloc);
            cont::String text(alloc);
            if (const DiagStatus s = read_kernel(cmd, call, parsed.kernel_dir,
                                                 cont::StringView{k.symbol.data(), k.symbol.size()}, relative, text,
                                                 out.reason);
                s != DiagStatus::Ok)
            {
                return s;
            }
            texts.push_back(std::move(text));
        }
        for (crd::usize i = 0U; i < texts.size(); ++i)
        {
            const DeviceKernelRecord& k = record.device_kernels[i];
            kernels.push_back(DeviceKernelSource{cont::StringView{k.symbol.data(), k.symbol.size()},
                                                 cont::StringView{texts[i].data(), texts[i].size()}});
        }
    }
    if (call.cancelled())
    {
        out.reason.append("cancelled before the program ran");
        return DiagStatus::Cancelled;
    }

    DeviceReplayOptions options;
    options.against         = {against.data(), against.size()};
    options.kernels_against = cont::as_const_span(kernels);
    options.any_build       = parsed.any_build;
    DeviceReplay             replayed(alloc);
    const DeviceReplayStatus s =
        replay_device_record(record, *cmd.device, cmd.registrar, cmd.user, options, replayed);
    if (s == DeviceReplayStatus::Ok || s == DeviceReplayStatus::DeviceFailed)
    {
        cmd.executions.fetch_add(1U, std::memory_order_relaxed);
    }
    if (s != DeviceReplayStatus::Ok)
    {
        const DiagStatus status =
            refuse_device(s, cont::StringView{replayed.reason.data(), replayed.reason.size()}, out.reason);
        if (s == DeviceReplayStatus::OtherBuild && record.device_envelope.kind == DeviceEnvelopeKind::Ulp)
        {
            out.reason.append("; pass build=any to replay this ulp record here anyway");
        }
        return status;
    }

    const DeviceDivergence& d = replayed.divergence;
    DiagFields              item(alloc);
    if (d.kind == DeviceDivergenceKind::Element)
    {
        item.str("kind", "divergence")
            .str("divergence", device_divergence_name(d.kind))
            .u64("buffer", d.buffer)
            .u64("element", d.element)
            .str("type", device_element_name(d.type))
            .u64("recorded", d.recorded)
            .u64("replayed", d.replayed)
            .u64("distance", d.distance)
            .u64("bound", d.bound)
            .u64("dispatch_op", replayed.dispatch.op);
        add_owned_site_as(item, "dispatch_file", "dispatch_line", "dispatch_col", replayed.dispatch);
        item.u64("declaration_op", replayed.declaration.op);
        add_owned_site_as(item, "declaration_file", "declaration_line", "declaration_col", replayed.declaration);
        (void)out.add_item(item);
    }
    else if (d.kind == DeviceDivergenceKind::Outcome)
    {
        item.str("kind", "divergence")
            .str("divergence", device_divergence_name(d.kind))
            .u64("recorded", d.recorded)
            .u64("replayed", d.replayed)
            .u64("recorded_op", d.recorded_fault)
            .u64("replayed_op", d.replayed_fault);
        (void)out.add_item(item);
    }
    // The replayed outcome differs from the recorded one only in an Outcome divergence (it is compared first).
    const bool outcome_differs = d.kind == DeviceDivergenceKind::Outcome;
    item.clear();
    item.str("kind", "outcome").str("run", "recorded").u64("error", record.device_error).u64("op", record.fault_op);
    add_owned_site(item, replayed.recorded_fault);
    (void)out.add_item(item);
    item.clear();
    item.str("kind", "outcome")
        .str("run", "replayed")
        .u64("error", outcome_differs ? d.replayed : record.device_error)
        .u64("op", replayed.fault.op);
    add_owned_site(item, replayed.fault);
    (void)out.add_item(item);

    const DeviceAdapterRecord& recorded = record.device_adapter;
    out.summary.str("path", call.request->path)
        .str("executor", replay_executor_name(record.executor))
        .str("recorded_backend", cont::StringView{recorded.backend.data(), recorded.backend.size()})
        .str("recorded_adapter", cont::StringView{recorded.name.data(), recorded.name.size()})
        .str("backend", cmd.device->adapter.backend)
        .str("adapter", cmd.device->adapter.name)
        .boolean("adapter_differs", replayed.adapter_differs)
        .str("envelope", device_envelope_name(record.device_envelope.kind))
        .u64("ulps", record.device_envelope.ulps)
        .str("program_source", parsed.program.empty() ? cont::StringView{"record"} : cont::StringView{"argument"})
        .str("program",
             parsed.program.empty() ? cont::StringView{record.program_path.data(), record.program_path.size()}
                                    : parsed.program)
        .u64("recorded_hash", record.content_hash)
        .u64("replayed_hash", replayed.replayed_hash)
        .boolean("program_matches", !replayed.program_differs)
        .str("kernels_source", parsed.kernel_dir.empty() ? cont::StringView{"record"} : cont::StringView{"argument"})
        .str("kernel_dir", parsed.kernel_dir)
        .boolean("kernels_match", !replayed.kernels_differ)
        .str("build", replayed.build_differs ? cont::StringView{"differs"} : cont::StringView{"same"})
        .u64("compared", replayed.compared)
        .u64("max_distance", replayed.max_distance)
        .str("result",
             d.kind == DeviceDivergenceKind::None ? cont::StringView{"reproduced"} : cont::StringView{"diverged"})
        .str("divergence", device_divergence_name(d.kind));
    return DiagStatus::Ok;
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

    // Compatibility, before anything runs: the executor, the build, then the inputs the program needs.
    const bool host   = record.executor == ReplayExecutorKind::Host;
    const bool device = record.executor == ReplayExecutorKind::Device;
    if (device && cmd->device == nullptr)
    {
        out.reason.append("incompatible replay: the record was made by the device executor and this host binds no "
                          "device executor");
        return DiagStatus::Unavailable;
    }
    if (host && (cmd->host == nullptr || cmd->host->replay == nullptr))
    {
        out.reason.append("incompatible replay: the record was made by the ");
        out.reason.append(replay_executor_name(record.executor));
        out.reason.append(" executor and this host binds no host executor");
        return DiagStatus::Unavailable;
    }
    if (!host && parsed.jobs != 0U)
    {
        out.reason.append("the argument 'jobs' is a host record's job split; this is a ");
        out.reason.append(replay_executor_name(record.executor));
        out.reason.append(" record");
        return DiagStatus::BadArgument;
    }
    if (!device && !parsed.kernel_dir.empty())
    {
        out.reason.append("the argument 'kernel_dir' replays a device record's kernels; this is a ");
        out.reason.append(replay_executor_name(record.executor));
        out.reason.append(" record");
        return DiagStatus::BadArgument;
    }
    if (device)
    {
        // A device record's build, adapter and inputs are checked by replay_device_record (an exact envelope is
        // claimed only on its own build, even with build=any), before anything runs.
        return replay_on_device(*cmd, call, parsed, record, out);
    }
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
    if (!record_missing_inputs(record, missing))
    {
        out.reason.append("incompatible replay: the record is missing inputs its program needs (");
        out.reason.append(cont::StringView{missing.data(), missing.size()});
        out.reason.append(")");
        return DiagStatus::Unavailable;
    }
    if (host)
    {
        return replay_on_host(*cmd, call, parsed, record, same, cont::StringView{differing.data(), differing.size()},
                              out);
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
        cont::Array<crd::u8> blob(alloc);
        if (const DiagStatus s = cook_other_program(*cmd, call, parsed.program, blob, out.reason);
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
    ReplayTrace     trace(alloc);
    InputFeed       feed({record.input_reads.data(), record.input_reads.size()}, trace); // never a live source
    run_traced(*replayed, cont::as_const_span(record.args), record.max_events, call.cancel, trace, feed.source());
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
        add_divergence(item, d);
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
        .str("executor", replay_executor_name(record.executor))
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
        .u64("recorded_input_reads", record.input_reads_total)
        .u64("replayed_input_reads", trace.input_reads_total)
        .str("result", d.kind == DivergenceKind::None ? cont::StringView{"reproduced"} : cont::StringView{"diverged"})
        .str("divergence", divergence_kind_name(d.kind));
    return DiagStatus::Ok;
}
} // namespace

crd::i64 monotonic_ns(void* /*user*/) noexcept
{
    return crd::time::MonotonicClock::now().ns_since_epoch();
}

void apply_clock(const HostClockSpec& spec, input::HostClock& clock) noexcept
{
    clock.clear();
    if (spec.live_wall)
    {
        clock.use_live_wall(&monotonic_ns, nullptr);
    }
    if (spec.has_sim_time)
    {
        clock.set_reading(kSimDomain, spec.sim_time);
    }
    if (spec.has_sim_step)
    {
        clock.set_step(kSimDomain, spec.sim_step);
    }
}

ClockArgument parse_clock_argument(cont::StringView name, cont::StringView value, HostClockSpec* spec,
                                   cont::StringView& requirement) noexcept
{
    if (name == "clock")
    {
        if (value != "wall")
        {
            requirement = cont::StringView{"must be 'wall'"};
            return ClockArgument::Refused;
        }
        if (spec != nullptr)
        {
            spec->live_wall = true;
        }
        return ClockArgument::Ok;
    }
    if (name != "sim_time" && name != "sim_step")
    {
        return ClockArgument::NotClock;
    }
    crd::i64 v = 0;
    if (!detail::parse_i64(value, v))
    {
        requirement = cont::StringView{"must be an i64 count of nanoseconds"};
        return ClockArgument::Refused;
    }
    if (spec != nullptr && name == "sim_time")
    {
        spec->has_sim_time = true;
        spec->sim_time     = v;
    }
    else if (spec != nullptr)
    {
        spec->has_sim_step = true;
        spec->sim_step     = v;
    }
    return ClockArgument::Ok;
}

void apply_events(const HostEventsSpec& spec, input::HostEvents& events)
{
    events.clear();
    if (!spec.open)
    {
        return;
    }
    (void)events.open(kEventQueue);
    for (const crd::i64 e : spec.events)
    {
        (void)events.push(kEventQueue, e); // at most kMaxArgumentEvents, far below the source's bound
    }
}

namespace
{
constexpr cont::StringView kEventsRule{
    "must be at most 32 comma-separated events: key_down, key_up, key_repeat, mouse_down or mouse_up:<code 0 to "
    "65535>[:<mods 0 to 15>], or mouse_move, scroll or resize:<x>:<y> (each -32768 to 32767)"};

// The next ':'-separated field of `item` from `at` (advanced past it); false when there is none.
[[nodiscard]] bool next_field(cont::StringView item, crd::usize& at, cont::StringView& field) noexcept
{
    if (at > item.size())
    {
        return false;
    }
    crd::usize end = item.find(':', at);
    if (end == cont::StringView::npos)
    {
        end = item.size();
    }
    field = item.substr(at, end - at);
    at    = end + 1U;
    return true;
}

// A decimal in [lo, hi].
[[nodiscard]] bool bounded(cont::StringView text, crd::i64 lo, crd::i64 hi, crd::i64& out) noexcept
{
    return detail::parse_i64(text, out) && out >= lo && out <= hi;
}

// One event of the `events` argument, packed.
[[nodiscard]] bool parse_event(cont::StringView item, crd::i64& packed) noexcept
{
    crd::usize       at = 0U;
    cont::StringView name;
    input::EventType type = input::EventType::None;
    if (!next_field(item, at, name) || !input::event_type_of(name, type) || type == input::EventType::None)
    {
        return false;
    }
    input::Event     e;
    cont::StringView a;
    cont::StringView b;
    crd::i64         first  = 0;
    crd::i64         second = 0;
    e.type = static_cast<crd::u8>(type);
    const bool pointer =
        type == input::EventType::MouseMove || type == input::EventType::Scroll || type == input::EventType::Resize;
    if (!next_field(item, at, a))
    {
        return false;
    }
    if (pointer)
    {
        if (!bounded(a, -32768, 32767, first) || !next_field(item, at, b) || !bounded(b, -32768, 32767, second))
        {
            return false;
        }
        e.x = static_cast<crd::i16>(first);
        e.y = static_cast<crd::i16>(second);
    }
    else
    {
        if (!bounded(a, 0, 65535, first))
        {
            return false;
        }
        e.code = static_cast<crd::u16>(first);
        if (next_field(item, at, b))
        {
            if (!bounded(b, 0, 15, second))
            {
                return false;
            }
            e.mods = static_cast<crd::u8>(second);
        }
    }
    packed = input::pack_event(e);
    return at > item.size(); // no field left over
}
} // namespace

bool parse_events_argument(cont::StringView value, cont::Array<crd::i64>* out, cont::StringView& requirement)
{
    crd::u32   count = 0U;
    const bool ok    = detail::for_each_item(value,
                                             [out, &count](cont::StringView item)
                                             {
                                                 crd::i64 packed = 0;
                                                 if (!parse_event(item, packed) || ++count > kMaxArgumentEvents)
                                                 {
                                                     return false;
                                                 }
                                                 if (out != nullptr)
                                                 {
                                                     out->push_back(packed);
                                                 }
                                                 return true;
                                             });
    if (!ok)
    {
        requirement = kEventsRule;
    }
    return ok;
}

// A seeded source holds one counter per random stream and the event queues at most 4,096 events, so their own
// allocator grows from a small first chunk.
constexpr crd::usize kRunInputsChunkBytes = crd::usize{64} << 10U;

RunInputs::RunInputs(const char* name)
    : m_alloc(kRunInputsChunkBytes, nullptr, name), m_seeded(0U, &m_alloc), m_events(&m_alloc)
{
    m_router.route(input::InputKind::Clock, m_clock.source());
    m_router.route(input::InputKind::TimeStep, m_clock.source());
    m_router.route(input::InputKind::Event, m_events.source());
}

void RunInputs::set(bool seeded, crd::u64 seed, const HostClockSpec& clock, const HostEventsSpec& events)
{
    m_seeded.reset(seed);
    m_router.route(input::InputKind::Random, seeded ? m_seeded.source() : nullptr);
    apply_clock(clock, m_clock);
    apply_events(events, m_events);
}

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
