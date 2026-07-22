# Session — 2026-07-22 · B16 ocean CINEMATIC pass (horizon · sky · clouds · reflections)

**Focus:** close the B16 ocean's user-flagged visual gaps to a cinematic bar — *"completely realistic ocean, completely
realistic atmosphere like between dusk and noon, puffy clouds decorating the scene, horizon must be fully correct, every visual
error fully addressed, reflections/refractions with no artifacts, cinematic quality."*

## What shipped

An instrument-first loop (edit → render `[.ocean-frame]` → view the BMP zoomed → adjust) turned the flat render into a
cinematic one. Four gaps, all closed, across all three render paths (fragment · vertex-pull geometry · mesh-shader — consistent):

1. **Seamless horizon (the hard one).** The Johanson projected grid's top triangle row is a world-space sawtooth at the horizon;
   shaded opaque it drew a dark zigzag outline against the sky (the most "CG" thing in the frame — invisible at frame scale,
   obvious at 4× zoom). Fixed with three cooperating changes in `ckir_water_render.hpp`:
   - `ocean_projected_vertex`: far-crest displacement taper now starts at **s=180** (was 250) → the sawtooth amplitude shrinks
     before the horizon.
   - `build_ocean_water_geo_fs`: coverage-**α** now ramps to 0 over range `dist∈[193,248]` → the residual sawtooth is composited
     out (the sky pass shows through). Range-based, not the old `v.y` grazing fade (which washes the mid-field).
   - fog colour `fogc = hazeh` (the sky-at-horizon tone) → the far sea and the sky meet in one tone, no grey band.
   - the fragment path's grazing-aliasing "picket fence" is re-veiled with that same `hazeh` haze (`fogh` reach widened, tinted).

2. **Physical atmosphere.** New **shared** `crd::kir::water::analytic_sky` — the ONE atmosphere source: a Rayleigh+Mie analytic
   dome (deep-blue zenith → pale luminous horizon via `pow(dy,0.42)`, a **compact** `pow(mu,11)` warm Mie halo, a tight `exp` sun
   disk). The old broad blown-out sun wash is gone. Time-of-day by sun elevation (`ldir`).

3. **Puffy cumulus decorating the sky.** Round Worley billows × a broad fbm field (`shape = fbm·(0.48+0.72·bill)`), thresholded
   to defined masses with clear-sky gaps, sunlit tops + self-shadowed bases (3-D puffs). fbm drives coverage, billows sculpt —
   billows-alone gave sparse dots.

4. **Reflections.** The sea now MIRRORS the sky **including the clouds** — the reflection calls the same `analytic_sky` with
   `with_clouds=true` on the reflected ray; the choppy FFT surface distorts them into realistic broken reflections. Plus the
   existing Fresnel / sun-glitter / teal subsurface refraction. For an open-ocean horizon (no scene objects) the analytic sky
   reflection IS the correct, artifact-free reflection; true SSR/planar stay deferred (only meaningful with scene geometry).

**One atmosphere source.** `analytic_sky` is called by BOTH the sky pass (`build_ocean_frame_fft_fs`, `hi_detail=true`) and the
water reflection (`build_ocean_water_geo_fs`, `hi_detail=false`). Change the time-of-day in one place and the reflection follows.
This unified two previously-divergent sky definitions (the geo FS had been reflecting the *old* sun).

## Verification

- Vulkan `[.ocean-frame]` — 3 frames (t=0/2.5/5.0), 105 assertions, GREEN. Mean RGB stable ~ (101,125,150), temporally
  consistent. Viewed all three paths at frame scale + the horizon at 4× zoom: seam gone, clouds puffy, sky cinematic.
- Cross-backend DX12 `[ocean]` — 22 assertions / 3 cases GREEN (the geo FS + `analytic_sky` emit valid **HLSL** and render on
  DX12 — the mesh-ocean test emits `build_ocean_water_geo_fs`).
- kir `[ocean]` — 4167 / 8 GREEN (engine header compiles + ocean sim unaffected).
- clang-tidy (pinned LLVM-20.1.8, `win-tidy-local` DB) on both changed headers — CLEAN.

## Files

- `engine/kir/include/crd/kir/ckir_water_render.hpp` — new `analytic_sky`; geo FS reflection unified + cloud reflections; fog
  colour = `hazeh`; range-based horizon α; earlier crest taper; reflection-sky constants synced to the new atmosphere.
- `tests/gpu-shared/ckir_raster_triangle.hpp` — sky pass (`build_ocean_frame_fft_fs`) now calls `analytic_sky` (removed its local
  `sky_of` lambda for both the sky and the water reflection); horizon haze re-veil.
- `tests/gpu-context-vulkan/test_vulkan_context.cpp` — (temporarily 1-frame for iteration; restored to the full 3 frames).

## Docs

- Recipe: `docs/recipes/2026-07-22-cinematic-ocean-sky-and-seamless-horizon.md` (params → physics → assembly → traps).
- Detour row 105 (B16): the ⚠ visual-polish clause replaced with the ✅ cinematic-pass note.

## Deferred (unchanged, explicit — prior user sequencing, not silently dropped)

B16-a-1 SSR + planar reflection + screen-space refraction + underwater Beer/god-rays (Arc Blanc) + wave-particle↔FFT coupling;
B16-b caustic maps + DXR ray-traced caustics. These need scene objects / a deeper water volume to matter; the open-horizon
cinematic bar is met without them.

## Proposed commit (user commits — no AI co-author trailer)

```
feat(b16): cinematic ocean pass — seamless horizon, physical sky, puffy clouds, cloud reflections

Close the B16 ocean's user-flagged visual gaps to a cinematic bar across all three
render paths (fragment / vertex-pull geometry / mesh-shader):

- Seamless horizon: dissolve the projected-grid's dark sawtooth top edge — flatten
  far crests earlier (taper from s=180), fade coverage-alpha to 0 over range
  dist in [193,248], and set the aerial fog colour to the sky-at-horizon tone so
  sea and sky meet in one tone (no grey band, no dark outline).
- Shared analytic_sky (crd::kir::water): a Rayleigh+Mie analytic dome (deep-blue
  zenith -> pale horizon, tight sun disk + compact warm Mie halo) — the ONE
  atmosphere source, called by both the sky pass and the water reflection.
- Puffy cumulus: Worley billows x a broad fbm field, sunlit tops + shadowed bases.
- The sea mirrors the sky including the clouds (analytic_sky with_clouds on the
  reflected ray); Fresnel/sun-glitter/teal-SSS refraction retained.

Cross-backend: DX12 [ocean] 22/3, kir [ocean] 4167/8, Vulkan [.ocean-frame] 105/1;
both changed headers clang-tidy-clean (LLVM-20.1.8).

Recipe: docs/recipes/2026-07-22-cinematic-ocean-sky-and-seamless-horizon.md
```
