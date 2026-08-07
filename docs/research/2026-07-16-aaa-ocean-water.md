# Research — 2026-07-16 — AAA ocean water (2026 state of the art)

> **Outcome:** **adopted** — the B16 4-cascade FFT ocean shipped in CKIR (displaced geometry + meshlets + water shading). *(stamped 2026-08-07, doc-hygiene pass)*

> D-007 / B16 water-ocean. Triggered by: the hand-tuned fragment render kept
> looking wrong (too choppy, foam everywhere, flat far field). User direction:
> "study the papers, learn AAA water properly, 2026 style." This doc is the
> science we build the *real* B16 ocean on — not vibes.

## Question

What actually constitutes a AAA real-time ocean in 2026 — the wave model, the
spectrum, multi-band composition (swell/wind/capillary), foam, and shading —
and how does it map onto Cerid's existing CKIR FFT-ocean (`ckir_ocean.hpp`,
`ckir_fft.hpp`, `ckir_water.hpp`)?

## TL;DR

- **The ocean is a *sum of spectra*, evaluated by FFT.** The gold pipeline is
  Tessendorf FFT synthesis driven by an **empirical directional spectrum** —
  the 2026 production standard is **Horvath 2015 (TMA non-directional × a
  directional spreading function × a `swell` elongation term)**, not raw
  Phillips. Wave *height, choppiness, and "different wave types" all fall out
  of the spectrum + wind/fetch/depth* — you do NOT hand-pick amplitude.
- **Detail across scales = wave CASCADES.** 3–4 independent FFT tiles at
  lengthscales chosen with **no common factors (golden-ratio-related)**,
  combined by world-space UV, with spectral band-splitting so cascades don't
  double-count wavenumbers. This is what kills tiling AND gives swell→capillary
  range in one surface.
- **Foam is physical + temporal, not painted.** Foam is injected where the
  **displacement Jacobian `J < threshold` (the surface folds = a breaking
  crest)** — so it appears *at crest tips, intermittently* — then **accumulates
  and decays exponentially** on a persistent map. Shading is Bruneton-style
  (geometry→normal→BRDF transition with distance) + subsurface. **Cerid already
  implements the Jacobian foam and the temporal accumulate** — the render just
  wasn't using them.

## Recommendation for Cerid

We are ~80% there already. `ckir_ocean.hpp` has JONSWAP `build_ocean_spectrum`,
`build_ocean_evolve` (exact-Hermitian time evolution), the Tessendorf 8-field
pack → one batched IFFT, `build_ocean_assemble` (displacement + slope-corrected
normal + Jacobian foam), `build_ocean_foam_accumulate` (max(prev·decay, inject)
temporal foam), and `OceanCascadeConfig`. The gaps, in priority order:

1. **Spectrum → TMA + directional spreading + swell (Horvath 2015).** Upgrade
   `build_ocean_spectrum`: multiply JONSWAP by the Kitaigorodskii depth
   attenuation `Φ(ω,h)` (→ TMA), add a directional spreading `D(ω,θ)` (Hasselmann
   or Donelan-Banner) blended flat↔peaked by a `spread` param, and Horvath's
   `swell ξ` term that elongates low-frequency waves into parallel swell trains.
   This is what gives "tidal-looking" long swells + wind chop from ONE model,
   with realistic amplitude keyed to wind speed U + fetch F + depth h.
2. **Real multi-cascade in the render.** We built `OceanCascadeConfig` and the
   batched FFT already does `batch = 4·C`; wire the render to sample **C real
   cascade tiles at golden-ratio lengthscales** (e.g. L = {250, 103, 42} m) by
   world UV, NOT one tile sampled at 3 fake scales (the hack that caused the
   lattice + the tuning pain). Band-split the spectrum per cascade so a
   wavenumber lives in exactly one cascade.
3. **Use the real temporal foam.** Feed `build_ocean_foam_accumulate` across
   frames (ping-pong) and sample its map for foam — crest-tip, intermittent,
   lingering-then-fading — instead of the ad-hoc `saturate(fa)` streak hack.
4. **Amplitude from the spectrum.** Drop the artistic `waveH`. Significant wave
   height `Hs ≈ 4√(∫S dk)` comes from U/F/h. Calm tropical = low U (~4–6 m/s),
   large fetch; storm = high U. This is the honest "adjust the sea state" knob.
5. **Shading → Bruneton transition + SSS.** Adopt Bruneton's geometry→BRDF
   hand-off (near = real normals; far = a **statistical BRDF whose roughness IS
   the sub-pixel slope variance** — the LEADR idea we already stumbled into),
   plus his sky-light sea-colour + a subsurface term. This is the principled
   version of the "keep the glitter, roughen it far" fix.

Everything above is expressible in CKIR value-graphs (we already emit the hard
parts: `atan2/tanh/log/asin`, exact-Hermitian evolution, the Jacobian) and
stays bit-exact across the 5 backends — no new IR needed, just new builders.

## What we read

