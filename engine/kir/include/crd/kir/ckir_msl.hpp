#pragma once

// ckir_msl.hpp — Phase 3.1.6 v17-d: the CKIR **Metal Shading Language emitter** (the Metal backend's code generator).
// Same three single-kernel shapes as the other emitters, as MSL compute kernels. `device` storage buffers bound by
// index; dims in a `constant` struct. Determinism: the Metal backend compiles with math-mode SAFE (fast-math OFF) ⇒ no
// FMA fusion ⇒ bit-matches the `-ffp-contract=off` CPU reference (like Vulkan/CUDA/DX12; MSL has no per-op `precise`,
// so it's a compile flag). Pure String production (no Metal dep); the backend compiles + dispatches. Reuses the shared
// helpers + GlslKernel. Validated on real Apple silicon at Part C (GitHub Actions macOS). ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_glsl.hpp> // GlslKernel + glsl_detail::{app_uint,app_flit,is_fusable}

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>

namespace crd::kir
{

namespace msl_detail
{
inline void header(crd::containers::String& s)
{
    s.append("#include <metal_stdlib>\nusing namespace metal;\n");
}
} // namespace msl_detail

// Fused-elementwise MSL kernel (dims: pc.d0 = n).
inline bool emit_elementwise_msl(const KGraph& g, int output, crd::memory::IAllocator* scratch, GlslKernel& out)
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
    msl_detail::header(s);
    s.append("struct PC { uint d0; uint d1; uint d2; uint d3; };\n");
    s.append("kernel void ckir(\n");
    for (int b = 0; b < out.n_inputs; ++b) { s.append("  device const float* in"); app_uint(s, b); s.append(" [[buffer("); app_uint(s, b); s.append(")]],\n"); }
    s.append("  device float* outb [[buffer("); app_uint(s, out.n_inputs); s.append(")]],\n");
    s.append("  constant PC& pc [[buffer("); app_uint(s, out.n_inputs + 1); s.append(")]],\n");
    s.append("  uint gid [[thread_position_in_grid]]) {\n  if (gid >= pc.d0) return;\n");
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        s.append("  float t"); app_uint(s, i); s.append(" = ");
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
        case KOp::Round: s.append("rint("); ta(nd.a); s.append(")"); break;
        case KOp::Sign: s.append("(("); ta(nd.a); s.append(" > 0.0f) ? 1.0f : (("); ta(nd.a); s.append(" < 0.0f) ? -1.0f : 0.0f))"); break;
        case KOp::Add: ta(nd.a); s.append(" + "); ta(nd.b); break;
        case KOp::Sub: ta(nd.a); s.append(" - "); ta(nd.b); break;
        case KOp::Mul: ta(nd.a); s.append(" * "); ta(nd.b); break;
        case KOp::Div: ta(nd.a); s.append(" / "); ta(nd.b); break;
        case KOp::Max: s.append("max("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Min: s.append("min("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::CmpLt: s.append("select(0.0f, 1.0f, ("); ta(nd.a); s.append(" < "); ta(nd.b); s.append("))"); break;
        case KOp::CmpEq: s.append("select(0.0f, 1.0f, ("); ta(nd.a); s.append(" == "); ta(nd.b); s.append("))"); break;
        case KOp::CmpLe: s.append("select(0.0f, 1.0f, ("); ta(nd.a); s.append(" <= "); ta(nd.b); s.append("))"); break;
        case KOp::Select: s.append("select("); ta(nd.b); s.append(", "); ta(nd.a); s.append(", ("); ta(nd.c); s.append(" != 0.0f))"); break;
        default: return false;
        }
        s.append(";\n");
    }
    s.append("  outb[gid] = t"); app_uint(s, output); s.append(";\n}\n");
    return true;
}

// Batched-matmul MSL kernel (dims d0=M, d1=K, d2=N, d3=nbatch).
inline bool emit_contract_msl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& c = g.node(output);
    if (c.op != KOp::Contract || g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    msl_detail::header(s);
    s.append("struct PC { uint M; uint K; uint N; uint nbatch; };\n");
    s.append("kernel void ckir(device const float* A [[buffer(0)]], device const float* Bm [[buffer(1)]], device float* C [[buffer(2)]], constant PC& pc [[buffer(3)]], uint gid [[thread_position_in_grid]]) {\n");
    s.append("  uint mn = pc.M * pc.N; uint total = mn * pc.nbatch;\n  if (gid >= total) return;\n");
    s.append("  uint b = gid / mn; uint rem = gid % mn; uint m = rem / pc.N; uint nn = rem % pc.N;\n");
    s.append("  uint aoff = b * pc.M * pc.K + m * pc.K; uint boff = b * pc.K * pc.N + nn;\n");
    s.append("  float acc = 0.0f;\n  for (uint k = 0; k < pc.K; ++k) { float prod = A[aoff + k] * Bm[boff + k * pc.N]; acc = acc + prod; }\n");
    s.append("  C[b * mn + m * pc.N + nn] = acc;\n}\n");
    return true;
}

