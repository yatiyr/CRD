# Recipe — 2D Gaussian Splatting (surfels): the geometrically-accurate radiance-field primitive

A **2D Gaussian Splat** (2DGS, Huang et al., *SIGGRAPH 2024*) represents a scene as a cloud of flat oriented
**disks** (surfels) instead of the 3D ellipsoids of 3DGS. Rendering is an **exact ray–surfel intersection**, which
buys two things the ellipsoid cannot: **view-consistent** splatting (a flat disk has no perspective-dependent
thickness) and a **geometrically meaningful depth + normal at every pixel** — the G-buffer a mesh extractor turns
into real surface geometry. This recipe teaches the primitive end to end: drive it from the parameter table, then
the maths (with the intersection derived), then the assembly, then the traps.

---

## 1. Parameters first

### Per-surfel (the primitive — 13 floats)

| Param | Meaning | Units | Default | Range / notes |
|---|---|---|---|---|
| `μ = (μx,μy,μz)` | disk **centre** in world space | world | — | anywhere in the scene |
| `s_u` | scale along the first tangent axis `t_u` | world | 0.05–0.6 | the disk's ellipse semi-axis; the 3σ footprint is ±3·s_u |
| `s_v` | scale along the second tangent axis `t_v` | world | 0.05–0.6 | `s_u=s_v` ⇒ a round disk; unequal ⇒ an ellipse |
| `q = (qx,qy,qz,qw)` | **orientation** quaternion (unit) | — | identity | its 3×3 columns are `t_u,t_v,t_w`; `t_w = t_u×t_v` is the surfel **normal** |
| `α` | opacity | — | 0.5–1.0 | multiplies the Gaussian falloff |
| `sh = (shR,shG,shB)` | SH degree-0 colour coefficients | — | — | screen colour = `0.5 + C0·sh`, `C0 = 0.28209479` |

The whole geometry is `(μ, s_u, s_v, q)`: a centre, two in-plane scales, and an orientation. The normal is **not**
stored — it is the third column of the rotation, so it is exact and always consistent with the tangent frame.

### Camera (20 floats, shared with 3DGS)

`[R00..R22 (row-major 3×3 view rotation) · tx ty tz · fx fy cx cy · near · imgW imgH]`. View space is
`v = R·μ + t`; a pixel's view-space ray is `d = ((px−cx)/fx, (py−cy)/fy, 1)`.

### Render knobs

| Param | Meaning | Default | Notes |
|---|---|---|---|
| 3σ footprint cull | keep only pixels with `u²+v² < 9` | 9 | the disk is unbounded; 3σ is where it is negligible |
| `alphaMin` | drop contributions below this α | 1/255 | skips imperceptible splats |
| degenerate ε | `|det| < ε` ⇒ surfel is edge-on, skip | 1e-12 | the ray lies in the disk plane, no intersection |

---

## 2. What it is / why it exists

3DGS renders millions of **3D ellipsoids** by the EWA splat: project each to a screen-space 2D Gaussian, sort,
alpha-composite. It looks great but has two structural weaknesses:

1. **View inconsistency** — the projected footprint of an ellipsoid depends on the viewing angle, so the same
   Gaussian contributes differently from different views. Multi-view training papers over this, but the underlying
   primitive is not a surface.
2. **No usable geometry** — an ellipsoid has no single depth or normal at a pixel; its "depth" is a soft blob.
   You cannot extract a clean mesh from a 3DGS cloud.

2DGS replaces the ellipsoid with a **flat disk**. A disk *is* a surface patch: it has one normal and, for any ray,
one intersection point with one depth. Rendering by ray–surfel intersection is therefore **view-consistent** (the
disk is the same object from every angle) and produces an **accurate depth + normal per pixel**. That per-pixel
geometry is exactly the input a TSDF / Poisson mesh extractor needs — 2DGS is the bridge from a captured radiance
field to real mesh geometry (and thence into a conventional material/mesh pipeline).

---

## 3. The maths — the ray–surfel intersection, derived

A surfel is the set of world points `P(u,v) = μ + s_u·u·t_u + s_v·v·t_v`, with the Gaussian weight
`G(u,v) = exp(−½(u²+v²))` in the disk's local `(u,v)` coordinates. Rendering a pixel means: find the `(u,v)` where
the pixel's ray pierces the disk's plane, evaluate `G` there, and composite.

