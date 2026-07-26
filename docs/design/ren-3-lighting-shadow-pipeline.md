# REN-3 design — the gold LIGHTING · SHADOW · SKY · ANTI-ALIASING device pipeline (D-007 row 100)

> ## ⛔⛔⛔ TOP RULE — WE WILL ONLY USE OUR AUTHORED FRAME GRAPHS
>
> **(user, restated IN ANGER 2026-07-25)** Every rendering technique ships as an **authored `.frame.toml` ASSET**
> (cooked → loaded → run by `execute_frame_graph`). **NOT** as C++ that builds passes. `FrameGraphBuilder` is for
> **tests, node editors and runtime-generated graphs ONLY** — never for shipping an engine technique.
>
> **The test:** *can a user change pass order / resource formats / cascade count / insert a pass by editing an
> ASSET, without recompiling the engine?* If no, the work is **not done**. A step that cannot be expressed is a
> missing `FramePassKind` — add the kind with its gate, then author the technique on top of it.
>
> ⛔ **Violation on record:** REN-3.2-b's cascaded shadow maps were built as hardcoded C++ in
> `scene_renderer.cpp` (atlas creation, four cascade passes, program selection) while this entire authoring stack
> sat unused beside it. Moving CSM to an authored graph asset is the top REN debt.



**Status**: spec v2, rewritten 2026-07-25 after a user-directed scope decision. v1 (same day) scoped REN-3 as
"depth RTT + CSM + clustered culling + set-frequency model", gated on readback tests.

**⭐ USER DIRECTION (2026-07-25), which defines this slice:**
> *"REN-3 needs to be visible in the sandbox to count as done, we need a fully correct and frontier looking
> scene that aligns with our direction to be safe."*
> *"I want full frontier TAA, full frontier, full smooth and awesome looking graphics, we need full frontier
> anti aliasing system in REN-3. I want atmosphere model to generate the environment procedurally."*

That is the contract. Three consequences, each locked:

1. **The close gate is the SANDBOX**, not a readback test. Readback gates still gate every increment (they are
   how we prove correctness), but REN-3 is not done until the sandbox scene *looks* frontier.
2. **Anti-aliasing is IN**, as a full system — jitter, motion vectors, a persistent history, temporal resolve,
   rectification, and post-resolve sharpening. Not a checkbox.
3. **The environment is PROCEDURAL** — the Hillaire sky-atmosphere generates the IBL, so sky and image-based
   lighting are ONE physically-consistent system and no third-party HDR capture enters the engine (consistent
   with the own-codec / own-content doctrine).

---

## The honest starting point

The sandbox renders through a **12-line toy shader**: `0.25 ambient + 0.75·N·L`, times an instance tint, times
a sampled base-color map (`scene_renderer.cpp:228`). None of the B8 lighting library is reachable from it. The
gap between that and "frontier" is six things, and it is important to be precise about which are *wiring* and
which are *new work* — because the shader library is far more complete than the renderer that consumes it.

### Already exists, bit-exact + pixel-identical both backends — DO NOT REBUILD

