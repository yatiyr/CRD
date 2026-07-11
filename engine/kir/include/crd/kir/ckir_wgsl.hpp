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

// WGSL spells a matrix `matCxR<f32>` -- COLUMNS first, like GLSL's `matCxR` and unlike HLSL's `floatRxC`.
inline const char* wmat(int rows, int cols) noexcept
{
    switch (cols * 10 + rows)
    {
    case 22: return "mat2x2<f32>"; case 23: return "mat2x3<f32>"; case 24: return "mat2x4<f32>";
    case 32: return "mat3x2<f32>"; case 33: return "mat3x3<f32>"; case 34: return "mat3x4<f32>";
    case 42: return "mat4x2<f32>"; case 43: return "mat4x3<f32>"; case 44: return "mat4x4<f32>";
    default: return "f32";
    }
}
inline const char* wscalar(DType d) noexcept
{
    if (d == DType::Bool) { return "bool"; }
    if (glsl_detail::dt_is_uint(d)) { return "u32"; }
    return glsl_detail::dt_is_int(d) ? "i32" : "f32";
}
// WGSL type name for a CKIR value type: f32 / i32 / u32 / bool, vecN<T>, matCxR<f32>.
inline const char* wtype(KType t) noexcept
{
    if (t.kind == TKind::Mat) { return wmat(t.rows, t.cols); }
    if (t.kind == TKind::Vec)
    {
        if (t.scalar == DType::Bool) { switch (t.rows) { case 2: return "vec2<bool>"; case 3: return "vec3<bool>"; case 4: return "vec4<bool>"; default: break; } }
        else if (glsl_detail::dt_is_uint(t.scalar)) { switch (t.rows) { case 2: return "vec2<u32>"; case 3: return "vec3<u32>"; case 4: return "vec4<u32>"; default: break; } }
        else if (glsl_detail::dt_is_int(t.scalar)) { switch (t.rows) { case 2: return "vec2<i32>"; case 3: return "vec3<i32>"; case 4: return "vec4<i32>"; default: break; } }
        else { switch (t.rows) { case 2: return "vec2<f32>"; case 3: return "vec3<f32>"; case 4: return "vec4<f32>"; default: break; } }
    }
    return wscalar(t.scalar);
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
        // every temp in this emitter is f32 (bool lowers to 0.0/1.0), so a Cast is an explicit f32 conversion.
        case KOp::Cast: s.append("f32("); ta(nd.a); s.append(")"); break;
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

// ── B0 fan-out: the TYPE-AWARE vec/mat/bool/struct emitter, mirroring `emit_vec_glsl` ────────────────────────────────
// WGSL's real divergences from GLSL, each of which a naive mirror gets wrong:
//   · no `?:` ternary            -> `select(false_val, true_val, cond)`  (note the operand order)
//   · no `inverse()`             -> emitted `crd_inv2` / `crd_inv3` helpers (4x4 refused, as on HLSL)
//   · no `outerProduct()`        -> built column-by-column: column k of `outer(a,b)` is `a * b[k]`
//   · `faceForward` is capitalised, and `inverseSqrt` replaces `inversesqrt`
//   · matrices are `matCxR<f32>` (columns first, like GLSL; the transpose of HLSL's `floatRxC`)
//   · NO implicit int->float on assignment, so a non-f32 result converts explicitly on buffer write
//   · no `precise` qualifier => WebGPU is ULP-tolerant vs the CPU oracle, never bit-exact (see this file's header)
inline bool emit_vec_wgsl(const KGraph& g, int output, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    using wgsl_detail::wmat;
    using wgsl_detail::wtype;
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
        // dynamic control flow needs the body-scoping pass the GLSL/HLSL emitters carry; not mirrored yet, so refuse
        // loudly (a wrong lowering here would be a silent wrong answer).
        if (nd.op == KOp::For || nd.op == KOp::LoopIndex || nd.op == KOp::LoopAcc) { return false; }
        if (nd.op == KOp::MatInverse && nd.type.rows == 4) { return false; } // mat4 inverse deferred, as on HLSL
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
    for (int b = 0; b < out.n_inputs; ++b) { s.append("@group(0) @binding("); app_uint(s, b); s.append(") var<storage, read> in"); app_uint(s, b); s.append(" : array<f32>;\n"); }
    s.append("@group(0) @binding("); app_uint(s, out.n_inputs); s.append(") var<storage, read_write> outb : array<f32>;\n");
    wgsl_detail::emit_uniform(s, out.n_inputs + 1);
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
        if (qm) { s.append("fn crd_qmul(a : vec4<f32>, b : vec4<f32>) -> vec4<f32> { return vec4<f32>(a.w*b.xyz + b.w*a.xyz + cross(a.xyz, b.xyz), a.w*b.w - dot(a.xyz, b.xyz)); }\n"); }
        if (qc) { s.append("fn crd_qconj(q : vec4<f32>) -> vec4<f32> { return vec4<f32>(-q.xyz, q.w); }\n"); }
        if (qr) { s.append("fn crd_qrot(q : vec4<f32>, v : vec3<f32>) -> vec3<f32> { let t = 2.0 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }\n"); }
        if (qa) { s.append("fn crd_qaa(ax : vec3<f32>, an : f32) -> vec4<f32> { let h = an * 0.5; return vec4<f32>(ax * sin(h), cos(h)); }\n"); }
        if (qt) { s.append("fn crd_qmat(q : vec4<f32>) -> mat3x3<f32> { let x = q.x; let y = q.y; let z = q.z; let w = q.w; return mat3x3<f32>(1.0-2.0*(y*y+z*z), 2.0*(x*y+w*z), 2.0*(x*z-w*y), 2.0*(x*y-w*z), 1.0-2.0*(x*x+z*z), 2.0*(y*z+w*x), 2.0*(x*z+w*y), 2.0*(y*z-w*x), 1.0-2.0*(x*x+y*y)); }\n"); }
        if (sl) { s.append("fn crd_slerp(a : vec4<f32>, b : vec4<f32>, t : f32) -> vec4<f32> { var d = dot(a, b); var sg = 1.0; if (d < 0.0) { d = -d; sg = -1.0; } if (d > 0.9995) { return normalize(mix(a, sg*b, vec4<f32>(t))); } let th = acos(d); let sn = sin(th); return (sin((1.0-t)*th)*a + sin(t*th)*sg*b) / sn; }\n"); }
        // m[col][row]: WGSL indexes a matrix by COLUMN first. col0 of inv2 is (m11, -m01)/det.
        if (iv2) { s.append("fn crd_inv2(m : mat2x2<f32>) -> mat2x2<f32> { let iv = 1.0 / determinant(m); return mat2x2<f32>(vec2<f32>(m[1][1], -m[0][1]) * iv, vec2<f32>(-m[1][0], m[0][0]) * iv); }\n"); }
        if (iv3) { s.append("fn crd_inv3(m : mat3x3<f32>) -> mat3x3<f32> { let a = m[0][0]; let b = m[1][0]; let c = m[2][0]; let d = m[0][1]; let e = m[1][1]; let f = m[2][1]; let g0 = m[0][2]; let h = m[1][2]; let i0 = m[2][2]; let A = e*i0 - f*h; let B = -(d*i0 - f*g0); let C = d*h - e*g0; let iv = 1.0 / (a*A + b*B + c*C); let D = -(b*i0 - c*h); let E = a*i0 - c*g0; let F = -(a*h - b*g0); let G = b*f - c*e; let H = -(a*f - c*d); let I = a*e - b*d; return mat3x3<f32>(vec3<f32>(A,B,C) * iv, vec3<f32>(D,E,F) * iv, vec3<f32>(G,H,I) * iv); }\n"); }
    }
    s.append("@compute @workgroup_size(256)\nfn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {\n  let i = gid.x;\n  if (i >= pc.d0) { return; }\n");

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
            s.append("  let t"); app_uint(s, i); s.append(" : "); s.append(wtype(nd.type)); s.append(" = "); ta(g.ext_operand(agg, nd.iidx)); s.append(";\n");
            continue;
        }
        s.append("  let t"); app_uint(s, i); s.append(" : "); s.append(wtype(nd.type)); s.append(" = ");
        switch (nd.op)
        {
        case KOp::Input: { const int bd = binding_of[static_cast<crd::usize>(i)]; if (c == 1) { s.append("in"); app_uint(s, bd); s.append("[i]"); } else { s.append(wtype(nd.type)); s.append("("); for (int k = 0; k < c; ++k) { if (k) { s.append(", "); } s.append("in"); app_uint(s, bd); s.append("[i*"); app_uint(s, c); s.append("+"); app_uint(s, k); s.append("]"); } s.append(")"); } break; } // WGSL matrix ctors take column-major scalars — our flat layout
        case KOp::Const:
            if (nd.dtype() == DType::Bool) { s.append(nd.cval != 0.0 ? "true" : "false"); }
            else if (dt_is_int(nd.dtype()) || dt_is_uint(nd.dtype())) { s.append(dt_is_uint(nd.dtype()) ? "u32(" : "i32("); app_ilit(s, nd.cval); s.append(")"); }
            else { app_flit(s, nd.cval); }
            break;
        case KOp::Cast: s.append(wtype(nd.type)); s.append("("); ta(nd.a); s.append(")"); break;
        case KOp::Neg: s.append("-"); ta(nd.a); break;
        case KOp::Recip: s.append("(1.0 / "); ta(nd.a); s.append(")"); break;
        case KOp::Abs: s.append("abs("); ta(nd.a); s.append(")"); break;
        case KOp::Sqrt: s.append("sqrt("); ta(nd.a); s.append(")"); break;
        case KOp::Rsqrt: s.append("inverseSqrt("); ta(nd.a); s.append(")"); break;
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
        case KOp::Vec2: s.append("vec2<f32>("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Vec3: s.append("vec3<f32>("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::VecConcat: s.append(wtype(nd.type)); s.append("("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::VecComp: ta(nd.a); s.append("."); sw1(nd.iidx); break;
        case KOp::Swizzle: ta(nd.a); s.append("."); for (int k = 0; k < c; ++k) { sw1(nd.perm[k]); } break;
        case KOp::Splat: s.append(wtype(nd.type)); s.append("("); ta(nd.a); s.append(")"); break;
        case KOp::Dot: s.append("dot("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Cross: s.append("cross("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Normalize: s.append("normalize("); ta(nd.a); s.append(")"); break;
        case KOp::VecLen: s.append("length("); ta(nd.a); s.append(")"); break;
        case KOp::Reflect: s.append("reflect("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Refract: s.append("refract("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::Faceforward: s.append("faceForward("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::MatVecMul: // WGSL spells both as `*` (column-major), like GLSL
        case KOp::MatMatMul: ta(nd.a); s.append(" * "); ta(nd.b); break;
        case KOp::MatTranspose: s.append("transpose("); ta(nd.a); s.append(")"); break;
        case KOp::Determinant: s.append("determinant("); ta(nd.a); s.append(")"); break;
        case KOp::MatInverse: s.append(nd.type.rows == 2 ? "crd_inv2(" : "crd_inv3("); ta(nd.a); s.append(")"); break;
        // WGSL has no outerProduct(): column k of an RxC outer product is `a * b[k]`.
        case KOp::OuterProduct: { const int oc = nd.type.cols; s.append(wmat(nd.type.rows, oc)); s.append("("); for (int k = 0; k < oc; ++k) { if (k) { s.append(", "); } ta(nd.a); s.append(" * "); ta(nd.b); s.append("."); sw1(k); } s.append(")"); break; }
        case KOp::MatFromCols: { const int mcols = nd.type.cols; const int operand[4] = {nd.a, nd.b, nd.c, nd.d}; s.append(wtype(nd.type)); s.append("("); for (int k = 0; k < mcols; ++k) { if (k) { s.append(", "); } ta(operand[k]); } s.append(")"); break; }
        case KOp::VecAny: if (g.node(nd.a).dtype() == DType::Bool) { s.append("any("); ta(nd.a); s.append(")"); } else { s.append("any("); ta(nd.a); s.append(" != "); s.append(wtype(g.node(nd.a).type)); s.append("(0.0))"); } break;
        case KOp::VecAll: if (g.node(nd.a).dtype() == DType::Bool) { s.append("all("); ta(nd.a); s.append(")"); } else { s.append("all("); ta(nd.a); s.append(" != "); s.append(wtype(g.node(nd.a).type)); s.append("(0.0))"); } break;
        // WGSL relational operators are componentwise on vectors and already yield vecN<bool> (like HLSL, unlike GLSL).
        case KOp::CmpLt: s.append("("); ta(nd.a); s.append(" < "); ta(nd.b); s.append(")"); break;
        case KOp::CmpLe: s.append("("); ta(nd.a); s.append(" <= "); ta(nd.b); s.append(")"); break;
        case KOp::CmpGt: s.append("("); ta(nd.a); s.append(" > "); ta(nd.b); s.append(")"); break;
        case KOp::CmpGe: s.append("("); ta(nd.a); s.append(" >= "); ta(nd.b); s.append(")"); break;
        case KOp::CmpEq: s.append("("); ta(nd.a); s.append(" == "); ta(nd.b); s.append(")"); break;
        case KOp::CmpNe: s.append("("); ta(nd.a); s.append(" != "); ta(nd.b); s.append(")"); break;
        // no ternary in WGSL: select(false_value, true_value, condition)
        case KOp::Select: s.append("select("); ta(nd.b); s.append(", "); ta(nd.a); s.append(", "); if (g.node(nd.c).dtype() == DType::Bool) { ta(nd.c); } else { s.append("("); ta(nd.c); s.append(" != 0.0)"); } s.append(")"); break;
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

    // write-back. WGSL has NO implicit conversion on assignment, so a bool/int result converts explicitly.
    const KType& oty = g.node(output).type;
    const int    oc  = out.out_comps;
    if (oty.kind == TKind::Mat)
    {
        for (int col = 0; col < oty.cols; ++col)
        {
            for (int row = 0; row < oty.rows; ++row)
            {
                s.append("  outb[i*"); app_uint(s, oc); s.append("+"); app_uint(s, col * oty.rows + row); s.append("] = t"); app_uint(s, output); s.append("["); app_uint(s, col); s.append("]["); app_uint(s, row); s.append("];\n");
            }
        }
    }
    else if (oc == 1)
    {
        s.append("  outb[i] = ");
        if (oty.scalar == DType::Bool) { s.append("select(0.0, 1.0, t"); app_uint(s, output); s.append(")"); }
        else if (oty.scalar != DType::F32 && oty.scalar != DType::F64) { s.append("f32(t"); app_uint(s, output); s.append(")"); }
        else { s.append("t"); app_uint(s, output); }
        s.append(";\n");
    }
    else
    {
        for (int k = 0; k < oc; ++k)
        {
            s.append("  outb[i*"); app_uint(s, oc); s.append("+"); app_uint(s, k); s.append("] = ");
            if (oty.scalar == DType::Bool) { s.append("select(0.0, 1.0, t"); app_uint(s, output); s.append("["); app_uint(s, k); s.append("])"); }
            else if (oty.scalar != DType::F32 && oty.scalar != DType::F64) { s.append("f32(t"); app_uint(s, output); s.append("["); app_uint(s, k); s.append("])"); }
            else { s.append("t"); app_uint(s, output); s.append("["); app_uint(s, k); s.append("]"); }
            s.append(";\n");
        }
    }
    s.append("}\n");
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

// T2 FAST GEMM (DetTier::Fast) WGSL/WebGPU — the ported crush schedule (64×64 block, 4×4 microtile, TRANSPOSED-A
// var<workgroup>, `fma()`). Group-per-tile. Mirrors emit_contract_fast_glsl; WebGPU inherits the crush schedule.
inline bool emit_contract_fast_wgsl(const KGraph& g, int output, GlslKernel& out)
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
    s.append("var<workgroup> As : array<f32, 512>;\n"); // TRANSPOSED [k][m]
    s.append("var<workgroup> Bs : array<f32, 512>;\n");
    s.append("@compute @workgroup_size(256)\nfn cs_main(@builtin(workgroup_id) wid : vec3<u32>, @builtin(local_invocation_id) lid : vec3<u32>) {\n");
    s.append("  let K = pc.d1; let N = pc.d2;\n");
    s.append("  let nbc = N / 64u; let bid = wid.x;\n");
    s.append("  let blockRow = (bid / nbc) * 64u; let blockCol = (bid % nbc) * 64u;\n");
    s.append("  let tid = lid.x; let tr = tid / 16u; let tc = tid % 16u;\n");
    s.append("  let arow = blockRow + tr * 4u; let acol = blockCol + tc * 4u;\n");
    const char* d[4] = {"0", "1", "2", "3"};
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j) { s.append("  var a"); s.append(d[i]); s.append(d[j]); s.append(" : f32 = 0.0;\n"); }
    }
    s.append("  for (var k0 : u32 = 0u; k0 < K; k0 = k0 + 8u) {\n");
    s.append("    for (var t : u32 = tid; t < 512u; t = t + 256u) { let r = t / 8u; let cc = t % 8u; As[cc * 64u + r] = A[(blockRow + r) * K + (k0 + cc)]; }\n");
    s.append("    for (var t : u32 = tid; t < 512u; t = t + 256u) { let r = t / 64u; let cc = t % 64u; Bs[r * 64u + cc] = Bm[(k0 + r) * N + (blockCol + cc)]; }\n");
    s.append("    workgroupBarrier();\n");
    s.append("    for (var kk : u32 = 0u; kk < 8u; kk = kk + 1u) {\n");
    s.append("      let ar0 = As[kk*64u+tr*4u+0u]; let ar1 = As[kk*64u+tr*4u+1u]; let ar2 = As[kk*64u+tr*4u+2u]; let ar3 = As[kk*64u+tr*4u+3u];\n");
    s.append("      let br0 = Bs[kk*64u+tc*4u+0u]; let br1 = Bs[kk*64u+tc*4u+1u]; let br2 = Bs[kk*64u+tc*4u+2u]; let br3 = Bs[kk*64u+tc*4u+3u];\n");
    const char* ar[4] = {"ar0", "ar1", "ar2", "ar3"};
    const char* br[4] = {"br0", "br1", "br2", "br3"};
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            s.append("      a"); s.append(d[i]); s.append(d[j]); s.append(" = fma("); s.append(ar[i]); s.append(", "); s.append(br[j]); s.append(", a"); s.append(d[i]); s.append(d[j]); s.append(");\n");
        }
    }
    s.append("    }\n    workgroupBarrier();\n  }\n");
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
                case KOp::Cast: s.append("f32("); te(nd.a); s.append(")"); break;
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
