#pragma once

// ckir_glsl.hpp — Phase 3.1.6 v17-b: the CKIR **GLSL emitter** (the Vulkan/SPIR-V backend's code generator). Lowers a
// FUSED ELEMENTWISE CKIR subgraph to a single GLSL compute shader — one thread per output element, the whole
// elementwise expression tree computed inline (kernel fusion: an N-op elementwise chain becomes ONE kernel, one global
// load per input + one store, no intermediate buffers). Each temp is `precise` (⇒ SPIR-V NoContraction ⇒ no FMA fusion
// ⇒ bit-matches the `-ffp-contract=off` CPU reference — the determinism lever, baked in from line one). The emitter is
// pure String production (no GPU/Vulkan dep); the test compiles the result to SPIR-V via crd-shader to prove it valid.
// Reduce/contract/movement kernels + the runtime dispatch are the rest of v17-b. ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_tile.hpp> // FuseInfo + detect_fuse (shared epilogue-fusion analysis)

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>

#include <cstdio>

namespace crd::kir
{

constexpr int kMaxKernelInputs = 32;

struct GlslKernel
{
    crd::containers::String source;
    int                     n_inputs = 0;
    int                     input_iidx[kMaxKernelInputs] = {}; // binding b (0..n_inputs-1) reads input iidx; output = binding n_inputs
    explicit GlslKernel(crd::memory::IAllocator* a) : source(a) {}
};

namespace glsl_detail
{
inline void app_uint(crd::containers::String& s, int v) { char b[24]; std::snprintf(b, sizeof(b), "%d", v); s.append(b); }
inline void app_flit(crd::containers::String& s, crd::f64 v) // a GLSL float literal (always has a '.' or exponent)
{
    char b[40];
    std::snprintf(b, sizeof(b), "%.9g", v);
    s.append(b);
    bool dotless = true;
    for (const char* p = b; *p != '\0'; ++p) { if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'n' || *p == 'i') { dotless = false; break; } }
    if (dotless) { s.append(".0"); }
}
[[nodiscard]] inline bool is_fusable(KOp op) noexcept
{
    switch (op)
    {
    case KOp::Input: case KOp::Const:
    case KOp::Neg: case KOp::Recip: case KOp::Abs: case KOp::Exp: case KOp::Log:
    case KOp::Sin: case KOp::Cos: case KOp::Sqrt: case KOp::Tanh: case KOp::Floor: case KOp::Ceil: case KOp::Sign: case KOp::Trunc: case KOp::Round:
    case KOp::Add: case KOp::Sub: case KOp::Mul: case KOp::Div: case KOp::Max: case KOp::Min:
    case KOp::CmpLt: case KOp::CmpEq: case KOp::CmpLe:
    case KOp::Select: return true;
    default: return false;
    }
}
// T2 fast-reduce codegen (shared by every backend emitter). `fast_comb` appends the per-op combine of x,y using the
// language's max/min builtins; `fast_init` is a lone lane's identity. Sum/Prod reassociate (RFA); Max/Min stay bit-exact.
inline void fast_comb(crd::containers::String& s, KOp op, const char* x, const char* y, const char* maxfn, const char* minfn)
{
    if (op == KOp::ReduceSum) { s.append(x); s.append(" + "); s.append(y); }
    else if (op == KOp::ReduceProd) { s.append(x); s.append(" * "); s.append(y); }
    else if (op == KOp::ReduceMax) { s.append(maxfn); s.append("("); s.append(x); s.append(", "); s.append(y); s.append(")"); }
    else { s.append(minfn); s.append("("); s.append(x); s.append(", "); s.append(y); s.append(")"); }
}
[[nodiscard]] inline const char* fast_init(KOp op, bool fsfx)
{
    if (op == KOp::ReduceProd) { return fsfx ? "1.0f" : "1.0"; }
    if (op == KOp::ReduceMax) { return fsfx ? "-3.402823466e38f" : "-3.402823466e38"; }
    if (op == KOp::ReduceMin) { return fsfx ? "3.402823466e38f" : "3.402823466e38"; }
    return fsfx ? "0.0f" : "0.0";
}
} // namespace glsl_detail

