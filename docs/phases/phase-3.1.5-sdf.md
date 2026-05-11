# Phase 3.1.5 — `crd-sdf` substrate

**Status:** ⏳ Planned (interleaved between Phase 3.1 eylem v2 and
Phase 3.1 v3 XPBD soft, then runs in parallel with v3+). **Slot
unchanged by the ADR-0076 §12 amendment (2026-05-11)**, but v2
mesh-bake now consumes `crd-geometry-mesh` + `crd-geometry-bvh`
directly from day 1 — the original "ship own narrow BVH +
winding-number, refactor later" plan (ADR-0064 §4) is OBSOLETE.
**Estimated duration:** ~5–6 weeks.
**Locked architecture:** ADR-0064 (amended consumer pattern per
ADR-0076 §12).
**Research:** `docs/research/cerid-sdf.md`.

## Why this phase exists

Signed distance fields are needed by at least four shipped or planned
modules: `crd-eylem` (mesh colliders + closest-point), `crd-font`
(MTSDF glyph atlases), `crd-renderer` (DFAO / DF soft shadows /
DF cone-trace GI in 3.5+), `crd-audio` (acoustic occlusion baking in
3.4), plus the editor (Phase 7) for CSG modelling.

Per-consumer rolls would fragment storage, baker, sampler, GPU path,
and CRDR persistence into four sub-implementations. The multi-domain
mandate (games + robotics + medical + cinematic + DAW) makes a unified
substrate non-negotiable. ADR-0064 locks the decisions; this file is
the slice plan.

## Slice structure

Eight slices over ~5–6 weeks. v0–v5 are the substrate (consumed
externally). v6–v7 are extraction + cooker glue. v8 is reserved for
GPU-side baking and VDB-like sparse hierarchy when consumer demand
surfaces (Phase 3.5+ renderer or Phase 3.8 GPU-driven path).

| Slice | Topic | Est. LOC | Tests | Duration |
| :---: | --- | :---: | :---: | :---: |
| v0 | Module scaffolding + analytic primitives | ~600 | 12 | ~3 days |
| v1 | Dense 3D grid backend + CRDR persistence | ~700 | 14 | ~5 days |
| v2 | Mesh → SDF baker (CPU, BVH-accelerated, parallel) | ~900 | 16 | ~7 days |
| v3 | Narrow-band sparse storage | ~700 | 12 | ~5 days |
| v4 | CSG operators + `SdfTree` composition | ~500 | 14 | ~3 days |
| v5 | GPU 3D texture upload path + GLSL helper | ~600 | 10 | ~4 days |
| v6 | Cooker integration (`.sdf.toml` + `bake_sdf` opt-in) | ~500 | 8 | ~4 days |
| v7 | Marching Cubes extraction (SDF → MeshResource) | ~600 | 10 | ~4 days |
| v8 | Reserved (GPU baker, VDB sparse, Dual Contouring) | — | — | deferred |

Total active substrate: ~4500 LOC, ~96 tests, ~5–6 weeks.

---

## v0 — Module scaffolding + analytic primitives (~3 days)

**Goal:** `crd-sdf` exists as a module, with analytic primitives that
sample correctly. Zero storage, zero baker. Pure unit-test fodder.

**Public surface:**

```cpp
namespace crd::sdf
{
    // Stable interface every backend implements.
    class ISdf
    {
    public:
        virtual ~ISdf() = default;
        [[nodiscard]] virtual f32        evaluate(Vec3f p) const noexcept = 0;
        [[nodiscard]] virtual Vec3f      gradient(Vec3f p) const noexcept = 0;
        [[nodiscard]] virtual math::AABB bounds()        const noexcept = 0;
    };

    // Analytic primitives.
    class SphereSdf  final : public ISdf { /* center, radius */ };
    class BoxSdf     final : public ISdf { /* center, half_extents */ };
    class CapsuleSdf final : public ISdf { /* a, b, radius */ };
    class PlaneSdf   final : public ISdf { /* point, normal */ };
    class TorusSdf   final : public ISdf { /* center, major, minor */ };
    class CylinderSdf final : public ISdf { /* a, b, radius */ };
}
```