| area | entry points |
|---|---|
| BRDF | `d_ggx` · `v_smith_ggx_correlated` · `f_schlick` · `fd_lambert`/`fd_burley` · `env_brdf_approx` · `energy_compensation` (Kulla-Conty) · `brdf_direct` |
| OpenPBR lobes | aniso specular · sheen (Charlie/Neubelt) · clearcoat · thin-film iridescence · transmission |
| Area lights | `ltc_evaluate_rect` · `ltc_evaluate_line` (tube) · `ltc_evaluate_disk` · `ltc_lut_uv` |
| IBL evaluation | `sh_irradiance` · `ibl_diffuse` · `ibl_specular` (Karis split-sum) |
| Shadows | `shadow_project` · `normal_offset_bias` · `shadow_factor` · `pcf_shadow` · `evsm_warp`/`evsm_shadow` · `msm_hamburger` |
| CSM | `csm_split_practical` · `csm_select_cascade` · `csm_texel_snap` · `csm_blend_factor` |
| Sky | `ckir_atmosphere.hpp` — Hillaire 2020, 4 LUT kernels: **transmittance · multiple-scattering · sky-view · aerial-perspective**, all statement-tier CKIR compute, bit-exact vs the CPU oracle |
| HDR / tonemap | `ev100_from_luminance` · `exposure_from_ev100` · `agx` (Troy Sobotka AgX) · Khronos PBR-Neutral · `srgb_encode` |
| Temporal AA | `rgb_to_ycocg`/`ycocg_to_rgb` · `clip_aabb` · `variance_clip` · `catmull_rom_weights` · `luma_feedback` · `taa_resolve` · `disocclusion` · `ign_temporal` · `dither_apply` · `smaa_luma_edge` |
| Device | frame graph (REN-1) · RTT **color** transients + `draw_storage_textured_depth` (REN-2) · `create_depth_texture` + comparison sampler + `draw_shadow` sample path · `create_texture_dim` incl. **Cube**/2DArray · GEO-2 **MikkTSpace tangents already in the cooked 48-byte vertex** |

### The gaps, classified

| | gap | class |
|---|---|---|
| **A** | SceneRenderer shades with a toy `N·L` instead of `brdf_direct` | wiring |
| **B** | Normal + metallic-roughness maps cooked in PBRM but never sampled (tangents already present) | wiring |
| **C** | **The device cannot RENDER a shadow map.** `ckir_lighting.hpp:992`: *"the device has no float/array-depth upload yet"* — every shadow test to date binds a CPU-**uploaded** depth texture | device — the load-bearing gap |
| **D** | No HDR render target, no exposure, no tonemap, no encode pass | wiring + one new target format |
| **E** | No sky **rendered**, no environment, no prefilter/irradiance bake, no cube RTT — **but all four Hillaire LUTs already bake AND dispatch oracle-green on Vulkan** (see the 3.5 reuse audit) | mostly **reuse**; genuinely new = IBL generation (SH9 + cube prefilter) + DX12 parity + the graph/render wiring |
| **F** | No jitter, no motion vectors, **no persistent cross-frame history resource in the frame graph**, no temporal resolve pass | **new work** — incl. a genuine frame-graph capability |

**Why D cannot be deferred:** a real BRDF *without* tonemapping renders **worse** than the current toy shader —
radiance above 1.0 clips to white and the image goes chalky. The filmic curve is what makes physically-based
shading read correctly. A+B without D is a visual regression.

**Why E is the difference between a tech demo and frontier:** everything not directly hit by the sun is
currently a flat `0.25` constant. That one number is the single biggest visual lie in the renderer.

---

## Acceptance criteria (REN-3 is done when ALL hold)

1. The **sandbox** renders a lit, shadowed, sky-lit, temporally-anti-aliased scene at interactive rate on
   **both backends**, and it *looks* frontier — smooth edges under camera motion, correct exposure, no
   clipping, no crawling, no shadow acne, no ghosting.
2. Every increment below is **oracle-gated by readback** (bit-exact vs the CKIR CPU oracle where sampled,
   ULP-tolerant where a backend builtin is involved) — the look never substitutes for correctness.
3. **Validation-silent** on Vulkan (`ValidationCapture` 0/0) and clean on the DX12 debug layer.
4. Deterministic: `gpu_determinism_check` 3 rounds on the compute bakes; TAA converges to a stable image on a
   static camera (a still frame must not shimmer).
5. Benchmarks written to `docs/bench/` **at measurement time** — shadow-pass cost, sky-bake cost, TAA resolve
   cost, and the end-to-end frame budget, per backend.

---

## Sequenced increments

Each is self-contained, gated on both backends, and lands before the next. Ordering is by dependency and by
"smallest thing that de-risks the most".

