// CEIR-22c-3e (DX12) — the DirectX-12 leg of the §137 pipeline proof. The SAME authored assets/ceir/tensor_pipeline.ceir is
// PARSE-LOADED, planned, and executed as ONE device-resident submit on a REAL D3D12 device — gemm+reduce (graph tier,
// emit_contract_hlsl/emit_reduce_hlsl) + fft + the two authored viz CKIR dispatches (kernel tier, emit_compute_kernel_hlsl) into
// ONE recorder, the intermediates device-resident (the migrated-executor-runs-both-backends rule). ⛔ the PORTABLE readback
// contract (DX12 forbids a UAV on an upload/readback heap): each logical buffer = a GpuOnly `dev` UAV + a CpuToGpu `up` staging
// buffer (copy → dev) + a GpuToCpu `rb` staging buffer (dev → copy); "shader writes a host-visible buffer then map()" is
// Vulkan-only and does NOT port. The terminal [64] normalized spectrum is validated vs an INDEPENDENT composed f64 reference.

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/passes/canonicalize.hpp> // CEIR-26b-2c: canonicalize_run — the reshape fold must be BIT-EXACT vs the raw device output
#include <crd/ceir/passes/cse.hpp>       // CEIR-26c-2c: cse_run — removing a DUPLICATE gemm dispatch must be BIT-EXACT vs the raw device output
#include <crd/ceir/passes/dce.hpp>       // CEIR-26a-3: dce_run — the DCE'd backward must be BIT-EXACT vs the raw device output
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/ckir_synth.hpp>
#include <crd/ceir/gpu/expand_ml.hpp> // CEIR-24b-4: expand_ml_ops (the ml.attention/ml.mlp -> 22/23 vocab rewrite)
#include <crd/ceir/gpu/grad.hpp>      // CEIR-25b-4b: build_gradient — the backward pass planned + EXECUTED device-resident
#include <crd/ceir/gpu/tensor_pipeline.hpp>
#include <crd/ceir/gpu/tensor_pipeline_exec.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/linalg.hpp>
#include <crd/ceir/ml.hpp> // CEIR-24b-4: ml.attention/ml.mlp + register_dialect
#include <crd/ceir/transform.hpp> // CEIR-27b: register_transform_ops + find_transform_misuse (the parsed schedule assets)
#include <crd/ceir/tune.hpp> // CEIR-28d: register_tune_ops + program_hash + find_tune_misuse (the DX12 measurer mirror)
#include <crd/ceir/gen/tune_ops.hpp> // CEIR-28d: tune::build_entry
#include <crd/ceir/parse.hpp>
#include <crd/ceir/quant.hpp> // CEIR-23b-2d: register quant + build_dequantize (the fused QuantGemm gate)
#include <crd/ceir/tensor.hpp>
#include <crd/ceir/type.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp> // ckir_read (the authored viz kernels)
#include <crd/kir/ckir_hlsl.hpp>  // emit_contract_hlsl / emit_reduce_hlsl / emit_compute_kernel_hlsl

#include <crd/gpu/dx12_compute_context.hpp>
#include <crd/gpu/dx12_context.hpp>

#include <crd/math/cmath.hpp>

#include <crd/hesap/autodiff/gradient_check.hpp> // CEIR-25b-4b: grad_fd — the FD witness on f(A)=sum(gemm(A,A))
#include <crd/hesap/autodiff/nn_reverse.hpp>     // CEIR-25b-4b: matmul_vjp — the analytic (dialect-independent) grad ref

#include "../../gpu/gpu-shared/autodiff_fd_functors.hpp" // CEIR-25c-2: SumGemmAA + MlpLoss FD witnesses (shared with the Vulkan pipeline TU)

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>  // CEIR-28d: std::snprintf — the "dx12:"+adapter_name() device key
#include <cstring> // std::memcpy
#include <fstream>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

namespace ce  = crd::ceir;
namespace ceg = crd::ceir::gpu;
namespace kir = crd::kir;

namespace
{
constexpr int kSide = 8;
constexpr int kL    = kSide * kSide;

crd::u32 dim_ext(ce::Context& c, ce::TypeId t, crd::u32 axis)
{
    const ce::Type sh = c.type_of(c.type_of(t).members[1]);
    return axis < sh.members.size() ? c.type_of(sh.members[static_cast<crd::usize>(axis)]).count : 0U;
}
// CEIR-23b-2d: shape/tensor helpers (the DX12 §137 test parse-loads its module; the fused QuantGemm gate builds one in C++).
ce::TypeId shp(ce::Context& c, crd::containers::ConstSpan<ce::TypeId> d) { return c.type_shape(d); }
ce::TypeId sh1(ce::Context& c, crd::u32 a) { const ce::TypeId d[1] = {c.type_dim_static(a)}; return shp(c, crd::containers::ConstSpan<ce::TypeId>(d, 1U)); } // CEIR-25b-4b: reduce output [N]
ce::TypeId sh2(ce::Context& c, crd::u32 a, crd::u32 b)
{
    const ce::TypeId d[2] = {c.type_dim_static(a), c.type_dim_static(b)};
    return shp(c, crd::containers::ConstSpan<ce::TypeId>(d, 2U));
}
ce::TypeId tf(ce::Context& c, ce::TypeId s) { return c.type_tensor(c.type_f32(), s); }
crd::u64 tnumel(ce::Context& c, ce::TypeId t)
{
    const ce::Type sh = c.type_of(c.type_of(t).members[1]);
    crd::u64       n  = 1;
    for (crd::usize i = 0; i < sh.members.size(); ++i) { n *= c.type_of(sh.members[i]).count; }
    return n;
}
crd::u32 const_grid(ce::Context& c, const ce::Value* v)
{
    const ce::Operation* const d = v->defining_op();
    if (d == nullptr) { return 1U; }
    const ce::AttrValue av = c.attr_value(d->attr(crd::containers::StringView("value")));
    return av.i > 0 ? static_cast<crd::u32>(av.i) : 1U;
}

// The DX12 resolver: re-synthesize/emit each stage to HLSL → a ComputePipeline it OWNS (the create_pipeline_from_hlsl mold).
struct Resolver
{
    ce::Context*                               alloc_ctx = nullptr;
    crd::memory::IAllocator*                   alloc     = nullptr;
    crd::gpu::Dx12ComputeContext*              compute   = nullptr;
    std::unique_ptr<crd::gpu::ComputePipeline> pipes[16]; // the 25c-2 MLP backward has 11 stages (gemm×5/transpose×4/dispatch×2)
    int                                        n = 0;
};

// CEIR-23b-2d: load an authored .ckir compute kernel BY PATH and emit its HLSL (the fused QuantGemm + symmetric Dequant kernels
// are hand-authored, ckir_read like VizDispatch — never ckir_synth'd). Returns false on read/emit failure.
// CEIR-26d-4b: `local_size_x` (default 0 = as-authored) cook-binds a SENTINEL asset (local_size=0) before emit — the emit guard
// refuses an unbound 0, so a sentinel kernel (relu_vjp.ckir) MUST have its local_size bound here (the resolver's job on device).
bool load_emit_ckir_hlsl(const char* path, kir::KGraph& g, kir::GlslKernel& kern, crd::memory::IAllocator* alloc, crd::u32 local_size_x = 0U)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.good()) { return false; }
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    crd::containers::Array<char> src(alloc);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    kir::KEntry ke;
    if (!kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, ke).ok) { return false; }
    if (local_size_x != 0U) { ke.local_size[0] = local_size_x; }
    return kir::emit_compute_kernel_hlsl(g, ke, alloc, kern);
}

ceg::ResolvedStage resolve_stage(const ceg::PlanStage& st, void* user)
{
    auto&              res = *static_cast<Resolver*>(user);
    ce::Context&       c   = *res.alloc_ctx;
    ceg::ResolvedStage rs;
    kir::KGraph        g(res.alloc);
    kir::GlslKernel    kern(res.alloc); // (a source container — emit_*_hlsl writes HLSL into it)
    int                nbind    = 0;
    crd::u32           pushsize = 0;

    if (st.kind == ceg::StageKind::Gemm || st.kind == ceg::StageKind::GemmRelu)
    {
        // ⭐ CEIR-26e (DX12 mirror): GemmRelu re-synthesizes the SAME contract with the relu epilogue (synth_gemm appends
        //    Max(Contract,0), which emit_contract_hlsl UNWRAPS to `max(acc, 0.0)`); push{M,K,N,1} + grid M·N + nbind 3 IDENTICAL.
        const ceg::GemmEpilogue ep = st.kind == ceg::StageKind::GemmRelu ? ceg::GemmEpilogue::Relu : ceg::GemmEpilogue::None;
        const ceg::GraphSynth   s  = ceg::synth_gemm(c, *st.op, g, ep);
        if (s.reject != ceg::SynthReject::None || !kir::emit_contract_hlsl(g, s.output, kern)) { return rs; }
        const crd::u32 m     = dim_ext(c, st.op->operand(0U)->type(), 0U);
        const crd::u32 k     = dim_ext(c, st.op->operand(0U)->type(), 1U);
        const crd::u32 nn    = dim_ext(c, st.op->operand(1U)->type(), 1U);
        const crd::u32 pc[4] = {m, k, nn, 1U};
        std::memcpy(rs.push, pc, sizeof(pc));
        pushsize = 16U;
        rs.gx    = (m * nn + 255U) / 256U;
        nbind    = 3;
    }
    else if (st.kind == ceg::StageKind::Fft)
    {
        const ceg::FftSynth s = ceg::synth_fft(c, *st.op, g);
        if (s.reject != ceg::SynthReject::None || !kir::emit_compute_kernel_hlsl(g, s.plan.entry, res.alloc, kern)) { return rs; }
        rs.gx    = 1U;
        pushsize = 0U;
        nbind    = 6;
    }
    else if (st.kind == ceg::StageKind::Reduce)
    {
        const ceg::GraphSynth s = ceg::synth_reduce(c, *st.op, g);
        if (s.reject != ceg::SynthReject::None || !kir::emit_reduce_hlsl(g, s.output, kern)) { return rs; }
        const crd::u64 in_n  = tnumel(c, st.op->operand(0U)->type());
        const crd::u64 out_n = tnumel(c, st.op->result(0U)->type());
        const crd::u32 pc[4] = {static_cast<crd::u32>(out_n), static_cast<crd::u32>(in_n / (out_n == 0U ? 1U : out_n)), 0U, 0U};
        std::memcpy(rs.push, pc, sizeof(pc));
        pushsize = 16U;
        rs.gx    = (static_cast<crd::u32>(out_n) + 255U) / 256U;
        nbind    = 2;
    }
    else if (st.kind == ceg::StageKind::QuantGemm)
    {
        if (!load_emit_ckir_hlsl(CRD_REPO_DIR "/assets/ckir/quant_gemm_q8.ckir", g, kern, res.alloc)) { return rs; }
        rs.gx    = 1U; // one workgroup of M*N threads
        pushsize = 0U;
        nbind    = static_cast<int>(st.nbind); // 4: A,W_q8,scale,D
    }
    else if (st.kind == ceg::StageKind::Dequant)
    {
        if (!load_emit_ckir_hlsl(CRD_REPO_DIR "/assets/ckir/quant_dequantize_q8_sym.ckir", g, kern, res.alloc)) { return rs; }
        rs.gx    = 1U; // one workgroup of K*N threads
        pushsize = 0U;
        nbind    = static_cast<int>(st.nbind); // 3: W_q8,scale,out
    }
    else if (st.kind == ceg::StageKind::Transpose || st.kind == ceg::StageKind::Broadcast
             || st.kind == ceg::StageKind::Elementwise)
    {
        // CEIR-25b-4a: the autodiff backward vocab — re-synthesize + emit the NEW HLSL root emitters (one thread/output, PC{nout}).
        bool ok = false;
        if (st.kind == ceg::StageKind::Transpose)
        {
            const ceg::GraphSynth s = ceg::synth_transpose(c, *st.op, g);
            ok = s.reject == ceg::SynthReject::None && kir::emit_permute_hlsl(g, s.output, kern);
        }
        else if (st.kind == ceg::StageKind::Broadcast)
        {
            const ceg::GraphSynth s = ceg::synth_broadcast(c, *st.op, g);
            ok = s.reject == ceg::SynthReject::None && kir::emit_broadcast_nd_hlsl(g, s.output, kern);
        }
        else
        {
            const ceg::GraphSynth s = ceg::synth_elementwise(c, *st.op, g);
            ok = s.reject == ceg::SynthReject::None && kir::emit_elementwise_hlsl(g, s.output, res.alloc, kern); // fused emitter needs scratch
        }
        if (!ok) { return rs; }
        const crd::u64 out_n = tnumel(c, st.op->result(0U)->type());
        const crd::u32 pc[4] = {static_cast<crd::u32>(out_n), 0U, 0U, 0U};
        std::memcpy(rs.push, pc, sizeof(pc));
        pushsize = 16U;
        rs.gx    = (static_cast<crd::u32>(out_n) + 255U) / 256U;
        nbind    = static_cast<int>(st.nbind); // 2 (transpose/broadcast) / 3 (elementwise)
    }
    else // VizDispatch — the authored viz .ckir, loaded by kernel symbol (emit HLSL)
    {
        const ce::AttrValue kv   = c.attr_value(st.op->attr(crd::containers::StringView("kernel")));
        const char*         path = nullptr;
        if (kv.s == crd::containers::StringView("viz_magnitude")) { path = CRD_REPO_DIR "/assets/ckir/tensor_viz_magnitude.ckir"; }
        else if (kv.s == crd::containers::StringView("viz_normalize")) { path = CRD_REPO_DIR "/assets/ckir/tensor_viz_normalize.ckir"; }
        else if (kv.s == crd::containers::StringView("relu")) { path = CRD_REPO_DIR "/assets/ckir/relu.ckir"; } // CEIR-23c MLP activation
        else if (kv.s == crd::containers::StringView("relu_vjp")) { path = CRD_REPO_DIR "/assets/ckir/relu_vjp.ckir"; } // CEIR-25c MLP-backward relu VJP
        // CEIR-26d-3: the @transpose baked dispatch is RETIRED — attention's Kᵀ is now the shape-generic tensor.transpose (synth).
        else if (kv.s == crd::containers::StringView("softmax")) { path = CRD_REPO_DIR "/assets/ckir/softmax.ckir"; } // CEIR-24b attention softmax
        if (path == nullptr) { return rs; }
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.good()) { return rs; }
        const std::streamsize sz = f.tellg();
        f.seekg(0);
        crd::containers::Array<char> src(res.alloc);
        src.resize(static_cast<crd::usize>(sz), '\0');
        f.read(src.data(), sz);
        kir::KEntry ve;
        if (!kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, ve).ok) { return rs; }
        // CEIR-26d cook-time kernel-shape specialization (per-kernel, keyed by symbol — the 20b binding-table precedent):
        if (kv.s == crd::containers::StringView("softmax"))
        {
            // CEIR-26d-3/4e: the GENERALIZED row-wise softmax — local_size = Sq (rows) + Sk (cols) as the spec-const loop bound
            // (constant_id 0), both from the probs [Sq,Sk] (write) type. ⛔ 26d-4e: softmax.ckir ships the SENTINEL (local_size=0);
            // route Sq through bind_authored_local_size (extent = Sq=rows) so an oversize Sq > kMaxAuthoredLocalSize (== the
            // groupshared m_sh/d_sh cap 1024) is a TYPED LocalSizeExceedsLimit ⇒ UnresolvedKernel. DX12 bakes the spec-const at cook — set Sk here.
            const crd::u32              wop = 3U + st.nbind - st.n_out; // probs [Sq, Sk]
            const ceg::KernelShapeError kse = ceg::bind_authored_local_size(
                ve.local_size[0], dim_ext(c, st.op->operand(wop)->type(), 0U), ceg::kMaxAuthoredLocalSize); // Sq (one lane per row), capped
            if (kse != ceg::KernelShapeError::None) { return rs; }
            (void)g.set_spec_const(0U, static_cast<crd::f64>(dim_ext(c, st.op->operand(wop)->type(), 1U))); // Sk
        }
        else if (st.n_out >= 1U)
        {
            // a SENTINEL local_size(0) (relu.ckir) binds to the trailing-write operand's numel; a non-sentinel authored size
            // (the viz kernels) is left as-is.
            const crd::u32              wop = 3U + st.nbind - st.n_out; // first trailing-write operand (grid 0..2, then binds)
            const ceg::KernelShapeError kse = ceg::bind_authored_local_size(
                ve.local_size[0], tnumel(c, st.op->operand(wop)->type()), ceg::kMaxAuthoredLocalSize);
            if (kse != ceg::KernelShapeError::None) { return rs; } // unbound / exceeds the single-workgroup cap ⇒ UnresolvedKernel
        }
        if (!kir::emit_compute_kernel_hlsl(g, ve, res.alloc, kern)) { return rs; }
        rs.gx    = const_grid(c, st.op->operand(0U));
        rs.gy    = const_grid(c, st.op->operand(1U));
        rs.gz    = const_grid(c, st.op->operand(2U));
        pushsize = 0U;
        nbind    = static_cast<int>(st.nbind);
    }

    if (res.n >= 16) { return rs; }
    res.pipes[res.n] = res.compute->create_pipeline_from_hlsl(crd::containers::to_view(kern.source), nbind, pushsize);
    if (res.pipes[res.n] == nullptr) { return rs; }
    rs.pipeline  = res.pipes[res.n].get();
    rs.push_size = pushsize;
    ++res.n;
    return rs;
}

// CEIR-23b-2d / 23c-d: one seeded ExternalIn — matched to a plan buffer by SSA Value. Exactly one of {floats, packed} is set.
struct QuantSeed
{
    const ce::Value* value  = nullptr;
    const float*     floats = nullptr;
    const crd::u32*  packed = nullptr;
    crd::u32         count  = 0;
};

// CEIR-25c-2 (b): one readback target — the buffer realizing `value` is copied into `dst` (`len` floats). MULTIPLE outputs let a
// backward pass read BOTH dW1 (Output) AND dW2 (Intermediate) in one submit. On DX12 EVERY dev buffer is GpuOnly and readback is a
// dedicated GpuToCpu `rb` filled by a transfer copy — so an Intermediate output reads back exactly like an Output (no role forcing).
struct QuantOut
{
    const ce::Value* value = nullptr;
    float*           dst   = nullptr;
    crd::usize       len   = 0;
};

// Materialize a planned module's buffers on DX12 (the PORTABLE dev/up/rb pattern), execute it, and read back each `outs[o]` (by SSA
// Value) into its dst. The ONE implementation (single-output callers use the thin wrapper below). ExternalIns matching a seed (by
// Value) are STAGED+uploaded (wq as u32-packed int8 BITS); an unseeded ExternalIn (zp / β=0 C) gets a dev buffer but no upload.
// Returns false only on a device/materialize failure.
bool run_quant_module_n(crd::gpu::Dx12ComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* alloc,
                        const ceg::TensorPipelinePlan& plan, const QuantSeed* seeds, crd::usize n_seeds, const QuantOut* outs,
                        crd::usize n_outs, crd::u32* n_allocated = nullptr)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;
    const crd::usize nb = plan.buffers.size();
    if (nb > 40U || n_outs > 8U) { return false; }
    std::unique_ptr<g::ComputeBuffer> dev[40];
    std::unique_ptr<g::ComputeBuffer> up[40];
    g::ComputeBuffer*                 bufs[40] = {};
    crd::u32                          alloc_count = 0; // CEIR-26f-4: physical create_buffer calls (aliased buffers skip it)
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        // ⛔ CEIR-26f-4 (the Vulkan 26f-3a twin): SHARE the realized buffer for ANY aliased buffer — a reshape VIEW (role Alias) OR a
        //    storage TENANT (role Intermediate, alias_of>=0 from assign_shared_storage). Teardown is RAII on dev[40] (a tenant leaves
        //    dev[i] null ⇒ ONLY the landlord is destroyed, NO double-destroy); the ExternalIn upload keys on role (below) so a tenant
        //    is not seeded; the rb readback keys on the output Value (never a tenant) — all THREE predicates verified vs the runner.
        if (pb.alias_of >= 0) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes; // round the 1-byte int8 zp up (DX12 raw-view alignment)
        dev[i]            = compute.create_buffer(sz, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        if (dev[i] == nullptr) { return false; }
        bufs[i]               = dev[i].get();
        ++alloc_count;
        const QuantSeed* seed = nullptr;
        if (pb.role == ceg::BufferRole::ExternalIn)
        {
            for (crd::usize s = 0; s < n_seeds; ++s) { if (seeds[s].value == pb.value) { seed = &seeds[s]; break; } }
        }
        if (seed == nullptr) { continue; } // Intermediate/Output, and the unbound zp/C ExternalIns — no host seed
        up[i] = compute.create_buffer(sz, transfer_src, g::ComputeMemory::CpuToGpu);
        if (up[i] == nullptr) { return false; }
        void* raw = up[i]->map();
        if (raw == nullptr) { return false; }
        if (seed->packed != nullptr)
        {
            auto* w = static_cast<crd::u32*>(raw);
            for (crd::u32 e = 0; e < seed->count; ++e) { w[e] = seed->packed[e]; } // u32-packed int8 — bits, not float
        }
        else
        {
            auto* d = static_cast<float*>(raw);
            for (crd::u32 e = 0; e < seed->count; ++e) { d[e] = seed->floats[e]; }
        }
        up[i]->unmap();
    }
    // one GpuToCpu readback buffer per named output (a dev-Intermediate reads back the same way as a dev-Output).
    std::unique_ptr<g::ComputeBuffer> rb[8];
    crd::i32                          out_idx[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    for (crd::usize o = 0; o < n_outs; ++o)
    {
        for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == outs[o].value) { out_idx[o] = static_cast<crd::i32>(i); } }
        if (out_idx[o] < 0) { return false; }
        // ⛔ CEIR-26f-4 (advisor): a named-out that is ALIASED (alias_of>=0 — a tenant or view) has dev[out_idx]==null ⇒ the copy
        //    below null-derefs. Safe today because every named-out is func.return'd (⇒ pinned ⇒ not shareable), but this is a TYPED
        //    REJECT for the latent pin-readback misuse (the [[feedback_plan_output_by_traversal_is_not_ssa_liveness...]] scar).
        if (plan.buffers[static_cast<crd::usize>(out_idx[o])].alias_of >= 0) { return false; }
        rb[o] = compute.create_buffer(plan.buffers[static_cast<crd::usize>(out_idx[o])].bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
        if (rb[o] == nullptr) { return false; }
    }

    Resolver res;
    res.alloc_ctx = &ctx;
    res.alloc     = alloc;
    res.compute   = &compute;
    auto& rec     = compute.begin();
    for (crd::usize i = 0; i < nb; ++i) { if (up[i] != nullptr) { rec.copy(*up[i], *dev[i], 0U, 0U, plan.buffers[i].bytes); } }
    for (crd::usize i = 0; i < nb; ++i) { if (up[i] != nullptr) { rec.barrier(*dev[i], g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead); } }
    const ceg::ExecuteError ee = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res,
                                                              crd::containers::ConstSpan<g::ComputeBuffer*>(bufs, nb));
    if (ee != ceg::ExecuteError::None) { return false; }
    // ⛔ record EVERY out barrier + copy BEFORE the single submit_and_wait (a barrier/copy after submit is a no-op → the 2nd readback
    //    races). The GpuToCpu rb is host-visible after the copy + the submit's fence — the existing single-output pattern, no rb barrier.
    for (crd::usize o = 0; o < n_outs; ++o)
    {
        rec.barrier(*dev[static_cast<crd::usize>(out_idx[o])], g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
        rec.copy(*dev[static_cast<crd::usize>(out_idx[o])], *rb[o], 0U, 0U, plan.buffers[static_cast<crd::usize>(out_idx[o])].bytes);
    }
    compute.submit_and_wait();
    for (crd::usize o = 0; o < n_outs; ++o)
    {
        const auto* got = static_cast<const float*>(rb[o]->map());
        if (got == nullptr) { return false; }
        for (crd::usize e = 0; e < outs[o].len; ++e) { outs[o].dst[e] = got[e]; }
        rb[o]->unmap();
    }
    if (n_allocated != nullptr) { *n_allocated = alloc_count; } // CEIR-26f-4: distinct physical buffers (26f-3b-mirror asserts the delta)
    return true;
}

// The thin single-output wrapper — the ONE buffer loop lives in run_quant_module_n (all pre-25c-2 callers use this).
bool run_quant_module(crd::gpu::Dx12ComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* alloc,
                      const ceg::TensorPipelinePlan& plan, const QuantSeed* seeds, crd::usize n_seeds, const ce::Value* out_val,
                      float* dst, crd::usize dst_len)
{
    const QuantOut o = {out_val, dst, dst_len};
    return run_quant_module_n(compute, ctx, alloc, plan, seeds, n_seeds, &o, 1U);
}

