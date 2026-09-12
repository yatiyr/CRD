// crd-ceir-gpu — CEIR-13z the GPU execution seam (ADR-0126). See execute.hpp for the contract.

#include <crd/ceir/gpu/execute.hpp>

#include <crd/containers/array.hpp>
#include <crd/gpu/command_model.hpp> // crd::gpu::kMaxBindings — the binding structural cap

namespace crd::ceir::gpu
{
namespace
{
// Resolve + structurally check ONE Dispatch command, and (if `out_bufs` is non-null) gather its ordered ComputeBuffer*
// bindings. ⛔ 13z-1 executes DIRECT const-grid dispatches only (the §129 proof kernels): a dynamic grid or an Indirect
// dispatch is a named-forward → UnsupportedCommand. Binding operands follow the grid prefix (Direct: 3 grid operands, so
// bindings start at operand 3), are resource_root-NORMALIZED (the 13d part-3 machinery), and looked up in `bindings`.
[[nodiscard]] ExecuteError check_dispatch(const Context& ctx, const LoweredCommand& cmd, KernelResolveFn resolver,
                                          void* user, containers::ConstSpan<ResolvedBinding> bindings,
                                          crd::gpu::ComputePipeline** out_pipe,
                                          containers::Array<crd::gpu::ComputeBuffer*>* out_bufs)
{
    if (cmd.dynamic_grid || cmd.dispatch_kind != crd::gpu::DispatchKind::Direct)
    {
        return ExecuteError::UnsupportedCommand; // dynamic-grid / Indirect resolution is named-forward (proof kernels are Direct const-grid)
    }
    if (cmd.groups_x == 0U || cmd.groups_y == 0U || cmd.groups_z == 0U) { return ExecuteError::ZeroDispatch; }

    crd::gpu::ComputePipeline* const pipe = (resolver != nullptr) ? resolver(cmd.op, user) : nullptr;
    if (pipe == nullptr) { return ExecuteError::UnresolvedKernel; }
    if (out_pipe != nullptr) { *out_pipe = pipe; }

    const Operation* const op    = cmd.op;
    const crd::u32         nops  = (op != nullptr) ? op->num_operands() : 0U;
    const crd::u32         fixed = 3U; // Direct: gx, gy, gz precede the bindings (mirrors lower.cpp's gather)
    const crd::u32         nbind = nops >= fixed ? nops - fixed : 0U;
    if (nbind > crd::gpu::kMaxBindings) { return ExecuteError::BindingArity; }

    if (out_bufs != nullptr) { out_bufs->clear(); }
    for (crd::u32 i = 0; i < nbind; ++i)
    {
        const Value* const           root = ctx.resource_root(op->operand(fixed + i));
        crd::gpu::ComputeBuffer*      buf  = nullptr;
        for (crd::u32 j = 0; j < static_cast<crd::u32>(bindings.size()); ++j)
        {
            if (bindings[j].resource == root)
            {
                buf = bindings[j].buffer;
                break;
            }
        }
        if (buf == nullptr) { return ExecuteError::UnmappedBinding; }
        if (out_bufs != nullptr) { out_bufs->push_back(buf); }
    }
    return ExecuteError::None;
}

// ⭐ CEIR-13z-3 part 2: a Barrier's HazardKind → the (from, to) ComputeAccess pair for a dispatch→dispatch barrier
// (ADR-0126 §3.3). ⛔ dispatch→dispatch ONLY — the harness owns the upload TransferDst→ShaderRead barrier (a dispatch-only
// CEIR asset has no transfer). ⛔ WAR is a REAL dependency on BOTH backends (verified): Vulkan issues a COMPUTE→COMPUTE
// execution dependency (the write waits for the read); DX12 IGNORES `from` and issues a `to`=ShaderWrite→UNORDERED_ACCESS
// UAV barrier (direction-agnostic serialization) — so no conservatism is needed. No `default` → -Werror=switch forces a new
// HazardKind to add a case.
void hazard_access(HazardKind h, crd::gpu::ComputeAccess& from, crd::gpu::ComputeAccess& to)
{
    switch (h)
    {
    case HazardKind::None: from = crd::gpu::ComputeAccess::ShaderRead;  to = crd::gpu::ComputeAccess::ShaderRead;  break; // inert (never emitted as a Barrier)
    case HazardKind::War:  from = crd::gpu::ComputeAccess::ShaderRead;  to = crd::gpu::ComputeAccess::ShaderWrite; break;
    case HazardKind::Raw:  from = crd::gpu::ComputeAccess::ShaderWrite; to = crd::gpu::ComputeAccess::ShaderRead;  break;
    case HazardKind::Waw:  from = crd::gpu::ComputeAccess::ShaderWrite; to = crd::gpu::ComputeAccess::ShaderWrite; break;
    }
}

// Replay one lowered Barrier as `rec.barrier(buffer, from, to)`. `cmd.resource` is the conflicting ROOT resource (already
// resource_root'd at lowering) — look it up in the binding table. ⛔ nullptr resource (ambient/whole-class) ⇒ barrier EVERY
// bound buffer (the conservative fallback).
void emit_barrier(crd::gpu::ComputeRecorder& rec, const LoweredCommand& cmd, containers::ConstSpan<ResolvedBinding> bindings)
{
    crd::gpu::ComputeAccess from = crd::gpu::ComputeAccess::ShaderWrite;
    crd::gpu::ComputeAccess to   = crd::gpu::ComputeAccess::ShaderRead;
    hazard_access(cmd.hazard, from, to);
    for (crd::u32 j = 0; j < static_cast<crd::u32>(bindings.size()); ++j)
    {
        if ((cmd.resource == nullptr || bindings[j].resource == cmd.resource) && bindings[j].buffer != nullptr)
        {
            rec.barrier(*bindings[j].buffer, from, to);
        }
    }
}
} // namespace

containers::StringView execute_error_name(ExecuteError e) noexcept
{
    switch (e)
    {
    case ExecuteError::None: return containers::StringView("None");
    case ExecuteError::UnresolvedKernel: return containers::StringView("UnresolvedKernel");
    case ExecuteError::ZeroDispatch: return containers::StringView("ZeroDispatch");
    case ExecuteError::BindingArity: return containers::StringView("BindingArity");
    case ExecuteError::UnmappedBinding: return containers::StringView("UnmappedBinding");
    case ExecuteError::UnsupportedCommand: return containers::StringView("UnsupportedCommand");
    case ExecuteError::UnresolvedProgram: return containers::StringView("UnresolvedProgram");
    case ExecuteError::NoFrameGraph: return containers::StringView("NoFrameGraph");
    case ExecuteError::FrameBuildFailed: return containers::StringView("FrameBuildFailed");
    case ExecuteError::SceneChainMisuse: return containers::StringView("SceneChainMisuse");
    case ExecuteError::UnresolvedSceneHandle: return containers::StringView("UnresolvedSceneHandle");
    case ExecuteError::AccelBuildFailed: return containers::StringView("AccelBuildFailed");
    case ExecuteError::UnresolvedTlas: return containers::StringView("UnresolvedTlas");
    case ExecuteError::TraceDispatchFailed: return containers::StringView("TraceDispatchFailed");
    case ExecuteError::UnresolvedQueue: return containers::StringView("UnresolvedQueue");
    case ExecuteError::WorkDispatchFailed: return containers::StringView("WorkDispatchFailed");
    }
    return containers::StringView("None");
}

ExecuteError validate_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands, KernelResolveFn resolver,
                              void* user, containers::ConstSpan<ResolvedBinding> bindings)
{
    for (crd::u32 i = 0; i < static_cast<crd::u32>(commands.size()); ++i)
    {
        const LoweredCommand& cmd = commands[i];
        if (cmd.kind == LoweredKind::Barrier) { continue; } // inert at 13z-1 (the resource-on-barrier map is 13z-3)
        if (cmd.kind == LoweredKind::Transfer) { return ExecuteError::UnsupportedCommand; }
        // CEIR-14b: render kinds (BeginRender/Draw/EndRender) target the 14z RASTER executor, not this IComputeContext
        // surface — reject them TYPED (the Transfer named-forward mirror), never fall through to check_dispatch.
        if (cmd.kind == LoweredKind::BeginRender || cmd.kind == LoweredKind::Draw || cmd.kind == LoweredKind::EndRender)
        {
            return ExecuteError::UnsupportedCommand;
        }
        // CEIR-19c: ceir.rt kinds (RayQuery/AccelBuild) target the RT executor (execute_rt_lowered, a caller-HOOK surface),
        // NOT this IComputeContext — reject them TYPED (the render/Transfer named-forward mirror).
        if (cmd.kind == LoweredKind::RayQuery || cmd.kind == LoweredKind::AccelBuild)
        {
            return ExecuteError::UnsupportedCommand;
        }
        // CEIR-20b: ceir.work's DispatchIndirect (a %queue-count-driven dispatch) targets the WORK executor
        // (execute_work_lowered, the queue resolver), NOT this IComputeContext — reject it TYPED (the RayQuery/Transfer mirror).
        if (cmd.kind == LoweredKind::DispatchIndirect)
        {
            return ExecuteError::UnsupportedCommand;
        }
        const ExecuteError err = check_dispatch(ctx, cmd, resolver, user, bindings, nullptr, nullptr);
        if (err != ExecuteError::None) { return err; }
    }
    return ExecuteError::None;
}

