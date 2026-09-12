#pragma once

// ckir_lighting.hpp — the shared CKIR LIGHTING LIBRARY (D-007 B8). The one surface→radiance evaluator every render path
// (Forward+, Deferred, and later the RT path) calls: given a lighting-agnostic OpenPBR surface (the B5 slab) + a light + the
// view, it returns outgoing radiance. B8-a is the Cook-Torrance microfacet CORE — a FAITHFUL transcription of Filament's
// `shaders/src/surface_brdf.fs` (the gold-standard real-time PBR reference), verified bit-exact on the CPU oracle and
// observable pixel-identical on both backends (the B5–B7 methodology). Later B8 sub-slices add the full OpenPBR lobe stack
// (aniso/clearcoat/sheen/thin-film/transmission/subsurface — B8-b), light types + the loop (B8-c), area lights (LTC, B8-d),
// and IBL (split-sum, B8-e).
//
// ROUGHNESS CONVENTION (Filament): the surface stores PERCEPTUAL roughness; the microfacet `D`/`V` take the LINEAR/alpha
// roughness `alpha = perceptual * perceptual`. The builders here name the alpha param `alpha` to keep that unambiguous; the
// combiner squares the surface's perceptual roughness once. All builders are CKIR value-graph builders (compose core KOps);
// no new device features — this rides the already-clean raster path.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_nodes.hpp>

