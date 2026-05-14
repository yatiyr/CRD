# ADR-0077: Multi-domain expansion — the "absolute beast" vision

**Status:** Accepted 2026-05-14
**Supersedes:** none (extends the long-term outlook in `docs/ROADMAP.md`)
**Cornerstones:**
- `docs/PRINCIPLES.md` ("modular by default", "single-path", "substrate-first")
- ADR-0065 (`crd-hesap` peer-module pattern)
- ADR-0076 (`crd-geometry` peer-module pattern)
- ADR-0062 (`crd-eylem` peer-module pattern)

## 1. Context

The roadmap up to Phase 3.1.7 has been driven by what was concretely in flight (graphics → resources → scene → physics → geometry). The user's stated long-term vision is broader: **Cerid as an "absolute beast" across gaming + simulation + manufacturing + CAD + CFD + math operations + aerospace + mechanics**.

When audited against that vision, the existing roadmap has gaps:

| Domain | What's covered | What's missing |
|---|---|---|
| Gaming | Renderer + scene + physics + animation placeholder | Mesh shaders / visibility buffer / GPU-driven culling / work graphs; procedural generation; ML inference |
| Simulation (robotics + medical + cinematic) | eylem rigid/soft/articulated/CCD/FEM; sensor sim reserved | State estimation (Kalman/EKF/UKF/SLAM); control theory library (PID/LQR/MPC); trajectory optimization library |
| Manufacturing | Nothing explicit | NURBS/B-rep substrate; feature trees; STEP/IGES import; GD&T; drawing/blueprint output; CAM toolpath generation |
| CAD | Nothing explicit | All of the above (manufacturing) plus parametric modeling, 2D drafting, sketches, exact booleans |
| CFD | `crd-hesap` sparse + iterative + ODE | Unstructured grid framework; FVM/FEM solver; turbulence models (k-ε, k-ω SST, LES, RANS); compressible/incompressible Navier-Stokes; multiphase (VOF, level-set); AMR; combustion |
| Math operations | `crd-hesap` covers MATLAB-class | Already comprehensive in plan |
| Aerospace | eylem-aero reserved (ADR-0073); orbital primitives implicit | State estimation; orbital mechanics library (Kepler propagation, J2/J3 perturbation, drag, SRP, N-body); VLM/panel methods explicit |
| Mechanics / engineering | eylem dynamic FEM (v7) | Static structural FEA; modal analysis; buckling; fatigue; engineering-grade FEA pipeline |

Plus engineering-cross-cutting tools that don't fit any single domain:
- **Scientific visualization** — isosurfaces, streamlines, slice planes, vector field viz, color mapping, in-engine plots/charts (engineering workflows live in 2D plots as much as 3D viewports)
- **Annotation / measurement** — distance/angle/area on 3D models (CAD review, engineering inspection)
- **Live debugging / profiling** — Renderdoc integration, frame capture, profiler hooks

Plus platform reach:
- **Console support** (PS5, Xbox Series, Switch)
- **Mobile** (iOS, Android — Vulkan + Metal backend)
- **Web** (WebGPU)
- **VR/AR** (OpenXR)
- **HPC** (cluster computing, MPI for distributed simulation)

## 2. Decision

**Adopt a substrate-first expansion plan covering all eight stated domains.** Each missing capability lands as either (a) a new peer-module substrate (the `crd-hesap` / `crd-geometry` pattern), or (b) a flesh-out of an existing placeholder phase, or (c) a prologue/amendment to an existing phase.

The expansion is **aspirational, not committed**. The roadmap captures the long-term vision; execution proceeds slowly and carefully, one phase at a time. The substrate-first pattern means: ship the substrate, then ship consumers, then ship integrations — never inline domain logic into a downstream module.

## 3. New substrates (peer-module slots)

Numbered in the `3.1.x` substrate family to signal "peer to `crd-math`":

### 3.1.8 — `crd-brep` (NURBS / B-rep core)

**Scope:** parametric surfaces (NURBS, Bézier patches), B-rep solid topology (face / edge / vertex with tolerances), exact boolean operations, filleting / chamfering / sweeping / lofting, STEP / IGES / Parasolid import & export.

