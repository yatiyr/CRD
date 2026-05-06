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
| 3.0   Scene / ECS foundation    | ⏳ ready to start | `docs/phases/phase-3.0-scene-ecs.md` (8-layer slot architecture; 14 slices; ADRs 0049–0057 all locked 2026-05-06) |
| 3     Simulation + visual effects | ⏳             | `docs/phases/phase-3-simulation.md` (3.0 scene/ECS → 3.1 physics → 3.2 animation → **3.3 crd-font** → 3.4 audio → **3.5** PBR+IBL+CSM+SSS+NPR+area lights → **3.6** sky atmosphere+volumetric fog+clouds+god rays+aurora → **3.7** bloom+GTAO+SSR+TAA+DoF+motion blur+upscaling → **3.8** GPU particles+ocean+decals+indirect rendering → **3.9** SSGI+DDGI+radiance cascades+lightmap baking; ADR-0047) |
| 5     RT + advanced rendering   | ⏳               | `docs/phases/phase-5-ui-rendering.md` (crd-ui + node editor + HybridRenderPath: RT AO/reflections/GI + denoiser; ADR-0046) |
| 4     Extensibility + Networking | ⏳              | `docs/phases/phase-4-extensibility.md` (4.0 C++ scripting, 4.1 advanced math, 4.2 networking; ADR-0034, ADR-0035) |
| 5     RT + UI + advanced rendering | ⏳            | `docs/phases/phase-5-ui-rendering.md` (HybridRenderPath: BLAS/TLAS, RT AO/reflections/shadows/GI, denoiser; crd-ui; node editor; ADR-0046) |
| 6     Native physics            | ⏳               | `docs/phases/phase-6-native-physics.md`  |
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

### 6–12 months (mid-2026 to early 2027)
- Resource system + asset cooker (Phase 2.6): handle table, ref-counted assets, hot-reload, cooked binary format ✅ SHIPPED
- Asset import bootstrap (Phase 2.7): TextureResource + MeshResource + glTF + material foundation + GPU upload + **crd-meshgen** + **crd-sandbox**; ADRs 0042–0043, 0045, 0048
- Material completion (Phase 2.8): per-material PSO cache + pass-keyed shader variants + depth-only prepass; ADRs 0044, 0046
- Scene/ECS foundation (Phase 3.0): hybrid hierarchy + SoA components + TOML → cooked binary
- Physics (Phase 3.1): PhysX 5 backend + scene integration + fixed-step
- Animation (Phase 3.2): skeletal + blend trees + IK
- Font rendering (Phase 3.3): `crd-font` — MTSDF, HarfBuzz, billboard + 3D text; ADR-0047

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

- **Cerid-native physics backend (Phase 6)**: alongside PhysX

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
