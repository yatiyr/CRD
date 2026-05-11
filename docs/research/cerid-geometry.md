# Cerid — `crd-geometry` substrate research

**Date:** 2026-05-11
**Locks:** ADR-00xx (`crd-geometry` substrate architecture — to be drafted from this document).
**Phase plan:** `docs/phases/phase-3.1.7-geometry.md` (to be drafted; recommendation in §10 below).
**Companion research:** `docs/research/cerid-eylem.md` (broadphase + narrow-phase architecture); `docs/research/cerid-sdf.md` (peer substrate, implicit-surface side); `docs/research/cerid-hesap.md` (peer substrate, numerical side).

> Source-of-truth document for the *why* behind every algorithm /
> data-structure / consumer choice in `crd-geometry`. The future ADR
> cites this file. The future phase plan implements against it.
>
> Companion piece to `cerid-sdf.md` and `cerid-hesap.md`. Reading
> all three back-to-back is the right way to understand the
> "substrate-tier" peer modules of Phase 3.1.x.

---

## 1. Executive summary

**`crd-geometry`** is a standalone Cerid substrate module providing
**computational-geometry primitives over explicit geometry**: vertex,
index, half-edge, polygon, BVH-and-friends spatial accelerators,
convex-shape distance/intersection, triangle-mesh queries, polygon
Booleans, mesh processing, and Delaunay/Voronoi structures. It is a
**peer of `crd-math`, `crd-jobs`, `crd-eylem`, `crd-sdf`, `crd-hesap`**
— the same posture the four other Phase 3.1.x substrate decisions
locked in (ADR-0062 / ADR-0063 / ADR-0064 / ADR-0065).

### 1.1 The two-line definition

- **`crd-sdf` is the implicit-surface substrate** — fields, voxel
  grids, sampling, gradient, marching cubes. It answers "what is the
  signed distance from this point to the surface?" without needing the
  surface to exist as triangles.
- **`crd-geometry` is the explicit-surface + spatial-reasoning
  substrate** — vertices, indices, half-edges, BVH, GJK, mesh
  closest-point, convex hull, polygon Boolean. It answers everything
  else: "given an explicit triangle mesh, where does this ray hit?",
  "what is the BVH over these AABBs?", "what is the convex hull of
  these points?", "do these two convex shapes overlap?".

The boundary is **representation-shaped, not problem-shaped**. The same
question — *closest point on a mesh* — is answered by `crd-geometry`
on a `(vertices, indices)` pair and by `crd-sdf` on a baked grid. The
two substrates intentionally overlap in *what they answer* and are
disjoint in *the data they consume*.

### 1.2 Why a separate module (one paragraph; full case in §2)

Bloating these algorithms into `crd-math` would (a) double its compile
time and double the dependency-graph fan-out for every module that
already pulls `crd-math` (which is every module), (b) conflate two
totally different testing tiers — `crd-math` ships exact-bit-pattern
unit tests on `Vec3f::dot`, while `crd-geometry` ships convergence
benchmarks and degenerate-input torture tests, and (c) violate
CLAUDE.md §7's module-isolation principle — a DAW build that needs
none of GJK / convex hull / mesh closest-point would still be paying
the binary-size and build-time tax. Per-consumer duplication is worse:
eylem, sdf, scene's `SpatialBVHIndex`, the renderer's frustum culler,
and the audio path-tracer all need a BVH, and writing five of them is
how engines drift apart.

### 1.3 Multi-domain mandate (per CLAUDE.md §1)

| Domain | What it pulls from `crd-geometry` |
| --- | --- |
| Games — physics (eylem) | BVH (broadphase), GJK + EPA (narrow), triangle-mesh closest-point + raycast (mesh collider), convex hull (collider conditioning) |
| Games — rendering | BVH for frustum culling and occlusion, triangle-mesh raycast for picking, polygon clip for stencil rendering |
| Robotics — motion planning | BVH for collision-free path checks, GJK for swept distance, half-edge mesh for navigation graph, convex decomposition for collision proxies |
| Robotics — sensor sim | Raycast on triangle mesh (LiDAR), point-on-mesh queries (touch sensors) |
| Medical — visualization | Mesh simplification (LOD on giant CT meshes), mesh repair (DICOM-derived geometry is rarely manifold), Delaunay for unstructured mesh reconstruction |
| Cinematic — VFX / pipeline | Polygon Boolean (CSG modelling at cook), Voronoi (cell shattering), Quadric Edge Collapse (LOD chains) |
| DAW / audio | BVH for path-traced acoustic occlusion (Steam Audio model), raycast for early reflections |
| Authoring / editor | Convex decomposition (V-HACD pipeline for collider authoring), polygon clip (selection ops), mesh repair tools |
| Cooker | Triangle-mesh winding-number queries (driving the SDF baker in `crd-sdf`), AABB tree for point-in-mesh, half-edge for cleanup |
| `crd-sdf` itself | BVH for the closest-point search inside `bake_mesh_to_grid`; winding-number test for the sign |

Every shipped or planned Cerid module that touches "spatial reasoning
over explicit geometry" lands here. No exceptions.

### 1.4 Relationship to `crd-sdf`

The dependency goes **`crd-sdf` → `crd-geometry`**. Specifically: the
mesh-bake step in `crd-sdf` v2 (Jacobson 2013 generalised winding
number) needs (a) a BVH over the input triangle mesh, (b) a robust
closest-point-on-triangle query, and (c) a numerically-stable
solid-angle accumulator. All three live in `crd-geometry`. Today
ADR-0064 §4 says the SDF mesh baker "reuses the dynamic AABB tree from
`crd-eylem`'s broadphase" — that line becomes "uses the BVH from
`crd-geometry`" once this substrate ships, and the refactor reservation
is documented in §9 of this dossier.

The reverse arrow does not exist: `crd-geometry` never depends on
`crd-sdf`. Geometry is the lower-level substrate; sdf builds on top of
it. This is the same posture as `crd-hesap` → `crd-math` (hesap depends
on math; math knows nothing of hesap).

---

## 2. Why a separate module — the case for `crd-geometry`

### 2.1 The cross-cutting consumer demand is concrete

This is not a speculative module. Every consumer below has either
shipped (and currently has a placeholder), is on the active phase plan,
or is on the ROADMAP with a near-term ETA.

| Consumer | Slice | Concrete API call(s) needed |
| --- | --- | --- |
| `crd-eylem` v1c (broadphase) | active | `BvhTree<AABB>::build(span<AABB>)`, `BvhTree::refit(span<AABB>)`, `BvhTree::query(AABB) → span<u32>`, `BvhTree::raycast(Ray) → optional<Hit>` |
| `crd-eylem` v1d (narrow phase, convex–convex) | active | `gjk_distance(SupportFn, SupportFn) → f32`, `gjk_intersects(SupportFn, SupportFn) → bool`, `epa_penetration(SupportFn, SupportFn, GjkSimplex) → ContactPoint` |
| `crd-eylem` v1d-mesh (TriangleMesh collider) | next | `mesh_closest_point(TriangleMeshView, Vec3) → ClosestPoint`, `mesh_raycast(TriangleMeshView, Ray) → Hit` |
| `crd-eylem` v1c (collider conditioning) | active | `convex_hull_3d(span<Vec3>) → ConvexHull` (used at cook to clean designer-authored convex colliders) |
| `crd-sdf` v2 (mesh-bake) | post-3.1 | `mesh_winding_number(TriangleMeshView, Vec3) → f32` (Jacobson 2013), `BvhTree::closest_triangle(Vec3) → (TriangleId, BarycentricCoord)` |
| `crd-renderer` v1 (frustum cull) | shipped (placeholder) | `frustum_cull(BvhTree, FrustumPlanes) → span<RenderableId>` |
| `crd-renderer` v3.5+ (occlusion BVH, decal projection) | future | BVH ray queries; polygon clip on screen-space rectangles |
| `crd-scene` `SpatialBVHIndex` (ADR-0053 reserved shell) | reserved shell exists | the BVH itself — the shell is reserved precisely because we knew this substrate was coming |
| `crd-audio` Phase 3.4 (acoustic ray-cast) | future | `BvhTree::raycast`, `mesh_raycast` for path-traced reflections |
| `crd-eylem-aero` (ADR-0073 reserved) | future | Triangle-mesh integration (surface area, surface normals) for aerodynamic forces |
| `crd-eylem-cine` (ADR-0074 reserved) | future | Animated mesh queries (rebuild BVH per-frame; or refit when topology stable) |
| Editor (Phase 7) | future | `convex_decomposition_vhacd(TriangleMeshView) → span<ConvexHull>`, polygon Boolean for selection ops, mesh repair |
| Cooker (always-on tooling) | active | Mesh repair (manifoldness fix), Delaunay (CDT for footprint generation), winding-number (driver of SDF baker) |

That is **at least nine independent consumers** of the BVH alone, and
**five independent consumers** of the triangle-mesh closest-point /
raycast pair. The marginal cost of a substrate module is repaid the
first time the second consumer calls into it.

### 2.2 Why bloating `crd-math` is the wrong move

`crd-math` is currently ~2 KLOC of `Vec`/`Mat`/`Quat`/`Transform` +
SIMD wrappers + the deterministic-stdlib substitutions from Phase 3.1
v0c. Its design intent is **"the small fixed-size linear-algebra layer
every other module pulls"**. Concretely:

- **Compile-time tax.** Every module in the dependency graph from
  CLAUDE.md §"Module Dependency Graph" pulls `crd-math`. Adding
  ~12 KLOC of BVH / GJK / mesh-processing templates to it would compound
  template instantiation across every translation unit in the engine.
  That's the same trap Eigen and CGAL fell into — header-only growth
  causes minute-scale build times in dependent projects.
- **Tooling-tier mismatch.** `crd-math` correctness tests are *unit
  tests* — exact bit pattern checks on dot products, matrix-vector
  multiplications, quaternion-to-matrix round-trips. `crd-geometry`
  tests are *property tests + degenerate-input torture tests + accuracy
  benchmarks* — random convex polytopes vs themselves under arbitrary
  rigid transforms must produce GJK distance ≤ epsilon; random meshes
  with intentional self-intersection must still produce a stable BVH;
  Boolean operations on near-degenerate polygons must not lose
  topology. These two tiers want different CTest labels, different
  reference-data fixtures, different CI budgets.
- **Scope conflation.** `crd-math` is the answer to "a single
  3-vector cross product" or "a 4×4 matrix inverse". `crd-geometry`
  is the answer to "5K vertices and 10K triangles, build me a query
  structure, then run a million ray queries against it." These are
  different libraries doing different jobs at different cost orders.
  Bundling them implies an architectural lie.

### 2.3 Why per-consumer duplication is worse

If we don't ship a substrate, we will end up with at least three BVH
implementations within six months:

1. `crd-eylem` ships its own quad-tree BVH (per ADR-0062 §3, the
   "Catto GDC 2019 dynamic AABB tree" pattern).
2. `crd-sdf` ships its own BVH inside the mesh baker, because the cook
   step needs a closest-triangle search and adopting the eylem one would
   require pulling `crd-eylem` into `crd-sdf`'s dependency graph (a
   cycle the moment eylem grows an SDF-collider type).
3. `crd-scene::SpatialBVHIndex` (ADR-0053 reserved shell) ships its own,
   because it lives in `crd-scene` whose dep graph cannot reach into
   `crd-eylem`.

Three implementations, three sets of tests, three drift opportunities.
The renderer's frustum culler makes four. The audio raycaster makes
five. This is exactly the failure mode the **"tak-çıkar third-party"**
principle (PRINCIPLES.md) addresses for *external* libraries; the same
discipline applies internally.

### 2.4 The peer-module pattern is established

The Phase 3.1.x substrate sequence is the model:

- Phase 3.1 / ADR-0062 / `cerid-eylem.md` — `crd-eylem` (physics)
- Phase 3.1.5 / ADR-0064 / `cerid-sdf.md` — `crd-sdf` (implicit surfaces)
- Phase 3.1.6 / ADR-0065 / `cerid-hesap.md` — `crd-hesap` (numerical computing)
- **Phase 3.1.7 (proposed) / future ADR / this document** — `crd-geometry` (computational geometry over explicit surfaces)

Each is a substrate, named with a domain-meaningful word (Turkish: *eylem* =
action, *hesap* = computation; English-language ones — *sdf*, *geometry* —
sit alongside). Each (a) compiles standalone, (b) inherits ADR-0063
determinism, (c) integrates `crd-jobs` for parallelism, (d) exposes
data-oriented APIs, (e) reserves a GPU sub-module, (f) ships a cooker
hook. `crd-geometry` follows the same template.

---

## 3. Industry survey — what shipped engines / libraries have done

This section is the bulk of the dossier. Each algorithm class in §4
references a subset of these libraries; here they are surveyed
end-to-end so the reader can navigate "which decision is informed by
which prior art".