// CEIR-28d — the PORTABLE MEASURER, MIRRORED onto DX12 (the ⛔ migrated=both-backends rule: the §80 autotuner must run on BOTH GPU
// backends, not just Vk). ⛔ MIRROR-INLINE, not a shared template (advisor reversal): the two `run_quant_module`s are file-local to
// two TUs, so a shared measurer would need a RunFn param + a shared header + a refactor of the green 28b-2a — churn for ~50 lines of
// TEST code the house already duplicates per backend (the run helpers, the MLP builds, the seed tables). Bodies are IDENTICAL to
// 28b-2a's Vk measurer; `plan_sig` is over the backend-INDEPENDENT plan, so the four sigs MUST equal Vk's (same payload + planner).
// ⛔ DX12 medians are NOT comparable to Vk's: DX12 readback is the copy(GpuOnly→GpuToCpu) form INSIDE the begin→submit timestamp
// bracket (Vk maps GpuToCpu directly), so DX12 medians carry a fixed copy cost — it CANCELS across configs (same output size) so the
// COMPARISON is sound, but never compare a DX12 median to a Vk one.
double median_of(double* v, crd::u32 k)
{
    for (crd::u32 i = 1; i < k; ++i) // insertion sort (k tiny; no std container / algorithm header)
    {
        const double t = v[i];
        crd::u32     j = i;
        for (; j > 0 && v[j - 1] > t; --j) { v[j] = v[j - 1]; }
        v[j] = t;
    }
    return v[k / 2U];
}
crd::u64 plan_sig(crd::memory::IAllocator* a, const ceg::TensorPipelinePlan& p)
{
    crd::containers::Array<crd::u32> buf(a);
    for (crd::usize i = 0; i < p.stages.size(); ++i) { buf.push_back(static_cast<crd::u32>(p.stages[i].kind)); }
    buf.push_back(0xFFFFFFFFU); // separate the stage-kind run from the alias_of run
    for (crd::usize i = 0; i < p.buffers.size(); ++i) { buf.push_back(static_cast<crd::u32>(p.buffers[i].alias_of + 1)); } // -1 (none) -> 0
    return crd::containers::fnv1a_64(buf.data(), buf.size() * sizeof(crd::u32));
}
ce::Module* measure_tune_entry(crd::gpu::Dx12ComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* root,
                               const ce::Module& payload, const QuantSeed* seeds, crd::usize n_seeds, const ce::Value* out_val,
                               crd::u32 out_len, crd::containers::StringView device, crd::containers::StringView env,
                               crd::containers::StringView shape, double medians_out[4], crd::u64 sig_out[4],
                               ceg::PlanOptions* winner)
{
    constexpr crd::u32 n_warmup = 5; // ⛔ DX12: 5 (not 3) — D3D12 does more PSO/pipeline-library work at first submit than Vulkan
    constexpr crd::u32 n_timed  = 7; // MEDIAN of K timed runs (odd → a real sample, not an average)

    ceg::PlanOptions cfgs[4];
    cfgs[0].fuse_gemm_relu = true;  cfgs[0].share_intermediate_storage = true;  // index 0 = the DEFAULT (PlanOptions{}) — the oracle
    cfgs[1].fuse_gemm_relu = true;  cfgs[1].share_intermediate_storage = false;
    cfgs[2].fuse_gemm_relu = false; cfgs[2].share_intermediate_storage = true;
    cfgs[3].fuse_gemm_relu = false; cfgs[3].share_intermediate_storage = false;

    crd::containers::Array<float> ref(root);
    crd::containers::Array<float> got(root);
    ref.resize(out_len, 0.0F);
    got.resize(out_len, 0.0F);

    for (crd::u32 c = 0; c < 4U; ++c)
    {
        const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, payload, root, cfgs[c]);
        REQUIRE(plan.reject == ceg::PlanReject::None);
        sig_out[c] = plan_sig(root, plan);
        for (crd::u32 w = 0; w < n_warmup; ++w) { REQUIRE(run_quant_module(compute, ctx, root, plan, seeds, n_seeds, out_val, got.data(), out_len)); }
        double ms[n_timed] = {};
        for (crd::u32 t = 0; t < n_timed; ++t)
        {
            float* const dst = (c == 0U && t == 0U) ? ref.data() : got.data();
            REQUIRE(run_quant_module(compute, ctx, root, plan, seeds, n_seeds, out_val, dst, out_len));
            ms[t] = compute.last_gpu_ms();
        }
        medians_out[c] = median_of(ms, n_timed);
        REQUIRE(medians_out[c] > 0.0); // ⛔ median==0 ⇒ no device timestamps ⇒ measuring NOTHING (hard fail, not skip)
        if (c != 0U)
        {
            int first_mismatch = -1;
            for (crd::u32 i = 0; i < out_len && first_mismatch < 0; ++i) { if (got[i] != ref[i]) { first_mismatch = static_cast<int>(i); } }
            CAPTURE(c, first_mismatch);
            CHECK(first_mismatch == -1);
        }
    }

    crd::u32 wi = 0; // argmin median across plan-CLASSES...
    for (crd::u32 c = 1; c < 4U; ++c) { if (medians_out[c] < medians_out[wi]) { wi = c; } }
    for (crd::u32 c = 0; c < wi; ++c) { if (sig_out[c] == sig_out[wi]) { wi = c; break; } } // ...collapsed to the canonical member (stable)
    if (winner != nullptr) { *winner = cfgs[wi]; }

    const crd::u64 ph = ce::tune::program_hash(ctx, payload, root);
    ce::Module* const emit = ctx.create_module();
    ce::Block*        eb   = emit->body()->first_block();
    if (eb == nullptr) { eb = ctx.create_block(0U); emit->body()->append(eb); }
    ce::Operation* const e = ce::tune::build_entry(ctx, ctx.attr_string(device), ctx.attr_string(env),
                                                   ctx.attr_int(static_cast<crd::i64>(ph)), ctx.attr_string(shape),
                                                   ctx.attr_bool(cfgs[wi].fuse_gemm_relu), ctx.attr_bool(cfgs[wi].share_intermediate_storage));
    eb->append(e);
    return emit;
}

// CEIR-35 Q7 (DX12) — the fp32 2-layer MLP payload (x·W1 → relu → ·W2), built + expanded (the Vulkan build_mlp_payload twin;
// ctx-only, names NO backend). Returns the module + the three ExternalIn SSA values (the caller seeds them).
struct MlpPayload
{
    ce::Module*      m  = nullptr;
    const ce::Value* x  = nullptr;
    const ce::Value* w1 = nullptr;
    const ce::Value* w2 = nullptr;
};
MlpPayload build_mlp_payload(ce::Context& ctx, crd::u32 mrows, crd::u32 d0, crd::u32 d1, crd::u32 d2)
{
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
    ce::Value* const     x_val     = mkd(tf(ctx, sh2(ctx, mrows, d0)));
    ce::Value* const     w1_val    = mkd(tf(ctx, sh2(ctx, d0, d1)));
    ce::Value* const     w2_val    = mkd(tf(ctx, sh2(ctx, d1, d2)));
    ce::Value*           mlpops[3] = {x_val, w1_val, w2_val};
    ce::Operation* const mo        = ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mlpops, 3U),
                                                          1U, tf(ctx, sh2(ctx, mrows, d2)), 0U);
    ctx.set_attr(mo, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
    b->append(mo);
    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, *m);
    REQUIRE(er.error == ceg::MlExpandError::None);
    REQUIRE(er.expanded == 1U);
    return {m, x_val, w1_val, w2_val};
}
} // namespace

// CEIR-35 Q7 (§PR-7 PQP-1) — the COMPILE-TIME DECOMPOSITION board, DX12 twin (⛔ both-backends). Same decomposition as the Vulkan
// board (docs/bench/2026-09-11-ceir35-compile-time-decomposition.md) but the external column is DXC + the D3D12 PSO
// (`create_pipeline_from_hlsl` bundles them — no device-free HLSL→DXIL split, so this board is device-gated). CEIR-owned = the
// device-free lowering (plan_tensor_pipeline) + HLSL codegen (synth_gemm + emit_contract_hlsl). Prints "[CEIR35-Q7-COMPILE-DX12]".
TEST_CASE("ceir35 Q7: compile-time decomposition -- CEIR lowering+codegen vs DXC+PSO (DX12)", "[ceir][ceir35][perf][compile][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    crd::memory::TlsfAllocator   alloc(64U << 20U);
    crd::gpu::Dx12ComputeContext compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device -- skipping the CEIR-35 Q7 compile-decomposition DX12 twin"); return; }

    constexpr crd::u32 mrows = 32;
    constexpr crd::u32 d0    = 64;
    constexpr crd::u32 d1    = 128;
    constexpr crd::u32 d2    = 64;
    const MlpPayload       pay = build_mlp_payload(ctx, mrows, d0, d1, d2);
    const ceg::PlanOptions opts; // fuse=true ⇒ [GemmRelu, Gemm]

    constexpr crd::u32 n_warmup  = 3;
    constexpr crd::u32 n_timed = 15;

    double lower_ms[n_timed] = {};
    for (crd::u32 w = 0; w < n_warmup; ++w) { (void)ceg::plan_tensor_pipeline(ctx, *pay.m, &root, opts); }
    for (crd::u32 t = 0; t < n_timed; ++t)
    {
        const auto                    a = std::chrono::steady_clock::now();
        const ceg::TensorPipelinePlan p = ceg::plan_tensor_pipeline(ctx, *pay.m, &root, opts);
        const auto                    b = std::chrono::steady_clock::now();
        REQUIRE(p.reject == ceg::PlanReject::None);
        lower_ms[t] = std::chrono::duration<double, std::milli>(b - a).count();
    }
    const double t_lower = median_of(lower_ms, n_timed);

    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *pay.m, &root, opts);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    for (crd::usize i = 0; i < plan.stages.size(); ++i)
    {
        const ceg::StageKind k = plan.stages[i].kind;
        REQUIRE((k == ceg::StageKind::Gemm || k == ceg::StageKind::GemmRelu));
    }

    double emit_ms[n_timed] = {};
    for (crd::u32 t = 0; t < n_timed; ++t)
    {
        const auto a = std::chrono::steady_clock::now();
        for (crd::usize i = 0; i < plan.stages.size(); ++i)
        {
            const ceg::PlanStage&   st = plan.stages[i];
            kir::KGraph             g(&root);
            kir::GlslKernel         kern(&root);
            const ceg::GemmEpilogue ep = st.kind == ceg::StageKind::GemmRelu ? ceg::GemmEpilogue::Relu : ceg::GemmEpilogue::None;
            const ceg::GraphSynth   s  = ceg::synth_gemm(ctx, *st.op, g, ep);
            REQUIRE(s.reject == ceg::SynthReject::None);
            REQUIRE(kir::emit_contract_hlsl(g, s.output, kern));
        }
        const auto b = std::chrono::steady_clock::now();
        emit_ms[t]   = std::chrono::duration<double, std::milli>(b - a).count();
    }
    const double t_emit = median_of(emit_ms, n_timed);

    // external: DXC + D3D12 PSO via create_pipeline_from_hlsl (device). Re-emit UNTIMED, clock only the pipeline build. GEMM: nbind 3, push 16.
    const auto compile_stage = [&](const ceg::PlanStage& st) -> double {
        kir::KGraph             g(&root);
        kir::GlslKernel         kern(&root);
        const ceg::GemmEpilogue ep = st.kind == ceg::StageKind::GemmRelu ? ceg::GemmEpilogue::Relu : ceg::GemmEpilogue::None;
        const ceg::GraphSynth   s  = ceg::synth_gemm(ctx, *st.op, g, ep);
        REQUIRE(s.reject == ceg::SynthReject::None);
        REQUIRE(kir::emit_contract_hlsl(g, s.output, kern));
        const auto                                 a   = std::chrono::steady_clock::now();
        std::unique_ptr<crd::gpu::ComputePipeline> pso = compute.create_pipeline_from_hlsl(crd::containers::to_view(kern.source), 3, 16U);
        const auto                                 b   = std::chrono::steady_clock::now();
        REQUIRE(pso != nullptr);
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    double dxc_ms[n_timed] = {};
    for (crd::u32 w = 0; w < n_warmup; ++w) { for (crd::usize i = 0; i < plan.stages.size(); ++i) { (void)compile_stage(plan.stages[i]); } }
    for (crd::u32 t = 0; t < n_timed; ++t)
    {
        double acc = 0.0;
        for (crd::usize i = 0; i < plan.stages.size(); ++i) { acc += compile_stage(plan.stages[i]); }
        dxc_ms[t] = acc;
    }
    const double t_dxc = median_of(dxc_ms, n_timed);

    REQUIRE(t_lower > 0.0);
    REQUIRE(t_emit > 0.0);
    REQUIRE(t_dxc > 0.0);
    const double ceir_owned = t_lower + t_emit;
    std::printf("[CEIR35-Q7-COMPILE-DX12] shape=%ux%ux%ux%u stages=%u | CEIR lower=%.4f ms emit=%.4f ms (owned=%.4f ms) | dxc+PSO=%.4f ms | owned/ext=%.4f\n",
                static_cast<unsigned>(mrows), static_cast<unsigned>(d0), static_cast<unsigned>(d1), static_cast<unsigned>(d2),
                static_cast<unsigned>(plan.stages.size()), t_lower, t_emit, ceir_owned, t_dxc, ceir_owned / t_dxc);
    (void)std::fflush(stdout);
    CHECK(ceir_owned < t_dxc); // the IR layer's compile cost sits BELOW the unavoidable DXC + PSO floor
}

// CEIR-35 Q7 (§PR-7 PQP-1) — the EXECUTOR-OVERHEAD board, DX12 twin (⛔ both-backends). Same A/B as the Vulkan board
// (docs/bench/2026-09-11-ceir35-executor-overhead.md): arm A = execute_tensor_pipeline, arm B = a hand-rolled dispatch loop of
// the SAME pre-resolved pipelines; GPU-identical (bit-exact output), the delta is the executor's CPU record. DX12 staging: dev
// (GpuOnly UAV) uploaded ONCE, then executed many times; the record clock brackets only begin→execute (upload/readback excluded).
TEST_CASE("ceir35 Q7: executor overhead vs hand-rolled dispatch (DX12)", "[ceir][ceir35][perf][executor][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    crd::memory::TlsfAllocator   devalloc(256U << 20U);
    crd::gpu::Dx12ComputeContext compute(&devalloc);
    if (!compute.valid()) { WARN("no D3D12 device -- skipping the CEIR-35 Q7 executor-overhead DX12 twin"); return; }
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    constexpr crd::u32 mrows = 4;
    constexpr crd::u32 d0    = 8;
    constexpr crd::u32 d1    = 16;
    constexpr crd::u32 d2    = 2;
    const MlpPayload       pay  = build_mlp_payload(ctx, mrows, d0, d1, d2);
    const ceg::PlanOptions opts;
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *pay.m, &root, opts);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    const crd::usize nb = plan.buffers.size();
    REQUIRE(nb <= 40U);
    const ce::Value* out_val = nullptr;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].role == ceg::BufferRole::Output) { out_val = plan.buffers[i].value; } }
    REQUIRE(out_val != nullptr);
    const crd::u32 out_len = mrows * d2;

    crd::containers::Array<float> x_in(&root);
    crd::containers::Array<float> w1_in(&root);
    crd::containers::Array<float> w2_in(&root);
    x_in.resize(mrows * d0, 0.0F);
    w1_in.resize(d0 * d1, 0.0F);
    w2_in.resize(d1 * d2, 0.0F);
    for (crd::u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<float>(static_cast<int>(i) - 12); }
    for (crd::u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<float>(static_cast<int>(i % 7) - 3); }
    for (crd::u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<float>(static_cast<int>(i % 5) - 2); }
    const QuantSeed      seeds[3] = {{pay.x, x_in.data(), nullptr, mrows * d0}, {pay.w1, w1_in.data(), nullptr, d0 * d1},
                                     {pay.w2, w2_in.data(), nullptr, d1 * d2}};
    constexpr crd::usize n_seeds  = 3;

    // materialize dev (GpuOnly) + up (CpuToGpu, ExternalIn seeds); upload ONCE
    std::unique_ptr<g::ComputeBuffer> dev[40];
    std::unique_ptr<g::ComputeBuffer> up[40];
    g::ComputeBuffer*                 bufs[40] = {};
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.alias_of >= 0) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes;
        dev[i]            = compute.create_buffer(sz, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        REQUIRE(dev[i] != nullptr);
        bufs[i] = dev[i].get();
        if (pb.role != ceg::BufferRole::ExternalIn) { continue; }
        const QuantSeed* seed = nullptr;
        for (crd::usize s = 0; s < n_seeds; ++s) { if (seeds[s].value == pb.value) { seed = &seeds[s]; break; } }
        if (seed == nullptr) { continue; }
        up[i] = compute.create_buffer(sz, transfer_src, g::ComputeMemory::CpuToGpu);
        REQUIRE(up[i] != nullptr);
        auto* const d = static_cast<float*>(up[i]->map());
        REQUIRE(d != nullptr);
        for (crd::u32 e = 0; e < seed->count; ++e) { d[e] = seed->floats[e]; }
        up[i]->unmap();
    }
    {
        auto& rec = compute.begin();
        for (crd::usize i = 0; i < nb; ++i) { if (up[i] != nullptr) { rec.copy(*up[i], *dev[i], 0U, 0U, plan.buffers[i].bytes); } }
        for (crd::usize i = 0; i < nb; ++i)
        {
            if (up[i] != nullptr) { rec.barrier(*dev[i], g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead); }
        }
        compute.submit_and_wait();
    }

    Resolver warm_res;
    warm_res.alloc_ctx  = &ctx;
    warm_res.alloc      = &root;
    warm_res.compute    = &compute;
    const crd::usize ns = plan.stages.size();
    REQUIRE(ns <= 16U);
    ceg::ResolvedStage resolved[16];
    for (crd::usize j = 0; j < ns; ++j)
    {
        resolved[j] = resolve_stage(plan.stages[j], &warm_res);
        REQUIRE(resolved[j].pipeline != nullptr);
    }
    crd::i32 out_idx = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == out_val) { out_idx = static_cast<crd::i32>(i); } }
    REQUIRE(out_idx >= 0);
    std::unique_ptr<g::ComputeBuffer> rb =
        compute.create_buffer(plan.buffers[static_cast<crd::usize>(out_idx)].bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
    REQUIRE(rb != nullptr);

    struct CacheState
    {
        const ceg::ResolvedStage* arr = nullptr;
        crd::u32                  idx = 0;
    };
    const ceg::StageResolveFn cached = [](const ceg::PlanStage&, void* u) -> ceg::ResolvedStage {
        auto& s = *static_cast<CacheState*>(u);
        return s.arr[s.idx++];
    };
    const auto hand_roll = [&](g::ComputeRecorder& rec) {
        for (crd::usize j = 0; j < ns; ++j)
        {
            const ceg::PlanStage& st       = plan.stages[j];
            g::ComputeBuffer*     binds[8] = {};
            for (crd::u32 i = 0; i < st.nbind; ++i) { binds[i] = bufs[static_cast<crd::usize>(st.bind[i])]; }
            rec.dispatch(*resolved[j].pipeline, crd::containers::ConstSpan<g::ComputeBuffer*>(binds, st.nbind), resolved[j].push,
                         resolved[j].push_size, resolved[j].gx, resolved[j].gy, resolved[j].gz);
            if (j + 1 < ns)
            {
                for (crd::u32 k = 0; k < st.n_out; ++k)
                {
                    const crd::u32 bi = st.nbind - st.n_out + k;
                    rec.barrier(*bufs[static_cast<crd::usize>(st.bind[bi])], g::ComputeAccess::ShaderWrite, g::ComputeAccess::ShaderRead);
                }
            }
        }
    };

    constexpr crd::u32 n_warmup  = 5;
    constexpr crd::u32 n_timed = 15;

    double a_rec[n_timed] = {};
    double a_gpu[n_timed] = {};
    for (crd::u32 t = 0; t < n_warmup + n_timed; ++t)
    {
        CacheState cs{resolved, 0};
        auto&      rec = compute.begin();
        const auto c0  = std::chrono::steady_clock::now();
        const ceg::ExecuteError ee =
            ceg::execute_tensor_pipeline(plan, rec, cached, &cs, crd::containers::ConstSpan<g::ComputeBuffer*>(bufs, nb));
        REQUIRE(ee == ceg::ExecuteError::None);
        const auto c1 = std::chrono::steady_clock::now();
        compute.submit_and_wait();
        if (t >= n_warmup)
        {
            a_rec[t - n_warmup] = std::chrono::duration<double, std::micro>(c1 - c0).count();
            a_gpu[t - n_warmup] = compute.last_gpu_ms();
        }
    }
    double b_rec[n_timed] = {};
    double b_gpu[n_timed] = {};
    for (crd::u32 t = 0; t < n_warmup + n_timed; ++t)
    {
        auto&      rec = compute.begin();
        const auto c0  = std::chrono::steady_clock::now();
        hand_roll(rec);
        const auto c1 = std::chrono::steady_clock::now();
        compute.submit_and_wait();
        if (t >= n_warmup)
        {
            b_rec[t - n_warmup] = std::chrono::duration<double, std::micro>(c1 - c0).count();
            b_gpu[t - n_warmup] = compute.last_gpu_ms();
        }
    }

    // correctness: a readback run of each arm — the two record paths produce BIT-IDENTICAL output (same kernels, same args)
    const auto readback = [&](bool executor, crd::containers::Array<float>& dst) {
        auto& rec = compute.begin();
        if (executor)
        {
            CacheState cs{resolved, 0};
            REQUIRE(ceg::execute_tensor_pipeline(plan, rec, cached, &cs, crd::containers::ConstSpan<g::ComputeBuffer*>(bufs, nb))
                    == ceg::ExecuteError::None);
        }
        else { hand_roll(rec); }
        rec.barrier(*bufs[static_cast<crd::usize>(out_idx)], g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
        rec.copy(*bufs[static_cast<crd::usize>(out_idx)], *rb, 0U, 0U, plan.buffers[static_cast<crd::usize>(out_idx)].bytes);
        compute.submit_and_wait();
        dst.resize(out_len, 0.0F);
        const auto* got = static_cast<const float*>(rb->map());
        REQUIRE(got != nullptr);
        for (crd::usize e = 0; e < out_len; ++e) { dst[e] = got[e]; }
        rb->unmap();
    };
    crd::containers::Array<float> out_a(&root);
    crd::containers::Array<float> out_b(&root);
    readback(true, out_a);
    readback(false, out_b);
    int mism = -1;
    for (crd::usize e = 0; e < out_len; ++e) { if (out_a[e] != out_b[e]) { mism = static_cast<int>(e); break; } }
    CHECK(mism == -1);

    const double median_a_rec = median_of(a_rec, n_timed);
    const double median_a_gpu = median_of(a_gpu, n_timed);
    const double median_b_rec = median_of(b_rec, n_timed);
    const double median_b_gpu = median_of(b_gpu, n_timed);
    REQUIRE(median_a_gpu > 0.0);
    REQUIRE(median_b_gpu > 0.0);
    std::printf("[CEIR35-Q7-EXEC-DX12] stages=%u | executor: rec=%.3f us gpu=%.5f ms | native: rec=%.3f us gpu=%.5f ms | rec_delta=%.3f us gpu_ratio=%.4f\n",
                static_cast<unsigned>(ns), median_a_rec, median_a_gpu, median_b_rec, median_b_gpu, median_a_rec - median_b_rec, median_a_gpu / median_b_gpu);
    (void)std::fflush(stdout);
    CHECK(median_a_gpu < median_b_gpu * 2.0 + 0.05);
    CHECK(median_b_gpu < median_a_gpu * 2.0 + 0.05);
}

// CEIR-35 Q7 (§PR-7 PQP-1) — the PLAN+PIPELINE REUSE (amortization) board, DX12 twin (⛔ both-backends). Same as the Vulkan board
// (docs/bench/2026-09-11-ceir35-plan-reuse-amortization.md): COLD (re-lower + fresh Resolver ⇒ recompile every stage's HLSL→DXIL+PSO
// + execute) vs WARM (reuse the lowered plan + built pipelines). Grounded in the §158 reuse, NOT the dead CEIR-10b PlanCache.
TEST_CASE("ceir35 Q7: plan+pipeline reuse amortization (DX12)", "[ceir][ceir35][perf][reuse][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    crd::memory::TlsfAllocator   devalloc(256U << 20U);
    crd::gpu::Dx12ComputeContext compute(&devalloc);
    if (!compute.valid()) { WARN("no D3D12 device -- skipping the CEIR-35 Q7 reuse-amortization DX12 twin"); return; }
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    constexpr crd::u32            mrows = 4;
    constexpr crd::u32            d0 = 8;
    constexpr crd::u32            d1 = 16;
    constexpr crd::u32            d2 = 2;
    const MlpPayload              pay  = build_mlp_payload(ctx, mrows, d0, d1, d2);
    const ceg::PlanOptions        opts;
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *pay.m, &root, opts);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    const crd::usize nb = plan.buffers.size();
    REQUIRE(nb <= 40U);

    crd::containers::Array<float> x_in(&root);
    crd::containers::Array<float> w1_in(&root);
    crd::containers::Array<float> w2_in(&root);
    x_in.resize(mrows * d0, 0.0F);
    w1_in.resize(d0 * d1, 0.0F);
    w2_in.resize(d1 * d2, 0.0F);
    for (crd::u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<float>(static_cast<int>(i) - 12); }
    for (crd::u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<float>(static_cast<int>(i % 7) - 3); }
    for (crd::u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<float>(static_cast<int>(i % 5) - 2); }
    const QuantSeed      seeds[3] = {{pay.x, x_in.data(), nullptr, mrows * d0}, {pay.w1, w1_in.data(), nullptr, d0 * d1},
                                     {pay.w2, w2_in.data(), nullptr, d1 * d2}};
    constexpr crd::usize n_seeds  = 3;

    std::unique_ptr<g::ComputeBuffer> dev[40];
    std::unique_ptr<g::ComputeBuffer> up[40];
    g::ComputeBuffer*                 bufs[40] = {};
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.alias_of >= 0) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes;
        dev[i]            = compute.create_buffer(sz, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        REQUIRE(dev[i] != nullptr);
        bufs[i] = dev[i].get();
        if (pb.role != ceg::BufferRole::ExternalIn) { continue; }
        const QuantSeed* seed = nullptr;
        for (crd::usize s = 0; s < n_seeds; ++s) { if (seeds[s].value == pb.value) { seed = &seeds[s]; break; } }
        if (seed == nullptr) { continue; }
        up[i] = compute.create_buffer(sz, transfer_src, g::ComputeMemory::CpuToGpu);
        REQUIRE(up[i] != nullptr);
        auto* const d = static_cast<float*>(up[i]->map());
        REQUIRE(d != nullptr);
        for (crd::u32 e = 0; e < seed->count; ++e) { d[e] = seed->floats[e]; }
        up[i]->unmap();
    }
    {
        auto& rec = compute.begin();
        for (crd::usize i = 0; i < nb; ++i) { if (up[i] != nullptr) { rec.copy(*up[i], *dev[i], 0U, 0U, plan.buffers[i].bytes); } }
        for (crd::usize i = 0; i < nb; ++i)
        {
            if (up[i] != nullptr) { rec.barrier(*dev[i], g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead); }
        }
        compute.submit_and_wait();
    }

    const crd::usize ns = plan.stages.size();
    REQUIRE(ns <= 16U);
    Resolver warm_res;
    warm_res.alloc_ctx = &ctx;
    warm_res.alloc     = &root;
    warm_res.compute   = &compute;
    ceg::ResolvedStage resolved[16];
    for (crd::usize j = 0; j < ns; ++j)
    {
        resolved[j] = resolve_stage(plan.stages[j], &warm_res);
        REQUIRE(resolved[j].pipeline != nullptr);
    }
    struct CacheState
    {
        const ceg::ResolvedStage* arr = nullptr;
        crd::u32                  idx = 0;
    };
    const ceg::StageResolveFn cached = [](const ceg::PlanStage&, void* u) -> ceg::ResolvedStage {
        auto& s = *static_cast<CacheState*>(u);
        return s.arr[s.idx++];
    };

    constexpr crd::u32 n_warmup  = 3;
    constexpr crd::u32 n_timed = 15;

    double cold_ms[n_timed] = {};
    for (crd::u32 t = 0; t < n_warmup + n_timed; ++t)
    {
        const auto                    c0 = std::chrono::steady_clock::now();
        const ceg::TensorPipelinePlan p  = ceg::plan_tensor_pipeline(ctx, *pay.m, &root, opts);
        REQUIRE(p.reject == ceg::PlanReject::None);
        Resolver cres;
        cres.alloc_ctx = &ctx;
        cres.alloc     = &root;
        cres.compute   = &compute;
        auto&                   rec = compute.begin();
        const ceg::ExecuteError ee =
            ceg::execute_tensor_pipeline(p, rec, &resolve_stage, &cres, crd::containers::ConstSpan<g::ComputeBuffer*>(bufs, nb));
        REQUIRE(ee == ceg::ExecuteError::None);
        compute.submit_and_wait();
        const auto c1 = std::chrono::steady_clock::now();
        if (t >= n_warmup) { cold_ms[t - n_warmup] = std::chrono::duration<double, std::milli>(c1 - c0).count(); }
    }
    double warm_ms[n_timed] = {};
    for (crd::u32 t = 0; t < n_warmup + n_timed; ++t)
    {
        CacheState cs{resolved, 0};
        const auto c0  = std::chrono::steady_clock::now();
        auto&      rec = compute.begin();
        const ceg::ExecuteError ee =
            ceg::execute_tensor_pipeline(plan, rec, cached, &cs, crd::containers::ConstSpan<g::ComputeBuffer*>(bufs, nb));
        REQUIRE(ee == ceg::ExecuteError::None);
        compute.submit_and_wait();
        const auto c1 = std::chrono::steady_clock::now();
        if (t >= n_warmup) { warm_ms[t - n_warmup] = std::chrono::duration<double, std::milli>(c1 - c0).count(); }
    }

    const double c = median_of(cold_ms, n_timed);
    const double w = median_of(warm_ms, n_timed);
    REQUIRE(c > 0.0);
    REQUIRE(w > 0.0);
    std::printf("[CEIR35-Q7-REUSE-DX12] stages=%u | cold(lower+compile+exec)=%.4f ms | warm(exec, plan+PSO reused)=%.4f ms | speedup=%.1fx\n",
                static_cast<unsigned>(ns), c, w, c / w);
    (void)std::fflush(stdout);
    CHECK(w < c);
}