### REN-3.1 — RTT **DEPTH** transients (the shadow-map substrate) — FIRST, unblocks C
- A `D32Float` transient with `sampled=true` becomes a **borrowed depth render target** (depth-only pass renders
  into it — VK `DEPTH_STENCIL_ATTACHMENT|SAMPLED`, DX12 `ALLOW_DEPTH_STENCIL`) **and** a **borrowed depth
  texture** (sampled through the comparison sampler in a later pass). Mirrors REN-2's borrowed-color path
  exactly, including the `m_borrowed` no-double-free discipline.
- New device draw: **`draw_storage_depth_only`** — no color attachment, depth write only (VK
  `vkCmdBeginRendering` with only `pDepthAttachment`; DX12 `OMSetRenderTargets(0, nullptr, &dsv)`).
  **⛔ This IS an interface change** (v1 of this spec hedged "likely no interface change" — wrong). It is a new
  pure-virtual on `IRasterContext` and MUST be **appended at the END** of the interface — inserting mid-vtable
  silently dispatches to the wrong method under win-release LTCG (the D135 scar).
- RTT barrier: `DEPTH_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY` (VK) / `DEPTH_WRITE → PIXEL_SHADER_RESOURCE` (DX12).
- **Gate**: a 2-pass graph — pass 1 renders an occluder's depth into a depth transient; pass 2 samples it with
  the comparison sampler (`shadow_factor`) → readback shows lit vs shadowed pixels differing where the occluder
  blocks the light. ONE submission, validation-silent, both backends.
- **Bench**: depth-pre-pass cost vs the equivalent color pass — the per-frame baseline 3.2 multiplies.

### REN-3.2 — CSM: the per-cascade depth-ARRAY atlas

> ✅ **3.2-a LANDED 2026-07-25 — the DEVICE atlas, both backends.** `FgImageDesc.layers` + `IFrameContext::image_layer`
> (appended at the vtable END), per-slice attachment views + a whole-array sampling view on VK and DX12, and the
> `KOp::SampleCmp` arrayed-shadow emitter fix (`sampler2DArrayShadow` needs `vec4(uv, layer, ref)`, not `vec3`).
> Gated by the 4-cascade **4-bit code** probe on both backends. Full detail + the three scars: D-007 row 100.
> **Still open in 3.2:** the SceneRenderer/sandbox integration below (the `csm_*` helpers already exist from B8)
> and the panning-camera no-swim gate. Authoring a cascade graph as an *asset* needs REN-36.3 `for_each` — now
> unblocked.

- A `D32Float` **2D-array** transient (N cascades). The shadow pre-pass renders each cascade slice with its own
  `light_vp` from `csm_split_practical` + Valient texel-snap (`csm_texel_snap` — without it cascades swim under
  camera motion, which TAA will *amplify*, not hide). The lighting pass selects via `csm_select_cascade` and
  blends the boundary with `csm_blend_factor`.
- **Gate**: a directional-lit scene with ≥2 visible cascades; the split is observable (depth-range banding
  test); shadowed pixels match the `pcf_shadow` oracle where sampled; a panning camera shows **no cascade swim**.

### REN-3.3 — MATERIAL COMPLETENESS + the real BRDF (gap A + B)
- SceneRenderer's FS replaces `0.25 + 0.75·N·L` with `brdf_direct` (Cook-Torrance + Kulla-Conty multiscatter).
- Sample **normal** and **metallic-roughness** maps from PBRM (`PbrmTextures.normal`, `.metallic_roughness`) —
  the tangent frame is already in the cooked 48-byte vertex from GEO-2, so this is TBN assembly, not new data.
- **Named dependency (v1 spec omitted this):** REN-2 resolves base-color **per group** and explicitly deferred
  per-instance material textures to "a bindless follow-up". A lit scene with N materials walks into that.
  **Decision for REN-3: use the bindless texture-array path (B2-d, already device-proven)** so a group can carry
  distinct materials per instance. If that proves costly, the fallback is per-group draws — but bindless is the
  frontier answer and the capability exists.
