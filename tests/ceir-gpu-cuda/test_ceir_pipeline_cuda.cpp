// CEIR-29b-1 (CUDA) — the multi-stage TENSOR PIPELINE on a real CUDA device: the FOURTH backend for execute_tensor_pipeline
// (the shared, backend-free §158 run-half beside plan_tensor_pipeline). Pays the 24z-ledgered CUDA-multi-stage debt: an
// expanded ml.mlp (gemm -> relu -> gemm), planned fuse=false, executed as ONE device submit with the intermediate device-
// resident, BIT-EXACT vs a CPU float MLP oracle whose arithmetic MIRRORS emit_contract_cuda (per-op multiply-then-add,
// sequential k). The CUDA pipelines compile fmad=false, so the multiply-adds round per-op exactly like the oracle (the 13z
// fmad=false bit-exact discipline) -> the gate asserts ==, not a tolerance (the Vk 24b-4 leg tolerates GLSL fma; CUDA earns
// bit-exact for free). This N-dispatch pipeline is the FALLBACK the CEIR-29b-2 CUDA-Graphs provider will CAPTURE into ONE
// cudaGraph (bit-exact vs this by construction). ⛔ the per-backend halves (resolve_stage emitter + run_mlp_module buffer
// plumbing) are MIRROR-INLINE per the house pattern (test_ceir_pipeline_dx12.cpp:399-403), not shared; only
// execute_tensor_pipeline is backend-agnostic. ⛔ fuse=false: emit_contract_cuda does NOT unwrap the fused GemmRelu Max
// epilogue (a ledgered gap -> fused GemmRelu on CUDA is a 29b-2+ trigger). Attention (transpose) + Reduce/Broadcast stages are
// out (emit_permute_cuda / emit_broadcast_nd_cuda absent -> ledgered). NVIDIA-gated: WARN-skips without a CUDA device (13z mold).
// ⛔ TEST NAME uses "CUDA" (uppercase) — run -R 29b (the band substring), not -R cuda (misses the uppercase).

#include <crd/ceir/context.hpp>
#include <crd/ceir/dist.hpp>                // CEIR-30b-3b: register_dist_ops + build_mesh/shard/all_reduce (the §140 chain)
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gen/transform_ops.hpp>   // CEIR-30c-2b: register_transform_ops (parse the committed place_mesh.ceir asset)
#include <crd/ceir/gpu/ckir_synth.hpp>      // synth_gemm / synth_elementwise / GemmEpilogue / bind_authored_local_size
#include <crd/ceir/gpu/expand_ml.hpp>       // expand_ml_ops (ml.mlp -> the 22/23 tensor vocab)
#include <crd/ceir/gpu/partition_ml.hpp>    // CEIR-29c-2: MlProvider / partition_ml / cuda_graphs_can_claim (the two-class tag source)
#include <crd/ceir/gpu/sharding.hpp>        // CEIR-30b-3b: materialize_sharding + lower_sharded_reduction (the reduction lowering)
#include <crd/ceir/gpu/tensor_pipeline.hpp> // plan_tensor_pipeline / plan_tensor_pipeline_partitioned / PlanOptions / StageKind
#include <crd/ceir/gpu/tensor_pipeline_exec.hpp> // execute_tensor_pipeline / ResolvedStage (the backend-agnostic run-half)
#include <crd/ceir/ir.hpp>
#include <crd/ceir/linalg.hpp>
#include <crd/ceir/ml.hpp>
#include <crd/ceir/parse.hpp> // CEIR-30c-2b: parse + ParseResult (load the committed place_mesh.ceir placement asset)
#include <crd/ceir/semantics.hpp> // CEIR-29c-2: ProviderClass / DeterminismClass (the cuda_graphs provider descriptor)
#include <crd/ceir/tensor.hpp>
#include <crd/ceir/type.hpp>

#include <crd/containers/hash_map.hpp> // CEIR-29c-2: the expand->ml-op lineage passed to plan_tensor_pipeline_partitioned

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp>            // ckir_read (relu.ckir, the authored activation)
#include <crd/kir/ckir_cuda.hpp>             // emit_contract_cuda / emit_elementwise_cuda / emit_compute_kernel_cuda

#include <crd/gpu/cuda_compute_context.hpp>  // create_cuda_compute_context / CudaComputeContext / create_pipeline_from_cuda

#include <crd/math/cmath.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "../gpu-shared/ceir_asset_slurp.hpp"   // CEIR-30c-2b: slurp_asset — the shared committed-asset reader (the place_mesh.ceir loader)
#include "../gpu-shared/ckir_asset_resolve.hpp" // CEIR-30b-2b-2b: resolve_ckir_asset — the shared VizDispatch .ckir loader
#include "../gpu-shared/two_class_fixture.hpp"  // CEIR-30b-2b-2b: fill_two_class_sandwich — the ONE oracle body

#include <catch2/catch_test_macros.hpp>

#include <chrono>  // CEIR-29z: steady_clock — the CPU submit-path wall-clock (the CUDA-Graphs win the GPU column can't show)
#include <cstdlib> // CEIR-29z: std::getenv — the CRD_CEIR_BENCH gate (the bench WARN-skips in the default sweep)
#include <cstring>
#include <fstream>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

namespace ce  = crd::ceir;
namespace ceg = crd::ceir::gpu;
namespace kir = crd::kir;

namespace
{
crd::u32 dim_ext(ce::Context& c, ce::TypeId t, crd::u32 axis)
{
    const ce::Type sh = c.type_of(c.type_of(t).members[1]);
    return axis < sh.members.size() ? c.type_of(sh.members[static_cast<crd::usize>(axis)]).count : 0U;
}
ce::TypeId sh2(ce::Context& c, crd::u32 a, crd::u32 b)
{
    const ce::TypeId d[2] = {c.type_dim_static(a), c.type_dim_static(b)};
    return c.type_shape(crd::containers::ConstSpan<ce::TypeId>(d, 2U));
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

// The CUDA resolver: re-synthesize/emit each stage to CUDA C -> a ComputePipeline it OWNS. ⛔ CUDA needs local_size (blockDim.x)
// + fmad PER PIPELINE (Vk/DX12 bake local_size into the shader). fmad=false everywhere -> per-op rounding == the CPU oracle.
struct Resolver
{
    ce::Context*                               alloc_ctx = nullptr;
    crd::memory::IAllocator*                   alloc     = nullptr;
    crd::gpu::CudaComputeContext*              compute   = nullptr;
    std::unique_ptr<crd::gpu::ComputePipeline> pipes[16];
    int                                        n = 0;
};

ceg::ResolvedStage resolve_stage(const ceg::PlanStage& st, void* user)
{
    auto&              res = *static_cast<Resolver*>(user);
    ce::Context&       c   = *res.alloc_ctx;
    ceg::ResolvedStage rs;
    kir::KGraph        g(res.alloc);
    kir::GlslKernel    kern(res.alloc); // a source container — emit_*_cuda writes CUDA C into it
    int                nbind      = 0;
    crd::u32           pushsize   = 0;
    crd::u32           local_size = 256U; // gemm/elementwise are guarded (gid>=n return) -> any block; VizDispatch overrides below

    if (st.kind == ceg::StageKind::Gemm || st.kind == ceg::StageKind::GemmRelu)
    {
        // ⛔ fuse=false in the gate ⇒ only plain Gemm reaches here. A fused GemmRelu (Max(Contract,0) output) makes
        //    emit_contract_cuda return false (it does NOT unwrap the epilogue like emit_contract_hlsl) ⇒ UnresolvedKernel.
        const ceg::GemmEpilogue ep = st.kind == ceg::StageKind::GemmRelu ? ceg::GemmEpilogue::Relu : ceg::GemmEpilogue::None;
        const ceg::GraphSynth   s  = ceg::synth_gemm(c, *st.op, g, ep);
        if (s.reject != ceg::SynthReject::None || !kir::emit_contract_cuda(g, s.output, kern)) { return rs; }
        const crd::u32 m     = dim_ext(c, st.op->operand(0U)->type(), 0U);
        const crd::u32 k     = dim_ext(c, st.op->operand(0U)->type(), 1U);
        const crd::u32 nn    = dim_ext(c, st.op->operand(1U)->type(), 1U);
        const crd::u32 pc[4] = {m, k, nn, 1U}; // {M,K,N,nbatch} — emit_contract_cuda's 16B by-value push, IDENTICAL layout to DX12
        std::memcpy(rs.push, pc, sizeof(pc));
        pushsize = 16U;
        rs.gx    = (m * nn + 255U) / 256U;
        nbind    = 3;
    }
    else if (st.kind == ceg::StageKind::Elementwise)
    {
        const ceg::GraphSynth s = ceg::synth_elementwise(c, *st.op, g);
        if (s.reject != ceg::SynthReject::None || !kir::emit_elementwise_cuda(g, s.output, res.alloc, kern)) { return rs; }
        const crd::u32 out_n = static_cast<crd::u32>(tnumel(c, st.op->result(0U)->type()));
        std::memcpy(rs.push, &out_n, sizeof(out_n)); // emit_elementwise_cuda's push is `unsigned n` (4B), NOT DX12's 16B blob
        pushsize = 4U;
        rs.gx    = (out_n + 255U) / 256U;
        nbind    = kern.n_inputs + 1; // the emitter counted the reachable Inputs; +1 for outb
    }
    else // VizDispatch — an authored .ckir loaded by kernel symbol (relu.ckir = the MLP activation), emit CUDA C (the DX12 mold)
    {
        const ce::AttrValue kv = c.attr_value(st.op->attr(crd::containers::StringView("kernel")));
        kir::KEntry         ve;
        // CEIR-30b-2b-2b: the shared VizDispatch loader (relu/viz_* → assets/ckir/<sym>.ckir → ckir_read) — the same resolver the
        // Host executor's HostKernelResolveFn uses, so the CUDA relu and the Host relu load the IDENTICAL authored kernel.
        if (!crd::tests::resolve_ckir_asset(kv.s, g, ve, res.alloc)) { return rs; }
        // a SENTINEL local_size(0) (relu.ckir) binds to the trailing-write operand's numel (M·hidden) — the DX12 resolver's job.
        if (st.n_out >= 1U)
        {
            const crd::u32              wop = 3U + st.nbind - st.n_out;
            const ceg::KernelShapeError kse = ceg::bind_authored_local_size(
                ve.local_size[0], tnumel(c, st.op->operand(wop)->type()), ceg::kMaxAuthoredLocalSize);
            if (kse != ceg::KernelShapeError::None) { return rs; } // unbound / exceeds the single-workgroup cap ⇒ UnresolvedKernel
        }
        if (!kir::emit_compute_kernel_cuda(g, ve, res.alloc, kern)) { return rs; }
        local_size = ve.local_size[0]; // CUDA launches this block dim (Vk/DX12 baked it into the shader)
        rs.gx      = const_grid(c, st.op->operand(0U));
        rs.gy      = const_grid(c, st.op->operand(1U));
        rs.gz      = const_grid(c, st.op->operand(2U));
        pushsize   = 0U;
        nbind      = static_cast<int>(st.nbind);
    }

    if (res.n >= 16) { return rs; }
    res.pipes[res.n] = res.compute->create_pipeline_from_cuda(crd::containers::to_view(kern.source),
                                                              crd::containers::StringView("ckir"), nbind, local_size, pushsize,
                                                              /*fmad=*/false);
    if (res.pipes[res.n] == nullptr) { return rs; }
    rs.pipeline  = res.pipes[res.n].get();
    rs.push_size = pushsize;
    ++res.n;
    return rs;
}

// CEIR-29z: a CACHING resolver wrapper for the bench. resolve_stage creates a NEW pipeline every call and caps at 16 (the
// correctness runners resolve <= 16 stages total, once each — fine). A bench calls execute_tensor_pipeline HUNDREDS of times,
// so it must NOT re-resolve: cache the whole ResolvedStage by st.op (a stage maps to ONE op; slice_plan copies keep the op
// pointer, so the same logical stage hits the cache whether it arrives via the whole plan or a sub-plan slice). Every stage is
// resolved ONCE (the first fb_once/pre-resolve); every later call is a hit => base.n stays at the distinct-stage count (3 or 5),
// never the 16 cap, and no compile lands inside begin_capture. ⛔ does NOT touch the shared resolve_stage (29c-2's n_pipes==12
// resolve-count identity is that runner's; the bench uses its own Resolver instance).
struct BenchResolver
{
    Resolver             base;             // owns the real pipelines (create_pipeline_from_cuda) + does the real resolve on a miss
    const ce::Operation* keys[16] = {};    // st.op cache keys
    ceg::ResolvedStage   vals[16] = {};    // the cached ResolvedStage (pipeline* + push + gx/gy/gz + push_size)
    int                  n_cached = 0;
};
ceg::ResolvedStage resolve_stage_cached(const ceg::PlanStage& st, void* user)
{
    auto& br = *static_cast<BenchResolver*>(user);
    for (int i = 0; i < br.n_cached; ++i) { if (br.keys[i] == st.op) { return br.vals[i]; } }
    const ceg::ResolvedStage rs = resolve_stage(st, &br.base); // real resolve — creates + OWNS the pipeline in br.base.pipes
    if (rs.pipeline != nullptr && br.n_cached < 16)
    {
        br.keys[br.n_cached] = st.op;
        br.vals[br.n_cached] = rs;
        ++br.n_cached;
    }
    return rs;
}

struct MlpSeed
{
    const ce::Value* value  = nullptr;
    const float*     floats = nullptr;
    crd::u32         count  = 0;
};
struct MlpOut
{
    const ce::Value* value = nullptr;
    float*           dst   = nullptr;
    crd::usize       len   = 0;
};

// Materialize a planned module's buffers on CUDA (the PORTABLE dev/up/rb copy pattern — the same 13z dispatch_kernel_1wg uses;
// CUDA's barrier is a documented no-op, but the copies + the single submit fence order correctly), execute, read back the
// output by SSA Value. `n_allocated` = distinct physical create_buffer calls (aliased buffers skip it — the 26f delta).
bool run_mlp_module(crd::gpu::CudaComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* alloc,
                    const ceg::TensorPipelinePlan& plan, const MlpSeed* seeds, crd::usize n_seeds, const MlpOut& out,
                    crd::u32* n_allocated = nullptr)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;
    const crd::usize nb = plan.buffers.size();
    if (nb > 40U) { return false; }
    std::unique_ptr<g::ComputeBuffer> dev[40];
    std::unique_ptr<g::ComputeBuffer> up[40];
    g::ComputeBuffer*                 bufs[40] = {};
    crd::u32                          alloc_count = 0;
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.alias_of >= 0) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; } // 26f: share the realized buffer
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes;
        dev[i]            = compute.create_buffer(sz, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        if (dev[i] == nullptr) { return false; }
        bufs[i] = dev[i].get();
        ++alloc_count;
        const MlpSeed* seed = nullptr;
        if (pb.role == ceg::BufferRole::ExternalIn)
        {
            for (crd::usize s = 0; s < n_seeds; ++s) { if (seeds[s].value == pb.value) { seed = &seeds[s]; break; } }
        }
        if (seed == nullptr) { continue; } // an Intermediate/Output (no host seed)
        up[i] = compute.create_buffer(sz, transfer_src, g::ComputeMemory::CpuToGpu);
        if (up[i] == nullptr) { return false; }
        auto* raw = static_cast<float*>(up[i]->map());
        if (raw == nullptr) { return false; }
        for (crd::u32 e = 0; e < seed->count; ++e) { raw[e] = seed->floats[e]; }
        up[i]->unmap();
    }
    crd::i32 out_idx = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == out.value) { out_idx = static_cast<crd::i32>(i); } }
    if (out_idx < 0 || plan.buffers[static_cast<crd::usize>(out_idx)].alias_of >= 0) { return false; }
    std::unique_ptr<g::ComputeBuffer> rb =
        compute.create_buffer(plan.buffers[static_cast<crd::usize>(out_idx)].bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
    if (rb == nullptr) { return false; }

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
    rec.barrier(*dev[static_cast<crd::usize>(out_idx)], g::ComputeAccess::ShaderWrite, g::ComputeAccess::TransferSrc);
    rec.copy(*dev[static_cast<crd::usize>(out_idx)], *rb, 0U, 0U, plan.buffers[static_cast<crd::usize>(out_idx)].bytes);
    compute.submit_and_wait();
    const auto* got = static_cast<const float*>(rb->map());
    if (got == nullptr) { return false; }
    for (crd::usize e = 0; e < out.len; ++e) { out.dst[e] = got[e]; }
    rb->unmap();
    if (n_allocated != nullptr) { *n_allocated = alloc_count; }
    return true;
}

