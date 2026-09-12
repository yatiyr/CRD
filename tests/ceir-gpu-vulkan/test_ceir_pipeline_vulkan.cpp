// CEIR-22c-2 (Vulkan) — the §137 GEMM→FFT→reduction PROOF: an authored-in-C++ ceir module (linalg.gemm → tensor.reshape →
// tensor.fft → tensor.reduce) is PLANNED (plan_tensor_pipeline → the device-resident buffer graph) and EXECUTED
// (execute_tensor_pipeline) as ONE SUBMIT on a REAL Vulkan device — NO CPU round-trip between stages (GpuOnly intermediates,
// the reshape a zero-copy ALIAS). The chain's final scalar is validated against an INDEPENDENT composed reference
// (triple-loop GEMM → naive DFT → serial sum, all f64) within a DERIVED, DECLARED tolerance. ⛔ the graph tier (gemm/reduce)
// + the kernel tier (fft) BOTH record into the ONE recorder — the tier split dissolves at GlslKernel→ComputePipeline. The
// device-free plan is proven in tests/ceir-gpu/test_tensor_pipeline.cpp; here the plan DRIVES a real chained GPU run.

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/passes/canonicalize.hpp> // CEIR-26b-2b: canonicalize_run — the reshape fold must be BIT-EXACT vs the raw device output
#include <crd/ceir/passes/cse.hpp>       // CEIR-26c-2b: cse_run — removing a DUPLICATE gemm dispatch must be BIT-EXACT vs the raw device output
#include <crd/ceir/passes/dce.hpp>       // CEIR-26a-3: dce_run — the DCE'd backward must be BIT-EXACT vs the raw device output
#include <crd/ceir/gen/arith_ops.hpp>    // CEIR-22c-3d: register_arith_ops (the parsed dispatch grid consts)
#include <crd/ceir/gen/compute_ops.hpp>  // CEIR-22c-3d: register_compute_ops (the parsed viz dispatches)
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/parse.hpp>            // CEIR-22c-3d: parse the authored .ceir asset
#include <crd/ceir/gpu/ckir_synth.hpp>
#include <crd/ceir/gpu/coopvec_mlp.hpp> // CEIR-24c-2b: coopvec_config_from_mlp / coopvec_weights_from_mlp (the CLAIM conversion)
#include <crd/ceir/gpu/expand_ml.hpp> // CEIR-24b-4: expand_ml_ops (the ml.attention/ml.mlp -> 22/23 vocab rewrite)
#include <crd/ceir/gpu/grad.hpp>      // CEIR-25b-4b: build_gradient — the backward pass planned + EXECUTED device-resident
#include <crd/ceir/gpu/partition_ml.hpp> // CEIR-24c-2b: coopvec_can_claim_mlp (the CLAIM check before native dispatch)
#include <crd/ceir/gpu/tensor_pipeline.hpp>
#include <crd/ceir/gpu/tensor_pipeline_exec.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/linalg.hpp>
#include <crd/ceir/ml.hpp> // CEIR-24b-4: ml.attention/ml.mlp + register_dialect
#include <crd/ceir/transform.hpp> // CEIR-27b: register_transform_ops + find_transform_misuse (the parsed schedule assets)
#include <crd/ceir/tune.hpp> // CEIR-28b-2a: register_tune_ops + program_hash + find_tune_misuse (the measured config-cache row)
#include <crd/ceir/gen/tune_ops.hpp> // CEIR-28b-2a: tune::build_entry (the emitted tune.entry op)
#include <crd/ceir/quant.hpp> // CEIR-23b-2d: register quant + build_dequantize (the fused QuantGemm gate)
#include <crd/ceir/tensor.hpp>
#include <crd/ceir/type.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp> // CEIR-22c-3d: ckir_read (the authored viz kernels)
#include <crd/kir/ckir_glsl.hpp>  // emit_contract_glsl / emit_reduce_glsl / emit_compute_kernel_glsl / GlslKernel

#include <crd/gpu/vulkan_compute_context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>

#include <crd/math/cmath.hpp>

#include <crd/hesap/autodiff/gradient_check.hpp> // CEIR-25b-4b: grad_fd — the FD witness on f(A)=sum(gemm(A,A))
#include <crd/hesap/autodiff/nn_reverse.hpp>     // CEIR-25b-4b: matmul_vjp — the analytic (dialect-independent) grad ref

#include "../gpu-shared/autodiff_fd_functors.hpp" // CEIR-25c-2: SumGemmAA + MlpLoss FD witnesses (shared with the DX12 pipeline TU)

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>  // CEIR-22c-3g: caller-stamped submit wall-time (the executor stays backend-free / untimed)
#include <cstdio>  // CEIR-28b-2a: std::snprintf — the "vk:"+adapter_name() device key (a scalar format, not a container)
#include <cstring> // std::memcpy (a scalar copy, not a container)
#include <fstream> // CEIR-22c-3d: read the authored .ceir / .ckir assets

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

namespace ce  = crd::ceir;
namespace ceg = crd::ceir::gpu;
namespace kir = crd::kir;

namespace
{
constexpr int kSide = 8;               // A,B,D are [8,8]
constexpr int kL    = kSide * kSide;   // reshaped length 64 (a power of two)

ce::TypeId shp(ce::Context& c, crd::containers::ConstSpan<ce::TypeId> d) { return c.type_shape(d); }
ce::TypeId sh1(ce::Context& c, crd::u32 a) { const ce::TypeId d[1] = {c.type_dim_static(a)}; return shp(c, crd::containers::ConstSpan<ce::TypeId>(d, 1U)); }
ce::TypeId sh2(ce::Context& c, crd::u32 a, crd::u32 b)
{
    const ce::TypeId d[2] = {c.type_dim_static(a), c.type_dim_static(b)};
    return shp(c, crd::containers::ConstSpan<ce::TypeId>(d, 2U));
}
ce::TypeId tf(ce::Context& c, ce::TypeId s) { return c.type_tensor(c.type_f32(), s); }

crd::u32 dim_ext(ce::Context& c, ce::TypeId t, crd::u32 axis)
{
    const ce::Type sh = c.type_of(c.type_of(t).members[1]);
    return axis < sh.members.size() ? c.type_of(sh.members[static_cast<crd::usize>(axis)]).count : 0U;
}
crd::u64 tnumel(ce::Context& c, ce::TypeId t)
{
    const ce::Type sh = c.type_of(c.type_of(t).members[1]);
    crd::u64       n  = 1;
    for (crd::usize i = 0; i < sh.members.size(); ++i) { n *= c.type_of(sh.members[i]).count; }
    return n;
}

// The gate's resolver state: it re-synthesizes + emits + compiles each stage to a ComputePipeline it OWNS (kept alive to submit).
struct Resolver
{
    ce::Context*                                alloc_ctx = nullptr;
    crd::memory::IAllocator*                    alloc     = nullptr;
    crd::gpu::VulkanComputeContext*             compute   = nullptr;
    std::unique_ptr<crd::gpu::ComputePipeline>  pipes[16]; // design B has 5 stages; the 25c-2 MLP backward has 11 (gemm×5/transpose×4/dispatch×2)
    int                                         n         = 0;
};

// The workgroup count an authored arith.const grid operand carries (the asset DRIVES the grid — NOT re-derived from numel).
crd::u32 const_grid(ce::Context& c, const ce::Value* v)
{
    const ce::Operation* const d = v->defining_op();
    if (d == nullptr) { return 1U; }
    const ce::AttrValue av = c.attr_value(d->attr(crd::containers::StringView("value")));
    return av.i > 0 ? static_cast<crd::u32>(av.i) : 1U;
}

// CEIR-23b-2d: load an authored .ckir compute kernel BY PATH and emit its GLSL (the fused QuantGemm + symmetric Dequant stages
// are hand-authored kernels the resolver ckir_reads, like VizDispatch — never ckir_synth'd). Returns false on read/emit failure.
// CEIR-26d-4b: `local_size_x` (default 0 = use as-authored) cook-binds a SENTINEL asset (local_size=0) before emit — the emit guard
// refuses an unbound 0, so a sentinel kernel (relu.ckir/relu_vjp.ckir) MUST have its local_size bound here (the resolver's job on
// the device path). Non-sentinel callers (quant kernels) pass nothing and emit as-authored.
bool load_emit_ckir(const char* path, kir::KGraph& g, kir::GlslKernel& kern, crd::memory::IAllocator* alloc, crd::u32 local_size_x = 0U)
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
    return kir::emit_compute_kernel_glsl(g, ke, alloc, kern);
}

ceg::ResolvedStage resolve_stage(const ceg::PlanStage& st, void* user)
{
    auto&              res = *static_cast<Resolver*>(user);
    ce::Context&       c   = *res.alloc_ctx;
    ceg::ResolvedStage rs;
    kir::KGraph        g(res.alloc);
    kir::GlslKernel    kern(res.alloc);
    int                nbind    = 0;
    crd::u32           pushsize = 0;

    if (st.kind == ceg::StageKind::Gemm || st.kind == ceg::StageKind::GemmRelu)
    {
        // ⭐ CEIR-26e: GemmRelu re-synthesizes the SAME contract with the relu epilogue (synth_gemm appends Max(Contract,0), which
        //    emit_contract_glsl UNWRAPS to `max(acc, 0.0)`); the push blob {M,K,N,1} + grid M·N + nbind 3 are IDENTICAL by construction.
        const ceg::GemmEpilogue ep = st.kind == ceg::StageKind::GemmRelu ? ceg::GemmEpilogue::Relu : ceg::GemmEpilogue::None;
        const ceg::GraphSynth   s  = ceg::synth_gemm(c, *st.op, g, ep);
        if (s.reject != ceg::SynthReject::None || !kir::emit_contract_glsl(g, s.output, kern)) { return rs; }
        const crd::u32 m = dim_ext(c, st.op->operand(0U)->type(), 0U);
        const crd::u32 k = dim_ext(c, st.op->operand(0U)->type(), 1U);
        const crd::u32 nn = dim_ext(c, st.op->operand(1U)->type(), 1U);
        const crd::u32 pc[4] = {m, k, nn, 1U};
        std::memcpy(rs.push, pc, sizeof(pc));
        pushsize = 16U;
        rs.gx    = (m * nn + 255U) / 256U;
        nbind    = 3;
    }
    else if (st.kind == ceg::StageKind::Fft)
    {
        const ceg::FftSynth s = ceg::synth_fft(c, *st.op, g);
        if (s.reject != ceg::SynthReject::None || !kir::emit_compute_kernel_glsl(g, s.plan.entry, res.alloc, kern)) { return rs; }
        rs.gx    = 1U; // one workgroup (local_size = n/2)
        pushsize = 0U;
        nbind    = 6;
    }
    else if (st.kind == ceg::StageKind::Reduce)
    {
        const ceg::GraphSynth s = ceg::synth_reduce(c, *st.op, g);
        if (s.reject != ceg::SynthReject::None || !kir::emit_reduce_glsl(g, s.output, kern)) { return rs; }
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
        // the FUSED symmetric per-tensor dequant-gemm kernel (CEIR-23b-2c); 1 workgroup of M*N threads, 4 binds {A,W_q8,scale,D}.
        if (!load_emit_ckir(CRD_REPO_DIR "/assets/ckir/quant_gemm_q8.ckir", g, kern, res.alloc)) { return rs; }
        rs.gx    = 1U;
        pushsize = 0U;
        nbind    = static_cast<int>(st.nbind); // 4
    }
    else if (st.kind == ceg::StageKind::Dequant)
    {
        // the UNFUSED symmetric dequant kernel (CEIR-23b-2a); 1 workgroup of K*N threads, 3 binds {W_q8,scale,out}.
        if (!load_emit_ckir(CRD_REPO_DIR "/assets/ckir/quant_dequantize_q8_sym.ckir", g, kern, res.alloc)) { return rs; }
        rs.gx    = 1U;
        pushsize = 0U;
        nbind    = static_cast<int>(st.nbind); // 3
    }
    else if (st.kind == ceg::StageKind::Transpose || st.kind == ceg::StageKind::Broadcast
             || st.kind == ceg::StageKind::Elementwise)
    {
        // CEIR-25b-4a: the autodiff backward vocab — re-synthesize + emit the NEW root emitters (one thread/output element, PC{nout}).
        bool ok = false;
        if (st.kind == ceg::StageKind::Transpose)
        {
            const ceg::GraphSynth s = ceg::synth_transpose(c, *st.op, g);
            ok = s.reject == ceg::SynthReject::None && kir::emit_permute_glsl(g, s.output, kern);
        }
        else if (st.kind == ceg::StageKind::Broadcast)
        {
            const ceg::GraphSynth s = ceg::synth_broadcast(c, *st.op, g);
            ok = s.reject == ceg::SynthReject::None && kir::emit_broadcast_nd_glsl(g, s.output, kern);
        }
        else
        {
            const ceg::GraphSynth s = ceg::synth_elementwise(c, *st.op, g);
            ok = s.reject == ceg::SynthReject::None && kir::emit_elementwise_glsl(g, s.output, res.alloc, kern); // fused emitter needs scratch
        }
        if (!ok) { return rs; }
        const crd::u64 out_n  = tnumel(c, st.op->result(0U)->type());
        const crd::u32 pc[4]  = {static_cast<crd::u32>(out_n), 0U, 0U, 0U};
        std::memcpy(rs.push, pc, sizeof(pc));
        pushsize = 16U;
        rs.gx    = (static_cast<crd::u32>(out_n) + 255U) / 256U;
        nbind    = static_cast<int>(st.nbind); // 2 (transpose/broadcast) / 3 (elementwise)
    }
    else // VizDispatch — the AUTHORED viz .ckir, loaded BY KERNEL SYMBOL (the mixed high-level-tensor + CKIR stage). ckir_read →
    {    // emit_compute_kernel_glsl; the grid is the asset's OWN arith.const operands (the asset-drives-it rule, not numel).
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
            // route Sq through bind_authored_local_size (the "extent" it supplies is Sq=rows, NOT the total numel) so an oversize
            // Sq > kMaxAuthoredLocalSize (== the groupshared m_sh/d_sh cap 1024) is a TYPED LocalSizeExceedsLimit ⇒ UnresolvedKernel,
            // not a silent device-illegal local_size. The scalar-eval spec-const default (3) is NOT applied on device — set Sk here.
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
        if (!kir::emit_compute_kernel_glsl(g, ve, res.alloc, kern)) { return rs; }
        rs.gx    = const_grid(c, st.op->operand(0U));
        rs.gy    = const_grid(c, st.op->operand(1U));
        rs.gz    = const_grid(c, st.op->operand(2U));
        pushsize = 0U;
        nbind    = static_cast<int>(st.nbind); // 3 (re,im,mag) / (mag,max,norm)
    }

    const auto spv = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                     "ceir_pipe", res.alloc);
    if (!spv.ok || res.n >= 16) { return rs; }
    res.pipes[res.n] = res.compute->create_pipeline_from_spirv(
        crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nbind, pushsize);
    rs.pipeline  = res.pipes[res.n].get();
    rs.push_size = pushsize;
    ++res.n;
    return rs;
}

// CEIR-23b-2d / 23c-c: one seeded ExternalIn — matched to a plan buffer by SSA Value. Exactly one of {floats, packed} is set:
// `floats` uploads `count` f32; `packed` uploads `count` u32-packed int8 words (BIT-reinterpret, not float). An ExternalIn with
// no matching seed (the int8 zp, the β=0 accumulator C) uploads zeros.
struct QuantSeed
{
    const ce::Value* value  = nullptr;
    const float*     floats = nullptr;
    const crd::u32*  packed = nullptr;
    crd::u32         count  = 0;
};

// CEIR-25c-2 (b): one readback target — the buffer realizing `value` is copied into `dst` (`len` floats). MULTIPLE outputs let a
// backward pass read BOTH dW1 (Output) AND dW2 (Intermediate = GpuOnly) in one submit — a by-Value readback regardless of role.
struct QuantOut
{
    const ce::Value* value = nullptr;
    float*           dst   = nullptr;
    crd::usize       len   = 0;
};

// Materialize a planned module's buffers, execute it on `compute`, and read back each `outs[o]` (by SSA Value) into its dst. The
// ONE implementation (single-output callers use the thin wrapper below). ExternalIns are seeded by IDENTITY from `seeds` (a LIST —
// a multi-input MLP feeds x, W1, W2, dLoss). ⛔ (b): a NAMED output is created GpuToCpu REGARDLESS of its plan role — an Intermediate
// (dW2, GpuOnly) is otherwise UNMAPPABLE (vulkan_compute_context.cpp:61/370). Returns false only on a device / materialize failure.
bool run_quant_module_n(crd::gpu::VulkanComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* root,
                        const ceg::TensorPipelinePlan& plan, const QuantSeed* seeds, crd::usize n_seeds, const QuantOut* outs,
                        crd::usize n_outs, crd::u32* n_allocated = nullptr)
{
    const crd::usize nb = plan.buffers.size();
    if (nb > 32U || n_outs > 8U) { return false; }
    const auto is_named_out = [&](const ce::Value* v) {
        for (crd::usize o = 0; o < n_outs; ++o) { if (outs[o].value == v) { return true; } }
        return false;
    };
    std::unique_ptr<crd::gpu::ComputeBuffer> owned[32];
    crd::gpu::ComputeBuffer*                 bufs[32] = {};
    crd::u32                                 alloc_count = 0; // CEIR-26f-3a: physical create_buffer calls (aliased buffers skip it)
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        // ⛔ CEIR-26f-3a: SHARE the realized buffer for ANY aliased buffer — a reshape VIEW (role Alias) OR a storage TENANT
        //    (role Intermediate, alias_of>=0 from assign_shared_storage). Widened from role==Alias (26f-2b hole was plan-side;
        //    this is the device-side twin — the tenant is alloc-skipped, so aliasing goes LIVE). owned[i] stays null ⇒ RAII
        //    destroys ONLY the landlord (no double-destroy); the ExternalIn upload keys on role (below) so a tenant is not seeded.
        if (pb.alias_of >= 0) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        crd::gpu::ComputeMemory mem = crd::gpu::ComputeMemory::GpuOnly;
        if (pb.role == ceg::BufferRole::ExternalIn) { mem = crd::gpu::ComputeMemory::CpuToGpu; }
        else if (pb.role == ceg::BufferRole::Output) { mem = crd::gpu::ComputeMemory::GpuToCpu; }
        if (is_named_out(pb.value)) { mem = crd::gpu::ComputeMemory::GpuToCpu; } // ⛔ (b): host-readable regardless of role (dW2)
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes; // round the 1-byte int8 zp up (raw-view alignment)
        owned[i]          = compute.create_buffer(sz, crd::gpu::compute_usage::storage, mem);
        if (owned[i] == nullptr) { return false; }
        bufs[i] = owned[i].get();
        ++alloc_count;
        if (pb.role != ceg::BufferRole::ExternalIn) { continue; }
        void* raw = owned[i]->map();
        if (raw == nullptr) { return false; }
        const QuantSeed* seed = nullptr;
        for (crd::usize s = 0; s < n_seeds; ++s) { if (seeds[s].value == pb.value) { seed = &seeds[s]; break; } }
        if (seed != nullptr && seed->packed != nullptr)
        {
            auto* w = static_cast<crd::u32*>(raw);
            for (crd::u32 e = 0; e < seed->count; ++e) { w[e] = seed->packed[e]; } // u32-packed int8 — bits, not float
        }
        else if (seed != nullptr)
        {
            auto* d = static_cast<float*>(raw);
            for (crd::u32 e = 0; e < seed->count; ++e) { d[e] = seed->floats[e]; }
        }
        else
        {
            auto*          d   = static_cast<float*>(raw);
            const crd::u64 cnt = pb.bytes / 4ULL;
            for (crd::u64 e = 0; e < cnt; ++e) { d[e] = 0.0F; } // unseeded ExternalIn (zp / β=0 accumulator C) → zeros
        }
        owned[i]->unmap();
    }

    Resolver res;
    res.alloc_ctx           = &ctx;
    res.alloc               = root;
    res.compute             = &compute;
    auto&                   rec = compute.begin();
    const ceg::ExecuteError ee  = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res,
                                                               crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(bufs, nb));
    if (ee != ceg::ExecuteError::None) { return false; }
    // ⛔ record EVERY ShaderWrite→HostRead barrier BEFORE the single submit_and_wait — a barrier recorded after submit is a no-op on
    //    already-completed work and the 2nd readback races.
    crd::i32 out_idx[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    for (crd::usize o = 0; o < n_outs; ++o)
    {
        for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == outs[o].value) { out_idx[o] = static_cast<crd::i32>(i); } }
        if (out_idx[o] < 0) { return false; }
        // ⛔ CEIR-26f-4 (advisor): a named-out that is ALIASED (alias_of>=0 — a tenant or view) has owned[out_idx]==null ⇒ the
        //    map() below null-derefs. Safe today because every named-out is func.return'd (⇒ pinned ⇒ not shareable), but this is a
        //    TYPED REJECT for the latent pin-readback misuse (the [[feedback_plan_output_by_traversal_is_not_ssa_liveness...]] scar).
        if (plan.buffers[static_cast<crd::usize>(out_idx[o])].alias_of >= 0) { return false; }
        rec.barrier(*bufs[static_cast<crd::usize>(out_idx[o])], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
    }
    compute.submit_and_wait();
    for (crd::usize o = 0; o < n_outs; ++o)
    {
        const auto* got = static_cast<const float*>(owned[static_cast<crd::usize>(out_idx[o])]->map());
        if (got == nullptr) { return false; }
        for (crd::usize e = 0; e < outs[o].len; ++e) { outs[o].dst[e] = got[e]; }
        owned[static_cast<crd::usize>(out_idx[o])]->unmap();
    }
    if (n_allocated != nullptr) { *n_allocated = alloc_count; } // CEIR-26f-3a: distinct physical buffers (26f-3b asserts the exact delta)
    return true;
}

// The thin single-output wrapper — the ONE buffer loop lives in run_quant_module_n (all pre-25c-2 callers use this).
bool run_quant_module(crd::gpu::VulkanComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* root,
                      const ceg::TensorPipelinePlan& plan, const QuantSeed* seeds, crd::usize n_seeds, const ce::Value* out_val,
                      float* dst, crd::usize dst_len)
{
    const QuantOut o = {out_val, dst, dst_len};
    return run_quant_module_n(compute, ctx, root, plan, seeds, n_seeds, &o, 1U);
}

// CEIR-28b-2a/2b — the 26e-3b fp32 2-layer MLP payload (x·W1 → relu → ·W2), built + expanded. SHARED so the measurer (28b-2a) and
// the committed-cache anti-drift (28b-2b) hash the IDENTICAL program — a divergent payload would give a hash the committed row
// misses, but sharing keeps the two slices genuinely the same program by construction. Returns the module + the three ExternalIn SSA
// values (the caller seeds them). Registration + the seed values stay caller-side (each TEST_CASE owns its device context + inputs).
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

