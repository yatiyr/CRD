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

// A3: comps-aware VECTOR emitter for HLSL/DX12 (mirror of emit_vec_glsl) — float/float2/3/4 temps, interleaved I/O,
// HLSL builtins + emitted quaternion helpers. Matrices (comps>4) are deferred (HLSL row-major convention) ⇒ bail cleanly.
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

    const char xyzw[4] = {'x', 'y', 'z', 'w'};
    const auto ta        = [&](int id) { s.append("t"); app_uint(s, static_cast<crd::u32>(id)); };
    const auto emit_expr = [&](int i) -> bool
    {
        const KNode& nd = g.node(i);
        const int    c  = nd.comps();
        // B0-4 SROA: the aggregate is never materialized; a FieldGet/ArrayGet resolves to the operand its index names.
        if (nd.op == KOp::StructMake || nd.op == KOp::ArrayMake) { return true; }
        if (nd.op == KOp::FieldGet || nd.op == KOp::ArrayGet)
        {
            const KNode& agg = g.node(nd.a);
            if (agg.op != KOp::StructMake && agg.op != KOp::ArrayMake) { return false; } // e.g. a Select of structs needs a real struct type
            s.append(glsl_detail::is_float_dtype(nd.dtype()) ? "  precise " : "  "); s.append(htype(nd.type));
            s.append(" t"); app_uint(s, i); s.append(" = "); ta(g.ext_operand(agg, nd.iidx)); s.append(";\n");
            return true;
        }
        s.append(glsl_detail::is_float_dtype(nd.dtype()) ? "  precise " : "  "); s.append(htype(nd.type)); s.append(" t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = ");
        switch (nd.op)
        {
        // mat: transpose(floatCxR(flat)) — our flat is column-major, HLSL's ctor fills row-major, so build the CxR
        // transpose from the flat scalars and flip it once. Matrix-ness is the TYPE's, not `c > 4` (mat2 has c == 4).
        case KOp::Input: { const int bd = binding_of[static_cast<crd::usize>(i)]; const bool is_mat = nd.type.kind == TKind::Mat; if (c == 1) { s.append("in"); app_uint(s, static_cast<crd::u32>(bd)); s.append("[gid]"); } else { if (is_mat) { s.append("transpose("); s.append(hmat(nd.type.cols, nd.type.rows)); } else { s.append(htype(nd.type)); } s.append("("); for (int k = 0; k < c; ++k) { if (k) { s.append(", "); } s.append("in"); app_uint(s, static_cast<crd::u32>(bd)); s.append("[gid*"); app_uint(s, static_cast<crd::u32>(c)); s.append("+"); app_uint(s, static_cast<crd::u32>(k)); s.append("]"); } s.append(")"); if (is_mat) { s.append(")"); } } break; }
        case KOp::Const: app_flit(s, nd.cval); break;
        case KOp::Cast: s.append(htype(nd.type)); s.append("("); ta(nd.a); s.append(")"); break;
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
        // any()/all() are bool-returning in HLSL and accept a numeric vector too (nonzero == true), so one form serves both.
        case KOp::VecAny: s.append("any("); ta(nd.a); s.append(")"); break;
        case KOp::VecAll: s.append("all("); ta(nd.a); s.append(")"); break;
        // B0-3: relational operators are componentwise on HLSL vectors and already yield boolN.
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
        // C column-vectors of R rows -> feed them as the ROWS of a CxR matrix, then transpose once -> our RxC.
        case KOp::MatFromCols: { const int mcols = nd.type.cols; const int operand[4] = {nd.a, nd.b, nd.c, nd.d}; s.append("transpose("); s.append(hmat(mcols, nd.type.rows)); s.append("("); for (int k = 0; k < mcols; ++k) { if (k) { s.append(", "); } ta(operand[k]); } s.append("))"); break; }
        case KOp::MatVecMul: // HLSL spells both as mul(); the row-major construct at MatFromCols keeps the convention
        case KOp::MatMatMul: s.append("mul("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::MatTranspose: s.append("transpose("); ta(nd.a); s.append(")"); break;
        case KOp::Determinant: s.append("determinant("); ta(nd.a); s.append(")"); break;
        case KOp::MatInverse: s.append(nd.type.rows == 2 ? "crd_inv2(" : "crd_inv3("); ta(nd.a); s.append(")"); break; // 4x4 is refused upstream by is_vec_fusable
        // a(R) (x) b(C) -> RxC. HLSL's ctor fills row-major and row r is exactly `a[r] * b`, so this handles any R,C
        // without a helper (and without the old float3x3-only `crd_outer`, which silently mistyped a 2x3 outer).
        case KOp::OuterProduct: { const int orows = nd.type.rows; s.append(hmat(orows, nd.type.cols)); s.append("("); for (int r = 0; r < orows; ++r) { if (r) { s.append(", "); } ta(nd.a); s.append("."); const char sw[2] = {xyzw[r], '\0'}; s.append(sw); s.append(" * "); ta(nd.b); } s.append(")"); break; }
        case KOp::QuatToMat3: s.append("crd_qmat("); ta(nd.a); s.append(")"); break;
        default: return false;
        }
        s.append(";\n");
        return true;
    };

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