TEST_CASE("ceir 22c-3e: the PARSE-LOADED design-B pipeline runs on DX12 (portable copy-readback) vs a composed ref",
          "[ceir][tensor-pipeline][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::tensor::register_dialect(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    crd::memory::TlsfAllocator alloc(64U << 20U);

    std::ifstream af(CRD_REPO_DIR "/assets/ceir/tensor_pipeline.ceir", std::ios::binary | std::ios::ate);
    REQUIRE(af.good());
    const std::streamsize asz = af.tellg();
    af.seekg(0);
    crd::containers::Array<char> asrc(&root);
    asrc.resize(static_cast<crd::usize>(asz), '\0');
    af.read(asrc.data(), asz);
    const ce::ParseResult pr = ce::parse(ctx, crd::containers::StringView(asrc.data(), static_cast<crd::usize>(asz)));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *pr.module, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 5U);

    // ── the INDEPENDENT composed f64 reference: triple-loop gemm -> naive DFT -> magnitude -> max -> normalize ──
    static float a_data[kL];
    static float b_data[kL];
    for (int i = 0; i < kSide; ++i)
    {
        for (int j = 0; j < kSide; ++j)
        {
            a_data[i * kSide + j] = static_cast<float>(((i + j) % 7) - 3);
            b_data[i * kSide + j] = static_cast<float>(((i * 2 + j) % 5) - 2);
        }
    }
    crd::f64 e_ref[kL];
    for (int i = 0; i < kSide; ++i)
    {
        for (int j = 0; j < kSide; ++j)
        {
            crd::f64 acc = 0.0;
            for (int kk = 0; kk < kSide; ++kk)
            {
                acc += static_cast<crd::f64>(a_data[i * kSide + kk]) * static_cast<crd::f64>(b_data[kk * kSide + j]);
            }
            e_ref[i * kSide + j] = acc;
        }
    }
    constexpr crd::f64 two_pi = 6.28318530717958647693;
    crd::f64           mag_ref[kL];
    crd::f64           mx_ref = 0.0;
    for (int k = 0; k < kL; ++k)
    {
        crd::f64 fr = 0.0;
        crd::f64 fi = 0.0;
        for (int l = 0; l < kL; ++l)
        {
            const crd::f64 ang = two_pi * static_cast<crd::f64>(k) * static_cast<crd::f64>(l) / static_cast<crd::f64>(kL);
            fr += e_ref[l] * crd::math::cos(ang);
            fi -= e_ref[l] * crd::math::sin(ang);
        }
        mag_ref[k] = crd::math::sqrt(fr * fr + fi * fi);
        mx_ref     = mx_ref > mag_ref[k] ? mx_ref : mag_ref[k];
    }
    crd::f64 norm_ref[kL];
    for (int k = 0; k < kL; ++k) { norm_ref[k] = mag_ref[k] / mx_ref; }

    // ── DEVICE (soft-skip with no D3D12 adapter) ──
    crd::gpu::Dx12ComputeContext compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-22c-3e pipeline gate"); return; }
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;

    // ── the PORTABLE three-buffer plan: dev (GpuOnly UAV) + up (upload) + rb (readback). Alias shares dev; twiddles by fft stage. ──
    const crd::i32 a_buf   = plan.stages[0].bind[0];
    const crd::i32 b_buf   = plan.stages[0].bind[1];
    crd::i32       twr_buf = -1;
    crd::i32       twi_buf = -1;
    int            fft_n   = 0;
    for (crd::usize s = 0; s < plan.stages.size(); ++s)
    {
        if (plan.stages[s].kind != ceg::StageKind::Fft) { continue; }
        twr_buf = plan.stages[s].bind[2];
        twi_buf = plan.stages[s].bind[3];
        fft_n   = static_cast<int>(plan.buffers[static_cast<crd::usize>(twr_buf)].bytes / 4ULL) * 2;
    }
    crd::i32 out_idx = -1;
    for (crd::usize i = 0; i < plan.buffers.size(); ++i)
    {
        if (plan.buffers[i].role == ceg::BufferRole::Output) { out_idx = static_cast<crd::i32>(i); }
    }
    REQUIRE(out_idx >= 0);

    const crd::usize nb = plan.buffers.size();
    REQUIRE(nb <= 40U);
    std::unique_ptr<g::ComputeBuffer> dev[40];
    std::unique_ptr<g::ComputeBuffer> up[40];
    g::ComputeBuffer*                 bufs[40] = {};
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.role == ceg::BufferRole::Alias) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        dev[i] = compute.create_buffer(pb.bytes, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        REQUIRE(dev[i] != nullptr);
        bufs[i] = dev[i].get();
        if (pb.role == ceg::BufferRole::ExternalIn)
        {
            up[i] = compute.create_buffer(pb.bytes, transfer_src, g::ComputeMemory::CpuToGpu);
            REQUIRE(up[i] != nullptr);
            auto*          dst = static_cast<float*>(up[i]->map());
            const crd::u64 cnt = pb.bytes / 4ULL;
            if (static_cast<crd::i32>(i) == a_buf) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = a_data[e]; } }
            else if (static_cast<crd::i32>(i) == b_buf) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = b_data[e]; } }
            else if (static_cast<crd::i32>(i) == twr_buf)
            {
                for (crd::u64 e = 0; e < cnt; ++e)
                {
                    dst[e] = static_cast<float>(crd::math::cos(two_pi * static_cast<crd::f64>(e) / static_cast<crd::f64>(fft_n)));
                }
            }
            else if (static_cast<crd::i32>(i) == twi_buf)
            {
                for (crd::u64 e = 0; e < cnt; ++e)
                {
                    dst[e] = static_cast<float>(-crd::math::sin(two_pi * static_cast<crd::f64>(e) / static_cast<crd::f64>(fft_n)));
                }
            }
            else { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = 0.0F; } } // C (unused), im0 (Zeros)
            up[i]->unmap();
        }
    }
    auto rb = compute.create_buffer(plan.buffers[static_cast<crd::usize>(out_idx)].bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
    REQUIRE(rb != nullptr);

    // ── RECORD one submit: upload copies → barrier → the pipeline → the Output readback copy. ──
    Resolver res;
    res.alloc_ctx = &ctx;
    res.alloc     = &alloc;
    res.compute   = &compute;

    auto& rec = compute.begin();
    for (crd::usize i = 0; i < nb; ++i)
    {
        if (up[i] != nullptr) { rec.copy(*up[i], *dev[i], 0U, 0U, plan.buffers[i].bytes); }
    }
    for (crd::usize i = 0; i < nb; ++i)
    {
        if (up[i] != nullptr) { rec.barrier(*dev[i], g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead); }
    }
    const ceg::ExecuteError ee = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res,
                                                              crd::containers::ConstSpan<g::ComputeBuffer*>(bufs, nb));
    REQUIRE(ee == ceg::ExecuteError::None);
    rec.barrier(*dev[static_cast<crd::usize>(out_idx)], g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
    rec.copy(*dev[static_cast<crd::usize>(out_idx)], *rb, 0U, 0U, plan.buffers[static_cast<crd::usize>(out_idx)].bytes);
    compute.submit_and_wait();

    const auto* got = static_cast<const float*>(rb->map());
    REQUIRE(got != nullptr);
    const float tol       = static_cast<float>(2.0 * 1.41421356 * 2e-3); // the derived quotient bound (as the Vulkan leg)
    int         worst     = -1;
    float       worst_err = 0.0F;
    for (int k = 0; k < kL; ++k)
    {
        const float d = got[k] - static_cast<float>(norm_ref[k]);
        const float e = d < 0.0F ? -d : d;
        if (e > worst_err) { worst_err = e; worst = k; }
    }
    rb->unmap();
    INFO("worst bin " << worst << " err " << worst_err << " tol " << tol);
    CHECK(worst_err <= tol);
}

TEST_CASE("ceir 23b-2d: the FUSED QuantGemm collapse (dequant-inline gemm) runs on DX12 == the unfused Dequant+Gemm == oracle",
          "[ceir][tensor-pipeline][gpu][quant]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ce::Context                        ctx(&alloc);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::tensor::register_dialect(ctx);
    (void)ce::quant::register_dialect(ctx);

    const auto absf = [](float x) { return x < 0.0F ? -x : x; };

    constexpr crd::u32 rows  = 4U;
    constexpr crd::u32 inner = 8U;
    constexpr crd::u32 cols  = 8U;

    struct QMod
    {
        ce::Module* m   = nullptr;
        ce::Value*  a   = nullptr;
        ce::Value*  wq  = nullptr;
        ce::Value*  sc  = nullptr;
        ce::Value*  out = nullptr;
    };
    const auto build = [&](bool unfused) -> QMod
    {
        ce::Module* const m   = ctx.create_module();
        ce::Block*        top = m->body()->first_block();
        if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
        ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
        top->append(f);
        ce::Block* const b   = ce::func::func_body_block(f);
        const ce::OpId   dcl = ctx.intern_op("resource", "declare");
        const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
        const ce::TypeId i8  = ctx.type_int(8U, true);
        const ce::TypeId r0  = shp(ctx, crd::containers::ConstSpan<ce::TypeId>{});
        ce::Value* const a   = mkd(tf(ctx, sh2(ctx, rows, inner)));
        ce::Value* const wq  = mkd(ctx.type_tensor(i8, sh2(ctx, inner, cols)));
        ce::Value* const sc  = mkd(tf(ctx, r0));
        ce::Value* const zp  = mkd(ctx.type_tensor(i8, r0));
        ce::Operation* const dq = ce::quant::build_dequantize(ctx, wq, sc, zp, ctx.attr_int(0),
                                                              ctx.attr_string(crd::containers::StringView("symmetric")), tf(ctx, sh2(ctx, inner, cols)));
        b->append(dq);
        ce::Value* const     c  = mkd(tf(ctx, sh2(ctx, rows, cols)));
        ce::Operation* const g1 = ce::linalg::build_gemm(ctx, a, dq->result(0U), c, ctx.attr_float(1.0), ctx.attr_float(0.0),
                                                         ctx.attr_bool(false), ctx.attr_bool(false), tf(ctx, sh2(ctx, rows, cols)));
        b->append(g1);
        ce::Operation* out_op = g1;
        if (unfused)
        {
            ce::Operation* const g2 = ce::linalg::build_gemm(ctx, a, dq->result(0U), c, ctx.attr_float(1.0), ctx.attr_float(0.0),
                                                             ctx.attr_bool(false), ctx.attr_bool(false), tf(ctx, sh2(ctx, rows, cols)));
            b->append(g2);
            out_op = g2; // 2nd gemm → W_dq multi-use → no fusion; g2 is the final stage (Output)
        }
        return {m, a, wq, sc, out_op->result(0U)};
    };

    const QMod fused_mod   = build(false);
    const QMod unfused_mod = build(true);

    const ceg::TensorPipelinePlan plan_f = ceg::plan_tensor_pipeline(ctx, *fused_mod.m, &alloc);
    REQUIRE(plan_f.reject == ceg::PlanReject::None);
    REQUIRE(plan_f.stages.size() == 1U);
    REQUIRE(plan_f.stages[0].kind == ceg::StageKind::QuantGemm);
    const ceg::TensorPipelinePlan plan_u = ceg::plan_tensor_pipeline(ctx, *unfused_mod.m, &alloc);
    REQUIRE(plan_u.reject == ceg::PlanReject::None);
    REQUIRE(plan_u.stages.size() == 3U);
    REQUIRE(plan_u.stages[0].kind == ceg::StageKind::Dequant);
    REQUIRE(plan_u.stages[2].kind == ceg::StageKind::Gemm);

    float a_data[rows * inner];
    for (crd::u32 i = 0; i < rows * inner; ++i) { a_data[i] = 0.25F * static_cast<float>(static_cast<int>((i * 13U + 5U) % 9U) - 4); }
    crd::i32 wq_i[inner * cols];
    for (crd::u32 i = 0; i < inner * cols; ++i) { wq_i[i] = static_cast<crd::i32>((i * 37U + 11U) % 256U) - 128; }
    crd::u32 packed[inner * cols / 4U];
    for (crd::u32 w = 0; w < inner * cols / 4U; ++w)
    {
        crd::u32 word = 0;
        for (crd::u32 j = 0; j < 4U; ++j) { word |= static_cast<crd::u32>(wq_i[4U * w + j] & 0xFF) << (8U * j); }
        packed[w] = word;
    }
    const float scale = 0.125F;
    float       oracle[rows * cols];
    for (crd::u32 mrow = 0; mrow < rows; ++mrow)
    {
        for (crd::u32 ncol = 0; ncol < cols; ++ncol)
        {
            float acc = 0.0F;
            for (crd::u32 k = 0; k < inner; ++k) { acc += a_data[mrow * inner + k] * static_cast<float>(wq_i[k * cols + ncol]); }
            oracle[mrow * cols + ncol] = acc * scale;
        }
    }

    // ── DEVICE (soft-skip with no D3D12 adapter) ──
    crd::gpu::Dx12ComputeContext compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-23b-2d fused QuantGemm gate"); return; }

    float d_fused[rows * cols]   = {};
    float d_unfused[rows * cols] = {};
    const QuantSeed seeds_f[3]   = {{fused_mod.a, a_data, nullptr, rows * inner},
                                    {fused_mod.wq, nullptr, packed, inner * cols / 4U},
                                    {fused_mod.sc, &scale, nullptr, 1U}};
    REQUIRE(run_quant_module(compute, ctx, &alloc, plan_f, seeds_f, 3U, fused_mod.out, d_fused, rows * cols));
    const QuantSeed seeds_u[3] = {{unfused_mod.a, a_data, nullptr, rows * inner},
                                  {unfused_mod.wq, nullptr, packed, inner * cols / 4U},
                                  {unfused_mod.sc, &scale, nullptr, 1U}};
    REQUIRE(run_quant_module(compute, ctx, &alloc, plan_u, seeds_u, 3U, unfused_mod.out, d_unfused, rows * cols));

    for (crd::u32 i = 0; i < rows * cols; ++i)
    {
        const float tol = 1e-4F * (1.0F + absf(oracle[i]));
        CHECK(absf(d_fused[i] - oracle[i]) <= tol);   // the FUSED QuantGemm kernel is correct on DX12
        CHECK(absf(d_unfused[i] - oracle[i]) <= tol); // the unfused Dequant+Gemm is correct (the folded 2a sym-dequant debt)
        CHECK(absf(d_fused[i] - d_unfused[i]) <= tol); // the fusion PRESERVES the gemm semantics
    }
}

TEST_CASE("ceir 23c-d: the PARSE-LOADED quant-MLP (QuantGemm->relu->QuantGemm) runs on DX12 vs a float MLP oracle",
          "[ceir][tensor-pipeline][gpu][quant]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ce::Context                        ctx(&alloc);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::tensor::register_dialect(ctx);
    (void)ce::quant::register_dialect(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);

    const auto absf = [](float x) { return x < 0.0F ? -x : x; };

    std::ifstream af(CRD_REPO_DIR "/assets/ceir/quant_mlp.ceir", std::ios::binary | std::ios::ate);
    REQUIRE(af.good());
    const std::streamsize asz = af.tellg();
    af.seekg(0);
    crd::containers::Array<char> asrc(&alloc);
    asrc.resize(static_cast<crd::usize>(asz), '\0');
    af.read(asrc.data(), asz);
    const ce::ParseResult pr = ce::parse(ctx, crd::containers::StringView(asrc.data(), static_cast<crd::usize>(asz)));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *pr.module, &alloc);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 3U);
    REQUIRE(plan.stages[0].kind == ceg::StageKind::QuantGemm);
    REQUIRE(plan.stages[1].kind == ceg::StageKind::VizDispatch);
    REQUIRE(plan.stages[2].kind == ceg::StageKind::QuantGemm);

    const auto val = [&](crd::u32 stage, crd::u32 b) { return plan.buffers[static_cast<crd::usize>(plan.stages[stage].bind[b])].value; };
    const ce::Value* const x_val   = val(0U, 0U);
    const ce::Value* const w1_val  = val(0U, 1U);
    const ce::Value* const s1_val  = val(0U, 2U);
    const ce::Value* const w2_val  = val(2U, 1U);
    const ce::Value* const s2_val  = val(2U, 2U);
    const ce::Value* const out_val = val(2U, 3U);

    constexpr crd::u32 rows  = 4U;
    constexpr crd::u32 inner = 8U;
    constexpr crd::u32 cols  = 8U;
    float              x[rows * inner];
    for (crd::u32 i = 0; i < rows * inner; ++i) { x[i] = 0.25F * static_cast<float>(static_cast<int>((i * 13U + 5U) % 9U) - 4); }
    crd::i32 wq1[inner * cols];
    crd::i32 wq2[inner * cols];
    for (crd::u32 i = 0; i < inner * cols; ++i)
    {
        wq1[i] = static_cast<crd::i32>((i * 37U + 11U) % 256U) - 128;
        wq2[i] = static_cast<crd::i32>((i * 29U + 7U) % 256U) - 128;
    }
    const auto pack = [](const crd::i32* w, crd::u32* p) {
        for (crd::u32 wd = 0; wd < inner * cols / 4U; ++wd)
        {
            crd::u32 word = 0;
            for (crd::u32 j = 0; j < 4U; ++j) { word |= static_cast<crd::u32>(w[4U * wd + j] & 0xFF) << (8U * j); }
            p[wd] = word;
        }
    };
    crd::u32 p1[inner * cols / 4U];
    crd::u32 p2[inner * cols / 4U];
    pack(wq1, p1);
    pack(wq2, p2);
    const float s1 = 0.125F;
    const float s2 = 0.0625F;
    float       h1[rows * cols];
    float       oracle[rows * cols];
    for (crd::u32 m = 0; m < rows; ++m)
    {
        for (crd::u32 n = 0; n < cols; ++n)
        {
            float acc = 0.0F;
            for (crd::u32 kk = 0; kk < inner; ++kk) { acc += x[m * inner + kk] * static_cast<float>(wq1[kk * cols + n]); }
            const float v    = acc * s1;
            h1[m * cols + n] = v > 0.0F ? v : 0.0F;
        }
    }
    for (crd::u32 m = 0; m < rows; ++m)
    {
        for (crd::u32 n = 0; n < cols; ++n)
        {
            float acc = 0.0F;
            for (crd::u32 kk = 0; kk < inner; ++kk) { acc += h1[m * inner + kk] * static_cast<float>(wq2[kk * cols + n]); }
            oracle[m * cols + n] = acc * s2;
        }
    }

    // ── DEVICE (soft-skip with no D3D12 adapter) ──
    crd::gpu::Dx12ComputeContext compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-23c-d quant-MLP gate"); return; }

    const QuantSeed seeds[5] = {{x_val, x, nullptr, rows * inner}, {w1_val, nullptr, p1, inner * cols / 4U},
                                {s1_val, &s1, nullptr, 1U},        {w2_val, nullptr, p2, inner * cols / 4U},
                                {s2_val, &s2, nullptr, 1U}};
    float d_out[rows * cols] = {};
    REQUIRE(run_quant_module(compute, ctx, &alloc, plan, seeds, 5U, out_val, d_out, rows * cols));

    for (crd::u32 i = 0; i < rows * cols; ++i)
    {
        CHECK(absf(d_out[i] - oracle[i]) <= 1e-4F * (1.0F + absf(oracle[i]))); // the composed 2-layer quant MLP == the float MLP
    }
}