namespace crd::kir::lighting
{

// Filament's PI literal — pinned for a bit-faithful match (a different π shifts every 1/π diffuse + D term).
inline constexpr double kPi = 3.14159265359;

namespace detail
{
[[nodiscard]] inline int kf(KGraph& g, int like, double v) { return g.constant(v, g.node(like).shape, g.node(like).dtype()); }
// pow5(x) = x²·x²·x  (Filament common_math.glsl — NOT pow(x,5); the factoring is part of the bit-exact contract).
[[nodiscard]] inline int pow5(KGraph& g, int x)
{
    const int x2 = g.binary(KOp::Mul, x, x);
    return g.binary(KOp::Mul, g.binary(KOp::Mul, x2, x2), x);
}
} // namespace detail

// D_GGX (Trowbridge-Reitz), Filament surface_brdf.fs: a = NoH·alpha ; k = min(alpha/((1−NoH²)+a²), 453.5) ; D = k·(k/π).
// (453.5 is Filament's fp16-overflow guard; kept for faithfulness — it only bites at a near-mirror highlight.)
[[nodiscard]] inline int d_ggx(KGraph& g, int noh, int alpha)
{
    const auto k1        = [&](double v) { return detail::kf(g, noh, v); };
    const int  a         = g.binary(KOp::Mul, noh, alpha);
    const int  one_m_nh2 = g.binary(KOp::Sub, k1(1.0), g.binary(KOp::Mul, noh, noh));                       // 1 − NoH²
    const int  denom     = g.binary(KOp::Add, one_m_nh2, g.binary(KOp::Mul, a, a));
    const int  kk        = g.binary(KOp::Min, g.binary(KOp::Div, alpha, denom), k1(453.5));
    return g.binary(KOp::Mul, kk, g.binary(KOp::Mul, kk, k1(1.0 / kPi)));                                    // k·(k·(1/π))
}

// V_SmithGGXCorrelated (Heitz 2014, height-correlated masking-shadowing; includes the 1/(4·NoL·NoV) of the BRDF):
//   a2 = alpha² ; λV = NoL·√((NoV − a2·NoV)·NoV + a2) ; λL = NoV·√((NoL − a2·NoL)·NoL + a2) ; V = 0.5 / max(λV+λL, ε).
[[nodiscard]] inline int v_smith_ggx_correlated(KGraph& g, int nov, int nol, int alpha)
{
    const auto k1  = [&](double v) { return detail::kf(g, nov, v); };
    const int  a2  = g.binary(KOp::Mul, alpha, alpha);
    const int  lv  = g.binary(KOp::Mul, nol, g.unary(KOp::Sqrt, g.binary(KOp::Add, g.binary(KOp::Mul, g.binary(KOp::Sub, nov, g.binary(KOp::Mul, a2, nov)), nov), a2)));
    const int  ll  = g.binary(KOp::Mul, nov, g.unary(KOp::Sqrt, g.binary(KOp::Add, g.binary(KOp::Mul, g.binary(KOp::Sub, nol, g.binary(KOp::Mul, a2, nol)), nol), a2)));
    return g.binary(KOp::Div, k1(0.5), g.binary(KOp::Max, g.binary(KOp::Add, lv, ll), k1(0.0000077))); // PREVENT_DIV0(0.5, λV+λL, 7.7e-6)
}

// F_Schlick (vec3 f0, scalar f90): f0 + (f90 − f0)·pow5(1 − VoH).
[[nodiscard]] inline int f_schlick(KGraph& g, int f0, int f90, int voh)
{
    const int t = detail::pow5(g, g.binary(KOp::Sub, detail::kf(g, voh, 1.0), voh));
    return nodes::detail::bin(g, KOp::Add, f0, nodes::detail::bin(g, KOp::Mul, nodes::detail::bin(g, KOp::Sub, f90, f0), t));
}
// F_Schlick (scalar f0, scalar f90) — the Fd_Burley scatter terms.
[[nodiscard]] inline int f_schlick_scalar(KGraph& g, int f0, int f90, int u)
{
    const int t = detail::pow5(g, g.binary(KOp::Sub, detail::kf(g, u, 1.0), u));
    return g.binary(KOp::Add, f0, g.binary(KOp::Mul, g.binary(KOp::Sub, f90, f0), t));
}

// Fd_Lambert = 1/π (a compile-time constant shaped like `like`).
[[nodiscard]] inline int fd_lambert(KGraph& g, int like) { return detail::kf(g, like, 1.0 / kPi); }
// Fd_Burley (Disney 2012 retro-reflective diffuse): f90 = 0.5 + 2·alpha·LoH² ; return Fs(NoL)·Fs(NoV)·(1/π).
[[nodiscard]] inline int fd_burley(KGraph& g, int nov, int nol, int loh, int alpha)
{
    const auto k1  = [&](double v) { return detail::kf(g, nov, v); };
    const int  f90 = g.binary(KOp::Add, k1(0.5), g.binary(KOp::Mul, g.binary(KOp::Mul, k1(2.0), alpha), g.binary(KOp::Mul, loh, loh)));
    const int  one = k1(1.0);
    const int  ls  = f_schlick_scalar(g, one, f90, nol);
    const int  vs  = f_schlick_scalar(g, one, f90, nov);
    return g.binary(KOp::Mul, g.binary(KOp::Mul, ls, vs), k1(1.0 / kPi));
}

// env_brdf_approx (Karis, "Physically Based Shading on Mobile") — the analytic DFG/environment-BRDF (scale, bias), so the
// multiscatter energy compensation needs no LUT in B8-a (the real prefiltered DFG LUT arrives with IBL at B8-e). Returns a
// vec2 = (scale, bias); `bias` (.y) drives the energy compensation. `perceptual` is PERCEPTUAL roughness.
[[nodiscard]] inline int env_brdf_approx(KGraph& g, int perceptual, int nov)
{
    const auto k1  = [&](double v) { return detail::kf(g, perceptual, v); };
    const int  c0  = g.vec4(k1(-1.0), k1(-0.0275), k1(-0.572), k1(0.022));
    const int  c1  = g.vec4(k1(1.0), k1(0.0425), k1(1.04), k1(-0.04));
    const int  r   = g.binary(KOp::Add, g.binary(KOp::Mul, g.splat(perceptual, 4), c0), c1);
    const int  rx  = g.swizzle(r, 0);
    const int  ry  = g.swizzle(r, 1);
    const int  a004 = g.binary(KOp::Add, g.binary(KOp::Mul, g.binary(KOp::Min, g.binary(KOp::Mul, rx, rx), g.unary(KOp::Exp2, g.binary(KOp::Mul, k1(-9.28), nov))), rx), ry);
    const int  sc  = g.binary(KOp::Add, g.binary(KOp::Mul, k1(-1.04), a004), g.swizzle(r, 2));
    const int  bi  = g.binary(KOp::Add, g.binary(KOp::Mul, k1(1.04), a004), g.swizzle(r, 3));
    return g.vec2(sc, bi);
}
// multiscatter energy compensation (Kulla-Conty / Fdez-Agüera, Filament): 1 + f0·(1/dfg.y − 1). vec3.
[[nodiscard]] inline int energy_compensation(KGraph& g, int f0, int dfg_bias)
{
    // The analytic env_brdf_approx BIAS (dfg.y) undershoots to NEGATIVE at high roughness (a known Karis-mobile-fit error) —
    // an unguarded `1/dfg.y` then flips ecmp large-negative and the surface renders BLACK for roughness ≳0.8. Floor the bias to
    // a small POSITIVE value (the DFG bias is physically ≥0), matching the codebase's PREVENT_DIV0 idiom. The physically-exact
    // high-roughness multiscatter needs the pre-integrated DFG LUT (a B8-e asset uploaded at B8-l); this keeps it bounded + non-black.
    const int bias = g.binary(KOp::Max, dfg_bias, detail::kf(g, dfg_bias, 1.0e-3));
    const int inv  = g.binary(KOp::Div, detail::kf(g, bias, 1.0), bias);
    return nodes::detail::bin(g, KOp::Add, detail::kf(g, f0, 1.0), nodes::detail::bin(g, KOp::Mul, f0, g.binary(KOp::Sub, inv, detail::kf(g, bias, 1.0))));
}

// ── the direct-light BRDF (single punctual light, radiance out) ──────────────────────────────────────────────────────
// brdf_direct(base_color, metallic, perceptual_roughness, N, V, L, light_color) — the Filament standard lit model for one
// light: metallic-roughness → f0/diffuseColor ; the half-vector angles ; Fr = D·V·F ; Fd = diffuseColor·Fd_Burley ;
// out = (Fd + Fr·energyCompensation)·NoL·light_color. All vectors are unit world-space; base_color/light_color are vec3.
[[nodiscard]] inline int brdf_direct(KGraph& g, int base_color, int metallic, int perceptual, int n, int v, int l, int light_color)
{
    namespace nd = nodes;
    const auto k1 = [&](double val) { return detail::kf(g, metallic, val); };
    // metallic-roughness workflow: dielectric f0 = 0.04, f0 = mix(0.04, base, metallic), diffuse = base·(1−metallic).
    const int f0        = nd::detail::tern(g, KOp::Mix, g.splat(k1(0.04), 3), base_color, metallic);
    const int diffuse_c = nd::detail::bin(g, KOp::Mul, base_color, g.binary(KOp::Sub, k1(1.0), metallic));
    // f90 = saturate(dot(f0, 50·0.33)) — Filament's grazing reflectance.
    const int f90 = nd::clamp01(g, g.dot(f0, g.splat(k1(50.0 * 0.33), 3)));
    const int alpha = g.binary(KOp::Mul, perceptual, perceptual); // linear/alpha roughness

    const int h   = g.normalize(nd::detail::bin(g, KOp::Add, v, l)); // half-vector
    const int noh = nd::clamp01(g, g.dot(n, h));
    const int nov = g.binary(KOp::Add, g.unary(KOp::Abs, g.dot(n, v)), k1(1e-5)); // abs+ε (Filament NoV floor)
    const int nol = nd::clamp01(g, g.dot(n, l));
    const int loh = nd::clamp01(g, g.dot(l, h));

    const int dterm = d_ggx(g, noh, alpha);
    const int vterm = v_smith_ggx_correlated(g, nov, nol, alpha);
    const int fterm = f_schlick(g, f0, f90, loh);                                                // vec3
    const int fr    = nd::detail::bin(g, KOp::Mul, g.splat(g.binary(KOp::Mul, dterm, vterm), 3), fterm); // (D·V)·F
    const int fd    = nd::detail::bin(g, KOp::Mul, diffuse_c, g.splat(fd_burley(g, nov, nol, loh, alpha), 3));

    const int dfg  = env_brdf_approx(g, perceptual, nov);
    const int ecmp = energy_compensation(g, f0, g.swizzle(dfg, 1));
    const int lit  = nd::detail::bin(g, KOp::Add, fd, nd::detail::bin(g, KOp::Mul, fr, ecmp));    // Fd + Fr·E
    return nd::detail::bin(g, KOp::Mul, nd::detail::bin(g, KOp::Mul, lit, g.splat(nol, 3)), light_color);
}

// ── B8-b: the OpenPBR LOBE STACK (layered on the B8-a core) ──────────────────────────────────────────────────────────
// Each lobe is a faithful transcription of its Filament function (surface_brdf.fs + surface_shading_model_*.fs). Roughness
// args named `alpha` are LINEAR (= perceptual²). The clean analytic lobes — anisotropic specular · sheen/fuzz · clearcoat ·
// subsurface — land here; thin-film iridescence (Belcour spectral Fresnel) and transmission's screen-space refraction ride
// the B8-b tail / the renderer (their weights/thickness/ior already live in the B5 slab).
inline constexpr double kMinRoughness = 0.007; // Filament MIN_ROUGHNESS — the anisotropic at/ab clamp

// D_GGX_Anisotropic (Burley 2012): a2=at·ab ; d=(ab·ToH, at·BoH, a2·NoH) ; return a2·(a2/‖d‖²)²·(1/π).
[[nodiscard]] inline int d_ggx_anisotropic(KGraph& g, int at, int ab, int toh, int boh, int noh)
{
    const int a2 = g.binary(KOp::Mul, at, ab);
    const int d  = g.vec3(g.binary(KOp::Mul, ab, toh), g.binary(KOp::Mul, at, boh), g.binary(KOp::Mul, a2, noh));
    const int b2 = g.binary(KOp::Div, a2, g.dot(d, d));
    // Filament `a2 * b2 * b2 * (1/π)` is LEFT-associative — ((a2·b2)·b2)·(1/π); the grouping is part of the bit contract.
    return g.binary(KOp::Mul, g.binary(KOp::Mul, g.binary(KOp::Mul, a2, b2), b2), detail::kf(g, noh, 1.0 / kPi));
}
// V_SmithGGXCorrelated_Anisotropic (Heitz 2014): λV=NoL·‖(at·ToV, ab·BoV, NoV)‖ ; λL=NoV·‖(at·ToL, ab·BoL, NoL)‖.
[[nodiscard]] inline int v_smith_ggx_correlated_anisotropic(KGraph& g, int at, int ab, int tov, int bov, int tol, int bol, int nov, int nol)
{
    const int lv = g.binary(KOp::Mul, nol, g.vlength(g.vec3(g.binary(KOp::Mul, at, tov), g.binary(KOp::Mul, ab, bov), nov)));
    const int ll = g.binary(KOp::Mul, nov, g.vlength(g.vec3(g.binary(KOp::Mul, at, tol), g.binary(KOp::Mul, ab, bol), nol)));
    return g.binary(KOp::Div, detail::kf(g, nov, 0.5), g.binary(KOp::Max, g.binary(KOp::Add, lv, ll), detail::kf(g, nov, 0.0000077)));
}
// anisotropic specular lobe (Filament anisotropicLobe): at=max(alpha·(1+aniso), MIN) ; ab=max(alpha·(1−aniso), MIN);
// (D·V)·F with the tangent frame (t, b). `n`,`v`,`l` unit; `f0` vec3; `alpha` linear roughness; `aniso` in [−1,1].
[[nodiscard]] inline int aniso_specular_lobe(KGraph& g, int f0, int f90, int alpha, int aniso, int t, int b, int n, int v, int l)
{
    const auto k1 = [&](double val) { return detail::kf(g, alpha, val); };
    const int  at = g.binary(KOp::Max, g.binary(KOp::Mul, alpha, g.binary(KOp::Add, k1(1.0), aniso)), k1(kMinRoughness));
    const int  ab = g.binary(KOp::Max, g.binary(KOp::Mul, alpha, g.binary(KOp::Sub, k1(1.0), aniso)), k1(kMinRoughness));
    const int  h  = g.normalize(nodes::detail::bin(g, KOp::Add, v, l));
    const int  nov = g.binary(KOp::Add, g.unary(KOp::Abs, g.dot(n, v)), k1(1e-5));
    const int  nol = nodes::clamp01(g, g.dot(n, l));
    const int  noh = nodes::clamp01(g, g.dot(n, h));
    const int  loh = nodes::clamp01(g, g.dot(l, h));
    const int  dd = d_ggx_anisotropic(g, at, ab, g.dot(t, h), g.dot(b, h), noh);
    const int  vv = v_smith_ggx_correlated_anisotropic(g, at, ab, g.dot(t, v), g.dot(b, v), g.dot(t, l), g.dot(b, l), nov, nol);
    return nodes::detail::bin(g, KOp::Mul, g.splat(g.binary(KOp::Mul, dd, vv), 3), f_schlick(g, f0, f90, loh));
}

// D_Charlie (sheen, Estevez-Kulla 2017): invα=1/α ; sin2h=max(1−NoH², 2⁻¹⁴) ; (2+invα)·sin2h^(invα/2)/(2π).
[[nodiscard]] inline int d_charlie(KGraph& g, int alpha, int noh)
{
    const auto k1   = [&](double v) { return detail::kf(g, noh, v); };
    const int  inva = g.binary(KOp::Div, k1(1.0), alpha);
    const int  sin2h = g.binary(KOp::Max, g.binary(KOp::Sub, k1(1.0), g.binary(KOp::Mul, noh, noh)), k1(0.0078125));
    return g.binary(KOp::Div, g.binary(KOp::Mul, g.binary(KOp::Add, k1(2.0), inva), g.binary(KOp::Pow, sin2h, g.binary(KOp::Mul, inva, k1(0.5)))), k1(2.0 * kPi));
}
// V_Neubelt (sheen visibility): 1 / max(4·(NoL+NoV−NoL·NoV), ε).
[[nodiscard]] inline int v_neubelt(KGraph& g, int nov, int nol)
{
    const auto k1 = [&](double v) { return detail::kf(g, nov, v); };
    const int  dn = g.binary(KOp::Mul, k1(4.0), g.binary(KOp::Sub, g.binary(KOp::Add, nol, nov), g.binary(KOp::Mul, nol, nov)));
    return g.binary(KOp::Div, k1(1.0), g.binary(KOp::Max, dn, k1(0.00001532)));
}
// sheen/fuzz lobe (Filament sheenLobe): (D_Charlie·V_Neubelt)·sheen_color. `sheen_alpha` linear.
[[nodiscard]] inline int sheen_lobe(KGraph& g, int sheen_color, int sheen_alpha, int nov, int nol, int noh)
{
    return nodes::detail::bin(g, KOp::Mul, sheen_color, g.splat(g.binary(KOp::Mul, d_charlie(g, sheen_alpha, noh), v_neubelt(g, nov, nol)), 3));
}

// V_Kelemen (clearcoat visibility): 0.25 / max(LoH², ε).
[[nodiscard]] inline int v_kelemen(KGraph& g, int loh) { return g.binary(KOp::Div, detail::kf(g, loh, 0.25), g.binary(KOp::Max, g.binary(KOp::Mul, loh, loh), detail::kf(g, loh, 0.0000039))); }
// clearcoat Fresnel (IOR 1.5, F0=0.04): F_Schlick(0.04, 1, LoH)·coat_weight — used both by the lobe and to attenuate the base.
[[nodiscard]] inline int clearcoat_fresnel(KGraph& g, int coat_weight, int loh) { return g.binary(KOp::Mul, f_schlick_scalar(g, detail::kf(g, loh, 0.04), detail::kf(g, loh, 1.0), loh), coat_weight); }
// clearcoat lobe (Filament clearCoatLobe): D_GGX(coat_alpha, NoH)·V_Kelemen(LoH)·Fcc. `coat_perceptual` is perceptual.
[[nodiscard]] inline int clearcoat_lobe(KGraph& g, int coat_weight, int coat_perceptual, int noh, int loh)
{
    const int alpha = g.binary(KOp::Mul, coat_perceptual, coat_perceptual);
    return g.binary(KOp::Mul, g.binary(KOp::Mul, d_ggx(g, noh, alpha), v_kelemen(g, loh)), clearcoat_fresnel(g, coat_weight, loh));
}

// subsurface transmission term (Filament surface_shading_model_subsurface.fs): a non-physical wrap/scatter BTDF —
//   scatterVoH = saturate(dot(V, −L)) ; fwd = exp2(scatterVoH·power − power) ; back = saturate(NoL·thick + (1−thick))·0.5 ;
//   ss = mix(back, 1, fwd)·(1−thick) ; return subsurface_color · (ss·(1/π)).  `nol` = saturate(N·L); `v`,`l` unit.
[[nodiscard]] inline int subsurface_term(KGraph& g, int subsurface_color, int power, int thickness, int nol, int v, int l)
{
    const auto k1   = [&](double val) { return detail::kf(g, nol, val); };
    const int  svoh = nodes::clamp01(g, g.dot(v, g.unary(KOp::Neg, l)));
    const int  fwd  = g.unary(KOp::Exp2, g.binary(KOp::Sub, g.binary(KOp::Mul, svoh, power), power));
    const int  back = g.binary(KOp::Mul, nodes::clamp01(g, g.binary(KOp::Add, g.binary(KOp::Mul, nol, thickness), g.binary(KOp::Sub, k1(1.0), thickness))), k1(0.5));
    const int  ss   = g.binary(KOp::Mul, nodes::detail::tern(g, KOp::Mix, back, k1(1.0), fwd), g.binary(KOp::Sub, k1(1.0), thickness));
    return nodes::detail::bin(g, KOp::Mul, subsurface_color, g.splat(g.binary(KOp::Mul, ss, k1(1.0 / kPi)), 3));
}

// ── B8-b (cont.): THIN-FILM IRIDESCENCE (Belcour-Barla) + TRANSMISSION/REFRACTION ───────────────────────────────────
// Faithful transcriptions of the glTF Sample Renderer (KHR_materials_iridescence / _transmission / _volume) — the spec
// reference for these OpenPBR layers. Pure analytic shader math (the only renderer-side piece is WHICH texture the
// refraction samples — a B2 sample the renderer binds at B8-l; the BTDF/absorption/Fresnel/direction are all here).
inline constexpr double kPiGltf = 3.141592653589793; // glTF M_PI (distinct from Filament's kPi — each source's own π)

namespace detail
{
[[nodiscard]] inline int sq(KGraph& g, int x) { return g.binary(KOp::Mul, x, x); } // glTF sq
[[nodiscard]] inline int kv3(KGraph& g, int like, double x, double y, double z) { return g.vec3(kf(g, like, x), kf(g, like, y), kf(g, like, z)); }
// evalSensitivity(OPD, shift) — the thin-film spectral sensitivity → sRGB (glTF iridescence.glsl). OPD scalar, shift vec3.
[[nodiscard]] inline int eval_sensitivity(KGraph& g, int opd, int shift)
{
    const auto ks    = [&](double v) { return kf(g, opd, v); };
    const int  phase = g.binary(KOp::Mul, g.binary(KOp::Mul, ks(2.0 * kPiGltf), opd), ks(1.0e-9)); // 2π·OPD·1e-9
    const int  val   = kv3(g, opd, 5.4856e-13, 4.4201e-13, 5.2481e-13);
    const int  pos   = kv3(g, opd, 1.6810e+06, 1.7953e+06, 2.2084e+06);
    const int  var   = kv3(g, opd, 4.3278e+09, 9.3046e+09, 6.6121e+09);
    const int  sqp   = g.binary(KOp::Mul, phase, phase);
    const int  b     = g.unary(KOp::Sqrt, g.binary(KOp::Mul, g.splat(ks(2.0 * kPiGltf), 3), var));      // sqrt(2π·var)
    const int  cterm = g.unary(KOp::Cos, g.binary(KOp::Add, g.binary(KOp::Mul, pos, g.splat(phase, 3)), shift)); // cos(pos·phase+shift)
    const int  dterm = g.unary(KOp::Exp, g.binary(KOp::Mul, g.unary(KOp::Neg, g.splat(sqp, 3)), var)); // exp(−phase²·var)
    int        xyz   = g.binary(KOp::Mul, g.binary(KOp::Mul, g.binary(KOp::Mul, val, b), cterm), dterm);
    // xyz.x += 9.7470e-14·sqrt(2π·4.5282e9)·cos(2.2399e6·phase+shift.x)·exp(−4.5282e9·phase²)
    const int extra = g.binary(KOp::Mul, g.binary(KOp::Mul, g.binary(KOp::Mul, ks(9.7470e-14), g.unary(KOp::Sqrt, ks(2.0 * kPiGltf * 4.5282e+09))), g.unary(KOp::Cos, g.binary(KOp::Add, g.binary(KOp::Mul, ks(2.2399e+06), phase), g.swizzle(shift, 0)))), g.unary(KOp::Exp, g.binary(KOp::Mul, ks(-4.5282e+09), sqp)));
    xyz = g.vec3(g.binary(KOp::Add, g.swizzle(xyz, 0), extra), g.swizzle(xyz, 1), g.swizzle(xyz, 2));
    xyz = g.binary(KOp::Div, xyz, g.splat(ks(1.0685e-7), 3)); // xyz /= 1.0685e-7
    // XYZ_TO_REC709 · xyz (glTF column-major mat3): result[row] = Σ col[row]·xyz[col].
    const int x = g.swizzle(xyz, 0);
    const int y = g.swizzle(xyz, 1);
    const int z = g.swizzle(xyz, 2);
    const auto row = [&](double c0, double c1, double c2) { return g.binary(KOp::Add, g.binary(KOp::Add, g.binary(KOp::Mul, ks(c0), x), g.binary(KOp::Mul, ks(c1), y)), g.binary(KOp::Mul, ks(c2), z)); };
    return g.vec3(row(3.2404542, -1.5371385, -0.4985314), row(-0.9692660, 1.8760108, 0.0415560), row(0.0556434, -0.2040259, 1.0572252));
}
} // namespace detail

// IorToFresnel0(transmitted, incident) = ((t−i)/(t+i))² — scalar + vec3 (i scalar).
[[nodiscard]] inline int ior_to_fresnel0(KGraph& g, int transmitted, int incident) { return detail::sq(g, g.binary(KOp::Div, g.binary(KOp::Sub, transmitted, incident), g.binary(KOp::Add, transmitted, incident))); }
[[nodiscard]] inline int ior_to_fresnel0_v(KGraph& g, int transmitted3, int incident) { return nodes::detail::bin(g, KOp::Mul, nodes::detail::bin(g, KOp::Div, nodes::detail::bin(g, KOp::Sub, transmitted3, incident), nodes::detail::bin(g, KOp::Add, transmitted3, incident)), nodes::detail::bin(g, KOp::Div, nodes::detail::bin(g, KOp::Sub, transmitted3, incident), nodes::detail::bin(g, KOp::Add, transmitted3, incident))); }
// Fresnel0ToIor(f0) = (1+√f0)/(1−√f0) — vec3.
[[nodiscard]] inline int fresnel0_to_ior(KGraph& g, int f0)
{
    const int s = g.unary(KOp::Sqrt, f0);
    return nodes::detail::bin(g, KOp::Div, nodes::detail::bin(g, KOp::Add, detail::kf(g, f0, 1.0), s), nodes::detail::bin(g, KOp::Sub, detail::kf(g, f0, 1.0), s));
}
// evalIridescence(outsideIOR, eta2, cosTheta1, thinFilmThickness, baseF0) — the Belcour-Barla thin-film Fresnel (glTF).
// Returns the iridescent reflectance (vec3) that replaces the base specular F0. `baseF0` vec3; the rest scalar.
[[nodiscard]] inline int eval_iridescence(KGraph& g, int outside_ior, int eta2, int cos_theta1, int thickness, int base_f0)
{
    namespace nd  = nodes;
    const auto ks = [&](double v) { return detail::kf(g, cos_theta1, v); };
    const int  irid_ior = g.ternary(KOp::Mix, outside_ior, eta2, g.ternary(KOp::Smoothstep, ks(0.0), ks(0.03), thickness));
    const int  sin2t2   = g.binary(KOp::Mul, detail::sq(g, g.binary(KOp::Div, outside_ior, irid_ior)), g.binary(KOp::Sub, ks(1.0), detail::sq(g, cos_theta1)));
    const int  cos2t2   = g.binary(KOp::Sub, ks(1.0), sin2t2);
    const int  cos_t2   = g.unary(KOp::Sqrt, g.binary(KOp::Max, cos2t2, ks(0.0))); // guard TIR (cos2t2<0 → the glTF vec3(1) case; tests stay in-range)
    const int  r0  = ior_to_fresnel0(g, irid_ior, outside_ior);
    const int  r12 = f_schlick_scalar(g, r0, ks(1.0), cos_theta1);
    const int  t121 = g.binary(KOp::Sub, ks(1.0), r12);
    const int  phi12 = g.select(g.binary(KOp::CmpLt, irid_ior, outside_ior), ks(kPiGltf), ks(0.0));
    const int  phi21 = g.binary(KOp::Sub, ks(kPiGltf), phi12);
    const int  base_ior = fresnel0_to_ior(g, nd::clamp(g, base_f0, ks(0.0), ks(0.9999)));
    const int  r1  = ior_to_fresnel0_v(g, base_ior, irid_ior);
    const int  r23 = f_schlick(g, r1, ks(1.0), cos_t2); // vec3 F_Schlick(f0=R1, f90=1, cosTheta2)
    // phi23 is PER-CHANNEL (base_ior[k] < irid_ior ? π : 0); core Select takes a SCALAR cond, so decompose per channel.
    const auto p23 = [&](int k) { return g.select(g.binary(KOp::CmpLt, g.swizzle(base_ior, k), irid_ior), ks(kPiGltf), ks(0.0)); };
    const int  phi23 = g.vec3(p23(0), p23(1), p23(2));
    const int  opd = g.binary(KOp::Mul, g.binary(KOp::Mul, g.binary(KOp::Mul, ks(2.0), irid_ior), thickness), cos_t2);
    const int  phi = nd::detail::bin(g, KOp::Add, g.splat(phi21, 3), phi23);
    const int  r123 = nd::clamp(g, nd::detail::bin(g, KOp::Mul, g.splat(r12, 3), r23), ks(1e-5), ks(0.9999));
    const int  r123r = g.unary(KOp::Sqrt, r123);
    const int  rs  = nd::detail::bin(g, KOp::Div, nd::detail::bin(g, KOp::Mul, g.splat(detail::sq(g, t121), 3), r23), nd::detail::bin(g, KOp::Sub, g.splat(ks(1.0), 3), r123));
    int        out_i = nd::detail::bin(g, KOp::Add, g.splat(r12, 3), rs); // C0 = R12 + Rs
    int        cm  = nd::detail::bin(g, KOp::Sub, rs, g.splat(t121, 3)); // Cm = Rs − T121
    for (int m = 1; m <= 2; ++m)
    {
        cm = nd::detail::bin(g, KOp::Mul, cm, r123r);
        const int sm = nd::detail::bin(g, KOp::Mul, g.splat(ks(2.0), 3), detail::eval_sensitivity(g, g.binary(KOp::Mul, ks(static_cast<double>(m)), opd), nd::detail::bin(g, KOp::Mul, g.splat(ks(static_cast<double>(m)), 3), phi)));
        out_i = nd::detail::bin(g, KOp::Add, out_i, nd::detail::bin(g, KOp::Mul, cm, sm));
    }
    return nd::detail::bin(g, KOp::Max, out_i, g.splat(ks(0.0), 3));
}

// ── transmission / refraction (glTF KHR_materials_transmission / _volume) ────────────────────────────────────────────
// applyVolumeAttenuation — Beer's law: transmittance = exp(−(−log(attenuationColor)/attenuationDistance)·distance).
[[nodiscard]] inline int volume_attenuation(KGraph& g, int radiance, int distance, int attenuation_color, int attenuation_distance)
{
    const int coeff = nodes::detail::bin(g, KOp::Div, g.unary(KOp::Neg, g.unary(KOp::Log, attenuation_color)), attenuation_distance);
    const int trans = g.unary(KOp::Exp, nodes::detail::bin(g, KOp::Mul, g.unary(KOp::Neg, coeff), distance));
    return nodes::detail::bin(g, KOp::Mul, trans, radiance);
}
// applyIorToRoughness(alpha, ior) = alpha·clamp(ior·2−2, 0, 1) — no refraction at IOR 1, default at 1.5.
[[nodiscard]] inline int apply_ior_to_roughness(KGraph& g, int alpha, int ior) { return g.binary(KOp::Mul, alpha, nodes::clamp(g, g.binary(KOp::Sub, g.binary(KOp::Mul, ior, detail::kf(g, alpha, 2.0)), detail::kf(g, alpha, 2.0)), detail::kf(g, alpha, 0.0), detail::kf(g, alpha, 1.0))); }
// the refraction ray direction (Snell, glTF getVolumeTransmissionRay core): refract(−v, n, 1/ior)·thickness. (The model-
// matrix scale + the scene-colour sample are the renderer's, at B8-l.)
[[nodiscard]] inline int refraction_ray(KGraph& g, int n, int v, int ior, int thickness)
{
    const int rv = g.refract(g.unary(KOp::Neg, v), g.normalize(n), g.binary(KOp::Div, detail::kf(g, ior, 1.0), ior));
    return nodes::detail::bin(g, KOp::Mul, g.normalize(rv), thickness);
}
// getPunctualRadianceTransmission (glTF): the BTDF for a punctual light through the surface — the specular lobe on the
// MIRRORED light direction, energy-split by (1−F), tinted by baseColor. `alpha` linear roughness; `f0`/`base_color` vec3.
[[nodiscard]] inline int transmission_btdf(KGraph& g, int base_color, int f0, int f90, int alpha, int ior, int n, int view, int l)
{
    namespace nd  = nodes;
    const auto ks = [&](double v) { return detail::kf(g, alpha, v); };
    const int  tr = apply_ior_to_roughness(g, alpha, ior);
    const int  nn = g.normalize(n);
    const int  vv = g.normalize(view);
    const int  ll = g.normalize(l);
    const int  l_mirror = g.normalize(nd::detail::bin(g, KOp::Add, ll, nd::detail::bin(g, KOp::Mul, nd::detail::bin(g, KOp::Mul, g.splat(ks(2.0), 3), nn), g.splat(g.dot(g.unary(KOp::Neg, ll), nn), 3)))); // l + 2n·dot(−l,n)
    const int  h   = g.normalize(nd::detail::bin(g, KOp::Add, l_mirror, vv));
    const int  noh = nd::clamp01(g, g.dot(nn, h));
    const int  voh = nd::clamp01(g, g.dot(vv, h));
    const int  nov = g.binary(KOp::Add, g.unary(KOp::Abs, g.dot(nn, vv)), ks(1e-5));
    const int  nolm = nd::clamp01(g, g.dot(nn, l_mirror));
    const int  d   = d_ggx(g, noh, tr);
    const int  vis = v_smith_ggx_correlated(g, nov, nolm, tr);
    const int  f   = f_schlick(g, f0, f90, voh);
    return nd::detail::bin(g, KOp::Mul, nd::detail::bin(g, KOp::Mul, nd::detail::bin(g, KOp::Mul, nd::detail::bin(g, KOp::Sub, g.splat(ks(1.0), 3), f), base_color), g.splat(d, 3)), g.splat(vis, 3)); // (1−F)·base·D·Vis
}

// ── B8-c: PUNCTUAL LIGHT TYPES — attenuation (Filament surface_light_punctual.fs) + the forward light loop ────────────
// Faithful transcription of Filament's punctual attenuation. saturate = clamp(x,0,1); falloff = 1/radius² (the reciprocal
// square of the light's influence radius). These are the per-light SHADER-side terms; the renderer feeds the light array
// (set-1 structured buffer, ADR-0102 D4) and unrolls/loops the accumulation — the loop MATH is here.
//
// getSquareFalloffAttenuation(dist², falloff): factor = dist²·falloff ; sf = saturate(1 − factor²) ; return sf².
[[nodiscard]] inline int square_falloff_attenuation(KGraph& g, int dist_sq, int falloff)
{
    const int factor = g.binary(KOp::Mul, dist_sq, falloff);
    const int sf     = nodes::clamp01(g, g.binary(KOp::Sub, detail::kf(g, dist_sq, 1.0), g.binary(KOp::Mul, factor, factor)));
    return g.binary(KOp::Mul, sf, sf);
}
// getDistanceAttenuation(posToLight, falloff): squareFalloff(|p|², falloff) / max(|p|², 1e-4). (Filament's optional far-plane
// fade is a frame-uniform global cull, not the point-light core — it rides the render path, not this shader term.)
[[nodiscard]] inline int distance_attenuation(KGraph& g, int pos_to_light, int falloff)
{
    const int d2 = g.dot(pos_to_light, pos_to_light);
    return g.binary(KOp::Div, square_falloff_attenuation(g, d2, falloff), g.binary(KOp::Max, d2, detail::kf(g, d2, 1e-4)));
}
// getAngleAttenuation(lightDir, l, (scale, offset)): cd = dot(lightDir, l) ; a = saturate(cd·scale + offset) ; return a².
// (scale/offset precompute the inner/outer cone: scale = 1/max(cosInner−cosOuter, 1e-4), offset = −cosOuter·scale.)
[[nodiscard]] inline int spot_angle_attenuation(KGraph& g, int light_dir, int l, int scale, int offset)
{
    const int cd  = g.dot(light_dir, l);
    const int att = nodes::clamp01(g, g.binary(KOp::Add, g.binary(KOp::Mul, cd, scale), offset));
    return g.binary(KOp::Mul, att, att);
}
// punctual_radiance: ONE light's attenuated direct contribution — brdf_direct·attenuation (scalar broadcast to the vec3).
[[nodiscard]] inline int punctual_radiance(KGraph& g, int base_color, int metallic, int perceptual, int n, int v, int l, int light_color, int attenuation)
{
    return nodes::detail::bin(g, KOp::Mul, brdf_direct(g, base_color, metallic, perceptual, n, v, l, light_color), g.splat(attenuation, 3));
}
// directional light: L = −normalize(direction), attenuation = 1.
[[nodiscard]] inline int directional_light(KGraph& g, int base, int metallic, int perceptual, int n, int v, int direction, int light_color)
{
    return brdf_direct(g, base, metallic, perceptual, n, v, g.normalize(g.unary(KOp::Neg, direction)), light_color);
}
// point light: inverse-square smooth-window falloff (falloff = 1/radius²).
[[nodiscard]] inline int point_light(KGraph& g, int base, int metallic, int perceptual, int n, int v, int world_pos, int light_pos, int light_color, int falloff)
{
    const int p   = g.binary(KOp::Sub, light_pos, world_pos); // posToLight
    const int l   = g.normalize(p);
    const int att = distance_attenuation(g, p, falloff);
    return punctual_radiance(g, base, metallic, perceptual, n, v, l, light_color, att);
}
// spot light: point falloff · cone-angle attenuation. `spot_dir` = the (unit) spotlight axis; Filament negates it for the
// cone test. (scale, offset) precomputed from the inner/outer cone half-angles.
[[nodiscard]] inline int spot_light(KGraph& g, int base, int metallic, int perceptual, int n, int v, int world_pos, int light_pos, int light_color, int falloff, int spot_dir, int scale, int offset)
{
    const int p   = g.binary(KOp::Sub, light_pos, world_pos);
    const int l   = g.normalize(p);
    const int att = g.binary(KOp::Mul, distance_attenuation(g, p, falloff), spot_angle_attenuation(g, g.unary(KOp::Neg, spot_dir), l, scale, offset));
    return punctual_radiance(g, base, metallic, perceptual, n, v, l, light_color, att);
}

// ── B8-d: AREA LIGHTS — Heitz Linearly-Transformed Cosines (LTC) ──────────────────────────────────────────────────────
// Faithful transcription of Heitz et al. "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines" (2016) —
// the gold-standard real-time area-light method (the selfshadow/ltc_code reference). The CLIPLESS form: transform the
// polygon by the fitted LTC matrix Minv, project + normalize the vertices, sum the per-edge form factors, scale by the
// horizon-clip magnitude (`scale`, the ltc_2 LUT `.w`). Minv + scale come from the fitted LTC LUT keyed by (roughness, NoV);
// the diffuse term uses Minv = identity + the un-clipped Lambertian normalisation. LUT_SIZE 64.
inline constexpr double kLtcLutSize  = 64.0;
inline constexpr double kLtcLutScale = (kLtcLutSize - 1.0) / kLtcLutSize; // 63/64
inline constexpr double kLtcLutBias  = 0.5 / kLtcLutSize;                 // 0.5/64

// IntegrateEdgeVec(v1, v2): the vector form factor of one polygon edge — the acos rational approximation (Heitz).
[[nodiscard]] inline int integrate_edge_vec(KGraph& g, int v1, int v2)
{
    const int  x  = g.dot(v1, v2);
    const auto ks = [&](double c) { return detail::kf(g, x, c); };
    const int  y  = g.unary(KOp::Abs, x);
    const int  a  = g.binary(KOp::Add, ks(0.8543985), g.binary(KOp::Mul, g.binary(KOp::Add, ks(0.4965155), g.binary(KOp::Mul, ks(0.0145206), y)), y));
    const int  b  = g.binary(KOp::Add, ks(3.4175940), g.binary(KOp::Mul, g.binary(KOp::Add, ks(4.1616724), y), y));
    const int  vv = g.binary(KOp::Div, a, b);
    // theta_sintheta = (x > 0) ? v : 0.5·rsqrt(max(1 − x², 1e-7)) − v
    const int  alt = g.binary(KOp::Sub, g.binary(KOp::Mul, ks(0.5), g.unary(KOp::Rsqrt, g.binary(KOp::Max, g.binary(KOp::Sub, ks(1.0), g.binary(KOp::Mul, x, x)), ks(1e-7)))), vv);
    const int  tst = g.select(g.binary(KOp::CmpGt, x, ks(0.0)), vv, alt);
    return nodes::detail::bin(g, KOp::Mul, g.cross(v1, v2), g.splat(tst, 3));
}
[[nodiscard]] inline int integrate_edge(KGraph& g, int v1, int v2) { return g.swizzle(integrate_edge_vec(g, v1, v2), 2); }

// LTC_Evaluate for a RECTANGLE (clipless). N/V unit world; P = shading point; Minv = the fitted LTC matrix (identity for the
// clamped-cosine diffuse term); p0..p3 = the rect corners (CCW, world). `scale` = the ltc_2 horizon-clip magnitude — pass
// 1/(2π) for the un-clipped Lambertian diffuse. `two_sided` lights skip the back-face cull. Returns the (≥0) form factor.
[[nodiscard]] inline int ltc_evaluate_rect(KGraph& g, int n, int v, int p, int minv, int p0, int p1, int p2, int p3, int scale, bool two_sided)
{
    namespace nd  = nodes;
    const int  nov = g.dot(v, n);
    const auto ks  = [&](double c) { return detail::kf(g, nov, c); };
    // orthonormal basis around N: T1 = normalize(V − N·(V·N)), T2 = cross(N, T1).
    const int  t1 = g.normalize(g.binary(KOp::Sub, v, nd::detail::bin(g, KOp::Mul, n, g.splat(nov, 3))));
    const int  t2 = g.cross(n, t1);
    // Minv = Minv · transpose(mat3(T1, T2, N)) — rotate the light into the shading frame.
    const int  m  = g.mat_mul(minv, g.mat_transpose(g.mat3(t1, t2, n)));
    const auto proj = [&](int pi) { return g.normalize(g.mat_mul_vec(m, g.binary(KOp::Sub, pi, p))); };
    const int  l0 = proj(p0);
    const int  l1 = proj(p1);
    const int  l2 = proj(p2);
    const int  l3 = proj(p3);
    // behind = dot(points[0]−P, cross(points[1]−points[0], points[3]−points[0])) < 0  (back-face test)
    const int  lnrm   = g.cross(g.binary(KOp::Sub, p1, p0), g.binary(KOp::Sub, p3, p0));
    const int  behind = g.binary(KOp::CmpLt, g.dot(g.binary(KOp::Sub, p0, p), lnrm), ks(0.0));
    // vsum = Σ IntegrateEdgeVec (4 edges, wrapped) ; len = |vsum| ; sum = len·scale (ltc_2 horizon-clip).
    int vsum = integrate_edge_vec(g, l0, l1);
    vsum = nd::detail::bin(g, KOp::Add, vsum, integrate_edge_vec(g, l1, l2));
    vsum = nd::detail::bin(g, KOp::Add, vsum, integrate_edge_vec(g, l2, l3));
    vsum = nd::detail::bin(g, KOp::Add, vsum, integrate_edge_vec(g, l3, l0));
    const int len = g.vlength(vsum);
    int       sum = g.binary(KOp::Mul, len, scale);
    if (!two_sided) { sum = g.select(behind, ks(0.0), sum); }
    return sum;
}
// The ltc_2 LUT coordinate for a rect: uv = (z·0.5 + 0.5, len)·LUT_SCALE + LUT_BIAS where z = ±vsum.z/len. Kept as a helper
// so the renderer samples the real fitted ltc_2 texture for the specular magnitude; `vsum` is the raw Σ IntegrateEdgeVec.
[[nodiscard]] inline int ltc_lut_uv(KGraph& g, int vsum, int behind)
{
    const auto ks  = [&](double c) { return detail::kf(g, g.swizzle(vsum, 2), c); };
    const int  len = g.vlength(vsum);
    int        z   = g.binary(KOp::Div, g.swizzle(vsum, 2), len);
    z = g.select(behind, g.unary(KOp::Neg, z), z);
    const int u = g.binary(KOp::Add, g.binary(KOp::Mul, g.binary(KOp::Add, g.binary(KOp::Mul, z, ks(0.5)), ks(0.5)), ks(kLtcLutScale)), ks(kLtcLutBias));
    const int w = g.binary(KOp::Add, g.binary(KOp::Mul, len, ks(kLtcLutScale)), ks(kLtcLutBias));
    return g.vec2(u, w);
}

// ── LINE / TUBE area lights (Heitz ltc_line.fs) ──────────────────────────────────────────────────────────────────────
// The tube (capsule) light integrates a line segment against the LTC. `Fpo`/`Fwt` are the two closed-form line-integral
// primitives; `i_diffuse_line` the clamped-cosine line integral (with horizon clipping); `i_ltc_line` the transformed
// integral (Minv + the |inverse(transpose(Minv))·ortho| weight); `ltc_evaluate_line` transforms the endpoints into the
// shading frame and scales by the tube RADIUS. The line-integral normalisation uses ltc_code's PI (3.14159265).
inline constexpr double kLtcPi = 3.14159265; // ltc_code's PI literal (NOT Filament's kPi — a distinct constant)

namespace detail
{
[[nodiscard]] inline int fpo(KGraph& g, int d, int l) // l/(d·(d²+l²)) + atan(l/d)/d²
{
    const int d2l2  = g.binary(KOp::Add, g.binary(KOp::Mul, d, d), g.binary(KOp::Mul, l, l));
    const int term1 = g.binary(KOp::Div, l, g.binary(KOp::Mul, d, d2l2));
    const int term2 = g.binary(KOp::Div, g.unary(KOp::Atan, g.binary(KOp::Div, l, d)), g.binary(KOp::Mul, d, d));
    return g.binary(KOp::Add, term1, term2);
}
[[nodiscard]] inline int fwt(KGraph& g, int d, int l) { return g.binary(KOp::Div, g.binary(KOp::Mul, l, l), g.binary(KOp::Mul, d, g.binary(KOp::Add, g.binary(KOp::Mul, d, d), g.binary(KOp::Mul, l, l)))); } // l²/(d·(d²+l²))
} // namespace detail

// I_diffuse_line — the clamped-cosine line integral of segment p1→p2 (in the shading frame), with z=0 horizon clipping.
[[nodiscard]] inline int i_diffuse_line(KGraph& g, int p1, int p2)
{
    namespace nd  = nodes;
    const int  wt  = g.normalize(g.binary(KOp::Sub, p2, p1)); // computed BEFORE clipping (matches the reference)
    const int  p1z = g.swizzle(p1, 2);
    const int  p2z = g.swizzle(p2, 2);
    const auto ks  = [&](double c) { return detail::kf(g, p1z, c); };
    // clip an endpoint that dips below the horizon to z=0 (branchless): only one fires when the segment isn't fully below.
    const int clip1 = nd::detail::bin(g, KOp::Div, nd::detail::bin(g, KOp::Sub, nd::detail::bin(g, KOp::Mul, p1, g.splat(p2z, 3)), nd::detail::bin(g, KOp::Mul, p2, g.splat(p1z, 3))), g.splat(g.binary(KOp::Sub, p2z, p1z), 3));
    const int clip2 = nd::detail::bin(g, KOp::Div, nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, g.unary(KOp::Neg, p1), g.splat(p2z, 3)), nd::detail::bin(g, KOp::Mul, p2, g.splat(p1z, 3))), g.splat(g.binary(KOp::Add, g.unary(KOp::Neg, p2z), p1z), 3));
    const int p1c = g.select(g.binary(KOp::CmpLt, p1z, ks(0.0)), clip1, p1);
    const int p2c = g.select(g.binary(KOp::CmpLt, p2z, ks(0.0)), clip2, p2);
    const int l1  = g.dot(p1c, wt);
    const int l2  = g.dot(p2c, wt);
    const int po  = g.binary(KOp::Sub, p1c, nd::detail::bin(g, KOp::Mul, g.splat(l1, 3), wt));
    const int d   = g.vlength(po);
    const int i   = g.binary(KOp::Add, g.binary(KOp::Mul, g.binary(KOp::Sub, detail::fpo(g, d, l2), detail::fpo(g, d, l1)), g.swizzle(po, 2)), g.binary(KOp::Mul, g.binary(KOp::Sub, detail::fwt(g, d, l2), detail::fwt(g, d, l1)), g.swizzle(wt, 2)));
    const int ipi = g.binary(KOp::Div, i, ks(kLtcPi));
    // if BOTH endpoints are below the horizon → 0 (the reference's early return; select avoids the clip's 0/0).
    return g.select(g.binary(KOp::CmpLe, p1z, ks(0.0)), g.select(g.binary(KOp::CmpLe, p2z, ks(0.0)), ks(0.0), ipi), ipi);
}
// I_ltc_line — the LTC line integral: transform by Minv, integrate, reweight by 1/|inverse(transpose(Minv))·ortho|.
[[nodiscard]] inline int i_ltc_line(KGraph& g, int minv, int p1, int p2)
{
    const int i_diffuse = i_diffuse_line(g, g.mat_mul_vec(minv, p1), g.mat_mul_vec(minv, p2));
    const int ortho     = g.normalize(g.cross(p1, p2));
    const int w         = g.binary(KOp::Div, detail::kf(g, i_diffuse, 1.0), g.vlength(g.mat_mul_vec(g.mat_inverse(g.mat_transpose(minv)), ortho)));
    return g.binary(KOp::Mul, w, i_diffuse);
}
// ltc_evaluate_line — a TUBE light from endpoints pa/pb (world) with radius `radius`. Minv = identity for the diffuse term.
[[nodiscard]] inline int ltc_evaluate_line(KGraph& g, int n, int v, int p, int minv, int pa, int pb, int radius)
{
    namespace nd  = nodes;
    const int nov = g.dot(v, n);
    const int t1  = g.normalize(g.binary(KOp::Sub, v, nd::detail::bin(g, KOp::Mul, n, g.splat(nov, 3))));
    const int t2  = g.cross(n, t1);
    const int bm  = g.mat_transpose(g.mat3(t1, t2, n)); // B = transpose(mat3(T1,T2,N))
    const int p1  = g.mat_mul_vec(bm, g.binary(KOp::Sub, pa, p));
    const int p2  = g.mat_mul_vec(bm, g.binary(KOp::Sub, pb, p));
    return g.binary(KOp::Mul, radius, i_ltc_line(g, minv, p1, p2));
}

