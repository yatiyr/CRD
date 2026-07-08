#pragma once

// ckir_hlsl.hpp — Phase 3.1.6 v17-d: the CKIR **HLSL emitter** (the DirectX 12 backend's code generator). Same three
// single-kernel shapes as the GLSL/CUDA emitters — fused-elementwise / matmul / reduce — emitted as HLSL compute
// shaders (Shader Model 6.0, dxc → DXIL). All buffers are `RWStructuredBuffer<float>` (UAVs u0..uN) for a uniform root
// signature; dims arrive as root constants in `cbuffer PC : register(b0)`. `precise float` temps ⇒ no mad-fusion
// (bit-matches the CPU reference for correctly-rounded ops; f32 division is a fast reciprocal on the GPU = ULP, like
// Vulkan). Pure String production; the backend compiles + dispatches. Reuses the shared helpers + GlslKernel. ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_glsl.hpp> // GlslKernel + glsl_detail::{app_uint,app_flit,is_fusable}

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>

namespace crd::kir
{

// Fused-elementwise HLSL kernel.
inline bool emit_elementwise_hlsl(const KGraph& g, int output, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    const int                       n = g.size();
    crd::containers::Array<crd::u8> reach(scratch);
    crd::containers::Array<int>     stk(scratch);
    reach.resize(static_cast<crd::usize>(n), 0);
    stk.push_back(output);
    while (stk.size() > 0)
    {
        const int i = stk[stk.size() - 1];
        stk.resize(stk.size() - 1);
        if (reach[static_cast<crd::usize>(i)]) { continue; }
        reach[static_cast<crd::usize>(i)] = 1;
        const KNode& nd = g.node(i);
        if (!is_fusable(nd.op)) { return false; }
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
    }
    crd::containers::Array<int> binding_of(scratch);
    binding_of.resize(static_cast<crd::usize>(n), -1);
    out.n_inputs = 0;
    for (int i = 0; i < n; ++i)
    {
        if (reach[static_cast<crd::usize>(i)] && g.node(i).op == KOp::Input)
        {
            binding_of[static_cast<crd::usize>(i)] = out.n_inputs;
            out.input_iidx[out.n_inputs]           = g.node(i).iidx;
            ++out.n_inputs;
        }
    }

    crd::containers::String& s = out.source;
    s.clear();
    for (int b = 0; b < out.n_inputs; ++b) { s.append("RWStructuredBuffer<float> in"); app_uint(s, b); s.append(" : register(u"); app_uint(s, b); s.append(");\n"); }
    s.append("RWStructuredBuffer<float> outb : register(u"); app_uint(s, out.n_inputs); s.append(");\n");
    s.append("cbuffer PC : register(b0) { uint n; };\n");
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 dtid : SV_DispatchThreadID) {\n  uint gid = dtid.x;\n  if (gid >= n) return;\n");
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        s.append("  precise float t"); app_uint(s, i); s.append(" = ");
        const auto ta = [&](int id) { s.append("t"); app_uint(s, id); };
        switch (nd.op)
        {
        case KOp::Input: s.append("in"); app_uint(s, binding_of[static_cast<crd::usize>(i)]); s.append("[gid]"); break;
        case KOp::Const: app_flit(s, nd.cval); break;
        case KOp::Neg: s.append("-"); ta(nd.a); break;
        case KOp::Recip: s.append("1.0/"); ta(nd.a); break;
        case KOp::Abs: s.append("abs("); ta(nd.a); s.append(")"); break;
        case KOp::Exp: s.append("exp("); ta(nd.a); s.append(")"); break;
        case KOp::Log: s.append("log("); ta(nd.a); s.append(")"); break;
        case KOp::Sin: s.append("sin("); ta(nd.a); s.append(")"); break;
        case KOp::Cos: s.append("cos("); ta(nd.a); s.append(")"); break;
        case KOp::Sqrt: s.append("sqrt("); ta(nd.a); s.append(")"); break;
        case KOp::Tanh: s.append("tanh("); ta(nd.a); s.append(")"); break;
        case KOp::Floor: s.append("floor("); ta(nd.a); s.append(")"); break;
        case KOp::Ceil: s.append("ceil("); ta(nd.a); s.append(")"); break;
        case KOp::Trunc: s.append("trunc("); ta(nd.a); s.append(")"); break;
        case KOp::Round: s.append("round("); ta(nd.a); s.append(")"); break;
        case KOp::Sign: s.append("(("); ta(nd.a); s.append(" > 0.0) ? 1.0 : (("); ta(nd.a); s.append(" < 0.0) ? -1.0 : 0.0))"); break;
        case KOp::Add: ta(nd.a); s.append(" + "); ta(nd.b); break;
        case KOp::Sub: ta(nd.a); s.append(" - "); ta(nd.b); break;
        case KOp::Mul: ta(nd.a); s.append(" * "); ta(nd.b); break;
        case KOp::Div: ta(nd.a); s.append(" / "); ta(nd.b); break;
        case KOp::Max: s.append("max("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Min: s.append("min("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::CmpLt: s.append("(("); ta(nd.a); s.append(" < "); ta(nd.b); s.append(") ? 1.0 : 0.0)"); break;
        case KOp::CmpEq: s.append("(("); ta(nd.a); s.append(" == "); ta(nd.b); s.append(") ? 1.0 : 0.0)"); break;
        case KOp::CmpLe: s.append("(("); ta(nd.a); s.append(" <= "); ta(nd.b); s.append(") ? 1.0 : 0.0)"); break;
        case KOp::Select: s.append("(("); ta(nd.c); s.append(" != 0.0) ? "); ta(nd.a); s.append(" : "); ta(nd.b); s.append(")"); break;
        default: return false;
        }
        s.append(";\n");
    }
    s.append("  outb[gid] = t"); app_uint(s, output); s.append(";\n}\n");
    return true;
}

// Batched-matmul HLSL kernel (root constants M,K,N,nbatch).
inline bool emit_contract_hlsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& c = g.node(output);
    if (c.op != KOp::Contract || g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("RWStructuredBuffer<float> A : register(u0);\nRWStructuredBuffer<float> Bm : register(u1);\nRWStructuredBuffer<float> C : register(u2);\n");
    s.append("cbuffer PC : register(b0) { uint M; uint K; uint N; uint nbatch; };\n");
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 dtid : SV_DispatchThreadID) {\n  uint gid = dtid.x;\n  uint mn = M * N; uint total = mn * nbatch;\n  if (gid >= total) return;\n");
    s.append("  uint b = gid / mn; uint rem = gid % mn; uint m = rem / N; uint nn = rem % N;\n");
    s.append("  uint aoff = b * M * K + m * K; uint boff = b * K * N + nn;\n");
    s.append("  precise float acc = 0.0;\n  for (uint k = 0; k < K; ++k) { precise float prod = A[aoff + k] * Bm[boff + k * N]; acc = acc + prod; }\n");
    s.append("  C[b * mn + m * N + nn] = acc;\n}\n");
    return true;
}

// Reduce HLSL kernel (trailing-contiguous, sequential order; root constants nout, redsize).
inline bool emit_reduce_hlsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& rn = g.node(output);
    if (!is_reduce(rn.op)) { return false; }
    if (g.node(rn.a).op != KOp::Input) { return false; }
    const Shape& ish = g.node(rn.a).shape;
    int          t   = 0;
    for (int k = ish.rank - 1; k >= 0; --k) { if (((rn.axes >> k) & 1U) != 0U) { ++t; } else { break; } }
    if (t == 0) { return false; }
    crd::u32 tmask = 0;
    for (int k = ish.rank - t; k < ish.rank; ++k) { tmask |= (1U << k); }
    if (rn.axes != tmask) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(rn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("RWStructuredBuffer<float> A : register(u0);\nRWStructuredBuffer<float> O : register(u1);\n");
    s.append("cbuffer PC : register(b0) { uint nout; uint redsize; };\n");
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 dtid : SV_DispatchThreadID) {\n  uint o = dtid.x;\n  if (o >= nout) return;\n  uint base = o * redsize;\n");
    if (rn.op == KOp::ReduceSum) { s.append("  precise float acc = 0.0;\n  for (uint r = 0; r < redsize; ++r) { acc = acc + A[base + r]; }\n"); }
    else if (rn.op == KOp::ReduceProd) { s.append("  precise float acc = 1.0;\n  for (uint r = 0; r < redsize; ++r) { acc = acc * A[base + r]; }\n"); }
    else if (rn.op == KOp::ReduceMax) { s.append("  precise float acc = A[base];\n  for (uint r = 1; r < redsize; ++r) { acc = max(acc, A[base + r]); }\n"); }
    else if (rn.op == KOp::ReduceMin) { s.append("  precise float acc = A[base];\n  for (uint r = 1; r < redsize; ++r) { acc = min(acc, A[base + r]); }\n"); }
    else if (rn.op == KOp::ArgMax) { s.append("  float bv = A[base]; uint bi = 0;\n  for (uint r = 1; r < redsize; ++r) { if (A[base + r] > bv) { bv = A[base + r]; bi = r; } }\n  precise float acc = (float)bi;\n"); }
    else { s.append("  float bv = A[base]; uint bi = 0;\n  for (uint r = 1; r < redsize; ++r) { if (A[base + r] < bv) { bv = A[base + r]; bi = r; } }\n  precise float acc = (float)bi;\n"); }
    s.append("  O[o] = acc;\n}\n");
    return true;
}

// T2 FAST parallel reduce (Sum/Prod/Max/Min) HLSL — one group per output, grid-stride partials + groupshared tree.
inline bool emit_reduce_fast_hlsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& rn = g.node(output);
    if (rn.tier != DetTier::Fast || !is_fast_reduceable(rn.op) || g.node(rn.a).op != KOp::Input) { return false; }
    const Shape& ish = g.node(rn.a).shape;
    int          t   = 0;
    for (int k = ish.rank - 1; k >= 0; --k) { if (((rn.axes >> k) & 1U) != 0U) { ++t; } else { break; } }
    if (t == 0) { return false; }
    crd::u32 tmask = 0;
    for (int k = ish.rank - t; k < ish.rank; ++k) { tmask |= (1U << k); }
    if (rn.axes != tmask) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(rn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("RWStructuredBuffer<float> A : register(u0);\nRWStructuredBuffer<float> O : register(u1);\n");
    s.append("cbuffer PC : register(b0) { uint nout; uint redsize; };\n");
    s.append("groupshared float sdata[256];\n");
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 gid : SV_GroupID, uint3 tid3 : SV_GroupThreadID) {\n");
    s.append("  uint o = gid.x; uint tid = tid3.x; uint base = o * redsize;\n");
    s.append("  float acc = "); s.append(glsl_detail::fast_init(rn.op, false));
    s.append(";\n  for (uint i = tid; i < redsize; i += 256u) { acc = ");
    glsl_detail::fast_comb(s, rn.op, "acc", "A[base + i]", "max", "min");
    s.append("; }\n  sdata[tid] = acc;\n  GroupMemoryBarrierWithGroupSync();\n");
    s.append("  for (uint sh = 128u; sh > 0u; sh >>= 1) { if (tid < sh) { sdata[tid] = ");
    glsl_detail::fast_comb(s, rn.op, "sdata[tid]", "sdata[tid + sh]", "max", "min");
    s.append("; } GroupMemoryBarrierWithGroupSync(); }\n");
    s.append("  if (tid == 0u) { O[o] = sdata[0]; }\n}\n");
    return true;
}

// Gather HLSL kernel — out[m, ...] = data[idx[m], ...] (row-gather along axis 0). idx holds f32-encoded integers.
inline bool emit_gather_hlsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& gn = g.node(output);
    if (gn.op != KOp::Gather || g.node(gn.a).op != KOp::Input || g.node(gn.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(gn.a).iidx; // data
    out.input_iidx[1] = g.node(gn.b).iidx; // idx
    crd::containers::String& s = out.source;
    s.clear();
    s.append("RWStructuredBuffer<float> A : register(u0);\nRWStructuredBuffer<float> I : register(u1);\nRWStructuredBuffer<float> O : register(u2);\n");
    s.append("cbuffer PC : register(b0) { uint nout; uint rowsize; };\n");
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 dtid : SV_DispatchThreadID) {\n  uint o = dtid.x;\n  if (o >= nout) return;\n");
    s.append("  uint m = o / rowsize; uint c = o % rowsize;\n");
    s.append("  uint r = (uint)(int)I[m];\n");
    s.append("  O[o] = A[r * rowsize + c];\n}\n");
    return true;
}

// Scatter HLSL kernel — out=base, then out[idx[m],...]=updates[m,...] (LAST-WINS, output-centric ⇒ race-free).
inline bool emit_scatter_hlsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::Scatter || g.node(sn.a).op != KOp::Input || g.node(sn.b).op != KOp::Input || g.node(sn.c).op != KOp::Input) { return false; }
    out.n_inputs      = 3;
    out.input_iidx[0] = g.node(sn.a).iidx; // base
    out.input_iidx[1] = g.node(sn.b).iidx; // idx
    out.input_iidx[2] = g.node(sn.c).iidx; // updates
    crd::containers::String& s = out.source;
    s.clear();
    s.append("RWStructuredBuffer<float> B : register(u0);\nRWStructuredBuffer<float> I : register(u1);\nRWStructuredBuffer<float> U : register(u2);\nRWStructuredBuffer<float> O : register(u3);\n");
    s.append("cbuffer PC : register(b0) { uint nout; uint rowsize; uint M; };\n");
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 dtid : SV_DispatchThreadID) {\n  uint o = dtid.x;\n  if (o >= nout) return;\n");
    s.append("  uint r = o / rowsize; uint c = o % rowsize;\n");
    s.append("  float result = B[o];\n");
    s.append("  for (uint m = 0; m < M; ++m) { if ((uint)(int)I[m] == r) { result = U[m * rowsize + c]; } }\n");
    s.append("  O[o] = result;\n}\n");
    return true;
}

// Inclusive SCAN (prefix-sum) HLSL kernel along the trailing axis — one thread per row, sequential `precise` ⇒ bit-exact.
inline bool emit_scan_hlsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::ScanSum || g.node(sn.a).op != KOp::Input) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(sn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("RWStructuredBuffer<float> A : register(u0);\nRWStructuredBuffer<float> O : register(u1);\n");
    s.append("cbuffer PC : register(b0) { uint nrows; uint scanlen; };\n");
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 dtid : SV_DispatchThreadID) {\n  uint row = dtid.x;\n  if (row >= nrows) return;\n");
    s.append("  uint base = row * scanlen;\n  precise float acc = 0.0;\n");
    s.append("  for (uint c = 0; c < scanlen; ++c) { acc = acc + A[base + c]; O[base + c] = acc; }\n}\n");
    return true;
}