// CEIR-24b-4 (DX12 leg) — the §138 ML PROOF headline on D3D12: an authored ml.attention EXPANDED by expand_ml_ops into
// synth transpose -> gemm(Q,Kt) -> softmax kernel -> gemm(.,V) (26d-3a/3b retired the baked ckir) runs DEVICE-RESIDENT on a real D3D12 device (HLSL/DXIL) and matches
// the CPU SDPA oracle (the migrated-executor-both-backends rule). scale = 1/√D is the sole 1-element ExternalIn. Dims Sq=2, Sk=3,
// D=4, Dv=2 (the transpose/softmax kernels are baked to these).
TEST_CASE("ceir 24b-4: an expanded ml.attention runs device-resident on DX12 (transpose+gemm+softmax+gemm vs the CPU SDPA oracle)",
          "[ceir][ml][gpu]")
{
    constexpr crd::u32 sq = 2;
    constexpr crd::u32 sk = 3;
    constexpr crd::u32 dd = 4;
    constexpr crd::u32 dv = 2;

    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };

    ce::Value* const q_val = mkd(tf(ctx, sh2(ctx, sq, dd)));
    ce::Value* const k_val = mkd(tf(ctx, sh2(ctx, sk, dd)));
    ce::Value* const v_val = mkd(tf(ctx, sh2(ctx, sk, dv)));
    b->append(ce::ml::build_attention(ctx, q_val, k_val, v_val, tf(ctx, sh2(ctx, sq, dv))));

    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, *m);
    REQUIRE(er.error == ceg::MlExpandError::None);
    REQUIRE(er.expanded == 1U);
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);

    const ce::Value* out_val   = nullptr;
    const ce::Value* scale_val = nullptr;
    for (crd::usize i = 0; i < plan.buffers.size(); ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.role == ceg::BufferRole::Output) { out_val = pb.value; }
        else if (pb.role == ceg::BufferRole::ExternalIn && tnumel(ctx, pb.value->type()) == 1ULL) { scale_val = pb.value; }
    }
    REQUIRE(out_val != nullptr);
    REQUIRE(scale_val != nullptr);

    const float q_in[sq * dd] = {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F};
    const float k_in[sk * dd] = {0.2F, 0.1F, 0.0F, 0.3F, 0.4F, 0.5F, 0.6F, 0.1F, 0.7F, 0.2F, 0.3F, 0.9F};
    const float v_in[sk * dv] = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
    const float inv_sqrt_d    = 1.0F / crd::math::sqrt(static_cast<float>(dd));

    float oracle[sq * dv] = {};
    for (crd::u32 i = 0; i < sq; ++i)
    {
        float sc[sk];
        float mx = -1e30F;
        for (crd::u32 j = 0; j < sk; ++j)
        {
            float dot = 0.0F;
            for (crd::u32 d = 0; d < dd; ++d) { dot += q_in[i * dd + d] * k_in[j * dd + d]; }
            sc[j] = dot * inv_sqrt_d;
            mx    = crd::math::max(mx, sc[j]);
        }
        float denom = 0.0F;
        for (crd::u32 j = 0; j < sk; ++j) { sc[j] = crd::math::exp(sc[j] - mx); denom += sc[j]; }
        for (crd::u32 kk = 0; kk < dv; ++kk)
        {
            float acc = 0.0F;
            for (crd::u32 j = 0; j < sk; ++j) { acc += (sc[j] / denom) * v_in[j * dv + kk]; }
            oracle[i * dv + kk] = acc;
        }
    }

    crd::gpu::Dx12ComputeContext compute(&root);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-24b-4 attention gate"); return; }

    const QuantSeed seeds[4] = {{q_val, q_in, nullptr, sq * dd},
                                {k_val, k_in, nullptr, sk * dd},
                                {v_val, v_in, nullptr, sk * dv},
                                {scale_val, &inv_sqrt_d, nullptr, 1U}};
    float d_out[sq * dv] = {};
    REQUIRE(run_quant_module(compute, ctx, &root, plan, seeds, 4U, out_val, d_out, sq * dv));

    for (crd::u32 i = 0; i < sq * dv; ++i)
    {
        CHECK(crd::math::abs(d_out[i] - oracle[i]) <= 1e-4F * (1.0F + crd::math::abs(oracle[i]))); // expanded SDPA == CPU attention
    }
}

// CEIR-26d-3c (DX12 leg) — the ATTENTION GENERALIZATION CAPSTONE mirror: the SAME ml.attention now runs at GENERIC dims (Sq=3,
// Sk=5, D=4, Dv=2) on D3D12 — dims the 24z baked-kernel pre-check REJECTED. 26d-3a (Kᵀ→synth tensor.transpose) + 26d-3b (softmax→
// spec-const loop kernel, local_size←Sq, Sk←spec-const) made the composite dimension-general; the [[migrated=both backends]] mandate
// requires this DX12 mirror of the Vulkan 26d-3c gate. ⛔ D held at 4 (scale=1/√D=0.5 caller-computed). NO "raw" for a previously-
// REJECTED program (24b device-vs-oracle standard). ⛔ exp is float math → a DERIVED tol.
TEST_CASE("ceir 26d-3c: a GENERIC-dims ml.attention (Sq=3, Sk=5, D=4) runs device-resident on DX12 vs the CPU SDPA oracle",
          "[ceir][ml][gpu]")
{
    constexpr crd::u32 sq = 3; // query positions (≠2 — was BakedKernelShapeUnsupported)
    constexpr crd::u32 sk = 5; // key positions (≠3 — was BakedKernelShapeUnsupported)
    constexpr crd::u32 dd = 4; // head dim D (held at 4 so scale=1/√D=0.5 stays caller-computable)
    constexpr crd::u32 dv = 2; // value dim Dv

    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };

    ce::Value* const q_val = mkd(tf(ctx, sh2(ctx, sq, dd)));
    ce::Value* const k_val = mkd(tf(ctx, sh2(ctx, sk, dd)));
    ce::Value* const v_val = mkd(tf(ctx, sh2(ctx, sk, dv)));
    b->append(ce::ml::build_attention(ctx, q_val, k_val, v_val, tf(ctx, sh2(ctx, sq, dv))));

    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, *m);
    REQUIRE(er.error == ceg::MlExpandError::None); // ⛔ 26d-3c: was BakedKernelShapeUnsupported before the pre-check retired
    REQUIRE(er.expanded == 1U);
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);

    const ce::Value* out_val   = nullptr;
    const ce::Value* scale_val = nullptr;
    for (crd::usize i = 0; i < plan.buffers.size(); ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.role == ceg::BufferRole::Output) { out_val = pb.value; }
        else if (pb.role == ceg::BufferRole::ExternalIn && tnumel(ctx, pb.value->type()) == 1ULL) { scale_val = pb.value; }
    }
    REQUIRE(out_val != nullptr);
    REQUIRE(scale_val != nullptr);

    // ── CPU SDPA oracle: out = softmax(Q·Kᵀ / √D) · V (dimension-general) ──
    const float q_in[sq * dd] = {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F, 0.9F, 1.0F, 1.1F, 1.2F};
    const float k_in[sk * dd] = {0.2F, 0.1F, 0.0F, 0.3F, 0.4F, 0.5F, 0.6F, 0.1F, 0.7F, 0.2F,
                                 0.3F, 0.9F, 0.1F, 0.8F, 0.4F, 0.2F, 0.5F, 0.3F, 0.6F, 0.7F};
    const float v_in[sk * dv] = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F, 9.0F, 10.0F};
    const float inv_sqrt_d    = 1.0F / crd::math::sqrt(static_cast<float>(dd)); // 1/√D = 0.5

    float oracle[sq * dv] = {};
    for (crd::u32 i = 0; i < sq; ++i)
    {
        float sc[sk];
        float mx = -1e30F;
        for (crd::u32 j = 0; j < sk; ++j)
        {
            float dot = 0.0F;
            for (crd::u32 d = 0; d < dd; ++d) { dot += q_in[i * dd + d] * k_in[j * dd + d]; }
            sc[j] = dot * inv_sqrt_d;
            mx    = crd::math::max(mx, sc[j]);
        }
        float denom = 0.0F;
        for (crd::u32 j = 0; j < sk; ++j) { sc[j] = crd::math::exp(sc[j] - mx); denom += sc[j]; }
        for (crd::u32 kk = 0; kk < dv; ++kk)
        {
            float acc = 0.0F;
            for (crd::u32 j = 0; j < sk; ++j) { acc += (sc[j] / denom) * v_in[j * dv + kk]; }
            oracle[i * dv + kk] = acc;
        }
    }

    crd::gpu::Dx12ComputeContext compute(&root);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-26d-3c generic-attention gate"); return; }

    const QuantSeed seeds[4] = {{q_val, q_in, nullptr, sq * dd},
                                {k_val, k_in, nullptr, sk * dd},
                                {v_val, v_in, nullptr, sk * dv},
                                {scale_val, &inv_sqrt_d, nullptr, 1U}};
    float d_out[sq * dv] = {};
    // ⛔ CEIR-26f-4: call the N-runner directly to capture n_allocated — aliasing is now LIVE on DX12 (line 309 widened to
    //    alias_of>=0), so `probs` SHARES `Kt`'s buffer. The oracle compare below is the LIVE-ALIASING WITNESS on DX12.
    const QuantOut qo      = {out_val, d_out, sq * dv};
    crd::u32       n_alloc = 0;
    REQUIRE(run_quant_module_n(compute, ctx, &root, plan, seeds, 4U, &qo, 1U, &n_alloc));
    crd::u32 tenants = 0;
    for (crd::usize i = 0; i < plan.buffers.size(); ++i)
    {
        if (plan.buffers[i].alias_of >= 0 && plan.buffers[i].role == ceg::BufferRole::Intermediate) { ++tenants; }
    }
    REQUIRE(tenants == 1U);                                                  // probs→Kt — the ONE tenant (device-free 26f-2b(e))
    CHECK(n_alloc == static_cast<crd::u32>(plan.buffers.size()) - tenants);  // ⭐ EXACT physical-buffer count (identity)

    for (crd::u32 i = 0; i < sq * dv; ++i)
    {
        CHECK(crd::math::abs(d_out[i] - oracle[i]) <= 1e-4F * (1.0F + crd::math::abs(oracle[i]))); // generic SDPA == CPU attention (derived tol)
    }
}

// CEIR-26f-3b (DX12 MIRROR) — the memory-aliasing DIFFERENTIAL on D3D12: the SAME ml.attention module planned TWICE (share OFF vs
// ON, default) runs BOTH and asserts the outputs are BIT-EXACT equal + the physical-buffer count drops by EXACTLY the tenant count.
// Verbatim mirror of the Vulkan 26f-3b (the [[migrated=both backends]] mandate); aliasing changes ADDRESSES not kernels, so a bit-
// identical result proves the DX12 runner's alias resolution + the plan's lifetime analysis are SOUND (a wrong alias clobbers a live
// buffer → wrong output). ⛔ HONEST SCOPE (as Vulkan 26f-3b): this proves the runner + no-wrong-tenancy, NOT the WAR-ordering (attention
// has no reader of Kt after probs's write ⇒ no WAR hazard here; the barrier property is 26f-2's spec fact).
TEST_CASE("ceir 26f-3b: buffer-aliasing is BIT-EXACT vs the un-shared plan on DX12 (attention, one module both ways) + exact buffer-count delta",
          "[ceir][ml][gpu]")
{
    constexpr crd::u32 sq = 2;
    constexpr crd::u32 sk = 3;
    constexpr crd::u32 dd = 4;
    constexpr crd::u32 dv = 2;

    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };

    ce::Value* const q_val = mkd(tf(ctx, sh2(ctx, sq, dd)));
    ce::Value* const k_val = mkd(tf(ctx, sh2(ctx, sk, dd)));
    ce::Value* const v_val = mkd(tf(ctx, sh2(ctx, sk, dv)));
    b->append(ce::ml::build_attention(ctx, q_val, k_val, v_val, tf(ctx, sh2(ctx, sq, dv))));

    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, *m);
    REQUIRE(er.error == ceg::MlExpandError::None);
    REQUIRE(er.expanded == 1U);

    // ── plan the SAME module BOTH ways — the ONLY difference is share_intermediate_storage (fuse_gemm_relu is a no-op for attention) ──
    ceg::PlanOptions opt_noshare;
    opt_noshare.share_intermediate_storage = false;
    const ceg::TensorPipelinePlan plan_noshare = ceg::plan_tensor_pipeline(ctx, *m, &root, opt_noshare);
    const ceg::PlanOptions        opt_share; // share_intermediate_storage = true (the default)
    const ceg::TensorPipelinePlan plan_share = ceg::plan_tensor_pipeline(ctx, *m, &root, opt_share);
    REQUIRE(plan_noshare.reject == ceg::PlanReject::None);
    REQUIRE(plan_share.reject == ceg::PlanReject::None);

    const auto count_tenants = [](const ceg::TensorPipelinePlan& p) {
        crd::u32 n = 0;
        for (crd::usize i = 0; i < p.buffers.size(); ++i)
        {
            if (p.buffers[i].alias_of >= 0 && p.buffers[i].role == ceg::BufferRole::Intermediate) { ++n; }
        }
        return n;
    };
    const crd::u32 tenants = count_tenants(plan_share);
    REQUIRE(tenants == 1U);                                             // ⭐ probs tenants Kt (the sole disjoint pair; device-free 26f-2b(e))
    REQUIRE(count_tenants(plan_noshare) == 0U);                         // ⭐ sharing OFF ⇒ NO tenants
    REQUIRE(plan_noshare.buffers.size() == plan_share.buffers.size()); // same buffers, only alias_of differs

    const ce::Value* out_val   = nullptr;
    const ce::Value* scale_val = nullptr;
    for (crd::usize i = 0; i < plan_share.buffers.size(); ++i)
    {
        const ceg::PlanBuffer& pb = plan_share.buffers[i];
        if (pb.role == ceg::BufferRole::Output) { out_val = pb.value; }
        else if (pb.role == ceg::BufferRole::ExternalIn && tnumel(ctx, pb.value->type()) == 1ULL) { scale_val = pb.value; }
    }
    REQUIRE(out_val != nullptr);
    REQUIRE(scale_val != nullptr);

    crd::gpu::Dx12ComputeContext compute(&root);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-26f-3b aliasing differential"); return; }

    const float     q_in[sq * dd] = {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F};
    const float     k_in[sk * dd] = {0.2F, 0.1F, 0.0F, 0.3F, 0.4F, 0.5F, 0.6F, 0.1F, 0.7F, 0.2F, 0.3F, 0.9F};
    const float     v_in[sk * dv] = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
    const float     inv_sqrt_d    = 1.0F / crd::math::sqrt(static_cast<float>(dd));
    const QuantSeed seeds[4]      = {{q_val, q_in, nullptr, sq * dd},
                                     {k_val, k_in, nullptr, sk * dd},
                                     {v_val, v_in, nullptr, sk * dv},
                                     {scale_val, &inv_sqrt_d, nullptr, 1U}};

    float          out_noshare[sq * dv] = {};
    float          out_share[sq * dv]   = {};
    crd::u32       n_alloc_noshare      = 0;
    crd::u32       n_alloc_share        = 0;
    const QuantOut qo_ns = {out_val, out_noshare, sq * dv};
    const QuantOut qo_s  = {out_val, out_share, sq * dv};
    REQUIRE(run_quant_module_n(compute, ctx, &root, plan_noshare, seeds, 4U, &qo_ns, 1U, &n_alloc_noshare));
    REQUIRE(run_quant_module_n(compute, ctx, &root, plan_share, seeds, 4U, &qo_s, 1U, &n_alloc_share));

    CHECK(n_alloc_share == n_alloc_noshare - tenants); // ⭐ sharing frees EXACTLY `tenants` physical buffers (NOT "fewer")

    // ⭐ BIT-EXACT (NOT tol): aliasing changes probs's ADDRESS not the kernels — a wrong lifetime clobbers a live Kt ⇒ wrong output.
    int first_mismatch = -1;
    for (crd::u32 i = 0; i < sq * dv && first_mismatch < 0; ++i)
    {
        if (out_share[i] != out_noshare[i]) { first_mismatch = static_cast<int>(i); }
    }
    CAPTURE(first_mismatch);
    if (first_mismatch >= 0)
    {
        CAPTURE(out_share[static_cast<crd::usize>(first_mismatch)], out_noshare[static_cast<crd::usize>(first_mismatch)]);
    }
    CHECK(first_mismatch == -1); // every output element bit-identical shared-vs-unshared ⇒ the lifetime analysis is SOUND on DX12
}

// CEIR-24b-4 (DX12 leg) — the §138 ML PROOF (MLP leg) on D3D12: an authored ml.mlp (2-layer, relu) EXPANDED into gemm/relu runs
// device-resident and matches the CPU float MLP oracle. Dims x[4,8]·W1[8,8]·relu·W2[8,2]->y[4,2] (h1=32 == relu.ckir local_size).
TEST_CASE("ceir 24b-4: an expanded ml.mlp runs device-resident on DX12 (gemm/relu vs the CPU float MLP oracle)", "[ceir][ml][gpu]")
{
    constexpr crd::u32 mrows = 4;
    constexpr crd::u32 d0    = 8;
    constexpr crd::u32 d1    = 8;
    constexpr crd::u32 d2    = 2;

    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };

    ce::Value* const x_val  = mkd(tf(ctx, sh2(ctx, mrows, d0)));
    ce::Value* const w1_val = mkd(tf(ctx, sh2(ctx, d0, d1)));
    ce::Value* const w2_val = mkd(tf(ctx, sh2(ctx, d1, d2)));
    ce::Value*       mlpops[3] = {x_val, w1_val, w2_val};
    ce::Operation* const mo = ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mlpops, 3U),
                                                   1U, tf(ctx, sh2(ctx, mrows, d2)), 0U);
    ctx.set_attr(mo, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
    b->append(mo);

    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, *m);
    REQUIRE(er.error == ceg::MlExpandError::None);
    REQUIRE(er.expanded == 1U);
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);

    const ce::Value* out_val = nullptr;
    for (crd::usize i = 0; i < plan.buffers.size(); ++i)
    {
        if (plan.buffers[i].role == ceg::BufferRole::Output) { out_val = plan.buffers[i].value; }
    }
    REQUIRE(out_val != nullptr);

    float x_in[mrows * d0];
    float w1_in[d0 * d1];
    float w2_in[d1 * d2];
    for (crd::u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<float>(static_cast<int>(i) - 12); }
    for (crd::u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<float>(static_cast<int>(i % 7) - 3); }
    for (crd::u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<float>(static_cast<int>(i % 5) - 2); }

    float oracle[mrows * d2] = {};
    for (crd::u32 mm = 0; mm < mrows; ++mm)
    {
        float h1[d1];
        for (crd::u32 n = 0; n < d1; ++n)
        {
            float acc = 0.0F;
            for (crd::u32 kk = 0; kk < d0; ++kk) { acc += x_in[mm * d0 + kk] * w1_in[kk * d1 + n]; }
            h1[n] = crd::math::max(acc, 0.0F);
        }
        for (crd::u32 j = 0; j < d2; ++j)
        {
            float acc = 0.0F;
            for (crd::u32 n = 0; n < d1; ++n) { acc += h1[n] * w2_in[n * d2 + j]; }
            oracle[mm * d2 + j] = acc;
        }
    }

    crd::gpu::Dx12ComputeContext compute(&root);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-24b-4 MLP gate"); return; }

    const QuantSeed seeds[3] = {{x_val, x_in, nullptr, mrows * d0}, {w1_val, w1_in, nullptr, d0 * d1}, {w2_val, w2_in, nullptr, d1 * d2}};
    float d_out[mrows * d2] = {};
    REQUIRE(run_quant_module(compute, ctx, &root, plan, seeds, 3U, out_val, d_out, mrows * d2));

    for (crd::u32 i = 0; i < mrows * d2; ++i)
    {
        CHECK(crd::math::abs(d_out[i] - oracle[i]) <= 1e-4F * (1.0F + crd::math::abs(oracle[i]))); // expanded MLP == CPU float MLP
    }
}

// CEIR-26d-2d (DX12 leg) — the shape-specialization PROVING gate MIRROR (the both-backends mandate; closes 26d-2): a non-32-width
// ml.mlp — previously BakedKernelShapeUnsupported — runs DEVICE-RESIDENT on D3D12, relu.ckir's local_size cook-bound to the
// intermediate numel (bind_authored_local_size, the SAME shared-lib mechanism proven on Vulkan at 26d-2c), == the CPU float MLP
// oracle. Two widths (h1=64, h1=96) + the oversize negative (h1=1200 > cap ⇒ resolver UnresolvedKernel), with the discriminating
// check pinning LocalSizeExceedsLimit specifically (not any failure). The 1e-4 rel-tol is f32 accumulation order (device vs
// sequential oracle), not slop; NO "raw" for a previously-REJECTED program (the 24b device-vs-oracle standard).
TEST_CASE("ceir 26d-2d: a non-32-width ml.mlp runs device-resident on DX12 (relu cook-bound) == the CPU float MLP oracle",
          "[ceir][ml][gpu]")
{
    crd::memory::GrowableTlsfAllocator devroot;
    crd::gpu::Dx12ComputeContext       compute(&devroot);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-26d-2d non-32 MLP gate"); return; }

    const auto run_mlp = [&](crd::u32 d1) -> bool {
        constexpr crd::u32 mrows = 4;
        constexpr crd::u32 d0    = 8;
        constexpr crd::u32 d2    = 2;
        crd::memory::GrowableTlsfAllocator root;
        ce::Context                        ctx(&root);
        (void)ce::func::register_dialect(ctx);
        (void)ce::resource::register_resource_ops(ctx);
        (void)ce::arith::register_arith_ops(ctx);
        (void)ce::compute::register_compute_ops(ctx);
        (void)ce::linalg::register_dialect(ctx);
        (void)ce::ml::register_dialect(ctx);
        ce::Module* const m   = ctx.create_module();
        ce::Block*        top = m->body()->first_block();
        if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
        ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
        top->append(f);
        ce::Block* const b   = ce::func::func_body_block(f);
        const ce::OpId   dcl = ctx.intern_op("resource", "declare");
        const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };

        ce::Value* const x_val     = mkd(tf(ctx, sh2(ctx, mrows, d0)));
        ce::Value* const w1_val    = mkd(tf(ctx, sh2(ctx, d0, d1)));
        ce::Value* const w2_val    = mkd(tf(ctx, sh2(ctx, d1, d2)));
        ce::Value*       mlpops[3] = {x_val, w1_val, w2_val};
        ce::Operation* const mo = ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mlpops, 3U),
                                                       1U, tf(ctx, sh2(ctx, mrows, d2)), 0U);
        ctx.set_attr(mo, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
        b->append(mo);

        const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, *m);
        REQUIRE(er.error == ceg::MlExpandError::None);
        REQUIRE(er.expanded == 1U);
        // ⭐ CEIR-26e-4: PIN fuse_gemm_relu=OFF (mirror of 26d-2c) — 26d-2d tests the relu VizDispatch cook-bind + oversize
        //    LocalSizeExceedsLimit cap (the 26d mechanism), which exists ONLY when relu is a STANDALONE dispatch; fusion default-ON
        //    folds it into the gemm store (no VizDispatch). The FUSED DX12 path is proven bit-exact vs THIS unfused form at 26e-4's
        //    differential; the mechanism's teeth (they still guard standalone relu / softmax / relu_vjp) need the unfused plan.
        ceg::PlanOptions opts;
        opts.fuse_gemm_relu = false;
        const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root, opts);
        REQUIRE(plan.reject == ceg::PlanReject::None);

        const ce::Value* out_val = nullptr;
        for (crd::usize i = 0; i < plan.buffers.size(); ++i)
        {
            if (plan.buffers[i].role == ceg::BufferRole::Output) { out_val = plan.buffers[i].value; }
        }
        REQUIRE(out_val != nullptr);

        // ⛔ DISCRIMINATING CHECK (advisor, mirror of the Vulkan leg): pin WHICH cook-bind outcome this width triggers on the relu
        //    VizDispatch stage, so the oversize negative asserts LocalSizeExceedsLimit specifically (not any failure).
        crd::u64 relu_numel = 0ULL;
        for (crd::usize i = 0; i < plan.stages.size(); ++i)
        {
            const ceg::PlanStage& st = plan.stages[i];
            if (st.kind == ceg::StageKind::VizDispatch && st.n_out >= 1U)
            {
                relu_numel = tnumel(ctx, st.op->operand(3U + st.nbind - st.n_out)->type());
                break;
            }
        }
        crd::u32                    probe_ls = 0U;
        const ceg::KernelShapeError kse      = ceg::bind_authored_local_size(probe_ls, relu_numel, ceg::kMaxAuthoredLocalSize);
        if (relu_numel > static_cast<crd::u64>(ceg::kMaxAuthoredLocalSize))
        {
            CHECK(kse == ceg::KernelShapeError::LocalSizeExceedsLimit);
        }
        else
        {
            CHECK(kse == ceg::KernelShapeError::None);
            CHECK(probe_ls == static_cast<crd::u32>(relu_numel));
        }

        crd::containers::Array<float> x_in(&root);
        crd::containers::Array<float> w1_in(&root);
        crd::containers::Array<float> w2_in(&root);
        x_in.resize(mrows * d0, 0.0F);
        w1_in.resize(d0 * d1, 0.0F);
        w2_in.resize(d1 * d2, 0.0F);
        for (crd::u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<float>(static_cast<int>(i) - 12); }
        for (crd::u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<float>(static_cast<int>(i % 7) - 3); }
        for (crd::u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<float>(static_cast<int>(i % 5) - 2); }

        crd::containers::Array<float> oracle(&root);
        oracle.resize(mrows * d2, 0.0F);
        for (crd::u32 mm = 0; mm < mrows; ++mm)
        {
            crd::containers::Array<float> h1(&root);
            h1.resize(d1, 0.0F);
            for (crd::u32 nnn = 0; nnn < d1; ++nnn)
            {
                float acc = 0.0F;
                for (crd::u32 kk = 0; kk < d0; ++kk) { acc += x_in[mm * d0 + kk] * w1_in[kk * d1 + nnn]; }
                h1[nnn] = crd::math::max(acc, 0.0F);
            }
            for (crd::u32 j = 0; j < d2; ++j)
            {
                float acc = 0.0F;
                for (crd::u32 nnn = 0; nnn < d1; ++nnn) { acc += h1[nnn] * w2_in[nnn * d2 + j]; }
                oracle[mm * d2 + j] = acc;
            }
        }

        const QuantSeed seeds[3] = {{x_val, x_in.data(), nullptr, mrows * d0},
                                    {w1_val, w1_in.data(), nullptr, d0 * d1},
                                    {w2_val, w2_in.data(), nullptr, d1 * d2}};
        crd::containers::Array<float> d_out(&root);
        d_out.resize(mrows * d2, 0.0F);
        const bool ran = run_quant_module(compute, ctx, &root, plan, seeds, 3U, out_val, d_out.data(), mrows * d2);
        if (ran)
        {
            for (crd::u32 i = 0; i < mrows * d2; ++i)
            {
                CHECK(crd::math::abs(d_out[i] - oracle[i]) <= 1e-4F * (1.0F + crd::math::abs(oracle[i])));
            }
        }
        return ran;
    };

    CHECK(run_mlp(16U));       // h1 = 4·16 = 64 — was BakedKernelShapeUnsupported, now cook-bound + runs == oracle on DX12
    CHECK(run_mlp(24U));       // h1 = 4·24 = 96 — a non-power-of-two width
    CHECK_FALSE(run_mlp(300U)); // h1 = 4·300 = 1200 > 1024 cap ⇒ resolver UnresolvedKernel (the discriminating check pins the reject)
}