// CEIR-28b-2a (sec-80) — the PORTABLE Vulkan MEASURER. Enumerate the FULL 4-config PlanOptions space (fuse × share), TIME each on
// the REAL device (N warmup + K timed, MEDIAN of compute.last_gpu_ms() — the whole-plan GPU time: 2 device timestamps bracket the
// ONE submit's command buffer, vulkan_compute_context.cpp:541 TOP_OF_PIPE / :549 BOTTOM_OF_PIPE, so the fixed ShaderWrite→HostRead
// barrier cost cancels across configs), and GATE EVERY config's output BIT-EXACT vs the default {fuse,share}={1,1} (the FREE
// oracle-gate — 27b covered only 2 of the 4 configs; share is 26f-3a-INERT on this MLP but "should be equal" is what the frontier
// rule forbids, so all 4 are compared). Emits ONE tune.entry ROW keying (device, env, program_hash(payload), shape) -> the WINNER's
// schedule. ⛔ the WINNER is the argmin median COLLAPSED to the canonical (lowest-index, default-most) config in its plan-EQUIVALENCE
// class (sig_out): tt and tf are the SAME plan (share inert), so a 64ns noise gap between them must NOT flip the emitted row — else
// 28b-2b's anti-drift fires on jitter, not on a hardware change. Only a plan-DISTINCT winner (the fuse win) is emitted. ⛔ asserts
// NOTHING about which plan-class wins (hardware-measured). Returns the emitted module (caller prints/parses/replays); fills
// medians_out[4] + sig_out[4] (the discriminating identity: sig[0]==sig[1], sig[2]==sig[3], sig[0]!=sig[2] ⇒ share inert, fuse live)
// + *winner. The v17 kir-autotune loop (enumerate → measure → oracle-gate → checked-in DB → replay), portable + device-driven.
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
// The plan's IDENTITY: fnv1a over every stage kind + every buffer's alias_of. Two configs with the same sig produce byte-identical
// plans (the knob was inert); a differing sig proves the knob changed the lowering. The winner-collapse + the discriminating gate.
crd::u64 plan_sig(crd::memory::IAllocator* a, const ceg::TensorPipelinePlan& p)
{
    crd::containers::Array<crd::u32> buf(a);
    for (crd::usize i = 0; i < p.stages.size(); ++i) { buf.push_back(static_cast<crd::u32>(p.stages[i].kind)); }
    buf.push_back(0xFFFFFFFFU); // separate the stage-kind run from the alias_of run
    for (crd::usize i = 0; i < p.buffers.size(); ++i) { buf.push_back(static_cast<crd::u32>(p.buffers[i].alias_of + 1)); } // -1 (none) -> 0
    return crd::containers::fnv1a_64(buf.data(), buf.size() * sizeof(crd::u32));
}
ce::Module* measure_tune_entry(crd::gpu::VulkanComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* root,
                               const ce::Module& payload, const QuantSeed* seeds, crd::usize n_seeds, const ce::Value* out_val,
                               crd::u32 out_len, crd::containers::StringView device, crd::containers::StringView env,
                               crd::containers::StringView shape, double medians_out[4], crd::u64 sig_out[4],
                               ceg::PlanOptions* winner)
{
    constexpr crd::u32 n_warmup = 5; // ⛔ CEIR-28z-1: matched to DX12 — D3D12's first-submit PSO work justified 5, and ONE warmup
                                     //    count across both backends beats two separately-justified numbers (the winner-collapse
                                     //    makes the emitted row warmup-INDEPENDENT, so this cannot change what 28b-2b committed).
    constexpr crd::u32 n_timed  = 7; // MEDIAN of K timed runs (odd → a real sample, not an average)

    ceg::PlanOptions cfgs[4];
    cfgs[0].fuse_gemm_relu = true;  cfgs[0].share_intermediate_storage = true;  // index 0 = the DEFAULT (PlanOptions{}) — the oracle
    cfgs[1].fuse_gemm_relu = true;  cfgs[1].share_intermediate_storage = false;
    cfgs[2].fuse_gemm_relu = false; cfgs[2].share_intermediate_storage = true;
    cfgs[3].fuse_gemm_relu = false; cfgs[3].share_intermediate_storage = false;

    crd::containers::Array<float> ref(root);  // the default config's output — the BIT-EXACT oracle for the other 3
    crd::containers::Array<float> got(root);  // the config-under-test's output (reused)
    ref.resize(out_len, 0.0F);
    got.resize(out_len, 0.0F);

    for (crd::u32 c = 0; c < 4U; ++c)
    {
        const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, payload, root, cfgs[c]);
        REQUIRE(plan.reject == ceg::PlanReject::None);
        sig_out[c] = plan_sig(root, plan); // the plan's identity (before timing) — the winner-collapse + discriminating gate key
        for (crd::u32 w = 0; w < n_warmup; ++w) { REQUIRE(run_quant_module(compute, ctx, root, plan, seeds, n_seeds, out_val, got.data(), out_len)); }
        double ms[n_timed] = {};
        for (crd::u32 t = 0; t < n_timed; ++t)
        {
            float* const dst = (c == 0U && t == 0U) ? ref.data() : got.data(); // capture the default output ONCE (t==0), rest reuse `got`
            REQUIRE(run_quant_module(compute, ctx, root, plan, seeds, n_seeds, out_val, dst, out_len));
            ms[t] = compute.last_gpu_ms();
        }
        medians_out[c] = median_of(ms, n_timed);
        REQUIRE(medians_out[c] > 0.0); // ⛔ median==0 ⇒ no device timestamps ⇒ the measurer is measuring NOTHING (hard fail, not skip)
        if (c != 0U) // the FREE oracle-gate: every config bit-EXACT to the default (share-inert or not, values are identical)
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

    const crd::u64 ph = ce::tune::program_hash(ctx, payload, root); // key by the POST-EXPANSION program the planner consumed
    ce::Module* const emit = ctx.create_module();
    ce::Block*        eb   = emit->body()->first_block();
    if (eb == nullptr) { eb = ctx.create_block(0U); emit->body()->append(eb); }
    ce::Operation* const e = ce::tune::build_entry(ctx, ctx.attr_string(device), ctx.attr_string(env),
                                                   ctx.attr_int(static_cast<crd::i64>(ph)), ctx.attr_string(shape),
                                                   ctx.attr_bool(cfgs[wi].fuse_gemm_relu), ctx.attr_bool(cfgs[wi].share_intermediate_storage));
    eb->append(e);
    return emit;
}
} // namespace

TEST_CASE("ceir 22c-2: the sec-137 GEMM->FFT->reduction pipeline runs device-resident on Vulkan (one submit, no CPU round-trip)",
          "[ceir][tensor-pipeline][gpu]")
{
    // ── build the asset module: gemm(A,B,C)[8,8] -> reshape[64] -> fft(.,0) -> reduce(sum) ──
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
    ce::Block* const  b   = ce::func::func_body_block(f);
    const ce::OpId    dcl = ctx.intern_op("resource", "declare");
    const auto        mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
    ce::Value* const  a_in = mkd(tf(ctx, sh2(ctx, kSide, kSide)));
    ce::Value* const  b_in = mkd(tf(ctx, sh2(ctx, kSide, kSide)));
    ce::Value* const  c_in = mkd(tf(ctx, sh2(ctx, kSide, kSide)));
    ce::Operation* const gm = ce::linalg::build_gemm(ctx, a_in, b_in, c_in, ctx.attr_float(1.0), ctx.attr_float(0.0), ctx.attr_bool(false),
                                                     ctx.attr_bool(false), tf(ctx, sh2(ctx, kSide, kSide)));
    b->append(gm);
    ce::Operation* const rs = ce::tensor::build_reshape(ctx, gm->result(0U), tf(ctx, sh1(ctx, kL)));
    b->append(rs);
    ce::Value* const  im0 = mkd(tf(ctx, sh1(ctx, kL)));
    ce::Value*        ffo[2] = {rs->result(0U), im0};
    ce::Operation* const ff = ctx.create_operation(ctx.intern_op("tensor", "fft"), crd::containers::ConstSpan<ce::Value*>(ffo, 2U),
                                                   2U, tf(ctx, sh1(ctx, kL)));
    ctx.set_attr(ff, crd::containers::StringView("direction"), ctx.attr_string(crd::containers::StringView("forward")));
    ctx.set_attr(ff, crd::containers::StringView("axis"), ctx.attr_int(0));
    b->append(ff);
    ce::Operation* const rd = ce::tensor::build_reduce(ctx, ff->result(0U), ctx.attr_int(0), ctx.attr_string(crd::containers::StringView("sum")),
                                                       tf(ctx, shp(ctx, crd::containers::ConstSpan<ce::TypeId>{})));
    b->append(rd);

    // ── PLAN (device-free, ALWAYS runs — the all-skip guard) ──
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 3U);

    // ── the INDEPENDENT composed reference (triple-loop GEMM -> naive DFT -> serial sum, f64) ──
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
    crd::f64 dref[kL];
    for (int i = 0; i < kSide; ++i)
    {
        for (int j = 0; j < kSide; ++j)
        {
            crd::f64 acc = 0.0;
            for (int k = 0; k < kSide; ++k) { acc += static_cast<crd::f64>(a_data[i * kSide + k]) * static_cast<crd::f64>(b_data[k * kSide + j]); }
            dref[i * kSide + j] = acc; // D flattened row-major == E
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
        maxmag = maxmag > am ? maxmag : am;
    }

    // ── DEVICE (soft-skip with no adapter) ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-22c pipeline gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    // ── MATERIALIZE the plan's buffers: Alias shares its target; ExternalIn = CpuToGpu (seed per FillKind); Intermediate =
    //    GpuOnly (device-resident, never host-touched); Output = GpuToCpu (read back once). ──
    const crd::usize nb = plan.buffers.size();
    std::unique_ptr<crd::gpu::ComputeBuffer> owned[32];
    crd::gpu::ComputeBuffer*                 bufs[32] = {};
    REQUIRE(nb <= 32U);
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.role == ceg::BufferRole::Alias)
        {
            bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; // zero-copy: SHARE the realized buffer
            continue;
        }
        crd::gpu::ComputeMemory mem = crd::gpu::ComputeMemory::GpuOnly; // Intermediate = device-resident (no host round-trip)
        if (pb.role == ceg::BufferRole::ExternalIn) { mem = crd::gpu::ComputeMemory::CpuToGpu; }
        else if (pb.role == ceg::BufferRole::Output) { mem = crd::gpu::ComputeMemory::GpuToCpu; }
        owned[i] = compute.create_buffer(pb.bytes, crd::gpu::compute_usage::storage, mem);
        REQUIRE(owned[i] != nullptr);
        bufs[i] = owned[i].get();
        if (pb.role == ceg::BufferRole::ExternalIn)
        {
            auto* dst = static_cast<float*>(owned[i]->map());
            REQUIRE(dst != nullptr);
            const crd::u64 cnt = pb.bytes / 4ULL;
            if (pb.value == a_in) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = a_data[e]; } }
            else if (pb.value == b_in) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = b_data[e]; } }
            else { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = 0.0F; } } // C (unused, β=0), im0 (Zeros), and twiddles (filled below)
            owned[i]->unmap();
        }
    }
    // fft twiddles: the fft stage's bind[2]=tw_re (cos), bind[3]=tw_im (-sin); n derived from tw bytes (n/2 entries).
    for (crd::usize s = 0; s < plan.stages.size(); ++s)
    {
        const ceg::PlanStage& st = plan.stages[s];
        if (st.kind != ceg::StageKind::Fft) { continue; }
        const crd::i32 btr = st.bind[2];
        const crd::i32 bti = st.bind[3];
        const int      half = static_cast<int>(plan.buffers[static_cast<crd::usize>(btr)].bytes / 4ULL);
        const int      n    = half * 2;
        auto*          tr   = static_cast<float*>(owned[static_cast<crd::usize>(btr)]->map());
        auto*          ti   = static_cast<float*>(owned[static_cast<crd::usize>(bti)]->map());
        REQUIRE(tr != nullptr);
        REQUIRE(ti != nullptr);
        for (int kk = 0; kk < half; ++kk)
        {
            const crd::f64 a = two_pi * static_cast<crd::f64>(kk) / static_cast<crd::f64>(n);
            tr[kk] = static_cast<float>(crd::math::cos(a));
            ti[kk] = static_cast<float>(-crd::math::sin(a));
        }
        owned[static_cast<crd::usize>(btr)]->unmap();
        owned[static_cast<crd::usize>(bti)]->unmap();
    }

    // ── RECORD + submit: ONE begin/submit — execute_tensor_pipeline records the 3 stages + inter-stage barriers; the gate adds
    //    the final ShaderWrite->HostRead on the Output, then reads it back ONCE (no round-trip between stages). ──
    Resolver res;
    res.alloc_ctx = &ctx;
    res.alloc     = &root;
    res.compute   = &compute;
    crd::i32 out_idx = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].role == ceg::BufferRole::Output) { out_idx = static_cast<crd::i32>(i); } }
    REQUIRE(out_idx >= 0);

    auto& rec = compute.begin();
    const ceg::ExecuteError ee = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res,
                                                              crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(bufs, nb));
    REQUIRE(ee == ceg::ExecuteError::None);
    rec.barrier(*bufs[static_cast<crd::usize>(out_idx)], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
    compute.submit_and_wait();

    const auto* got = static_cast<const float*>(owned[static_cast<crd::usize>(out_idx)]->map());
    REQUIRE(got != nullptr);
    const float device_s = got[0];
    owned[static_cast<crd::usize>(out_idx)]->unmap();

    // ── COMPARE within a DERIVED, DECLARED tolerance. The gemm+reshape are bit-exact (small-int data); the fft is a float
    //    butterfly (per-bin error ≤ 2e-3·maxmag, the FFT-kernel precedent); the reduce SUMS kL bins ⇒ the error PROPAGATES to
    //    ≤ kL · 2e-3 · maxmag. NOT a tuned epsilon — the propagated bound. ──
    const float tol = static_cast<float>(static_cast<crd::f64>(kL) * 2e-3 * maxmag);
    const float fa  = device_s - static_cast<float>(s_ref);
    CHECK((fa < 0.0F ? -fa : fa) <= tol);
}

TEST_CASE("ceir 22c-3d: the PARSE-LOADED design-B pipeline (gemm->fft->mag->reduce(max)->normalize) runs on Vulkan vs a composed ref",
          "[ceir][tensor-pipeline][gpu]")
{
    // ── register the dialects + PARSE-LOAD the authored asset (the FILE is the pipeline — no C++ builder) ──
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::tensor::register_dialect(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);

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
    ce::Module* const m = pr.module;

    // ── PLAN (device-free, ALWAYS runs — the all-skip guard) ──
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 5U);
    REQUIRE(plan.stages[0].kind == ceg::StageKind::Gemm);
    REQUIRE(plan.stages[2].kind == ceg::StageKind::VizDispatch);
    REQUIRE(plan.stages[4].kind == ceg::StageKind::VizDispatch);

    // ── the INDEPENDENT composed f64 reference: triple-loop gemm -> naive DFT (re+im) -> magnitude -> max -> normalize ──
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
            e_ref[i * kSide + j] = acc; // D flattened row-major == the fft input
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
            fi -= e_ref[l] * crd::math::sin(ang); // forward DFT: X[k] = Σ x[n]·(cos - i·sin)
        }
        mag_ref[k] = crd::math::sqrt(fr * fr + fi * fi);
        mx_ref     = mx_ref > mag_ref[k] ? mx_ref : mag_ref[k];
    }
    crd::f64 norm_ref[kL];
    for (int k = 0; k < kL; ++k) { norm_ref[k] = mag_ref[k] / mx_ref; }

    // ── DEVICE (soft-skip with no adapter) ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-22c-3d pipeline gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    // ── MATERIALIZE the plan's buffers. ⛔ the module is PARSED (no C++ Value handles) — A/B are identified by the GEMM stage's
    //    binds; mag/mx/norm are Intermediate/Output (device-resident, never seeded). ──
    const crd::i32   a_buf = plan.stages[0].bind[0];
    const crd::i32   b_buf = plan.stages[0].bind[1];
    const crd::usize nb    = plan.buffers.size();
    std::unique_ptr<crd::gpu::ComputeBuffer> owned[40];
    crd::gpu::ComputeBuffer*                 bufs[40] = {};
    REQUIRE(nb <= 40U);
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.role == ceg::BufferRole::Alias) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        crd::gpu::ComputeMemory mem = crd::gpu::ComputeMemory::GpuOnly;
        if (pb.role == ceg::BufferRole::ExternalIn) { mem = crd::gpu::ComputeMemory::CpuToGpu; }
        else if (pb.role == ceg::BufferRole::Output) { mem = crd::gpu::ComputeMemory::GpuToCpu; }
        owned[i] = compute.create_buffer(pb.bytes, crd::gpu::compute_usage::storage, mem);
        REQUIRE(owned[i] != nullptr);
        bufs[i] = owned[i].get();
        if (pb.role == ceg::BufferRole::ExternalIn)
        {
            auto* dst = static_cast<float*>(owned[i]->map());
            REQUIRE(dst != nullptr);
            const crd::u64 cnt = pb.bytes / 4ULL;
            if (static_cast<crd::i32>(i) == a_buf) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = a_data[e]; } }
            else if (static_cast<crd::i32>(i) == b_buf) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = b_data[e]; } }
            else { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = 0.0F; } } // C (unused, β=0), im0 (Zeros), twiddles (filled below)
            owned[i]->unmap();
        }
    }
    // fft twiddles (the fft stage's bind[2]=cos, bind[3]=-sin; n from the tw buffer bytes)
    for (crd::usize s = 0; s < plan.stages.size(); ++s)
    {
        const ceg::PlanStage& st = plan.stages[s];
        if (st.kind != ceg::StageKind::Fft) { continue; }
        const crd::i32 btr  = st.bind[2];
        const crd::i32 bti  = st.bind[3];
        const int      half = static_cast<int>(plan.buffers[static_cast<crd::usize>(btr)].bytes / 4ULL);
        const int      n    = half * 2;
        auto*          tr   = static_cast<float*>(owned[static_cast<crd::usize>(btr)]->map());
        auto*          ti   = static_cast<float*>(owned[static_cast<crd::usize>(bti)]->map());
        REQUIRE(tr != nullptr);
        REQUIRE(ti != nullptr);
        for (int kk = 0; kk < half; ++kk)
        {
            const crd::f64 a = two_pi * static_cast<crd::f64>(kk) / static_cast<crd::f64>(n);
            tr[kk] = static_cast<float>(crd::math::cos(a));
            ti[kk] = static_cast<float>(-crd::math::sin(a));
        }
        owned[static_cast<crd::usize>(btr)]->unmap();
        owned[static_cast<crd::usize>(bti)]->unmap();
    }

    // ── RECORD + submit ONE pass (2 CKIR dispatches interleaved with the graph-tier gemm/reduce). ──
    Resolver res;
    res.alloc_ctx = &ctx;
    res.alloc     = &root;
    res.compute   = &compute;
    crd::i32 out_idx = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].role == ceg::BufferRole::Output) { out_idx = static_cast<crd::i32>(i); } }
    REQUIRE(out_idx >= 0);
    REQUIRE(plan.buffers[static_cast<crd::usize>(out_idx)].bytes == static_cast<crd::u64>(kL) * 4ULL); // norm is the [64] spectrum

    ceg::TensorPipelineProfile profile(&root); // §137 profiling: one structural row per stage (record-time)
    auto&                      rec = compute.begin();
    const ceg::ExecuteError    ee  = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res,
                                                                  crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(bufs, nb),
                                                                  &profile);
    REQUIRE(ee == ceg::ExecuteError::None);
    rec.barrier(*bufs[static_cast<crd::usize>(out_idx)], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
    const auto t0 = std::chrono::steady_clock::now(); // CALLER-stamped total submit wall-time (the executor never times)
    compute.submit_and_wait();
    const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();

    const auto* got = static_cast<const float*>(owned[static_cast<crd::usize>(out_idx)]->map());
    REQUIRE(got != nullptr);

    // ── COMPARE the normalized spectrum within a DERIVED tolerance. Per-bin fft error ≤ 2e-3·maxmag (the FFT-kernel precedent);
    //    magnitude scales it by ≤ √2 (sqrt of the re/im errors); normalize (÷mx, mx≈maxmag) doubles it (the quotient rule) ⇒
    //    |Δnorm| ≤ 2·√2·2e-3 ≈ 5.7e-3. NOT a tuned epsilon — the propagated bound. ──
    const float tol       = static_cast<float>(2.0 * 1.41421356 * 2e-3);
    int         worst     = -1;
    float       worst_err = 0.0F;
    for (int k = 0; k < kL; ++k)
    {
        const float d = got[k] - static_cast<float>(norm_ref[k]);
        const float e = d < 0.0F ? -d : d;
        if (e > worst_err) { worst_err = e; worst = k; }
    }
    owned[static_cast<crd::usize>(out_idx)]->unmap();
    INFO("worst bin " << worst << " err " << worst_err << " tol " << tol);
    CHECK(worst_err <= tol);

    // ── §137 PROFILE (22c-3g): 5 coherent per-stage rows + a positive submit wall-time. ⛔ soft-perf — NO time threshold (a
    //    threshold would be flaky + drag in median-of-five); the gate proves the instrument measured THIS pipeline (row/byte
    //    coherence), not just that the API ran (the empty-frame scar). ──
    REQUIRE(profile.stages.size() == 5U);
    for (crd::usize s = 0; s < profile.stages.size(); ++s)
    {
        const ceg::TensorStageProfile& sp = profile.stages[s];
        CHECK(sp.kind == plan.stages[s].kind);
        CHECK(sp.workgroups == static_cast<crd::u64>(sp.gx) * sp.gy * sp.gz);
        CHECK(sp.workgroups >= 1U);
        crd::u64 bind_sum = 0;
        for (crd::u32 i = 0; i < plan.stages[s].nbind; ++i)
        {
            bind_sum += plan.buffers[static_cast<crd::usize>(plan.stages[s].bind[i])].bytes;
        }
        CHECK(sp.bytes_in + sp.bytes_out == bind_sum); // in+out partitions the stage's binds — measured THIS plan
        CHECK(sp.bytes_out > 0U);                       // every stage produces something
    }
    CHECK(profile.stages[0].bytes_out == static_cast<crd::u64>(kL) * 4ULL); // gemm D = [64] f32 = 256 B
    CHECK(profile.stages[4].bytes_out == static_cast<crd::u64>(kL) * 4ULL); // normalize norm = [64] f32 = 256 B
    CHECK(wall_ns > 0);                                                     // the submit was actually clocked
}

