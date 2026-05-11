# ADR-0066 — `crd-draw` substrate architecture

**Date:** 2026-05-10
**Status:** Accepted
**Tags:** [arch] [draw] [eylem] [sdf] [audio] [renderer] [editor] [resources]
**Research:** `docs/research/cerid-draw.md`

## Context

Eylem v1c+ (broadphase, narrow-phase, solver) needs visual debugging
from day 1. Going blind on broadphase + GJK/EPA + SI-solver instability
is a false economy — printf + faith does not scale to 1k bodies +
ragdoll articulations.

Scope quickly expands beyond eylem. The same shape-rendering machinery
is required by **at least seven shipped or planned modules**:

- **`crd-eylem`** (Phase 3.1 v1c+) — bodies, AABBs, contact normals,
  impulse arrows, island colors, joint frames, ragdoll articulations
- **`crd-sdf`** (Phase 3.1.5) — narrow-band cells, isosurface samples,
  CSG tree, closest-point overlays
- **`crd-audio`** (Phase 3.4) — acoustic ray probes, occlusion volumes,
  sound source bounds
- **Navmesh** (Phase 7) — polygons, portal edges, agent paths,
  RVO velocity obstacles
- **Editor** (Phase 7) — selection outlines, manipulator gizmos,
  per-tool overlays, brush strokes
- **Renderer** (Phase 3.5+) — light bounds, frustum, clusters, BVH,
  GI probe placement, shadow cascades
- **Sandbox** (already shipped) — per-entity inspection, transform
  handles, asset preview gizmos

Embedding debug rendering inside any single one of them duplicates the work
N-1 times. ODE's bundled `drawstuff` is the canonical anti-pattern of
embedding the renderer in the substrate; every real ODE user rebuilds
viz from scratch. PhysX's `PxRenderBuffer` is the canonical right
pattern: substrate emits primitives, the host paints pixels.

## Decision

Cerid ships **`crd-draw`** as a standalone substrate module — a
peer of `crd-renderer`, with a small upstream surface and many
downstream consumers. **The substrate IS the interface**; no consumer
implements its own line-quad-expansion shader or per-shape tessellator.
Same architectural posture as ADR-0062 (eylem) and ADR-0064 (sdf).

### 1. Module split

```
crd-draw                       ← this ADR locks
  └─ engine/draw/
       include/crd/draw/       — public headers
       src/                           — RenderBuffer, immediate-mode API,
                                        per-shape tessellators, overlay pass
       shaders/                       — GLSL source: line_aa, point, triangle,
                                        glyph_sdf (reserved Phase 3.1.5+)

crd-eylem-viz                      ← companion module (Phase 3.1 v1a-draw d3)
  └─ engine/eylem-viz/
       include/crd/eylem_debug/      — register_eylem_visualizers()
       src/                           — RigidBody / Collider / Joint visualizers

crd-sdf-debug                        ← companion module (Phase 3.1.5)
  (similar shape; bridges crd-sdf to crd-draw via VisualizerRegistry)

crd-audio-debug                      ← companion module (Phase 3.4)
  (similar shape)
```

The companion-module pattern is **dependency-inverted**: substrate
modules (`crd-eylem`, `crd-sdf`, `crd-audio`) emit pure data and have
*zero* rendering dependency. Companion modules (`crd-eylem-viz`,
etc.) bridge them to `crd-draw` and are otherwise empty.

### 2. Module dependencies (one-way, no cycles)

```
crd-draw depends on:
  crd-core         (types, asserts, platform macros)
  crd-memory       (IAllocator)
  crd-containers   (Array, HashMap, ConstSpan)
  crd-math         (Vec3f, Mat4f, AABB)
  crd-rhi          (backend-agnostic GPU interface)
  crd-renderer     (FrameGraph, ImageHandle, IRenderPath)
  crd-shader       (cooked SPIR-V loading + hot-reload)
  crd-resources    (ResourceManager + CRDR pack mounting)
  crd-scene        (ISystem + DebugVizComponent registration)
  crd-imgui        (day-one world-space text via projection)

crd-draw does NOT depend on:
  crd-eylem, crd-sdf, crd-audio, crd-jobs (no fanout-emission
                                            requirement at substrate level)
  (those depend on companion modules that bridge to us)
```

