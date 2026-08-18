#include <crd/ceir/gpu/tensor_pipeline_exec.hpp>

namespace crd::ceir::gpu
{
ExecuteError validate_tensor_pipeline(const TensorPipelinePlan& plan, crd::u32 n_buffers)
{
    for (usize s = 0; s < plan.stages.size(); ++s)
    {
        const PlanStage& st = plan.stages[s];
        if (st.nbind > 8U) { return ExecuteError::BindingArity; }
        for (crd::u32 i = 0; i < st.nbind; ++i)
        {
            if (st.bind[i] < 0 || static_cast<crd::u32>(st.bind[i]) >= n_buffers) { return ExecuteError::UnmappedBinding; }
        }
    }
    return ExecuteError::None;
}

ExecuteError execute_tensor_pipeline(const TensorPipelinePlan& plan, crd::gpu::ComputeRecorder& rec, StageResolveFn resolve,
                                     void* user, containers::ConstSpan<crd::gpu::ComputeBuffer*> buffers)
{
    const ExecuteError v = validate_tensor_pipeline(plan, static_cast<crd::u32>(buffers.size()));
    if (v != ExecuteError::None) { return v; }

    for (usize s = 0; s < plan.stages.size(); ++s)
    {
        const PlanStage&    st = plan.stages[s];
        const ResolvedStage rs = resolve(st, user);
        if (rs.pipeline == nullptr) { return ExecuteError::UnresolvedKernel; }

        // assemble the ordered ComputeBuffer* bindings (the 13a positional-slot order the emitters + eval_cpu_kernel share).
        crd::gpu::ComputeBuffer* binds[8] = {};
        for (crd::u32 i = 0; i < st.nbind; ++i) { binds[i] = buffers[static_cast<usize>(st.bind[i])]; }
        rec.dispatch(*rs.pipeline, containers::ConstSpan<crd::gpu::ComputeBuffer*>(binds, st.nbind), rs.push, rs.push_size,
                     rs.gx, rs.gy, rs.gz);

        // inter-stage barrier: after every NON-final stage, ShaderWrite→ShaderRead on each written output (the last n_out binds)
        // — so the next stage reads device-resident data (no round-trip; the §137 chain). The caller owns the boundary
        // transitions (upload→ShaderRead before, ShaderWrite→TransferSrc/HostRead after the terminal stage).
        if (s + 1U != plan.stages.size())
        {
            for (crd::u32 o = 0; o < st.n_out; ++o)
            {
                const crd::u32 bi = st.nbind - st.n_out + o; // the o-th output bind (outputs are the trailing binds)
                rec.barrier(*buffers[static_cast<usize>(st.bind[bi])], crd::gpu::ComputeAccess::ShaderWrite,
                            crd::gpu::ComputeAccess::ShaderRead);
            }
        }
    }
    return ExecuteError::None;
}
} // namespace crd::ceir::gpu