// CEIR-29b: build + expand + plan the canonical unfused 3-stage ml.mlp (gemm -> relu -> gemm) into `m` (owned by the caller's
// ctx; ctx/alloc must outlive the returned plan). fuse=false so every stage rides a CUDA emitter. Fills the 3 input SSA values
// + the output value. Shared by the 29b-1 (fallback) and 29b-2a (graph-capture) gates so both plan the IDENTICAL program.
ceg::TensorPipelinePlan build_mlp_plan(ce::Context& ctx, ce::Module& m, crd::memory::IAllocator* alloc, crd::u32 mrows,
                                       crd::u32 d0, crd::u32 d1, crd::u32 d2, ce::Value*& x_val, ce::Value*& w1_val,
                                       ce::Value*& w2_val, const ce::Value*& out_val)
{
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    ce::Block* top = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
    x_val  = mkd(tf(ctx, sh2(ctx, mrows, d0)));
    w1_val = mkd(tf(ctx, sh2(ctx, d0, d1)));
    w2_val = mkd(tf(ctx, sh2(ctx, d1, d2)));
    ce::Value*           mlpops[3] = {x_val, w1_val, w2_val}; // ml.mlp(input, W_1, W_2)
    ce::Operation* const mo        = ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mlpops, 3U),
                                                          1U, tf(ctx, sh2(ctx, mrows, d2)), 0U);
    ctx.set_attr(mo, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
    b->append(mo);
    const ceg::MlExpandResult er = ceg::expand_ml_ops(ctx, m);
    REQUIRE(er.error == ceg::MlExpandError::None);
    REQUIRE(er.expanded == 1U);
    ceg::PlanOptions opts;
    opts.fuse_gemm_relu = false; // emit_contract_cuda does not unwrap a fused GemmRelu (a ledgered 29b-2+ trigger)
    ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline(ctx, m, alloc, opts);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    out_val = nullptr;
    for (crd::usize i = 0; i < plan.buffers.size(); ++i)
    {
        if (plan.buffers[i].role == ceg::BufferRole::Output) { out_val = plan.buffers[i].value; }
    }
    return plan;
}

// Fill the 3 input arrays (x has negatives so relu matters) + the CPU MLP oracle whose arithmetic MIRRORS emit_contract_cuda
// (prod=a*b; acc=acc+prod; sequential-k; relu=max) so the fmad=false device output is BIT-EXACT vs it. Same inputs both gates.
void fill_mlp_data_and_oracle(crd::u32 mrows, crd::u32 d0, crd::u32 d1, crd::u32 d2, float* x_in, float* w1_in, float* w2_in,
                              float* oracle)
{
    for (crd::u32 i = 0; i < mrows * d0; ++i) { x_in[i] = 0.1F * static_cast<float>(static_cast<int>(i) - 12); }
    for (crd::u32 i = 0; i < d0 * d1; ++i) { w1_in[i] = 0.05F * static_cast<float>(static_cast<int>(i % 7) - 3); }
    for (crd::u32 i = 0; i < d1 * d2; ++i) { w2_in[i] = 0.1F * static_cast<float>(static_cast<int>(i % 5) - 2); }
    float h1[64]; // hidden row (relu output); d1 <= 64 in every gate here (no VLA)
    for (crd::u32 mm = 0; mm < mrows; ++mm)
    {
        for (crd::u32 nn = 0; nn < d1; ++nn)
        {
            float acc = 0.0F;
            for (crd::u32 kk = 0; kk < d0; ++kk) { const float prod = x_in[mm * d0 + kk] * w1_in[kk * d1 + nn]; acc = acc + prod; }
            h1[nn] = crd::math::max(acc, 0.0F); // relu
        }
        for (crd::u32 j = 0; j < d2; ++j)
        {
            float acc = 0.0F;
            for (crd::u32 nn = 0; nn < d1; ++nn) { const float prod = h1[nn] * w2_in[nn * d2 + j]; acc = acc + prod; }
            oracle[mm * d2 + j] = acc;
        }
    }
}

// CEIR-29b-2a: upload ONCE, then run the SAME plan THREE ways into three host buffers -- (1) the N-dispatch FALLBACK
// (execute straight onto the stream), (2) a CAPTURED cudaGraph launched once, (3) the SAME instantiated graph launched AGAIN
// (replay). ⛔ uploads run OUTSIDE the captured region (a captured cuMemcpyAsync would re-run on every launch); only the
// stage dispatches are captured. Fills *node_count (recorded graph nodes) + *graph_ok (graph.valid()). Returns false only on
// a hard buffer/exec failure (device OOM, resolve error) -- an invalid capture is surfaced via *graph_ok, not a false return.
bool run_mlp_captured(crd::gpu::CudaComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* alloc,
                      const ceg::TensorPipelinePlan& plan, const MlpSeed* seeds, crd::usize n_seeds, const ce::Value* out_val,
                      crd::usize len, float* fallback_dst, float* graph_dst, float* graph_replay_dst, crd::u32* node_count,
                      bool* graph_ok)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;
    const crd::usize nb = plan.buffers.size();
    if (nb > 40U) { return false; }
    std::unique_ptr<g::ComputeBuffer> dev[40];
    std::unique_ptr<g::ComputeBuffer> up[40];
    g::ComputeBuffer*                 bufs[40] = {};
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.alias_of >= 0) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes;
        dev[i]            = compute.create_buffer(sz, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        if (dev[i] == nullptr) { return false; }
        bufs[i] = dev[i].get();
        const MlpSeed* seed = nullptr;
        if (pb.role == ceg::BufferRole::ExternalIn)
        {
            for (crd::usize s = 0; s < n_seeds; ++s) { if (seeds[s].value == pb.value) { seed = &seeds[s]; break; } }
        }
        if (seed == nullptr) { continue; }
        up[i] = compute.create_buffer(sz, transfer_src, g::ComputeMemory::CpuToGpu);
        if (up[i] == nullptr) { return false; }
        auto* raw = static_cast<float*>(up[i]->map());
        if (raw == nullptr) { return false; }
        for (crd::u32 e = 0; e < seed->count; ++e) { raw[e] = seed->floats[e]; }
        up[i]->unmap();
    }
    crd::i32 out_idx = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == out_val) { out_idx = static_cast<crd::i32>(i); } }
    if (out_idx < 0 || plan.buffers[static_cast<crd::usize>(out_idx)].alias_of >= 0) { return false; }
    const crd::u64                    out_bytes = plan.buffers[static_cast<crd::usize>(out_idx)].bytes;
    std::unique_ptr<g::ComputeBuffer> rb = compute.create_buffer(out_bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
    if (rb == nullptr) { return false; }

    Resolver res;
    res.alloc_ctx = &ctx;
    res.alloc     = alloc;
    res.compute   = &compute;
    const auto span = crd::containers::ConstSpan<g::ComputeBuffer*>(bufs, nb);

    // 1. UPLOAD (once, outside any capture): H->D copies of the seeded inputs.
    {
        auto& rec = compute.begin();
        for (crd::usize i = 0; i < nb; ++i) { if (up[i] != nullptr) { rec.copy(*up[i], *dev[i], 0U, 0U, plan.buffers[i].bytes); } }
        compute.submit_and_wait();
    }
    // readback dev[out] -> host dst via a fresh submit (also outside any capture).
    const auto readback = [&](float* dst) -> bool
    {
        auto& rec = compute.begin();
        rec.copy(*dev[static_cast<crd::usize>(out_idx)], *rb, 0U, 0U, out_bytes);
        compute.submit_and_wait();
        const auto* got = static_cast<const float*>(rb->map());
        if (got == nullptr) { return false; }
        for (crd::usize e = 0; e < len; ++e) { dst[e] = got[e]; }
        rb->unmap();
        return true;
    };

    // 2. FALLBACK — the N-dispatch path.
    {
        auto&                   rec = compute.begin();
        const ceg::ExecuteError ee  = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res, span);
        if (ee != ceg::ExecuteError::None) { return false; }
        compute.submit_and_wait();
    }
    if (!readback(fallback_dst)) { return false; }

    // 3. CAPTURE — record the SAME dispatches into a cudaGraph, instantiate once, launch, read back.
    auto&                   crec = compute.begin_capture();
    const ceg::ExecuteError ce2  = ceg::execute_tensor_pipeline(plan, crec, &resolve_stage, &res, span);
    if (ce2 != ceg::ExecuteError::None) { return false; }
    std::unique_ptr<g::CudaGraph> graph = compute.end_capture();
    *node_count                         = graph->node_count();
    *graph_ok                           = graph->valid();
    if (!graph->valid()) { return true; } // surfaced via *graph_ok; the test REQUIREs it before comparing
    compute.launch(*graph);
    if (!readback(graph_dst)) { return false; }

    // 4. REPLAY — launch the SAME instantiated exec again (instantiate-once / launch-many).
    compute.launch(*graph);
    return readback(graph_replay_dst);
}

// CEIR-29c-2: the cuda_graphs launch-graph provider descriptor (the test_band24_gate.cpp mold) — BitExact (a captured run of
// fmad=false kernels is the correctly-rounded reference value), claims_subgraphs=true (it captures a maximal run into ONE graph),
// advertise = cuda_graphs_can_claim. Only `available`+`advertise` steer partition_ml's assignment; the class fields are honest.
ceg::MlProvider cuda_graphs_provider()
{
    ceg::MlProvider p;
    p.name             = crd::containers::StringView("cuda_graphs");
    p.available        = true;
    p.advertise        = &ceg::cuda_graphs_can_claim;
    p.provider_class   = ce::ProviderClass::Gpu;
    p.memory_domain    = crd::containers::StringView("device_local");
    p.determinism      = ce::DeterminismClass::BitExact;
    p.claims_subgraphs = true;
    return p;
}

// CEIR-29c-2: build the DEPENDENT SANDWICH gemm(x,W0) -> mlp_relu(.; W1,W2) -> gemm(.,W3), PARTITIONED so cuda_graphs claims the
// mlp while the two flanking linalg.gemms fall back (they are NOT ml ops -> partition_ml never sees them -> no lineage -> -1). The
// stages come out [gemm, gemm, relu, gemm, gemm] tagged [-1, 0, 0, 0, -1]: a captured mlp run flanked by fallback gemms that cross
// the capture edge with a REAL data dependency (x' feeds the mlp; y feeds z). fuse=false (the CUDA emitter path — no GemmRelu). Fills
// the 5 input SSA values (for seeding) + the output z value + *n_assign (partition ml-op count, must be 1). ctx/alloc outlive the plan.
ceg::TensorPipelinePlan build_two_class_plan(ce::Context& ctx, ce::Module& m, crd::memory::IAllocator* alloc, crd::u32 mrows,
                                             crd::u32 d0, crd::u32 d1, crd::u32 d2, crd::u32 d3, ce::Value*& x_val,
                                             ce::Value*& w0_val, ce::Value*& w1_val, ce::Value*& w2_val, ce::Value*& w3_val,
                                             const ce::Value*& out_val, crd::u32* n_assign)
{
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::arith::register_arith_ops(ctx);
    (void)ce::compute::register_compute_ops(ctx);
    (void)ce::linalg::register_dialect(ctx);
    (void)ce::ml::register_dialect(ctx);
    ce::Block* top = m.body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m.body()->append(top); }
    ce::Operation* const f = ce::func::create_func(ctx, m, "main", ce::Visibility::Public, 0U);
    top->append(f);
    ce::Block* const b   = ce::func::func_body_block(f);
    const ce::OpId   dcl = ctx.intern_op("resource", "declare");
    const auto       mkd = [&](ce::TypeId t) { ce::Operation* const d = ctx.create_operation(dcl, {}, 1U, t); b->append(d); return d->result(0U); };
    // a plain f32 gemm(a[mm,kk], bb[kk,nn]) -> [mm,nn], alpha=1/beta=0 (the C accumulator is structurally required but unread — the
    // 29b-1 mlp-internal gemms prove an unseeded beta=0 C is safe). Appends the op; returns its result value.
    const auto gemm = [&](ce::Value* a, ce::Value* bb, crd::u32 mm, crd::u32 kk, crd::u32 nn) -> ce::Value* {
        ce::Value* const     c = mkd(tf(ctx, sh2(ctx, mm, nn)));
        ce::Operation* const g = ce::linalg::build_gemm(ctx, a, bb, c, ctx.attr_float(1.0), ctx.attr_float(0.0),
                                                        ctx.attr_bool(false), ctx.attr_bool(false), tf(ctx, sh2(ctx, mm, nn)));
        (void)kk;
        b->append(g);
        return g->result(0U);
    };
    x_val               = mkd(tf(ctx, sh2(ctx, mrows, d0)));
    w0_val              = mkd(tf(ctx, sh2(ctx, d0, d0)));
    ce::Value* const xp = gemm(x_val, w0_val, mrows, d0, d0); // x' [M,d0]  (leading FALLBACK)
    w1_val              = mkd(tf(ctx, sh2(ctx, d0, d1)));
    w2_val              = mkd(tf(ctx, sh2(ctx, d1, d2)));
    ce::Value*           mlpops[3] = {xp, w1_val, w2_val}; // ml.mlp(x', W1, W2) -> y [M,d2]  (CLAIMED by cuda_graphs)
    ce::Operation* const mo = ctx.create_operation(ctx.intern_op("ml", "mlp"), crd::containers::ConstSpan<ce::Value*>(mlpops, 3U),
                                                   1U, tf(ctx, sh2(ctx, mrows, d2)), 0U);
    ctx.set_attr(mo, crd::containers::StringView("activation"), ctx.attr_string(crd::containers::StringView("relu")));
    b->append(mo);
    w3_val = mkd(tf(ctx, sh2(ctx, d2, d3)));
    (void)gemm(mo->result(0U), w3_val, mrows, d2, d3); // z [M,d3]  (trailing FALLBACK)

    // PARTITION (cuda_graphs claims the mlp; the flanking gemms are non-ml -> assignments.size()==1) BEFORE expansion.
    const ceg::MlProvider  provs[1] = {cuda_graphs_provider()};
    const ceg::MlPartition part     = ceg::partition_ml(ctx, m, crd::containers::ConstSpan<ceg::MlProvider>(provs, 1U), alloc);
    if (n_assign != nullptr) { *n_assign = static_cast<crd::u32>(part.assignments.size()); }

    // EXPAND with lineage, then plan PARTITIONED: the mlp's stages tag provider 0, the flanking gemms (no lineage) tag -1.
    crd::containers::HashMap<const ce::Operation*, crd::i32> lineage(alloc);
    const ceg::MlExpandResult                                er = ceg::expand_ml_ops(ctx, m, lineage);
    REQUIRE(er.error == ceg::MlExpandError::None);
    REQUIRE(er.expanded == 1U);
    ceg::PlanOptions opts;
    opts.fuse_gemm_relu          = false; // emit_contract_cuda does not unwrap a fused GemmRelu (the ledgered 29b gap)
    ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline_partitioned(ctx, m, alloc, part, lineage, opts);
    out_val                      = nullptr;
    for (crd::usize i = 0; i < plan.buffers.size(); ++i)
    {
        if (plan.buffers[i].role == ceg::BufferRole::Output) { out_val = plan.buffers[i].value; } // z, the terminal value
    }
    return plan;
}

// CEIR-30b-2b-2b: the two-class fill+oracle body was HOISTED to `tests/gpu-shared/two_class_fixture.hpp`
// (`crd::tests::fill_two_class_sandwich`) — the ONE oracle body shared with the device-free runner + Host executor gates. Call
// sites below use it directly.

// CEIR-30b-2b-2: `slice_plan` (a SUB-PLAN over stages [lo,hi) with ALL buffers copied verbatim so every bind index +
// validate_tensor_pipeline's n_buffers stay valid) was HOISTED to the engine (tensor_pipeline_exec.hpp) for execute_two_class. The
// unqualified `slice_plan(plan, …)` calls below resolve to `crd::ceir::gpu::slice_plan` by ADL (the first arg is a ceg:: plan) — a
// local of the same signature here would make every such call ambiguous, so it is intentionally gone.