This ordering matters: substrate stays free of physics/sdf/audio
concerns; companion modules sit between them and the renderer. Eylem
v1+ can ship without `crd-draw` if needed (e.g. on a CI runner
without GPU). Companion module + substrate together turn it on.

### 3. Two-layer API

**Bottom layer — `RenderBuffer` (durable contract):**

```cpp
namespace crd::draw {

struct Color { u8 r, g, b, a; };  // packed RGBA8 (PhysX lesson)

struct PrimFlags {
    u32 depth_mode  : 2;   // Test / Always / XRay
    u32 category    : 4;   // Physics / Audio / SDF / Nav / Scene / User0..7
    u32 width_units : 1;   // 0 = pixels, 1 = world units
    u32 picking_id  : 16;  // 0 = no picking; reserved Phase 7 editor
    u32 reserved    : 9;
};

struct DebugPoint    { Vec3 pos;             u32 color; PrimFlags flags;
                       f32 size; f32 lifetime_s; };
struct DebugLine     { Vec3 a, b;            u32 color; PrimFlags flags;
                       f32 width; f32 lifetime_s; };
struct DebugTriangle { Vec3 a, b, c;         u32 color; PrimFlags flags;
                       f32 lifetime_s; };
struct DebugText     { Vec3 pos; const char* str; u8 size_px; u8 anchor;
                       u32 color; f32 lifetime_s; };

class RenderBuffer {
    void clear();
    void append(const RenderBuffer& other);   // for fan-out merge
    void shift(Vec3 origin_delta);                 // floating-origin support

    span<const DebugPoint>    points()    const noexcept;
    span<const DebugLine>     lines()     const noexcept;
    span<const DebugTriangle> triangles() const noexcept;
    span<const DebugText>     texts()     const noexcept;

    // Mutators called by immediate-mode API.
    void add_point(...); void add_line(...);
    void add_triangle(...); void add_text(...);
};
}
```

**Top layer — immediate-mode free functions (ergonomic sugar):**

```cpp
namespace crd::draw {
void line(Vec3 a, Vec3 b, Color c, f32 thickness_px = 1, f32 lifetime_s = 0);
void point(Vec3 p, Color c, f32 size_px = 4);
void box_wire(Mat4 transform, Vec3 half_extents, Color c, f32 thickness_px = 1);
void box_solid(Mat4 transform, Vec3 half_extents, Color c, f32 alpha = 0.3f);
void sphere_wire(Vec3 center, f32 radius, Color c, u32 segments = 16);
void sphere_solid(Vec3 center, f32 radius, Color c, f32 alpha = 0.3f);
void capsule_wire(Vec3 a, Vec3 b, f32 radius, Color c);
void capsule_solid(Vec3 a, Vec3 b, f32 radius, Color c, f32 alpha = 0.3f);
void aabb(AABB box, Color c, f32 thickness_px = 1);
void arrow(Vec3 origin, Vec3 dir, f32 length, Color c, f32 head_size = 0.1f);
void axis_triad(Mat4 transform, f32 size = 1.0f);          // RGB triad
void arc(Vec3 center, Vec3 axis, Vec3 zero_dir, f32 radius,
         f32 angle_min, f32 angle_max, Color c, u32 segments = 24);
void cross_3d(Vec3 center, f32 size, Color c);
void grid(Vec3 origin, Vec3 right, Vec3 forward,
          u32 cells_x, u32 cells_z, f32 cell_size, Color c);
void frustum(Mat4 view_proj, Color c);
void text(Vec3 world_pos, StringView str, Color c, u8 size_px = 14);

// Master scale (PhysX eSCALE) + category mask.
void set_master_scale(f32);  f32 master_scale();
void set_category_mask(u32); u32 category_mask();

// Lifecycle.
void frame_begin();
void frame_end();
}
```

