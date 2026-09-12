#pragma once

// ckir_screen.hpp — B12: the SCREEN-SPACE lighting frontier (the indirect + volumetric contributions the raster path can't get
// from IBL alone), each a backend-neutral CKIR pass. B12-a AMBIENT OCCLUSION: the 2026 frontier is Visibility-Bitmask AO
// (SSILVB, Therrien 2023 — an N-sector hemisphere bitmask that handles thin/partial occluders + yields indirect diffuse + bent
// cones), with GTAO (Jimenez ground-truth + multi-bounce) as the fallback tier, + specular occlusion (Frostbite).
//
// SCOPE: the per-fragment integrands / bitmask / occlusion MATH is here + testable now. The DEPTH-BUFFER MARCH that finds the
// horizon angles + accumulates the sector bitmask + the bent-normal direction (and the temporal/spatial denoise) is the
// renderer leaf (post-detour screen-space pass) — it needs the depth+normal G-buffer bound as textures; the math it invokes is
// complete here.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_lighting.hpp>

namespace crd::kir::screen
{

namespace detail
{
[[nodiscard]] inline int kf(KGraph& g, int like, double v) { return g.constant(v, g.node(like).shape, g.node(like).dtype()); }
[[nodiscard]] inline int ku(KGraph& g, int like, crd::u32 v) { return g.constant(static_cast<double>(v), g.node(like).shape, DType::U32); }
} // namespace detail

// ── B12-a: AMBIENT OCCLUSION ─────────────────────────────────────────────────────────────────────────────────────────────

// gtao_slice — the Jimenez GTAO arc-integral for ONE slice: the cosine-weighted visibility between the two horizon angles
// h1,h2, around the projected-normal angle `gamma` (`n_len` = the projected normal's length in the slice plane). Summed over a
// few slices (the march that finds h1/h2 is renderer-side) this is ground-truth ambient occlusion.
//   inner(h) = −cos(2h − γ) + cos γ + 2h·sin γ  ;  slice = ¼·n_len·(inner(h1) + inner(h2))
[[nodiscard]] inline int gtao_slice(KGraph& g, int h1, int h2, int gamma, int n_len)
{
    const auto k     = [&](int like, double v) { return detail::kf(g, like, v); };
    const int  cg    = g.unary(KOp::Cos, gamma);
    const int  sg    = g.unary(KOp::Sin, gamma);
    const auto inner = [&](int h) {
        const int t = g.binary(KOp::Sub, g.binary(KOp::Mul, k(h, 2.0), h), gamma); // 2h − γ
        return g.binary(KOp::Add, g.binary(KOp::Add, g.unary(KOp::Neg, g.unary(KOp::Cos, t)), cg), g.binary(KOp::Mul, g.binary(KOp::Mul, k(h, 2.0), h), sg));
    };
    return g.binary(KOp::Mul, g.binary(KOp::Mul, k(h1, 0.25), n_len), g.binary(KOp::Add, inner(h1), inner(h2)));
}

// gtao_multibounce — the GTAO multi-bounce albedo re-lighting (Jimenez): a per-channel cubic in the visibility that adds back
// the light a single-scatter AO drops. `albedo` vec3, `visibility` scalar. `max(v, ((v·a+b)·v+c)·v)`.
[[nodiscard]] inline int gtao_multibounce(KGraph& g, int visibility, int albedo)
{
    namespace nd   = nodes;
    const auto kc  = [&](double v) { return nd::detail::konst(g, albedo, v); };
    const int  a   = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, albedo, kc(2.0404)), kc(-0.3324));
    const int  b   = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, albedo, kc(-4.7951)), kc(0.6417));
    const int  c   = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, albedo, kc(2.7552)), kc(0.6903));
    const int  v3  = g.splat(visibility, 3);
    const int  p   = nd::detail::bin(g, KOp::Mul, nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, v3, a), b), v3), c), v3);
    return g.binary(KOp::Max, v3, p);
}