**Why a separate substrate:** triangle meshes are a viewing/export format; CAD users work in exact parametric geometry. B-rep is foundational — without it, CAD/manufacturing customers cannot use Cerid for what they need. Boolean operations on B-rep are exact (Parasolid kernel pattern); boolean operations on meshes (`crd-geometry-polygon` v6) are approximate. Two different algorithms, two different correctness bars.

**Slot:** after Phase 3.1.7 close (geometry substrate ships before B-rep — meshing/tessellation lives in geometry).

**Dependencies:** `crd-geometry-primitives`, `crd-hesap-iterative` (Newton-Raphson surface intersection), `crd-math`.

**Consumers:** Phase 3.1.9 (`crd-cad-feature` parametric features), Phase 3.1.13 (`crd-cam` toolpaths), Phase 7 (editor), domain-modules (Phase 8).

**Reference reading:** Parasolid kernel design papers, OpenCASCADE architecture, ACIS, "B-rep Solid Modeling" (Mäntylä 1988), "Geometric Modeling" (Mortenson 2006), Piegl & Tiller "The NURBS Book" (1997).

### 3.1.9 — `crd-cad-feature` (parametric features + drafting + GD&T)

**Scope:** feature trees (parametric modeling history), 2D sketching with constraints (geometric + dimensional), 2D drafting (engineering drawings per ISO/ASME), dimensioning, section views, GD&T (Geometric Dimensioning and Tolerancing) per ASME Y14.5.

**Why a separate substrate from `-brep`:** B-rep is the geometric kernel; feature trees + sketches + drafting are the *modeling workflow* layer. They naturally separate.

**Slot:** after `crd-brep` (3.1.8) closes.

**Dependencies:** `crd-brep`, `crd-hesap-opt` (constraint solver), `crd-scene` (feature tree as ECS entities).

**Reference reading:** Solidworks / Onshape / Fusion 360 user manuals (parametric workflow patterns); ASME Y14.5; "Constraint-Based Geometric Modeling" papers (Sutherland 1963 Sketchpad through modern variants).

### 3.1.10 — `crd-cfd` (computational fluid dynamics)

**Scope:** unstructured grid topology (cell-centered + vertex-centered with face connectivity), FVM (finite volume method) solver framework, FEM solver for fluid problems, compressible and incompressible Navier-Stokes, turbulence models (k-ε, k-ω SST, LES sub-grid models, RANS), heat transfer (conduction, convection, radiation, conjugate), multiphase (VOF, level-set built on `crd-sdf`), AMR (adaptive mesh refinement), combustion (premixed/non-premixed with chemistry tables).

**Why a separate substrate:** CFD's data structures (face-connected unstructured grids with ghost cells for parallelism) are different from `crd-geometry-mesh`'s triangle-soup BVH model. Solvers are stiff and require specific preconditioners not in eylem.

**Slot:** after `crd-hesap` close (3.1.6).

**Dependencies:** `crd-hesap-sparse`, `crd-hesap-iterative`, `crd-hesap-ode`, `crd-sdf` (for level-set methods), `crd-geometry-mesh` (for surface meshes / boundary representation).

**Reference reading:** Versteeg & Malalasekera "An Introduction to CFD" (2007); Ferziger, Perić & Street "Computational Methods for Fluid Dynamics" (2020); OpenFOAM architecture; SU2 architecture; Wilcox "Turbulence Modeling for CFD" (2006).

### 3.1.11 — `crd-estimation` + `crd-control` (aerospace + robotics)

**Scope:**
- `crd-estimation` — Kalman filter, Extended Kalman Filter (EKF), Unscented Kalman Filter (UKF), particle filter, factor graphs, SLAM substrate.
- `crd-control` — PID library, LQR (linear-quadratic regulator), MPC (model predictive control), optimal control trajectory optimization, robust control (H∞), path planning (RRT, RRT*, PRM, A*).

**Why separate substrates:** estimation reads sensor data → state; control consumes state → actuator commands. Different consumers (estimation feeds eylem v9 differentiable, control feeds eylem joints).

**Slot:** after `crd-hesap` close (3.1.6). Can land in parallel with `crd-cfd` or sequentially.

**Dependencies:** `crd-hesap-dense`, `crd-hesap-opt`, `crd-hesap-stats`, `crd-eylem` (for plant model in MPC), `crd-geometry-spatial` (for path planning).