State-machine API (Unity Gizmos.color/.matrix) **rejected**.
Parameter-passing per call is stateless-safe. Optional
`ScopedColor` / `ScopedTransform` RAII helpers in
`draw/scope.hpp` for ergonomics where they help.

### 4. Rendering — vertex-shader quad expansion + per-pixel AA

`GL_LINES` / `VK_PRIMITIVE_TOPOLOGY_LINE_LIST` rejected — Vulkan core
requires width=1.0 only; `wideLines` is an optional feature; AA is
hand-rolled and crude.

Algorithm (Three.js / Mapbox standard):
1. CPU emits each line as one instance of a unit quad with attributes
   `(start, end, color, flags, width)` — ~32 bytes/instance
2. Vertex shader transforms both endpoints to clip space, computes
   screen-space perpendicular, pushes vertex out by `width/2 * side`
   in **pixels** (default) or **world units** (per-flag toggle)
3. Fragment shader:
   `alpha = 1 - smoothstep(width/2 - 0.5, width/2 + 0.5, abs(distance_from_center))`

Pixel-perfect AA at any line angle, any thickness. Identical look on
every backend (Vulkan / D3D12 / Metal). ~15 lines of vertex shader,
~5 lines of fragment shader.

Geometry-shader expansion **rejected** — slow on modern GPUs (mobile,
Apple Silicon). Vertex-shader is the standard.

Chan & Durand 2005 prefiltered LUT lines **rejected** — overkill for
1-2px engine debug viz where per-pixel distance is indistinguishable.

### 5. Solid translucent rendering

- **Per-shape unit primitives precomputed at compile time** as
  `static const Vec3` arrays (icosphere subdivision 1 = 80 triangles,
  capsule 24×8, box 12 triangles). Total static const memory ~10-30
  KB. Zero per-frame tessellation cost.
- **CPU sort by centroid depth + standard alpha blend**, back-to-front
  per draw call. Hundreds of shapes per frame, sort cost negligible.
- **WBOIT (McGuire & Bavoil 2013) reserved as switchable path** for
  Phase 3.1.5+ SDF cell visualization where translucent cell counts
  spike. NOT enabled day 1.

### 6. Three depth modes per primitive

Packed in `PrimFlags::depth_mode` (2 bits):
- **`Depth_Test`** — standard depth test; primitive occluded by world
  geometry (default)
- **`Depth_Always`** — `depthCompareOp = VK_COMPARE_OP_ALWAYS`;
  always-on-top; for selection outlines, tagged actors
- **`Depth_XRay`** — dual-pass: first `depthCompareOp = GREATER` with
  dimmed color (occluded portion), second `LESS_OR_EQUAL` with full
  color (visible portion); reads as "I see the whole shape but can
  tell which part is behind walls"

Wireframe-over-solid uses pipeline with `depthBiasConstantFactor =
-1.0` to push wireframe slightly toward camera (prevents z-fighting).
Per-pipeline state — separate pipeline for biased rendering. Not a
global state knob.

### 7. Shape conventions (UV vs icosphere)

| Shape | Wireframe | Solid |
|---|---|---|
| Sphere | UV (16 long × 8 lat) | Icosphere subdivision 1 (80 tris) |
| Capsule | 16 circumferential, 2 hemispheres × 8 stacks | 24×8 |
| Cylinder | 16 segments | 24 |
| Cone | 16 base segments | 24 base, 4 cap |
| Box | 12 edges | 12 triangles |
| Circle | 32 segments | 32 tris fan |
| Arrow | 1 line + 4-segment cone for head | n/a |

