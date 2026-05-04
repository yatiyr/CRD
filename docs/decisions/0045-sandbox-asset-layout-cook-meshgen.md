# ADR-0045 — Sandbox executable, asset source layout, cook workflow, crd-meshgen

**Status:** Accepted
**Date:** 2026-05-04
**Tags:** arch, sandbox, resources, cooker, build, meshgen

---

## Context

Phase 2.7 introduces real assets (TextureResource, MeshResource, GPU upload). Before implementation
begins, three structural decisions must be locked:

1. How to validate the full asset pipeline end-to-end as a real engine consumer (not just a smoke)
2. Where source assets live in the repository and how they get cooked
3. What to call and how to scope the procedural geometry generation module

---

## Decisions

### 1. `crd-sandbox` — canonical long-lived engine consumer

A new `crd-sandbox` target is added alongside the existing smoke executables. It is NOT a smoke — it does
not exit after one assertion. It is the engine's canonical consumer: a real application that uses
`crd-app::Application` + `LayerStack`, loads cooked assets via `ResourceManager`, renders via
`ForwardRenderPath`, and grows one capability per phase.

**Phase 2.7 scope:**
- `SandboxLayer` renders a loaded mesh with a loaded texture
- ImGui asset browser panel: lists cooked meshes/textures/materials, click to switch rendered asset
- Runtime parameter display (mesh triangle count, texture resolution, load time)
- `crd-meshgen` shapes available alongside loaded glTF assets

**Phase 2.8 adds:** shader switcher panel (PBR / toon / unlit using pass-keyed variants), material
parameter editor (roughness/metallic sliders), wireframe toggle, PSO state display

**Phase 3.0 adds:** scene hierarchy panel, entity list, transform inspector

**Scope contract:** crd-sandbox is always a demo of the current phase's features, nothing more. It must
not become a scratch pad. New capabilities enter only when the relevant phase ships.

### 2. Headless CI mode

`crd-sandbox --headless` (or env `CRD_SANDBOX_HEADLESS=1`) runs the full app loop but skips GPU calls,
validates CPU-side resource loading (all handles reach `Ready` state), then exits 0/1. This mode runs in
the headless CI matrix without a GPU. GPU rendering is verified manually before each phase ships.

### 3. `assets/source/` at repo root

Source assets (`.glb`, `.png`, `.mat.toml`) live at `assets/source/`. Subdirectories:
```
assets/source/
  meshes/       ← .glb files
  textures/     ← .png / .hdr files
  materials/    ← .mat.toml files
  LICENSES.md   ← one entry per asset: name, source URL, license, attribution
```

**Licensing rule:** CC0, Apache 2.0, or MIT only. No CC BY-ND or CC BY-NC. Attribution goes in
`LICENSES.md`, not in source code.

**Initial Phase 2.7 assets:**
| File | License | Source |
|------|---------|--------|
| `meshes/BoxTextured.glb` | CC0 | Khronos glTF-Sample-Assets |
| `meshes/Duck.glb` | Apache 2.0 | Khronos glTF-Sample-Assets |
| `meshes/Suzanne.glb` | CC0 | Blender Foundation |
| `textures/checker_512.png` | CC0 | Synthetically generated |
| `textures/bricks_512.png` | CC0 | ambientCG.com |

Phase 2.8 adds: `DamagedHelmet.glb` (CC BY 4.0, Khronos) — held until PBR shading lands so it looks right.

Cook output lives in `${CMAKE_BINARY_DIR}/assets/cooked/` — gitignored, never committed.

### 4. Cook workflow — `CRD_BUILD_SANDBOX` gate

```cmake
option(CRD_BUILD_SANDBOX "Build crd-sandbox and cook demo assets" ON)
```

- **Default ON:** developer builds always include the sandbox and cook demo assets.
- **CI sets `CRD_BUILD_SANDBOX=OFF`** for headless-only matrix runs — no cook step, no GPU binary.
- When ON, `cook_sandbox_assets` CMake custom target cooks `assets/source/` → `${CMAKE_BINARY_DIR}/assets/cooked/` before building `crd-sandbox`.
- The existing `CRD_COOK_ROOT` / `CRD_COOK_OUT` CMake variables drive this.

### 5. `crd-meshgen` — procedural geometry module

The procedural geometry module is named **`crd-meshgen`**, namespace **`crd::meshgen`**.

- Pure CPU-side geometry generation. No deps on `crd-rhi`, `crd-renderer`, or `crd-resources`.
- Dependencies: `crd-math`, `crd-containers`, `crd-memory` only.
- Output: `MeshData` struct with interleaved 48B/vertex data (pos + normal + uv0 + tangent) — same
  layout as `MeshResource`. Compatible with `GpuMeshUploader` directly.
- Geometry shaders are **not added** as a first-class engine feature. They are deprecated on AMD/RDNA,
  absent on Metal, and architecturally hostile (serialize pipeline output). CPU regeneration is correct
  for runtime parameter changes (e.g. changing sphere resolution in ImGui).
- Tessellation shaders (TCS/TES) reserved for Phase 3.4 when terrain LOD creates a real consumer.
- Mesh shaders reserved for Phase 5 alongside hardware RT.

**Module location:** `engine/meshgen/`
**Phase:** 2.7 v1e (after GPU upload in v1d)

---

## Consequences

- New top-level directories: `sandbox/`, `assets/source/`
- `sandbox/src/main.cpp` + `sandbox/src/sandbox_layer.hpp/.cpp` + `sandbox/src/asset_browser.hpp/.cpp`
- `sandbox/CMakeLists.txt` links: `crd-app`, `crd-resources`, `crd-renderer`, `crd-imgui`, `crd-rhi`, `crd-rhi-vulkan`, `crd-jobs`, `crd-meshgen`
- `engine/meshgen/` module follows standard layout (include/crd/meshgen/, src/, CMakeLists.txt)
- `assets/source/LICENSES.md` must be updated when any asset is added
- `build/<preset>/assets/cooked/` is created by the cook step; CI matrix with `CRD_BUILD_SANDBOX=OFF` never touches it
- `docs/systems/sandbox.md` created to document sandbox scope contract

---

## Alternatives considered

- **`crd-metamesh`** for the module name — rejected; "meta" implies reflection about meshes, not generation of them
- **`crd-shapes`** — rejected; conflicts with upcoming physics collision shapes in Phase 3.1
- **`crd-primitives`** — rejected; "primitives" is overloaded in graphics (draw primitive types)
- **Committed cooked artifacts** — rejected; binary churn in VCS, violates authoring-vs-runtime principle; cooker is idempotent from source
- **GPU geometry shaders** for resolution changes — rejected; deprecated, slow on AMD, absent on Metal; CPU regeneration is correct and simple

---

## References

- ADR-0013 — Asset pipeline (cooker is always a separate exe; cook output is never committed)
- ADR-0040 — Cooker CLI + CMake integration (`CRD_COOK_ROOT`/`CRD_COOK_OUT`)
- ADR-0043 — MeshResource vertex layout (48B interleaved — meshgen uses same format)
- `docs/systems/sandbox.md` — sandbox scope contract
- `docs/phases/phase-2.7-asset-import.md` — v1e crd-meshgen slice
