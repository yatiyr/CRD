#include <crd/ceir/gpu/tensor_pipeline_exec.hpp>

#include <crd/ceir/gpu/ckir_synth.hpp> // CEIR-30b-1: synth_gemm/reduce/transpose/broadcast/elementwise — the graph-tier CKIR
#include <crd/ceir/ir.hpp>             // CEIR-30b-2b-1: Operation::attr/operand + Value::defining_op — the VizDispatch symbol/grid reads
#include <crd/kir/ckir.hpp>            // CEIR-30b-1: kir::KGraph (the synthesized per-stage graph)
#include <crd/kir/ckir_eval.hpp>       // CEIR-30b-1: kir::eval_cpu — the f32-faithful CPU reference (the Host executor's backend)
#include <crd/kir/ckir_kernel_eval.hpp> // CEIR-30b-2b-1: kir::eval_cpu_kernel + KernelBuffer — the authored-kernel (VizDispatch) CPU path

#include <crd/containers/array.hpp> // CEIR-30b-1: the per-stage f32↔f64 conversion scratch

namespace crd::ceir::gpu
{
namespace
{
// CEIR-30b-1: synthesize the stage's op into `g` (graph-tier CKIR) and return the output node, or -1 (reject / not graph-tier).
// ⛔ the switch is TOTAL over StageKind (no default) so a NEW kind trips -Wswitch (gcc -Werror) — a silent "unsupported" would
// mask a real gap. Fft is kernel-tier (eval_cpu_kernel); VizDispatch/Dequant/QuantGemm are authored .ckir — a CPU path for those
// is name-forward, and until then a Host stage of that kind is a TYPED UnresolvedKernel (never a silent skip). n_out is 1 for
// every graph-tier synth (one output node), which the caller relies on for the trailing-output write-back.
[[nodiscard]] int synth_stage_host(const Context& ctx, const PlanStage& st, kir::KGraph& g)
{
    if (st.op == nullptr) { return -1; } // a partition-metadata stage carries no op — standalone-robust like synth_gemm's guard
    switch (st.kind)
    {
    case StageKind::Gemm: { const GraphSynth s = synth_gemm(ctx, *st.op, g, GemmEpilogue::None); return s.reject == SynthReject::None ? s.output : -1; }
    case StageKind::GemmRelu: { const GraphSynth s = synth_gemm(ctx, *st.op, g, GemmEpilogue::Relu); return s.reject == SynthReject::None ? s.output : -1; }
    case StageKind::Reduce: { const GraphSynth s = synth_reduce(ctx, *st.op, g); return s.reject == SynthReject::None ? s.output : -1; }
    case StageKind::Transpose: { const GraphSynth s = synth_transpose(ctx, *st.op, g); return s.reject == SynthReject::None ? s.output : -1; }
    case StageKind::Broadcast: { const GraphSynth s = synth_broadcast(ctx, *st.op, g); return s.reject == SynthReject::None ? s.output : -1; }
    case StageKind::Elementwise: { const GraphSynth s = synth_elementwise(ctx, *st.op, g); return s.reject == SynthReject::None ? s.output : -1; }
    case StageKind::Fft:
    case StageKind::VizDispatch:
    case StageKind::Dequant:
    case StageKind::QuantGemm: return -1; // not graph-tier — the CPU oracle does not run these yet (name-forward)
    }
    return -1; // unreachable (the switch is total) — silences a missing-return on a widened enum
}

// CEIR-30b-2b-1: the dispatch grid dim from an arith.const operand (the VizDispatch grid is authored as index consts; the CUDA
// resolver's const_grid mirror). A non-const / non-positive operand ⇒ 1 (a single workgroup — the relu/viz kernels' grid).
[[nodiscard]] crd::u32 const_grid_dim(const Context& ctx, const Value* v)
{
    const Operation* const d = v->defining_op();
    if (d == nullptr) { return 1U; }
    const AttrValue av = ctx.attr_value(d->attr(containers::StringView("value")));
    return av.i > 0 ? static_cast<crd::u32>(av.i) : 1U;
}

// CEIR-30b-2b-1: run a VizDispatch stage's AUTHORED kernel on the CPU (kir::eval_cpu_kernel), the kernel-tier mirror of the CUDA
// resolver's VizDispatch branch. Resolve the `kernel` symbol → CKIR graph+entry (caller-owned .ckir load), bind the sentinel
// local_size from the WRITE operand's numel, f64 ALL binds (eval_cpu_kernel reads+writes in place), eval, then f64→f32 the
// OUTPUTS ONLY. ⛔ no resolver ⇒ UnresolvedKernel (the 30b-1 behavior). n_out==0 handled by the caller's guard.
[[nodiscard]] ExecuteError eval_viz_stage_host(const Context& ctx, const TensorPipelinePlan& plan, const PlanStage& st,
                                               containers::ConstSpan<crd::f32*> buffers, memory::IAllocator* alloc,
                                               const HostRunOptions& opts)
{
    if (opts.kernel == nullptr || st.op == nullptr) { return ExecuteError::UnresolvedKernel; }
    kir::KGraph g(alloc);
    kir::KEntry entry;
    const AttrValue kv = ctx.attr_value(st.op->attr(containers::StringView("kernel")));
    if (!opts.kernel(kv.s, g, entry, opts.user)) { return ExecuteError::UnresolvedKernel; } // unknown symbol / bad .ckir read

    // bind the sentinel local_size from the WRITE operand's numel (== the trailing output buffer's f32 count). KernelBuffer.len
    // is i32 — reject an over-i32 numel rather than silently truncate.
    const usize    out_bi   = static_cast<usize>(st.bind[st.nbind - 1U]);
    const crd::u64 out_elem = plan.buffers[out_bi].bytes / sizeof(crd::f32);
    if (out_elem > 0x7fffffffULL) { return ExecuteError::UnresolvedKernel; }
    const KernelShapeError kse = bind_authored_local_size(entry.local_size[0], out_elem, kMaxAuthoredLocalSize);
    if (kse != KernelShapeError::None) { return ExecuteError::UnresolvedKernel; } // unbound / over the single-workgroup cap

    // f64 ALL binds into one contiguous scratch; KernelBuffer{data, len, set=0, binding=i} (the authored kernels' slot-order
    // decls — relu in@0/out@1). eval_cpu_kernel reads+writes in place, so EVERY bind is materialized f64.
    crd::u64 total = 0;
    for (crd::u32 i = 0; i < st.nbind; ++i) { total += plan.buffers[static_cast<usize>(st.bind[i])].bytes / sizeof(crd::f32); }
    containers::Array<crd::f64> f64buf(alloc);
    f64buf.resize(static_cast<usize>(total), 0.0);
    kir::KernelBuffer kb[8];
    crd::u64          cursor = 0;
    for (crd::u32 i = 0; i < st.nbind; ++i)
    {
        const usize     bi  = static_cast<usize>(st.bind[i]);
        const crd::u64  ne  = plan.buffers[bi].bytes / sizeof(crd::f32);
        crd::f64* const dst = f64buf.data() + cursor;
        const crd::f32* src = buffers[bi];
        for (crd::u64 e = 0; e < ne; ++e) { dst[e] = static_cast<crd::f64>(src[e]); }
        kb[i].data    = dst;
        kb[i].len     = static_cast<crd::i32>(ne);
        kb[i].set     = 0U;
        kb[i].binding = static_cast<crd::u8>(i);
        cursor += ne;
    }
    const crd::u32 groups = const_grid_dim(ctx, st.op->operand(0U));
    kir::eval_cpu_kernel(g, entry, kb, static_cast<int>(st.nbind), entry.local_size[0], alloc, groups);

    // f64→f32 back-convert OUTPUTS ONLY (the trailing n_out binds). A kernel may clobber a scratch INPUT in place; the plan still
    // considers the host input array valid, so writing an input back would corrupt it — only the declared outputs are results.
    const crd::u32 n_in = st.nbind - st.n_out;
    cursor              = 0;
    for (crd::u32 i = 0; i < st.nbind; ++i)
    {
        const usize    bi = static_cast<usize>(st.bind[i]);
        const crd::u64 ne = plan.buffers[bi].bytes / sizeof(crd::f32);
        if (i >= n_in)
        {
            const crd::f64* srcd = f64buf.data() + cursor;
            crd::f32* const outp = buffers[bi];
            for (crd::u64 e = 0; e < ne; ++e) { outp[e] = static_cast<crd::f32>(srcd[e]); }
        }
        cursor += ne;
    }
    return ExecuteError::None;
}

// CEIR-30b-2b-1: the §137 structural profile row (shared by the graph-tier + VizDispatch host paths). CPU: no workgroup grid.
void push_stage_profile(TensorPipelineProfile& profile, const TensorPipelinePlan& plan, const PlanStage& st)
{
    TensorStageProfile sp;
    sp.kind       = st.kind;
    sp.gx         = 1U;
    sp.gy         = 1U;
    sp.gz         = 1U;
    sp.workgroups = 0U;
    const crd::u32 n_in = st.nbind - st.n_out;
    for (crd::u32 i = 0; i < st.nbind; ++i)
    {
        const crd::u64 b = plan.buffers[static_cast<usize>(st.bind[i])].bytes;
        if (i < n_in) { sp.bytes_in += b; }
        else { sp.bytes_out += b; }
    }
    profile.stages.push_back(sp);
}
} // namespace

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
                                     void* user, containers::ConstSpan<crd::gpu::ComputeBuffer*> buffers,
                                     TensorPipelineProfile* profile)
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

        // §137 STRUCTURAL PROFILE row (record-time; the grid from the resolved stage, the bytes from the plan buffers). Inputs
        // are the first nbind−n_out binds, outputs the trailing n_out (the plan's positional-slot contract).
        if (profile != nullptr)
        {
            TensorStageProfile sp;
            sp.kind       = st.kind;
            sp.gx         = rs.gx;
            sp.gy         = rs.gy;
            sp.gz         = rs.gz;
            sp.workgroups = static_cast<crd::u64>(rs.gx) * rs.gy * rs.gz;
            const crd::u32 n_in = st.nbind - st.n_out;
            for (crd::u32 i = 0; i < st.nbind; ++i)
            {
                const crd::u64 b = plan.buffers[static_cast<usize>(st.bind[i])].bytes;
                if (i < n_in) { sp.bytes_in += b; }
                else { sp.bytes_out += b; }
            }
            profile->stages.push_back(sp);
        }

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