UV sphere for wireframe (recognisable equator/axis). Icosphere for
solid (vertex regularity, better lighting if it's ever added).

### 8. Joint visualization conventions

- **RGB triad universally**: red = +X, green = +Y, blue = +Z (RViz /
  Maya / Houdini convention)
- **Articulation chains** as cone-segment connectors between joint
  origins (apex at child, base at parent)
- **Constraint frames** drawn as both parent and child triads
  overlaid, slightly offset, dimmed to show alignment error
- **Joint limits**: hinges as arcs from min-angle to max-angle in the
  constraint plane; ball joints as swing cone + twist arc; sliders as
  line with tick marks at min/max
- **Forces/impulses** as arrows with shaft-length proportional to
  `log(magnitude)` so order-of-magnitude differences fit on screen

### 9. Renderer integration via helper function (not new IRenderPath)

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

Grafts onto `ForwardRenderPath`, future `DeferredRenderPath`, future
`VisibilityBufferRenderPath` — any `IRenderPath` calls it after its
own passes. Avoids the new-IRenderPath alternative which would force
consumers to swap between scene render and debug render (can't have
both).

The overlay pass runs in order:
1. Solid triangles (back-to-front sorted, alpha-blended)
2. Lines (vertex-shader quad expansion, AA fragment)
3. Points (small AA quads)
4. Glyphs (only if SDF text path is active; ImGui handled separately)

### 10. Asset shipping — single-path via crd-shader

GLSL source files live in `engine/draw/shaders/`. Asset cooker
compiles them to SPIR-V at build time. CRDR pack
`cooked_assets/draw_shaders.crdr` is mounted at
`crd::draw::init(rm)`. Hot-reload via the existing
`crd-shader::Runtime::reload_effect()` watch system.

**ImGui-style embedded `static const u32[]` SPIR-V arrays rejected** —
violates Cerid's elite-quality bar (`feedback_quality_bar.md`:
single-path, no dual demo/real). Reusing `crd-shader` end-to-end gives
hot-reload + reflection + variant infrastructure for free.

### 11. ECS integration

Single `DebugVizSystem : ISystem` registered in **`PostRender`
phase** (phase 6 of 7). Per-entity opt-in via:

```cpp
struct DebugVizComponent {
    bool wireframe   : 1;     // wireframe outline
    bool solid       : 1;     // translucent solid fill
    bool show_velocity : 1;   // arrow from body origin
    bool show_aabb   : 1;     // broadphase AABB
    bool show_joints : 1;     // joint markers + chain lines
    bool show_contacts : 1;   // contact normals + points
    bool highlight   : 1;     // tinted overlay (selection)
    f32  solid_alpha;
    Color tint;
};
```

Default-on for new entities (avoids the PhysX 3-AND opt-in footgun).
Disable per-entity for surgical control.

### 12. Per-component visualizer plug-in registry

Companion modules register visualizers for their component types:

```cpp
namespace crd::draw {
template <typename T>
using ComponentVisualizer = void(*)(const T& component, const Transform&,
                                    RenderBuffer&, const VizConfig&);

class VisualizerRegistry {
    template <typename T> void register_visualizer(ComponentVisualizer<T> fn);
    void run_all(const World& world, RenderBuffer& out, const VizConfig&);
};
}
```

`crd-eylem-viz::register_eylem_visualizers(VisualizerRegistry&)`
plugs in `RigidBody`, `Collider`, `Joint` visualizers. Each visualizer
reads the component, calls immediate-mode API.

### 13. Master scale + categories

PhysX `eSCALE` lesson:
- Global `f32 master_scale` (default 1.0) multiplies every primitive
  size — one ImGui slider toggles the entire visualization scale
- 7 named categories (`Physics`, `Audio`, `SDF`, `Nav`, `Scene`,
  `Renderer`, `User`) packed in 4 bits
- Per-frame category mask filters primitives at submit time — toggle
  Physics off without losing AABBs by category, etc.
- Profile-system gated (per ADR-0060): `dev` profile = overlay ON,
  `shipping` / `cinematic` profiles = overlay OFF (zero CPU overhead
  via early-out in `frame_begin()`)