TEST_CASE("ceir 22c-3d: per-stage oracle + determinism (each device stage matches the independent ref; readback doesn't perturb)",
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
    std::ifstream af(CRD_REPO_DIR "/assets/ceir/tensor_pipeline.ceir", std::ios::binary | std::ios::ate);
    REQUIRE(af.good());
    const std::streamsize asz = af.tellg();
    af.seekg(0);
    crd::containers::Array<char> asrc(&root);
    asrc.resize(static_cast<crd::usize>(asz), '\0');
    af.read(asrc.data(), asz);
    const ce::ParseResult pr = ce::parse(ctx, crd::containers::StringView(asrc.data(), static_cast<crd::usize>(asz)));
    REQUIRE(pr.ok);
    ce::Module* const             m    = pr.module;
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 5U);

    // ── the INDEPENDENT composed f64 reference, kept PER STAGE (D, Fr, Fi, mag, mx, norm) ──
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
    crd::f64           fr_ref[kL];
    crd::f64           fi_ref[kL];
    crd::f64           mag_ref[kL];
    crd::f64           mx_ref  = 0.0;
    crd::f64           sig_max = 1e-6; // max |Fr|,|Fi| over bins — the fft-error scale
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
        fr_ref[k]           = fr;
        fi_ref[k]           = fi;
        mag_ref[k]          = crd::math::sqrt(fr * fr + fi * fi);
        mx_ref              = mx_ref > mag_ref[k] ? mx_ref : mag_ref[k];
        const crd::f64 afr  = fr < 0.0 ? -fr : fr;
        const crd::f64 afi  = fi < 0.0 ? -fi : fi;
        sig_max             = sig_max > afr ? sig_max : afr;
        sig_max             = sig_max > afi ? sig_max : afi;
    }
    crd::f64 norm_ref[kL];
    for (int k = 0; k < kL; ++k) { norm_ref[k] = mag_ref[k] / mx_ref; }

    // ── DEVICE (soft-skip) ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-22c-3d per-stage gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    const crd::i32   a_buf    = plan.stages[0].bind[0];
    const crd::i32   b_buf    = plan.stages[0].bind[1];
    const crd::i32   d_buf    = plan.stages[0].bind[2]; // gemm output
    const crd::i32   fr_buf   = plan.stages[1].bind[4]; // fft out_re
    const crd::i32   fi_buf   = plan.stages[1].bind[5]; // fft out_im
    const crd::i32   mag_buf  = plan.stages[2].bind[2]; // magnitude dispatch output
    const crd::i32   mx_buf   = plan.stages[3].bind[1]; // reduce(max) output (rank-0)
    crd::i32         norm_buf = -1;
    const crd::usize nb       = plan.buffers.size();
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].role == ceg::BufferRole::Output) { norm_buf = static_cast<crd::i32>(i); } }
    REQUIRE(norm_buf >= 0);
    REQUIRE(nb <= 40U);

    // read a device buffer's first `count` floats into `out`.
    const auto readback = [](std::unique_ptr<crd::gpu::ComputeBuffer>& buf, float* out, int count) {
        const auto* p = static_cast<const float*>(buf->map());
        REQUIRE(p != nullptr);
        for (int e = 0; e < count; ++e) { out[e] = p[e]; }
        buf->unmap();
    };

    // materialize + run one pass. `readable` ⇒ intermediates are GpuToCpu (so the host can read every stage's output — the
    // INSTRUMENTED run). Reads the terminal norm into norm_out; if `readable`, also reads D/Fr/Fi/mag/mx into their arrays.
    const auto run_once = [&](bool readable, float* norm_out, float* d_out, float* fr_out, float* fi_out, float* mag_out,
                              float* mx_out) {
        std::unique_ptr<crd::gpu::ComputeBuffer> owned[40];
        crd::gpu::ComputeBuffer*                 bufs[40] = {};
        for (crd::usize i = 0; i < nb; ++i)
        {
            const ceg::PlanBuffer& pb = plan.buffers[i];
            if (pb.role == ceg::BufferRole::Alias) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
            crd::gpu::ComputeMemory mem = crd::gpu::ComputeMemory::GpuOnly;
            if (pb.role == ceg::BufferRole::ExternalIn) { mem = crd::gpu::ComputeMemory::CpuToGpu; }
            // Output is host-read; an Intermediate is host-read too ONLY on the instrumented run (else device-resident GpuOnly).
            else if (pb.role == ceg::BufferRole::Output || readable) { mem = crd::gpu::ComputeMemory::GpuToCpu; }
            owned[i] = compute.create_buffer(pb.bytes, crd::gpu::compute_usage::storage, mem);
            REQUIRE(owned[i] != nullptr);
            bufs[i] = owned[i].get();
            if (pb.role == ceg::BufferRole::ExternalIn)
            {
                auto* dst = static_cast<float*>(owned[i]->map());
                REQUIRE(dst != nullptr);
                const crd::u64 cnt = pb.bytes / 4ULL;
                if (static_cast<crd::i32>(i) == a_buf) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = a_data[e]; } }
                else if (static_cast<crd::i32>(i) == b_buf) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = b_data[e]; } }
                else { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = 0.0F; } }
                owned[i]->unmap();
            }
        }
        for (crd::usize s = 0; s < plan.stages.size(); ++s)
        {
            const ceg::PlanStage& st = plan.stages[s];
            if (st.kind != ceg::StageKind::Fft) { continue; }
            const int half = static_cast<int>(plan.buffers[static_cast<crd::usize>(st.bind[2])].bytes / 4ULL);
            const int n    = half * 2;
            auto*     tr   = static_cast<float*>(owned[static_cast<crd::usize>(st.bind[2])]->map());
            auto*     ti   = static_cast<float*>(owned[static_cast<crd::usize>(st.bind[3])]->map());
            for (int kk = 0; kk < half; ++kk)
            {
                const crd::f64 a = two_pi * static_cast<crd::f64>(kk) / static_cast<crd::f64>(n);
                tr[kk] = static_cast<float>(crd::math::cos(a));
                ti[kk] = static_cast<float>(-crd::math::sin(a));
            }
            owned[static_cast<crd::usize>(st.bind[2])]->unmap();
            owned[static_cast<crd::usize>(st.bind[3])]->unmap();
        }

        Resolver res;
        res.alloc_ctx = &ctx;
        res.alloc     = &root;
        res.compute   = &compute;
        auto&                   rec = compute.begin();
        const ceg::ExecuteError ee  = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res,
                                                                   crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(bufs, nb));
        REQUIRE(ee == ceg::ExecuteError::None);
        rec.barrier(*bufs[static_cast<crd::usize>(norm_buf)], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
        if (readable)
        {
            const crd::i32 rd[5] = {d_buf, fr_buf, fi_buf, mag_buf, mx_buf};
            for (const crd::i32 bi : rd)
            {
                rec.barrier(*bufs[static_cast<crd::usize>(bi)], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
            }
        }
        compute.submit_and_wait();

        readback(owned[static_cast<crd::usize>(norm_buf)], norm_out, kL);
        if (readable)
        {
            readback(owned[static_cast<crd::usize>(d_buf)], d_out, kL);
            readback(owned[static_cast<crd::usize>(fr_buf)], fr_out, kL);
            readback(owned[static_cast<crd::usize>(fi_buf)], fi_out, kL);
            readback(owned[static_cast<crd::usize>(mag_buf)], mag_out, kL);
            readback(owned[static_cast<crd::usize>(mx_buf)], mx_out, 1);
        }
    };

    float norm_a[kL] = {};
    float norm_b[kL] = {};
    float d_dev[kL]  = {};
    float fr_dev[kL] = {};
    float fi_dev[kL] = {};
    float mag_dev[kL] = {};
    float mx_dev[1]  = {};
    run_once(false, norm_a, nullptr, nullptr, nullptr, nullptr, nullptr); // UNINSTRUMENTED (device-resident intermediates)
    run_once(true, norm_b, d_dev, fr_dev, fi_dev, mag_dev, mx_dev);       // INSTRUMENTED (readable intermediates)

    // ── (a) DETERMINISM: making the intermediates host-readable did NOT perturb the terminal (bit-exact, catches nondeterminism) ──
    for (int k = 0; k < kL; ++k) { CHECK(norm_a[k] == norm_b[k]); }

    // ── (b) PER-STAGE vs the INDEPENDENT ref (DERIVED tolerances). gemm: small-int products, f32-exact. fft: ≤ 2e-3·sig_max
    //    per bin. magnitude: ≤ √2·(that). reduce(max): a bin selection, ≤ the magnitude bound. normalize: the 5.7e-3 quotient. ──
    const auto absf = [](float x) { return x < 0.0F ? -x : x; };
    const float tol_gemm = 1e-3F;
    const float tol_fft  = static_cast<float>(2e-3 * sig_max);
    const float tol_mag  = static_cast<float>(1.41421356 * 2e-3 * mx_ref);
    const float tol_norm = static_cast<float>(2.0 * 1.41421356 * 2e-3);
    for (int k = 0; k < kL; ++k)
    {
        CHECK(absf(d_dev[k] - static_cast<float>(e_ref[k])) <= tol_gemm);   // stage 0: gemm
        CHECK(absf(fr_dev[k] - static_cast<float>(fr_ref[k])) <= tol_fft);  // stage 1: fft re
        CHECK(absf(fi_dev[k] - static_cast<float>(fi_ref[k])) <= tol_fft);  // stage 1: fft im
        CHECK(absf(mag_dev[k] - static_cast<float>(mag_ref[k])) <= tol_mag); // stage 2: magnitude (CKIR dispatch)
        CHECK(absf(norm_b[k] - static_cast<float>(norm_ref[k])) <= tol_norm); // stage 4: normalize (CKIR dispatch)
    }
    CHECK(absf(mx_dev[0] - static_cast<float>(mx_ref)) <= tol_mag); // stage 3: reduce(max)
}

TEST_CASE("ceir 23b-2d: the FUSED QuantGemm collapse (dequant-inline gemm) runs on Vulkan == the unfused Dequant+Gemm == oracle",
          "[ceir][tensor-pipeline][gpu][quant]")
{
    // ── register dialects (incl. quant) ──
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::tensor::register_dialect(ctx);
    (void)ce::quant::register_dialect(ctx);

    const auto absf = [](float x) { return x < 0.0F ? -x : x; };

    constexpr crd::u32 rows  = 4U; // M — the fused kernel's baked dims (M*N=32 threads, K*N=64 packed weights)
    constexpr crd::u32 inner = 8U; // K
    constexpr crd::u32 cols  = 8U; // N

    struct QMod
    {
        ce::Module* m   = nullptr;
        ce::Value*  a   = nullptr;
        ce::Value*  wq  = nullptr;
        ce::Value*  sc  = nullptr;
        ce::Value*  out = nullptr;
    };
    // Build A[M,K] · dequant(W_q8[K,N], scale, zp, symmetric) → W_dq[K,N] · gemm(A, W_dq, C)[M,N]. `unfused` adds a 2nd gemm on
    // the SAME W_dq → its result is multi-use → NO fusion (Dequant + Gemm + Gemm); the 2nd (final) gemm's D is the read-back Output.
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
            out_op = g2; // the SECOND gemm makes W_dq multi-use → the plan does NOT fuse; g2 is the final stage (Output)
        }
        return {m, a, wq, sc, out_op->result(0U)};
    };

    const QMod fused_mod   = build(false);
    const QMod unfused_mod = build(true);

    // ── PLAN both (device-free, ALWAYS runs — the all-skip guard) ──
    const ceg::TensorPipelinePlan plan_f = ceg::plan_tensor_pipeline(ctx, *fused_mod.m, &root);
    REQUIRE(plan_f.reject == ceg::PlanReject::None);
    REQUIRE(plan_f.stages.size() == 1U); // the 2→1 collapse
    REQUIRE(plan_f.stages[0].kind == ceg::StageKind::QuantGemm);
    const ceg::TensorPipelinePlan plan_u = ceg::plan_tensor_pipeline(ctx, *unfused_mod.m, &root);
    REQUIRE(plan_u.reject == ceg::PlanReject::None);
    REQUIRE(plan_u.stages.size() == 3U); // Dequant + Gemm + Gemm (unfused)
    REQUIRE(plan_u.stages[0].kind == ceg::StageKind::Dequant);
    REQUIRE(plan_u.stages[2].kind == ceg::StageKind::Gemm);

    // ── data (A as 0.25-multiples · int8 weights → every f32 intermediate is EXACT: the device result is bit-exact to the ref) ──
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

    // ── DEVICE (soft-skip with no adapter) ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-23b-2d fused QuantGemm gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    float d_fused[rows * cols]   = {};
    float d_unfused[rows * cols] = {};
    const QuantSeed seeds_f[3]   = {{fused_mod.a, a_data, nullptr, rows * inner},
                                    {fused_mod.wq, nullptr, packed, inner * cols / 4U},
                                    {fused_mod.sc, &scale, nullptr, 1U}};
    REQUIRE(run_quant_module(compute, ctx, &root, plan_f, seeds_f, 3U, fused_mod.out, d_fused, rows * cols));
    const QuantSeed seeds_u[3] = {{unfused_mod.a, a_data, nullptr, rows * inner},
                                  {unfused_mod.wq, nullptr, packed, inner * cols / 4U},
                                  {unfused_mod.sc, &scale, nullptr, 1U}};
    REQUIRE(run_quant_module(compute, ctx, &root, plan_u, seeds_u, 3U, unfused_mod.out, d_unfused, rows * cols));

    for (crd::u32 i = 0; i < rows * cols; ++i)
    {
        const float tol = 1e-4F * (1.0F + absf(oracle[i]));
        CHECK(absf(d_fused[i] - oracle[i]) <= tol);    // the FUSED QuantGemm kernel is correct on device
        CHECK(absf(d_unfused[i] - oracle[i]) <= tol);  // the unfused Dequant+Gemm is correct (the folded 2a sym-dequant debt)
        CHECK(absf(d_fused[i] - d_unfused[i]) <= tol); // the fusion PRESERVES the gemm semantics
    }
}

TEST_CASE("ceir 23c-c: the PARSE-LOADED quant-MLP (QuantGemm->relu->QuantGemm) runs on Vulkan vs a float MLP oracle",
          "[ceir][tensor-pipeline][gpu][quant]")
{
    // ── register dialects (incl. quant + arith + compute) + PARSE-LOAD the crown asset (the FILE is the MLP — no C++ builder) ──
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
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
    crd::containers::Array<char> asrc(&root);
    asrc.resize(static_cast<crd::usize>(asz), '\0');
    af.read(asrc.data(), asz);
    const ce::ParseResult pr = ce::parse(ctx, crd::containers::StringView(asrc.data(), static_cast<crd::usize>(asz)));
    REQUIRE(pr.ok);
    REQUIRE(pr.module != nullptr);

    // ── PLAN (device-free, ALWAYS runs) → QuantGemm(layer1) · VizDispatch(relu) · QuantGemm(layer2) ──
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *pr.module, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 3U);
    REQUIRE(plan.stages[0].kind == ceg::StageKind::QuantGemm);
    REQUIRE(plan.stages[1].kind == ceg::StageKind::VizDispatch);
    REQUIRE(plan.stages[2].kind == ceg::StageKind::QuantGemm);

    // ── the input Values are identified via the STAGE BINDS (the module is PARSED — no C++ Value handles). QuantGemm binds are
    //    {A, W_q8, scale, D}; layer-1's A = x, layer-2's A = the relu output (device-resident, not seeded). ──
    const auto val = [&](crd::u32 stage, crd::u32 b) { return plan.buffers[static_cast<crd::usize>(plan.stages[stage].bind[b])].value; };
    const ce::Value* const x_val  = val(0U, 0U);
    const ce::Value* const w1_val = val(0U, 1U);
    const ce::Value* const s1_val = val(0U, 2U);
    const ce::Value* const w2_val = val(2U, 1U);
    const ce::Value* const s2_val = val(2U, 2U);
    const ce::Value* const out_val = val(2U, 3U); // layer-2 D — the terminal Output

    constexpr crd::u32 rows  = 4U;
    constexpr crd::u32 inner = 8U;
    constexpr crd::u32 cols  = 8U;
    // data (0.25-multiple x · int8 weights · power-of-2 scales → every f32 intermediate across BOTH layers is EXACT).
    float x[rows * inner];
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
    // float MLP oracle: h1 = relu(scale1 · x·int8(W1)) ; out = scale2 · h1·int8(W2).
    float h1[rows * cols];
    float oracle[rows * cols];
    for (crd::u32 m = 0; m < rows; ++m)
    {
        for (crd::u32 n = 0; n < cols; ++n)
        {
            float acc = 0.0F;
            for (crd::u32 kk = 0; kk < inner; ++kk) { acc += x[m * inner + kk] * static_cast<float>(wq1[kk * cols + n]); }
            const float v = acc * s1;
            h1[m * cols + n] = v > 0.0F ? v : 0.0F; // relu
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

    // ── DEVICE (soft-skip with no adapter) ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-23c-c quant-MLP gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    const QuantSeed seeds[5] = {{x_val, x, nullptr, rows * inner}, {w1_val, nullptr, p1, inner * cols / 4U},
                                {s1_val, &s1, nullptr, 1U},        {w2_val, nullptr, p2, inner * cols / 4U},
                                {s2_val, &s2, nullptr, 1U}};
    float d_out[rows * cols] = {};
    REQUIRE(run_quant_module(compute, ctx, &root, plan, seeds, 5U, out_val, d_out, rows * cols));

    for (crd::u32 i = 0; i < rows * cols; ++i)
    {
        CHECK(absf(d_out[i] - oracle[i]) <= 1e-4F * (1.0F + absf(oracle[i]))); // the composed 2-layer quant MLP == the float MLP
    }
}

// CEIR-24b-4 — the §138 ML PROOF headline: an authored ml.attention (single-head SDPA) EXPANDED by expand_ml_ops into the proven
// CEIR-22/23 primitive pipeline (synth transpose -> gemm(Q,Kt) -> softmax kernel -> gemm(.,V); 26d-3a/3b retired the baked ckir) runs DEVICE-RESIDENT on a real Vulkan
// device in ONE submit, and matches the CPU scaled-dot-product-attention oracle. The composite op decomposes into proven primitives
// that execute on-device — NO new StageKinds, the same execute_tensor_pipeline the §137 pipeline uses. scale = 1/√D is a caller-
// uploaded 1-element buffer (found in the plan as the sole 1-element ExternalIn). Proof dims Sq=2, Sk=3, D=4, Dv=2 (the transpose/
// softmax kernels are baked to these). ⛔ exp is float math -> a DERIVED tol.
TEST_CASE("ceir 24b-4: an expanded ml.attention runs device-resident on Vulkan (transpose+gemm+softmax+gemm vs the CPU SDPA oracle)",
          "[ceir][ml][gpu]")
{
    constexpr crd::u32 sq = 2; // query positions
    constexpr crd::u32 sk = 3; // key positions
    constexpr crd::u32 dd = 4; // head dim D
    constexpr crd::u32 dv = 2; // value dim Dv

    // ── build the asset module: ml.attention(Q[Sq,D], K[Sk,D], V[Sk,Dv]) -> out[Sq,Dv] ──
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);     // the expansion's dispatch grid consts
    (void)ce::compute::register_compute_ops(ctx); // the expansion's transpose/softmax dispatches
    (void)ce::linalg::register_dialect(ctx);      // the expansion's gemms
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

    // ── EXPAND the composite op into the 22/23 vocab, then PLAN (device-free, ALWAYS runs) ──
    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, *m);
    REQUIRE(er.error == ceg::MlExpandError::None);
    REQUIRE(er.expanded == 1U);
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);

    // The output = the plan's sole Output buffer; the scale = the sole 1-element ExternalIn (the expansion created it internally).
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

    // ── CPU SDPA oracle: out = softmax(Q·Kᵀ / √D) · V ──
    const float q_in[sq * dd] = {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F};
    const float k_in[sk * dd] = {0.2F, 0.1F, 0.0F, 0.3F, 0.4F, 0.5F, 0.6F, 0.1F, 0.7F, 0.2F, 0.3F, 0.9F};
    const float v_in[sk * dv] = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
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

    // ── DEVICE (soft-skip with no adapter) ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-24b-4 attention gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    const QuantSeed seeds[4] = {{q_val, q_in, nullptr, sq * dd},
                                {k_val, k_in, nullptr, sk * dd},
                                {v_val, v_in, nullptr, sk * dv},
                                {scale_val, &inv_sqrt_d, nullptr, 1U}};
    float d_out[sq * dv] = {};
    REQUIRE(run_quant_module(compute, ctx, &root, plan, seeds, 4U, out_val, d_out, sq * dv));

    for (crd::u32 i = 0; i < sq * dv; ++i)
    {
        CHECK(crd::math::abs(d_out[i] - oracle[i]) <= 1e-4F * (1.0F + crd::math::abs(oracle[i]))); // expanded SDPA == CPU attention (derived tol)
    }
}

// CEIR-26d-3c — the ATTENTION GENERALIZATION CAPSTONE (Vulkan): the SAME ml.attention now runs at GENERIC dims (Sq=3, Sk=5, D=4,
// Dv=2) — dims the 24z baked-kernel pre-check REJECTED (transpose.ckir baked Sk=3,D=4 + softmax.ckir baked Sq=2,Sk=3). 26d-3a made
// the transpose a shape-generic tensor.transpose (synth) + 26d-3b made softmax a spec-const loop kernel (local_size←Sq, Sk←spec-
// const), so the composite EXPANDS→PLANS→runs at any (Sq,Sk,Dv). ⛔ D is HELD at 4: scale=1/√D is caller-computed and =0.5 for D=4;
// a D-generic gate would recompute 1/√D (trivial — name-forward). NO "raw" for a previously-REJECTED program (24b device-vs-oracle
// standard). This is the POSITIVE gate the retired pre-check's teeth transfer to (the test_expand_ml Sk=4 reject is superseded in
// the same slice). ⛔ exp is float math → a DERIVED tol.
TEST_CASE("ceir 26d-3c: a GENERIC-dims ml.attention (Sq=3, Sk=5, D=4) runs device-resident on Vulkan vs the CPU SDPA oracle",
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

    // ── DEVICE (soft-skip with no adapter) ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-26d-3c generic-attention gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    const QuantSeed seeds[4] = {{q_val, q_in, nullptr, sq * dd},
                                {k_val, k_in, nullptr, sk * dd},
                                {v_val, v_in, nullptr, sk * dv},
                                {scale_val, &inv_sqrt_d, nullptr, 1U}};
    float d_out[sq * dv] = {};
    // ⛔ CEIR-26f-3a: call the N-runner directly to capture n_allocated — aliasing is now LIVE on device (line 321 widened to
    //    alias_of>=0), so `probs` SHARES `Kt`'s buffer instead of allocating its own. The oracle compare below is the
    //    LIVE-ALIASING WITNESS (a wrong lifetime ⇒ probs clobbers a still-live Kt ⇒ wrong output — caught at teeth here).
    const QuantOut qo      = {out_val, d_out, sq * dv};
    crd::u32       n_alloc = 0;
    REQUIRE(run_quant_module_n(compute, ctx, &root, plan, seeds, 4U, &qo, 1U, &n_alloc));
    // ⭐ CEIR-26f-3a IDENTITY (advisor — not category): the attention plan has EXACTLY ONE tenant (probs tenants Kt, proven
    //    device-free at 26f-2b(e)) ⇒ the runner allocates EXACTLY nb-1 physical buffers. Pinning `tenants==1` on device catches a
    //    plan drift that changes the aliasing SHAPE while the oracle stays green. (share-false-vs-true bit-exact is 26f-3b.)
    crd::u32 tenants = 0;
    for (crd::usize i = 0; i < plan.buffers.size(); ++i)
    {
        if (plan.buffers[i].alias_of >= 0 && plan.buffers[i].role == ceg::BufferRole::Intermediate) { ++tenants; }
    }
    REQUIRE(tenants == 1U);                                                  // probs→Kt — the ONE tenant
    CHECK(n_alloc == static_cast<crd::u32>(plan.buffers.size()) - tenants);  // ⭐ EXACT physical-buffer count (identity)

    for (crd::u32 i = 0; i < sq * dv; ++i)
    {
        CHECK(crd::math::abs(d_out[i] - oracle[i]) <= 1e-4F * (1.0F + crd::math::abs(oracle[i]))); // generic SDPA == CPU attention (derived tol)
    }
}