// T2 FAST GEMM (DetTier::Fast) MSL/Metal — the ported crush schedule (64×64 block, 4×4 microtile, TRANSPOSED-A
// threadgroup, `fma()`). Threadgroup-per-tile. Mirrors emit_contract_fast_glsl; Metal inherits the crush schedule.
inline bool emit_contract_fast_msl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& c = g.node(output);
    if (c.op != KOp::Contract || g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    msl_detail::header(s);
    s.append("struct PC { uint M; uint K; uint N; uint nbatch; };\n");
    s.append("kernel void ckir(device const float* A [[buffer(0)]], device const float* Bm [[buffer(1)]], device float* C [[buffer(2)]], constant PC& pc [[buffer(3)]], uint gid [[threadgroup_position_in_grid]], uint tid [[thread_position_in_threadgroup]]) {\n");
    s.append("  threadgroup float As[512];\n  threadgroup float Bs[512];\n"); // As TRANSPOSED [k][m]
    s.append("  uint N = pc.N; uint K = pc.K;\n");
    s.append("  uint nbc = N / 64u; uint bid = gid;\n");
    s.append("  uint blockRow = (bid / nbc) * 64u; uint blockCol = (bid % nbc) * 64u;\n");
    s.append("  uint tr = tid / 16u; uint tc = tid % 16u;\n");
    s.append("  uint arow = blockRow + tr * 4u; uint acol = blockCol + tc * 4u;\n");
    const char* d[4] = {"0", "1", "2", "3"};
    for (int i = 0; i < 4; ++i)
    {
        s.append("  float ");
        for (int j = 0; j < 4; ++j) { s.append("a"); s.append(d[i]); s.append(d[j]); s.append(" = 0.0f"); if (j < 3) { s.append(", "); } }
        s.append(";\n");
    }
    s.append("  for (uint k0 = 0u; k0 < K; k0 += 8u) {\n");
    s.append("    for (uint t = tid; t < 512u; t += 256u) { uint r = t / 8u; uint cc = t % 8u; As[cc * 64u + r] = A[(blockRow + r) * K + (k0 + cc)]; }\n");
    s.append("    for (uint t = tid; t < 512u; t += 256u) { uint r = t / 64u; uint cc = t % 64u; Bs[r * 64u + cc] = Bm[(k0 + r) * N + (blockCol + cc)]; }\n");
    s.append("    threadgroup_barrier(mem_flags::mem_threadgroup);\n");
    s.append("    for (uint kk = 0u; kk < 8u; ++kk) {\n");
    s.append("      float ar0 = As[kk*64u+tr*4u+0u], ar1 = As[kk*64u+tr*4u+1u], ar2 = As[kk*64u+tr*4u+2u], ar3 = As[kk*64u+tr*4u+3u];\n");
    s.append("      float br0 = Bs[kk*64u+tc*4u+0u], br1 = Bs[kk*64u+tc*4u+1u], br2 = Bs[kk*64u+tc*4u+2u], br3 = Bs[kk*64u+tc*4u+3u];\n");
    const char* ar[4] = {"ar0", "ar1", "ar2", "ar3"};
    const char* br[4] = {"br0", "br1", "br2", "br3"};
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            s.append("      a"); s.append(d[i]); s.append(d[j]); s.append(" = fma("); s.append(ar[i]); s.append(", "); s.append(br[j]); s.append(", a"); s.append(d[i]); s.append(d[j]); s.append(");\n");
        }
    }
    s.append("    }\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n  }\n");
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            s.append("  C[(arow + "); s.append(d[i]); s.append("u) * N + (acol + "); s.append(d[j]); s.append("u)] = a"); s.append(d[i]); s.append(d[j]); s.append(";\n");
        }
    }
    s.append("}\n");
    return true;
}

