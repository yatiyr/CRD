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
| 3.1.7 **crd-geometry substrate** | ⏳ **next-active** — kicks off immediately after Phase 3.1 v1b sweep PASS (ADR-0076 §12 amendment 2026-05-11 pivoted the slot from "after 3.1.6 hesap" to "before 3.1 v1c"; full 29-slice phase ships, no narrow subset; eylem v1c/v1d/v1d-mesh + sdf v2 consume from day 1) | `docs/phases/phase-3.1.7-geometry.md` (11 sub-modules / 29 slices / ~15.8 KLOC engine + ~5 KLOC editor + ~4 KLOC cooker-emitted GLSL/HLSL / ~5–7 months: v0 primitives + v0e iq-formulary substrate (smin/domain ops + `crd::math::simd::reduce_argmax_with_lex_tiebreak` substrate primitive) → v1 BVH (binned SAH + Catto 2019 refit + quad-BVH) + v1g BVH4 SIMD ray-vs-4-AABB → v2 GJK+EPA+SAT → v3 Quickhull → v4 mesh queries (closest-point + Möller-Trumbore + Jacobson 2013 winding number) + v4g per-leaf SIMD Möller-Trumbore over 8 triangles → v5 KD-tree+octree+R-tree+spatial hash + scene IComponentIndex bring-up → v6 polygon ops (Vatti + CDT + Bentley-Ottmann) → v7 mesh processing (QEM + Loop subd + remesh + repair + Taubin) → v8 Delaunay + Voronoi 2D/3D → v9 GPU LBVH (Karras 2012) + V-HACD editor-tier + REPL + v9e GLSL/HLSL `crd-geometry-shader-helpers` cooker-emitted output (ULP-conformance-tested against C++ ref); ADR-0076 (Accepted 2026-05-11; §12 amendment 2026-05-11) + research: `docs/research/cerid-geometry.md` + `docs/research/cerid-geometry-supplement.md`; deferred-refactor pattern OBSOLETED by §12 — eylem v1c/v1d + sdf v2 consume from day 1) |
| 3     Simulation + visual effects | ⏳             | `docs/phases/phase-3-simulation.md` (3.0 ✅ → 3.1 eylem → 3.2 animation → **3.3 crd-font** → 3.4 audio → **3.5** PBR+IBL+CSM+SSS+NPR+area lights → **3.6** sky atmosphere+volumetric fog+clouds+god rays+aurora → **3.7** bloom+GTAO+SSR+TAA+DoF+motion blur+upscaling → **3.8** GPU particles+ocean+decals+indirect rendering → **3.9** SSGI+DDGI+radiance cascades+lightmap baking; ADR-0047) |
| 5     RT + advanced rendering   | ⏳               | `docs/phases/phase-5-ui-rendering.md` (crd-ui + node editor + HybridRenderPath: RT AO/reflections/GI + denoiser; ADR-0046) |
| 4     Extensibility + Networking | ⏳              | `docs/phases/phase-4-extensibility.md` (4.0 C++ scripting, 4.1 advanced math, 4.2 networking; ADR-0034, ADR-0035) |
| 5     RT + UI + advanced rendering | ⏳            | `docs/phases/phase-5-ui-rendering.md` (HybridRenderPath: BLAS/TLAS, RT AO/reflections/shadows/GI, denoiser; crd-ui; node editor; ADR-0046) |
| ~~6     Native physics~~ | folded into 3.1 (eylem ships native from day 1) | `docs/phases/phase-3.1-eylem.md` |
| 7     Editor                    | ⏳               | `docs/phases/phase-7-editor.md`          |
| 8     Domain modules            | ⏳               | `docs/phases/phase-8-domain-modules.md` (robotics, aerospace, advanced math, cinematic, procgen) |

Legend: ✅ shipped · 🚧 active · ⏳ planned · ❌ blocked

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