**Reference reading:** Anderson & Moore "Optimal Filtering" (1979); Thrun, Burgard & Fox "Probabilistic Robotics" (2005); Stengel "Optimal Control and Estimation" (1994); LaValle "Planning Algorithms" (2006).

### 3.1.12 — `crd-fea` (engineering FEA — static + modal + buckling + fatigue)

**Scope:** static structural FEA (linear + nonlinear materials, contact), modal analysis (natural frequencies + mode shapes), buckling analysis, fatigue analysis (S-N curves, Goodman/Soderberg), linear elasticity → plasticity → hyperelasticity material models, thermal-structural coupling.

**Why separate from eylem v7 FEM:** eylem v7 is *dynamic* time-domain FEM (collision-coupled, real-time). Engineering FEA is *offline* steady-state or modal analysis (industry pattern from ANSYS / Abaqus / Nastran). Different solvers, different consumer, different correctness bar (engineering FEA needs convergence proofs and mesh-independence studies).

**Slot:** after `crd-hesap` and `crd-cfd` close (uses similar sparse linear-algebra paths).

**Dependencies:** `crd-hesap-direct` (sparse direct solver), `crd-hesap-eig` (modal analysis), `crd-geometry-mesh` (FEA mesh), `crd-eylem` (only for shared collider primitives).

**Reference reading:** Bathe "Finite Element Procedures" (2014); Hughes "The Finite Element Method" (2000); Zienkiewicz et al. "The Finite Element Method" set (2013).

### 3.1.13 — `crd-cam` (manufacturing — toolpath generation)

**Scope:** 3-axis + 5-axis milling toolpaths (pocketing, contouring, parallel finishing, scallop, swarf machining), turning toolpaths, additive manufacturing slicing (FDM, SLA, SLS, DMLS), G-code output with configurable post-processors per machine controller, simulation of material removal / deposition, sheet metal (bend allowance, flat patterns), PCB stack-up / routing.

**Why separate substrate:** CAM is a fundamentally different workflow from CAD (manufacturing engineer vs. design engineer). Toolpath generation has its own algorithms (offset curve, medial axis, voronoi-based finishing).

**Slot:** after `crd-brep` (3.1.8) and `crd-cad-feature` (3.1.9) — needs both the geometry and feature data.

**Dependencies:** `crd-brep`, `crd-cad-feature`, `crd-geometry-mesh`, `crd-geometry-decomposition` (V-HACD-class).

**Reference reading:** "CNC Programming Handbook" (Smid 2008); FreeCAD Path Workbench architecture; OpenSCAM / CAMotics architecture.

### 3.1.14 — `crd-ml-inference` (neural network inference + differentiable bridge)

**Scope:** ONNX runtime integration, neural network inference on GPU (via `crd-rhi` compute), differentiable programming layer (extends `crd-hesap-autodiff`), use cases: in-game AI / NPC behavior, denoising for RT (OIDN / NRD), super-resolution (DLSS-class), content generation (text-to-mesh, text-to-texture), animation (motion-matching ML), audio (noise reduction, source separation).

**Why a separate substrate:** ML inference has explicit memory layout requirements (tensor strides), specific kernel patterns (convolution, attention), and integration touchpoints (ONNX format, custom-op registration). Bundling into `crd-hesap` would bloat the math substrate with ML-specific concerns.

**Slot:** after `crd-hesap-autodiff` lands (Phase 3.1.6).

**Dependencies:** `crd-hesap-tensor`, `crd-hesap-autodiff`, `crd-rhi` (for GPU compute kernels).

**Reference reading:** ONNX specification; PyTorch internals; ggml / llama.cpp architecture (CPU inference); NVIDIA TensorRT design.

### 3.1.15 — `crd-procgen` (procedural content generation)

**Scope:** noise primitives beyond formulary (Perlin, Worley, Voronoi, simplex), Wave Function Collapse (WFC), L-systems (plants, fractals), terrain generation (erosion sim, hydrology), city/building generation, Substance-style procedural materials (node-based texture synthesis, tile-able, parametric).

**Why a substrate:** content generation is cross-cutting (games / cinematic / CAD test data / scientific visualization). Belongs at substrate tier, not bolted onto editor.

