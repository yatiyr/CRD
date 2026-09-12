#pragma once

// ckir_taa.hpp — B13-a: TEMPORAL antialiasing + upscale + frame generation, each a backend-neutral CKIR resolve pass. The
// temporal spine of the renderer: a jittered camera accumulates sub-pixel samples across frames, reprojected along the
// motion-vector G-buffer into a history buffer, rectified against the current neighborhood (so ghosting/streaking don't
// survive), and blended. Everything here is the per-pixel RESOLVE MATH — bit-exact + testable now.
//
// SCOPE (the analytic core, buildable now): the YCoCg clamp space, the history AABB / variance rectification (Karis +
// Salvi), the sharp Catmull-Rom history weights, the luma-driven anti-flicker feedback, the disocclusion reject, the
// temporal blue-noise dither, the frame-gen interpolation blend, and the SMAA luma-edge core.
//
// RENDERER LEAVES (deferred — they need bound resources / external SDKs, not new IR): the 3×3 neighborhood GATHER + the
// motion-vector reprojection SAMPLE (texture taps over the history + MV G-buffer); the common ML-UPSCALER SEAM backing
// DLSS 4 / FSR 4 / XeSS (external SDK device path) with this TAAU as the portable fallback + our OWN learned upscaler via
// B10 cooperative-vectors; frame generation's REQUIRED low-latency partner (a Reflex-class driver seam) + the MV-warped
// motion sample; the blue-noise + SMAA area/search textures (B2 asset binds); the history ping-pong orchestration. Every
// transform those leaves invoke is complete here.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_nodes.hpp> // nodes::clamp / clamp01 / detail::bin (broadcast-safe vec·scalar)

