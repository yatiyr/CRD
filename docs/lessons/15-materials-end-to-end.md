# 15 — Materials, end to end: surface, lowering, variants, neural

> *Everything about materials in Cerid — what a material is, the OpenPBR surface contract, the MaterialX node library, how a
> material fragment is emitted and lowered, how it flows through the variant matrix (with the real gotchas hit while wiring it),
> deferred vs forward, and how neural materials fit. Grounded in `ckir_material.hpp`, `ckir_nodes.hpp`, `ckir_cook.hpp`,
> `ckir_lower.hpp`, `ckir_neural.hpp`, and the `[variant][material]` gate.*

Read [12](12-ckir-deploy-pipeline.md) (deploy) and [14](14-variants-permutation-and-specialization.md) (variants) first — this
lesson builds on both.

---

## 1. The one line that defines the whole system

> **A material is a surface response. Lighting is a separate technique.** (ADR-0102)

A material in Cerid is a **CKIR fragment graph that outputs a surface-parameter struct and computes *no* lighting**
(`ckir_material.hpp:3-9`). It answers exactly one question — *at this pixel, what are the shading inputs?* — and stops. The
lights, shadows, GI and tone-mapping are the *render path's* job, downstream and independent.

Why this line matters more than any other: because a material never mentions lights, the **same** material works unchanged under
forward rendering, a deferred G-buffer, or a path tracer. Swap the lighting technique and every material in the scene comes
along for free. Break this line — let a material bake in "3 point lights" — and you've fused surface and transport, and now every
material has to be rewritten when the renderer changes.

---

## 2. The surface contract — the OpenPBR 1.1 slab

The output every material and the future lighting pass agree on is a fixed, **append-only** struct: the OpenPBR 1.1 surface slab
(`SurfaceField` enum, `ckir_material.hpp:43-96`). Field order *is* the layout — new fields append at the end, never reorder
(matching the shipped renderer's `SurfaceData` stability rule).

- **Compact core (the deferred G-buffer, B5-a):** `BaseColor`(vec3) · `Metallic` · `Roughness` · `Normal`(vec3) ·
  `Emissive`(vec3) · `Occlusion` · `Opacity`.
- **OpenPBR layers (B5-b):** base extras (`BaseWeight`, `DiffuseRoughness`) · specular (`SpecularWeight/Color/Ior/Anisotropy/
  Rotation`) · transmission · subsurface · coat · fuzz/sheen · thin-film · geometry (`ThinWalled`, `Tangent`, `CoatNormal`) ·
  `EmissionLuminance`.
- **Intent tags (float-encoded enums):** `ShadingModel`, `AlphaMode`.

A material builds a surface with a few helpers:

- `int struct_id = material::define_surface(g)` — registers the slab as a struct type (`:99`).
- `material::surface_defaults(g, out[SfCount])` — fills OpenPBR defaults (base 0.8, roughness 0.5, IOR 1.5, off-layers weight 0);
  a material overrides only what it drives (`:119`).
- `int surf = material::build_surface(g, struct_id, base, metallic, roughness, normal, emissive, occlusion, opacity)` — the
  metallic-roughness convenience: defaults + the 7 core overrides (`:159`).
- `material::build_surface_full(g, struct_id, fields[SfCount])` — every field explicitly (`:153`).

**Intent tags** (`:19-38`): `ShadingModel { Standard, Unlit, Toon, Cel, Gooch, Outline, Hatching }` and
`AlphaMode { Opaque, Masked, Translucent, Additive }`. The material declares intent; the render path (B8) reads it and applies
the matching model. The material itself stays lighting-agnostic.

---

## 3. Authoring — the MaterialX node library

A material's surface fields are computed by composing nodes. The library (`ckir_nodes.hpp`, ~97 builders, + 8 noise in
`ckir_noise.hpp`) is a **bit-exact transcription of MaterialX's `genglsl` reference**, verified against the CPU oracle. A "node"
is a free function `int node(KGraph& g, int a, …)` returning the output node id. Categories:

