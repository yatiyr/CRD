# crd-sandbox — System Overview

**Module:** `sandbox/` (repo root, not under `engine/`)
**Status:** ⏳ planned — Phase 2.7
**ADR:** ADR-0045

---

## Purpose

`crd-sandbox` is the **canonical long-lived engine consumer** — a real application built with Cerid
that exercises the full stack each phase: asset loading, rendering, ImGui tooling, and eventually
scene, physics, and animation. It is not a test; it does not exit after one assertion. It is what
"a product built with Cerid" looks like.

Every phase adds one capability. The sandbox is the first place a developer goes to see the engine
running with real content.

---

## Architecture

```
sandbox/
  src/
    main.cpp                    ← Application entry, SandboxDesc configuration
    sandbox_layer.hpp/.cpp      ← ILayer implementation (render + update)
    asset_browser.hpp/.cpp      ← ImGui panel: browse loaded assets, switch displayed mesh/texture
  CMakeLists.txt                ← CRD_BUILD_SANDBOX gate; cook_sandbox_assets dependency
```

The sandbox uses `crd-app::Application` + `LayerStack`. `SandboxLayer` is the only layer.
`Application::run()` owns the frame loop and jobs lifecycle.

**Dependencies:** `crd-app`, `crd-resources`, `crd-renderer`, `crd-imgui`, `crd-rhi`,
`crd-rhi-vulkan`, `crd-jobs`, `crd-meshgen`, `crd-config`

---

## Headless mode

`crd-sandbox --headless` (or `CRD_SANDBOX_HEADLESS=1`) skips GPU calls, validates that all cooked
assets reach `ResourceHandle::Ready`, and exits 0/1. Used in CI for CPU-side validation.

---

## Scope contract (what the sandbox is and is not)

**Is:**
- A demo of the current phase's features
- The primary tool for manually verifying GPU smokes before a phase ships
- A growing reference application showing Cerid integration patterns

**Is not:**
- A scratch pad for experiments (features must belong to a shipped phase)
- A second test suite (unit tests live in `tests/`, smokes in `runtime/examples/`)
- An editor (that is Phase 7)

---

## Per-phase growth plan

| Phase | Sandbox capability added |
|-------|--------------------------|
| **2.7** | Asset browser: browse cooked meshes/textures/materials, click to switch rendered asset; crd-meshgen shapes alongside glTF assets; material parameter readout (display values) |
| **2.8** | Shader switcher panel (PBR / toon / unlit — pass-keyed variants); editable material parameters (roughness/metallic sliders); wireframe toggle; PSO state display per material |
| **3.0** | Scene hierarchy panel; entity list; transform inspector; load a `.scene.toml` |
| **3.1** | Physics debug overlay (rigid body bounds, contact points, sleep state) |
| **3.2** | Animation panel (play/pause/scrub; bone hierarchy viewer) |
| **3.4** | Lighting panel (add/remove point/spot/directional lights); CSM debug view; IBL environment switcher; post-FX toggle (bloom, SSAO, tone map) |
| **5.0** | RT toggle (RT reflections, RTAO, RTGI on/off); denoiser selector; hardware caps display |

---

## Demo assets (`assets/source/`)

Source assets are in `assets/source/` at the repo root. Cook output is in
`${CMAKE_BINARY_DIR}/assets/cooked/` (gitignored). See `assets/source/LICENSES.md` for attribution.

| Phase | Asset | License |
|-------|-------|---------|
| 2.7 | `BoxTextured.glb`, `Duck.glb`, `Suzanne.glb` | CC0 / Apache 2.0 |
| 2.7 | `checker_512.png`, `bricks_512.png` | CC0 |
| 2.8 | `DamagedHelmet.glb` | CC BY 4.0 |

---

## References

- ADR-0045 — Sandbox, asset layout, cook workflow, crd-meshgen
- `docs/phases/phase-2.7-asset-import.md` — first sandbox implementation
- `docs/systems/meshgen.md` — crd-meshgen overview (created Phase 2.7)