ExecuteError execute_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                             crd::gpu::ComputeRecorder& rec, KernelResolveFn resolver, void* user,
                             containers::ConstSpan<ResolvedBinding> bindings)
{
    containers::Array<crd::gpu::ComputeBuffer*> bufs(ctx.allocator());
    for (crd::u32 i = 0; i < static_cast<crd::u32>(commands.size()); ++i)
    {
        const LoweredCommand& cmd = commands[i];
        if (cmd.kind == LoweredKind::Barrier) // ⭐ 13z-3 part 2: replay as rec.barrier on the conflicting root buffer(s)
        {
            emit_barrier(rec, cmd, bindings);
            continue;
        }
        if (cmd.kind == LoweredKind::Transfer) { return ExecuteError::UnsupportedCommand; }
        // CEIR-14b: render kinds target the 14z RASTER executor — reject them TYPED (the Transfer mirror).
        if (cmd.kind == LoweredKind::BeginRender || cmd.kind == LoweredKind::Draw || cmd.kind == LoweredKind::EndRender)
        {
            return ExecuteError::UnsupportedCommand;
        }
        // CEIR-19c: ceir.rt kinds (RayQuery/AccelBuild) target the RT executor (execute_rt_lowered) — reject them TYPED here.
        if (cmd.kind == LoweredKind::RayQuery || cmd.kind == LoweredKind::AccelBuild)
        {
            return ExecuteError::UnsupportedCommand;
        }
        // CEIR-20b: ceir.work's DispatchIndirect targets execute_work_lowered (the queue resolver), NOT this
        // IComputeContext — reject it TYPED (the RayQuery/Transfer named-forward mirror).
        if (cmd.kind == LoweredKind::DispatchIndirect)
        {
            return ExecuteError::UnsupportedCommand;
        }
        crd::gpu::ComputePipeline* pipe = nullptr;
        const ExecuteError         err  = check_dispatch(ctx, cmd, resolver, user, bindings, &pipe, &bufs);
        if (err != ExecuteError::None) { return err; }
        rec.dispatch(*pipe, containers::ConstSpan<crd::gpu::ComputeBuffer*>(bufs.data(), bufs.size()), nullptr, 0U,
                     cmd.groups_x, cmd.groups_y, cmd.groups_z);
    }
    return ExecuteError::None;
}

