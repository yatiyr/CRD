#pragma once

// ckir_nodes.hpp — the MATERIALX-PARITY NODE LIBRARY layer on core CKIR (D-007 B6). A "node" here is a value-graph BUILDER:
// a free function `int node(KGraph& g, int a, ...)` that composes core KOps and returns the output node id — exactly the
// style of ckir_material.hpp's surface builders. This layer gives CKIR the MaterialX standard-library OPERATOR set (math ·
// logical · conditional · channel · adjustment · compositing · convolution), so a material graph can be authored ONCE in
// the central IR and lowered to EVERY backend (ADR-0101). B6-b adds SOURCE nodes (noise/shapes/geometric); B6-c adds UV;
// B6-d adds NPR.
//
// ── CONFORMANCE METHODOLOGY (how "bit-exact vs the MaterialX reference" is met) ───────────────────────────────────────
// B6 is a CONFORMANCE library, not a perf crush: the bar is "produce the same values as MaterialX", and MaterialX's
// published genglsl/nodegraph reference IS the definition of each node. So each node here is a FAITHFUL TRANSCRIPTION of
// that reference's exact operation sequence, and is verified bit-exact on CKIR's own deterministic F32 CPU oracle
// (`eval_cpu`) against the same reference formula, plus observed pixel-identically on both GPU backends (Vulkan + DX12).
// Op ORDER is preserved so the emitted GLSL/HLSL is operation-equivalent to MaterialX genglsl under NoContraction (no
// stray FMA fusion). Every non-obvious constant/factoring is transcribed from the MaterialX source and cited inline
// (MaterialX main @ AcademySoftwareFoundation/MaterialX, libraries/stdlib). Key transcription facts pinned from source:
//   * MaterialX `modulo` == GLSL `mod` == FLOOR-based (`a - b*floor(a/b)`), NOT C `fmod`/`KOp::Mod` (differs for negatives).
//   * `mix(fg,bg,m)` == `bg*(1-m)+fg*m`; `KOp::Mix(a,b,c)=a*(1-c)+b*c` is bit-identical (IEEE add/mul commute), so the
//     compositing "optional mixing" tail is `KOp::Mix(bg, base, m)` for every blend, matching MaterialX's `<mix>` tail.
//   * default `lumacoeffs` = ACEScg Rec.709 `(0.2722287, 0.6740818, 0.0536895)`; `M_FLOAT_EPS = 1e-8`.
//   * `smoothstep(in,low,high)` == GLSL `smoothstep(low,high,in)` == `KOp::Smoothstep(low,high,in)` (the MaterialX >=/<=
//     wrapper is a no-op on the result).
// Branchy references (rgbtohsv/hsvtorgb, burn/dodge/disjointover guards) become branchless `Select` chains: the unselected
// arm may compute a NaN (e.g. /delta when delta==0) but is discarded, so the selected result stays bit-exact.

#include <crd/kir/ckir.hpp>

