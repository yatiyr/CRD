# Recipe — Physically-based hair, film / offline quality

> How to build cinematic path-traced hair from the fibre up: the scattering model, the geometry, the renderer.
> Read this once and you understand every equation in `ckir_hair.hpp` / `ckir_lss.hpp` / `ckir_hair_rt.hpp` and
> can rebuild the system. The real-time form is a separate recipe (`2026-07-21-hair-realtime.md`); this is the
> reference it is measured against.

---

## 1. Parameters first

Drive the whole system from this table. Everything below explains *why* these are the knobs.

### 1a. Fibre optics — the BCSDF (`hair_bcsdf_eval_angles`)

| Parameter | Meaning | Units | Default | Range / notes |
|---|---|---|---|---|
| `sigma_a` (σₐ) | **Absorption coefficient, PER RGB CHANNEL.** This IS the hair colour — hair colour is absorption, not a surface tint. | 1/(fibre radius) | from melanin (below) | 0 (white/translucent) … ~5 (black). Spectral: the three channels differ, which is why transmitted light is coloured. |
| `eta` (η) | Index of refraction of the cortex. | — | 1.55 | Marschner's value. 1.4–1.6. Sets Fresnel + the refraction angle. |
| `beta_m` (β_m) | **Longitudinal roughness** — how tight the R/TT/TRT highlights are in θ (along the fibre). | — | 0.25 | 0.05 (glassy, razor highlight) … 0.6 (soft). The R lobe is only a couple of degrees wide at 0.22 — this matters for tangent continuity (trap #2). |
| `beta_n` (β_n) | **Azimuthal roughness** — spread of the glints around the fibre. | — | 0.30 | 0.1 … 0.6. |
| `alpha_deg` (α) | Cuticle scale tilt. The angle of the surface scales that SEPARATES the R and TRT highlights into two distinct bands. | degrees | 2–3 | 0 (no separation) … 5. Set to 0 and the two highlights coincide — the tell of a wrong hair shader. |

Melanin → σₐ (Chiang 2016 §4), `melanin_sigma(eumelanin, pheomelanin)`:

| Pigment | Meaning | σₐ contribution (R,G,B) |
|---|---|---|
| eumelanin | brown/black pigment | `eu · (0.419, 0.697, 1.370)` |
| pheomelanin | red/yellow pigment | `ph · (0.187, 0.400, 1.050)` |

Example looks: **black** eu=3.2 ph=0.1 · **chestnut** eu=0.8 ph=0.55 · **auburn** eu=0.42 ph=1.35 ·
**platinum blonde** eu=0.045 ph=0.09. Blonde is a *near-transparent fibre*, not a beige one.

### 1b. Geometry — the swatch / groom (`hair_swatch.hpp`)

| Parameter | Meaning | Default | Notes |
|---|---|---|---|
| `root_radius` / `tip_radius` | fibre radius at root / tip | 68 µm / 32 µm (0.000068 / 0.000032 world) | **Real hair is ~1.2e-4 of strand length.** Sub-pixel at any sane framing. Fat strands are the #1 tell of CG hair. Trap #1 lives here. |
| `length` | strand length | 0.30–0.56 | Long enough that a ringlet can loop and gravity can arc it. |
| `segments` | swept-sphere segments per strand (curve tessellation) | `max(40, turns·28)` | ~28 points per helix turn. The *surface* is analytic-exact; the *centreline* is a polyline, so this controls curvature fidelity only. |
| `per_clump` × `grid_x·grid_z` | strands per lock × lock count | ~2600 × 8 | Density = strands/lock; separation = lock count. Few and fat locks read as ringlets; many thin ones average to a mass. |
| `clump_tight` | how tightly a lock converges into a rope | 0.7–0.8 | HIGH holds a ringlet together. Low separates strands = the opposite of legible curls. |
| `curl_amp` / `curl_freq` | ringlet radius / turns along the strand | per look | It is the RATIO 2·r/pitch that reads as a curl (≈0.68), not the radius. See trap #4. Applied to the LOCK centreline, not per strand. |
| `curl_ramp` | fraction of length before curl reaches full amplitude | 0.30 | Straight at the root, tightening down — a constant-radius helix from the root looks like a spring. |
| `stray` | smooth low-frequency wander amplitude | 0.010 | A SUM OF SINUSOIDS, not a random walk. Trap #3. |
| `flyaway_frac` / `flyaway_amp` | fraction of stray strands / how far they leave the mass | 0.02 / 0.04 | A perfectly bounded silhouette reads as CG instantly; one stray hair breaks it. |

### 1c. Renderer — the path tracer (`RtHairSwatchConfig`)

| Parameter | Meaning | Film default | Notes |
|---|---|---|---|
| `spp` × passes | samples per pixel (total) | ~384–512 | Sub-pixel fibres are resolved by COVERAGE STATISTICS — high spp is not optional. |
| `bounces` | path length | 3 | The interior of a groom is almost entirely bounces 2+. 1 = silhouette only. |
| `shadow_steps` | fibres a shadow ray marches THROUGH per light | 8 | A fibre is a FILTER, not an occluder (trap #6). |
| `fibre_depth` | optical path of one shadow crossing, in σₐ units | 2.0 | ≈ one diameter. |
| `light_dir / col / radius` (×3) | direction / colour / angular size of each light | key+rim+fill | Rim light behind = the TT/TRT transmission glow that makes hair read as hair. |
| `eta, beta_m, beta_n, alpha_deg, sigma_a` | forwarded to the BCSDF | see 1a | |
| `env_lo / env_hi` | studio environment escaped rays gather | dim | With none, GI collects nothing. |

---

## 2. What it is, and why the naive approach fails

Hair is not a surface. A single fibre is a translucent dielectric cylinder ~70 µm across; a groom is ~100,000 of
them, and any given camera pixel sees ~150 overlapping. Two consequences drive everything:

- **You cannot shade it like a surface.** Light does not just reflect off a fibre — it refracts INTO the cortex,
  is partly absorbed (that is the colour), and exits the far side. The lobes that carry a fibre's characteristic
  look (the coloured transmission glow, the secondary highlight) are TRANSMISSION events. A Lambert/GGX surface
  model cannot produce them.
- **You cannot rasterise its visibility.** A deferred rasteriser keeps ONE fibre per pixel out of ~150; neighbouring
  pixels keep different ones, and the groom streaks into painted ribbons. The information was thrown away at
  visibility. Film shoots hundreds of rays per pixel and lets each hit a real fibre, so the pixel holds the AVERAGE
  of the mass — which is what hair actually looks like.

So film hair = a physically-based fibre **BCSDF** + **ray-traced** visibility against **real analytic fibre
geometry** + **multi-bounce** transport for the interior.

---

## 3. The physics — the fibre BCSDF (Marschner 2003 → Chiang 2016)

A **BCSDF** (Bidirectional Curve Scattering Distribution Function) is the fibre analogue of a BSDF. It is written
in the **fibre frame**: θ measured from the plane perpendicular to the strand tangent (longitudinal, along the
fibre), φ around it (azimuthal). It **separates** into a longitudinal term M and an azimuthal term N per lobe:

> f(ωo, ωi) = Σ_p M_p(θo, θi) · N_p(φ) · A_p / |cos θi|

The sum is over scattering **lobes** p, each a different path of the light through the cylinder:

- **p=0, R** — reflection off the cuticle. The primary white highlight.
- **p=1, TT** — Transmit-Transmit: refracts in, out the far side. The soft coloured glow of backlit hair; dominant
  in pale hair.
- **p=2, TRT** — Transmit-Reflect-Transmit: in, internal reflection, out. The SECONDARY coloured highlight, offset
  from R by the cuticle tilt α.
- **p=3, residual** — the TRRT+ tail (further internal bounces), summed as a geometric series.

The pieces (all in `hair_bcsdf_eval_angles`, `ckir_hair.hpp`):

- **M_p (longitudinal), Chiang's energy-conserving form** — a normalised Gaussian-like lobe in θ with variance
  v_p derived from β_m (`hair_mp`, built on a Bessel-I₀; `v0 = (0.726 + 0.812β_m + 3.7β_m²⁰)²`, v₁=v₀/4, v₂=4v₀).
- **N_p (azimuthal)** — for R the perfect-mirror direction; for TT/TRT the refracted azimuth via γt = asin(h/η′),
  spread by a **trimmed logistic** of scale s(β_n) (`hair_np`, `trimmed_logistic`).
- **A_p (attenuation)** — Fresnel + Beer-Lambert absorption along the internal path:
  A₀=F, A₁=(1−F)²·T, A₂=(1−F)²·F·T², where T = exp(−σₐ·dist) is the **spectral** transmittance (this is where the
  colour enters), F is the dielectric Fresnel, dist = 2·cos γt / cos θt.
- **h** — the azimuthal offset where the ray enters the fibre, in radius units, h ∈ [−1, 1]. γo = asin(h) is the
  axis the whole model turns on. h=0 is centre, |h|=1 is grazing the edge. **This one parameter is the source of
  the most expensive bug in this system — trap #8.**
- **α** — the cuticle scale tilt, applied via a sin/cos double-angle recurrence to shift each lobe's longitudinal
  angle, which is what separates R and TRT into two visible bands.

Energy conservation is verified by a **white-furnace test** (uniform lighting → albedo should be ≤1): this model
measures 1.00009, energy-conserving to 0.02%. ⚠ But the furnace INTEGRATES over h, so it is blind to a narrow bad
band near |h|=1 — see trap #8.

Extensions in the codebase (optional, host-guarded so they emit zero nodes when off): **fur medulla** (Yan 2017
double-cylinder scattered lobe) and the **Huang 2022** non-separable microfacet R lobe.

---

## 4. The geometry — linear swept spheres (Quilez round cone)

The right ray-tracing primitive for a fibre segment is a **sphere swept linearly along it** — a capsule, or a
round cone when the radii differ (a strand tapers). Why analytic and not tessellated tubes: a 170k-strand groom at
30 segments is 5M segments; tessellated at 8–24 triangles each that is 40M+ triangles — a multi-gigabyte AS, and
STILL faceted at a close camera. One procedural AABB per segment is ~24 bytes and the swept surface is exact at any
zoom.

The intersector (`lss_intersect`, `ckir_lss.hpp`) solves the ray/round-cone quadratic (Quilez's `iRoundCone` form,
specialised for rb ≤ ra): the conical side gives a quadratic in the (normalised) ray parameter whose valid range
is the axial span [0, d²]; the two end caps are two ray-sphere tests. Committed via `TraceRayCurves` (a CKIR
statement) against a **procedural-AABB BLAS** — non-opaque, so traversal runs the intersection shader per candidate
box. NVIDIA's `VK_NV_ray_tracing_linear_swept_spheres` does this in hardware, but it is Blackwell-only; the analytic
CKIR path is portable and is THE strand tier on everything else (DXR has no swept-sphere primitive at any tier).

The hit record is `RtCurveHit{t, u, prim}`. `prim` (which segment) is load-bearing: shading needs the fibre
tangent, which is a property of the segment, so t and u alone cannot shade.

---

## 5. The full assembly — the path tracer (`build_rt_hair_swatch_kernel`)

Per pixel, per sample (jittered camera ray), a multi-bounce path with next-event estimation:

1. **Primary** — `TraceRayCurves` against the curve BLAS → hit segment, t, u.
2. **Rebuild the fibre frame at the hit:**
   - **tangent** = the INTERPOLATED per-endpoint tangent (smooth, trap #2), not the segment direction.
   - **h** = the radial offset of the hit from the axis, projected onto binormal(tangent, ωo), divided by the
     radius — measured **in the segment's own frame** (trap #8) after **re-origining the ray at the segment**
     (trap #8, the precision fix).
   - frame: x=tangent, y = ωo's axis-perpendicular part (so φo ≡ 0), z = x×y.
3. **Next-event estimation, per light** — sample a point on the light's disc (soft shadow), then MARCH a shadow
   ray through up to `shadow_steps` fibres, multiplying the per-channel transmittance exp(−σₐ·fibre_depth) at each
   crossing. This is the coloured self-shadow. Evaluate the BCSDF per channel; add `f · cos θi · light · trans`.
   The cos θi is part of the fibre measure, not decoration (trap #5).
4. **Indirect bounce** — uniform-sphere sample, weighted `f · cos θ · 4π`; multiply the path throughput by the
   fibre's own coloured response and continue. The throughput carries the colour, so pale hair keeps bouncing and
   dark hair dies after one — exactly the difference between them in life.
5. **Environment / ground** — escaped rays gather a studio env; an analytic ground plane receives the groom's real
   contact shadow (a shadow-march against the same fibres), composited only where the primary ray missed the hair.
6. **Accumulate** into the out-buffer (RMW). The host sums several dispatches with different seeds (a single
   dispatch large enough to converge trips the Windows GPU watchdog), auto-exposes to a log-average key, and ACES
   tonemaps (hair's dynamic range — the TRT glint vs the shadowed interior — needs it).

---

## 6. The traps — every scar, why it happened, how to recognise it

These cost real time. Each is in code as a ⛔ comment; this is the consolidated list.

1. **Fat strands = CG hair; but radius and count move together.** Real hair is sub-pixel. Thinning fibres without
   adding far more of them makes the groom a transparent PHANTOM (mass mostly air). Scalp density ~150 hairs/cm².
2. **Flat tangents chop the highlight into per-segment dashes.** Taking the tangent as `normalize(pb−pa)` steps at
   every joint; the R lobe is ~2° wide, so each step breaks the specular. Symptom: a bright/dark dash pattern one
   segment long. Fix: smooth per-endpoint tangents, central-differenced, lerped. Analytic thickness buys an exact
   silhouette, NOT a continuous tangent field.
3. **A random-walk centreline is a zigzag by construction.** Every control point a corner; a swept sphere renders
   the silhouette exactly, so every corner shows as a hard kink, and MORE segments make it worse. Fix: a sum of
   sinusoids (C^∞); randomness in the parameters, never in the points.
4. **The helix belongs to the LOCK, not the strand; and it is the RATIO that reads as a curl.** Per-strand phase
   turns a ringlet into a tangle of wide independent spirals. And 2r/pitch ≈ 0.68 reads as a ringlet; ≈0.17 reads
   as a crimped spring. The wide-helix look was the phase, not the radius.
5. **The cos θi term is not optional** — it is part of the fibre rendering equation. Omitting it over-weights the
   grazing lobe (which does not carry the colour), so the hue comes out wrong.
6. **Binary shadow rays are wrong for hair.** Inside a groom every point is occluded from every light, so binary
   occlusion blacks the whole interior. A fibre is a FILTER: march per-channel transmittance. Because σₐ is
   spectral the shadow is COLOURED — the light surviving inside blonde hair comes out gold.
7. **Black speckle at the fibres was Monte-Carlo shadow variance, not geometry.** For dark hair a single crossing
   absorbs ~93%, so a shadow sample is binary at fibre frequency; the variance prints as dots. Fix: more spp +
   tighter light sources (the source's angular size IS the shadow-ray direction variance).
8. **⛔⛔ THE BIG ONE — f32 catastrophic cancellation in the round-cone solve.** The quadratic recovers a term of
   order `m0·ra²` (the radius) by subtracting quantities of order `|ro−pa|²` (camera distance). At a REALISTIC
   68 µm fibre viewed from ~1 unit away those differ by EIGHT orders of magnitude, so in f32 the radius is below
   the cancellation noise of the terms carrying it, and the solve commits hits ~9 radii off the surface. Symptom:
   fibres render as a chain of light/dark BEADS. **Fix: re-origin the ray at the segment before solving**
   (`tsh = dot(pa−ro, d)`, add back after). ⭐⭐ THE DEFECT ARRIVED WITH CORRECTNESS: at the fat placeholder radius
   the term survived, so "it looked fine before" — making the fibres realistic made a latent precision bug the
   common case. Found by INSTRUMENT-FIRST: a clean AOV (plane+env suppressed, |h| accumulated only on hits) showed
   mean|h|=0.92 where a cylinder must give 0.5, then |roff|/rad=8.9 named the intersector. Four re-read-the-code
   hypotheses had already failed. **The lesson: build and validate the instrument, then measure, then hypothesise.**
9. **The maths lives in FOUR homes** (the IR intersector, the CPU oracle, the GLSL emitter, the HLSL emitter) and
   drifts if you fix one. Every fix above went to all four; the cross-check gate catches drift.
10. **`eval_cpu_kernel` is SCALAR** — Vec3/Swizzle/Dot have no case and evaluate to garbage silently. Compute-tier
    hair maths is written component-wise on scalars; the vec3 BCSDF wrapper is the raster tier's.

---

## 7. Measured performance

Board: `docs/bench/2026-07-20-hair-rt-swatch-perf.md`. On an RTX 4070 Ti SUPER, 1400×1000, optimised SPIR-V:
**~194 ms per full-frame sample** at 3 bounces + 3-light 8-step shadows. A converged ~384-spp film frame ≈ 75 s of
GPU. This is an OFFLINE renderer — which is correct: it is the physically-based reference. Real-time is the other
recipe.

## 8. Where the code lives

- `engine/kir/include/crd/kir/ckir_hair.hpp` — the BCSDF (Marschner/Chiang, + fur medulla, + Huang)
- `engine/kir/include/crd/kir/ckir_lss.hpp` — the swept-sphere intersector + AABB build
- `engine/kir/include/crd/kir/ckir_hair_rt.hpp` — the path tracer
- `engine/kir/include/crd/kir/ckir.hpp` — `TraceRayCurves` + the curve BLAS statement
- `engine/gpu-context-*/src/*_ray_tracing_context.cpp` — `build_scene_curves` (both backends)
- `tests/gpu-context-vulkan/hair_swatch.hpp` — the groom generator
- Gates: `tests/kir/test_ckir_lss.cpp`, `test_ckir_curve_rt.cpp`, `test_ckir_hair*.cpp`

## 9. Papers

- Marschner et al. 2003, *Light Scattering from Human Hair Fibers* (SIGGRAPH) — the R/TT/TRT model.
- Chiang et al. 2016, *A Practical and Controllable Hair and Fur Model for Production Path Tracing* (Disney, EGSR)
  — energy conservation, melanin, the production standard.
- d'Eon et al. 2011, *An Energy-Conserving Hair Reflectance Model* (EGSR).
- Yan et al. 2017, *An Efficient and Practical Near and Far Field Fur Reflectance Model* — the medulla.
- Huang et al. 2022, *A Microfacet-based Hair Scattering Model* (EGSR, CGF 41(4)).
- Quilez, *Intersectors* (iquilezles.org) — the round-cone analytic form.