// ── CEIR-19c: the ceir.rt executor. Walks the SAME lowered list (the render_materialize precedent); AccelBuild → build_scene
// (handle keyed by the op's %result); RayQuery → dispatch_inline_ray_query. Barriers inert (submit+wait per trace_dispatch).
ExecuteError validate_rt_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands, const RtHooks& hooks,
                                 containers::ConstSpan<RtHostBinding> bindings)
{
    (void)bindings; // the pure half checks STRUCTURE (grid/kernel/%tlas), not the host spans (those bind at execute)
    containers::Array<const Value*> built(ctx.allocator()); // AccelBuild %results in program order (the %tlas candidates)
    for (crd::u32 i = 0; i < static_cast<crd::u32>(commands.size()); ++i)
    {
        const LoweredCommand& cmd = commands[i];
        if (cmd.kind == LoweredKind::Barrier) { continue; } // inert (submit+wait per dispatch)
        if (cmd.kind == LoweredKind::AccelBuild)
        {
            if (cmd.op != nullptr && cmd.op->num_results() > 0U) { built.push_back(cmd.op->result(0U)); }
            continue;
        }
        if (cmd.kind == LoweredKind::RayQuery)
        {
            if (cmd.dynamic_grid) { return ExecuteError::UnsupportedCommand; } // const-grid witness (a host-readback grid is named-forward)
            if (cmd.groups_x == 0U || cmd.groups_y == 0U || cmd.groups_z == 0U) { return ExecuteError::ZeroDispatch; }
            const containers::ConstSpan<crd::u8> kb =
                (hooks.kernel_bytes != nullptr) ? hooks.kernel_bytes(cmd.op, hooks.user) : containers::ConstSpan<crd::u8>{};
            if (kb.size() == 0U) { return ExecuteError::UnresolvedKernel; }
            const Operation* const op = cmd.op;
            if (op == nullptr || op->num_operands() < 4U) { return ExecuteError::UnresolvedTlas; } // grid(0..2) + %tlas(3) minimum
            bool found = false; // %tlas = operand 3, matched by SSA identity to an earlier AccelBuild %result (NOT resource_root:
            for (crd::u32 j = 0; j < static_cast<crd::u32>(built.size()); ++j) // it is an rt.tlas Extern handle, not a buffer)
            {
                if (built[j] == op->operand(3U)) { found = true; break; }
            }
            if (!found) { return ExecuteError::UnresolvedTlas; }
            continue;
        }
        return ExecuteError::UnsupportedCommand; // Dispatch/Transfer/render kinds are not the RT surface
    }
    return ExecuteError::None;
}

