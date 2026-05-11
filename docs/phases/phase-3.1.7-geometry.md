# Phase 3.1.7 — `crd-geometry`: computational-geometry substrate

**Status:** ⏳ **next-active** (research dossier closed 2026-05-11;
ADR-0076 Accepted + Amended 2026-05-11 §12; slice list locked;
**first-light kickoff IMMEDIATELY after Phase 3.1 v1b cluster close
(sweep-PASS gates the pivot)** — not after Phase 3.1.6 `crd-hesap`
as originally locked. Sequence pivot per ADR-0076 §12: full 29-slice
phase executes BEFORE Phase 3.1 v1c so eylem v1c+v1d+v1d-mesh and
sdf v2 consume geometry from day 1, dissolving the deferred-refactor
debt the original sequence required.)

**Research dossier:** `docs/research/cerid-geometry.md` (11,523 words —
industry survey, algorithmic scope, determinism contract, module split,
slice list, consumer integration plan, slot decision)

**ADR:** `docs/decisions/0076-geometry-substrate-architecture.md` (Accepted 2026-05-11)

**Cornerstones:** ADR-0063 (determinism contract — Cerid-internal
predicates, deterministic-sort SAH BVH split, deterministic GJK simplex
update) · PRINCIPLES.md ("modular by default", "single-path", "no STL
in hot paths" — algorithms work over `crd::containers::Array` /
`ConstSpan` not `std::vector`)

**Supersedes:** none — net-new module slot.

## Why this exists

Computational geometry is genuinely cross-cutting in Cerid. The
following modules ALL need spatial-acceleration structures + convex
shape ops + triangle mesh queries + polygon/mesh processing:

- **eylem broadphase** (v1c) — dynamic AABB tree (BVH refit + insert + query + raycast)
- **eylem narrow phase** (v1d) — GJK distance + EPA penetration depth + MPR alternative
- **eylem mesh collider** (v1d-mesh) — triangle mesh closest-point + raycast (BVH-accelerated)
- **eylem convex hull collider** — convex hull build + conditioning (V-HACD-class decomposition for editor)
- **crd-sdf v2 mesh-bake** — winding-number test (Jacobson 2013 — robust on non-watertight meshes) + BVH closest-point
- **crd-renderer** (Phase 3.5+) — frustum cull + occlusion BVH
- **crd-scene `SpatialBVHIndex`** (ADR-0053 reserved IComponentIndex shell) — the actual BVH implementation
- **crd-audio** (Phase 3.4) — acoustic ray-casts on level geometry
- **crd-eylem-aero** (ADR-0073 reserved) — aerodynamic surface evaluation on triangle mesh
- **crd-eylem-cine** (ADR-0074 reserved) — animated mesh queries for cinematic-bridge
- **navmesh** (Phase 7) — Delaunay tetrahedralisation + connectivity graphs
- **editor** (Phase 7) — V-HACD pipeline + selection geometry + picking + manipulator gizmos

Without a shared substrate, EACH consumer ships its own BVH +
GJK/EPA + closest-point — divergent perf, divergent determinism, 3-5×
maintenance burden. The Cerid pattern (`crd-sdf`, `crd-hesap`) for
cross-cutting domain substrates is to ship as a peer module to
`crd-math`, NOT bloat `crd-math` itself.

## Why NOT bloat `crd-math`

Same reasoning that motivated `crd-sdf` (ADR-0064 §2) and `crd-hesap`
(ADR-0065 §2):

1. **Compile-time tax** — every module that links `crd-math` would
   recompile every header that includes a triangulation algorithm or
   BVH builder template. Currently `crd-math` is ~30 headers; bloated
   it'd be ~150.
2. **Scope conflation** — `crd-math` is "vectors / matrices / quats /
   determinism wrappers / SIMD substrate". Computational geometry is
   "spatial reasoning over arbitrary shape representations". Different
   tier of abstraction.
3. **Tooling tier mismatch** — many crd-geometry algorithms (V-HACD,
   QEM simplification, Delaunay tetrahedralisation) are cooker/editor
   tier — they shouldn't live in the runtime engine's leaf math module.
4. **Independent evolution** — `crd-geometry-bvh`'s GPU mirror needs
   `crd-rhi` (for VkBuffer staging); `crd-math` must NEVER depend on
   `crd-rhi` (cycle).

## Multi-domain consumer matrix