### 14. Lifetime per primitive (Unreal-style)

`f32 lifetime_s` per primitive:
- 0.0 = single-frame (default)
- > 0.0 = decay over time, alpha-fades in last 0.25 seconds
- Replaces persistent-vs-instant distinction at the API surface

Persistent-handle alternative (Godot `MeshInstance3D` pattern)
**reserved** for shapes that don't change frame-to-frame:

```cpp
DebugDrawHandle h = persistent_box(transform, half_extents, color);
h.update_color(new_color);   // mutate without re-emit
h.destroy();                  // remove
```

Backed by the same `RenderBuffer` at a stable index. Day-one
implementation: lifetime-based persistence is sufficient. Handle path
implemented when a real consumer demands it.

### 15. Picking ID slot reserved (Unreal HitProxy)

`PrimFlags::picking_id` is a 16-bit slot, default 0 (no picking). Day
1 unused. Phase 7 editor wires it: click → ray-cast through debug
buffer → identify primitive by `picking_id` → resolve to ECS entity.

### 16. Day-one text — ImGui projection; SDF text reserved

`draw_text(world_pos, str)` projects world position to screen via
`Camera::project()`, calls ImGui draw-list at that pixel. Always
pixel-perfect. No SDF baker needed. Text is not in the 3D scene (no
depth occlusion, no transform-with-camera-roll) — acceptable for day
1; physics debugging primarily wants identifiers + state values, not
mood lighting.

`draw_text_3d(world_pos, str, {.size_world, .billboard, .occlude})`
**reserved for Phase 3.1.5+** when `crd-sdf` ships MTSDF (Chris
Green / Valve 2007 SIGGRAPH). Don't build the SDF path until the SDF
substrate exists.

### 17. Replay-friendly via retained buffer

`RenderBuffer` is plain data. `serialize(buffer)` writes the
entire frame's debug primitives to disk. During deterministic replay
capture (ADR-0063), the buffer is dumped per-frame alongside the
physics replay log. Phase 7+ replay viewer consumes both streams.

This is the Migdalskiy out-of-process pattern (Source 2 GDC 2014),
achieved naturally because we chose retained-buffer over
immediate-callback.

### 18. Performance budget

Eylem v1+ target: 1k bodies @ 60 Hz, debug viz on.

Worst-case per-frame counts:
- ~100k lines (1k bodies × 90 lines each: collider wireframe + velocity
  arrow + axis triad + every-other-frame AABB)
- ~10k points (~1500 contacts × ~1 point + headroom)
- ~5k triangles (translucent solid fills for selected entities)

Vertex memory with instancing:
- Lines: 100k × 32 bytes/instance = 3.2 MB
- Triangles: 5k × 96 bytes = 0.5 MB
- Points: 10k × 32 bytes = 0.3 MB

Total GPU upload ~4 MB/frame at peak. Well under the budget.

CPU cost: per-primitive `add_line()` ~100 ns (push_back + flag pack).
100k primitives = ~10 ms — too slow on one thread. **Fan-out via
`parallel_for` over entity chunks; each chunk has thread-local
`RenderBuffer`; merge at end of phase via `append()` in
deterministic order.** Matches the eylem job pattern (ADR-0063).

### 19. Substrate solidification decisions (locked 2026-05-10 mid-d2)

After shipping d0+d1+d2 the user requested locking 7 architecture
choices to make the substrate genuinely future-proof for Phase 7
editor manipulators + multi-domain consumers (eylem v1c+ at 100k
primitives/frame, sdf v3 narrow-band cells, audio occlusion overlays).
The chosen options below maximise long-term leverage even where the
short-term implementation is more involved.

#### 19.1 DepthMode pipeline strategy: 6-pipeline matrix

Build 6 pre-baked pipelines = `(line, triangle) x (Test, Always,
GreaterDimmed)`. At submit time the overlay-pass execute lambda bins
each primitive by `flags.depth_mode` into one of 4 buckets:

