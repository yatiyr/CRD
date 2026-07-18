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
        // every temp here is float (bool lowers to 0.0f/1.0f), so a Cast is an explicit float conversion.
        case KOp::Cast: s.append("float("); ta(nd.a); s.append(")"); break;
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

// B-cmp: emit an IMPERATIVE compute KERNEL (shared memory + barriers — `KEntry.is_kernel()`) as CUDA C++ (`.cu`, NVRTC →
// PTX). Mirror of emit_compute_kernel_glsl. Storage buffers become TYPED raw pointer params (`float*`/`unsigned*`/`int*`,
// binding order — CUDA has typed pointers, unlike HLSL's raw UAV); shared arrays are `__shared__`; the barrier is
// `__syncthreads()`; LocalInvocationIndex is `threadIdx.x` (1-D workgroup). CUDA is the ONE backend that can be
// CPU-oracle bit-exact (`--fmad=false --prec-div=true`) once dispatched on real hardware (ADR-0098 Part C).
inline bool emit_compute_kernel_cuda(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    if (!entry.is_kernel()) { return false; }
    const int                n = g.size();
    crd::containers::String& s = out.source;
    s.clear();
    out.n_inputs = 0;

    const auto cty = [](DType d) -> const char* { if (dt_is_uint(d)) { return "unsigned"; } return dt_is_int(d) ? "int" : "float"; };
    crd::containers::Array<crd::u8> matd(scratch); // Materialized (frozen) nodes emit a `t<node>` reference, not their inline expr
    matd.resize(static_cast<crd::usize>(n), 0);

    s.append("extern \"C\" __global__ void ckir(");
    bool first = true;
    for (int i = 0; i < n; ++i)
    {
        const KNode& nd = g.node(i);
        if (nd.op == KOp::BufferDecl)
        {
            if (!first) { s.append(", "); }
            first = false;
            if (nd.axes == 3U) { s.append("volatile "); } // COHERENT buffer (lookback/spin cells): defeat load hoisting
            s.append(cty(nd.dtype())); s.append("* buf"); app_uint(s, nd.iidx);
        }
    }
    s.append(") {\n");
    for (int i = 0; i < n; ++i) // __shared__ arrays
    {
        const KNode& nd = g.node(i);
        if (nd.op == KOp::SharedDecl)
        {
            s.append("  __shared__ "); s.append(cty(nd.dtype())); s.append(" sh"); app_uint(s, i);
            s.append("["); app_uint(s, nd.iidx + static_cast<int>(nd.axes)); s.append("];\n");
        }
    }

    bool       ok = true;
    const auto ev = [&](auto&& self, int node) -> void {
        if (matd[static_cast<crd::usize>(node)] != 0U) { s.append("t"); app_uint(s, static_cast<crd::u32>(node)); return; }
        const KNode& nd  = g.node(node);
        const auto   bin = [&](const char* o) { s.append("("); self(self, nd.a); s.append(o); self(self, nd.b); s.append(")"); };
        const auto   fn2 = [&](const char* f) { s.append(f); s.append("("); self(self, nd.a); s.append(", "); self(self, nd.b); s.append(")"); };
        const auto   fn1 = [&](const char* f) { s.append(f); s.append("("); self(self, nd.a); s.append(")"); };
        switch (nd.op)
        {
        case KOp::Const:
            // %lld (full 64-bit) via app_int_const — NOT static_cast<int> (MSVC clamps a u32 const > INT_MAX to INT_MIN).
            if (dt_is_uint(nd.dtype()) || dt_is_int(nd.dtype())) { app_int_const(s, nd.cval, nd.dtype()); }
            else { app_flit(s, nd.cval); s.append("f"); }
            break;
        case KOp::Builtin:
            if (static_cast<KBuiltin>(nd.iidx) == KBuiltin::LocalInvocationIndex) { s.append("threadIdx.x"); }
            else if (static_cast<KBuiltin>(nd.iidx) == KBuiltin::WorkgroupIndex) { s.append("blockIdx.x"); }
            else { ok = false; s.append("0u"); }
            break;
        case KOp::KernelLoopVar: s.append("lv"); app_uint(s, nd.a); break;
        case KOp::BufferLoad: s.append("buf"); app_uint(s, g.node(nd.a).iidx); s.append("["); self(self, nd.b); s.append("]"); break;
        case KOp::SharedLoad: s.append("sh"); app_uint(s, nd.a); s.append("["); self(self, nd.b); s.append("]"); break;
        case KOp::Cast: s.append(dt_is_uint(nd.dtype()) ? "(unsigned)(" : dt_is_int(nd.dtype()) ? "(int)(" : "float("); self(self, nd.a); s.append(")"); break; // NOLINT(readability-avoid-nested-conditional-operator) 3-way cast-syntax select
        case KOp::Neg: s.append("(-"); self(self, nd.a); s.append(")"); break;
        case KOp::Abs: fn1("fabsf"); break;
        case KOp::Sqrt: fn1("sqrtf"); break;
        case KOp::Sin: fn1("sinf"); break;
        case KOp::Cos: fn1("cosf"); break;
        case KOp::Exp: fn1("expf"); break;
        case KOp::Pow: fn2("powf"); break;
        case KOp::Log: fn1("logf"); break;
        case KOp::Log2: fn1("log2f"); break;
        case KOp::Tanh: fn1("tanhf"); break;
        case KOp::Atan2: fn2("atan2f"); break;
        case KOp::Atan: fn1("atanf"); break;
        case KOp::Asin: fn1("asinf"); break;
        case KOp::Acos: fn1("acosf"); break;
        case KOp::Sinh: fn1("sinhf"); break;
        case KOp::Cosh: fn1("coshf"); break;
        case KOp::Floor: fn1("floorf"); break;
        case KOp::Add: bin(" + "); break;
        case KOp::Sub: bin(" - "); break;
        case KOp::Mul: bin(" * "); break;
        case KOp::Div: bin(" / "); break;
        case KOp::Min: fn2("fminf"); break;
        case KOp::Max: fn2("fmaxf"); break;
        case KOp::Mod: if (dt_is_int(nd.dtype()) || dt_is_uint(nd.dtype())) { bin(" % "); } else { fn2("fmodf"); } break;
        case KOp::CmpLt: bin(" < "); break;
        case KOp::CmpLe: bin(" <= "); break;
        case KOp::CmpGt: bin(" > "); break;
        case KOp::CmpGe: bin(" >= "); break;
        case KOp::CmpEq: bin(" == "); break;
        case KOp::CmpNe: bin(" != "); break;
        case KOp::BitAnd: bin(" & "); break;
        case KOp::BitOr: bin(" | "); break;
        case KOp::BitXor: bin(" ^ "); break;
        case KOp::Shl: bin(" << "); break;
        case KOp::Shr: bin(" >> "); break;
        case KOp::Fma: s.append("fmaf("); self(self, nd.a); s.append(", "); self(self, nd.b); s.append(", "); self(self, nd.c); s.append(")"); break;
        case KOp::Select: s.append("(("); self(self, nd.c); s.append(") ? "); self(self, nd.a); s.append(" : "); self(self, nd.b); s.append(")"); break; // a=true b=false c=cond
        case KOp::BitNot: s.append("(~"); self(self, nd.a); s.append(")"); break;
        case KOp::BitCount: s.append("__popc("); self(self, nd.a); s.append(")"); break;
        // subgroup (warp) ops — CUDA's NATIVE forms: full-mask sync variants; the 32-lane subgroup is the hardware warp.
        case KOp::SubgroupBallot: s.append("__ballot_sync(0xffffffffu, ("); self(self, nd.a); s.append(") != 0u)"); break;
        case KOp::SubgroupBallotExclCount: s.append("__popc(("); self(self, nd.a); s.append(") & ((1u << (threadIdx.x & 31u)) - 1u))"); break;
        case KOp::SubgroupMatch: s.append("__match_any_sync(0xffffffffu, "); self(self, nd.a); s.append(")"); break; // hardware match
        default: ok = false; s.append("0"); break;
        }
    };
    // CSE (matches the GLSL/HLSL emitters): materialize EVERY non-inline arithmetic node as a `t<node>` temp keyed by node id, so
    // a shared subtree emits ONCE. Without this the recursive `ev` inline-expands a value referenced M times M-fold — a DEEP shared
    // value DAG (e.g. the B15-b Perlin-Worley cloud density: perlin FBM + Burtle-Jenkins hash) then explodes EXPONENTIALLY (OOM).
    // LEAVES + cast/select/compare/bitops stay INLINE. Determinism is unaffected — CUDA already compiles `--fmad=false`.
    const auto is_inline_op = [](KOp op) -> bool {
        switch (op)
        {
        case KOp::Const: case KOp::Builtin: case KOp::KernelLoopVar: case KOp::BufferLoad: case KOp::SharedLoad:
        case KOp::BufferDecl: case KOp::SharedDecl: case KOp::Cast: case KOp::Select:
        case KOp::CmpLt: case KOp::CmpLe: case KOp::CmpGt: case KOp::CmpGe: case KOp::CmpEq: case KOp::CmpNe:
        case KOp::BitAnd: case KOp::BitOr: case KOp::BitXor: case KOp::Shl: case KOp::Shr: return true;
        default: return false;
        }
    };
    const auto decl = [&](auto&& self, int node) -> void {
        const KNode& nd = g.node(node);
        if (nd.op == KOp::BufferLoad || nd.op == KOp::SharedLoad) { self(self, nd.b); return; } // resource leaf: only the index carries temps
        if (nd.a >= 0) { self(self, nd.a); }
        if (nd.b >= 0) { self(self, nd.b); }
        if (nd.c >= 0) { self(self, nd.c); }
        if (!is_inline_op(nd.op) && matd[static_cast<crd::usize>(node)] == 0U)
        {
            s.append("  "); s.append(cty(nd.dtype())); s.append(" t"); app_uint(s, static_cast<crd::u32>(node)); s.append(" = ");
            ev(ev, node); // matd[node] still 0 ⇒ emits the one-level expr (children already materialized ⇒ temp refs)
            s.append(";\n");
            matd[static_cast<crd::usize>(node)] = 1U;
        }
    };
    const auto emit_body = [&](auto&& self_b, int begin, int count) -> void {
        int i = begin;
        while (i < begin + count) // a For/If body lives CONTIGUOUSLY after it → recurse then SKIP past it (never re-emit)
        {
            const KStmt& st = g.stmt(i);
            switch (st.kind)
            {
            case KStmtKind::BufferStore: decl(decl, st.index); decl(decl, st.value); s.append("  buf"); app_uint(s, g.node(st.target).iidx); s.append("["); ev(ev, st.index); s.append("] = "); ev(ev, st.value); s.append(";\n"); ++i; break;
            case KStmtKind::SharedStore: decl(decl, st.index); decl(decl, st.value); s.append("  sh"); app_uint(s, st.target); s.append("["); ev(ev, st.index); s.append("] = "); ev(ev, st.value); s.append(";\n"); ++i; break;
            case KStmtKind::Barrier: if (st.scope == BarrierScope::Buffer) { s.append("  __threadfence();\n"); } s.append("  __syncthreads();\n"); ++i; break;
            case KStmtKind::Materialize: // FREEZE st.value into a temp NOW (survives a later shared overwrite)
                decl(decl, st.value);
                if (matd[static_cast<crd::usize>(st.value)] == 0U)
                {
                    s.append("  "); s.append(cty(g.node(st.value).dtype())); s.append(" t"); app_uint(s, static_cast<crd::u32>(st.value)); s.append(" = ");
                    ev(ev, st.value); s.append(";\n");
                    matd[static_cast<crd::usize>(st.value)] = 1U;
                }
                ++i;
                break;
            case KStmtKind::For: decl(decl, st.value); s.append("  for (unsigned lv"); app_uint(s, i); s.append(" = 0u; lv"); app_uint(s, i); s.append(" < (unsigned)("); ev(ev, st.value); s.append("); ++lv"); app_uint(s, i); s.append(") {\n"); self_b(self_b, st.body_begin, st.body_count); s.append("  }\n"); i = st.body_begin + st.body_count; break;
            case KStmtKind::If: decl(decl, st.value); s.append("  if ("); ev(ev, st.value); s.append(") {\n"); self_b(self_b, st.body_begin, st.body_count); s.append("  }\n"); i = st.body_begin + st.body_count; break;
            case KStmtKind::SpinUntilNonzero: decl(decl, st.index); s.append("  while (buf"); app_uint(s, g.node(st.target).iidx); s.append("["); ev(ev, st.index); s.append("] == 0u) { __nanosleep(64); }\n"); ++i; break; // guarded spin (diagnosable, never deadlocks); volatile re-read, NO per-iter fence
            case KStmtKind::SharedAtomicAdd: decl(decl, st.index); decl(decl, st.value); s.append("  atomicAdd(&sh"); app_uint(s, st.target); s.append("["); ev(ev, st.index); s.append("], "); ev(ev, st.value); s.append(");\n"); ++i; break;
            case KStmtKind::BufferAtomicAdd: decl(decl, st.index); decl(decl, st.value); s.append("  atomicAdd(&buf"); app_uint(s, g.node(st.target).iidx); s.append("["); ev(ev, st.index); s.append("], "); ev(ev, st.value); s.append(");\n"); ++i; break;
            case KStmtKind::ForBreakIf: decl(decl, st.value); s.append("  if (("); ev(ev, st.value); s.append(") != 0u) break;\n"); ++i; break;
            case KStmtKind::BufferTicket: decl(decl, st.index); s.append("  if (threadIdx.x == 0u) { sh"); app_uint(s, st.value); s.append("[0] = atomicAdd((unsigned*)&buf"); app_uint(s, g.node(st.target).iidx); s.append("["); ev(ev, st.index); s.append("], 1u); }\n"); ++i; break;
            case KStmtKind::SyncWarp: s.append("  __syncwarp();\n"); ++i; break;
            }
        }
    };
    emit_body(emit_body, entry.kernel_body_begin, entry.kernel_body_count);
    s.append("}\n");
    return ok;
}

