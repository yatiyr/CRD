#pragma once

// ckir_water.hpp — the CKIR WATER-SHADING LIBRARY (D-007 B16-a-4). The FFT-ocean (a-1..a-3) produces a displaced, foaming
// surface with per-texel normal + foam; this library shades it. `water_shade` is the frontier surface BRDF — a Fresnel-
// DIELECTRIC split of the air-water interface: the reflected side gathers sky + a sun highlight, the transmitted side gathers
// the depth-ABSORBED refraction (Beer-Lambert) of the scene below plus a SUBSURFACE backscatter (the glow of a backlit wave
// crest), and foam overlays where a-3's Jacobian coverage says whitecaps formed. All CKIR value-graph builders (compose core
// KOps; no new device features) — bit-exact on the CPU oracle, emitting on all five backends, riding the clean raster path.
// The screen-space passes (SSR reflection, underwater god-rays) + the multi-cascade world-uv sample are the following a-4
// sub-slices; the analytic surface response is the core every path calls, so it lands first (the B8 lighting methodology).
//
// Air-water Fresnel f0 = ((n_water − 1)/(n_water + 1))² = ((1.333 − 1)/(1.333 + 1))² ≈ 0.02 (Schlick, f90 = 1). Absorption is
// per-channel (water swallows red first ⇒ the deep blue-green). Inputs: n/v/l unit vec3 (v,l point AWAY from the surface);
// sun_color/sky_color/scene_color/deep_color/extinction/foam_color vec3; depth/wave_height/foam/roughness scalar.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_lighting.hpp>
#include <crd/kir/ckir_nodes.hpp>