namespace crd::kir::nodes
{

// ── broadcast helpers ────────────────────────────────────────────────────────────────────────────────────────────────
// MaterialX ops are per-channel and mix float params with color/vector operands; core KOps are same-shape (broadcast is
// explicit via Splat). These widen a scalar operand to a vector operand's component count so the node builders read cleanly.
namespace detail
{
[[nodiscard]] inline int   ncomp(const KGraph& g, int n) noexcept { return g.node(n).comps(); }
// a scalar constant shaped AND dtyped like node `like` (so a same-shape binary op is legal, and the library is dtype-
// flexible: F32 for the GPU path, F64 for the exact CPU-oracle conformance tests — no stray F32 rounding either way).
[[nodiscard]] inline int   konst(KGraph& g, int like, crd::f64 v) { return g.constant(v, g.node(like).shape, g.node(like).dtype()); }
// binary op with scalar<->vector broadcast (splat the narrower operand to the wider one's comps).
[[nodiscard]] inline int bin(KGraph& g, KOp op, int a, int b)
{
    const int ca = ncomp(g, a);
    const int cb = ncomp(g, b);
    if (ca == cb) { return g.binary(op, a, b); }
    if (ca == 1) { return g.binary(op, g.splat(a, cb), b); }
    if (cb == 1) { return g.binary(op, a, g.splat(b, ca)); }
    return g.binary(op, a, b); // two mismatched vectors — a caller error; `ckir_shape.hpp`'s checker (run by
                               // every cooker) refuses the graph BY NAME instead of letting the shader compiler
                               // fail far from the asset (the 38-E7 pcf-uv scar)
}
// ternary op (Clamp/Mix/Smoothstep) with every operand broadcast to the widest comps.
[[nodiscard]] inline int tern(KGraph& g, KOp op, int a, int b, int c)
{
    int w = ncomp(g, a);
    if (ncomp(g, b) > w) { w = ncomp(g, b); }
    if (ncomp(g, c) > w) { w = ncomp(g, c); }
    const auto up = [&](int x) { return (ncomp(g, x) == 1 && w > 1) ? g.splat(x, w) : x; };
    return g.ternary(op, up(a), up(b), up(c));
}
// select with a SCALAR (per-invocation) condition, a/b broadcast to the widest comps. NOTE: core `KOp::Select` takes ONE
// condition per element (it broadcasts across components) — it does NOT accept a per-channel `bvecN`. A node whose branch
// is genuinely PER-CHANNEL (burn/dodge/overlay, matching MaterialX's per-channel genglsl loop) must use `per_channel`.
[[nodiscard]] inline int sel(KGraph& g, int cond, int a, int b)
{
    int w = ncomp(g, a);
    if (ncomp(g, b) > w) { w = ncomp(g, b); }
    const auto up = [&](int x) { return (ncomp(g, x) == 1 && w > 1) ? g.splat(x, w) : x; };
    return g.select(cond, up(a), up(b));
}
// apply a SCALAR 2-input builder `f(g, a_c, b_c)` to each channel of a<->b (scalar operands broadcast), recombining to the
// wider comps. This mirrors MaterialX's per-channel genglsl (mx_<blend>_color3 = 3 scalar mx_<blend>_float calls) — the
// only faithful way to express a per-channel BRANCH given the scalar-condition Select.
template <class F>
[[nodiscard]] inline int per_channel(KGraph& g, int a, int b, const F& f)
{
    const int n = ncomp(g, a);
    const int m = ncomp(g, b);
    const int w = n > m ? n : m;
    if (w == 1) { return f(g, a, b); }
    const auto ch = [&](int x, int c) { return ncomp(g, x) == 1 ? x : g.swizzle(x, c); };
    int        out[4] = {-1, -1, -1, -1};
    for (int c = 0; c < w; ++c) { out[c] = f(g, ch(a, c), ch(b, c)); }
    if (w == 2) { return g.vec2(out[0], out[1]); }
    if (w == 3) { return g.vec3(out[0], out[1], out[2]); }
    return g.vec4(out[0], out[1], out[2], out[3]);
}
} // namespace detail

// ── MATH nodes (MaterialX nodegroup "math") ──────────────────────────────────────────────────────────────────────────
// Per-channel / broadcasting. add/subtract/multiply/divide are the primitive KOps; the rest transcribe the MaterialX math.
[[nodiscard]] inline int add(KGraph& g, int a, int b) { return detail::bin(g, KOp::Add, a, b); }
[[nodiscard]] inline int subtract(KGraph& g, int a, int b) { return detail::bin(g, KOp::Sub, a, b); }
[[nodiscard]] inline int multiply(KGraph& g, int a, int b) { return detail::bin(g, KOp::Mul, a, b); }
[[nodiscard]] inline int divide(KGraph& g, int a, int b) { return detail::bin(g, KOp::Div, a, b); }
// MaterialX `modulo` == GLSL mod == floor-based: a - b*floor(a/b). (NOT KOp::Mod, which is C fmod / trunc-based.)
[[nodiscard]] inline int modulo(KGraph& g, int a, int b)
{
    return detail::bin(g, KOp::Sub, a, detail::bin(g, KOp::Mul, b, g.unary(KOp::Floor, detail::bin(g, KOp::Div, a, b))));
}
// MaterialX `invert(amount, in)` = amount - in (amount default 1).
[[nodiscard]] inline int invert(KGraph& g, int in_, int amount) { return detail::bin(g, KOp::Sub, amount, in_); }
[[nodiscard]] inline int invert(KGraph& g, int in_) { return detail::bin(g, KOp::Sub, detail::konst(g, in_, 1.0), in_); }
[[nodiscard]] inline int absval(KGraph& g, int a) { return g.unary(KOp::Abs, a); }
[[nodiscard]] inline int floor(KGraph& g, int a) { return g.unary(KOp::Floor, a); }
[[nodiscard]] inline int ceil(KGraph& g, int a) { return g.unary(KOp::Ceil, a); }
[[nodiscard]] inline int round(KGraph& g, int a) { return g.unary(KOp::Round, a); }
[[nodiscard]] inline int sign(KGraph& g, int a) { return g.unary(KOp::Sign, a); }
[[nodiscard]] inline int power(KGraph& g, int a, int b) { return detail::bin(g, KOp::Pow, a, b); }
[[nodiscard]] inline int sqrt(KGraph& g, int a) { return g.unary(KOp::Sqrt, a); }
[[nodiscard]] inline int ln(KGraph& g, int a) { return g.unary(KOp::Log, a); }
[[nodiscard]] inline int exp(KGraph& g, int a) { return g.unary(KOp::Exp, a); }
[[nodiscard]] inline int sin(KGraph& g, int a) { return g.unary(KOp::Sin, a); }
[[nodiscard]] inline int cos(KGraph& g, int a) { return g.unary(KOp::Cos, a); }
[[nodiscard]] inline int tan(KGraph& g, int a) { return g.unary(KOp::Tan, a); }
[[nodiscard]] inline int asin(KGraph& g, int a) { return g.unary(KOp::Asin, a); }
[[nodiscard]] inline int acos(KGraph& g, int a) { return g.unary(KOp::Acos, a); }
[[nodiscard]] inline int atan2(KGraph& g, int iny, int inx) { return detail::bin(g, KOp::Atan2, iny, inx); }
// MaterialX min/max; named `minimum`/`maximum` to dodge the windows.h min()/max() macros the DX12 TUs pull in.
[[nodiscard]] inline int minimum(KGraph& g, int a, int b) { return detail::bin(g, KOp::Min, a, b); }
[[nodiscard]] inline int maximum(KGraph& g, int a, int b) { return detail::bin(g, KOp::Max, a, b); }
// MaterialX clamp(in, low=0, high=1). KOp::Clamp(x, min, max).
[[nodiscard]] inline int clamp(KGraph& g, int in_, int low, int high) { return detail::tern(g, KOp::Clamp, in_, low, high); }
[[nodiscard]] inline int clamp01(KGraph& g, int in_) { return detail::tern(g, KOp::Clamp, in_, detail::konst(g, in_, 0.0), detail::konst(g, in_, 1.0)); }
// MaterialX smoothstep(in, low, high) == GLSL smoothstep(low, high, in) == KOp::Smoothstep(low, high, in).
[[nodiscard]] inline int smoothstep(KGraph& g, int in_, int low, int high) { return detail::tern(g, KOp::Smoothstep, low, high, in_); }
// MaterialX remap(in, inlow, inhigh, outlow, outhigh) = outlow + (in-inlow)/(inhigh-inlow) * (outhigh-outlow).
[[nodiscard]] inline int remap(KGraph& g, int in_, int inlow, int inhigh, int outlow, int outhigh)
{
    const int t = detail::bin(g, KOp::Div, detail::bin(g, KOp::Sub, in_, inlow), detail::bin(g, KOp::Sub, inhigh, inlow));
    return detail::bin(g, KOp::Add, outlow, detail::bin(g, KOp::Mul, t, detail::bin(g, KOp::Sub, outhigh, outlow)));
}
[[nodiscard]] inline int remap01(KGraph& g, int in_, int inlow, int inhigh)
{
    return detail::bin(g, KOp::Div, detail::bin(g, KOp::Sub, in_, inlow), detail::bin(g, KOp::Sub, inhigh, inlow));
}

// ── GEOMETRIC nodes (MaterialX math, vector-valued) ──────────────────────────────────────────────────────────────────
[[nodiscard]] inline int normalize(KGraph& g, int v) { return g.normalize(v); }
[[nodiscard]] inline int magnitude(KGraph& g, int v) { return g.vlength(v); }
[[nodiscard]] inline int dotproduct(KGraph& g, int a, int b) { return g.dot(a, b); }
[[nodiscard]] inline int crossproduct(KGraph& g, int a, int b) { return g.cross(a, b); }
[[nodiscard]] inline int distance(KGraph& g, int a, int b) { return g.distance(a, b); }

// ── LOGICAL nodes (MaterialX nodegroup "conditional": and/or/xor/not on booleans, held as exact 0/1 floats) ───────────
// `and`/`or`/`not`/`xor` are C++ alternative-token keywords, so these keep the MaterialX ND_logical_* prefix.
[[nodiscard]] inline int logical_and(KGraph& g, int a, int b) { return detail::bin(g, KOp::Min, a, b); }               // 1&&1 -> min = 1
[[nodiscard]] inline int logical_or(KGraph& g, int a, int b) { return detail::bin(g, KOp::Max, a, b); }                // 1||0 -> max = 1
[[nodiscard]] inline int logical_not(KGraph& g, int a) { return detail::bin(g, KOp::Sub, detail::konst(g, a, 1.0), a); } // 1 - a
[[nodiscard]] inline int logical_xor(KGraph& g, int a, int b) { return g.binary(KOp::CmpNe, a, b); }                   // a != b

// ── CONDITIONAL nodes (MaterialX nodegroup "conditional") ────────────────────────────────────────────────────────────
// ifgreater(value1, value2, in1, in2): value1 > value2 ? in1 : in2  (spec: in1 if v1>v2, else in2).
[[nodiscard]] inline int ifgreater(KGraph& g, int value1, int value2, int in1, int in2) { return detail::sel(g, g.binary(KOp::CmpGt, value1, value2), in1, in2); }
// ifgreatereq(value1, value2, in1, in2): value1 >= value2 ? in1 : in2.
[[nodiscard]] inline int ifgreatereq(KGraph& g, int value1, int value2, int in1, int in2) { return detail::sel(g, g.binary(KOp::CmpGe, value1, value2), in1, in2); }
// ifequal(value1, value2, in1, in2): value1 == value2 ? in1 : in2.
[[nodiscard]] inline int ifequal(KGraph& g, int value1, int value2, int in1, int in2) { return detail::sel(g, g.binary(KOp::CmpEq, value1, value2), in1, in2); }
// switch (5-stream classic): pass on in[round(which)] for which in {0..4}. Bands centred on the integers (round-to-nearest).
[[nodiscard]] inline int switch5(KGraph& g, int in0, int in1, int in2, int in3, int in4, int which)
{
    const auto band = [&](double edge) { return g.binary(KOp::CmpLt, which, detail::konst(g, which, edge)); };
    return detail::sel(g, band(0.5), in0, detail::sel(g, band(1.5), in1, detail::sel(g, band(2.5), in2, detail::sel(g, band(3.5), in3, in4))));
}

// ── CHANNEL nodes (MaterialX nodegroup "channel") ────────────────────────────────────────────────────────────────────
[[nodiscard]] inline int combine2(KGraph& g, int a, int b) { return g.vec2(a, b); }
[[nodiscard]] inline int combine3(KGraph& g, int a, int b, int c) { return g.vec3(a, b, c); }
[[nodiscard]] inline int combine4(KGraph& g, int a, int b, int c, int d) { return g.vec4(a, b, c, d); }
// combine a color3 + a float alpha into a color4 (the common "rgb + a" combine).
[[nodiscard]] inline int combine_c3f(KGraph& g, int rgb, int a) { return g.vec_concat(rgb, a); }
// extract(in, index): pull channel `index` out of a vector (MaterialX extract / separate outputs).
[[nodiscard]] inline int extract(KGraph& g, int v, int index) { return g.swizzle(v, index); }
// convert float -> colorN (splat) and color3 -> color4 (append alpha 1).
[[nodiscard]] inline int convert_f_vec(KGraph& g, int a, int width) { return g.splat(a, width); }
[[nodiscard]] inline int convert_c3_c4(KGraph& g, int rgb) { return g.vec_concat(rgb, detail::konst(g, rgb, 1.0)); }

// ── ADJUSTMENT nodes (MaterialX nodegroup "adjustment") ──────────────────────────────────────────────────────────────
// The default ACEScg Rec.709 luma weights (MaterialX default `lumacoeffs`).
inline constexpr double kLumaR = 0.2722287;
inline constexpr double kLumaG = 0.6740818;
inline constexpr double kLumaB = 0.0536895;
// luminance(in, lumacoeffs) = vec3(dot(in.rgb, lumacoeffs)) — broadcast the grey to all channels (mx_luminance_color3).
[[nodiscard]] inline int luminance(KGraph& g, int in3, int lumacoeffs3) { return g.splat(g.dot(in3, lumacoeffs3), 3); }
[[nodiscard]] inline int luminance(KGraph& g, int in3)
{
    const int coeff = g.vec3(detail::konst(g, in3, kLumaR), detail::konst(g, in3, kLumaG), detail::konst(g, in3, kLumaB));
    return luminance(g, in3, coeff);
}
// contrast(in, amount=1, pivot=0.5) = (in - pivot) * amount + pivot  (NG_contrast: sub, mul, add).
[[nodiscard]] inline int contrast(KGraph& g, int in_, int amount, int pivot)
{
    return detail::bin(g, KOp::Add, detail::bin(g, KOp::Mul, detail::bin(g, KOp::Sub, in_, pivot), amount), pivot);
}
// range(in, inlow, inhigh, gamma, outlow, outhigh, doclamp): remap to 0..1, gamma via pow(_,1/gamma), remap to out, opt clamp.
[[nodiscard]] inline int range(KGraph& g, int in_, int inlow, int inhigh, int gamma, int outlow, int outhigh, bool doclamp)
{
    int t = remap01(g, in_, inlow, inhigh);
    t     = detail::bin(g, KOp::Pow, t, detail::bin(g, KOp::Div, detail::konst(g, gamma, 1.0), gamma)); // pow(t, 1/gamma)
    t     = detail::bin(g, KOp::Add, outlow, detail::bin(g, KOp::Mul, t, detail::bin(g, KOp::Sub, outhigh, outlow)));
    return doclamp ? detail::tern(g, KOp::Clamp, t, outlow, outhigh) : t;
}
// saturate(in, amount=1, lumacoeffs) = mix(fg=in, bg=luminance(in), amount) = luma*(1-amount) + in*amount (NG_saturate).
[[nodiscard]] inline int saturate(KGraph& g, int in3, int amount)
{
    // ⛔⛔ RAF-10: robust to a SAMPLED vec4 (a post grade pipes the sampler result straight in). Luminance is computed
    // over RGB and the mix runs on RGB; a vec4 keeps its alpha lane untouched. Without this, `Mix(luminance[vec3],
    // in[vec4], amount)` lowered to `mix(vec3, vec4, float)` — no GLSL overload → create_program returned null while the
    // CPU oracle stayed green. The vec3 path is node-for-node the historical one, so every material is byte-unchanged.
    if (g.node(in3).comps() >= 4)
    {
        const int rgb   = g.vec3(g.vec_comp(in3, 0), g.vec_comp(in3, 1), g.vec_comp(in3, 2));
        const int mixed = detail::tern(g, KOp::Mix, luminance(g, rgb), rgb, amount);
        return g.vec4(g.vec_comp(mixed, 0), g.vec_comp(mixed, 1), g.vec_comp(mixed, 2), g.vec_comp(in3, 3));
    }
    return detail::tern(g, KOp::Mix, luminance(g, in3), in3, amount); // Mix(a=luma, b=in, c=amount)
}
// rgbtohsv(c) — transcribed from mx_rgbtohsv (branchless Select form; NaN /delta arms are discarded when s<=0).
[[nodiscard]] inline int rgbtohsv(KGraph& g, int c)
{
    const auto k = [&](double v) { return detail::konst(g, c, v); };
    const int  r = g.swizzle(c, 0);
    const int  gg = g.swizzle(c, 1);
    const int  b = g.swizzle(c, 2);
    const int  maxcomp = g.binary(KOp::Max, r, g.binary(KOp::Max, gg, b));
    const int  mincomp = g.binary(KOp::Min, r, g.binary(KOp::Min, gg, b));
    const int  delta   = g.binary(KOp::Sub, maxcomp, mincomp);
    const int  v       = maxcomp;
    const int  s       = g.select(g.binary(KOp::CmpGt, maxcomp, k(0.0)), g.binary(KOp::Div, delta, maxcomp), k(0.0));
    const int  h_r = g.binary(KOp::Div, g.binary(KOp::Sub, gg, b), delta);                                        // r max: (g-b)/delta
    const int  h_g = g.binary(KOp::Add, k(2.0), g.binary(KOp::Div, g.binary(KOp::Sub, b, r), delta));             // g max: 2+(b-r)/delta
    const int  h_b = g.binary(KOp::Add, k(4.0), g.binary(KOp::Div, g.binary(KOp::Sub, r, gg), delta));            // else:  4+(r-g)/delta
    const int  hsel = g.select(g.binary(KOp::CmpGe, r, maxcomp), h_r, g.select(g.binary(KOp::CmpGe, gg, maxcomp), h_g, h_b));
    int        h    = g.binary(KOp::Mul, hsel, k(1.0 / 6.0));
    h               = g.select(g.binary(KOp::CmpLt, h, k(0.0)), g.binary(KOp::Add, h, k(1.0)), h);                // wrap negative
    h               = g.select(g.binary(KOp::CmpLe, s, k(0.0)), k(0.0), h);                                       // s<=0 -> h=0
    return g.vec3(h, s, v);
}
// hsvtorgb(hsv) — transcribed from mx_hsvtorgb (branchless: the 6 sector candidates + the s<0.0001 grey short-circuit).
[[nodiscard]] inline int hsvtorgb(KGraph& g, int hsv)
{
    const auto k = [&](double v) { return detail::konst(g, hsv, v); };
    const int  h = g.swizzle(hsv, 0);
    const int  s = g.swizzle(hsv, 1);
    const int  v = g.swizzle(hsv, 2);
    const int  h6 = g.binary(KOp::Mul, k(6.0), g.binary(KOp::Sub, h, g.unary(KOp::Floor, h)));                    // 6*(h-floor(h))
    const int  hi = g.unary(KOp::Trunc, h6);                                                                       // int(trunc(h6))
    const int  f  = g.binary(KOp::Sub, h6, hi);
    const int  p  = g.binary(KOp::Mul, v, g.binary(KOp::Sub, k(1.0), s));                                          // v*(1-s)
    const int  q  = g.binary(KOp::Mul, v, g.binary(KOp::Sub, k(1.0), g.binary(KOp::Mul, s, f)));                   // v*(1-s*f)
    const int  t  = g.binary(KOp::Mul, v, g.binary(KOp::Sub, k(1.0), g.binary(KOp::Mul, s, g.binary(KOp::Sub, k(1.0), f)))); // v*(1-s*(1-f))
    const int  c0 = g.vec3(v, t, p); // hi==0
    const int  c1 = g.vec3(q, v, p); // hi==1
    const int  c2 = g.vec3(p, v, t); // hi==2
    const int  c3 = g.vec3(p, q, v); // hi==3
    const int  c4 = g.vec3(t, p, v); // hi==4
    const int  c5 = g.vec3(v, p, q); // else
    const auto eqk = [&](double kk) { return g.binary(KOp::CmpEq, hi, k(kk)); };
    const int  rgb = g.select(eqk(0.0), c0, g.select(eqk(1.0), c1, g.select(eqk(2.0), c2, g.select(eqk(3.0), c3, g.select(eqk(4.0), c4, c5)))));
    const int  grey = g.vec3(v, v, v);
    return g.select(g.binary(KOp::CmpLt, s, k(0.0001)), grey, rgb); // s < 0.0001 -> (v,v,v)
}
// hsvadjust(in, amount) : hue += amount.x, sat *= amount.y, val *= amount.z (NG_hsvadjust).
[[nodiscard]] inline int hsvadjust(KGraph& g, int c3, int amount3)
{
    const int hsv = rgbtohsv(g, c3);
    const int h   = g.binary(KOp::Add, g.swizzle(hsv, 0), g.swizzle(amount3, 0));
    const int s   = g.binary(KOp::Mul, g.swizzle(hsv, 1), g.swizzle(amount3, 1));
    const int v   = g.binary(KOp::Mul, g.swizzle(hsv, 2), g.swizzle(amount3, 2));
    return hsvtorgb(g, g.vec3(h, s, v));
}

// ── COMPOSITING nodes (MaterialX nodegroup "compositing") ────────────────────────────────────────────────────────────
// M_FLOAT_EPS for the burn/dodge/disjointover division guards.
inline constexpr double kFloatEps = 1e-8;
namespace detail
{
// the shared "optional mixing" tail every merge node ends in: mix(bg, base, mix) = bg*(1-mix) + base*mix.
[[nodiscard]] inline int mix_tail(KGraph& g, int base, int bg, int mix) { return tern(g, KOp::Mix, bg, base, mix); }
} // namespace detail

// premult(in) = (in.rgb * in.a, in.a).
[[nodiscard]] inline int premult(KGraph& g, int c4)
{
    const int rgb = g.swizzle(c4, 0, 1, 2);
    const int a   = g.swizzle(c4, 3);
    return g.vec_concat(detail::bin(g, KOp::Mul, rgb, a), a);
}
// unpremult(in) = (in.rgb / in.a, in.a) — matches the genglsl (raw divide; spec passthrough-on-zero is not in the reference).
[[nodiscard]] inline int unpremult(KGraph& g, int c4)
{
    const int rgb = g.swizzle(c4, 0, 1, 2);
    const int a   = g.swizzle(c4, 3);
    return g.vec_concat(detail::bin(g, KOp::Div, rgb, a), a);
}
// plus/minus/difference: base op then the mix tail.
[[nodiscard]] inline int plus(KGraph& g, int fg, int bg, int mix) { return detail::mix_tail(g, detail::bin(g, KOp::Add, fg, bg), bg, mix); }
[[nodiscard]] inline int minus(KGraph& g, int fg, int bg, int mix) { return detail::mix_tail(g, detail::bin(g, KOp::Sub, bg, fg), bg, mix); }
[[nodiscard]] inline int difference(KGraph& g, int fg, int bg, int mix) { return detail::mix_tail(g, g.unary(KOp::Abs, detail::bin(g, KOp::Sub, fg, bg)), bg, mix); }
// burn (per-channel): base = (|fg|<eps) ? 0 : 1-(1-bg)/fg   (mx_burn_float, one scalar call per channel).
[[nodiscard]] inline int burn(KGraph& g, int fg, int bg, int mix)
{
    const int base = detail::per_channel(g, fg, bg, [](KGraph& gg, int f, int b) {
        const int one = detail::konst(gg, b, 1.0);
        const int val = gg.binary(KOp::Sub, one, gg.binary(KOp::Div, gg.binary(KOp::Sub, one, b), f));
        return gg.select(gg.binary(KOp::CmpLt, gg.unary(KOp::Abs, f), detail::konst(gg, f, kFloatEps)), detail::konst(gg, b, 0.0), val);
    });
    return detail::mix_tail(g, base, bg, mix);
}
// dodge (per-channel): base = (|1-fg|<eps) ? 0 : bg/(1-fg)   (mx_dodge_float, one scalar call per channel).
[[nodiscard]] inline int dodge(KGraph& g, int fg, int bg, int mix)
{
    const int base = detail::per_channel(g, fg, bg, [](KGraph& gg, int f, int b) {
        const int omf = gg.binary(KOp::Sub, detail::konst(gg, f, 1.0), f);
        return gg.select(gg.binary(KOp::CmpLt, gg.unary(KOp::Abs, omf), detail::konst(gg, f, kFloatEps)), detail::konst(gg, b, 0.0), gg.binary(KOp::Div, b, omf));
    });
    return detail::mix_tail(g, base, bg, mix);
}
// screen: base = 1 - (1-fg)*(1-bg)  (no branch — plain per-channel arithmetic).
[[nodiscard]] inline int screen(KGraph& g, int fg, int bg, int mix)
{
    const int one  = detail::konst(g, bg, 1.0);
    const int base = detail::bin(g, KOp::Sub, one, detail::bin(g, KOp::Mul, detail::bin(g, KOp::Sub, one, fg), detail::bin(g, KOp::Sub, one, bg)));
    return detail::mix_tail(g, base, bg, mix);
}
// overlay (per-channel): base = bg<0.5 ? 2*fg*bg : 1 - 2*(1-bg)*(1-fg)   (NG_overlay: bg-pivot ifgreatereq, per channel).
[[nodiscard]] inline int overlay(KGraph& g, int fg, int bg, int mix)
{
    const int base = detail::per_channel(g, fg, bg, [](KGraph& gg, int f, int b) {
        const int one   = detail::konst(gg, b, 1.0);
        const int two   = detail::konst(gg, b, 2.0);
        const int lower = gg.binary(KOp::Mul, gg.binary(KOp::Mul, f, b), two);
        const int upper = gg.binary(KOp::Sub, one, gg.binary(KOp::Mul, two, gg.binary(KOp::Mul, gg.binary(KOp::Sub, one, b), gg.binary(KOp::Sub, one, f))));
        return gg.select(gg.binary(KOp::CmpGe, b, detail::konst(gg, b, 0.5)), upper, lower);
    });
    return detail::mix_tail(g, base, bg, mix);
}
// disjointover(color4): mx_disjointover_color4 (summed-alpha compositing, branchless).
[[nodiscard]] inline int disjointover(KGraph& g, int fg, int bg, int mix)
{
    const int frgb = g.swizzle(fg, 0, 1, 2);
    const int brgb = g.swizzle(bg, 0, 1, 2);
    const int fa   = g.swizzle(fg, 3);
    const int ba   = g.swizzle(bg, 3);
    const int one  = detail::konst(g, fg, 1.0);
    const int summed = g.binary(KOp::Add, fa, ba);
    const int xfac   = g.binary(KOp::Div, g.binary(KOp::Sub, one, fa), ba);                                     // (1-fa)/ba
    const int sum_rgb = detail::bin(g, KOp::Add, frgb, brgb);                                                   // fg+bg
    const int mix_rgb = detail::bin(g, KOp::Add, frgb, detail::bin(g, KOp::Mul, brgb, g.splat(xfac, 3)));       // fg + bg*x
    const int else_rgb = detail::sel(g, g.binary(KOp::CmpLt, g.unary(KOp::Abs, ba), detail::konst(g, fg, kFloatEps)), g.splat(detail::konst(g, fg, 0.0), 3), mix_rgb);
    const int rgb    = detail::sel(g, g.binary(KOp::CmpLe, summed, one), sum_rgb, else_rgb);
    const int outa   = g.binary(KOp::Min, summed, one);
    const int out4   = g.vec_concat(rgb, outa);
    // mixval lerp against the background color4 (per the genglsl tail).
    return detail::tern(g, KOp::Mix, bg, out4, g.splat(mix, 4));
}
// Porter-Duff (color4): rgb and alpha composited by the built-in alpha; then the mix tail against bg.
namespace detail
{
[[nodiscard]] inline int porter(KGraph& g, int rgb, int a, int bg, int mix)
{
    return tern(g, KOp::Mix, bg, g.vec_concat(rgb, a), g.splat(mix, 4));
}
} // namespace detail
// over: F + B*(1-fa)   (alpha: fa + ba*(1-fa)).
[[nodiscard]] inline int over(KGraph& g, int fg, int bg, int mix)
{
    const int fa  = g.swizzle(fg, 3);
    const int omfa = detail::bin(g, KOp::Sub, detail::konst(g, fg, 1.0), fa);
    const int rgb = detail::bin(g, KOp::Add, g.swizzle(fg, 0, 1, 2), detail::bin(g, KOp::Mul, g.swizzle(bg, 0, 1, 2), omfa));
    const int a   = g.binary(KOp::Add, fa, g.binary(KOp::Mul, g.swizzle(bg, 3), omfa));
    return detail::porter(g, rgb, a, bg, mix);
}
// comp_in (MaterialX `in`): F*ba   (alpha: fa*ba).
[[nodiscard]] inline int comp_in(KGraph& g, int fg, int bg, int mix)
{
    const int ba  = g.swizzle(bg, 3);
    const int rgb = detail::bin(g, KOp::Mul, g.swizzle(fg, 0, 1, 2), ba);
    const int a   = g.binary(KOp::Mul, g.swizzle(fg, 3), ba);
    return detail::porter(g, rgb, a, bg, mix);
}
// comp_out (MaterialX `out`): F*(1-ba)   (alpha: fa*(1-ba)).
[[nodiscard]] inline int comp_out(KGraph& g, int fg, int bg, int mix)
{
    const int omba = detail::bin(g, KOp::Sub, detail::konst(g, bg, 1.0), g.swizzle(bg, 3));
    const int rgb  = detail::bin(g, KOp::Mul, g.swizzle(fg, 0, 1, 2), omba);
    const int a    = g.binary(KOp::Mul, g.swizzle(fg, 3), omba);
    return detail::porter(g, rgb, a, bg, mix);
}
// mask: B*fa   (alpha: ba*fa).
[[nodiscard]] inline int mask(KGraph& g, int fg, int bg, int mix)
{
    const int fa  = g.swizzle(fg, 3);
    const int rgb = detail::bin(g, KOp::Mul, g.swizzle(bg, 0, 1, 2), fa);
    const int a   = g.binary(KOp::Mul, g.swizzle(bg, 3), fa);
    return detail::porter(g, rgb, a, bg, mix);
}
// matte: F*fa + B*(1-fa)   (alpha: fa + ba*(1-fa)).
[[nodiscard]] inline int matte(KGraph& g, int fg, int bg, int mix)
{
    const int fa   = g.swizzle(fg, 3);
    const int omfa = detail::bin(g, KOp::Sub, detail::konst(g, fg, 1.0), fa);
    const int rgb  = detail::bin(g, KOp::Add, detail::bin(g, KOp::Mul, g.swizzle(fg, 0, 1, 2), fa), detail::bin(g, KOp::Mul, g.swizzle(bg, 0, 1, 2), omfa));
    const int a    = g.binary(KOp::Add, fa, g.binary(KOp::Mul, g.swizzle(bg, 3), omfa));
    return detail::porter(g, rgb, a, bg, mix);
}
// inside(in, mask) = in * mask ; outside(in, mask) = in * (1-mask).
[[nodiscard]] inline int inside(KGraph& g, int in_, int maskv) { return detail::bin(g, KOp::Mul, in_, maskv); }
[[nodiscard]] inline int outside(KGraph& g, int in_, int maskv) { return detail::bin(g, KOp::Mul, in_, detail::bin(g, KOp::Sub, detail::konst(g, maskv, 1.0), maskv)); }
// mix(fg, bg, mix) = bg*(1-mix) + fg*mix  (MaterialX mix; KOp::Mix(a=bg,b=fg,c=mix)).
[[nodiscard]] inline int mix(KGraph& g, int fg, int bg, int mix) { return detail::tern(g, KOp::Mix, bg, fg, mix); }

// ── CONVOLUTION nodes (MaterialX nodegroup "convolution2d") ──────────────────────────────────────────────────────────
// heighttonormal(height, scale, texcoord) — Sobel-parity screen-space normal from a heightfield (mx_heighttonormal_vector3).
// FRAGMENT-ONLY (uses dFdx/dFdy). Returns the encoded normal (n*0.5+0.5).
[[nodiscard]] inline int heighttonormal(KGraph& g, int height, int scale, int texcoord)
{
    const auto k    = [&](double v) { return detail::konst(g, height, v); };
    const int  sob  = g.binary(KOp::Mul, scale, k(1.0 / 16.0)); // scale * SOBEL_SCALE_FACTOR
    const int  dhx  = g.binary(KOp::Mul, g.dfdx(height), sob);
    const int  dhy  = g.binary(KOp::Mul, g.dfdy(height), sob);
    const int  u    = g.swizzle(texcoord, 0);
    const int  v    = g.swizzle(texcoord, 1);
    const int  tangent   = g.vec3(g.dfdx(u), g.dfdx(v), dhx);
    const int  bitangent = g.vec3(g.dfdy(u), g.dfdy(v), dhy);
    int        n         = g.cross(tangent, bitangent);
    // flip to +z hemisphere if n.z < 0 (the mirrored-uv guard; the degenerate dot<eps^2 case falls out as (0,0,1) via normalize of a near-zero cross only when authored so — kept faithful with the sign flip).
    n = g.select(g.binary(KOp::CmpLt, g.swizzle(n, 2), k(0.0)), g.binary(KOp::Mul, n, g.splat(k(-1.0), 3)), n);
    const int nn = g.normalize(n);
    return g.binary(KOp::Add, g.binary(KOp::Mul, nn, g.splat(k(0.5), 3)), g.splat(k(0.5), 3)); // *0.5+0.5
}

// ── SOURCE: SHAPES (MaterialX nodegroup "procedural2d") ──────────────────────────────────────────────────────────────
// ramplr(valuel, valuer, texcoord) = mix(valuel, valuer, clamp(texcoord.x, 0, 1)) — horizontal ramp (mx_ramplr).
[[nodiscard]] inline int ramplr(KGraph& g, int valuel, int valuer, int texcoord)
{
    return detail::tern(g, KOp::Mix, valuel, valuer, clamp01(g, g.swizzle(texcoord, 0)));
}
// ramptb(valuet, valueb, texcoord) = mix(valueb, valuet, clamp(texcoord.y, 0, 1)) — vertical ramp (mx_ramptb; b→t).
[[nodiscard]] inline int ramptb(KGraph& g, int valuet, int valueb, int texcoord)
{
    return detail::tern(g, KOp::Mix, valueb, valuet, clamp01(g, g.swizzle(texcoord, 1)));
}
// aastep(threshold, value) = smoothstep(t-afw, t+afw, value), afw = length(vec2(dFdx,dFdy))*0.7071… — antialiased step
// (mx_aastep). FRAGMENT-ONLY (screen-space derivatives).
[[nodiscard]] inline int aastep(KGraph& g, int threshold, int value)
{
    const int afw = g.binary(KOp::Mul, g.vlength(g.vec2(g.dfdx(value), g.dfdy(value))), detail::konst(g, value, 0.70710678118654757));
    const int lo  = g.binary(KOp::Sub, threshold, afw);
    const int hi  = g.binary(KOp::Add, threshold, afw);
    return g.ternary(KOp::Smoothstep, lo, hi, value);
}
// splitlr(valuel, valuer, center, texcoord) = mix(valuel, valuer, aastep(center, texcoord.x)) (mx_splitlr). FRAGMENT-ONLY.
[[nodiscard]] inline int splitlr(KGraph& g, int valuel, int valuer, int center, int texcoord)
{
    return detail::tern(g, KOp::Mix, valuel, valuer, aastep(g, center, g.swizzle(texcoord, 0)));
}
// splittb(valuet, valueb, center, texcoord) = mix(valueb, valuet, aastep(center, texcoord.y)) (mx_splittb). FRAGMENT-ONLY.
[[nodiscard]] inline int splittb(KGraph& g, int valuet, int valueb, int center, int texcoord)
{
    return detail::tern(g, KOp::Mix, valueb, valuet, aastep(g, center, g.swizzle(texcoord, 1)));
}
// checkerboard(color1, color2, uvtiling, uvoffset, texcoord): mix(color2, color1, modulo(dot(floor(texcoord*uvtiling -
// uvoffset), (1,1)), 2)) — alternating cells (NG_checkerboard). modulo is FLOOR-mod (see `modulo`).
[[nodiscard]] inline int checkerboard(KGraph& g, int color1, int color2, int uvtiling, int uvoffset, int texcoord)
{
    const int m   = detail::bin(g, KOp::Sub, detail::bin(g, KOp::Mul, texcoord, uvtiling), uvoffset);
    const int fl  = g.unary(KOp::Floor, m);
    const int d   = g.dot(fl, g.vec2(detail::konst(g, texcoord, 1.0), detail::konst(g, texcoord, 1.0))); // fl.x + fl.y
    const int md  = modulo(g, d, detail::konst(g, d, 2.0));                                              // floor-mod(d, 2)
    return detail::tern(g, KOp::Mix, color2, color1, md);                                                // color2*(1-md)+color1*md
}

// ── SOURCE: GEOMETRIC (MaterialX nodegroup "geometric") ──────────────────────────────────────────────────────────────
// Geometric nodes read INTERPOLATED per-vertex attributes (position/normal/tangent/bitangent/texcoord/geomcolor). In a
// fragment graph they are stage inputs at a convention `location`; the vertex program supplies them. Thin wrappers over
// `stage_in` — no math (their "bit-exactness" is the pass-through the interpolator already provides).
[[nodiscard]] inline int position(KGraph& g, int location) { return g.stage_in(KType::vec(DType::F32, 3), location, Interp::Smooth); }
[[nodiscard]] inline int normal(KGraph& g, int location) { return g.stage_in(KType::vec(DType::F32, 3), location, Interp::Smooth); }
[[nodiscard]] inline int tangent(KGraph& g, int location) { return g.stage_in(KType::vec(DType::F32, 3), location, Interp::Smooth); }
[[nodiscard]] inline int bitangent(KGraph& g, int location) { return g.stage_in(KType::vec(DType::F32, 3), location, Interp::Smooth); }
[[nodiscard]] inline int texcoord(KGraph& g, int location) { return g.stage_in(KType::vec(DType::F32, 2), location, Interp::Smooth); }
[[nodiscard]] inline int geomcolor(KGraph& g, int location) { return g.stage_in(KType::vec(DType::F32, 4), location, Interp::Smooth); }

// ── UV / vector transforms (MaterialX nodegroup "math" — the panner/rotator/triplanar family) ────────────────────────
// rotate2d(in, amount_deg) = vec2(ca*x + sa*y, -sa*x + ca*y), ca/sa = cos/sin(radians(amount)) (mx_rotate_vector2).
[[nodiscard]] inline int rotate2d(KGraph& g, int in2, int amount)
{
    const int rad = g.unary(KOp::Radians, amount);
    const int sa  = g.unary(KOp::Sin, rad);
    const int ca  = g.unary(KOp::Cos, rad);
    const int x   = g.swizzle(in2, 0);
    const int y   = g.swizzle(in2, 1);
    const int rx  = g.binary(KOp::Add, g.binary(KOp::Mul, ca, x), g.binary(KOp::Mul, sa, y));
    const int ry  = g.binary(KOp::Add, g.binary(KOp::Mul, g.unary(KOp::Neg, sa), x), g.binary(KOp::Mul, ca, y));
    return g.vec2(rx, ry);
}
// rotate3d(in, amount_deg, axis) — Rodrigues: in*c + cross(in, normalize(axis))*s + n*dot(n,in)*(1-c) (mx_rotate_vector3).
[[nodiscard]] inline int rotate3d(KGraph& g, int in3, int amount, int axis)
{
    const int n   = g.normalize(axis);
    const int rad = g.unary(KOp::Radians, amount);
    const int s   = g.unary(KOp::Sin, rad);
    const int c   = g.unary(KOp::Cos, rad);
    const int oc  = g.binary(KOp::Sub, detail::konst(g, amount, 1.0), c);
    const int t0  = g.binary(KOp::Mul, in3, g.splat(c, 3));                                     // in*c
    const int t1  = g.binary(KOp::Mul, g.cross(in3, n), g.splat(s, 3));                         // cross(in,n)*s
    const int t2  = g.binary(KOp::Mul, g.binary(KOp::Mul, n, g.splat(g.dot(n, in3), 3)), g.splat(oc, 3)); // n*dot(n,in)*oc
    return g.binary(KOp::Add, g.binary(KOp::Add, t0, t1), t2);
}
// place2d(texcoord, pivot, scale, rotate_deg, offset, order) — the panner/rotator UV transform (NG_place2d). order 0 = SRT
// (scale→rotate→translate), order 1 = TRS. Compile-time `order` selects the branch (the nodegraph's `switch`).
[[nodiscard]] inline int place2d(KGraph& g, int texcoord, int pivot, int scale, int rotate, int offset, int order)
{
    const int sub = detail::bin(g, KOp::Sub, texcoord, pivot); // texcoord - pivot
    if (order == 1)                                            // TRS: rotate2d((sub - offset), rot) / scale + pivot
    {
        const int r = rotate2d(g, detail::bin(g, KOp::Sub, sub, offset), rotate);
        return detail::bin(g, KOp::Add, detail::bin(g, KOp::Div, r, scale), pivot);
    }
    // SRT: rotate2d(sub / scale, rot) - offset + pivot
    const int r = rotate2d(g, detail::bin(g, KOp::Div, sub, scale), rotate);
    return detail::bin(g, KOp::Add, detail::bin(g, KOp::Sub, r, offset), pivot);
}
// triplanar_weights(normal, blend) — the blend weights for triplanar projection (NG_triplanarprojection): normalize the
// abs-normal, sharpen by pow(·, 1/clamp(blend,0.03,1)), re-normalize. Returns a vec3 summing to 1.
[[nodiscard]] inline int triplanar_weights(KGraph& g, int normal, int blend)
{
    const int ones = g.vec3(detail::konst(g, normal, 1.0), detail::konst(g, normal, 1.0), detail::konst(g, normal, 1.0));
    const int absn = g.unary(KOp::Abs, g.normalize(normal));
    const int w0   = detail::bin(g, KOp::Div, absn, g.dot(absn, ones));                                  // absN / dot(absN,1)
    const int inv  = g.binary(KOp::Div, detail::konst(g, blend, 1.0), g.ternary(KOp::Clamp, blend, detail::konst(g, blend, 0.03), detail::konst(g, blend, 1.0)));
    const int wp   = detail::bin(g, KOp::Pow, w0, inv);                                                  // pow(w0, 1/clamp)
    return detail::bin(g, KOp::Div, wp, g.dot(wp, ones));                                                // re-normalize
}
// triplanar(sample_x, sample_y, sample_z, weights) — blend three planar samples by the weights. The caller supplies the
// three texture samples (B2 sampling: position.yz / .xz / .xy projections); this is the portable IR core. The full
// texture-binding `triplanarprojection` (3 image binds + upaxis + periodic addressing) composes this with B2 at B8.
[[nodiscard]] inline int triplanar(KGraph& g, int sx, int sy, int sz, int weights)
{
    const int wx = g.swizzle(weights, 0);
    const int wy = g.swizzle(weights, 1);
    const int wz = g.swizzle(weights, 2);
    return detail::bin(g, KOp::Add, detail::bin(g, KOp::Add, detail::bin(g, KOp::Mul, sx, wx), detail::bin(g, KOp::Mul, sy, wy)), detail::bin(g, KOp::Mul, sz, wz));
}

// ── NPR (non-photorealistic rendering) nodes (MaterialX nodegroup "npr", libraries/nprlib) ───────────────────────────
// viewdirection: the view direction (a geomprop — a stage input in a fragment graph, like the geometric nodes).
[[nodiscard]] inline int viewdirection(KGraph& g, int location) { return g.stage_in(KType::vec(DType::F32, 3), location, Interp::Smooth); }
// facingratio(viewdir, normal, faceforward, invert): f = faceforward ? abs(dot) : -dot ; result = invert ? 1-f : f
// (NG_facingratio). faceforward/invert are compile-time (the nodegraph's `ifequal` switches).
[[nodiscard]] inline int facingratio(KGraph& g, int viewdir, int normal, bool faceforward, bool invert)
{
    const int d = g.dot(viewdir, normal);
    int       f = faceforward ? g.unary(KOp::Abs, d) : g.binary(KOp::Mul, d, detail::konst(g, d, -1.0)); // -dot == dot*-1
    if (invert) { f = g.binary(KOp::Sub, detail::konst(g, f, 1.0), f); }
    return f;
}
// gooch_shade(normal, viewdir, warm_color, cool_color, specular_intensity, shininess, light_direction) — Gooch warm/cool
// technical shading + a Phong specular (NG_gooch_shade). N/V are supplied by the caller (geomprops).
[[nodiscard]] inline int gooch_shade(KGraph& g, int normal, int viewdir, int warm_color, int cool_color, int specular_intensity, int shininess, int light_direction)
{
    const int nrm     = g.normalize(normal);
    const int view    = g.normalize(viewdir);
    const int light   = g.normalize(light_direction);
    const int ndotl   = g.dot(nrm, light);
    const int cool_i  = g.binary(KOp::Div, g.binary(KOp::Add, detail::konst(g, ndotl, 1.0), ndotl), detail::konst(g, ndotl, 2.0)); // (1+N·L)/2
    const int diffuse = detail::tern(g, KOp::Mix, warm_color, cool_color, cool_i);                                                 // warm*(1-ci)+cool*ci
    const int refl    = g.reflect(view, nrm);
    const int neg_l   = detail::bin(g, KOp::Mul, light, detail::konst(g, light, -1.0));
    const int vdotr   = g.binary(KOp::Max, g.dot(neg_l, refl), detail::konst(g, ndotl, 0.0));
    const int spec    = g.binary(KOp::Mul, g.binary(KOp::Pow, vdotr, shininess), specular_intensity);
    return detail::bin(g, KOp::Add, diffuse, spec); // color3 + float (broadcast)
}

} // namespace crd::kir::nodes
