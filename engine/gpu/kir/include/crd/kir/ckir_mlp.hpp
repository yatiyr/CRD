#pragma once

// ckir_mlp.hpp — Phase 3.1.6 v17: the CKIR **fused-MLP tensor recipe** (the NRC moat, ported into CKIR). A fully-fused
// multi-layer perceptron whose activations NEVER leave the chip — the tiny-cuda-nn / Neural Radiance Cache technique that a
// per-call GEMM library (cuBLAS) structurally cannot express (fusion across GEMM calls is off its menu). Standalone gold
// references measured a DOUBLE CRUSH on an RTX 4070 Ti SUPER — forward 2.37×, backward 1.90× — vs cuBLAS at its best
// split-K algorithm (bench/gpu-compute/mlp_fused_bench.cu + mlp_backward_bench.cu; board 2026-07-14-fused-mlp-cublas-gold.md).
//
// Like the coopmat2 GEMM tensor tier (emit_contract_coopmat2_glsl), this is a MONOLITHIC per-backend recipe emitter keyed on
// an MlpConfig — cooperative-matrix semantics (wmma / coopmat / WaveMatrix / simdgroup_matrix) do not map onto CKIR's
// per-invocation statement tier, so the tensor kernels are emitted as whole-kernel templates rather than built from KStmts.
// ONE config feeds every backend + one shared CPU reference oracle proves the math. HONEST: the tensor path is fp16 with a
// hardware-defined accumulation order ⇒ portable + per-backend deterministic (fixed schedule, no atomics on the forward) +
// matched-accuracy, but NOT bit-exact across vendors (tensor cores forbid that — the FP32-precise tier owns bit-exactness).
//
// Emitters produce pure String source (no GPU dependency); the KIR test emits the .cu, the bench driver compiles it with
// nvcc and duels vs cuBLAS to prove the CKIR-authored kernel reproduces the crush. ADR-0098.

#include <crd/kir/ckir_glsl.hpp> // GlslKernel carrier + glsl_detail::app_uint

namespace crd::kir
{

// A fully-fused MLP: `layers` weight matrices of `width`×`width`, each hidden layer ReLU, the final layer linear.
// Activations ping-pong in shared through all layers; one thread-block processes `batch_tile` samples. fp16 in/out, fp16
// accumulate (matches the vendor pipeline's math exactly). Constraints: width % 16 == 0 (wmma tile), batch_tile % warps == 0,
// (batch_tile / warps) % 16 == 0. The 64-wide / 6-layer / tile-128 / 2-warp config is the measured 2.37× crush.
struct MlpConfig
{
    int width      = 64;  // W — MLP width (must be a multiple of 16)
    int layers      = 6;  // number of weight matrices (hidden layers are ReLU, the last is linear)
    int batch_tile = 128; // rows per block
    int warps      = 2;   // warps per block; each warp owns (batch_tile / warps) rows

