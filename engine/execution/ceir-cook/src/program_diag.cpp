#include <crd/ceir/cook/program_diag.hpp>

#include "program_load.hpp"

#include <crd/ceir/binary.hpp> // stable_hash
#include <crd/ceir/context.hpp>
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

// The walk checks the caller's cancel flag once per this many ops.
constexpr crd::u32 kCancelStride = 256U;

[[nodiscard]] cont::StringView space_name(OriginSpace s) noexcept
{
    switch (s) // no default: every space is named
    {
    case OriginSpace::CarrierOp: return cont::StringView{"carrier-op"};
    case OriginSpace::CeirOp: return cont::StringView{"ceir-op"};
    case OriginSpace::ChirNode: return cont::StringView{"chir-node"};
    }
    return cont::StringView{"?"};
}

// What the walk counts for the summary.
struct Tally
{
    crd::u64 ops          = 0U;
    crd::u64 positioned   = 0U; // ops with an authored source position
    crd::u64 chir         = 0U; // ops lowered from at least one CHIR node
    crd::u64 intrinsic    = 0U;
    crd::u64 unregistered = 0U;
    crd::u64 origin_items = 0U;
};

// One op's items. The op item carries the closest-known position and the first CHIR origin; the fields a reader acts
// on come first and the rendered provenance last, so an item clipped at its byte bound still carries them. When the op
// item cannot state every origin (an op a pass merged from several, or one whose single origin is another op), one
// origin item per origin follows it, so no origin is lost to a clipped rendering.
void describe(const Context& ctx, const Operation* op, crd::u64 index, crd::u32 depth, cont::Array<Origin>& storage,
              crd::memory::IAllocator* alloc, DiagFields& item, DiagSnapshot& out, Tally& tally)
{
    const Provenance    p      = resolve_provenance(ctx, op, storage);
    const Origin*       at     = p.primary();
    const NativeBinding native = native_binding(ctx, op);

    const Origin* chir = nullptr;
    for (crd::usize i = 0U; i < p.origins.size(); ++i)
    {
        if (p.origins[i].space == OriginSpace::ChirNode)
        {
            chir = &p.origins[i];
            break;
        }
    }

    item.clear();
    item.str("kind", "op")
        .u64("index", index)
        .u64("op", p.op.value)
        .str("name", native.op_name)
        .u64("depth", depth)
        .str("file", at != nullptr ? ctx.file_path(at->loc.file_id) : cont::StringView{})
        .u64("line", at != nullptr ? at->loc.line : 0U)
        .u64("col", at != nullptr ? at->loc.col : 0U)
        .str("gap", provenance_gap_name(p.gap))
        .u64("origins", p.origins.size())
        .u64("chir", chir != nullptr ? chir->node.value : 0U)
        .str("chir_file", chir != nullptr ? ctx.file_path(chir->loc.file_id) : cont::StringView{})
        .u64("chir_line", chir != nullptr ? chir->loc.line : 0U)
        .u64("chir_col", chir != nullptr ? chir->loc.col : 0U)
        .str("native", native_kind_name(native.kind))
        .str("provider", native.provider);
    const cont::String site = render_provenance(ctx, p, alloc);
    item.str("provenance", cont::StringView{site.data(), site.size()});
    (void)out.add_item(item); // past the snapshot's item bound the drop is counted there

    // A single origin is stated by the op item when it is the CHIR node above or the op's own position.
    const bool stated = p.origins.empty() ||
                        (p.origins.size() == 1U &&
                         (p.origins[0].space == OriginSpace::ChirNode || p.origins[0].node == p.op));
    if (!stated)
    {
        for (crd::usize k = 0U; k < p.origins.size(); ++k)
        {
            const Origin& o = p.origins[k];
            item.clear();
            item.str("kind", "origin")
                .u64("index", index)
                .u64("op", p.op.value)
                .u64("ordinal", k)
                .str("space", space_name(o.space))
                .u64("node", o.node.value)
                .str("file", ctx.file_path(o.loc.file_id))
                .u64("line", o.loc.line)
                .u64("col", o.loc.col);
            (void)out.add_item(item);
            ++tally.origin_items;
        }
    }

    ++tally.ops;
    tally.positioned += at != nullptr ? 1U : 0U;
    tally.chir += chir != nullptr ? 1U : 0U;
    tally.intrinsic += native.kind == NativeKind::Intrinsic ? 1U : 0U;
    tally.unregistered += native.kind == NativeKind::Unregistered ? 1U : 0U;
}