// CEIR-26e-4 — the gemm→relu FUSION DIFFERENTIAL, DX12 MIRROR of 26e-3b (closes 26e — both backends): the SAME fp32-MLP module
// planned TWICE — fuse OFF (Gemm→relu.ckir dispatch, 3 stages) vs ON (GemmRelu's max(acc,0) store, 2 stages) — runs BOTH on D3D12
// and asserts the outputs are BIT-EXACT equal (NOT tol). Same argument as Vulkan: the contract K-loop is the SAME emit_contract_hlsl
// (`precise float acc`), an f32 store/load is identity + max is FMax — the fused synthesized kernel is a NEW EMITTER PATH that
// stays byte-identical to the authored-relu-dispatch program. The [[feedback_semantics_preserving_pass_differential_test_is_bit_exact_vs_unoptimized_program]] contract.
TEST_CASE("ceir 26e-4: the gemm-relu fusion is BIT-EXACT vs the unfused program on DX12 (raw-vs-opt, one module planned both ways)",
          "[ceir][ml][gpu]")
{
    crd::memory::GrowableTlsfAllocator devroot;
    crd::gpu::Dx12ComputeContext       compute(&devroot);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-26e-4 fusion differential"); return; }

    constexpr crd::u32 mrows = 4;
    constexpr crd::u32 d0    = 8;
    constexpr crd::u32 d1    = 16; // 26d-2d's KNOWN-GOOD shape (h1 numel 64 ≤ 1024 — the unfused relu cook-binds + ran green)
    constexpr crd::u32 d2    = 2;
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };

    ce::Value* const     x_val     = mkd(tf(ctx, sh2(ctx, mrows, d0)));
    ce::Value* const     w1_val    = mkd(tf(ctx, sh2(ctx, d0, d1)));
    ce::Value* const     w2_val    = mkd(tf(ctx, sh2(ctx, d1, d2)));
    ce::Value*           mlpops[3] = {x_val, w1_val, w2_val};
    ce::Operation* const mo        = ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mlpops, 3U),
                                                          1U, tf(ctx, sh2(ctx, mrows, d2)), 0U);
    ctx.set_attr(mo, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
    b->append(mo);
    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, *m);
    REQUIRE(er.error == ceg::MlExpandError::None);
    REQUIRE(er.expanded == 1U);

    // ── plan the SAME module BOTH ways — the raw-vs-opt pair ──
    ceg::PlanOptions opt_u;
    opt_u.fuse_gemm_relu = false;
    const ceg::TensorPipelinePlan plan_u = ceg::plan_tensor_pipeline(ctx, *m, &root, opt_u);
    const ceg::PlanOptions        opt_f; // fuse_gemm_relu = true (the default)
    const ceg::TensorPipelinePlan plan_f = ceg::plan_tensor_pipeline(ctx, *m, &root, opt_f);
    REQUIRE(plan_u.reject == ceg::PlanReject::None);
    REQUIRE(plan_f.reject == ceg::PlanReject::None);
    REQUIRE(plan_u.stages.size() == 3U); // Gemm + VizDispatch(relu) + Gemm
    REQUIRE(plan_f.stages.size() == 2U); // GemmRelu + Gemm (N-1)
    bool has_gemmrelu = false;
    for (crd::usize i = 0; i < plan_f.stages.size(); ++i) { has_gemmrelu = has_gemmrelu || plan_f.stages[i].kind == ceg::StageKind::GemmRelu; }
    REQUIRE(has_gemmrelu);

    const ce::Value* out_val = nullptr;
    for (crd::usize i = 0; i < plan_f.buffers.size(); ++i)
    {
        if (plan_f.buffers[i].role == ceg::BufferRole::Output) { out_val = plan_f.buffers[i].value; }
    }
    REQUIRE(out_val != nullptr);

    // 26d-2d's EXACT seeds (NEGATIVE pre-activations, magnitudes ≫ ulp — asserted below, so CPU-sign == GPU-sign, non-vacuous).
    crd::containers::Array<float> x_in(&root);
    crd::containers::Array<float> w1_in(&root);
    crd::containers::Array<float> w2_in(&root);
    x_in.resize(mrows * d0, 0.0F);
    w1_in.resize(d0 * d1, 0.0F);
    w2_in.resize(d1 * d2, 0.0F);
    for (crd::u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<float>(static_cast<int>(i) - 12); }
    for (crd::u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<float>(static_cast<int>(i % 7) - 3); }
    for (crd::u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<float>(static_cast<int>(i % 5) - 2); }

    int neg_count = 0;
    for (crd::u32 mm = 0; mm < mrows; ++mm)
    {
        for (crd::u32 nnn = 0; nnn < d1; ++nnn)
        {
            float acc = 0.0F;
            for (crd::u32 kk = 0; kk < d0; ++kk) { acc += x_in[mm * d0 + kk] * w1_in[kk * d1 + nnn]; }
            if (acc < 0.0F) { ++neg_count; }
        }
    }
    REQUIRE(neg_count > 0); // relu flips ≥1 element — the witness has teeth

    const QuantSeed seeds[3] = {{x_val, x_in.data(), nullptr, mrows * d0},
                                {w1_val, w1_in.data(), nullptr, d0 * d1},
                                {w2_val, w2_in.data(), nullptr, d1 * d2}};
    crd::containers::Array<float> out_u(&root);
    crd::containers::Array<float> out_f(&root);
    out_u.resize(mrows * d2, 0.0F);
    out_f.resize(mrows * d2, 0.0F);
    REQUIRE(run_quant_module(compute, ctx, &root, plan_u, seeds, 3U, out_val, out_u.data(), mrows * d2)); // unfused arm (fuse=false)
    REQUIRE(run_quant_module(compute, ctx, &root, plan_f, seeds, 3U, out_val, out_f.data(), mrows * d2)); // fused arm (fuse=true)

    int first_mismatch = -1;
    for (crd::u32 i = 0; i < mrows * d2 && first_mismatch < 0; ++i)
    {
        if (out_f[i] != out_u[i]) { first_mismatch = static_cast<int>(i); }
    }
    CAPTURE(first_mismatch);
    if (first_mismatch >= 0)
    {
        CAPTURE(out_f[static_cast<crd::usize>(first_mismatch)], out_u[static_cast<crd::usize>(first_mismatch)]);
    }
    CHECK(first_mismatch == -1); // every output element bit-identical fused-vs-unfused on DX12
}

// CEIR-27b — the §146 TWO-SCHEDULE DIFFERENTIAL, DX12 MIRROR of the Vulkan 27b (closes 27b — both backends): the SAME fp32-MLP
// payload planned with TWO committed `.ceir` transform schedules — schedule_nofuse (fuse=false → 3 stages) vs schedule_fuse
// (fuse=true → GemmRelu, 2 stages) — runs BOTH on D3D12 and asserts BIT-EXACT outputs. 26e-4's fusion differential with the C++
// PlanOptions LITERALS replaced by PARSED authored schedule assets (plan_options_from_transform): "two schedules optimize ONE
// semantic program" (§71/§146). ⛔ share is program-global-INERT on the MLP (26f-3a) — asserted tenants==0 on BOTH.
TEST_CASE("ceir 27b: two AUTHORED .ceir transform schedules optimize one MLP program BIT-EXACT on DX12 (fuse asset vs no-fuse asset)",
          "[ceir][ml][gpu][transform]")
{
    crd::memory::GrowableTlsfAllocator devroot;
    crd::gpu::Dx12ComputeContext       compute(&devroot);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-27b two-schedule differential"); return; }

    constexpr crd::u32 mrows = 4;
    constexpr crd::u32 d0    = 8;
    constexpr crd::u32 d1    = 16; // 26e-4's KNOWN-GOOD shape
    constexpr crd::u32 d2    = 2;
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    (void)ce::transform::register_transform_ops(ctx); // ⛔ register transform BEFORE parsing a schedule (else its ops parse opaque)

    // ── the PAYLOAD: an fp32 2-layer MLP (x·W1 → relu → ·W2) ──
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
    ce::Value* const     x_val     = mkd(tf(ctx, sh2(ctx, mrows, d0)));
    ce::Value* const     w1_val    = mkd(tf(ctx, sh2(ctx, d0, d1)));
    ce::Value* const     w2_val    = mkd(tf(ctx, sh2(ctx, d1, d2)));
    ce::Value*           mlpops[3] = {x_val, w1_val, w2_val};
    ce::Operation* const mo        = ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mlpops, 3U),
                                                          1U, tf(ctx, sh2(ctx, mrows, d2)), 0U);
    ctx.set_attr(mo, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
    b->append(mo);
    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, *m);
    REQUIRE(er.error == ceg::MlExpandError::None);
    REQUIRE(er.expanded == 1U);

    // ── the two SCHEDULES are AUTHORED .ceir ASSETS: parse → walk (well-formed) → load into PlanOptions (§146) ──
    const auto load_sched = [&](const char* path) -> ceg::PlanOptions {
        std::ifstream sf(path, std::ios::binary | std::ios::ate);
        REQUIRE(sf.good());
        const std::streamsize ssz = sf.tellg();
        sf.seekg(0);
        crd::containers::Array<char> ssrc(&root);
        ssrc.resize(static_cast<crd::usize>(ssz), '\0');
        sf.read(ssrc.data(), ssz);
        const ce::ParseResult spr = ce::parse(ctx, crd::containers::StringView(ssrc.data(), static_cast<crd::usize>(ssz)));
        REQUIRE(spr.ok);
        REQUIRE(spr.module != nullptr);
        CHECK(ce::transform::find_transform_misuse(ctx, *spr.module).kind == ce::transform::TransformMisuseKind::None);
        return ceg::plan_options_from_transform(ctx, *spr.module, ceg::PlanOptions{});
    };
    const ceg::PlanOptions opt_u = load_sched(CRD_REPO_DIR "/assets/ceir/schedule_nofuse.ceir");
    const ceg::PlanOptions opt_f = load_sched(CRD_REPO_DIR "/assets/ceir/schedule_fuse.ceir");
    REQUIRE(opt_u.fuse_gemm_relu == false);
    REQUIRE(opt_f.fuse_gemm_relu == true);

    // ── plan the SAME payload with the TWO authored schedules ──
    const ceg::TensorPipelinePlan plan_u = ceg::plan_tensor_pipeline(ctx, *m, &root, opt_u);
    const ceg::TensorPipelinePlan plan_f = ceg::plan_tensor_pipeline(ctx, *m, &root, opt_f);
    REQUIRE(plan_u.reject == ceg::PlanReject::None);
    REQUIRE(plan_f.reject == ceg::PlanReject::None);
    REQUIRE(plan_u.stages.size() == 3U); // no-fuse: Gemm + VizDispatch(relu) + Gemm
    REQUIRE(plan_f.stages.size() == 2U); // fuse: GemmRelu + Gemm
    bool has_gemmrelu = false;
    for (crd::usize i = 0; i < plan_f.stages.size(); ++i) { has_gemmrelu = has_gemmrelu || plan_f.stages[i].kind == ceg::StageKind::GemmRelu; }
    REQUIRE(has_gemmrelu);
    const auto count_tenants = [](const ceg::TensorPipelinePlan& p) {
        crd::u32 n = 0;
        for (crd::usize i = 0; i < p.buffers.size(); ++i)
        {
            if (p.buffers[i].alias_of >= 0 && p.buffers[i].role == ceg::BufferRole::Intermediate) { ++n; }
        }
        return n;
    };
    REQUIRE(count_tenants(plan_u) == 0U); // share is program-global-INERT on the MLP (26f-3a no-disjoint-pair)
    REQUIRE(count_tenants(plan_f) == 0U);

    const ce::Value* out_val = nullptr;
    for (crd::usize i = 0; i < plan_f.buffers.size(); ++i)
    {
        if (plan_f.buffers[i].role == ceg::BufferRole::Output) { out_val = plan_f.buffers[i].value; }
    }
    REQUIRE(out_val != nullptr);

    crd::containers::Array<float> x_in(&root);
    crd::containers::Array<float> w1_in(&root);
    crd::containers::Array<float> w2_in(&root);
    x_in.resize(mrows * d0, 0.0F);
    w1_in.resize(d0 * d1, 0.0F);
    w2_in.resize(d1 * d2, 0.0F);
    for (crd::u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<float>(static_cast<int>(i) - 12); }
    for (crd::u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<float>(static_cast<int>(i % 7) - 3); }
    for (crd::u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<float>(static_cast<int>(i % 5) - 2); }
    int neg_count = 0;
    for (crd::u32 mm = 0; mm < mrows; ++mm)
    {
        for (crd::u32 nnn = 0; nnn < d1; ++nnn)
        {
            float acc = 0.0F;
            for (crd::u32 kk = 0; kk < d0; ++kk) { acc += x_in[mm * d0 + kk] * w1_in[kk * d1 + nnn]; }
            if (acc < 0.0F) { ++neg_count; }
        }
    }
    REQUIRE(neg_count > 0); // relu flips ≥1 element — the differential has teeth

    const QuantSeed seeds[3] = {{x_val, x_in.data(), nullptr, mrows * d0},
                                {w1_val, w1_in.data(), nullptr, d0 * d1},
                                {w2_val, w2_in.data(), nullptr, d1 * d2}};
    crd::containers::Array<float> out_u(&root);
    crd::containers::Array<float> out_f(&root);
    out_u.resize(mrows * d2, 0.0F);
    out_f.resize(mrows * d2, 0.0F);
    REQUIRE(run_quant_module(compute, ctx, &root, plan_u, seeds, 3U, out_val, out_u.data(), mrows * d2)); // no-fuse schedule
    REQUIRE(run_quant_module(compute, ctx, &root, plan_f, seeds, 3U, out_val, out_f.data(), mrows * d2)); // fuse schedule

    // ⭐ BIT-EXACT (NOT tol): the two AUTHORED schedules lower the SAME program to different plans but IDENTICAL values.
    int first_mismatch = -1;
    for (crd::u32 i = 0; i < mrows * d2 && first_mismatch < 0; ++i)
    {
        if (out_f[i] != out_u[i]) { first_mismatch = static_cast<int>(i); }
    }
    CAPTURE(first_mismatch);
    if (first_mismatch >= 0)
    {
        CAPTURE(out_f[static_cast<crd::usize>(first_mismatch)], out_u[static_cast<crd::usize>(first_mismatch)]);
    }
    CHECK(first_mismatch == -1); // every output element bit-identical: two schedules, one program, same values (§146) on DX12
}

TEST_CASE("ceir 25b-4a: a transpose+broadcast+elementwise chain runs device-resident on D3D12 (the new StageKinds, multi-stage)",
          "[ceir][tensor-pipeline][gpu]")
{
    // in0[2,3] --transpose[1,0]--> t[3,2] ; in1[3,1] --broadcast--> bc[3,2] ; add(t,bc)-> e[3,2] (three NEW-kind stages, one submit).
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::tensor::register_dialect(ctx);
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
    ce::Value* const     in0 = mkd(tf(ctx, sh2(ctx, 2U, 3U)));
    ce::Operation* const tr  = ce::tensor::build_transpose(ctx, in0, ctx.attr_string(crd::containers::StringView("1,0")), tf(ctx, sh2(ctx, 3U, 2U)));
    b->append(tr);
    ce::Value* const     in1 = mkd(tf(ctx, sh2(ctx, 3U, 1U)));
    ce::Operation* const bc  = ce::tensor::build_broadcast(ctx, in1, tf(ctx, sh2(ctx, 3U, 2U)));
    b->append(bc);
    ce::Operation* const ew = ce::tensor::build_elementwise(ctx, tr->result(0U), bc->result(0U),
                                                            ctx.attr_string(crd::containers::StringView("add")), tf(ctx, sh2(ctx, 3U, 2U)));
    b->append(ew);

    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 3U);

    // independent ref: e[r,c] = in0[c*3+r] (transpose) + in1[r] (broadcast).
    static float in0_data[6] = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
    static float in1_data[3] = {10.0F, 20.0F, 30.0F};
    float              ref[6];
    for (int r = 0; r < 3; ++r) { for (int cc = 0; cc < 2; ++cc) { ref[r * 2 + cc] = in0_data[cc * 3 + r] + in1_data[r]; } }

    crd::gpu::Dx12ComputeContext compute(&root);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-25b-4a chain gate"); return; }
    const QuantSeed seeds[2] = {{in0, in0_data, nullptr, 6U}, {in1, in1_data, nullptr, 3U}};
    float           got[6]   = {};
    REQUIRE(run_quant_module(compute, ctx, &root, plan, seeds, 2U, ew->result(0U), got, 6U));
    for (int i = 0; i < 6; ++i) { CHECK(got[i] == ref[i]); } // pure data-movement + add of exact f32 ⇒ EXACT
}

// CEIR-25b-4b / 25c-2: the FD-witness functors SumGemmAA + MlpLoss are SHARED with the Vulkan pipeline TU — ONE definition in
// gpu-shared/autodiff_fd_functors.hpp (pure f64/templated, no backend dependency; the "forward is hesap, not hand-written" rationale
// lives there). Hoisted at 25c-2b close (2nd distinct FD functor × 2 TUs = 4 copies).
namespace
{
using crd::ceir::gpu_test::MlpLoss;
using crd::ceir::gpu_test::SumGemmAA;
} // namespace

TEST_CASE("ceir 25b-4b: the backward pass of sum(gemm(A,A)) runs device-resident on DX12 (build_gradient->plan->execute) == hesap matmul_vjp + FD",
          "[ceir][tensor-pipeline][gpu]")
{
    // ── the 25a-2 corpus differentiated END-TO-END: loss[3]=reduce(gemm(A[3,3],A[3,3]),axis=1,sum); build_gradient emits the WHOLE
    //    backward graph (reduce-VJP reshape+broadcast, gemm-VJP 2 transpose + 2 gemm, 1 accumulation add). grads[0] = d sum(C)/dA. ──
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::tensor::register_dialect(ctx);
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };

    constexpr int        nn   = 3;
    ce::Value* const     a    = mkd(tf(ctx, sh2(ctx, nn, nn)));
    ce::Value* const     carg = mkd(tf(ctx, sh2(ctx, nn, nn))); // the gemm beta*C operand (beta=0)
    ce::Operation* const g    = ce::linalg::build_gemm(ctx, a, a, carg, ctx.attr_float(1.0), ctx.attr_float(0.0),
                                                       ctx.attr_bool(false), ctx.attr_bool(false), tf(ctx, sh2(ctx, nn, nn)));
    b->append(g);
    ce::Operation* const rd = ce::tensor::build_reduce(ctx, g->result(0U), ctx.attr_int(1),
                                                       ctx.attr_string(crd::containers::StringView("sum")), tf(ctx, sh1(ctx, nn)));
    b->append(rd);

    ceg::VjpRegistry reg(&root);
    ceg::register_builtin_vjps(reg, ctx);
    ce::Value*            wrt[1]   = {a};
    ce::Value*            grads[1] = {nullptr};
    const ceg::GradResult gr =
        ceg::build_gradient(ctx, *m, reg, rd->result(0U), crd::containers::ConstSpan<ce::Value*>(wrt, 1U),
                            crd::containers::Span<ce::Value*>(grads, 1U), &root);
    REQUIRE(gr.error == ceg::GradError::None);
    REQUIRE(grads[0] != nullptr);
    REQUIRE(gr.seed != nullptr); // STEP 1's seed-handle API — the harness names THIS buffer to upload all-ones (dLoss)

    // ── PLAN (device-free, ALWAYS runs — the all-skip guard). STEP 1 pinned the 8 stages + the readback target. ──
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 8U);

    // ── the DIALECT-INDEPENDENT reference: grads[0] = d sum(gemm(A,A))/dA = matmul_vjp(A,A,dC).ga + .gb with dC = all-ones (seeding
    //    dLoss=1 broadcasts a 1 into every dC[i,j] through the reduce VJP). Cross-checked against a central-difference FD witness on
    //    f(A)=sum(gemm(A,A)) — both f64, so the ANALYTIC ref is VALIDATED before it judges the device (never a hand-computed ref). ──
    namespace nnr = crd::hesap::autodiff::reverse::nn;
    double av[nn * nn];
    for (int i = 0; i < nn * nn; ++i) { av[i] = 0.4 + 0.17 * static_cast<double>(i) - 0.03 * static_cast<double>((i * 5) % 7); }
    double dc[nn * nn];
    for (int i = 0; i < nn * nn; ++i) { dc[i] = 1.0; }
    double ga[nn * nn];
    double gb[nn * nn];
    nnr::matmul_vjp(av, av, dc, ga, gb, nn, nn, nn);
    double ref[nn * nn];
    for (int i = 0; i < nn * nn; ++i) { ref[i] = ga[i] + gb[i]; }
    double g_fd[nn * nn];
    crd::hesap::autodiff::testing::grad_fd<nn * nn>(SumGemmAA{nn}, av, g_fd);
    for (int i = 0; i < nn * nn; ++i) { CHECK(crd::math::abs(ref[i] - g_fd[i]) <= 1e-5 * (1.0 + crd::math::abs(ref[i]))); }

    // ── DEVICE (soft-skip with no adapter): seed A + the all-ones dLoss BY VALUE; the dead β=0 accumulators (carg + the two backward
    //    gemm C-operands) auto-zero (unseeded ExternalIn → zeros). Read back grads[0] (the plan's single Output). ──
    crd::gpu::Dx12ComputeContext compute(&root);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-25b-4b backward-pass gate"); return; }

    float av_f[nn * nn];
    for (int i = 0; i < nn * nn; ++i) { av_f[i] = static_cast<float>(av[i]); }
    float           ones[nn]       = {1.0F, 1.0F, 1.0F};
    const QuantSeed seeds[2]       = {{a, av_f, nullptr, static_cast<crd::u32>(nn * nn)}, {gr.seed, ones, nullptr, static_cast<crd::u32>(nn)}};
    float           got[nn * nn]   = {};
    REQUIRE(run_quant_module(compute, ctx, &root, plan, seeds, 2U, grads[0], got, static_cast<crd::usize>(nn * nn)));
    // device f32 (two backward gemms + an add) vs the f64 analytic ref — a DERIVED relative tolerance (3-element f32 dot, |ref|~O(6)).
    for (int i = 0; i < nn * nn; ++i)
    {
        CHECK(crd::math::abs(static_cast<double>(got[i]) - ref[i]) <= 1e-4 * (1.0 + crd::math::abs(ref[i])));
    }
}