// CEIR-24b-4 — the §138 ML PROOF (MLP leg): an authored ml.mlp (2-layer, relu) EXPANDED by expand_ml_ops into gemm/relu runs
// DEVICE-RESIDENT on Vulkan and matches the CPU float MLP oracle. FLOAT-only (the dialect requires Float+equal weights; quantized
// MLP is proven at 23c-c). Dims x[4,8]·W1[8,8]·relu·W2[8,2]->y[4,2] — the hidden layer h1[4,8] is 32 elements, matching the authored
// relu.ckir's baked local_size=32 (a dimension-general relu is name-forward). NO scale buffer (relu has none).
TEST_CASE("ceir 24b-4: an expanded ml.mlp runs device-resident on Vulkan (gemm/relu vs the CPU float MLP oracle)",
          "[ceir][ml][gpu]")
{
    constexpr crd::u32 mrows = 4; // batch rows M
    constexpr crd::u32 d0    = 8; // input width
    constexpr crd::u32 d1    = 8; // hidden width (h1 = M*d1 = 32 == relu.ckir local_size)
    constexpr crd::u32 d2    = 2; // output width

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
    ce::Value*       mlpops[3] = {x_val, w1_val, w2_val}; // ml.mlp(input, W_1, W_2) — the variadic weights tail
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

    // data: x has negatives so relu matters.
    float x_in[mrows * d0];
    float w1_in[d0 * d1];
    float w2_in[d1 * d2];
    for (crd::u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<float>(static_cast<int>(i) - 12); } // -1.2..1.9
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
            h1[n] = crd::math::max(acc, 0.0F); // relu
        }
        for (crd::u32 j = 0; j < d2; ++j)
        {
            float acc = 0.0F;
            for (crd::u32 n = 0; n < d1; ++n) { acc += h1[n] * w2_in[n * d2 + j]; }
            oracle[mm * d2 + j] = acc;
        }
    }

    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-24b-4 MLP gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    const QuantSeed seeds[3] = {{x_val, x_in, nullptr, mrows * d0}, {w1_val, w1_in, nullptr, d0 * d1}, {w2_val, w2_in, nullptr, d1 * d2}};
    float d_out[mrows * d2] = {};
    REQUIRE(run_quant_module(compute, ctx, &root, plan, seeds, 3U, out_val, d_out, mrows * d2));

    for (crd::u32 i = 0; i < mrows * d2; ++i)
    {
        CHECK(crd::math::abs(d_out[i] - oracle[i]) <= 1e-4F * (1.0F + crd::math::abs(oracle[i]))); // expanded MLP == CPU float MLP
    }
}

// CEIR-26d-2c — the shape-specialization PROVING gate (the 24z BakedKernelShapeUnsupported reject FLIPS to a run): an ml.mlp whose
// relu'd intermediate is NOT 32 elements — previously rejected pre-emission — now runs DEVICE-RESIDENT on Vulkan, relu.ckir's
// local_size cook-bound (bind_authored_local_size) to the intermediate numel, == the CPU float MLP oracle. Two widths: h1=4·16=64
// (a non-{power-of-two multiple of 32} — catches an alignment assumption) and h1=4·24=96 (not a power of two). Plus the OVERSIZE
// NEGATIVE (h1=4·300=1200 > the 1024 single-workgroup cap ⇒ the resolver returns unresolved ⇒ execute UnresolvedKernel) so the
// deleted pre-check's teeth are REPLACED, not merely removed (the delete=can't-fail scar). ⛔ NO "raw" for a previously-REJECTED
// program — the oracle is the CPU float MLP (the 24b device-vs-oracle standard), not a bit-exact opt-vs-raw.
TEST_CASE("ceir 26d-2c: a non-32-width ml.mlp runs device-resident on Vulkan (relu cook-bound) == the CPU float MLP oracle",
          "[ceir][ml][gpu]")
{
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-26d-2c non-32 MLP gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    // Build + expand + plan + run a 2-layer float MLP x[M,d0]·W1[d0,d1]·relu·W2[d1,d2] for a given hidden width d1 (the relu'd
    // intermediate numel = M·d1); compare device output to a CPU float MLP oracle. Returns the run_quant_module success flag.
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
        REQUIRE(er.error == ceg::MlExpandError::None); // ⭐ 26d: NO BakedKernelShapeUnsupported for a non-32 width anymore
        REQUIRE(er.expanded == 1U);
        // ⭐ CEIR-26e-3a: PIN fuse_gemm_relu=OFF — 26d-2c tests the relu VizDispatch cook-bind + oversize LocalSizeExceedsLimit cap
        //    (the 26d shape-specialization mechanism), which exists ONLY when relu is a STANDALONE dispatch. With fusion default-ON the
        //    MLP's relu folds into the gemm store (no VizDispatch to cook-bind) — that FUSED path is proven bit-exact vs THIS unfused
        //    form at 26e-3b. Keeping the mechanism's teeth here (they still guard standalone relu / softmax / relu_vjp) needs fuse-off.
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

        // ⛔ DISCRIMINATING CHECK (advisor): pin WHICH cook-bind outcome this width triggers on the relu VizDispatch stage, so the
        //    oversize negative asserts LocalSizeExceedsLimit specifically (not merely "run_quant_module returned false for any
        //    reason" — the can't-fail trap). Mirrors the resolver's own bind_authored_local_size call.
        crd::u64 relu_numel = 0ULL;
        for (crd::usize i = 0; i < plan.stages.size(); ++i)
        {
            const ceg::PlanStage& st = plan.stages[i];
            if (st.kind == ceg::StageKind::VizDispatch && st.n_out >= 1U)
            {
                relu_numel = tnumel(ctx, st.op->operand(3U + st.nbind - st.n_out)->type()); // the trailing-write (relu out) numel
                break;
            }
        }
        crd::u32                    probe_ls = 0U; // the sentinel — mirror what the resolver does to this stage
        const ceg::KernelShapeError kse      = ceg::bind_authored_local_size(probe_ls, relu_numel, ceg::kMaxAuthoredLocalSize);
        if (relu_numel > static_cast<crd::u64>(ceg::kMaxAuthoredLocalSize))
        {
            CHECK(kse == ceg::KernelShapeError::LocalSizeExceedsLimit); // THIS is why the oversize run fails — a specific reject
        }
        else
        {
            CHECK(kse == ceg::KernelShapeError::None);                              // in-range width cook-binds cleanly...
            CHECK(probe_ls == static_cast<crd::u32>(relu_numel));                   // ...to the relu'd intermediate numel
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

    CHECK(run_mlp(16U)); // h1 = 4·16 = 64 elements — was BakedKernelShapeUnsupported, now cook-bound + runs == oracle
    CHECK(run_mlp(24U)); // h1 = 4·24 = 96 elements — a non-power-of-two width

    // OVERSIZE NEGATIVE: h1 = 4·300 = 1200 > kMaxAuthoredLocalSize (1024) ⇒ the resolver's bind_authored_local_size returns
    // LocalSizeExceedsLimit ⇒ the relu stage resolves to a null pipeline ⇒ execute_tensor_pipeline returns UnresolvedKernel
    // ⇒ run_quant_module is false. The replacement teeth for the deleted M·hidden==32 pre-check (delete=can't-fail scar).
    CHECK_FALSE(run_mlp(300U));
}

// CEIR-26e-3b — the gemm→relu FUSION DIFFERENTIAL (the first DISPATCHED-stage 26e witness at FULL TEETH): the SAME fp32-MLP module
// planned TWICE — fuse_gemm_relu OFF (Gemm→relu.ckir dispatch, 3 stages) vs ON (GemmRelu's max(acc,0) store, 2 stages) — runs BOTH
// on Vulkan and asserts the outputs are BIT-EXACT equal (NOT tol). The gemm K-loop is the SAME emit_contract; the unfused arm stores
// z=acc then relu.ckir loads it + max(z,0); the fused arm does max(acc,0) in-register — an f32 store/load is identity + max is FMax
// either way ⇒ bit-identical. The [[feedback_semantics_preserving_pass_differential_test_is_bit_exact_vs_unoptimized_program]]
// contract (the 26a/26c standard), now at full teeth on a DISPATCHED stage (26c touched only alias buffers; this changes kernels).
TEST_CASE("ceir 26e-3b: the gemm-relu fusion is BIT-EXACT vs the unfused program on Vulkan (raw-vs-opt, one module planned both ways)",
          "[ceir][ml][gpu]")
{
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-26e-3b fusion differential"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    constexpr crd::u32 mrows = 4;
    constexpr crd::u32 d0    = 8;
    constexpr crd::u32 d1    = 16; // 26d-2c's KNOWN-GOOD shape (h1 numel 64 ≤ 1024 — the unfused relu cook-binds + ran green #4753)
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

    // ── plan the SAME module BOTH ways — the raw-vs-opt pair (this is why PlanOptions is a PLAN-time flag, not a build flag) ──
    ceg::PlanOptions opt_u;
    opt_u.fuse_gemm_relu = false;
    const ceg::TensorPipelinePlan plan_u = ceg::plan_tensor_pipeline(ctx, *m, &root, opt_u);
    const ceg::PlanOptions        opt_f; // fuse_gemm_relu = true (the default)
    const ceg::TensorPipelinePlan plan_f = ceg::plan_tensor_pipeline(ctx, *m, &root, opt_f);
    REQUIRE(plan_u.reject == ceg::PlanReject::None);
    REQUIRE(plan_f.reject == ceg::PlanReject::None);
    REQUIRE(plan_u.stages.size() == 3U); // Gemm + VizDispatch(relu) + Gemm (unfused)
    REQUIRE(plan_f.stages.size() == 2U); // GemmRelu + Gemm (fused — N-1)
    bool has_gemmrelu = false;
    for (crd::usize i = 0; i < plan_f.stages.size(); ++i) { has_gemmrelu = has_gemmrelu || plan_f.stages[i].kind == ceg::StageKind::GemmRelu; }
    REQUIRE(has_gemmrelu);

    // out_val — the terminal Output buffer's SSA value (the SAME in both plans; the module is shared).
    const ce::Value* out_val = nullptr;
    for (crd::usize i = 0; i < plan_f.buffers.size(); ++i)
    {
        if (plan_f.buffers[i].role == ceg::BufferRole::Output) { out_val = plan_f.buffers[i].value; }
    }
    REQUIRE(out_val != nullptr);

    // 26d-2c's EXACT seeds (they give NEGATIVE pre-activations — asserted below so the witness is not vacuous).
    crd::containers::Array<float> x_in(&root);
    crd::containers::Array<float> w1_in(&root);
    crd::containers::Array<float> w2_in(&root);
    x_in.resize(mrows * d0, 0.0F);
    w1_in.resize(d0 * d1, 0.0F);
    w2_in.resize(d1 * d2, 0.0F);
    for (crd::u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<float>(static_cast<int>(i) - 12); }
    for (crd::u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<float>(static_cast<int>(i % 7) - 3); }
    for (crd::u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<float>(static_cast<int>(i % 5) - 2); }

    // ⛔ NON-VACUOUS (advisor): the differential is meaningless if relu is identity — REQUIRE ≥1 pre-activation z1 = x·W1 < 0.
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
    REQUIRE(neg_count > 0); // relu actually flips ≥1 element — the fusion witness has teeth

    const QuantSeed seeds[3] = {{x_val, x_in.data(), nullptr, mrows * d0},
                                {w1_val, w1_in.data(), nullptr, d0 * d1},
                                {w2_val, w2_in.data(), nullptr, d1 * d2}};
    crd::containers::Array<float> out_u(&root);
    crd::containers::Array<float> out_f(&root);
    out_u.resize(mrows * d2, 0.0F);
    out_f.resize(mrows * d2, 0.0F);
    REQUIRE(run_quant_module(compute, ctx, &root, plan_u, seeds, 3U, out_val, out_u.data(), mrows * d2)); // unfused arm (fuse=false)
    REQUIRE(run_quant_module(compute, ctx, &root, plan_f, seeds, 3U, out_val, out_f.data(), mrows * d2)); // fused arm (fuse=true)

    // ⭐ BIT-EXACT (NOT tol): fused max(acc,0) store == unfused Gemm-store + relu.ckir max(z,0), same K-loop. Report the FIRST
    //    mismatch index + both values so a fail is diagnosable in ONE run (advisor). A CONSISTENT wrong value ⇒ the h/z barrier
    //    path, NOT the K-loop (the fused path has one fewer sync point — z's barrier is gone, which is correct).
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
    CHECK(first_mismatch == -1); // every output element bit-identical fused-vs-unfused
}

// CEIR-27b — the §146 TWO-SCHEDULE DIFFERENTIAL (authored transform ASSETS drive it): the SAME fp32-MLP payload planned with TWO
// committed `.ceir` transform schedules — schedule_nofuse (fuse=false, share=false → Gemm + relu.ckir + Gemm, 3 stages) vs
// schedule_fuse (fuse=true, share=true → GemmRelu + Gemm, 2 stages) — runs BOTH on Vulkan and asserts BIT-EXACT outputs. This is
// 26e-3b's fusion differential with the C++ PlanOptions LITERALS replaced by PARSED authored schedule assets
// (plan_options_from_transform): "two schedules optimize ONE semantic program, different lowering, same values" (§71/§146). ⛔
// share is program-global-INERT on the MLP (the 26f-3a no-disjoint-pair fact) — asserted (tenants==0 on BOTH), so the observable
// plan difference is purely the fusion the fuse directive drives. Asset canonicality/round-trip is the device-free ceir 27b.
TEST_CASE("ceir 27b: two AUTHORED .ceir transform schedules optimize one MLP program BIT-EXACT on Vulkan (fuse asset vs no-fuse asset)",
          "[ceir][ml][gpu][transform]")
{
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-27b two-schedule differential"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    constexpr crd::u32 mrows = 4;
    constexpr crd::u32 d0    = 8;
    constexpr crd::u32 d1    = 16; // 26e-3b's KNOWN-GOOD shape (h1 numel 64 ≤ 1024 — the relu cook-binds)
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

    // ── the PAYLOAD: an fp32 2-layer MLP (x·W1 → relu → ·W2), the 26e-3b module ──
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
    REQUIRE(opt_u.fuse_gemm_relu == false); // the no-fuse schedule loaded
    REQUIRE(opt_f.fuse_gemm_relu == true);  // the fuse schedule loaded

    // ── plan the SAME payload with the TWO authored schedules (the raw-vs-opt pair, now ASSET-DRIVEN) ──
    const ceg::TensorPipelinePlan plan_u = ceg::plan_tensor_pipeline(ctx, *m, &root, opt_u);
    const ceg::TensorPipelinePlan plan_f = ceg::plan_tensor_pipeline(ctx, *m, &root, opt_f);
    REQUIRE(plan_u.reject == ceg::PlanReject::None);
    REQUIRE(plan_f.reject == ceg::PlanReject::None);
    REQUIRE(plan_u.stages.size() == 3U); // no-fuse schedule: Gemm + VizDispatch(relu) + Gemm
    REQUIRE(plan_f.stages.size() == 2U); // fuse schedule: GemmRelu + Gemm (N-1)
    bool has_gemmrelu = false;
    for (crd::usize i = 0; i < plan_f.stages.size(); ++i) { has_gemmrelu = has_gemmrelu || plan_f.stages[i].kind == ceg::StageKind::GemmRelu; }
    REQUIRE(has_gemmrelu);
    // ⛔ share is program-global-INERT on the MLP (26f-3a no-disjoint-pair): BOTH schedules produce ZERO tenants — the schedules
    //    DIFFER only in fusion, so the plan-shape difference is entirely the fuse directive's (share is authored + read, nothing to alias).
    const auto count_tenants = [](const ceg::TensorPipelinePlan& p) {
        crd::u32 n = 0;
        for (crd::usize i = 0; i < p.buffers.size(); ++i)
        {
            if (p.buffers[i].alias_of >= 0 && p.buffers[i].role == ceg::BufferRole::Intermediate) { ++n; }
        }
        return n;
    };
    REQUIRE(count_tenants(plan_u) == 0U);
    REQUIRE(count_tenants(plan_f) == 0U);

    // out_val — the terminal Output buffer's SSA value (the SAME in both plans; the payload is shared).
    const ce::Value* out_val = nullptr;
    for (crd::usize i = 0; i < plan_f.buffers.size(); ++i)
    {
        if (plan_f.buffers[i].role == ceg::BufferRole::Output) { out_val = plan_f.buffers[i].value; }
    }
    REQUIRE(out_val != nullptr);

    // 26e-3b's EXACT seeds (NEGATIVE pre-activations so relu is non-vacuous).
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
    REQUIRE(neg_count > 0); // relu actually flips ≥1 element — the differential has teeth

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
    CHECK(first_mismatch == -1); // every output element bit-identical: two schedules, one program, same values (§146)
}

// CEIR-35 Q7 (§PR-7 PQP-1, docs/bench/ convention) — the COMPILE-TIME DECOMPOSITION board. Isolates the CEIR SUBSTRATE's OWN compile
// cost from the unavoidable external shader compilation: for a realistic MLP layer, TIME (device-free — NO GPU) the CEIR-owned lowering
// (plan_tensor_pipeline, the §158 device-free compile half) + the CEIR GLSL CODEGEN (synth_gemm + emit_contract_glsl per GEMM stage)
// SEPARATELY from glslang (compile_glsl_to_spirv). ⛔ NOT a peer crush — a Cerid-internal DECOMPOSITION (the CEIR-28/29/REN-1 label); the
// claim it qualifies is that the IR layer's compile overhead is a small, BOUNDED fraction of the glslang floor, not that CEIR beats a
// peer. Prints "[CEIR35-Q7-COMPILE]" rows captured into docs/bench/2026-09-11-ceir35-compile-time-decomposition.md. ⛔ measure on
// win-release (asserts/profiling OFF) — a debug number is a CEILING, not a claim (docs/bench/README honesty rule). fuse=true ⇒ the plan is
// [GemmRelu, Gemm] (relu is the epilogue), so the codegen path is pure synth+emit with NO .ckir file I/O to pollute the emit clock.
TEST_CASE("ceir35 Q7: compile-time decomposition -- CEIR lowering+codegen vs glslang (device-free)", "[ceir][ceir35][perf][compile]")
{
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);

    constexpr crd::u32 mrows = 32; // a realistic MLP layer (2 GEMM stages after expand + fuse)
    constexpr crd::u32 d0    = 64;
    constexpr crd::u32 d1    = 128;
    constexpr crd::u32 d2    = 64;
    const MlpPayload   pay  = build_mlp_payload(ctx, mrows, d0, d1, d2);
    const ceg::PlanOptions opts; // {fuse=true, share=true} ⇒ [GemmRelu, Gemm]

    constexpr crd::u32 n_warmup  = 3;
    constexpr crd::u32 n_timed = 15; // median of an odd K (a real sample, not an average — the measurer mold)

    // ── phase L: the CEIR-owned IR lowering (device-free) — plan_tensor_pipeline over the expanded module ──
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

    // the reference plan whose GEMM stages the codegen/compile phases decompose (fuse=true ⇒ every stage is Gemm/GemmRelu)
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *pay.m, &root, opts);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    crd::u32 n_gemm = 0;
    for (crd::usize i = 0; i < plan.stages.size(); ++i)
    {
        const ceg::StageKind k = plan.stages[i].kind;
        REQUIRE((k == ceg::StageKind::Gemm || k == ceg::StageKind::GemmRelu)); // fuse=true has no VizDispatch relu — pure codegen path
        ++n_gemm;
    }
    REQUIRE(n_gemm >= 1U);

    // ── phase E: the CEIR GLSL CODEGEN (synth_gemm + emit_contract_glsl) per GEMM stage (device-free) ──
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
            REQUIRE(kir::emit_contract_glsl(g, s.output, kern));
        }
        const auto b = std::chrono::steady_clock::now();
        emit_ms[t]   = std::chrono::duration<double, std::milli>(b - a).count();
    }
    const double t_emit = median_of(emit_ms, n_timed);

    // ── phase C: the EXTERNAL glslang compile (compile_glsl_to_spirv). Per timed sweep, RE-EMIT each stage's source UNTIMED, then wrap
    //    ONLY the compile_glsl_to_spirv call in the clock — the compile phase times glslang alone (emit is phase E, already isolated). ──
    const auto compile_stage = [&](const ceg::PlanStage& st) -> double {
        kir::KGraph             g(&root);
        kir::GlslKernel         kern(&root);
        const ceg::GemmEpilogue ep = st.kind == ceg::StageKind::GemmRelu ? ceg::GemmEpilogue::Relu : ceg::GemmEpilogue::None;
        const ceg::GraphSynth   s  = ceg::synth_gemm(ctx, *st.op, g, ep);
        REQUIRE(s.reject == ceg::SynthReject::None);
        REQUIRE(kir::emit_contract_glsl(g, s.output, kern)); // UNTIMED codegen (phase E measures this)
        const auto a   = std::chrono::steady_clock::now();
        const auto spv = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                         "ceir35_q7", &root);
        const auto b = std::chrono::steady_clock::now();
        REQUIRE(spv.ok);
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    double spirv_ms[n_timed] = {};
    for (crd::u32 w = 0; w < n_warmup; ++w) { for (crd::usize i = 0; i < plan.stages.size(); ++i) { (void)compile_stage(plan.stages[i]); } }
    for (crd::u32 t = 0; t < n_timed; ++t)
    {
        double acc = 0.0;
        for (crd::usize i = 0; i < plan.stages.size(); ++i) { acc += compile_stage(plan.stages[i]); }
        spirv_ms[t] = acc;
    }
    const double t_spirv = median_of(spirv_ms, n_timed);

    REQUIRE(t_lower > 0.0); // ⛔ a 0 median ⇒ the clock resolved nothing ⇒ the measurer is measuring NOTHING (hard fail, not a pass)
    REQUIRE(t_emit > 0.0);
    REQUIRE(t_spirv > 0.0);
    const double ceir_owned = t_lower + t_emit;
    std::printf("[CEIR35-Q7-COMPILE] shape=%ux%ux%ux%u stages=%u | CEIR lower=%.4f ms emit=%.4f ms (owned=%.4f ms) | glslang=%.4f ms | owned/glslang=%.4f\n",
                static_cast<unsigned>(mrows), static_cast<unsigned>(d0), static_cast<unsigned>(d1), static_cast<unsigned>(d2),
                static_cast<unsigned>(plan.stages.size()), t_lower, t_emit, ceir_owned, t_spirv, ceir_owned / t_spirv);
    (void)std::fflush(stdout);
    CHECK(ceir_owned < t_spirv); // the qualifying claim: the IR layer's compile cost sits BELOW the unavoidable glslang floor
}

