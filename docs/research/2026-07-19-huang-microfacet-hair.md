# Huang 2022 — A Microfacet-based Hair Scattering Model (B18-b research)

> Source: Weizhen Huang, Matthias B. Hullin, Johannes Hanika, *"A Microfacet-based Hair Scattering
> Model"*, Computer Graphics Forum 41(4) (EGSR 2022), pp. 79–91. DOI 10.1111/cgf.14588.
> Paper: `docs/research/papers/huang2022.pdf`. Reference code: `github.com/RiverIntheSky/roughhair`.
> This note grounds the CKIR implementation of B18-b (the Huang half). Read `ckir_hair.hpp` alongside.

## What it is (why it's frontier)

The **first microfacet-based hair BCSDF**. Instead of the Marschner/d'Eon *separable* Mp(θ)·Np(φ)
factorization, Huang models the fiber as a **rough dielectric cylinder** (radius 1) whose surface is a
**Smith microfacet surface**, and applies Cook-Torrance directly on the curved surface. The model is
**non-separable** (computes the half-vector between ωi/ωo and integrates its distribution along the
azimuth), which is what physically explains the **glint-like forward-scattering** in the R lobe that
prior separable models could only fake. Supports **elliptical** cross-sections and gives an
**analytic closed form for the R lobe** under GGX (the headline result).

## Geometry (Fig 2/3)

- Longitudinal-azimuthal param: **θ** = angle between ω and the x–z plane; **φ** = angle between z and
  the projection of ω onto x–z. A direction ω = {θ, φ}.
- Half vector (micronormal): **ωh1 = normalize(ωi + ωo)** (R lobe).
- **Mesonormal with cuticle tilt α** (Eq scale-tilt): `ωmα = {sinφm·cosα, sinα, cosφm·cosα}` (the
  macronormal is replaced by this everywhere). α = 0 ⇒ `ωm = {sinφm, 0, cosφm}`.
- Relative IOR **η** (hair vs air). Absorption **σa** per unit length (eumelanin ρe + pheomelanin ρp).

## Cook-Torrance base (Eq 1) and BCSDF (Eq 2,3)

```
f_r(ωi,ωo) = F(ωh,ωo)·G(ωo,ωi,ωh)·D(ωh) / (4|ωm·ωo||ωm·ωi|)       (1)
L_o(ωo)    = ∫ L_i(ωi)·S(ωi,ωo)·cosθi dωi                          (2)
S = S_R + S_TT + S_TRT                                             (3)
```

## R lobe (Eq 12) + analytic GGX solution (Appendix A)

```
S_R(ωi,ωo) = R(ωh1,ωo) / (8 cosθo cosθi) · ∫ D(ωh1,ωm1)·G_ωm1 dφm1          (12)
```
G ≈ 1 for low-roughness hair ⇒ the integral is analytic. GGX NDF (Eq 41):
```
D(ωh,ωmα) = β² / ( π·(1 + (β²−1)(ωh·ωmα)²)² )                                (41)
ωh·ωmα = cosθh·cosα·cos(φh−φm) + sinθh·sinα                                 (42)
```
Substitute **A = cosθh·cosα·√(1−β²)**, **B = sinθh·sinα·√(1−β²)**. The indefinite integral (Eq 43)
simplifies for **α = 0** (B = 0, A = cosθh·√(1−β²)) to (Eq 44):
```
∫ D dφm = (β²/2π)·[  (A²−2)/(1−A²)^{3/2} · atan( tan(φh−φm)/√(1−A²) )
                   + A²·sin(2(φh−φm)) / ( (1−A²)(A²·cos(2(φh−φm)) + A²−2) )  ] + C
```
Evaluate between **φm bounds** = the min/max φm with ωm·ωi > 0 AND ωm·ωo > 0 (the visible arc).
This is a **closed form** — implementable bit-exact (atan/tan/sin, no quadrature). This is the CKIR
R-lobe target.

## TT / TRT lobes (Eq 23/24) — numerical, combined MC-Simpson (Eq 31/32)

Refractive half-vectors (unnormalized): `ω̄h1 = −ωi − η·ωt`, `ω̄h2 = −ωt + ωo/η`. Refracted ray:
```
ωt = (1/η)·( (|ωi·ωh1| − √(η² + |ωi·ωh1|² − 1))·ωh1 − ωi )
```
Absorption along an internal segment (Eq 17, corrected path length 2cosγt, NOT Marschner's 2+2cos2γt):
```
A_t = exp( −σa · 2cos(φt − φm1 + π) / cosθt )
```
TT/TRT are evaluated as **1-D composite Simpson over φm1** (step ≈ 0.7β, ~28 sub-intervals at β=0.08),
sampling ONE internal refracted path per φm1 (Eq 31 for TT, Eq 32 for TRT — mesonormals
`ωm2={−α, 2φt−φm1}`, `ωm3={−α, φm1−2(φt−φtr)+π}`; T_i = 1−R_i Fresnel transmittances; G_i one-sided
Smith). Combined MC (over h in the render integral) + Simpson (over φm1) — the paper's key efficiency.

## Scale tilt (3.2), elliptical (3.3), sampling (3.4)

- Tilt: only the mesonormal changes (Eq scale-tilt); R-lobe outgoing longitudinal deflects ~2α but is
  **non-separably contracted** near grazing when |φi−φo|→π (θo→−θi). Adjust ωm only.
- Elliptical (ellipse x=sinγ, z=b·cosγ, e=√(1−b²)): da_m and a_o⊥ gain a `√(1−e²sin²γ)` factor;
  BCSDFs Eq 28–30 (two extra terms only). Circular = b=1, e=0.
- Importance sampling (3.4.1): 8 random numbers — pick h, build ωm1={α,−asin h}, ωh1, Fresnel; refract;
  ωm2, ωh2, Fresnel R2, absorption A_t; reflect ωtr, ωm3, T3, A_tr; pick lobe ∝ attenuation A_p; return
  weight A_R+A_TT+A_TRT × G(ωo). PDF (Eq 33) mirrors the eval.

## CKIR implementation plan (B18-b, both backends, bit-exact)

1. **`huang_r_lobe_eval_angles`** — the ANALYTIC R lobe (Eq 44, α=0 first; α≠0 via Eq 43). Closed form
   ⇒ straight value-graph (atan/tan/sin/sqrt), rides the scalar compute emitter. Oracle = same graph in
   f64 (`eval_cpu`). GPU==oracle to-ULP on Vulkan + DX12 (like B18-a/b). Physical gate: white-furnace
   albedo of the R lobe alone ≤ 1 and matches a numerical ∫D reference within Simpson error.
2. **`huang_tt_trt_eval_angles`** — TT/TRT via a runtime `For` composite-Simpson loop over φm1 (Eq
   31/32), one internal path per sample. The oracle uses the IDENTICAL fixed sub-interval count +
   Simpson weights (bit-exact requires matching quadrature — pin the sub-interval count, don't adapt).
3. **Energy gate** — full R+TT+TRT white-furnace ≈ 1 for a non-absorbing rough fiber (a few % Simpson
   slack), monotone in σa, and reduces toward the Chiang B18-a look as β→0.
4. **Selectable model** — expose Huang vs the B18-a Chiang model as a `HairModel` enum on the kernel
   config; both live in `ckir_hair.hpp`. Huang is the physically-based / glint-accurate path, Chiang the
   cheap separable path.

Divergences to document as needed: G≈1 in the analytic R lobe (paper's own assumption); fixed Simpson
sub-interval count for TT/TRT (bit-exact determinism over adaptive step).
