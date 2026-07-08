#pragma once

// ckir_wgsl.hpp — Phase 3.1.6 v17-d: the CKIR **WGSL emitter** (the WebGPU backend's code generator — the browser/WASM
// path). Same three single-kernel shapes as the other emitters, as WGSL compute shaders. Storage buffers via
// @group(0) @binding(k); dims via a uniform buffer (WebGPU has no push constants). NOTE: WGSL has no `precise`
// qualifier, so an implementation MAY fuse FMAs ⇒ WebGPU is ULP-tolerant vs the CPU reference (not guaranteed
// bit-exact like Vulkan/CUDA/DX12). Pure String production; the backend compiles + dispatches. Reuses the shared
// helpers + GlslKernel. ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_glsl.hpp> // GlslKernel + glsl_detail::{app_uint,app_flit,is_fusable}

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>

namespace crd::kir
{

namespace wgsl_detail
{
// binding n_inputs = output; binding n_inputs+1 = the uniform (dims). PC struct has 4 u32 fields d0..d3.
inline void emit_uniform(crd::containers::String& s, int binding)
{
    s.append("struct PC { d0 : u32, d1 : u32, d2 : u32, d3 : u32, };\n");
    s.append("@group(0) @binding("); glsl_detail::app_uint(s, binding); s.append(") var<uniform> pc : PC;\n");
}
} // namespace wgsl_detail

// Fused-elementwise WGSL kernel (dims: pc.d0 = n).
inline bool emit_elementwise_wgsl(const KGraph& g, int output, crd::memory::IAllocator* scratch, GlslKernel& out)
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
    for (int b = 0; b < out.n_inputs; ++b) { s.append("@group(0) @binding("); app_uint(s, b); s.append(") var<storage, read> in"); app_uint(s, b); s.append(" : array<f32>;\n"); }
    s.append("@group(0) @binding("); app_uint(s, out.n_inputs); s.append(") var<storage, read_write> outb : array<f32>;\n");
    wgsl_detail::emit_uniform(s, out.n_inputs + 1);
    s.append("@compute @workgroup_size(256)\nfn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {\n  let i = gid.x;\n  if (i >= pc.d0) { return; }\n");
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        s.append("  let t"); app_uint(s, i); s.append(" : f32 = ");
        const auto ta = [&](int id) { s.append("t"); app_uint(s, id); };
        switch (nd.op)
        {
        case KOp::Input: s.append("in"); app_uint(s, binding_of[static_cast<crd::usize>(i)]); s.append("[i]"); break;
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
        case KOp::Sign: s.append("select(select(0.0, -1.0, ("); ta(nd.a); s.append(" < 0.0)), 1.0, ("); ta(nd.a); s.append(" > 0.0))"); break;
        case KOp::Add: ta(nd.a); s.append(" + "); ta(nd.b); break;
        case KOp::Sub: ta(nd.a); s.append(" - "); ta(nd.b); break;
        case KOp::Mul: ta(nd.a); s.append(" * "); ta(nd.b); break;
        case KOp::Div: ta(nd.a); s.append(" / "); ta(nd.b); break;
        case KOp::Max: s.append("max("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Min: s.append("min("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::CmpLt: s.append("select(0.0, 1.0, "); ta(nd.a); s.append(" < "); ta(nd.b); s.append(")"); break;
        case KOp::CmpEq: s.append("select(0.0, 1.0, "); ta(nd.a); s.append(" == "); ta(nd.b); s.append(")"); break;
        case KOp::CmpLe: s.append("select(0.0, 1.0, "); ta(nd.a); s.append(" <= "); ta(nd.b); s.append(")"); break;
        case KOp::Select: s.append("select("); ta(nd.b); s.append(", "); ta(nd.a); s.append(", "); ta(nd.c); s.append(" != 0.0)"); break;
        default: return false;
        }
        s.append(";\n");
    }
    s.append("  outb[i] = t"); app_uint(s, output); s.append(";\n}\n");
    return true;
}

// Batched-matmul WGSL kernel (dims: d0=M, d1=K, d2=N, d3=nbatch).
inline bool emit_contract_wgsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& c = g.node(output);
    if (c.op != KOp::Contract || g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("@group(0) @binding(0) var<storage, read> A : array<f32>;\n@group(0) @binding(1) var<storage, read> Bm : array<f32>;\n@group(0) @binding(2) var<storage, read_write> C : array<f32>;\n");
    wgsl_detail::emit_uniform(s, 3);
    s.append("@compute @workgroup_size(256)\nfn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {\n  let M = pc.d0; let K = pc.d1; let N = pc.d2;\n  let mn = M * N; let total = mn * pc.d3;\n  if (gid.x >= total) { return; }\n");
    s.append("  let b = gid.x / mn; let rem = gid.x % mn; let m = rem / N; let nn = rem % N;\n");
    s.append("  let aoff = b * M * K + m * K; let boff = b * K * N + nn;\n");
    s.append("  var acc : f32 = 0.0;\n  for (var k : u32 = 0u; k < K; k = k + 1u) { let prod : f32 = A[aoff + k] * Bm[boff + k * N]; acc = acc + prod; }\n");
    s.append("  C[b * mn + m * N + nn] = acc;\n}\n");
    return true;
}

// Reduce WGSL kernel (trailing-contiguous; dims d0=nout, d1=redsize).
inline bool emit_reduce_wgsl(const KGraph& g, int output, GlslKernel& out)
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
    s.append("@group(0) @binding(0) var<storage, read> A : array<f32>;\n@group(0) @binding(1) var<storage, read_write> O : array<f32>;\n");
    wgsl_detail::emit_uniform(s, 2);
    s.append("@compute @workgroup_size(256)\nfn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {\n  let o = gid.x;\n  if (o >= pc.d0) { return; }\n  let redsize = pc.d1; let base = o * redsize;\n");
    if (rn.op == KOp::ReduceSum) { s.append("  var acc : f32 = 0.0;\n  for (var r : u32 = 0u; r < redsize; r = r + 1u) { acc = acc + A[base + r]; }\n"); }
    else if (rn.op == KOp::ReduceProd) { s.append("  var acc : f32 = 1.0;\n  for (var r : u32 = 0u; r < redsize; r = r + 1u) { acc = acc * A[base + r]; }\n"); }
    else if (rn.op == KOp::ReduceMax) { s.append("  var acc : f32 = A[base];\n  for (var r : u32 = 1u; r < redsize; r = r + 1u) { acc = max(acc, A[base + r]); }\n"); }
    else if (rn.op == KOp::ReduceMin) { s.append("  var acc : f32 = A[base];\n  for (var r : u32 = 1u; r < redsize; r = r + 1u) { acc = min(acc, A[base + r]); }\n"); }
    else if (rn.op == KOp::ArgMax) { s.append("  var bv : f32 = A[base]; var bi : u32 = 0u;\n  for (var r : u32 = 1u; r < redsize; r = r + 1u) { if (A[base + r] > bv) { bv = A[base + r]; bi = r; } }\n  var acc : f32 = f32(bi);\n"); }
    else { s.append("  var bv : f32 = A[base]; var bi : u32 = 0u;\n  for (var r : u32 = 1u; r < redsize; r = r + 1u) { if (A[base + r] < bv) { bv = A[base + r]; bi = r; } }\n  var acc : f32 = f32(bi);\n"); }
    s.append("  O[o] = acc;\n}\n");
    return true;
}

