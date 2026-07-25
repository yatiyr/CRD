# REN-36 — THE AUTHORABLE FRAME GRAPH: render passes, pipelines and whole rendering architectures as ASSETS

**Status**: spec v1, 2026-07-25. **Priority: user-declared MUST, ASAP.**

> ⭐ **USER DIRECTION (2026-07-25), verbatim — this is the contract:**
> *"every kind of shader, pipeline, render pass and all that sorts of things should be an asset! and we must make
> them completely authorable by everyone! it is the MUST!"*
> *"lightings, renderpasses, the graph that does a thing on a framebuffer and passes it to another pass and then
> produces stuff must be fully authorable and flexible."*
> *"all the users (us for tools like editors and all the others making apps with this tool, maybe using editor to
> make a game, authoring new rendering styles and types like deferred rendering forward+ rendering or other
> innovative stuff) should be able to create their own render passes top to bottom easily or modify the existing
> pipelines easily."*
> *"AND IT MUST BE API AGNOSTIC SO IT DOES NOT MATTER WE ARE USING VULKAN OR DIRECTX."*

**Three design questions answered by the user, locked:**
1. **Draw lists** — the graph declares its own **filters**, not just names of engine-provided views. *(the more flexible option)*
2. **Multi-view** — a pass declares `for_each` (cascades, eyes, faces, probes). *(the more flexible option)*
3. **Defaults** — shipped as cooked assets in a **built-in pack that mounts first**, so any app overrides by name.

---

## 1. The principle: what is data, what is code

The whole design rests on one split. Get it right and there is no scripting language; get it wrong and we
reinvent one (which ADR-0081 forbids).

| | authored as | who changes it |
|---|---|---|
| What a pass computes **per pixel / per thread** | **CKIR** shader / material / kernel asset | anyone — works today ✅ |
| The **graph**: which passes exist, what they read/write, resource formats, order | **the frame-graph asset** (this slice) | anyone |
| The **mechanic**: "bind these attachments and iterate this list" | C++ `PassKind` | engine only; short list, grows slowly |

