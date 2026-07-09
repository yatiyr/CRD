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
            in_dtype[out.n_inputs]                 = g.node(i).dtype;
            ++out.n_inputs;
        }
    }

    crd::containers::String& s = out.source;
    s.clear();
    for (int b = 0; b < out.n_inputs; ++b) { s.append("RWStructuredBuffer<"); s.append(ctype(in_dtype[b])); s.append("> in"); app_uint(s, b); s.append(" : register(u"); app_uint(s, b); s.append(");\n"); }
    s.append("RWStructuredBuffer<"); s.append(ctype(g.node(output).dtype)); s.append("> outb : register(u"); app_uint(s, out.n_inputs); s.append(");\n");
    s.append("cbuffer PC : register(b0) { uint n; };\n");
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 dtid : SV_DispatchThreadID) {\n  uint gid = dtid.x;\n  if (gid >= n) return;\n");
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        const bool ii = dt_is_int(nd.dtype);
        s.append(ii ? "  int t" : "  precise float t"); app_uint(s, i); s.append(" = ");
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
        case KOp::CmpLt: s.append("(("); ta(nd.a); s.append(" < "); ta(nd.b); s.append(") ? 1.0 : 0.0)"); break;
        case KOp::CmpEq: s.append("(("); ta(nd.a); s.append(" == "); ta(nd.b); s.append(") ? 1.0 : 0.0)"); break;
        case KOp::CmpLe: s.append("(("); ta(nd.a); s.append(" <= "); ta(nd.b); s.append(") ? 1.0 : 0.0)"); break;
        case KOp::CmpGt: s.append("(("); ta(nd.a); s.append(" > "); ta(nd.b); s.append(") ? 1.0 : 0.0)"); break;
        case KOp::CmpGe: s.append("(("); ta(nd.a); s.append(" >= "); ta(nd.b); s.append(") ? 1.0 : 0.0)"); break;
        case KOp::CmpNe: s.append("(("); ta(nd.a); s.append(" != "); ta(nd.b); s.append(") ? 1.0 : 0.0)"); break;
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
        case KOp::Select: s.append("(("); ta(nd.c); s.append(" != 0.0) ? "); ta(nd.a); s.append(" : "); ta(nd.b); s.append(")"); break;
        default: return false;
        }
        s.append(";\n");
    }
    s.append("  outb[gid] = t"); app_uint(s, output); s.append(";\n}\n");
    return true;
}