// CEIR-29c-2: run the two-class plan with the PER-STAGE CAPTURE predicate. Upload once, then: (2) an ALL-FALLBACK N-dispatch run of
// the WHOLE plan (the bit-exact reference) -> fallback_dst; (3) the TWO-CLASS run -- the fallback PREFIX eager on the stream, then
// ONLY the contiguous provider==cuda_graphs run captured into ONE cudaGraph (node_count = its stages, NOT the total), launched, then
// the fallback SUFFIX eager -> graph_dst; (4) REPLAY (re-run prefix + launch + re-run suffix -- live aliasing makes the captured
// run's inputs non-persistent, so a faithful replay re-establishes them) -> graph_replay_dst. The claimed run [lo,hi)
// is DERIVED by scanning provider==0 (surfaced via *claim_lo/*claim_hi). *n_aliases = 26f alias materializations honored. ⛔ the
// inter-stage barrier execute_tensor_pipeline would place at the 3->4 split is dropped by the slice, and that is CORRECT: CUDA's
// rec.barrier is a documented no-op and same-stream order (eager prefix -> cuGraphLaunch -> eager suffix) carries the x'->mlp->z
// dependency. Returns false only on a hard buffer/exec failure; an invalid capture is surfaced via *graph_ok.
bool run_two_class_captured(crd::gpu::CudaComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* alloc,
                            const ceg::TensorPipelinePlan& plan, const MlpSeed* seeds, crd::usize n_seeds,
                            const ce::Value* out_val, crd::usize len, float* fallback_dst, float* graph_dst,
                            float* graph_replay_dst, crd::u32* node_count, bool* graph_ok, crd::usize* claim_lo,
                            crd::usize* claim_hi, crd::u32* n_aliases, crd::u32* n_pipes, float* enqueue_dst)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;
    const crd::usize nb = plan.buffers.size();
    if (nb > 40U) { return false; }
    std::unique_ptr<g::ComputeBuffer> dev[40];
    std::unique_ptr<g::ComputeBuffer> up[40];
    g::ComputeBuffer*                 bufs[40]  = {};
    crd::u32                          aliases   = 0;
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.alias_of >= 0) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; ++aliases; continue; }
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes;
        dev[i]            = compute.create_buffer(sz, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        if (dev[i] == nullptr) { return false; }
        bufs[i] = dev[i].get();
        const MlpSeed* seed = nullptr;
        if (pb.role == ceg::BufferRole::ExternalIn)
        {
            for (crd::usize s = 0; s < n_seeds; ++s) { if (seeds[s].value == pb.value) { seed = &seeds[s]; break; } }
        }
        if (seed == nullptr) { continue; }
        up[i] = compute.create_buffer(sz, transfer_src, g::ComputeMemory::CpuToGpu);
        if (up[i] == nullptr) { return false; }
        auto* raw = static_cast<float*>(up[i]->map());
        if (raw == nullptr) { return false; }
        for (crd::u32 e = 0; e < seed->count; ++e) { raw[e] = seed->floats[e]; }
        up[i]->unmap();
    }
    if (n_aliases != nullptr) { *n_aliases = aliases; }
    crd::i32 out_idx = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == out_val) { out_idx = static_cast<crd::i32>(i); } }
    if (out_idx < 0 || plan.buffers[static_cast<crd::usize>(out_idx)].alias_of >= 0) { return false; }
    const crd::u64                    out_bytes = plan.buffers[static_cast<crd::usize>(out_idx)].bytes;
    std::unique_ptr<g::ComputeBuffer> rb = compute.create_buffer(out_bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
    if (rb == nullptr) { return false; }

    Resolver res;
    res.alloc_ctx = &ctx;
    res.alloc     = alloc;
    res.compute   = &compute;
    const auto span = crd::containers::ConstSpan<g::ComputeBuffer*>(bufs, nb);

    // DERIVE the claimed run [lo,hi) — the maximal contiguous provider==0 range (29c-1 proved it is a single contiguous run).
    crd::usize lo = 0;
    while (lo < plan.stages.size() && plan.stages[lo].provider != 0) { ++lo; }
    crd::usize hi = lo;
    while (hi < plan.stages.size() && plan.stages[hi].provider == 0) { ++hi; }
    if (claim_lo != nullptr) { *claim_lo = lo; }
    if (claim_hi != nullptr) { *claim_hi = hi; }

    // 1. UPLOAD (once, outside any capture).
    {
        auto& rec = compute.begin();
        for (crd::usize i = 0; i < nb; ++i) { if (up[i] != nullptr) { rec.copy(*up[i], *dev[i], 0U, 0U, plan.buffers[i].bytes); } }
        compute.submit_and_wait();
    }
    const auto readback = [&](float* dst) -> bool
    {
        auto& rec = compute.begin();
        rec.copy(*dev[static_cast<crd::usize>(out_idx)], *rb, 0U, 0U, out_bytes);
        compute.submit_and_wait();
        const auto* got = static_cast<const float*>(rb->map());
        if (got == nullptr) { return false; }
        for (crd::usize e = 0; e < len; ++e) { dst[e] = got[e]; }
        rb->unmap();
        return true;
    };
    const auto run_range = [&](crd::usize rlo, crd::usize rhi) -> bool
    {
        if (rlo >= rhi) { return true; } // an empty class (no prefix / no suffix) is a no-op
        const ceg::TensorPipelinePlan sub = slice_plan(plan, rlo, rhi, alloc);
        auto&                         rec = compute.begin();
        const ceg::ExecuteError       ee  = ceg::execute_tensor_pipeline(sub, rec, &resolve_stage, &res, span);
        if (ee != ceg::ExecuteError::None) { return false; }
        compute.submit_and_wait();
        return true;
    };

    // 2. ALL-FALLBACK — the WHOLE plan as N dispatches (the bit-exact reference).
    {
        auto&                   rec = compute.begin();
        const ceg::ExecuteError ee  = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res, span);
        if (ee != ceg::ExecuteError::None) { return false; }
        compute.submit_and_wait();
    }
    if (!readback(fallback_dst)) { return false; }

    // 3. TWO-CLASS — fallback prefix eager, then CAPTURE only the claimed run, launch, then fallback suffix eager.
    if (!run_range(0U, lo)) { return false; }
    auto&                   crec = compute.begin_capture();
    const ceg::ExecuteError ce2  = ceg::execute_tensor_pipeline(slice_plan(plan, lo, hi, alloc), crec, &resolve_stage, &res, span);
    if (ce2 != ceg::ExecuteError::None) { return false; }
    std::unique_ptr<g::CudaGraph> graph = compute.end_capture();
    *node_count                         = graph->node_count();
    *graph_ok                           = graph->valid();
    if (!graph->valid()) { return true; } // surfaced via *graph_ok; the test REQUIREs it before comparing
    compute.launch(*graph);
    if (!run_range(hi, plan.stages.size())) { return false; }
    if (!readback(graph_dst)) { return false; }

    // 4. REPLAY — re-run the PREFIX first: with 26f storage-sharing LIVE the captured run destroys its own input's slot (a1
    //    aliases x''s storage), so x' is NOT stable across launches — a faithful instantiate-once/launch-many must re-establish
    //    the graph's inputs before each launch. Recompute x' -> launch the SAME exec -> re-run the suffix -> read z.
    if (!run_range(0U, lo)) { return false; }
    compute.launch(*graph);
    if (!run_range(hi, plan.stages.size())) { return false; }
    // the total distinct pipeline resolves: ALL-FALLBACK 5 + TWO-CLASS (prefix 1 + captured 3 + suffix 1 = 5) + REPLAY (prefix
    // 1 + suffix 1 = 2) = 12 — every fallback stage resolved once per dispatch pass, the captured run ONCE (the launches re-run
    // the SAME exec, resolving NOTHING). The identity that proves the flanking gemms actually DISPATCHED and the graph launched.
    if (n_pipes != nullptr) { *n_pipes = static_cast<crd::u32>(res.n); }
    if (!readback(graph_replay_dst)) { return false; }

    // CEIR-29z: PROVE the SINGLE-SUBMIT enqueue form here (CI-gated), not only in the env-gated bench — the two-class boundary
    // now has TWO execution paths (waiting launch() above + this one) and both must be bit-exact. begin -> eager prefix ->
    // enqueue(graph) [no wait] -> eager suffix -> submit_and_wait (ONE wait; same-stream order carries x'->graph->z). Runs
    // AFTER *n_pipes is captured so it does not perturb the 12-resolve identity.
    if (enqueue_dst != nullptr)
    {
        auto& erec = compute.begin();
        if (lo > 0U && ceg::execute_tensor_pipeline(slice_plan(plan, 0U, lo, alloc), erec, &resolve_stage, &res, span) !=
                           ceg::ExecuteError::None)
        {
            return false;
        }
        compute.enqueue(*graph);
        if (hi < plan.stages.size() &&
            ceg::execute_tensor_pipeline(slice_plan(plan, hi, plan.stages.size(), alloc), erec, &resolve_stage, &res, span) !=
                ceg::ExecuteError::None)
        {
            return false;
        }
        compute.submit_and_wait();
        if (!readback(enqueue_dst)) { return false; }
    }
    return true;
}

// CEIR-29z: median of k tiny samples (insertion sort in place; no std container / algorithm header — the 28b measurer mold).
double median_of(double* v, crd::u32 k)
{
    for (crd::u32 i = 1; i < k; ++i)
    {
        const double t = v[i];
        crd::u32     j = i;
        for (; j > 0 && v[j - 1] > t; --j) { v[j] = v[j - 1]; }
        v[j] = t;
    }
    return v[k / 2U];
}

// CEIR-29z BENCH runner — the N-dispatch fallback vs the 1-graph launch, measured BOTH ways (the REN-1 frame-graph board mold,
// 2026-07-24-ren1-frame-graph-batching.md): `cpu_us` = the CPU submit-path wall-clock per pipeline iteration (where the CUDA-
// Graphs win lives — N cuLaunchKernel + the recorder walk vs one cuGraphLaunch), `gpu_ms` = compute.last_gpu_ms() (a sanity
// column: identical kernels in identical stream order => GPU parity is EXPECTED, and its ~1.0x is the sentence that proves the
// win is submit-path, not a GPU artifact). Upload ONCE; capture the contiguous provider==0 run ONCE (the whole plan when the
// plan is unpartitioned — the mlp fixture); then two arms sharing the SAME device buffers:
//   FALLBACK  = execute the WHOLE plan as N dispatches (one submit) -> the bit-exact reference.
//   GRAPH     = the fallback PREFIX eager + launch(graph) + the fallback SUFFIX eager (prefix/suffix EMPTY for the whole-plan
//               mlp fixture; for the sandwich the prefix MUST re-run each iteration -- the aliased-storage finding: the captured
//               run destroys x''s slot, so a faithful launch-many re-establishes its inputs. NEVER dropped to flatter the graph).
// K in {1,10,100} launches per timed sample amortizes nothing one-time (capture/instantiate are outside the loop) -- it quantifies
// the per-launch CPU cost and shrinks timer noise (median of 9). ⛔ FALSIFIABILITY: *bitexact compares BOTH arms' output to the
// oracle (a timing loop that skips the check is the --gpu-cull empty-canvas scar) and the caller REQUIREs *fb_gpu_ms/*gr_gpu_ms > 0
// (the 28b "median==0 => measuring nothing" hard-fail) + node_count == the claimed run. The sub-plans are pre-sliced ONCE so
// slice_plan allocation never lands inside the timed region (it would unfairly tax the graph arm's prefix/suffix).
bool bench_captured(crd::gpu::CudaComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* alloc,
                    const ceg::TensorPipelinePlan& plan, const MlpSeed* seeds, crd::usize n_seeds, const ce::Value* out_val,
                    const float* oracle, crd::usize len, double fb_cpu_us[3], double gr_cpu_us[3], double* fb_gpu_ms,
                    double* gr_gpu_ms, crd::u32* node_count, bool* graph_ok, bool* bitexact, crd::usize* cap_lo_out,
                    crd::usize* cap_hi_out)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;
    const crd::usize nb = plan.buffers.size();
    if (nb > 40U || len > 64U) { return false; }
    std::unique_ptr<g::ComputeBuffer> dev[40];
    std::unique_ptr<g::ComputeBuffer> up[40];
    g::ComputeBuffer*                 bufs[40] = {};
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.alias_of >= 0) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes;
        dev[i]            = compute.create_buffer(sz, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        if (dev[i] == nullptr) { return false; }
        bufs[i]             = dev[i].get();
        const MlpSeed* seed = nullptr;
        if (pb.role == ceg::BufferRole::ExternalIn)
        {
            for (crd::usize s = 0; s < n_seeds; ++s) { if (seeds[s].value == pb.value) { seed = &seeds[s]; break; } }
        }
        if (seed == nullptr) { continue; }
        up[i] = compute.create_buffer(sz, transfer_src, g::ComputeMemory::CpuToGpu);
        if (up[i] == nullptr) { return false; }
        auto* raw = static_cast<float*>(up[i]->map());
        if (raw == nullptr) { return false; }
        for (crd::u32 e = 0; e < seed->count; ++e) { raw[e] = seed->floats[e]; }
        up[i]->unmap();
    }
    crd::i32 out_idx = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == out_val) { out_idx = static_cast<crd::i32>(i); } }
    if (out_idx < 0 || plan.buffers[static_cast<crd::usize>(out_idx)].alias_of >= 0) { return false; }
    const crd::u64                    out_bytes = plan.buffers[static_cast<crd::usize>(out_idx)].bytes;
    std::unique_ptr<g::ComputeBuffer> rb = compute.create_buffer(out_bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
    if (rb == nullptr) { return false; }

    BenchResolver bres;
    bres.base.alloc_ctx = &ctx;
    bres.base.alloc     = alloc;
    bres.base.compute   = &compute;
    const auto span = crd::containers::ConstSpan<g::ComputeBuffer*>(bufs, nb);

    // the claimed run [lo,hi) — the contiguous provider==0 range; NONE (an unpartitioned mlp plan, all -1) => the WHOLE plan.
    crd::usize lo = 0;
    while (lo < plan.stages.size() && plan.stages[lo].provider != 0) { ++lo; }
    crd::usize hi = lo;
    while (hi < plan.stages.size() && plan.stages[hi].provider == 0) { ++hi; }
    if (lo >= plan.stages.size()) { lo = 0; hi = plan.stages.size(); }
    if (cap_lo_out != nullptr) { *cap_lo_out = lo; }
    if (cap_hi_out != nullptr) { *cap_hi_out = hi; }

    // 1. UPLOAD (once, outside any capture).
    {
        auto& rec = compute.begin();
        for (crd::usize i = 0; i < nb; ++i) { if (up[i] != nullptr) { rec.copy(*up[i], *dev[i], 0U, 0U, plan.buffers[i].bytes); } }
        compute.submit_and_wait();
    }
    const auto readback = [&](float* dst) -> bool
    {
        auto& rec = compute.begin();
        rec.copy(*dev[static_cast<crd::usize>(out_idx)], *rb, 0U, 0U, out_bytes);
        compute.submit_and_wait();
        const auto* got = static_cast<const float*>(rb->map());
        if (got == nullptr) { return false; }
        for (crd::usize e = 0; e < len; ++e) { dst[e] = got[e]; }
        rb->unmap();
        return true;
    };

    // pre-slice the three sub-plans ONCE (so slice_plan allocation never lands in the timed region).
    const ceg::TensorPipelinePlan sub_pre = slice_plan(plan, 0U, lo, alloc);
    const ceg::TensorPipelinePlan sub_cap = slice_plan(plan, lo, hi, alloc);
    const ceg::TensorPipelinePlan sub_suf = slice_plan(plan, hi, plan.stages.size(), alloc);
    bool       ok      = true;
    float      tmp[64] = {};
    const auto fb_once = [&]() -> bool
    {
        auto&                   rec = compute.begin();
        const ceg::ExecuteError ee  = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage_cached, &bres, span);
        if (ee != ceg::ExecuteError::None) { return false; }
        compute.submit_and_wait();
        return true;
    };
    // ⛔ PRE-RESOLVE before capturing: a NORMAL execute of the whole plan compiles + caches every kernel (NVRTC) NOW — a
    //    resolve/compile inside begin_capture is not stream-ordered and invalidates the capture (the run_mlp_captured
    //    "fallback before capture" order). This resolves every stage the capture + prefix/suffix later replay from cache.
    if (!fb_once()) { return false; }

    // CAPTURE the claimed run ONCE (kernels now cached => the capture only RECORDS dispatches); the launches replay the SAME exec.
    auto&                   crec = compute.begin_capture();
    const ceg::ExecuteError cee  = ceg::execute_tensor_pipeline(sub_cap, crec, &resolve_stage_cached, &bres, span);
    if (cee != ceg::ExecuteError::None) { return false; }
    std::unique_ptr<g::CudaGraph> graph = compute.end_capture();
    *node_count                         = graph->node_count();
    *graph_ok                           = graph->valid();
    if (!graph->valid()) { return true; } // surfaced via *graph_ok; the caller REQUIREs it
    // ONE begin()/submit_and_wait() bracket: eager prefix + the ENQUEUED graph + eager suffix (ONE wait; same-stream order
    // carries x'->graph->z). For the whole-plan mlp fixture pre/suf are empty => begin/enqueue/submit_and_wait (the graph
    // alone in one bracket). The prefix re-runs each iteration — the 29c-2 aliased-storage finding: it re-establishes x'.
    const auto gr_once = [&]() -> bool
    {
        auto& rec = compute.begin();
        if (sub_pre.stages.size() != 0U &&
            ceg::execute_tensor_pipeline(sub_pre, rec, &resolve_stage_cached, &bres, span) != ceg::ExecuteError::None)
        {
            return false;
        }
        compute.enqueue(*graph);
        if (sub_suf.stages.size() != 0U &&
            ceg::execute_tensor_pipeline(sub_suf, rec, &resolve_stage_cached, &bres, span) != ceg::ExecuteError::None)
        {
            return false;
        }
        compute.submit_and_wait();
        return true;
    };

    // WARMUP (device clocks + first-launch driver work — the 28b n_warmup=5 mold).
    for (crd::u32 w = 0; w < 5U; ++w) { if (!fb_once() || !gr_once()) { return false; } }

    // SINGLE-SHOT gpu_ms + bit-exact (both arms == the CPU oracle). fb: one submit => last_gpu_ms is the whole N-dispatch run.
    if (!fb_once()) { return false; }
    *fb_gpu_ms = compute.last_gpu_ms();
    if (!readback(tmp)) { return false; }
    for (crd::usize e = 0; e < len; ++e) { if (tmp[e] != oracle[e]) { ok = false; } }
    // gr: ONE bracket around prefix + enqueued graph + suffix => last_gpu_ms is the whole two-class pipeline's GPU time,
    // DIRECTLY comparable to fb's one-bracket whole-plan GPU time (both arms now: ONE event bracket, ONE wait — the honest
    // measurement; a summed-three-brackets form would inflate gr by ~2 event-record+sync fixed costs — the artifact the
    // enqueue split removes).
    if (!gr_once()) { return false; }
    *gr_gpu_ms = compute.last_gpu_ms();
    if (!readback(tmp)) { return false; }
    for (crd::usize e = 0; e < len; ++e) { if (tmp[e] != oracle[e]) { ok = false; } }
    *bitexact = ok;

    // CPU SUBMIT-PATH wall-clock: median of 9, per-iteration (wall / K), for K in {1,10,100}.
    const crd::u32 kvals[3] = {1U, 10U, 100U};
    for (crd::u32 ki = 0; ki < 3U; ++ki)
    {
        constexpr crd::u32 nt = 9U;
        double             s_fb[nt] = {};
        double             s_gr[nt] = {};
        for (crd::u32 si = 0; si < nt; ++si)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (crd::u32 k = 0; k < kvals[ki]; ++k) { if (!fb_once()) { return false; } }
            const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            s_fb[si]        = ns / 1000.0 / static_cast<double>(kvals[ki]);
        }
        fb_cpu_us[ki] = median_of(s_fb, nt);
        for (crd::u32 si = 0; si < nt; ++si)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (crd::u32 k = 0; k < kvals[ki]; ++k) { if (!gr_once()) { return false; } }
            const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            s_gr[si]        = ns / 1000.0 / static_cast<double>(kvals[ki]);
        }
        gr_cpu_us[ki] = median_of(s_gr, nt);
    }
    return true;
}