// T2 FAST parallel reduce (Sum/Prod/Max/Min) WGSL — one workgroup per output, grid-stride partials + workgroup tree.
inline bool emit_reduce_fast_wgsl(const KGraph& g, int output, GlslKernel& out)
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
    s.append("@group(0) @binding(0) var<storage, read> A : array<f32>;\n@group(0) @binding(1) var<storage, read_write> O : array<f32>;\n");
    wgsl_detail::emit_uniform(s, 2);
    s.append("var<workgroup> sdata : array<f32, 256>;\n");
    s.append("@compute @workgroup_size(256)\nfn cs_main(@builtin(workgroup_id) wid : vec3<u32>, @builtin(local_invocation_id) lid : vec3<u32>) {\n");
    s.append("  let o = wid.x; let tid = lid.x; let redsize = pc.d1; let base = o * redsize;\n");
    s.append("  var acc : f32 = "); s.append(glsl_detail::fast_init(rn.op, false));
    s.append(";\n  for (var i : u32 = tid; i < redsize; i = i + 256u) { acc = ");
    glsl_detail::fast_comb(s, rn.op, "acc", "A[base + i]", "max", "min");
    s.append("; }\n  sdata[tid] = acc;\n  workgroupBarrier();\n");
    s.append("  for (var sh : u32 = 128u; sh > 0u; sh = sh >> 1u) { if (tid < sh) { sdata[tid] = ");
    glsl_detail::fast_comb(s, rn.op, "sdata[tid]", "sdata[tid + sh]", "max", "min");
    s.append("; } workgroupBarrier(); }\n");
    s.append("  if (tid == 0u) { O[o] = sdata[0]; }\n}\n");
    return true;
}

