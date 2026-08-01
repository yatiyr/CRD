# 2026-07-31 — REN-40-D closed: PCSS debugged (three stacked defects) + the moment tier (EVSM/MSM), both backends

## What shipped

**REN-40-D is closed.** `forward_csm` now carries the full softness axis as DECLARED options —
`soft_mode` = 0 PCF · 1 PCSS · 2 EVSM · 3 MSM — plus `cascade_blend_pct`, `light_angle_x100`,
`soft_max_texels` and `soft_search_taps` (the last two were literals; a penumbra cap and a search
budget are content decisions). All four modes render and are gated on **Vulkan and DX12**.

## PCSS: three stacked defects, each rendering a plausible soft shadow

The "inverted penumbra" the previous window left open was not one bug. In order of discovery
(memory: `feedback_pcss_three_defects_unbound_sampler_ring_search_receiver_plane`):

1. **The plain depth sampler (binding 6) was missing from the Vulkan descriptor set LAYOUT.** A
   blocker search needs the *stored* depth, which a comparison sampler cannot return. The search read
   an unbound descriptor, found "blockers" everywhere, and dimmed every lit surface ~50%. The
   instrument that cracked it was dumping a scanline **as numbers** — the *lit plateau* had moved
   (`114…145` → `57…72`), which no amount of looking at images showed. The sampler is NEAREST +
   CLAMP_TO_EDGE (filtered depths across an edge belong to no blocker; REPEAT wraps the search to the
   far side of the cascade slice).
   **DX12 had the same gap one register over**: no `s6` in the scene root signature ⇒ PSO rejection ⇒
   `set_shadows_enabled` false ⇒ silently unshadowed — the cook-only scar. Now a fixed sampler-heap
   slot (`2 + kSamplerCacheCap`) + root param 8, bound wherever `s5` is bound.
2. **The blocker search was a RING, not a disc** — eight taps at exactly `±search` never sample the
   middle, so the commonest blocker (directly overhead) was invisible. `avg` tracked the *search
   radius* instead of the caster: non-monotone in height (wider at h=2 than h=4). Replaced with a
   Vogel golden-angle spiral (`r_i = sqrt((i+½)/N)`) — equal-area (unbiased mean), deterministic
   (nothing for a temporal filter to chase), normalised **per tap count** (a prefix of the 16-tap
   table only reaches `sqrt(N/16)` of the radius).
3. **No receiver-plane depth bias** (Isidoro). Every tap compared against the fragment's own depth, so
   on a tilted receiver a tap `k` texels out sits `k·texel·tan(tilt)` deeper than its reference — past
   ~1 texel the receiver became **its own blocker**, and the penumbra scaled with the CAP rather than
   the caster (nearly flat over a 5× height change; grew every time the cap was raised). Per-cascade
   `dz/du, dz/dv` now ride the containment walk like the bias does, extended to every search **and**
   filter tap. ⛔ The `n·ẑ` guard is sign-preserving — a positive floor flips the plane's tilt on half
   of all cascade fits and adds the error it exists to remove.

**The proof is proportionality in BOTH terms** (penumbra = `d·tanθ`): measured 2→4→14 px across
h = 2→4→10, and 2→9→25 px across θ = 1°→4°→12°; PCF flat at 1 px in every arm. Contrast *improved*
(51→70) once the wide filter stopped shadowing itself.

## The moment tier (EVSM + MSM) — fully authored

New frame graph `crd://frame/forward_csm_moment` (builtin + shipped asset, drift-gated):
depth atlas → per-slice **moment convert** → separable Gaussian **blur X/Y** as `raster.fullscreen`
`for_each` passes — over **three single-writer RGBA16F atlases** (a ping-pong gives one image two
writers in the same frame; three keeps every dependency an ordinary RAW). The forward pass reads the
final atlas; visibility reconstructs from **one bilinear read** (`evsm_shadow` / `msm_hamburger`,
already bit-exact under B8-g).

- The convert/blur are technique-library shaders behind `crd://shadow/moment_*` names — C++ KGraph
  builders selected by the authored graph, exactly as `forward_csm` itself is. The cascade layer and
  `1/map_size` bake per instance program (the same rule the per-cascade shadow VS follows).
- `instance_program(name, index)` now dispatches **by pass name** — four `for_each` passes exist.
- `EVSM c = 5.54` and `MSM bias = 6e-5` are **format-derived** (fp16 ceilings), defined once where
  both the convert and the resolve read them.
- Atlas routing keys on **layered-ness** now (`sampled_is_array`) — depth-ness was a proxy that held
  only while the sole atlas was a depth atlas; under the proxy a colour moment atlas routed as a
  *material map* and any textured draw silently lost its shadows.
- The binding-5 sampler keys on `ITexture::is_depth()` — comparison for depth, LINEAR/CLAMP for
  moments. On DX12 the flag is **explicit at construction** (depth SRVs are `R32_FLOAT`, the format
  cannot answer).
- Public API: `SceneRenderer::SoftShadow` enum; `set_soft_shadows(Evsm|Msm)` swaps the default frame
  graph for the moment tier (never an explicitly installed one).

## Two latent engine defects the tier exposed (fixed at the root)

Memory: `feedback_transient_aliaser_must_check_slot_size_and_borrowed_bundle_format`.

1. **The transient aliasers chose freed slots by lifetime alone.** A heap cannot grow after creation;
   the first graph with unequal same-class transient sizes (three RGBA16F atlases beside the
   R32_TYPELESS depth atlas) made `CreatePlacedResource` fail and the **whole DX12 graph refuse to
   build** — `0 passes`, previous canvas shown, which made the A/B gate read "moment == hard,
   pixel-identical". The chooser now requires the slot to *fit* (DX12 images+buffers; Vulkan buffers —
   its image path already sized slots to the max occupant before allocating).
2. **Vulkan's borrowed transient bundle carried no format** — `is_depth()` answered "colour" for the
   depth atlas, the comparison sampler was replaced by the linear one, and `tex_sample_cmp` returned
   the **raw stored depth as visibility**: a half-bright frame with a shallow shadow. Same scanline
   instrument found it.

## Gates (all green)

- PCSS two-distance dichotomy — **Vulkan + DX12** (the DX12 twin exists to catch the s6/root-signature
  class; it `REQUIRE`s the program set to build).
- Moment tier — **Vulkan + DX12**, three properties each chosen against a defect class that fakes the
  others: the floor must NOT dim (unbound-read canary), the shadow must EXIST (a dropped pass leaves a
  uniform frame), the edge must be softer than 4-tap PCF. Measured: hard 1 px, EVSM 3 px, MSM 5 px —
  EVSM narrower than MSM **by physics** (the exponential warp re-sharpens; same property that kills
  its light leak).
- Cascade cross-fade seam gate + `blend = 0` bit-identical parity (unchanged, still green).
- Suites: scene-render **40 cases / 948 assertions**, kir **262 / 52,917**, technique-cook 5, lod 9,
  vertex-cook 24; both DRIFT gates; clang-tidy clean on every touched file.

## Where the band stands

- 40-A ✅ · 40-B ✅ (board) · 40-D ✅ (this session)
- 40-C: C1–C3 ✅ landed+gated; **C4 dither is declared-not-consumed; C5 impostors not started** — next.
- 40-E/F/G/H open. No 1M board since LOD started drawing — the fps figures stay WITHDRAWN until 40-H.
