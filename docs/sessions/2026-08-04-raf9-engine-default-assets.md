# Session — RAF-9: engine default assets load by canonical `engine://` id (2026-08-04)

**Branch:** `raf-9-engine-default-assets` (stacked on the RAF-8 tail + A13-VRS-fix WIP).
**Plan executed:** `docs/research/RAF9plan.md` (Ultraplan-refined, user-approved), verbatim.

## What shipped

RAF-1 shipped the asset *identity* model (`engine://` schemes, `AssetRef`, `AssetRegistry`) but it was never wired to
the live loader — `SceneRenderer` selected everything by relative filename and resolved a frame's programs through a
hard-coded `str_is(id, "crd://scene/…")` chain. RAF-9 wires identity into content resolution and makes the default
renderer load by canonical id through the same public registry an app uses.

Two thin layers over what already existed:

1. **render-asset-core stays a pure leaf** — added `asset_extension(folder)` + `on_disk_relative(AssetRef, out)` to
   `identity.{hpp,cpp}` (I/O-free; the folder→ext table is keyed on the folder STRING because `infer_type` has no
   `post` and maps `light`≠`lighting`). New `AssetNotFound` diagnostic.
2. **scene-render gets the I/O layer** — new header-only `AssetResolver` (mount table + `platform::fs` read; `engine://`
   mounts `asset_root`, so a resolved file is the exact pre-RAF-9 file) and a public `ProgramRegistry` (`AssetId →
   provider`, the `FramePassFn` fn-ptr + `void*` idiom, no `std::function`). `asset_text` → one `read_relative` path;
   `resolve_frame_asset` → `read_ref` (dropped the `crd://frame/` prefix-strip); `SceneHost::program()`/`kernel()` →
   parse-to-`AssetId` registry lookup (the whole `str_is` program chain is gone; the bare-name graph-resource-name
   resolvers stay). `ensure_post_program` gained a `bool is_agx` discriminator. New public `set_frame_graph(engine:// id)`
   + `register_raster_program`/`register_kernel_program` (the RAF-10 seam). Sandbox selects `engine://frame/…`; the 16
   `assets/frame/*.frame.toml` rewritten `crd://`→`engine://` (pixel-neutral — `crd://` folds to `engine://` so the
   `AssetId` is identical).

## Verification (Gate 9 met)

- **Device-free** `RAF-9: loads by canonical id` (tests/scene-render): id-load · crd alias · missing refused · non-frame
  rejected · relative wrapper · public program-registry API. Plus `on_disk_relative` unit test (tests/render-asset-core:
  7 folders · alias==engine id · app≠engine no-shadow · unknown folder refused).
- **Pixel-exact BOTH backends** `RAF-9 GATE` (Vulkan + DX12): the `engine://` id-selected default renders BIT-IDENTICAL
  to the relative name (`diffs == 0`, `covered > 500`).
- **Sandbox smoke byte-identical both backends** (shadows ON, full scene, `--gpu-cull`/`--gpu-skin` kernels resolve).
- render-asset-core / render-pass / render-graph device-free suites green. Changed library + test code LLVM-20
  tidy-clean.

## Pre-existing, NOT RAF-9 (flagged)

`test_scene_render_gpu.cpp` **REN-39-C1** (pull/indexed bit-identity) + **REN-40-D** (EVSM/MSM moment shadows) — 4 cases
/ ~14 assertions — fail. **Proven pre-existing:** stashing the RAF-9 scene-render changes and re-running reproduces the
identical failures, so they come from the prior RAF-8 WIP baseline, not RAF-9. A separate investigation. Also
pre-existing: geometry-mesh-processing tidy debt blocks win-tidy of scene-render deps; main.cpp + test_scene_render_gpu.cpp
carry tidy-gate-excluded multi-decl/complexity debt.

## Next

Investigate the two pre-existing RAF-8-baseline GPU-gate failures → RAF-10 (app-custom renderer proof, leverages the
new `register_*_program` seam).
