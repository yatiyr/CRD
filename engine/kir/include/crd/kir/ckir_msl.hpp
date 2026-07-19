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

// MSL spells a matrix `floatCxR` -- COLUMNS first, like GLSL's `matCxR` and WGSL's `matCxR<f32>`, and unlike HLSL's
// `floatRxC`. `m[col]` yields a column vector, so the flat column-major storage maps straight across.
inline const char* mmat(int rows, int cols) noexcept
{
    switch (cols * 10 + rows)
    {
    case 22: return "float2x2"; case 23: return "float2x3"; case 24: return "float2x4";
    case 32: return "float3x2"; case 33: return "float3x3"; case 34: return "float3x4";
    case 42: return "float4x2"; case 43: return "float4x3"; case 44: return "float4x4";
    default: return "float";
    }
}
inline const char* mscalar(DType d) noexcept
{
    if (d == DType::Bool) { return "bool"; }
    if (glsl_detail::dt_is_uint(d)) { return "uint"; }
    return glsl_detail::dt_is_int(d) ? "int" : "float";
}
// MSL type name for a CKIR value type: float/int/uint/bool, floatN/intN/uintN/boolN, floatCxR.
inline const char* mtype(KType t) noexcept
{
    if (t.kind == TKind::Mat) { return mmat(t.rows, t.cols); }
    if (t.kind == TKind::Vec)
    {
        if (t.scalar == DType::Bool) { switch (t.rows) { case 2: return "bool2"; case 3: return "bool3"; case 4: return "bool4"; default: break; } }
        else if (glsl_detail::dt_is_uint(t.scalar)) { switch (t.rows) { case 2: return "uint2"; case 3: return "uint3"; case 4: return "uint4"; default: break; } }
        else if (glsl_detail::dt_is_int(t.scalar)) { switch (t.rows) { case 2: return "int2"; case 3: return "int3"; case 4: return "int4"; default: break; } }
        else { switch (t.rows) { case 2: return "float2"; case 3: return "float3"; case 4: return "float4"; default: break; } }
    }
    return mscalar(t.scalar);
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
        // every temp here is float (bool lowers to 0.0/1.0), so a Cast is an explicit float conversion.
        case KOp::Cast: s.append("float("); ta(nd.a); s.append(")"); break;
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

// B-cmp: emit an IMPERATIVE compute KERNEL (shared memory + barriers — `KEntry.is_kernel()`) as Metal Shading Language.
// Mirror of emit_compute_kernel_glsl. Storage buffers are `device [const] T* [[buffer(N)]]` params (N = binding);
// `threadgroup` shared arrays are declared INSIDE the body (Metal's threadgroup address space is function-local, unlike
// GLSL's file-scope `shared`); the barrier is `threadgroup_barrier(mem_flags::mem_threadgroup)`; LocalInvocationIndex is
// `thread_position_in_threadgroup` (1-D workgroup). Structural-gated (no Metal off macOS); compile+run at ADR-0098 Part C.
inline bool emit_compute_kernel_msl(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    if (!entry.is_kernel()) { return false; }
    const int                n = g.size();
    crd::containers::String& s = out.source;
    s.clear();
    out.n_inputs = 0;

    const auto cty = [](DType d) -> const char* { if (dt_is_uint(d)) { return "uint"; } return dt_is_int(d) ? "int" : "float"; };

    msl_detail::header(s); // #include <metal_stdlib>  using namespace metal;
    s.append("kernel void ckir(");
    bool first = true;
    for (int i = 0; i < n; ++i)
    {
        const KNode& nd = g.node(i);
        if (nd.op == KOp::BufferDecl)
        {
            if (!first) { s.append(", "); }
            first = false;
            s.append(nd.axes != 0U ? "device " : "device const "); s.append(cty(nd.dtype())); s.append("* buf"); app_uint(s, nd.iidx);
            s.append(" [[buffer("); app_uint(s, nd.iidx); s.append(")]]");
        }
    }
    s.append(first ? "" : ", "); s.append("uint lidx [[thread_position_in_threadgroup]], uint wgid [[threadgroup_position_in_grid]]) {\n");
    for (int i = 0; i < n; ++i) // threadgroup arrays — function-local in Metal
    {
        const KNode& nd = g.node(i);
        if (nd.op == KOp::SharedDecl)
        {
            s.append("  threadgroup "); s.append(cty(nd.dtype())); s.append(" sh"); app_uint(s, i);
            s.append("["); app_uint(s, nd.iidx + static_cast<int>(nd.axes)); s.append("];\n");
        }
    }

    bool                            ok = true;
    crd::containers::Array<crd::u8> matd(scratch); // Materialized (frozen) nodes emit `t<node>`, not their inline expr
    matd.resize(static_cast<crd::usize>(n), 0);
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
            else { app_flit(s, nd.cval); }
            break;
        case KOp::Builtin:
            if (static_cast<KBuiltin>(nd.iidx) == KBuiltin::LocalInvocationIndex) { s.append("lidx"); }
            else if (static_cast<KBuiltin>(nd.iidx) == KBuiltin::WorkgroupIndex) { s.append("wgid"); }
            else { ok = false; s.append("0u"); }
            break;
        case KOp::KernelLoopVar: s.append("lv"); app_uint(s, nd.a); break;
        case KOp::BufferLoad: s.append("buf"); app_uint(s, g.node(nd.a).iidx); s.append("["); self(self, nd.b); s.append("]"); break;
        case KOp::SharedLoad: s.append("sh"); app_uint(s, nd.a); s.append("["); self(self, nd.b); s.append("]"); break;
        case KOp::Cast: s.append(cty(nd.dtype())); s.append("("); self(self, nd.a); s.append(")"); break;
        case KOp::Neg: s.append("(-"); self(self, nd.a); s.append(")"); break;
        case KOp::Abs: fn1("abs"); break;
        case KOp::Sqrt: fn1("sqrt"); break;
        case KOp::Sin: fn1("sin"); break;
        case KOp::Cos: fn1("cos"); break;
        case KOp::Exp: fn1("exp"); break;
        case KOp::Pow: fn2("pow"); break;
        case KOp::Log: fn1("log"); break;
        case KOp::Log2: fn1("log2"); break;
        case KOp::Tanh: fn1("tanh"); break;
        case KOp::Atan2: fn2("atan2"); break;
        case KOp::Atan: fn1("atan"); break;
        case KOp::Asin: fn1("asin"); break;
        case KOp::Acos: fn1("acos"); break;
        case KOp::Sinh: fn1("sinh"); break;
        case KOp::Cosh: fn1("cosh"); break;
        case KOp::Floor: fn1("floor"); break;
        case KOp::Ceil: fn1("ceil"); break; // B4-vis: bbox ceil
        case KOp::Add: bin(" + "); break;
        case KOp::Sub: bin(" - "); break;
        case KOp::Mul: bin(" * "); break;
        case KOp::Div: bin(" / "); break;
        case KOp::Min: fn2("min"); break;
        case KOp::Max: fn2("max"); break;
        case KOp::Clamp: s.append("min(max("); self(self, nd.a); s.append(", "); self(self, nd.b); s.append("), "); self(self, nd.c); s.append(")"); break; // B4-vis: bbox clamp = min(max()) (matches oracle)
        case KOp::Mod: if (dt_is_int(nd.dtype()) || dt_is_uint(nd.dtype())) { bin(" % "); } else { fn2("fmod"); } break;
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
        case KOp::Fma: s.append("fma("); self(self, nd.a); s.append(", "); self(self, nd.b); s.append(", "); self(self, nd.c); s.append(")"); break;
        case KOp::Select: s.append("(("); self(self, nd.c); s.append(") ? "); self(self, nd.a); s.append(" : "); self(self, nd.b); s.append(")"); break; // a=true b=false c=cond
        default: ok = false; s.append("0"); break;
        }
    };
    // CSE (matches GLSL/HLSL/CUDA): materialize EVERY non-inline arithmetic node as a `t<node>` temp keyed by node id ⇒ a shared
    // subtree emits ONCE. Without it the recursive `ev` inline-expands a value referenced M times M-fold, so a DEEP shared value
    // DAG (the B15-b Perlin-Worley cloud density) explodes EXPONENTIALLY (OOM). LEAVES + cast/select/compare/bitops stay INLINE.
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
            ev(ev, node); // matd[node] still 0 ⇒ one-level expr (children already materialized ⇒ temp refs)
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
            case KStmtKind::Barrier: s.append(st.scope == BarrierScope::Buffer ? "  threadgroup_barrier(mem_flags::mem_device);\n" : "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"); ++i; break;
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
            case KStmtKind::For: decl(decl, st.value); s.append("  for (uint lv"); app_uint(s, i); s.append(" = 0u; lv"); app_uint(s, i); s.append(" < uint("); ev(ev, st.value); s.append("); ++lv"); app_uint(s, i); s.append(") {\n"); self_b(self_b, st.body_begin, st.body_count); s.append("  }\n"); i = st.body_begin + st.body_count; break;
            case KStmtKind::If: decl(decl, st.value); s.append("  if ("); ev(ev, st.value); s.append(") {\n"); self_b(self_b, st.body_begin, st.body_count); s.append("  }\n"); i = st.body_begin + st.body_count; break;
            case KStmtKind::SpinUntilNonzero: decl(decl, st.index); s.append("  while (buf"); app_uint(s, g.node(st.target).iidx); s.append("["); ev(ev, st.index); s.append("] == 0u) { threadgroup_barrier(mem_flags::mem_device); }\n"); ++i; break;
            case KStmtKind::SharedAtomicAdd: decl(decl, st.index); decl(decl, st.value); s.append("  atomic_fetch_add_explicit(&sh"); app_uint(s, st.target); s.append("["); ev(ev, st.index); s.append("], "); ev(ev, st.value); s.append(", memory_order_relaxed);\n"); ++i; break;
            case KStmtKind::BufferAtomicAdd: decl(decl, st.index); decl(decl, st.value); s.append("  atomic_fetch_add_explicit((device atomic_uint*)&buf"); app_uint(s, g.node(st.target).iidx); s.append("["); ev(ev, st.index); s.append("], "); ev(ev, st.value); s.append(", memory_order_relaxed);\n"); ++i; break;
            case KStmtKind::BufferAtomicMin: decl(decl, st.index); decl(decl, st.value); s.append("  atomic_fetch_min_explicit((device atomic_uint*)&buf"); app_uint(s, g.node(st.target).iidx); s.append("["); ev(ev, st.index); s.append("], "); ev(ev, st.value); s.append(", memory_order_relaxed);\n"); ++i; break; // B4-vis: visibility key (nearest wins)
            case KStmtKind::BufferAtomicAddFetch: // value-returning: atomic_fetch_add_explicit RETURNS the old value
                decl(decl, st.index); decl(decl, st.value);
                s.append("  "); s.append(cty(g.node(st.result).dtype())); s.append(" t"); app_uint(s, static_cast<crd::u32>(st.result));
                s.append(" = atomic_fetch_add_explicit((device atomic_uint*)&buf"); app_uint(s, g.node(st.target).iidx); s.append("["); ev(ev, st.index); s.append("], "); ev(ev, st.value); s.append(", memory_order_relaxed);\n");
                matd[static_cast<crd::usize>(st.result)] = 1U; ++i; break;
            case KStmtKind::BufferAtomicExchange: // value-returning: atomic_exchange_explicit RETURNS the old value
                decl(decl, st.index); decl(decl, st.value);
                s.append("  "); s.append(cty(g.node(st.result).dtype())); s.append(" t"); app_uint(s, static_cast<crd::u32>(st.result));
                s.append(" = atomic_exchange_explicit((device atomic_uint*)&buf"); app_uint(s, g.node(st.target).iidx); s.append("["); ev(ev, st.index); s.append("], "); ev(ev, st.value); s.append(", memory_order_relaxed);\n");
                matd[static_cast<crd::usize>(st.result)] = 1U; ++i; break;
            case KStmtKind::ForBreakIf: decl(decl, st.value); s.append("  if (("); ev(ev, st.value); s.append(") != 0u) break;\n"); ++i; break;
            case KStmtKind::BufferTicket: decl(decl, st.index); s.append("  if (lidx == 0u) { sh"); app_uint(s, st.value); s.append("[0] = atomic_fetch_add_explicit((device atomic_uint*)&buf"); app_uint(s, g.node(st.target).iidx); s.append("["); ev(ev, st.index); s.append("], 1u, memory_order_relaxed); }\n"); ++i; break;
            case KStmtKind::SyncWarp: s.append("  simdgroup_barrier(mem_flags::mem_threadgroup);\n"); ++i; break;
            }
        }
    };
    emit_body(emit_body, entry.kernel_body_begin, entry.kernel_body_count);
    s.append("}\n");
    return ok;
}

