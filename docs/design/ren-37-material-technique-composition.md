# REN-37 design — MATERIAL × TECHNIQUE composition: how an authored frame graph reaches into the fragment shader

> ## ⛔⛔⛔ TOP RULE — WE WILL ONLY USE OUR AUTHORED FRAME GRAPHS
> Every rendering technique ships as an authored `.frame.toml` ASSET, never as C++ that builds passes.
> This document exists because CSM exposed the one place that rule was not yet expressible: **a lighting
> technique changes the FRAGMENT SHADER of every material it touches**, and an asset that can only schedule
> passes cannot express that. This is the design that closes it.

**Status**: design, 2026-07-25. Written after REN-3.2-b's cascaded shadow maps were (wrongly) implemented as
hardcoded C++, and the user asked for the architecture to be made *fully right* before going further.

---

## 1. The question, stated precisely

A frame-graph asset schedules **passes**. But a technique like CSM is not only a schedule — it changes what the
**fragment shader computes**:

- the shadow pass needs a **depth-only** FS for every material,
- the forward pass needs each material's FS to additionally **select a cascade, project, PCF-filter, and
  attenuate the light**,
- both need **pass-frequency bindings** (the atlas, the cascade matrices, the splits) that the material author
  never mentions and must not have to.

So the question is: *what is the seam between the authored graph, the authored material, and the generated
shader — such that a user can invent a technique without engine code, and without every material knowing about
every technique?*

## 2. What already exists (this is the important part)

The answer is **mostly already designed and built**, which changes the recommendation from "invent an
architecture" to "connect three things that were built separately". Grounded in the code:

| piece | where | what it gives us |
|---|---|---|
| **The governing principle** | ADR-0102, quoted in `ckir_material.hpp:3` | *"material = surface response; render path = lighting technique"* — a material **outputs an OpenPBR surface struct and does NOT compute lighting** |
| **The surface contract** | `ckir_material.hpp` | the OpenPBR 1.1 surface slab, **append-only** field order — "the surface contract every material and the future lighting pass agree on" |
| **Shading model as a TAG** | `ckir_material.hpp` `ShadingModel` | the material *declares intent* (Standard/Unlit/Toon/Cel/Gooch/Outline/Hatching); the render path *applies* it |
| **Pass differentiation** | `ckir_cook.hpp` `PassType` | Shadow · DepthPrepass · GBuffer · Forward — *"The FS differs per pass; the SURFACE is authored once"* |
| **The composition point** | `ckir_cook.hpp` `build_fs_for_pass` | surface built once, then routed per pass: depth-only (`n_out=0`), G-buffer pack, or `shade_forward` |
| **Variant key + specialization** | `ckir_cook.hpp` `VariantOptions`, `specialize_variant` | bake a `ShaderOption` to a compile-time value; the static branch collapses and DCE reclaims the dead side |
| **Lowering** | B7 `lower_entry` | const-fold → DCE → CSE, round-trip bit-stable |
| **Permutation + dedup** | D3 | the variant matrix with **content-hash dedup** |
| **Pass declares its material variant** | `frame_asset.hpp` `FrameMaterialPass` | the ASSET can already say `material_pass = "Forward"` |

**So the split the user is asking for already exists as doctrine and as code.** The material is lighting-agnostic;
the FS differs per pass; variants are specialized, lowered, and deduped.

## 3. ⛔ The actual root problem

`build_fs_for_pass` is referenced in exactly three files: its own header, and **two test files**.

```
engine/kir/include/crd/kir/ckir_cook.hpp
tests/gpu-shared/ckir_raster_triangle.hpp
tests/kir/test_ckir_lighting.cpp
```

`scene_renderer.cpp` references it **zero** times. The real renderer **hand-writes** its vertex and fragment
shaders (`build_scene_vs`, `build_scene_fs`, `build_scene_fs_textured`, and — my doing — `build_scene_fs_shadowed`)
and never touches the material cook path at all.

That is the root cause of everything that went wrong in REN-3.2-b:

- CSM became C++ **because the FS was already C++**. There was no material-variant pipeline to extend, so
  "add shadows" meant "hand-write another FS", which meant "hand-write the passes that feed it".
- Shadows and albedo **fight over descriptor slot 1** because the hand-written FS has an ad-hoc binding layout
  instead of the set-frequency model.
- A material's `ShadingModel` tag is **ignored** — the renderer has one hardcoded `0.25 + 0.75·N·L`.

**The bridge — renderer consumes cooked material variants instead of hand-written shaders — is the slice that
makes the top rule achievable.** Without it, every future technique repeats this failure.

## 4. The architecture: three authored layers, one generated shader

```
   MATERIAL GRAPH            TECHNIQUE GRAPH             FRAME GRAPH
   (.crdm, authored)         (.crdt, authored)           (.frame.toml, authored)
   surface response          lighting technique          schedule + resources
        │                          │                            │
        │  SurfaceData             │  shade(surface, bindings)  │  passes, reads/writes,
        │  (OpenPBR slab)          │                            │  material_pass, for_each
        └──────────┬───────────────┘                            │
                   ▼                                            │
            COOK-TIME COMPOSITION  ◄───── variant key ──────────┘
            build_fs_for_pass(material, technique, pass, options)
                   │
                   ├─ B7 lower_entry  (const-fold → DCE → CSE)
                   ├─ specialize_variant  (bake declared options)
                   └─ D3 content-hash dedup
                   ▼
            ONE cooked FS variant per (material × technique × pass × options)
```

**The rule that makes it composable:** a material may only *read* the surface contract; a technique may only
*read* `SurfaceData` + its declared bindings. Neither can see the other. The **cooker** is the only thing that
sees both, and it is the only thing allowed to join them.

### 4.1 What must be added — the TECHNIQUE as a first-class authored asset

Today the lighting half is `shade_forward` — a **fixed function** with a hardcoded single directional light and
no shadow lookup. It must become the same kind of thing a material is: an authored, named, cookable graph.

```
technique "crd://technique/forward_csm"
  consumes  SurfaceData
  requires  bindings:
              shadow_atlas : texture2DArrayShadow   (pass frequency)
              csm_splits   : float[4]               (pass frequency)
              csm_light_vp : mat4[4]                (pass frequency)
  options   cascade_count : static int  (1..4)
            pcf_taps      : static int  (1|4|16)
  provides  shade(surface, view, light) -> vec4
```

`build_fs_for_pass` then generalizes from *"call `shade_forward`"* to *"call the technique the pass named"*.
That single change is what lets an authored graph choose the lighting.

### 4.2 The BINDING CONTRACT — the piece that makes it safe

This is the load-bearing idea, and it is what turns "the graph reaches into the shader" from a hack into a
checked contract:

- the **technique declares** the pass-frequency inputs it consumes, by name and type;
- the **frame-graph pass declares** what it reads (`reads = ["shadow_atlas"]`);
- the **cooker verifies they match** — name, type, and array-ness.

A mismatch is a **cook-time rejection with a name**, exactly like the 19 rejections the frame cooker already has.
Not a black screen, not a validation error on a user's machine. This is also what removes the descriptor-slot
fight: bindings are assigned by the cooker from the set-frequency model (0 frame · 1 pass · 2 material ·
3 object), not hand-picked per draw.

## 5. Übershader vs variants — and why our IR changes the usual answer

The classical trade:

