#pragma once

// ckir_finish.hpp — B13-e: the FINISHING pass — the last per-pixel touches before display, each a backend-neutral CKIR pass.
// Geometric specular AA (Tokuyoshi-Kaplanyan screen-space NDF filtering — non-negotiable for stable PBR: it kills the
// specular shimmer/crawl that MSAA + TAA alone can't), chromatic aberration, optical cos⁴ vignette, Lottes film grain, and
// AMD FidelityFX CAS contrast-adaptive sharpening (the natural partner to a TAAU-upscaled image).
//
// SCOPE (buildable now): the roughness-widening from the normal's screen-space derivatives; the radial CA offset; the cos⁴
// vignette attenuation; the luminance-weighted grain; the CAS adaptive-sharpen kernel. All per-pixel transforms.
//
// RENDERER LEAVES (deferred — bound resources, not new IR): the normal-derivative source (dFdx/dFdy — a fragment builtin the
// caller feeds in); the CA per-channel SAMPLE at the offset UVs (3 texture taps); the blue-noise / grain texture (B2 bind);
// the CAS 3×3 neighborhood GATHER (the caller supplies the 5 cross taps). Every transform those invoke is complete here.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_nodes.hpp> // nodes::clamp / clamp01 / detail::bin (broadcast-safe vec·scalar)