// Reduce MSL kernel (trailing-contiguous; dims d0=nout, d1=redsize).
inline bool emit_reduce_msl(const KGraph& g, int output, GlslKernel& out)
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
    msl_detail::header(s);
    s.append("struct PC { uint nout; uint redsize; };\n");
    s.append("kernel void ckir(device const float* A [[buffer(0)]], device float* O [[buffer(1)]], constant PC& pc [[buffer(2)]], uint o [[thread_position_in_grid]]) {\n");
    s.append("  if (o >= pc.nout) return;\n  uint base = o * pc.redsize;\n");
    if (rn.op == KOp::ReduceSum) { s.append("  float acc = 0.0f;\n  for (uint r = 0; r < pc.redsize; ++r) { acc = acc + A[base + r]; }\n"); }
    else if (rn.op == KOp::ReduceProd) { s.append("  float acc = 1.0f;\n  for (uint r = 0; r < pc.redsize; ++r) { acc = acc * A[base + r]; }\n"); }
    else if (rn.op == KOp::ReduceMax) { s.append("  float acc = A[base];\n  for (uint r = 1; r < pc.redsize; ++r) { acc = max(acc, A[base + r]); }\n"); }
    else if (rn.op == KOp::ReduceMin) { s.append("  float acc = A[base];\n  for (uint r = 1; r < pc.redsize; ++r) { acc = min(acc, A[base + r]); }\n"); }
    else if (rn.op == KOp::ArgMax) { s.append("  float bv = A[base]; uint bi = 0u;\n  for (uint r = 1; r < pc.redsize; ++r) { if (A[base + r] > bv) { bv = A[base + r]; bi = r; } }\n  float acc = (float)bi;\n"); }
    else { s.append("  float bv = A[base]; uint bi = 0u;\n  for (uint r = 1; r < pc.redsize; ++r) { if (A[base + r] < bv) { bv = A[base + r]; bi = r; } }\n  float acc = (float)bi;\n"); }
    s.append("  O[o] = acc;\n}\n");
    return true;
}

// T2 FAST parallel reduce (Sum/Prod/Max/Min) MSL — one threadgroup per output, grid-stride partials + threadgroup tree.
inline bool emit_reduce_fast_msl(const KGraph& g, int output, GlslKernel& out)
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
    msl_detail::header(s);
    s.append("struct PC { uint nout; uint redsize; };\n");
    s.append("kernel void ckir(device const float* A [[buffer(0)]], device float* O [[buffer(1)]], constant PC& pc [[buffer(2)]], uint o [[threadgroup_position_in_grid]], uint tid [[thread_position_in_threadgroup]]) {\n");
    s.append("  threadgroup float sdata[256];\n  uint base = o * pc.redsize;\n");
    s.append("  float acc = "); s.append(glsl_detail::fast_init(rn.op, true));
    s.append(";\n  for (uint i = tid; i < pc.redsize; i += 256u) { acc = ");
    glsl_detail::fast_comb(s, rn.op, "acc", "A[base + i]", "max", "min");
    s.append("; }\n  sdata[tid] = acc;\n  threadgroup_barrier(mem_flags::mem_threadgroup);\n");
    s.append("  for (uint sh = 128u; sh > 0u; sh >>= 1) { if (tid < sh) { sdata[tid] = ");
    glsl_detail::fast_comb(s, rn.op, "sdata[tid]", "sdata[tid + sh]", "max", "min");
    s.append("; } threadgroup_barrier(mem_flags::mem_threadgroup); }\n");
    s.append("  if (tid == 0u) { O[o] = sdata[0]; }\n}\n");
    return true;
}