| category | examples |
|----------|----------|
| Math (~30) | add/sub/mul/div, `modulo`, power, trig, min/max, `clamp`, `smoothstep`, `remap`, sign, ln, exp |
| Geometric | normalize, magnitude, dotproduct, crossproduct, distance |
| Logical / conditional | and/or/not/xor, `ifgreater`, `ifequal`, `switch5` |
| Channel | combine2/3/4, extract, convert |
| Adjustment / color | luminance, contrast, saturate, `rgbtohsv`/`hsvtorgb`, hsvadjust |
| Compositing (~18) | plus, minus, screen, overlay, dodge, burn, `over`, `in`, `out`, `mask`, `mix` |
| Convolution | `heighttonormal` (Sobel, fragment-only) |
| Procedural | ramplr/tb, aastep, `checkerboard`, `perlin2/3`, `cell2/3`, `fractal2/3`, `worley2/3` |
| Geometry / UV inputs | position, normal, tangent, bitangent, `texcoord`, geomcolor |
| UV transforms | rotate2d/3d, place2d, `triplanar` |
| NPR | viewdirection, facingratio, gooch_shade |

The **per-fragment inputs** a surface consumes are the interpolated varyings a vertex shader supplies —
`cook::SurfaceInputs { uv, world_normal, view_dir, world_pos }` (`ckir_cook.hpp:38`). A concrete surface reads them:

```cpp
int base   = g.vec3(g.swizzle(in.uv, 0), g.swizzle(in.uv, 1), kf(0.5)); // base color from uv
int normal = g.normalize(in.world_normal);
return material::build_surface(g, struct_id, base, kf(0.0), kf(0.5), normal, emissive, kf(1.0), kf(1.0));
```

---

## 4. From surface to a shader — deferred vs forward

A material graph is compiled into a **fragment `KEntry`** whose outputs depend on the render pass
(`cook::build_fs_for_pass`, `ckir_cook.hpp:78`; `cook::PassType { Shadow, DepthPrepass, GBuffer, Forward }`):

- **Deferred (`GBuffer`):** `material::pack_gbuffer(g, e, surface)` (`ckir_material.hpp:189`) sets `e.stage = Fragment`,
  `e.n_out = 4`, and packs the surface into 4 RGBA8 MRT attachments: `(base,metal)`, `(enc_normal,rough)`, `(emissive,occ)`,
  `(opacity, shading_model/255, alpha_mode/255, 1)`. `pack_gbuffer_ext` (`:217`) is the 8-attachment version carrying every
  OpenPBR layer. The lighting pass reads the G-buffer later.
- **Forward:** `cook::shade_forward(g, surface, in, light_dir, light_color)` (`:65`) extracts (base, metallic, roughness,
  normal, emissive) from the surface and evaluates the Cook-Torrance BRDF against a light *right there*, writing one lit color.
- **Masked alpha:** `material::set_masked(g, e, surface, cutoff)` sets `e.discard_cond = (opacity < cutoff)` → the emitter
  writes `if (discard_cond) discard;`.

That fragment entry emits to real bytecode **today**: `emit_stage_glsl` / `emit_stage_hlsl` (`ckir_glsl.hpp:1152`) produce the
VS/FS text — `layout(location) in`/`out`, texture/sampler binds, `std140` UBOs, the MRT color writes — and
`compile_glsl_to_spirv(ShaderStage::Fragment, …)` / `compile_hlsl_to_dxil` produce SPIR-V / DXIL. A fragment `KEntry` differs
from a compute kernel: it carries its results in `out[]`/`n_out`/`frag_depth`/`discard_cond` and `stage == Fragment`, whereas a
kernel has an imperative statement body and writes via storage buffers (`ckir.hpp:682` says it outright: *"n_out/out are unused
for a kernel"*).

---

## 5. Lowering — pushing work to the cheapest stage

Before it ships, a material graph is *lowered* (B7, `ckir_lower.hpp`). Every node is classified by how often it changes —
`Frequency { Constant, Uniform, Vertex, Fragment }` (`:27`) — and pushed to the cheapest stage that's still correct
(`classify`, `:47`). A `Const` is compile-time; a `UniformBlock`/`Texture` is per-draw; a vertex attribute is per-vertex; a
`FragCoord`/derivative/implicit-LOD sample/storage read forces per-fragment (`is_fragment_forcing`, `:40`). An interior node is
the *max* of its inputs' frequencies. `uniform_boundary` (`:132`) finds the maximal uniform subexpressions consumed on the
per-fragment path — the hoist targets (compute per-draw instead of per-pixel).

The single lowering entry a cook calls is `lower::lower_entry(g, e)` (`:107`): it gathers the entry's live roots (position,
frag_depth, discard, every `out[k].node`), runs `optimize` (const-fold → DCE → CSE), and writes the renumbered ids back. This is
the **same `optimize`** that powers variant specialization — materials and kernels share it.