namespace crd::kir::taa
{

namespace detail
{
// kf — a scalar constant shaped like `like` (a tuning constant; nodes::detail::bin() broadcasts it across a vector operand).
[[nodiscard]] inline int kf(KGraph& g, int like, double v) { return g.constant(v, g.node(like).shape, g.node(like).dtype()); }
} // namespace detail

// ── YCoCg clamp space ────────────────────────────────────────────────────────────────────────────────────────────────────
// History rectification clamps in YCoCg, not RGB: luma/chroma separation makes the neighborhood AABB far tighter (kills the
// purple/green fringing RGB clamping leaves behind). rgb→ycocg and back are the exact lifting transform (bit-reversible in F64).
//   Y = ¼R + ½G + ¼B ;  Co = ½R − ½B ;  Cg = −¼R + ½G − ¼B

[[nodiscard]] inline int rgb_to_ycocg(KGraph& g, int rgb)
{
    const auto k  = [&](double v) { return detail::kf(g, rgb, v); };
    const int  yy = g.dot(rgb, g.vec3(k(0.25), k(0.5), k(0.25)));
    const int  co = g.dot(rgb, g.vec3(k(0.5), k(0.0), k(-0.5)));
    const int  cg = g.dot(rgb, g.vec3(k(-0.25), k(0.5), k(-0.25)));
    return g.vec3(yy, co, cg);
}

[[nodiscard]] inline int ycocg_to_rgb(KGraph& g, int ycocg)
{
    const int yy = g.swizzle(ycocg, 0);
    const int co = g.swizzle(ycocg, 1);
    const int cg = g.swizzle(ycocg, 2);
    const int rr = g.binary(KOp::Sub, g.binary(KOp::Add, yy, co), cg); // Y + Co − Cg
    const int gg = g.binary(KOp::Add, yy, cg);                         // Y + Cg
    const int bb = g.binary(KOp::Sub, g.binary(KOp::Sub, yy, co), cg); // Y − Co − Cg
    return g.vec3(rr, gg, bb);
}

// ── history rectification ────────────────────────────────────────────────────────────────────────────────────────────────

// clip_aabb — the Karis/Playdead history clip: pull `history` to the surface of the neighborhood colour box
// [aabb_min, aabb_max] ALONG the ray toward the box centre (a clip, not a clamp — preserves hue, only shortens the vector).
// Branchless: `centre + v / max(maxRatio, 1)` returns `history` unchanged when it is already inside (ratio ≤ 1 ⇒ ÷1).
[[nodiscard]] inline int clip_aabb(KGraph& g, int history, int aabb_min, int aabb_max)
{
    namespace nd     = nodes;
    const auto k     = [&](double v) { return detail::kf(g, history, v); };
    const int  centre = nd::detail::bin(g, KOp::Mul, g.binary(KOp::Add, aabb_max, aabb_min), k(0.5)); // ½(max+min)
    const int  extent = nd::detail::bin(g, KOp::Mul, g.binary(KOp::Sub, aabb_max, aabb_min), k(0.5)); // ½(max−min)
    const int  v      = g.binary(KOp::Sub, history, centre);                                          // history − centre
    // per-channel ratio |v| / (extent + ε); ε keeps a degenerate (zero-extent) axis from dividing by zero.
    const int  ratio  = g.binary(KOp::Div, g.unary(KOp::Abs, v), nd::detail::bin(g, KOp::Add, extent, k(1.0e-7)));
    const int  maxr   = g.binary(KOp::Max, g.swizzle(ratio, 0), g.binary(KOp::Max, g.swizzle(ratio, 1), g.swizzle(ratio, 2)));
    const int  denom  = g.binary(KOp::Max, maxr, k(1.0));                                             // scalar ≥ 1
    return g.binary(KOp::Add, centre, nd::detail::bin(g, KOp::Div, v, denom));                        // centre + v/denom
}

// variance_clip — Salvi's variance clipping: build the clamp box from the neighborhood colour MOMENTS (m1 = mean, m2 = mean
// of squares, both per channel over the 3×3 tap — the gather is renderer-side) as `mean ± γ·σ`, σ = √(m2 − m1²), then
// clip_aabb the history into it. Tighter + cheaper than a min/max box, and rejects fireflies. γ ≈ 1.0.
[[nodiscard]] inline int variance_clip(KGraph& g, int history, int m1, int m2, double gamma)
{
    namespace nd     = nodes;
    const int  var   = nd::detail::bin(g, KOp::Max, g.binary(KOp::Sub, m2, g.binary(KOp::Mul, m1, m1)), detail::kf(g, m1, 0.0)); // max(m2−m1²,0)
    const int  gsig  = nd::detail::bin(g, KOp::Mul, g.unary(KOp::Sqrt, var), detail::kf(g, m1, gamma));                          // γ·σ
    return clip_aabb(g, history, g.binary(KOp::Sub, m1, gsig), g.binary(KOp::Add, m1, gsig));
}

// catmull_rom_weights — the four 1-D Catmull-Rom cubic weights for a fractional sample position `t` ∈ [0,1]. Resampling the
// history with a Catmull-Rom (bicubic, negative-lobe) kernel instead of bilinear is what keeps a reprojected image SHARP
// under motion (bilinear history softens every frame). Returned packed as vec4 (w₋₁, w₀, w₁, w₂); Σw = 1.
[[nodiscard]] inline int catmull_rom_weights(KGraph& g, int t)
{
    const auto k  = [&](double v) { return detail::kf(g, t, v); };
    const int  t2 = g.binary(KOp::Mul, t, t);
    const int  t3 = g.binary(KOp::Mul, t2, t);
    // w₋₁ = −½t + t² − ½t³ ; w₀ = 1 − 5⁄2 t² + 3⁄2 t³ ; w₁ = ½t + 2t² − 3⁄2 t³ ; w₂ = −½t² + ½t³
    const int  wm = g.binary(KOp::Add, g.binary(KOp::Sub, g.binary(KOp::Mul, k(-0.5), t), g.binary(KOp::Mul, k(0.5), t3)), t2);
    const int  w0 = g.binary(KOp::Add, g.binary(KOp::Sub, k(1.0), g.binary(KOp::Mul, k(2.5), t2)), g.binary(KOp::Mul, k(1.5), t3));
    const int  w1 = g.binary(KOp::Sub, g.binary(KOp::Add, g.binary(KOp::Mul, k(0.5), t), g.binary(KOp::Mul, k(2.0), t2)), g.binary(KOp::Mul, k(1.5), t3));
    const int  w2 = g.binary(KOp::Add, g.binary(KOp::Mul, k(-0.5), t2), g.binary(KOp::Mul, k(0.5), t3));
    return g.vec4(wm, w0, w1, w2);
}

// ── blend + rejection ────────────────────────────────────────────────────────────────────────────────────────────────────

// luma_feedback — Karis' luma-difference anti-flicker: the exponential-blend feedback weight is modulated by how much the
// history and current LUMA disagree. Steady pixels lean on history (min_alpha ⇒ maximal accumulation / AA); a pixel whose
// luma jumped leans on the current sample (max_alpha ⇒ less ghost). factor = clamp01(|Δluma| / max(hl,cl,ε)).
[[nodiscard]] inline int luma_feedback(KGraph& g, int hist_luma, int cur_luma, double min_alpha, double max_alpha)
{
    const auto k    = [&](double v) { return detail::kf(g, cur_luma, v); };
    const int  diff = g.unary(KOp::Abs, g.binary(KOp::Sub, hist_luma, cur_luma));
    const int  den  = g.binary(KOp::Max, g.binary(KOp::Max, hist_luma, cur_luma), k(1.0e-5));
    const int  fac  = nodes::clamp01(g, g.binary(KOp::Div, diff, den));
    return g.binary(KOp::Add, k(min_alpha), g.binary(KOp::Mul, k(max_alpha - min_alpha), fac)); // mix(min,max,fac)
}

// taa_resolve — the exponential history blend: mix(history, current, alpha). `alpha` is the (per-pixel, luma_feedback-driven)
// current-sample weight; a small alpha (≈0.05) = long accumulation, strong AA; a large alpha = fast, ghost-free.
[[nodiscard]] inline int taa_resolve(KGraph& g, int history, int current, int alpha)
{
    return g.ternary(KOp::Mix, history, current, g.splat(alpha, 3));
}

// disocclusion — the history REJECT weight from the reprojected-vs-current DEPTH mismatch: 1 where depths agree (trust the
// history), falling to 0 as |Δdepth| exceeds `threshold` (a newly-revealed surface — its history is garbage). Multiply the
// blend alpha's history term by this (or force alpha→1) to kill reprojection ghosts at silhouettes.
[[nodiscard]] inline int disocclusion(KGraph& g, int prev_depth, int cur_depth, double threshold)
{
    const auto k    = [&](double v) { return detail::kf(g, cur_depth, v); };
    const int  diff = g.unary(KOp::Abs, g.binary(KOp::Sub, prev_depth, cur_depth));
    return nodes::clamp01(g, g.binary(KOp::Sub, k(1.0), g.binary(KOp::Div, diff, k(threshold))));
}

// ── dither + frame generation + SMAA ─────────────────────────────────────────────────────────────────────────────────────

// ign_temporal — the Jimenez interleaved-gradient noise, TEMPORALLY scrambled: a cheap [0,1) hash of FragCoord offset per
// frame (×5.588238 golden-ish shift) so the dither pattern animates and averages out under TAA instead of standing still.
// Feeds deband (below) and any per-pixel stochastic offset (kernel rotation, stochastic transparency).
[[nodiscard]] inline int ign_temporal(KGraph& g, int frag_xy, int frame)
{
    const auto k   = [&](int like, double v) { return detail::kf(g, like, v); };
    const int  fx  = g.binary(KOp::Add, g.swizzle(frag_xy, 0), g.binary(KOp::Mul, k(frame, 5.588238), frame)); // x + 5.588238·frame
    const int  fy  = g.swizzle(frag_xy, 1);
    const int  d   = g.binary(KOp::Add, g.binary(KOp::Mul, fx, k(fx, 0.06711056)), g.binary(KOp::Mul, fy, k(fy, 0.00583715)));
    return g.unary(KOp::Fract, g.binary(KOp::Mul, k(d, 52.9829189), g.unary(KOp::Fract, d)));
}

// dither_apply — deband before the final N-bit quantization: nudge the colour by a sub-quantum of triangular/blue noise so a
// smooth gradient dithers instead of showing 8-bit banding. `noise` ∈ [0,1) (ign_temporal or a blue-noise tap); `levels` =
// 2^bits − 1 of the output. Adds ±½ LSB of noise.
[[nodiscard]] inline int dither_apply(KGraph& g, int color, int noise, double levels)
{
    const int amt = g.binary(KOp::Div, g.binary(KOp::Sub, noise, detail::kf(g, noise, 0.5)), detail::kf(g, noise, levels));
    return nodes::detail::bin(g, KOp::Add, color, amt); // color + (noise−½)/levels
}

// frame_gen_blend — the frame-generation interpolation core: a motion-compensated blend of the previous and next real frames
// at phase `t` ∈ (0,1) to synthesize an in-between frame. `prev`/`next` are the two frames sampled at the MV-warped position
// (the warp + disocclusion hole-fill is renderer-side); this is the temporal lerp mix(prev, next, t).
[[nodiscard]] inline int frame_gen_blend(KGraph& g, int prev, int next, int t)
{
    return g.ternary(KOp::Mix, prev, next, g.splat(t, 3));
}

// smaa_luma_edge — the SMAA (Jimenez MLAA-successor) edge-detection pass core: mark a left/top boundary where the luma
// contrast to the neighbor exceeds `threshold`. Returned as vec2 (left_edge, top_edge) ∈ {0,1}. The local-contrast
// adaptation second pass + the area/search-texture blend-weight lookup are the renderer leaf (B2 LUT binds).
[[nodiscard]] inline int smaa_luma_edge(KGraph& g, int luma_c, int luma_left, int luma_top, double threshold)
{
    const auto k  = [&](double v) { return detail::kf(g, luma_c, v); };
    const int  el = g.binary(KOp::Step, k(threshold), g.unary(KOp::Abs, g.binary(KOp::Sub, luma_c, luma_left)));
    const int  et = g.binary(KOp::Step, k(threshold), g.unary(KOp::Abs, g.binary(KOp::Sub, luma_c, luma_top)));
    return g.vec2(el, et);
}

} // namespace crd::kir::taa