**Slot:** after `crd-geometry-mesh-processing` (Phase 3.1.7 v7) — uses mesh ops as building blocks.

**Dependencies:** `crd-geometry-primitives` (formulary already), `crd-geometry-mesh-processing` (remesh / subdivide / repair), `crd-hesap-stats` (random distributions).

**Reference reading:** Ebert et al. "Texturing & Modeling: A Procedural Approach" (2003); Gumin "Wave Function Collapse" (2016); "Algorithmic Beauty of Plants" (Prusinkiewicz & Lindenmayer 1990); "Texturing for Games" (various).

### 3.1.16 — `crd-sciviz` (scientific visualization)

**Scope:** isosurface extraction (Marching Cubes + Dual Contouring; partially in `crd-geometry-mesh-processing` v7 but with sciviz-specific quality knobs), streamline integration (vector fields via RK4), pathlines + streaklines, slice plane rendering, vector field arrows + LIC (line integral convolution), color mapping with perceptually uniform ramps (viridis, plasma, magma, cividis), in-engine 2D/3D plotting (time series, scatter, surface plots, contour), annotation + measurement (distance / angle / area on 3D models), comparison views.

**Why a substrate:** engineering / scientific workflows live in 2D plots as much as 3D viewports. Bundling plotting into `crd-ui` (Phase 5) is wrong — sciviz quality is the FEA / CFD validation tool, and ships before the editor.

**Slot:** after `crd-cfd` (3.1.10) and `crd-fea` (3.1.12) — the consumers are the first users.

**Dependencies:** `crd-geometry`, `crd-hesap-stats` (for histogram / box-plot), `crd-renderer`.

**Reference reading:** ParaView architecture; VTK design; "The Visualization Toolkit" (Schroeder, Martin, Lorensen 2006).

## 4. New application phases

### 3.2 — Animation (flesh out the placeholder)

**Scope:** skeletal animation (linear blend skinning + dual quaternion skinning), animation state machines + blend trees, IK (FABRIK two-bone + full-body IK), mocap retargeting, procedural animation (look-at, foot-IK, lean, eye tracking), morph targets / blendshapes, motion matching (modern AAA pattern, replaces hand-authored state machines), facial animation (FACS + ML-driven), crowd simulation (RVO-based steering, behavior trees).

ADR-0021 already exists with the architectural shape. Phase 3.2 doc gets the slice list.

### 3.5 — Modern rendering pipeline prologue (amendment to existing Phase 3.5)

**Scope addition (Phase 3.5 v0 prologue, before PBR work):**
- **Mesh shaders** (`VK_EXT_mesh_shader` / DX12 mesh shaders) — replaces vertex-shader / tessellation chain for fine-grained per-meshlet culling and LOD. Hand-in-hand with:
- **Visibility buffer rendering** — Nanite-style; record visibility (mesh+triangle id) to a buffer, do material shading deferred. Variant of deferred rendering with one G-buffer-equivalent slot.
- **GPU-driven culling** — frustum + occlusion culling on compute; `VkDrawIndirectCount` for variable draw lists.
- **Work graphs** (DX12 Ultimate / `VK_EXT_work_graphs` proposal) — replaces dependency-graph dispatching with self-feeding compute pipelines.
- **Variable rate shading (VRS)** — per-tile/per-primitive shading rate to save GPU work where pixels don't need full-rate.

**Slot:** before existing Phase 3.5 PBR slices. PBR depends on the shading pipeline, so mesh-shader/visibility-buffer infrastructure lands first.

This is **an amendment to the existing Phase 3.5**, not a new phase — the existing PBR/IBL/CSM/SSS/NPR/area-lights scope all stays. The mesh-shader work is the new prologue. Phase 3.5 sub-numbering becomes: `3.5.0` modern pipeline, `3.5.1` PBR, `3.5.2` IBL, etc.

### 3.4 — Audio (flesh out the placeholder)

**Scope addition** beyond what existing placeholder hints at:
- DSP framework (FIR/IIR/biquad/polyphase, already in `crd-hesap-dsp`)
- Spatial audio (HRTF, ambisonics)
- **Ray-traced audio** (occlusion + reflection paths via `crd-geometry-bvh` acoustic raycasts) — research-grade engine differentiator
- DAW plugin host (VST3, AU, LV2)
- Speech synthesis / voice (consumes `crd-ml-inference`)
- ML audio (noise reduction, source separation — consumes `crd-ml-inference`)