| Consumer | Sub-module(s) needed | When |
|---|---|---|
| eylem v1c broadphase | `crd-geometry-bvh` | when 3.1.7 lands; ships own narrow BVH first |
| eylem v1d narrow phase | `crd-geometry-convex` (GJK + EPA) | when 3.1.7 lands; ships own GJK first |
| eylem v1d-mesh TriangleMesh collider | `crd-geometry-mesh` (closest-point, raycast) + `crd-geometry-bvh` | when 3.1.7 lands |
| crd-sdf v2 mesh-bake | `crd-geometry-mesh` (winding number) + `crd-geometry-bvh` | when 3.1.7 lands; ships own mesh-bake first |
| crd-renderer 3.5+ frustum cull | `crd-geometry-bvh` | natural at 3.5 |
| crd-scene SpatialBVHIndex | `crd-geometry-bvh` | reserved shell; lights up when 3.1.7 lands |
| crd-audio 3.4 acoustic raycasts | `crd-geometry-bvh` + `crd-geometry-mesh` | natural at 3.4 |
| crd-eylem-aero (ADR-0073) | `crd-geometry-mesh` (surface eval) | natural at v6f |
| crd-eylem-cine (ADR-0074) | `crd-geometry-mesh` (animated queries) | natural at v4d |
| editor V-HACD pipeline | `crd-geometry-decomposition` | Phase 7 |
| navmesh | `crd-geometry-delaunay` + `crd-geometry-mesh` | Phase 7 |

## Refactor reservation pattern (eylem v1c + v1d)

Same precedent already in the codebase:

- **eylem v7 FEM** ships own narrow internal PCG → consumes
  `crd-hesap-iterative` later (per Phase 3.1.6 doc).
- **eylem v9 differentiable** ships its own narrow autodiff → consumes
  `crd-hesap-autodiff` later.

For v1c + v1d the same applies:

- **eylem v1c (broadphase)** ships its own dynamic AABB tree (Catto
  GDC 2019) inside `crd-eylem-rigid3d`. When `crd-geometry-bvh`
  lands, refactor to consume it. Refactor is non-API-breaking — the
  broadphase System's public surface stays.
- **eylem v1d (narrow phase)** ships its own GJK + EPA inside
  `crd-eylem-rigid3d`. Refactors to consume `crd-geometry-convex`
  when 3.1.7 lands.

This lets eylem v1 ship without blocking on a 4-6 month substrate
slip; the refactor is bounded scope + delivers measurable perf +
maintenance wins when it happens.

## Slot in the broader Cerid roadmap

**Recommended:** Phase 3.1.7 (after `crd-hesap` 3.1.6, before Phase
3.2 animation). The ordering is:

1. `crd-sdf` (3.1.5) — implicit-surface representation, narrow scope
2. `crd-hesap` (3.1.6) — numerical computing, narrow scope
3. `crd-geometry` (3.1.7) — computational geometry, narrow scope

All three are substrate modules that **don't depend on each other**
internally (modulo trivial imports of `crd-math` types). The slot
ordering is determined by *consumer-side dependency* — Phase 3.5+
renderer consumes BVH; Phase 3.4 audio consumes BVH; eylem v1c+v1d
consume convex/BVH but ship own first → 3.1.7 lands BEFORE Phase 3.2
animation so renderer/audio/eylem-refactor can light up cleanly.

**Alternative considered:** Phase 3.1.55 (between sdf and hesap). The
case for this ordering is that geometry is closer to sdf in
dependency direction (both are spatial). The case AGAINST is that
hesap's matrix solvers are needed by some geometry ops (sparse
Cholesky for parameterisation, eigendecomposition for QEM
simplification's quadric error ellipsoids); hesap-first lets
crd-geometry consume crd-hesap-direct for the matrix-heavy parts.

**Decision:** Phase 3.1.7 (after hesap), driven by the matrix-solver
dependency.

## Slice list — locked (research dossier §8 + ADR-0076 §7)