// ── B0 fan-out: the TYPE-AWARE value layer on CUDA, by SCALARIZATION ─────────────────────────────────────────────────
// CUDA is the odd backend: it has **no native vector arithmetic**. `float3` exists but carries no operators (that is
// what `helper_math.h` is for), and there is no matrix type at all. So rather than emit a vector runtime into every
// kernel, a value of `comps` components becomes `comps` scalar temps `t<node>_<comp>` and every op is written out
// componentwise. nvcc's own SROA would scalarize a struct back into registers anyway, so this costs nothing and keeps
// the emitted PTX a direct function of the IR. It also makes the B0-4 aggregates FREE: `StructMake`/`ArrayMake`/
// `FieldGet`/`ArrayGet` emit nothing and are resolved at emit time by walking a component index back to its producer —
// which even lets CUDA handle a `Select` of two structs, something the GLSL/HLSL SROA path must refuse.
// Determinism: the backend compiles with `--fmad=false --prec-div=true --prec-sqrt=true`, so each component follows the
// CPU oracle's exact operation order (note `1.0f/sqrtf(x)` rather than the approximate `rsqrtf`, and `a[k] / len`
// rather than a reciprocal multiply).
// NOLINTBEGIN(readability-function-size) -- one branch per KOp, each a few lines of componentwise index arithmetic.
// Splitting it into per-op functions would scatter one backend's lowering across ~40 call sites without removing a line
// of logic; the size is the shape of the problem, exactly as in `eval_cpu`.
inline bool emit_vec_cuda(const KGraph& g, int output, crd::memory::IAllocator* scratch, GlslKernel& out)
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
        if (i < 0 || reach[static_cast<crd::usize>(i)]) { continue; }
        reach[static_cast<crd::usize>(i)] = 1;
        const KNode& nd = g.node(i);
        if (!is_vec_fusable(nd.op)) { return false; }
        if (nd.op == KOp::For || nd.op == KOp::LoopIndex || nd.op == KOp::LoopAcc) { return false; } // dynamic control flow not mirrored yet — refuse loudly
        if (nd.op == KOp::MatInverse && nd.type.rows == 4) { return false; }                          // mat4 inverse deferred, as on HLSL/WGSL
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
        if (nd.d >= 0) { stk.push_back(nd.d); }
        for (int k = 0; k < static_cast<int>(nd.n_ext); ++k) { stk.push_back(g.ext_operand(nd, k)); }
    }
    crd::containers::Array<int> binding_of(scratch);
    binding_of.resize(static_cast<crd::usize>(n), -1);
    out.n_inputs = 0;
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        if (g.node(i).op == KOp::Input)
        {
            binding_of[static_cast<crd::usize>(i)] = out.n_inputs;
            out.input_iidx[out.n_inputs]           = g.node(i).iidx;
            out.in_comps[out.n_inputs]             = g.node(i).comps();
            ++out.n_inputs;
        }
    }
    out.out_comps              = g.node(output).comps();
    crd::containers::String& s = out.source;
    s.clear();
    s.append("extern \"C\" __global__ void ckir(");
    for (int b = 0; b < out.n_inputs; ++b) { s.append("const float* in"); app_uint(s, b); s.append(", "); }
    s.append("float* outb, unsigned n) {\n");
    s.append("  unsigned gid = blockIdx.x * blockDim.x + threadIdx.x;\n  if (gid >= n) return;\n");

    // Walk a (node, component) pair back through the aggregate ops to the scalar that actually produces it.
    const auto resolve = [&](int node, int comp, int& rn, int& rc)
    {
        for (;;)
        {
            const KNode& nd = g.node(node);
            if (nd.op == KOp::StructMake || nd.op == KOp::ArrayMake)
            {
                int  off  = 0;
                bool hit  = false;
                for (int k = 0; k < static_cast<int>(nd.n_ext); ++k)
                {
                    const int src = g.ext_operand(nd, k);
                    const int sc  = g.node(src).comps();
                    if (comp < off + sc) { node = src; comp -= off; hit = true; break; }
                    off += sc;
                }
                if (hit) { continue; }
            }
            else if (nd.op == KOp::FieldGet) { comp += g.struct_field_offset(g.node(nd.a).type.struct_id, nd.iidx); node = nd.a; continue; }
            else if (nd.op == KOp::ArrayGet) { comp += nd.iidx * g.node(nd.a).type.elem_size(); node = nd.a; continue; }
            rn = node;
            rc = comp;
            return;
        }
    };
    const auto tc = [&](int node, int comp) { int rn = 0; int rc = 0; resolve(node, comp, rn, rc); s.append("t"); app_uint(s, rn); s.append("_"); app_uint(s, rc); };
    // BROADCASTING accessor: GLSL/HLSL let a scalar operand meet a vector one (`vec3 * float`), and CKIR takes a binary
    // node's type from operand `a`, so a scalar `b` must replicate its single component rather than index past its end.
    const auto tcb = [&](int node, int comp) { tc(node, g.node(node).comps() == 1 ? 0 : comp); };
    const auto decl = [&](int i, int k, DType d)
    {
        s.append("  ");
        if (d == DType::Bool) { s.append("bool"); }
        else if (dt_is_uint(d)) { s.append("unsigned"); }
        else if (dt_is_int(d)) { s.append("int"); }
        else { s.append("float"); }
        s.append(" t"); app_uint(s, i); s.append("_"); app_uint(s, k); s.append(" = ");
    };
    // truthiness of one component of `node` (bool components test directly; numeric ones compare against zero)
    const auto truth = [&](int node, int k) { if (g.node(node).dtype() == DType::Bool) { tc(node, k); } else { s.append("("); tc(node, k); s.append(" != 0.0f)"); } };
    // 2x2 / 3x3 determinant of a column-major index set into `node`'s components (mirrors ckir_eval's mat_det order)
    const auto det2 = [&](int node, const int* x) { tc(node, x[0]); s.append("*"); tc(node, x[3]); s.append(" - "); tc(node, x[2]); s.append("*"); tc(node, x[1]); };
    const auto det3 = [&](int node, const int* x)
    {
        tc(node, x[0]); s.append("*("); tc(node, x[4]); s.append("*"); tc(node, x[8]); s.append(" - "); tc(node, x[7]); s.append("*"); tc(node, x[5]); s.append(")");
        s.append(" - "); tc(node, x[3]); s.append("*("); tc(node, x[1]); s.append("*"); tc(node, x[8]); s.append(" - "); tc(node, x[7]); s.append("*"); tc(node, x[2]); s.append(")");
        s.append(" + "); tc(node, x[6]); s.append("*("); tc(node, x[1]); s.append("*"); tc(node, x[5]); s.append(" - "); tc(node, x[4]); s.append("*"); tc(node, x[2]); s.append(")");
    };

    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        if (is_aggregate(nd.op)) { continue; } // resolved at emit time by `resolve()` — nothing is materialized
        const int   c  = nd.comps();
        const DType dt = nd.dtype();
        switch (nd.op)
        {
        case KOp::Input:
            for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("in"); app_uint(s, binding_of[static_cast<crd::usize>(i)]); if (c == 1) { s.append("[gid]"); } else { s.append("[gid*"); app_uint(s, c); s.append("+"); app_uint(s, k); s.append("]"); } s.append(";\n"); }
            break;
        case KOp::Const:
            for (int k = 0; k < c; ++k) { decl(i, k, dt); if (dt == DType::Bool) { s.append(nd.cval != 0.0 ? "true" : "false"); } else if (dt_is_int(dt) || dt_is_uint(dt)) { app_ilit(s, nd.cval); } else { app_flit(s, nd.cval); s.append("f"); } s.append(";\n"); }
            break;
        case KOp::Cast:
            for (int k = 0; k < c; ++k) { decl(i, k, dt); if (dt == DType::Bool) { s.append("bool("); } else if (dt_is_uint(dt)) { s.append("(unsigned)("); } else if (dt_is_int(dt)) { s.append("int("); } else { s.append("float("); } tc(nd.a, k); s.append(");\n"); }
            break;
        case KOp::Neg: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("-"); tc(nd.a, k); s.append(";\n"); } break;
        case KOp::Recip: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("1.0f / "); tc(nd.a, k); s.append(";\n"); } break;
        case KOp::Abs: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("fabsf("); tc(nd.a, k); s.append(");\n"); } break;
        case KOp::Sqrt: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("sqrtf("); tc(nd.a, k); s.append(");\n"); } break;
        case KOp::Rsqrt: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("1.0f / sqrtf("); tc(nd.a, k); s.append(");\n"); } break; // NOT rsqrtf: that is the approximate intrinsic
        case KOp::Exp: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("expf("); tc(nd.a, k); s.append(");\n"); } break;
        case KOp::Log: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("logf("); tc(nd.a, k); s.append(");\n"); } break;
        case KOp::Sin: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("sinf("); tc(nd.a, k); s.append(");\n"); } break;
        case KOp::Cos: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("cosf("); tc(nd.a, k); s.append(");\n"); } break;
        case KOp::Floor: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("floorf("); tc(nd.a, k); s.append(");\n"); } break;
        case KOp::Fract: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("("); tc(nd.a, k); s.append(" - floorf("); tc(nd.a, k); s.append("));\n"); } break;
        case KOp::Add: for (int k = 0; k < c; ++k) { decl(i, k, dt); tc(nd.a, k); s.append(" + "); tcb(nd.b, k); s.append(";\n"); } break;
        case KOp::Sub: for (int k = 0; k < c; ++k) { decl(i, k, dt); tc(nd.a, k); s.append(" - "); tcb(nd.b, k); s.append(";\n"); } break;
        case KOp::Mul: for (int k = 0; k < c; ++k) { decl(i, k, dt); tc(nd.a, k); s.append(" * "); tcb(nd.b, k); s.append(";\n"); } break;
        case KOp::Div: for (int k = 0; k < c; ++k) { decl(i, k, dt); tc(nd.a, k); s.append(" / "); tcb(nd.b, k); s.append(";\n"); } break;
        case KOp::Min: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("fminf("); tc(nd.a, k); s.append(", "); tcb(nd.b, k); s.append(");\n"); } break;
        case KOp::Max: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("fmaxf("); tc(nd.a, k); s.append(", "); tcb(nd.b, k); s.append(");\n"); } break;
        case KOp::Pow: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("powf("); tc(nd.a, k); s.append(", "); tcb(nd.b, k); s.append(");\n"); } break;
        case KOp::Clamp: for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("fminf(fmaxf("); tc(nd.a, k); s.append(", "); tcb(nd.b, k); s.append("), "); tcb(nd.c, k); s.append(");\n"); } break;
        case KOp::Mix: for (int k = 0; k < c; ++k) { decl(i, k, dt); tc(nd.a, k); s.append(" * (1.0f - "); tcb(nd.c, k); s.append(") + "); tcb(nd.b, k); s.append(" * "); tcb(nd.c, k); s.append(";\n"); } break; // oracle order: a*(1-t) + b*t
        case KOp::Vec2: decl(i, 0, dt); tc(nd.a, 0); s.append(";\n"); decl(i, 1, dt); tc(nd.b, 0); s.append(";\n"); break;
        case KOp::Vec3: decl(i, 0, dt); tc(nd.a, 0); s.append(";\n"); decl(i, 1, dt); tc(nd.b, 0); s.append(";\n"); decl(i, 2, dt); tc(nd.c, 0); s.append(";\n"); break;
        case KOp::VecConcat: { const int ac = g.node(nd.a).comps(); for (int k = 0; k < c; ++k) { decl(i, k, dt); if (k < ac) { tc(nd.a, k); } else { tc(nd.b, k - ac); } s.append(";\n"); } break; }
        case KOp::VecComp: decl(i, 0, dt); tc(nd.a, nd.iidx); s.append(";\n"); break;
        case KOp::Swizzle: for (int k = 0; k < c; ++k) { decl(i, k, dt); tc(nd.a, nd.perm[k]); s.append(";\n"); } break;
        case KOp::Splat: for (int k = 0; k < c; ++k) { decl(i, k, dt); tc(nd.a, 0); s.append(";\n"); } break;
        case KOp::Dot: { const int ac = g.node(nd.a).comps(); decl(i, 0, dt); for (int k = 0; k < ac; ++k) { if (k) { s.append(" + "); } tc(nd.a, k); s.append("*"); tc(nd.b, k); } s.append(";\n"); break; }
        case KOp::Cross:
            decl(i, 0, dt); tc(nd.a, 1); s.append("*"); tc(nd.b, 2); s.append(" - "); tc(nd.a, 2); s.append("*"); tc(nd.b, 1); s.append(";\n");
            decl(i, 1, dt); tc(nd.a, 2); s.append("*"); tc(nd.b, 0); s.append(" - "); tc(nd.a, 0); s.append("*"); tc(nd.b, 2); s.append(";\n");
            decl(i, 2, dt); tc(nd.a, 0); s.append("*"); tc(nd.b, 1); s.append(" - "); tc(nd.a, 1); s.append("*"); tc(nd.b, 0); s.append(";\n");
            break;
        case KOp::VecLen: { const int ac = g.node(nd.a).comps(); decl(i, 0, dt); s.append("sqrtf("); for (int k = 0; k < ac; ++k) { if (k) { s.append(" + "); } tc(nd.a, k); s.append("*"); tc(nd.a, k); } s.append(");\n"); break; }
        case KOp::Normalize:
        {
            s.append("  float t"); app_uint(s, i); s.append("_L = sqrtf(");
            for (int k = 0; k < c; ++k) { if (k) { s.append(" + "); } tc(nd.a, k); s.append("*"); tc(nd.a, k); }
            s.append(");\n");
            for (int k = 0; k < c; ++k) { decl(i, k, dt); tc(nd.a, k); s.append(" / t"); app_uint(s, i); s.append("_L;\n"); } // oracle divides by len
            break;
        }
        case KOp::Reflect:
        {
            s.append("  float t"); app_uint(s, i); s.append("_D = ");
            for (int k = 0; k < c; ++k) { if (k) { s.append(" + "); } tc(nd.b, k); s.append("*"); tc(nd.a, k); } // dp = n . i
            s.append(";\n");
            for (int k = 0; k < c; ++k) { decl(i, k, dt); tc(nd.a, k); s.append(" - 2.0f * t"); app_uint(s, i); s.append("_D * "); tc(nd.b, k); s.append(";\n"); }
            break;
        }
        case KOp::Refract:
        {
            s.append("  float t"); app_uint(s, i); s.append("_D = ");
            for (int k = 0; k < c; ++k) { if (k) { s.append(" + "); } tc(nd.b, k); s.append("*"); tc(nd.a, k); }
            s.append(";\n");
            s.append("  float t"); app_uint(s, i); s.append("_E = "); tc(nd.c, 0); s.append(";\n");
            s.append("  float t"); app_uint(s, i); s.append("_K = 1.0f - t"); app_uint(s, i); s.append("_E * t"); app_uint(s, i); s.append("_E * (1.0f - t"); app_uint(s, i); s.append("_D * t"); app_uint(s, i); s.append("_D);\n");
            s.append("  float t"); app_uint(s, i); s.append("_C = t"); app_uint(s, i); s.append("_E * t"); app_uint(s, i); s.append("_D + sqrtf(fmaxf(t"); app_uint(s, i); s.append("_K, 0.0f));\n");
            for (int k = 0; k < c; ++k)
            {
                decl(i, k, dt); s.append("(t"); app_uint(s, i); s.append("_K < 0.0f) ? 0.0f : (t"); app_uint(s, i); s.append("_E * "); tc(nd.a, k); s.append(" - t"); app_uint(s, i); s.append("_C * "); tc(nd.b, k); s.append(");\n");
            }
            break;
        }
        case KOp::Faceforward:
        {
            s.append("  float t"); app_uint(s, i); s.append("_D = ");
            for (int k = 0; k < c; ++k) { if (k) { s.append(" + "); } tc(nd.c, k); s.append("*"); tc(nd.b, k); } // dp = nref . i
            s.append(";\n");
            for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("((t"); app_uint(s, i); s.append("_D < 0.0f) ? 1.0f : -1.0f) * "); tc(nd.a, k); s.append(";\n"); }
            break;
        }
        case KOp::MatFromCols: { const int mr = nd.type.rows; const int mc = nd.type.cols; const int operand[4] = {nd.a, nd.b, nd.c, nd.d}; for (int col = 0; col < mc; ++col) { for (int row = 0; row < mr; ++row) { decl(i, col * mr + row, dt); tc(operand[col], row); s.append(";\n"); } } break; }
        case KOp::MatVecMul:
        {
            const int mr = g.node(nd.a).type.rows;
            const int mc = g.node(nd.a).type.cols;
            for (int r = 0; r < mr; ++r) { decl(i, r, dt); for (int col = 0; col < mc; ++col) { if (col) { s.append(" + "); } tc(nd.a, col * mr + r); s.append("*"); tc(nd.b, col); } s.append(";\n"); }
            break;
        }
        case KOp::MatMatMul:
        {
            const int ar = g.node(nd.a).type.rows;
            const int ak = g.node(nd.a).type.cols;
            const int bc = g.node(nd.b).type.cols;
            for (int col = 0; col < bc; ++col) { for (int r = 0; r < ar; ++r) { decl(i, col * ar + r, dt); for (int k = 0; k < ak; ++k) { if (k) { s.append(" + "); } tc(nd.a, k * ar + r); s.append("*"); tc(nd.b, col * ak + k); } s.append(";\n"); } }
            break;
        }
        case KOp::MatTranspose: { const int orows = nd.type.rows; const int ocols = nd.type.cols; for (int col = 0; col < ocols; ++col) { for (int r = 0; r < orows; ++r) { decl(i, col * orows + r, dt); tc(nd.a, r * ocols + col); s.append(";\n"); } } break; }
        case KOp::OuterProduct: { const int orows = nd.type.rows; const int ocols = nd.type.cols; for (int col = 0; col < ocols; ++col) { for (int row = 0; row < orows; ++row) { decl(i, col * orows + row, dt); tc(nd.a, row); s.append("*"); tc(nd.b, col); s.append(";\n"); } } break; }
        case KOp::Determinant:
        {
            const int d = g.node(nd.a).type.rows;
            decl(i, 0, dt);
            if (d == 2) { const int x[4] = {0, 1, 2, 3}; det2(nd.a, x); }
            else if (d == 3) { int x[9]; for (int k = 0; k < 9; ++k) { x[k] = k; } det3(nd.a, x); }
            else { return false; } // 4x4 determinant not mirrored on CUDA (documented gap)
            s.append(";\n");
            break;
        }
        case KOp::MatInverse:
        {
            const int d = nd.type.rows;
            if (d != 2 && d != 3) { return false; }
            s.append("  float t"); app_uint(s, i); s.append("_DET = ");
            if (d == 2) { const int x[4] = {0, 1, 2, 3}; det2(nd.a, x); }
            else { int x[9]; for (int k = 0; k < 9; ++k) { x[k] = k; } det3(nd.a, x); }
            s.append(";\n");
            // mirrors ckir_eval: dst[cj*d + ri] = sign(ri+cj) * minor_det(sr=cj, sc=ri) / det
            for (int cj = 0; cj < d; ++cj)
            {
                for (int ri = 0; ri < d; ++ri)
                {
                    int mi  = 0;
                    int idx[4] = {0, 0, 0, 0};
                    for (int col = 0; col < d; ++col) { if (col == ri) { continue; } for (int row = 0; row < d; ++row) { if (row == cj) { continue; } idx[mi++] = col * d + row; } }
                    decl(i, cj * d + ri, dt);
                    s.append(((ri + cj) % 2 == 0) ? "(" : "-(");
                    if (d == 2) { tc(nd.a, idx[0]); }
                    else { det2(nd.a, idx); }
                    s.append(") / t"); app_uint(s, i); s.append("_DET;\n");
                }
            }
            break;
        }
        case KOp::VecAny: { const int ac = g.node(nd.a).comps(); decl(i, 0, dt); for (int k = 0; k < ac; ++k) { if (k) { s.append(" || "); } truth(nd.a, k); } s.append(";\n"); break; }
        case KOp::VecAll: { const int ac = g.node(nd.a).comps(); decl(i, 0, dt); for (int k = 0; k < ac; ++k) { if (k) { s.append(" && "); } truth(nd.a, k); } s.append(";\n"); break; }
        case KOp::CmpLt: case KOp::CmpLe: case KOp::CmpGt: case KOp::CmpGe: case KOp::CmpEq: case KOp::CmpNe:
        {
            const char* sym = " < ";
            switch (nd.op)
            {
            case KOp::CmpLe: sym = " <= "; break;
            case KOp::CmpGt: sym = " > "; break;
            case KOp::CmpGe: sym = " >= "; break;
            case KOp::CmpEq: sym = " == "; break;
            case KOp::CmpNe: sym = " != "; break;
            default: break;
            }
            for (int k = 0; k < c; ++k) { decl(i, k, dt); s.append("("); tc(nd.a, k); s.append(sym); tcb(nd.b, k); s.append(");\n"); }
            break;
        }
        case KOp::Select: for (int k = 0; k < c; ++k) { decl(i, k, dt); truth(nd.c, 0); s.append(" ? "); tc(nd.a, k); s.append(" : "); tcb(nd.b, k); s.append(";\n"); } break;
        case KOp::QuatConj:
            for (int k = 0; k < 3; ++k) { decl(i, k, dt); s.append("-"); tc(nd.a, k); s.append(";\n"); }
            decl(i, 3, dt); tc(nd.a, 3); s.append(";\n");
            break;
        case KOp::QuatMul:
            decl(i, 0, dt); tc(nd.a, 3); s.append("*"); tc(nd.b, 0); s.append(" + "); tc(nd.a, 0); s.append("*"); tc(nd.b, 3); s.append(" + "); tc(nd.a, 1); s.append("*"); tc(nd.b, 2); s.append(" - "); tc(nd.a, 2); s.append("*"); tc(nd.b, 1); s.append(";\n");
            decl(i, 1, dt); tc(nd.a, 3); s.append("*"); tc(nd.b, 1); s.append(" - "); tc(nd.a, 0); s.append("*"); tc(nd.b, 2); s.append(" + "); tc(nd.a, 1); s.append("*"); tc(nd.b, 3); s.append(" + "); tc(nd.a, 2); s.append("*"); tc(nd.b, 0); s.append(";\n");
            decl(i, 2, dt); tc(nd.a, 3); s.append("*"); tc(nd.b, 2); s.append(" + "); tc(nd.a, 0); s.append("*"); tc(nd.b, 1); s.append(" - "); tc(nd.a, 1); s.append("*"); tc(nd.b, 0); s.append(" + "); tc(nd.a, 2); s.append("*"); tc(nd.b, 3); s.append(";\n");
            decl(i, 3, dt); tc(nd.a, 3); s.append("*"); tc(nd.b, 3); s.append(" - "); tc(nd.a, 0); s.append("*"); tc(nd.b, 0); s.append(" - "); tc(nd.a, 1); s.append("*"); tc(nd.b, 1); s.append(" - "); tc(nd.a, 2); s.append("*"); tc(nd.b, 2); s.append(";\n");
            break;
        case KOp::QuatAxisAngle:
            s.append("  float t"); app_uint(s, i); s.append("_H = "); tc(nd.b, 0); s.append(" * 0.5f;\n");
            s.append("  float t"); app_uint(s, i); s.append("_S = sinf(t"); app_uint(s, i); s.append("_H);\n");
            for (int k = 0; k < 3; ++k) { decl(i, k, dt); tc(nd.a, k); s.append(" * t"); app_uint(s, i); s.append("_S;\n"); }
            decl(i, 3, dt); s.append("cosf(t"); app_uint(s, i); s.append("_H);\n");
            break;
        case KOp::QuatRotate:
            s.append("  float t"); app_uint(s, i); s.append("_TX = 2.0f * ("); tc(nd.a, 1); s.append("*"); tc(nd.b, 2); s.append(" - "); tc(nd.a, 2); s.append("*"); tc(nd.b, 1); s.append(");\n");
            s.append("  float t"); app_uint(s, i); s.append("_TY = 2.0f * ("); tc(nd.a, 2); s.append("*"); tc(nd.b, 0); s.append(" - "); tc(nd.a, 0); s.append("*"); tc(nd.b, 2); s.append(");\n");
            s.append("  float t"); app_uint(s, i); s.append("_TZ = 2.0f * ("); tc(nd.a, 0); s.append("*"); tc(nd.b, 1); s.append(" - "); tc(nd.a, 1); s.append("*"); tc(nd.b, 0); s.append(");\n");
            decl(i, 0, dt); tc(nd.b, 0); s.append(" + "); tc(nd.a, 3); s.append(" * t"); app_uint(s, i); s.append("_TX + ("); tc(nd.a, 1); s.append(" * t"); app_uint(s, i); s.append("_TZ - "); tc(nd.a, 2); s.append(" * t"); app_uint(s, i); s.append("_TY);\n");
            decl(i, 1, dt); tc(nd.b, 1); s.append(" + "); tc(nd.a, 3); s.append(" * t"); app_uint(s, i); s.append("_TY + ("); tc(nd.a, 2); s.append(" * t"); app_uint(s, i); s.append("_TX - "); tc(nd.a, 0); s.append(" * t"); app_uint(s, i); s.append("_TZ);\n");
            decl(i, 2, dt); tc(nd.b, 2); s.append(" + "); tc(nd.a, 3); s.append(" * t"); app_uint(s, i); s.append("_TZ + ("); tc(nd.a, 0); s.append(" * t"); app_uint(s, i); s.append("_TY - "); tc(nd.a, 1); s.append(" * t"); app_uint(s, i); s.append("_TX);\n");
            break;
        case KOp::QuatToMat3:
        {
            const char* nm[4] = {"_QX", "_QY", "_QZ", "_QW"};
            for (int k = 0; k < 4; ++k) { s.append("  float t"); app_uint(s, i); s.append(nm[k]); s.append(" = "); tc(nd.a, k); s.append(";\n"); }
            const auto q = [&](int k) { s.append("t"); app_uint(s, i); s.append(nm[k]); };
            const auto m2 = [&](int p, int r) { q(p); s.append("*"); q(r); };
            decl(i, 0, dt); s.append("1.0f - 2.0f * ("); m2(1, 1); s.append(" + "); m2(2, 2); s.append(");\n");
            decl(i, 1, dt); s.append("2.0f * ("); m2(0, 1); s.append(" + "); m2(3, 2); s.append(");\n");
            decl(i, 2, dt); s.append("2.0f * ("); m2(0, 2); s.append(" - "); m2(3, 1); s.append(");\n");
            decl(i, 3, dt); s.append("2.0f * ("); m2(0, 1); s.append(" - "); m2(3, 2); s.append(");\n");
            decl(i, 4, dt); s.append("1.0f - 2.0f * ("); m2(0, 0); s.append(" + "); m2(2, 2); s.append(");\n");
            decl(i, 5, dt); s.append("2.0f * ("); m2(1, 2); s.append(" + "); m2(3, 0); s.append(");\n");
            decl(i, 6, dt); s.append("2.0f * ("); m2(0, 2); s.append(" + "); m2(3, 1); s.append(");\n");
            decl(i, 7, dt); s.append("2.0f * ("); m2(1, 2); s.append(" - "); m2(3, 0); s.append(");\n");
            decl(i, 8, dt); s.append("1.0f - 2.0f * ("); m2(0, 0); s.append(" + "); m2(1, 1); s.append(");\n");
            break;
        }
        case KOp::Slerp:
        {
            for (int k = 0; k < c; ++k) { s.append("  float t"); app_uint(s, i); s.append("_"); app_uint(s, k); s.append(";\n"); }
            s.append("  {\n    float d = ");
            for (int k = 0; k < c; ++k) { if (k) { s.append(" + "); } tc(nd.a, k); s.append("*"); tc(nd.b, k); }
            s.append(";\n    float sg = 1.0f;\n    if (d < 0.0f) { d = -d; sg = -1.0f; }\n");
            s.append("    if (d > 0.9995f) {\n");
            for (int k = 0; k < c; ++k) { s.append("      float m"); app_uint(s, k); s.append(" = "); tc(nd.a, k); s.append(" + "); tc(nd.c, 0); s.append(" * (sg * "); tc(nd.b, k); s.append(" - "); tc(nd.a, k); s.append(");\n"); }
            s.append("      float s2 = ");
            for (int k = 0; k < c; ++k) { if (k) { s.append(" + "); } s.append("m"); app_uint(s, k); s.append("*m"); app_uint(s, k); }
            s.append(";\n      float il = 1.0f / sqrtf(s2);\n");
            for (int k = 0; k < c; ++k) { s.append("      t"); app_uint(s, i); s.append("_"); app_uint(s, k); s.append(" = m"); app_uint(s, k); s.append(" * il;\n"); }
            s.append("    } else {\n      float th = acosf(d);\n      float sn = sinf(th);\n      float w1 = sinf((1.0f - "); tc(nd.c, 0); s.append(") * th) / sn;\n      float w2 = sinf("); tc(nd.c, 0); s.append(" * th) / sn;\n");
            for (int k = 0; k < c; ++k) { s.append("      t"); app_uint(s, i); s.append("_"); app_uint(s, k); s.append(" = w1 * "); tc(nd.a, k); s.append(" + w2 * sg * "); tc(nd.b, k); s.append(";\n"); }
            s.append("    }\n  }\n");
            break;
        }
        default: return false;
        }
    }

    // write-back: `comps` interleaved floats per element; bool/int components convert explicitly.
    const KType& oty = g.node(output).type;
    const int    oc  = out.out_comps;
    for (int k = 0; k < oc; ++k)
    {
        s.append("  outb[");
        if (oc == 1) { s.append("gid"); } else { s.append("gid*"); app_uint(s, oc); s.append("+"); app_uint(s, k); }
        s.append("] = ");
        if (oty.scalar == DType::Bool) { s.append("("); tc(output, k); s.append(" ? 1.0f : 0.0f)"); }
        else if (oty.scalar != DType::F32 && oty.scalar != DType::F64) { s.append("float("); tc(output, k); s.append(")"); }
        else { tc(output, k); }
        s.append(";\n");
    }
    s.append("}\n");
    return true;
}
// NOLINTEND(readability-function-size)

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
                case KOp::Cast: s.append("float("); te(nd.a); s.append(")"); break;
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