// Gather WGSL kernel — out[m, ...] = data[idx[m], ...] (row-gather along axis 0). idx holds f32-encoded integers.
inline bool emit_gather_wgsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& gn = g.node(output);
    if (gn.op != KOp::Gather || g.node(gn.a).op != KOp::Input || g.node(gn.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(gn.a).iidx; // data
    out.input_iidx[1] = g.node(gn.b).iidx; // idx
    crd::containers::String& s = out.source;
    s.clear();
    s.append("@group(0) @binding(0) var<storage, read> A : array<f32>;\n@group(0) @binding(1) var<storage, read> I : array<f32>;\n@group(0) @binding(2) var<storage, read_write> O : array<f32>;\n");
    wgsl_detail::emit_uniform(s, 3);
    s.append("@compute @workgroup_size(256)\nfn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {\n  let o = gid.x;\n  if (o >= pc.d0) { return; }\n  let rowsize = pc.d1;\n");
    s.append("  let m = o / rowsize; let c = o % rowsize;\n");
    s.append("  let r = u32(i32(I[m]));\n");
    s.append("  O[o] = A[r * rowsize + c];\n}\n");
    return true;
}

// Scatter WGSL kernel — out=base, then out[idx[m],...]=updates[m,...] (LAST-WINS, output-centric ⇒ race-free).
inline bool emit_scatter_wgsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::Scatter || g.node(sn.a).op != KOp::Input || g.node(sn.b).op != KOp::Input || g.node(sn.c).op != KOp::Input) { return false; }
    out.n_inputs      = 3;
    out.input_iidx[0] = g.node(sn.a).iidx; // base
    out.input_iidx[1] = g.node(sn.b).iidx; // idx
    out.input_iidx[2] = g.node(sn.c).iidx; // updates
    crd::containers::String& s = out.source;
    s.clear();
    s.append("@group(0) @binding(0) var<storage, read> B : array<f32>;\n@group(0) @binding(1) var<storage, read> I : array<f32>;\n@group(0) @binding(2) var<storage, read> U : array<f32>;\n@group(0) @binding(3) var<storage, read_write> O : array<f32>;\n");
    wgsl_detail::emit_uniform(s, 4);
    s.append("@compute @workgroup_size(256)\nfn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {\n  let o = gid.x;\n  if (o >= pc.d0) { return; }\n  let rowsize = pc.d1; let mcount = pc.d2;\n");
    s.append("  let r = o / rowsize; let c = o % rowsize;\n");
    s.append("  var result : f32 = B[o];\n");
    s.append("  for (var m : u32 = 0u; m < mcount; m = m + 1u) { if (u32(i32(I[m])) == r) { result = U[m * rowsize + c]; } }\n");
    s.append("  O[o] = result;\n}\n");
    return true;
}

// Inclusive SCAN (prefix-sum) WGSL kernel along the trailing axis — one thread per row, sequential (ULP-tolerant on WGSL).
inline bool emit_scan_wgsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::ScanSum || g.node(sn.a).op != KOp::Input) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(sn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("@group(0) @binding(0) var<storage, read> A : array<f32>;\n@group(0) @binding(1) var<storage, read_write> O : array<f32>;\n");
    wgsl_detail::emit_uniform(s, 2);
    s.append("@compute @workgroup_size(256)\nfn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {\n  let row = gid.x;\n  if (row >= pc.d0) { return; }\n  let scanlen = pc.d1; let base = row * scanlen;\n");
    s.append("  var acc : f32 = 0.0;\n");
    s.append("  for (var c : u32 = 0u; c < scanlen; c = c + 1u) { acc = acc + A[base + c]; O[base + c] = acc; }\n}\n");
    return true;
}

