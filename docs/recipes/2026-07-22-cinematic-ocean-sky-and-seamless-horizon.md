# Recipe — Cinematic ocean sky + a seamless projected-grid horizon (CKIR)

> How to take a functional-but-flat FFT ocean render to a cinematic bar: a physical analytic sky, puffy cumulus, and — the
> hard part — a **seamless horizon** where a displaced projected-grid meets the sky with no dark sawtooth outline. All in CKIR,
> portable to GLSL + HLSL, rendering identically across the fragment / vertex-pull / mesh-shader paths.
>
> Code: `engine/kir/include/crd/kir/ckir_water_render.hpp` (`analytic_sky`, `ocean_projected_vertex`, `build_ocean_water_geo_fs`)
> and the sky pass `build_ocean_frame_fft_fs` in `tests/gpu-shared/ckir_raster_triangle.hpp`. Deliverable: `[.ocean-frame]` →
> `ocean_{frame,geo,mesh}_*.bmp`.

## Parameters (the dials that matter)

**Sky (`analytic_sky`)** — colours passed in as nodes so the caller owns the time-of-day dial:
| dial | value (bright day) | effect |
|---|---|---|
| `ldir` (sun dir) | `normalize(0.30, 0.52, 0.80)` | sun elevation ≈ 33° — lower `.y` → golden hour, higher → noon |
| `sunc` | `(2.05, 1.86, 1.55)` | warm-white sun radiance |
| `hazeh` (horizon) | `(0.56, 0.71, 0.86)` | pale luminous horizon (air-mass whitening) |
| `zen` (zenith) | `(0.07, 0.27, 0.64)` | deep saturated blue |
| gradient exponent | `pow(dy, 0.42)` | horizon→zenith blend curve |
| Mie halo | `pow(mu,11)·(1.25−tg)`, tint `(0.34,0.25,0.15)` | **compact** warm glow (exponent 11 = halo, not a wash) |
| sun disk / glow | `exp((mu−1)·1800)·18` + `exp((mu−1)·60)·0.40` | tight bright disk + a small glow |

**Cumulus** (inside `analytic_sky`, `with_clouds=true`):
| dial | value | effect |
|---|---|---|
| billow (Worley) freq | `0.95` | cell size → puff size (lower = bigger puffs) |
| broad field (fbm) freq | `1.7`, 4 octaves | the decorative scatter across the sky |
| `shape = fbm·(0.48 + 0.72·bill)` | — | fbm is the field, billows sculpt it into puffs |
| coverage threshold | `sat((shape−0.32)·3.0)` | defined masses with clear-sky gaps |
| `hmask = sat(dir.y·3.0 + 0.14)` | — | decorate from just above the horizon upward |
| lit / shad | `0.70+0.55·bill` / `1−0.38·dens` | sunlit tops + self-shadowed bases (3-D puffs) |

**Horizon (the seam fix)** — three cooperating dials:
| dial | file / value | effect |
|---|---|---|
| crest taper start | `ocean_projected_vertex`: `(s−180)·0.00625` | flatten far crests **before** the horizon (was 250) |
| coverage-α dissolve | `build_ocean_water_geo_fs`: `alpha = 1 − sat((dist−193)/55)` | far grid rows fade into the real sky |
| fog colour | `fogc = hazeh` | far sea hazes to the **same tone as the sky behind it** |

## Physics / why it works

**Sky.** A daytime clear sky is Rayleigh scattering (blue, strongest at the zenith where the air-mass is thin — hence *deep* blue up,
*pale* toward the horizon where you look through more atmosphere) plus a Mie forward-scattering lobe around the sun (the warm halo).
Modelling this as an analytic dome — `mix(hazeh, zen, pow(dy,k))` for Rayleigh + a tight `pow(mu,n)` lobe for Mie + an `exp` sun disk —
gives a physically-plausible gradient far richer than a single `mix(horizon, zenith, sqrt(dy))`. The **compact** Mie exponent (11, not 6)
is what keeps the sun a halo instead of a blown-out wash covering a third of the frame.

**Cumulus.** Real cumulus are *rounded billows* with clear gaps. Inverted Worley (`1 − sqrt(F1)`) is high near feature points → round
blobs; a broad fbm scatters them across the sky. Using **fbm as the coverage field and billows as the sculptor** (`fbm·(a+b·bill)`) gives
a decorative *field* of puffs; using billows alone gives sparse dots. A density-driven self-shadow (`1 − k·dens`) darkens the thick cores
so the puffs read three-dimensional against the blue.

**The horizon — the real lesson.** A Johanson projected grid tessellates screen space and raycasts onto the water plane, so its **top row
of triangles is a sawtooth in world space** right at the horizon. Shaded as opaque water it draws a dark zigzag outline against the sky —
the single most "CG" artifact in the frame. Three things must cooperate to dissolve it:
1. **Flatten the far crests early** (taper from s=180) so the sawtooth *amplitude* shrinks before the horizon.
2. **Fade coverage-α to 0 over the last ~55 m of range** (`dist∈[193,248]`) so the residual sawtooth is *composited out* — the real sky
   pass shows through where the geometry dissolves.
3. **Make the far-water haze colour equal the sky-at-horizon colour** (`fogc = hazeh`). Then even the partially-opaque transition pixels
   are the same tone as the sky behind them, so the fade is *invisible*, not a grey band.

Range-based, not view-angle-based: `v.y` (the grazing cosine) varies too slowly with distance to target only the horizon sliver — pushing
it hard enough to kill the seam also washes out mid-field water. `dist` (= `wz`, the plane-intersection range) is the right variable: it maps
precisely to the thin top band and leaves the near water fully opaque.

## Assembly (one atmosphere source)

`analytic_sky(g, dir, ldir, sunc, hazeh, zen, with_clouds, hi_detail)` is the **single** sky function, called by:
- the sky pass — `analytic_sky(rdir, …, with_clouds=true, hi_detail=true)` (the sky we look straight at),
- the water reflection — `analytic_sky(unit3(reflect(rdir,n)), …, with_clouds=true, hi_detail=false)` (cheap; the choppy surface hides
  the erosion detail and distorts the clouds into realistic broken reflections).

Because both callers share it, **the sea mirrors the exact sky** — change the time-of-day `ldir` in one place and the reflection follows.

## Traps (scars)

- **Dark sawtooth horizon on a projected grid** is not a shading bug — it's the mesh's top-edge silhouette. No per-pixel tweak fixes it;
  you must dissolve the geometry (taper + coverage-α over *range*) and match the haze tone to the sky. Diagnose by **zooming the horizon
  band 4× nearest-neighbour** — the sawtooth is invisible at frame scale but obvious zoomed.
- **A view-angle (`v.y`) horizon fade washes the mid-field.** Fade on `dist`, not grazing angle.
- **A blown-out sun** is usually the Mie/glow *breadth*, not the disk. Tighten the `pow`/`exp` exponents before touching intensity.
- **Two sky definitions drift apart.** The reflection sky and the primary sky *must* be one function, or the sea reflects yesterday's sun.
- **Billows-only clouds are sparse dots.** Drive coverage with a broad fbm; use billows to shape, not to gate.
- Reflecting clouds on **calm** water can look like doubled geometry; here the FFT chop breaks them up so it reads as real. On a dead-flat
  sea, damp the cloud reflection or it mirrors too crisply.
