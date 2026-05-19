# Cerid Engine — Roadmap Hub

> **Cerid is a general-purpose C++20 real-time engine substrate.** Games are
> one consumer; simulation (incl. robotics), medical visualization,
> DAW-class creative tools, and offline cinematic pipelines are equal-class
> consumers.
>
> **This is a hub.** Don't read it end-to-end. Follow the link relevant to
> your task. The detailed plans live in `docs/phases/`, the architectural
> decisions live in `docs/decisions/`, and the live state lives in
> `context.md`.

---

## Core docs (read every session)

- **`AGENTS.md`** (project root) — project rules, agent roster, coding standards
- **`docs/PRINCIPLES.md`** — engineering principles + pinned cornerstones
- **`context.md`** (project root) — live "where we are now" state

## Status (snapshot)

| Phase | State        | Detail                                       |
| ----- | ------------ | -------------------------------------------- |
| 1.0   Foundations               | ✅ shipped       | `docs/phases/phase-1-foundations.md`     |
| 1.5   Application skeleton      | ✅ shipped       | `docs/phases/phase-1.5-app.md`           |
| 1.6   Configuration substrate   | 🚧 active        | `docs/phases/phase-1.6-config.md` (1.6a shipped; consumers next) |
| 2.0   RHI + Vulkan + triangle   | ✅ shipped (a–d) | `docs/phases/phase-2-graphics.md`        |
| 2.1   ImGui debug overlay       | ✅ shipped       | `docs/phases/phase-2-graphics.md`        |
| 2.2   GPU memory + streaming    | ✅ shipped       | `docs/phases/phase-2-graphics.md`        |
| 2.3   Shader system             | ✅ shipped (a–g) | `docs/phases/phase-2.3-shader.md`        |
| 2.4   Renderer v1               | 🚧 active        | `docs/phases/phase-2-graphics.md` (v1a–i shipped; v1j = GPU instancing, Phase 3.2 dep); material system v1 gaps documented in `docs/debt.md` |
| 2.5   Jobs (threads + fibers)   | ✅ shipped       | `docs/phases/phase-2.5-jobs.md` (v1a–v1k all shipped; ADR-0033) |
| 2.6   Resources + asset cooker  | ✅ shipped       | `docs/phases/phase-2.6-resources.md` (v1a–v1g COMPLETE 2026-05-04; ADRs 0036–0041) |
| 2.7   Asset import bootstrap    | ⏳               | `docs/phases/phase-2.7-asset-import.md` (TextureResource + MeshResource + glTF + **full material foundation** (ADR-0048): MaterialTemplate/Instance, ParameterType, ShaderOptions, SurfaceData contract, new MATR format + GPU upload + **crd-meshgen** + **crd-sandbox** bootstrap; ADRs 0042–0043, 0045, 0048) |
| 2.8   Material completion       | ✅ shipped       | `docs/phases/phase-2.8-material-completion.md` (v1a–v1g all complete 2026-05-06; ADRs 0044, 0046, 0048) |
| 3.0   Scene / ECS foundation    | ✅ shipped 2026-05-10 (v1a–v1p, all 17 slices) | `docs/phases/phase-3.0-scene-ecs.md` (8-layer slot architecture; ADRs 0049–0061 all realised; 856 unit tests across 12 build configs) |
| 3.1   **Eylem (Cerid-native physics)** | 🚧 v1b in-flight; **⏸ PAUSES at v1b close — Phase 3.1.7 executes next, then v1c resumes** (v0 ✅ + v1a ✅ + v1a-draw d0..d4 ✅ + v1b-a/b/c/d ✅ + v1a-material a/b/c/d ✅ + v1b-e 🚧 sweep running 2026-05-11) | `docs/phases/phase-3.1-eylem.md` (~50 sub-slices over v0-v9; **pause-marker between v1b and v1c per ADR-0076 §12 amendment 2026-05-11**; v0 = `crd-math` SIMD substrate ✅; v1 = rigid 3D substrate + crd-draw + Material substrate + 5-tier collision filter + deferred ECS event-stream callbacks + force-field substrate + conservation-law CI; v2 = rigid 2D specialisation; v3 = XPBD soft/cloth/rope; v4 = maximal-coord articulations + cinematic bridge; v5 = vehicles + 5 deferred friction models; v6 = CCD + Featherstone + URDF/SDF/MJCF importers + nonsmooth Newton + sensors + aerospace substrate; v7 = FEM + HuntCrossley; v8 = GPU acceleration + Newton restitution; v9 = differentiable + cross-engine bench + drift CI; ADRs 0062 + 0063 + 0066 + 0067 + 0068 + 0069 + 0075 all Accepted; ADRs 0070 + 0071 + 0072 + 0073 + 0074 reserved Planned; supersedes ADR-0018 + Phase 6 native physics) |
| 3.1.7 **crd-geometry substrate** | 🚧 **in-flight** (as of 2026-05-19: **11 of 11 sub-modules COMPLETE** — `crd-geometry` substrate done; v9 cluster CLOSED via §24+§25+§26; remaining v10 `-curves` + v11 transform-aware ~2.5 wk to phase-fully-closes; see `docs/phases/phase-3.1.7-geometry.md` for live state). Historical scope: v0 sub-phase ✅ (v0a–v0f); `-bvh` sub-module ✅ (v1a–v1g); `-primitives` v1h ✅; v1i unified query facade ✅ (v1i-a/b/c); `-viz` v1j ✅; **`-convex` v2 ✅ COMPLETE 2026-05-14** (v2a–v2j + v2-close, all 11 slices); **v3 ✅ shipped 2026-05-14/15** (Shewchuk adaptive predicates + 2D convex hull + 3D Quickhull + hull simplification + v3-close); **`-mesh` v4 ✅ CLOSED 2026-05-16** (v4a closest_point Ericson + v4b raycast Woop watertight + v4c winding_number Jacobson 2013 + v4d raycast_simd MT 8-wide + v4-validate 6-defect-class formal validation + v4-close: 5 slices / 39 cases / 503 assertions; ADR-0076 §19 amendment 9 locked decisions; `docs/systems/geometry-mesh.md`; 18-config sweep PASS); **6th of 11 sub-modules COMPLETE; v5 `-spatial` CLOSED 2026-05-16 (8 slices: v5a + v5b + v5c + v5d + v5e + v5 thread-safety validation + v5-index-bringup + v5-queries-extension + v5-close — ADR-0076 §20 amendment 20 locked substrate decisions; `docs/systems/geometry-spatial.md`; 18-config sweep PASS; ~4900 LOC engine + ~4330 LOC tests + cross-backend ASan race coverage 2000 fan-out tasks; facade now over 8 backend types); 45 of 49 renewed-scope slices shipped (~92%). **v6 `-polygon` CLOSED 2026-05-16 — all 6 slices shipped** (v6a substrate + v6b ear-clipping w/ holes + v6c CDT + v6d Vatti polygon Boolean + v6e Bentley-Ottmann + v6-close); 92 cases / 695 assertions; ~4350 LOC engine + ~2750 LOC tests; 7th of 11 sub-modules COMPLETE. **🚧 v7 `-mesh-processing` IN PROGRESS** (9-slice plan: v7a HalfEdgeMesh substrate → v7b Garland-Heckbert QEM → v7c Loop subdivision → v7d Botsch-Kobbelt isotropic remesh → v7e Liepa hole filling → v7f manifoldness repair → v7g self-intersection removal consuming v6e Bentley-Ottmann → v7h Taubin volume-preserving smoothing → v7-close ADR §22 amendment; v7 expanded 7→9 per substrate separation; nothing cut). **v5 `-spatial` cluster IN PROGRESS** (7 sub-slices, ~13-16 days, ~3.6 KLOC engine + ~1.5 KLOC tests; locked plan 2026-05-16): **v5a KD-tree ✅ 2026-05-16** (new `engine/geometry-spatial/`; widest-extent split + leaf=8 + lex-tuple `crd::containers::nth_element` + caller-heap k-NN with strict-`>` pruning + explicit-`k` API; 24 cases brute-force cross-validated; 5-config DoD PASS) → **v5b LooseOctree ✅ 2026-05-16** (Ulrich 2000 dynamic AABB index; 64 B node + free-list pool with stable `OctreeObjectId`; loose factor 2.0; **fast-path AABB-fit invariant 100% verified by sized-by-construction test**; Morton-order overlap + t-near-first raycast; 17 cases / 5-config DoD PASS) → **v5c R*-tree ✅ 2026-05-16** (FULL Beckmann 1990 — NOT quadratic-split simplification — split + forced reinsertion + Guttman condense-tree + STR bulk-load first-class + Hjaltason-Samet 1999 incremental k-NN + indirection-table stable handles; 17 cases / 5-config DoD PASS) → **v5d SpatialHash ✅ 2026-05-16** (Teschner 2003 hash + Amanatides-Woo 1987 voxel raycast + `find_overlapping_pairs` broadphase pair query + **per-query generation-counter dedup (zero allocation)** + same-cell-range update fast-path; **v5d-fast caller-scratch overload shipped same session per "elite, no deferring" mandate** — `SpatialHashScratch` POD + scratch-taking overloads + concurrent-query test via `crd::jobs::parallel_for` 4-worker fiber pool with per-task isolated scratch + ASan race-clean; 28 cases / 5-config DoD PASS) → **v5e UniformGrid ✅ 2026-05-16** (dense bounded-domain 3D cell array; flat `Array<Array<u32>>` sized `nx·ny·nz`; out-of-bounds AABBs CLAMP to grid; **grid-bounds-clipped Amanatides-Woo raycast** — slab-clip ray vs grid AABB then walk; **scratch overloads + crd-jobs concurrent test SHIPPED FROM DAY 1** per "elite, no deferring" mandate — `UniformGridScratch` POD with per-scratch generation counter, ASan race-clean across 400 fan-out tasks; 26 cases / 5-config DoD PASS) → **v5-index-bringup ✅ 2026-05-16** (`SpatialBVHIndex` promoted from ADR-0053 no-op shell to real LooseOctree-backed component index in `crd-scene` — FIRST non-reserved spatial index; two-state design preserves day-one promise; canonical `IAabbExtractor(EntityId, ComponentId, data)` pattern avoids storage-migration-mid-commit hazard; UPSERT-only update contract; concurrent-via-`crd::jobs` ASan-clean; 8 cases / 5-config DoD PASS) → **v5-queries-extension ✅ 2026-05-16** (unified `crd::geometry::{raycast/overlap/radius/nearest_n/find_overlapping_pairs}` facade extended over all 5 v5 backends; header-only `crd/geometry/spatial/queries.hpp`; compile-time overload polymorphism per ADR-0076 §16 pin #1; support matrix locked — don't fake what doesn't fit; 16 facade-parity tests + cross-backend uniformity demo; 5-config DoD PASS; session log `2026-05-16-geometry-v5-queries-extension.md`) → **🎯 v5e NEXT** Dense UniformGrid (Ulrich 2000 dynamic AABB index, workhorse for scene culling) → v5c R-tree (Beckmann 1990 R*-tree, may split v5c-a/b) → v5d Spatial hash (Teschner 2003 particle broadphase) → v5e Dense UniformGrid (3D cell array bounded domain) → v5-index-bringup (`scene::SpatialBvhIndex` reserved-shell from ADR-0053 realized, default backend = LooseOctree, IComponentIndex hooks) → v5-queries-extension (compile-time `queries.hpp` facade dispatch) → v5-close (ADR-0076 §20 amendment + `docs/systems/geometry-spatial.md` + 18-config sweep). Sandbox SpatialQueries viz deferred to focused phase-segment session. **Renewed scope 2026-05-14 (in-phase additions, see phase doc "Renewed scope coverage" section):** +1 `v4-validate` formal mesh-validation slice inside v4 cluster + 5 `v10a–e` new `-curves` sub-module (Bezier / Hermite / Catmull-Rom / B-spline / arcs + uniform/adaptive/curvature sampling + arc-length system + closest-point + Frenet + RMF frames) + 1 `v11` transform-aware query helpers in `crd-geometry-primitives` (`TransformedShape<T>` + `transform_aabb/obb/capsule3/cylinder3` + `transform_ray3_to_local`); **Units/scale/precision question RESOLVED at engine-wide scope by Phase 3.1.7.5 `crd-units` (locked 2026-05-14)** — SI everywhere internally + compile-time-dimensional types at every API surface; the geometry-local ADR collapses to a precision-tier + epsilon-policy ADR only. 3 remaining ADR candidates (transform-aware adapter pattern, curve mathematics ownership, mesh validation pipeline stage) — minted at slice time, not eagerly. **PCB/EDA elevated to new `crd-eda` future-major phase stub** (see Multi-domain expansion section + new row below). ADR-0076 §12 amendment 2026-05-11 pivoted the slot to before 3.1 v1c so eylem consumes geometry from day 1. | `docs/phases/phase-3.1.7-geometry.md` (13 sub-modules / **49 slices** / **~22 KLOC engine + ~5 KLOC editor + ~4 KLOC cooker-emitted GLSL/HLSL / ~7.5–9.5 months** post-renewal: v0 primitives [v0a absorbs+deletes the pre-existing `crd::math::geometry` per ADR-0076 §13 — `crd-math` then keeps only Vec/Mat/Quat/Transform/SIMD/deterministic] + v0e iq-formulary substrate (smin/domain ops + `crd::math::simd::reduce_argmax_with_lex_tiebreak` substrate primitive) + v0f cutting-edge/branchless/SIMD intersection corpus (watertight ray-tri Woop 2013 + Baldwin-Weber 2016 + branchless NaN-safe slab ray-AABB + Ize 2013 robust-traversal precompute + Plücker edge tests + Ericson Voronoi-region closest-point + `Vec4f`/`Vec8f` batch kernels + ULP-conformance tests) → v1 BVH (binned SAH + Catto 2019 refit + quad-BVH) + v1g BVH4 SIMD ray-vs-4-AABB → v2 GJK+EPA+SAT → v3 Quickhull → v4 mesh queries (closest-point + Möller-Trumbore + Jacobson 2013 winding number) + v4g per-leaf SIMD Möller-Trumbore over 8 triangles → v5 KD-tree+octree+R-tree+spatial hash + scene IComponentIndex bring-up → v6 polygon ops (Vatti + CDT + Bentley-Ottmann) → v7 mesh processing (QEM + Loop subd + remesh + repair + Taubin) → v8 Delaunay + Voronoi 2D/3D → v9 GPU LBVH (Karras 2012) + V-HACD editor-tier + REPL + v9e GLSL/HLSL `crd-geometry-shader-helpers` cooker-emitted output (ULP-conformance-tested against C++ ref) → **v10 `-curves` sub-module** (NEW renewed-scope addition 2026-05-14: Bezier / Hermite / Catmull-Rom / B-spline / circular & elliptic arcs + 2D peers + sampling/flattening/curvature + arc-length system + closest-point + Frenet/RMF frames + viz) → **v11 transform-aware helpers** (NEW renewed-scope addition 2026-05-14: `TransformedShape<T>` + `transform_aabb/obb/capsule3/cylinder3/ray3_to_local` in `crd-geometry-primitives`; `crd-geometry` stays local-space pure; world-space dispatch is consumer concern); ADR-0076 (Accepted 2026-05-11; §12 + §13 amendments 2026-05-11; §15 checklist additions 2026-05-13; renewed-scope additions 2026-05-14) + research: `docs/research/cerid-geometry.md` (incl. §13 addendum) + `docs/research/cerid-geometry-supplement.md`; deferred-refactor pattern OBSOLETED by §12 — eylem v1c/v1d + sdf v2 consume from day 1) |
| 3.1.7.5 **crd-units** | ✅ **CLOSED 2026-05-15** — all 4 adoption arcs shipped same day (v0a substrate + v0b adoption A + v0c adoption B + v0d adoption C). 4.5 KLOC engine + 2.3 KLOC tests across 19 sub-slices. ADR-0078 §1-§4 (31 locked design decisions). Full project ctest 1546 → 1913 win-debug (+367 cases across phase). Net: every physical/scientific quantity at every API boundary carries a compile-time dimension tag — Mars Climate Orbiter at the engine surface is now a compile error. **Highlights:** v0a substrate + 6-layer conversion system + 138 cases / 464 assertions + `crd-no-untagged-physical-numeric` CI guard. **v0b** Vec<Quantity> enablement + crd-config 13 TOML accessors + scene::Transform = Vec3<Length32> + glTF [cook] position_scale. **v0c** crd-eylem RigidBody/PhysicsConfig/IPhysicsScene dimensional with 80-byte freeze pin preserved + integrator typed-math via cross-Dim Vec*Q overloads + ForceFieldComponent geometric params typed. **v0d** Vec reductions widened (length/dot/cross/distance/normalized) without DimRoot via DimMul-then-retag + crd-geometry-primitives struct widening + queries_typed.hpp boundary-wrapper layer (closest_point/distance/distance_squared typed overloads for 8 primitives) + crd-renderer raw-Mat4f boundary contract pinned + crd-resources byte-buffer contract pinned + Layer-6 UnitPreferences format/parse with 11 discipline presets (game/CAD/robotics/aerospace/PCB/audio/3D-print/CAM/cinematic/imperial/SI-strict/scientific) + crd-imgui inspector. Phase close session log `docs/sessions/2026-05-15-units-v0d-phase-close.md`. **Open follow-ups (not blockers):** DimRoot<> for unknown-Dim sqrt (deferred until consumer surfaces); Mat<Quantity> (deferred); federated unit packs (Layer-5 ready, first consumer = crd-eylem-aero in Phase 3.1 v6). | `docs/phases/phase-3.1.7.5-units.md` — **compile-time-dimensional units substrate + 6-layer conversion system**, the architectural answer to "every physical and scientific quantity always carries a unit, no matter what" (user 2026-05-14, pinned in `docs/PRINCIPLES.md`). Zero-overhead `Quantity<D, T>` wrapper + 7 SI base + Angle as 8th tagged dimension + ~50 named derived quantities + ~120 user-defined literals + `Vec3<Quantity>`/`Mat<Quantity>` wrappers + `.value_in<TargetUnit>()` (compile-time) / `.value_in(UnitTag)` (runtime, UI). **6-layer conversion system**: (1) `LinearUnit<Dim, std::ratio>` — exact rational factors, bit-exact SI-prefix + standardised-imperial round-trips; (2) `AffineUnit` + distinct `Temperature`/`TemperatureDelta` types (absolute-vs-delta trap closed at compile time); (3) `NonLinearUnit` for dB / cents / magnitude / Richter / pH with arithmetic-disabled marker (compile-time block on `dB + dB`); (4) `UnitMul`/`UnitDiv` compound auto-derivation via `std::ratio_multiply`/`std::ratio_divide` (one new base unit unlocks N compound units automatically — the extensibility multiplier); (5) federated domain registration — `crd-eylem-aero` adds AU/ly/parsec/SolarMass, `crd-eda` adds Mil/OhmCm/AmpHour, `crd-cam` adds RPM/IPM/SFM, etc., each in their own `units` sub-namespace with ADL lookup, no central registry; (6) format + parse + `UnitPreferences` 11-discipline-preset table (game / CAD / robotics / aerospace / PCB / audio / 3D-print / CAM / cinematic / imperial / SI-strict / scientific). **Slot: between Phase 3.1.7 close and eylem v1c resume** — eylem v1c+ and every downstream module consumes dimensional types from day 1, no per-domain unit-system divergence ever. 4 slices over ~4.5 weeks (~3.5 KLOC engine + ~2.4 KLOC tests): v0a substrate + full 6-layer conversion system (~1.6 KLOC) → v0b adoption A (`crd-config` + `crd-scene Transform` + glTF cooker, ~600 LOC) → v0c adoption B (`crd-eylem RigidBody` + integrator + force fields + `crd-geometry` API-surface re-tag, ~800 LOC) → v0d adoption C (`crd-renderer` + cookers + ImGui + Layer-6 format/parse/UnitPreferences + cross-engine readers + 17-config sweep close, ~700 LOC). **Performance pin**: Layer 1/4 conversion = 1 FP multiply; Layer 2 = multiply+add; Layer 3 = pow/log at boundary only; `Quantity` arithmetic produces identical codegen to bare-scalar (objdump-verified in v0a). **Extensibility pin**: adding a new base unit = 1 line; adding a domain pack = 1 header file; adding a new conversion class = ~50 LOC. **Frame transforms (ENU/NED/ECEF, body-vs-inertial, world-vs-local) are NOT in `crd-units` scope** — they live in `crd-math::Transform` + `crd-geometry::transform_aabb`. ADR-0078 minted at v0a close; PRINCIPLES.md cornerstone added at v0d close. **`crd-no-untagged-physical-numeric` CI guard** added — bare-`f32` for length/mass/time/force/etc. fields is a build failure across `engine/` (minus `crd-math/src/simd/` + `crd-rhi-vulkan/` where raw scalars are intentional). |
| 3.1.8 **crd-brep (NURBS / B-rep)** | 📋 planned (ADR-0077) | `docs/phases/phase-3.1.8-brep.md` (parametric surfaces, B-rep solid topology, exact booleans, fillet/chamfer/sweep/loft, STEP / IGES / Parasolid import — the manufacturing/CAD substrate). |
| 3.1.9 **crd-cad-feature**        | 📋 planned (ADR-0077) | `docs/phases/phase-3.1.9-cad-feature.md` (feature trees, 2D sketching with constraints, ISO/ASME drafting, dimensioning, section views, GD&T per ASME Y14.5). |
| 3.1.10 **crd-cfd**               | 📋 planned (ADR-0077) | `docs/phases/phase-3.1.10-cfd.md` (unstructured grid topology, FVM/FEM, compressible/incompressible Navier-Stokes, k-ε / k-ω SST / LES / RANS turbulence, multiphase VOF + level-set, heat transfer, AMR, combustion). |
| 3.1.11 **crd-estimation + crd-control** | 📋 planned (ADR-0077) | `docs/phases/phase-3.1.11-estimation-control.md` (Kalman / EKF / UKF / particle filter / SLAM substrate; PID / LQR / MPC / optimal trajectory / robust control; path planning RRT / RRT* / PRM / A*). |
| 3.1.12 **crd-fea**               | 📋 planned (ADR-0077) | `docs/phases/phase-3.1.12-fea.md` (engineering FEA: static structural, modal, buckling, fatigue; linear→plastic→hyperelastic material models; thermal-structural coupling — distinct from eylem v7 dynamic FEM). |
| 3.1.13 **crd-cam**               | 📋 planned (ADR-0077) | `docs/phases/phase-3.1.13-cam.md` (3- / 5-axis milling toolpaths, turning, additive slicing, G-code post-processors, material-removal simulation, sheet-metal, PCB stack-up). |
| 3.1.14 **crd-ml-inference**      | 📋 planned (ADR-0077) | `docs/phases/phase-3.1.14-ml-inference.md` (ONNX runtime, GPU inference via `crd-rhi`, differentiable bridge extending `crd-hesap-autodiff`, denoising/SR/content-generation/in-game-AI consumers). |
| 3.1.15 **crd-procgen**           | 📋 planned (ADR-0077) | `docs/phases/phase-3.1.15-procgen.md` (noise primitives, Wave Function Collapse, L-systems, terrain erosion sim, city/building generation, Substance-style procedural materials). |
| 3.1.16 **crd-sciviz**            | 📋 planned (ADR-0077) | `docs/phases/phase-3.1.16-sciviz.md` (isosurfaces, streamlines, slice planes, vector-field LIC, perceptual color maps, in-engine 2D/3D plots, annotation + measurement — engineering / FEA / CFD validation tool). |
| 3.1.17 **crd-eda**               | 📋 planned (renewed-scope 2026-05-14) | `docs/phases/phase-3.1.17-eda.md` (PCB / EDA geometry substrate — board outline polygon + holes/cutouts + layers + keepout zones; trace geometry (segments + arcs + vias + pads + copper zones); DRC clearance checks (trace-trace + pad-trace + via + board-edge + zone-fill + acute-sliver); routing helpers (orthogonal + 45° + obstacle-avoidance; shove + autorouter are much-later follow-ups); Gerber X2/X3 + drill + solder-mask + paste output for manufacturing fab; future panelization. Future-major phase stub elevated 2026-05-14 from the renewed-scope review of Phase 3.1.7 `crd-geometry` — surfaces the EDA ambition without committing scope. Slot: after `crd-cad-feature` (3.1.9) close — EDA reuses the parametric-sketch + drafting substrate. Reading: KiCad architecture, FreeRouting algorithm survey, IPC-7351 land-pattern standards, Gerber X2/X3 format spec.). |
| 3     Simulation + visual effects | ⏳             | `docs/phases/phase-3-simulation.md` (3.0 ✅ → 3.1 eylem → 3.2 animation (flesh out) → **3.3 crd-font** → 3.4 audio (flesh out incl. ray-traced acoustics) → **3.5** modern rendering prologue (mesh shaders / visibility buffer / GPU-driven culling / VRS / work graphs) + PBR+IBL+CSM+SSS+NPR+area lights → **3.6** sky atmosphere+volumetric fog+clouds+god rays+aurora → **3.7** bloom+GTAO+SSR+TAA+DoF+motion blur+upscaling → **3.8** GPU particles+ocean+decals+indirect rendering → **3.9** SSGI+DDGI+radiance cascades+lightmap baking; ADR-0047, ADR-0077 §4) |
| 4     Extensibility + Networking | ⏳              | `docs/phases/phase-4-extensibility.md` (4.0 C++ scripting, 4.1 advanced math, 4.2 networking; ADR-0034, ADR-0035) |
| 5     RT + UI + advanced rendering | ⏳            | `docs/phases/phase-5-ui-rendering.md` (HybridRenderPath: BLAS/TLAS, RT AO/reflections/shadows/GI, ML denoiser via `crd-ml-inference`; crd-ui; node editor; ADR-0046) |
| 6     Platform expansion         | 📋 planned (ADR-0077) | `docs/phases/phase-6-platform-expansion.md` (console support PS5/Xbox/Switch, mobile iOS/Android, web WebGPU, VR/AR OpenXR, HPC/MPI distributed simulation. Reuses the slot left by the "Native physics" folding into Phase 3.1.) |
| 7     Editor                    | ⏳               | `docs/phases/phase-7-editor.md`          |
| 8     Domain modules            | ⏳               | `docs/phases/phase-8-domain-modules.md` (application-layer integration: robotics (URDF+ROS2), aerospace (VLM + orbital prop), cinematic (mocap retarget + camera systems), medical viz (DICOM + transfer fn), manufacturing (full CAD/CAM/CAE), scientific computing (REPL+notebook); ADR-0077 §5) |

Legend: ✅ shipped · 🚧 active · ⏳ planned · ❌ blocked

## Strategic Execution Plan (locked 2026-05-15; **revised 2026-05-19 — agent-native pivot + hesap-elite + C++-only-scripting**)

After a step-back strategic review (Pathways A–E + cross-cuts) on
2026-05-15, the user locked the original execution sequence. On
2026-05-19, the user locked three additional load-bearing decisions
(see below). **This is the canonical near-to-medium-term plan.
Future sessions read this first when they don't know what to work on.**

### Revision 2026-05-19 (load-bearing user direction)

- **Agent-native engine** is now a load-bearing cornerstone (ADR-0081
  Proposed; PRINCIPLES.md updated). CLI / JSON-RPC / **Anthropic MCP
  (exact compatibility)** is the source of truth; GUI is a
  visualization layer that emits CLI commands. AI agents (Claude
  Code, Anthropic SDK, OpenAI / Gemini Function Calling) drive the
  engine end-to-end via the same surface. Research dossiers:
  `docs/research/cerid-agent-native-engine.md` +
  `docs/research/cerid-hesap-2026-update.md`.

- **C++ hot-reload is the ONLY scripting language.** Cerid scripts
  ARE C++ files (`.crds.cpp`) compiled into hot-reloadable DLLs.
  **No Lua / Python / GDScript / JavaScript embedded interpreter.**
  ADR-0034 (C++ DLL hot-reload) is now subsumed by ADR-0081 as the
  scripting-substrate sub-aspect of the broader agent-native vision.
  Locked 2026-05-19; revisiting requires a new ADR.

- **`crd-hesap` goes elite-and-big** (same precedent as Phase 3.1.7
  `crd-geometry`). Full SOTA scope from v0: matrix-type catalog
  (~30 types: dense / banded / triangular / Hermitian / Toeplitz /
  Hankel / circulant / Vandermonde / sparse {CSR/CSC/BSR/COO/ELL/HYB
  /DIA/CSR5/Merge-CSR} / hierarchical {HSS/H-matrix/BLR}),
  **complex-number support from v0**, `LinearOp<T>` abstraction,
  **task-DAG scheduling via crd-jobs** (vs fork-join BLAS),
  **mixed-precision iterative refinement** (HPL-AI pattern), modern
  preconditioners (SPAI / ILUPACK / SA-AMG / AGMG), **Krylov subspace
  recycling** for eylem-class consumers, **JAX-style operator-level
  autodiff** (vs tape-only), **modern hardware support** (AVX-512 /
  SVE2 / Apple AMX / Intel AMX). Hesap v0 expands from 1.5 weeks →
  ~5 weeks of v0a-f sub-slices. Full phase ~10-12 months elite-tier
  (~52 KLOC; comparable to geometry's 22 KLOC scaled by surface
  breadth).

- **Per-slice DoD adds CLI command schema registration** from
  2026-05-19 forward. Every new slice ships typed `CommandSchema`
  declarations alongside the C++ API. The CLI parser substrate
  itself ships in Phase 4.0 (sequenced after hesap) — protocol
  plumbing now, parser later (same pattern as ADR-0076 §12 used for
  geometry-before-eylem).

- **Phase 4.0 redefined** = `crd-cli` + `crd-rpc` + `crd-script`
  substrate (~12 weeks). MCP exact-compatibility + JSON-RPC server +
  capability-based security + transactional sessions + sandbox
  isolation + C++ hot-reload supervisor. Subsumes the original
  Phase 4.0 = "C++ scripting / DLL hot-reload" (now a sub-aspect).

- **Sequencing (locked 2026-05-19, supersedes 2026-05-15 calendar):**
  1. Phase 3.1.7 close (v11 in flight; targeted fixes verified; CI
     catches any residual drift per `feedback_targeted_fix_skip_resweep`).
  2. **Phase 3.1.6 `crd-hesap` v0-v17** elite-and-big (8-12 months).
     CLI protocol plumbing per slice; parser substrate deferred.
  3. **Phase 3.1 eylem v1c-v9 resume** — consumes geometry from
     day 1; consumes hesap-dense from v1f-articulation onward.
  4. **Phase 4.0 `crd-cli` + `crd-rpc` + `crd-script`** — the
     formalized agent-native substrate.
  5. Per-module CLI back-fill (cross-cutting); notebook + Claude
     Code agent reference integration; engine-wide MCP surface.

### Pinned strategic decisions (2026-05-15 original; superseded items annotated)

1. **Pathway A — Units-first.** Phase 3.1.7.5 `crd-units` is the *immediate next phase*. Project-wide dimensional safety lands before any further geometry slices ship so the v4–v11 + v4-validate API surfaces are typed from day 1 (no retroactive-typing cost).

2. **Engineering-platform leader is the long-term direction (Pathway E).** Reason (user 2026-05-15): *"if engineering work is performant and good, it is easier to put game and animation and entertainment related stuff there."* Engineering rigor (deterministic + dimensional + numerically robust + differentiable) can't be retrofitted; rendering can. Cerid picks the harder-to-fake direction and lets entertainment features grow on top.

3. **Geometry phase ships in FULL (no consumer-driven cutting).** Per user 2026-05-15: *"I need curves, I need all the other things it is the base, we will plug in where we need them in the future and our needs are not secret."* The renewed-scope 49-slice plan stays intact. Pathway B (cut to consumer-driven) is rejected — the user has product clarity on every substrate's eventual consumer (curves → cinematic + robotics + path tools; polygon → PCB + navmesh + CAD sketches; mesh-processing → cooker LOD + FEA prep; delaunay → navmesh + FEA tetmesh; transform-aware → every consumer; v4-validate → cooker + editor mesh-import gate).

4. **`crd-hesap-dense` v0 ships BEFORE eylem v1c resume** (after 3.1.7 close). **REVISED 2026-05-19**: hesap-elite-and-big — full v0-v17 substrate (~10-12 months) ships before eylem v1c resume; v0 expands to v0a-f over ~5 weeks (matrix types + complex + LinearOp + task-DAG + mixed-precision IR + CLI plumbing + bench substrate). Aligns with the engineering-platform pivot. Eylem v7 FEM and v9 differentiable later consume hesap natively. Per the user "elite, no shortcuts" mandate.

5. **C++ scripting + DLL hot-reload DEFERRED to Phase 4.0 as planned.** Not pulled forward. **REVISED 2026-05-19**: locked as the **ONLY** scripting language (no Lua / Python / GDScript). Phase 4.0 absorbs `crd-cli` + `crd-rpc` + `crd-script` into one agent-native substrate phase. ADR-0034 is subsumed by ADR-0081 (Proposed). Original 2026-05-15 reasoning still valid: (a) no consumer-tier code exists yet to reload — hesap-v0 protocol plumbing serves as the first consumer; (b) DLL supervisor's state-migration design crystallizes when crd-hesap-repl + crd-cli land; (c) script-as-DLL-loaded-into-engine over a domain is the right shape (agent emits C++ → cooker compiles → hot-reload → call); (d) iteration-speed productivity gain available cheaper via per-slice protocol plumbing until Phase 4.0 ships the parser.

6. **Cross-cuts run in flight with units adoption + through geometry phase.** Four detours: **D-006 `crd-time` substrate ✅ shipped 2026-05-15** (absorbs `platform::Timer`/`FrameClock`, ships `Instant`/`Duration = Quantity<dim::Time, f64>`/`Stopwatch`/`FrameClock` fixed-step + alpha/`DeterministicClock`/`Deadline`/GPU timestamp delegation; **first major consumer of the units substrate**; foundation for D-003 + D-004 + eylem v1c+ fixed-step); **D-003 `crd-perf` profiler + ImGui frontend ✅ shipped 2026-05-15** (renamed from `crd-profiler` at v0a — collision with existing `crd-profile` quality-preset module; **all 8 slices v0a-v0h shipped same session** = substrate + UX + sandbox wiring + ADR-0079 + system doc + `win-shipping-profile` preset extending per-slice DoD 4 → 5 configs; 97 perf-* test cases / 336 assertions); D-004 deterministic-replay sandbox queued (uses D-006 `DeterministicClock`); D-005 config/resource hot-reload polish queued. Per-slice protocol fix shipped 2026-05-15 as Sprint 0 ahead of units v0a (codified in `feedback_per_slice_run_ctest.md` + `scripts/per-slice-check.{ps1,sh}` + `docs/protocols/per-slice-verification.md`).

7. **Eylem cold-storage mitigation.** Eylem v1b shipped 2026-05-11; v1c resumes ~7 months from now per this plan. To prevent code rot: as each geometry sub-module ships (v4 mesh / v5 spatial / v6 polygon / …), run a ~30-min integration smoke against the corresponding eylem v1c+ stub path (e.g. v4 mesh → smoke `eylem::TriangleMeshCollider` stub; v2 GJK → smoke eylem v1d narrowphase stub). Not a slice; a per-sub-module hygiene practice. See `feedback_per_slice_run_ctest.md` for the protocol.

### Calendar (target)

| Window | Work | Outcome |
|---|---|---|
| 2026-05-15 (done) | **Sprint 0** per-slice protocol fix + **Sprint 1 Phase 3.1.7.5 v0a `crd-units` substrate** ✅ shipped (138 cases / 464 assertions; ADR-0078; `docs/systems/units.md`; CI guard live) + **Detour D-006 `crd-time`** ✅ shipped (first major units consumer; `Duration = Quantity<dim::Time, f64>`) + **Detour D-003 `crd-perf` profiler + ImGui frontend** ✅ shipped (8 slices v0a-v0h; 97 perf-* cases / 336 assertions; ADR-0079; `docs/systems/perf.md`; `win-shipping-profile` preset; per-slice DoD 4 → 5 configs). | Project-wide dimensional safety substrate; timing substrate; live frame metrics (CPU per-thread + GPU per-pass + memory per-allocator + user counters); ImGui flame graph + capture/replay file export. |
| Now → +5 weeks | **Phase 3.1.7.5 v0b/c/d adoption pass** (`crd-config` + `crd-scene Transform` + glTF cooker for v0b ~600 LOC ~5 days; `crd-eylem RigidBody` + integrator + force fields + `crd-geometry-primitives` API surface for v0c ~800 LOC; `crd-renderer` + cookers + ImGui + Layer-6 format/parse/UnitPreferences + cross-engine readers + 17-config sweep close for v0d ~700 LOC) — IN PARALLEL with **D-004 replay sandbox**, **D-005 config/resource hot-reload polish** | Project-wide dimensional safety adopted across `crd-config`/`crd-scene`/`crd-eylem`/`crd-geometry`/`crd-renderer`/cookers; deterministic-replay validated; config-tier iteration speed |
| +5 weeks → +5 months | **Resume Phase 3.1.7** geometry: v4 `-mesh` (+ v4-validate) → v5 `-spatial` → v6 `-polygon` → v7 `-mesh-processing` → v8 `-delaunay` (with v8c-pre `insphere` Stage D paydown) → v9 `-gpu` + V-HACD + REPL → v9e shader-helpers GLSL/HLSL emit → v10 `-curves` (a–e) → v11 transform-aware. Per-sub-module eylem smoke against the relevant v1c+ stub. | All planned substrate consumers (sdf, renderer-cull, audio raycasts, editor picking, navmesh, V-HACD, cinematic paths, robotics trajectories, eylem mesh-collider) light up |
| +5 → +6 months | **Phase 3.1.7 CLOSE** (full 17-config sweep + ADR-0076 §19 amendment) + **Phase 3.1.6 `crd-hesap-dense` v0** (BLAS L1/L2/L3 + LAPACK-class direct: Cholesky / LU / QR — minimum to unblock eylem v7 FEM + future CFD/FEA/control) | Engineering-platform pivot officially starts; first real numerical-computing substrate slice |
| +6 → +9 months | **Resume Phase 3.1 eylem v1c+**: broadphase consuming `crd-geometry-bvh::DynamicBvh` → v1d narrowphase consuming v2 GJK/EPA → v1d-manifold consuming v2j → v1d-mesh consuming v4 mesh closest-point + raycast. **First playable physics demo** in the sandbox with units-throughout + profiler instrumentation + deterministic-replay validation. | Cerid's first integrated end-to-end demo; substrate dogfood |
| +9 months → … | Continue engineering-platform pivot: full hesap rollout (sparse / iterative / direct / eig / opt / ode / fft / dsp / stats / tensor / autodiff / gpu), then 3.1.5 sdf, then 3.1.8+ domain substrates (brep / cad-feature / cfd / estimation+control / fea / cam / ml-inference / procgen / sciviz / eda) | The 8-domain mandate per ADR-0077 starts shipping |

### What this plan does NOT change

- ADR-0076 §1-§18 architecture (sub-module split, dimension exponents, predicate tiering, query API shape) — all stays as before.
- The renewed-scope 49 slices stay intact (no cutting).
- The substrate-first principle — extended with the engineering-platform priority for the medium-term sequencing.
- ADR-0077 multi-domain expansion — unchanged; the engineering-platform pivot just reorders WHEN these phases start (after physics demo, not after a hypothetical "game-first" milestone that no longer exists).

### What this plan REPLACES

- The implicit "geometry → eylem v1c+ resume → sdf interleave → hesap (3.1.6) after eylem" sequence is REPLACED with: **geometry → units (3.1.7.5) → cross-cuts (D-003/D-004/D-005) → hesap-dense-v0 → eylem v1c+ resume → physics demo → full hesap → sdf → domain substrates.** The early hesap-dense-v0 is the engineering-platform pivot's first concrete artifact.
- The implicit "C++ scripting + DLL hot-reload land in Phase 4.0 someday" was already correct; this plan explicitly says **do not pull it forward** until a domain module's consumer pulls it.

ADR-0076 §19 amendment will record this plan formally at the next ADR session.

## Decision log

All architectural decisions are individual ADRs under `docs/decisions/`.
Tag-indexed list: `docs/decisions/README.md`.

## Detour queue

Active detour, if any, is named in `context.md`. Queue and rules:
`docs/detours/README.md`.

## Open debt

`docs/debt.md`.

## Long-range outlook

Direction, not commitment.

### Already achieved (as of 2026-05-16)
- Stable Vulkan backend, GPU allocator strategy in place
- Shader system v1 with hot-reload, variant keys, pipeline handoff
- Renderer v1a–i (Clustered Forward+, frame graph, descriptor system, material system)
- **Jobs system v1 complete** (v1a–v1k): fiber pool, work-stealing deque, ABA-safe scheduler, counter/wait, worker pool, public API (`run`/`wait`/`make_job`/`parallel_for`), 41-byte SBO, per-frame arena, crd-app wired
- ImGui debug tooling; crd-config TOML substrate
- **Resource system v1 complete** (v1a–v1g): handle table, ref-counting, sync/async/streamed loading, typed loaders (shader, material), hot-reload with atomic payload swap + mtime polling + callbacks, 2Q LRU eviction, memory budget, pin/unpin
- **Asset import bootstrap** (Phase 2.7): TextureResource + MeshResource + glTF + MaterialTemplate/Instance + GpuUploader + crd-meshgen + crd-sandbox
- **Material GPU wiring** (Phase 2.8): per-material pipeline cache + multi-pass ForwardRenderPath + depth-only prepass + sandbox 3D rendering + glTF demo asset bundle + unified Asset Browser
- **`crd-scene` v1a–v1p CLOSED 2026-05-10** (Phase 3.0): 8-layer slot-shaped ECS — Entity/SlotMap, Archetype+SparseSet hybrid storage, mixed-backend visitor, six built-in relations, Query DSL, System+Schedule+Commands (7-phase fixed), IComponentIndex framework + 5 reserved index shells (one now realized as `SpatialBVHIndex` LooseOctree-backed per Phase 3.1.7 v5-index-bringup), Transform+TransformPropagation (bit-exact deterministic), SceneResource+SceneLoader + cook_scene cooker, **Öbek system (ADR-0058)** with all 3 InheritPolicy values + transparent CoW + 4 revert granularities + AAAA-tier reservations (BatchHints / BatchInstanceTag / OBAT chunk for GPU instancing Phase 3.5+, ObekEntityGuid for replication/replay), ScriptComponent + reserved spatial DSL operators, API surface freeze. ADRs 0049–0061
- **Memory subsystem v2** (2026-05-07, Detour D-001 closed): production-grade `TlsfAllocator` (arbitrary alignment, try-allocate, ~7 KB metadata, ASan-validated) + `GrowablePoolAllocator` (page-pooled fixed-size aligned blocks, O(1) alloc/free); `ChunkAllocator` refactored on top of GrowablePool (closed v1c1 O(N) free debt); `World`/`ArchetypeGraph`/`SparseSetStorage` route every byte through one root `IAllocator`
- **Phase 3.1 eylem v0/v1a/v1a-material/v1a-draw/v1b-a..d shipped** (paused at v1b close per the Strategic Execution Plan 2026-05-15; v1c resumes after Phase 3.1.7 close + crd-hesap-dense v0)
- **Phase 3.1.7.5 `crd-units` CLOSED 2026-05-15** (v0a substrate + v0b/c/d adoption passes; ADR-0078 §1–§5): zero-overhead `Quantity<D, T>` + 6-layer conversion system + 8 SI dimensions + ~120 UDLs + `Vec<Quantity>` + 13 typed config accessors + scene::Transform::translation typed + glTF cooker `[cook] position_scale` + crd-eylem RigidBody/PhysicsConfig/IPhysicsScene dimensional with 80-byte freeze pin + crd-geometry-primitives struct widening + queries_typed.hpp boundary layer + Layer-6 UnitPreferences + 11 discipline-preset factories. **§5 two-layer typed architecture pin** locked engine-wide. **`crd-no-untagged-physical-numeric` CI guard** live
- **`crd-time` D-006 shipped 2026-05-15**: `Instant` + `Duration = Quantity<dim::Time, f64>` + `MonotonicClock` / `WallClock` / `CycleCounter` + `Stopwatch` + `FrameClock` (fixed-step + alpha) + `DeterministicClock` + `Deadline` + `GpuTimestampHandle`. Absorbs `platform::Timer` via ADR-0076 §13 move-and-delete pattern
- **`crd-perf` + `crd-perf-ui` D-003 shipped 2026-05-15** (v0a–v0h all in one day): per-thread SPSC ring of 32 B Samples + interned names + `ScopedRegion` RAII + `JobObserver` hook + VulkanProfilerBackend GPU timestamps + per-allocator memory stats + ImGui flame graph + capture/replay file export + `win-shipping-profile` preset (per-slice DoD 4→5 configs); ADR-0079
- **Phase 3.1.7 `crd-geometry` — 8 of 11 sub-modules COMPLETE** (59 of 49 renewed-scope slices shipped, 120%): `-primitives` ✅ v0a–v0f + v1h (2026-05-13) + `-bvh` ✅ v1a–v1g (2026-05-13) + `-convex` ✅ v2a–v2j + v2-close + v3 Shewchuk-predicates + hull-extension + simplify (2026-05-14/15) + `-mesh` ✅ v4a–v4d + v4-validate + v4-close (2026-05-16) + `-spatial` ✅ v5a–v5e + thread-safety pass + scene-index bringup + queries-extension + v5-close (2026-05-16) + `-polygon` ✅ v6a–v6e + v6-close (2026-05-16) + **`-mesh-processing` ✅ v7a HalfEdgeMesh + v7b QEM (Garland-Heckbert 1997) + v7c Loop (1987) + v7d Botsch-Kobbelt remesh (2004) + v7e Liepa hole-fill §3+§4+§5 (2003) + v7f manifoldness repair + v7g self-intersection removal (Möller 1997 + per-tri v6c CDT) + v7h Taubin smoothing (1995) + v7-close (2026-05-17)** + `-viz` ✅ v1j companion module. ADR-0076 §12+§13+§15+§16+§17+§18+§19+§20+§21+§22 amendments all accepted. 18-config full sweep PASS at each cluster close

### 6–12 months (mid-2026 to early 2027)
- Resource system + asset cooker (Phase 2.6): handle table, ref-counted assets, hot-reload, cooked binary format ✅ SHIPPED
- Asset import bootstrap (Phase 2.7): TextureResource + MeshResource + glTF + material foundation + GPU upload + **crd-meshgen** + **crd-sandbox**; ADRs 0042–0043, 0045, 0048
- Material completion (Phase 2.8): per-material PSO cache + pass-keyed shader variants + depth-only prepass; ADRs 0044, 0046
- Scene/ECS foundation (Phase 3.0): hybrid hierarchy + SoA components + TOML → cooked binary
- **Eylem — Cerid-native physics (Phase 3.1)**: built from day 1 (no PhysX wrap step). **⏸ PAUSED at v1b close** (v0/v1a/v1a-material/v1a-draw/v1b-a..d ✅ shipped 2026-05-11). v1c+ resumes AFTER Phase 3.1.7 `crd-geometry` close + `crd-hesap-dense` v0 per the Strategic Execution Plan locked 2026-05-15 (engineering-platform pivot). When v1c resumes it consumes `crd-geometry-bvh`/`-convex`/`-mesh`/`-spatial`/`-polygon` from day 1 with no deferred-refactor. Pivot per ADR-0076 §12 amendment 2026-05-11.** Module split: `crd-eylem` substrate + `crd-eylem-rigid3d` (active; BodyPool + ColliderPool + EylemSystem + RigidBodyInterpolationSystem shipped) + `crd-eylem-rigid2d` / `-soft` / `-articulation` / `-vehicles` / `-ccd` / `-fem` / `-gpu` / `-diff` (planned) + `crd-eylem-aero` (planned per ADR-0073) + `crd-eylem-cine` (planned per ADR-0074) + `crd-eylem-viz` (companion bridging eylem to crd-draw VisualizerRegistry). v0 ✅ + v1a ✅ + v1a-draw d0..d4 ✅ + v1b-a/b/c/d ✅ + v1a-material a/b/c/d ✅ + v1b-e 🚧 (sweep running). v1 = rigid 3D substrate + crd-draw substrate + Material substrate + 5-tier collision filter + deferred ECS event-stream callbacks + force-field substrate (9 formulas) + conservation-law CI + deterministic snapshot/replay across 9 CI configs. v2-v9 stage 2D specialisation, XPBD soft/cloth/rope, articulations + cinematic bridge, vehicles + 5 friction models, CCD + Featherstone + URDF/SDF/MJCF + nonsmooth Newton + sensors + aerospace substrate, FEM + HuntCrossley, GPU acceleration + Newton restitution, differentiable + cross-engine bench. **8 ADRs Accepted** (0062 / 0063 / 0066 / 0067 / 0068 / 0069 / 0075 + 0066 §19.2.1 extension) + **5 ADRs reserved Planned** (0070 / 0071 / 0072 / 0073 / 0074 mint when their research dossiers ship at slice time). 4 research dossiers shipped; 6 more planned for Wave 2/3 ADRs. Supersedes ADR-0018 + Phase 6.
- **`crd-sdf` substrate (Phase 3.1.5)**: signed-distance-field module consumed by eylem (mesh colliders + closest-point), font (MTSDF), renderer (DFAO/DFGI in 3.5+), audio (acoustic occlusion in 3.4), editor (CSG modelling in 7). 8 slices over ~5–6 wk: v0 analytic primitives → v1 dense 3D grid + CRDR → v2 mesh-bake (Jacobson 2013 winding-number sign + BVH closest-point, parallel) → v3 narrow-band sparse → v4 CSG + smooth-min → v5 GPU 3D-texture upload + GLSL helper → v6 cooker → v7 Marching Cubes extraction → v8 reserved (GPU baker / VDB / Dual Contouring). ADR-0064; deterministic per ADR-0063; plan: `docs/phases/phase-3.1.5-sdf.md`; research: `docs/research/cerid-sdf.md`. **Slot unchanged by ADR-0076 §12 (2026-05-11): still interleaved between eylem v2 and v3.** v2 mesh-bake now consumes `crd-geometry-mesh` + `crd-geometry-bvh` directly from day 1 (no narrow-version refactor — the original ADR-0064 §4 deferred-refactor is OBSOLETE since geometry ships before sdf).
- **`crd-geometry` substrate (Phase 3.1.7)** — ADR-0076 ✅ Accepted 2026-05-11 + **11 amendments (§12 through §23) + §24/§25/§26 planned for v9 sub-cluster closes**. **Status:** 🚧 ACTIVE — **10 of 11 sub-modules COMPLETE 2026-05-18** (`-primitives` ✅ + `-bvh` ✅ + `-convex` ✅ + v3 hull extension ✅ + `-mesh` ✅ + `-spatial` ✅ + `-polygon` ✅ + `-mesh-processing` ✅ + `-delaunay` ✅ + **`-decomposition` ✅**; 73 of 49 renewed-scope slices shipped, 149%). **Phase 3.1.7.6 `crd-rhi-compute` prerequisite sub-phase ✅ CLOSED 2026-05-17** (6 slices v0a-v0e + v0-close; ~1290 LOC engine + ~1200 LOC tests; ADR-0080 Accepted with D1-D12 + D2/D7/D8 revisions; system doc `docs/systems/rhi-compute.md`; 5 days actual vs 4 wk budget ≈ 5-6× ahead). Substrate ready: compute pipelines + storage buffers + dispatch + cross-stage barriers + async compute queue + binary semaphores + spec-const & workgroup reflection. **v9-prereq-test-harness ✅ SHIPPED 2026-05-18** — GPU sanity-check infrastructure for the 16 v9 GPU slices: `crd::rhi::ValidationCapture` RAII messenger on `VkInstance` + `crd::test::ulp_compare<f32>` / `bit_compare<T>` + `crd::perf::measure_ms` + `CRD_PERF_BUDGET_LE` macro + `crd::test::gpu_determinism_check` + per-slice DoD `-IncludeRelease` flag (opt-in 5th config catches LTCG miscompiles). Discipline locked: every v9 GPU slice = (a) ValidationCapture asserts 0 errors/warnings, (b) ulp/bit_compare GPU vs CPU reference, (c) determinism check 3 rounds if claiming determinism, (d) CRD_PERF_BUDGET_LE per published per-kernel budget. 5-config DoD PASS in 02:48. **v9c-a V-HACD voxelize ✅ SHIPPED 2026-05-18** — new module `crd-geometry-decomposition` (10th `-geometry-*` sub-module); opens v9c `-decomposition` cluster. Algorithm: two strictly non-overlapping passes (D126) — (1) parallel SAT surface marking via Akenine-Möller 2001 13-axis exact test + per-voxel `std::atomic_ref<u8>::fetch_or(Surface)` race-free union via `crd::jobs::parallel_for`; (2) classification of still-Unknown voxels via WindingNumber (default, robust on non-watertight via Jacobson 2013) OR FloodFill (fast, requires watertight; leaks documented). `VoxelGrid` opaque dense `Array<u8>` + `atomic_ref` accessors (future bricked/sparse backend non-breaking). Sizing precedence: `fixed_resolution` wins, else `target_voxel_count`. Divergence from Mamou (D124): exact SAT not conservative centroid-classification — substrate serves CAD/SDF too. f32 only (matches `mesh_winding_number`); f64 follow-on. Pinned D123-D128 for ADR-0076 §24 amendment. **v9c-b V-HACD decompose ✅ SHIPPED 2026-05-18** — `vhacd_decompose(VoxelGrid, opts, alloc) -> VhacdResult { Array<QuickhullResult<f32>> parts; ... }`. Module gains PUBLIC dep on `crd-geometry-convex`. Mamou §3.2-3.4 recursive plane-search. Concavity D129 = voxel-fraction (NOT Hausdorff; documented divergence). Cluster repr D131 = voxel_indices + per-voxel sidecar. β=0 default. **v9c `-decomposition` CLUSTER CLOSED 2026-05-18** — 18-config full sweep PASS (`scripts/full-sweep.ps1`: 11 Windows + 7 Linux); ADR-0076 §24 amendment locks D123-D131 (9 design decisions); eylem v1c convex-collider-conditioning **stub integration smoke** SHIPPED (`[eylem-stub]` tag in test_vhacd.cpp; full pipeline triangle-mesh→voxelize→vhacd_decompose→per-part ConvexHullView→`contains(view, centroid)` — canary catches API drift before eylem v1c lands at Phase 3.1 v1c-resume). Cluster totals: 3 slices, ~1330 LOC engine + ~820 LOC tests, 21 cases / 89 assertions. 10 follow-on slices filed (none regressions). **Phase 3.1.7 sub-module 10 of 11 ✅** (primitives + bvh + convex + v3 hull-ext + mesh + spatial + polygon + mesh-processing + delaunay + **decomposition** ✅). **v9a-a + 4 follow-ons ALL SHIPPED 2026-05-18** — new sibling module `crd-geometry-bvh-gpu` (D132 — mirror of `crd-rhi`/`crd-rhi-vulkan`). **Base v9a-a**: 30-bit Morton CPU oracle + GPU dispatch; GLSL shader is mechanical translation of CPU body (D134); pre-existing rhi-vulkan VK_KHR_surface bug surfaced + fixed. **4 follow-ons paid in-line same day** (initial deferral overturned per user "build substrate fully" direction; memory rule refined): `v9a-a-typed` (Length<T> strip-compute-retag wrappers, D137); `v9a-60bit-cpu` (u64 60-bit Morton CPU oracle + km-scale discriminator, D138); `v9a-a-async-compute` (new `Device::create_command_buffer_for_queue` virtual + Vulkan compute-family pool + opt-in async dispatch, D139); `v9a-60bit-gpu` (`Device::supports_shader_int64` + GLSL u64 shader + sibling pipeline class, D140). 5-config DoD PASS in 39 s. Combined: 5 slices, ~1170 LOC engine + ~880 LOC tests, 26 cases / 175 assertions, 9 locked decisions D132-D140. RHI surface gained 2 new virtuals (both appended at END per vtable-stability). **v9a-b1 CPU stable LSD radix sort ✅ SHIPPED 2026-05-18** (`sort_morton_pairs<KeyT>` templated for `KeyT ∈ {u32, u64}` from day 1; 20 tests / 40 134 assertions; D141-D144 pinned). **v9a-b2 GPU LSD radix sort ✅ SHIPPED 2026-05-18** (`MortonRadixGpuPipeline::dispatch_radix_sort` — 4-bit-digit × 8 passes; prefix-sum scatter D146 ⇒ output byte-identical to v9a-b1 oracle, not throughput-tier; 4 compute shaders + 25 dispatches/sort; 9 tests / 8 257 assertions; D146-D149 pinned). **v9a-b1-simd scalar+prefetch ✅ SHIPPED 2026-05-18** (Investigated AVX2 sub-histograms + Wassenberg SWWC + multi-pass histogram via web research, all slower at 1M / 8MB working set; `_mm_prefetch` 8 iters ahead in scatter wins +7%; D145/D150/D151 pinned; debt cleared). **v9a-b1-parallel 3-phase parallel via crd-jobs ✅ SHIPPED 2026-05-18** (opt-in `sort_morton_pairs_parallel<KeyT>` with deterministic per-(chunk, bucket) stable-merge; 8 tests / 400 025 byte-identical assertions across worker-spanning corpus + num_jobs ∈ {1,2,4,8,16} byte-identity; measured **1.86× speedup → 2.56 ms / 1M u32 win-shipping**, 7.8× budget headroom; D152-D155 pinned; substrate template for eylem v1c broadphase + parallel BVH refit + cooker bakes). **v9a-c LBVH tree + AABB upsweep ✅ SHIPPED 2026-05-18 (elite combined slice, absorbs originally-separate v9a-d)** — Karras 2012 §2.2 binary-tree-from-sorted-codes + §2.4 atomic-counter parent-walk upsweep + canonical-layout DFS reorder; the original "atomic vs 2-pass barriers" decision-fork dissolves (D159: Karras §2.4 is BOTH faster AND bit-deterministic via commutative AABB union); `coherent` qualifier + `memoryBarrierBuffer()` load-bearing in GLSL upsweep (Lesson 09 — atomicAdd's acquire-release on the atomic does NOT synchronize non-atomic bounds writes); 12 tests / **379 021 byte-identical-topology + 1-ULP-bounds assertions** including 4 advisor-flagged degenerate cases (N=1 / N=2 / all-equal / adjacent-equal-interspersed) + end-to-end CPU↔GPU pipeline; D156-D164 pinned. **v9a-close ✅ CLUSTER CLOSED 2026-05-18** — ADR-0076 §25 ✅ Accepted, 33 design decisions D132-D164 locked, 18-config full sweep PASS, performance characterization honestly pinned (test-harness 53.7 ms / 1M dev box; pure-GPU pipeline ~5-8 ms / 1M RTX 3060 estimate). **Phase 3.1.7 sub-module 11 of 11 ✅ — `crd-geometry` substrate COMPLETE.** Cluster totals: 10 algorithmic slices + close in 1 day; ~5 600 LOC engine + ~3 800 LOC tests + ~71 cases / ~827 K assertions; 3 lessons captured (02, 04, 09) in `docs/lessons/`. **Next = v10 `-curves` cluster (Bezier / B-spline / Hermite, 5 slices, ~2 wk)** (16 unbundled slices: v9c V-HACD + v9a GPU LBVH + v9b GPU refit + v9e shader-helpers + 3 cluster-closes; ~7300 LOC / ~6 wk; ADR-0076 §24/§25/§26 amendments lock at sub-cluster closes) → v10 `-curves` → v11 transform-aware. **v9d REPL bindings deferred** to Phase 3.1.6+ when `crd-hesap-repl` host exists. Computational-geometry primitives + spatial-acceleration substrate. Peer module to `crd-math` / `crd-sdf` / `crd-hesap` (NOT bloated into `crd-math`). Multi-domain consumer list: eylem broadphase (BVH refit/build, v1c) + narrow phase (GJK + EPA, v1d) + mesh collider (closest-point + raycast on triangle mesh, v1d-mesh) + convex collider conditioning (v1c); crd-sdf v2 mesh-bake (winding-number test + BVH closest-point); crd-renderer Phase 3.5+ (frustum cull + occlusion BVH); crd-scene `SpatialBVHIndex` reserved shell (ADR-0053); crd-audio Phase 3.4 (acoustic ray-casts); crd-eylem-aero (ADR-0073, surface eval); crd-eylem-cine (ADR-0074, animated mesh queries); editor Phase 7 (V-HACD pipeline + selection + picking). Inherits ADR-0063 determinism contract (deterministic BVH SAH split tiebreak; deterministic GJK simplex update; deterministic-FP polygon predicates per Shewchuk 1997). 11 sub-modules: `-primitives` + `-bvh` + `-convex` + `-mesh` + `-spatial` + `-polygon` + `-mesh-processing` + `-delaunay` + `-gpu` + `-decomposition` + `-shader-helpers` (cooker-emitted GLSL/HLSL) + `-viz` (debug-draw companion, depends crd-draw). Two-layer API mirrors crd-hesap: typed C++ Eigen-class for engine code + opt-in cooker/editor façade. **§12 amendment dissolves the deferred-refactor pattern** — eylem v1c/v1d/v1d-mesh + sdf v2 all consume `crd-geometry` from day 1, no ships-own narrow versions. **§13 amendment (2026-05-11)**: v0a move-and-deletes the pre-existing `crd::math::geometry` into `crd-geometry-primitives` (`crd-math` then lean — Vec/Mat/Quat/Transform/SIMD/deterministic only); + new v0f cutting-edge/branchless/SIMD intersection corpus (watertight ray-tri Woop 2013, Baldwin-Weber 2016, branchless NaN-safe slab ray-AABB, Ize 2013 robust-traversal precompute, Plücker edge tests, Ericson Voronoi-region closest-point, `Vec4f`/`Vec8f` batch kernels). **§15 amendment (2026-05-13, checklist-driven):** 3 new slices — v1h primitives hardening (`constants.hpp` epsilon policy + `is_finite`/NaN-Inf contract + `signed_distance.hpp` iq analytic SDFs in C++ + `ConvexHullView`), v1i unified query facade (`queries.hpp` raycast/overlap/closest-point/contains/distance over {primitive, BvhTree, Bvh4Tree, DynamicBvh} + ray/sphere/box shapecast + `find_overlapping_pairs(DynamicBvh)` + degenerate/large-coordinate validation sweep), v1j `crd-geometry-viz` companion module — plus `-convex` v2 `ConvexHullView` queries + GJK convex shapecast, `-spatial` v5 dense `UniformGrid`, `-shader-helpers` v9e GLSL twins of `signed_distance.hpp`; pinned: query API compile-time-overload-polymorphic not virtual, NaN/Inf queries-tolerate-builders-reject, epsilon policy in one place. Full **33 slices / ~18.5 KLOC engine + ~5 KLOC editor + ~4 KLOC cooker-emitted GLSL/HLSL / ~6–8 months**. Research dossiers: `docs/research/cerid-geometry.md` (11,523 words base + §13 addendum) + `docs/research/cerid-geometry-supplement.md` (6,116 words); phase plan: `docs/phases/phase-3.1.7-geometry.md`.

- **`crd-hesap` substrate (Phase 3.1.6)**: MATLAB-class numerical computing substrate. Sequential successor to Phase 3.1 (eylem). 18 slices over ~6–8 months. Sub-modules: dense (BLAS L1/L2/L3 + LAPACK-class direct + SVD + eig), sparse (CSR/CSC/BSR/COO/ELL/HYB + spmv/spmm/spgemm), iterative (CG/PCG/BiCGSTAB/GMRES/MINRES/LSQR/IDR + Jacobi/IC/ILU/AMG preconditioners), direct (sparse LU/Cholesky/QR multifrontal + AMD/RCM/nested-dissection reorderings), eig (Lanczos/Arnoldi/IRA/LOBPCG), opt (L-BFGS/SQP/IPOPT-class NLP/OSQP-style QP/simplex+IPM LP), ode (DOPRI5/8 + BDF + Rosenbrock + Pantelides DAE), fft (Cooley-Tukey + Bluestein + DCT/DST/Hartley), dsp (FIR/IIR + biquad + polyphase resample + Welch spectral), stats (20+ distributions + tests + special functions + splittable PCG + Xoshiro256\*\*), tensor (N-dim + broadcasting + einsum), autodiff (forward dual/Jet + reverse tape over BLAS), gpu (mirrors CPU API via Vulkan compute + ADR-0061 UploadHandle/Fence), repl (interactive + `.cnb` notebook + plug-in C ABI per ADR-0034). Inherits ADR-0063 determinism contract; deterministic same-input → byte-exact-output across compilers/platforms/SIMD widths. Eylem v7 (FEM) ships its own narrow internal PCG until `crd-hesap` arrives, then refactors to use it. Underwrites the MATLAB-class scientific tool ambition. ADR-0065; plan: `docs/phases/phase-3.1.6-hesap.md`; research: `docs/research/cerid-hesap.md`.
- Animation (Phase 3.2): skeletal + blend trees + IK
- Font rendering (Phase 3.3): `crd-font` — MTSDF (consumes `crd-sdf`), HarfBuzz, billboard + 3D text; ADR-0047

### 12–24 months (2027–2028)

- **Audio (Phase 3.4)**: spatialized audio, mix graph, DAW plugin host scaffold

- **PBR + lighting + NPR (Phase 3.5)**: the core visual quality of the engine.
  - HDR pipeline (R16G16B16A16F) + ACES / AgX tone map + auto-exposure
  - Cook-Torrance GGX PBR (metallic-roughness, albedo/normal/roughness/metallic/AO/emissive textures)
  - Image-Based Lighting: HDRI import → prefiltered env map + irradiance SH + BRDF LUT
  - Punctual lights: point, spot, directional with physical attenuation
  - Cascaded Shadow Maps (CSM, 4 cascades, PCF) + PCSS soft shadows + contact shadows
  - Area lights: rectangular/disk/sphere/tube (LTC approximation, Heitz 2016)
  - Emissive meshes (HDR values, drives bloom)
  - Extended shading models: **clear coat** (car paint, wet surfaces), **anisotropic specular** (brushed metal, hair, vinyl), **cloth/velvet** (Ashikhmin-Premoze), **iridescence** (soap bubbles, oil slicks, beetle shells), **transmission/refraction** (glass, ice, gems, thin fabric)
  - **Subsurface scattering**: pre-integrated (skin, wax, marble, jade, leaves — LUT per diffusion profile) + screen-space Jimenez separable blur (close-up skin, candles)
  - **Toon / NPR**: ramp-based cel shading, stepped specular, inverted-hull outline pass, rim lighting, hatching

- **Atmosphere + volumetrics (Phase 3.6)**: physically-based sky and participating media.
  - **Sky atmosphere** (Hillaire 2020): transmittance LUT + multi-scatter LUT + sky-view LUT + aerial perspective froxel volume; physical sun disc with limb darkening; moon + star field; time-of-day system
  - **Volumetric fog**: froxel grid (160×90×64), Henyey-Greenstein phase function, temporal reprojection
  - **God rays / volumetric light shafts**: screen-space radial blur, shadow-masked
  - **Volumetric clouds**: layered Perlin-Worley noise raymarching, powder term, temporal reprojection at ¼ res, wind animation
  - **Aurora borealis**: thin-volume emission curtain, spectrum LUT, temporal reprojection
  - **Weather system**: rain + snow + dust GPU particles, wet-surface material modulation, fog density coupling

- **Post-processing stack (Phase 3.7)**:
  - Bloom: dual-Kawase 6-level filter + lens dirt mask + anamorphic streaks
  - SSAO / GTAO (Jimenez 2021) + bent normals for directional occlusion
  - Screen-Space Reflections (SSR): HiZ traversal, roughness-blended fallback to IBL
  - Temporal Anti-Aliasing (TAA): Halton jitter + variance clipping + sharpening
  - Color grading: LUT-based (64³, `.cube` cooker import) + chromatic aberration + vignette + film grain
  - Depth of Field: CoC compute + hexagonal bokeh scatter-gather (near/far separated)
  - Motion Blur: per-object velocity buffer + tile-max filter + gather reconstruction
  - Lens flare: screen-space occlusion-tested sun/light flare sprites
  - Upscaling: FSR 3 (open default) / DLSS 3.x / XeSS — `IUpscaler` interface; replaces TAA when active

### 24–36 months (2028+)

- **GPU-driven rendering + particles + water (Phase 3.8)**:
  - Hi-Z occlusion culling (depth pyramid, async compute)
  - GPU-driven indirect rendering (compute cull → `VkDrawIndirectCount`, 100k+ objects)
  - GPU particle system (emit/update/sort compute; ribbons/trails)
  - VFX library: fire, smoke, explosion, magic sparks, dust, waterfall splash
  - Ocean / water: Gerstner wave sum + FFT mode, foam map, spray, Beer-Lambert underwater
  - Caustics: screen-space photon splat from water normals
  - Dynamic decals (`MaterialDomain::Decal`, OBB projection, GPU-culled)

- **Global Illumination pre-RT (Phase 3.9)**:
  - SSGI: half-res hemisphere ray march, temporal accumulation (~1.5ms)
  - **DDGI**: dynamic diffuse GI probe grid (Majercik 2021); 64 rays/probe/frame via BVH; irradiance/depth octahedral atlas; trilinear probe interpolation with visibility; relighting in ~1s (~2–4ms)
  - **Radiance Cascades** (Sannikov 2024): hierarchical radiance cache; spatial + angular multi-level merge; may supersede DDGI pending quality/perf evaluation
  - Lightmap baking pipeline: offline pathtracer → TextureResource atlas; xatlas UV unwrap
  - Bent normals + directional AO (free with GTAO)

- **Hardware ray tracing + denoising (Phase 5):** `HybridRenderPath` — rasterized primary visibility + RT secondary effects.
  - `AccelerationStructure` (BLAS/TLAS) + `RayTracingPipeline` as opt-in RHI extensions; software fallback on non-RT hardware
  - RT Ambient Occlusion (RTAO): 1 ray/pixel + OIDN/ReLAX denoiser; replaces GTAO
  - RT Reflections: 1 ray/pixel + temporal reprojection; replaces SSR
  - RT Shadows: per-light soft shadow rays; replaces CSM for primary directional
  - One-bounce RTGI: replaces DDGI for highest-quality mode
  - DLSS 3 / FSR 3 as the upscaling layer on RT output
  - Full path tracing: explicitly deferred (requires 4090-class; editor-only preview mode)
  - See ADR-0046

- **Deferred + Visibility-Buffer paths (Phase 5.2–5.3)**: second and third `IRenderPath` alongside Forward+; G-buffer deferred for high light-count scenes; Visibility Buffer for dense triangle scenes

- **`crd-ui` + shader node editor (Phase 5.0–5.1)**: retained-mode UI, HarfBuzz text, node-authored materials compiled to `crd-shader` CRDR artifacts

- **C++ hot-reload scripting (Phase 4.0)**: DLL reload supervisor, stable C ABI facade; ADR-0034

- **Networking (Phase 4.2)**: transport layer, deterministic simulation, rollback netcode; ADR-0035

- **Editor shell (Phase 7)**: node shader graph editor → CRDR material artifacts; scene hierarchy + inspector; asset browser from `crd-sandbox`

- **Domain modules (Phase 8)**: application-layer integration packs consuming the substrates:
  - **Robotics** (`crd-eylem` + `crd-estimation` + `crd-control` + `crd-geometry-spatial`): URDF / SDF / MJCF importers, ROS2 bridge, ROS-Industrial pattern
  - **Aerospace** (`crd-eylem-aero` + `crd-estimation` + `crd-control`): VLM solver, panel methods, orbital propagation (Kepler + J2/J3 + drag + SRP), atmosphere models (US Standard / COESA-76 / NRLMSISE-00)
  - **Cinematic** (`crd-eylem-cine` + animation + rendering): mocap pipeline, animation retargeting, cinematic camera systems
  - **Medical visualization** (`crd-sciviz` + volumetric `crd-rhi`): DICOM import, transfer-function authoring, multi-modal registration (CT+MRI overlay), surgical planning
  - **Manufacturing** (`crd-brep` + `crd-cad-feature` + `crd-cam` + `crd-fea`): full CAD/CAM/CAE workflow
  - **Scientific computing** (`crd-hesap` + `crd-ml-inference` + `crd-sciviz`): REPL + `.cnb` notebook integration (per ADR-0065)

- **Cerid-native physics (Phase 3.1, eylem)**: built from day 1 — no
  PhysX wrap step. Deterministic-by-construction, ECS-native,
  fiber-jobified, multi-domain (games + robotics + medical + cinematic
  + DAW), templated 2D + 3D, GPU-extensible. ADR-0062, ADR-0063;
  research: `docs/research/cerid-eylem.md`; plan:
  `docs/phases/phase-3.1-eylem.md`. Supersedes ADR-0018.

### `crd-units` architectural commitment (locked 2026-05-14)

**Every physical and scientific quantity carries a compile-time unit, no exceptions.** Pinned in `docs/PRINCIPLES.md` as a Cerid project-wide rule (user mandate 2026-05-14, "make everything, every physical and scientific stuff always having units no matter what"). The substrate is **Phase 3.1.7.5 `crd-units`** — a leaf module (deps: `crd-core` only) wrapping every dimensional quantity in a zero-overhead `Quantity<D, T>` template. Internal canonical = SI base (m / kg / s / rad / K / A / cd / mol; Angle tagged as 8th compile-time dimension to keep `Angle + Length` a compile error). Precision tier (f32 / f64) is orthogonal to unit choice. Asset / file / UI / network boundaries normalize to SI at load via unit-tagged keys (TOML `length_mm = 25.4` / `mass_kg = 5.0` / `force_N = 100.0` / `voltage_V = 3.3` / `temperature_celsius = 25.0` / etc.). SIMD / GPU hot paths reach raw scalar via `.value` member (bit-equal layout, `static_assert`-pinned). `crd-math` stays raw — the dimensional layer is *around* `crd-math`, not inside it. ADR-0078 candidate minted at slice time. **Adoption sequencing**: ships between Phase 3.1.7 close and Phase 3.1 eylem v1c resume (4.5-week window) — eylem v1c+ consumes typed-math from day 1, and every later substrate (sdf, hesap, brep, cad-feature, cfd, estimation, control, fea, cam, ml-inference, procgen, sciviz, eda) consumes typed-math from day 1 by virtue of `crd-units` being upstream in the dependency graph. The Mars Climate Orbiter class of bug ($327M, 1999, Newton·seconds vs pound·force·seconds across a module boundary) becomes a compile error.

**6-layer conversion system (the boundary surface):** (1) **Linear units with `std::ratio` factors** — SI prefixes + standardised imperial encoded as exact compile-time rationals, bit-exact round-trips for `m ↔ mm` / `inch ↔ mm` / `mile ↔ km` / `lb ↔ kg`. (2) **Affine units** — `AffineUnit<Dim, Scale, Offset>` for temperature; distinct `Temperature` / `TemperatureDelta` types so `°C_a - °C_b` returns a delta-temperature (not an absolute), and `°C_a + °C_b` is a compile error. Reserved pattern for `Pressure / PressureDelta` (gauge vs absolute), `Datetime / Duration`. (3) **Non-linear units** — `NonLinearUnit<Dim, ToSi, FromSi>` for dB SPL / dB V / dB W / cents / semitones (v0a ships); stellar magnitude / pH / Richter via the same framework when consumers ask; arithmetic disabled at the type level (`dB + dB ≠ dB(sum)` — caller converts to linear, adds, converts back). (4) **Compound auto-derivation** — `UnitMul<A, B>` / `UnitDiv<Num, Den>` combine via `std::ratio_multiply` / `std::ratio_divide` at compile time; adding one base unit unlocks N compound units automatically (the extensibility multiplier). (5) **Federated domain registration** — `crd-eylem-aero::units` adds AU / ly / parsec / SolarMass / EarthRadius / JulianYear / SiderealDay / StandardG; `crd-eda::units` adds Mil / OhmCm / AmpHour / DbMilliWatt; `crd-cam::units` adds RPM / InchPerMinute / SurfaceFootPerMin / CubicInchPerMin; `crd-eylem-cine::units` adds Frame_24/25/30/48/60/120fps; future `crd-material::units` adds centipoise / pascal-second / specific-heat-capacity. **No central registry, no plugin system** — pure C++ namespace + ADL. (6) **Format + parse + UnitPreferences** — `format_quantity(q, UnitTag, FormatOptions)` for every dimension; `parse_*(StringView) → Result<Quantity>` with imperial-mixed support (`3'6"`), scientific notation, π-literal angles, error-as-value never-throws; `UnitPreferences` per-document with 11 discipline presets (game / CAD / robotics / aerospace / PCB / audio / 3D-print / CAM / cinematic / imperial / SI-strict / scientific); CRDR scene carries preference + raw-SI value (re-format on discipline switch, bytes-on-disk unchanged); cross-engine file readers (glTF `KHR_unit`, STEP `SI_UNIT`, IGES `GLOBAL`, FBX `UnitScaleFactor`, IFC `IFCSIUNIT`, Gerber `%MOIN*%` / `%MOMM*%`) plumb unit tags through their cookers. **Performance:** Layer 1/4 = 1 FP multiply; Layer 2 = multiply+add; Layer 3 = pow/log boundary-only; `Quantity` arithmetic = identical codegen to bare-scalar (objdump-verified). **Extensibility cost:** new base unit = 1 line; new domain pack = 1 header; new conversion class = ~50 LOC. **Frame transforms** (ENU / NED / ECEF, body-vs-inertial, world-vs-local) are *NOT* in `crd-units` scope — they live in `crd-math::Transform` + `crd-geometry::transform_aabb` (the conceptual line that prevents `Position<ENU, Length>` template explosions).

### Multi-domain substrate expansion (ADR-0077, scoped 2026-05-14)

The roadmap added 9 new peer-module substrates to cover the full multi-domain mandate (gaming + simulation + manufacturing + CAD + CFD + math + aerospace + mechanics). These are **aspirational, not committed** — captured in advance so the substrate-first pattern stays clean and we don't bolt them on after the fact. See ADR-0077 §3 for scope, §6 for sequencing, §10 for revisit triggers.

- **Phase 3.1.8 `crd-brep` — NURBS / B-rep core.** Parametric surfaces, B-rep solid topology with tolerances, exact booleans, fillet / chamfer / sweep / loft, STEP / IGES / Parasolid import-export. The CAD/manufacturing substrate; without it, Cerid cannot serve real CAD/manufacturing users (triangle meshes are export-only for them). Slot: after Phase 3.1.7 close. Reading: Parasolid design papers, OpenCASCADE architecture, Piegl & Tiller "The NURBS Book".
- **Phase 3.1.9 `crd-cad-feature` — parametric features + drafting + GD&T.** Feature trees (parametric history), 2D sketches with geometric + dimensional constraints, ISO/ASME drafting, dimensioning, section views, GD&T per ASME Y14.5. Slot: after 3.1.8. Reading: Solidworks / Onshape / Fusion 360 workflow patterns; ASME Y14.5.
- **Phase 3.1.10 `crd-cfd` — computational fluid dynamics.** Unstructured grid topology (cell-centered + vertex-centered with face connectivity for flux computation), FVM/FEM solver framework, compressible/incompressible Navier-Stokes, k-ε / k-ω SST / LES / RANS turbulence models, heat transfer, multiphase VOF + level-set (builds on `crd-sdf`), AMR, combustion. Slot: after `crd-hesap` close (uses sparse + iterative + ODE). Reading: Versteeg & Malalasekera, Ferziger Perić & Street, OpenFOAM, SU2.
- **Phase 3.1.11 `crd-estimation` + `crd-control` — aerospace/robotics substrates.** Kalman / EKF / UKF / particle filter / SLAM substrate; PID / LQR / MPC / robust control / path planning (RRT*, PRM, A*). Slot: after hesap close. Reading: Thrun-Burgard-Fox "Probabilistic Robotics", Stengel "Optimal Control and Estimation", LaValle "Planning Algorithms".
- **Phase 3.1.12 `crd-fea` — engineering FEA.** Static structural, modal, buckling, fatigue analysis; linear → plastic → hyperelastic material models; thermal-structural coupling. Distinct from eylem v7 dynamic FEM (engineering FEA is offline steady-state / modal; eylem FEM is real-time time-domain). Slot: after hesap close. Reading: Bathe "Finite Element Procedures", Hughes "The Finite Element Method".
- **Phase 3.1.13 `crd-cam` — manufacturing toolpaths.** 3- and 5-axis milling, turning, additive slicing (FDM/SLA/SLS/DMLS), G-code output with configurable post-processors per machine controller, material-removal simulation, sheet metal, PCB stack-up. Slot: after `crd-brep` and `crd-cad-feature` close. Reading: Smid "CNC Programming Handbook", FreeCAD Path workbench architecture.
- **Phase 3.1.14 `crd-ml-inference` — neural network inference + differentiable bridge.** ONNX runtime integration, GPU inference via `crd-rhi` compute, differentiable programming layer extending `crd-hesap-autodiff`. Consumers: in-game AI / NPC behavior, denoising (OIDN/NRD), super-resolution (DLSS-class), content generation (text-to-mesh / text-to-texture), animation (motion-matching ML), audio (noise reduction / source separation). Slot: after `crd-hesap-autodiff` lands. Reading: ONNX spec, ggml architecture, NVIDIA TensorRT design.
- **Phase 3.1.15 `crd-procgen` — procedural content generation.** Noise primitives beyond formulary (Perlin / Worley / Voronoi / simplex), Wave Function Collapse, L-systems, terrain (erosion sim, hydrology), city/building generation, Substance-style procedural materials. Slot: after `crd-geometry-mesh-processing` (Phase 3.1.7 v7). Reading: Ebert et al. "Texturing & Modeling", Prusinkiewicz "Algorithmic Beauty of Plants".
- **Phase 3.1.16 `crd-sciviz` — scientific visualization.** Isosurfaces, streamlines, slice planes, vector-field LIC, perceptually-uniform color maps (viridis / plasma / magma / cividis), in-engine 2D/3D plotting (time series / scatter / surface / contour), annotation + measurement (distance / angle / area on 3D models). Slot: after `crd-cfd` and `crd-fea` (the consumers). Reading: ParaView architecture, VTK design, Schroeder/Martin/Lorensen "The Visualization Toolkit".
- **Phase 3.1.17 `crd-eda` — PCB / EDA geometry substrate.** Elevated 2026-05-14 from the renewed-scope review of Phase 3.1.7 `crd-geometry` (the missing-work inventory surfaced PCB/EDA as a domain area that `crd-geometry` should *not* directly address — it's a substantial domain phase of its own). Scope: board outline polygon + holes/cutouts + layer stack + keepout zones; trace geometry (segments, arcs, vias, pads, copper zones); DRC (design-rule checking) for trace-trace clearance, pad-trace clearance, via clearance, board-edge clearance, zone-fill validation, acute-angle / sliver detection; routing helpers (orthogonal, 45°, obstacle avoidance — shove routing + full autorouter are much-later follow-ups); Gerber X2/X3 + drill files + solder-mask + paste-mask output for manufacturing fab; future panelization. Slot: after `crd-cad-feature` (3.1.9) close — EDA reuses the parametric-sketch + drafting substrate (constraint solver, ISO/ASME-class drafting conventions) and the `crd-geometry-polygon` v6 layer (Vatti Boolean, polygon offset). Reading: KiCad source architecture, FreeRouting + DSN format, IPC-7351 land-pattern standards, Gerber X2/X3 format spec, JLCPCB/PCBWay fab capability documents.

### Modern rendering pipeline prologue (Phase 3.5 amendment, ADR-0077 §4)

The existing Phase 3.5 (PBR / IBL / CSM / SSS / NPR / area lights) gains a v0 prologue covering the modern GPU pipeline. AAA engines (UE5 Nanite, Frostbite, idTech) are all moving this way; ship the pipeline before the shading content.

- **Mesh shaders** (`VK_EXT_mesh_shader` / DX12 mesh shaders) — replaces vertex-shader / tessellation chain for fine-grained per-meshlet culling and LOD.
- **Visibility buffer rendering** — Nanite-class. Record visibility (mesh + triangle id) to a buffer; do material shading deferred.
- **GPU-driven culling** — frustum + occlusion on compute; `VkDrawIndirectCount` for variable draw lists.
- **Work graphs** (DX12 Ultimate / `VK_EXT_work_graphs` proposal) — replaces dependency-graph dispatching with self-feeding compute pipelines.
- **Variable rate shading (VRS)** — per-tile / per-primitive shading rate.

### Platform expansion (Phase 6 — reuses the folded "Native physics" slot, ADR-0077 §5)

- **Console support** — PS5, Xbox Series, Switch (each NDA-gated; substrate layer abstracts where possible).
- **Mobile** — iOS (Metal backend for `crd-rhi`), Android (Vulkan + adaptive resolution).
- **Web** — WebGPU backend; emscripten / WASM toolchain.
- **VR/AR** — OpenXR integration; head/hand tracking; per-eye render passes.
- **HPC / cluster computing** — MPI integration for distributed simulation (CFD multi-node, FEA partitioned solve, multi-player physics islands).

The goal: Cerid becomes a real-time substrate for interactive applications
across **games, simulation, creative tools, manufacturing, CAD, CFD, aerospace, mechanics, and engineering** — not "another Vulkan engine."

## Glossary

- **Channel** — a named log filter for one subsystem.
- **Sink** — a log destination (Console / File / Debugger / RingBuffer / Null).
- **RHI** — Render Hardware Interface; the API-agnostic graphics layer.
- **TLSF** — Two-Level Segregated Fit; an O(1) general-purpose allocator.
- **VMA** — Vulkan Memory Allocator (AMD library; Cerid writes its own).
- **ADR** — Architecture Decision Record; one decision per file.
- **SPSC** — Single Producer Single Consumer (lock-free queue class).
- **CSM** — Cascaded Shadow Maps.
- **IBL** — Image-Based Lighting.
- **TAA** — Temporal Anti-Aliasing.
- **TOI** — Time of Impact (continuous collision detection).
- **GJK / EPA** — narrowphase distance / penetration algorithms.
- **Forward+ / Clustered Forward** — forward shading with per-tile or
  per-cluster light lists; scales to many lights without a G-buffer.
- **Visibility Buffer** — render path that defers material evaluation by
  storing only triangle IDs in the framebuffer.
- **Hi-Z** — depth pyramid used for occlusion queries.
- **Fiber** — a cooperative-switch execution context on an OS thread stack; suspends without blocking the thread. Fibers ensure threads are never idle while runnable work exists.
- **DLL hot-reload** — recompile a shared library and swap it at runtime without restarting the process; a stable C ABI boundary prevents name-mangling and vtable issues across the reload.
- **Rollback netcode** — client-side input prediction with server-authoritative correction; the server sends authoritative state, the client re-simulates diverged frames. Canonical for action/fighting games.
- **Deterministic replay** — engine state is reproducible from an initial snapshot plus an input log; prerequisite for rollback netcode, debugging, and simulation validation.
- **ROS2** — Robot Operating System 2; the industry standard message-passing middleware for robotics. Target integration for Cerid's robotics domain module.
- **Digital twin** — real-time synchronization between a physical system (robot, aircraft, medical device) and its Cerid simulation counterpart; driven by live sensor feeds.
- **SE(3)** — Special Euclidean group in 3D; the Lie group of rigid-body transforms (rotation × translation); core to robotics kinematics and aerospace flight models.
- **IBL** — Image-Based Lighting; environment light encoded as a prefiltered cubemap (specular) and spherical harmonics (diffuse irradiance). Drives ambient shading in PBR.
- **CSM** — Cascaded Shadow Maps; multiple shadow map frustums at increasing range to give high-resolution near shadows and wide-coverage far shadows from a directional light.
- **PCSS** — Percentage-Closer Soft Shadows; blocker search in the shadow map to estimate penumbra width; produces soft shadows whose size scales with blocker distance.
- **LTC** — Linearly Transformed Cosines; analytic approximation for area light shading that reduces an arbitrary area light integral to a lookup in two 64×64 LUTs. Heitz 2016.
- **SSS** — Subsurface Scattering; light penetrates a translucent surface (skin, wax, marble) and re-exits at a different point, giving a soft warm glow. Pre-integrated (LUT) or screen-space (blur pass).
- **NPR** — Non-Photorealistic Rendering; shading styles that intentionally diverge from physical accuracy — toon/cel shading, hatching, watercolour, outline passes.
- **Froxel** — frustum-aligned voxel; a cell in a 3D grid aligned to the camera frustum (X/Y in screen tiles, Z in view-space slices). Used for volumetric fog and clustering.
- **Hillaire atmosphere** — Sébastien Hillaire's 2020 EGSR paper on scalable precomputed atmospheric scattering; transmittance + multi-scatter + sky-view + aerial perspective LUTs. Used in UE5.
- **DDGI** — Dynamic Diffuse Global Illumination; probe-based irradiance caching (Majercik 2019/2021). Probes trace rays each frame via BVH; surfaces trilinearly interpolate nearby probes.
- **Radiance cascades** — Alexander Sannikov's 2024 GI algorithm; hierarchical spatial/angular radiance cache merged from coarse to fine each frame. Eliminates probe placement artifacts.
- **GTAO** — Ground-Truth Ambient Occlusion (Jimenez 2021); horizon-based SSAO variant that also outputs bent normals for directional occlusion weighting.
- **SSR** — Screen-Space Reflections; HiZ-traversal ray marching in screen space; blends with IBL at high roughness; falls back to cubemap when reflection leaves the screen.
- **TAA** — Temporal Anti-Aliasing; Halton-jittered projection + history reprojection + variance clipping; prerequisite for temporal effects (SSR, SSGI, DDGI reprojection).
- **DoF** — Depth of Field; CoC (circle of confusion) compute pass + bokeh scatter-gather blur; near/far fields separated to prevent background-leaking-into-foreground artifacts.
- **FSR / DLSS / XeSS** — Temporal upscaling algorithms (AMD FidelityFX Super Resolution / NVIDIA Deep Learning Super Sampling / Intel Xe Super Sampling). Reduce render resolution; reconstruct full-res output. Replaces TAA when active.
- **BLAS / TLAS** — Bottom/Top Level Acceleration Structure; Vulkan RT extension BVH representation for hardware ray traversal. BLAS per mesh; TLAS per scene frame.
- **Gerstner waves** — analytic ocean surface model: sum of sinusoidal wave trains each producing circular particle orbits, yielding the characteristic peaked crests and flat troughs of ocean waves.
- **Dual-Kawase bloom** — efficient downsample/upsample bloom filter by Marius Bjørge; better frequency response and fewer samples than Gaussian; used in production engines (Unreal, Godot 4).
- **AgX** — tone mapping operator designed by Troy Sobotka; superior hue preservation in high-chroma highlights compared to ACES; becoming the new standard in Blender / open-source workflows.
- **GGX** — Trowbridge-Reitz normal distribution function; the standard specular NDF for PBR; produces a long tail (characteristic "sparkle") and physically correct energy conservation.
- **Iridescence** — structural colour from thin-film interference; phase shift of reflected light varies with angle, producing rainbow sheen on soap bubbles, oil slicks, beetle shells, and CD surfaces.
- **Beer-Lambert** — optical transmittance law: `T = exp(-extinction × distance)`; used for deep water colour, coloured glass absorption, fog density, and underwater visibility.

## Document conventions

- `docs/ROADMAP.md` (this hub) — navigation only. Doesn't grow.
- `docs/PRINCIPLES.md` — engineering compass. Stable.
- `docs/phases/<phase>.md` — one file per phase. Slices, decisions ref,
  open questions.
- `docs/decisions/<NNNN>-<slug>.md` — one ADR per decision. Append-only
  numbering. Index at `docs/decisions/README.md`.
- `docs/detours/<D-NNN>-<slug>.md` — one file per detour. Index at
  `docs/detours/README.md`.
- `docs/sessions/<YYYY-MM-DD>-<slug>.md` — one file per work session.
- `docs/systems/<module>.md` — short overview per shipped module.
- `docs/<module>/<MODULE>_FILE.md` — long-form deep-dive (only when
  explicitly requested).
- `docs/bench/` — benchmark baselines, one file per snapshot.
- `docs/debt.md` — open debt list.
- `context.md` (project root) — live state. Short. Updated by
  `@docs-keeper` at session end.

After a system has shipped, **prefer adding to its session log over
rewriting its overview**.
