// CEIR-22c-3e (DX12) — the DirectX-12 leg of the §137 pipeline proof. The SAME authored assets/ceir/tensor_pipeline.ceir is
// PARSE-LOADED, planned, and executed as ONE device-resident submit on a REAL D3D12 device — gemm+reduce (graph tier,
// emit_contract_hlsl/emit_reduce_hlsl) + fft + the two authored viz CKIR dispatches (kernel tier, emit_compute_kernel_hlsl) into
// ONE recorder, the intermediates device-resident (the migrated-executor-runs-both-backends rule). ⛔ the PORTABLE readback
// contract (DX12 forbids a UAV on an upload/readback heap): each logical buffer = a GpuOnly `dev` UAV + a CpuToGpu `up` staging
// buffer (copy → dev) + a GpuToCpu `rb` staging buffer (dev → copy); "shader writes a host-visible buffer then map()" is
// Vulkan-only and does NOT port. The terminal [64] normalized spectrum is validated vs an INDEPENDENT composed f64 reference.

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/ckir_synth.hpp>
#include <crd/ceir/gpu/expand_ml.hpp> // CEIR-24b-4: expand_ml_ops (the ml.attention/ml.mlp -> 22/23 vocab rewrite)
#include <crd/ceir/gpu/tensor_pipeline.hpp>
#include <crd/ceir/gpu/tensor_pipeline_exec.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/linalg.hpp>
#include <crd/ceir/ml.hpp> // CEIR-24b-4: ml.attention/ml.mlp + register_dialect
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

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

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
    std::unique_ptr<crd::gpu::ComputePipeline> pipes[8];
    int                                        n = 0;
};

// CEIR-23b-2d: load an authored .ckir compute kernel BY PATH and emit its HLSL (the fused QuantGemm + symmetric Dequant kernels
// are hand-authored, ckir_read like VizDispatch — never ckir_synth'd). Returns false on read/emit failure.
bool load_emit_ckir_hlsl(const char* path, kir::KGraph& g, kir::GlslKernel& kern, crd::memory::IAllocator* alloc)
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

    if (st.kind == ceg::StageKind::Gemm)
    {
        const ceg::GraphSynth s = ceg::synth_gemm(c, *st.op, g);
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
    else // VizDispatch — the authored viz .ckir, loaded by kernel symbol (emit HLSL)
    {
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
        if (!kir::emit_compute_kernel_hlsl(g, ve, res.alloc, kern)) { return rs; }
        rs.gx    = const_grid(c, st.op->operand(0U));
        rs.gy    = const_grid(c, st.op->operand(1U));
        rs.gz    = const_grid(c, st.op->operand(2U));
        pushsize = 0U;
        nbind    = static_cast<int>(st.nbind);
    }

    if (res.n >= 8) { return rs; }
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

// Materialize a planned QUANT module's buffers on DX12 (the PORTABLE dev/up/rb pattern), execute it, and read back the buffer
// realizing `out_val` into `dst` (dst_len floats). ExternalIns matching a seed (by Value) are STAGED+uploaded (wq as u32-packed
// int8 BITS, not float); an ExternalIn with NO seed (the int8 zp, the β=0 accumulator C — bound by no stage) gets a GpuOnly dev
// buffer but no upload. `seeds` is a LIST (a multi-input MLP feeds x, W1, s1, W2, s2). Returns false on a device/materialize failure.
bool run_quant_module(crd::gpu::Dx12ComputeContext& compute, ce::Context& ctx, crd::memory::IAllocator* alloc,
                      const ceg::TensorPipelinePlan& plan, const QuantSeed* seeds, crd::usize n_seeds, const ce::Value* out_val,
                      float* dst, crd::usize dst_len)
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
    crd::i32                          out_idx = -1;
    for (crd::usize i = 0; i < nb; ++i)
    {
        const ceg::PlanBuffer& pb = plan.buffers[i];
        if (pb.value == out_val) { out_idx = static_cast<crd::i32>(i); }
        if (pb.role == ceg::BufferRole::Alias) { bufs[i] = bufs[static_cast<crd::usize>(pb.alias_of)]; continue; }
        const crd::u64 sz = pb.bytes < 16ULL ? 16ULL : pb.bytes; // round the 1-byte int8 zp up (DX12 raw-view alignment)
        dev[i]            = compute.create_buffer(sz, storage | transfer_dst | transfer_src, g::ComputeMemory::GpuOnly);
        if (dev[i] == nullptr) { return false; }
        bufs[i]               = dev[i].get();
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
    if (out_idx < 0) { return false; }
    auto rb = compute.create_buffer(plan.buffers[static_cast<crd::usize>(out_idx)].bytes, transfer_dst, g::ComputeMemory::GpuToCpu);
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
    for (crd::usize e = 0; e < dst_len; ++e) { dst[e] = got[e]; }
    rb->unmap();
    return true;
}
} // namespace

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
// transpose.ckir -> gemm(Q,Kt) -> softmax.ckir -> gemm(.,V) runs DEVICE-RESIDENT on a real D3D12 device (HLSL/DXIL) and matches
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