    [[nodiscard]] int rows_per_warp() const noexcept { return batch_tile / warps; }
    [[nodiscard]] int row_frags() const noexcept { return rows_per_warp() / 16; } // 16-row wmma fragments per warp
    [[nodiscard]] int col_frags() const noexcept { return width / 16; }           // 16-col wmma fragments across width
    [[nodiscard]] int k_blocks() const noexcept { return width / 16; }            // contraction over width
    [[nodiscard]] bool valid() const noexcept
    {
        return width % 16 == 0 && warps > 0 && batch_tile % warps == 0 && rows_per_warp() % 16 == 0 && layers >= 1;
    }
};

namespace mlp_detail
{
inline void u(crd::containers::String& s, int v) { glsl_detail::app_uint(s, v); }
} // namespace mlp_detail

// Emit the FUSED-MLP FORWARD kernel as CUDA (wmma fp16). Reproduces the gold-ref schedule exactly: input tile loaded once,
// weights staged in shared per layer, `row_frags × col_frags` independent wmma accumulator chains per warp (the register-ILP
// lever = the crush), ReLU folded in-register on the accumulators, ping-pong shared store. Signature:
// `NAME(const __half* in, const __half* w, __half* out, int nrows)`. Deterministic (fixed schedule, no atomics).
inline bool emit_fused_mlp_fwd_cuda(const MlpConfig& cfg, const char* name, GlslKernel& out)
{
    if (!cfg.valid()) { return false; }
    using mlp_detail::u;
    const int wd  = cfg.width;
    const int nl  = cfg.layers;
    const int tl  = cfg.batch_tile;
    const int nw  = cfg.warps;
    const int rpw = cfg.rows_per_warp();
    const int rf  = cfg.row_frags();
    const int cf  = cfg.col_frags();
    const int kb  = cfg.k_blocks();

    crd::containers::String& s = out.source;
    s.clear();
    // signature
    s.append("extern \"C\" __global__ void __launch_bounds__(");
    u(s, nw * 32);
    s.append(") ");
    s.append(name);
    s.append("(const __half* __restrict__ in, const __half* __restrict__ w, __half* __restrict__ out, int nrows)\n{\n");
    // shared: ping/pong activations + this layer's weights
    s.append("  __shared__ __half sa[");
    u(s, tl * wd);
    s.append("];\n  __shared__ __half sb[");
    u(s, tl * wd);
    s.append("];\n  __shared__ __half sw[");
    u(s, wd * wd);
    s.append("];\n");
    s.append("  const int block_row = blockIdx.x * ");
    u(s, tl);
    s.append(";\n  const int tid = threadIdx.x;\n  const int warp = tid / 32;\n");
    // load input tile (float4-coalesced)
    s.append("  { const __half* src = in + (size_t)block_row * ");
    u(s, wd);
    s.append(";\n    for (int i = tid; i < ");
    u(s, tl * wd / 8);
    s.append("; i += ");
    u(s, nw * 32);
    s.append(") reinterpret_cast<float4*>(sa)[i] = reinterpret_cast<const float4*>(src)[i]; }\n");
    s.append("  __half* cur = sa; __half* nxt = sb;\n");
    s.append("  for (int layer = 0; layer < ");
    u(s, nl);
    s.append("; ++layer) {\n");
    // stage weights
    s.append("    const __half* wl = w + (size_t)layer * ");
    u(s, wd * wd);
    s.append(";\n    for (int i = tid; i < ");
    u(s, wd * wd / 8);
    s.append("; i += ");
    u(s, nw * 32);
    s.append(") reinterpret_cast<float4*>(sw)[i] = reinterpret_cast<const float4*>(wl)[i];\n");
    s.append("    __syncthreads();\n");
    // accumulators
    s.append("    nvcuda::wmma::fragment<nvcuda::wmma::accumulator, 16, 16, 16, __half> acc[");
    u(s, rf);
    s.append("][");
    u(s, cf);
    s.append("];\n");
    s.append("    for (int m = 0; m < ");
    u(s, rf);
    s.append("; ++m) for (int n = 0; n < ");
    u(s, cf);
    s.append("; ++n) nvcuda::wmma::fill_fragment(acc[m][n], __float2half(0.0f));\n");
    // contraction
    s.append("    for (int k = 0; k < ");
    u(s, kb);
    s.append("; ++k) {\n");
    s.append("      nvcuda::wmma::fragment<nvcuda::wmma::matrix_a, 16, 16, 16, __half, nvcuda::wmma::row_major> af[");
    u(s, rf);
    s.append("];\n      for (int m = 0; m < ");
    u(s, rf);
    s.append("; ++m) nvcuda::wmma::load_matrix_sync(af[m], cur + (warp * ");
    u(s, rpw);
    s.append(" + m * 16) * ");
    u(s, wd);
    s.append(" + k * 16, ");
    u(s, wd);
    s.append(");\n      for (int n = 0; n < ");
    u(s, cf);
    s.append("; ++n) {\n");
    s.append("        nvcuda::wmma::fragment<nvcuda::wmma::matrix_b, 16, 16, 16, __half, nvcuda::wmma::row_major> bf;\n");
    s.append("        nvcuda::wmma::load_matrix_sync(bf, sw + (k * 16) * ");
    u(s, wd);
    s.append(" + n * 16, ");
    u(s, wd);
    s.append(");\n        for (int m = 0; m < ");
    u(s, rf);
    s.append("; ++m) nvcuda::wmma::mma_sync(acc[m][n], af[m], bf, acc[m][n]);\n      }\n    }\n");
    // ReLU on hidden layers (in-register on the accumulator fragments)
    s.append("    if (layer + 1 < ");
    u(s, nl);
    s.append(") for (int m = 0; m < ");
    u(s, rf);
    s.append("; ++m) for (int n = 0; n < ");
    u(s, cf);
    s.append("; ++n) for (int e = 0; e < acc[m][n].num_elements; ++e)\n");
    s.append("      acc[m][n].x[e] = __hgt(acc[m][n].x[e], __float2half(0.0f)) ? acc[m][n].x[e] : __float2half(0.0f);\n");
    // ping-pong store into the OTHER buffer
    s.append("    for (int m = 0; m < ");
    u(s, rf);
    s.append("; ++m) for (int n = 0; n < ");
    u(s, cf);
    s.append("; ++n) nvcuda::wmma::store_matrix_sync(nxt + (warp * ");
    u(s, rpw);
    s.append(" + m * 16) * ");
    u(s, wd);
    s.append(" + n * 16, acc[m][n], ");
    u(s, wd);
    s.append(", nvcuda::wmma::mem_row_major);\n");
    s.append("    __half* t = cur; cur = nxt; nxt = t;\n    __syncthreads();\n  }\n");
    // write output tile
    s.append("  { __half* dst = out + (size_t)block_row * ");
    u(s, wd);
    s.append(";\n    for (int i = tid; i < ");
    u(s, tl * wd / 8);
    s.append("; i += ");
    u(s, nw * 32);
    s.append(") reinterpret_cast<float4*>(dst)[i] = reinterpret_cast<const float4*>(cur)[i]; }\n");
    s.append("  (void)nrows;\n}\n");
    out.n_inputs = 3; // in, w, out
    return true;
}

// CPU REFERENCE ORACLE (forward): the plain fp32 GEMM chain the fused kernel computes — the shared reference every backend
// matches within fp16 tolerance (tensor cores' hardware accumulation order forbids bit-exactness; run-to-run determinism is
// verified per-backend). `in` is [batch × width] row-major, `w` is [layers × width × width] ROW-MAJOR so that
// out[n] = sum_k a[k]·w[l][k*width + n] — the convention the emitted wmma kernel loads B with (verified vs cuBLAS: the
// column-major reading is 95× wrong). `out` is [batch × width]. Deterministic ascending accumulation (no FMA).
inline void mlp_forward_ref(const MlpConfig& cfg, const float* in, const float* w, float* scratch_a, float* scratch_b,
                            float* outp, int batch)
{
    const int wd = cfg.width;
    const int nl = cfg.layers;
    for (int r = 0; r < batch; ++r)
    {
        for (int c = 0; c < wd; ++c) { scratch_a[c] = in[r * wd + c]; }
        float* cur = scratch_a;
        float* nxt = scratch_b;
        for (int l = 0; l < nl; ++l)
        {
            const float* wl = w + static_cast<crd::usize>(l) * static_cast<crd::usize>(wd) * static_cast<crd::usize>(wd);
            for (int n = 0; n < wd; ++n)
            {
                float acc = 0.0F;
                for (int k = 0; k < wd; ++k) { acc = acc + cur[k] * wl[k * wd + n]; } // z[n] = sum_k a[k]·w[l][k*wd + n] (row-major B)
                nxt[n] = (l + 1 < nl && acc < 0.0F) ? 0.0F : acc;                      // ReLU on hidden, linear on last
            }
            float* t = cur;
            cur      = nxt;
            nxt      = t;
        }
        for (int c = 0; c < wd; ++c) { outp[r * wd + c] = cur[c]; }
    }
}

// Build the FUSED-MLP FORWARD as a CKIR STATEMENT-TIER graph — the PORTABLE + BIT-EXACT tier (the mission's strongest moat:
// FP32 `precise`, no FMA ⇒ every generic backend emitter — GLSL/HLSL/MSL/WGSL/CUDA — lowers it and it bit-matches the CPU
// oracle AND every other backend, which the tensor tier cannot). One workgroup = one sample, one thread = one output feature
// (local_size = width). Activations ping-pong in SHARED across all layers (the fusion — input read once, output written once,
// weights from global/L2); the dot product is an ascending `acc = acc + a·w` fold (fixed order, no atomics ⇒ deterministic).
// Buffers: 0=in (F32 batch×W), 1=w (F32 layers×W×W row-major w[l*W*W + k*W + n]), 2=out (F32 batch×W). Grid = batch workgroups.
[[nodiscard]] inline KEntry build_mlp_fwd_fp32(KGraph& g, const MlpConfig& cfg)
{
    const int   wd  = cfg.width;
    const int   nl  = cfg.layers;
    const Shape sh1 = make_shape({1});
    const auto  ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int in_buf  = g.buffer_decl(DType::F32, 0, 0, false);
    const int w_buf   = g.buffer_decl(DType::F32, 0, 1, false);
    const int out_buf = g.buffer_decl(DType::F32, 0, 2, true);
    const int sa      = g.shared_decl(DType::F32, wd); // activations (ping)
    const int sb      = g.shared_decl(DType::F32, wd); // activations (pong)
    const int tid     = g.builtin(KBuiltin::LocalInvocationIndex); // 0..W-1 = this thread's output feature n
    const int wid     = g.builtin(KBuiltin::WorkgroupIndex);       // = the sample index
    const int base    = mul(wid, ku(static_cast<crd::u32>(wd)));
    const int fzero   = g.constant(0.0, sh1, DType::F32);

    const int mark = g.kernel_stmt_mark();

    g.stmt_shared_store(sa, tid, g.buffer_load(in_buf, add(base, tid))); // load input tile (one feature/thread)
    g.stmt_barrier();

    int cur = sa;
    int nxt = sb;
    for (int layer = 0; layer < nl; ++layer)
    {
        int acc = fzero; // z[n] = Σ_k cur[k]·w[layer][k*W + n]  (ascending fold, precise ⇒ bit-exact)
        for (int k = 0; k < wd; ++k)
        {
            const int a  = g.shared_load(cur, ku(static_cast<crd::u32>(k)));
            const int wv = g.buffer_load(w_buf, add(ku(static_cast<crd::u32>(layer * wd * wd + k * wd)), tid));
            acc          = add(acc, mul(a, wv));
        }
        const int act = (layer + 1 < nl) ? g.binary(KOp::Max, acc, fzero) : acc; // ReLU on hidden, linear on last
        g.stmt_shared_store(nxt, tid, act);
        g.stmt_barrier();
        const int t = cur;
        cur         = nxt;
        nxt         = t;
    }
    g.stmt_buffer_store(out_buf, add(base, tid), g.shared_load(cur, tid));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(wd);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── BACKWARD (the training half), statement-tier, DETERMINISTIC ─────────────────────────────────────────────────────────
// Kernel A: the per-sample activation-gradient chain. One workgroup = one sample, one thread = one feature. Given the forward
// activations a[0..L] and the loss gradient gout = dL/da[L], walk layers backward computing dz[l] (the pre-activation
// gradient) ON-CHIP (the fusion), writing each dz[l][r] to dz_all for kernel B. da[l] = dz[l]·Wᵀ becomes the next layer's g.
// Buffers: 0=a_all (F32 (L+1)×batch×W), 1=w (F32 L×W×W), 2=gout (F32 batch×W), 3=dz_all (F32 L×batch×W, out). Grid = batch.
[[nodiscard]] inline KEntry build_mlp_bwd_dz(KGraph& g, const MlpConfig& cfg, int batch)
{
    const int   wd  = cfg.width;
    const int   nl  = cfg.layers;
    const Shape sh1 = make_shape({1});
    const auto  ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int a_buf  = g.buffer_decl(DType::F32, 0, 0, false);
    const int w_buf  = g.buffer_decl(DType::F32, 0, 1, false);
    const int go_buf = g.buffer_decl(DType::F32, 0, 2, false);
    const int dz_buf = g.buffer_decl(DType::F32, 0, 3, true);
    const int ga     = g.shared_decl(DType::F32, wd); // gradient (ping)
    const int gb     = g.shared_decl(DType::F32, wd); // gradient (pong)
    const int sdz    = g.shared_decl(DType::F32, wd); // this layer's dz (shared for the daᵀ contraction)
    const int tid    = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid    = g.builtin(KBuiltin::WorkgroupIndex);
    const int rbase  = mul(wid, ku(static_cast<crd::u32>(wd)));
    const int fzero  = g.constant(0.0, sh1, DType::F32);
    const int bw     = batch * wd;

    const int mark = g.kernel_stmt_mark();
    g.stmt_shared_store(ga, tid, g.buffer_load(go_buf, add(rbase, tid))); // g = dL/da[L]
    g.stmt_barrier();

    int cur = ga;
    int nxt = gb;
    for (int layer = nl - 1; layer >= 0; --layer)
    {
        const int gv = g.shared_load(cur, tid);
        int       dz = gv; // output layer is linear: act' = 1
        if (layer + 1 < nl)                                                                  // hidden: dz = g ⊙ (a[l+1] > 0)
        {
            const int aout = g.buffer_load(a_buf, add(ku(static_cast<crd::u32>((layer + 1) * bw)), add(rbase, tid)));
            dz             = g.select(g.binary(KOp::CmpGt, aout, fzero), gv, fzero);
        }
        g.stmt_shared_store(sdz, tid, dz);
        g.stmt_buffer_store(dz_buf, add(ku(static_cast<crd::u32>(layer * bw)), add(rbase, tid)), dz); // → kernel B
        g.stmt_barrier();
        if (layer > 0) // da[k=tid] = Σ_n dz[n]·w[layer][tid*W + n]  → next g (layer 0's da = dL/dinput, unused)
        {
            int acc = fzero;
            for (int n = 0; n < wd; ++n)
            {
                const int dzn = g.shared_load(sdz, ku(static_cast<crd::u32>(n)));
                const int wv  = g.buffer_load(w_buf, add(ku(static_cast<crd::u32>(layer * wd * wd + n)), mul(tid, ku(static_cast<crd::u32>(wd)))));
                acc           = add(acc, mul(dzn, wv));
            }
            g.stmt_shared_store(nxt, tid, acc);
            g.stmt_barrier();
            const int t = cur;
            cur         = nxt;
            nxt         = t;
        }
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(wd);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Kernel B: the DETERMINISTIC weight-gradient reduction — dW[l][k][n] = Σ_r a[l][r][k]·dz[l][r][n], the sum taken in
// ASCENDING sample order (fixed order, NO atomics ⇒ bit-exact + run-to-run identical, the moat the standalone's fp32-atomic
// dW could not hold). One workgroup per (l,k) [WorkgroupIndex = l·W + k], one thread per output column n. Grid = L·W.
// Buffers: 0=a_all (F32 (L+1)×batch×W), 1=dz_all (F32 L×batch×W), 2=dW (F32 L×W×W, out).
[[nodiscard]] inline KEntry build_mlp_bwd_dw(KGraph& g, const MlpConfig& cfg, int batch)
{
    const int   wd  = cfg.width;
    const Shape sh1 = make_shape({1});
    const auto  ku  = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), sh1, DType::U32); };
    const auto  add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto  mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };

    const int a_buf  = g.buffer_decl(DType::F32, 0, 0, false);
    const int dz_buf = g.buffer_decl(DType::F32, 0, 1, false);
    const int dw_buf = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid    = g.builtin(KBuiltin::LocalInvocationIndex); // n (output column)
    const int wid    = g.builtin(KBuiltin::WorkgroupIndex);       // l*W + k
    const int fzero  = g.constant(0.0, sh1, DType::F32);
    const int lyr    = g.binary(KOp::Div, wid, ku(static_cast<crd::u32>(wd)));
    const int kk     = g.binary(KOp::Sub, wid, mul(lyr, ku(static_cast<crd::u32>(wd))));
    const int lbw    = mul(lyr, ku(static_cast<crd::u32>(batch * wd))); // l · batch · W
    const int abase  = add(lbw, kk);                                    // a[l][0][k]
    const int dbase  = add(lbw, tid);                                   // dz[l][0][n]

    const int mark = g.kernel_stmt_mark();
    int       acc  = fzero;
    for (int r = 0; r < batch; ++r) // ASCENDING sample order ⇒ deterministic bit-exact reduction
    {
        const int a_lrk  = g.buffer_load(a_buf, add(abase, ku(static_cast<crd::u32>(r * wd))));
        const int dz_lrn = g.buffer_load(dz_buf, add(dbase, ku(static_cast<crd::u32>(r * wd))));
        acc              = add(acc, mul(a_lrk, dz_lrn));
    }
    // dW[l][k][n] = l·W·W + k·W + n
    g.stmt_buffer_store(dw_buf, add(add(mul(lyr, ku(static_cast<crd::u32>(wd * wd))), mul(kk, ku(static_cast<crd::u32>(wd)))), tid), acc);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(wd);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// CPU REFERENCE ORACLE (backward): the deterministic reference kernels A+B match. `a_all` is [(L+1)×batch×W] (the forward
// activations, a[0]=input), `w` [L×W×W row-major], `gout` [batch×W] = dL/da[L]. Fills `dz_all` [L×batch×W] and `dW` [L×W×W].
// Ascending-order f32 folds (no FMA) — the same order the kernels use ⇒ bit-exact.
inline void mlp_backward_ref(const MlpConfig& cfg, const float* a_all, const float* w, const float* gout, int batch,
                             float* dz_all, float* dw, float* g_scratch, float* ng_scratch)
{
    const int wd = cfg.width;
    const int nl = cfg.layers;
    const int bw = batch * wd;
    for (int r = 0; r < batch; ++r)
    {
        for (int n = 0; n < wd; ++n) { g_scratch[n] = gout[r * wd + n]; }
        for (int layer = nl - 1; layer >= 0; --layer)
        {
            for (int n = 0; n < wd; ++n)
            {
                float dz = g_scratch[n];
                if (layer + 1 < nl) { dz = (a_all[(layer + 1) * bw + r * wd + n] > 0.0F) ? g_scratch[n] : 0.0F; }
                dz_all[layer * bw + r * wd + n] = dz;
            }
            if (layer > 0)
            {
                for (int k = 0; k < wd; ++k)
                {
                    float acc = 0.0F;
                    for (int n = 0; n < wd; ++n) { acc = acc + dz_all[layer * bw + r * wd + n] * w[layer * wd * wd + k * wd + n]; }
                    ng_scratch[k] = acc;
                }
                for (int k = 0; k < wd; ++k) { g_scratch[k] = ng_scratch[k]; }
            }
        }
    }
    for (int layer = 0; layer < nl; ++layer)
    {
        for (int k = 0; k < wd; ++k)
        {
            for (int n = 0; n < wd; ++n)
            {
                float acc = 0.0F;
                for (int r = 0; r < batch; ++r) { acc = acc + a_all[layer * bw + r * wd + k] * dz_all[layer * bw + r * wd + n]; }
                dw[layer * wd * wd + k * wd + n] = acc;
            }
        }
    }
}

// Emit the FUSED-MLP FORWARD kernel as GLSL using VK_NV_cooperative_matrix2 (workgroup-scoped cooperative matrices — the
// portable tensor path, the driver owns the tile schedule). Same fusion as CUDA: one workgroup owns `batch_tile` samples,
// activations ping in SHARED across all layers (never touch global between layers), the per-layer matmul runs on the tensor
// units. Cooperative matrices load/store the shared activation buffer; the accumulator (fp32) is stored to a shared scratch,
// then invocations apply ReLU (hidden) / linear (last) and repack fp16 for the next layer. Bindings: 0=In(fp16 batch×W),
// 1=W(fp16 layers×W×W row-major), 2=Out(fp16 batch×W). Dims baked as literals ⇒ the driver specializes. Deterministic
// (fixed schedule, no atomics). Requires cfg where 6·batch_tile·width ≤ maxComputeSharedMemorySize (48 KB ⇒ tile ≤ 128@W64).
inline bool emit_fused_mlp_fwd_glsl(const MlpConfig& cfg, GlslKernel& out)
{
    if (!cfg.valid()) { return false; }
    using mlp_detail::u;
    const int wd  = cfg.width;
    const int nl  = cfg.layers;
    const int tl  = cfg.batch_tile;
    const int wg  = 128;      // workgroup threads (4 subgroups) — drives the shared copy loops, not the coopmat scope
    const int tw  = tl * wd;  // tile elements

    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("#extension GL_KHR_cooperative_matrix : require\n");
    s.append("#extension GL_NV_cooperative_matrix2 : require\n");
    s.append("#extension GL_KHR_memory_scope_semantics : require\n");
    s.append("#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n");
    s.append("layout(local_size_x = ");
    u(s, wg);
    s.append(") in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BIn { float16_t Inb[]; };\n");
    s.append("layout(std430, binding = 1) readonly buffer BW { float16_t Wb[]; };\n");
    s.append("layout(std430, binding = 2) writeonly buffer BOut { float16_t Outb[]; };\n");
    s.append("shared float16_t sact[");
    u(s, tw);
    s.append("];\n");
    s.append("shared float stmp[");
    u(s, tw);
    s.append("];\n");
    s.append("void main() {\n");
    s.append("  uint br = gl_WorkGroupID.x * ");
    u(s, tl);
    s.append("u;\n  uint lid = gl_LocalInvocationID.x;\n");
    // load the input tile into shared
    s.append("  for (uint i = lid; i < ");
    u(s, tw);
    s.append("u; i += ");
    u(s, wg);
    s.append("u) sact[i] = Inb[br * ");
    u(s, wd);
    s.append("u + i];\n  barrier();\n");
    // layer loop
    s.append("  for (uint layer = 0u; layer < ");
    u(s, nl);
    s.append("u; ++layer) {\n");
    s.append("    coopmat<float16_t, gl_ScopeWorkgroup, ");
    u(s, tl);
    s.append(", ");
    u(s, wd);
    s.append(", gl_MatrixUseA> a;\n");
    s.append("    coopMatLoad(a, sact, 0, ");
    u(s, wd);
    s.append(", gl_CooperativeMatrixLayoutRowMajor);\n");
    s.append("    coopmat<float16_t, gl_ScopeWorkgroup, ");
    u(s, wd);
    s.append(", ");
    u(s, wd);
    s.append(", gl_MatrixUseB> b;\n");
    s.append("    coopMatLoad(b, Wb, layer * ");
    u(s, wd * wd);
    s.append("u, ");
    u(s, wd);
    s.append(", gl_CooperativeMatrixLayoutRowMajor);\n");
    s.append("    coopmat<float, gl_ScopeWorkgroup, ");
    u(s, tl);
    s.append(", ");
    u(s, wd);
    s.append(", gl_MatrixUseAccumulator> acc = coopmat<float, gl_ScopeWorkgroup, ");
    u(s, tl);
    s.append(", ");
    u(s, wd);
    s.append(", gl_MatrixUseAccumulator>(0.0);\n");
    s.append("    acc = coopMatMulAdd(a, b, acc);\n");
    s.append("    coopMatStore(acc, stmp, 0, ");
    u(s, wd);
    s.append(", gl_CooperativeMatrixLayoutRowMajor);\n");
    s.append("    barrier();\n");
    // ReLU (hidden) / linear (last) + repack fp16 for the next layer
    s.append("    if (layer + 1u < ");
    u(s, nl);
    s.append("u) { for (uint i = lid; i < ");
    u(s, tw);
    s.append("u; i += ");
    u(s, wg);
    s.append("u) sact[i] = float16_t(max(stmp[i], 0.0)); }\n");
    s.append("    else { for (uint i = lid; i < ");
    u(s, tw);
    s.append("u; i += ");
    u(s, wg);
    s.append("u) sact[i] = float16_t(stmp[i]); }\n");
    s.append("    barrier();\n  }\n");
    // write the output tile
    s.append("  for (uint i = lid; i < ");
    u(s, tw);
    s.append("u; i += ");
    u(s, wg);
    s.append("u) Outb[br * ");
    u(s, wd);
    s.append("u + i] = sact[i];\n}\n");
    out.n_inputs = 3;
    return true;
}

// Emit the FUSED-MLP BACKWARD as CUDA (wmma fp16) — the tensor-tier training crush, ported into CKIR (mirrors the forward
// tensor recipe; the standalone gold ref = 1.90× vs cuBLAS at its best split-K algo). Emits TWO kernels: `reduce_dw` (sums
// the NGROUP dW partials) and `<name>` = the fused backward (per-sample dz mask + on-chip da wmma chain + dW = aᵀ·dz wmma
// reduced over the batch via per-warp fragments + fp32 atomic into NGROUP partials). Dims baked as literals (per-shape).
// Signatures: reduce_dw(const float* partial, float* dw); <name>(const __half* acts, const __half* w, const __half* gout,
// float* dw_partial). ⚠ fp32-atomic dW ⇒ PER-BACKEND deterministic-ish (atomic order), NOT bit-exact — the crush tier; the
// FP32 statement-tier backward (build_mlp_bwd_dw) is the bit-exact/deterministic companion. Baked BATCH ⇒ pass it here.
inline bool emit_fused_mlp_bwd_cuda(const MlpConfig& cfg, int batch, int ngroup, const char* name, GlslKernel& out)
{
    if (!cfg.valid()) { return false; }
    const int wd  = cfg.width;
    const int nl  = cfg.layers;
    const int tl  = cfg.batch_tile;
    const int nw  = cfg.warps;
    const int rpw = tl / nw;   // rows per warp (da)
    const int cpw = wd / nw;   // dW cols per warp
    if (cpw % 16 != 0 || rpw % 16 != 0 || tl % 16 != 0) { return false; }
    const int dw_cf = cpw / 16; // dW col fragments per warp
    const int dw_rf = wd / 16;  // dW row fragments (feature-in k)
    const int dw_kb = tl / 16;  // dW contraction blocks (batch)
    const int da_rf = rpw / 16; // da row fragments
    const int da_cf = wd / 16;  // da col fragments
    const int da_kb = wd / 16;  // da contraction blocks (feature)
    const int ldw   = nl * wd * wd;
    using mlp_detail::u;

    crd::containers::String& s = out.source;
    s.clear();
    // reduce_dw: sum the ngroup partials → dw
    s.append("extern \"C\" __global__ void reduce_dw(const float* __restrict__ partial, float* __restrict__ dw)\n{\n");
    s.append("  const int i = blockIdx.x * blockDim.x + threadIdx.x;\n  if (i < ");
    u(s, ldw);
    s.append(") {\n    float acc = 0.0f;\n    for (int gp = 0; gp < ");
    u(s, ngroup);
    s.append("; ++gp) acc += partial[(size_t)gp * ");
    u(s, ldw);
    s.append(" + i];\n    dw[i] = acc;\n  }\n}\n\n");

    // the fused backward kernel
    s.append("extern \"C\" __global__ void __launch_bounds__(");
    u(s, nw * 32);
    s.append(") ");
    s.append(name);
    s.append("(const __half* __restrict__ acts, const __half* __restrict__ w, const __half* __restrict__ gout, float* __restrict__ dw)\n{\n");
    s.append("  __shared__ __half sg_a[");
    u(s, tl * wd);
    s.append("];\n  __shared__ __half sg_b[");
    u(s, tl * wd);
    s.append("];\n  __shared__ __half sw[");
    u(s, wd * wd);
    s.append("];\n  __shared__ float sdw[");
    u(s, nw);
    s.append("][256];\n");
    s.append("  const int block_row = blockIdx.x * ");
    u(s, tl);
    s.append(";\n  const int tid = threadIdx.x;\n  const int warp = tid / 32;\n");
    s.append("  { const __half* src = gout + (size_t)block_row * ");
    u(s, wd);
    s.append(";\n    for (int i = tid; i < ");
    u(s, tl * wd / 8);
    s.append("; i += ");
    u(s, nw * 32);
    s.append(") reinterpret_cast<float4*>(sg_a)[i] = reinterpret_cast<const float4*>(src)[i]; }\n");
    s.append("  __half* cur = sg_a; __half* nxt = sg_b;\n  __syncthreads();\n");
    s.append("  for (int layer = ");
    u(s, nl - 1);
    s.append("; layer >= 0; --layer) {\n");
    s.append("    const __half* wl = w + (size_t)layer * ");
    u(s, wd * wd);
    s.append(";\n    const __half* aIn = acts + ((size_t)layer * ");
    u(s, batch);
    s.append(" + block_row) * ");
    u(s, wd);
    s.append(";\n    const __half* aOut = acts + ((size_t)(layer + 1) * ");
    u(s, batch);
    s.append(" + block_row) * ");
    u(s, wd);
    s.append(";\n    for (int i = tid; i < ");
    u(s, wd * wd / 8);
    s.append("; i += ");
    u(s, nw * 32);
    s.append(") reinterpret_cast<float4*>(sw)[i] = reinterpret_cast<const float4*>(wl)[i];\n    __syncthreads();\n");
    // dz mask (hidden layers)
    s.append("    if (layer + 1 < ");
    u(s, nl);
    s.append(") { for (int i = tid; i < ");
    u(s, tl * wd);
    s.append("; i += ");
    u(s, nw * 32);
    s.append(") cur[i] = __hgt(aOut[i], __float2half(0.0f)) ? cur[i] : __float2half(0.0f); __syncthreads(); }\n");
    // dW = a^T . dz  (fp32 accumulate, per-warp fragments → atomic into partials)
    s.append("    {\n      nvcuda::wmma::fragment<nvcuda::wmma::accumulator, 16, 16, 16, float> dacc[");
    u(s, dw_cf);
    s.append("][");
    u(s, dw_rf);
    s.append("];\n      for (int cb = 0; cb < ");
    u(s, dw_cf);
    s.append("; ++cb) for (int rb = 0; rb < ");
    u(s, dw_rf);
    s.append("; ++rb) nvcuda::wmma::fill_fragment(dacc[cb][rb], 0.0f);\n      for (int kb = 0; kb < ");
    u(s, dw_kb);
    s.append("; ++kb) {\n        nvcuda::wmma::fragment<nvcuda::wmma::matrix_a, 16, 16, 16, __half, nvcuda::wmma::col_major> af[");
    u(s, dw_rf);
    s.append("];\n        for (int rb = 0; rb < ");
    u(s, dw_rf);
    s.append("; ++rb) nvcuda::wmma::load_matrix_sync(af[rb], aIn + (kb * 16) * ");
    u(s, wd);
    s.append(" + rb * 16, ");
    u(s, wd);
    s.append(");\n        for (int cb = 0; cb < ");
    u(s, dw_cf);
    s.append("; ++cb) {\n          nvcuda::wmma::fragment<nvcuda::wmma::matrix_b, 16, 16, 16, __half, nvcuda::wmma::row_major> bf;\n          nvcuda::wmma::load_matrix_sync(bf, cur + (kb * 16) * ");
    u(s, wd);
    s.append(" + (warp * ");
    u(s, cpw);
    s.append(" + cb * 16), ");
    u(s, wd);
    s.append(");\n          for (int rb = 0; rb < ");
    u(s, dw_rf);
    s.append("; ++rb) nvcuda::wmma::mma_sync(dacc[cb][rb], af[rb], bf, dacc[cb][rb]);\n        }\n      }\n");
    s.append("      float* dwl = dw + ((size_t)(blockIdx.x % ");
    u(s, ngroup);
    s.append(") * ");
    u(s, nl);
    s.append(" + layer) * ");
    u(s, wd * wd);
    s.append(";\n      for (int cb = 0; cb < ");
    u(s, dw_cf);
    s.append("; ++cb) for (int rb = 0; rb < ");
    u(s, dw_rf);
    s.append("; ++rb) {\n        nvcuda::wmma::store_matrix_sync(sdw[warp], dacc[cb][rb], 16, nvcuda::wmma::mem_row_major);\n        __syncwarp();\n        for (int e = (tid & 31); e < 256; e += 32) {\n          const int kk = rb * 16 + e / 16;\n          const int nn = warp * ");
    u(s, cpw);
    s.append(" + cb * 16 + e % 16;\n          atomicAdd(&dwl[kk + ");
    u(s, wd);
    s.append(" * nn], sdw[warp][e]);\n        }\n        __syncwarp();\n      }\n    }\n    __syncthreads();\n");
    // da = dz . W^T -> new g
    s.append("    {\n      nvcuda::wmma::fragment<nvcuda::wmma::accumulator, 16, 16, 16, __half> acc[");
    u(s, da_rf);
    s.append("][");
    u(s, da_cf);
    s.append("];\n      for (int m = 0; m < ");
    u(s, da_rf);
    s.append("; ++m) for (int n = 0; n < ");
    u(s, da_cf);
    s.append("; ++n) nvcuda::wmma::fill_fragment(acc[m][n], __float2half(0.0f));\n      for (int kb = 0; kb < ");
    u(s, da_kb);
    s.append("; ++kb) {\n        nvcuda::wmma::fragment<nvcuda::wmma::matrix_a, 16, 16, 16, __half, nvcuda::wmma::row_major> af[");
    u(s, da_rf);
    s.append("];\n        for (int m = 0; m < ");
    u(s, da_rf);
    s.append("; ++m) nvcuda::wmma::load_matrix_sync(af[m], cur + (warp * ");
    u(s, rpw);
    s.append(" + m * 16) * ");
    u(s, wd);
    s.append(" + kb * 16, ");
    u(s, wd);
    s.append(");\n        for (int n = 0; n < ");
    u(s, da_cf);
    s.append("; ++n) {\n          nvcuda::wmma::fragment<nvcuda::wmma::matrix_b, 16, 16, 16, __half, nvcuda::wmma::row_major> bf;\n          nvcuda::wmma::load_matrix_sync(bf, sw + (kb * 16) * ");
    u(s, wd);
    s.append(" + n * 16, ");
    u(s, wd);
    s.append(");\n          for (int m = 0; m < ");
    u(s, da_rf);
    s.append("; ++m) nvcuda::wmma::mma_sync(acc[m][n], af[m], bf, acc[m][n]);\n        }\n      }\n      for (int m = 0; m < ");
    u(s, da_rf);
    s.append("; ++m) for (int n = 0; n < ");
    u(s, da_cf);
    s.append("; ++n) nvcuda::wmma::store_matrix_sync(nxt + (warp * ");
    u(s, rpw);
    s.append(" + m * 16) * ");
    u(s, wd);
    s.append(" + n * 16, acc[m][n], ");
    u(s, wd);
    s.append(", nvcuda::wmma::mem_row_major);\n    }\n    __half* t = cur; cur = nxt; nxt = t;\n    __syncthreads();\n  }\n}\n");
    out.n_inputs = 4;
    return true;
}

} // namespace crd::kir
