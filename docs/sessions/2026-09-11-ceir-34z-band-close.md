# 2026-09-11 — CEIR-34z band close: legacy deletion → one execution-program architecture

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

CEIR-34 executed the CEIR-0h deletion ledger to reach **one execution-program architecture** — no privileged or
duplicate native render/execution path; every legacy frame/executor/residual either deleted at a prior band or
promoted to an authored CEIR asset / a sanctioned host resolver. The 34-0 census graded every 0h row with
file:line evidence and found the spine deletions already executed; the only two open items (E4, R2) were **user
decisions**, and the user ruled to **pull both into the loop as real deletions** (not park). Both are now done.
CEIR-34 therefore closes as a **re-verification band** — the correct outcome when a census finds every spine
deletion already executed, confirmed by a fresh 17z-discipline gate re-run on the current working tree.

## Slices

| slice | what | record |
|-------|------|--------|
| 34-0 | census — every 0h row (F1–3 / E1–5 / R1–2 / M1 / §7 / §8) graded with file:line evidence | `docs/design/ceir-34-legacy-deletion-census.md` |
| E4 | the imperative `register_default_programs` hand-list → the `assets/scene_programs.manifest` + `register_from_manifest` loader (adding a program = a manifest edit, zero C++) | 4 configs + tidy (`context.md`) |
| R2 | the `draw_overlay`/`draw_overlay_range`/`record_overlay` device verb retired — the overlay rides generic `draw_storage`/`draw_storage_depth_load`; + the DX12 `first_vertex`→`SV_VertexID` identity-index-buffer fix | 4 configs + tidy — `docs/sessions/2026-09-11-ceir-34-r2-overlay-verb-retirement.md` |
| 34a | re-verification of the DONE rows (17z discipline) + E3b §88 / §7 verify | this doc |

## The census verdict (every 0h row)

- **Frame-path** F1 SATISFIED (`frame_runtime.cpp` is the thin cook→CEIR frontend, not a distinct driver) · F2/F3
  DONE (CEIR-15f/15z — the `CRD_FRAME_VIA_CEIR` bypass deleted; `validate_ceir_frame` is the live cook gate).
- **Executor** E1/E2/E5 DONE (CEIR-16d §128 — `record_scene_raster` deleted; `scene.raster` → `record_ceir_render`
  unconditionally; no private C++ render path) · E3a SATISFIED (17c CEIR resolve chain; the camera-fit host resolver
  is ledger-permitted) · E3b DONE (CEIR-15 §88 shadow corpus) · **E4 DONE** (this loop).
- **Residual** R1 DONE (CEIR-16z — `visbuffer.raster` dissolved, 14→13 built-ins) · **R2 DONE** (this loop).
- **Supersession** M1 DONE (CEIR-0f — struck in place). **§7** clean (no `executors = [` in any committed asset;
  CEIR-13c removed executor lists). **§8-1** resolved (`frame_runtime.cpp` retained as the thin frontend).

## 34a re-verification (17z discipline — deletion-gate re-run + HEAD baseline, current working tree)

Re-ran the DONE-row parity gates on **one-Windows + one-Linux, both backends** (the RAF/REN render-band discipline);
no latent CEIR-replay drop.

- **win-debug**: `ceir 1x` migration gates 79/79 (F1/F2/F3 `validate_ceir_frame`/`build_frame_plans`/CEIR-15
  lowering + E1/E2/E5 `record_ceir_render` §128) · `raf7` 7/7 (frame graph on **Vulkan AND D3D12** via the encoder)
  · `RAF-8` 6/6 (forward_csm → render-graph bridge).
- **linux-gcc-debug**: gcc 13.3.0 -Werror clean build of the DONE-row test targets · `ceir 1x` 265/265 · `raf7`
  6/6 (Vulkan/llvmpipe) · `RAF-8` 6/6.
- **E3b §88 corpus** committed + verified: `moment_convert_evsm.ckir` (EVSM), `moment_convert_msm.ckir` (MSM),
  `forward_csm*` ×8 + `forward_csm.crdt` (CSM), `rt_shadow.frame.toml` (RT); exercised by the shipped-asset plan
  gate + REN-3. **§7** verified clean.
- **E4** (scene-render, 4 configs at E4-close) + **R2** (gpu-context, freshly 4-config swept) carry their own full
  matrices.

## Outcome

Every CEIR-0h spine deletion is executed; the render/execution path is a single authored-CEIR + generic-command
architecture with no privileged native bypass. CEIR-34 CLOSED. The census's §3/§4 park recommendations for E4/R2
are superseded (annotated in the census); no deferral-ledger entry is owed (both done, not deferred).

**Next:** whole-engine PQP (CEIR-35 — the full §PR-7 PQP-0..4 matrix per the user ruling; spine slices 35b/35a-Q1/
35a-Q4/35c/35d-Q6 already done), then CEIR-33 (the D7E universal program editor, un-parked).

## Proposed commit (user commits — NO AI co-author trailer)

```
docs(ceir-34): close the legacy-deletion band — one execution-program architecture

Re-verification band: the 34-0 census found every CEIR-0h spine deletion already
executed; E4 (program hand-list -> manifest+loader) and R2 (overlay device verb ->
generic command model + DX12 first_vertex identity-IB fix) were driven to completion
this loop. 34a re-ran the DONE-row parity gates green on win-debug + linux-gcc-debug
(both backends); E3b shadow corpus + §7 manifest verified. Census §3/§4 park
recommendation superseded.
```
```
(pairs with the E4 and R2 code commits proposed in their session logs / context.md)
```