ADR pending; Phase 3.4 doc gets the slice list.

## 5. New integration phases

### Phase 6 — Platform expansion (reuse the folded "Native physics" slot)

The Phase 6 slot is currently marked "folded into 3.1 (eylem native from day 1)". Reuse the number for platform expansion:

**Scope:**
- **Console support** — PS5, Xbox Series, Switch (platform abstraction work; each console requires its own NDA-gated SDK; substrate layer abstracts where possible).
- **Mobile** — iOS (Metal backend for `crd-rhi`), Android (Vulkan + adaptive resolution).
- **Web** — WebGPU backend for `crd-rhi`; emscripten / WASM toolchain.
- **VR/AR** — OpenXR integration; head/hand tracking; per-eye render passes.
- **HPC / cluster computing** — MPI integration for distributed simulation (CFD multi-node, FEA partitioned solve, multi-player physics islands).

**Slot:** after Phase 3.x rendering polish (Phase 3.9 close); platform-portability work amortises across the established rendering stack.

### Phase 8 (existing) — Domain modules (expand the scope description)

The existing Phase 8 is a placeholder for "robotics, aerospace, advanced math, cinematic, procgen". Expand to cover the *application-layer* domains that consume the new substrates:

- **Robotics** (consumes `crd-eylem` + `crd-estimation` + `crd-control` + `crd-geometry-spatial`): URDF/SDF/MJCF importers, ROS2 bridge, ROS-Industrial pattern.
- **Aerospace** (consumes `crd-eylem-aero` + `crd-estimation` + `crd-control`): VLM solver, panel methods, orbital propagation, atmosphere models (US Standard Atmosphere, COESA-76, NRLMSISE-00).
- **Cinematic** (consumes `crd-eylem-cine` + animation + rendering): mocap pipeline, animation retargeting, cinematic camera systems, motion-blur policies.
- **Medical visualization** (consumes `crd-sciviz` + `crd-rhi` volumetric): DICOM import, transfer-function authoring, multi-modal registration (CT+MRI overlay), surgical planning.
- **Manufacturing** (consumes `crd-brep` + `crd-cad-feature` + `crd-cam` + `crd-fea`): full CAD/CAM/CAE workflow integration.
- **Scientific computing** (consumes `crd-hesap` + `crd-ml-inference` + `crd-sciviz`): REPL workflows, notebook integration (the `.cnb` format from ADR-0065).

## 6. Sequencing strategy

The point of capturing this expansion now (rather than letting it accrete) is that it lets us **plan the substrate ordering correctly**. Key sequencing observations:

1. **`crd-hesap` is foundational to CFD / FEA / control / ML / estimation.** It must close before any of those start.
2. **`crd-brep` enables CAD / CAM / engineering visualization.** It's the next big substrate after geometry close.
3. **CFD and FEA share linear algebra paths.** They can land in parallel post-hesap, sharing the sparse-direct / sparse-iterative kernels.
4. **`crd-ml-inference` enables modern denoising / SR / content generation.** Should land before Phase 5 RT (ML denoiser for RT effects) and before Phase 3.4 ML audio.
5. **`crd-procgen` and `crd-sciviz` consume `crd-geometry-mesh` / `crd-geometry-mesh-processing`.** They land after Phase 3.1.7 v7 ships.
6. **Phase 6 platform expansion comes late.** Console / mobile / web / VR amortise across a stable rendering stack; don't kick off until 3.x rendering is polished.

The execution order suggested by these constraints (NOT a commitment — just a sensible default sequence):

