# 2026-07-28 — 38-G1 live close: every default is an asset · zero validation errors · 53 → 119 fps

> Continues `2026-07-27-ren38-authored-programs.md` (the 38-G1 live-sandbox integration). User direction, verbatim
> intent: "fix all of them, I only want default assets to run gpu … and then I want blazing speed, we are racing
> with frontier engines." Three demands: (1) EVERY embedded default resolves disk-first, (2) all remaining
> validation errors gone, (3) full performance. All three landed and are gated.

## 1. THE DEFAULTS ARE ASSETS (disk-first, everywhere)

The builtin pack (`builtin_asset_text`) was always the FALLBACK; the defect was that several cook sites read the
`constexpr` texts DIRECTLY, so a user's `assets/**` override was silently ignored (the files existed, were
drift-gated, and did nothing). Every consumer now routes through `asset_text` (disk-first → builtin):

- **Scene materials** — `cook_fs` resolves `material/scene[_textured].crdm` for Forward passes; the resolved
  text rides `SceneShaderConfig::material_text` → `SceneSurfaceCtx::disk_text` (F15, exactly like flat).
- **Lighting** — `lighting/scene_forward.crdl` injected into `scene_lighting_disk_text()` before first cook
  (the parsed-once identity rule holds: whichever text won, it wins for the process).
- **Vertex programs** — `vertex/scene.crdv`, `vertex/scene_skinned.crdv` resolve by name; the REBASED twin
  derives from the SAME resolved text (cannot drift). The per-cascade shadow variants resolve
  `vertex/shadow.crdv` ONCE and stamp `cascade` / `instance_capacity_word` on the PARSED desc — the variant is
  the renderer's pass semantics, the vocabulary is the asset's. `cook_stage(pre, body)` (embedded-text variant)
  DELETED — dead wrapper.
- **Sandbox order** — `set_asset_root` now installs BEFORE `init_programs` (it used to install after, so every
  cooked program came from the embedded pack and only the frame graphs honoured the root).

**The override PROOF has two arms** (both exercised): a valid R/B-swapped `scene.crdm` cooks + renders; a
deliberately BROKEN copy fails BY NAME ("Scene renderer unavailable"), no crash. The broken arm found a real
engine bug: `build_fs_for_pass` never checked the material template's documented negative-node failure shape —
`unpack_surface(g, -1, …)` indexed the node table with -1 → access violation. Guard added at the seam.

## 2. ZERO VALIDATION ERRORS (app, all modes)

- **VUID-09588** (companion depth stuck UNDEFINED): the graph-owned colour-transient write barrier now also
  transitions the 38-G1 COMPANION depth (`depth_buffer = true`) — it is graph-owned, so the graph transitions it.
- **10-image leak at vkDestroyDevice**: `retire_transients_to` (the fence-guarded retire path) skipped the
  companion depth image/view/memory — freed inline nowhere, leaked per graph rebuild. It now rides the same
  retire list. (`free_transients` had the same gap — both fixed.)