// CEIR-35 Q7 (§PR-7 PQP-1, docs/bench/ convention) — the EXECUTOR-OVERHEAD board. Does the CEIR runtime executor
// (execute_tensor_pipeline: the plan-walk + per-stage bind-assembly + inter-stage barriers) cost anything OVER hand-rolling the
// identical dispatches directly on the compute context? Both arms record the SAME pre-resolved ComputePipelines (resolve/compile is
// EXCLUDED from timing — pre-built once, reused) into ONE recorder + ONE submit_and_wait; only the RECORD path differs. GPU time must
// be identical (same kernels, same grids, same push blobs — proven by the BIT-IDENTICAL output gate), so the only possible delta is
// the executor's CPU record cost. ⛔ NOT a peer crush — a Cerid-internal A/B (the CEIR-29 mold: matched 1-submit/1-wait bracket per
// arm, else you measure sync). The claim it qualifies: the IR executor is ZERO-overhead at GPU time + negligible at record time.
// Prints "[CEIR35-Q7-EXEC]" captured into docs/bench/2026-09-11-ceir35-executor-overhead.md. Measured on win-release.
TEST_CASE("ceir35 Q7: executor overhead vs hand-rolled dispatch (Vulkan)", "[ceir][ceir35][perf][executor][gpu]")
{
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device -- skipping the CEIR-35 Q7 executor-overhead board"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    constexpr crd::u32 mrows = 4; // the 26e-3b known-good MLP shape (last_gpu_ms > 0 at this size)
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

    const MlpPayload              pay  = build_mlp_payload(ctx, mrows, d0, d1, d2);
    const ceg::PlanOptions        opts;
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *pay.m, &root, opts);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    const crd::usize nb = plan.buffers.size();
    REQUIRE(nb <= 32U);
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
    const QuantSeed seeds[3]   = {{pay.x, x_in.data(), nullptr, mrows * d0}, {pay.w1, w1_in.data(), nullptr, d0 * d1},
                                  {pay.w2, w2_in.data(), nullptr, d1 * d2}};
    constexpr crd::usize n_seeds = 3;

    // ── materialize buffers ONCE (persistent across every timed submit — run_quant_module_n's create+seed logic, kept alive) ──
    std::unique_ptr<crd::gpu::ComputeBuffer> owned[32];
    crd::gpu::ComputeBuffer*                 bufs[32] = {};
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.alias_of >= 0) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        crd::gpu::ComputeMemory mem = crd::gpu::ComputeMemory::GpuOnly;
        if (pb.role == ceg::BufferRole::ExternalIn) { mem = crd::gpu::ComputeMemory::CpuToGpu; }
        else if (pb.role == ceg::BufferRole::Output || pb.value == out_val) { mem = crd::gpu::ComputeMemory::GpuToCpu; }
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes;
        owned[i]          = compute.create_buffer(sz, crd::gpu::compute_usage::storage, mem);
        REQUIRE(owned[i] != nullptr);
        bufs[i] = owned[i].get();
        if (pb.role != ceg::BufferRole::ExternalIn) { continue; }
        void* raw = owned[i]->map();
        REQUIRE(raw != nullptr);
        const QuantSeed* seed = nullptr;
        for (crd::usize s = 0; s < n_seeds; ++s) { if (seeds[s].value == pb.value) { seed = &seeds[s]; break; } }
        auto* const    d   = static_cast<float*>(raw);
        const crd::u64 cnt = pb.bytes / 4ULL;
        if (seed != nullptr) { for (crd::u32 e = 0; e < seed->count; ++e) { d[e] = seed->floats[e]; } }
        else { for (crd::u64 e = 0; e < cnt; ++e) { d[e] = 0.0F; } }
        owned[i]->unmap();
    }

    // ── pre-resolve every stage ONCE (build + own the ComputePipelines; the timed loops REUSE them, so compile is EXCLUDED) ──
    Resolver res;
    res.alloc_ctx     = &ctx;
    res.alloc         = &root;
    res.compute       = &compute;
    const crd::usize ns = plan.stages.size();
    REQUIRE(ns <= 16U);
    ceg::ResolvedStage resolved[16];
    for (crd::usize j = 0; j < ns; ++j)
    {
        resolved[j] = resolve_stage(plan.stages[j], &res);
        REQUIRE(resolved[j].pipeline != nullptr);
    }
    crd::i32 out_idx = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == out_val) { out_idx = static_cast<crd::i32>(i); } }
    REQUIRE(out_idx >= 0);
    const auto read_out = [&](crd::containers::Array<float>& dst) {
        dst.resize(out_len, 0.0F);
        const auto* g = static_cast<const float*>(owned[static_cast<crd::usize>(out_idx)]->map());
        REQUIRE(g != nullptr);
        for (crd::usize e = 0; e < out_len; ++e) { dst[e] = g[e]; }
        owned[static_cast<crd::usize>(out_idx)]->unmap();
    };

    constexpr crd::u32 n_warmup  = 5;
    constexpr crd::u32 n_timed = 15;

    // ── Arm A: the CEIR EXECUTOR (execute_tensor_pipeline) with a CACHING resolver (returns the pre-built stages, no recompile) ──
    struct CacheState
    {
        const ceg::ResolvedStage* arr = nullptr;
        crd::u32                  idx = 0;
    };
    const ceg::StageResolveFn cached = [](const ceg::PlanStage&, void* u) -> ceg::ResolvedStage {
        auto& s = *static_cast<CacheState*>(u);
        return s.arr[s.idx++]; // execute_tensor_pipeline resolves stages in order ⇒ a counter returns the matching pre-built stage
    };
    double a_rec[n_timed] = {};
    double a_gpu[n_timed] = {};
    for (crd::u32 t = 0; t < n_warmup + n_timed; ++t)
    {
        CacheState cs{resolved, 0};
        auto&      rec = compute.begin();
        const auto c0  = std::chrono::steady_clock::now();
        const ceg::ExecuteError ee =
            ceg::execute_tensor_pipeline(plan, rec, cached, &cs, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(bufs, nb));
        REQUIRE(ee == ceg::ExecuteError::None);
        rec.barrier(*bufs[static_cast<crd::usize>(out_idx)], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
        const auto c1 = std::chrono::steady_clock::now();
        compute.submit_and_wait();
        if (t >= n_warmup)
        {
            a_rec[t - n_warmup] = std::chrono::duration<double, std::micro>(c1 - c0).count();
            a_gpu[t - n_warmup] = compute.last_gpu_ms();
        }
    }
    crd::containers::Array<float> out_a(&root);
    read_out(out_a);

    // ── Arm B: HAND-ROLLED dispatch (the "native" arm) — replicate the executor's per-stage record: assemble bindings from bind[],
    //    dispatch the SAME resolved pipeline, and the SAME inter-stage ShaderWrite→ShaderRead barrier on the last n_out binds ──
    double b_rec[n_timed] = {};
    double b_gpu[n_timed] = {};
    for (crd::u32 t = 0; t < n_warmup + n_timed; ++t)
    {
        auto&      rec = compute.begin();
        const auto c0  = std::chrono::steady_clock::now();
        for (crd::usize j = 0; j < ns; ++j)
        {
            const ceg::PlanStage&    st       = plan.stages[j];
            crd::gpu::ComputeBuffer* binds[8] = {};
            for (crd::u32 i = 0; i < st.nbind; ++i) { binds[i] = bufs[static_cast<crd::usize>(st.bind[i])]; }
            rec.dispatch(*resolved[j].pipeline, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(binds, st.nbind), resolved[j].push,
                         resolved[j].push_size, resolved[j].gx, resolved[j].gy, resolved[j].gz);
            if (j + 1 < ns) // inter-stage barrier on each of the stage's n_out written outputs (the last n_out binds) — before the next reads
            {
                for (crd::u32 k = 0; k < st.n_out; ++k)
                {
                    const crd::u32 bi = st.nbind - st.n_out + k;
                    rec.barrier(*bufs[static_cast<crd::usize>(st.bind[bi])], crd::gpu::ComputeAccess::ShaderWrite,
                                crd::gpu::ComputeAccess::ShaderRead);
                }
            }
        }
        rec.barrier(*bufs[static_cast<crd::usize>(out_idx)], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
        const auto c1 = std::chrono::steady_clock::now();
        compute.submit_and_wait();
        if (t >= n_warmup)
        {
            b_rec[t - n_warmup] = std::chrono::duration<double, std::micro>(c1 - c0).count();
            b_gpu[t - n_warmup] = compute.last_gpu_ms();
        }
    }
    crd::containers::Array<float> out_b(&root);
    read_out(out_b);

    // the two record paths dispatch the SAME kernels with the SAME args ⇒ BIT-IDENTICAL output (the "same GPU work" proof the GPU
    // parity claim rests on — a differing pixel would mean the arms are NOT the same work and the timing comparison is meaningless)
    int mism = -1;
    for (crd::usize e = 0; e < out_len; ++e) { if (out_a[e] != out_b[e]) { mism = static_cast<int>(e); break; } }
    CHECK(mism == -1);

    const double A_rec = median_of(a_rec, n_timed);
    const double A_gpu = median_of(a_gpu, n_timed);
    const double B_rec = median_of(b_rec, n_timed);
    const double B_gpu = median_of(b_gpu, n_timed);
    REQUIRE(A_gpu > 0.0); // a 0 median ⇒ the device timestamps resolved nothing (measuring NOTHING) — hard fail, not a pass
    REQUIRE(B_gpu > 0.0);
    std::printf("[CEIR35-Q7-EXEC] stages=%u | executor: rec=%.3f us gpu=%.5f ms | native: rec=%.3f us gpu=%.5f ms | rec_delta=%.3f us gpu_ratio=%.4f\n",
                static_cast<unsigned>(ns), A_rec, A_gpu, B_rec, B_gpu, A_rec - B_rec, A_gpu / B_gpu);
    (void)std::fflush(stdout);
    // GPU parity: same kernels + same dispatch args ⇒ GPU time equal within noise (generous band — tiny-work timestamps are noisy;
    // the bit-identical output is the hard proof the work is identical, this only guards a gross regression).
    CHECK(A_gpu < B_gpu * 2.0 + 0.05);
    CHECK(B_gpu < A_gpu * 2.0 + 0.05);
}

// CEIR-35 Q7 (§PR-7 PQP-1, docs/bench/ convention) — the PLAN+PIPELINE REUSE (amortization) board — the "plan-cache hit" dimension.
// ⛔ NOTE the real reuse mechanism: the CEIR-10b PlanCache is NOT wired into the live execute path (a store, no producers), so this
// benches the ACTUAL reuse — the §158 lower-once / compile-once / execute-many split: COLD (first run of a program) pays lowering
// (plan_tensor_pipeline) + resolve/compile (synth+emit+glslang+PSO per stage) + record + GPU; WARM (every subsequent run) reuses the
// lowered plan + the built ComputePipelines and pays only record + GPU. The amortization win is dominated by the shader compile the
// COLD path repeats and the WARM path skips. ⛔ Cerid-internal A/B (NOT a peer crush); matched 1-submit/1-wait bracket per arm.
// Prints "[CEIR35-Q7-REUSE]" captured into docs/bench/2026-09-11-ceir35-plan-reuse-amortization.md. Measured on win-release.
TEST_CASE("ceir35 Q7: plan+pipeline reuse amortization (Vulkan)", "[ceir][ceir35][perf][reuse][gpu]")
{
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device -- skipping the CEIR-35 Q7 reuse-amortization board"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

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

    const MlpPayload              pay  = build_mlp_payload(ctx, mrows, d0, d1, d2);
    const ceg::PlanOptions        opts;
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *pay.m, &root, opts);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    const crd::usize nb = plan.buffers.size();
    REQUIRE(nb <= 32U);
    const ce::Value* out_val = nullptr;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].role == ceg::BufferRole::Output) { out_val = plan.buffers[i].value; } }
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
    const QuantSeed      seeds[3] = {{pay.x, x_in.data(), nullptr, mrows * d0}, {pay.w1, w1_in.data(), nullptr, d0 * d1},
                                     {pay.w2, w2_in.data(), nullptr, d1 * d2}};
    constexpr crd::usize n_seeds  = 3;

    std::unique_ptr<crd::gpu::ComputeBuffer> owned[32];
    crd::gpu::ComputeBuffer*                 bufs[32] = {};
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.alias_of >= 0) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        crd::gpu::ComputeMemory mem = crd::gpu::ComputeMemory::GpuOnly;
        if (pb.role == ceg::BufferRole::ExternalIn) { mem = crd::gpu::ComputeMemory::CpuToGpu; }
        else if (pb.role == ceg::BufferRole::Output || pb.value == out_val) { mem = crd::gpu::ComputeMemory::GpuToCpu; }
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes;
        owned[i]          = compute.create_buffer(sz, crd::gpu::compute_usage::storage, mem);
        REQUIRE(owned[i] != nullptr);
        bufs[i] = owned[i].get();
        if (pb.role != ceg::BufferRole::ExternalIn) { continue; }
        void* raw = owned[i]->map();
        REQUIRE(raw != nullptr);
        const QuantSeed* seed = nullptr;
        for (crd::usize s = 0; s < n_seeds; ++s) { if (seeds[s].value == pb.value) { seed = &seeds[s]; break; } }
        auto* const    d   = static_cast<float*>(raw);
        const crd::u64 cnt = pb.bytes / 4ULL;
        if (seed != nullptr) { for (crd::u32 e = 0; e < seed->count; ++e) { d[e] = seed->floats[e]; } }
        else { for (crd::u64 e = 0; e < cnt; ++e) { d[e] = 0.0F; } }
        owned[i]->unmap();
    }
    crd::i32 out_idx = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == out_val) { out_idx = static_cast<crd::i32>(i); } }
    REQUIRE(out_idx >= 0);
    const crd::usize ns = plan.stages.size();
    REQUIRE(ns <= 16U);

    // WARM setup: pre-resolve every stage ONCE (build + own the pipelines) — the warm arm reuses these, paying no compile
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

    // ── COLD: a first-run each time — re-lower + a FRESH Resolver (recompiles every stage's shader+PSO) + execute + submit ──
    double cold_ms[n_timed] = {};
    for (crd::u32 t = 0; t < n_warmup + n_timed; ++t)
    {
        const auto                    c0 = std::chrono::steady_clock::now();
        const ceg::TensorPipelinePlan p  = ceg::plan_tensor_pipeline(ctx, *pay.m, &root, opts); // re-lower (deterministic ⇒ same layout)
        REQUIRE(p.reject == ceg::PlanReject::None);
        Resolver cres; // FRESH ⇒ resolve_stage recompiles synth→emit→glslang→PSO for every stage (the cold-start cost)
        cres.alloc_ctx = &ctx;
        cres.alloc     = &root;
        cres.compute   = &compute;
        auto&                   rec = compute.begin();
        const ceg::ExecuteError ee =
            ceg::execute_tensor_pipeline(p, rec, &resolve_stage, &cres, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(bufs, nb));
        REQUIRE(ee == ceg::ExecuteError::None);
        rec.barrier(*bufs[static_cast<crd::usize>(out_idx)], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
        compute.submit_and_wait();
        const auto c1 = std::chrono::steady_clock::now();
        if (t >= n_warmup) { cold_ms[t - n_warmup] = std::chrono::duration<double, std::milli>(c1 - c0).count(); }
    }

    // ── WARM: every subsequent run — reuse the lowered plan + the built pipelines; pay only record + GPU ──
    double warm_ms[n_timed] = {};
    for (crd::u32 t = 0; t < n_warmup + n_timed; ++t)
    {
        CacheState cs{resolved, 0};
        const auto c0  = std::chrono::steady_clock::now();
        auto&      rec = compute.begin();
        const ceg::ExecuteError ee =
            ceg::execute_tensor_pipeline(plan, rec, cached, &cs, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(bufs, nb));
        REQUIRE(ee == ceg::ExecuteError::None);
        rec.barrier(*bufs[static_cast<crd::usize>(out_idx)], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
        compute.submit_and_wait();
        const auto c1 = std::chrono::steady_clock::now();
        if (t >= n_warmup) { warm_ms[t - n_warmup] = std::chrono::duration<double, std::milli>(c1 - c0).count(); }
    }

    const double C = median_of(cold_ms, n_timed);
    const double W = median_of(warm_ms, n_timed);
    REQUIRE(C > 0.0);
    REQUIRE(W > 0.0);
    std::printf("[CEIR35-Q7-REUSE] stages=%u | cold(lower+compile+exec)=%.4f ms | warm(exec, plan+PSO reused)=%.4f ms | speedup=%.1fx\n",
                static_cast<unsigned>(ns), C, W, C / W);
    (void)std::fflush(stdout);
    CHECK(W < C); // reuse (lower-once/compile-once/execute-many) is strictly cheaper than a cold re-lower+recompile every run
}

// CEIR-28b-2a — the §80 AUTOTUNER measured on a REAL device (the v17 kir-autotune loop, portable). The SAME 26e-3b fp32-MLP payload
// is planned with the FULL 4-config PlanOptions space (fuse × share), each TIMED (median of compute.last_gpu_ms) + gated BIT-EXACT
// vs the default (the free oracle-gate — 27b proved only 2 of the 4). The WINNER is emitted as ONE tune.entry ROW keyed by
// (device, env, program_hash(post-expansion payload), shape); the row round-trips back through the REAL loader
// (plan_options_from_tune_cache) → HIT gives the winner's schedule, a wrong hash MISSES, and replaying the looked-up schedule is
// BIT-EXACT to the default. ⛔ NO assertion on WHICH config wins (hardware-measured). The committed .ceir cache + anti-drift is 28b-2b.
TEST_CASE("ceir 28b-2a: the sec-80 autotuner measures the 4-config PlanOptions space on Vulkan, emits + replays one tune.entry (BIT-EXACT)",
          "[ceir][ml][gpu][tune]")
{
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-28b-2a autotuner measurer"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    constexpr crd::u32 mrows = 4;
    constexpr crd::u32 d0    = 8;
    constexpr crd::u32 d1    = 16; // 26e-3b's KNOWN-GOOD shape (h1 numel 64 ≤ 1024 — the no-fuse relu VizDispatch cook-binds)
    constexpr crd::u32 d2    = 2;
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    (void)ce::tune::register_tune_ops(ctx); // ⛔ register tune BEFORE build_entry / print / find_tune_misuse (else the row is opaque)

    // ── the PAYLOAD: the 26e-3b fp32 2-layer MLP (x·W1 → relu → ·W2), SHARED with 28b-2b so both hash the identical program ──
    const MlpPayload pay = build_mlp_payload(ctx, mrows, d0, d1, d2);
    ce::Module* const m  = pay.m;

    // ── the default plan gives out_val (the terminal Output SSA value) + the reference output the winner replays against ──
    const ceg::PlanOptions        def_opts; // {fuse=true, share=true}
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
    const QuantSeed seeds[3] = {{pay.x, x_in.data(), nullptr, mrows * d0},
                                {pay.w1, w1_in.data(), nullptr, d0 * d1},
                                {pay.w2, w2_in.data(), nullptr, d1 * d2}};
    crd::containers::Array<float> out0(&root);
    out0.resize(out_len, 0.0F);
    REQUIRE(run_quant_module(compute, ctx, &root, plan0, seeds, 3U, out_val, out0.data(), out_len)); // the default reference output

    // ── the device KEY (portable: adapter_name is IGpuContext's, so 28d reuses it verbatim) ──
    char dev_buf[320] = {};
    (void)std::snprintf(dev_buf, sizeof(dev_buf), "vk:%s", vk->adapter_name());
    const crd::containers::StringView device(dev_buf);
#if defined(_WIN32)
    const crd::containers::StringView env("win32");            // the OS/driver bundle KEY — platform-derived (the driver/compiler split is deferred)
#else
    const crd::containers::StringView env("linux");
#endif
    const crd::containers::StringView shape("mlp:4x8x16x2");   // the payload's derived shape (mrows × d0 × d1 × d2)
    const crd::u64                    ph = ce::tune::program_hash(ctx, *m, &root);

    // ── MEASURE all 4 configs + emit the winner's tune.entry (the measurer owns the timing + the free oracle-gate) ──
    double            medians[4] = {};
    crd::u64          sig[4]     = {};
    ceg::PlanOptions  winner;
    ce::Module* const emit = measure_tune_entry(compute, ctx, &root, *m, seeds, 3U, out_val, out_len, device, env, shape, medians, sig, &winner);
    REQUIRE(emit != nullptr);
    // ⭐ the discriminating IDENTITY gate (not a category count): share is INERT (tt≡tf, ft≡ff) but fuse is LIVE (tt≢ft). If
    //    fuse_gemm_relu silently stopped changing the lowering, sig[0]==sig[2] would FAIL — the knob-does-something proof.
    CAPTURE(sig[0], sig[1], sig[2], sig[3]);
    CHECK(sig[0] == sig[1]);  // {fuse=T}: share inert (identical plan)
    CHECK(sig[2] == sig[3]);  // {fuse=F}: share inert (identical plan)
    CHECK(sig[0] != sig[2]);  // fuse ON vs OFF: DIFFERENT plan (GemmRelu 2-stage vs Gemm+relu+Gemm 3-stage)
    CHECK(ce::tune::find_tune_misuse(ctx, *emit).kind == ce::tune::TuneMisuseKind::None); // one clean row

    // ── REPLAY: the emitted row round-trips back through the REAL loader (the v17 select_schedule leg) ──
    const ceg::TuneCacheLookup hit = ceg::plan_options_from_tune_cache(ctx, *emit, device, env, ph, shape);
    REQUIRE(hit.hit);                                          // the full (device,env,program_hash,shape) key matched
    CHECK(hit.opts.fuse_gemm_relu == winner.fuse_gemm_relu);
    CHECK(hit.opts.share_intermediate_storage == winner.share_intermediate_storage);
    const ceg::TuneCacheLookup miss = ceg::plan_options_from_tune_cache(ctx, *emit, device, env, ph + 1U, shape);
    CHECK_FALSE(miss.hit);                                     // a WRONG program_hash misses — the key is real, not a rubber stamp

    // ── the replayed schedule is BIT-EXACT to the default (semantics-preserving, whichever config won) ──
    const ceg::TensorPipelinePlan plan_r = ceg::plan_tensor_pipeline(ctx, *m, &root, hit.opts);
    REQUIRE(plan_r.reject == ceg::PlanReject::None);
    crd::containers::Array<float> out_r(&root);
    out_r.resize(out_len, 0.0F);
    REQUIRE(run_quant_module(compute, ctx, &root, plan_r, seeds, 3U, out_val, out_r.data(), out_len));
    int replay_mismatch = -1;
    for (crd::u32 i = 0; i < out_len && replay_mismatch < 0; ++i) { if (out_r[i] != out0[i]) { replay_mismatch = static_cast<int>(i); } }
    CAPTURE(replay_mismatch);
    CHECK(replay_mismatch == -1);

    // record the measured row for the tracker + 28b-2b's committed asset (visible with catch2 -s):
    const crd::containers::String emit_txt = ce::print(ctx, *emit, &root);
    INFO("28b-2a device=\"" << dev_buf << "\" program_hash=" << ph << " shape=mlp:4x8x16x2 winner{fuse=" << winner.fuse_gemm_relu
                            << ",share=" << winner.share_intermediate_storage << "} medians_ms[tt,tf,ft,ff]=[" << medians[0] << ","
                            << medians[1] << "," << medians[2] << "," << medians[3] << "]\nemit:\n" << emit_txt.c_str());
    CHECK(true); // anchor the INFO (the row/asset data prints under -s on this passing case)
}

// CEIR-28b-2b / 28z-2 — the COMMITTED target-specific config cache is a real AUTHORED asset (the §80 "checked-in DB" made portable +
// the v17 "tune offline, replay forever" discipline). ⛔ 28z-2: `assets/ceir/tune_cache.ceir` is now the ONE cross-backend cache — 3
// rows (vk:RTX, dx12:RTX, vk:llvmpipe), each bootstrapped from its measurer's printed `-s` emit (the measurer IS the builder). A
// multi-row cache means the anti-drift can no longer be module-print-equality (3 rows vs a 1-row fresh emit) — it becomes PER-ROW
// FIELD-equality via the loader (both sides read through `load_tune_entries`, so still no raw-byte compare). This TU's gates:
//   • ONE `find_tune_misuse` → None on the 3-row module (no duplicate KEY across backends — the multi-row cache the 28a(f) gate is for).
//   • ROWS-ADDRESSABLE table: all 3 rows independently HIT their own key (proves ONE cross-backend artifact, not 3 same-named files).
//   • wrong-ENV miss (RTX-Vk device under env="linux") — env is a real key dimension, now proven on a REAL FILE (28a(d) was constructed).
//   • ANTI-DRIFT on THIS device's row: re-measure → the fresh emit's entry FIELD-matches the committed row (a winner FLIP changes fuse
//     → caught = the DESIGNED re-commit signal), then REPLAY that row BIT-EXACT to the default. Runs the vk:RTX row on Windows, the
//     vk:llvmpipe row on WSL lavapipe (the DX12 row is 28z-2's DX12 TU). ⛔ the "device with NO row" else-branch is UNEXERCISED on the
//     RTX/lavapipe boxes (every device we run has a row) — kept for correctness, not claimed as covered.
TEST_CASE("ceir 28b-2b: the committed 3-row tune_cache.ceir anti-drifts this device's row + all rows addressable + env discriminates",
          "[ceir][ml][gpu][tune]")
{
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-28b-2b committed-cache anti-drift"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

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
    (void)ce::tune::register_tune_ops(ctx); // ⛔ register tune BEFORE parsing the cache asset (else tune.entry parses opaque)

    // ── the SAME payload as 28b-2a (shared builder) → the SAME program_hash the committed row keys on ──
    const MlpPayload pay = build_mlp_payload(ctx, mrows, d0, d1, d2);
    ce::Module* const m  = pay.m;

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
    const QuantSeed seeds[3] = {{pay.x, x_in.data(), nullptr, mrows * d0},
                                {pay.w1, w1_in.data(), nullptr, d0 * d1},
                                {pay.w2, w2_in.data(), nullptr, d1 * d2}};
    crd::containers::Array<float> out0(&root);
    out0.resize(out_len, 0.0F);
    REQUIRE(run_quant_module(compute, ctx, &root, plan0, seeds, 3U, out_val, out0.data(), out_len));

    char dev_buf[320] = {};
    (void)std::snprintf(dev_buf, sizeof(dev_buf), "vk:%s", vk->adapter_name());
    const crd::containers::StringView device(dev_buf);
#if defined(_WIN32)
    const crd::containers::StringView env("win32");
#else
    const crd::containers::StringView env("linux");
#endif
    const crd::containers::StringView shape("mlp:4x8x16x2");
    const crd::u64                    ph = ce::tune::program_hash(ctx, *m, &root);

    // ── PARSE the committed 3-row cross-backend cache asset ──
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
    CHECK(ce::tune::find_tune_misuse(ctx, *cpr.module).kind == ce::tune::TuneMisuseKind::None); // no duplicate KEY across the 3 backends

    crd::containers::Array<ce::tune::TuneEntry> entries(&root);
    const crd::u32                              n_rows = ce::tune::load_tune_entries(ctx, *cpr.module, entries);
    REQUIRE(n_rows == 3U); // vk:RTX + dx12:RTX + vk:llvmpipe — ONE cross-backend artifact

    // ── ROWS-ADDRESSABLE (device-INDEPENDENT): every row HITS its OWN key with the shared schedule — proves it is one cache with
    //    three real rows, not three same-named files. program_hash is portable (same payload on every backend). ──
    struct CacheKey { const char* device; const char* env; };
    const CacheKey known[3] = {{"vk:NVIDIA GeForce RTX 4070 Ti SUPER", "win32"},
                               {"dx12:NVIDIA GeForce RTX 4070 Ti SUPER", "win32"},
                               {"vk:llvmpipe (LLVM 20.1.2, 256 bits)", "linux"}};
    for (crd::u32 i = 0; i < 3U; ++i)
    {
        const ceg::TuneCacheLookup lk = ceg::plan_options_from_tune_cache(
            ctx, *cpr.module, crd::containers::StringView(known[i].device), crd::containers::StringView(known[i].env), ph, shape);
        CAPTURE(known[i].device);
        REQUIRE(lk.hit);                                    // keyed for THIS program on every backend (portable hash)
        CHECK(lk.opts.fuse_gemm_relu == true);
        CHECK(lk.opts.share_intermediate_storage == true);
    }
    // ⛔ wrong-ENV MISS: the RTX-Vk device under env="linux" MISSES — env is a REAL key dimension (first proven on a real FILE; 28a(d)
    //    proved it on a constructed module). ⛔ program_hash pin: the i64 the file prints (-4961225458425459157) reads back as this u64.
    const ceg::TuneCacheLookup wrong_env = ceg::plan_options_from_tune_cache(
        ctx, *cpr.module, crd::containers::StringView("vk:NVIDIA GeForce RTX 4070 Ti SUPER"), crd::containers::StringView("linux"), ph, shape);
    CHECK_FALSE(wrong_env.hit);
    CHECK(entries[0].program_hash == 13485518615284092459ULL); // the 28b-1 u64-widen, GATED against a REAL parsed asset

    // ── find THIS device's row (exactly one on a known device) ──
    int mi = -1;
    for (crd::u32 i = 0; i < n_rows; ++i) { if (entries[i].device == device && entries[i].env == env) { mi = static_cast<int>(i); } }
    if (mi >= 0)
    {
        const ce::tune::TuneEntry& row = entries[static_cast<crd::usize>(mi)];
        // ── ANTI-DRIFT: re-measure the SAME payload; the fresh emit's entry must FIELD-match the committed row (per-row via the
        //    loader — no raw-byte compare). A re-tune that FLIPS the winner changes `fuse` ⇒ CHECK below fails = the re-commit signal. ──
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

        // ── REPLAY the committed row through the real loader → BIT-EXACT to the default ──
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
        // ⛔ UNEXERCISED on the RTX/lavapipe boxes (every device we run has a row) — kept for correctness, NOT claimed as covered.
        WARN("this device=\"" << dev_buf << "\" has no committed row — anti-drift branch skipped (a 4th device would exercise it)");
    }
}

// CEIR-26f-3b — the memory-aliasing DIFFERENTIAL (share_intermediate_storage false-vs-true): the SAME ml.attention module planned
// TWICE — sharing OFF (every Intermediate its own buffer) vs ON (default; probs TENANTS Kt) — runs BOTH on Vulkan and asserts the
// outputs are BIT-EXACT equal. Aliasing changes ADDRESSES, not kernels (the same transpose/gemm/softmax/gemm run either way; only
// probs's storage differs), so a bit-identical result PROVES the LIFETIME ANALYSIS is SOUND — a wrong alias would overlap Kt's live
// range and CLOBBER it (wrong output). The physical-buffer count drops by EXACTLY the tenant count. The
// [[feedback_semantics_preserving_pass_differential_test_is_bit_exact_vs_unoptimized_program]] contract, LOAD-BEARING here (a bad
// lifetime corrupts data — the 26f-0 "aliasing differential HAS teeth" claim, on a real device).
TEST_CASE("ceir 26f-3b: buffer-aliasing is BIT-EXACT vs the un-shared plan on Vulkan (attention, one module both ways) + exact buffer-count delta",
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
    REQUIRE(count_tenants(plan_noshare) == 0U);                         // ⭐ sharing OFF ⇒ NO tenants (every Intermediate its own buffer)
    REQUIRE(plan_noshare.buffers.size() == plan_share.buffers.size()); // same buffers, only alias_of differs

    // out_val + scale_val — the same SSA values in both plans (one shared module).
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

    // ── DEVICE (soft-skip with no adapter) ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-26f-3b aliasing differential"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

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
    CHECK(first_mismatch == -1); // every output element bit-identical shared-vs-unshared ⇒ the lifetime analysis is SOUND
}

// CEIR-24c-2b — the coopvec NATIVE MLP CLAIM path on device (the §69 native provider's device half): an ml.mlp op is CLAIMED
// whole by the coopvec provider — coopvec_config_from_mlp (dims from the op) + coopvec_weights_from_mlp (the caller's f32 weights,
// TRANSPOSED to fp16) feed crd::kir::neural::emit_coopvec_mlp_glsl (VK_NV_cooperative_vector, per-invocation tensor-unit MLP). The
// native dispatch on a real NVIDIA device matches eval_coopvec_mlp_cpu. ⛔ HONEST SCOREBOARD: WARN-skip when cooperative_vector()
// (C6) is absent — a coopvec kernel is NVIDIA-only (the CKIR-expansion path 24b-4 is the portable fallback). Dims x[4,8]·W1[8,8]·
// relu·W2[8,2] (in=8, hidden=8, out=2, 1 hidden layer).
TEST_CASE("ceir 24c-2b: an ml.mlp CLAIMED by the coopvec provider dispatches native on Vulkan (VK_NV_cooperative_vector) == the fp16 ref",
          "[ceir][ml][gpu][coopvec]")
{
    namespace nn = crd::kir::neural;
    constexpr crd::u32 n_s = 4U; // M samples
    constexpr crd::u32 d0  = 8U;
    constexpr crd::u32 d1  = 8U;
    constexpr crd::u32 d2  = 2U;

    // build the ml.mlp op (its TYPES drive the config).
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::ml::register_dialect(ctx);
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
    ce::Value* const x   = mkd(tf(ctx, sh2(ctx, n_s, d0)));
    ce::Value* const w1v = mkd(tf(ctx, sh2(ctx, d0, d1)));
    ce::Value* const w2v = mkd(tf(ctx, sh2(ctx, d1, d2)));
    ce::Value*       mlpops[3] = {x, w1v, w2v};
    ce::Operation* const mo = ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mlpops, 3U),
                                                   1U, tf(ctx, sh2(ctx, n_s, d2)), 0U);
    ctx.set_attr(mo, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
    b->append(mo);
    REQUIRE(ceg::coopvec_can_claim_mlp(ctx, mo)); // the coopvec provider claims it

    const nn::CoopVecMlpConfig mlp = ceg::coopvec_config_from_mlp(ctx, mo);
    REQUIRE(mlp.valid());

    // asymmetric f32 weights + convert to coopvec fp16 (transposed); zero bias.
    float w1[d0 * d1];
    float w2[d1 * d2];
    for (crd::u32 i = 0; i < d0; ++i)
    {
        for (crd::u32 j = 0; j < d1; ++j) { w1[i * d1 + j] = 0.05F * static_cast<float>(i + 1U) - 0.031F * static_cast<float>(j + 1U); }
    }
    for (crd::u32 i = 0; i < d1; ++i)
    {
        for (crd::u32 j = 0; j < d2; ++j) { w2[i * d2 + j] = 0.1F * static_cast<float>((i % 3U) + 1U) - 0.043F * static_cast<float>(j + 1U); }
    }
    const float* wptrs[2] = {&w1[0], &w2[0]};
    crd::containers::Array<crd::u16> wf16(&root);
    wf16.resize(static_cast<crd::usize>(mlp.weight_count()));
    REQUIRE(ceg::coopvec_weights_from_mlp(mlp, wptrs, wf16.data()));
    crd::containers::Array<crd::u16> bf16(&root);
    bf16.resize(static_cast<crd::usize>(mlp.bias_count()), crd::math::f32_to_f16_bits(0.0F)); // zero bias

    // inputs (some negatives so relu bites) -> fp16.
    crd::containers::Array<crd::u16> in_h(&root);
    in_h.resize(static_cast<crd::usize>(n_s) * d0);
    for (crd::u32 s = 0; s < n_s; ++s)
    {
        for (crd::u32 c = 0; c < d0; ++c)
        {
            const float v = 0.2F * static_cast<float>(static_cast<int>(s) - 1) + 0.06F * static_cast<float>(static_cast<int>(c) - 4);
            in_h[static_cast<crd::usize>(s) * d0 + c] = crd::math::f32_to_f16_bits(v);
        }
    }
    // the CPU coopvec reference (the device must match this).
    crd::containers::Array<crd::u16> ref(&root);
    ref.resize(static_cast<crd::usize>(n_s) * d2, static_cast<crd::u16>(0));
    nn::eval_coopvec_mlp_cpu(mlp, wf16.data(), bf16.data(), in_h.data(), static_cast<int>(n_s), ref.data());

    // ── DEVICE (soft-skip: no adapter OR no cooperative_vector — the honest NVIDIA-gated scoreboard) ──
    crd::gpu::GpuContextConfig gcfg;
    gcfg.backend  = crd::gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto devctx   = crd::gpu::create_vulkan_gpu_context(gcfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-24c-2b coopvec gate"); return; }
    auto* const vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    if (!vk->cooperative_vector()) { WARN("no VK_NV_cooperative_vector (non-NVIDIA) — the coopvec CLAIM path is unavailable; CKIR expansion (24b-4) is the portable fallback"); return; }
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::kir::GlslKernel kern(&root);
    REQUIRE(nn::emit_coopvec_mlp_glsl(mlp, kern));
    const auto spv = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                     "ceir_coopvec_mlp", &root);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 5, 0U);
    REQUIRE(pipe != nullptr);

    using crd::gpu::compute_usage::storage;
    using crd::gpu::compute_usage::transfer_dst;
    using crd::gpu::compute_usage::transfer_src;
    auto       d_in  = compute.create_buffer(static_cast<crd::u64>(n_s * d0) * 2U, storage | transfer_dst, crd::gpu::ComputeMemory::GpuOnly);
    auto       d_w   = compute.create_buffer(static_cast<crd::u64>(mlp.weight_count()) * 2U, storage | transfer_dst, crd::gpu::ComputeMemory::GpuOnly);
    auto       d_b   = compute.create_buffer(static_cast<crd::u64>(mlp.bias_count()) * 2U, storage | transfer_dst, crd::gpu::ComputeMemory::GpuOnly);
    auto       d_out = compute.create_buffer(static_cast<crd::u64>(n_s * d2) * 2U, storage | transfer_src, crd::gpu::ComputeMemory::GpuOnly);
    auto       d_cfg = compute.create_buffer(16U, storage | transfer_dst, crd::gpu::ComputeMemory::GpuOnly);
    const auto up    = [&](crd::gpu::ComputeBuffer& dst, const void* src, crd::u64 nbytes) {
        auto        stg  = compute.create_buffer(nbytes, transfer_src, crd::gpu::ComputeMemory::CpuToGpu);
        auto*       p    = static_cast<crd::u8*>(stg->map());
        const auto* srcb = static_cast<const crd::u8*>(src);
        for (crd::u64 i = 0; i < nbytes; ++i) { p[i] = srcb[i]; }
        stg->unmap();
        auto& rc = compute.begin();
        rc.copy(*stg, dst, 0U, 0U, nbytes);
        compute.submit_and_wait();
    };
    const crd::u32 cfgv[4] = {n_s, 0U, 0U, 0U};
    up(*d_in, in_h.data(), static_cast<crd::u64>(n_s * d0) * 2U);
    up(*d_w, wf16.data(), static_cast<crd::u64>(mlp.weight_count()) * 2U);
    up(*d_b, bf16.data(), static_cast<crd::u64>(mlp.bias_count()) * 2U);
    up(*d_cfg, cfgv, 16U);

    auto&                    rec      = compute.begin();
    crd::gpu::ComputeBuffer* binds[5] = {d_in.get(), d_w.get(), d_b.get(), d_out.get(), d_cfg.get()};
    rec.dispatch(*pipe, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(binds, 5), nullptr, 0U, (n_s + 63U) / 64U, 1U, 1U);
    rec.barrier(*d_out, crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::TransferSrc);
    compute.submit_and_wait();

    auto rb = compute.create_buffer(static_cast<crd::u64>(n_s * d2) * 2U, transfer_dst, crd::gpu::ComputeMemory::GpuToCpu);
    {
        auto& r2 = compute.begin();
        r2.copy(*d_out, *rb, 0U, 0U, static_cast<crd::u64>(n_s * d2) * 2U);
        compute.submit_and_wait();
    }
    const auto* out = static_cast<const crd::u16*>(rb->map());
    for (crd::u32 i = 0; i < n_s * d2; ++i)
    {
        const float got = crd::math::f16_bits_to_f32(out[i]);
        const float rf  = crd::math::f16_bits_to_f32(ref[static_cast<crd::usize>(i)]);
        CHECK(crd::math::abs(got - rf) < 0.05F); // the claimed ml.mlp dispatches native == the coopvec CPU reference (fp16)
    }
    rb->unmap();
}