ExecuteError execute_rt_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands, const RtHooks& hooks,
                                containers::ConstSpan<RtHostBinding> bindings)
{
    const ExecuteError verr = validate_rt_lowered(ctx, commands, hooks, bindings);
    if (verr != ExecuteError::None) { return verr; }

    containers::Array<const Value*>  keys(ctx.allocator());    // AccelBuild %result → handle (parallel arrays, program order)
    containers::Array<RtSceneHandle> handles(ctx.allocator());
    containers::Array<RtHostBinding> ordered(ctx.allocator()); // scratch: a ray_query's SSBO bindings in operand order
    for (crd::u32 i = 0; i < static_cast<crd::u32>(commands.size()); ++i)
    {
        const LoweredCommand& cmd = commands[i];
        if (cmd.kind == LoweredKind::Barrier) { continue; } // inert
        if (cmd.kind == LoweredKind::AccelBuild)
        {
            const RtSceneHandle h = (hooks.build_scene != nullptr) ? hooks.build_scene(cmd.op, hooks.user) : 0U;
            if (h == 0U) { return ExecuteError::AccelBuildFailed; }
            if (cmd.op != nullptr && cmd.op->num_results() > 0U)
            {
                keys.push_back(cmd.op->result(0U));
                handles.push_back(h);
            }
            continue;
        }
        // RayQuery — validate already rejected foreign kinds + guaranteed the grid/kernel/%tlas structure.
        const Operation* const op   = cmd.op;
        RtSceneHandle          tlas = 0U;
        for (crd::u32 j = 0; j < static_cast<crd::u32>(keys.size()); ++j)
        {
            if (keys[j] == op->operand(3U)) { tlas = handles[j]; break; }
        }
        if (tlas == 0U) { return ExecuteError::UnresolvedTlas; }
        const containers::ConstSpan<crd::u8> kb =
            (hooks.kernel_bytes != nullptr) ? hooks.kernel_bytes(op, hooks.user) : containers::ConstSpan<crd::u8>{};
        if (kb.size() == 0U) { return ExecuteError::UnresolvedKernel; }
        // SSBO bindings: operands 4+ (resource_root-normalized) → the caller's host spans, in operand order (slots 1,2,…).
        ordered.clear();
        const crd::u32 nops = op->num_operands();
        for (crd::u32 b = 4U; b < nops; ++b)
        {
            const Value* const root  = ctx.resource_root(op->operand(b));
            bool               found = false;
            for (crd::u32 k = 0; k < static_cast<crd::u32>(bindings.size()); ++k)
            {
                if (bindings[k].resource == root)
                {
                    ordered.push_back(bindings[k]);
                    found = true;
                    break;
                }
            }
            if (!found) { return ExecuteError::UnmappedBinding; }
        }
        const bool ok = (hooks.trace_dispatch != nullptr)
                        && hooks.trace_dispatch(tlas, kb,
                                                containers::ConstSpan<RtHostBinding>(ordered.data(), ordered.size()),
                                                cmd.groups_x, cmd.groups_y, cmd.groups_z, hooks.user);
        if (!ok) { return ExecuteError::TraceDispatchFailed; }
    }
    return ExecuteError::None;
}
// ── CEIR-20b: the ceir.work executor (the execute_rt_lowered mirror; see execute.hpp).
// ──────────────────────────────────
namespace
{
// The GRID-operand prefix of a work op — the operands to SKIP to reach the RESOURCE DESCRIPTORS. ⛔ The queue/src
// operands are DESCRIPTORS (the producer WRITES its queue; compact reads src + writes dst), so only the launch grid is
// skipped: produce = 3 (grid 0-2, then queue + bindings), consume = 0 (queue + bindings), compact = 0 (src + dst +
// bindings). kNotWorkOp ⇒ not a work op.
constexpr crd::u32 kNotWorkOp = 0xFFFFFFFFU;
[[nodiscard]] crd::u32 work_grid_prefix(containers::StringView nm) noexcept
{
    if (nm == containers::StringView("work.produce"))
    {
        return 3U;
    }
    if (nm == containers::StringView("work.consume"))
    {
        return 0U;
    }
    if (nm == containers::StringView("work.compact"))
    {
        return 0U;
    }
    return kNotWorkOp;
}
// Look a descriptor operand up in the caller's WorkResolvedBinding table by resource_root (queues pass through
// resource_root unchanged, so queues + buffers key uniformly). 0 ⇒ unmapped.
[[nodiscard]] WorkBufferHandle work_lookup(const Context& ctx, const Value* operand,
                                           containers::ConstSpan<WorkResolvedBinding> table)
{
    const Value* const root = ctx.resource_root(operand);
    for (crd::u32 k = 0; k < static_cast<crd::u32>(table.size()); ++k)
    {
        if (table[k].resource == root)
        {
            return table[k].buffer;
        }
    }
    return 0U;
}
} // namespace