// ── DISK / SPHERE area lights (Heitz ltc_disk.fs) ────────────────────────────────────────────────────────────────────
// SolveCubic (Blinn) — the three real roots of the ellipse's characteristic cubic. coeff = vec4(c0,c1,c2,c3). Returns a
// vec3 of roots sorted so root.y is the "middle" one (which the disk eval reads for avgDir/formFactor).
[[nodiscard]] inline int solve_cubic(KGraph& g, int coeff)
{
    const int  cw  = g.swizzle(coeff, 3);
    const auto ks  = [&](double c) { return detail::kf(g, cw, c); };
    const int  aa  = cw;                                                                           // A = Coefficient.w
    const int  cx  = g.binary(KOp::Div, g.swizzle(coeff, 0), cw);                                  // Coefficient.x = c0/w
    const int  cy  = g.binary(KOp::Div, g.binary(KOp::Div, g.swizzle(coeff, 1), cw), ks(3.0));     // .y = (c1/w)/3
    const int  cz  = g.binary(KOp::Div, g.binary(KOp::Div, g.swizzle(coeff, 2), cw), ks(3.0));     // .z = (c2/w)/3
    const int  bb  = cz;                                                                           // B
    const int  cc  = cy;                                                                           // C
    const int  dd  = cx;                                                                           // D
    const int  dx  = g.binary(KOp::Add, g.unary(KOp::Neg, g.binary(KOp::Mul, cz, cz)), cy);        // Delta.x = -z²+y
    const int  dy  = g.binary(KOp::Add, g.unary(KOp::Neg, g.binary(KOp::Mul, cy, cz)), cx);        // Delta.y = -y·z+x
    const int  dz  = g.binary(KOp::Sub, g.binary(KOp::Mul, cz, cx), g.binary(KOp::Mul, cy, cy));   // Delta.z = z·x-y²
    const int  disc = g.binary(KOp::Sub, g.binary(KOp::Mul, g.binary(KOp::Mul, ks(4.0), dx), dz), g.binary(KOp::Mul, dy, dy)); // 4·Δx·Δz − Δy²
    const int  sd  = g.unary(KOp::Sqrt, disc);
    const int  t23 = ks((2.0 / 3.0) * kLtcPi);
    // block A (largest root)
    const int  da_a = g.binary(KOp::Add, g.binary(KOp::Mul, g.binary(KOp::Mul, ks(-2.0), bb), dx), dy);  // -2B·Δx+Δy
    const int  th_a = g.binary(KOp::Div, g.binary(KOp::Atan2, sd, g.unary(KOp::Neg, da_a)), ks(3.0));
    const int  sc_a = g.binary(KOp::Mul, ks(2.0), g.unary(KOp::Sqrt, g.unary(KOp::Neg, dx)));
    const int  x1a = g.binary(KOp::Mul, sc_a, g.unary(KOp::Cos, th_a));
    const int  x3a = g.binary(KOp::Mul, sc_a, g.unary(KOp::Cos, g.binary(KOp::Add, th_a, t23)));
    const int  xl  = g.select(g.binary(KOp::CmpGt, g.binary(KOp::Add, x1a, x3a), g.binary(KOp::Mul, ks(2.0), bb)), x1a, x3a);
    const int  xlx = g.binary(KOp::Sub, xl, bb);
    const int  xly = aa;
    // block D (smallest root)
    const int  dd_d = g.binary(KOp::Add, g.binary(KOp::Mul, g.unary(KOp::Neg, dd), dy), g.binary(KOp::Mul, g.binary(KOp::Mul, ks(2.0), cc), dz)); // -D·Δy+2C·Δz
    const int  th_d = g.binary(KOp::Div, g.binary(KOp::Atan2, g.binary(KOp::Mul, dd, sd), g.unary(KOp::Neg, dd_d)), ks(3.0));
    const int  sc_d = g.binary(KOp::Mul, ks(2.0), g.unary(KOp::Sqrt, g.unary(KOp::Neg, dz)));
    const int  x1d = g.binary(KOp::Mul, sc_d, g.unary(KOp::Cos, th_d));
    const int  x3d = g.binary(KOp::Mul, sc_d, g.unary(KOp::Cos, g.binary(KOp::Add, th_d, t23)));
    const int  xs  = g.select(g.binary(KOp::CmpLt, g.binary(KOp::Add, x1d, x3d), g.binary(KOp::Mul, ks(2.0), cc)), x1d, x3d);
    const int  xsx = g.unary(KOp::Neg, dd);
    const int  xsy = g.binary(KOp::Add, xs, cc);
    // combine (the middle root via the resultant)
    const int  ee = g.binary(KOp::Mul, xly, xsy);
    const int  ff = g.binary(KOp::Sub, g.unary(KOp::Neg, g.binary(KOp::Mul, xlx, xsy)), g.binary(KOp::Mul, xly, xsx));
    const int  gg = g.binary(KOp::Mul, xlx, xsx);
    const int  xmx = g.binary(KOp::Sub, g.binary(KOp::Mul, cc, ff), g.binary(KOp::Mul, bb, gg));
    const int  xmy = g.binary(KOp::Add, g.unary(KOp::Neg, g.binary(KOp::Mul, bb, ff)), g.binary(KOp::Mul, cc, ee));
    const int  r0 = g.binary(KOp::Div, xsx, xsy);
    const int  r1 = g.binary(KOp::Div, xmx, xmy);
    const int  r2 = g.binary(KOp::Div, xlx, xly);
    // sort: cond1 (r0 smallest) → (r1,r0,r2) ; else cond2 (r2 smallest) → (r0,r2,r1) ; else (r0,r1,r2)
    const int  rd = g.vec3(r0, r1, r2);
    const int  rc2 = g.vec3(r0, r2, r1);
    const int  rc1 = g.vec3(r1, r0, r2);
    const int  inner = g.select(g.binary(KOp::CmpLt, r2, r0), g.select(g.binary(KOp::CmpLt, r2, r1), rc2, rd), rd);
    return g.select(g.binary(KOp::CmpLt, r0, r1), g.select(g.binary(KOp::CmpLt, r0, r2), rc1, inner), inner);
}