| | übershader (one program, dynamic branches) | variant explosion (specialize everything) |
|---|---|---|
| codegen | worst-case registers, occupancy loss for everyone | optimal per combination |
| PSO count | one | thousands |
| cook time | trivial | large |
| iteration | instant | recompile per tweak |
| failure mode | *everything is slightly slow* | *cook times and memory blow up* |

**Most engines pick a point on this line. We do not have to, because we own an IR.**

`specialize_variant` + `lower_entry` means a *single authored übergraph* can be mechanically reduced to an
optimal static variant: bake the option, the static branch collapses, DCE reclaims the dead side. The übershader
and the variant are **the same authored artifact at two lowering levels**. That is a genuine advantage over
engines whose shaders are text.

**Therefore the recommendation is a dual mode from one source:**

- **Ship / cook mode** → specialize each *declared* combination, lower, dedup by content hash. Optimal codegen,
  no runtime branching, PSO set known ahead of time.
- **Editor / iteration mode** → emit the übergraph **unspecialized**, with options as *uniform* branches (uniform
  control flow is nearly free — the branch is coherent across the whole draw). One program, instant material
  tweaks, no recook per slider drag.

Same graph. Same authored material. The mode is a cook flag, not a second authoring path. **A second authoring
path would be the real mistake** — it is how engines end up with an editor look that differs from the shipped one.

## 6. Controlling the explosion — and the collapse that is already free

The variant key must be **DECLARED, not discovered**:

```
key = (material_id, technique_id, pass_type, shading_model, alpha_mode, declared_option_values, vertex_layout)
```

Three mechanisms keep the matrix small, and the third is the elegant one:

1. **The asset declares what varies.** A pass states its technique and which options are static for it. The cook
   enumerates only that set — it never explores combinations no authored graph asks for.
2. **`ShadingModel` is a material tag.** Only materials that declare `Toon` ever get a Toon variant.
3. **⭐ Lowering collapses whole axes automatically.** For `PassType::Shadow`, `build_fs_for_pass` sets
   `n_out = 0` and never consumes the surface. `lower_entry` then DCEs the *entire* surface computation — every
   texture fetch, every parameter. **All opaque materials therefore cook to the SAME empty shadow FS and dedup
   to one variant by content hash.** A thousand materials produce one shadow program, with no special-casing
   anywhere. Only `Masked` materials keep an alpha path, so they form a second small family.

That third point is worth stating loudly: the property engines usually engineer with hand-written "depth-only
permutations" falls out of *lowering + content hash* for free, because we compose in an IR rather than in text.

## 7. The übergraph ACROSS passes = quality tiers

The same reasoning applies one level up. A frame graph can itself be an übergraph: shadows on/off, GI on/off,
post chain long/short. The asset already has the machinery — `requires` capability tiers and a `fallback` graph.
So:

- **quality tier = frame-graph variant**, selected by declared capability, with a named fallback;
- a low tier that drops the cascade passes *also* drops the shadow option from the material variant key, so the
  material variants for that tier are automatically fewer.

The two übergraph levels (shader and frame) share one idea: **declare the axis, specialize on it, dedup the
result.**

## 8. Sequenced plan

| # | slice | why this order |
|---|---|---|
| **37.1** | **Renderer consumes cooked material variants.** Replace `build_scene_*` hand-written shaders with `MaterialTemplate` + `build_fs_for_pass`. Set-frequency binding layout (0 frame · 1 pass · 2 material · 3 object). | Nothing else is possible while the FS is hand-written. This alone fixes the shadow/albedo slot fight. |
| **37.2** | **Technique as an authored asset.** `.crdt` schema + cooker; `build_fs_for_pass` calls the named technique instead of `shade_forward`. Port `shade_forward` to be the first authored technique (`standard_forward`). | Makes the lighting half authorable without changing any material. |
| **37.3** | **The binding contract.** Technique declares required pass-frequency inputs; frame-graph pass declares reads; cooker verifies and assigns bindings. Mismatch = named cook-time rejection. | The safety property. Also removes hand-picked descriptor slots. |
| **37.4** | **CSM as an authored technique.** `forward_csm` technique + the existing `forward_csm.frame.toml`. Delete the hand-written shadowed FS and the C++ cascade passes. | The user-visible payoff, and the deletion is the proof. |
| **37.5** | **Variant matrix + dual mode.** Declared key, cook-time specialization, content-hash dedup (D3), plus the unspecialized editor übershader from the same graph. | Performance and iteration, once correctness is authored. |

**Gate for the whole thing:** a user changes the cascade count, swaps `forward_csm` for `deferred_csm`, or
authors a brand-new toon technique — **by editing assets only**, with no engine recompile, and the sandbox
renders it. That is the same test the top rule states, applied to the shader half.

## 9. What I am explicitly NOT proposing

- **Not** a shader-graph DSL. ADR-0081 stands: C++ only, no scripting language. A technique is a CKIR graph
  built by the same builders materials use, serialized like any other asset.
- **Not** letting materials see lighting. The moment a material can sample a shadow map, the technique axis and
  the material axis stop being independent and the variant matrix becomes the product of two unbounded sets.
- **Not** a runtime übershader as the shipping path. It is the *editor* path; shipping specializes.
- **Not** per-technique renderer code paths. If a technique needs something the composition cannot express, that
  is a missing `FramePassKind` or a missing binding type — extend the vocabulary, never the special cases.

---

## 10. Industry grounding (2025–26) — what others actually do

Researched rather than recalled. Each row is here because it either **validates** a decision above or **supplies
a capability we lack**.