- `Depth_Test`         -> `[primitive_kind][Test]`
- `Depth_Always`       -> `[primitive_kind][Always]`
- `Depth_XRay` emits TWICE: once into `[*][GreaterDimmed]` (color
  alpha-multiplied by ~0.3 at submit) AND once into `[*][Test]` (full
  color, depth-tested so only the visible portion shows).

Why pre-baked over `VK_EXT_dynamic_state2`: extension support is
weak on Apple Silicon + older ARM + WSL2 GPU passthrough; pre-baked
pipelines are universal Vulkan 1.3 baseline. Pipeline switches are
microseconds; 6 switches per frame is noise.

#### 19.2 VisualizerRegistry: typed function-pointer with category iteration

```cpp
template <typename T>
using Visualizer = void(*)(const T&, const Transform&,
                           RenderBuffer&, const VizConfig&);

class VisualizerRegistry {
    template <typename T> void register_visualizer(Visualizer<T> fn);
    void run_all(const World&, RenderBuffer&, const VizConfig&) const;
};
```

Per-category iteration order (Physics -> Audio -> SDF -> Nav -> Scene
-> Renderer -> User0..User2 -> Debug -> Gizmo -> Brush) matches the
Category enum (decision 13) so UI filtering is predictable.

Companion modules (`crd-eylem-viz`, `crd-sdf-viz`, etc.) call
`register_with(reg)` once at app init. Direct bulk emission (eylem
v1c+ broadphase calls `add_line_to(buf, ...)` on per-fiber buffers)
remains supported and bypasses the registry; both paths coexist.

Rejected: std::function (overhead + allocations on registration);
CRTP (template ugliness, harder downstream registration).

#### 19.2.1 Reserved diagnostic-viewer hook category (added 2026-05-11)

Per coverage audit §1.13, the **solver-convergence viewer** (penetration
depth heat-map, contact-force arrows, per-iteration residual plot,
island-boundary outlines) ships as a `Category::Diagnostic` registry
slot reserved here. Concrete impl arrives with the eylem v1k sandbox
demo + Phase 7 editor integration. The Diagnostic category value is
reserved at slot 12 (above the existing 0-11 closed enum, with the 4
remaining bits providing 16-slot headroom for future diagnostic
viewers — replay-snapshot scrubber UI, profiler ribbon, etc.). The
existing per-category iteration order extends to Diagnostic last
(rendered on top of all other viz).

#### 19.3 Current-buffer pattern: explicit canonical + thread-local convenience

Two-tier API:
- **Canonical**: every primitive function has a `*_to(buf, ...)` form
  taking an explicit `RenderBuffer&`. Stateless-safe; supports
  per-fiber fan-out emission.
- **Convenience wrappers**: thin `line(a, b, color)` / `box_wire(...)`
  forms that route to `active_buffer()` (a thread-local pointer).
  Consumers install one via `set_active_buffer(buf)`. Calling a
  convenience wrapper without an active buffer is `CRD_ASSERT` in
  debug, no-op in release.

Best of both: physics solver fan-out emission stays clean (explicit
threading); editor + dev-console one-liner calls become ergonomic
(`crd::draw::line({0,0,0}, {1,1,1}, kRed);`). Avoids the Unity
`Gizmos.color = ...` global-state footgun (decision via Unity lesson
in section 2.2 above).

#### 19.4 Buffer overflow: multi-batch submit (no truncation, no auto-grow)

When `N > max_per_frame`, the overlay pass loops over batches:
```cpp
for (offset = 0; offset < N; offset += cap) {
    map -> fill batch [offset, min(offset+cap, N)) -> unmap
    bind_vertex_buffer + draw_instanced(batch_size)
}
```

Same buffer reused per batch. Eylem v1c+ visualising 100k+ AABBs
runs unbounded with no warning, no truncation, no GPU memory churn.
The `max_per_frame` cap becomes "minimum batch size" rather than
"max work."