ExecuteError execute_tensor_pipeline_host(const Context& ctx, const TensorPipelinePlan& plan,
                                          containers::ConstSpan<crd::f32*> buffers, memory::IAllocator* alloc,
                                          TensorPipelineProfile* profile, HostRunOptions opts)
{
    const ExecuteError v = validate_tensor_pipeline(plan, static_cast<crd::u32>(buffers.size()));
    if (v != ExecuteError::None) { return v; }

    for (usize s = 0; s < plan.stages.size(); ++s)
    {
        const PlanStage& st = plan.stages[s];

        // VizDispatch: the AUTHORED kernel-tier path (eval_cpu_kernel), gated on opts.kernel. n_out==0 (a resultless dispatch)
        // is a structural fault the write-back relies against — reject BEFORE resolving so a 0-output dispatch is not run blind.
        if (st.kind == StageKind::VizDispatch)
        {
            if (st.n_out == 0U || st.n_out > st.nbind) { return ExecuteError::BindingArity; }
            const ExecuteError ee = eval_viz_stage_host(ctx, plan, st, buffers, alloc, opts);
            if (ee != ExecuteError::None) { return ee; }
            if (profile != nullptr) { push_stage_profile(*profile, plan, st); }
            continue;
        }

        kir::KGraph g(alloc);
        const int   output_node = synth_stage_host(ctx, st, g);
        // classify FIRST: a reject or a NON-graph-tier kind (e.g. a multi-output Fft) is UnresolvedKernel — the kind, not the
        // arity, is what is wrong. NEVER a silent skip.
        if (output_node < 0) { return ExecuteError::UnresolvedKernel; }
        // a graph-tier synth produced EXACTLY ONE output node; the trailing-output write-back below relies on it — a graph-tier
        // stage claiming any other n_out is a structural fault (it would leave an output bind unwritten), not a partial write.
        if (st.n_out != 1U || st.nbind < st.n_out) { return ExecuteError::BindingArity; }

        // f32→f64 each INPUT bind (the first nbind−n_out) into one contiguous scratch, then f64 out, all carved by offset (RAII-
        // freed). eval_cpu reads inputs[iidx]; synth builds Input nodes in operand order == the plan's positional bind order.
        const crd::u32 n_in     = st.nbind - st.n_out;
        const usize    out_bi   = static_cast<usize>(st.bind[st.nbind - 1U]); // the single graph-tier output = the last bind
        const crd::u64 out_elem = plan.buffers[out_bi].bytes / sizeof(crd::f32);
        crd::u64       total    = out_elem;
        for (crd::u32 i = 0; i < n_in; ++i) { total += plan.buffers[static_cast<usize>(st.bind[i])].bytes / sizeof(crd::f32); }

        containers::Array<crd::f64> f64buf(alloc);
        f64buf.resize(static_cast<usize>(total), 0.0);
        const crd::f64* ins[8]  = {};
        crd::u64        cursor  = 0;
        for (crd::u32 i = 0; i < n_in; ++i)
        {
            const usize     bi  = static_cast<usize>(st.bind[i]);
            const crd::u64  ne  = plan.buffers[bi].bytes / sizeof(crd::f32);
            crd::f64* const dst = f64buf.data() + cursor;
            const crd::f32* src = buffers[bi];
            for (crd::u64 e = 0; e < ne; ++e) { dst[e] = static_cast<crd::f64>(src[e]); }
            ins[i] = dst;
            cursor += ne;
        }
        crd::f64* const out64 = f64buf.data() + cursor;
        kir::eval_cpu(g, ins, alloc, output_node, out64);

        // f64→f32 write-back: an F32 node's value is already exactly F32-representable (eval_cpu rounded every op to F32), so the
        // down-cast is LOSSLESS (the gate asserts the f32 bit pattern round-trips). Written ONLY on success — no partial write.
        crd::f32* const outp = buffers[out_bi];
        for (crd::u64 e = 0; e < out_elem; ++e) { outp[e] = static_cast<crd::f32>(out64[e]); }

        if (profile != nullptr) { push_stage_profile(*profile, plan, st); }
    }
    return ExecuteError::None;
}