// spec_occlusion — Frostbite specular occlusion from AO + NoV + roughness: rough surfaces see more of the AO, smooth ones less.
//   saturate(pow(NoV + ao, exp2(−16·roughness − 1)) − 1 + ao)
[[nodiscard]] inline int spec_occlusion(KGraph& g, int nov, int ao, int roughness)
{
    const auto k    = [&](int like, double v) { return detail::kf(g, like, v); };
    const int  e    = g.unary(KOp::Exp2, g.binary(KOp::Sub, g.binary(KOp::Mul, k(roughness, -16.0), roughness), k(roughness, 1.0)));
    const int  p    = g.binary(KOp::Pow, g.binary(KOp::Add, nov, ao), e);
    return nodes::clamp01(g, g.binary(KOp::Add, g.binary(KOp::Sub, p, k(nov, 1.0)), ao));
}

// ssilvb_sector_mask — Visibility-Bitmask AO (SSILVB, Therrien 2023): the u32 sector mask a SINGLE depth sample occludes,
// [min_h, max_h] normalized hemisphere angles ∈ [0,1], `n_sectors` = N (32). OR these across the march → the visibility
// bitfield. Shifts are clamped (start ≤ N−1, count ≤ 31, start+count ≤ N) so the mask fits in 32 bits — GPU u32 == oracle.
//   start = min(⌊min_h·N⌋, N−1) ; count = clamp(⌈(max_h−min_h)·N⌉, 0, min(31, N−start)) ; mask = ((1<<count)−1) << start
[[nodiscard]] inline int ssilvb_sector_mask(KGraph& g, int min_h, int max_h, double n_sectors)
{
    const auto k        = [&](double v) { return detail::kf(g, min_h, v); };
    const int  start_f  = g.binary(KOp::Min, g.unary(KOp::Floor, g.binary(KOp::Mul, min_h, k(n_sectors))), k(n_sectors - 1.0));
    const int  raw_f    = g.unary(KOp::Ceil, g.binary(KOp::Mul, g.binary(KOp::Sub, max_h, min_h), k(n_sectors)));
    const int  max_cnt  = g.binary(KOp::Min, k(31.0), g.binary(KOp::Sub, k(n_sectors), start_f));
    const int  count_f  = nodes::clamp(g, raw_f, k(0.0), max_cnt);
    const int  start    = g.cast(start_f, DType::U32);
    const int  count    = g.cast(count_f, DType::U32);
    const int  one      = detail::ku(g, min_h, 1U);
    const int  bits     = g.binary(KOp::Sub, g.binary(KOp::Shl, one, count), one); // (1<<count) − 1
    return g.binary(KOp::Shl, bits, start);
}
// ssilvb_ao — the AO from the accumulated visibility bitfield: 1 − popcount(bitfield)/N (the fraction of the hemisphere the
// occluders cover). popcount → float before the divide (integer div would truncate on the GPU).
[[nodiscard]] inline int ssilvb_ao(KGraph& g, int bitfield, double n_sectors)
{
    const int cnt = g.cast(g.unary(KOp::BitCount, bitfield), DType::F32);
    return g.binary(KOp::Sub, detail::kf(g, cnt, 1.0), g.binary(KOp::Div, cnt, detail::kf(g, cnt, n_sectors)));
}

// ── B12-b: SCREEN-SPACE REFLECTIONS (Hi-Z ray-march + stochastic rough + resolve + edge fade) ─────────────────────────────

// ssr_reflect — the reflection ray a screen-space march follows: `reflect(incident, N) = incident − 2·(N·incident)·N`. The
// incident is the view ray toward the surface; for rough SSR the direction is GGX-importance-perturbed (reuse
// `lighting::importance_sample_ggx`). The Hi-Z hierarchical-depth march itself is renderer-side.
[[nodiscard]] inline int ssr_reflect(KGraph& g, int incident, int normal) { return g.reflect(incident, normal); }