// LTC_Evaluate for a DISK (Heitz ltc_disk.fs). p0/p1/p2 encode the disk's bounding parallelogram (C = ½(L0+L2), the two
// radius vectors from L1); Minv the fitted LTC matrix; `scale` the ltc_2 magnitude. A SPHERE light = the disk facing P.
// Returns the form factor. The ellipse eigendecomposition + the V3 orientation flip are done branchless (compute + select).
[[nodiscard]] inline int ltc_evaluate_disk(KGraph& g, int n, int v, int p, int minv, int p0, int p1, int p2, int scale, bool two_sided)
{
    namespace nd  = nodes;
    const int  nov = g.dot(v, n);
    const auto ks  = [&](double c) { return detail::kf(g, nov, c); };
    const int  t1 = g.normalize(g.binary(KOp::Sub, v, nd::detail::bin(g, KOp::Mul, n, g.splat(nov, 3))));
    const int  t2 = g.cross(n, t1);
    const int  rmat = g.mat_transpose(g.mat3(t1, t2, n));
    const int  ll0 = g.mat_mul_vec(rmat, g.binary(KOp::Sub, p0, p));
    const int  ll1 = g.mat_mul_vec(rmat, g.binary(KOp::Sub, p1, p));
    const int  ll2 = g.mat_mul_vec(rmat, g.binary(KOp::Sub, p2, p));
    const int  cpre  = nd::detail::bin(g, KOp::Mul, g.splat(ks(0.5), 3), nd::detail::bin(g, KOp::Add, ll0, ll2)); // C  = ½(L0+L2)
    const int  v1pre = nd::detail::bin(g, KOp::Mul, g.splat(ks(0.5), 3), nd::detail::bin(g, KOp::Sub, ll1, ll2)); // V1 = ½(L1−L2)
    const int  v2pre = nd::detail::bin(g, KOp::Mul, g.splat(ks(0.5), 3), nd::detail::bin(g, KOp::Sub, ll1, ll0)); // V2 = ½(L1−L0)
    const int  cc = g.mat_mul_vec(minv, cpre);
    const int  V1 = g.mat_mul_vec(minv, v1pre);
    const int  V2 = g.mat_mul_vec(minv, v2pre);
    const int  cull = g.binary(KOp::CmpLt, g.dot(g.cross(V1, V2), cc), ks(0.0)); // dot(cross(V1,V2),C) < 0
    const int  d11 = g.dot(V1, V1);
    const int  d22 = g.dot(V2, V2);
    const int  d12 = g.dot(V1, V2);
    // GENERAL ellipse path (|d12| not negligible): eigenvalues of the 2×2 form.
    const int  tr   = g.binary(KOp::Add, d11, d22);
    const int  det  = g.unary(KOp::Sqrt, g.binary(KOp::Add, g.unary(KOp::Neg, g.binary(KOp::Mul, d12, d12)), g.binary(KOp::Mul, d11, d22)));
    const int  u    = g.binary(KOp::Mul, ks(0.5), g.unary(KOp::Sqrt, g.binary(KOp::Sub, tr, g.binary(KOp::Mul, ks(2.0), det))));
    const int  vv   = g.binary(KOp::Mul, ks(0.5), g.unary(KOp::Sqrt, g.binary(KOp::Add, tr, g.binary(KOp::Mul, ks(2.0), det))));
    const int  emax = detail::sq(g, g.binary(KOp::Add, u, vv));
    const int  emin = detail::sq(g, g.binary(KOp::Sub, u, vv));
    const int  d11gt = g.binary(KOp::CmpGt, d11, d22);
    const int  v1a = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, g.splat(d12, 3), V1), nd::detail::bin(g, KOp::Mul, g.splat(g.binary(KOp::Sub, emax, d11), 3), V2));
    const int  v2a = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, g.splat(d12, 3), V1), nd::detail::bin(g, KOp::Mul, g.splat(g.binary(KOp::Sub, emin, d11), 3), V2));
    const int  v1b = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, g.splat(d12, 3), V2), nd::detail::bin(g, KOp::Mul, g.splat(g.binary(KOp::Sub, emax, d22), 3), V1));
    const int  v2b = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, g.splat(d12, 3), V2), nd::detail::bin(g, KOp::Mul, g.splat(g.binary(KOp::Sub, emin, d22), 3), V1));
    const int  v1gen = g.normalize(g.select(d11gt, v1a, v1b));
    const int  v2gen = g.normalize(g.select(d11gt, v2a, v2b));
    const int  agen = g.binary(KOp::Div, ks(1.0), emax);
    const int  bgen = g.binary(KOp::Div, ks(1.0), emin);
    // AXIS-ALIGNED path (|d12| negligible).
    const int  aax = g.binary(KOp::Div, ks(1.0), d11);
    const int  bax = g.binary(KOp::Div, ks(1.0), d22);
    const int  v1ax = nd::detail::bin(g, KOp::Mul, V1, g.splat(g.unary(KOp::Sqrt, aax), 3));
    const int  v2ax = nd::detail::bin(g, KOp::Mul, V2, g.splat(g.unary(KOp::Sqrt, bax), 3));
    const int  ecc = g.binary(KOp::CmpGt, g.binary(KOp::Div, g.unary(KOp::Abs, d12), g.unary(KOp::Sqrt, g.binary(KOp::Mul, d11, d22))), ks(0.0001));
    const int  aa = g.select(ecc, agen, aax);
    const int  bb = g.select(ecc, bgen, bax);
    const int  vv1 = g.select(ecc, v1gen, v1ax);
    const int  vv2 = g.select(ecc, v2gen, v2ax);
    int        v3  = g.cross(vv1, vv2);
    v3 = g.select(g.binary(KOp::CmpLt, g.dot(cc, v3), ks(0.0)), g.unary(KOp::Neg, v3), v3);
    const int  ld  = g.dot(v3, cc);
    const int  x0  = g.binary(KOp::Div, g.dot(vv1, cc), ld);
    const int  y0  = g.binary(KOp::Div, g.dot(vv2, cc), ld);
    const int  l2  = g.binary(KOp::Mul, ld, ld);
    const int  a2  = g.binary(KOp::Mul, aa, l2); // a *= L²
    const int  b2  = g.binary(KOp::Mul, bb, l2);
    const int  x0s = g.binary(KOp::Mul, x0, x0);
    const int  y0s = g.binary(KOp::Mul, y0, y0);
    const int  k0  = g.binary(KOp::Mul, a2, b2);
    const int  k1  = g.binary(KOp::Sub, g.binary(KOp::Sub, g.binary(KOp::Mul, k0, g.binary(KOp::Add, g.binary(KOp::Add, ks(1.0), x0s), y0s)), a2), b2);
    const int  k2  = g.binary(KOp::Sub, g.binary(KOp::Sub, ks(1.0), g.binary(KOp::Mul, a2, g.binary(KOp::Add, ks(1.0), x0s))), g.binary(KOp::Mul, b2, g.binary(KOp::Add, ks(1.0), y0s)));
    const int  roots = solve_cubic(g, g.vec4(k0, k1, k2, ks(1.0)));
    const int  e1r = g.swizzle(roots, 0);
    const int  e2r = g.swizzle(roots, 1);
    const int  e3r = g.swizzle(roots, 2);
    const int  adx = g.binary(KOp::Div, g.binary(KOp::Mul, a2, x0), g.binary(KOp::Sub, a2, e2r));
    const int  ady = g.binary(KOp::Div, g.binary(KOp::Mul, b2, y0), g.binary(KOp::Sub, b2, e2r));
    const int  avg = g.normalize(g.mat_mul_vec(g.mat3(vv1, vv2, v3), g.vec3(adx, ady, ks(1.0))));
    const int  ll1r = g.unary(KOp::Sqrt, g.binary(KOp::Div, g.unary(KOp::Neg, e2r), e3r));
    const int  ll2r = g.unary(KOp::Sqrt, g.binary(KOp::Div, g.unary(KOp::Neg, e2r), e1r));
    const int  ffac = g.binary(KOp::Mul, g.binary(KOp::Mul, ll1r, ll2r), g.unary(KOp::Rsqrt, g.binary(KOp::Mul, g.binary(KOp::Add, ks(1.0), g.binary(KOp::Mul, ll1r, ll1r)), g.binary(KOp::Add, ks(1.0), g.binary(KOp::Mul, ll2r, ll2r)))));
    int        spec = g.binary(KOp::Mul, ffac, scale);
    (void)avg; // avgDir.z drives the ltc_2 uv; the renderer samples it — `scale` carries the sampled magnitude here.
    if (!two_sided) { spec = g.select(cull, ks(0.0), spec); }
    return spec;
}
// the ltc_2 LUT coord for a disk: uv = (avgDir.z·0.5 + 0.5, formFactor)·LUT_SCALE + LUT_BIAS. Exposed so the renderer can
// sample the fitted magnitude; here `avg_z` = avgDir.z and `form_factor` the disk form factor.
[[nodiscard]] inline int ltc_disk_lut_uv(KGraph& g, int avg_z, int form_factor)
{
    const auto ks = [&](double c) { return detail::kf(g, avg_z, c); };
    const int  u  = g.binary(KOp::Add, g.binary(KOp::Mul, g.binary(KOp::Add, g.binary(KOp::Mul, avg_z, ks(0.5)), ks(0.5)), ks(kLtcLutScale)), ks(kLtcLutBias));
    const int  w  = g.binary(KOp::Add, g.binary(KOp::Mul, form_factor, ks(kLtcLutScale)), ks(kLtcLutBias));
    return g.vec2(u, w);
}

