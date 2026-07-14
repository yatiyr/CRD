#pragma once

// ckir_cinematic.hpp — B13-d: the CINEMATIC lens effects — depth of field + motion blur, each a backend-neutral CKIR pass.
// DoF uses the Garcia (Frostbite 2017) SEPARABLE complex-phasor circular bokeh: a circular blur kernel is approximated as a
// sum of complex Gaussians, so a true 2-D disk convolution factors into cheap horizontal + vertical complex passes (real
// circular bokeh, not a box/hex hack). Motion blur is the McGuire (I3D 2012) velocity-tile reconstruction: cone/cylinder
// coverage weights + a soft depth compare over the dilated tile-max velocity field.
//
// SCOPE (buildable now): the thin-lens circle-of-confusion, the complex-Gaussian phasor component + its real reconstruction,
// the CoC scatter-as-gather coverage, the near/far DoF composite; the shutter velocity scale/clamp, the cone + cylinder
// reconstruction weights, and the soft depth classification.
//
// RENDERER LEAVES (deferred — bound resources, not new IR): the separable horizontal/vertical GATHER passes (per-component
// complex accumulation over the CoC-scaled tap line — texture taps + a ping-pong target); the velocity-field TILE-MAX +
// NEIGHBORMAX dilation gather; the half-res DoF + MB scatter targets. Every per-sample transform is complete here.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_nodes.hpp> // nodes::clamp / clamp01 / detail::bin (broadcast-safe vec·scalar)