// T2 FAST parallel prefix-sum WGSL (one workgroup per row): per-thread chunk scan + serial chunk-total scan + prefix add.
inline bool emit_scan_fast_wgsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::ScanSum || sn.tier != DetTier::Fast || g.node(sn.a).op != KOp::Input) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(sn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("@group(0) @binding(0) var<storage, read> A : array<f32>;\n@group(0) @binding(1) var<storage, read_write> O : array<f32>;\n");
    wgsl_detail::emit_uniform(s, 2);
    s.append("var<workgroup> ctot : array<f32, 256>;\n");
    s.append("@compute @workgroup_size(256)\nfn cs_main(@builtin(workgroup_id) wid : vec3<u32>, @builtin(local_invocation_id) lid : vec3<u32>) {\n");
    s.append("  let row = wid.x; let tid = lid.x; let scanlen = pc.d1; let base = row * scanlen;\n");
    s.append("  let C = (scanlen + 255u) / 256u; let lo = tid * C; let hi = min(lo + C, scanlen);\n");
    s.append("  var acc : f32 = 0.0;\n  for (var i : u32 = lo; i < hi; i = i + 1u) { acc = acc + A[base + i]; O[base + i] = acc; }\n");
    s.append("  ctot[tid] = acc;\n  workgroupBarrier();\n");
    s.append("  if (tid == 0u) { var run : f32 = 0.0; for (var t : u32 = 0u; t < 256u; t = t + 1u) { let v = ctot[t]; ctot[t] = run; run = run + v; } }\n");
    s.append("  workgroupBarrier();\n  let prefix = ctot[tid];\n");
    s.append("  for (var i : u32 = lo; i < hi; i = i + 1u) { O[base + i] = O[base + i] + prefix; }\n}\n");
    return true;
}

// Emit the epilogue cone as a WGSL `fn epi(acc : f32, b0 : f32, ...) -> f32` (let-based; `select(f,t,cond)`).
inline void emit_epi_wgsl(const KGraph& g, int output, const FuseInfo& fi, crd::memory::IAllocator* scratch, crd::containers::String& s)
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
        if (nd.op == KOp::Contract || nd.op == KOp::Broadcast) { continue; }
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
    }
    s.append("fn epi(acc : f32");
    for (int j = 0; j < fi.n_bias; ++j) { s.append(", b"); app_uint(s, j); s.append(" : f32"); }
    s.append(") -> f32 {\n");
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        s.append("  let e"); app_uint(s, i); s.append(" : f32 = ");
        const auto te = [&](int id) { s.append("e"); app_uint(s, id); };
        if (i == fi.contract) { s.append("acc"); }
        else
        {
            bool is_bias = false;
            for (int j = 0; j < fi.n_bias; ++j) { if (fi.bias_node[j] == i) { s.append("b"); app_uint(s, j); is_bias = true; break; } }
            if (!is_bias)
            {
                switch (nd.op)
                {
                case KOp::Const: app_flit(s, nd.cval); break;
                case KOp::Add: te(nd.a); s.append(" + "); te(nd.b); break;
                case KOp::Sub: te(nd.a); s.append(" - "); te(nd.b); break;
                case KOp::Mul: te(nd.a); s.append(" * "); te(nd.b); break;
                case KOp::Div: te(nd.a); s.append(" / "); te(nd.b); break;
                case KOp::Max: s.append("max("); te(nd.a); s.append(", "); te(nd.b); s.append(")"); break;
                case KOp::Min: s.append("min("); te(nd.a); s.append(", "); te(nd.b); s.append(")"); break;
                case KOp::CmpLt: s.append("select(0.0, 1.0, "); te(nd.a); s.append(" < "); te(nd.b); s.append(")"); break;
                case KOp::CmpEq: s.append("select(0.0, 1.0, "); te(nd.a); s.append(" == "); te(nd.b); s.append(")"); break;
                case KOp::CmpLe: s.append("select(0.0, 1.0, "); te(nd.a); s.append(" <= "); te(nd.b); s.append(")"); break;
                case KOp::Select: s.append("select("); te(nd.b); s.append(", "); te(nd.a); s.append(", "); te(nd.c); s.append(" != 0.0)"); break;
                case KOp::Neg: s.append("-"); te(nd.a); break;
                case KOp::Recip: s.append("1.0/"); te(nd.a); break;
                case KOp::Abs: s.append("abs("); te(nd.a); s.append(")"); break;
                case KOp::Exp: s.append("exp("); te(nd.a); s.append(")"); break;
                case KOp::Log: s.append("log("); te(nd.a); s.append(")"); break;
                case KOp::Sin: s.append("sin("); te(nd.a); s.append(")"); break;
                case KOp::Cos: s.append("cos("); te(nd.a); s.append(")"); break;
                case KOp::Sqrt: s.append("sqrt("); te(nd.a); s.append(")"); break;
                case KOp::Tanh: s.append("tanh("); te(nd.a); s.append(")"); break;
                case KOp::Floor: s.append("floor("); te(nd.a); s.append(")"); break;
                case KOp::Ceil: s.append("ceil("); te(nd.a); s.append(")"); break;
                case KOp::Trunc: s.append("trunc("); te(nd.a); s.append(")"); break;
                case KOp::Round: s.append("round("); te(nd.a); s.append(")"); break;
                case KOp::Sign: s.append("select(select(0.0, -1.0, ("); te(nd.a); s.append(" < 0.0)), 1.0, ("); te(nd.a); s.append(" > 0.0))"); break;
                default: break;
                }
            }
        }
        s.append(";\n");
    }
    s.append("  return e"); app_uint(s, output); s.append(";\n}\n");
}