// CEIR-25c-1b — the STANDALONE relu_vjp device gate (DX12). An authored compute.dispatch(@relu_vjp, {x, gy, gx} r,r,w) — the
// MLP-backward relu VJP (gx = x>0 ? gy : 0, x the forward PRE-activation) — is PLANNED (one VizDispatch stage) and EXECUTED
// device-resident on DX12, matching hesap nn_reverse::relu_vjp. relu_vjp.ckir declares readonly inputs (x@0/gy@1 axes=0), BUT the
// HLSL emitter DROPS readonly — every buffer becomes a `RWByteAddressBuffer` UAV (ckir_hlsl.hpp:217/243, the uniform root sig). This
// is the ledgered HLSL-masks-readonly gap: the gate ASSERTS the HLSL has NO `readonly` token + IS all `RWByteAddressBuffer` (the
// opposite of the Vulkan gate's `readonly buffer` — identity-not-category, both backends). Same corpus / EXACT `==` as the Vulkan gate.
TEST_CASE("ceir 25c-1b: the authored relu_vjp compute kernel (readonly dropped to UAV) runs device-resident on DX12 == hesap relu_vjp",
          "[ceir][tensor-pipeline][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);     // arith.const — the dispatch grid operand
    (void)ce::compute::register_compute_ops(ctx); // compute.dispatch — the authored-kernel stage
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };

    constexpr crd::u32 nel = 32; // x/gy/gx are [8,4] = 32 f32; relu_vjp.ckir's local_size is the 26d-4 sentinel (resolver cook-binds it to nel)
    ce::Value* const   x   = mkd(tf(ctx, sh2(ctx, 8U, 4U)));
    ce::Value* const   gy  = mkd(tf(ctx, sh2(ctx, 8U, 4U)));
    ce::Value* const   gx  = mkd(tf(ctx, sh2(ctx, 8U, 4U)));
    // grid = arith.const{value=1} : index (one workgroup — the asset DRIVES the grid, not numel).
    ce::Operation* const grid = ctx.create_operation(ctx.intern_op("arith", "const"), {}, 1U, ctx.type_index());
    ctx.set_attr(grid, crd::containers::StringView("value"), ctx.attr_int(1));
    b->append(grid);
    // compute.dispatch(grid,grid,grid, x, gy, gx) {kernel=@relu_vjp, access="r,r,w"} — RESULTLESS (gx written THROUGH the declare).
    ce::Value*     dops[6] = {grid->result(0U), grid->result(0U), grid->result(0U), x, gy, gx};
    ce::Operation* disp    = ctx.create_operation(ctx.intern_op("compute", "dispatch"), crd::containers::ConstSpan<ce::Value*>(dops, 6U), 0U);
    ctx.set_attr(disp, crd::containers::StringView("kernel"), ctx.attr_symbol(crd::containers::StringView("relu_vjp")));
    ctx.set_attr(disp, crd::containers::StringView("access"), ctx.attr_string(crd::containers::StringView("r,r,w")));
    b->append(disp);

    // ── PLAN (device-free, ALWAYS runs): the dispatch → ONE VizDispatch stage; nbind=3 (x,gy,gx), n_out=1 (gx the trailing write =
    //    the Output/readback target). x,gy are ExternalIn; gx a WRITE-THROUGH-DECLARE (resultless dispatch → no SSA edge; 25c-1b-2). ──
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 1U);
    REQUIRE(plan.stages[0].kind == ceg::StageKind::VizDispatch);
    CHECK(plan.stages[0].nbind == 3U);
    CHECK(plan.stages[0].n_out == 1U);
    // ⛔ WRITE-THROUGH-DECLARE role check (the 25c-1b-2 baseline): x is ExternalIn (read, caller-seeded); gx — a resource.declare
    //    with NO SSA writer, distinguished ONLY by being the terminal dispatch's trailing WRITE bind — must be Output, else the
    //    readback would pick the wrong buffer class (an IDENTITY assertion, not "n_out counted it"). bind order = operand order.
    REQUIRE(plan.stages[0].bind[0] >= 0);
    REQUIRE(plan.stages[0].bind[2] >= 0);
    CHECK(plan.buffers[static_cast<crd::usize>(plan.stages[0].bind[0])].role == ceg::BufferRole::ExternalIn); // x
    CHECK(plan.buffers[static_cast<crd::usize>(plan.stages[0].bind[2])].role == ceg::BufferRole::Output);     // gx

    // ⛔ READONLY-DROP CODEGEN (independent of the device run): emit the asset's HLSL and CONFIRM there is NO `readonly` token and every
    //    buffer is a `RWByteAddressBuffer` UAV — the ledgered HLSL-masks-readonly behavior (the OPPOSITE of the Vulkan gate). A
    //    device-numeric pass alone cannot distinguish "readonly correctly dropped to UAV" from any other decl choice.
    kir::KGraph     rg(&root);
    kir::GlslKernel rk(&root);
    REQUIRE(load_emit_ckir_hlsl(CRD_REPO_DIR "/assets/ckir/relu_vjp.ckir", rg, rk, &root, nel)); // 26d-4b: cook-bind the sentinel before the codegen emit
    CHECK(std::strstr(rk.source.c_str(), "readonly") == nullptr);            // HLSL has no readonly keyword — the ledgered drop
    CHECK(std::strstr(rk.source.c_str(), "RWByteAddressBuffer") != nullptr); // x/gy (readonly in .ckir) become UAVs too

    // ── the DIALECT-INDEPENDENT reference: gx = hesap nn_reverse::relu_vjp(x, gy) = (x>0)?gy:0 (strict). f64 over the SAME f32 inputs
    //    the device is seeded with, so the cast-back is lossless (pure select, no arithmetic) ⇒ EXACT. ──
    namespace nnr = crd::hesap::autodiff::reverse::nn;
    float x_f[nel];
    float gy_f[nel];
    for (crd::u32 i = 0; i < nel; ++i)
    {
        x_f[i]  = static_cast<float>(static_cast<int>(i % 5U) - 2) * 0.5F; // -1,-0.5,0,0.5,1 cycling: neg + ZERO + pos
        gy_f[i] = 0.125F + 0.0625F * static_cast<float>(i);                // non-uniform, distinct from x, EXACT f32 (2^-3 + i*2^-4)
    }
    double x_d[nel];
    double gy_d[nel];
    double gx_ref[nel];
    for (crd::u32 i = 0; i < nel; ++i) { x_d[i] = static_cast<double>(x_f[i]); gy_d[i] = static_cast<double>(gy_f[i]); }
    nnr::relu_vjp(x_d, gy_d, gx_ref, static_cast<int>(nel));

    // ── DEVICE (soft-skip with no adapter): seed x + gy BY VALUE; read back gx (the plan's single Output). ──
    crd::gpu::Dx12ComputeContext compute(&root);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-25c-1b relu_vjp gate"); return; }

    const QuantSeed seeds[2] = {{x, x_f, nullptr, nel}, {gy, gy_f, nullptr, nel}};
    float           got[nel] = {};
    REQUIRE(run_quant_module(compute, ctx, &root, plan, seeds, 2U, gx, got, static_cast<crd::usize>(nel)));
    for (crd::u32 i = 0; i < nel; ++i) { CHECK(got[i] == static_cast<float>(gx_ref[i])); } // pure select ⇒ EXACT
}

// CEIR-25c-2 (DX12 leg) — the §138 ML PROOF crown mirror: an ml.mlp(x[8,4], W1[4,4], W2[4,4]){relu} differentiated by vjp_mlp then
// PLANNED (forward composite ERASED) and EXECUTED device-resident on DX12, reading back BOTH dW1 (Output) + dW2 (Intermediate, via the
// (b) multi-output-by-Value readback — on DX12 every dev buffer is GpuOnly + read back via a GpuToCpu copy, so no role forcing) vs a
// DIALECT-INDEPENDENT hesap reference (forward matmul/relu + backward matmul_vjp/relu_vjp composed, FD-cross-validated on <M,mlp(x,W)>).
TEST_CASE("ceir 25c-2: the vjp_mlp backward of a 2-layer MLP runs device-resident on DX12 (dW1+dW2) == hesap matmul_vjp+relu_vjp + FD",
          "[ceir][tensor-pipeline][autodiff][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::tensor::register_dialect(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::ml::register_dialect(ctx);
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };

    constexpr int    mrows   = 8;
    constexpr int    d0      = 4;
    constexpr int    d1      = 4; // hidden: m*d1 = 8*4 = 32 (the 32-width REGRESSION leg; relu.ckir/relu_vjp.ckir cook-bind local_size to it via the 26d sentinel — the non-32 h1=64 case is the 26d-4c gate below)
    constexpr int    d2      = 4;
    ce::Value* const xin     = mkd(tf(ctx, sh2(ctx, mrows, d0)));
    ce::Value* const w1      = mkd(tf(ctx, sh2(ctx, d0, d1)));
    ce::Value* const w2      = mkd(tf(ctx, sh2(ctx, d1, d2)));
    ce::Value*       mops[3] = {xin, w1, w2};
    ce::Operation* const mlp =
        ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mops, 3U), 1U, tf(ctx, sh2(ctx, mrows, d2)));
    ctx.set_attr(mlp, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
    b->append(mlp);

    ceg::VjpRegistry reg(&root);
    ceg::register_builtin_vjps(reg, ctx);
    ce::Value*            wrt[2]   = {w1, w2};
    ce::Value*            grads[2] = {nullptr, nullptr};
    const ceg::GradResult gr = ceg::build_gradient(ctx, *m, reg, mlp->result(0U), crd::containers::ConstSpan<ce::Value*>(wrt, 2U),
                                                   crd::containers::Span<ce::Value*>(grads, 2U), &root);
    REQUIRE(gr.error == ceg::GradError::None);
    REQUIRE(grads[0] != nullptr);
    REQUIRE(grads[1] != nullptr);
    REQUIRE(gr.seed != nullptr);
    REQUIRE(!mlp->result(0U)->has_uses());
    mlp->erase();
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);

    // ── data: x ALL-POSITIVE + W1 columns cleanly signed (cols 0,1 +, cols 2,3 −) ⇒ z1 cleanly ± and WELL-SEPARATED from 0 (relu
    //    BITES, cols 2,3 dead) AND the FD stays OFF the relu kink; M non-uniform. (Identical corpus to the Vulkan leg.) ──
    double xd[mrows * d0];
    double w1d[d0 * d1];
    double w2d[d1 * d2];
    double md[mrows * d2];
    for (int i = 0; i < mrows; ++i)
    {
        for (int a = 0; a < d0; ++a) { xd[i * d0 + a] = 0.5 + 0.5 * static_cast<double>((i * 3 + a) % 7) / 6.0; } // [0.5, 1.0]
    }
    for (int a = 0; a < d0; ++a)
    {
        for (int c = 0; c < d1; ++c) { w1d[a * d1 + c] = (c < 2 ? 1.0 : -1.0) * (0.3 + 0.1 * static_cast<double>(a)); }
    }
    for (int i = 0; i < d1 * d2; ++i) { w2d[i] = static_cast<double>((i * 3 + 2) % 9 - 4) * 0.2; }
    for (int i = 0; i < mrows * d2; ++i) { md[i] = 0.2 + 0.11 * static_cast<double>(i); }

    namespace nnr = crd::hesap::autodiff::reverse::nn;
    double z1[mrows * d1];
    double h1[mrows * d1];
    double z2[mrows * d2];
    nnr::matmul(xd, w1d, z1, mrows, d0, d1);
    nnr::relu(z1, h1, mrows * d1);
    nnr::matmul(h1, w2d, z2, mrows, d1, d2);
    int    neg    = 0;
    double minabs = 1e30;
    for (int i = 0; i < mrows * d1; ++i)
    {
        neg += z1[i] <= 0.0 ? 1 : 0;
        const double a = crd::math::abs(z1[i]);
        if (a < minabs) { minabs = a; }
    }
    REQUIRE(neg > 0);
    REQUIRE(neg < mrows * d1);
    REQUIRE(minabs > 0.05); // z1 bounded OFF the relu kink so the FD witness is valid
    bool m_distinct = false;
    for (int i = 0; i < mrows * d2 && !m_distinct; ++i)
    {
        for (int j = i + 1; j < mrows * d2; ++j) { if (md[i] != md[j]) { m_distinct = true; break; } }
    }
    REQUIRE(m_distinct);

    double dz2[mrows * d2];
    for (int i = 0; i < mrows * d2; ++i) { dz2[i] = md[i]; }
    double dh1[mrows * d1];
    double dw2ref[d1 * d2];
    nnr::matmul_vjp(h1, w2d, dz2, dh1, dw2ref, mrows, d1, d2);
    double dz1[mrows * d1];
    nnr::relu_vjp(z1, dh1, dz1, mrows * d1);
    double dxref[mrows * d0];
    double dw1ref[d0 * d1];
    nnr::matmul_vjp(xd, w1d, dz1, dxref, dw1ref, mrows, d0, d1);

    // ⛔ half-dead hidden ⇒ PROVEN zero blocks: dW1[:,c>=2]==0, dW2[c>=2,:]==0; the c<2 blocks meaningfully nonzero.
    for (int a = 0; a < d0; ++a)
    {
        for (int c = 0; c < d1; ++c)
        {
            if (c >= 2) { CHECK(dw1ref[a * d1 + c] == 0.0); }
            else { CHECK(crd::math::abs(dw1ref[a * d1 + c]) > 0.05); }
        }
    }
    for (int c = 0; c < d1; ++c)
    {
        for (int e = 0; e < d2; ++e)
        {
            if (c >= 2) { CHECK(dw2ref[c * d2 + e] == 0.0); }
            else { CHECK(crd::math::abs(dw2ref[c * d2 + e]) > 0.05); }
        }
    }

    constexpr int nw = d0 * d1 + d1 * d2;
    double        wflat[nw];
    for (int i = 0; i < d0 * d1; ++i) { wflat[i] = w1d[i]; }
    for (int i = 0; i < d1 * d2; ++i) { wflat[d0 * d1 + i] = w2d[i]; }
    double gfd[nw];
    crd::hesap::autodiff::testing::grad_fd<nw>(MlpLoss{xd, md, mrows, d0, d1, d2}, wflat, gfd);
    for (int i = 0; i < d0 * d1; ++i) { CHECK(crd::math::abs(gfd[i] - dw1ref[i]) <= 1e-5 * (1.0 + crd::math::abs(dw1ref[i]))); }
    for (int i = 0; i < d1 * d2; ++i) { CHECK(crd::math::abs(gfd[d0 * d1 + i] - dw2ref[i]) <= 1e-5 * (1.0 + crd::math::abs(dw2ref[i]))); }

    // ── DEVICE (soft-skip with no adapter): seed x, W1, W2, dLoss=M BY VALUE; read back BOTH dW1 + dW2 (via run_quant_module_n). ──
    crd::gpu::Dx12ComputeContext compute(&root);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-25c-2 MLP backward gate"); return; }

    float xf[mrows * d0];
    float w1f[d0 * d1];
    float w2f[d1 * d2];
    float mf[mrows * d2];
    for (int i = 0; i < mrows * d0; ++i) { xf[i] = static_cast<float>(xd[i]); }
    for (int i = 0; i < d0 * d1; ++i) { w1f[i] = static_cast<float>(w1d[i]); }
    for (int i = 0; i < d1 * d2; ++i) { w2f[i] = static_cast<float>(w2d[i]); }
    for (int i = 0; i < mrows * d2; ++i) { mf[i] = static_cast<float>(md[i]); }
    const QuantSeed seeds[4]        = {{xin, xf, nullptr, mrows * d0}, {w1, w1f, nullptr, d0 * d1},
                                       {w2, w2f, nullptr, d1 * d2}, {gr.seed, mf, nullptr, mrows * d2}};
    float           dw1got[d0 * d1] = {};
    float           dw2got[d1 * d2] = {};
    const QuantOut  outs[2]         = {{grads[0], dw1got, static_cast<crd::usize>(d0 * d1)},
                                       {grads[1], dw2got, static_cast<crd::usize>(d1 * d2)}};
    REQUIRE(run_quant_module_n(compute, ctx, &root, plan, seeds, 4U, outs, 2U));
    for (int i = 0; i < d0 * d1; ++i) { CHECK(crd::math::abs(static_cast<double>(dw1got[i]) - dw1ref[i]) <= 1e-4 * (1.0 + crd::math::abs(dw1ref[i]))); }
    for (int i = 0; i < d1 * d2; ++i) { CHECK(crd::math::abs(static_cast<double>(dw2got[i]) - dw2ref[i]) <= 1e-4 * (1.0 + crd::math::abs(dw2ref[i]))); }

    // ── CEIR-26a-3 DCE DIFFERENTIAL (leg (b), DX12 mirror of the Vulkan leg): for a SEMANTICS-PRESERVING pass the reference is the RAW
    //    program's OWN device output, BIT-EXACT (not hesap-within-tol). PIN the readback gradients (func.return roots them — readback-by-
    //    Value, NOT SSA-consumed; the planner SKIPS func.return so the plan is unchanged), dce_run (11→9 stages), re-plan + re-execute the
    //    SAME corpus/SAME device/SAME seeds → dce'd == raw for every element. run_quant_module_n reads plan.buffers fresh + fresh rb per
    //    call (no buffer-count / stage-index caching), so the 9-stage plan executes cleanly. See dce.hpp LIVENESS CONTRACT. ──
    ce::Value* const rets[2] = {grads[0], grads[1]};
    b->append(ce::func::create_return(ctx, crd::containers::ConstSpan<ce::Value*>(rets, 2U)));
    REQUIRE(grads[0]->has_uses()); // the pin is present (its ABSENCE is the 26a-2 device-free NEGATIVE gate: unpinned ⇒ DELETED)
    ce::DiagnosticEngine diag(ctx, &root);
    REQUIRE(ce::dce_run(ctx, *m, diag)); // DCE fired
    const ceg::TensorPipelinePlan plan_dce = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan_dce.reject == ceg::PlanReject::None);
    REQUIRE(plan_dce.stages.size() == 9U); // 11→9 on THIS corpus (dx gemm + W1ᵀ transpose pruned) — proves DCE ACTED, not just ran
    float          dw1got_dce[d0 * d1] = {};
    float          dw2got_dce[d1 * d2] = {};
    const QuantOut outs_dce[2]         = {{grads[0], dw1got_dce, static_cast<crd::usize>(d0 * d1)},
                                          {grads[1], dw2got_dce, static_cast<crd::usize>(d1 * d2)}};
    REQUIRE(run_quant_module_n(compute, ctx, &root, plan_dce, seeds, 4U, outs_dce, 2U)); // SAME seeds — every input Value survives DCE
    for (int i = 0; i < d0 * d1; ++i) { CHECK(dw1got_dce[i] == dw1got[i]); } // BIT-EXACT vs the raw device output — DCE touched nothing observable
    for (int i = 0; i < d1 * d2; ++i) { CHECK(dw2got_dce[i] == dw2got[i]); }
    for (int i = 0; i < d0 * d1; ++i) { CHECK(crd::math::abs(static_cast<double>(dw1got_dce[i]) - dw1ref[i]) <= 1e-4 * (1.0 + crd::math::abs(dw1ref[i]))); }
    for (int i = 0; i < d1 * d2; ++i) { CHECK(crd::math::abs(static_cast<double>(dw2got_dce[i]) - dw2ref[i]) <= 1e-4 * (1.0 + crd::math::abs(dw2ref[i]))); }
}