### Already achieved (as of 2026-05)
- Stable Vulkan backend, GPU allocator strategy in place
- Shader system v1 with hot-reload, variant keys, pipeline handoff
- Renderer v1a–i (Clustered Forward+, frame graph, descriptor system, material system)
- **Jobs system v1 complete** (v1a–v1k): fiber pool, work-stealing deque, ABA-safe scheduler, counter/wait, worker pool, public API (`run`/`wait`/`make_job`/`parallel_for`), 41-byte SBO, per-frame arena, crd-app wired
- ImGui debug tooling; crd-config TOML substrate
- **Resource system v1 complete** (v1a–v1g): handle table, ref-counting, sync/async/streamed loading, typed loaders (shader, material), hot-reload with atomic payload swap + mtime polling + callbacks, 2Q LRU eviction, memory budget, pin/unpin
- **Asset import bootstrap** (Phase 2.7): TextureResource + MeshResource + glTF + MaterialTemplate/Instance + GpuUploader + crd-meshgen + crd-sandbox
- **Material GPU wiring** (Phase 2.8): per-material pipeline cache + multi-pass ForwardRenderPath + depth-only prepass + sandbox 3D rendering + glTF demo asset bundle + unified Asset Browser
- **`crd-scene` architecture locked** (2026-05-06, ADRs 0049–0057): 8-layer slot-shaped ECS — Entity/SlotMap, Archetype+SparseSet hybrid storage, first-class Relations, Query/System/Schedule, Component index framework (ChangeDetect+AsyncAware ship now; History/SpatialBVH/GpuResident reserve API), reserved L6–L8 slots (Replication, Scripts, Reflection)
- **`crd-scene` v1a–v1m shipped** (2026-05-06 → 2026-05-08): EntityId+SlotMap, ComponentRegistry+trait grammar, ArchetypeChunkStorage+SparseSetStorage hybrid behind IStorageBackend, mixed-backend chunk visitor, six built-in relations with reverse indexes/acyclic/on-target-destroyed traits, Query DSL, System+Schedule+Commands (7-phase fixed), IComponentIndex framework with ChangeDetect/AsyncAware live + 5 reserved no-op shells, Transform+TransformPropagation (cross-domain robust, bit-exact deterministic), SceneResource+SceneLoader (FourCC `'SCEN'`) + cook_scene cooker (TOML→SCEN), and the **Öbek system (ADR-0058 / FourCC `'OBEK'`)** — cooked entity-graph templates with composition (nested öbek refs), variation (`extends` chain with cycle detection), override patches (cook-time + runtime, deepest-wins), all three InheritPolicy values including transparent CoW (per-slot ownership, content-hash dedup, refcount eviction), revert at four granularities, unpack semantics, and AAAA-tier reservations (BatchHints / BatchInstanceTag / instantiate_obek_batch + OBAT chunk for GPU instancing in Phase 3.5+, ObekEntityGuid for replication/replay)
- **Memory subsystem v2** (2026-05-07, Detour D-001 closed): production-grade `TlsfAllocator` (arbitrary alignment, try-allocate, ~7 KB metadata, ASan-validated) + `GrowablePoolAllocator` (page-pooled fixed-size aligned blocks, O(1) alloc/free); `ChunkAllocator` refactored on top of GrowablePool (closed v1c1 O(N) free debt); `World`/`ArchetypeGraph`/`SparseSetStorage` route every byte through one root `IAllocator`