// FUSED GEMM+epilogue WGSL kernel (naive matmul + epilogue in the store) — WebGPU inherits the fusion crush. Dims via
// the uniform (d0=M, d1=N, d2=K). ULP-tolerant (WGSL has no `precise`).
inline bool emit_contract_fused_wgsl(const KGraph& g, int output, int contract, const FuseInfo& fi, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    const KNode& c = g.node(contract);
    if (c.op != KOp::Contract || g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2 + fi.n_bias;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;
    for (int j = 0; j < fi.n_bias; ++j) { out.input_iidx[2 + j] = fi.bias_iidx[j]; }
    crd::containers::String& s = out.source;
    s.clear();
    s.append("@group(0) @binding(0) var<storage, read> A : array<f32>;\n@group(0) @binding(1) var<storage, read> Bm : array<f32>;\n");
    for (int j = 0; j < fi.n_bias; ++j) { s.append("@group(0) @binding("); glsl_detail::app_uint(s, 2 + j); s.append(") var<storage, read> bias"); glsl_detail::app_uint(s, j); s.append(" : array<f32>;\n"); }
    s.append("@group(0) @binding("); glsl_detail::app_uint(s, 2 + fi.n_bias); s.append(") var<storage, read_write> C : array<f32>;\n");
    wgsl_detail::emit_uniform(s, 2 + fi.n_bias + 1);
    emit_epi_wgsl(g, output, fi, scratch, s);
    s.append("@compute @workgroup_size(256)\nfn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {\n  let i = gid.x;\n  if (i >= pc.d0 * pc.d1) { return; }\n");
    s.append("  let m = i / pc.d1; let nn = i % pc.d1;\n  var acc : f32 = 0.0;\n  for (var k : u32 = 0u; k < pc.d2; k = k + 1u) { let prod : f32 = A[m * pc.d2 + k] * Bm[k * pc.d1 + nn]; acc = acc + prod; }\n");
    s.append("  C[m * pc.d1 + nn] = epi(acc");
    for (int j = 0; j < fi.n_bias; ++j) { s.append(", bias"); glsl_detail::app_uint(s, j); s.append("[nn]"); }
    s.append(");\n}\n");
    return true;
}

} // namespace crd::kir
