// A device program's block run on a GPU from host data. Declared in crd/ceir/gpu/device_run.hpp.

#include <crd/ceir/gpu/device_run.hpp>

#include <crd/ceir/gpu/lower.hpp>
#include <crd/containers/array.hpp>
#include <crd/kir/ckir_asset.hpp> // ckir_read

namespace crd::ceir::gpu
{
namespace
{
namespace cont = crd::containers;

// The dispatches one run compiles (a pipeline each).
constexpr crd::u32 kMaxDispatches = 64U;

// Each dispatch op's pipeline, resolved by op identity.
struct PipelineTable
{
    const Operation*                           ops[kMaxDispatches]{};
    std::unique_ptr<crd::gpu::ComputePipeline> pipes[kMaxDispatches];
    crd::u32                                   n = 0U;
};

crd::gpu::ComputePipeline* resolve_by_op(const Operation* op, void* user)
{
    const auto* const t = static_cast<const PipelineTable*>(user);
    for (crd::u32 i = 0U; i < t->n; ++i)
    {
        if (t->ops[i] == op)
        {
            return t->pipes[i].get();
        }
    }
    return nullptr;
}
} // namespace

bool run_device_block(Context& ctx, const Block& block, cont::ConstSpan<DeviceKernelText> kernels,
                      cont::ConstSpan<HostBufferBinding> buffers, DeviceRunRig& rig, DeviceRunResult& out,
                      cont::String& reason)
{
    ++rig.runs;
    out = DeviceRunResult{};
    if (rig.device == nullptr || rig.compile == nullptr || rig.alloc == nullptr)
    {
        reason.append("the GPU device run has no compute context, compile hook or allocator");
        return false;
    }
    cont::Array<LoweredCommand> cmds(rig.alloc);
    lower_region(ctx, block, cmds);

    PipelineTable table;
    for (const LoweredCommand& cmd : cmds)
    {
        if (cmd.kind != LoweredKind::Dispatch)
        {
            continue;
        }
        if (table.n >= kMaxDispatches)
        {
            reason.append("a GPU device run compiles at most 64 dispatches");
            return false;
        }
        const AttrValue  kv = ctx.attr_value(cmd.op->attr(cont::StringView{"kernel"}));
        cont::StringView text;
        bool             found = false;
        for (const DeviceKernelText& k : kernels)
        {
            if (k.symbol == kv.s)
            {
                text  = k.ckir;
                found = true;
            }
        }
        crd::kir::KGraph g(rig.alloc);
        crd::kir::KEntry e;
        if (!found || !crd::kir::ckir_read(text, g, e).ok)
        {
            reason.append("the CKIR text of kernel @");
            reason.append(kv.s);
            reason.append(found ? cont::StringView{" does not read"} : cont::StringView{" is not given"});
            return false;
        }
        table.ops[table.n] = cmd.op;
        table.pipes[table.n] =
            rig.compile(g, e, static_cast<int>(cmd.op->num_operands()) - 3, rig.compile_user, rig.alloc);
        if (table.pipes[table.n] == nullptr)
        {
            reason.append("kernel @");
            reason.append(kv.s);
            reason.append(" did not compile for the device");
            return false;
        }
        ++table.n;
    }

    DispatchSites sites(rig.alloc);
    out.error    = execute_lowered_host(ctx, cont::ConstSpan<LoweredCommand>(cmds.data(), cmds.size()), *rig.device,
                                        &resolve_by_op, &table, buffers, &sites);
    out.fault_op = sites.fault() != nullptr ? sites.fault()->stable_id().value : 0U;
    return true;
}
} // namespace crd::ceir::gpu
