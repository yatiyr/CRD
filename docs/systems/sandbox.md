# crd-sandbox — System Overview

**Module:** `sandbox/` (repo root, not under `engine/`)
**Status:** ✅ updated 2026-05-06 — bootstrap (Phase 2.7 v1d) + 3D rendering wired (Phase 2.8 v1e) + unified Asset Browser (Phase 2.8 v1g) shipped.
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
    main.cpp                    ← Application entry, --headless flag, explicit render loop
    sandbox_layer.hpp/.cpp      ← ILayer implementation: OrbitCamera + ImGui panel
  CMakeLists.txt                ← CRD_BUILD_SANDBOX gate; imgui_layer.toml copy
```

The sandbox uses `crd-app::Application` + `LayerStack`. `SandboxLayer` is a regular layer; `ImGuiLayer` is an overlay. The render loop is explicit (not `app.run()`) to allow sandbox control over acquire/submit/present. `Application::tick()` is called once per frame.

**`OrbitCamera`** struct (owned by `SandboxLayer`):
- Target state: `yaw, pitch, distance` (driven by input)
- Smoothed state: `s_yaw, s_pitch, s_dist, s_target` (rendered from)
- Controls: left-drag orbit, Ctrl+MMB pan, scroll zoom; pitch clamped to ±89°
- Smoothing: exponential lerp `s = s + (target - s) * (1 - exp(-kOrbitSpeed * dt))` where `kOrbitSpeed = 8.0`

**`--headless` flag**: runs one frame then calls `app.close()`; exits 0. Used in CI for sandbox build verification (no GPU required — just proves the binary runs).

**ImGui panel** (fixed-position, top-left): shows viewport size, smoothed camera yaw/pitch/distance/target, help text (`LMB drag=orbit  Ctrl+MMB=pan  Scroll=zoom`).

**Current dependencies (Phase 2.8 v1g):** `crd-app`, `crd-config`, `crd-imgui`, `crd-log`, `crd-math`, `crd-memory`, `crd-meshgen`, `crd-renderer`, `crd-resources`, `crd-rhi`, `crd-rhi-vulkan`, `crd-shader`.

## Asset Browser (Phase 2.8 v1g)

The ImGui side panel is the **Asset Browser** — a single window with two `CollapsingHeader` sections:

- **Procedural Shapes (8)** — Plane, Box, Sphere, Icosphere, Cylinder, Cone, Capsule, Torus. Each row shows live vertex/index/triangle counts; the selected row exposes per-shape sliders (radius, segments, etc.) that re-cook + re-upload on slider release.
- **Imported Assets (3)** — `BoxTextured`, `Duck`, `BoomBox` (when the demo pack is mounted).

Internally, both sections feed a single `Array<AssetEntry>` with a `kind: Procedural | Imported` tag. Click a row → on the next `on_update`, the layer's `upload_selected_asset()` either runs the meshgen function or `ResourceManager::load_sync<MeshResource>(uuid)`, then `GpuUploader::upload_mesh()`.

The imported-assets branch is best-effort. If the cooked pack is missing, or a `.meta` sidecar is missing, or a UUID isn't in the manifest (typical after a re-cook with content changes), the panel logs a Warn and hides only the affected entry — the rest of the panel still works.

## Cook target

`sandbox/CMakeLists.txt` declares `cook-demo-assets`, an `add_custom_command(OUTPUT demo_assets.crdr)` that runs `asset_cooker cook --root assets/source --out <build_dir>/sandbox/assets/cooked/demo_assets.crdr` — i.e. the pack lands **next to the executable** (`build/<preset>/sandbox/`), so `build/<preset>/sandbox/` is a self-contained shippable folder. The DEPENDS list is the explicit set of source files plus the `asset_cooker` target, so the pack rebuilds whenever any source asset changes. `crd-sandbox` declares `add_dependencies(crd-sandbox cook-demo-assets)` and resolves the pack at runtime as `crd::platform::fs::executable_dir() / CRD_DEMO_ASSETS_REL_PACK` (where `CRD_DEMO_ASSETS_REL_PACK = "assets/cooked/demo_assets.crdr"` — a compile def carrying only the relative path; no compile-time absolute paths bake into the binary).

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

Source assets are in `assets/source/` at the repo root. Cook output is in `assets/cooked/` (gitignored). See `assets/source/LICENSES.md` for full attribution and license URLs.

| Phase | Asset | License | Notes |
|-------|-------|---------|-------|
| 2.8 v1f | `BoxTextured.glb`  | CC-BY 4.0 (Cesium / Khronos) | 5 KB |
| 2.8 v1f | `Duck.glb`         | SCEA Shared Source 1.0 (Sony / Khronos) | 118 KB |
| 2.8 v1f | `BoomBox.glb`      | CC0 1.0 (UX3D / Khronos) | 10 MB; PBR test asset (replaced the originally planned Suzanne — not in Khronos sample-assets) |
| 2.8 v1f | `checker_512.png`  | CC0 1.0 (procedural) | Generated by `generate_textures.ps1` |
| 2.8 v1f | `bricks_512.png`   | CC0 1.0 (procedural) | Generated by `generate_textures.ps1` |

---

## References

- ADR-0045 — Sandbox, asset layout, cook workflow, crd-meshgen
- `docs/phases/phase-2.7-asset-import.md` — first sandbox implementation
- `docs/systems/meshgen.md` — crd-meshgen overview (created Phase 2.7)
