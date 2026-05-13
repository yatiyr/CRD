# ADR-0076 — `crd-geometry` substrate architecture

**Status:** Accepted (2026-05-11) · **Amended (2026-05-11)** — sequence pivoted to precede Phase 3.1 v1c instead of following Phase 3.1.6 (see §12).

**Phase:** 3.1.7 — executed **after Phase 3.1 v1b cluster close and BEFORE Phase 3.1 v1c (broadphase)**. Originally slotted after Phase 3.1.6 `crd-hesap` and before Phase 3.2 animation; resequenced by the §12 amendment to eliminate the deferred-refactor debt on eylem v1c/v1d and sdf v2.

**Tags:** [substrate] [architecture] [computational-geometry] [bvh]
[gjk-epa] [mesh-processing] [determinism]

**Supersedes:** none — net-new module slot.

**Cornerstones:** ADR-0063 (eylem determinism contract — inherited
wholesale) · ADR-0050 (storage hint hierarchy — `crd-geometry` plays no
role here, but its consumers do) · ADR-0061 (async GPU upload — `crd-
geometry-gpu` v9a/v9b consumes the same `UploadHandle`/`Fence` pattern)
· PRINCIPLES.md ("modular by default", "single-path", "no STL in hot
paths" — algorithms work over `crd::containers` not STL)

**Research dossier:** `docs/research/cerid-geometry.md` (11,523 words —
industry survey across Bullet/PhysX/Jolt/Box2D v3/Havok/Unity DOTS/
Unreal/Godot/CGAL/Geogram/libigl/GTE/Open3D/OpenMesh/MeshLab/PMP/
Embree/nanoRT/Boost.Geometry/S2/FCL/HPP-FCL/OMPL/Sutherland-Hodgman/
Vatti/Greiner-Hormann/Boost.Polygon/Clipper2/V-HACD/HACD/CoACD/
TetGen/fTetWild/MeshFix/qhull; algorithmic scope across 13 domains;
determinism contract; module split across 10 sub-modules; ~25-slice
plan; consumer integration plan across 12 named consumers; three-way
slot decision)

**Phase plan:** `docs/phases/phase-3.1.7-geometry.md`

---

## 1. Decision

Cerid ships **`crd-geometry`** as a **standalone peer module** to
`crd-math` / `crd-sdf` / `crd-hesap` (NOT bloated into `crd-math`),
providing computational-geometry primitives + spatial-acceleration
structures + convex shape ops + triangle-mesh queries + polygon ops +
mesh processing + Delaunay/Voronoi + convex decomposition (editor tier)
+ GPU mirror (Phase 3.5+).

Module split (10 sub-modules, ~14 KLOC engine + ~5 KLOC editor-tier
across ~25 slices over ~4-6 months):

1. **`crd-geometry-primitives`** (v0, ~3.8 KLOC) — point/line/segment/ray/
   plane/triangle/sphere/AABB/OBB/capsule distance + intersection +
   closest-point + frustum tests + barycentric utilities; plus (v0e) the
   iq smooth-blending / domain-op formulary; plus (v0f — added by the §13
   amendment) the cutting-edge / branchless / SIMD intersection corpus
   (watertight ray-tri Woop 2013, Baldwin-Weber 2016 ray-tri, branchless
   NaN-safe slab ray-AABB, Ize 2013 robust-traversal ray precompute,
   Plücker edge classification, Ericson Voronoi-region closest-point-on-
   triangle, `Vec4f`/`Vec8f` batch kernels). **The §13 amendment also
   move-and-deletes the pre-existing `crd::math::geometry`** (404 LOC —
   `Ray`/`Plane`/`Sphere`/`AABB`/`Triangle`/`Frustum` + ~16 closest-point/
   intersect helpers) into `crd::geometry::primitives::*`; `crd-math`
   thereafter ships only Vec/Mat/Quat/Transform/SIMD/`deterministic`. See
   §13.
2. **`crd-geometry-bvh`** (v1, ~5 KLOC) — `BvhTree<AABB>` with binned-
   SAH builder + O(n) refit + O(log n) insert/remove via tree rotations
   (Catto GDC 2019). Quad-BVH topology default; binary-BVH variant.
   Raycast / overlap query / closest-point.
3. **`crd-geometry-convex`** (v2 + v3, ~4 KLOC) — GJK distance + EPA
   penetration + SAT box-pair fast path; 2D monotone chain hull + 3D
   Quickhull (Barber-Dobkin-Huhdanpaa 1996); hull simplification.
4. **`crd-geometry-mesh`** (v4, ~3 KLOC) — `TriangleMeshView` (read-only),
   half-edge mesh data structure, BVH-accelerated mesh closest-point +
   raycast (Möller-Trumbore in leaves), generalised winding number
   (Jacobson 2013 — robust on non-watertight).
5. **`crd-geometry-spatial`** (v5, ~3 KLOC) — KD-tree, loose octree
   (Ulrich 2000), R-tree (Beckmann 1990), spatial hash (Teschner 2003).
6. **`crd-geometry-polygon`** (v6, ~3 KLOC) — 2D ear-clipping
   triangulation, constrained Delaunay, Sutherland-Hodgman convex clip,
   Vatti polygon Boolean (Clipper2 conventions), Bentley-Ottmann sweep.
7. **`crd-geometry-mesh-processing`** (v7, ~4 KLOC) — Quadric Edge
   Collapse Decimation (Garland & Heckbert 1997), Loop subdivision
   (Loop 1987), isotropic remeshing (Botsch & Kobbelt 2004), hole filling
   (Liepa 2003), manifoldness repair, self-intersection removal,
   Taubin smoothing (Taubin 1995).
8. **`crd-geometry-delaunay`** (v8, ~2 KLOC) — 2D + 3D Bowyer-Watson,
   2D CDT, Voronoi-from-Delaunay extraction.
9. **`crd-geometry-gpu`** (v9, ~3 KLOC) — GPU LBVH builder
   (Karras 2012 + 2013), GPU BVH refit, GPU per-pair GJK.
10. **`crd-geometry-decomposition`** (v9, ~5 KLOC editor-tier only) —
    V-HACD-class convex decomposition (Mamou 2014); cooker-only.
11. **`crd-geometry-shader-helpers`** (cooker-emitted, ~2 KLOC GLSL +
    ~2 KLOC HLSL + manifest + generator; landed via supplement
    research dossier 2026-05-11) — GLSL/HLSL primitive library
    mirroring `crd-geometry-primitives` formulas + iq's published
    smooth-blending / domain operators (polynomial smin, exponential
    smin, opSmoothUnion / Subtraction / Intersection, domain repeat /
    mirror / warp). Cooker emits CPU + GPU sides from a single
    formula-IR manifest; CI conformance test asserts ULP-bounded
    match (1 ULP at f32) between GLSL output and the C++ scalar
    reference. Consumed by `crd-sdf` (analytic-storage backend),
    `crd-renderer` Phase 3.5+ (DFAO, DF-soft-shadow, volumetric SDF),
    `crd-font` (MTSDF), editor (analytic-collider preview).

## 2. Why a separate module (not in `crd-math`)

Same reasoning that motivated `crd-sdf` (ADR-0064 §2) and `crd-hesap`
(ADR-0065 §2):

1. **Compile-time tax.** `crd-math` is currently ~30 headers; bloated
   with computational-geometry templates it would balloon to ~150.
   Every module that links `crd-math` (which is everything) would pay
   the recompile bill.
2. **Scope conflation.** `crd-math` is "vectors / matrices / quats /
   determinism wrappers / SIMD substrate" — leaf-tier numerics.
   `crd-geometry` is "spatial reasoning over arbitrary shape
   representations" — a higher tier of abstraction.
3. **Tooling tier mismatch.** Many algorithms (V-HACD, QEM
   simplification, Delaunay tetrahedralisation) are cooker / editor
   tier — they shouldn't live in the runtime engine's leaf math module.
4. **Independent evolution.** `crd-geometry-gpu`'s LBVH refit needs
   `crd-rhi` (for VkBuffer staging); `crd-math` must NEVER depend on
   `crd-rhi` (cycle).
5. **Multi-domain consumer breadth.** 12 named consumers (eylem
   broadphase + narrow + mesh; sdf mesh-bake; renderer cull; scene
   SpatialBVHIndex shell from ADR-0053; audio raycasts; eylem-aero;
   eylem-cine; navmesh; editor V-HACD; medical/scientific Delaunay) —
   exactly the cross-cutting reach that justifies its own module.

## 3. Multi-domain consumer matrix

| Consumer | Sub-modules | When | Pattern |
|---|---|---|---|
| eylem v1c broadphase | `-bvh` | **consumes from day 1** (post-amendment 2026-05-11) | no ships-own / refactor — see §12 |
| eylem v1d narrow phase | `-convex` (GJK + EPA) | **consumes from day 1** (post-amendment 2026-05-11) | no ships-own / refactor — see §12 |
| eylem v1d-mesh TriangleMesh collider | `-mesh` + `-bvh` | **consumes from day 1** (post-amendment 2026-05-11) | first-light when 3.1.7 ships |
| eylem convex hull collider conditioning | `-convex` (Quickhull) | **consumes from day 1** (post-amendment 2026-05-11) | depends-on-substrate |
| crd-sdf v2 mesh-bake | `-mesh` (winding number) + `-bvh` | **consumes from day 1** (post-amendment 2026-05-11) | no ships-own — ADR-0064 §4 deferred-refactor obsolete after §12 |
| crd-renderer Phase 3.5+ frustum cull | `-bvh` | natural at 3.5 | first-light |
| crd-scene SpatialBVHIndex (ADR-0053 reserved IComponentIndex shell) | `-bvh` | reserved shell | lights up when 3.1.7 lands |
| crd-audio Phase 3.4 acoustic raycasts | `-bvh` + `-mesh` | natural at 3.4 | first-light |
| crd-eylem-aero (ADR-0073 reserved) | `-mesh` (surface eval) | natural at v6f | first-light |
| crd-eylem-cine (ADR-0074 reserved) | `-mesh` (animated queries) | natural at v4d | first-light |
| navmesh Phase 7 | `-delaunay` + `-mesh` + `-spatial` | natural at Phase 7 | first-light |
| editor Phase 7 V-HACD pipeline | `-decomposition` | natural at Phase 7 | first-light |

## 4. Determinism contract — ADR-0063 inheritance

`crd-geometry` inherits the ADR-0063 deterministic-FP contract
**wholesale**: same compiler flags (`/fp:precise`, `-ffp-contract=off`),
same Cerid-internal transcendentals (`crd::math::deterministic`), same
`crd-no-std-math-check` + `crd-no-std-sort-check` + `crd-no-non-ascii-test-names`
CI guards (auto-fire on `engine/geometry/**`).

Algorithm-specific tiebreaks pinned at substrate level (research
dossier §5):

1. **GJK simplex update**: Ericson reference (not van den Bergen) for
   the "remove which vertex" decision. Documented in `-convex` impl;
   conformance test in v2f.
2. **BVH SAH split**: tiebreak on axis order X-then-Y-then-Z when two
   axes have equal cost. Documented in `-bvh` impl.
3. **Quickhull**: deterministic when ties broken by lex order on
   coordinates (X then Y then Z). Documented in `-convex` impl.
4. **Vatti polygon Boolean**: Clipper2 vertex-on-edge convention pinned
   (vertex-on-edge counted as "on the edge", not on either side).
   Documented in `-polygon` impl.
5. **Floating-point predicates**: v0 ships **fixed-precision predicates
   with `Geometry::kPredicateEpsilon`** (documented value, conformance
   tests). v1+ may upgrade to Shewchuk 1997 adaptive predicates
   conditioned on observed wrong-sign / non-determinism rates from
   `crd-sdf` v2 mesh-bake stress tests (research dossier §11.1 open
   question).
6. **Sort everywhere uses `crd::containers::sort` (merge sort —
   stable + deterministic per ADR-0063)**. No `std::sort` in
   `engine/geometry/**` (CI guard inherited).

**SIMD-specific tiebreak pins** (added 2026-05-11 from supplement
dossier §4.3):

7. **SIMD reductions use the fixed pairwise-binary-tree from
   `crd::math::simd::reduce_*`** — never lane-order accumulation,
   which is non-associative under FP and produces compiler-dependent
   results.
8. **SIMD comparisons return all-bits-set masks; tiebreaks select
   via `select_lane(mask, candidate_a, candidate_b)`** with
   deterministic candidate-ordering pinned per algorithm
   ("prefer lower-index lane on equal values" — Cerid convention).
9. **BVH binned-SAH uses INTEGER bin counts** (not FP histograms);
   the only FP operation in cost computation is per-bin cost, which
   uses `crd::math::deterministic::*` (no `std::log`).
10. **Quickhull's "furthest-point" SIMD reduction uses
    `reduce_argmax_with_lex_tiebreak`** — pins X-then-Y-then-Z lex
    tiebreak across SIMD lanes when two lanes have equal furthest-
    distance. **This primitive does not exist in `crd::math::simd`
    today** — v0 of `crd-geometry` lands it as a `crd-math::simd`
    extension before v3 (Quickhull) needs it.
11. **Shader-helper library GLSL/HLSL output is held to a 1-ULP-
    at-f32 conformance budget** against the C++ scalar reference;
    cooker validates per-emit, CI gates on the assertion.

**Cutting-edge intersection corpus tiebreak pins** (added 2026-05-11
with the §13 amendment, for the v0f sub-slice):

12. **Watertight ray-triangle (Woop 2013)**: the ray-transform axis
    selection (Woop picks the largest-magnitude component of the ray
    direction as the "z" axis) is pinned to **X-then-Y-then-Z on
    magnitude ties**; the edge-function products `U = Cx·By − Cy·Bx`
    etc. are evaluated in the published fixed order; the back-face /
    on-edge tests use `>= 0` / `<= 0` (closed) so that for two
    triangles sharing an edge, the ray either hits both or hits the
    consistent one — never neither. The 2-1-0 edge-permutation order
    is the dossier §13 reference order, identical regardless of input
    vertex winding.
13. **Plücker-coordinate edge classification**: the six permuted
    coordinate products of the Plücker side-operator are summed in a
    fixed `(d0·m1 + d1·m2 + d2·m0) − (m0·d1 + m1·d2 + m2·d0)`-shaped
    order (no compiler reassociation — the sum goes through the
    `crd::math` deterministic-FP path); a result of exactly `±0.0` is
    counted as **on the line** (Cerid convention), not on either side.
    Batched `Vec8f` Plücker tests use `crd::math::simd` pairwise-tree
    reductions, same as pin 7.

GPU geometry kernels (v9 onward) match CPU bit-exactly via the same
predicate strategy — same approach as `crd-hesap-gpu` (ADR-0065).

## 5. API design philosophy

Two-layer API mirrors `crd-hesap` (ADR-0065 §3):

- **Typed C++ Eigen-class layer** for engine code (eylem, sdf,
  renderer): templated on the scalar type / bvh-topology / shape variant;
  zero-overhead inlining; data-oriented spans.
- **Opt-in cooker/editor façade** for tooling: handle-based, allocator-
  agnostic, JSON/TOML serialisation hooks for cooker integration.

Common conventions across both layers:

- **Data-oriented**: pass `crd::containers::ConstSpan<T>` of
  vertex/index data; never `Mesh*` objects. Consumer owns storage.
- **Functional algorithm form**: `bvh_build(...) → BvhTree` not
  `BvhTree::build(...)`. Consumers stash trees in their own allocators.
- **ECS-friendly**: BVH refit is a System; spatial hash is an
  IComponentIndex shell; results are arrays of `(entity, hit_data)`.
- **Allocator discipline** per CLAUDE.md: `IAllocator*` constructor
  argument throughout.
- **GPU-first reservations**: BVH leaves map cleanly to GPU AABB sets
  for `crd-rhi` consumers in v9.
- **No exceptions**: error paths return `std::optional` or sentinel
  values per ADR-0063 (no exception throwing in deterministic paths).
- **SIMD substrate commitment** (added 2026-05-11 from supplement
  dossier §4): hot-path algorithms use `crd::math::simd::Vec4f` /
  `Vec8f` / `Soa<TChunk, Lane>` exclusively for batched lanes; never
  raw intrinsics. AoSoA layout is the default storage for batched
  primitive collections (BVH leaves, point clouds, triangle batches).
  Per-sub-module SIMD posture documented in supplement dossier §4.1
  (table). `crd::containers::Array` / `ConstSpan` / `HashMap` /
  `String` are the canonical owning + view + map + string types
  throughout — no STL containers in hot paths per CLAUDE.md rule.
  Sort uses `crd::containers::sort` (merge sort, deterministic per
  ADR-0063); no `std::sort`.

## 6. Slot in the broader Cerid roadmap — Phase 3.1.7

> **Amended 2026-05-11 — see §12.** This section preserves the
> originally-locked sequencing for historical record. The current
> execution order is: Phase 3.1 v1b ✅ closed → **Phase 3.1.7 (full,
> all 29 slices) executes next** → Phase 3.1 v1c resumes after 3.1.7
> close → Phase 3.1.5 (sdf) stays interleaved between eylem v2 / v3
> → Phase 3.1.6 (hesap) sequential successor to eylem. The
> deferred-refactor pattern documented below is OBSOLETED by the
> amendment: eylem v1c/v1d/v1d-mesh + sdf v2 all consume `crd-geometry`
> from day 1 with no ships-own / refactor step.

### Original decision (superseded by §12)

**Decision: Phase 3.1.7** (between `crd-hesap` 3.1.6 and Phase 3.2
animation), per research dossier §10.3. Three options analysed; this
one wins on architectural clarity (one substrate-tier per quarter, in
dependency order: physics → implicit geometry → numerics → explicit
geometry).

**Deferred-refactor pattern** locked for eylem v1c + v1d (and crd-sdf
v2) — **OBSOLETED 2026-05-11 per §12**; preserved here so the original
reasoning is not lost when reading old session logs:

- **eylem v1c (broadphase)** ships its own dynamic AABB tree (Catto
  GDC 2019) inside `crd-eylem-rigid3d`. When `crd-geometry-bvh`
  v1 lands, refactors to consume it. Refactor is non-API-breaking —
  the broadphase System's public surface stays.
- **eylem v1d (narrow phase)** ships its own GJK + EPA inside
  `crd-eylem-rigid3d`. Refactors to consume `crd-geometry-convex`
  v2 when 3.1.7 lands.
- **crd-sdf v2 (mesh-bake)** ships its own BVH + winding-number
  test in 3.1.5 v2. Refactors to consume `crd-geometry-mesh` v4 when
  3.1.7 lands. ADR-0064 §4 already documented as deferred refactor.

This mirrors the existing precedent: **eylem v7 FEM ships own narrow
PCG → consumes `crd-hesap-iterative` later**. Same reasoning, same
refactor shape, same documentation pattern. (eylem v7 / hesap
relationship is unaffected by the §12 amendment; hesap stays sequential
successor to eylem.)

## 7. Slice list (~25 slices)

Per research dossier §8 + phase plan `docs/phases/phase-3.1.7-geometry.md`:

| Slice | Scope | LOC | Calendar |
|---|---|---|---|
| **v0a** | `crd-geometry-primitives` skeleton + primitive types (`Line`/`Segment`/`Ray`/`Plane`/`AABB`/`OBB`/`Sphere`/`Capsule`/`Triangle3`/`Frustum`). **§13 move-and-delete:** absorbs `crd::math::geometry` (types + ~16 helpers → `crd::geometry::primitives::*`), deletes `engine/math/include/crd/math/geometry.hpp`, repoints ~9 consumers; `crd-math` thereafter lean | ~700 + refactor | ~3 days |
| **v0b** | closest-point formulas (point → everything) + Ericson Voronoi-region closest-point-on-triangle | ~800 | ~3 days |
| **v0c** | intersection tests (everything-vs-everything: ray-X, AABB-tri Akenine-Möller 2001, OBB-OBB 15-axis SAT, tri-tri Möller 1997, sphere-X, capsule-capsule, frustum-X) | ~1500 | ~4 days |
| **v0d** | barycentric + 3-tetrahedron utilities | ~200 | ~1 day |
| **v0e** | iq-formulary primitives substrate — polynomial + exponential smin operators + domain-repeat / domain-mirror / domain-warp ops + the shader-helpers cooker generator skeleton (no GPU side yet); ALSO lands the `crd::math::simd::reduce_argmax_with_lex_tiebreak` substrate primitive that v3 Quickhull SIMD reduction needs | ~1000 | ~3 days |
| **v0f** | cutting-edge / branchless / SIMD intersection corpus (§13) — watertight ray-tri (Woop/Benthin/Wald 2013) + Baldwin-Weber 2016 ray-tri (precomputed) + branchless NaN-safe slab ray-AABB (Tavianator / Williams 2005) + Ize 2013 robust-traversal ray precompute (`RayPacket` for `-bvh` v1g) + Plücker edge classification + `Vec4f`/`Vec8f` batch kernels (ray-vs-N-AABB, ray-vs-N-triangle, N-sphere, segment-vs-N-segment, AABB-vs-N-AABB) + ULP-conformance + watertight-shared-edge property tests | ~700–900 | ~4 days |
| **v1a–v1f** | binned-SAH BVH + refit + insert/erase + quad-BVH + closest-point + Embree benchmark | ~5000 | ~2 wk |
| **v1g** | BVH4 quad-topology promoted to default + SIMD ray-vs-4-AABB intersect kernel (`Vec4f` lanes) | ~500 | ~3 days |
| **v2a–v2f** | GJK distance + GJK boolean + EPA + SAT box-pair + tiebreak conformance | ~3000 | ~2 wk |
| **v3a–v3c** | 2D monotone chain + 3D Quickhull + hull simplification | ~2000 | ~1 wk |
| **v4a–v4f** | TriangleMeshView + half-edge + mesh closest-point + mesh raycast + winding number + sdf mesh-bake refactor | ~3000 | ~2 wk |
| **v4g** | per-leaf SIMD triangle intersection — `Vec8f` Möller-Trumbore over 8 triangles per BVH leaf | ~300 | ~2 days |
| **v5a–v5e** | KD-tree + loose octree + R-tree + spatial hash + scene-IComponentIndex bring-up | ~3000 | ~2 wk |
| **v6a–v6e** | ear clipping + CDT + Sutherland-Hodgman + Vatti + Bentley-Ottmann | ~3000 | ~2 wk |
| **v7a–v7g** | QEM + Loop subdivision + isotropic remesh + hole fill + manifoldness fix + self-intersection removal + Taubin smoothing | ~4000 | ~3 wk |
| **v8a–v8d** | 2D Bowyer-Watson + 2D CDT + 3D Bowyer-Watson + Voronoi extraction | ~2000 | ~1 wk |
| **v9a–v9d** | GPU LBVH builder + GPU refit + V-HACD (editor-tier) + REPL bindings | ~8000 | ~3 wk |
| **v9e** | `crd-geometry-shader-helpers` GLSL/HLSL output side — cooker emits primitive library + iq smin/domain-ops from formula-IR manifest seeded at v0e; ULP-conformance test against C++ reference; first-light consumer = `crd-renderer` Phase 3.5+ DFAO/DF-soft-shadow | ~4000 | ~2 wk |

**Total: ~30 slices, ~16.6 KLOC engine + ~5 KLOC editor-tier + ~4 KLOC
cooker-emitted GLSL/HLSL, ~5–7 months calendar** (was 25 / 14 + 5 /
4–6 months prior to supplement-dossier additions; supplement adds v0e,
v1g, v4g, v9e plus the 11th sub-module `crd-geometry-shader-helpers`
which is mostly cooker-emitted; the §13 amendment adds v0f and folds
the move-and-deleted `crd::math::geometry` ~0.4 KLOC into v0a).

## 8. What's OUT of scope

Per research dossier §4.14:

- **Implicit-surface representation** — that's `crd-sdf` (ADR-0064).
  Boundary stated three times in the dossier (§1, §3 intro, §4.14).
- **Purely-numerical methods** — sparse linear solves consumed
  internally by `crd-geometry-mesh-processing` (e.g., LSCM
  parameterisation needs sparse Cholesky) live in
  `crd-hesap-direct` / `crd-hesap-iterative`.
- **2D image processing** — out of substrate scope.
- **Mesh streaming / IO format parsers** (.obj, .ply, .glb) — those
  live in the cooker (ADR-0042 + Phase 2.7).
- **Path planning** — that's a navmesh consumer of `crd-geometry`,
  not part of `crd-geometry` itself.

## 9. Risks + mitigations

1. **Substrate slip impacts every downstream consumer.** Mitigation:
   eylem v1c+v1d + sdf v2 ship their own narrow versions FIRST per
   the deferred-refactor pattern. The substrate landing is an
   improvement, not a gate.
2. **Determinism across compilers harder than for `crd-hesap`.**
   Computational geometry is full of tiebreak edge cases that diverge
   across MSVC / clang / gcc on subtle FP rounding. Mitigation:
   substrate-level tiebreak rules pinned in this ADR §4; conformance
   tests per slice; cross-platform replay-hash CI lights up at v9b.
3. **Module proliferation.** Cerid is now planning peer modules
   `crd-math` + `crd-sdf` + `crd-hesap` + `crd-geometry` — 4 substrate
   modules. Mitigation: each is genuinely cross-cutting (multi-domain
   consumer evidence in §3); the alternative (bloating `crd-math`)
   has worse compile-time + scope-coupling consequences.
4. **CGAL / Geogram are GPL / LGPL.** Cerid cannot wrap them; we
   must implement primary algorithms ourselves. Mitigation: research
   dossier surveys algorithms (not implementations); we re-implement
   from papers. Adds ~6-9 months over a "wrap CGAL" alternative,
   bought back by license freedom + Cerid-determinism control.

## 10. Open research questions (deferred)

Per research dossier §11:

1. **Robust geometric predicates**: fixed-precision-with-epsilon vs
   Shewchuk 1997 adaptive precision. v0 ships fixed-precision; v1+
   may upgrade conditioned on `crd-sdf` v2 mesh-bake stress-test data.
2. **Half-edge mesh ownership**: ship our own (~2K LOC) modeled on
   libigl's API but using `crd::containers`. Decision: ship our own.
3. **GPU BVH algorithm**: LBVH (Lauterbach 2009) vs PLOC (Meister-
   Bittner 2018). Decision: LBVH first (simpler), PLOC reserved for
   v9b polish.
4. **Runtime convex decomposition**: V-HACD is offline-only; whether
   a runtime decomposer is needed in Cerid. Decision: editor-tier only
   for v9; revisit if real workload surfaces.
5. **Differentiable geometry hooks**: integration with
   `crd-hesap-autodiff` for differentiable mesh deformation.
   Decision: reserved for v9d+ post-eylem-v9 differentiable physics.
6. **Shader-helper library ownership** (added 2026-05-11 from
   supplement dossier §6.3): `crd-geometry-shader-helpers` (Cerid
   owns from `crd-geometry`'s side) vs `crd-sdf-shader-helpers`
   (Cerid owns from `crd-sdf`'s side). The math is geometric (shape
   distance + closest-point); but the primary GPU-side consumer is
   sdf rendering (DFAO, DF-soft-shadow). **Decision deferred to
   Phase 3.1.5 close** — when crd-sdf's shader requirements are
   concrete, the consumer-weighted answer becomes clear. Until then,
   treat the library as Cerid-owned + name-agnostic; the cooker-side
   generator implementation is identical regardless of which module
   owns the symbol.

## 11. Acceptance + closure

**Accepted 2026-05-11.** Research dossier locked at 11,523 words.

ADR locks substrate architecture + module split + deferred-refactor
pattern + Phase 3.1.7 slot. Per-slice details mint as the slices ship
(starting v0 at Phase 3.1.7 kickoff after 3.1.6 `crd-hesap` closes).

Phase plan rows + ROADMAP entries reserved 2026-05-11; first-light
expected ~Phase 3.1.7 kickoff (estimated ~mid-2027 given the
3.1.5 + 3.1.6 sequencing ahead).

## 12. Amendment 2026-05-11 — sequence pivot to precede Phase 3.1 v1c

**What changed.** Phase 3.1.7 (`crd-geometry`) is now executed
**immediately after Phase 3.1 v1b cluster close** and **BEFORE
Phase 3.1 v1c (broadphase)**, instead of its original slot after
Phase 3.1.6 (`crd-hesap`). The full 29-slice plan ships in this
pivoted slot — no scope reduction, no narrow-subset shortcut.

The deferred-refactor pattern documented in §3 (consumer table) and
§6 (subsection) is **OBSOLETED**: eylem v1c (broadphase), v1d (GJK +
EPA), v1d-mesh (TriangleMesh + closest-point + raycast), v1d's
ConvexHull conditioning (Quickhull), and crd-sdf v2 (mesh-bake)
all consume `crd-geometry` from day 1 with **no ships-own narrow
version**. The §3 table rows have been updated in-place; §6's
"original decision" subsection is preserved verbatim so prior
session logs remain auditable, with a callout that the
deferred-refactor pattern it describes is no longer active.

**Sequencing impact.** Execution order through end of Phase 3.1
becomes:

1. Phase 3.1 v1b ✅ closed (sweep PASS gates closure)
2. **Phase 3.1.7 — all 29 slices, ~5–7 months calendar**
3. Phase 3.1 v1c (broadphase) — consumes `crd-geometry-bvh`
4. Phase 3.1 v1d (narrow phase) — consumes `crd-geometry-convex`
5. Phase 3.1 v1d-mesh — consumes `crd-geometry-mesh`
6. Phase 3.1 v1e–v1k as planned
7. Phase 3.1.5 (`crd-sdf`) — stays interleaved between eylem v2 / v3;
   v2 mesh-bake consumes `crd-geometry-mesh` directly (no refactor)
8. Phase 3.1 v3–v9 as planned
9. Phase 3.1.6 (`crd-hesap`) — unchanged: sequential successor to
   Phase 3.1; eylem v7 FEM still ships own PCG and refactors to
   `crd-hesap-iterative` later (that deferred-refactor remains intact;
   only the geometry one is dissolved)

**Calendar implication.** ~5–7 months before eylem v1c can resume.
Original plan had v1c starting within days of v1b close; amended plan
has v1c starting after geometry's full slice list ships. This is the
explicit tradeoff: shorter physics critical-path lost in exchange for
zero substrate refactor debt.

**Rationale.** Two converging arguments:

- **PRINCIPLES.md "single-path / no shortcuts"** (memory:
  `feedback_quality_bar.md` — "Cerid quality bar: elite, no shortcuts,
  single-path"). The deferred-refactor pattern is a shortcut by
  construction — it produces throwaway BVH / GJK / EPA / winding-number
  code that exists only until the substrate lands. ~1500 LOC of
  throwaway across eylem + sdf if the original sequence had run. The
  user reviewed this tradeoff explicitly and chose to pay the calendar
  cost to avoid the shortcut.

- **The substrate-tier sequencing rationale in the original §6
  strengthens rather than weakens under the pivot.** Original §6
  argued for "physics → implicit geometry → numerics → explicit
  geometry" as the dependency order. The amendment observes that
  eylem's broadphase + narrow-phase ARE explicit geometry consumers
  from day 1, so the more honest ordering is **physics-interface →
  explicit geometry → physics-runtime → implicit geometry → numerics**
  (eylem v1a interface ✅, eylem v1b storage ✅, geometry, eylem v1c+
  runtime, sdf, hesap).

**What is not affected by this amendment.**

- Architecture (§1–§5): module split, 11 sub-modules, API design
  philosophy, determinism contract, SIMD/containers commitments
  — all unchanged.
- Slice list (§7): same 29 slices, same LOC budget, same calendar.
- Out-of-scope list (§8): unchanged.
- Risks (§9): unchanged.
- Open research questions (§10): unchanged; the v1+ adaptive-precision
  question still resolves on sdf v2 mesh-bake stress-test data, which
  is now produced by an in-substrate consumer (not a deferred-refactor
  consumer), so the experimental signal is cleaner if anything.

**Memory cross-reference.** Saved as project memory
`project_phase_sequencing_pivot.md` so the decision survives context
compaction.

## 13. Amendment 2026-05-11 — `crd::math::geometry` move-and-delete + v0f cutting-edge intersection corpus

**What changed (two coupled decisions, reviewed by the user before
v0a kickoff).**

### 13.1 `crd::math::geometry` → `crd-geometry-primitives` (move-and-delete)

`engine/math/include/crd/math/geometry.hpp` (404 LOC) already ships
`Ray<T>` / `Plane<T>` / `Sphere<T>` / `AABB<T>` / `Triangle<T>` /
`Frustum<T>` plus ~16 helpers (`closest_point`, `intersects`,
`contains`, `signed_distance`, `intersect_ray_plane`,
`intersect_ray_sphere`, `intersect_ray_triangle`, `intersects(Frustum,
AABB|Sphere)`), consumed by ~9 files (`crd-scene` `world.hpp` +
`query.hpp` — the reserved spatial-DSL operators; `crd-math` umbrella +
`format.hpp`; `tests/math`, `tests/bench`, `tests/scene`,
`runtime/examples/smoke_math.cpp`).

**Decision: move-and-delete** (option (a) of the three the user was
shown — vs (b) thin alias shim, (c) coexist additive). v0a of
`crd-geometry-primitives` absorbs these types + helpers as
`crd::geometry::primitives::*` (extended with the dossier's full v0
catalogue — `Line`/`Segment`, `OBB`, `Capsule`, `Triangle3` etc.);
`engine/math/include/crd/math/geometry.hpp` is **deleted**; the ~9
consumers are repointed to `<crd/geometry/primitives/*.hpp>`. After
v0a, `crd-math` ships **only** Vec / Mat / Quat / Transform / SIMD
wrappers / `crd::math::deterministic` — the lean leaf substrate its
design intent always was (§2.2 of the research dossier). Rationale:
one source of truth for `AABB`/`Sphere`/etc. semantics, no decade-long
drift between two copies, and the leaf math module stays small (the
same reasoning that motivated the separate-module decision in §2). The
refactor is ~1 day across math + scene + tests; it is the smallest of
the three options' long-term cost even though it carries the only
non-zero up-front churn.

`crd-math`'s existing scalar Möller-Trumbore (`intersect_ray_triangle`)
becomes the v0f cross-check reference before deletion (v0f asserts the
new watertight + Baldwin-Weber + batch implementations agree with it on
the non-degenerate corpus).

### 13.2 New v0f sub-slice — cutting-edge / branchless / SIMD intersection corpus

The dossier's §4.13 "GTE catalogue" is the curated 2005-era Ericson /
Eberly set. The user asked for the cutting-edge bar raised; v0f adds
the modern production-renderer corpus that the base dossier + supplement
did not enumerate (all branchless, all SIMD-friendly, all shipped in
Embree / production engines):

- **Watertight ray–triangle** — Woop, Benthin, Wald (2013), JCGT 2(1).
  Shear+scale ray transform → edge-function form; guarantees a ray
  hits both or the consistent one of two triangles sharing an edge
  (Möller-Trumbore does not). Becomes the default ray-tri for
  `crd-geometry-mesh` v4d BVH leaves and for `crd-sdf` v2 mesh-bake
  (correctness on imperfect / non-watertight meshes; resolves part of
  the §10.1 robust-predicates open question for the ray-tri case).
- **Baldwin–Weber (2016)**, JCGT 5(3) — ray-tri via a precomputed
  3×4 unit-triangle affine transform; per-ray test is 9 mul + 6 add,
  branchless. The opt-in default for cooked static meshes
  (`MeshResource` BVH, static colliders), 48 B/tri storage cost.
- **Branchless NaN-safe slab ray–AABB** — Tavianator formulation
  (`tmin = max(min(...))` / `tmax = min(max(...))` with IEEE min/max
  ordering) over Williams, Barrus, Morley, Shirley (2005), JGT 10(1)
  precomputed `inv_dir` + sign-mask. Zero hot-path conditionals.
- **Robust BVH-traversal ray precompute** — `RayPacket` per Ize
  (2013), JCGT 2(2): `inv_dir` + per-axis sign flags + the
  boundary-consistent comparison constants. Consumed by `-bvh` v1g
  (the partner to Woop watertight tri — no holes at node boundaries).
- **Plücker-coordinate edge classification** — sign-only line /
  segment / ray vs triangle, and segment vs segment, side tests.
  Fully branchless; `Vec8f`-batchable. The clean "which side"
  primitive for GJK / clipping / triangle-orientation.
- **Ericson Voronoi-region closest-point-on-triangle** (RTCD §5.1.5)
  — the near-branchless `closest_point(Triangle3, p)` (v0b) — the
  form GJK fallback / EPA / capsule-vs-mesh need, not a naive
  plane-project + clamp.
- **SIMD batch kernels** (`Vec4f` / `Vec8f` wrappers over the scalar
  cores; AoSoA via `crd::math::simd::Soa`): ray-vs-N-AABB (broadphase
  prefilter + BVH leaf), ray-vs-N-triangle (Woop & Möller-Trumbore
  variants), N-sphere-vs-N-sphere, segment-vs-N-segment closest-pair
  (capsule-vs-mesh inner loop), AABB-vs-N-AABB overlap mask.
- **Tests** — per-algorithm scalar correctness + degenerate-input
  torture (zero-area tri, ray ∥ tri, ray origin on AABB face,
  antipodal segments) + ULP-conformance of each SIMD batch kernel
  vs its scalar core + watertight-shared-edge consistency property
  test + cross-check vs `crd-math`'s migrated Möller-Trumbore.

~700–900 LOC + ~25–35 test cases; slots after v0e (last v0 sub-slice,
still inside the v0 primitives block — i.e. at the *beginning* of the
phase, per the user's ask). Determinism pins: ADR §4 items 12–13.

**Why a discrete v0f rather than folding into v0c** (the user picked
v0f over the fold): the corpus is a coherent deliverable with its own
DoD (the ULP-conformance + watertight-property tests), it has
downstream consumers named at the substrate level (`-mesh` v4d,
`-bvh` v1g, `crd-sdf` v2), and a distinct slice keeps it visible in
the slice ledger rather than buried in a 2200-LOC v0c.

### 13.3 What is not affected by this amendment

- §1–§5 architecture (modulo the §1 sub-module-1 description edit and
  the §4 pins 12–13 above), §6 sequencing, §8 out-of-scope, §9 risks,
  §10 open questions (the robust-predicates question narrows slightly
  — watertight ray-tri removes the ray-tri sub-case from it).
- Slice list (§7): now **30 slices** (was 29); LOC ~16.6 KLOC engine
  (was 15.8); calendar unchanged (~5–7 months — v0f's ~4 days absorb
  into the v0 week-plus, the move-and-delete refactor is ~1 day).

### 13.4 Research record

The cutting-edge corpus + the move-and-delete rationale are documented
in `docs/research/cerid-geometry.md` §13 (addendum, 2026-05-11) with
full citations added to that dossier's §12.3.

## 14. Amendment 2026-05-13 — v0b: 2D peer set + dimension-suffix naming rule

**What changed (reviewed by the user before v0b kickoff).** v0a's
"Naming rule" pin said `crd-geometry-primitives` was 3D-only and 2D
types would land later in `crd-geometry-polygon` (v6) — `Triangle3`
carried the `3` solely because `Triangle2` was foreseen there. The
user's multi-domain mandate (robotics, aerospace, mechanics, PCB /
electrical sims, animation, games) wants 2D as a first-class peer of
3D in the *primitive* tier, not deferred. So v0b adds the full 2D peer
catalogue to `crd-geometry-primitives` now, and the naming rule is
re-pinned:

* All shape types are templated on the scalar `T` (`crd::math::MathScalar`).
* Where a concept has both a 2D and a 3D form **under the same name**,
  BOTH carry a dimension suffix — `Line2`/`Line3`, `Segment2`/`Segment3`,
  `Ray2`/`Ray3`, `AABB2`/`AABB3`, `OBB2`/`OBB3`, `Triangle2`/`Triangle3`,
  `Capsule2`/`Capsule3` — mirroring `crd::math::Vec2`/`Vec3`/`Mat2`/`Mat3`.
  This means `Line`/`Segment`/`Ray`/`AABB`/`OBB`/`Capsule` from v0a are
  **renamed** to their `…3` forms (mechanical; ~8 consumer files, no
  behaviour change — the same set v0a's move-and-delete touched).
* Where the 2D and 3D forms have distinct natural names, neither is
  suffixed — `Circle` (2D) / `Sphere` (3D).
* Where only one dimension exists, no suffix — `Plane` and `Frustum`
  stay 3D-only (a 2D half-space boundary is a `Line2` carrying a
  normal+offset; there is no 2D frustum). `Point2`/`Point3` are aliases
  of `Vec2<T>`/`Vec3<T>`.

ADR-0053's "frozen" `Query::in_aabb(crd::geometry::primitives::AABB<f32>)`
signature is unaffected in shape; the type spelling becomes `AABB3<f32>`
as part of this rename (the freeze pinned the parameter *form*, not the
literal identifier).

`crd-geometry-polygon` (v6) keeps its 2D-mesh-processing scope (ear-
clipping, Vatti, CDT, Bentley-Ottmann); it now consumes the `…2`
primitive types from this tier instead of declaring its own.

**Slice impact.** v0b grows from the dossier's ~800 LOC to ~1.6 KLOC
engine (the 2D type catalogue + 2D closest-point overloads + the
segment↔segment pair `closest_points`) + ~450 LOC tests; the +~0.8 KLOC
absorbs into the v0 budget (the v0 week-plus has slack). Slice count
unchanged (still 30). Calendar unchanged. v0c–v0f gain 2D counterparts
of their intersection / barycentric / formulary / SIMD work as they land
(no new slices — the 2D path rides each existing slice).

**What is not affected.** §1–§5 architecture, §6 sequencing, §8
out-of-scope, §9 risks, §10 open questions, the §12 sequence pivot, the
§13 move-and-delete + v0f corpus — all unchanged.

## 15. Amendment 2026-05-13 — checklist-driven additions (3 new slices + the `crd-geometry-viz` companion module)

**What changed (reviewed by the user against a domain-checklist after
`-bvh` v1a–v1d landed).** The user's checklist — *primitives* (incl.
convex hull + mesh views), a *unified query API* (raycasts / overlaps /
contains / distance / sweep tests / intersection tests / signed-distance
+ iq-style SDF utilities), *acceleration* (BVH / dynamic BVH / uniform
grid / spatial hash / loose octree / broadphase query API), *validation*
(brute-force-vs-BVH / randomized / degenerate / large-coordinate /
epsilon policy / NaN-Inf guards), *debug draw* (ray hits / normals /
closest points / BVH nodes / frustum culling / overlap pairs) — overlaps
the existing plan heavily but surfaces real gaps. The gaps are folded in
as **three new slices** plus tweaks to v2 / v5 / v9e; the architecture
principles are pinned in §16.

**New slices (slot: after `-bvh` v1g, before `-convex` v2):**

* **v1h — primitives-substrate hardening.** `crd/geometry/primitives/
  constants.hpp` — the geometry-wide *epsilon / tolerance policy* named
  by intent, not magnitude (`k_distance_epsilon`, `k_area_epsilon`,
  `k_parallel_epsilon`, `k_degenerate_extent_epsilon`, `k_sah_cost_epsilon`,
  `k_default_fat_margin`, `k_robust_aabb_pad_ulps`, …); the ad-hoc
  `1e-6F` / `default_epsilon` uses (v1a SAH-cost ε, v0f Ize ray-AABB
  pad, …) retrofit onto it. `is_finite.hpp` — `is_finite(primitive)`
  for every type, plus the **NaN/Inf contract** (§16.3). `signed_distance.hpp`
  — Inigo Quilez's ~30 **analytic signed distance functions in C++**
  (`sd_sphere` / `sd_box` / `sd_round_box` / `sd_box_frame` / `sd_plane`
  / `sd_capsule` / `sd_cylinder` / `sd_cone` / `sd_torus` / `sd_triangle`
  / `sd_ellipsoid` / `sd_octahedron` / … + 2D peers) — `closest_point.hpp`
  (v0b) gives the *unsigned* distance; this adds the *signed,
  negative-inside* form; it is the C++ scalar reference
  `crd-geometry-shader-helpers` (v9e) emits GLSL/HLSL twins of and
  `crd-sdf` v0 reuses (so `crd-sdf` v0's "analytic primitives" become a
  thin domain-side wrapper, not a re-derivation). New view type
  `ConvexHullView<T>` in `primitives.hpp` (non-owning: `ConstSpan<Vec3>`
  vertices + `ConstSpan<Plane>` faces + `ConstSpan<u32>` face-vertex
  indices — the *query-side* hull; `-convex` v3 *produces* one;
  `crd-eylem`'s `Collider::ConvexHull` references one).
* **v1i — unified query facade + shapecast + broadphase-pair API +
  validation discipline.** `crd/geometry/queries.hpp` — `raycast` /
  `overlap` / `closest_point` / `contains` / `distance` as **compile-time
  overload-polymorphic** free functions over `{primitive, BvhTree,
  Bvh4Tree, DynamicBvh}` (and the `-spatial` structures at v5), with the
  shared result types `RayHit{t, payload}` / `ClosestPointResult{point,
  distance², payload}` and the overlap-callback convention — one "give me
  hits" surface that forwards to the right backend. **Shapecast (sweep
  tests):** `cast_ray` / `cast_sphere` / `cast_box` vs a primitive *and*
  a BVH — closed-form TOI (sphere-cast vs AABB = ray-vs-AABB-grown-by-r;
  box-cast vs AABB = ray-vs-Minkowski-AABB; capsule-cast vs triangle; …),
  no GJK needed; the general convex-cast (GJK-cast) extends this in v2;
  eylem v6 CCD's *two moving convex shapes* case stays in eylem.
  **Broadphase pairs:** `find_overlapping_pairs(const DynamicBvh&, OutFn)`
  — the dual-descent self-overlap (all `(i<j)` fat-AABB-overlapping leaf
  pairs in one traversal, not n separate queries) — the all-pairs
  primitive eylem v1c's broadphase wraps. **Validation discipline:** a
  systematic **degenerate-geometry corpus** (zero-volume AABBs / collinear
  or zero-area triangles / coincident points / NaN-Inf inputs) and a
  **large-coordinate** sweep (geometry shifted to a +1e6 / +1e7 far origin
  — queries still correct within an f32-precision tolerance; flags where
  f64 staging is wanted) added across the `crd-geometry` test suite + as
  reusable test helpers.
* **v1j — `crd-geometry-viz` companion module (NEW, 12th sub-module).**
  Debug-only; depends `crd-geometry-*` + `crd-draw`. `crd-geometry`
  itself **never** depends on `crd-draw` — the lean-substrate /
  `crd-eylem` + `crd-eylem-viz` precedent (a headless/server build links
  the geometry substrate without the draw layer). Pure functions emitting
  `crd::draw::RenderBuffer` primitives: `draw_aabb` / `draw_obb` /
  `draw_sphere` / `draw_capsule` / `draw_frustum` / `draw_triangle` /
  `draw_ray` (primitive wireframes); `draw_ray_hit` (hit point + normal
  arrow + the `t`-segment); `draw_closest_point` (segment query→closest);
  `draw_normals` (normal hairs on a mesh view); `draw_bvh(BvhTree |
  Bvh4Tree | DynamicBvh, depth_limit)` (node AABBs colour-keyed by depth);
  `draw_frustum_cull(Frustum, BvhTree)` (kept vs culled in two colours);
  `draw_overlap_pairs(DynamicBvh)` (lines between overlapping leaf
  centroids). Extended incrementally — v4 adds mesh-query draws, v5 adds
  octree/grid-cell draws, v6 adds polygon-clip draws.

**v2 / v5 / v9e tweaks:** v2 (`-convex`) also lands `ConvexHullView`
queries (ray-vs-hull / closest-point-on-hull / contains-point-in-hull) +
the GJK-based convex shapecast (extends v1i's). v5 (`-spatial`) adds a
dense `UniformGrid` (3D cell array for small bounded domains — distinct
from the hashed grid) and extends the v1i query facade over the spatial
structures; sweep-and-prune stays Reserved. v9e (`-shader-helpers`) emits
GLSL/HLSL twins of v1h's `signed_distance.hpp` analytic SDFs (alongside
v0e's formulary), ULP-conformance-tested against those C++ references.

## 16. Pinned architecture principles (from the §15 amendment)

1. **Query API is compile-time overload-polymorphic, not virtual.**
   `raycast(const BvhTree&, …)`, `raycast(const Bvh4Tree&, …)`,
   `raycast(const DynamicBvh&, …)`, `raycast(const Sphere<T>&, …)` are
   free functions resolved at compile time — no `IAcceleration` vtable.
   Zero-overhead, matches the §5 "Eigen-class typed layer". A
   runtime-polymorphic "pick whichever structure" (e.g. for the editor)
   is a thin `std::variant<…>` wrapper if ever needed, not a vtable.
2. **`RayHit` carries a templated payload, not a fat union.** `RayHit{t,
   payload}` where `payload` is `u32 prim_index` for `BvhTree`, `{u32
   tri_index; f32 u, v;}` for a mesh raycast (`-mesh` v4), an entity id
   for a scene raycast. Distinct concrete result types per backend rather
   than one over-generic struct — clarity over genericity.
3. **NaN/Inf: queries tolerate, builders reject (in debug).** A query
   against a possibly-garbage scene must never crash — `intersects` etc.
   are NaN-safe by IEEE comparison semantics (a NaN/∞ primitive is
   silently never-hit / never-closest), and that is the contract. A
   *builder* fed garbage must scream in debug (`CRD_ASSERT` finite
   inputs) — it is a programmer error — and produce a defined-but-useless
   structure in release, never UB. `is_finite()` helpers let cookers /
   importers validate upstream.
4. **Epsilon policy lives in one place, named by intent.**
   `k_parallel_epsilon` (directions parallel?) ≠ `k_distance_epsilon`
   (points the same?) ≠ `k_sah_cost_epsilon` (SAH split-cost tie) —
   independently tunable, all in `crd/geometry/primitives/constants.hpp`
   (the leaf substrate); every other sub-module consumes them.
5. **Viz is a companion module, never woven in.** `crd-geometry-viz`
   functions are pure (`RenderBuffer&` + a const-ref to the geometry
   structure, no state) and live in their own module — `crd-geometry`
   stays clean for headless builds.

**What is not affected.** §1–§5 architecture (modulo the new `-viz`
sub-module and the `queries.hpp` / `constants.hpp` / `is_finite.hpp` /
`signed_distance.hpp` / `ConvexHullView` additions to `-primitives`),
§6 sequencing, §8 out-of-scope (the GPU / decomposition lines unchanged),
§9 risks, the §12 sequence pivot, the §13/§14 amendments. Slice count
30 → 33; engine LOC ~16.6 → ~18.5 KLOC; calendar ~5–7 → ~6–8 months.