// ── CEIR-30b-2b-2b: the §140 Host+CUDA two-class runner gate ─────────────────────────────────────────────────────────────────
// The device-class caller for `execute_two_class`: owns the CUDA device buffer table (alias→landlord; NO seed — the ExternalIn
// uploads are PLANNED transfers), the Resolver (its pipelines), the context, and identity counters.
struct TwoClassGpu
{
    crd::gpu::CudaComputeContext*            compute = nullptr;
    ce::Context*                             ctx     = nullptr;
    crd::memory::IAllocator*                 alloc   = nullptr;
    std::unique_ptr<crd::gpu::ComputeBuffer> dev[40];          // owns the non-alias (landlord) buffers
    crd::gpu::ComputeBuffer*                 bufs[40]  = {};    // the raw span for execute_tensor_pipeline (aliases → landlord ptr)
    crd::usize                               nb        = 0;
    Resolver                                 res;               // resolved pipelines accumulate here (the n_pipes identity)
    crd::u32                                 transfers = 0;     // the applied transfers (== plan_transfers size, the identity)
};

// the device-class per-stage runner: record the 1-stage slice over the device table into a fresh submission, submit, wait.
ceg::ExecuteError cuda_gpu_stage(const ceg::TensorPipelinePlan& slice, void* user)
{
    auto&                   tg   = *static_cast<TwoClassGpu*>(user);
    auto&                   rec  = tg.compute->begin();
    const auto              span = crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(tg.bufs, tg.nb);
    const ceg::ExecuteError ee   = ceg::execute_tensor_pipeline(slice, rec, &resolve_stage, &tg.res, span);
    if (ee != ceg::ExecuteError::None) { return ee; }
    tg.compute->submit_and_wait();
    return ceg::ExecuteError::None;
}

// the cross-domain copy: a per-transfer staging buffer (CpuToGpu upload / GpuToCpu readback) + rec.copy + submit_and_wait. `bytes`
// is the plan's (the runner owns it); the staging buffer is min-16 (a d2·d3=16B transfer is right at the boundary) but copies `bytes`.
ceg::ExecuteError cuda_transfer(const ceg::Transfer& t, float* host_ptr, crd::u64 bytes, void* user)
{
    namespace g          = crd::gpu;
    auto&           tg   = *static_cast<TwoClassGpu*>(user);
    const crd::u64  sz   = bytes < 16ULL ? 16ULL : bytes;
    const crd::usize n   = static_cast<crd::usize>(bytes / sizeof(float));
    g::ComputeBuffer* const dst = tg.bufs[static_cast<crd::usize>(t.buffer)];
    ++tg.transfers;
    if (t.direction == ceg::TransferDir::HostToDevice)
    {
        std::unique_ptr<g::ComputeBuffer> up = tg.compute->create_buffer(sz, g::compute_usage::transfer_src, g::ComputeMemory::CpuToGpu);
        if (up == nullptr) { return ceg::ExecuteError::UnmappedBinding; }
        auto* raw = static_cast<float*>(up->map());
        if (raw == nullptr) { return ceg::ExecuteError::UnmappedBinding; }
        for (crd::usize e = 0; e < n; ++e) { raw[e] = host_ptr[e]; }
        up->unmap();
        auto& rec = tg.compute->begin();
        rec.copy(*up, *dst, 0U, 0U, bytes);
        tg.compute->submit_and_wait();
    }
    else
    {
        std::unique_ptr<g::ComputeBuffer> rb = tg.compute->create_buffer(sz, g::compute_usage::transfer_dst, g::ComputeMemory::GpuToCpu);
        if (rb == nullptr) { return ceg::ExecuteError::UnmappedBinding; }
        auto& rec = tg.compute->begin();
        rec.copy(*dst, *rb, 0U, 0U, bytes);
        tg.compute->submit_and_wait();
        const auto* got = static_cast<const float*>(rb->map());
        if (got == nullptr) { return ceg::ExecuteError::UnmappedBinding; }
        for (crd::usize e = 0; e < n; ++e) { host_ptr[e] = got[e]; }
        rb->unmap();
    }
    return ceg::ExecuteError::None;
}

// CEIR-30z — the sharded-reduction bench wrappers. bench_gpu_stage CACHES the resolved combine (a bench calls execute_two_class
// K*medians times; the uncached tg.res would create a pipeline every call — the 29z resolve_stage_cached rationale). The captured
// combine_gpu_ms is the Gpu combine's OWN bracket (last_gpu_ms right after ITS submit, before the trailing readback). user for BOTH
// execute_two_class callbacks is a BenchTwoClass* (tg is its first member); bench_transfer forwards to cuda_transfer over the inner tg.
struct BenchTwoClass
{
    TwoClassGpu   tg;
    BenchResolver bres;
    double        combine_gpu_ms = 0.0;
};
ceg::ExecuteError bench_gpu_stage(const ceg::TensorPipelinePlan& slice, void* user)
{
    auto&                   btc  = *static_cast<BenchTwoClass*>(user);
    auto&                   rec  = btc.tg.compute->begin();
    const auto              span = crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(btc.tg.bufs, btc.tg.nb);
    const ceg::ExecuteError ee   = ceg::execute_tensor_pipeline(slice, rec, &resolve_stage_cached, &btc.bres, span);
    if (ee != ceg::ExecuteError::None) { return ee; }
    btc.tg.compute->submit_and_wait();
    btc.combine_gpu_ms = btc.tg.compute->last_gpu_ms(); // the combine kernel's OWN GPU bracket (not the trailing readback)
    return ceg::ExecuteError::None;
}
ceg::ExecuteError bench_transfer(const ceg::Transfer& t, float* host_ptr, crd::u64 bytes, void* user)
{
    return cuda_transfer(t, host_ptr, bytes, &static_cast<BenchTwoClass*>(user)->tg);
}

// ARM 1 — run the WHOLE plan all on the HOST (execute_tensor_pipeline_host over one f32 region per landlord buffer, aliases share,
// externals seeded, the relu VizDispatch resolved via the shared loader). Device-FREE, so this arm runs in CI without a GPU.
bool run_all_host(ce::Context& ctx, crd::memory::IAllocator* alloc, const ceg::TensorPipelinePlan& plan, const MlpSeed* seeds,
                  crd::usize n_seeds, const ce::Value* out_val, crd::usize len, float* out)
{
    const crd::usize nb = plan.buffers.size();
    if (nb > 40U) { return false; }
    crd::u64 offs[40] = {};
    crd::u64 total    = 0;
    for (crd::usize i = 0; i < nb; ++i) { offs[i] = total; if (plan.buffers[i].alias_of < 0) { total += plan.buffers[i].bytes / sizeof(float); } }
    crd::containers::Array<float> store(alloc);
    store.resize(static_cast<crd::usize>(total), -777.0F); // sentinel: a skipped stage / missed write leaves a loud marker, not 0
    float* ptr[40] = {};
    for (crd::usize i = 0; i < nb; ++i) { ptr[i] = plan.buffers[i].alias_of >= 0 ? ptr[static_cast<crd::usize>(plan.buffers[i].alias_of)] : store.data() + offs[i]; }
    for (crd::usize i = 0; i < nb; ++i)
    {
        if (plan.buffers[i].role != ceg::BufferRole::ExternalIn) { continue; }
        for (crd::usize s = 0; s < n_seeds; ++s)
        {
            if (seeds[s].value == plan.buffers[i].value) { for (crd::u32 e = 0; e < seeds[s].count; ++e) { ptr[i][e] = seeds[s].floats[e]; } }
        }
    }
    ceg::HostRunOptions host_opts;
    host_opts.kernel = &crd::tests::resolve_ckir_asset;
    host_opts.user   = alloc;
    if (ceg::execute_tensor_pipeline_host(ctx, plan, crd::containers::ConstSpan<float*>(ptr, nb), alloc, nullptr, host_opts) != ceg::ExecuteError::None) { return false; }
    crd::i32 oi = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == out_val) { oi = static_cast<crd::i32>(i); } }
    if (oi < 0) { return false; }
    const crd::usize ol = plan.buffers[static_cast<crd::usize>(oi)].alias_of >= 0 ? static_cast<crd::usize>(plan.buffers[static_cast<crd::usize>(oi)].alias_of) : static_cast<crd::usize>(oi);
    for (crd::usize e = 0; e < len; ++e) { out[e] = ptr[ol][e]; }
    return true;
}

// ARM 2 — run the WHOLE plan all on CUDA (execute_tensor_pipeline): create the device table, upload every seeded external once, run,
// read the Output back. The 29c-2 all-fallback shape, minus the capture.
bool run_all_gpu(crd::gpu::CudaComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* alloc,
                 const ceg::TensorPipelinePlan& plan, const MlpSeed* seeds, crd::usize n_seeds, const ce::Value* out_val,
                 crd::usize len, float* out)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;
    const crd::usize nb = plan.buffers.size();
    if (nb > 40U) { return false; }
    std::unique_ptr<g::ComputeBuffer> dev[40];
    std::unique_ptr<g::ComputeBuffer> up[40];
    g::ComputeBuffer*                 bufs[40] = {};
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.alias_of >= 0) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes;
        dev[i]            = compute.create_buffer(sz, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        if (dev[i] == nullptr) { return false; }
        bufs[i] = dev[i].get();
        const MlpSeed* seed = nullptr;
        if (pb.role == ceg::BufferRole::ExternalIn) { for (crd::usize s = 0; s < n_seeds; ++s) { if (seeds[s].value == pb.value) { seed = &seeds[s]; break; } } }
        if (seed == nullptr) { continue; }
        up[i] = compute.create_buffer(sz, transfer_src, g::ComputeMemory::CpuToGpu);
        if (up[i] == nullptr) { return false; }
        auto* raw = static_cast<float*>(up[i]->map());
        if (raw == nullptr) { return false; }
        for (crd::u32 e = 0; e < seed->count; ++e) { raw[e] = seed->floats[e]; }
        up[i]->unmap();
    }
    crd::i32 out_idx = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == out_val) { out_idx = static_cast<crd::i32>(i); } }
    if (out_idx < 0 || plan.buffers[static_cast<crd::usize>(out_idx)].alias_of >= 0) { return false; }
    const crd::u64                    out_bytes = plan.buffers[static_cast<crd::usize>(out_idx)].bytes;
    std::unique_ptr<g::ComputeBuffer> rb        = compute.create_buffer(out_bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
    if (rb == nullptr) { return false; }
    Resolver res;
    res.alloc_ctx   = &ctx;
    res.alloc       = alloc;
    res.compute     = &compute;
    const auto span = crd::containers::ConstSpan<g::ComputeBuffer*>(bufs, nb);
    {
        auto& rec = compute.begin();
        for (crd::usize i = 0; i < nb; ++i) { if (up[i] != nullptr) { rec.copy(*up[i], *dev[i], 0U, 0U, plan.buffers[i].bytes); } }
        compute.submit_and_wait();
    }
    {
        auto&                   rec = compute.begin();
        const ceg::ExecuteError ee  = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res, span);
        if (ee != ceg::ExecuteError::None) { return false; }
        compute.submit_and_wait();
    }
    {
        auto& rec = compute.begin();
        rec.copy(*dev[static_cast<crd::usize>(out_idx)], *rb, 0U, 0U, out_bytes);
        compute.submit_and_wait();
        const auto* got = static_cast<const float*>(rb->map());
        if (got == nullptr) { return false; }
        for (crd::usize e = 0; e < len; ++e) { out[e] = got[e]; }
        rb->unmap();
    }
    return true;
}