### 3.1 Game / physics engines — broadphase + narrow phase

#### Bullet (Erwin Coumans et al., 1997+)

The reference open-source 3D physics engine, used by Blender, the
DARPA Robotics Challenge, ROS / Gazebo, and historically much of the
indie Unity ecosystem before Jolt.

- **Broadphase: `btDbvt`** — dynamic bounding-volume hierarchy of AABBs
  (binary tree, balanced via subtree-rotations during insert). ~3 KLOC.
  Supports incremental updates: bodies' AABBs grow + shrink, the tree
  refits without rebuild.
- **Broadphase alt: `btAxisSweep3`** — sweep-and-prune over three sorted
  axes. Originally the default; superseded by `btDbvt` for clustered
  scenes. Sweep-and-prune still wins for *uniform-spread* scenes (e.g.
  particle systems).
- **Narrow phase: GJK + EPA** — `btGjkPairDetector`, `btGjkEpaSolver2`.
  Standard. Margin-extended convex shapes for stability.
- **Mesh collider: `btBvhTriangleMeshShape`** — pre-built quantised
  16-bit-per-axis BVH over a static triangle mesh. Per-pair queries
  walk the tree and return triangle indices for narrow phase.
- **Continuous detection: GJK-cast** (Cameron 1997 for distance, Mirtich
  for time-of-impact).

`btCollisionWorld` is the umbrella class. Total geometry-related code in
Bullet is ~25 KLOC.

#### PhysX (NVIDIA, 2001+)

The dominant commercial / proprietary physics engine.

- **Broadphase: `PxBroadPhase`** with two backends — sweep-and-prune
  (`PxBPType::eSAP`, default for moderate object counts) and a parallel
  multi-box-pruning (`PxBPType::eMBP`, default for very large worlds).
- **`PxBVH`** — Pre-built static BVH for query-only use cases (raycasts
  against static geometry). Public API; consumed by the scene query
  module.
- **Narrow phase: GJK** in `PxgGjk` (the GPU narrow phase) and
  hand-rolled SAT for box-box / capsule-box where the closed-form is
  cheaper.
- **Mesh collider: `PxTriangleMesh`** — 18-byte-per-node quantised BVH,
  precomputed at cook time by `PxCooking`. R-tree variant for very large
  meshes.
- **Convex collider: `PxConvexMesh`** — Quickhull-derived convex hull,
  precomputed at cook time. The cook step also computes the SDF used
  for the SDF-collider feature added in 5.1 (PhysX SDF colliders).

PhysX's architectural separation between cook-time geometry and
runtime collision is **the** model Cerid emulates: heavy structures
(BVH, convex hull, SDF) are baked once and loaded as immutable data;
the runtime traverses but never builds.

#### Jolt (Jorrit Rouwé, 2018+; open-sourced 2021)

The Horizon Forbidden West physics engine; rapidly displacing PhysX as
the open-source go-to for 2024+ projects (used by Godot 4.4+, JoltPhysics
ships 16 KLOC of broadphase alone).

- **Broadphase: `BroadPhaseQuadTree`** — *quad*-tree (4-way fan-out
  rather than binary), specifically because four AABBs fit in one SIMD
  ray-vs-AABB test. ~2× throughput vs binary BVH on AVX2 hardware.
- **Narrow phase: GJK** in `GJKClosestPoint` + EPA in `EPAPenetrationDepth`.
  ~3 KLOC each.
- **Mesh collider: `MeshShape`** — quantised quad-tree (same
  representation as broadphase).
- **Determinism: opt-in via `JPH_CROSS_PLATFORM_DETERMINISTIC`** —
  forces software-emulated trig and `_mm_setcsr` calls. ~8 % perf cost.
  Cerid's `crd-eylem` chose to bake determinism in from day 1
  (ADR-0063) rather than gate it; `crd-geometry` inherits the same
  posture.

#### Box2D v3 (Erin Catto, 2024)

Catto's 2024 ground-up rewrite of Box2D, designed for cache locality
and SIMD.

- **Broadphase: `b2DynamicTree`** — binary AABB tree with *rotations*
  (left-rotate / right-rotate to keep the tree balanced under inserts
  and removes). The reference for incrementally-updatable BVH; covered
  in Catto's GDC 2019 *Dynamic Bounding Volume Hierarchies* talk.
- **Narrow phase: GJK + EPA**, deterministic from day 1, Cody-Waite
  trig.
- **No mesh collider** in 2D; Box2D handles polygons + circles only.

#### Havok (Microsoft, 1998+)

Closed-source console physics engine; reference for "how to ship
physics on memory-constrained platforms".

- **`hkBV`** — bounding-volume tree, often quantised aggressively.
- **`hkpMoppCode`** — Memory-Optimized Partial Polytope encoding for
  static meshes; bytecode-compressed BVH that decodes during traversal.
  Saves ~50 % vs uncompressed BVH at the cost of decode overhead.
- Cerid does not pursue this; CPU memory is cheap and decompression
  overhead trades against branch predictor performance.

#### Unity DOTS Physics + Unreal Chaos

- **Unity DOTS: `Unity.Physics`** — Burst-compiled BVH (`BoundingVolumeHierarchy`
  in `Unity.Physics`) over `NativeArray<Aabb>`. SoA throughout. The *data
  layout* is the lesson here — pure SoA, allocator-aware, no virtuals.
- **Unreal Chaos: `Chaos::FBVHParticles`** — particle BVH. Internal API.
  Uses Morton-code linear BVH (LBVH) for build, then refines.
- **Godot: `BroadPhaseBVH`** + GJK in `GJKAlgorithm`. Open source. ~5 KLOC.

### 3.2 Geometric-processing libraries (CGAL-class)

#### CGAL (Computational Geometry Algorithms Library, 1996+)

The canonical academic computational-geometry library. ~1M LOC. Output
of decades of INRIA, MPI, ETH, Tel Aviv research. Triple-licensed
(GPL/LGPL/commercial).

- **Kernel concept** — every algorithm is templatised on a *kernel*
  that defines `Point_3`, `Vector_3`, `Plane_3`, `Triangle_3`, etc.,
  and the predicates `orientation`, `coplanar`, `side_of_oriented_sphere`.
  Two kernels: `Exact_predicates_inexact_constructions_kernel` (EPICK)
  and `Exact_predicates_exact_constructions_kernel` (EPECK). The first
  uses `double` for storage, **exact arithmetic for predicates** (via
  CGAL::Lazy_exact_nt + filtered constructions). The second pushes exact
  through everything. EPICK is the production sweet spot — predicates
  never lie, constructions are fast.
- **Algorithms**: 3D convex hull (Quickhull + others), 2D triangulations
  (Delaunay, constrained Delaunay, Delaunay-conforming), 3D
  triangulation, surface mesh / polyhedron / surface mesh classes,
  arrangement of curves, Boolean operations on Nef polyhedra, mesh
  simplification (QEM via Garland-Heckbert), mesh repair, 3D Boolean
  on triangle meshes (corefinement + classification), the AABB tree
  (`AABB_tree<Traits>`), Bowyer-Watson 3D Delaunay, mesh smoothing
  (Taubin, ARAP), surface reconstruction (Poisson, advancing front).
- **Performance**: not the fastest at any one task. The point of CGAL
  is *correctness*: predicates never wrong, degenerate input never
  crashes, algorithms terminate.

CGAL is **the algorithm reference**. Cerid does not link CGAL (GPL,
1M-LOC dependency, exception-heavy, no allocator discipline). Cerid
*reads* CGAL papers + its source as the algorithmic ground truth.

#### Geogram (Bruno Lévy / INRIA / Alice, 2010+)

Production-grade computational geometry from Alice (the team that
shipped Vorpaline → Graphite → LLNL Mercury). C++, BSD-style. ~150 KLOC.

- **Predicates**: Shewchuk-style adaptive precision (`PCK` —
  predicate-construction kit). Faster than CGAL EPICK on average.
- **Voronoi / Delaunay**: 3D restricted Voronoi diagrams (Lévy 2010
  *Variational Anisotropic Surface Meshing with Voronoi Parallel Linear
  Enumeration*).
- **Mesh repair**: extensive — duplicate vertex merging, intersection
  removal, hole filling, manifoldness recovery.
- **Strong reference for "how to write production-grade geometry C++
  with predicate discipline."**

#### libigl (Daniele Panozzo / Alec Jacobson et al., 2013+)

Modern header-only computational-geometry library, MPL2 licensed.
Eigen-based throughout. ~50 KLOC.

- **Algorithms**: generalized winding number (Jacobson 2013 — *the*
  reference impl), AABB tree, point-in-triangle / point-in-mesh,
  geodesics (heat method), parameterization (LSCM, ARAP, SCAF),
  remeshing, simplification (QEM), tetrahedralization (TetGen wrapper),
  Boolean operations (CGAL wrapper).
- **Strong reference for "modern header-only C++ geometry + Eigen
  ergonomics."** Cerid does not link libigl (Eigen dependency we don't
  want) but borrows API style from it heavily.

#### Geometric Tools / Eberly (David Eberly, 1998+)

The most comprehensive single-author computational-geometry library.
Boost license. ~200 KLOC. Backs up Eberly's three-volume *Geometric
Tools for Computer Graphics* + *3D Game Engine Design*.

- **Distance / intersection**: every pair imaginable — point-segment,
  segment-segment-3D, ray-OBB, capsule-cone, ellipsoid-ellipsoid,
  oriented-bbox-oriented-bbox, etc. **Exhaustive.** When you need
  closest-point-on-X-from-Y, GTE has the formula, the proof, and the
  C++ source.
- **Convex hull**: Quickhull 2D + 3D + nD.
- **Delaunay**: 2D + 3D + nD.
- **Splines, surfaces, curves**: Bézier, B-spline, NURBS.
- **Strong reference for "the closed-form formula for any geometric
  primitive pair."**

#### Open3D (Intel ISL, 2018+)

Modern point-cloud + mesh library. MIT. C++ + Python bindings.
- **Point cloud processing**: KD-tree, voxel downsampling, statistical
  outlier removal, normal estimation.
- **Mesh**: simplification, smoothing, hole filling.
- **Reconstruction**: Poisson surface reconstruction, ball-pivoting.

#### OpenMesh (RWTH Aachen, 2002+)

The canonical **half-edge data structure** library. LGPL. ~30 KLOC.
- Half-edge mesh with property-system architecture: arbitrary properties
  on vertices/edges/faces via dynamic attribute tables.
- Strong reference for half-edge API design. Cerid takes the
  *concept* (half-edge as primary connectivity representation) but
  not the source (LGPL + heavyweight templates).

#### MeshLab + PMP (Polygon Mesh Processing Library)

- **MeshLab** — Cignoni's mesh processing tool / library (VCG library
  underneath). Reference for end-user mesh-tool UX and the algorithm
  catalogue.
- **PMP** (Daniel Sieger / Mario Botsch, 2018) — Botsch's *Polygon Mesh
  Processing* book companion. Clean modern C++ half-edge mesh + the
  algorithm set from the book. ~10 KLOC. **Strong reference for
  "the algorithms a teaching-quality mesh-processing library ships."**

### 3.3 Spatial-acceleration libraries

#### Embree (Intel, 2011+)

The gold-standard CPU ray-tracing BVH. Apache 2.0. ~150 KLOC. The
*reference* for "how fast can a CPU BVH go".

- **Build modes**: SAH (surface-area heuristic — high-quality, slow),
  binned-SAH (fast, near-SAH quality), morton-LBVH (fastest build,
  lower quality). Embree picks dynamically.
- **Topologies**: BVH4 (4-way) for SSE, BVH8 (8-way) for AVX, BVH16
  for AVX-512. The fan-out matches SIMD lane count.
- **Refit**: O(n) bottom-up AABB recomputation.
- **Strong reference for "the BVH every other BVH benchmarks against."**
  Cerid will not match Embree perf at v1 (Embree is the result of a
  decade of microkernel tuning); the *algorithmic shape* (binned SAH,
  fat-BVH topology, refit support) is the right shape to aim at.

#### nanoRT (Syoyo Fujita, 2015+)

Header-only minimal CPU ray tracer / BVH. MIT. ~3 KLOC for the BVH.
- Reference for "the smallest reasonable production BVH."
- Used by indie projects, research code, and as a learning reference.

#### Boost.Geometry (Barend Gehrels et al., 2011+)

Boost-licensed geometry library. R-tree implementation is the standard
C++ R-tree reference. Otherwise heavy on 2D GIS-style operations.

#### Google S2 (2017+)

S2 spherical-geometry library; geo-spatial focus. Apache 2.0. Reference
for spherical-coordinate predicates and quadrilateral cell hierarchies
on the sphere. Out of scope for Cerid (we are not a GIS engine).

### 3.4 Robotics motion-planning libraries