// Emit a fused elementwise f32 compute kernel for `output`. Returns false if the subtree isn't purely elementwise
// (Input(same-shape) / Const / unary / binary / Select) — that boundary is where the scheduler (v17-e) would split.
inline bool emit_elementwise_glsl(const KGraph& g, int output, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    const int n = g.size();
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
        if (!is_fusable(nd.op)) { return false; } // a non-elementwise node in the cone ⇒ not a single elementwise kernel
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
    }

    // binding map: distinct input iidx -> binding index (in first-seen id order)
    crd::containers::Array<int> binding_of(scratch); // per node id: the input binding, or -1
    binding_of.resize(static_cast<crd::usize>(n), -1);
    out.n_inputs = 0;
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        if (g.node(i).op == KOp::Input)
        {
            binding_of[static_cast<crd::usize>(i)] = out.n_inputs;
            out.input_iidx[out.n_inputs] = g.node(i).iidx;
            ++out.n_inputs;
        }
    }

    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    for (int b = 0; b < out.n_inputs; ++b)
    {
        s.append("layout(std430, binding = "); app_uint(s, b); s.append(") readonly buffer B"); app_uint(s, b); s.append(" { float in"); app_uint(s, b); s.append("[]; };\n");
    }
    s.append("layout(std430, binding = "); app_uint(s, out.n_inputs); s.append(") writeonly buffer BOUT { float outb[]; };\n");
    s.append("layout(push_constant) uniform PC { uint n; };\n");
    s.append("void main() {\n  uint gid = gl_GlobalInvocationID.x;\n  if (gid >= n) { return; }\n");

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
        case KOp::Round: s.append("roundEven("); ta(nd.a); s.append(")"); break;
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

// Emit a batched-matmul kernel for a Contract node whose two operands are Input leaves (a single-kernel matmul; the
// scheduler splits deeper graphs). One thread per output element C[b,m,n], sequential-k `precise` product +
// accumulation ⇒ bit-matches the CPU reference (dtype-faithful, ascending-k). Push constants: M, K, N, nbatch.
// binding 0 = A (iidx input_iidx[0]), 1 = B (input_iidx[1]), 2 = C (output).
inline bool emit_contract_glsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& c = g.node(output);
    if (c.op != KOp::Contract) { return false; }
    if (g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;

    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BA { float A[]; };\n");
    s.append("layout(std430, binding = 1) readonly buffer BB { float Bm[]; };\n");
    s.append("layout(std430, binding = 2) writeonly buffer BC { float C[]; };\n");
    s.append("layout(push_constant) uniform PC { uint M; uint K; uint N; uint nbatch; };\n");
    s.append("void main() {\n");
    s.append("  uint gid = gl_GlobalInvocationID.x;\n");
    s.append("  uint mn = M * N;\n  uint total = mn * nbatch;\n  if (gid >= total) { return; }\n");
    s.append("  uint b = gid / mn;\n  uint rem = gid % mn;\n  uint m = rem / N;\n  uint nn = rem % N;\n");
    s.append("  uint aoff = b * M * K + m * K;\n  uint boff = b * K * N + nn;\n");
    s.append("  precise float acc = 0.0;\n");
    s.append("  for (uint k = 0u; k < K; ++k) {\n");
    s.append("    precise float prod = A[aoff + k] * Bm[boff + k * N];\n");
    s.append("    acc = acc + prod;\n");
    s.append("  }\n");
    s.append("  C[b * mn + m * N + nn] = acc;\n}\n");
    return true;
}