// ARM 3 — run the plan Host+CUDA via execute_two_class under `sc`: a host f32 table (seeded externals) + a FRESH device table (NO
// seed — the transfers populate it). The Output ends Host-visible in the host table (the runner hand-copies nothing). *n_pipes = the
// device stages resolved, *n_xfer = the transfers applied (the two falsifiability identities).
bool run_two_class_cuda(crd::gpu::CudaComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* alloc,
                        const ceg::TensorPipelinePlan& plan, crd::containers::ConstSpan<ce::ProviderClass> sc, const MlpSeed* seeds,
                        crd::usize n_seeds, const ce::Value* out_val, crd::usize len, float* out, crd::u32* n_pipes,
                        crd::u32* n_xfer, ceg::TensorPipelineProfile* profile)
{
    namespace g = crd::gpu;
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;
    const crd::usize nb = plan.buffers.size();
    if (nb > 40U) { return false; }
    crd::u64 offs[40] = {};
    crd::u64 total    = 0;
    for (crd::usize i = 0; i < nb; ++i) { offs[i] = total; if (plan.buffers[i].alias_of < 0) { total += plan.buffers[i].bytes / sizeof(float); } }
    crd::containers::Array<float> hstore(alloc);
    hstore.resize(static_cast<crd::usize>(total), -777.0F); // sentinel: a skipped Host stage leaves -777 (·beta=0 biases stay safe)
    float* hptr[40] = {};
    for (crd::usize i = 0; i < nb; ++i) { hptr[i] = plan.buffers[i].alias_of >= 0 ? hptr[static_cast<crd::usize>(plan.buffers[i].alias_of)] : hstore.data() + offs[i]; }
    for (crd::usize i = 0; i < nb; ++i)
    {
        if (plan.buffers[i].role != ceg::BufferRole::ExternalIn) { continue; }
        for (crd::usize s = 0; s < n_seeds; ++s)
        {
            if (seeds[s].value == plan.buffers[i].value) { for (crd::u32 e = 0; e < seeds[s].count; ++e) { hptr[i][e] = seeds[s].floats[e]; } }
        }
    }
    TwoClassGpu tg;
    tg.compute       = &compute;
    tg.ctx           = &ctx;
    tg.alloc         = alloc;
    tg.nb            = nb;
    tg.res.alloc_ctx = &ctx;
    tg.res.alloc     = alloc;
    tg.res.compute   = &compute;
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.alias_of >= 0) { tg.bufs[i] = tg.bufs[static_cast<crd::usize>(pb.alias_of)]; continue; } // NO seed — transfers fill it
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes;
        tg.dev[i]         = compute.create_buffer(sz, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        if (tg.dev[i] == nullptr) { return false; }
        tg.bufs[i] = tg.dev[i].get();
    }
    ceg::HostRunOptions host_opts;
    host_opts.kernel = &crd::tests::resolve_ckir_asset;
    host_opts.user   = alloc;
    const ceg::ExecuteError ee = ceg::execute_two_class(ctx, plan, sc, crd::containers::ConstSpan<float*>(hptr, nb), &tg,
                                                        &cuda_gpu_stage, &cuda_transfer, alloc, host_opts, profile);
    if (ee != ceg::ExecuteError::None) { return false; }
    if (n_pipes != nullptr) { *n_pipes = static_cast<crd::u32>(tg.res.n); }
    if (n_xfer != nullptr) { *n_xfer = tg.transfers; }
    crd::i32 oi = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == out_val) { oi = static_cast<crd::i32>(i); } }
    if (oi < 0) { return false; }
    const crd::usize ol = plan.buffers[static_cast<crd::usize>(oi)].alias_of >= 0 ? static_cast<crd::usize>(plan.buffers[static_cast<crd::usize>(oi)].alias_of) : static_cast<crd::usize>(oi);
    for (crd::usize e = 0; e < len; ++e) { out[e] = hptr[ol][e]; }
    return true;
}

// forward decls — the definitions live in the later unnamed-namespace block (shared across the TU) beside the 30b-3b test.
void  collect_named_cuda(ce::Context& ctx, ce::Region* r, crd::containers::StringView name, crd::containers::Array<ce::Operation*>& out);
float fold_sum_c(const float* base, crd::u32 n, crd::u32 stride);

// CEIR-30z — the sharded-reduction TRANSFER-COST bench for ONE dim [rows,cols] (rows sharded axis-0 into two [rows/2,cols]).
// THREE arms on the SAME lowered [Reduce,Reduce,Elementwise] plan, buffers set up ONCE (NO alloc / NO re-seed in the timed loop —
// the reduces read the shards read-only and regenerate the intermediates each run, so one seeding holds): A = all-Host
// (execute_tensor_pipeline_host, device-free baseline); B = sharded [Host,Host,Gpu] (execute_two_class: reduces on Host, the
// all-reduce combine on CUDA, 2 partial uploads + 1 readback); C = the transfer FLOOR (arm B's 3 planned transfers via
// cuda_transfer, NO compute — the discriminator: is the tax the crossing or the combine?). cpu_us[a/b/c] = median-of-9 wall/K
// (K=10); gpu_ms_b = the Gpu combine's own bracket. Gates (on the caller): bit-exact after 10 back-to-back executes (the
// aliased-storage clobber catch), n_xfer==3, gpu_ms_b>0. Returns false only on a hard build/buffer/exec failure.
bool bench_one_dim(crd::gpu::CudaComputeContext& compute, crd::memory::IAllocator* root, crd::u32 rows, crd::u32 cols,
                   double* cpu_us_a, double* cpu_us_b, double* cpu_us_c, double* gpu_ms_b, bool* bitexact, crd::u32* n_xfer)
{
    namespace g = crd::gpu;
    using crd::containers::ConstSpan;
    using crd::containers::StringView;
    ce::Context ctx(root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::tensor::register_dialect(ctx);
    (void)ce::dist::register_dist_ops(ctx);
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const fn = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(fn);
    ce::Block* const b = ce::func::func_body_block(fn);

    // the §140 chain at [rows,cols]: mesh "2" + declare[rows,cols] -> shard(axis0) -> reduce(axis0,sum) -> export.
    const ce::TypeId trc  = tf(ctx, sh2(ctx, rows, cols));
    const ce::TypeId dimc = ctx.type_dim_static(cols);
    const ce::TypeId tc   = tf(ctx, ctx.type_shape(ConstSpan<ce::TypeId>(&dimc, 1U)));
    b->append(ce::dist::build_mesh(ctx, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("2"))));
    ce::Operation* const decl = ctx.create_operation(ctx.intern_op("resource", "declare"), {}, 1U, trc);
    b->append(decl);
    ce::Operation* const sh =
        ce::dist::build_shard(ctx, decl->result(0U), ctx.attr_symbol(StringView("m2")), ctx.attr_int(0), ctx.attr_int(0), trc);
    b->append(sh);
    ce::Operation* const rd = ce::tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), ctx.attr_string(StringView("sum")), tc);
    b->append(rd);
    ce::Value* exops[1] = {rd->result(0U)};
    b->append(ctx.create_operation(ctx.intern_op("resource", "export"), ConstSpan<ce::Value*>(exops, 1U), 0U));

    if (ceg::materialize_sharding(ctx, *m, root).inserted != 1U) { return false; }
    crd::containers::HashMap<const ce::Operation*, crd::i32> lineage(root);
    if (ceg::lower_sharded_reduction(ctx, *m, root, lineage).ranks != 2U) { return false; }
    ceg::MlPartition partition(root);
    partition.assignments.push_back(ceg::MlAssignment{nullptr, 0, -1});
    partition.assignments.push_back(ceg::MlAssignment{nullptr, 1, -1});
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline_partitioned(ctx, *m, root, partition, lineage);
    if (plan.reject != ceg::PlanReject::None || plan.stages.size() != 3U) { return false; }
    ceg::MlProvider provs[2] = {};
    provs[0].provider_class = ce::ProviderClass::Host;
    provs[1].provider_class = ce::ProviderClass::Host;
    const crd::containers::Array<ce::ProviderClass> scv =
        ceg::stage_class_from_partition(plan, ConstSpan<ceg::MlProvider>(provs, 2U), ce::ProviderClass::Gpu, root);
    const auto sc = ConstSpan<ce::ProviderClass>(scv.data(), scv.size());

    crd::containers::Array<ce::Operation*> decls(root);
    collect_named_cuda(ctx, m->body(), StringView("resource.declare"), decls);
    if (decls.size() != 2U) { return false; }
    crd::containers::Array<ce::Operation*> exps(root);
    collect_named_cuda(ctx, m->body(), StringView("resource.export"), exps);
    if (exps.size() != 1U) { return false; }
    const ce::Value* const out_val = exps[0]->operand(0U);

    // seed T[rows,cols] (0.01*(i-mid)); shard r = rows [r*half,(r+1)*half). Oracle[c] = fold(shard0 col c) + fold(shard1 col c).
    const crd::u32                half = rows / 2U;
    crd::containers::Array<float> t(root);
    t.resize(static_cast<crd::usize>(rows) * cols, 0.0F);
    crd::containers::Array<float> shard0(root);
    shard0.resize(static_cast<crd::usize>(half) * cols, 0.0F);
    crd::containers::Array<float> shard1(root);
    shard1.resize(static_cast<crd::usize>(half) * cols, 0.0F);
    crd::containers::Array<float> oracle(root);
    oracle.resize(cols, 0.0F);
    const float mid = 0.5F * static_cast<float>(rows * cols);
    for (crd::u32 i = 0; i < rows * cols; ++i) { t[i] = 0.01F * (static_cast<float>(i) - mid); }
    for (crd::u32 li = 0; li < half; ++li)
    {
        for (crd::u32 c = 0; c < cols; ++c)
        {
            shard0[li * cols + c] = t[li * cols + c];
            shard1[li * cols + c] = t[(half + li) * cols + c];
        }
    }
    for (crd::u32 c = 0; c < cols; ++c)
    {
        oracle[c] = fold_sum_c(&t[c], half, cols) + fold_sum_c(&t[static_cast<crd::usize>(half) * cols + c], half, cols);
    }
    const MlpSeed seeds[2] = {{decls[0]->result(0U), shard0.data(), half * cols}, {decls[1]->result(0U), shard1.data(), half * cols}};

    const crd::usize nb = plan.buffers.size();
    if (nb > 40U) { return false; }
    crd::u64 offs[40] = {};
    crd::u64 total    = 0;
    for (crd::usize i = 0; i < nb; ++i) { offs[i] = total; if (plan.buffers[i].alias_of < 0) { total += plan.buffers[i].bytes / sizeof(float); } }
    const auto seed_host = [&](float** ptr)
    {
        for (crd::usize i = 0; i < nb; ++i)
        {
            if (plan.buffers[i].role != ceg::BufferRole::ExternalIn) { continue; }
            for (crd::usize s = 0; s < 2U; ++s)
            {
                if (seeds[s].value == plan.buffers[i].value) { for (crd::u32 e = 0; e < seeds[s].count; ++e) { ptr[i][e] = seeds[s].floats[e]; } }
            }
        }
    };
    ceg::HostRunOptions host_opts;
    host_opts.kernel = &crd::tests::resolve_ckir_asset;
    host_opts.user   = root;
    crd::i32 oi      = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == out_val) { oi = static_cast<crd::i32>(i); } }
    if (oi < 0) { return false; }
    const crd::usize ol = plan.buffers[static_cast<crd::usize>(oi)].alias_of >= 0
                              ? static_cast<crd::usize>(plan.buffers[static_cast<crd::usize>(oi)].alias_of)
                              : static_cast<crd::usize>(oi);

    // ── ARM A setup (host store, once) ──
    crd::containers::Array<float> a_store(root);
    a_store.resize(static_cast<crd::usize>(total), -777.0F);
    float* a_ptr[40] = {};
    for (crd::usize i = 0; i < nb; ++i) { a_ptr[i] = plan.buffers[i].alias_of >= 0 ? a_ptr[static_cast<crd::usize>(plan.buffers[i].alias_of)] : a_store.data() + offs[i]; }
    const auto arm_a = [&]() -> bool
    { return ceg::execute_tensor_pipeline_host(ctx, plan, ConstSpan<float*>(a_ptr, nb), root, nullptr, host_opts) == ceg::ExecuteError::None; };

    // ── ARM B setup (device table + host store + caching resolver, once) ──
    using g::compute_usage::storage;
    using g::compute_usage::transfer_dst;
    using g::compute_usage::transfer_src;
    BenchTwoClass btc;
    btc.tg.compute       = &compute;
    btc.tg.ctx           = &ctx;
    btc.tg.alloc         = root;
    btc.tg.nb            = nb;
    btc.tg.res.alloc_ctx = &ctx;
    btc.tg.res.alloc     = root;
    btc.tg.res.compute   = &compute;
    btc.bres.base.alloc_ctx = &ctx;
    btc.bres.base.alloc     = root;
    btc.bres.base.compute   = &compute;
    crd::containers::Array<float> b_store(root);
    b_store.resize(static_cast<crd::usize>(total), -777.0F);
    float* b_ptr[40] = {};
    for (crd::usize i = 0; i < nb; ++i) { b_ptr[i] = plan.buffers[i].alias_of >= 0 ? b_ptr[static_cast<crd::usize>(plan.buffers[i].alias_of)] : b_store.data() + offs[i]; }
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.alias_of >= 0) { btc.tg.bufs[i] = btc.tg.bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes;
        btc.tg.dev[i]     = compute.create_buffer(sz, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        if (btc.tg.dev[i] == nullptr) { return false; }
        btc.tg.bufs[i] = btc.tg.dev[i].get();
    }
    const auto arm_b = [&]() -> bool {
        return ceg::execute_two_class(ctx, plan, sc, ConstSpan<float*>(b_ptr, nb), &btc, &bench_gpu_stage, &bench_transfer, root,
                                      host_opts, nullptr)
               == ceg::ExecuteError::None;
    };

    // ── ARM C: the transfer FLOOR = arm B's 3 planned transfers via cuda_transfer, NO compute (matches B's per-transfer submits) ──
    const ceg::TransferPlan tp = ceg::plan_transfers(plan, sc, root);
    const auto              arm_c = [&]() -> bool
    {
        for (crd::usize i = 0; i < tp.transfers.size(); ++i)
        {
            const ceg::Transfer& xf    = tp.transfers[i];
            const crd::u64       bytes = plan.buffers[static_cast<crd::usize>(xf.buffer)].bytes;
            if (cuda_transfer(xf, b_ptr[static_cast<crd::usize>(xf.buffer)], bytes, &btc.tg) != ceg::ExecuteError::None) { return false; }
        }
        return true;
    };

    // seed ONCE (the reduces read the shards read-only; each execute regenerates the intermediates — no re-seed in the timed loop).
    seed_host(a_ptr);
    seed_host(b_ptr);

    // ── GATE run: 10 back-to-back executes of BOTH arms, then bit-exact vs oracle on the 10th output (the aliased-storage clobber
    //    catch — symmetric on A and B so the "asserted after 10 back-to-back" claim holds for both, not just B) ──
    btc.tg.transfers = 0;
    for (int it = 0; it < 10; ++it) { if (!arm_b()) { return false; } }
    for (int it = 0; it < 10; ++it) { if (!arm_a()) { return false; } }
    *bitexact = true;
    for (crd::u32 c = 0; c < cols; ++c)
    {
        if (a_ptr[ol][c] != oracle[c]) { *bitexact = false; }
        if (b_ptr[ol][c] != oracle[c]) { *bitexact = false; }
    }
    *n_xfer   = btc.tg.transfers / 10U; // 3 per execute
    *gpu_ms_b = btc.combine_gpu_ms;

    // ── TIMING: warmup 5, median of 9, K=10 iterations per sample; per-iter = wall/K in microseconds ──
    constexpr crd::u32 warmup = 5;
    constexpr crd::u32 n_samp = 9;
    constexpr crd::u32 k_rep  = 10;
    for (crd::u32 w = 0; w < warmup; ++w) { if (!arm_a() || !arm_b() || !arm_c()) { return false; } }
    double s_a[n_samp] = {};
    double s_b[n_samp] = {};
    double s_c[n_samp] = {};
    const auto time_arm = [&](double* samples, const auto& arm) -> bool
    {
        for (crd::u32 s = 0; s < n_samp; ++s)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (crd::u32 k = 0; k < k_rep; ++k) { if (!arm()) { return false; } }
            const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            samples[s]      = ns / static_cast<double>(k_rep) / 1000.0; // per-iter microseconds
        }
        return true;
    };
    if (!time_arm(s_a, arm_a) || !time_arm(s_b, arm_b) || !time_arm(s_c, arm_c)) { return false; }
    *cpu_us_a = median_of(s_a, n_samp);
    *cpu_us_b = median_of(s_b, n_samp);
    *cpu_us_c = median_of(s_c, n_samp);
    return true;
}
} // namespace