---

## 6. Materials through the variant matrix — the wiring, and its scars

The whole point of D3: a material with feature toggles (normal-map on/off, emissive on/off, alpha-test on/off) is an
**übershader**, and each toggle combination is a **variant**. A material variant now flows through the *exact same*
`cook_variant_matrix` as a compute kernel — the builder just returns a `Fragment` entry:

```cpp
KEntry build_material_variant(KGraph& g, u32 key, void*) {
    SurfaceInputs in;  in.uv = g.stage_in(vec2, 0, Smooth);  in.world_normal = g.stage_in(vec3, 1, Smooth);
    int opt_emissive = g.constant(0.0);            // ShaderOption 0 — emissive on/off (real)
    int opt_debug    = g.constant(0.0);            // ShaderOption 1 — declared, UNUSED (dead) → dedups
    int emissive = g.select(g.binary(CmpGt, opt_emissive, kf(0.5)), emis_on, black);
    int surf = material::build_surface(g, define_surface(g), base, kf(0.0), kf(0.5), normal, emissive, kf(1.0), kf(1.0));
    KEntry e;  material::pack_gbuffer(g, e, surf);       // → Fragment, n_out = 4
    int opts[2] = {opt_emissive, opt_debug};
    shadercook::specialize(g, e, opts, key, 2);          // the SAME helper as a compute kernel
    return e;
}
```

`[variant][material]`: **`requested=4 unique=2` (50% dedup)** — the dead `opt_debug` DCE's away, so the four variants collapse to
two bundles; the cooked artifact is real **Fragment SPIR-V (1032 B)**, verified by loading it into a `VkShaderModule`. Three
things had to be true to get here, and each was a scar worth keeping:

1. **The cook had to become stage-aware.** `cook_compute_shader` hardcoded the compute emitters + `ShaderStage::Compute`. It now
   dispatches on `e.is_kernel()`: a kernel → `emit_compute_kernel_glsl` @ Compute; a material → `emit_stage_glsl` @ Fragment. So
   both flow through one cook (CUDA/MSL/WGSL stay compute-only — a material cooks SPIR-V + DXIL).
2. **All options must be pinned before the fold — not one at a time.** `optimize` renumbers the graph, so specializing option A
   first invalidates option B's node id. The unified `specialize()` pins *every* option, *then* folds once. (The material path
   dispatches to `lower_entry`; the compute path to `specialize_kernel`.)
3. **A vector `Select` needs `fold_static_branches`, not just `optimize`.** `optimize`'s const-fold only collapses *scalar* ops
   (`comps()==1`) — it deliberately skips vector nodes. An emissive `select(cond, vec3, vec3)` is exactly a vector select, so
   `optimize` left `bool t12 = 1.0; vec3 t13 = t12 ? t9 : t11;` — invalid GLSL (`bool = float`) and an uncollapsed branch. The
   fix is the full B7 sequence the material path now runs: `lower_entry` → `fold_static_branches` (which *aliases* a
   const-condition Select to its chosen branch, vector or not) → `lower_entry` (DCE the dead branch). This is why the material
   branch of `specialize()` is three steps, not one.

The unified `specialize()` (`variant.hpp`) is the elegant result — one call, dispatching on stage:

```cpp
if (e.is_kernel()) { /* pin + specialize_kernel (compute body) */ }
else { for each opt: g.pin_const(opt, bit);
       lower_entry(g,e); fold_static_branches(g); lower_entry(g,e); }   // material/raster
```

---

## 7. Neural materials — the same question, a learned answer

A **neural material** (`ckir_neural.hpp`, B10) answers the same question — surface properties at a pixel — by evaluating a tiny
trained MLP instead of fetching textures. The inputs (uv, and later view/params) are **frequency-encoded** (NeRF / Instant-NGP
positional encoding: band `k` contributes `[sin(2^k·π·u), cos(…), sin(…v), cos(…)]`, `neural_uv_encode`, `:306`) and pushed
through a small MLP that runs on the **cooperative-vector** tensor path (`emit_coopvec_mlp_glsl`, `:63` — `coopVecMatMulAddNV`
per layer, ReLU hidden, linear out). It even trains **on the GPU**: `emit_coopvec_linear_train_glsl` (`:253`) does the forward,
the loss gradient, and the hardware `coopVecOuterProductAccumulateNV` weight-grad + `coopVecReduceSumAccumulateNV` bias-grad.