namespace crd::kir::cinematic
{

namespace detail
{
[[nodiscard]] inline int kf(KGraph& g, int like, double v) { return g.constant(v, g.node(like).shape, g.node(like).dtype()); }
} // namespace detail

// ── B13-d: DEPTH OF FIELD (Garcia separable complex bokeh) ───────────────────────────────────────────────────────────────

// circle_of_confusion — the thin-lens signed CoC radius for a pixel at `depth`, focused at `focus`: how blurred it is.
// CoC = (f²/N)·(depth−focus) / (depth·(focus−f)) — `f` = focal length, `N` = f-number (aperture). Sign: + = far field
// (behind focus), − = near field (in front). Its magnitude drives the bokeh kernel radius; the sign drives near/far occlusion.
[[nodiscard]] inline int circle_of_confusion(KGraph& g, int depth, int focus, double focal_len, double f_number)
{
    const auto k   = [&](double v) { return detail::kf(g, depth, v); };
    const int  num = g.binary(KOp::Mul, k(focal_len * focal_len / f_number), g.binary(KOp::Sub, depth, focus)); // (f²/N)(depth−focus)
    const int  den = g.binary(KOp::Mul, depth, g.binary(KOp::Sub, focus, k(focal_len)));                        // depth·(focus−f)
    return g.binary(KOp::Div, num, den);
}

// complex_gaussian — one Garcia phasor component evaluated at squared radius `r2`: env·(cos, sin) with env = exp(a·r2) and
// phase b·r2. Summing a few of these (each with fitted real coeffs a<0, b) reconstructs the flat-topped circular bokeh disk
// that a box/Gaussian blur can't. Returns the complex value as vec2 (re, im). `a`,`b` are the component's fitted constants.
[[nodiscard]] inline int complex_gaussian(KGraph& g, int r2, double a, double b)
{
    const auto k   = [&](double v) { return detail::kf(g, r2, v); };
    const int  env = g.unary(KOp::Exp, g.binary(KOp::Mul, k(a), r2));   // exp(a·r²)
    const int  ph  = g.binary(KOp::Mul, k(b), r2);                       // b·r²
    return g.vec2(g.binary(KOp::Mul, env, g.unary(KOp::Cos, ph)), g.binary(KOp::Mul, env, g.unary(KOp::Sin, ph)));
}

// bokeh_realize — reconstruct the REAL blurred value from a component's accumulated complex sum `(re,im)` weighted by that
// component's output coefficient `(cre,cim)`: Re((re+i·im)·(cre+i·cim)) = re·cre − im·cim. Sum this over the components to
// get the final DoF pixel. (The complex accumulation across the tap line is the renderer's separable gather.)
[[nodiscard]] inline int bokeh_realize(KGraph& g, int re, int im, double cre, double cim)
{
    const auto k = [&](double v) { return detail::kf(g, re, v); };
    return g.binary(KOp::Sub, g.binary(KOp::Mul, re, k(cre)), g.binary(KOp::Mul, im, k(cim))); // re·cre − im·cim
}

// coc_coverage — the scatter-as-gather tap weight: how much a neighbor whose CoC radius is `tap_coc` (pixels) covers the
// centre pixel `dist` pixels away. A soft disk: clamp01(tap_coc − dist + ½) — full inside the CoC, feathering by one pixel at
// the rim. Guards a sharp tap from bleeding onto a pixel outside its blur circle.
[[nodiscard]] inline int coc_coverage(KGraph& g, int tap_coc, int dist)
{
    return nodes::clamp01(g, g.binary(KOp::Add, g.binary(KOp::Sub, tap_coc, dist), detail::kf(g, tap_coc, 0.5)));
}

// dof_composite — blend the sharp scene with its blurred version by the normalized blur amount |coc|/max_coc. The final DoF
// resolve: in focus (coc≈0) → sharp; far from focus → fully blurred. `sharp`/`blurred` are vec3, `coc` the signed CoC.
[[nodiscard]] inline int dof_composite(KGraph& g, int sharp, int blurred, int coc, double max_coc)
{
    const int t = nodes::clamp01(g, g.binary(KOp::Div, g.unary(KOp::Abs, coc), detail::kf(g, coc, max_coc)));
    return g.ternary(KOp::Mix, sharp, blurred, g.splat(t, 3));
}

// ── B13-d: MOTION BLUR (McGuire velocity-tile reconstruction) ────────────────────────────────────────────────────────────

// velocity_scale — the per-pixel screen-space velocity scaled by the shutter fraction and CLAMPED to `max_len` pixels (so a
// fast object can't smear across the whole frame / blow the tile budget). `vel` = (cur−prev) screen position, vec2. Returns
// the clamped blur vector.
[[nodiscard]] inline int velocity_scale(KGraph& g, int vel, double shutter, double max_len)
{
    namespace nd   = nodes;
    const auto k   = [&](double v) { return detail::kf(g, vel, v); };
    const int  v   = nd::detail::bin(g, KOp::Mul, vel, k(shutter));                             // vel·shutter
    const int  len = g.binary(KOp::Add, g.unary(KOp::Sqrt, g.dot(v, v)), k(1.0e-6));            // |v| (ε-guarded)
    const int  s   = g.binary(KOp::Min, k(1.0), g.binary(KOp::Div, k(max_len), len));           // min(1, maxLen/|v|)
    return nd::detail::bin(g, KOp::Mul, v, s);
}

// mb_cone — the McGuire CONE weight: a sample `dist` pixels away contributes on a linear falloff to `vel_len` (its own blur
// extent) — clamp01(1 − dist/vel_len). Used for the sample BEING blurred (its trail tapers to a point).
[[nodiscard]] inline int mb_cone(KGraph& g, int dist, int vel_len)
{
    return nodes::clamp01(g, g.binary(KOp::Sub, detail::kf(g, dist, 1.0), g.binary(KOp::Div, dist, g.binary(KOp::Add, vel_len, detail::kf(g, vel_len, 1.0e-6)))));
}

// mb_cylinder — the McGuire CYLINDER weight: full coverage out to the blur radius then a soft rim — 1 − smoothstep(0.95·r,
// 1.05·r, dist). Used for the sample DOING the blurring over a neighbor (a moving occluder paints a flat streak, not a cone).
[[nodiscard]] inline int mb_cylinder(KGraph& g, int dist, int vel_len)
{
    const auto k = [&](double v) { return detail::kf(g, vel_len, v); };
    return g.binary(KOp::Sub, detail::kf(g, dist, 1.0), g.ternary(KOp::Smoothstep, g.binary(KOp::Mul, k(0.95), vel_len), g.binary(KOp::Mul, k(1.05), vel_len), dist));
}

// mb_soft_depth — the McGuire soft depth classification: is sample A in front of centre B? clamp01(1 − (zA − zB)/extent) →
// 1 when A is nearer (it can blur over B), 0 when A is well behind. `extent` softens the transition so silhouettes don't
// hard-edge. Weights whether a moving foreground correctly smears over a static background.
[[nodiscard]] inline int mb_soft_depth(KGraph& g, int za, int zb, double extent)
{
    const auto k = [&](double v) { return detail::kf(g, za, v); };
    return nodes::clamp01(g, g.binary(KOp::Sub, k(1.0), g.binary(KOp::Div, g.binary(KOp::Sub, za, zb), k(extent))));
}

} // namespace crd::kir::cinematic