TEST_CASE("ceir 29b-1: an expanded ml.mlp runs device-resident on CUDA via execute_tensor_pipeline (gemm/relu vs the CPU MLP oracle, bit-exact)",
          "[ceir][ml][gpu][cuda]")
{
    namespace gpu = crd::gpu;
    constexpr crd::u32 mrows = 4; // batch rows M
    constexpr crd::u32 d0    = 8; // input width
    constexpr crd::u32 d1    = 8; // hidden width (M*d1 = 32 == relu.ckir bound local_size)
    constexpr crd::u32 d2    = 2; // output width

    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    ce::Module* const                  m       = ctx.create_module();
    ce::Value*                         x_val   = nullptr;
    ce::Value*                         w1_val  = nullptr;
    ce::Value*                         w2_val  = nullptr;
    const ce::Value*                   out_val = nullptr;
    const ceg::TensorPipelinePlan plan = build_mlp_plan(ctx, *m, &root, mrows, d0, d1, d2, x_val, w1_val, w2_val, out_val);
    REQUIRE(plan.stages.size() == 3U); // IDENTITY: gemm, relu, gemm — a silent stage drop can't pass
    REQUIRE(out_val != nullptr);

    // ⭐ the CPU oracle MIRRORS emit_contract_cuda's arithmetic EXACTLY (prod=a*b; acc=acc+prod; sequential k -> no MSVC
    //    cross-statement FMA; relu=max). The CUDA pipelines are fmad=false, so the device output is BIT-EXACT vs this oracle.
    float x_in[mrows * d0];
    float w1_in[d0 * d1];
    float w2_in[d1 * d2];
    float oracle[mrows * d2] = {};
    fill_mlp_data_and_oracle(mrows, d0, d1, d2, x_in, w1_in, w2_in, oracle);

    crd::memory::TlsfAllocator devalloc(64U << 20U);
    auto                       cudactx = gpu::create_cuda_compute_context(devalloc);
    REQUIRE(cudactx != nullptr);
    if (!cudactx->valid()) { WARN("no CUDA device available; skipping the CEIR-29b-1 MLP pipeline gate"); return; }

    const MlpSeed seeds[3] = {{x_val, x_in, mrows * d0}, {w1_val, w1_in, d0 * d1}, {w2_val, w2_in, d1 * d2}};
    float         d_out[mrows * d2] = {};
    crd::u32      n_alloc = 0;
    REQUIRE(run_mlp_module(*cudactx, ctx, &root, plan, seeds, 3U, MlpOut{out_val, d_out, mrows * d2}, &n_alloc));

    for (crd::u32 i = 0; i < mrows * d2; ++i) { CHECK(d_out[i] == oracle[i]); } // ⭐ BIT-EXACT (fmad=false + the per-op oracle)

    // IDENTITY on the plan's buffer topology (not self-consistency): the unfused 3-stage MLP has exactly 8 buffers -- x,W1,W2
    // (ExternalIn) + a per-gemm C-accumulator + h1,a1 (the two Intermediates) + out (Output). 26f storage-sharing is ON (the
    // PlanOptions default; this test leaves it default) and the free-list pass RUNS -- but finds NO tenancy: only Intermediate-
    // role buffers share, so `out` (Output, func-return-pinned) never lends/borrows, and the two real Intermediates h1,a1 have
    // NON-disjoint lifetimes (relu produces a1 in the SAME stage it last-reads h1, so the strict free_at<produce guard forbids
    // it). => 0 aliases, all 8 physical. n_alloc (what the CUDA loop created) must equal that count: the loop honored the plan.
    // (The alias-materialization branch of run_mlp_module is thus NOT exercised here -- trigger: a DEEPER chain with a disjoint-
    // lifetime Intermediate pair, e.g. a 2-hidden-layer MLP whose first hidden output frees before a later one is born.)
    crd::u32 non_alias = 0;
    crd::u32 aliases   = 0;
    for (crd::usize i = 0; i < plan.buffers.size(); ++i)
    {
        if (plan.buffers[i].alias_of < 0) { ++non_alias; }
        else { ++aliases; }
    }
    CHECK(plan.buffers.size() == 8U); // silent stage/buffer add or drop can't pass
    CHECK(aliases == 0U);             // the only Intermediates (h1,a1) have non-disjoint lifetimes; out is Output-role
    CHECK(non_alias == 8U);
    CHECK(n_alloc == non_alias);
}

TEST_CASE("ceir 29b-2a: the CUDA-Graphs capture of the ml.mlp pipeline is BIT-EXACT vs the N-dispatch fallback and replays deterministically",
          "[ceir][ml][gpu][cuda]")
{
    namespace gpu = crd::gpu;
    constexpr crd::u32 mrows = 4;
    constexpr crd::u32 d0    = 8;
    constexpr crd::u32 d1    = 8;
    constexpr crd::u32 d2    = 2;

    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    ce::Module* const                  m       = ctx.create_module();
    ce::Value*                         x_val   = nullptr;
    ce::Value*                         w1_val  = nullptr;
    ce::Value*                         w2_val  = nullptr;
    const ce::Value*                   out_val = nullptr;
    const ceg::TensorPipelinePlan plan = build_mlp_plan(ctx, *m, &root, mrows, d0, d1, d2, x_val, w1_val, w2_val, out_val);
    REQUIRE(plan.stages.size() == 3U);
    REQUIRE(out_val != nullptr);

    float x_in[mrows * d0];
    float w1_in[d0 * d1];
    float w2_in[d1 * d2];
    float oracle[mrows * d2] = {};
    fill_mlp_data_and_oracle(mrows, d0, d1, d2, x_in, w1_in, w2_in, oracle);

    crd::memory::TlsfAllocator devalloc(64U << 20U);
    auto                       cudactx = gpu::create_cuda_compute_context(devalloc);
    REQUIRE(cudactx != nullptr);
    if (!cudactx->valid()) { WARN("no CUDA device available; skipping the CEIR-29b-2a CUDA-Graphs capture gate"); return; }

    const MlpSeed seeds[3] = {{x_val, x_in, mrows * d0}, {w1_val, w1_in, d0 * d1}, {w2_val, w2_in, d1 * d2}};
    float         fb_out[mrows * d2]     = {};
    float         gr_out[mrows * d2]     = {};
    float         gr_replay[mrows * d2]  = {};
    crd::u32      node_count             = 0;
    bool          graph_ok               = false;
    REQUIRE(run_mlp_captured(*cudactx, ctx, &root, plan, seeds, 3U, out_val, mrows * d2, fb_out, gr_out, gr_replay,
                             &node_count, &graph_ok));

    // The capture must have produced a valid, non-empty graph (a silently-invalidated or empty capture can NOT pass here).
    REQUIRE(graph_ok);                 // cuStreamEndCapture succeeded + cuGraphInstantiateWithFlags succeeded
    CHECK(node_count == 3U);           // IDENTITY: one node per stage dispatch (gemm/relu/gemm); barriers are CUDA no-ops

    // ⭐ BIT-EXACT BY CONSTRUCTION: the graph launches the SAME fmad=false kernels in the SAME stream-capture dependency order
    //    as the fallback, so every element matches the fallback AND the CPU oracle exactly (no tolerance).
    for (crd::u32 i = 0; i < mrows * d2; ++i)
    {
        CHECK(gr_out[i] == fb_out[i]);   // graph == the N-dispatch fallback
        CHECK(gr_out[i] == oracle[i]);   // ...and both == the CPU MLP oracle
    }
    // REPLAY: the instantiate-once/launch-many property -- a second launch of the SAME exec is bit-identical (the property
    // 29z benches: launch overhead is the only difference, never the result).
    for (crd::u32 i = 0; i < mrows * d2; ++i) { CHECK(gr_replay[i] == gr_out[i]); }
}

// CEIR-29c-2 — the TWO-CLASS boundary on a real device: a partitioned plan whose cuda_graphs-claimed mlp is CAPTURED into ONE
// cudaGraph while the flanking fallback gemms run eager on the SAME stream, crossing the capture edge with a real data dependency
// (x' -> mlp -> z). ⛔ NOT build_band24: the attention fallback can't run on CUDA (emit_permute_cuda absent, ledgered at the file
// header). The proof: node_count == the CLAIMED run's stages (3), not the total (5); and the two-class output is BIT-EXACT vs the
// all-fallback N-dispatch run AND the CPU oracle (a dropped 3->4 barrier is safe -- CUDA barrier is a no-op + same-stream order).
TEST_CASE("ceir 29c-2: a two-class plan captures ONLY the cuda_graphs mlp run; the flanking fallback gemms cross the capture edge BIT-EXACT on CUDA",
          "[ceir][ml][gpu][cuda]")
{
    namespace gpu = crd::gpu;
    constexpr crd::u32 mrows = 4;
    constexpr crd::u32 d0    = 8;
    constexpr crd::u32 d1    = 8; // M*d1 = 32 == relu.ckir bound local_size
    constexpr crd::u32 d2    = 2;
    constexpr crd::u32 d3    = 2;

    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    ce::Module* const                  m       = ctx.create_module();
    ce::Value*                         x_val   = nullptr;
    ce::Value*                         w0_val  = nullptr;
    ce::Value*                         w1_val  = nullptr;
    ce::Value*                         w2_val  = nullptr;
    ce::Value*                         w3_val  = nullptr;
    const ce::Value*                   out_val = nullptr;
    crd::u32                           n_assign = 0;
    const ceg::TensorPipelinePlan      plan = build_two_class_plan(ctx, *m, &root, mrows, d0, d1, d2, d3, x_val, w0_val, w1_val,
                                                                   w2_val, w3_val, out_val, &n_assign);

    // ── PLAN HALF (device-free — CI without a CUDA device still runs this): the two-class tag pattern. ──
    REQUIRE(plan.reject == ceg::PlanReject::None); // PartitionLineageMismatch UNTRIGGERED (every lineage index is 0 < assignments==1)
    REQUIRE(out_val != nullptr);
    CHECK(n_assign == 1U);              // only the mlp is an ml op; the flanking linalg.gemms are invisible to partition_ml
    REQUIRE(plan.stages.size() == 5U);  // [gemm(x'), gemm(mlp), relu, gemm(mlp), gemm(z)] — a silent stage drop can't pass
    const crd::i32 want[5] = {-1, 0, 0, 0, -1}; // one contiguous cuda_graphs run flanked by two fallback gemms
    for (crd::usize s = 0; s < 5U; ++s) { CHECK(plan.stages[s].provider == want[s]); }
    // derive the claimed run the SAME way the runner does (scan provider==0) — it must be exactly [1,4).
    crd::usize lo = 0;
    while (lo < plan.stages.size() && plan.stages[lo].provider != 0) { ++lo; }
    crd::usize hi = lo;
    while (hi < plan.stages.size() && plan.stages[hi].provider == 0) { ++hi; }
    CHECK(lo == 1U);
    CHECK(hi == 4U);
    // READ (not guess — the 29b-1 mechanism-read scar), then LOCK the 26f alias materializations (the topology, the 29b-1
    // "8 buffers / 0 aliases" mold). 29b-1's pure mlp had 0 aliases; this sandwich has EXACTLY 2, both Intermediate->Intermediate
    // with tenant.bytes <= landlord.bytes (the 26f landlord-grows rule). READ from the plan (op kinds + shapes):
    //   (a) a1 — the RELU OUTPUT (a `resource.declare` the relu dispatch WRITES, so plan-role Intermediate, [M,d1]) — reuses
    //       x''s slot (the leading gemm output [M,d0], dead after the mlp's first gemm reads it). ⛔ THIS is the alias the relu
    //       write lands in, so the captured mlp run DESTROYS x' — the replay-breaking tenancy (see run_two_class_captured step 4).
    //   (b) y — the mlp's second gemm output [M,d2] — reuses h1's slot (the mlp's first gemm output [M,d1], dead after the relu).
    // Both are the disjoint-lifetime Intermediate pair 29b-1 named as the un-exercised trigger.
    const crd::u32 b_md0        = mrows * d0 * static_cast<crd::u32>(sizeof(float)); // [M,d0] = x' bytes
    const crd::u32 b_md1        = mrows * d1 * static_cast<crd::u32>(sizeof(float)); // [M,d1] = a1/h1 bytes (==b_md0 here, d0==d1)
    const crd::u32 b_md2        = mrows * d2 * static_cast<crd::u32>(sizeof(float)); // [M,d2] = y bytes
    crd::u32       plan_aliases = 0;
    crd::u32       alias_a1_x   = 0; // (a) a1 [M,d1] reuses x''s [M,d0] slot — the relu write destroys x'
    crd::u32       alias_y_h1   = 0; // (b) y  [M,d2] reuses h1's [M,d1] slot — the landlord grew to fit
    for (crd::usize i = 0; i < plan.buffers.size(); ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.alias_of < 0) { continue; }
        ++plan_aliases;
        const ceg::PlanBuffer& lp = plan.buffers[static_cast<crd::usize>(pb.alias_of)];
        CHECK(pb.role == ceg::BufferRole::Intermediate); // only Intermediate-role buffers tenant (out is Output, pinned). NOTE
        CHECK(lp.role == ceg::BufferRole::Intermediate); // (a)'s tenant is a `resource.declare` (a1) — declares aren't always
        CHECK(pb.bytes <= lp.bytes);                     // ExternalIn; the plan gives a written-then-read declare Intermediate role.
        if (pb.bytes == b_md1 && lp.bytes == b_md0) { ++alias_a1_x; } // a1 -> x'
        if (pb.bytes == b_md2 && lp.bytes == b_md1) { ++alias_y_h1; } // y  -> h1
    }
    CHECK(plan_aliases == 2U); // ⭐ FIRST live alias materialization on the CUDA runner (29b-1 had 0)
    CHECK(alias_a1_x == 1U);
    CHECK(alias_y_h1 == 1U);

    float x_in[mrows * d0];
    float w0_in[d0 * d0];
    float w1_in[d0 * d1];
    float w2_in[d1 * d2];
    float w3_in[d2 * d3];
    float oracle[mrows * d3] = {};
    crd::tests::fill_two_class_sandwich(mrows, d0, d1, d2, d3, x_in, w0_in, w1_in, w2_in, w3_in, oracle);

    crd::memory::TlsfAllocator devalloc(64U << 20U);
    auto                       cudactx = gpu::create_cuda_compute_context(devalloc);
    REQUIRE(cudactx != nullptr);
    if (!cudactx->valid()) { WARN("no CUDA device available; skipping the CEIR-29c-2 two-class capture gate"); return; }

    const MlpSeed seeds[5] = {{x_val, x_in, mrows * d0}, {w0_val, w0_in, d0 * d0}, {w1_val, w1_in, d0 * d1},
                              {w2_val, w2_in, d1 * d2}, {w3_val, w3_in, d2 * d3}};
    float         fb_out[mrows * d3]    = {};
    float         gr_out[mrows * d3]    = {};
    float         gr_replay[mrows * d3] = {};
    float         enq_out[mrows * d3]   = {}; // CEIR-29z: the single-submit enqueue path's output
    crd::u32      node_count            = 0;
    crd::u32      n_aliases             = 0;
    crd::u32      n_pipes               = 0;
    bool          graph_ok             = false;
    crd::usize    rlo                   = 0;
    crd::usize    rhi                   = 0;
    REQUIRE(run_two_class_captured(*cudactx, ctx, &root, plan, seeds, 5U, out_val, mrows * d3, fb_out, gr_out, gr_replay,
                                   &node_count, &graph_ok, &rlo, &rhi, &n_aliases, &n_pipes, enq_out));

    REQUIRE(graph_ok);         // cuStreamEndCapture + cuGraphInstantiateWithFlags succeeded
    CHECK(node_count == 3U);   // ⭐ IDENTITY: the graph captured ONLY the claimed mlp run (3 stages), NOT the flanking gemms (total 5)
    CHECK(rlo == 1U);          // the runner scanned the SAME claimed run the plan half asserted
    CHECK(rhi == 4U);
    CHECK(n_aliases == plan_aliases); // the runner's buffer loop honored the plan's aliasing exactly
    // ⭐ FALSIFIABILITY (advisor): node_count==3 alone can't distinguish "captured 3, flanking gemms ran" from "captured 3, flanking
    //    gemms silently skipped". The resolve count DOES: ALL-FALLBACK 5 + TWO-CLASS (1+3+1) + REPLAY (prefix 1 + suffix 1) = 12.
    //    A skipped fallback dispatch drops below 12; a graph launch that secretly re-resolved rises above. Exactly 12 = each
    //    fallback stage DISPATCHED through execute_tensor_pipeline + the captured run resolved ONCE + the launches re-ran nothing.
    CHECK(n_pipes == 12U);

    // ⭐ the load-bearing claim: capturing ONLY the mlp (the fallback gemms run eager on the same stream) is BIT-EXACT vs the
    //    all-fallback N-dispatch run AND the CPU oracle. A shared-alias bug can't hide — it would corrupt BOTH runs vs the oracle.
    for (crd::u32 i = 0; i < mrows * d3; ++i)
    {
        CHECK(gr_out[i] == fb_out[i]); // two-class == the N-dispatch fallback
        CHECK(gr_out[i] == oracle[i]); // ...and both == the CPU oracle (fmad=false, sequential-k)
    }
    for (crd::u32 i = 0; i < mrows * d3; ++i) { CHECK(gr_replay[i] == gr_out[i]); } // instantiate-once / launch-many

    // ⭐ CEIR-29z: the SINGLE-SUBMIT enqueue path (begin -> prefix -> enqueue(graph) -> suffix -> submit_and_wait, ONE wait) is
    //    the honest two-class execution the 29z bench times — PROVEN bit-exact here (CI-gated), so the enqueue split is a real
    //    execution path, not a bench-only convenience. Same output as the waiting-launch two-class run AND the oracle.
    for (crd::u32 i = 0; i < mrows * d3; ++i)
    {
        CHECK(enq_out[i] == gr_out[i]); // single-submit == the waiting-launch two-class run
        CHECK(enq_out[i] == oracle[i]); // ...and == the CPU oracle
    }
}

