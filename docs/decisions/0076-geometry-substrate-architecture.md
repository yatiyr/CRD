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

1. **`crd-geometry-primitives`** (v0, ~3 KLOC) — point/line/segment/ray/
   plane/triangle/sphere/AABB/OBB/capsule distance + intersection +
   closest-point + frustum tests + barycentric utilities.
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
| **v0a–v0d** | primitives substrate (types + closest-point + intersection + barycentric) | ~3000 | ~1 wk |
| **v0e** | iq-formulary primitives substrate — polynomial + exponential smin operators + domain-repeat / domain-mirror / domain-warp ops + the shader-helpers cooker generator skeleton (no GPU side yet); ALSO lands the `crd::math::simd::reduce_argmax_with_lex_tiebreak` substrate primitive that v3 Quickhull SIMD reduction needs | ~1000 | ~3 days |
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

**Total: ~29 slices, ~15.8 KLOC engine + ~5 KLOC editor-tier + ~4 KLOC
cooker-emitted GLSL/HLSL, ~5–7 months calendar** (was 25 / 14 + 5 /
4–6 months prior to supplement-dossier additions; supplement adds v0e,
v1g, v4g, v9e plus the 11th sub-module `crd-geometry-shader-helpers`
which is mostly cooker-emitted).

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