Rejected: hard-cap-and-warn (loses primitives when needed most),
auto-grow (complex GPU memory recreation mid-frame).

#### 19.5 MSAA quality tier mapping (locks d2-aa scope)

**Status (2026-05-10):** d2-aa execution deferred until a real MSAA consumer exists (e.g. scene-wide MSAA in a later phase). The mapping below stays as the design of record; only the *when* slipped, not the *what*.

| Tier   | Strategy                                              | VRAM cost |
|--------|-------------------------------------------------------|-----------|
| Off    | `add_draw_overlay_pass` no-ops entirely               | 0         |
| Low    | Per-pixel distance AA only (current d0 behavior)      | 0         |
| Med    | 2x MSAA overlay attachment + resolve to scene_color   | ~2 MB     |
| High   | 4x MSAA overlay + resolve                             | ~4 MB     |
| Ultra  | 8x MSAA + reserved slot for temporal accumulation     | ~8 MB     |

`QualityPreset` schema bumps v1 -> v2; new `crd::u8 debug_overlay_aa`
field (5-value enum). v1 loaders default the new field to `Low` for
backward compatibility.

#### 19.6 API surface freeze at d4 close

When v1a-draw d4 (ImGui control panel + 1k-box ragdoll demo + the
final substrate piece) closes, the public API of `crd-draw` is
**frozen**. No breaking changes ever after. This commitment exists
because Phase 7 editor builds manipulator gizmos against this
surface; future Cerid modules build against it. Consumers can
trust the API across the rest of the engine's lifetime.

Internal types in `crd::draw::detail::` namespace stay free to
change.

#### 19.7 Replay capture: API surface reserved, impl deferred to Phase 7

Add `crd::draw::serialize_render_buffer(const RenderBuffer&,
std::ostream&)` as a no-op stub in d4. Real binary wire format +
out-of-process replay viewer (Migdalskiy/PVD pattern from section
2.4) lands in Phase 7 editor. Substrate API surface stays stable
across the deferral.

## Consequences

### Positive

- **Substrate-quality debug viz from day 1 of eylem v1c.** Broadphase,
  GJK, solver are debuggable visually instead of via printf + faith.
- **Reused across 7+ consumers** with zero per-consumer
  rendering-pipeline duplication.
- **Replay-determinism preserved.** Buffer is plain data, serializes
  cleanly, replay capture is ~free.
- **Hot-reload Just Works** — shaders go through `crd-shader`'s watch
  system, same as every other shader in the engine.
- **Profile-gated zero-cost in shipping** — `dev` profile turns it on,
  `shipping` profile early-outs in `frame_begin()`.
- **Companion-module pattern** keeps substrate modules
  (eylem/sdf/audio) free of any rendering deps. They emit pure data;
  companion modules bridge to the debug renderer.

### Negative

- **One more module to ship.** ~2500 LOC + 4 GLSL shaders + ~30
  tests across 5 sub-slices over ~7-9 days. Delays eylem v1b by that
  amount. Mitigated by the multi-consumer payback.
- **Companion module per substrate** (`crd-eylem-viz`, etc.) is a
  small but real per-consumer cost. Each is ~100-300 LOC.
- **No GPU-side culling day 1.** At >200k primitives/frame the
  overlay pass becomes a real cost. Reserved for Phase 3.1.5+ SDF
  cell viz where it matters; eylem won't hit it.
- **No out-of-process replay viewer day 1.** Data path is laid (the
  serializable buffer) but the viewer executable is Phase 7+.

### Neutral

- Picking pipeline reserved but unused day 1. Slot is in `PrimFlags`
  (`picking_id : 16`).
- SDF text reserved but unused day 1. ImGui projection is the
  workhorse until `crd-sdf` MTSDF lands.
- WBOIT reserved but unused day 1. Sort-by-centroid + alpha blend is
  sufficient for hundreds of debug shapes.

## Alternatives considered

### A. Inline debug rendering inside `crd-eylem`