// BLOCK-TILED GEMM (single batch): 64x64 output block per workgroup, 4x4 SCALARIZED register microtile, A/B staged
// through shared memory. K-loop stays SEQUENTIAL + `precise` ⇒ BIT-EXACT vs the naive kernel + oracle. ⚠ WIP / UNROUTED:
// measured SLOWER than naive for L2-resident sizes — (M/64)*(N/64) is too few workgroups to fill the SMs, so the GPU
// starves. A winning version needs block-size + occupancy tuning under Nsight; kept as the correct starting point.
inline bool emit_contract_tiled_glsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& c = g.node(output);
    if (c.op != KOp::Contract || g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BA { float A[]; };\n");
    s.append("layout(std430, binding = 1) readonly buffer BB { float Bm[]; };\n");
    s.append("layout(std430, binding = 2) writeonly buffer BC { float C[]; };\n");
    s.append("layout(push_constant) uniform PC { uint M; uint K; uint N; uint nbatch; };\n");
    s.append("shared float As[512];\n"); // 64 x 8
    s.append("shared float Bs[512];\n"); // 8 x 64
    s.append("void main() {\n");
    s.append("  uint nbc = N / 64u; uint bid = gl_WorkGroupID.x;\n");
    s.append("  uint blockRow = (bid / nbc) * 64u; uint blockCol = (bid % nbc) * 64u;\n");
    s.append("  uint tid = gl_LocalInvocationID.x; uint tr = tid / 16u; uint tc = tid % 16u;\n");
    s.append("  uint arow = blockRow + tr * 4u; uint acol = blockCol + tc * 4u;\n");
    const char* d[4] = {"0", "1", "2", "3"};
    // SCALARIZED accumulators (16 named regs, not an array ⇒ no local-memory spill under `precise`).
    for (int i = 0; i < 4; ++i)
    {
        s.append("  precise float ");
        for (int j = 0; j < 4; ++j) { s.append("a"); s.append(d[i]); s.append(d[j]); s.append(" = 0.0"); if (j < 3) { s.append(", "); } }
        s.append(";\n");
    }
    s.append("  for (uint k0 = 0u; k0 < K; k0 += 8u) {\n");
    s.append("    for (uint t = tid; t < 512u; t += 256u) { uint r = t / 8u; uint cc = t % 8u; As[t] = A[(blockRow + r) * K + (k0 + cc)]; }\n");
    s.append("    for (uint t = tid; t < 512u; t += 256u) { uint r = t / 64u; uint cc = t % 64u; Bs[t] = Bm[(k0 + r) * N + (blockCol + cc)]; }\n");
    s.append("    barrier();\n");
    s.append("    for (uint kk = 0u; kk < 8u; ++kk) {\n");
    s.append("      float ar0 = As[(tr*4u+0u)*8u+kk], ar1 = As[(tr*4u+1u)*8u+kk], ar2 = As[(tr*4u+2u)*8u+kk], ar3 = As[(tr*4u+3u)*8u+kk];\n");
    s.append("      float br0 = Bs[kk*64u+tc*4u+0u], br1 = Bs[kk*64u+tc*4u+1u], br2 = Bs[kk*64u+tc*4u+2u], br3 = Bs[kk*64u+tc*4u+3u];\n");
    const char* ar[4] = {"ar0", "ar1", "ar2", "ar3"};
    const char* br[4] = {"br0", "br1", "br2", "br3"};
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            s.append("      { precise float p = "); s.append(ar[i]); s.append(" * "); s.append(br[j]);
            s.append("; a"); s.append(d[i]); s.append(d[j]); s.append(" = a"); s.append(d[i]); s.append(d[j]); s.append(" + p; }\n");
        }
    }
    s.append("    }\n    barrier();\n  }\n");
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