// CEIR-26d-4c (DX12 leg) — the vjp shape-specialization PROVING gate MIRROR: the SAME vjp_mlp backward as the Vulkan 26d-4b but on
// D3D12 at a NON-32 interior width — ml.mlp(x[8,4], W1[4,8], W2[8,4]){relu}, interior z1 = [8,8] = 64 (M·hidden = 64 ≠ 32, previously
// MlpBakedShapeUnsupported). relu.ckir + relu_vjp.ckir cook-bind local_size via the 26d sentinel (the mechanism rode the shared lib
// from 26d-4b, ZERO new code). Reads back BOTH dW1+dW2 == the hesap reference (matmul_vjp+relu_vjp), FD-cross-validated; the column-
// signed W1 makes half the hidden units DEAD ⇒ PROVEN zero blocks. The [[migrated=both backends]] mandate. ⛔ NO "raw" for a
// previously-REJECTED program. ⛔ exp-free — f32 chained matmul tol.
TEST_CASE("ceir 26d-4c: the vjp_mlp backward at a NON-32 interior width (h1=64) runs device-resident on DX12 (dW1+dW2) == hesap",
          "[ceir][tensor-pipeline][autodiff][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::tensor::register_dialect(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::ml::register_dialect(ctx);
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };

    constexpr int    mrows = 8;
    constexpr int    d0    = 4;
    constexpr int    d1    = 8; // hidden: m*d1 = 8*8 = 64 ≠ 32 — was MlpBakedShapeUnsupported, now cook-bound
    constexpr int    d2    = 4;
    constexpr int    dhalf = d1 / 2; // W1 cols [0,dhalf) positive (alive), [dhalf,d1) negative (dead)
    ce::Value* const xin   = mkd(tf(ctx, sh2(ctx, mrows, d0)));
    ce::Value* const w1    = mkd(tf(ctx, sh2(ctx, d0, d1)));
    ce::Value* const w2    = mkd(tf(ctx, sh2(ctx, d1, d2)));
    ce::Value*       mops[3] = {xin, w1, w2};
    ce::Operation* const mlp =
        ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mops, 3U), 1U, tf(ctx, sh2(ctx, mrows, d2)));
    ctx.set_attr(mlp, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
    b->append(mlp);

    ceg::VjpRegistry reg(&root);
    ceg::register_builtin_vjps(reg, ctx);
    ce::Value*            wrt[2]   = {w1, w2};
    ce::Value*            grads[2] = {nullptr, nullptr};
    const ceg::GradResult gr = ceg::build_gradient(ctx, *m, reg, mlp->result(0U), crd::containers::ConstSpan<ce::Value*>(wrt, 2U),
                                                   crd::containers::Span<ce::Value*>(grads, 2U), &root);
    REQUIRE(gr.error == ceg::GradError::None); // ⛔ 26d-4c: was MlpBakedShapeUnsupported at h1=64 before the grad pre-check retired
    REQUIRE(grads[0] != nullptr);
    REQUIRE(grads[1] != nullptr);
    REQUIRE(gr.seed != nullptr);
    REQUIRE(!mlp->result(0U)->has_uses());
    mlp->erase();
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);

    // ── data: x ALL-POSITIVE; W1 cols [0,dhalf) +, [dhalf,d1) − ⇒ z1 cleanly signed, off the kink ⇒ relu BITES + FD valid. M non-uniform. ──
    double xd[mrows * d0];
    double w1d[d0 * d1];
    double w2d[d1 * d2];
    double md[mrows * d2];
    for (int i = 0; i < mrows; ++i)
    {
        for (int a = 0; a < d0; ++a) { xd[i * d0 + a] = 0.5 + 0.5 * static_cast<double>((i * 3 + a) % 7) / 6.0; } // [0.5, 1.0]
    }
    for (int a = 0; a < d0; ++a)
    {
        for (int c = 0; c < d1; ++c) { w1d[a * d1 + c] = (c < dhalf ? 1.0 : -1.0) * (0.3 + 0.1 * static_cast<double>(a)); }
    }
    for (int i = 0; i < d1 * d2; ++i) { w2d[i] = static_cast<double>((i * 3 + 2) % 9 - 4) * 0.2; }
    for (int i = 0; i < mrows * d2; ++i) { md[i] = 0.2 + 0.11 * static_cast<double>(i); }

    namespace nnr = crd::hesap::autodiff::reverse::nn;
    double z1[mrows * d1];
    double h1[mrows * d1];
    double z2[mrows * d2];
    nnr::matmul(xd, w1d, z1, mrows, d0, d1);
    nnr::relu(z1, h1, mrows * d1);
    nnr::matmul(h1, w2d, z2, mrows, d1, d2);
    int    neg    = 0;
    double minabs = 1e30;
    for (int i = 0; i < mrows * d1; ++i)
    {
        neg += z1[i] <= 0.0 ? 1 : 0;
        const double a = crd::math::abs(z1[i]);
        if (a < minabs) { minabs = a; }
    }
    REQUIRE(neg > 0);
    REQUIRE(neg < mrows * d1);
    REQUIRE(minabs > 0.05);

    double dz2[mrows * d2];
    for (int i = 0; i < mrows * d2; ++i) { dz2[i] = md[i]; }
    double dh1[mrows * d1];
    double dw2ref[d1 * d2];
    nnr::matmul_vjp(h1, w2d, dz2, dh1, dw2ref, mrows, d1, d2);
    double dz1[mrows * d1];
    nnr::relu_vjp(z1, dh1, dz1, mrows * d1);
    double dxref[mrows * d0];
    double dw1ref[d0 * d1];
    nnr::matmul_vjp(xd, w1d, dz1, dxref, dw1ref, mrows, d0, d1);

    // ⛔ half-dead hidden ⇒ PROVEN zero blocks: dW1[:,c>=dhalf]==0, dW2[c>=dhalf,:]==0; the c<dhalf blocks meaningfully nonzero.
    for (int a = 0; a < d0; ++a)
    {
        for (int c = 0; c < d1; ++c)
        {
            if (c >= dhalf) { CHECK(dw1ref[a * d1 + c] == 0.0); }
            else { CHECK(crd::math::abs(dw1ref[a * d1 + c]) > 0.05); }
        }
    }
    for (int c = 0; c < d1; ++c)
    {
        for (int e = 0; e < d2; ++e)
        {
            if (c >= dhalf) { CHECK(dw2ref[c * d2 + e] == 0.0); }
            else { CHECK(crd::math::abs(dw2ref[c * d2 + e]) > 0.05); }
        }
    }

    constexpr int nw = d0 * d1 + d1 * d2;
    double        wflat[nw];
    for (int i = 0; i < d0 * d1; ++i) { wflat[i] = w1d[i]; }
    for (int i = 0; i < d1 * d2; ++i) { wflat[d0 * d1 + i] = w2d[i]; }
    double gfd[nw];
    crd::hesap::autodiff::testing::grad_fd<nw>(MlpLoss{xd, md, mrows, d0, d1, d2}, wflat, gfd);
    for (int i = 0; i < d0 * d1; ++i) { CHECK(crd::math::abs(gfd[i] - dw1ref[i]) <= 1e-5 * (1.0 + crd::math::abs(dw1ref[i]))); }
    for (int i = 0; i < d1 * d2; ++i) { CHECK(crd::math::abs(gfd[d0 * d1 + i] - dw2ref[i]) <= 1e-5 * (1.0 + crd::math::abs(dw2ref[i]))); }

    // ── DEVICE (soft-skip with no adapter): seed x, W1, W2, dLoss=M BY VALUE; read back BOTH dW1 + dW2. ──
    crd::gpu::Dx12ComputeContext compute(&root);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-26d-4c non-32 vjp gate"); return; }

    float xf[mrows * d0];
    float w1f[d0 * d1];
    float w2f[d1 * d2];
    float mf[mrows * d2];
    for (int i = 0; i < mrows * d0; ++i) { xf[i] = static_cast<float>(xd[i]); }
    for (int i = 0; i < d0 * d1; ++i) { w1f[i] = static_cast<float>(w1d[i]); }
    for (int i = 0; i < d1 * d2; ++i) { w2f[i] = static_cast<float>(w2d[i]); }
    for (int i = 0; i < mrows * d2; ++i) { mf[i] = static_cast<float>(md[i]); }
    const QuantSeed seeds[4]        = {{xin, xf, nullptr, mrows * d0}, {w1, w1f, nullptr, d0 * d1},
                                       {w2, w2f, nullptr, d1 * d2}, {gr.seed, mf, nullptr, mrows * d2}};
    float           dw1got[d0 * d1] = {};
    float           dw2got[d1 * d2] = {};
    const QuantOut  outs[2]         = {{grads[0], dw1got, static_cast<crd::usize>(d0 * d1)},
                                       {grads[1], dw2got, static_cast<crd::usize>(d1 * d2)}};
    REQUIRE(run_quant_module_n(compute, ctx, &root, plan, seeds, 4U, outs, 2U));
    for (int i = 0; i < d0 * d1; ++i) { CHECK(crd::math::abs(static_cast<double>(dw1got[i]) - dw1ref[i]) <= 1e-4 * (1.0 + crd::math::abs(dw1ref[i]))); }
    for (int i = 0; i < d1 * d2; ++i) { CHECK(crd::math::abs(static_cast<double>(dw2got[i]) - dw2ref[i]) <= 1e-4 * (1.0 + crd::math::abs(dw2ref[i]))); }
}

// CEIR-26b-2c (DX12) — the DirectX-12 leg of the canonicalize reshape-fold BIT-EXACT differential (26b-2 -> COMPLETE). Mirrors
// the Vulkan 26b-2b: a sec-137 gemm[8,8]->reshape[2,32]->reshape[64]->fft->reduce(sum) pipeline (the DOUBLE reshape rooted at the
// GEMM output: T_out=[64] != T_in=[8,8], so the fold fires WITHOUT the identity exclusion) runs RAW on a real D3D12 device, then
// canonicalize+dce collapse it to the single-reshape pipeline, re-runs, and the reduce scalar is BIT-EXACT vs the raw one, using
// the DX12 portable dev/up/rb materialization (run_quant_module_n zero-fills fft twiddles, so this reuses the 22c-3e pattern).
TEST_CASE("ceir 26b-2: canonicalize reshape-fold is BIT-EXACT vs the RAW pipeline on DX12 (sec-137)", "[ceir][tensor-pipeline][gpu]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ce::Context                        ctx(&alloc);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::tensor::register_dialect(ctx);
    REQUIRE(ctx.intern_op("tensor", "reshape") == ce::OpId{ce::fnv1a_ct("tensor.reshape")}); // pins the fold's capture-free match id

    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
    ce::Value* const a_in = mkd(tf(ctx, sh2(ctx, kSide, kSide)));
    ce::Value* const b_in = mkd(tf(ctx, sh2(ctx, kSide, kSide)));
    ce::Value* const c_in = mkd(tf(ctx, sh2(ctx, kSide, kSide)));
    ce::Operation* const gm = ce::linalg::build_gemm(ctx, a_in, b_in, c_in, ctx.attr_float(1.0), ctx.attr_float(0.0),
                                                     ctx.attr_bool(false), ctx.attr_bool(false), tf(ctx, sh2(ctx, kSide, kSide)));
    b->append(gm);
    // the DOUBLE reshape rooted at the GEMM output: [8,8] -> [2,32] -> [64] (T_out=[64] != T_in [8,8] -> fold fires, no identity).
    ce::Operation* const r_in = ce::tensor::build_reshape(ctx, gm->result(0U), tf(ctx, sh2(ctx, 2U, static_cast<crd::u32>(kL / 2))));
    b->append(r_in);
    ce::Operation* const r_out = ce::tensor::build_reshape(ctx, r_in->result(0U), tf(ctx, sh1(ctx, kL)));
    b->append(r_out);
    ce::Value* const     im0    = mkd(tf(ctx, sh1(ctx, kL)));
    ce::Value*           ffo[2] = {r_out->result(0U), im0};
    ce::Operation* const ff = ctx.create_operation(ctx.intern_op("tensor", "fft"), crd::containers::ConstSpan<ce::Value*>(ffo, 2U),
                                                   2U, tf(ctx, sh1(ctx, kL)));
    ctx.set_attr(ff, crd::containers::StringView("direction"), ctx.attr_string(crd::containers::StringView("forward")));
    ctx.set_attr(ff, crd::containers::StringView("axis"), ctx.attr_int(0));
    b->append(ff);
    ce::Operation* const rd = ce::tensor::build_reduce(ctx, ff->result(0U), ctx.attr_int(0),
                                                       ctx.attr_string(crd::containers::StringView("sum")),
                                                       tf(ctx, shp(ctx, crd::containers::ConstSpan<ce::TypeId>{})));
    b->append(rd);
    // PIN the terminal reduce output (26a-2 liveness contract) so DCE keeps the reduce->reshape chain; the planner skips func.return.
    ce::Value* const rets[1] = {rd->result(0U)};
    b->append(ce::func::create_return(ctx, crd::containers::ConstSpan<ce::Value*>(rets, 1U)));

    // a/b data + the INDEPENDENT composed ref (triple-loop GEMM -> naive DFT -> serial sum, f64).
    float a_data[kL];
    float b_data[kL];
    for (int i = 0; i < kSide; ++i)
    {
        for (int j = 0; j < kSide; ++j)
        {
            a_data[i * kSide + j] = static_cast<float>(((i + j) % 7) - 3);
            b_data[i * kSide + j] = static_cast<float>(((i * 2 + j) % 5) - 2);
        }
    }
    crd::f64 dref[kL];
    for (int i = 0; i < kSide; ++i)
    {
        for (int j = 0; j < kSide; ++j)
        {
            crd::f64 acc = 0.0;
            for (int k = 0; k < kSide; ++k) { acc += static_cast<crd::f64>(a_data[i * kSide + k]) * static_cast<crd::f64>(b_data[k * kSide + j]); }
            dref[i * kSide + j] = acc;
        }
    }
    constexpr crd::f64 two_pi = 6.28318530717958647693;
    crd::f64           s_ref  = 0.0;
    crd::f64           maxmag = 1e-6;
    for (int kk = 0; kk < kL; ++kk)
    {
        crd::f64 fr = 0.0;
        for (int l = 0; l < kL; ++l) { fr += dref[l] * crd::math::cos(two_pi * static_cast<crd::f64>(kk) * static_cast<crd::f64>(l) / static_cast<crd::f64>(kL)); }
        s_ref += fr;
        const crd::f64 am = fr < 0.0 ? -fr : fr;
        maxmag            = maxmag > am ? maxmag : am;
    }

    // device soft-skip.
    crd::gpu::Dx12ComputeContext compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device -- skipping the CEIR-26b-2c reshape-fold differential"); return; }

    // the runner: the DX12 portable dev/up/rb materialization (Alias shares dev; ExternalIn uploaded a/b + fft twiddles; the
    // Output read back via a dedicated rb copy), execute as ONE submit, return the reduce scalar.
    const auto run = [&](const ceg::TensorPipelinePlan& plan) -> float {
        namespace g = crd::gpu;
        using g::compute_usage::storage;
        using g::compute_usage::transfer_dst;
        using g::compute_usage::transfer_src;
        const crd::usize nb = plan.buffers.size();
        REQUIRE(nb <= 40U);
        crd::i32 twr = -1;
        crd::i32 twi = -1;
        int      fftn = 0;
        for (crd::usize s = 0; s < plan.stages.size(); ++s)
        {
            if (plan.stages[s].kind != ceg::StageKind::Fft) { continue; }
            twr  = plan.stages[s].bind[2];
            twi  = plan.stages[s].bind[3];
            fftn = static_cast<int>(plan.buffers[static_cast<crd::usize>(twr)].bytes / 4ULL) * 2;
        }
        crd::i32 out_idx = -1;
        int      n_out   = 0;
        for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].role == ceg::BufferRole::Output) { out_idx = static_cast<crd::i32>(i); ++n_out; } }
        REQUIRE(out_idx >= 0);
        REQUIRE(n_out == 1); // exactly ONE Output (the rank-0 reduce) — a 2nd would make bit-exact compare the wrong scalar to itself
        std::unique_ptr<g::ComputeBuffer> dev[40];
        std::unique_ptr<g::ComputeBuffer> up[40];
        g::ComputeBuffer*                 bufs[40] = {};
        for (crd::usize i = 0; i < nb; ++i)
        {
            const ceg::PlanBuffer& pb = plan.buffers[i];
            if (pb.role == ceg::BufferRole::Alias) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
            dev[i] = compute.create_buffer(pb.bytes, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
            REQUIRE(dev[i] != nullptr);
            bufs[i] = dev[i].get();
            if (pb.role != ceg::BufferRole::ExternalIn) { continue; }
            up[i] = compute.create_buffer(pb.bytes, transfer_src, g::ComputeMemory::CpuToGpu);
            REQUIRE(up[i] != nullptr);
            auto*          dst = static_cast<float*>(up[i]->map());
            const crd::u64 cnt = pb.bytes / 4ULL;
            if (pb.value == a_in) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = a_data[e]; } }
            else if (pb.value == b_in) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = b_data[e]; } }
            else if (static_cast<crd::i32>(i) == twr) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = static_cast<float>(crd::math::cos(two_pi * static_cast<crd::f64>(e) / static_cast<crd::f64>(fftn))); } }
            else if (static_cast<crd::i32>(i) == twi) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = static_cast<float>(-crd::math::sin(two_pi * static_cast<crd::f64>(e) / static_cast<crd::f64>(fftn))); } }
            else { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = 0.0F; } }
            up[i]->unmap();
        }
        auto rb = compute.create_buffer(plan.buffers[static_cast<crd::usize>(out_idx)].bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
        REQUIRE(rb != nullptr);
        Resolver res;
        res.alloc_ctx = &ctx;
        res.alloc     = &alloc;
        res.compute   = &compute;
        auto& rec = compute.begin();
        for (crd::usize i = 0; i < nb; ++i) { if (up[i] != nullptr) { rec.copy(*up[i], *dev[i], 0U, 0U, plan.buffers[i].bytes); } }
        for (crd::usize i = 0; i < nb; ++i) { if (up[i] != nullptr) { rec.barrier(*dev[i], g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead); } }
        const ceg::ExecuteError ee = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res,
                                                                  crd::containers::ConstSpan<g::ComputeBuffer*>(bufs, nb));
        REQUIRE(ee == ceg::ExecuteError::None);
        rec.barrier(*dev[static_cast<crd::usize>(out_idx)], g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
        rec.copy(*dev[static_cast<crd::usize>(out_idx)], *rb, 0U, 0U, plan.buffers[static_cast<crd::usize>(out_idx)].bytes);
        compute.submit_and_wait();
        const auto* got = static_cast<const float*>(rb->map());
        REQUIRE(got != nullptr);
        const float v = got[0];
        rb->unmap();
        return v;
    };

    // RAW: 3 stages (Gemm, Fft, Reduce; the TWO reshapes are aliases), 2 Alias buffers.
    const ceg::TensorPipelinePlan raw_plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc);
    REQUIRE(raw_plan.reject == ceg::PlanReject::None);
    REQUIRE(raw_plan.stages.size() == 3U);
    int raw_alias = 0;
    for (crd::usize i = 0; i < raw_plan.buffers.size(); ++i)
    {
        if (raw_plan.buffers[i].role != ceg::BufferRole::Alias) { continue; }
        ++raw_alias;
        // the runner's one-level bufs[i]=bufs[alias_of] resolves the r_out->r_in->gemm chain transitively ONLY because plan
        // buffers are walk-ordered producer-first (alias_of < i). GATE it: a reorder would silently null-deref the raw run.
        REQUIRE(raw_plan.buffers[i].alias_of >= 0);
        REQUIRE(static_cast<crd::usize>(raw_plan.buffers[i].alias_of) < i);
    }
    REQUIRE(raw_alias == 2);
    const float raw_s = run(raw_plan);

    // canonicalize (fold reshape-of-reshape) + dce (reclaim the dead reshapes).
    ce::DiagnosticEngine diag(ctx, &alloc);
    REQUIRE(ce::canonicalize_run(ctx, *m, diag));
    REQUIRE(ce::dce_run(ctx, *m, diag));
    REQUIRE_FALSE(diag.has_fatal());

    // OPT: SAME 3 stages (0 stage shift -- reshape is an alias), now 1 Alias buffer.
    const ceg::TensorPipelinePlan opt_plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc);
    REQUIRE(opt_plan.reject == ceg::PlanReject::None);
    REQUIRE(opt_plan.stages.size() == 3U);
    int opt_alias = 0;
    for (crd::usize i = 0; i < opt_plan.buffers.size(); ++i) { if (opt_plan.buffers[i].role == ceg::BufferRole::Alias) { ++opt_alias; } }
    CHECK(opt_alias == 1); // 2 -> 1
    const float opt_s = run(opt_plan);

    // BIT-EXACT: the fold is semantics-preserving -- the collapsed pipeline reads the SAME physical gemm buffer (via the alias),
    // so the fft sees identical bytes and the reduce scalar is bit-identical (the 26a differential-test standard). ⛔ as on the
    // Vulkan leg, this witness proves alias-WIRING preservation NOT kernel correctness (the 3 dispatches are identical); a
    // dispatched-stage fold (26e fusion) is where bit-exact regains its 26a teeth.
    CHECK(opt_s == raw_s);
    // and both are CORRECT (not merely self-consistent) vs the independent composed ref within the propagated tol.
    const float tol = static_cast<float>(static_cast<crd::f64>(kL) * 2e-3 * maxmag);
    const float fa  = raw_s - static_cast<float>(s_ref);
    CHECK((fa < 0.0F ? -fa : fa) <= tol);
}