// CEIR-24c-3 — the §136 CROWN: ONE ml.mlp region, TWO §69 PARTITION STRATEGIES, the SAME numbers. Run A (caps=coopvec-off) →
// the partitioner assigns the CKIR FALLBACK → apply_partition expands to gemm/relu → plan → execute (the 24b portable path,
// f32, Vulkan). Run B (caps=on) → the partitioner CLAIMS the op for the coopvec provider → the native cooperative-vector MLP
// dispatch (fp16 tensor units). Same op, same weights → the two heterogeneous strategies agree within fp16 tol. ⛔ weights AND
// inputs are PRE-ROUNDED through fp16 for BOTH legs, so the tolerance measures "same computation", not weight-quant error. ⛔
// HONEST NVIDIA-GATED SCOREBOARD: on a device without cooperative_vector() the crown proves the CKIR partition (vs the f32
// oracle) + WARN-notes that the coopvec partition is unavailable; on NVIDIA it proves BOTH + their agreement. CUDA = a 24z ledger
// row. Dims x[4,8]·W1[8,8]·relu·W2[8,2] (M·hidden=32 for relu.ckir + a valid CoopVecMlpConfig).
TEST_CASE("ceir 24c-3: the sec-136 crown -- ONE ml.mlp, two partition strategies (CKIR-expand vs coopvec-native) agree",
          "[ceir][ml][gpu][coopvec]")
{
    namespace nn = crd::kir::neural;
    constexpr crd::u32 n_s = 4U;
    constexpr crd::u32 d0  = 8U;
    constexpr crd::u32 d1  = 8U;
    constexpr crd::u32 d2  = 2U;

    // ── shared: fp16-PRE-ROUNDED weights + inputs (both legs see the SAME fp16 values) + the f32 MLP oracle ──
    const auto r16 = [](float v) { return crd::math::f16_bits_to_f32(crd::math::f32_to_f16_bits(v)); };
    float      w1[d0 * d1];
    float      w2[d1 * d2];
    for (crd::u32 i = 0; i < d0; ++i)
    {
        for (crd::u32 j = 0; j < d1; ++j) { w1[i * d1 + j] = r16(0.05F * static_cast<float>(i + 1U) - 0.031F * static_cast<float>(j + 1U)); }
    }
    for (crd::u32 i = 0; i < d1; ++i)
    {
        for (crd::u32 j = 0; j < d2; ++j) { w2[i * d2 + j] = r16(0.1F * static_cast<float>((i % 3U) + 1U) - 0.043F * static_cast<float>(j + 1U)); }
    }
    float x_in[n_s * d0];
    for (crd::u32 s = 0; s < n_s; ++s)
    {
        for (crd::u32 c = 0; c < d0; ++c)
        {
            x_in[s * d0 + c] = r16(0.2F * static_cast<float>(static_cast<int>(s) - 1) + 0.06F * static_cast<float>(static_cast<int>(c) - 4));
        }
    }
    float oracle[n_s * d2] = {};
    for (crd::u32 s = 0; s < n_s; ++s)
    {
        float h1[d1];
        for (crd::u32 n = 0; n < d1; ++n)
        {
            float acc = 0.0F;
            for (crd::u32 c = 0; c < d0; ++c) { acc += x_in[s * d0 + c] * w1[c * d1 + n]; }
            h1[n] = acc < 0.0F ? 0.0F : acc; // relu
        }
        for (crd::u32 o = 0; o < d2; ++o)
        {
            float acc = 0.0F;
            for (crd::u32 n = 0; n < d1; ++n) { acc += h1[n] * w2[n * d2 + o]; }
            oracle[s * d2 + o] = acc;
        }
    }

    // ── DEVICE (soft-skip: no adapter) ──
    crd::gpu::GpuContextConfig gcfg;
    gcfg.backend  = crd::gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto devctx   = crd::gpu::create_vulkan_gpu_context(gcfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-24c-3 crown"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    // helper: build the ml.mlp module (captures x/W1/W2 Values for seeding), return the op + values.
    const auto build_mlp = [&](ce::Context& ctx, ce::Module*& m, ce::Value*& xv, ce::Value*& w1v, ce::Value*& w2v) {
        (void)ce::func::register_dialect(ctx);
        (void)ce::resource::register_resource_ops(ctx);
        (void)ce::arith::register_arith_ops(ctx);
        (void)ce::compute::register_compute_ops(ctx);
        (void)ce::linalg::register_dialect(ctx);
        (void)ce::ml::register_dialect(ctx);
        m               = ctx.create_module();
        ce::Block* top  = m->body()->first_block();
        if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
        ce::Operation* const f = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
        top->append(f);
        ce::Block* const b   = ce::func::func_body_block(f);
        const ce::OpId   dcl = ctx.intern_op("resource", "declare");
        const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
        xv                   = mkd(tf(ctx, sh2(ctx, n_s, d0)));
        w1v                  = mkd(tf(ctx, sh2(ctx, d0, d1)));
        w2v                  = mkd(tf(ctx, sh2(ctx, d1, d2)));
        ce::Value*           mlpops[3] = {xv, w1v, w2v};
        ce::Operation* const mo        = ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mlpops, 3U),
                                                              1U, tf(ctx, sh2(ctx, n_s, d2)), 0U);
        ctx.set_attr(mo, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
        b->append(mo);
        return mo;
    };
    const ceg::MlProvider prov_off = {crd::containers::StringView("coopvec"), false, &ceg::coopvec_can_claim_mlp};
    const ceg::MlProvider prov_on  = {crd::containers::StringView("coopvec"), true, &ceg::coopvec_can_claim_mlp};

    // ── RUN A: caps OFF -> CKIR FALLBACK partition -> apply_partition (expand) -> plan -> execute (the portable f32 path) ──
    float d_ckir[n_s * d2] = {};
    {
        crd::memory::GrowableTlsfAllocator root;
        ce::Context                        ctx(&root);
        ce::Module*                        m   = nullptr;
        ce::Value*                         xv  = nullptr;
        ce::Value*                         w1v = nullptr;
        ce::Value*                         w2v = nullptr;
        (void)build_mlp(ctx, m, xv, w1v, w2v);
        const ceg::MlPartition part = ceg::partition_ml(ctx, *m, crd::containers::ConstSpan<ceg::MlProvider>(&prov_off, 1U), &root);
        REQUIRE(part.fallback() == 1U); // caps off -> the mlp falls back to CKIR
        REQUIRE(ceg::apply_partition(ctx, *m, part).error == ceg::MlExpandError::None);
        const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
        REQUIRE(plan.reject == ceg::PlanReject::None);
        const ce::Value* out_val = nullptr;
        for (crd::usize i = 0; i < plan.buffers.size(); ++i)
        {
            if (plan.buffers[i].role == ceg::BufferRole::Output) { out_val = plan.buffers[i].value; }
        }
        REQUIRE(out_val != nullptr);
        const QuantSeed seeds[3] = {{xv, x_in, nullptr, n_s * d0}, {w1v, w1, nullptr, d0 * d1}, {w2v, w2, nullptr, d1 * d2}};
        REQUIRE(run_quant_module(compute, ctx, &root, plan, seeds, 3U, out_val, d_ckir, n_s * d2));
    }
    // the CKIR partition (f32) tracks the f32 oracle tightly.
    for (crd::u32 i = 0; i < n_s * d2; ++i) { CHECK(crd::math::abs(d_ckir[i] - oracle[i]) <= 2e-3F * (1.0F + crd::math::abs(oracle[i]))); }

    if (!vk->cooperative_vector())
    {
        WARN("no VK_NV_cooperative_vector (non-NVIDIA) — the crown proved the CKIR partition; the coopvec partition needs NVIDIA");
        return;
    }

    // ── RUN B: caps ON -> the coopvec provider CLAIMS the op -> native cooperative-vector MLP dispatch (fp16 tensor units) ──
    float d_coop[n_s * d2] = {};
    {
        crd::memory::GrowableTlsfAllocator root;
        ce::Context                        ctx(&root);
        ce::Module*                        m   = nullptr;
        ce::Value*                         xv  = nullptr;
        ce::Value*                         w1v = nullptr;
        ce::Value*                         w2v = nullptr;
        ce::Operation* const               mo  = build_mlp(ctx, m, xv, w1v, w2v);
        (void)xv;
        (void)w1v;
        (void)w2v;
        const ceg::MlPartition part = ceg::partition_ml(ctx, *m, crd::containers::ConstSpan<ceg::MlProvider>(&prov_on, 1U), &root);
        REQUIRE(part.claimed_by(0) == 1U); // caps on -> the coopvec provider claims the mlp
        const nn::CoopVecMlpConfig mlp = ceg::coopvec_config_from_mlp(ctx, mo);
        REQUIRE(mlp.valid());
        const float* wptrs[2] = {&w1[0], &w2[0]};
        crd::containers::Array<crd::u16> wf16(&root);
        wf16.resize(static_cast<crd::usize>(mlp.weight_count()));
        REQUIRE(ceg::coopvec_weights_from_mlp(mlp, wptrs, wf16.data()));
        crd::containers::Array<crd::u16> bf16(&root);
        bf16.resize(static_cast<crd::usize>(mlp.bias_count()), crd::math::f32_to_f16_bits(0.0F));
        crd::containers::Array<crd::u16> in_h(&root);
        in_h.resize(static_cast<crd::usize>(n_s) * d0);
        for (crd::u32 i = 0; i < n_s * d0; ++i) { in_h[i] = crd::math::f32_to_f16_bits(x_in[i]); }

        crd::kir::GlslKernel kern(&root);
        REQUIRE(nn::emit_coopvec_mlp_glsl(mlp, kern));
        const auto spv = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                         "ceir_crown_coopvec", &root);
        REQUIRE(spv.ok);
        auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 5, 0U);
        REQUIRE(pipe != nullptr);
        using crd::gpu::compute_usage::storage;
        using crd::gpu::compute_usage::transfer_dst;
        using crd::gpu::compute_usage::transfer_src;
        auto       d_in  = compute.create_buffer(static_cast<crd::u64>(n_s * d0) * 2U, storage | transfer_dst, crd::gpu::ComputeMemory::GpuOnly);
        auto       d_w   = compute.create_buffer(static_cast<crd::u64>(mlp.weight_count()) * 2U, storage | transfer_dst, crd::gpu::ComputeMemory::GpuOnly);
        auto       d_b   = compute.create_buffer(static_cast<crd::u64>(mlp.bias_count()) * 2U, storage | transfer_dst, crd::gpu::ComputeMemory::GpuOnly);
        auto       d_out = compute.create_buffer(static_cast<crd::u64>(n_s * d2) * 2U, storage | transfer_src, crd::gpu::ComputeMemory::GpuOnly);
        auto       d_cfg = compute.create_buffer(16U, storage | transfer_dst, crd::gpu::ComputeMemory::GpuOnly);
        const auto up    = [&](crd::gpu::ComputeBuffer& dst, const void* src, crd::u64 nbytes) {
            auto        stg  = compute.create_buffer(nbytes, transfer_src, crd::gpu::ComputeMemory::CpuToGpu);
            auto*       p    = static_cast<crd::u8*>(stg->map());
            const auto* srcb = static_cast<const crd::u8*>(src);
            for (crd::u64 i = 0; i < nbytes; ++i) { p[i] = srcb[i]; }
            stg->unmap();
            auto& rc = compute.begin();
            rc.copy(*stg, dst, 0U, 0U, nbytes);
            compute.submit_and_wait();
        };
        const crd::u32 cfgv[4] = {n_s, 0U, 0U, 0U};
        up(*d_in, in_h.data(), static_cast<crd::u64>(n_s * d0) * 2U);
        up(*d_w, wf16.data(), static_cast<crd::u64>(mlp.weight_count()) * 2U);
        up(*d_b, bf16.data(), static_cast<crd::u64>(mlp.bias_count()) * 2U);
        up(*d_cfg, cfgv, 16U);
        auto&                    rec      = compute.begin();
        crd::gpu::ComputeBuffer* binds[5] = {d_in.get(), d_w.get(), d_b.get(), d_out.get(), d_cfg.get()};
        rec.dispatch(*pipe, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(binds, 5), nullptr, 0U, (n_s + 63U) / 64U, 1U, 1U);
        rec.barrier(*d_out, crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::TransferSrc);
        compute.submit_and_wait();
        auto rb = compute.create_buffer(static_cast<crd::u64>(n_s * d2) * 2U, transfer_dst, crd::gpu::ComputeMemory::GpuToCpu);
        {
            auto& r2 = compute.begin();
            r2.copy(*d_out, *rb, 0U, 0U, static_cast<crd::u64>(n_s * d2) * 2U);
            compute.submit_and_wait();
        }
        const auto* out = static_cast<const crd::u16*>(rb->map());
        for (crd::u32 i = 0; i < n_s * d2; ++i) { d_coop[i] = crd::math::f16_bits_to_f32(out[i]); }
        rb->unmap();
    }

    // ⭐ THE CROWN: the coopvec-native partition tracks the f32 oracle within fp16 tol, AND the two heterogeneous partitions
    //    (CKIR f32 expansion vs coopvec fp16 native) compute the SAME numbers for the SAME ml.mlp.
    for (crd::u32 i = 0; i < n_s * d2; ++i)
    {
        CHECK(crd::math::abs(d_coop[i] - oracle[i]) <= 3e-2F * (1.0F + crd::math::abs(oracle[i]))); // coopvec fp16 == oracle
        CHECK(crd::math::abs(d_ckir[i] - d_coop[i]) <= 3e-2F * (1.0F + crd::math::abs(oracle[i]))); // the two partitions AGREE
    }
}