**Tests (~12):**
- `evaluate()` at known interior / surface / exterior points for each
  primitive (closed-form expected values).
- `gradient()` matches analytic derivative (closed form per primitive,
  not finite difference).
- `bounds()` is correct for transformed primitives.
- Cross-primitive sanity (sphere with `radius=0` is a point;
  capsule with `a == b` reduces to sphere; box with `half_extents=0`
  is a point).

**Smoke test:** `smoke_sdf_analytic` — instantiate all six primitives,
sample 1000 random points each, validate sign + gradient direction
(gradient at exterior should point outward).

**Module deps locked:** `crd-core` + `crd-memory` + `crd-containers` +
`crd-math` only. No `crd-jobs` until v1 (parallel sampling smoke).

**Out of scope:**
- Any storage backend.
- CSG composition (v4).
- GPU path (v5).

**Definition of done:** all six configs green + `smoke_sdf_analytic`
green; ADR-0064 sections 1–3 implemented for the analytic backend.

---

## v1 — Dense 3D grid backend + CRDR persistence (~5 days)

**Goal:** `DenseSdfGrid` storage backend with trilinear sampling,
central-difference gradient, CRDR `'SDFR'` artifact format wired
through the existing loader pattern.

**Public surface:**

```cpp
namespace crd::sdf
{
    enum class GridFormat : u8 { F16 = 0, F32 = 1 };

    struct GridDesc
    {
        Vec3i      dims;          // (N_x, N_y, N_z)
        Vec3f      origin;        // world-space lower corner
        f32        voxel_size;    // uniform (anisotropic reserved)
        GridFormat format = GridFormat::F16;
    };

    class DenseSdfGrid final : public ISdf
    {
    public:
        DenseSdfGrid(IAllocator* alloc, GridDesc desc);

        [[nodiscard]] f32 evaluate(Vec3f world_p) const noexcept override;
        [[nodiscard]] Vec3f gradient(Vec3f world_p) const noexcept override;
        [[nodiscard]] math::AABB bounds() const noexcept override;

        // Direct voxel write (used by baker in v2 + cooker in v6).
        void set_voxel(Vec3i ijk, f32 d) noexcept;
        [[nodiscard]] f32 get_voxel(Vec3i ijk) const noexcept;

        [[nodiscard]] containers::ConstSpan<u8> voxel_bytes() const noexcept;
        [[nodiscard]] const GridDesc& desc() const noexcept { return m_desc; }
    };
}
```

**CRDR layout:**
- FourCC `'SDFR'`, schema_version 1.
- `SINF` chunk (64 B header pinned by `static_assert`).
- `SDFG` chunk (raw voxel payload).

**Resource integration:**
- `SdfResource` (payload type holds an owned `DenseSdfGrid`).
- `SdfResourceLoader` registered with the existing `LoaderRegistry`.

**Tests (~14):**
- Sample at voxel-centre points returns the stored value (no
  interpolation degeneracy).
- Trilinear sample is C0 across cell boundaries.
- Central-difference gradient on a sphere SDF (analytic baseline)
  has L2 error below `2 × voxel_size` of the analytic gradient.
- `f16` round-trip preserves values within 11-bit precision.
- CRDR write → read → byte-exact comparison.
- Loader produces identical sampling values vs in-memory construction.
- Header pinning: a `static_assert(sizeof(SinfChunk) == 64)` and
  schema-version mismatch path returns `LoadState::Failed`.

**Smoke test:** `smoke_sdf_grid` — bake a sphere SDF analytically into
a 32³ grid, sample at 1000 points, RMSE vs analytic must be below
`voxel_size`.

**Out of scope:**
- Mesh baker (v2).
- Sparse storage (v3).
- GPU upload (v5).
- Anisotropic voxel size (reserved; uniform-only in v1 keeps the
  trilinear math simple and the GPU path direct).

**Definition of done:** all six configs green + `smoke_sdf_grid` green
+ CRDR loader round-trip test green.

---