// CEIR-26c-2c (DX12) — the DirectX-12 leg of the CSE duplicate-gemm-removal BIT-EXACT differential (26c-2 -> COMPLETE). Mirrors
// the Vulkan 26c-2b: two IDENTICAL gemm(A,B,C):[8,8], each reshaped [8,8]->[64] (alias) then FULL-reduced to a scalar by two
// DISTINCT consumers (max = a SELECTION/wiring witness; sum = the REWIRED consumer, an ACCUMULATION carrying the bit-exact teeth).
// cse_run removes one gemm DISPATCH; both reduce scalars are BIT-EXACT raw==opt + correct vs an f64 ref. This is the 26a witness
// at FULL teeth on a REMOVED DISPATCH. ⛔ the resource.declare Allocate-effect fix (26c-2b) rides the shared crd-ceir lib, so the
// declares are NOT merged here (a Pure declare would collapse A/B/C -> gemm A@A). DX12 readback = a FRESH rb + copy PER reduce
// output (the planner marks only ONE terminal Output — the 26a-2 g0/g1 finding — so each output gets its own rb).
TEST_CASE("ceir 26c-2: CSE duplicate-gemm removal is BIT-EXACT vs the RAW pipeline on DX12 (sec-137)", "[ceir][tensor-pipeline][gpu]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ce::Context                        ctx(&alloc);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::tensor::register_dialect(ctx);

    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
    ce::Value* const a_in = mkd(tf(ctx, sh2(ctx, kSide, kSide)));
    ce::Value* const b_in = mkd(tf(ctx, sh2(ctx, kSide, kSide)));
    ce::Value* const c_in = mkd(tf(ctx, sh2(ctx, kSide, kSide)));
    const auto       mkgemm = [&]() {
        ce::Operation* const gg = ce::linalg::build_gemm(ctx, a_in, b_in, c_in, ctx.attr_float(1.0), ctx.attr_float(0.0),
                                                         ctx.attr_bool(false), ctx.attr_bool(false), tf(ctx, sh2(ctx, kSide, kSide)));
        b->append(gg);
        return gg;
    };
    ce::Operation* const g1  = mkgemm();
    ce::Operation* const g2  = mkgemm();
    ce::Operation* const rs1 = ce::tensor::build_reshape(ctx, g1->result(0U), tf(ctx, sh1(ctx, kL)));
    b->append(rs1);
    ce::Operation* const rs2 = ce::tensor::build_reshape(ctx, g2->result(0U), tf(ctx, sh1(ctx, kL)));
    b->append(rs2);
    const ce::TypeId     scalar = tf(ctx, shp(ctx, crd::containers::ConstSpan<ce::TypeId>{}));
    ce::Operation* const ra     = ce::tensor::build_reduce(ctx, rs1->result(0U), ctx.attr_int(0),
                                                           ctx.attr_string(crd::containers::StringView("max")), scalar);
    b->append(ra);
    ce::Operation* const rb = ce::tensor::build_reduce(ctx, rs2->result(0U), ctx.attr_int(0),
                                                       ctx.attr_string(crd::containers::StringView("sum")), scalar);
    b->append(rb);
    ce::Value* const rets[2] = {ra->result(0U), rb->result(0U)};
    b->append(ce::func::create_return(ctx, crd::containers::ConstSpan<ce::Value*>(rets, 2U)));
    REQUIRE(ce::linalg::find_linalg_misuse(ctx, *m).kind == ce::linalg::LinalgMisuseKind::None);
    REQUIRE(ce::tensor::find_tensor_misuse(ctx, *m).kind == ce::tensor::TensorMisuseKind::None);

    // a/b data + the INDEPENDENT f64 ref (full reduction over all 64 gemm outputs).
    float a_data[kL];
    float b_data[kL];
    for (int i = 0; i < kSide; ++i)
    {
        for (int j = 0; j < kSide; ++j)
        {
            a_data[i * kSide + j] = static_cast<float>(((i + j) % 7) - 3);
            b_data[i * kSide + j] = static_cast<float>(((i * 2 + j) % 5) - 2);
        }
    }
    crd::f64 max_ref = -1e300;
    crd::f64 sum_ref = 0.0;
    for (int i = 0; i < kSide; ++i)
    {
        for (int j = 0; j < kSide; ++j)
        {
            crd::f64 acc = 0.0;
            for (int kk = 0; kk < kSide; ++kk) { acc += static_cast<crd::f64>(a_data[i * kSide + kk]) * static_cast<crd::f64>(b_data[kk * kSide + j]); }
            max_ref = max_ref > acc ? max_ref : acc;
            sum_ref += acc;
        }
    }
    const crd::f64 sum_abs = sum_ref < 0.0 ? -sum_ref : sum_ref;
    const crd::f64 maxabs  = sum_abs > 1e-6 ? sum_abs : 1e-6;

    // device soft-skip.
    crd::gpu::Dx12ComputeContext compute(&alloc);
    if (!compute.valid()) { WARN("no D3D12 device -- skipping the CEIR-26c-2c duplicate-gemm differential"); return; }

    // the runner: DX12 portable dev/up/rb; both reduce outputs read back via a FRESH rb + copy each (BY OP IDENTITY ra=max, rb=sum).
    const auto run = [&](const ceg::TensorPipelinePlan& plan, float* out_max, float* out_sum) {
        namespace g = crd::gpu;
        using g::compute_usage::storage;
        using g::compute_usage::transfer_dst;
        using g::compute_usage::transfer_src;
        const crd::usize nb = plan.buffers.size();
        REQUIRE(nb <= 40U);
        crd::i32 max_idx = -1;
        crd::i32 sum_idx = -1;
        for (crd::usize s = 0; s < plan.stages.size(); ++s)
        {
            if (plan.stages[s].kind != ceg::StageKind::Reduce) { continue; }
            if (plan.stages[s].op == ra) { max_idx = plan.stages[s].bind[1]; }
            else if (plan.stages[s].op == rb) { sum_idx = plan.stages[s].bind[1]; }
        }
        REQUIRE(max_idx >= 0);
        REQUIRE(sum_idx >= 0);
        REQUIRE(max_idx != sum_idx);
        std::unique_ptr<g::ComputeBuffer> dev[40];
        std::unique_ptr<g::ComputeBuffer> up[40];
        g::ComputeBuffer*                 bufs[40] = {};
        for (crd::usize i = 0; i < nb; ++i)
        {
            const ceg::PlanBuffer& pb = plan.buffers[i];
            if (pb.role == ceg::BufferRole::Alias) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
            dev[i] = compute.create_buffer(pb.bytes, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
            REQUIRE(dev[i] != nullptr);
            bufs[i] = dev[i].get();
            if (pb.role != ceg::BufferRole::ExternalIn) { continue; }
            up[i] = compute.create_buffer(pb.bytes, transfer_src, g::ComputeMemory::CpuToGpu);
            REQUIRE(up[i] != nullptr);
            auto*          dst = static_cast<float*>(up[i]->map());
            const crd::u64 cnt = pb.bytes / 4ULL;
            if (pb.value == a_in) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = a_data[e]; } }
            else if (pb.value == b_in) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = b_data[e]; } }
            else { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = 0.0F; } }
            up[i]->unmap();
        }
        auto rb_max = compute.create_buffer(plan.buffers[static_cast<crd::usize>(max_idx)].bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
        auto rb_sum = compute.create_buffer(plan.buffers[static_cast<crd::usize>(sum_idx)].bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
        REQUIRE(rb_max != nullptr);
        REQUIRE(rb_sum != nullptr);
        Resolver res;
        res.alloc_ctx = &ctx;
        res.alloc     = &alloc;
        res.compute   = &compute;
        auto& rec = compute.begin();
        for (crd::usize i = 0; i < nb; ++i) { if (up[i] != nullptr) { rec.copy(*up[i], *dev[i], 0U, 0U, plan.buffers[i].bytes); } }
        for (crd::usize i = 0; i < nb; ++i) { if (up[i] != nullptr) { rec.barrier(*dev[i], g::ComputeAccess::TransferDst, g::ComputeAccess::ShaderRead); } }
        const ceg::ExecuteError ee = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res,
                                                                  crd::containers::ConstSpan<g::ComputeBuffer*>(bufs, nb));
        REQUIRE(ee == ceg::ExecuteError::None);
        rec.barrier(*dev[static_cast<crd::usize>(max_idx)], g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
        rec.barrier(*dev[static_cast<crd::usize>(sum_idx)], g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
        rec.copy(*dev[static_cast<crd::usize>(max_idx)], *rb_max, 0U, 0U, plan.buffers[static_cast<crd::usize>(max_idx)].bytes);
        rec.copy(*dev[static_cast<crd::usize>(sum_idx)], *rb_sum, 0U, 0U, plan.buffers[static_cast<crd::usize>(sum_idx)].bytes);
        compute.submit_and_wait();
        const auto* gm = static_cast<const float*>(rb_max->map());
        REQUIRE(gm != nullptr);
        *out_max = gm[0];
        rb_max->unmap();
        const auto* gs = static_cast<const float*>(rb_sum->map());
        REQUIRE(gs != nullptr);
        *out_sum = gs[0];
        rb_sum->unmap();
    };

    // RAW: 2 Gemm + 2 Reduce (the two gemms write DIFFERENT buffers — the planner did NOT dedupe; CSE has real work).
    const ceg::TensorPipelinePlan raw_plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc);
    REQUIRE(raw_plan.reject == ceg::PlanReject::None);
    REQUIRE(raw_plan.stages.size() == 4U);
    crd::i32 raw_g_a = -1;
    crd::i32 raw_g_b = -1;
    for (crd::usize s = 0; s < raw_plan.stages.size(); ++s)
    {
        if (raw_plan.stages[s].kind != ceg::StageKind::Gemm) { continue; }
        if (raw_g_a < 0) { raw_g_a = raw_plan.stages[s].bind[2]; }
        else { raw_g_b = raw_plan.stages[s].bind[2]; }
    }
    REQUIRE(raw_g_a >= 0);
    REQUIRE(raw_g_b >= 0);
    REQUIRE(raw_g_a != raw_g_b);
    for (crd::usize i = 0; i < raw_plan.buffers.size(); ++i)
    {
        if (raw_plan.buffers[i].role != ceg::BufferRole::Alias) { continue; }
        REQUIRE(raw_plan.buffers[i].alias_of >= 0);
        REQUIRE(static_cast<crd::usize>(raw_plan.buffers[i].alias_of) < i); // producer-first (one-level alias resolution)
    }
    float raw_max = 0.0F;
    float raw_sum = 0.0F;
    run(raw_plan, &raw_max, &raw_sum);

    // cse_run: the two identical gemms collapse (d2 erased, rb rewired onto d1) — one DISPATCH gone.
    ce::DiagnosticEngine diag(ctx, &alloc);
    REQUIRE(ce::cse_run(ctx, *m, diag));
    REQUIRE_FALSE(diag.has_fatal());
    CHECK(g2->is_erased());
    CHECK_FALSE(g1->is_erased());
    CHECK(rs2->is_erased());
    CHECK(rb->operand(0U) == rs1->result(0U));
    CHECK(ra->operand(0U) == rb->operand(0U)); // both reduces read the SAME merged Value — IR fork correct

    // OPT: 1 Gemm + 2 Reduce.
    const ceg::TensorPipelinePlan opt_plan = ceg::plan_tensor_pipeline(ctx, *m, &alloc);
    REQUIRE(opt_plan.reject == ceg::PlanReject::None);
    REQUIRE(opt_plan.stages.size() == 3U);
    int opt_gemm = 0;
    for (crd::usize s = 0; s < opt_plan.stages.size(); ++s) { opt_gemm += opt_plan.stages[s].kind == ceg::StageKind::Gemm ? 1 : 0; }
    REQUIRE(opt_gemm == 1);
    // wiring + the gemm operand value-identity (the check that caught the merged-declare bug on Vulkan).
    crd::i32 o_gemm  = -1;
    crd::i32 o_ga    = -1;
    crd::i32 o_gb    = -1;
    crd::i32 o_maxin = -1;
    crd::i32 o_sumin = -1;
    for (crd::usize s = 0; s < opt_plan.stages.size(); ++s)
    {
        if (opt_plan.stages[s].kind == ceg::StageKind::Gemm) { o_ga = opt_plan.stages[s].bind[0]; o_gb = opt_plan.stages[s].bind[1]; o_gemm = opt_plan.stages[s].bind[2]; }
        if (opt_plan.stages[s].kind != ceg::StageKind::Reduce) { continue; }
        if (opt_plan.stages[s].op == ra) { o_maxin = opt_plan.stages[s].bind[0]; }
        else if (opt_plan.stages[s].op == rb) { o_sumin = opt_plan.stages[s].bind[0]; }
    }
    REQUIRE(o_maxin >= 0);
    REQUIRE(o_sumin >= 0);
    CHECK(o_maxin == o_sumin);                                                    // both reduces read the ONE merged alias
    REQUIRE(o_gemm >= 0);
    CHECK(opt_plan.buffers[static_cast<crd::usize>(o_maxin)].alias_of == o_gemm); // the alias points at the surviving gemm output
    REQUIRE(o_ga >= 0);
    REQUIRE(o_gb >= 0);
    CHECK(opt_plan.buffers[static_cast<crd::usize>(o_ga)].value == a_in); // ⛔ gemm reads A (not the merged declare — the fix holds)
    CHECK(opt_plan.buffers[static_cast<crd::usize>(o_gb)].value == b_in);
    float opt_max = 0.0F;
    float opt_sum = 0.0F;
    run(opt_plan, &opt_max, &opt_sum);

    // BIT-EXACT: removing the duplicate gemm dispatch changed nothing observable (the 26a differential-test standard on a REMOVED
    // DISPATCH — the witness at full teeth). rb (the sum) is the load-bearing rewired consumer.
    CHECK(opt_max == raw_max);
    CHECK(opt_sum == raw_sum);
    // and both CORRECT vs the independent f64 ref (a TOLERANCE — device gemm is f32, ref is f64).
    const float tol = static_cast<float>(static_cast<crd::f64>(kL) * 2e-3 * maxabs);
    const float fm  = raw_max - static_cast<float>(max_ref);
    CHECK((fm < 0.0F ? -fm : fm) <= tol);
    const float fs = raw_sum - static_cast<float>(sum_ref);
    CHECK((fs < 0.0F ? -fs : fs) <= tol);
}

// CEIR-28d — the §80 autotuner MEASURED on a REAL D3D12 device (the ⛔ migrated=both-backends rule: the measurer runs on BOTH GPU
// backends). The SAME 26e-3b fp32-MLP payload is planned with the FULL 4-config PlanOptions space, each TIMED (median of DX12's
// last_gpu_ms) + gated BIT-EXACT vs the default (the free oracle-gate); the winner is emitted as one tune.entry keyed by
// (device="dx12:"+adapter, env, program_hash(post-expansion payload), shape) and REPLAYED through the real loader (hit + wrong-hash
// miss + replay BIT-EXACT). ⛔ NO committed .ceir asset this tick (self-consistent measure+replay; a committed DX12 row + anti-drift
// waits on knowing the DX12 winner — 28d-2/28z). D3D12 is Windows-only; the module self-skips elsewhere.
TEST_CASE("ceir 28d: the sec-80 autotuner measures the 4-config PlanOptions space on DX12, emits + replays one tune.entry (BIT-EXACT)",
          "[ceir][ml][gpu][tune]")
{
    crd::memory::GrowableTlsfAllocator devroot;
    crd::gpu::Dx12ComputeContext       compute(&devroot);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-28d autotuner measurer"); return; }
    auto gpuctx = crd::gpu::create_dx12_gpu_context(); // the adapter NAME lives on the gpu context (mirrors Vk reading it off VulkanGpuContext)
    if (gpuctx == nullptr) { WARN("no D3D12 gpu context — skipping the CEIR-28d device key"); return; }

    constexpr crd::u32 mrows = 4;
    constexpr crd::u32 d0    = 8;
    constexpr crd::u32 d1    = 16; // 26e-3b's KNOWN-GOOD shape (h1 numel 64 — the no-fuse relu VizDispatch cook-binds)
    constexpr crd::u32 d2    = 2;
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    (void)ce::tune::register_tune_ops(ctx);

    // ── the PAYLOAD: the 26e-3b fp32 2-layer MLP (x·W1 → relu → ·W2) ──
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
    ce::Value* const     x_val     = mkd(tf(ctx, sh2(ctx, mrows, d0)));
    ce::Value* const     w1_val    = mkd(tf(ctx, sh2(ctx, d0, d1)));
    ce::Value* const     w2_val    = mkd(tf(ctx, sh2(ctx, d1, d2)));
    ce::Value*           mlpops[3] = {x_val, w1_val, w2_val};
    ce::Operation* const mo        = ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mlpops, 3U),
                                                          1U, tf(ctx, sh2(ctx, mrows, d2)), 0U);
    ctx.set_attr(mo, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
    b->append(mo);
    REQUIRE(ceg::expand_ml_ops(ctx, *m).error == ceg::MlExpandError::None);

    const ceg::PlanOptions        def_opts;
    const ceg::TensorPipelinePlan plan0 = ceg::plan_tensor_pipeline(ctx, *m, &root, def_opts);
    REQUIRE(plan0.reject == ceg::PlanReject::None);
    const ce::Value* out_val = nullptr;
    for (crd::usize i = 0; i < plan0.buffers.size(); ++i) { if (plan0.buffers[i].role == ceg::BufferRole::Output) { out_val = plan0.buffers[i].value; } }
    REQUIRE(out_val != nullptr);
    const crd::u32 out_len = mrows * d2;

    // 26e-3b's EXACT seeds (NEGATIVE pre-activations so relu is non-vacuous — the bit-exact oracle-gate has teeth).
    crd::containers::Array<float> x_in(&root);
    crd::containers::Array<float> w1_in(&root);
    crd::containers::Array<float> w2_in(&root);
    x_in.resize(mrows * d0, 0.0F);
    w1_in.resize(d0 * d1, 0.0F);
    w2_in.resize(d1 * d2, 0.0F);
    for (crd::u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<float>(static_cast<int>(i) - 12); }
    for (crd::u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<float>(static_cast<int>(i % 7) - 3); }
    for (crd::u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<float>(static_cast<int>(i % 5) - 2); }
    const QuantSeed seeds[3] = {{x_val, x_in.data(), nullptr, mrows * d0},
                                {w1_val, w1_in.data(), nullptr, d0 * d1},
                                {w2_val, w2_in.data(), nullptr, d1 * d2}};
    crd::containers::Array<float> out0(&root);
    out0.resize(out_len, 0.0F);
    REQUIRE(run_quant_module(compute, ctx, &root, plan0, seeds, 3U, out_val, out0.data(), out_len)); // the default reference output

    // ── the device KEY: "dx12:"+adapter_name (a valid derived string). ⛔ create_dx12_gpu_context + Dx12ComputeContext both pick the
    //    DEFAULT adapter, so the name identifies the device doing the work; a STRICT same-adapter proof is deferred to the committed
    //    DX12 row (28d-2/28z) — 28d's measure+replay is self-consistent regardless of exact adapter identity. ──
    const char* const adapter = gpuctx->adapter_name();
    REQUIRE(adapter != nullptr);
    REQUIRE(adapter[0] != '\0'); // a non-empty derived key
    char dev_buf[320] = {};
    (void)std::snprintf(dev_buf, sizeof(dev_buf), "dx12:%s", adapter);
    const crd::containers::StringView device(dev_buf);
    const crd::containers::StringView env("win32"); // D3D12 is Windows-only
    const crd::containers::StringView shape("mlp:4x8x16x2");
    const crd::u64                    ph = ce::tune::program_hash(ctx, *m, &root);

    double            medians[4] = {};
    crd::u64          sig[4]     = {};
    ceg::PlanOptions  winner;
    ce::Module* const emit = measure_tune_entry(compute, ctx, &root, *m, seeds, 3U, out_val, out_len, device, env, shape, medians, sig, &winner);
    REQUIRE(emit != nullptr);
    // ⭐ the discriminating IDENTITY gate — MUST match Vk's sigs (plan_sig is over the backend-independent plan): share INERT (tt≡tf,
    //    ft≡ff), fuse LIVE (tt≢ft). CAPTURE to eyeball against 28b-2a's 0x83e33779c4351c48 / 0x4e7803adf4d43172.
    CAPTURE(sig[0], sig[1], sig[2], sig[3]);
    CHECK(sig[0] == sig[1]);
    CHECK(sig[2] == sig[3]);
    CHECK(sig[0] != sig[2]);
    CHECK(ce::tune::find_tune_misuse(ctx, *emit).kind == ce::tune::TuneMisuseKind::None);

    // ── REPLAY through the real loader (the v17 select_schedule leg, on DX12) ──
    const ceg::TuneCacheLookup hit = ceg::plan_options_from_tune_cache(ctx, *emit, device, env, ph, shape);
    REQUIRE(hit.hit);
    CHECK(hit.opts.fuse_gemm_relu == winner.fuse_gemm_relu);
    CHECK(hit.opts.share_intermediate_storage == winner.share_intermediate_storage);
    const ceg::TuneCacheLookup miss = ceg::plan_options_from_tune_cache(ctx, *emit, device, env, ph + 1U, shape);
    CHECK_FALSE(miss.hit);

    const ceg::TensorPipelinePlan plan_r = ceg::plan_tensor_pipeline(ctx, *m, &root, hit.opts);
    REQUIRE(plan_r.reject == ceg::PlanReject::None);
    crd::containers::Array<float> out_r(&root);
    out_r.resize(out_len, 0.0F);
    REQUIRE(run_quant_module(compute, ctx, &root, plan_r, seeds, 3U, out_val, out_r.data(), out_len));
    int replay_mismatch = -1;
    for (crd::u32 i = 0; i < out_len && replay_mismatch < 0; ++i) { if (out_r[i] != out0[i]) { replay_mismatch = static_cast<int>(i); } }
    CAPTURE(replay_mismatch);
    CHECK(replay_mismatch == -1);

    const crd::containers::String emit_txt = ce::print(ctx, *emit, &root);
    INFO("28d device=\"" << dev_buf << "\" program_hash=" << ph << " shape=mlp:4x8x16x2 winner{fuse=" << winner.fuse_gemm_relu
                         << ",share=" << winner.share_intermediate_storage << "} medians_ms[tt,tf,ft,ff]=[" << medians[0] << ","
                         << medians[1] << "," << medians[2] << "," << medians[3] << "]\nemit:\n" << emit_txt.c_str());
    CHECK(true); // anchor the INFO (row/asset data prints under -s)
}

// CEIR-28z-2 (DX12 leg) — the committed 3-row `assets/ceir/tune_cache.ceir` anti-drifts the DX12 row on a real D3D12 device (the
// migrated=both-backends shape: one committed-asset test PER backend TU, both reading the SAME file). Mirrors the Vk 28b-2b: parse
// the 3-row cache → find_tune_misuse None → all rows addressable → wrong-env miss → THIS device's (dx12:RTX) row FIELD-anti-drifts a
// fresh re-measure + REPLAYS BIT-EXACT. ⛔ per-row FIELD-equality via the loader (a 3-row cache breaks module-print-equality).
TEST_CASE("ceir 28z-2: the committed 3-row tune_cache.ceir anti-drifts the DX12 row + all rows addressable + env discriminates",
          "[ceir][ml][gpu][tune]")
{
    crd::memory::GrowableTlsfAllocator devroot;
    crd::gpu::Dx12ComputeContext       compute(&devroot);
    if (!compute.valid()) { WARN("no D3D12 device — skipping the CEIR-28z-2 DX12 committed-cache anti-drift"); return; }
    auto gpuctx = crd::gpu::create_dx12_gpu_context();
    if (gpuctx == nullptr) { WARN("no D3D12 gpu context — skipping the CEIR-28z-2 device key"); return; }

    constexpr crd::u32 mrows = 4;
    constexpr crd::u32 d0    = 8;
    constexpr crd::u32 d1    = 16;
    constexpr crd::u32 d2    = 2;
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    (void)ce::tune::register_tune_ops(ctx);

    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
    ce::Value* const     x_val     = mkd(tf(ctx, sh2(ctx, mrows, d0)));
    ce::Value* const     w1_val    = mkd(tf(ctx, sh2(ctx, d0, d1)));
    ce::Value* const     w2_val    = mkd(tf(ctx, sh2(ctx, d1, d2)));
    ce::Value*           mlpops[3] = {x_val, w1_val, w2_val};
    ce::Operation* const mo        = ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mlpops, 3U),
                                                          1U, tf(ctx, sh2(ctx, mrows, d2)), 0U);
    ctx.set_attr(mo, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
    b->append(mo);
    REQUIRE(ceg::expand_ml_ops(ctx, *m).error == ceg::MlExpandError::None);

    const ceg::PlanOptions        def_opts;
    const ceg::TensorPipelinePlan plan0 = ceg::plan_tensor_pipeline(ctx, *m, &root, def_opts);
    REQUIRE(plan0.reject == ceg::PlanReject::None);
    const ce::Value* out_val = nullptr;
    for (crd::usize i = 0; i < plan0.buffers.size(); ++i) { if (plan0.buffers[i].role == ceg::BufferRole::Output) { out_val = plan0.buffers[i].value; } }
    REQUIRE(out_val != nullptr);
    const crd::u32 out_len = mrows * d2;

    crd::containers::Array<float> x_in(&root);
    crd::containers::Array<float> w1_in(&root);
    crd::containers::Array<float> w2_in(&root);
    x_in.resize(mrows * d0, 0.0F);
    w1_in.resize(d0 * d1, 0.0F);
    w2_in.resize(d1 * d2, 0.0F);
    for (crd::u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<float>(static_cast<int>(i) - 12); }
    for (crd::u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<float>(static_cast<int>(i % 7) - 3); }
    for (crd::u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<float>(static_cast<int>(i % 5) - 2); }
    const QuantSeed seeds[3] = {{x_val, x_in.data(), nullptr, mrows * d0},
                                {w1_val, w1_in.data(), nullptr, d0 * d1},
                                {w2_val, w2_in.data(), nullptr, d1 * d2}};
    crd::containers::Array<float> out0(&root);
    out0.resize(out_len, 0.0F);
    REQUIRE(run_quant_module(compute, ctx, &root, plan0, seeds, 3U, out_val, out0.data(), out_len));

    const char* const adapter = gpuctx->adapter_name();
    REQUIRE(adapter != nullptr);
    char dev_buf[320] = {};
    (void)std::snprintf(dev_buf, sizeof(dev_buf), "dx12:%s", adapter);
    const crd::containers::StringView device(dev_buf);
    const crd::containers::StringView env("win32"); // D3D12 is Windows-only
    const crd::containers::StringView shape("mlp:4x8x16x2");
    const crd::u64                    ph = ce::tune::program_hash(ctx, *m, &root);

    std::ifstream cf(CRD_REPO_DIR "/assets/ceir/tune_cache.ceir", std::ios::binary | std::ios::ate);
    REQUIRE(cf.good());
    const std::streamsize csz = cf.tellg();
    cf.seekg(0);
    crd::containers::Array<char> csrc(&root);
    csrc.resize(static_cast<crd::usize>(csz), '\0');
    cf.read(csrc.data(), csz);
    const ce::ParseResult cpr = ce::parse(ctx, crd::containers::StringView(csrc.data(), static_cast<crd::usize>(csz)));
    REQUIRE(cpr.ok);
    REQUIRE(cpr.module != nullptr);
    CHECK(ce::tune::find_tune_misuse(ctx, *cpr.module).kind == ce::tune::TuneMisuseKind::None);

    crd::containers::Array<ce::tune::TuneEntry> entries(&root);
    const crd::u32                              n_rows = ce::tune::load_tune_entries(ctx, *cpr.module, entries);
    REQUIRE(n_rows == 3U);

    // ── ROWS-ADDRESSABLE (device-independent): all 3 rows HIT their own key with the shared schedule ──
    struct CacheKey { const char* device; const char* env; };
    const CacheKey known[3] = {{"vk:NVIDIA GeForce RTX 4070 Ti SUPER", "win32"},
                               {"dx12:NVIDIA GeForce RTX 4070 Ti SUPER", "win32"},
                               {"vk:llvmpipe (LLVM 20.1.2, 256 bits)", "linux"}};
    for (crd::u32 i = 0; i < 3U; ++i)
    {
        const ceg::TuneCacheLookup lk = ceg::plan_options_from_tune_cache(
            ctx, *cpr.module, crd::containers::StringView(known[i].device), crd::containers::StringView(known[i].env), ph, shape);
        CAPTURE(known[i].device);
        REQUIRE(lk.hit);
        CHECK(lk.opts.fuse_gemm_relu == true);
        CHECK(lk.opts.share_intermediate_storage == true);
    }
    // ⛔ wrong-ENV MISS: the dx12:RTX device under env="linux" MISSES (env is a real key dimension).
    const ceg::TuneCacheLookup wrong_env = ceg::plan_options_from_tune_cache(
        ctx, *cpr.module, crd::containers::StringView("dx12:NVIDIA GeForce RTX 4070 Ti SUPER"), crd::containers::StringView("linux"), ph, shape);
    CHECK_FALSE(wrong_env.hit);
    CHECK(entries[1].program_hash == 13485518615284092459ULL); // the i64 the file prints reads back as this u64 (28b-1 u64-widen)

    // ── find THIS device's row (dx12:RTX on this box) → anti-drift + replay ──
    int mi = -1;
    for (crd::u32 i = 0; i < n_rows; ++i) { if (entries[i].device == device && entries[i].env == env) { mi = static_cast<int>(i); } }
    if (mi >= 0)
    {
        const ce::tune::TuneEntry& row = entries[static_cast<crd::usize>(mi)];
        double            medians[4] = {};
        crd::u64          sig[4]     = {};
        ceg::PlanOptions  winner;
        ce::Module* const emit = measure_tune_entry(compute, ctx, &root, *m, seeds, 3U, out_val, out_len, device, env, shape, medians, sig, &winner);
        REQUIRE(emit != nullptr);
        crd::containers::Array<ce::tune::TuneEntry> fresh(&root);
        REQUIRE(ce::tune::load_tune_entries(ctx, *emit, fresh) == 1U);
        INFO("committed row fuse=" << row.fuse << " share=" << row.share << " | fresh fuse=" << fresh[0].fuse << " share=" << fresh[0].share);
        CHECK(row.device == fresh[0].device);
        CHECK(row.env == fresh[0].env);
        CHECK(row.program_hash == fresh[0].program_hash);
        CHECK(row.shape == fresh[0].shape);
        CHECK(row.fuse == fresh[0].fuse);   // ⛔ a re-tune winner FLIP fails HERE
        CHECK(row.share == fresh[0].share);

        const ceg::TuneCacheLookup hit = ceg::plan_options_from_tune_cache(ctx, *cpr.module, device, env, ph, shape);
        REQUIRE(hit.hit);
        const ceg::TensorPipelinePlan plan_r = ceg::plan_tensor_pipeline(ctx, *m, &root, hit.opts);
        REQUIRE(plan_r.reject == ceg::PlanReject::None);
        crd::containers::Array<float> out_r(&root);
        out_r.resize(out_len, 0.0F);
        REQUIRE(run_quant_module(compute, ctx, &root, plan_r, seeds, 3U, out_val, out_r.data(), out_len));
        int replay_mismatch = -1;
        for (crd::u32 i = 0; i < out_len && replay_mismatch < 0; ++i) { if (out_r[i] != out0[i]) { replay_mismatch = static_cast<int>(i); } }
        CAPTURE(replay_mismatch);
        CHECK(replay_mismatch == -1);
    }
    else
    {
        WARN("this device=\"" << dev_buf << "\" has no committed row — anti-drift branch skipped");
    }
}
