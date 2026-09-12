#pragma once

// ckir_post.hpp — B13: the POST-PROCESSING frontier (HDR → display, the beautiful finish), each a backend-neutral CKIR
// compute / full-screen pass. B13-c EXPOSURE + TONEMAP + HDR OUTPUT (the HDR core): histogram auto-exposure → EV100
// (Frostbite), the tonemap/"look" DECOUPLED from the HDR output — AgX (filmic) + Khronos PBR-Neutral (base-colour accuracy)
// as the built-in ANALYTIC looks (Tony McMapface + ACES 2.0 are baked 3D-LUTs sampled via the B2 Tex3D path), + the output
// encodes: sRGB (SDR) and PQ / ST.2084 (HDR10, BT.2408 paper-white). Explicitly NOT the per-channel Narkowicz-"ACES" hue-skew.
//
// SCOPE: every per-pixel exposure/tonemap/encode transform is here + testable now. The histogram REDUCTION that yields the
// average luminance (a parallel reduce), the eye-adaptation temporal blend, and the 3D-LUT texture bind are the renderer leaf
// (B-compute reduction + B2 Tex3D); the curves they feed are complete here.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_nodes.hpp> // nodes::clamp / clamp01 / detail::bin (broadcast-safe vec·scalar)

namespace crd::kir::post
{

namespace detail
{
[[nodiscard]] inline int kf(KGraph& g, int like, double v) { return g.constant(v, g.node(like).shape, g.node(like).dtype()); }
// ⛔⛔ RAF-10: a POST color op receives whatever the graph feeds it, and a post graph pipes the SAMPLER result straight
// in — so `color` is a SAMPLED vec4, not a vec3. A display transform is an RGB operation, so drop the alpha lane ONCE
// here (rather than at every call site). Without this, an op's internal vec3 math (a hardcoded `g.vec3(...)`, a `dot`
// against a vec3 coeff) collides with a vec4 operand and lowers to an invalid `mix(vec4, vec3, …)` / `dot(vec4, vec3)`
// — the shader will not compile and `create_program` returns null, while the CPU oracle stays green (the classic
// emitter-vs-oracle gap: a valid graph is NOT a valid shader — see the raster-emitter-lag scar family).
[[nodiscard]] inline int rgb3(KGraph& g, int color)
{
    return g.node(color).comps() >= 4
               ? g.vec3(g.vec_comp(color, 0), g.vec_comp(color, 1), g.vec_comp(color, 2))
               : color;
}
} // namespace detail

// ── B13-c: EXPOSURE (Frostbite EV100) ────────────────────────────────────────────────────────────────────────────────────

// ev100_from_luminance — the exposure value (ISO 100) for a scene's average luminance (Frostbite "Moving Frostbite to PBR"):
// `EV100 = log2(avgLum · 100/12.5)`. 12.5 = the reflected-light meter calibration constant.
[[nodiscard]] inline int ev100_from_luminance(KGraph& g, int avg_lum)
{
    return g.unary(KOp::Log2, g.binary(KOp::Mul, avg_lum, detail::kf(g, avg_lum, 100.0 / 12.5)));
}
// exposure_from_ev100 — the linear exposure multiplier the scene is scaled by before tonemapping: `1 / (1.2 · 2^EV100)`.
[[nodiscard]] inline int exposure_from_ev100(KGraph& g, int ev100)
{
    return g.binary(KOp::Div, detail::kf(g, ev100, 1.0), g.binary(KOp::Mul, detail::kf(g, ev100, 1.2), g.unary(KOp::Exp2, ev100)));
}

// ── B13-c: TONEMAP "looks" (analytic; decoupled from the output encode) ───────────────────────────────────────────────────

// agx — the AgX filmic tonemap (Troy Sobotka; Filament/Godot fit). Rotate into the AgX inset space → log2-encode over
// [−12.47, 4.03] EV → the 6th-order sigmoid contrast curve. Returns the display-encoded [0,1] look. Hue-preserving, no skew.
namespace agx_detail
{
[[nodiscard]] inline int contrast(KGraph& g, int x)
{
    namespace nd  = nodes;
    const auto k  = [&](double v) { return detail::kf(g, x, v); }; // scalar consts → bin() broadcasts them across the vec
    const int  x2 = g.binary(KOp::Mul, x, x);
    const int  x4 = g.binary(KOp::Mul, x2, x2);
    const int  x6 = g.binary(KOp::Mul, x4, x2);
    // 15.5·x⁶ − 40.14·x⁵ + 31.96·x⁴ − 6.868·x³ + 0.4298·x² + 0.1191·x − 0.00232  (per channel)
    int r = nd::detail::bin(g, KOp::Mul, x6, k(15.5));
    r     = g.binary(KOp::Sub, r, nd::detail::bin(g, KOp::Mul, g.binary(KOp::Mul, x4, x), k(40.14)));
    r     = g.binary(KOp::Add, r, nd::detail::bin(g, KOp::Mul, x4, k(31.96)));
    r     = g.binary(KOp::Sub, r, nd::detail::bin(g, KOp::Mul, g.binary(KOp::Mul, x2, x), k(6.868)));
    r     = g.binary(KOp::Add, r, nd::detail::bin(g, KOp::Mul, x2, k(0.4298)));
    r     = g.binary(KOp::Add, r, nd::detail::bin(g, KOp::Mul, x, k(0.1191)));
    return nd::detail::bin(g, KOp::Sub, r, k(0.00232));
}
} // namespace agx_detail

[[nodiscard]] inline int agx(KGraph& g, int color)
{
    const auto k     = [&](double v) { return detail::kf(g, color, v); };
    namespace nd     = nodes;
    // 38-G1 / RAF-10: accept a SAMPLED vec4 too (a post graph feeds the sampler result straight in). AgX is an RGB
    // transform, so the alpha lane is dropped here rather than at every call site — the shared post-color guard.
    const int  rgb   = detail::rgb3(g, color);
    // AgX inset matrix as ROW dot products — identical math to the mat3 multiply, in ops every stage lowers
    // (the raster FS emitter has no MatVecMul arm; a display transform must run exactly there).
    const auto kk    = [&](double v) { return detail::kf(g, rgb, v); };
    const int  row0  = g.vec3(kk(0.842479062253094), kk(0.0423282422610123), kk(0.0423756549057051));
    const int  row1  = g.vec3(kk(0.0784335999999992), kk(0.878468636469772), kk(0.0784336));
    const int  row2  = g.vec3(kk(0.0792237451477643), kk(0.0791661274605434), kk(0.879142973793104));
    const int  v     = g.vec3(g.dot(rgb, row0), g.dot(rgb, row1), g.dot(rgb, row2));
    const int  vpos  = nd::detail::bin(g, KOp::Max, v, k(1.0e-10));             // avoid log2(0)
    const int  lg    = nodes::clamp(g, g.unary(KOp::Log2, vpos), k(-12.47393), k(4.026069));
    const int  norm  = nd::detail::bin(g, KOp::Div, nd::detail::bin(g, KOp::Sub, lg, k(-12.47393)), k(4.026069 - (-12.47393))); // (lg−min)/(max−min)
    const int  sig   = agx_detail::contrast(g, norm);
    // ⛔⛔ THE OUTSET — the half everyone forgets. AgX INSETS toward the achromatic axis before the log+
    // sigmoid (that is what tames the hue shifts), and the OUTSET is the inverse that puts the saturation
    // BACK. Without it every frame comes out visibly washed out — the user's first words on seeing it live
    // were "the hdr is desaturating a lot", which is exactly the missing-outset signature and not a taste
    // question. Filament's AgX outset (the inverse of the inset above), as row dot products.
    const int  orow0 = g.vec3(kk(1.19687900512017), kk(-0.0528968517574562), kk(-0.0529716355144438));
    const int  orow1 = g.vec3(kk(-0.0980208811401368), kk(1.15190312990417), kk(-0.0980434501171241));
    const int  orow2 = g.vec3(kk(-0.0990297440797205), kk(-0.0989611768448433), kk(1.15107367264116));
    const int  outs  = g.vec3(g.dot(sig, orow0), g.dot(sig, orow1), g.dot(sig, orow2));
    // the outset can push a channel slightly negative on saturated inputs; clamp before the OETF that follows
    return nodes::clamp(g, outs, k(0.0), k(1.0));
}

// pbr_neutral — the Khronos PBR Neutral tone mapper (base-colour accuracy: preserves saturated hues, compresses only the
// highlights). The published reference, branchless via `Select` (the branch cond is a per-pixel scalar).
[[nodiscard]] inline int pbr_neutral(KGraph& g, int color)
{
    namespace nd  = nodes;
    const auto ks = [&](int like, double v) { return detail::kf(g, like, v); };
    const int  rgb = detail::rgb3(g, color);                             // ⛔ RAF-10: drop a sampled vec4's alpha to vec3
    const int  r   = g.swizzle(rgb, 0);
    const int  gc  = g.swizzle(rgb, 1);
    const int  b   = g.swizzle(rgb, 2);
    const int  x   = g.binary(KOp::Min, r, g.binary(KOp::Min, gc, b));   // min channel (scalar)
    // offset = x < 0.08 ? x − 6.25·x² : 0.04
    const int  offset = g.select(g.binary(KOp::CmpLt, x, ks(x, 0.08)), g.binary(KOp::Sub, x, g.binary(KOp::Mul, ks(x, 6.25), g.binary(KOp::Mul, x, x))), ks(x, 0.04));
    const int  col1   = nd::detail::bin(g, KOp::Sub, rgb, offset);       // rgb − offset (vec3 − scalar)
    const int  peak   = g.binary(KOp::Max, g.swizzle(col1, 0), g.binary(KOp::Max, g.swizzle(col1, 1), g.swizzle(col1, 2)));
    const double start_c = 0.8 - 0.04; const double dd = 1.0 - start_c;    // 0.76, 0.24
    // new_peak = 1 − d²/(peak + d − start_c)
    const int  new_peak = g.binary(KOp::Sub, ks(peak, 1.0), g.binary(KOp::Div, ks(peak, dd * dd), g.binary(KOp::Sub, g.binary(KOp::Add, peak, ks(peak, dd)), ks(peak, start_c))));
    const int  scaled  = nd::detail::bin(g, KOp::Mul, col1, g.binary(KOp::Div, new_peak, peak)); // color · new_peak/peak
    // g = 1 − 1/(0.15·(peak − new_peak) + 1)
    const int  gg      = g.binary(KOp::Sub, ks(peak, 1.0), g.binary(KOp::Div, ks(peak, 1.0), g.binary(KOp::Add, g.binary(KOp::Mul, ks(peak, 0.15), g.binary(KOp::Sub, peak, new_peak)), ks(peak, 1.0))));
    const int  npv     = g.vec3(new_peak, new_peak, new_peak);
    const int  compressed = g.ternary(KOp::Mix, scaled, npv, g.splat(gg, 3)); // mix(scaled, new_peak, g)
    return g.select(g.binary(KOp::CmpLt, peak, ks(peak, start_c)), col1, compressed);
}

// ── B13-c: OUTPUT ENCODES ────────────────────────────────────────────────────────────────────────────────────────────────

// srgb_encode — the sRGB OETF (linear → display), the SDR output transform. Per channel: `x≤0.0031308 ? 12.92x : 1.055·x^(1/2.4)−0.055`.
[[nodiscard]] inline int srgb_encode(KGraph& g, int color)
{
    const auto k  = [&](int like, double v) { return detail::kf(g, like, v); };
    // PER CHANNEL: the branch is a scalar `Select` cond (a vec3 cond would misalign — `Select` reads a scalar cond).
    const auto ch = [&](int c) {
        const int x  = g.swizzle(color, c);
        const int lo = g.binary(KOp::Mul, x, k(x, 12.92));
        const int hi = g.binary(KOp::Sub, g.binary(KOp::Mul, g.binary(KOp::Pow, x, k(x, 1.0 / 2.4)), k(x, 1.055)), k(x, 0.055));
        return g.select(g.binary(KOp::CmpLe, x, k(x, 0.0031308)), lo, hi);
    };
    return g.vec3(ch(0), ch(1), ch(2));
}

// pq_encode — the SMPTE ST.2084 (PQ) EOTF⁻¹ for HDR10 output. `L` is the display luminance normalized to 10000 nits.
//   Lp = L^m1 ;  PQ = ((c1 + c2·Lp)/(1 + c3·Lp))^m2
[[nodiscard]] inline int pq_encode(KGraph& g, int l)
{
    const auto k   = [&](double v) { return detail::kf(g, l, v); };
    const int  lp  = g.binary(KOp::Pow, l, k(0.1593017578125));                      // m1
    const int  num = g.binary(KOp::Add, k(0.8359375), g.binary(KOp::Mul, k(18.8515625), lp)); // c1 + c2·Lp
    const int  den = g.binary(KOp::Add, k(1.0), g.binary(KOp::Mul, k(18.6875), lp));          // 1 + c3·Lp
    return g.binary(KOp::Pow, g.binary(KOp::Div, num, den), k(78.84375));             // ^m2
}

// gamut_compress — a simple perceptual gamut compression: pull out-of-[0,1] colours toward their luminance (desaturate the
// over-bright), so wide-gamut / bloomed highlights map into the display gamut without hard clipping. `amount` ∈ [0,1].
[[nodiscard]] inline int gamut_compress(KGraph& g, int color, int amount)
{
    namespace nd    = nodes;
    const auto k    = [&](int like, double v) { return detail::kf(g, like, v); };
    const int  rgb  = detail::rgb3(g, color);                            // ⛔ RAF-10: drop a sampled vec4's alpha to vec3
    const int  luma = g.dot(rgb, g.vec3(k(rgb, 0.2126), k(rgb, 0.7152), k(rgb, 0.0722))); // Rec.709 luma (scalar)
    const int  peak = g.binary(KOp::Max, g.swizzle(rgb, 0), g.binary(KOp::Max, g.swizzle(rgb, 1), g.swizzle(rgb, 2)));
    const int  over = nodes::clamp01(g, g.binary(KOp::Mul, g.binary(KOp::Sub, peak, k(peak, 1.0)), amount)); // how far over gamut, scaled
    return nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, rgb, g.binary(KOp::Sub, k(over, 1.0), over)), nd::detail::bin(g, KOp::Mul, g.splat(luma, 3), over)); // mix(rgb, luma, over)
}

} // namespace crd::kir::post
