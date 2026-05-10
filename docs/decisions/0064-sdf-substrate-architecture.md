# ADR-0064 — `crd-sdf` substrate architecture

**Date:** 2026-05-10
**Status:** Accepted
**Tags:** [arch] [sdf] [eylem] [renderer] [font] [audio] [editor] [resources]

## Context

Signed distance fields (SDFs) are required by **at least four shipped or
planned modules**:

- **`crd-eylem`** — robust mesh colliders + closest-point queries for soft
  body / FEM contact (Drake-hydroelastic, MuJoCo signed-distance contact,
  FleX/Vellum, NVIDIA Newton).
- **`crd-font`** (Phase 3.3) — MTSDF glyph atlases (ADR-0047 already
  commits the renderer to MTSDF text).
- **`crd-renderer`** (Phase 3.5+) — Distance Field Ambient Occlusion,
  Distance Field soft shadows, cone-traced GI in the Lumen-non-RT mould.
- **`crd-audio`** (Phase 3.4) — voxelised scene SDF for acoustic
  occlusion / reverb baking (Steam Audio, Phonon model).
- **Editor / authoring** (Phase 7) — SDF modelling (Dreams, MagicaCSG,
  Claybook) needs CSG ops + extraction.

Letting each consumer roll its own SDF representation would fragment
storage, baker, sampler, GPU upload path, and CRDR persistence across
four sub-implementations with subtly incompatible behaviour. Robotics
and medical domains both treat SDF colliders as table stakes; the
multi-domain mandate makes a unified substrate non-negotiable.

## Decision

Cerid ships **`crd-sdf`** as a standalone substrate module — a peer of
`crd-math` and `crd-eylem`, with minimal upstream deps and many
downstream consumers. **The substrate IS the interface**; no consumer
wraps a third party. (Same posture as the eylem decision in ADR-0062.)

### 1. Module split

```
crd-sdf                              ← this ADR locks
  └─ engine/sdf/
       include/crd/sdf/             — public headers
       src/                          — backends (analytic, dense, narrow-band, CSG)
       (later, gated slices)
         crd-sdf-extract/            — Marching Cubes + Dual Contouring
         crd-sdf-vdb/                — VDB-like sparse hierarchy (very-large-scene)
```

Reserved sub-modules (`extract`, `vdb`) ship as additional namespaces
when consumer demand surfaces; they do not block the v1 substrate.

### 2. Module dependencies (one-way, no cycles)

```
crd-sdf depends on:
  crd-core         (types, asserts, platform macros)
  crd-memory       (IAllocator)
  crd-containers   (Array, HashMap, ConstSpan, sort)
  crd-math         (Vec3f, Mat4f, AABB, SIMD wrappers from Phase 3.1 v0)
  crd-jobs         (parallel_for for baking + parallel sampling)

crd-sdf does NOT depend on:
  crd-rhi, crd-renderer, crd-scene, crd-eylem, crd-resources
  (those depend on us; resource loader registration is a glue layer
   inside crd-resources, not the other way around)
```

The GPU upload path uses **the existing `GpuUploader::upload_texture3d_async`**
from `crd-renderer` (added in v5 of this phase). `crd-sdf` itself stays
RHI-agnostic — it produces the bytes; the uploader moves them.

### 3. Storage backends (closed set, four kinds)

| Backend | When | Purpose |
| --- | --- | --- |
| **Analytic** | always | Sphere / Box / Capsule / Plane / Torus / Cylinder primitives. Zero memory. Closed-form sampling + closed-form gradient. |
| **Dense grid** | v1 | `N_x × N_y × N_z` `f16`/`f32` voxel lattice. Trilinear sample. CRDR-persisted. GPU 3D texture path. |
| **Narrow-band sparse** | v3 | Tile-based (8³ tiles); only tiles intersecting the surface ±band are allocated. Outside tiles fall through to a constant. ~5–10× memory savings vs dense for typical meshes. |
| **CSG tree** | v4 | Composed expression of analytic + sampled SDFs with min / max / smooth-min / difference operators. Stateless; sampling walks the tree. |

VDB-like sparse hierarchy reserved for v8+ if very-large-scene
(km-scale terrain, full-building) demand surfaces. **Not v1 work.**

### 4. Mesh → SDF baker