### 6–12 months (mid-2026 to early 2027)
- Resource system + asset cooker (Phase 2.6): handle table, ref-counted assets, hot-reload, cooked binary format ✅ SHIPPED
- Asset import bootstrap (Phase 2.7): TextureResource + MeshResource + glTF + material foundation + GPU upload + **crd-meshgen** + **crd-sandbox**; ADRs 0042–0043, 0045, 0048
- Material completion (Phase 2.8): per-material PSO cache + pass-keyed shader variants + depth-only prepass; ADRs 0044, 0046
- Scene/ECS foundation (Phase 3.0): hybrid hierarchy + SoA components + TOML → cooked binary
- **Eylem — Cerid-native physics (Phase 3.1)**: built from day 1 (no PhysX wrap step). **⏸ v1b cluster closes (sweep 2026-05-11), then PAUSES for Phase 3.1.7 `crd-geometry` (full 29-slice phase, ~5–7 months); v1c resumes after geometry close, consuming `crd-geometry-bvh`/`-convex`/`-mesh` from day 1 with no deferred-refactor. Pivot per ADR-0076 §12 amendment 2026-05-11.** Module split: `crd-eylem` substrate + `crd-eylem-rigid3d` (active; BodyPool + ColliderPool + EylemSystem + RigidBodyInterpolationSystem shipped) + `crd-eylem-rigid2d` / `-soft` / `-articulation` / `-vehicles` / `-ccd` / `-fem` / `-gpu` / `-diff` (planned) + `crd-eylem-aero` (planned per ADR-0073) + `crd-eylem-cine` (planned per ADR-0074) + `crd-eylem-viz` (companion bridging eylem to crd-draw VisualizerRegistry). v0 ✅ + v1a ✅ + v1a-draw d0..d4 ✅ + v1b-a/b/c/d ✅ + v1a-material a/b/c/d ✅ + v1b-e 🚧 (sweep running). v1 = rigid 3D substrate + crd-draw substrate + Material substrate + 5-tier collision filter + deferred ECS event-stream callbacks + force-field substrate (9 formulas) + conservation-law CI + deterministic snapshot/replay across 9 CI configs. v2-v9 stage 2D specialisation, XPBD soft/cloth/rope, articulations + cinematic bridge, vehicles + 5 friction models, CCD + Featherstone + URDF/SDF/MJCF + nonsmooth Newton + sensors + aerospace substrate, FEM + HuntCrossley, GPU acceleration + Newton restitution, differentiable + cross-engine bench. **8 ADRs Accepted** (0062 / 0063 / 0066 / 0067 / 0068 / 0069 / 0075 + 0066 §19.2.1 extension) + **5 ADRs reserved Planned** (0070 / 0071 / 0072 / 0073 / 0074 mint when their research dossiers ship at slice time). 4 research dossiers shipped; 6 more planned for Wave 2/3 ADRs. Supersedes ADR-0018 + Phase 6.
- **`crd-sdf` substrate (Phase 3.1.5)**: signed-distance-field module consumed by eylem (mesh colliders + closest-point), font (MTSDF), renderer (DFAO/DFGI in 3.5+), audio (acoustic occlusion in 3.4), editor (CSG modelling in 7). 8 slices over ~5–6 wk: v0 analytic primitives → v1 dense 3D grid + CRDR → v2 mesh-bake (Jacobson 2013 winding-number sign + BVH closest-point, parallel) → v3 narrow-band sparse → v4 CSG + smooth-min → v5 GPU 3D-texture upload + GLSL helper → v6 cooker → v7 Marching Cubes extraction → v8 reserved (GPU baker / VDB / Dual Contouring). ADR-0064; deterministic per ADR-0063; plan: `docs/phases/phase-3.1.5-sdf.md`; research: `docs/research/cerid-sdf.md`. **Slot unchanged by ADR-0076 §12 (2026-05-11): still interleaved between eylem v2 and v3.** v2 mesh-bake now consumes `crd-geometry-mesh` + `crd-geometry-bvh` directly from day 1 (no narrow-version refactor — the original ADR-0064 §4 deferred-refactor is OBSOLETE since geometry ships before sdf).
- **`crd-geometry` substrate (Phase 3.1.7)** — ADR-0076 ✅ Accepted 2026-05-11 + **Amended 2026-05-11 §12** (sequence pivot: now executes BEFORE Phase 3.1 v1c instead of after Phase 3.1.6 hesap). **Status:** ⏳ next-active — kicks off immediately after Phase 3.1 v1b sweep PASS. Computational-geometry primitives + spatial-acceleration substrate. Peer module to `crd-math` / `crd-sdf` / `crd-hesap` (NOT bloated into `crd-math`). Multi-domain consumer list: eylem broadphase (BVH refit/build, v1c) + narrow phase (GJK + EPA, v1d) + mesh collider (closest-point + raycast on triangle mesh, v1d-mesh) + convex collider conditioning (v1c); crd-sdf v2 mesh-bake (winding-number test + BVH closest-point); crd-renderer Phase 3.5+ (frustum cull + occlusion BVH); crd-scene `SpatialBVHIndex` reserved shell (ADR-0053); crd-audio Phase 3.4 (acoustic ray-casts); crd-eylem-aero (ADR-0073, surface eval); crd-eylem-cine (ADR-0074, animated mesh queries); editor Phase 7 (V-HACD pipeline + selection + picking). Inherits ADR-0063 determinism contract (deterministic BVH SAH split tiebreak; deterministic GJK simplex update; deterministic-FP polygon predicates per Shewchuk 1997). 11 sub-modules: `-primitives` + `-bvh` + `-convex` + `-mesh` + `-spatial` + `-polygon` + `-mesh-processing` + `-delaunay` + `-gpu` + `-decomposition` + `-shader-helpers` (cooker-emitted GLSL/HLSL). Two-layer API mirrors crd-hesap: typed C++ Eigen-class for engine code + opt-in cooker/editor façade. **§12 amendment dissolves the deferred-refactor pattern** — eylem v1c/v1d/v1d-mesh + sdf v2 all consume `crd-geometry` from day 1, no ships-own narrow versions. Full **29 slices / ~15.8 KLOC engine + ~5 KLOC editor + ~4 KLOC cooker-emitted GLSL/HLSL / ~5–7 months**. Research dossiers locked: `docs/research/cerid-geometry.md` (11,523 words base) + `docs/research/cerid-geometry-supplement.md` (6,116 words); phase plan: `docs/phases/phase-3.1.7-geometry.md`.

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

- **Domain modules (Phase 8)**: robotics substrate (URDF, SE(3), ROS2 bridge), cinematic tools, procedural generation

- **Cerid-native physics (Phase 3.1, eylem)**: built from day 1 — no
  PhysX wrap step. Deterministic-by-construction, ECS-native,
  fiber-jobified, multi-domain (games + robotics + medical + cinematic
  + DAW), templated 2D + 3D, GPU-extensible. ADR-0062, ADR-0063;
  research: `docs/research/cerid-eylem.md`; plan:
  `docs/phases/phase-3.1-eylem.md`. Supersedes ADR-0018.

The goal: Cerid becomes a real-time substrate for interactive applications
across games, simulation, creative tools, and engineering — not "another Vulkan engine."

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