- [Robert Ryan — Ocean Rendering, Part 1: Simulation (2025)](https://rtryan98.github.io/2025/10/04/ocean-rendering-part-1.html) — the
  most complete modern deep-dive. Gives JONSWAP/TMA/PM formulas, Donelan-Banner
  & Hasselmann spreading, finite-depth + capillary dispersion with derivatives,
  the change-of-variables `S(k) = S(ω,θ)·(dω/dk)/k`, the full spectral
  displacement/slope/cross-derivative set, Hermitian two-for-one FFT packing,
  the Jacobian `J = (1+∂ηx/∂x)(1+∂ηy/∂y) − (∂ηy/∂x)(∂ηx/∂y)` foam criterion,
  slope-corrected normals, and the Nyquist bounds `π/L ≤ k ≤ πN/L`.
- [GodotOceanWaves (2Retr0)](https://github.com/2Retr0/GodotOceanWaves) — a
  reference modern impl. TMA spectrum, mixed flat↔Hasselmann directional with a
  swell `ξ` and small-wave suppression `exp(−k²(1−δ)²)`; **foam grows linearly,
  decays exponentially** on a texture; multiple cascades with per-cascade tile
  size; Stockham GPU FFT row→transpose→column (same shape as our batched IFFT).
- [Horvath 2015 — Empirical Directional Wave Spectra for Computer Graphics](https://dl.acm.org/doi/10.1145/2791261.2791267) — the
  standard. TMA non-directional + comparison of directional spreading functions
  + the novel normalized **`swell`** parameter (wavelength-dependent elongation
  into parallel wave trains). Plausible results across wind/depth WITHOUT
  per-shot artist gain tuning. Reference impls: EncinoWaves, GX-EncinoWaves.
- [Real-Time Interactive Hybrid Ocean (arXiv 2511.02852, Nov 2025)](https://arxiv.org/html/2511.02852v1) — the
  2026 frontier: a **global FFT far-field + local wave-particle patches** around
  interactive objects (wakes/buoyancy), both driven by the *same* JONSWAP
  spectrum so injected particles match the far-field frequency-direction
  distribution ("spectrum consistency"). Solves the FFT "can't do local
  interaction" limit. 86+ FPS at 512². This is the direction once we have boats.
- [Bruneton — Real-time Realistic Ocean Lighting, seamless geometry→BRDF](https://www.researchgate.net/publication/40837615) — hierarchical
  mix of geometry / normals / BRDF with distance; sun BRDF + sky-light sea
  colour + faked SSS. The principled basis for our far-field roughness fix.

## Alternatives considered

- **Raw Phillips spectrum (original Tessendorf).** Rejected: needs per-shot
  artist gain/cutoff tuning, no depth/fetch realism, no swell. Horvath exists
  precisely to fix this. (We started here; it's why tuning was painful.)
- **Gerstner sum-of-sines (no FFT).** Rejected: a handful of directional sines
  interfere into the parallelogram lattice we already saw and hated. FFT with a
  real spectrum is strictly better and we already have it.
- **One FFT tile sampled at N fake scales (what the render did).** Rejected:
  correlated repeats, no real band separation, and it forced endless hand
  tuning. Real independent cascades are the fix.
- **Wave particles / FLIP for the whole ocean.** Rejected for the base surface:
  don't scale to horizon. Correct only as the *local* layer (the hybrid paper) —
  a post-B16 item once boats exist.
- **Full raymarch for the calm surface.** Rejected for calm seas: overkill,
  triggered the grazing-angle horizon stripes + an MSVC C1001. Single-step
  parallax off the mip-filtered height is stable and enough for low amplitude;
  keep raymarch only for genuinely stormy/tall seas.

## Pitfalls / gotchas

- **Grazing-angle horizon aliasing is anisotropic** — isotropic mip filtering
  can't resolve it. Fixes: anisotropic sampler, a distance/angle **haze band**
  (what we did), or fade to the statistical BRDF. Do NOT drive fog/LOD off an
  unstable raymarch `thit` — its per-column jitter poisons the implicit-LOD
  screen derivative → picket-fence stripes. Key fades off the *smooth analytic
  plane distance*.
- **Cascade lengthscales must share no common factors** or the tiling beats
  return. Golden-ratio-related L, and band-split the spectrum (a wavenumber in
  exactly one cascade) or crossovers double-count energy.
- **Foam colour must be HDR/applied post-tonemap** or exposure+ACES darkens it
  to a washed teal-grey (we hit this exactly). Foam is ~white; treat as coverage
  over the tonemapped water, or push it bright enough to clip.
- **`dω/dk` and the `sech²(kh)` in finite-depth dispersion overflow** — clamp
  the `kh` argument (Ryan uses ±9). We already clamp in `detail::dispersion`.
- **MSVC C1001** on very large fragment builders — keep the shader graph modest;
  bake heavy things (the FFT field, and later the Nubis cloud volume) to
  textures rather than evaluating per-fragment.
- **`swell` elongation is wavelength-dependent** — it must attenuate the
  directional spread of *low* frequencies only; applying it flat looks wrong.

## Open questions

- Best directional spreading for our look: Hasselmann (cheap) vs Donelan-Banner
  (accurate, frequency-dependent `β_s`)? Start Hasselmann, A/B later.
- Cascade count for the hero render: 3 (swell/wind/ripple) is the sweet spot;
  4 if GPU budget allows. Confirm against tiling at the horizon.
- Whether to add the Horvath ±k separate-storage negative-spectrum fix now or
  after — it removes a Tessendorf directional-bias artifact.
- Wave-particle near-field (hybrid paper) — deferred until boats/interaction
  (post-B16, ties into eylem buoyancy).

## Used by

- 2026-07-16 B16 water-ocean re-architecture (this session): re-plan the ocean
  onto the spectrum+cascade+temporal-foam pipeline instead of hand tuning.
