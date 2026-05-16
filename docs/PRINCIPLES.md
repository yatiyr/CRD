# Cerid Engine — Engineering Principles

> Non-negotiable. Every slice respects them; deviations are explicit,
> justified, and recorded as an ADR under `docs/decisions/`.
>
> Read this every session. It's short and it's the architectural compass.

## Identity

Cerid is a **general-purpose C++20 real-time engine substrate**. Games are
one consumer; **simulation (incl. robotics), medical visualization,
DAW-class creative tools, and offline cinematic pipelines** are equal-class
consumers. The architecture serves all of them; no domain is privileged.

| Domain                       | Why Cerid fits                                                                |
| ---------------------------- | ----------------------------------------------------------------------------- |
| Games                        | Real-time renderer, physics, animation, scripting, scene/entity model         |
| Simulation (incl. robotics)  | Deterministic option, swappable physics, math depth, sensor/actuator hookable |
| Medical visualization        | High-quality rendering, large-volume data, deterministic playback             |
| DAWs / creative tools        | Custom retained-mode UI, node editors, plugin/script extensibility, low jitter |
| Offline cinematic pipelines  | Same render path, scriptable, deterministic, scene-graph aware                |

## Principles

- **Modular by default.** Every subsystem is a separable module with a clear
  public surface. A DAW build that doesn't need physics/animation must be
  able to omit them at link time.
- **Vertical slice over horizontal completeness.** Walk a small path
  end-to-end before widening. The first triangle gate is a permanent example.
- **Authoring text, runtime binary.** Human-edited data is text (TOML / JSON
  / glTF). Engine-consumed data is cooked binary. Runtime never imports
  source assets.
- **One-way module dependencies.** Cycles are bugs. The dependency graph is
  reviewed at every module boundary change.
- **Real workload before optimization.** No SIMD, fiber, GPU-allocator, ECS
  rewrite, or render-path swap without a measured baseline and target.
- **API stable across backends.** Public surfaces (RHI, physics, audio,
  render path) are designed assuming multiple implementations even when
  only one exists. Vendor types do not leak.
- **Tak-çıkar (plug-out) third-party.** Where Cerid uses an external
  (glslang/shaderc, spirv-reflect, ImGui, toml++), the integration is a
  backend behind a Cerid-owned interface. Core simulation surfaces
  (renderer, physics/eylem, audio) are Cerid-native — no vendor wraps.
- **Determinism is a first-class option.** Not the default, but reachable:
  fixed-step physics, deterministic random, replay-friendly event log.
- **Every shipped slice ends green on Debug + Release + ASan.** Three
  flavours. No exceptions.
- **The engine is allowed to be slow before it is allowed to be wrong.**
- **Units live at the API surface, raw scalars live in the inner loop.**
  Cerid runs a **two-layer typed architecture** (formalised at Phase 3.1.7.5
  close; ADR-0078 §5):

  - **Upper layer — typed.** Every public API, every ECS component field,
    every config key, every cross-module function signature, every cooker /
    loader public surface, every UI display path uses `Quantity<D, T>`
    (`Length<T>` / `Mass<T>` / `Force<T>` / `Velocity<T>` / `Torque<T>` / …).
    The dimensional check happens here, at compile time, on the SI value.
  - **Lower layer — raw.** SIMD kernels, math primitives (Vec / Mat / Quat
    inner ops), numerical algorithms (BLAS / LAPACK / closest-point /
    raycast bodies / Möller-Trumbore / Vatti clipper / etc.), GPU command-
    buffer writes, file and wire byte buffers, on-disk asset payloads
    operate on raw `f32` / `f64`. No dimensional tag rides through an
    `_mm256_*` intrinsic or a `vkCmdPushConstants` call.

  The two layers meet at **the API surface**, and only there. Crossings
  use `.value`, `to_raw_vec` / `from_raw_vec` (constexpr — compile away),
  or a documented strip-compute-retag wrapper. Each crossing is one line
  with a one-line comment naming the boundary (e.g. `// GPU push constant
  — raw f32 by ADR-0078 §3 D22`).

  **The internal canonical unit is SI base** (meters / kg / seconds /
  radians / kelvin / ampere / candela / mole) at the typed layer. Asset /
  file / UI boundaries normalise to SI at load (`get_length("65_mph")` →
  `Velocity32{29.0576F}`); runtime never sees non-SI. The user-facing
  display layer (`UnitPreferences` + 11 discipline presets) translates SI
  back to authoring-convention strings for the UI only.

  **Precision tier (f32 / f64) is orthogonal to the dimensional type.**
  Same `Length<T>`; games + runtime pick `f32`; aerospace large-world +
  CAD micrometer + scientific pick `f64`. Zero-overhead layout
  (`sizeof(Quantity<D, T>) == sizeof(T)`) means the precision choice is
  the only one that affects storage; the dimensional tag is compile-time
  metadata.

  **There is no opt-out at the upper layer.** Bare-float-for-physical-
  quantity at any API surface is a code-review block and a CI-guard
  violation (`crd-no-untagged-physical-numeric`). The lower layer stays
  raw on purpose — pretending an `_mm256_load_ps` carries a `Length`
  dead-ends in the first lane shuffle.

  → `crd-units` (Phase 3.1.7.5 ✅ CLOSED 2026-05-15); ADR-0078 §1-§5.

## Architectural Cornerstones (pinned)

These come from accepted ADRs and are not re-litigated in routine sessions.
If circumstances genuinely change, open a new ADR or escalate to `@heavy`.

- **Render path:** Renderer v1 ships **Clustered Forward+** behind an
  `IRenderPath` interface. Deferred and Visibility-Buffer paths land later
  as additional implementations, not replacements.  → ADR-0016
- **Culling:** Frustum culling in v1 → BVH-accelerated when scene grows →
  Hi-Z occlusion later. Per-light culling is part of clustered Forward+. →
  ADR-0017
- **Scene + ECS:** **Hybrid model.** SoA component storage for cache-friendly
  iteration; hierarchical scene tree for traversal/authoring. Not pure ECS,
  not naive scene graph. → ADR-0020
- **UI in the scene tree:** Godot-style. Spatial nodes (3D) and Control
  nodes (UI) coexist as children of the same scene root. → ADR-0020
- **Physics — Cerid-native (eylem) from day 1.** No third-party wrap. The
  `crd-eylem` module is built deterministic-by-construction (compile +
  runtime FP contract), ECS-native, fiber-jobified, multi-domain (games
  + robotics + medical + cinematic + DAW), templated 2D + 3D from a
  single substrate, and GPU-extensible. → ADR-0062, ADR-0063
  (supersedes ADR-0018; phase plan: `docs/phases/phase-3.1-eylem.md`;
  research: `docs/research/cerid-eylem.md`)
- **Authoring vs runtime:** Configs and scenes authored in TOML; scenes
  cooked to binary for runtime. → ADR-0012, ADR-0013
- **ImGui's role:** Debug-only forever. After `crd-ui` ships, ImGui never
  grows into editor or game surfaces. → ADR-0023
- **Reference counting split:** Generic intrusive ref-counting in
  `crd-memory`. Resource-facing shared references in `crd-resources`. →
  ADR-0014