#### FCL — Flexible Collision Library (Pan, Chitta, Manocha 2012)

The reference open-source collision library for robotics. BSD. ~25 KLOC.
- **Broad phase**: BVH (`BVHModel<RSS>` etc.), spatial hash, interval
  tree, dynamic AABB tree.
- **Narrow phase**: GJK + EPA, MPR, separating axis test, swept-volume
  GJK for continuous detection.
- **Bounding volumes**: AABB, OBB, RSS (rectangle-swept-sphere — the
  default for FCL because it's tighter than AABB on rod-like robot
  links), kDOP, kIOS.
- **The single closest analogue to what `crd-geometry` becomes for the
  robotics consumer**. ROS, MoveIt, Drake-collision, OMPL all use FCL.
- **Strong reference for "what does a robotics-grade collision library
  ship?"** Cerid's eylem mesh collider + GJK + EPA + RSS is direct
  parallel.

#### HPP-FCL (humanoid-path-planner fork, Stephane Caron / LAAS-CNRS, 2019+)

Maintained fork of FCL with bug fixes, additional bounding volumes,
better Python bindings. Used by Pinocchio robotics framework. The
reference for "the modern fork of FCL the community has actually
maintained."

#### OMPL (Open Motion Planning Library, Ioan Şucan / Mark Moll, 2010+)

Sampling-based motion planning. Uses FCL for collision; OMPL itself
ships limited geometry (KD-tree for sample / nearest-neighbour). BSD.

### 3.5 Polygon / mesh processing — clipping + Boolean

#### Sutherland-Hodgman (1974)

The classical convex polygon clipping algorithm. O(nm) for n vertices
clipping against m planes. Limited: subject polygon must be convex (or
splits into multiple at concave vertices). Used in every triangle
rasteriser ever shipped (clip-against-frustum). Cerid ships this for
the renderer's frustum clip.

#### Vatti (1992)

The general polygon-clipping algorithm: handles concave + self-intersecting
+ holes. The reference for "general 2D polygon Boolean". Implemented in
**Clipper / Clipper2** (Angus Johnson, BSD, ~10 KLOC). Used by FreeCAD,
Inkscape's path operations, OpenSCAD's 2D primitives.

#### Greiner-Hormann (1998)

Alternative to Vatti; simpler core algorithm but historically
problematic on self-intersecting input. Fixed by Foster-Hormann-Popa
(2019). Cerid does not pick this — Vatti+Clipper2 is more battle-tested.

#### Boost.Polygon (Lucanus Simonson, 2009+)

Manhattan-geometry-focused polygon Boolean library. Coordinate-type
parameterised. Used in EDA / chip layout tools. Strong reference for
"the integer-coordinate Boolean" but Cerid's polygons are
floating-point so we follow Vatti instead.

### 3.6 Convex decomposition

#### V-HACD (Voxelized Hierarchical Approximate Convex Decomposition, John Ratcliff, 2014)

The canonical convex-decomposition algorithm. Voxelizes the input mesh,
recursively splits along best-cut planes, extracts convex hulls of the
resulting voxel clusters. ~5 KLOC. BSD. Used by Bullet, PhysX cooking,
Unity, Unreal collision authoring.

- Offline tool — runs in seconds-to-minutes per mesh, not at runtime.
- Output: list of N convex hulls approximating the input mesh; N
  controlled by a quality parameter.
- **Cerid disposition**: ship in `crd-geometry-decomposition`,
  editor-tier (Phase 7), not runtime.

#### HACD (Hierarchical Approximate Convex Decomposition, Khaled Mamou, 2010)

Predecessor of V-HACD; surface-based rather than voxel-based. Less
robust; superseded by V-HACD.

#### CoACD (Wei et al., 2022)

Modern academic convex-decomposition algorithm — *Approximate Convex
Decomposition for 3D Meshes with Collision-Aware Concavity and Tree
Search* (SIGGRAPH 2022). MIT. Higher quality than V-HACD on some
inputs; slower. Reserved for evaluation.

### 3.7 Tetrahedralization

#### TetGen (Hang Si, 1996+; current 1.6 from 2020)

The reference 3D tetrahedral-mesh generator. Constrained Delaunay
tetrahedralization with quality control. ~50 KLOC C++. AGPL (which
disqualifies it as a Cerid dependency, but it's the algorithmic
reference).

#### fTetWild (Yixin Hu et al., 2020)

*Fast Tetrahedral Meshing in the Wild* (SIGGRAPH 2020). Robust on
non-watertight + self-intersecting meshes (the "in the wild" cases
that break TetGen). MIT. ~10 KLOC. **Cerid disposition**: this is the
algorithm to implement when eylem v7 (FEM) needs tet mesh generation.

### 3.8 Mesh repair

- **MeshFix** (Marco Attene, 2010) — the reference manifoldness-repair
  tool. Wins competitions on "fix the mesh" benchmarks.
