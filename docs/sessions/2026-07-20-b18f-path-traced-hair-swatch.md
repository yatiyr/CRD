# Session — 2026-07-20 — B18-f: the path-traced hair swatch (the RT strand tier's payoff)

## Goal

Turn the B18-f RT strand tier into an actual renderer: film-scale path-traced hair at realistic fibre size,
driving the B18-a Chiang BCSDF through hardware curve traversal. The user asked, in order, for: the true
capability of the hair renderer; different hair types and colours; smaller/denser patches; a close camera on
real-size fibres; real GI; and finally "record everything + can we do real-time?".

## What we built

- **`engine/kir/include/crd/kir/ckir_hair_rt.hpp`** (new) — the path-traced hair kernel. A per-pixel sample loop
  that traces a jittered camera ray against the procedural curve BLAS, rebuilds the fibre frame from the hit, and
  runs a **3-bounce path** with next-event estimation per light and a uniform-sphere indirect bounce. Direct
  shadows are a **transmittance march** through up to 8 fibres (a fibre is a filter, not an occluder). Includes an
  analytic gray ground plane that receives the groom's real contact shadow, and a studio environment the escaped
  rays gather. The whole thing is authored in CKIR and lowered to GLSL.
- **`tests/gpu-context-vulkan/hair_swatch.hpp`** (new) — the swatch geometry generator: locks on a jittered grid,
  each a coherent ringlet (helix on the *lock* centreline, not per strand), strands hanging under gravity from an
  elevated card, smooth analytic centrelines, per-endpoint smooth tangents, melanin → σₐ.
- **`tests/gpu-context-vulkan/test_vulkan_hair_swatch_rt.cpp`** (new) — the render driver + auto-exposure + ACES
  tonemap + BMP writer + a per-sample performance sweep + a clean unblended AOV probe path.

## The bug that mattered most — a precision failure in the intersector

Rendering at realistic 68 µm fibre radius produced a **beading** artifact: every strand rendered as a chain of
light/dark beads. Four hypotheses were disproven by experiment before the real cause was found by **instrumenting
`h` directly** (an AOV accumulating |h| on hits only, plane+env suppressed so nothing blended in):

- mean |h| conditioned on a hit came out **0.92**, where a cylinder must give **0.5** → h genuinely wrong.
- |u − u_true| = 0.0004 → u fine.
- **|roff| / rad = 8.885** → the hit is ~9 radii off the surface. The intersector is committing hits that are
  not on the fibre.

Root cause: **catastrophic cancellation in the round-cone quadratic**. It recovers a term of order `m0·ra²` by
subtracting quantities of order `|ro−pa|²`. With the camera ~1 unit from a 68 µm fibre those differ by **eight
orders of magnitude**, so in f32 the radius information sits below the cancellation noise of the very terms
carrying it. **Fix:** re-origin the ray at the segment before solving (`tsh = dot(pa−ro, d)`), everything local
scale, add `tsh` back to reach the true parameter. Propagated to all four homes (GLSL, HLSL, `lss_intersect`,
oracle). After: mean |h| = **0.4996**, |roff| = 5.01e-5 vs a 5.0e-5 mean radius, and both hardware gates got two
orders of magnitude MORE accurate (Vulkan 8.3e-05 → **4.8e-07**, DX12 2.6e-05 → **7.2e-07**) — independent
confirmation this was a real correctness defect, not a look change.

⭐ **The defect arrived WITH correctness, not with the change that exposed it.** At the fat placeholder radius the
term sat right at f32's edge and mostly survived, so every earlier render "looked fine". Making the fibres
realistic made a latent precision bug the common case. "It looked fine before" was not evidence.

## The other defects fixed this session (all first looked like art direction)

1. **Random-walk centreline** → a zigzag by construction; every control point a corner, and a swept sphere renders
   its silhouette exactly. Replaced with a sum of sinusoids (C^∞); randomness lives in the parameters.
2. **Per-strand helix phase** → wide independent spirals. A ringlet is a *lock* spiralling together; phase/radius
   are clump properties now.
3. **Flat tangents** → the sharp R lobe chopped into per-segment dashes. Smooth per-endpoint tangents, lerped.
4. **Missing cos θi** in the fibre rendering equation.
5. **Plane light added OVER hair** (no occlusion test) → an achromatic term erased the BCSDF colour → white phantom.
6. **Binary shadow rays** → black interior; a fibre is a filter, march per-channel transmittance (coloured shadow).
7. **Speckle = Monte-Carlo shadow variance**, not geometry; fixed by more spp + tighter light sources.
8. **h measured from the segment's start point** → stepped at joints; measured against the axis point at the hit.

## The diagnostic discipline that finally worked

Four confident wrong guesses cost real time. What broke the deadlock: **build the instrument first, then measure,
then hypothesise.** A clean AOV that (a) suppressed the plane and environment so nothing blended into the reading
and (b) accumulated the quantity ONLY on hits with a separate hit counter, so a partially-covered pixel could not
masquerade as a high value. An earlier version of the same probe DID composite the background and produced a
confident wrong answer — the instrument itself has to be validated.

## Performance — measured

Per-sample GPU cost isolated by sweeping spp in single dispatches (slope = pure GPU, intercept = per-dispatch
overhead). On the RTX 4070 Ti SUPER, 1400×1000, 3 bounces, 3-light NEE, 8-step transmittance shadows, OPTIMISED
SPIR-V:  **time ≈ 3350 ms fixed + 194 ms × spp.** One full-frame sample = **~194 ms**. Board:
`docs/bench/2026-07-20-hair-rt-swatch-perf.md`.

Verdict: as configured this is an **offline/film** renderer (a converged ~384 spp frame ≈ 75 s of pure GPU).
Real-time is reachable but not with this path unchanged — see the bench doc for the lever analysis.

## Files touched

- `engine/kir/include/crd/kir/ckir_hair_rt.hpp` — new: the path tracer
- `engine/kir/include/crd/kir/ckir_lss.hpp`, `ckir_glsl.hpp`, `ckir_hlsl.hpp`, `ckir_kernel_eval.hpp` — the re-origin precision fix in all four homes
- `tests/gpu-context-vulkan/hair_swatch.hpp`, `test_vulkan_hair_swatch_rt.cpp` — new: geometry + driver + probe + perf sweep

## Tests / verification

- `[lss]` + `[curvert]` CPU gates: **8/8, 6740 assertions** (after the re-origin, held tight)
- Vulkan hardware curve traversal: 45 hits, ZERO disagreements, maxabs **4.8e-07**
- DX12 LSS dispatch: 18 hits, ZERO disagreements, maxabs **7.2e-07**
- h probe: mean|h| = **0.4996** (theory 0.5) over 107k well-covered pixels

## Next session starts with

- clang-tidy on the new files (`ckir_hair_rt.hpp`, `hair_swatch.hpp`, `test_vulkan_hair_swatch_rt.cpp`)
- a grazing-limit gate for the BCSDF (the white-furnace test integrates over h and misses a narrow bad band —
  `test_ckir_hair_grazing.cpp` exists as a probe but is not yet an assertion)
- if pursuing real-time: wire the B18-c deep-opacity-map shadow in place of the 8-step march, and the B14
  ReSTIR/SVGF denoiser to drop spp — see the bench doc
