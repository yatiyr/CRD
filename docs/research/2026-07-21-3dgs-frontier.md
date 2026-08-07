# Research — 2026-07-21 — 3D Gaussian Splatting frontier (B19 dossier)

> **Outcome:** **adopted** — the 3DGS renderer shipped (1080p perf recorded; reference-comparison blocker noted in memory `project_3dgs_1080p_perf_and_reference_blocker`). *(stamped 2026-08-07, doc-hygiene pass)*

## Question

B19 makes **radiance fields a first-class primitive** in Cerid — 3D Gaussian Splatting (3DGS). Before building,
learn the whole subject end to end, especially the 2024–2026 cutting edge, and map it onto what the engine
already has (CKIR, the B17 OIT tiers, GPU sort, v15/v16 autodiff, the C3/B9 RT context we just used for hair).
What is the method, what is the current frontier along every axis, and how do we decompose B19?

## TL;DR

- **3DGS is a SORT + OIT-COMPOSITE problem, and we already built both halves.** The forward renderer projects
  anisotropic Gaussians to 2D, bins them into screen tiles, **globally depth-sorts** all Gaussian-tile instances
  (our GPU radix/onesweep sort), and **front-to-back alpha-composites** per tile (our B17 A-buffer/OIT). Training
  differentiates that renderer — **our v15 fwd-AD / v16 rev-AD**. The **ray-traced** variant (3DGRT) wraps each
  Gaussian in a procedural-AABB proxy and traces it — **exactly the LSS pattern we just shipped for hair (B18-f).**
  B19 is an assembly of existing engine capabilities more than new machinery.
- **The frontier has settled into clear, mostly-orthogonal axes**, each with a mandatory winner: anti-aliasing
  (**Mip-Splatting** + **StopThePop** view-consistency), geometry/mesh (**2DGS** surfels / **GOF**), compression
  (**Self-Organizing Gaussians** / **HAC**, 20–40×), training (**3DGS-MCMC**, **Taming 3DGS**), ray-traced +
  relightable (**3DGRT**, **Relightable 3DGS**), and large-scale LoD (**Hierarchical-3DGS**, **Octree-GS**).
- **The recommended B19 decomposition** builds the forward rasterizer core first (proves the primitive renders),
  then layers the mandatory frontier: **B19-a** forward splat → **B19-b** Mip AA + StopThePop consistency →
  **B19-c** 2DGS surfels + mesh extraction (the bridge to B1 materials) → **B19-d** compression → **B19-e**
  ray-traced + relightable Gaussians (on the C3/B9 RT path) → **B19-f** training / differentiable fit (v16 AD) +
  LoD. Densification training can ride alongside once the forward+backward pair exists.

## Recommendation for Cerid

Build B19 bottom-up in CKIR, reusing what exists, in this order:

1. **B19-a — the forward splat rasterizer (the core, do first).** Everything else layers on it and it renders a
   scene end to end. See §4 for the exact algorithm. Compute deps ALL exist: covariance projection is plain math;
   tile binning is a scatter; the **global (tile,depth) sort is our GPU radix/onesweep** (integer keys — bit-exact,
   the sort that is legal to crush per `project_integer_lookback_is_bit_exact_sort_not_scan_walled`); the per-tile
   **front-to-back over-composite is B17's A-buffer / OIT** (bit-exact, already both-backend). Author the whole
   pass as a CKIR compute kernel so it lowers to all five backends.
2. **B19-b — anti-aliasing + view consistency (mandatory).** Mip-Splatting's 3D smoothing + 2D Mip filter, and
   StopThePop's per-pixel hierarchical resort. Without these, zoom aliases and rotation pops — the two tells of
   a naïve 3DGS.
3. **B19-c — 2DGS surfels + mesh extraction.** This is the **bridge into the B1/B8 material + raster pipeline**:
   a 2D-disk representation has a real normal, gives clean depth via ray-splat intersection, and extracts a mesh
   (TSDF fusion, or GOF) that our existing material system can shade. Captured content → editable geometry.
4. **B19-d — compression (mandatory to ship).** Self-Organizing Gaussians turns the unordered splat set into a 2D
   grid + off-the-shelf image codec (**our own HDR/image codec**, per `project_own_hdr_image_codec_no_third_party`)
   → 20–40×. This is where our owned codec earns its place.
