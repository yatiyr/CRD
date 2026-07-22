# 2026-07-21 — B19-c1: 2D Gaussian Splatting (surfels) — the geometrically-accurate primitive

**Detour:** D-007 GPU-program-system · **Slice:** B19 (3D Gaussian Splatting) · **Sub-slice:** B19-c1
**Directive:** "let's go" (continue B19 → the geometrically-accurate frontier axis + the bridge toward B1 materials).

## What shipped

A second radiance-field primitive alongside 3DGS: **2D Gaussian Splatting** (Huang et al., SIGGRAPH 2024). Where
3DGS is a cloud of 3D ellipsoids projected by the EWA splat, 2DGS is a cloud of flat oriented **disks** (surfels)
rendered by an **exact ray–surfel intersection**. Two payoffs the ellipsoid cannot give:

- **View-consistent** splats — a flat disk has no perspective-dependent thickness, so it looks identical from every
  angle.
- **Geometrically-meaningful depth + normal per pixel** — the disk *is* the surface, so every pixel has one
  intersection depth and one normal. That depth+normal G-buffer is the input a mesh extractor (TSDF/Poisson)
  consumes — the bridge from a captured radiance field into real mesh geometry and the B1 material pipeline.

Two CKIR compute kernels in `engine/kir/include/crd/kir/ckir_gsplat2d.hpp`:

1. **`build_gsplat2d_project_kernel`** — surfel `[μ · s_u s_v · quat · α · SH]` (13 floats) + camera → prepared
   view-space surfel `[vc · A=s_u·R·t_u · B=s_v·R·t_v · N · depth · colour · α · radius · valid]` (19 floats). The
   quaternion's three columns are the tangent frame `t_u,t_v,t_w`; the normal `t_w` is exact (never stored raw).
2. **`build_gsplat2d_render_kernel`** — per pixel, solve the 3×3 ray–surfel system `vc + u·A + v·B = λ·d` (view
   space, `d=((px−cx)/fx,(py−cy)/fy,1)`) by **Cramer's rule**, `G = exp(−½(u²+v²))` with a 3σ (`u²+v²<9`) cull,
   over-composite colour + α-weighted depth (`λ`) + normal. Output 8 floats/pixel `[R G B T · depthSum · Nx Ny Nz]`;
   surface depth `= depthSum/(1−T)`, surface normal `= normalize(normalSum)`.

The intersection is solved in **view space** (matching the 3DGS camera layout, no separate clip matrix). Derivation
verified against a facing disk: `u = L·dx/S`, `v = L·dy/S`, `λ = L`. Full derivation + the intersection formulas are
in the recipe.

## Gates

| gate | result |
|---|---|
| CPU oracle: project == closed-form tangent frame + normal + view centre | 16 assertions |
| CPU oracle: facing-surfel depth = view-z, normal = +z, red composite | exact (<1e-3) |
| CPU oracle: **SLANTED surfel per-pixel depth == exact plane `λ=(vc·N)/(d·N)`** | <2e-2 over covered pixels; left farther than right |
| CPU oracle: near surfel composites over far | red wins, surface depth ≈ near |
| `crd-kir-tests [gsplat2d]` (4 cases) | 36 assertions — PASS |
| **real Vulkan** `[gsplat2d]`: GPU project+render == oracle | **worst 1.4e-06** (prep 1.9e-06), 24 surfels 64×64, 9 assertions |

The slanted-surfel depth gradient is the substance of the gate: a 3DGS ellipsoid gives one flat depth per splat,
whereas 2DGS gives the true per-pixel plane depth — that is the geometric-accuracy property, pinned against the
closed-form plane intersection.

## Traps hit

- **NaN from edge-on surfels.** When the ray lies in the disk plane, `DEN → 0` and `u,v,λ` blow up; a masked-out
  lane still doing `depthSum += λ·(α·T)` yields `inf·0 = NaN`. Fixed with a divide-safe denominator
  `DEN_safe = (|DEN|>ε)?DEN:1` for the divisions plus `|DEN|>ε` in the keep-mask.
- **A big test surfel covers the whole small frame.** `s=3` at depth 5 with `fx=100` on a 32×32 frame has a 3σ
  footprint wider than the frame, so the "corner is background" check failed though the render was correct. Sized
  the facing-test surfel smaller (`s=0.6`) with a wider FOV (`fx=50`) so corners fall outside 3σ. (Physics, not a
  bug — noted in the recipe.)
- The straight-line Cramer solve emitted to SPIR-V first try (no `If`/`For` scoping traps this time) — a contrast
  with the B19-a4 ranges kernel, where the nested `If` needed the materialize discipline.

## State

B19-c1 DONE. `ckir_gsplat2d.hpp` + both test files tidy-clean (pinned LLVM 20.1.8). New recipe
`docs/recipes/2026-07-21-2d-gaussian-splatting-surfels.md`. context.md + detour updated.

**Next:** B19-c2 — the **mesh bridge**: unproject the depth+normal G-buffer to an oriented point cloud, fuse
across views (TSDF) or Poisson-reconstruct, march cubes → a triangle mesh into the B1 material pipeline. Then
StopThePop per-pixel resort, and the cross-backend DX12/HLSL pass (B19 is Vulkan-only so far).