// ── the LTC LUT: sample → Minv → magnitude ───────────────────────────────────────────────────────────────────────────
// The fitted LTC LUT is a 64×64 texture pair the RENDERER uploads at runtime via the B2 sampler path (ltc_1 = the 4 Minv
// coefficients, ltc_2 = magnitude/Fresnel), keyed by uv = (roughness, √(1−NoV)) or (roughness, NoV). These build the uv and
// reconstruct the matrix on the shader side; the diffuse-vs-specular split feeds ltc_evaluate_rect/disk/line.
// ltc_lut_coord: uv into the LTC LUT for a (perceptual roughness, NoV) shading point (the ltc_code convention).
[[nodiscard]] inline int ltc_lut_coord(KGraph& g, int perceptual, int nov)
{
    const auto ks = [&](double c) { return detail::kf(g, nov, c); };
    // u = √(1 − NoV) (a cosine-warp so the grazing angles get more LUT resolution), v = roughness ; then LUT_SCALE/BIAS.
    const int u = g.unary(KOp::Sqrt, g.binary(KOp::Sub, ks(1.0), nov));
    const int c = g.binary(KOp::Add, nodes::detail::bin(g, KOp::Mul, g.vec2(u, perceptual), g.splat(ks(kLtcLutScale), 2)), g.splat(ks(kLtcLutBias), 2));
    return c;
}
// ltc_matrix: reconstruct the ISOTROPIC LTC Minv (3×3) from the 4-coefficient ltc_1 sample t1=(a,b,c,d). The sparse form
// columns = (a,0,c),(0,1,0),(b,0,d) — the standard runtime LTC matrix.
[[nodiscard]] inline int ltc_matrix(KGraph& g, int t1)
{
    const auto ks = [&](double c) { return detail::kf(g, g.swizzle(t1, 0), c); };
    return g.mat3(g.vec3(g.swizzle(t1, 0), ks(0.0), g.swizzle(t1, 2)), g.vec3(ks(0.0), ks(1.0), ks(0.0)), g.vec3(g.swizzle(t1, 1), ks(0.0), g.swizzle(t1, 3)));
}
// ltc_matrix_aniso: the ANISOTROPIC-GGX Minv from a fuller 6-coefficient fit (roughness × anisotropy × NoV LUT). The aniso
// fit lights up the off-diagonal (m01/m10) terms the isotropic form leaves zero; the eval (rect/disk/line) is IDENTICAL —
// only the fitted matrix changes. t1=(m00,m02,m20,m22), t2=(m01,m11) with m11 the tangent-scale (isotropic ⇒ t2=(0,1)).
[[nodiscard]] inline int ltc_matrix_aniso(KGraph& g, int t1, int t2)
{
    const auto ks = [&](double c) { return detail::kf(g, g.swizzle(t1, 0), c); };
    return g.mat3(g.vec3(g.swizzle(t1, 0), g.swizzle(t2, 0), g.swizzle(t1, 2)), g.vec3(g.swizzle(t2, 0), g.swizzle(t2, 1), ks(0.0)), g.vec3(g.swizzle(t1, 1), ks(0.0), g.swizzle(t1, 3)));
}

