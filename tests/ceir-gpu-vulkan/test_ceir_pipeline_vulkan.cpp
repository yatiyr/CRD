// CEIR-22c-2 (Vulkan) — the §137 GEMM→FFT→reduction PROOF: an authored-in-C++ ceir module (linalg.gemm → tensor.reshape →
// tensor.fft → tensor.reduce) is PLANNED (plan_tensor_pipeline → the device-resident buffer graph) and EXECUTED
// (execute_tensor_pipeline) as ONE SUBMIT on a REAL Vulkan device — NO CPU round-trip between stages (GpuOnly intermediates,
// the reshape a zero-copy ALIAS). The chain's final scalar is validated against an INDEPENDENT composed reference
// (triple-loop GEMM → naive DFT → serial sum, all f64) within a DERIVED, DECLARED tolerance. ⛔ the graph tier (gemm/reduce)
// + the kernel tier (fft) BOTH record into the ONE recorder — the tier split dissolves at GlslKernel→ComputePipeline. The
// device-free plan is proven in tests/ceir-gpu/test_tensor_pipeline.cpp; here the plan DRIVES a real chained GPU run.

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>    // CEIR-22c-3d: register_arith_ops (the parsed dispatch grid consts)
#include <crd/ceir/gen/compute_ops.hpp>  // CEIR-22c-3d: register_compute_ops (the parsed viz dispatches)
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/parse.hpp>            // CEIR-22c-3d: parse the authored .ceir asset
#include <crd/ceir/gpu/ckir_synth.hpp>
#include <crd/ceir/gpu/coopvec_mlp.hpp> // CEIR-24c-2b: coopvec_config_from_mlp / coopvec_weights_from_mlp (the CLAIM conversion)
#include <crd/ceir/gpu/expand_ml.hpp> // CEIR-24b-4: expand_ml_ops (the ml.attention/ml.mlp -> 22/23 vocab rewrite)
#include <crd/ceir/gpu/partition_ml.hpp> // CEIR-24c-2b: coopvec_can_claim_mlp (the CLAIM check before native dispatch)
#include <crd/ceir/gpu/tensor_pipeline.hpp>
#include <crd/ceir/gpu/tensor_pipeline_exec.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/linalg.hpp>
#include <crd/ceir/ml.hpp> // CEIR-24b-4: ml.attention/ml.mlp + register_dialect
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

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>  // CEIR-22c-3g: caller-stamped submit wall-time (the executor stays backend-free / untimed)
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
    std::unique_ptr<crd::gpu::ComputePipeline>  pipes[8]; // design B has 5 dispatched stages (gemm/fft/mag/reduce/normalize)
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
bool load_emit_ckir(const char* path, kir::KGraph& g, kir::GlslKernel& kern, crd::memory::IAllocator* alloc)
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

    if (st.kind == ceg::StageKind::Gemm)
    {
        const ceg::GraphSynth s = ceg::synth_gemm(c, *st.op, g);
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
    else // VizDispatch — the AUTHORED viz .ckir, loaded BY KERNEL SYMBOL (the mixed high-level-tensor + CKIR stage). ckir_read →
    {    // emit_compute_kernel_glsl; the grid is the asset's OWN arith.const operands (the asset-drives-it rule, not numel).
        const ce::AttrValue kv   = c.attr_value(st.op->attr(crd::containers::StringView("kernel")));
        const char*         path = nullptr;
        if (kv.s == crd::containers::StringView("viz_magnitude")) { path = CRD_REPO_DIR "/assets/ckir/tensor_viz_magnitude.ckir"; }
        else if (kv.s == crd::containers::StringView("viz_normalize")) { path = CRD_REPO_DIR "/assets/ckir/tensor_viz_normalize.ckir"; }
        else if (kv.s == crd::containers::StringView("relu")) { path = CRD_REPO_DIR "/assets/ckir/relu.ckir"; } // CEIR-23c MLP activation
        else if (kv.s == crd::containers::StringView("transpose")) { path = CRD_REPO_DIR "/assets/ckir/transpose.ckir"; } // CEIR-24b attention Q·Kᵀ
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
        if (!kir::emit_compute_kernel_glsl(g, ve, res.alloc, kern)) { return rs; }
        rs.gx    = const_grid(c, st.op->operand(0U));
        rs.gy    = const_grid(c, st.op->operand(1U));
        rs.gz    = const_grid(c, st.op->operand(2U));
        pushsize = 0U;
        nbind    = static_cast<int>(st.nbind); // 3 (re,im,mag) / (mag,max,norm)
    }

    const auto spv = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                     "ceir_pipe", res.alloc);
    if (!spv.ok || res.n >= 8) { return rs; }
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

// Materialize a planned QUANT module's buffers, execute it on `compute`, and read back the buffer realizing `out_val` into `dst`
// (dst_len floats). ExternalIns are seeded by IDENTITY from `seeds` (a LIST — a multi-input MLP feeds x, W1, s1, W2, s2, not one
// trio). Returns false only on a device / materialize failure.
bool run_quant_module(crd::gpu::VulkanComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* root,
                      const ceg::TensorPipelinePlan& plan, const QuantSeed* seeds, crd::usize n_seeds, const ce::Value* out_val,
                      float* dst, crd::usize dst_len)
{
    const crd::usize nb = plan.buffers.size();
    if (nb > 32U) { return false; }
    std::unique_ptr<crd::gpu::ComputeBuffer> owned[32];
    crd::gpu::ComputeBuffer*                 bufs[32] = {};
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.role == ceg::BufferRole::Alias) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        crd::gpu::ComputeMemory mem = crd::gpu::ComputeMemory::GpuOnly;
        if (pb.role == ceg::BufferRole::ExternalIn) { mem = crd::gpu::ComputeMemory::CpuToGpu; }
        else if (pb.role == ceg::BufferRole::Output) { mem = crd::gpu::ComputeMemory::GpuToCpu; }
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes; // round the 1-byte int8 zp up (raw-view alignment)
        owned[i]          = compute.create_buffer(sz, crd::gpu::compute_usage::storage, mem);
        if (owned[i] == nullptr) { return false; }
        bufs[i] = owned[i].get();
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
    crd::i32 out_idx = -1;
    for (crd::usize i = 0; i < nb; ++i) { if (plan.buffers[i].value == out_val) { out_idx = static_cast<crd::i32>(i); } }
    if (out_idx < 0) { return false; }

    Resolver res;
    res.alloc_ctx           = &ctx;
    res.alloc               = root;
    res.compute             = &compute;
    auto&                   rec = compute.begin();
    const ceg::ExecuteError ee  = ceg::execute_tensor_pipeline(plan, rec, &resolve_stage, &res,
                                                               crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(bufs, nb));
    if (ee != ceg::ExecuteError::None) { return false; }
    rec.barrier(*bufs[static_cast<crd::usize>(out_idx)], crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::HostRead);
    compute.submit_and_wait();
    const auto* got = static_cast<const float*>(owned[static_cast<crd::usize>(out_idx)]->map());
    if (got == nullptr) { return false; }
    for (crd::usize e = 0; e < dst_len; ++e) { dst[e] = got[e]; }
    owned[static_cast<crd::usize>(out_idx)]->unmap();
    return true;
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
// CEIR-22/23 primitive pipeline (transpose.ckir -> gemm(Q,Kt) -> softmax.ckir -> gemm(.,V)) runs DEVICE-RESIDENT on a real Vulkan
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