// CEIR-30b-2b-2b: THE §140 REAL-DOMAIN PROOF — the SAME sandwich plan runs BIT-EXACT across three placements: all-Host (CPU eval),
// all-CUDA, and a Host+CUDA SPLIT [Gpu,Host,Host,Host,Gpu] via execute_two_class (the leading/trailing gemms on CUDA, the mlp+relu
// on the CPU — a placement that DELIBERATELY cross-cuts the partition tags, proving the split is driven by the placement vector, not
// the plan). "Placement is semantic, not glue": each class is bit-exact vs the oracle and the planned host↔device transfers are
// lossless, so split == all-Gpu == all-Host == oracle. The all-Host arm is device-FREE (CI without a GPU still runs it); the two
// device arms WARN-skip when no CUDA device is present. Falsifiability: n_pipes==2 (exactly the 2 device stages resolved, not the
// Host ones) + n_xfer == plan_transfers (the runner applied exactly the planned crossings, hand-copied nothing).
TEST_CASE("ceir 30b-2b-2b: execute_two_class runs the sandwich Host+CUDA bit-exact vs all-Host, all-Gpu, and the CPU oracle",
          "[ceir][ml][gpu][cuda]")
{
    namespace gpu = crd::gpu;
    constexpr crd::u32 mrows = 4;
    constexpr crd::u32 d0    = 8;
    constexpr crd::u32 d1    = 8; // M*d1 = 32 == relu.ckir bound local_size
    constexpr crd::u32 d2    = 2;
    constexpr crd::u32 d3    = 2;

    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    ce::Module* const                  m       = ctx.create_module();
    ce::Value*                         x_val   = nullptr;
    ce::Value*                         w0_val  = nullptr;
    ce::Value*                         w1_val  = nullptr;
    ce::Value*                         w2_val  = nullptr;
    ce::Value*                         w3_val  = nullptr;
    const ce::Value*                   out_val = nullptr;
    const ceg::TensorPipelinePlan      plan = build_two_class_plan(ctx, *m, &root, mrows, d0, d1, d2, d3, x_val, w0_val, w1_val,
                                                                   w2_val, w3_val, out_val, nullptr);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 5U);
    REQUIRE(out_val != nullptr);

    float x_in[mrows * d0];
    float w0_in[d0 * d0];
    float w1_in[d0 * d1];
    float w2_in[d1 * d2];
    float w3_in[d2 * d3];
    float oracle[mrows * d3] = {};
    crd::tests::fill_two_class_sandwich(mrows, d0, d1, d2, d3, x_in, w0_in, w1_in, w2_in, w3_in, oracle);
    const MlpSeed seeds[5] = {{x_val, x_in, mrows * d0}, {w0_val, w0_in, d0 * d0}, {w1_val, w1_in, d0 * d1},
                              {w2_val, w2_in, d1 * d2}, {w3_val, w3_in, d2 * d3}};

    // ARM 1 — all-HOST (device-FREE: runs in a CI job with no CUDA device).
    float host_out[mrows * d3] = {};
    REQUIRE(run_all_host(ctx, &root, plan, seeds, 5U, out_val, mrows * d3, host_out));
    for (crd::u32 i = 0; i < mrows * d3; ++i) { CHECK(host_out[i] == oracle[i]); } // the Host executor is bit-exact vs the oracle

    crd::memory::TlsfAllocator devalloc(64U << 20U);
    auto                       cudactx = gpu::create_cuda_compute_context(devalloc);
    REQUIRE(cudactx != nullptr);
    if (!cudactx->valid()) { WARN("no CUDA device available; skipping the CEIR-30b-2b-2b device arms (the all-Host arm ran above)"); return; }

    // ARM 2 — all-CUDA (the whole plan through execute_tensor_pipeline).
    float gpu_out[mrows * d3] = {};
    REQUIRE(run_all_gpu(*cudactx, ctx, &root, plan, seeds, 5U, out_val, mrows * d3, gpu_out));

    // ARM 3 — the Host+CUDA SPLIT [Gpu,Host,Host,Host,Gpu] via execute_two_class.
    const ce::ProviderClass sc[5] = {ce::ProviderClass::Gpu, ce::ProviderClass::Host, ce::ProviderClass::Host,
                                     ce::ProviderClass::Host, ce::ProviderClass::Gpu};
    float                 mix_out[mrows * d3] = {};
    crd::u32              n_pipes             = 0;
    crd::u32              n_xfer              = 0;
    ceg::TensorPipelineProfile profile(&root);
    REQUIRE(run_two_class_cuda(*cudactx, ctx, &root, plan, crd::containers::ConstSpan<ce::ProviderClass>(sc, 5U), seeds, 5U, out_val,
                               mrows * d3, mix_out, &n_pipes, &n_xfer, &profile));

    // ⭐ FALSIFIABILITY: exactly the 2 device stages (0 and 4) resolved on the device — none of the Host stages touched the CUDA
    //    Resolver (else "the device stage silently ran on Host, oracle still matched" would pass); and the runner applied EXACTLY
    //    the planned crossings (hand-copied nothing).
    CHECK(n_pipes == 2U);
    const ceg::TransferPlan tp = ceg::plan_transfers(plan, crd::containers::ConstSpan<ce::ProviderClass>(sc, 5U), &root);
    CHECK(n_xfer == static_cast<crd::u32>(tp.transfers.size()));
    // ⭐ and every stage RAN, in order (Host + device alike): a profile row per stage proves the Host stages 1-3 were not silently
    //    skipped (a skip would zero-propagate through the beta=0 tail and the seed pattern alone would not catch it).
    REQUIRE(profile.stages.size() == 5U);
    const ceg::StageKind want[5] = {ceg::StageKind::Gemm, ceg::StageKind::Gemm, ceg::StageKind::VizDispatch,
                                    ceg::StageKind::Gemm, ceg::StageKind::Gemm};
    for (crd::usize s = 0; s < 5U; ++s) { CHECK(profile.stages[s].kind == want[s]); }

    // ⭐ THE §140 CLAIM: split == all-Gpu == all-Host == the oracle, BIT-EXACT (placement changed the WHERE, never the WHAT).
    for (crd::u32 i = 0; i < mrows * d3; ++i)
    {
        CHECK(gpu_out[i] == oracle[i]);  // all-CUDA is bit-exact (fmad=false + the per-op oracle)
        CHECK(mix_out[i] == oracle[i]);  // the Host+CUDA split is bit-exact
        CHECK(mix_out[i] == gpu_out[i]); // ...and identical to all-CUDA (the transfers are lossless)
    }
}

namespace
{
// CEIR-30b-3b: collect every op named `name` (pre-order) — the lowered shard declares + the export, post-lowering.
void collect_named_cuda(ce::Context& ctx, ce::Region* r, crd::containers::StringView name, // NOLINT(misc-no-recursion)
                        crd::containers::Array<ce::Operation*>& out)
{
    if (r == nullptr) { return; }
    for (ce::Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (ce::Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == name) { out.push_back(op); }
            for (crd::u32 i = 0; i < op->num_regions(); ++i) { collect_named_cuda(ctx, op->region(i), name, out); }
        }
    }
}
// a LEFT-FOLD f32 sum of `n` values at `base` strided by `stride` (matches eval_cpu's ReduceSum order — the two-stage oracle).
float fold_sum_c(const float* base, crd::u32 n, crd::u32 stride)
{
    float acc = base[0];
    for (crd::u32 i = 1; i < n; ++i) { acc = acc + base[static_cast<crd::usize>(i) * stride]; }
    return acc;
}
} // namespace

// CEIR-30b-3b — THE §140 SHARDED-REDUCTION PROOF on Host+CUDA. The `lower_sharded_reduction` output (2 per-rank reduces + an
// all-reduce elementwise combine) runs through `execute_two_class` with the placement `[Host, Host, Gpu]` PRODUCED from the
// lowering's rank_lineage (both ranks' reduces on the CPU — eval_cpu handles the axis0 strided fold; the all-reduce COMBINE on
// CUDA via emit_elementwise_cuda). The two partials cross H->D as PLANNED uploads, combine on the device, the result crosses
// D->H — placement drove every transfer, the runner hand-copied nothing (§140 "placement is semantic, not glue"). Bit-exact vs
// all-Host AND the TWO-STAGE oracle (fold(rows 0-3)+fold(rows 4-7), NOT a flat 8-row fold — f32 non-associative). ⛔ NO all-Gpu
// arm: a reduce stage on CUDA is UnresolvedKernel (emit_reduce_cuda is trailing-axis-only + its 2-scalar push is incompatible
// with the single-blob CUDA dispatch — the cross-launch-site contract of [[feedback_cuda_emitter_signature...]], its OWN slice).
TEST_CASE("ceir 30b-3b/30c-2b: the sec-140 reduction lowered [Host,Host,Gpu] runs on CUDA (placement HAND-WRITTEN 30b-3b + "
          "AUTHORED-ASSET-LOADED 30c-2b) bit-exact vs all-Host and the two-stage oracle",
          "[ceir][ml][gpu][cuda][dist][30c-2b]")
{
    namespace gpu = crd::gpu;
    using crd::containers::ConstSpan;
    using crd::containers::StringView;

    crd::memory::GrowableTlsfAllocator root;
    ce::Context                        ctx(&root);
    (void)ce::func::register_dialect(ctx);
    (void)ce::resource::register_resource_ops(ctx);
    (void)ce::tensor::register_dialect(ctx);
    (void)ce::dist::register_dist_ops(ctx);
    (void)ce::transform::register_transform_ops(ctx); // CEIR-30c-2b: parse the committed place_mesh.ceir placement asset
    ce::Module* const m   = ctx.create_module();
    ce::Block*        top = m->body()->first_block();
    if (top == nullptr) { top = ctx.create_block(0U); m->body()->append(top); }
    ce::Operation* const fn = ce::func::create_func(ctx, *m, "main", ce::Visibility::Public, 0U);
    top->append(fn);
    ce::Block* const b = ce::func::func_body_block(fn);

    const ce::TypeId t84  = tf(ctx, sh2(ctx, 8U, 4U)); // [8,4], sharded along axis0 into two [4,4]
    const ce::TypeId dim4 = ctx.type_dim_static(4U);
    const ce::TypeId t4   = tf(ctx, ctx.type_shape(ConstSpan<ce::TypeId>(&dim4, 1U))); // [4]

    // the §140 chain: declare[8,4] -> shard(mesh "2", axis0) -> reduce(axis0, sum) -> export.
    b->append(ce::dist::build_mesh(ctx, ctx.attr_symbol(StringView("m2")), ctx.attr_string(StringView("2"))));
    ce::Operation* const decl = ctx.create_operation(ctx.intern_op("resource", "declare"), {}, 1U, t84);
    b->append(decl);
    ce::Operation* const sh =
        ce::dist::build_shard(ctx, decl->result(0U), ctx.attr_symbol(StringView("m2")), ctx.attr_int(0), ctx.attr_int(0), t84);
    b->append(sh);
    ce::Operation* const rd = ce::tensor::build_reduce(ctx, sh->result(0U), ctx.attr_int(0), ctx.attr_string(StringView("sum")), t4);
    b->append(rd);
    ce::Value* exops[1] = {rd->result(0U)};
    b->append(ctx.create_operation(ctx.intern_op("resource", "export"), ConstSpan<ce::Value*>(exops, 1U), 0U));

    const ceg::MaterializeResult mres = ceg::materialize_sharding(ctx, *m, &root); // inserts the all_reduce completing the Partial
    REQUIRE(mres.inserted == 1U);

    // CEIR-30c-2b — the AUTHORED placement drives the WHERE. ⭐ dist-verify-FIRST: the payload walks clean BEFORE the loader
    //   (this is the OTHER real call site the 30c-1 `partition_ml.cpp` fold comment named — now BOTH sites walk find_dist_misuse,
    //   so "extent<=0 unreachable past find_dist_misuse" is honest end-to-end). Resolve the placement BEFORE lowering:
    //   lower_sharded_reduction STRIPS the mesh, and placement_from_transform needs mesh "m2" present to validate the rank count.
    REQUIRE(ce::dist::find_dist_misuse(ctx, *m).kind == ce::dist::DistMisuseKind::None);
    const crd::containers::Array<char> pl_src =
        ce::test_support::slurp_asset(CRD_REPO_DIR "/assets/ceir/place_mesh_host_host_gpu.ceir", ctx);
    const ce::ParseResult pl_pr = ce::parse(ctx, StringView(pl_src.data(), pl_src.size()));
    REQUIRE(pl_pr.ok);
    REQUIRE(pl_pr.module != nullptr);
    const ceg::Placement plc = ceg::placement_from_transform(ctx, *pl_pr.module, *m, &root);
    REQUIRE(plc.kind == ceg::PlacementKind::Resolved);
    REQUIRE(plc.rank_classes.size() == 2U);

    crd::containers::HashMap<const ce::Operation*, crd::i32> lineage(&root);
    const ceg::LowerResult                                  lr = ceg::lower_sharded_reduction(ctx, *m, &root, lineage);
    REQUIRE(lr.ranks == 2U);
    REQUIRE_FALSE(lr.had_conflict);

    ceg::MlPartition partition(&root);
    partition.assignments.push_back(ceg::MlAssignment{nullptr, 0, -1});
    partition.assignments.push_back(ceg::MlAssignment{nullptr, 1, -1});
    const ceg::TensorPipelinePlan plan = ceg::plan_tensor_pipeline_partitioned(ctx, *m, &root, partition, lineage);
    REQUIRE(plan.reject == ceg::PlanReject::None);
    REQUIRE(plan.stages.size() == 3U);
    CHECK(plan.stages[0].kind == ceg::StageKind::Reduce);
    CHECK(plan.stages[1].kind == ceg::StageKind::Reduce);
    CHECK(plan.stages[2].kind == ceg::StageKind::Elementwise);

    // the placement PRODUCED from rank_lineage: both tagged reduces -> Host, the untagged combine -> the Gpu fallback => [Host,Host,Gpu].
    ceg::MlProvider provs[2] = {};
    provs[0].provider_class  = ce::ProviderClass::Host;
    provs[1].provider_class  = ce::ProviderClass::Host;
    const crd::containers::Array<ce::ProviderClass> scv =
        ceg::stage_class_from_partition(plan, ConstSpan<ceg::MlProvider>(provs, 2U), ce::ProviderClass::Gpu, &root);
    REQUIRE(scv.size() == 3U);
    CHECK(scv[0] == ce::ProviderClass::Host);
    CHECK(scv[1] == ce::ProviderClass::Host);
    CHECK(scv[2] == ce::ProviderClass::Gpu);

    // ⭐ 30c-2b — the AUTHORED-ASSET placement produces the SAME per-stage vector as the hand-written partition producer.
    //   The hand-written stage_class_from_partition STAYS as the PARITY reference (deleting it would degrade the arm to
    //   can't-fail); the LOADED vector below DRIVES execute_two_class — placement is an authored ASSET, not code (§146).
    const crd::containers::Array<ce::ProviderClass> scv_asset = ceg::stage_class_from_placement(
        plan, ConstSpan<ce::ProviderClass>(plc.rank_classes.data(), plc.rank_classes.size()), plc.fallback, &root);
    REQUIRE(scv_asset.size() == scv.size());
    // ⭐ REQUIRE, not CHECK: the LOADED vector drives execution below, so every downstream falsifiability check
    //   (plan_transfers, n_xfer, oracle) self-agrees with WHATEVER the asset resolved to — a DRIFTED asset must ABORT
    //   the arm HERE (the ONE place the loaded vector is compared to the independent hand-written reference), not
    //   decorate an otherwise-green run with a single buried failure.
    for (crd::usize i = 0; i < scv.size(); ++i) { REQUIRE(scv_asset[i] == scv[i]); }
    const ConstSpan<ce::ProviderClass> sc(scv_asset.data(), scv_asset.size()); // ⭐ the LOADED placement drives execution

    // the 2 shard-declare inputs (decls[0]=rank 0 rows [0,4), decls[1]=rank 1 rows [4,8)) + the Output (the combine the export reads).
    crd::containers::Array<ce::Operation*> decls(&root);
    collect_named_cuda(ctx, m->body(), StringView("resource.declare"), decls);
    REQUIRE(decls.size() == 2U);
    crd::containers::Array<ce::Operation*> exps(&root);
    collect_named_cuda(ctx, m->body(), StringView("resource.export"), exps);
    REQUIRE(exps.size() == 1U);
    const ce::Value* const out_val = exps[0]->operand(0U);

    // seed T[8,4] (0.1*(i-12)); shard0 = rows [0,4), shard1 = rows [4,8), each [4,4]. Oracle = fold(rows 0-3)+fold(rows 4-7) per col.
    float t[32];
    for (crd::u32 i = 0; i < 32U; ++i) { t[i] = 0.1F * static_cast<float>(static_cast<crd::i32>(i) - 12); }
    float shard0[16];
    float shard1[16];
    for (crd::u32 li = 0; li < 4U; ++li)
    {
        for (crd::u32 c = 0; c < 4U; ++c)
        {
            shard0[li * 4U + c] = t[(0U * 4U + li) * 4U + c]; // rows [0,4)
            shard1[li * 4U + c] = t[(1U * 4U + li) * 4U + c]; // rows [4,8)
        }
    }
    float oracle[4];
    bool  any_diff = false;
    for (crd::u32 c = 0; c < 4U; ++c)
    {
        const float p0 = fold_sum_c(&t[c], 4U, 4U);       // rank 0: rows [0,4), column c
        const float p1 = fold_sum_c(&t[16U + c], 4U, 4U); // rank 1: rows [4,8)
        oracle[c]      = p0 + p1;                          // the two-stage combine
        if (oracle[c] != fold_sum_c(&t[c], 8U, 4U)) { any_diff = true; } // != a flat 8-row fold (non-assoc)
    }
    CHECK(any_diff); // the split changes the summation grouping — the oracle is genuinely the TWO-STAGE one
    const MlpSeed seeds[2] = {{decls[0]->result(0U), shard0, 16U}, {decls[1]->result(0U), shard1, 16U}};

    // ARM 1 — all-HOST (device-FREE: runs in CI without a CUDA device).
    float host_out[4] = {};
    REQUIRE(run_all_host(ctx, &root, plan, seeds, 2U, out_val, 4U, host_out));
    for (crd::u32 c = 0; c < 4U; ++c) { CHECK(host_out[c] == oracle[c]); }

    crd::memory::TlsfAllocator devalloc(64U << 20U);
    auto                       cudactx = gpu::create_cuda_compute_context(devalloc);
    REQUIRE(cudactx != nullptr);
    if (!cudactx->valid()) { WARN("no CUDA device available; skipping the CEIR-30b-3b device arm (the all-Host arm ran above)"); return; }

    // ARM 2 — the Host+CUDA SPLIT [Host, Host, Gpu] via execute_two_class: the per-rank reduces on the CPU, the all-reduce COMBINE on CUDA.
    float                      mix_out[4] = {};
    crd::u32                   n_pipes    = 0;
    crd::u32                   n_xfer     = 0;
    ceg::TensorPipelineProfile profile(&root);
    REQUIRE(run_two_class_cuda(*cudactx, ctx, &root, plan, sc, seeds, 2U, out_val, 4U, mix_out, &n_pipes, &n_xfer, &profile));

    // ⭐ FALSIFIABILITY: EXACTLY the ONE combine resolved on CUDA (both reduces ran on Host — else n_pipes would count them).
    CHECK(n_pipes == 1U);
    // the runner applied EXACTLY the planned crossings: 2 partial uploads (before the Gpu combine, stage 2) + the ONE Output readback.
    const ceg::TransferPlan tp = ceg::plan_transfers(plan, sc, &root);
    CHECK(n_xfer == static_cast<crd::u32>(tp.transfers.size()));
    crd::u32 up_at2    = 0;
    crd::u32 rb_at_end = 0;
    for (crd::usize i = 0; i < tp.transfers.size(); ++i)
    {
        if (tp.transfers[i].direction == ceg::TransferDir::HostToDevice && tp.transfers[i].before_stage == 2U) { ++up_at2; }
        if (tp.transfers[i].direction == ceg::TransferDir::DeviceToHost
            && tp.transfers[i].before_stage == static_cast<crd::u32>(plan.stages.size()))
        {
            ++rb_at_end;
        }
    }
    CHECK(tp.transfers.size() == 3U);
    CHECK(up_at2 == 2U);    // both partials upload before the Gpu combine (stage 2)
    CHECK(rb_at_end == 1U); // ⭐ the Output reads back at before_stage==ns — fires ONLY because the combine ran on the DEVICE
    REQUIRE(profile.stages.size() == 3U);
    CHECK(profile.stages[0].kind == ceg::StageKind::Reduce);
    CHECK(profile.stages[1].kind == ceg::StageKind::Reduce);
    CHECK(profile.stages[2].kind == ceg::StageKind::Elementwise);

    // ⭐ THE §140 CLAIM: the Host+CUDA split == all-Host == the two-stage oracle, EVERY element (placement drove the WHERE, not the WHAT).
    for (crd::u32 c = 0; c < 4U; ++c)
    {
        CHECK(mix_out[c] == oracle[c]);
        CHECK(mix_out[c] == host_out[c]);
    }
}