ExecuteError validate_work_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                   const WorkHooks& hooks, containers::ConstSpan<WorkResolvedBinding> bindings)
{
    (void)bindings; // the pure half checks STRUCTURE (kind/kernel/%queue), not the resolved handles (those bind at
                    // execute)
    for (crd::u32 i = 0; i < static_cast<crd::u32>(commands.size()); ++i)
    {
        const LoweredCommand& cmd = commands[i];
        if (cmd.kind == LoweredKind::Barrier)
        {
            continue;
        } // inert (submit+wait per stage)
        if (cmd.kind != LoweredKind::Dispatch && cmd.kind != LoweredKind::DispatchIndirect)
        {
            return ExecuteError::UnsupportedCommand; // a Transfer/render/RT kind is not the work surface
        }
        const Operation* const op = cmd.op;
        if (op == nullptr)
        {
            return ExecuteError::UnsupportedCommand;
        }
        const crd::u32 gp = work_grid_prefix(ctx.op_name(op->kind()));
        if (gp == kNotWorkOp)
        {
            return ExecuteError::UnsupportedCommand;
        } // a non-work Dispatch (a plain compute.dispatch)
        // a DispatchIndirect sizes from its %queue (the first descriptor, operand `gp`) — guard it is present.
        if (cmd.kind == LoweredKind::DispatchIndirect && op->num_operands() <= gp)
        {
            return ExecuteError::UnresolvedQueue;
        }
        const containers::ConstSpan<crd::u8> kb =
            (hooks.kernel_bytes != nullptr) ? hooks.kernel_bytes(op, hooks.user) : containers::ConstSpan<crd::u8>{};
        if (kb.size() == 0U)
        {
            return ExecuteError::UnresolvedKernel;
        }
    }
    return ExecuteError::None;
}