**Algorithm:** closest-point per voxel + **generalized winding number**
for the sign (Jacobson, Kavan & Sorkine-Hornung 2013, "Robust
inside-outside segmentation using generalized winding numbers"). This is
the right choice because:

- Robust on **non-watertight** meshes — Cerid imports glTF, FBX, USD,
  OBJ, scan data, CAD; few real-world assets are watertight.
- Robust on **self-intersecting** meshes (game and CAD content both
  produce these regularly).
- Differentiable — reserved hook for Phase 3.1 v9 differentiable physics.

Rejected alternatives:

- **Ray-cast intersection counting** — fragile on bad meshes; fails on
  non-watertight; standard pre-2013 approach but superseded.
- **Pseudo-normal angle-weighted** (Bærentzen 2005) — fast but assumes
  watertight. Reserved as an opt-in fast-path for known-clean assets.

Closest-point search uses a **BVH built per mesh** (reuses the dynamic
AABB tree from `crd-eylem`'s broadphase — same data structure, same
test surface). Fan-out via `crd-jobs::parallel_for` over voxels.

### 5. Sampling + gradient

- **Trilinear interpolation** for sample (standard; matches GPU
  hardware sampling exactly when the same `f16`/`f32` format is used).
- **Central differences** for gradient: `(d(p+h) - d(p-h)) / (2h)`
  with `h = voxel_size`. Tetrahedron-method (Akinci 2011) reserved for
  v8 if numerical instability appears in soft-body contact normals.

### 6. CSG operators (closed set)

| Op | Formula | Notes |
| --- | --- | --- |
| Union | `min(a, b)` | trivial |
| Intersection | `max(a, b)` | trivial |
| Difference | `max(a, -b)` | A minus B |
| Smooth union | Quílez polynomial smin | continuous gradient at the seam |
| Smooth intersection | Quílez polynomial smax | symmetric |

CSG operates uniformly on analytic and dense backends through the
`Sdf` interface (`evaluate(p) → f32`, `gradient(p) → Vec3f`).

### 7. GPU path

- **Format:** `R16Sfloat` 3D texture (Vulkan
  `VK_FORMAT_R16_SFLOAT`) — half precision is sufficient for typical
  SDF dynamic range; halves VRAM vs `R32Sfloat`. Opt-in `R32Sfloat`
  reserved for high-precision physics SDFs.
- **Sampling:** linear filter + clamp-to-edge addressing. The clamp
  wall outside the grid extends the boundary value, which composes
  cleanly with grid padding (every bake adds `padding_voxels` of
  outside-air around the bounds).
- **Upload:** `GpuUploader::upload_texture3d_async(SdfGridSpan,
  GpuTexture3D)` — added in v5; reuses ADR-0061's `UploadHandle` /
  `Fence` infrastructure exactly.
- **Shader-side helper:** `assets/shaders/lib/crd_sdf.glsl` ships
  `sample_sdf(sampler3D, vec3 world_pos, mat4 inv_world) → float` +
  `sample_sdf_gradient` + `cone_trace_sdf` so consumers do not
  re-derive the lookup math.

### 8. Resource integration

- **`SdfResource`** payload (in `crd-resources`-registered loader,
  but the type lives in `crd-sdf`). FourCC `'SDFR'`. CRDR chunks:
  `SINF` (header — bounds, dims, voxel_size, format) + `SDFG` (raw
  voxel bytes) + optional `SDFA` (analytic-overlay description for
  hybrid analytic+grid SDFs, e.g. for character capsules baked into
  an environment grid).
- **Loader:** `SdfResourceLoader` registered with the existing
  `LoaderRegistry`. Same pattern as `MeshResourceLoader`,
  `TextureResourceLoader`.
- **Cooker:** `tools/asset_cooker/` recognises `.sdf.toml` (standalone
  SDF asset) and adds a `bake_sdf = true` opt-in to mesh assets to
  emit SDFG alongside the mesh artifact.

### 9. Determinism contract

`crd-sdf` inherits the **eylem determinism contract (ADR-0063)** —
the baker, the sampler, and CSG ops use only the deterministic stdlib
substitutions (`crd::math::deterministic` for trig / exp / sqrt;
`crd::containers::sort` for stable sort; FNV-1a 64 for any hash).

This is required because:

- Eylem's SDF colliders are sampled inside the deterministic physics
  step; non-determinism in the sampler would break replay-hash CI.
- `bake_mesh_to_grid` is called both at cook time (offline) and at
  runtime (procedural meshes); both paths must produce **bit-exact
  identical** voxel bytes for identical inputs across compilers /
  platforms / SIMD widths.

### 10. Threading model

`crd-sdf` integrates into `crd-jobs` with the same posture as `crd-eylem`
(ADR-0062 §7): **never spawns its own threads**. Baking, parallel
sampling, and CSG evaluation fan out through `parallel_for` and
`run_and_wait`. From the schedule's perspective, the SDF baker is one
atomic phase boundary; internally it's saturated parallel.

### 11. Versioning + format freeze

Every cooked artifact carries (FourCC, schema_version, payload_size).
The cooker writes versions; the loader pins them with `static_assert`
(matching the v1p Phase 3.0 freeze pattern):

- `'SDFR'` schema v1
- `'SINF'` chunk v1: 64 B fixed header
- `'SDFG'` chunk v1: raw voxel payload (size = N_x × N_y × N_z × bytes_per_voxel)
- `'SDFA'` chunk v1: analytic overlay descriptor (reserved; v4)

Bumps require a deliberate schema break visible in source; loader
rejects mismatched versions with `LoadState::Failed`.

## Consequences

**Positive:**

- One substrate, one baker, one sampler, one GPU path — consumed by
  4+ modules through a single typed surface.
- Robotics and medical domains get the SDF colliders they need without
  per-domain forks.
- The renderer's DFAO / DFGI work in Phase 3.5+ inherits the substrate
  for free; same for audio's acoustic occlusion in 3.4.
- Cooker integration matches every other Cerid asset (`MeshResource`,
  `TextureResource`, `MaterialResource`) — no new authoring pattern.
- Determinism contract piggybacks on ADR-0063; eylem's replay-hash CI
  catches SDF baker regressions automatically.

**Negative:**

- **One more module to maintain.** ~5–6 weeks of substrate work before
  any consumer ships its first SDF feature.
- **Memory cost.** A 256³ grid is 32 MB at `R16` / 64 MB at `R32`.
  Mitigated by narrow-band (v3) and per-mesh sparse storage; not
  catastrophic but real.
- **Cooker complexity.** SDF baking is the most expensive cook step
  Cerid will run (CPU-baker is O(voxels × triangles) before BVH
  acceleration; with BVH it's O(voxels × log triangles)). Mitigated
  by parallel_for over voxels and asset-cooker caching.
- **GPU 3D texture pressure.** Every per-mesh SDF mirrored to GPU is
  another 3D texture; memory budget needs explicit accounting in the
  ResourceManager budget. v3 narrow-band reduces this 5–10×.

**Insertion point:**

`crd-sdf` slots in as **Phase 3.1.5** between Phase 3.1 (eylem rigid
3D + 2D done; before XPBD soft) and Phase 3.2 (animation). This
ordering means:

- Eylem v3 (XPBD soft / cloth / rope) lands **after** `crd-sdf` and can
  use SDF environment colliders from day 1.
- `crd-font` (Phase 3.3) consumes the substrate for MTSDF.
- `crd-audio` (Phase 3.4) consumes it for acoustic occlusion baking.
- Renderer DFAO / DFGI (Phase 3.5+) consume it.
- Editor (Phase 7) consumes CSG + extraction.

Phase plan: `docs/phases/phase-3.1.5-sdf.md`.
Research: `docs/research/cerid-sdf.md`.

## References

- Jacobson, Kavan & Sorkine-Hornung (2013) — *Robust inside-outside segmentation using generalized winding numbers*. SIGGRAPH.
- Bærentzen & Aanaes (2005) — *Signed distance computation using the angle weighted pseudo-normal*. IEEE TVCG.
- Lorensen & Cline (1987) — *Marching Cubes*. SIGGRAPH.
- Ju et al. (2002) — *Dual Contouring of Hermite Data*. SIGGRAPH.
- Quílez (2008+) — *smooth minimum* family. iquilezles.org/articles/smin/.
- Karras (2012) — *Maximizing Parallelism in the Construction of BVHs, Octrees, and k-d Trees*. HPG. (LBVH baker for GPU SDF in v8.)
- Akinci, Ihmsen et al. (2011) — *Tetrahedral gradient* — used in PCISPH; reserved gradient method for v8.
- ADR-0061 — Async GPU upload contract (`UploadHandle` + `Fence` reused for `upload_texture3d_async`).
- ADR-0062 — Eylem physics architecture (this ADR mirrors its module-split + ECS-native + jobs-integration posture).
- ADR-0063 — Eylem determinism contract (this ADR inherits it wholesale).
- ADR-0047 — Font rendering (the original MTSDF commitment that motivates one of the four consumers).