containers::StringView transfer_dir_name(TransferDir d) noexcept
{
    switch (d)
    {
    case TransferDir::HostToDevice: return containers::StringView("host_to_device");
    case TransferDir::DeviceToHost: return containers::StringView("device_to_host");
    }
    return containers::StringView("?");
}

namespace
{
// CEIR-30b-2a: the LANDLORD of a plan buffer — a tenant/alias shares the realized buffer's storage (alias_of<i), so its bytes live
// in the landlord's slot; a transfer must name the landlord (transferring the tenant index would be redundant/wrong).
[[nodiscard]] crd::u32 landlord_of(const TensorPipelinePlan& plan, crd::u32 i) noexcept
{
    const crd::i32 a = plan.buffers[static_cast<usize>(i)].alias_of;
    return a >= 0 ? static_cast<crd::u32>(a) : i;
}
} // namespace

TransferPlan plan_transfers(const TensorPipelinePlan& plan, containers::ConstSpan<ProviderClass> stage_class,
                            memory::IAllocator* alloc)
{
    TransferPlan out(alloc);
    const usize  nb = plan.buffers.size();
    const usize  ns = plan.stages.size();
    if (stage_class.size() != ns) { return out; } // a size mismatch is a caller bug — empty plan, defensive (no error channel)

    // per LANDLORD buffer: the last stage that WROTE it (-1 = an ExternalIn / unwritten) + the class its authoritative contents
    // live on + whether it has contents yet. ExternalIn buffers are Host-BORN (the documented model — the caller holds the f32).
    containers::Array<crd::i32>     writer_stage(alloc);
    containers::Array<ProviderClass> writer_class(alloc);
    containers::Array<crd::u8>       has_contents(alloc);
    writer_stage.resize(nb, -1);
    writer_class.resize(nb, ProviderClass::Host);
    has_contents.resize(nb, 0U);
    // dedupe: the writer_stage for which a transfer of THIS direction was already emitted (a re-read on the same class with no
    // intervening write crosses ONCE). -2 = none emitted (distinct from a -1 ExternalIn writer).
    containers::Array<crd::i32> xfer_h2d(alloc);
    containers::Array<crd::i32> xfer_d2h(alloc);
    xfer_h2d.resize(nb, -2);
    xfer_d2h.resize(nb, -2);

    for (usize i = 0; i < nb; ++i)
    {
        if (plan.buffers[i].role != BufferRole::ExternalIn) { continue; }
        const crd::u32 lbl        = landlord_of(plan, static_cast<crd::u32>(i));
        writer_class[lbl]         = ProviderClass::Host; // Host-born
        writer_stage[lbl]         = -1;
        has_contents[lbl]         = 1U;
    }

    for (usize s = 0; s < ns; ++s)
    {
        const PlanStage&    st = plan.stages[s];
        const ProviderClass sc = stage_class[s];
        if (st.n_out > st.nbind) { continue; } // malformed stage — skip (validate_tensor_pipeline owns the hard reject)
        const crd::u32 n_in = st.nbind - st.n_out;

        // READS (the first n_in binds) — cross the boundary if the buffer's contents live on a different class.
        for (crd::u32 i = 0; i < n_in; ++i)
        {
            const crd::u32 lbl = landlord_of(plan, static_cast<crd::u32>(st.bind[i]));
            if (has_contents[lbl] == 0U || writer_class[lbl] == sc) { continue; }
            const TransferDir dir  = (sc == ProviderClass::Gpu) ? TransferDir::HostToDevice : TransferDir::DeviceToHost;
            crd::i32&         mark = (dir == TransferDir::HostToDevice) ? xfer_h2d[lbl] : xfer_d2h[lbl];
            if (mark == writer_stage[lbl]) { continue; } // already transferred this writer's contents this direction (dedupe)
            Transfer t;
            t.buffer       = lbl;
            t.before_stage = static_cast<crd::u32>(s);
            t.direction    = dir;
            out.transfers.push_back(t);
            mark = writer_stage[lbl];
        }
        // WRITES (the trailing n_out binds) — a new write makes this class authoritative + invalidates prior transfers.
        for (crd::u32 o = 0; o < st.n_out; ++o)
        {
            const crd::u32 lbl = landlord_of(plan, static_cast<crd::u32>(st.bind[st.nbind - st.n_out + o]));
            writer_stage[lbl]  = static_cast<crd::i32>(s);
            writer_class[lbl]  = sc;
            has_contents[lbl]  = 1U;
            xfer_h2d[lbl]      = -2;
            xfer_d2h[lbl]      = -2;
        }
    }

    // the Output MUST end Host-visible: a terminal Gpu writer needs a final readback (before_stage == num_stages).
    for (usize i = 0; i < nb; ++i)
    {
        if (plan.buffers[i].role != BufferRole::Output) { continue; }
        const crd::u32 lbl = landlord_of(plan, static_cast<crd::u32>(i));
        if (has_contents[lbl] != 0U && writer_class[lbl] == ProviderClass::Gpu)
        {
            Transfer t;
            t.buffer       = lbl;
            t.before_stage = static_cast<crd::u32>(ns);
            t.direction    = TransferDir::DeviceToHost;
            out.transfers.push_back(t);
        }
    }
    return out;
}

