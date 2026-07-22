# Recipe — mesh extraction: TSDF fusion + marching cubes

Turning depth maps into a triangle mesh is the bridge from a captured radiance field (2DGS, ckir_gsplat2d.hpp) — or
any depth source (RGBD, a depth pass) — into real geometry for a mesh/material pipeline. The gold-standard route,
the one the 2DGS paper (Huang et al. 2024) uses, is two stages: **TSDF fusion** builds a signed distance field on a
voxel grid whose zero-level set is the surface, then **marching cubes** extracts that isosurface as triangles. This
recipe teaches both end to end.

---

## 1. Parameters first

### TSDF fusion

| Param | Meaning | Units | Default | Notes |
|---|---|---|---|---|
| grid `nx,ny,nz` | voxel resolution | — | scene-dependent | finer = more detail + more memory (N³) |
| `origin` | world position of voxel (0,0,0)'s corner | world | — | the grid's lower bound |
| `h` (voxel size) | edge length of one voxel | world | ~surface-detail scale | the mesh resolution floor |
| `μ` (truncation) | half-width of the integrated band around the surface | world | 2–4·h | voxels farther behind than μ are unobserved |
| depth map | observed surface depth (view-z) per pixel | world | — | ≤0 ⇒ no surface (skipped); from `depthSum/(1−T)` for a splat render |
| camera | R,t,fx,fy,cx,cy,near | — | — | one posed depth map per view; fuse many |

### Marching cubes

| Param | Meaning | Default | Notes |
|---|---|---|---|
| isovalue | the field level meshed | 0 | the SDF surface |
| field | scalar per voxel (finalised TSDF) | — | inside <0, outside >0 |
| triTable / edgeTable | the 256-case connectivity | canonical | Lorensen/Bourke; `mc_tables.hpp` |

---

## 2. What it is / why it exists

A depth map is a 2.5-D snapshot: it has holes (occlusions), is per-view, and is not a surface you can light,
collide, or UV-map. Naively triangulating one depth map (each 2×2 pixel block → 2 triangles) gives a view-locked,
non-watertight sheet with seams at every depth discontinuity, and it cannot fuse multiple views.

**TSDF fusion** solves the fusion + hole problem: it represents the surface implicitly as the zero-crossing of a
signed distance field averaged over all views. Free space in front of the surface is positive, occluded space
behind is negative, and the crossing is the surface — so overlapping views reinforce, noise averages out, and a
voxel seen by many cameras is confident. **Marching cubes** then turns that implicit field into an explicit,
watertight triangle mesh in one parallel pass over the grid cells.

---

## 3. The maths

### TSDF fusion

For a voxel at world centre `P`, project it into a view: `V = R·P + t`, pixel `(fx·V.x/V.z+cx, fy·V.y/V.z+cy)`.
Read the observed surface depth `d_obs` at that pixel. The **signed distance** along the view ray is

```
sdf = d_obs − V.z
```

positive when the voxel is closer to the camera than the surface (free space), negative when behind (occluded).
Truncate to a band and normalise: `tsdf = clamp(sdf/μ, −1, +1)`. Integrate as a running weighted average across
views (weight `w`, here 1):

```
tsdf_sum += w·tsdf ;   w_sum += w        (only if in-image, has-surface, and sdf > −μ)
field = tsdf_sum / w_sum                 (finalise; unobserved voxels → +1 = free space)
```

Voxels farther behind the surface than μ are *not observed* by that view (occluded, unknown) and are skipped.

### Marching cubes (Lorensen & Cline 1987)

Process each **cell** (8 neighbouring voxels forming a cube). Build an 8-bit `cubeindex`, bit *i* set iff
`field[corner i] < isovalue` (corner inside). The surface enters/exits the cube through a set of its 12 edges,
determined by `cubeindex`; `triTable[cubeindex]` lists up to 5 triangles as edge indices. The **vertex on edge
(a,b)** is the linear interpolation to the field zero:

```
t = field[a] / (field[a] − field[b]) ;   vertex = P[a] + t·(P[b] − P[a])
```

The **outward normal** is the face normal `normalize((v1−v0)×(v2−v0))` — but the canonical table winds triangles so
this points *inward* for the `inside = field<iso` convention, so reverse the winding (emit v0,v2,v1) and negate the
normal to get an outward-consistent triangle. (Smooth per-vertex normals from the field gradient are a refinement;
face normals are correct and a mesh pipeline can re-derive smooth ones from topology.)

**Variable output** (0–5 triangles per cell) is handled exactly like the B19-a4 splat bin: **count** triangles per
cell → **scan** (exclusive prefix-sum) → **emit** each cell's triangles at its scanned offset. No cell can spill
into another's range because emit writes triangle *t* only when it is valid (`triTable[…] ≥ 0`).

Papers: Lorensen & Cline, "Marching Cubes", SIGGRAPH 1987; Paul Bourke, "Polygonising a scalar field" (the
canonical `edgeTable`/`triTable`); Curless & Levoy, "A Volumetric Method…" SIGGRAPH 1996 (TSDF fusion);
Newcombe et al., KinectFusion 2011 (real-time TSDF).

