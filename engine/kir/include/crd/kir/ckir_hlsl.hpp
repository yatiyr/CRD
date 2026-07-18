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
    DType in_dtype[kMaxKernelInputs] = {};
    out.n_inputs                     = 0;
    for (int i = 0; i < n; ++i)
    {
        if (reach[static_cast<crd::usize>(i)] && g.node(i).op == KOp::Input)
        {
            binding_of[static_cast<crd::usize>(i)] = out.n_inputs;
            out.input_iidx[out.n_inputs]           = g.node(i).iidx;
            in_dtype[out.n_inputs]                 = g.node(i).dtype();
            ++out.n_inputs;
        }
    }

    crd::containers::String& s = out.source;
    s.clear();
    for (int b = 0; b < out.n_inputs; ++b) { s.append("RWStructuredBuffer<"); s.append(ctype(in_dtype[b])); s.append("> in"); app_uint(s, b); s.append(" : register(u"); app_uint(s, b); s.append(");\n"); }
    s.append("RWStructuredBuffer<"); s.append(buf_ctype(g.node(output).dtype())); s.append("> outb : register(u"); app_uint(s, out.n_inputs); s.append(");\n");
    s.append("cbuffer PC : register(b0) { uint n; };\n");
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 dtid : SV_DispatchThreadID) {\n  uint gid = dtid.x;\n  if (gid >= n) return;\n");
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        const bool ii = dt_is_int(nd.dtype()) || dt_is_uint(nd.dtype());
        if (nd.dtype() == DType::Bool) { s.append("  bool t"); }
        else { s.append(ii ? "  int t" : "  precise float t"); }
        app_uint(s, i); s.append(" = ");
        const auto ta = [&](int id) { s.append("t"); app_uint(s, id); };
        switch (nd.op)
        {
        case KOp::Input: s.append("in"); app_uint(s, binding_of[static_cast<crd::usize>(i)]); s.append("[gid]"); break;
        case KOp::Const: if (ii) { app_ilit(s, nd.cval); } else { app_flit(s, nd.cval); } break;
        case KOp::Cast: s.append(ii ? "int(" : "float("); ta(nd.a); s.append(")"); break;
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
        // B0-3: comparisons are bool-typed. HLSL's relational operators are componentwise on vectors and yield boolN,
        // so unlike GLSL there is no separate lessThan()/equal() family to call.
        case KOp::CmpLt: s.append("("); ta(nd.a); s.append(" < "); ta(nd.b); s.append(")"); break;
        case KOp::CmpEq: s.append("("); ta(nd.a); s.append(" == "); ta(nd.b); s.append(")"); break;
        case KOp::CmpLe: s.append("("); ta(nd.a); s.append(" <= "); ta(nd.b); s.append(")"); break;
        case KOp::CmpGt: s.append("("); ta(nd.a); s.append(" > "); ta(nd.b); s.append(")"); break;
        case KOp::CmpGe: s.append("("); ta(nd.a); s.append(" >= "); ta(nd.b); s.append(")"); break;
        case KOp::CmpNe: s.append("("); ta(nd.a); s.append(" != "); ta(nd.b); s.append(")"); break;
        case KOp::BitNot: s.append("(~"); ta(nd.a); s.append(")"); break;
        case KOp::BitCount: s.append("countbits("); ta(nd.a); s.append(")"); break;
        case KOp::FindLSB: s.append("firstbitlow("); ta(nd.a); s.append(")"); break;
        case KOp::FindMSB: s.append("firstbithigh("); ta(nd.a); s.append(")"); break;
        case KOp::BitfieldExtract: s.append("(("); ta(nd.a); s.append(" >> "); ta(nd.b); s.append(") & ((1 << "); ta(nd.c); s.append(") - 1))"); break;
        case KOp::BitReverse: s.append("reversebits("); ta(nd.a); s.append(")"); break;
        case KOp::Ldexp: s.append("ldexp("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::FloatBitsToInt: s.append("asint("); ta(nd.a); s.append(")"); break;
        case KOp::IntBitsToFloat: s.append("asfloat("); ta(nd.a); s.append(")"); break;
        case KOp::Shl: ta(nd.a); s.append(" << "); ta(nd.b); break;
        case KOp::Shr: ta(nd.a); s.append(" >> "); ta(nd.b); break;
        case KOp::BitAnd: ta(nd.a); s.append(" & "); ta(nd.b); break;
        case KOp::BitOr: ta(nd.a); s.append(" | "); ta(nd.b); break;
        case KOp::BitXor: ta(nd.a); s.append(" ^ "); ta(nd.b); break;
        case KOp::Fract: s.append("("); ta(nd.a); s.append(" - floor("); ta(nd.a); s.append("))"); break;
        case KOp::Step: s.append("(("); ta(nd.b); s.append(" < "); ta(nd.a); s.append(") ? 0.0 : 1.0)"); break;
        case KOp::Pow: s.append("pow("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Clamp: s.append("min(max("); ta(nd.a); s.append(", "); ta(nd.b); s.append("), "); ta(nd.c); s.append(")"); break;
        case KOp::Mix: s.append("("); ta(nd.a); s.append(" * (1.0 - "); ta(nd.c); s.append(") + "); ta(nd.b); s.append(" * "); ta(nd.c); s.append(")"); break;
        case KOp::Rsqrt: s.append("rsqrt("); ta(nd.a); s.append(")"); break;
        case KOp::Exp2: s.append("exp2("); ta(nd.a); s.append(")"); break;
        case KOp::Log2: s.append("log2("); ta(nd.a); s.append(")"); break;
        case KOp::Tan: s.append("tan("); ta(nd.a); s.append(")"); break;
        case KOp::Radians: s.append("("); ta(nd.a); s.append(" * 0.017453292519943295)"); break;
        case KOp::Degrees: s.append("("); ta(nd.a); s.append(" * 57.29577951308232)"); break;
        case KOp::Atan2: s.append("atan2("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break; // HLSL atan2(y,x)
        case KOp::Smoothstep: s.append("smoothstep("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::Asin: s.append("asin("); ta(nd.a); s.append(")"); break;
        case KOp::Acos: s.append("acos("); ta(nd.a); s.append(")"); break;
        case KOp::Atan: s.append("atan("); ta(nd.a); s.append(")"); break;
        case KOp::Sinh: s.append("sinh("); ta(nd.a); s.append(")"); break;
        case KOp::Cosh: s.append("cosh("); ta(nd.a); s.append(")"); break;
        case KOp::Cbrt: s.append("(sign("); ta(nd.a); s.append(") * pow(abs("); ta(nd.a); s.append("), 0.3333333333333333))"); break; // no builtin
        case KOp::Mod: s.append("fmod("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break; // C fmod (sign of x)
        case KOp::Fma: s.append("fma("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::Select: s.append("("); if (g.node(nd.c).dtype() == DType::Bool) { ta(nd.c); } else { s.append("("); ta(nd.c); s.append(" != 0.0)"); } s.append(" ? "); ta(nd.a); s.append(" : "); ta(nd.b); s.append(")"); break;
        default: return false;
        }
        s.append(";\n");
    }
    // a bool result is stored as float 0.0/1.0 (RWStructuredBuffer<bool> has no defined layout).
    s.append("  outb[gid] = ");
    if (g.node(output).dtype() == DType::Bool) { s.append("float(t"); app_uint(s, output); s.append(")"); }
    else { s.append("t"); app_uint(s, output); }
    s.append(";\n}\n");
    return true;
}

// B-cmp: emit an IMPERATIVE compute KERNEL (the shared-memory / barrier IR — `KEntry.is_kernel()`) as HLSL (SM6, dxc →
// DXIL). The exact DX12 mirror of `emit_compute_kernel_glsl` (ckir_glsl.hpp): `[numthreads]` + `RWStructuredBuffer<T> bufN
// : register(uN)` (all UAV — binding N → uN, matching create_pipeline_from_hlsl's root signature; `readonly` is a neutral
// hint dropped here) + `groupshared T shNODE[len+pad]` + the statement body with INLINE recursive value expressions (a
// SharedLoad emits AT its statement, never hoisted across a barrier — same as GLSL). `SV_GroupIndex` is the flattened local
// index (= gl_LocalInvocationIndex); `GroupMemoryBarrierWithGroupSync()` is the control+shared barrier. Returns false on an
// op it can't lower.
inline bool emit_compute_kernel_hlsl(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    if (!entry.is_kernel()) { return false; }
    const int                n = g.size();
    crd::containers::String& s = out.source;
    s.clear();
    out.n_inputs = 0;
    for (int i = 0; i < n; ++i) // resource decls: storage buffers (UAV) + groupshared arrays
    {
        const KNode& nd = g.node(i);
        if (nd.op == KOp::BufferDecl)
        {
            // RAW UAV: the DX12 compute context binds every storage buffer as a RWByteAddressBuffer (R32_TYPELESS, no
            // element type baked into the descriptor — see dx12_compute_context.cpp). So loads/stores are byte-addressed
            // (index * 4) with asfloat/asuint reinterpret. 32-bit element types only (F32/U32/I32); F64 storage → refuse.
            if ((nd.axes & 2U) != 0U) { s.append("globallycoherent "); } // cross-workgroup visible (spin-wait publish/read)
            s.append("RWByteAddressBuffer buf"); app_uint(s, nd.iidx);
            s.append(" : register(u"); app_uint(s, nd.iidx); s.append(");\n");
        }
        else if (nd.op == KOp::SharedDecl)
        {
            s.append("groupshared "); s.append(buf_ctype(nd.dtype())); s.append(" sh"); app_uint(s, i);
            s.append("["); app_uint(s, nd.iidx + static_cast<int>(nd.axes)); s.append("];\n");
        }
    }
    s.append("[numthreads("); app_uint(s, static_cast<int>(entry.local_size[0]));
    s.append(", ");            app_uint(s, static_cast<int>(entry.local_size[1]));
    s.append(", ");            app_uint(s, static_cast<int>(entry.local_size[2]));
    s.append(")]\nvoid cs_main(uint lidx : SV_GroupIndex, uint3 wgid3 : SV_GroupID) {\n");

    // DETERMINISM: FLOAT arithmetic materializes as `precise` temps (HLSL `precise` ⇒ no mad-fusion ⇒ bit-matches the CPU
    // oracle) — the same lever as the GLSL kernel emitter. Leaves (raw-UAV loads / consts / builtins) + cast/select/compare/
    // bitops stay INLINE so a load re-reads at each use (correct across barriers); temps CSE by node id.
    bool                            ok = true;
    crd::containers::Array<crd::u8> temped(scratch);
    temped.resize(static_cast<crd::usize>(n), 0);
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
    const auto pv = [&](auto&& self, int node) -> void {
        if (temped[static_cast<crd::usize>(node)] != 0U) { s.append("t"); app_uint(s, static_cast<crd::u32>(node)); return; }
        const KNode& nd  = g.node(node);
        const auto   bin = [&](const char* o) { s.append("("); self(self, nd.a); s.append(o); self(self, nd.b); s.append(")"); };
        switch (nd.op)
        {
        case KOp::Const:
            if (nd.dtype() == DType::Bool) { s.append(nd.cval != 0.0 ? "true" : "false"); }
            // %lld (full 64-bit) via app_int_const — NOT static_cast<int> (MSVC clamps a u32 const > INT_MAX to INT_MIN).
            else if (dt_is_uint(nd.dtype()) || dt_is_int(nd.dtype())) { app_int_const(s, nd.cval, nd.dtype()); }
            else { app_flit(s, nd.cval); }
            break;
        case KOp::Builtin:
            if (static_cast<KBuiltin>(nd.iidx) == KBuiltin::LocalInvocationIndex) { s.append("lidx"); }
            else if (static_cast<KBuiltin>(nd.iidx) == KBuiltin::WorkgroupIndex) { s.append("wgid3.x"); }
            else { ok = false; s.append("0u"); }
            break;
        case KOp::KernelLoopVar: s.append("lv"); app_uint(s, nd.a); break;
        case KOp::BufferLoad: { // asfloat/asint(bufN.Load(idx*4)) — RAW byte-addressed UAV (see BufferDecl); uint loads bare
            const DType bt = g.node(nd.a).dtype();
            if (bt == DType::F64) { ok = false; s.append("0.0"); break; }
            const char* pre = !(dt_is_int(bt) || dt_is_uint(bt)) ? "asfloat(" : dt_is_int(bt) ? "asint(" : ""; // NOLINT(readability-avoid-nested-conditional-operator) 3-way reinterpret select
            s.append(pre);
            s.append("buf"); app_uint(s, g.node(nd.a).iidx); s.append(".Load(("); self(self, nd.b); s.append(") * 4u)");
            if (pre[0] != '\0') { s.append(")"); }
            break;
        }
        case KOp::SharedLoad: s.append("sh"); app_uint(s, nd.a); s.append("["); self(self, nd.b); s.append("]"); break;
        case KOp::Cast: s.append(ctype(nd.dtype())); s.append("("); self(self, nd.a); s.append(")"); break;
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
        case KOp::Select: // a=true b=false c=cond; wrap a non-bool cond in `!= 0.0` (mirrors the elementwise HLSL Select)
            s.append("(");
            if (g.node(nd.c).dtype() == DType::Bool) { self(self, nd.c); }
            else { s.append("("); self(self, nd.c); s.append(" != 0.0)"); }
            s.append(" ? "); self(self, nd.a); s.append(" : "); self(self, nd.b); s.append(")");
            break;
        default: ok = false; s.append("0"); break;
        }
    };
    const auto rhs = [&](const KNode& nd) -> void {
        const auto b2 = [&](const char* o) { s.append("("); pv(pv, nd.a); s.append(o); pv(pv, nd.b); s.append(")"); };
        const auto f2 = [&](const char* f) { s.append(f); s.append("("); pv(pv, nd.a); s.append(", "); pv(pv, nd.b); s.append(")"); };
        const auto f1 = [&](const char* f) { s.append(f); s.append("("); pv(pv, nd.a); s.append(")"); };
        switch (nd.op)
        {
        case KOp::Neg: s.append("(-"); pv(pv, nd.a); s.append(")"); break;
        case KOp::Abs: f1("abs"); break;
        case KOp::Sqrt: f1("sqrt"); break;
        case KOp::Sin: f1("sin"); break;
        case KOp::Cos: f1("cos"); break;
        case KOp::Exp: f1("exp"); break;
        case KOp::Pow: f2("pow"); break;
        case KOp::Log: f1("log"); break;
        case KOp::Log2: f1("log2"); break;
        case KOp::Tanh: f1("tanh"); break;
        case KOp::Atan2: f2("atan2"); break;
        case KOp::Atan: f1("atan"); break;
        case KOp::Asin: f1("asin"); break;
        case KOp::Acos: f1("acos"); break;
        case KOp::Sinh: f1("sinh"); break;
        case KOp::Cosh: f1("cosh"); break;
        case KOp::Floor: f1("floor"); break;
        case KOp::Add: b2(" + "); break;
        case KOp::Sub: b2(" - "); break;
        case KOp::Mul: b2(" * "); break;
        case KOp::Div: b2(" / "); break;
        case KOp::Min: f2("min"); break;
        case KOp::Max: f2("max"); break;
        case KOp::Mod: if (dt_is_int(nd.dtype()) || dt_is_uint(nd.dtype())) { b2(" % "); } else { f2("fmod"); } break;
        case KOp::Fma: s.append("fma("); pv(pv, nd.a); s.append(", "); pv(pv, nd.b); s.append(", "); pv(pv, nd.c); s.append(")"); break;
        default: ok = false; s.append("0"); break;
        }
    };
    const auto decl = [&](auto&& self, int node) -> void {
        const KNode& nd = g.node(node);
        if (nd.op == KOp::BufferLoad || nd.op == KOp::SharedLoad) { self(self, nd.b); return; } // resource leaf: only the index carries temps
        if (nd.a >= 0) { self(self, nd.a); }
        if (nd.b >= 0) { self(self, nd.b); }
        if (nd.c >= 0) { self(self, nd.c); }
        if (!is_inline_op(nd.op) && temped[static_cast<crd::usize>(node)] == 0U)
        {
            temped[static_cast<crd::usize>(node)] = 1U;
            s.append(is_float_dtype(nd.dtype()) ? "  precise " : "  ");
            s.append(ctype(nd.dtype())); s.append(" t"); app_uint(s, static_cast<crd::u32>(node)); s.append(" = ");
            rhs(nd);
            s.append(";\n");
        }
    };
    const auto emit_body = [&](auto&& self_b, int begin, int count) -> void {
        int i = begin;
        while (i < begin + count) // a For/If body lives CONTIGUOUSLY after it → recurse then SKIP past it (never re-emit)
        {
            const KStmt& st = g.stmt(i);
            switch (st.kind)
            {
            case KStmtKind::BufferStore: { // bufN.Store(idx*4, asuint(val)) — RAW byte-addressed UAV; uint stores bare
                const DType bt = g.node(st.target).dtype();
                if (bt == DType::F64) { ok = false; ++i; break; }
                decl(decl, st.index); decl(decl, st.value);
                s.append("  buf"); app_uint(s, g.node(st.target).iidx); s.append(".Store(("); pv(pv, st.index); s.append(") * 4u, ");
                if (dt_is_uint(bt)) { pv(pv, st.value); } else { s.append("asuint("); pv(pv, st.value); s.append(")"); }
                s.append(");\n");
                ++i;
                break;
            }
            case KStmtKind::SharedStore: decl(decl, st.index); decl(decl, st.value); s.append("  sh"); app_uint(s, st.target); s.append("["); pv(pv, st.index); s.append("] = "); pv(pv, st.value); s.append(";\n"); ++i; break;
            case KStmtKind::Barrier: s.append(st.scope == BarrierScope::Workgroup ? "  GroupMemoryBarrierWithGroupSync();\n" : "  DeviceMemoryBarrierWithGroupSync();\n"); ++i; break;
            case KStmtKind::Materialize: // FREEZE st.value into a temp NOW (survives a later shared overwrite)
                decl(decl, st.value);
                if (temped[static_cast<crd::usize>(st.value)] == 0U)
                {
                    s.append(is_float_dtype(g.node(st.value).dtype()) ? "  precise " : "  ");
                    s.append(ctype(g.node(st.value).dtype())); s.append(" t"); app_uint(s, static_cast<crd::u32>(st.value)); s.append(" = ");
                    pv(pv, st.value); s.append(";\n");
                    temped[static_cast<crd::usize>(st.value)] = 1U;
                }
                ++i;
                break;
            case KStmtKind::For: decl(decl, st.value); s.append("  for (uint lv"); app_uint(s, i); s.append(" = 0u; lv"); app_uint(s, i); s.append(" < uint("); pv(pv, st.value); s.append("); ++lv"); app_uint(s, i); s.append(") {\n"); self_b(self_b, st.body_begin, st.body_count); s.append("  }\n"); i = st.body_begin + st.body_count; break;
            case KStmtKind::If: decl(decl, st.value); s.append("  if ("); pv(pv, st.value); s.append(") {\n"); self_b(self_b, st.body_begin, st.body_count); s.append("  }\n"); i = st.body_begin + st.body_count; break;
            case KStmtKind::SpinUntilNonzero: decl(decl, st.index); s.append("  while (buf"); app_uint(s, g.node(st.target).iidx); s.append(".Load((("); pv(pv, st.index); s.append(")) * 4u) == 0u) { DeviceMemoryBarrier(); }\n"); ++i; break;
            case KStmtKind::SharedAtomicAdd: decl(decl, st.index); decl(decl, st.value); s.append("  { uint oldv_; InterlockedAdd(sh"); app_uint(s, st.target); s.append("["); pv(pv, st.index); s.append("], "); pv(pv, st.value); s.append(", oldv_); }\n"); ++i; break;
            case KStmtKind::BufferAtomicAdd: decl(decl, st.index); decl(decl, st.value); s.append("  buf"); app_uint(s, g.node(st.target).iidx); s.append(".InterlockedAdd((("); pv(pv, st.index); s.append(")) * 4u, "); pv(pv, st.value); s.append(");\n"); ++i; break; // RAW byte-addressed UAV
            case KStmtKind::ForBreakIf: decl(decl, st.value); s.append("  if (("); pv(pv, st.value); s.append(") != 0u) break;\n"); ++i; break;
            case KStmtKind::BufferTicket: decl(decl, st.index); s.append("  if (lidx == 0u) { uint tick_; buf"); app_uint(s, g.node(st.target).iidx); s.append(".InterlockedAdd((("); pv(pv, st.index); s.append(")) * 4u, 1u, tick_); sh"); app_uint(s, st.value); s.append("[0] = tick_; }\n"); ++i; break;
            case KStmtKind::SyncWarp: s.append("  GroupMemoryBarrierWithGroupSync();\n"); ++i; break; // no wave barrier in HLSL — conservative
            }
        }
    };
    emit_body(emit_body, entry.kernel_body_begin, entry.kernel_body_count);
    s.append("}\n");
    return ok;
}

// The bare HLSL matrix spelling: `floatRxC` = R ROWS by C columns. This is the TRANSPOSE of GLSL's `matCxR`
// (ckir_glsl.hpp) -- the single most dangerous asymmetry in this file. It is also why every matrix construction below
// is wrapped in `transpose(...)`: HLSL's matrix constructor fills ROW-major, so feeding it our C column-vectors builds
// the CxR transpose, and one `transpose` puts it back. Verified by the `M * inverse(M) * v == v` GPU test, never by eye.
inline const char* hmat(int rows, int cols) noexcept
{
    switch (rows * 10 + cols)
    {
    case 22: return "float2x2"; case 23: return "float2x3"; case 24: return "float2x4";
    case 32: return "float3x2"; case 33: return "float3x3"; case 34: return "float3x4";
    case 42: return "float4x2"; case 43: return "float4x3"; case 44: return "float4x4";
    default: return "float";
    }
}
// HLSL type name for a CKIR value type (a comps==4 value is a float4 OR a float2x2 -- only the type knows which).
// Vectors carry the component scalar: floatN / intN / uintN / boolN (B0-3).
inline const char* htype(KType t) noexcept
{
    if (t.kind == TKind::Mat) { return hmat(t.rows, t.cols); }
    if (t.kind == TKind::Vec)
    {
        if (t.scalar == DType::Bool) { switch (t.rows) { case 2: return "bool2"; case 3: return "bool3"; case 4: return "bool4"; default: break; } }
        else if (glsl_detail::dt_is_uint(t.scalar)) { switch (t.rows) { case 2: return "uint2"; case 3: return "uint3"; case 4: return "uint4"; default: break; } }
        else if (glsl_detail::dt_is_int(t.scalar)) { switch (t.rows) { case 2: return "int2"; case 3: return "int3"; case 4: return "int4"; default: break; } }
        else { switch (t.rows) { case 2: return "float2"; case 3: return "float3"; case 4: return "float4"; default: break; } }
    }
    return glsl_detail::ctype(t.scalar);
}

// B2: HLSL texture typing. `Texture<dim><, sampled4>` (e.g. `Texture2D<float4>`); `Sampler[Comparison]State`; the sampled
// element is the scalar widened to 4 (`float4`/`int4`/`uint4`). Mirror of the GLSL prefix/suffix helpers.
inline const char* hlsl_tex_dim(const KType& t) noexcept
{
    switch (t.tex_dim())
    {
    case TexDim::Tex1D:   return t.tex_arrayed() ? "Texture1DArray" : "Texture1D";
    case TexDim::Tex2D:   if (t.tex_ms()) { return "Texture2DMS"; } return t.tex_arrayed() ? "Texture2DArray" : "Texture2D";
    case TexDim::Tex3D:   return "Texture3D";
    case TexDim::TexCube: return t.tex_arrayed() ? "TextureCubeArray" : "TextureCube";
    }
    return "Texture2D";
}
inline const char* hlsl_tex_elem(DType d) noexcept
{
    if (glsl_detail::dt_is_uint(d)) { return "uint4"; }
    if (glsl_detail::dt_is_int(d)) { return "int4"; }
    return "float4";
}

// A3: comps-aware VECTOR emitter for HLSL/DX12 (mirror of emit_vec_glsl) — float/float2/3/4 temps, interleaved I/O,
// HLSL builtins + emitted quaternion helpers. Matrices (comps>4) are deferred (HLSL row-major convention) ⇒ bail cleanly.
// B3-d: emit the `precise? <htype> tN = ` statement prefix for node `i` (`precise` is float-only). Shared by the HLSL
// compute + raster statement paths (mirror of `emit_stmt_prefix` in ckir_glsl.hpp).
inline void emit_stmt_prefix_hlsl(const KGraph& g, int i, crd::containers::String& s)
{
    const KNode& nd = g.node(i);
    s.append(glsl_detail::is_float_dtype(nd.dtype()) ? "  precise " : "  ");
    s.append(htype(nd.type));
    s.append(" t");
    glsl_detail::app_uint(s, static_cast<crd::u32>(i));
    s.append(" = ");
}

// B3-d: the SHARED HLSL value-expression statement emitter — one home for the ~60 operation cases so the compute path AND
// the raster (VS/FS) path never duplicate them (mirror of `emit_value_stmt` in ckir_glsl.hpp). `leaf(g, i, s)` emits the
// WHOLE statement for a STAGE-SPECIFIC leaf (compute `Input` → a buffer read; raster `StageIn`/`Builtin` → a stage input,
// UBO `FieldGet` → `ubo.member`) and returns true when it handled node `i`. Returns false on an op it cannot lower.
template <typename LeafFn>
inline bool emit_value_stmt_hlsl(const KGraph& g, int i, crd::containers::String& s, const LeafFn& leaf)
{
    using namespace glsl_detail;
    const KNode& nd = g.node(i);
    const int    c  = nd.comps();
    const char   xyzw[4] = {'x', 'y', 'z', 'w'};
    const auto   ta      = [&](int id) { s.append("t"); app_uint(s, static_cast<crd::u32>(id)); };
    // B2: `tex_S_B.` (from operand a) and `samp_S_B` (from operand b) — HLSL is inherently separable.
    const auto   tex_dot  = [&]() { const KNode& tx = g.node(nd.a); s.append("tex_"); app_uint(s, static_cast<crd::u32>(tx.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(tx.iidx)); s.append("."); };
    const auto   samp_ref = [&]() { const KNode& sm = g.node(nd.b); s.append("samp_"); app_uint(s, static_cast<crd::u32>(sm.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(sm.iidx)); };

    if (nd.op == KOp::StructMake || nd.op == KOp::ArrayMake) { return true; } // B0-4 SROA: aggregates materialize nothing
    if (nd.op == KOp::TexSize) // B2-b: HLSL GetDimensions is a void method with out-params — emit its own statement pair.
    {
        const KNode& tx = g.node(nd.a);
        s.append("  uint2 t"); app_uint(s, static_cast<crd::u32>(i)); s.append("_gd; tex_"); app_uint(s, static_cast<crd::u32>(tx.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(tx.iidx));
        s.append(".GetDimensions(t"); app_uint(s, static_cast<crd::u32>(i)); s.append("_gd.x, t"); app_uint(s, static_cast<crd::u32>(i)); s.append("_gd.y);\n");
        s.append("  int2 t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = int2(t"); app_uint(s, static_cast<crd::u32>(i)); s.append("_gd);\n");
        return true;
    }
    if (nd.op == KOp::FieldGet || nd.op == KOp::ArrayGet)
    {
        const KNode& agg = g.node(nd.a);
        if (agg.op == KOp::StructMake || agg.op == KOp::ArrayMake) { emit_stmt_prefix_hlsl(g, i, s); ta(g.ext_operand(agg, nd.iidx)); s.append(";\n"); return true; }
        return leaf(g, i, s); // raster UBO member (compute leaf returns false ⇒ matches the old `return false`)
    }
    if (leaf(g, i, s)) { return true; } // stage-specific leaf owns the whole statement (Input / StageIn / Builtin)

    emit_stmt_prefix_hlsl(g, i, s);
    switch (nd.op)
    {
    // Emit an integer literal for int/uint constants (parity with GLSL; avoids `int t = 0.0` and truncation surprises).
    // UNSIGNED gets the `u` suffix so 32-bit masks / hash seeds > INT_MAX (B6-b noise) are valid `uint` literals.
    case KOp::Const: if (dt_is_int(nd.dtype()) || dt_is_uint(nd.dtype())) { app_ilit(s, nd.cval); if (dt_is_uint(nd.dtype())) { s.append("u"); } } else { app_flit(s, nd.cval); } break;
    case KOp::Cast: s.append(htype(nd.type)); s.append("("); ta(nd.a); s.append(")"); break;
    case KOp::Neg: s.append("-"); ta(nd.a); break;
    case KOp::Recip: s.append("(1.0 / "); ta(nd.a); s.append(")"); break;
    case KOp::Abs: s.append("abs("); ta(nd.a); s.append(")"); break;
    case KOp::DFdx: s.append("ddx("); ta(nd.a); s.append(")"); break;    // B1 fragment derivative ∂/∂x
    case KOp::DFdy: s.append("ddy("); ta(nd.a); s.append(")"); break;    // B1 fragment derivative ∂/∂y
    case KOp::Fwidth: s.append("fwidth("); ta(nd.a); s.append(")"); break; // B1 |ddx|+|ddy|
    case KOp::StorageLoad: s.append("sbuf["); ta(nd.a); s.append("]"); break; // B1-f: read the FS storage buffer at index `a`
    case KOp::TexSample:  tex_dot(); s.append("Sample(");      samp_ref(); s.append(", "); ta(nd.c); s.append(")"); break; // B2 implicit-LOD
    case KOp::SampleLod:  tex_dot(); s.append("SampleLevel("); samp_ref(); s.append(", "); ta(nd.c); s.append(", "); ta(nd.d); s.append(")"); break; // B2-b
    case KOp::SampleGrad: tex_dot(); s.append("SampleGrad(");  samp_ref(); s.append(", "); ta(nd.c); s.append(", "); ta(nd.d); s.append(", "); ta(g.ext_operand(nd, 0)); s.append(")"); break; // B2-b
    case KOp::SampleCmp:  tex_dot(); s.append("SampleCmp(");   samp_ref(); s.append(", "); ta(nd.c); s.append(", "); ta(nd.d); s.append(")"); break; // B2-b shadow → float
    case KOp::TexelFetch: // B2-b integer fetch (no filtering). A 2DMS texture's Load takes (coord, sampleIndex); others int3(coord, lod).
        if (g.node(nd.a).type.tex_ms()) { tex_dot(); s.append("Load("); ta(nd.c); s.append(", "); ta(nd.d); s.append(")"); }
        else { tex_dot(); s.append("Load(int3("); ta(nd.c); s.append(", "); ta(nd.d); s.append("))"); }
        break;
    case KOp::SampleIndexed: // B2-d: index a bindless texture ARRAY (NonUniformResourceIndex — the index varies per fragment).
    {
        const KNode& tx = g.node(nd.a);
        s.append("tex_"); app_uint(s, static_cast<crd::u32>(tx.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(tx.iidx));
        s.append("[NonUniformResourceIndex("); ta(nd.d); s.append(")].Sample("); samp_ref(); s.append(", "); ta(nd.c); s.append(")");
        break;
    }
    case KOp::SampleIndexedLod: // B16: bindless ARRAY sample at an EXPLICIT LOD (VS displacement — no derivatives). lod in ext[0].
    {
        const KNode& tx = g.node(nd.a);
        s.append("tex_"); app_uint(s, static_cast<crd::u32>(tx.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(tx.iidx));
        s.append("[NonUniformResourceIndex("); ta(nd.d); s.append(")].SampleLevel("); samp_ref(); s.append(", "); ta(nd.c); s.append(", "); ta(g.ext_operand(nd, 0)); s.append(")");
        break;
    }
    case KOp::TexGather: // B2-b: HLSL's channel is baked into the method name (GatherRed/Green/Blue/Alpha); comp is a literal.
    {
        const char* g4[4] = {"GatherRed", "GatherGreen", "GatherBlue", "GatherAlpha"};
        const int   comp  = static_cast<int>(g.node(nd.d).cval) & 3;
        tex_dot(); s.append(g4[comp]); s.append("("); samp_ref(); s.append(", "); ta(nd.c); s.append(")");
        break;
    }
    case KOp::Sqrt: s.append("sqrt("); ta(nd.a); s.append(")"); break;
    case KOp::Rsqrt: s.append("rsqrt("); ta(nd.a); s.append(")"); break;
    case KOp::Exp: s.append("exp("); ta(nd.a); s.append(")"); break;
    case KOp::Log: s.append("log("); ta(nd.a); s.append(")"); break;
    case KOp::Sin: s.append("sin("); ta(nd.a); s.append(")"); break;
    case KOp::Cos: s.append("cos("); ta(nd.a); s.append(")"); break;
    case KOp::Floor: s.append("floor("); ta(nd.a); s.append(")"); break;
    case KOp::Fract: s.append("frac("); ta(nd.a); s.append(")"); break;
    case KOp::Add: ta(nd.a); s.append(" + "); ta(nd.b); break;
    case KOp::Sub: ta(nd.a); s.append(" - "); ta(nd.b); break;
    case KOp::Mul: ta(nd.a); s.append(" * "); ta(nd.b); break;
    case KOp::Div: ta(nd.a); s.append(" / "); ta(nd.b); break;
    case KOp::Min: s.append("min("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::Max: s.append("max("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::Pow: s.append("pow("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::Clamp: s.append("clamp("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::Mix: s.append("lerp("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::Vec2: s.append("float2("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::Vec3: s.append("float3("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::VecConcat: s.append(htype(nd.type)); s.append("("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::VecComp: ta(nd.a); s.append("."); { const char sw[2] = {xyzw[nd.iidx], '\0'}; s.append(sw); } break;
    case KOp::Swizzle: ta(nd.a); s.append("."); for (int k = 0; k < c; ++k) { const char sw[2] = {xyzw[nd.perm[k]], '\0'}; s.append(sw); } break;
    case KOp::Splat: s.append(htype(nd.type)); s.append("("); for (int k = 0; k < c; ++k) { if (k) { s.append(", "); } ta(nd.a); } s.append(")"); break;
    case KOp::Dot: s.append("dot("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::Cross: s.append("cross("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::Normalize: s.append("normalize("); ta(nd.a); s.append(")"); break;
    case KOp::VecLen: s.append("length("); ta(nd.a); s.append(")"); break;
    case KOp::Reflect: s.append("reflect("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::Refract: s.append("refract("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::Faceforward: s.append("faceforward("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::VecAny: s.append("any("); ta(nd.a); s.append(")"); break;
    case KOp::VecAll: s.append("all("); ta(nd.a); s.append(")"); break;
    case KOp::CmpLt: s.append("("); ta(nd.a); s.append(" < "); ta(nd.b); s.append(")"); break;
    case KOp::CmpLe: s.append("("); ta(nd.a); s.append(" <= "); ta(nd.b); s.append(")"); break;
    case KOp::CmpGt: s.append("("); ta(nd.a); s.append(" > "); ta(nd.b); s.append(")"); break;
    case KOp::CmpGe: s.append("("); ta(nd.a); s.append(" >= "); ta(nd.b); s.append(")"); break;
    case KOp::CmpEq: s.append("("); ta(nd.a); s.append(" == "); ta(nd.b); s.append(")"); break;
    case KOp::CmpNe: s.append("("); ta(nd.a); s.append(" != "); ta(nd.b); s.append(")"); break;
    case KOp::Select: s.append("("); if (g.node(nd.c).dtype() == DType::Bool) { ta(nd.c); } else { s.append("("); ta(nd.c); s.append(" != 0.0)"); } s.append(" ? "); ta(nd.a); s.append(" : "); ta(nd.b); s.append(")"); break;
    case KOp::Slerp: s.append("crd_slerp("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::QuatMul: s.append("crd_qmul("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::QuatConj: s.append("crd_qconj("); ta(nd.a); s.append(")"); break;
    case KOp::QuatRotate: s.append("crd_qrot("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::QuatAxisAngle: s.append("crd_qaa("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::MatFromCols: { const int mcols = nd.type.cols; const int operand[4] = {nd.a, nd.b, nd.c, nd.d}; s.append("transpose("); s.append(hmat(mcols, nd.type.rows)); s.append("("); for (int k = 0; k < mcols; ++k) { if (k) { s.append(", "); } ta(operand[k]); } s.append("))"); break; }
    case KOp::MatVecMul: // HLSL spells both as mul(); the row-major construct at MatFromCols keeps the convention
    case KOp::MatMatMul: s.append("mul("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::MatTranspose: s.append("transpose("); ta(nd.a); s.append(")"); break;
    case KOp::Determinant: s.append("determinant("); ta(nd.a); s.append(")"); break;
    case KOp::MatInverse: s.append(nd.type.rows == 2 ? "crd_inv2(" : "crd_inv3("); ta(nd.a); s.append(")"); break;
    case KOp::OuterProduct: { const int orows = nd.type.rows; s.append(hmat(orows, nd.type.cols)); s.append("("); for (int r = 0; r < orows; ++r) { if (r) { s.append(", "); } ta(nd.a); s.append("."); const char sw[2] = {xyzw[r], '\0'}; s.append(sw); s.append(" * "); ta(nd.b); } s.append(")"); break; }
    case KOp::QuatToMat3: s.append("crd_qmat("); ta(nd.a); s.append(")"); break;
    // B6: the remaining scalar-math + integer bitwise ops (parity with the GLSL raster emitter + the HLSL compute path).
    // Materials (B6-a) use trunc/ceil/round/sign/tan/asin/acos/atan2/smoothstep; noise (B6-b) uses the bitwise ops for the
    // Bob-Jenkins hash (uint → logical >>).
    case KOp::Tanh: s.append("tanh("); ta(nd.a); s.append(")"); break;
    case KOp::Trunc: s.append("trunc("); ta(nd.a); s.append(")"); break;
    case KOp::Ceil: s.append("ceil("); ta(nd.a); s.append(")"); break;
    case KOp::Round: s.append("round("); ta(nd.a); s.append(")"); break; // HLSL round = ties-to-even, matches the oracle
    case KOp::Sign: s.append("(("); ta(nd.a); s.append(" > 0.0) ? 1.0 : (("); ta(nd.a); s.append(" < 0.0) ? -1.0 : 0.0))"); break;
    case KOp::Exp2: s.append("exp2("); ta(nd.a); s.append(")"); break;
    case KOp::Log2: s.append("log2("); ta(nd.a); s.append(")"); break;
    case KOp::Tan: s.append("tan("); ta(nd.a); s.append(")"); break;
    case KOp::Radians: s.append("("); ta(nd.a); s.append(" * 0.017453292519943295)"); break;
    case KOp::Degrees: s.append("("); ta(nd.a); s.append(" * 57.29577951308232)"); break;
    case KOp::Asin: s.append("asin("); ta(nd.a); s.append(")"); break;
    case KOp::Acos: s.append("acos("); ta(nd.a); s.append(")"); break;
    case KOp::Atan: s.append("atan("); ta(nd.a); s.append(")"); break;
    case KOp::Sinh: s.append("sinh("); ta(nd.a); s.append(")"); break;
    case KOp::Cosh: s.append("cosh("); ta(nd.a); s.append(")"); break;
    case KOp::Cbrt: s.append("(sign("); ta(nd.a); s.append(") * pow(abs("); ta(nd.a); s.append("), 0.3333333333333333))"); break;
    case KOp::Atan2: s.append("atan2("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::Mod: s.append("fmod("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break; // C fmod (sign of x)
    case KOp::Step: s.append("(("); ta(nd.b); s.append(" < "); ta(nd.a); s.append(") ? 0.0 : 1.0)"); break;
    case KOp::Smoothstep: s.append("smoothstep("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::Fma: s.append("fma("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::Shl: ta(nd.a); s.append(" << "); ta(nd.b); break;
    case KOp::Shr: ta(nd.a); s.append(" >> "); ta(nd.b); break;
    case KOp::BitAnd: ta(nd.a); s.append(" & "); ta(nd.b); break;
    case KOp::BitOr: ta(nd.a); s.append(" | "); ta(nd.b); break;
    case KOp::BitXor: ta(nd.a); s.append(" ^ "); ta(nd.b); break;
    case KOp::BitNot: s.append("(~"); ta(nd.a); s.append(")"); break; // B12-a: raster emitter lagged compute on these bit ops
    case KOp::BitCount: s.append("countbits("); ta(nd.a); s.append(")"); break;
    case KOp::FindLSB: s.append("firstbitlow("); ta(nd.a); s.append(")"); break;
    case KOp::FindMSB: s.append("firstbithigh("); ta(nd.a); s.append(")"); break;
    default: return false;
    }
    s.append(";\n");
    return true;
}

// B3-d: for a VS/FS builtin, the HLSL member type + `SV_` semantic (the input struct carries it). nullptr = unsupported.
// (The member type is the SV-native one — `SV_VertexID` is `uint`, `SV_Position` is `float4` — the read converts to the
// KIR builtin type via the temp's declared type.)
[[nodiscard]] inline bool hlsl_vsfs_builtin(KBuiltin b, const char*& hlsl_type, const char*& sv) noexcept
{
    switch (b)
    {
    case KBuiltin::VertexIndex:   hlsl_type = "uint";   sv = "SV_VertexID";     return true;
    case KBuiltin::InstanceIndex: hlsl_type = "uint";   sv = "SV_InstanceID";   return true;
    case KBuiltin::FragCoord:     hlsl_type = "float4"; sv = "SV_Position";     return true;
    case KBuiltin::FrontFacing:   hlsl_type = "bool";   sv = "SV_IsFrontFace";  return true;
    case KBuiltin::InnerCoverage: hlsl_type = "uint";   sv = "SV_InnerCoverage"; return true; // B1-f: bit0 = fully covered
    default:                      return false;
    }
}

// B1-c: the HLSL interpolation modifier (trailing space; goes before the member type). `nointerpolation` = GLSL `flat`.
[[nodiscard]] inline const char* hlsl_interp(Interp i) noexcept
{
    switch (i)
    {
    case Interp::Flat:          return "nointerpolation ";
    case Interp::NoPerspective: return "noperspective ";
    case Interp::Centroid:      return "centroid ";
    case Interp::Sample:        return "sample ";
    default:                    return ""; // linear (default)
    }
}

// B1-d: the depth output SEMANTIC for a `frag_depth` write. Conservative depth (Greater/Less) keeps early-Z even while
// the shader writes depth; plain `Any` writes unconstrained depth (defeats early-Z).
[[nodiscard]] inline const char* hlsl_depth_semantic(DepthMode m) noexcept
{
    switch (m)
    {
    case DepthMode::Greater: return "SV_DepthGreaterEqual";
    case DepthMode::Less:    return "SV_DepthLessEqual";
    default:                 return "SV_Depth";
    }
}

// B3-d: emit a VERTEX or FRAGMENT HLSL shader from a stage `entry` — the DX12 mirror of `emit_stage_glsl`. Reuses
// `emit_value_stmt_hlsl` for the value ops. HLSL raster I/O is STRUCT-based with `SV_` semantics; `[[vk::location(N)]]`
// pins the SPIR-V location so it matches the GLSL emitter. The RASTER LEAF resolves stage values: `StageIn`→`i.aL`,
// `Builtin`→`i.biN`, UBO `FieldGet`→a cbuffer member `uS_B_fN`. An unlowerable builtin/op returns false.
inline bool emit_stage_hlsl(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    if (entry.stage != KStage::Vertex && entry.stage != KStage::Fragment) { return false; }
    const bool is_vertex = (entry.stage == KStage::Vertex);
    if (is_vertex && entry.position < 0) { return false; }

    const int                       n = g.size();
    crd::containers::Array<crd::u8> reach(scratch);
    crd::containers::Array<int>     stk(scratch);
    reach.resize(static_cast<crd::usize>(n), 0);
    const auto push_root = [&](int r) { if (r >= 0) { stk.push_back(r); } };
    push_root(entry.position);
    push_root(entry.frag_depth);
    push_root(entry.discard_cond); // B1-b: the alpha-test condition must be reachable so its temp is emitted
    push_root(entry.shading_rate); // B1-e: per-primitive VRS rate node must be reachable
    push_root(entry.storage_write_index); // B1-f: the storage write's index + value must be reachable
    push_root(entry.storage_write_value);
    for (int k = 0; k < entry.n_out; ++k) { push_root(entry.out[k].node); }
    while (stk.size() > 0)
    {
        const int i = stk[stk.size() - 1];
        stk.resize(stk.size() - 1);
        if (i < 0 || reach[static_cast<crd::usize>(i)]) { continue; }
        reach[static_cast<crd::usize>(i)] = 1;
        const KNode& nd = g.node(i);
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
        if (nd.d >= 0) { stk.push_back(nd.d); }
        for (int e = 0; e < static_cast<int>(nd.n_ext); ++e) { stk.push_back(g.ext_operand(nd, e)); }
    }

    crd::containers::String& s = out.source;
    s.clear();
    // input struct: StageIn (location) + Builtin (SV_) members
    s.append(is_vertex ? "struct VSIn {\n" : "struct PSIn {\n");
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)] || g.node(i).op != KOp::StageIn) { continue; }
        const KNode& nd = g.node(i);
        s.append("  [[vk::location("); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(")]] ");
        if (!is_vertex) { s.append(hlsl_interp(static_cast<Interp>(nd.dset))); } // B1-c: interp on FS interpolant inputs
        s.append(htype(nd.type)); s.append(" a"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(" : TEXCOORD"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(";\n");
    }
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)] || g.node(i).op != KOp::Builtin) { continue; }
        const char* bt = nullptr;
        const char* sv = nullptr;
        if (!hlsl_vsfs_builtin(static_cast<KBuiltin>(g.node(i).iidx), bt, sv)) { return false; } // unsupported builtin
        s.append("  ");
        // B1-d: DXIL forbids the default `linear noperspective` on the SV_Position input when the shader outputs
        // conservative depth (SV_DepthGreaterEqual/LessEqual) — it must be `noperspective centroid` (or run per-sample).
        if (!is_vertex && entry.depth_mode != DepthMode::Any
            && static_cast<KBuiltin>(g.node(i).iidx) == KBuiltin::FragCoord)
        {
            s.append("noperspective centroid ");
        }
        s.append(bt); s.append(" bi"); app_uint(s, static_cast<crd::u32>(g.node(i).iidx)); s.append(" : "); s.append(sv); s.append(";\n");
    }
    s.append("};\n");
    // output struct: StageOutputs (VS interpolants / FS colour attachments) + SV_Position (VS) + SV_Depth (FS).
    // ⚠ SV_Position is emitted LAST in VSOut, NOT first: DXIL matches inter-stage USER varyings by packed register, and a
    // leading `SV_Position` steals output register 0 — pushing TEXCOORD0 to register 1 while the fragment PSIn packs it at
    // register 0 (its builtins trail the StageIns), so the graphics PSO fails link with E_INVALIDARG. Declaring the user
    // varyings first packs them at registers 0..N-1 on BOTH sides. (SPIR-V is immune — it matches by explicit vk::location
    // and Position is a no-location builtin — so the Vulkan HLSL gate is unaffected by the order.)
    s.append(is_vertex ? "struct VSOut {\n" : "struct PSOut {\n");
    for (int k = 0; k < entry.n_out; ++k)
    {
        const int nid = entry.out[k].node;
        if (nid < 0) { continue; }
        if (is_vertex) { s.append("  [[vk::location("); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(")]] "); s.append(hlsl_interp(entry.out[k].interp)); s.append(htype(g.node(nid).type)); s.append(" o"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(" : TEXCOORD"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(";\n"); }
        else { s.append("  "); s.append(htype(g.node(nid).type)); s.append(" o"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(" : SV_Target"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(";\n"); }
    }
    if (is_vertex) { s.append("  float4 clip : SV_Position;\n"); }
    if (is_vertex && entry.shading_rate >= 0) { s.append("  uint sr : SV_ShadingRate;\n"); } // B1-e: per-primitive VRS out
    if (!is_vertex && entry.frag_depth >= 0) // B1-d: conservative depth ⇒ the SV_Depth* semantic that keeps early-Z
    {
        s.append("  float o_depth : ");
        s.append(hlsl_depth_semantic(entry.depth_mode));
        s.append(";\n");
    }
    s.append("};\n");
    // uniform blocks -> cbuffer (register(bBinding, spaceSet)); members are HLSL globals, named u{S}_{B}_f{N}
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)] || g.node(i).op != KOp::UniformBlock) { continue; }
        const KNode& nd  = g.node(i);
        const int    sid = nd.type.struct_id;
        s.append("cbuffer U_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(" : register(b"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", space"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(") {\n");
        const int fc = g.struct_field_count(sid);
        for (int f = 0; f < fc; ++f) { s.append("  "); s.append(htype(g.struct_field(sid, f))); s.append(" u"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append("_f"); app_uint(s, static_cast<crd::u32>(f)); s.append(";\n"); }
        s.append("};\n");
    }
    bool fs_uses_storage = false; // B1-f: does the FS read/write the storage buffer (a StorageLoad or a storage write)?
    if (!is_vertex)
    {
        if (entry.storage_write_index >= 0) { fs_uses_storage = true; }
        for (int i = 0; !fs_uses_storage && i < n; ++i)
        {
            if (reach[static_cast<crd::usize>(i)] && g.node(i).op == KOp::StorageLoad) { fs_uses_storage = true; }
        }
    }
    if (fs_uses_storage) // B1-f: the FS storage buffer at u0 / set 0-binding 0 (matches draw_storage's root UAV / descriptor).
    {                    // ROV (RasterizerOrdered) when interlock — DXIL serialises overlapping-pixel access automatically.
        s.append("[[vk::binding(0, 0)]] ");
        s.append(entry.interlock ? "RasterizerOrderedStructuredBuffer<uint>" : "RWStructuredBuffer<uint>");
        s.append(" sbuf : register(u0);\n");
    }
    for (int i = 0; i < n; ++i) // B2: separable texture (register(tB, spaceS)) + sampler (register(sB, spaceS)) declarations
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        if (nd.op == KOp::Texture)
        {
            s.append("[[vk::binding("); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", "); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(")]] ");
            // A shadow/depth texture is single-channel (`Texture2D<float>`) so `.SampleCmp` is well-typed; colour is float4.
            s.append(hlsl_tex_dim(nd.type)); s.append("<"); s.append(nd.type.tex_shadow() ? "float" : hlsl_tex_elem(nd.type.scalar)); s.append("> tex_");
            app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx));
            if (nd.type.count > 1U) { s.append("["); app_uint(s, static_cast<crd::u32>(nd.type.count)); s.append("]"); } // B2-d: bindless array
            s.append(" : register(t"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", space"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(");\n");
        }
        else if (nd.op == KOp::Sampler)
        {
            s.append("[[vk::binding("); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", "); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(")]] ");
            s.append(nd.type.tex_shadow() ? "SamplerComparisonState" : "SamplerState"); s.append(" samp_"); // B2-b: comparison sampler
            app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx));
            s.append(" : register(s"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", space"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(");\n");
        }
    }
    { // quaternion/slerp + matrix helpers (shared with the compute HLSL path)
        bool qm = false;
        bool qc = false;
        bool qr = false;
        bool qa = false;
        bool sl = false;
        bool qt = false;
        bool iv = false;
        bool iv2 = false;
        for (int i = 0; i < n; ++i) { if (!reach[static_cast<crd::usize>(i)]) { continue; } switch (g.node(i).op) { case KOp::QuatMul: qm = true; break; case KOp::QuatConj: qc = true; break; case KOp::QuatRotate: qr = true; break; case KOp::QuatAxisAngle: qa = true; break; case KOp::Slerp: sl = true; break; case KOp::QuatToMat3: qt = true; break; case KOp::MatInverse: if (g.node(i).type.rows == 2) { iv2 = true; } else { iv = true; } break; default: break; } }
        if (qm) { s.append("float4 crd_qmul(float4 a,float4 b){return float4(a.w*b.xyz+b.w*a.xyz+cross(a.xyz,b.xyz),a.w*b.w-dot(a.xyz,b.xyz));}\n"); }
        if (qc) { s.append("float4 crd_qconj(float4 q){return float4(-q.xyz,q.w);}\n"); }
        if (qr) { s.append("float3 crd_qrot(float4 q,float3 v){float3 t=2.0*cross(q.xyz,v);return v+q.w*t+cross(q.xyz,t);}\n"); }
        if (qa) { s.append("float4 crd_qaa(float3 ax,float an){float h=an*0.5;return float4(ax*sin(h),cos(h));}\n"); }
        if (sl) { s.append("float4 crd_slerp(float4 a,float4 b,float t){float d=dot(a,b);float sg=1.0;if(d<0.0){d=-d;sg=-1.0;}if(d>0.9995){return normalize(lerp(a,sg*b,t));}float th=acos(d);float sn=sin(th);return (sin((1.0-t)*th)*a+sin(t*th)*sg*b)/sn;}\n"); }
        if (qt) { s.append("float3x3 crd_qmat(float4 q){float x=q.x,y=q.y,z=q.z,w=q.w;return float3x3(1.0-2.0*(y*y+z*z),2.0*(x*y-w*z),2.0*(x*z+w*y),2.0*(x*y+w*z),1.0-2.0*(x*x+z*z),2.0*(y*z-w*x),2.0*(x*z-w*y),2.0*(y*z+w*x),1.0-2.0*(x*x+y*y));}\n"); }
        if (iv2) { s.append("float2x2 crd_inv2(float2x2 m){float a=m._m00,b=m._m01,c=m._m10,d=m._m11;float iv=1.0/(a*d-b*c);return float2x2(d*iv,-b*iv,-c*iv,a*iv);}\n"); }
        if (iv) { s.append("float3x3 crd_inv3(float3x3 m){float a=m._m00,b=m._m01,c=m._m02,d=m._m10,e=m._m11,f=m._m12,g=m._m20,h=m._m21,i=m._m22;float A=e*i-f*h,B=d*i-f*g,C=d*h-e*g;float iv=1.0/(a*A-b*B+c*C);return float3x3(A*iv,(c*h-b*i)*iv,(b*f-c*e)*iv,(f*g-d*i)*iv,(a*i-c*g)*iv,(c*d-a*f)*iv,(d*h-e*g)*iv,(b*g-a*h)*iv,(a*e-b*d)*iv);}\n"); }
    }
    if (!is_vertex && entry.early_fragment_tests) { s.append("[earlydepthstencil]\n"); } // B1-d: force early-Z
    s.append(is_vertex ? "VSOut main(VSIn i) {\n  VSOut o;\n" : "PSOut main(PSIn i) {\n  PSOut o;\n");

    crd::containers::Array<crd::u8> varying(scratch);
    varying.resize(static_cast<crd::usize>(n), 0);
    for (int i = 0; i < n; ++i) { const KNode& v = g.node(i); if (v.op == KOp::For) { continue; } const bool loop_leaf = v.op == KOp::LoopIndex || v.op == KOp::LoopAcc; const bool from_operand = (v.a >= 0 && varying[static_cast<crd::usize>(v.a)]) || (v.b >= 0 && varying[static_cast<crd::usize>(v.b)]) || (v.c >= 0 && varying[static_cast<crd::usize>(v.c)]) || (v.d >= 0 && varying[static_cast<crd::usize>(v.d)]); if (loop_leaf || from_operand) { varying[static_cast<crd::usize>(i)] = 1; } }
    crd::containers::Array<int> body_of(scratch);
    body_of.resize(static_cast<crd::usize>(n), -1);
    crd::containers::Array<int> rstk(scratch);
    for (int fi = 0; fi < n; ++fi) { if (g.node(fi).op != KOp::For) { continue; } rstk.push_back(g.node(fi).c); while (rstk.size() > 0) { const int bid = rstk[rstk.size() - 1]; rstk.resize(rstk.size() - 1); if (bid < 0 || !varying[static_cast<crd::usize>(bid)] || body_of[static_cast<crd::usize>(bid)] != -1) { continue; } body_of[static_cast<crd::usize>(bid)] = fi; const KNode& bn = g.node(bid); rstk.push_back(bn.a); rstk.push_back(bn.b); rstk.push_back(bn.c); rstk.push_back(bn.d); } }

    const auto raster_leaf = [&](const KGraph& gg, int li, crd::containers::String& ss) -> bool
    {
        const KNode& lnd = gg.node(li);
        if (lnd.op == KOp::UniformBlock) { return true; } // the cbuffer materializes nothing; only its FieldGets read members
        if (lnd.op == KOp::Texture || lnd.op == KOp::Sampler) { return true; } // B2: opaque binding leaves — declared in the prologue
        if (lnd.op == KOp::StageIn) { emit_stmt_prefix_hlsl(gg, li, ss); ss.append("i.a"); app_uint(ss, static_cast<crd::u32>(lnd.iidx)); ss.append(";\n"); return true; }
        if (lnd.op == KOp::Builtin)
        {
            const char* bt = nullptr;
            const char* sv = nullptr;
            if (!hlsl_vsfs_builtin(static_cast<KBuiltin>(lnd.iidx), bt, sv)) { return false; }
            emit_stmt_prefix_hlsl(gg, li, ss); ss.append("i.bi"); app_uint(ss, static_cast<crd::u32>(lnd.iidx)); ss.append(";\n"); return true;
        }
        if (lnd.op == KOp::FieldGet)
        {
            const KNode& agg = gg.node(lnd.a);
            if (agg.op != KOp::UniformBlock) { return false; }
            emit_stmt_prefix_hlsl(gg, li, ss); ss.append("u"); app_uint(ss, static_cast<crd::u32>(agg.dset)); ss.append("_"); app_uint(ss, static_cast<crd::u32>(agg.iidx)); ss.append("_f"); app_uint(ss, static_cast<crd::u32>(lnd.iidx)); ss.append(";\n");
            return true;
        }
        return false;
    };

    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)] || varying[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        if (nd.op == KOp::For)
        {
            s.append("  precise "); s.append(htype(nd.type)); s.append(" t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nd.b)); s.append(";\n");
            s.append("  for (int li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = 0; li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(" < int(t"); app_uint(s, static_cast<crd::u32>(nd.a)); s.append("); li_"); app_uint(s, static_cast<crd::u32>(i)); s.append("++) {\n");
            for (int bid = 0; bid < i; ++bid)
            {
                if (body_of[static_cast<crd::usize>(bid)] != i) { continue; }
                const KNode& bn = g.node(bid);
                if (bn.op == KOp::LoopIndex) { s.append("  precise float t"); app_uint(s, static_cast<crd::u32>(bid)); s.append(" = float(li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(");\n"); }
                else if (bn.op == KOp::LoopAcc) { s.append("  precise "); s.append(htype(bn.type)); s.append(" t"); app_uint(s, static_cast<crd::u32>(bid)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(i)); s.append(";\n"); }
                else if (!emit_value_stmt_hlsl(g, bid, s, raster_leaf)) { return false; }
            }
            s.append("  t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nd.c)); s.append(";\n  }\n");
        }
        else if (!emit_value_stmt_hlsl(g, i, s, raster_leaf)) { return false; }
    }

    if (!is_vertex && entry.storage_write_index >= 0) // B1-f: the storage-buffer write (ROV serialises it when interlock)
    {
        s.append("  sbuf[t"); app_uint(s, static_cast<crd::u32>(entry.storage_write_index));
        s.append("] = t"); app_uint(s, static_cast<crd::u32>(entry.storage_write_value)); s.append(";\n");
    }
    if (!is_vertex && entry.discard_cond >= 0) // B1-b: alpha-test / cutout — kill the fragment before writing outputs
    {
        s.append("  if (t"); app_uint(s, static_cast<crd::u32>(entry.discard_cond)); s.append(") { discard; }\n");
    }
    if (is_vertex) { s.append("  o.clip = t"); app_uint(s, static_cast<crd::u32>(entry.position)); s.append(";\n"); }
    if (is_vertex && entry.shading_rate >= 0) { s.append("  o.sr = (uint)t"); app_uint(s, static_cast<crd::u32>(entry.shading_rate)); s.append(";\n"); } // B1-e
    for (int k = 0; k < entry.n_out; ++k) { const int nid = entry.out[k].node; if (nid < 0) { continue; } s.append("  o.o"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nid)); s.append(";\n"); }
    if (!is_vertex && entry.frag_depth >= 0) { s.append("  o.o_depth = t"); app_uint(s, static_cast<crd::u32>(entry.frag_depth)); s.append(";\n"); }
    s.append("  return o;\n}\n");
    return true;
}

// B4: MESH-shader emit for DX12 / HLSL (Shader Model 6.5). The DX12 analogue of emit_mesh_glsl: [outputtopology("triangle")] +
// [numthreads] + SetMeshOutputCounts, with the vertex/primitive outputs as `out vertices`/`out indices` array PARAMETERS (not
// gl_MeshVerticesEXT). Thread `tid` (SV_GroupIndex) writes vertex tid + primitive tid, guarded by the counts; `gid` (SV_GroupID)
// is the meshlet index. Reuses the raster value machinery; the workgroup builtins map to tid / gid.x. No StageIn (mesh generates
// geometry). SV_Position is emitted LAST in VOut (the DXIL register-packing scar — same as VSOut).
// B4: TASK / AMPLIFICATION emit (Shader Model 6.5 amplification shader). A task workgroup computes `task_emit` = the mesh-
// workgroup count + an optional single-uint `task_payload`, writes the groupshared payload, then `DispatchMesh(n,1,1,payload)`.
// The mesh reads the payload via `in payload MeshPayload mp`. Value graphs read the workgroup builtins (SV_GroupID / SV_GroupIndex
// / cbuffers). No geometry. Amplification count is a scalar expr; a loop-bearing task is refused (extend if ever needed).
inline bool emit_task_hlsl(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    if (entry.stage != KStage::Task || entry.task_emit < 0) { return false; }

    const int                       n = g.size();
    crd::containers::Array<crd::u8> reach(scratch);
    crd::containers::Array<int>     stk(scratch);
    reach.resize(static_cast<crd::usize>(n), 0);
    stk.push_back(entry.task_emit);
    if (entry.task_payload >= 0) { stk.push_back(entry.task_payload); }
    while (stk.size() > 0)
    {
        const int i = stk[stk.size() - 1];
        stk.resize(stk.size() - 1);
        if (i < 0 || reach[static_cast<crd::usize>(i)]) { continue; }
        reach[static_cast<crd::usize>(i)] = 1;
        const KNode& nd = g.node(i);
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
        if (nd.d >= 0) { stk.push_back(nd.d); }
        for (int e = 0; e < static_cast<int>(nd.n_ext); ++e) { stk.push_back(g.ext_operand(nd, e)); }
    }

    crd::containers::String& s = out.source;
    s.clear();
    for (int i = 0; i < n; ++i) // uniform blocks → cbuffer (a task may read uniforms to compute the count)
    {
        if (!reach[static_cast<crd::usize>(i)] || g.node(i).op != KOp::UniformBlock) { continue; }
        const KNode& nd  = g.node(i);
        const int    sid = nd.type.struct_id;
        s.append("cbuffer U_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(" : register(b"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", space"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(") {\n");
        const int fc = g.struct_field_count(sid);
        for (int f = 0; f < fc; ++f) { s.append("  "); s.append(htype(g.struct_field(sid, f))); s.append(" u"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append("_f"); app_uint(s, static_cast<crd::u32>(f)); s.append(";\n"); }
        s.append("};\n");
    }
    s.append("struct MeshPayload { uint v0; };\ngroupshared MeshPayload s_payload;\n");
    const crd::u32 ls = entry.local_size[0] > 0U ? entry.local_size[0] : 1U;
    s.append("[numthreads("); app_uint(s, ls); s.append(", 1, 1)]\nvoid main(uint tid : SV_GroupIndex, uint3 gid : SV_GroupID) {\n");

    const auto task_leaf = [&](const KGraph& gg, int li, crd::containers::String& ss) -> bool
    {
        const KNode& lnd = gg.node(li);
        if (lnd.op == KOp::UniformBlock) { return true; }
        if (lnd.op == KOp::Builtin)
        {
            const KBuiltin bi = static_cast<KBuiltin>(lnd.iidx);
            emit_stmt_prefix_hlsl(gg, li, ss);
            if (bi == KBuiltin::LocalInvocationIndex) { ss.append("tid"); }
            else if (bi == KBuiltin::WorkgroupIndex) { ss.append("gid.x"); }
            else { return false; }
            ss.append(";\n");
            return true;
        }
        if (lnd.op == KOp::FieldGet)
        {
            const KNode& agg = gg.node(lnd.a);
            if (agg.op != KOp::UniformBlock) { return false; }
            emit_stmt_prefix_hlsl(gg, li, ss); ss.append("u"); app_uint(ss, static_cast<crd::u32>(agg.dset)); ss.append("_"); app_uint(ss, static_cast<crd::u32>(agg.iidx)); ss.append("_f"); app_uint(ss, static_cast<crd::u32>(lnd.iidx)); ss.append(";\n");
            return true;
        }
        return false;
    };
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        if (g.node(i).op == KOp::For) { return false; } // a task's amplification count is a scalar expr — no loops
        if (!emit_value_stmt_hlsl(g, i, s, task_leaf)) { return false; }
    }
    if (entry.task_payload >= 0)
    {
        s.append("  s_payload.v0 = t"); app_uint(s, static_cast<crd::u32>(entry.task_payload)); s.append(";\n");
    }
    s.append("  DispatchMesh(t"); app_uint(s, static_cast<crd::u32>(entry.task_emit)); s.append(", 1, 1, s_payload);\n}\n");
    return true;
}

inline bool emit_mesh_hlsl(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    if (entry.stage != KStage::Mesh || entry.mesh_vertices == 0U || entry.position < 0 || entry.mesh_prim < 0) { return false; }
    const crd::u32 n_verts    = entry.mesh_vertices;
    const crd::u32 n_prims    = entry.mesh_primitives;
    const crd::u32 local_size = n_verts > n_prims ? n_verts : n_prims;

    const int                       n = g.size();
    crd::containers::Array<crd::u8> reach(scratch);
    crd::containers::Array<int>     stk(scratch);
    reach.resize(static_cast<crd::usize>(n), 0);
    const auto push_root = [&](int r) { if (r >= 0) { stk.push_back(r); } };
    push_root(entry.position);
    push_root(entry.mesh_prim);
    for (int k = 0; k < entry.n_out; ++k) { push_root(entry.out[k].node); }
    while (stk.size() > 0)
    {
        const int i = stk[stk.size() - 1];
        stk.resize(stk.size() - 1);
        if (i < 0 || reach[static_cast<crd::usize>(i)]) { continue; }
        reach[static_cast<crd::usize>(i)] = 1;
        const KNode& nd = g.node(i);
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
        if (nd.d >= 0) { stk.push_back(nd.d); }
        for (int e = 0; e < static_cast<int>(nd.n_ext); ++e) { stk.push_back(g.ext_operand(nd, e)); }
    }

    crd::containers::String& s = out.source;
    s.clear();
    s.append("struct VOut {\n"); // per-VERTEX output (SV_Position LAST — DXIL register packing)
    for (int k = 0; k < entry.n_out; ++k)
    {
        const int nid = entry.out[k].node;
        if (nid < 0) { continue; }
        s.append("  [[vk::location("); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(")]] ");
        s.append(hlsl_interp(entry.out[k].interp)); s.append(htype(g.node(nid).type)); s.append(" o"); app_uint(s, static_cast<crd::u32>(entry.out[k].location));
        s.append(" : TEXCOORD"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(";\n");
    }
    s.append("  float4 clip : SV_Position;\n};\n");
    for (int i = 0; i < n; ++i) // uniform blocks → cbuffer (same as raster)
    {
        if (!reach[static_cast<crd::usize>(i)] || g.node(i).op != KOp::UniformBlock) { continue; }
        const KNode& nd  = g.node(i);
        const int    sid = nd.type.struct_id;
        s.append("cbuffer U_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(" : register(b"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", space"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(") {\n");
        const int fc = g.struct_field_count(sid);
        for (int f = 0; f < fc; ++f) { s.append("  "); s.append(htype(g.struct_field(sid, f))); s.append(" u"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append("_f"); app_uint(s, static_cast<crd::u32>(f)); s.append(";\n"); }
        s.append("};\n");
    }
    for (int i = 0; i < n; ++i) // B2: texture + sampler bindings (same as raster)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        if (nd.op == KOp::Texture)
        {
            s.append("[[vk::binding("); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", "); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(")]] ");
            s.append(hlsl_tex_dim(nd.type)); s.append("<"); s.append(nd.type.tex_shadow() ? "float" : hlsl_tex_elem(nd.type.scalar)); s.append("> tex_");
            app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx));
            if (nd.type.count > 1U) { s.append("["); app_uint(s, static_cast<crd::u32>(nd.type.count)); s.append("]"); }
            s.append(" : register(t"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", space"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(");\n");
        }
        else if (nd.op == KOp::Sampler)
        {
            s.append("[[vk::binding("); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", "); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(")]] ");
            s.append(nd.type.tex_shadow() ? "SamplerComparisonState" : "SamplerState"); s.append(" samp_");
            app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx));
            s.append(" : register(s"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", space"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(");\n");
        }
    }

    bool reads_payload = false; // B4: does this mesh read the task→mesh payload (KBuiltin::TaskPayload)?
    for (int i = 0; i < n; ++i)
    {
        if (reach[static_cast<crd::usize>(i)] && g.node(i).op == KOp::Builtin
            && static_cast<KBuiltin>(g.node(i).iidx) == KBuiltin::TaskPayload)
        {
            reads_payload = true;
            break;
        }
    }
    if (reads_payload) { s.append("struct MeshPayload { uint v0; };\n"); }
    s.append("[outputtopology(\"triangle\")]\n[numthreads("); app_uint(s, local_size); s.append(", 1, 1)]\n");
    s.append("void main(uint tid : SV_GroupIndex, uint3 gid : SV_GroupID,\n");
    if (reads_payload) { s.append("          in payload MeshPayload mp,\n"); } // B4: the amplification payload input
    s.append("          out vertices VOut verts["); app_uint(s, n_verts); s.append("],\n");
    s.append("          out indices uint3 tris["); app_uint(s, n_prims); s.append("]) {\n");
    s.append("  SetMeshOutputCounts("); app_uint(s, n_verts); s.append("u, "); app_uint(s, n_prims); s.append("u);\n");

    const auto mesh_leaf = [&](const KGraph& gg, int li, crd::containers::String& ss) -> bool
    {
        const KNode& lnd = gg.node(li);
        if (lnd.op == KOp::UniformBlock) { return true; }
        if (lnd.op == KOp::Texture || lnd.op == KOp::Sampler) { return true; }
        if (lnd.op == KOp::Builtin)
        {
            const KBuiltin bi = static_cast<KBuiltin>(lnd.iidx);
            emit_stmt_prefix_hlsl(gg, li, ss);
            if (bi == KBuiltin::LocalInvocationIndex) { ss.append("tid"); }
            else if (bi == KBuiltin::WorkgroupIndex) { ss.append("gid.x"); }
            else if (bi == KBuiltin::TaskPayload) { ss.append("mp.v0"); } // B4: the task→mesh payload (in payload MeshPayload mp)
            else { return false; }
            ss.append(";\n");
            return true;
        }
        if (lnd.op == KOp::FieldGet)
        {
            const KNode& agg = gg.node(lnd.a);
            if (agg.op != KOp::UniformBlock) { return false; }
            emit_stmt_prefix_hlsl(gg, li, ss); ss.append("u"); app_uint(ss, static_cast<crd::u32>(agg.dset)); ss.append("_"); app_uint(ss, static_cast<crd::u32>(agg.iidx)); ss.append("_f"); app_uint(ss, static_cast<crd::u32>(lnd.iidx)); ss.append(";\n");
            return true;
        }
        return false;
    };

    // value statements (no For-scoping needed for the projected-grid math, but keep the pattern general via the shared emitter).
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        if (g.node(i).op == KOp::For) { return false; } // (mesh geometry graphs have no runtime loops yet)
        if (!emit_value_stmt_hlsl(g, i, s, mesh_leaf)) { return false; }
    }

    s.append("  if (tid < "); app_uint(s, n_verts); s.append("u) {\n");
    s.append("    verts[tid].clip = t"); app_uint(s, static_cast<crd::u32>(entry.position)); s.append(";\n");
    for (int k = 0; k < entry.n_out; ++k) { const int nid = entry.out[k].node; if (nid < 0) { continue; } s.append("    verts[tid].o"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nid)); s.append(";\n"); }
    s.append("  }\n");
    s.append("  if (tid < "); app_uint(s, n_prims); s.append("u) {\n");
    s.append("    tris[tid] = t"); app_uint(s, static_cast<crd::u32>(entry.mesh_prim)); s.append(";\n");
    s.append("  }\n}\n");
    return true;
}

inline bool emit_vec_hlsl(const KGraph& g, int output, crd::memory::IAllocator* scratch, GlslKernel& out)
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
        if (!is_vec_fusable(nd.op)) { return false; }
        if (nd.op == KOp::MatInverse && nd.type.rows == 4) { return false; } // mat4 inverse deferred on HLSL (huge cofactor formula); 2x2/3x3 have helpers
        for (int k = 0; k < static_cast<int>(nd.n_ext); ++k) { stk.push_back(g.ext_operand(nd, k)); } // B0-4 variadic operands
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
        if (nd.d >= 0) { stk.push_back(nd.d); }
    }
    crd::containers::Array<int> binding_of(scratch);
    binding_of.resize(static_cast<crd::usize>(n), -1);
    out.n_inputs = 0;
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        if (g.node(i).op == KOp::Input) { binding_of[static_cast<crd::usize>(i)] = out.n_inputs; out.input_iidx[out.n_inputs] = g.node(i).iidx; out.in_comps[out.n_inputs] = g.node(i).comps(); ++out.n_inputs; }
    }
    out.out_comps              = g.node(output).comps();
    crd::containers::String& s = out.source;
    s.clear();
    for (int b = 0; b < out.n_inputs; ++b) { s.append("RWStructuredBuffer<float> in"); app_uint(s, static_cast<crd::u32>(b)); s.append(" : register(u"); app_uint(s, static_cast<crd::u32>(b)); s.append(");\n"); }
    s.append("RWStructuredBuffer<float> outb : register(u"); app_uint(s, static_cast<crd::u32>(out.n_inputs)); s.append(");\n");
    s.append("cbuffer PC : register(b0) { uint n; };\n");
    { // quaternion/slerp + matrix helpers (no HLSL builtins for these)
        bool qm = false;
        bool qc = false;
        bool qr = false;
        bool qa = false;
        bool sl = false;
        bool qt = false;
        bool iv = false;
        bool iv2 = false;
        for (int i = 0; i < n; ++i) { if (!reach[static_cast<crd::usize>(i)]) { continue; } switch (g.node(i).op) { case KOp::QuatMul: qm = true; break; case KOp::QuatConj: qc = true; break; case KOp::QuatRotate: qr = true; break; case KOp::QuatAxisAngle: qa = true; break; case KOp::Slerp: sl = true; break; case KOp::QuatToMat3: qt = true; break; case KOp::MatInverse: if (g.node(i).type.rows == 2) { iv2 = true; } else { iv = true; } break; default: break; } }
        if (qm) { s.append("float4 crd_qmul(float4 a,float4 b){return float4(a.w*b.xyz+b.w*a.xyz+cross(a.xyz,b.xyz),a.w*b.w-dot(a.xyz,b.xyz));}\n"); }
        if (qc) { s.append("float4 crd_qconj(float4 q){return float4(-q.xyz,q.w);}\n"); }
        if (qr) { s.append("float3 crd_qrot(float4 q,float3 v){float3 t=2.0*cross(q.xyz,v);return v+q.w*t+cross(q.xyz,t);}\n"); }
        if (qa) { s.append("float4 crd_qaa(float3 ax,float an){float h=an*0.5;return float4(ax*sin(h),cos(h));}\n"); }
        if (sl) { s.append("float4 crd_slerp(float4 a,float4 b,float t){float d=dot(a,b);float sg=1.0;if(d<0.0){d=-d;sg=-1.0;}if(d>0.9995){return normalize(lerp(a,sg*b,t));}float th=acos(d);float sn=sin(th);return (sin((1.0-t)*th)*a+sin(t*th)*sg*b)/sn;}\n"); }
        if (qt) { s.append("float3x3 crd_qmat(float4 q){float x=q.x,y=q.y,z=q.z,w=q.w;return float3x3(1.0-2.0*(y*y+z*z),2.0*(x*y-w*z),2.0*(x*z+w*y),2.0*(x*y+w*z),1.0-2.0*(x*x+z*z),2.0*(y*z-w*x),2.0*(x*z-w*y),2.0*(y*z+w*x),1.0-2.0*(x*x+y*y));}\n"); } // row-major R[row][col]
        if (iv2) { s.append("float2x2 crd_inv2(float2x2 m){float a=m._m00,b=m._m01,c=m._m10,d=m._m11;float iv=1.0/(a*d-b*c);return float2x2(d*iv,-b*iv,-c*iv,a*iv);}\n"); }
        if (iv) { s.append("float3x3 crd_inv3(float3x3 m){float a=m._m00,b=m._m01,c=m._m02,d=m._m10,e=m._m11,f=m._m12,g=m._m20,h=m._m21,i=m._m22;float A=e*i-f*h,B=d*i-f*g,C=d*h-e*g;float iv=1.0/(a*A-b*B+c*C);return float3x3(A*iv,(c*h-b*i)*iv,(b*f-c*e)*iv,(f*g-d*i)*iv,(a*i-c*g)*iv,(c*d-a*f)*iv,(d*h-e*g)*iv,(b*g-a*h)*iv,(a*e-b*d)*iv);}\n"); }
    }
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 dtid : SV_DispatchThreadID) {\n  uint gid = dtid.x;\n  if (gid >= n) return;\n");
    // A4 tier-2: body-scoping — mark loop-varying nodes (LoopIndex/LoopAcc + consumers; For = barrier) + their owning For.
    crd::containers::Array<crd::u8> varying(scratch);
    varying.resize(static_cast<crd::usize>(n), 0);
    for (int i = 0; i < n; ++i) { const KNode& v = g.node(i); if (v.op == KOp::For) { continue; } const bool loop_leaf = v.op == KOp::LoopIndex || v.op == KOp::LoopAcc; const bool from_operand = (v.a >= 0 && varying[static_cast<crd::usize>(v.a)]) || (v.b >= 0 && varying[static_cast<crd::usize>(v.b)]) || (v.c >= 0 && varying[static_cast<crd::usize>(v.c)]) || (v.d >= 0 && varying[static_cast<crd::usize>(v.d)]); if (loop_leaf || from_operand) { varying[static_cast<crd::usize>(i)] = 1; } }
    crd::containers::Array<int> body_of(scratch);
    body_of.resize(static_cast<crd::usize>(n), -1);
    crd::containers::Array<int> rstk(scratch);
    for (int fi = 0; fi < n; ++fi) { if (g.node(fi).op != KOp::For) { continue; } rstk.push_back(g.node(fi).c); while (rstk.size() > 0) { const int bid = rstk[rstk.size() - 1]; rstk.resize(rstk.size() - 1); if (bid < 0 || !varying[static_cast<crd::usize>(bid)] || body_of[static_cast<crd::usize>(bid)] != -1) { continue; } body_of[static_cast<crd::usize>(bid)] = fi; const KNode& bn = g.node(bid); rstk.push_back(bn.a); rstk.push_back(bn.b); rstk.push_back(bn.c); rstk.push_back(bn.d); } }

    const auto compute_leaf = [&](const KGraph& gg, int li, crd::containers::String& ss) -> bool
    {
        const KNode& lnd = gg.node(li);
        if (lnd.op != KOp::Input) { return false; } // compute leaf: only `Input` reads a storage buffer
        const int  lc     = lnd.comps();
        const int  bd     = binding_of[static_cast<crd::usize>(li)];
        const bool is_mat = lnd.type.kind == TKind::Mat;
        emit_stmt_prefix_hlsl(gg, li, ss);
        if (lc == 1) { ss.append("in"); app_uint(ss, static_cast<crd::u32>(bd)); ss.append("[gid]"); }
        else { if (is_mat) { ss.append("transpose("); ss.append(hmat(lnd.type.cols, lnd.type.rows)); } else { ss.append(htype(lnd.type)); } ss.append("("); for (int k = 0; k < lc; ++k) { if (k) { ss.append(", "); } ss.append("in"); app_uint(ss, static_cast<crd::u32>(bd)); ss.append("[gid*"); app_uint(ss, static_cast<crd::u32>(lc)); ss.append("+"); app_uint(ss, static_cast<crd::u32>(k)); ss.append("]"); } ss.append(")"); if (is_mat) { ss.append(")"); } }
        ss.append(";\n");
        return true;
    };
    const auto emit_expr = [&](int i) -> bool { return emit_value_stmt_hlsl(g, i, s, compute_leaf); }; // B3-d: shared emitter

    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)] || varying[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        if (nd.op == KOp::For) // A4 tier-2: native per-thread `for`; body-scoped nodes (loop-varying) emit INSIDE the loop
        {
            s.append("  precise "); s.append(htype(nd.type)); s.append(" t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nd.b)); s.append(";\n");
            s.append("  for (int li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = 0; li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(" < int(t"); app_uint(s, static_cast<crd::u32>(nd.a)); s.append("); li_"); app_uint(s, static_cast<crd::u32>(i)); s.append("++) {\n");
            for (int bid = 0; bid < i; ++bid)
            {
                if (body_of[static_cast<crd::usize>(bid)] != i) { continue; }
                const KNode& bn = g.node(bid);
                if (bn.op == KOp::LoopIndex) { s.append("  precise float t"); app_uint(s, static_cast<crd::u32>(bid)); s.append(" = float(li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(");\n"); }
                else if (bn.op == KOp::LoopAcc) { s.append("  precise "); s.append(htype(bn.type)); s.append(" t"); app_uint(s, static_cast<crd::u32>(bid)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(i)); s.append(";\n"); }
                else if (!emit_expr(bid)) { return false; }
            }
            s.append("  t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nd.c)); s.append(";\n  }\n");
        }
        else if (!emit_expr(i)) { return false; }
    }
    // Write back column-major (flat[col*R + row]). HLSL indexes `t[row][col]`, the transpose of GLSL's `t[col][row]`.
    // Matrix-ness is the TYPE's: a comps==4 output is a float4 or a float2x2, and `oc` alone cannot tell them apart.
    const int    oc  = out.out_comps;
    const KType& oty = g.node(output).type;
    if (oty.kind == TKind::Mat)
    {
        for (int col = 0; col < oty.cols; ++col) { for (int row = 0; row < oty.rows; ++row) { s.append("  outb[gid*"); app_uint(s, oc); s.append("+"); app_uint(s, col * oty.rows + row); s.append("] = t"); app_uint(s, output); s.append("["); app_uint(s, row); s.append("]["); app_uint(s, col); s.append("];\n"); } }
    }
    // bool / bool-vector results convert to float 0.0/1.0 on write (the buffer is RWStructuredBuffer<float>).
    else if (oc == 1)
    {
        s.append("  outb[gid] = ");
        if (oty.scalar == DType::Bool) { s.append("float(t"); app_uint(s, output); s.append(")"); } else { s.append("t"); app_uint(s, output); }
        s.append(";\n");
    }
    else
    {
        for (int k = 0; k < oc; ++k)
        {
            s.append("  outb[gid*"); app_uint(s, oc); s.append("+"); app_uint(s, k); s.append("] = ");
            if (oty.scalar == DType::Bool) { s.append("float(t"); app_uint(s, output); s.append("["); app_uint(s, k); s.append("])"); }
            else { s.append("t"); app_uint(s, output); s.append("["); app_uint(s, k); s.append("]"); }
            s.append(";\n");
        }
    }
    s.append("}\n");
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

// T2 FAST GEMM (DetTier::Fast) HLSL/DX12 — the ported crush schedule (64×64 block, 4×4 microtile, TRANSPOSED-A
// groupshared, `mad()` FMA). Group-per-tile ((M/64)*(N/64) groups). Mirrors emit_contract_fast_glsl; DX12 inherits the
// tensor/FP32 crush schedule. Fast tier ⇒ matches the FMA oracle within tolerance, run-to-run deterministic.
inline bool emit_contract_fast_hlsl(const KGraph& g, int output, GlslKernel& out)
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
    s.append("groupshared float As[512];\n"); // TRANSPOSED [k][m]
    s.append("groupshared float Bs[512];\n");
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 gid3 : SV_GroupID, uint3 tid3 : SV_GroupThreadID) {\n");
    s.append("  uint nbc = N / 64u; uint bid = gid3.x;\n");
    s.append("  uint blockRow = (bid / nbc) * 64u; uint blockCol = (bid % nbc) * 64u;\n");
    s.append("  uint tid = tid3.x; uint tr = tid / 16u; uint tc = tid % 16u;\n");
    s.append("  uint arow = blockRow + tr * 4u; uint acol = blockCol + tc * 4u;\n");
    const char* d[4] = {"0", "1", "2", "3"};
    for (int i = 0; i < 4; ++i)
    {
        s.append("  float ");
        for (int j = 0; j < 4; ++j) { s.append("a"); s.append(d[i]); s.append(d[j]); s.append(" = 0.0"); if (j < 3) { s.append(", "); } }
        s.append(";\n");
    }
    s.append("  for (uint k0 = 0u; k0 < K; k0 += 8u) {\n");
    s.append("    for (uint t = tid; t < 512u; t += 256u) { uint r = t / 8u; uint cc = t % 8u; As[cc * 64u + r] = A[(blockRow + r) * K + (k0 + cc)]; }\n");
    s.append("    for (uint t = tid; t < 512u; t += 256u) { uint r = t / 64u; uint cc = t % 64u; Bs[r * 64u + cc] = Bm[(k0 + r) * N + (blockCol + cc)]; }\n");
    s.append("    GroupMemoryBarrierWithGroupSync();\n");
    s.append("    for (uint kk = 0u; kk < 8u; ++kk) {\n");
    s.append("      float ar0 = As[kk*64u+tr*4u+0u], ar1 = As[kk*64u+tr*4u+1u], ar2 = As[kk*64u+tr*4u+2u], ar3 = As[kk*64u+tr*4u+3u];\n");
    s.append("      float br0 = Bs[kk*64u+tc*4u+0u], br1 = Bs[kk*64u+tc*4u+1u], br2 = Bs[kk*64u+tc*4u+2u], br3 = Bs[kk*64u+tc*4u+3u];\n");
    const char* ar[4] = {"ar0", "ar1", "ar2", "ar3"};
    const char* br[4] = {"br0", "br1", "br2", "br3"};
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            s.append("      a"); s.append(d[i]); s.append(d[j]); s.append(" = mad("); s.append(ar[i]); s.append(", "); s.append(br[j]); s.append(", a"); s.append(d[i]); s.append(d[j]); s.append(");\n");
        }
    }
    s.append("    }\n    GroupMemoryBarrierWithGroupSync();\n  }\n");
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
// Atomic scatter-ADD (histogram) HLSL kernel — out[M] (pre-zeroed / D3D12 zero-inits committed resources), then
// InterlockedAdd(out[idx[i]], upd[i]) over N inputs. Integer atomics ⇒ order-independent ⇒ deterministic. Push: n = N.
inline bool emit_scatteradd_hlsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::ScatterAdd || g.node(sn.a).op != KOp::Input || g.node(sn.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(sn.a).iidx; // idx
    out.input_iidx[1] = g.node(sn.b).iidx; // updates
    crd::containers::String& s = out.source;
    s.clear();
    s.append("RWStructuredBuffer<int> idxb : register(u0);\n");
    s.append("RWStructuredBuffer<int> updb : register(u1);\n");
    s.append("RWStructuredBuffer<int> outb : register(u2);\n");
    s.append("cbuffer PC : register(b0) { uint n; };\n");
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 dtid : SV_DispatchThreadID) {\n  uint i = dtid.x;\n  if (i >= n) return;\n");
    s.append("  InterlockedAdd(outb[idxb[i]], updb[i]);\n}\n");
    return true;
}

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