- **Gate**: a normal-mapped, metallic-rough sphere/mesh matches the CKIR oracle per-pixel where sampled;
  a dielectric vs metal pair reads correctly; no regression on the GEO-7 10k flat path.

### REN-3.4 — HDR target + exposure + AgX tonemap + encode (gap D)
- The scene renders into an **HDR transient** (RGBA16F), not straight to an 8-bit target. A full-screen post
  pass applies `exposure_from_ev100` → `agx` → `srgb_encode` → the swapchain target.
- Auto-exposure: `ev100_from_luminance` driven by a luminance reduction. **Named scope call:** the histogram
  *reduction* is a compute pass; REN-3.4 ships **manual/keyed exposure first** (a correct, deterministic,
  gated value), and auto-exposure lands with the compute plumbing in 3.5 which needs it anyway.
- **Gate**: a >1.0 radiance input tonemaps to the AgX oracle bit-exact; the encode round-trips; a bright
  emissive does not clip to flat white.

### REN-3.5 — the PROCEDURAL SKY + the ENVIRONMENT it generates (gap E) ⭐ user-directed

**⚠ REUSE AUDIT (user-prompted 2026-07-25 — "we had implemented a procedural sky before in D-007, are we going
to reuse it?"). Yes, and far more of it than this spec's first draft assumed.** The audit changed the sizing:

**ALREADY BUILT AND DEVICE-PROVEN (B15-a) — reuse verbatim, do not re-derive:**
- All four Hillaire LUT kernels in `ckir_atmosphere.hpp`, bit-exact vs the CPU oracle.
- **All four already DISPATCH ON VULKAN and match the oracle** (`test_vulkan_context.cpp:5755/5809/5872/5952`):
  transmittance (maxabs **6.25e-5**), multiple-scattering (**1.46e-5**), sky-view (both LUTs sampled), and
  aerial-perspective **3-D froxels**. There is even a GPU perf bench (`[.gi-bench]`).
- The physics is validated, not just the arithmetic: T∈(0,1], Rayleigh blue>green>red, horizon darker,
  T→1 with altitude, Ψ grows toward the ground.

**So the LUT BAKES ARE NOT NEW WORK.** What is actually missing is narrower and much better understood:

| | missing piece | note |
|---|---|---|
| a | **DX12 parity** — there is NO atmosphere test in `test_dx12_compute.cpp`; the sky is Vulkan-proven only | the kernels are backend-neutral CKIR, so this is *running* them, not porting them |
| b | **Frame-graph integration** — they dispatch today through the standalone compute path, not as graph passes | ⭐ still the **first real COMPUTE pass in the frame graph**, exercising REN-1's never-run async seam |
| c | **Nothing ever RENDERS a sky** — the LUTs are computed and read back for oracle comparison; no pass samples sky-view to produce an image | genuinely new, but small: a full-screen pass |
| d | **Aerial-perspective compositing** over distant geometry | the froxel volume exists; the composite does not |
| e | **IBL generation from the sky** — SH9 projection + cube RTT + GGX roughness-mip prefilter | the only substantial new work in 3.5 |
| f | **Zero references from `scene_renderer.cpp` or the sandbox** | the wiring |

- **Render the sky** into the HDR target (a full-screen pass sampling the *existing, oracle-gated* sky-view
  LUT), with aerial perspective composited over distant geometry.
- **Generate the environment FROM the same atmosphere** — the user-directed unification:
  - **diffuse irradiance** → project the sky into SH9 (a small compute reduction) → feed `sh_irradiance`;
  - **specular** → render the sky into a **cube RTT** and prefilter the roughness mip chain (GGX importance
    sampling) → feed `ibl_specular`'s `prefiltered` input.
  Sky and IBL are then one physically-consistent system by construction: change the sun angle and *everything*
  — sky, ambient, reflections, aerial haze — moves together. No captured HDR asset ever enters the engine.
- Replaces the `0.25` ambient constant with real image-based lighting.
- **Gate**: the four LUTs stay oracle-green **on DX12 too** (the existing Vulkan gates are the template —
  same tolerances: 6.25e-5 / 1.46e-5); `gpu_determinism_check` ×3 through the graph; the SH projection matches a
  CPU reference; a mirror-roughness sweep across the prefiltered chain matches the oracle; a sun-angle sweep
  shows sky + ambient + reflections + haze all responding coherently.
- **Bench**: LUT bake cost (cold + per-frame) — **the existing `[.gi-bench]` board is the baseline to compare
  against, not a number to re-derive** — plus prefilter cost and the sky path's share of the frame.

### REN-3.6 — the FULL FRONTIER ANTI-ALIASING SYSTEM (gap F) ⭐ user-directed
Not a resolve shader — the whole chain. Four parts, three of which are new plumbing:
1. **Sub-pixel camera jitter** — a Halton(2,3) sequence offsetting the projection matrix per frame, phase-length
   tied to the history blend. CPU-side, in the renderer. *(No Halton generator exists in the tree — small,
   new, and it belongs in the renderer, not CKIR.)*
2. **Motion vectors** — a per-pixel velocity target (RG16F) written by the scene pass from
   `current_clip − previous_clip`, which requires the SceneRenderer to keep **previous-frame transforms per
   instance** (and previous-frame skinning for the animated ring). This is the least glamorous and most
   error-prone part; it gets its own gate.
3. **⛔ A PERSISTENT, PING-PONG HISTORY RESOURCE IN THE FRAME GRAPH — a genuinely new frame-graph capability.**
   Everything the graph owns today is a *transient* whose whole point is that its memory gets **aliased away**
   after its last reader. TAA history must survive **across frames** and must be double-buffered
   (read frame N−1, write frame N, swap). The graph needs a `create_persistent_image` / imported-history concept
   that the aliasing pass is forbidden to touch. **This is the one architectural change in REN-3** — it will
   also serve every future temporal technique (SSR accumulation, DDGI, ReSTIR temporal reuse, auto-exposure
   adaptation), so it is worth doing properly rather than special-casing TAA.
4. **The resolve pass**, using the existing bit-exact helpers: reproject history by motion vector,
   **Catmull-Rom** history sample (`catmull_rom_weights` — bilinear history is the #1 source of TAA blur),
   rectify in **YCoCg** with `variance_clip`, reject on `disocclusion`, blend with `luma_feedback`.
   Then **post-resolve sharpening** (RCAS-class) to recover the softness temporal accumulation costs —
   *the one piece with no existing helper; it is small and lands here.*
- **Gate**: (a) a **static** camera converges to a stable image — a still frame must not shimmer, measured as
  frame-to-frame delta → 0; (b) a **moving** camera on a high-frequency checkerboard shows the jitter phase
  resolving sub-pixel detail, with no ghosting behind a moving occluder (the disocclusion path proves itself);
  (c) motion vectors match a CPU reference for a known rigid transform; (d) history survives a graph rebuild
  and is NOT aliased (assert its allocation is disjoint from every transient).
- **Named non-goal, stated so it is not a silent exclusion:** *temporal UPSCALING* (DLSS/FSR-class render-at-
  lower-resolution). Every prerequisite — jitter, motion vectors, persistent history, variance clipping — is
  built here, so the upscaler is a later slice with its own row and its own bench. REN-3 delivers AA quality
  at native resolution; it does not claim upscaling.

### REN-3.7 — the descriptor-set FREQUENCY model + the SANDBOX SCENE (the close gate)
- Realize ADR-0102's set model: **set 0 = frame** (view/proj/jitter/time/exposure) · **set 1 = pass**
  (shadow atlas · sky LUTs · env cube · SH) · **set 2 = material** (the bindless map arrays) ·
  **set 3 = object/skin** (the storage pull). `PassType::Shadow` realized.
- **The sandbox scene** — the actual deliverable: the cooked GEO pack scene, lit by a directional sun with CSM,
  sky-lit by the procedural atmosphere, materials with albedo/normal/metallic-roughness, HDR + AgX, TAA on,
  camera on the existing GEO-9 timeline crane move.
- **Gate**: it looks right, on both backends, at interactive rate, validation-silent — and every underlying
  increment is still oracle-green.

---

## Explicitly NOT in REN-3 (moved, with reasons)

- **Clustered / froxel light culling (Olsson 2012)** — v1 had this as REN-3.3. It is a **scale** feature
  (thousands of *dynamic* lights), not a *looks* feature: a frontier image needs one good sun + IBL + shadows.
  Its original secondary justification — "the first compute pass in the frame graph" — is now covered by the
  sky bake in 3.5. **Moves to its own D-007 row after REN-3**, where it earns its own scaling bench.
- **Temporal upscaling** — see 3.6's named non-goal.
- **Volumetric fog / clouds / SSR / SSGI** — the CKIR libraries exist (B12/B15); they are additive passes on the
  finished pipeline, and each deserves its own gate. Naming them here so their absence is a decision, not drift.

## Risks, called in advance

1. **Motion vectors for skinned meshes** (3.6.2) are the most defect-prone item in this slice — previous-frame
   skinning means keeping the previous pose. If it slips, static-geometry motion vectors still gate 3.6 and the
   animated ring falls back to a conservative disocclusion reject (visible as slightly softer AA on the ring
   only). That fallback must be *stated in the session log*, never silent.
2. **Cube RTT + mip prefilter** (3.5) touches render-target creation in a way REN-2 did not (array-of-6 faces,
   per-mip targets). Budget for it being its own sub-increment on both backends.
3. **The persistent-history frame-graph change** (3.6.3) touches the aliasing pass, which REN-1's whole
   correctness argument rests on. The existing aliasing gates (`physical_bytes < logical_bytes`, orphan-fails-
   `build()`) must stay green, plus a new "persistent is never aliased" assertion.
4. **Bindless per-instance materials** (3.3) is the one place REN-3 depends on a capability proven in a test but
   never driven from the SceneRenderer.

## Files

- `frame_graph.hpp` — depth/depth-array transients; **`create_persistent_image` + ping-pong**; compute-pass
  wiring for the sky bakes.
- `raster_context.hpp` — `draw_storage_depth_only` **appended at END** (D135); cube/per-mip render targets;
  the velocity target.
- `vulkan_raster_context.cpp` / `dx12_raster_context.cpp` — borrowed depth target/texture; depth-only record
  path; depth RTT barriers; depth-array slices; cube RTT + mip chain; the persistent-history exclusion.
- `scene_renderer.cpp` — shadow pre-pass; the real BRDF + normal/MR sampling; HDR target + post chain; sky +
  IBL binding; jitter + previous-frame transforms + motion vectors; the TAA resolve + sharpen passes.
- `sandbox/src/main.cpp` — the scene that is the close gate.
- tests — every increment above, both backends; `docs/bench/` boards at measurement time.

## Why this sequencing

3.1 is the smallest self-contained increment and everything shadow-shaped depends on it. 3.2 completes shadows
before anything amplifies their artifacts. 3.3+3.4 must land **together conceptually** (a real BRDF without a
tonemap is a visual regression). 3.5 is where the scene stops looking like a tech demo, and it de-risks compute
in the graph. 3.6 is last among the systems because TAA *exposes* every temporal instability upstream of it —
cascade swim, shadow acne, exposure flicker — so it should land when there is a stable image to accumulate.
3.7 assembles the result into the thing the user actually asked to see.