- **VUID-00922** (buffer in use by descriptor set at teardown): sandbox teardown order — the SURFACE dies first
  (its dtor is the vkDeviceWaitIdle), then the SCENE RENDERER (its graph's descriptor pools reference the draw
  system's buffers), then draw/imgui.
- **VUID-07752** (viewType, from the previous session's list): no longer reproduces after the binding-1
  null-view fix + companion-depth work; watched across every run this session.
- **Debug-utils naming**: every VkImage the backend creates gets a SITE+SIZE label (`name_image` — "fg-persist
  2048x2048x4"), so validation/leak reports are attributions, not bare handles. This is what identified the
  leak class in one run.

## 3. PERFORMANCE — 53 fps (session start, shadows on) → **119 fps**, loop 8.4 ms

Measured honestly: `--no-validation --present immediate` (the shipped-config doctrine), REN-8 phase means.

- **THE fix: batched uploads.** `upload_storage` was contractually synchronous — fresh staging buffer +
  submit + **queue idle PER CALL**. The live scene issued ~50/frame: **8.3 ms of a 16 ms frame was upload
  waits** (the new `SyncStats` extract/upload/palette split named it in one run). New contract at vtable END:
  `begin/end_upload_batch` — a persistent mapped staging ring (2 fence-guarded slots), copies recorded into ONE
  transfer cmd, WAR barrier at open, transfer→consumers barrier at close, ONE submit, **no host wait**
  (same-queue submission order sequences it before the frame's draws). Every synchronous verb flushes an open
  batch via `begin_cmd` — upload-then-read code observes exactly what it always did. `sync()` opens the batch;
  the graph's `execute()` flushes it. sync: **8.66 → 0.38 ms**.
  - ⛔⛔ **llvmpipe found the lifetime hazard on day one**: a storage buffer destroyed while a batch (open or
    in flight) held recorded copies into it → SEGFAULT executing the copy (a discrete GPU corrupts silently).
    `~VulkanStorageBuffer` now drains the batch machinery via a context drain hook before its handles die.
- **Present ring contract made explicit**: `IPresentSurface::wait_idle()` (vtable END). `present()` keeps a
  2-frame ring in flight; resources a present referenced (blit source, overlay textures) must outlive until the
  surface drains. RET-5 encoded the old synchronous contract and caught the gap (target destroyed scope-inner
  while the surface lived); the gate now calls `wait_idle()` and documents why.
- **Per-pass GPU board**: `SceneRenderer::debug_frame_graph()` + sandbox log — "gpu 8.0 ms" is now an
  attribution: csm 0.03/0.16/0.80/**3.27** + forward **3.70** + post 0.008 + overlay 0.013.
- `--no-validation` sandbox flag (a perf number with validation on measures a config nothing ships).

**The named NEXT lever (measured, not guessed):** halving the shadow map barely moved cascade 3 (3.27 → 3.08 ms)
→ the frame is **VERTEX-bound**: the non-indexed vertex pull re-shades every triangle corner (zero post-transform
reuse) — ~38 M vertex invocations/frame on an RTX 4070 Ti SUPER. The lever is INDEXED pull:
`vkCmdBindIndexBuffer` on the SAME storage buffer at `indices_off` (add INDEX_BUFFER usage — zero duplication),
`vkCmdDrawIndexedIndirect`, instance = `gl_InstanceIndex` through the visible list, vertex pull by
`gl_VertexIndex` (= the index value). Expected 3–6× vertex-work reduction across every pass, both backends.
That is a REN-39-class slice (cook contract + both backends + oracle mirror), not a patch — now SLICED as row 142 in `docs/detours/D-007-gpu-program-system.md`: 39-A1/A2 (device verb) → 39-B1/B2 (cook contract) → 39-C1/C2 (renderer switch + measured close).

## Sweeps

win-debug: scene 30/30 · scene/technique/material 78/78 · kir 34/34 · vulkan 191/191 · dx12 161/161.
WSL llvmpipe (linux-gcc-release): 280/280 (the segfault fix verified here first).
clang-tidy (LLVM 20.1.8): all touched files clean — including a pre-existing widening-cast in
`ckir_technique.hpp` and duplicate includes in the vulkan test TU that the per-file gate surfaced.
Sandbox: smoke PASS 119 fps · validation arm: **0 errors**.

One intermittent: `REN-38 RT GATE (DX12) … ANY-HIT can IGNORE every hit` failed once in a sweep ("1 1 -1 -1"),
then passed 3× standalone + 2× via ctest + full family rerun 161/161. Nothing in the DX12 RT path changed this
session. Watch: if it recurs, it is a real race, not noise.

## Also

- `scripts/reconfigure-preset.bat` — the honest repair for a cache whose `CMAKE_MAKE_PROGRAM` went NOTFOUND
  (reconfigure under vcvars; never hand-edit the cache — the stale-toolset scar).
