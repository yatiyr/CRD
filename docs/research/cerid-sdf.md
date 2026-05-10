# Cerid — SDF substrate research

**Date:** 2026-05-10
**Locks:** ADR-0064 (`crd-sdf` substrate architecture).
**Phase plan:** `docs/phases/phase-3.1.5-sdf.md`.

> Source-of-truth document for the *why* behind every algorithm /
> data-structure / consumer choice in `crd-sdf`. ADR-0064 cites this
> file. Phase plan implements against it.

## 1. Why SDFs at all (per-domain)

| Domain | Use case | Without SDF substrate |
| --- | --- | --- |
| Games — rendering | DFAO, DF soft shadows, DF cone-traced GI (Lumen non-RT path) | screen-space approximations only; no robust GI fallback |
| Games — gameplay | Volumetric effects, raymarched detail, decals on arbitrary geometry | brittle; per-effect bespoke math |
| Robotics | Robot–environment collision queries, sensor simulation, motion-planning potential fields | brittle GJK/SAT on imported scan/CAD; no closed-form gradient |
| Medical | DICOM/CT implicit surfaces (skin/bone separation), volume-preserving deformation | no implicit-surface path — falls back to mesh-only |
| Cinematic | Volumetric VFX, smoke/fire density fields, hair / fur footprint | per-shot bespoke pipelines |
| DAW | Acoustic occlusion / reverb baking against scene geometry (Steam Audio model) | no acoustic primitive geometry available |
| Authoring | SDF modelling (Dreams, MagicaCSG, Claybook), CSG booleans, mesh extraction | no first-class implicit modelling path |
| Physics (eylem) | Mesh colliders + closest-point + hydroelastic contact | brittle mesh-vs-mesh GJK on imported geometry; no smooth contact normals |

The **multi-domain mandate** (CLAUDE.md §1) makes this non-negotiable.
A single-domain engine (a pure FPS engine) could ship without; Cerid
explicitly cannot.

## 2. Industry landscape

### 2.1 Real-time rendering

- **Unreal Engine 5 — Lumen non-RT path.** Per-mesh SDFs baked at
  import time → fused into a global scene SDF for cone-traced
  occlusion + GI. Distance Field Ambient Occlusion + Distance Field
  Soft Shadows shipped since UE4. Mesh distance fields are the *only*
  reason Lumen GI works without RT hardware.