// ── B8-e: IMAGE-BASED LIGHTING (Karis split-sum specular + SH L2 irradiance diffuse) ─────────────────────────────────
// The runtime IBL evals (per-pixel): diffuse = the L2 spherical-harmonic irradiance (Filament's pre-convolved form) ×
// diffuse colour; specular = the prefiltered environment radiance × the split-sum DFG term (Karis). The prefiltered env
// cubemap (roughness mips), the DFG LUT, and the 9 SH coefficients are precomputed RENDERER assets (the generation math —
// importance sampling + DFG integration — is below); these functions consume the sampled results.
//
// Irradiance_SphericalHarmonics (Filament surface_light_indirect.fs): a linear combination of L2 polynomial terms in the
// unit normal. sh[0..8] = the 9 RGB coefficients (already fold the cosine convolution + basis constants). max(·, 0).
[[nodiscard]] inline int sh_irradiance(KGraph& g, int n, const int sh[9])
{
    namespace nd  = nodes;
    const int  nx = g.swizzle(n, 0);
    const int  ny = g.swizzle(n, 1);
    const int  nz = g.swizzle(n, 2);
    const auto ks = [&](double c) { return detail::kf(g, nx, c); };
    const int  t4 = g.binary(KOp::Mul, ny, nx);
    const int  t5 = g.binary(KOp::Mul, ny, nz);
    const int  t6 = g.binary(KOp::Sub, g.binary(KOp::Mul, g.binary(KOp::Mul, ks(3.0), nz), nz), ks(1.0)); // (3·n.z)·n.z − 1
    const int  t7 = g.binary(KOp::Mul, nz, nx);
    const int  t8 = g.binary(KOp::Sub, g.binary(KOp::Mul, nx, nx), g.binary(KOp::Mul, ny, ny));
    // band1 = sh1·n.y + sh2·n.z + sh3·n.x ; band2 = sh4·t4 + … + sh8·t8 (each band summed, then accumulated — Filament order).
    const int  band1 = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, sh[1], ny), nd::detail::bin(g, KOp::Mul, sh[2], nz)), nd::detail::bin(g, KOp::Mul, sh[3], nx));
    int        band2 = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, sh[4], t4), nd::detail::bin(g, KOp::Mul, sh[5], t5));
    band2 = nd::detail::bin(g, KOp::Add, band2, nd::detail::bin(g, KOp::Mul, sh[6], t6));
    band2 = nd::detail::bin(g, KOp::Add, band2, nd::detail::bin(g, KOp::Mul, sh[7], t7));
    band2 = nd::detail::bin(g, KOp::Add, band2, nd::detail::bin(g, KOp::Mul, sh[8], t8));
    const int  r = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Add, sh[0], band1), band2);
    return nd::detail::bin(g, KOp::Max, r, ks(0.0));
}
// ibl_diffuse: the diffuse IBL contribution = diffuse_color · irradiance (the SH irradiance already carries the 1/π + cosine).
[[nodiscard]] inline int ibl_diffuse(KGraph& g, int diffuse_color, int irradiance) { return nodes::detail::bin(g, KOp::Mul, diffuse_color, irradiance); }
// ibl_specular: the Karis split-sum specular = prefiltered_radiance · (F0·dfg.scale + dfg.bias) · multiscatter-energy-comp.
// `prefiltered` = the roughness-mip env sample at R = reflect(−V,N); dfg from the analytic env-BRDF (the real DFG LUT is a
// renderer texture — swap env_brdf_approx for a sampled (scale,bias) there). f0 vec3.
[[nodiscard]] inline int ibl_specular(KGraph& g, int prefiltered, int f0, int perceptual, int nov)
{
    namespace nd  = nodes;
    const int  dfg   = env_brdf_approx(g, perceptual, nov); // (scale, bias)
    const int  scale = g.swizzle(dfg, 0);
    const int  bias  = g.swizzle(dfg, 1);
    const int  term  = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, f0, scale), bias); // F0·scale + bias
    return nd::detail::bin(g, KOp::Mul, nd::detail::bin(g, KOp::Mul, prefiltered, term), energy_compensation(g, f0, bias));
}

// ── IBL GENERATION (precompute): importance sampling + the split-sum DFG integrand ──────────────────────────────────
// These run as PRECOMPUTE compute passes (once per env map): the DFG LUT = the average of `dfg_integrand` over a low-
// discrepancy sample set; the prefiltered radiance mips = the importance-sampled env average using the same GGX sampler.
// The sample point `u` ∈ [0,1)² comes from a Hammersley/Van-der-Corput sequence (CKIR `Shl`/`Shr`/`BitAnd` with explicit
// 32-bit masks — the oracle's bitwise runs through i64, so mask each step) — fed here as input so the analytic core is
// isolated. `roughness` is the LINEAR (alpha) roughness.
//
// importanceSamplingNdfDggx (Filament) — a GGX-NDF-distributed half-vector in TANGENT space (N = +z) from a 2D sample.
[[nodiscard]] inline int importance_sample_ggx(KGraph& g, int u, int roughness)
{
    const auto ks   = [&](double c) { return detail::kf(g, roughness, c); };
    const int  ux   = g.swizzle(u, 0);
    const int  uy   = g.swizzle(u, 1);
    const int  a2   = g.binary(KOp::Mul, roughness, roughness);
    const int  phi  = g.binary(KOp::Mul, ks(2.0 * kPi), ux);
    const int  cos2 = g.binary(KOp::Div, g.binary(KOp::Sub, ks(1.0), uy), g.binary(KOp::Add, ks(1.0), g.binary(KOp::Mul, g.binary(KOp::Sub, a2, ks(1.0)), uy))); // (1−u.y)/(1+(a²−1)u.y)
    const int  cost = g.unary(KOp::Sqrt, cos2);
    const int  sint = g.unary(KOp::Sqrt, g.binary(KOp::Sub, ks(1.0), cos2));
    return g.vec3(g.binary(KOp::Mul, g.unary(KOp::Cos, phi), sint), g.binary(KOp::Mul, g.unary(KOp::Sin, phi), sint), cost);
}
// the split-sum DFG integrand (Karis): one sample's (A, B) contribution. V = (√(1−NoV²), 0, NoV), N = +z. Averaging this
// over the sample set yields the (scale, bias) DFG LUT that `ibl_specular` samples. IBL Smith geometry: k = roughness²/2.
[[nodiscard]] inline int dfg_integrand(KGraph& g, int u, int nov, int roughness)
{
    const auto ks  = [&](double c) { return detail::kf(g, nov, c); };
    const int  h   = importance_sample_ggx(g, u, roughness);
    const int  vv  = g.vec3(g.unary(KOp::Sqrt, g.binary(KOp::Sub, ks(1.0), g.binary(KOp::Mul, nov, nov))), ks(0.0), nov); // V
    const int  voh = g.dot(vv, h);
    const int  noh = g.swizzle(h, 2);
    const int  nol = g.binary(KOp::Sub, g.binary(KOp::Mul, g.binary(KOp::Mul, ks(2.0), voh), noh), nov); // L.z = 2·VoH·H.z − V.z
    const int  k   = g.binary(KOp::Mul, g.binary(KOp::Mul, roughness, roughness), ks(0.5));              // k = roughness²/2
    const auto g1  = [&](int x) { return g.binary(KOp::Div, x, g.binary(KOp::Add, g.binary(KOp::Mul, x, g.binary(KOp::Sub, ks(1.0), k)), k)); }; // x/(x(1−k)+k)
    const int  gvis = g.binary(KOp::Div, g.binary(KOp::Mul, g.binary(KOp::Mul, g1(nov), g1(nol)), voh), g.binary(KOp::Mul, noh, nov)); // G·VoH/(NoH·NoV)
    const int  fc   = detail::pow5(g, g.binary(KOp::Sub, ks(1.0), voh));
    const int  a    = g.binary(KOp::Mul, g.binary(KOp::Sub, ks(1.0), fc), gvis);
    const int  b    = g.binary(KOp::Mul, fc, gvis);
    return g.select(g.binary(KOp::CmpGt, nol, ks(0.0)), g.vec2(a, b), g.vec2(ks(0.0), ks(0.0))); // NoL>0 guard
}

// ── B8-f: SHADOW-MAP FOUNDATION + the BIAS STACK (kills acne + peter-panning) ─────────────────────────────────────────
// The shader-side shadow foundation: project a world position into a light's shadow map, apply the gold-standard 3-part
// bias stack (normal-offset · slope-scaled · receiver-plane, MJP/Isidoro), and hardware-PCF compare (KOp::SampleCmp on the
// B2-b comparison-sampler + depth-texture path). The PassType::Shadow depth RENDER (rendering the map from the light) is
// the renderer/frame-graph leaf (B8-k/l); these consume the shadow map. Bias references: Holbert (normal offset) · Isidoro
// GDC 2006 (receiver-plane) · Pettineo "A Sampling of Shadow Techniques".
//
// shadow_project — light_vp·(worldPos,1) → NDC → (uv, depth). Returns vec3 (uv.x, uv.y, depth). The caller's light_vp bakes
// the [0,1] clip-depth + UV convention; uv = ndc.xy·0.5 + 0.5.
[[nodiscard]] inline int shadow_project(KGraph& g, int world_pos, int light_vp)
{
    namespace nd  = nodes;
    const auto ks = [&](double c) { return detail::kf(g, g.swizzle(world_pos, 0), c); };
    const int  wp4  = g.vec4(g.swizzle(world_pos, 0), g.swizzle(world_pos, 1), g.swizzle(world_pos, 2), ks(1.0));
    const int  clip = g.mat_mul_vec(light_vp, wp4);
    const int  invw = g.binary(KOp::Div, ks(1.0), g.swizzle(clip, 3));
    const int  ndc  = nd::detail::bin(g, KOp::Mul, g.vec3(g.swizzle(clip, 0), g.swizzle(clip, 1), g.swizzle(clip, 2)), invw); // clip.xyz / w
    const int  uv   = nd::detail::bin(g, KOp::Add, nd::detail::bin(g, KOp::Mul, g.vec2(g.swizzle(ndc, 0), g.swizzle(ndc, 1)), ks(0.5)), ks(0.5));
    return g.vec3(g.swizzle(uv, 0), g.swizzle(uv, 1), g.swizzle(ndc, 2));
}
// normal_offset_bias — push the receiver along its normal by scale·sin(incidence) (sin = √(1−NoL²)); moves the sample off the
// surface at grazing angles to kill self-shadow acne. `scale` bakes the shadow-map world-space texel size.
[[nodiscard]] inline int normal_offset_bias(KGraph& g, int world_pos, int normal, int nol, int scale)
{
    const auto ks    = [&](double c) { return detail::kf(g, nol, c); };
    const int  sin_a = g.unary(KOp::Sqrt, g.binary(KOp::Max, g.binary(KOp::Sub, ks(1.0), g.binary(KOp::Mul, nol, nol)), ks(0.0)));
    return nodes::detail::bin(g, KOp::Add, world_pos, nodes::detail::bin(g, KOp::Mul, normal, g.splat(g.binary(KOp::Mul, scale, sin_a), 3)));
}
// slope_scaled_bias — base·tan(incidence) = base·√(1−NoL²)/NoL, clamped to [0, max]; subtracted from the receiver depth.
[[nodiscard]] inline int slope_scaled_bias(KGraph& g, int nol, int base, int max_bias)
{
    const auto ks    = [&](double c) { return detail::kf(g, nol, c); };
    const int  tan_a = g.binary(KOp::Div, g.unary(KOp::Sqrt, g.binary(KOp::Max, g.binary(KOp::Sub, ks(1.0), g.binary(KOp::Mul, nol, nol)), ks(0.0))), nol);
    return nodes::clamp(g, g.binary(KOp::Mul, base, tan_a), ks(0.0), max_bias);
}
// receiver_plane_bias (Isidoro 2006) — from the screen-space derivatives dx=ddx(shadowCoord.xyz), dy=ddy(shadowCoord.xyz),
// the depth gradient (∂depth/∂u, ∂depth/∂v) in UV space; the per-texel-offset bias = dot(offset, this). Feeds B8-g PCF.
[[nodiscard]] inline int receiver_plane_bias(KGraph& g, int dx, int dy)
{
    const int dxx = g.swizzle(dx, 0);
    const int dxy = g.swizzle(dx, 1);
    const int dxz = g.swizzle(dx, 2);
    const int dyx = g.swizzle(dy, 0);
    const int dyy = g.swizzle(dy, 1);
    const int dyz = g.swizzle(dy, 2);
    const int bx  = g.binary(KOp::Sub, g.binary(KOp::Mul, dyy, dxz), g.binary(KOp::Mul, dxy, dyz)); // dy.y·dx.z − dx.y·dy.z
    const int by  = g.binary(KOp::Sub, g.binary(KOp::Mul, dxx, dyz), g.binary(KOp::Mul, dyx, dxz)); // dx.x·dy.z − dy.x·dx.z
    const int det = g.binary(KOp::Sub, g.binary(KOp::Mul, dxx, dyy), g.binary(KOp::Mul, dxy, dyx)); // dx.x·dy.y − dx.y·dy.x
    return nodes::detail::bin(g, KOp::Mul, g.vec2(bx, by), g.splat(g.binary(KOp::Div, detail::kf(g, det, 1.0), det), 2));
}
// shadow_factor — the B8-f foundation: normal-offset bias → project → slope-scaled depth bias → hardware-PCF compare. Returns
// visibility ∈ [0,1] (1 = lit). `tex`/`samp` = a depth texture + comparison sampler. (Receiver-plane per-tap bias = B8-g PCF.)
[[nodiscard]] inline int shadow_factor(KGraph& g, int world_pos, int normal, int nol, int light_vp, int tex, int samp, int normal_scale, int slope_base, int slope_max)
{
    const int proj  = shadow_project(g, normal_offset_bias(g, world_pos, normal, nol, normal_scale), light_vp);
    const int uv    = g.vec2(g.swizzle(proj, 0), g.swizzle(proj, 1));
    const int depth = g.binary(KOp::Sub, g.swizzle(proj, 2), slope_scaled_bias(g, nol, slope_base, slope_max));
    return g.tex_sample_cmp(tex, samp, uv, depth);
}