namespace crd::kir::water
{

namespace lt = crd::kir::lighting;

// air→water Schlick Fresnel reflectance (f0 = 0.02, f90 = 1) at grazing cosine `nov`.
[[nodiscard]] inline int fresnel_water(KGraph& g, int nov)
{
    return lt::f_schlick_scalar(g, lt::detail::kf(g, nov, 0.02), lt::detail::kf(g, nov, 1.0), nov);
}

// Beer-Lambert per-channel transmittance exp(−extinction·depth) through `depth` metres of water (vec3 ext, scalar depth).
[[nodiscard]] inline int beer(KGraph& g, int ext3, int depth)
{
    return g.unary(KOp::Exp, nodes::detail::bin(g, KOp::Mul, ext3, g.unary(KOp::Neg, depth)));
}

// The OCEAN SUN GLITTER — the signature broad, sparkly sun path. Real oceans reflect the sun off a DISTRIBUTION of unresolved
// sub-texel wave slopes, so the highlight is a wide statistical smear, not a plastic point. We drive a GGX microfacet lobe
// with a roughness derived from the wave-SLOPE VARIANCE σ² (the summed energy of the cascade waves finer than the shading
// texel): a Beckmann slope-Gaussian of variance σ² ≈ a GGX of α = √(2σ²). Far water (larger σ²) ⇒ a broader, softer path;
// calm water ⇒ a tight glint. n/v/l unit; returns sun_color · (D·V·F·NoL). Reuses the B8 lighting microfacet core.
[[nodiscard]] inline int ocean_sun_glitter(KGraph& g, int n, int v, int l, int sun_color, int slope_variance)
{
    const auto ks    = [&](double x) { return lt::detail::kf(g, slope_variance, x); };
    const auto sat   = [&](int x) { return g.binary(KOp::Max, g.binary(KOp::Min, x, ks(1.0)), ks(0.0)); };
    const int  hraw  = g.binary(KOp::Add, v, l);
    const int  h     = nodes::detail::bin(g, KOp::Div, hraw, g.unary(KOp::Sqrt, g.dot(hraw, hraw)));
    const int  noh   = sat(g.dot(n, h));
    const int  nov   = g.binary(KOp::Max, g.dot(n, v), ks(1e-4));
    const int  nol   = sat(g.dot(n, l));
    const int  voh   = sat(g.dot(v, h));
    const int  alpha = g.binary(KOp::Max, g.unary(KOp::Sqrt, g.binary(KOp::Mul, ks(2.0), slope_variance)), ks(0.02));
    const int  spec  = g.binary(KOp::Mul, g.binary(KOp::Mul, lt::d_ggx(g, noh, alpha), lt::v_smith_ggx_correlated(g, nov, nol, alpha)),
                                g.binary(KOp::Mul, fresnel_water(g, voh), nol));
    return nodes::detail::bin(g, KOp::Mul, sun_color, spec);
}

// A cheap-but-pretty ANALYTIC SKY for the water's reflection (until the reflected ray samples the B15 Hillaire sky / an env
// probe / SSR): a horizon→zenith gradient by the reflected ray's height `dir.y`, plus a soft sun DISK where the ray aligns
// with the sun (exp falloff — a bright core + halo). `dir`/`sun_dir` unit; horizon/zenith/sun_color vec3. Returns the sky
// radiance the surface reflects.
[[nodiscard]] inline int sky_color(KGraph& g, int dir, int sun_dir, int horizon_col, int zenith_col, int sun_color)
{
    const auto ks  = [&](double x) { return lt::detail::kf(g, sun_dir, x); };
    const auto b   = [&](KOp op, int a, int c) { return nodes::detail::bin(g, op, a, c); };
    const int  ty  = g.binary(KOp::Max, g.binary(KOp::Min, g.swizzle(dir, 1), ks(1.0)), ks(0.0)); // saturate(dir.y)
    const int  grad = b(KOp::Add, b(KOp::Mul, horizon_col, g.binary(KOp::Sub, ks(1.0), ty)), b(KOp::Mul, zenith_col, ty));
    const int  sd  = g.binary(KOp::Max, g.dot(dir, sun_dir), ks(0.0));
    const int  disk = g.unary(KOp::Exp, g.binary(KOp::Mul, g.binary(KOp::Sub, sd, ks(1.0)), ks(120.0))); // exp((sd−1)·120)
    const int  halo = g.unary(KOp::Exp, g.binary(KOp::Mul, g.binary(KOp::Sub, sd, ks(1.0)), ks(8.0)));   // broad glow
    const int  glow = g.binary(KOp::Add, disk, g.binary(KOp::Mul, halo, ks(0.15)));
    return b(KOp::Add, grad, b(KOp::Mul, sun_color, glow));
}

// The frontier water surface BRDF. Returns the outgoing radiance (vec3).
[[nodiscard]] inline int water_shade(KGraph& g, int n, int v, int l, int sun_color, int sky_color, int scene_color,
                                     int deep_color, int extinction, int depth, int wave_height, int foam, int foam_color,
                                     int roughness)
{
    const auto ks  = [&](double x) { return lt::detail::kf(g, depth, x); };                                  // scalar const
    const auto sat = [&](int x) { return g.binary(KOp::Max, g.binary(KOp::Min, x, ks(1.0)), ks(0.0)); };     // saturate
    const auto b   = [&](KOp op, int a, int c) { return nodes::detail::bin(g, op, a, c); };                  // broadcast binary
    const auto one3 = [&]() { return g.vec3(ks(1.0), ks(1.0), ks(1.0)); };

    // geometry: NoV (grazing), NoL, and the half-vector (manual normalize for a bit-exact reference).
    const int nov  = g.binary(KOp::Max, g.dot(n, v), ks(1e-4));
    const int nol  = sat(g.dot(n, l));
    const int hraw = g.binary(KOp::Add, v, l);
    const int hlen = g.unary(KOp::Sqrt, g.dot(hraw, hraw));
    const int h    = b(KOp::Div, hraw, hlen);
    const int noh  = sat(g.dot(n, h));

    // Fresnel + a GGX sun highlight (the sun glitter): sun · (D · F · NoL).
    const int fr    = fresnel_water(g, nov);
    const int alpha = g.binary(KOp::Mul, roughness, roughness);
    const int sunsp = b(KOp::Mul, sun_color, g.binary(KOp::Mul, lt::d_ggx(g, noh, alpha), g.binary(KOp::Mul, fr, nol)));
    const int refl  = b(KOp::Add, sky_color, sunsp); // sky env + sun highlight

    // refraction: Beer depth-absorption ⇒ mix(deep, scene, exp(−ext·depth)) = scene·a + deep·(1−a).
    const int absorb = beer(g, extinction, depth);
    const int refr   = b(KOp::Add, b(KOp::Mul, scene_color, absorb), b(KOp::Mul, deep_color, b(KOp::Sub, one3(), absorb)));

    // subsurface backscatter: the wave glows when the sun is behind it and high — deep_color · sat(wave)·(sat(V·−L))³.
    const int vdl  = sat(g.dot(v, g.unary(KOp::Neg, l)));
    const int vdl3 = g.binary(KOp::Mul, g.binary(KOp::Mul, vdl, vdl), vdl);
    const int sss  = b(KOp::Mul, deep_color, g.binary(KOp::Mul, sat(wave_height), vdl3));

    // Fresnel-dielectric blend: refl·F + (refr + sss)·(1−F).
    const int water = b(KOp::Add, b(KOp::Mul, refl, fr), b(KOp::Mul, b(KOp::Add, refr, sss), g.binary(KOp::Sub, ks(1.0), fr)));

    // foam overlay: mix(water, foam_color·(NoL + 0.2), foam).
    const int foamlit = b(KOp::Mul, foam_color, g.binary(KOp::Add, nol, ks(0.2)));
    return b(KOp::Add, b(KOp::Mul, water, g.binary(KOp::Sub, ks(1.0), foam)), b(KOp::Mul, foamlit, foam));
}

} // namespace crd::kir::water