// Emit a REDUCE kernel (ReduceSum/ReduceMax over the trailing contiguous axes) for a Reduce of an Input leaf. One
// thread per output element, **sequential ascending** accumulation (`precise` ⇒ fixed order, no float atomics = the
// determinism moat; bit-matches the dtype-faithful CPU reference). Only trailing-contiguous reductions here (the
// common sum-over-rows / reduce-all); strided reductions come with the scheduler. Push constants: nout, redsize.
inline bool emit_reduce_glsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& rn = g.node(output);
    if (!is_reduce(rn.op)) { return false; }
    if (g.node(rn.a).op != KOp::Input) { return false; }
    const Shape& ish = g.node(rn.a).shape;
    int          t   = 0; // trailing reduced axes
    for (int k = ish.rank - 1; k >= 0; --k) { if (((rn.axes >> k) & 1U) != 0U) { ++t; } else { break; } }
    if (t == 0) { return false; }
    crd::u32 tmask = 0;
    for (int k = ish.rank - t; k < ish.rank; ++k) { tmask |= (1U << k); }
    if (rn.axes != tmask) { return false; } // only trailing-contiguous reductions supported by this emitter
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(rn.a).iidx;

    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BA { float A[]; };\n");
    s.append("layout(std430, binding = 1) writeonly buffer BO { float O[]; };\n");
    s.append("layout(push_constant) uniform PC { uint nout; uint redsize; };\n");
    s.append("void main() {\n  uint o = gl_GlobalInvocationID.x;\n  if (o >= nout) { return; }\n");
    s.append("  uint base = o * redsize;\n");
    if (rn.op == KOp::ReduceSum) { s.append("  precise float acc = 0.0;\n  for (uint r = 0u; r < redsize; ++r) { acc = acc + A[base + r]; }\n"); }
    else if (rn.op == KOp::ReduceProd) { s.append("  precise float acc = 1.0;\n  for (uint r = 0u; r < redsize; ++r) { acc = acc * A[base + r]; }\n"); }
    else if (rn.op == KOp::ReduceMax) { s.append("  precise float acc = A[base];\n  for (uint r = 1u; r < redsize; ++r) { acc = max(acc, A[base + r]); }\n"); }
    else if (rn.op == KOp::ReduceMin) { s.append("  precise float acc = A[base];\n  for (uint r = 1u; r < redsize; ++r) { acc = min(acc, A[base + r]); }\n"); }
    else if (rn.op == KOp::ArgMax) { s.append("  float bv = A[base]; uint bi = 0u;\n  for (uint r = 1u; r < redsize; ++r) { if (A[base + r] > bv) { bv = A[base + r]; bi = r; } }\n  precise float acc = float(bi);\n"); }
    else { s.append("  float bv = A[base]; uint bi = 0u;\n  for (uint r = 1u; r < redsize; ++r) { if (A[base + r] < bv) { bv = A[base + r]; bi = r; } }\n  precise float acc = float(bi);\n"); }
    s.append("  O[o] = acc;\n}\n");
    return true;
}

// Emit the T2 FAST parallel reduce (ReduceSum only) — ONE workgroup per output, grid-stride partials + a shared-memory
// tree reduction (log2(256) depth). Reorders the sum (RFA, not bit-exact vs T1) but is run-to-run deterministic + far
// faster than the one-thread-per-output T1 kernel when the reduce axis is long. The "push to the hardware limit" path.
inline bool emit_reduce_fast_glsl(const KGraph& g, int output, GlslKernel& out)
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
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BA { float A[]; };\n");
    s.append("layout(std430, binding = 1) writeonly buffer BO { float O[]; };\n");
    s.append("layout(push_constant) uniform PC { uint nout; uint redsize; };\n");
    s.append("shared float sdata[256];\n");
    s.append("void main() {\n  uint o = gl_WorkGroupID.x; uint tid = gl_LocalInvocationID.x; uint base = o * redsize;\n");
    s.append("  float acc = "); s.append(glsl_detail::fast_init(rn.op, false));
    s.append(";\n  for (uint i = tid; i < redsize; i += 256u) { acc = ");
    glsl_detail::fast_comb(s, rn.op, "acc", "A[base + i]", "max", "min");
    s.append("; }\n  sdata[tid] = acc;\n  barrier();\n");
    s.append("  for (uint sh = 128u; sh > 0u; sh >>= 1) { if (tid < sh) { sdata[tid] = ");
    glsl_detail::fast_comb(s, rn.op, "sdata[tid]", "sdata[tid + sh]", "max", "min");
    s.append("; } barrier(); }\n");
    s.append("  if (tid == 0u) { O[o] = sdata[0]; }\n}\n");
    return true;
}