## v2 — Mesh → SDF baker (CPU, BVH, parallel) (~7 days)

**Goal:** `bake_mesh_to_grid(triangles, GridDesc, padding_voxels) →
DenseSdfGrid`. Robust on non-watertight meshes via the generalised
winding number sign test.

**Public surface:**

```cpp
namespace crd::sdf
{
    struct BakeRequest
    {
        containers::ConstSpan<Vec3f>           positions;   // N positions
        containers::ConstSpan<u32>             indices;     // 3*M triangle indices
        math::AABB                             world_bounds;  // optional; auto-computed if zeroed
        Vec3i                                  grid_dims    = {64, 64, 64};
        f32                                    padding_voxels = 4.0F;  // outside-air around mesh
        GridFormat                             format       = GridFormat::F16;
    };

    [[nodiscard]] DenseSdfGrid bake_mesh_to_grid(
        IAllocator*  alloc,
        BakeRequest  req,
        jobs::Counter* signal = nullptr) noexcept;
}
```

**Algorithm (per ADR-0064 §4):**
1. Build a per-mesh BVH (reuse `crd-eylem`'s dynamic AABB tree —
   shared data structure, not duplicated).
2. For each voxel (parallel_for over `N_x * N_y * N_z`):
   a. Closest-point query against the BVH → `unsigned_distance`.
   b. Generalised winding number (Jacobson 2013) → sign in {−1, +1}.
   c. Store `sign × unsigned_distance` into the grid.

**Async signal:** if `signal` is non-null, baking dispatches as one
big `parallel_for` and the caller can `signal->wait()`. Otherwise
synchronous on calling thread.

**Tests (~16):**
- Bake unit sphere mesh (icosphere, 162 verts) into 32³ grid; RMSE vs
  analytic sphere SDF must be below `2 × voxel_size`.
- Bake closed cube mesh; verify all 8 corners are interior, all 6 face
  centres are surface (within `voxel_size`), all far points are exterior.
- Non-watertight mesh (sphere with one triangle removed): generalised
  winding number must still classify near-interior as interior with
  ≥95 % accuracy.
- Self-intersecting mesh (two overlapping cubes): no NaN, no Inf, all
  voxels have a finite signed distance.
- Empty mesh (0 triangles) returns a grid of constant `+infinity`.
- Async path: `signal->wait()` blocks until grid is fully populated;
  reading before wait returns partial / zero (acceptable; document
  contract).
- Determinism: bake-the-same-mesh-twice produces byte-exact grids
  (ADR-0063 contract test).

**Smoke test:** `smoke_sdf_bake` — load a small mesh from
`assets/test/`, bake into 64³ grid, sample at 1000 random points,
sanity-check sign distribution (interior count vs ray-cast intersection
count must agree within 5 %).

**Out of scope:**
- GPU baker (reserved v8).
- Anisotropic / adaptive voxel size (reserved).
- Pseudo-normal sign for known-clean meshes (reserved opt-in).

**Definition of done:** all six configs green + `smoke_sdf_bake` green
+ determinism contract test (bake-twice = byte-exact) green.

---

## v3 — Narrow-band sparse storage (~5 days)

**Goal:** `NarrowBandSdf` — only voxels within ±`band_voxels` of the
surface stored densely; outside-tile constant fall-through. Target 5–10×
memory savings vs dense for typical mesh assets.

**Public surface:**

```cpp
namespace crd::sdf
{
    struct NarrowBandDesc
    {
        Vec3i      dims;
        Vec3f      origin;
        f32        voxel_size;
        u32        tile_size  = 8;     // 8³ voxels per tile
        f32        band_voxels = 4.0F; // tiles within ±4 voxel_size kept
        GridFormat format     = GridFormat::F16;
    };

    class NarrowBandSdf final : public ISdf
    {
    public:
        NarrowBandSdf(IAllocator* alloc, NarrowBandDesc desc);

        // Same ISdf interface; sampling returns +outside_const for
        // unallocated tiles.
        f32   evaluate(Vec3f world_p) const noexcept override;
        Vec3f gradient(Vec3f world_p) const noexcept override;
        math::AABB bounds() const noexcept override;

        // Conversion from a dense bake.
        [[nodiscard]] static NarrowBandSdf from_dense(
            IAllocator* alloc, const DenseSdfGrid& dense, NarrowBandDesc desc);

        [[nodiscard]] u64 memory_bytes() const noexcept;
    };
}
```