containers::Array<ProviderClass> stage_class_from_partition(const TensorPipelinePlan& plan,
                                                            containers::ConstSpan<MlProvider> providers,
                                                            ProviderClass fallback_class, memory::IAllocator* alloc)
{
    containers::Array<ProviderClass> out(alloc);
    out.reserve(plan.stages.size());
    for (usize s = 0; s < plan.stages.size(); ++s)
    {
        const crd::i32 p = plan.stages[s].provider;
        if (p >= 0 && static_cast<usize>(p) < providers.size()) { out.push_back(providers[static_cast<usize>(p)].provider_class); }
        else { out.push_back(fallback_class); }
    }
    return out;
}

containers::Array<ProviderClass> stage_class_from_placement(const TensorPipelinePlan& plan,
                                                            containers::ConstSpan<ProviderClass> rank_classes,
                                                            ProviderClass fallback_class, memory::IAllocator* alloc)
{
    containers::Array<ProviderClass> out(alloc);
    out.reserve(plan.stages.size());
    for (usize s = 0; s < plan.stages.size(); ++s)
    {
        const crd::i32 p = plan.stages[s].provider;
        if (p >= 0 && static_cast<usize>(p) < rank_classes.size()) { out.push_back(rank_classes[static_cast<usize>(p)]); }
        else { out.push_back(fallback_class); }
    }
    return out;
}