5. **B19-e — ray-traced + relightable Gaussians (the moat synergy).** 3DGRT traces Gaussian particles through a
   BVH of procedural-AABB proxies — **the same `build_scene_curves`/procedural-AABB path we built for hair LSS**,
   with a Gaussian intersector instead of a round-cone one. This unlocks secondary rays (reflection/refraction/
   shadows), distorted cameras, mixing splats with triangle geometry, and — with a BRDF decomposition — RELIGHTING
   captured content. Reuses C3 (`IRayTracingContext`) + B9 (`TraceRay*`) wholesale.
6. **B19-f — training / differentiable fit + LoD.** The backward pass through the forward rasterizer is
   **CKIR autodiff (v15/v16)** — the differentiable renderer is why 3DGS beat NeRF. Adaptive density control
   (clone/split/prune) via the modern **3DGS-MCMC** / **Taming 3DGS** schemes (principled, budgeted, fewer/better
   Gaussians than the 2023 heuristics). LoD (Octree-GS / Hierarchical-3DGS) for large scenes.

The through-line: **B19 is where four prior investments compound** — the OIT tiers (B17), the GPU sort, the
autodiff (v15/v16), and the RT procedural-primitive path (B18-f/C3/B9). That is the argument for doing it now,
right after hair.

## The core method (Kerbl et al., SIGGRAPH 2023) — §4

A scene is a set of **3D Gaussians**, each carrying:
- position **μ** (3), anisotropic **covariance Σ** stored as scale **s** (3) + rotation quaternion **q** (4), with
  Σ = R(q)·S(s)·S(s)ᵀ·R(q)ᵀ — this factoring keeps Σ positive-semidefinite under optimisation;
- **opacity α** (1);
- **colour as spherical-harmonic coefficients** (SH degree 0–3 → 3×{1,4,9,16} = up to 48 floats), so colour is
  **view-dependent** (specular-ish highlights fall out of the SH).

**Forward render — tile-based rasterisation ("splatting"), the EWA splat (Zwicker 2001):**
1. **Project** each 3D Gaussian to a 2D screen-space Gaussian: the 2D covariance is Σ′ = J·W·Σ·Wᵀ·Jᵀ, where W is
   the view rotation and J is the Jacobian of the affine approximation of the perspective projection at μ. This
   gives a 2D mean and a 2×2 covariance → an elliptical screen footprint.
2. **Cull + tile-bin**: screen is 16×16-pixel tiles; each Gaussian is assigned to every tile its 3σ ellipse
   overlaps, emitting one (Gaussian, tile) instance with a **sort key = (tileID, viewDepth)**.
3. **Global sort** all instances by that key — one radix sort. After this, each tile has its Gaussians in
   front-to-back depth order.
4. **Per tile, per pixel**, iterate the sorted Gaussians front-to-back: weight w = α_i · exp(−½·dᵀΣ′⁻¹d) (d =
   pixel − 2D mean), composite `C += c_i · w · T; T *= (1 − w)`, evaluate SH colour c_i in the view direction,
   stop when transmittance T falls below ε. **This inner loop IS the front-to-back `over` — B17's A-buffer.**

**Training — the differentiable half (why 3DGS won):**
- Optimise μ, s, q, α, SH by gradient descent (Adam) against captured images, loss = L1 + D-SSIM. The rasteriser is
  differentiable end to end → gradients flow to every Gaussian parameter. **This backward pass = CKIR autodiff.**
- **Adaptive density control**: clone/split Gaussians in under-reconstructed regions (large positional gradient),
  prune near-transparent or over-large ones, periodic opacity reset. The 2024 frontier replaces these heuristics
  (§ training).

## The frontier collection (organised by axis)

### Anti-aliasing + view consistency — MANDATORY
- **Mip-Splatting** [Yu et al., CVPR 2024]. 3DGS aliases when the sampling rate changes (zoom, focal-length,
  distance): tiny Gaussians erode, large ones dilate. Fix: (1) a **3D smoothing filter** capping each Gaussian's
  max frequency to the Nyquist limit set by the training views; (2) replace the original screen-space dilation
  with a **2D Mip filter** approximating the pixel's integration footprint. Alias-free at any zoom.
- **StopThePop** [Radl et al., SIGGRAPH 2024]. The per-tile sort uses ONE depth per Gaussian per tile, so blend
  order is a single approximation across the whole tile → **popping** when the view rotates and a Gaussian's sort
  order flips. Fix: **hierarchical per-pixel resort** so the blend order is correct per ray, + tile culling to keep
  it cheap. View-consistent, pop-free — important for interactive/VR.