- **libigl::is_edge_manifold` / `connected_components`** — Eigen-based
  primitives for diagnosing brokenness.
- **OpenMesh's mesh-doctor** — practical-engineering mesh-repair pass.
- Cerid ships its own (no GPL deps, allocator-discipline, deterministic)
  but tracks MeshFix as the "did we miss a case?" reference.

### 3.9 Convex hull libraries

- **Quickhull** — Barber, Dobkin, Huhdanpaa 1996, *The Quickhull
  Algorithm for Convex Hulls*. Implemented in **qhull** (~30 KLOC,
  permissive). The 3D Quickhull is the algorithm Cerid implements;
  qhull source is the cross-check reference.
- **CGAL Convex_hull_3** — exact-predicates Quickhull variant. The
  reference for "what does a numerically-bulletproof hull do on
  cocircular / coplanar input?"
- **Chan's algorithm (1996)** — output-sensitive 2D hull, O(n log h).
  Asymptotically beats Quickhull on outputs with very few vertices.
  Reserved.

### 3.10 What we synthesize

Cerid's `crd-geometry` is best understood as:

> **PMP-style API ergonomics + libigl-style data orientation +
> CGAL-quality predicates + Embree-style BVH performance + FCL-shape
> coverage of physics-collider primitives**, all under the Cerid
> determinism + allocator-discipline + zero-third-party umbrella.

No existing library covers that intersection. Each cell of that matrix
exists in some library; the intersection is the hole `crd-geometry`
fills.

---

## 4. Algorithmic scope — what `crd-geometry` ships vs defers

This section mirrors the disposition-table format from `cerid-sdf.md`
§3 and `cerid-hesap.md` §3. Each algorithm class gets a table; the
**chosen** algorithm is the one Cerid implements at v1; **reserved**
items ship in later slices when consumer demand surfaces; **rejected**
items are documented so we don't relitigate.

### 4.1 Spatial acceleration — BVH

| Algorithm | Year | Build cost | Query cost | Cerid disposition |
| --- | :---: | :---: | :---: | --- |
| **Top-down SAH BVH (binary)** | classical | O(n log² n) | O(log n) avg | **CHOSEN — v1.** Standard "good-quality, moderate-build" BVH. Reference: Wald 2007 *On fast Construction of SAH-based Bounding Volume Hierarchies*. |
| **Binned SAH** (Wald 2007) | 2007 | O(n log n) | O(log n) avg | **CHOSEN — v1.** The Embree binning trick: bucket-based split candidate evaluation. ~10× faster build than full SAH at ~95 % quality. |
| **LBVH (Linear BVH)** (Lauterbach 2009) | 2009 | O(n) | O(log n) | **Reserved — v9 GPU.** Morton-code sort + radix tree. Build is GPU-friendly; tree quality is poor (Morton-induced spatial discontinuity). Used as a build-only path; refit/rebuild between frames. |
| **HLBVH** (Pantaleoni-Luebke 2010) | 2010 | O(n) | O(log n) | Reserved — alternative GPU build, two-level. |
| **PLOC** (Meister-Bittner 2018) | 2018 | O(n log n) | O(log n) | Reserved — modern parallel BVH builder; better quality than LBVH at modest extra cost. |
| **TRBVH** (Karras-Aila 2013) | 2013 | post-process | O(log n) | Reserved — treelet restructuring as a *post-pass* over LBVH; recovers SAH-quality. |
| **Quad-BVH (4-way fan-out)** | classical | O(n log n) | O(log_4 n) | **CHOSEN — v1 default branch factor.** Matches SIMD lane count 4 (SSE) / 8 (AVX); 1.5–2× ray throughput vs binary BVH. Used by Embree, Jolt. |
| **Refit (bottom-up AABB recomputation)** | classical | O(n) | n/a | **CHOSEN — v1.** Required for dynamic-scene refit-without-rebuild. Reference: Catto GDC 2019. |
| **Tree rotations on insert/erase** | classical | O(log n) per op | n/a | **CHOSEN — v1.** Required for incremental insert/erase. Reference: Catto GDC 2019 + Box2D v3 `b2DynamicTree`. |
| **Naive median-split** | n/a | O(n log n) | poor | Rejected — degenerate quality on long-thin scenes (rooms, terrain). |

Concrete number target: `BvhTree::build` on 100K AABBs ≤ 50 ms on a
single thread with binned SAH; `BvhTree::raycast` ≥ 5M rays/sec
single-threaded against a 1M-triangle mesh. Both within 2× of Embree.

### 4.2 Spatial acceleration — KD-tree

| Algorithm | Year | Best for | Cerid disposition |
| --- | :---: | --- | --- |
| **KD-tree with median split** | classical | nearest-neighbour on point clouds | **CHOSEN — v5.** Standard. ~500 LOC. |
| **KD-tree with SAH split** | 2001 | ray-tracing static geometry | Rejected at v1 — BVH is the better choice for the dynamic-scene case Cerid optimises for. |
| **Bucket KD-tree** | classical | very-large point clouds | Reserved. |
| **ANN (Approximate NN, Mount 1998)** | 1998 | high-dimensional NN | Reserved. |

### 4.3 Spatial acceleration — octree, R-tree, spatial hash

| Structure | Best for | Cerid disposition |
| --- | --- | --- |
| **Loose octree** | sparse large-extent dynamic scenes | **CHOSEN — v5.** Classical loose-octree from Ulrich 2000. Used by `crd-scene::SpatialOctreeIndex` reserved shell. |
| **R-tree (R\*)** | range queries on dynamic 2D/3D | **CHOSEN — v5.** Beckmann 1990 R*-tree. Boost.Geometry is the API reference. |
| **Spatial hash (grid)** | broadphase prefilter on uniform-spread bodies | **CHOSEN — v5.** Reference: Teschner 2003. Used as eylem broadphase optional alternative + fluid neighbour search. |
| **BSP tree** | static-only architecture, in-or-out | Rejected — superseded by BVH for general queries. |

### 4.4 Spatial acceleration — sweep + line intersection

| Algorithm | Year | Use | Cerid disposition |
| --- | :---: | --- | --- |
| **Bentley-Ottmann sweep** | 1979 | all line-segment intersections in O((n + k) log n) | **CHOSEN — v6.** Reference for polygon Boolean intersection-detection step. |
| **Sweep-and-prune (3-axis)** | classical | broadphase alternative on uniform scenes | Reserved — not v1 (BVH wins for the scenes Cerid targets); revisit if a particle-system consumer surfaces. |

### 4.5 Convex hull

| Algorithm | Year | Dim | Complexity | Cerid disposition |
| --- | :---: | :---: | :---: | --- |
| **Andrew's monotone chain** | 1979 | 2D | O(n log n) | **CHOSEN — v3 (2D path).** Simplest correct 2D hull; ~50 LOC. |
| **Graham scan** | 1972 | 2D | O(n log n) | Equivalent quality to monotone chain; monotone chain is slightly cleaner. Rejected as redundant. |
| **Quickhull 2D** | 1996 | 2D | O(n log n) avg | Reserved — algorithmic cousin of 3D hull, useful for code sharing but monotone chain is simpler in 2D. |
| **Quickhull 3D** | 1996 | 3D | O(n log n) avg, O(n²) worst | **CHOSEN — v3.** Barber-Dobkin-Huhdanpaa. The standard 3D hull for physics colliders. ~1200 LOC. |
| **Incremental + randomized** | classical | nD | O(n log n) expected | Reserved for nD if hesap consumer needs it (it won't soon). |
| **Chan's algorithm** | 1996 | 2D | O(n log h) — output-sensitive | Reserved. |
| **Hull simplification** (vertex limit) | n/a | 3D | n/a | **CHOSEN — v3.** Eylem cooks convex colliders with a designer-set max vertex count (typically 32–128); we keep the most-extreme support directions. Reference: V-HACD's hull-simplifier. |

### 4.6 Convex shape distance / intersection

| Algorithm | Year | What it does | Cerid disposition |
| --- | :---: | --- | --- |
| **GJK** (Gilbert-Johnson-Keerthi) | 1988 | distance + boolean test between two convex shapes via support functions | **CHOSEN — v2.** The standard. Linear iterations (2–6 typical). Christer Ericson's *Real-Time Collision Detection* + Catto GDC 2010 + van den Bergen 1999 are the implementation references. |
| **EPA** (Expanding Polytope Algorithm) | 1999 | penetration depth + normal when GJK reports overlap | **CHOSEN — v2.** Van den Bergen 1999. The standard EPA formulation. |
| **MPR** (Minkowski Portal Refinement) | 2008 | alternative to GJK+EPA | Reserved — Game Programming Gems 7 (Snethen). Slower than GJK but more numerically robust on penetrating cases. |
| **GJK-cast** (continuous) | 1997 | swept distance / time-of-impact | Reserved — eylem v6 CCD slice consumer. |
| **SAT** (Separating Axis Theorem) | 1948+ | exact box-box / OBB-OBB / polygon-polygon | **CHOSEN — v2.** Closed-form for box vs box; cheaper than GJK in those special cases. |
| **Lin-Canny** (closest-features) | 1991 | feature-tracking distance for moving convex shapes | Reserved — historical; GJK is preferred. |
| **V-Clip** (Mirtich 1998) | 1998 | feature-based distance + penetration | Rejected — superseded by GJK + EPA. |

### 4.7 Triangle mesh queries

| Query | Algorithm | Cerid disposition |
| --- | --- | --- |
| **Closest point on mesh** | BVH-accelerated; per-leaf-triangle branch from Christer Ericson §5.1.5 | **CHOSEN — v4.** O(log n) avg. |
| **Raycast on mesh** | BVH-accelerated; per-leaf Möller-Trumbore (1997) ray-triangle | **CHOSEN — v4.** O(log n) avg. |
| **Point in mesh — closed watertight** | ray-stabbing parity test | **CHOSEN — v4 fast path.** Rejects on first non-watertight hit; falls back to winding-number. |
| **Point in mesh — robust** | Generalised winding number (Jacobson 2013) | **CHOSEN — v4.** Robust on non-watertight + self-intersecting. The same algorithm `crd-sdf` v2 mesh-bake calls. |
| **Point in mesh — degenerate** | Hormann-Agathos 2001 (*The point in polygon problem for arbitrary polygons*) for the 2D analogue | Reserved — useful when 2D / planar polygon point-in tests fail at degenerate edges. |
| **Inside-outside test** (signed) | combination of closest-point + winding-number for sign | **CHOSEN — v4.** This is what `crd-sdf::bake_mesh_to_grid` consumes. |
| **Nearest-vertex / nearest-edge** | half-edge traversal seeded by closest-point | **CHOSEN — v4.** |

### 4.8 Polygon ops (2D)

| Operation | Algorithm | Cerid disposition |
| --- | --- | --- |
| **Triangulation — convex** | fan triangulation | **CHOSEN — v6.** Trivial. |
| **Triangulation — simple polygon** | ear clipping (O'Rourke) | **CHOSEN — v6.** O(n²) but simple. |
| **Triangulation — with holes** | constrained Delaunay triangulation (CDT) | **CHOSEN — v6.** Bowyer-Watson + edge-flip post-pass. Reference: *Triangulations and Applications* (Hjelle-Dæhlen 2006). |
| **Polygon Boolean (intersection / union / diff / xor)** | Vatti 1992 | **CHOSEN — v6.** Clipper2 is the implementation reference. |
| **Polygon clipping — convex against convex** | Sutherland-Hodgman 1974 | **CHOSEN — v6.** O(nm). The frustum clip + per-triangle clip path. |
| **Convex polygon overlap area** | derivative of Sutherland-Hodgman | **CHOSEN — v6.** |
| **Polygon offset / inset** | (no Cerid consumer at v1) | Reserved. |

### 4.9 Voronoi / Delaunay

| Algorithm | Year | Dim | Cerid disposition |
| --- | :---: | :---: | --- |
| **Bowyer-Watson 2D** | 1981 | 2D | **CHOSEN — v8.** The classical incremental Delaunay. ~500 LOC. |
| **Lawson edge-flip** | 1972 | 2D | **CHOSEN — v8.** Used as the local-repair primitive in CDT. |
| **Bowyer-Watson 3D** | 1981 | 3D | **CHOSEN — v8.** Same algorithm in 3D; predicate-sensitive. |
| **Voronoi from Delaunay dual** | classical | 2D / 3D | **CHOSEN — v8.** Trivial post-construction. |
| **Fortune's sweep-line Voronoi** | 1987 | 2D | Rejected — Bowyer-Watson is more general (works in 3D, supports CDT). |
| **Constrained Delaunay tetrahedralization** | TetGen-class | 3D | Reserved — eylem v7 FEM consumer. References: Si 2015 *TetGen, a Delaunay-Based Quality Tetrahedral Mesh Generator*; fTetWild for "in the wild" robustness. |
| **Restricted Voronoi diagram** | Lévy 2010 | surface | Reserved — research-tier; Geogram is the reference if needed. |

### 4.10 Mesh data structures

| Structure | Trade | Cerid disposition |
| --- | --- | --- |
| **Indexed face set (`vertices: Vec3[]`, `indices: u32[3*N]`)** | minimal, no adjacency | **CHOSEN — v0.** The "input format" Cerid consumes everywhere. Every algorithm operates on `TriangleMeshView` (= a `ConstSpan<Vec3>` + `ConstSpan<u32>`). |
| **Half-edge mesh** | full topology, heavy | **CHOSEN — v4.** Cerid's primary in-memory mesh-processing structure. PMP / OpenMesh API as reference. |
| **Winged edge** | Baumgart 1975 | Rejected — superseded by half-edge in modern usage. |
| **Quad-edge** | Guibas-Stolfi 1985 | Rejected — elegant but heavier than half-edge for triangle-only meshes. |
| **Adjacency arrays (CSR-like)** | compact, read-only | Reserved — useful for cooked / immutable meshes; produced from half-edge at cook. |
| **Compact half-edge / corner table** (Rossignac) | compact half-edge variant | Reserved — half-mem but read-mostly. |

### 4.11 Mesh processing

| Algorithm | Reference | Cerid disposition |
| --- | --- | --- |
| **Quadric Edge Collapse Decimation (QEM)** | Garland-Heckbert 1997 | **CHOSEN — v7.** The standard mesh simplification. Used for LOD chain generation at cook + runtime lighting-bounce mesh. |
| **Loop subdivision** | Loop 1987 | **CHOSEN — v7.** Triangle-mesh subdivision. |
| **Catmull-Clark subdivision** | 1978 | Reserved — quad-mesh subdivision; needs a quad-mesh data structure variant. |
| **Isotropic remeshing** | Botsch-Kobbelt 2004 | **CHOSEN — v7.** Used to clean up triangle quality on imported meshes (for FEM, for SDF baking). |
| **LSCM parameterization** | Lévy 2002 | Reserved — UV unwrapping. Editor-tier. |
| **ARAP parameterization** | Liu et al. 2008 | Reserved. |
| **Hole filling** | Liepa 2003 | **CHOSEN — v7.** Mesh-repair primitive. |
| **Manifoldness fix** | MeshFix-class | **CHOSEN — v7.** Mesh-repair primitive. |
| **Self-intersection removal** | CGAL corefinement | **CHOSEN — v7.** Mesh-repair primitive. |
| **Mesh smoothing** | Taubin λ/μ filter (1995); bilateral mesh smoothing (Fleishman-Drori-Cohen-Or 2003) | **CHOSEN — v7.** Used for mesh-from-scan denoising. |

### 4.12 Convex decomposition

| Algorithm | Year | Cerid disposition |
| --- | :---: | --- |
| **V-HACD** (Mamou 2014) | 2014 | **CHOSEN — v9, editor-tier.** The standard. Cooker-only; not runtime. |
| **HACD** (Mamou 2010) | 2010 | Rejected — superseded by V-HACD. |
| **CoACD** (Wei 2022) | 2022 | Reserved — modern alternative; evaluate if V-HACD quality is insufficient on a real consumer. |

### 4.13 Geometric-primitive ops (the "GTE catalogue")

The exhaustive set of point/line/segment/ray/plane/triangle/sphere/AABB/OBB/capsule
distance + intersection + closest-point pairs. **All ship at v0** as
the substrate that every higher slice builds on.

- Point–{line, segment, ray, plane, triangle, AABB, OBB, sphere, capsule}
- Line–line, line–plane, line–triangle
- Segment–segment-3D, segment–plane, segment–triangle
- Ray–{plane, triangle (Möller-Trumbore 1997), AABB (slab test),
  OBB, sphere, capsule, cylinder}
- Plane–plane, plane–triangle
- Triangle–triangle (Möller 1997 *A Fast Triangle-Triangle Intersection
  Test*)
- Sphere–{sphere, AABB, OBB, capsule, plane, triangle}
- AABB–AABB, AABB–OBB, AABB–triangle (Akenine-Möller 2001)
- OBB–OBB (separating axis theorem, 15 axes)
- Capsule–capsule (closest-point on segment pair)
- Barycentric coordinates: point in {triangle, tetrahedron}
  (Christer Ericson §3.4)

Reference: Eberly *Geometric Tools for Computer Graphics* (the
exhaustive catalogue) + Christer Ericson *Real-Time Collision
Detection* (the curated subset that physics actually uses).
**~3 KLOC** of closed-form geometry; ships at v0 because every higher
slice depends on it.

### 4.14 What is OUT of scope

- **SDF and voxel grids** — `crd-sdf`. Geometry stays on explicit data.
- **Sparse linear algebra used internally** — defer to `crd-hesap-direct` /
  `crd-hesap-iterative` once that substrate ships. (E.g. CDT's
  edge-flip post-pass needs no LA; isotropic remeshing's geometric
  mean step is local.)
- **2D image processing** — out of substrate-tier scope. Lives in a
  future `crd-image` module if ever needed.
- **Mesh streaming + IO format parsers** (glTF, FBX, USD, OBJ, PLY,
  STL parsers) — those live in `tools/asset_cooker/`. Geometry consumes
  cooked `MeshResource` data and `TriangleMeshView` spans; never source
  asset format data.
- **Surface reconstruction from point clouds** (Poisson, ball-pivoting)
  — research-tier; reserved if a medical-imaging consumer surfaces.
- **Rendering / visualisation of geometry** — the renderer's job.
  Geometry produces data; renderer draws it.
- **Symbolic geometry / CAS-style** — out of substrate-tier scope.

---

## 5. Determinism contract

`crd-geometry` inherits **ADR-0063 (eylem determinism contract)**
wholesale — same compile flags (`-ffp-contract=off`, `/fp:precise`),
same FPU state, same banned stdlib (no `std::sin/cos/atan2`, no
`std::sort`, no `std::hash`), same Cerid-internal substitutions
(`crd::math::deterministic` for trig; `crd::containers::sort`; FNV-1a
for hash). The deltas specific to computational geometry are below;
all are concrete, all are CI-checkable.

### 5.1 GJK simplex update — pin the tiebreak

The GJK simplex-update step (which vertex to drop when the new
support point produces a degenerate simplex) has **two competing
conventions**:

- **Christer Ericson** (*Real-Time Collision Detection* §9.5) drops
  the vertex with the smallest barycentric weight under the new
  closest-point computation.
- **Van den Bergen** (*Collision Detection in Interactive 3D
  Environments* 2003 §4.3.5) drops the vertex with the smallest index
  in the simplex.

When two weights are within machine epsilon, the two conventions
produce different simplex states. Across compilers / SIMD widths,
which convention "wins" diverges. Cerid pins the rule:

> **Cerid GJK uses Ericson barycentric weights; ties broken by
> drop-the-lowest-index-vertex.** If two weights are within
> `Geometry::kSimplexEpsilon = 1e-7f` of each other in absolute value,
> we drop the one with the lower simplex-slot index.

This produces bit-exact GJK behaviour across all `crd-eylem-deterministic`
build configurations.

### 5.2 BVH build — pin the SAH split tiebreak

When the binned-SAH cost evaluation produces multiple candidate splits
with cost within `Geometry::kSahCostEpsilon = 1e-6f`:

> **Cerid SAH split chooses the X axis first; then Y; then Z. Within
> an axis, the lower bin index wins.**

This is a strict deterministic ordering. No `std::sort` in the build
(use `crd::containers::sort` which is the deterministic introsort with
pinned tie-breaker). No `std::accumulate`-style reductions across
threads — the parallel build sums per-bin AABB centroids using a
deterministic reduction tree (binary tree over thread results, in
fixed thread-id order).

### 5.3 Convex hull — pin the lex-order tiebreak

When Quickhull encounters cocircular / coplanar points:

> **Cerid Quickhull breaks coplanar/cocircular ties by
> lexicographic order on `(x, y, z)` of the candidate vertex.**

This produces bit-exact hulls for the same input across all
configurations. CGAL EPICK does this with exact predicates; Cerid
does it with float predicates + a pinned tiebreak rule. The cost is
that pathologically degenerate inputs (4096 cocircular points) may
produce a slightly suboptimal hull (vs. EPICK), but the answer is
identical across runs.

### 5.4 Polygon Boolean — pin Vatti's degenerate-edge handling

Vatti 1992's algorithm has multiple legitimate ways to handle
edge-on-edge / vertex-on-edge events. Cerid pins the **Clipper2
convention** (which is itself the most widely-shared interpretation):

> **At a vertex-on-edge intersection, the vertex is treated as
> infinitesimally above the edge in `+y` direction.**

This makes the algorithm well-defined on degenerate inputs.
Implementation references: Clipper2 source (BSD; algorithmic
reference, not a link).

### 5.5 Robust geometric predicates — the open question

The hard one. Two strategies in tension:

| Strategy | Cost | Robustness |
| --- | --- | --- |
| **Shewchuk adaptive expansions** (1997, *Adaptive Precision Floating-Point Arithmetic and Fast Robust Geometric Predicates*) | ~5–10× a naive predicate; staged precision (cheap test first, exact fallback on tie) | Bulletproof — exact for any IEEE-754 input |
| **Fixed-precision with documented epsilon** | 1× | Works for non-pathological input; fails on cocircular / coplanar / etc edge cases |

`crd-geometry` v0 ships **fixed-precision predicates with a
documented epsilon contract** (the SDF substrate's posture). Adaptive
predicates land in v1 *only if* a real consumer (the `crd-sdf`
mesh-bake on real-world non-watertight meshes) shows a regression.
This is surfaced in §11 as an explicit open question.

### 5.6 Cross-thread reductions

All cross-thread reductions in `crd-geometry` (parallel BVH build,
parallel Quickhull, parallel mesh-processing) use **commutative
reductions over fixed-id orderings** (per ADR-0063 §5). No
"first-thread-to-finish-wins" patterns; no thread-id-dependent
ordering.

### 5.7 GPU geometry kernels (Phase 3.1.5+ if needed)

The future `crd-geometry-gpu` sub-module (§7.9) ships GPU LBVH +
parallel Quickhull. **Bit-exact match with the CPU path** is the
contract — the same predicate strategy (fixed-precision with
documented epsilon) ports to GLSL/HLSL with the same constants.
Reference: PhysX's GPU narrow phase achieves this with `_mm_setcsr`
and per-shader `precise` qualifiers.

### 5.8 What CI catches

- **Build-twice byte-exact**: build a BVH from the same input twice in
  the same process; assert the two trees serialise to the same bytes.
- **Cross-platform byte-exact**: build a BVH on Windows-MSVC and on
  Linux-GCC; assert the same bytes.
- **Replay-hash CI**: eylem's existing replay-hash CI (Phase 3.1 v9b
  matrix) catches `crd-geometry` regressions automatically because
  every eylem step that calls into the BVH or GJK is hashed.

---

## 6. API design philosophy

### 6.1 The two-layer choice (mirrors `crd-hesap`)

- **Layer A — typed C++ engine API.** Used by `crd-eylem`, `crd-sdf`,
  `crd-renderer`, `crd-scene`. Reference: libigl's data orientation +
  PMP's API ergonomics. Free-function-first: `bvh_build(...) → BvhTree`,
  `gjk_distance(SupportFn, SupportFn) → f32`, `mesh_closest_point(view,
  point) → ClosestPoint`.
- **Layer B — high-level façade for tooling.** Used by editor, cooker,
  REPL. Builder-pattern + named-argument struct style:
  `MeshProcessor{ allocator }.simplify(mesh).remesh().repair()`.

### 6.2 Data-oriented throughout

Pass `ConstSpan<Vec3>` of vertices and `ConstSpan<u32>` of indices —
not `Mesh*` objects. The consumer owns the storage representation;
`crd-geometry` operates on a *view* of that storage. This matches:

- `crd-resources::MeshResource` already stores `vertices` + `indices`
  as raw spans in the cooked artifact.
- `crd-eylem` will store its collider geometry in the per-archetype
  SoA storage; passing object pointers would force an extra layer of
  indirection.
- `crd-sdf` mesh-bake consumes the bake input as a `TriangleMeshView`
  which is a thin alias over the same span pair.

### 6.3 Functional algorithm form

`bvh_build(...) → BvhTree` not `BvhTree::build(...)`. Three reasons:

1. **Allocator discipline** — the `IAllocator*` is passed to the
   builder, and the resulting tree owns its memory through that
   allocator.
2. **Multiple build strategies** — `bvh_build_sah(...)`,
   `bvh_build_lbvh(...)`, `bvh_build_morton(...)`. Free functions
   compose into named entry points; instance methods would either
   require enum-dispatched switches or virtual dispatch.
3. **Testability** — pure functions are trivial to unit-test.

### 6.4 ECS-friendly

Where consumers want ECS integration:

- **BVH refit** is a `crd-scene::ISystem` registered by the consumer
  module (eylem's broadphase system, scene's `SpatialBVHIndex`
  rebuild system).
- **Spatial hash** is exposed via the `IComponentIndex` shell pattern
  established by ADR-0053 (`SpatialBVHIndex`, `SpatialHashIndex`).
- Query results are arrays of `(EntityId, HitData)` pairs, packed in
  a `ConstSpan` over per-frame allocator memory (`crd::jobs::frame_alloc`).

### 6.5 Allocator discipline (CLAUDE.md)

Every constructor takes `IAllocator*`. Every container is
`crd::containers::*`. No `std::vector`, no `std::string`, no
`std::unordered_map` — same posture as the rest of the engine.

### 6.6 GPU-first reservations

BVH leaves are designed to map to GPU AABB sets cleanly: 32-byte
node layout (2 × `Vec4f` for child AABB min/max + child indices
packed into one `u32`). When the GPU sub-module ships, the CPU
representation uploads as-is to the GPU LBVH consumer with no
re-layout step. Same for the half-edge mesh: the cooker emits a
GPU-friendly adjacency-array representation alongside.

### 6.7 No exceptions

All fallible operations return `crd::core::Result<T, Error>` (the
existing engine result-type pattern from `crd-resources`'s loader
API). No exceptions cross the API surface, no exceptions used
internally — same posture as `crd-rhi`, `crd-shader`, `crd-resources`.

---

## 7. Module split — sub-modules

Mirrors the `crd-hesap` 14-submodule pattern (ADR-0065 §1). Each
sub-module compiles independently; consumers link only what they use.
The umbrella target `crd-geometry` is a meta-target convenience.

### 7.1 `crd-geometry-primitives` (v0)

- All point / line / segment / ray / plane / triangle / sphere /
  AABB / OBB / capsule distance + intersection + closest-point
  formulas (§4.13 catalogue).
- `Plane`, `Ray`, `AABB`, `OBB`, `Sphere`, `Capsule`, `Triangle3`
  primitive types.
- Barycentric utilities.
- Frustum (six planes) + frustum-vs-AABB / frustum-vs-sphere tests.
- ~3 KLOC. The substrate every higher slice builds on.

### 7.2 `crd-geometry-bvh` (v1)

- `BvhTree<AABB>` with binned-SAH builder, O(n) refit, O(log n) per
  insert/remove via tree rotations.
- Quad-BVH topology (4 children per node) by default; binary BVH
  topology behind a `BvhTopology::Binary` parameter.
- `BvhTree::raycast`, `BvhTree::query`, `BvhTree::closest_point`
  query helpers.
- ~5 KLOC.

### 7.3 `crd-geometry-convex` (v2 + v3)

- v2: GJK + EPA + SAT for box-pairs.
- v3: Quickhull 2D + 3D, hull simplification.
- ~4 KLOC.

### 7.4 `crd-geometry-mesh` (v4)

- `TriangleMeshView` (read-only thin view).
- Half-edge mesh data structure.
- BVH-accelerated mesh closest-point + raycast + winding-number.
- Conversion utilities between indexed-face-set and half-edge.
- ~3 KLOC.

### 7.5 `crd-geometry-spatial` (v5)

- KD-tree, octree, R-tree, spatial hash. All four implementations
  share the same query-result format.
- ~3 KLOC.

### 7.6 `crd-geometry-polygon` (v6)

- 2D triangulation (ear clipping, CDT).
- Polygon Boolean (Vatti).
- Sutherland-Hodgman clip.
- Bentley-Ottmann sweep for the intersection-detection step.
- ~3 KLOC.

### 7.7 `crd-geometry-mesh-processing` (v7)

- QEM simplification.
- Loop subdivision.
- Isotropic remeshing.
- Hole filling, manifoldness fix, self-intersection removal,
  Taubin smoothing.
- ~4 KLOC.

### 7.8 `crd-geometry-delaunay` (v8)

- 2D Bowyer-Watson, 2D CDT, 3D Bowyer-Watson, Voronoi-from-Delaunay.
- ~2 KLOC.

### 7.9 `crd-geometry-gpu` (v9 — Phase 3.5+ when consumers exist)

- GPU LBVH builder (Karras 2012 + 2013).
- GPU parallel BVH refit.
- GPU GJK (per-pair compute thread).
- ~3 KLOC.

### 7.10 `crd-geometry-decomposition` (v9 — editor tier)

- V-HACD-class convex decomposition.
- Cooker-only; not runtime.
- ~5 KLOC.

**Total: ~35 KLOC across 10 sub-modules over ~25 slices.** Comparable
to `crd-hesap` (~50 KLOC across 14 sub-modules over 18 slices) and
`crd-sdf` (~10 KLOC across 1 module + extraction over 8 slices).

---

## 8. Phase plan — slice list with LOC estimates

The phase plan develops in `docs/phases/phase-3.1.7-geometry.md`
(to be drafted from this dossier). Slice list below mirrors the
v-numbering convention used in Phase 3.1 (eylem) and 3.1.5 (sdf).

### v0 — primitives substrate (~1 week, ~3 KLOC)

- v0a: types — `Plane`, `Ray`, `AABB`, `OBB`, `Sphere`, `Capsule`,
  `Triangle3`, `Frustum`. ~500 LOC.
- v0b: closest-point formulas (point–everything). ~800 LOC.
- v0c: intersection tests (everything–everything). ~1500 LOC.
- v0d: barycentric + 3-tetrahedron utilities. ~200 LOC.

### v1 — BVH (~2 weeks, ~5 KLOC)

- v1a: binary BVH with binned SAH builder + raycast.
- v1b: O(n) bottom-up refit.
- v1c: incremental insert/erase with tree rotations (Catto GDC 2019).
- v1d: quad-BVH topology variant + SIMD ray-vs-4-AABB.
- v1e: closest-point query.
- v1f: integration test against Embree (off-by-default benchmark).

### v2 — GJK + EPA (~2 weeks, ~3 KLOC)

- v2a: support functions for primitive shapes.
- v2b: GJK distance (Ericson reference).
- v2c: GJK boolean test.
- v2d: EPA penetration depth + normal.
- v2e: SAT for box-vs-box (specialised fast path).
- v2f: deterministic-tiebreak conformance tests.

### v3 — convex hull (~1 week, ~2 KLOC)

- v3a: 2D monotone chain.
- v3b: 3D Quickhull (Barber 1996).
- v3c: hull simplification (vertex-budget keep-N-extremes).

### v4 — triangle mesh queries (~2 weeks, ~3 KLOC)

- v4a: `TriangleMeshView`.
- v4b: half-edge mesh data structure.
- v4c: BVH-accelerated mesh closest-point.
- v4d: BVH-accelerated mesh raycast (Möller-Trumbore in leaves).
- v4e: generalised winding number (Jacobson 2013).
- v4f: integration with `crd-sdf` mesh-bake (replaces the `crd-eylem`
  BVH dep in ADR-0064 §4).

### v5 — additional spatial accelerators (~2 weeks, ~3 KLOC)

- v5a: KD-tree.
- v5b: loose octree (Ulrich 2000).
- v5c: R-tree (Beckmann 1990).
- v5d: spatial hash (Teschner 2003).
- v5e: integration with `crd-scene::SpatialBVHIndex` /
  `SpatialHashIndex` / `SpatialOctreeIndex` reserved shells (ADR-0053).

### v6 — polygon ops (~2 weeks, ~3 KLOC)

- v6a: ear clipping triangulation.
- v6b: constrained Delaunay triangulation (Bowyer-Watson + edge flip).
- v6c: Sutherland-Hodgman convex clip.
- v6d: Vatti polygon Boolean.
- v6e: Bentley-Ottmann line-segment intersection.

### v7 — mesh processing (~3 weeks, ~4 KLOC)

- v7a: Quadric Edge Collapse Decimation (Garland-Heckbert 1997).
- v7b: Loop subdivision.
- v7c: isotropic remeshing.
- v7d: hole filling (Liepa 2003).
- v7e: manifoldness repair.
- v7f: self-intersection removal.
- v7g: Taubin smoothing.

### v8 — Delaunay + Voronoi (~1 week, ~2 KLOC)

- v8a: 2D Bowyer-Watson.
- v8b: 2D CDT.
- v8c: 3D Bowyer-Watson.
- v8d: Voronoi-from-Delaunay extraction.

### v9 — GPU mirror + decomposition + REPL (~3 weeks, ~8 KLOC)

- v9a: GPU LBVH builder (Karras 2012).
- v9b: GPU BVH refit.
- v9c: V-HACD convex decomposition (editor-tier, cooker-side).
- v9d: REPL bindings (consumed by the future `crd-hesap-repl` notebook
  surface for "draw this convex hull / Voronoi / triangulation").

**Total: ~25 slices across ~9 sub-modules; ~14 KLOC engine + ~5 KLOC
editor-tier; ~4–6 months of substrate work.**

---

## 9. Consumer integration plan

This section makes concrete the API calls the future ADR locks. Each
consumer is named with its current state and the callsite that lights
up post-`crd-geometry-v*`.

### 9.1 `crd-eylem` v1c — broadphase

**Today (Phase 3.1 v1c):** eylem ships its OWN dynamic AABB tree
(per ADR-0062 §3 — "Catto GDC 2019 dynamic AABB tree"). Same pattern
as `eylem-iterative` shipping its own CG before `crd-hesap-iterative`
existed (per the `crd-hesap` deferred-refactor pattern in ADR-0065).

**Post-v1 of `crd-geometry`:** eylem's broadphase becomes a thin
wrapper over `crd-geometry-bvh::BvhTree<AABB>`. The dynamic-tree
operations (insert / remove / refit) are forwarded; the broadphase
ECS system structure stays in eylem (it knows about contact
manifolds, sleeping, islands — none of which is geometry's concern).

**Refactor reservation:** eylem v1c passes its full broadphase
test suite against the eylem-internal BVH; the v1 of `crd-geometry`
adopts those tests as its own; the eylem migration is a 2-day refactor.

### 9.2 `crd-eylem` v1d — narrow phase (convex–convex)

**Today:** eylem v1d is on the Phase 3.1 plan to ship its own GJK
+ EPA. **Disposition:** do NOT ship eylem-internal GJK if `crd-geometry`
v2 lands first. Wait for `crd-geometry-convex` and consume directly.

**Post-v2:** eylem v1d calls
`crd::geometry::gjk_distance(SupportFn, SupportFn)` and
`crd::geometry::epa_penetration(...)` directly. Eylem retains the
contact-manifold construction logic (which is contact-domain-specific)
and the support-function definitions for its collider types.

### 9.3 `crd-eylem` v1d-mesh — TriangleMesh collider

**Status:** planned for after eylem v1d basic narrow-phase. Direct
consumer of `crd-geometry-mesh` v4. Eylem implements
`TriangleMeshShape` with the geometry calls:

```cpp
// Eylem's TriangleMeshShape::raycast (sketch)
auto result = crd::geometry::mesh_raycast(view, ray);
// Eylem then converts result into the contact-manifold representation.
```

### 9.4 `crd-eylem` v1c — collider conditioning

**Status:** Phase 3.1 v1c. Eylem cooks designer-authored convex
colliders through `crd::geometry::convex_hull_3d(designer_vertices)`
to enforce a clean hull (the v3 hull simplifier respects designer
vertex budgets).

### 9.5 `crd-sdf` v2 — mesh-bake (Jacobson winding number)

**Status:** Phase 3.1.5 v2. The mesh-bake step calls into geometry
twice:

```cpp
// crd-sdf::bake_mesh_to_grid (sketch)
auto bvh = crd::geometry::bvh_build(triangle_aabbs);
parallel_for(voxels, [&](Voxel v) {
    auto closest = crd::geometry::bvh_closest_triangle(bvh, v.center);
    auto wn = crd::geometry::mesh_winding_number(view, v.center);
    grid[v.index] = (wn > 0.5f ? -1.0f : +1.0f) * length(v.center - closest.point);
});
```

ADR-0064 §4 currently says "reuses the dynamic AABB tree from
`crd-eylem`'s broadphase". On `crd-geometry` v1 + v4 landing, this
sentence becomes "uses `crd::geometry::bvh_build` and
`crd::geometry::mesh_winding_number`." Documented refactor.

### 9.6 `crd-renderer` — frustum cull + occlusion BVH + decals

**Today (v1):** the renderer's frustum culler is a placeholder linear
scan over `Renderable[]`. ADR-0017 (culling) commits to BVH-accelerated
culling once the scene grows.

**Post-v1 of `crd-geometry`:** the renderer's
`FrustumCullingPass::execute()` calls
`crd::geometry::frustum_cull(scene_bvh, camera.frustum())`.

**Future (v3.5+):** occlusion BVH for Hi-Z / hi-Z occlusion-culling
path; decal projection via Sutherland-Hodgman polygon clip.

### 9.7 `crd-scene::SpatialBVHIndex` (ADR-0053 reserved shell)

**Status:** the index *interface* exists already (Phase 3.0 v1k —
`IComponentIndex`); the BVH backend is reserved-shell only. Once
`crd-geometry-bvh` ships, the shell's `rebuild()` method calls
`bvh_build`, the `query()` method calls `BvhTree::query`, the
`refit()` method calls `BvhTree::refit`. **One-day glue PR.**

### 9.8 `crd-audio` Phase 3.4 — acoustic ray-cast

**Status:** Phase 3.4 future. Steam-Audio-style acoustic propagation
pre-bakes paths by Monte-Carlo ray-casting from sources/listeners
against scene geometry. Direct consumer of
`crd::geometry::mesh_raycast` (per-triangle-material reflection
coefficients) + `BvhTree::raycast` for fast scene-level rejection.

### 9.9 `crd-eylem-aero` (ADR-0073 reserved)

Reserved aerodynamic-physics module. Consumes `crd-geometry-mesh`
for surface-area integration over the body's mesh + per-triangle
normal sampling.

### 9.10 `crd-eylem-cine` (ADR-0074 reserved)

Reserved cinematic-physics module. Consumes `crd-geometry-mesh` for
animated-mesh queries — the BVH refit/rebuild distinction matters
here (skinned meshes refit per-frame; topology-stable but vertex-positions
change).

### 9.11 Editor (Phase 7)

**Direct consumers:**

- Convex decomposition — `crd::geometry::convex_decomposition_vhacd`
  for the V-HACD pipeline (designer authors a single mesh, editor
  emits N convex hulls for runtime physics).
- Mesh repair — manifoldness fix, hole fill, self-intersection
  removal exposed as editor mesh-doctor commands.
- Polygon Boolean — selection ops (union / subtract / intersect on
  polygonal lasso selections).

### 9.12 Cooker

**Direct consumer of multiple sub-modules:**

- BVH baker — every cooked `MeshResource` includes a pre-built BVH
  (reduces runtime build work to zero for static colliders).
- Convex hull baker — every `ConvexResource` includes a Quickhull-cleaned
  hull from the designer's input vertices.
- Winding-number queries — driver of `crd-sdf` cooker's mesh-bake.

---

## 10. Where it slots in the broader Cerid roadmap

Three candidate insertions, with the trade-off articulated. **Cerid
recommends Phase 3.1.7.**

### 10.1 Option A — Phase 3.1.45 (between eylem v1 and sdf)

Insert *before* sdf so sdf can consume `crd-geometry` from day 1.

- **Pro:** ADR-0064 §4 doesn't need its "reuses eylem BVH" line at
  all — sdf just calls geometry directly.
- **Pro:** eylem v2 (rigid 2D specialisation) and beyond can build
  their broadphase on geometry from the start.
- **Con:** delays sdf by ~4–6 months; sdf is already on the
  near-term roadmap and well-scoped.
- **Con:** eylem v1c is *currently shipping* its own BVH; reordering
  to wait for geometry is a real disruption.

### 10.2 Option B — Phase 3.1.55 (between sdf and hesap)

Insert immediately after sdf but before hesap. Geometry is
"closer to sdf" architecturally (both are spatial substrates).

- **Pro:** clusters the two spatial substrates.
- **Pro:** sdf is the immediate consumer; the refactor of ADR-0064 §4
  happens fresh while sdf is still being implemented.
- **Con:** sdf is mid-flight; substrate-changing under it is risky.
- **Con:** hesap is a much larger substrate; deferring it for
  geometry pushes the FEM / robotics / DAW timelines.

### 10.3 Option C — Phase 3.1.7 (between hesap and Phase 3.2 animation) — RECOMMENDED

Insert *after* both sdf and hesap, sequentially in the substrate-tier
ordering.

- **Pro:** geometry depends on neither sdf nor hesap — but both sdf
  and hesap may grow incidental geometry needs (hesap's `crd-hesap-tensor`
  could surface a "sparse-AABB" use that wants a BVH; sdf's v3
  narrow-band tile structure benefits from spatial-hash). Landing
  geometry last lets it absorb those late requirements.
- **Pro:** the refactor of ADR-0064 §4 is documented as a deferred
  refactor with a clear v4f slice (in §8 above) — same pattern as
  `eylem-iterative` → `crd-hesap-iterative` documented in ADR-0065.
- **Pro:** lets eylem v1c–v1d ship its own internal BVH and GJK
  *first*, which is the cleanest substrate-bootstrap — eylem's
  internal versions become the v1 `crd-geometry` test fixtures
  ("if the substrate produces different output than eylem's internal,
  one of them is wrong").
- **Pro:** consolidates all four substrate-tier modules (eylem,
  sdf, hesap, geometry) before Phase 3.2 (animation) and Phase 3.3
  (font / MTSDF). Animation needs none of them; font needs only sdf;
  the substrate-tier sequencing is clean.
- **Con:** geometry's renderer / scene / audio consumers wait
  ~6 months longer than under Option A.
- **Con:** the ADR-0064 §4 "reuses eylem BVH" line stays in place
  for ~6 months as a known-deferred-refactor.

**Recommendation: Phase 3.1.7.** The deferred-refactor pattern is
already established (ADR-0065 + eylem v7 FEM doing the same with
hesap-iterative). The substrate-tier sequencing wins on architectural
clarity — one substrate-tier per quarter, in dependency order
(physics → implicit geometry → numerics → explicit geometry).

### 10.4 Estimated total

- ~25 slices across 9 sub-modules.
- ~14 KLOC engine + ~5 KLOC editor-tier.
- ~4–6 months calendar (assuming v1 + v2 + v4 are the critical-path
  v0-then-three-slice stack required by physics + sdf consumers; later
  slices ship as consumer demand surfaces).

---

## 11. Open research notes

Items surfaced during research that don't make it into v0–v9 but
should be revisited as consumers drive them.

### 11.1 Robust geometric predicates — the unresolved tension

**Status:** v0 ships fixed-precision predicates with a documented
`Geometry::kPredicateEpsilon`. **Open question:** should v1 (post-`crd-sdf`
mesh-bake stress tests) upgrade to Shewchuk-style adaptive predicates?

The data point that resolves this: run the `crd-sdf` v2 mesh-bake
on a corpus of real-world non-watertight glTF / FBX assets and
measure (a) how many produce wrong-sign voxels at the boundary,
(b) how many produce non-deterministic across-platform output. If
(a) > 1 % or (b) > 0 %, adopt Shewchuk. Otherwise stay
fixed-precision.

References:
- Shewchuk (1997) — *Adaptive Precision Floating-Point Arithmetic
  and Fast Robust Geometric Predicates*. Discrete & Computational
  Geometry.
- Geogram's PCK is the production reference impl.

### 11.2 Half-edge mesh ownership — own vs wrap

**Status:** v4 ships a Cerid-native half-edge mesh (no GPL deps,
allocator-discipline, deterministic). **Open question:** does the
Cerid implementation reach feature parity with OpenMesh / PMP within
~3 KLOC, or does it stretch to ~8 KLOC?

OpenMesh's property-system architecture (arbitrary properties per
vertex/edge/face via dynamic attribute tables) is heavyweight. PMP
restricts properties to a fixed compile-time set + a `std::map`
fallback. Cerid v4 ships the PMP-style fixed-set; opens the question
of when arbitrary properties become a tooling requirement
(probably never for the engine; possibly for the editor's mesh-doctor
panel). Reserved for a later editor-tier refactor.

### 11.3 GPU BVH builder — LBVH vs PLOC vs HLBVH first

**Status:** v9a ships LBVH (Karras 2012). **Open question:** does
PLOC (Meister-Bittner 2018) or HLBVH (Pantaleoni-Luebke 2010) ship
v9b for higher-quality builds?

LBVH is cheapest to implement (~500 LOC of GPU code), produces
poor-quality trees (Morton-induced spatial discontinuity) but builds
extremely fast. The "TRBVH treelet restructuring" post-pass
(Karras-Aila 2013) recovers SAH-quality at modest cost. The decision
is whether to ship LBVH + TRBVH together, or LBVH first and let
consumer measurements drive the upgrade.

References:
- Lauterbach (2009) — *Fast BVH Construction on GPUs*. EG.
- Karras (2012) — *Maximizing Parallelism in the Construction of
  BVHs, Octrees, and k-d Trees*. HPG.
- Karras-Aila (2013) — *Fast Parallel Construction of High-Quality
  Bounding Volume Hierarchies*. HPG.
- Pantaleoni-Luebke (2010) — *HLBVH: Hierarchical LBVH Construction
  for Real-Time Ray Tracing*. HPG.
- Meister-Bittner (2018) — *Parallel Locally-Ordered Clustering for
  Bounding Volume Hierarchy Construction*. TVCG.

### 11.4 Convex decomposition — runtime feasibility

**Status:** v9c ships V-HACD as a cooker-only / editor-tier tool.
**Open question:** is there a use case for runtime convex decomposition?

V-HACD takes seconds-to-minutes per mesh. It's plainly an offline
tool. But: a procedurally-destructed mesh (dynamically generated
fragments after a destruction event) could in principle want
runtime decomposition. Reserved — not a v9 concern; revisit if a
real consumer surfaces.

### 11.5 Cross-platform exact predicates via integer arithmetic

**Status:** if §11.1 resolves toward "we need exact predicates",
the Shewchuk adaptive-expansion approach is the most-used. **Reserved
alternative:** use 64-bit integer arithmetic (snap floats to a
fixed quantisation grid; integer arithmetic is exact) — the **Clipper2
posture** for 2D polygon Boolean.

Trade-off: integer arithmetic is genuinely exact but loses the
"dynamic range" of floats; CAD-style precision (1 mm precision over
a 1 km world = 6 decimal digits, comfortably 64-bit integer) is fine,
but scientific-precision (1 nm over 1 km = 12 digits) wants 128-bit
or floating-point. Cerid's geometry is *macro* — physics colliders,
audio surfaces, render-time culling — so quantised integer is a
plausible alternative. Reserved for evaluation if Shewchuk overhead
proves unacceptable.

### 11.6 Differentiable geometry primitives

**Status:** out of scope at v0–v9. **Open question:** Phase 3.1 v9
(differentiable physics) needs gradients through the geometry stack.
Ray-vs-triangle has a closed-form gradient; closest-point-on-mesh
gradient is well-defined except at the medial axis. GJK distance is
piecewise-smooth — gradients exist almost everywhere. **Reserved
hook for Phase 3.1 v9.**

### 11.7 Surface reconstruction from point clouds

Reserved — Poisson surface reconstruction (Kazhdan 2006), ball-pivoting
(Bernardini 1999), screened Poisson (Kazhdan-Hoppe 2013). Ships if a
medical-imaging consumer (DICOM-derived volumes converting to mesh)
surfaces.

### 11.8 N-dimensional geometry

CGAL supports nD via templated kernels. Cerid v0–v9 is 2D + 3D only.
Reserved for a future scientific-computing consumer if it materialises.

---

## 12. References (curated)

### 12.1 Books

- Christer Ericson (2005) — *Real-Time Collision Detection*. Morgan
  Kaufmann. **The single most-referenced book in this dossier.**
- David Eberly (2007) — *3D Game Engine Design* (2nd ed.). Morgan
  Kaufmann. **The exhaustive geometric-primitive catalogue.**
- David Eberly (2014) — *Geometric Tools for Computer Graphics*.
  Morgan Kaufmann. (GTE library companion.)
- Joseph O'Rourke (1998) — *Computational Geometry in C* (2nd ed.).
  Cambridge.
- de Berg, Cheong, van Kreveld, Overmars (2008) — *Computational
  Geometry: Algorithms and Applications* (3rd ed.). Springer. (The
  canonical CG textbook.)
- Mario Botsch et al. (2010) — *Polygon Mesh Processing*. A K Peters /
  CRC. **The single most-referenced book for §4.10–4.11.**
- Sieger & Botsch (2018) — *PMP Library* documentation (companion to
  the book).
- Gino van den Bergen (2003) — *Collision Detection in Interactive
  3D Environments*. Morgan Kaufmann. (GJK / EPA reference.)
- Hjelle & Dæhlen (2006) — *Triangulations and Applications*.
  Springer. (CDT reference.)
- Akenine-Möller, Haines, Hoffman et al. (2018) — *Real-Time
  Rendering* (4th ed.). A K Peters / CRC. (Frustum cull, BVH for
  rendering.)

### 12.2 Papers — spatial acceleration

- Lauterbach et al. (2009) — *Fast BVH Construction on GPUs*. EG.
- Karras (2012) — *Maximizing Parallelism in the Construction of
  BVHs, Octrees, and k-d Trees*. HPG.
- Karras & Aila (2013) — *Fast Parallel Construction of High-Quality
  Bounding Volume Hierarchies*. HPG.
- Wald (2007) — *On fast Construction of SAH-based Bounding Volume
  Hierarchies*. RT.
- Pantaleoni & Luebke (2010) — *HLBVH: Hierarchical LBVH Construction
  for Real-Time Ray Tracing*. HPG.
- Meister & Bittner (2018) — *Parallel Locally-Ordered Clustering for
  Bounding Volume Hierarchy Construction*. TVCG.
- Catto (2019) — *Dynamic Bounding Volume Hierarchies*. GDC. (Box2D
  v3 reference.)
- Ulrich (2000) — *Loose Octrees*. Game Programming Gems.
- Beckmann et al. (1990) — *The R\*-tree: An Efficient and Robust
  Access Method for Points and Rectangles*. SIGMOD.
- Teschner et al. (2003) — *Optimized Spatial Hashing for Collision
  Detection of Deformable Objects*. VMV.

### 12.3 Papers — convex distance / intersection

- Gilbert, Johnson & Keerthi (1988) — *A fast procedure for computing
  the distance between complex objects in three-dimensional space*.
  IEEE J. Robotics & Automation. (GJK.)
- van den Bergen (1999) — *A Fast and Robust GJK Implementation for
  Collision Detection of Convex Objects*. JGT.
- van den Bergen (2001) — *Proximity Queries and Penetration Depth
  Computation on 3D Game Objects*. GDC. (EPA.)
- Snethen (2008) — *XenoCollide: Complex Collision Made Simple*. Game
  Programming Gems 7. (MPR.)
- Cameron (1997) — *Enhancing GJK: Computing Minimum and Penetration
  Distances Between Convex Polyhedra*. ICRA.
- Catto (2010) — *Computing Distance (GJK)*. GDC.
- Möller (1997) — *A Fast Triangle-Triangle Intersection Test*. JGT.
- Möller & Trumbore (1997) — *Fast, Minimum Storage Ray-Triangle
  Intersection*. JGT.
- Akenine-Möller (2001) — *Fast 3D Triangle-Box Overlap Testing*. JGT.
- Williams, Barrus, Morley & Shirley (2005) — *An Efficient and Robust
  Ray-Box Intersection Algorithm*. JGT 10(1). (Precomputed
  `inv_dir` + sign-mask slab test; the v0f branchless ray-AABB.)
- Woop, Benthin & Wald (2013) — *Watertight Ray/Triangle Intersection*.
  JCGT 2(1). (Embree's default ray-tri; shared-edge sign consistency.)
- Ize (2013) — *Robust BVH Ray Traversal*. JCGT 2(2). (Boundary-
  consistent BVH traversal; partner to Woop watertight tri.)
- Baldwin & Weber (2016) — *Fast Ray-Triangle Intersections by
  Coordinate Transformation*. JCGT 5(3). (Precomputed unit-triangle
  transform; the cooked-static-mesh ray-tri default.)
- Erickson, J. (1997) — *Plücker Coordinates* (Ray Tracing News
  10(3)) — sign-only line/segment/ray-vs-triangle edge classification.
- Barnes, T. ("Tavianator", 2011 / 2015 / 2022 revisions) — *Fast,
  Branchless Ray/Bounding Box Intersections* — the de-facto NaN-safe
  `tmin/tmax` slab formulation (equivalent to Williams 2005 with IEEE
  min/max ordering).

### 12.4 Papers — convex hull

- Barber, Dobkin & Huhdanpaa (1996) — *The Quickhull Algorithm for
  Convex Hulls*. ACM TOMS. (qhull source.)
- Chan (1996) — *Optimal output-sensitive convex hull algorithms in
  two and three dimensions*. Discrete & Computational Geometry.

### 12.5 Papers — mesh queries

- Jacobson, Kavan & Sorkine-Hornung (2013) — *Robust inside-outside
  segmentation using generalized winding numbers*. SIGGRAPH.
- Bærentzen & Aanaes (2005) — *Signed distance computation using the
  angle weighted pseudo-normal*. IEEE TVCG.
- Hormann & Agathos (2001) — *The point in polygon problem for
  arbitrary polygons*. CGTA.

### 12.6 Papers — polygon ops

- Vatti (1992) — *A generic solution to polygon clipping*.
  Communications of the ACM.
- Sutherland & Hodgman (1974) — *Reentrant Polygon Clipping*. CACM.
- Greiner & Hormann (1998) — *Efficient Clipping of Arbitrary
  Polygons*. ACM TOG.
- Bentley & Ottmann (1979) — *Algorithms for reporting and counting
  geometric intersections*. IEEE TC.
- Foster, Hormann & Popa (2019) — *Clipping Simple Polygons with
  Degenerate Intersections*. CAG.

### 12.7 Papers — Delaunay / Voronoi

- Bowyer (1981) — *Computing Dirichlet tessellations*. Computer Journal.
- Watson (1981) — *Computing the n-dimensional Delaunay tessellation
  with application to Voronoi polytopes*. Computer Journal.
- Lawson (1972) — *Transforming triangulations*. Discrete Mathematics.
- Si (2015) — *TetGen, a Delaunay-Based Quality Tetrahedral Mesh
  Generator*. ACM TOMS.
- Hu et al. (2020) — *Fast Tetrahedral Meshing in the Wild*. SIGGRAPH.
- Lévy (2010) — *Variational Anisotropic Surface Meshing with Voronoi
  Parallel Linear Enumeration*. IMR.
- Fortune (1987) — *A sweepline algorithm for Voronoi diagrams*.
  SoCG.

### 12.8 Papers — mesh processing

- Garland & Heckbert (1997) — *Surface Simplification Using Quadric
  Error Metrics*. SIGGRAPH. **The reference for QEM.**
- Loop (1987) — *Smooth subdivision surfaces based on triangles*.
  M.S. thesis, University of Utah.
- Catmull & Clark (1978) — *Recursively generated B-spline surfaces
  on arbitrary topological meshes*. CAD.
- Botsch & Kobbelt (2004) — *A remeshing approach to multiresolution
  modeling*. SGP.
- Lévy et al. (2002) — *Least Squares Conformal Maps for Automatic
  Texture Atlas Generation*. SIGGRAPH.
- Liu et al. (2008) — *A local/global approach to mesh parameterization*.
  SGP. (ARAP.)
- Liepa (2003) — *Filling holes in meshes*. SGP.
- Taubin (1995) — *A signal processing approach to fair surface
  design*. SIGGRAPH.
- Fleishman, Drori & Cohen-Or (2003) — *Bilateral Mesh Denoising*.
  SIGGRAPH.
- Pauly, Mitra & Guibas (2003) — *Multi-scale Feature Extraction on
  Point-Sampled Surfaces*. EG.
- Attene (2010) — *A lightweight approach to repairing digitized
  polygon meshes*. The Visual Computer. (MeshFix.)

### 12.9 Papers — convex decomposition

- Mamou & Ghorbel (2010) — *A simple and efficient approach for 3D
  mesh approximate convex decomposition*. ICIP. (HACD.)
- Mamou (2014) — *Volumetric Hierarchical Approximate Convex
  Decomposition*. (V-HACD.)
- Wei, Liu, Shen, Wang (2022) — *Approximate Convex Decomposition
  for 3D Meshes with Collision-Aware Concavity and Tree Search*.
  SIGGRAPH. (CoACD.)

### 12.10 Papers — robust predicates

- Shewchuk (1997) — *Adaptive Precision Floating-Point Arithmetic and
  Fast Robust Geometric Predicates*. Discrete & Computational
  Geometry. **The single reference for robust predicates.**

### 12.11 Software (algorithm references, not source)

- **CGAL** — INRIA / MPI / ETH / Tel Aviv, computational-geometry
  algorithms library. The canonical reference. cgal.org.
- **Embree** — Intel CPU ray tracing. github.com/embree/embree.
- **Geogram** — Bruno Lévy / INRIA. brunolevy.github.io/geogram.
- **libigl** — Daniele Panozzo / Alec Jacobson, Eigen-based geometry.
  libigl.github.io.
- **OpenMesh** — RWTH Aachen, half-edge data structure.
  openmesh.org.
- **PMP** — Sieger & Botsch, polygon mesh processing.
  pmp-library.org.
- **MeshLab** — VCG library. meshlab.net.
- **Geometric Tools (GTE)** — David Eberly. geometrictools.com.
- **qhull** — Barber 1996. qhull.org.
- **Clipper2** — Angus Johnson. github.com/AngusJohnson/Clipper2.
- **V-HACD** — Khaled Mamou. github.com/kmammou/v-hacd.
- **TetGen** — Hang Si. wias-berlin.de/software/tetgen.
- **fTetWild** — Hu et al. github.com/wildmeshing/fTetWild.
- **MeshFix** — Marco Attene. github.com/MarcoAttene/MeshFix-V2.1.
- **Bullet** — Erwin Coumans. bulletphysics.org.
- **PhysX** — NVIDIA. github.com/NVIDIA-Omniverse/PhysX.
- **Jolt** — Jorrit Rouwé. github.com/jrouwe/JoltPhysics.
- **Box2D v3** — Erin Catto. box2d.org.
- **FCL** — Pan, Chitta, Manocha. github.com/flexible-collision-library/fcl.
- **HPP-FCL** — humanoid-path-planner fork. github.com/humanoid-path-planner/hpp-fcl.
- **OMPL** — Şucan & Moll. ompl.kavrakilab.org.
- **Boost.Geometry** — Barend Gehrels. boost.org/libs/geometry.

### 12.12 Cerid-internal cross-references

- ADR-0033 — `crd-jobs` (substrate for parallel BVH build / mesh
  processing).
- ADR-0053 — `IComponentIndex` reserved shells (`SpatialBVHIndex`,
  `SpatialHashIndex`, `SpatialOctreeIndex`).
- ADR-0061 — Async GPU upload contract (reused for `crd-geometry-gpu`).
- ADR-0062 — Eylem physics architecture (sibling substrate; same
  posture).
- ADR-0063 — Eylem determinism contract (inherited wholesale).
- ADR-0064 — `crd-sdf` substrate (sibling substrate; consumer of
  `crd-geometry-bvh` + `crd-geometry-mesh`).
- ADR-0065 — `crd-hesap` numerical computing substrate (sibling
  substrate; same posture).
- ADR-0073 — `crd-eylem-aero` reserved (consumer).
- ADR-0074 — `crd-eylem-cine` reserved (consumer).
- `docs/research/cerid-eylem.md` — Phase 3.1 physics research dossier.
- `docs/research/cerid-sdf.md` — Phase 3.1.5 implicit-surface research.
- `docs/research/cerid-hesap.md` — Phase 3.1.6 numerical-substrate
  research.

---

## 13. Addendum 2026-05-11 — v0f cutting-edge / branchless / SIMD intersection corpus

**Context.** The base dossier (§4.13) catalogs the curated Ericson /
Eberly "GTE" intersection set — solid, but it is the 2005-era state of
the art. The user asked for the cutting-edge bar raised explicitly:
"lots of cutting edge and branchless and fast (possibly using our SIMD
operations) intersection functions". This addendum is the research
record behind the **v0f sub-slice** (added to the slice plan + ADR-0076
§13 the same day) and behind the **`crd::math::geometry` move-and-delete**
(ADR-0076 §13.1).

### 13.1 `crd::math::geometry` move-and-delete

`engine/math/include/crd/math/geometry.hpp` already ships `Ray<T>` /
`Plane<T>` / `Sphere<T>` / `AABB<T>` / `Triangle<T>` / `Frustum<T>`
+ ~16 helpers (`closest_point` / `intersects` / `contains` /
`signed_distance` / `intersect_ray_plane` / `intersect_ray_sphere` /
`intersect_ray_triangle` / `intersects(Frustum, AABB|Sphere)`),
consumed by ~9 files. `crd-geometry-primitives` v0a would re-declare
the same names. Three options were considered:

| Option | Up-front cost | Long-term cost |
|---|---|---|
| (a) **Move-and-delete** | ~1 day refactor across math + scene + tests | none — one source of truth |
| (b) Thin `using`-alias shim in `crd-math/geometry.hpp` | ~0 (one include hop) | a perpetual indirection layer; the shim never naturally dies |
| (c) Coexist (geometry strictly additive) | ~0 | two `AABB`/`Sphere`/etc. definitions that drift over a decade; the exact failure mode §2.3 warns about for *external* libs, internalised |

**Decision: (a) move-and-delete.** The leaf math module's design intent
(§2.2) is Vec/Mat/Quat/Transform/SIMD/`deterministic` — geometric
primitives over those types are the next tier up, which is precisely
why `crd-geometry` is a separate module (§2). Keeping a half-copy in
`crd-math` would re-introduce the scope conflation the module split
exists to prevent. The ~1-day refactor is the smallest total cost of
the three. `crd-math`'s scalar `intersect_ray_triangle` (Möller-Trumbore)
becomes the v0f cross-check reference before it is deleted.

### 13.2 The cutting-edge corpus — algorithm-by-algorithm

Each entry is the *why* behind a v0f deliverable. Format mirrors §4
(chosen / reference / disposition).

#### Ray–triangle

| Algorithm | Year | Property | Disposition |
|---|---|---|---|
| **Möller-Trumbore** | 1997 | minimal storage; fast; **not** edge-consistent (a ray can slip between two triangles sharing an edge) | already in §4.13 — kept as the general-purpose scalar default + the v0f cross-check reference |
| **Watertight (Woop / Benthin / Wald)** | 2013 | shear+scale ray transform → edge-function form; **edge-consistent** — for two triangles sharing an edge a ray hits both or the consistent one, never neither; ~1.3× M-T cost | **CHOSEN — v0f.** Embree's default. Becomes the default ray-tri for `crd-geometry-mesh` v4d BVH leaves and `crd-sdf` v2 mesh-bake (correctness on imperfect meshes — narrows the §11.1 robust-predicates open question to the non-ray-tri cases). |
| **Baldwin-Weber** | 2016 | precomputed 3×4 unit-triangle affine transform per tri; per-ray test 9 mul + 6 add, branchless; ~0.8× M-T on read-mostly meshes; +48 B/tri | **CHOSEN — v0f.** Opt-in default for cooked static meshes (`MeshResource` BVH, static colliders) where the storage pays off. |
| Plücker ray-triangle | classical | sign-only edge tests via Plücker side-operator; fully branchless; no division until a hit is confirmed | **CHOSEN — v0f** (as the edge-classification primitive; the "which side / does it cross" form, used by GJK / clipping too). Reference: Erickson 1997 *Plücker Coordinates*; Ericson RTCD §5.3; Eberly GTE. |
| Segura-Feito | 2001 | predicate-only ray-tri (no intersection point) | Reserved — Plücker covers the same need. |

#### Ray–AABB

| Algorithm | Property | Disposition |
|---|---|---|
| Naive 6-plane slab with branches | conditional per axis; branch-mispredict-prone | Rejected for the hot path. |
| **Williams 2005 + Tavianator branchless slab** | precompute `inv_dir` (3 reciprocals once) + sign mask; per-test `tmin = max(min(t0,t1)...)`, `tmax = min(max(t0,t1)...)` with IEEE min/max ordering so a NaN from `0·∞` resolves the "right" way; zero hot-path conditionals; ~2 ns scalar | **CHOSEN — v0f.** The de-facto industry-standard ray-box; what every modern BVH traverser uses. |
| `Vec4f` ray-vs-4-AABB | one min/max chain over 4 boxes' interleaved planes | **CHOSEN — v0f** (the scalar core for BVH4 traversal — `-bvh` v1g consumes it). |
| `Vec8f` ray-vs-8-AABB | AVX2 8-wide | **CHOSEN — v0f** (for the wide-BVH path). |

#### Robust BVH traversal precompute

| Algorithm | Property | Disposition |
|---|---|---|
| **Ize 2013 — Robust BVH Ray Traversal** | the boundary-consistent comparison constants + the precomputed-`inv_dir`/sign-flag `RayPacket`; guarantees a ray whose origin lies exactly on a node-split plane is not lost | **CHOSEN — v0f** (ships the `RayPacket` type; `-bvh` v1g uses it). The partner to Woop watertight tri — together they make BVH ray queries hole-free. |
| Pharr-Humphreys (PBRT) ray-AABB with rounding-error bounds | analytic conservative epsilon on `tHit` | Reserved — relevant if a renderer consumer needs sub-pixel-precise primary visibility; physics doesn't. |

#### Closest-point-on-triangle

| Algorithm | Property | Disposition |
|---|---|---|
| Plane-project + clamp-to-edges | branchy; subtly wrong near vertices | Rejected. |
| **Ericson Voronoi-region** (RTCD §5.1.5) | classifies the query against the 7 Voronoi regions (3 vertex + 3 edge + 1 face) of the triangle with a fixed sequence of barycentric sign tests; near-branchless; the canonical physics form | **CHOSEN — v0b** (and the `Vec8f` batched variant in v0f for capsule-vs-mesh / EPA / GJK-fallback inner loops). |

#### SIMD batch kernels (v0f)

`Vec4f` / `Vec8f` wrappers over the scalar cores above, AoSoA storage
via `crd::math::simd::Soa`:

- `ray_vs_n_aabb` — broadphase prefilter + BVH leaf test.
- `ray_vs_n_triangle` — Woop (watertight) and Möller-Trumbore variants;
  `Vec8f` over 8 triangles per leaf (the v4g pattern, but the kernel
  itself lands here in v0f as the reusable primitive).
- `n_sphere_vs_n_sphere` — particle / fluid neighbour prefilter.
- `segment_vs_n_segment` closest-pair — the capsule-vs-mesh / capsule-
  vs-capsule-array inner loop.
- `aabb_vs_n_aabb` overlap mask — the per-node child-overlap test that
  the dynamic-tree query (`-bvh` v1) calls.

All batch reductions go through `crd::math::simd::reduce_*` pairwise
binary trees (ADR-0076 §4 pin 7) — never lane-order accumulation.

### 13.3 Reference catalogues / prior art for the corpus

- **Embree** (Intel) — the reference for what a production CPU BVH +
  primitive intersector ships: Woop 2013 watertight tri is its default;
  Ize 2013 robust traversal; BVH4/BVH8 SIMD ray-vs-N-AABB. Apache-2.0
  — algorithm reference, not source (we re-implement deterministically).
- **PBRT (Pharr, Jakob, Humphreys)** — *Physically Based Rendering*
  4th ed., Ch. 6 — the canonical exposition of robust ray-bounds and
  ray-triangle with explicit floating-point error analysis.
- **Tavian Barnes' "Fast, Branchless Ray/Bounding Box Intersections"**
  (tavianator.com, 2011 + 2015 + 2022 revisions) — the widely-cited
  derivation of the NaN-safe branchless slab test; the 2022 revision
  covers the SIMD packing.
- **JCGT (Journal of Computer Graphics Techniques)** — Woop 2013,
  Ize 2013, Baldwin-Weber 2016 all published here; the modern home of
  practical-intersection-algorithm papers.
- **Inigo Quilez's "intersectors"** (iquilezles.org) — analytic
  ray-vs-{sphere, ellipsoid, box, plane, disk, capsule, cylinder,
  capped-cylinder, torus, triangle, ...} — the branch-minimal closed
  forms; already pulled into v0e for the SDF-side primitives, the
  ray-intersector forms feed v0c + v0f.
- **Ericson, RTCD Ch. 5** — the curated physics-grade subset (the
  v0b/v0c anchor); §5.1.5 closest-point-on-triangle, §5.3 line/segment
  classification, §5.5 ray queries.
- **Eberly, GTE / "3D Game Engine Design" Ch. 14–17** — the exhaustive
  primitive-pair catalogue (the v0c anchor; ~70 pairs).

### 13.4 Determinism

v0f inherits the ADR-0063 contract wholesale; the corpus-specific
tiebreak pins are ADR-0076 §4 items 12 (watertight ray-tri axis
selection + closed on-edge tests) and 13 (Plücker fixed sum order +
sign-zero = "on the line"). The branchless slab test is naturally
deterministic — it is pure min/max/mul/sub with no compiler-reorderable
reductions and no transcendentals. SIMD batch kernels: pin 7 (pairwise-
tree reductions). Cross-platform byte-exact and the eylem v9b
replay-hash CI cover regressions (§5.8).

### 13.5 New references (added to §12.3 above)

Williams-Barrus-Morley-Shirley 2005 · Woop-Benthin-Wald 2013 · Ize
2013 · Baldwin-Weber 2016 · Erickson 1997 (Plücker) · Barnes
("Tavianator") — see §12.3.