// ── B8-g: FILTERED SOFT SHADOWS — PCF · PCSS · EVSM · Moment Shadow Maps ──────────────────────────────────────────────
// The shader-side soft-shadow filters over B8-f's shadow map. PCF/PCSS ride the comparison-sampler `tex_sample_cmp` path;
// EVSM/MSM read filterable moment maps (a float render target the shadow pass writes — B8-k/l) and their RESOLVE is here.
namespace detail
{
// interleaved gradient noise (Jimenez, CoD:AW) — a cheap per-pixel [0,1) hash of FragCoord for rotating the PCF kernel.
[[nodiscard]] inline int ign(KGraph& g, int frag_xy)
{
    const auto ks = [&](double c) { return kf(g, g.swizzle(frag_xy, 0), c); };
    const int  d  = g.binary(KOp::Add, g.binary(KOp::Mul, g.swizzle(frag_xy, 0), ks(0.06711056)), g.binary(KOp::Mul, g.swizzle(frag_xy, 1), ks(0.00583715)));
    return g.unary(KOp::Fract, g.binary(KOp::Mul, ks(52.9829189), g.unary(KOp::Fract, d)));
}
// the fixed 8-tap Poisson disk (unit) used by PCF/PCSS.
[[nodiscard]] inline int poisson8(KGraph& g, int like, int i)
{
    static constexpr double px[8] = {-0.7071, 0.0, -0.5303, 0.6187, 0.8750, -0.8750, 0.3536, -0.3536};
    static constexpr double py[8] = {0.7071, -0.8750, -0.5303, 0.6187, 0.0, 0.0, -0.3536, 0.3536};
    return g.vec2(kf(g, like, px[i]), kf(g, like, py[i]));
}
} // namespace detail

// pcf_shadow — rotated-Poisson percentage-closer filtering: 8 taps rotated by a per-pixel IGN angle, each hardware-PCF
// compared, averaged → anti-aliased soft edges. `radius` = filter radius in UV; `frag_xy` = FragCoord.xy.
[[nodiscard]] inline int pcf_shadow(KGraph& g, int tex, int samp, int uv, int depth, int radius, int frag_xy)
{
    namespace nd  = nodes;
    const auto ks = [&](double c) { return detail::kf(g, radius, c); };
    const int  ang = g.binary(KOp::Mul, detail::ign(g, frag_xy), ks(6.28318530718)); // IGN·2π
    const int  cs  = g.unary(KOp::Cos, ang);
    const int  sn  = g.unary(KOp::Sin, ang);
    int        sum = ks(0.0);
    for (int i = 0; i < 8; ++i)
    {
        const int p  = detail::poisson8(g, radius, i);
        const int px = g.swizzle(p, 0);
        const int py = g.swizzle(p, 1);
        const int rx = g.binary(KOp::Sub, g.binary(KOp::Mul, px, cs), g.binary(KOp::Mul, py, sn)); // rotate2
        const int ry = g.binary(KOp::Add, g.binary(KOp::Mul, px, sn), g.binary(KOp::Mul, py, cs));
        const int suv = nd::detail::bin(g, KOp::Add, uv, nd::detail::bin(g, KOp::Mul, g.vec2(rx, ry), g.splat(radius, 2)));
        sum = g.binary(KOp::Add, sum, g.tex_sample_cmp(tex, samp, suv, depth));
    }
    return g.binary(KOp::Mul, sum, ks(1.0 / 8.0));
}
// pcss_penumbra — the PCSS penumbra ratio from the average blocker depth: (z_receiver − z_blocker)/z_blocker · light_size.
// The variable PCF radius = this. (The blocker search that yields z_blocker samples the map — the observable's caller.)
[[nodiscard]] inline int pcss_penumbra(KGraph& g, int z_receiver, int z_blocker, int light_size)
{
    return g.binary(KOp::Mul, g.binary(KOp::Div, g.binary(KOp::Sub, z_receiver, z_blocker), z_blocker), light_size);
}

// ── EVSM (Lauritzen) — the exp-warped, filterable variance shadow map ────────────────────────────────────────────────
// evsm_warp — warp a depth into the positive + negative exponential domain (kills VSM light-bleed): (exp(c⁺·d), −exp(−c⁻·d)).
[[nodiscard]] inline int evsm_warp(KGraph& g, int depth, int c_pos, int c_neg)
{
    const int wp = g.unary(KOp::Exp, g.binary(KOp::Mul, c_pos, depth));
    const int wn = g.unary(KOp::Neg, g.unary(KOp::Exp, g.binary(KOp::Mul, g.unary(KOp::Neg, c_neg), depth)));
    return g.vec2(wp, wn);
}
// chebyshev_bound — the one-tailed Chebyshev upper bound on P(depth ≥ t): variance / (variance + (t − mean)²), clamped so
// the lit case returns 1. `min_var` floors the variance (numerical). Returns the (un-bleed-reduced) visibility.
[[nodiscard]] inline int chebyshev_bound(KGraph& g, int m1, int m2, int t, int min_var)
{
    const auto ks   = [&](double c) { return detail::kf(g, m1, c); };
    const int  var  = g.binary(KOp::Max, g.binary(KOp::Sub, m2, g.binary(KOp::Mul, m1, m1)), min_var); // max(m2−m1², minVar)
    const int  d    = g.binary(KOp::Sub, t, m1);
    const int  pmax = g.binary(KOp::Div, var, g.binary(KOp::Add, var, g.binary(KOp::Mul, d, d))); // var/(var+d²)
    return g.select(g.binary(KOp::CmpLe, t, m1), ks(1.0), pmax); // t ≤ mean ⇒ fully lit
}
// reduce_light_bleed — linstep(amount, 1, p): remaps the Chebyshev bound to darken the residual VSM light leak.
[[nodiscard]] inline int reduce_light_bleed(KGraph& g, int p, int amount)
{
    const auto ks = [&](double c) { return detail::kf(g, p, c); };
    return nodes::clamp01(g, g.binary(KOp::Div, g.binary(KOp::Sub, p, amount), g.binary(KOp::Sub, ks(1.0), amount)));
}
// evsm_shadow — resolve the filtered 4-moment EVSM map: warp the receiver depth, Chebyshev on BOTH warps, take the min,
// bleed-reduce. `moments` = the (filtered) (m1⁺, m2⁺, m1⁻, m2⁻). Returns visibility ∈ [0,1].
[[nodiscard]] inline int evsm_shadow(KGraph& g, int moments, int depth, int c_pos, int c_neg, int min_var, int bleed)
{
    const int w  = evsm_warp(g, depth, c_pos, c_neg);
    const int pp = chebyshev_bound(g, g.swizzle(moments, 0), g.swizzle(moments, 1), g.swizzle(w, 0), min_var);
    const int pn = chebyshev_bound(g, g.swizzle(moments, 2), g.swizzle(moments, 3), g.swizzle(w, 1), min_var);
    return reduce_light_bleed(g, g.binary(KOp::Min, pp, pn), bleed);
}

// ── Moment Shadow Maps (Peters-Klein I3D 2015) — the 4-moment filterable soft shadow ──────────────────────────────────
// msm_hamburger — reconstruct the shadow intensity from the (filtered) 4 power moments b=(b1..b4) at fragment depth z via
// the Hamburger moment problem: bias, Cholesky-factor the Hankel matrix, solve for the quadratic whose roots bound z, then
// the 3-case form factor. Returns visibility ∈ [0,1] (1 = lit).
[[nodiscard]] inline int msm_hamburger(KGraph& g, int moments, int z, int depth_bias, int moment_bias)
{
    const auto ks = [&](double c) { return detail::kf(g, z, c); };
    // bias toward the moments of a uniform distribution (kills quantisation ringing). moment_bias is scalar → splat to vec4
    // (Mix/ternary is same-shape; a raw scalar factor makes the oracle read OOB on comps 1..3).
    const int b = g.ternary(KOp::Mix, moments, g.vec4(ks(0.0), ks(0.375), ks(0.0), ks(0.375)), g.splat(moment_bias, 4));
    const int b0 = g.swizzle(b, 0);
    const int b1 = g.swizzle(b, 1);
    const int b2 = g.swizzle(b, 2);
    const int b3 = g.swizzle(b, 3);
    const int z0 = g.binary(KOp::Sub, z, depth_bias);
    // Cholesky of [[1,b0,b1],[b0,b1,b2],[b1,b2,b3]] (only the needed products).
    const int l21d11 = g.binary(KOp::Add, g.unary(KOp::Neg, g.binary(KOp::Mul, b0, b1)), b2);  // b2 − b0·b1
    const int d11    = g.binary(KOp::Add, g.unary(KOp::Neg, g.binary(KOp::Mul, b0, b0)), b1);  // b1 − b0²
    const int sqvar  = g.binary(KOp::Add, g.unary(KOp::Neg, g.binary(KOp::Mul, b1, b1)), b3);  // b3 − b1²
    const int d22d11 = g.binary(KOp::Sub, g.binary(KOp::Mul, sqvar, d11), g.binary(KOp::Mul, l21d11, l21d11)); // D22·D11
    const int invd11 = g.binary(KOp::Div, ks(1.0), d11);
    const int l21    = g.binary(KOp::Mul, l21d11, invd11);
    const int invd22 = g.binary(KOp::Div, d11, d22d11);
    // forward: L·c1 = (1, z0, z0²)
    int c1 = g.binary(KOp::Sub, z0, b0);
    int c2 = g.binary(KOp::Sub, g.binary(KOp::Sub, g.binary(KOp::Mul, z0, z0), b1), g.binary(KOp::Mul, l21, c1));
    // D·c2 = c1
    c1 = g.binary(KOp::Mul, c1, invd11);
    c2 = g.binary(KOp::Mul, c2, invd22);
    // backward: Lᵀ·c3 = c2
    c1 = g.binary(KOp::Sub, c1, g.binary(KOp::Mul, l21, c2));
    const int c0 = g.binary(KOp::Sub, ks(1.0), g.binary(KOp::Add, g.binary(KOp::Mul, c1, b0), g.binary(KOp::Mul, c2, b1)));
    // solve c2·x² + c1·x + c0 = 0 for the two depths z1 ≤ z2.
    const int p  = g.binary(KOp::Div, c1, c2);
    const int q  = g.binary(KOp::Div, c0, c2);
    const int r  = g.unary(KOp::Sqrt, g.binary(KOp::Max, g.binary(KOp::Sub, g.binary(KOp::Mul, g.binary(KOp::Mul, p, p), ks(0.25)), q), ks(0.0)));
    const int z1 = g.binary(KOp::Sub, g.binary(KOp::Mul, g.unary(KOp::Neg, p), ks(0.5)), r);
    const int z2 = g.binary(KOp::Add, g.binary(KOp::Mul, g.unary(KOp::Neg, p), ks(0.5)), r);
    // 3-case shadow intensity (Peters-Klein): switch on (z2<z0) / (z1<z0).
    const int caseA = g.binary(KOp::CmpLt, z2, z0);
    const int caseB = g.binary(KOp::CmpLt, z1, z0);
    const int sA = g.vec4(z1, z0, ks(1.0), ks(1.0));
    const int sB = g.vec4(z0, z1, ks(0.0), ks(1.0));
    const int sC = g.vec4(ks(0.0), ks(0.0), ks(0.0), ks(0.0));
    const int sw = g.select(caseA, sA, g.select(caseB, sB, sC));
    const int s0 = g.swizzle(sw, 0);
    const int quotient = g.binary(KOp::Div, g.binary(KOp::Add, g.binary(KOp::Sub, g.binary(KOp::Mul, s0, z2), g.binary(KOp::Mul, b0, g.binary(KOp::Add, s0, z2))), b1), g.binary(KOp::Mul, g.binary(KOp::Sub, z2, g.swizzle(sw, 1)), g.binary(KOp::Sub, z0, z1)));
    const int intensity = g.binary(KOp::Add, g.swizzle(sw, 2), g.binary(KOp::Mul, g.swizzle(sw, 3), quotient));
    return g.binary(KOp::Sub, ks(1.0), nodes::clamp01(g, intensity)); // visibility = 1 − shadow
}

