# Session — RAF-11: dependency-aware hot reload, all 5 kinds (2026-08-05)

**Branch:** `main`, stacked on the RAF-10 close (which sits on the RAF-8/9 tail).
**Gate 11:** reload material param/default · material graph · technique · shader body · frame graph;
dependency-chain invalidation; interface-change rejection; last-good preservation; atomic generation install;
deferred GPU destruction after in-flight frames; NO stale mixed-generation variant.

## What shipped

The §13 hot-reload pipeline — **detect → reparse → validate → re-cook → find affected dependents → rebuild in
dependency order → validate the set → install atomically at a frame boundary → bump generations → defer GPU
destruction until no in-flight frame references the old objects → report; on failure keep the last-good generation,
never a mixed-generation set.** It is ORCHESTRATION over a per-kind plug-in (fn-ptr + `void*`, NO std::function, NO
virtual across the seam), and the "generation" is always RAF-3's generation-tagged `RuntimeSlot` — one staleness
source of truth, never a second counter.

### The reload core (Inc1–4, committed `53d32f6`, device-free)

- **`DependencyGraph::affected_by(changed, out, diags)`** (`render-asset-core`) — reverse-BFS collects the transitive
  dependents of the changed asset, then filters them through `topo_order` so the rebuild set comes back deterministic
  and DEPS-FIRST. A cyclic graph has no safe order → returns false + a named diagnostic.
- **`RenderAssetReloader`** (`scene-render/src/reload.{hpp,cpp}`) — `register_asset(id, vtbl, user, content, deps…)`
  and `reload(changed)`:
  1. stage (re-cook) the changed asset into a staging slot; a byte-identical re-cook is a clean NO-OP (no swap, no
     generation bump);
  2. otherwise build the `affected_by` rebuild set and stage each dependent — skip a byte-identical (unaffected)
     dependent, and if a dependent can no longer cook against the changed asset's new INTERFACE (the interface-change
     rejection) FAIL the whole set;
  3. COMMIT all-or-none — the changed asset first, then dependents in dependency order, every generation bumping
     together — or roll back every staged object (last-good preserved; no caller ever observes a mixed generation).
- **`DeferredReleaseQueue`** — a frame-indexed, type-erased (`void(*)(void*, void*)`) release queue: a retired GPU
  object is freed only once `frames_in_flight` (= 2, matching both presenters) `begin_frame()` cycles have elapsed —
  the same fence depth the frame graph gives its own transients. `drain_all()` frees the rest on device-idle shutdown.
- New `DiagCode`s `AssetCookFailed` / `InterfaceIncompatible`.

**Device-free gate (`crd-scene-render-tests [raf11]`, `tests/scene-render/test_raf11_reload.cpp`):**
no-op / swap / last-good on a REAL frame cook (`parse_frame_toml`, generation carried by `RuntimeSlot`, a pre-reload
handle goes STALE after the swap) · a module→consumer dependency chain (content-only change leaves the consumer
untouched; a compatible interface change rebuilds BOTH atomically, module-before-consumer; a breaking interface
change rejects the whole set so NEITHER generation moves) · deferred release frees only after the in-flight window
and a shutdown drain frees the rest.

### The live wiring (Inc5, on `SceneRenderer`)

Public `reload(canonical_id)` (parse → `AssetRef` → `reloader.reload`; logs every diagnostic on rejection, never a
silent last-good) + `asset_generation(canonical_id)`.

- **Frame-graph kind** — `set_frame_graph` registers the installed graph as reloadable (`cook_frame_text` =
  parse → flatten → validate). Gated device-free: `SceneRenderer::reload` re-reads a frame's `.frame.toml` FROM DISK,
  re-cooks, installs a new generation, or keeps last-good on a broken edit (the render loop reads the installed desc
  live — no device needed for this kind).