**Storage:**
- `HashMap<TileKey, TileBlockIdx>` → tile presence map.
- Pool-allocated tile blocks via `GrowablePoolAllocator` (one block
  per occupied tile).
- Outside-tile const = max signed distance representable in the format
  (`+inf` clamped to format max).

**Tests (~12):**
- Round-trip: `dense → from_dense → narrow_band → evaluate(p)` matches
  `dense.evaluate(p)` within format precision for all surface-band
  voxels.
- Memory benchmark: bake bunny mesh (or stand-in) into dense + narrow
  band; assert `narrow_band.memory_bytes() < 0.20 * dense.memory_bytes()`.
- Sample outside the narrow band returns +outside_const (no NaN, no
  garbage memory read).
- Gradient at narrow-band boundary returns outward-pointing vector
  (exterior gradient direction).
- Tile eviction: re-baking with the same tile pool reuses blocks (no
  leak; pool refcount goes back to zero on `NarrowBandSdf` destruction).

**Smoke test:** `smoke_sdf_narrow_band` — bake dense + narrow, log
memory ratio, RMSE between the two.

**Out of scope:**
- VDB-like multi-level sparse hierarchy (reserved v8).
- Tile-level hot-reload / streaming (Phase 3.5+ when scene SDF appears).

**Definition of done:** all six configs green + `smoke_sdf_narrow_band`
green + memory-savings benchmark logs ≥5× reduction.

---

## v4 — CSG operators + `SdfTree` composition (~3 days)

**Goal:** Compose analytic + sampled SDFs through CSG operators.
Stateless tree; sampling walks the tree.

**Public surface:**

```cpp
namespace crd::sdf
{
    enum class CsgOp : u8
    {
        Union              = 0,
        Intersection       = 1,
        Difference         = 2,
        SmoothUnion        = 3,
        SmoothIntersection = 4,
    };

    class SdfTree final : public ISdf
    {
    public:
        SdfTree(IAllocator* alloc);

        [[nodiscard]] u32 add_leaf(const ISdf* sdf) noexcept;
        [[nodiscard]] u32 add_op(CsgOp op, u32 a, u32 b, f32 smooth_k = 0.0F) noexcept;
        void              set_root(u32 node_idx) noexcept;

        f32   evaluate(Vec3f p) const noexcept override;
        Vec3f gradient(Vec3f p) const noexcept override;
        math::AABB bounds() const noexcept override;
    };

    // Free functions for ad-hoc one-shot composition (no tree allocation).
    [[nodiscard]] inline f32 op_union(f32 a, f32 b)        noexcept { return min(a, b); }
    [[nodiscard]] inline f32 op_intersect(f32 a, f32 b)    noexcept { return max(a, b); }
    [[nodiscard]] inline f32 op_difference(f32 a, f32 b)   noexcept { return max(a, -b); }
    [[nodiscard]]        f32 op_smooth_union(f32 a, f32 b, f32 k) noexcept;
    [[nodiscard]]        f32 op_smooth_intersect(f32 a, f32 b, f32 k) noexcept;
}
```

**Smooth-min:** Quílez polynomial smin with smoothness factor `k`:
```
smin(a, b, k) = min(a, b) - h*h*k*0.25, where h = max(k - |a-b|, 0)
```
`gradient()` walks the tree and chains gradients with the appropriate
smooth-min derivative at smooth-union nodes (continuous everywhere).

**Tests (~14):**
- Union of two unit spheres at distance 1.5: surface point at midpoint
  has SDF 0 ± `epsilon`.
- Intersection of two boxes: result is the geometric AND.
- Difference (sphere − box): hollowed-out region matches analytic.
- Smooth union: gradient is C1-continuous across the smooth seam
  (numerical check: gradient(p) and gradient(p + epsilon * gradient(p))
  are within 0.05 angular distance).
