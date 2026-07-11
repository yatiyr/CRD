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

    const char xyzw[4] = {'x', 'y', 'z', 'w'};
    const auto ta       = [&](int id) { s.append("t"); app_uint(s, static_cast<crd::u32>(id)); };
    const auto emit_expr = [&](int i) -> bool
    {
        const KNode& nd = g.node(i);
        const int    c  = nd.comps();
        // B0-4 SROA: an aggregate is never materialized on the GPU. `StructMake`/`ArrayMake` emit nothing, and a
        // `FieldGet`/`ArrayGet` resolves straight to the operand temp its index names -- what Slang and DXC do. This
        // means the aggregate must come from a Make node; a struct produced by a `Select` would need a real GLSL struct
        // type, so refuse it loudly rather than emit something subtly wrong.
        if (nd.op == KOp::StructMake || nd.op == KOp::ArrayMake) { return true; }
        if (nd.op == KOp::FieldGet || nd.op == KOp::ArrayGet)
        {
            const KNode& agg = g.node(nd.a);
            if (agg.op != KOp::StructMake && agg.op != KOp::ArrayMake) { return false; }
            s.append(glsl_detail::is_float_dtype(nd.dtype()) ? "  precise " : "  "); s.append(vtype(nd.type));
            s.append(" t"); app_uint(s, i); s.append(" = "); ta(g.ext_operand(agg, nd.iidx)); s.append(";\n");
            return true;
        }
        // `precise` is a float-only qualifier in GLSL -- a `precise bool`/`precise ivec3` is a compile error.
        s.append(glsl_detail::is_float_dtype(nd.dtype()) ? "  precise " : "  "); s.append(vtype(nd.type)); s.append(" t"); app_uint(s, static_cast<crd::u32>(i)); s.append(" = ");
        switch (nd.op)
        {
        case KOp::Input: { const int bd = binding_of[static_cast<crd::usize>(i)]; if (c == 1) { s.append("in"); app_uint(s, static_cast<crd::u32>(bd)); s.append("[gid]"); } else { s.append(vtype(nd.type)); s.append("("); for (int k = 0; k < c; ++k) { if (k) { s.append(", "); } s.append("in"); app_uint(s, static_cast<crd::u32>(bd)); s.append("[gid*"); app_uint(s, static_cast<crd::u32>(c)); s.append("+"); app_uint(s, static_cast<crd::u32>(k)); s.append("]"); } s.append(")"); } break; } // GLSL matrix ctors take column-major scalars — exactly our flat layout
        case KOp::Const: app_flit(s, nd.cval); break;
        case KOp::Cast: s.append(vtype(nd.type)); s.append("("); ta(nd.a); s.append(")"); break;
        case KOp::Neg: s.append("-"); ta(nd.a); break;
        case KOp::Recip: s.append("(1.0 / "); ta(nd.a); s.append(")"); break;
        case KOp::Abs: s.append("abs("); ta(nd.a); s.append(")"); break;
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
        case KOp::Vec2: s.append("vec2("); ta(nd.a); s.append(", "); ta(nd.b); s.append(")"); break;
        case KOp::Vec3: s.append("vec3("); ta(nd.a); s.append(", "); ta(nd.b); s.append(", "); ta(nd.c); s.append(")"); break;
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
        // C column-vectors -> matCxR. Driven by type.cols, not by comps: mat2 has only 2 operands (c/d are -1).
        case KOp::MatFromCols: { const int mcols = nd.type.cols; const int operand[4] = {nd.a, nd.b, nd.c, nd.d}; s.append(vtype(nd.type)); s.append("("); for (int k = 0; k < mcols; ++k) { if (k) { s.append(", "); } ta(operand[k]); } s.append(")"); break; }
        // any/all take a bvec directly; a numeric vector is first compared componentwise against zero. Result is `bool`.
        case KOp::VecAny: if (g.node(nd.a).dtype() == DType::Bool) { s.append("any("); ta(nd.a); s.append(")"); } else { s.append("any(notEqual("); ta(nd.a); s.append(", "); s.append(vtype(g.node(nd.a).type)); s.append("(0.0)))"); } break;
        case KOp::VecAll: if (g.node(nd.a).dtype() == DType::Bool) { s.append("all("); ta(nd.a); s.append(")"); } else { s.append("all(notEqual("); ta(nd.a); s.append(", "); s.append(vtype(g.node(nd.a).type)); s.append("(0.0)))"); } break;
        // B0-3 comparisons. GLSL has no `<` on vectors -- componentwise needs lessThan()/equal()/... yielding a bvecN.
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