// Emit a GATHER kernel — out[m, ...] = data[idx[m], ...] (row-gather along axis 0). idx holds f32-encoded integers.
inline bool emit_gather_glsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& gn = g.node(output);
    if (gn.op != KOp::Gather || g.node(gn.a).op != KOp::Input || g.node(gn.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(gn.a).iidx; // data
    out.input_iidx[1] = g.node(gn.b).iidx; // idx
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BA { float A[]; };\n");
    s.append("layout(std430, binding = 1) readonly buffer BI { float I[]; };\n");
    s.append("layout(std430, binding = 2) writeonly buffer BO { float O[]; };\n");
    s.append("layout(push_constant) uniform PC { uint nout; uint rowsize; };\n");
    s.append("void main() {\n  uint o = gl_GlobalInvocationID.x;\n  if (o >= nout) { return; }\n");
    s.append("  uint m = o / rowsize; uint c = o % rowsize;\n");
    s.append("  uint r = uint(int(I[m]));\n");
    s.append("  O[o] = A[r * rowsize + c];\n}\n");
    return true;
}

// Emit a SCATTER kernel — out=base, then out[idx[m],...]=updates[m,...] (LAST-WINS, output-centric ⇒ race-free).
inline bool emit_scatter_glsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::Scatter || g.node(sn.a).op != KOp::Input || g.node(sn.b).op != KOp::Input || g.node(sn.c).op != KOp::Input) { return false; }
    out.n_inputs      = 3;
    out.input_iidx[0] = g.node(sn.a).iidx; // base
    out.input_iidx[1] = g.node(sn.b).iidx; // idx
    out.input_iidx[2] = g.node(sn.c).iidx; // updates
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BB { float B[]; };\n");
    s.append("layout(std430, binding = 1) readonly buffer BI { float I[]; };\n");
    s.append("layout(std430, binding = 2) readonly buffer BU { float U[]; };\n");
    s.append("layout(std430, binding = 3) writeonly buffer BO { float O[]; };\n");
    s.append("layout(push_constant) uniform PC { uint nout; uint rowsize; uint M; };\n");
    s.append("void main() {\n  uint o = gl_GlobalInvocationID.x;\n  if (o >= nout) { return; }\n");
    s.append("  uint r = o / rowsize; uint c = o % rowsize;\n");
    s.append("  float result = B[o];\n");
    s.append("  for (uint m = 0u; m < M; ++m) { if (uint(int(I[m])) == r) { result = U[m * rowsize + c]; } }\n");
    s.append("  O[o] = result;\n}\n");
    return true;
}

// Inclusive SCAN (prefix-sum) GLSL kernel along the trailing axis — one thread per row, sequential `precise` ⇒ bit-exact.
inline bool emit_scan_glsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::ScanSum || g.node(sn.a).op != KOp::Input) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(sn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BA { float A[]; };\n");
    s.append("layout(std430, binding = 1) writeonly buffer BO { float O[]; };\n");
    s.append("layout(push_constant) uniform PC { uint nrows; uint scanlen; };\n");
    s.append("void main() {\n  uint row = gl_GlobalInvocationID.x;\n  if (row >= nrows) { return; }\n");
    s.append("  uint base = row * scanlen;\n  precise float acc = 0.0;\n");
    s.append("  for (uint c = 0u; c < scanlen; ++c) { acc = acc + A[base + c]; O[base + c] = acc; }\n}\n");
    return true;
}

// T2 FAST parallel prefix-sum — ONE workgroup per row: each thread inclusive-scans a contiguous chunk + records its
// total; thread 0 exclusive-scans the 256 chunk totals (small serial); each thread adds its chunk prefix. RFA (chunk
// totals reassociate) but run-to-run deterministic + ~2N/256 work/thread vs N serial. Push-to-the-limit long-row scans.
inline bool emit_scan_fast_glsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::ScanSum || sn.tier != DetTier::Fast || g.node(sn.a).op != KOp::Input) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(sn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BA { float A[]; };\n");
    s.append("layout(std430, binding = 1) buffer BO { float O[]; };\n"); // read_write: loop 2 reads O to add the chunk prefix
    s.append("layout(push_constant) uniform PC { uint nrows; uint scanlen; };\n");
    s.append("shared float ctot[256];\n");
    s.append("void main() {\n  uint row = gl_WorkGroupID.x; uint tid = gl_LocalInvocationID.x; uint base = row * scanlen;\n");
    s.append("  uint C = (scanlen + 255u) / 256u; uint lo = tid * C; uint hi = min(lo + C, scanlen);\n");
    s.append("  float acc = 0.0;\n  for (uint i = lo; i < hi; ++i) { acc = acc + A[base + i]; O[base + i] = acc; }\n");
    s.append("  ctot[tid] = acc;\n  barrier();\n");
    s.append("  if (tid == 0u) { float run = 0.0; for (uint t = 0u; t < 256u; ++t) { float v = ctot[t]; ctot[t] = run; run = run + v; } }\n");
    s.append("  barrier();\n  float prefix = ctot[tid];\n");
    s.append("  for (uint i = lo; i < hi; ++i) { O[base + i] = O[base + i] + prefix; }\n}\n");
    return true;
}