We solve it in **view space** (which matches the 3DGS camera layout and avoids building a separate clip matrix).
Write the view-space centre `vc = R·μ + t` and the **scaled** view-space tangent axes
`A = s_u·(R·t_u)`, `B = s_v·(R·t_v)`. A pixel's ray is `λ·d`, `d = ((px−cx)/fx, (py−cy)/fy, 1)`, `λ>0` the depth
(because `d.z = 1`, the intersection's view-z is exactly `λ`). The intersection point must satisfy

```
vc + u·A + v·B = λ·d           (3 equations, unknowns u, v, λ)
  ⇔  u·A + v·B − λ·d = −vc
```

This is a 3×3 linear system with column matrix `[A | B | −d]` and right-hand side `−vc`. Solve by **Cramer's
rule** (`det[p,q,r] = p·(q×r)`, replace one column with the RHS per unknown). Working the cross products through:

```
BxD = B × d
DEN = A · BxD                         (= det[A, B, d]; the system determinant is −DEN)
u   = −(vc · BxD) / DEN
v   = −(A · (vc × d)) / DEN
λ   =  (A · (B × vc)) / DEN
```

**Sanity check (facing disk):** `vc=(0,0,L)`, `t_u=(1,0,0)`, `t_v=(0,1,0)`, `s_u=s_v=S`, so `A=(S,0,0)`,
`B=(0,S,0)`. For a ray `d=(dx,dy,1)`: `BxD=(S,0,−S·dx)`, `DEN=S²`, `u = L·dx/S`, `v = L·dy/S`, `λ = L`. At the
centre pixel (`dx=dy=0`) → `u=v=0, G=1, λ=L`. Exactly what geometry demands. ✔

Then `G = exp(−½(u²+v²))`, `α_eff = min(opacity·G, 0.99)`, and a **3σ footprint cull** drops the pixel when
`u²+v² ≥ 9`. Compositing is the standard front-to-back `over`, and — the geometry payoff — the intersection depth
`λ` and the surfel normal `N = normalize(R·t_w)` are **α-weighted-accumulated** alongside colour:

```
w      = α_eff · T                    (T = running transmittance, starts 1)
colour += c · w ;  depthSum += λ · w ;  normalSum += N · w ;  T *= (1 − α_eff)
```

The **surface depth** at a pixel is `depthSum / (1−T)` and the **surface normal** is `normalize(normalSum)` —
i.e. the alpha-expectation over the surfels the ray pierced. (Reference 2DGS also blends a screen-space low-pass
for near-edge-on splats and adds depth-distortion + normal-consistency regularisers **during training**; the
forward render here is the geometry core those build on.)

Papers: Huang, Yu, Chen, Geiger, Gao, *"2D Gaussian Splatting for Geometrically Accurate Radiance Fields"*,
SIGGRAPH 2024 — §3.1 (the 2D splat), §3.2 (the ray-splat intersection, our view-space form), §3.3 (depth/normal).

---

## 4. The full assembly

Two CKIR compute kernels (scalar tier ⇒ all maths component-wise), sharing the sort/binning machinery already
built for 3DGS:

1. **Project** (`build_gsplat2d_project_kernel`, one thread per surfel). Reads the 13-float surfel + camera,
   computes the quaternion→tangent-frame, the view-space centre `vc`, the scaled view tangents `A, B`, the view
   normal `N`, an approximate screen radius (for later tiling), the SH colour, and a validity flag (`vc.z > near`).
   Emits a **19-float prepared surfel**: `[vc(3) · A(3) · B(3) · N(3) · depth · colour(3) · opacity · radius · valid]`.
   Precomputing `A,B,N` here makes the per-pixel intersection a pure 3×3 solve.
2. **Depth sort** — sort the prepared surfels nearest-first by `vc.z`. Reuse the on-device KV radix sort (B19-a3);
   the forward render only ever sees depth-sorted input.
3. **Render** (`build_gsplat2d_render_kernel`, one thread per pixel). Loops the depth-sorted surfels, solves the
   Cramer intersection above, culls at 3σ / near / degenerate, over-composites colour + α-weighted depth + normal.
   Emits **8 floats per pixel**: `[R G B T · depthSum · Nx Ny Nz]`.
4. **Mesh bridge** — `depthSum/(1−T)` per pixel is the surface depth (`build_surface_depth_kernel`, ckir_mesh.hpp);
   fuse one or more posed depth maps into a **TSDF** voxel grid (`build_tsdf_integrate_kernel` — B19-c2a), then
   **marching cubes** on the fused field → a triangle mesh into the B1 material pipeline (`build_mc_*` — B19-c2b).
   **Both done** — the full 2DGS render → depth → TSDF → marching cubes chain runs on-device. Its own recipe teaches
   the mesh extraction end to end: `docs/recipes/2026-07-21-mesh-extraction-tsdf-marching-cubes.md`.

For many surfels, reuse the B19-a4 tile bin (tilecount → scan → scatter → sort-by-tile → ranges → block render);
the surfel's screen radius from the project kernel is the bbox the binner needs.

---

## 5. The traps

- **Divide-by-zero for edge-on surfels.** When the ray lies in the disk plane, `DEN → 0` and `u,v,λ` blow up to
  ±inf. If you then compute `α_eff = 0` (masked out) but still do `depthSum += λ·(α·T)`, you get `inf·0 = NaN` that
  poisons the depth buffer. **Fix:** compute a divide-safe denominator `DEN_safe = (|DEN|>ε) ? DEN : 1` for the
  divisions, and put `|DEN|>ε` in the keep-mask so the contribution is a clean zero. Never let a masked-out lane
  carry a NaN into an accumulator.
- **A big surfel covers a small test frame entirely.** At `fx=100` a 32×32 frame spans only `dx∈[−0.16,0.16]`; a
  surfel with `s=3` at depth 5 has a 3σ footprint far wider than that, so *every* pixel including the corners is
  covered — a "corner should be background" assertion then fails though the render is correct. Size the test
  surfel (or widen the FOV / enlarge the frame) so some region is genuinely outside 3σ. The footprint half-extent
  in pixels along x is `≈ 3·s·fx/(λ)`.
- **CKIR emission scars carry over.** The scalar compute tier evaluates Vec3/Dot to garbage — write every vector
  op component-wise (the kernel-eval-is-scalar rule). Bool comparison results must stay
  inline (never `stmt_materialize` a Bool — no bool temp type); materialize only the shared u32/f32 temps. The
  straight-line Cramer solve here has no `If`, so it emitted first try — but the moment you add an `If`/`For`, emit
  to a real backend, because the CPU oracle passes graphs the SPIR-V compiler rejects.
- **Depth is α-weighted, not the nearest hit.** The output `depthSum` is `Σ w·λ`; the surface depth is
  `depthSum/(1−T)`, not `depthSum`. Forgetting the `/(1−T)` normalisation makes the depth read low by a factor of
  the accumulated opacity. Same for the normal.

---

## 6. Measured numbers

- CPU oracle: facing-surfel depth = view-z exactly; **slanted-surfel per-pixel depth matches the exact plane
  formula `λ = (vc·N)/(d·N)` to < 2e-2** across the covered pixels (the geometric-accuracy property — a 3DGS
  ellipsoid gives a single flat depth); composite = near-over-far.
- Real Vulkan: GPU project + ray-surfel render == CPU oracle, **worst |GPU − oracle| = 1.4e-06** (prep 1.9e-06),
  24 surfels at 64×64. (Perf board to `docs/bench/` when the tiled surfel path + mesh bridge land.)

---

## 7. Where the code lives

- `engine/kir/include/crd/kir/ckir_gsplat2d.hpp` — `build_gsplat2d_project_kernel`, `build_gsplat2d_render_kernel`,
  the config structs and buffer layouts.
- `tests/kir/test_ckir_gsplat2d.cpp` — the closed-form project + facing/slanted/composite render gates (CPU oracle).
- `tests/gpu-context-vulkan/test_vulkan_gsplat.cpp` — the `[gsplat2d]` Vulkan gate (GPU == oracle).
- 3DGS sibling primitive + the shared sort/bin machinery: `ckir_gsplat.hpp`, `ckir_sort.hpp`, `ckir_scan.hpp`.
- Frontier dossier: `docs/research/2026-07-21-3dgs-frontier.md`. Session log: `docs/sessions/2026-07-21-b19-c-2dgs-surfels.md`.