| Slice | Scope | LOC | Calendar |
|---|---|---|---|
| **v0a–v0d** | primitives substrate: types (`Plane`/`Ray`/`AABB`/`OBB`/`Sphere`/`Capsule`/`Triangle3`/`Frustum`) + closest-point formulas + intersection tests + barycentric utilities | ~3000 | ~1 wk |
| **v0e** | iq-formulary primitives substrate — polynomial + exponential smin operators (`smin_poly`, `smin_exp`) + domain-repeat / domain-mirror / domain-warp ops + the shader-helpers cooker generator skeleton (no GPU side yet); ALSO lands the `crd::math::simd::reduce_argmax_with_lex_tiebreak` substrate primitive that v3 Quickhull SIMD reduction needs | ~1000 | ~3 days |
| **v1a–v1f** | binned-SAH BVH + O(n) refit + insert/erase via Catto 2019 tree rotations + quad-BVH + closest-point + Embree benchmark | ~5000 | ~2 wk |
| **v1g** | BVH4 quad-topology promoted to default + SIMD ray-vs-4-AABB intersect kernel (`Vec4f` lanes) | ~500 | ~3 days |
| **v2a–v2f** | GJK distance + GJK boolean + EPA penetration + SAT box-pair fast path + tiebreak conformance tests | ~3000 | ~2 wk |
| **v3a–v3c** | 2D monotone chain + 3D Quickhull (Barber 1996) + hull simplification | ~2000 | ~1 wk |
| **v4a–v4f** | TriangleMeshView + half-edge + mesh closest-point + mesh raycast (Möller-Trumbore in leaves) + winding number (Jacobson 2013) + sdf mesh-bake refactor | ~3000 | ~2 wk |
| **v4g** | per-leaf SIMD triangle intersection — `Vec8f` Möller-Trumbore over 8 triangles per BVH leaf | ~300 | ~2 days |
| **v5a–v5e** | KD-tree + loose octree (Ulrich 2000) + R-tree (Beckmann 1990) + spatial hash (Teschner 2003) + scene-IComponentIndex bring-up (lights up ADR-0053 reserved shells) | ~3000 | ~2 wk |
| **v6a–v6e** | ear clipping triangulation + CDT + Sutherland-Hodgman + Vatti polygon Boolean + Bentley-Ottmann sweep | ~3000 | ~2 wk |
| **v7a–v7g** | QEM (Garland-Heckbert 1997) + Loop subdivision (Loop 1987) + isotropic remesh (Botsch-Kobbelt 2004) + hole filling (Liepa 2003) + manifoldness fix + self-intersection removal + Taubin smoothing (Taubin 1995) | ~4000 | ~3 wk |
| **v8a–v8d** | 2D Bowyer-Watson + 2D CDT + 3D Bowyer-Watson + Voronoi-from-Delaunay extraction | ~2000 | ~1 wk |
| **v9a–v9d** | GPU LBVH builder (Karras 2012+2013) + GPU BVH refit + V-HACD convex decomposition (Mamou 2014, editor-tier) + REPL bindings | ~8000 | ~3 wk |
| **v9e** | `crd-geometry-shader-helpers` GLSL/HLSL output side — cooker emits primitive library + iq smin/domain-ops from formula-IR manifest seeded at v0e; ULP-conformance test against C++ reference; first-light consumer = `crd-renderer` Phase 3.5+ DFAO/DF-soft-shadow | ~4000 | ~2 wk |

**Total: ~29 slices, ~15.8 KLOC engine + ~5 KLOC editor-tier + ~4 KLOC
cooker-emitted GLSL/HLSL, ~5–7 months calendar** (was 25 / 14 + 5 /
4–6 months prior to supplement-dossier additions).

## Performance budgets (per supplement dossier §4.1)

Targets per sub-module on a Zen 4 reference CPU (Win-shipping config,
full LTO + AVX2). Per-slice DoD measures against these:

| Sub-module / op | Target | Reference |
|---|---|---|
| BVH4 traversal — ray-vs-4-AABB node test | ~8 ns/node on AVX2 | Embree publishes ~6 ns; ~30% gap accepted for determinism |
| GJK — convex pair distance | 50–200 ns/pair, 2–6 iterations typical | Box2D v3 published envelope |
| Quickhull — 100k points to closed 3D hull | <30 ms | qhull reference: ~25 ms on identical input |
| Mesh closest-point — 100k-tri BVH-accelerated, `Vec8f` lane batch | <1 µs / 8-point batch | Embree per-leaf SIMD pattern |
| QEM simplification — 1M-tri to 100k-tri | <3 s | Garland-Heckbert ~2 s; ~50% headroom for determinism + container overhead |
| LBVH GPU build (v9a) — 1M primitives on RTX 3060 | <8 ms | Karras 2012 reports 4–6 ms on Fermi |

SIMD posture per sub-module (full table in supplement §4.1):
`-primitives` SIMD-natural; `-bvh` SIMD-critical; `-convex` partial;
`-mesh` SIMD batch queries; `-spatial` mostly scalar; `-polygon`
minimal SIMD; `-mesh-processing` SIMD for QEM only; `-delaunay` mostly
scalar; `-gpu` already SIMT.

## Reference reading

- **Base research dossier:** `docs/research/cerid-geometry.md`
  (11,523 words — industry survey + algorithmic scope + module split
  + slice plan + Phase 3.1.7 slot decision)
- **Supplement dossier:** `docs/research/cerid-geometry-supplement.md`
  (6,116 words — Inigo Quilez body of work + computational-geometry
  textbook foundation + SIMD performance contract + `crd::containers`
  integration patterns + GLSL/HLSL shader-helper proposed sub-module)
