# Cerid — draw substrate research

**Date:** 2026-05-10
**Locks:** ADR-0066 (`crd-draw` substrate architecture).
**Phase plan:** `docs/phases/phase-3.1-eylem.md` v1a-draw section.

> Source-of-truth document for the *why* behind every algorithm /
> data-structure / consumer choice in `crd-draw`. ADR-0066 cites
> this file. v1a-draw ships the substrate against this design.
> The eylem v1c+ broadphase/solver work uses it from day 1.

## 1. Why we need a draw substrate (per consumer)

| Consumer | Use case | Without `crd-draw` |
|---|---|---|
| Eylem v1c+ rigid 3D | Bodies + AABBs + contact normals + impulse arrows + island colors + joint frames + ragdoll articulation | debug with printf + faith |
| Eylem v3 XPBD soft/cloth | Particle positions + constraint links + tetrahedral mesh + collision pairs | re-write per-feature ad hoc |
| `crd-sdf` (Phase 3.1.5) | Narrow-band cells + isosurface samples + CSG tree + closest-point overlays | re-write per-feature ad hoc |
| `crd-audio` (Phase 3.4) | Acoustic ray probes + occlusion volumes + sound source bounds | no path at all |
| Navmesh (Phase 7) | Navmesh polygons + portal edges + agent paths + RVO velocity obstacles | no path at all |
| Editor (Phase 7) | Selection outlines + manipulator gizmos + per-tool overlays + brush strokes | no path at all |
| Renderer (Phase 3.5+) | Light bounds + frustum + clusters + BVH + GI probe placement + shadow cascades | re-write per-feature |
| Sandbox (already shipped) | Per-entity inspection + transform handles + asset preview gizmos | bounded by ImGui flat overlay |

Every one of these is a real consumer in the locked roadmap. Embedding
debug rendering inside any single one of them (e.g. eylem) duplicates
the work N-1 times. The substrate has to be a peer module, sized for
N consumers from day one.

## 2. Industry landscape

### 2.1 Physics SDK debug interfaces — the canonical pattern

Every major physics SDK separates the "physics emits debug data" half
from the "renderer paints pixels" half. Three flavors:

#### Bullet — `btIDebugDraw` (callback / immediate-mode)