**Consequence, and it is the point of the slice:** *inventing a rendering technique needs ZERO engine code.*
A new lighting model, a toon outline, a hidden-line CAD renderer, deferred vs Forward+ — all are
(CKIR shader) + (graph node). Engine work is needed only for a genuinely new **mechanic** (e.g. "dispatch mesh
shaders indirectly from a GPU-computed count"), which becomes a new `PassKind`.

## 2. ⛔ THE API-AGNOSTICISM CONTRACT — and how it is GATED

The asset references **engine concepts only**. It must be impossible to express a backend concept in it:
`FgImageFormat::D32Float`, never `VK_FORMAT_D32_SFLOAT` or `DXGI_FORMAT_D32_FLOAT`; `DepthCompare::LessEqual`,
never `VK_COMPARE_OP_LESS_OR_EQUAL`. This is already how `frame_graph.hpp` and `IRasterContext` are written —
the asset inherits that neutrality rather than inventing it.

**The gate — the load-bearing acceptance criterion of this slice:**
> **ONE cooked frame-graph asset, loaded unmodified on Vulkan AND DX12, produces bit-identical readback.**

Not "works on both" — *the same bytes*. We already prove this shape for REN-1/2/3 with mirrored per-backend
gates; here it becomes a single asset checked twice. Any backend-specific escape hatch in the format is a
**design failure**, not a feature: if a graph needs different content per backend, that is a **capability tier**
(§8), declared and validated, never an `#if VULKAN`.

Corollary: a cooked graph authored on a Windows/DX12 machine must load and run identically on a Linux/Vulkan
machine. The cooked form is therefore endian-defined and ABI-independent — the same discipline the KIR
serializer just learned the hard way (canonical, padding-free records; see `ckir_serialize.hpp`).

## 3. The authoring format (TOML — "authoring text, runtime binary")

### The engine default, shipped in the built-in pack

`crd://frames/forward_shadowed.frame.toml`

```toml
schema = 1
name   = "forward_shadowed"

# ── resources the graph owns. Transients are aliased automatically; no manual lifetimes. ──
[[resource]]
name = "shadow_atlas"; kind = "transient_image"
format = "D32Float"; width = 2048; height = 2048; layers = 4; sampled = true

[[resource]]
name = "hdr"; kind = "transient_image"
format = "RGBA16F"; scale = 1.0; sampled = true       # scale = relative to @output

# ── draw lists: ECS QUERIES, declared by the graph (user answer #1) ──
[[draw_list]]
name  = "shadow_casters"
all   = ["MeshRenderer", "Transform"]
none  = ["NoShadowCast"]
cull  = "frustum"
sort  = "front_to_back"

[[draw_list]]
name = "visible_opaque"
all  = ["MeshRenderer", "Transform"]
none = ["Transparent"]
cull = "frustum"
sort = "material"                                      # minimise state changes

# ── passes ──
[[pass]]
name          = "shadow"
kind          = "raster.depth_only"
for_each      = "light.0.cascades"                     # user answer #2 — 4 instances, one per cascade
writes        = ["shadow_atlas[$index]"]               # each instance writes its own array slice
draw_list     = "shadow_casters"
view          = "light.0.cascade[$index]"
clear_depth   = 1.0
depth         = "LessEqual"
material_pass = "Shadow"                               # the PassType that ALREADY exists in ckir_cook.hpp

[[pass]]
name          = "forward"
kind          = "raster.geometry"
reads         = ["shadow_atlas"]
writes        = ["hdr"]
draw_list     = "visible_opaque"
view          = "camera.main"
material_pass = "Forward"
clear_color   = [0.0, 0.0, 0.0, 1.0]

[[pass]]
name   = "tonemap"
kind   = "raster.fullscreen"
reads  = ["hdr"]
writes = ["@output"]
shader = "crd://shaders/post/agx_tonemap"
[pass.params]
exposure_ev100 = 13.5
```

**Nobody wrote a barrier, an ordering, or an aliasing decision.** Those stay derived from `reads`/`writes`
exactly as they are today — the asset replaces the `add_pass()` *calls*, never the machinery.

### A game adds a toon outline — zero engine code

```toml
[[pass]]
name   = "toon_outline"
kind   = "raster.fullscreen"
reads  = ["depth", "gbuffer_normal"]
writes = ["hdr"]
shader = "myGame://shaders/toon_outline"      # their own CKIR graph
blend  = "alpha"
[pass.params]
thickness_px = 2.0
edge_color   = [0.0, 0.0, 0.0, 1.0]
```

Ordering, the barrier, and the read-after-write hazard are all derived. The engine is not rebuilt.

### A CAD tool invents a different renderer entirely

No shadows; hidden-line + SSAO + a picking ID buffer. Same engine binary:

```toml
[[pass]] name="depth_prepass"; kind="raster.depth_only"; writes=["depth"]
         draw_list="visible"; material_pass="DepthPrepass"
[[pass]] name="ids";  kind="raster.geometry"; writes=["ids"]
         draw_list="visible"; shader="cad://shaders/entity_id"
[[pass]] name="ssao"; kind="compute"; reads=["depth"]; writes=["ao"]
         kernel="crd://kernels/ssao"; dispatch="output/8"
[[pass]] name="hidden_line"; kind="raster.fullscreen"; reads=["depth","ao"]
         writes=["@output"]; shader="cad://shaders/hidden_line"
```

**This is the case that proves the design.** A CAD tool is not a game with the lights off.

### Deferred, and Forward+ — architectures, not special cases

Deferred is a graph whose geometry pass uses `material_pass = "GBuffer"` (already exists) plus a fullscreen
lighting pass. Forward+ is a `compute` light-culling pass writing a cluster buffer that the forward pass reads.
**Both are graph edits.** That is exactly the user's "deferred rendering forward+ rendering or other innovative
stuff" requirement.

## 4. `PassKind` — the C++/data boundary, stated explicitly

| kind | mechanic |
|---|---|
| `raster.geometry` | iterate a draw list into colour(+depth) attachments |
| `raster.depth_only` | iterate a draw list into depth only *(REN-3.1 — built)* |
| `raster.fullscreen` | one triangle over the target, sampling declared inputs |
| `raster.mrt` | geometry into N colour attachments (G-buffer) |
| `compute` | dispatch a CKIR kernel over a declared grid |
| `present` | hand the target to the swapchain |

Later, as their device features land: `raster.mesh`, `raster.mesh_indirect` (REN-4), `raster.cube_face`
(REN-3.5's env prefilter), `compute.indirect` (C5). **Adding a kind is a deliberate engine change with its own
gate — never an escape hatch to describe arbitrary logic in data.**

## 5. Draw lists = ECS queries *(user answer #1)*

A draw list is a declared query over `crd-scene`'s archetype storage plus cull/sort policy:

```toml
[[draw_list]]
name = "hair"; all = ["HairRenderer","Transform"]; any = []; none = ["Hidden"]
cull = "frustum"; sort = "back_to_front"; limit = 0
```

Reuses the ECS's existing archetype matching — no new query engine. `cull` ∈ `none|frustum|frustum+occlusion`
(the last arriving with REN-4's HiZ). `sort` ∈ `none|front_to_back|back_to_front|material`.

## 6. `for_each` — multi-view *(user answer #2)*

One pass declaration, N instantiations, each with its own view and its own resource slice:

| generator | instances | used by |
|---|---|---|
| `light.N.cascades` | one per CSM cascade | REN-3.2 |
| `views.stereo` | left/right eye | VR |
| `cube.faces` | 6 faces | env prefilter (REN-3.5), point-light shadows |
| `lights.shadow_casting` | one per shadow-casting light | many-light shadows |

`$index` substitutes into resource subscripts (`shadow_atlas[$index]`) and view names. The graph builder expands
`for_each` **at build time** into real passes, so lifetime analysis, barriers and aliasing see ordinary passes
and need no special cases.

## 7. Defaults as a built-in pack *(user answer #3)*

The engine ships cooked graphs (`forward_shadowed`, `deferred`, `unlit`, `editor_viewport`, `cad_hidden_line`)
in a **built-in pack mounted FIRST**. An application overrides purely by shadowing the name from a later mount —
the existing resource-mount precedence, no new mechanism. Every default is therefore also a worked example, and
"modify the existing pipeline easily" means: copy the TOML, edit, ship in your own pack.

## 8. Capability tiering (the API-agnosticism escape valve, done honestly)

```toml
requires = ["mesh_shaders", "bindless"]
fallback = "crd://frames/forward_shadowed_basic"
```

Cook validates the requirement names; the loader picks the highest tier the device supports. A WebGPU/mobile
build degrades **by declared tier**, never by silent breakage (REN-35's rule). This is the *only* sanctioned way
content differs per platform — and it differs by **capability**, never by graphics API.

## 9. Cook-time validation (errors belong at cook, not on a player's machine)

The cooker rejects, by name: dependency **cycles** · a resource **no pass writes** · a read of an undeclared
resource · unknown `kind`/`format`/`compare`/`sort` · a `shader`/`kernel`/`material_pass` that does not resolve ·
`for_each` over an unknown generator · a subscript on a non-layered resource · a `requires` naming an unknown
capability · a `fallback` that does not exist. `build()`'s existing runtime checks stay as the last line of
defence, but a shipped graph must already be proven well-formed.

## 10. Increments (each gated, both backends)

- **36.1 — the schema + cooker.** `.frame.toml` → cooked CRDR (canonical, padding-free, ABI-independent).
  Gate: every rejection in §9 fires with its own message; a valid graph round-trips byte-identically.
- **36.2 — the runtime loader + executor.** Cooked graph → `IFrameGraph` calls. Gate: **the API-agnosticism
  gate of §2** — the hand-built REN-3.1 two-pass shadow graph, re-expressed as an asset, produces readback
  **bit-identical to the C++ version, on BOTH backends.**
- **36.3 — draw lists + `for_each`.** ECS-query draw lists and multi-view expansion. Gate: a 4-cascade shadow
  graph from ONE pass declaration matches four hand-written passes exactly.
- **36.4 — the built-in pack + override.** Ship the defaults; prove an app overrides one by name.
- **36.5 — hot-reload + the editor seam.** Edit TOML → cook → swap live (D5's proven mechanism); pass params
  exposed through `crd-reflect` so the inspector and the REN-23 node editor get them for free.

## 11. Risks

1. **`params` is where a DSL sneaks in.** Params are typed scalars/vectors/resource references — **never
   expressions**. If a graph needs arithmetic, that arithmetic belongs in CKIR.
2. **`for_each` × aliasing.** Expansion must happen before lifetime analysis or the aliaser will see phantom
   lifetimes. Expand at build, keep the existing aliasing gates green.
3. **Draw-list queries becoming a second ECS API.** Reuse `crd-scene`'s archetype matching verbatim; if a query
   cannot be expressed, extend the ECS, do not fork it.
4. **The bit-identical gate is strict on purpose** and will catch real backend divergence (REN-3.1's DX12 depth
   asymmetry is exactly the class of thing it exists to surface). Expect it to fail first and teach us something.

## 12. Relationship to the rest of the band

This does **not** replace REN-3.2–3.7 — those build the *passes*. REN-36 makes the *composition* authorable.
Every pass REN-3 adds becomes available to authors the moment its `PassKind` exists. Scheduling: the user has
declared this a MUST/ASAP, so it runs **as a peer of REN-3.2 onward**, not after the band.
