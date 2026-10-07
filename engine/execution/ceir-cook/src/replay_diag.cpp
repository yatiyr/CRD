#include <crd/ceir/cook/replay_diag.hpp>

#include "bounded_file.hpp"
#include "program_load.hpp"
#include "replay_needs.hpp"

#include <crd/ceir/binary.hpp> // stable_hash
#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/perf/diag_commands.hpp>

namespace crd::ceir::cook
{
namespace
{
namespace cont = crd::containers;

using crd::perf::DiagCall;
using crd::perf::DiagFields;
using crd::perf::DiagSnapshot;
using crd::perf::DiagStatus;
using detail::kReplayInputCount;
using detail::Need;

// The op an item blames, at its closest-known authored position.
void blame(const Context& ctx, const Operation* op, cont::Array<Origin>& storage, DiagFields& item)
{
    const Provenance p  = resolve_provenance(ctx, op, storage);
    const Origin*    at = op != nullptr ? p.primary() : nullptr;
    item.u64("op", op != nullptr ? p.op.value : 0U)
        .str("op_name", op != nullptr ? ctx.op_name(op->kind()) : cont::StringView{})
        .str("file", at != nullptr ? ctx.file_path(at->loc.file_id) : cont::StringView{})
        .u64("line", at != nullptr ? at->loc.line : 0U)
        .u64("col", at != nullptr ? at->loc.col : 0U);
}

DiagStatus run_replay_prepare(void* context, const DiagCall& call, DiagSnapshot& out)
{
    auto* const                    command = static_cast<ReplayPrepareCommand*>(context);
    crd::memory::IAllocator* const alloc   = out.allocator();
    command->runs.fetch_add(1U, std::memory_order_relaxed);

    cont::Array<crd::u8> bytes(alloc);
    if (const DiagStatus read =
            detail::read_bounded_file(call.file, command->max_program_bytes, bytes, out.reason, command->bytes_read);
        read != DiagStatus::Ok)
    {
        return read;
    }

    // A run record answers from what it holds: its own program, build, arguments and input states.
    Context               ctx(alloc);
    ReplayRecord          record(alloc);
    ReplayProgram         from_record(alloc);
    detail::LoadedProgram loaded;
    const bool            is_record = is_replay_record({bytes.data(), bytes.size()});
    const Module*         module    = nullptr;
    if (is_record)
    {
        if (const RecordError e = decode_record({bytes.data(), bytes.size()}, record); e != RecordError::Ok)
        {
            out.reason.append("the run record did not decode: ");
            out.reason.append(record_error_name(e));
            return DiagStatus::Failed;
        }
        load_replay_program(ctx, {record.program.data(), record.program.size()},
                            cont::StringView{record.entry.data(), record.entry.size()}, command->registrar,
                            command->user, from_record);
        if (from_record.module == nullptr)
        {
            out.reason.append("the run record's program did not load: ");
            out.reason.append(read_error_name(from_record.read));
            return DiagStatus::Failed;
        }
        module = from_record.module;
    }
    else
    {
        const DiagStatus load = detail::load_program_bytes(bytes, call.request->path, call.cancel, command->registrar,
                                                           command->user, ctx, loaded, out.reason);
        if (load != DiagStatus::Ok)
        {
            return load;
        }
        module = loaded.module;
    }

    detail::ProgramNeeds needs;
    if (!detail::analyze_needs(ctx, *module, alloc, call.cancel, needs))
    {
        out.reason.append("cancelled while classifying the program's ops");
        return DiagStatus::Cancelled;
    }

    cont::Array<Origin> storage(alloc);
    DiagFields          item(alloc);
    cont::String        reason(alloc);
    cont::String        missing_names(alloc);
    crd::u64            needed  = 0U;
    crd::u64            unknown = 0U;
    crd::u64            missing = 0U;
    for (crd::u32 i = 0U; i < kReplayInputCount; ++i)
    {
        const detail::ReplayInputSpec& spec = detail::replay_input(i);
        const detail::InputNeed&       t    = needs.inputs[i];
        const Need                     need = t.need;

        // A record holds the identity and arguments of its run; a program file only its own content hash.
        const bool available = i == detail::kReplayProgramInput ||
                               (is_record && record.inputs[i].state == ReplayInputState::Recorded);
        reason.clear();
        cont::StringView state{"missing"};
        if (need == Need::No)
        {
            state = cont::StringView{"not-needed"};
            reason.append("no op needs ");
            reason.append(spec.what);
        }
        else if (available)
        {
            state = cont::StringView{"available"};
            reason.append(spec.what);
            reason.append(is_record ? cont::StringView{" is in the run record"}
                                    : cont::StringView{" is computed from the file"});
        }
        else
        {
            if (need == Need::Unknown)
            {
                reason.append("an opaque op may need ");
                reason.append(spec.what);
                reason.append("; ");
            }
            reason.append(spec.missing);
            ++missing;
            if (!missing_names.empty())
            {
                missing_names.push_back(',');
            }
            missing_names.append(spec.name);
        }
        needed += need == Need::Yes ? 1U : 0U;
        unknown += need == Need::Unknown ? 1U : 0U;

        item.clear();
        item.str("kind", "input")
            .str("input", spec.name)
            .str("guarantee", spec.guarantee)
            .str("needed", detail::need_name(need))
            .str("state", state)
            .u64("ops", t.ops);
        blame(ctx, need == Need::No ? nullptr : t.cause, storage, item);
        item.str("reason", cont::StringView{reason.data(), reason.size()});
        (void)out.add_item(item);
    }

    out.summary.str("path", call.request->path)
        .str("form", is_record ? cont::StringView{"record"} : detail::program_form_name(loaded.form))
        .u64("bytes", bytes.size())
        .u64("content_hash", stable_hash(ctx, *module, alloc))
        .u64("recorded_hash", is_record ? record.content_hash : loaded.recorded_hash)
        .u64("ops", needs.ops)
        .u64("unregistered", needs.unregistered)
        .u64("opaque", needs.opaque)
        .str("weakest_claim", detail::claim_name(needs.weakest))
        .u64("unclaimed", needs.unclaimed)
        .u64("needed", needed)
        .u64("unknown", unknown)
        .u64("missing", missing)
        .str("replay", missing == 0U ? cont::StringView{"prepared"} : cont::StringView{"unavailable"})
        .str("missing_inputs", cont::StringView{missing_names.data(), missing_names.size()});
    return DiagStatus::Ok;
}
} // namespace

bool register_replay_prepare(perf::DiagCommandService& service, ReplayPrepareCommand& command)
{
    perf::DiagCommandSpec spec;
    spec.name       = kReplayPrepareCommand;
    spec.owner      = containers::StringView{"ceir"};
    spec.summary    = containers::StringView{"what replaying an authored program needs, and why each input is missing"};
    spec.authority  = perf::DiagAuthority::Read;
    spec.takes_path = true;
    return service.register_command(spec, &run_replay_prepare, &command);
}
} // namespace crd::ceir::cook