// HLSL type for a component count: 1=float, 2/3/4=floatN, 9=float3x3, 16=float4x4.
inline const char* htype(int comps) noexcept
{
    switch (comps) { case 2: return "float2"; case 3: return "float3"; case 4: return "float4"; case 9: return "float3x3"; case 16: return "float4x4"; default: return "float"; }
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
        if (nd.op == KOp::MatInverse && nd.comps == 16) { return false; } // mat4 inverse deferred on HLSL (huge cofactor formula)
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
        if (g.node(i).op == KOp::Input) { binding_of[static_cast<crd::usize>(i)] = out.n_inputs; out.input_iidx[out.n_inputs] = g.node(i).iidx; out.in_comps[out.n_inputs] = g.node(i).comps; ++out.n_inputs; }
    }
    out.out_comps              = g.node(output).comps;
    crd::containers::String& s = out.source;
    s.clear();
    for (int b = 0; b < out.n_inputs; ++b) { s.append("RWStructuredBuffer<float> in"); app_uint(s, static_cast<crd::u32>(b)); s.append(" : register(u"); app_uint(s, static_cast<crd::u32>(b)); s.append(");\n"); }
    s.append("RWStructuredBuffer<float> outb : register(u"); app_uint(s, static_cast<crd::u32>(out.n_inputs)); s.append(");\n");
    s.append("cbuffer PC : register(b0) { uint n; };\n");
    { // quaternion/slerp + matrix helpers (no HLSL builtins for these)
        bool qm = false, qc = false, qr = false, qa = false, sl = false, qt = false, iv = false, op = false;
        for (int i = 0; i < n; ++i) { if (!reach[static_cast<crd::usize>(i)]) { continue; } switch (g.node(i).op) { case KOp::QuatMul: qm = true; break; case KOp::QuatConj: qc = true; break; case KOp::QuatRotate: qr = true; break; case KOp::QuatAxisAngle: qa = true; break; case KOp::Slerp: sl = true; break; case KOp::QuatToMat3: qt = true; break; case KOp::MatInverse: iv = true; break; case KOp::OuterProduct: op = true; break; default: break; } }
        if (qm) { s.append("float4 crd_qmul(float4 a,float4 b){return float4(a.w*b.xyz+b.w*a.xyz+cross(a.xyz,b.xyz),a.w*b.w-dot(a.xyz,b.xyz));}\n"); }
        if (qc) { s.append("float4 crd_qconj(float4 q){return float4(-q.xyz,q.w);}\n"); }
        if (qr) { s.append("float3 crd_qrot(float4 q,float3 v){float3 t=2.0*cross(q.xyz,v);return v+q.w*t+cross(q.xyz,t);}\n"); }
        if (qa) { s.append("float4 crd_qaa(float3 ax,float an){float h=an*0.5;return float4(ax*sin(h),cos(h));}\n"); }
        if (sl) { s.append("float4 crd_slerp(float4 a,float4 b,float t){float d=dot(a,b);float sg=1.0;if(d<0.0){d=-d;sg=-1.0;}if(d>0.9995){return normalize(lerp(a,sg*b,t));}float th=acos(d);float sn=sin(th);return (sin((1.0-t)*th)*a+sin(t*th)*sg*b)/sn;}\n"); }
        if (qt) { s.append("float3x3 crd_qmat(float4 q){float x=q.x,y=q.y,z=q.z,w=q.w;return float3x3(1.0-2.0*(y*y+z*z),2.0*(x*y-w*z),2.0*(x*z+w*y),2.0*(x*y+w*z),1.0-2.0*(x*x+z*z),2.0*(y*z-w*x),2.0*(x*z-w*y),2.0*(y*z+w*x),1.0-2.0*(x*x+y*y));}\n"); } // row-major R[row][col]
        if (op) { s.append("float3x3 crd_outer(float3 a,float3 b){return float3x3(a.x*b.x,a.x*b.y,a.x*b.z,a.y*b.x,a.y*b.y,a.y*b.z,a.z*b.x,a.z*b.y,a.z*b.z);}\n"); }
        if (iv) { s.append("float3x3 crd_inv3(float3x3 m){float a=m._m00,b=m._m01,c=m._m02,d=m._m10,e=m._m11,f=m._m12,g=m._m20,h=m._m21,i=m._m22;float A=e*i-f*h,B=d*i-f*g,C=d*h-e*g;float iv=1.0/(a*A-b*B+c*C);return float3x3(A*iv,(c*h-b*i)*iv,(b*f-c*e)*iv,(f*g-d*i)*iv,(a*i-c*g)*iv,(c*d-a*f)*iv,(d*h-e*g)*iv,(b*g-a*h)*iv,(a*e-b*d)*iv);}\n"); }
    }
    s.append("[numthreads(256,1,1)]\nvoid cs_main(uint3 dtid : SV_DispatchThreadID) {\n  uint gid = dtid.x;\n  if (gid >= n) return;\n");
    // A4 tier-2: body-scoping — mark loop-varying nodes (LoopIndex/LoopAcc + consumers; For = barrier) + their owning For.
    crd::containers::Array<crd::u8> varying(scratch);
    varying.resize(static_cast<crd::usize>(n), 0);
    for (int i = 0; i < n; ++i) { const KNode& v = g.node(i); if (v.op == KOp::For) { continue; } if (v.op == KOp::LoopIndex || v.op == KOp::LoopAcc) { varying[static_cast<crd::usize>(i)] = 1; } else if ((v.a >= 0 && varying[static_cast<crd::usize>(v.a)]) || (v.b >= 0 && varying[static_cast<crd::usize>(v.b)]) || (v.c >= 0 && varying[static_cast<crd::usize>(v.c)]) || (v.d >= 0 && varying[static_cast<crd::usize>(v.d)])) { varying[static_cast<crd::usize>(i)] = 1; } }
    crd::containers::Array<int> body_of(scratch);
    body_of.resize(static_cast<crd::usize>(n), -1);
    crd::containers::Array<int> rstk(scratch);
    for (int fi = 0; fi < n; ++fi) { if (g.node(fi).op != KOp::For) { continue; } rstk.push_back(g.node(fi).c); while (rstk.size() > 0) { const int bid = rstk[rstk.size() - 1]; rstk.resize(rstk.size() - 1); if (bid < 0 || !varying[static_cast<crd::usize>(bid)] || body_of[static_cast<crd::usize>(bid)] != -1) { continue; } body_of[static_cast<crd::usize>(bid)] = fi; const KNode& bn = g.node(bid); rstk.push_back(bn.a); rstk.push_back(bn.b); rstk.push_back(bn.c); rstk.push_back(bn.d); } }

    const char xyzw[4] = {'x', 'y', 'z', 'w'};
    const auto ta        = [&](int id) { s.append("t"); app_uint(s, static_cast<crd::u32>(id)); };
    const auto emit_expr = [&](int i) -> bool
    {
        const KNode& nd = g.node(i);
        const int    c  = nd.comps;
        s.append("  precise "); s.append(htype(c)); s.append(" t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = ");
        switch (nd.op)
        {
        case KOp::Input: { const int bd = binding_of[static_cast<crd::usize>(i)]; if (c == 1) { s.append("in"); app_uint(s, static_cast<crd::u32>(bd)); s.append("[gid]"); } else { if (c > 4) { s.append("transpose("); } s.append(htype(c)); s.append("("); for (int k = 0; k < c; ++k) { if (k) { s.append(", "); } s.append("in"); app_uint(s, static_cast<crd::u32>(bd)); s.append("[gid*"); app_uint(s, static_cast<crd::u32>(c)); s.append("+"); app_uint(s, static_cast<crd::u32>(k)); s.append("]"); } s.append(")"); if (c > 4) { s.append(")"); } } break; } // mat: transpose(floatNxN(flat)) — flat is column-major, HLSL ctor is row-major
        case KOp::Const: app_flit(s, nd.cval); break;
        case KOp::Cast: s.append(htype(c)); s.append("("); ta(nd.a); s.append(")"); break;
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
        case KOp::VecConcat: s.append(htype(c)); s.append("("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::VecComp: ta(nd.a); s.append("."); { const char sw[2] = {xyzw[nd.iidx], '\0'}; s.append(sw); } break;
        case KOp::Swizzle: ta(nd.a); s.append("."); for (int k = 0; k < c; ++k) { const char sw[2] = {xyzw[nd.perm[k]], '\0'}; s.append(sw); } break;
        case KOp::Splat: s.append(htype(c)); s.append("("); for (int k = 0; k < c; ++k) { if (k) { s.append(", "); } ta(nd.a); } s.append(")"); break;
        case KOp::Dot: s.append("dot("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Cross: s.append("cross("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Normalize: s.append("normalize("); ta(nd.a); s.append(")"); break;
        case KOp::VecLen: s.append("length("); ta(nd.a); s.append(")"); break;
        case KOp::Reflect: s.append("reflect("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Refract: s.append("refract("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::Faceforward: s.append("faceforward("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::VecAny: s.append("(any("); ta(nd.a); s.append(") ? 1.0 : 0.0)"); break;
        case KOp::VecAll: s.append("(all("); ta(nd.a); s.append(") ? 1.0 : 0.0)"); break;
        case KOp::Slerp: s.append("crd_slerp("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::QuatMul: s.append("crd_qmul("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::QuatConj: s.append("crd_qconj("); ta(nd.a); s.append(")"); break;
        case KOp::QuatRotate: s.append("crd_qrot("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::QuatAxisAngle: s.append("crd_qaa("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::MatFromCols: s.append("transpose("); s.append(htype(c)); s.append("("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); if (c == 16) { s.append(", "); ta(nd.d); } s.append("))"); break;
        case KOp::MatVecMul: s.append("mul("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::MatMatMul: s.append("mul("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::MatTranspose: s.append("transpose("); ta(nd.a); s.append(")"); break;
        case KOp::Determinant: s.append("determinant("); ta(nd.a); s.append(")"); break;
        case KOp::MatInverse: s.append("crd_inv3("); ta(nd.a); s.append(")"); break;
        case KOp::OuterProduct: s.append("crd_outer("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
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
            s.append("  precise "); s.append(htype(nd.comps)); s.append(" t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nd.b)); s.append(";\n");
            s.append("  for (int li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = 0; li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(" < int(t"); app_uint(s, static_cast<crd::u32>(nd.a)); s.append("); li_"); app_uint(s, static_cast<crd::u32>(i)); s.append("++) {\n");
            for (int bid = 0; bid < i; ++bid)
            {
                if (body_of[static_cast<crd::usize>(bid)] != i) { continue; }
                const KNode& bn = g.node(bid);
                if (bn.op == KOp::LoopIndex) { s.append("  precise float t"); app_uint(s, static_cast<crd::u32>(bid)); s.append(" = float(li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(");\n"); }
                else if (bn.op == KOp::LoopAcc) { s.append("  precise "); s.append(htype(bn.comps)); s.append(" t"); app_uint(s, static_cast<crd::u32>(bid)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(i)); s.append(";\n"); }
                else if (!emit_expr(bid)) { return false; }
            }
            s.append("  t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nd.c)); s.append(";\n  }\n");
        }
        else if (!emit_expr(i)) { return false; }
    }
    const int oc = out.out_comps;
    if (oc == 1) { s.append("  outb[gid] = t"); app_uint(s, static_cast<crd::u32>(output)); s.append(";\n"); }
    else if (oc <= 4) { for (int k = 0; k < oc; ++k) { s.append("  outb[gid*"); app_uint(s, static_cast<crd::u32>(oc)); s.append("+"); app_uint(s, static_cast<crd::u32>(k)); s.append("] = t"); app_uint(s, static_cast<crd::u32>(output)); s.append("["); app_uint(s, static_cast<crd::u32>(k)); s.append("];\n"); } }
    else { const int d = (oc == 16) ? 4 : 3; for (int col = 0; col < d; ++col) { for (int row = 0; row < d; ++row) { s.append("  outb[gid*"); app_uint(s, static_cast<crd::u32>(oc)); s.append("+"); app_uint(s, static_cast<crd::u32>(col * d + row)); s.append("] = t"); app_uint(s, static_cast<crd::u32>(output)); s.append("["); app_uint(s, static_cast<crd::u32>(row)); s.append("]["); app_uint(s, static_cast<crd::u32>(col)); s.append("];\n"); } } }
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