DiagStatus run_program_provenance(void* context, const DiagCall& call, DiagSnapshot& out)
{
    auto* const                    command = static_cast<ProgramProvenanceCommand*>(context);
    crd::memory::IAllocator* const alloc   = out.allocator();
    command->runs.fetch_add(1U, std::memory_order_relaxed);

    cont::Array<crd::u8>  bytes(alloc);
    Context               ctx(alloc);
    detail::LoadedProgram loaded;
    const DiagStatus load = detail::load_program(call, command->max_program_bytes, command->registrar, command->user,
                                                 ctx, bytes, loaded, out.reason, command->bytes_read);
    if (load != DiagStatus::Ok)
    {
        return load;
    }
    const Module* const module = loaded.module;

    // Pre-order over every region, iteratively: the authored nesting is the order a reader expects, and a deep nest
    // costs heap, not stack.
    struct Pending
    {
        const Operation* op    = nullptr;
        crd::u32         depth = 0U;
    };
    cont::Array<Pending>          stack(alloc);
    cont::Array<const Operation*> children(alloc);
    cont::Array<Origin>           storage(alloc);
    DiagFields                    item(alloc);
    Tally                         tally;

    const auto push_children = [&](const Region* region, crd::u32 depth)
    {
        children.clear();
        for (const Block* b = region->first_block(); b != nullptr; b = b->next_in_region())
        {
            for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
            {
                children.push_back(op);
            }
        }
        for (crd::usize i = children.size(); i > 0U; --i)
        {
            stack.push_back(Pending{children[i - 1U], depth});
        }
    };
    push_children(module->body(), 0U);

    while (!stack.empty())
    {
        const Pending next = stack.back();
        stack.pop_back();
        if (tally.ops % kCancelStride == 0U && call.cancelled())
        {
            out.reason.append("cancelled while listing the program's ops");
            return DiagStatus::Cancelled;
        }
        describe(ctx, next.op, tally.ops, next.depth, storage, alloc, item, out, tally);
        for (crd::u32 r = next.op->num_regions(); r > 0U; --r)
        {
            push_children(next.op->region(r - 1U), next.depth + 1U);
        }
    }

    out.summary.str("path", call.request->path)
        .str("form", detail::program_form_name(loaded.form))
        .u64("bytes", bytes.size())
        .u64("content_hash", stable_hash(ctx, *module, alloc))
        .u64("recorded_hash", loaded.recorded_hash)
        .u64("ops", tally.ops)
        .u64("positioned", tally.positioned)
        .u64("unpositioned", tally.ops - tally.positioned)
        .u64("chir_origins", tally.chir)
        .u64("intrinsic", tally.intrinsic)
        .u64("unregistered", tally.unregistered)
        .u64("origin_items", tally.origin_items);
    return DiagStatus::Ok;
}
} // namespace

bool register_program_provenance(perf::DiagCommandService& service, ProgramProvenanceCommand& command)
{
    perf::DiagCommandSpec spec;
    spec.name       = kProgramProvenanceCommand;
    spec.owner      = containers::StringView{"ceir"};
    spec.summary    = containers::StringView{"an authored program's ops with their source positions and CHIR origins"};
    spec.authority  = perf::DiagAuthority::Read;
    spec.takes_path = true;
    return service.register_command(spec, &run_program_provenance, &command);
}
} // namespace crd::ceir::cook