- **Frostbite (DICE).** Internal DFGI prototype, paper at SIGGRAPH 2018
  ("Real-time Global Illumination by Precomputed Local Reconstruction
  from Sparse Radiance Probes" + DFAO). Production DFAO across most
  shipped Frostbite titles.
- **Claybook (2018).** Entire game world rendered as raymarched SDFs;
  no triangles. Demonstrated SDF rendering can be a primary path,
  not just an effect.
- **Dreams (Media Molecule, 2020).** Fully SDF-based modelling +
  rendering pipeline. CSG + smooth-min compositions in the editor,
  splat rendering at runtime.
- **id Tech 7 (Doom Eternal).** Mesh distance fields used for soft
  shadows, decals, and some volumetric effects.

### 2.2 Physics

- **Drake (Toyota Research Institute).** Hydroelastic contact model
  uses pressure fields derived from SDF-like representations. Robust
  contact for non-watertight imported robot meshes — exactly the
  Cerid use case.
- **MuJoCo / MJX.** Signed-distance contact mode for arbitrary mesh
  geometry; supersedes the older convex-decomposition path on
  complex robot models.
- **NVIDIA FleX / Vellum / Flex.** Per-particle SDF queries for soft
  body and cloth collision against rigid environment meshes. The
  performant path for "10000 particles vs 1000-triangle environment".
- **NVIDIA Newton (2025).** SDF colliders are a first-class collider
  type; the framework explicitly recommends them for non-watertight
  robot parts.
- **PhysX 5.** Added SDF colliders in 5.1 specifically to handle
  non-convex mesh contact robustly. Acknowledges that GJK + SAT on
  arbitrary mesh geometry is fragile in production.
- **Bullet 3.** SDF heightfield collider; full mesh SDF reserved for
  GPU pipeline.

### 2.3 Authoring + procedural

- **MagicaCSG.** Real-time SDF modeller with CSG ops (boolean +
  smooth-min); exports to mesh via Marching Cubes.
- **Houdini (SideFX).** VDB volumes + signed distance representations
  are core to the procedural pipeline. NanoVDB now ships GPU sampling
  for use in production renderers.
- **Blender.** Volumes node-graph workflow + OpenVDB import.

### 2.4 Audio

- **Steam Audio (Valve).** Voxelised scene SDF feeds the acoustic
  occlusion pass + reverb impulse-response baker.
- **Phonon SDK (Apple).** Same pattern: scene voxelisation → distance
  field → acoustic propagation.

### 2.5 Differentiable / ML

- **DiffTaichi, Brax, MJX, NVIDIA Newton.** All of these support
  differentiable SDF queries because SDFs *are* C¹ almost everywhere
  (the field is differentiable except at medial axis points). This
  matters for Cerid's Phase 3.1 v9 differentiable-physics target.

**Conclusion of the landscape:** every engine class that overlaps
Cerid's domain (games, robotics, medical, DAW, cinematic, authoring)
ships an SDF substrate or has explicit roadmap commitments to one.
Going without is a competitive deficit, not a roadmap-friendly
deferral.

## 3. Algorithm choices (with dispositions)

### 3.1 Mesh → SDF baker

| Algorithm | Year | Watertight required? | Robust to self-intersection? | Differentiable? | Cerid disposition |
| --- | :---: | :---: | :---: | :---: | --- |
| **Generalised winding number** (Jacobson, Kavan, Sorkine-Hornung) | 2013 | **No** | **Yes** | **Yes** | **CHOSEN — v2 baker.** Cerid imports glTF / FBX / USD / OBJ / scan data; few are watertight. |
| Pseudo-normal angle-weighted (Bærentzen, Aanaes) | 2005 | Yes | No | Partial | Reserved — opt-in fast path for known-clean assets. |
| Ray-cast intersection counting | 1980s | Yes | No | No | Rejected — fragile on bad meshes. |
| Voxel paint + flood fill | n/a | Yes | No | No | Rejected — slow + brittle. |

The Jacobson 2013 paper is **the** reference for robust signed
distance from real-world mesh data. It's the same algorithm Drake,
NVIDIA Newton, MuJoCo MJX, and Houdini's VDB-from-mesh node converge
on.

### 3.2 Storage backend

| Backend | Memory | GPU-friendly? | Cerid disposition |
| --- | --- | :---: | --- |
| Analytic primitives | 0 | Closed-form in shader | **v0** |
| Dense `N³` grid | `N³ × bytes_per_voxel` | Native (3D texture) | **v1** |
| Narrow-band sparse (tile-based) | ~10–20 % of dense | Partial (tile atlas + lookup) | **v3** |
| VDB-like multi-level sparse (NanoVDB pattern) | ~5–10 % of dense | Yes (NanoVDB shipped GPU sampler) | **v8 reserved** |
| BSP / octree | varies | Awkward sampling | Rejected |

### 3.3 Sampling

- **Trilinear interpolation.** Standard, matches GPU texture sampling
  exactly. C0 continuous; gradient continuity comes from the gradient
  evaluator, not the interpolant. **CHOSEN — v1.**
- **Cubic / monotonic** (Catmull-Rom etc). Smoother gradient at the
  cost of 27 samples vs 8. Reserved.
- **Hermite-data + QEF solver** (DC). Sharp features at the cost of
  per-edge Hermite data. Reserved for v8 Dual Contouring.

### 3.4 Gradient evaluation

- **Central differences** (`(d(p+h) - d(p-h)) / 2h`). 6 samples per
  gradient. Standard. **CHOSEN — v1.**
- **Forward differences** (4 samples). Less accurate; gradient bias
  toward sample direction. Rejected.
- **Tetrahedron method** (Akinci 2011) — 4 samples around a regular
  tetrahedron. Better numerical stability for steep gradients.
  Reserved for v8 if eylem soft-body contact reveals instability.

### 3.5 CSG operators

- **Min / max** for union / intersection. Trivial; standard.
- **`max(a, -b)`** for difference. Standard.
- **Quílez polynomial smin** for smooth-union with C¹ gradient
  continuity. iquilezles.org/articles/smin/. **CHOSEN.**
- Alternatives: exponential smin, root smin, polynomial smin variants.
  Polynomial smin is the cheapest with continuous gradient. Choice
  matches Dreams + MagicaCSG + most SDF demoscene work.

### 3.6 Mesh extraction

| Algorithm | Year | Sharp features? | Hole-free? | Cerid disposition |
| --- | :---: | :---: | :---: | --- |
| **Marching Cubes** (Lorensen, Cline) | 1987 | No | Yes | **CHOSEN — v7.** Standard, ~300 LOC including 256-case lookup table. |
| Marching Tetrahedra | 1991 | No | Yes | Rejected — more triangles for same fidelity. |
| **Dual Contouring** (Ju et al.) | 2002 | **Yes** (with Hermite data + QEF) | Yes | **Reserved — v8.** Sharp-feature support requires per-edge Hermite data + QEF solver — not v7 substrate work. |
| Surface Nets | 1998 | Limited | Yes | Reserved. |

### 3.7 GPU baker

- **LBVH** (Karras 2012, "Maximizing Parallelism in the Construction of
  BVHs, Octrees, and k-d Trees"). The standard GPU BVH builder.
  Reserved for v8 — needed when scene-global SDF rebake per frame
  becomes a workload (Phase 3.5+ renderer or Phase 3.8 GPU-driven path).
- **Sparse-tile GPU bake** (NanoVDB-style). Reserved alongside the
  VDB sparse storage hierarchy.

## 4. Determinism

`crd-sdf` inherits ADR-0063 (eylem determinism contract) wholesale:

- `-ffp-contract=off` / `/fp:precise` in compile flags.
- Cerid-internal trig / sort / hash (`crd::math::deterministic`,
  `crd::containers::sort`, FNV-1a 64).
- Commutative cross-thread merges in the parallel baker (id-stable
  voxel ordering; reduction operators are commutative).
- Bake-twice byte-exact contract test (v2).

This matters because:

1. Eylem samples SDF colliders inside the deterministic physics step.
   A non-deterministic SDF baker breaks replay-hash CI.
2. Procedural mesh generation at runtime + offline asset cooks must
   produce **bit-exact identical** SDF artifacts for identical inputs.
3. Differentiable physics (Phase 3.1 v9) needs deterministic
   gradients through the SDF chain.

## 5. Open research notes

These are research observations that did not make it into ADR-0064 or
the phase plan but are worth surfacing for future slices:

- **Per-cluster SDF (Nanite-adjacent path).** UE5 Nanite stores
  per-cluster representations of meshes; per-cluster SDFs would let
  the renderer trace into very-high-detail meshes without dense
  volumetric storage. Reserved for Phase 3.8 GPU-driven rendering.
- **Differentiable winding number.** Jacobson 2013 is differentiable;
  the gradient w.r.t. mesh vertices feeds Phase 3.1 v9 differentiable
  physics. Reserved hook.
- **Hybrid analytic + grid SDFs** (`'SDFA'` chunk reserved in ADR-0064
  §11). Useful for character capsules baked into environment grids;
  evaluator picks the closer of analytic-overlay or grid.
- **MPM / FLIP SDF coupling.** When eylem v8 ships GPU MPM, SDF
  colliders provide the rigid environment boundary cheaply.

## 6. References (curated)

- Jacobson, Kavan & Sorkine-Hornung (2013) — *Robust inside-outside
  segmentation using generalized winding numbers*. SIGGRAPH.
- Bærentzen & Aanaes (2005) — *Signed distance computation using the
  angle weighted pseudo-normal*. IEEE TVCG.
- Lorensen & Cline (1987) — *Marching Cubes: A high resolution 3D
  surface construction algorithm*. SIGGRAPH.
- Ju, Losasso, Schaefer & Warren (2002) — *Dual Contouring of Hermite
  Data*. SIGGRAPH.
- Quílez — *smooth minimum* family. iquilezles.org/articles/smin/.
- Karras (2012) — *Maximizing Parallelism in the Construction of BVHs,
  Octrees, and k-d Trees*. HPG.
- Akinci, Ihmsen et al. (2011) — *Tetrahedral gradient*. PCISPH.
- *Real-Time Global Illumination by Precomputed Local Reconstruction
  from Sparse Radiance Probes* (Frostbite, SIGGRAPH 2018).
- *Mesh Distance Fields in Unreal Engine 4/5* (Epic Games
  documentation).
- *Steam Audio acoustic propagation* (Valve documentation).
- *NanoVDB* (NVIDIA). github.com/AcademySoftwareFoundation/openvdb.