// CEIR-29z — the BAND-CLOSE BENCH: the N-dispatch fallback vs the 1-graph launch, on BOTH device fixtures. Env-gated
// (CRD_CEIR_BENCH) so the default ctest sweep does not time; the measured board is docs/bench/2026-09-05-ceir29-cuda-graphs-
// launch.md (written at measurement time). ⛔ cpu_us (median-of-9, per pipeline iteration) is the HEADLINE — the CUDA-Graphs win
// is the CPU submit path (N cuLaunchKernel + the recorder walk vs one cuGraphLaunch); gpu_ms (compute.last_gpu_ms) is the parity
// sanity column — identical kernels in identical stream order => ~1.0x is EXPECTED, and its parity is the sentence that proves the
// win is submit-path, not a GPU artifact (SANITY #6: a "loss" recorded honestly). The bit-exact gate + gpu_ms>0 + node_count==3
// keep it a bench, not a number (the --gpu-cull empty-canvas scar). Run: CRD_CEIR_BENCH=1 ctest -V -R "29z" (or the exe with -s).
TEST_CASE("ceir 29z: CUDA-Graphs launch vs the N-dispatch fallback bench (submit-path win; GPU parity)",
          "[ceir][ml][gpu][cuda][bench]")
{
    namespace gpu = crd::gpu;
    if (std::getenv("CRD_CEIR_BENCH") == nullptr)
    {
        WARN("CRD_CEIR_BENCH unset; skipping the CEIR-29z CUDA-Graphs launch bench (set CRD_CEIR_BENCH=1 to time)");
        return;
    }
    crd::memory::TlsfAllocator devalloc(64U << 20U);
    auto                       cudactx = gpu::create_cuda_compute_context(devalloc);
    REQUIRE(cudactx != nullptr);
    if (!cudactx->valid()) { WARN("no CUDA device available; skipping the CEIR-29z bench"); return; }

    // ── fixture A: the 3-stage mlp (gemm/relu/gemm) — the WHOLE plan captured into ONE graph (unpartitioned; cap=[0,3)) ──
    {
        constexpr crd::u32 mrows = 4;
        constexpr crd::u32 d0    = 8;
        constexpr crd::u32 d1    = 8;
        constexpr crd::u32 d2    = 2;
        crd::memory::GrowableTlsfAllocator root;
        ce::Context                        ctx(&root);
        ce::Module* const                  m       = ctx.create_module();
        ce::Value*                         x_val   = nullptr;
        ce::Value*                         w1_val  = nullptr;
        ce::Value*                         w2_val  = nullptr;
        const ce::Value*                   out_val = nullptr;
        const ceg::TensorPipelinePlan      plan = build_mlp_plan(ctx, *m, &root, mrows, d0, d1, d2, x_val, w1_val, w2_val, out_val);
        REQUIRE(plan.stages.size() == 3U);
        float x_in[mrows * d0];
        float w1_in[d0 * d1];
        float w2_in[d1 * d2];
        float oracle[mrows * d2] = {};
        fill_mlp_data_and_oracle(mrows, d0, d1, d2, x_in, w1_in, w2_in, oracle);
        const MlpSeed seeds[3] = {{x_val, x_in, mrows * d0}, {w1_val, w1_in, d0 * d1}, {w2_val, w2_in, d1 * d2}};
        double        fb[3] = {};
        double        gr[3] = {};
        double        fg  = 0.0;
        double        gg  = 0.0;
        crd::u32      nc  = 0;
        bool          gok = false;
        bool          bx  = false;
        crd::usize    clo = 0;
        crd::usize    chi = 0;
        REQUIRE(bench_captured(*cudactx, ctx, &root, plan, seeds, 3U, out_val, oracle, mrows * d2, fb, gr, &fg, &gg, &nc, &gok,
                               &bx, &clo, &chi));
        REQUIRE(gok);
        CHECK(nc == 3U);
        CHECK(bx);
        CHECK(fg > 0.0);
        CHECK(gg > 0.0);
        CHECK(clo == 0U);
        CHECK(chi == 3U);
        INFO("ceir29z fixture=mlp3 cap=[" << clo << "," << chi << ") node_count=" << nc << " gpu_ms{fb=" << fg << ",gr=" << gg
                                          << ",ratio=" << (gg > 0.0 ? fg / gg : 0.0) << "} cpu_us_per_iter K1{fb=" << fb[0]
                                          << ",gr=" << gr[0] << ",x=" << (gr[0] > 0.0 ? fb[0] / gr[0] : 0.0) << "} K10{fb=" << fb[1]
                                          << ",gr=" << gr[1] << ",x=" << (gr[1] > 0.0 ? fb[1] / gr[1] : 0.0) << "} K100{fb=" << fb[2]
                                          << ",gr=" << gr[2] << ",x=" << (gr[2] > 0.0 ? fb[2] / gr[2] : 0.0) << "}");
        CHECK(true); // anchor the INFO row (prints under catch2 -s / ctest -V)
    }

    // ── fixture B: the 5-stage two-class sandwich — the 3-stage cuda_graphs run (cap=[1,4)) flanked by 2 eager fallback gemms ──
    {
        constexpr crd::u32 mrows = 4;
        constexpr crd::u32 d0    = 8;
        constexpr crd::u32 d1    = 8;
        constexpr crd::u32 d2    = 2;
        constexpr crd::u32 d3    = 2;
        crd::memory::GrowableTlsfAllocator root;
        ce::Context                        ctx(&root);
        ce::Module* const                  m        = ctx.create_module();
        ce::Value*                         x_val    = nullptr;
        ce::Value*                         w0_val   = nullptr;
        ce::Value*                         w1_val   = nullptr;
        ce::Value*                         w2_val   = nullptr;
        ce::Value*                         w3_val   = nullptr;
        const ce::Value*                   out_val  = nullptr;
        crd::u32                           n_assign = 0;
        const ceg::TensorPipelinePlan      plan     = build_two_class_plan(ctx, *m, &root, mrows, d0, d1, d2, d3, x_val, w0_val,
                                                                           w1_val, w2_val, w3_val, out_val, &n_assign);
        REQUIRE(plan.stages.size() == 5U);
        float x_in[mrows * d0];
        float w0_in[d0 * d0];
        float w1_in[d0 * d1];
        float w2_in[d1 * d2];
        float w3_in[d2 * d3];
        float oracle[mrows * d3] = {};
        crd::tests::fill_two_class_sandwich(mrows, d0, d1, d2, d3, x_in, w0_in, w1_in, w2_in, w3_in, oracle);
        const MlpSeed seeds[5] = {{x_val, x_in, mrows * d0}, {w0_val, w0_in, d0 * d0}, {w1_val, w1_in, d0 * d1},
                                  {w2_val, w2_in, d1 * d2},  {w3_val, w3_in, d2 * d3}};
        double        fb[3] = {};
        double        gr[3] = {};
        double        fg  = 0.0;
        double        gg  = 0.0;
        crd::u32      nc  = 0;
        bool          gok = false;
        bool          bx  = false;
        crd::usize    clo = 0;
        crd::usize    chi = 0;
        REQUIRE(bench_captured(*cudactx, ctx, &root, plan, seeds, 5U, out_val, oracle, mrows * d3, fb, gr, &fg, &gg, &nc, &gok,
                               &bx, &clo, &chi));
        REQUIRE(gok);
        CHECK(nc == 3U); // the graph captured ONLY the claimed run (3 stages), NOT the flanking gemms (total 5)
        CHECK(bx);
        CHECK(fg > 0.0);
        CHECK(gg > 0.0);
        CHECK(clo == 1U);
        CHECK(chi == 4U);
        INFO("ceir29z fixture=sandwich5 cap=[" << clo << "," << chi << ") node_count=" << nc << " gpu_ms{fb=" << fg << ",gr=" << gg
                                               << ",ratio=" << (gg > 0.0 ? fg / gg : 0.0) << "} cpu_us_per_iter K1{fb=" << fb[0]
                                               << ",gr=" << gr[0] << ",x=" << (gr[0] > 0.0 ? fb[0] / gr[0] : 0.0)
                                               << "} K10{fb=" << fb[1] << ",gr=" << gr[1] << ",x=" << (gr[1] > 0.0 ? fb[1] / gr[1] : 0.0)
                                               << "} K100{fb=" << fb[2] << ",gr=" << gr[2] << ",x=" << (gr[2] > 0.0 ? fb[2] / gr[2] : 0.0) << "}");
        CHECK(true); // anchor the INFO row
    }
}

// CEIR-30z-1 — the BAND-CLOSE BENCH: the sharded-reduction TRANSFER-COST decomposition, the census-predicted HONEST toy-dim loss
// made QUANTITATIVE. Board -> docs/bench/2026-09-05-ceir30-sharded-reduction-transfer-cost.md (written at measurement time). Env-gated
// (CRD_CEIR_BENCH) so the default sweep does not time. Run: CRD_CEIR_BENCH=1 ctest -V -R "30z" (or the exe with -s to see the rows).
// ⛔ NO all-Gpu arm (reduce-on-CUDA is ledgered — the reduce can't run on CUDA this band), NO 2-GPU arm (single-GPU hardware).
TEST_CASE("ceir 30z: sharded-reduction transfer-cost bench (all-Host vs [Host,Host,Gpu] vs the transfer floor; the honest toy-dim tax)",
          "[ceir][ml][gpu][cuda][dist][bench]")
{
    namespace gpu = crd::gpu;
    if (std::getenv("CRD_CEIR_BENCH") == nullptr)
    {
        WARN("CRD_CEIR_BENCH unset; skipping the CEIR-30z sharded-reduction bench (set CRD_CEIR_BENCH=1 to time)");
        return;
    }
    crd::memory::TlsfAllocator devalloc(64U << 20U);
    auto                       cudactx = gpu::create_cuda_compute_context(devalloc);
    REQUIRE(cudactx != nullptr);
    if (!cudactx->valid()) { WARN("no CUDA device available; skipping the CEIR-30z bench"); return; }

    // the dim sweep: [8,4] the proof fixture, then growing rows×cols. The partials that CROSS the domain are [cols] (the reduce
    // outputs), so the transfer floor is ~constant in rows while the Host reduce is O(rows×cols) — the tax AMORTIZES as rows grow.
    const crd::u32 dims[4][2] = {{8U, 4U}, {64U, 32U}, {512U, 128U}, {4096U, 512U}};
    for (const auto& d : dims)
    {
        crd::memory::GrowableTlsfAllocator root;
        double                             cpu_a = 0.0;
        double                             cpu_b = 0.0;
        double                             cpu_c = 0.0;
        double                             gpu_b = 0.0;
        bool                               bx = false;
        crd::u32                           nx = 0;
        REQUIRE(bench_one_dim(*cudactx, &root, d[0], d[1], &cpu_a, &cpu_b, &cpu_c, &gpu_b, &bx, &nx));
        CHECK(bx);          // bit-exact vs the two-stage oracle after 10 back-to-back executes (no aliased-storage clobber)
        CHECK(nx == 3U);    // 2 partial uploads + 1 readback — the planned crossings, per execute
        CHECK(gpu_b > 0.0); // the Gpu combine actually launched real work (the 28b median==0 hard-fail)
        INFO("ceir30z dim=[" << d[0] << "," << d[1] << "] cpu_us{A_host=" << cpu_a << ",B_sharded=" << cpu_b << ",C_xferfloor="
                             << cpu_c << "} tax_B_minus_A=" << (cpu_b - cpu_a) << " combine_gpu_us=" << (gpu_b * 1000.0)
                             << " B_over_A=" << (cpu_a > 0.0 ? cpu_b / cpu_a : 0.0));
        CHECK(true); // anchor the INFO row (prints under catch2 -s / ctest -V)
    }
}