| | conventional | neural |
|-|--------------|--------|
| surface = | texture samples + node math | a per-pixel MLP eval |
| bound by | memory bandwidth + VRAM | compute (small fp16 matvecs) |
| authored | artists / MaterialX | trained to match a reference (Adam) |
| in Cerid | ~97 nodes → OpenPBR slab | 12.5× over scalar MLP, ~38.9 dB, on-device training |

**Wiring the neural material into the OpenPBR slab (done, 2026-07-22).** The original neural render kernel was a *compute* kernel
that wrote RGBA8 directly (`packUnorm4x8`, `:398`) — not a `SurfaceField`/`Fragment` producer, so it couldn't feed the deferred
lighting path. `emit_neural_surface_fs_glsl` closes that: it's a **fragment** shader that runs the SAME coopvec MLP *per pixel*,
maps its outputs onto the surface slab (`[0..2]`=base color, `[3]`=metallic, `[4]`=roughness; normal from the interpolated
varying), and writes the **4-MRT deferred G-buffer** in the exact `pack_gbuffer` layout. So a neural material now produces the
*same* G-buffer as a conventional one and feeds the *same* lighting pass. Verified: it compiles to real Fragment SPIR-V (3792 B)
the driver accepts, with the coopvec matmul running in the fragment stage (the intended neural-material path). The remaining
refinement is outputting a *learned* normal (needs `out_dim ≥ 8`) rather than the geometric one, and training against a reference
material — both ride the existing coopvec training path (`emit_coopvec_linear_train_glsl`).

---

## 8. How a material feeds lighting

The seam is one-directional and clean:

```
material graph → surface slab → { deferred: pack_gbuffer → G-buffer → lighting pass reads it
                                  forward:  shade_forward → Cook-Torrance BRDF vs lights → lit color }
                                                          ↳ + DDGI / ReSTIR / neural radiance cache (indirect)
                                                          ↳ + SVGF denoise
```

`shade_forward` (`ckir_cook.hpp:65`) is the reference integrator: pull (base, metallic, roughness, normal, emissive) out of the
slab, run `lighting::directional_light` (the B8 Cook-Torrance BRDF), add emissive, clamp. In deferred, the same slab is packed
into the G-buffer and a separate full-screen lighting pass does the integration. Either way the material never changed — that's
the payoff of the surface-vs-transport line from §1.

---

## 9. Authoring, and the node editor

Materials are authored **programmatically** today — the ~97 MaterialX builders + the `material::` surface helpers construct a
`KGraph`. A future **node editor** is a second front-end onto the same IR: a "normal map" node becomes a `Texture` +
`heighttonormal`, a feature checkbox becomes a `ShaderOption`. Everything downstream — lowering, variants, cook, dedup, cache,
hot-reload — is identical regardless of which front-end drew the graph. The surface contract, the node library, and the variant
machinery are the stable substrate; the editor is just paint.

---

## The one-paragraph version

A material is a fragment graph that outputs the append-only OpenPBR surface slab and computes no lighting — that surface-vs-
transport line is what lets one material survive any renderer. You author it by composing ~97 bit-exact MaterialX nodes into the
slab's fields, pack it for deferred (`pack_gbuffer` → 4 MRT) or forward (`shade_forward` → BRDF), and lower it so each node runs
at its cheapest correct frequency. Feature toggles are `ShaderOption`s, and a material variant flows through the *same*
`cook_variant_matrix` as a compute kernel — once the cook is stage-aware, all options are pinned before folding, and the vector
`Select`s are collapsed with `fold_static_branches`. Neural materials answer the same surface question with a trained
cooperative-vector MLP; wiring their outputs into the OpenPBR slab is the one piece still ahead.

---

*Companion lessons: [13](13-shaders-pipelines-materials-lighting.md) (concepts), [14](14-variants-permutation-and-specialization.md)
(variant internals), [12](12-ckir-deploy-pipeline.md) (deploy). References: ADR-0102 (material contract), ADR-0104 (deploy).*