// T2 FAST parallel prefix-sum HLSL (one group per row): per-thread chunk scan + serial chunk-total scan + prefix add.
inline bool emit_scan_fast_hlsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::ScanSum || sn.tier != DetTier::Fast || g.node(sn.a).op != KOp::Input) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(sn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("RWStructuredBuffer<float> A : register(u0);\nRWStructuredBuffer<float> O : register(u1);\n");
    s.append("cbuffer PC : register(b0) { uint nrows; uint scanlen; };\n");
    s.append("groupshared float ctot[256];\n");
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 gid : SV_GroupID, uint3 tid3 : SV_GroupThreadID) {\n");
    s.append("  uint row = gid.x; uint tid = tid3.x; uint base = row * scanlen;\n");
    s.append("  uint C = (scanlen + 255u) / 256u; uint lo = tid * C; uint hi = min(lo + C, scanlen);\n");
    s.append("  float acc = 0.0;\n  for (uint i = lo; i < hi; ++i) { acc = acc + A[base + i]; O[base + i] = acc; }\n");
    s.append("  ctot[tid] = acc;\n  GroupMemoryBarrierWithGroupSync();\n");
    s.append("  if (tid == 0u) { float run = 0.0; for (uint t = 0u; t < 256u; ++t) { float v = ctot[t]; ctot[t] = run; run = run + v; } }\n");
    s.append("  GroupMemoryBarrierWithGroupSync();\n  float prefix = ctot[tid];\n");
    s.append("  for (uint i = lo; i < hi; ++i) { O[base + i] = O[base + i] + prefix; }\n}\n");
    return true;
}