Pure-virtual interface ([source](https://github.com/bulletphysics/bullet3/blob/master/src/LinearMath/btIDebugDraw.h)):
- `drawLine(from, to, color)` is the only required primitive
- `drawContactPoint`, `drawArc`, `drawSphere`, `drawCapsule`, `drawTransform`,
  `draw3dText`, `reportErrorWarning` — defaults built on `drawLine`
- `setDebugMode(int)` is a 16-flag bitmask covering wireframe, AABBs,
  contact points, constraint frames, and engine-state toggles
  (no-deactivation, no-help-text, profile-timings, CCD enable)

Consumer accumulates in their own buffer and issues actual draws at
end-of-frame. Zero coupling, trivial port.

**What to learn:** the bitmask conflates *visualization config* with
*engine debug behavior*. Don't carry that mistake forward — keep them
orthogonal in Cerid.

**What to avoid:** `draw3dText(location, text)` with no font-size,
anchor, alignment hints. Every Bullet host ends up with inconsistent
text. Specify size + anchor + alignment up front.

#### NVIDIA PhysX — `PxRenderBuffer` + `PxVisualizationParameter` (retained / pull)

Inverts Bullet ([docs](https://nvidia-omniverse.github.io/PhysX/physx/5.4.1/docs/DebugVisualization.html)
and [PxRenderBuffer.h](https://docs.nvidia.com/gameworks/content/gameworkslibrary/physx/apireference/files/PxRenderBuffer_8h-source.html)):
the SDK fills an internal buffer during simulation, host *pulls* it
after `simulate()` finishes.

```cpp
struct PxDebugPoint    { PxVec3 pos; PxU32 color; };
struct PxDebugLine     { PxVec3 pos0; PxU32 color0; PxVec3 pos1; PxU32 color1; };
struct PxDebugTriangle { PxVec3 pos0; PxU32 color0; PxVec3 pos1; PxU32 color1;
                         PxVec3 pos2; PxU32 color2; };
struct PxDebugText     { PxVec3 position; PxReal size; PxU32 color; const char* string; };

class PxRenderBuffer {
    virtual PxU32                 getNbLines()     const = 0;
    virtual const PxDebugLine*    getLines()       const = 0;
    virtual void                  reserveLines(PxU32 n);
    virtual void                  addLine(...);
    /* points, triangles, text symmetric */
    virtual void                  append(const PxRenderBuffer&);
    virtual void                  clear();
    virtual void                  shift(const PxVec3&);
};
```

`PxVisualizationParameter` is ~30 named flags, each carrying a
**float scale** (not a bool): `eSCALE` (master switch), `eWORLD_AXES`,
`eBODY_AXES`, `eBODY_LIN_VELOCITY`, `eCONTACT_POINT`, `eCONTACT_NORMAL`,
`eCONTACT_FORCE`, `eCOLLISION_AABBS`, `eCOLLISION_SHAPES`,
`eJOINT_LOCAL_FRAMES`, `eJOINT_LIMITS`, `eMBP_REGIONS`, `eSDF`, etc.
Per-actor opt-in via `PxActorFlag::eVISUALIZATION` and
`PxShapeFlag::eVISUALIZATION` — three-way AND with master scale and
parameter scale.

**What to learn:** packed `u32` color (RGBA8) is 4× memory savings vs
float-per-channel — GPU upload is the bottleneck. Master scale is a
beautiful one-knob ergonomic. Per-parameter float scale beats bool.
Buffer is plain data — replayable, network-streamable, serializable.

**What to avoid:** the 3-AND opt-in is a footgun. Default new actors to
visualization-on; require explicit opt-out, not opt-in.

#### Box2D — `b2Draw` (callback / immediate-mode, 2D)

Same pattern as Bullet ([b2Draw.h](https://github.com/google/liquidfun/blob/master/liquidfun/Box2D/Box2D/Common/b2Draw.h)).
**Notable:** distinguishes `DrawCircle` from `DrawSolidCircle` at the
API surface. Outline vs fill is a first-class API distinction.

#### ODE — `drawstuff` (bundled demo, anti-pattern)

ODE bundles `drawstuff` as a *demo* renderer ([docs](http://opende.sourceforge.net/docs/group__drawstuff.html)).
ODE docs say *"DrawStuff is a simple library written only to display
the ODE demos and is not intended to be used in your own projects."*
Consequence: every real ODE user rebuilds debug viz from scratch.

**The lesson:** never bake the renderer into the physics library.
`crd-eylem` should expose the primitive stream + a separate
`crd-draw` module that consumes it.

#### Synthesis

| Pattern | SDKs | Pro | Con |
|---|---|---|---|
| Pure virtual callback (immediate) | Bullet, Box2D | Zero alloc, host owns batching | One virtual call per primitive; many overrides |
| Retained buffer (pull) | PhysX | Cache-friendly, serializable, replayable | Two-phase; buffer in SDK heap |
| Bundled demo renderer | ODE | Easy demo | Not for production; everyone rewrites |

Cerid takes the **PhysX retained-buffer model as the durable contract**
plus **immediate-mode wrappers as ergonomic sugar** on top. Buffer →
serializability + replay (already a Cerid roadmap goal, ADR-0063).
Sugar → ergonomic at call sites.

### 2.2 AAA engines — Unreal, Unity, Godot, Source 2, Frostbite, idTech 7

#### Unreal — five overlapping subsystems

Unreal does not have one debug renderer; it has at least four:

1. **`DrawDebugHelpers.h`** — runtime free functions: `DrawDebugLine`,
   `DrawDebugSphere`, `DrawDebugBox`, `DrawDebugCapsule`, `DrawDebugCone`,
   `DrawDebugCoordinateSystem`, `DrawDebugFrustum`, `DrawDebugCamera`,
   `DrawDebugMesh`, `DrawDebugString`. Each takes
   `(UWorld*, ..., color, bPersistentLines, LifeTime, DepthPriority,
    Thickness)`. Lifetime per call. Depth priority: `SDPG_World`
   (depth-tested) vs `SDPG_Foreground` (always-on-top).
   ([source](https://docs.unrealengine.com/en-US/API/Runtime/Engine/DrawDebugSphere/index.html))
2. **`ULineBatchComponent`** — the batching layer behind the free
   functions. Epic explicitly tells you to use `DrawLines(TArrayView)`
   for runtime ([source](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/Components/ULineBatchComponent)).
3. **`FPrimitiveDrawInterface` (PDI)** — editor/render-thread debug
   API used by `UComponentVisualizer::DrawVisualization()`. Hooks via
   **HitProxies** (`PDI->SetHitProxy(new HMyProxy(...))`) so debug
   primitives become *clickable* — picking is built into the viz.
   ([source](https://unrealcommunity.wiki/component-visualizers-xaa1qsng))
4. **`Gameplay Debugger`** — fully retained, replicated debug system.
   Categories inherit `FGameplayDebuggerCategory` and override
   `CollectData()` + `DrawData()`. Data added with `AddTextLine` /
   `AddShape` / `SetDataPackReplication` is **automatically replicated
   server→client**. ([source](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/GameplayDebugger/FGameplayDebuggerCategory))
5. **`UCanvas` / `HUD`** — projected text via `Canvas->Project(WorldLocation)`.

**What to learn:**
- Flat `DrawDebug*` free-function API is what users actually call. Even
  if implementation is sophisticated, API should be one-line at call
  site.
- Batching component sits behind the API. Don't expose batching to
  call sites.
- **Picking** (HitProxy-style) is worth designing in from day one.
  Reserve a `u32 picking_id` slot in the per-primitive header.
- A **categorized, replication-aware** layer (Gameplay Debugger) is
  for shipped multiplayer. Cerid doesn't need it day 1, but a
  category/tag system in the primitive stream makes it tractable.

#### Unity — Gizmos, Handles, Debug.Draw

Editor-only: `Gizmos.DrawWireSphere`, `Gizmos.DrawSphere`,
`Gizmos.DrawCube`, etc. State-machine API
(`Gizmos.color = X; Gizmos.matrix = M; Gizmos.DrawWireSphere(...)`).
([source](https://docs.unity3d.com/ScriptReference/Gizmos.DrawWireSphere.html))
Physics Debug Visualization window: per-rigidbody/collider visibility,
asleep/dynamic/kinematic color rules.

**What to learn:** state-machine API is concise but a footgun if you
forget to reset state. Cerid prefers **parameter-passing per call** for
stateless safety, with optional `ScopedColor` / `ScopedTransform` RAII
helpers as syntactic sugar.

**What to avoid:** the runtime/editor split. Cerid debug viz must work
in shipping builds (sandbox is built in every config —
`feedback_sandbox_always_built.md`).

#### Godot 4 — `MeshInstance3D` + Jolt debug

Godot's first-party debug is sparse: `get_debug_mesh()` on collision
shapes returns an `ArrayMesh` you parent into the scene tree.
**No immediate-mode draw API in core.** Community fills the gap
([DmitriySalnikov/godot_debug_draw_3d](https://github.com/DmitriySalnikov/godot_debug_draw_3d)).
Jolt physics debug is a known weak spot
([issue 101212](https://github.com/godotengine/godot/issues/101212)).

**What to learn:** the **retained-mesh** alternative (build a mesh,
parent it). Bad for high-frequency per-frame churn; good for static
visualization. Cerid supports both: an **ephemeral immediate-mode
stream** for per-frame churn, and an **opt-in persistent handle**
(`DebugDrawHandle h = persistent_box(...); h.update(); h.destroy();`)
for shapes that don't change frame-to-frame.

#### Source 2 — Migdalskiy GDC 2014 (the seminal physics debug talk)

Sergiy Migdalskiy's "Physics for Game Programmers: Debugging Physics"
talk ([slides](https://valvearchive.com/Presentations/GDC%202014/Migdalskiy_Sergiy_Physics_for_Game_02.pdf),
[video](https://www.gdcvault.com/play/1020065/Physics-for-Game-Programmers-Debugging))
is the seminal industry talk on physics debug viz. Key ideas:

- **Out-of-process debugger via `ReadProcessMemory`** — Source 2's
  physics debugger is a *separate process* that reads the live game's
  memory using Windows' `ReadProcessMemory` API. Decouples viz from
  the game's frame budget entirely. Inspired OmniPVD, RemedyBG.
- **Clang-driven serializer generation** — they parse C++ headers with
  libclang to auto-generate a serializer for every physics struct
  (`CRnCapsuleShape`, etc.). The debugger gets a fresh schema for free
  as the engine evolves.
- **Replay (stepping back in time) is the killer feature.** Cannot do
  with immediate-mode draw calls — need a retained, serializable
  debug-data buffer.

**What to learn:** out-of-process / replayable / serializable debug data
is *strictly more powerful* than in-process draw calls. Cerid is
already committed to determinism + replay (ADR-0063). Build the
debug-data buffer such that it can be serialized to disk and replayed
in a separate viewer. PhysX `PxRenderBuffer` pattern at scale.

#### Frostbite — debug viz is a frame graph node

Could not locate a Frostbite-specific debug-viz talk. The
[Frostbite FrameGraph talk](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in)
treats debug-viz passes as ordinary nodes that consume the depth target
and produce a transparent overlay.

**What to learn:** integrate Cerid's debug renderer as a
**frame-graph pass** in `crd-renderer`, not as a side-channel that
bypasses the graph.

#### idTech 7 — engine-internal metrics + console var toggles

Billy Khan tweet ([source](https://x.com/billykhan/status/1240739446024265739)):
external overlays (RTSS etc.) "introduce bubbles" in the GPU pipeline.
**Engine-internal performance metrics outperform external overlays.**

**What to learn:** Cerid's debug HUD must integrate with the existing
frame graph for the same reason. And debug viz should be **toggleable
by ImGui control panel + console var** (when the console lands).

### 2.3 Open-source library survey

#### Lampert `debug-draw` — the gold-standard minimal baseline

[Repo](https://github.com/glampert/debug-draw). ~3000 lines,
single-header (`debug_draw.hpp`), `DEBUG_DRAW_IMPLEMENTATION` define
for the impl. Three context modes (global, per-thread TLS, explicit).

`RenderInterface` callbacks (host implements ~5 methods total):
```cpp
virtual void beginDraw();
virtual void endDraw();
virtual GlyphTextureHandle createGlyphTexture(int w, int h, const void* pixels);
virtual void destroyGlyphTexture(GlyphTextureHandle);
virtual void drawPointList(const DrawVertex* points, int count, bool depthEnabled);
virtual void drawLineList(const DrawVertex* lines,  int count, bool depthEnabled);
virtual void drawGlyphList(const DrawVertex* glyphs, int count, GlyphTextureHandle);
```

Public API surface:
`point`, `line`, `screenText`, `projectedText`, `arrow`, `cross`,
`circle`, `plane`, `sphere`, `cone`, `box`, `aabb`, `frustum`,
`axisTriad`, `vertexNormal`, `tangentBasis`, `xzSquareGrid`, `capsule`.

Lifecycle: `initialize` / `flush(currTimeMillis)` / `clear` / `shutdown`.
The `flush(time_ms)` enables time-based persistence — primitives with
non-zero TTL fade out automatically.

**This is the architectural baseline Cerid mirrors.**

What to copy:
- The 3-callback surface (point list, line list, glyph list)
- Lifecycle (`flush(time_ms)` for time-based persistence)
- Explicit-context mode (TLS adds overhead; explicit cleanest for
  jobified architecture)
- Tiny `DrawVertex` union

What to improve:
- Add **triangle list** callback (Lampert is wireframe-only — Cerid
  needs solid)
- Pack color as `u32` (Lampert uses 3 floats — wasteful)
- **Three depth modes** per primitive (Lampert has one bool)
- **Persistent handles** (Lampert is purely immediate-mode)
- **Picking IDs** (`u32` slot per primitive)

#### Other libraries surveyed

- [sjb3d/imdd](https://github.com/sjb3d/imdd) — header-only C/C++ with
  drop-in OpenGL 3.2 + Vulkan back-ends bundled. Proves out the
  shader-shipping model Cerid wants.
- [ozz-animation](https://github.com/guillaumeblanc/ozz-animation) —
  has `DrawPosture(skeleton, model_space_matrices)` for skeletons.
  **Worth modeling Cerid's articulation rendering on directly.**
  Joints as small spheres connected by capsule "bone" segments, with
  tail-direction indicator.
- NVIDIA Falcor `DebugDrawer` — thin batched-line-only utility. Proves
  that even research-grade rendering frameworks treat debug-viz as a
  small utility, not a core system.

### 2.4 Self-contained shader-shipping patterns

Three options observed in the wild:

#### ImGui — backend-bundled, embedded SPIR-V byte arrays

Each rendering backend (`imgui_impl_vulkan.cpp`,
`imgui_impl_dx12.cpp`, etc.) embeds its own shader as a
`static const uint32_t` SPIR-V byte array, compiled at ImGui build
time and baked into the C++ source. Backend creates its own pipeline,
its own descriptor set layout. **Zero runtime dependency** on the
host's shader system. **No hot-reload** — ImGui shaders are stable;
reload isn't a feature.

#### NVIDIA Falcor — shaders in source tree, runtime compile

Ships shaders as `.slang` / `.hlsl` files in repo, compiled at runtime
via Slang or DXC. **Hot-reload works.** Requires the Slang/DXC
runtime.

#### Lampert `debug-draw` — host owns the shader

Ships *no* shaders at all. The host's `RenderInterface::drawLineList(...)`
is responsible for binding whatever shader the host already has set
up for line rendering. **Maximum decoupling, maximum host work.**

#### Recommendation for Cerid

Cerid already has `crd-shader` as a first-class module with hot-reload.
The right answer: **ship GLSL source files inside `crd-draw`'s
asset tree, register them as cookable resources, integrate with
`crd-shader`'s hot-reload pipeline.** Specifically:

1. `engine/draw/shaders/line_aa.{vert,frag}.glsl`,
   `point.{vert,frag}.glsl`, `triangle.{vert,frag}.glsl`,
   `glyph.{vert,frag}.glsl` — checked into repo
2. Asset cooker compiles them to SPIR-V at build time → ships them in
   a small CRDR pack
3. `crd-draw::init(ResourceManager&)` mounts the pack +
   registers the visualizer
4. Hot-reload Just Works in dev — same path as every other shader

This contradicts the "decoupled like ImGui" approach but matches
Cerid's elite-quality bar (`feedback_quality_bar.md`): single-path,
hook-based contracts, no dual demo/real shader paths.

## 3. Rendering-quality techniques

This is where Cerid stands out. Most debug renderers look *crude*. The
techniques below are how the best projects look professional.

### 3.1 Anti-aliased thick lines — vertex-shader quad expansion

`GL_LINES` / `VK_PRIMITIVE_TOPOLOGY_LINE_LIST` is fundamentally
limited:
- Width support is driver-dependent and capped (Vulkan core requires
  width=1.0 only; `wideLines` is an *optional* feature)
- No anti-aliasing on Vulkan unless line smooth + alpha blend is
  hand-rolled (and quality is poor)
- No miters/bevels

Industry-standard alternative: **vertex-shader quad expansion**.
What Three.js `LineSegments2`/`Line2`/`LineMaterial`
([source](https://threejs.org/docs/pages/LineSegments2.html)) and
Mapbox-GL ([source](https://blog.mapbox.com/drawing-antialiased-lines-with-opengl-8766f34192dc))
do, walked through in Matt DesLauriers' [Drawing Lines is Hard](https://mattdesl.svbtle.com/drawing-lines-is-hard).

Algorithm:
1. CPU emits each line segment as **two vertices duplicated 2× = 4
   vertices** (or as instanced quads). Per-vertex attributes:
   `instance_start`, `instance_end`, `side` (-1 or +1),
   `corner` (0 or 1).
2. Vertex shader transforms both endpoints to clip space, computes
   screen-space segment direction + perpendicular, pushes the vertex
   out by `line_width/2 * side` *in screen-space pixels* (constant
   pixel width regardless of distance) **or** in world units (constant
   world thickness).
3. Vertex shader passes a **signed distance from line center** as a
   varying.
4. Fragment shader:
   `alpha = 1 - smoothstep(width/2 - 0.5, width/2 + 0.5, abs(distance))`.
   Per-pixel distance falloff = perfect 1-pixel-wide AA at any line
   angle, any thickness.

Mapbox refinement: encode the perpendicular sign in the LSB of a
doubled coordinate to save 4 bytes/vertex.

Joints: bevel joints (clip the perpendicular extrusion at half segment
length) are sufficient for wireframe debug shapes. Miter joints overkill.

**Chan & Durand 2005 prefiltered lines**
([GPU Gems 2 ch. 22](https://developer.nvidia.com/gpugems/gpugems2/part-iii-high-quality-rendering/chapter-22-fast-prefiltered-lines))
uses CPU-precomputed LUT-sampled filters (Gaussian, optimal cubic, box).
*Higher quality* than per-pixel distance for thick stylized lines, but
overkill for engine debug viz where 1-2 pixel lines dominate.

**Geometry-shader AA lines** (atyuwen, McGuire 2007): same per-pixel
distance core but in a geometry shader. Geometry shaders are slow on
modern GPUs (especially mobile/Apple Silicon). **Avoid.** Use
vertex-shader expansion.

**Recommendation:** vertex-shader quad expansion + per-pixel distance
AA. ~15 lines vertex, ~5 lines fragment. Beats `GL_LINES` on every
axis: AA, thickness, portability, identical look across Vulkan / D3D12
/ Metal. Screen-space pixel width by default; `world_units` flag per
primitive for cases where physical scale matters (joint markers,
articulation chains).

### 3.2 Solid translucent rendering

Box → 12 triangles (6 quads × 2). Sphere (icosphere subdivision 1) →
80 triangles. Capsule → ~600 triangles. AABB → 12 triangles.

**Order-independent transparency (OIT)?** McGuire & Bavoil 2013
weighted-blended OIT
([JCGT paper](https://jcgt.org/published/0002/02/09/paper.pdf),
[author blog](http://casual-effects.blogspot.com/2014/03/weighted-blended-order-independent.html))
defines a two-target approach:

```glsl
// per-fragment write to accum (RGBA16F) and revealage (R8 or R16F):
float weight = clamp(pow(min(1.0, color.a * 10.0) + 0.01, 3.0)
                    * 1e8 * pow(1.0 - z * 0.9, 3.0), 1e-2, 3e3);
accum     = vec4(color.rgb * color.a, color.a) * weight;
revealage = color.a;
// composite:
final = accum.rgb / max(accum.a, 1e-5) * (1 - revealage) + bg * revealage;
```

Generally **not worth it for debug viz**. Debug primitive overdraw is
bounded (hundreds of shapes, not millions). Back-to-front sort by
centroid distance gives correct results 99% of the time at zero shader
cost.

WBOIT becomes worthwhile when (a) you have *thousands* of overlapping
translucent shapes (e.g. SDF cell visualization, particle field), or
(b) CPU sort cost matters.

**Recommendation:** default to **CPU sort by centroid depth + standard
alpha blend**. Add WBOIT as switchable path *behind* the same primitive
API for SDF visualization (Phase 3.1.5+).

### 3.3 Depth-aware rendering — overlay vs occluded

Three modes per primitive:

1. **`Depth_Test`** — standard depth test, primitive occluded by world
   geometry (default).
2. **`Depth_Always`** — `depthCompareOp = VK_COMPARE_OP_ALWAYS`,
   depth write off. Always-on-top. Used for selection outlines, tagged
   actors.
3. **`Depth_XRay`** — render twice. First pass `depthCompareOp =
   GREATER` with dimmed color (occluded portion). Second pass
   `LESS_OR_EQUAL` with full color (visible portion). Reads as "I can
   see the whole shape, but I can tell which part is behind walls."

**Polygon offset** (`VkPipelineRasterizationStateCreateInfo::depthBiasEnable`)
to push wireframe slightly toward camera in depth, preventing z-fighting
when wireframe overlays solid. Per-pipeline state — needs a separate
pipeline for biased rendering. Aras Pranckevičius warning:
[Depth bias and the power of deceiving yourself](https://aras-p.info/blog/2008/06/12/depth-bias-and-the-power-of-deceiving-yourself/).
Don't bias every pass.

**Recommendation:** support all three modes per primitive, packed into
the same `u32` as color. Wireframe-over-solid uses pipeline with
`depthBiasConstantFactor = -1.0`.

### 3.4 World-space text

Two viable techniques:

#### Chris Green / Valve SIGGRAPH 2007 SDF text

[Paper](https://steamcdn-a.akamaihd.net/apps/valve/2007/SIGGRAPH2007_AlphaTestedMagnification.pdf).
Bake an 8-bit alpha distance-field atlas at moderate resolution
(256×256 fits ~95 ASCII glyphs at 32px), render quads, fragment:
`alpha = smoothstep(0.5 - smoothing, 0.5 + smoothing, distance)`.
Crisp at any zoom. Outlines/glow/drop-shadow trivially via remapped
`smoothstep` thresholds. **MTSDF** (multi-channel) is the modern
refinement — preserves sharp corners.

#### ImGui-projected screen-space text

Call `Camera::project(world_pos)` to get screen coords, draw via
ImGui's draw-list at that pixel. Always pixel-perfect, no SDF baker
needed. But text is *not* in the 3D scene (no depth occlusion, no
transform-with-camera-roll).

**Recommendation:** ship both. **Day-one default `draw_text(world_pos,
str)` uses ImGui projection** (we already have `crd-imgui`; bitmap
font is ready). **Reserve `draw_text_3d(world_pos, str, {.size_world=0.1,
.billboard=true, .occlude=true})` for Phase 3.1.5+** when `crd-sdf`
ships MTSDF (signed-distance-field substrate is on the roadmap for
fonts via `phase-3.1.5-sdf.md`).

Don't build the SDF path until the SDF substrate exists. ImGui
projection is honest about what's available now.

### 3.5 Per-shape tessellation conventions

From Bullet's `drawSphere`, PhysX wireframe defaults, and
[songho.ca sphere reference](https://songho.ca/opengl/gl_sphere.html):

| Shape | Wireframe | Solid (icosphere subdivision) |
|---|---|---|
| Sphere (UV) | 16 longitude × 8 latitude | n/a |
| Sphere (icosphere) | n/a — UV is more recognisable | sub 1 (80 tris) lo, sub 2 (320) hi |
| Capsule | 16 circumferential, 2 hemispheres × 8 stacks | 24×8 |
| Cylinder | 16 segments | 24 |
| Cone | 16 base segments | 24 base, 4 cap |
| Box | 12 edges (no subdivision) | 12 tris always |
| Circle/disc | 32 segments | 32 tris fan |
| Arrow | 1 line + 4-segment cone for head | n/a |

**UV vs icosphere tradeoff:** UV spheres render the "great circle"
wireframe users expect (you can read latitude/longitude on a UV
sphere). Icospheres look uniform but lack the visual cues physics
debug viz relies on (no obvious "equator" or "axis"). **Use UV for
wireframe, icosphere for solid** — wireframe benefits from
recognisable structure, solid benefits from icosphere's vertex
regularity (better lighting if we ever add it).

### 3.6 Joint visualization

Conventions across engines:
- **ROS RViz / Maya / Houdini** — RGB triad: red = +X, green = +Y,
  blue = +Z. **Universal.**
  ([source](https://docs.ros.org/en/humble/Tutorials/Intermediate/RViz/RViz-User-Guide/RViz-User-Guide.html))
- **Maya joint markers** — articulation chain rendered as cone-segment
  connectors between joint origins (apex at child, base at parent).
  Cone *direction* encodes parent→child relation.
- **Constraint frames (Bullet/PhysX)** — draw both frames (parent and
  child) overlaid on joint position, slightly offset, dimmed to show
  alignment error.
- **Limits** — for hinges, draw an arc from min-angle to max-angle in
  the constraint plane. For ball joints, draw the swing cone + twist
  arc. For sliders, draw a line from min to max with tick marks.
- **Forces/impulses** — arrow with shaft-length proportional to
  `log(magnitude)` so order-of-magnitude differences fit on screen.

**Recommendation:** lock RGB triad universally. Build articulation
rendering as cone-segments. Joint limits get a dedicated
`draw_arc(plane, min_angle, max_angle)` primitive in the API surface
from the start.

## 4. Cerid integration plan

### 4.1 Module location

`engine/draw/` — peer of `engine/renderer/`, `engine/sdf/`,
`engine/audio/`. Public headers under `engine/draw/include/crd/draw/`.

### 4.2 CMake link line

```cmake
target_link_libraries(crd-draw PUBLIC
    crd-core      # types, asserts
    crd-memory    # IAllocator, frame-arena allocations
    crd-containers# Array<T>
    crd-math      # Vec3, Mat4, Quat (NOT crd-math-simd — host-side bookkeeping)
    crd-rhi       # backend-agnostic GPU interface
    crd-renderer  # IRenderPath integration via add_draw_overlay_pass
    crd-shader    # cooked SPIR-V loading + hot-reload
    crd-resources # ResourceManager + CRDR pack mounting
    crd-scene     # ISystem + DebugVizComponent registration
    crd-imgui     # day-one world-space text via projection
)
```

### 4.3 Schedule slot

`PostRender` phase (per the Cerid-audit research). Single
`DebugVizSystem : ISystem` reads `DebugVizComponent` flags + queries
component-typed visualizers from a per-module registry, calls
immediate-mode API on each.

### 4.4 Render-path integration

**Helper function pattern**, not a new `IRenderPath`:

```cpp
namespace crd::draw {
    // Add an overlay pass to the active frame graph that consumes the
    // scene color attachment and draws all submitted debug primitives.
    // Called from inside a render path's build() method.
    void add_draw_overlay_pass(
        renderer::FrameGraph&  fg,
        renderer::ImageHandle  scene_color,
        renderer::ImageHandle  scene_depth,
        const RenderBuffer& buffer);
}
```

This grafts onto `ForwardRenderPath`, future `DeferredRenderPath`,
future `VisibilityBufferRenderPath` — any `IRenderPath` calls it after
its own passes.

The overlay pass:
- Reads scene_depth (for `Depth_Test` mode primitives)
- Reads + writes scene_color (alpha-blends overlay)
- Runs three sub-passes in order:
  1. Solid triangles (back-to-front sorted, alpha-blended)
  2. Lines (vertex-shader quad expansion, AA fragment)
  3. Points (small AA quads)
  4. Glyphs (only if SDF text path is active; ImGui handled separately)

### 4.5 Asset shipping

- `engine/draw/shaders/line_aa.{vert,frag}.glsl` — line
  rendering
- `engine/draw/shaders/triangle.{vert,frag}.glsl` — solid fill
- `engine/draw/shaders/point.{vert,frag}.glsl` — points
- `engine/draw/shaders/glyph_sdf.{vert,frag}.glsl` — reserved
  for Phase 3.1.5 SDF text
- Asset cooker compiles them at build time → CRDR pack
  `cooked_assets/draw_shaders.crdr`
- `crd::draw::init(ResourceManager& rm)` mounts the pack + loads
  shader resources by ID

No embedded `static const u32[]` byte arrays. Use the existing pipeline
end-to-end. Hot-reload free.

### 4.6 Per-component visualizer plug-in registry

The Cerid audit notes there's no formal pattern. Inventing one (small,
optional layer):

```cpp
namespace crd::draw {

// Visualizer for one component type. Modules register their own.
template <typename T>
using ComponentVisualizer = void(*)(const T& component, const Transform& xform,
                                    RenderBuffer& out, const VizConfig& cfg);

// Registry sits in crd-draw; modules register at init time.
class VisualizerRegistry {
public:
    template <typename T>
    void register_visualizer(ComponentVisualizer<T> fn);

    void run_all(const World& world, RenderBuffer& out, const VizConfig& cfg);
};

// crd-eylem-viz (small companion module) registers:
//   register_visualizer<RigidBodyComponent>(&draw_rigid_body);
//   register_visualizer<ColliderComponent>(&draw_collider);
//   register_visualizer<JointComponent>(&draw_joint);
}
```

Plug-in pattern: `crd-eylem-viz` is a small wrapper module that
depends on both `crd-eylem` and `crd-draw`, registers visualizers,
and is otherwise empty. `crd-sdf-debug`, `crd-audio-debug`, etc. follow
the same pattern. Keeps `crd-eylem` itself free of any rendering
dependency.

This is the **dependency-inverted plug-in** pattern. Substrate modules
(`crd-eylem`, `crd-sdf`, `crd-audio`) emit pure data. Companion modules
(`crd-eylem-viz` etc.) bridge to `crd-draw`.

## 5. Data model

### 5.1 `RenderBuffer` — the durable contract

```cpp
namespace crd::draw {

// 4-byte packed color: RGBA8.
struct Color { u8 r, g, b, a; };
constexpr Color kRed     = {255,   0,   0, 255};
constexpr Color kGreen   = {  0, 255,   0, 255};
constexpr Color kBlue    = {  0,   0, 255, 255};
constexpr Color kYellow  = {255, 255,   0, 255};
constexpr Color kCyan    = {  0, 255, 255, 255};
constexpr Color kMagenta = {255,   0, 255, 255};
constexpr Color kWhite   = {255, 255, 255, 255};
// ... + named conventions for physics: kBodyDynamic, kBodyAsleep,
// kContactNormal, kJointFrame, kVelocityArrow, ...

// Per-primitive flags packed into a u32 header.
struct PrimFlags {
    u32 depth_mode    : 2;   // Test / Always / XRay
    u32 category      : 4;   // Physics / Audio / SDF / Nav / Scene / User0..7
    u32 width_units   : 1;   // 0 = pixels, 1 = world units
    u32 picking_id    : 16;  // 0 = no picking
    u32 reserved      : 9;
};

struct DebugPoint    { Vec3 pos;                        u32 color; PrimFlags flags; f32 size; f32 lifetime_s; };
struct DebugLine     { Vec3 a, b;                       u32 color; PrimFlags flags; f32 width; f32 lifetime_s; };
struct DebugTriangle { Vec3 a, b, c;                    u32 color; PrimFlags flags;            f32 lifetime_s; };
struct DebugText     { Vec3 pos; const char* str; u8 size_px; u8 anchor; u32 color; f32 lifetime_s; };

class RenderBuffer {
public:
    void clear();
    void append(const RenderBuffer& other);
    void shift(Vec3 origin_delta);  // for floating-origin support

    // Accessors for the renderer pass + serializer.
    span<const DebugPoint>    points()    const noexcept;
    span<const DebugLine>     lines()     const noexcept;
    span<const DebugTriangle> triangles() const noexcept;
    span<const DebugText>     texts()     const noexcept;

    // Mutators called by immediate-mode API.
    void add_point(...);
    void add_line(...);
    void add_triangle(...);
    void add_text(...);

private:
    Array<DebugPoint>    m_points;
    Array<DebugLine>     m_lines;
    Array<DebugTriangle> m_triangles;
    Array<DebugText>     m_texts;
};

}  // namespace crd::draw
```

Packed `u32` color (4 bytes, RGBA8) per PhysX. `f32 lifetime_s` per
primitive (Unreal-style; 0 = single-frame). Picking ID slot (Unreal
HitProxy lesson). Categories (Gameplay Debugger lesson).
Floating-origin support via `shift()` (PhysX lesson — important for
large worlds eylem v6+ will need).

### 5.2 Immediate-mode API — the ergonomic top layer

```cpp
namespace crd::draw {

// Free functions delegate to a current-frame singleton RenderBuffer
// or to an explicit handle (jobified-safe).
void line(Vec3 a, Vec3 b, Color c, f32 thickness_px = 1.0f, f32 lifetime_s = 0);
void point(Vec3 p, Color c, f32 size_px = 4.0f);
void box_wire(Mat4 transform, Vec3 half_extents, Color c, f32 thickness_px = 1.0f);
void box_solid(Mat4 transform, Vec3 half_extents, Color c, f32 alpha = 0.3f);
void sphere_wire(Vec3 center, f32 radius, Color c, u32 segments = 16);
void sphere_solid(Vec3 center, f32 radius, Color c, f32 alpha = 0.3f);
void capsule_wire(Vec3 a, Vec3 b, f32 radius, Color c);
void capsule_solid(Vec3 a, Vec3 b, f32 radius, Color c, f32 alpha = 0.3f);
void aabb(AABB box, Color c, f32 thickness_px = 1.0f);
void arrow(Vec3 origin, Vec3 dir, f32 length, Color c, f32 head_size = 0.1f);
void axis_triad(Mat4 transform, f32 size = 1.0f);   // RGB convention
void arc(Vec3 center, Vec3 axis_normal, Vec3 zero_dir, f32 radius,
         f32 angle_min, f32 angle_max, Color c, u32 segments = 24);
void cross_3d(Vec3 center, f32 size, Color c);
void grid(Vec3 origin, Vec3 right, Vec3 forward, u32 cells_x, u32 cells_z,
          f32 cell_size, Color c);
void frustum(Mat4 view_proj, Color c);
void text(Vec3 world_pos, StringView str, Color c, u8 size_px = 14);

// Master scale (PhysX eSCALE lesson) — multiplies every primitive size.
void set_master_scale(f32 s);
f32  master_scale();

// Category filter — multiplied in via per-frame mask.
void set_category_mask(u32 mask);
u32  category_mask();

// Lifecycle.
void frame_begin();
void frame_end();   // commits to active overlay pass
}
```

State-machine API rejected — parameter-passing is stateless-safe (Unity
lesson). Optional RAII helpers (`ScopedColor`, `ScopedTransform`) live
in a separate `draw/scope.hpp` for users who want them.

### 5.3 Per-shape tessellator caches

Static const `Vec3` arrays for unit primitives, baked at compile time:
- `kUnitBoxLines[24]` — 12 lines × 2 endpoints
- `kUnitBoxTriangles[36]` — 12 triangles × 3 vertices
- `kUnitSphereWireframe16x8[N]` — pre-tessellated UV sphere lines
- `kUnitSphereIcosphere1[80*3]` — pre-tessellated icosphere triangles
- `kUnitCapsuleWireframe16x8[N]` — pre-tessellated capsule lines
- `kUnitCapsuleSolid24x8[N*3]` — pre-tessellated capsule triangles

Total static const memory: ~10-30 KB. Zero per-frame tessellation cost
for the standard shapes. Custom shapes (frustum, arc) tessellate on
the fly because their parameters change per call.

## 6. Performance budget

Eylem v1+ target: 1k bodies @ 60 Hz, debug viz on.

Per body:
- 1 wireframe collider shape (e.g. capsule = 70 lines)
- 1 velocity arrow (1 line + 4-line cone head = 5 lines)
- 1 axis triad (3 lines)
- Avg AABB (12 lines) — every other frame
**Total ≈ 90 lines × 1k bodies = 90k lines/frame.**

Per contact (avg ~3 contacts/dynamic body, 50% bodies dynamic):
- 1 contact point (1 point) + 1 normal arrow (5 lines) = 6 primitives
- 1500 contacts × 6 = 9k primitives

Per joint (assume 100 joints):
- 1 joint frame triad (6 lines) + 1 limit arc (24 lines) = 30 primitives
- 100 × 30 = 3k primitives

**Worst-case total: ~100k lines + ~10k points + ~5k triangles per frame.**

Vertex memory:
- Lines: 100k × (2 verts × 4 corners × ~32 bytes/vert) ≈ **25 MB**
  (way too much — needs instancing)
- With instanced quads: 100k × (1 instance × 32 bytes/instance) = 3.2 MB
- Triangles: 5k × 3 × 32 bytes = 0.5 MB
- Points: 10k × 32 = 0.3 MB

**Verdict: instancing is mandatory for lines.** Each line = one
instance of a unit quad with `(start, end, color, flags, width)`
attributes = ~32 bytes/line. GPU upload ~3-4 MB/frame at peak. Well
under the budget.

Per-frame CPU cost estimate:
- Per-primitive `add_line()` etc.: ~100 ns (push_back + flag pack)
- 100k primitives: 10 ms — too slow if all on one thread

**Mitigation:** the API supports fan-out emission. `parallel_for` over
chunks of bodies can each have their own thread-local
`RenderBuffer`; merge at end of phase via `append()` in
deterministic order. This matches the eylem job pattern.

## 7. Cerid-specific decisions

### 7.1 Determinism contract

`crd-draw` does NOT participate in the simulation. It reads
post-step state, emits visualization data. No physics computation, no
FP state mutation. Deterministic by construction (no FP comparisons
that depend on runtime state — only `add_line(...)` style appends
which are bit-exact across runs given the same input).

### 7.2 Replay capture

`RenderBuffer` is plain data — `serialize(buffer)` writes the
entire frame's debug primitives to disk. During deterministic replay
(ADR-0063 capture mode), the buffer is dumped per-frame alongside
the physics replay log. The replay viewer (Phase 7+) consumes both
streams to render the historical scene.

This is the Migdalskiy out-of-process pattern, achieved naturally
because we chose retained-buffer over immediate-callback.

### 7.3 Sandbox-smoke compatible

The visualizer must not crash if the sandbox-smoke harness runs the
sandbox for 3 seconds without any interactive input. Achieved by:
- All primitives are no-ops if no overlay pass is active (frame
  graph not built)
- Buffer auto-clears each frame via `frame_begin()`
- Lifetime decay handled even with zero active primitives

### 7.4 Profile-system gating

Per ADR-0060, the debug overlay is a profile-driven toggle:
- `dev` profile (default in non-shipping configs) — overlay ON
- `shipping` / `cinematic` profiles — overlay OFF (zero CPU overhead;
  the system early-outs in `frame_begin()`)
- Per-category bits exposed in the profile too (so a sim build can
  enable Audio category but disable Physics)

### 7.5 Hot-reload story

Shaders shipped via `crd-shader` get the existing watch-based reload
for free. Per-shape tessellator caches are `static const` — no reload
needed (changes require rebuild, which is what you want for debug data
shapes).

## 8. The chosen architecture (synthesis)

The locked decisions from sections 1-7 in 14 bullets — these are also
the ADR-0066 commitments:

1. **Two-layer API.** Bottom: retained `RenderBuffer` (PhysX-style
   SoA arrays of `DebugPoint` / `DebugLine` / `DebugTriangle` /
   `DebugText`, packed `u32` color, `u32` flag header). Top:
   immediate-mode free functions (`draw::line`,
   `sphere_wire`, etc.).
2. **Renderer integration via 4 callbacks** (Lampert pattern, evolved):
   `submit_points`, `submit_lines`, `submit_triangles`, `submit_glyphs`.
   Implemented as a frame-graph overlay pass added by
   `add_draw_overlay_pass(fg, color, depth, buffer)`.
3. **Vertex-shader quad-expanded AA lines** (Three.js / Mapbox).
   Pixel-width by default, world-units toggle per primitive. Per-pixel
   distance falloff in fragment shader.
4. **Three depth modes per primitive** (packed in flag `u32`):
   `Depth_Test`, `Depth_Always` (overlay), `Depth_XRay` (dual-pass
   dimmed-occluded + bright-visible).
5. **Master visualization scale** (PhysX `eSCALE` lesson) — global
   `f32` that multiplies every primitive size.
6. **Categories per primitive** (Gameplay Debugger lesson):
   `Physics` / `Audio` / `Navmesh` / `SDF` / `Scene` / `User0..7`.
   Per-category mask filters at render time.
7. **Persistent handles** (Godot lesson):
   `DebugDrawHandle h = persistent_box(...); h.update(...); h.destroy()`
   for shapes that don't change frame-to-frame.
8. **Lifetime per primitive** (Unreal lesson) — `f32 lifetime_s`,
   alpha-fades in last 0.25s, decays automatically. Replaces
   persistent-vs-instant distinction at the API.
9. **Day-one `draw_text` uses ImGui projection.** Reserve
   `draw_text_3d` for Phase 3.1.5+ when `crd-sdf` ships MTSDF.
10. **Shaders shipped via `crd-shader` + `crd-resources`.** GLSL
    source in `engine/draw/shaders/`; cooked SPIR-V in a CRDR
    pack `cooked_assets/draw_shaders.crdr`; mounted at
    `crd::draw::init(rm)`. Hot-reload via the existing watch
    system. NO embedded static const SPIR-V arrays.
11. **Picking ID slot reserved** (Unreal HitProxy lesson). Day-one
    `picking_id = 0` for everything; editor wires it later.
12. **Sort-by-centroid + standard alpha blend** for translucent solids.
    WBOIT switchable path reserved for SDF cell visualization in
    Phase 3.1.5+.
13. **Dependency-inverted plug-in pattern.** Substrate modules
    (`crd-eylem`, `crd-sdf`, `crd-audio`) emit pure data, no rendering
    deps. Companion modules (`crd-eylem-viz`, `crd-sdf-debug`, ...)
    bridge to `crd-draw`. `VisualizerRegistry` lives in
    `crd-draw` itself.
14. **Replay-friendly.** Serialize the per-frame `RenderBuffer`
    to disk during deterministic replay capture (ADR-0063). Out-of-
    process viewer pattern (Migdalskiy / PVD) becomes a pure file
    consumer — no live-process coupling.

## 9. Alternatives rejected

### 9.1 Inline debug rendering inside `crd-eylem`

- Pro: simpler — no new module
- Con: violates module isolation (CLAUDE.md cornerstone); not reusable
  for SDF/audio/nav; couples renderer to eylem; rebuilds substrate per
  consumer (ODE drawstuff anti-pattern played out)

### 9.2 ImGui-only flat 2D overlay (no 3D world-space rendering)

- Pro: zero new GPU code
- Con: no 3D world-space rendering; can't visualize bodies in scene;
  useless for physics + SDF debugging; can't show shape geometry
- Useful only for HUD-style metric displays (kept for that role)

### 9.3 Triangle wireframe (use existing renderer + wireframe material)

- Pro: smaller — no new module
- Con: triangle wireframe limited to 1px line width on Vulkan; not
  "like pros"; can't AA; can't control thickness; no XRay mode

### 9.4 Geometry-shader line expansion

- Pro: less CPU vertex work
- Con: geometry shaders slow on modern GPUs (especially mobile / Apple
  Silicon); not portable to Metal-on-iOS

### 9.5 Embedded `static const u32[]` SPIR-V (ImGui pattern)

- Pro: zero runtime dep on `crd-shader`; no asset cooker step
- Con: violates Cerid quality bar (`feedback_quality_bar.md` —
  single-path, hook-based contracts); can't hot-reload; shader changes
  require module recompile; bypasses the hard-won reflection
  + variant infrastructure

### 9.6 New `IRenderPath` for debug viz

- Pro: fully self-contained
- Con: forces consumer to swap between scene render and debug render
  (can't have both); doesn't compose with ForwardRenderPath / future
  DeferredRenderPath
- Helper-function pattern (`add_draw_overlay_pass()`) composes
  cleanly with any IRenderPath

## 10. Open questions reserved for later

### 10.1 SDF text MTSDF baker

Bound to `crd-sdf` v2 (mesh-to-SDF baker). Until then, ImGui projection
is the day-one path. Reserved API: `draw_text_3d(world_pos, str,
{.size_world, .billboard, .occlude})`. Phase 3.1.5+.

### 10.2 Out-of-process replay viewer

Bound to Phase 7 editor. The data path is laid: serialize
`RenderBuffer` per frame; viewer is a separate executable. No
work needed in v1a-debug. Phase 7+.

### 10.3 Picking pipeline

Bound to Phase 7 editor. The slot is reserved (`u32 picking_id` in
`PrimFlags`). Click-to-select, hover-info, drag-handles all hang off
this. Phase 7+.

### 10.4 Vulkan secondary command buffer optimization

For very high primitive counts (>200k/frame), recording the overlay
pass into a secondary command buffer that can be re-recorded across
frames may pay back. Profile in eylem v1c-v1e benchmarking; revisit
only if measurements demand. Reserved.

### 10.5 GPU-side culling

Frustum-culling debug primitives on the GPU could win at very high
density. Probably overkill until SDF cell visualization (Phase 3.1.5+)
where cell counts spike. Reserved.

### 10.6 WBOIT switchable path for SDF cells

Decided: keep alpha-blend + sort default. Add WBOIT as a flag-toggled
fallback when SDF cell viz lands. Phase 3.1.5+.

## 11. Used by

- v1a-draw (Phase 3.1) — implements the substrate per ADR-0066.
  Sub-slices d0-d4.
- v1c+ (Phase 3.1) — eylem broadphase / narrow phase / solver use the
  substrate from day 1.
- v3 (Phase 3.1) — XPBD soft / cloth visualization.
- v4 (Phase 3.1) — articulation / ragdoll visualization (this is the
  user's headline use case).
- Phase 3.1.5 v1-v4 — SDF cell + isosurface + CSG visualization.
- Phase 3.4 — `crd-audio` acoustic occlusion + ray probe visualization.
- Phase 3.5+ — renderer light bounds + frustum + cluster + BVH viz.
- Phase 7 — editor selection + manipulator + picking.

## Sources

Industry survey delegated to a research agent. Primary sources:

- Bullet `btIDebugDraw.h`: https://github.com/bulletphysics/bullet3/blob/master/src/LinearMath/btIDebugDraw.h
- PhysX 5.4.1 Debug Visualization docs: https://nvidia-omniverse.github.io/PhysX/physx/5.4.1/docs/DebugVisualization.html
- PhysX 3.4 `PxRenderBuffer.h`: https://docs.nvidia.com/gameworks/content/gameworkslibrary/physx/apireference/files/PxRenderBuffer_8h-source.html
- Box2D `b2Draw.h`: https://github.com/google/liquidfun/blob/master/liquidfun/Box2D/Box2D/Common/b2Draw.h
- ODE `drawstuff` docs: http://opende.sourceforge.net/docs/group__drawstuff.html
- Unreal `DrawDebugSphere`: https://docs.unrealengine.com/en-US/API/Runtime/Engine/DrawDebugSphere/index.html
- Unreal `ULineBatchComponent`: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/Components/ULineBatchComponent
- Unreal HitProxy / Component Visualizers wiki: https://unrealcommunity.wiki/component-visualizers-xaa1qsng
- Unreal `FGameplayDebuggerCategory`: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/GameplayDebugger/FGameplayDebuggerCategory
- Unity `Gizmos.DrawWireSphere`: https://docs.unity3d.com/ScriptReference/Gizmos.DrawWireSphere.html
- Godot Debug Draw 3D add-on: https://github.com/DmitriySalnikov/godot_debug_draw_3d
- Godot Jolt debug crash issue: https://github.com/godotengine/godot/issues/101212
- Migdalskiy GDC 2014 — Debugging Physics: https://valvearchive.com/Presentations/GDC%202014/Migdalskiy_Sergiy_Physics_for_Game_02.pdf
- Migdalskiy GDC 2014 video: https://www.gdcvault.com/play/1020065/Physics-for-Game-Programmers-Debugging
- Frostbite FrameGraph: https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in
- idTech 7 / DOOM Eternal SIGGRAPH 2020: https://advances.realtimerendering.com/s2020/RenderingDoomEternal.pdf
- Three.js LineSegments2 / LineMaterial: https://threejs.org/docs/pages/LineSegments2.html
- Mapbox — Drawing Antialiased Lines with OpenGL: https://blog.mapbox.com/drawing-antialiased-lines-with-opengl-8766f34192dc
- Matt DesLauriers — Drawing Lines is Hard: https://mattdesl.svbtle.com/drawing-lines-is-hard
- Chan & Durand 2005 — Fast Prefiltered Lines (GPU Gems 2): https://developer.nvidia.com/gpugems/gpugems2/part-iii-high-quality-rendering/chapter-22-fast-prefiltered-lines
- McGuire & Bavoil 2013 — Weighted Blended OIT: https://jcgt.org/published/0002/02/09/paper.pdf
- WBOIT — Casual Effects blog: http://casual-effects.blogspot.com/2014/03/weighted-blended-order-independent.html
- Khronos OpenGL Wiki — Polygon Offset basics: https://www.khronos.org/opengl/wiki/Basics_Of_Polygon_Offset
- Aras Pranckevičius — Depth bias and the power of deceiving yourself: https://aras-p.info/blog/2008/06/12/depth-bias-and-the-power-of-deceiving-yourself/
- Chris Green / Valve 2007 — Improved Alpha-Tested Magnification (SDF text): https://steamcdn-a.akamaihd.net/apps/valve/2007/SIGGRAPH2007_AlphaTestedMagnification.pdf
- songho.ca — OpenGL Sphere reference: https://songho.ca/opengl/gl_sphere.html
- ROS 2 RViz User Guide: https://docs.ros.org/en/humble/Tutorials/Intermediate/RViz/RViz-User-Guide/RViz-User-Guide.html
- Lampert `debug-draw` repo: https://github.com/glampert/debug-draw
- sjb3d `imdd` — Immediate Mode Debug Draw: https://github.com/sjb3d/imdd
- ozz-animation: https://github.com/guillaumeblanc/ozz-animation