// ssr_hiz_hit — the per-step intersection test: the marched ray HIT the surface if it crossed BEHIND it (ray_z > scene_z) but
// within `thickness` (a surface is a thin shell, not an infinite occluder — the thickness test kills false hits behind it).
// Branchless (GLSL `step`). The depth SAMPLE that yields `scene_z` is the renderer's Hi-Z lookup.
[[nodiscard]] inline int ssr_hiz_hit(KGraph& g, int ray_z, int scene_z, int thickness)
{
    const int d = g.binary(KOp::Sub, ray_z, scene_z);
    return g.binary(KOp::Mul, g.binary(KOp::Step, detail::kf(g, d, 0.0), d), g.binary(KOp::Step, d, thickness)); // 0 < d ≤ thickness
}

// ssr_edge_fade — attenuate a reflection whose march runs off-screen: smoothstep the distance to the nearest screen border so
// the reflection fades out at the edges (instead of smearing the clamped border pixel). `uv` ∈ [0,1]², `border` = fade width.
[[nodiscard]] inline int ssr_edge_fade(KGraph& g, int uv, int border)
{
    const auto k  = [&](int like, double v) { return detail::kf(g, like, v); };
    const int  ux = g.swizzle(uv, 0);
    const int  uy = g.swizzle(uv, 1);
    const int  ex = g.binary(KOp::Min, ux, g.binary(KOp::Sub, k(ux, 1.0), ux)); // min(ux, 1−ux)
    const int  ey = g.binary(KOp::Min, uy, g.binary(KOp::Sub, k(uy, 1.0), uy));
    const int  e  = g.binary(KOp::Min, ex, ey);
    return g.ternary(KOp::Smoothstep, k(e, 0.0), border, e); // smoothstep(0, border, nearest-edge-dist)
}

// ssr_confidence — the reflection confidence that weights the SS hit before it composites over the cache→RT fallback: fades
// with roughness (rough = blurry, less reliable SS trace), the edge fade, and the hit-distance falloff `exp(−2·dist)`.
[[nodiscard]] inline int ssr_confidence(KGraph& g, int roughness, int edge_fade, int hit_dist)
{
    const auto k       = [&](int like, double v) { return detail::kf(g, like, v); };
    const int  rough_w = g.binary(KOp::Sub, k(roughness, 1.0), roughness);               // 1 − roughness
    const int  dist_w  = g.unary(KOp::Exp, g.binary(KOp::Mul, k(hit_dist, -2.0), hit_dist)); // exp(−2·dist)
    return nodes::clamp01(g, g.binary(KOp::Mul, g.binary(KOp::Mul, rough_w, edge_fade), dist_w));
}

// ── B12-c: SCREEN-SPACE GLOBAL ILLUMINATION (visibility-bitmask indirect diffuse) ─────────────────────────────────────────

// ssgi_bounce — the indirect-diffuse contribution of ONE screen-space sample, computed FOR FREE on the SSILVB bitmask (B12-c
// rides B12-a): the sample's `radiance` (from the colour buffer at the sampled point — renderer-side) illuminates the sectors
// it NEWLY occludes, `sample_mask & ~bitfield`, weighted by their count/N and the receiver cosine. Σ over the march = the
// single-bounce indirect diffuse — the contact-GI layer that sits ON TOP of the B14 world-space cache, never the sole GI.
[[nodiscard]] inline int ssgi_bounce(KGraph& g, int radiance, int sample_mask, int bitfield, int cos_n, double n_sectors)
{
    const int new_bits = g.binary(KOp::BitAnd, sample_mask, g.unary(KOp::BitNot, bitfield));
    const int cnt      = g.cast(g.unary(KOp::BitCount, new_bits), g.node(cos_n).dtype()); // count → the float dtype of the math around it
    const int w        = g.binary(KOp::Mul, g.binary(KOp::Div, cnt, detail::kf(g, cnt, n_sectors)), cos_n);
    return nodes::detail::bin(g, KOp::Mul, radiance, w); // radiance × (newly-lit fraction · cos)
}