// Gather MSL kernel — out[m, ...] = data[idx[m], ...] (row-gather along axis 0). idx holds f32-encoded integers.
inline bool emit_gather_msl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& gn = g.node(output);
    if (gn.op != KOp::Gather || g.node(gn.a).op != KOp::Input || g.node(gn.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(gn.a).iidx; // data
    out.input_iidx[1] = g.node(gn.b).iidx; // idx
    crd::containers::String& s = out.source;
    s.clear();
    msl_detail::header(s);
    s.append("struct PC { uint nout; uint rowsize; };\n");
    s.append("kernel void ckir(device const float* A [[buffer(0)]], device const float* I [[buffer(1)]], device float* O [[buffer(2)]], constant PC& pc [[buffer(3)]], uint o [[thread_position_in_grid]]) {\n");
    s.append("  if (o >= pc.nout) return;\n");
    s.append("  uint m = o / pc.rowsize; uint c = o % pc.rowsize;\n");
    s.append("  uint r = (uint)(int)I[m];\n");
    s.append("  O[o] = A[r * pc.rowsize + c];\n}\n");
    return true;
}

// Scatter MSL kernel — out=base, then out[idx[m],...]=updates[m,...] (LAST-WINS, output-centric ⇒ race-free).
inline bool emit_scatter_msl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::Scatter || g.node(sn.a).op != KOp::Input || g.node(sn.b).op != KOp::Input || g.node(sn.c).op != KOp::Input) { return false; }
    out.n_inputs      = 3;
    out.input_iidx[0] = g.node(sn.a).iidx; // base
    out.input_iidx[1] = g.node(sn.b).iidx; // idx
    out.input_iidx[2] = g.node(sn.c).iidx; // updates
    crd::containers::String& s = out.source;
    s.clear();
    msl_detail::header(s);
    s.append("struct PC { uint nout; uint rowsize; uint M; };\n");
    s.append("kernel void ckir(device const float* B [[buffer(0)]], device const float* I [[buffer(1)]], device const float* U [[buffer(2)]], device float* O [[buffer(3)]], constant PC& pc [[buffer(4)]], uint o [[thread_position_in_grid]]) {\n");
    s.append("  if (o >= pc.nout) return;\n");
    s.append("  uint r = o / pc.rowsize; uint c = o % pc.rowsize;\n");
    s.append("  float result = B[o];\n");
    s.append("  for (uint m = 0u; m < pc.M; ++m) { if ((uint)(int)I[m] == r) { result = U[m * pc.rowsize + c]; } }\n");
    s.append("  O[o] = result;\n}\n");
    return true;
}

// Inclusive SCAN (prefix-sum) MSL kernel along the trailing axis — one thread per row, sequential (fast-math OFF ⇒ bit-exact).
inline bool emit_scan_msl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::ScanSum || g.node(sn.a).op != KOp::Input) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(sn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    msl_detail::header(s);
    s.append("struct PC { uint nrows; uint scanlen; };\n");
    s.append("kernel void ckir(device const float* A [[buffer(0)]], device float* O [[buffer(1)]], constant PC& pc [[buffer(2)]], uint row [[thread_position_in_grid]]) {\n");
    s.append("  if (row >= pc.nrows) return;\n  uint base = row * pc.scanlen;\n  float acc = 0.0f;\n");
    s.append("  for (uint c = 0u; c < pc.scanlen; ++c) { acc = acc + A[base + c]; O[base + c] = acc; }\n}\n");
    return true;
}

// T2 FAST parallel prefix-sum MSL (one threadgroup per row): per-thread chunk scan + serial chunk-total scan + prefix add.
inline bool emit_scan_fast_msl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::ScanSum || sn.tier != DetTier::Fast || g.node(sn.a).op != KOp::Input) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(sn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    msl_detail::header(s);
    s.append("struct PC { uint nrows; uint scanlen; };\n");
    s.append("kernel void ckir(device const float* A [[buffer(0)]], device float* O [[buffer(1)]], constant PC& pc [[buffer(2)]], uint row [[threadgroup_position_in_grid]], uint tid [[thread_position_in_threadgroup]]) {\n");
    s.append("  threadgroup float ctot[256];\n  uint base = row * pc.scanlen;\n");
    s.append("  uint C = (pc.scanlen + 255u) / 256u; uint lo = tid * C; uint hi = min(lo + C, pc.scanlen);\n");
    s.append("  float acc = 0.0f;\n  for (uint i = lo; i < hi; ++i) { acc = acc + A[base + i]; O[base + i] = acc; }\n");
    s.append("  ctot[tid] = acc;\n  threadgroup_barrier(mem_flags::mem_threadgroup);\n");
    s.append("  if (tid == 0u) { float run = 0.0f; for (uint t = 0u; t < 256u; ++t) { float v = ctot[t]; ctot[t] = run; run = run + v; } }\n");
    s.append("  threadgroup_barrier(mem_flags::mem_threadgroup);\n  float prefix = ctot[tid];\n");
    s.append("  for (uint i = lo; i < hi; ++i) { O[base + i] = O[base + i] + prefix; }\n}\n");
    return true;
}

} // namespace crd::kir
