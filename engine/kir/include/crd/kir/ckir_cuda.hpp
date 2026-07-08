#pragma once

// ckir_cuda.hpp — Phase 3.1.6 v17-c: the CKIR **CUDA-C emitter** (the CUDA/PTX backend's code generator). Same three
// single-kernel shapes as the GLSL emitter — fused-elementwise / matmul / reduce — but emitted as CUDA C for NVRTC.
// Determinism lever: NVRTC compiles with `--fmad=false` (no FMA fusion) + `--prec-div=true` + `--prec-sqrt=true`
// (correctly-rounded divide/sqrt) ⇒ CUDA is bit-exact vs the CPU reference for ALL correctly-rounded ops INCLUDING
// division (Vulkan's fast divide can't — see the gotchas). Pure String production (no CUDA dep); the backend compiles
// + launches. Reuses the GLSL emitter's shared helpers + the GlslKernel carrier. ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_glsl.hpp> // GlslKernel + glsl_detail::{app_uint,app_flit,is_fusable}
#include <crd/kir/ckir_tile.hpp> // TileSchedule (the schedule IR the tiled emitter consumes)

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>

namespace crd::kir
{

namespace cuda_detail
{
inline void emit_unary(crd::containers::String& s, KOp op, int a)
{
    using glsl_detail::app_uint;
    const auto ta = [&](int id) { s.append("t"); app_uint(s, id); };
    switch (op)
    {
    case KOp::Neg: s.append("-"); ta(a); break;
    case KOp::Recip: s.append("1.0f/"); ta(a); break;
    case KOp::Abs: s.append("fabsf("); ta(a); s.append(")"); break;
    case KOp::Exp: s.append("expf("); ta(a); s.append(")"); break;
    case KOp::Log: s.append("logf("); ta(a); s.append(")"); break;
    case KOp::Sin: s.append("sinf("); ta(a); s.append(")"); break;
    case KOp::Cos: s.append("cosf("); ta(a); s.append(")"); break;
    case KOp::Sqrt: s.append("sqrtf("); ta(a); s.append(")"); break;
    case KOp::Tanh: s.append("tanhf("); ta(a); s.append(")"); break;
    case KOp::Floor: s.append("floorf("); ta(a); s.append(")"); break;
    case KOp::Ceil: s.append("ceilf("); ta(a); s.append(")"); break;
    case KOp::Trunc: s.append("truncf("); ta(a); s.append(")"); break;
    case KOp::Round: s.append("rintf("); ta(a); s.append(")"); break;
    case KOp::Sign: s.append("(("); ta(a); s.append(" > 0.0f) ? 1.0f : (("); ta(a); s.append(" < 0.0f) ? -1.0f : 0.0f))"); break;
    default: break;
    }
}
} // namespace cuda_detail

// Fused-elementwise CUDA kernel `ckir(const float* in0, ..., float* outb, unsigned n)`.
inline bool emit_elementwise_cuda(const KGraph& g, int output, crd::memory::IAllocator* scratch, GlslKernel& out)
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
    s.append("extern \"C\" __global__ void ckir(");
    for (int b = 0; b < out.n_inputs; ++b) { s.append("const float* in"); app_uint(s, b); s.append(", "); }
    s.append("float* outb, unsigned n) {\n");
    s.append("  unsigned gid = blockIdx.x * blockDim.x + threadIdx.x;\n  if (gid >= n) return;\n");
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        s.append("  float t"); app_uint(s, i); s.append(" = ");
        const auto ta = [&](int id) { s.append("t"); app_uint(s, id); };
        switch (nd.op)
        {
        case KOp::Input: s.append("in"); app_uint(s, binding_of[static_cast<crd::usize>(i)]); s.append("[gid]"); break;
        case KOp::Const: app_flit(s, nd.cval); s.append("f"); break;
        case KOp::Add: ta(nd.a); s.append(" + "); ta(nd.b); break;
        case KOp::Sub: ta(nd.a); s.append(" - "); ta(nd.b); break;
        case KOp::Mul: ta(nd.a); s.append(" * "); ta(nd.b); break;
        case KOp::Div: ta(nd.a); s.append(" / "); ta(nd.b); break;
        case KOp::Max: s.append("fmaxf("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Min: s.append("fminf("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::CmpLt: s.append("(("); ta(nd.a); s.append(" < "); ta(nd.b); s.append(") ? 1.0f : 0.0f)"); break;
        case KOp::CmpEq: s.append("(("); ta(nd.a); s.append(" == "); ta(nd.b); s.append(") ? 1.0f : 0.0f)"); break;
        case KOp::CmpLe: s.append("(("); ta(nd.a); s.append(" <= "); ta(nd.b); s.append(") ? 1.0f : 0.0f)"); break;
        case KOp::Select: s.append("(("); ta(nd.c); s.append(" != 0.0f) ? "); ta(nd.a); s.append(" : "); ta(nd.b); s.append(")"); break;
        default: cuda_detail::emit_unary(s, nd.op, nd.a); break;
        }
        s.append(";\n");
    }
    s.append("  outb[gid] = t"); app_uint(s, output); s.append(";\n}\n");
    return true;
}

// Batched-matmul CUDA kernel `ckir(const float* A, const float* Bm, float* C, uint M, uint K, uint N, uint nbatch)`.
inline bool emit_contract_cuda(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& c = g.node(output);
    if (c.op != KOp::Contract || g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("extern \"C\" __global__ void ckir(const float* A, const float* Bm, float* C, unsigned M, unsigned K, unsigned N, unsigned nbatch) {\n");
    s.append("  unsigned gid = blockIdx.x * blockDim.x + threadIdx.x;\n  unsigned mn = M * N; unsigned total = mn * nbatch;\n  if (gid >= total) return;\n");
    s.append("  unsigned b = gid / mn; unsigned rem = gid % mn; unsigned m = rem / N; unsigned nn = rem % N;\n");
    s.append("  unsigned aoff = b * M * K + m * K; unsigned boff = b * K * N + nn;\n");
    s.append("  float acc = 0.0f;\n  for (unsigned k = 0; k < K; ++k) { float prod = A[aoff + k] * Bm[boff + k * N]; acc = acc + prod; }\n");
    s.append("  C[b * mn + m * N + nn] = acc;\n}\n");
    return true;
}

// Reduce CUDA kernel `ckir(const float* A, float* O, uint nout, uint redsize)` (trailing-contiguous, sequential order).
inline bool emit_reduce_cuda(const KGraph& g, int output, GlslKernel& out)
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
    s.append("extern \"C\" __global__ void ckir(const float* A, float* O, unsigned nout, unsigned redsize) {\n");
    s.append("  unsigned o = blockIdx.x * blockDim.x + threadIdx.x;\n  if (o >= nout) return;\n  unsigned base = o * redsize;\n");
    if (rn.op == KOp::ReduceSum) { s.append("  float acc = 0.0f;\n  for (unsigned r = 0; r < redsize; ++r) { acc = acc + A[base + r]; }\n"); }
    else if (rn.op == KOp::ReduceProd) { s.append("  float acc = 1.0f;\n  for (unsigned r = 0; r < redsize; ++r) { acc = acc * A[base + r]; }\n"); }
    else if (rn.op == KOp::ReduceMax) { s.append("  float acc = A[base];\n  for (unsigned r = 1; r < redsize; ++r) { acc = fmaxf(acc, A[base + r]); }\n"); }
    else if (rn.op == KOp::ReduceMin) { s.append("  float acc = A[base];\n  for (unsigned r = 1; r < redsize; ++r) { acc = fminf(acc, A[base + r]); }\n"); }
    else if (rn.op == KOp::ArgMax) { s.append("  float bv = A[base]; unsigned bi = 0u;\n  for (unsigned r = 1; r < redsize; ++r) { if (A[base + r] > bv) { bv = A[base + r]; bi = r; } }\n  float acc = (float)bi;\n"); }
    else { s.append("  float bv = A[base]; unsigned bi = 0u;\n  for (unsigned r = 1; r < redsize; ++r) { if (A[base + r] < bv) { bv = A[base + r]; bi = r; } }\n  float acc = (float)bi;\n"); }
    s.append("  O[o] = acc;\n}\n");
    return true;
}

// Emit the T2 FAST parallel reduce (Sum/Prod/Max/Min) — one block per output, grid-stride partials + shared-mem tree.
inline bool emit_reduce_fast_cuda(const KGraph& g, int output, GlslKernel& out)
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
    s.append("extern \"C\" __global__ void ckir(const float* A, float* O, unsigned nout, unsigned redsize) {\n");
    s.append("  __shared__ float sdata[256];\n");
    s.append("  unsigned o = blockIdx.x; unsigned tid = threadIdx.x; unsigned base = o * redsize;\n");
    s.append("  float acc = "); s.append(glsl_detail::fast_init(rn.op, true));
    s.append(";\n  for (unsigned i = tid; i < redsize; i += 256u) { acc = ");
    glsl_detail::fast_comb(s, rn.op, "acc", "A[base + i]", "fmaxf", "fminf");
    s.append("; }\n  sdata[tid] = acc;\n  __syncthreads();\n");
    s.append("  for (unsigned sh = 128u; sh > 0u; sh >>= 1) { if (tid < sh) { sdata[tid] = ");
    glsl_detail::fast_comb(s, rn.op, "sdata[tid]", "sdata[tid + sh]", "fmaxf", "fminf");
    s.append("; } __syncthreads(); }\n");
    s.append("  if (tid == 0u) { O[o] = sdata[0]; }\n}\n");
    return true;
}