// ── B12-d: VOLUMETRIC LIGHTING + FOG (the phase-function family + Beer-Lambert + froxel scatter) ──────────────────────────
namespace vol { inline constexpr double kPi = 3.14159265358979323846; }

// henyey_greenstein — the HG phase function: `(1−g²) / (4π·(1+g²−2g·cosθ)^1.5)`. `g_aniso` ∈ (−1,1): forward (>0) / back (<0).
[[nodiscard]] inline int henyey_greenstein(KGraph& g, int cos_theta, int g_aniso)
{
    const auto k     = [&](int like, double v) { return detail::kf(g, like, v); };
    const int  g2    = g.binary(KOp::Mul, g_aniso, g_aniso);
    const int  d     = g.binary(KOp::Sub, g.binary(KOp::Add, k(g2, 1.0), g2), g.binary(KOp::Mul, g.binary(KOp::Mul, k(g2, 2.0), g_aniso), cos_theta)); // 1+g²−2g·cosθ
    const int  denom = g.binary(KOp::Mul, k(g2, 4.0 * vol::kPi), g.binary(KOp::Pow, d, k(g2, 1.5)));
    return g.binary(KOp::Div, g.binary(KOp::Sub, k(g2, 1.0), g2), denom);
}
// cornette_shanks — the improved (Mie-like) HG: `(3/(8π))·(1−g²)(1+cos²θ) / ((2+g²)(1+g²−2g·cosθ)^1.5)`.
[[nodiscard]] inline int cornette_shanks(KGraph& g, int cos_theta, int g_aniso)
{
    const auto k     = [&](int like, double v) { return detail::kf(g, like, v); };
    const int  g2    = g.binary(KOp::Mul, g_aniso, g_aniso);
    const int  c2    = g.binary(KOp::Mul, cos_theta, cos_theta);
    const int  num   = g.binary(KOp::Mul, g.binary(KOp::Mul, k(g2, 3.0 / (8.0 * vol::kPi)), g.binary(KOp::Sub, k(g2, 1.0), g2)), g.binary(KOp::Add, k(c2, 1.0), c2));
    const int  d     = g.binary(KOp::Sub, g.binary(KOp::Add, k(g2, 1.0), g2), g.binary(KOp::Mul, g.binary(KOp::Mul, k(g2, 2.0), g_aniso), cos_theta));
    const int  denom = g.binary(KOp::Mul, g.binary(KOp::Add, k(g2, 2.0), g2), g.binary(KOp::Pow, d, k(g2, 1.5)));
    return g.binary(KOp::Div, num, denom);
}
// draine_mie — Draine's approximate-Mie phase (Jendersie & d'Eon 2023): the α term reshapes the peak toward true Mie.
//   `(1−g²)(1+α·cos²θ) / (4π·(1 + α(1+2g²)/3)·(1+g²−2g·cosθ)^1.5)`
[[nodiscard]] inline int draine_mie(KGraph& g, int cos_theta, int g_aniso, int alpha)
{
    const auto k     = [&](int like, double v) { return detail::kf(g, like, v); };
    const int  g2    = g.binary(KOp::Mul, g_aniso, g_aniso);
    const int  c2    = g.binary(KOp::Mul, cos_theta, cos_theta);
    const int  num   = g.binary(KOp::Mul, g.binary(KOp::Sub, k(g2, 1.0), g2), g.binary(KOp::Add, k(c2, 1.0), g.binary(KOp::Mul, alpha, c2)));
    const int  norm  = g.binary(KOp::Add, k(g2, 1.0), g.binary(KOp::Div, g.binary(KOp::Mul, alpha, g.binary(KOp::Add, k(g2, 1.0), g.binary(KOp::Mul, k(g2, 2.0), g2))), k(g2, 3.0)));
    const int  d     = g.binary(KOp::Sub, g.binary(KOp::Add, k(g2, 1.0), g2), g.binary(KOp::Mul, g.binary(KOp::Mul, k(g2, 2.0), g_aniso), cos_theta));
    const int  denom = g.binary(KOp::Mul, g.binary(KOp::Mul, k(g2, 4.0 * vol::kPi), norm), g.binary(KOp::Pow, d, k(g2, 1.5)));
    return g.binary(KOp::Div, num, denom);
}
// beer_lambert — transmittance through an absorbing/scattering medium: `exp(−σ·dist)`. `sigma` may be vec3 (per-channel).
[[nodiscard]] inline int beer_lambert(KGraph& g, int sigma, int dist)
{
    return g.unary(KOp::Exp, g.unary(KOp::Neg, nodes::detail::bin(g, KOp::Mul, sigma, dist)));
}
// froxel_scatter — the in-scattering accumulated into a froxel cell: `light_color · phase · transmittance · density` (the
// froxel march + temporal reprojection are renderer-side; this is the per-cell scatter integrand). light_color/transmittance vec3.
[[nodiscard]] inline int froxel_scatter(KGraph& g, int light_color, int phase, int transmittance, int density)
{
    const int lp = nodes::detail::bin(g, KOp::Mul, light_color, phase);
    return nodes::detail::bin(g, KOp::Mul, g.binary(KOp::Mul, lp, transmittance), density);
}