```
Phase 3.1.7 close (current)
  → Phase 3.1.8 crd-brep (NURBS / B-rep)
  → Phase 3.1 resume (eylem v1c onward — broadphase + narrow + manifold)
  → Phase 3.1.5 crd-sdf (interleaved as before)
  → Phase 3.1.6 crd-hesap (sequential after eylem close)
  → Phase 3.1.9 crd-cad-feature
  → Phase 3.1.10 crd-cfd       (parallel)
  → Phase 3.1.11 crd-estimation + crd-control
  → Phase 3.1.12 crd-fea
  → Phase 3.1.13 crd-cam
  → Phase 3.1.14 crd-ml-inference
  → Phase 3.1.15 crd-procgen
  → Phase 3.1.16 crd-sciviz
  → Phase 3.2 animation (flesh out)
  → Phase 3.3 crd-font
  → Phase 3.4 audio (flesh out)
  → Phase 3.5 modern rendering pipeline + PBR
  → Phase 3.6 atmosphere + volumetrics
  → Phase 3.7 post-processing
  → Phase 3.8 GPU-driven rendering + particles + water
  → Phase 3.9 GI pre-RT
  → Phase 4 extensibility + networking
  → Phase 5 RT + UI + advanced rendering
  → Phase 6 platform expansion
  → Phase 7 editor
  → Phase 8 domain modules
```

Total horizon: **5–8 years** of execution if every phase ships in full. This is the long-term roadmap; the engine ships *useful* increments at every substrate close (each substrate has multi-domain consumers ready to bind).

## 7. Out of scope (deliberate exclusions)

These were considered and deliberately deferred:

- **Quantum simulation** — not in any of the eight stated domains.
- **Symbolic math (Mathematica-class)** — `crd-hesap` is numerical; symbolic would be a wholly different module (`crd-symbolic`), defer unless a consumer surfaces.
- **Game-specific genres (RPG inventory, FPS netcode features)** — game-genre features live at the application layer above `crd-scene` / `crd-eylem` / etc., not in the engine substrate. They can be example projects in `runtime/` but not engine phases.
- **Hardware-specific optimizations (PS5 SSD streaming, Xbox sampler feedback)** — these slot into Phase 6 platform expansion, not as separate phases.
- **DAW-specific features (mixer UI, MIDI sequencing UI)** — application-layer; substrate (`crd-audio` + `crd-hesap-dsp` + DAW plugin host) is in scope, UI is a `crd-ui` consumer.

## 8. Consequences

**Positive:**
- The roadmap captures the full long-term vision; new contributors / future-me can see where the engine is heading.
- Substrate boundaries are pre-planned, reducing the risk of bolt-on integration later (the kind of debt that killed Phase 3.1's original ADR-0018 PhysX-wrap pattern).
- Cross-domain consumers can be planned with substrate dependencies in mind (eylem v9 differentiable consumes `crd-ml-inference`'s autodiff bridge; CFD consumes `crd-hesap-sparse`; CAM consumes `crd-brep` + `crd-cad-feature`; etc.).

**Negative:**
- The expansion is large (~5–8 years of execution). Risk: planning overhead vs. execution pace. Mitigation: substrate stubs are lightweight (~100–200 LOC of doc per new phase); full plans land when work actually starts on each phase.
- Some substrates (CAD/CAM specifically) require domain expertise the team doesn't yet have. Mitigation: defer those phases until the team has either grown into them or found a consultant.

**Neutral:**
- The roadmap numbering becomes denser (`3.1.x` substrate family now has 16+ entries). This is OK — the substrate-first pattern naturally produces many small peer modules; the alternative (one monolithic engine) is worse.

## 9. References

- ADR-0062 (`crd-eylem` physics architecture — substrate pattern)
- ADR-0064 (`crd-sdf` substrate architecture — substrate pattern)
- ADR-0065 (`crd-hesap` numerical substrate — substrate pattern, two-layer API design)
- ADR-0076 (`crd-geometry` substrate architecture — substrate pattern, query facade, NaN/Inf contract, viz companion)
- `docs/PRINCIPLES.md` (substrate-first, modular by default, single-path)
- `docs/ROADMAP.md` (the hub — this ADR's expansion is reflected there)

## 10. Revisit triggers

This ADR's substrate list is **aspirational, not committed**. The order and exact scope of each new phase will be revisited when:
- A specific phase is about to start (a research dossier lands first, then the phase-specific ADR with the locked scope).
- A consumer surfaces that needs a substrate sooner than planned (e.g. if a CAD partner appears, `crd-brep` jumps the queue).
- A substrate proves harder than expected and needs to be split (e.g. `crd-brep` might split into `crd-brep-core` + `crd-brep-import` + `crd-brep-edit`).

The expansion list is the **superset**; execution carves out the **subset** that ships next.
