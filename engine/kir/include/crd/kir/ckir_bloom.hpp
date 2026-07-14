#pragma once

// ckir_bloom.hpp — B13-b: BLOOM + light emission, the HDR glow that makes emissive / neon / LED / sci-fi surfaces shine.
// The frontier bloom is a physically-based DUAL-FILTER pyramid (Jimenez "Next Generation Post Processing in Call of Duty:
// Advanced Warfare"): a Karis-weighted 13-tap downsample chain (firefly-free, energy-conserving, threshold-free) and a
// 3×3-tent upsample chain, composited back over the scene. Plus the FFT-convolution glare's frequency-domain core, and the
// lens-flare / starburst analytic pieces.
//
// SCOPE (buildable now): the 13-tap downsample WEIGHTED COMBINE (+ its Karis firefly-suppressed first-mip variant), the
// soft-knee prefilter, the tent-upsample combine, the scene composite, the FFT-convolution per-frequency complex multiply,
// and the lens-flare halo ring / aperture starburst / spectral-ghost tint.
//
// RENDERER LEAVES (deferred — bound resources / a compute primitive, not new IR): the pyramid TAP GATHER (the 13/9 bilinear
// texture samples per mip) + the mip-chain render-target ping-pong; the **FFT forward/inverse GPU transform** for the
// convolution-bloom path (a B-compute kernel — its per-frequency multiply `complex_mul` is here, the transform is the dep);
// the lens-dirt + aperture textures (B2 binds); the ghost SAMPLE along the flare vector. Every per-pixel transform is here.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_nodes.hpp> // nodes::clamp / clamp01 / detail::bin (broadcast-safe vec·scalar)