// ── B12-e: SCREEN-SPACE SUBSURFACE SCATTERING (separable, Burley/Christensen diffusion) ───────────────────────────────────

// burley_diffusion — the Burley/Christensen normalized diffusion profile R(r): the fraction of light exiting at radius `r` for
// a per-channel scattering distance `d` (vec3 = the subsurface colour's rgb falloff). `(exp(−r/d)+exp(−r/3d)) / (8π·d·r)`.
[[nodiscard]] inline int burley_diffusion(KGraph& g, int r, int d)
{
    const auto k     = [&](int like, double v) { return detail::kf(g, like, v); };
    const int  rd    = nodes::detail::bin(g, KOp::Div, r, d);                 // r/d (vec3)
    const int  e1    = g.unary(KOp::Exp, g.unary(KOp::Neg, rd));
    const int  e2    = g.unary(KOp::Exp, g.unary(KOp::Neg, nodes::detail::bin(g, KOp::Mul, rd, k(rd, 1.0 / 3.0)))); // exp(−r/3d); bin: rd is vec3, the 1/3 const is scalar
    const int  denom = nodes::detail::bin(g, KOp::Mul, nodes::detail::bin(g, KOp::Mul, d, k(d, 8.0 * vol::kPi)), r); // 8π·d·r
    return g.binary(KOp::Div, g.binary(KOp::Add, e1, e2), denom);
}
// sss_gaussian — a separable Gaussian blur weight (Jimenez separable SSS): `exp(−r²/(2σ²)) / sqrt(2π·σ²)`. The separable
// horizontal+vertical passes (a sum of these approximates the diffusion profile) are renderer-side; this is the kernel weight.
[[nodiscard]] inline int sss_gaussian(KGraph& g, int r, int variance)
{
    const auto k  = [&](int like, double v) { return detail::kf(g, like, v); };
    const int  r2 = g.binary(KOp::Mul, r, r);
    const int  e  = g.unary(KOp::Exp, g.unary(KOp::Neg, g.binary(KOp::Div, r2, g.binary(KOp::Mul, k(r, 2.0), variance))));
    return g.binary(KOp::Div, e, g.unary(KOp::Sqrt, g.binary(KOp::Mul, k(r, 2.0 * vol::kPi), variance)));
}

} // namespace crd::kir::screen