// Emit the epilogue cone as a C-like `float epi(float acc, float b0, ...)` function — SHARED by GLSL and HLSL (identical
// syntax: `exp`/`max`/`abs`/ternary-select, no `f` suffix). Contract → `acc`; each bias Broadcast → its `b{j}` param.
inline void emit_epi_clike(const KGraph& g, int output, const FuseInfo& fi, crd::memory::IAllocator* scratch, crd::containers::String& s)
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
    s.append("float epi(float acc");
    for (int j = 0; j < fi.n_bias; ++j) { s.append(", float b"); app_uint(s, j); }
    s.append(") {\n");
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        s.append("  float e"); app_uint(s, i); s.append(" = ");
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
                case KOp::CmpLt: s.append("(("); te(nd.a); s.append(" < "); te(nd.b); s.append(") ? 1.0 : 0.0)"); break;
                case KOp::CmpEq: s.append("(("); te(nd.a); s.append(" == "); te(nd.b); s.append(") ? 1.0 : 0.0)"); break;
                case KOp::CmpLe: s.append("(("); te(nd.a); s.append(" <= "); te(nd.b); s.append(") ? 1.0 : 0.0)"); break;
                case KOp::Select: s.append("(("); te(nd.c); s.append(" != 0.0) ? "); te(nd.a); s.append(" : "); te(nd.b); s.append(")"); break;
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
                case KOp::Round: s.append("roundEven("); te(nd.a); s.append(")"); break;
                case KOp::Sign: s.append("(("); te(nd.a); s.append(" > 0.0) ? 1.0 : (("); te(nd.a); s.append(" < 0.0) ? -1.0 : 0.0))"); break;
                default: break;
                }
            }
        }
        s.append(";\n");
    }
    s.append("  return e"); app_uint(s, output); s.append(";\n}\n");
}

// FUSED GEMM+epilogue GLSL kernel: naive matmul (one thread per C[m,n], `precise` = bit-exact GEMM) + the epilogue
// applied in the store (`epi(acc, bias[nn])`) — the fusion crush inherited by Vulkan. Buffers A(0) B(1) bias..(2..) C.
inline bool emit_contract_fused_glsl(const KGraph& g, int output, int contract, const FuseInfo& fi, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    const KNode& c = g.node(contract);
    if (c.op != KOp::Contract || g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2 + fi.n_bias;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;
    for (int j = 0; j < fi.n_bias; ++j) { out.input_iidx[2 + j] = fi.bias_iidx[j]; }
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BA { float A[]; };\n");
    s.append("layout(std430, binding = 1) readonly buffer BB { float Bm[]; };\n");
    for (int j = 0; j < fi.n_bias; ++j) { s.append("layout(std430, binding = "); glsl_detail::app_uint(s, 2 + j); s.append(") readonly buffer BBias"); glsl_detail::app_uint(s, j); s.append(" { float bias"); glsl_detail::app_uint(s, j); s.append("[]; };\n"); }
    s.append("layout(std430, binding = "); glsl_detail::app_uint(s, 2 + fi.n_bias); s.append(") writeonly buffer BC { float C[]; };\n");
    s.append("layout(push_constant) uniform PC { uint M; uint N; uint K; };\n");
    emit_epi_clike(g, output, fi, scratch, s);
    s.append("void main() {\n  uint gid = gl_GlobalInvocationID.x;\n  if (gid >= M * N) { return; }\n");
    s.append("  uint m = gid / N; uint nn = gid % N;\n");
    s.append("  precise float acc = 0.0;\n  for (uint k = 0u; k < K; ++k) { precise float prod = A[m * K + k] * Bm[k * N + nn]; acc = acc + prod; }\n");
    s.append("  C[m * N + nn] = epi(acc");
    for (int j = 0; j < fi.n_bias; ++j) { s.append(", bias"); glsl_detail::app_uint(s, j); s.append("[nn]"); }
    s.append(");\n}\n");
    return true;
}

} // namespace crd::kir