namespace crd::kir::bloom
{

namespace detail
{
[[nodiscard]] inline int kf(KGraph& g, int like, double v) { return g.constant(v, g.node(like).shape, g.node(like).dtype()); }
// luma — Rec.709 relative luminance (scalar) of an RGB node, the firefly/brightness measure the Karis weighting keys off.
[[nodiscard]] inline int luma(KGraph& g, int color) { return g.dot(color, g.vec3(kf(g, color, 0.2126), kf(g, color, 0.7152), kf(g, color, 0.0722))); }
// karis_weight — the firefly-suppression weight 1/(1+luma): a super-bright tap is down-weighted so a single hot pixel can't
// bloom the whole neighborhood (the reason the first downsample uses a weighted, not plain, average).
[[nodiscard]] inline int karis_weight(KGraph& g, int color) { return g.binary(KOp::Div, kf(g, color, 1.0), g.binary(KOp::Add, kf(g, color, 1.0), luma(g, color))); }
} // namespace detail

// ── B13-b: the dual-filter pyramid ───────────────────────────────────────────────────────────────────────────────────────

// downsample_13tap — the Jimenez 13-tap dual-filter downsample WEIGHTED COMBINE. `t` = the 13 bilinear taps: t[0..8] the 3×3
// grid at ±1 texel (t[4] = centre), t[9..12] the inner 2×2 box at ±0.5 texel. Five overlapping 2×2 blocks, the centre block
// weighted ½ and the four outer blocks ⅛ each (Σ = 1) — a wide, stable, alias-free downsample.
[[nodiscard]] inline int downsample_13tap(KGraph& g, const int* t)
{
    namespace nd    = nodes;
    const auto q    = [&](int a, int b, int c, int d) { return nd::detail::bin(g, KOp::Mul, g.binary(KOp::Add, g.binary(KOp::Add, a, b), g.binary(KOp::Add, c, d)), detail::kf(g, a, 0.25)); };
    const int  inner = q(t[9], t[10], t[11], t[12]);           // centre block (the ±0.5 inner box)
    const int  tl    = q(t[0], t[1], t[3], t[4]);              // outer blocks over the 3×3 grid
    const int  tr    = q(t[1], t[2], t[4], t[5]);
    const int  bl    = q(t[3], t[4], t[6], t[7]);
    const int  br    = q(t[4], t[5], t[7], t[8]);
    const int  outer = g.binary(KOp::Add, g.binary(KOp::Add, tl, tr), g.binary(KOp::Add, bl, br));
    return g.binary(KOp::Add, nd::detail::bin(g, KOp::Mul, inner, detail::kf(g, inner, 0.5)), nd::detail::bin(g, KOp::Mul, outer, detail::kf(g, outer, 0.125)));
}

// downsample_karis — the FIRST-mip downsample with per-block Karis luma weighting: each of the five blocks is weighted by
// 1/(1+luma(block)) × its pyramid weight, then normalized. This is what makes the bloom firefly-free / threshold-free — a
// lone hot sample can't dominate. (Later mips use the plain downsample_13tap; the fireflies are already gone.)
[[nodiscard]] inline int downsample_karis(KGraph& g, const int* t)
{
    namespace nd  = nodes;
    const auto q  = [&](int a, int b, int c, int d) { return nd::detail::bin(g, KOp::Mul, g.binary(KOp::Add, g.binary(KOp::Add, a, b), g.binary(KOp::Add, c, d)), detail::kf(g, a, 0.25)); };
    const int  b0 = q(t[9], t[10], t[11], t[12]); const int b1 = q(t[0], t[1], t[3], t[4]);
    const int  b2 = q(t[1], t[2], t[4], t[5]);     const int b3 = q(t[3], t[4], t[6], t[7]); const int b4 = q(t[4], t[5], t[7], t[8]);
    const auto w  = [&](int blk, double bw) { return g.binary(KOp::Mul, detail::karis_weight(g, blk), detail::kf(g, blk, bw)); };
    const int  w0 = w(b0, 0.5); const int w1 = w(b1, 0.125); const int w2 = w(b2, 0.125); const int w3 = w(b3, 0.125); const int w4 = w(b4, 0.125);
    const int  num = g.binary(KOp::Add, g.binary(KOp::Add, g.binary(KOp::Add, nd::detail::bin(g, KOp::Mul, b0, w0), nd::detail::bin(g, KOp::Mul, b1, w1)), g.binary(KOp::Add, nd::detail::bin(g, KOp::Mul, b2, w2), nd::detail::bin(g, KOp::Mul, b3, w3))), nd::detail::bin(g, KOp::Mul, b4, w4));
    const int  den = g.binary(KOp::Add, g.binary(KOp::Add, g.binary(KOp::Add, w0, w1), g.binary(KOp::Add, w2, w3)), w4);
    return nd::detail::bin(g, KOp::Div, num, g.binary(KOp::Add, den, detail::kf(g, den, 1.0e-5)));
}

// soft_knee — the optional Jimenez quadratic soft-knee prefilter (for a thresholded bloom look): below `threshold` no bloom,
// above it full, with a smooth `knee`-wide quadratic ramp across the boundary (no hard edge). Returns `color` scaled by its
// bloom contribution. `threshold`/`knee` are tuning constants.
[[nodiscard]] inline int soft_knee(KGraph& g, int color, double threshold, double knee)
{
    namespace nd    = nodes;
    const auto k    = [&](double v) { return detail::kf(g, color, v); };
    const int  br   = g.binary(KOp::Max, g.swizzle(color, 0), g.binary(KOp::Max, g.swizzle(color, 1), g.swizzle(color, 2))); // brightness
    const int  soft = nodes::clamp(g, g.binary(KOp::Sub, g.binary(KOp::Add, br, k(knee)), k(threshold)), k(0.0), k(2.0 * knee)); // clamp(br−thr+knee, 0, 2·knee)
    const int  sq   = g.binary(KOp::Div, g.binary(KOp::Mul, soft, soft), k(4.0 * knee + 1.0e-5));                              // soft²/(4·knee)
    const int  contrib = g.binary(KOp::Div, g.binary(KOp::Max, sq, g.binary(KOp::Sub, br, k(threshold))), g.binary(KOp::Max, br, k(1.0e-5)));
    return nd::detail::bin(g, KOp::Mul, color, contrib);
}

// upsample_tent — the 3×3 tent (bilinear) upsample combine used on the way back UP the pyramid. `t` = the 9 taps of a 3×3
// neighborhood (t[4] = centre); weights (1 2 1 / 2 4 2 / 1 2 1)/16 — the smooth tent that gives bloom its soft, wide falloff.
[[nodiscard]] inline int upsample_tent(KGraph& g, const int* t)
{
    namespace nd    = nodes;
    const auto w2   = [&](int a) { return nd::detail::bin(g, KOp::Mul, a, detail::kf(g, a, 2.0)); };
    const int  corners = g.binary(KOp::Add, g.binary(KOp::Add, t[0], t[2]), g.binary(KOp::Add, t[6], t[8]));          // ×1
    const int  edges   = g.binary(KOp::Add, g.binary(KOp::Add, w2(t[1]), w2(t[3])), g.binary(KOp::Add, w2(t[5]), w2(t[7]))); // ×2
    const int  centre  = nd::detail::bin(g, KOp::Mul, t[4], detail::kf(g, t[4], 4.0));                                // ×4
    const int  sum     = g.binary(KOp::Add, g.binary(KOp::Add, corners, edges), centre);
    return nd::detail::bin(g, KOp::Mul, sum, detail::kf(g, sum, 1.0 / 16.0));
}

// combine — composite the accumulated bloom pyramid back over the scene: mix(scene, bloom, intensity). A lerp (not a raw add)
// keeps the result energy-bounded; `intensity` ∈ [0,1] is the bloom strength.
[[nodiscard]] inline int combine(KGraph& g, int scene, int bloom_col, int intensity)
{
    return g.ternary(KOp::Mix, scene, bloom_col, g.splat(intensity, 3));
}

// ── B13-b: FFT-convolution glare (frequency-domain core) ─────────────────────────────────────────────────────────────────

// complex_mul — the per-frequency complex product `(a·b)` that IS the convolution in the frequency domain: FFT-convolution
// bloom multiplies the image spectrum by the aperture-PSF spectrum bin-by-bin, giving physically-shaped diffraction glare
// (kernel-shaped aperture flares, anamorphic streaks) impossible with a separable pyramid. Returns vec2 (re, im). The forward
// / inverse FFT itself is the B-compute kernel this feeds.
[[nodiscard]] inline int complex_mul(KGraph& g, int are, int aim, int bre, int bim)
{
    const int re = g.binary(KOp::Sub, g.binary(KOp::Mul, are, bre), g.binary(KOp::Mul, aim, bim)); // ac − bd
    const int im = g.binary(KOp::Add, g.binary(KOp::Mul, are, bim), g.binary(KOp::Mul, aim, bre)); // ad + bc
    return g.vec2(re, im);
}

// ── B13-b: lens flare / starburst ────────────────────────────────────────────────────────────────────────────────────────

// lens_halo — the radial HALO ring weight of a lens flare: a soft ring of light at `radius` from the flare centre (the
// bright ring you see around a strong off-screen light). `uv`/`centre` are vec2; the weight peaks on the ring and falls off
// over `thickness`. Multiply a warm tint by this for the halo.
[[nodiscard]] inline int lens_halo(KGraph& g, int uv, int centre, double radius, double thickness)
{
    const auto k = [&](double v) { return detail::kf(g, uv, v); };
    const int  d = g.binary(KOp::Sub, uv, centre);
    const int  r = g.unary(KOp::Sqrt, g.dot(d, d));                                   // |uv − centre|
    return g.binary(KOp::Sub, k(1.0), g.ternary(KOp::Smoothstep, k(0.0), k(thickness), g.unary(KOp::Abs, g.binary(KOp::Sub, r, k(radius)))));
}

// starburst — the aperture DIFFRACTION starburst (the ray-spikes a bright light throws through a bladed aperture): a
// `blades`-fold cosine pattern of the view angle, raised to `sharpness` for tight spikes. `angle` = atan2 of the pixel about
// the light. Scalar weight ∈ [0,1].
[[nodiscard]] inline int starburst(KGraph& g, int angle, double blades, double sharpness)
{
    const auto k  = [&](double v) { return detail::kf(g, angle, v); };
    const int  c  = g.binary(KOp::Add, k(0.5), g.binary(KOp::Mul, k(0.5), g.unary(KOp::Cos, g.binary(KOp::Mul, k(blades), angle)))); // ½+½cos(blades·θ)
    return g.binary(KOp::Pow, c, k(sharpness));
}

// spectral_tint — a cheap chromatic dispersion tint for lens-flare ghosts: a scalar position `t` ∈ [0,1] along the ghost
// vector → an RGB tint (red at t→0, green mid, blue at t→1), so ghosts fringe with wavelength like a real lens. Returns a
// clamped vec3.
[[nodiscard]] inline int spectral_tint(KGraph& g, int t)
{
    const auto k = [&](double v) { return detail::kf(g, t, v); };
    const int  r = g.binary(KOp::Sub, k(1.0), t);                                             // 1 − t
    const int  gg = g.binary(KOp::Mul, k(4.0), g.binary(KOp::Mul, t, g.binary(KOp::Sub, k(1.0), t))); // 4·t·(1−t), peaks at ½
    return nodes::clamp01(g, g.vec3(r, gg, t));
}

} // namespace crd::kir::bloom