- **AAA-Gaussians** [ICCV 2025] builds on both; artefact-free + anti-aliased.

### Geometry / mesh extraction — the bridge to materials
- **2DGS** [Huang et al., SIGGRAPH 2024]. Replace 3D ellipsoids with **oriented 2D disks (surfels)**: a disk has a
  well-defined normal (an ellipsoid's "surface" is ambiguous), depth is exact via **ray-splat intersection**, and
  **depth-distortion + normal-consistency regularisers** clean the geometry. Extract a mesh via **TSDF fusion** of
  rendered depth. Best thin-structure + boundary fidelity.
- **GOF (Gaussian Opacity Fields)**. Extract geometry DIRECTLY from 3D Gaussians via ray-traced volume rendering —
  no Poisson/TSDF step.
- **SuGaR** [Guédon & Lepetit, CVPR 2024]. Regularise Gaussians onto the surface → Poisson-reconstruct a mesh →
  bind Gaussians to it, giving an editable/animatable hybrid mesh+splat.

### Compression — MANDATORY to ship
- **Self-Organizing Gaussians** [fraunhoferhhi, ECCV 2024]. Sort the (unordered) Gaussian parameters into a **2D
  grid with local smoothness**, then compress the resulting attribute images with an **off-the-shelf image codec**
  → **20–40×**. The trick: turn a point cloud into an image and let image compression do the work. **This is where
  our OWN HDR/image codec is the natural payload.**
- **HAC (Hash-grid Assisted Context)** [ECCV 2024]. Context-model anchor features against a hash grid → top
  compression; 2nd-smallest to Self-Organizing at ECCV'24.
- **3dgs.zip survey** [CGF 2025] — the taxonomy: pruning, vector-quantisation, entropy coding, structured/anchor
  representations. **PCGS** (2025) adds progressive/streamable compression.

### Ray-traced + relightable — the moat synergy with C3/B9
- **3DGRT — 3D Gaussian Ray Tracing** [Moenne-Loccoz et al., SIGGRAPH Asia 2024; NVIDIA `nv-tlabs/3dgrut`].
  Ray-trace the Gaussian particles instead of rasterising: wrap each in a **bounding-mesh proxy** (icosahedron),
  build a **BVH**, trace, shade batches of intersections in depth order. Enables **secondary rays** (reflection,
  refraction, shadows), **distorted / rolling-shutter cameras**, and mixing splats with triangle meshes. Needs RT
  cores; slower than rasterisation but physically richer. **A Vulkan port exists as of Aug 2025.** ⭐ For us this is
  the **same procedural-AABB BLAS + custom intersector path we built for hair LSS** — a Gaussian intersector in
  place of the round-cone one, through the existing `IRayTracingContext`.
- **Relightable 3D Gaussians**. Decompose each Gaussian into a **BRDF** (albedo/roughness/metallic) + normal +
  incident light, ray-trace visibility/shadows → **relight captured content** under new lighting. **GaussianShader**
  (reflective surfaces), **IRGS** (inter-reflections via 2D-Gaussian ray tracing), **PTIR-GS** (path-traced inverse
  rendering with GI), **SSD-GS** (scattering/shadow decomposition).

### Training / densification frontier — quality per Gaussian
- **3DGS-MCMC** [Kheradmand et al., NeurIPS 2024]. Reframe densification as **Stochastic Gradient Langevin
  Dynamics** — add/move/remove Gaussians under one principled SGMCMC framework instead of clone/split heuristics.
  Better quality at a fixed Gaussian count; less tuning.
- **Taming 3DGS** [Mallick et al., SIGGRAPH Asia 2024]. **Budgeted** densification (metric-driven scores) +
  efficient per-splat backprop + **sparse Adam** → target a Gaussian budget, train faster.
- **Spectral-GS** [SIGGRAPH Asia 2025]. Spectral-entropy density control; fixes needle/blur artefacts.

### Large-scale + LoD
- **Hierarchical-3DGS** [Kerbl et al., SIGGRAPH 2024] — hierarchy + streaming for city-scale.
- **Octree-GS** — octree anchor Gaussians, LoD by camera distance; km-scale on a single GPU.
- **LODGE** — chunked LoD with dynamic loading + opacity-blended chunk boundaries. **CLoD-GS** — continuous LoD.

## Cerid / CKIR implementation notes (what maps where)

| 3DGS piece | Cerid capability it rides on | Status |
|---|---|---|
| per-tile front-to-back alpha composite | **B17 A-buffer / OIT** (bit-exact, both backends) | ✅ built |
| global (tile,depth) instance sort | **GPU radix / onesweep** (integer keys, bit-exact) | ✅ built |
| differentiable backward pass (training) | **CKIR autodiff v15 fwd / v16 rev** | ✅ built |
| ray-traced Gaussians (3DGRT) | **procedural-AABB BLAS + custom intersector (B18-f LSS pattern) via C3/B9** | ✅ built (curve variant) |
| compression payload | **our own HDR/image codec** | ✅ built |
| covariance projection, tile binning, EWA weight | plain CKIR compute math | to build (B19-a) |
| SH colour evaluation | CKIR compute (a small basis eval) | to build (B19-a) |
| Mip/StopThePop, 2DGS, MCMC densify, relight BRDF | new CKIR kernels | to build (B19-b…f) |

Author every pass as CKIR so it lowers to all five backends bit-exact — the mission (`feedback_mission_portable_gpu_compute_all_backends`). The sort key must stay **integer** (tile in the high bits, quantised depth in the low bits) to keep the sort bit-exact and legal to crush.

## Pitfalls / gotchas (anticipated; confirm at build)

- **The blend order is the whole ballgame.** A single per-tile depth (original 3DGS) pops on rotation; per-pixel
  resort (StopThePop) fixes it but costs more. Decide the tier per use (interactive → StopThePop; static → the
  cheap global sort). Our B17 already has both a static A-buffer and per-pixel machinery.
- **EWA projection is an AFFINE approximation** of the perspective transform — it degrades at wide FOV / screen
  edges. 3DGEER (2025) and the ray-traced path are the exact alternatives; note the approximation, don't fight it.
- **Densification is where quality and cost are decided.** The 2023 clone/split heuristics over-populate; use the
  MCMC/Taming budgeted schemes from the start.
- **Depth precision in the sort key.** Quantising view-depth into the low bits of an integer key loses precision at
  range — the same class as the DOM's z0 precision note in hair. Position the quantisation around the scene's depth
  range.
- **Mip-Splatting must be trained in, not just applied at render.** The 3D smoothing filter changes the optimised
  parameters; bolting only the 2D filter onto a non-Mip-trained scene is half the fix.
- **Training needs the camera-calibrated capture pipeline (SfM/COLMAP-class)** to produce the initial point cloud +
  poses. That input side is out of CKIR's scope — B19 renders + fits given poses; capture is a separate concern.

## Papers (the B19 collection)

Core: Kerbl et al. 2023 *3D Gaussian Splatting for Real-Time Radiance Field Rendering* (SIGGRAPH); Zwicker et al.
2001 *EWA Volume Splatting* (the projection). AA: Yu et al. 2024 *Mip-Splatting* (CVPR); Radl et al. 2024
*StopThePop* (SIGGRAPH); *AAA-Gaussians* (ICCV 2025). Geometry: Huang et al. 2024 *2DGS* (SIGGRAPH); *GOF*; Guédon
& Lepetit 2024 *SuGaR* (CVPR). Compression: Morgenstern et al. 2024 *Self-Organizing Gaussians* (ECCV); *HAC*
(ECCV 2024); Bagdasarian et al. 2025 *3dgs.zip survey* (CGF); *PCGS* 2025. Ray-traced/relight: Moenne-Loccoz et al.
2024 *3D Gaussian Ray Tracing* (SIGGRAPH Asia; `nv-tlabs/3dgrut`); *Relightable 3D Gaussians*; *GaussianShader*;
*IRGS*; *PTIR-GS*. Training: Kheradmand et al. 2024 *3DGS-MCMC* (NeurIPS); Mallick et al. 2024 *Taming 3DGS*
(SIGGRAPH Asia); *Spectral-GS* (SIGGRAPH Asia 2025). LoD: Kerbl et al. 2024 *Hierarchical-3DGS* (SIGGRAPH);
*Octree-GS*; *LODGE*; *CLoD-GS*.

Surveys: Chen & Wang 2024 *A Survey on 3D Gaussian Splatting* (arXiv 2401.03890, updated through 2026); Fei et al.
2025 *3D Gaussian Splatting as a New Era: A Survey* (IEEE TVCG); *Compression in 3DGS: A Survey* (arXiv 2502.19457).