// Emit a GATHER kernel — out[m, ...] = data[idx[m], ...] (row-gather along axis 0). Data + idx are Input leaves; idx
// holds f32-encoded integer indices (exact for indices < 2^24). One thread per output element. ADR-0098.
inline bool emit_gather_cuda(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& gn = g.node(output);
    if (gn.op != KOp::Gather || g.node(gn.a).op != KOp::Input || g.node(gn.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(gn.a).iidx; // data
    out.input_iidx[1] = g.node(gn.b).iidx; // idx
    crd::containers::String& s = out.source;
    s.clear();
    s.append("extern \"C\" __global__ void ckir(const float* A, const float* I, float* O, unsigned nout, unsigned rowsize) {\n");
    s.append("  unsigned o = blockIdx.x * blockDim.x + threadIdx.x;\n  if (o >= nout) return;\n");
    s.append("  unsigned m = o / rowsize; unsigned c = o % rowsize;\n");
    s.append("  unsigned r = (unsigned)(int)I[m];\n");
    s.append("  O[o] = A[r * rowsize + c];\n}\n");
    return true;
}

// Emit a SCATTER kernel — out=base, then out[idx[m],...]=updates[m,...] (LAST-WINS, output-centric so it's race-free +
// deterministic in ONE dispatch). base+idx+updates are Input leaves. One thread per output element scans the M indices.
inline bool emit_scatter_cuda(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::Scatter || g.node(sn.a).op != KOp::Input || g.node(sn.b).op != KOp::Input || g.node(sn.c).op != KOp::Input) { return false; }
    out.n_inputs      = 3;
    out.input_iidx[0] = g.node(sn.a).iidx; // base
    out.input_iidx[1] = g.node(sn.b).iidx; // idx
    out.input_iidx[2] = g.node(sn.c).iidx; // updates
    crd::containers::String& s = out.source;
    s.clear();
    s.append("extern \"C\" __global__ void ckir(const float* B, const float* I, const float* U, float* O, unsigned nout, unsigned rowsize, unsigned M) {\n");
    s.append("  unsigned o = blockIdx.x * blockDim.x + threadIdx.x;\n  if (o >= nout) return;\n");
    s.append("  unsigned r = o / rowsize; unsigned c = o % rowsize;\n");
    s.append("  float result = B[o];\n");
    s.append("  for (unsigned m = 0; m < M; ++m) { if ((unsigned)(int)I[m] == r) { result = U[m * rowsize + c]; } }\n");
    s.append("  O[o] = result;\n}\n");
    return true;
}

// Emit an inclusive SCAN (prefix-sum) kernel along the trailing axis — one thread per row does the sequential scan
// (fixed order ⇒ bit-exact vs the CPU oracle). nrows = numel/scanlen independent rows. ADR-0098.
inline bool emit_scan_cuda(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::ScanSum || g.node(sn.a).op != KOp::Input) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(sn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("extern \"C\" __global__ void ckir(const float* A, float* O, unsigned nrows, unsigned scanlen) {\n");
    s.append("  unsigned row = blockIdx.x * blockDim.x + threadIdx.x;\n  if (row >= nrows) return;\n");
    s.append("  unsigned base = row * scanlen;\n  float acc = 0.0f;\n");
    s.append("  for (unsigned c = 0; c < scanlen; ++c) { acc = acc + A[base + c]; O[base + c] = acc; }\n}\n");
    return true;
}

// T2 FAST parallel prefix-sum (one block per row): per-thread chunk scan + serial chunk-total scan + prefix add.
inline bool emit_scan_fast_cuda(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::ScanSum || sn.tier != DetTier::Fast || g.node(sn.a).op != KOp::Input) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(sn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("extern \"C\" __global__ void ckir(const float* A, float* O, unsigned nrows, unsigned scanlen) {\n");
    s.append("  __shared__ float ctot[256];\n");
    s.append("  unsigned row = blockIdx.x; unsigned tid = threadIdx.x; unsigned base = row * scanlen;\n");
    s.append("  unsigned C = (scanlen + 255u) / 256u; unsigned lo = tid * C; unsigned hi = (lo + C < scanlen) ? (lo + C) : scanlen;\n");
    s.append("  float acc = 0.0f;\n  for (unsigned i = lo; i < hi; ++i) { acc = acc + A[base + i]; O[base + i] = acc; }\n");
    s.append("  ctot[tid] = acc;\n  __syncthreads();\n");
    s.append("  if (tid == 0u) { float run = 0.0f; for (unsigned t = 0u; t < 256u; ++t) { float v = ctot[t]; ctot[t] = run; run = run + v; } }\n");
    s.append("  __syncthreads();\n  float prefix = ctot[tid];\n");
    s.append("  for (unsigned i = lo; i < hi; ++i) { O[base + i] = O[base + i] + prefix; }\n}\n");
    return true;
}

// Emit the WARP-TILED matmul kernel (CUTLASS block→warp→thread hierarchy) for a `WarpTiled` schedule — the v17-e crush
// kernel, now a *compiler output* rather than a benchmark. Signature `ckir(A, Bm, C, uint M, uint N, uint K)`, grid =
// (N/BN, M/BM), block = NT. Transposed padded shared-A (BMP=BM+4, conflict-free vectorizable regM read) + optional
// two-stage smem double-buffer (DB) + FMA fast tier / no-FMA exact tier (EXACT, bit-matches the oracle). The schedule
// ints become `#define`s (NVRTC folds them); the body below is the faithful transcription of the verified kernel.
inline bool emit_contract_tiled_cuda(const KGraph& g, int output, const TileSchedule& sch, GlslKernel& out)
{
    const KNode& c = g.node(output);
    if (sch.kind != Sched::WarpTiled) { return false; }
    if (c.op != KOp::Contract || g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    const auto def = [&](const char* nm, int v) {
        s.append("#define ");
        s.append(nm);
        s.append(" ");
        glsl_detail::app_uint(s, static_cast<crd::u32>(v));
        s.append("\n");
    };
    def("BM", sch.bm);
    def("BN", sch.bn);
    def("BK", sch.bk);
    def("WM", sch.wm);
    def("WN", sch.wn);
    def("WNITER", sch.wniter);
    def("TM", sch.tm);
    def("TN", sch.tn);
    def("NT", sch.nt);
    def("DB", sch.double_buffer ? 1 : 0);
    def("EXACT", sch.fma ? 0 : 1);
    s.append(R"CKIR(
#define BMP (BM + 4)
#define WMITER ((WM * WN) / (32 * TM * TN * WNITER))
#define WSUBM (WM / WMITER)
#define WSUBN (WN / WNITER)
#define SSA (BK * BMP)
#define SSB (BK * BN)
#define STRIDEA ((NT * 4) / BK)
#define STRIDEB (NT / (BN / 4))
#define NA (BM / STRIDEA)
#define NB (BK / STRIDEB)
#if EXACT
#define ACCUM(o, x, y) acc[o] = __fadd_rn(acc[o], __fmul_rn((x), (y)))
#else
#define ACCUM(o, x, y) acc[o] += (x) * (y)
#endif
extern "C" __global__ void __launch_bounds__(NT) ckir(const float* A, const float* Bm, float* C, unsigned M, unsigned N, unsigned K) {
  (void)M;
  const unsigned cRow = blockIdx.y, cCol = blockIdx.x;
  const unsigned warpIdx = threadIdx.x / 32u;
  const unsigned warpCol = warpIdx % (BN / WN);
  const unsigned warpRow = warpIdx / (BN / WN);
  const unsigned tiw = threadIdx.x % 32u;
  const unsigned threadColInWarp = tiw % (WSUBN / TN);
  const unsigned threadRowInWarp = tiw / (WSUBN / TN);
#if DB
  __shared__ float As[2 * SSA];
  __shared__ float Bs[2 * SSB];
#else
  __shared__ float As[SSA];
  __shared__ float Bs[SSB];
#endif
  A += cRow * BM * K;
  Bm += cCol * BN;
  const unsigned cBaseCol = cCol * BN + warpCol * WN;
  C += (cRow * BM + warpRow * WM) * N + cBaseCol;
  const unsigned irA = threadIdx.x / (BK / 4), icA = threadIdx.x % (BK / 4);
  const unsigned irB = threadIdx.x / (BN / 4), icB = threadIdx.x % (BN / 4);
  float acc[WMITER * TM * WNITER * TN] = {0.0f};
  float regM[WMITER * TM];
  float regN[WNITER * TN];
  float4 pa[NA];
  float4 pb[NB];
  auto fetchA = [&](const float* Ag) {
    #pragma unroll
    for (int ia = 0; ia < NA; ++ia) pa[ia] = *reinterpret_cast<const float4*>(&Ag[(irA + ia * STRIDEA) * K + icA * 4]);
  };
  auto fetchB = [&](const float* Bg) {
    #pragma unroll
    for (int ib = 0; ib < NB; ++ib) pb[ib] = *reinterpret_cast<const float4*>(&Bg[(irB + ib * STRIDEB) * N + icB * 4]);
  };
  auto commit = [&](float* AsS, float* BsS) {
    #pragma unroll
    for (int ia = 0; ia < NA; ++ia) {
      const unsigned row = irA + ia * STRIDEA;
      AsS[(icA * 4 + 0) * BMP + row] = pa[ia].x;
      AsS[(icA * 4 + 1) * BMP + row] = pa[ia].y;
      AsS[(icA * 4 + 2) * BMP + row] = pa[ia].z;
      AsS[(icA * 4 + 3) * BMP + row] = pa[ia].w;
    }
    #pragma unroll
    for (int ib = 0; ib < NB; ++ib) *reinterpret_cast<float4*>(&BsS[(irB + ib * STRIDEB) * BN + icB * 4]) = pb[ib];
  };
  auto compute = [&](const float* AsS, const float* BsS) {
    #pragma unroll
    for (int dot = 0; dot < BK; ++dot) {
      #pragma unroll
      for (int wm = 0; wm < WMITER; ++wm)
        #pragma unroll
        for (int i = 0; i < TM; ++i)
          regM[wm * TM + i] = AsS[dot * BMP + warpRow * WM + wm * WSUBM + threadRowInWarp * TM + i];
      #pragma unroll
      for (int wn = 0; wn < WNITER; ++wn)
        #pragma unroll
        for (int i = 0; i < TN; ++i)
          regN[wn * TN + i] = BsS[dot * BN + warpCol * WN + wn * WSUBN + threadColInWarp * TN + i];
      #pragma unroll
      for (int wm = 0; wm < WMITER; ++wm)
        #pragma unroll
        for (int wn = 0; wn < WNITER; ++wn)
          #pragma unroll
          for (int m = 0; m < TM; ++m)
            #pragma unroll
            for (int n = 0; n < TN; ++n) {
              const int o = (wm * TM + m) * (WNITER * TN) + wn * TN + n;
              ACCUM(o, regM[wm * TM + m], regN[wn * TN + n]);
            }
    }
  };
#if DB
  fetchA(A); fetchB(Bm); commit(As, Bs);
  __syncthreads();
  int cur = 0;
  for (int bk = 0; bk < (int)K; bk += BK) {
    const bool has_next = bk + BK < (int)K;
    if (has_next) { fetchA(A + BK); fetchB(Bm + BK * N); }
    compute(As + cur * SSA, Bs + cur * SSB);
    if (has_next) {
      commit(As + (cur ^ 1) * SSA, Bs + (cur ^ 1) * SSB);
      __syncthreads();
      cur ^= 1;
      A += BK; Bm += BK * N;
    }
  }
#else
  for (int bk = 0; bk < (int)K; bk += BK) {
    fetchA(A); fetchB(Bm); commit(As, Bs);
    __syncthreads();
    compute(As, Bs);
    A += BK; Bm += BK * N;
    __syncthreads();
  }
#endif
  #pragma unroll
  for (int wm = 0; wm < WMITER; ++wm)
    #pragma unroll
    for (int wn = 0; wn < WNITER; ++wn) {
      float* Cw = C + (wm * WSUBM) * N + wn * WSUBN;
      #pragma unroll
      for (int m = 0; m < TM; ++m)
        #pragma unroll
        for (int n = 0; n < TN; n += 4) {
          float4 v;
          v.x = acc[(wm * TM + m) * (WNITER * TN) + wn * TN + n + 0];
          v.y = acc[(wm * TM + m) * (WNITER * TN) + wn * TN + n + 1];
          v.z = acc[(wm * TM + m) * (WNITER * TN) + wn * TN + n + 2];
          v.w = acc[(wm * TM + m) * (WNITER * TN) + wn * TN + n + 3];
          *reinterpret_cast<float4*>(&Cw[(threadRowInWarp * TM + m) * N + threadColInWarp * TN + n]) = v;
        }
    }
}
)CKIR");
    return true;
}

// Emit the epilogue as a `__device__` function `epi(float acc, float b0, ...)` — the elementwise cone with the Contract
// mapped to `acc` and each bias Broadcast to its `b{j}` param. Walks reachable nodes in ascending (topological) order.
inline void emit_epi_fn(const KGraph& g, int output, const FuseInfo& fi, crd::memory::IAllocator* scratch, crd::containers::String& s)
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
        if (nd.op == KOp::Contract || nd.op == KOp::Broadcast) { continue; } // leaves
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
    }
    s.append("__device__ __forceinline__ float epi(float acc");
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
                case KOp::Const: app_flit(s, nd.cval); s.append("f"); break;
                case KOp::Add: te(nd.a); s.append(" + "); te(nd.b); break;
                case KOp::Sub: te(nd.a); s.append(" - "); te(nd.b); break;
                case KOp::Mul: te(nd.a); s.append(" * "); te(nd.b); break;
                case KOp::Div: te(nd.a); s.append(" / "); te(nd.b); break;
                case KOp::Max: s.append("fmaxf("); te(nd.a); s.append(", "); te(nd.b); s.append(")"); break;
                case KOp::Min: s.append("fminf("); te(nd.a); s.append(", "); te(nd.b); s.append(")"); break;
                case KOp::CmpLt: s.append("(("); te(nd.a); s.append(" < "); te(nd.b); s.append(") ? 1.0f : 0.0f)"); break;
                case KOp::CmpEq: s.append("(("); te(nd.a); s.append(" == "); te(nd.b); s.append(") ? 1.0f : 0.0f)"); break;
                case KOp::CmpLe: s.append("(("); te(nd.a); s.append(" <= "); te(nd.b); s.append(") ? 1.0f : 0.0f)"); break;
                case KOp::Select: s.append("(("); te(nd.c); s.append(" != 0.0f) ? "); te(nd.a); s.append(" : "); te(nd.b); s.append(")"); break;
                case KOp::Neg: s.append("-"); te(nd.a); break;
                case KOp::Recip: s.append("1.0f/"); te(nd.a); break;
                case KOp::Abs: s.append("fabsf("); te(nd.a); s.append(")"); break;
                case KOp::Exp: s.append("expf("); te(nd.a); s.append(")"); break;
                case KOp::Log: s.append("logf("); te(nd.a); s.append(")"); break;
                case KOp::Sin: s.append("sinf("); te(nd.a); s.append(")"); break;
                case KOp::Cos: s.append("cosf("); te(nd.a); s.append(")"); break;
                case KOp::Sqrt: s.append("sqrtf("); te(nd.a); s.append(")"); break;
                case KOp::Tanh: s.append("tanhf("); te(nd.a); s.append(")"); break;
                case KOp::Floor: s.append("floorf("); te(nd.a); s.append(")"); break;
                case KOp::Ceil: s.append("ceilf("); te(nd.a); s.append(")"); break;
                case KOp::Trunc: s.append("truncf("); te(nd.a); s.append(")"); break;
                case KOp::Round: s.append("rintf("); te(nd.a); s.append(")"); break;
                case KOp::Sign: s.append("(("); te(nd.a); s.append(" > 0.0f) ? 1.0f : (("); te(nd.a); s.append(" < 0.0f) ? -1.0f : 0.0f))"); break;
                default: break;
                }
            }
        }
        s.append(";\n");
    }
    s.append("  return e"); app_uint(s, output); s.append(";\n}\n");
}