// ── B8-h: CASCADED SHADOW MAPS (the gold-standard directional shadow system) ──────────────────────────────────────────
// The shader-side CSM math: the practical (log-uniform) split scheme, cascade selection, Valient texel-snap stabilization
// (no shimmer), and smooth cross-cascade blending. The per-cascade shadow maps live in a 2D-array atlas the shadow pass
// renders (B8-k/l — the device has no float/array-depth upload yet); each cascade samples via B8-g PCF + B8-f bias.
//
// csm_split_practical — the Zhang PSSM 2006 practical split: blend the logarithmic (perspective-correct) + uniform splits.
// Returns the far distance of cascade `i` of `count`. λ=1 → pure log, λ=0 → pure uniform.
[[nodiscard]] inline int csm_split_practical(KGraph& g, int near, int far, int lambda, int i, int count)
{
    const int  si  = g.binary(KOp::Div, i, count);
    const int  lg  = g.binary(KOp::Mul, near, g.unary(KOp::Exp, g.binary(KOp::Mul, si, g.unary(KOp::Log, g.binary(KOp::Div, far, near))))); // near·(far/near)^si
    const int  un  = g.binary(KOp::Add, near, g.binary(KOp::Mul, g.binary(KOp::Sub, far, near), si)); // near + (far−near)·si
    const auto ks  = [&](double c) { return detail::kf(g, lambda, c); };
    return g.binary(KOp::Add, g.binary(KOp::Mul, lambda, lg), g.binary(KOp::Mul, g.binary(KOp::Sub, ks(1.0), lambda), un));
}
// csm_select_cascade — the cascade index (0..3) for a view-space depth, = how many split planes it lies beyond. Uses the
// GLSL `step` (step(edge,v)=v<edge?0:1) so it's branchless + filterable.
[[nodiscard]] inline int csm_select_cascade(KGraph& g, int view_depth, int s0, int s1, int s2)
{
    return g.binary(KOp::Add, g.binary(KOp::Add, g.binary(KOp::Step, s0, view_depth), g.binary(KOp::Step, s1, view_depth)), g.binary(KOp::Step, s2, view_depth));
}
// csm_texel_snap (Valient stabilization) — round the shadow-space position to the shadow-map texel grid so a rotating/moving
// camera doesn't shimmer the shadow edges. `uv` in [0,1] shadow space, `map_size` = texels per side.
[[nodiscard]] inline int csm_texel_snap(KGraph& g, int uv, int map_size)
{
    return nodes::detail::bin(g, KOp::Div, g.unary(KOp::Round, nodes::detail::bin(g, KOp::Mul, uv, g.splat(map_size, 2))), g.splat(map_size, 2));
}
// csm_blend_factor — the smooth cross-cascade blend weight near a split: clamp((d − (split − w)) / w, 0, 1). 0 = full current
// cascade, 1 = full next cascade (blended over a band of width `blend_width` before the split).
[[nodiscard]] inline int csm_blend_factor(KGraph& g, int view_depth, int split, int blend_width)
{
    return nodes::clamp01(g, g.binary(KOp::Div, g.binary(KOp::Sub, view_depth, g.binary(KOp::Sub, split, blend_width)), blend_width));
}

// ─── B8-i: SCREEN-SPACE + TRANSLUCENT SHADOW FRONTIER (the analytic cores) ───────────────────────────────────────────────
// The parts of B8-i that are pure shader math on the current infra. Contact shadows march the DEPTH BUFFER (bound at B8-l);
// the deep-shadow transmittance + VSM addressing are self-contained. RT shadows (+ SIGMA/ReBLUR denoise) ride B9 (they need
// the GPU ray-scene traversal) and MegaLights/ReSTIR many-light shadows ride B14 (the reservoir substrate) — those are
// infra-sequenced leaves, not deferrals; their analytic cores are built where their infra lands.

// contact_shadow — screen-space ray-marched contact shadows (Bend/UE style), the 4-tap windowed occlusion + fade. `ray_z`
// and `scene_z` are vec4 — the ray depth and the sampled scene depth at 4 march taps. A tap OCCLUDES when the ray is behind
// the surface by more than `bias` but less than `thickness` (an in-front occluder of bounded thickness). The occlusion is the
// hardest (max) tap, attenuated by a distance/edge `fade`. Branchless via GLSL `step` (step(edge,v)=v<edge?0:1). Returns a
// visibility in [0,1] (1 = lit). The real per-tap depth SAMPLE (`ray_z`/`scene_z` from the depth buffer) is bound at B8-l.
[[nodiscard]] inline int contact_shadow(KGraph& g, int ray_z, int scene_z, int bias, int thickness, int fade)
{
    // Each of the 4 taps is an INDEPENDENT scalar depth sample → scalar Step per tap, then a horizontal max (the taps are
    // not a vector quantity — a scalar reduction is the idiomatic form, and it stays on the wired scalar emitter path).
    const auto tap = [&](int kk) {
        const int d = g.binary(KOp::Sub, g.swizzle(ray_z, kk), g.swizzle(scene_z, kk));
        return g.binary(KOp::Mul, g.binary(KOp::Step, bias, d), g.binary(KOp::Step, d, thickness)); // 1 iff bias ≤ d ≤ thickness
    };
    const int occ = g.binary(KOp::Max, g.binary(KOp::Max, tap(0), tap(1)), g.binary(KOp::Max, tap(2), tap(3))); // hardest tap
    return nodes::clamp01(g, g.binary(KOp::Sub, detail::kf(g, occ, 1.0), g.binary(KOp::Mul, occ, fade)));
}

// fourier_opacity_transmittance — translucent / deep shadows via Fourier Opacity Maps (Jansen & Bavoil, HPG 2010). A
// participating occluder (hair/foliage/smoke) stores the Fourier series of its absorption; the transmittance at a normalized
// depth `d` ∈ [0,1] reconstructs from the coefficients (here the n=2 series: a0 + (a1,b1) + (a2,b2)). `optical_depth =
// ½a0·d + Σ_k (1/kπ)·[a_k·sin(2πk d) + b_k·(1−cos(2πk d))]`, and `T = exp(−optical_depth)`. This is the ORDER-INDEPENDENT
// fractional shadow that hard depth maps can't express. Bit-exact (Sin/Cos/Exp are ULP-faithful on the F64 oracle).
[[nodiscard]] inline int fourier_opacity_transmittance(KGraph& g, int a0, int a1, int b1, int a2, int b2, int depth)
{
    const auto kf  = [&](double v) { return detail::kf(g, depth, v); };
    const int  w1  = g.binary(KOp::Mul, kf(2.0 * kPi), depth);        // 2π·1·d
    const int  w2  = g.binary(KOp::Mul, kf(4.0 * kPi), depth);        // 2π·2·d
    const int  t1  = g.binary(KOp::Mul, kf(1.0 / kPi),
                              g.binary(KOp::Add, g.binary(KOp::Mul, a1, g.unary(KOp::Sin, w1)),
                                                 g.binary(KOp::Mul, b1, g.binary(KOp::Sub, kf(1.0), g.unary(KOp::Cos, w1)))));
    const int  t2  = g.binary(KOp::Mul, kf(1.0 / (2.0 * kPi)),
                              g.binary(KOp::Add, g.binary(KOp::Mul, a2, g.unary(KOp::Sin, w2)),
                                                 g.binary(KOp::Mul, b2, g.binary(KOp::Sub, kf(1.0), g.unary(KOp::Cos, w2)))));
    const int  tau = g.binary(KOp::Add, g.binary(KOp::Add, g.binary(KOp::Mul, g.binary(KOp::Mul, kf(0.5), a0), depth), t1), t2);
    return g.unary(KOp::Exp, g.binary(KOp::Sub, kf(0.0), tau));       // exp(−optical_depth)
}

// vsm_clipmap_level — Virtual Shadow Map clipmap level selection (UE5 VSM): the level whose texels project ≈1:1 at the
// receiver, `clamp(floor(log2(view_z / base_extent)), 0, max_level)`. Coarser levels cover the distance, finer the foreground.
[[nodiscard]] inline int vsm_clipmap_level(KGraph& g, int view_z, int base_extent, int max_level)
{
    const int lvl = g.unary(KOp::Floor, g.unary(KOp::Log2, g.binary(KOp::Div, view_z, base_extent)));
    return nodes::clamp(g, lvl, detail::kf(g, lvl, 0.0), max_level);
}
// vsm_page_coord — the virtual PAGE coordinate for a clip-space uv at a level: `floor(uv · pages_at_level)` (uv ∈ [0,1]², a
// vec2). The page-table lookup + GPU-feedback allocation + virtual→physical remap is the renderer subsystem (B8-l); this is
// the addressing math that indexes it.
[[nodiscard]] inline int vsm_page_coord(KGraph& g, int uv, int pages)
{
    return g.unary(KOp::Floor, nodes::detail::bin(g, KOp::Mul, uv, g.splat(pages, 2)));
}

// ─── B8-j: SKINNING (linear-blend + dual-quaternion) ─────────────────────────────────────────────────────────────────────
// Vertex-stage deformation math. The bone PALETTE + per-vertex indices/weights bind as a set-3 structured buffer (and an
// optional compute pre-skin pass) at B8-l; the blend MATH is complete here — both take the bone transforms as inputs and
// produce the skinned position/normal.

// lbs_skin_position — LINEAR-BLEND skinning: `p' = Σ wᵢ·(Mᵢ·[p,1])`. Blending the 4 transformed positions is affine-equivalent
// to blending the matrices (linearity) and needs only matvec + a weighted vector sum. `w` = a vec4 of the 4 bone weights.
[[nodiscard]] inline int lbs_skin_position(KGraph& g, int m0, int m1, int m2, int m3, int w, int pos)
{
    const int  p4      = g.vec4(g.swizzle(pos, 0), g.swizzle(pos, 1), g.swizzle(pos, 2), detail::kf(g, g.swizzle(pos, 0), 1.0));
    const auto contrib = [&](int m, int wk) { return nodes::detail::bin(g, KOp::Mul, g.swizzle(g.mat_mul_vec(m, p4), 0, 1, 2), wk); };
    const int  s01     = g.binary(KOp::Add, contrib(m0, g.swizzle(w, 0)), contrib(m1, g.swizzle(w, 1)));
    const int  s23     = g.binary(KOp::Add, contrib(m2, g.swizzle(w, 2)), contrib(m3, g.swizzle(w, 3)));
    return g.binary(KOp::Add, s01, s23);
}
// lbs_skin_normal — the same weighted blend on the DIRECTION `[n,0]` (drops the translation column), re-normalized.
[[nodiscard]] inline int lbs_skin_normal(KGraph& g, int m0, int m1, int m2, int m3, int w, int nrm)
{
    const int  n4      = g.vec4(g.swizzle(nrm, 0), g.swizzle(nrm, 1), g.swizzle(nrm, 2), detail::kf(g, g.swizzle(nrm, 0), 0.0));
    const auto contrib = [&](int m, int wk) { return nodes::detail::bin(g, KOp::Mul, g.swizzle(g.mat_mul_vec(m, n4), 0, 1, 2), wk); };
    const int  s01     = g.binary(KOp::Add, contrib(m0, g.swizzle(w, 0)), contrib(m1, g.swizzle(w, 1)));
    const int  s23     = g.binary(KOp::Add, contrib(m2, g.swizzle(w, 2)), contrib(m3, g.swizzle(w, 3)));
    return g.normalize(g.binary(KOp::Add, s01, s23));
}
// dquat_skin_position — DUAL-QUATERNION skinning (Kavan et al. 2007), volume-preserving (no candy-wrapper collapse on twist).
// A 2-bone antipodality-corrected weighted blend of the (real qr, dual qd) parts → normalize by |qr| → the rigid point
// transform `p + 2·(rxyz × (rxyz×p + rw·p)) + 2·(rw·dxyz − dw·rxyz + rxyz×dxyz)`. `r*`/`d*` vec4 real/dual quats, `w*` scalars.
[[nodiscard]] inline int dquat_skin_position(KGraph& g, int r0, int d0, int r1, int d1, int w0, int w1, int pos)
{
    const int dt   = g.dot(r0, r1);                                                                                  // hemisphere test
    const int sgn  = g.binary(KOp::Sub, g.binary(KOp::Mul, detail::kf(g, dt, 2.0), g.binary(KOp::Step, detail::kf(g, dt, 0.0), dt)), detail::kf(g, dt, 1.0)); // dot<0 ? −1 : 1
    const int w1s  = g.binary(KOp::Mul, w1, sgn);
    const int br   = g.binary(KOp::Add, nodes::detail::bin(g, KOp::Mul, r0, w0), nodes::detail::bin(g, KOp::Mul, r1, w1s));
    const int bd   = g.binary(KOp::Add, nodes::detail::bin(g, KOp::Mul, d0, w0), nodes::detail::bin(g, KOp::Mul, d1, w1s));
    const int len  = g.vlength(br);
    const int qr   = nodes::detail::bin(g, KOp::Div, br, len);
    const int qd   = nodes::detail::bin(g, KOp::Div, bd, len);
    const int rxyz = g.swizzle(qr, 0, 1, 2); const int rw = g.swizzle(qr, 3);
    const int dxyz = g.swizzle(qd, 0, 1, 2); const int dw = g.swizzle(qd, 3);
    const int inner = g.binary(KOp::Add, g.cross(rxyz, pos), nodes::detail::bin(g, KOp::Mul, pos, rw));              // rxyz×p + rw·p
    const int rot   = g.binary(KOp::Add, pos, nodes::detail::bin(g, KOp::Mul, g.cross(rxyz, inner), detail::kf(g, rw, 2.0)));
    const int tt    = g.binary(KOp::Add, g.binary(KOp::Sub, nodes::detail::bin(g, KOp::Mul, dxyz, rw), nodes::detail::bin(g, KOp::Mul, rxyz, dw)), g.cross(rxyz, dxyz));
    return g.binary(KOp::Add, rot, nodes::detail::bin(g, KOp::Mul, tt, detail::kf(g, rw, 2.0)));
}

} // namespace crd::kir::lighting