---

## 4. The full assembly

All CKIR compute kernels (scalar tier ⇒ component-wise); `ckir_mesh.hpp`, tables in `mc_tables.hpp`:

1. **`build_surface_depth_kernel`** — splat render G-buffer → `depthSum/(1−T)` depth map (0 where no surface).
2. **`build_tsdf_integrate_kernel`** — one thread/voxel; integrate ONE posed depth map into `(tsdf_sum, w_sum)`.
   Call once per view (multi-view = repeated dispatches into the same accumulators).
3. **`build_tsdf_finalize_kernel`** — `field = w>0 ? tsdf_sum/w_sum : +1`.
4. **`build_mc_count_kernel`** — per cell → cubeindex → triangle count.
5. **scan** (`ckir_scan.build_scan`, exclusive) → per-cell output offset + total triangle count.
6. **`build_mc_emit_kernel`** — per cell → write its triangles (positions + outward face normals, 18 floats/tri) at
   the scanned offset. Only the small corner/edge tables are baked into the kernel; the 256×16 triTable is a bound
   buffer indexed by the runtime cubeindex.

Output: a triangle soup `[pos.xyz · n.xyz] × 3` per triangle, ready to index/weld and feed a material pipeline.

---

## 5. The traps

- **Winding is inward with the canonical table.** For `inside = field<iso`, the standard triTable winds triangles
  so `cross(v1−v0,v2−v0)` points toward the interior — a sphere came out with **0/140 outward normals** until the
  winding was reversed (emit v0,v2,v1) and the normal negated. The vertex-vs-host check does NOT catch this (both
  use the same table order); an **independent geometric check does** — sum of triangle areas ≈ 4πR² and every face
  normal · centroid > 0. Always gate connectivity with an independent surface metric, not just a self-comparison.
- **Vertices lie on the surface regardless of the table.** Vertex positions come from edge zero-crossing
  interpolation, which is independent of `triTable` — so "vertices on-surface" validates the *interpolation*, never
  the *connectivity*. Different checks validate different halves.
- **Variable per-cell output must be guarded, not capped-and-masked.** Emit writes triangle *t* at `offset+t` only
  when `triTable[cubeindex·16 + 3t] ≥ 0`; an unguarded write for an invalid *t* spills into the next cell's range.
  The scan makes the offsets exact, so no fixed cap is needed (unlike the pre-a4 splat bucket).
- **Type the GPU buffers.** The kernel declares `triTable` as I32 and `edgeConn`/`cornerOff` as U32; the GPU buffer
  is raw bytes, so upload integer data as integer bytes (`dispatch_kernel_1wg` uploads floats and would corrupt an
  int table — use a typed manual dispatch). The CPU oracle stores everything as f64 and reads by declared dtype, so
  it is forgiving; only the real backend is strict.
- **Unobserved voxels at the band edge.** Finalising unobserved voxels to +1 (free space) avoids a spurious
  interior surface, but a cell straddling the observed-band boundary (some corners real-negative, some +1) can emit
  a thin wall. Fully-observed regions (an analytic field, or a well-covered capture) have no such artifact; a
  larger μ or restricting emit to fully-observed cells removes it.
- **Scan sizing.** `build_scan` is single-pass only when `n ≤ threads·8` with `threads | n` (a power of two).
  Pick `ncells = (nx−1)³` accordingly (e.g. 8³=512 with threads 256), or use the multi-block plan.

---

## 6. Measured numbers

- CPU oracle: TSDF plane depth → exact truncated-SDF ramp, zero crossing at the surface (<1e-3). MC sphere (R=0.5,
  h=0.25): 140 triangles, **vertices on-surface worst |r−R| = 0.0104**, area 2.90 vs ideal 3.14, **outward normals
  140/140**, and **vertex-for-vertex identical to a host MC reference**.
- Real Vulkan: TSDF == oracle bit-exact (0.000e+00); the full count→scan→emit marching cubes == oracle mesh, **worst
  |GPU − oracle| = 5.96e-08** (140 triangles).

---

## 7. Where the code lives

- `engine/kir/include/crd/kir/ckir_mesh.hpp` — surface-depth, TSDF integrate/finalise, MC count/emit kernels + the
  `edge_vertex`/`cube_index` helpers.
- `engine/kir/include/crd/kir/mc_tables.hpp` — the canonical corner/edge/triangle tables (data only).
- `tests/kir/test_ckir_mesh.cpp` — TSDF gates + the MC count/emit gates (sphere, vs host reference).
- `tests/gpu-context-vulkan/test_vulkan_gsplat.cpp` — the `[mesh]` Vulkan gates (TSDF + MC == oracle).
- Upstream primitive + its recipe: `ckir_gsplat2d.hpp`, `docs/recipes/2026-07-21-2d-gaussian-splatting-surfels.md`.