// FUSED GEMM+epilogue HLSL kernel: naive `precise` matmul + the epilogue applied in the store (`epi(acc, bias[nn])`) —
// DX12 inherits the fusion crush. Reuses the shared C-like `emit_epi_clike`. All buffers UAV; root constants M,N,K.
inline bool emit_contract_fused_hlsl(const KGraph& g, int output, int contract, const FuseInfo& fi, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    const KNode& c = g.node(contract);
    if (c.op != KOp::Contract || g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2 + fi.n_bias;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;
    for (int j = 0; j < fi.n_bias; ++j) { out.input_iidx[2 + j] = fi.bias_iidx[j]; }
    crd::containers::String& s = out.source;
    s.clear();
    s.append("RWStructuredBuffer<float> A : register(u0);\nRWStructuredBuffer<float> Bm : register(u1);\n");
    for (int j = 0; j < fi.n_bias; ++j) { s.append("RWStructuredBuffer<float> bias"); glsl_detail::app_uint(s, j); s.append(" : register(u"); glsl_detail::app_uint(s, 2 + j); s.append(");\n"); }
    s.append("RWStructuredBuffer<float> C : register(u"); glsl_detail::app_uint(s, 2 + fi.n_bias); s.append(");\n");
    s.append("cbuffer PC : register(b0) { uint M; uint N; uint K; };\n");
    emit_epi_clike(g, output, fi, scratch, s);
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 dtid : SV_DispatchThreadID) {\n  uint gid = dtid.x;\n  if (gid >= M * N) { return; }\n");
    s.append("  uint m = gid / N; uint nn = gid % N;\n  precise float acc = 0.0;\n  for (uint k = 0; k < K; ++k) { precise float prod = A[m * K + k] * Bm[k * N + nn]; acc = acc + prod; }\n");
    s.append("  C[m * N + nn] = epi(acc");
    for (int j = 0; j < fi.n_bias; ++j) { s.append(", bias"); glsl_detail::app_uint(s, j); s.append("[nn]"); }
    s.append(");\n}\n");
    return true;
}

} // namespace crd::kir
