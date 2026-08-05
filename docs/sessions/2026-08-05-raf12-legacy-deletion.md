# Session — RAF-12: delete the legacy rendering architecture (2026-08-05)

**Branch:** `main`. Detour D-007, RAF band, Phase 12 (§7 deletion list in `docs/design/raf-0-rendering-foundation-design.md`).
**Goal:** ONE rendering architecture — delete the legacy that RAF-8/9 already moved off, proven empty via repo-wide grep.
Every increment is sandbox-safe, gated **byte-identical both backends + one Linux gcc build + LLVM-20 tidy**, and banked
for the user's commit (per the 2026-08-05 test-scope directive — no full multi-config sweep per slice).

## Increments completed this session

### 12.1 — retire the deprecated `set_frame_graph_asset` wrapper
A frame is selected ONLY by canonical `engine://` id. Migrated the sandbox `--frame`/`install_frame`, the gizmo default,
and converted the two RAF-9 `by_id==relative` parity gates to by-id-only renders (the migration they A/B'd against is
complete). `[raf9]` green both backends; smoke byte-identical.

### Linux/gcc build RESTORED (incidental, but it blocked the Linux leg of every gate)
The RAF/REN band had accumulated gcc-only breaks never Linux-verified (CI runs tidy relaxed; the sweeps were
Windows-only or killed early):
- dead statics `bits_to_f32` / `clampf` (`-Werror=unused-function`);
- **two latent `-Werror=switch` gaps MSVC hid — real bugs:** `command_model::validate_packet` never validated
  `DrawMulti`/`DrawMultiIndexed` (RAF-8 kinds), `frame_emit::from_pass_kind` mis-emitted `FramePassKind::Custom`
  (RAF-10) as `raster.geometry`;
- a `-Werror=shadow` (`wx`/`wy` in the impostor sampler);
- my own RAF-11 DX12 gate missing its `#ifdef _WIN32` guard;
- **the sandbox never compiled on Linux** — unguarded DX12 includes + unconditional `crd-gpu-context-dx12` link
  (REN-39-D debt). Guarded the DX12 code paths + made the CMake linkage `if(WIN32)`.
Result: every RAF/REN engine lib + `crd-scene-render-tests` + `crd-sandbox` + `crd-gizmo-probe` build under gcc;
Windows unaffected (both backends smoke, incl. DX12). Memory: this is why the RAF/REN Linux build must be part of the
one-Linux-one-Windows cadence.

### 12.2 core — the entire inline verb path DELETED from `record_pass`
`frame_runtime.cpp`'s `record_pass` collapsed **~700 → ~65 lines** (16 insertions, 621 deletions). Every
`FramePassKind` case that carried an inline `draw_storage_*`/`dispatch_*`/`trace_*` fallback now records ONLY through
its render-graph `record_*_via_executor` adapter. **Proven safe:** the executor registry is wired into EVERY `PassRec`
(`rec.records = &m_impl->records`) since the RAF-8 flip, so each `via_executor` always records — the inline fallbacks
were dead. The only real coverage gap (a multi-colour G-buffer, `n_writes>1`) is unused by every shipped frame and stays
executor-gated. Done with **deterministic, assertion-guarded Python scripts** (a 288-line + an 11-case replacement) —
no hand-transcription of core-renderer code. **Zero verb call sites remain in frame-cook** — the verbs are now reached
only by the `TranslatingCommandEncoder` (this unblocks 12.4). Gated: smoke byte-identical Vk+DX12 (+`--gpu-cull`);
`crd-scene-render-tests` **972 assertions / 52 cases** (REN-38/39/40/41 + RAF-9/10/11, every advanced kind: tess · mesh
· visbuffer · composite · raytrace · RT-pipeline); `crd-frame-cook-tests` 459; tidy-clean; Linux gcc clean.

### 12.4 started — first real verb removals (59 → 57)
The 2 verbs the fallback deletion left with ZERO callers — `draw_storage_indexed_mrt` ·
`draw_storage_multi_indexed_mrt_indirect` (the CPU/GPU-driven MRT velocity verbs) — deleted from the interface + both
backend bodies (deterministic assertion-guarded matcher; the interface decls are virtual-with-default-empty-body, same
4-indent brace shape as the backend bodies). Validated: smoke both backends + 632 REN-38/40 assertions; Linux gcc clean.

## In progress — the ~49 encoder-used verbs (the mission's core 12.4 move)

Deleting the remaining verbs requires the mission's stated refactor: **inline the ~18k lines of verb bodies into
per-backend command encoders** (so `IRasterContext` loses the verbs but the lowering stays), + migrate ~150 test call
sites. This is a large, delicate refactor — approached via a **read-only analysis workflow** (`raf12-verb-dossier`):
parallel per-verb dossiers (exact decl/body ranges, callers, classification) + adversarial verification that each
test-only verb's capability is covered by a non-verb (encoder/command-model) test, synthesized into an apply-ready
safe-deletion list + the per-backend-encoder relocation design. The main thread applies deletions with the
assertion-guarded scripts + per-family gating (agents never edit the tree — "do not break anything").

Classification so far: 6 verbs (`draw_depth`/`draw_gbuffer`/`draw_bindless_depth`/`draw_mesh_bindless_depth`/
`draw_mesh_vrs`/`draw_wboit`) look TEST-ONLY (encoder + live engine never call them); the rest are encoder-used.

## Remaining in RAF-12 / 13

- delete the encoder-used verbs (per-backend encoder relocation, family-by-family) + migrate test sites;
- retire `FramePassKind` (214 refs; cook emits `ExecutorTypeId`) + the full render-graph driver unification (12.3);
- hard-coded bindings, `crd://` shadow remnants, grep-proof §7 empty (12.5);
- docs + the mission §22 35-condition DoD (13).

## Discipline notes

- Large core-renderer block deletions are done with **deterministic, assertion-guarded scripts** (dry-run → verify →
  apply), never hand-transcription — the safe way to cut hundreds of lines from a live file.
- Every increment gated byte-identical smoke both backends + the affected GPU suite + one Linux gcc build + tidy.