Rejected. Violates module isolation (CLAUDE.md cornerstone). Not
reusable for SDF / audio / nav. Couples the renderer to eylem.
Reproduces the ODE drawstuff anti-pattern.

### B. ImGui-only flat 2D overlay (no 3D world-space rendering)

Rejected. Useless for visualizing 3D shapes. No depth integration.
Useful only for HUD-style metric displays — that role kept (we
already have ImGui for it), but it's not a debug renderer.

### C. Triangle wireframe via existing renderer + wireframe material

Rejected. Triangle wireframe limited to 1 px line width on Vulkan
(`wideLines` is optional). Can't AA. Can't control thickness. No
XRay mode. Doesn't meet "beautiful" quality bar.

### D. Geometry-shader line expansion

Rejected. Geometry shaders slow on modern GPUs (especially mobile /
Apple Silicon). Vertex-shader quad expansion is the standard.

### E. Embedded `static const u32[]` SPIR-V arrays (ImGui pattern)

Rejected. Violates Cerid quality bar (`feedback_quality_bar.md` —
single-path, hook-based contracts). Can't hot-reload. Bypasses the
hard-won reflection + variant infrastructure in `crd-shader`. Saves
zero meaningful build complexity (the asset cooker step is already
in place).

### F. New `IRenderPath` for debug viz

Rejected. Would force consumers to swap between scene render and
debug render (can't have both simultaneously). Helper-function
pattern composes cleanly with any `IRenderPath`.

### G. Pure callback / immediate interface (Bullet `btIDebugDraw` style)

Rejected as the *primary* pattern. Each call would be a virtual call
into the host renderer; no batching control; no replay path. Adopted
as the *top-layer ergonomic API* on top of the retained
`RenderBuffer` — best of both: the retained buffer is the
durable contract, the immediate-mode functions are sugar.

## Implementation plan

Phase 3.1 v1a-debug, 5 sub-slices over ~7-9 days, ~2500 LOC, ~30 tests:

| Sub-slice | What | Days | LOC | Tests |
|---|---|---:|---:|---:|
| **v1a-d0** | Module skeleton + line + box wireframe primitives + screen-space quad shader + RenderBuffer + add_draw_overlay_pass | 3 | ~1000 | ~12 |
| **v1a-d1** | Solid pipeline + icosphere/capsule/box triangle tessellators + sphere_solid/capsule_solid/box_solid | 2 | ~600 | ~8 |
| **v1a-d2** | Full immediate-mode API: arrow / axis_triad / arc / cross_3d / grid / frustum / aabb + per-shape tessellator caches | 1 | ~400 | ~6 |
| **v1a-d3** | DebugVizSystem + DebugVizComponent + VisualizerRegistry + crd-eylem-viz companion module (registers visualizers) | 2 | ~400 | ~5 |
| **v1a-d4** | ImGui control panel + sandbox demo (1k box ragdoll falling) + sandbox-smoke verification | 1 | ~150 | smokes |

Slots between v1a (eylem interface, ~600 LOC, interface only) and v1b
(eylem AoSoA storage). v1c+ uses the substrate from day 1.

## References

- `docs/research/cerid-draw.md` — research dossier with full
  industry survey
- ADR-0017 — IRenderPath interface
- ADR-0033 — `crd-jobs` substrate
- ADR-0049 — `crd-scene` ECS architecture
- ADR-0060 — Profile system (overlay gating)
- ADR-0061 — Async GPU upload contract (`UploadHandle`)
- ADR-0062 — Eylem physics architecture
- ADR-0063 — Eylem determinism contract (replay path)
- ADR-0064 — `crd-sdf` substrate (future consumer)
- `feedback_quality_bar.md` — single-path / no-dual-paths principle
  (rejects ImGui embedded-SPIR-V pattern)
- `feedback_sandbox_always_built.md` — sandbox built every config
  (rejects Unity editor-only pattern)
- `feedback_full_sweep_required.md` — DoD sweep required at slice
  closure