// Emit the FUSED warp-tiled GEMM+epilogue kernel: same compute as `emit_contract_tiled_cuda`, but with bias params and
// the epilogue applied in the C write (bias+activation lands in registers before the store — no extra VRAM round-trip
// = the structural crush the vendor can't match when the activation is off its epilogue menu). `contract` is the
// Contract node (its inputs are A,B); `fi` names the bias buffers. Signature `ckir(A,Bm,C, bias0.., M,N,K)`.
inline bool emit_contract_tiled_fused_cuda(const KGraph& g, int output, int contract, const TileSchedule& sch,
                                           const FuseInfo& fi, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    const KNode& c = g.node(contract);
    if (sch.kind != Sched::WarpTiled || c.op != KOp::Contract) { return false; }
    if (g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2 + fi.n_bias;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;
    for (int j = 0; j < fi.n_bias; ++j) { out.input_iidx[2 + j] = fi.bias_iidx[j]; }

    crd::containers::String& s = out.source;
    s.clear();
    const auto def = [&](const char* nm, int v) { s.append("#define "); s.append(nm); s.append(" "); glsl_detail::app_uint(s, static_cast<crd::u32>(v)); s.append("\n"); };
    def("BM", sch.bm); def("BN", sch.bn); def("BK", sch.bk); def("WM", sch.wm); def("WN", sch.wn);
    def("WNITER", sch.wniter); def("TM", sch.tm); def("TN", sch.tn); def("NT", sch.nt);
    def("DB", sch.double_buffer ? 1 : 0); def("EXACT", sch.fma ? 0 : 1);
    s.append(R"CKIR(
#define BMP (BM + 4)
#define WMITER ((WM * WN) / (32 * TM * TN * WNITER))
#define WSUBM (WM / WMITER)
#define WSUBN (WN / WNITER)
#define SSA (BK * BMP)
#define SSB (BK * BN)
#define STRIDEA ((NT * 4) / BK)
#define STRIDEB (NT / (BN / 4))
#define NA (BM / STRIDEA)
#define NB (BK / STRIDEB)
#if EXACT
#define ACCUM(o, x, y) acc[o] = __fadd_rn(acc[o], __fmul_rn((x), (y)))
#else
#define ACCUM(o, x, y) acc[o] += (x) * (y)
#endif
)CKIR");
    emit_epi_fn(g, output, fi, scratch, s);
    s.append("extern \"C\" __global__ void __launch_bounds__(NT) ckir(const float* A, const float* Bm, float* C");
    for (int j = 0; j < fi.n_bias; ++j) { s.append(", const float* bias"); glsl_detail::app_uint(s, j); }
    s.append(", unsigned M, unsigned N, unsigned K) {\n");
    s.append(R"CKIR(  (void)M;
  const unsigned cRow = blockIdx.y, cCol = blockIdx.x;
  const unsigned warpIdx = threadIdx.x / 32u;
  const unsigned warpCol = warpIdx % (BN / WN);
  const unsigned warpRow = warpIdx / (BN / WN);
  const unsigned tiw = threadIdx.x % 32u;
  const unsigned threadColInWarp = tiw % (WSUBN / TN);
  const unsigned threadRowInWarp = tiw / (WSUBN / TN);
#if DB
  __shared__ float As[2 * SSA];
  __shared__ float Bs[2 * SSB];
#else
  __shared__ float As[SSA];
  __shared__ float Bs[SSB];
#endif
  A += cRow * BM * K;
  Bm += cCol * BN;
  const unsigned cBaseCol = cCol * BN + warpCol * WN;
  C += (cRow * BM + warpRow * WM) * N + cBaseCol;
  const unsigned irA = threadIdx.x / (BK / 4), icA = threadIdx.x % (BK / 4);
  const unsigned irB = threadIdx.x / (BN / 4), icB = threadIdx.x % (BN / 4);
  float acc[WMITER * TM * WNITER * TN] = {0.0f};
  float regM[WMITER * TM];
  float regN[WNITER * TN];
  float4 pa[NA];
  float4 pb[NB];
  auto fetchA = [&](const float* Ag) {
    #pragma unroll
    for (int ia = 0; ia < NA; ++ia) pa[ia] = *reinterpret_cast<const float4*>(&Ag[(irA + ia * STRIDEA) * K + icA * 4]);
  };
  auto fetchB = [&](const float* Bg) {
    #pragma unroll
    for (int ib = 0; ib < NB; ++ib) pb[ib] = *reinterpret_cast<const float4*>(&Bg[(irB + ib * STRIDEB) * N + icB * 4]);
  };
  auto commit = [&](float* AsS, float* BsS) {
    #pragma unroll
    for (int ia = 0; ia < NA; ++ia) {
      const unsigned row = irA + ia * STRIDEA;
      AsS[(icA * 4 + 0) * BMP + row] = pa[ia].x;
      AsS[(icA * 4 + 1) * BMP + row] = pa[ia].y;
      AsS[(icA * 4 + 2) * BMP + row] = pa[ia].z;
      AsS[(icA * 4 + 3) * BMP + row] = pa[ia].w;
    }
    #pragma unroll
    for (int ib = 0; ib < NB; ++ib) *reinterpret_cast<float4*>(&BsS[(irB + ib * STRIDEB) * BN + icB * 4]) = pb[ib];
  };
  auto compute = [&](const float* AsS, const float* BsS) {
    #pragma unroll
    for (int dot = 0; dot < BK; ++dot) {
      #pragma unroll
      for (int wm = 0; wm < WMITER; ++wm)
        #pragma unroll
        for (int i = 0; i < TM; ++i)
          regM[wm * TM + i] = AsS[dot * BMP + warpRow * WM + wm * WSUBM + threadRowInWarp * TM + i];
      #pragma unroll
      for (int wn = 0; wn < WNITER; ++wn)
        #pragma unroll
        for (int i = 0; i < TN; ++i)
          regN[wn * TN + i] = BsS[dot * BN + warpCol * WN + wn * WSUBN + threadColInWarp * TN + i];
      #pragma unroll
      for (int wm = 0; wm < WMITER; ++wm)
        #pragma unroll
        for (int wn = 0; wn < WNITER; ++wn)
          #pragma unroll
          for (int mi = 0; mi < TM; ++mi)
            #pragma unroll
            for (int ni = 0; ni < TN; ++ni) {
              const int o = (wm * TM + mi) * (WNITER * TN) + wn * TN + ni;
              ACCUM(o, regM[wm * TM + mi], regN[wn * TN + ni]);
            }
    }
  };
#if DB
  fetchA(A); fetchB(Bm); commit(As, Bs);
  __syncthreads();
  int cur = 0;
  for (int bk = 0; bk < (int)K; bk += BK) {
    const bool has_next = bk + BK < (int)K;
    if (has_next) { fetchA(A + BK); fetchB(Bm + BK * N); }
    compute(As + cur * SSA, Bs + cur * SSB);
    if (has_next) {
      commit(As + (cur ^ 1) * SSA, Bs + (cur ^ 1) * SSB);
      __syncthreads();
      cur ^= 1;
      A += BK; Bm += BK * N;
    }
  }
#else
  for (int bk = 0; bk < (int)K; bk += BK) {
    fetchA(A); fetchB(Bm); commit(As, Bs);
    __syncthreads();
    compute(As, Bs);
    A += BK; Bm += BK * N;
    __syncthreads();
  }
#endif
  #pragma unroll
  for (int wm = 0; wm < WMITER; ++wm)
    #pragma unroll
    for (int wn = 0; wn < WNITER; ++wn) {
      float* Cw = C + (wm * WSUBM) * N + wn * WSUBN;
      const unsigned gcol = cBaseCol + wn * WSUBN + threadColInWarp * TN;
      #pragma unroll
      for (int m = 0; m < TM; ++m)
        #pragma unroll
        for (int n = 0; n < TN; n += 4) {
          const int base = (wm * TM + m) * (WNITER * TN) + wn * TN + n;
)CKIR");
    for (int j = 0; j < fi.n_bias; ++j) { s.append("          float4 B"); glsl_detail::app_uint(s, j); s.append(" = *reinterpret_cast<const float4*>(&bias"); glsl_detail::app_uint(s, j); s.append("[gcol + n]);\n"); }
    s.append("          float4 v;\n");
    const char* comp[4] = {"x", "y", "z", "w"};
    for (int k = 0; k < 4; ++k)
    {
        s.append("          v."); s.append(comp[k]); s.append(" = epi(acc[base + "); glsl_detail::app_uint(s, static_cast<crd::u32>(k)); s.append("]");
        for (int j = 0; j < fi.n_bias; ++j) { s.append(", B"); glsl_detail::app_uint(s, j); s.append("."); s.append(comp[k]); }
        s.append(");\n");
    }
    s.append(R"CKIR(          *reinterpret_cast<float4*>(&Cw[(threadRowInWarp * TM + m) * N + threadColInWarp * TN + n]) = v;
        }
    }
}
)CKIR");
    return true;
}

} // namespace crd::kir