// ── B0 fan-out: the TYPE-AWARE vec/mat/bool/struct emitter, mirroring `emit_vec_glsl` ────────────────────────────────
// MSL is a C++14 dialect, so it is the closest of the four to GLSL: native `float3`/`float3x3`/`bool3`, componentwise
// relational operators yielding `boolN` (like HLSL, unlike GLSL), a real `?:`, and the same COLUMN-first `floatCxR`
// matrix spelling as GLSL/WGSL. Its gaps: **no `inverse()`** and **no `outerProduct()`** (emitted helpers / column
// construction), and no per-op `precise` — the Metal backend gets determinism from a compile flag (math-mode SAFE,
// fast-math OFF), so there is no FMA fusion.
// NOTE Metal cannot be compiled off macOS: this emitter is gated STRUCTURALLY in `tests/kir/test_ckir_msl.cpp`; the
// compile + bit-exact run is ADR-0098 §3 v17-n on real Apple silicon (GitHub Actions).
// NOLINTBEGIN(readability-function-size) -- one branch per KOp; see the same note on `eval_cpu` / `emit_vec_cuda`.
inline bool emit_vec_msl(const KGraph& g, int output, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    using msl_detail::mmat;
    using msl_detail::mtype;
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
        if (nd.op == KOp::MatInverse && nd.type.rows == 4) { return false; }                          // mat4 inverse deferred, as on HLSL/WGSL/CUDA
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
    msl_detail::header(s);
    s.append("struct PC { uint d0; uint d1; uint d2; uint d3; };\n");
    { // helpers, emitted once, only when the graph uses them
        bool qm = false; bool qc = false; bool qr = false; bool qa = false; bool qt = false; bool sl = false;
        bool iv2 = false; bool iv3 = false;
        for (int i = 0; i < n; ++i)
        {
            if (!reach[static_cast<crd::usize>(i)]) { continue; }
            switch (g.node(i).op)
            {
            case KOp::QuatMul: qm = true; break;
            case KOp::QuatConj: qc = true; break;
            case KOp::QuatRotate: qr = true; break;
            case KOp::QuatAxisAngle: qa = true; break;
            case KOp::QuatToMat3: qt = true; break;
            case KOp::Slerp: sl = true; break;
            case KOp::MatInverse: if (g.node(i).type.rows == 2) { iv2 = true; } else { iv3 = true; } break;
            default: break;
            }
        }
        if (qm) { s.append("static float4 crd_qmul(float4 a, float4 b) { return float4(a.w*b.xyz + b.w*a.xyz + cross(a.xyz, b.xyz), a.w*b.w - dot(a.xyz, b.xyz)); }\n"); }
        if (qc) { s.append("static float4 crd_qconj(float4 q) { return float4(-q.xyz, q.w); }\n"); }
        if (qr) { s.append("static float3 crd_qrot(float4 q, float3 v) { float3 t = 2.0 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }\n"); }
        if (qa) { s.append("static float4 crd_qaa(float3 ax, float an) { float h = an * 0.5; return float4(ax * sin(h), cos(h)); }\n"); }
        if (qt) { s.append("static float3x3 crd_qmat(float4 q) { float x=q.x, y=q.y, z=q.z, w=q.w; return float3x3(1.0-2.0*(y*y+z*z), 2.0*(x*y+w*z), 2.0*(x*z-w*y), 2.0*(x*y-w*z), 1.0-2.0*(x*x+z*z), 2.0*(y*z+w*x), 2.0*(x*z+w*y), 2.0*(y*z-w*x), 1.0-2.0*(x*x+y*y)); }\n"); }
        if (sl) { s.append("static float4 crd_slerp(float4 a, float4 b, float t) { float d = dot(a,b); float sg = 1.0; if (d < 0.0) { d = -d; sg = -1.0; } if (d > 0.9995) { return normalize(mix(a, sg*b, t)); } float th = acos(d); float sn = sin(th); return (sin((1.0-t)*th)*a + sin(t*th)*sg*b) / sn; }\n"); }
        // m[col][row]: MSL indexes a matrix by COLUMN first, exactly like GLSL.
        if (iv2) { s.append("static float2x2 crd_inv2(float2x2 m) { float iv = 1.0 / determinant(m); return float2x2(float2(m[1][1], -m[0][1]) * iv, float2(-m[1][0], m[0][0]) * iv); }\n"); }
        if (iv3) { s.append("static float3x3 crd_inv3(float3x3 m) { float a=m[0][0], b=m[1][0], c=m[2][0], d=m[0][1], e=m[1][1], f=m[2][1], g0=m[0][2], h=m[1][2], i0=m[2][2]; float A=e*i0-f*h, B=-(d*i0-f*g0), C=d*h-e*g0; float iv = 1.0/(a*A + b*B + c*C); float D=-(b*i0-c*h), E=a*i0-c*g0, F=-(a*h-b*g0); float G=b*f-c*e, H=-(a*f-c*d), I=a*e-b*d; return float3x3(float3(A,B,C)*iv, float3(D,E,F)*iv, float3(G,H,I)*iv); }\n"); }
    }
    s.append("kernel void ckir(\n");
    for (int b = 0; b < out.n_inputs; ++b) { s.append("  device const float* in"); app_uint(s, b); s.append(" [[buffer("); app_uint(s, b); s.append(")]],\n"); }
    s.append("  device float* outb [[buffer("); app_uint(s, out.n_inputs); s.append(")]],\n");
    s.append("  constant PC& pc [[buffer("); app_uint(s, out.n_inputs + 1); s.append(")]],\n");
    s.append("  uint gid [[thread_position_in_grid]]) {\n  if (gid >= pc.d0) return;\n");

    const char xyzw[4] = {'x', 'y', 'z', 'w'};
    const auto ta      = [&](int id) { s.append("t"); app_uint(s, id); };
    const auto sw1     = [&](int k) { const char b[2] = {xyzw[k], '\0'}; s.append(b); };

    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        const int    c  = nd.comps();
        // B0-4 SROA: the aggregate is never materialized; FieldGet/ArrayGet resolves to the field's temp.
        if (nd.op == KOp::StructMake || nd.op == KOp::ArrayMake) { continue; }
        if (nd.op == KOp::FieldGet || nd.op == KOp::ArrayGet)
        {
            const KNode& agg = g.node(nd.a);
            if (agg.op != KOp::StructMake && agg.op != KOp::ArrayMake) { return false; }
            s.append("  "); s.append(mtype(nd.type)); s.append(" t"); app_uint(s, i); s.append(" = "); ta(g.ext_operand(agg, nd.iidx)); s.append(";\n");
            continue;
        }
        s.append("  "); s.append(mtype(nd.type)); s.append(" t"); app_uint(s, i); s.append(" = ");
        switch (nd.op)
        {
        case KOp::Input: { const int bd = binding_of[static_cast<crd::usize>(i)]; if (c == 1) { s.append("in"); app_uint(s, bd); s.append("[gid]"); } else { s.append(mtype(nd.type)); s.append("("); for (int k = 0; k < c; ++k) { if (k) { s.append(", "); } s.append("in"); app_uint(s, bd); s.append("[gid*"); app_uint(s, c); s.append("+"); app_uint(s, k); s.append("]"); } s.append(")"); } break; } // MSL matrix ctors take column-major scalars — our flat layout
        case KOp::Const:
            if (nd.dtype() == DType::Bool) { s.append(nd.cval != 0.0 ? "true" : "false"); }
            else if (dt_is_int(nd.dtype()) || dt_is_uint(nd.dtype())) { app_ilit(s, nd.cval); }
            else { app_flit(s, nd.cval); }
            break;
        case KOp::Cast: s.append(mtype(nd.type)); s.append("("); ta(nd.a); s.append(")"); break;
        case KOp::Neg: s.append("-"); ta(nd.a); break;
        case KOp::Recip: s.append("(1.0 / "); ta(nd.a); s.append(")"); break;
        case KOp::Abs: s.append("abs("); ta(nd.a); s.append(")"); break;
        case KOp::Sqrt: s.append("sqrt("); ta(nd.a); s.append(")"); break;
        case KOp::Rsqrt: s.append("rsqrt("); ta(nd.a); s.append(")"); break;
        case KOp::Exp: s.append("exp("); ta(nd.a); s.append(")"); break;
        case KOp::Log: s.append("log("); ta(nd.a); s.append(")"); break;
        case KOp::Sin: s.append("sin("); ta(nd.a); s.append(")"); break;
        case KOp::Cos: s.append("cos("); ta(nd.a); s.append(")"); break;
        case KOp::Floor: s.append("floor("); ta(nd.a); s.append(")"); break;
        case KOp::Fract: s.append("fract("); ta(nd.a); s.append(")"); break;
        case KOp::Add: ta(nd.a); s.append(" + "); ta(nd.b); break;
        case KOp::Sub: ta(nd.a); s.append(" - "); ta(nd.b); break;
        case KOp::Mul: ta(nd.a); s.append(" * "); ta(nd.b); break;
        case KOp::Div: ta(nd.a); s.append(" / "); ta(nd.b); break;
        case KOp::Min: s.append("min("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Max: s.append("max("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Pow: s.append("pow("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Clamp: s.append("clamp("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::Mix: s.append("mix("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::Vec2: s.append("float2("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Vec3: s.append("float3("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::VecConcat: s.append(mtype(nd.type)); s.append("("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::VecComp: ta(nd.a); s.append("."); sw1(nd.iidx); break;
        case KOp::Swizzle: ta(nd.a); s.append("."); for (int k = 0; k < c; ++k) { sw1(nd.perm[k]); } break;
        case KOp::Splat: s.append(mtype(nd.type)); s.append("("); ta(nd.a); s.append(")"); break;
        case KOp::Dot: s.append("dot("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Cross: s.append("cross("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Normalize: s.append("normalize("); ta(nd.a); s.append(")"); break;
        case KOp::VecLen: s.append("length("); ta(nd.a); s.append(")"); break;
        case KOp::Reflect: s.append("reflect("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Refract: s.append("refract("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::Faceforward: s.append("faceforward("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::MatVecMul: // MSL spells both as `*` (column-major), like GLSL
        case KOp::MatMatMul: ta(nd.a); s.append(" * "); ta(nd.b); break;
        case KOp::MatTranspose: s.append("transpose("); ta(nd.a); s.append(")"); break;
        case KOp::Determinant: s.append("determinant("); ta(nd.a); s.append(")"); break;
        case KOp::MatInverse: s.append(nd.type.rows == 2 ? "crd_inv2(" : "crd_inv3("); ta(nd.a); s.append(")"); break;
        // MSL has no outerProduct(): column k of an RxC outer product is `a * b[k]`.
        case KOp::OuterProduct: { const int oc = nd.type.cols; s.append(mmat(nd.type.rows, oc)); s.append("("); for (int k = 0; k < oc; ++k) { if (k) { s.append(", "); } ta(nd.a); s.append(" * "); ta(nd.b); s.append("."); sw1(k); } s.append(")"); break; }
        case KOp::MatFromCols: { const int mcols = nd.type.cols; const int operand[4] = {nd.a, nd.b, nd.c, nd.d}; s.append(mtype(nd.type)); s.append("("); for (int k = 0; k < mcols; ++k) { if (k) { s.append(", "); } ta(operand[k]); } s.append(")"); break; }
        case KOp::VecAny: if (g.node(nd.a).dtype() == DType::Bool) { s.append("any("); ta(nd.a); s.append(")"); } else { s.append("any("); ta(nd.a); s.append(" != "); s.append(mtype(g.node(nd.a).type)); s.append("(0.0))"); } break;
        case KOp::VecAll: if (g.node(nd.a).dtype() == DType::Bool) { s.append("all("); ta(nd.a); s.append(")"); } else { s.append("all("); ta(nd.a); s.append(" != "); s.append(mtype(g.node(nd.a).type)); s.append("(0.0))"); } break;
        // MSL relational operators are componentwise on vectors and already yield boolN (like HLSL, unlike GLSL).
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
        case KOp::QuatToMat3: s.append("crd_qmat("); ta(nd.a); s.append(")"); break;
        default: return false;
        }
        s.append(";\n");
    }

    const KType& oty = g.node(output).type;
    const int    oc  = out.out_comps;
    if (oty.kind == TKind::Mat)
    {
        for (int col = 0; col < oty.cols; ++col)
        {
            for (int row = 0; row < oty.rows; ++row)
            {
                s.append("  outb[gid*"); app_uint(s, oc); s.append("+"); app_uint(s, col * oty.rows + row); s.append("] = t"); app_uint(s, output); s.append("["); app_uint(s, col); s.append("]["); app_uint(s, row); s.append("];\n");
            }
        }
    }
    else if (oc == 1)
    {
        s.append("  outb[gid] = ");
        if (oty.scalar == DType::Bool) { s.append("(t"); app_uint(s, output); s.append(" ? 1.0 : 0.0)"); }
        else if (oty.scalar != DType::F32 && oty.scalar != DType::F64) { s.append("float(t"); app_uint(s, output); s.append(")"); }
        else { s.append("t"); app_uint(s, output); }
        s.append(";\n");
    }
    else
    {
        for (int k = 0; k < oc; ++k)
        {
            s.append("  outb[gid*"); app_uint(s, oc); s.append("+"); app_uint(s, k); s.append("] = ");
            if (oty.scalar == DType::Bool) { s.append("(t"); app_uint(s, output); s.append("["); app_uint(s, k); s.append("] ? 1.0 : 0.0)"); }
            else if (oty.scalar != DType::F32 && oty.scalar != DType::F64) { s.append("float(t"); app_uint(s, output); s.append("["); app_uint(s, k); s.append("])"); }
            else { s.append("t"); app_uint(s, output); s.append("["); app_uint(s, k); s.append("]"); }
            s.append(";\n");
        }
    }
    s.append("}\n");
    return true;
}
// NOLINTEND(readability-function-size)

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