| source | what they do | what it means for us |
|---|---|---|
| **Slang** (Khronos, 2025) | modules + **interfaces + generics** express specialization "without the need for preprocessor techniques or string-pasting"; modules compile to a **custom IR**, link at runtime to DXIL/SPIR-V | **Direct validation of CKIR.** The frontier is moving *toward* an IR with typed interfaces — which is what we already have. Our technique/material split is Slang's interface/implementation split, expressed in our own IR. |
| **UE5 Substrate** (5.4→5.8) | replaces the fixed shading-model list with **modular slabs + operators**; *parameter blending* merges two slabs so only **one lighting evaluation** is needed | Confirms modular composition beats a fixed model enum. Adopt the **cost fallback**: when two techniques/layers would each evaluate lighting, offer a merged single-evaluation path. |
| **Filament** | material = surface properties; **engine = light culling, shadow sampling, BRDF, AO, compositing**; `variantFilter` lets the app **declare variants it guarantees are never needed** | Same split as ADR-0102 — good. **Adopt `variantFilter`**: our declared key should support explicit *negative* declarations, not just positive enumeration. |
| **Frostbite FrameGraph / AMD RPS / Activision task graph** | transient vs **persistent**: history buffers (TAA), shadow atlases are **imported** and **excluded from aliasing**; RPS has `RPS_RESOURCE_FLAG_PERSISTENT` | We already exclude imported resources from aliasing. What we **lack** is a way for the ASSET to declare persistence — today only C++ can import. That is 37.5. |
| **AMD RPSL** | render graph as a **program**; **subprograms** compose whole graphs as node callbacks; `[subgraph(atomic, sequential)]` prevents external nodes being scheduled into a protected scope | Supplies **subgraphs** (37.6). Also a warning: composition needs **scheduling attributes**, or an injected pass can be reordered into the middle of something that must stay atomic. |
| **Unity URP** | **injection points** (`RenderPassEvent`) + Renderer Features (`AddRenderPasses`); render graph merges passes and keeps TBDR work in tile memory | Supplies **injection points** (37.6) — the mechanism for "add a pass between existing nodes" *without forking the base graph*. |
| **Unity / Unreal variant handling** | keyword explosion reaches millions pre-filter; Unity **dedups identical variants to the same bytecode**; Unreal auto-strips unused permutations and uses **Material Layers** | Our content-hash dedup is the same idea — but ours dedups the **lowered IR**, which collapses more (see §6's shadow-pass collapse). |

**The synthesis:** the industry is converging on *(a)* an IR with typed interfaces for shaders, *(b)* declarative
render graphs with subgraph composition and named injection points, *(c)* aggressive variant dedup with
author-declared filters. We already have (a) and most of (c). **(b) is the gap**, and it is exactly what the user
asked for: blur chains, ping-pong, and inserting passes between existing nodes.

## 11. The three capabilities this adds to the asset

### 11.1 PERSISTENT + PING-PONG resources (blur chains, TAA history, temporal reuse)

Everything the graph owns today is a transient whose *entire purpose* is to be aliased away. A blur chain needs
intermediate targets (fine, transient), but TAA history, SSR/DDGI/ReSTIR reuse, and auto-exposure need data that
**survives across frames** — and must therefore be **excluded from aliasing**, exactly as Frostbite and RPS do.

```toml
[[resource]]
name   = "bloom_h"          # ordinary transient - aliased freely
kind   = "transient_image"
scale  = 0.5
format = "RGBA16F"

[[resource]]
name   = "taa_history"      # survives frames; NEVER aliased
kind   = "pingpong_image"   # two physical buffers, addressed $prev / $curr
scale  = 1.0
format = "RGBA16F"
```

- `persistent_image` — one buffer, contents survive; excluded from the aliasing pool.
- `pingpong_image` — **two** buffers with `$prev` / `$curr` addressing, swapped each frame by the executor. A
  pass reads `taa_history[$prev]` and writes `taa_history[$curr]`; the swap is the executor's job, so no author
  ever hand-manages a frame parity bit (the classic source of one-frame-stale bugs).
- ⛔ Both must be **excluded from transient aliasing** and from the retire queue — the existing
  `retire_transients_to` path frees graph-owned images once their fence signals, which is precisely wrong for a
  resource whose value *is* its history.

A separable Gaussian blur then needs **no engine code**: two fullscreen passes over a transient pair, plus a
`for_each` if you want a mip chain.

### 11.2 SUBGRAPHS — techniques compose instead of being copy-pasted

```toml
[[include]]
graph = "crd://technique/bloom"
as    = "bloom"
bind  = { input = "hdr_color", output = "hdr_color" }   # parameter binding, not global names
```

The included graph's resources and passes are namespaced under `bloom.*`, so two instances cannot collide. Taken
from RPS subprograms — **and so is the warning**: composition needs scheduling attributes. A subgraph that must
not be interleaved declares itself atomic, or a later injection can be scheduled into the middle of it.

### 11.3 INJECTION POINTS — insert a pass without forking the base graph

This is the Unity URP idea, and it is the one that makes the system genuinely *extensible* rather than merely
*editable*: a user should not have to fork `forward_csm` to add an outline pass.

```toml
# in the base graph
[[anchor]]
name  = "after_opaque"
after = ["forward"]
before = ["post"]

# in a SEPARATE asset the app ships
[[inject]]
at   = "crd://frame/forward_csm@after_opaque"
pass = "my_outline"
```

The executor splices injected passes at the anchor, then runs the **normal dependency sort** — so an injected
pass that reads something produced later still lands correctly, and a cycle is still rejected by name. Anchors
are **declared**, which means the base-graph author states where extension is safe; that is the difference
between an extension point and a monkey-patch.

## 12. Why this is future-proof (the honest argument)

Not "it is flexible", but *what specifically it absorbs without redesign*:

- **A new shading model** (hair, cloth, car paint, NPR) → a technique asset. No engine change.
- **A new render architecture** (deferred, Forward+, visibility buffer) → a frame-graph asset + a `material_pass`
  value that already exists (`GBuffer`). The material is untouched.
- **A new temporal technique** (SSR, DDGI, ReSTIR, auto-exposure) → `pingpong_image` + passes. Already the shape
  TAA needs, so the first one pays for the rest.
- **A new backend** (WebGPU, Metal) → CKIR already emits WGSL/MSL; the asset names no API.
- **A new hardware capability** (work graphs, mesh nodes) → a new `FramePassKind` + a capability tier, with a
  declared fallback graph. The vocabulary grows; the special cases do not.

The failure mode this design is chosen to avoid: **per-technique renderer code paths**. Every one of them is a
place where the authoring system provably cannot express something, and they compound. If a technique needs
something the composition cannot express, the fix is a new *binding type* or a new *pass kind* — never an
`if (technique == CSM)` in the renderer.


---

## 13. 37.1 progress + the open lead (2026-07-25)

**Landed and live:**
- `scene_build_surface` — the scene's material as an **OpenPBR surface with no lighting** (ADR-0102), fed to
  `build_fs_for_pass` through `MaterialTemplate`. The per-instance tint and the base-colour map ride
  `MaterialTemplate::user`, so the shared `SurfaceInputs` stays the engine-wide contract.
- `build_scene_fs_cooked(pass, textured)` — cooks the scene FS for any `PassType`.
- ⭐ **The shadow FS is no longer hand-written.** `build_shadow_fs` now calls the cooked path at
  `PassType::Shadow`; `n_out = 0`, the surface is never consumed, and B7 `lower_entry` DCEs the whole surface
  computation. **The free collapse of §6, exercised for real.**
- The cooked VS varying set (normal · tint · worldpos+depth · uv).

**⚠ NOT yet wired: the cooked FORWARD variant.** It renders BLACK, and the forward programs are deliberately
still on the hand-written FS rather than leaving the renderer broken.

**Evidence gathered (so the next session does not repeat it):**

| probe | result | conclusion |
|---|---|---|
| emissive = constant colour | **renders** | geometry, VS/FS interface, storage/header reads are all CORRECT |
| base colour = constant | still black | not the instance tint |
| emissive = `abs(world_normal)` | red ≈ 14/255 ≈ 0 | the normal varying **is** correct — `(0,1,0)` as expected |
| `in.world_normal` = **constant** `(0,1,0)` | `lit` = **27** (was 0) | with a constant normal the BRDF produces light |

⛔ **RESOLVED — and my reading of the evidence above was wrong.** The `abs(world_normal)` probe read the RED
channel, which is 0 for **both** `(0,1,0)` and `(0,0,0)`; it proved nothing. Re-probing with
`splat(world_normal.y)` gives **~17/255 ≈ 0.07** — the world-normal varying is **essentially ZERO**.

The emitter hypothesis is also ruled out: `ckir_glsl.hpp:1257` emits `layout(location = nd.iidx)` from the node's
own explicit index, and only reachable `StageIn` nodes are declared. Locations are honoured.

**So the real bug is that the scene VS's world normal arrives at the FS as ~0**, and
`lit = (Fd + Fr·E)·NoL·light` is therefore 0. Full measurement table and the ordered list of things to check
live in memory (`project_world_normal_varying_reads_zero`).

⛔⛔ **Why this went unseen:** the toy shader is `0.25 + 0.75·max(N·L,0)`. With a zero normal it still returns the
**0.25 ambient floor** — bright enough to pass every existing gate. Only the real BRDF, which has no floor and
multiplies by `NoL`, renders it black. **A constant ambient term hides a broken normal**, and it very likely
explains the user's repeated "lighting looks wrong / the torus is in complete shadow" reports.

**Second, separate finding:** even with a correct normal the BRDF gives `lit` = 27 vs the toy shader's 40+. That
is **expected and correct** — `0.25 + 0.75·N·L` has an ambient floor the real BRDF does not, and there is no IBL
or tonemap yet. It is the reason §8 pairs 3.3 (BRDF) with 3.4 (HDR + AgX) as inseparable; the gate thresholds
were written against the toy and must be re-derived once exposure lands, not loosened to make the BRDF pass.


### 13.1 Narrowed: it is the VARYING TRANSPORT (2026-07-25)

A further probe settles where the zero comes from: make the VS emit a **constant** `vec3(0,1,0)` at `out[0]`
(location 0, `Smooth`) and read it in the cooked FS via `stage_in(vec3, 0, Smooth)`. **The FS still sees ~0.**

So the VS-side normal arithmetic is exonerated — and independently, the *position* transform uses the same `m[]`
instance-matrix loads and renders correctly, which clears `ibase` and the column-major assumption too.

**The bug is in VS→FS varying linkage.** Under `VK_EXT_shader_object` the stages are separable, so matching is by
**location**. Suspects, in order: a `vec3` varying at location 0 ahead of a `vec4` at 1; a naming/qualifier
mismatch between the VS output block and the FS input block (`ckir_glsl.hpp:1184` maps `StageIn` → `a_L` — confirm
what the VS side emits); and whether `smooth` is emitted on both sides.

⛔ **Next action is to DUMP THE EMITTED GLSL FOR BOTH STAGES AND READ IT.** Four successive attempts to reason
this out from the builder code produced three wrong conclusions. The generated text is the ground truth.

This is now the **highest-value open bug in the REN band**: it silently zeroes lighting everywhere, it is masked
by any constant ambient term, and REN-37.1 cannot complete until it is fixed.


### 13.2 GLSL dumped — the generated code is correct; the value does not arrive (2026-07-25)

Emitted both stages with `emit_stage_glsl` and read them:

- VS: `layout(location = 0) out vec3 o_0;` … and the body **writes** `o_0 = t293;`
- FS: `layout(location = 0) in vec3 a_0;` … `t27 = normalize(a_0)` feeding `dot(N,H)`, `dot(N,V)`, `dot(N,L)`
- locations 2 and 3 are correctly **DCE'd** from the FS (unused in this variant) — the variant machinery works

Declarations match, the VS writes, the FS consumes. **The defect is not in CKIR or the emitter.** Together with
the constant-VS probe, that leaves the **shader-object interface/linkage at runtime**.

**Next action (and it is a small, isolated one):** reproduce minimally in `tests/gpu-context-vulkan` — a VS with
one `vec3` varying, an FS that outputs it as colour, readback. If that reads zero, the bug is in the
`VK_EXT_shader_object` raster-program path and affects **every** raster program, which would make it the most
important bug in the engine, not merely in REN-37. If it reads correctly, the difference is the *extra* VS
outputs with no matching FS input (locations 2/3), which is the next thing to bisect.


### 13.3 Minimal repro: varyings transport fine — the difference is the DRAW PATH (2026-07-25)

Gate `[ren37]` in `test_vulkan_frame_graph.cpp`: a fullscreen VS writing a constant `vec3(0,1,0)` at location 0,
an FS that writes it straight out, readback — swept over 0..3 **unmatched** extra VS outputs:

```
extra_outs=0 -> r=0 g=255    extra_outs=2 -> r=0 g=255
extra_outs=1 -> r=0 g=255    extra_outs=3 -> r=0 g=255
```

**Varyings transport correctly, and unconsumed extra VS outputs do not break linkage.** Both remaining suspects
are eliminated, and this is NOT an engine-wide raster bug. The probe stays as a permanent pin on the transport
layer.

**What is left is the draw path.** The probe uses `IRasterContext::draw`; the scene uses
`draw_storage_depth` / `_load` — vertex **pulling** from a storage buffer into a colour+depth target. That is now
the only unexamined difference, and the next experiment is to run the same constant-varying pair through
`draw_storage_depth`.


### 13.4 Draw path exonerated too — trace `t293` in the VS body next (2026-07-25)

Second probe in the `[ren37]` gate: the same constant-varying pair through **`draw_storage_depth`** (vertex pull,
colour+depth target, 4 VS outputs — the scene's exact configuration) reads `r=0 g=255`.

**Every layer is now proven working:** emitter locations · VS arithmetic · varying transport under `draw` AND
`draw_storage_depth` · unmatched extra VS outputs · `ibase` and the column-major matrix layout (position renders
correctly from the same `m[]` loads). The scene FS still measures `n.y ≈ 0.07`.

⛔ **The one piece of evidence dumped but never read: the VS body.** The dump showed `o_0 = t293;` and `t293` was
never traced. It should resolve to `vec3(nwx, nwy, nwz)` built from `m[1]/m[5]/m[9]` and `nx/ny/nz` loaded at
`vbase+3,4,5`. With everything upstream proven, the defect is very likely visible in that dependency chain — the
wrong temp packed into the vec3, a shape/swizzle mismatch, or `ny` reading a different word.

**Read the emitted VS body. Do not reason about the builder.** This investigation has been six plausible theories
falsified by measurement; the remaining unread evidence is the cheapest thing left.


### 13.5 VS body traced — it is CORRECT, so the measurement is now the suspect (2026-07-25)

Followed `o_0` back through the emitted VS:

```
o_0  = vec3(t157, t162, t167)
t162 = m[1]*nx + m[5]*ny + m[9]*nz          <- nwy, exactly as intended
       matrix words 1/5/9 from ibase, normal words 3/4/5 from vbase   BOTH CORRECT
```

For the GEO-7 fixture (`from_trs(pos, identity, scale 1)`, vertex normal `(0,1,0)`) that is `m[5]*1 = 1`.

⛔ **So `o_0.y` must be 1 — contradicting the measured `n.y ≈ 0.07`.** With the emitter, VS body, transport (both
draw paths), extra outputs, `ibase` and the matrix layout all proven, the suspect is now **the measurement
itself**. One probe run in this investigation silently used a **stale binary** after a warning-as-error build
failure and still printed "All tests passed" — precisely the trap that yields a bogus reading.

**Next: re-measure `n.y` with the build VERIFIED** (confirm compilation succeeded and the binary actually
changed) before trusting it. If it reads ~1, the world normal was never broken, and the black cooked-Forward has
a different cause — the prime candidate then being `view_dir`, which is still a placeholder constant `(0,1,0)`
while `brdf_direct` derives `NoV` (and the whole specular/energy chain) from it.


### 13.6 ⛔⛔ RETRACTION — the "world normal reads zero" finding was a MISREAD METRIC (2026-07-25)

**Sections 13.1–13.5 above chased a bug that does not exist.** The GEO-7 gate's `lit` is a **COUNT of non-black
pixels out of 256 samples**, not a colour channel:

```cpp
if ((target->read_pixel(sx, sy) & 0x00FFFFFFU) != 0U) { ++lit; }
CHECK(lit > 40U);   // 256 samples
```

Every reading in that investigation — 0, 14, 17, 24, 27 — was a pixel count read as a normalized channel value.
**The world normal was never measured.** A direct probe (bypass the BRDF, write the raw varying out as colour)
**passes**: the normal arrives correctly.

Five theories were built and reported on that misreading, each "falsified" by a further misread measurement.
The rule, recorded in memory as a conduct entry:

> **Before interpreting any number a gate prints, READ THE ASSERTION THAT PRODUCED IT** — channel, count,
> percentage, delta or mean. A plausible integer in `0..255` proves nothing until you know what it counts.
> Print units in log lines (`lit_pixels=24/256`), and validate a shader probe against a control whose expected
> value you can state in advance.

**What survives from 13.1–13.5** (all independently useful, all proven by measurement):
- the emitter honours explicit `layout(location=)` (`ckir_glsl.hpp:1257`);
- the scene VS normal arithmetic is correct in the emitted GLSL (`nwy = m[1]*nx + m[5]*ny + m[9]*nz`);
- vec3 varyings transport under BOTH `draw` and `draw_storage_depth`, and unmatched extra VS outputs do not
  break linkage — now pinned by two permanent `[ren37]` gates;
- the cooked **Shadow** variant works and demonstrates the free DCE collapse.

**The one genuinely open issue** is narrower than advertised: the cooked **Forward** variant renders 0 non-black
pixels, while the same FS with a constant `(0,1,0)` normal renders 27. With the varying exonerated, the suspect
is `shade_forward`'s **`view_dir`, still a placeholder constant `(0,1,0)`** — `brdf_direct` derives `NoV`, the
Smith visibility term, `env_brdf_approx` and `energy_compensation` from it, and a degenerate view vector
plausibly zeroes the chain. The real fix is the frame-frequency camera uniform (REN-37.3), not a patch.


### 13.7 ⭐ ROOT CAUSE FOUND AND FIXED — the B7 const-folder ate `KOp::StorageLoad` (2026-07-26)

The suspicion in 13.6 was also wrong. **`build_fs_for_pass(..., do_lower = false)` renders; `do_lower = true` is
black** — every ingredient was correct and *lowering* zeroed the shader.

`optimize()`'s const-fold exclusion list covered `Input` / `Call` / resource DECLARATIONS / stage leaves /
aggregates / vectors — but **not `KOp::StorageLoad`**. `StorageLoad`'s ONLY operand is the **index**, so
`sbuf.data[22]` looked like a fully-constant expression and the folder replaced the **memory read** with a
literal. Every lowered shader reading a storage buffer at a fixed slot was miscompiled; the scene's cooked
forward variant reads its light direction at word 22 and rendered black. `BufferLoad`/`SharedLoad` escaped only
BY ACCIDENT (their first operand is a resource declaration, already unfoldable) — all three are now excluded so
that safety is intentional.

> ⛔⛔ **A MEMORY READ IS NEVER A COMPILE-TIME CONSTANT, however constant its INDEX is.**

Pinned by the `[kir][lower][b7]` gate. This mattered far beyond one shader: **D3's variant matrix and
content-hash dedup all run on the LOWERED graph**, so a lowering pass that can silently zero a graph makes every
cooked variant suspect.

**37.1 is therefore CLOSED:** the renderer now cooks its Forward *and* Shadow programs through
`build_fs_for_pass`, the shadowed FS is unified on the same surface + Cook-Torrance BRDF × visibility, and the
toy `build_scene_vs` / `build_scene_fs` are **deleted** — clang-tidy flagging them unused is the proof the cooked
path took over.


---

## 13.8 LANDED: 37.2 · 37.3 · 37.4 · 37.5 (2026-07-26)

### 37.2 — the technique is a NAME
`ckir_technique.hpp` makes the lighting half the same kind of thing a material is. **Two provenances, one
contract**: a registered `TechniqueBody` (engine/plugin C++) *or* an **authored** serialized CKIR graph spliced in
at cook time — no engine code, no recompile. `splice_graph` inlines a deserialized graph into the host FS graph,
substituting each `KOp::Input` leaf by ABI index, remapping struct ids and the variadic ext pool. It **rejects**
(never partially splices) a blob carrying kernel statements: a fragment technique is a value expression by
construction, and a silently-dropped statement would be a miscompile.

`crd-technique-cook` owns `.crdt` — parse · validate (12 named rejections) · lossless emit · canonical cook. It
carries the `crd-kir` edge deliberately so `crd-frame-cook` keeps depending on nothing but API-neutral GPU enums.

Built-ins: `standard_forward` (the port of the old fixed `shade_forward`), `unlit`, `forward_csm`.

### 37.3 — the binding contract, split in two on purpose
The technique declares its pass-frequency inputs by name and type. **Resource-class bindings (textures) must
appear in the pass's `reads`** — a frame graph's `reads` list is what lifetime analysis, barriers and aliasing are
derived from, so a sampled texture the pass never declares is a real dependency error, rejected by name at cook
time. ⭐ The check is by SHAPE too: a `texture2DArrayShadow` wired to a `layers == 1` resource renders every
cascade from slice 0 — a failure that *looks like art direction* — so it is `PassBindingNotLayered`, at cook time.

**Value bindings (`csm_light_vp`, `csm_map_size`) are engine state, not graph resources.** Listing them in `reads`
would put non-resources into the dependency graph, which the cooker would then reject as never-written. They are
checked one layer down: `resolve_scene_bindings` fails by name and `init_programs` returns false.

Also landed: the **frame-frequency camera position** (header words 96–98, from `camera_position_from_vp` — the
same exact reconstruction the cascade fit uses, so the two cannot disagree). `view_dir` was a placeholder constant
`(0,1,0)` until now, which degenerates `NoV` and with it the Smith term, `env_brdf_approx` and energy compensation.

### 37.4 — CSM is an authored technique, and the DELETION is the proof
`build_scene_fs_shadowed` (~120 lines of hand-written cascade select + PCF + bias) is **gone**, replaced by
`forward_csm` in `ckir_technique.hpp` + `assets/technique/forward_csm.crdt`, selected by `technique =
"forward_csm"` on the authored graph's forward pass. `build_scene_vs_textured` / `build_scene_fs_textured` are
**gone** too — the textured variant is the same cooked material with `textured = true` over the ONE shared VS.
**There is no hand-written fragment shader left in `scene_renderer.cpp`.**

Device gate: one scene, one camera, three techniques by name — `standard_forward` gives a lit gradient, `unlit`
draws the same geometry with materially different pixels, `forward_csm` darkens under a caster and never
brightens. ⛔ Plus the assertion that makes the other three mean anything: **an unresolved technique name FAILS**
`init_programs` rather than falling back, because a plausible frame rendered with the wrong technique is
indistinguishable from a correct one and a typo would otherwise read as a pass.

### 37.5 — persistent + ping-pong, both backends
`create_persistent_image(key, desc)` + `persistent_image_was_live(key)`, appended at the vtable END. Keyed by a
stable identity, excluded from transient aliasing and from the retire queue, contents and layout/state carried
across `reset()`. A desc change (a resize) recreates it and reports `was_live == false` — a differently-shaped
history is *worse* than none, because reprojection would read plausible-looking garbage. Ping-pong is two keys and
an executor-owned swap, deliberately not a new device concept.

#### ⭐ The design defect persistence exposed — and how it was removed rather than documented

Adding a persistent resource broke three things at once, and all three had **one cause**: `ImageNode::transient`
was a single bool standing in for **three independent questions**.

| the question | what it decides |
|---|---|
| is it **graph-owned**? | RTT barrier semantics, and whether the end-of-frame readback applies (a borrowed wrapper has **no readback buffer** to copy into) |
| is it **aliasable**? | the transient memory pool, the retire queue, the free path |
| is its state **frame-local**? | whether the frame-start reset applies |

For a transient all three answers are *yes*; for an import all three are *no*. One bool served perfectly — right
up until a resource that is graph-owned, **not** aliasable and **not** frame-local. Every site that read
`transient` meant exactly one of the three and silently got the other two wrong.

**The fix is structural, in both backends:**

```cpp
enum class Own : u8 { Imported, Transient, Persistent };
static bool graph_owned(const ImageNode& n) { return n.own != Own::Imported; }  // barriers, readback
static bool aliasable  (const ImageNode& n) { return n.own == Own::Transient; } // pool, retire, free
```

There is no `n.transient` any more, so a site cannot ask the wrong question by accident, and a fourth ownership
kind cannot silently inherit a previous kind's answers.

**The third question has no predicate on purpose — it is answered by where the state lives.** A persistent
image's live layout (Vulkan) / resource state (DX12) is stored in the **registry entry**, reached only through
`live_layout()` / `live_state()`. The frame-start reset writes the *node's* fields, which for a persistent node
are dead. So:

- the "skip persistent" exception in the reset **no longer exists** — it cannot reach that state;
- the end-of-frame **write-back no longer exists** — there is only ever one home, so nothing can fall out of sync;
- a transition from `UNDEFINED` (which the spec permits to **discard contents**) is unreachable by construction.

Two conditional exceptions and a synchronisation step were replaced by one accessor. What was a rule to remember
is now a property of the layout.

**Verified:** Vulkan 3621/210 · DX12 1180/118 · scene-render 467/14 · technique-cook 45/4 — all green.

### 37.6 — subgraphs + injection points, flattened away before `build()`
`flatten_frame_graph` (new `frame_compose.cpp`) inlines every `[[include]]` and splices every `[[inject]]`, then
runs the **same** validator a hand-authored graph faces. ⭐ Same decision REN-36.3 made for `for_each`, for the
same reason: composition that reached the scheduler would need a special case in lifetime analysis, barriers,
aliasing *and* the dependency sort. It reaches none of them.

- **`[[include]] graph / as / bind`** — `as` namespaces every resource, draw list, pass and anchor the subgraph
  declares. ⛔ That is load-bearing, not cosmetic: two instances of one subgraph would otherwise both declare
  `scratch` and both write it, which no validator flags — the picture just comes out *almost* right. Nesting
  compounds (`vp.main.bloom.*`), so two viewports each running a bloom chain stay disjoint all the way down.
- **`bind`** rewrites a name inside the subgraph to one in the includer's scope. `@`-prefixed names are
  **external sentinels**, not graph resources — `@output` always was, and a subgraph's `@input` is the same idea.
  A graph *with includes* is legitimately **partial**, so `NoOutputPass` / `ResourceNeverWritten` are deferred to
  the flattened result. That is what lets a subgraph be authored and shipped on its own.
- **`[[anchor]]` + `[[inject]]`** — anchors are **declared**, so the base-graph author states where extension is
  safe; that is the whole difference between an extension point and a monkey-patch. An unknown anchor or an
  unknown pass is a named rejection. The anchor decides where a pass is *inserted*; the dependency sort still
  decides where it *executes*.
- 7 new named rejections; blob v3; the editor round-trip stays byte-lossless with composition included.

⛔ **One finding worth keeping:** the first version of the gate's asset had the blur read and write `hdr`
in place — and the cycle detector rejected it. It was **right to**: an in-place ping-pong *is* a dependency
cycle, and resolving it is exactly what 37.5's persistent/ping-pong pair exists for. The composed asset became a
real chain (`hdr → outlined → bloomed → @output`).

**Verified:** frame-cook 196/10 · Vulkan frame-graph 255/19 · DX12 frame-graph 177/11 · scene-render 494/15.

### 37.7 — the variant matrix + the dual mode
`ckir_variant.hpp`. The matrix is **declared, not discovered**: every axis comes from something an author wrote
(a technique's `TechniqueOption` range, a material's `ShadingModel` tag, the passes a frame graph asks for), plus
Filament's `variantFilter` as a first-class **negative** declaration applied *before* cooking, so a filtered
combination costs nothing at all.

- **Two hashes, deliberately distinct.** `variant_key_hash` looks a program up; `graph_content_hash` (over the
  canonical, padding-free serialization of the **lowered** graph) decides what is the *same* program. Two
  different keys legitimately share content — that is dedup working — so conflating them would be a collision.
- **⭐ The free collapse, measured rather than asserted.** `dedup_ratio()`. A flat material and a texture-sampling
  one produce bit-identical `Shadow` programs while their `Forward` programs differ, and `DepthPrepass` collapses
  onto the same program — so it is one program for every opaque material across *both* depth-only passes, and
  swapping the technique cannot change it (a technique is never invoked there).
- **Shape vs branch options.** A tap count *unrolls* a different number of samples; no uniform expresses that. So
  `VariantAxis::shape` says which axes the editor übershader can genuinely cover and which it pins at their
  default and recooks. Stating it is the difference between a dual mode and a claim.

⛔ **One API defect found and removed, not documented:** `cook_variant_matrix` originally took `SurfaceInputs` and
binding node ids *by value* — but a CKIR node id indexes **one** graph, and the matrix cooks each variant into a
fresh one. Passing ids across graphs made the builders walk operands that meant something else (the first run
hung). The signature now takes a `VariantEnvFn` the matrix calls **with the graph it is about to fill**, so a
cross-graph id is not expressible.

**Verified:** kir `[variant]` 57/5.

### 37.8 + 37.9 — the frame-level graph and its scheduler
`viewport.hpp` / `viewport.cpp` in `crd-frame-cook`, plus the renderer split.

- **`SceneRenderer::contribute(fg, …)`** records without owning. `render()` is now exactly `contribute()` plus
  reset/build/execute, so the single- and multi-viewport paths cannot drift. **Gated on device: two viewports of
  different sizes, from different cameras, both rendered, `last_submit_count() == 1`.**
- **`compose_frame`** assembles one `FrameGraphDesc` whose `[[include]]`s are the active viewports — each
  namespaced by its id, each bound to its own output, with at most one `present`. A `shared` graph is included
  once, outside any viewport, so two viewports reading the shadow atlas order behind a single producer for free.
- **`select_viewports`** admits `EveryFrame` viewports first and unconditionally, then fills the remaining budget
  (measured GPU ms, fed by REN-8) by **priority + age**. ⭐ The ageing term is what prevents starvation. Deferred
  viewports stay dirty and are reported. A settled thumbnail grid costs **zero** viewport work.
- **Invalidation is declared, not hand-set** (`depends_on(Asset|Material|Camera|Resize)`), because the failure
  mode of a hand-set flag is a *stale thumbnail*, which looks exactly like a correct one.

⛔ **The split exposed a genuine dangling-pointer bug, and the fix is structural.** `add_pass(…).execute(fn, user)`
stores the user pointer and dereferences it at `execute()` — which only worked because `render()` executed in the
same call, keeping its **locals** alive. Separate recording from execution and every such local dangles (it
segfaulted immediately). The per-contribution draw list and state now live in a **contribution arena reserved to
its exact capacity** in the constructor, so no `push_back` can reallocate and move an entry the graph already
points at — the same discipline REN-36.3's expansion table uses. The cap is **checked**: exceeding it reports
zero draws rather than corrupting memory.

**Verified:** scene-render 511/16 · frame-cook 267/13 · Vulkan 3621/210 · DX12 1180/118 · technique-cook 45/4 ·
kir 52,881/257.

### 37.10 — the integration: the renderer HOSTS a graph instead of building passes

⛔ Until this, REN-37 had built the whole stack and proved every layer on device while `scene_renderer.cpp` still
hand-built its cascade and scene passes in C++, carrying its own violation notice. That is the **same defect 37.1
was created to fix** — machinery referenced only by tests. Leaving a second instance of it would have been the
worst possible outcome for this slice.

**The ~50-line C++ pass block is deleted.** The identical frame is `assets/frame/forward_csm.frame.toml`, recorded
through `FrameRecorder`, with the renderer supplying only what a graph *cannot* know: the target, the resolved
draw lists, the cascade count, the per-cascade programs.

| | |
|---|---|
| **`FrameRecorder`** | `execute_frame_graph` had the same ownership defect one layer down — it created its own graph per call. It is now `record()` + create/build/execute, so a host can record N authored graphs into one graph. Its arena is reserved to exact capacity, because `execute(fn, user)` stores pointers dereferenced long after `record()` returns. |
| **⭐ REN-36.3-b closed** | The asset's `all`/`any`/`none` filters are *evaluated*, via `World::component_id_by_name` (trailing-identifier match over the decorated `typeid` name) + a new runtime `has_component_id`. ⛔ An unknown name matches **nothing** — "I could not resolve this filter" must never silently mean "this filter passes". Per group, not per instance. |
| **⭐ Shadows on/off is a capability tier** | `requires = ["shadows"]` + `fallback = "crd://frame/forward_basic"` (new asset: no atlas, no cascade passes, `standard_forward`). Off costs *nothing* rather than running the machinery and multiplying by one — and the step-down is reported. |
| **37.7 in the renderer** | Every FS goes through `cook_fs`: cook → lower → content-hash → reuse. The free collapse becomes work saved rather than a claim. FS members are borrowed from the cache that owns them. |
| **Per-draw textures** | `DrawItem::texture` beats the pass's sampled read — removing the interim state where a textured group lost its albedo the instant shadows turned on. |

**The whole loop, gated:** `ViewportRegistry` → `select_viewports` → N × `contribute()` → one build, **one
submission**. Three viewports in one submit, all three targets verified rendered; the settled frame collapses to
the live view alone; invalidating one asset brings back exactly its thumbnail.

**Verified:** scene-render **537/17** · frame-cook 267/13 · Vulkan frame-graph 241/17 · DX12 frame-graph 177/11.

> ⭐ **The close test the top rule always asked for now passes:** cascade count, atlas size and format, pass order,
> what casts, what receives, and *which technique shades it* are all asset text. None of them need a rebuild.

---

## 14. REN-37.8 — THE FRAME-LEVEL GRAPH: many viewports, ONE submission

> **Design, 2026-07-26.** Written in answer to the user's question: *"how different viewports will be rendered
> inside the main loop, because for example in editor tool, we might need mesh renderers to see meshes inside the
> file browser or other windows, maybe animation players, vfx viewers in browsers, main viewport and so on."*

### 14.1 The defect this fixes, stated plainly

`SceneRenderer::render(target, ...)` **creates and executes its own frame graph per call**. That shape is fine
for a game with one view and structurally wrong for anything else:

| consequence | why it is a real cost |
|---|---|
| **N submissions per frame** | each `execute()` is a `vkQueueSubmit` / `ExecuteCommandLists` + its own fence slot. An editor with a main viewport, an animation preview and 12 dirty thumbnails submits **14 times** to render one frame. |
| **No cross-viewport aliasing** | every graph allocates its own transients. The thumbnail's depth buffer cannot share memory with the main viewport's bloom scratch, even though their lifetimes are disjoint. Peak VRAM is the SUM of every viewport's working set instead of the MAX. |
| **No cross-viewport ordering** | a viewport that consumes another's output (a picking buffer, a shared reflection probe, a shadow atlas reused by two views) has no way to say so — the dependency sort only ever sees one viewport at a time. |
| **Per-graph frames-in-flight** | the `kFramesInFlight = 2` ring is per `IFrameGraph`. N graphs means N rings, N query pools, N retire queues. |
| **Shared work is duplicated** | two viewports lighting the same scene each rebuild the shadow atlas. Nothing can notice they are the same pass. |

### 14.2 The model

> **ONE frame graph per FRAME. A viewport is a SUBGRAPH INSTANCE inside it.**

```
                          ┌──────────── the FRAME (one IFrameGraph, one submission) ────────────┐
  ViewportRegistry ──►    │  [shared]     shadow_atlas                                          │
   main      (EveryFrame) │  [vp.main]    include forward_csm  bind output=@swapchain           │
   anim_prev (OnDemand)   │  [vp.anim]    include forward_lite bind output=vp.anim.rt           │
   thumb_17  (OnDemand)   │  [vp.thumb17] include thumbnail    bind output=vp.thumb17.rt        │
   vfx_3     (EveryFrame) │  [vp.vfx3]    include vfx_preview  bind output=vp.vfx3.rt           │
                          │  [present]    @swapchain                                            │
                          └─────────────────────────────────────────────────────────────────────┘
```

Every mechanism this needs **already exists or is already scheduled**:

| need | mechanism | status |
|---|---|---|
| a viewport is a reusable graph | **subgraph `include` + parameter binding** | REN-37.6 |
| instances must not collide | `include ... as = "vp.main"` namespaces resources and passes | REN-37.6 |
| N instances from one declaration | `for_each` expansion at build time | ✅ REN-36.3-a |
| its own camera / draw scope | `bind = { camera = …, draws = … }` — the host resolves per instance | ✅ `IFrameGraphHost` |
| cached thumbnails must survive | `persistent_image`, excluded from aliasing | REN-37.5 |
| ordering across viewports | the existing Kahn topological sort — it never knew about viewports | ✅ REN-1 |

**So 37.8 adds no new graph machinery.** It adds the *composer* that assembles the per-frame `FrameGraphDesc`
from a viewport registry, and it moves `SceneRenderer` from *"owns a graph"* to *"contributes to one"*.

### 14.3 The viewport record

```cpp
struct ViewportDesc
{
    crd::containers::String  id;          // "main", "thumb.17", "anim.preview" — the include namespace
    crd::containers::String  graph;       // "crd://frame/forward_csm" | ".../thumbnail" | ".../vfx_preview"
    crd::gpu::IRasterTarget* target;      // where it renders (swapchain view, or a persistent RT)
    ViewportView             view;        // camera transform + projection + jitter (frame-frequency uniform, 37.3)
    crd::containers::String  draw_scope;  // an ECS-query PREFIX the viewport's draw lists intersect with
    ViewportPolicy           policy;      // EveryFrame | OnDemand | Periodic     (§15)
    crd::u32                 priority;    // scheduling priority                  (§15)
    bool                     present;     // true for the one viewport that owns the swapchain
    bool                     readback;    // thumbnail CAPTURE wants the CPU copy; a live viewport must not pay for it
};
```

Two fields carry the whole "different viewports render different things" story:

- **`graph`** — a thumbnail is not the main renderer with features switched off; it is a **different authored
  asset** (`thumbnail.frame.toml`: one forward pass, no shadows, no post). Cost tiering is *authored*, never an
  `if (is_thumbnail)` in C++. This is the top rule applied to viewports.
- **`draw_scope`** — the main viewport draws the world; a thumbnail draws ONE asset; a VFX viewer draws one
  emitter. The scope intersects with the graph's own `all`/`any`/`none` query, so the asset still declares *what
  kind* of thing it draws and the viewport declares *which subset*.

### 14.4 The composer, in the main loop

```cpp
FrameComposer composer(alloc);
for (;;)
{
    scheduler.select(registry, budget, /*out*/ active);   // §15 — WHICH viewports run this frame
    composer.begin(frame_index);
    for (const ViewportDesc& vp : active) { composer.add_viewport(vp); }  // include + bind, namespaced
    composer.add_shared(shared_graph);                    // shadow atlas / sky / IBL, produced ONCE
    const FrameGraphDesc& frame = composer.build();       // ONE description
    execute_frame_graph_with_fallback(frame, raster, host, alloc);   // ONE build, ONE submission
}
```

`composer.build()` is a pure `FrameGraphDesc` assembly — it runs the **same 19 cook-time rejections** every
authored graph gets, so a viewport that names a missing resource fails **by name**, not with a black panel.

### 14.5 ⭐ The property that makes this worth doing

Because every viewport lands in ONE graph, the **existing** lifetime analysis sees the whole frame at once:

- 12 thumbnails at 256² each need **one** 256² depth buffer of physical memory, not 12 — their lifetimes are
  disjoint and the aliasing allocator already knows how to prove that.
- The shadow atlas is declared **once, outside any viewport**, and both the main viewport and the animation
  preview *read* it. The topological sort orders the producer first automatically; neither viewport's asset
  mentions the other.
- Timestamps (REN-8) become **per-viewport** for free — each include is a named group of passes, so
  `pass_gpu_ms` already reports "main.forward 3.1 ms / thumb.17.forward 0.2 ms" with no new instrumentation.

### 14.6 What must NOT happen (the failure this design refuses)

- ⛔ **No `render_thumbnail()` / `render_preview()` entry points.** The moment a viewport kind gets its own C++
  function, the top rule is dead again and the thumbnail path drifts from the main path — which is exactly how
  engines end up with an editor look that differs from the shipped one (§5's warning, one level up).
- ⛔ **No implicit viewport.** `SceneRenderer` must not keep a "default" graph it executes when nobody asked. If
  the registry is empty, the frame is empty.
- ⛔ **No cross-viewport global state.** Everything a viewport needs arrives through its `bind` — that is what
  makes two instances of the same graph safe.

---

## 15. REN-37.9 — ON-DEMAND + BUDGETED VIEWPORT SCHEDULING

Composition alone is not enough. A file browser scrolled to a folder of 400 assets must not render 400 viewports,
and must not stall for one frame while it renders them either. **Scheduling is the half that makes §14 usable.**

### 15.1 Policies

```cpp
enum class ViewportPolicy : crd::u8
{
    EveryFrame = 0, // the main viewport, a playing animation preview, a live VFX viewer
    OnDemand,       // renders when DIRTY, then holds its last result — thumbnails, static previews
    Periodic,       // renders at most every N frames — a background bake preview, a far-off reflection probe
};
```

`OnDemand` is the one that carries the editor. A thumbnail renders **once**, its result lives in a
`persistent_image` (37.5 — excluded from aliasing precisely *because* its value is its history), and the steady
state of a settled browser is **zero passes**. It re-renders only when something it depends on changes.

### 15.2 Invalidation — the correctness half

A dirty flag that is set by hand is a bug generator. The viewport declares **what it depends on**, and the
registry invalidates it:

```cpp
registry.invalidate_on(vp_id, DependencyKind::Asset,    mesh_guid);
registry.invalidate_on(vp_id, DependencyKind::Material, material_guid);
registry.invalidate_on(vp_id, DependencyKind::Camera);      // its own view moved
registry.invalidate_on(vp_id, DependencyKind::Resize);      // its target changed size
```

⛔ **A missing invalidation must be visible, not silent.** In debug builds the registry can re-render an
`OnDemand` viewport every Nth frame and compare a hash of the result against the cached one; a mismatch means a
dependency was never declared. That is the same doctrine as `UnresolvedForEach` — a stale thumbnail is
indistinguishable from a correct one by eye, so the *check* has to be mechanical.

### 15.3 The budget

```cpp
struct ViewportBudget
{
    crd::u32 max_viewports   = 8;        // hard cap on instances composed into one frame
    crd::u64 max_pixels      = 8u << 20; // total shaded pixels across on-demand viewports
    crd::u32 max_draw_items  = 4096;     // total resolved draws
    double   max_gpu_ms      = 4.0;      // measured (REN-8 timestamps) — the previous frame's cost feeds back
};
```

Selection each frame:

1. **Every `EveryFrame` viewport is admitted first** and its cost is charged. The main viewport is never starved
   by thumbnails — that is non-negotiable and is why it is not merely "priority 0".
2. Remaining budget goes to dirty `OnDemand` / due `Periodic` viewports, ordered by **priority, then age**.
3. ⭐ **Ageing is what prevents starvation.** Ordering by priority alone lets a stream of high-priority
   invalidations keep one thumbnail permanently unrendered. Each skipped frame raises effective priority, so
   every dirty viewport renders in bounded time.
4. Whatever does not fit stays dirty and is retried next frame. **`log()` what was deferred** — a silent cap
   reads as "everything rendered" (the no-silent-caps rule).

`max_gpu_ms` closes the loop with REN-8: the scheduler charges each viewport its **last measured** GPU cost, so
the budget is in real milliseconds rather than a guessed pixel count. A viewport with no measurement yet is
charged a pessimistic estimate so a first-time expensive viewport cannot blow the frame.

### 15.4 Amortization: a viewport may also render *across* frames

A 2048² reflection probe or an expensive VFX preview does not have to complete in one frame. Because a viewport
is a graph instance with a **persistent** target, it can render a *slice* per frame (one cube face, one tile) and
publish when complete. That is `for_each` + `Periodic` + a persistent target — again, no new machinery, and it
is the same shape point-light shadow atlases and DDGI probe updates need later.

### 15.5 Readback is a per-viewport property, not a global one

`set_readback_enabled` is currently a *renderer-wide* switch, and REN-8 already showed the hazard: with readback
off, a layout transition that the present path depends on was quietly dropped. Per viewport:

- **thumbnail CAPTURE** (writing a `.png` to the asset cache) → `readback = true`, pays the stall, once;
- **live viewport** → `readback = false`, never stalls;
- the transition is emitted **either way** — the readback flag may only remove the *wait*, never a barrier. That
  scar is now a rule.

### 15.6 The gate

An editor-shaped test: 1 main viewport (`EveryFrame`) + 16 thumbnails (`OnDemand`) + 1 animation preview, with a
budget admitting 4 on-demand viewports per frame.

- frame 0: main + 4 thumbnails render; 12 remain dirty and are **reported** as deferred;
- frames 1–3: the remaining 12 render, oldest-first, none skipped twice in a row;
- frame 4+: **only** the main viewport and the animation preview record passes — `pass_count()` proves the
  thumbnails contribute nothing once settled;
- invalidating one thumbnail's material re-renders **exactly that one**;
- ⭐ the whole thing is **ONE submission per frame** throughout, and peak transient memory is bounded by the
  budget, not by the viewport count.