TEST_CASE("ceir 25b-4a: a transpose+broadcast+elementwise chain runs device-resident on Vulkan (the new StageKinds, multi-stage)",
          "[ceir][tensor-pipeline][gpu]")
{
    // ── build the backward-vocab chain: in0[2,3] --transpose[1,0]--> t[3,2] ; in1[3,1] --broadcast--> bc[3,2] ; add(t,bc)-> e[3,2].
    //    Three NEW-kind stages in one device-resident pipeline (Transpose→..., Broadcast→..., Elementwise consumes BOTH). ──
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

    // ── PLAN (device-free, ALWAYS runs — the all-skip guard) ──
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 3U);

    // ── the INDEPENDENT reference: e[r,c] = t[r,c] + bc[r,c] = in0[c*3+r] (transpose) + in1[r] (broadcast down the row) ──
    static float in0_data[6] = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
    static float in1_data[3] = {10.0F, 20.0F, 30.0F};
    float              ref[6];
    for (int r = 0; r < 3; ++r) { for (int cc = 0; cc < 2; ++cc) { ref[r * 2 + cc] = in0_data[cc * 3 + r] + in1_data[r]; } }

    // ── DEVICE (soft-skip with no adapter) ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-25b-4a chain gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    const QuantSeed seeds[2] = {{in0, in0_data, nullptr, 6U}, {in1, in1_data, nullptr, 3U}};
    float           got[6]   = {};
    REQUIRE(run_quant_module(compute, ctx, &root, plan, seeds, 2U, ew->result(0U), got, 6U));
    for (int i = 0; i < 6; ++i) { CHECK(got[i] == ref[i]); } // pure data-movement + add of exact f32 ⇒ EXACT
}

// CEIR-25b-4b / 25c-2: the FD-witness functors SumGemmAA + MlpLoss are SHARED with the DX12 pipeline TU — ONE definition in
// gpu-shared/autodiff_fd_functors.hpp (pure f64/templated, no backend dependency; the "forward is hesap, not hand-written" rationale
// lives there). Hoisted at 25c-2b close (2nd distinct FD functor × 2 TUs = 4 copies).
namespace
{
using crd::ceir::gpu_test::MlpLoss;
using crd::ceir::gpu_test::SumGemmAA;
} // namespace

TEST_CASE("ceir 25b-4b: the backward pass of sum(gemm(A,A)) runs device-resident on Vulkan (build_gradient->plan->execute) == hesap matmul_vjp + FD",
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
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-25b-4b backward-pass gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

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

// CEIR-25c-1b — the STANDALONE relu_vjp device gate. An authored compute.dispatch(@relu_vjp, {x, gy, gx} r,r,w) — the MLP-backward
// relu VJP (gx = x>0 ? gy : 0, x the forward PRE-activation) — is PLANNED (one VizDispatch stage) and EXECUTED device-resident on
// Vulkan, matching hesap nn_reverse::relu_vjp. relu_vjp.ckir is the FIRST authored kernel with READONLY-declared inputs (x@0/gy@1
// axes=0 readonly, gx@2 axes=1 writable) — the GLSL emitter emits `readonly buffer` for x/gy (ckir_glsl.hpp:735), a codegen path NO
// prior gate exercised (relu.ckir's in AND out are writable). ⛔ a device-numeric match alone does NOT prove the readonly path was
// TAKEN (a kernel emitting a plain `buffer` for x/gy passes IDENTICALLY), so the gate ALSO asserts `readonly buffer` is in the
// emitted GLSL — identity, not category. 32 elements = the baked local_size (one workgroup); x spans neg/ZERO/pos (hesap relu_vjp is
// STRICT `>0`, so gx[x==0]==0 is the discriminating sample vs a `>=`/step() formulation); pure select ⇒ EXACT `==`.
TEST_CASE("ceir 25c-1b: the authored relu_vjp compute kernel (readonly inputs) runs device-resident on Vulkan == hesap relu_vjp",
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
    //    the Output/readback target). x,gy are ExternalIn (read, no writer); gx a WRITE-THROUGH-DECLARE (resultless dispatch → no SSA
    //    edge to the declare — correctness rides submit order; the dedicated plan gate for that pattern is 25c-1b-2). ──
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

    // ⛔ READONLY CODEGEN (independent of the device run): emit the asset's GLSL and CONFIRM `readonly buffer` is present — proves the
    //    readonly-declared inputs (x@0/gy@1) emit as readonly SSBOs (ckir_glsl.hpp:735), the path this gate exists to exercise. A
    //    device-numeric pass alone cannot tell "readonly codegen works" from "readonly silently dropped".
    kir::KGraph     rg(&root);
    kir::GlslKernel rk(&root);
    REQUIRE(load_emit_ckir(CRD_REPO_DIR "/assets/ckir/relu_vjp.ckir", rg, rk, &root, nel)); // 26d-4b: cook-bind the sentinel before the codegen emit
    CHECK(std::strstr(rk.source.c_str(), "readonly buffer") != nullptr);

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
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-25c-1b relu_vjp gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    const QuantSeed seeds[2] = {{x, x_f, nullptr, nel}, {gy, gy_f, nullptr, nel}};
    float           got[nel] = {};
    REQUIRE(run_quant_module(compute, ctx, &root, plan, seeds, 2U, gx, got, static_cast<crd::usize>(nel)));
    for (crd::u32 i = 0; i < nel; ++i) { CHECK(got[i] == static_cast<float>(gx_ref[i])); } // pure select ⇒ EXACT
}

// CEIR-25c-2 — the §138 ML PROOF crown (MLP-backward leg): an ml.mlp(x[8,4], W1[4,4], W2[4,4]){relu} differentiated by vjp_mlp (the
// COMPOSITE, before expansion), then PLANNED (forward composite ERASED — 25c-1b-2a contract) and EXECUTED device-resident on Vulkan,
// reading back BOTH gradients — dW1 (Output) AND dW2 (Intermediate, via the (b) by-Value readback) — vs a DIALECT-INDEPENDENT hesap
// reference (forward matmul/relu + backward matmul_vjp/relu_vjp composed, itself FD-cross-validated on L = <M, mlp(x,W)>). The seed
// dLoss = M is NON-UNIFORM (fixes the 25b-4b all-ones-dC scope gap). ⛔ the relu_vjp mask must BITE (some z1 <= 0) or the backward
// proves nothing about relu_vjp — asserted on the actual forward. The strict `>0` boundary (z1 == 0) is owned by the 25c-1b-1 gate.
TEST_CASE("ceir 25c-2: the vjp_mlp backward of a 2-layer MLP runs device-resident on Vulkan (dW1+dW2) == hesap matmul_vjp+relu_vjp + FD",
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
    constexpr int    d1      = 4; // hidden: m*d1 = 8*4 = 32 = relu.ckir / relu_vjp.ckir baked local_size
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
    // erase the DEAD forward composite (the backward recomputes; plan_tensor_pipeline TYPED-REJECTS ml.mlp) — the 25c-1b-2a contract.
    REQUIRE(!mlp->result(0U)->has_uses());
    mlp->erase();
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);

    // ── data: x ALL-POSITIVE + W1 columns cleanly signed (cols 0,1 positive; 2,3 negative) ⇒ z1 columns are cleanly ± and WELL-SEPARATED
    //    from 0 (|z1| ~ O(1)) — so relu BITES (cols 2,3 dead) AND the FD witness stays OFF the relu kink (a kink corrupts central
    //    differences — gradient_check.hpp: FD "NOT valid at kinks"). M non-uniform. Deterministic. ──
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
    for (int i = 0; i < mrows * d2; ++i) { md[i] = 0.2 + 0.11 * static_cast<double>(i); } // NON-uniform mask

    // ── the DIALECT-INDEPENDENT reference (all hesap): forward matmul/relu -> z1,h1,z2; backward matmul_vjp/relu_vjp composed. ──
    namespace nnr = crd::hesap::autodiff::reverse::nn;
    double z1[mrows * d1];
    double h1[mrows * d1];
    double z2[mrows * d2];
    nnr::matmul(xd, w1d, z1, mrows, d0, d1);
    nnr::relu(z1, h1, mrows * d1);
    nnr::matmul(h1, w2d, z2, mrows, d1, d2);
    // ⛔ the relu_vjp mask must BITE: some z1 <= 0 AND some z1 > 0 (else relu_vjp is the identity and the crown proves nothing about it).
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
    REQUIRE(minabs > 0.05); // z1 bounded OFF the relu kink so the FD witness is valid (central differences corrupt at a kink)
    // ⛔ M NON-uniform (the 25b-4b all-ones-dC gap): at least two distinct entries.
    bool m_distinct = false;
    for (int i = 0; i < mrows * d2 && !m_distinct; ++i)
    {
        for (int j = i + 1; j < mrows * d2; ++j) { if (md[i] != md[j]) { m_distinct = true; break; } }
    }
    REQUIRE(m_distinct);

    double dz2[mrows * d2];
    for (int i = 0; i < mrows * d2; ++i) { dz2[i] = md[i]; } // dLoss/dout = M (loss = <M, out>)
    double dh1[mrows * d1];
    double dw2ref[d1 * d2];
    nnr::matmul_vjp(h1, w2d, dz2, dh1, dw2ref, mrows, d1, d2); // dh1 = dz2·W2ᵀ; dW2 = h1ᵀ·dz2
    double dz1[mrows * d1];
    nnr::relu_vjp(z1, dh1, dz1, mrows * d1); // dz1 = (z1>0)?dh1:0
    double dxref[mrows * d0];
    double dw1ref[d0 * d1];
    nnr::matmul_vjp(xd, w1d, dz1, dxref, dw1ref, mrows, d0, d1); // dW1 = xᵀ·dz1

    // ⛔ the column-signed corpus makes half the hidden units DEAD (cols 2,3: z1<0 ⇒ relu=0 ⇒ relu_vjp=0), so BOTH gradients carry a
    //    PROVEN zero block that a dropped / wrong-half relu_vjp (or relu forward) would break: dW1[:,c>=2]==0 (dz1 zeroed) and
    //    dW2[c>=2,:]==0 (h1 zeroed); the c<2 blocks are meaningfully nonzero. Discriminating power the FD kink-fix did NOT trade away.
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

    // ── FD witness on L(W) = <M, mlp(x,W)>, W = [W1;W2] flattened — validates the analytic ref BEFORE it judges the device. ──
    constexpr int nw = d0 * d1 + d1 * d2;
    double        wflat[nw];
    for (int i = 0; i < d0 * d1; ++i) { wflat[i] = w1d[i]; }
    for (int i = 0; i < d1 * d2; ++i) { wflat[d0 * d1 + i] = w2d[i]; }
    double gfd[nw];
    crd::hesap::autodiff::testing::grad_fd<nw>(MlpLoss{xd, md, mrows, d0, d1, d2}, wflat, gfd);
    for (int i = 0; i < d0 * d1; ++i) { CHECK(crd::math::abs(gfd[i] - dw1ref[i]) <= 1e-5 * (1.0 + crd::math::abs(dw1ref[i]))); }
    for (int i = 0; i < d1 * d2; ++i) { CHECK(crd::math::abs(gfd[d0 * d1 + i] - dw2ref[i]) <= 1e-5 * (1.0 + crd::math::abs(dw2ref[i]))); }

    // ── DEVICE (soft-skip with no adapter): seed x, W1, W2, dLoss=M BY VALUE; read back BOTH dW1 (Output) + dW2 (Intermediate, via (b)). ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-25c-2 MLP backward gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

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
    // device f32 (chained matmuls + relu_vjp) vs the f64 analytic ref — DERIVED relative tolerance (small dots, |ref| ~ O(1)).
    for (int i = 0; i < d0 * d1; ++i) { CHECK(crd::math::abs(static_cast<double>(dw1got[i]) - dw1ref[i]) <= 1e-4 * (1.0 + crd::math::abs(dw1ref[i]))); }
    for (int i = 0; i < d1 * d2; ++i) { CHECK(crd::math::abs(static_cast<double>(dw2got[i]) - dw2ref[i]) <= 1e-4 * (1.0 + crd::math::abs(dw2ref[i]))); }

    // ── CEIR-26a-3 DCE DIFFERENTIAL (leg (b) of the band-lock): for a SEMANTICS-PRESERVING pass the reference is NOT hesap-within-tol
    //    (a 1e-5 perturbation would pass) — it is the RAW program's OWN device output, BIT-EXACT. PIN the readback gradients first (a
    //    func.return roots them — grads[] are readback-by-Value, NOT SSA-consumed, so a naive DCE would delete them; the planner SKIPS
    //    func.return so the plan is unchanged), run dce_run (prunes the dead dx branch → 11→9 stages), re-plan + re-execute the SAME
    //    corpus on the SAME device with the SAME seeds, and require dce'd == raw for every element. See dce.hpp LIVENESS CONTRACT. ──
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
    // the DCE'd output also stands alone vs hesap (transitivity, but reads as "dce'd == hesap" on its own).
    for (int i = 0; i < d0 * d1; ++i) { CHECK(crd::math::abs(static_cast<double>(dw1got_dce[i]) - dw1ref[i]) <= 1e-4 * (1.0 + crd::math::abs(dw1ref[i]))); }
    for (int i = 0; i < d1 * d2; ++i) { CHECK(crd::math::abs(static_cast<double>(dw2got_dce[i]) - dw2ref[i]) <= 1e-4 * (1.0 + crd::math::abs(dw2ref[i]))); }
}

