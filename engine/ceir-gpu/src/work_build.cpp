// CEIR-20b step 2: the work-chain builder (see work_build.hpp). Emits an authored ceir.work program (queue_alloc +
// produce/consume/compact) from a WorkBuildDesc and verifies it with `find_work_misuse`. The MOLD is
// build_fullscreen_ceir: register the dialects on a fresh Context → create a module + block → emit resource.declare
// bindings (their `source` attr is the record-time resolver identity) + arith.const grid + the dialect op → verify. ⛔
// Nothing wavefront-specific: this materializes ANY authored work pass (the algorithm lives in the .frame.toml + the
// .ckir kernels). Lowering to a LoweredCommand plan is CEIR-20b step 3.

#include <crd/ceir/gen/arith_ops.hpp>    // register_arith_ops + arith.const
#include <crd/ceir/gen/resource_ops.hpp> // register_resource_ops + resource.declare
#include <crd/ceir/gpu/lower.hpp>        // lower_region (the ceir.work -> LoweredCommand plan)
#include <crd/ceir/gpu/work_build.hpp>
#include <crd/ceir/work.hpp> // work::register_dialect + type_queue + find_work_misuse

namespace crd::ceir::gpu
{
namespace
{
// The `access` token for one binding — the find_work_misuse vocabulary (comma-joined into the op's `access` string).
[[nodiscard]] containers::StringView access_token(WorkAccess a) noexcept
{
    switch (a)
    {
        case WorkAccess::Write:
            return containers::StringView("w");
        case WorkAccess::ReadWrite:
            return containers::StringView("rw");
        case WorkAccess::Read:
            break;
    }
    return containers::StringView("r");
}
} // namespace

Module* build_work_ceir(Context& ctx, const WorkBuildDesc& desc, containers::Array<LoweredCommand>& out_plan,
                        WorkAssetResources& out_resources)
{
    out_plan.clear();
    out_resources.count = 0U;
    if (desc.num_queues > 8U || desc.num_stages > 8U)
    {
        return nullptr;
    }

    // The work program uses the arith (const grid), resource (declared queues/bindings) and work dialects. Registration
    // is idempotent on a fresh Context (the documented precondition — the build_fullscreen_ceir shape).
    (void)arith::register_arith_ops(ctx);
    (void)resource::register_resource_ops(ctx);
    (void)work::register_dialect(ctx);

    const OpId k_alloc = ctx.intern_op("work", "queue_alloc");
    const OpId k_produce = ctx.intern_op("work", "produce");
    const OpId k_consume = ctx.intern_op("work", "consume");
    const OpId k_compact = ctx.intern_op("work", "compact");
    const OpId k_decl = ctx.intern_op("resource", "declare");
    const OpId k_cst = ctx.intern_op("arith", "const");

    Module* const m = ctx.create_module();
    Block* bb = m->body()->first_block();
    if (bb == nullptr)
    {
        bb = ctx.create_block(0U);
        m->body()->append(bb);
    }

    // ── queue_alloc per queue. Each produces a %queue (work.queue) the stages flow through. capacity/record_stride are
    // the 20a required attrs (find_work_misuse: >= 1); `source` is an OPEN attr (the resolver identity → the device
    // buffer the WorkHooks.resolve_queue hook provisions — the build_fullscreen_ceir source-param precedent; the opgen
    // verifier ignores it).
    Value* queue_vals[8] = {};
    for (crd::u32 q = 0U; q < desc.num_queues; ++q)
    {
        Operation* const alloc = ctx.create_operation(k_alloc, {}, 1U, work::type_queue(ctx));
        ctx.set_attr(alloc, "capacity", ctx.attr_int(static_cast<crd::i64>(desc.queues[q].capacity)));
        ctx.set_attr(alloc, "record_stride", ctx.attr_int(static_cast<crd::i64>(desc.queues[q].record_stride)));
        ctx.set_attr(alloc, "source", ctx.attr_int(static_cast<crd::i64>(desc.queues[q].source_param)));
        bb->append(alloc);
        queue_vals[q] = alloc->result(0U);
        if (out_resources.count < 64U)
        {
            out_resources.entries[out_resources.count++] = {desc.queues[q].source_param, queue_vals[q]};
        }
    }

    // ── one produce/consume/compact op per stage. Operand order MIRRORS the 20a contract (verified by
    // find_work_misuse):
    //    produce = [gx, gy, gz, %queue, bindings...]; consume = [%queue, bindings...]; compact = [%src, %dst,
    //    bindings...].
    for (crd::u32 s = 0U; s < desc.num_stages; ++s)
    {
        const WorkStageDesc& st = desc.stages[s];
        if (st.kernel.size() == 0U || st.num_bindings > 8U)
        {
            return nullptr;
        }
        if (st.queue >= desc.num_queues)
        {
            return nullptr;
        }
        if (st.kind == WorkStageKind::Compact && st.src_queue >= desc.num_queues)
        {
            return nullptr;
        }

        Value* ops[16] = {};
        crd::u32 n = 0U;

        // the fixed operands (queue-family), by op.
        if (st.kind == WorkStageKind::Produce)
        {
            for (crd::u32 g = 0U; g < 3U; ++g)
            {
                Operation* const gc = ctx.create_operation(k_cst, {}, 1U, ctx.type_index());
                ctx.set_attr(gc, "value", ctx.attr_int(static_cast<crd::i64>(st.grid[g])));
                bb->append(gc);
                ops[n++] = gc->result(0U);
            }
            ops[n++] = queue_vals[st.queue];
        }
        else if (st.kind == WorkStageKind::Consume)
        {
            ops[n++] = queue_vals[st.queue];
        }
        else // Compact: src then dst
        {
            ops[n++] = queue_vals[st.src_queue];
            ops[n++] = queue_vals[st.queue];
        }

        // the variadic binding tail + the comma-joined `access` string (find_work_misuse: tokens == binding count).
        char acc[64] = {};
        crd::u32 na = 0U;
        for (crd::u32 b = 0U; b < st.num_bindings; ++b)
        {
            Operation* const bd =
                ctx.create_operation(k_decl, {}, 1U, ctx.type_buffer(BufferMode::Plain, ctx.type_f32()));
            ctx.set_attr(bd, "slot", ctx.attr_int(static_cast<crd::i64>(b)));
            ctx.set_attr(bd, "source", ctx.attr_int(static_cast<crd::i64>(st.bindings[b].source_param)));
            bb->append(bd);
            ops[n++] = bd->result(0U);
            if (out_resources.count < 64U)
            {
                out_resources.entries[out_resources.count++] = {st.bindings[b].source_param, bd->result(0U)};
            }

            if (na > 0U)
            {
                acc[na++] = ',';
            }
            const containers::StringView tok = access_token(st.bindings[b].access);
            for (crd::usize c = 0U; c < tok.size(); ++c)
            {
                acc[na++] = tok.data()[c];
            }
        }

        const OpId kind = st.kind == WorkStageKind::Produce   ? k_produce
                          : st.kind == WorkStageKind::Consume ? k_consume
                                                              : k_compact;
        Operation* const op = ctx.create_operation(kind, containers::ConstSpan<Value*>(ops, n), 0U, TypeId{});
        ctx.set_attr(op, "kernel", ctx.attr_symbol(st.kernel));
        ctx.set_attr(op, "access", ctx.attr_string(containers::StringView(acc, na)));
        bb->append(op);
    }

    // ── verifier-first: the executor (step 4) assumes a find_work_misuse-clean program (the build_fullscreen_ceir
    // contract).
    if (work::find_work_misuse(ctx, *m).kind != work::WorkMisuseKind::None)
    {
        return nullptr;
    }
    // ── lower to the inspectable plan: produce → Dispatch, consume/compact → DispatchIndirect, queue_alloc → nothing.
    lower_region(ctx, *bb, out_plan);
    return m;
}
} // namespace crd::ceir::gpu
