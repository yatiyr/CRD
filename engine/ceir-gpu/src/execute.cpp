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
        crd::gpu::ComputePipeline* pipe = nullptr;
        const ExecuteError         err  = check_dispatch(ctx, cmd, resolver, user, bindings, &pipe, &bufs);
        if (err != ExecuteError::None) { return err; }
        rec.dispatch(*pipe, containers::ConstSpan<crd::gpu::ComputeBuffer*>(bufs.data(), bufs.size()), nullptr, 0U,
                     cmd.groups_x, cmd.groups_y, cmd.groups_z);
    }
    return ExecuteError::None;
}
} // namespace crd::ceir::gpu