- **Program-input kinds — shader body (`vertex/scene.crdv`), technique (`lighting/scene_forward.crdl`), material
  param + graph (`material/flat.crdm`).** `init_programs` is now RE-RUNNABLE: `prepare_reinit()` retires EVERY live
  program (the ~40 VS/FS/shadow/moment/tess/mesh/visbuffer/impostor/hzb/velocity/post/taa members + the `fs_programs`
  and `adv_stages` caches) to the `DeferredReleaseQueue` and clears the technique library, then the existing rebuild
  re-reads and re-cooks everything from the (edited) authored sources. A `SourceReload` adapter registers the three
  sources ONCE (a `sources_registered` guard — `init_programs` re-runs on every reload, and re-registering would grow
  the stable-storage array and dangle the reloader's `&source_reloads[i]` user pointer; `reserve(8)` for the same
  reason). Each `stage` re-reads the file through the engine mount and CPU-VALIDATES it with the KIND's own cooker
  (`cook_vs` / `parse_lighting_toml` / `parse_material_toml`) — a malformed edit is rejected BEFORE any live program is
  touched (last-good); `commit` re-cooks the whole program set (`owner->init_programs`) and bumps the shared
  program-input generation. `~Impl` drains the release queue at shutdown (the device is idle and still alive — the
  renderer holds `ctx`/`raster` as non-owning back-pointers that outlive it — and a retired program was `.release()`'d
  out of its member, so no double-free with the members' own unique_ptr destructors).

**Device gate (`crd-scene-render-tests [raf11][gpu]`, Vulkan AND DX12 twins over one shared body,
`tests/scene-render/test_scene_render_gpu.cpp`):** a fresh writable MIRROR of the shipped asset tree is edited between
reloads exactly as a file watcher would deliver — material PARAM edit → gen 1 · material GRAPH edit (node op
`multiply`→`add`) → gen 2 · TECHNIQUE edit (PCF taps 4 → 8) → gen 3 · SHADER-BODY edit → gen 4 · each BROKEN edit →
REJECTED with the generation held (last-good) · an unregistered id reported, never silent. 33 assertions (Vulkan) /
32 (DX12), both genuinely executed on a real device.

## Decisions / honest notes

- **The three program-input sources SHARE one generation.** `SceneRenderer` builds every program in a single
  monolithic `init_programs`, so ANY source edit re-cooks the whole set. The reload ORCHESTRATION is complete and
  gated — the generic core proves fine-grained dependency-ordered rebuild + interface rejection on the Inc3
  module/consumer chain — but the per-program dependency graph that would rebuild ONLY a material's own programs is the
  program-cache DECOMPOSITION RAF-12 performs when it unifies the ~40 caches into a per-asset registry. This is the
  current program-system granularity, NOT a gap in the reload machinery. Recorded in the D-007 RAF-11 row + context.md.
- **Tidy debt on the touched file.** `tests/scene-render/test_scene_render_gpu.cpp` carried 6 pre-existing
  `readability-isolate-declaration` violations in its rasteriser oracle (`f32 ax, ad, au…`). Fixed them mechanically
  (split declarations, zero-init) — never accumulate tidy debt on a file you edit. LLVM-20 tidy-clean on every changed
  file.

## Verification

- `crd-scene-render-tests [raf11]` — **141 assertions in 6 cases** (4 device-free + Vulkan + DX12), all green.
- LLVM-20 clang-tidy clean: `scene_renderer.cpp`, `scene_renderer.hpp`, `reload.hpp`, `ckir_technique.hpp`,
  `test_raf11_reload.cpp`, `test_scene_render_gpu.cpp`.
- Because RAF-11's own gates already pass on both GPU backends + tidy, and the full 5-config sweep was (a) hitting a
  pre-existing unrelated abort and (b) can't go green regardless, verification was done as TARGETED per-config runs of
  RAF-11's scope: **win-debug** `[raf11]` 141 assertions / 6 cases · **win-asan** 141 assertions, exit 0, NO leak/UAF
  (the leak check on the new `DeferredReleaseQueue` + `~Impl` drain, on a real device, both backends) · **win-release
  (LTO/O2)** 141 assertions, no LTCG miscompile · **sandbox smoke PASS both backends** (Vulkan 126 / DX12 94 frames,
  full 11-pass frame — RAF-11 is out-of-band, render path unchanged). This matches the user's directive to test RAF/REN
  slices on one Windows + one Linux config rather than the full multi-config sweep.
- **Pre-existing REN-40 lod-atlas failures FIXED this session (a follow-up the user asked for).** The sweep surfaced
  `crd-lod-tests` REN-40-C5 / C5.6 failing + a hard TLSF OOM abort: REN-41 made the impostor atlas a MIP PYRAMID
  (buffer = `impostor_atlas_texels(grid, tile)` across all levels), but three test sites still hard-coded the pre-mip
  level-0 formula `(grid*tile)^2 * 4` — the exact "second copy of the arithmetic" the header (`impostor_atlas.hpp`)
  warns is silent atlas/reader drift — and one sized its TLSF arena to the smaller number, OOM-ing on the larger tiles.
  Fixed by using the ONE canonical `impostor_atlas_texels` formula and sizing the arena to the pyramid + scratch. The
  BAKER was correct (the mip pyramid is the intended REN-41 design); only the stale tests needed updating. Full
  `crd-lod-tests` now 24673 assertions / 18 cases green; both changed files LLVM-20 tidy-clean (also fixed 3
  pre-existing isolate-declaration violations in the touched oct-decode test). This is a REN-40 test fix, logically a
  separate commit from RAF-11.

## Files

- `engine/render-asset-core/{include/crd/renderasset/dependency.hpp,src/dependency.cpp}` — `affected_by` (Inc1).
- `engine/render-asset-core/{include/crd/renderasset/diagnostic.hpp,src/diagnostic.cpp}` — new DiagCodes.
- `engine/scene-render/src/reload.{hpp,cpp}` — `RenderAssetReloader` · `DeferredReleaseQueue` · vtbl (Inc2–4).
- `engine/scene-render/src/scene_renderer.cpp` — the live integration (frame reloadable · `SourceReload` adapter ·
  `prepare_reinit`/`retire_all_programs`/`rebuild_programs` · `~Impl` drain · public `reload`/`asset_generation`).
- `engine/scene-render/include/crd/scenerender/scene_renderer.hpp` — public `reload` + `asset_generation`.
- `engine/kir/include/crd/kir/ckir_technique.hpp` — `TechniqueLibrary::clear()`.
- `tests/scene-render/test_raf11_reload.cpp` — device-free Inc2–5 (frame) gate.
- `tests/scene-render/test_scene_render_gpu.cpp` — the `[raf11][gpu]` Vulkan + DX12 program-input gate.

## Next

RAF-12 (DELETE legacy: the specialized draw-verb surface + the `FramePassKind` switch + embedded default frames +
hard-coded bindings; unify the two frame graphs; decompose the ~40 program caches into a per-asset registry — which
also gives the program-input reload its fine-grained per-asset dependency granularity) · RAF-13 (docs + §22 DoD close).