ExecuteError execute_work_lowered(const Context& ctx, containers::ConstSpan<LoweredCommand> commands,
                                  const WorkHooks& hooks, containers::ConstSpan<WorkResolvedBinding> bindings)
{
    const ExecuteError verr = validate_work_lowered(ctx, commands, hooks, bindings);
    if (verr != ExecuteError::None)
    {
        return verr;
    }

    containers::Array<WorkBufferHandle> handles(
        ctx.allocator()); // scratch: a stage's resource descriptors in operand order
    for (crd::u32 i = 0; i < static_cast<crd::u32>(commands.size()); ++i)
    {
        const LoweredCommand& cmd = commands[i];
        if (cmd.kind == LoweredKind::Barrier)
        {
            // ⛔ REPLAY the inter-stage hazard (the DEVICE-RESIDENT model — the RtHostBinding host-span mirror made
            // these inert; device-resident does NOT). lower_region's conservative whole-Memory gather emits a
            // nullptr-resource barrier between every work-stage pair; hazard_access maps the kind → (from,to); a
            // nullptr resource ⇒ barrier every table buffer.
            if (hooks.barrier != nullptr)
            {
                crd::gpu::ComputeAccess from = crd::gpu::ComputeAccess::ShaderWrite;
                crd::gpu::ComputeAccess to = crd::gpu::ComputeAccess::ShaderRead;
                hazard_access(cmd.hazard, from, to);
                if (cmd.resource == nullptr)
                {
                    for (crd::u32 k = 0; k < static_cast<crd::u32>(bindings.size()); ++k)
                    {
                        (void)hooks.barrier(bindings[k].buffer, from, to, hooks.user);
                    }
                }
                else
                {
                    const WorkBufferHandle h = work_lookup(ctx, cmd.resource, bindings);
                    if (h != 0U)
                    {
                        (void)hooks.barrier(h, from, to, hooks.user);
                    }
                }
            }
            continue;
        }
        const Operation* const op = cmd.op;
        const crd::u32 gp = work_grid_prefix(ctx.op_name(op->kind()));

        // the RESOURCE descriptors: operands `gp`.. (the queue/src, then the bindings) → the caller's named device
        // buffers.
        handles.clear();
        const crd::u32 nops = op->num_operands();
        for (crd::u32 b = gp; b < nops; ++b)
        {
            const WorkBufferHandle h = work_lookup(ctx, op->operand(b), bindings);
            if (h == 0U)
            {
                return ExecuteError::UnmappedBinding;
            }
            handles.push_back(h);
        }

        const containers::ConstSpan<crd::u8> kb =
            (hooks.kernel_bytes != nullptr) ? hooks.kernel_bytes(op, hooks.user) : containers::ConstSpan<crd::u8>{};
        if (kb.size() == 0U)
        {
            return ExecuteError::UnresolvedKernel;
        }
        const containers::ConstSpan<WorkBufferHandle> hspan(handles.data(), handles.size());

        bool ok = false;
        if (cmd.kind == LoweredKind::Dispatch) // produce: a DIRECT dispatch over the authored const grid
        {
            ok = (hooks.dispatch != nullptr) &&
                 hooks.dispatch(op, kb, hspan, cmd.groups_x, cmd.groups_y, cmd.groups_z, hooks.user);
        }
        else // DispatchIndirect: consume/compact — the DEVICE reads the %queue's (first-descriptor) count; the host
             // never sizes it.
        {
            const WorkBufferHandle queue =
                handles.size() > 0U ? handles[0] : 0U; // operand `gp` = the queue/src = handles[0]
            if (queue == 0U)
            {
                return ExecuteError::UnresolvedQueue;
            }
            // ⛔ the one hazard the plan CAN'T express (no HazardKind maps to an indirect-args read): the
            // produce-written queue count read by vkCmdDispatchIndirect — the executor OWNS it (the C5
            // ComputeAccess::IndirectRead pattern, added for this).
            if (hooks.barrier != nullptr)
            {
                (void)hooks.barrier(queue, crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::IndirectRead,
                                    hooks.user);
            }
            ok = (hooks.dispatch_indirect != nullptr) && hooks.dispatch_indirect(op, queue, kb, hspan, hooks.user);
        }
        if (!ok)
        {
            return ExecuteError::WorkDispatchFailed;
        }
    }
    return ExecuteError::None;
}

} // namespace crd::ceir::gpu