// CEIR-26d-4b (Vulkan) — the vjp shape-specialization PROVING gate (the grad.cpp MlpBakedShapeUnsupported reject FLIPS to a run):
// the SAME vjp_mlp backward as 25c-2 but at a NON-32 interior width — ml.mlp(x[8,4], W1[4,8], W2[8,4]){relu}, so the interior z1 is
// [8,8] = 64 elements (M·hidden = 64 ≠ 32, previously MlpBakedShapeUnsupported). relu.ckir (recompute) + relu_vjp.ckir (backward)
// both cook-bind local_size to the intermediate numel via the 26d-4 sentinel, so the vjp runs device-resident. Reads back BOTH dW1
// (Output) + dW2 (Intermediate, by-Value) == the DIALECT-INDEPENDENT hesap reference (matmul_vjp+relu_vjp), FD-cross-validated. The
// column-signed W1 (cols [0,4) +, [4,8) −) makes half the hidden units DEAD ⇒ PROVEN zero blocks a dropped/wrong-half relu_vjp
// breaks. ⛔ NO "raw" for a previously-REJECTED program (the 24b device-vs-oracle standard). ⛔ exp-free — f32 chained matmul tol.
TEST_CASE("ceir 26d-4b: the vjp_mlp backward at a NON-32 interior width (h1=64) runs device-resident on Vulkan (dW1+dW2) == hesap",
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
    REQUIRE(gr.error == ceg::GradError::None); // ⛔ 26d-4b: was MlpBakedShapeUnsupported at h1=64 before the grad pre-check retired
    REQUIRE(grads[0] != nullptr);
    REQUIRE(grads[1] != nullptr);
    REQUIRE(gr.seed != nullptr);
    REQUIRE(!mlp->result(0U)->has_uses());
    mlp->erase();
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(plan.reject == ceg::PlanReject::None);

    // ── data: x ALL-POSITIVE; W1 columns cleanly ± (cols [0,dhalf) +, [dhalf,d1) −) ⇒ z1 columns cleanly signed, WELL-SEPARATED from
    //    0 ⇒ relu BITES (dead cols) AND the FD witness stays OFF the kink. dLoss = M NON-uniform. Deterministic. ──
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
    for (int i = 0; i < mrows * d2; ++i) { md[i] = 0.2 + 0.11 * static_cast<double>(i); } // NON-uniform mask

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
    REQUIRE(minabs > 0.05); // z1 off the relu kink so the FD witness is valid

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

    // ⛔ the dead-column blocks (c >= dhalf) are a PROVEN zero a dropped / wrong-half relu_vjp breaks: dW1[:,c>=dhalf]==0 (dz1 zeroed)
    //    and dW2[c>=dhalf,:]==0 (h1 zeroed); the c<dhalf blocks are meaningfully nonzero.
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

    // ── FD witness on L(W) = <M, mlp(x,W)> — validates the analytic ref BEFORE it judges the device. ──
    constexpr int nw = d0 * d1 + d1 * d2;
    double        wflat[nw];
    for (int i = 0; i < d0 * d1; ++i) { wflat[i] = w1d[i]; }
    for (int i = 0; i < d1 * d2; ++i) { wflat[d0 * d1 + i] = w2d[i]; }
    double gfd[nw];
    crd::hesap::autodiff::testing::grad_fd<nw>(MlpLoss{xd, md, mrows, d0, d1, d2}, wflat, gfd);
    for (int i = 0; i < d0 * d1; ++i) { CHECK(crd::math::abs(gfd[i] - dw1ref[i]) <= 1e-5 * (1.0 + crd::math::abs(dw1ref[i]))); }
    for (int i = 0; i < d1 * d2; ++i) { CHECK(crd::math::abs(gfd[d0 * d1 + i] - dw2ref[i]) <= 1e-5 * (1.0 + crd::math::abs(dw2ref[i]))); }

    // ── DEVICE (soft-skip with no adapter): seed x, W1, W2, dLoss=M BY VALUE; read back BOTH dW1 (Output) + dW2 (Intermediate). ──
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device — skipping the CEIR-26d-4b non-32 vjp gate"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

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

// CEIR-26b-2b (Vulkan) — the canonicalize reshape-fold BIT-EXACT vs the RAW device output (the 26a semantics-preserving-pass
// standard applied to 26b). A sec-137 gemm[8,8]->reshape[2,32]->reshape[64]->fft->reduce pipeline (the DOUBLE reshape rooted at
// the GEMM output: T_out=[64] != T_in=[8,8], so the fold fires WITHOUT hitting the identity exclusion) runs RAW on a real Vulkan
// device; then canonicalize (fold reshape-of-reshape) + dce (reclaim the dead reshapes) collapse it to the single-reshape
// pipeline; it re-runs and the reduce scalar is BIT-EXACT vs the raw one (reshape is a pure reinterpret sharing the SAME physical
// buffer, so the fft reads identical bytes). Both also match the independent composed ref within the propagated tol.
TEST_CASE("ceir 26b-2: canonicalize reshape-fold is BIT-EXACT vs the RAW pipeline on Vulkan (sec-137)", "[ceir][tensor-pipeline][gpu]")
{
    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
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
    // the DOUBLE reshape rooted at the GEMM output: [8,8] -> [2,32] -> [64]. The outer's T_out=[64] != T_in (gemm [8,8]), so the
    // fold fires and collapses it to reshape([8,8] -> [64]) (the original single-reshape sec-137 pipeline).
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
    // PIN the terminal reduce output (26a-2 liveness contract) so DCE keeps the reduce->reshape chain; the planner skips
    // func.return so the plan is unaffected.
    ce::Value* const rets[1] = {rd->result(0U)};
    b->append(ce::func::create_return(ctx, crd::containers::ConstSpan<ce::Value*>(rets, 1U)));

    // a/b data + the INDEPENDENT composed ref (triple-loop GEMM -> naive DFT -> serial sum, f64), mirroring 22c-2.
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
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device -- skipping the CEIR-26b-2b reshape-fold differential"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    // the runner: materialize the plan (Alias shares its target; ExternalIn seeded a/b + fft twiddles; Intermediate GpuOnly;
    // Output GpuToCpu), execute as ONE submit, read back the reduce scalar. (run_quant_module_n zero-fills the twiddles.)
    const auto run = [&](const ceg::TensorPipelinePlan& plan) -> float {
        const crd::usize                         nb = plan.buffers.size();
        std::unique_ptr<crd::gpu::ComputeBuffer> owned[32];
        crd::gpu::ComputeBuffer*                 bufs[32] = {};
        REQUIRE(nb <= 32U);
        for (crd::usize i = 0; i < nb; ++i)
        {
            const ceg::PlanBuffer& pb = plan.buffers[i];
            if (pb.role == ceg::BufferRole::Alias) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
            crd::gpu::ComputeMemory mem = crd::gpu::ComputeMemory::GpuOnly;
            if (pb.role == ceg::BufferRole::ExternalIn) { mem = crd::gpu::ComputeMemory::CpuToGpu; }
            else if (pb.role == ceg::BufferRole::Output) { mem = crd::gpu::ComputeMemory::GpuToCpu; }
            owned[i] = compute.create_buffer(pb.bytes, crd::gpu::compute_usage::storage, mem);
            REQUIRE(owned[i] != nullptr);
            bufs[i] = owned[i].get();
            if (pb.role == ceg::BufferRole::ExternalIn)
            {
                auto* dst = static_cast<float*>(owned[i]->map());
                REQUIRE(dst != nullptr);
                const crd::u64 cnt = pb.bytes / 4ULL;
                if (pb.value == a_in) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = a_data[e]; } }
                else if (pb.value == b_in) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = b_data[e]; } }
                else { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = 0.0F; } }
                owned[i]->unmap();
            }
        }
        for (crd::usize s = 0; s < plan.stages.size(); ++s)
        {
            const ceg::PlanStage& st = plan.stages[s];
            if (st.kind != ceg::StageKind::Fft) { continue; }
            const crd::i32 btr  = st.bind[2];
            const crd::i32 bti  = st.bind[3];
            const int      half = static_cast<int>(plan.buffers[static_cast<crd::usize>(btr)].bytes / 4ULL);
            const int      n    = half * 2;
            auto*          tr   = static_cast<float*>(owned[static_cast<crd::usize>(btr)]->map());
            auto*          ti   = static_cast<float*>(owned[static_cast<crd::usize>(bti)]->map());
            REQUIRE(tr != nullptr);
            REQUIRE(ti != nullptr);
            for (int kk = 0; kk < half; ++kk)
            {
                const crd::f64 aa = two_pi * static_cast<crd::f64>(kk) / static_cast<crd::f64>(n);
                tr[kk]            = static_cast<float>(crd::math::cos(aa));
                ti[kk]            = static_cast<float>(-crd::math::sin(aa));
            }
            owned[static_cast<crd::usize>(btr)]->unmap();
            owned[static_cast<crd::usize>(bti)]->unmap();
        }
        Resolver res;
        res.alloc_ctx = &ctx;
        res.alloc     = &root;
        res.compute   = &compute;
        crd::i32 out_idx = -1;
        int      n_out   = 0;
        for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].role == ceg::BufferRole::Output) { out_idx = static_cast<crd::i32>(i); ++n_out; } }
        REQUIRE(out_idx >= 0);
        REQUIRE(n_out == 1); // exactly ONE Output (the rank-0 reduce) — a 2nd would make bit-exact compare the wrong scalar to itself
        auto&                   rec = compute.begin();
        const ceg::ExecuteError ee  = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res,
                                                                   crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(bufs, nb));
        REQUIRE(ee == ceg::ExecuteError::None);
        rec.barrier(*bufs[static_cast<crd::usize>(out_idx)], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
        compute.submit_and_wait();
        const auto* got = static_cast<const float*>(owned[static_cast<crd::usize>(out_idx)]->map());
        REQUIRE(got != nullptr);
        const float v = got[0];
        owned[static_cast<crd::usize>(out_idx)]->unmap();
        return v;
    };

    // RAW: 3 stages (Gemm, Fft, Reduce; the TWO reshapes are aliases), 2 Alias buffers.
    const ceg::TensorPipelinePlan raw_plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(raw_plan.reject == ceg::PlanReject::None);
    REQUIRE(raw_plan.stages.size() == 3U);
    int raw_alias = 0;
    for (crd::usize i = 0; i < raw_plan.buffers.size(); ++i)
    {
        if (raw_plan.buffers[i].role != ceg::BufferRole::Alias) { continue; }
        ++raw_alias;
        // ⛔ the runner's one-level bufs[i]=bufs[alias_of] resolves the r_out->r_in->gemm chain transitively ONLY because plan
        //    buffers are walk-ordered producer-first (alias_of < i). GATE it: a reorder would silently null-deref the raw run.
        REQUIRE(raw_plan.buffers[i].alias_of >= 0);
        REQUIRE(static_cast<crd::usize>(raw_plan.buffers[i].alias_of) < i);
    }
    REQUIRE(raw_alias == 2);
    const float raw_s = run(raw_plan);

    // canonicalize (fold reshape-of-reshape) + dce (reclaim the dead reshapes).
    ce::DiagnosticEngine diag(ctx, &root);
    REQUIRE(ce::canonicalize_run(ctx, *m, diag));
    REQUIRE(ce::dce_run(ctx, *m, diag));
    REQUIRE_FALSE(diag.has_fatal());

    // OPT: SAME 3 stages (0 stage shift -- reshape is an alias, never a stage), now 1 Alias buffer.
    const ceg::TensorPipelinePlan opt_plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(opt_plan.reject == ceg::PlanReject::None);
    REQUIRE(opt_plan.stages.size() == 3U);
    int opt_alias = 0;
    for (crd::usize i = 0; i < opt_plan.buffers.size(); ++i) { if (opt_plan.buffers[i].role == ceg::BufferRole::Alias) { ++opt_alias; } }
    CHECK(opt_alias == 1); // 2 -> 1
    const float opt_s = run(opt_plan);

    // BIT-EXACT: the fold is semantics-preserving -- the collapsed pipeline reads the SAME physical gemm buffer, so the fft sees
    // identical bytes and the reduce scalar is bit-identical (the 26a differential-test standard, NOT a tolerance-vs-oracle).
    // ⛔ WITNESS STRENGTH (advisor): this fold touches only ALIAS buffers -- the 3 dispatches are identical before/after, so
    //    bit-exact here proves alias-WIRING preservation (the "wrong T_out -> fft reads a wrong-shaped alias" concern), NOT
    //    kernel-level correctness (there is no kernel change to miscompile). A DISPATCHED-stage fold (26e fusion) is where this
    //    witness regains its 26a teeth (there a bad prune changes which kernels run).
    CHECK(opt_s == raw_s);
    // and both are CORRECT (not merely self-consistent) vs the independent composed ref within the propagated tol.
    const float tol = static_cast<float>(static_cast<crd::f64>(kL) * 2e-3 * maxmag);
    const float fa  = raw_s - static_cast<float>(s_ref);
    CHECK((fa < 0.0F ? -fa : fa) <= tol);
}

// CEIR-26c-2b (Vulkan) — CSE removing a DUPLICATE gemm DISPATCH is BIT-EXACT vs the RAW device output. This is the 26a
// semantics-preserving witness at FULL teeth again: unlike 26b's reshape fold (alias-only — the dispatches were identical
// before/after, so bit-exact proved only wiring), CSE deletes a real DISPATCH — the OPT program runs ONE gemm where RAW ran two,
// and a miscompile (the survivor is not a valid substitute for the erased twin, or the rewired consumer reads a wrong buffer)
// changes the output. Corpus: two IDENTICAL gemm(A,B,C):[8,8] feed two DISTINCT axis-0 reduces — `max` (control on d1) + `sum`
// (the REWIRED consumer, reader of the erased d2 → accumulation teeth). RAW plans 2 Gemm + 2 Reduce; cse_run collapses d2→d1
// (one gemm dispatch gone) ⇒ 1 Gemm + 2 Reduce; both reduce vectors are BIT-EXACT raw==opt + correct vs an independent f64 ref.
TEST_CASE("ceir 26c-2: CSE duplicate-gemm removal is BIT-EXACT vs the RAW pipeline on Vulkan (sec-137)", "[ceir][tensor-pipeline][gpu]")
{
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
    ce::Value* const a_in = mkd(tf(ctx, sh2(ctx, kSide, kSide)));
    ce::Value* const b_in = mkd(tf(ctx, sh2(ctx, kSide, kSide)));
    ce::Value* const c_in = mkd(tf(ctx, sh2(ctx, kSide, kSide)));
    // two STRUCTURALLY IDENTICAL gemms (same a/b/c operands + attrs) — CSE will collapse the later onto the earlier.
    const auto mkgemm = [&]() {
        ce::Operation* const g = ce::linalg::build_gemm(ctx, a_in, b_in, c_in, ctx.attr_float(1.0), ctx.attr_float(0.0),
                                                        ctx.attr_bool(false), ctx.attr_bool(false), tf(ctx, sh2(ctx, kSide, kSide)));
        b->append(g);
        return g;
    };
    ce::Operation* const g1 = mkgemm();
    ce::Operation* const g2 = mkgemm();
    // reshape each gemm output [8,8] -> [64] (a zero-copy alias) so each reduce is a FULL reduction to a SCALAR — the
    // device-proven emit_reduce_glsl path (out_n==1); a rank-2 partial-axis reduce is NOT emittable (emit_reduce_glsl rejects
    // out_n>1). CSE also merges the two now-identical reshapes (rs2->rs1) after the gemms collapse — harmless (aliases, 0 stages).
    ce::Operation* const rs1 = ce::tensor::build_reshape(ctx, g1->result(0U), tf(ctx, sh1(ctx, kL)));
    b->append(rs1);
    ce::Operation* const rs2 = ce::tensor::build_reshape(ctx, g2->result(0U), tf(ctx, sh1(ctx, kL)));
    b->append(rs2);
    const ce::TypeId scalar = tf(ctx, shp(ctx, crd::containers::ConstSpan<ce::TypeId>{})); // rank-0
    // TWO DISTINCT consumers keep BOTH gemms live in RAW (so there are two dispatches for CSE to collapse). ra = reduce(rs1,"max")
    // is a SELECTION (the wiring witness); rb = reduce(rs2,"sum") is the REWIRED consumer (reader of the to-be-erased d2's reshape)
    // and an ACCUMULATION over 64 elements ⇒ the load-bearing bit-exact teeth. Distinct fn ⇒ the reduces never CSE-merge.
    ce::Operation* const ra = ce::tensor::build_reduce(ctx, rs1->result(0U), ctx.attr_int(0),
                                                       ctx.attr_string(crd::containers::StringView("max")), scalar);
    b->append(ra);
    ce::Operation* const rb = ce::tensor::build_reduce(ctx, rs2->result(0U), ctx.attr_int(0),
                                                       ctx.attr_string(crd::containers::StringView("sum")), scalar);
    b->append(rb);
    ce::Value* const rets[2] = {ra->result(0U), rb->result(0U)}; // pin BOTH terminal reduces (26a-2 liveness contract)
    b->append(ce::func::create_return(ctx, crd::containers::ConstSpan<ce::Value*>(rets, 2U)));
    REQUIRE(ce::linalg::find_linalg_misuse(ctx, *m).kind == ce::linalg::LinalgMisuseKind::None);
    REQUIRE(ce::tensor::find_tensor_misuse(ctx, *m).kind == ce::tensor::TensorMisuseKind::None);

    // a/b data + the INDEPENDENT ref: d = A@B (f64); axis-0 reduce ⇒ out[j] = reduce_i d[i,j] (drop_axis_shape keeps the NON-reduced
    // axis). max_ref[j] = max_i d[i,j] (a selection); sum_ref[j] = sum_i d[i,j] (the accumulation).
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
            max_ref = max_ref > acc ? max_ref : acc; // max over all 64 gemm outputs
            sum_ref += acc;                          // sum over all 64
        }
    }
    const crd::f64 sum_abs = sum_ref < 0.0 ? -sum_ref : sum_ref;
    const crd::f64 maxabs  = sum_abs > 1e-6 ? sum_abs : 1e-6;

    // device soft-skip.
    crd::gpu::GpuContextConfig cfg;
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto devctx  = crd::gpu::create_vulkan_gpu_context(cfg);
    if (devctx == nullptr) { WARN("no Vulkan device -- skipping the CEIR-26c-2b duplicate-gemm differential"); return; }
    auto* const                    vk = static_cast<crd::gpu::VulkanGpuContext*>(devctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());

    // the runner: materialize the plan (ExternalIn seeded a/b, else 0; both reduce outputs FORCED GpuToCpu — the planner marks
    // only ONE terminal Output, the other is Intermediate/GpuOnly [the 26a-2 g0/g1 finding], so force both readable), execute as
    // ONE submit, read BOTH reduce scalars back BY OP IDENTITY (ra=max, rb=sum — robust to stage order).
    const auto run = [&](const ceg::TensorPipelinePlan& plan, float* out_max, float* out_sum) {
        const crd::usize                         nb = plan.buffers.size();
        std::unique_ptr<crd::gpu::ComputeBuffer> owned[32];
        crd::gpu::ComputeBuffer*                 bufs[32] = {};
        REQUIRE(nb <= 32U);
        // find the two reduce OUTPUT buffers by OP IDENTITY (ra=max, rb=sum — robust to stage order); FORCE both readable (the
        // planner marks only ONE terminal Output, the other Intermediate/GpuOnly — the 26a-2 g0/g1 finding).
        crd::i32 max_idx = -1;
        crd::i32 sum_idx = -1;
        for (crd::usize s = 0; s < plan.stages.size(); ++s)
        {
            if (plan.stages[s].kind != ceg::StageKind::Reduce) { continue; }
            const crd::i32 ob = plan.stages[s].bind[1]; // reduce: bind[0]=input, bind[1]=output
            if (plan.stages[s].op == ra) { max_idx = ob; }
            else if (plan.stages[s].op == rb) { sum_idx = ob; }
        }
        REQUIRE(max_idx >= 0);
        REQUIRE(sum_idx >= 0);
        REQUIRE(max_idx != sum_idx); // two distinct result buffers (the reduces were not merged)
        for (crd::usize i = 0; i < nb; ++i)
        {
            const ceg::PlanBuffer& pb = plan.buffers[i];
            if (pb.role == ceg::BufferRole::Alias) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
            crd::gpu::ComputeMemory mem = crd::gpu::ComputeMemory::GpuOnly;
            if (pb.role == ceg::BufferRole::ExternalIn) { mem = crd::gpu::ComputeMemory::CpuToGpu; }
            else if (pb.role == ceg::BufferRole::Output) { mem = crd::gpu::ComputeMemory::GpuToCpu; }
            if (static_cast<crd::i32>(i) == max_idx || static_cast<crd::i32>(i) == sum_idx) { mem = crd::gpu::ComputeMemory::GpuToCpu; }
            owned[i] = compute.create_buffer(pb.bytes, crd::gpu::compute_usage::storage, mem);
            REQUIRE(owned[i] != nullptr);
            bufs[i] = owned[i].get();
            if (pb.role == ceg::BufferRole::ExternalIn)
            {
                auto* dst = static_cast<float*>(owned[i]->map());
                REQUIRE(dst != nullptr);
                const crd::u64 cnt = pb.bytes / 4ULL;
                if (pb.value == a_in) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = a_data[e]; } }
                else if (pb.value == b_in) { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = b_data[e]; } }
                else { for (crd::u64 e = 0; e < cnt; ++e) { dst[e] = 0.0F; } }
                owned[i]->unmap();
            }
        }
        Resolver res;
        res.alloc_ctx = &ctx;
        res.alloc     = &root;
        res.compute   = &compute;
        auto&                   rec = compute.begin();
        const ceg::ExecuteError ee  = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res,
                                                                   crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(bufs, nb));
        REQUIRE(ee == ceg::ExecuteError::None);
        rec.barrier(*bufs[static_cast<crd::usize>(max_idx)], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
        rec.barrier(*bufs[static_cast<crd::usize>(sum_idx)], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
        compute.submit_and_wait();
        const auto* gm = static_cast<const float*>(owned[static_cast<crd::usize>(max_idx)]->map());
        REQUIRE(gm != nullptr);
        *out_max = gm[0]; // rank-0 scalar
        owned[static_cast<crd::usize>(max_idx)]->unmap();
        const auto* gs = static_cast<const float*>(owned[static_cast<crd::usize>(sum_idx)]->map());
        REQUIRE(gs != nullptr);
        *out_sum = gs[0];
        owned[static_cast<crd::usize>(sum_idx)]->unmap();
    };

    // RAW: 2 Gemm + 2 Reduce (the two gemms write DIFFERENT buffers — the planner did NOT dedupe; CSE has real work).
    const ceg::TensorPipelinePlan raw_plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
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
    // ⛔ alias-order gate (the 26b-2b rule): the runner's one-level bufs[i]=bufs[alias_of] resolves the reshape->gemm alias ONLY
    //    because plan buffers are walk-ordered producer-first (alias_of < i); a reorder would silently null-deref.
    for (crd::usize i = 0; i < raw_plan.buffers.size(); ++i)
    {
        if (raw_plan.buffers[i].role != ceg::BufferRole::Alias) { continue; }
        REQUIRE(raw_plan.buffers[i].alias_of >= 0);
        REQUIRE(static_cast<crd::usize>(raw_plan.buffers[i].alias_of) < i);
    }
    float raw_max = 0.0F;
    float raw_sum = 0.0F;
    run(raw_plan, &raw_max, &raw_sum);

    // cse_run: the two identical gemms collapse (d2 erased, rb rewired onto d1) — one DISPATCH gone.
    ce::DiagnosticEngine diag(ctx, &root);
    REQUIRE(ce::cse_run(ctx, *m, diag));
    REQUIRE_FALSE(diag.has_fatal());
    CHECK(g2->is_erased());
    CHECK_FALSE(g1->is_erased());
    CHECK(rs2->is_erased());                       // the two identical reshapes also merged (rs2 -> rs1)
    CHECK(rb->operand(0U) == rs1->result(0U));     // rb rewired onto the surviving reshape (which aliases the surviving gemm g1)
    CHECK(ra->operand(0U) == rb->operand(0U));     // ⛔ both reduces now read the SAME Value (the merged rs1) — IR fork is correct

    // OPT: 1 Gemm + 2 Reduce (the removed dispatch; both reduces read the ONE surviving gemm).
    const ceg::TensorPipelinePlan opt_plan = ceg::plan_tensor_pipeline(ctx, *m, &root);
    REQUIRE(opt_plan.reject == ceg::PlanReject::None);
    REQUIRE(opt_plan.stages.size() == 3U);
    int opt_gemm = 0;
    for (crd::usize s = 0; s < opt_plan.stages.size(); ++s) { opt_gemm += opt_plan.stages[s].kind == ceg::StageKind::Gemm ? 1 : 0; }
    REQUIRE(opt_gemm == 1);
    // ⛔ advisor 3-check (the fork-on-ALIAS defect the device-free plan gate cannot see — OPT is RE-planned after 2 erasures):
    // (1) OPT alias-order gate (RAW had it; OPT did not); (2) both reduces bind the SAME merged alias; (3) it aliases THIS plan's
    // gemm output.
    for (crd::usize i = 0; i < opt_plan.buffers.size(); ++i)
    {
        if (opt_plan.buffers[i].role != ceg::BufferRole::Alias) { continue; }
        REQUIRE(opt_plan.buffers[i].alias_of >= 0);
        REQUIRE(static_cast<crd::usize>(opt_plan.buffers[i].alias_of) < i); // producer-first
    }
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
    CHECK(o_maxin == o_sumin);                                        // both reduces read the ONE merged rs1 alias
    REQUIRE(o_gemm >= 0);
    CHECK(opt_plan.buffers[static_cast<crd::usize>(o_maxin)].alias_of == o_gemm); // the alias points at THIS plan's gemm output
    // ⛔ advisor: the gemm's INPUT binds must be a_in / b_in BY VALUE IDENTITY — a fork whose result has two consumers via an
    //    alias may shadow the planner's operand->buffer lookup and mis-bind A/B (the 37 fingerprint = a real-but-wrong matrix).
    REQUIRE(o_ga >= 0);
    REQUIRE(o_gb >= 0);
    CAPTURE(o_ga, o_gb, o_gemm, o_maxin);
    CHECK(opt_plan.buffers[static_cast<crd::usize>(o_ga)].value == a_in);
    CHECK(opt_plan.buffers[static_cast<crd::usize>(o_gb)].value == b_in);
    float opt_max = 0.0F;
    float opt_sum = 0.0F;
    run(opt_plan, &opt_max, &opt_sum);

    // BIT-EXACT: removing the duplicate gemm dispatch changed nothing observable (the 26a differential-test standard on a REMOVED
    // DISPATCH — the witness at full teeth, unlike 26b's alias-only fold). rb (the sum) is the load-bearing rewired consumer.
    CHECK(opt_max == raw_max);
    CHECK(opt_sum == raw_sum);
    // and both are CORRECT vs the independent f64 ref (not merely self-consistent) — a TOLERANCE, since the device gemm
    // accumulates in f32 while the ref is f64 (bit-exact belongs to opt-vs-raw above, same-precision device output).
    const float tol = static_cast<float>(static_cast<crd::f64>(kL) * 2e-3 * maxabs);
    const float fm  = raw_max - static_cast<float>(max_ref);
    CHECK((fm < 0.0F ? -fm : fm) <= tol);
    const float fs = raw_sum - static_cast<float>(sum_ref);
    CHECK((fs < 0.0F ? -fs : fs) <= tol);
}
