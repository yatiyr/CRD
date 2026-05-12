# Phase 3.1.7 — `crd-geometry`: computational-geometry substrate

**Status:** ⏳ **next-active** (research dossier closed 2026-05-11
+ addendum §13 added 2026-05-11; ADR-0076 Accepted + Amended
2026-05-11 §12 + §13; slice list locked at **30 slices**;
**first-light kickoff IMMEDIATELY after Phase 3.1 v1b cluster close
(sweep-PASS gates the pivot)** — not after Phase 3.1.6 `crd-hesap`
as originally locked. Sequence pivot per ADR-0076 §12: full 30-slice
phase executes BEFORE Phase 3.1 v1c so eylem v1c+v1d+v1d-mesh and
sdf v2 consume geometry from day 1, dissolving the deferred-refactor
debt the original sequence required. ADR-0076 §13 (2026-05-11): the
pre-existing `crd::math::geometry` (404 LOC — `Ray`/`Plane`/`Sphere`/
`AABB`/`Triangle`/`Frustum` + ~16 closest-point/intersect helpers,
~9 consumers) is **moved-and-deleted** into `crd-geometry-primitives`
v0a — `crd-math` keeps only Vec/Mat/Quat/Transform/SIMD/deterministic;
and a new **v0f cutting-edge / branchless / SIMD intersection corpus**
sub-slice lands after v0e — watertight ray-tri (Woop 2013),
Baldwin-Weber 2016, branchless NaN-safe slab ray-AABB, Williams/Ize
robust-traversal ray precompute, Plücker edge classification, Ericson
Voronoi-region closest-point-on-triangle, + `Vec4f`/`Vec8f` batch
kernels + ULP-conformance tests.)

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
| **v0a** ✅ 2026-05-12 | `crd-geometry-primitives` module skeleton + primitive types (`Point3` alias, `Line`/`Segment`/`Ray`, `Plane`, `AABB`, `OBB`, `Sphere`, `Capsule`, `Triangle3`, `Frustum`). **ADR-0076 §13 move-and-delete:** absorbed `crd::math::geometry` (`Ray`/`Plane`/`Sphere`/`AABB`/`Triangle`→`Triangle3`/`Frustum` + ~16 helpers + their `std::formatter`s) → `crd::geometry::primitives::*` in `crd/geometry/primitives/{primitives,format}.hpp`; `crd/math/geometry.hpp` **deleted**; 9 consumers repointed (`crd-scene` `query.hpp`/`world.hpp` — new PUBLIC edge `crd-scene → crd-geometry-primitives`; `crd-math` umbrella + `format.hpp`; `tests/math` geometry tests moved to new `tests/geometry-primitives/test_primitives.cpp`; `tests/bench`; `tests/scene`; `runtime/examples/smoke_math.cpp`). `crd-math` now ships only Vec/Mat/Quat/Transform/SIMD/`deterministic`; `/wd4723` MSVC suppression moved to `crd-geometry-primitives`; `crd-no-std-math-check` CI guard now scopes `engine/geometry-primitives`. Built + run green on win-debug/win-asan/win-clang-cl/linux-gcc-debug. System doc: `docs/systems/geometry-primitives.md`. | ~700 + refactor | done |
| **v0b** ✅ 2026-05-13 | **2D peer set + closest-point catalogue (2D + 3D) + segment↔segment** (ADR-0076 §14 amendment). Added 2D types `Line2`/`Segment2`/`Ray2`/`AABB2`/`OBB2`/`Circle`/`Capsule2`/`Triangle2`/`Point2` + their `operator==`/helpers/`std::formatter`s; **renamed** the v0a 3D types to their `…3` forms (`Line→Line3` … `Capsule→Capsule3`; `Plane`/`Sphere`/`Frustum`/`Triangle3` unchanged) per the new dimension-suffix naming rule; ~8 consumer files repointed (`crd-scene` query/world incl. the ADR-0053 `in_aabb(AABB3<f32>)` shape, `tests/geometry-primitives`, `tests/bench`, `tests/scene`, `runtime/examples/smoke_math.cpp`, `crd-math` umbrella+format comments). New `crd/geometry/primitives/closest_point.hpp`: `closest_point` + `closest_param(Segment)` + `distance` + `distance_squared` for every primitive (2D & 3D), and `closest_points(seg1,seg2,out_c1,out_c2)` + `distance(seg1,seg2)` — the mutually-closest pair. Dimension-agnostic cores written once over a vector concept: **Ericson §5.1.5** Voronoi-region closest-point-on-triangle (7-region branch-light cascade, with a collinear-triangle 3-edge fallback), **Ericson §5.1.9** segment↔segment (robust on parallel/degenerate), the linear-projection param. Sphere/Circle/Capsule return the closest *surface/boundary* point (defined for interior `p`); degenerate query (on center/spine) → fixed `+x` deterministic tiebreak; sub-mm scales preserved (`numeric_limits<T>::min()` degenerate guards). New `tests/geometry-primitives/test_closest_point.cpp` (TEMPLATE_TEST_CASE float+double, ~14 cases / property tests vs dense barycentric sample). clang-format/tidy clean; `crd-no-std-math-check` guard green. Built + run green on **win-debug + win-shipping** (geometry-primitives-tests 20 cases / 39 726 assertions; crd-scene-tests 271; crd-math-tests 140; smoke_math) — full 14-config sweep deferred to v0 close. System doc updated. | ~1.6 KLOC + ~450 LOC tests + rename | done |
| **v0c** ✅ 2026-05-13 | **intersection corpus (2D + 3D)** — new `crd/geometry/primitives/intersect.hpp`. Ray casts: `intersect_ray_{aabb,obb,cylinder,capsule}` (3D, Tavianator branch-light slab for AABB/OBB; the robust precomputed-`RayPacket` is v0f) + `intersect_ray2_{aabb,obb,line,segment,circle,capsule,cylinder}` (2D). Boolean overlap: `intersects(AABB3,Triangle3)` Akenine-Möller 2001 13-axis SAT, `intersects(OBB3,OBB3)` 15-axis SAT, `intersects(OBB2,OBB2)` 4-axis, `intersects(Triangle2,Triangle2)` 6-axis SAT, `intersects(Triangle3,Triangle3)` Möller 1997 (coplanar case → 2D test), `segments_intersect(Segment2,Segment2)` (+ out-point), sphere/circle/capsule↔{AABB,OBB,Triangle,Segment,Plane,Capsule} reductions (exact, via v0b `distance_squared`), `intersects(Plane,{AABB3,OBB3,Sphere})`, `intersects(Frustum,OBB3)`, `intersects(Triangle2,{AABB2,Segment2,Circle})`, `intersects(Line2,{AABB2,OBB2})`. `intersects(Capsule3,{AABB3,OBB3})` conservative (segment-vs-box-grown-by-r — no false negatives; exact SAT-box-pair deferred to eylem v2). Determinism: SAT axes in a fixed enumeration order; near-zero cross-product axes skipped (never a false "separated"). **New types `Cylinder3`/`Cylinder2`** (flat-cap segment+radius — distinct from `Capsule`'s hemispherical caps; the v0c plan's user-confirmed addition) + `operator==`/`std::formatter`/`contains`. New `tests/geometry-primitives/test_intersect.cpp` (TEMPLATE_TEST_CASE float+double; ray hit/miss/grazing/inside-start, SAT per-axis-class + brute-force cross-checks, tri-tri piercing/coplanar/disjoint, sphere/capsule reductions vs `distance_squared`, 2D segment crossing/parallel/collinear/T-junction). Caught + fixed a real bug in the first AABB-tri SAT (the `(0,0,1)×e` block's f0/f2 vertex-pair selection was swapped) — rewrote the 9 edge-cross axes as a clean lambda over all 3 vertex projections. clang-format/tidy clean; `crd-no-std-math-check` green. Built + run green on **win-debug + win-shipping** (geometry-primitives-tests 38 cases / 40 988 assertions) — full 14-config sweep deferred to v0 close. System doc updated. | ~1.6 KLOC + ~600 LOC tests | done |
| **v0d** ✅ 2026-05-13 | **`Tetrahedron` type + barycentric / tetrahedron utilities** (Ericson §3.4). New type `Tetrahedron<T>` in `primitives.hpp` (3D-only → no suffix per the naming rule, like `Plane`/`Frustum`) + `operator==`/`std::formatter`/`f`-`d` aliases + `centroid`/`signed_volume`(= `(1/6)·det[b−a|c−a|d−a]`)/`volume`. New `crd/geometry/primitives/barycentric.hpp`: `barycentric(Tetrahedron,p)→Vec4` (the 4 weights as signed-volume ratios with `p` substituted for each vertex — orientation-stable; `CRD_ASSERT` on a flat tetra), `contains(Tetrahedron,p)` (all weights ≥ −ε), `from_barycentric` (the inverse — `Triangle3`/`Triangle2`/`Tetrahedron`), `decompose_prism_to_tets(Triangle3 bottom, Triangle3 top)→StaticArray<Tetrahedron,3>` (the canonical 3-tet split of a triangular wedge through `a₀` — fixed diagonal so neighbouring wedges share matching tets; the Marching-Tetrahedra / FEM hex-to-tet form). The triangle barycentric/contains forms already live in `primitives.hpp` (v0a–v0b) — unchanged. New `tests/geometry-primitives/test_barycentric.cpp` (TEMPLATE_TEST_CASE float+double; tetra volume/centroid/orientation, vertices→basis & centroid→(¼,¼,¼,¼), `contains` inside/face/edge/vertex/outside, orientation-independence on a negatively-oriented tetra, `barycentric∘from_barycentric` round-trip on random interior points, prism-split non-degeneracy + volume-sum + coverage). clang-format/tidy clean; `crd-no-std-math-check` green. Built + run green on **win-debug + win-shipping** (geometry-primitives-tests 46 cases / 42 182 assertions) — full 14-config sweep deferred to v0 close. System doc updated. | ~250 LOC + ~250 LOC tests | done |
| **v0e** ✅ 2026-05-13 | **iq formulary + `reduce_argmax_with_lex_tiebreak` + the shader-helpers module skeleton.** New `crd/geometry/primitives/formulary.hpp`: smooth-min/max — `smin_poly`/`smin_cubic` (iq quadratic & cubic; exact `min` outside the ±k blend band, dip = k at the crossover, collapse to `min` as k→0) + `smin_exp` (associative; via `crd::math::deterministic::exp2/log2`) + `smax_*` counterparts; value-domain `op_round`/`op_onion`/`extrude_2d`; position-domain `domain_repeat`/`domain_mirror` (2D/3D/scalar, exact `std::floor`-modulo, no trig) + `domain_elongate` + `domain_twist`/`domain_bend` (via `crd::math::deterministic::sin/cos`). New `crd::math::simd::reduce_argmax_with_lex_tiebreak` (`engine/math/include/crd/math/simd/reduce.hpp`, also wired into `simd.hpp`): scalar-deterministic horizontal argmax over a `Vec8f`/`Vec4f` chunk — `ArgmaxLex{index,score,x,y,z}` running-best + `argmax_lex_beats` (score-desc → (x,y,z)-asc-lex → earliest-index; NaN-scores never win; partition-independent), the ADR-0076 §4 #10 determinism pin v3 Quickhull needs. New module `engine/geometry-shader-helpers/` (target `crd-geometry-shader-helpers`, namespace `crd::geometry::shader_helpers`) — **skeleton only** (force-link stub + reserved header; the formula-IR cooker + GLSL/HLSL backends + ULP-conformance test land in v9e); added to root `CMakeLists.txt` and to the `crd-no-std-math-check` guard's scoped dirs. New tests `tests/math/test_reduce.cpp` (comparator: score/lex/index/NaN/invalid; chunk folds; partial lanes; partition-independence vs a scalar reference) and `tests/geometry-primitives/test_formulary.cpp` (smin exact-outside-band / crossover-dip = k / k→0-collapse / smax-duality / ≤min; `op_*`; `extrude_2d` cylinder hand-cases; `domain_repeat`/`mirror` periodicity + cell bounds; `domain_elongate` zero-inside-±h; `domain_twist`/`bend` k=0-identity + rigidity). clang-format/tidy clean (my files); `crd-no-std-math-check` green. Built + run green on **win-debug + win-shipping** (geometry-primitives-tests 56 cases / 49 224 assertions; crd-math-tests 143 / 2 956) — full 14-config sweep deferred to v0 close. System doc updated. | ~1.0 KLOC + ~450 LOC tests | done |
| **v0f** ✅ 2026-05-13 | **cutting-edge / branchless / SIMD intersection corpus** (ADR-0076 §13; research dossier §13) — **closes the v0 sub-phase**. New scalar headers: `watertight_ray_tri.hpp` — `precompute_ray_tri(Ray3)→RayTriShear` + `intersect_ray_triangle_watertight` (Woop/Benthin/Wald 2013: per-ray shear+scale to the dominant-axis frame, edge-function form, sign consistent across shared edges → no cracks; on an exact-zero edge function it recomputes that test in `double` when `T==float`; the default ray-tri for `-mesh` v4d leaves + `crd-sdf` v2 mesh-bake) + `TriAffine` / `precompute_triangle_affine` / `intersect_ray_triangle_precomputed` (Baldwin-Weber 2016: per-triangle 3x4 affine = inverse of `[e1|e2|n]`, ~9 mul/ray, branchless — opt-in for cooked static meshes); `robust_ray_aabb.hpp` — `RayAABBPrecompute{inv_dir, sign[3]}` + `precompute_ray_aabb(Ray3)` + `intersect_ray_aabb_robust` (Williams 2005 precomputed slab + Ize 2013 conservative `tmax` widening `x(1+2γ₃)`; NaN-safe min/max so a zero-direction axis drops out rather than poisoning); `plucker.hpp` — `PluckerLine{d,m}` + `plucker_from(Segment3|Ray3|Line3|p,q)` + `plucker_side(a,b)=dot(a.d,b.m)+dot(b.d,a.m)` (fixed sum order; sign-zero = on-the-line) + `intersect_ray_triangle_plucker` (sign-only edge classification, fully branchless). **Single-ray vs multi-ray** is an explicit split: the scalar precompute structs are *per-ray* (leaf-batch — one ray vs the N AoSoA primitive columns of a BVH leaf); the `Vec8f` `RayPacket8` is *per-packet* (Wald-style — 8 coherent rays, the box scalar/broadcast). New SIMD module piece: `simd_batch.hpp` + out-of-line `src/simd_batch.cpp` (so `crd-simd-emission-check` has a SIMD `.obj`) — `Vec8f` AoSoA bundles (`Aabb8`/`Sphere8`/`Triangle38`/`Segment38Pair`) + kernels `ray_vs_8_aabb` (one ray, 8 child boxes — Williams slab x8 + Ize pad), `ray_packet8_vs_aabb` (8 rays, 1 box), `ray_vs_8_triangle` (Möller-Trumbore x8, `cull_back` flag), `aabb8_vs_aabb`, `sphere8_vs_sphere`, `segment8_vs_segment_distsq` (Ericson §5.1.9 in SIMD via masked selects — robust on parallel/degenerate); all comparisons return all-bits-set masks, `1/x` is `_mm256_div_ps` (ADR-0076 §4 #8-#11). New tests `tests/geometry-primitives/test_v0f_corpus.cpp` (Woop basic+back-cull, shared-edge no-crack property, Woop vs v0c-MT agreement on a random non-degenerate corpus, Baldwin-Weber vs Woop, robust ray-AABB vs v0c slab + conservatism, Plücker side-values + ray-tri boolean vs Woop), `tests/geometry-primitives/test_v0f_simd.cpp` (each kernel lane-by-lane vs its scalar reference; mask convention hit!=0/miss==0). Also fixed two cross-config issues found by the v0-close sweep: (a) GCC `-Werror=shadow` — `intersects(Triangle3,Triangle3)`'s `interval` lambda params `d0/d1/d2` shadowed outer `d1`/`d2`; renamed to `e0/e1/e2`; (b) **non-ASCII characters in `TEST_CASE` names** (em-dash etc.) broke Windows `ctest` name-matching → all the new v0b–v0f test cases reported `(Failed)` — replaced with ASCII in every new test file; `scripts/check_no_non_ascii_test_names` guard green. clang-format/tidy clean; `crd-no-std-math-check` + `crd-simd-emission-check` green. **Full 14-config `scripts/full-sweep.ps1` PASS** (geometry-primitives-tests 74 cases; crd-math-tests with `test_reduce`). System doc updated. | ~1.4 KLOC + ~700 LOC tests | done |
| — | **v0 sub-phase ✅ CLOSED 2026-05-13** — `crd-geometry-primitives` ships the full primitive type set (2D + 3D + `Tetrahedron`), closest-point catalogue, intersection corpus (scalar + branchless cutting-edge + `Vec8f` SIMD), iq formulary, and `reduce_argmax_with_lex_tiebreak`; `crd-geometry-shader-helpers` skeleton in place. Next = **v1a** (`-bvh`: binned-SAH BVH). | | |
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

**Total: ~30 slices, ~16.6 KLOC engine + ~5 KLOC editor-tier + ~4 KLOC
cooker-emitted GLSL/HLSL, ~5–7 months calendar** (was 25 / 14 + 5 /
4–6 months prior to supplement-dossier additions; +v0e/v1g/v4g/v9e
from the supplement; +v0f from the ADR-0076 §13 amendment; the §13
move-and-delete also nets ~−0.4 KLOC out of `crd-math` into v0a).

## Performance budgets (per supplement dossier §4.1)

Targets per sub-module on a Zen 4 reference CPU (Win-shipping config,
full LTO + AVX2). Per-slice DoD measures against these:

| Sub-module / op | Target | Reference |
|---|---|---|
| Ray-vs-AABB — branchless NaN-safe slab (scalar) | ~2 ns/test | Tavianator / Williams 2005; no hot-path branches |
| Ray-vs-4-AABB — `Vec4f` batch slab | ~3–4 ns/4-test | one `Vec4f` min/max chain |
| Ray-vs-triangle — Möller-Trumbore (scalar) | baseline (~10 ns) | Möller-Trumbore 1997 |
| Ray-vs-triangle — watertight Woop 2013 (scalar) | ≤ 1.3× Möller-Trumbore | shear+scale overhead bought back by shared-edge consistency |
| Ray-vs-triangle — Baldwin-Weber precomputed | ≤ 0.8× Möller-Trumbore on read-mostly meshes | 9 mul + 6 add/ray; 48 B/tri storage |
| Ray-vs-8-triangle — `Vec8f` Woop / M-T batch | <10 ns/8-tri batch on AVX2 | matches the v4g per-leaf SIMD budget shape |
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
- **v0f cutting-edge intersection corpus** (research dossier §13):
  - Woop, Benthin, Wald (2013) — *Watertight Ray/Triangle Intersection*.
    JCGT 2(1). (Embree's default ray-tri; consistent sign across shared
    edges — the v4d `-mesh` leaf default + `crd-sdf` v2 mesh-bake.)
  - Baldwin, Weber (2016) — *Fast Ray-Triangle Intersections by
    Coordinate Transformation*. JCGT 5(3). (Precomputed unit-triangle
    transform; the cooked-static-mesh default.)
  - Williams, Barrus, Morley, Shirley (2005) — *An Efficient and Robust
    Ray-Box Intersection Algorithm*. JGT 10(1). (Precomputed
    `inv_dir` + sign-mask slab test.)
  - Ize (2013) — *Robust BVH Ray Traversal*. JCGT 2(2). (Boundary-
    consistent traversal — partner to Woop watertight tri; the v1g
    `RayPacket` precompute.)
  - "Tavianator" branchless slab form — Tavian Barnes (2011, 2015, 2022
    revisions), *Fast, Branchless Ray/Bounding Box Intersections* — the
    de-facto NaN-safe `tmin/tmax` formulation; mathematically equivalent
    to Williams 2005 with IEEE min/max ordering.
  - Plücker-coordinate edge tests — see Erickson (1997) *Plücker
    Coordinates Tutorial* + Ericson *RTCD* §5.3 / Eberly GTE
    line-triangle classification (sign-only, branchless edge functions).
  - Ericson *Real-Time Collision Detection* §5.1.5 — the Voronoi-region
    `closest_point(Triangle, p)` (v0b).

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
