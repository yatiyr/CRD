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
    int                     in_comps[kMaxKernelInputs]   = {}; // A3: per-binding vector width (1=scalar,2/3/4=vecN,9/16=mat) — for interleaved I/O sizing
    int                     out_comps                    = 1; // A3: output vector width
    explicit GlslKernel(crd::memory::IAllocator* a) : source(a) {}
};

namespace glsl_detail
{
// Accepts any integral index type (int / u32 / usize) WITHOUT a narrowing conversion at the call site: the ~60 callers
// pass `static_cast<crd::u32>(idx)`, which used to narrow straight back to this `int` parameter. The explicit cast here
// keeps the emitted text bit-identical (every value is a small non-negative index) while stating the truncation.
template <typename T>
inline void app_uint(crd::containers::String& s, T v) { char b[24]; std::snprintf(b, sizeof(b), "%d", static_cast<int>(v)); s.append(b); }
inline void app_flit(crd::containers::String& s, crd::f64 v) // a GLSL float literal (always has a '.' or exponent)
{
    char b[40];
    std::snprintf(b, sizeof(b), "%.9g", v);
    s.append(b);
    bool dotless = true;
    for (const char* p = b; *p != '\0'; ++p) { if (*p == '.' || *p == 'e' || *p == 'E' || *p == 'n' || *p == 'i') { dotless = false; break; } }
    if (dotless) { s.append(".0"); }
}
// Integer dtypes lower to `int` (bit ops, morton/radix); float dtypes to `float`. GLSL `int` is 32-bit; morton stays
// within 30 bits (positive) so signed `int` agrees bit-for-bit with the i64 CPU reference.
[[nodiscard]] inline bool        dt_is_int(DType d) noexcept { return d == DType::I32 || d == DType::I64; }
[[nodiscard]] inline bool        dt_is_uint(DType d) noexcept { return d == DType::U8 || d == DType::U32; }
[[nodiscard]] inline bool        is_float_dtype(DType d) noexcept { return !dt_is_int(d) && !dt_is_uint(d) && d != DType::Bool; }
[[nodiscard]] inline const char* ctype(DType d) noexcept
{
    if (d == DType::Bool) { return "bool"; }
    if (dt_is_uint(d)) { return "uint"; }
    return dt_is_int(d) ? "int" : "float";
}
// The element type of a storage BUFFER. std430 has no `bool` (its size is undefined), so a bool-typed value is stored
// as a float 0.0/1.0 — the CPU oracle materializes exactly those, so a readback still compares bit-exact.
[[nodiscard]] inline const char* buf_ctype(DType d) noexcept { return d == DType::Bool ? "float" : ctype(d); }
inline void app_ilit(crd::containers::String& s, crd::f64 v) { char b[24]; std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(v)); s.append(b); }
// An integer Const literal, dtype-aware: UNSIGNED dtypes get the `u` suffix so (a) a value > INT_MAX (32-bit masks / hash
// seeds — B6-b noise) is a valid `uint` literal rather than an out-of-range `int`, and (b) a `uint <op> literal` stays
// uint-vs-uint (type-strict GLSL rejects mixing `uint` with a bare `int` literal).
inline void app_int_const(crd::containers::String& s, crd::f64 v, DType dt) { app_ilit(s, v); if (dt_is_uint(dt)) { s.append("u"); } }
[[nodiscard]] inline bool is_fusable(KOp op) noexcept
{
    switch (op)
    {
    case KOp::Input: case KOp::Const: case KOp::Cast:
    case KOp::Neg: case KOp::Recip: case KOp::Abs: case KOp::Exp: case KOp::Log:
    case KOp::Sin: case KOp::Cos: case KOp::Sqrt: case KOp::Tanh: case KOp::Floor: case KOp::Ceil: case KOp::Sign: case KOp::Trunc: case KOp::Round:
    case KOp::Add: case KOp::Sub: case KOp::Mul: case KOp::Div: case KOp::Max: case KOp::Min:
    case KOp::CmpLt: case KOp::CmpEq: case KOp::CmpLe:
    case KOp::Shl: case KOp::Shr: case KOp::BitAnd: case KOp::BitOr: case KOp::BitXor:
    case KOp::Pow: case KOp::Step: case KOp::Fract: case KOp::Clamp: case KOp::Mix:
    case KOp::Rsqrt: case KOp::Exp2: case KOp::Log2: case KOp::Tan: case KOp::Radians: case KOp::Degrees: case KOp::Atan2: case KOp::Smoothstep:
    case KOp::Asin: case KOp::Acos: case KOp::Atan: case KOp::Sinh: case KOp::Cosh: case KOp::Cbrt: case KOp::Mod: case KOp::Fma:
    case KOp::CmpGt: case KOp::CmpGe: case KOp::CmpNe:
    case KOp::BitNot: case KOp::BitCount: case KOp::FindLSB: case KOp::FindMSB: case KOp::BitfieldExtract:
    case KOp::BitReverse: case KOp::Ldexp: case KOp::FloatBitsToInt: case KOp::IntBitsToFloat:
    case KOp::SubgroupBallot: case KOp::SubgroupBallotExclCount: case KOp::SubgroupMatch:
    case KOp::SubgroupAdd: case KOp::SubgroupMin: case KOp::SubgroupMax: case KOp::SubgroupAnd: case KOp::SubgroupOr: case KOp::SubgroupXor:
    case KOp::SubgroupInclusiveAdd: case KOp::SubgroupExclusiveAdd: case KOp::SubgroupBroadcastFirst: case KOp::SubgroupShuffle:
    case KOp::QuadBroadcast: case KOp::QuadSwapX: case KOp::QuadSwapY: case KOp::QuadSwapDiagonal:
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
        if (nd.d >= 0) { stk.push_back(nd.d); }
    }

    // binding map: distinct input iidx -> binding index (in first-seen id order)
    crd::containers::Array<int> binding_of(scratch); // per node id: the input binding, or -1
    binding_of.resize(static_cast<crd::usize>(n), -1);
    DType in_dtype[kMaxKernelInputs] = {}; // per binding: the input's dtype (typed storage buffers)
    out.n_inputs                     = 0;
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        if (g.node(i).op == KOp::Input)
        {
            binding_of[static_cast<crd::usize>(i)] = out.n_inputs;
            out.input_iidx[out.n_inputs]           = g.node(i).iidx;
            in_dtype[out.n_inputs]                 = g.node(i).dtype();
            ++out.n_inputs;
        }
    }

    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    for (int b = 0; b < out.n_inputs; ++b)
    {
        s.append("layout(std430, binding = "); app_uint(s, b); s.append(") readonly buffer B"); app_uint(s, b); s.append(" { "); s.append(ctype(in_dtype[b])); s.append(" in"); app_uint(s, b); s.append("[]; };\n");
    }
    s.append("layout(std430, binding = "); app_uint(s, out.n_inputs); s.append(") writeonly buffer BOUT { "); s.append(buf_ctype(g.node(output).dtype())); s.append(" outb[]; };\n");
    s.append("layout(push_constant) uniform PC { uint n; };\n");
    s.append("void main() {\n  uint gid = gl_GlobalInvocationID.x;\n  if (gid >= n) { return; }\n");

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
        case KOp::Const: if (ii) { app_int_const(s, nd.cval, nd.dtype()); } else { app_flit(s, nd.cval); } break;
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
        case KOp::Round: s.append("roundEven("); ta(nd.a); s.append(")"); break;
        case KOp::Sign: s.append("(("); ta(nd.a); s.append(" > 0.0) ? 1.0 : (("); ta(nd.a); s.append(" < 0.0) ? -1.0 : 0.0))"); break;
        case KOp::Add: ta(nd.a); s.append(" + "); ta(nd.b); break;
        case KOp::Sub: ta(nd.a); s.append(" - "); ta(nd.b); break;
        case KOp::Mul: ta(nd.a); s.append(" * "); ta(nd.b); break;
        case KOp::Div: ta(nd.a); s.append(" / "); ta(nd.b); break;
        case KOp::Max: s.append("max("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Min: s.append("min("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        // comparisons are BOOL-typed (B0-3): the temp is a `bool`, so no `? 1.0 : 0.0` lowering here.
        case KOp::CmpLt: s.append("("); ta(nd.a); s.append(" < "); ta(nd.b); s.append(")"); break;
        case KOp::CmpEq: s.append("("); ta(nd.a); s.append(" == "); ta(nd.b); s.append(")"); break;
        case KOp::CmpLe: s.append("("); ta(nd.a); s.append(" <= "); ta(nd.b); s.append(")"); break;
        case KOp::CmpGt: s.append("("); ta(nd.a); s.append(" > "); ta(nd.b); s.append(")"); break;
        case KOp::CmpGe: s.append("("); ta(nd.a); s.append(" >= "); ta(nd.b); s.append(")"); break;
        case KOp::CmpNe: s.append("("); ta(nd.a); s.append(" != "); ta(nd.b); s.append(")"); break;
        case KOp::BitNot: s.append("(~"); ta(nd.a); s.append(")"); break;
        case KOp::BitCount: s.append("bitCount("); ta(nd.a); s.append(")"); break;
        case KOp::FindLSB: s.append("findLSB("); ta(nd.a); s.append(")"); break;
        case KOp::FindMSB: s.append("findMSB("); ta(nd.a); s.append(")"); break;
        case KOp::BitfieldExtract: s.append("(("); ta(nd.a); s.append(" >> "); ta(nd.b); s.append(") & ((1 << "); ta(nd.c); s.append(") - 1))"); break;
        case KOp::BitReverse: s.append("bitfieldReverse("); ta(nd.a); s.append(")"); break;
        case KOp::Ldexp: s.append("ldexp("); ta(nd.a); s.append(", int("); ta(nd.b); s.append("))"); break;
        case KOp::FloatBitsToInt: s.append("floatBitsToInt("); ta(nd.a); s.append(")"); break;
        case KOp::IntBitsToFloat: s.append("intBitsToFloat("); ta(nd.a); s.append(")"); break;
        case KOp::Shl: ta(nd.a); s.append(" << "); ta(nd.b); break;
        case KOp::Shr: ta(nd.a); s.append(" >> "); ta(nd.b); break;
        case KOp::BitAnd: ta(nd.a); s.append(" & "); ta(nd.b); break;
        case KOp::BitOr: ta(nd.a); s.append(" | "); ta(nd.b); break;
        case KOp::BitXor: ta(nd.a); s.append(" ^ "); ta(nd.b); break;
        case KOp::Fract: s.append("("); ta(nd.a); s.append(" - floor("); ta(nd.a); s.append("))"); break;
        case KOp::Step: s.append("(("); ta(nd.b); s.append(" < "); ta(nd.a); s.append(") ? 0.0 : 1.0)"); break; // step(edge=a,x=b)
        case KOp::Pow: s.append("pow("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;             // ULP
        case KOp::Clamp: s.append("min(max("); ta(nd.a); s.append(", "); ta(nd.b); s.append("), "); ta(nd.c); s.append(")"); break;
        case KOp::Mix: s.append("("); ta(nd.a); s.append(" * (1.0 - "); ta(nd.c); s.append(") + "); ta(nd.b); s.append(" * "); ta(nd.c); s.append(")"); break;
        case KOp::Rsqrt: s.append("inversesqrt("); ta(nd.a); s.append(")"); break;
        case KOp::Exp2: s.append("exp2("); ta(nd.a); s.append(")"); break;
        case KOp::Log2: s.append("log2("); ta(nd.a); s.append(")"); break;
        case KOp::Tan: s.append("tan("); ta(nd.a); s.append(")"); break;
        case KOp::Radians: s.append("("); ta(nd.a); s.append(" * 0.017453292519943295)"); break; // π/180, exact
        case KOp::Degrees: s.append("("); ta(nd.a); s.append(" * 57.29577951308232)"); break;     // 180/π, exact
        case KOp::Atan2: s.append("atan("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break; // GLSL atan(y,x)
        case KOp::Smoothstep: s.append("smoothstep("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        case KOp::Asin: s.append("asin("); ta(nd.a); s.append(")"); break;
        case KOp::Acos: s.append("acos("); ta(nd.a); s.append(")"); break;
        case KOp::Atan: s.append("atan("); ta(nd.a); s.append(")"); break;
        case KOp::Sinh: s.append("sinh("); ta(nd.a); s.append(")"); break;
        case KOp::Cosh: s.append("cosh("); ta(nd.a); s.append(")"); break;
        case KOp::Cbrt: s.append("(sign("); ta(nd.a); s.append(") * pow(abs("); ta(nd.a); s.append("), 0.3333333333333333))"); break; // no builtin
        case KOp::Mod: s.append("("); ta(nd.a); s.append(" - "); ta(nd.b); s.append(" * trunc("); ta(nd.a); s.append(" / "); ta(nd.b); s.append("))"); break; // C fmod
        case KOp::Fma: s.append("fma("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
        // a Bool condition tests directly; a numeric one (bit-extraction flags) still compares against zero.
        case KOp::Select: s.append("("); if (g.node(nd.c).dtype() == DType::Bool) { ta(nd.c); } else { s.append("("); ta(nd.c); s.append(" != 0.0)"); } s.append(" ? "); ta(nd.a); s.append(" : "); ta(nd.b); s.append(")"); break;
        default: return false;
        }
        s.append(";\n");
    }
    // std430 cannot hold a `bool`, so a bool result is written as float 0.0/1.0 (matches the oracle's materialization).
    s.append("  outb[gid] = ");
    if (g.node(output).dtype() == DType::Bool) { s.append("float(t"); app_uint(s, output); s.append(")"); }
    else { s.append("t"); app_uint(s, output); }
    s.append(";\n}\n");
    return true;
}

// GLSL type name for a CKIR value type. GLSL spells a matrix `matCxR` -- COLUMNS first, then rows -- and abbreviates
// the square case to `matN`; its constructors and `m[col][row]` indexing are column-major, matching our flat storage
// exactly. This is the TRANSPOSE of HLSL's `floatRxC` spelling (see `htype` in ckir_hlsl.hpp); getting the two mixed up
// silently transposes every matrix, which is why naming is driven by KType and never by a component count (a comps==4
// value is a vec4 OR a mat2, and only the type knows which).
inline const char* vtype(KType t) noexcept
{
    if (t.kind == TKind::Mat)
    {
        if (t.rows == t.cols) { switch (t.rows) { case 2: return "mat2"; case 3: return "mat3"; case 4: return "mat4"; default: return "float"; } }
        switch (static_cast<int>(t.cols) * 10 + static_cast<int>(t.rows))
        {
        case 23: return "mat2x3"; case 24: return "mat2x4";
        case 32: return "mat3x2"; case 34: return "mat3x4";
        case 42: return "mat4x2"; case 43: return "mat4x3";
        default: return "float";
        }
    }
    // vecN / ivecN / uvecN / bvecN -- the component scalar picks the prefix. (Matrices are float-only in GLSL.)
    if (t.kind == TKind::Vec)
    {
        if (t.scalar == DType::Bool) { switch (t.rows) { case 2: return "bvec2"; case 3: return "bvec3"; case 4: return "bvec4"; default: break; } }
        else if (glsl_detail::dt_is_uint(t.scalar)) { switch (t.rows) { case 2: return "uvec2"; case 3: return "uvec3"; case 4: return "uvec4"; default: break; } }
        else if (glsl_detail::dt_is_int(t.scalar)) { switch (t.rows) { case 2: return "ivec2"; case 3: return "ivec3"; case 4: return "ivec4"; default: break; } }
        else { switch (t.rows) { case 2: return "vec2"; case 3: return "vec3"; case 4: return "vec4"; default: break; } }
    }
    return glsl_detail::ctype(t.scalar);
}

// B2: GLSL texture typing. Prefix by sampled scalar (float="" · int="i" · uint="u"); suffix by dim + arrayed/MS. The
// separable DECLARATION is `<prefix>texture<suffix>`; the COMBINED type at the sample site is `<prefix>sampler<suffix>`.
inline const char* glsl_tex_scalar_prefix(DType d) noexcept
{
    if (glsl_detail::dt_is_uint(d)) { return "u"; }
    if (glsl_detail::dt_is_int(d)) { return "i"; }
    return "";
}
inline const char* glsl_tex_dim_suffix(const KType& t) noexcept
{
    switch (t.tex_dim())
    {
    case TexDim::Tex1D:   return t.tex_arrayed() ? "1DArray" : "1D";
    case TexDim::Tex2D:   if (t.tex_ms()) { return "2DMS"; } return t.tex_arrayed() ? "2DArray" : "2D";
    case TexDim::Tex3D:   return "3D";
    case TexDim::TexCube: return t.tex_arrayed() ? "CubeArray" : "Cube";
    }
    return "2D";
}

// ops the vec/mat emitter fuses into one per-element kernel (scalar-fusable + the vec/mat value ops backed by GLSL builtins).
inline bool is_vec_fusable(KOp op) noexcept
{
    if (glsl_detail::is_fusable(op)) { return true; }
    switch (op)
    {
    case KOp::Vec2: case KOp::Vec3: case KOp::VecConcat: case KOp::VecComp: case KOp::Swizzle: case KOp::Splat:
    case KOp::Dot: case KOp::Cross: case KOp::Normalize: case KOp::VecLen: case KOp::Reflect: case KOp::Refract: case KOp::Faceforward:
    case KOp::MatVecMul: case KOp::MatMatMul: case KOp::MatTranspose: case KOp::Determinant: case KOp::MatInverse: case KOp::OuterProduct: case KOp::MatFromCols:
    case KOp::VecAny: case KOp::VecAll: case KOp::Slerp: case KOp::QuatMul: case KOp::QuatConj: case KOp::QuatRotate: case KOp::QuatAxisAngle: case KOp::QuatToMat3:
    case KOp::For: case KOp::LoopIndex: case KOp::LoopAcc:                                          // A4 tier-2 dynamic control flow
    case KOp::StructMake: case KOp::ArrayMake: case KOp::FieldGet: case KOp::ArrayGet:              // B0-4 aggregates (SROA'd)
    case KOp::StageIn: case KOp::Builtin: case KOp::UniformBlock: return true;                      // B3 raster leaves
    default: return false;
    }
}

// Does the graph reachable from `output` carry any vec/mat/bool/struct VALUE (or dynamic control flow)? ⇒ route to the
// type-aware emitter rather than the scalar one. Shared by every backend: it used to be copy-pasted as an
// EXTERNAL-LINKAGE function in both backend_vulkan.cpp and backend_dx12.cpp — two definitions of one symbol, an ODR
// violation that only stayed quiet because no program links both backends at once.
[[nodiscard]] inline bool graph_uses_vec(const KGraph& g, int output, crd::memory::IAllocator* scratch)
{
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
        if (nd.comps() > 1 || nd.op == KOp::For || nd.op == KOp::LoopIndex || nd.op == KOp::LoopAcc) { return true; }
        if (is_aggregate(nd.op)) { return true; } // a struct of one scalar has comps == 1, so route by OP
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
        if (nd.d >= 0) { stk.push_back(nd.d); }
        for (int k = 0; k < static_cast<int>(nd.n_ext); ++k) { stk.push_back(g.ext_operand(nd, k)); }
    }
    return false;
}

// B3-c: emit the `precise? <type> tN = ` statement prefix for node `i` (`precise` is a float-only qualifier in GLSL — a
// `precise bool`/`precise ivec3` is a compile error). Shared by the compute and raster statement paths.
inline void emit_stmt_prefix(const KGraph& g, int i, crd::containers::String& s)
{
    using namespace glsl_detail;
    const KNode& nd = g.node(i);
    s.append(is_float_dtype(nd.dtype()) ? "  precise " : "  ");
    s.append(vtype(nd.type));
    s.append(" t");
    app_uint(s, static_cast<crd::u32>(i));
    s.append(" = ");
}

// B3-c: the SHARED value-expression statement emitter — one home for the ~60 operation cases so the compute path AND the
// raster (VS/FS) path never duplicate them. Emits the full `precise? <type> tN = <rhs>;` for one node. The OPERATION cases
// (Const/Cast/math/vec/mat/quat/compare/select) are stage-agnostic; `leaf(g, i, s)` emits the WHOLE statement for a
// STAGE-SPECIFIC leaf (compute: `Input` → a storage-buffer read; raster: `StageIn`/`Builtin` → a stage input, and a
// `FieldGet` on a `UniformBlock` → `ubo.member`) and returns true when it handled node `i`. B0-4 SROA is handled here:
// `StructMake`/`ArrayMake` materialize nothing, and a `FieldGet`/`ArrayGet` on a value aggregate resolves straight to the
// operand temp its index names. Returns false on an op this emitter cannot lower — the caller refuses loudly.
template <typename LeafFn>
inline bool emit_value_stmt(const KGraph& g, int i, crd::containers::String& s, const LeafFn& leaf)
{
    using namespace glsl_detail;
    const KNode& nd = g.node(i);
    const int    c  = nd.comps();
    const char   xyzw[4] = {'x', 'y', 'z', 'w'};
    const auto   ta      = [&](int id) { s.append("t"); app_uint(s, static_cast<crd::u32>(id)); };
    // B2: the combined `<p>sampler<d>[Shadow](tex_S_B, samp_S_B)` expression from the texture (a) + sampler (b) leaves.
    const auto   samp_expr = [&]() {
        const KNode& tx = g.node(nd.a);
        const KNode& sm = g.node(nd.b);
        s.append(glsl_tex_scalar_prefix(tx.type.scalar)); s.append("sampler"); s.append(glsl_tex_dim_suffix(tx.type));
        if (sm.type.tex_shadow()) { s.append("Shadow"); }
        s.append("(tex_"); app_uint(s, static_cast<crd::u32>(tx.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(tx.iidx));
        s.append(", samp_"); app_uint(s, static_cast<crd::u32>(sm.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(sm.iidx)); s.append(")");
    };

    // B0-4 SROA: an aggregate is never materialized on the GPU. StructMake/ArrayMake emit nothing.
    if (nd.op == KOp::StructMake || nd.op == KOp::ArrayMake) { return true; }
    // FieldGet/ArrayGet: a value aggregate (from a Make) resolves to its operand temp — what Slang/DXC do. A NON-Make
    // aggregate is a UniformBlock (raster): the stage leaf reads `ubo.member`. A struct produced by a `Select` would need
    // a real GLSL struct type; refuse it loudly (compute's leaf returns false ⇒ this is a no-op there, matching the old code).
    if (nd.op == KOp::FieldGet || nd.op == KOp::ArrayGet)
    {
        const KNode& agg = g.node(nd.a);
        if (agg.op == KOp::StructMake || agg.op == KOp::ArrayMake)
        {
            emit_stmt_prefix(g, i, s);
            ta(g.ext_operand(agg, nd.iidx));
            s.append(";\n");
            return true;
        }
        return leaf(g, i, s); // raster UBO member (emits the whole statement); false on compute (unreachable there)
    }

    // Stage-specific leaves (Input / StageIn / Builtin) — the leaf emits the WHOLE statement (prefix + RHS + `;\n`).
    if (leaf(g, i, s)) { return true; }

    emit_stmt_prefix(g, i, s);
    switch (nd.op)
    {
    // Int/uint constants MUST emit an integer literal — type-strict GLSL rejects `int t = 0.0` (HLSL would coerce it).
    // D12: a spec constant references its module-scope `layout(constant_id=N)` name (declared in the stage prologue).
    case KOp::Const: if (is_spec_const(nd)) { s.append("_spec"); app_uint(s, static_cast<int>(spec_const_id(nd))); } else if (dt_is_int(nd.dtype()) || dt_is_uint(nd.dtype())) { app_int_const(s, nd.cval, nd.dtype()); } else { app_flit(s, nd.cval); } break;
    case KOp::Cast: s.append(vtype(nd.type)); s.append("("); ta(nd.a); s.append(")"); break;
    case KOp::Neg: s.append("-"); ta(nd.a); break;
    case KOp::Recip: s.append("(1.0 / "); ta(nd.a); s.append(")"); break;
    case KOp::Abs: s.append("abs("); ta(nd.a); s.append(")"); break;
    // GEO-1 (the compute-emitter-lag scar, 5th occurrence): the bit reinterprets — needed by the vertex-pulling VS
    case KOp::FloatBitsToInt: s.append("floatBitsToInt("); ta(nd.a); s.append(")"); break;
    case KOp::IntBitsToFloat: s.append("intBitsToFloat("); ta(nd.a); s.append(")"); break;
    case KOp::DFdx: s.append("dFdx("); ta(nd.a); s.append(")"); break;   // B1 fragment derivative ∂/∂x
    case KOp::DFdy: s.append("dFdy("); ta(nd.a); s.append(")"); break;   // B1 fragment derivative ∂/∂y
    case KOp::StorageLoad: s.append("sbuf.data["); ta(nd.a); s.append("]"); break; // B1-f: read the FS storage buffer
    case KOp::TexSample:  s.append("texture(");     samp_expr(); s.append(", "); ta(nd.c); s.append(")"); break; // B2 implicit-LOD
    case KOp::SampleLod:  s.append("textureLod(");  samp_expr(); s.append(", "); ta(nd.c); s.append(", "); ta(nd.d); s.append(")"); break; // B2-b
    case KOp::SampleGrad: s.append("textureGrad("); samp_expr(); s.append(", "); ta(nd.c); s.append(", "); ta(nd.d); s.append(", "); ta(g.ext_operand(nd, 0)); s.append(")"); break; // B2-b (ddy in ext)
    case KOp::SampleCmp:  s.append("texture(");     samp_expr(); s.append(", vec3("); ta(nd.c); s.append(", "); ta(nd.d); s.append("))"); break; // B2-b shadow: vec3(uv, ref) → float
    case KOp::TexelFetch: s.append("texelFetch(");  samp_expr(); s.append(", "); ta(nd.c); s.append(", "); ta(nd.d); s.append(")"); break; // B2-b integer fetch
    case KOp::TexGather:  s.append("textureGather("); samp_expr(); s.append(", "); ta(nd.c); s.append(", "); app_ilit(s, g.node(nd.d).cval); s.append(")"); break; // B2-b gather; comp is a compile-time literal
    case KOp::TexSize:    s.append("textureSize("); samp_expr(); s.append(", "); ta(nd.d); s.append(")"); break; // B2-b size query → ivecN
    case KOp::SampleIndexed: // B2-d: index a bindless texture ARRAY (nonuniformEXT — the index varies per fragment).
    {
        const KNode& tx = g.node(nd.a);
        const KNode& sm = g.node(nd.b);
        s.append("texture("); s.append(glsl_tex_scalar_prefix(tx.type.scalar)); s.append("sampler"); s.append(glsl_tex_dim_suffix(tx.type));
        s.append("(tex_"); app_uint(s, static_cast<crd::u32>(tx.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(tx.iidx));
        // A UNIFORM (compile-time constant) index needs no `nonuniformEXT` — and some drivers reject/return-zero for nonuniform
        // bindless in a MESH shader. Only a dynamic index gets the qualifier.
        if (g.node(nd.d).op == KOp::Const) { s.append("["); ta(nd.d); s.append("]"); }
        else { s.append("[nonuniformEXT("); ta(nd.d); s.append(")]"); }
        s.append(", samp_"); app_uint(s, static_cast<crd::u32>(sm.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(sm.iidx));
        s.append("), "); ta(nd.c); s.append(")");
        break;
    }
    case KOp::SampleIndexedLod: // B16: bindless ARRAY sample at an EXPLICIT LOD (VS displacement — no derivatives). lod in ext[0].
    {
        const KNode& tx = g.node(nd.a);
        const KNode& sm = g.node(nd.b);
        s.append("textureLod("); s.append(glsl_tex_scalar_prefix(tx.type.scalar)); s.append("sampler"); s.append(glsl_tex_dim_suffix(tx.type));
        s.append("(tex_"); app_uint(s, static_cast<crd::u32>(tx.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(tx.iidx));
        // A UNIFORM (compile-time constant) index needs no `nonuniformEXT` — and some drivers reject/return-zero for nonuniform
        // bindless in a MESH shader. Only a dynamic index gets the qualifier.
        if (g.node(nd.d).op == KOp::Const) { s.append("["); ta(nd.d); s.append("]"); }
        else { s.append("[nonuniformEXT("); ta(nd.d); s.append(")]"); }
        s.append(", samp_"); app_uint(s, static_cast<crd::u32>(sm.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(sm.iidx));
        s.append("), "); ta(nd.c); s.append(", "); ta(g.ext_operand(nd, 0)); s.append(")");
        break;
    }
    case KOp::Fwidth: s.append("fwidth("); ta(nd.a); s.append(")"); break; // B1 |dFdx|+|dFdy|
    case KOp::Sqrt: s.append("sqrt("); ta(nd.a); s.append(")"); break;
    case KOp::Rsqrt: s.append("inversesqrt("); ta(nd.a); s.append(")"); break;
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
    case KOp::Vec2: s.append(vtype(nd.type)); s.append("("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break; // uvec2/ivec2/vec2 by component type
    case KOp::Vec3: s.append(vtype(nd.type)); s.append("("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::VecConcat: s.append(vtype(nd.type)); s.append("("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::VecComp: ta(nd.a); s.append("."); { const char sw[2] = {xyzw[nd.iidx], '\0'}; s.append(sw); } break;
    case KOp::Swizzle: ta(nd.a); s.append("."); for (int k = 0; k < c; ++k) { const char sw[2] = {xyzw[nd.perm[k]], '\0'}; s.append(sw); } break;
    case KOp::Splat: s.append(vtype(nd.type)); s.append("("); ta(nd.a); s.append(")"); break;
    case KOp::Dot: s.append("dot("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::Cross: s.append("cross("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::Normalize: s.append("normalize("); ta(nd.a); s.append(")"); break;
    case KOp::VecLen: s.append("length("); ta(nd.a); s.append(")"); break;
    case KOp::Reflect: s.append("reflect("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::Refract: s.append("refract("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::Faceforward: s.append("faceforward("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::MatVecMul: // GLSL spells both as the `*` operator (column-major); HLSL needs mul() — see ckir_hlsl.hpp
    case KOp::MatMatMul: ta(nd.a); s.append(" * "); ta(nd.b); break;
    case KOp::MatTranspose: s.append("transpose("); ta(nd.a); s.append(")"); break;
    case KOp::Determinant: s.append("determinant("); ta(nd.a); s.append(")"); break;
    case KOp::MatInverse: s.append("inverse("); ta(nd.a); s.append(")"); break;
    case KOp::OuterProduct: s.append("outerProduct("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::MatFromCols: { const int mcols = nd.type.cols; const int operand[4] = {nd.a, nd.b, nd.c, nd.d}; s.append(vtype(nd.type)); s.append("("); for (int k = 0; k < mcols; ++k) { if (k) { s.append(", "); } ta(operand[k]); } s.append(")"); break; }
    case KOp::VecAny: if (g.node(nd.a).dtype() == DType::Bool) { s.append("any("); ta(nd.a); s.append(")"); } else { s.append("any(notEqual("); ta(nd.a); s.append(", "); s.append(vtype(g.node(nd.a).type)); s.append("(0.0)))"); } break;
    case KOp::VecAll: if (g.node(nd.a).dtype() == DType::Bool) { s.append("all("); ta(nd.a); s.append(")"); } else { s.append("all(notEqual("); ta(nd.a); s.append(", "); s.append(vtype(g.node(nd.a).type)); s.append("(0.0)))"); } break;
    case KOp::CmpLt: case KOp::CmpLe: case KOp::CmpGt: case KOp::CmpGe: case KOp::CmpEq: case KOp::CmpNe:
    {
        const bool  vecop = g.node(nd.a).type.kind == TKind::Vec;
        const char* fn    = "";
        const char* sym   = "";
        switch (nd.op)
        {
        case KOp::CmpLt: fn = "lessThan("; sym = " < "; break;
        case KOp::CmpLe: fn = "lessThanEqual("; sym = " <= "; break;
        case KOp::CmpGt: fn = "greaterThan("; sym = " > "; break;
        case KOp::CmpGe: fn = "greaterThanEqual("; sym = " >= "; break;
        case KOp::CmpEq: fn = "equal("; sym = " == "; break;
        default: fn = "notEqual("; sym = " != "; break;
        }
        if (vecop) { s.append(fn); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); }
        else { s.append("("); ta(nd.a); s.append(sym); ta(nd.b); s.append(")"); }
        break;
    }
    case KOp::Select: s.append("("); if (g.node(nd.c).dtype() == DType::Bool) { ta(nd.c); } else { s.append("("); ta(nd.c); s.append(" != 0.0)"); } s.append(" ? "); ta(nd.a); s.append(" : "); ta(nd.b); s.append(")"); break;
    case KOp::Slerp: s.append("crd_slerp("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::QuatMul: s.append("crd_qmul("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::QuatConj: s.append("crd_qconj("); ta(nd.a); s.append(")"); break;
    case KOp::QuatRotate: s.append("crd_qrot("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::QuatAxisAngle: s.append("crd_qaa("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::QuatToMat3: s.append("crd_qmat("); ta(nd.a); s.append(")"); break;
    // B6: the remaining scalar-math + integer bitwise ops — raster parity with the compute emit_kernel (same spellings, so
    // a fragment/vertex graph lowers identically). Materials (B6-a) use trunc/ceil/round/sign/tan/asin/acos/atan2/
    // smoothstep; the noise nodes (B6-b) use the bitwise ops for the Bob-Jenkins hash (uint → logical >>).
    case KOp::Tanh: s.append("tanh("); ta(nd.a); s.append(")"); break;
    case KOp::Trunc: s.append("trunc("); ta(nd.a); s.append(")"); break;
    case KOp::Ceil: s.append("ceil("); ta(nd.a); s.append(")"); break;
    case KOp::Round: s.append("roundEven("); ta(nd.a); s.append(")"); break; // ties-to-even, matches the CPU oracle (nearbyint)
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
    case KOp::Atan2: s.append("atan("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break; // GLSL atan(y,x)
    case KOp::Mod: s.append("("); ta(nd.a); s.append(" - "); ta(nd.b); s.append(" * trunc("); ta(nd.a); s.append(" / "); ta(nd.b); s.append("))"); break; // C fmod
    case KOp::Step: s.append("(("); ta(nd.b); s.append(" < "); ta(nd.a); s.append(") ? 0.0 : 1.0)"); break;
    case KOp::Smoothstep: s.append("smoothstep("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::Fma: s.append("fma("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
    case KOp::Shl: ta(nd.a); s.append(" << "); ta(nd.b); break;
    case KOp::Shr: ta(nd.a); s.append(" >> "); ta(nd.b); break;
    case KOp::BitAnd: ta(nd.a); s.append(" & "); ta(nd.b); break;
    case KOp::BitOr: ta(nd.a); s.append(" | "); ta(nd.b); break;
    case KOp::BitXor: ta(nd.a); s.append(" ^ "); ta(nd.b); break;
    case KOp::BitNot: s.append("(~"); ta(nd.a); s.append(")"); break; // B12-a: raster emitter lagged compute on these bit ops
    case KOp::BitCount: s.append("bitCount("); ta(nd.a); s.append(")"); break;
    case KOp::FindLSB: s.append("findLSB("); ta(nd.a); s.append(")"); break;
    case KOp::FindMSB: s.append("findMSB("); ta(nd.a); s.append(")"); break;
    case KOp::SubgroupBallot: s.append("subgroupBallot("); ta(nd.a); s.append(" != 0u).x"); break;
    case KOp::SubgroupBallotExclCount: s.append("subgroupBallotExclusiveBitCount(uvec4("); ta(nd.a); s.append(", 0u, 0u, 0u))"); break;
    case KOp::SubgroupMatch: s.append("subgroupPartitionNV("); ta(nd.a); s.append(").x"); break;
    case KOp::SubgroupAdd: s.append("subgroupAdd("); ta(nd.a); s.append(")"); break;
    case KOp::SubgroupMin: s.append("subgroupMin("); ta(nd.a); s.append(")"); break;
    case KOp::SubgroupMax: s.append("subgroupMax("); ta(nd.a); s.append(")"); break;
    case KOp::SubgroupAnd: s.append("subgroupAnd("); ta(nd.a); s.append(")"); break;
    case KOp::SubgroupOr: s.append("subgroupOr("); ta(nd.a); s.append(")"); break;
    case KOp::SubgroupXor: s.append("subgroupXor("); ta(nd.a); s.append(")"); break;
    case KOp::SubgroupInclusiveAdd: s.append("subgroupInclusiveAdd("); ta(nd.a); s.append(")"); break;
    case KOp::SubgroupExclusiveAdd: s.append("subgroupExclusiveAdd("); ta(nd.a); s.append(")"); break;
    case KOp::SubgroupBroadcastFirst: s.append("subgroupBroadcastFirst("); ta(nd.a); s.append(")"); break;
    case KOp::SubgroupShuffle: s.append("subgroupShuffle("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::QuadBroadcast: s.append("subgroupQuadBroadcast("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
    case KOp::QuadSwapX: s.append("subgroupQuadSwapHorizontal("); ta(nd.a); s.append(")"); break;
    case KOp::QuadSwapY: s.append("subgroupQuadSwapVertical("); ta(nd.a); s.append(")"); break;
    case KOp::QuadSwapDiagonal: s.append("subgroupQuadSwapDiagonal("); ta(nd.a); s.append(")"); break;
    default: return false;
    }
    s.append(";\n");
    return true;
}

// B3-c: the GLSL spelling of a KBuiltin for the VERTEX/FRAGMENT stages. nullptr for a builtin this emitter does not lower
// yet (compute/tess/geo/RT) — the caller refuses loudly rather than emit something wrong.
[[nodiscard]] inline const char* glsl_vsfs_builtin_name(KBuiltin b) noexcept
{
    switch (b)
    {
    case KBuiltin::VertexIndex:   return "gl_VertexIndex";
    case KBuiltin::InstanceIndex: return "gl_InstanceIndex";
    case KBuiltin::FragCoord:     return "gl_FragCoord";
    case KBuiltin::FrontFacing:   return "gl_FrontFacing";
    case KBuiltin::PrimitiveId:   return "gl_PrimitiveID"; // B4-vis-4: the HW-raster visibility-buffer primitive id (int)
    case KBuiltin::PointCoord:    return "gl_PointCoord";
    case KBuiltin::InnerCoverage: return "gl_FragFullyCoveredNV"; // B1-f: a bool (NV underestimation) — read as uint()
    // B4: workgroup builtins — legal only in a MESH/task/compute stage (entry_valid rejects them in VS/FS), so listing them
    // here is harmless for the raster path and lets the mesh emitter reuse this leaf.
    case KBuiltin::LocalInvocationIndex: return "gl_LocalInvocationIndex";
    case KBuiltin::WorkgroupIndex:       return "gl_WorkGroupID.x";
    case KBuiltin::LocalInvocationId:    return "gl_LocalInvocationID";
    case KBuiltin::WorkgroupId:          return "gl_WorkGroupID";
    case KBuiltin::GlobalInvocationId:   return "gl_GlobalInvocationID";
    case KBuiltin::NumWorkgroups:        return "gl_NumWorkGroups";
    case KBuiltin::TaskPayload:          return "mesh_payload.v0"; // B4: the task→mesh payload (mesh stage reads it)
    case KBuiltin::TaskPayload1:         return "mesh_payload.v1"; // B4: payload field 1
    case KBuiltin::TaskPayload2:         return "mesh_payload.v2"; // B4: payload field 2
    case KBuiltin::TaskPayload3:         return "mesh_payload.v3"; // B4: payload field 3
    case KBuiltin::TessCoord:            return "gl_TessCoord";     // B4-tess: the domain coord in the TessEval stage
    case KBuiltin::TessPatchPosition:    return "patch_pos";        // B4-tess: emitter-provided bilerp of the patch corners
    default:                      return nullptr;
    }
}

// B1-c: the GLSL interpolation qualifier (with a trailing space so it slots before `in`/`out`). Smooth is the default → "".
[[nodiscard]] inline const char* glsl_interp(Interp i) noexcept
{
    switch (i)
    {
    case Interp::Flat:          return "flat ";
    case Interp::NoPerspective: return "noperspective ";
    case Interp::Centroid:      return "centroid ";
    case Interp::Sample:        return "sample ";
    default:                    return ""; // Smooth (perspective-correct)
    }
}

// ── B-cmp: emit an imperative COMPUTE KERNEL (shared memory + barriers + storage buffers) to GLSL ─────────────────────────
// The hand-authored on-chip kernel path (FFT / reduction / transpose / conv), distinct from the functional elementwise
// emitter. Resource decls (buffers + `shared` arrays) + the KEntry statement body with INLINE value expressions (a SharedLoad
// must emit at its statement, never hoisted across a barrier). Returns false on an op it can't yet lower (refuses loudly).
// NOLINTBEGIN(readability-function-size) -- one branch per KStmtKind/KOp, each a few lines of textual emission.
// Splitting it would only scatter a flat dispatch across helpers that share the whole local emitter state
// (`s`, `temped`, `decl`, `pv`), which is how the two dialects drift apart. Same rationale as ckir_cuda.hpp.
inline bool emit_compute_kernel_glsl(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    if (!entry.is_kernel()) { return false; }
    const int                n = g.size();
    crd::containers::String& s = out.source;
    s.clear();
    out.n_inputs = 0;
    bool uses_rayquery = false; // B9/RT-1: inline ray query needs GLSL 4.60 + GL_EXT_ray_query when an AccelStructDecl is present
    for (int i = 0; i < n; ++i) { if (g.node(i).op == KOp::AccelStructDecl) { uses_rayquery = true; break; } }
    s.append(uses_rayquery ? "#version 460\n" : "#version 450\n"); // GL_EXT_ray_query's rayQueryEXT type requires #version 460
    s.append("#extension GL_KHR_shader_subgroup_basic : require\n");  // B-cmp: subgroup (wave) ops — the cheap deterministic
    s.append("#extension GL_KHR_shader_subgroup_ballot : require\n"); // radix rank; bit-exact under a forced 32-lane subgroup
    s.append("#extension GL_KHR_shader_subgroup_arithmetic : require\n"); // B11: subgroupAdd/Min/Max/And/Or/Xor + inclusive/exclusive scan
    s.append("#extension GL_KHR_shader_subgroup_shuffle : require\n"); // B11: subgroupShuffle (broadcastFirst is in _ballot)
    s.append("#extension GL_KHR_shader_subgroup_quad : require\n"); // B11: subgroupQuadBroadcast / subgroupQuadSwap{Horizontal,Vertical,Diagonal}
    s.append("#extension GL_NV_shader_subgroup_partitioned : enable\n"); // hardware match_any (SubgroupMatch); SPIR-V cap emitted only when used
    if (uses_rayquery) { s.append("#extension GL_EXT_ray_query : require\n"); }
    s.append("layout(local_size_x = "); app_uint(s, entry.local_size[0]);
    s.append(", local_size_y = ");      app_uint(s, entry.local_size[1]);
    s.append(", local_size_z = ");      app_uint(s, entry.local_size[2]); s.append(") in;\n");
    bool spec_declared[256] = {}; // D12: dedup spec-constant declarations by constant_id (one `layout(constant_id=N)` per id)
    for (int i = 0; i < n; ++i) // resource decls: storage buffers + workgroup shared arrays + acceleration structures
    {
        const KNode& nd = g.node(i);
        if (nd.op == KOp::BufferDecl && !is_spec_const(nd))
        {
            s.append("layout(std430, binding = "); app_uint(s, nd.iidx); s.append(") ");
            if ((nd.axes & 2U) != 0U) { s.append("coherent volatile "); } // cross-workgroup visible (spin-wait publish/read)
            s.append(nd.axes != 0U ? "" : "readonly "); s.append("buffer B"); app_uint(s, nd.iidx);
            s.append(" { "); s.append(buf_ctype(nd.dtype())); s.append(" buf"); app_uint(s, nd.iidx); s.append("[]; };\n");
        }
        else if (is_spec_const(nd)) // D12: a specialization constant — pipeline-time-overridable module-scope scalar const
        {
            const crd::u32 id = spec_const_id(nd);
            if (id < 256U && !spec_declared[id])
            {
                spec_declared[id] = true;
                s.append("layout(constant_id = "); app_uint(s, static_cast<int>(id)); s.append(") const ");
                const char* spec_ctype = "float";
                if (nd.dtype() == DType::Bool) { spec_ctype = "bool"; }
                else if (dt_is_uint(nd.dtype())) { spec_ctype = "uint"; }
                else if (dt_is_int(nd.dtype())) { spec_ctype = "int"; }
                s.append(spec_ctype);
                s.append(" _spec"); app_uint(s, static_cast<int>(id)); s.append(" = ");
                if (nd.dtype() == DType::Bool) { s.append(nd.cval != 0.0 ? "true" : "false"); }
                else if (dt_is_uint(nd.dtype()) || dt_is_int(nd.dtype())) { app_int_const(s, nd.cval, nd.dtype()); }
                else { app_flit(s, nd.cval); }
                s.append(";\n");
            }
        }
        else if (nd.op == KOp::SharedDecl)
        {
            s.append("shared "); s.append(buf_ctype(nd.dtype())); s.append(" sh"); app_uint(s, i);
            s.append("["); app_uint(s, nd.iidx + static_cast<int>(nd.axes)); s.append("];\n");
        }
        else if (nd.op == KOp::AccelStructDecl) // B9/RT-1: opaque TLAS bound at (set 0, binding)
        {
            s.append("layout(set = 0, binding = "); app_uint(s, nd.iidx); s.append(") uniform accelerationStructureEXT as"); app_uint(s, nd.iidx); s.append(";\n");
        }
    }
    s.append("void main() {\n");

    // DETERMINISM: every FLOAT arithmetic node materializes as a `precise` temp (SPIR-V NoContraction ⇒ no FMA fusion ⇒
    // bit-matches the CPU oracle's op-by-op rounding — the same lever the elementwise emitter uses). LEAVES (loads/consts/
    // builtins/loop-var) + cast/select/compare/bitops stay INLINE, so a shared/buffer load RE-READS at each use (correct
    // across barriers). Temps CSE by node id; every kernel authors FRESH load nodes after a barrier, so a load-derived temp
    // is never referenced across a barrier (and sibling loops never share an arithmetic node ⇒ no cross-scope temp).
    bool                            ok = true;
    crd::containers::Array<crd::u8> temped(scratch);
    temped.resize(static_cast<crd::usize>(n), 0);
    // ⛔ DAG MEMO for `decl` below. `decl` recurses into every child, and `temped` gates only the temp EMISSION — not the
    // descent. So a node referenced by k parents had its WHOLE subtree re-walked k times ⇒ traversal cost exponential in DAG
    // depth. Shallow kernels never noticed; a deep diamond-shaped one (B18-b Huang: stacked normalize()/cross() chains) made
    // emit_compute_kernel_glsl hang outright. Memoizing the visit is behaviour-IDENTICAL (a re-visit emits nothing, since
    // temped[] is already 1 and the children are already declared) and makes emission linear in graph size.
    crd::containers::Array<crd::u8> declseen(scratch);
    declseen.resize(static_cast<crd::usize>(n), 0);
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
    // pv: print a node's VALUE — a temp reference if it was materialized, else its inline expression (children via pv).
    const auto pv = [&](auto&& self, int node) -> void {
        if (temped[static_cast<crd::usize>(node)] != 0U) { s.append("t"); app_uint(s, static_cast<crd::u32>(node)); return; }
        const KNode& nd  = g.node(node);
        const auto   bin = [&](const char* o) { s.append("("); self(self, nd.a); s.append(o); self(self, nd.b); s.append(")"); };
        // AND/OR/XOR of BOOL operands (e.g. Perlin gradient3's `(h==12) | (h==14)`) must be LOGICAL in GLSL (`bool | bool` is a
        // type error); on integer operands they stay bitwise. Pick by the operand dtype.
        const auto   binbit = [&](const char* opi, const char* opb) { bin(g.node(nd.a).dtype() == DType::Bool ? opb : opi); };
        switch (nd.op)
        {
        case KOp::Const:
            if (is_spec_const(nd)) { s.append("_spec"); app_uint(s, static_cast<int>(spec_const_id(nd))); } // D12: the pipeline-time spec constant
            else if (nd.dtype() == DType::Bool) { s.append(nd.cval != 0.0 ? "true" : "false"); }
            // app_int_const uses %lld (full 64-bit) + a `u` suffix for uint — a u32 const > INT_MAX (hash seeds like
            // 2654435761) must NOT go through static_cast<int> (MSVC clamps out-of-range double→int to INT_MIN, mangling it).
            else if (dt_is_uint(nd.dtype()) || dt_is_int(nd.dtype())) { app_int_const(s, nd.cval, nd.dtype()); }
            else { app_flit(s, nd.cval); }
            break;
        case KOp::Builtin:
            if (static_cast<KBuiltin>(nd.iidx) == KBuiltin::LocalInvocationIndex) { s.append("gl_LocalInvocationIndex"); }
            else if (static_cast<KBuiltin>(nd.iidx) == KBuiltin::WorkgroupIndex) { s.append("gl_WorkGroupID.x"); }
            else { ok = false; s.append("0u"); }
            break;
        case KOp::KernelLoopVar: s.append("lv"); app_uint(s, nd.a); break;
        case KOp::BufferLoad: s.append("buf"); app_uint(s, g.node(nd.a).iidx); s.append("["); self(self, nd.b); s.append("]"); break;
        case KOp::SharedLoad: s.append("sh"); app_uint(s, nd.a); s.append("["); self(self, nd.b); s.append("]"); break;
        case KOp::Cast: { const char* ct = "float("; if (dt_is_uint(nd.dtype())) { ct = "uint("; } else if (dt_is_int(nd.dtype())) { ct = "int("; } s.append(ct); self(self, nd.a); s.append(")"); break; }
        case KOp::CmpLt: bin(" < "); break;
        case KOp::CmpLe: bin(" <= "); break;
        case KOp::CmpGt: bin(" > "); break;
        case KOp::CmpGe: bin(" >= "); break;
        case KOp::CmpEq: bin(" == "); break;
        case KOp::CmpNe: bin(" != "); break;
        case KOp::BitAnd: binbit(" & ", " && "); break;
        case KOp::BitOr: binbit(" | ", " || "); break;
        case KOp::BitXor: binbit(" ^ ", " != "); break; // xor of bools = not-equal
        case KOp::Shl: bin(" << "); break;
        case KOp::Shr: bin(" >> "); break;
        case KOp::Select: s.append("("); self(self, nd.c); s.append(" ? "); self(self, nd.a); s.append(" : "); self(self, nd.b); s.append(")"); break; // a=true b=false c=cond
        case KOp::BitNot: s.append("(~"); self(self, nd.a); s.append(")"); break;
        case KOp::BitCount: s.append("bitCount("); self(self, nd.a); s.append(")"); break;
        case KOp::SubgroupBallot: s.append("subgroupBallot("); self(self, nd.a); s.append(" != 0u).x"); break;
        case KOp::SubgroupBallotExclCount: s.append("subgroupBallotExclusiveBitCount(uvec4("); self(self, nd.a); s.append(", 0u, 0u, 0u))"); break;
        case KOp::SubgroupMatch: s.append("subgroupPartitionNV("); self(self, nd.a); s.append(").x"); break;
        case KOp::SubgroupAdd: s.append("subgroupAdd("); self(self, nd.a); s.append(")"); break;
        case KOp::SubgroupMin: s.append("subgroupMin("); self(self, nd.a); s.append(")"); break;
        case KOp::SubgroupMax: s.append("subgroupMax("); self(self, nd.a); s.append(")"); break;
        case KOp::SubgroupAnd: s.append("subgroupAnd("); self(self, nd.a); s.append(")"); break;
        case KOp::SubgroupOr: s.append("subgroupOr("); self(self, nd.a); s.append(")"); break;
        case KOp::SubgroupXor: s.append("subgroupXor("); self(self, nd.a); s.append(")"); break;
        case KOp::SubgroupInclusiveAdd: s.append("subgroupInclusiveAdd("); self(self, nd.a); s.append(")"); break;
        case KOp::SubgroupExclusiveAdd: s.append("subgroupExclusiveAdd("); self(self, nd.a); s.append(")"); break;
        case KOp::SubgroupBroadcastFirst: s.append("subgroupBroadcastFirst("); self(self, nd.a); s.append(")"); break;
        case KOp::SubgroupShuffle: s.append("subgroupShuffle("); self(self, nd.a); s.append(", "); self(self, nd.b); s.append(")"); break;
        case KOp::QuadBroadcast: s.append("subgroupQuadBroadcast("); self(self, nd.a); s.append(", "); self(self, nd.b); s.append(")"); break;
        case KOp::QuadSwapX: s.append("subgroupQuadSwapHorizontal("); self(self, nd.a); s.append(")"); break;
        case KOp::QuadSwapY: s.append("subgroupQuadSwapVertical("); self(self, nd.a); s.append(")"); break;
        case KOp::QuadSwapDiagonal: s.append("subgroupQuadSwapDiagonal("); self(self, nd.a); s.append(")"); break;
        default: ok = false; s.append("0"); break; // an arithmetic node reaches pv only via a temp ref above
        }
    };
    // the RHS of a materialized arithmetic node (children referenced via pv).
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
        // ⛔ Exp2 was MISSING here while Log2 was present, so any compute kernel using it failed to emit at all
        //    (emit returns false) rather than miscompiling — the same COMPUTE-EMITTER-LAGS scar as Exp/Pow/Round.
        //    When adding a KOp, wire EVERY emitter, not just the one the current test happens to exercise.
        case KOp::Exp2: f1("exp2"); break;
        // ── COMPUTE-EMITTER COMPLETENESS (2026-07-20). Enumerating the oracle's KOp set against this switch turned up 20
        //    ops it could evaluate but this emitter could not lower, every one a latent `emit == false`. Spellings copied
        //    from the raster value emitter in this same file so the two paths cannot disagree. DFdx/DFdy/Fwidth are
        //    deliberately absent: screen-space derivatives do not exist in a compute kernel and MUST keep failing.
        case KOp::Rsqrt: f1("inversesqrt"); break;
        case KOp::Tan: f1("tan"); break;
        case KOp::Trunc: f1("trunc"); break;
        case KOp::Ldexp: f2("ldexp"); break;
        case KOp::Smoothstep: s.append("smoothstep("); pv(pv, nd.a); s.append(", "); pv(pv, nd.b); s.append(", "); pv(pv, nd.c); s.append(")"); break;
        case KOp::Recip: s.append("(1.0/"); pv(pv, nd.a); s.append(")"); break;
        case KOp::Sign: s.append("(("); pv(pv, nd.a); s.append(" > 0.0) ? 1.0 : (("); pv(pv, nd.a); s.append(" < 0.0) ? -1.0 : 0.0))"); break;
        case KOp::Fract: s.append("("); pv(pv, nd.a); s.append(" - floor("); pv(pv, nd.a); s.append("))"); break;
        case KOp::Step: s.append("(("); pv(pv, nd.b); s.append(" < "); pv(pv, nd.a); s.append(") ? 0.0 : 1.0)"); break; // step(edge=a, x=b)
        case KOp::Mix: s.append("("); pv(pv, nd.a); s.append(" * (1.0 - "); pv(pv, nd.c); s.append(") + "); pv(pv, nd.b); s.append(" * "); pv(pv, nd.c); s.append(")"); break;
        case KOp::Cbrt: s.append("(sign("); pv(pv, nd.a); s.append(") * pow(abs("); pv(pv, nd.a); s.append("), 0.3333333333333333))"); break; // no builtin
        case KOp::BitReverse: f1("bitfieldReverse"); break;
        case KOp::FindLSB: f1("findLSB"); break;
        case KOp::FindMSB: f1("findMSB"); break;
        case KOp::FloatBitsToInt: f1("floatBitsToInt"); break;
        case KOp::IntBitsToFloat: f1("intBitsToFloat"); break;
        case KOp::BitfieldExtract: s.append("bitfieldExtract("); pv(pv, nd.a); s.append(", int("); pv(pv, nd.b); s.append("), int("); pv(pv, nd.c); s.append("))"); break;
        case KOp::Tanh: f1("tanh"); break;
        case KOp::Atan2: f2("atan"); break; // GLSL 2-arg atan(y, x)
        case KOp::Atan: f1("atan"); break;
        case KOp::Asin: f1("asin"); break;
        case KOp::Acos: f1("acos"); break;
        case KOp::Sinh: f1("sinh"); break;
        case KOp::Cosh: f1("cosh"); break;
        case KOp::Floor: f1("floor"); break;
        case KOp::Ceil: f1("ceil"); break; // B4-vis: bbox ceil (the compute-kernel rhs lacked it — only the elementwise path had it)
        case KOp::Round: s.append("roundEven("); pv(pv, nd.a); s.append(")"); break; // B18-a: ties-to-even, matches oracle nearbyint (Np Δφ wrap)
        case KOp::Radians: s.append("("); pv(pv, nd.a); s.append(" * 0.017453292519943295)"); break; // B18-a: π/180 exact (cuticle-scale tilt)
        case KOp::Degrees: s.append("("); pv(pv, nd.a); s.append(" * 57.29577951308232)"); break; // 180/π (sibling of Radians, parity with elementwise)
        case KOp::Add: b2(" + "); break;
        case KOp::Sub: b2(" - "); break;
        case KOp::Mul: b2(" * "); break;
        case KOp::Div: b2(" / "); break;
        case KOp::Min: f2("min"); break;
        case KOp::Max: f2("max"); break;
        case KOp::Clamp: s.append("min(max("); pv(pv, nd.a); s.append(", "); pv(pv, nd.b); s.append("), "); pv(pv, nd.c); s.append(")"); break; // B4-vis: bbox clamp (matches oracle + HLSL)
        case KOp::Mod: if (dt_is_int(nd.dtype()) || dt_is_uint(nd.dtype())) { b2(" % "); } else { f2("mod"); } break; // GLSL mod() is float-only
        case KOp::Fma: s.append("fma("); pv(pv, nd.a); s.append(", "); pv(pv, nd.b); s.append(", "); pv(pv, nd.c); s.append(")"); break;
        case KOp::BitNot: s.append("(~"); pv(pv, nd.a); s.append(")"); break;
        case KOp::BitCount: s.append("bitCount("); pv(pv, nd.a); s.append(")"); break;
        case KOp::SubgroupBallot: s.append("subgroupBallot("); pv(pv, nd.a); s.append(" != 0u).x"); break;
        case KOp::SubgroupBallotExclCount: s.append("subgroupBallotExclusiveBitCount(uvec4("); pv(pv, nd.a); s.append(", 0u, 0u, 0u))"); break;
        case KOp::SubgroupMatch: s.append("subgroupPartitionNV("); pv(pv, nd.a); s.append(").x"); break;
        case KOp::SubgroupAdd: s.append("subgroupAdd("); pv(pv, nd.a); s.append(")"); break;
        case KOp::SubgroupMin: s.append("subgroupMin("); pv(pv, nd.a); s.append(")"); break;
        case KOp::SubgroupMax: s.append("subgroupMax("); pv(pv, nd.a); s.append(")"); break;
        case KOp::SubgroupAnd: s.append("subgroupAnd("); pv(pv, nd.a); s.append(")"); break;
        case KOp::SubgroupOr: s.append("subgroupOr("); pv(pv, nd.a); s.append(")"); break;
        case KOp::SubgroupXor: s.append("subgroupXor("); pv(pv, nd.a); s.append(")"); break;
        case KOp::SubgroupInclusiveAdd: s.append("subgroupInclusiveAdd("); pv(pv, nd.a); s.append(")"); break;
        case KOp::SubgroupExclusiveAdd: s.append("subgroupExclusiveAdd("); pv(pv, nd.a); s.append(")"); break;
        case KOp::SubgroupBroadcastFirst: s.append("subgroupBroadcastFirst("); pv(pv, nd.a); s.append(")"); break;
        case KOp::SubgroupShuffle: s.append("subgroupShuffle("); pv(pv, nd.a); s.append(", "); pv(pv, nd.b); s.append(")"); break;
        case KOp::QuadBroadcast: s.append("subgroupQuadBroadcast("); pv(pv, nd.a); s.append(", "); pv(pv, nd.b); s.append(")"); break;
        case KOp::QuadSwapX: s.append("subgroupQuadSwapHorizontal("); pv(pv, nd.a); s.append(")"); break;
        case KOp::QuadSwapY: s.append("subgroupQuadSwapVertical("); pv(pv, nd.a); s.append(")"); break;
        case KOp::QuadSwapDiagonal: s.append("subgroupQuadSwapDiagonal("); pv(pv, nd.a); s.append(")"); break;
        default: ok = false; s.append("0"); break;
        }
    };
    // decl: materialize temps for the arithmetic nodes in a subtree (children first, CSE by node id).
    const auto decl = [&](auto&& self, int node) -> void {
        if (declseen[static_cast<crd::usize>(node)] != 0U) { return; } // DAG memo — see declseen above
        declseen[static_cast<crd::usize>(node)] = 1U;
        const KNode& nd = g.node(node);
        if (nd.op == KOp::BufferLoad || nd.op == KOp::SharedLoad) { self(self, nd.b); return; } // resource leaf: only the index carries temps
        if (nd.a >= 0) { self(self, nd.a); }
        if (nd.b >= 0) { self(self, nd.b); }
        if (nd.c >= 0) { self(self, nd.c); }
        if (!is_inline_op(nd.op) && temped[static_cast<crd::usize>(node)] == 0U)
        {
            temped[static_cast<crd::usize>(node)] = 1U;
            s.append(is_float_dtype(nd.dtype()) ? "  precise " : "  ");
            s.append(buf_ctype(nd.dtype())); s.append(" t"); app_uint(s, static_cast<crd::u32>(node)); s.append(" = ");
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
            case KStmtKind::BufferStore: decl(decl, st.index); decl(decl, st.value); s.append("  buf"); app_uint(s, g.node(st.target).iidx); s.append("["); pv(pv, st.index); s.append("] = "); pv(pv, st.value); s.append(";\n"); ++i; break;
            case KStmtKind::SharedStore: decl(decl, st.index); decl(decl, st.value); s.append("  sh"); app_uint(s, st.target); s.append("["); pv(pv, st.index); s.append("] = "); pv(pv, st.value); s.append(";\n"); ++i; break;
            case KStmtKind::Barrier: s.append("  barrier();\n"); s.append(st.scope == BarrierScope::Workgroup ? "  memoryBarrierShared();\n" : "  memoryBarrierBuffer();\n"); ++i; break;
            case KStmtKind::Materialize: // FREEZE st.value into a temp NOW (survives a later shared overwrite)
                decl(decl, st.value);
                if (temped[static_cast<crd::usize>(st.value)] == 0U)
                {
                    s.append(is_float_dtype(g.node(st.value).dtype()) ? "  precise " : "  ");
                    s.append(buf_ctype(g.node(st.value).dtype())); s.append(" t"); app_uint(s, static_cast<crd::u32>(st.value)); s.append(" = ");
                    pv(pv, st.value); s.append(";\n");
                    temped[static_cast<crd::usize>(st.value)] = 1U;
                }
                ++i;
                break;
            case KStmtKind::For: decl(decl, st.value); s.append("  for (uint lv"); app_uint(s, i); s.append(" = 0u; lv"); app_uint(s, i); s.append(" < uint("); pv(pv, st.value); s.append("); ++lv"); app_uint(s, i); s.append(") {\n"); self_b(self_b, st.body_begin, st.body_count); s.append("  }\n"); i = st.body_begin + st.body_count; break;
            case KStmtKind::If: decl(decl, st.value); s.append("  if ("); pv(pv, st.value); s.append(") {\n"); self_b(self_b, st.body_begin, st.body_count); s.append("  }\n"); i = st.body_begin + st.body_count; break;
            case KStmtKind::SpinUntilNonzero: decl(decl, st.index); s.append("  while (buf"); app_uint(s, g.node(st.target).iidx); s.append("["); pv(pv, st.index); s.append("] == 0u) { memoryBarrierBuffer(); }\n"); ++i; break;
            case KStmtKind::SharedAtomicAdd: decl(decl, st.index); decl(decl, st.value); s.append("  atomicAdd(sh"); app_uint(s, st.target); s.append("["); pv(pv, st.index); s.append("], "); pv(pv, st.value); s.append(");\n"); ++i; break;
            case KStmtKind::BufferAtomicAdd: decl(decl, st.index); decl(decl, st.value); s.append("  atomicAdd(buf"); app_uint(s, g.node(st.target).iidx); s.append("["); pv(pv, st.index); s.append("], "); pv(pv, st.value); s.append(");\n"); ++i; break;
            case KStmtKind::BufferAtomicMin: decl(decl, st.index); decl(decl, st.value); s.append("  atomicMin(buf"); app_uint(s, g.node(st.target).iidx); s.append("["); pv(pv, st.index); s.append("], "); pv(pv, st.value); s.append(");\n"); ++i; break; // B4-vis: visibility key (nearest wins)
            case KStmtKind::BufferAtomicAddFetch: decl(decl, st.index); decl(decl, st.value); s.append("  "); s.append(buf_ctype(g.node(st.result).dtype())); s.append(" t"); app_uint(s, static_cast<crd::u32>(st.result)); s.append(" = atomicAdd(buf"); app_uint(s, g.node(st.target).iidx); s.append("["); pv(pv, st.index); s.append("], "); pv(pv, st.value); s.append(");\n"); temped[static_cast<crd::usize>(st.result)] = 1U; ++i; break; // B17: value-returning node allocator
            case KStmtKind::BufferAtomicExchange: decl(decl, st.index); decl(decl, st.value); s.append("  "); s.append(buf_ctype(g.node(st.result).dtype())); s.append(" t"); app_uint(s, static_cast<crd::u32>(st.result)); s.append(" = atomicExchange(buf"); app_uint(s, g.node(st.target).iidx); s.append("["); pv(pv, st.index); s.append("], "); pv(pv, st.value); s.append(");\n"); temped[static_cast<crd::usize>(st.result)] = 1U; ++i; break; // B17: linked-list head push
            case KStmtKind::TraceRayCurves: // B18-f: procedural curve BLAS — the shader intersects each candidate AABB's
            {                               // linear swept sphere and COMMITS it; hardware cannot resolve this itself.
                for (int k = 0; k < 8; ++k) { decl(decl, g.stmt_ext_operand(st, k)); }
                const auto op = [&](int k) { pv(pv, g.stmt_ext_operand(st, k)); };
                const crd::u32 bnd  = g.node(st.target).iidx;
                const crd::u32 sbuf = g.node(g.stmt_ext_operand(st, 8)).iidx;
                const crd::u32 res  = static_cast<crd::u32>(st.result);
                const crd::u32 ures = static_cast<crd::u32>(g.stmt_ext_operand(st, 9));
                const crd::u32 pres = static_cast<crd::u32>(g.stmt_ext_operand(st, 10));

                s.append("  rayQueryEXT rq"); app_uint(s, res); s.append(";\n");
                s.append("  rayQueryInitializeEXT(rq"); app_uint(s, res); s.append(", as"); app_uint(s, bnd);
                s.append(", gl_RayFlagsNoneEXT, 0xFFu, vec3("); op(0); s.append(", "); op(1); s.append(", "); op(2); s.append("), ");
                op(6); s.append(", vec3("); op(3); s.append(", "); op(4); s.append(", "); op(5); s.append("), "); op(7); s.append(");\n");
                s.append("  float cu"); app_uint(s, res); s.append(" = 0.0;\n");
                s.append("  while (rayQueryProceedEXT(rq"); app_uint(s, res); s.append(")) {\n");
                s.append("    if (rayQueryGetIntersectionTypeEXT(rq"); app_uint(s, res);
                s.append(", false) != gl_RayQueryCandidateIntersectionAABBEXT) { continue; }\n");
                s.append("    uint sgi = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq"); app_uint(s, res); s.append(", false)) * 8u;\n");
                s.append("    vec3 pa = vec3(buf"); app_uint(s, sbuf); s.append("[sgi], buf"); app_uint(s, sbuf);
                s.append("[sgi+1u], buf"); app_uint(s, sbuf); s.append("[sgi+2u]);\n");
                s.append("    float ra = buf"); app_uint(s, sbuf); s.append("[sgi+3u];\n");
                s.append("    vec3 pb = vec3(buf"); app_uint(s, sbuf); s.append("[sgi+4u], buf"); app_uint(s, sbuf);
                s.append("[sgi+5u], buf"); app_uint(s, sbuf); s.append("[sgi+6u]);\n");
                s.append("    float rb = buf"); app_uint(s, sbuf); s.append("[sgi+7u];\n");
                s.append("    vec3 ro = vec3("); op(0); s.append(", "); op(1); s.append(", "); op(2); s.append(");\n");
                s.append("    vec3 rdr = vec3("); op(3); s.append(", "); op(4); s.append(", "); op(5); s.append(");\n");
                s.append("    float rl = max(length(rdr), 1e-20); float rinv = 1.0/rl; vec3 rd = rdr*rinv;\n");
                // ⛔⛔ RE-ORIGIN THE RAY AT THE SEGMENT BEFORE SOLVING. The round-cone quadratic subtracts quantities of
                //    order |ro-pa|^2 to recover a term of order |ba|^2*ra^2. With the camera ~1 unit from a 68 micron
                //    fibre those differ by EIGHT orders of magnitude, so in f32 the radius information sits below the
                //    cancellation noise of the very terms carrying it, and the solve commits hits that are not on the
                //    surface at all (measured: ~6x the fibre radius off-axis). Sliding the origin down the ray to the
                //    segment makes every term local-scale. One dot product, and the solve is restored.
                //    THIS IS WHY IT APPEARED ONLY AT REALISTIC THICKNESS: at a fat 0.75mm radius the same term sat
                //    right at the edge of f32 and mostly survived, so fat strands looked fine.
                s.append("    float tsh = dot(pa - ro, rd); vec3 roL = ro + rd*tsh;\n");
                s.append("    vec3 ba = pb - pa; vec3 oa = roL - pa; vec3 ob = roL - pb; float rr = ra - rb;\n");
                s.append("    float m0 = dot(ba,ba), m1 = dot(ba,oa), m2 = dot(ba,rd);\n");
                s.append("    float m3 = dot(rd,oa), m5 = dot(oa,oa), m6 = dot(ob,rd), m7 = dot(ob,ob);\n");
                s.append("    float d2 = m0 - rr*rr;\n");
                s.append("    float tminu = "); op(6); s.append(" * rl;\n");
                // ⛔ Seed the candidate search from the CURRENTLY COMMITTED t, not the ray's original tmax:
                //    rayQueryGenerateIntersectionEXT requires tHit inside the ray's CURRENT range, which traversal
                //    narrows on every commit. Scaled by rl because the search below runs in unit-direction units.
                s.append("    float ct"); app_uint(s, res); s.append(" = (rayQueryGetIntersectionTypeEXT(rq"); app_uint(s, res);
                s.append(", true) != gl_RayQueryCommittedIntersectionNoneEXT) ? rayQueryGetIntersectionTEXT(rq"); app_uint(s, res);
                s.append(", true) : "); op(7); s.append(";\n");
                s.append("    float bt = ct"); app_uint(s, res); s.append(" * rl; float bu = 0.0; bool got = false;\n");
                s.append("    float k2 = d2 - m2*m2;\n");
                s.append("    float k1 = d2*m3 - m1*m2 + m2*rr*ra;\n");
                s.append("    float k0 = d2*m5 - m1*m1 + m1*rr*ra*2.0 - m0*ra*ra;\n");
                s.append("    float hh = k1*k1 - k0*k2;\n");
                s.append("    if (hh > 0.0 && abs(k2) > 1e-20) { float tc = (-sqrt(hh) - k1)/k2;\n");
                s.append("      float yc = m1 - ra*rr + tc*m2;\n");
                // every candidate t is measured from the SHIFTED origin, so add tsh back before comparing or committing
                s.append("      float tcT = tc + tsh;\n");
                s.append("      if (tcT > tminu && tcT < bt && yc > 0.0 && yc < d2) { bt = tcT; bu = yc/max(d2,1e-20); got = true; } }\n");
                s.append("    float h1 = m3*m3 - m5 + ra*ra; float h2 = m6*m6 - m7 + rb*rb;\n");
                s.append("    if (h1 > 0.0) { float t1 = -m3 - sqrt(h1);\n");
                s.append("      float t1T = t1 + tsh; if (t1T > tminu && t1T < bt) { bt = t1T; bu = 0.0; got = true; } }\n");
                s.append("    if (h2 > 0.0) { float t2 = -m6 - sqrt(h2);\n");
                s.append("      float t2T = t2 + tsh; if (t2T > tminu && t2T < bt) { bt = t2T; bu = 1.0; got = true; } }\n");
                s.append("    if (got) { cu"); app_uint(s, res); s.append(" = bu; rayQueryGenerateIntersectionEXT(rq"); app_uint(s, res); s.append(", bt*rinv); }\n");
                s.append("  }\n");
                s.append("  bool hit"); app_uint(s, res); s.append(" = (rayQueryGetIntersectionTypeEXT(rq"); app_uint(s, res);
                s.append(", true) == gl_RayQueryCommittedIntersectionGeneratedEXT);\n");
                s.append("  float t"); app_uint(s, res); s.append(" = hit"); app_uint(s, res);
                s.append(" ? rayQueryGetIntersectionTEXT(rq"); app_uint(s, res); s.append(", true) : "); op(7); s.append(";\n");
                s.append("  float t"); app_uint(s, ures); s.append(" = hit"); app_uint(s, res); s.append(" ? cu"); app_uint(s, res); s.append(" : 0.0;\n");
                // WHICH segment won: read back from the committed intersection rather than tracking it through the
                // candidate loop, so it cannot drift out of step with the t the hardware actually kept.
                s.append("  uint t"); app_uint(s, pres); s.append(" = hit"); app_uint(s, res);
                s.append(" ? uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq"); app_uint(s, res);
                s.append(", true)) : 0xFFFFFFFFu;\n");

                temped[static_cast<crd::usize>(st.result)] = 1U;
                temped[static_cast<crd::usize>(g.stmt_ext_operand(st, 9))] = 1U;
                temped[static_cast<crd::usize>(g.stmt_ext_operand(st, 10))] = 1U;
                ++i;
                break;
            }
            case KStmtKind::TraceRayClosest: // B9/RT-1: inline ray query — closest-hit distance `t` (or tmax on miss)
            {
                for (int k = 0; k < 8; ++k) { decl(decl, g.stmt_ext_operand(st, k)); } // materialize ox..dz, tmin, tmax
                const auto op = [&](int k) { pv(pv, g.stmt_ext_operand(st, k)); };
                const crd::u32 bnd = g.node(st.target).iidx;
                const crd::u32 res = static_cast<crd::u32>(st.result);
                s.append("  rayQueryEXT rq"); app_uint(s, res); s.append(";\n");
                s.append("  rayQueryInitializeEXT(rq"); app_uint(s, res); s.append(", as"); app_uint(s, bnd);
                s.append(", gl_RayFlagsNoneEXT, 0xFFu, vec3("); op(0); s.append(", "); op(1); s.append(", "); op(2); s.append("), ");
                op(6); s.append(", vec3("); op(3); s.append(", "); op(4); s.append(", "); op(5); s.append("), "); op(7); s.append(");\n");
                s.append("  while (rayQueryProceedEXT(rq"); app_uint(s, res); s.append(")) {}\n");
                s.append("  float t"); app_uint(s, res); s.append(" = (rayQueryGetIntersectionTypeEXT(rq"); app_uint(s, res);
                s.append(", true) == gl_RayQueryCommittedIntersectionTriangleEXT) ? rayQueryGetIntersectionTEXT(rq"); app_uint(s, res);
                s.append(", true) : "); op(7); s.append(";\n");
                temped[static_cast<crd::usize>(st.result)] = 1U;
                ++i;
                break;
            }
            case KStmtKind::TraceRayHit: // B9/RT-2: inline ray query — closest-hit distance `t` + PRIMITIVE INDEX (for shading)
            {
                for (int k = 0; k < 8; ++k) { decl(decl, g.stmt_ext_operand(st, k)); }
                const auto op = [&](int k) { pv(pv, g.stmt_ext_operand(st, k)); };
                const crd::u32 bnd  = g.node(st.target).iidx;
                const crd::u32 res  = static_cast<crd::u32>(st.result);
                const crd::u32 prim = static_cast<crd::u32>(g.stmt_ext_operand(st, 8));
                s.append("  rayQueryEXT rq"); app_uint(s, res); s.append(";\n");
                s.append("  rayQueryInitializeEXT(rq"); app_uint(s, res); s.append(", as"); app_uint(s, bnd);
                s.append(", gl_RayFlagsNoneEXT, 0xFFu, vec3("); op(0); s.append(", "); op(1); s.append(", "); op(2); s.append("), ");
                op(6); s.append(", vec3("); op(3); s.append(", "); op(4); s.append(", "); op(5); s.append("), "); op(7); s.append(");\n");
                s.append("  while (rayQueryProceedEXT(rq"); app_uint(s, res); s.append(")) {}\n");
                s.append("  bool hit"); app_uint(s, res); s.append(" = (rayQueryGetIntersectionTypeEXT(rq"); app_uint(s, res);
                s.append(", true) == gl_RayQueryCommittedIntersectionTriangleEXT);\n");
                s.append("  float t"); app_uint(s, res); s.append(" = hit"); app_uint(s, res); s.append(" ? rayQueryGetIntersectionTEXT(rq"); app_uint(s, res); s.append(", true) : "); op(7); s.append(";\n");
                s.append("  uint t"); app_uint(s, prim); s.append(" = hit"); app_uint(s, res); s.append(" ? uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq"); app_uint(s, res); s.append(", true)) : 0xFFFFFFFFu;\n");
                temped[static_cast<crd::usize>(st.result)] = 1U;
                temped[static_cast<crd::usize>(g.stmt_ext_operand(st, 8))] = 1U;
                ++i;
                break;
            }
            case KStmtKind::ForBreakIf: decl(decl, st.value); s.append("  if (bool("); pv(pv, st.value); s.append(")) break;\n"); ++i; break; // bool() accepts bool AND uint (type-strict GLSL)
            case KStmtKind::BufferTicket: decl(decl, st.index); s.append("  if (gl_LocalInvocationIndex == 0u) { sh"); app_uint(s, st.value); s.append("[0] = atomicAdd(buf"); app_uint(s, g.node(st.target).iidx); s.append("["); pv(pv, st.index); s.append("], 1u); }\n"); ++i; break;
            case KStmtKind::SyncWarp: s.append("  subgroupBarrier();\n"); ++i; break;
            // ⛔ RT-PIPELINE statements have no meaning in a COMPUTE kernel — but this switch has no `default`, and only
            //    its cases advance `i`. An unhandled kind therefore spins forever rather than failing, the same latent
            //    hang that existed in the CPU oracle's statement switch. Consume them and report failure instead.
            case KStmtKind::TraceRayPipeline:
            case KStmtKind::PayloadStore:
            case KStmtKind::ReorderThread:
            case KStmtKind::IgnoreHitIf: ok = false; ++i; break;
            }
        }
    };
    emit_body(emit_body, entry.kernel_body_begin, entry.kernel_body_count);
    s.append("}\n");
    return ok;
}
// NOLINTEND(readability-function-size)

// FA-2 (portable RT PIPELINE): emit a raygen / closest-hit / miss GLSL shader (GL_EXT_ray_tracing) from a KEntry whose body is the
// statement pool [kernel_body_begin, +count). The RAYGEN traces via `traceRayEXT` (or, when `ser` is set AND the body requests a
// reorder, the SER hitObject flow: hitObjectTraceRayNV → reorderThreadNV → hitObjectExecuteShaderNV) and stores the payload; the
// CLOSEST-HIT / MISS stages write the shared payload. `ser` is the caller's capability answer — the reorder HINT is portable and
// simply drops on a target without SER (perf only). The same three KEntries lower to DXR HLSL via emit_rt_stage_hlsl.
inline bool emit_rt_stage_glsl(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* /*scratch*/, GlslKernel& out, bool ser)
{
    using namespace glsl_detail;
    const KStage st = entry.stage;
    if (st != KStage::RayGen && st != KStage::ClosestHit && st != KStage::Miss && st != KStage::AnyHit) { return false; }
    const int                n = g.size();
    crd::containers::String& s = out.source;
    s.clear();

    int payload_n = 1;
    for (int i = 0; i < n; ++i) { if (g.node(i).op == KOp::RayPayloadDecl) { payload_n = g.node(i).iidx > 0 ? g.node(i).iidx : 1; } }
    const int b0 = entry.kernel_body_begin;
    const int bn = entry.kernel_body_count;
    bool      has_reorder = false;
    for (int i = 0; i < bn; ++i) { if (g.stmt(b0 + i).kind == KStmtKind::ReorderThread) { has_reorder = true; } }
    const bool use_ser = ser && has_reorder && st == KStage::RayGen;

    s.append("#version 460\n#extension GL_EXT_ray_tracing : require\n");
    if (use_ser) { s.append("#extension GL_NV_shader_invocation_reorder : require\n"); }
    s.append("struct RtPayload { ");
    for (int c = 0; c < payload_n; ++c) { s.append("float m"); app_uint(s, c); s.append("; "); }
    s.append("};\n");
    for (int i = 0; i < n; ++i) // resource decls: AS + storage buffers
    {
        const KNode& nd = g.node(i);
        if (nd.op == KOp::AccelStructDecl) { s.append("layout(set = 0, binding = "); app_uint(s, nd.iidx); s.append(") uniform accelerationStructureEXT as"); app_uint(s, nd.iidx); s.append(";\n"); }
        else if (nd.op == KOp::BufferDecl) { s.append("layout(std430, binding = "); app_uint(s, nd.iidx); s.append(") buffer B"); app_uint(s, nd.iidx); s.append(" { "); s.append(buf_ctype(nd.dtype())); s.append(" buf"); app_uint(s, nd.iidx); s.append("[]; };\n"); }
    }
    if (st == KStage::RayGen) { s.append("layout(location = 0) rayPayloadEXT RtPayload pl;\n"); }
    else { s.append("layout(location = 0) rayPayloadInEXT RtPayload pl;\n"); if (st != KStage::Miss) { s.append("hitAttributeEXT vec2 hattr;\n"); } }
    s.append("void main() {\n");

    // compact recursive value printer for the op subset RT-pipeline shaders use (everything inlined — the bodies are small).
    const char* xyzw = "xyzw";
    const auto  pv = [&](auto&& self, int node) -> void {
        const KNode& nd = g.node(node);
        const auto   bin = [&](const char* o) { s.append("("); self(self, nd.a); s.append(o); self(self, nd.b); s.append(")"); };
        switch (nd.op)
        {
        case KOp::Const:
            if (nd.dtype() == DType::Bool) { s.append(nd.cval != 0.0 ? "true" : "false"); }
            else if (nd.dtype() == DType::U32) { char b[32]; std::snprintf(b, sizeof(b), "%uu", static_cast<unsigned>(static_cast<crd::i64>(nd.cval))); s.append(b); }
            else if (nd.dtype() == DType::I32) { char b[32]; std::snprintf(b, sizeof(b), "%d", static_cast<int>(static_cast<crd::i64>(nd.cval))); s.append(b); }
            else { char b[40]; std::snprintf(b, sizeof(b), "%.9g", nd.cval); s.append(b); if (std::strchr(b, '.') == nullptr && std::strchr(b, 'e') == nullptr && std::strchr(b, 'n') == nullptr) { s.append(".0"); } }
            break;
        case KOp::Builtin:
            switch (static_cast<KBuiltin>(nd.iidx))
            {
            case KBuiltin::LaunchId: s.append("gl_LaunchIDEXT"); break;
            case KBuiltin::LaunchSize: s.append("gl_LaunchSizeEXT"); break;
            case KBuiltin::HitT: s.append("gl_HitTEXT"); break;
            case KBuiltin::PrimitiveId: s.append("gl_PrimitiveID"); break;
            case KBuiltin::InstanceId: s.append("gl_InstanceID"); break;
            case KBuiltin::InstanceCustomIndex: s.append("gl_InstanceCustomIndexEXT"); break;
            case KBuiltin::HitBary: s.append("hattr"); break; // P4: the barycentric hitAttribute (vec2 u,v)
            default: s.append("0u"); break;
            }
            break;
        case KOp::VecComp: self(self, nd.a); s.append("."); { const char sw[2] = {xyzw[nd.iidx], '\0'}; s.append(sw); } break;
        case KOp::PayloadLoad: s.append("pl.m"); app_uint(s, nd.iidx); break;
        case KOp::BufferLoad: s.append("buf"); app_uint(s, g.node(nd.a).iidx); s.append("["); self(self, nd.b); s.append("]"); break;
        case KOp::Cast: s.append(ctype(nd.dtype())); s.append("("); self(self, nd.a); s.append(")"); break;
        case KOp::Neg: s.append("(-"); self(self, nd.a); s.append(")"); break;
        case KOp::Add: bin(" + "); break;
        case KOp::Sub: bin(" - "); break;
        case KOp::Mul: bin(" * "); break;
        case KOp::Div: bin(" / "); break;
        case KOp::CmpLt: bin(" < "); break;
        case KOp::CmpLe: bin(" <= "); break;
        case KOp::CmpGt: bin(" > "); break;
        case KOp::CmpGe: bin(" >= "); break;
        case KOp::CmpEq: bin(" == "); break;
        case KOp::CmpNe: bin(" != "); break;
        case KOp::Max: s.append("max("); self(self, nd.a); s.append(", "); self(self, nd.b); s.append(")"); break;
        case KOp::Min: s.append("min("); self(self, nd.a); s.append(", "); self(self, nd.b); s.append(")"); break;
        default: s.append("0.0"); break;
        }
    };
    const auto vv = [&](int node) { pv(pv, node); };

    for (int i = 0; i < bn; ++i) // emit the body statements
    {
        const KStmt& stm = g.stmt(b0 + i);
        switch (stm.kind)
        {
        case KStmtKind::TraceRayPipeline:
        {
            const auto ex = [&](int k) { return g.stmt_ext_operand(stm, k); };
            s.append("  ");
            if (use_ser)
            {
                s.append("hitObjectNV hobj; hitObjectTraceRayNV(hobj, as"); app_uint(s, g.node(stm.target).iidx);
                s.append(", gl_RayFlagsNoneEXT, 0xFFu, 0u, 0u, 0u, vec3("); vv(ex(0)); s.append(", "); vv(ex(1)); s.append(", "); vv(ex(2)); s.append("), "); vv(ex(6));
                s.append(", vec3("); vv(ex(3)); s.append(", "); vv(ex(4)); s.append(", "); vv(ex(5)); s.append("), "); vv(ex(7)); s.append(", 0);\n");
                s.append("  reorderThreadNV(hobj);\n  hitObjectExecuteShaderNV(hobj, 0);\n");
            }
            else
            {
                s.append("traceRayEXT(as"); app_uint(s, g.node(stm.target).iidx);
                s.append(", gl_RayFlagsNoneEXT, 0xFFu, 0u, 0u, 0u, vec3("); vv(ex(0)); s.append(", "); vv(ex(1)); s.append(", "); vv(ex(2)); s.append("), "); vv(ex(6));
                s.append(", vec3("); vv(ex(3)); s.append(", "); vv(ex(4)); s.append(", "); vv(ex(5)); s.append("), "); vv(ex(7)); s.append(", 0);\n");
            }
            break;
        }
        case KStmtKind::PayloadStore: s.append("  pl.m"); app_uint(s, stm.index); s.append(" = "); vv(stm.value); s.append(";\n"); break;
        case KStmtKind::BufferStore: s.append("  buf"); app_uint(s, g.node(stm.target).iidx); s.append("["); vv(stm.index); s.append("] = "); vv(stm.value); s.append(";\n"); break;
        case KStmtKind::IgnoreHitIf: s.append("  if ("); vv(stm.value); s.append(") { ignoreIntersectionEXT; }\n"); break; // P4 any-hit alpha
        // ReorderThread is absorbed into the trace when use_ser, and is a perf-only no-op otherwise — same as default.
        default: break;
        }
    }
    s.append("}\n");
    return true;
}

// B3-c: emit a VERTEX or FRAGMENT GLSL shader from a stage `entry` (reached through `create_program(KGraph, KEntry)` — the
// gpu-context program seam, never crd-shader). It reuses `emit_value_stmt` for the ~60 value-op cases (no duplication with
// the compute path) and adds the raster PROLOGUE (`layout(location)` in/out, `layout(set,binding,std140) uniform` UBO,
// per-stage builtins) + EPILOGUE (`gl_Position` / colour attachments / `gl_FragDepth`). The RASTER LEAF resolves stage
// values: `StageIn`→`a_L`, `Builtin`→`gl_*`, `UniformBlock` member `FieldGet`→`ubo_S_B.fN`. A buffer `Input` or an
// unlowerable builtin returns false. Assumes the entry is `entry_valid` (the seam checks it first).
inline bool emit_stage_glsl(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    if (entry.stage != KStage::Vertex && entry.stage != KStage::Fragment) { return false; } // B3-c lowers VS/FS only
    const bool is_vertex = (entry.stage == KStage::Vertex);
    if (is_vertex && entry.position < 0) { return false; } // a vertex entry must write clip position

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
    s.append("#version 450\n");
    if (is_vertex && entry.shading_rate >= 0) { s.append("#extension GL_EXT_fragment_shading_rate : require\n"); } // B1-e
    for (int i = 0; i < n; ++i) // B2-d: a bindless SampleIndexed needs the nonuniform-indexing qualifier
    {
        if (reach[static_cast<crd::usize>(i)] && (g.node(i).op == KOp::SampleIndexed || g.node(i).op == KOp::SampleIndexedLod))
        {
            s.append("#extension GL_EXT_nonuniform_qualifier : require\n");
            break;
        }
    }
    if (!is_vertex) // B1-f: gl_FragFullyCoveredNV (InnerCoverage) needs the NV conservative-raster underestimation extension
    {
        for (int i = 0; i < n; ++i)
        {
            if (reach[static_cast<crd::usize>(i)] && g.node(i).op == KOp::Builtin
                && static_cast<KBuiltin>(g.node(i).iidx) == KBuiltin::InnerCoverage)
            {
                s.append("#extension GL_NV_conservative_raster_underestimation : require\n");
                break;
            }
        }
    }
    // B1-f (FS) + GEO-1 (VS vertex pulling): does this stage touch the storage buffer? The FS may read AND write (+ROV);
    // a VERTEX stage may READ (StorageLoad by VertexIndex — the bindless vertex-feeding path). Writes stay FS-only.
    bool fs_uses_storage = false;
    if (!is_vertex && entry.storage_write_index >= 0) { fs_uses_storage = true; }
    for (int i = 0; !fs_uses_storage && i < n; ++i)
    {
        if (reach[static_cast<crd::usize>(i)] && g.node(i).op == KOp::StorageLoad) { fs_uses_storage = true; }
    }
    // Rasterizer-ordered access (ROV) — the whole main() body serialises per pixel between begin/endInvocationInterlockARB.
    if (!is_vertex && fs_uses_storage && entry.interlock) { s.append("#extension GL_ARB_fragment_shader_interlock : require\n"); }
    for (int i = 0; i < n; ++i) // stage inputs: StageIn at (location) — VS attribute / FS interpolant
    {
        if (!reach[static_cast<crd::usize>(i)] || g.node(i).op != KOp::StageIn) { continue; }
        const KNode& nd = g.node(i);
        s.append("layout(location = "); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(") ");
        if (!is_vertex) { s.append(glsl_interp(static_cast<Interp>(nd.dset))); } // B1-c: interp on FS interpolant inputs
        s.append("in "); s.append(vtype(nd.type)); s.append(" a_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(";\n");
    }
    for (int k = 0; k < entry.n_out; ++k) // stage outputs: VS interpolants / FS colour attachments, at (location)
    {
        const int nid = entry.out[k].node;
        if (nid < 0) { continue; }
        s.append("layout(location = "); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(") ");
        if (is_vertex) { s.append(glsl_interp(entry.out[k].interp)); } // B1-c: interp on VS interpolant outputs (matches FS)
        s.append("out "); s.append(vtype(g.node(nid).type)); s.append(" o_"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(";\n");
    }
    if (!is_vertex && entry.early_fragment_tests) { s.append("layout(early_fragment_tests) in;\n"); } // B1-d: force early-Z
    if (!is_vertex && entry.frag_depth >= 0 && entry.depth_mode != DepthMode::Any) // B1-d: conservative depth on gl_FragDepth
    {
        s.append("layout(depth_"); s.append(entry.depth_mode == DepthMode::Greater ? "greater" : "less");
        s.append(") out float gl_FragDepth;\n");
    }
    if (fs_uses_storage) // B1-f: the storage buffer at set 0 / binding 0 (matches draw_storage's descriptor). FS: `coherent`
    {                    // (writes visible cross-invocation) + `pixel_interlock_ordered` when ROV. VS (GEO-1 vertex
                         // pulling): READONLY — the vertex stage only ever fetches.
        if (!is_vertex && entry.interlock) { s.append("layout(pixel_interlock_ordered) in;\n"); }
        s.append(is_vertex ? "layout(set = 0, binding = 0, std430) readonly buffer StorageBuf { uint data[]; } sbuf;\n"
                           : "layout(set = 0, binding = 0, std430) coherent buffer StorageBuf { uint data[]; } sbuf;\n");
    }
    for (int i = 0; i < n; ++i) // B2: separable texture + sampler bindings — `uniform texture2D tex_S_B` / `uniform sampler samp_S_B`
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        if (nd.op == KOp::Texture)
        {
            s.append("layout(set = "); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(", binding = "); app_uint(s, static_cast<crd::u32>(nd.iidx));
            s.append(") uniform "); s.append(glsl_tex_scalar_prefix(nd.type.scalar)); s.append("texture"); s.append(glsl_tex_dim_suffix(nd.type));
            s.append(" tex_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx));
            if (nd.type.count > 1U) { s.append("["); app_uint(s, static_cast<crd::u32>(nd.type.count)); s.append("]"); } // B2-d: bindless array
            s.append(";\n");
        }
        else if (nd.op == KOp::Sampler)
        {
            s.append("layout(set = "); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(", binding = "); app_uint(s, static_cast<crd::u32>(nd.iidx));
            s.append(") uniform sampler"); if (nd.type.tex_shadow()) { s.append("Shadow"); } // B2-b: comparison sampler
            s.append(" samp_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(";\n");
        }
    }
    for (int i = 0; i < n; ++i) // uniform blocks: UniformBlock at (set = ADR-0102 frequency slot, binding), std140 members
    {
        if (!reach[static_cast<crd::usize>(i)] || g.node(i).op != KOp::UniformBlock) { continue; }
        const KNode& nd  = g.node(i);
        const int    sid = nd.type.struct_id;
        s.append("layout(set = "); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(", binding = "); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", std140) uniform U_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(" {\n");
        const int fc = g.struct_field_count(sid);
        for (int f = 0; f < fc; ++f) { s.append("  "); s.append(vtype(g.struct_field(sid, f))); s.append(" f"); app_uint(s, static_cast<crd::u32>(f)); s.append(";\n"); }
        s.append("} ubo_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(";\n");
    }
    { // D12: specialization constants — module-scope `layout(constant_id=N) const t _specN = default;` (pipeline-time overridable)
        bool spec_declared[256] = {};
        for (int i = 0; i < n; ++i)
        {
            if (!reach[static_cast<crd::usize>(i)]) { continue; }
            const KNode& nd = g.node(i);
            if (!is_spec_const(nd)) { continue; }
            const crd::u32 id = spec_const_id(nd);
            if (id >= 256U || spec_declared[id]) { continue; }
            spec_declared[id] = true;
            s.append("layout(constant_id = "); app_uint(s, static_cast<crd::u32>(id)); s.append(") const ");
            const char* spec_ctype = "float";
            if (nd.dtype() == DType::Bool) { spec_ctype = "bool"; }
            else if (dt_is_uint(nd.dtype())) { spec_ctype = "uint"; }
            else if (dt_is_int(nd.dtype())) { spec_ctype = "int"; }
            s.append(spec_ctype);
            s.append(" _spec"); app_uint(s, static_cast<crd::u32>(id)); s.append(" = ");
            if (nd.dtype() == DType::Bool) { s.append(nd.cval != 0.0 ? "true" : "false"); }
            else if (dt_is_uint(nd.dtype()) || dt_is_int(nd.dtype())) { app_int_const(s, nd.cval, nd.dtype()); }
            else { app_flit(s, nd.cval); }
            s.append(";\n");
        }
    }
    { // quaternion/slerp helper functions (no GLSL builtins) — emitted once when the graph uses them (shared with compute)
        bool qm = false;
        bool qc = false;
        bool qr = false;
        bool qa = false;
        bool qt = false;
        bool sl = false;
        for (int i = 0; i < n; ++i) { if (!reach[static_cast<crd::usize>(i)]) { continue; } switch (g.node(i).op) { case KOp::QuatMul: qm = true; break; case KOp::QuatConj: qc = true; break; case KOp::QuatRotate: qr = true; break; case KOp::QuatAxisAngle: qa = true; break; case KOp::QuatToMat3: qt = true; break; case KOp::Slerp: sl = true; break; default: break; } }
        if (qm) { s.append("vec4 crd_qmul(vec4 a,vec4 b){return vec4(a.w*b.xyz+b.w*a.xyz+cross(a.xyz,b.xyz),a.w*b.w-dot(a.xyz,b.xyz));}\n"); }
        if (qc) { s.append("vec4 crd_qconj(vec4 q){return vec4(-q.xyz,q.w);}\n"); }
        if (qr) { s.append("vec3 crd_qrot(vec4 q,vec3 v){vec3 t=2.0*cross(q.xyz,v);return v+q.w*t+cross(q.xyz,t);}\n"); }
        if (qa) { s.append("vec4 crd_qaa(vec3 ax,float an){float h=an*0.5;return vec4(ax*sin(h),cos(h));}\n"); }
        if (qt) { s.append("mat3 crd_qmat(vec4 q){float x=q.x,y=q.y,z=q.z,w=q.w;return mat3(1.0-2.0*(y*y+z*z),2.0*(x*y+w*z),2.0*(x*z-w*y),2.0*(x*y-w*z),1.0-2.0*(x*x+z*z),2.0*(y*z+w*x),2.0*(x*z+w*y),2.0*(y*z-w*x),1.0-2.0*(x*x+y*y));}\n"); }
        if (sl) { s.append("vec4 crd_slerp(vec4 a,vec4 b,float t){float d=dot(a,b);float sg=1.0;if(d<0.0){d=-d;sg=-1.0;}if(d>0.9995){return normalize(mix(a,sg*b,t));}float th=acos(d);float sn=sin(th);return (sin((1.0-t)*th)*a+sin(t*th)*sg*b)/sn;}\n"); }
    }
    s.append("void main() {\n");
    if (!is_vertex && entry.interlock) { s.append("  beginInvocationInterlockARB();\n"); } // B1-f: rasterizer-ordered access

    // A4 tier-2 body-scoping (shared with the compute path): loop-varying nodes emit INSIDE their owning `for`.
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
        if (lnd.op == KOp::UniformBlock) { return true; } // the block materializes nothing; only its FieldGets read members
        if (lnd.op == KOp::Texture || lnd.op == KOp::Sampler) { return true; } // B2: opaque binding leaves — declared in the prologue
        if (lnd.op == KOp::StageIn) { emit_stmt_prefix(gg, li, ss); ss.append("a_"); app_uint(ss, static_cast<crd::u32>(lnd.iidx)); ss.append(";\n"); return true; }
        if (lnd.op == KOp::Builtin)
        {
            const KBuiltin bi = static_cast<KBuiltin>(lnd.iidx);
            const char*    bn = glsl_vsfs_builtin_name(bi);
            if (bn == nullptr) { return false; } // a builtin this VS/FS emitter cannot lower -> refuse loudly
            emit_stmt_prefix(gg, li, ss);
            // B1-f: gl_FragFullyCoveredNV is a bool; the IR builtin is uint ⇒ convert on read.
            if (bi == KBuiltin::InnerCoverage) { ss.append("uint("); ss.append(bn); ss.append(")"); }
            else { ss.append(bn); }
            ss.append(";\n");
            return true;
        }
        if (lnd.op == KOp::FieldGet) // a UBO member read (agg is a UniformBlock; the SROA-on-Make case is handled in emit_value_stmt)
        {
            const KNode& agg = gg.node(lnd.a);
            if (agg.op != KOp::UniformBlock) { return false; }
            emit_stmt_prefix(gg, li, ss); ss.append("ubo_"); app_uint(ss, static_cast<crd::u32>(agg.dset)); ss.append("_"); app_uint(ss, static_cast<crd::u32>(agg.iidx)); ss.append(".f"); app_uint(ss, static_cast<crd::u32>(lnd.iidx)); ss.append(";\n");
            return true;
        }
        return false; // `Input` (a compute storage buffer) is invalid in raster; everything else -> the shared switch
    };

    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)] || varying[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        if (nd.op == KOp::For)
        {
            s.append("  precise "); s.append(vtype(nd.type)); s.append(" t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nd.b)); s.append(";\n");
            s.append("  for (int li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = 0; li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(" < int(t"); app_uint(s, static_cast<crd::u32>(nd.a)); s.append("); li_"); app_uint(s, static_cast<crd::u32>(i)); s.append("++) {\n");
            for (int bid = 0; bid < i; ++bid)
            {
                if (body_of[static_cast<crd::usize>(bid)] != i) { continue; }
                const KNode& bn = g.node(bid);
                if (bn.op == KOp::LoopIndex) { s.append("  precise float t"); app_uint(s, static_cast<crd::u32>(bid)); s.append(" = float(li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(");\n"); }
                else if (bn.op == KOp::LoopAcc) { s.append("  precise "); s.append(vtype(bn.type)); s.append(" t"); app_uint(s, static_cast<crd::u32>(bid)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(i)); s.append(";\n"); }
                else if (!emit_value_stmt(g, bid, s, raster_leaf)) { return false; }
            }
            s.append("  t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nd.c)); s.append(";\n  }\n");
        }
        else if (!emit_value_stmt(g, i, s, raster_leaf)) { return false; }
    }

    if (!is_vertex && entry.storage_write_index >= 0) // B1-f: the storage-buffer write (inside the interlock region)
    {
        s.append("  sbuf.data[t"); app_uint(s, static_cast<crd::u32>(entry.storage_write_index));
        s.append("] = t"); app_uint(s, static_cast<crd::u32>(entry.storage_write_value)); s.append(";\n");
    }
    if (!is_vertex && entry.interlock) { s.append("  endInvocationInterlockARB();\n"); } // B1-f

    if (!is_vertex && entry.discard_cond >= 0) // B1-b: alpha-test / cutout — kill the fragment before writing outputs
    {
        s.append("  if (t"); app_uint(s, static_cast<crd::u32>(entry.discard_cond)); s.append(") { discard; }\n");
    }
    if (is_vertex) { s.append("  gl_Position = t"); app_uint(s, static_cast<crd::u32>(entry.position)); s.append(";\n"); }
    if (is_vertex && entry.shading_rate >= 0) { s.append("  gl_PrimitiveShadingRateEXT = t"); app_uint(s, static_cast<crd::u32>(entry.shading_rate)); s.append(";\n"); } // B1-e
    for (int k = 0; k < entry.n_out; ++k) { const int nid = entry.out[k].node; if (nid < 0) { continue; } s.append("  o_"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nid)); s.append(";\n"); }
    if (!is_vertex && entry.frag_depth >= 0) { s.append("  gl_FragDepth = t"); app_uint(s, static_cast<crd::u32>(entry.frag_depth)); s.append(";\n"); }
    s.append("}\n");
    return true;
}

// B4: TASK / AMPLIFICATION emit (GL_EXT_mesh_shader). A task workgroup computes `task_emit` = the MESH-workgroup count to
// launch (GPU-driven amplification) + an optional single-uint `task_payload`, writes the payload, then `EmitMeshTasksEXT`.
// The value graphs read the workgroup builtins (WorkgroupId / LocalInvocationIndex / uniforms) via the shared value machinery.
// No geometry (the mesh emits that). The amplification count is a scalar expression; a loop-bearing task is refused (extend
// with the mesh emitter's body-scoping if ever needed).
inline bool emit_task_glsl(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    if (entry.stage != KStage::Task || entry.task_emit < 0) { return false; }

    const int                       n = g.size();
    crd::containers::Array<crd::u8> reach(scratch);
    crd::containers::Array<int>     stk(scratch);
    reach.resize(static_cast<crd::usize>(n), 0);
    stk.push_back(entry.task_emit);
    for (crd::u32 pf = 0; pf < entry.n_task_payload; ++pf) { stk.push_back(entry.task_payload[pf]); }
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
    s.append("#version 460\n#extension GL_EXT_mesh_shader : require\n");
    const crd::u32 ls = entry.local_size[0] > 0U ? entry.local_size[0] : 1U;
    s.append("layout(local_size_x = "); app_uint(s, ls); s.append(") in;\n");
    for (int i = 0; i < n; ++i) // uniform blocks (a task may read uniforms to compute the count)
    {
        if (!reach[static_cast<crd::usize>(i)] || g.node(i).op != KOp::UniformBlock) { continue; }
        const KNode& nd  = g.node(i);
        const int    sid = nd.type.struct_id;
        s.append("layout(set = "); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(", binding = "); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", std140) uniform U_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(" {\n");
        const int fc = g.struct_field_count(sid);
        for (int f = 0; f < fc; ++f) { s.append("  "); s.append(vtype(g.struct_field(sid, f))); s.append(" f"); app_uint(s, static_cast<crd::u32>(f)); s.append(";\n"); }
        s.append("} ubo_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(";\n");
    }
    if (entry.n_task_payload > 0U) { s.append("struct TaskPayload { uint v0; uint v1; uint v2; uint v3; };\ntaskPayloadSharedEXT TaskPayload mesh_payload;\n"); } // B4: FIXED 4-field payload (task + mesh layouts always match)

    s.append("void main() {\n");
    const auto task_leaf = [&](const KGraph& gg, int li, crd::containers::String& ss) -> bool
    {
        const KNode& lnd = gg.node(li);
        if (lnd.op == KOp::UniformBlock) { return true; }
        if (lnd.op == KOp::Builtin)
        {
            const KBuiltin bi = static_cast<KBuiltin>(lnd.iidx);
            const char*    bn = glsl_vsfs_builtin_name(bi);
            if (bn == nullptr) { return false; }
            emit_stmt_prefix(gg, li, ss); ss.append(bn); ss.append(";\n");
            return true;
        }
        if (lnd.op == KOp::FieldGet)
        {
            const KNode& agg = gg.node(lnd.a);
            if (agg.op != KOp::UniformBlock) { return false; }
            emit_stmt_prefix(gg, li, ss); ss.append("ubo_"); app_uint(ss, static_cast<crd::u32>(agg.dset)); ss.append("_"); app_uint(ss, static_cast<crd::u32>(agg.iidx)); ss.append(".f"); app_uint(ss, static_cast<crd::u32>(lnd.iidx)); ss.append(";\n");
            return true;
        }
        return false;
    };
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        if (g.node(i).op == KOp::For) { return false; } // a task's amplification count is a scalar expr — no loops
        if (!emit_value_stmt(g, i, s, task_leaf)) { return false; }
    }
    for (crd::u32 pf = 0; pf < entry.n_task_payload; ++pf) // write each active payload field
    {
        s.append("  mesh_payload.v"); app_uint(s, pf); s.append(" = t"); app_uint(s, static_cast<crd::u32>(entry.task_payload[pf])); s.append(";\n");
    }
    s.append("  EmitMeshTasksEXT(t"); app_uint(s, static_cast<crd::u32>(entry.task_emit)); s.append(", 1u, 1u);\n}\n");
    return true;
}

// B4: MESH-shader emit (GL_EXT_mesh_shader). One workgroup emits up to mesh_vertices verts + mesh_primitives triangles; thread
// tid writes vertex tid (position + per-vertex out arrays) and primitive tid (mesh_prim → the local triangle indices), each
// guarded by its count. The per-vertex value graph reads the workgroup builtins (a global vertex id built from WorkgroupIndex +
// LocalInvocationIndex), so it reuses the raster value machinery + the raster_leaf. No StageIn (a mesh stage has no vertex
// attribute inputs — it GENERATES geometry).
inline bool emit_mesh_glsl(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    if (entry.stage != KStage::Mesh || entry.mesh_vertices == 0U || entry.position < 0 || entry.mesh_prim < 0) { return false; }
    const crd::u32 n_verts    = entry.mesh_vertices;
    const crd::u32 n_prims    = entry.mesh_primitives;
    const crd::u32 local_size = n_verts > n_prims ? n_verts : n_prims; // one thread per output slot; write guarded by count

    const int                       n = g.size();
    crd::containers::Array<crd::u8> reach(scratch);
    crd::containers::Array<int>     stk(scratch);
    reach.resize(static_cast<crd::usize>(n), 0);
    const auto push_root = [&](int r) { if (r >= 0) { stk.push_back(r); } };
    push_root(entry.position);
    push_root(entry.mesh_prim);
    push_root(entry.shading_rate); // B4: per-primitive VRS rate from the mesh (gl_MeshPrimitivesEXT[].gl_PrimitiveShadingRateEXT)
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
    s.append("#version 460\n#extension GL_EXT_mesh_shader : require\n");
    if (entry.shading_rate >= 0) { s.append("#extension GL_EXT_fragment_shading_rate : require\n"); } // B4: per-primitive VRS
    for (int i = 0; i < n; ++i)
    {
        if (reach[static_cast<crd::usize>(i)] && (g.node(i).op == KOp::SampleIndexed || g.node(i).op == KOp::SampleIndexedLod))
        {
            s.append("#extension GL_EXT_nonuniform_qualifier : require\n");
            break;
        }
    }
    s.append("layout(local_size_x = "); app_uint(s, local_size); s.append(") in;\n");
    s.append("layout(triangles, max_vertices = "); app_uint(s, n_verts); s.append(", max_primitives = "); app_uint(s, n_prims); s.append(") out;\n");
    for (int k = 0; k < entry.n_out; ++k) // per-VERTEX output arrays (unsized ⇒ max_vertices)
    {
        const int nid = entry.out[k].node;
        if (nid < 0) { continue; }
        s.append("layout(location = "); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(") ");
        s.append(glsl_interp(entry.out[k].interp));
        s.append("out "); s.append(vtype(g.node(nid).type)); s.append(" o_"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append("[];\n");
    }
    for (int i = 0; i < n; ++i) // B2: texture + sampler binding leaves (same as the raster prologue)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        if (nd.op == KOp::Texture)
        {
            s.append("layout(set = "); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(", binding = "); app_uint(s, static_cast<crd::u32>(nd.iidx));
            s.append(") uniform "); s.append(glsl_tex_scalar_prefix(nd.type.scalar)); s.append("texture"); s.append(glsl_tex_dim_suffix(nd.type));
            s.append(" tex_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx));
            if (nd.type.count > 1U) { s.append("["); app_uint(s, static_cast<crd::u32>(nd.type.count)); s.append("]"); }
            s.append(";\n");
        }
        else if (nd.op == KOp::Sampler)
        {
            s.append("layout(set = "); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(", binding = "); app_uint(s, static_cast<crd::u32>(nd.iidx));
            s.append(") uniform sampler"); if (nd.type.tex_shadow()) { s.append("Shadow"); }
            s.append(" samp_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(";\n");
        }
    }
    for (int i = 0; i < n; ++i) // uniform blocks (same as raster)
    {
        if (!reach[static_cast<crd::usize>(i)] || g.node(i).op != KOp::UniformBlock) { continue; }
        const KNode& nd  = g.node(i);
        const int    sid = nd.type.struct_id;
        s.append("layout(set = "); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(", binding = "); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", std140) uniform U_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(" {\n");
        const int fc = g.struct_field_count(sid);
        for (int f = 0; f < fc; ++f) { s.append("  "); s.append(vtype(g.struct_field(sid, f))); s.append(" f"); app_uint(s, static_cast<crd::u32>(f)); s.append(";\n"); }
        s.append("} ubo_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(";\n");
    }
    for (int i = 0; i < n; ++i) // B4: declare the task→mesh payload iff this mesh reads ANY payload field (v0..v3)
    {
        const bool is_payload = g.node(i).op == KOp::Builtin
                                && (static_cast<KBuiltin>(g.node(i).iidx) == KBuiltin::TaskPayload
                                    || static_cast<KBuiltin>(g.node(i).iidx) == KBuiltin::TaskPayload1
                                    || static_cast<KBuiltin>(g.node(i).iidx) == KBuiltin::TaskPayload2
                                    || static_cast<KBuiltin>(g.node(i).iidx) == KBuiltin::TaskPayload3);
        if (reach[static_cast<crd::usize>(i)] && is_payload)
        {
            s.append("struct TaskPayload { uint v0; uint v1; uint v2; uint v3; };\ntaskPayloadSharedEXT TaskPayload mesh_payload;\n"); // FIXED 4-field (matches the task)
            break;
        }
    }

    s.append("void main() {\n");
    s.append("  SetMeshOutputsEXT("); app_uint(s, n_verts); s.append("u, "); app_uint(s, n_prims); s.append("u);\n");

    // A4 tier-2 body-scoping (identical to the raster path): loop-varying nodes emit INSIDE their owning `for`.
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
        if (lnd.op == KOp::UniformBlock) { return true; }
        if (lnd.op == KOp::Texture || lnd.op == KOp::Sampler) { return true; }
        if (lnd.op == KOp::Builtin)
        {
            const KBuiltin bi = static_cast<KBuiltin>(lnd.iidx);
            const char*    bn = glsl_vsfs_builtin_name(bi);
            if (bn == nullptr) { return false; }
            emit_stmt_prefix(gg, li, ss); ss.append(bn); ss.append(";\n");
            return true;
        }
        if (lnd.op == KOp::FieldGet)
        {
            const KNode& agg = gg.node(lnd.a);
            if (agg.op != KOp::UniformBlock) { return false; }
            emit_stmt_prefix(gg, li, ss); ss.append("ubo_"); app_uint(ss, static_cast<crd::u32>(agg.dset)); ss.append("_"); app_uint(ss, static_cast<crd::u32>(agg.iidx)); ss.append(".f"); app_uint(ss, static_cast<crd::u32>(lnd.iidx)); ss.append(";\n");
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
            s.append("  precise "); s.append(vtype(nd.type)); s.append(" t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nd.b)); s.append(";\n");
            s.append("  for (int li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = 0; li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(" < int(t"); app_uint(s, static_cast<crd::u32>(nd.a)); s.append("); li_"); app_uint(s, static_cast<crd::u32>(i)); s.append("++) {\n");
            for (int bid = 0; bid < i; ++bid)
            {
                if (body_of[static_cast<crd::usize>(bid)] != i) { continue; }
                const KNode& bn = g.node(bid);
                if (bn.op == KOp::LoopIndex) { s.append("  precise float t"); app_uint(s, static_cast<crd::u32>(bid)); s.append(" = float(li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(");\n"); }
                else if (bn.op == KOp::LoopAcc) { s.append("  precise "); s.append(vtype(bn.type)); s.append(" t"); app_uint(s, static_cast<crd::u32>(bid)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(i)); s.append(";\n"); }
                else if (!emit_value_stmt(g, bid, s, raster_leaf)) { return false; }
            }
            s.append("  t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nd.c)); s.append(";\n  }\n");
        }
        else if (!emit_value_stmt(g, i, s, raster_leaf)) { return false; }
    }

    // per-vertex writes (guarded by max_vertices), then the per-primitive triangle indices (guarded by max_primitives).
    s.append("  if (gl_LocalInvocationIndex < "); app_uint(s, n_verts); s.append("u) {\n");
    s.append("    gl_MeshVerticesEXT[gl_LocalInvocationIndex].gl_Position = t"); app_uint(s, static_cast<crd::u32>(entry.position)); s.append(";\n");
    for (int k = 0; k < entry.n_out; ++k) { const int nid = entry.out[k].node; if (nid < 0) { continue; } s.append("    o_"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append("[gl_LocalInvocationIndex] = t"); app_uint(s, static_cast<crd::u32>(nid)); s.append(";\n"); }
    s.append("  }\n");
    s.append("  if (gl_LocalInvocationIndex < "); app_uint(s, n_prims); s.append("u) {\n");
    s.append("    gl_PrimitiveTriangleIndicesEXT[gl_LocalInvocationIndex] = t"); app_uint(s, static_cast<crd::u32>(entry.mesh_prim)); s.append(";\n");
    if (entry.shading_rate >= 0) { s.append("    gl_MeshPrimitivesEXT[gl_LocalInvocationIndex].gl_PrimitiveShadingRateEXT = t"); app_uint(s, static_cast<crd::u32>(entry.shading_rate)); s.append(";\n"); } // B4: per-primitive VRS
    s.append("  }\n}\n");
    return true;
}

// B4-tess: TESS-CONTROL (hull) emit — a passthrough quad-patch hull. Copies each control point (gl_in→gl_out) and sets the
// inner/outer tess levels from `tess_inner`/`tess_outer` (float value graphs, computed once by invocation 0). The VS output
// feeds gl_in; the TessEval stage does the domain eval. No StageIn / geometry of its own.
inline bool emit_tesc_glsl(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    if (entry.stage != KStage::TessControl || entry.tess_patch_size == 0U || entry.tess_inner < 0 || entry.tess_outer < 0)
    {
        return false;
    }
    const int                       n = g.size();
    crd::containers::Array<crd::u8> reach(scratch);
    crd::containers::Array<int>     stk(scratch);
    reach.resize(static_cast<crd::usize>(n), 0);
    stk.push_back(entry.tess_inner);
    stk.push_back(entry.tess_outer);
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
    s.append("#version 460\nlayout(vertices = "); app_uint(s, entry.tess_patch_size); s.append(") out;\n");
    s.append("void main() {\n");
    s.append("  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"); // passthrough control points
    const auto leaf = [&](const KGraph& gg, int li, crd::containers::String& ss) -> bool
    {
        const KNode& lnd = gg.node(li);
        if (lnd.op == KOp::Builtin)
        {
            const char* bn = glsl_vsfs_builtin_name(static_cast<KBuiltin>(lnd.iidx));
            if (bn == nullptr) { return false; }
            emit_stmt_prefix(gg, li, ss); ss.append(bn); ss.append(";\n");
            return true;
        }
        return false;
    };
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        if (g.node(i).op == KOp::For) { return false; }
        if (!emit_value_stmt(g, i, s, leaf)) { return false; }
    }
    s.append("  if (gl_InvocationID == 0) {\n");
    s.append("    gl_TessLevelInner[0] = t"); app_uint(s, static_cast<crd::u32>(entry.tess_inner)); s.append("; gl_TessLevelInner[1] = t"); app_uint(s, static_cast<crd::u32>(entry.tess_inner)); s.append(";\n");
    s.append("    gl_TessLevelOuter[0] = t"); app_uint(s, static_cast<crd::u32>(entry.tess_outer)); s.append("; gl_TessLevelOuter[1] = t"); app_uint(s, static_cast<crd::u32>(entry.tess_outer)); s.append(";\n");
    s.append("    gl_TessLevelOuter[2] = t"); app_uint(s, static_cast<crd::u32>(entry.tess_outer)); s.append("; gl_TessLevelOuter[3] = t"); app_uint(s, static_cast<crd::u32>(entry.tess_outer)); s.append(";\n");
    s.append("  }\n}\n");
    return true;
}

// B4-tess: TESS-EVAL (domain) emit — a quad domain. The emitter provides `patch_pos` = the bilinear interpolation of the 4
// control-point positions by gl_TessCoord.xy (KBuiltin::TessPatchPosition); the value graph adds a displacement (reading
// TessCoord + uniforms) and writes the clip `position` + `out[]` interpolants. The portable heightfield / ocean grid path.
inline bool emit_tese_glsl(const KGraph& g, const KEntry& entry, crd::memory::IAllocator* scratch, GlslKernel& out)
{
    using namespace glsl_detail;
    if (entry.stage != KStage::TessEval || entry.tess_patch_size == 0U || entry.position < 0) { return false; }

    const int                       n = g.size();
    crd::containers::Array<crd::u8> reach(scratch);
    crd::containers::Array<int>     stk(scratch);
    reach.resize(static_cast<crd::usize>(n), 0);
    const auto push_root = [&](int r) { if (r >= 0) { stk.push_back(r); } };
    push_root(entry.position);
    for (int k = 0; k < entry.n_out; ++k) { push_root(entry.out[k].node); }
    bool needs_patch = false;
    while (stk.size() > 0)
    {
        const int i = stk[stk.size() - 1];
        stk.resize(stk.size() - 1);
        if (i < 0 || reach[static_cast<crd::usize>(i)]) { continue; }
        reach[static_cast<crd::usize>(i)] = 1;
        const KNode& nd = g.node(i);
        if (nd.op == KOp::Builtin && static_cast<KBuiltin>(nd.iidx) == KBuiltin::TessPatchPosition) { needs_patch = true; }
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
        if (nd.d >= 0) { stk.push_back(nd.d); }
        for (int e = 0; e < static_cast<int>(nd.n_ext); ++e) { stk.push_back(g.ext_operand(nd, e)); }
    }

    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 460\nlayout(quads, equal_spacing, ccw) in;\n");
    for (int k = 0; k < entry.n_out; ++k) // per-vertex interpolants to the FS
    {
        const int nid = entry.out[k].node;
        if (nid < 0) { continue; }
        s.append("layout(location = "); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(") ");
        s.append(glsl_interp(entry.out[k].interp)); s.append("out "); s.append(vtype(g.node(nid).type)); s.append(" o_"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(";\n");
    }
    for (int i = 0; i < n; ++i) // uniform blocks (a displacement may read a uniform amplitude/time)
    {
        if (!reach[static_cast<crd::usize>(i)] || g.node(i).op != KOp::UniformBlock) { continue; }
        const KNode& nd  = g.node(i);
        const int    sid = nd.type.struct_id;
        s.append("layout(set = "); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append(", binding = "); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(", std140) uniform U_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(" {\n");
        const int fc = g.struct_field_count(sid);
        for (int f = 0; f < fc; ++f) { s.append("  "); s.append(vtype(g.struct_field(sid, f))); s.append(" f"); app_uint(s, static_cast<crd::u32>(f)); s.append(";\n"); }
        s.append("} ubo_"); app_uint(s, static_cast<crd::u32>(nd.dset)); s.append("_"); app_uint(s, static_cast<crd::u32>(nd.iidx)); s.append(";\n");
    }

    s.append("void main() {\n");
    if (needs_patch)
    {
        s.append("  vec4 patch_pos = mix(mix(gl_in[0].gl_Position, gl_in[1].gl_Position, gl_TessCoord.x), "
                 "mix(gl_in[3].gl_Position, gl_in[2].gl_Position, gl_TessCoord.x), gl_TessCoord.y);\n");
    }
    const auto tese_leaf = [&](const KGraph& gg, int li, crd::containers::String& ss) -> bool
    {
        const KNode& lnd = gg.node(li);
        if (lnd.op == KOp::UniformBlock) { return true; }
        if (lnd.op == KOp::Builtin)
        {
            const char* bn = glsl_vsfs_builtin_name(static_cast<KBuiltin>(lnd.iidx)); // TessCoord / TessPatchPosition handled
            if (bn == nullptr) { return false; }
            emit_stmt_prefix(gg, li, ss); ss.append(bn); ss.append(";\n");
            return true;
        }
        if (lnd.op == KOp::FieldGet)
        {
            const KNode& agg = gg.node(lnd.a);
            if (agg.op != KOp::UniformBlock) { return false; }
            emit_stmt_prefix(gg, li, ss); ss.append("ubo_"); app_uint(ss, static_cast<crd::u32>(agg.dset)); ss.append("_"); app_uint(ss, static_cast<crd::u32>(agg.iidx)); ss.append(".f"); app_uint(ss, static_cast<crd::u32>(lnd.iidx)); ss.append(";\n");
            return true;
        }
        return false;
    };
    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)]) { continue; }
        if (g.node(i).op == KOp::For) { return false; }
        if (!emit_value_stmt(g, i, s, tese_leaf)) { return false; }
    }
    s.append("  gl_Position = t"); app_uint(s, static_cast<crd::u32>(entry.position)); s.append(";\n");
    for (int k = 0; k < entry.n_out; ++k)
    {
        const int nid = entry.out[k].node;
        if (nid < 0) { continue; }
        s.append("  o_"); app_uint(s, static_cast<crd::u32>(entry.out[k].location)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nid)); s.append(";\n");
    }
    s.append("}\n");
    return true;
}

// A3: comps-aware VECTOR/MATRIX elementwise emitter — vecN/matN temps, INTERLEAVED buffer I/O (comps floats/element),
// GLSL builtins for the vec/mat ops. One per-element kernel (dot/length/det reduce over COMPONENTS, still per-element).
inline bool emit_vec_glsl(const KGraph& g, int output, crd::memory::IAllocator* scratch, GlslKernel& out)
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
        if (nd.a >= 0) { stk.push_back(nd.a); }
        if (nd.b >= 0) { stk.push_back(nd.b); }
        if (nd.c >= 0) { stk.push_back(nd.c); }
        if (nd.d >= 0) { stk.push_back(nd.d); }
        for (int k = 0; k < static_cast<int>(nd.n_ext); ++k) { stk.push_back(g.ext_operand(nd, k)); } // B0-4 variadic operands
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
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    for (int b = 0; b < out.n_inputs; ++b) { s.append("layout(std430, binding = "); app_uint(s, static_cast<crd::u32>(b)); s.append(") readonly buffer B"); app_uint(s, static_cast<crd::u32>(b)); s.append(" { float in"); app_uint(s, static_cast<crd::u32>(b)); s.append("[]; };\n"); }
    s.append("layout(std430, binding = "); app_uint(s, static_cast<crd::u32>(out.n_inputs)); s.append(") writeonly buffer BOUT { float outb[]; };\n");
    s.append("layout(push_constant) uniform PC { uint n; };\n");
    { // quaternion/slerp helper functions (no GLSL builtins) — emitted once when the graph uses them
        bool qm = false;
        bool qc = false;
        bool qr = false;
        bool qa = false;
        bool qt = false;
        bool sl = false;
        for (int i = 0; i < n; ++i) { if (!reach[static_cast<crd::usize>(i)]) { continue; } switch (g.node(i).op) { case KOp::QuatMul: qm = true; break; case KOp::QuatConj: qc = true; break; case KOp::QuatRotate: qr = true; break; case KOp::QuatAxisAngle: qa = true; break; case KOp::QuatToMat3: qt = true; break; case KOp::Slerp: sl = true; break; default: break; } }
        if (qm) { s.append("vec4 crd_qmul(vec4 a,vec4 b){return vec4(a.w*b.xyz+b.w*a.xyz+cross(a.xyz,b.xyz),a.w*b.w-dot(a.xyz,b.xyz));}\n"); }
        if (qc) { s.append("vec4 crd_qconj(vec4 q){return vec4(-q.xyz,q.w);}\n"); }
        if (qr) { s.append("vec3 crd_qrot(vec4 q,vec3 v){vec3 t=2.0*cross(q.xyz,v);return v+q.w*t+cross(q.xyz,t);}\n"); }
        if (qa) { s.append("vec4 crd_qaa(vec3 ax,float an){float h=an*0.5;return vec4(ax*sin(h),cos(h));}\n"); }
        if (qt) { s.append("mat3 crd_qmat(vec4 q){float x=q.x,y=q.y,z=q.z,w=q.w;return mat3(1.0-2.0*(y*y+z*z),2.0*(x*y+w*z),2.0*(x*z-w*y),2.0*(x*y-w*z),1.0-2.0*(x*x+z*z),2.0*(y*z+w*x),2.0*(x*z+w*y),2.0*(y*z-w*x),1.0-2.0*(x*x+y*y));}\n"); }
        if (sl) { s.append("vec4 crd_slerp(vec4 a,vec4 b,float t){float d=dot(a,b);float sg=1.0;if(d<0.0){d=-d;sg=-1.0;}if(d>0.9995){return normalize(mix(a,sg*b,t));}float th=acos(d);float sn=sin(th);return (sin((1.0-t)*th)*a+sin(t*th)*sg*b)/sn;}\n"); }
    }
    s.append("void main() {\n  uint gid = gl_GlobalInvocationID.x;\n  if (gid >= n) { return; }\n");
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
        if (lnd.op != KOp::Input) { return false; } // compute leaf: only `Input` reads a storage buffer (raster uses its own)
        const int lc = lnd.comps();
        const int bd = binding_of[static_cast<crd::usize>(li)];
        emit_stmt_prefix(gg, li, ss);
        if (lc == 1) { ss.append("in"); app_uint(ss, static_cast<crd::u32>(bd)); ss.append("[gid]"); }
        else { ss.append(vtype(lnd.type)); ss.append("("); for (int k = 0; k < lc; ++k) { if (k) { ss.append(", "); } ss.append("in"); app_uint(ss, static_cast<crd::u32>(bd)); ss.append("[gid*"); app_uint(ss, static_cast<crd::u32>(lc)); ss.append("+"); app_uint(ss, static_cast<crd::u32>(k)); ss.append("]"); } ss.append(")"); } // GLSL matrix ctors take column-major scalars
        ss.append(";\n");
        return true;
    };
    const auto emit_expr = [&](int i) -> bool { return emit_value_stmt(g, i, s, compute_leaf); }; // B3-c: the shared emitter

    for (int i = 0; i < n; ++i)
    {
        if (!reach[static_cast<crd::usize>(i)] || varying[static_cast<crd::usize>(i)]) { continue; }
        const KNode& nd = g.node(i);
        if (nd.op == KOp::For) // A4 tier-2: native per-thread `for`; body-scoped nodes (loop-varying) emit INSIDE the loop
        {
            s.append("  precise "); s.append(vtype(nd.type)); s.append(" t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nd.b)); s.append(";\n");
            s.append("  for (int li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = 0; li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(" < int(t"); app_uint(s, static_cast<crd::u32>(nd.a)); s.append("); li_"); app_uint(s, static_cast<crd::u32>(i)); s.append("++) {\n");
            for (int bid = 0; bid < i; ++bid)
            {
                if (body_of[static_cast<crd::usize>(bid)] != i) { continue; }
                const KNode& bn = g.node(bid);
                if (bn.op == KOp::LoopIndex) { s.append("  precise float t"); app_uint(s, static_cast<crd::u32>(bid)); s.append(" = float(li_"); app_uint(s, static_cast<crd::u32>(i)); s.append(");\n"); }
                else if (bn.op == KOp::LoopAcc) { s.append("  precise "); s.append(vtype(bn.type)); s.append(" t"); app_uint(s, static_cast<crd::u32>(bid)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(i)); s.append(";\n"); }
                else if (!emit_expr(bid)) { return false; }
            }
            s.append("  t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = t"); app_uint(s, static_cast<crd::u32>(nd.c)); s.append(";\n  }\n");
        }
        else if (!emit_expr(i)) { return false; }
    }
    // Write back column-major. Matrix-ness comes from the TYPE, not from the component count: a comps==4 output is a
    // vec4 (indexed `t[k]`) or a mat2 (indexed `t[col][row]`), and guessing from `oc` gets mat2 wrong.
    const int    oc  = out.out_comps;
    const KType& oty = g.node(output).type;
    if (oty.kind == TKind::Mat)
    {
        for (int col = 0; col < oty.cols; ++col) { for (int row = 0; row < oty.rows; ++row) { s.append("  outb[gid*"); app_uint(s, oc); s.append("+"); app_uint(s, col * oty.rows + row); s.append("] = t"); app_uint(s, output); s.append("["); app_uint(s, col); s.append("]["); app_uint(s, row); s.append("];\n"); } }
    }
    // the output buffer is `float`; a bool / bvec component converts on write (the oracle materializes 0.0 / 1.0 too).
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

// AS-6b (ADR-0098 §4): the PARAMETERIZED GLSL tiled GEMM — the Vulkan/SPIR-V analogue of `emit_contract_tiled_cuda`, so the
// autotuner drives the Vulkan backend too. Block tile BT×BT, K-depth BK, TM×TM register microtile per thread, NT=(BT/TM)²
// threads. PLAIN-float accumulate (shaderc's performance opt fuses FMA — the fast tier, ULP-tolerant, not bit-exact). The GLSL
// schedule is a subset of `TileSchedule`: BT=bm(=bn), BK=bk, TM=tm(=tn). Constraints: BT%TM==0, NT≤1024, M%BT==N%BT==0, K%BK==0,
// 2·BT·BK·4 ≤ smem. Cooperative shared load: As[row·BK+k] (BT·BK floats) + Bs[k·BT+col] (BK·BT). The autotuner enumerates
// (BT,BK,TM), Vulkan-times each, the oracle certifies, and the winner is the Vulkan device's DB entry (AS-6a device key).
inline bool emit_contract_tiled_glsl_sched(const KGraph& g, int output, int bt, int bk, int tm, GlslKernel& out)
{
    using namespace glsl_detail;
    const KNode& c = g.node(output);
    if (c.op != KOp::Contract || g.node(c.a).op != KOp::Input || g.node(c.b).op != KOp::Input) { return false; }
    if (bt <= 0 || bk <= 0 || tm <= 0 || (bt % tm) != 0) { return false; }
    const int tpr = bt / tm;      // threads per tile-row/col
    const int nt  = tpr * tpr;    // NT = (BT/TM)^2
    if (nt < 1 || nt > 1024) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(c.a).iidx;
    out.input_iidx[1] = g.node(c.b).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    const auto n = [&](int v) { app_uint(s, static_cast<crd::u32>(v)); }; // append an int
    s.append("#version 450\n");
    s.append("layout(local_size_x = "); n(nt); s.append(") in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BA { float A[]; };\n");
    s.append("layout(std430, binding = 1) readonly buffer BB { float Bm[]; };\n");
    s.append("layout(std430, binding = 2) writeonly buffer BC { float C[]; };\n");
    s.append("layout(push_constant) uniform PC { uint M; uint K; uint N; uint nbatch; };\n");
    s.append("shared float As["); n(bt * bk); s.append("];\n");
    s.append("shared float Bs["); n(bk * bt); s.append("];\n");
    s.append("void main() {\n");
    s.append("  uint nbc = N / "); n(bt); s.append("u; uint bid = gl_WorkGroupID.x;\n");
    s.append("  uint blockRow = (bid / nbc) * "); n(bt); s.append("u; uint blockCol = (bid % nbc) * "); n(bt); s.append("u;\n");
    s.append("  uint tid = gl_LocalInvocationID.x; uint tr = tid / "); n(tpr); s.append("u; uint tc = tid % "); n(tpr); s.append("u;\n");
    s.append("  uint arow = blockRow + tr * "); n(tm); s.append("u; uint acol = blockCol + tc * "); n(tm); s.append("u;\n");
    for (int i = 0; i < tm; ++i) // TM×TM scalar accumulators (register-resident)
    {
        s.append("  float ");
        for (int j = 0; j < tm; ++j) { s.append("a"); n(i); n(j); s.append(" = 0.0"); if (j < tm - 1) { s.append(", "); } }
        s.append(";\n");
    }
    s.append("  for (uint k0 = 0u; k0 < K; k0 += "); n(bk); s.append("u) {\n");
    s.append("    for (uint t = tid; t < "); n(bt * bk); s.append("u; t += "); n(nt);
    s.append("u) { uint r = t / "); n(bk); s.append("u; uint cc = t % "); n(bk); s.append("u; As[t] = A[(blockRow + r) * K + (k0 + cc)]; }\n");
    s.append("    for (uint t = tid; t < "); n(bk * bt); s.append("u; t += "); n(nt);
    s.append("u) { uint r = t / "); n(bt); s.append("u; uint cc = t % "); n(bt); s.append("u; Bs[t] = Bm[(k0 + r) * N + (blockCol + cc)]; }\n");
    s.append("    barrier();\n");
    s.append("    for (uint kk = 0u; kk < "); n(bk); s.append("u; ++kk) {\n");
    s.append("      ");
    for (int i = 0; i < tm; ++i)
    {
        s.append("float ar"); n(i); s.append(" = As[(tr * "); n(tm); s.append("u + "); n(i); s.append("u) * "); n(bk); s.append("u + kk]; ");
    }
    s.append("\n      ");
    for (int j = 0; j < tm; ++j)
    {
        s.append("float br"); n(j); s.append(" = Bs[kk * "); n(bt); s.append("u + tc * "); n(tm); s.append("u + "); n(j); s.append("u]; ");
    }
    s.append("\n");
    for (int i = 0; i < tm; ++i)
    {
        s.append("      ");
        for (int j = 0; j < tm; ++j)
        {
            s.append("a"); n(i); n(j); s.append(" = a"); n(i); n(j); s.append(" + ar"); n(i); s.append(" * br"); n(j); s.append("; ");
        }
        s.append("\n");
    }
    s.append("    }\n    barrier();\n  }\n");
    for (int i = 0; i < tm; ++i)
    {
        for (int j = 0; j < tm; ++j)
        {
            s.append("  C[(arow + "); n(i); s.append("u) * N + (acol + "); n(j); s.append("u)] = a"); n(i); n(j); s.append(";\n");
        }
    }
    s.append("}\n");
    return true;
}

// Emit the T2 FAST GEMM (DetTier::Fast) — the ported crush schedule: 64×64 block, 4×4 register microtile, TRANSPOSED-A
// shared (As[k][m] ⇒ the inner-loop A reads are unit-stride) + explicit `fma()` (FMA fusion = the fast tier, ~2× the
// no-FMA `precise` path). Run-to-run deterministic (fixed tile order, no atomics); matches the FMA-tier oracle within
// tolerance, NOT bit-exact vs the T1 `precise` kernel. Winning-schedule lessons from the CUDA campaign (docs/bench/
// 2026-07-08-v17g): transposed shared for conflict-free contiguous reads; keep the grid full (64×64 ⇒ (M/64)*(N/64)
// groups — ample at N≥1024; small-N wants split-K, task #11).
inline bool emit_contract_fast_glsl(const KGraph& g, int output, GlslKernel& out)
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
    // 128×128 block, 8×8 register microtile (256 threads, 16×16). BK=8. Double-buffered transposed shared, vec4.
    // 8×8 microtile ⇒ 64 FMA per shared-read pair ⇒ enough compute to hide shared-load latency (the 4×4 was barrier-bound,
    // nsys: SM-issue 1.4% / DRAM idle). As[k][mg] mg=m/4 (32 vec4/k), Bs[k][ng]. 256 vec4 each per buffer.
    const char* cp[4] = {"x", "y", "z", "w"};
    s.append("shared vec4 As[2][256];\n");
    s.append("shared vec4 Bs[2][256];\n");
    s.append("void main() {\n");
    s.append("  uint nbc = N / 128u; uint bid = gl_WorkGroupID.x;\n");
    s.append("  uint blockRow = (bid / nbc) * 128u; uint blockCol = (bid % nbc) * 128u;\n");
    s.append("  uint tid = gl_LocalInvocationID.x; uint tr = tid / 16u; uint tc = tid % 16u;\n");
    // accumulators: 8 rows × 2 vec4 (cols 0..3, 4..7)
    for (int m = 0; m < 8; ++m)
    {
        char mb[2] = {static_cast<char>('0' + m), '\0'};
        s.append("  vec4 ac"); s.append(mb); s.append("0 = vec4(0.0), ac"); s.append(mb); s.append("1 = vec4(0.0);\n");
    }
    s.append("  uint ntiles = K / 8u;\n");
    // each thread loads 1 As vec4 (slot=tid) + 1 Bs vec4 (slot=tid): k=tid/32, mg/ng=tid%32.
    s.append("  uint sk = tid / 32u; uint smg = tid % 32u;\n");
    s.append("  uint arow0 = (blockRow + smg * 4u) * K + sk; uint bcol0 = blockCol + smg * 4u;\n");
    s.append("  { uint r = arow0; As[0][tid] = vec4(A[r], A[r + K], A[r + 2u * K], A[r + 3u * K]);\n");
    s.append("    uint bo = sk * N + bcol0; Bs[0][tid] = vec4(Bm[bo], Bm[bo + 1u], Bm[bo + 2u], Bm[bo + 3u]); }\n");
    s.append("  barrier();\n");
    s.append("  for (uint i = 0u; i < ntiles; ++i) {\n");
    s.append("    uint cur = i & 1u; uint nxt = (i + 1u) & 1u; bool hn = (i + 1u < ntiles); uint k0n = (i + 1u) * 8u;\n");
    // prefetch next tile (both As+Bs) into registers — latency overlaps the 64-FMA compute below.
    s.append("    vec4 ra = vec4(0.0), rb = vec4(0.0);\n");
    s.append("    if (hn) { uint r = arow0 + k0n; ra = vec4(A[r], A[r + K], A[r + 2u * K], A[r + 3u * K]);\n");
    s.append("              uint bo = (k0n + sk) * N + bcol0; rb = vec4(Bm[bo], Bm[bo + 1u], Bm[bo + 2u], Bm[bo + 3u]); }\n");
    s.append("    for (uint kk = 0u; kk < 8u; ++kk) {\n");
    s.append("      vec4 a0 = As[cur][kk * 32u + tr * 2u]; vec4 a1 = As[cur][kk * 32u + tr * 2u + 1u];\n");   // 8 A rows
    s.append("      vec4 b0 = Bs[cur][kk * 32u + tc * 2u]; vec4 b1 = Bs[cur][kk * 32u + tc * 2u + 1u];\n");   // 8 B cols
    for (int m = 0; m < 8; ++m)
    {
        char        mb[2] = {static_cast<char>('0' + m), '\0'};
        const char* av    = (m < 4) ? "a0." : "a1.";
        const char* comp  = cp[m % 4];
        s.append("      ac"); s.append(mb); s.append("0 = fma(vec4("); s.append(av); s.append(comp); s.append("), b0, ac"); s.append(mb); s.append("0);");
        s.append(" ac"); s.append(mb); s.append("1 = fma(vec4("); s.append(av); s.append(comp); s.append("), b1, ac"); s.append(mb); s.append("1);\n");
    }
    s.append("    }\n");
    s.append("    if (hn) { As[nxt][tid] = ra; Bs[nxt][tid] = rb; }\n");
    s.append("    barrier();\n");
    s.append("  }\n");
    s.append("  uint crow = blockRow + tr * 8u; uint ccol = blockCol + tc * 8u;\n");
    for (int m = 0; m < 8; ++m)
    {
        char mb[2] = {static_cast<char>('0' + m), '\0'};
        s.append("  { uint co = (crow + "); s.append(mb); s.append("u) * N + ccol;");
        s.append(" C[co] = ac"); s.append(mb); s.append("0.x; C[co + 1u] = ac"); s.append(mb); s.append("0.y; C[co + 2u] = ac"); s.append(mb); s.append("0.z; C[co + 3u] = ac"); s.append(mb); s.append("0.w;");
        s.append(" C[co + 4u] = ac"); s.append(mb); s.append("1.x; C[co + 5u] = ac"); s.append(mb); s.append("1.y; C[co + 6u] = ac"); s.append(mb); s.append("1.z; C[co + 7u] = ac"); s.append(mb); s.append("1.w; }\n");
    }
    s.append("}\n");
    return true;
}

// Emit the TENSOR-TIER GEMM (VK_NV_cooperative_matrix2, workgroup-scoped) — fp16 A/B in, fp32 C out. One workgroup owns
// a whole 128×128 accumulator; the DRIVER schedules the entire tile (register blocking, tensor-core dual-issue, load
// pipelining) = cuBLAS-class SASS, ~70 TF on RTX 4070 Ti Super = CUDA-`wmma2` parity, PORTABLY. Dispatched 1D:
// (M/128)*(N/128) workgroups of 256 threads. Push constants {M,K,N}. A/B are float16_t buffers, C is float. v17-h #9.
inline bool emit_contract_coopmat2_glsl(GlslKernel& out, crd::u32 kdim, crd::u32 ndim)
{
    crd::containers::String& s = out.source;
    s.clear();
    // Bake K and N as GLSL literals (not push constants): the driver then specializes the coopmat loop — known trip
    // count, known strides, unrolled — which is worth ~2× on the tensor path vs a runtime dim. (per-shape recompile.)
    char kb[16];
    char nb[16];
    auto tou = [](crd::u32 v, char* buf) {
        char tmp[12];
        int  t = 0;
        if (v == 0) { tmp[t++] = '0'; }
        while (v > 0) { tmp[t++] = static_cast<char>('0' + (v % 10U)); v /= 10U; }
        int p = 0;
        while (t > 0) { buf[p++] = tmp[--t]; }
        buf[p++] = 'u';
        buf[p]   = '\0';
    };
    tou(kdim, kb);
    tou(ndim, nb);
    s.append("#version 450\n");
    s.append("#extension GL_KHR_cooperative_matrix : require\n");
    s.append("#extension GL_NV_cooperative_matrix2 : require\n");
    s.append("#extension GL_KHR_memory_scope_semantics : require\n");
    s.append("#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BA { float16_t A[]; };\n");
    s.append("layout(std430, binding = 1) readonly buffer BB { float16_t Bm[]; };\n");
    s.append("layout(std430, binding = 2) writeonly buffer BC { float C[]; };\n");
    s.append("void main() {\n");
    s.append("  uint blockRow = gl_WorkGroupID.y * 128u; uint blockCol = gl_WorkGroupID.x * 128u;\n"); // 2D grid = L2 locality
    s.append("  coopmat<float, gl_ScopeWorkgroup, 128, 128, gl_MatrixUseAccumulator> acc =\n");
    s.append("      coopmat<float, gl_ScopeWorkgroup, 128, 128, gl_MatrixUseAccumulator>(0.0);\n");
    s.append("  for (uint k = 0u; k < "); s.append(kb); s.append("; k += 16u) {\n");
    s.append("    coopmat<float16_t, gl_ScopeWorkgroup, 128, 16, gl_MatrixUseA> a;\n");
    s.append("    coopmat<float16_t, gl_ScopeWorkgroup, 16, 128, gl_MatrixUseB> b;\n");
    s.append("    coopMatLoad(a, A, blockRow * "); s.append(kb); s.append(" + k, "); s.append(kb); s.append(", gl_CooperativeMatrixLayoutRowMajor);\n");
    s.append("    coopMatLoad(b, Bm, k * "); s.append(nb); s.append(" + blockCol, "); s.append(nb); s.append(", gl_CooperativeMatrixLayoutRowMajor);\n");
    s.append("    acc = coopMatMulAdd(a, b, acc);\n");
    s.append("  }\n");
    s.append("  coopMatStore(acc, C, blockRow * "); s.append(nb); s.append(" + blockCol, "); s.append(nb); s.append(", gl_CooperativeMatrixLayoutRowMajor);\n");
    s.append("}\n");
    out.n_inputs = 2;
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

// Emit an atomic scatter-ADD (histogram) kernel — out[M] (pre-zeroed by the backend), then atomicAdd(out[idx[i]], upd[i])
// over the N inputs. INTEGER atomics ⇒ order-independent ⇒ bit-exact + run-to-run deterministic (the moat survives). idx
// + updates are Input leaves (the index computation, e.g. a radix digit, is a prior elementwise kernel). Push: n = N.
inline bool emit_scatteradd_glsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& sn = g.node(output);
    if (sn.op != KOp::ScatterAdd || g.node(sn.a).op != KOp::Input || g.node(sn.b).op != KOp::Input) { return false; }
    out.n_inputs      = 2;
    out.input_iidx[0] = g.node(sn.a).iidx; // idx
    out.input_iidx[1] = g.node(sn.b).iidx; // updates
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BI { int idxb[]; };\n");
    s.append("layout(std430, binding = 1) readonly buffer BU { int updb[]; };\n");
    s.append("layout(std430, binding = 2) buffer BO { int outb[]; };\n"); // read-write for atomicAdd
    s.append("layout(push_constant) uniform PC { uint n; };\n");
    s.append("void main() {\n  uint i = gl_GlobalInvocationID.x;\n  if (i >= n) { return; }\n");
    s.append("  atomicAdd(outb[idxb[i]], updb[i]);\n}\n");
    return true;
}

// Scalar BROADCAST: out[gid] = A[0] — a 1-element source fanned out to [N]. (The radix's totalFalses fan-out.) dtype-aware.
inline bool emit_broadcast_glsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& bn = g.node(output);
    if (bn.op != KOp::Broadcast || g.node(bn.a).op != KOp::Input || g.node(bn.a).shape.numel() != 1) { return false; }
    out.n_inputs      = 1;
    out.input_iidx[0] = g.node(bn.a).iidx;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) readonly buffer BA { "); s.append(glsl_detail::ctype(g.node(bn.a).dtype())); s.append(" A[]; };\n");
    s.append("layout(std430, binding = 1) writeonly buffer BO { "); s.append(glsl_detail::ctype(bn.dtype())); s.append(" O[]; };\n");
    s.append("layout(push_constant) uniform PC { uint n; };\n");
    s.append("void main() {\n  uint gid = gl_GlobalInvocationID.x;\n  if (gid >= n) { return; }\n");
    s.append("  O[gid] = A[0];\n}\n");
    return true;
}

// 1-D IOTA: out[gid] = gid — a materialized index vector (the elementwise emitter can't see gid across a kernel boundary).
inline bool emit_iota_glsl(const KGraph& g, int output, GlslKernel& out)
{
    const KNode& in = g.node(output);
    if (in.op != KOp::Iota || in.shape.rank != 1) { return false; } // 1-D only (flat index == gid)
    out.n_inputs = 0;
    crd::containers::String& s = out.source;
    s.clear();
    s.append("#version 450\n");
    s.append("layout(local_size_x = 256) in;\n");
    s.append("layout(std430, binding = 0) writeonly buffer BO { "); s.append(glsl_detail::ctype(in.dtype())); s.append(" O[]; };\n");
    s.append("layout(push_constant) uniform PC { uint n; };\n");
    s.append("void main() {\n  uint gid = gl_GlobalInvocationID.x;\n  if (gid >= n) { return; }\n");
    s.append("  O[gid] = "); s.append(glsl_detail::dt_is_int(in.dtype()) ? "int(gid)" : "float(gid)"); s.append(";\n}\n");
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
        if (nd.d >= 0) { stk.push_back(nd.d); }
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