namespace crd::kir::finish
{

namespace detail
{
[[nodiscard]] inline int kf(KGraph& g, int like, double v) { return g.constant(v, g.node(like).shape, g.node(like).dtype()); }
[[nodiscard]] inline int luma(KGraph& g, int color) { return g.dot(color, g.vec3(kf(g, color, 0.2126), kf(g, color, 0.7152), kf(g, color, 0.0722))); }
} // namespace detail

// specular_aa — Tokuyoshi-Kaplanyan geometric specular antialiasing: widen the roughness (filter α²) by the sub-pixel normal
// variance so a curved/bumpy surface stops sparkling under motion. `dndx`/`dndy` = the normal's screen-space derivatives
// (dFdx/dFdy of the world normal — the caller feeds them). `kappa` scales the variance, `sigma2_max` caps the widening.
//   filteredα = clamp01( √( α² + min(2·κ·(|dNdx|²+|dNdy|²), σ²max) ) )
[[nodiscard]] inline int specular_aa(KGraph& g, int alpha, int dndx, int dndy, double kappa, double sigma2_max)
{
    const auto k    = [&](double v) { return detail::kf(g, alpha, v); };
    const int  var  = g.binary(KOp::Mul, k(kappa), g.binary(KOp::Add, g.dot(dndx, dndx), g.dot(dndy, dndy))); // κ·(|dNdx|²+|dNdy|²)
    const int  ker  = g.binary(KOp::Min, g.binary(KOp::Mul, k(2.0), var), k(sigma2_max));                     // min(2·var, σ²max)
    return nodes::clamp01(g, g.unary(KOp::Sqrt, g.binary(KOp::Add, g.binary(KOp::Mul, alpha, alpha), ker)));   // √(α²+kernel)
}

// ca_offset — the radial chromatic-aberration UV displacement: an offset that grows with distance from `centre` (the effect
// a real lens shows at the frame edges). The caller samples R at uv+offset, B at uv−offset (G unshifted) for the fringe.
// Returns the vec2 offset. `strength` sets the dispersion.
[[nodiscard]] inline int ca_offset(KGraph& g, int uv, int centre, double strength)
{
    const int d  = g.binary(KOp::Sub, uv, centre);
    const int r2 = g.dot(d, d);
    return nodes::detail::bin(g, KOp::Mul, d, g.binary(KOp::Mul, detail::kf(g, r2, strength), r2)); // d·(strength·r²)
}

// vignette — the optical cos⁴ falloff (natural lens light drop toward the frame edges). With r² = |uv−centre|² and
// att = 1/(1 + r²/f²), the cos⁴ law is att². `inv_f2` = 1/f² sets how fast it darkens. Multiply the image by this.
[[nodiscard]] inline int vignette(KGraph& g, int uv, int centre, double inv_f2)
{
    const int d   = g.binary(KOp::Sub, uv, centre);
    const int att = g.binary(KOp::Div, detail::kf(g, uv, 1.0), g.binary(KOp::Add, detail::kf(g, uv, 1.0), g.binary(KOp::Mul, g.dot(d, d), detail::kf(g, uv, inv_f2))));
    return g.binary(KOp::Mul, att, att); // cos⁴ = att²
}

// film_grain — Timothy Lottes' luminance-weighted film grain: additive noise scaled by a midtone response luma·(1−luma) (grain
// reads strongest in the mids, vanishes in crushed blacks / blown highlights — like real film). `noise` ∈ [0,1) (blue-noise
// or ign); `intensity` sets the amount. Returns the grained colour.
[[nodiscard]] inline int film_grain(KGraph& g, int color, int noise, double intensity)
{
    const auto k    = [&](double v) { return detail::kf(g, color, v); };
    const int  lum  = detail::luma(g, color);
    const int  resp = g.binary(KOp::Mul, lum, g.binary(KOp::Sub, k(1.0), lum));                            // luma·(1−luma)
    const int  amt  = g.binary(KOp::Mul, g.binary(KOp::Mul, g.binary(KOp::Sub, g.binary(KOp::Mul, noise, k(2.0)), k(1.0)), k(intensity)), resp); // (2n−1)·intensity·resp
    return nodes::detail::bin(g, KOp::Add, color, amt);
}

// cas_sharpen — AMD FidelityFX CAS (Contrast-Adaptive Sharpening): sharpen the centre pixel by its 4 cross neighbours with a
// per-channel weight that ADAPTS to local contrast (min/max ring) so flat regions and already-sharp edges are left alone (no
// ringing/oversharpen). The natural pairing with a TAAU-upscaled frame. `c/u/d/l/r` = the centre + 4 cross taps; `sharpness`
// ∈ [0,1]. out = (c + w·Σneighbours)/(1 + 4w), w = −amp·(0.125 + 0.075·sharpness), amp = √clamp01(min(mn,1−mx)/mx).
[[nodiscard]] inline int cas_sharpen(KGraph& g, int c, int u, int d, int l, int r, double sharpness)
{
    namespace nd  = nodes;
    const auto k  = [&](double v) { return detail::kf(g, c, v); };
    const int  mn = g.binary(KOp::Min, g.binary(KOp::Min, g.binary(KOp::Min, u, d), g.binary(KOp::Min, l, r)), c); // ring + centre min
    const int  mx = g.binary(KOp::Max, g.binary(KOp::Max, g.binary(KOp::Max, u, d), g.binary(KOp::Max, l, r)), c); // ring + centre max
    // NB: `1−mx` and `mx+ε` are scalar⊗vec3 — must splat/bin (a bare g.binary broadcasts on GPU but reads OOB in the oracle).
    const int  one_mx = g.binary(KOp::Sub, g.splat(k(1.0), 3), mx);                       // 1 − mx (per channel)
    const int  amp    = g.unary(KOp::Sqrt, nd::clamp01(g, g.binary(KOp::Div, g.binary(KOp::Min, mn, one_mx), nd::detail::bin(g, KOp::Add, mx, k(1.0e-5)))));
    const int  w   = nd::detail::bin(g, KOp::Mul, amp, k(-(0.125 + 0.075 * sharpness))); // negative sharpen weight (per channel)
    const int  ring = g.binary(KOp::Add, g.binary(KOp::Add, u, d), g.binary(KOp::Add, l, r));
    const int  num  = g.binary(KOp::Add, c, g.binary(KOp::Mul, ring, w));                                // c + w·Σ
    const int  den  = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, w, k(4.0)), k(1.0));     // 1 + 4w
    return g.binary(KOp::Div, num, den);
}

} // namespace crd::kir::finish