TensorPipelinePlan slice_plan(const TensorPipelinePlan& plan, crd::usize lo, crd::usize hi, memory::IAllocator* alloc)
{
    TensorPipelinePlan sub(alloc);
    for (usize i = 0; i < plan.buffers.size(); ++i) { sub.buffers.push_back(plan.buffers[i]); } // the shared table — indices unchanged
    for (usize s = lo; s < hi; ++s) { sub.stages.push_back(plan.stages[s]); }
    return sub;
}

ExecuteError execute_two_class(const Context& ctx, const TensorPipelinePlan& plan,
                               containers::ConstSpan<ProviderClass> stage_class, containers::ConstSpan<crd::f32*> host_bufs,
                               void* gpu_user, GpuStageFn gpu, TransferFn xfer, memory::IAllocator* alloc,
                               HostRunOptions host_opts, TensorPipelineProfile* profile)
{
    const ExecuteError v = validate_tensor_pipeline(plan, static_cast<crd::u32>(host_bufs.size()));
    if (v != ExecuteError::None) { return v; }
    // the placement vector's arity MUST match the plan — plan_transfers silently yields an empty plan on a mismatch, and a runner
    // that proceeded would move NOTHING and evaluate a device stage on stale/sentinel memory.
    if (stage_class.size() != plan.stages.size()) { return ExecuteError::BindingArity; }

    const TransferPlan tp = plan_transfers(plan, stage_class, alloc);
    const usize        ns = plan.stages.size();

    // apply every transfer scheduled BEFORE stage `s` (before_stage == ns is the final Output readback, applied after the loop). ⛔
    // this preserves tp.transfers ORDER within a stage, and stages run ascending, so the applied order == tp order (plan_transfers
    // emits before_stage-ascending) — a gate asserts log == tp by index, so a future re-sort of tp must keep that or fix the gate.
    const auto apply_before = [&](usize s) -> ExecuteError
    {
        for (usize t = 0; t < tp.transfers.size(); ++t)
        {
            const Transfer& xf = tp.transfers[t];
            if (xf.before_stage != static_cast<crd::u32>(s)) { continue; }
            const crd::u64     bytes = plan.buffers[static_cast<usize>(xf.buffer)].bytes;
            const ExecuteError ee    = xfer(xf, host_bufs[static_cast<usize>(xf.buffer)], bytes, gpu_user);
            if (ee != ExecuteError::None) { return ee; }
        }
        return ExecuteError::None;
    };

    for (usize s = 0; s < ns; ++s)
    {
        const ExecuteError te = apply_before(s);
        if (te != ExecuteError::None) { return te; }

        const TensorPipelinePlan slice = slice_plan(plan, s, s + 1U, alloc); // one stage, the shared buffer table (bind[] valid)
        if (stage_class[s] == ProviderClass::Host)
        {
            const ExecuteError ee = execute_tensor_pipeline_host(ctx, slice, host_bufs, alloc, profile, host_opts);
            if (ee != ExecuteError::None) { return ee; }
        }
        else
        {
            const ExecuteError ee = gpu(slice, gpu_user); // the device-class caller records/dispatches/awaits this one stage
            if (ee != ExecuteError::None) { return ee; }
            if (profile != nullptr) { push_stage_profile(*profile, plan, plan.stages[s]); } // Host stages profile themselves inside
        }
    }
    return apply_before(ns); // the final Output readback(s) — the Output ends Host-visible in host_bufs
}
} // namespace crd::ceir::gpu