- **ADR:** `docs/decisions/0076-geometry-substrate-architecture.md`
  (Accepted 2026-05-11; supplement additions integrated 2026-05-11)
- **Foundational textbooks** (per supplement §3): Ericson "Real-Time
  Collision Detection" (v0/v1/v2 anchor); Eberly "Geometric Tools"
  (v0/v3 anchor); de Berg et al. "Computational Geometry" (v6/v7/v8
  anchor); Botsch et al. "Polygon Mesh Processing" (v7 anchor);
  Akenine-Möller et al. "Real-Time Rendering 4e" (broad).
- **Foundational web reference:** [iquilezles.org](https://iquilezles.org/articles/)
  — Inigo Quilez's article catalog (analytic distance functions 2D + 3D,
  smooth-blending operators, domain ops, raymarching, soft shadows, AO).

## Open questions (resolved by dossier)

1. Robust geometric predicates: Shewchuk 1997 adaptive precision vs
   fixed-precision-with-epsilon — pick one, document the contract.
2. Half-edge mesh: ship our own (~2K LOC) vs wrap CGAL (GPL — reject)
   vs wrap libigl (Eigen-based — reject for STL-discipline reasons) vs
   wrap OpenMesh (LGPL, large) — likely answer: ship our own, modeled
   on libigl's API but using `crd::containers`.
3. GPU BVH algorithm: LBVH (Lauterbach 2009 — Morton-code based) vs
   PLOC vs HLBVH — start with LBVH (simplest), reserve PLOC for v9b.
4. Convex decomposition: V-HACD is offline-only; whether a runtime
   decomposer is even useful in Cerid (probably not — editor tier).
5. Determinism strategy: how the cross-platform replay-hash CI
   exercises crd-geometry primitives (the dossier proposes a
   `crd-geometry-bench` config matrix mirroring eylem's v9b CI).
6. **Shader-helper library ownership** (added 2026-05-11 from
   supplement dossier §6.3): `crd-geometry-shader-helpers` (Cerid
   owns it from `crd-geometry`'s side) vs `crd-sdf-shader-helpers`
   (Cerid owns it from `crd-sdf`'s side). The math is geometric (shape
   distance + closest-point); the primary GPU-side consumer is
   sdf rendering (DFAO, DF-soft-shadow). **Decision deferred to
   Phase 3.1.5 close** — when crd-sdf's shader requirements are
   concrete, the consumer-weighted answer becomes clear. Until then,
   treat the library as Cerid-owned + name-agnostic; the cooker-side
   generator implementation is identical regardless of which module
   owns the symbol.

## References

Industry survey (in research dossier, ~10-12K words):
- **Game/physics**: Bullet btDbvt, PhysX PxBVH + Pxg GJK, Jolt, Havok HKBV, Box2D v3 b2DynamicTree, Unity DOTS, Unreal FBVHTree, Godot
- **Geometric processing**: CGAL (academic reference), Geogram (Inria), libigl (Eigen-based), Geometric Tools / Eberly (GTE), Open3D, OpenMesh, MeshLab core, PMP
- **Spatial acceleration**: Embree (Intel — gold standard), nanoRT, Boost.Geometry, S2 (Google geo)
- **Robotics**: FCL (Flexible Collision Library — exact analogue of what eylem needs), HPP-FCL, OMPL primitives
- **Polygon/mesh**: Vatti, Greiner-Hormann, Sutherland-Hodgman, Boost.Polygon, Clipper2 (Angus Johnson)
- **Convex decomposition**: V-HACD, HACD, CoACD
- **Tetrahedralisation**: TetGen, fTetWild
- **Mesh repair**: MeshFix, libigl::is_edge_manifold, OpenMesh mesh-doctor

Foundational papers cited in the dossier (preliminary):
- Shewchuk 1997 — Adaptive precision floating-point arithmetic + fast
  robust geometric predicates
- Jacobson 2013 — Robust inside-outside segmentation using
  generalised winding numbers (the standard for non-watertight mesh)
- Garland & Heckbert 1997 — Surface simplification using quadric
  error metrics (QEM)
- Lauterbach 2009 — Fast BVH construction on GPUs (LBVH)
- Karras 2012 — Maximizing parallelism in the construction of BVHs,
  octrees, and k-d trees
- Catto GDC 2019 — Dynamic AABB Tree (the eylem v1c reference)
- Bowyer 1981 + Watson 1981 — Computing Dirichlet tessellations (2D
  Delaunay)

(Full citation list lands in the research dossier.)