- Tree with mixed analytic + sampled leaves evaluates each correctly.
- `bounds()` is the union of leaf bounds for Union / SmoothUnion;
  conservative (full union) for the other ops in v4.

**Smoke test:** `smoke_sdf_csg` — compose
`SmoothUnion(Sphere, Box) − Cylinder`, sample 1000 points, validate.

**Out of scope:**
- CSG of two `DenseSdfGrid`s into a new baked grid (offline tool;
  reserved v6 cooker work).
- Repeated / mirrored / instanced CSG (reserved).

**Definition of done:** all six configs green + `smoke_sdf_csg` green
+ smooth-union gradient continuity test green.

---

## v5 — GPU 3D texture upload path + GLSL helper (~4 days)

**Goal:** Mirror a `DenseSdfGrid` (or `NarrowBandSdf` densified back
into a tile-packed 3D texture) to a GPU 3D texture via the existing
async upload path.

**Cross-module surface:**

```cpp
// In crd-renderer (extends GpuUploader).
namespace crd::renderer
{
    [[nodiscard]] UploadHandle GpuUploader::upload_sdf_async(
        const sdf::DenseSdfGrid& src,
        rhi::Texture3D&          dst);
}
```

`upload_sdf_async` records a copy command, submits with a fence
(reuses ADR-0061's `Queue::submit(cmd, fence)`), returns a moveable
`UploadHandle`. Same pattern as `upload_mesh_async` /
`upload_texture_async`.

**Shader-side helper** (new `assets/shaders/lib/crd_sdf.glsl`):

```glsl
// world_pos in world-space; sdf_inv_world transforms world → SDF UVW.
float crd_sample_sdf(sampler3D sdf_tex, vec3 world_pos, mat4 sdf_inv_world)
{
    vec3 uvw = (sdf_inv_world * vec4(world_pos, 1.0)).xyz;
    return texture(sdf_tex, uvw).r;
}

vec3 crd_sample_sdf_gradient(sampler3D sdf_tex, vec3 world_pos, mat4 sdf_inv_world, float h)
{
    // Central differences via 6 samples.
    return ...;
}

// Cone trace through the SDF; used by DFAO / DFGI in renderer 3.5+.
float crd_cone_trace_sdf(sampler3D sdf_tex, mat4 inv_world,
                         vec3 origin, vec3 dir, float cone_angle, float max_t);
```

**Tests (~10):**
- Round-trip: CPU bake → upload → GPU readback (compute shader writes
  to a buffer) → byte-exact (within `f16` precision tolerance).
- `UploadHandle` lifecycle: `is_ready()` returns false before fence,
  true after, `take_texture()` transfers ownership.
- Smoke `smoke_sdf_gpu` (GPU/window category): bakes a sphere, uploads,
  runs a compute shader that samples 1000 points, validates RMSE ≤
  `1.5 × voxel_size` against analytic.

**GLSL helper tests:** none (shader code; validated through
smoke_sdf_gpu and renderer integration smokes in v6+).

**Out of scope:**
- DFAO / DFGI shader integration in the renderer (Phase 3.5+).
- Bindless 3D-texture descriptor heap (Phase 3.8+ when bindless lands).

**Definition of done:** all six configs build clean +
`smoke_sdf_gpu` green on machines with Vulkan + display.

---

## v6 — Cooker integration (~4 days)

**Goal:** `tools/asset_cooker/` recognises SDF assets — both standalone
`.sdf.toml` files and `bake_sdf = true` opt-in on existing mesh assets.

**Authoring:**

```toml
# assets/sdf/scene_aabb.sdf.toml — standalone SDF asset
type = "mesh-bake"
mesh = "geom/scene.crdr"
grid_dims = [128, 128, 128]
voxel_size = 0.05
padding_voxels = 4
format = "f16"

# Or, in an existing mesh asset (assets/geom/character.mesh.toml):
[bake_sdf]
enabled = true
grid_dims = [64, 64, 64]
padding_voxels = 4
```

**Cooker work:**
- Recognise `.sdf.toml` extension; route to `SdfCooker`.
- Recognise `[bake_sdf]` table in mesh assets; co-emit SDFG chunk
  alongside the mesh artifact (single CRDR pack with both MESH + SDFG
  chunks).
- Hot-reload watcher integration (matches v1l scene cooker pattern).

**Tests (~8):**
- `.sdf.toml` cook → CRDR artifact loads back as `SdfResource`.
- `bake_sdf = true` on a mesh: the resulting CRDR has both `'MESH'`
  and `'SDFR'` payloads; both round-trip correctly.
- Re-cook with same input → byte-exact artifact (determinism).
- Re-cook with mesh changed → fresh bake; loader returns the new SDF.

**Smoke test:** integration with `smoke_asset_import`-style — cook a
mesh with `bake_sdf = true`, load through `ResourceManager`, sample
the SDF.

**Out of scope:**
- Visual SDF preview tool (Phase 7 editor).
- Cook-time CSG of multiple SDFs (reserved).

**Definition of done:** all six configs green + cooker integration
test green + at least one demo SDF asset shipped under `assets/sdf/`.

---

## v7 — Marching Cubes extraction (SDF → MeshResource) (~4 days)

**Goal:** `extract_mesh_from_sdf(grid) → MeshData` — produce a
triangle mesh from an SDF grid via Marching Cubes (Lorensen & Cline 1987).

**Public surface:**

```cpp
namespace crd::sdf
{
    struct ExtractRequest
    {
        const DenseSdfGrid* grid;
        f32                 iso_value = 0.0F;  // 0 for surface
        bool                generate_normals = true;
    };

    [[nodiscard]] containers::Array<MeshVertex> extract_vertices(IAllocator*, ExtractRequest);
    [[nodiscard]] containers::Array<u32>        extract_indices(IAllocator*, ExtractRequest);

    // Convenience: one-shot to a MeshResource-shaped result.
    struct ExtractedMesh
    {
        containers::Array<MeshVertex> vertices;
        containers::Array<u32>        indices;
    };
    [[nodiscard]] ExtractedMesh extract_mesh_from_sdf(IAllocator*, ExtractRequest);
}
```

**Algorithm:** classical 256-case Marching Cubes table; gradient at
each emitted vertex via the grid's central-difference gradient.
Standard, well-understood, ~300 LOC including the lookup table.

**Tests (~10):**
- Bake sphere SDF → extract → vertex count is in expected range
  (Marching Cubes resolution-dependent; check 90-110 % of grid-derived
  estimate).
- Extracted mesh is closed (every edge appears in exactly two triangles
  for an interior surface).
- Normals point outward (dot product with gradient is positive).
- Re-extracting same grid produces byte-exact same vertex/index arrays
  (determinism).
- Iso value other than 0 extracts an offset surface (sphere of radius
  `r + iso`).
- Extract empty grid returns 0 vertices, 0 indices.

**Smoke test:** `smoke_sdf_extract` — bake sphere → extract → log
vertex/triangle counts; verify mesh is renderable (vertex/index buffer
sizes are valid).

**Out of scope:**
- Dual Contouring with sharp features (reserved v8 — needs Hermite
  data + QEF solver).
- Adaptive / multi-resolution extraction (reserved).
- GPU extraction (reserved Phase 3.8+ when GPU compute matures).

**Definition of done:** all six configs green + `smoke_sdf_extract`
green + closed-mesh property test green.

---

## v8 — Reserved (deferred until consumer demand)

These three items are **reserved API slots**, not Phase 3.1.5 work.
Each gets its own slice / phase when a real consumer lands:

1. **GPU baker (LBVH-based, Karras 2012).** Speeds large-scene SDF
   bakes by 50–100×. Lands when Phase 3.8 GPU-driven rendering needs
   per-frame scene SDF rebuilds.
2. **VDB-like sparse hierarchy.** Multi-level tile structure for
   km-scale terrain / full-building scene SDFs. Lands when Phase 3.5+
   open-world streaming surfaces the need.
3. **Dual Contouring with sharp features (Ju 2002).** QEF solver +
   Hermite data sampling for crisp edges. Lands when an editor (Phase 7)
   workflow needs hard-edge SDF modelling.

The substrate is **forward-compatible** with all three: same `ISdf`
interface, same CRDR format with optional new chunks, same GPU upload
path.

---

## Cross-module integration touchpoints

By the time Phase 3.1.5 ships, these consumers can plug in immediately:

- **`crd-eylem` v3 (XPBD soft / cloth / rope)** — uses
  `bake_mesh_to_grid` for environment colliders; samples via
  `DenseSdfGrid::evaluate` + `gradient` inside the constraint solver.
  No changes to eylem v3's public API.
- **`crd-font` (Phase 3.3)** — uses `bake_mesh_to_grid` (2D variant
  reserved as a v8 follow-up — for v1, font ships its own 2D MTSDF
  baker on top of `crd-sdf`'s pattern). The 3D substrate teaches the
  cooker / loader / GPU patterns.
- **`crd-renderer` (Phase 3.5+)** — `crd_sdf.glsl` cone-trace helper
  feeds DFAO compute pass + DFGI prototype.
- **`crd-audio` (Phase 3.4)** — voxelised scene SDF for acoustic
  occlusion baking.

## Determinism contract

Every cooker step + every baker call + every CSG evaluation must obey
ADR-0063 (eylem determinism contract):

- `crd::math::deterministic` for trig / exp / sqrt (no `std::sin` /
  `std::cos` / `std::sqrt` in the source).
- `crd::containers::sort` for any sort (no `std::sort`).
- FNV-1a 64 for any hash (no `std::hash`).
- `-ffp-contract=off` / `/fp:precise` in the module's compile flags.
- No fast-math, no x87.

The 9-config replay-hash CI (Phase 3.1 v9b) hashes a representative
SDF bake into the per-frame replay hash; cross-platform divergence in
the SDF baker fails CI immediately.

## Test budget

| Slice | Tests | Smokes |
| :---: | :---: | --- |
| v0 | 12 | smoke_sdf_analytic |
| v1 | 14 | smoke_sdf_grid |
| v2 | 16 | smoke_sdf_bake |
| v3 | 12 | smoke_sdf_narrow_band |
| v4 | 14 | smoke_sdf_csg |
| v5 | 10 | smoke_sdf_gpu (GPU/window) |
| v6 | 8  | (asset_cooker integration) |
| v7 | 10 | smoke_sdf_extract |
| **Total** | **96** | **7 headless + 1 GPU/window** |

Adds ~+12 % to the current 856-test baseline; six headless smokes
added to the per-config sweep.

## Open questions (to resolve in-flight)

- **Anisotropic voxel size.** v1 ships uniform `voxel_size`; non-uniform
  is straightforward but doubles the trilinear math. Defer until
  consumer asks (likely Phase 3.5 renderer for compressed-Z scene SDFs).
- **Per-mesh vs scene-global SDF.** Phase 3.1.5 ships per-mesh.
  Scene-global (one fused SDF over all static geometry) is a Phase 3.5+
  renderer concern — same baker, different driver.
- **`f16` vs `f32` default.** Substrate defaults to `f16` for memory.
  Eylem soft-body contact may need `f32` for numerical stability;
  decision deferred to eylem v3 in-flight.

## References

- ADR-0064 — `crd-sdf` substrate architecture (locks the decisions).
- `docs/research/cerid-sdf.md` — industry survey + algorithm choices.
- ADR-0061 — Async GPU upload contract (`UploadHandle` + `Fence`
  reused for `upload_sdf_async`).
- ADR-0062 — Eylem physics (consumes SDF colliders in v3).
- ADR-0063 — Eylem determinism contract (inherited wholesale).
- ADR-0047 — Font rendering (MTSDF consumer in Phase 3.3).
- Jacobson et al. (2013) — Generalised winding numbers (baker sign).
- Lorensen & Cline (1987) — Marching Cubes (extraction).
- Quílez — Smooth minimum family (CSG smooth-union).
